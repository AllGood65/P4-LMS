/*
 * P4-LMS - Forward declarations, PNG decoding, microphone task, screensaver
 *
 * Part of main.c.  This file is #included by main.c (in order) and is not
 * compiled on its own, so it shares main.c's includes and static names.
 */

/* ============================================================
 * FORWARD DECLARATIONS
 * ============================================================ */

static void lyrion_status_task(void *arg);

static void lyrion_command_task(void *arg);
static void np_power_room_changed(void);      /* Now Playing power button, 11_transport_controls.c */
static void np_power_refresh_soon(void);
static volatile bool wifi_have_ip = false;    /* set once the display has an address */
static void source_browser_task(void *arg);
static void lvgl_mem_log(const char *when);   /* defined later, near SETTINGS */
static bool source_http_query(int player_index, const char *command_json,
                              char *response_buffer, size_t response_size);
static lv_obj_t *create_button(
    lv_obj_t *parent,
    const char *text,
    int32_t width,
    int32_t height
);

static lv_obj_t *create_source_item_button(
    lv_obj_t *parent,
    const char *text,
    int32_t width,
    int32_t height,
    bool is_folder,
    bool is_song
);


/* Decode a PNG into a freshly allocated RGB565 buffer (row stride padded to a
 * multiple of 16 pixels).  The caller owns and must free *out_pixels. */
static bool png_decode_rgb565_buffer(const uint8_t *png_data, size_t png_size,
                                     uint8_t **out_pixels, uint32_t *out_w, uint32_t *out_h,
                                     uint32_t *out_padded_w, size_t *out_size)
{
    if (png_data == NULL || png_size < 8) {
        return false;
    }

    png_image image;
    memset(&image, 0, sizeof(image));
    image.version = PNG_IMAGE_VERSION;

    if (!png_image_begin_read_from_memory(&image, png_data, png_size)) {
        ESP_LOGW(TAG, "PNG begin decode failed: %s", image.message);
        return false;
    }

    ESP_LOGI(TAG, "PNG reported dimensions: %ux%u",
             (unsigned)image.width, (unsigned)image.height);

    if (image.width == 0 || image.height == 0 ||
        image.width > 1024 || image.height > 1024) {
        ESP_LOGW(TAG, "Rejecting invalid PNG dimensions: %ux%u",
                 (unsigned)image.width, (unsigned)image.height);
        png_image_free(&image);
        return false;
    }

    /* RGBA, not RGB: asking libpng for RGB makes it composite any
     * transparency itself, in linear light, pixel by pixel - that took up to
     * 8 s for one cover.  With RGBA it only unpacks; the (rare) transparent
     * pixels are blended onto black below. */
    image.format = PNG_FORMAT_RGBA;

    size_t rgb_size = (size_t)image.width * (size_t)image.height * 4U;
    uint8_t *rgb = (uint8_t *)heap_caps_malloc(
        rgb_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (rgb == NULL) {
        rgb = (uint8_t *)heap_caps_malloc(rgb_size, MALLOC_CAP_8BIT);
    }

    if (rgb == NULL) {
        ESP_LOGW(TAG, "Could not allocate PNG RGB buffer (%u bytes)",
                 (unsigned)rgb_size);
        png_image_free(&image);
        return false;
    }

    if (!png_image_finish_read(&image, NULL, rgb, 0, NULL)) {
        ESP_LOGW(TAG, "PNG decode failed: %s", image.message);
        heap_caps_free(rgb);
        png_image_free(&image);
        return false;
    }

    const uint32_t width = image.width;
    const uint32_t height = image.height;
    const uint32_t padded_width = (width + 15U) & ~15U;
    const size_t rgb565_size = (size_t)padded_width * (size_t)height * 2U;

    uint8_t *new_pixels = (uint8_t *)heap_caps_malloc(
        rgb565_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (new_pixels == NULL) {
        new_pixels = (uint8_t *)heap_caps_malloc(rgb565_size, MALLOC_CAP_8BIT);
    }

    if (new_pixels == NULL) {
        ESP_LOGW(TAG, "Could not allocate PNG RGB565 buffer (%u bytes)",
                 (unsigned)rgb565_size);
        heap_caps_free(rgb);
        png_image_free(&image);
        return false;
    }

    memset(new_pixels, 0, rgb565_size);

    for (uint32_t y = 0; y < height; y++) {
        const uint8_t *src = rgb + (size_t)y * width * 4U;
        uint16_t *dst = (uint16_t *)(new_pixels + (size_t)y * padded_width * 2U);

        for (uint32_t x = 0; x < width; x++) {
            uint8_t r = src[x * 4U + 0U];
            uint8_t g = src[x * 4U + 1U];
            uint8_t b = src[x * 4U + 2U];
            const uint8_t a = src[x * 4U + 3U];
            if (a != 255U) {                      /* onto black */
                r = (uint8_t)((r * a) / 255U);
                g = (uint8_t)((g * a) / 255U);
                b = (uint8_t)((b * a) / 255U);
            }
            uint16_t pixel = (uint16_t)(((uint16_t)(r & 0xF8U) << 8) |
                                        ((uint16_t)(g & 0xFCU) << 3) |
                                        ((uint16_t)b >> 3));
            dst[x] = pixel;
        }
    }

    heap_caps_free(rgb);
    png_image_free(&image);

    *out_pixels = new_pixels;
    *out_w = width;
    *out_h = height;
    *out_padded_w = padded_width;
    *out_size = rgb565_size;
    return true;
}

/* Hand a freshly decoded cover to the LVGL task.  On success the LVGL task
 * owns `pixels` (and frees the previous cover); on failure the caller keeps
 * them and must free them. */
typedef struct { uint8_t *pixels; uint32_t w, h, padded; size_t size; } album_art_msg_t;
static void screensaver_cover_changed(bool present);
/* The analog clock and the cover wall live in 18e_screensavers.c. */
static void ss_extra_create(lv_obj_t *overlay);
static void ss_extra_show(int view);          /* shows that view's box (or none) */
static void ss_extra_tick(void);              /* once a second while showing     */
static void ss_extra_shift(int index);        /* slow drift                      */
static void format_time_value(double seconds, char *buffer, size_t buffer_size);   /* part 08 */

static void album_art_show_job(void *arg)
{
    const album_art_msg_t *m = (const album_art_msg_t *)arg;
    if (album_pixels != NULL) {
        heap_caps_free(album_pixels);
    }
    album_pixels = m->pixels;

    memset(&album_image_dsc, 0, sizeof(album_image_dsc));
    album_image_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
    album_image_dsc.header.w = m->w;
    album_image_dsc.header.h = m->h;
    album_image_dsc.header.stride = m->padded * 2U;
    album_image_dsc.data_size = m->size;
    album_image_dsc.data = album_pixels;

    if (album_image != NULL) {
        lv_image_set_src(album_image, &album_image_dsc);
        lv_obj_clear_flag(album_image, LV_OBJ_FLAG_HIDDEN);
    }
    if (album_placeholder != NULL) {
        lv_obj_add_flag(album_placeholder, LV_OBJ_FLAG_HIDDEN);
    }
    screensaver_cover_changed(true);
}

static bool album_art_show(uint8_t *pixels, uint32_t w, uint32_t h, uint32_t padded, size_t size)
{
    const album_art_msg_t m = { .pixels = pixels, .w = w, .h = h, .padded = padded, .size = size };
    return ui_post(album_art_show_job, &m, sizeof(m));
}

static bool decode_png_to_rgb565(const uint8_t *png_data, size_t png_size)
{
    uint8_t *new_pixels = NULL;
    uint32_t width = 0, height = 0, padded_width = 0;
    size_t rgb565_size = 0;
    if (!png_decode_rgb565_buffer(png_data, png_size, &new_pixels,
                                  &width, &height, &padded_width, &rgb565_size)) {
        return false;
    }

    if (!album_art_show(new_pixels, width, height, padded_width, rgb565_size)) {
        heap_caps_free(new_pixels);
        return false;
    }

    ESP_LOGI(TAG, "PNG album art loaded: %ux%u (%u bytes RGB565)",
             (unsigned)width, (unsigned)height, (unsigned)rgb565_size);
    return true;
}

static bool update_album_art(const char *player_id);
static void clear_album_art(void);

static esp_err_t microphone_init(void);
static void microphone_task(void *arg);
static void vu_meter_timer_cb(lv_timer_t *timer);

/* Time zones offered on the web settings page (POSIX rules, daylight saving
 * included).  P4_DEFAULT_TZ (02_config.c) is used until one is chosen. */
typedef struct { const char *name; const char *posix; } tz_choice_t;
static const tz_choice_t tz_choices[] = {
    { "Newfoundland (St. John's)",          "NST3:30NDT,M3.2.0,M11.1.0" },
    { "Atlantic (Halifax)",                 "AST4ADT,M3.2.0,M11.1.0" },
    { "Eastern (Toronto, New York)",        "EST5EDT,M3.2.0,M11.1.0" },
    { "Central (Winnipeg, Chicago)",        "CST6CDT,M3.2.0,M11.1.0" },
    { "Mountain (Calgary, Denver)",         "MST7MDT,M3.2.0,M11.1.0" },
    { "Arizona (no DST, UTC-7)",            "MST7" },
    { "Saskatchewan (no DST, UTC-6)",       "CST6" },
    { "Pacific (Vancouver, Los Angeles)",   "PST8PDT,M3.2.0,M11.1.0" },
    { "Alaska",                             "AKST9AKDT,M3.2.0,M11.1.0" },
    { "Hawaii",                             "HST10" },
    { "UK / Ireland / Portugal",            "GMT0BST,M3.5.0/1,M10.5.0" },
    { "Central Europe (Paris, Berlin)",     "CET-1CEST,M3.5.0,M10.5.0/3" },
    { "Eastern Europe (Athens, Helsinki)",  "EET-2EEST,M3.5.0/3,M10.5.0/4" },
    { "India",                              "IST-5:30" },
    { "China / Singapore / Perth",          "CST-8" },
    { "Japan / Korea",                      "JST-9" },
    { "Australia East (Sydney, Melbourne)", "AEST-10AEDT,M10.1.0,M4.1.0/3" },
    { "Queensland (Brisbane)",              "AEST-10" },
    { "New Zealand",                        "NZST-12NZDT,M9.5.0,M4.1.0/3" },
    { "UTC",                                "UTC0" },
};
#define TZ_CHOICES ((int)(sizeof(tz_choices) / sizeof(tz_choices[0])))
static int tz_index = P4_DEFAULT_TZ;

static void time_apply_timezone(void)
{
    if (tz_index < 0 || tz_index >= TZ_CHOICES) tz_index = P4_DEFAULT_TZ;
    setenv("TZ", tz_choices[tz_index].posix, 1);
    tzset();
}

/* Set at boot so the clocks are right as soon as SNTP delivers the time
 * (the saved choice is applied again once settings are read). */
static void time_init_timezone(void)
{
    time_apply_timezone();
}

/* Before SNTP has synced, the RTC still reports 1970. */
static bool time_is_valid(const struct tm *t)
{
    return t != NULL && t->tm_year >= (2024 - 1900);
}

static void screensaver_update_clock(void)
{
    if (screensaver_clock_label == NULL ||
        screensaver_date_label == NULL) {
        return;
    }

    time_t now = time(NULL);
    struct tm local_time;

    if (localtime_r(&now, &local_time) == NULL) {
        return;
    }

    if (!time_is_valid(&local_time)) {
        ui_set_label(screensaver_clock_label, "--:-- --");
        ui_set_label(screensaver_date_label, "SYNCING TIME");
        if (ss_art_clock != NULL) ui_set_label(ss_art_clock, "");
        return;
    }

    /* Clean 12-hour clock with AM/PM. */
    char clock_text[32];
    char date_text[32];

    strftime(clock_text, sizeof(clock_text), "%I:%M %p", &local_time);
    strftime(date_text, sizeof(date_text), "%a  %b %d  %Y", &local_time);

    /* Keep a fixed 8-character clock width so the clock never changes
     * horizontal geometry when going from 9:xx to 10:xx.  A space replaces
     * the leading zero for the receiver-style look. */
    if (clock_text[0] == '0') {
        clock_text[0] = ' ';
    }

    ui_set_label(screensaver_clock_label, clock_text);
    ui_set_label(screensaver_date_label, date_text);
    if (ss_art_clock != NULL) {
        ui_set_label(ss_art_clock, clock_text[0] == ' ' ? clock_text + 1 : clock_text);
    }
}


/* Small clock in the main header (blank until the time is known). */
static void header_clock_timer_cb(lv_timer_t *timer)
{
    (void)timer;

    if (header_clock_label == NULL || visualizer_visible || screensaver_active) {
        return;
    }

    time_t now = time(NULL);
    struct tm local_time;
    char text[16];

    if (localtime_r(&now, &local_time) != NULL &&
        time_is_valid(&local_time)) {
        strftime(text, sizeof(text), "%I:%M %p", &local_time);
        ui_set_label(header_clock_label, text[0] == '0' ? text + 1 : text);
    } else {
        ui_set_label(header_clock_label, "");
    }
}


/* ---------------------------------------------------------------- colours */
static uint32_t ss_scale_rgb(uint32_t rgb, int pct)
{
    const uint32_t r = ((rgb >> 16) & 0xFF) * (uint32_t)pct / 100U;
    const uint32_t g = ((rgb >> 8) & 0xFF) * (uint32_t)pct / 100U;
    const uint32_t b = (rgb & 0xFF) * (uint32_t)pct / 100U;
    return (r << 16) | (g << 8) | b;
}

/* Background and progress-track colours follow the cover. */
static void ss_apply_colours(void)
{
    if (screensaver_overlay == NULL) return;
    const bool art = (screensaver_view == SS_VIEW_ART);
    const uint32_t base = ss_have_cover ? ss_art_avg : 0x303030;
    lv_obj_set_style_bg_color(screensaver_overlay,
                              lv_color_hex(art ? ss_scale_rgb(base, 22) : 0x000000), 0);
    if (ss_art_bar != NULL) {
        lv_obj_set_style_bg_color(ss_art_bar, lv_color_hex(ss_scale_rgb(base, 45)), LV_PART_MAIN);
    }
}

/* Called on the LVGL task whenever the Now Playing cover changes (or is
 * cleared).  album_image_dsc is reused for every cover, so the screensaver
 * images must be pointed at it again. */
static void screensaver_cover_changed(bool present)
{
    ss_have_cover = present && album_image_dsc.data != NULL;
    if (ss_have_cover) {
        /* Average colour, from a sparse grid of pixels. */
        const uint32_t w = album_image_dsc.header.w, h = album_image_dsc.header.h;
        const uint32_t stride = album_image_dsc.header.stride / 2U;
        const uint16_t *px = (const uint16_t *)album_image_dsc.data;
        uint32_t r = 0, g = 0, b = 0, n = 0;
        for (uint32_t y = 4; y < h; y += 12) {
            for (uint32_t x = 4; x < w; x += 12) {
                const uint16_t v = px[y * stride + x];
                r += (uint32_t)((v >> 11) & 0x1F) << 3;
                g += (uint32_t)((v >> 5) & 0x3F) << 2;
                b += (uint32_t)(v & 0x1F) << 3;
                n++;
            }
        }
        if (n > 0) ss_art_avg = ((r / n) << 16) | ((g / n) << 8) | (b / n);
    }
    lv_obj_t *imgs[2] = { ss_art_img, ss_pause_img };
    for (int i = 0; i < 2; i++) {
        if (imgs[i] == NULL) continue;
        lv_image_set_src(imgs[i], ss_have_cover ? (const void *)&album_image_dsc : NULL);
        if (ss_have_cover) lv_obj_clear_flag(imgs[i], LV_OBJ_FLAG_HIDDEN);
        else               lv_obj_add_flag(imgs[i], LV_OBJ_FLAG_HIDDEN);
    }
    ss_apply_colours();
}

/* "Office  ·  FLAC | 44.1 kHz" under the art-view clock. */
static void ss_update_info(void)
{
    if (ss_art_info == NULL) return;
    char t[160];
    const char *room = (current_player_index >= 0 && current_player_index < lyrion_player_count)
                       ? lyrion_players[current_player_index].short_name : "";
    const char *fmt = format_label != NULL ? lv_label_get_text(format_label) : "";
    if (fmt != NULL && fmt[0]) snprintf(t, sizeof(t), "%s  " LV_SYMBOL_BULLET "  %s", room, fmt);
    else                       snprintf(t, sizeof(t), "%s", room);
    ui_set_label(ss_art_info, t);
}

/* Song text for both views; called with every status update. */
static void screensaver_set_track(const char *title, const char *artist,
                                  const char *album, const char *mode)
{
    ss_have_track = (title != NULL && title[0] != '\0');
    snprintf(ss_track_mode, sizeof(ss_track_mode), "%s", mode ? mode : "");
    if (ss_art_title == NULL) return;
    /* Title: wrap to two lines at most, with "..." when even that is too long. */
    const char *tt = ss_have_track ? title : "";
    if (strcmp(lv_label_get_text(ss_art_title), tt) != 0) {
        const lv_font_t *font = &p4_font_28;
        lv_point_t sz;
        lv_text_get_size(&sz, tt, font, 0, 0, 440, LV_TEXT_FLAG_NONE);
        const int32_t line_h = lv_font_get_line_height(font);
        if (sz.y > 2 * line_h) {
            lv_label_set_long_mode(ss_art_title, LV_LABEL_LONG_DOT);
            lv_obj_set_height(ss_art_title, 2 * line_h + 4);
        } else {
            lv_label_set_long_mode(ss_art_title, LV_LABEL_LONG_WRAP);
            lv_obj_set_height(ss_art_title, LV_SIZE_CONTENT);
        }
        lv_label_set_text(ss_art_title, tt);
    }
    ui_set_label(ss_art_artist, artist ? artist : "");
    ui_set_label(ss_art_album, album ? album : "");
    ui_set_label(ss_pause_title, ss_have_track ? title : "");
    ui_set_label(ss_pause_artist, artist ? artist : "");
    char t[64];
    const char *room = (current_player_index >= 0 && current_player_index < lyrion_player_count)
                       ? lyrion_players[current_player_index].short_name : "";
    snprintf(t, sizeof(t), "%s  " LV_SYMBOL_BULLET "  %s",
             strcmp(ss_track_mode, "stop") == 0 ? "Stopped" : "Paused", room);
    ui_set_label(ss_pause_state, t);
    if (screensaver_active) ss_update_info();
}

/* Progress bar and times in the art view (once a second). */
static void ss_update_progress(void)
{
    if (ss_art_bar == NULL || screensaver_view != SS_VIEW_ART) return;
    double elapsed = playback_elapsed_s;
    if (current_player_playing && playback_valid) {
        elapsed += (double)lv_tick_elaps(playback_sync_tick) / 1000.0;
    }
    const double duration = playback_duration_s;
    if (duration > 0.0 && elapsed > duration) elapsed = duration;
    char a[16], b[16];
    format_time_value(elapsed, a, sizeof(a));
    ui_set_label(ss_art_elapsed, a);
    if (duration > 0.0) {
        format_time_value(duration, b, sizeof(b));
        ui_set_label(ss_art_total, b);
        lv_bar_set_value(ss_art_bar, (int32_t)(elapsed * 1000.0 / duration), LV_ANIM_OFF);
        lv_obj_clear_flag(ss_art_bar, LV_OBJ_FLAG_HIDDEN);
    } else {                                                 /* radio: no length */
        ui_set_label(ss_art_total, "");
        lv_obj_add_flag(ss_art_bar, LV_OBJ_FLAG_HIDDEN);
    }
}


static void screensaver_apply_shift(void)
{
    if (screensaver_overlay == NULL) {
        return;
    }

    if (screensaver_view >= SS_VIEW_ANALOG) {
        ss_extra_shift(screensaver_shift_index);
        return;
    }
    if (screensaver_view == SS_VIEW_ART) {
        const lv_point_t *d = &ss_art_drift[screensaver_shift_index % SS_ART_DRIFT_COUNT];
        if (ss_art_box != NULL) lv_obj_set_pos(ss_art_box, d->x, d->y);
        return;
    }

    /* Clock views: a one-pixel nudge, or half the art drift when the paused
     * cover is showing (a picture needs to move further than thin text). */
    int32_t dx, dy;
    if (screensaver_view == SS_VIEW_CLOCK_COVER) {
        const lv_point_t *d = &ss_art_drift[screensaver_shift_index % SS_ART_DRIFT_COUNT];
        dx = d->x / 2;
        dy = d->y / 2;
    } else {
        const lv_point_t *d = &screensaver_shifts[screensaver_shift_index % SCREENSAVER_SHIFT_COUNT];
        dx = d->x;
        dy = d->y;
    }
    if (ss_clock_box != NULL) lv_obj_set_pos(ss_clock_box, dx, dy);
}

static int ss_brightness_for_view(void)
{
    if (screensaver_deep) return BACKLIGHT_DEEP_PCT;
    /* the "while music plays" looks use the album art brightness */
    const int v = screensaver_view;
    return (v == SS_VIEW_ART || v == SS_VIEW_WALL || v == SS_VIEW_BIGTEXT || v == SS_VIEW_VU)
           ? bl_art_pct : BACKLIGHT_STANDBY_PCT;
}

/* Switch what the overlay shows (it may already be visible). */
static void screensaver_set_view(int view)
{
    screensaver_view = view;
    const bool art = (view == SS_VIEW_ART);
    const bool cover = (view == SS_VIEW_CLOCK_COVER);

    ss_extra_show(view);
    if (view >= SS_VIEW_ANALOG) {
        lv_obj_add_flag(ss_clock_box, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(ss_art_box, LV_OBJ_FLAG_HIDDEN);
    } else if (art) {
        lv_obj_add_flag(ss_clock_box, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(ss_art_box, LV_OBJ_FLAG_HIDDEN);
        ss_update_info();
        ss_update_progress();
    } else {
        lv_obj_add_flag(ss_art_box, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(ss_clock_box, LV_OBJ_FLAG_HIDDEN);
        /* Clock sits higher when the paused song is shown below it. */
        lv_obj_set_pos(screensaver_clock_label, 0, cover ? 150 : 234);
        lv_obj_set_pos(screensaver_date_label, 0, cover ? 232 : 321);
        if (cover) lv_obj_clear_flag(ss_pause_box, LV_OBJ_FLAG_HIDDEN);
        else       lv_obj_add_flag(ss_pause_box, LV_OBJ_FLAG_HIDDEN);
    }
    ss_apply_colours();
    screensaver_apply_shift();
    if (screensaver_active) backlight_target_pct = ss_brightness_for_view();
}

/* The view chosen for "paused or stopped". */
static int screensaver_paused_view(void)
{
    switch (ss_pause_mode) {
        case 1:  return ss_have_track ? SS_VIEW_CLOCK_COVER : SS_VIEW_CLOCK;
        case 2:  return SS_VIEW_ANALOG;
        case 3:  return SS_VIEW_WALL;
        case 4:  return SS_VIEW_WEATHER;
        default: return SS_VIEW_CLOCK;
    }
}

/* Which view idle time should bring up now; -1 = none (stay on Now Playing). */
static int screensaver_idle_view(void)
{
    if (current_player_playing) {
        static const int play_views[5] = { -1, SS_VIEW_ART, SS_VIEW_WALL, SS_VIEW_BIGTEXT, SS_VIEW_VU };
        return (ss_play_mode >= 0 && ss_play_mode < 5) ? play_views[ss_play_mode] : -1;
    }
    return screensaver_paused_view();
}

static void screensaver_show(int view)
{
    if (screensaver_overlay == NULL || screensaver_active) {
        return;
    }

    screensaver_active = true;
    screensaver_deep = false;
    screensaver_shift_index = 0;
    screensaver_shown_ms = esp_log_timestamp();
    screensaver_last_shift_ms = screensaver_shown_ms;

    screensaver_update_clock();
    screensaver_set_view(view);

    lv_obj_clear_flag(screensaver_overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(screensaver_overlay);

    backlight_target_pct = ss_brightness_for_view();

    static const char *const vn[8] = { "clock", "clock + cover", "album art", "analog clock", "cover wall",
                                       "big text", "VU meters", "weather" };
    ESP_LOGI(TAG, "Screensaver active - %s", (view >= 0 && view < 8) ? vn[view] : "?");
}

/* Settings > Screensaver > PREVIEW. */
/* PREVIEW from Settings > Screensaver: the look last tapped there (else the
 * one that would come up now); a touch goes back to that settings page. */
static int  ss_preview_view = -1;
static bool screensaver_back_to_settings = false;
static void ssp_show(void);                    /* 18d_screensaver_panel.c */

static void screensaver_preview_show(void)
{
    int view = ss_preview_view;
    if (view < 0) view = screensaver_idle_view();
    if (view < 0) view = screensaver_paused_view();
    screensaver_show(view);
    screensaver_preview = true;
    screensaver_back_to_settings = true;
    /* Bright enough to judge it (the real standby level can be very dim). */
    if (backlight_target_pct < 50) backlight_target_pct = 50;
}


static void screensaver_hide(void)
{
    if (screensaver_overlay == NULL || !screensaver_active) {
        return;
    }

    screensaver_active = false;
    screensaver_preview = false;

    lv_obj_add_flag(screensaver_overlay, LV_OBJ_FLAG_HIDDEN);

    /* Full brightness the instant the user touches or music starts. */
    backlight_target_pct = BACKLIGHT_FULL_PCT;

    screensaver_last_state_change_ms = esp_log_timestamp();

    ESP_LOGI(TAG, "Screensaver dismissed");

    if (screensaver_back_to_settings) {         /* after a PREVIEW */
        screensaver_back_to_settings = false;
        ssp_show();
    }
}


static void screensaver_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);

    if (code == LV_EVENT_PRESSED || code == LV_EVENT_CLICKED) {
        screensaver_hide();
    }
}


static void screensaver_timer_cb(lv_timer_t *timer)
{
    (void)timer;

    if (screensaver_overlay == NULL) {
        return;
    }

    uint32_t now = esp_log_timestamp();

    if (!screensaver_active) {
        const int want = screensaver_idle_view();
        if (want < 0) {
            return;                         /* playing, and set to stay on Now Playing */
        }
        if (want == SS_VIEW_ART && visualizer_visible) {
            return;                         /* the visualizer was opened on purpose */
        }
        /* Idle = time since the last play/pause change AND since the last touch,
         * whichever is more recent.  Without the touch part, browsing SOURCE
         * (or using any control) for a few minutes after pausing let the
         * screensaver pop up while the screen was in use. */
        uint32_t idle_ms = now - screensaver_last_state_change_ms;
        const uint32_t touch_idle_ms = lv_display_get_inactive_time(NULL);
        if (touch_idle_ms < idle_ms) {
            idle_ms = touch_idle_ms;
        }
        if (idle_ms >= SCREENSAVER_TIMEOUT_MS) {
            screensaver_show(want);
        }
        return;
    }

    /* Follow play / pause while showing: art while playing, clock when not. */
    if (!screensaver_preview) {
        const int want = screensaver_idle_view();
        if (want < 0) {
            screensaver_hide();
            return;
        }
        if (want != screensaver_view) {
            screensaver_set_view(want);
        }
    }

    /* Keep the clock current even if NTP sync occurs after standby begins. */
    screensaver_update_clock();
    ss_update_progress();
    ss_extra_tick();

    /* After a long idle period, drop to the lowest backlight level. */
    if (!screensaver_deep && (now - screensaver_shown_ms) >= SCREENSAVER_DEEP_DIM_MS) {
        screensaver_deep = true;
        backlight_target_pct = BACKLIGHT_DEEP_PCT;
    }

    /* Nudge the layout now and then. */
    if ((now - screensaver_last_shift_ms) >= SCREENSAVER_SHIFT_MS) {
        screensaver_last_shift_ms = now;
        screensaver_shift_index =
            (screensaver_shift_index + 1) % 8U;
        screensaver_apply_shift();
    }
}


static void screensaver_start_ntp(void)
{
    if (ntp_started) {
        return;
    }

    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_init();

    ntp_started = true;

    ESP_LOGI(TAG, "NTP time synchronization started");
}


static lv_obj_t *ss_label(lv_obj_t *parent, const lv_font_t *font, uint32_t color,
                          int32_t x, int32_t y, int32_t w)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, "");
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_obj_set_width(l, w);
    lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
    label_one_line(l);
    lv_obj_set_pos(l, x, y);
    return l;
}

static lv_obj_t *ss_box(lv_obj_t *parent, int32_t w, int32_t h)
{
    lv_obj_t *b = lv_obj_create(parent);
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, w, h);
    lv_obj_clear_flag(b, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(b, LV_OBJ_FLAG_CLICKABLE);     /* touches go to the overlay */
    return b;
}

static void create_screensaver(lv_obj_t *screen)
{
    screensaver_overlay = lv_obj_create(screen);
    lv_obj_remove_style_all(screensaver_overlay);
    lv_obj_set_size(screensaver_overlay, 1024, 600);
    lv_obj_set_pos(screensaver_overlay, 0, 0);
    lv_obj_set_style_bg_color(screensaver_overlay, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(screensaver_overlay, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_clear_flag(screensaver_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(screensaver_overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(screensaver_overlay, screensaver_event_cb, LV_EVENT_PRESSED, NULL);

    /* ---- clock views ---- */
    ss_clock_box = ss_box(screensaver_overlay, 1024, 600);

    /* Clock: native 48 px font, full-screen width, centred text.  Warm amber
     * so the standby screen stays easy on the eyes at night. */
    screensaver_clock_label = lv_label_create(ss_clock_box);
    lv_label_set_text(screensaver_clock_label, "--:-- --");
    lv_obj_set_style_text_font(screensaver_clock_label, &p4_font_48, 0);
    lv_obj_set_style_text_color(screensaver_clock_label, lv_color_hex(UI_ACCENT_GLOW), 0);
    lv_obj_set_style_text_align(screensaver_clock_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_letter_space(screensaver_clock_label, 3, 0);
    lv_obj_set_width(screensaver_clock_label, 1024);

    /* Date: native 40 px font, dimmer than the clock. */
    screensaver_date_label = lv_label_create(ss_clock_box);
    lv_label_set_text(screensaver_date_label, "SYNCING TIME");
    lv_obj_set_style_text_font(screensaver_date_label, &p4_font_40, 0);
    lv_obj_set_style_text_color(screensaver_date_label, lv_color_hex(UI_DATE), 0);
    lv_obj_set_style_text_align(screensaver_date_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_letter_space(screensaver_date_label, 2, 0);
    lv_obj_set_width(screensaver_date_label, 1024);

    /* The paused song: small dimmed cover and two lines of text. */
    ss_pause_box = ss_box(ss_clock_box, 540, 100);
    lv_obj_set_pos(ss_pause_box, (1024 - 540) / 2, 350);
    ss_pause_img = lv_image_create(ss_pause_box);
    lv_obj_set_size(ss_pause_img, 96, 96);
    lv_image_set_inner_align(ss_pause_img, LV_IMAGE_ALIGN_STRETCH);
    lv_obj_set_style_image_recolor(ss_pause_img, lv_color_hex(0x000000), 0);
    lv_obj_set_style_image_recolor_opa(ss_pause_img, LV_OPA_40, 0);
    lv_obj_set_pos(ss_pause_img, 0, 2);
    lv_obj_add_flag(ss_pause_img, LV_OBJ_FLAG_HIDDEN);
    ss_pause_state  = ss_label(ss_pause_box, &p4_font_14, UI_TEXT_DIM, 116, 12, 420);
    ss_pause_title  = ss_label(ss_pause_box, &p4_font_18, UI_TEXT_SOFT, 116, 36, 420);
    ss_pause_artist = ss_label(ss_pause_box, &p4_font_16, UI_TEXT_DIM, 116, 64, 420);
    lv_obj_add_flag(ss_pause_box, LV_OBJ_FLAG_HIDDEN);

    /* ---- album art view ---- */
    ss_art_box = ss_box(screensaver_overlay, 1024, 600);
    ss_art_img = lv_image_create(ss_art_box);
    lv_obj_set_size(ss_art_img, 440, 440);
    lv_image_set_inner_align(ss_art_img, LV_IMAGE_ALIGN_STRETCH);
    lv_obj_set_pos(ss_art_img, 56, 80);
    lv_obj_add_flag(ss_art_img, LV_OBJ_FLAG_HIDDEN);

    /* Title (up to two lines), artist and album stacked, ending above the bar. */
    lv_obj_t *col = ss_box(ss_art_box, 440, 170);
    lv_obj_set_pos(col, 532, 110);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(col, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(col, 6, 0);
    ss_art_title = ss_label(col, &p4_font_28, UI_TEXT, 0, 0, 440);
    lv_label_set_long_mode(ss_art_title, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_pad_bottom(ss_art_title, 4, 0);
    ss_art_artist = ss_label(col, &p4_font_18, UI_TEXT_SOFT, 0, 0, 440);
    ss_art_album  = ss_label(col, &p4_font_16, UI_TEXT_DIM, 0, 0, 440);

    ss_art_bar = lv_bar_create(ss_art_box);
    lv_obj_remove_style_all(ss_art_bar);
    lv_obj_set_size(ss_art_bar, 420, 6);
    lv_obj_set_pos(ss_art_bar, 532, 296);
    lv_bar_set_range(ss_art_bar, 0, 1000);
    lv_obj_set_style_bg_opa(ss_art_bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(ss_art_bar, 3, LV_PART_MAIN);
    lv_obj_set_style_bg_color(ss_art_bar, lv_color_hex(UI_ACCENT), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(ss_art_bar, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(ss_art_bar, 3, LV_PART_INDICATOR);
    lv_obj_clear_flag(ss_art_bar, LV_OBJ_FLAG_CLICKABLE);
    ss_art_elapsed = ss_label(ss_art_box, &p4_font_14, UI_TEXT_DIM, 532, 312, 120);
    ss_art_total   = ss_label(ss_art_box, &p4_font_14, UI_TEXT_DIM, 832, 312, 120);
    lv_obj_set_style_text_align(ss_art_total, LV_TEXT_ALIGN_RIGHT, 0);

    ss_art_clock = ss_label(ss_art_box, &p4_font_40, UI_ACCENT_GLOW, 532, 390, 440);
    ss_art_info  = ss_label(ss_art_box, &p4_font_14, UI_TEXT_DIM, 532, 446, 440);
    lv_obj_add_flag(ss_art_box, LV_OBJ_FLAG_HIDDEN);

    ss_extra_create(screensaver_overlay);

    /* Start hidden.  The timer decides when standby is needed. */
    lv_obj_add_flag(screensaver_overlay, LV_OBJ_FLAG_HIDDEN);

    screensaver_last_state_change_ms = esp_log_timestamp();
    screensaver_last_shift_ms = screensaver_last_state_change_ms;

    screensaver_update_clock();
    screensaver_set_view(SS_VIEW_CLOCK);

    screensaver_timer = lv_timer_create(screensaver_timer_cb, 1000, NULL);

    ESP_LOGI(TAG,
             "Screensaver enabled: %lu min timeout, %lu min pixel shift, %lu min deep dim",
             (unsigned long)(SCREENSAVER_TIMEOUT_MS / 60000UL),
             (unsigned long)(SCREENSAVER_SHIFT_MS / 60000UL),
             (unsigned long)(SCREENSAVER_DEEP_DIM_MS / 60000UL));
}
