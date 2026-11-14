/*
 * P4-LMS - Command queue, refresh events, response buffers, shared UI objects
 *
 * Part of main.c.  This file is #included by main.c (in order) and is not
 * compiled on its own, so it shares main.c's includes and static names.
 */

/* ============================================================
 * LYRION COMMAND QUEUE
 * ============================================================ */

typedef enum {
    LYRION_COMMAND_PLAY = 0,
    LYRION_COMMAND_PAUSE,
    LYRION_COMMAND_PREVIOUS,
    LYRION_COMMAND_NEXT,
    LYRION_COMMAND_VOLUME,
    LYRION_COMMAND_SHUFFLE,     /* value: 0 off, 1 songs, 2 albums */
    LYRION_COMMAND_REPEAT,      /* value: 0 off, 1 song, 2 playlist */
    LYRION_COMMAND_SEEK,        /* value: seconds from the start   */
    LYRION_COMMAND_SLEEP,       /* value: seconds until off, 0 = cancel     */
    LYRION_COMMAND_SYNC,        /* value: index of the room that joins this one */
    LYRION_COMMAND_UNSYNC,      /* this room leaves its group                */
    LYRION_COMMAND_MUTE,        /* value: 1 mute, 0 unmute (Lyrion keeps the volume) */
    LYRION_COMMAND_POWER        /* value: 1 on, 0 off (the player's own power)       */
} lyrion_command_type_t;


typedef struct {
    int player_index;
    lyrion_command_type_t type;
    int volume;
} lyrion_command_t;


/*
 * Commands are queued here and handled by a dedicated task.
 *
 * This prevents HTTP requests from blocking the LVGL event
 * task when a touchscreen button is pressed.
 */
/* A "..." label must also get a one-line height: with only a width set,
 * LVGL still wraps it onto more lines, which then overlap the text below.
 * Call after the label's font is set. */
static void label_one_line(lv_obj_t *l)
{
    lv_obj_set_height(l, lv_font_get_line_height(lv_obj_get_style_text_font(l, LV_PART_MAIN)));
}

/* A plain label: text, font, colour, position; w > 0 also fixes its width
 * and cuts long text off with "..." on one line. */
static lv_obj_t *ui_text(lv_obj_t *parent, const char *text, const lv_font_t *font, uint32_t color,
                         int32_t x, int32_t y, int32_t w)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    if (w > 0) {
        lv_obj_set_width(l, w);
        lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
        label_one_line(l);
    }
    lv_obj_set_pos(l, x, y);
    return l;
}

/* create_button() colours its label directly, which beats any text colour set
 * on the button itself.  For buttons that change text colour with their state
 * (dark on the accent fill, dim when disabled), drop the label's own colour so
 * it follows the button. */
static void button_text_follows_state(lv_obj_t *b)
{
    lv_obj_t *l = lv_obj_get_child(b, 0);
    if (l != NULL) lv_obj_remove_local_style_prop(l, LV_STYLE_TEXT_COLOR, LV_PART_MAIN);
}

static QueueHandle_t lyrion_command_queue = NULL;


/* ============================================================
 * LYRION STATUS REFRESH EVENT
 * ============================================================ */

/* Screen layout: header at the top, navigation bar at the bottom; full
 * panels (Library, Settings, Wi-Fi...) fill the space above the bar. */
#define HEADER_H      56
#define NAV_BAR_H     64
#define PANEL_X       47
#define PANEL_Y       4
#define PANEL_W       930
#define PANEL_H       (600 - NAV_BAR_H - 2 * PANEL_Y)     /* 528 */

/* Favorites: is what's playing on the selected room a favorite?
 * -1 unknown / nothing to favorite, 0 no, 1 yes (index = its favorites id). */
static volatile int np_fav = -1;
static char np_fav_url[512] = "";          /* what's playing (status "url")          */
static char np_fav_title[128] = "";        /* name to save it under                  */
static void ui_add_settings_back(lv_obj_t *panel);   /* BACK to Settings (18b) */
static lv_obj_t *ssp_overlay = NULL;
static lv_obj_t *csp_overlay = NULL;       /* Settings > Screensaver > Clock style (18d) */
static lv_obj_t *vsp_overlay = NULL;       /* Settings > Visualizer panel (18d)      */       /* Settings > Screensaver panel (18d)     */
static lv_obj_t *fav_button = NULL;        /* "+ FAV" / "FAV" on Now Playing         */
static lv_obj_t *np_list_button = NULL;    /* "+ PLAYLIST" beside it: add to a playlist */

/* Sleep timer of the selected room, as last reported by Lyrion. */
static volatile int     sleep_left_s = 0;        /* seconds left when reported   */
static volatile int64_t sleep_left_at_ms = 0;    /* when that was (esp_timer ms) */
static volatile int     sleep_left_player = -1;  /* which room it belongs to     */
/* "Amp off with the sleep timer": the room, and when its timer should end. */
static volatile bool    sleep_amp_enabled = true;
static volatile int     sleep_amp_room = -1;
static volatile int64_t sleep_amp_due_ms = 0;

#define LYRION_REFRESH_BIT BIT0
#define LYRION_HOUSE_BIT   BIT1      /* another player changed: refresh the room dots */

/* Push updates (09b_lyrion_events.c): while connected, polling slows down. */
#define LYRION_POLL_PUSH_MS 10000    /* safety-net poll while push updates work */
static volatile bool lms_events_connected = false;

static EventGroupHandle_t lyrion_refresh_event = NULL;


/* ============================================================
 * LYRION RESPONSE BUFFER
 * ============================================================ */

typedef struct {
    char *data;
    size_t length;
    size_t capacity;   /* size of data[] including the terminating NUL */
    bool truncated;    /* set when the reply did not fit               */
} lyrion_response_t;


/*
 * Global response buffer.
 *
 * This is intentionally not on a task stack.
 */
static char lyrion_response_buffer[
    LYRION_RESPONSE_MAX
];


/* ============================================================
 * LYRION UI OBJECTS
 * ============================================================ */

static lv_obj_t *artist_label = NULL;
static lv_obj_t *song_label = NULL;
static lv_obj_t *album_name_label = NULL;

static lv_obj_t *elapsed_label = NULL;
static lv_obj_t *total_label = NULL;
static lv_obj_t *progress_knob = NULL;      /* round handle on the progress bar */
#define PROGRESS_KNOB_PX 24

static lv_obj_t *progress_bar = NULL;

static lv_obj_t *volume_slider = NULL;
static lv_obj_t *volume_value_label = NULL;

static lv_obj_t *play_button = NULL;

static lv_obj_t *album_image = NULL;
static lv_obj_t *album_placeholder = NULL;

static lv_obj_t *play_state_label = NULL;      /* "PLAYING" / "PAUSED" chip   */
static volatile int playback_album_id = 0;     /* 0 = unknown / not applicable (radio)  */
static lv_obj_t *header_clock_label = NULL;    /* small clock in the header   */
static lv_obj_t *visualizer_track_label = NULL;

/* Local playback clock: the server is polled every couple of seconds, but the
 * elapsed time / progress bar are advanced locally so they move smoothly. */
static double   playback_elapsed_s = 0.0;
static double   playback_duration_s = 0.0;
static uint32_t playback_sync_tick = 0;
static bool     playback_valid = false;

/* Volume slider: remember when the user last touched it so a status poll
 * cannot yank the knob back while it is being dragged. */
static bool     volume_user_touched = false;
static uint32_t volume_last_user_tick = 0;
static uint32_t volume_last_sent_tick = 0;

/* -1 = never applied, 0 = paused look, 1 = playing look. */
static int      ui_play_state_applied = -1;

/* Format badge, shuffle / repeat buttons, whole-house player indicators. */
static lv_obj_t *format_label = NULL;          /* "FLAC | 44.1 kHz | 16-bit"  */
static lv_obj_t *shuffle_button = NULL;
static lv_obj_t *repeat_button = NULL;
static lv_obj_t *player_status_dots[LMS_MAX_PLAYERS];

/* Last known / optimistic values: shuffle 0 off, 1 songs, 2 albums;
 * repeat 0 off, 1 this song, 2 whole playlist. */
static volatile int shuffle_mode = 0;
static volatile int repeat_mode = 0;
static int      ui_shuffle_applied = -1;
static int      ui_repeat_applied = -1;
static bool     mode_user_touched = false;
static uint32_t mode_user_tick = 0;
static int      ui_format_lossless = -1;

/* 0 = off / unreachable (no dot), 1 = on but idle, 2 = playing. */
static volatile int house_state[LMS_MAX_PLAYERS];
static int      ui_house_applied[LMS_MAX_PLAYERS];

/* Tap / drag on the progress bar to seek. */
static volatile bool seek_dragging = false;
static bool     seek_release_pending = false;
static uint32_t seek_release_tick = 0;

/* Consecutive failed status polls; one hiccup should not blank the screen. */
static int      lyrion_fail_count = 0;

static bool     screensaver_active = false;

/* Only touch a label when its text really changed.  Every lv_label_set_text()
 * invalidates the object (wasted redraw + screen traffic when the polled value
 * is unchanged) and would restart a scrolling label's marquee. */
/*
 * The display runs in triple-buffered partial-update mode.  Very occasionally a
 * region that changed is left showing old pixels in one of the three frame
 * buffers (seen as a stray word from the previous screen).  After a screen
 * change, repaint the affected object a few times over the next few frames so
 * every buffer ends up with the correct picture.
 */
static lv_obj_t *repaint_target = NULL;

static void repaint_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    if (repaint_target != NULL) {
        lv_obj_invalidate(repaint_target);
    }
}

/* LVGL lock must be held (or call from an LVGL callback). */

static void ui_schedule_repaint(lv_obj_t *target)
{
    if (target == NULL) {
        return;
    }
    repaint_target = target;
    lv_obj_invalidate(target);

    lv_timer_t *timer = lv_timer_create(repaint_timer_cb, 50, NULL);
    if (timer != NULL) {
        lv_timer_set_repeat_count(timer, 3);   /* deletes itself afterwards */
    }
}

static void ui_set_label(lv_obj_t *label, const char *text)
{
    if (label == NULL || text == NULL) {
        return;
    }

    const char *current = lv_label_get_text(label);
    if (current != NULL && strcmp(current, text) == 0) {
        return;
    }

    lv_label_set_text(label, text);
}

/* ---- Transitions (Settings on the web page can turn them off) ---------------
 * Kept small on purpose: the panel is drawn in software, so short slides and
 * fades of small areas only. */
static volatile bool ui_anim_on = true;
/* Show the ReplayGain Lyrion applies to the song (e.g. "RG -2.34 dB") on the
 * format line, when it applies one (web page setting, off by default). */
static volatile bool ui_show_rg = false;

static void ui_anim_translate_y_cb(void *obj, int32_t v)
{
    lv_obj_set_style_translate_y((lv_obj_t *)obj, v, 0);
}

static void ui_anim_text_opa_cb(void *obj, int32_t v)
{
    lv_obj_set_style_text_opa((lv_obj_t *)obj, (lv_opa_t)v, 0);
}

static void ui_anim_run(lv_obj_t *obj, lv_anim_exec_xcb_t cb, int32_t from, int32_t to, uint32_t ms)
{
    lv_anim_delete(obj, cb);
    if (!ui_anim_on) {
        cb(obj, to);
        return;
    }
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, obj);
    lv_anim_set_exec_cb(&a, cb);
    lv_anim_set_values(&a, from, to);
    lv_anim_set_duration(&a, ms);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    cb(obj, from);
    lv_anim_start(&a);
}

/* ---- How fast the screen draws ------------------------------------------
 * Every frame LVGL renders is timed.  Settings > About shows the figures (and
 * the log, once a minute while something is drawing), and the panel
 * transition below only runs when a full panel draws quickly enough to look
 * smooth - so a slow build never gets a stuttering animation. */
#define UI_PANEL_FAST_US   30000         /* a full panel in under 30 ms: animate it */
static struct {
    int64_t  t_start;                    /* this frame's start (esp_timer us) */
    uint32_t frames, sum_us, max_us;     /* since the last one-second roll-up  */
    uint32_t fps, avg_us, worst_us;      /* the last full second               */
    uint32_t min_frames, min_sum_us, min_max_us;   /* this minute, for the log */
    bool     panel_pending;              /* the next frame draws a newly shown panel */
    uint32_t panel_us;                   /* how long that took last time (0 = not measured) */
} ui_perf;

static void ui_perf_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_RENDER_START) {
        ui_perf.t_start = esp_timer_get_time();
        return;
    }
    const uint32_t us = (uint32_t)(esp_timer_get_time() - ui_perf.t_start);
    ui_perf.frames++;
    ui_perf.sum_us += us;
    if (us > ui_perf.max_us) ui_perf.max_us = us;
    if (ui_perf.panel_pending) {
        ui_perf.panel_pending = false;
        ui_perf.panel_us = us;
    }
}

static void ui_perf_timer_cb(lv_timer_t *t)
{
    (void)t;
    static int seconds = 0;
    ui_perf.fps = ui_perf.frames;
    ui_perf.avg_us = ui_perf.frames ? ui_perf.sum_us / ui_perf.frames : 0;
    ui_perf.worst_us = ui_perf.max_us;
    ui_perf.min_frames += ui_perf.frames;
    ui_perf.min_sum_us += ui_perf.sum_us;
    if (ui_perf.max_us > ui_perf.min_max_us) ui_perf.min_max_us = ui_perf.max_us;
    ui_perf.frames = ui_perf.sum_us = ui_perf.max_us = 0;
    if (++seconds >= 60) {
        seconds = 0;
        if (ui_perf.min_frames > 0) {
            ESP_LOGI(TAG, "Screen: %u frames this minute, %u.%u ms each on average, slowest %u ms; a full panel takes %u ms",
                     (unsigned)ui_perf.min_frames,
                     (unsigned)(ui_perf.min_sum_us / ui_perf.min_frames / 1000),
                     (unsigned)(ui_perf.min_sum_us / ui_perf.min_frames % 1000 / 100),
                     (unsigned)(ui_perf.min_max_us / 1000), (unsigned)(ui_perf.panel_us / 1000));
        }
        ui_perf.min_frames = ui_perf.min_sum_us = ui_perf.min_max_us = 0;
    }
}

/* Call once with the display lock held. */
static void ui_perf_start(lv_display_t *display)
{
    lv_display_add_event_cb(display, ui_perf_event_cb, LV_EVENT_RENDER_START, NULL);
    lv_display_add_event_cb(display, ui_perf_event_cb, LV_EVENT_RENDER_READY, NULL);
    lv_timer_create(ui_perf_timer_cb, 1000, NULL);
}

/* "34 frames/s, 9.4 ms a frame (slowest 21), panel 24 ms" for Settings > About. */
static void ui_perf_text(char *out, size_t len)
{
    snprintf(out, len, "Screen %u frames/s  %u.%u ms each (slowest %u)  panel %u ms",
             (unsigned)ui_perf.fps, (unsigned)(ui_perf.avg_us / 1000), (unsigned)(ui_perf.avg_us % 1000 / 100),
             (unsigned)(ui_perf.worst_us / 1000), (unsigned)(ui_perf.panel_us / 1000));
}

/* Show a panel.  It rises a few pixels into place - but only when the last
 * panel drew fast enough for that to look smooth (see ui_perf above); a full
 * panel that takes long to draw simply appears, as an animation made of three
 * or four frames looks like a stutter. */
static void ui_show_panel(lv_obj_t *p)
{
    if (p == NULL) return;
    const bool was_hidden = lv_obj_has_flag(p, LV_OBJ_FLAG_HIDDEN);
    lv_anim_delete(p, ui_anim_translate_y_cb);
    lv_obj_set_style_translate_y(p, 0, 0);
    lv_obj_clear_flag(p, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(p);
    if (!was_hidden) return;                       /* already up: nothing to animate */
    const bool fast = ui_perf.panel_us > 0 && ui_perf.panel_us < UI_PANEL_FAST_US;
    ui_perf.panel_pending = true;                  /* time the frame that draws it */
    if (ui_anim_on && fast) ui_anim_run(p, ui_anim_translate_y_cb, 22, 0, 190);
}

/* New text fades in. */
static void ui_fade_in_text(lv_obj_t *label)
{
    if (label != NULL) ui_anim_run(label, ui_anim_text_opa_cb, LV_OPA_TRANSP, LV_OPA_COVER, 350);
}

/* Decoded album artwork. 370x370 RGB565 is about 274 KB. */
static uint8_t *album_pixels = NULL;
static lv_image_dsc_t album_image_dsc;
static char last_album_art_key[256] = {0};


