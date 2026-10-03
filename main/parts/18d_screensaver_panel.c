/*
 * P4-LMS - Settings > Screensaver panel
 *
 * Part of main.c.  This file is #included by main.c (in order) and is not
 * compiled on its own, so it shares main.c's includes and static names.
 *
 * The same settings are on the web page; both save to NVS through
 * web_settings_save() (part 19).
 */

static bool web_settings_save(void);           /* 19_web_settings.c */

#define SSP_CHOICES 5
static lv_obj_t *ssp_play_btn[SSP_CHOICES] = { NULL };
static lv_obj_t *ssp_pause_btn[SSP_CHOICES] = { NULL };
static lv_obj_t *ssp_start_lbl = NULL;
static lv_obj_t *ssp_deep_lbl = NULL;
static lv_obj_t *ssp_slider[3] = { NULL };     /* album art, standby, deep dim */
static lv_obj_t *ssp_slider_lbl[3] = { NULL };

/* Minutes offered by the - / + steppers. */
static const uint8_t ssp_minutes[] = { 1, 2, 3, 5, 10, 15, 20, 30, 45, 60, 90, 120, 180, 240 };
#define SSP_MINUTES_COUNT (sizeof(ssp_minutes) / sizeof(ssp_minutes[0]))

static void ssp_save(void)
{
    if (!web_settings_save()) ui_toast("Could not save the screensaver settings");
}

static void ssp_style_choice(lv_obj_t *b, bool on)
{
    lv_obj_set_style_bg_color(b, lv_color_hex(on ? UI_ACCENT : UI_BUTTON), LV_PART_MAIN);
    lv_obj_set_style_border_color(b, lv_color_hex(on ? UI_ACCENT : UI_EDGE), LV_PART_MAIN);
    lv_obj_set_style_text_color(lv_obj_get_child(b, 0), lv_color_hex(on ? UI_ON_ACCENT : UI_TEXT), 0);
}

static void ssp_refresh(void)
{
    for (int i = 0; i < SSP_CHOICES; i++) {
        ssp_style_choice(ssp_play_btn[i], ss_play_mode == i);
        ssp_style_choice(ssp_pause_btn[i], ss_pause_mode == i);
    }
    char t[24];
    snprintf(t, sizeof(t), "%u min", (unsigned)ss_timeout_min);
    lv_label_set_text(ssp_start_lbl, t);
    snprintf(t, sizeof(t), "%u min", (unsigned)ss_deep_min);
    lv_label_set_text(ssp_deep_lbl, t);
    const int v[3] = { bl_art_pct, bl_standby_pct, bl_deep_pct };
    for (int i = 0; i < 3; i++) {
        if (!lv_slider_is_dragged(ssp_slider[i])) lv_slider_set_value(ssp_slider[i], v[i], LV_ANIM_OFF);
        snprintf(t, sizeof(t), "%d%%", v[i]);
        lv_label_set_text(ssp_slider_lbl[i], t);
    }
}

static void ssp_choice_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    const int which = (int)(intptr_t)lv_event_get_user_data(e);    /* 0..2 play, 10..13 pause */
    if (which < 10) ss_play_mode = which;
    else            ss_pause_mode = which - 10;
    /* PREVIEW shows the look just tapped. */
    static const int play_view[SSP_CHOICES]  = { -1, SS_VIEW_ART, SS_VIEW_WALL, SS_VIEW_BIGTEXT, SS_VIEW_VU };
    static const int pause_view[SSP_CHOICES] = { SS_VIEW_CLOCK, SS_VIEW_CLOCK_COVER, SS_VIEW_ANALOG,
                                                 SS_VIEW_WALL, SS_VIEW_WEATHER };
    ss_preview_view = (which < 10) ? play_view[which] : pause_view[which - 10];
    if (ss_preview_view == SS_VIEW_CLOCK_COVER && !ss_have_track) ss_preview_view = SS_VIEW_CLOCK;
    ssp_refresh();
    ssp_save();
}

/* Next / previous entry of ssp_minutes from the current value. */
static uint32_t ssp_step_minutes(uint32_t cur, int dir, uint32_t max)
{
    if (dir > 0) {
        for (size_t i = 0; i < SSP_MINUTES_COUNT; i++) {
            if (ssp_minutes[i] > cur && ssp_minutes[i] <= max) return ssp_minutes[i];
        }
        return cur;
    }
    for (size_t i = SSP_MINUTES_COUNT; i-- > 0;) {
        if (ssp_minutes[i] < cur) return ssp_minutes[i];
    }
    return cur;
}

static void ssp_step_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    const int which = (int)(intptr_t)lv_event_get_user_data(e);    /* 0/1 start -/+, 2/3 deep -/+ */
    const int dir = (which & 1) ? 1 : -1;
    if (which < 2) ss_timeout_min = ssp_step_minutes(ss_timeout_min, dir, 120);
    else           ss_deep_min    = ssp_step_minutes(ss_deep_min, dir, 240);
    ssp_refresh();
    ssp_save();
}

static void ssp_slider_event_cb(lv_event_t *e)
{
    const lv_event_code_t code = lv_event_get_code(e);
    const int which = (int)(intptr_t)lv_event_get_user_data(e);
    int v = (int)lv_slider_get_value(ssp_slider[which]);
    if (v < 1) v = 1;
    if (which == 0)      bl_art_pct = v;
    else if (which == 1) bl_standby_pct = v;
    else                 bl_deep_pct = v;
    char t[16];
    snprintf(t, sizeof(t), "%d%%", v);
    lv_label_set_text(ssp_slider_lbl[which], t);
    if (code == LV_EVENT_RELEASED) ssp_save();       /* not on every step of a drag */
}

static void ssp_close_event_cb(lv_event_t *e);
static void ssp_preview_event_cb(lv_event_t *e);

static void ssp_hide(void)
{
    if (ssp_overlay != NULL) lv_obj_add_flag(ssp_overlay, LV_OBJ_FLAG_HIDDEN);
}

static void ssp_close_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) ssp_hide();
}

static void ssp_preview_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    ssp_hide();
    screensaver_preview_show();
}

static void ssp_show(void)
{
    if (ssp_overlay == NULL) return;
    nav_hide_panels();
    ssp_refresh();
    ui_show_panel(ssp_overlay);
}

static lv_obj_t *ssp_label(lv_obj_t *parent, const char *text, const lv_font_t *font,
                           uint32_t color, int x, int y)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_obj_set_pos(l, x, y);
    return l;
}

static lv_obj_t *ssp_stepper(lv_obj_t *p, int y, int user_base)
{
    const int x = 480;
    lv_obj_t *minus = create_button(p, LV_SYMBOL_MINUS, 64, 46);
    lv_obj_set_pos(minus, x, y);
    lv_obj_add_event_cb(minus, ssp_step_event_cb, LV_EVENT_CLICKED, (void *)(intptr_t)user_base);
    lv_obj_t *plus = create_button(p, LV_SYMBOL_PLUS, 64, 46);
    lv_obj_set_pos(plus, x + 64 + 120, y);
    lv_obj_add_event_cb(plus, ssp_step_event_cb, LV_EVENT_CLICKED, (void *)(intptr_t)(user_base + 1));
    lv_obj_t *l = lv_label_create(p);
    lv_label_set_text(l, "");
    lv_obj_set_width(l, 120);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(l, &p4_font_18, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(UI_TEXT), 0);
    lv_obj_set_pos(l, x + 64, y + 12);
    return l;
}

static void ssp_create(lv_obj_t *screen)
{
    lv_obj_t *p = nav_panel_create(screen, "SCREENSAVER", ssp_close_event_cb);
    ssp_overlay = p;
    ui_add_settings_back(p);

    static const char *const play_names[SSP_CHOICES]  = { "Now Playing", "Album art", "Cover wall",
                                                          "Big text", "VU meters" };
    static const char *const pause_names[SSP_CHOICES] = { "Clock", "Clock + cover", "Analog clock",
                                                          "Cover wall", "Weather" };

    /* PREVIEW up in the title bar, left of BACK. */
    lv_obj_t *prev = create_button(p, LV_SYMBOL_EYE_OPEN "  PREVIEW", 150, 50);
    lv_obj_align(prev, LV_ALIGN_TOP_RIGHT, -362, 14);
    lv_obj_add_event_cb(prev, ssp_preview_event_cb, LV_EVENT_CLICKED, NULL);

    ssp_label(p, "While music plays", &p4_font_18, UI_TEXT, 30, 84);
    ssp_label(p, "When paused or stopped", &p4_font_18, UI_TEXT, 30, 172);
    for (int i = 0; i < SSP_CHOICES; i++) {
        ssp_play_btn[i] = create_button(p, play_names[i], 164, 46);
        lv_obj_set_pos(ssp_play_btn[i], 30 + i * 174, 110);
        lv_obj_add_event_cb(ssp_play_btn[i], ssp_choice_event_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        ssp_pause_btn[i] = create_button(p, pause_names[i], 164, 46);
        lv_obj_set_pos(ssp_pause_btn[i], 30 + i * 174, 198);
        lv_obj_add_event_cb(ssp_pause_btn[i], ssp_choice_event_cb, LV_EVENT_CLICKED, (void *)(intptr_t)(i + 10));
    }

    ssp_label(p, "Start after", &p4_font_18, UI_TEXT, 30, 270);
    ssp_label(p, "minutes without a touch", &p4_font_14, UI_TEXT_DIM, 30, 294);
    ssp_start_lbl = ssp_stepper(p, 262, 0);
    ssp_label(p, "Deep dim after", &p4_font_18, UI_TEXT, 30, 328);
    ssp_label(p, "minutes on the screensaver", &p4_font_14, UI_TEXT_DIM, 30, 352);
    ssp_deep_lbl = ssp_stepper(p, 320, 2);

    ssp_label(p, "Brightness", &p4_font_18, UI_TEXT, 30, 390);
    static const char *const bl_names[3] = { "While playing", "Standby", "Deep dim" };
    for (int i = 0; i < 3; i++) {
        const int y = 390 + i * 38;
        ssp_label(p, bl_names[i], &p4_font_14, UI_TEXT_SOFT, 340, y);
        lv_obj_t *s = lv_slider_create(p);
        lv_slider_set_range(s, 1, 100);
        lv_obj_set_size(s, 330, 12);
        lv_obj_set_pos(s, 480, y + 4);
        lv_obj_set_style_bg_color(s, lv_color_hex(UI_SLIDER_BG), LV_PART_MAIN);
        lv_obj_set_style_bg_color(s, lv_color_hex(UI_TERTIARY), LV_PART_INDICATOR);
        lv_obj_set_style_bg_color(s, lv_color_hex(UI_ACCENT_BRIGHT), LV_PART_KNOB);
        lv_obj_set_ext_click_area(s, 12);
        lv_obj_add_event_cb(s, ssp_slider_event_cb, LV_EVENT_VALUE_CHANGED, (void *)(intptr_t)i);
        lv_obj_add_event_cb(s, ssp_slider_event_cb, LV_EVENT_RELEASED, (void *)(intptr_t)i);
        ssp_slider[i] = s;
        ssp_slider_lbl[i] = ssp_label(p, "", &p4_font_16, UI_TEXT, 836, y - 2);
    }
}

/* ======================================================================
 * Settings > Visualizer: pick the style and colours; OPEN shows it.
 * (A tap on the VU meter on Now Playing still opens the chosen one.)
 * ====================================================================== */
static lv_obj_t *vsp_style_btn[VIS_STYLE_COUNT + 1];
static lv_obj_t *vsp_pal_btn[VIS_PAL_COUNT];
static lv_obj_t *vsp_gain_btn[4];
static lv_obj_t *vsp_vu_btn = NULL;

static void vsp_refresh(void)
{
    for (int i = 0; i <= VIS_STYLE_COUNT; i++) {
        if (vsp_style_btn[i] != NULL) ssp_style_choice(vsp_style_btn[i], i == vis_style_selected);
    }
    for (int i = 0; i < VIS_PAL_COUNT; i++) {
        if (vsp_pal_btn[i] != NULL) ssp_style_choice(vsp_pal_btn[i], i == vis_palette_selected);
    }
    for (int i = 0; i < 4; i++) {
        if (vsp_gain_btn[i] != NULL) ssp_style_choice(vsp_gain_btn[i], i == mic_gain_step);
    }
    if (vsp_vu_btn != NULL) {
        ssp_style_choice(vsp_vu_btn, vu_meter_shown);
        lv_label_set_text(lv_obj_get_child(vsp_vu_btn, 0), vu_meter_shown ? "VU METER  ON" : "VU METER  OFF");
    }
}

/* VU meter on Now Playing on / off. */
static void vsp_vu_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    vu_meter_shown = !vu_meter_shown;
    vu_apply_shown();
    vu_shown_save();
    vsp_refresh();
}

static void vsp_gain_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    const int g = (int)(intptr_t)lv_event_get_user_data(e);
    if (g < 0 || g > 3) return;
    mic_gain_step = g;
    mic_gain_save();
    vsp_refresh();
}

static void vsp_style_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    const int choice = (int)(intptr_t)lv_event_get_user_data(e);
    if (choice < 0 || choice > VIS_STYLE_COUNT) return;
    vis_style_selected = choice;
    if (choice == VIS_STYLE_AUTO) vis_auto_tick = lv_tick_get();
    else                          vis_style_active = choice;
    vis_apply_style_ui();
    visualizer_style_save();
    vsp_refresh();
}

static void vsp_pal_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    const int choice = (int)(intptr_t)lv_event_get_user_data(e);
    if (choice < 0 || choice >= VIS_PAL_COUNT) return;
    vis_palette_selected = choice;
    vis_palette_active = choice;
    vis_apply_style_ui();
    visualizer_style_save();
    vsp_refresh();
}

static void vsp_hide(void)
{
    if (vsp_overlay != NULL) lv_obj_add_flag(vsp_overlay, LV_OBJ_FLAG_HIDDEN);
}

static void vsp_close_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) vsp_hide();
}

/* PREVIEW: the chosen style full screen; a tap comes back here. */
static void vsp_open_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    vsp_hide();
    vis_return_to_settings = true;
    visualizer_set_visible(true);
}

static void vsp_show(void)
{
    if (vsp_overlay == NULL) return;
    nav_hide_panels();
    vsp_refresh();
    ui_show_panel(vsp_overlay);
}

static void vsp_create(lv_obj_t *screen)
{
    lv_obj_t *p = nav_panel_create(screen, "VISUALIZER", vsp_close_event_cb);
    vsp_overlay = p;
    ui_add_settings_back(p);

    ssp_label(p, "Style", &p4_font_18, UI_TEXT, 30, 84);
    for (int i = 0; i <= VIS_STYLE_COUNT; i++) {
        lv_obj_t *b = create_button(p, vis_style_names[i], 164, 46);
        lv_obj_set_pos(b, 30 + (i % 5) * 174, 110 + (i / 5) * 56);
        lv_obj_add_event_cb(b, vsp_style_event_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        vsp_style_btn[i] = b;
    }

    const int py = 110 + ((VIS_STYLE_COUNT + 5) / 5) * 56 + 34;
    ssp_label(p, "Colours", &p4_font_18, UI_TEXT, 30, py - 26);
    for (int i = 0; i < VIS_PAL_COUNT; i++) {
        lv_obj_t *b = create_button(p, vis_palette_names[i], 118, 46);
        lv_obj_set_pos(b, 30 + i * 124, py);
        lv_obj_add_event_cb(b, vsp_pal_event_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        /* a strip of the palette's loud colour under the name */
        lv_obj_t *strip = lv_obj_create(b);
        lv_obj_remove_style_all(strip);
        lv_obj_set_size(strip, 70, 4);
        lv_obj_align(strip, LV_ALIGN_BOTTOM_MID, 0, 4);
        lv_obj_set_style_bg_color(strip, lv_color_hex(vis_palette_colors[i][4]), 0);
        lv_obj_set_style_bg_opa(strip, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(strip, 2, 0);
        lv_obj_clear_flag(strip, LV_OBJ_FLAG_CLICKABLE);
        vsp_pal_btn[i] = b;
    }

    /* Mic sensitivity: livelier meters when the music is quiet. */
    ssp_label(p, "Mic sensitivity", &p4_font_18, UI_TEXT, 30, py + 64);
    ssp_label(p, "for the VU meters and visualizers at low volume", &p4_font_14, UI_TEXT_DIM, 210, py + 67);
    static const char *const gains[4] = { "Normal", "+6 dB", "+12 dB", "+18 dB" };
    for (int i = 0; i < 4; i++) {
        lv_obj_t *b = create_button(p, gains[i], 164, 46);
        lv_obj_set_pos(b, 30 + i * 174, py + 92);
        lv_obj_add_event_cb(b, vsp_gain_event_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        vsp_gain_btn[i] = b;
    }
    /* beside them: the VU meter on Now Playing on or off */
    vsp_vu_btn = create_button(p, "VU METER  ON", 168, 46);
    lv_obj_set_pos(vsp_vu_btn, 30 + 4 * 174 + 6, py + 92);
    lv_obj_add_event_cb(vsp_vu_btn, vsp_vu_event_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *open = create_button(p, LV_SYMBOL_EYE_OPEN "  PREVIEW", 150, 50);
    lv_obj_align(open, LV_ALIGN_TOP_RIGHT, -362, 14);
    lv_obj_add_event_cb(open, vsp_open_event_cb, LV_EVENT_CLICKED, NULL);
    ssp_label(p, "Tap a style, then PREVIEW to see it (tap the preview to come back).", &p4_font_14,
              UI_TEXT_SOFT, 30, PANEL_H - 84);
    ssp_label(p, "On Now Playing, a tap on the VU meter opens the visualizer; NEXT there steps "
                 "through the styles.", &p4_font_14, UI_TEXT_DIM, 30, PANEL_H - 58);
}
