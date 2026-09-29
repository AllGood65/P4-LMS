/*
 * P4-LMS - Alarms (Settings > Alarms)
 *
 * Part of main.c.  This file is #included by main.c (in order) and is not
 * compiled on its own, so it shares main.c's includes and static names.
 *
 * The alarms themselves live in Lyrion ("<player> alarm add|update|delete",
 * "<player> alarms 0 n"), so they ring even when this display is off.  This
 * file is only a remote control for them, for the room that was selected
 * when the Alarms screen opened.
 *
 * Optional "Amp on for alarms": Lyrion starts the music but can't power the
 * amplifier, which Home Assistant switches.  With this on, the command task
 * (alarm_amp_tick) turns the room's amp and pre-amp on about a minute before
 * each enabled alarm - which does need this display to be running.
 *
 * Network work runs on the SOURCE browser task (source_queue); results come
 * back to the LVGL task through ui_call().
 */

#define ALARM_MAX            12
#define ALARM_URL_MAX        256
#define ALARM_NVS_AMP        "alarm_amp"
#define ALARM_ROW_H          96

typedef enum { ALARM_PLAY_QUEUE = 0, ALARM_PLAY_PLAYLIST, ALARM_PLAY_RANDOM } alarm_play_t;

typedef struct {
    char    id[24];                 /* Lyrion's id, "" for a new alarm   */
    int     time_s;                 /* seconds after midnight            */
    uint8_t dow;                    /* bit 0 = Sunday ... bit 6 = Saturday */
    bool    enabled;
    bool    repeat;
    int     volume;                 /* 0-100, -1 = Lyrion's default      */
    char    url[ALARM_URL_MAX];     /* "0" = current queue               */
} alarm_t;

static alarm_t alarm_list[ALARM_MAX];
static int     alarm_count = 0;
static int     alarms_room = 0;          /* room the screen is showing   */
static alarm_t alarm_edit;               /* the one open in the editor   */
static bool    alarm_edit_is_new = false;
static alarm_t alarm_pending;            /* handed to the browser task   */
static volatile bool alarm_amp_enabled = false;
static volatile bool alarm_sched_dirty = true;   /* re-read before the next amp check */

/* ---- playlists for the chooser (browser task fills, LVGL task shows) ---- */
#define ALARM_PL_MAX 60
typedef struct { char name[64]; char url[ALARM_URL_MAX]; } alarm_playlist_t;
static alarm_playlist_t *alarm_playlists = NULL;   /* PSRAM, ALARM_PL_MAX entries */
static int alarm_playlist_count = 0;

/* ---- widgets ---- */
static lv_obj_t *alarms_overlay = NULL, *alarms_list = NULL, *alarms_status = NULL, *alarms_amp_sw = NULL;
static lv_obj_t *aed_overlay = NULL, *aed_hour = NULL, *aed_min = NULL, *aed_am = NULL, *aed_pm = NULL;
static lv_obj_t *aed_days[7], *aed_repeat = NULL, *aed_vol = NULL, *aed_vol_lbl = NULL;
static lv_obj_t *aed_play_btn[3], *aed_pl_field = NULL, *aed_pl_name = NULL, *aed_delete = NULL;
static lv_obj_t *aed_msg = NULL;
static lv_obj_t *apl_overlay = NULL, *apl_panel = NULL, *apl_list = NULL;

static void alarms_show(void);
static void alarms_hide(void);

/* ===================================================================== text */
static void alarm_time_text(int time_s, char *out, size_t n)
{
    const int h = (time_s / 3600) % 24, m = (time_s / 60) % 60;
    snprintf(out, n, "%d:%02d %s", (h % 12) ? (h % 12) : 12, m, h < 12 ? "AM" : "PM");
}

static void alarm_days_text(uint8_t dow, char *out, size_t n)
{
    static const char *const names[7] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
    if (dow == 0x7F)      { snprintf(out, n, "Every day"); return; }
    if (dow == 0x3E)      { snprintf(out, n, "Weekdays");  return; }
    if (dow == 0x41)      { snprintf(out, n, "Weekends");  return; }
    if (dow == 0)         { snprintf(out, n, "No days");   return; }
    out[0] = '\0';
    for (int k = 1; k <= 7; k++) {               /* Monday first, Sunday last */
        const int d = k % 7;
        if (!(dow & (1 << d))) continue;
        if (out[0]) strncat(out, " ", n - strlen(out) - 1);
        strncat(out, names[d], n - strlen(out) - 1);
    }
}

static alarm_play_t alarm_play_kind(const char *url)
{
    if (url[0] == '\0' || strcmp(url, "0") == 0) return ALARM_PLAY_QUEUE;
    if (strncmp(url, "randomplay:", 11) == 0)     return ALARM_PLAY_RANDOM;
    return ALARM_PLAY_PLAYLIST;
}

/* "file:///playlists/Morning%20Mix.m3u" -> "Morning Mix" */
static void alarm_url_name(const char *url, char *out, size_t n)
{
    const alarm_play_t k = alarm_play_kind(url);
    if (k == ALARM_PLAY_QUEUE)  { snprintf(out, n, "Current queue"); return; }
    if (k == ALARM_PLAY_RANDOM) { snprintf(out, n, "Random songs");  return; }
    /* A playlist we know by URL: use its real name. */
    for (int i = 0; alarm_playlists != NULL && i < alarm_playlist_count; i++) {
        if (strcmp(alarm_playlists[i].url, url) == 0) {
            snprintf(out, n, "%s", alarm_playlists[i].name);
            return;
        }
    }
    const char *base = strrchr(url, '/');
    base = base ? base + 1 : url;
    size_t o = 0;
    for (size_t i = 0; base[i] && o + 1 < n; i++) {
        if (base[i] == '%' && isxdigit((unsigned char)base[i + 1]) && isxdigit((unsigned char)base[i + 2])) {
            char hex[3] = { base[i + 1], base[i + 2], 0 };
            out[o++] = (char)strtol(hex, NULL, 16);
            i += 2;
        } else {
            out[o++] = base[i];
        }
    }
    out[o] = '\0';
    char *dot = strrchr(out, '.');
    if (dot != NULL && (strcasecmp(dot, ".m3u") == 0 || strcasecmp(dot, ".m3u8") == 0)) *dot = '\0';
}

/* ================================================================ parsing */
static int alarm_json_int(cJSON *j, int fallback)
{
    if (cJSON_IsNumber(j)) return j->valueint;
    if (cJSON_IsString(j) && j->valuestring && j->valuestring[0]) return atoi(j->valuestring);
    return fallback;
}

/* Parse an "alarms" reply into out[]; returns the count. */
static int alarms_parse(const char *json, alarm_t *out, int max)
{
    cJSON *root = cJSON_Parse(json);
    cJSON *res = root ? cJSON_GetObjectItem(root, "result") : NULL;
    cJSON *loop = cJSON_IsObject(res) ? cJSON_GetObjectItem(res, "alarms_loop") : NULL;
    int n = 0;
    cJSON *a = NULL;
    cJSON_ArrayForEach(a, loop) {
        if (n >= max) break;
        alarm_t *x = &out[n];
        memset(x, 0, sizeof(*x));
        cJSON *id = cJSON_GetObjectItem(a, "id");
        if (cJSON_IsString(id) && id->valuestring) snprintf(x->id, sizeof(x->id), "%s", id->valuestring);
        else if (cJSON_IsNumber(id)) snprintf(x->id, sizeof(x->id), "%d", id->valueint);
        if (!x->id[0]) continue;
        x->time_s = alarm_json_int(cJSON_GetObjectItem(a, "time"), 0);
        x->enabled = alarm_json_int(cJSON_GetObjectItem(a, "enabled"), 0) != 0;
        x->repeat = alarm_json_int(cJSON_GetObjectItem(a, "repeat"), 1) != 0;
        x->volume = alarm_json_int(cJSON_GetObjectItem(a, "volume"), -1);
        cJSON *dow = cJSON_GetObjectItem(a, "dow");
        if (cJSON_IsString(dow) && dow->valuestring) {
            for (const char *p = dow->valuestring; *p; p++) {
                if (*p >= '0' && *p <= '6') x->dow |= (uint8_t)(1 << (*p - '0'));
            }
        } else if (cJSON_IsNumber(dow) && dow->valueint >= 0 && dow->valueint <= 6) {
            x->dow = (uint8_t)(1 << dow->valueint);
        }
        cJSON *url = cJSON_GetObjectItem(a, "url");
        snprintf(x->url, sizeof(x->url), "%s", (cJSON_IsString(url) && url->valuestring) ? url->valuestring : "0");
        n++;
    }
    if (root) cJSON_Delete(root);
    return n;
}

/* A JSON string item: "key:value" with quotes and backslashes escaped. */
static void alarm_json_arg(char *out, size_t n, const char *key, const char *value)
{
    size_t o = (size_t)snprintf(out, n, ",\"%s:", key);
    for (const char *p = value; *p && o + 3 < n; p++) {
        if (*p == '"' || *p == '\\') out[o++] = '\\';
        out[o++] = *p;
    }
    if (o + 2 < n) { out[o++] = '"'; }
    out[o] = '\0';
}

/* ======================================================= browser-task work */
typedef struct { const char *msg; bool error; } alarms_render_ctx_t;
static void alarms_render_job(void *arg);
static void alarm_playlists_render_job(void *arg);

static bool alarm_send(int room, const char *params_json, char *buf, size_t buf_size)
{
    char *json = heap_caps_malloc(1024, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (json == NULL) return false;
    snprintf(json, 1024, "{\"id\":1,\"method\":\"slim.request\",\"params\":[\"%s\",[%s]]}",
             lyrion_players[room].id, params_json);
    const bool ok = source_http_query(room, json, buf, buf_size);
    heap_caps_free(json);
    return ok;
}

static void alarms_do_load(int room, const char *done_msg)
{
    char *buf = heap_caps_malloc(16384, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    alarms_render_ctx_t ctx = { .msg = done_msg, .error = false };
    if (buf == NULL || !alarm_send(room, "\"alarms\",0,20,\"filter:all\"", buf, 16384)) {
        ctx.msg = "Could not reach the Lyrion server.";
        ctx.error = true;
        alarm_count = 0;
    } else {
        alarm_count = alarms_parse(buf, alarm_list, ALARM_MAX);
        /* Earlier versions saved random songs as "randomplay:track", which
         * Lyrion doesn't recognise (the alarm came up but played nothing).
         * The Random Mix plugin's URL is "randomplay://track": fix any old
         * ones on the server as they're read. */
        for (int i = 0; i < alarm_count; i++) {
            alarm_t *a = &alarm_list[i];
            if (strncmp(a->url, "randomplay:", 11) != 0 || strncmp(a->url, "randomplay://", 13) == 0) continue;
            char fixed[ALARM_URL_MAX], p[ALARM_URL_MAX + 96], arg[ALARM_URL_MAX + 16];
            snprintf(fixed, sizeof(fixed), "randomplay://%s", a->url + 11);
            snprintf(p, sizeof(p), "\"alarm\",\"update\"");
            alarm_json_arg(arg, sizeof(arg), "id", a->id);  strncat(p, arg, sizeof(p) - strlen(p) - 1);
            alarm_json_arg(arg, sizeof(arg), "url", fixed); strncat(p, arg, sizeof(p) - strlen(p) - 1);
            if (alarm_send(room, p, buf, 16384)) {
                ESP_LOGI(TAG, "Alarm %s: random-songs address fixed (%s -> %s)", a->id, a->url, fixed);
                snprintf(a->url, sizeof(a->url), "%s", fixed);
            }
        }
    }
    if (buf) heap_caps_free(buf);
    ui_call(alarms_render_job, &ctx);
}

/* Alarms only ring on a player whose "alarmsEnabled" preference is on. */
static void alarms_allow(int room, char *buf, size_t n)
{
    (void)alarm_send(room, "\"playerpref\",\"alarmsEnabled\",\"1\"", buf, n);
}

static void alarms_worker(source_request_t request)
{
    const int room = alarms_room;
    if (room < 0 || room >= lyrion_player_count) return;
    char reply[LYRION_RESPONSE_MAX];
    char p[768];
    const char *done = NULL;

    switch (request.mode) {
        case SOURCE_ALARMS_LOAD:
            alarms_do_load(room, NULL);
            return;
        case SOURCE_ALARM_SAVE: {
            const alarm_t *a = &alarm_pending;
            char days[16] = "", num[16], arg[ALARM_URL_MAX + 16];
            for (int d = 0; d < 7; d++) {
                if (!(a->dow & (1 << d))) continue;
                char one[3];
                snprintf(one, sizeof(one), "%s%d", days[0] ? "," : "", d);
                strncat(days, one, sizeof(days) - strlen(days) - 1);
            }
            snprintf(p, sizeof(p), "\"alarm\",\"%s\"", a->id[0] ? "update" : "add");
            if (a->id[0]) { alarm_json_arg(arg, sizeof(arg), "id", a->id); strncat(p, arg, sizeof(p) - strlen(p) - 1); }
            snprintf(num, sizeof(num), "%d", a->time_s);
            alarm_json_arg(arg, sizeof(arg), "time", num);       strncat(p, arg, sizeof(p) - strlen(p) - 1);
            alarm_json_arg(arg, sizeof(arg), "dow", days[0] ? days : "0,1,2,3,4,5,6");
            strncat(p, arg, sizeof(p) - strlen(p) - 1);
            alarm_json_arg(arg, sizeof(arg), "enabled", a->enabled ? "1" : "0"); strncat(p, arg, sizeof(p) - strlen(p) - 1);
            alarm_json_arg(arg, sizeof(arg), "repeat", a->repeat ? "1" : "0");   strncat(p, arg, sizeof(p) - strlen(p) - 1);
            if (a->volume >= 0) {
                snprintf(num, sizeof(num), "%d", a->volume);
                alarm_json_arg(arg, sizeof(arg), "volume", num); strncat(p, arg, sizeof(p) - strlen(p) - 1);
            }
            alarm_json_arg(arg, sizeof(arg), "url", a->url[0] ? a->url : "0");
            strncat(p, arg, sizeof(p) - strlen(p) - 1);
            const bool ok = alarm_send(room, p, reply, sizeof(reply));
            if (ok && a->enabled) alarms_allow(room, reply, sizeof(reply));
            ESP_LOGI(TAG, "Alarm %s on %s: %s", a->id[0] ? "update" : "add", lyrion_players[room].name,
                     ok ? "ok" : "failed");
            done = ok ? "Alarm saved." : "Could not save - the Lyrion server didn't answer.";
            break;
        }
        case SOURCE_ALARM_DELETE: {
            char arg[64];
            snprintf(p, sizeof(p), "\"alarm\",\"delete\"");
            alarm_json_arg(arg, sizeof(arg), "id", request.item_id);
            strncat(p, arg, sizeof(p) - strlen(p) - 1);
            done = alarm_send(room, p, reply, sizeof(reply)) ? "Alarm deleted." : "Could not delete it - try again.";
            break;
        }
        case SOURCE_ALARM_TOGGLE: {
            char arg[64];
            snprintf(p, sizeof(p), "\"alarm\",\"update\"");
            alarm_json_arg(arg, sizeof(arg), "id", request.item_id);   strncat(p, arg, sizeof(p) - strlen(p) - 1);
            alarm_json_arg(arg, sizeof(arg), "enabled", request.id ? "1" : "0");
            strncat(p, arg, sizeof(p) - strlen(p) - 1);
            const bool ok = alarm_send(room, p, reply, sizeof(reply));
            if (ok && request.id) alarms_allow(room, reply, sizeof(reply));
            done = ok ? (request.id ? "Alarm on." : "Alarm off.") : "Could not reach the Lyrion server.";
            break;
        }
        case SOURCE_ALARM_PLAYLISTS: {
            char *buf = heap_caps_malloc(32768, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (alarm_playlists == NULL) {
                alarm_playlists = heap_caps_calloc(ALARM_PL_MAX, sizeof(alarm_playlist_t),
                                                   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            }
            int n = 0;
            if (buf != NULL && alarm_playlists != NULL &&
                alarm_send(room, "\"playlists\",0,60,\"tags:u\"", buf, 32768)) {
                cJSON *root = cJSON_Parse(buf);
                cJSON *res = root ? cJSON_GetObjectItem(root, "result") : NULL;
                cJSON *loop = cJSON_IsObject(res) ? cJSON_GetObjectItem(res, "playlists_loop") : NULL;
                cJSON *e = NULL;
                cJSON_ArrayForEach(e, loop) {
                    if (n >= ALARM_PL_MAX) break;
                    cJSON *nm = cJSON_GetObjectItem(e, "playlist");
                    cJSON *u = cJSON_GetObjectItem(e, "url");
                    if (!cJSON_IsString(nm) || !nm->valuestring || !cJSON_IsString(u) || !u->valuestring) continue;
                    snprintf(alarm_playlists[n].name, sizeof(alarm_playlists[n].name), "%s", nm->valuestring);
                    snprintf(alarm_playlists[n].url, sizeof(alarm_playlists[n].url), "%s", u->valuestring);
                    n++;
                }
                if (root) cJSON_Delete(root);
            }
            if (buf) heap_caps_free(buf);
            alarm_playlist_count = n;
            ui_call(alarm_playlists_render_job, NULL);
            return;
        }
        default:
            return;
    }
    alarm_sched_dirty = true;                /* the amp check re-reads the alarms */
    alarms_do_load(room, done);
}

/* ======================================================== the alarm list */
static void alarm_send_request(source_mode_t mode, int id_value, const char *alarm_id)
{
    if (source_queue == NULL) return;
    source_request_t r = { .mode = mode, .id = id_value, .push_history = false };
    snprintf(r.item_id, sizeof(r.item_id), "%s", alarm_id ? alarm_id : "");
    xQueueSend(source_queue, &r, 0);
}

static void alarm_editor_open(int index);

static void alarm_row_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) alarm_editor_open((int)(intptr_t)lv_event_get_user_data(e));
}

static void alarm_row_switch_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    const int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i < 0 || i >= alarm_count) return;
    const bool on = lv_obj_has_state((lv_obj_t *)lv_event_get_target(e), LV_STATE_CHECKED);
    alarm_list[i].enabled = on;
    alarm_send_request(SOURCE_ALARM_TOGGLE, on ? 1 : 0, alarm_list[i].id);
}

static lv_obj_t *alarm_switch(lv_obj_t *parent)
{
    lv_obj_t *sw = lv_switch_create(parent);
    lv_obj_set_size(sw, 64, 34);
    lv_obj_set_style_bg_color(sw, lv_color_hex(UI_TRACK), LV_PART_MAIN);
    lv_obj_set_style_bg_color(sw, lv_color_hex(UI_ACCENT), LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(sw, lv_color_hex(UI_ACCENT_BRIGHT), LV_PART_KNOB);
    lv_obj_set_ext_click_area(sw, 10);
    return sw;
}

static lv_obj_t *alarm_label(lv_obj_t *parent, const char *text, const lv_font_t *font, uint32_t color,
                             int x, int y, int w)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    if (w > 0) {
        lv_obj_set_width(l, w);
        lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
    }
    lv_obj_set_pos(l, x, y);
    return l;
}

static void alarms_render_job(void *arg)
{
    const alarms_render_ctx_t *c = (const alarms_render_ctx_t *)arg;
    if (alarms_list == NULL) return;
    lv_obj_clean(alarms_list);
    for (int i = 0; i < alarm_count; i++) {
        const alarm_t *a = &alarm_list[i];
        lv_obj_t *row = lv_obj_create(alarms_list);
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, 850, ALARM_ROW_H - 8);
        lv_obj_set_pos(row, 0, i * ALARM_ROW_H);
        lv_obj_set_style_bg_color(row, lv_color_hex(UI_BUTTON), 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(row, 16, 0);
        lv_obj_set_style_border_width(row, 1, 0);
        lv_obj_set_style_border_color(row, lv_color_hex(UI_EDGE), 0);
        lv_obj_set_style_bg_color(row, lv_color_hex(UI_ACCENT_DARK), LV_STATE_PRESSED);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(row, alarm_row_event_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);

        char t[96], w[96], v[140];
        alarm_time_text(a->time_s, t, sizeof(t));
        alarm_label(row, t, &p4_font_28, a->enabled ? UI_TEXT : UI_TEXT_DIM, 22, 10, 0);
        alarm_days_text(a->dow, t, sizeof(t));
        if (!a->repeat) strncat(t, "  (once)", sizeof(t) - strlen(t) - 1);
        alarm_label(row, t, &p4_font_16, UI_TEXT_SOFT, 22, 54, 250);
        alarm_url_name(a->url, w, sizeof(w));
        if (a->volume >= 0) snprintf(v, sizeof(v), "%.90s  " LV_SYMBOL_BULLET "  volume %d", w, a->volume);
        else                snprintf(v, sizeof(v), "%.90s", w);
        alarm_label(row, v, &p4_font_16, UI_TEXT_SOFT, 290, 16, 440);
        alarm_label(row, "Tap to edit", &p4_font_14, UI_TEXT_DIM, 290, 54, 0);

        lv_obj_t *sw = alarm_switch(row);
        lv_obj_align(sw, LV_ALIGN_RIGHT_MID, -20, 0);
        if (a->enabled) lv_obj_add_state(sw, LV_STATE_CHECKED);
        lv_obj_add_event_cb(sw, alarm_row_switch_event_cb, LV_EVENT_VALUE_CHANGED, (void *)(intptr_t)i);
    }
    if (alarms_status != NULL) {
        const char *msg = c ? c->msg : NULL;
        if (msg == NULL && alarm_count == 0) msg = "No alarms yet - tap + NEW ALARM.";
        lv_label_set_text(alarms_status, msg ? msg : "");
        lv_obj_set_style_text_color(alarms_status, lv_color_hex(c && c->error ? UI_RED : UI_TEXT_SOFT), 0);
    }
}

static void alarms_new_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) alarm_editor_open(-1);
}

static void alarms_close_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) alarms_hide();
}

static void alarm_amp_save(bool on)
{
    nvs_handle_t nvs;
    if (nvs_open(PLAYER_NVS_NAMESPACE, NVS_READWRITE, &nvs) != ESP_OK) return;
    nvs_set_u8(nvs, ALARM_NVS_AMP, on ? 1 : 0);
    nvs_commit(nvs);
    nvs_close(nvs);
}

static void alarms_amp_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    const bool on = lv_obj_has_state(alarms_amp_sw, LV_STATE_CHECKED);
    alarm_amp_enabled = on;
    alarm_sched_dirty = true;
    alarm_amp_save(on);
    ui_toast(on ? (HA_TOKEN_SET ? "Amps will switch on for alarms"
                                : "Amps will switch on for alarms - set the Home Assistant token first")
                : "Amps won't be switched for alarms");
}

static void alarms_create(lv_obj_t *screen)
{
    alarms_overlay = nav_panel_create(screen, "ALARMS", alarms_close_event_cb);
    ui_add_settings_back(alarms_overlay);

    lv_obj_t *add = create_button(alarms_overlay, LV_SYMBOL_PLUS "  NEW ALARM", 200, 50);
    lv_obj_set_pos(add, 30, 80);
    lv_obj_set_style_bg_color(add, lv_color_hex(UI_ACCENT), LV_PART_MAIN);
    lv_obj_set_style_text_color(lv_obj_get_child(add, 0), lv_color_hex(UI_ON_ACCENT), 0);
    lv_obj_add_event_cb(add, alarms_new_event_cb, LV_EVENT_CLICKED, NULL);
    alarms_status = alarm_label(alarms_overlay, "", &p4_font_14, UI_TEXT_SOFT, 250, 96, 640);

    alarms_list = lv_obj_create(alarms_overlay);
    lv_obj_remove_style_all(alarms_list);
    lv_obj_set_size(alarms_list, 870, PANEL_H - 144 - 76);
    lv_obj_set_pos(alarms_list, 30, 144);
    lv_obj_add_flag(alarms_list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(alarms_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(alarms_list, LV_SCROLLBAR_MODE_AUTO);

    lv_obj_t *amp = lv_obj_create(alarms_overlay);
    lv_obj_remove_style_all(amp);
    lv_obj_set_size(amp, 870, 54);
    lv_obj_set_pos(amp, 30, PANEL_H - 68);
    lv_obj_set_style_radius(amp, 12, 0);
    lv_obj_set_style_border_width(amp, 1, 0);
    lv_obj_set_style_border_color(amp, lv_color_hex(UI_EDGE), 0);
    lv_obj_clear_flag(amp, LV_OBJ_FLAG_SCROLLABLE);
    alarm_label(amp, "Amp on for alarms", &p4_font_16, UI_TEXT, 20, 16, 0);
    alarm_label(amp, "switches the room's amp on a minute before (needs this display running)",
                &p4_font_14, UI_TEXT_DIM, 200, 18, 560);
    alarms_amp_sw = alarm_switch(amp);
    lv_obj_align(alarms_amp_sw, LV_ALIGN_RIGHT_MID, -16, 0);
    if (alarm_amp_enabled) lv_obj_add_state(alarms_amp_sw, LV_STATE_CHECKED);
    lv_obj_add_event_cb(alarms_amp_sw, alarms_amp_event_cb, LV_EVENT_VALUE_CHANGED, NULL);
}

static void alarms_show(void)
{
    if (alarms_overlay == NULL) return;
    if (current_player_index < 0 || current_player_index >= lyrion_player_count) {
        ui_toast("No room selected");
        return;
    }
    alarms_room = current_player_index;
    if (alarm_amp_enabled) lv_obj_add_state(alarms_amp_sw, LV_STATE_CHECKED);   /* may have changed on the web page */
    else                   lv_obj_clear_state(alarms_amp_sw, LV_STATE_CHECKED);
    char t[64];
    snprintf(t, sizeof(t), "ALARMS  -  %s", lyrion_players[alarms_room].short_name);
    for (char *p = t; *p; p++) *p = (char)toupper((unsigned char)*p);
    lv_label_set_text(lv_obj_get_child(alarms_overlay, 0), t);
    lv_obj_clean(alarms_list);
    lv_label_set_text(alarms_status, "Loading alarms...");
    lv_obj_set_style_text_color(alarms_status, lv_color_hex(UI_TEXT_SOFT), 0);
    ui_show_panel(alarms_overlay);
    alarm_send_request(SOURCE_ALARMS_LOAD, 0, NULL);
    /* Playlist names for the list (and the editor's chooser). */
    alarm_send_request(SOURCE_ALARM_PLAYLISTS, 0, NULL);
}

static void alarms_hide(void)
{
    if (alarms_overlay != NULL) lv_obj_add_flag(alarms_overlay, LV_OBJ_FLAG_HIDDEN);
    if (aed_overlay != NULL)    lv_obj_add_flag(aed_overlay, LV_OBJ_FLAG_HIDDEN);
    if (apl_overlay != NULL)    lv_obj_add_flag(apl_overlay, LV_OBJ_FLAG_HIDDEN);
}

/* ============================================================ the editor */
static void aed_refresh(void)
{
    const int h = (alarm_edit.time_s / 3600) % 24, m = (alarm_edit.time_s / 60) % 60;
    char t[16];
    snprintf(t, sizeof(t), "%d", (h % 12) ? (h % 12) : 12);
    lv_label_set_text(aed_hour, t);
    snprintf(t, sizeof(t), "%02d", m);
    lv_label_set_text(aed_min, t);
    const bool pm = h >= 12;
    lv_obj_set_style_bg_color(aed_am, lv_color_hex(pm ? UI_BUTTON : UI_ACCENT), LV_PART_MAIN);
    lv_obj_set_style_text_color(lv_obj_get_child(aed_am, 0), lv_color_hex(pm ? UI_TEXT : UI_ON_ACCENT), 0);
    lv_obj_set_style_bg_color(aed_pm, lv_color_hex(pm ? UI_ACCENT : UI_BUTTON), LV_PART_MAIN);
    lv_obj_set_style_text_color(lv_obj_get_child(aed_pm, 0), lv_color_hex(pm ? UI_ON_ACCENT : UI_TEXT), 0);
    for (int d = 0; d < 7; d++) {
        const bool on = (alarm_edit.dow & (1 << d)) != 0;
        lv_obj_set_style_bg_color(aed_days[d], lv_color_hex(on ? UI_ACCENT : UI_BUTTON), 0);
        lv_obj_set_style_text_color(lv_obj_get_child(aed_days[d], 0), lv_color_hex(on ? UI_ON_ACCENT : UI_TEXT_SOFT), 0);
    }
    if (alarm_edit.repeat) lv_obj_add_state(aed_repeat, LV_STATE_CHECKED);
    else                   lv_obj_clear_state(aed_repeat, LV_STATE_CHECKED);
    const int vol = alarm_edit.volume < 0 ? 30 : alarm_edit.volume;
    lv_slider_set_value(aed_vol, vol, LV_ANIM_OFF);
    snprintf(t, sizeof(t), "%d%%", vol);
    lv_label_set_text(aed_vol_lbl, t);
    const alarm_play_t k = alarm_play_kind(alarm_edit.url);
    for (int i = 0; i < 3; i++) {
        const bool on = ((int)k == i);
        lv_obj_set_style_bg_color(aed_play_btn[i], lv_color_hex(on ? UI_ACCENT : UI_BUTTON), LV_PART_MAIN);
        lv_obj_set_style_text_color(lv_obj_get_child(aed_play_btn[i], 0), lv_color_hex(on ? UI_ON_ACCENT : UI_TEXT), 0);
    }
    if (k == ALARM_PLAY_PLAYLIST) {
        char name[96];
        alarm_url_name(alarm_edit.url, name, sizeof(name));
        lv_label_set_text(aed_pl_name, alarm_edit.url[0] && strcmp(alarm_edit.url, "file:") != 0
                                        ? name : "Choose a playlist");
        lv_obj_clear_flag(aed_pl_field, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(aed_pl_field, LV_OBJ_FLAG_HIDDEN);
    }
}

#define AED_HOUR_UP 0
#define AED_HOUR_DN 1
#define AED_MIN_UP  2
#define AED_MIN_DN  3
static void aed_step_event_cb(lv_event_t *e)
{
    const lv_event_code_t code = lv_event_get_code(e);
    if (code != LV_EVENT_SHORT_CLICKED && code != LV_EVENT_LONG_PRESSED_REPEAT) return;
    const int which = (int)(intptr_t)lv_event_get_user_data(e);
    int t = alarm_edit.time_s / 60;                       /* minutes after midnight */
    switch (which) {
        case AED_HOUR_UP: t += 60; break;
        case AED_HOUR_DN: t -= 60; break;
        case AED_MIN_UP:  t += 1;  break;
        default:          t -= 1;  break;
    }
    t = ((t % 1440) + 1440) % 1440;
    alarm_edit.time_s = t * 60;
    aed_refresh();
}

static void aed_ampm_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    const bool want_pm = (int)(intptr_t)lv_event_get_user_data(e) != 0;
    const bool is_pm = (alarm_edit.time_s / 3600) % 24 >= 12;
    if (want_pm != is_pm) alarm_edit.time_s = (alarm_edit.time_s + (want_pm ? 12 : -12) * 3600 + 86400) % 86400;
    aed_refresh();
}

static void aed_day_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    alarm_edit.dow ^= (uint8_t)(1 << (int)(intptr_t)lv_event_get_user_data(e));
    aed_refresh();
}

static void aed_repeat_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    alarm_edit.repeat = lv_obj_has_state(aed_repeat, LV_STATE_CHECKED);
}

static void aed_vol_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    alarm_edit.volume = lv_slider_get_value(aed_vol);
    char t[16];
    snprintf(t, sizeof(t), "%d%%", alarm_edit.volume);
    lv_label_set_text(aed_vol_lbl, t);
}

static void alarm_playlist_picker_open(void);

static void aed_play_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    const int k = (int)(intptr_t)lv_event_get_user_data(e);
    if (k == ALARM_PLAY_QUEUE) {
        snprintf(alarm_edit.url, sizeof(alarm_edit.url), "0");
    } else if (k == ALARM_PLAY_RANDOM) {
        snprintf(alarm_edit.url, sizeof(alarm_edit.url), "randomplay://track");   /* Random Mix plugin's alarm URL */
    } else if (alarm_play_kind(alarm_edit.url) != ALARM_PLAY_PLAYLIST) {
        snprintf(alarm_edit.url, sizeof(alarm_edit.url), "file:");      /* marks "not chosen yet" */
        aed_refresh();
        alarm_playlist_picker_open();
        return;
    }
    aed_refresh();
}

static void aed_choose_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) alarm_playlist_picker_open();
}

static void aed_set_msg(const char *msg)
{
    lv_label_set_text(aed_msg, msg);
}

static void aed_save_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (alarm_edit.dow == 0) {
        aed_set_msg("Pick at least one day.");
        return;
    }
    if (alarm_play_kind(alarm_edit.url) == ALARM_PLAY_PLAYLIST && strcmp(alarm_edit.url, "file:") == 0) {
        aed_set_msg("Choose a playlist, or pick Current queue or Random songs.");
        return;
    }
    if (alarm_edit.volume < 0) alarm_edit.volume = lv_slider_get_value(aed_vol);
    alarm_pending = alarm_edit;
    alarm_send_request(SOURCE_ALARM_SAVE, 0, alarm_edit.id);
    lv_obj_add_flag(aed_overlay, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(alarms_status, "Saving...");
}

/* DELETE asks for a second tap within 3 s. */
static uint32_t aed_delete_armed = 0;
static void aed_delete_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED || alarm_edit_is_new) return;
    if (aed_delete_armed == 0 || lv_tick_elaps(aed_delete_armed) > 3000) {
        aed_delete_armed = lv_tick_get();
        lv_label_set_text(lv_obj_get_child(aed_delete, 0), "SURE?");
        aed_set_msg("Tap SURE? to delete this alarm.");
        return;
    }
    aed_delete_armed = 0;
    alarm_send_request(SOURCE_ALARM_DELETE, 0, alarm_edit.id);
    lv_obj_add_flag(aed_overlay, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(alarms_status, "Deleting...");
}

static void aed_close_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) lv_obj_add_flag(aed_overlay, LV_OBJ_FLAG_HIDDEN);
}

static lv_obj_t *aed_button(lv_obj_t *parent, const char *text, int x, int y, int w, int h,
                            lv_event_cb_t cb, int user, bool repeat)
{
    lv_obj_t *b = create_button(parent, text, w, h);
    lv_obj_set_pos(b, x, y);
    if (repeat) {
        lv_obj_add_event_cb(b, cb, LV_EVENT_SHORT_CLICKED, (void *)(intptr_t)user);
        lv_obj_add_event_cb(b, cb, LV_EVENT_LONG_PRESSED_REPEAT, (void *)(intptr_t)user);
    } else {
        lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, (void *)(intptr_t)user);
    }
    return b;
}

static void alarm_editor_create(lv_obj_t *screen)
{
    aed_overlay = nav_panel_create(screen, "EDIT ALARM", aed_close_event_cb);
    lv_obj_t *p = aed_overlay;
    lv_obj_t *save = create_button(p, "SAVE", 150, 50);
    lv_obj_align(save, LV_ALIGN_TOP_RIGHT, -196, 14);
    lv_obj_set_style_bg_color(save, lv_color_hex(UI_ACCENT), LV_PART_MAIN);
    lv_obj_set_style_text_color(lv_obj_get_child(save, 0), lv_color_hex(UI_ON_ACCENT), 0);
    lv_obj_add_event_cb(save, aed_save_event_cb, LV_EVENT_CLICKED, NULL);
    aed_delete = create_button(p, "DELETE", 130, 50);
    lv_obj_align(aed_delete, LV_ALIGN_TOP_RIGHT, -362, 14);
    lv_obj_set_style_bg_color(aed_delete, lv_color_hex(UI_RED), LV_PART_MAIN);
    lv_obj_set_style_text_color(lv_obj_get_child(aed_delete, 0), lv_color_hex(UI_ON_ACCENT), 0);
    lv_obj_add_event_cb(aed_delete, aed_delete_event_cb, LV_EVENT_CLICKED, NULL);

    /* time */
    aed_button(p, LV_SYMBOL_UP,   40, 84,  130, 44, aed_step_event_cb, AED_HOUR_UP, true);
    aed_button(p, LV_SYMBOL_DOWN, 40, 214, 130, 44, aed_step_event_cb, AED_HOUR_DN, true);
    aed_button(p, LV_SYMBOL_UP,   210, 84,  130, 44, aed_step_event_cb, AED_MIN_UP, true);
    aed_button(p, LV_SYMBOL_DOWN, 210, 214, 130, 44, aed_step_event_cb, AED_MIN_DN, true);
    aed_hour = alarm_label(p, "6", &p4_font_48, UI_TEXT, 40, 140, 130);
    lv_obj_set_style_text_align(aed_hour, LV_TEXT_ALIGN_CENTER, 0);
    alarm_label(p, ":", &p4_font_48, UI_TEXT, 182, 138, 0);
    aed_min = alarm_label(p, "00", &p4_font_48, UI_TEXT, 210, 140, 130);
    lv_obj_set_style_text_align(aed_min, LV_TEXT_ALIGN_CENTER, 0);
    aed_am = aed_button(p, "AM", 364, 112, 80, 50, aed_ampm_event_cb, 0, false);
    aed_pm = aed_button(p, "PM", 364, 172, 80, 50, aed_ampm_event_cb, 1, false);

    /* days, Sunday first as in Lyrion */
    alarm_label(p, "DAYS", &p4_font_14, UI_AMBER, 490, 80, 0);
    static const char *const letters[7] = { "S", "M", "T", "W", "T", "F", "S" };
    for (int d = 0; d < 7; d++) {
        lv_obj_t *b = lv_obj_create(p);
        lv_obj_remove_style_all(b);
        lv_obj_set_size(b, 48, 48);
        lv_obj_set_pos(b, 490 + d * 58, 102);
        lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(b, 1, 0);
        lv_obj_set_style_border_color(b, lv_color_hex(UI_EDGE), 0);
        lv_obj_set_style_translate_y(b, 2, LV_STATE_PRESSED);
        lv_obj_clear_flag(b, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_ext_click_area(b, 4);
        lv_obj_add_event_cb(b, aed_day_event_cb, LV_EVENT_CLICKED, (void *)(intptr_t)d);
        lv_obj_t *l = lv_label_create(b);
        lv_label_set_text(l, letters[d]);
        lv_obj_set_style_text_font(l, &p4_font_18, 0);
        lv_obj_center(l);
        aed_days[d] = b;
    }
    alarm_label(p, "Repeat every week", &p4_font_16, UI_TEXT, 490, 172, 0);
    aed_repeat = alarm_switch(p);
    lv_obj_set_pos(aed_repeat, 834, 166);
    lv_obj_add_event_cb(aed_repeat, aed_repeat_event_cb, LV_EVENT_VALUE_CHANGED, NULL);
    alarm_label(p, "Volume", &p4_font_16, UI_TEXT, 490, 222, 0);
    aed_vol = lv_slider_create(p);
    lv_slider_set_range(aed_vol, 0, 100);
    lv_obj_set_size(aed_vol, 240, 14);
    lv_obj_set_pos(aed_vol, 574, 228);
    lv_obj_set_style_bg_color(aed_vol, lv_color_hex(UI_SLIDER_BG), LV_PART_MAIN);
    lv_obj_set_style_bg_color(aed_vol, lv_color_hex(UI_TERTIARY), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(aed_vol, lv_color_hex(UI_ACCENT_BRIGHT), LV_PART_KNOB);
    lv_obj_set_ext_click_area(aed_vol, 14);
    lv_obj_add_event_cb(aed_vol, aed_vol_event_cb, LV_EVENT_VALUE_CHANGED, NULL);
    aed_vol_lbl = alarm_label(p, "30%", &p4_font_16, UI_TEXT, 844, 222, 0);

    lv_obj_t *rule = lv_obj_create(p);
    lv_obj_remove_style_all(rule);
    lv_obj_set_size(rule, 870, 1);
    lv_obj_set_pos(rule, 30, 276);
    lv_obj_set_style_bg_color(rule, lv_color_hex(UI_EDGE), 0);
    lv_obj_set_style_bg_opa(rule, LV_OPA_COVER, 0);

    /* what plays */
    alarm_label(p, "PLAY", &p4_font_14, UI_AMBER, 40, 290, 0);
    static const char *const kinds[3] = { "Current queue", "A playlist", "Random songs" };
    for (int i = 0; i < 3; i++) {
        aed_play_btn[i] = aed_button(p, kinds[i], 40 + i * 214, 314, 200, 52, aed_play_event_cb, i, false);
    }
    aed_pl_field = create_button(p, "", 630, 52);
    lv_obj_set_pos(aed_pl_field, 40, 382);
    lv_obj_add_event_cb(aed_pl_field, aed_choose_event_cb, LV_EVENT_CLICKED, NULL);
    aed_pl_name = lv_obj_get_child(aed_pl_field, 0);
    lv_obj_set_width(aed_pl_name, 440);
    lv_label_set_long_mode(aed_pl_name, LV_LABEL_LONG_DOT);
    lv_obj_align(aed_pl_name, LV_ALIGN_LEFT_MID, 18, 0);
    lv_obj_t *ch = lv_label_create(aed_pl_field);
    lv_label_set_text(ch, "CHOOSE  " LV_SYMBOL_RIGHT);
    lv_obj_set_style_text_color(ch, lv_color_hex(UI_ACCENT), 0);
    lv_obj_align(ch, LV_ALIGN_RIGHT_MID, -18, 0);

    aed_msg = alarm_label(p, "", &p4_font_16, UI_RED, 40, 454, 860);
}

static void alarm_editor_open(int index)
{
    if (aed_overlay == NULL) return;
    alarm_edit_is_new = (index < 0 || index >= alarm_count);
    if (alarm_edit_is_new) {
        memset(&alarm_edit, 0, sizeof(alarm_edit));
        alarm_edit.time_s = 7 * 3600;             /* 7:00 AM, weekdays */
        alarm_edit.dow = 0x3E;
        alarm_edit.enabled = true;
        alarm_edit.repeat = true;
        alarm_edit.volume = 30;
        snprintf(alarm_edit.url, sizeof(alarm_edit.url), "0");
    } else {
        alarm_edit = alarm_list[index];
        alarm_edit.enabled = true;                /* saving an edited alarm switches it on */
    }
    lv_label_set_text(lv_obj_get_child(aed_overlay, 0), alarm_edit_is_new ? "NEW ALARM" : "EDIT ALARM");
    lv_label_set_text(lv_obj_get_child(aed_delete, 0), "DELETE");
    aed_delete_armed = 0;
    if (alarm_edit_is_new) lv_obj_add_flag(aed_delete, LV_OBJ_FLAG_HIDDEN);
    else                   lv_obj_clear_flag(aed_delete, LV_OBJ_FLAG_HIDDEN);
    aed_set_msg("");
    aed_refresh();
    ui_show_panel(aed_overlay);
}

/* ================================================== the playlist chooser */
static void apl_pick_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    const int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (alarm_playlists == NULL || i < 0 || i >= alarm_playlist_count) return;
    snprintf(alarm_edit.url, sizeof(alarm_edit.url), "%s", alarm_playlists[i].url);
    lv_obj_add_flag(apl_overlay, LV_OBJ_FLAG_HIDDEN);
    aed_set_msg("");
    aed_refresh();
}

static void apl_fill(void)
{
    if (apl_list == NULL) return;
    lv_obj_clean(apl_list);
    if (alarm_playlists == NULL || alarm_playlist_count == 0) {
        alarm_label(apl_list, "No saved playlists found (save one from the Queue).",
                    &p4_font_16, UI_TEXT_SOFT, 8, 8, 480);
        return;
    }
    for (int i = 0; i < alarm_playlist_count; i++) {
        lv_obj_t *b = create_source_item_button(apl_list, alarm_playlists[i].name, 480, 46, false, true);
        lv_obj_set_pos(b, 0, i * 52);
        lv_obj_add_event_cb(b, apl_pick_event_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    }
}

static void alarm_playlists_render_job(void *arg)
{
    (void)arg;
    if (apl_overlay != NULL && !lv_obj_has_flag(apl_overlay, LV_OBJ_FLAG_HIDDEN)) apl_fill();
    /* Names may have just arrived for alarms already listed. */
    if (alarms_overlay != NULL && !lv_obj_has_flag(alarms_overlay, LV_OBJ_FLAG_HIDDEN)) {
        alarms_render_ctx_t ctx = { .msg = NULL, .error = false };
        if (alarm_count > 0) alarms_render_job(&ctx);
    }
}

static void alarm_playlist_picker_open(void)
{
    if (apl_overlay == NULL) return;
    apl_fill();
    lv_obj_clear_flag(apl_overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(apl_overlay);
    if (alarm_playlist_count == 0) alarm_send_request(SOURCE_ALARM_PLAYLISTS, 0, NULL);
}

static void alarm_playlist_picker_create(lv_obj_t *screen)
{
    apl_overlay = modal_create(screen, 560, 440, &apl_panel);
    modal_text(apl_panel, "CHOOSE A PLAYLIST", &p4_font_28, UI_TEXT, 28, 18);
    apl_list = lv_obj_create(apl_panel);
    lv_obj_remove_style_all(apl_list);
    lv_obj_set_size(apl_list, 500, 440 - 64 - 84);
    lv_obj_set_pos(apl_list, 28, 64);
    lv_obj_add_flag(apl_list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(apl_list, LV_DIR_VER);
    lv_obj_t *close = create_button(apl_panel, "CLOSE", 140, 52);
    lv_obj_align(close, LV_ALIGN_BOTTOM_RIGHT, -28, -18);
    lv_obj_add_event_cb(close, modal_close_event_cb, LV_EVENT_CLICKED, apl_overlay);
}

/* ================================================= set-up and amp timing */
static void alarms_ui_create(lv_obj_t *screen)
{
    nvs_handle_t nvs;
    uint8_t v = 0;
    if (nvs_open(PLAYER_NVS_NAMESPACE, NVS_READONLY, &nvs) == ESP_OK) {
        if (nvs_get_u8(nvs, ALARM_NVS_AMP, &v) == ESP_OK) alarm_amp_enabled = (v != 0);
        nvs_close(nvs);
    }
    alarms_create(screen);
    alarm_editor_create(screen);
    alarm_playlist_picker_create(screen);
}

/* Called by the command task every few seconds.  Keeps a copy of every
 * room's enabled alarms (re-read every 10 minutes, or after a change) and
 * switches a room's amp and pre-amp on when one of its alarms is under
 * 75 s away. */
typedef struct { int time_s; uint8_t dow; } alarm_sched_t;
static alarm_sched_t alarm_sched[LMS_MAX_PLAYERS][ALARM_MAX];
static int           alarm_sched_n[LMS_MAX_PLAYERS];
static int           alarm_fired_key[LMS_MAX_PLAYERS];

static void alarm_amp_tick(void)
{
    if (!alarm_amp_enabled || !HA_TOKEN_SET) return;
    const time_t now = time(NULL);
    struct tm tmv;
    localtime_r(&now, &tmv);
    if (tmv.tm_year < 120) return;                         /* clock not set yet */

    static int64_t last_read_ms = 0;
    const int64_t ms = esp_timer_get_time() / 1000;
    if (alarm_sched_dirty || last_read_ms == 0 || ms - last_read_ms > 600000) {
        alarm_sched_dirty = false;
        last_read_ms = ms;
        char *buf = heap_caps_malloc(16384, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        alarm_t *tmp = heap_caps_malloc(sizeof(alarm_t) * ALARM_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        for (int i = 0; buf != NULL && tmp != NULL && i < lyrion_player_count; i++) {
            alarm_sched_n[i] = 0;
            if (!alarm_send(i, "\"alarms\",0,20,\"filter:enabled\"", buf, 16384)) continue;
            const int n = alarms_parse(buf, tmp, ALARM_MAX);
            for (int k = 0; k < n; k++) {
                if (!tmp[k].enabled) continue;
                alarm_sched[i][alarm_sched_n[i]].time_s = tmp[k].time_s;
                alarm_sched[i][alarm_sched_n[i]].dow = tmp[k].dow;
                alarm_sched_n[i]++;
            }
        }
        if (buf) heap_caps_free(buf);
        if (tmp) heap_caps_free(tmp);
    }

    const int now_s = tmv.tm_hour * 3600 + tmv.tm_min * 60 + tmv.tm_sec;
    for (int i = 0; i < lyrion_player_count; i++) {
        for (int k = 0; k < alarm_sched_n[i]; k++) {
            const alarm_sched_t *a = &alarm_sched[i][k];
            if (!(a->dow & (1 << tmv.tm_wday))) continue;
            const int ahead = a->time_s - now_s;
            if (ahead < 0 || ahead > 75) continue;
            const int key = tmv.tm_yday * 100000 + a->time_s;
            if (alarm_fired_key[i] == key) continue;
            alarm_fired_key[i] = key;
            const ha_player_switches_t *sw = ha_switches_for_index(i);
            ESP_LOGI(TAG, "Alarm in %d s on %s - switching the amp on", ahead, lyrion_players[i].name);
            ha_power_sequence(sw, true);     /* pre-amp on first, then the amp */
        }
    }
}
