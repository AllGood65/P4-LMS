/*
 * P4-LMS - Screensaver / standby settings and state
 *
 * Part of main.c.  This file is #included by main.c (in order) and is not
 * compiled on its own, so it shares main.c's includes and static names.
 */

/* ============================================================
 * DISPLAY SCREENSAVER / STANDBY
 * ============================================================ */

/* Changeable on the web settings page; these are the defaults. */
static volatile uint32_t ss_timeout_min = 5;     /* idle before standby    */
static volatile uint32_t ss_deep_min    = 30;    /* standby -> deep dim    */
static volatile int      bl_standby_pct = 15;
static volatile int      bl_deep_pct    = 10;
static volatile int      ss_play_mode   = 1;     /* while playing: 0 stay on Now Playing, 1 album art, 2 cover wall */
static volatile int      ss_pause_mode  = 1;     /* paused/stopped: 0 clock, 1 clock + cover, 2 analog, 3 cover wall */
static volatile int      bl_art_pct     = 60;    /* album art view brightness                         */
#define SCREENSAVER_TIMEOUT_MS       ((uint32_t)ss_timeout_min * 60000UL)
#define SCREENSAVER_SHIFT_MS         (2UL * 60UL * 1000UL)    /* pixel-shift interval  */
#define SCREENSAVER_DEEP_DIM_MS      ((uint32_t)ss_deep_min * 60000UL)

#define BACKLIGHT_FULL_PCT           100
#define BACKLIGHT_STANDBY_PCT        bl_standby_pct
#define BACKLIGHT_DEEP_PCT           bl_deep_pct

/* Clock screensaver look (Settings > Screensaver > CLOCK STYLE). */
static int ss_clk_font = 0;     /* 0 Classic, 1 Condensed, 2 Digital, 3 Elegant, 4 Future */
static int ss_clk_size = 1;     /* 0 Small, 1 Medium, 2 Large, 3 Huge                    */
static int ss_clk_fmt  = 0;     /* 0 12-hour + AM/PM, 1 12-hour, 2 24-hour               */
static int ss_clk_date = 2;     /* 0 off, 1 Sunday, October 4, 2 Sun  Oct 04  2026,
                                   3 Sunday 4 October, 4 2026-10-04                      */
static int ss_clk_col  = 0;     /* 0 theme colour, 1 white, 2 dim                        */
#define SS_CLK_FONTS 5
#define SS_CLK_SIZES 4
#define SS_CLK_FMTS  3
#define SS_CLK_DATES 5
#define SS_CLK_COLS  3

static lv_obj_t *screensaver_overlay = NULL;
static lv_obj_t *screensaver_clock_row = NULL;     /* the time + AM/PM, centred together */
static lv_obj_t *screensaver_ampm_label = NULL;
static lv_obj_t *screensaver_clock_label = NULL;
static lv_obj_t *screensaver_date_label = NULL;
static lv_timer_t *screensaver_timer = NULL;
static uint32_t screensaver_last_state_change_ms = 0;
static uint32_t screensaver_last_shift_ms = 0;
static uint32_t screensaver_shown_ms = 0;
static uint8_t screensaver_shift_index = 0;
static bool ntp_started = false;

/* What the standby overlay shows. */
#define SS_VIEW_CLOCK        0     /* clock + date                          */
#define SS_VIEW_CLOCK_COVER  1     /* clock + date + the paused song        */
#define SS_VIEW_ART          2     /* big cover, song, progress, clock      */
#define SS_VIEW_ANALOG       3     /* analog clock face (18e)               */
#define SS_VIEW_WALL         4     /* wall of covers from the library (18e) */
#define SS_VIEW_BIGTEXT      5     /* big song text (18e)                   */
#define SS_VIEW_VU           6     /* two analog VU meters (18e)            */
#define SS_VIEW_WEATHER      7     /* clock + weather (18e / 18w)           */
static int  screensaver_view = SS_VIEW_CLOCK;
static bool screensaver_preview = false;     /* opened with PREVIEW: stays until touched */
static bool screensaver_deep = false;        /* already dropped to deep-dim brightness   */

static lv_obj_t *ss_clock_box = NULL;        /* clock, date and the paused-song strip */
static lv_obj_t *ss_pause_box = NULL;
static lv_obj_t *ss_pause_img = NULL;
static lv_obj_t *ss_pause_state = NULL;
static lv_obj_t *ss_pause_title = NULL;
static lv_obj_t *ss_pause_artist = NULL;
static lv_obj_t *ss_art_box = NULL;          /* album art view */
static lv_obj_t *ss_art_img = NULL;
static lv_obj_t *ss_art_title = NULL;
static lv_obj_t *ss_art_artist = NULL;
static lv_obj_t *ss_art_album = NULL;
static lv_obj_t *ss_art_bar = NULL;
static lv_obj_t *ss_art_elapsed = NULL;
static lv_obj_t *ss_art_total = NULL;
static lv_obj_t *ss_art_clock = NULL;
static lv_obj_t *ss_art_info = NULL;
static bool      ss_have_track = false;
static bool      ss_have_cover = false;
static uint32_t  ss_art_avg = 0x202020;      /* average colour of the cover (RGB888) */
static char      ss_track_mode[16] = "";

/* Slow drift for the views with a picture (pixels). */
static const lv_point_t ss_art_drift[] = {
    {   0,   0 }, { -14,  10 }, {  12,  -8 }, {  -6, -16 },
    {  16,  14 }, { -18,  -4 }, {   8,  18 }, {   4, -12 }
};
#define SS_ART_DRIFT_COUNT (sizeof(ss_art_drift) / sizeof(ss_art_drift[0]))

/*
 * Backlight control.
 *
 * LVGL callbacks and timers only write backlight_target_pct.  This small task
 * applies it, so bsp_display_brightness_set() is never called from inside an
 * LVGL callback (where it could contend for the display lock).  Waking is
 * instant; dimming fades over about a second.
 */
static volatile int backlight_target_pct = BACKLIGHT_FULL_PCT;

static void backlight_task(void *arg)
{
    (void)arg;

    int applied = BACKLIGHT_FULL_PCT;
    bool warned = false;

    while (true) {
        int target = backlight_target_pct;
        int next = applied;

        if (target > applied) {
            next = target;
        } else if (target < applied) {
            next = applied - 2;
            if (next < target) {
                next = target;
            }
        }

        if (next != applied) {
            esp_err_t err = bsp_display_brightness_set(next);
            if (err != ESP_OK && !warned) {
                ESP_LOGW(TAG, "Backlight change failed: %s", esp_err_to_name(err));
                warned = true;
            }
            applied = next;
        }

        vTaskDelay(pdMS_TO_TICKS(applied == target ? 100 : 30));
    }
}

static const lv_point_t screensaver_shifts[] = {
    {  0,  0 },
    { -1,  0 },
    {  1,  0 },
    {  0,  1 },
    {  0, -1 },
    {  1,  1 },
    { -1, -1 },
    {  0,  0 }
};
#define SCREENSAVER_SHIFT_COUNT \
    (sizeof(screensaver_shifts) / sizeof(screensaver_shifts[0]))


static lv_obj_t *player_buttons[LMS_MAX_PLAYERS] = { NULL };


/*
 * Current playback state reported by Lyrion.
 *
 * true  = playing
 * false = paused/stopped/other
 */
static volatile bool current_player_playing = false;


/*
 * Prevent the volume-slider callback from sending a command
 * when the slider is being updated from Lyrion status.
 */
static volatile bool updating_volume_from_server = false;


