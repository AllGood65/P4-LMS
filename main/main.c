#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <math.h>
#include <time.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "esp_err.h"
#include "esp_crt_bundle.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_sntp.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_heap_caps.h"
#include "esp_codec_dev.h"
#include "esp_timer.h"
#ifdef ESP_PLATFORM
#include "lwip/sockets.h"          /* UDP broadcast to find Lyrion servers */
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#endif
#include "esp_system.h"
#include "mdns.h"                  /* http://p4lms.local/ (espressif/mdns component) */
#include <ctype.h>
#include <strings.h>

#include "driver/jpeg_decode.h"
#include <png.h>

#include "cJSON.h"

#include "lvgl.h"

/* Montserrat Medium with accented letters (Latin-1 + Latin Extended-A) and
 * the LVGL symbols; built from main/fonts/ (see CMakeLists.txt).  40 and 48
 * are only used for the clock and date, so they carry plain ASCII. */
LV_FONT_DECLARE(p4_font_14)
LV_FONT_DECLARE(p4_font_16)
LV_FONT_DECLARE(p4_font_18)
LV_FONT_DECLARE(p4_font_28)
LV_FONT_DECLARE(p4_font_40)
LV_FONT_DECLARE(p4_font_48)
LV_FONT_DECLARE(p4_clock_montserrat)
LV_FONT_DECLARE(p4_clock_oswald)
LV_FONT_DECLARE(p4_clock_dseg7)
LV_FONT_DECLARE(p4_clock_dmserif)
LV_FONT_DECLARE(p4_clock_orbitron)
#define P4_SYMBOL_STAR "\xEF\x80\x85"     /* U+F005, in the p4 fonts */

#include "bsp/esp-bsp.h"
#include "bsp/display.h"


static const char *TAG = "P4-LMS";

/* cJSON makes one small allocation per value; a big reply (a long app or
 * radio menu) would otherwise eat the scarce internal RAM. */
static void *cjson_psram_malloc(size_t n)
{
    void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return p != NULL ? p : malloc(n);
}

/*
 * The program is split into the ordered parts below.  They are included
 * here, in this exact order, and built as one unit - so everything behaves
 * exactly as when it was a single file.  The order matters: C needs things
 * declared before they are used.
 */
#include "parts/01_themes.c"
#include "parts/02_config.c"
#include "parts/03_shared_state.c"
#include "parts/03b_ui_jobs.c"
#include "parts/04_vu_and_visualizer.c"
#include "parts/05_screensaver_state.c"
#include "parts/06_wifi_state_and_locations.c"
#include "parts/07_declarations_audio_screensaver.c"
#include "parts/08_now_playing.c"
#include "parts/09_commands_and_home_assistant.c"
#include "parts/09b_lyrion_events.c"
#include "parts/09c_mdns.c"
#include "parts/10_wifi.c"
#include "parts/11_transport_controls.c"
#include "parts/12_source_browser_core.c"
#include "parts/13_settings_themes.c"
#include "parts/14_power_and_browser_rows.c"
#include "parts/15_queue_save_and_browser_query.c"
#include "parts/16_wifi_picker.c"
#include "parts/17_lyrion_server_and_tasks.c"
#include "parts/18_widgets.c"
#include "parts/18b_navigation.c"
#include "parts/18c_alarms.c"
#include "parts/18d_screensaver_panel.c"
#include "parts/18w_weather.c"
#include "parts/18e_screensavers.c"
#include "parts/18f_playlists.c"
#include "qr/p4qr.c"                /* QR code maker (Project Nayuki, MIT) */
#include "parts/18u_updates.c"
#include "parts/18g_about.c"
#include "parts/19_web_settings.c"

/* ============================================================
 * MAIN
 * ============================================================ */

void app_main(void)
{
    ESP_LOGI(
        TAG,
        "Starting P4-LMS"
    );

    /* Keep the chip's small internal RAM for what really needs it (Wi-Fi's
     * DMA buffers above all).  By default any malloc() under 16 KB goes to
     * internal RAM first, so the interface's thousands of small objects had
     * filled it and the Wi-Fi link failed to start ("dma_alloc failed").
     * From here on, anything of 64 bytes or more comes from the 32 MB PSRAM
     * first; drivers that need internal / DMA memory still ask for it. */
    heap_caps_malloc_extmem_enable(64);

    {
        cJSON_Hooks hooks = { .malloc_fn = cjson_psram_malloc, .free_fn = free };
        cJSON_InitHooks(&hooks);
    }

    /* Local time zone is set once here; SNTP only supplies the time. */
    time_init_timezone();

    /* Saved look-and-feel settings are read before the first widget exists;
     * otherwise everything built earlier keeps the default (amber) colours. */
    early_nvs_init();
    ui_theme_load();

    /* Which location (server + players) to run as: the saved profile for the
     * Wi-Fi network that will be used. */
    wifi_load_saved_creds();
    lms_profiles_load();
    lms_select_at_boot(wifi_cur_ssid);

    /* Settings from the web page: Home Assistant, switches, screensaver. */
    web_settings_load();
    visualizer_style_load();


    /* ========================================================
     * CREATE LYRION COMMAND QUEUE
     * ======================================================== */

    lyrion_command_queue =
        xQueueCreate(
            8,
            sizeof(lyrion_command_t)
        );


    ESP_ERROR_CHECK(
        lyrion_command_queue != NULL
            ? ESP_OK
            : ESP_ERR_NO_MEM
    );


    /* ========================================================
     * CREATE SOURCE BROWSER QUEUE
     * ======================================================== */

    source_queue =
        xQueueCreate(
            8,
            sizeof(source_request_t)
        );

    ha_command_queue = xQueueCreate(4, sizeof(ha_command_t));
    if (ha_command_queue == NULL) {
        ESP_LOGW(TAG, "Failed to create Home Assistant command queue; power control disabled");
    }

    if (lyrion_command_queue != NULL && ha_command_queue != NULL) {
        command_queue_set = xQueueCreateSet(8 + 4);
        if (command_queue_set == NULL ||
            xQueueAddToSet(lyrion_command_queue, command_queue_set) != pdPASS ||
            xQueueAddToSet(ha_command_queue, command_queue_set) != pdPASS) {
            ESP_LOGW(TAG, "Failed to set up the combined command queue; Home Assistant polling disabled");
            command_queue_set = NULL;
        }
    }

    ESP_ERROR_CHECK(
        source_queue != NULL
            ? ESP_OK
            : ESP_ERR_NO_MEM
    );


    /* ========================================================
     * CREATE LYRION REFRESH EVENT
     * ======================================================== */

    lyrion_refresh_event =
        xEventGroupCreate();


    ESP_ERROR_CHECK(
        lyrion_refresh_event != NULL
            ? ESP_OK
            : ESP_ERR_NO_MEM
    );


    /* ========================================================
     * DISPLAY CONFIGURATION
     * ======================================================== */

    bsp_display_cfg_t cfg = {
        .lv_adapter_cfg =
            ESP_LV_ADAPTER_DEFAULT_CONFIG(),

        .rotation =
            ESP_LV_ADAPTER_ROTATE_0,

        .tear_avoid_mode =
            ESP_LV_ADAPTER_TEAR_AVOID_MODE_TRIPLE_PARTIAL,

        /*
         * These are the touch settings that worked on your
         * physically mounted display.
         */
        .touch_flags = {
            .swap_xy = 0,
            .mirror_x = 0,
            .mirror_y = 0,
        },
    };

    /* The LVGL task now also runs the UI jobs (list building for SOURCE, the
     * Wi-Fi and server screens), so give it more than the 8 KB default. */
    cfg.lv_adapter_cfg.task_stack_size = 16 * 1024;

    /* LVGL gets CPU 0 to itself (the network, cover decoding and microphone
     * tasks run on CPU 1) and always pauses at least 2 ms between rounds, so
     * the idle task and touch input are never starved during heavy redraws. */
    cfg.lv_adapter_cfg.task_core_id = 0;
    cfg.lv_adapter_cfg.task_min_delay_ms = 2;


    /* ========================================================
     * START DISPLAY + TOUCH + LVGL
     * ======================================================== */

    lv_display_t *display =
        bsp_display_start_with_config(
            &cfg
        );


    ESP_ERROR_CHECK(
        display != NULL
            ? ESP_OK
            : ESP_FAIL
    );


    ESP_LOGI(
        TAG,
        "Display started"
    );


    /* ========================================================
     * BACKLIGHT
     * ======================================================== */

    ESP_ERROR_CHECK(
        bsp_display_backlight_on()
    );


    /* ========================================================
     * LOCK LVGL
     * ======================================================== */

    ESP_ERROR_CHECK(
        bsp_display_lock(-1)
            ? ESP_OK
            : ESP_ERR_TIMEOUT
    );


    /* ========================================================
     * ROTATE DISPLAY 180°
     * ======================================================== */

    bsp_display_rotate(
        display,
        LV_DISPLAY_ROTATION_180
    );


    ESP_LOGI(
        TAG,
        "Display rotated 180 degrees"
    );


    /* ========================================================
     * MAIN SCREEN
     * ======================================================== */

    lv_obj_t *screen =
        lv_screen_active();

    /* Every label without its own font inherits this one (with accents). */
    lv_obj_set_style_text_font(screen, &p4_font_14, 0);
    lv_obj_set_style_text_font(lv_layer_top(), &p4_font_14, 0);


    lv_obj_set_style_bg_color(
        screen,
        lv_color_hex(UI_BG),
        LV_PART_MAIN
    );


    lv_obj_set_style_bg_opa(
        screen,
        LV_OPA_COVER,
        LV_PART_MAIN
    );

    /* A tap on the album art opens that album in Library; the visualizer
     * opens from Settings > Visualizer. */


    /* ========================================================
     * HEADER
     * ======================================================== */

    lv_obj_t *header =
        lv_obj_create(screen);


    lv_obj_remove_style_all(
        header
    );


    lv_obj_set_size(
        header,
        1024,
        HEADER_H
    );


    lv_obj_set_pos(
        header,
        0,
        0
    );


    lv_obj_set_style_bg_color(
        header,
        lv_color_hex(UI_PANEL),
        LV_PART_MAIN
    );


    lv_obj_set_style_bg_opa(
        header,
        LV_OPA_COVER,
        LV_PART_MAIN
    );

    /* Clean header: no horizontal separator between the top controls and album art. */

    /* ========================================================
     * POWER BUTTON
     * ======================================================== */
    /* The "P4-LMS" wordmark used to sit here; that space now holds the one
     * button that turns the current room's amp / pre-amp on and off. */

    /* The room chip (left) opens the room list; players, SOURCE, power and
     * settings now live on the bottom navigation bar (18b_navigation.c). */
    nav_room_chip_create(header);

    update_player_button_styles_locked();

    /* Status dots belonged to the old header player buttons, which the room
     * chip replaced (the room list shows each room's state instead).  Only
     * create a dot where a button exists - create_status_dot() styles its
     * button, so a NULL one would crash at start-up. */
    memset(ui_house_applied, 0xFF, sizeof(ui_house_applied));
    for (int i = 0; i < (int)LYRION_PLAYER_COUNT; i++) {
        player_status_dots[i] = (player_buttons[i] != NULL) ? create_status_dot(player_buttons[i]) : NULL;
    }


    /* ========================================================
     * SOURCE / LIBRARY BUTTON
     * ======================================================== */

    connection_label =
        lv_label_create(header);


    lv_label_set_text(
        connection_label,
        "Wi-Fi   CONNECTING..."
    );


    lv_obj_set_style_text_color(
        connection_label,
        lv_color_hex(UI_CONN_AMBER),
        0
    );


    lv_obj_align(
        connection_label,
        LV_ALIGN_RIGHT_MID,
        -28,
        0
    );

    /* The Wi-Fi status is display-only; Wi-Fi is set up in Settings > Wi-Fi. */

    /* Small clock in the header; stays blank until SNTP has the time. */
    header_clock_label =
        lv_label_create(header);

    lv_label_set_text(header_clock_label, "");
    lv_obj_set_style_text_color(
        header_clock_label,
        lv_color_hex(UI_TEXT_SOFT),
        0
    );
    lv_obj_set_style_text_font(
        header_clock_label,
        &p4_font_28,
        0
    );
    lv_obj_align(
        header_clock_label,
        LV_ALIGN_CENTER,
        0,
        0
    );


    /* ========================================================
     * ALBUM ART
     * ======================================================== */

    const uint32_t np_first_child = lv_obj_get_child_count(screen);

    lv_obj_t *album =
        lv_obj_create(screen);


    lv_obj_remove_style_all(
        album
    );


    lv_obj_set_size(
        album,
        ALBUM_ART_WIDTH,
        ALBUM_ART_HEIGHT
    );


    lv_obj_set_pos(
        album,
        28,
        102
    );


    lv_obj_set_style_bg_color(
        album,
        lv_color_hex(UI_ALBUM_BG),
        LV_PART_MAIN
    );


    lv_obj_set_style_bg_opa(
        album,
        LV_OPA_COVER,
        LV_PART_MAIN
    );

    lv_obj_set_style_border_width(
        album,
        1,
        LV_PART_MAIN
    );
    lv_obj_set_style_border_color(
        album,
        lv_color_hex(UI_AMBER),
        LV_PART_MAIN
    );
    lv_obj_set_style_radius(
        album,
        4,
        LV_PART_MAIN
    );


    /* Tapping the album art opens this song's album in Library. */
    lv_obj_add_flag(album, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(album, 0);
    lv_obj_set_style_translate_y(album, 2, LV_STATE_PRESSED);      /* press feedback */
    lv_obj_add_event_cb(album, album_art_event_cb, LV_EVENT_CLICKED, NULL);

    album_image = lv_image_create(album);
    lv_obj_clear_flag(album_image, LV_OBJ_FLAG_CLICKABLE);      /* touches go to the frame */


    lv_obj_set_size(
        album_image,
        ALBUM_ART_WIDTH,
        ALBUM_ART_HEIGHT
    );

    lv_obj_center(album_image);
    lv_obj_add_flag(album_image, LV_OBJ_FLAG_HIDDEN);


    album_placeholder =
        lv_label_create(album);


    lv_label_set_text(
        album_placeholder,
        "ALBUM ART"
    );


    lv_obj_set_style_text_color(
        album_placeholder,
        lv_color_hex(UI_PLACEHOLDER),
        0
    );


    lv_obj_center(
        album_placeholder
    );


    /* ========================================================
     * NOW PLAYING RECEIVER PANEL
     * ======================================================== */

    lv_obj_t *now_playing_panel = lv_obj_create(screen);
    lv_obj_remove_style_all(now_playing_panel);
    lv_obj_clear_flag(now_playing_panel, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(now_playing_panel, 575, 365);
    lv_obj_set_pos(now_playing_panel, 423, 84);
    lv_obj_set_style_bg_color(now_playing_panel, lv_color_hex(UI_PANEL_ALT), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(now_playing_panel, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(now_playing_panel, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(now_playing_panel, lv_color_hex(UI_EDGE), LV_PART_MAIN);
    lv_obj_set_style_radius(now_playing_panel, 12, LV_PART_MAIN);
    lv_obj_clear_flag(now_playing_panel, LV_OBJ_FLAG_SCROLLABLE);

    /* ========================================================
     * NOW PLAYING CAPTION
     * ======================================================== */

    lv_obj_t *now_playing_label =
        lv_label_create(screen);

    lv_label_set_text(
        now_playing_label,
        "NOW PLAYING"
    );

    lv_obj_set_style_text_color(
        now_playing_label,
        lv_color_hex(UI_ACCENT_GLOW),
        0
    );

    lv_obj_set_style_text_font(
        now_playing_label,
        &p4_font_14,
        0
    );

    lv_obj_align(
        now_playing_label,
        LV_ALIGN_TOP_LEFT,
        435,
        96
    );


    /* ========================================================
     * PLAY STATE CHIP
     * ======================================================== */

    play_state_label =
        lv_label_create(screen);

    lv_label_set_text(play_state_label, "");
    lv_obj_set_style_text_color(
        play_state_label,
        lv_color_hex(UI_TEXT_DIM),
        0
    );
    lv_obj_set_style_text_font(
        play_state_label,
        &p4_font_14,
        0
    );
    lv_obj_align(
        play_state_label,
        LV_ALIGN_TOP_RIGHT,
        -34,
        94
    );
    lv_obj_set_style_bg_color(play_state_label, lv_color_hex(UI_ACCENT_DARK), 0);
    lv_obj_set_style_bg_opa(play_state_label, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(play_state_label, 12, 0);
    lv_obj_set_style_pad_hor(play_state_label, 12, 0);
    lv_obj_set_style_pad_ver(play_state_label, 4, 0);


    /* ========================================================
     * ARTIST
     * ======================================================== */

    artist_label =
        lv_label_create(screen);


    lv_label_set_text(
        artist_label,
        "Artist Name"
    );


    lv_obj_set_style_text_color(
        artist_label,
        lv_color_hex(UI_TEXT_SOFT),
        0
    );
    lv_obj_set_style_text_font(
        artist_label,
        &p4_font_18,
        0
    );
    /* Fixed width so a long name scrolls instead of running off the panel. */
    lv_obj_set_width(artist_label, 550);
    lv_label_set_long_mode(artist_label, LV_LABEL_LONG_SCROLL_CIRCULAR);


    lv_obj_align(
        artist_label,
        LV_ALIGN_TOP_LEFT,
        435,
        166
    );


    /* ========================================================
     * SONG TITLE
     * ======================================================== */

    song_label =
        lv_label_create(screen);


    lv_label_set_text(
        song_label,
        "Song Title"
    );


    lv_obj_set_style_text_color(
        song_label,
        lv_color_hex(UI_TEXT),
        0
    );
    lv_obj_set_style_text_font(
        song_label,
        &p4_font_28,
        0
    );


    lv_obj_set_width(
        song_label,
        550
    );


    /* Long titles scroll (only when they do not fit) instead of ending in "...". */
    lv_label_set_long_mode(
        song_label,
        LV_LABEL_LONG_SCROLL_CIRCULAR
    );


    lv_obj_align(
        song_label,
        LV_ALIGN_TOP_LEFT,
        435,
        124
    );

    /* Tap the song title to open the queue. */
    lv_obj_add_flag(song_label, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(song_label, 8);
    lv_obj_add_event_cb(song_label, song_label_event_cb, LV_EVENT_CLICKED, NULL);


    /* ========================================================
     * ALBUM NAME
     * ======================================================== */

    album_name_label =
        lv_label_create(screen);


    lv_label_set_text(
        album_name_label,
        "Album Name"
    );


    lv_obj_set_style_text_color(
        album_name_label,
        lv_color_hex(UI_LABEL_SOFT),
        0
    );


    lv_obj_set_width(
        album_name_label,
        550
    );


    lv_label_set_long_mode(
        album_name_label,
        LV_LABEL_LONG_SCROLL_CIRCULAR
    );
    lv_obj_set_style_text_font(album_name_label, &p4_font_16, 0);


    lv_obj_align(
        album_name_label,
        LV_ALIGN_TOP_LEFT,
        435,
        192
    );

    /* Audio format badge, e.g. "FLAC  |  44.1 kHz  |  16-bit". */
    format_label = lv_label_create(screen);
    lv_label_set_text(format_label, "");
    lv_obj_set_width(format_label, 296);   /* + PLAYLIST and FAV buttons to its right */
    lv_label_set_long_mode(format_label, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_color(format_label, lv_color_hex(UI_TEXT_DIM), 0);
    lv_obj_set_style_text_font(format_label, &p4_font_14, 0);
    lv_obj_align(format_label, LV_ALIGN_TOP_LEFT, 435, 222);
    lv_obj_set_style_border_width(format_label, 1, 0);
    lv_obj_set_style_border_color(format_label, lv_color_hex(UI_EDGE), 0);
    lv_obj_set_style_radius(format_label, 12, 0);
    lv_obj_set_style_pad_hor(format_label, 12, 0);
    lv_obj_set_style_pad_ver(format_label, 3, 0);

    /* Add / remove what's playing from LMS Favorites. Hidden until known. */
    fav_button = create_button(screen, LV_SYMBOL_PLUS " FAV", 104, 34);
    lv_obj_align(fav_button, LV_ALIGN_TOP_LEFT, 886, 216);
    lv_obj_set_ext_click_area(fav_button, 6);
    lv_obj_add_flag(fav_button, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(fav_button, fav_button_event_cb, LV_EVENT_CLICKED, NULL);

    /* Add what's playing to one of the saved playlists (18f_playlists.c). */
    np_list_button = create_button(screen, LV_SYMBOL_LIST " + PLAYLIST", 140, 34);   /* 140 wide: fits the longer word */
    lv_obj_align(np_list_button, LV_ALIGN_TOP_LEFT, 738, 216);
    lv_obj_set_ext_click_area(np_list_button, 6);
    lv_obj_add_flag(np_list_button, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(np_list_button, np_list_button_event_cb, LV_EVENT_CLICKED, NULL);


    /* ========================================================
     * PROGRESS BAR
     * ======================================================== */

    progress_bar =
        lv_bar_create(screen);


    lv_obj_set_size(
        progress_bar,
        555,
        14
    );


    lv_obj_align(
        progress_bar,
        LV_ALIGN_TOP_LEFT,
        435,
        266
    );
    /* Per-mille scale: smooth movement on long tracks. */
    lv_bar_set_range(
        progress_bar,
        0,
        1000
    );


    lv_bar_set_value(
        progress_bar,
        0,
        LV_ANIM_OFF
    );


    lv_obj_set_style_bg_color(
        progress_bar,
        lv_color_hex(UI_TRACK),
        LV_PART_MAIN
    );


    lv_obj_set_style_bg_opa(
        progress_bar,
        LV_OPA_COVER,
        LV_PART_MAIN
    );


    lv_obj_set_style_bg_color(
        progress_bar,
        lv_color_hex(UI_AMBER),
        LV_PART_INDICATOR
    );


    lv_obj_set_style_bg_opa(
        progress_bar,
        LV_OPA_COVER,
        LV_PART_INDICATOR
    );
    lv_obj_set_style_radius(progress_bar, 7, LV_PART_MAIN);
    lv_obj_set_style_radius(progress_bar, 7, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(progress_bar, lv_color_hex(UI_ACCENT), LV_PART_INDICATOR);

    /* Tap or drag on the bar to seek.  The bar is thin, so make the touch
     * area taller than what is drawn. */
    lv_obj_add_flag(progress_bar, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(progress_bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_ext_click_area(progress_bar, 14);

    progress_knob = lv_obj_create(screen);
    lv_obj_remove_style_all(progress_knob);
    lv_obj_set_size(progress_knob, PROGRESS_KNOB_PX, PROGRESS_KNOB_PX);
    lv_obj_set_style_radius(progress_knob, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(progress_knob, lv_color_hex(UI_ACCENT_BRIGHT), 0);
    lv_obj_set_style_bg_opa(progress_knob, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(progress_knob, 2, 0);
    lv_obj_set_style_border_color(progress_knob, lv_color_hex(UI_ACCENT), 0);
    lv_obj_clear_flag(progress_knob, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(progress_knob, LV_ALIGN_TOP_LEFT, 435 - PROGRESS_KNOB_PX / 2,
                 266 + 7 - PROGRESS_KNOB_PX / 2);
    lv_obj_add_event_cb(progress_bar, progress_seek_event_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(progress_bar, progress_seek_event_cb, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(progress_bar, progress_seek_event_cb, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(progress_bar, progress_seek_event_cb, LV_EVENT_PRESS_LOST, NULL);


    /* ========================================================
     * ELAPSED TIME
     * ======================================================== */

    elapsed_label =
        lv_label_create(screen);


    lv_label_set_text(
        elapsed_label,
        "0:00"
    );


    lv_obj_set_style_text_color(
        elapsed_label,
        lv_color_hex(UI_LABEL_DIM),
        0
    );


    lv_obj_align(
        elapsed_label,
        LV_ALIGN_TOP_LEFT,
        435,
        288
    );


    /* ========================================================
     * TOTAL TIME
     * ======================================================== */

    total_label =
        lv_label_create(screen);


    lv_label_set_text(
        total_label,
        "0:00"
    );


    lv_obj_set_style_text_color(
        total_label,
        lv_color_hex(UI_LABEL_DIM),
        0
    );


    lv_obj_align(total_label, LV_ALIGN_TOP_RIGHT, -34, 288);   /* right edge of the bar */
    /* Tap: the song's length <-> the time left (remembered). */
    np_time_left_load();
    lv_obj_add_flag(total_label, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(total_label, 18);
    lv_obj_add_event_cb(total_label, np_total_tap_event_cb, LV_EVENT_CLICKED, NULL);


    /* ========================================================
     * PREVIOUS
     * ======================================================== */

    lv_obj_t *previous =
        create_button(
            screen,
            LV_SYMBOL_PREV,
            95,
            72
        );


    lv_obj_align(
        previous,
        LV_ALIGN_TOP_LEFT,
        435,
        316
    );

    lv_obj_set_style_bg_color(previous, lv_color_hex(UI_BUTTON), LV_PART_MAIN);
    lv_obj_set_style_border_width(previous, 2, LV_PART_MAIN);
    lv_obj_set_style_border_color(previous, lv_color_hex(UI_AMBER), LV_PART_MAIN);
    lv_obj_set_style_text_color(lv_obj_get_child(previous, 0), lv_color_hex(UI_ACCENT_BRIGHT), 0);


    lv_obj_add_event_cb(
        previous,
        previous_button_event_cb,
        LV_EVENT_CLICKED,
        NULL
    );


    /* ========================================================
     * PLAY / PAUSE
     * ======================================================== */

    play_button =
        create_button(
            screen,
            LV_SYMBOL_PLAY,
            170,
            72
        );


    lv_obj_align(
        play_button,
        LV_ALIGN_TOP_LEFT,
        540,
        316
    );


    lv_obj_add_event_cb(
        play_button,
        play_button_event_cb,
        LV_EVENT_CLICKED,
        NULL
    );

    lv_obj_set_style_bg_color(
        play_button,
        lv_color_hex(UI_ACCENT_DARK),
        LV_PART_MAIN
    );
    lv_obj_set_style_text_color(
        lv_obj_get_child(play_button, 0),
        lv_color_hex(UI_ACCENT_BRIGHT),
        0
    );
    lv_obj_set_style_border_width(play_button, 2, LV_PART_MAIN);
    lv_obj_set_style_border_color(play_button, lv_color_hex(UI_AMBER), LV_PART_MAIN);


    /* ========================================================
     * NEXT
     * ======================================================== */

    lv_obj_t *next =
        create_button(
            screen,
            LV_SYMBOL_NEXT,
            95,
            72
        );


    lv_obj_align(
        next,
        LV_ALIGN_TOP_LEFT,
        720,
        316
    );

    lv_obj_set_style_bg_color(next, lv_color_hex(UI_BUTTON), LV_PART_MAIN);
    lv_obj_set_style_border_width(next, 2, LV_PART_MAIN);
    lv_obj_set_style_border_color(next, lv_color_hex(UI_AMBER), LV_PART_MAIN);
    lv_obj_set_style_text_color(lv_obj_get_child(next, 0), lv_color_hex(UI_ACCENT_BRIGHT), 0);


    lv_obj_add_event_cb(
        next,
        next_button_event_cb,
        LV_EVENT_CLICKED,
        NULL
    );

    /* Shuffle and repeat toggles, to the right of the transport buttons. */
    shuffle_button = create_button(screen, LV_SYMBOL_SHUFFLE, 52, 72);
    lv_obj_align(shuffle_button, LV_ALIGN_TOP_LEFT, 823, 316);
    lv_obj_set_style_border_width(shuffle_button, 2, LV_PART_MAIN);
    lv_obj_set_style_text_font(lv_obj_get_child(shuffle_button, 0), &p4_font_18, 0);
    lv_obj_add_event_cb(shuffle_button, shuffle_button_event_cb, LV_EVENT_CLICKED, NULL);

    repeat_button = create_button(screen, LV_SYMBOL_LOOP, 52, 72);
    lv_obj_align(repeat_button, LV_ALIGN_TOP_LEFT, 881, 316);
    lv_obj_set_style_border_width(repeat_button, 2, LV_PART_MAIN);
    lv_obj_set_style_text_font(lv_obj_get_child(repeat_button, 0), &p4_font_18, 0);
    lv_obj_add_event_cb(repeat_button, repeat_button_event_cb, LV_EVENT_CLICKED, NULL);

    /* System power for this room (amp + pre-amp, via Home Assistant). */
    np_power_button = create_button(screen, LV_SYMBOL_POWER, 52, 72);
    lv_obj_align(np_power_button, LV_ALIGN_TOP_LEFT, 939, 316);
    lv_obj_set_style_border_width(np_power_button, 2, LV_PART_MAIN);
    lv_obj_set_style_text_font(lv_obj_get_child(np_power_button, 0), &p4_font_18, 0);
    lv_obj_add_event_cb(np_power_button, np_power_event_cb, LV_EVENT_CLICKED, NULL);
    np_power_apply_locked();

    ui_apply_mode_buttons_locked();


    /* ========================================================
     * VOLUME LABEL
     * ======================================================== */

    /* Speaker button (same size as - and +): tap to mute, tap again to go
     * back to the old volume. */
    /* (44 tall at y 400: clear of the panel's outline, which ends at 449) */
    volume_mute_button = create_button(screen, LV_SYMBOL_VOLUME_MAX, 48, 44);
    lv_obj_align(volume_mute_button, LV_ALIGN_TOP_LEFT, 435, 400);
    lv_obj_add_event_cb(volume_mute_button, volume_icon_event_cb, LV_EVENT_CLICKED, NULL);
    volume_icon_apply_locked();

    /* "-" button: volume down 5 (the slider is narrower to make room). */
    lv_obj_t *vol_down = create_button(screen, LV_SYMBOL_MINUS, 48, 44);
    lv_obj_align(vol_down, LV_ALIGN_TOP_LEFT, 493, 400);
    lv_obj_add_event_cb(vol_down, volume_step_event_cb, LV_EVENT_CLICKED, (void *)(intptr_t)-5);


    /* ========================================================
     * VOLUME SLIDER
     * ======================================================== */

    volume_slider =
        lv_slider_create(screen);


    /* 300 wide (was 445) so the speaker, - and + buttons fit beside it. */
    lv_obj_set_size(
        volume_slider,
        300,
        18
    );
    /* The bar is only 18 px tall: let touches a little above/below it count. */
    lv_obj_set_ext_click_area(volume_slider, 16);


    /* Volume control: replace LVGL's default blue with the same amber
     * receiver palette used everywhere else. */
    lv_obj_set_style_bg_color(
        volume_slider,
        lv_color_hex(UI_SLIDER_BG),
        LV_PART_MAIN
    );
    lv_obj_set_style_bg_opa(
        volume_slider,
        LV_OPA_COVER,
        LV_PART_MAIN
    );
    lv_obj_set_style_bg_color(
        volume_slider,
        lv_color_hex(UI_TERTIARY),
        LV_PART_INDICATOR
    );
    lv_obj_set_style_bg_opa(
        volume_slider,
        LV_OPA_COVER,
        LV_PART_INDICATOR
    );
    lv_obj_set_style_bg_color(
        volume_slider,
        lv_color_hex(UI_ACCENT),
        LV_PART_KNOB
    );
    lv_obj_set_style_border_color(
        volume_slider,
        lv_color_hex(UI_ACCENT_DARK),
        LV_PART_KNOB
    );
    lv_obj_set_style_border_width(volume_slider, 2, LV_PART_KNOB);
    lv_obj_set_style_pad_all(volume_slider, 9, LV_PART_KNOB);


    lv_obj_align(
        volume_slider,
        LV_ALIGN_TOP_LEFT,
        558,
        413                         /* centred on the buttons beside it */
    );

    /* "+" button: volume up 5. */
    lv_obj_t *vol_up = create_button(screen, LV_SYMBOL_PLUS, 48, 44);
    lv_obj_align(vol_up, LV_ALIGN_TOP_LEFT, 874, 400);
    lv_obj_add_event_cb(vol_up, volume_step_event_cb, LV_EVENT_CLICKED, (void *)(intptr_t)5);


    lv_slider_set_range(
        volume_slider,
        0,
        100
    );


    lv_slider_set_value(
        volume_slider,
        0,
        LV_ANIM_OFF
    );


    /* ========================================================
     * VOLUME VALUE
     * ======================================================== */

    volume_value_label =
        lv_label_create(screen);


    lv_label_set_text(
        volume_value_label,
        "0%"
    );


    lv_obj_set_style_text_color(
        volume_value_label,
        lv_color_hex(UI_VALUE_TEXT),
        0
    );


    lv_obj_align(
        volume_value_label,
        LV_ALIGN_TOP_LEFT,
        940,
        411
    );


    lv_obj_add_event_cb(
        volume_slider,
        volume_slider_event_cb,
        LV_EVENT_VALUE_CHANGED,
        NULL
    );
    lv_obj_add_event_cb(
        volume_slider,
        volume_slider_event_cb,
        LV_EVENT_RELEASED,
        NULL
    );


    /* ========================================================
     * REAL-TIME VU METER
     * ======================================================== */

    /* Room for the bottom navigation bar: the album art moves up 30 px and
     * the right-hand column 24 px (everything created since np_first_child). */
    {
        const uint32_t np_last_child = lv_obj_get_child_count(screen);
        for (uint32_t k = np_first_child; k < np_last_child; k++) {
            lv_obj_t *o = lv_obj_get_child(screen, (int32_t)k);
            const int32_t dy = (o == album) ? -30 : -24;
            lv_obj_set_y(o, lv_obj_get_style_y(o, LV_PART_MAIN) + dy);
        }
    }

    lv_obj_t *spectrum = lv_obj_create(screen);
    lv_obj_remove_style_all(spectrum);
    /* Tap the VU meter to open the full-screen visualizer. */
    lv_obj_add_flag(spectrum, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_translate_y(spectrum, 2, LV_STATE_PRESSED);
    lv_obj_add_event_cb(spectrum, vu_meter_tap_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_set_size(
        spectrum,
        VU_PANEL_WIDTH,
        VU_PANEL_HEIGHT
    );
    lv_obj_align(
        spectrum,
        LV_ALIGN_BOTTOM_MID,
        0,
        -(NAV_BAR_H + 8)
    );
    lv_obj_clear_flag(
        spectrum,
        LV_OBJ_FLAG_SCROLLABLE
    );
    lv_obj_set_style_bg_color(
        spectrum,
        lv_color_hex(UI_PANEL_ALT),
        LV_PART_MAIN
    );
    lv_obj_set_style_bg_opa(
        spectrum,
        LV_OPA_COVER,
        LV_PART_MAIN
    );
    lv_obj_set_style_border_width(spectrum, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(spectrum, lv_color_hex(UI_EDGE), LV_PART_MAIN);
    lv_obj_set_style_radius(spectrum, 4, LV_PART_MAIN);

    lv_obj_t *vu_title = lv_label_create(spectrum);
    lv_label_set_text(
        vu_title,
        "VU METER"
    );
    lv_obj_set_style_text_color(
        vu_title,
        lv_color_hex(UI_ACCENT_BRIGHT),
        0
    );
    lv_obj_set_style_text_font(
        vu_title,
        &p4_font_14,
        0
    );
    lv_obj_add_flag(vu_title, LV_OBJ_FLAG_HIDDEN);        /* compact meter: no title row */

    lv_obj_t *left_label = lv_label_create(spectrum);
    lv_label_set_text(left_label, "L");
    lv_obj_set_style_text_color(
        left_label,
        lv_color_hex(UI_AMBER),
        0
    );
    lv_obj_set_style_text_font(
        left_label,
        &p4_font_14,
        0
    );
    lv_obj_align(left_label, LV_ALIGN_TOP_LEFT, 14, VU_ROW_L_Y + 1);

    lv_obj_t *right_label = lv_label_create(spectrum);
    lv_label_set_text(right_label, "R");
    lv_obj_set_style_text_color(
        right_label,
        lv_color_hex(UI_AMBER),
        0
    );
    lv_obj_set_style_text_font(
        right_label,
        &p4_font_14,
        0
    );
    lv_obj_align(right_label, LV_ALIGN_TOP_LEFT, 14, VU_ROW_R_Y + 1);

    const int meter_start_x = VU_METER_START_X;
    const int meter_width = VU_METER_WIDTH;

    for (int i = 0; i < VU_SEGMENT_COUNT; i++) {
        lv_color_t segment_color;

        if (i >= 19) {
            segment_color = lv_color_hex(UI_RED);
        } else if (i >= 17) {
            segment_color = lv_color_hex(UI_AMBER);
        } else {
            segment_color = lv_color_hex(UI_ACCENT);
        }

        vu_left_segments[i] =
            lv_obj_create(spectrum);
        lv_obj_remove_style_all(
            vu_left_segments[i]
        );
        lv_obj_set_size(
            vu_left_segments[i],
            VU_SEGMENT_WIDTH,
            VU_SEGMENT_HEIGHT
        );
        lv_obj_set_style_bg_color(
            vu_left_segments[i],
            segment_color,
            LV_PART_MAIN
        );
        lv_obj_set_style_bg_opa(
            vu_left_segments[i],
            LV_OPA_20,
            LV_PART_MAIN
        );
        lv_obj_set_style_radius(
            vu_left_segments[i],
            2,
            LV_PART_MAIN
        );
        lv_obj_align(
            vu_left_segments[i],
            LV_ALIGN_TOP_LEFT,
            meter_start_x +
            i * (VU_SEGMENT_WIDTH + VU_SEGMENT_GAP),
            VU_ROW_L_Y
        );

        vu_right_segments[i] =
            lv_obj_create(spectrum);
        lv_obj_remove_style_all(
            vu_right_segments[i]
        );
        lv_obj_set_size(
            vu_right_segments[i],
            VU_SEGMENT_WIDTH,
            VU_SEGMENT_HEIGHT
        );
        lv_obj_set_style_bg_color(
            vu_right_segments[i],
            segment_color,
            LV_PART_MAIN
        );
        lv_obj_set_style_bg_opa(
            vu_right_segments[i],
            LV_OPA_20,
            LV_PART_MAIN
        );
        lv_obj_set_style_radius(
            vu_right_segments[i],
            2,
            LV_PART_MAIN
        );
        lv_obj_align(
            vu_right_segments[i],
            LV_ALIGN_TOP_LEFT,
            meter_start_x +
            i * (VU_SEGMENT_WIDTH + VU_SEGMENT_GAP),
            VU_ROW_R_Y
        );
    }

    /* Peak-hold markers give the meter a classic hardware-meter feel. */
    vu_left_peak_marker = lv_obj_create(spectrum);
    lv_obj_remove_style_all(vu_left_peak_marker);
    lv_obj_set_size(vu_left_peak_marker, 2, VU_SEGMENT_HEIGHT + 4);
    lv_obj_set_style_bg_color(vu_left_peak_marker, lv_color_hex(UI_AMBER), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(vu_left_peak_marker, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_add_flag(vu_left_peak_marker, LV_OBJ_FLAG_HIDDEN);

    vu_right_peak_marker = lv_obj_create(spectrum);
    lv_obj_remove_style_all(vu_right_peak_marker);
    lv_obj_set_size(vu_right_peak_marker, 2, VU_SEGMENT_HEIGHT + 4);
    lv_obj_set_style_bg_color(vu_right_peak_marker, lv_color_hex(UI_AMBER), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(vu_right_peak_marker, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_add_flag(vu_right_peak_marker, LV_OBJ_FLAG_HIDDEN);

    /* Small center divider between the two stereo rows. */
    lv_obj_t *vu_divider = lv_obj_create(spectrum);
    lv_obj_remove_style_all(vu_divider);
    lv_obj_set_size(vu_divider, meter_width, 1);
    lv_obj_set_style_bg_color(vu_divider, lv_color_hex(UI_DIVIDER), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(vu_divider, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_pos(vu_divider, meter_start_x, VU_ROW_R_Y - 3);

    /* Scale: the left side starts around -45 dBFS and the red
     * section is reserved for the hottest levels. */
    const struct {
        int db;
        const char *text;
    } scale[] = {
        {-40, "-40"},
        {-20, "-20"},
        {-10, "-10"},
        {-5,  "-5"},
        {0,   "0"}
    };

    for (size_t s = 0;
         s < sizeof(scale) / sizeof(scale[0]);
         s++) {

        float normalized =
            ((float)scale[s].db - VU_MIN_DBFS) /
            (VU_MAX_DBFS - VU_MIN_DBFS);

        int x =
            meter_start_x +
            (int)(normalized * (float)(meter_width - 1));

        lv_obj_t *tick = lv_obj_create(spectrum);
        lv_obj_remove_style_all(tick);
        lv_obj_set_size(tick, 2, 5);
        lv_obj_set_style_bg_color(
            tick,
            lv_color_hex(UI_TICK),
            LV_PART_MAIN
        );
        lv_obj_set_style_bg_opa(
            tick,
            LV_OPA_COVER,
            LV_PART_MAIN
        );
        lv_obj_align(
            tick,
            LV_ALIGN_TOP_LEFT,
            x,
            VU_ROW_R_Y + VU_SEGMENT_HEIGHT + 3
        );

        lv_obj_t *label = lv_label_create(spectrum);
        lv_label_set_text(
            label,
            scale[s].text
        );
        lv_obj_set_style_text_color(
            label,
            lv_color_hex(UI_SCALE_TEXT),
            0
        );
        lv_obj_set_style_text_font(
            label,
            &p4_font_14,
            0
        );
        lv_obj_set_pos(
            label,
            x - 12,
            VU_ROW_R_Y + VU_SEGMENT_HEIGHT + 7
        );
    }

    lv_obj_t *stereo_label =
        lv_label_create(spectrum);
    lv_label_set_text(
        stereo_label,
        "L/R  |  MICROPHONE"
    );
    lv_obj_set_style_text_color(
        stereo_label,
        lv_color_hex(UI_SCALE_TEXT),
        0
    );
    lv_obj_set_style_text_font(
        stereo_label,
        &p4_font_14,
        0
    );
    lv_obj_add_flag(stereo_label, LV_OBJ_FLAG_HIDDEN);    /* compact meter: no title row */

    /* "VU meter off" (Settings > Visualizer): a plain cover over the meter. */
    vu_off_cover_create(spectrum);

    /* The bars, markers and scale are objects too and would catch a tap
     * themselves; let every tap on the meter reach the meter. */
    for (uint32_t k = 0; k < lv_obj_get_child_count(spectrum); k++) {
        lv_obj_t *child = lv_obj_get_child(spectrum, (int32_t)k);
        lv_obj_clear_flag(child, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(child, LV_OBJ_FLAG_EVENT_BUBBLE);
    }

    ESP_LOGI(
        TAG,
        "Real-time microphone VU meter UI created: %d segments per channel",
        VU_SEGMENT_COUNT
    );

    vu_timer = lv_timer_create(
        vu_meter_timer_cb,
        50,
        NULL
    );
    lv_timer_create(vu_panel_keepalive_timer_cb, 2000, spectrum);

    /* Header clock (1 Hz) and local elapsed-time interpolation (4 Hz). */
    lv_timer_create(header_clock_timer_cb, 1000, NULL);
    lv_timer_create(playback_tick_cb, 250, NULL);

    /* ========================================================
     * SOURCE / LIBRARY BROWSER
     * ======================================================== */

    create_source_browser(screen);
    create_settings(screen);
    lv_timer_create(source_idle_timer_cb, 1000, NULL);


    /* ========================================================
     * SCREEN SAVER / STANDBY
     * ======================================================== */

    create_screensaver(screen);

    /* Optional full-screen visualizer.  It starts hidden and does not alter
     * any of the normal receiver controls until opened from Settings. */
    /* Bottom navigation bar, room list, Settings hub, notifications. */
    nav_create(screen);

    create_visualizer(screen);

    /* From here on background tasks change the screen only through UI jobs
     * (see parts/03b_ui_jobs.c). */
    ui_jobs_start();
    lvgl_mem_log("interface built");

    /* First start: open the Wi-Fi / Lyrion server screens if nothing is set
     * up yet (parts/17_lyrion_server_and_tasks.c). */
    first_start_begin();

    /* First start after this update: a one-time tip about the hidden taps
     * (parts/18g_about.c). */
    about_first_tip_schedule();


    /* ========================================================
     * INITIAL PLAYER BUTTON COLORS
     * ========================================================
     * Apply the exact same styling used when a player is selected.
     * Office therefore starts green immediately at boot.
     */
    update_player_button_styles_locked();


    /* ========================================================
     * UNLOCK LVGL
     * ======================================================== */

    bsp_display_unlock();


    /* ========================================================
     * INITIALIZE REAL-TIME MICROPHONE INPUT
     * ======================================================== */

    if (microphone_init() == ESP_OK) {
        BaseType_t mic_task_created = xTaskCreatePinnedToCore(
            microphone_task,
            "microphone_vu",
            8192,
            NULL,
            6,               /* short bursts; stays smooth while a cover decodes */
            NULL,
            1                /* CPU 1: keeps CPU 0 for the display */
        );

        ESP_ERROR_CHECK(
            mic_task_created == pdPASS
                ? ESP_OK
                : ESP_ERR_NO_MEM
        );
    } else {
        ESP_LOGW(
            TAG,
            "Real microphone VU disabled; display will remain at zero"
        );
    }



    /* ========================================================
     * START WIFI
     * ======================================================== */

    wifi_init();

    /* The web settings page (http://<display IP>/). */
    web_settings_start();

    /* Restore the player selected before the last power-off. */
    load_saved_player();

    if (bsp_display_lock(1000)) {
        update_player_button_styles_locked();
        bsp_display_unlock();
    }


    /* ========================================================
     * START WIFI STATUS TASK
     * ======================================================== */

    BaseType_t wifi_task_result =
        xTaskCreate(
            wifi_status_task,
            "wifi_status",
            4096,
            NULL,
            5,
            NULL
        );


    if (wifi_task_result != pdPASS) {

        ESP_LOGE(
            TAG,
            "Failed to create Wi-Fi status task"
        );
    }


    /* ========================================================
     * START BACKLIGHT TASK (screensaver dimming)
     * ======================================================== */

    if (xTaskCreate(backlight_task, "backlight", 3072, NULL, 3, NULL) != pdPASS) {
        ESP_LOGW(TAG, "Failed to create backlight task; screen stays at full brightness");
    }


    /* ========================================================
     * START LYRION STATUS TASK
     * ======================================================== */

    BaseType_t lyrion_status_result =
        xTaskCreatePinnedToCore(
            lyrion_status_task,
            "lyrion_status",
            12288,
            NULL,
            5,
            NULL,
            1                /* CPU 1: keeps CPU 0 for the display */
        );

    /* Push updates from Lyrion (falls back to polling if unavailable). */
    if (xTaskCreate(lms_events_task, "lms_events", 6144, NULL, 4, NULL) != pdPASS) {
        ESP_LOGW(TAG, "Push updates unavailable (no memory) - polling instead");
    }


    if (lyrion_status_result != pdPASS) {

        ESP_LOGE(
            TAG,
            "Failed to create Lyrion status task"
        );
    }


    /* ========================================================
     * START LYRION COMMAND TASK
     * ======================================================== */

    BaseType_t lyrion_command_result =
        xTaskCreatePinnedToCore(
            lyrion_command_task,
            "lyrion_command",
            7168,          /* was 6144; a little extra now that it also handles Home Assistant */
            NULL,
            5,
            NULL,
            1                /* CPU 1: keeps CPU 0 for the display */
        );


    if (lyrion_command_result != pdPASS) {

        ESP_LOGE(
            TAG,
            "Failed to create Lyrion command task"
        );
    }


    /* ========================================================
     * START SOURCE BROWSER TASK
     *
     * HTTP requests and cJSON parsing happen outside the LVGL
     * task. The larger stack is intentional for JSON parsing.
     * ======================================================== */

    /* The browser task's 32 KB stack is reserved at build time: by the time
     * this runs, the interface has used most of the internal RAM, and one day
     * there was no longer a free 32 KB block for it ("Failed to create source
     * browser task" - Library and Queue then did nothing). */
    static StackType_t source_browser_stack[32768];
    static StaticTask_t source_browser_tcb;
    BaseType_t source_task_result =
        xTaskCreateStaticPinnedToCore(
            source_browser_task,
            "source_browser",
            sizeof(source_browser_stack),
            NULL,
            5,
            source_browser_stack,
            &source_browser_tcb,
            1                /* CPU 1: keeps CPU 0 for the display */
        ) != NULL ? pdPASS : pdFAIL;

    if (source_task_result != pdPASS) {
        ESP_LOGE(
            TAG,
            "Failed to create source browser task"
        );
    }


    /* ========================================================
     * READY
     * ======================================================== */

    ESP_LOGI(TAG, "P4-LMS interface ready - internal RAM free %u (largest %u), DMA free %u, PSRAM free %u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));


    /* ========================================================
     * KEEP APPLICATION RUNNING
     * ======================================================== */

    while (1) {

        vTaskDelay(
            pdMS_TO_TICKS(1000)
        );
    }
}