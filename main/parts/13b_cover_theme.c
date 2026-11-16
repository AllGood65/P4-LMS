/*
 * P4-LMS - The COVER COLOURS theme: the interface takes its colours from the
 * album art that is showing.
 *
 * Part of main.c.  This file is #included by main.c (in order) and is not
 * compiled on its own, so it shares main.c's includes and static names.
 *
 * How it works
 *  - Each new cover is sampled for its strongest colours: a main one, a second
 *    and a third with clearly different hues.  Greys, near-black and
 *    near-white are ignored; a black-and-white cover keeps the last colours.
 *  - Every colour of the theme is worked out from those hues with FIXED
 *    lightness levels (cover_theme_derive), so text stays readable whatever
 *    the cover: only the hue and the amount of colour change.
 *  - The widgets already on screen carry their colours in their styles, so
 *    the whole widget tree is walked once and each old theme colour is
 *    swapped for its new one (cover_theme_apply).  New widgets read the live
 *    theme (ui_theme_live) and come out right by themselves.
 *  Nothing here runs unless COVER COLOURS is the chosen theme.
 */
#include "lvgl_private.h"      /* the widget tree's style lists (lv_obj_t internals) */

/* The hues the theme is built from (degrees) and their colour strength (0-1). */
typedef struct { float h1, s1, h2, s2, h3, s3; } cover_hues_t;
static cover_hues_t cover_hues_now = { 174.0f, 0.62f, 42.0f, 0.80f, 340.0f, 0.70f };

static float cover_wrap(float h) { while (h < 0) h += 360.0f; while (h >= 360.0f) h -= 360.0f; return h; }
static float cover_clamp(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

/* HSL (h degrees, s and l 0-1) -> 0xRRGGBB */
static uint32_t cover_hsl(float h, float s, float l)
{
    h = cover_wrap(h) / 360.0f;
    s = cover_clamp(s, 0.0f, 1.0f);
    l = cover_clamp(l, 0.0f, 1.0f);
    const float q = l < 0.5f ? l * (1.0f + s) : l + s - l * s;
    const float p = 2.0f * l - q;
    float rgb[3];
    const float off[3] = { 1.0f / 3.0f, 0.0f, -1.0f / 3.0f };
    for (int i = 0; i < 3; i++) {
        float t = h + off[i];
        if (t < 0) t += 1.0f;
        if (t > 1) t -= 1.0f;
        float c;
        if (t < 1.0f / 6.0f)      c = p + (q - p) * 6.0f * t;
        else if (t < 0.5f)        c = q;
        else if (t < 2.0f / 3.0f) c = p + (q - p) * (2.0f / 3.0f - t) * 6.0f;
        else                      c = p;
        rgb[i] = c;
    }
    return ((uint32_t)(rgb[0] * 255.0f + 0.5f) << 16) |
           ((uint32_t)(rgb[1] * 255.0f + 0.5f) << 8) |
            (uint32_t)(rgb[2] * 255.0f + 0.5f);
}

/* Every colour of the theme from the three hues.  Each field has its own
 * lightness / strength, so no two fields can come out the same for one cover
 * and differently for the next (the swap in cover_theme_apply relies on it). */
static void cover_theme_derive(const cover_hues_t *c, ui_theme_t *t)
{
    const float h1 = c->h1, s1 = cover_clamp(c->s1, 0.45f, 0.85f);
    const float h2 = c->h2, s2 = cover_clamp(c->s2, 0.45f, 0.85f);
    const float h3 = c->h3, s3 = cover_clamp(c->s3, 0.45f, 0.85f);

    /* backgrounds: dark, lightly tinted with the main hue */
    t->bg          = cover_hsl(h1, 0.30f, 0.055f);
    t->deep        = cover_hsl(h1, 0.30f, 0.035f);
    t->panel       = cover_hsl(h1, 0.28f, 0.105f);
    t->panel_alt   = cover_hsl(h1, 0.28f, 0.090f);
    t->album_bg    = cover_hsl(h1, 0.26f, 0.135f);
    t->track       = cover_hsl(h1, 0.24f, 0.150f);
    t->button      = cover_hsl(h1, 0.32f, 0.165f);
    t->slider_bg   = cover_hsl(h1, 0.30f, 0.185f);
    t->edge_soft   = cover_hsl(h1, 0.30f, 0.215f);
    t->divider     = cover_hsl(h1, 0.34f, 0.255f);
    t->edge        = cover_hsl(h1, 0.36f, 0.300f);
    /* text: nearly neutral, always light */
    t->text        = cover_hsl(h1, 0.30f, 0.940f);
    t->text_soft   = cover_hsl(h1, 0.20f, 0.760f);
    t->label_soft  = cover_hsl(h1, 0.16f, 0.690f);
    t->label_dim   = cover_hsl(h1, 0.12f, 0.650f);
    t->text_dim    = cover_hsl(h1, 0.14f, 0.610f);
    t->placeholder = cover_hsl(h1, 0.12f, 0.560f);
    /* the main colour */
    t->accent        = cover_hsl(h1, s1, 0.600f);
    t->accent_glow   = cover_hsl(h1, s1, 0.720f);
    t->accent_bright = cover_hsl(h1, s1 * 0.85f, 0.860f);
    t->accent_dark   = cover_hsl(h1, s1 * 0.80f, 0.320f);
    t->on_accent     = cover_hsl(h1, 0.60f, 0.075f);
    t->value_text    = cover_hsl(h1, 0.48f, 0.690f);
    t->tick          = t->value_text;
    t->scale_text    = cover_hsl(h1, 0.42f, 0.620f);
    t->date          = cover_hsl(h1, 0.38f, 0.650f);
    t->conn_amber    = cover_hsl(h1, 0.55f, 0.665f);
    /* second colour (trim, outlines, icons) and third (volume, PLAYING chip) */
    t->amber    = cover_hsl(h2, s2, 0.580f);
    t->orange   = t->amber;
    t->tertiary = cover_hsl(h3, s3, 0.660f);
    t->red      = 0xE0484F;
    /* visualizer "THEME" colours, quiet to loud */
    t->vis[0] = cover_hsl(h1, s1, 0.220f);
    t->vis[1] = cover_hsl(h1, s1, 0.400f);
    t->vis[2] = t->accent;
    t->vis[3] = t->amber;
    t->vis[4] = t->tertiary;
    t->vis[5] = cover_hsl(h3, s3 * 0.70f, 0.860f);
}

/* Called by ui_theme_load() before any widget exists. */
static void cover_theme_init(void)
{
    cover_theme_derive(&cover_hues_now, &ui_theme_live);
}

/* ---- picking the colours out of a cover ---------------------------------- */
#define COVER_BINS 36                   /* 10-degree hue bins */

static float cover_bin_dist(int a, int b)
{
    int d = abs(a - b) % COVER_BINS;
    return (float)(d > COVER_BINS / 2 ? COVER_BINS - d : d);
}

/* false: the cover has too little colour to go by (keep the current hues). */
static bool cover_pick_hues(const lv_image_dsc_t *img, cover_hues_t *out)
{
    if (img == NULL || img->data == NULL || img->header.w < 8 || img->header.h < 8) return false;
    const uint32_t w = img->header.w, h = img->header.h;
    const uint32_t stride = img->header.stride / 2U;
    const uint16_t *px = (const uint16_t *)img->data;

    float weight[COVER_BINS] = { 0 }, sat[COVER_BINS] = { 0 };
    float vx[COVER_BINS] = { 0 }, vy[COVER_BINS] = { 0 };   /* hue as a vector, for an exact mean */
    float total = 0.0f;
    for (uint32_t y = 2; y < h; y += 4) {
        for (uint32_t x = 2; x < w; x += 4) {
            const uint16_t v = px[y * stride + x];
            const float r = (float)((v >> 11) & 0x1F) / 31.0f;
            const float g = (float)((v >> 5) & 0x3F) / 63.0f;
            const float b = (float)(v & 0x1F) / 31.0f;
            const float mx = fmaxf(r, fmaxf(g, b)), mn = fminf(r, fminf(g, b));
            const float d = mx - mn;
            if (mx < 0.20f || d < 0.08f) continue;           /* near-black or grey */
            const float s = d / mx;
            if (s < 0.22f) continue;
            float hue;
            if (mx == r)      hue = 60.0f * fmodf((g - b) / d, 6.0f);
            else if (mx == g) hue = 60.0f * ((b - r) / d + 2.0f);
            else              hue = 60.0f * ((r - g) / d + 4.0f);
            hue = cover_wrap(hue);
            const float wgt = s * mx;
            const int bin = (int)(hue / 10.0f) % COVER_BINS;
            weight[bin] += wgt;
            sat[bin] += s * wgt;
            vx[bin] += cosf(hue * (float)M_PI / 180.0f) * wgt;
            vy[bin] += sinf(hue * (float)M_PI / 180.0f) * wgt;
            total += wgt;
        }
    }
    const float samples = (float)((w / 4) * (h / 4));
    if (total < samples * 0.04f) return false;               /* hardly any colour */

    /* a bin's strength counts its neighbours a little (a hue split across two bins) */
    float score[COVER_BINS];
    for (int i = 0; i < COVER_BINS; i++) {
        score[i] = weight[i] + 0.5f * (weight[(i + 1) % COVER_BINS] + weight[(i + COVER_BINS - 1) % COVER_BINS]);
    }
    int b1 = 0;
    for (int i = 1; i < COVER_BINS; i++) if (score[i] > score[b1]) b1 = i;
    int b2 = -1, b3 = -1;
    for (int i = 0; i < COVER_BINS; i++) {
        if (cover_bin_dist(i, b1) < 5 || score[i] < score[b1] * 0.12f) continue;
        if (b2 < 0 || score[i] > score[b2]) b2 = i;
    }
    for (int i = 0; i < COVER_BINS; i++) {
        if (cover_bin_dist(i, b1) < 5 || (b2 >= 0 && cover_bin_dist(i, b2) < 5) ||
            score[i] < score[b1] * 0.08f) continue;
        if (b3 < 0 || score[i] > score[b3]) b3 = i;
    }

    /* mean hue and strength of a bin and its neighbours */
    #define COVER_HUE(bin, hue_out, sat_out) do {                                       \
        float x_ = 0, y_ = 0, s_ = 0, w_ = 0;                                           \
        for (int k_ = -1; k_ <= 1; k_++) {                                              \
            const int j_ = ((bin) + k_ + COVER_BINS) % COVER_BINS;                      \
            x_ += vx[j_]; y_ += vy[j_]; s_ += sat[j_]; w_ += weight[j_];                \
        }                                                                               \
        (hue_out) = cover_wrap(atan2f(y_, x_) * 180.0f / (float)M_PI);                  \
        (sat_out) = w_ > 0 ? s_ / w_ : 0.6f;                                            \
    } while (0)

    COVER_HUE(b1, out->h1, out->s1);
    if (b2 >= 0) { COVER_HUE(b2, out->h2, out->s2); }
    else         { out->h2 = cover_wrap(out->h1 + 35.0f); out->s2 = out->s1; }   /* one-colour cover: a neighbouring hue */
    if (b3 >= 0) { COVER_HUE(b3, out->h3, out->s3); }
    else         { out->h3 = cover_wrap(out->h1 - 35.0f); out->s3 = out->s1; }
    #undef COVER_HUE
    return true;
}

/* ---- recolouring what is already on screen -------------------------------- */
typedef struct { uint32_t from, to; } cover_swap_t;
#define COVER_MAX_SWAPS 48
#define COVER_MAX_SHARED 128

static const lv_style_prop_t cover_color_props[] = {
    LV_STYLE_BG_COLOR, LV_STYLE_BG_GRAD_COLOR, LV_STYLE_BORDER_COLOR, LV_STYLE_OUTLINE_COLOR,
    LV_STYLE_SHADOW_COLOR, LV_STYLE_TEXT_COLOR, LV_STYLE_IMAGE_RECOLOR, LV_STYLE_LINE_COLOR,
    LV_STYLE_ARC_COLOR,
};

typedef struct {
    cover_swap_t swaps[COVER_MAX_SWAPS];
    int n;
    const lv_style_t *shared[COVER_MAX_SHARED];   /* styles used by several widgets: done once */
    int shared_n;
    uint32_t changed;
} cover_walk_t;

static void cover_recolor_style(cover_walk_t *w, lv_style_t *st)
{
    for (size_t p = 0; p < sizeof(cover_color_props) / sizeof(cover_color_props[0]); p++) {
        lv_style_value_t v;
        if (lv_style_get_prop(st, cover_color_props[p], &v) != LV_STYLE_RES_FOUND) continue;
        const uint32_t c = lv_color_to_u32(v.color) & 0xFFFFFFU;
        for (int i = 0; i < w->n; i++) {
            if (w->swaps[i].from == c) {
                v.color = lv_color_hex(w->swaps[i].to);
                lv_style_set_prop(st, cover_color_props[p], v);
                w->changed++;
                break;
            }
        }
    }
}

static void cover_recolor_tree(cover_walk_t *w, lv_obj_t *obj, int depth)
{
    if (obj == NULL || depth > 40) return;
    for (uint32_t i = 0; i < obj->style_cnt; i++) {
        lv_obj_style_t *os = &obj->styles[i];
        if (os->is_theme || os->style == NULL || lv_style_is_const(os->style)) continue;
        if (!os->is_local) {
            bool seen = false;
            for (int k = 0; k < w->shared_n; k++) if (w->shared[k] == os->style) { seen = true; break; }
            if (seen) continue;
            if (w->shared_n < COVER_MAX_SHARED) w->shared[w->shared_n++] = os->style;
        }
        cover_recolor_style(w, (lv_style_t *)os->style);
    }
    const uint32_t n = lv_obj_get_child_count(obj);
    for (uint32_t i = 0; i < n; i++) cover_recolor_tree(w, lv_obj_get_child(obj, (int32_t)i), depth + 1);
}

static void cover_add_swap(cover_walk_t *w, uint32_t from, uint32_t to)
{
    if (from == to) return;
    for (int i = 0; i < w->n; i++) if (w->swaps[i].from == from) return;   /* first one wins */
    if (w->n < COVER_MAX_SWAPS) { w->swaps[w->n].from = from; w->swaps[w->n].to = to; w->n++; }
}

/* LVGL task: switch the live theme to `next` and recolour the screen. */
static void cover_theme_apply(const ui_theme_t *next)
{
    static cover_walk_t w;                      /* (static: keeps it off the LVGL task's stack) */
    memset(&w, 0, sizeof(w));
    const uint32_t *a = &ui_theme_live.bg, *b = &next->bg;
    const size_t count = (size_t)(&ui_theme_live.vis[5] - &ui_theme_live.bg) + 1;   /* every colour field */
    for (size_t i = 0; i < count; i++) cover_add_swap(&w, a[i], b[i]);
    if (w.n == 0) return;

    const int64_t t0 = esp_timer_get_time();
    cover_recolor_tree(&w, lv_screen_active(), 0);
    cover_recolor_tree(&w, lv_layer_top(), 0);
    /* the live theme takes the new colours; its name stays */
    const char *name = ui_theme_live.name;
    memcpy((void *)&ui_theme_live.bg, (const void *)&next->bg, count * sizeof(uint32_t));
    ui_theme_live.name = name;
    lv_obj_invalidate(lv_screen_active());
    lv_obj_invalidate(lv_layer_top());
    ESP_LOGI(TAG, "Cover colours: hues %d / %d / %d - %u colours changed in %u ms",
             (int)cover_hues_now.h1, (int)cover_hues_now.h2, (int)cover_hues_now.h3,
             (unsigned)w.changed, (unsigned)((esp_timer_get_time() - t0) / 1000));
}

/* LVGL task, after a new cover is in album_image_dsc. */
static void cover_theme_new_cover(void)
{
    if (ui_theme_index != UI_THEME_COVER) return;
    cover_hues_t hues;
    if (!cover_pick_hues(&album_image_dsc, &hues)) return;      /* black-and-white cover: keep the colours */
    /* hardly different from now (same album, a slightly different picture): leave it */
    const float dh = fabsf(cover_wrap(hues.h1 - cover_hues_now.h1 + 180.0f) - 180.0f);
    const float dh2 = fabsf(cover_wrap(hues.h2 - cover_hues_now.h2 + 180.0f) - 180.0f);
    if (dh < 6.0f && dh2 < 10.0f && fabsf(hues.s1 - cover_hues_now.s1) < 0.08f) return;
    cover_hues_now = hues;
    static ui_theme_t next;
    next = ui_theme_live;
    cover_theme_derive(&hues, &next);
    cover_theme_apply(&next);
}
