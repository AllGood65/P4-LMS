/*
 * P4-LMS - Extra screensavers: analog clock and cover wall
 *
 * Part of main.c.  This file is #included by main.c (in order) and is not
 * compiled on its own, so it shares main.c's includes and static names.
 *
 * Both are built for software drawing: the clock redraws once a second, the
 * wall changes one cover every ~25 s.  Both drift slowly (burn-in).
 */

/* ======================================================= analog clock */
#define SS_AN_SIZE   440
static lv_obj_t *ss_an_box = NULL;       /* full screen, drifts           */
static lv_obj_t *ss_an_face = NULL;      /* drawn in ss_an_draw_cb        */
static lv_obj_t *ss_an_date = NULL;
static int ss_an_last_sec = -1;

static lv_color_t ss_dim(uint32_t rgb, int pct) { return lv_color_hex(ss_scale_rgb(rgb, pct)); }

static lv_point_precise_t ss_an_point(int32_t cx, int32_t cy, float deg, float r)
{
    const float a = (deg - 90.0f) * 3.14159265f / 180.0f;
    lv_point_precise_t p;
    p.x = (lv_value_precise_t)(cx + r * cosf(a));
    p.y = (lv_value_precise_t)(cy + r * sinf(a));
    return p;
}

static void ss_an_draw_cb(lv_event_t *e)
{
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_obj_t *obj = (lv_obj_t *)lv_event_get_target(e);
    lv_area_t c;
    lv_obj_get_coords(obj, &c);
    const int32_t cx = (c.x1 + c.x2) / 2, cy = (c.y1 + c.y2) / 2;
    const float R = SS_AN_SIZE / 2 - 10;
    const uint32_t glow = UI_ACCENT_GLOW;

    /* rim */
    lv_draw_arc_dsc_t ad;
    lv_draw_arc_dsc_init(&ad);
    ad.center.x = cx;
    ad.center.y = cy;
    ad.radius = (uint16_t)R;
    ad.width = 3;
    ad.start_angle = 0;
    ad.end_angle = 360;
    ad.color = ss_dim(glow, 45);
    lv_draw_arc(layer, &ad);

    /* minute ticks, longer every five */
    lv_draw_line_dsc_t ld;
    lv_draw_line_dsc_init(&ld);
    for (int k = 0; k < 60; k++) {
        const bool major = (k % 5) == 0;
        ld.width = major ? 4 : 2;
        ld.color = ss_dim(glow, major ? 70 : 35);
        ld.p1 = ss_an_point(cx, cy, (float)k * 6.0f, R - (major ? 24.0f : 12.0f));
        ld.p2 = ss_an_point(cx, cy, (float)k * 6.0f, R - 5.0f);
        lv_draw_line(layer, &ld);
    }

    /* 12 / 3 / 6 / 9 */
    lv_draw_label_dsc_t td;
    lv_draw_label_dsc_init(&td);
    td.font = &p4_font_28;
    td.color = ss_dim(glow, 65);
    td.align = LV_TEXT_ALIGN_CENTER;
    static const char *const nums[4] = { "12", "3", "6", "9" };
    for (int k = 0; k < 4; k++) {
        const lv_point_precise_t p = ss_an_point(cx, cy, (float)k * 90.0f, R - 54.0f);
        lv_area_t a = { (int32_t)p.x - 30, (int32_t)p.y - 16, (int32_t)p.x + 30, (int32_t)p.y + 16 };
        td.text = nums[k];
        lv_draw_label(layer, &td, &a);
    }

    /* hands */
    time_t now = time(NULL);
    struct tm t;
    if (localtime_r(&now, &t) != NULL && time_is_valid(&t)) {
        const float hour = (float)(t.tm_hour % 12) + (float)t.tm_min / 60.0f;
        const float min  = (float)t.tm_min + (float)t.tm_sec / 60.0f;
        ld.round_start = 1;
        ld.round_end = 1;

        ld.width = 10;
        ld.color = lv_color_hex(glow);
        ld.p1 = ss_an_point(cx, cy, hour * 30.0f + 180.0f, 18.0f);
        ld.p2 = ss_an_point(cx, cy, hour * 30.0f, R * 0.52f);
        lv_draw_line(layer, &ld);

        ld.width = 6;
        ld.p1 = ss_an_point(cx, cy, min * 6.0f + 180.0f, 22.0f);
        ld.p2 = ss_an_point(cx, cy, min * 6.0f, R * 0.80f);
        lv_draw_line(layer, &ld);

        ld.width = 2;
        ld.color = lv_color_hex(UI_ACCENT);
        ld.p1 = ss_an_point(cx, cy, (float)t.tm_sec * 6.0f + 180.0f, 30.0f);
        ld.p2 = ss_an_point(cx, cy, (float)t.tm_sec * 6.0f, R * 0.88f);
        lv_draw_line(layer, &ld);
    }

    lv_draw_rect_dsc_t rd;
    lv_draw_rect_dsc_init(&rd);
    rd.bg_color = lv_color_hex(UI_ACCENT);
    rd.bg_opa = LV_OPA_COVER;
    rd.radius = LV_RADIUS_CIRCLE;
    lv_area_t hub = { cx - 9, cy - 9, cx + 9, cy + 9 };
    lv_draw_rect(layer, &rd, &hub);
}

static void ss_an_create(lv_obj_t *overlay)
{
    ss_an_box = lv_obj_create(overlay);
    lv_obj_remove_style_all(ss_an_box);
    lv_obj_set_size(ss_an_box, 1024, 600);
    lv_obj_clear_flag(ss_an_box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(ss_an_box, LV_OBJ_FLAG_CLICKABLE);

    ss_an_face = lv_obj_create(ss_an_box);
    lv_obj_remove_style_all(ss_an_face);
    lv_obj_set_size(ss_an_face, SS_AN_SIZE, SS_AN_SIZE);
    lv_obj_set_pos(ss_an_face, (1024 - SS_AN_SIZE) / 2, 30);
    lv_obj_clear_flag(ss_an_face, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(ss_an_face, ss_an_draw_cb, LV_EVENT_DRAW_MAIN, NULL);

    ss_an_date = lv_label_create(ss_an_box);
    lv_label_set_text(ss_an_date, "");
    lv_obj_set_width(ss_an_date, 1024);
    lv_obj_set_style_text_align(ss_an_date, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(ss_an_date, &p4_font_28, 0);
    lv_obj_set_style_text_color(ss_an_date, lv_color_hex(UI_DATE), 0);
    lv_obj_set_style_text_letter_space(ss_an_date, 2, 0);
    lv_obj_set_pos(ss_an_date, 0, 30 + SS_AN_SIZE + 22);

    lv_obj_add_flag(ss_an_box, LV_OBJ_FLAG_HIDDEN);
}

static void ss_an_tick(void)
{
    time_t now = time(NULL);
    struct tm t;
    if (localtime_r(&now, &t) == NULL) return;
    if (t.tm_sec == ss_an_last_sec) return;
    ss_an_last_sec = t.tm_sec;
    lv_obj_invalidate(ss_an_face);                 /* the hands moved */
    char d[48];
    if (time_is_valid(&t)) {
        ss_clk_date_text(&t, d, sizeof(d));      /* Settings > Clock style date */
        for (char *p = d; *p; p++) *p = (char)toupper((unsigned char)*p);
    } else {
        snprintf(d, sizeof(d), "SYNCING TIME");
    }
    ui_set_label(ss_an_date, d);
}

/* ========================================================== cover wall */
#define WALL_COLS    5
#define WALL_ROWS    3
#define WALL_TILES   (WALL_COLS * WALL_ROWS)
#define WALL_PX      180
#define WALL_GAP     12
#define WALL_NEXT_MS 25000

static lv_obj_t *ss_wall_box = NULL;
static lv_obj_t *ss_wall_img[WALL_TILES];
static lv_obj_t *ss_wall_caption = NULL;
static lv_obj_t *ss_wall_clock = NULL;
static uint8_t  *ss_wall_pixels[WALL_TILES];
static lv_image_dsc_t ss_wall_dsc[WALL_TILES];
static int      ss_wall_filled = 0;         /* tiles with a cover            */
static int      ss_wall_next = 0;           /* tile the next cover replaces  */
static int      ss_wall_lit = -1;           /* the full-brightness tile      */
static uint32_t ss_wall_asked_tick = 0;     /* when covers were last asked for */
static bool     ss_wall_asked = false;

static void ss_wall_light(int k, bool on)
{
    if (k < 0 || k >= WALL_TILES || ss_wall_img[k] == NULL) return;
    lv_obj_set_style_image_recolor_opa(ss_wall_img[k], on ? LV_OPA_TRANSP : LV_OPA_50, 0);
    lv_obj_set_style_outline_width(ss_wall_img[k], on ? 3 : 0, 0);
}

static void ss_wall_create(lv_obj_t *overlay)
{
    ss_wall_box = lv_obj_create(overlay);
    lv_obj_remove_style_all(ss_wall_box);
    lv_obj_set_size(ss_wall_box, 1024, 600);
    lv_obj_clear_flag(ss_wall_box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(ss_wall_box, LV_OBJ_FLAG_CLICKABLE);

    const int x0 = (1024 - (WALL_COLS * WALL_PX + (WALL_COLS - 1) * WALL_GAP)) / 2;
    const int y0 = (600 - (WALL_ROWS * WALL_PX + (WALL_ROWS - 1) * WALL_GAP)) / 2 - 10;
    for (int k = 0; k < WALL_TILES; k++) {
        lv_obj_t *img = lv_image_create(ss_wall_box);
        lv_obj_set_size(img, WALL_PX, WALL_PX);
        lv_image_set_inner_align(img, LV_IMAGE_ALIGN_STRETCH);
        lv_obj_set_pos(img, x0 + (k % WALL_COLS) * (WALL_PX + WALL_GAP), y0 + (k / WALL_COLS) * (WALL_PX + WALL_GAP));
        lv_obj_set_style_bg_color(img, lv_color_hex(0x141414), 0);
        lv_obj_set_style_bg_opa(img, LV_OPA_COVER, 0);        /* empty tiles: a dark square */
        lv_obj_set_style_image_recolor(img, lv_color_hex(0x000000), 0);
        lv_obj_set_style_image_recolor_opa(img, LV_OPA_50, 0);
        lv_obj_set_style_outline_color(img, lv_color_hex(UI_ACCENT), 0);
        lv_obj_set_style_outline_pad(img, 2, 0);
        lv_obj_set_style_outline_width(img, 0, 0);
        lv_obj_clear_flag(img, LV_OBJ_FLAG_CLICKABLE);
        ss_wall_img[k] = img;
        ss_wall_pixels[k] = NULL;
    }

    ss_wall_caption = lv_label_create(ss_wall_box);
    lv_label_set_text(ss_wall_caption, "");
    lv_label_set_long_mode(ss_wall_caption, LV_LABEL_LONG_DOT);
    lv_obj_set_width(ss_wall_caption, 520);
    lv_obj_set_style_text_align(ss_wall_caption, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(ss_wall_caption, &p4_font_18, 0);
    lv_obj_set_style_text_color(ss_wall_caption, lv_color_hex(UI_TEXT_SOFT), 0);
    lv_obj_set_style_bg_color(ss_wall_caption, lv_color_hex(0x140C14), 0);
    lv_obj_set_style_bg_opa(ss_wall_caption, LV_OPA_80, 0);
    lv_obj_set_style_radius(ss_wall_caption, 12, 0);
    lv_obj_set_style_pad_ver(ss_wall_caption, 10, 0);
    lv_obj_set_style_pad_hor(ss_wall_caption, 16, 0);
    lv_obj_set_height(ss_wall_caption, lv_font_get_line_height(&p4_font_18) + 20);   /* one line + padding */
    lv_obj_set_pos(ss_wall_caption, (1024 - 520) / 2, 600 - 70);
    lv_obj_add_flag(ss_wall_caption, LV_OBJ_FLAG_HIDDEN);

    ss_wall_clock = lv_label_create(ss_wall_box);
    lv_label_set_text(ss_wall_clock, "");
    lv_obj_set_style_text_font(ss_wall_clock, &p4_font_28, 0);
    lv_obj_set_style_text_color(ss_wall_clock, ss_dim(UI_ACCENT_GLOW, 75), 0);
    lv_obj_set_style_bg_color(ss_wall_clock, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(ss_wall_clock, LV_OPA_60, 0);
    lv_obj_set_style_pad_all(ss_wall_clock, 6, 0);
    lv_obj_set_style_radius(ss_wall_clock, 8, 0);
    lv_obj_align(ss_wall_clock, LV_ALIGN_TOP_RIGHT, -16, 12);

    lv_obj_add_flag(ss_wall_box, LV_OBJ_FLAG_HIDDEN);
}

/* Ask the browser task for n random covers. */
static void ss_wall_request(int n)
{
    if (source_queue == NULL) return;
    source_request_t r = { .mode = SOURCE_WALL_FETCH, .id = n, .push_history = false };
    if (xQueueSend(source_queue, &r, 0) == pdTRUE) {
        ss_wall_asked = true;
        ss_wall_asked_tick = lv_tick_get();
    }
}

typedef struct {
    uint8_t *pixels; uint32_t w, h, padded; size_t size;
    char caption[160];
    bool last;
    bool taken;
} wall_cover_ctx_t;

/* LVGL task: put one downloaded cover on the next tile. */
static void ss_wall_put_job(void *arg)
{
    wall_cover_ctx_t *c = (wall_cover_ctx_t *)arg;
    if (c->last) ss_wall_asked = false;
    if (c->pixels == NULL || ss_wall_box == NULL) return;
    const int k = ss_wall_next;
    ss_wall_next = (ss_wall_next + 1) % WALL_TILES;
    if (ss_wall_pixels[k] != NULL) heap_caps_free(ss_wall_pixels[k]);
    ss_wall_pixels[k] = c->pixels;
    c->taken = true;
    lv_image_dsc_t *d = &ss_wall_dsc[k];
    memset(d, 0, sizeof(*d));
    d->header.cf = LV_COLOR_FORMAT_RGB565;
    d->header.w = c->w;
    d->header.h = c->h;
    d->header.stride = c->padded * 2U;
    d->data_size = c->size;
    d->data = c->pixels;
    lv_image_set_src(ss_wall_img[k], d);
    if (ss_wall_filled < WALL_TILES) ss_wall_filled++;
    ss_wall_light(ss_wall_lit, false);
    ss_wall_lit = k;
    ss_wall_light(k, true);
    ui_set_label(ss_wall_caption, c->caption);
    lv_obj_clear_flag(ss_wall_caption, LV_OBJ_FLAG_HIDDEN);
}

/* Browser task: n random albums from the library, their covers one by one. */
static void ss_wall_fetch(int n)
{
    const int p = current_player_index;
    if (p < 0 || p >= lyrion_player_count || n <= 0) {
        wall_cover_ctx_t done = { .last = true };
        ui_call(ss_wall_put_job, &done);
        return;
    }
    if (n > WALL_TILES) n = WALL_TILES;
    char json[192];
    snprintf(json, sizeof(json),
             "{\"id\":1,\"method\":\"slim.request\",\"params\":[\"%s\",[\"albums\",0,%d,\"sort:random\",\"tags:laj\"]]}",
             lyrion_players[p].id, n + 4);                 /* a few spare: some have no cover */
    char *buf = heap_caps_malloc(SOURCE_BROWSE_BUF, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    cJSON *root = NULL;
    if (buf != NULL && source_http_query(p, json, buf, SOURCE_BROWSE_BUF)) root = cJSON_Parse(buf);
    if (buf != NULL) heap_caps_free(buf);
    cJSON *res = root ? cJSON_GetObjectItem(root, "result") : NULL;
    cJSON *loop = res ? cJSON_GetObjectItem(res, "albums_loop") : NULL;
    int got = 0;
    cJSON *a = NULL;
    if (cJSON_IsArray(loop)) {
        cJSON_ArrayForEach(a, loop) {
            if (got >= n) break;
            if (uxQueueMessagesWaiting(source_queue) > 0 || !screensaver_active) break;   /* in use again */
            cJSON *aw = cJSON_GetObjectItem(a, "artwork_track_id");
            char key[24] = "";
            if (cJSON_IsNumber(aw)) snprintf(key, sizeof(key), "%d", aw->valueint);
            else if (cJSON_IsString(aw) && aw->valuestring) snprintf(key, sizeof(key), "%.22s", aw->valuestring);
            if (!key[0]) continue;
            wall_cover_ctx_t c = { 0 };
            if (!source_download_cover(key, WALL_PX, &c.pixels, &c.w, &c.h, &c.padded, &c.size)) continue;
            cJSON *al = cJSON_GetObjectItem(a, "album");
            cJSON *ar = cJSON_GetObjectItem(a, "artist");
            char album[SOURCE_TEXT_MAX], artist[SOURCE_TEXT_MAX];
            source_clean_display_text(cJSON_IsString(al) ? al->valuestring : "", album, sizeof(album));
            source_clean_display_text(cJSON_IsString(ar) ? ar->valuestring : "", artist, sizeof(artist));
            snprintf(c.caption, sizeof(c.caption), artist[0] ? "%.70s  " LV_SYMBOL_BULLET "  %.60s" : "%.70s%.0s",
                     album, artist);
            got++;
            c.last = (got >= n);
            ui_call(ss_wall_put_job, &c);
            if (!c.taken) heap_caps_free(c.pixels);
        }
    }
    if (root) cJSON_Delete(root);
    if (got < n) {                                         /* let the next tick ask again */
        wall_cover_ctx_t done = { .last = true };
        ui_call(ss_wall_put_job, &done);
    }
    ESP_LOGI(TAG, "Cover wall: %d cover(s) added", got);
}

static void ss_wall_tick(void)
{
    char t[16] = "";
    time_t now = time(NULL);
    struct tm tmv;
    if (localtime_r(&now, &tmv) != NULL && time_is_valid(&tmv)) {
        clock_short_text(&tmv, t, sizeof(t));
    }
    ui_set_label(ss_wall_clock, t);

    if (ss_wall_asked && lv_tick_elaps(ss_wall_asked_tick) < 60000) return;   /* still coming */
    if (ss_wall_filled < WALL_TILES) {
        if (lv_tick_elaps(ss_wall_asked_tick) > 10000) ss_wall_request(WALL_TILES - ss_wall_filled);
    } else if (lv_tick_elaps(ss_wall_asked_tick) >= WALL_NEXT_MS) {
        ss_wall_request(1);
    }
}

/* ============================================================ big text */
static lv_obj_t *ss_big_box = NULL, *ss_big_title = NULL, *ss_big_artist = NULL, *ss_big_album = NULL;
static lv_obj_t *ss_big_bar = NULL, *ss_big_el = NULL, *ss_big_total = NULL, *ss_big_clock = NULL;

static lv_obj_t *ss_new_box(lv_obj_t *overlay)
{
    lv_obj_t *b = lv_obj_create(overlay);
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, 1024, 600);
    lv_obj_clear_flag(b, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(b, LV_OBJ_FLAG_HIDDEN);
    return b;
}

static void ss_big_create(lv_obj_t *overlay)
{
    ss_big_box = ss_new_box(overlay);
    lv_obj_t *col = lv_obj_create(ss_big_box);
    lv_obj_remove_style_all(col);
    lv_obj_set_size(col, 884, 330);
    lv_obj_set_pos(col, 70, 90);
    lv_obj_clear_flag(col, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(col, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(col, 8, 0);
    ss_big_title = ui_text(col, "", &p4_font_48, UI_TEXT, 0, 0, 884);
    lv_label_set_long_mode(ss_big_title, LV_LABEL_LONG_WRAP);
    lv_obj_set_height(ss_big_title, LV_SIZE_CONTENT);
    lv_obj_set_style_max_height(ss_big_title, 2 * lv_font_get_line_height(&p4_font_48), 0);
    lv_obj_set_style_pad_bottom(ss_big_title, 6, 0);
    ss_big_artist = ui_text(col, "", &p4_font_28, UI_TEXT_SOFT, 0, 0, 884);
    ss_big_album = ui_text(col, "", &p4_font_18, UI_TEXT_DIM, 0, 0, 884);

    ss_big_bar = lv_bar_create(ss_big_box);
    lv_obj_remove_style_all(ss_big_bar);
    lv_obj_set_size(ss_big_bar, 884, 6);
    lv_obj_set_pos(ss_big_bar, 70, 450);
    lv_bar_set_range(ss_big_bar, 0, 1000);
    lv_obj_set_style_bg_opa(ss_big_bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(ss_big_bar, lv_color_hex(0x2A1E2A), LV_PART_MAIN);
    lv_obj_set_style_radius(ss_big_bar, 3, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(ss_big_bar, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(ss_big_bar, lv_color_hex(UI_ACCENT), LV_PART_INDICATOR);
    lv_obj_set_style_radius(ss_big_bar, 3, LV_PART_INDICATOR);
    lv_obj_clear_flag(ss_big_bar, LV_OBJ_FLAG_CLICKABLE);
    ss_big_el = ui_text(ss_big_box, "", &p4_font_16, UI_TEXT_DIM, 70, 466, 120);
    ss_big_total = ui_text(ss_big_box, "", &p4_font_16, UI_TEXT_DIM, 954 - 120, 466, 120);
    lv_obj_set_style_text_align(ss_big_total, LV_TEXT_ALIGN_RIGHT, 0);
    ss_big_clock = ui_text(ss_big_box, "", &p4_font_28, UI_ACCENT_GLOW, 954 - 220, 30, 220);
    lv_obj_set_style_text_align(ss_big_clock, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_style_text_color(ss_big_clock, ss_dim(UI_ACCENT_GLOW, 70), 0);
}

static void ss_big_tick(void)
{
    /* the same song text the album art view keeps up to date */
    if (ss_art_title != NULL) ui_set_label(ss_big_title, lv_label_get_text(ss_art_title));
    if (ss_art_artist != NULL) ui_set_label(ss_big_artist, lv_label_get_text(ss_art_artist));
    if (ss_art_album != NULL) ui_set_label(ss_big_album, lv_label_get_text(ss_art_album));
    if (ss_art_clock != NULL) ui_set_label(ss_big_clock, lv_label_get_text(ss_art_clock));
    double elapsed = playback_elapsed_s;
    if (current_player_playing && playback_valid) elapsed += (double)lv_tick_elaps(playback_sync_tick) / 1000.0;
    const double duration = playback_duration_s;
    if (duration > 0.0 && elapsed > duration) elapsed = duration;
    char a[16], b[16];
    format_time_value(elapsed, a, sizeof(a));
    ui_set_label(ss_big_el, a);
    if (duration > 0.0) {
        format_time_value(duration, b, sizeof(b));
        ui_set_label(ss_big_total, b);
        lv_bar_set_value(ss_big_bar, (int32_t)(elapsed * 1000.0 / duration), LV_ANIM_OFF);
        lv_obj_clear_flag(ss_big_bar, LV_OBJ_FLAG_HIDDEN);
    } else {
        ui_set_label(ss_big_total, "");
        lv_obj_add_flag(ss_big_bar, LV_OBJ_FLAG_HIDDEN);
    }
}

/* ============================================================ VU meters */
#define SS_VU_W      432
#define SS_VU_H      290
#define SS_VU_R      178.0f          /* scale radius                       */
#define SS_VU_SWEEP  50.0f           /* degrees either side of straight up */
static lv_obj_t *ss_vu_box = NULL, *ss_vu_face[2] = { NULL, NULL }, *ss_vu_song = NULL, *ss_vu_clock = NULL;
static lv_timer_t *ss_vu_timer = NULL;
static float ss_vu_pos[2] = { 0.0f, 0.0f }, ss_vu_vel[2] = { 0.0f, 0.0f };

static void ss_vu_pivot(lv_obj_t *face, int32_t *px, int32_t *py)
{
    lv_area_t c;
    lv_obj_get_coords(face, &c);
    *px = (c.x1 + c.x2) / 2;
    *py = c.y2 - 34;
}

static void ss_vu_draw_cb(lv_event_t *e)
{
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_obj_t *face = (lv_obj_t *)lv_event_get_target(e);
    const int ch = (int)(intptr_t)lv_event_get_user_data(e);
    int32_t cx, cy;
    ss_vu_pivot(face, &cx, &cy);
    const uint32_t glow = UI_ACCENT_GLOW;

    lv_draw_arc_dsc_t ad;
    lv_draw_arc_dsc_init(&ad);
    ad.center.x = cx;
    ad.center.y = cy;
    ad.radius = (uint16_t)(SS_VU_R - 4);
    ad.width = 2;
    ad.start_angle = 270.0f - SS_VU_SWEEP;
    ad.end_angle = 270.0f + SS_VU_SWEEP * 0.6f;
    ad.color = ss_dim(glow, 45);
    lv_draw_arc(layer, &ad);
    ad.start_angle = ad.end_angle;
    ad.end_angle = 270.0f + SS_VU_SWEEP;
    ad.width = 5;
    ad.color = ss_dim(UI_RED, 70);
    lv_draw_arc(layer, &ad);

    lv_draw_line_dsc_t ld;
    lv_draw_line_dsc_init(&ld);
    for (int i = 0; i <= 10; i++) {
        const float deg = -SS_VU_SWEEP + (float)i * (2.0f * SS_VU_SWEEP / 10.0f);
        ld.width = (i % 2) ? 2 : 3;
        ld.color = (i >= 8) ? ss_dim(UI_RED, 80) : ss_dim(glow, 60);
        ld.p1 = ss_an_point(cx, cy, deg, SS_VU_R);
        ld.p2 = ss_an_point(cx, cy, deg, SS_VU_R + ((i % 2) ? 8.0f : 14.0f));
        lv_draw_line(layer, &ld);
    }

    lv_draw_label_dsc_t td;
    lv_draw_label_dsc_init(&td);
    td.font = &p4_font_28;
    td.color = ss_dim(glow, 40);
    td.align = LV_TEXT_ALIGN_CENTER;
    td.text = "VU";
    lv_area_t va = { cx - 40, cy - 80, cx + 40, cy - 46 };
    lv_draw_label(layer, &td, &va);
    td.font = &p4_font_18;
    td.align = LV_TEXT_ALIGN_LEFT;
    td.text = ch ? "R" : "L";
    lv_area_t la;
    lv_obj_get_coords(face, &la);
    la.x1 += 16; la.y1 += 12; la.x2 = la.x1 + 30; la.y2 = la.y1 + 24;
    lv_draw_label(layer, &td, &la);

    /* needle */
    const float deg = -SS_VU_SWEEP + ss_vu_pos[ch] * 2.0f * SS_VU_SWEEP;
    ld.width = 3;
    ld.round_end = 1;
    ld.color = lv_color_hex(UI_TEXT);
    ld.p1.x = cx; ld.p1.y = cy;
    ld.p2 = ss_an_point(cx, cy, deg, SS_VU_R - 16.0f);
    lv_draw_line(layer, &ld);

    lv_draw_rect_dsc_t rd;
    lv_draw_rect_dsc_init(&rd);
    rd.bg_color = ss_dim(glow, 50);
    rd.bg_opa = LV_OPA_COVER;
    rd.radius = LV_RADIUS_CIRCLE;
    lv_area_t hub = { cx - 10, cy - 10, cx + 10, cy + 10 };
    lv_draw_rect(layer, &rd, &hub);
}

/* The area a needle at pos covers (to redraw only that). */
static void ss_vu_needle_area(lv_obj_t *face, float pos, lv_area_t *a)
{
    int32_t cx, cy;
    ss_vu_pivot(face, &cx, &cy);
    const lv_point_precise_t tip = ss_an_point(cx, cy, -SS_VU_SWEEP + pos * 2.0f * SS_VU_SWEEP, SS_VU_R - 16.0f);
    a->x1 = LV_MIN(cx, (int32_t)tip.x) - 12;
    a->x2 = LV_MAX(cx, (int32_t)tip.x) + 12;
    a->y1 = LV_MIN(cy, (int32_t)tip.y) - 12;
    a->y2 = LV_MAX(cy, (int32_t)tip.y) + 12;
}

static void ss_vu_timer_cb(lv_timer_t *t)
{
    (void)t;
    if (!screensaver_active || screensaver_view != SS_VIEW_VU) return;
    const float db[2] = { vu_left_db, vu_right_db };
    for (int ch = 0; ch < 2; ch++) {
        float target = (db[ch] - VU_MIN_DBFS) / (VU_MAX_DBFS - VU_MIN_DBFS);
        if (target < 0.0f) target = 0.0f;
        if (target > 1.0f) target = 1.0f;
        const float old = ss_vu_pos[ch];
        ss_vu_vel[ch] += (target - ss_vu_pos[ch]) * 0.20f;
        ss_vu_vel[ch] *= 0.60f;
        float pos = old + ss_vu_vel[ch];
        if (pos < -0.02f) { pos = -0.02f; ss_vu_vel[ch] = 0.0f; }
        if (pos > 1.03f)  { pos = 1.03f;  ss_vu_vel[ch] = 0.0f; }
        if (fabsf(pos - old) * SS_VU_R * 1.6f > 0.5f) {
            lv_area_t a, b;
            ss_vu_needle_area(ss_vu_face[ch], old, &a);
            ss_vu_needle_area(ss_vu_face[ch], pos, &b);
            ss_vu_pos[ch] = pos;
            lv_area_t u = { LV_MIN(a.x1, b.x1), LV_MIN(a.y1, b.y1), LV_MAX(a.x2, b.x2), LV_MAX(a.y2, b.y2) };
            lv_obj_invalidate_area(ss_vu_face[ch], &u);
        }
    }
}

static void ss_vu_create(lv_obj_t *overlay)
{
    ss_vu_box = ss_new_box(overlay);
    for (int ch = 0; ch < 2; ch++) {
        lv_obj_t *f = lv_obj_create(ss_vu_box);
        lv_obj_remove_style_all(f);
        lv_obj_set_size(f, SS_VU_W, SS_VU_H);
        lv_obj_set_pos(f, ch ? 532 : 60, 110);
        lv_obj_set_style_bg_color(f, lv_color_hex(0x16100A), 0);
        lv_obj_set_style_bg_opa(f, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(f, 18, 0);
        lv_obj_set_style_border_width(f, 2, 0);
        lv_obj_set_style_border_color(f, ss_dim(UI_ACCENT_GLOW, 40), 0);
        lv_obj_clear_flag(f, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_clear_flag(f, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_event_cb(f, ss_vu_draw_cb, LV_EVENT_DRAW_MAIN, (void *)(intptr_t)ch);
        ss_vu_face[ch] = f;
    }
    ss_vu_song = ui_text(ss_vu_box, "", &p4_font_18, UI_TEXT_DIM, 60, 440, 904);
    lv_obj_set_style_text_align(ss_vu_song, LV_TEXT_ALIGN_CENTER, 0);
    ss_vu_clock = ui_text(ss_vu_box, "", &p4_font_28, UI_ACCENT_GLOW, 60, 480, 904);
    lv_obj_set_style_text_align(ss_vu_clock, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(ss_vu_clock, ss_dim(UI_ACCENT_GLOW, 70), 0);
    ss_vu_timer = lv_timer_create(ss_vu_timer_cb, 33, NULL);
    lv_timer_pause(ss_vu_timer);
}

static void ss_vu_tick(void)
{
    char t[SOURCE_TEXT_MAX * 2 + 16];
    const char *ti = ss_art_title ? lv_label_get_text(ss_art_title) : "";
    const char *ar = ss_art_artist ? lv_label_get_text(ss_art_artist) : "";
    if (ti[0] && ar[0]) snprintf(t, sizeof(t), "%s  " LV_SYMBOL_BULLET "  %s", ti, ar);
    else                snprintf(t, sizeof(t), "%s", ti);
    ui_set_label(ss_vu_song, t);
    if (ss_art_clock != NULL) ui_set_label(ss_vu_clock, lv_label_get_text(ss_art_clock));
}

/* ======================================================== weather clock */
static lv_obj_t *ss_wx_box = NULL, *ss_wx_clock = NULL, *ss_wx_date = NULL, *ss_wx_icon = NULL;
static lv_obj_t *ss_wx_temp = NULL, *ss_wx_cond = NULL, *ss_wx_detail = NULL, *ss_wx_src = NULL;
static uint32_t ss_wx_shown_version = 0xFFFFFFFF;
static lv_obj_t *ss_wx_fc_day[WX_FC_DAYS], *ss_wx_fc_icon[WX_FC_DAYS], *ss_wx_fc_temp[WX_FC_DAYS];

static void ss_wx_circle(lv_layer_t *layer, int32_t x, int32_t y, int32_t r, uint32_t rgb, lv_opa_t opa)
{
    lv_draw_rect_dsc_t rd;
    lv_draw_rect_dsc_init(&rd);
    rd.bg_color = lv_color_hex(rgb);
    rd.bg_opa = opa;
    rd.radius = LV_RADIUS_CIRCLE;
    lv_area_t a = { x - r, y - r, x + r, y + r };
    lv_draw_rect(layer, &rd, &a);
}

static void ss_wx_cloud(lv_layer_t *layer, int32_t x, int32_t y, uint32_t rgb)
{
    ss_wx_circle(layer, x - 28, y + 6, 26, rgb, LV_OPA_COVER);
    ss_wx_circle(layer, x + 4, y - 12, 34, rgb, LV_OPA_COVER);
    ss_wx_circle(layer, x + 36, y + 8, 24, rgb, LV_OPA_COVER);
    lv_draw_rect_dsc_t rd;
    lv_draw_rect_dsc_init(&rd);
    rd.bg_color = lv_color_hex(rgb);
    rd.bg_opa = LV_OPA_COVER;
    rd.radius = 12;
    lv_area_t a = { x - 54, y + 4, x + 60, y + 32 };
    lv_draw_rect(layer, &rd, &a);
}

/* user data: -1 = now, 0..2 = forecast day (drawn full size, shown at half
 * size through the object's transform - only redrawn when the data changes). */
static void ss_wx_icon_draw_cb(lv_event_t *e)
{
    if (!wx_have) return;
    const int which = (int)(intptr_t)lv_event_get_user_data(e);
    if (which >= wx_fc_n) return;
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_obj_t *obj = (lv_obj_t *)lv_event_get_target(e);
    lv_area_t c;
    lv_obj_get_coords(obj, &c);
    const int32_t x = (c.x1 + c.x2) / 2, y = (c.y1 + c.y2) / 2;
    wx_icon_t icon;
    (void)weather_words(which < 0 ? wx_code : wx_fc_code[which], &icon);
    if (which >= 0 && icon == WX_ICON_MOON) icon = WX_ICON_SUN;      /* days, not nights */
    const uint32_t sun = 0xFFC857, cloud = 0xB8A8B4, dark = 0x7A6A78, drop = 0x7FB2E5;
    lv_draw_line_dsc_t ld;
    lv_draw_line_dsc_init(&ld);
    ld.round_start = ld.round_end = 1;
    switch (icon) {
        case WX_ICON_SUN:
            ld.width = 4;
            ld.color = lv_color_hex(sun);
            for (int k = 0; k < 8; k++) {
                ld.p1 = ss_an_point(x, y, (float)k * 45.0f, 36.0f);
                ld.p2 = ss_an_point(x, y, (float)k * 45.0f, 50.0f);
                lv_draw_line(layer, &ld);
            }
            ss_wx_circle(layer, x, y, 26, sun, LV_OPA_COVER);
            break;
        case WX_ICON_MOON:
            ss_wx_circle(layer, x, y, 32, 0xE8DCC0, LV_OPA_COVER);
            ss_wx_circle(layer, x + 16, y - 10, 28, 0x000000, LV_OPA_COVER);
            break;
        case WX_ICON_PARTLY:
            ss_wx_circle(layer, x + 22, y - 18, 26, sun, LV_OPA_COVER);
            ss_wx_cloud(layer, x - 6, y + 8, cloud);
            break;
        case WX_ICON_FOG:
            ld.width = 6;
            ld.color = lv_color_hex(cloud);
            for (int k = 0; k < 4; k++) {
                ld.p1.x = x - 54 + (k % 2) * 10; ld.p1.y = y - 24 + k * 16;
                ld.p2.x = x + 54 - (k % 2) * 10; ld.p2.y = ld.p1.y;
                lv_draw_line(layer, &ld);
            }
            break;
        case WX_ICON_RAIN:
        case WX_ICON_SNOW:
        case WX_ICON_STORM:
            ss_wx_cloud(layer, x, y - 14, icon == WX_ICON_STORM ? dark : cloud);
            if (icon == WX_ICON_SNOW) {
                for (int k = 0; k < 3; k++) ss_wx_circle(layer, x - 28 + k * 28, y + 38 + (k % 2) * 8, 5, 0xFFFFFF, LV_OPA_COVER);
            } else if (icon == WX_ICON_STORM) {
                ld.width = 5;
                ld.color = lv_color_hex(sun);
                ld.p1.x = x + 6;  ld.p1.y = y + 20; ld.p2.x = x - 8; ld.p2.y = y + 40; lv_draw_line(layer, &ld);
                ld.p1 = ld.p2;    ld.p2.x = x + 8; ld.p2.y = y + 40; lv_draw_line(layer, &ld);
                ld.p1 = ld.p2;    ld.p2.x = x - 6; ld.p2.y = y + 60; lv_draw_line(layer, &ld);
            } else {
                ld.width = 4;
                ld.color = lv_color_hex(drop);
                for (int k = 0; k < 4; k++) {
                    ld.p1.x = x - 36 + k * 24; ld.p1.y = y + 30;
                    ld.p2.x = ld.p1.x - 8;     ld.p2.y = y + 50;
                    lv_draw_line(layer, &ld);
                }
            }
            break;
        case WX_ICON_CLOUD:
        default:
            ss_wx_cloud(layer, x, y, cloud);
            break;
    }
}

static void ss_wx_create(lv_obj_t *overlay)
{
    ss_wx_box = ss_new_box(overlay);
    ss_wx_clock = ui_text(ss_wx_box, "", &p4_font_48, UI_ACCENT_GLOW, 0, 110, 1024);
    lv_obj_set_style_text_align(ss_wx_clock, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_letter_space(ss_wx_clock, 3, 0);
    ss_wx_date = ui_text(ss_wx_box, "", &p4_font_28, UI_DATE, 0, 186, 1024);
    lv_obj_set_style_text_align(ss_wx_date, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_letter_space(ss_wx_date, 2, 0);

    lv_obj_t *rule = lv_obj_create(ss_wx_box);
    lv_obj_remove_style_all(rule);
    lv_obj_set_size(rule, 400, 1);
    lv_obj_set_pos(rule, 312, 250);
    lv_obj_set_style_bg_color(rule, lv_color_hex(0x322332), 0);
    lv_obj_set_style_bg_opa(rule, LV_OPA_COVER, 0);

    ss_wx_icon = lv_obj_create(ss_wx_box);
    lv_obj_remove_style_all(ss_wx_icon);
    lv_obj_set_size(ss_wx_icon, 140, 140);
    lv_obj_set_pos(ss_wx_icon, 290, 280);
    lv_obj_clear_flag(ss_wx_icon, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(ss_wx_icon, ss_wx_icon_draw_cb, LV_EVENT_DRAW_MAIN, (void *)(intptr_t)-1);

    /* the next three days */
    for (int k = 0; k < WX_FC_DAYS; k++) {
        const int cx = 512 + (k - 1) * 150;
        ss_wx_fc_day[k] = ui_text(ss_wx_box, "", &p4_font_16, UI_TEXT_SOFT, cx - 60, 438, 120);
        lv_obj_set_style_text_align(ss_wx_fc_day[k], LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_t *ic = lv_obj_create(ss_wx_box);
        lv_obj_remove_style_all(ic);
        lv_obj_set_size(ic, 140, 140);
        lv_obj_set_pos(ic, cx - 70, 490 - 70);
        lv_obj_set_style_transform_pivot_x(ic, 70, 0);
        lv_obj_set_style_transform_pivot_y(ic, 70, 0);
        lv_obj_set_style_transform_scale(ic, 110, 0);          /* ~43 % */
        lv_obj_clear_flag(ic, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(ic, ss_wx_icon_draw_cb, LV_EVENT_DRAW_MAIN, (void *)(intptr_t)k);
        ss_wx_fc_icon[k] = ic;
        ss_wx_fc_temp[k] = ui_text(ss_wx_box, "", &p4_font_16, UI_TEXT_DIM, cx - 60, 528, 120);
        lv_obj_set_style_text_align(ss_wx_fc_temp[k], LV_TEXT_ALIGN_CENTER, 0);
    }

    ss_wx_temp = ui_text(ss_wx_box, "", &p4_font_48, UI_TEXT, 440, 318, 170);
    ss_wx_cond = ui_text(ss_wx_box, "", &p4_font_28, UI_TEXT_SOFT, 580, 300, 400);
    ss_wx_detail = ui_text(ss_wx_box, "", &p4_font_16, UI_TEXT_DIM, 580, 342, 400);
    ss_wx_src = ui_text(ss_wx_box, "", &p4_font_14, UI_TEXT_DIM, 580, 370, 400);
    lv_obj_set_style_text_color(ss_wx_src, ss_dim(0x9A7688, 70), 0);
}

static void ss_wx_tick(void)
{
    if (ss_art_clock != NULL) ui_set_label(ss_wx_clock, lv_label_get_text(ss_art_clock));
    time_t now = time(NULL);
    struct tm t;
    if (localtime_r(&now, &t) != NULL && time_is_valid(&t)) {
        char d[48];
        ss_clk_date_text(&t, d, sizeof(d));      /* Settings > Clock style date */
        for (char *p = d; *p; p++) *p = (char)toupper((unsigned char)*p);
        ui_set_label(ss_wx_date, d);
    }
    if (wx_version == ss_wx_shown_version) return;
    ss_wx_shown_version = wx_version;
    if (wx_have) {
        char tmp[16], det[96];
        snprintf(tmp, sizeof(tmp), "%.0f\xC2\xB0", (double)wx_temp);          /* degree sign */
        ui_set_label(ss_wx_temp, tmp);
        wx_icon_t icon;
        ui_set_label(ss_wx_cond, weather_words(wx_code, &icon));
        det[0] = '\0';
        if (wx_humidity >= 0) snprintf(det, sizeof(det), "Humidity %d%%", wx_humidity);
        if (wx_wind >= 0.0f) {
            const size_t n = strlen(det);
            snprintf(det + n, sizeof(det) - n, "%sWind %.0f %s", n ? "  " LV_SYMBOL_BULLET "  " : "",
                     (double)wx_wind, wx_units ? "mph" : "km/h");
        }
        ui_set_label(ss_wx_detail, det);
        ui_set_label(ss_wx_src, wx_place);
        for (int k = 0; k < WX_FC_DAYS; k++) {
            if (k < wx_fc_n) {
                char ft[24];
                snprintf(ft, sizeof(ft), "%.0f\xC2\xB0 / %.0f\xC2\xB0", (double)wx_fc_hi[k], (double)wx_fc_lo[k]);
                ui_set_label(ss_wx_fc_day[k], wx_fc_day[k]);
                ui_set_label(ss_wx_fc_temp[k], ft);
            } else {
                ui_set_label(ss_wx_fc_day[k], "");
                ui_set_label(ss_wx_fc_temp[k], "");
            }
            lv_obj_invalidate(ss_wx_fc_icon[k]);
        }
    } else {
        ui_set_label(ss_wx_temp, "");
        ui_set_label(ss_wx_cond, "");
        ui_set_label(ss_wx_detail, wx_status);
        ui_set_label(ss_wx_src, "");
    }
    lv_obj_invalidate(ss_wx_icon);
}

/* ========================================================= hooks (07) */
static void ss_extra_create(lv_obj_t *overlay)
{
    ss_an_create(overlay);
    ss_wall_create(overlay);
    ss_big_create(overlay);
    ss_vu_create(overlay);
    ss_wx_create(overlay);
}

static void ss_extra_show(int view)
{
    if (ss_an_box == NULL || ss_wall_box == NULL || ss_wx_box == NULL) return;
    lv_obj_t *boxes[3] = { ss_big_box, ss_vu_box, ss_wx_box };
    const int views[3] = { SS_VIEW_BIGTEXT, SS_VIEW_VU, SS_VIEW_WEATHER };
    for (int k = 0; k < 3; k++) {
        if (view == views[k]) lv_obj_clear_flag(boxes[k], LV_OBJ_FLAG_HIDDEN);
        else                  lv_obj_add_flag(boxes[k], LV_OBJ_FLAG_HIDDEN);
    }
    if (view == SS_VIEW_VU) lv_timer_resume(ss_vu_timer); else lv_timer_pause(ss_vu_timer);
    if (view == SS_VIEW_BIGTEXT) ss_big_tick();
    if (view == SS_VIEW_VU) ss_vu_tick();
    if (view == SS_VIEW_WEATHER) {
        ss_wx_shown_version = 0xFFFFFFFF;
        wx_want_now = wx_want_now || !wx_have;
        ss_wx_tick();
    }
    if (view == SS_VIEW_ANALOG) {
        lv_obj_clear_flag(ss_an_box, LV_OBJ_FLAG_HIDDEN);
        ss_an_last_sec = -1;
        ss_an_tick();
    } else {
        lv_obj_add_flag(ss_an_box, LV_OBJ_FLAG_HIDDEN);
    }
    if (view == SS_VIEW_WALL) {
        lv_obj_clear_flag(ss_wall_box, LV_OBJ_FLAG_HIDDEN);
        ss_wall_asked = false;                 /* fresh covers soon after it appears */
        ss_wall_asked_tick = lv_tick_get() - WALL_NEXT_MS + 2000;
        ss_wall_tick();
    } else {
        lv_obj_add_flag(ss_wall_box, LV_OBJ_FLAG_HIDDEN);
    }
}

static void ss_extra_tick(void)
{
    if (screensaver_view == SS_VIEW_ANALOG) ss_an_tick();
    else if (screensaver_view == SS_VIEW_WALL) ss_wall_tick();
    else if (screensaver_view == SS_VIEW_BIGTEXT) ss_big_tick();
    else if (screensaver_view == SS_VIEW_VU) ss_vu_tick();
    else if (screensaver_view == SS_VIEW_WEATHER) ss_wx_tick();
}

static void ss_extra_shift(int index)
{
    const lv_point_t *d = &ss_art_drift[index % SS_ART_DRIFT_COUNT];
    if (screensaver_view == SS_VIEW_ANALOG && ss_an_box != NULL) {
        lv_obj_set_pos(ss_an_box, d->x, d->y / 2);
    } else if (screensaver_view == SS_VIEW_WALL && ss_wall_box != NULL) {
        lv_obj_set_pos(ss_wall_box, d->x / 2, d->y / 2);
    } else if (screensaver_view == SS_VIEW_BIGTEXT && ss_big_box != NULL) {
        lv_obj_set_pos(ss_big_box, d->x, d->y);
    } else if (screensaver_view == SS_VIEW_VU && ss_vu_box != NULL) {
        lv_obj_set_pos(ss_vu_box, d->x / 2, d->y / 2);
    } else if (screensaver_view == SS_VIEW_WEATHER && ss_wx_box != NULL) {
        lv_obj_set_pos(ss_wx_box, d->x, d->y / 2);
    }
}

/* The screensaver closed: its VU meters' 30-a-second timer stops too. */
static void ss_extra_hidden(void)
{
    if (ss_vu_timer != NULL) lv_timer_pause(ss_vu_timer);
}
