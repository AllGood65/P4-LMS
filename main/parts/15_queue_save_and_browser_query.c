/*
 * P4-LMS - Save-queue-as-playlist, list drawing, Lyrion browse queries, queue commands
 *
 * Part of main.c.  This file is #included by main.c (in order) and is not
 * compiled on its own, so it shares main.c's includes and static names.
 */

/* ------------------------------------------------------------------------
 * SAVE QUEUE AS PLAYLIST - a name box and on-screen keyboard over SOURCE
 * ------------------------------------------------------------------------ */
static void qs_hide(void)
{
    if (qs_backdrop != NULL) {
        lv_obj_add_flag(qs_backdrop, LV_OBJ_FLAG_HIDDEN);
    }
}

static void qs_close_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    qs_hide();
}

/* Called from the browser task with the display lock held. */
static void qs_set_status_locked(const char *text, uint32_t color, bool close_soon)
{
    if (qs_status == NULL) return;
    lv_label_set_text(qs_status, text);
    lv_obj_set_style_text_color(qs_status, lv_color_hex(color), 0);
    if (close_soon) {
        lv_timer_t *t = lv_timer_create(qs_close_timer_cb, 1600, NULL);
        if (t != NULL) lv_timer_set_repeat_count(t, 1);
    }
}

/* Keep the name safe as both a file name and a JSON string. */
static void qs_clean_name(const char *in, char *out, size_t out_size)
{
    size_t n = 0;
    while (*in == ' ') in++;
    for (; *in && n + 1 < out_size; in++) {
        const char c = *in;
        if (c == '"' || c == '\\' || c == '/' || c == ':' || c == '*' || c == '?' ||
            c == '<' || c == '>' || c == '|' || (unsigned char)c < 0x20) {
            continue;
        }
        out[n++] = c;
    }
    while (n > 0 && out[n - 1] == ' ') n--;
    out[n] = '\0';
}

static void qs_submit(void)
{
    if (qs_textarea == NULL || source_queue == NULL) return;

    char name[SOURCE_TEXT_MAX];
    qs_clean_name(lv_textarea_get_text(qs_textarea), name, sizeof(name));
    if (name[0] == '\0') {
        qs_set_status_locked("Type a name for the playlist first.", UI_RED, false);
        return;
    }

    if (qs_mode == 1) {                      /* new playlist, then add what was chosen */
        source_request_t r = { .mode = SOURCE_PL_NEW_ADD, .push_history = false,
                               .queue_op = pl_pending.kind, .offset = pl_pending.count };
        snprintf(r.item_id, sizeof(r.item_id), "%.90s", name);
        snprintf(r.radio_action, sizeof(r.radio_action), "%s", pl_pending.payload);
        snprintf(r.title, sizeof(r.title), "%s", pl_pending.what);
        qs_set_status_locked("Creating the playlist...", UI_TEXT_SOFT, false);
        xQueueSend(source_queue, &r, 0);
        return;
    }
    if (qs_mode == 2) {                      /* rename the playlist on screen */
        source_request_t r = { .mode = SOURCE_PL_RENAME, .id = source_parent_id, .push_history = false,
                               .offset = source_page_offset };
        snprintf(r.title, sizeof(r.title), "%s", name);
        qs_set_status_locked("Renaming...", UI_TEXT_SOFT, false);
        xQueueSend(source_queue, &r, 0);
        return;
    }

    source_request_t request = { .mode = SOURCE_QUEUE_SAVE, .push_history = false };
    snprintf(request.title, sizeof(request.title), "%s", name);
    /* A second SAVE with the same name after the "already exists" warning
     * means: replace it. */
    request.id = (strcasecmp(name, qs_replace_name) == 0) ? 1 : 0;
    qs_set_status_locked("Saving...", UI_TEXT_SOFT, false);
    xQueueSend(source_queue, &request, 0);
}

static void qs_save_btn_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) qs_submit();
}

static void qs_cancel_btn_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) qs_hide();
}

/* The keyboard's own tick key saves, its close key cancels. */
static void qs_keyboard_cb(lv_event_t *e)
{
    const lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_READY) qs_submit();
    else if (code == LV_EVENT_CANCEL) qs_hide();
}

/* Typing a different name forgets an earlier "replace?" warning. */
static void qs_text_changed_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    if (qs_replace_name[0] != '\0') {
        qs_replace_name[0] = '\0';
        qs_set_status_locked("", UI_TEXT_SOFT, false);
    }
}

static void qs_create(void)
{
    /* Full-screen while typing, so the keyboard can be as large as possible. */
    qs_backdrop = lv_obj_create(lv_screen_active());
    lv_obj_remove_style_all(qs_backdrop);
    lv_obj_set_size(qs_backdrop, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(qs_backdrop, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(qs_backdrop, LV_OPA_80, 0);
    lv_obj_clear_flag(qs_backdrop, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(qs_backdrop, LV_OBJ_FLAG_CLICKABLE);       /* nothing behind it reacts */

    lv_obj_t *panel = lv_obj_create(qs_backdrop);
    lv_obj_remove_style_all(panel);
    lv_obj_set_size(panel, 996, 196);
    lv_obj_set_pos(panel, 14, 10);
    lv_obj_set_style_bg_color(panel, lv_color_hex(UI_PANEL), 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(panel, 14, 0);
    lv_obj_set_style_border_width(panel, 2, 0);
    lv_obj_set_style_border_color(panel, lv_color_hex(UI_AMBER), 0);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(panel);
    qs_title_label = title;
    lv_label_set_text(title, "SAVE QUEUE AS A PLAYLIST");
    lv_obj_set_style_text_color(title, lv_color_hex(UI_TEXT), 0);
    lv_obj_set_style_text_font(title, &p4_font_18, 0);
    lv_obj_set_pos(title, 24, 12);

    qs_textarea = lv_textarea_create(panel);
    lv_textarea_set_one_line(qs_textarea, true);
    lv_textarea_set_max_length(qs_textarea, 60);
    lv_obj_set_size(qs_textarea, 948, 54);
    lv_obj_set_pos(qs_textarea, 24, 40);
    lv_obj_set_style_text_font(qs_textarea, &p4_font_18, 0);
    lv_obj_set_style_bg_color(qs_textarea, lv_color_hex(UI_BG), 0);
    lv_obj_set_style_text_color(qs_textarea, lv_color_hex(UI_TEXT), 0);
    lv_obj_set_style_border_color(qs_textarea, lv_color_hex(UI_ACCENT), 0);
    lv_obj_set_style_border_width(qs_textarea, 2, 0);
    lv_obj_add_event_cb(qs_textarea, qs_text_changed_cb, LV_EVENT_VALUE_CHANGED, NULL);

    qs_status = lv_label_create(panel);
    lv_label_set_text(qs_status, "");
    lv_obj_set_width(qs_status, 620);
    lv_label_set_long_mode(qs_status, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(qs_status, &p4_font_14, 0);
    lv_obj_set_pos(qs_status, 24, 106);

    lv_obj_t *cancel = create_button(panel, "CANCEL", 140, 50);
    lv_obj_set_pos(cancel, 680, 132);
    lv_obj_add_event_cb(cancel, qs_cancel_btn_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *save = create_button(panel, "SAVE", 140, 50);
    lv_obj_set_pos(save, 832, 132);
    lv_obj_set_style_bg_color(save, lv_color_hex(UI_ACCENT), LV_PART_MAIN);
    lv_obj_set_style_text_color(lv_obj_get_child(save, 0), lv_color_hex(UI_ON_ACCENT), 0);
    lv_obj_add_event_cb(save, qs_save_btn_cb, LV_EVENT_CLICKED, NULL);

    qs_keyboard = lv_keyboard_create(qs_backdrop);
    lv_obj_set_size(qs_keyboard, 1004, 372);
    /* The keyboard aligns itself to its parent's bottom centre when it is
     * created, and a plain set_pos() would only offset it from there (off the
     * bottom of the screen) - so set the alignment explicitly. */
    lv_obj_align(qs_keyboard, LV_ALIGN_BOTTOM_MID, 0, -8);
    lv_obj_set_style_text_font(qs_keyboard, &p4_font_18, LV_PART_ITEMS);
    lv_obj_set_style_pad_row(qs_keyboard, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_column(qs_keyboard, 8, LV_PART_MAIN);
    lv_keyboard_set_textarea(qs_keyboard, qs_textarea);
    lv_obj_add_event_cb(qs_keyboard, qs_keyboard_cb, LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(qs_keyboard, qs_keyboard_cb, LV_EVENT_CANCEL, NULL);

    lv_obj_add_flag(qs_backdrop, LV_OBJ_FLAG_HIDDEN);
}

/* Open the name panel: mode 0 save queue, 1 new playlist, 2 rename. */
static void qs_open(int mode, const char *title, const char *text, const char *status)
{
    if (qs_backdrop == NULL) qs_create();
    qs_mode = mode;
    lv_label_set_text(qs_title_label, title);
    lv_textarea_set_text(qs_textarea, text);
    qs_replace_name[0] = '\0';
    qs_set_status_locked(status, UI_TEXT_SOFT, false);
    lv_obj_clear_flag(qs_backdrop, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(qs_backdrop);
}

/* PLAY in the queue view: start the queue from its first song. */
static void source_queue_play_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED || source_queue == NULL) return;
    /* queue_op = 1 on a jump means: return to Now Playing once it starts. */
    source_request_t request = { .mode = SOURCE_QUEUE_JUMP, .id = 0, .offset = 0,
                                 .push_history = false, .queue_op = 1 };
    xQueueSend(source_queue, &request, 0);
}

static void source_save_button_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED || source_overlay == NULL) return;
    if (source_current_mode == SOURCE_PLAYLIST_TRACKS) {       /* this button is RENAME here */
        qs_open(2, "RENAME PLAYLIST", source_folder_title, "Type the new name, then SAVE.");
        return;
    }

    /* Suggest a dated name; it is selected-free, so just clear it to type your own. */
    char name[40] = "My Queue";
    time_t now = time(NULL);
    struct tm local;
    if (localtime_r(&now, &local) != NULL && time_is_valid(&local)) {
        strftime(name, sizeof(name), "Queue %d %b %H.%M", &local);
    }
    qs_open(0, "SAVE QUEUE AS A PLAYLIST", name, "Keep this name or type your own, then SAVE.");
}

static void source_play_context_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (source_queue == NULL) return;

    /* In the queue view this button is CLEAR QUEUE, and needs a second tap
     * within 3 seconds so a stray touch cannot wipe the queue. */
    if (source_current_mode == SOURCE_QUEUE) {
        static bool armed = false;
        static uint32_t armed_tick = 0;
        lv_obj_t *label = lv_obj_get_child(source_play_context_button, 0);
        if (!armed || lv_tick_elaps(armed_tick) > 3000) {
            armed = true;
            armed_tick = lv_tick_get();
            if (label) lv_label_set_text(label, "TAP AGAIN");
            lv_timer_t *t = lv_timer_create(source_clear_confirm_reset_cb, 3000, NULL);
            if (t != NULL) lv_timer_set_repeat_count(t, 1);
            return;
        }
        armed = false;
        if (label) lv_label_set_text(label, "CLEAR QUEUE");
        source_request_t request = { .mode = SOURCE_QUEUE_CLEAR, .push_history = false };
        xQueueSend(source_queue, &request, 0);
        return;
    }
    if (source_parent_id <= 0) {
        ESP_LOGW(TAG, "Cannot play source: invalid parent id");
        return;
    }

    source_mode_t load_mode;
    const char *what;

    switch (source_current_mode) {
        case SOURCE_FOLDER_CONTENT:
            load_mode = SOURCE_LOAD_FOLDER;
            what = "folder";
            break;
        case SOURCE_ARTIST_ALBUMS:
            load_mode = SOURCE_LOAD_ARTIST;
            what = "artist";
            break;
        case SOURCE_GENRE_ALBUMS:
            load_mode = SOURCE_LOAD_GENRE;
            what = "genre";
            break;
        case SOURCE_ALBUM_TRACKS:
            load_mode = SOURCE_LOAD_ALBUM;
            what = "album";
            break;
        case SOURCE_PLAYLIST_TRACKS:
            load_mode = SOURCE_LOAD_PLAYLIST;
            what = "playlist";
            break;
        default:
            return;
    }

    source_request_t request = {
        .mode = load_mode,
        .id = source_parent_id,
        .offset = 0
    };

    ESP_LOGI(TAG, "Playing entire %s id %d", what, source_parent_id);
    xQueueSend(source_queue, &request, 0);
}

/* Albums as a 5 x 2 grid of covers.  Each tile's children, in order:
 * 0 placeholder, 1 cover image (hidden until it arrives), 2 title,
 * 3 artist, 4 "+" button.  Covers are fetched afterwards by the browser
 * task (source_grid_fetch_covers). */
static void source_render_grid(void)
{
    const int tile_w = 174, tile_h = 176, x0 = 30, y0 = 84;
    for (int i = 0; i < source_item_count && i < SOURCE_MAX_ITEMS; i++) {
        const int col = i % SOURCE_GRID_COLS, row = i / SOURCE_GRID_COLS;
        lv_obj_t *tile = lv_obj_create(source_overlay);
        lv_obj_remove_style_all(tile);
        lv_obj_set_size(tile, tile_w - 10, tile_h);
        lv_obj_set_pos(tile, x0 + col * tile_w, y0 + row * (tile_h + 2));
        lv_obj_set_style_radius(tile, 12, 0);
        lv_obj_set_style_bg_color(tile, lv_color_hex(UI_ACCENT_DARK), LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(tile, LV_OPA_COVER, LV_STATE_PRESSED);
        lv_obj_clear_flag(tile, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(tile, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(tile, source_item_event_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);

        lv_obj_t *ph = lv_obj_create(tile);                                   /* 0 */
        lv_obj_remove_style_all(ph);
        lv_obj_set_size(ph, SOURCE_GRID_COVER, SOURCE_GRID_COVER);
        lv_obj_set_pos(ph, 14, 0);
        lv_obj_set_style_bg_color(ph, lv_color_hex(UI_ALBUM_BG), 0);
        lv_obj_set_style_bg_opa(ph, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(ph, 8, 0);
        lv_obj_clear_flag(ph, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_t *note = lv_label_create(ph);
        lv_label_set_text(note, LV_SYMBOL_AUDIO);
        lv_obj_set_style_text_color(note, lv_color_hex(UI_TEXT_DIM), 0);
        lv_obj_set_style_text_font(note, &p4_font_28, 0);
        lv_obj_center(note);

        lv_obj_t *img = lv_image_create(tile);                                /* 1 */
        lv_obj_set_pos(img, 14, 0);
        lv_obj_clear_flag(img, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(img, LV_OBJ_FLAG_HIDDEN);

        char clean[SOURCE_TEXT_MAX];
        source_strip_audio_extension(source_items[i].title, clean, sizeof(clean));
        lv_obj_t *title = lv_label_create(tile);                              /* 2 */
        lv_label_set_text(title, clean);
        lv_obj_set_width(title, SOURCE_GRID_COVER + 4);
        lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_font(title, &p4_font_14, 0);
        label_one_line(title);
        lv_obj_set_style_text_color(title, lv_color_hex(UI_TEXT), 0);
        lv_obj_set_pos(title, 14, SOURCE_GRID_COVER + 4);

        lv_obj_t *artist = lv_label_create(tile);                             /* 3 */
        lv_label_set_text(artist, source_items[i].subtitle);
        lv_obj_set_width(artist, SOURCE_GRID_COVER + 4);
        lv_label_set_long_mode(artist, LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_font(artist, &p4_font_14, 0);
        label_one_line(artist);
        lv_obj_set_style_text_color(artist, lv_color_hex(UI_TEXT_SOFT), 0);
        lv_obj_set_pos(artist, 14, SOURCE_GRID_COVER + 20);

        lv_obj_t *plus = lv_obj_create(tile);                                 /* 4 */
        lv_obj_remove_style_all(plus);
        lv_obj_set_size(plus, 34, 34);
        lv_obj_set_pos(plus, 14 + SOURCE_GRID_COVER - 40, SOURCE_GRID_COVER - 40);
        lv_obj_set_style_bg_color(plus, lv_color_hex(UI_ACCENT), 0);
        lv_obj_set_style_bg_opa(plus, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(plus, 8, 0);
        lv_obj_set_style_bg_color(plus, lv_color_hex(UI_ACCENT_DARK), LV_STATE_PRESSED);
        lv_obj_clear_flag(plus, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(plus, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_ext_click_area(plus, 6);
        lv_obj_t *pl = lv_label_create(plus);
        lv_label_set_text(pl, LV_SYMBOL_PLUS);
        lv_obj_set_style_text_color(pl, lv_color_hex(UI_ON_ACCENT), 0);
        lv_obj_center(pl);
        lv_obj_add_event_cb(plus, source_plus_event_cb, LV_EVENT_SHORT_CLICKED, (void *)(intptr_t)i);
        lv_obj_add_event_cb(plus, source_plus_event_cb, LV_EVENT_LONG_PRESSED, (void *)(intptr_t)i);

        source_item_buttons[i] = tile;
    }
}

/* A grid cover arrives (browser task -> LVGL task).  Dropped if the page
 * changed meanwhile; then the task frees the pixels. */
typedef struct { uint32_t gen; int index; uint8_t *pixels; uint32_t w, h, padded; size_t size; bool shown; } grid_cover_ctx_t;

static void source_grid_cover_job(void *arg)
{
    grid_cover_ctx_t *c = (grid_cover_ctx_t *)arg;
    if (c->gen != source_grid_gen || c->index < 0 || c->index >= SOURCE_MAX_ITEMS) return;
    lv_obj_t *tile = source_item_buttons[c->index];
    if (tile == NULL || source_overlay == NULL || lv_obj_has_flag(source_overlay, LV_OBJ_FLAG_HIDDEN)) return;

    if (source_grid_pixels[c->index] != NULL) heap_caps_free(source_grid_pixels[c->index]);
    source_grid_pixels[c->index] = c->pixels;
    lv_image_dsc_t *d = &source_grid_dsc[c->index];
    memset(d, 0, sizeof(*d));
    d->header.cf = LV_COLOR_FORMAT_RGB565;
    d->header.w = c->w;
    d->header.h = c->h;
    d->header.stride = c->padded * 2U;
    d->data_size = c->size;
    d->data = c->pixels;
    lv_obj_t *img = lv_obj_get_child(tile, 1);
    lv_image_set_src(img, d);
    lv_obj_clear_flag(img, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(lv_obj_get_child(tile, 0), LV_OBJ_FLAG_HIDDEN);
    c->shown = true;
}

/* Fetch the page's covers one by one; stop as soon as the user asks for
 * something else (a new request is waiting) or the page is redrawn. */
static void source_grid_fetch_covers(void)
{
    const uint32_t gen = source_grid_gen;
    int shown = 0, missing = 0, failed = 0;
    for (int i = 0; i < source_item_count && i < SOURCE_MAX_ITEMS; i++) {
        if (gen != source_grid_gen || uxQueueMessagesWaiting(source_queue) > 0) return;
        const char *key = source_items[i].cover_key;
        if (!key[0]) { missing++; continue; }
        grid_cover_ctx_t c = { .gen = gen, .index = i };
        if (!source_download_cover(key, SOURCE_GRID_COVER, &c.pixels, &c.w, &c.h, &c.padded, &c.size)) {
            if (failed++ == 0) {
                ESP_LOGW(TAG, "Grid cover: could not load %s/music/%s/cover_%dx%d.png",
                         LYRION_SERVER_BASE, key, SOURCE_GRID_COVER, SOURCE_GRID_COVER);
            }
            continue;
        }
        shown++;
        ui_call(source_grid_cover_job, &c);
        if (!c.shown) heap_caps_free(c.pixels);
    }
    ESP_LOGI(TAG, "Grid covers: %d shown, %d failed, %d without a cover id", shown, failed, missing);
}

/* The Queue: one song per row, full width.  Tap a row to play that song;
 * "..." on a row opens its move controls (14_power_and_browser_rows.c). */
static void source_render_queue(void)
{
    /* Live radio: one station card instead of a list with sort buttons. */
    if (source_queue_radio && source_item_count == 1) {
        lv_obj_t *card = lv_obj_create(source_overlay);
        lv_obj_remove_style_all(card);
        lv_obj_set_size(card, 870, 150);
        lv_obj_set_pos(card, SOURCE_GRID_X0, 96);
        lv_obj_set_style_bg_color(card, lv_color_hex(UI_ACCENT_DARK), 0);
        lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(card, 12, 0);
        lv_obj_set_style_border_width(card, 2, 0);
        lv_obj_set_style_border_color(card, lv_color_hex(UI_ACCENT), 0);
        lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(card, LV_OBJ_FLAG_CLICKABLE);
        source_item_buttons[0] = card;          /* so the next view clears it */

        lv_obj_t *tag = lv_label_create(card);
        lv_label_set_text(tag, LV_SYMBOL_AUDIO "  LIVE RADIO");
        lv_obj_set_style_text_font(tag, &p4_font_16, 0);
        lv_obj_set_style_text_color(tag, lv_color_hex(UI_ACCENT), 0);
        lv_obj_set_pos(tag, 24, 20);

        lv_obj_t *name = lv_label_create(card);
        lv_label_set_text(name, source_items[0].title);
        lv_obj_set_style_text_font(name, &p4_font_28, 0);
        lv_obj_set_style_text_color(name, lv_color_hex(UI_TEXT), 0);
        lv_obj_set_width(name, 870 - 48);
        lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
        label_one_line(name);
        lv_obj_set_pos(name, 24, 52);

        lv_obj_t *note = lv_label_create(card);
        lv_label_set_text(note, "A station plays on its own. Playing songs or an album replaces it.");
        lv_obj_set_style_text_font(note, &p4_font_14, 0);
        lv_obj_set_style_text_color(note, lv_color_hex(UI_TEXT_SOFT), 0);
        lv_obj_set_width(note, 870 - 48);
        lv_label_set_long_mode(note, LV_LABEL_LONG_DOT);
        label_one_line(note);
        lv_obj_set_pos(note, 24, 108);
        return;
    }

    for (int i = 0; i < source_item_count && i < SOURCE_QUEUE_PAGE; i++) {
        char clean[SOURCE_TEXT_MAX];
        source_strip_audio_extension(source_items[i].title, clean, sizeof(clean));
        lv_obj_t *row = create_source_item_button(source_overlay, clean, 870, SOURCE_QUEUE_ROW_H - 4,
                                                  false, true);
        lv_obj_set_pos(row, SOURCE_GRID_X0, 84 + i * SOURCE_QUEUE_ROW_H);
        lv_obj_add_event_cb(row, source_item_event_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        source_item_buttons[i] = row;
        queue_row_add_controls(row, i);
    }
    queue_rows_apply_selection();
}

static void source_render_items(void)
{
    /* Caller already owns the LVGL lock.  Never call bsp_display_lock()
     * from this function or from helpers it invokes. */
    int parsed_count = source_item_count;
    source_clear_items();
    source_item_count = parsed_count;

    /* Ticks belong to one list: a different view ends SELECT. */
    if (source_select_mode && source_view_key() != source_sel_view_key) {
        source_select_set(false);
    }

    if (source_item_count == 0) {
        if (source_empty_label != NULL) {
            lv_label_set_text(source_empty_label,
                              source_current_mode == SOURCE_QUEUE ? "THE QUEUE IS EMPTY"
                              : source_current_mode == SOURCE_PLAYLIST_TRACKS
                                ? "THIS PLAYLIST IS EMPTY - ADD SONGS FROM NOW PLAYING, THE QUEUE OR SELECT"
                              : source_current_mode == SOURCE_SEARCH ? "NOTHING FOUND - TRY FEWER LETTERS"
                              : "NO ITEMS FOUND");
            lv_obj_clear_flag(source_empty_label, LV_OBJ_FLAG_HIDDEN);
        }
        source_hide_detail_widgets();
        source_update_page_controls();
        source_diag_log("empty");
        ui_schedule_repaint(source_overlay);
        return;
    }

    if (source_empty_label != NULL) {
        lv_obj_add_flag(source_empty_label, LV_OBJ_FLAG_HIDDEN);
    }

    bool album_detail = (source_current_mode == SOURCE_ALBUM_TRACKS);
    bool folder_detail = (source_current_mode == SOURCE_FOLDER_CONTENT &&
                          source_artwork_track_id > 0);
    bool detail_layout = album_detail || folder_detail;

    if (source_detail_album_caption != NULL) {
        if (detail_layout) {
            lv_label_set_text(source_detail_album_caption, album_detail ? "ALBUM" : "FOLDER");
            lv_obj_clear_flag(source_detail_album_caption, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(source_detail_album_caption, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (source_detail_album_label != NULL) {
        if (detail_layout) {
            lv_label_set_text(source_detail_album_label,
                              album_detail ? source_current_title : source_folder_title);
            lv_obj_clear_flag(source_detail_album_label, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(source_detail_album_label, LV_OBJ_FLAG_HIDDEN);
        }
    }

    /* Keep the cover while paging through the same album; drop it otherwise. */
    if (!detail_layout || source_art_key_shown != source_art_key_wanted) {
        source_clear_art();
    }
    if (source_detail_track_count != NULL) {
        if (album_detail) {
            char count_text[32];
            snprintf(count_text, sizeof(count_text), "%d TRACKS", source_item_count);
            lv_label_set_text(source_detail_track_count, count_text);
            lv_obj_clear_flag(source_detail_track_count, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(source_detail_track_count, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (source_detail_fav_button != NULL) {
        if (album_detail && source_parent_id > 0) lv_obj_clear_flag(source_detail_fav_button, LV_OBJ_FLAG_HIDDEN);
        else                                      lv_obj_add_flag(source_detail_fav_button, LV_OBJ_FLAG_HIDDEN);
    }

    if (source_track_list != NULL) {
        if (detail_layout) {
            lv_obj_clear_flag(source_track_list, LV_OBJ_FLAG_HIDDEN);
            lv_obj_scroll_to(source_track_list, 0, 0, LV_ANIM_OFF);
        } else {
            lv_obj_add_flag(source_track_list, LV_OBJ_FLAG_HIDDEN);
        }
    }

    if (source_grid_mode()) {
        source_render_grid();
        if (source_nav_bar != NULL) lv_obj_move_foreground(source_nav_bar);
        source_update_page_controls();
        source_diag_log("grid");
        ui_schedule_repaint(source_overlay);
        return;
    }

    if (source_current_mode == SOURCE_PLAYLIST_TRACKS) {
        source_render_playlist();
        if (source_nav_bar != NULL) lv_obj_move_foreground(source_nav_bar);
        source_update_page_controls();
        source_diag_log("playlist");
        ui_schedule_repaint(source_overlay);
        return;
    }

    if (source_current_mode == SOURCE_QUEUE) {
        source_render_queue();
        if (source_nav_bar != NULL) lv_obj_move_foreground(source_nav_bar);
        source_update_page_controls();
        source_diag_log("queue");
        ui_schedule_repaint(source_overlay);
        return;
    }

    for (int i = 0; i < source_item_count; i++) {
        int col = i % 2;
        int row = i / 2;
        char display_text[SOURCE_TEXT_MAX + 40];
        char clean_title[SOURCE_TEXT_MAX];

        source_strip_audio_extension(source_items[i].title,
                                      clean_title, sizeof(clean_title));

        if (album_detail) {
            snprintf(display_text, sizeof(display_text), "%s",
                     clean_title);
        } else if (source_items[i].action == SOURCE_ALBUM_TRACKS) {
            /* Album names are intentionally shown cleanly, without a type prefix. */
            snprintf(display_text, sizeof(display_text), "%s",
                     clean_title);
        } else if (source_items[i].action == SOURCE_FOLDER_CONTENT) {
            /* Folder names are intentionally shown cleanly, without a type prefix. */
            snprintf(display_text, sizeof(display_text), "%s",
                     clean_title);
        } else if (source_items[i].action == SOURCE_LOAD_TRACK) {
            /* Songs are shown with a small music icon; keep the title clean. */
            snprintf(display_text, sizeof(display_text), "%s",
                     clean_title);
        } else if (source_items[i].action == SOURCE_LOAD_PLAYLIST) {
            /* Playlists show a music icon instead of a text prefix. */
            snprintf(display_text, sizeof(display_text), "%s",
                     clean_title);
        } else if (source_items[i].action == SOURCE_FAVORITE_CONTENT) {
            /* Favorite folders are shown simply by their names. */
            snprintf(display_text, sizeof(display_text), "%s",
                     clean_title);
        } else if (source_items[i].action == SOURCE_LOAD_FAVORITE ||
                   source_items[i].action == SOURCE_LOAD_FAVORITE_URL) {
            /* Favorite stations/items are shown simply by their names. */
            snprintf(display_text, sizeof(display_text), "%s",
                     clean_title);
        } else if (source_items[i].action == SOURCE_RADIO_MENU ||
                   source_items[i].action == SOURCE_RADIO_PLAY) {
            /* Radio menus show a folder icon, stations a music icon. */
            snprintf(display_text, sizeof(display_text), "%s",
                     clean_title);
        } else {
            snprintf(display_text, sizeof(display_text), "%s",
                     clean_title);
        }

        /* Folder and song indicators keep the names clean while making the
         * source browser immediately readable. */
        /* Show a folder icon for anything that opens another collection/view.
         * This keeps Artists, Genres, Albums, Music Folders, and Favorites
         * visually consistent instead of making folder icons appear only
         * inside the Music Folders browser.  Actual songs keep the music-note
         * icon. */
        bool show_folder_icon =
            source_items[i].action == SOURCE_ARTIST_ALBUMS ||
            source_items[i].action == SOURCE_GENRE_ALBUMS ||
            source_items[i].action == SOURCE_FOLDER_CONTENT ||
            source_items[i].action == SOURCE_ALBUM_TRACKS ||
            source_items[i].action == SOURCE_FAVORITE_CONTENT ||
            source_items[i].action == SOURCE_RADIO_MENU;
        bool show_song_icon =
            source_items[i].action == SOURCE_LOAD_TRACK ||
            source_items[i].action == SOURCE_LOAD_PLAYLIST ||
            source_items[i].action == SOURCE_LOAD_FAVORITE ||
            source_items[i].action == SOURCE_LOAD_FAVORITE_URL ||
            source_items[i].action == SOURCE_RADIO_PLAY ||
            source_items[i].action == SOURCE_QUEUE_JUMP;

        source_item_buttons[i] = create_source_item_button(
            detail_layout && source_track_list != NULL ? source_track_list : source_overlay,
            display_text,
            detail_layout ? 630 : SOURCE_BUTTON_W,
            detail_layout ? SOURCE_TRACK_BUTTON_H : SOURCE_MENU_BUTTON_H,
            show_folder_icon,
            show_song_icon
        );

        /* Playlists get a list icon, so they are not mistaken for songs. */
        if (source_items[i].action == SOURCE_LOAD_PLAYLIST) {
            lv_obj_t *icon = lv_obj_get_child(source_item_buttons[i], 1);
            if (icon != NULL && lv_obj_check_type(icon, &lv_label_class)) {
                lv_label_set_text(icon, LV_SYMBOL_LIST);
            }
        }

        if (detail_layout) {
            /* Album/folder detail is a single-column track list.  The old
             * code used row=i/2 but a fixed X position, which caused tracks
             * 6, 7, 8... to be drawn directly on top of tracks 1, 2, 3.... */
            lv_obj_set_pos(source_item_buttons[i], 0, i * SOURCE_TRACK_ROW_H);
        } else {
            lv_obj_set_pos(source_item_buttons[i],
                           SOURCE_GRID_X0 + col * SOURCE_GRID_DX,
                           100 + row * SOURCE_MENU_ROW_H);
        }
        lv_obj_add_event_cb(source_item_buttons[i],
                            source_item_event_cb,
                            LV_EVENT_CLICKED,
                            (void *)(intptr_t)i);

        const bool is_song = (source_items[i].action == SOURCE_LOAD_TRACK && source_items[i].id > 0);
        const bool is_fav_row = (source_current_mode == SOURCE_FAVORITES ||
                                 source_current_mode == SOURCE_FAVORITE_CONTENT) &&
                                (source_items[i].action == SOURCE_LOAD_FAVORITE ||
                                 source_items[i].action == SOURCE_LOAD_FAVORITE_URL);
        const bool deletable_fav = is_fav_row &&
            (source_items[i].fav_id[0] != '\0' || source_items[i].radio_play_json[0] != '\0');
        const bool is_station = source_favorite_is_stream(&source_items[i]);

        if (source_items[i].action == SOURCE_QUEUE_JUMP) {
            source_add_row_button(source_item_buttons[i], LV_SYMBOL_CLOSE,
                                  source_remove_event_cb, i, false, 0);
        } else if (source_select_mode && is_song) {
            /* SELECT: a tick circle instead of the buttons; the row toggles. */
            source_add_select_circle(source_item_buttons[i], source_sel_has(source_items[i].id));
            if (source_items[i].queued) source_row_show_queued(source_item_buttons[i], 1);
        } else if (is_station) {
            /* A stream never ends: play it or remove it, but don't queue it. */
            lv_obj_t *icon = lv_obj_get_child(source_item_buttons[i], 1);
            if (icon != NULL && lv_obj_check_type(icon, &lv_label_class)) {
                lv_label_set_text(icon, LV_SYMBOL_WIFI);
            }
            if (deletable_fav) {
                source_add_row_button(source_item_buttons[i], LV_SYMBOL_TRASH,
                                      source_playlist_trash_event_cb, i, false, 0);
            }
        } else if (source_queue_mode_for(source_items[i].action) != SOURCE_ROOT) {
            const bool queued = source_items[i].queued;
            lv_obj_t *plus = source_add_row_button(source_item_buttons[i],
                                                   queued ? LV_SYMBOL_OK : LV_SYMBOL_PLUS,
                                                   source_plus_event_cb, i, true, 0);
            /* Saved playlists and favorites can also be deleted (with a
             * confirming second tap). */
            if ((source_current_mode == SOURCE_PLAYLISTS &&
                 source_items[i].action == SOURCE_LOAD_PLAYLIST) || deletable_fav) {
                source_add_row_button(source_item_buttons[i], LV_SYMBOL_TRASH,
                                      source_playlist_trash_event_cb, i, false, 1);
            }
            /* (Songs go to Favorites through SELECT - no star per row.) */
            if (queued && is_song) {
                lv_obj_t *plus_label = lv_obj_get_child(plus, 0);
                if (plus_label != NULL) lv_obj_set_user_data(plus_label, (void *)1);
                source_style_queued_button(plus);
                source_row_show_queued(source_item_buttons[i], 1);
            }
        }
    }

    /* Item buttons are created after the nav bar, so explicitly restore the
     * nav bar to the foreground after every render.  This is especially
     * important for the fifth row of two-column folder/album views. */
    if (source_nav_bar != NULL) {
        lv_obj_move_foreground(source_nav_bar);
    }
    source_update_page_controls();
    source_diag_log("list");
    ui_schedule_repaint(source_overlay);
    /* Source-browser artwork disabled; normal album-art path handles it. */
}

static bool source_radio_action_to_request(const char *action_json,
                                           int player_index,
                                           char *out,
                                           size_t out_size)
{
    if (action_json == NULL || action_json[0] == '\0' || out == NULL) return false;

    cJSON *action = cJSON_Parse(action_json);
    if (action == NULL) return false;

    /* LMS SlimBrowse returns actions in two shapes:
     * 1. Top-level:  {"cmd":[...], "params":{...}}
     * 2. Nested:     {"do":{"cmd":[...], "params":{...}}}
     * Try the nested "do" form first, fall back to top-level. */
    cJSON *action_inner = cJSON_GetObjectItem(action, "do");
    if (!cJSON_IsObject(action_inner)) {
        action_inner = action;
    }

    cJSON *cmds = cJSON_GetObjectItem(action_inner, "cmd");
    if (!cJSON_IsArray(cmds) || cJSON_GetArraySize(cmds) == 0) {
        cJSON_Delete(action);
        return false;
    }

    cJSON *params = cJSON_GetObjectItem(action_inner, "params");
    cJSON *rpc_params = cJSON_CreateArray();
    cJSON *player = cJSON_CreateString(lyrion_players[player_index].id);
    cJSON *cmd_array = cJSON_CreateArray();
    cJSON_AddItemToArray(rpc_params, player);
    cJSON_AddItemToArray(rpc_params, cmd_array);

    cJSON *cmd = NULL;
    const char *last_cmd = NULL;
    cJSON_ArrayForEach(cmd, cmds) {
        if (!cJSON_IsString(cmd) || cmd->valuestring == NULL) continue;
        cJSON_AddItemToArray(cmd_array, cJSON_CreateString(cmd->valuestring));
        last_cmd = cmd->valuestring;
    }
    /* Browse commands ("tunein items", "radios", ...) are queries that need
     * <start> <count> right after the command words - the app sending them is
     * expected to add those.  Without them Lyrion answers with no items,
     * which is why radio folders used to open empty. */
    if (last_cmd != NULL && (strcmp(last_cmd, "items") == 0 || strcmp(last_cmd, "radios") == 0 ||
                             strcmp(last_cmd, "apps") == 0)) {
        cJSON_AddItemToArray(cmd_array, cJSON_CreateString("0"));
        cJSON_AddItemToArray(cmd_array, cJSON_CreateString("200"));
    }

    if (cJSON_IsObject(params)) {
        cJSON *param = NULL;
        cJSON_ArrayForEach(param, params) {
            if (param->string == NULL) continue;
            char value[256];
            value[0] = '\0';
            if (cJSON_IsString(param) && param->valuestring) {
                snprintf(value, sizeof(value), "%s", param->valuestring);
            } else if (cJSON_IsNumber(param)) {
                snprintf(value, sizeof(value), "%g", param->valuedouble);
            } else if (cJSON_IsBool(param)) {
                snprintf(value, sizeof(value), "%s", cJSON_IsTrue(param) ? "1" : "0");
            } else {
                continue;
            }
            char tagged[320];
            snprintf(tagged, sizeof(tagged), "%s:%s", param->string, value);
            cJSON_AddItemToArray(cmd_array, cJSON_CreateString(tagged));
        }
    }

    cJSON *rpc = cJSON_CreateObject();
    cJSON_AddNumberToObject(rpc, "id", 1);
    cJSON_AddStringToObject(rpc, "method", "slim.request");
    cJSON_AddItemToObject(rpc, "params", rpc_params);

    char *printed = cJSON_PrintUnformatted(rpc);
    bool ok = false;
    if (printed != NULL) {
        if (strlen(printed) < out_size) {
            strcpy(out, printed);
            ok = true;
        }
        free(printed);
    }
    cJSON_Delete(rpc);
    cJSON_Delete(action);
    return ok;
}

/* A play action turned into an add / insert one: the last command word, or
 * a "cmd" parameter, that says "play" becomes verb.  False if neither does. */
static bool source_radio_action_as(const char *action_json, const char *verb, char *out, size_t out_size)
{
    cJSON *action = cJSON_Parse(action_json);
    if (action == NULL) return false;
    cJSON *inner = cJSON_GetObjectItem(action, "do");
    if (!cJSON_IsObject(inner)) inner = action;
    bool changed = false;
    cJSON *cmds = cJSON_GetObjectItem(inner, "cmd");
    const int n = cJSON_IsArray(cmds) ? cJSON_GetArraySize(cmds) : 0;
    cJSON *last = n > 0 ? cJSON_GetArrayItem(cmds, n - 1) : NULL;
    if (cJSON_IsString(last) && last->valuestring && strcmp(last->valuestring, "play") == 0) {
        cJSON_ReplaceItemInArray(cmds, n - 1, cJSON_CreateString(verb));
        changed = true;
    }
    cJSON *params = cJSON_GetObjectItem(inner, "params");
    cJSON *pc = cJSON_IsObject(params) ? cJSON_GetObjectItem(params, "cmd") : NULL;
    if (cJSON_IsString(pc) && pc->valuestring && strcmp(pc->valuestring, "play") == 0) {
        cJSON_ReplaceItemInObject(params, "cmd", cJSON_CreateString(verb));
        changed = true;
    }
    bool ok = false;
    if (changed) {
        char *printed = cJSON_PrintUnformatted(action);
        if (printed != NULL) {
            if (strlen(printed) < out_size) { snprintf(out, out_size, "%s", printed); ok = true; }
            free(printed);
        }
    }
    cJSON_Delete(action);
    return ok;
}

/* Build an action JSON string from a template (e.g. the response-wide "base"
 * actions) with this item's own params merged in - LMS sends menus that way,
 * with the item-specific id in the item and the command in "base". */
static bool source_radio_action_from_template(const cJSON *template_action,
                                              const cJSON *item_params,
                                              char *out, size_t out_size)
{
    if (!cJSON_IsObject(template_action) || out == NULL || out_size == 0) return false;

    cJSON *copy = cJSON_Duplicate(template_action, true);
    if (copy == NULL) return false;

    if (cJSON_IsObject(item_params)) {
        cJSON *inner = cJSON_GetObjectItem(copy, "do");
        cJSON *target = cJSON_IsObject(inner) ? inner : copy;
        cJSON *params = cJSON_GetObjectItem(target, "params");
        if (!cJSON_IsObject(params)) {
            cJSON_DeleteItemFromObject(target, "params");
            params = cJSON_CreateObject();
            cJSON_AddItemToObject(target, "params", params);
        }
        cJSON *p = NULL;
        cJSON_ArrayForEach(p, item_params) {
            if (p->string == NULL) continue;
            cJSON_DeleteItemFromObject(params, p->string);
            cJSON_AddItemToObject(params, p->string, cJSON_Duplicate(p, true));
        }
    }

    char *printed = cJSON_PrintUnformatted(copy);
    bool ok = false;
    if (printed != NULL) {
        if (strlen(printed) < out_size) {
            snprintf(out, out_size, "%s", printed);
            ok = true;
        }
        free(printed);
    }
    cJSON_Delete(copy);
    return ok;
}

static bool source_radio_action_to_string(const cJSON *action, char *out, size_t out_size)
{
    if (action == NULL) return false;
    char *printed = cJSON_PrintUnformatted(action);
    bool ok = false;
    if (printed != NULL) {
        if (strlen(printed) < out_size) {
            snprintf(out, out_size, "%s", printed);
            ok = true;
        }
        free(printed);
    }
    return ok;
}

/* The item's own detail object for an action, or NULL. */
static cJSON *source_radio_item_params(cJSON *entry, const cJSON *action, bool from_base)
{
    const cJSON *inner = cJSON_GetObjectItem(action, "do");
    if (!cJSON_IsObject(inner)) inner = action;
    const cJSON *name = cJSON_GetObjectItem(inner, "itemsParams");
    const char *key = (cJSON_IsString(name) && name->valuestring) ? name->valuestring
                    : (from_base ? "params" : NULL);
    if (key == NULL) return NULL;
    cJSON *obj = cJSON_GetObjectItem(entry, key);
    return cJSON_IsObject(obj) ? obj : NULL;
}

static void source_radio_add_item(cJSON *entry, const cJSON *base_actions)
{
    if (entry == NULL || source_item_count >= SOURCE_MAX_ITEMS) return;

    cJSON *text = cJSON_GetObjectItem(entry, "text");
    if (!cJSON_IsString(text)) text = cJSON_GetObjectItem(entry, "name");
    if (!cJSON_IsString(text)) text = cJSON_GetObjectItem(entry, "title");

    source_item_t *item = &source_items[source_item_count++];
    memset(item, 0, sizeof(*item));
    item->id = 0;
    item->action = SOURCE_RADIO_MENU;
    snprintf(item->title, sizeof(item->title), "%s",
             cJSON_IsString(text) && text->valuestring ? text->valuestring : "Radio");

    cJSON *actions = cJSON_GetObjectItem(entry, "actions");
    cJSON *go = cJSON_IsObject(actions) ? cJSON_GetObjectItem(actions, "go") : NULL;
    cJSON *play = cJSON_IsObject(actions) ? cJSON_GetObjectItem(actions, "play") : NULL;

    /* Search entries ("input" field) need words typed before they can open. */
    item->needs_input = cJSON_IsObject(cJSON_GetObjectItem(entry, "input"));

    /* Items that carry no actions of their own inherit the response-wide
     * "base" actions, with the item's "params" merged in. */
    bool from_base = false;
    if (go == NULL && play == NULL && cJSON_IsObject(base_actions)) {
        go = cJSON_GetObjectItem(base_actions, "go");
        play = cJSON_GetObjectItem(base_actions, "play");
        from_base = (go != NULL || play != NULL);
    }

    if (go == NULL && play == NULL) {
        /* Some radio/TuneIn responses expose a station URL directly rather
         * than an actions.play object. Treat that as a playable station. */
        cJSON *url = cJSON_GetObjectItem(entry, "url");
        if (!cJSON_IsString(url)) url = cJSON_GetObjectItem(entry, "value");
        if (cJSON_IsString(url) && url->valuestring && url->valuestring[0]) {
            item->action = SOURCE_LOAD_FAVORITE_URL;
            snprintf(item->item_id, sizeof(item->item_id), "%s", url->valuestring);
        }
        return;
    }

    /* An action names which of the item's fields holds its details (usually
     * "params", holding item_id) in "itemsParams"; base actions always use
     * "params".  Those details are merged into the action's own params. */
    if (go != NULL) {
        cJSON *gp = source_radio_item_params(entry, go, from_base);
        if (gp != NULL) {
            source_radio_action_from_template(go, gp, item->radio_go_json, sizeof(item->radio_go_json));
        } else {
            source_radio_action_to_string(go, item->radio_go_json, sizeof(item->radio_go_json));
        }
    }
    if (play != NULL) {
        cJSON *pp = source_radio_item_params(entry, play, from_base);
        if (pp != NULL) {
            source_radio_action_from_template(play, pp, item->radio_play_json, sizeof(item->radio_play_json));
        } else {
            source_radio_action_to_string(play, item->radio_play_json, sizeof(item->radio_play_json));
        }
    }

    /* A station is not a folder.  LMS marks playable entries as type "audio"
     * (or style "itemplay" / nextWindow "nowPlaying") and often gives them a
     * "go" action too, which used to make them look like empty sub-menus. */
    cJSON *type = cJSON_GetObjectItem(entry, "type");
    cJSON *style = cJSON_GetObjectItem(entry, "style");
    cJSON *next_window = cJSON_GetObjectItem(entry, "nextWindow");
    cJSON *is_audio_item = cJSON_GetObjectItem(entry, "isaudio");
    bool is_audio =
        (cJSON_IsString(type) && type->valuestring && strcmp(type->valuestring, "audio") == 0) ||
        (cJSON_IsString(style) && style->valuestring && strcmp(style->valuestring, "itemplay") == 0) ||
        (cJSON_IsString(next_window) && next_window->valuestring &&
         strcmp(next_window->valuestring, "nowPlaying") == 0) ||
        (cJSON_IsNumber(is_audio_item) && is_audio_item->valueint == 1) ||
        cJSON_IsTrue(is_audio_item);

    if (is_audio) {
        if (item->radio_play_json[0] == '\0') {
            /* No explicit play action: the "go" action is what plays it. */
            snprintf(item->radio_play_json, sizeof(item->radio_play_json), "%s",
                     item->radio_go_json);
        }
        item->radio_go_json[0] = '\0';
    }

    if (item->radio_go_json[0] == '\0' && item->radio_play_json[0] != '\0') {
        item->action = SOURCE_RADIO_PLAY;
        /* Do not copy the action into item_id: radio_play_json is intentionally
         * larger than the normal 96-byte Favorites ID field. */
    }

    ESP_LOGI(TAG, "Radio item '%s': %s%s", item->title,
             item->action == SOURCE_RADIO_PLAY ? "station" : "menu",
             from_base ? " (base actions)" : "");
}

/* Record the view being left (when the request asked for history) and make
 * the requested view the current one.  Called for successful loads and for
 * failed ones alike, so BACK always stays consistent with what is on screen. */
static void source_commit_view(const source_request_t *request, source_mode_t display_mode)
{
    if (request->push_history) {
        bool id_based = (source_current_mode == SOURCE_ARTIST_ALBUMS ||
                         source_current_mode == SOURCE_GENRE_ALBUMS ||
                         source_current_mode == SOURCE_FOLDER_CONTENT ||
                         source_current_mode == SOURCE_ALBUM_TRACKS ||
                         source_current_mode == SOURCE_PLAYLIST_TRACKS);
        source_nav_push(source_current_mode,
                        id_based ? source_parent_id : 0,
                        source_page_offset,
                        source_current_mode == SOURCE_FAVORITE_CONTENT
                            ? source_parent_item_id : "",
                        (source_current_mode == SOURCE_FOLDER_CONTENT ||
                         source_current_mode == SOURCE_PLAYLIST_TRACKS)
                            ? source_folder_title : "");
    }

    source_current_mode = display_mode;
    source_page_offset = request->offset;

    if (request->mode == SOURCE_ARTIST_ALBUMS ||
        request->mode == SOURCE_GENRE_ALBUMS ||
        request->mode == SOURCE_FOLDER_CONTENT ||
        request->mode == SOURCE_ALBUM_TRACKS ||
        request->mode == SOURCE_PLAYLIST_TRACKS) {
        source_parent_id = request->id;
    } else if (request->mode == SOURCE_FAVORITE_CONTENT) {
        snprintf(source_parent_item_id, sizeof(source_parent_item_id), "%s", request->item_id);
    }

    if (request->mode == SOURCE_FOLDER_CONTENT || request->mode == SOURCE_PLAYLIST_TRACKS) {
        snprintf(source_folder_title, sizeof(source_folder_title), "%s",
                 request->title[0] ? request->title
                 : (request->mode == SOURCE_PLAYLIST_TRACKS ? "Playlist" : "FOLDER"));
    }
}

/* ---- UI jobs for the browser task (run on the LVGL task via ui_call) ---- */
static void source_show_root_job(void *arg) { (void)arg; source_show_root(); }
static void source_hide_job(void *arg)      { (void)arg; source_hide(); }

typedef struct { source_mode_t mode; const char *message; } source_error_ctx_t;
static void source_show_error_job(void *arg)
{
    const source_error_ctx_t *c = (const source_error_ctx_t *)arg;
    source_item_count = 0;
    source_has_next = false;
    source_clear_items();
    source_set_title(source_mode_title(c->mode));
    source_hide_detail_widgets();
    source_update_page_controls();
    if (source_empty_label != NULL) {
        lv_label_set_text(source_empty_label, c->message);
        lv_obj_clear_flag(source_empty_label, LV_OBJ_FLAG_HIDDEN);
    }
    source_show();
}

typedef struct { const char *title; int art_key; bool set_art_key; } source_render_ctx_t;
static void source_render_job(void *arg)
{
    const source_render_ctx_t *c = (const source_render_ctx_t *)arg;
    if (c->set_art_key) source_art_key_wanted = c->art_key;
    source_set_title(c->title);
    source_render_items();
    source_show();
}

/* Status line of the save-queue panel. */
typedef struct {
    const char *text;
    uint32_t color;
    bool close_soon;
    const char *replace_name;    /* non-NULL: remember this name for "replace?" */
    bool clear_replace;
} qs_status_ctx_t;
static void qs_status_job(void *arg)
{
    const qs_status_ctx_t *c = (const qs_status_ctx_t *)arg;
    if (c->replace_name != NULL) {
        snprintf(qs_replace_name, sizeof(qs_replace_name), "%s", c->replace_name);
    }
    if (c->clear_replace) qs_replace_name[0] = '\0';
    qs_set_status_locked(c->text, c->color, c->close_soon);
}
static void qs_status_from_task(const char *text, uint32_t color, bool close_soon)
{
    qs_status_ctx_t c = { .text = text, .color = color, .close_soon = close_soon };
    ui_call(qs_status_job, &c);
}

/* Show a message in place of the list (used when the server cannot be
 * reached or answers with something unusable). */
static void source_show_error(const source_request_t *request,
                              source_mode_t display_mode,
                              const char *message)
{
    source_commit_view(request, display_mode);

    source_error_ctx_t ctx = { .mode = display_mode, .message = message };
    ui_call(source_show_error_job, &ctx);
}

/* Ask Lyrion which songs are in this player's queue and flag the matching
 * song rows, so they show a lasting tick instead of "+".  Asked every time a
 * song list is drawn, so it stays right even if the queue was changed from
 * Lyrion's web page or phone app. */
#define QUEUE_LOOKUP_MAX 400   /* keeps the reply well inside the 64 KB buffer */

static void source_mark_queued_items(int player_index)
{
    bool any_songs = false;
    for (int i = 0; i < source_item_count; i++) {
        source_items[i].queued = false;
        if (source_items[i].action == SOURCE_LOAD_TRACK) any_songs = true;
    }
    if (!any_songs) return;

    char *buf = (char *)heap_caps_malloc(SOURCE_BROWSE_BUF, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (buf == NULL) return;

    char json[192];
    snprintf(json, sizeof(json),
             "{\"id\":1,\"method\":\"slim.request\",\"params\":[\"%s\",[\"status\",0,%d,\"tags:d\"]]}",
             lyrion_players[player_index].id, QUEUE_LOOKUP_MAX);
    if (!source_http_query(player_index, json, buf, SOURCE_BROWSE_BUF)) {
        heap_caps_free(buf);
        return;                      /* no ticks this time; browsing still works */
    }

    cJSON *root = cJSON_Parse(buf);
    heap_caps_free(buf);
    cJSON *res = root ? cJSON_GetObjectItem(root, "result") : NULL;
    cJSON *loop = cJSON_IsObject(res) ? cJSON_GetObjectItem(res, "playlist_loop") : NULL;
    int marked = 0;
    cJSON *entry = NULL;
    cJSON_ArrayForEach(entry, loop) {
        cJSON *idj = cJSON_GetObjectItem(entry, "id");
        const int id = cJSON_IsNumber(idj) ? idj->valueint
                     : (cJSON_IsString(idj) && idj->valuestring ? atoi(idj->valuestring) : 0);
        if (id <= 0) continue;
        for (int i = 0; i < source_item_count; i++) {
            if (source_items[i].action == SOURCE_LOAD_TRACK && source_items[i].id == id &&
                !source_items[i].queued) {
                source_items[i].queued = true;
                marked++;
            }
        }
    }
    if (root) cJSON_Delete(root);
    if (marked > 0) {
        ESP_LOGI(TAG, "Queue ticks: %d song(s) on this page already queued", marked);
    }
}

/* Search the library for request->title: up to 3 artists, up to 3 albums,
 * then songs, as one list (tap to open / play, + to queue). */
static void source_query_search(source_request_t *request, int player_index, char *response)
{
    char term[64];
    size_t n = 0;
    for (const char *p = request->title; *p && n + 1 < sizeof(term); p++) {
        if (*p != '"' && *p != '\\' && (unsigned char)*p >= 0x20) term[n++] = *p;   /* JSON-safe */
    }
    term[n] = '\0';

    char json[256];
    snprintf(json, sizeof(json),
             "{\"id\":1,\"method\":\"slim.request\",\"params\":[\"%s\",[\"search\",0,10,\"term:%s\"]]}",
             lyrion_players[player_index].id, term);
    if (!source_http_query(player_index, json, response, SOURCE_BROWSE_BUF)) {
        source_show_error(request, SOURCE_SEARCH, "Could not reach the Lyrion server");
        return;
    }
    cJSON *root = cJSON_Parse(response);
    cJSON *res = root ? cJSON_GetObjectItem(root, "result") : NULL;

    source_item_count = 0;
    struct { const char *loop, *name_key, *id_key; source_mode_t action; int max; } kinds[3] = {
        { "contributors_loop", "contributor", "contributor_id", SOURCE_ARTIST_ALBUMS, 3 },
        { "albums_loop",       "album",       "album_id",       SOURCE_ALBUM_TRACKS,  3 },
        { "tracks_loop",       "track",       "track_id",       SOURCE_LOAD_TRACK,    SOURCE_MAX_ITEMS },
    };
    for (int k = 0; k < 3 && cJSON_IsObject(res); k++) {
        cJSON *loop = cJSON_GetObjectItem(res, kinds[k].loop);
        int added = 0;
        cJSON *e = NULL;
        cJSON_ArrayForEach(e, loop) {
            if (added >= kinds[k].max || source_item_count >= SOURCE_MAX_ITEMS) break;
            cJSON *nm = cJSON_GetObjectItem(e, kinds[k].name_key);
            if (!cJSON_IsString(nm)) nm = cJSON_GetObjectItem(e, "name");
            cJSON *id = cJSON_GetObjectItem(e, kinds[k].id_key);
            if (!cJSON_IsNumber(id) && !cJSON_IsString(id)) id = cJSON_GetObjectItem(e, "id");
            const int idv = cJSON_IsNumber(id) ? id->valueint
                          : (cJSON_IsString(id) && id->valuestring ? atoi(id->valuestring) : 0);
            if (!cJSON_IsString(nm) || !nm->valuestring || idv <= 0) continue;
            char text[SOURCE_TEXT_MAX];
            snprintf(text, sizeof(text), "%s%.80s",
                     k == 0 ? "Artist:  " : k == 1 ? "Album:  " : "", nm->valuestring);
            source_add_item(idv, kinds[k].action, text);
            added++;
        }
    }
    if (root) cJSON_Delete(root);
    source_has_next = false;
    source_mark_queued_items(player_index);

    source_commit_view(request, SOURCE_SEARCH);
    char title[96];
    snprintf(title, sizeof(title), "SEARCH:  %.60s", term);
    source_render_ctx_t ctx = { .title = title };
    ui_call(source_render_job, &ctx);
    ESP_LOGI(TAG, "Search \"%s\": %d result(s)", term, source_item_count);
}

/* ---- Favorites: add / remove ------------------------------------------ */

/* Is `url` a favorite?  Sets *index to its favorites id.  -1 = couldn't tell. */
static int favorites_exists(int player_index, const char *url, char *index, size_t index_size)
{
    cJSON *rpc = cJSON_CreateObject();
    cJSON *params = cJSON_CreateArray();
    cJSON *cmd = cJSON_CreateArray();
    cJSON_AddNumberToObject(rpc, "id", 1);
    cJSON_AddStringToObject(rpc, "method", "slim.request");
    cJSON_AddItemToArray(params, cJSON_CreateString(lyrion_players[player_index].id));
    cJSON_AddItemToArray(cmd, cJSON_CreateString("favorites"));
    cJSON_AddItemToArray(cmd, cJSON_CreateString("exists"));
    cJSON_AddItemToArray(cmd, cJSON_CreateString(url));
    cJSON_AddItemToArray(params, cmd);
    cJSON_AddItemToObject(rpc, "params", params);
    char *json = cJSON_PrintUnformatted(rpc);
    cJSON_Delete(rpc);
    if (json == NULL) return -1;

    char reply[LYRION_RESPONSE_MAX];
    int result = -1;
    index[0] = '\0';
    if (source_http_query(player_index, json, reply, sizeof(reply))) {
        cJSON *root = cJSON_Parse(reply);
        cJSON *res = root ? cJSON_GetObjectItem(root, "result") : NULL;
        if (cJSON_IsObject(res)) {
            cJSON *ex = cJSON_GetObjectItem(res, "exists");
            cJSON *ix = cJSON_GetObjectItem(res, "index");
            result = (cJSON_IsNumber(ex) ? ex->valueint
                      : (cJSON_IsString(ex) && ex->valuestring ? atoi(ex->valuestring) : 0)) ? 1 : 0;
            if (cJSON_IsString(ix) && ix->valuestring) snprintf(index, index_size, "%s", ix->valuestring);
            else if (cJSON_IsNumber(ix)) snprintf(index, index_size, "%d", ix->valueint);
        }
        if (root) cJSON_Delete(root);
    }
    free(json);
    return result;
}

/* Send ["favorites", verb, "key:value", ...] (values escaped by cJSON). */
static bool favorites_command(int player_index, const char *verb, const char *k1, const char *v1,
                              const char *k2, const char *v2)
{
    cJSON *rpc = cJSON_CreateObject();
    cJSON *params = cJSON_CreateArray();
    cJSON *cmd = cJSON_CreateArray();
    cJSON_AddNumberToObject(rpc, "id", 1);
    cJSON_AddStringToObject(rpc, "method", "slim.request");
    cJSON_AddItemToArray(params, cJSON_CreateString(lyrion_players[player_index].id));
    cJSON_AddItemToArray(cmd, cJSON_CreateString("favorites"));
    cJSON_AddItemToArray(cmd, cJSON_CreateString(verb));
    char tagged[640];
    if (k1 != NULL) {
        snprintf(tagged, sizeof(tagged), "%s:%s", k1, v1);
        cJSON_AddItemToArray(cmd, cJSON_CreateString(tagged));
    }
    if (k2 != NULL) {
        snprintf(tagged, sizeof(tagged), "%s:%s", k2, v2);
        cJSON_AddItemToArray(cmd, cJSON_CreateString(tagged));
    }
    if (strcmp(verb, "add") == 0) {
        /* New favorites go to the top of the list, where they're easy to find
         * (without this they land at the very end, often on the last page). */
        cJSON_AddItemToArray(cmd, cJSON_CreateString("index:0"));
    }
    cJSON_AddItemToArray(params, cmd);
    cJSON_AddItemToObject(rpc, "params", params);
    char *json = cJSON_PrintUnformatted(rpc);
    cJSON_Delete(rpc);
    if (json == NULL) return false;
    char reply[LYRION_RESPONSE_MAX];
    const bool ok = source_http_query(player_index, json, reply, sizeof(reply));
    ESP_LOGI(TAG, "Favorites %s: %s  sent %.200s  reply %.160s", verb, ok ? "ok" : "failed",
             json, ok ? reply : "");
    free(json);
    return ok;
}

/* Called by the status task when the playing URL changes. */
static void favorites_check_now_playing(void)
{
    const int p = current_player_index;
    if (p < 0 || p >= lyrion_player_count || np_fav_url[0] == '\0') {
        np_fav = -1;
        return;
    }
    char index[24];
    const int ex = favorites_exists(p, np_fav_url, index, sizeof(index));
    snprintf(np_fav_index, sizeof(np_fav_index), "%s", index);
    np_fav = ex;
}

/* From the status task: queue a check on the browser task. */
static void favorites_request_check(void)
{
    source_request_t chk = { .mode = SOURCE_FAV_CHECK };
    xQueueSend(source_queue, &chk, 0);
}

/* One Lyrion query; returns the parsed reply's "result" loop's first entry
 * (caller frees *root). */
static cJSON *favorites_query_first(int p, const char *cmd_json, const char *loop_name, cJSON **root)
{
    *root = NULL;
    char json[512];
    snprintf(json, sizeof(json),
             "{\"id\":1,\"method\":\"slim.request\",\"params\":[\"%s\",%s]}",
             lyrion_players[p].id, cmd_json);
    char *reply = heap_caps_malloc(LYRION_RESPONSE_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (reply == NULL) return NULL;
    cJSON *first = NULL;
    if (source_http_query(p, json, reply, LYRION_RESPONSE_MAX)) {
        *root = cJSON_Parse(reply);
        cJSON *res = *root ? cJSON_GetObjectItem(*root, "result") : NULL;
        cJSON *loop = res ? cJSON_GetObjectItem(res, loop_name) : NULL;
        if (cJSON_IsArray(loop)) first = cJSON_GetArrayItem(loop, 0);
    }
    heap_caps_free(reply);
    return first;
}

static const char *favorites_str(cJSON *o, const char *key)
{
    cJSON *v = o ? cJSON_GetObjectItem(o, key) : NULL;
    return (cJSON_IsString(v) && v->valuestring) ? v->valuestring : "";
}

/* Percent-encode for a db: URL (letters, digits and -_.~ stay). */
static void favorites_url_escape(const char *in, char *out, size_t out_size)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t o = 0;
    for (const unsigned char *p = (const unsigned char *)in; *p && o + 4 < out_size; p++) {
        if (isalnum(*p) || *p == '-' || *p == '_' || *p == '.' || *p == '~') {
            out[o++] = (char)*p;
        } else {
            out[o++] = '%';
            out[o++] = hex[*p >> 4];
            out[o++] = hex[*p & 15];
        }
    }
    out[o] = '\0';
}

/* Library star buttons: add a song or an album to Favorites. */
/* Returns 1 added, 0 already there, -1 failed.  quiet: no messages. */
static int favorites_add_from_library(int p, source_request_t request, bool quiet)
{
    char url[600] = "", name[160] = "";
    cJSON *root = NULL;
    char cmd[128];
    if (request.mode == SOURCE_FAV_ADD_TRACK) {
        snprintf(cmd, sizeof(cmd), "[\"songinfo\",0,100,\"track_id:%d\",\"tags:uat\"]", request.id);
        /* songinfo returns a list of one-key objects: collect url/title/artist. */
        cJSON *first = favorites_query_first(p, cmd, "songinfo_loop", &root);
        (void)first;
        cJSON *res = root ? cJSON_GetObjectItem(root, "result") : NULL;
        cJSON *loop = res ? cJSON_GetObjectItem(res, "songinfo_loop") : NULL;
        const char *title = "", *artist = "";
        cJSON *e = NULL;
        if (cJSON_IsArray(loop)) {
            cJSON_ArrayForEach(e, loop) {
                if (cJSON_GetObjectItem(e, "url"))    snprintf(url, sizeof(url), "%s", favorites_str(e, "url"));
                if (cJSON_GetObjectItem(e, "title"))  title = favorites_str(e, "title");
                if (cJSON_GetObjectItem(e, "artist")) artist = favorites_str(e, "artist");
            }
        }
        if (title[0] && artist[0]) snprintf(name, sizeof(name), "%.90s - %.60s", title, artist);
        else snprintf(name, sizeof(name), "%.150s", title[0] ? title : request.title);
    } else {
        snprintf(cmd, sizeof(cmd), "[\"albums\",0,1,\"album_id:%d\",\"tags:la\"]", request.id);
        cJSON *a = favorites_query_first(p, cmd, "albums_loop", &root);
        const char *album = favorites_str(a, "album");
        const char *artist = favorites_str(a, "artist");
        if (album[0]) {
            char ea[256], er[200];
            favorites_url_escape(album, ea, sizeof(ea));
            favorites_url_escape(artist, er, sizeof(er));
            if (artist[0]) snprintf(url, sizeof(url), "db:album.title=%s&contributor.name=%s", ea, er);
            else           snprintf(url, sizeof(url), "db:album.title=%s", ea);
            if (artist[0]) snprintf(name, sizeof(name), "%.90s - %.60s", album, artist);
            else           snprintf(name, sizeof(name), "%.150s", album);
        }
    }
    if (root) cJSON_Delete(root);

    if (!url[0]) {
        ESP_LOGW(TAG, "Favorite add: no URL for id %d", request.id);
        if (!quiet) ui_toast_post("Could not find that on the Lyrion server");
        return -1;
    }
    char index[24];
    if (favorites_exists(p, url, index, sizeof(index)) == 1) {
        if (!quiet) ui_toast_post("Already in Favorites");
        return 0;
    }
    const bool ok = favorites_command(p, "add", "url", url, "title", name);
    ESP_LOGI(TAG, "Favorite add %s [%s]", ok ? "ok" : "failed", url);
    if (!quiet) {
        char msg[96];
        snprintf(msg, sizeof(msg), ok ? "Added to Favorites:  %.66s" : "Could not reach the Lyrion server", name);
        ui_toast_post(msg);
        favorites_check_now_playing();
    }
    return ok ? 1 : -1;
}

static void favorites_worker(source_request_t request)
{
    const int p = current_player_index;
    if (request.mode == SOURCE_FAV_CHECK) {
        favorites_check_now_playing();
        return;
    }
    if (p < 0 || p >= lyrion_player_count) return;

    ESP_LOGI(TAG, "Favorites request %d (id %d)", (int)request.mode, request.id);
    if (request.mode == SOURCE_FAV_ADD_TRACK || request.mode == SOURCE_FAV_ADD_ALBUM) {
        (void)favorites_add_from_library(p, request, false);
        return;
    }
    if (request.mode == SOURCE_SELECT_FAV) {            /* several songs at once */
        int added = 0, had = 0, failed = 0;
        char *save = NULL;
        for (char *tok = strtok_r(request.radio_action, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
            source_request_t one = { .mode = SOURCE_FAV_ADD_TRACK, .id = atoi(tok) };
            const int r = favorites_add_from_library(p, one, true);
            if (r > 0) added++; else if (r == 0) had++; else failed++;
        }
        char msg[96];
        if (failed && !added) snprintf(msg, sizeof(msg), "Could not add those to Favorites");
        else if (had) snprintf(msg, sizeof(msg), "Added %d to Favorites (%d already there)", added, had);
        else snprintf(msg, sizeof(msg), "Added %d song%s to Favorites", added, added == 1 ? "" : "s");
        ui_toast_post(msg);
        favorites_check_now_playing();
        return;
    }

    if (request.mode == SOURCE_FAV_TOGGLE) {
        if (np_fav_url[0] == '\0') return;
        bool ok;
        if (request.id) {                                     /* remove */
            char index[24];
            if (favorites_exists(p, np_fav_url, index, sizeof(index)) == 1 && index[0]) {
                ok = favorites_command(p, "delete", "item_id", index, NULL, NULL);
            } else {
                ok = true;                                    /* already gone */
            }
            ui_toast_post(ok ? "Removed from Favorites" : "Could not reach the Lyrion server");
        } else {                                              /* add */
            ok = favorites_command(p, "add", "url", np_fav_url, "title",
                                   np_fav_title[0] ? np_fav_title : "Favorite");
            char msg[96];
            snprintf(msg, sizeof(msg), ok ? "Added to Favorites:  %.60s" : "Could not reach the Lyrion server",
                     np_fav_title);
            ui_toast_post(msg);
        }
        favorites_check_now_playing();                        /* the real state */
        return;
    }

    /* SOURCE_FAV_DELETE from the Favorites list */
    char index[96] = "";
    if (request.item_id[0]) {
        snprintf(index, sizeof(index), "%s", request.item_id);
    } else if (request.radio_action[0]) {
        /* A row from the top-level list: find it by its URL. */
        if (favorites_exists(p, request.radio_action, index, sizeof(index)) != 1) index[0] = '\0';
        ESP_LOGI(TAG, "Favorite by URL -> index [%s]", index);
    }
    const bool ok = index[0] && favorites_command(p, "delete", "item_id", index, NULL, NULL);
    ui_toast_post(ok ? "Removed from Favorites" : "Could not delete that favorite");
    favorites_check_now_playing();
    source_request_t view = { .mode = (source_mode_t)request.queue_op, .offset = request.offset,
                              .push_history = false };
    snprintf(view.item_id, sizeof(view.item_id), "%s", request.title);
    xQueueSend(source_queue, &view, 0);
}

static void source_query_and_render(source_request_t request)
{
    char json[1024];
    int player_index = current_player_index;

    /* Allocate response buffer from PSRAM — keeps it off the task stack
     * and handles large radio/library responses without overflow. */
    /* App and radio menus (a YouTube search, say) can return a couple of
     * hundred entries with long picture links: give them a bigger buffer. */
    const size_t resp_size = (request.mode == SOURCE_RADIO_MENU) ? SOURCE_APP_BUF : SOURCE_BROWSE_BUF;
    char *response = (char *)heap_caps_malloc(resp_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (response == NULL) {
        response = (char *)heap_caps_malloc(SOURCE_BROWSE_BUF, MALLOC_CAP_8BIT);
    }
    if (response == NULL) {
        ESP_LOGE(TAG, "source_query_and_render: no memory for response buffer");
        return;
    }

    if (request.mode == SOURCE_ROOT) {
        ui_call(source_show_root_job, NULL);
        heap_caps_free(response);
        return;
    }

    if (request.mode == SOURCE_SEARCH) {
        source_query_search(&request, player_index, response);
        heap_caps_free(response);
        return;
    }

    if (request.mode == SOURCE_QUEUE) {
        /* The queue is the player's current playlist, read a page at a time
         * from "status" with the artist tag. */
        snprintf(json, sizeof(json),
                 "{\"id\":1,\"method\":\"slim.request\",\"params\":[\"%s\",[\"status\",%d,%d,\"tags:audN\"]]}",
                 lyrion_players[player_index].id, request.offset, SOURCE_QUEUE_PAGE);
        if (!source_http_query(player_index, json, response, SOURCE_BROWSE_BUF)) {
            heap_caps_free(response);
            source_show_error(&request, SOURCE_QUEUE, "Could not reach the Lyrion server");
            return;
        }
        cJSON *qroot = cJSON_Parse(response);
        heap_caps_free(response);
        cJSON *qres = qroot ? cJSON_GetObjectItem(qroot, "result") : NULL;
        if (!cJSON_IsObject(qres)) {
            if (qroot) cJSON_Delete(qroot);
            source_show_error(&request, SOURCE_QUEUE, "Unexpected reply from Lyrion");
            return;
        }

        int total = 0, current = -1;
        cJSON *tot = cJSON_GetObjectItem(qres, "playlist_tracks");
        if (cJSON_IsNumber(tot)) total = tot->valueint;
        cJSON *cur = cJSON_GetObjectItem(qres, "playlist_cur_index");
        if (cJSON_IsNumber(cur)) current = cur->valueint;
        else if (cJSON_IsString(cur) && cur->valuestring) current = atoi(cur->valuestring);

        source_item_count = 0;
        bool first_is_live = false;        /* a stream with no length: live radio */
        char station[SOURCE_TEXT_MAX] = "";
        cJSON *loop = cJSON_GetObjectItem(qres, "playlist_loop");
        cJSON *entry = NULL;
        cJSON_ArrayForEach(entry, loop) {
            cJSON *ix = cJSON_GetObjectItem(entry, "playlist index");
            cJSON *ti = cJSON_GetObjectItem(entry, "title");
            cJSON *ar = cJSON_GetObjectItem(entry, "artist");
            const int index = cJSON_IsNumber(ix) ? ix->valueint
                            : (cJSON_IsString(ix) && ix->valuestring ? atoi(ix->valuestring)
                               : request.offset + source_item_count);
            const char *title = (cJSON_IsString(ti) && ti->valuestring) ? ti->valuestring : "Unknown";
            const char *artist = (cJSON_IsString(ar) && ar->valuestring && ar->valuestring[0])
                               ? ar->valuestring : NULL;
            char text[SOURCE_TEXT_MAX];
            snprintf(text, sizeof(text), "%s%d. %s%s%s",
                     index == current ? LV_SYMBOL_PLAY " " : "", index + 1,
                     artist ? artist : "", artist ? " - " : "", title);
            source_add_item(index, SOURCE_QUEUE_JUMP, text);
            if (source_item_count > 0) {
                /* the song's library id, for the row's Favorites star
                 * (streams have none, or a negative one) */
                cJSON *tid = cJSON_GetObjectItem(entry, "id");
                source_items[source_item_count - 1].artwork_track_id =
                    cJSON_IsNumber(tid) ? tid->valueint
                    : (cJSON_IsString(tid) && tid->valuestring ? atoi(tid->valuestring) : 0);
                /* its address, for "add to playlist" (works for streams too) */
                cJSON *u = cJSON_GetObjectItem(entry, "url");
                snprintf(source_items[source_item_count - 1].radio_play_json,
                         sizeof(source_items[0].radio_play_json), "%s",
                         (cJSON_IsString(u) && u->valuestring) ? u->valuestring : "");
                if (source_item_count == 1) {
                    const char *url = source_items[0].radio_play_json;
                    cJSON *du = cJSON_GetObjectItem(entry, "duration");
                    const double len = cJSON_IsNumber(du) ? du->valuedouble
                                     : (cJSON_IsString(du) && du->valuestring ? atof(du->valuestring) : 0.0);
                    first_is_live = url[0] && strncmp(url, "file:", 5) != 0 &&
                                    strncmp(url, "db:", 3) != 0 && len <= 0.0;
                    cJSON *rt = cJSON_GetObjectItem(entry, "remote_title");   /* the station's name */
                    snprintf(station, sizeof(station), "%s",
                             (cJSON_IsString(rt) && rt->valuestring && rt->valuestring[0])
                                 ? rt->valuestring : title);
                }
            }
        }
        cJSON_Delete(qroot);
        source_has_next = (request.offset + SOURCE_QUEUE_PAGE < total);
        source_queue_total = total;
        source_queue_current = current;
        source_queue_radio = (total == 1 && source_item_count == 1 && first_is_live);
        if (source_queue_radio) {
            snprintf(source_items[0].title, sizeof(source_items[0].title), "%s", station);
        }
        if (!source_queue_keep_sel) source_queue_sel = -1;   /* only a move keeps it */
        source_queue_keep_sel = false;

        source_commit_view(&request, SOURCE_QUEUE);
        {
            char title[40];
            if (source_queue_radio) snprintf(title, sizeof(title), "QUEUE  (LIVE RADIO)");
            else snprintf(title, sizeof(title), total == 1 ? "QUEUE  (1 SONG)" : "QUEUE  (%d SONGS)", total);
            source_render_ctx_t ctx = { .title = title };
            ui_call(source_render_job, &ctx);
        }
        return;
    }

    source_mode_t display_mode = request.mode;
    const char *array_name = NULL;

    switch (request.mode) {
        case SOURCE_PLAYLISTS:
            snprintf(json, sizeof(json),
                     "{\"id\":1,\"method\":\"slim.request\",\"params\":[\"%s\",[\"playlists\",%d,%d]]}",
                     lyrion_players[player_index].id, request.offset, SOURCE_QUERY_ITEMS);
            array_name = "playlists_loop";
            break;
        case SOURCE_ARTISTS:
            snprintf(json, sizeof(json),
                     "{\"id\":1,\"method\":\"slim.request\",\"params\":[\"%s\",[\"artists\",%d,%d]]}",
                     lyrion_players[player_index].id, request.offset, SOURCE_QUERY_ITEMS);
            array_name = "artists_loop";
            break;
        case SOURCE_ALBUMS:
            snprintf(json, sizeof(json),
                     "{\"id\":1,\"method\":\"slim.request\",\"params\":[\"%s\",[\"albums\",%d,%d,\"tags:laj\"]]}",
                     lyrion_players[player_index].id, request.offset, SOURCE_QUERY_ITEMS);
            array_name = "albums_loop";
            break;
        case SOURCE_GENRES:
            snprintf(json, sizeof(json),
                     "{\"id\":1,\"method\":\"slim.request\",\"params\":[\"%s\",[\"genres\",%d,%d]]}",
                     lyrion_players[player_index].id, request.offset, SOURCE_QUERY_ITEMS);
            array_name = "genres_loop";
            break;
        case SOURCE_FOLDERS:
            snprintf(json, sizeof(json),
                     "{\"id\":1,\"method\":\"slim.request\",\"params\":[\"%s\",[\"musicfolder\",%d,%d]]}",
                     lyrion_players[player_index].id, request.offset, SOURCE_QUERY_ITEMS);
            array_name = "folder_loop";
            break;
        case SOURCE_FAVORITES:
            /* LMS returns the Favorites tree under result.items when feedMode:1
             * is requested.  This is the form returned by the user's server. */
            snprintf(json, sizeof(json),
                     "{\"id\":1,\"method\":\"slim.request\",\"params\":[\"%s\",[\"favorites\",\"items\",0,%d,\"feedMode:1\",\"want_url:1\"]]}",
                     lyrion_players[player_index].id,
                     request.offset + SOURCE_QUERY_ITEMS);   /* feedMode ignores the start: page locally */
            array_name = "items";
            display_mode = SOURCE_FAVORITES;
            break;
        case SOURCE_FAVORITE_CONTENT:
            snprintf(json, sizeof(json),
                     "{\"id\":1,\"method\":\"slim.request\",\"params\":[\"%s\",[\"favorites\",\"items\",%d,%d,\"want_url:1\",\"item_id:%s\"]]}",
                     lyrion_players[player_index].id, request.offset, SOURCE_QUERY_ITEMS,
                     request.item_id[0] ? request.item_id : "0");
            array_name = "item_loop";
            display_mode = SOURCE_FAVORITE_CONTENT;
            break;
        case SOURCE_RADIO_MENU:
            if (request.radio_action[0] != '\0') {
                if (!source_radio_action_to_request(request.radio_action, player_index, json, sizeof(json))) {
                    heap_caps_free(response);
                    ESP_LOGW(TAG, "Radio: could not turn this action into a request: %.300s",
                             request.radio_action);
                    source_show_error(&request, SOURCE_RADIO_MENU, "Radio action not supported");
                    return;
                }
                ESP_LOGI(TAG, "Radio request: %.400s", json);
            } else {
                /* LMS Radio's root menu is an XMLBrowser-style response.
                 * Use the same large item request as the working LMS web UI
                 * rather than limiting the root menu to our 8-item page size.
                 * We still display only SOURCE_MAX_ITEMS on this screen. */
                snprintf(json, sizeof(json),
                         source_menu_is_apps
                         ? "{\"id\":1,\"method\":\"slim.request\",\"params\":[\"%s\",[\"apps\",0,25000,\"menu:apps\"]]}"
                         : "{\"id\":1,\"method\":\"slim.request\",\"params\":[\"%s\",[\"radios\",0,25000,\"menu:radio\"]]}",
                         lyrion_players[player_index].id);
            }
            array_name = "item_loop";
            display_mode = SOURCE_RADIO_MENU;
            break;
        case SOURCE_ARTIST_ALBUMS:
            snprintf(json, sizeof(json),
                     "{\"id\":1,\"method\":\"slim.request\",\"params\":[\"%s\",[\"albums\",%d,%d,\"artist_id:%d\",\"tags:laj\"]]}",
                     lyrion_players[player_index].id, request.offset, SOURCE_QUERY_ITEMS, request.id);
            array_name = "albums_loop";
            display_mode = SOURCE_ARTIST_ALBUMS;
            break;
        case SOURCE_GENRE_ALBUMS:
            snprintf(json, sizeof(json),
                     "{\"id\":1,\"method\":\"slim.request\",\"params\":[\"%s\",[\"albums\",%d,%d,\"genre_id:%d\",\"tags:laj\"]]}",
                     lyrion_players[player_index].id, request.offset, SOURCE_QUERY_ITEMS, request.id);
            array_name = "albums_loop";
            display_mode = SOURCE_GENRE_ALBUMS;
            break;
        case SOURCE_FOLDER_CONTENT:
            snprintf(json, sizeof(json),
                     "{\"id\":1,\"method\":\"slim.request\",\"params\":[\"%s\",[\"musicfolder\",%d,%d,\"folder_id:%d\"]]}",
                     lyrion_players[player_index].id, request.offset, SOURCE_QUERY_ITEMS, request.id);
            array_name = "folder_loop";
            display_mode = SOURCE_FOLDER_CONTENT;
            break;
        case SOURCE_ALBUM_TRACKS:
            snprintf(json, sizeof(json),
                     "{\"id\":1,\"method\":\"slim.request\",\"params\":[\"%s\",[\"tracks\",%d,%d,\"album_id:%d\"]]}",
                     lyrion_players[player_index].id, request.offset, SOURCE_MAX_TRACKS, request.id);
            array_name = "titles_loop";
            display_mode = SOURCE_ALBUM_TRACKS;
            break;
        default:
            heap_caps_free(response);
            return;
    }

    ESP_LOGI(TAG, "Source query: mode=%d id=%d offset=%d",
             (int)request.mode, request.id, request.offset);
    if (request.mode == SOURCE_RADIO_MENU) {
        ESP_LOGI(TAG, "Radio request JSON: %s", json);
    }

    if (!source_http_query(player_index, json, response, resp_size)) {
        heap_caps_free(response);
        source_show_error(&request, display_mode, "Could not reach the Lyrion server");
        return;
    }

    /* The response buffer stays allocated until the (rare) radio fallback
     * below has had a chance to reuse it. */
    cJSON *root = cJSON_Parse(response);
    if (root == NULL) {
        if (request.mode == SOURCE_RADIO_MENU) {
            ESP_LOGW(TAG, "Radio response was not valid JSON");
        }
        heap_caps_free(response);
        source_show_error(&request, display_mode, "Unexpected reply from Lyrion");
        return;
    }

    cJSON *result = cJSON_GetObjectItem(root, "result");
    cJSON *array = result != NULL ? cJSON_GetObjectItem(result, array_name) : NULL;

    /*
     * Some LMS radio plugins expose their SlimBrowse "go" action with a
     * menu: parameter for SqueezePlay, but their custom command does not
     * accept that tagged parameter when called through JSON-RPC.  The
     * root Radio menu proves the plugin is present, so if the first
     * action returns no item_loop, retry the same command without the
     * action params.  This is deliberately limited to Radio submenus.
     */
    if (request.mode == SOURCE_RADIO_MENU &&
        request.radio_action[0] != '\0' &&
        (!cJSON_IsArray(array) || cJSON_GetArraySize(array) == 0)) {

        cJSON *action_retry = cJSON_Parse(request.radio_action);
        cJSON *retry_cmds = action_retry ? cJSON_GetObjectItem(action_retry, "cmd") : NULL;

        if (cJSON_IsArray(retry_cmds) && cJSON_GetArraySize(retry_cmds) > 0) {
            cJSON *rpc_params = cJSON_CreateArray();
            cJSON_AddItemToArray(rpc_params,
                                 cJSON_CreateString(lyrion_players[player_index].id));
            cJSON *cmd_array = cJSON_CreateArray();
            cJSON_AddItemToArray(rpc_params, cmd_array);

            cJSON *retry_cmd = NULL;
            cJSON_ArrayForEach(retry_cmd, retry_cmds) {
                if (cJSON_IsString(retry_cmd) && retry_cmd->valuestring) {
                    cJSON_AddItemToArray(cmd_array,
                                         cJSON_CreateString(retry_cmd->valuestring));
                }
            }

            cJSON *rpc_retry = cJSON_CreateObject();
            cJSON_AddNumberToObject(rpc_retry, "id", 1);
            cJSON_AddStringToObject(rpc_retry, "method", "slim.request");
            cJSON_AddItemToObject(rpc_retry, "params", rpc_params);

            char *retry_printed = cJSON_PrintUnformatted(rpc_retry);
            if (retry_printed != NULL && strlen(retry_printed) < sizeof(json)) {
                snprintf(json, sizeof(json), "%s", retry_printed);
                ESP_LOGI(TAG, "Radio fallback request JSON: %s", json);

                if (source_http_query(player_index, json, response, resp_size)) {
                    cJSON_Delete(root);
                    root = cJSON_Parse(response);
                    if (root != NULL) {
                        result = cJSON_GetObjectItem(root, "result");
                        array = result != NULL ? cJSON_GetObjectItem(result, array_name) : NULL;
                        ESP_LOGI(TAG, "Radio fallback response: item_loop=%d count=%d",
                                 cJSON_IsArray(array),
                                 cJSON_IsArray(array) ? cJSON_GetArraySize(array) : 0);
                        if (!cJSON_IsArray(array) || cJSON_GetArraySize(array) == 0) {
                            ESP_LOGI(TAG, "Radio fallback raw response: %.900s", response);
                        }
                    }
                }
            }

            if (retry_printed != NULL) free(retry_printed);
            cJSON_Delete(rpc_retry);
        }

        if (action_retry != NULL) cJSON_Delete(action_retry);
    }

    heap_caps_free(response);  /* everything needed is now in the cJSON tree */
    response = NULL;

    source_item_count = 0;
    source_artwork_track_id = 0;
    source_has_next = false;
    int first_album_track_id = 0;
    int first_folder_track_id = 0;
    char album_detail_name[SOURCE_TEXT_MAX] = "ALBUM TRACKS";

    if (request.mode == SOURCE_FOLDERS || request.mode == SOURCE_FOLDER_CONTENT ||
        request.mode == SOURCE_FAVORITES || request.mode == SOURCE_FAVORITE_CONTENT ||
        request.mode == SOURCE_RADIO_MENU) {
        ESP_LOGI(TAG, "Source special response: array=%s is_array=%d count=%d",
                 array_name, cJSON_IsArray(array), cJSON_IsArray(array) ? cJSON_GetArraySize(array) : 0);
    }

    const cJSON *radio_base_actions = NULL;
    if (request.mode == SOURCE_RADIO_MENU && result != NULL) {
        cJSON *base = cJSON_GetObjectItem(result, "base");
        if (cJSON_IsObject(base)) {
            radio_base_actions = cJSON_GetObjectItem(base, "actions");
        }
    }

    if (cJSON_IsArray(array)) {
        int returned_count = cJSON_GetArraySize(array);
        if ((request.mode == SOURCE_FOLDERS || request.mode == SOURCE_FOLDER_CONTENT ||
             request.mode == SOURCE_FAVORITES || request.mode == SOURCE_FAVORITE_CONTENT ||
             request.mode == SOURCE_RADIO_MENU) &&
            returned_count > 0) {
            cJSON *first = cJSON_GetArrayItem(array, 0);
            char *first_text = cJSON_PrintUnformatted(first);
            if (first_text != NULL) {
                ESP_LOGI(TAG, "SOURCE first item: %s", first_text);
                free(first_text);
            }
        }
        /* For album tracks fetch all; for everything else page at SOURCE_MAX_ITEMS */
        int item_limit = (request.mode == SOURCE_ALBUM_TRACKS) ? SOURCE_MAX_TRACKS : SOURCE_MAX_ITEMS;
        /* Radio menus and the Favorites list (feedMode ignores the start
         * index) arrive as one long list, so page through them locally;
         * every other list is paged by the server (offset in the request). */
        const int local_skip = (request.mode == SOURCE_RADIO_MENU || request.mode == SOURCE_FAVORITES)
                               ? request.offset : 0;
        source_has_next = (request.mode != SOURCE_ALBUM_TRACKS) &&
                          (returned_count > local_skip + SOURCE_MAX_ITEMS);
        cJSON *entry = NULL;
        int parsed = 0;
        int seen = 0;

        cJSON_ArrayForEach(entry, array) {
            if (seen++ < local_skip) continue;
            if (parsed >= item_limit) break;
            parsed++;

            cJSON *id_item = cJSON_GetObjectItem(entry, "id");
            int id = source_json_int(id_item);
            cJSON *title_item = NULL;

            if (request.mode == SOURCE_PLAYLISTS) {
                /* Lyrion puts a playlist's name in "playlist" (not "name"). */
                title_item = cJSON_GetObjectItem(entry, "playlist");
                if (!cJSON_IsString(title_item)) {
                    title_item = cJSON_GetObjectItem(entry, "name");
                }
                source_add_item(id, SOURCE_LOAD_PLAYLIST,
                                cJSON_IsString(title_item) ? title_item->valuestring : "Playlist");
            } else if (request.mode == SOURCE_ARTISTS) {
                title_item = cJSON_GetObjectItem(entry, "artist");
                source_add_item(id, SOURCE_ARTIST_ALBUMS,
                                cJSON_IsString(title_item) ? title_item->valuestring : "Artist");
            } else if (request.mode == SOURCE_GENRES) {
                title_item = cJSON_GetObjectItem(entry, "genre");
                source_add_item(id, SOURCE_GENRE_ALBUMS,
                                cJSON_IsString(title_item) ? title_item->valuestring : "Genre");
            } else if (request.mode == SOURCE_ALBUMS ||
                       request.mode == SOURCE_ARTIST_ALBUMS ||
                       request.mode == SOURCE_GENRE_ALBUMS) {
                title_item = cJSON_GetObjectItem(entry, "album");
                const int before = source_item_count;
                source_add_item(id, SOURCE_ALBUM_TRACKS,
                                cJSON_IsString(title_item) ? title_item->valuestring : "Album");
                if (source_item_count > before) {
                    /* For the cover grid: the artist, and a track to take the cover from. */
                    source_item_t *it = &source_items[before];
                    cJSON *ar = cJSON_GetObjectItem(entry, "artist");
                    if (cJSON_IsString(ar) && ar->valuestring) {
                        snprintf(it->subtitle, sizeof(it->subtitle), "%s", ar->valuestring);
                    }
                    /* Lyrion gives either a track number or a cover code such as
                     * "43f4745a"; /music/<either>/cover.png works for both, so
                     * keep it as text rather than converting it to a number. */
                    cJSON *aw = cJSON_GetObjectItem(entry, "artwork_track_id");
                    if (cJSON_IsNumber(aw)) {
                        snprintf(it->cover_key, sizeof(it->cover_key), "%d", aw->valueint);
                    } else if (cJSON_IsString(aw) && aw->valuestring && aw->valuestring[0]) {
                        snprintf(it->cover_key, sizeof(it->cover_key), "%.22s", aw->valuestring);
                    }
                }
            } else if (request.mode == SOURCE_RADIO_MENU) {
                source_radio_add_item(entry, radio_base_actions);
            } else if (request.mode == SOURCE_FAVORITES ||
                       request.mode == SOURCE_FAVORITE_CONTENT) {
                /* feedMode:1 returns the Favorites hierarchy as result.items.
                 * Leaf radio stations have a name + url. We can play those
                 * directly with LMS playlist play, while keeping normal
                 * Favorites item-id support for non-feedMode responses. */
                cJSON *name_item = cJSON_GetObjectItem(entry, "name");
                cJSON *url_item = cJSON_GetObjectItem(entry, "url");
                cJSON *fav_id_item = cJSON_GetObjectItem(entry, "id");
                const char *name = cJSON_IsString(name_item) ? name_item->valuestring : "Favorite";
                const int before = source_item_count;
                if (cJSON_IsString(url_item) && url_item->valuestring && url_item->valuestring[0]) {
                    source_add_favorite_item(url_item->valuestring, SOURCE_LOAD_FAVORITE_URL, name);
                    if (source_item_count > before) {
                        /* Full URL (item_id is only 96 bytes): removing it looks the
                         * favorite up by URL, since feedMode rows carry no id. */
                        snprintf(source_items[before].radio_play_json,
                                 sizeof(source_items[before].radio_play_json), "%s", url_item->valuestring);
                    }
                } else if (cJSON_IsString(fav_id_item) && fav_id_item->valuestring) {
                    source_add_favorite_item(fav_id_item->valuestring, SOURCE_LOAD_FAVORITE, name);
                }
                if (source_item_count > before && cJSON_IsString(fav_id_item) && fav_id_item->valuestring) {
                    /* Remember the favorite's own id, so it can be deleted. */
                    snprintf(source_items[before].fav_id, sizeof(source_items[before].fav_id), "%s",
                             fav_id_item->valuestring);
                }
                if (source_item_count > before) {
                    /* added above */
                } else {
                    /* A nested folder can still be displayed as a label. */
                    cJSON *children = cJSON_GetObjectItem(entry, "items");
                    if (cJSON_IsArray(children) && cJSON_GetArraySize(children) > 0) {
                        source_add_favorite_item("", SOURCE_FAVORITE_CONTENT, name);
                    }
                }
            } else if (request.mode == SOURCE_ALBUM_TRACKS) {
                if (first_album_track_id == 0 && id > 0) first_album_track_id = id;

                cJSON *art_item = cJSON_GetObjectItem(entry, "artwork_track_id");
                int art_id = source_json_int(art_item);
                if (source_artwork_track_id == 0 && art_id > 0) source_artwork_track_id = art_id;

                cJSON *cover_item = cJSON_GetObjectItem(entry, "coverid");
                int cover_id = source_json_int(cover_item);
                if (source_artwork_track_id == 0 && cover_id > 0) source_artwork_track_id = cover_id;

                cJSON *album_item = cJSON_GetObjectItem(entry, "album");
                if (cJSON_IsString(album_item) && album_item->valuestring) {
                    snprintf(album_detail_name, sizeof(album_detail_name), "%s", album_item->valuestring);
                }

                title_item = cJSON_GetObjectItem(entry, "title");
                if (!cJSON_IsString(title_item)) title_item = cJSON_GetObjectItem(entry, "track");
                cJSON *tracknum_item = cJSON_GetObjectItem(entry, "tracknum");
                char track_title[SOURCE_TEXT_MAX];
                if (cJSON_IsString(tracknum_item) && tracknum_item->valuestring &&
                    cJSON_IsString(title_item) && title_item->valuestring) {
                    snprintf(track_title, sizeof(track_title), "%s  %s",
                             tracknum_item->valuestring, title_item->valuestring);
                } else if (cJSON_IsNumber(tracknum_item) &&
                           cJSON_IsString(title_item) && title_item->valuestring) {
                    snprintf(track_title, sizeof(track_title), "%02d  %s",
                             tracknum_item->valueint, title_item->valuestring);
                } else {
                    snprintf(track_title, sizeof(track_title), "%s",
                             cJSON_IsString(title_item) ? title_item->valuestring : "Track");
                }
                source_add_item(id, SOURCE_LOAD_TRACK, track_title);
            } else if (request.mode == SOURCE_FOLDERS ||
                       request.mode == SOURCE_FOLDER_CONTENT) {
                title_item = cJSON_GetObjectItem(entry, "title");
                if (!cJSON_IsString(title_item)) title_item = cJSON_GetObjectItem(entry, "filename");
                if (!cJSON_IsString(title_item)) title_item = cJSON_GetObjectItem(entry, "name");
                cJSON *type_item = cJSON_GetObjectItem(entry, "type");
                const char *type = cJSON_IsString(type_item) ? type_item->valuestring : "";

                /* Folder listings sometimes expose coverid/artwork_track_id. */
                cJSON *cover_item = cJSON_GetObjectItem(entry, "coverid");
                int cover_id = source_json_int(cover_item);
                cJSON *art_item = cJSON_GetObjectItem(entry, "artwork_track_id");
                int art_id = source_json_int(art_item);
                if (source_artwork_track_id == 0) {
                    if (cover_id > 0) source_artwork_track_id = cover_id;
                    else if (art_id > 0) source_artwork_track_id = art_id;
                }

                source_mode_t action = SOURCE_LOAD_TRACK;
                if (strcmp(type, "folder") == 0 || strcmp(type, "dir") == 0) {
                    action = SOURCE_FOLDER_CONTENT;
                } else if (strcmp(type, "playlist") == 0) {
                    action = SOURCE_LOAD_PLAYLIST;
                } else if (source_artwork_track_id == 0 &&
                           request.mode == SOURCE_FOLDER_CONTENT && id > 0) {
                    /* A track id is also a valid artwork URL id. */
                    source_artwork_track_id = id;
                }
                if (action == SOURCE_LOAD_TRACK && id > 0 && first_folder_track_id == 0) {
                    first_folder_track_id = id;
                }
                source_add_item(id, action,
                                cJSON_IsString(title_item) ? title_item->valuestring : "Folder item");
            }
        }
    }

    cJSON_Delete(root);

    source_commit_view(&request, display_mode);

    if (request.mode == SOURCE_RADIO_MENU) {
        if (request.item_id[0] != '\0') {
            /* request.item_id contains the child action. The parent command
             * is already stored by the click handler before navigation. */
        }
        snprintf(source_current_title, sizeof(source_current_title), "INTERNET RADIO");
    }

    if (request.mode == SOURCE_ALBUM_TRACKS) {
        snprintf(source_current_title, sizeof(source_current_title), "%s", album_detail_name);
        if (source_artwork_track_id == 0) source_artwork_track_id = first_album_track_id;
        ESP_LOGI(TAG, "Album artwork candidate: %d", source_artwork_track_id);
    } else {
        snprintf(source_current_title, sizeof(source_current_title), "%s", source_mode_title(display_mode));
    }

    /* Ticks on songs that are already in the queue. */
    source_mark_queued_items(player_index);

    /* Album / folder cover: albums are keyed by id, folders by minus id. */
    int art_track_id = 0;
    int art_key = 0;
    if (SOURCE_ART_ALBUMS && request.mode == SOURCE_ALBUM_TRACKS && request.id > 0) {
        art_track_id = first_album_track_id;
        art_key = request.id;
    } else if (SOURCE_ART_FOLDERS && request.mode == SOURCE_FOLDER_CONTENT && request.id > 0) {
        art_track_id = first_folder_track_id;
        art_key = -request.id;
    }
    if (art_track_id <= 0) {
        art_key = 0;
    }

    {
        source_render_ctx_t ctx = { .title = source_current_title, .art_key = art_key,
                                    .set_art_key = true };
        ui_call(source_render_job, &ctx);
    }

    /* Fetched after the list is visible, so browsing never waits for it. */
    ESP_LOGI(TAG, "Source view drawn: mode=%d items=%d art_key=%d art_track=%d",
             (int)request.mode, source_item_count, art_key, art_track_id);
    if (art_key != 0 && source_art_key_shown != art_key) {
        source_fetch_art(art_track_id, art_key);
    }
    if (source_grid_mode()) {
        source_grid_fetch_covers();
    }
}

/* SELECT: add / play next several songs in one command, in the order ticked. */
static void source_select_queue(source_request_t request)
{
    const int p = current_player_index;
    if (p < 0 || p >= lyrion_player_count || request.radio_action[0] == '\0') return;
    char command[1024];
    snprintf(command, sizeof(command),
             "{\"id\":1,\"method\":\"slim.request\",\"params\":[\"%s\",[\"playlistcontrol\",\"cmd:%s\",\"track_id:%.800s\"]]}",
             lyrion_players[p].id, request.queue_op == QUEUE_OP_INSERT ? "insert" : "add", request.radio_action);
    char response[512];
    const bool ok = source_http_query(p, command, response, sizeof(response));
    ESP_LOGI(TAG, "SELECT %s %d song(s): %s", request.queue_op == QUEUE_OP_INSERT ? "play next" : "queue",
             request.id, ok ? "ok" : "failed");
    if (!ok) ui_toast_post("Could not reach the Lyrion server");
    if (lyrion_refresh_event != NULL) xEventGroupSetBits(lyrion_refresh_event, LYRION_REFRESH_BIT);
}

static void source_send_load(source_request_t request)
{
    int player_index = current_player_index;
    const char *player_id = lyrion_players[player_index].id;
    char command[1024];
    const char *cmd = (request.queue_op == QUEUE_OP_ADD) ? "add"
                    : (request.queue_op == QUEUE_OP_INSERT) ? "insert" : "load";
    const char *fav_cmd = (request.queue_op == QUEUE_OP_ADD) ? "add"
                        : (request.queue_op == QUEUE_OP_INSERT) ? "insert" : "play";

    switch (request.mode) {
        case SOURCE_LOAD_PLAYLIST:
            snprintf(command, sizeof(command),
                     "{\"id\":1,\"method\":\"slim.request\",\"params\":[\"%s\",[\"playlistcontrol\",\"cmd:%s\",\"playlist_id:%d\"]]}",
                     player_id, cmd, request.id);
            break;
        case SOURCE_LOAD_ALBUM:
            snprintf(command, sizeof(command),
                     "{\"id\":1,\"method\":\"slim.request\",\"params\":[\"%s\",[\"playlistcontrol\",\"cmd:%s\",\"album_id:%d\"]]}",
                     player_id, cmd, request.id);
            break;
        case SOURCE_LOAD_ARTIST:
            snprintf(command, sizeof(command),
                     "{\"id\":1,\"method\":\"slim.request\",\"params\":[\"%s\",[\"playlistcontrol\",\"cmd:%s\",\"artist_id:%d\"]]}",
                     player_id, cmd, request.id);
            break;
        case SOURCE_LOAD_GENRE:
            snprintf(command, sizeof(command),
                     "{\"id\":1,\"method\":\"slim.request\",\"params\":[\"%s\",[\"playlistcontrol\",\"cmd:%s\",\"genre_id:%d\"]]}",
                     player_id, cmd, request.id);
            break;
        case SOURCE_LOAD_FOLDER:
            snprintf(command, sizeof(command),
                     "{\"id\":1,\"method\":\"slim.request\",\"params\":[\"%s\",[\"playlistcontrol\",\"cmd:%s\",\"folder_id:%d\"]]}",
                     player_id, cmd, request.id);
            break;
        case SOURCE_LOAD_TRACK:
            snprintf(command, sizeof(command),
                     "{\"id\":1,\"method\":\"slim.request\",\"params\":[\"%s\",[\"playlistcontrol\",\"cmd:%s\",\"track_id:%d\"]]}",
                     player_id, cmd, request.id);
            break;
        case SOURCE_LOAD_FAVORITE:
            snprintf(command, sizeof(command),
                     "{\"id\":1,\"method\":\"slim.request\",\"params\":[\"%s\",[\"favorites\",\"playlist\",\"%s\",\"item_id:%s\"]]}",
                     player_id, fav_cmd, request.item_id);
            break;
        case SOURCE_LOAD_FAVORITE_URL:
            snprintf(command, sizeof(command),
                     "{\"id\":1,\"method\":\"slim.request\",\"params\":[\"%s\",[\"playlist\",\"%s\",\"%.700s\"]]}",
                     player_id, fav_cmd, request.radio_action[0] ? request.radio_action : request.item_id);
            break;
        case SOURCE_RADIO_PLAY:
            if (request.queue_op != QUEUE_OP_PLAY) {
                /* "+" on an app result: the same action with "add" / "insert"
                 * in place of "play", so it joins the queue instead. */
                char changed[sizeof(request.radio_action)];
                if (!source_radio_action_as(request.radio_action, request.queue_op == QUEUE_OP_INSERT ? "insert" : "add",
                                            changed, sizeof(changed)) ||
                    !source_radio_action_to_request(changed, player_index, command, sizeof(command))) {
                    ui_toast_post("This one can't be added to the queue - tap it to play it");
                    return;
                }
                break;
            }
            if (!source_radio_action_to_request(request.radio_action, player_index, command, sizeof(command))) {
                return;
            }
            break;
        default:
            return;
    }

    ESP_LOGI(TAG, "%s source item %d for %s",
             request.queue_op == QUEUE_OP_ADD ? "Queueing" :
             request.queue_op == QUEUE_OP_INSERT ? "Playing next:" : "Loading",
             request.id, lyrion_players[player_index].name);

    char response[LYRION_RESPONSE_MAX];
    (void)source_http_query(player_index, command, response, sizeof(response));

    vTaskDelay(pdMS_TO_TICKS(200));
    if (lyrion_refresh_event != NULL) {
        xEventGroupSetBits(lyrion_refresh_event, LYRION_REFRESH_BIT);
    }

    /* Adding to the queue keeps you browsing; playing closes SOURCE. */
    if (request.queue_op == QUEUE_OP_PLAY) {
        ui_call(source_hide_job, NULL);
    }
}

/* Jump / remove / clear on the player's queue, then redraw the queue view. */
static void source_queue_command(source_request_t request)
{
    const int player_index = current_player_index;
    const char *player_id = lyrion_players[player_index].id;
    char command[256];

    switch (request.mode) {
        case SOURCE_QUEUE_JUMP:
            snprintf(command, sizeof(command),
                     "{\"id\":1,\"method\":\"slim.request\",\"params\":[\"%s\",[\"playlist\",\"index\",\"%d\"]]}",
                     player_id, request.id);
            break;
        case SOURCE_QUEUE_REMOVE:
            snprintf(command, sizeof(command),
                     "{\"id\":1,\"method\":\"slim.request\",\"params\":[\"%s\",[\"playlist\",\"delete\",\"%d\"]]}",
                     player_id, request.id);
            break;
        case SOURCE_QUEUE_CLEAR:
            snprintf(command, sizeof(command),
                     "{\"id\":1,\"method\":\"slim.request\",\"params\":[\"%s\",[\"playlist\",\"clear\"]]}",
                     player_id);
            break;
        case SOURCE_QUEUE_MOVE:
            snprintf(command, sizeof(command),
                     "{\"id\":1,\"method\":\"slim.request\",\"params\":[\"%s\",[\"playlist\",\"move\",\"%d\",\"%d\"]]}",
                     player_id, request.id, request.queue_op);
            /* Redraw with the moved song still selected, on its new page. */
            source_queue_sel = request.queue_op;
            source_queue_keep_sel = true;
            break;
        default:
            return;
    }
    ESP_LOGI(TAG, "Queue command (%d) index %d for %s", (int)request.mode, request.id,
             lyrion_players[player_index].name);
    if (request.mode == SOURCE_QUEUE_CLEAR) ui_toast_post("Queue cleared");

    char response[LYRION_RESPONSE_MAX];
    (void)source_http_query(player_index, command, response, sizeof(response));
    vTaskDelay(pdMS_TO_TICKS(200));
    if (lyrion_refresh_event != NULL) {
        xEventGroupSetBits(lyrion_refresh_event, LYRION_REFRESH_BIT);
    }

    /* PLAY: back to the Now Playing screen instead of redrawing the queue. */
    if (request.mode == SOURCE_QUEUE_JUMP && request.queue_op == 1) {
        ui_call(source_hide_job, NULL);
        return;
    }

    /* Redraw the queue at the same page (or the start, after a clear). */
    source_request_t view = { .mode = SOURCE_QUEUE, .push_history = false };
    view.offset = (request.mode == SOURCE_QUEUE_CLEAR) ? 0 : request.offset;
    xQueueSend(source_queue, &view, 0);
}

/* Returns 1 if a playlist with exactly this name exists, 0 if not, -1 on error. */
static int source_playlist_exists(int player_index, const char *name, char *buf)
{
    /* The full list, compared here: Lyrion's own search ignores punctuation,
     * so searching for "Queue 23 Sep 21.31" may not find that exact name. */
    char json[160];
    snprintf(json, sizeof(json),
             "{\"id\":1,\"method\":\"slim.request\",\"params\":[\"%s\",[\"playlists\",0,999]]}",
             lyrion_players[player_index].id);
    (void)name;
    if (!source_http_query(player_index, json, buf, SOURCE_BROWSE_BUF)) return -1;

    cJSON *root = cJSON_Parse(buf);
    cJSON *res = root ? cJSON_GetObjectItem(root, "result") : NULL;
    cJSON *loop = cJSON_IsObject(res) ? cJSON_GetObjectItem(res, "playlists_loop") : NULL;
    int found = 0;
    cJSON *entry = NULL;
    cJSON_ArrayForEach(entry, loop) {
        cJSON *pl = cJSON_GetObjectItem(entry, "playlist");
        if (cJSON_IsString(pl) && pl->valuestring && strcasecmp(pl->valuestring, name) == 0) {
            found = 1;
            break;
        }
    }
    if (root) cJSON_Delete(root);
    return root ? found : -1;
}

/* Save the player's current queue under request.title.  request.id == 1
 * means the user already confirmed replacing an existing playlist. */
static void source_queue_save(source_request_t request)
{
    const int player_index = current_player_index;
    char *buf = (char *)heap_caps_malloc(SOURCE_BROWSE_BUF, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (buf == NULL) return;

    char msg[256];                    /* room for the longest name plus the text */
    const char *name = request.title;

    if (request.id != 1) {
        const int exists = source_playlist_exists(player_index, name, buf);
        if (exists < 0) {
            heap_caps_free(buf);
            qs_status_from_task("Could not reach the Lyrion server.", UI_RED, false);
            return;
        }
        if (exists == 1) {
            heap_caps_free(buf);
            snprintf(msg, sizeof(msg),
                     "A playlist called \"%s\" already exists. Tap SAVE again to replace it, "
                     "or change the name.", name);
            qs_status_ctx_t c = { .text = msg, .color = UI_AMBER, .replace_name = name };
            ui_call(qs_status_job, &c);
            return;
        }
    }

    char json[256];
    snprintf(json, sizeof(json),
             "{\"id\":1,\"method\":\"slim.request\",\"params\":[\"%s\",[\"playlist\",\"save\",\"%s\",\"silent:1\"]]}",
             lyrion_players[player_index].id, name);
    const bool sent = source_http_query(player_index, json, buf, SOURCE_BROWSE_BUF);

    /* Lyrion answers a successful save with the new playlist's id. */
    bool reply_ok = false;
    if (sent) {
        ESP_LOGI(TAG, "Playlist save reply: %.200s", buf);
        cJSON *r = cJSON_Parse(buf);
        cJSON *res = r ? cJSON_GetObjectItem(r, "result") : NULL;
        cJSON *pid = cJSON_IsObject(res) ? cJSON_GetObjectItem(res, "__playlist_id") : NULL;
        reply_ok = (pid != NULL && !cJSON_IsNull(pid));
        if (r) cJSON_Delete(r);
    }

    /* Then confirm by finding it in the playlist list. */
    vTaskDelay(pdMS_TO_TICKS(500));
    int saved = sent ? source_playlist_exists(player_index, name, buf) : -1;
    if (saved != 1 && reply_ok) saved = 1;
    heap_caps_free(buf);

    ESP_LOGI(TAG, "Save queue as \"%s\": %s", name, saved == 1 ? "ok" : "failed");
    if (saved == 1) {
        snprintf(msg, sizeof(msg), "Saved as \"%s\". Find it under Playlists.", name);
        qs_status_ctx_t c = { .text = msg, .color = UI_ACCENT_GLOW, .close_soon = true,
                              .clear_replace = true };
        ui_call(qs_status_job, &c);
    } else if (!sent) {
        qs_status_from_task("Could not reach the Lyrion server.", UI_RED, false);
    } else {
        qs_status_from_task("Lyrion did not save it. Check that a Playlists folder is set "
                  "in Lyrion's Settings > Basic Settings.", UI_RED, false);
    }
}

/* Delete a saved playlist (Lyrion also removes its file), then redraw. */
static void source_playlist_delete(source_request_t request)
{
    const int player_index = current_player_index;
    char json[192];
    snprintf(json, sizeof(json),
             "{\"id\":1,\"method\":\"slim.request\",\"params\":[\"%s\",[\"playlists\",\"delete\",\"playlist_id:%d\"]]}",
             lyrion_players[player_index].id, request.id);

    char reply[LYRION_RESPONSE_MAX];
    const bool ok = source_http_query(player_index, json, reply, sizeof(reply));
    ESP_LOGI(TAG, "Delete playlist \"%s\" (id %d): %s | reply: %.160s",
             request.title, request.id, ok ? "sent" : "failed", ok ? reply : "-");
    ui_toast_post(ok ? "Playlist deleted" : "Could not delete the playlist");

    vTaskDelay(pdMS_TO_TICKS(300));
    source_request_t view = { .mode = SOURCE_PLAYLISTS, .offset = request.offset,
                              .push_history = false };
    xQueueSend(source_queue, &view, 0);
}

