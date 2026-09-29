/*
 * P4-LMS - LYRION SERVER screen, network switching, browser task, SOURCE panel
 *
 * Part of main.c.  This file is #included by main.c (in order) and is not
 * compiled on its own, so it shares main.c's includes and static names.
 */

/* ========================================================================
 * LYRION SERVER - find the server on this network and use its players
 * ======================================================================== */
#define LMS_MAX_FOUND 6
typedef struct { char name[40]; char base[64]; } lms_found_t;
static lms_found_t lms_found[LMS_MAX_FOUND];
static int lms_found_count = 0;

static lv_obj_t *lms_overlay = NULL;
static lv_obj_t *lms_cur_label = NULL;
static lv_obj_t *lms_msg_label = NULL;
static lv_obj_t *lms_list = NULL;

/* Lyrion answers the same UDP broadcast the Squeezebox apps use to find it:
 * "e" + tags asking for its name and web port, sent to port 3483. */
static int lms_discover(void)
{
    lms_found_count = 0;
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) return -1;

    int yes = 1;
    setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &yes, sizeof(yes));
    struct timeval tv = { .tv_sec = 0, .tv_usec = 250000 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    struct sockaddr_in to;
    memset(&to, 0, sizeof(to));
    to.sin_family = AF_INET;
    to.sin_port = htons(3483);
    to.sin_addr.s_addr = htonl(INADDR_BROADCAST);
    static const char probe[] = "eIPAD\0NAME\0JSON\0";

    for (int round = 0; round < 2; round++) {
        sendto(sock, probe, sizeof(probe) - 1, 0, (struct sockaddr *)&to, sizeof(to));
        const int64_t until = esp_timer_get_time() + 1000000;
        while (esp_timer_get_time() < until) {
            char buf[256];
            struct sockaddr_in from;
            socklen_t from_len = sizeof(from);
            const int n = recvfrom(sock, buf, sizeof(buf), 0, (struct sockaddr *)&from, &from_len);
            if (n <= 1 || buf[0] != 'E') continue;

            char name[40] = "Lyrion Music Server";
            int port = 9000;
            for (int i = 1; i + 5 <= n; ) {                  /* tag(4) len(1) value */
                const int len = (uint8_t)buf[i + 4];
                if (i + 5 + len > n) break;
                if (memcmp(buf + i, "NAME", 4) == 0 && len > 0) {
                    snprintf(name, sizeof(name), "%.*s", len, buf + i + 5);
                } else if (memcmp(buf + i, "JSON", 4) == 0 && len > 0) {
                    char p[8];
                    snprintf(p, sizeof(p), "%.*s", len < 7 ? len : 7, buf + i + 5);
                    if (atoi(p) > 0) port = atoi(p);
                }
                i += 5 + len;
            }
            char base[64];
            snprintf(base, sizeof(base), "http://%s:%d", inet_ntoa(from.sin_addr), port);

            bool dup = false;
            for (int k = 0; k < lms_found_count; k++) {
                if (strcmp(lms_found[k].base, base) == 0) dup = true;
            }
            if (!dup && lms_found_count < LMS_MAX_FOUND) {
                snprintf(lms_found[lms_found_count].name, sizeof(lms_found[0].name), "%s", name);
                snprintf(lms_found[lms_found_count].base, sizeof(lms_found[0].base), "%s", base);
                lms_found_count++;
            }
        }
    }
    close(sock);
    ESP_LOGI(TAG, "Lyrion servers found: %d", lms_found_count);
    return lms_found_count;
}

/* "Office Music" -> "Office" for the header buttons. */
static void lms_short_name(const char *name, char *out, size_t out_size)
{
    static const char *const endings[] = { " Music", " Player", " Speaker", " Audio" };
    char tmp[48];
    snprintf(tmp, sizeof(tmp), "%s", name);
    size_t len = strlen(tmp);
    for (size_t k = 0; k < sizeof(endings) / sizeof(endings[0]); k++) {
        const size_t el = strlen(endings[k]);
        if (len > el && strcasecmp(tmp + len - el, endings[k]) == 0) {
            tmp[len - el] = '\0';
            break;
        }
    }
    /* Copy by hand: the header label has room for 19 characters. */
    const char *src = tmp[0] ? tmp : name;
    size_t k = 0;
    for (; src[k] && k + 1 < out_size; k++) out[k] = src[k];
    if (out_size > 0) out[k] = '\0';
}

/* Ask a server for its players (connected ones first, up to four).
 * Returns the number found, or -1 if the server could not be reached. */
static int lms_fetch_players(const char *base, lyrion_player_t *out)
{
    char url[96];
    snprintf(url, sizeof(url), "%s/jsonrpc.js", base);
    char *buf = (char *)heap_caps_malloc(16384, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (buf == NULL) return -1;

    static const char req[] =
        "{\"id\":1,\"method\":\"slim.request\",\"params\":[\"\",[\"players\",0,20]]}";
    if (!http_post_json(url, req, buf, 16384)) {
        heap_caps_free(buf);
        return -1;
    }
    cJSON *root = cJSON_Parse(buf);
    heap_caps_free(buf);
    cJSON *res = root ? cJSON_GetObjectItem(root, "result") : NULL;
    cJSON *loop = cJSON_IsObject(res) ? cJSON_GetObjectItem(res, "players_loop") : NULL;

    int count = 0;
    for (int pass = 0; pass < 2 && count < LMS_MAX_PLAYERS; pass++) {  /* connected first */
        cJSON *p = NULL;
        cJSON_ArrayForEach(p, loop) {
            if (count >= LMS_MAX_PLAYERS) break;
            cJSON *id = cJSON_GetObjectItem(p, "playerid");
            cJSON *nm = cJSON_GetObjectItem(p, "name");
            cJSON *isp = cJSON_GetObjectItem(p, "isplayer");
            cJSON *con = cJSON_GetObjectItem(p, "connected");
            if (!cJSON_IsString(id) || !id->valuestring) continue;
            if (cJSON_IsNumber(isp) && isp->valueint == 0) continue;
            const bool connected = cJSON_IsNumber(con) ? con->valueint != 0 : true;
            if ((pass == 0) != connected) continue;
            lyrion_player_t *o = &out[count++];
            memset(o, 0, sizeof(*o));
            snprintf(o->id, sizeof(o->id), "%s", id->valuestring);
            snprintf(o->name, sizeof(o->name), "%s",
                     (cJSON_IsString(nm) && nm->valuestring) ? nm->valuestring : id->valuestring);
            lms_short_name(o->name, o->short_name, sizeof(o->short_name));
        }
    }
    if (root) cJSON_Delete(root);
    return root ? count : -1;
}

/* ---- screen (LVGL lock held) ---- */
static void lms_msg_locked(const char *text, uint32_t color)
{
    if (lms_msg_label == NULL) return;
    lv_label_set_text(lms_msg_label, text);
    lv_obj_set_style_text_color(lms_msg_label, lv_color_hex(color), 0);
}

static void lms_refresh_current_locked(void)
{
    if (lms_cur_label == NULL) return;
    char text[200];
    if (lms_find_profile(wifi_cur_ssid) >= 0) {
        snprintf(text, sizeof(text), "Network %s uses %s  (%d player%s)", wifi_cur_ssid,
                 lyrion_server_base, lyrion_player_count, lyrion_player_count == 1 ? "" : "s");
    } else {
        snprintf(text, sizeof(text), "No Lyrion server saved for network %s yet.", wifi_cur_ssid);
    }
    lv_label_set_text(lms_cur_label, text);
}

static void lms_row_event_cb(lv_event_t *e);

static void lms_rebuild_list_locked(void)
{
    if (lms_list == NULL) return;
    lv_obj_clean(lms_list);
    for (int i = 0; i < lms_found_count; i++) {
        const bool current = strcmp(lms_found[i].base, lyrion_server_base) == 0;
        lv_obj_t *row = lv_obj_create(lms_list);
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, 860, 56);
        lv_obj_set_pos(row, 6, 4 + i * 64);
        lv_obj_set_style_bg_color(row, lv_color_hex(current ? UI_ACCENT_DARK : UI_BUTTON), 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(row, 10, 0);
        lv_obj_set_style_border_width(row, current ? 2 : 1, 0);
        lv_obj_set_style_border_color(row, lv_color_hex(current ? UI_ACCENT_GLOW : UI_EDGE), 0);
        lv_obj_set_style_bg_color(row, lv_color_hex(UI_ACCENT_DARK), LV_STATE_PRESSED);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(row, lms_row_event_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);

        lv_obj_t *name = lv_label_create(row);
        lv_label_set_text(name, lms_found[i].name);
        lv_obj_set_width(name, 480);
        lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_color(name, lv_color_hex(UI_TEXT), 0);
        lv_obj_set_style_text_font(name, &p4_font_18, 0);
        label_one_line(name);
        lv_obj_align(name, LV_ALIGN_LEFT_MID, 16, 0);

        char info[96];
        snprintf(info, sizeof(info), "%s%s", current ? LV_SYMBOL_OK " In use  |  " : "",
                 lms_found[i].base + 7);                      /* without "http://" */
        lv_obj_t *meta = lv_label_create(row);
        lv_label_set_text(meta, info);
        lv_obj_set_style_text_color(meta, lv_color_hex(UI_TEXT_SOFT), 0);
        lv_obj_set_style_text_font(meta, &p4_font_14, 0);
        lv_obj_align(meta, LV_ALIGN_RIGHT_MID, -16, 0);
    }
}

static void lms_close(void)
{
    if (lms_overlay != NULL) {
        lv_obj_add_flag(lms_overlay, LV_OBJ_FLAG_HIDDEN);
        ui_schedule_repaint(lv_screen_active());
    }
}

static void lms_close_btn_cb(lv_event_t *e) { if (lv_event_get_code(e) == LV_EVENT_CLICKED) lms_close(); }

static void lms_find_btn_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED || wifi_busy) return;
    lms_msg_locked("Looking for Lyrion servers on this network...", UI_TEXT_SOFT);
    wifi_send(SOURCE_LMS_FIND, NULL, NULL);
}

static void lms_enter_btn_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED || wifi_busy) return;
    wp_open(2, "", -1);
}

static void lms_row_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED || wifi_busy) return;
    const int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i < 0 || i >= lms_found_count) return;
    char msg[128];
    snprintf(msg, sizeof(msg), "Reading the players on %s...", lms_found[i].name);
    lms_msg_locked(msg, UI_TEXT_SOFT);
    wifi_send(SOURCE_LMS_USE, lms_found[i].base, NULL);
}

static void lms_create(void)
{
    lms_overlay = lv_obj_create(lv_screen_active());
    lv_obj_remove_style_all(lms_overlay);
    lv_obj_set_size(lms_overlay, PANEL_W, PANEL_H);
    lv_obj_set_pos(lms_overlay, PANEL_X, PANEL_Y);
    lv_obj_set_style_bg_color(lms_overlay, lv_color_hex(UI_PANEL), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(lms_overlay, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(lms_overlay, 2, LV_PART_MAIN);
    lv_obj_set_style_border_color(lms_overlay, lv_color_hex(UI_AMBER), LV_PART_MAIN);
    lv_obj_set_style_radius(lms_overlay, 16, LV_PART_MAIN);
    lv_obj_clear_flag(lms_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(lms_overlay, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *title = lv_label_create(lms_overlay);
    lv_label_set_text(title, "LYRION SERVER");
    lv_obj_set_style_text_color(title, lv_color_hex(UI_TEXT), 0);
    lv_obj_set_style_text_font(title, &p4_font_28, 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 30, 20);

    lv_obj_t *close = create_button(lms_overlay, "CLOSE", 150, 50);
    lv_obj_align(close, LV_ALIGN_TOP_RIGHT, -30, 14);
    ui_add_settings_back(lms_overlay);
    lv_obj_add_event_cb(close, lms_close_btn_cb, LV_EVENT_CLICKED, NULL);

    lms_cur_label = lv_label_create(lms_overlay);
    lv_obj_set_width(lms_cur_label, 870);
    lv_label_set_long_mode(lms_cur_label, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_color(lms_cur_label, lv_color_hex(UI_ACCENT_GLOW), 0);
    lv_obj_set_style_text_font(lms_cur_label, &p4_font_16, 0);
    label_one_line(lms_cur_label);
    lv_obj_set_pos(lms_cur_label, 30, 80);

    lms_msg_label = lv_label_create(lms_overlay);
    lv_label_set_text(lms_msg_label, "");
    lv_obj_set_width(lms_msg_label, 870);
    lv_label_set_long_mode(lms_msg_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(lms_msg_label, &p4_font_14, 0);
    lv_obj_set_pos(lms_msg_label, 30, 106);

    lms_list = lv_obj_create(lms_overlay);
    lv_obj_remove_style_all(lms_list);
    lv_obj_set_size(lms_list, 890, PANEL_H - 142 - 76);
    lv_obj_set_pos(lms_list, 20, 142);
    lv_obj_add_flag(lms_list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(lms_list, LV_DIR_VER);

    lv_obj_t *find = create_button(lms_overlay, LV_SYMBOL_REFRESH "  FIND SERVERS", 230, 50);
    lv_obj_set_pos(find, 30, PANEL_H - 66);
    lv_obj_set_style_bg_color(find, lv_color_hex(UI_ACCENT), LV_PART_MAIN);
    lv_obj_set_style_text_color(lv_obj_get_child(find, 0), lv_color_hex(UI_ON_ACCENT), 0);
    lv_obj_add_event_cb(find, lms_find_btn_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *enter = create_button(lms_overlay, "ENTER ADDRESS", 220, 50);
    lv_obj_set_pos(enter, 276, PANEL_H - 66);
    lv_obj_add_event_cb(enter, lms_enter_btn_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_add_flag(lms_overlay, LV_OBJ_FLAG_HIDDEN);
}

static void lms_open(void)
{
    if (lms_overlay == NULL) lms_create();
    lms_refresh_current_locked();
    lms_rebuild_list_locked();
    if (!wifi_busy) {
        lms_msg_locked("FIND SERVERS looks for Lyrion on this network. Choosing one restarts "
                       "the display with that server's players.", UI_TEXT_SOFT);
    }
    ui_show_panel(lms_overlay);
}

static void lms_open_btn_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) lms_open();
}

/* ---- UI jobs for the worker side ---- */
typedef struct { const char *text; uint32_t color; bool rebuild; } screen_msg_ctx_t;
static void lms_msg_job(void *arg)
{
    const screen_msg_ctx_t *c = (const screen_msg_ctx_t *)arg;
    if (c->rebuild) lms_rebuild_list_locked();
    lms_msg_locked(c->text, c->color);
}
static void wifi_result_job(void *arg)
{
    const screen_msg_ctx_t *c = (const screen_msg_ctx_t *)arg;
    wifi_msg_locked(c->text, c->color);
    wifi_refresh_current_locked();
    wifi_rebuild_list_locked();
}

/* ---- worker side ---- */
static void lms_do_find(void)
{
    const int n = lms_discover();
    screen_msg_ctx_t ctx = {
        .text = n > 0 ? "Tap a server to use it on this network."
              : n == 0 ? "No Lyrion server answered. Check it's running and on this "
                         "network, or use ENTER ADDRESS."
                       : "Could not search the network. Try again.",
        .color = n > 0 ? UI_TEXT_SOFT : UI_RED,
        .rebuild = true };
    ui_call(lms_msg_job, &ctx);
    wifi_busy = false;
}

/* Use `base` for the current Wi-Fi network: read its players, save the
 * profile and restart into it.  Returns true if it will restart. */
static bool lms_use_server(const char *base, char *msg, size_t msg_size)
{
    lyrion_player_t players[LMS_MAX_PLAYERS];
    const int n = lms_fetch_players(base, players);
    if (n < 0) {
        snprintf(msg, msg_size, "Could not reach a Lyrion server at %s.", base + 7);
        return false;
    }
    if (n == 0) {
        snprintf(msg, msg_size, "The server at %s has no players yet.", base + 7);
        return false;
    }
    lms_save_profile(wifi_cur_ssid, wifi_cur_pass, base, players, n);
    snprintf(msg, msg_size, "Saved %s with %d player%s for network %s. Restarting...",
             base + 7, n, n == 1 ? "" : "s", wifi_cur_ssid);
    lms_request_restart("new Lyrion server for this network");
    return true;
}

static void lms_do_use(const char *base)
{
    char msg[200];
    const bool ok = lms_use_server(base, msg, sizeof(msg));
    screen_msg_ctx_t ctx = { .text = msg, .color = ok ? UI_ACCENT_GLOW : UI_RED };
    ui_call(lms_msg_job, &ctx);
    wifi_busy = false;
}

/* After joining a network: switch to its saved server, or find one. */
static void lms_after_wifi_change(char *msg, size_t msg_size)
{
    const int p = lms_find_profile(wifi_cur_ssid);
    if (p >= 0) {
        /* Keep the saved password current (the router's may have changed). */
        if (strcmp(lms_profiles[p].pass, wifi_cur_pass) != 0) {
            snprintf(lms_profiles[p].pass, sizeof(lms_profiles[p].pass), "%s", wifi_cur_pass);
            lms_profiles_save();
        }
        if (p != lms_active) {
            size_t used = strlen(msg);
            snprintf(msg + used, msg_size - used, " Switching to this network's Lyrion server - restarting...");
            lms_request_restart("network has a saved Lyrion server");
        }
        return;
    }
    /* New network: if exactly one server answers, set it up automatically. */
    const int n = lms_discover();
    size_t used = strlen(msg);
    if (n == 1) {
        char sub[200];
        if (lms_use_server(lms_found[0].base, sub, sizeof(sub))) {
            snprintf(msg + used, msg_size - used, " Found %s. %s", lms_found[0].name, sub);
            return;
        }
    }
    snprintf(msg + used, msg_size - used,
             n > 1 ? " Several Lyrion servers found - tap LYRION SERVER to choose one."
                   : " No Lyrion server found yet - tap LYRION SERVER.");
}

/* Switch to another network; if it doesn't connect, go back to the one
 * that was in use, so a typo can never leave the display offline. */
static void wifi_do_connect(const char *ssid, const char *pass, bool remember)
{
    char prev_ssid[33], prev_pass[65];
    snprintf(prev_ssid, sizeof(prev_ssid), "%s", wifi_cur_ssid);
    snprintf(prev_pass, sizeof(prev_pass), "%s", wifi_cur_pass);

    ESP_LOGI(TAG, "Wi-Fi: switching from %s to %s", prev_ssid, ssid);
    wifi_manual = true;
    if (wifi_reconnect_timer != NULL) esp_timer_stop(wifi_reconnect_timer);
    esp_wifi_disconnect();
    vTaskDelay(pdMS_TO_TICKS(500));

    bool ok = false;
    uint8_t reason = 0;
    if (wifi_apply_config(ssid, pass) == ESP_OK) {
        for (int attempt = 0; attempt < 3 && !ok; attempt++) {
            xEventGroupClearBits(wifi_event_group, WIFI_ATTEMPT_FAIL_BIT | WIFI_CONNECTED_BIT);
            esp_wifi_connect();
            const EventBits_t bits = xEventGroupWaitBits(wifi_event_group,
                                         WIFI_CONNECTED_BIT | WIFI_ATTEMPT_FAIL_BIT,
                                         pdFALSE, pdFALSE, pdMS_TO_TICKS(12000));
            if (bits & WIFI_CONNECTED_BIT) {
                ok = true;
            } else {
                reason = wifi_last_reason;
                /* A wrong password won't get better by retrying. */
                if (reason == WIFI_REASON_AUTH_FAIL || reason == WIFI_REASON_HANDSHAKE_TIMEOUT ||
                    reason == WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT) {
                    break;
                }
                vTaskDelay(pdMS_TO_TICKS(1000));
            }
        }
    }

    char msg[400];
    if (ok) {
        snprintf(wifi_cur_ssid, sizeof(wifi_cur_ssid), "%s", ssid);
        snprintf(wifi_cur_pass, sizeof(wifi_cur_pass), "%s", pass);
        if (remember) wifi_save_creds(ssid, pass);
        else          wifi_forget_creds();
        wifi_retry_count = 0;
        wifi_manual = false;
        snprintf(msg, sizeof(msg), "Connected to %s.%s", ssid,
                 remember ? " It will be used from now on." : "");
        ESP_LOGI(TAG, "Wi-Fi: now on %s", ssid);
        lms_after_wifi_change(msg, sizeof(msg));
    } else {
        const char *why =
            (reason == WIFI_REASON_AUTH_FAIL || reason == WIFI_REASON_HANDSHAKE_TIMEOUT ||
             reason == WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT) ? "the password looks wrong"
          : (reason == WIFI_REASON_NO_AP_FOUND) ? "the network was not found"
          : "it did not respond";
        ESP_LOGW(TAG, "Wi-Fi: could not join %s (reason %u) - returning to %s",
                 ssid, (unsigned)reason, prev_ssid);
        esp_wifi_disconnect();
        vTaskDelay(pdMS_TO_TICKS(300));
        wifi_apply_config(prev_ssid, prev_pass);
        snprintf(wifi_cur_ssid, sizeof(wifi_cur_ssid), "%s", prev_ssid);
        snprintf(wifi_cur_pass, sizeof(wifi_cur_pass), "%s", prev_pass);
        xEventGroupClearBits(wifi_event_group, WIFI_ATTEMPT_FAIL_BIT);
        wifi_manual = false;                   /* normal retries take over from here */
        wifi_retry_count = 0;
        esp_wifi_connect();
        snprintf(msg, sizeof(msg), "Could not connect to %s - %s. Going back to %s.",
                 ssid, why, prev_ssid);
    }

    {
        screen_msg_ctx_t ctx = { .text = msg, .color = ok ? UI_ACCENT_GLOW : UI_RED };
        ui_call(wifi_result_job, &ctx);
    }
    if (lyrion_refresh_event != NULL) {
        xEventGroupSetBits(lyrion_refresh_event, LYRION_REFRESH_BIT);
    }
    wifi_busy = false;
}

static void alarms_worker(source_request_t request);   /* 18c_alarms.c */
static void playlists_worker(source_request_t request);   /* 18f_playlists.c */

static void ss_wall_fetch(int n);            /* 18e_screensavers.c */

static void source_browser_task(void *arg)
{
    (void)arg;
    source_request_t request;

    ESP_LOGI(TAG, "Lyrion source browser task started");

    while (1) {
        if (xQueueReceive(source_queue, &request, portMAX_DELAY) == pdTRUE) {
            if (request.mode == SOURCE_LOAD_PLAYLIST ||
                request.mode == SOURCE_LOAD_ALBUM ||
                request.mode == SOURCE_LOAD_ARTIST ||
                request.mode == SOURCE_LOAD_GENRE ||
                request.mode == SOURCE_LOAD_FOLDER ||
                request.mode == SOURCE_LOAD_TRACK ||
                request.mode == SOURCE_LOAD_FAVORITE ||
                request.mode == SOURCE_LOAD_FAVORITE_URL ||
                request.mode == SOURCE_RADIO_PLAY) {
                source_send_load(request);
            } else if (request.mode == SOURCE_QUEUE_JUMP ||
                       request.mode == SOURCE_QUEUE_REMOVE ||
                       request.mode == SOURCE_QUEUE_CLEAR ||
                       request.mode == SOURCE_QUEUE_MOVE) {
                source_queue_command(request);
            } else if (request.mode == SOURCE_WALL_FETCH) {
                ss_wall_fetch(request.id);
            } else if (request.mode == SOURCE_SELECT_QUEUE) {
                source_select_queue(request);
            } else if (request.mode == SOURCE_QUEUE_SAVE) {
                source_queue_save(request);
            } else if (request.mode == SOURCE_PLAYLIST_DELETE) {
                source_playlist_delete(request);
            } else if (request.mode == SOURCE_WIFI_SCAN) {
                wifi_do_scan();
            } else if (request.mode == SOURCE_WIFI_CONNECT) {
                wifi_do_connect(request.title, request.item_id, true);
            } else if (request.mode == SOURCE_WIFI_FORGET) {
                wifi_do_connect(CONFIG_P4_LMS_WIFI_SSID, CONFIG_P4_LMS_WIFI_PASSWORD, false);
            } else if (request.mode == SOURCE_LMS_FIND) {
                lms_do_find();
            } else if (request.mode == SOURCE_LMS_USE) {
                lms_do_use(request.title);
            } else if (request.mode == SOURCE_FAV_TOGGLE || request.mode == SOURCE_FAV_DELETE ||
                       request.mode == SOURCE_FAV_CHECK || request.mode == SOURCE_FAV_ADD_TRACK ||
                       request.mode == SOURCE_FAV_ADD_ALBUM || request.mode == SOURCE_SELECT_FAV) {
                favorites_worker(request);
            } else if (request.mode >= SOURCE_ALARMS_LOAD && request.mode <= SOURCE_ALARM_PLAYLISTS) {
                alarms_worker(request);
            } else if (request.mode >= SOURCE_PLAYLIST_TRACKS && request.mode <= SOURCE_PL_NEW_ADD) {
                playlists_worker(request);
            } else {
                source_query_and_render(request);
            }
        }
    }
}

/* On the album art: jump straight into this track's album in SOURCE,
 * bypassing Artists/Albums/Genres entirely. */
/* A tap on the album art opens this song's album in Library. */
static void album_art_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED || source_queue == NULL) return;
    if (playback_album_id <= 0) {
        ui_toast("No album to open for this stream");
        return;
    }

    source_nav_depth = 0;      /* BACK returns to the SOURCE home screen, not old history */

    source_request_t request = {
        .mode = SOURCE_ALBUM_TRACKS,
        .id = playback_album_id,
        .offset = 0
    };
    xQueueSend(source_queue, &request, 0);
}


/* Tapping the song title on the main screen opens the queue. */
static void song_label_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED || source_queue == NULL) return;
    source_nav_depth = 0;              /* BACK from here goes to the SOURCE home */
    source_request_t request = { .mode = SOURCE_QUEUE, .offset = 0, .push_history = false };
    xQueueSend(source_queue, &request, 0);
}


static void create_source_browser(lv_obj_t *screen)
{
    if (!source_init_items()) {
        ESP_LOGE(TAG, "Source browser disabled: PSRAM allocation failed");
        return;
    }

    source_overlay = lv_obj_create(screen);
    lv_obj_remove_style_all(source_overlay);
    lv_obj_set_size(source_overlay, PANEL_W, PANEL_H);
    lv_obj_set_pos(source_overlay, PANEL_X, PANEL_Y);
    lv_obj_set_style_bg_color(source_overlay, lv_color_hex(UI_PANEL), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(source_overlay, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(source_overlay, 2, LV_PART_MAIN);
    lv_obj_set_style_border_color(source_overlay, lv_color_hex(UI_AMBER), LV_PART_MAIN);
    lv_obj_set_style_radius(source_overlay, 16, LV_PART_MAIN);
    lv_obj_clear_flag(source_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(source_overlay, LV_OBJ_FLAG_HIDDEN);

    /* Dedicated scroll area for album/folder tracks.  Keeping this separate
     * from the overlay prevents the BACK/CLOSE and page controls from
     * scrolling off the screen with the track list. */
    source_track_list = lv_obj_create(source_overlay);
    lv_obj_remove_style_all(source_track_list);
    lv_obj_set_size(source_track_list, SOURCE_TRACK_LIST_W, SOURCE_TRACK_LIST_H);
    lv_obj_set_pos(source_track_list, SOURCE_TRACK_LIST_X, SOURCE_TRACK_LIST_Y);
    lv_obj_set_style_bg_opa(source_track_list, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(source_track_list, 0, LV_PART_MAIN);
    lv_obj_add_flag(source_track_list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(source_track_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(source_track_list, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_add_flag(source_track_list, LV_OBJ_FLAG_HIDDEN);

    source_title_label = lv_label_create(source_overlay);
    lv_label_set_text(source_title_label, "MUSIC SOURCE");
    lv_obj_set_style_text_color(source_title_label, lv_color_hex(UI_TEXT), 0);
    lv_obj_set_style_text_font(source_title_label, &p4_font_28, 0);
    /* Long album names end in "..." instead of running under BACK/CLOSE. */
    lv_obj_set_width(source_title_label, 620);
    lv_label_set_long_mode(source_title_label, LV_LABEL_LONG_DOT);
    label_one_line(source_title_label);
    lv_obj_align(source_title_label, LV_ALIGN_TOP_LEFT, 30, 20);

    /* Hairline under the title row. */
    lv_obj_t *source_title_rule = lv_obj_create(source_overlay);
    lv_obj_remove_style_all(source_title_rule);
    lv_obj_set_size(source_title_rule, 870, 1);
    lv_obj_set_pos(source_title_rule, 30, 76);
    lv_obj_set_style_bg_color(source_title_rule, lv_color_hex(UI_EDGE_SOFT), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(source_title_rule, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_clear_flag(source_title_rule, LV_OBJ_FLAG_CLICKABLE);

    source_detail_album_caption = lv_label_create(source_overlay);
    lv_label_set_text(source_detail_album_caption, "ALBUM");
    lv_obj_set_style_text_color(source_detail_album_caption, lv_color_hex(UI_AMBER), 0);
    lv_obj_set_style_text_font(source_detail_album_caption, &p4_font_14, 0);
    lv_obj_align(source_detail_album_caption, LV_ALIGN_TOP_LEFT, 30, 108);
    lv_obj_add_flag(source_detail_album_caption, LV_OBJ_FLAG_HIDDEN);

    source_detail_album_label = lv_label_create(source_overlay);
    lv_label_set_text(source_detail_album_label, "");
    lv_obj_set_size(source_detail_album_label, 220, 68);      /* two lines, then "..." */
    lv_label_set_long_mode(source_detail_album_label, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_color(source_detail_album_label, lv_color_hex(UI_TEXT), 0);
    lv_obj_set_style_text_font(source_detail_album_label, &p4_font_28, 0);
    lv_obj_set_style_text_align(source_detail_album_label, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_pos(source_detail_album_label, 30, 130);
    lv_obj_add_flag(source_detail_album_label, LV_OBJ_FLAG_HIDDEN);

    source_detail_track_count = lv_label_create(source_overlay);
    lv_label_set_text(source_detail_track_count, "");
    lv_obj_set_style_text_color(source_detail_track_count, lv_color_hex(UI_TEXT_SOFT), 0);
    lv_obj_set_style_text_font(source_detail_track_count, &p4_font_16, 0);
    lv_obj_set_pos(source_detail_track_count, 30, 204);
    lv_obj_add_flag(source_detail_track_count, LV_OBJ_FLAG_HIDDEN);

    source_detail_fav_button = create_button(source_overlay, P4_SYMBOL_STAR "  FAV", 96, 34);
    lv_obj_set_pos(source_detail_fav_button, 162, 196);
    lv_obj_set_ext_click_area(source_detail_fav_button, 6);
    lv_obj_add_event_cb(source_detail_fav_button, source_album_fav_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_flag(source_detail_fav_button, LV_OBJ_FLAG_HIDDEN);

    source_back_button = create_button(source_overlay, "BACK", 100, 52);
    lv_obj_align(source_back_button, LV_ALIGN_TOP_RIGHT, -150, 12);
    lv_obj_add_event_cb(source_back_button, source_back_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_set_style_bg_color(source_back_button, lv_color_hex(UI_ACCENT), LV_PART_MAIN);
    lv_obj_set_style_bg_color(source_back_button, lv_color_hex(UI_ACCENT), LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_text_color(lv_obj_get_child(source_back_button, 0), lv_color_hex(UI_ON_ACCENT), 0);

    source_close_button = create_button(source_overlay, "CLOSE", 100, 52);
    lv_obj_align(source_close_button, LV_ALIGN_TOP_RIGHT, -30, 12);
    lv_obj_add_event_cb(source_close_button, source_close_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_set_style_bg_color(source_close_button, lv_color_hex(UI_ACCENT), LV_PART_MAIN);
    lv_obj_set_style_bg_color(source_close_button, lv_color_hex(UI_ACCENT), LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_text_color(lv_obj_get_child(source_close_button, 0), lv_color_hex(UI_ON_ACCENT), 0);

    /* Fixed navigation bar.  The normal two-column menu uses 60 px rows
     * so all five rows fit above this bar.  The bar is also kept opaque and
     * in the foreground so no item button can appear underneath it. */
    source_nav_bar = lv_obj_create(source_overlay);
    lv_obj_remove_style_all(source_nav_bar);
    lv_obj_set_size(source_nav_bar, 930, 78);
    lv_obj_set_pos(source_nav_bar, 0, PANEL_H - 82);
    lv_obj_set_style_bg_color(source_nav_bar, lv_color_hex(UI_PANEL), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(source_nav_bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(source_nav_bar, 1, LV_PART_MAIN);
    lv_obj_set_style_border_side(source_nav_bar, LV_BORDER_SIDE_TOP, LV_PART_MAIN);
    lv_obj_set_style_border_color(source_nav_bar, lv_color_hex(UI_EDGE_SOFT), LV_PART_MAIN);
    lv_obj_clear_flag(source_nav_bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(source_nav_bar, LV_OBJ_FLAG_FLOATING);

    source_play_context_button = create_button(source_nav_bar, "PLAY", 170, 44);
    lv_obj_set_pos(source_play_context_button, 25, 8);
    lv_obj_add_event_cb(source_play_context_button, source_play_context_event_cb,
                        LV_EVENT_CLICKED, NULL);
    lv_obj_add_flag(source_play_context_button, LV_OBJ_FLAG_HIDDEN);

    /* Queue view only: save the queue as a named playlist. */
    source_queue_play_button = create_button(source_nav_bar, LV_SYMBOL_PLAY "  PLAY", 104, 44);
    lv_obj_set_pos(source_queue_play_button, 690, 8);
    lv_obj_set_style_bg_color(source_queue_play_button, lv_color_hex(UI_ACCENT), LV_PART_MAIN);
    lv_obj_set_style_text_color(lv_obj_get_child(source_queue_play_button, 0),
                                lv_color_hex(UI_ON_ACCENT), 0);
    lv_obj_add_event_cb(source_queue_play_button, source_queue_play_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_flag(source_queue_play_button, LV_OBJ_FLAG_HIDDEN);

    source_save_button = create_button(source_nav_bar, "SAVE", 104, 44);
    lv_obj_set_pos(source_save_button, 806, 8);
    lv_obj_add_event_cb(source_save_button, source_save_button_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_flag(source_save_button, LV_OBJ_FLAG_HIDDEN);

    source_prev_button = create_button(source_nav_bar, "< PREV", 110, 44);
    lv_obj_set_pos(source_prev_button, 253, 8);
    lv_obj_add_event_cb(source_prev_button, source_prev_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_set_style_bg_color(source_prev_button, lv_color_hex(UI_ACCENT), LV_PART_MAIN);
    lv_obj_set_style_bg_color(source_prev_button, lv_color_hex(UI_PANEL), LV_PART_MAIN | LV_STATE_DISABLED);
    lv_obj_set_style_bg_color(source_prev_button, lv_color_hex(UI_ACCENT), LV_PART_MAIN | LV_STATE_PRESSED);
    /* Text colour on the button (the label inherits it), so it can change
     * with the state: dark on the accent colour, dim when greyed out. */
    lv_obj_set_style_text_color(source_prev_button, lv_color_hex(UI_ON_ACCENT), LV_PART_MAIN);
    lv_obj_set_style_text_color(source_prev_button, lv_color_hex(UI_TEXT_SOFT), LV_PART_MAIN | LV_STATE_DISABLED);
    button_text_follows_state(source_prev_button);
    lv_obj_add_state(source_prev_button, LV_STATE_DISABLED);

    source_page_label = lv_label_create(source_nav_bar);
    lv_obj_set_style_text_color(source_page_label, lv_color_hex(UI_TEXT_SOFT), 0);
    lv_obj_set_pos(source_page_label, 398, 22);
    lv_obj_set_size(source_page_label, 135, 32);
    lv_obj_set_style_text_align(source_page_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(source_page_label, "Items 1-0");

    source_next_button = create_button(source_nav_bar, "NEXT >", 110, 44);
    lv_obj_set_pos(source_next_button, 567, 8);
    lv_obj_add_event_cb(source_next_button, source_next_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_set_style_bg_color(source_next_button, lv_color_hex(UI_ACCENT), LV_PART_MAIN);
    lv_obj_set_style_bg_color(source_next_button, lv_color_hex(UI_PANEL), LV_PART_MAIN | LV_STATE_DISABLED);
    lv_obj_set_style_bg_color(source_next_button, lv_color_hex(UI_ACCENT), LV_PART_MAIN | LV_STATE_PRESSED);
    /* Text colour on the button (the label inherits it), so it can change
     * with the state: dark on the accent colour, dim when greyed out. */
    lv_obj_set_style_text_color(source_next_button, lv_color_hex(UI_ON_ACCENT), LV_PART_MAIN);
    lv_obj_set_style_text_color(source_next_button, lv_color_hex(UI_TEXT_SOFT), LV_PART_MAIN | LV_STATE_DISABLED);
    button_text_follows_state(source_next_button);
    lv_obj_add_state(source_next_button, LV_STATE_DISABLED);

    source_select_create(source_nav_bar);

    source_art_image = lv_image_create(source_overlay);
    lv_obj_set_size(source_art_image, SOURCE_ART_SIZE, SOURCE_ART_SIZE);
    lv_obj_set_pos(source_art_image, 30, 232);     /* ends at 432, above the paging row (446) */
    lv_obj_add_flag(source_art_image, LV_OBJ_FLAG_HIDDEN);

    source_empty_label = lv_label_create(source_overlay);
    lv_label_set_text(source_empty_label, "NO ITEMS FOUND");
    lv_obj_set_style_text_color(source_empty_label, lv_color_hex(UI_TEXT_SOFT), 0);
    lv_obj_set_style_text_font(source_empty_label, &p4_font_16, 0);
    lv_obj_center(source_empty_label);
    lv_obj_add_flag(source_empty_label, LV_OBJ_FLAG_HIDDEN);
}

