/*
 * P4-LMS - Saved playlists: "add to playlist", and viewing / editing one
 *
 * Part of main.c.  This file is #included by main.c (in order) and is not
 * compiled on its own, so it shares main.c's includes and static names.
 *
 * The playlists live in Lyrion (its Playlists folder), so they also show in
 * Lyrion's web page and apps, and the alarm editor can use them.
 *
 *   - Now Playing "+ PLAYLIST", a queue row's list button and SELECT > PLAYLIST
 *     fill pl_pending and open the chooser (a list of playlists plus
 *     "NEW PLAYLIST", which uses the save-queue name panel in mode 1).
 *   - Library > Playlists > a playlist shows its songs 7 at a time, like the
 *     queue: tap a song to play the playlist from there; its "..." opens
 *     up / down / add-to-queue / remove.  RENAME and PLAY ALL sit in the
 *     bottom bar.
 * ======================================================================== */

#define PL_PICK_MAX 100
typedef struct { int id; char name[64]; } pl_entry_t;
static pl_entry_t *pl_entries = NULL;          /* PSRAM, filled by the browser task */
static int pl_entry_count = 0;

static lv_obj_t *pl_picker = NULL, *pl_picker_panel = NULL;
static lv_obj_t *pl_picker_what = NULL, *pl_picker_list = NULL, *pl_picker_status = NULL;

/* ------------------------------------------------------------ the chooser */
static void pl_picker_hide(void)
{
    if (pl_picker != NULL) lv_obj_add_flag(pl_picker, LV_OBJ_FLAG_HIDDEN);
}

static void pl_pick_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED || source_queue == NULL) return;
    const int k = (int)(intptr_t)lv_event_get_user_data(e);
    if (k < 0 || k >= pl_entry_count || pl_entries == NULL) return;
    source_request_t r = { .mode = SOURCE_PL_ADD, .id = pl_entries[k].id, .push_history = false,
                           .queue_op = pl_pending.kind, .offset = pl_pending.count };
    snprintf(r.item_id, sizeof(r.item_id), "%.90s", pl_entries[k].name);
    snprintf(r.radio_action, sizeof(r.radio_action), "%s", pl_pending.payload);
    snprintf(r.title, sizeof(r.title), "%s", pl_pending.what);
    xQueueSend(source_queue, &r, 0);
    char msg[128];
    snprintf(msg, sizeof(msg), "Adding to %.60s...", pl_entries[k].name);
    ui_toast(msg);
    pl_picker_hide();
}

static void pl_new_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    pl_picker_hide();
    qs_open(1, "NEW PLAYLIST", "", "Type a name, then SAVE. What you chose goes into it.");
}

static void pl_cancel_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) pl_picker_hide();
}

static void pl_picker_create(void)
{
    pl_picker = modal_create(lv_screen_active(), 640, 470, &pl_picker_panel);
    ui_text(pl_picker_panel, "ADD TO PLAYLIST", &p4_font_28, UI_TEXT, 26, 18, 0);
    pl_picker_what = ui_text(pl_picker_panel, "", &p4_font_16, UI_TEXT_SOFT, 26, 60, 0);
    lv_obj_set_width(pl_picker_what, 590);
    lv_label_set_long_mode(pl_picker_what, LV_LABEL_LONG_DOT);
    label_one_line(pl_picker_what);

    pl_picker_list = lv_obj_create(pl_picker_panel);
    lv_obj_remove_style_all(pl_picker_list);
    lv_obj_set_size(pl_picker_list, 590, 290);
    lv_obj_set_pos(pl_picker_list, 25, 92);
    lv_obj_add_flag(pl_picker_list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(pl_picker_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(pl_picker_list, LV_SCROLLBAR_MODE_AUTO);

    pl_picker_status = ui_text(pl_picker_panel, "", &p4_font_16, UI_TEXT_SOFT, 26, 100, 0);
    lv_obj_set_width(pl_picker_status, 590);
    lv_label_set_long_mode(pl_picker_status, LV_LABEL_LONG_WRAP);

    lv_obj_t *nw = create_button(pl_picker_panel, LV_SYMBOL_PLUS "  NEW PLAYLIST", 240, 52);
    lv_obj_set_pos(nw, 25, 396);
    lv_obj_set_style_bg_color(nw, lv_color_hex(UI_ACCENT), LV_PART_MAIN);
    lv_obj_set_style_text_color(lv_obj_get_child(nw, 0), lv_color_hex(UI_ON_ACCENT), 0);
    lv_obj_add_event_cb(nw, pl_new_event_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *cancel = create_button(pl_picker_panel, "CANCEL", 150, 50);
    lv_obj_set_pos(cancel, 465, 396);
    lv_obj_add_event_cb(cancel, pl_cancel_event_cb, LV_EVENT_CLICKED, NULL);
}

/* Open the chooser for whatever pl_pending holds. */
static void pl_picker_open(void)
{
    if (source_queue == NULL) return;
    if (pl_picker == NULL) pl_picker_create();
    char what[SOURCE_TEXT_MAX + 16];
    snprintf(what, sizeof(what), "Adding:  %s", pl_pending.what);
    lv_label_set_text(pl_picker_what, what);
    lv_obj_clean(pl_picker_list);
    lv_label_set_text(pl_picker_status, "Loading your playlists...");
    lv_obj_clear_flag(pl_picker_status, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(pl_picker, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(pl_picker);
    source_request_t r = { .mode = SOURCE_PL_LIST, .push_history = false };
    xQueueSend(source_queue, &r, 0);
}

typedef struct { bool ok; } pl_fill_ctx_t;
static void pl_picker_fill_job(void *arg)
{
    const pl_fill_ctx_t *c = (const pl_fill_ctx_t *)arg;
    if (pl_picker == NULL || lv_obj_has_flag(pl_picker, LV_OBJ_FLAG_HIDDEN)) return;
    lv_obj_clean(pl_picker_list);
    if (!c->ok) {
        lv_label_set_text(pl_picker_status, "Could not reach the Lyrion server.");
        return;
    }
    if (pl_entry_count == 0) {
        lv_label_set_text(pl_picker_status, "No playlists yet - tap NEW PLAYLIST to start one.");
        return;
    }
    lv_obj_add_flag(pl_picker_status, LV_OBJ_FLAG_HIDDEN);
    for (int k = 0; k < pl_entry_count; k++) {
        lv_obj_t *b = create_source_item_button(pl_picker_list, pl_entries[k].name, 574, 50, false, true);
        lv_obj_set_pos(b, 0, k * 56);
        lv_obj_t *icon = lv_obj_get_child(b, 1);
        if (icon != NULL && lv_obj_check_type(icon, &lv_label_class)) lv_label_set_text(icon, LV_SYMBOL_LIST);
        lv_obj_add_event_cb(b, pl_pick_event_cb, LV_EVENT_CLICKED, (void *)(intptr_t)k);
    }
    lv_obj_scroll_to(pl_picker_list, 0, 0, LV_ANIM_OFF);
}

/* Now Playing "+ PLAYLIST": what's playing into a playlist. */
static void np_list_button_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (np_fav_url[0] == '\0') return;
    memset(&pl_pending, 0, sizeof(pl_pending));
    pl_pending.kind = PL_KIND_URL;
    pl_pending.count = 1;
    snprintf(pl_pending.payload, sizeof(pl_pending.payload), "%s", np_fav_url);
    snprintf(pl_pending.what, sizeof(pl_pending.what), "%.90s", np_fav_title[0] ? np_fav_title : "What's playing");
    pl_picker_open();
}

/* ------------------------------------------------------ a playlist's songs */
#define PL_MOVE_UP   0
#define PL_MOVE_DOWN 1

static void pl_move_event_cb(lv_event_t *e)
{
    if (source_list_busy && lv_event_get_code(e) == LV_EVENT_CLICKED) return;   /* a new page is loading */
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    const int packed = (int)(intptr_t)lv_event_get_user_data(e);
    const int i = packed >> 2, kind = packed & 3;
    if (i < 0 || i >= source_item_count || source_queue == NULL || source_queue_busy) return;
    const int from = source_items[i].id;
    const int to = (kind == PL_MOVE_UP) ? from - 1 : from + 1;
    if (to < 0 || to >= source_queue_total) return;
    source_queue_busy = true;
    source_request_t r = { .mode = SOURCE_PL_EDIT, .id = source_parent_id, .queue_op = PL_EDIT_MOVE,
                           .offset = 0, .push_history = false };
    snprintf(r.item_id, sizeof(r.item_id), "%d,%d", from, to);
    snprintf(r.title, sizeof(r.title), "%s", source_folder_title);
    if (xQueueSend(source_queue, &r, 0) != pdTRUE) source_queue_busy = false;
}

static void pl_remove_event_cb(lv_event_t *e)
{
    if (source_list_busy && lv_event_get_code(e) == LV_EVENT_CLICKED) return;   /* a new page is loading */
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    const int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i < 0 || i >= source_item_count || source_queue == NULL || source_queue_busy) return;
    source_queue_busy = true;
    source_request_t r = { .mode = SOURCE_PL_EDIT, .id = source_parent_id, .queue_op = PL_EDIT_DELETE,
                           .offset = 0, .push_history = false };
    snprintf(r.item_id, sizeof(r.item_id), "%d", source_items[i].id);
    snprintf(r.title, sizeof(r.title), "%s", source_folder_title);
    if (xQueueSend(source_queue, &r, 0) != pdTRUE) { source_queue_busy = false; return; }
    ui_toast("Removed from the playlist");
}

/* "+" on a playlist song: add just that song to the queue. */
static void pl_queue_event_cb(lv_event_t *e)
{
    if (source_list_busy && lv_event_get_code(e) == LV_EVENT_CLICKED) return;   /* a new page is loading */
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    const int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i < 0 || i >= source_item_count || source_queue == NULL) return;
    const source_item_t *it = &source_items[i];
    source_request_t r = { .push_history = false, .queue_op = QUEUE_OP_ADD };
    if (it->artwork_track_id > 0) {
        r.mode = SOURCE_LOAD_TRACK;
        r.id = it->artwork_track_id;
    } else if (it->radio_play_json[0]) {
        r.mode = SOURCE_LOAD_FAVORITE_URL;
        snprintf(r.radio_action, sizeof(r.radio_action), "%s", it->radio_play_json);
    } else {
        return;
    }
    xQueueSend(source_queue, &r, 0);
    char msg[128];
    snprintf(msg, sizeof(msg), "Added to queue:  %.70s", queue_row_name(it->title));
    ui_toast(msg);
}

/* "..." and the (hidden) control strip on playlist song row i:  ^  v  +  x */
static void pl_row_add_controls(lv_obj_t *row, int i)
{
    lv_obj_t *more = queue_ctrl_button(row, LV_SYMBOL_BARS, 0, 40, UI_BUTTON, queue_more_event_cb, i);
    lv_obj_align(more, LV_ALIGN_RIGHT_MID, -5, 0);
    lv_obj_set_style_border_width(more, 1, 0);
    lv_obj_set_style_border_color(more, lv_color_hex(UI_EDGE), 0);
    lv_obj_set_style_text_color(lv_obj_get_child(more, 0), lv_color_hex(UI_TEXT_SOFT), 0);
    queue_row_more[i] = more;

    lv_obj_t *strip = lv_obj_create(row);
    lv_obj_remove_style_all(strip);
    lv_obj_set_size(strip, 206, 46);
    lv_obj_align(strip, LV_ALIGN_RIGHT_MID, -4, 0);
    lv_obj_clear_flag(strip, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(strip, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(strip, LV_OBJ_FLAG_HIDDEN);
    queue_row_strip[i] = strip;               /* its buttons: pl_row_fill_strip */
}

/* The controls on playlist row i, made when "..." first opens them. */
static void pl_row_fill_strip(int i)
{
    lv_obj_t *strip = queue_row_strip[i];
    if (strip == NULL) return;
    const int idx = source_items[i].id;
    lv_obj_t *up   = queue_ctrl_button(strip, LV_SYMBOL_UP,   0,   42, UI_ACCENT, pl_move_event_cb, (i << 2) | PL_MOVE_UP);
    lv_obj_t *down = queue_ctrl_button(strip, LV_SYMBOL_DOWN, 50,  42, UI_ACCENT, pl_move_event_cb, (i << 2) | PL_MOVE_DOWN);
    lv_obj_t *add  = queue_ctrl_button(strip, LV_SYMBOL_PLUS, 100, 42, UI_ACCENT, pl_queue_event_cb, i);
    /* Remove sits a little apart from the others, so it isn't hit by mistake. */
    queue_ctrl_button(strip, LV_SYMBOL_CLOSE, 164, 42, UI_RED, pl_remove_event_cb, i);
    if (idx <= 0) lv_obj_add_state(up, LV_STATE_DISABLED);
    if (idx >= source_queue_total - 1) lv_obj_add_state(down, LV_STATE_DISABLED);
    if (source_items[i].artwork_track_id <= 0 && !source_items[i].radio_play_json[0]) {
        lv_obj_add_state(add, LV_STATE_DISABLED);
    }
}

/* Rows like the queue's: tap to play from here, "..." for the controls. */
static void source_render_playlist(void)
{
    source_render_song_list();
}

/* ============================================================ browser task */
/* One Lyrion command for the current player; reply in buf. */
static bool pl_cmd(const char *params, char *buf, size_t n)
{
    const int p = current_player_index;
    if (p < 0 || p >= lyrion_player_count) return false;
    const size_t jl = strlen(params) + 160;
    char *json = heap_caps_malloc(jl, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (json == NULL) return false;
    snprintf(json, jl, "{\"id\":1,\"method\":\"slim.request\",\"params\":[\"%s\",[%s]]}",
             lyrion_players[p].id, params);
    const bool ok = source_http_query(p, json, buf, n);
    heap_caps_free(json);
    return ok;
}

static int pl_json_int(const cJSON *o)
{
    if (cJSON_IsNumber(o)) return o->valueint;
    if (cJSON_IsString(o) && o->valuestring) return atoi(o->valuestring);
    return 0;
}

/* Add one address to playlist pl_id (title helps name a stream). */
static bool pl_add_url(int pl_id, const char *url, const char *title, char *buf, size_t n)
{
    char *p = heap_caps_malloc(2048, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (p == NULL) return false;
    char arg[900];
    int len = snprintf(p, 2048, "\"playlists\",\"edit\",\"cmd:add\",\"playlist_id:%d\"", pl_id);
    json_kv_arg(arg, sizeof(arg), "url", url);
    len += snprintf(p + len, 2048 - len, "%s", arg);
    if (title != NULL && title[0]) {
        json_kv_arg(arg, sizeof(arg), "title", title);
        snprintf(p + len, 2048 - len, "%s", arg);
    }
    const bool ok = pl_cmd(p, buf, n);
    heap_caps_free(p);
    return ok;
}

/* A library song's address. */
static bool pl_track_url(int track_id, char *url, size_t url_size, char *buf, size_t n)
{
    char p[96];
    snprintf(p, sizeof(p), "\"songinfo\",0,100,\"track_id:%d\",\"tags:u\"", track_id);
    url[0] = '\0';
    if (!pl_cmd(p, buf, n)) return false;
    cJSON *root = cJSON_Parse(buf);
    cJSON *res = root ? cJSON_GetObjectItem(root, "result") : NULL;
    cJSON *loop = cJSON_IsObject(res) ? cJSON_GetObjectItem(res, "songinfo_loop") : NULL;
    cJSON *e = NULL;
    cJSON_ArrayForEach(e, loop) {
        cJSON *u = cJSON_GetObjectItem(e, "url");
        if (cJSON_IsString(u) && u->valuestring) { snprintf(url, url_size, "%s", u->valuestring); break; }
    }
    if (root) cJSON_Delete(root);
    return url[0] != '\0';
}

/* Put what the request carries into playlist pl_id; returns songs added. */
static int pl_add_payload(int pl_id, const source_request_t *r, char *buf, size_t n)
{
    if (r->queue_op == PL_KIND_URL) {
        return pl_add_url(pl_id, r->radio_action, r->title, buf, n) ? 1 : 0;
    }
    int added = 0;
    char ids[sizeof(r->radio_action)];
    snprintf(ids, sizeof(ids), "%s", r->radio_action);
    char *save = NULL;
    for (char *tok = strtok_r(ids, ",", &save); tok != NULL; tok = strtok_r(NULL, ",", &save)) {
        char url[700];
        const int id = atoi(tok);
        if (id > 0 && pl_track_url(id, url, sizeof(url), buf, n) && pl_add_url(pl_id, url, NULL, buf, n)) {
            added++;
        }
    }
    return added;
}

static void pl_redraw(int pl_id, int offset, const char *name)
{
    source_request_t view = { .mode = SOURCE_PLAYLIST_TRACKS, .id = pl_id, .offset = offset,
                              .push_history = false };
    snprintf(view.title, sizeof(view.title), "%s", name);
    xQueueSend(source_queue, &view, 0);
}

static void pl_rename_done_job(void *arg)
{
    (void)arg;
    qs_hide();
}

static void pl_show_tracks(source_request_t request, char *buf, size_t n)
{
    /* All of the playlist (up to SOURCE_ITEM_CAP songs), a couple of hundred
     * at a time; the view scrolls. */
    char p[128];
    int total = 0;
    source_item_count = 0;
    for (int off = 0; off < SOURCE_ITEM_CAP; off += SOURCE_MAX_ITEMS) {
        snprintf(p, sizeof(p), "\"playlists\",\"tracks\",%d,%d,\"playlist_id:%d\",\"tags:au\"",
                 off, SOURCE_MAX_ITEMS, request.id);
        if (!pl_cmd(p, buf, n)) {
            if (off > 0) break;
            source_show_error(&request, SOURCE_PLAYLIST_TRACKS, "Could not reach the Lyrion server");
            return;
        }
        cJSON *root = cJSON_Parse(buf);
        cJSON *res = root ? cJSON_GetObjectItem(root, "result") : NULL;
        if (!cJSON_IsObject(res)) {
            if (root) cJSON_Delete(root);
            if (off > 0) break;
            source_show_error(&request, SOURCE_PLAYLIST_TRACKS, "Unexpected reply from Lyrion");
            return;
        }
        total = pl_json_int(cJSON_GetObjectItem(res, "count"));
        const int before = source_item_count;
        cJSON *loop = cJSON_GetObjectItem(res, "playlisttracks_loop");
        cJSON *entry = NULL;
        cJSON_ArrayForEach(entry, loop) {
            if (source_item_count >= SOURCE_ITEM_CAP) break;
            cJSON *ix = cJSON_GetObjectItem(entry, "playlist index");
            const int index = ix ? pl_json_int(ix) : source_item_count;
            cJSON *ti = cJSON_GetObjectItem(entry, "title");
            cJSON *ar = cJSON_GetObjectItem(entry, "artist");
            const char *title = (cJSON_IsString(ti) && ti->valuestring) ? ti->valuestring : "Unknown";
            const char *artist = (cJSON_IsString(ar) && ar->valuestring && ar->valuestring[0]) ? ar->valuestring : NULL;
            char text[SOURCE_TEXT_MAX];
            snprintf(text, sizeof(text), "%d. %s%s%s", index + 1, artist ? artist : "", artist ? " - " : "", title);
            source_add_item(index, SOURCE_PL_PLAY_FROM, text);
            if (source_item_count > 0) {
                source_item_t *it = &source_items[source_item_count - 1];
                it->artwork_track_id = pl_json_int(cJSON_GetObjectItem(entry, "id"));
                cJSON *u = cJSON_GetObjectItem(entry, "url");
                snprintf(it->radio_play_json, sizeof(it->radio_play_json), "%s",
                         (cJSON_IsString(u) && u->valuestring) ? u->valuestring : "");
            }
        }
        cJSON_Delete(root);
        if (source_item_count == before || source_item_count >= total) break;
    }
    request.offset = 0;
    source_has_next = false;
    source_queue_total = total;
    source_queue_current = -1;
    if (!source_queue_keep_sel) source_queue_sel = -1;
    source_queue_keep_sel = false;

    source_commit_view(&request, SOURCE_PLAYLIST_TRACKS);
    char title[SOURCE_TEXT_MAX + 32];
    snprintf(title, sizeof(title), "%.60s  (%d SONG%s)", source_folder_title, total, total == 1 ? "" : "S");
    for (char *c = title; *c; c++) *c = (char)toupper((unsigned char)*c);
    source_render_ctx_t ctx = { .title = title };
    ui_call(source_render_job, &ctx);
}

static void playlists_worker(source_request_t request)
{
    char *buf = heap_caps_malloc(SOURCE_BROWSE_BUF, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (buf == NULL) return;
    char p[256], arg[160];

    switch (request.mode) {
        case SOURCE_PLAYLIST_TRACKS:
            pl_show_tracks(request, buf, SOURCE_BROWSE_BUF);
            break;

        case SOURCE_PL_PLAY_FROM: {
            snprintf(p, sizeof(p), "\"playlistcontrol\",\"cmd:load\",\"playlist_id:%d\",\"play_index:%d\"",
                     request.id, request.queue_op);
            const bool ok = pl_cmd(p, buf, SOURCE_BROWSE_BUF);
            ESP_LOGI(TAG, "Play playlist %d from song %d: %s", request.id, request.queue_op + 1, ok ? "ok" : "failed");
            if (!ok) ui_toast_post("Could not reach the Lyrion server");
            vTaskDelay(pdMS_TO_TICKS(200));
            if (lyrion_refresh_event != NULL) xEventGroupSetBits(lyrion_refresh_event, LYRION_REFRESH_BIT);
            if (ok) ui_call(source_hide_job, NULL);
            break;
        }

        case SOURCE_PL_EDIT: {
            int from = 0, to = 0;
            sscanf(request.item_id, "%d,%d", &from, &to);
            if (request.queue_op == PL_EDIT_MOVE) {
                snprintf(p, sizeof(p), "\"playlists\",\"edit\",\"cmd:move\",\"playlist_id:%d\",\"index:%d\",\"toindex:%d\"",
                         request.id, from, to);
                source_queue_sel = to;                    /* keep the moved song selected */
                source_queue_keep_sel = true;
            } else {
                snprintf(p, sizeof(p), "\"playlists\",\"edit\",\"cmd:delete\",\"playlist_id:%d\",\"index:%d\"",
                         request.id, from);
            }
            const bool ok = pl_cmd(p, buf, SOURCE_BROWSE_BUF);
            ESP_LOGI(TAG, "Playlist %d edit (%s %d->%d): %s", request.id,
                     request.queue_op == PL_EDIT_MOVE ? "move" : "delete", from, to, ok ? "ok" : "failed");
            if (!ok) ui_toast_post("Could not reach the Lyrion server");
            vTaskDelay(pdMS_TO_TICKS(150));
            pl_redraw(request.id, request.offset, request.title);
            break;
        }

        case SOURCE_PL_RENAME: {
            /* A dry run first: never replace another playlist by accident. */
            snprintf(p, sizeof(p), "\"playlists\",\"rename\",\"playlist_id:%d\"", request.id);
            json_kv_arg(arg, sizeof(arg), "newname", request.title);
            strncat(p, arg, sizeof(p) - strlen(p) - 1);
            char dry[300];
            snprintf(dry, sizeof(dry), "%s,\"dry_run:1\"", p);
            if (!pl_cmd(dry, buf, SOURCE_BROWSE_BUF)) {
                qs_status_from_task("Could not reach the Lyrion server.", UI_RED, false);
                break;
            }
            cJSON *root = cJSON_Parse(buf);
            cJSON *res = root ? cJSON_GetObjectItem(root, "result") : NULL;
            const int clash = cJSON_IsObject(res) ? pl_json_int(cJSON_GetObjectItem(res, "overwritten_playlist_id")) : 0;
            if (root) cJSON_Delete(root);
            if (clash > 0 && clash != request.id) {
                qs_status_from_task("Another playlist already has that name - choose a different one.", UI_AMBER, false);
                break;
            }
            const bool ok = pl_cmd(p, buf, SOURCE_BROWSE_BUF);
            ESP_LOGI(TAG, "Rename playlist %d to \"%s\": %s", request.id, request.title, ok ? "ok" : "failed");
            if (!ok) {
                qs_status_from_task("Could not reach the Lyrion server.", UI_RED, false);
                break;
            }
            ui_call(pl_rename_done_job, NULL);
            ui_toast_post("Playlist renamed");
            vTaskDelay(pdMS_TO_TICKS(300));
            pl_redraw(request.id, request.offset, request.title);
            break;
        }

        case SOURCE_PL_LIST: {
            if (pl_entries == NULL) {
                pl_entries = heap_caps_calloc(PL_PICK_MAX, sizeof(pl_entry_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            }
            pl_fill_ctx_t c = { .ok = false };
            int count = 0;
            snprintf(p, sizeof(p), "\"playlists\",0,%d", PL_PICK_MAX);
            if (pl_entries != NULL && pl_cmd(p, buf, SOURCE_BROWSE_BUF)) {
                cJSON *root = cJSON_Parse(buf);
                cJSON *res = root ? cJSON_GetObjectItem(root, "result") : NULL;
                cJSON *loop = cJSON_IsObject(res) ? cJSON_GetObjectItem(res, "playlists_loop") : NULL;
                cJSON *e = NULL;
                cJSON_ArrayForEach(e, loop) {
                    if (count >= PL_PICK_MAX) break;
                    cJSON *nm = cJSON_GetObjectItem(e, "playlist");
                    const int id = pl_json_int(cJSON_GetObjectItem(e, "id"));
                    if (id <= 0 || !cJSON_IsString(nm) || nm->valuestring == NULL) continue;
                    pl_entries[count].id = id;
                    snprintf(pl_entries[count].name, sizeof(pl_entries[count].name), "%s", nm->valuestring);
                    count++;
                }
                c.ok = (root != NULL);
                if (root) cJSON_Delete(root);
            }
            pl_entry_count = count;
            ui_call(pl_picker_fill_job, &c);
            break;
        }

        case SOURCE_PL_ADD:
        case SOURCE_PL_NEW_ADD: {
            int pl_id = request.id;
            const bool fresh = (request.mode == SOURCE_PL_NEW_ADD);
            if (fresh) {
                /* "playlists new" answers with the new id, or the id of an
                 * existing playlist with that name (then we add to that one). */
                snprintf(p, sizeof(p), "\"playlists\",\"new\"");
                json_kv_arg(arg, sizeof(arg), "name", request.item_id);
                strncat(p, arg, sizeof(p) - strlen(p) - 1);
                pl_id = 0;
                if (pl_cmd(p, buf, SOURCE_BROWSE_BUF)) {
                    cJSON *root = cJSON_Parse(buf);
                    cJSON *res = root ? cJSON_GetObjectItem(root, "result") : NULL;
                    if (cJSON_IsObject(res)) {
                        pl_id = pl_json_int(cJSON_GetObjectItem(res, "playlist_id"));
                        if (pl_id <= 0) pl_id = pl_json_int(cJSON_GetObjectItem(res, "overwritten_playlist_id"));
                    }
                    if (root) cJSON_Delete(root);
                }
                if (pl_id <= 0) {
                    qs_status_from_task("Lyrion did not create it. Check that a Playlists folder is set "
                                        "in Lyrion's Settings > Basic Settings.", UI_RED, false);
                    break;
                }
            }
            const int added = pl_add_payload(pl_id, &request, buf, SOURCE_BROWSE_BUF);
            ESP_LOGI(TAG, "Add to playlist %d \"%s\": %d of %d", pl_id, request.item_id, added, request.offset);
            char msg[96];
            if (added <= 0) {
                snprintf(msg, sizeof(msg), "Could not add it to %.60s", request.item_id);
            } else if (request.offset > 1) {
                snprintf(msg, sizeof(msg), "Added %d song%s to %.50s", added, added == 1 ? "" : "s", request.item_id);
            } else {
                snprintf(msg, sizeof(msg), "Added to %.60s", request.item_id);
            }
            if (fresh) qs_status_from_task(msg, added > 0 ? UI_ACCENT_GLOW : UI_RED, added > 0);
            ui_toast_post(msg);
            break;
        }

        default:
            break;
    }
    heap_caps_free(buf);
}
