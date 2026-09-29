/*
 * P4-LMS - Microphone VU meter state and the full-screen visualizer (all styles)
 *
 * Part of main.c.  This file is #included by main.c (in order) and is not
 * compiled on its own, so it shares main.c's includes and static names.
 */

/* ============================================================
 * REAL-TIME MICROPHONE VU METER
 * ============================================================ */

#define VU_SEGMENT_COUNT       20
#define VU_SEGMENT_WIDTH       40
#define VU_SEGMENT_GAP         5
#define VU_SEGMENT_HEIGHT      18
#define VU_PANEL_WIDTH         976
#define VU_PANEL_HEIGHT        76      /* compact: leaves room for the navigation bar */
#define VU_ROW_L_Y             8
#define VU_ROW_R_Y             32
#define VU_AUDIO_SAMPLE_RATE   16000
#define VU_AUDIO_CHANNELS      2
#define VU_AUDIO_FRAME_MS      30
#define VU_AUDIO_SAMPLES       ((VU_AUDIO_SAMPLE_RATE * VU_AUDIO_FRAME_MS) / 1000)
#define VU_AUDIO_BUFFER_BYTES  (VU_AUDIO_SAMPLES * VU_AUDIO_CHANNELS * sizeof(int16_t))
#define VU_MIN_DBFS            (-45.0f)
#define VU_MAX_DBFS            (0.0f)

static lv_obj_t *vu_left_segments[VU_SEGMENT_COUNT] = {0};
static lv_obj_t *vu_right_segments[VU_SEGMENT_COUNT] = {0};
static lv_timer_t *vu_timer = NULL;
static volatile int vu_left_target = 0;
static volatile int vu_right_target = 0;
/* Raw RMS level per channel in dBFS - drives the analog needle meters. */
static volatile float vu_left_db = VU_MIN_DBFS;
static volatile float vu_right_db = VU_MIN_DBFS;
static int vu_left_display = 0;
static int vu_right_display = 0;
static float vu_left_display_f = 0.0f;
static float vu_right_display_f = 0.0f;
static int vu_left_peak_hold = 0;
static int vu_right_peak_hold = 0;
static uint32_t vu_left_peak_time = 0;
static uint32_t vu_right_peak_time = 0;
static lv_obj_t *vu_left_peak_marker = NULL;
static lv_obj_t *vu_right_peak_marker = NULL;

/* ============================================================
 * FULL-SCREEN MUSIC VISUALIZER
 * ============================================================ */

#define VIS_BAR_COUNT 36
#define VIS_BAR_WIDTH 16
#define VIS_BAR_GAP   8
#define VIS_MAX_H     285
/* Set to 0 to silence the "VISDIAG" log lines used to track down the flash. */
#define VIS_DIAG      1
#define VIS_TIMER_MS  40   /* microphone frames only arrive every 30 ms anyway */
#define VIS_BAR_START_X ((1024 - (VIS_BAR_COUNT * VIS_BAR_WIDTH + (VIS_BAR_COUNT - 1) * VIS_BAR_GAP)) / 2)

static lv_obj_t *visualizer_overlay = NULL;
/*
 * The spectrum is drawn by ONE custom-draw object instead of 36 x 3 separate
 * objects.  Moving/restyling 100+ objects every frame made LVGL run out of
 * invalidation slots (it then repaints the whole screen every frame) and it
 * walks the full object tree on every single invalidation, which saturated the
 * LVGL task, starved the idle task (task watchdog) and made the display flash.
 * Now the timer only updates plain numbers and invalidates one rectangle.
 */
#define VIS_CANVAS_X     VIS_BAR_START_X
#define VIS_CANVAS_Y     100
#define VIS_CANVAS_BASE  300      /* bar baseline (y = 400) relative to the canvas top */
#define VIS_CANVAS_W     (VIS_BAR_COUNT * VIS_BAR_WIDTH + (VIS_BAR_COUNT - 1) * VIS_BAR_GAP)
#define VIS_CANVAS_H     425      /* covers the tallest bar down to the reflections */

typedef struct {
    int16_t h;        /* bar height                         */
    int16_t rh;       /* reflection height                  */
    int16_t peak_h;   /* height of the peak-hold marker     */
    uint8_t bucket;   /* colour bucket 0..5                 */
    uint8_t refl;     /* 1 = brighter reflection            */
} vis_bar_draw_t;

static vis_bar_draw_t vis_bars[VIS_BAR_COUNT];
static lv_obj_t *visualizer_canvas = NULL;
static volatile int64_t vis_draw_us_max = 0;
static volatile int64_t vis_draw_us_last = 0;   /* cost of the last frame (frame pacing) */
static int64_t vis_draw_us_accum = 0;            /* all strips of the frame being drawn */

/* ---- visualizer styles ---------------------------------------------------- */
typedef enum {
    VIS_STYLE_BARS = 0,   /* classic bars with reflection and peak caps  */
    VIS_STYLE_LEDS,       /* segmented LED columns                        */
    VIS_STYLE_MIRROR,     /* bars growing up and down from a centre line  */
    VIS_STYLE_CURVE,      /* smooth filled spectrum                       */
    VIS_STYLE_SCOPE,      /* waveform / oscilloscope                      */
    VIS_STYLE_RADIAL,     /* bars fanned out around a circle              */
    VIS_STYLE_NEEDLES,    /* two analog VU needle meters (left / right)   */
    VIS_STYLE_STEREO,     /* left spectrum up, right spectrum down        */
    VIS_STYLE_DOTS,       /* dot-matrix spectrum                          */
    VIS_STYLE_COUNT
} vis_style_t;

#define VIS_STYLE_AUTO      VIS_STYLE_COUNT   /* chip value: rotate through all styles */

/* 1 = CURVE and SCOPE draw a real translucent glow, offloaded to the
 *     ESP32-P4's PPA hardware blender (requires LV_USE_PPA enabled in
 *     menuconfig: Component config -> LVGL configuration -> Rendering
 *     Configuration -> PPA).  If LV_USE_PPA is off, or the board's LVGL
 *     adapter does not wire PPA into LVGL's draw pipeline, this does the
 *     blending on the CPU instead, which is what caused the ~5-second
 *     watchdog stalls - so only set this to 1 once LV_USE_PPA is on.
 * 0 = the safe fallback: the same look pre-mixed into a solid colour, no
 *     real transparency, no per-pixel blending cost. */
#define VIS_TRANSLUCENT_GLOW 1
#define VIS_AUTO_SWITCH_MS  25000             /* time on each style in AUTO            */
#define VIS_WAVE_POINTS     128               /* scope resolution                      */
#define VIS_STYLE_NVS_KEY   "vis_style"
#define MIC_GAIN_NVS_KEY    "mic_gain"
/* Mic sensitivity for the meters and visualizers: 0 normal, 1 +6 dB,
 * 2 +12 dB, 3 +18 dB (a digital boost, for quiet listening levels). */
static volatile int mic_gain_step = 0;
static const int16_t mic_gain_q8[4] = { 256, 511, 1019, 2033 };   /* x1, x2, x4, x7.9 */

static const char *const vis_style_names[VIS_STYLE_COUNT + 1] = {
    "BARS", "LED", "MIRROR", "CURVE", "SCOPE", "RADIAL", "NEEDLES", "STEREO", "DOTS", "AUTO"
};
static int vis_style_selected = VIS_STYLE_BARS;          /* chip value, saved to flash */
static volatile int vis_style_active = VIS_STYLE_BARS;   /* style being drawn          */
static uint32_t vis_auto_tick = 0;
static lv_obj_t *vis_style_chip_label = NULL;
static lv_obj_t *vis_axis_objs[16];                      /* baseline + frequency labels */
static int vis_axis_count = 0;
static volatile int8_t vis_wave[VIS_WAVE_POINTS];
static volatile uint32_t vis_wave_seq = 0;

/* ---- visualizer colour palettes ------------------------------------------- */
typedef enum {
    VIS_PAL_THEME = 0,    /* the original warm receiver colours              */
    VIS_PAL_VU,           /* classic green / yellow / red meter              */
    VIS_PAL_FIRE,         /* deep red through orange to white-hot            */
    VIS_PAL_OCEAN,        /* deep blue through cyan to white                 */
    VIS_PAL_NEON,         /* purple, magenta, pink and cyan                  */
    VIS_PAL_RAINBOW,      /* every band its own colour, low notes red        */
    VIS_PAL_CYCLE,        /* rainbow that slowly rotates                     */
    VIS_PAL_COUNT
} vis_palette_t;

#define VIS_COLOR_NVS_KEY "vis_color"

static const char *const vis_palette_names[VIS_PAL_COUNT] = {
    "THEME", "VU GREEN", "FIRE", "OCEAN", "NEON", "RAINBOW", "CYCLE"
};

/* Six colours from quiet to loud (RAINBOW / CYCLE colour by position instead). */
static const uint32_t vis_palette_colors[VIS_PAL_COUNT][6] = {
    /* THEME   */ { 0, 0, 0, 0, 0, 0 },      /* taken from the active theme */
    /* VU GREEN*/ { 0x1E8C3A, 0x2FBF4B, 0x7ED957, 0xE6D93A, 0xF39C2E, 0xE0352B },
    /* FIRE    */ { 0x7A1010, 0xB3261E, 0xE8541A, 0xFF8A1F, 0xFFC93C, 0xFFF3B0 },
    /* OCEAN   */ { 0x0B3B66, 0x125E9C, 0x1E90D6, 0x27C4E8, 0x7FE7F2, 0xE6FBFF },
    /* NEON    */ { 0x4B1FA8, 0x8A2BE2, 0xD62BC4, 0xFF4FA3, 0x2BE4FF, 0xB6FFF7 },
    /* RAINBOW */ { 0, 0, 0, 0, 0, 0 },      /* coloured by position instead */
    /* CYCLE   */ { 0, 0, 0, 0, 0, 0 },
};
static const uint32_t vis_palette_peak[VIS_PAL_COUNT] = {
    0, 0xF5F5F5, 0xFFFFFF, 0xFFFFFF, 0xFFFFFF, 0xFFFFFF, 0xFFFFFF   /* [0]: theme's bright accent */
};

static int vis_palette_selected = VIS_PAL_THEME;
static volatile int vis_palette_active = VIS_PAL_THEME;
static lv_obj_t *vis_color_chip_label = NULL;
static lv_timer_t *visualizer_timer = NULL;
static volatile bool visualizer_visible = false;
static void vis_picker_close(void);   /* defined with the style picker */

/* The VU meter only ever moves a handful of its ~40 small segment bars at
 * once, so it can go a long time between full repaints of the panel.  On
 * this display's triple-buffered partial-update mode, a panel that is
 * hardly ever fully repainted is more likely to have one of the three
 * buffers holding a stale pixel from far earlier - seen as a brief flash a
 * few seconds apart.  Forcing one full repaint every couple of seconds
 * keeps all three buffers in step without adding noticeable load. */
static void vu_panel_keepalive_timer_cb(lv_timer_t *timer)
{
    if (screensaver_active || visualizer_visible) {
        return;           /* the panel is not on screen right now */
    }
    lv_obj_t *panel = (lv_obj_t *)lv_timer_get_user_data(timer);
    if (panel != NULL) {
        lv_obj_invalidate(panel);
    }
}
static volatile float visualizer_level = 0.0f;
static volatile float visualizer_bass = 0.0f;
static volatile float visualizer_mid = 0.0f;
static volatile float visualizer_treble = 0.0f;
static volatile float visualizer_peak = 0.0f;
static volatile float visualizer_spectrum[VIS_BAR_COUNT] = {0};
static volatile float vis_spectrum_l[VIS_BAR_COUNT] = {0};   /* STEREO style only */
static volatile float vis_spectrum_r[VIS_BAR_COUNT] = {0};

static void album_art_event_cb(lv_event_t *e);          /* 17_lyrion_server_and_tasks.c */
static void visualizer_overlay_event_cb(lv_event_t *e);
static void vsp_show(void);                 /* Settings > Visualizer (18d) */
static bool vis_return_to_settings = false; /* opened as a PREVIEW from that page */
static void visualizer_timer_cb(lv_timer_t *timer);

#define VU_METER_START_X 40
#define VU_METER_WIDTH \
    (VU_SEGMENT_COUNT * VU_SEGMENT_WIDTH + \
     (VU_SEGMENT_COUNT - 1) * VU_SEGMENT_GAP)
static esp_codec_dev_handle_t microphone_codec = NULL;
static volatile bool microphone_ready = false;

static int vu_db_to_segments(float dbfs)
{
    if (dbfs <= VU_MIN_DBFS) return 0;
    if (dbfs >= VU_MAX_DBFS) return VU_SEGMENT_COUNT;

    float normalized = (dbfs - VU_MIN_DBFS) /
                       (VU_MAX_DBFS - VU_MIN_DBFS);
    int segments = (int)(normalized * (float)VU_SEGMENT_COUNT + 0.5f);

    if (segments < 0) segments = 0;
    if (segments > VU_SEGMENT_COUNT) segments = VU_SEGMENT_COUNT;
    return segments;
}

/* Place a peak-hold marker, touching the object only when it actually moves. */
static void vu_update_peak_marker(lv_obj_t *marker, int peak, int y, int *last_x)
{
    if (marker == NULL) {
        return;
    }

    if (peak > 0) {
        int x = VU_METER_START_X +
                (peak - 1) * (VU_SEGMENT_WIDTH + VU_SEGMENT_GAP) +
                VU_SEGMENT_WIDTH - 2;
        if (x != *last_x) {
            lv_obj_set_pos(marker, x, y);
            *last_x = x;
        }
        if (lv_obj_has_flag(marker, LV_OBJ_FLAG_HIDDEN)) {
            lv_obj_clear_flag(marker, LV_OBJ_FLAG_HIDDEN);
        }
    } else if (!lv_obj_has_flag(marker, LV_OBJ_FLAG_HIDDEN)) {
        lv_obj_add_flag(marker, LV_OBJ_FLAG_HIDDEN);
        *last_x = -1;
    }
}

static void vu_meter_timer_cb(lv_timer_t *timer)
{
    (void)timer;

    /* A full-screen overlay is hiding the meter: nothing to animate. */
    if (screensaver_active || visualizer_visible) {
        return;
    }

    static int last_left_marker_x = -1;
    static int last_right_marker_x = -1;

    const float rise_speed = 0.42f;
    const float fall_speed = 0.16f;
    const uint32_t now = lv_tick_get();

    int target_l = vu_left_target;
    int target_r = vu_right_target;

    int old_l = vu_left_display;
    int old_r = vu_right_display;

    /* Smooth, analog-style ballistics: quick attack, slower decay. */
    if ((float)target_l > vu_left_display_f) {
        vu_left_display_f += ((float)target_l - vu_left_display_f) * rise_speed;
    } else {
        vu_left_display_f += ((float)target_l - vu_left_display_f) * fall_speed;
    }

    if ((float)target_r > vu_right_display_f) {
        vu_right_display_f += ((float)target_r - vu_right_display_f) * rise_speed;
    } else {
        vu_right_display_f += ((float)target_r - vu_right_display_f) * fall_speed;
    }

    vu_left_display = (int)(vu_left_display_f + 0.5f);
    vu_right_display = (int)(vu_right_display_f + 0.5f);

    if (vu_left_display < 0) vu_left_display = 0;
    if (vu_left_display > VU_SEGMENT_COUNT) vu_left_display = VU_SEGMENT_COUNT;
    if (vu_right_display < 0) vu_right_display = 0;
    if (vu_right_display > VU_SEGMENT_COUNT) vu_right_display = VU_SEGMENT_COUNT;

    /* Peak-hold markers: catch transients, hold briefly, then fall away. */
    if (vu_left_display > vu_left_peak_hold) {
        vu_left_peak_hold = vu_left_display;
        vu_left_peak_time = now;
    } else if (vu_left_peak_hold > 0 &&
               (uint32_t)(now - vu_left_peak_time) > 650U) {
        vu_left_peak_hold--;
        vu_left_peak_time = now;
    }

    if (vu_right_display > vu_right_peak_hold) {
        vu_right_peak_hold = vu_right_display;
        vu_right_peak_time = now;
    } else if (vu_right_peak_hold > 0 &&
               (uint32_t)(now - vu_right_peak_time) > 650U) {
        vu_right_peak_hold--;
        vu_right_peak_time = now;
    }

    /* Only touch segments whose state actually changed. */
    if (old_l != vu_left_display) {
        int from = old_l < vu_left_display ? old_l : vu_left_display;
        int to   = old_l > vu_left_display ? old_l : vu_left_display;
        for (int i = from; i < to; i++) {
            if (vu_left_segments[i] != NULL) {
                lv_obj_set_style_bg_opa(
                    vu_left_segments[i],
                    i < vu_left_display ? LV_OPA_COVER : LV_OPA_20,
                    LV_PART_MAIN
                );
            }
        }
    }

    if (old_r != vu_right_display) {
        int from = old_r < vu_right_display ? old_r : vu_right_display;
        int to   = old_r > vu_right_display ? old_r : vu_right_display;
        for (int i = from; i < to; i++) {
            if (vu_right_segments[i] != NULL) {
                lv_obj_set_style_bg_opa(
                    vu_right_segments[i],
                    i < vu_right_display ? LV_OPA_COVER : LV_OPA_20,
                    LV_PART_MAIN
                );
            }
        }
    }

    vu_update_peak_marker(vu_left_peak_marker, vu_left_peak_hold,
                          VU_ROW_L_Y, &last_left_marker_x);
    vu_update_peak_marker(vu_right_peak_marker, vu_right_peak_hold,
                          VU_ROW_R_Y, &last_right_marker_x);
}

static void visualizer_set_visible(bool visible)
{
    visualizer_visible = visible;

    if (visualizer_overlay != NULL) {
        if (visible) {
            vis_auto_tick = lv_tick_get();
            lv_obj_clear_flag(visualizer_overlay, LV_OBJ_FLAG_HIDDEN);
            lv_obj_move_foreground(visualizer_overlay);
        } else {
            vis_picker_close();
            lv_obj_add_flag(visualizer_overlay, LV_OBJ_FLAG_HIDDEN);
            ui_schedule_repaint(lv_screen_active());
        }
    }
}


/* Now Playing's VU meter strip opens the visualizer. */
static void vu_meter_tap_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) visualizer_set_visible(true);
}

static void visualizer_overlay_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        visualizer_set_visible(false);
        if (vis_return_to_settings) {          /* after a PREVIEW: back to that page */
            vis_return_to_settings = false;
            vsp_show();
        }
    }
}


/* ---- colour helpers ---- */

/* Colour for one band.  Palettes colour by loudness (bucket); RAINBOW and CYCLE
 * colour by position along the spectrum and use the level for brightness. */
static lv_color_t vis_color(int band, float level, int bucket)
{
    const int pal = vis_palette_active;

    if (pal == VIS_PAL_RAINBOW || pal == VIS_PAL_CYCLE) {
        int hue = (band * 300) / (VIS_BAR_COUNT - 1);          /* red ... magenta */
        if (pal == VIS_PAL_CYCLE) {
            hue = (hue + (int)((lv_tick_get() / 30U) % 360U)) % 360;
        }
        int value = 55 + (int)(level * 45.0f);
        if (value > 100) value = 100;
        return lv_color_hsv_to_rgb((uint16_t)hue, 95, (uint8_t)value);
    }
    if (pal == VIS_PAL_THEME) {
        return lv_color_hex(ui_theme->vis[bucket]);
    }
    return lv_color_hex(vis_palette_colors[pal][bucket]);
}

/* Mixes fg toward the canvas background by (255-pct)/255, as a flat RGB
 * colour, so the result can be drawn fully opaque (LV_OPA_COVER).  Real
 * per-pixel alpha blending (LV_OPA_30 etc. on a translucent fill) is one of
 * the more expensive things LVGL's software renderer does; baking the same
 * "faded" look into the colour itself is nearly free by comparison, and on
 * this display the difference was large enough to stall the whole UI task
 * for 100ms+ per frame and trip the watchdog. */
static lv_color_t vis_mix_toward_bg(uint32_t fg, uint8_t pct)
{
    const uint32_t bg = UI_DEEP;
    const uint8_t fr = (uint8_t)(fg >> 16), fgc = (uint8_t)(fg >> 8), fb = (uint8_t)fg;
    const uint8_t br = (uint8_t)(bg >> 16), bgc = (uint8_t)(bg >> 8), bb = (uint8_t)bg;
    const uint8_t r = (uint8_t)(((uint16_t)fr * pct + (uint16_t)br * (255 - pct)) / 255);
    const uint8_t g = (uint8_t)(((uint16_t)fgc * pct + (uint16_t)bgc * (255 - pct)) / 255);
    const uint8_t b = (uint8_t)(((uint16_t)fb * pct + (uint16_t)bb * (255 - pct)) / 255);
    return lv_color_make(r, g, b);
}

static lv_color_t vis_peak_color(void)
{
    if (vis_palette_active == VIS_PAL_THEME) {
        return lv_color_hex(UI_ACCENT_BRIGHT);
    }
    return lv_color_hex(vis_palette_peak[vis_palette_active]);
}

/* ---- style helpers ---- */
static inline float vis_bar_level(const vis_bar_draw_t *b)
{
    float v = (float)(b->h - 12) / (float)VIS_MAX_H;
    return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
}

static inline float vis_bar_peak(const vis_bar_draw_t *b)
{
    float v = (float)(b->peak_h - 12) / (float)VIS_MAX_H;
    return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
}

static inline int vis_bucket_for(float n)
{
    if (n < 0.18f) return 0;
    if (n < 0.38f) return 1;
    if (n < 0.58f) return 2;
    if (n < 0.76f) return 3;
    if (n < 0.90f) return 4;
    return 5;
}

static void mic_gain_save(void)
{
    nvs_handle_t nvs;
    if (nvs_open(PLAYER_NVS_NAMESPACE, NVS_READWRITE, &nvs) != ESP_OK) return;
    nvs_set_u8(nvs, MIC_GAIN_NVS_KEY, (uint8_t)mic_gain_step);
    nvs_commit(nvs);
    nvs_close(nvs);
}

/* Which style is remembered across restarts. */
static void visualizer_style_load(void)
{
    {
        nvs_handle_t g;
        uint8_t v = 0;
        if (nvs_open(PLAYER_NVS_NAMESPACE, NVS_READONLY, &g) == ESP_OK) {
            if (nvs_get_u8(g, MIC_GAIN_NVS_KEY, &v) == ESP_OK && v < 4) mic_gain_step = v;
            nvs_close(g);
        }
    }
    nvs_handle_t nvs;
    uint8_t value = 0;

    if (nvs_open(PLAYER_NVS_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) {
        return;
    }
    esp_err_t ret = nvs_get_u8(nvs, VIS_STYLE_NVS_KEY, &value);
    nvs_close(nvs);

    /* AUTO is stored as 0xFF so that adding new styles later never changes
     * what a saved value means. */
    if (ret == ESP_OK) {
        if (value == 0xFF) {
            vis_style_selected = VIS_STYLE_AUTO;
        } else if (value < VIS_STYLE_COUNT) {
            vis_style_selected = value;
        }
    }
    vis_style_active = (vis_style_selected == VIS_STYLE_AUTO) ? VIS_STYLE_BARS : vis_style_selected;

    if (nvs_open(PLAYER_NVS_NAMESPACE, NVS_READONLY, &nvs) == ESP_OK) {
        uint8_t pal = 0;
        if (nvs_get_u8(nvs, VIS_COLOR_NVS_KEY, &pal) == ESP_OK && pal < VIS_PAL_COUNT) {
            vis_palette_selected = pal;
            vis_palette_active = pal;
        }
        nvs_close(nvs);
    }
}

static void visualizer_style_save(void)
{
    nvs_handle_t nvs;
    if (nvs_open(PLAYER_NVS_NAMESPACE, NVS_READWRITE, &nvs) != ESP_OK) {
        return;
    }
    const uint8_t stored_style = (vis_style_selected == VIS_STYLE_AUTO)
                                     ? 0xFF : (uint8_t)vis_style_selected;
    if (nvs_set_u8(nvs, VIS_STYLE_NVS_KEY, stored_style) == ESP_OK &&
        nvs_set_u8(nvs, VIS_COLOR_NVS_KEY, (uint8_t)vis_palette_selected) == ESP_OK) {
        nvs_commit(nvs);
    }
    nvs_close(nvs);
}

/* Update the chip text, show/hide the frequency axis and redraw
 * (LVGL lock held or LVGL callback context). */
static void vis_apply_style_ui(void)
{
    const int active = vis_style_active;
    const bool show_axis = (active == VIS_STYLE_BARS || active == VIS_STYLE_LEDS ||
                            active == VIS_STYLE_CURVE || active == VIS_STYLE_DOTS);

    for (int i = 0; i < vis_axis_count; i++) {
        if (vis_axis_objs[i] == NULL) continue;
        if (show_axis) lv_obj_clear_flag(vis_axis_objs[i], LV_OBJ_FLAG_HIDDEN);
        else           lv_obj_add_flag(vis_axis_objs[i], LV_OBJ_FLAG_HIDDEN);
    }

    if (vis_style_chip_label != NULL) {
        char text[40];
        if (vis_style_selected == VIS_STYLE_AUTO) {
            snprintf(text, sizeof(text), "AUTO (%s)   NEXT " LV_SYMBOL_RIGHT, vis_style_names[active]);
        } else {
            snprintf(text, sizeof(text), "%s   NEXT " LV_SYMBOL_RIGHT, vis_style_names[vis_style_selected]);
        }
        lv_label_set_text(vis_style_chip_label, text);
    }
    if (vis_color_chip_label != NULL) {
        char text[40];
        snprintf(text, sizeof(text), LV_SYMBOL_SETTINGS "  SETTINGS");
        lv_label_set_text(vis_color_chip_label, text);
    }
    if (visualizer_canvas != NULL) {
        lv_obj_invalidate(visualizer_canvas);
    }
}

/* Tap the colour chip to step through the palettes. */
/* SETTINGS: the Settings > Visualizer page (styles, colours, preview). */
static void vis_color_chip_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    vis_return_to_settings = false;
    visualizer_set_visible(false);
    vsp_show();
}

/* ---- STYLE picker: a grid of every style, opened from the STYLE chip ---- */
static lv_obj_t *vis_picker = NULL;                       /* full-overlay backdrop */


static void vis_picker_close(void)
{
    if (vis_picker != NULL) {
        lv_obj_add_flag(vis_picker, LV_OBJ_FLAG_HIDDEN);
    }
}


/* A tap on the dimmed area around the grid just closes the picker (it does
 * not fall through and close the visualizer). */


/* Tap the STYLE chip to open the picker. */
/* NEXT / PREV: step through the styles (AUTO last), and remember it. */
static void vis_style_chip_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    const int step = (int)(intptr_t)lv_event_get_user_data(e) < 0 ? VIS_STYLE_COUNT : 1;   /* -1 = back */
    vis_style_selected = (vis_style_selected + step) % (VIS_STYLE_COUNT + 1);
    if (vis_style_selected == VIS_STYLE_AUTO) vis_auto_tick = lv_tick_get();
    else                                      vis_style_active = vis_style_selected;
    vis_apply_style_ui();
    visualizer_style_save();
}

/* ---- the styles (each draws into the canvas rectangle) ---- */


/* ---- Culling --------------------------------------------------------------
 * The screen is drawn in horizontal strips, and the draw callback runs once
 * per strip.  LVGL queues every shape it is given (a small allocation plus a
 * walk to the end of the task list), even ones that fall outside the strip,
 * so a style with a thousand dots used to queue ~10 000 shapes per frame.
 * These wrappers drop shapes that miss the strip being drawn. */
static inline bool vis_in_clip(lv_layer_t *layer, int32_t x1, int32_t y1, int32_t x2, int32_t y2)
{
    const lv_area_t *c = &layer->_clip_area;
    return !(x2 < c->x1 || x1 > c->x2 || y2 < c->y1 || y1 > c->y2);
}

static inline void vis_rect(lv_layer_t *layer, const lv_draw_rect_dsc_t *dsc, const lv_area_t *a)
{
    if (vis_in_clip(layer, a->x1, a->y1, a->x2, a->y2)) lv_draw_rect(layer, dsc, a);
}

static inline void vis_label(lv_layer_t *layer, const lv_draw_label_dsc_t *dsc, const lv_area_t *a)
{
    if (vis_in_clip(layer, a->x1, a->y1, a->x2, a->y2)) lv_draw_label(layer, dsc, a);
}

static inline void vis_line(lv_layer_t *layer, const lv_draw_line_dsc_t *dsc)
{
    const int32_t w = (int32_t)dsc->width + 2;
    const int32_t x1 = (int32_t)LV_MIN(dsc->p1.x, dsc->p2.x) - w, x2 = (int32_t)LV_MAX(dsc->p1.x, dsc->p2.x) + w;
    const int32_t y1 = (int32_t)LV_MIN(dsc->p1.y, dsc->p2.y) - w, y2 = (int32_t)LV_MAX(dsc->p1.y, dsc->p2.y) + w;
    if (vis_in_clip(layer, x1, y1, x2, y2)) lv_draw_line(layer, dsc);
}

static inline void vis_arc(lv_layer_t *layer, const lv_draw_arc_dsc_t *dsc)
{
    const int32_t r = (int32_t)dsc->radius + 2;
    if (vis_in_clip(layer, dsc->center.x - r, dsc->center.y - r, dsc->center.x + r, dsc->center.y + r))
        lv_draw_arc(layer, dsc);
}

/* Classic: bars with faint reflections and peak-hold caps. */
static void vis_draw_bars(lv_layer_t *layer, const lv_area_t *canvas)
{
    const int base = canvas->y1 + VIS_CANVAS_BASE;
    lv_draw_rect_dsc_t dsc;

    for (int i = 0; i < VIS_BAR_COUNT; i++) {
        const vis_bar_draw_t *b = &vis_bars[i];
        const int x1 = canvas->x1 + i * (VIS_BAR_WIDTH + VIS_BAR_GAP);
        const int x2 = x1 + VIS_BAR_WIDTH - 1;
        const lv_color_t color = vis_color(i, vis_bar_level(b), b->bucket);
        lv_area_t a;

        /* reflection (faint, below the baseline) */
        lv_draw_rect_dsc_init(&dsc);
        dsc.bg_color = color;
        dsc.bg_opa = b->refl ? LV_OPA_30 : LV_OPA_10;
        a.x1 = x1; a.x2 = x2;
        a.y1 = base + 3;
        a.y2 = base + 3 + b->rh - 1;
        vis_rect(layer, &dsc, &a);

        /* bar */
        lv_draw_rect_dsc_init(&dsc);
        dsc.bg_color = color;
        dsc.bg_opa = LV_OPA_COVER;
        dsc.radius = 2;
        a.y1 = base - b->h;
        a.y2 = base - 1;
        vis_rect(layer, &dsc, &a);

        /* peak-hold marker */
        lv_draw_rect_dsc_init(&dsc);
        dsc.bg_color = vis_peak_color();
        dsc.bg_opa = LV_OPA_COVER;
        a.y1 = base - b->peak_h;
        a.y2 = a.y1 + 2;
        vis_rect(layer, &dsc, &a);
    }
}

/* LED: each column is a stack of small lit segments. */
static void vis_draw_leds(lv_layer_t *layer, const lv_area_t *canvas)
{
    const int base = canvas->y1 + VIS_CANVAS_BASE;
    const int pitch = 12;
    const int seg_h = 9;
    const int max_seg = VIS_MAX_H / pitch;
    lv_draw_rect_dsc_t dsc;

    lv_draw_rect_dsc_init(&dsc);
    dsc.bg_opa = LV_OPA_COVER;
    dsc.radius = 1;

    for (int i = 0; i < VIS_BAR_COUNT; i++) {
        const vis_bar_draw_t *b = &vis_bars[i];
        const int x1 = canvas->x1 + i * (VIS_BAR_WIDTH + VIS_BAR_GAP);
        const int x2 = x1 + VIS_BAR_WIDTH - 1;
        const int lit = (int)(vis_bar_level(b) * (float)max_seg + 0.5f);
        const int peak_seg = (int)(vis_bar_peak(b) * (float)max_seg + 0.5f);
        lv_area_t a;
        a.x1 = x1;
        a.x2 = x2;

        for (int j = 0; j < lit; j++) {
            int bucket = (j * 6) / max_seg;
            if (bucket > 5) bucket = 5;
            dsc.bg_color = vis_color(i, (float)(j + 1) / (float)max_seg, bucket);
            a.y2 = base - j * pitch - 1;
            a.y1 = a.y2 - seg_h + 1;
            vis_rect(layer, &dsc, &a);
        }
        if (peak_seg > lit && peak_seg > 0) {
            dsc.bg_color = vis_peak_color();
            a.y2 = base - (peak_seg - 1) * pitch - 1;
            a.y1 = a.y2 - seg_h + 1;
            vis_rect(layer, &dsc, &a);
        }
    }
}

/* Mirror: bars grow up and down from a centre line. */
static void vis_draw_mirror(lv_layer_t *layer, const lv_area_t *canvas)
{
    const int cy = canvas->y1 + 212;
    const int span = 195;
    lv_draw_rect_dsc_t dsc;
    lv_area_t a;

    /* centre line */
    lv_draw_rect_dsc_init(&dsc);
    dsc.bg_color = lv_color_hex(UI_ACCENT_DARK);
    dsc.bg_opa = LV_OPA_COVER;
    a.x1 = canvas->x1;
    a.x2 = canvas->x2;
    a.y1 = cy - 1;
    a.y2 = cy;
    vis_rect(layer, &dsc, &a);

    for (int i = 0; i < VIS_BAR_COUNT; i++) {
        const vis_bar_draw_t *b = &vis_bars[i];
        const int x1 = canvas->x1 + i * (VIS_BAR_WIDTH + VIS_BAR_GAP);
        const int x2 = x1 + VIS_BAR_WIDTH - 1;
        const int half = 5 + (int)(vis_bar_level(b) * (float)span);
        const int peak = 5 + (int)(vis_bar_peak(b) * (float)span);
        const lv_color_t color = vis_color(i, vis_bar_level(b), b->bucket);

        lv_draw_rect_dsc_init(&dsc);
        dsc.bg_color = color;
        dsc.radius = 2;
        a.x1 = x1;
        a.x2 = x2;

        dsc.bg_opa = LV_OPA_COVER;                 /* upper half */
        a.y1 = cy - half;
        a.y2 = cy - 2;
        vis_rect(layer, &dsc, &a);

        dsc.bg_opa = LV_OPA_60;                    /* lower half, a little softer */
        a.y1 = cy + 2;
        a.y2 = cy + half;
        vis_rect(layer, &dsc, &a);

        lv_draw_rect_dsc_init(&dsc);               /* peak caps */
        dsc.bg_color = vis_peak_color();
        dsc.bg_opa = LV_OPA_COVER;
        a.y1 = cy - peak - 2;
        a.y2 = a.y1 + 2;
        vis_rect(layer, &dsc, &a);
        a.y1 = cy + peak;
        a.y2 = a.y1 + 2;
        vis_rect(layer, &dsc, &a);
    }
}

/* Curve: smooth filled spectrum with a bright outline. */
static void vis_draw_curve(lv_layer_t *layer, const lv_area_t *canvas)
{
    const int base = canvas->y1 + VIS_CANVAS_BASE;
    const int pitch = VIS_BAR_WIDTH + VIS_BAR_GAP;
    #if VIS_TRANSLUCENT_GLOW
    const int col_w = 4;      /* full resolution - blending is offloaded to the PPA */
#else
    const int col_w = 8;      /* CPU fallback: halves the draw calls, still looks smooth */
#endif
    const int cols = VIS_CANVAS_W / col_w;
    lv_draw_rect_dsc_t dsc;
    lv_area_t a;

    for (int c = 0; c < cols; c++) {
        const int x = c * col_w;
        float pos = ((float)x + (float)col_w * 0.5f - (float)VIS_BAR_WIDTH * 0.5f) / (float)pitch;
        int i0 = (int)floorf(pos);
        float frac = pos - (float)i0;
        if (i0 < 0) { i0 = 0; frac = 0.0f; }
        if (i0 >= VIS_BAR_COUNT - 1) { i0 = VIS_BAR_COUNT - 1; frac = 0.0f; }

        const float l0 = vis_bar_level(&vis_bars[i0]);
        const float l1 = vis_bar_level(&vis_bars[i0 + (frac > 0.0f ? 1 : 0)]);
        const float f = 0.5f - 0.5f * cosf(3.14159265f * frac);
        const float level = l0 * (1.0f - f) + l1 * f;
        const int h = 6 + (int)(level * (float)(VIS_MAX_H - 6));
        const int cband = (c * (VIS_BAR_COUNT - 1)) / (cols - 1);
        const lv_color_t color = vis_color(cband, level, vis_bucket_for(level));

        a.x1 = canvas->x1 + x;
        a.x2 = a.x1 + col_w - 1;

        lv_draw_rect_dsc_init(&dsc);               /* filled body */
        dsc.bg_color = color;
#if VIS_TRANSLUCENT_GLOW
        dsc.bg_opa = LV_OPA_30;                     /* real blend - needs PPA, see the toggle above */
#else
        dsc.bg_color = vis_mix_toward_bg((uint32_t)color.red << 16 | (uint32_t)color.green << 8 | color.blue, 80);
        dsc.bg_opa = LV_OPA_COVER;                  /* pre-mixed, no blending cost */
#endif
        a.y1 = base - h;
        a.y2 = base - 1;
        vis_rect(layer, &dsc, &a);

        lv_draw_rect_dsc_init(&dsc);               /* bright top edge */
        dsc.bg_color = color;
        dsc.bg_opa = LV_OPA_COVER;
        a.y1 = base - h;
        a.y2 = a.y1 + 2;
        vis_rect(layer, &dsc, &a);
    }
}

/* Scope: the audio waveform itself, auto-scaled. */
static void vis_draw_scope(lv_layer_t *layer, const lv_area_t *canvas)
{
    const int cy = canvas->y1 + 212;
#if VIS_TRANSLUCENT_GLOW
    const int col_w = 4;      /* full resolution - blending is offloaded to the PPA */
#else
    const int col_w = 8;      /* CPU fallback: halves the draw calls, still looks smooth */
#endif
    const int cols = VIS_CANVAS_W / col_w;
    const float amp = 190.0f;
    const int min_h = 6;       /* guaranteed trace thickness, even where the signal is flat */
    lv_draw_rect_dsc_t dsc;
    lv_area_t a;

    /* centre line */
    lv_draw_rect_dsc_init(&dsc);
    dsc.bg_color = lv_color_hex(UI_EDGE_SOFT);
    dsc.bg_opa = LV_OPA_COVER;
    a.x1 = canvas->x1;
    a.x2 = canvas->x2;
    a.y1 = cy;
    a.y2 = cy + 1;
    vis_rect(layer, &dsc, &a);

    /* A genuinely diagonal, anti-aliased line per sample (what this looked
     * like briefly) turned out to be far too expensive on this hardware -
     * LVGL's line anti-aliasing math (not blending) was the actual cost, and
     * it stalled the whole display every frame.  Axis-aligned RECTANGLES
     * have none of that cost, same as every other style here, so the trace
     * is a filled bar per column connecting to the previous one - just with
     * light smoothing and a guaranteed minimum thickness so it still reads
     * as one continuous, fairly thick ribbon instead of a thin hairline
     * that disappears wherever the signal is briefly flat. */
    float smoothed = 0.0f;
    int prev_y = cy;

    for (int c = 0; c < cols; c++) {
        const float pos = (float)c * (float)(VIS_WAVE_POINTS - 1) / (float)(cols - 1);
        int i0 = (int)pos;
        if (i0 > VIS_WAVE_POINTS - 2) i0 = VIS_WAVE_POINTS - 2;
        const float frac = pos - (float)i0;
        const float v = ((float)vis_wave[i0] * (1.0f - frac) + (float)vis_wave[i0 + 1] * frac) / 127.0f;

        smoothed = (c == 0) ? v : (smoothed * 0.55f + v * 0.45f);
        const int y = cy - (int)(smoothed * amp);

        int y1 = (c == 0) ? y : (prev_y < y ? prev_y : y);
        int y2 = (c == 0) ? y : (prev_y < y ? y : prev_y);
        if (y2 - y1 < min_h) {
            const int mid = (y1 + y2) / 2;
            y1 = mid - min_h / 2;
            y2 = mid + min_h / 2;
        }
        prev_y = y;

        const lv_color_t color = vis_color((c * (VIS_BAR_COUNT - 1)) / (cols - 1),
                                           fabsf(smoothed), vis_bucket_for(fabsf(smoothed)));
        a.x1 = canvas->x1 + c * col_w;
        a.x2 = a.x1 + col_w - 1;

        lv_draw_rect_dsc_init(&dsc);               /* soft glow */
        dsc.bg_color = color;
#if VIS_TRANSLUCENT_GLOW
        dsc.bg_opa = LV_OPA_20;                     /* real blend - proven safe for rectangles */
#else
        dsc.bg_color = vis_mix_toward_bg((uint32_t)color.red << 16 | (uint32_t)color.green << 8 | color.blue, 55);
        dsc.bg_opa = LV_OPA_COVER;                  /* pre-mixed, no blending cost */
#endif
        a.y1 = y1 - 4;
        a.y2 = y2 + 4;
        vis_rect(layer, &dsc, &a);

        lv_draw_rect_dsc_init(&dsc);               /* bright trace */
        dsc.bg_color = color;
        dsc.bg_opa = LV_OPA_COVER;
        a.y1 = y1;
        a.y2 = y2;
        vis_rect(layer, &dsc, &a);
    }
}

/* Radial: 72 spokes fanned round a ring, low notes at the top. */
static void vis_draw_radial(lv_layer_t *layer, const lv_area_t *canvas)
{
    static float cs[72];
    static float sn[72];
    static bool ready = false;
    const int cx = (canvas->x1 + canvas->x2) / 2;
    const int cy = (canvas->y1 + canvas->y2) / 2;
    const int r0 = 62;
    const int len_max = 138;

    if (!ready) {
        for (int k = 0; k < 72; k++) {
            const float ang = -1.57079633f + (float)k * 6.28318531f / 72.0f;
            cs[k] = cosf(ang);
            sn[k] = sinf(ang);
        }
        ready = true;
    }

    /* inner ring */
    lv_draw_rect_dsc_t rd;
    lv_draw_rect_dsc_init(&rd);
    rd.bg_opa = LV_OPA_TRANSP;
    rd.border_color = lv_color_hex(UI_ACCENT_DARK);
    rd.border_width = 2;
    rd.radius = LV_RADIUS_CIRCLE;
    lv_area_t ring = { cx - (r0 - 8), cy - (r0 - 8), cx + (r0 - 8), cy + (r0 - 8) };
    vis_rect(layer, &rd, &ring);

    lv_draw_line_dsc_t ld;
    lv_draw_line_dsc_init(&ld);
    ld.width = 5;
    ld.opa = LV_OPA_COVER;

    for (int k = 0; k < 72; k++) {
        const int band = (k < 36) ? k : 71 - k;
        const float level = vis_bar_level(&vis_bars[band]);
        const float len = 6.0f + level * (float)len_max;

        ld.color = vis_color(band, level, vis_bucket_for(level));
        ld.p1.x = cx + (int)(cs[k] * (float)r0);
        ld.p1.y = cy + (int)(sn[k] * (float)r0);
        ld.p2.x = cx + (int)(cs[k] * ((float)r0 + len));
        ld.p2.y = cy + (int)(sn[k] * ((float)r0 + len));
        vis_line(layer, &ld);
    }
}

/* ---- NEEDLES: two analog VU meters -------------------------------------
 * Drawn with a handful of shapes (two faces, a few arcs, ~20 short ticks,
 * a few labels and two needles) - nothing like the hundreds of anti-aliased
 * segments that stalled SCOPE.  Each frame only the area swept by the two
 * needles is invalidated, not the whole canvas. */
#define NEEDLE_FACE_W     400
#define NEEDLE_FACE_H     300
#define NEEDLE_FACE_TOP   36      /* face top, relative to the canvas top   */
#define NEEDLE_PIVOT_DY   282     /* pivot below the face top               */
#define NEEDLE_SCALE_R    228     /* radius of the printed scale arc        */
#define NEEDLE_LEN        236
#define NEEDLE_HUB_R      26
#define NEEDLE_SWEEP_DEG  45.0f   /* +/- from vertical                      */
#define NEEDLE_RED_FROM   0.80f   /* where the red zone starts (0..1)        */

static float vis_needle_pos[2] = { 0.0f, 0.0f };   /* 0..1 along the scale */
static bool  vis_needle_peak[2] = { false, false };

static void needle_face_geom(const lv_area_t *canvas, int ch, lv_area_t *face, lv_point_t *pivot)
{
    const int gap = VIS_CANVAS_W - 2 * NEEDLE_FACE_W;
    face->x1 = canvas->x1 + ch * (NEEDLE_FACE_W + gap);
    face->y1 = canvas->y1 + NEEDLE_FACE_TOP;
    face->x2 = face->x1 + NEEDLE_FACE_W - 1;
    face->y2 = face->y1 + NEEDLE_FACE_H - 1;
    pivot->x = face->x1 + NEEDLE_FACE_W / 2;
    pivot->y = face->y1 + NEEDLE_PIVOT_DY;
}

/* Point at radius r for scale position n (0..1). */
static lv_point_t needle_point(lv_point_t pivot, float n, int r)
{
    const float th = (-NEEDLE_SWEEP_DEG + 2.0f * NEEDLE_SWEEP_DEG * n) * 0.01745329f;
    lv_point_t p;
    p.x = pivot.x + (int32_t)lroundf((float)r * sinf(th));
    p.y = pivot.y - (int32_t)lroundf((float)r * cosf(th));
    return p;
}

/* Rainbow / cycle colour by position, which makes no sense on a meter face -
 * those two fall back to the theme's own colours here. */
static uint32_t needle_palette(int bucket)
{
    const int pal = vis_palette_active;
    if (pal == VIS_PAL_THEME || pal == VIS_PAL_RAINBOW || pal == VIS_PAL_CYCLE) {
        return ui_theme->vis[bucket];
    }
    return vis_palette_colors[pal][bucket];
}

static void needle_peak_led_area(const lv_area_t *face, lv_area_t *a)
{
    a->x1 = face->x2 - 36;
    a->y1 = face->y1 + 16;
    a->x2 = a->x1 + 15;
    a->y2 = a->y1 + 15;
}

static void vis_draw_needles(lv_layer_t *layer, const lv_area_t *canvas)
{
    static const char *const labels[] = { "-40", "-30", "-20", "-10", "-5", "0" };
    static const float label_db[] = { -40.0f, -30.0f, -20.0f, -10.0f, -5.0f, 0.0f };
    static const char *const side_name[2] = { "LEFT", "RIGHT" };

    const uint32_t tint = needle_palette(2);
    const lv_color_t face_col  = vis_mix_toward_bg(tint, 38);
    const lv_color_t rim_col   = vis_mix_toward_bg(tint, 150);
    const lv_color_t scale_col = vis_peak_color();
    const lv_color_t red_col   = lv_color_hex(0xE0453A);
    const lv_color_t dim_col   = vis_mix_toward_bg(0xFFFFFF, 110);

    for (int ch = 0; ch < 2; ch++) {
        lv_area_t face;
        lv_point_t pivot;
        needle_face_geom(canvas, ch, &face, &pivot);

        /* face */
        lv_draw_rect_dsc_t rd;
        lv_draw_rect_dsc_init(&rd);
        rd.bg_color = face_col;
        rd.bg_opa = LV_OPA_COVER;
        rd.radius = 16;
        rd.border_color = rim_col;
        rd.border_width = 2;
        rd.border_opa = LV_OPA_COVER;
        vis_rect(layer, &rd, &face);

        /* scale arc: normal part, then the red zone */
        lv_draw_arc_dsc_t ad;
        lv_draw_arc_dsc_init(&ad);
        ad.center = pivot;
        ad.radius = NEEDLE_SCALE_R;
        ad.width = 3;
        ad.color = scale_col;
        ad.start_angle = 270.0f - NEEDLE_SWEEP_DEG;
        ad.end_angle = 270.0f - NEEDLE_SWEEP_DEG + 2.0f * NEEDLE_SWEEP_DEG * NEEDLE_RED_FROM;
        vis_arc(layer, &ad);

        ad.width = 8;
        ad.color = red_col;
        ad.start_angle = ad.end_angle;
        ad.end_angle = 270.0f + NEEDLE_SWEEP_DEG;
        vis_arc(layer, &ad);

        /* ticks every 5 dB, longer ones where there is a number */
        lv_draw_line_dsc_t ld;
        lv_draw_line_dsc_init(&ld);
        ld.width = 2;
        ld.opa = LV_OPA_COVER;
        for (int db = (int)VU_MIN_DBFS; db <= (int)VU_MAX_DBFS; db += 5) {
            const float n = ((float)db - VU_MIN_DBFS) / (VU_MAX_DBFS - VU_MIN_DBFS);
            const bool major = (db == -40 || db == -30 || db == -20 || db == -10 || db == -5 || db == 0);
            const lv_point_t p1 = needle_point(pivot, n, NEEDLE_SCALE_R - (major ? 14 : 8));
            const lv_point_t p2 = needle_point(pivot, n, NEEDLE_SCALE_R + (major ? 10 : 5));
            ld.color = (n >= NEEDLE_RED_FROM) ? red_col : scale_col;
            ld.p1.x = p1.x; ld.p1.y = p1.y;
            ld.p2.x = p2.x; ld.p2.y = p2.y;
            vis_line(layer, &ld);
        }

        /* numbers */
        lv_draw_label_dsc_t td;
        lv_draw_label_dsc_init(&td);
        td.font = &p4_font_14;
        td.align = LV_TEXT_ALIGN_CENTER;
        for (int i = 0; i < 6; i++) {
            const float n = (label_db[i] - VU_MIN_DBFS) / (VU_MAX_DBFS - VU_MIN_DBFS);
            const lv_point_t p = needle_point(pivot, n, NEEDLE_SCALE_R + 24);
            lv_area_t ta = { p.x - 20, p.y - 8, p.x + 20, p.y + 8 };
            td.text = labels[i];
            td.color = (n >= NEEDLE_RED_FROM) ? red_col : scale_col;
            vis_label(layer, &td, &ta);
        }

        /* "VU" and the channel name */
        td.font = &p4_font_28;
        td.color = dim_col;
        td.text = "VU";
        lv_area_t vu_a = { pivot.x - 40, pivot.y - 128, pivot.x + 40, pivot.y - 96 };
        vis_label(layer, &td, &vu_a);

        td.font = &p4_font_14;
        td.text = side_name[ch];
        lv_area_t nm_a = { pivot.x - 50, pivot.y - 84, pivot.x + 50, pivot.y - 68 };
        vis_label(layer, &td, &nm_a);

        /* peak lamp */
        lv_area_t led;
        needle_peak_led_area(&face, &led);
        lv_draw_rect_dsc_init(&rd);
        rd.radius = LV_RADIUS_CIRCLE;
        rd.bg_opa = LV_OPA_COVER;
        rd.bg_color = vis_needle_peak[ch] ? red_col : vis_mix_toward_bg(0xE0453A, 60);
        vis_rect(layer, &rd, &led);

        td.text = "PEAK";
        td.color = dim_col;
        lv_area_t pk_a = { led.x1 - 50, led.y1, led.x1 - 6, led.y2 };
        td.align = LV_TEXT_ALIGN_RIGHT;
        vis_label(layer, &td, &pk_a);
        td.align = LV_TEXT_ALIGN_CENTER;

        /* needle */
        const lv_point_t tip = needle_point(pivot, vis_needle_pos[ch], NEEDLE_LEN);
        const lv_point_t root = needle_point(pivot, vis_needle_pos[ch], NEEDLE_HUB_R - 6);
        lv_draw_line_dsc_init(&ld);
        ld.width = 3;
        ld.opa = LV_OPA_COVER;
        ld.color = scale_col;
        ld.round_end = 1;
        ld.p1.x = root.x; ld.p1.y = root.y;
        ld.p2.x = tip.x;  ld.p2.y = tip.y;
        vis_line(layer, &ld);

        /* hub */
        lv_draw_rect_dsc_init(&rd);
        rd.radius = LV_RADIUS_CIRCLE;
        rd.bg_opa = LV_OPA_COVER;
        rd.bg_color = lv_color_hex(0x15171A);
        rd.border_color = rim_col;
        rd.border_width = 2;
        rd.border_opa = LV_OPA_COVER;
        lv_area_t hub = { pivot.x - 14, pivot.y - 14, pivot.x + 14, pivot.y + 14 };
        vis_rect(layer, &rd, &hub);
    }
}

/* Bounding box of one needle at scale position n, with a little margin. */
static void needle_box(lv_point_t pivot, float n, lv_area_t *a)
{
    const lv_point_t tip = needle_point(pivot, n, NEEDLE_LEN);
    const lv_point_t root = needle_point(pivot, n, NEEDLE_HUB_R - 6);
    a->x1 = LV_MIN(tip.x, root.x) - 6;
    a->x2 = LV_MAX(tip.x, root.x) + 6;
    a->y1 = LV_MIN(tip.y, root.y) - 6;
    a->y2 = LV_MAX(tip.y, root.y) + 6;
}

/* VU-style ballistics (a lightly damped spring, so the needle swings and
 * settles like a real movement) and minimal invalidation.  Called from the
 * visualizer timer while NEEDLES is the active style. */
static void vis_needles_update(void)
{
    static float vel[2] = { 0.0f, 0.0f };
    static uint32_t peak_tick[2] = { 0, 0 };

    if (visualizer_canvas == NULL) return;

    lv_area_t canvas;
    lv_obj_get_coords(visualizer_canvas, &canvas);

    const float db[2] = { vu_left_db, vu_right_db };
    for (int ch = 0; ch < 2; ch++) {
        float target = (db[ch] - VU_MIN_DBFS) / (VU_MAX_DBFS - VU_MIN_DBFS);
        if (target < 0.0f) target = 0.0f;
        if (target > 1.0f) target = 1.0f;

        const float old = vis_needle_pos[ch];
        vel[ch] += (target - vis_needle_pos[ch]) * 0.20f;
        vel[ch] *= 0.60f;
        float pos = vis_needle_pos[ch] + vel[ch];
        if (pos < -0.02f) { pos = -0.02f; vel[ch] = 0.0f; }     /* rests on the left pin */
        if (pos > 1.03f)  { pos = 1.03f;  vel[ch] = 0.0f; }     /* and the right one      */

        lv_area_t face;
        lv_point_t pivot;
        needle_face_geom(&canvas, ch, &face, &pivot);

        /* Redraw only when the needle moved at least a fraction of a pixel. */
        if (fabsf(pos - old) * (float)NEEDLE_LEN * 1.6f > 0.5f) {
            lv_area_t a, b;
            needle_box(pivot, old, &a);
            needle_box(pivot, pos, &b);
            vis_needle_pos[ch] = pos;
            lv_area_t u = { LV_MIN(a.x1, b.x1), LV_MIN(a.y1, b.y1),
                            LV_MAX(a.x2, b.x2), LV_MAX(a.y2, b.y2) };
            lv_obj_invalidate_area(visualizer_canvas, &u);
        }

        /* Peak lamp: lights near the top of the scale, holds for a moment. */
        bool peak = vis_needle_peak[ch];
        if (target >= 0.92f) {
            peak = true;
            peak_tick[ch] = lv_tick_get();
        } else if (peak && lv_tick_elaps(peak_tick[ch]) > 900) {
            peak = false;
        }
        if (peak != vis_needle_peak[ch]) {
            vis_needle_peak[ch] = peak;
            lv_area_t led;
            needle_peak_led_area(&face, &led);
            lv_obj_invalidate_area(visualizer_canvas, &led);
        }
    }
}

/* ---- STEREO: left channel grows up, right channel grows down ---------- */
#define STEREO_SPAN 190
static int16_t vis_st_h[2][VIS_BAR_COUNT];      /* bar height, px       */
static int16_t vis_st_pk[2][VIS_BAR_COUNT];     /* peak-cap height, px  */

static void vis_draw_stereo(lv_layer_t *layer, const lv_area_t *canvas)
{
    const int cy = canvas->y1 + 212;
    lv_draw_rect_dsc_t dsc;
    lv_area_t a;

    lv_draw_rect_dsc_init(&dsc);                 /* centre line */
    dsc.bg_color = lv_color_hex(UI_ACCENT_DARK);
    dsc.bg_opa = LV_OPA_COVER;
    a.x1 = canvas->x1; a.x2 = canvas->x2;
    a.y1 = cy - 1;     a.y2 = cy;
    vis_rect(layer, &dsc, &a);

    for (int i = 0; i < VIS_BAR_COUNT; i++) {
        a.x1 = canvas->x1 + i * (VIS_BAR_WIDTH + VIS_BAR_GAP);
        a.x2 = a.x1 + VIS_BAR_WIDTH - 1;
        for (int ch = 0; ch < 2; ch++) {
            const int h = vis_st_h[ch][i];
            const float lvl = (float)h / (float)STEREO_SPAN;
            lv_draw_rect_dsc_init(&dsc);
            dsc.bg_color = vis_color(i, lvl, vis_bucket_for(lvl));
            dsc.bg_opa = (ch == 0) ? LV_OPA_COVER : LV_OPA_80;
            dsc.radius = 2;
            if (ch == 0) { a.y1 = cy - 2 - h; a.y2 = cy - 2; }
            else         { a.y1 = cy + 2;     a.y2 = cy + 2 + h; }
            vis_rect(layer, &dsc, &a);

            const int pk = vis_st_pk[ch][i];
            lv_draw_rect_dsc_init(&dsc);
            dsc.bg_color = vis_peak_color();
            dsc.bg_opa = LV_OPA_COVER;
            if (ch == 0) { a.y1 = cy - 2 - pk - 3; a.y2 = a.y1 + 2; }
            else         { a.y2 = cy + 2 + pk + 3; a.y1 = a.y2 - 2; }
            vis_rect(layer, &dsc, &a);
        }
    }

    lv_draw_label_dsc_t td;
    lv_draw_label_dsc_init(&td);
    td.font = &p4_font_14;
    td.color = vis_mix_toward_bg(0xFFFFFF, 140);
    td.text = "LEFT";
    lv_area_t la = { canvas->x1, canvas->y1, canvas->x1 + 60, canvas->y1 + 16 };
    vis_label(layer, &td, &la);
    td.text = "RIGHT";
    lv_area_t ra = { canvas->x1, canvas->y2 - 16, canvas->x1 + 60, canvas->y2 };
    vis_label(layer, &td, &ra);
}

/* Smooths the two per-channel spectra; returns true if anything moved. */
static bool vis_stereo_update(void)
{
    static float lvl[2][VIS_BAR_COUNT];
    static float pk[2][VIS_BAR_COUNT];
    static uint32_t pk_t[2][VIS_BAR_COUNT];
    const uint32_t now = lv_tick_get();
    bool changed = false;

    for (int ch = 0; ch < 2; ch++) {
        const volatile float *src = (ch == 0) ? vis_spectrum_l : vis_spectrum_r;
        for (int i = 0; i < VIS_BAR_COUNT; i++) {
            float t = src[i];
            if (t < 0.015f) t = 0.015f;
            if (t > 1.0f) t = 1.0f;
            lvl[ch][i] += (t - lvl[ch][i]) * (t > lvl[ch][i] ? 0.95f : 0.29f);

            if (lvl[ch][i] > pk[ch][i]) {
                pk[ch][i] = lvl[ch][i];
                pk_t[ch][i] = now;
            } else if ((uint32_t)(now - pk_t[ch][i]) > 120U) {
                pk[ch][i] -= 0.024f;
                if (pk[ch][i] < lvl[ch][i]) pk[ch][i] = lvl[ch][i];
            }

            const int16_t h = (int16_t)(4 + ((int)(lvl[ch][i] * STEREO_SPAN) / 4) * 4);
            const int16_t p = (int16_t)(4 + ((int)(pk[ch][i] * STEREO_SPAN) / 3) * 3);
            if (vis_st_h[ch][i] != h || vis_st_pk[ch][i] != p) {
                vis_st_h[ch][i] = h;
                vis_st_pk[ch][i] = p;
                changed = true;
            }
        }
    }
    return changed;
}

/* ---- DOTS: the spectrum as a column of round lamps per band ------------ */
static void vis_draw_dots(lv_layer_t *layer, const lv_area_t *canvas)
{
    const int base = canvas->y1 + VIS_CANVAS_BASE;
    const int pitch = 12;
    const int dot = 10;
    const int rows = VIS_MAX_H / pitch;
    lv_draw_rect_dsc_t dsc;

    lv_draw_rect_dsc_init(&dsc);
    dsc.bg_opa = LV_OPA_COVER;
    dsc.radius = LV_RADIUS_CIRCLE;

    for (int i = 0; i < VIS_BAR_COUNT; i++) {
        const vis_bar_draw_t *b = &vis_bars[i];
        const int xc = canvas->x1 + i * (VIS_BAR_WIDTH + VIS_BAR_GAP) + VIS_BAR_WIDTH / 2;
        const int lit = (int)(vis_bar_level(b) * (float)rows + 0.5f);
        const int peak_row = (int)(vis_bar_peak(b) * (float)rows + 0.5f);
        lv_area_t a;
        a.x1 = xc - dot / 2;
        a.x2 = a.x1 + dot - 1;

        for (int j = 0; j < lit; j++) {
            int bucket = (j * 6) / rows;
            if (bucket > 5) bucket = 5;
            dsc.bg_color = vis_color(i, (float)(j + 1) / (float)rows, bucket);
            a.y2 = base - j * pitch - 1;
            a.y1 = a.y2 - dot + 1;
            vis_rect(layer, &dsc, &a);
        }
        if (peak_row > lit && peak_row > 0) {
            dsc.bg_color = vis_peak_color();
            a.y2 = base - (peak_row - 1) * pitch - 1;
            a.y1 = a.y2 - dot + 1;
            vis_rect(layer, &dsc, &a);
        }
    }
}

/* Draws the active style. */
static void visualizer_canvas_draw_cb(lv_event_t *e)
{
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_obj_t *obj = (lv_obj_t *)lv_event_get_target(e);
    if (layer == NULL || obj == NULL) return;

    const int64_t t0 = esp_timer_get_time();

    lv_area_t canvas;
    lv_obj_get_coords(obj, &canvas);

    switch (vis_style_active) {
        case VIS_STYLE_LEDS:   vis_draw_leds(layer, &canvas);   break;
        case VIS_STYLE_MIRROR: vis_draw_mirror(layer, &canvas); break;
        case VIS_STYLE_CURVE:  vis_draw_curve(layer, &canvas);  break;
        case VIS_STYLE_SCOPE:  vis_draw_scope(layer, &canvas);  break;
        case VIS_STYLE_RADIAL: vis_draw_radial(layer, &canvas); break;
        case VIS_STYLE_NEEDLES: vis_draw_needles(layer, &canvas); break;
        case VIS_STYLE_STEREO: vis_draw_stereo(layer, &canvas); break;
        case VIS_STYLE_DOTS:   vis_draw_dots(layer, &canvas);   break;
        case VIS_STYLE_BARS:
        default:               vis_draw_bars(layer, &canvas);   break;
    }

    const int64_t took = esp_timer_get_time() - t0;
    vis_draw_us_accum += took;               /* runs once per screen strip */
#if VIS_DIAG
    if (took > vis_draw_us_max) vis_draw_us_max = took;
#endif
}

/* Frame pacing: never ask for a new frame sooner than 1.25x what the last
 * one cost to draw, so heavy styles slow down instead of starving the rest
 * of the display (touch, clock, the screen refresh itself). */
static bool vis_frame_due(void)
{
    static uint32_t last_frame_tick = 0;
    if (vis_draw_us_accum > 0) {             /* the previous frame is on screen by now */
        vis_draw_us_last = vis_draw_us_accum;
        vis_draw_us_accum = 0;
    }
    uint32_t gap = VIS_TIMER_MS;
    const uint32_t cost_ms = (uint32_t)(vis_draw_us_last / 1000);
    if (cost_ms + cost_ms / 4U > gap) gap = cost_ms + cost_ms / 4U;   /* 20 % breathing room */
    if (gap > 250U) gap = 250U;
    if (lv_tick_elaps(last_frame_tick) < gap) return false;
    last_frame_tick = lv_tick_get();
    return true;
}

static void visualizer_timer_cb(lv_timer_t *timer)
{
    (void)timer;

    if (!visualizer_visible) return;

#if VIS_DIAG
    /* Diagnostics for the occasional green flash: log any time the LVGL task
     * fails to run this timer on schedule, plus a heap summary every 5 s.
     * Compare the log timestamps with when the flash happens on screen. */
    {
        static int64_t last_tick_us = 0;
        static int64_t last_report_us = 0;
        static int ticks = 0;
        static int stalls = 0;
        static int64_t worst_gap_us = 0;
        int64_t now_us = esp_timer_get_time();

        if (last_tick_us != 0 && (now_us - last_tick_us) < 2000000) {
            int64_t gap = now_us - last_tick_us;
            if (gap > worst_gap_us) worst_gap_us = gap;
            if (gap > 120000) {
                stalls++;
                ESP_LOGW(TAG, "VISDIAG: LVGL stall - visualizer tick came %d ms late",
                         (int)(gap / 1000));
            }
        }
        last_tick_us = now_us;
        ticks++;

        if (now_us - last_report_us > 5000000) {
            ESP_LOGI(TAG,
                     "VISDIAG: ticks=%d stalls=%d worst_gap=%dms draw_max=%dus "
                     "internal_free=%u largest_internal=%u psram_free=%u",
                     ticks, stalls, (int)(worst_gap_us / 1000), (int)vis_draw_us_max,
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                     (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
            last_report_us = now_us;
            vis_draw_us_max = 0;
            ticks = 0;
            stalls = 0;
            worst_gap_us = 0;
        }
    }
#endif

    /* AUTO: move on to the next style every so often. */
    if (vis_style_selected == VIS_STYLE_AUTO &&
        lv_tick_elaps(vis_auto_tick) > VIS_AUTO_SWITCH_MS) {
        vis_auto_tick = lv_tick_get();
        vis_style_active = (vis_style_active + 1) % VIS_STYLE_COUNT;
        vis_apply_style_ui();
    }

    /* Display-side smoothing: fast attack, deliberately slower decay.
     * This makes transients jump immediately while keeping the spectrum
     * readable instead of making every bar flicker. */
    static float display_level[VIS_BAR_COUNT] = {0};
    static float peak_hold[VIS_BAR_COUNT] = {0};
    static uint32_t peak_timer[VIS_BAR_COUNT] = {0};

    bool changed = false;
    const float attack = 0.95f;
    const float decay = 0.29f;
    const float peak_decay = 0.024f;
    const uint32_t now = (uint32_t)lv_tick_get();

    for (int i = 0; i < VIS_BAR_COUNT; i++) {
        float target = visualizer_spectrum[i];
        if (target < 0.015f) target = 0.015f;
        if (target > 1.0f) target = 1.0f;

        if (target > display_level[i]) {
            display_level[i] += (target - display_level[i]) * attack;
        } else {
            display_level[i] += (target - display_level[i]) * decay;
        }

        if (display_level[i] > peak_hold[i]) {
            peak_hold[i] = display_level[i];
            peak_timer[i] = now;
        } else {
            if ((uint32_t)(now - peak_timer[i]) > 120U) {
                peak_hold[i] -= peak_decay;
                if (peak_hold[i] < display_level[i]) {
                    peak_hold[i] = display_level[i];
                }
            }
        }

        float normalized = display_level[i];
        float peak = peak_hold[i];

        /* Give the outer bars a tiny lift so the display feels broad,
         * while keeping the center capable of producing the tallest peaks. */
        float center = fabsf((float)i - (float)(VIS_BAR_COUNT - 1) * 0.5f) /
                       ((float)(VIS_BAR_COUNT - 1) * 0.5f);
        normalized *= 1.0f - 0.05f * center;
        if (normalized < 0.02f) normalized = 0.02f;
        if (normalized > 1.0f) normalized = 1.0f;

        /* Snap to 4 px steps: imperceptible, but far fewer bars change (and
         * therefore repaint) on each frame. */
        int h = 12 + ((int)(normalized * (float)VIS_MAX_H) / 4) * 4;

        int bucket;
        if (normalized < 0.18f)      bucket = 0;
        else if (normalized < 0.38f) bucket = 1;
        else if (normalized < 0.58f) bucket = 2;
        else if (normalized < 0.76f) bucket = 3;
        else if (normalized < 0.90f) bucket = 4;
        else                         bucket = 5;

        int rh = (int)(h * 0.40f);
        if (rh < 8) rh = 8;
        int peak_h = 12 + ((int)(peak * (float)VIS_MAX_H) / 3) * 3;
        uint8_t refl = normalized > 0.10f ? 1 : 0;

        vis_bar_draw_t *b = &vis_bars[i];
        if (b->h != h || b->rh != rh || b->peak_h != peak_h ||
            b->bucket != bucket || b->refl != refl) {
            b->h = (int16_t)h;
            b->rh = (int16_t)rh;
            b->peak_h = (int16_t)peak_h;
            b->bucket = (uint8_t)bucket;
            b->refl = refl;
            changed = true;
        }
    }

    /* The scope follows new audio samples; every other style follows the bars. */
    static uint32_t last_wave_seq = 0;
    if (vis_style_active == VIS_STYLE_SCOPE) {
        const uint32_t seq = vis_wave_seq;
        changed = (seq != last_wave_seq);
        last_wave_seq = seq;
    } else if (vis_style_active == VIS_STYLE_STEREO) {
        changed = vis_stereo_update();      /* its own left / right spectra */
    }

    /* CYCLE rotates its colours, so it needs a redraw even when the audio is steady. */
    if (vis_palette_active == VIS_PAL_CYCLE) {
        static uint8_t cycle_divider = 0;
        if ((++cycle_divider & 1U) == 0U) {
            changed = true;
        }
    }

    if (vis_style_active == VIS_STYLE_NEEDLES) {
        if (vis_frame_due()) vis_needles_update();   /* invalidates only where the needles moved */
        return;
    }

    /* One invalidation per frame, and none at all when nothing moved. */
    if (changed && visualizer_canvas != NULL && vis_frame_due()) {
        lv_obj_invalidate(visualizer_canvas);
    }
}

/* Small, self-contained 512-point real-input FFT.  This avoids adding an
 * external DSP component and uses the same microphone samples already being
 * captured for the working VU meter.  At 16 kHz the resolution is 31.25 Hz per
 * bin - twice as fine as the old 256-point version, so the lowest bars no
 * longer all show the same two bins.  The transform runs over a rolling
 * 512-sample history, so each 30 ms audio frame refreshes the spectrum. */
#define VIS_FFT_N    512
#define VIS_FFT_GAIN (512.0f / (float)VIS_FFT_N)   /* keeps the old tuning */
#define VIS_MIN_HZ   80.0f
#define VIS_MAX_HZ   7600.0f

/* FFT tables shared by every spectrum computed below. */
static float vis_fft_window[VIS_FFT_N];
static float vis_fft_cos[VIS_FFT_N / 2];
static float vis_fft_sin[VIS_FFT_N / 2];
static bool  vis_fft_ready = false;

static void vis_fft_init(void)
{
    const float pi = 3.14159265358979323846f;
    const int N = VIS_FFT_N;
    for (int i = 0; i < N; i++) {
        vis_fft_window[i] = 0.5f - 0.5f * cosf((2.0f * pi * (float)i) / (float)(N - 1));
    }
    for (int k = 0; k < N / 2; k++) {
        float angle = -2.0f * pi * (float)k / (float)N;
        vis_fft_cos[k] = cosf(angle);
        vis_fft_sin[k] = sinf(angle);
    }
    vis_fft_ready = true;
}

/* One windowed FFT of a sample history, reduced to VIS_BAR_COUNT log bands. */
static void vis_spectrum_from(const float *history, volatile float *out)
{
    static float real[VIS_FFT_N];
    static float imag[VIS_FFT_N];
    static float mag[VIS_FFT_N / 2 + 1];
    const int N = VIS_FFT_N;

    for (int i = 0; i < N; i++) {
        real[i] = history[i] * vis_fft_window[i];
        imag[i] = 0.0f;
    }

    for (int i = 1, j = 0; i < N; i++) {                  /* bit reversal */
        int bit = N >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) {
            float tr = real[i]; real[i] = real[j]; real[j] = tr;
            float ti = imag[i]; imag[i] = imag[j]; imag[j] = ti;
        }
    }

    for (int len = 2; len <= N; len <<= 1) {              /* radix-2 FFT */
        int half = len >> 1;
        int step = N / len;
        for (int i = 0; i < N; i += len) {
            for (int j = 0; j < half; j++) {
                float wr = vis_fft_cos[j * step];
                float wi = vis_fft_sin[j * step];
                int u = i + j;
                int v = u + half;
                float vr = real[v] * wr - imag[v] * wi;
                float vi = real[v] * wi + imag[v] * wr;
                real[v] = real[u] - vr;
                imag[v] = imag[u] - vi;
                real[u] += vr;
                imag[u] += vi;
            }
        }
    }

    for (int k = 0; k <= N / 2; k++) {
        mag[k] = sqrtf(real[k] * real[k] + imag[k] * imag[k]);
    }

    /* Log-spaced bands make bass wider and highs more detailed.  Bands that
     * are narrower than one FFT bin interpolate between neighbouring bins
     * instead of all reading the same one. */
    const float bin_hz = (float)VU_AUDIO_SAMPLE_RATE / (float)N;

    for (int band = 0; band < VIS_BAR_COUNT; band++) {
        float a = (float)band / (float)VIS_BAR_COUNT;
        float b = (float)(band + 1) / (float)VIS_BAR_COUNT;
        float f0 = VIS_MIN_HZ * powf(VIS_MAX_HZ / VIS_MIN_HZ, a);
        float f1 = VIS_MIN_HZ * powf(VIS_MAX_HZ / VIS_MIN_HZ, b);

        int k0 = (int)ceilf(f0 / bin_hz);
        int k1 = (int)floorf(f1 / bin_hz);
        if (k0 < 1) k0 = 1;
        if (k1 > N / 2 - 1) k1 = N / 2 - 1;

        float energy;
        if (k1 >= k0) {
            float sum = 0.0f;
            float max_mag = 0.0f;
            for (int k = k0; k <= k1; k++) {
                if (mag[k] > max_mag) max_mag = mag[k];
                sum += mag[k];
            }
            float avg = sum / (float)(k1 - k0 + 1);
            energy = avg * 0.60f + max_mag * 0.40f;
        } else {
            float pos = sqrtf(f0 * f1) / bin_hz;
            int k = (int)pos;
            if (k < 1) k = 1;
            if (k > N / 2 - 1) k = N / 2 - 1;
            float frac = pos - (float)k;
            if (frac < 0.0f) frac = 0.0f;
            if (frac > 1.0f) frac = 1.0f;
            energy = mag[k] * (1.0f - frac) + mag[k + 1] * frac;
        }

        /* The gain and nonlinear curve are intentionally generous because the
         * built-in mic signal is relatively small at normal listening levels. */
        float norm = energy * 2.0f * VIS_FFT_GAIN;
        if (norm > 1.0f) norm = 1.0f;

        /* Expand quiet signals while retaining strong transient peaks. */
        norm = powf(norm, 0.48f);
        if (norm < 0.018f) norm = 0.018f;

        /* A little frequency weighting keeps bass from dominating the entire
         * screen and lets the upper bands visibly participate. */
        float freq_weight = 0.90f + 0.22f * ((float)band / (float)(VIS_BAR_COUNT - 1));
        norm *= freq_weight;
        if (norm > 1.0f) norm = 1.0f;

        out[band] = norm;
    }
}

/* Strong transients lift a whole spectrum very briefly. */
static void vis_apply_peak_lift(volatile float *spec)
{
    float peak_norm = visualizer_peak;
    if (peak_norm > 1.0f) peak_norm = 1.0f;
    if (peak_norm > 0.75f) {
        /* Strong mic transients lift the whole spectrum very briefly, but
         * the FFT still determines which individual bands rise highest. */
        float lift = (peak_norm - 0.75f) * 0.8f;
        for (int band = 0; band < VIS_BAR_COUNT; band++) {
            float v = spec[band] + lift;
            if (v > 1.0f) v = 1.0f;
            spec[band] = v;
        }
    }
}

static void visualizer_analyze_audio(const int16_t *audio_buffer, int frame_count)
{
    static float history[VIS_FFT_N];      /* left + right mixed */
    static float hist_l[VIS_FFT_N];
    static float hist_r[VIS_FFT_N];
    const int N = VIS_FFT_N;

    if (audio_buffer == NULL || frame_count <= 0) return;
    if (!vis_fft_ready) vis_fft_init();

    /* Slide the newest samples into the rolling histories. */
    int new_samples = frame_count > N ? N : frame_count;
    int keep = N - new_samples;
    if (keep > 0) {
        memmove(history, history + new_samples, (size_t)keep * sizeof(float));
        memmove(hist_l, hist_l + new_samples, (size_t)keep * sizeof(float));
        memmove(hist_r, hist_r + new_samples, (size_t)keep * sizeof(float));
    }
    for (int i = 0; i < new_samples; i++) {
        int src = frame_count - new_samples + i;
        float left = (float)audio_buffer[src * 2] / 32768.0f;
        float right = (float)audio_buffer[src * 2 + 1] / 32768.0f;
        history[keep + i] = (left + right) * 0.5f;
        hist_l[keep + i] = left;
        hist_r[keep + i] = right;
    }

    /* Waveform for the scope style: 128 points from the newest samples, with the
     * DC offset removed and automatic scaling so quiet room audio still fills
     * the screen. */
    {
        static float wave_peak = 0.05f;
        float mean = 0.0f;
        for (int i = 0; i < N; i++) mean += history[i];
        mean /= (float)N;

        float maxabs = 0.0f;
        for (int i = 0; i < N; i++) {
            const float a = fabsf(history[i] - mean);
            if (a > maxabs) maxabs = a;
        }
        wave_peak = (maxabs > wave_peak) ? maxabs : wave_peak * 0.992f;
        if (wave_peak < 0.02f) wave_peak = 0.02f;
        const float scale = 0.9f / wave_peak;

        const int group = N / VIS_WAVE_POINTS;
        for (int p = 0; p < VIS_WAVE_POINTS; p++) {
            float sum = 0.0f;
            for (int g = 0; g < group; g++) sum += history[p * group + g] - mean;
            float v = (sum / (float)group) * scale;
            if (v > 1.0f) v = 1.0f;
            if (v < -1.0f) v = -1.0f;
            vis_wave[p] = (int8_t)(v * 127.0f);
        }
        vis_wave_seq++;
    }

    vis_spectrum_from(history, visualizer_spectrum);
    vis_apply_peak_lift(visualizer_spectrum);

    /* Separate left / right spectra cost two extra FFTs, so only while the
     * STEREO style is actually on screen. */
    if (vis_style_active == VIS_STYLE_STEREO) {
        vis_spectrum_from(hist_l, vis_spectrum_l);
        vis_spectrum_from(hist_r, vis_spectrum_r);
        vis_apply_peak_lift(vis_spectrum_l);
        vis_apply_peak_lift(vis_spectrum_r);
    }
}

static void create_visualizer(lv_obj_t *screen)
{
    const int bars_w = VIS_BAR_COUNT * VIS_BAR_WIDTH + (VIS_BAR_COUNT - 1) * VIS_BAR_GAP;

    visualizer_overlay = lv_obj_create(screen);
    lv_obj_remove_style_all(visualizer_overlay);
    lv_obj_set_size(visualizer_overlay, 1024, 600);
    lv_obj_set_pos(visualizer_overlay, 0, 0);
    lv_obj_set_style_bg_color(visualizer_overlay, lv_color_hex(UI_DEEP), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(visualizer_overlay, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(visualizer_overlay, 2, LV_PART_MAIN);
    lv_obj_set_style_border_color(visualizer_overlay, lv_color_hex(UI_EDGE), LV_PART_MAIN);
    lv_obj_clear_flag(visualizer_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(visualizer_overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(visualizer_overlay, visualizer_overlay_event_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *title = lv_label_create(visualizer_overlay);
    lv_label_set_text(title, "P4-LMS  \xE2\x80\xA2  MUSIC VISUALIZER");
    lv_obj_set_style_text_color(title, lv_color_hex(UI_ACCENT_GLOW), 0);
    lv_obj_set_style_text_font(title, &p4_font_28, 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 34, 22);

    /* What is playing, so the full-screen view never hides the track. */
    visualizer_track_label = lv_label_create(visualizer_overlay);
    lv_label_set_text(visualizer_track_label, "");
    lv_obj_set_width(visualizer_track_label, 800);
    lv_label_set_long_mode(visualizer_track_label, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_color(visualizer_track_label, lv_color_hex(UI_TEXT_SOFT), 0);
    lv_obj_set_style_text_font(visualizer_track_label, &p4_font_16, 0);
    label_one_line(visualizer_track_label);
    lv_obj_align(visualizer_track_label, LV_ALIGN_TOP_LEFT, 36, 62);

    lv_obj_t *hint = lv_label_create(visualizer_overlay);
    lv_label_set_text(hint, "TAP TO RETURN");
    lv_obj_set_style_text_color(hint, lv_color_hex(UI_TEXT_DIM), 0);
    lv_obj_set_style_text_font(hint, &p4_font_14, 0);
    lv_obj_align(hint, LV_ALIGN_TOP_RIGHT, -34, 34);

    /* Tap this chip to change the visualizer style. */
    lv_obj_t *chip = lv_obj_create(visualizer_overlay);
    lv_obj_remove_style_all(chip);
    lv_obj_set_size(chip, 250, 42);
    lv_obj_align(chip, LV_ALIGN_BOTTOM_MID, -80, -14);
    lv_obj_set_style_bg_color(chip, lv_color_hex(UI_BUTTON), 0);
    lv_obj_set_style_bg_opa(chip, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(chip, 10, 0);
    lv_obj_set_style_border_width(chip, 1, 0);
    lv_obj_set_style_border_color(chip, lv_color_hex(UI_EDGE), 0);
    lv_obj_set_style_bg_color(chip, lv_color_hex(UI_ACCENT_DARK), LV_STATE_PRESSED);
    lv_obj_clear_flag(chip, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(chip, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(chip, 8);
    lv_obj_add_event_cb(chip, vis_style_chip_event_cb, LV_EVENT_CLICKED, (void *)(intptr_t)1);

    /* PREV: one style back. */
    lv_obj_t *prev_chip = lv_obj_create(visualizer_overlay);
    lv_obj_remove_style_all(prev_chip);
    lv_obj_set_size(prev_chip, 130, 42);
    lv_obj_align(prev_chip, LV_ALIGN_BOTTOM_MID, -285, -14);
    lv_obj_set_style_bg_color(prev_chip, lv_color_hex(UI_BUTTON), 0);
    lv_obj_set_style_bg_opa(prev_chip, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(prev_chip, 10, 0);
    lv_obj_set_style_border_width(prev_chip, 1, 0);
    lv_obj_set_style_border_color(prev_chip, lv_color_hex(UI_EDGE), 0);
    lv_obj_set_style_bg_color(prev_chip, lv_color_hex(UI_ACCENT_DARK), LV_STATE_PRESSED);
    lv_obj_clear_flag(prev_chip, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(prev_chip, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(prev_chip, 8);
    lv_obj_add_event_cb(prev_chip, vis_style_chip_event_cb, LV_EVENT_CLICKED, (void *)(intptr_t)-1);
    lv_obj_t *prev_lbl = lv_label_create(prev_chip);
    lv_label_set_text(prev_lbl, LV_SYMBOL_LEFT "  PREV");
    lv_obj_set_style_text_color(prev_lbl, lv_color_hex(UI_ACCENT_GLOW), 0);
    lv_obj_set_style_text_font(prev_lbl, &p4_font_14, 0);
    lv_obj_center(prev_lbl);

    vis_style_chip_label = lv_label_create(chip);
    lv_label_set_text(vis_style_chip_label, "STYLE");
    lv_obj_set_style_text_color(vis_style_chip_label, lv_color_hex(UI_ACCENT_GLOW), 0);
    lv_obj_set_style_text_font(vis_style_chip_label, &p4_font_14, 0);
    lv_obj_center(vis_style_chip_label);

    /* Second chip: colour palette. */
    lv_obj_t *color_chip = lv_obj_create(visualizer_overlay);
    lv_obj_remove_style_all(color_chip);
    lv_obj_set_size(color_chip, 190, 42);
    lv_obj_align(color_chip, LV_ALIGN_BOTTOM_MID, 150, -14);
    lv_obj_set_style_bg_color(color_chip, lv_color_hex(UI_BUTTON), 0);
    lv_obj_set_style_bg_opa(color_chip, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(color_chip, 10, 0);
    lv_obj_set_style_border_width(color_chip, 1, 0);
    lv_obj_set_style_border_color(color_chip, lv_color_hex(UI_EDGE), 0);
    lv_obj_set_style_bg_color(color_chip, lv_color_hex(UI_ACCENT_DARK), LV_STATE_PRESSED);
    lv_obj_clear_flag(color_chip, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(color_chip, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(color_chip, 8);
    lv_obj_add_event_cb(color_chip, vis_color_chip_event_cb, LV_EVENT_CLICKED, NULL);

    vis_color_chip_label = lv_label_create(color_chip);
    lv_label_set_text(vis_color_chip_label, "COLOR");
    lv_obj_set_style_text_color(vis_color_chip_label, lv_color_hex(UI_ACCENT_GLOW), 0);
    lv_obj_set_style_text_font(vis_color_chip_label, &p4_font_14, 0);
    lv_obj_center(vis_color_chip_label);

    /* Baseline sits directly under the bars (bars end at y = 400). */
    lv_obj_t *baseline = lv_obj_create(visualizer_overlay);
    lv_obj_remove_style_all(baseline);
    lv_obj_set_size(baseline, bars_w, 2);
    lv_obj_set_pos(baseline, VIS_BAR_START_X, 400);
    lv_obj_set_style_bg_color(baseline, lv_color_hex(UI_ACCENT_DARK), 0);
    lv_obj_set_style_bg_opa(baseline, LV_OPA_COVER, 0);
    vis_axis_count = 0;
    vis_axis_objs[vis_axis_count++] = baseline;

    /* Bars, reflections and peak markers: one custom-drawn object. */
    for (int i = 0; i < VIS_BAR_COUNT; i++) {
        vis_bars[i].h = 12;
        vis_bars[i].rh = 8;
        vis_bars[i].peak_h = 12;
        vis_bars[i].bucket = 0;
        vis_bars[i].refl = 0;
    }
    visualizer_canvas = lv_obj_create(visualizer_overlay);
    lv_obj_remove_style_all(visualizer_canvas);
    lv_obj_set_size(visualizer_canvas, VIS_CANVAS_W, VIS_CANVAS_H);
    lv_obj_set_pos(visualizer_canvas, VIS_CANVAS_X, VIS_CANVAS_Y);
    lv_obj_clear_flag(visualizer_canvas, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(visualizer_canvas, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(visualizer_canvas, visualizer_canvas_draw_cb,
                        LV_EVENT_DRAW_MAIN, NULL);

    /* Frequency axis.  The bands are log-spaced between VIS_MIN_HZ and
     * VIS_MAX_HZ, so each label is placed with the same maths. */
    static const struct { float hz; const char *text; } axis[] = {
        { 100.0f,  "100 Hz" },
        { 250.0f,  "250 Hz" },
        { 500.0f,  "500 Hz" },
        { 1000.0f, "1 kHz"  },
        { 2000.0f, "2 kHz"  },
        { 4000.0f, "4 kHz"  }
    };
    for (size_t i = 0; i < sizeof(axis) / sizeof(axis[0]); i++) {
        float band_pos = (float)VIS_BAR_COUNT *
                         logf(axis[i].hz / VIS_MIN_HZ) /
                         logf(VIS_MAX_HZ / VIS_MIN_HZ);
        int cx = VIS_BAR_START_X +
                 (int)(band_pos * (float)(VIS_BAR_WIDTH + VIS_BAR_GAP)) +
                 VIS_BAR_WIDTH / 2;

        lv_obj_t *tick = lv_obj_create(visualizer_overlay);
        lv_obj_remove_style_all(tick);
        lv_obj_set_size(tick, 2, 6);
        lv_obj_set_pos(tick, cx - 1, 404);
        lv_obj_set_style_bg_color(tick, lv_color_hex(UI_ACCENT_DARK), 0);
        lv_obj_set_style_bg_opa(tick, LV_OPA_COVER, 0);
        if (vis_axis_count < 16) vis_axis_objs[vis_axis_count++] = tick;

        lv_obj_t *label = lv_label_create(visualizer_overlay);
        lv_label_set_text(label, axis[i].text);
        lv_obj_set_style_text_color(label, lv_color_hex(UI_TEXT_SOFT), 0);
        lv_obj_set_style_text_font(label, &p4_font_14, 0);
        lv_obj_set_width(label, 70);
        lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_pos(label, cx - 35, 412);
        if (vis_axis_count < 16) vis_axis_objs[vis_axis_count++] = label;
    }

    vis_apply_style_ui();

    lv_obj_add_flag(visualizer_overlay, LV_OBJ_FLAG_HIDDEN);
    visualizer_visible = false;
    visualizer_timer = lv_timer_create(visualizer_timer_cb, VIS_TIMER_MS, NULL);
}

static esp_err_t microphone_init(void)
{
    esp_codec_dev_sample_info_t sample_info = {
        .sample_rate = VU_AUDIO_SAMPLE_RATE,
        .channel = VU_AUDIO_CHANNELS,
        .bits_per_sample = 16,
    };

    microphone_codec = bsp_audio_codec_microphone_init();
    if (microphone_codec == NULL) {
        ESP_LOGE(TAG, "Microphone codec init failed");
        return ESP_FAIL;
    }

    esp_err_t ret = esp_codec_dev_open(
        microphone_codec,
        &sample_info
    );

    if (ret != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Microphone codec open failed: %s",
            esp_err_to_name(ret)
        );
        return ret;
    }

    /* Waveshare's own microphone example uses 24 dB as its
     * default input gain. */
    ret = esp_codec_dev_set_in_gain(
        microphone_codec,
        40
    );

    if (ret != ESP_OK) {
        ESP_LOGW(
            TAG,
            "Microphone gain setup failed: %s",
            esp_err_to_name(ret)
        );
    }

    microphone_ready = true;

    ESP_LOGI(
        TAG,
        "Real microphone VU input ready: 16 kHz / stereo / 16-bit"
    );

    return ESP_OK;
}

static void microphone_task(void *arg)
{
    (void)arg;

    int16_t *audio_buffer =
        malloc(VU_AUDIO_BUFFER_BYTES);

    if (audio_buffer == NULL) {
        ESP_LOGE(
            TAG,
            "Unable to allocate microphone buffer"
        );
        microphone_ready = false;
        vTaskDelete(NULL);
        return;
    }

    uint32_t log_counter = 0;

    while (true) {
        esp_err_t ret = esp_codec_dev_read(
            microphone_codec,
            audio_buffer,
            VU_AUDIO_BUFFER_BYTES
        );

        if (ret != ESP_OK) {
            ESP_LOGW(
                TAG,
                "Microphone read failed: %s",
                esp_err_to_name(ret)
            );
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        /* Mic sensitivity: boost the samples (with clipping) before the
         * meters and visualizers see them. */
        {
            const int step = mic_gain_step;
            if (step > 0 && step < 4) {
                const int32_t g = mic_gain_q8[step];
                const int n = VU_AUDIO_SAMPLES * VU_AUDIO_CHANNELS;
                for (int k = 0; k < n; k++) {
                    int32_t v = ((int32_t)audio_buffer[k] * g) >> 8;
                    audio_buffer[k] = (int16_t)(v > 32767 ? 32767 : (v < -32768 ? -32768 : v));
                }
            }
        }

        const int frame_count = VU_AUDIO_SAMPLES;
        int64_t sum_l = 0;
        int64_t sum_r = 0;
        int32_t peak_l = 0;
        int32_t peak_r = 0;

        /* esp_codec_dev supplies interleaved stereo PCM when opened
         * with channel=2: L,R,L,R,... */
        for (int i = 0; i < frame_count; i++) {
            int32_t l = audio_buffer[i * 2];
            int32_t r = audio_buffer[i * 2 + 1];

            int32_t al = l < 0 ? -l : l;
            int32_t ar = r < 0 ? -r : r;

            if (al > peak_l) peak_l = al;
            if (ar > peak_r) peak_r = ar;

            sum_l += (int64_t)l * l;
            sum_r += (int64_t)r * r;
        }

        float rms_l = sqrtf(
            (float)sum_l / (float)frame_count
        );
        float rms_r = sqrtf(
            (float)sum_r / (float)frame_count
        );

        float db_l = rms_l > 1.0f
            ? 20.0f * log10f(rms_l / 32768.0f)
            : VU_MIN_DBFS;

        float db_r = rms_r > 1.0f
            ? 20.0f * log10f(rms_r / 32768.0f)
            : VU_MIN_DBFS;

        int level_l = vu_db_to_segments(db_l);
        int level_r = vu_db_to_segments(db_r);

        /* Blend a little peak information into the RMS reading so
         * transients look lively without making the meter twitch. */
        if (peak_l > 0) {
            float peak_db_l =
                20.0f * log10f((float)peak_l / 32768.0f);
            int peak_level_l =
                vu_db_to_segments(peak_db_l);

            if (peak_level_l > level_l) {
                level_l =
                    (level_l * 3 + peak_level_l) / 4;
            }
        }

        if (peak_r > 0) {
            float peak_db_r =
                20.0f * log10f((float)peak_r / 32768.0f);
            int peak_level_r =
                vu_db_to_segments(peak_db_r);

            if (peak_level_r > level_r) {
                level_r =
                    (level_r * 3 + peak_level_r) / 4;
            }
        }

        vu_left_target = level_l;
        vu_right_target = level_r;
        vu_left_db = db_l;
        vu_right_db = db_r;

        /* Feed the same microphone buffer into the FFT used by the
         * visualizer.  The VU calculations above are unchanged. */
        float base_level = ((float)level_l + (float)level_r) /
                           (2.0f * (float)VU_SEGMENT_COUNT);
        float peak_norm = ((float)peak_l > (float)peak_r ?
                           (float)peak_l : (float)peak_r) / 32768.0f;
        if (peak_norm > 1.0f) peak_norm = 1.0f;

        visualizer_level = base_level;
        visualizer_peak = peak_norm;

        /* The spectrum is only needed while the visualizer is on screen. */
        if (visualizer_visible) {
            visualizer_analyze_audio(audio_buffer, frame_count);
        }

        if ((++log_counter % 100U) == 0U) {
            ESP_LOGI(
                TAG,
                "Mic VU: L=%d seg R=%d seg | peak L=%ld R=%ld",
                level_l,
                level_r,
                (long)peak_l,
                (long)peak_r
            );
        }
    }
}


