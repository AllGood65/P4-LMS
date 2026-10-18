/*
 * P4-LMS - Wi-Fi screen: scan, password entry
 *
 * Part of main.c.  This file is #included by main.c (in order) and is not
 * compiled on its own, so it shares main.c's includes and static names.
 */

/* ========================================================================
 * WI-FI PICKER - tap the Wi-Fi status in the header
 * ======================================================================== */
#define WIFI_MAX_NETS 20
typedef struct { char ssid[33]; int8_t rssi; int auth; } wifi_net_t;
static wifi_net_t wifi_nets[WIFI_MAX_NETS];
static int wifi_net_count = 0;
static volatile bool wifi_busy = false;

static lv_obj_t *wifi_overlay = NULL;
static lv_obj_t *wifi_cur_label = NULL;
static lv_obj_t *wifi_msg_label = NULL;
static lv_obj_t *wifi_list = NULL;
static lv_obj_t *wifi_builtin_btn = NULL;

static lv_obj_t *wp_backdrop = NULL;           /* name / password entry */
static lv_obj_t *wp_title = NULL;
static lv_obj_t *wp_textarea = NULL;
static lv_obj_t *wp_status = NULL;
static lv_obj_t *wp_show_btn = NULL;
static lv_obj_t *wp_ok_btn = NULL;             /* CONNECT / USE / SEARCH, by stage */
static int  wp_stage = 0;  /* 0 = password, 1 = network name, 2 = server address, 3 = library search, 4 = app search */
static char wp_ssid[33] = "";
static int  wp_auth = -1;                       /* -1 = unknown (typed-in network) */

/* ---- screen helpers (LVGL lock held) ---- */
static void wifi_msg_locked(const char *text, uint32_t color)
{
    if (wifi_msg_label == NULL) return;
    lv_label_set_text(wifi_msg_label, text);
    lv_obj_set_style_text_color(wifi_msg_label, lv_color_hex(color), 0);
}

static void wifi_refresh_current_locked(void)
{
    if (wifi_cur_label == NULL) return;
    char text[128];
    const bool up = (xEventGroupGetBits(wifi_event_group) & WIFI_CONNECTED_BIT) != 0;
    esp_netif_ip_info_t ip;
    memset(&ip, 0, sizeof(ip));
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (up && netif != NULL && esp_netif_get_ip_info(netif, &ip) == ESP_OK) {
        snprintf(text, sizeof(text), LV_SYMBOL_WIFI "  Connected to %s   (" IPSTR ")",
                 wifi_cur_ssid, IP2STR(&ip.ip));
    } else {
        snprintf(text, sizeof(text), LV_SYMBOL_WIFI "  Not connected - trying %s", wifi_cur_ssid);
    }
    lv_label_set_text(wifi_cur_label, text);
    if (wifi_builtin_btn != NULL) {
        if (wifi_has_saved_creds()) lv_obj_clear_flag(wifi_builtin_btn, LV_OBJ_FLAG_HIDDEN);
        else                        lv_obj_add_flag(wifi_builtin_btn, LV_OBJ_FLAG_HIDDEN);
    }
}

static const char *wifi_strength(int8_t rssi)
{
    if (rssi >= -55) return "Excellent";
    if (rssi >= -67) return "Good";
    if (rssi >= -75) return "Fair";
    return "Weak";
}

static void wp_open(int stage, const char *ssid, int auth);
static void wifi_row_event_cb(lv_event_t *e);
static void lms_msg_locked(const char *text, uint32_t color);   /* LYRION SERVER screen */
static void lms_open_btn_cb(lv_event_t *e);

static void wifi_rebuild_list_locked(void)
{
    if (wifi_list == NULL) return;
    lv_obj_clean(wifi_list);
    const bool up = (xEventGroupGetBits(wifi_event_group) & WIFI_CONNECTED_BIT) != 0;

    for (int i = 0; i < wifi_net_count; i++) {
        const wifi_net_t *n = &wifi_nets[i];
        const bool current = up && strcmp(n->ssid, wifi_cur_ssid) == 0;

        lv_obj_t *row = lv_obj_create(wifi_list);
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
        lv_obj_add_event_cb(row, wifi_row_event_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);

        lv_obj_t *name = lv_label_create(row);
        lv_label_set_text(name, n->ssid);
        lv_obj_set_width(name, 520);
        lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_color(name, lv_color_hex(UI_TEXT), 0);
        lv_obj_set_style_text_font(name, &p4_font_18, 0);
        label_one_line(name);
        lv_obj_align(name, LV_ALIGN_LEFT_MID, 16, 0);

        char info[64];
        snprintf(info, sizeof(info), "%s%s  |  %s",
                 current ? LV_SYMBOL_OK " Connected  |  " : "",
                 wifi_strength(n->rssi), n->auth == WIFI_AUTH_OPEN ? "Open" : "Secured");
        lv_obj_t *meta = lv_label_create(row);
        lv_label_set_text(meta, info);
        lv_obj_set_style_text_color(meta, lv_color_hex(UI_TEXT_SOFT), 0);
        lv_obj_set_style_text_font(meta, &p4_font_14, 0);
        lv_obj_align(meta, LV_ALIGN_RIGHT_MID, -16, 0);
    }
}

/* ---- buttons ---- */
static void wifi_send(source_mode_t mode, const char *ssid, const char *pass)
{
    if (source_queue == NULL) return;
    source_request_t r = { .mode = mode, .push_history = false };
    if (ssid) snprintf(r.title, sizeof(r.title), "%s", ssid);
    if (pass) snprintf(r.item_id, sizeof(r.item_id), "%s", pass);
    /* busy only once the job is really queued (a full queue would otherwise
     * leave every Wi-Fi and server button ignored until a restart) */
    wifi_busy = true;
    if (xQueueSend(source_queue, &r, pdMS_TO_TICKS(300)) != pdTRUE) {
        wifi_busy = false;
        ui_toast("Busy - try again in a moment");
    }
}

static void wifi_scan_btn_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED || wifi_busy) return;
    wifi_msg_locked("Scanning for networks...", UI_TEXT_SOFT);
    wifi_send(SOURCE_WIFI_SCAN, NULL, NULL);
}

static void wifi_other_btn_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED || wifi_busy) return;
    wp_open(1, "", -1);
}

static void wifi_builtin_btn_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED || wifi_busy) return;
    char msg[96];
    snprintf(msg, sizeof(msg), "Switching back to the built-in network (%s)...", CONFIG_P4_LMS_WIFI_SSID);
    wifi_msg_locked(msg, UI_TEXT_SOFT);
    wifi_send(SOURCE_WIFI_FORGET, NULL, NULL);
}

static void wifi_row_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED || wifi_busy) return;
    const int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i < 0 || i >= wifi_net_count) return;
    const wifi_net_t *n = &wifi_nets[i];

    const bool up = (xEventGroupGetBits(wifi_event_group) & WIFI_CONNECTED_BIT) != 0;
    if (up && strcmp(n->ssid, wifi_cur_ssid) == 0) {
        wifi_msg_locked("Already connected to this network.", UI_TEXT_SOFT);
        return;
    }
    if (n->auth == WIFI_AUTH_OPEN) {
        char msg[96];
        snprintf(msg, sizeof(msg), "Connecting to %s...", n->ssid);
        wifi_msg_locked(msg, UI_TEXT_SOFT);
        wifi_send(SOURCE_WIFI_CONNECT, n->ssid, "");
        return;
    }
    wp_open(0, n->ssid, n->auth);
}

static void wifi_close(void)
{
    if (wp_backdrop != NULL) lv_obj_add_flag(wp_backdrop, LV_OBJ_FLAG_HIDDEN);
    if (wifi_overlay != NULL) {
        lv_obj_add_flag(wifi_overlay, LV_OBJ_FLAG_HIDDEN);
        ui_schedule_repaint(lv_screen_active());
    }
}

static void wifi_close_btn_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) wifi_close();
}

/* ---- name / password entry ---- */
static void wp_hide(void)
{
    if (wp_backdrop != NULL) lv_obj_add_flag(wp_backdrop, LV_OBJ_FLAG_HIDDEN);
}

static void wp_status_set(const char *text, uint32_t color)
{
    if (wp_status == NULL) return;
    lv_label_set_text(wp_status, text);
    lv_obj_set_style_text_color(wp_status, lv_color_hex(color), 0);
}

/* Copy src to dst, replacing every "from" with "to". */
static void wp_replace_all(char *dst, size_t dst_size, const char *src, const char *from, const char *to)
{
    const size_t fl = strlen(from), tl = strlen(to);
    size_t n = 0;
    while (*src && n + 1 < dst_size) {
        if (strncmp(src, from, fl) == 0) {
            if (n + tl + 1 >= dst_size) break;
            memcpy(dst + n, to, tl);
            n += tl;
            src += fl;
        } else {
            dst[n++] = *src++;
        }
    }
    dst[n] = '\0';
}

static void wp_submit(void)
{
    const char *txt = lv_textarea_get_text(wp_textarea);
    if (wp_stage == 4) {                       /* search inside an app (YouTube, ...) */
        while (*txt == ' ') txt++;
        if (strlen(txt) < 1) {
            wp_status_set("Type what to look for.", UI_RED);
            return;
        }
        char words[96];                        /* keep the JSON action valid */
        size_t n = 0;
        for (const char *p = txt; *p && n + 1 < sizeof(words); p++) {
            if (*p != '"' && *p != '\\') words[n++] = *p;
        }
        words[n] = '\0';
        char tmp[768];
        source_request_t r = { .mode = SOURCE_RADIO_MENU, .offset = 0, .push_history = false };
        wp_replace_all(tmp, sizeof(tmp), source_app_input_action, "__TAGGEDINPUT__", words);
        wp_replace_all(r.radio_action, sizeof(r.radio_action), tmp, "__INPUT__", words);
        snprintf(r.title, sizeof(r.title), "%.60s", words);
        if (source_radio_depth < 6) {          /* BACK returns to the app's menu */
            snprintf(source_radio_parent_commands[source_radio_depth],
                     sizeof(source_radio_parent_commands[0]), "%s", source_radio_current_action);
            source_radio_depth++;
        }
        snprintf(source_radio_current_action, sizeof(source_radio_current_action), "%s", r.radio_action);
        ESP_LOGI(TAG, "App search: %.300s", r.radio_action);
        if (source_queue != NULL) xQueueSend(source_queue, &r, 0);
        wp_hide();
        return;
    }
    if (wp_stage == 3) {                       /* search the library */
        while (*txt == ' ') txt++;
        if (strlen(txt) < 2) {
            wp_status_set("Type at least two letters.", UI_RED);
            return;
        }
        source_request_t r = { .mode = SOURCE_SEARCH, .offset = 0, .push_history = true };
        snprintf(r.title, sizeof(r.title), "%.60s", txt);
        if (source_queue != NULL) xQueueSend(source_queue, &r, 0);
        wp_hide();
        return;
    }
    if (wp_stage == 2) {                       /* Lyrion server address */
        char host[64];
        const char *t = txt;
        while (*t == ' ') t++;
        if (strncasecmp(t, "http://", 7) == 0) t += 7;
        snprintf(host, sizeof(host), "%s", t);
        size_t hl = strlen(host);
        while (hl > 0 && (host[hl - 1] == '/' || host[hl - 1] == ' ')) host[--hl] = '\0';
        if (hl == 0) {
            wp_status_set("Type the server's address first.", UI_RED);
            return;
        }
        char base[80];
        snprintf(base, sizeof(base), strchr(host, ':') ? "http://%s" : "http://%s:9000", host);
        char msg[128];
        snprintf(msg, sizeof(msg), "Reading the players on %s...", base + 7);
        lms_msg_locked(msg, UI_TEXT_SOFT);
        wifi_send(SOURCE_LMS_USE, base, NULL);
        wp_hide();
        return;
    }
    if (wp_stage == 1) {                       /* typed network name -> ask for its password */
        char ssid[33];
        snprintf(ssid, sizeof(ssid), "%s", txt);
        if (ssid[0] == '\0') {
            wp_status_set("Type the network name first.", UI_RED);
            return;
        }
        wp_open(0, ssid, -1);
        return;
    }

    const size_t len = strlen(txt);
    if (wp_auth != WIFI_AUTH_OPEN && wp_auth != -1 && len == 0) {
        wp_status_set("This network needs a password.", UI_RED);
        return;
    }
    if (len > 0 && (len < 8 || len > 63)) {
        wp_status_set("Wi-Fi passwords are 8 to 63 characters.", UI_RED);
        return;
    }
    char msg[128];
    snprintf(msg, sizeof(msg), "Connecting to %s... (this can take up to 30 seconds)", wp_ssid);
    wifi_msg_locked(msg, UI_TEXT_SOFT);
    wifi_send(SOURCE_WIFI_CONNECT, wp_ssid, txt);
    lv_textarea_set_text(wp_textarea, "");    /* don't leave the password sitting on screen */
    wp_hide();
}

static void wp_connect_btn_cb(lv_event_t *e) { if (lv_event_get_code(e) == LV_EVENT_CLICKED) wp_submit(); }
static void wp_cancel_btn_cb(lv_event_t *e)  { if (lv_event_get_code(e) == LV_EVENT_CLICKED) wp_hide(); }
static void wp_keyboard_cb(lv_event_t *e)
{
    const lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_READY) wp_submit();
    else if (code == LV_EVENT_CANCEL) wp_hide();
}

static void wp_show_btn_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    const bool hidden = lv_textarea_get_password_mode(wp_textarea);
    lv_textarea_set_password_mode(wp_textarea, !hidden);
    lv_label_set_text(lv_obj_get_child(wp_show_btn, 0), hidden ? "HIDE" : "SHOW");
}

static void wp_create(void)
{
    wp_backdrop = lv_obj_create(lv_screen_active());
    lv_obj_remove_style_all(wp_backdrop);
    lv_obj_set_size(wp_backdrop, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(wp_backdrop, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(wp_backdrop, LV_OPA_80, 0);
    lv_obj_clear_flag(wp_backdrop, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(wp_backdrop, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *panel = lv_obj_create(wp_backdrop);
    lv_obj_remove_style_all(panel);
    lv_obj_set_size(panel, 996, 196);
    lv_obj_set_pos(panel, 14, 10);
    lv_obj_set_style_bg_color(panel, lv_color_hex(UI_PANEL), 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(panel, 14, 0);
    lv_obj_set_style_border_width(panel, 2, 0);
    lv_obj_set_style_border_color(panel, lv_color_hex(UI_AMBER), 0);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);

    wp_title = lv_label_create(panel);
    lv_obj_set_width(wp_title, 948);
    lv_label_set_long_mode(wp_title, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_color(wp_title, lv_color_hex(UI_TEXT), 0);
    lv_obj_set_style_text_font(wp_title, &p4_font_18, 0);
    label_one_line(wp_title);
    lv_obj_set_pos(wp_title, 24, 12);

    wp_textarea = lv_textarea_create(panel);
    lv_textarea_set_one_line(wp_textarea, true);
    lv_textarea_set_max_length(wp_textarea, 63);
    lv_obj_set_size(wp_textarea, 840, 54);
    lv_obj_set_pos(wp_textarea, 24, 40);
    lv_obj_set_style_text_font(wp_textarea, &p4_font_18, 0);
    lv_obj_set_style_bg_color(wp_textarea, lv_color_hex(UI_BG), 0);
    lv_obj_set_style_text_color(wp_textarea, lv_color_hex(UI_TEXT), 0);
    lv_obj_set_style_border_color(wp_textarea, lv_color_hex(UI_ACCENT), 0);
    lv_obj_set_style_border_width(wp_textarea, 2, 0);

    wp_show_btn = create_button(panel, "SHOW", 96, 54);
    lv_obj_set_pos(wp_show_btn, 876, 40);
    lv_obj_add_event_cb(wp_show_btn, wp_show_btn_cb, LV_EVENT_CLICKED, NULL);

    wp_status = lv_label_create(panel);
    lv_label_set_text(wp_status, "");
    lv_obj_set_width(wp_status, 620);
    lv_label_set_long_mode(wp_status, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(wp_status, &p4_font_14, 0);
    lv_obj_set_pos(wp_status, 24, 106);

    lv_obj_t *cancel = create_button(panel, "CANCEL", 140, 50);
    lv_obj_set_pos(cancel, 680, 132);
    lv_obj_add_event_cb(cancel, wp_cancel_btn_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *ok = create_button(panel, "CONNECT", 140, 50);
    wp_ok_btn = ok;
    lv_obj_set_pos(ok, 832, 132);
    lv_obj_set_style_bg_color(ok, lv_color_hex(UI_ACCENT), LV_PART_MAIN);
    lv_obj_set_style_text_color(lv_obj_get_child(ok, 0), lv_color_hex(UI_ON_ACCENT), 0);
    lv_obj_add_event_cb(ok, wp_connect_btn_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *kb = lv_keyboard_create(wp_backdrop);
    lv_obj_set_size(kb, 1004, 372);
    lv_obj_align(kb, LV_ALIGN_BOTTOM_MID, 0, -8);    /* see qs_create: override its own alignment */
    lv_obj_set_style_text_font(kb, &p4_font_18, LV_PART_ITEMS);
    lv_obj_set_style_pad_row(kb, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_column(kb, 8, LV_PART_MAIN);
    lv_keyboard_set_textarea(kb, wp_textarea);
    lv_obj_add_event_cb(kb, wp_keyboard_cb, LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(kb, wp_keyboard_cb, LV_EVENT_CANCEL, NULL);

    lv_obj_add_flag(wp_backdrop, LV_OBJ_FLAG_HIDDEN);
}

/* stage 0: password for `ssid` (auth -1 = unknown, may be open); stage 1: network name */
static void wp_open(int stage, const char *ssid, int auth)
{
    if (wp_backdrop == NULL) wp_create();
    wp_stage = stage;
    wp_auth = auth;
    snprintf(wp_ssid, sizeof(wp_ssid), "%s", ssid ? ssid : "");

    char title[96];
    if (stage == 4) {
        snprintf(title, sizeof(title), "%.60s", wp_ssid[0] ? wp_ssid : "SEARCH");
        lv_textarea_set_password_mode(wp_textarea, false);
        lv_obj_add_flag(wp_show_btn, LV_OBJ_FLAG_HIDDEN);
        wp_status_set("Type what to look for - then tap SEARCH or the keyboard's tick.", UI_TEXT_SOFT);
    } else if (stage == 3) {
        snprintf(title, sizeof(title), "SEARCH THE LIBRARY");
        lv_textarea_set_password_mode(wp_textarea, false);
        lv_obj_add_flag(wp_show_btn, LV_OBJ_FLAG_HIDDEN);
        wp_status_set("Part of an artist, album or song name - then tap SEARCH or the keyboard's tick.",
                      UI_TEXT_SOFT);
    } else if (stage == 2) {
        snprintf(title, sizeof(title), "LYRION SERVER ADDRESS");
        lv_textarea_set_password_mode(wp_textarea, false);
        lv_obj_add_flag(wp_show_btn, LV_OBJ_FLAG_HIDDEN);
        wp_status_set("For example 192.168.1.20 or 192.168.1.20:9000 (use the keyboard's 1# key "
                      "for numbers).", UI_TEXT_SOFT);
    } else if (stage == 1) {
        snprintf(title, sizeof(title), "NETWORK NAME (SSID)");
        lv_textarea_set_password_mode(wp_textarea, false);
        lv_obj_add_flag(wp_show_btn, LV_OBJ_FLAG_HIDDEN);
        wp_status_set("Type the exact network name, including capitals.", UI_TEXT_SOFT);
    } else {
        snprintf(title, sizeof(title), "PASSWORD FOR  %s", wp_ssid);
        lv_textarea_set_password_mode(wp_textarea, true);
        lv_obj_clear_flag(wp_show_btn, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(lv_obj_get_child(wp_show_btn, 0), "SHOW");
        wp_status_set(auth == -1 ? "Leave empty if the network is open."
                                 : "If it can't connect, the display goes back to the current network.",
                      UI_TEXT_SOFT);
    }
    lv_label_set_text(wp_title, title);
    if (wp_ok_btn != NULL) {
        lv_label_set_text(lv_obj_get_child(wp_ok_btn, 0),
                          (stage == 3 || stage == 4) ? "SEARCH" : stage == 2 ? "USE" : "CONNECT");
    }
    lv_textarea_set_text(wp_textarea, "");
    lv_obj_clear_flag(wp_backdrop, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(wp_backdrop);
}

/* ---- the Wi-Fi screen itself ---- */
static void wifi_create(void)
{
    wifi_overlay = lv_obj_create(lv_screen_active());
    lv_obj_remove_style_all(wifi_overlay);
    lv_obj_set_size(wifi_overlay, PANEL_W, PANEL_H);
    lv_obj_set_pos(wifi_overlay, PANEL_X, PANEL_Y);
    lv_obj_set_style_bg_color(wifi_overlay, lv_color_hex(UI_PANEL), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(wifi_overlay, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(wifi_overlay, 2, LV_PART_MAIN);
    lv_obj_set_style_border_color(wifi_overlay, lv_color_hex(UI_AMBER), LV_PART_MAIN);
    lv_obj_set_style_radius(wifi_overlay, 16, LV_PART_MAIN);
    lv_obj_clear_flag(wifi_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(wifi_overlay, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *title = lv_label_create(wifi_overlay);
    lv_label_set_text(title, "WI-FI");
    lv_obj_set_style_text_color(title, lv_color_hex(UI_TEXT), 0);
    lv_obj_set_style_text_font(title, &p4_font_28, 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 30, 20);

    lv_obj_t *close = create_button(wifi_overlay, "CLOSE", 150, 50);
    lv_obj_align(close, LV_ALIGN_TOP_RIGHT, -30, 14);
    ui_add_settings_back(wifi_overlay);
    lv_obj_add_event_cb(close, wifi_close_btn_cb, LV_EVENT_CLICKED, NULL);

    wifi_cur_label = lv_label_create(wifi_overlay);
    lv_obj_set_width(wifi_cur_label, 870);
    lv_label_set_long_mode(wifi_cur_label, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_color(wifi_cur_label, lv_color_hex(UI_ACCENT_GLOW), 0);
    lv_obj_set_style_text_font(wifi_cur_label, &p4_font_16, 0);
    label_one_line(wifi_cur_label);
    lv_obj_set_pos(wifi_cur_label, 30, 80);

    wifi_msg_label = lv_label_create(wifi_overlay);
    lv_label_set_text(wifi_msg_label, "");
    lv_obj_set_width(wifi_msg_label, 870);
    lv_label_set_long_mode(wifi_msg_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(wifi_msg_label, &p4_font_14, 0);
    lv_obj_set_pos(wifi_msg_label, 30, 106);

    wifi_list = lv_obj_create(wifi_overlay);
    lv_obj_remove_style_all(wifi_list);
    lv_obj_set_size(wifi_list, 890, PANEL_H - 142 - 76);
    lv_obj_set_pos(wifi_list, 20, 142);
    lv_obj_add_flag(wifi_list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(wifi_list, LV_DIR_VER);
    lv_obj_set_style_bg_color(wifi_list, lv_color_hex(UI_ACCENT), LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(wifi_list, LV_OPA_70, LV_PART_SCROLLBAR);
    lv_obj_set_style_width(wifi_list, 6, LV_PART_SCROLLBAR);

    lv_obj_t *scan = create_button(wifi_overlay, LV_SYMBOL_REFRESH "  SCAN", 150, 50);
    lv_obj_set_pos(scan, 30, PANEL_H - 66);
    lv_obj_set_style_bg_color(scan, lv_color_hex(UI_ACCENT), LV_PART_MAIN);
    lv_obj_set_style_text_color(lv_obj_get_child(scan, 0), lv_color_hex(UI_ON_ACCENT), 0);
    lv_obj_add_event_cb(scan, wifi_scan_btn_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *other = create_button(wifi_overlay, "OTHER NETWORK", 210, 50);
    lv_obj_set_pos(other, 196, PANEL_H - 66);
    lv_obj_add_event_cb(other, wifi_other_btn_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *server = create_button(wifi_overlay, "LYRION SERVER", 270, 50);
    lv_obj_set_pos(server, 422, PANEL_H - 66);
    lv_obj_add_event_cb(server, lms_open_btn_cb, LV_EVENT_CLICKED, NULL);

    wifi_builtin_btn = create_button(wifi_overlay, "USE BUILT-IN", 190, 50);
    lv_obj_set_pos(wifi_builtin_btn, 710, PANEL_H - 66);
    lv_obj_add_event_cb(wifi_builtin_btn, wifi_builtin_btn_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_add_flag(wifi_overlay, LV_OBJ_FLAG_HIDDEN);
}

static void wifi_open(void)
{
    if (wifi_overlay == NULL) wifi_create();
    wifi_refresh_current_locked();
    wifi_rebuild_list_locked();
    if (!wifi_busy) {
        wifi_msg_locked(wifi_net_count ? "" : "Tap SCAN to look for networks (2.4 GHz only).",
                        UI_TEXT_SOFT);
    }
    ui_show_panel(wifi_overlay);
}

/* ---- worker side (runs in the source browser task) ---- */
static int wifi_net_cmp(const void *a, const void *b)
{
    return ((const wifi_net_t *)b)->rssi - ((const wifi_net_t *)a)->rssi;
}

typedef struct { esp_err_t err; int count; } wifi_scan_ctx_t;
static void wifi_scan_result_job(void *arg)
{
    const wifi_scan_ctx_t *c = (const wifi_scan_ctx_t *)arg;
    if (c->err == ESP_OK) {
        wifi_net_count = c->count;
        wifi_rebuild_list_locked();
        wifi_msg_locked(c->count ? "Tap a network to connect to it."
                                 : "No networks found. Only 2.4 GHz networks can be seen.",
                        UI_TEXT_SOFT);
    } else {
        wifi_msg_locked("Scan failed - the Wi-Fi may be busy reconnecting. Try again in a moment.",
                        UI_RED);
    }
    wifi_refresh_current_locked();
}

static void wifi_do_scan(void)
{
    static wifi_ap_record_t recs[WIFI_MAX_NETS * 2];
    wifi_scan_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.show_hidden = false;

    /* While offline the reconnect attempts keep the radio busy and a scan
     * would fail - pause them for the scan. */
    const bool offline = (xEventGroupGetBits(wifi_event_group) & WIFI_CONNECTED_BIT) == 0;
    if (offline) {
        wifi_manual = true;
        if (wifi_reconnect_timer != NULL) esp_timer_stop(wifi_reconnect_timer);
        esp_wifi_disconnect();
        vTaskDelay(pdMS_TO_TICKS(300));
    }

    esp_err_t err = esp_wifi_scan_start(&cfg, true);
    uint16_t n = sizeof(recs) / sizeof(recs[0]);
    if (err == ESP_OK) {
        err = esp_wifi_scan_get_ap_records(&n, recs);     /* also frees the driver's list */
    }

    int count = 0;
    if (err == ESP_OK) {
        for (int i = 0; i < n && count < WIFI_MAX_NETS; i++) {
            const char *ssid = (const char *)recs[i].ssid;
            if (ssid[0] == '\0') continue;               /* hidden network */
            int j;
            for (j = 0; j < count; j++) {                /* one row per name, strongest kept */
                if (strcmp(wifi_nets[j].ssid, ssid) == 0) break;
            }
            if (j < count) {
                if (recs[i].rssi > wifi_nets[j].rssi) wifi_nets[j].rssi = recs[i].rssi;
                continue;
            }
            snprintf(wifi_nets[count].ssid, sizeof(wifi_nets[count].ssid), "%s", ssid);
            wifi_nets[count].rssi = recs[i].rssi;
            wifi_nets[count].auth = recs[i].authmode;
            count++;
        }
        qsort(wifi_nets, count, sizeof(wifi_nets[0]), wifi_net_cmp);
    }

    if (offline) {                          /* resume trying to connect */
        wifi_manual = false;
        esp_wifi_connect();
    }
    ESP_LOGI(TAG, "Wi-Fi scan: %s, %d network(s)", esp_err_to_name(err), count);
    wifi_scan_ctx_t ctx = { .err = err, .count = count };
    ui_call(wifi_scan_result_job, &ctx);
    wifi_busy = false;
}

