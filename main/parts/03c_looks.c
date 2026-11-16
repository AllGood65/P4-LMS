/*
 * P4-LMS - Theme STYLES ("looks"): corner shape, trim, buttons, tab marker
 * and title lettering, chosen per theme (ui_theme_t.look, 01_themes.c).
 *
 * Part of main.c.  This file is #included by main.c (in order) and is not
 * compiled on its own, so it shares main.c's includes and static names.
 *
 * Everything here is drawn as cheaply as the original look: no gradients,
 * shadows or see-through layers (the display draws in software).  A double
 * trim is the plain border plus a thin outline a few pixels outside it (an
 * outline drawn INSIDE would need a negative corner radius, which LVGL can't
 * draw).
 */

typedef struct {
    uint8_t radius_cap;     /* corners never rounder than this (99: as designed) */
    uint8_t frame_w;        /* trim line width on panels and frames               */
    uint8_t frame_gap;      /* >0: a second, thin trim line this far outside it    */
    uint8_t btn_border;     /* outline width on ordinary buttons (0: none)        */
    bool    btn_accent;     /* button outline in the accent colour (outlined look)*/
    bool    tab_underline;  /* chosen tab: a line under it instead of a filled pill */
    int8_t  title_space;    /* extra letter spacing on titles                     */
} ui_look_t;

static const ui_look_t ui_looks[] = {
    /* CLASSIC */ { 99, 2, 0, 1, false, false, 0 },
    /* SOFT    */ { 99, 1, 0, 0, false, false, 0 },
    /* DECO    */ {  2, 1, 3, 1, true,  true,  2 },
    /* BRASS   */ {  5, 3, 3, 1, false, false, 1 },
    /* NEON    */ {  0, 1, 0, 1, true,  true,  3 },
};
#define UI_LOOK_COUNT ((int)(sizeof(ui_looks) / sizeof(ui_looks[0])))
#define UI_LOOK (ui_looks[ui_theme->look < UI_LOOK_COUNT ? ui_theme->look : 0])

/* Corner radius for this theme's look (round dots and knobs stay round). */
static int32_t ui_radius(int32_t r)
{
    if (r >= LV_RADIUS_CIRCLE) return r;
    return r < UI_LOOK.radius_cap ? r : UI_LOOK.radius_cap;
}
#define UI_R(r) ui_radius(r)

/* The trim around a panel or frame, in `color`.  `classic_w` is its line
 * width in the CLASSIC look (each frame keeps its own there). */
static void ui_frame(lv_obj_t *obj, uint32_t color, int32_t classic_w)
{
    if (obj == NULL) return;
    const ui_look_t *l = &UI_LOOK;
    lv_obj_set_style_border_width(obj, ui_theme->look == UI_LOOK_CLASSIC ? classic_w : l->frame_w, 0);
    lv_obj_set_style_border_color(obj, lv_color_hex(color), 0);
    if (l->frame_gap > 0) {
        lv_obj_set_style_outline_width(obj, 1, 0);
        lv_obj_set_style_outline_color(obj, lv_color_hex(color), 0);
        lv_obj_set_style_outline_pad(obj, l->frame_gap, 0);
    }
}

/* The title lettering for this theme (Montserrat for CLASSIC and SOFT). */
static const lv_font_t *ui_title_font_for(int look)
{
    switch (look) {
        case UI_LOOK_DECO:  return &p4_title_deco;
        case UI_LOOK_BRASS: return &p4_title_brass;
        case UI_LOOK_NEON:  return &p4_title_neon;
        default:            return &p4_font_28;
    }
}

/* A panel title (SETTINGS, LIBRARY ...) in this theme's lettering. */
static void ui_title(lv_obj_t *label)
{
    if (label == NULL) return;
    lv_obj_set_style_text_font(label, ui_title_font_for(ui_theme->look), 0);
    lv_obj_set_style_text_letter_space(label, UI_LOOK.title_space, 0);
}
