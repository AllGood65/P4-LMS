/*
 * P4-LMS - Navigation: bottom tab bar, room chip, room list, Settings hub,
 * and short notifications ("toasts")
 *
 * Part of main.c.  This file is #included by main.c (in order) and is not
 * compiled on its own, so it shares main.c's includes and static names.
 *
 * Tabs:  Now Playing | Library | Queue | Rooms | Settings
 * The tabs open the existing screens (the SOURCE browser, its Queue view, the
 * Settings / Wi-Fi / server screens), which now stop just above the bar.  A
 * small timer keeps the highlighted tab in step with whatever is showing, so
 * CLOSE, BACK or the idle timeout on any screen keep the bar correct.
 */
#include "esp_app_desc.h"

enum { TAB_NOW = 0, TAB_LIBRARY, TAB_QUEUE, TAB_ROOMS, TAB_SETTINGS, TAB_COUNT };

static lv_obj_t *nav_bar = NULL;
static lv_obj_t *nav_tabs[TAB_COUNT];
static int       nav_tab_shown = -1;

static lv_obj_t *room_chip = NULL;
static lv_obj_t *room_chip_label = NULL;
static lv_obj_t *room_chip_dot = NULL;

static lv_obj_t *rooms_overlay = NULL;

/* One card per room. */
typedef struct {
    lv_obj_t *card, *cover, *name, *dot, *state_lbl, *selected_lbl;
    lv_obj_t *artist, *title, *play_btn, *vol_slider, *vol_lbl;
    lv_obj_t *no_sw_lbl;
    lv_obj_t *sys_row, *sys_sw;      /* "System power": the room's amp (+ pre-amp, in order) */
    lv_obj_t *sys_note;              /* what it switches, e.g. "Pre-amp, then amp - 3 s apart" */
    lv_obj_t *linked_lbl;            /* "Linked with ..." when in a sync group */
    lv_image_dsc_t cover_dsc;
    uint8_t  *cover_pixels;
    int       state;                 /* as in room_msg_t */
    uint32_t  vol_touch, play_touch;
    uint32_t  sys_touch, sys_hold;   /* after a System tap, ignore refreshes for sys_hold ms */
    int       sel_shown;             /* -1 = not drawn yet */
} room_card_t;
static room_card_t rooms_cards[LMS_MAX_PLAYERS];
static int rooms_group_mask[LMS_MAX_PLAYERS];   /* from Lyrion: who plays with whom */

static lv_obj_t *hub_overlay = NULL;
static lv_obj_t *hub_subtitles[8];

static lv_obj_t *toast = NULL;
static lv_obj_t *toast_label = NULL;
static lv_timer_t *toast_timer = NULL;

/* ------------------------------------------------------------------ toast */
static void toast_hide_cb(lv_timer_t *t)
{
    (void)t;
    if (toast != NULL) lv_obj_add_flag(toast, LV_OBJ_FLAG_HIDDEN);
}

/* Show a short message above the bar for a few seconds (LVGL task only). */
static void ui_toast(const char *text)
{
    if (toast == NULL || text == NULL) return;
    lv_label_set_text(toast_label, text);
    lv_obj_clear_flag(toast, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(toast);
    if (toast_timer == NULL) {
        toast_timer = lv_timer_create(toast_hide_cb, 2800, NULL);
        lv_timer_set_repeat_count(toast_timer, -1);
    }
    lv_timer_reset(toast_timer);
    lv_timer_resume(toast_timer);
}

typedef struct { char text[96]; } toast_msg_t;
static void ui_toast_job(void *arg) { ui_toast(((const toast_msg_t *)arg)->text); }

/* The same, from any task. */
static void ui_toast_post(const char *text)
{
    toast_msg_t m;
    snprintf(m.text, sizeof(m.text), "%s", text);
    ui_post(ui_toast_job, &m, sizeof(m));
}

static void toast_create(lv_obj_t *screen)
{
    toast = lv_obj_create(screen);
    lv_obj_remove_style_all(toast);
    lv_obj_set_size(toast, LV_SIZE_CONTENT, 42);
    lv_obj_set_style_pad_hor(toast, 22, 0);
    lv_obj_set_style_bg_color(toast, lv_color_hex(UI_TEXT), 0);
    lv_obj_set_style_bg_opa(toast, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(toast, 21, 0);
    lv_obj_clear_flag(toast, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(toast, LV_ALIGN_BOTTOM_MID, 0, -(NAV_BAR_H + 14));
    toast_label = lv_label_create(toast);
    lv_obj_set_style_text_color(toast_label, lv_color_hex(UI_BG), 0);
    lv_obj_set_style_text_font(toast_label, &p4_font_16, 0);
    lv_obj_center(toast_label);
    lv_obj_add_flag(toast, LV_OBJ_FLAG_HIDDEN);
}

/* ---------------------------------------------------------- room chip */
static void rooms_show(void);

static void nav_room_chip_refresh(void)
{
    if (room_chip_label == NULL) return;
    const int i = current_player_index;
    char text[48];
    snprintf(text, sizeof(text), "%s  " LV_SYMBOL_DOWN,
             (i >= 0 && i < lyrion_player_count) ? lyrion_players[i].short_name : "No players");
    lv_label_set_text(room_chip_label, text);
    lv_obj_set_style_bg_color(room_chip_dot,
        lv_color_hex(current_player_playing ? UI_ACCENT : UI_TEXT_DIM), 0);
}

static void room_chip_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) rooms_show();
}

/* ------------------------------------------------------------- sleep timer */
static lv_obj_t *sleep_chip = NULL, *sleep_chip_label = NULL;
static lv_obj_t *sleep_overlay = NULL, *sleep_panel = NULL, *sleep_status_label = NULL;
static lv_obj_t *sleep_back_btn = NULL;     /* only when opened from Settings */
static int sleep_chip_minutes_shown = -2;

/* Seconds left on the selected room's sleep timer, counted down locally
 * between status replies. */
static int sleep_left_now(void)
{
    if (sleep_left_player != current_player_index || sleep_left_s <= 0) return 0;
    const int64_t gone_s = (esp_timer_get_time() / 1000 - sleep_left_at_ms) / 1000;
    const int left = sleep_left_s - (int)gone_s;
    return left > 0 ? left : 0;
}

static void sleep_refresh_locked(void)
{
    const int left = sleep_left_now();
    const int mins = left > 0 ? (left + 59) / 60 : 0;
    if (sleep_chip_label != NULL && mins != sleep_chip_minutes_shown) {
        sleep_chip_minutes_shown = mins;
        char t[32];
        if (mins > 0) snprintf(t, sizeof(t), "SLEEP  %d min", mins);
        else          snprintf(t, sizeof(t), "SLEEP");
        lv_label_set_text(sleep_chip_label, t);
        lv_obj_set_style_text_color(sleep_chip_label, lv_color_hex(mins > 0 ? UI_ACCENT : UI_TEXT_DIM), 0);
        lv_obj_set_style_border_color(sleep_chip, lv_color_hex(mins > 0 ? UI_ACCENT : UI_EDGE), 0);
        lv_obj_set_style_bg_color(sleep_chip, lv_color_hex(mins > 0 ? UI_ACCENT_DARK : UI_BUTTON), 0);
    }
    if (sleep_status_label != NULL && sleep_overlay != NULL &&
        !lv_obj_has_flag(sleep_overlay, LV_OBJ_FLAG_HIDDEN)) {
        char t[80];
        if (left <= 0) {
            snprintf(t, sizeof(t), "No sleep timer set.");
        } else {
            const time_t end = time(NULL) + left;
            struct tm tmv;
            localtime_r(&end, &tmv);
            char clock[16] = "";
            if (tmv.tm_year > 120) {                     /* clock is set (NTP) */
                strftime(clock, sizeof(clock), "%I:%M %p", &tmv);
            }
            snprintf(t, sizeof(t), "Stops in %d min%s%s%s", mins, clock[0] ? "  (at " : "",
                     clock[0] == '0' ? clock + 1 : clock, clock[0] ? ")" : "");
        }
        lv_label_set_text(sleep_status_label, t);
    }
}

#define SLEEP_NVS_AMP "sleep_amp"
static lv_obj_t *sleep_amp_sw = NULL;

/* The timer ended on the room this display shows: go straight to the
 * screensaver instead of waiting the usual few idle minutes. */
static void sleep_screensaver_job(void *arg)
{
    (void)arg;
    if (screensaver_active) return;
    screensaver_show(screensaver_paused_view());      /* the music has stopped */
    screensaver_preview = true;     /* stay up until touched or music starts, even if the
                                       status still says "playing" for a moment */
}

/* Runs on the command task every few seconds.  Once a room's sleep timer has
 * run out and its player is really off (or stopped): the screensaver comes
 * on (if that room is the one on screen) and, if enabled, the room's amp and
 * pre-amp are switched off through Home Assistant. */
static void sleep_amp_tick(void)
{
    const int room = sleep_amp_room;
    if (room < 0) return;
    const int64_t now = esp_timer_get_time() / 1000;
    if (now < sleep_amp_due_ms) return;
    if (room >= lyrion_player_count) {
        sleep_amp_room = -1;
        return;
    }
    if (now > sleep_amp_due_ms + 90000) {                  /* still playing: leave it */
        ESP_LOGI(TAG, "Sleep timer on %s: player still playing - amp left on", lyrion_players[room].name);
        sleep_amp_room = -1;
        return;
    }

    char json[160];
    snprintf(json, sizeof(json),
             "{\"id\":1,\"method\":\"slim.request\",\"params\":[\"%s\",[\"status\",0,0]]}",
             lyrion_players[room].id);
    char *reply = heap_caps_malloc(4096, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (reply == NULL) return;
    bool off = false;
    if (http_post_json(LYRION_SERVER_URL, json, reply, 4096)) {
        cJSON *root = cJSON_Parse(reply);
        cJSON *res = root ? cJSON_GetObjectItem(root, "result") : NULL;
        if (cJSON_IsObject(res)) {
            cJSON *pw = cJSON_GetObjectItem(res, "power");
            cJSON *md = cJSON_GetObjectItem(res, "mode");
            const int power = cJSON_IsNumber(pw) ? pw->valueint
                            : (cJSON_IsString(pw) && pw->valuestring ? atoi(pw->valuestring) : 1);
            const bool playing = cJSON_IsString(md) && md->valuestring && strcmp(md->valuestring, "play") == 0;
            off = (power == 0) || !playing;
        }
        if (root) cJSON_Delete(root);
    }
    heap_caps_free(reply);
    if (!off) return;                                       /* check again shortly */

    sleep_amp_room = -1;
    if (room == current_player_index) ui_post(sleep_screensaver_job, NULL, 0);
    if (!sleep_amp_enabled || !HA_TOKEN_SET) return;
    const ha_player_switches_t *sw = ha_switches_for_index(room);
    if (sw == NULL || (sw->amp_entity == NULL && sw->preamp_entity == NULL)) return;
    ESP_LOGI(TAG, "Sleep timer ended on %s - switching the amp off", lyrion_players[room].name);
    ha_power_sequence(sw, false);            /* amp off first, then the pre-amp */
    char msg[80];
    snprintf(msg, sizeof(msg), "Sleep timer: %s amp switched off", lyrion_players[room].short_name);
    ui_toast_post(msg);
}

static void sleep_amp_switch_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    lv_obj_t *sw = (lv_obj_t *)lv_event_get_target(e);
    sleep_amp_enabled = lv_obj_has_state(sw, LV_STATE_CHECKED);
    nvs_handle_t nvs;
    if (nvs_open(PLAYER_NVS_NAMESPACE, NVS_READWRITE, &nvs) == ESP_OK) {
        nvs_set_u8(nvs, SLEEP_NVS_AMP, sleep_amp_enabled ? 1 : 0);
        nvs_commit(nvs);
        nvs_close(nvs);
    }
    ui_toast(sleep_amp_enabled ? (HA_TOKEN_SET ? "The amp will switch off when the timer ends"
                                               : "Set the Home Assistant token first (web page)")
                               : "The amp stays on when the timer ends");
}

static void sleep_set(int seconds)
{
    if (!queue_lyrion_command(LYRION_COMMAND_SLEEP, seconds)) {
        ui_toast("Couldn't send that - try again");
        return;
    }
    /* Show it straight away; the next status reply confirms it. */
    sleep_left_s = seconds;
    sleep_left_at_ms = esp_timer_get_time() / 1000;
    sleep_left_player = current_player_index;
    if (seconds > 0) {
        sleep_amp_room = current_player_index;
        sleep_amp_due_ms = sleep_left_at_ms + (int64_t)seconds * 1000 + 3000;
    } else if (sleep_amp_room == current_player_index) {
        sleep_amp_room = -1;
    }
    sleep_chip_minutes_shown = -2;
    sleep_refresh_locked();
    char msg[64];
    if (seconds > 0) snprintf(msg, sizeof(msg), "Sleep timer: %d min", (seconds + 59) / 60);
    else             snprintf(msg, sizeof(msg), "Sleep timer off");
    ui_toast(msg);
}

#define SLEEP_END_OF_SONG (-1)
static void sleep_option_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    const int minutes = (int)(intptr_t)lv_event_get_user_data(e);
    if (minutes == SLEEP_END_OF_SONG) {
        if (!playback_valid || !current_player_playing || playback_duration_s <= 0.0) {
            ui_toast("Nothing with an end is playing");
            return;
        }
        const double elapsed = playback_elapsed_s + (double)lv_tick_elaps(playback_sync_tick) / 1000.0;
        const int left = (int)(playback_duration_s - elapsed + 0.5);
        if (left < 5) {
            ui_toast("This song is just ending");
            return;
        }
        sleep_set(left);
    } else {
        sleep_set(minutes * 60);
    }
    if (sleep_overlay != NULL) lv_obj_add_flag(sleep_overlay, LV_OBJ_FLAG_HIDDEN);
}

static void sleep_off_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    sleep_set(0);
    if (sleep_overlay != NULL) lv_obj_add_flag(sleep_overlay, LV_OBJ_FLAG_HIDDEN);
}

static void sleep_picker_open(void);

static void sleep_chip_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) sleep_picker_open();
}

static void sleep_chip_create(lv_obj_t *header)
{
    sleep_chip = lv_obj_create(header);
    lv_obj_remove_style_all(sleep_chip);
    lv_obj_set_size(sleep_chip, 170, 40);
    lv_obj_align(sleep_chip, LV_ALIGN_LEFT_MID, 616, 0);        /* between the clock and Wi-Fi */
    lv_obj_set_style_bg_opa(sleep_chip, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(sleep_chip, 20, 0);
    lv_obj_set_style_border_width(sleep_chip, 1, 0);
    lv_obj_set_style_translate_y(sleep_chip, 2, LV_STATE_PRESSED);
    lv_obj_clear_flag(sleep_chip, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(sleep_chip, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(sleep_chip, 8);
    lv_obj_add_event_cb(sleep_chip, sleep_chip_event_cb, LV_EVENT_CLICKED, NULL);
    sleep_chip_label = lv_label_create(sleep_chip);
    lv_obj_set_style_text_font(sleep_chip_label, &p4_font_16, 0);
    lv_obj_center(sleep_chip_label);
    sleep_refresh_locked();
}

static void nav_room_chip_create(lv_obj_t *header)
{
    sleep_chip_create(header);

    room_chip = lv_obj_create(header);
    lv_obj_remove_style_all(room_chip);
    lv_obj_set_size(room_chip, 240, 40);
    lv_obj_align(room_chip, LV_ALIGN_LEFT_MID, 16, 0);
    lv_obj_set_style_bg_color(room_chip, lv_color_hex(UI_BUTTON), 0);
    lv_obj_set_style_bg_opa(room_chip, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(room_chip, 20, 0);
    lv_obj_set_style_border_width(room_chip, 1, 0);
    lv_obj_set_style_border_color(room_chip, lv_color_hex(UI_EDGE), 0);
    lv_obj_set_style_bg_color(room_chip, lv_color_hex(UI_ACCENT_DARK), LV_STATE_PRESSED);
    lv_obj_clear_flag(room_chip, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(room_chip, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(room_chip, 8);
    lv_obj_add_event_cb(room_chip, room_chip_event_cb, LV_EVENT_CLICKED, NULL);

    room_chip_dot = lv_obj_create(room_chip);
    lv_obj_remove_style_all(room_chip_dot);
    lv_obj_set_size(room_chip_dot, 10, 10);
    lv_obj_set_style_radius(room_chip_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(room_chip_dot, LV_OPA_COVER, 0);
    lv_obj_align(room_chip_dot, LV_ALIGN_LEFT_MID, 16, 0);

    room_chip_label = lv_label_create(room_chip);
    lv_obj_set_width(room_chip_label, 190);
    lv_label_set_long_mode(room_chip_label, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_color(room_chip_label, lv_color_hex(UI_TEXT), 0);
    lv_obj_set_style_text_font(room_chip_label, &p4_font_18, 0);
    label_one_line(room_chip_label);
    lv_obj_align(room_chip_label, LV_ALIGN_LEFT_MID, 36, 0);
    nav_room_chip_refresh();
}

/* ------------------------------------------------------------- Rooms cards */
#define ROOM_TOUCH_HOLD_MS 4000     /* after a touch, ignore refreshes this long */

static lv_obj_t *group_overlay;              /* the GROUP dialog (defined below) */

static void rooms_hide(void)
{
    rooms_open = false;
    if (rooms_overlay != NULL) lv_obj_add_flag(rooms_overlay, LV_OBJ_FLAG_HIDDEN);
    if (group_overlay != NULL) lv_obj_add_flag(group_overlay, LV_OBJ_FLAG_HIDDEN);
}

static void room_show_state(room_card_t *c, int state)
{
    c->state = state;
    lv_obj_set_style_bg_color(c->dot, lv_color_hex(state == 2 ? UI_ACCENT : UI_TEXT_DIM), 0);
    lv_label_set_text(c->state_lbl, state == 2 ? "Playing" : state == 1 ? "Paused" : "Off");
    lv_label_set_text(lv_obj_get_child(c->play_btn, 0), state == 2 ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
    lv_obj_set_style_bg_color(c->play_btn, lv_color_hex(state == 2 ? UI_ACCENT : UI_BUTTON), LV_PART_MAIN);
    lv_obj_set_style_text_color(lv_obj_get_child(c->play_btn, 0),
                                lv_color_hex(state == 2 ? UI_ON_ACCENT : UI_TEXT), 0);
}

/* The selected room gets a bright border and a "Selected" note. */
static void rooms_refresh_locked(void)
{
    if (rooms_overlay == NULL || lv_obj_has_flag(rooms_overlay, LV_OBJ_FLAG_HIDDEN)) return;
    for (int i = 0; i < lyrion_player_count; i++) {
        room_card_t *c = &rooms_cards[i];
        if (c->card == NULL) continue;
        const int sel = (i == current_player_index);
        if (c->sel_shown == sel) continue;
        c->sel_shown = sel;
        lv_obj_set_style_border_color(c->card, lv_color_hex(sel ? UI_ACCENT : UI_EDGE), 0);
        lv_obj_set_style_border_width(c->card, sel ? 3 : 1, 0);
        if (sel) lv_obj_clear_flag(c->selected_lbl, LV_OBJ_FLAG_HIDDEN);
        else     lv_obj_add_flag(c->selected_lbl, LV_OBJ_FLAG_HIDDEN);
    }
}

/* "Linked with Office, Basement" under a grouped room's name. */
static void rooms_set_linked_label(int i)
{
    room_card_t *c = &rooms_cards[i];
    if (c->linked_lbl == NULL) return;
    const int others = rooms_group_mask[i] & ~(1 << i);
    if (others == 0) {
        lv_label_set_text(c->linked_lbl, "");
        return;
    }
    char text[96] = "Linked with ";
    bool first = true;
    for (int k = 0; k < lyrion_player_count; k++) {
        if (!(others & (1 << k))) continue;
        if (!first) strncat(text, ", ", sizeof(text) - strlen(text) - 1);
        strncat(text, lyrion_players[k].short_name, sizeof(text) - strlen(text) - 1);
        first = false;
    }
    lv_label_set_text(c->linked_lbl, text);
}

/* Fresh status for one room (from the command task). */
static void rooms_card_job(void *arg)
{
    const room_msg_t *m = (const room_msg_t *)arg;
    if (m->idx < 0 || m->idx >= lyrion_player_count) return;
    room_card_t *c = &rooms_cards[m->idx];
    if (c->card == NULL) return;

    if (lv_tick_elaps(c->play_touch) > 2500) {      /* not right after a play/pause tap */
        room_show_state(c, m->state);
    }
    rooms_group_mask[m->idx] = m->group_mask;
    rooms_set_linked_label(m->idx);
    lv_label_set_text(c->artist, m->state == 0 ? "" : m->artist);
    lv_label_set_text(c->title, m->state == 0 ? "Not available" : (m->title[0] ? m->title : "Nothing queued"));

    if (m->volume >= 0 && lv_tick_elaps(c->vol_touch) > ROOM_TOUCH_HOLD_MS) {
        lv_slider_set_value(c->vol_slider, m->volume, LV_ANIM_OFF);
        char v[8];
        snprintf(v, sizeof(v), "%d%%", m->volume);
        lv_label_set_text(c->vol_lbl, v);
    }

    const bool any = (m->amp != ROOM_SWITCH_NONE || m->pre != ROOM_SWITCH_NONE);
    if (any) lv_obj_add_flag(c->no_sw_lbl, LV_OBJ_FLAG_HIDDEN);
    else     lv_obj_clear_flag(c->no_sw_lbl, LV_OBJ_FLAG_HIDDEN);

    /* One "System power" switch: on only when everything it switches is on. */
    if (!any) {
        lv_obj_add_flag(c->sys_row, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(c->sys_note, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_clear_flag(c->sys_row, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(c->sys_note, LV_OBJ_FLAG_HIDDEN);
    const bool known = (m->amp == ROOM_SWITCH_NONE || m->amp >= 0) &&
                       (m->pre == ROOM_SWITCH_NONE || m->pre >= 0);
    if (known && lv_tick_elaps(c->sys_touch) > c->sys_hold) {
        const bool all_on = (m->amp == ROOM_SWITCH_NONE || m->amp == 1) &&
                            (m->pre == ROOM_SWITCH_NONE || m->pre == 1);
        if (all_on) lv_obj_add_state(c->sys_sw, LV_STATE_CHECKED);
        else        lv_obj_clear_state(c->sys_sw, LV_STATE_CHECKED);
    }

    const ha_player_switches_t *hs = ha_switches_for_index(m->idx);
    char note[64];
    if (hs->amp_entity != NULL && hs->preamp_entity != NULL) {
        if (hs->delay_s == 0) snprintf(note, sizeof(note), "%s together",
                                       hs->amp_first ? "Amp + pre-amp" : "Pre-amp + amp");
        else snprintf(note, sizeof(note), "%s on first, %u s apart",
                      hs->amp_first ? "Amp" : "Pre-amp", (unsigned)hs->delay_s);
    } else {
        snprintf(note, sizeof(note), "%s", hs->amp_entity != NULL ? "Amp" : "Pre-amp");
    }
    if (strcmp(lv_label_get_text(c->sys_note), note) != 0) lv_label_set_text(c->sys_note, note);
}

/* A new cover for one room (the job now owns the pixels). */
static void rooms_cover_job(void *arg)
{
    const room_cover_msg_t *m = (const room_cover_msg_t *)arg;
    if (m->idx < 0 || m->idx >= lyrion_player_count || rooms_cards[m->idx].cover == NULL) {
        heap_caps_free(m->pixels);
        return;
    }
    room_card_t *c = &rooms_cards[m->idx];
    if (c->cover_pixels != NULL) heap_caps_free(c->cover_pixels);
    c->cover_pixels = m->pixels;
    memset(&c->cover_dsc, 0, sizeof(c->cover_dsc));
    c->cover_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
    c->cover_dsc.header.w = m->w;
    c->cover_dsc.header.h = m->h;
    c->cover_dsc.header.stride = m->padded * 2U;
    c->cover_dsc.data_size = m->size;
    c->cover_dsc.data = c->cover_pixels;
    lv_image_set_src(c->cover, &c->cover_dsc);
    lv_obj_clear_flag(c->cover, LV_OBJ_FLAG_HIDDEN);
}

static void player_select(int index);

static void room_card_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    player_select((int)(intptr_t)lv_event_get_user_data(e));
    rooms_refresh_locked();
}

static void room_play_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    const int i = (int)(intptr_t)lv_event_get_user_data(e);
    room_card_t *c = &rooms_cards[i];
    const bool pause = (c->state == 2);
    if (queue_lyrion_command_for(i, pause ? LYRION_COMMAND_PAUSE : LYRION_COMMAND_PLAY, 0)) {
        room_show_state(c, pause ? 1 : 2);           /* show it straight away */
        c->play_touch = lv_tick_get();
    }
}

static void room_volume_event_cb(lv_event_t *e)
{
    const lv_event_code_t code = lv_event_get_code(e);
    const int i = (int)(intptr_t)lv_event_get_user_data(e);
    room_card_t *c = &rooms_cards[i];
    const int v = lv_slider_get_value(c->vol_slider);
    c->vol_touch = lv_tick_get();
    if (code == LV_EVENT_VALUE_CHANGED) {
        char t[8];
        snprintf(t, sizeof(t), "%d%%", v);
        lv_label_set_text(c->vol_lbl, t);
    } else if (code == LV_EVENT_RELEASED) {
        queue_lyrion_command_for(i, LYRION_COMMAND_VOLUME, v);
    }
}

/* "System power": amp + pre-amp in the room's order. */
static void room_system_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    const int i = (int)(intptr_t)lv_event_get_user_data(e);
    room_card_t *c = &rooms_cards[i];
    lv_obj_t *sw = c->sys_sw;
    const bool on = lv_obj_has_state(sw, LV_STATE_CHECKED);
    const ha_player_switches_t *hs = ha_switches_for_index(i);
    if ((hs->amp_entity == NULL && hs->preamp_entity == NULL) || !HA_TOKEN_SET || !queue_ha_system(i, on)) {
        if (on) lv_obj_clear_state(sw, LV_STATE_CHECKED); else lv_obj_add_state(sw, LV_STATE_CHECKED);
        ui_toast(HA_TOKEN_SET ? "Couldn't reach Home Assistant" : "Set the Home Assistant token on the web page");
        return;
    }
    /* Polls can't flip the switch back until the second switch has had
     * time to follow. */
    const bool both = (hs->amp_entity != NULL && hs->preamp_entity != NULL);
    c->sys_touch = lv_tick_get();
    c->sys_hold = (both ? (uint32_t)hs->delay_s * 1000u : 0u) + ROOM_TOUCH_HOLD_MS;
    if (i == current_player_index) {          /* the Now Playing power button follows */
        np_power_want = on;
        np_power_touch = c->sys_touch;
        np_power_hold = c->sys_hold + 1000u;
        np_power_apply_locked();
    }

    const bool pre_leads = (on != hs->amp_first);
    char msg[96];
    if (!both) {
        snprintf(msg, sizeof(msg), "%s: %s %s", lyrion_players[i].short_name,
                 hs->amp_entity != NULL ? "amp" : "pre-amp", on ? "on" : "off");
    } else if (hs->delay_s == 0) {
        snprintf(msg, sizeof(msg), "%s: %s, then %s %s", lyrion_players[i].short_name,
                 pre_leads ? "pre-amp" : "amp", pre_leads ? "amp" : "pre-amp", on ? "on" : "off");
    } else {
        snprintf(msg, sizeof(msg), "%s: %s %s, %s in %u s", lyrion_players[i].short_name,
                 pre_leads ? "pre-amp" : "amp", on ? "on" : "off", pre_leads ? "amp" : "pre-amp",
                 (unsigned)hs->delay_s);
    }
    ui_toast(msg);
}

static void rooms_close_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) rooms_hide();
}

static lv_obj_t *nav_panel_create(lv_obj_t *screen, const char *title_text,
                                  lv_event_cb_t close_cb)
{
    lv_obj_t *p = lv_obj_create(screen);
    lv_obj_remove_style_all(p);
    lv_obj_set_size(p, PANEL_W, PANEL_H);
    lv_obj_set_pos(p, PANEL_X, PANEL_Y);
    lv_obj_set_style_bg_color(p, lv_color_hex(UI_PANEL), 0);
    lv_obj_set_style_bg_opa(p, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(p, 2, 0);
    lv_obj_set_style_border_color(p, lv_color_hex(UI_AMBER), 0);
    lv_obj_set_style_radius(p, 16, 0);
    lv_obj_clear_flag(p, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(p, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *title = lv_label_create(p);
    lv_label_set_text(title, title_text);
    lv_obj_set_style_text_color(title, lv_color_hex(UI_TEXT), 0);
    lv_obj_set_style_text_font(title, &p4_font_28, 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 30, 20);

    lv_obj_t *close = create_button(p, "CLOSE", 150, 50);
    lv_obj_align(close, LV_ALIGN_TOP_RIGHT, -30, 14);
    lv_obj_add_event_cb(close, close_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_flag(p, LV_OBJ_FLAG_HIDDEN);
    return p;
}

static lv_obj_t *room_label(lv_obj_t *parent, const lv_font_t *font, uint32_t color,
                            int x, int y, int w)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, "");
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    if (w > 0) {
        lv_obj_set_width(l, w);
        lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
        label_one_line(l);
    }
    lv_obj_set_pos(l, x, y);
    return l;
}

/* One switch row ("Amp" / "Pre-amp") at y. */
/* The "System power" row at y. */
static lv_obj_t *room_switch_row(room_card_t *c, int w, int y, const char *label, int i)
{
    lv_obj_t *row = lv_obj_create(c->card);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, w - 32, 40);
    lv_obj_set_pos(row, 16, y);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *l = lv_label_create(row);
    lv_label_set_text(l, label);
    lv_obj_set_style_text_font(l, &p4_font_18, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(UI_TEXT_SOFT), 0);
    lv_obj_align(l, LV_ALIGN_LEFT_MID, 0, 0);

    lv_obj_t *sw = lv_switch_create(row);
    lv_obj_set_size(sw, 64, 34);
    lv_obj_align(sw, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_bg_color(sw, lv_color_hex(UI_TRACK), LV_PART_MAIN);
    lv_obj_set_style_bg_color(sw, lv_color_hex(UI_ACCENT), LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(sw, lv_color_hex(UI_ACCENT_BRIGHT), LV_PART_KNOB);
    lv_obj_set_ext_click_area(sw, 8);
    lv_obj_set_style_text_color(l, lv_color_hex(UI_TEXT), 0);
    lv_obj_add_event_cb(sw, room_system_event_cb, LV_EVENT_VALUE_CHANGED, (void *)(intptr_t)i);
    c->sys_sw = sw;
    lv_obj_add_flag(row, LV_OBJ_FLAG_HIDDEN);           /* shown once we know it exists */
    return row;
}

/* Width of `text` in `font`, from the public per-character widths (plain
 * ASCII room names; good enough to choose a font size). */
static int room_text_px(const lv_font_t *font, const char *text)
{
    int px = 0;
    for (const char *p = text; *p; p++) {
        px += lv_font_get_glyph_width(font, (uint32_t)(uint8_t)p[0], (uint32_t)(uint8_t)p[1]);
    }
    return px;
}

/* ------------------------------------------------------ play rooms together */
static lv_obj_t *group_overlay = NULL;     /* dims the screen behind the dialog */
static lv_obj_t *group_panel = NULL;
static lv_obj_t *group_switches[LMS_MAX_PLAYERS];

/* A dimmed full-screen layer with a centred panel; tapping outside closes it. */
static void modal_backdrop_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (lv_event_get_target(e) != lv_event_get_current_target(e)) return;   /* a tap on the panel */
    lv_obj_add_flag((lv_obj_t *)lv_event_get_current_target(e), LV_OBJ_FLAG_HIDDEN);
}

static lv_obj_t *modal_create(lv_obj_t *screen, int w, int h, lv_obj_t **panel_out)
{
    lv_obj_t *back = lv_obj_create(screen);
    lv_obj_remove_style_all(back);
    lv_obj_set_size(back, 1024, 600);
    lv_obj_set_style_bg_color(back, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(back, LV_OPA_60, 0);
    lv_obj_clear_flag(back, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(back, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(back, modal_backdrop_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_flag(back, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *p = lv_obj_create(back);
    lv_obj_remove_style_all(p);
    lv_obj_set_size(p, w, h);
    lv_obj_align(p, LV_ALIGN_CENTER, 0, -30);
    lv_obj_set_style_bg_color(p, lv_color_hex(UI_PANEL), 0);
    lv_obj_set_style_bg_opa(p, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(p, 2, 0);
    lv_obj_set_style_border_color(p, lv_color_hex(UI_AMBER), 0);
    lv_obj_set_style_radius(p, 18, 0);
    lv_obj_clear_flag(p, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(p, LV_OBJ_FLAG_CLICKABLE);                   /* taps on it stay on it */
    *panel_out = p;
    return back;
}

static void modal_close_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    lv_obj_t *back = (lv_obj_t *)lv_event_get_user_data(e);
    if (back != NULL) lv_obj_add_flag(back, LV_OBJ_FLAG_HIDDEN);
}

static lv_obj_t *modal_text(lv_obj_t *p, const char *text, const lv_font_t *font, uint32_t color, int x, int y)
{
    lv_obj_t *l = lv_label_create(p);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_obj_set_pos(l, x, y);
    return l;
}

static void group_switch_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    const int j = (int)(intptr_t)lv_event_get_user_data(e);
    const int sel = current_player_index;
    if (j < 0 || j >= lyrion_player_count || sel < 0 || sel >= lyrion_player_count) return;
    const bool on = lv_obj_has_state(group_switches[j], LV_STATE_CHECKED);
    char msg[80];
    if (on) {
        queue_lyrion_command_for(sel, LYRION_COMMAND_SYNC, j);
        snprintf(msg, sizeof(msg), "%s now plays with %s", lyrion_players[j].short_name,
                 lyrion_players[sel].short_name);
    } else {
        queue_lyrion_command_for(j, LYRION_COMMAND_UNSYNC, 0);
        snprintf(msg, sizeof(msg), "%s plays on its own", lyrion_players[j].short_name);
    }
    ui_toast(msg);
}

static void group_ungroup_all_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    int sent = 0;
    for (int i = 0; i < lyrion_player_count; i++) {
        if (rooms_group_mask[i] & ~(1 << i)) {
            queue_lyrion_command_for(i, LYRION_COMMAND_UNSYNC, 0);
            sent++;
        }
    }
    ui_toast(sent ? "Every room plays on its own" : "No rooms are grouped");
    if (group_overlay != NULL) lv_obj_add_flag(group_overlay, LV_OBJ_FLAG_HIDDEN);
}

/* Rebuilt each time, because it is about whichever room is selected. */
static void group_dialog_open(void)
{
    const int sel = current_player_index;
    if (group_panel == NULL || sel < 0 || sel >= lyrion_player_count) return;
    lv_obj_clean(group_panel);
    for (int i = 0; i < LMS_MAX_PLAYERS; i++) group_switches[i] = NULL;

    /* Short title on one line; the room goes in the line under it (a long
     * "PLAY TOGETHER WITH <room>" wrapped onto that line and covered it). */
    modal_text(group_panel, "PLAY TOGETHER", &p4_font_28, UI_TEXT, 28, 20);
    char sub[96];
    snprintf(sub, sizeof(sub), "Rooms switched on play the same music as %s.",
             lyrion_players[sel].short_name);
    lv_obj_t *st = modal_text(group_panel, sub, &p4_font_16, UI_TEXT_SOFT, 28, 62);
    lv_obj_set_width(st, 504);
    label_one_line(st);
    lv_label_set_long_mode(st, LV_LABEL_LONG_DOT);

    int row = 0;
    for (int j = 0; j < lyrion_player_count; j++) {
        if (j == sel) continue;
        lv_obj_t *r = lv_obj_create(group_panel);
        lv_obj_remove_style_all(r);
        lv_obj_set_size(r, 500, 58);
        lv_obj_set_pos(r, 28, 100 + row * 68);
        lv_obj_set_style_bg_color(r, lv_color_hex(UI_BUTTON), 0);
        lv_obj_set_style_bg_opa(r, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(r, 12, 0);
        lv_obj_set_style_border_width(r, 1, 0);
        lv_obj_set_style_border_color(r, lv_color_hex(UI_EDGE), 0);
        lv_obj_clear_flag(r, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_t *n = lv_label_create(r);
        lv_label_set_text(n, lyrion_players[j].short_name);
        lv_obj_set_style_text_font(n, &p4_font_18, 0);
        lv_obj_set_style_text_color(n, lv_color_hex(UI_TEXT), 0);
        lv_obj_align(n, LV_ALIGN_LEFT_MID, 20, 0);
        lv_obj_t *sw = lv_switch_create(r);
        lv_obj_set_size(sw, 64, 34);
        lv_obj_align(sw, LV_ALIGN_RIGHT_MID, -16, 0);
        lv_obj_set_style_bg_color(sw, lv_color_hex(UI_TRACK), LV_PART_MAIN);
        lv_obj_set_style_bg_color(sw, lv_color_hex(UI_ACCENT), LV_PART_INDICATOR | LV_STATE_CHECKED);
        lv_obj_set_style_bg_color(sw, lv_color_hex(UI_ACCENT_BRIGHT), LV_PART_KNOB);
        lv_obj_set_ext_click_area(sw, 10);
        if (rooms_group_mask[sel] & (1 << j)) lv_obj_add_state(sw, LV_STATE_CHECKED);
        lv_obj_add_event_cb(sw, group_switch_event_cb, LV_EVENT_VALUE_CHANGED, (void *)(intptr_t)j);
        group_switches[j] = sw;
        row++;
    }
    modal_text(group_panel, "Each room keeps its own volume.", &p4_font_14, UI_TEXT_DIM,
               28, 100 + row * 68 + 4);

    lv_obj_t *ung = create_button(group_panel, "UNGROUP ALL", 220, 52);
    lv_obj_align(ung, LV_ALIGN_BOTTOM_LEFT, 28, -20);
    lv_obj_add_event_cb(ung, group_ungroup_all_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *done = create_button(group_panel, "DONE", 150, 52);
    lv_obj_align(done, LV_ALIGN_BOTTOM_RIGHT, -28, -20);
    lv_obj_set_style_bg_color(done, lv_color_hex(UI_ACCENT), LV_PART_MAIN);
    lv_obj_set_style_text_color(lv_obj_get_child(done, 0), lv_color_hex(UI_ON_ACCENT), 0);
    lv_obj_add_event_cb(done, modal_close_event_cb, LV_EVENT_CLICKED, group_overlay);

    lv_obj_clear_flag(group_overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(group_overlay);
}

static void hub_show(void);

static void nav_hide_panels(void);

static void sleep_close_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_CLICKED && sleep_overlay != NULL) {
        lv_obj_add_flag(sleep_overlay, LV_OBJ_FLAG_HIDDEN);
    }
}

static void sleep_back_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (sleep_overlay != NULL) lv_obj_add_flag(sleep_overlay, LV_OBJ_FLAG_HIDDEN);
    hub_show();
}

static void sleep_picker_create(lv_obj_t *screen)
{
    {
        nvs_handle_t nvs;
        uint8_t v = 1;
        if (nvs_open(PLAYER_NVS_NAMESPACE, NVS_READONLY, &nvs) == ESP_OK) {
            if (nvs_get_u8(nvs, SLEEP_NVS_AMP, &v) == ESP_OK) sleep_amp_enabled = (v != 0);
            nvs_close(nvs);
        }
    }
    /* A full-size page like the other Settings pages. */
    sleep_overlay = nav_panel_create(screen, "SLEEP TIMER", sleep_close_event_cb);
    sleep_panel = sleep_overlay;
    lv_obj_t *st = lv_obj_get_child(sleep_panel, 0);
    lv_obj_set_width(st, 520);                            /* stops before BACK / CLOSE */
    label_one_line(st);
    lv_label_set_long_mode(st, LV_LABEL_LONG_DOT);

    modal_text(sleep_panel, "Music stops and the player turns off after:", &p4_font_18,
               UI_TEXT_SOFT, 30, 92);
    static const int mins[6] = { 15, 30, 45, 60, 90, SLEEP_END_OF_SONG };
    for (int i = 0; i < 6; i++) {
        char t[16];
        if (mins[i] > 0) snprintf(t, sizeof(t), "%d min", mins[i]);
        else             snprintf(t, sizeof(t), "End of song");
        lv_obj_t *b = create_button(sleep_panel, t, 276, 64);
        lv_obj_set_style_text_font(lv_obj_get_child(b, 0), &p4_font_18, 0);
        lv_obj_set_pos(b, 30 + (i % 3) * 297, 130 + (i / 3) * 80);
        lv_obj_add_event_cb(b, sleep_option_event_cb, LV_EVENT_CLICKED, (void *)(intptr_t)mins[i]);
    }
    sleep_status_label = modal_text(sleep_panel, "", &p4_font_18, UI_ACCENT, 30, 300);

    /* Amp off too, when the timer runs out. */
    lv_obj_t *amp = lv_obj_create(sleep_panel);
    lv_obj_remove_style_all(amp);
    lv_obj_set_size(amp, 870, 58);
    lv_obj_set_pos(amp, 30, 344);
    lv_obj_set_style_radius(amp, 12, 0);
    lv_obj_set_style_border_width(amp, 1, 0);
    lv_obj_set_style_border_color(amp, lv_color_hex(UI_EDGE), 0);
    lv_obj_clear_flag(amp, LV_OBJ_FLAG_SCROLLABLE);
    modal_text(amp, "Also switch the amp off", &p4_font_18, UI_TEXT, 20, 17);
    modal_text(amp, "when the timer ends (needs the Home Assistant switch set up)", &p4_font_14,
               UI_TEXT_DIM, 270, 21);
    lv_obj_t *amp_sw = lv_switch_create(amp);
    sleep_amp_sw = amp_sw;
    lv_obj_set_size(amp_sw, 64, 34);
    lv_obj_align(amp_sw, LV_ALIGN_RIGHT_MID, -16, 0);
    lv_obj_set_style_bg_color(amp_sw, lv_color_hex(UI_TRACK), LV_PART_MAIN);
    lv_obj_set_style_bg_color(amp_sw, lv_color_hex(UI_ACCENT), LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(amp_sw, lv_color_hex(UI_ACCENT_BRIGHT), LV_PART_KNOB);
    lv_obj_set_ext_click_area(amp_sw, 10);
    if (sleep_amp_enabled) lv_obj_add_state(amp_sw, LV_STATE_CHECKED);
    lv_obj_add_event_cb(amp_sw, sleep_amp_switch_event_cb, LV_EVENT_VALUE_CHANGED, NULL);

    lv_obj_t *off = create_button(sleep_panel, "TURN OFF TIMER", 220, 52);
    lv_obj_set_pos(off, 30, PANEL_H - 72);
    lv_obj_set_style_bg_color(off, lv_color_hex(UI_RED), LV_PART_MAIN);
    lv_obj_set_style_text_color(lv_obj_get_child(off, 0), lv_color_hex(UI_ON_ACCENT), 0);
    lv_obj_add_event_cb(off, sleep_off_event_cb, LV_EVENT_CLICKED, NULL);

    /* BACK to the Settings tiles (shown only when opened from Settings). */
    sleep_back_btn = create_button(sleep_panel, LV_SYMBOL_LEFT "  BACK", 150, 50);
    lv_obj_align(sleep_back_btn, LV_ALIGN_TOP_RIGHT, -196, 14);
    lv_obj_add_event_cb(sleep_back_btn, sleep_back_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_flag(sleep_back_btn, LV_OBJ_FLAG_HIDDEN);
}

static void sleep_picker_open_from(bool from_settings)
{
    if (sleep_overlay == NULL) return;
    if (sleep_back_btn != NULL) {
        if (from_settings) lv_obj_clear_flag(sleep_back_btn, LV_OBJ_FLAG_HIDDEN);
        else               lv_obj_add_flag(sleep_back_btn, LV_OBJ_FLAG_HIDDEN);
    }
    if (current_player_index < 0 || current_player_index >= lyrion_player_count) return;
    lv_obj_t *title = lv_obj_get_child(sleep_panel, 0);
    char t[64];
    snprintf(t, sizeof(t), "SLEEP TIMER  -  %s", lyrion_players[current_player_index].short_name);
    for (char *p = t; *p; p++) *p = (char)toupper((unsigned char)*p);
    lv_label_set_text(title, t);
    if (sleep_amp_sw != NULL) {   /* may have changed on the web page */
        if (sleep_amp_enabled) lv_obj_add_state(sleep_amp_sw, LV_STATE_CHECKED);
        else                   lv_obj_clear_state(sleep_amp_sw, LV_STATE_CHECKED);
    }
    nav_hide_panels();
    ui_show_panel(sleep_overlay);
    sleep_refresh_locked();
}

static void sleep_picker_open(void) { sleep_picker_open_from(false); }   /* header chip */

static void rooms_group_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) group_dialog_open();
}

static void rooms_create(lv_obj_t *screen)
{
    rooms_overlay = nav_panel_create(screen, "ROOMS", rooms_close_event_cb);
    if (lyrion_player_count > 1) {
        lv_obj_t *group = create_button(rooms_overlay, "GROUP", 150, 50);
        lv_obj_align(group, LV_ALIGN_TOP_RIGHT, -196, 14);
        lv_obj_add_event_cb(group, rooms_group_event_cb, LV_EVENT_CLICKED, NULL);
    }
    group_overlay = modal_create(screen, 560, 60 + 100 + (LMS_MAX_PLAYERS - 1) * 68 + 50,
                                 &group_panel);

    const int n = lyrion_player_count > 0 ? lyrion_player_count : 1;
    const int gap = 18, x0 = 30, y0 = 88, h = PANEL_H - y0 - 16;
    const int w = (870 - (n - 1) * gap) / n;
    /* Big room names only if every one of them fits beside the cover, so
     * all the cards match. */
    bool wide = true;
    for (int i = 0; i < lyrion_player_count; i++) {
        if (room_text_px(&p4_font_28, lyrion_players[i].short_name) > w - (16 + ROOM_COVER_PX + 14) - 12) {
            wide = false;
        }
    }

    for (int i = 0; i < lyrion_player_count; i++) {
        room_card_t *c = &rooms_cards[i];
        memset(c, 0, sizeof(*c));
        c->sel_shown = -1;
        c->vol_touch = c->play_touch = lv_tick_get() - ROOM_TOUCH_HOLD_MS - 1;

        c->card = lv_obj_create(rooms_overlay);
        lv_obj_remove_style_all(c->card);
        lv_obj_set_size(c->card, w, h);
        lv_obj_set_pos(c->card, x0 + i * (w + gap), y0);
        lv_obj_set_style_bg_color(c->card, lv_color_hex(UI_BUTTON), 0);
        lv_obj_set_style_bg_opa(c->card, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(c->card, 18, 0);
        lv_obj_set_style_border_width(c->card, 1, 0);
        lv_obj_set_style_border_color(c->card, lv_color_hex(UI_EDGE), 0);
        lv_obj_clear_flag(c->card, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(c->card, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(c->card, room_card_event_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);

        /* cover (placeholder square until the first cover arrives) */
        lv_obj_t *ph = lv_obj_create(c->card);
        lv_obj_remove_style_all(ph);
        lv_obj_set_size(ph, ROOM_COVER_PX, ROOM_COVER_PX);
        lv_obj_set_pos(ph, 16, 16);
        lv_obj_set_style_bg_color(ph, lv_color_hex(UI_ALBUM_BG), 0);
        lv_obj_set_style_bg_opa(ph, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(ph, 10, 0);
        lv_obj_clear_flag(ph, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_t *note = lv_label_create(ph);
        lv_label_set_text(note, LV_SYMBOL_AUDIO);
        lv_obj_set_style_text_color(note, lv_color_hex(UI_TEXT_DIM), 0);
        lv_obj_center(note);
        c->cover = lv_image_create(c->card);
        lv_obj_set_pos(c->cover, 16, 16);
        lv_obj_clear_flag(c->cover, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(c->cover, LV_OBJ_FLAG_HIDDEN);

        const int tx = 16 + ROOM_COVER_PX + 14;
        c->name = room_label(c->card, wide ? &p4_font_28 : &p4_font_18,
                             UI_TEXT, tx, 18, w - tx - 12);
        lv_label_set_text(c->name, lyrion_players[i].short_name);

        c->dot = lv_obj_create(c->card);
        lv_obj_remove_style_all(c->dot);
        lv_obj_set_size(c->dot, 12, 12);
        lv_obj_set_style_radius(c->dot, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(c->dot, LV_OPA_COVER, 0);
        lv_obj_set_pos(c->dot, tx, 66);
        lv_obj_clear_flag(c->dot, LV_OBJ_FLAG_CLICKABLE);
        c->state_lbl = room_label(c->card, &p4_font_16, UI_TEXT_SOFT, tx + 20, 61, w - tx - 32);
        c->selected_lbl = room_label(c->card, &p4_font_14, UI_ACCENT, tx, 88, 0);
        lv_label_set_text(c->selected_lbl, LV_SYMBOL_OK " Selected");
        lv_obj_add_flag(c->selected_lbl, LV_OBJ_FLAG_HIDDEN);
        c->linked_lbl = room_label(c->card, &p4_font_14, UI_ACCENT_GLOW, tx, 106, w - tx - 12);

        c->artist = room_label(c->card, &p4_font_16, UI_TEXT_SOFT, 16, 126, w - 32);
        c->title  = room_label(c->card, &p4_font_18, UI_TEXT, 16, 150, w - 32);
        lv_obj_set_height(c->title, 2 * lv_font_get_line_height(&p4_font_18));   /* title may use two lines */

        c->play_btn = create_button(c->card, LV_SYMBOL_PLAY, 84, 56);
        lv_obj_set_pos(c->play_btn, 16, 196);
        lv_obj_set_style_text_font(lv_obj_get_child(c->play_btn, 0), &p4_font_28, 0);
        lv_obj_add_event_cb(c->play_btn, room_play_event_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);

        lv_obj_t *vcap = room_label(c->card, &p4_font_14, UI_TEXT_DIM, 116, 198, 0);
        lv_label_set_text(vcap, "Volume");
        c->vol_lbl = room_label(c->card, &p4_font_14, UI_TEXT, 0, 198, 0);
        lv_obj_align(c->vol_lbl, LV_ALIGN_TOP_RIGHT, -16, 198);
        c->vol_slider = lv_slider_create(c->card);
        lv_slider_set_range(c->vol_slider, 0, 100);
        lv_obj_set_size(c->vol_slider, w - 116 - 26, 14);
        lv_obj_set_pos(c->vol_slider, 116, 228);
        lv_obj_set_style_bg_color(c->vol_slider, lv_color_hex(UI_SLIDER_BG), LV_PART_MAIN);
        lv_obj_set_style_bg_color(c->vol_slider, lv_color_hex(UI_TERTIARY), LV_PART_INDICATOR);
        lv_obj_set_style_bg_color(c->vol_slider, lv_color_hex(UI_ACCENT_BRIGHT), LV_PART_KNOB);
        lv_obj_set_ext_click_area(c->vol_slider, 14);
        lv_obj_add_event_cb(c->vol_slider, room_volume_event_cb, LV_EVENT_VALUE_CHANGED, (void *)(intptr_t)i);
        lv_obj_add_event_cb(c->vol_slider, room_volume_event_cb, LV_EVENT_RELEASED, (void *)(intptr_t)i);

        lv_obj_t *rule = lv_obj_create(c->card);
        lv_obj_remove_style_all(rule);
        lv_obj_set_size(rule, w - 32, 1);
        lv_obj_set_pos(rule, 16, 272);
        lv_obj_set_style_bg_color(rule, lv_color_hex(UI_EDGE), 0);
        lv_obj_set_style_bg_opa(rule, LV_OPA_COVER, 0);

        c->sys_row = room_switch_row(c, w, 294, "System power", i);
        c->sys_note = room_label(c->card, &p4_font_14, UI_TEXT_DIM, 16, 340, w - 32);
        lv_label_set_text(c->sys_note, "");
        lv_obj_add_flag(c->sys_note, LV_OBJ_FLAG_HIDDEN);
        c->no_sw_lbl = room_label(c->card, &p4_font_14, UI_TEXT_DIM, 16, 300, w - 32);
        lv_label_set_text(c->no_sw_lbl, "No power switches");
        lv_obj_add_flag(c->no_sw_lbl, LV_OBJ_FLAG_HIDDEN);

        room_show_state(c, 0);
    }

    /* The separate Power screen is replaced by the switches on each card; its
     * code stays for now (nav_hide_panels still closes it). */
    (void)power_show;
}

static void nav_hide_panels(void);

static void rooms_show(void)
{
    nav_hide_panels();
    /* Until the first refresh arrives, show what's already known. */
    for (int i = 0; i < lyrion_player_count; i++) {
        room_card_t *c = &rooms_cards[i];
        if (c->card == NULL) continue;
        const int st = (i == current_player_index && lyrion_fail_count < 2)
                       ? (current_player_playing ? 2 : 1) : house_state[i];
        room_show_state(c, st);
        c->sel_shown = -1;
    }
    ui_show_panel(rooms_overlay);
    rooms_refresh_locked();
    rooms_open = true;
    rooms_wake_worker();
}

/* ----------------------------------------------------------- Settings hub */
static void hub_hide(void)
{
    if (hub_overlay != NULL) lv_obj_add_flag(hub_overlay, LV_OBJ_FLAG_HIDDEN);
}

static void hub_close_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) hub_hide();
}

static void hub_web_address(char *out, size_t n)
{
    esp_netif_ip_info_t ip;
    memset(&ip, 0, sizeof(ip));
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif != NULL && esp_netif_get_ip_info(netif, &ip) == ESP_OK && ip.ip.addr != 0) {
        snprintf(out, n, "http://" IPSTR "/", IP2STR(&ip.ip));
    } else {
        snprintf(out, n, "(not connected)");
    }
}

static void alarms_show(void);                 /* 18c_alarms.c */
static void alarms_hide(void);
static void alarms_ui_create(lv_obj_t *screen);
static void ssp_create(lv_obj_t *screen);      /* 18d_screensaver_panel.c */
static void ssp_show(void);
static void ssp_hide(void);
static void vsp_create(lv_obj_t *screen);
static void csp_create(lv_obj_t *screen);
static void csp_hide(void);
static void vsp_show(void);
static void vsp_hide(void);
static lv_obj_t *alarms_overlay, *aed_overlay;
static void sleep_picker_open(void);

static void hub_tile_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    const int which = (int)(intptr_t)lv_event_get_user_data(e);
    char addr[48], msg[96];
    switch (which) {
        case 0: hub_hide(); settings_show(); break;                     /* theme */
        case 1: hub_hide(); vsp_show(); break;                          /* visualizer */
        case 2: hub_hide(); wifi_open(); break;                         /* Wi-Fi */
        case 3: hub_hide(); lms_open(); break;                          /* Lyrion server */
        case 5: hub_hide(); alarms_show(); break;                       /* alarms */
        case 6: hub_hide(); sleep_picker_open_from(true); break;        /* sleep timer */
        case 4: hub_hide(); ss_preview_view = -1; ssp_show(); break;    /* screensaver */
        default:                                                         /* about */
            hub_web_address(addr, sizeof(addr));
            snprintf(msg, sizeof(msg), "Web settings and updates:  %s", addr);
            ui_toast(msg);
            break;
    }
}

static void hub_refresh_locked(void)
{
    char t[96], addr[48];
    snprintf(t, sizeof(t), "%s", ui_theme->name);
    lv_label_set_text(hub_subtitles[0], t);
    snprintf(t, sizeof(t), "%s  " LV_SYMBOL_BULLET "  tap to open",
             vis_style_names[vis_style_selected]);
    lv_label_set_text(hub_subtitles[1], t);
    lv_label_set_text(hub_subtitles[2], wifi_cur_ssid);
    snprintf(t, sizeof(t), "%s  " LV_SYMBOL_BULLET "  %d player%s", lyrion_server_base + 7,
             lyrion_player_count, lyrion_player_count == 1 ? "" : "s");
    lv_label_set_text(hub_subtitles[3], t);
    snprintf(t, sizeof(t), "%s  " LV_SYMBOL_BULLET "  after %u min",
             (ss_pause_mode >= 0 && ss_pause_mode < 5)
             ? (const char *const[]){ "Clock", "Clock + cover", "Analog clock", "Cover wall", "Weather" }[ss_pause_mode] : "Clock",
             (unsigned)ss_timeout_min);
    lv_label_set_text(hub_subtitles[4], t);
    snprintf(t, sizeof(t), "Wake-up music for %s",
             (current_player_index >= 0 && current_player_index < lyrion_player_count)
             ? lyrion_players[current_player_index].short_name : "a room");
    lv_label_set_text(hub_subtitles[5], t);
    {
        const int left = sleep_left_now();
        if (left > 0) snprintf(t, sizeof(t), "%d min left", (left + 59) / 60);
        else          snprintf(t, sizeof(t), "Off");
        lv_label_set_text(hub_subtitles[6], t);
    }
    hub_web_address(addr, sizeof(addr));
    snprintf(t, sizeof(t), "%s  " LV_SYMBOL_BULLET "  %s", esp_app_get_description()->version, addr);
    lv_label_set_text(hub_subtitles[7], t);
}

static void hub_create(lv_obj_t *screen)
{
    static const char *const names[8] = { "Theme", "Visualizer", "Wi-Fi", "Lyrion server",
                                          "Screensaver", "Alarms", "Sleep timer", "About" };
    static const char *const icons[8] = { LV_SYMBOL_IMAGE, LV_SYMBOL_AUDIO, LV_SYMBOL_WIFI,
                                          LV_SYMBOL_DRIVE, LV_SYMBOL_EYE_CLOSE, LV_SYMBOL_BELL,
                                          LV_SYMBOL_POWER, LV_SYMBOL_LIST };
    hub_overlay = nav_panel_create(screen, "SETTINGS", hub_close_event_cb);

    for (int i = 0; i < 8; i++) {
        lv_obj_t *tile = lv_obj_create(hub_overlay);
        lv_obj_remove_style_all(tile);
        lv_obj_set_size(tile, 426, 92);
        lv_obj_set_pos(tile, 30 + (i % 2) * 444, 90 + (i / 2) * 104);
        lv_obj_set_style_bg_color(tile, lv_color_hex(UI_BUTTON), 0);
        lv_obj_set_style_bg_opa(tile, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(tile, 18, 0);
        lv_obj_set_style_border_width(tile, 1, 0);
        lv_obj_set_style_border_color(tile, lv_color_hex(UI_EDGE), 0);
        lv_obj_set_style_bg_color(tile, lv_color_hex(UI_ACCENT_DARK), LV_STATE_PRESSED);
        lv_obj_clear_flag(tile, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(tile, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(tile, hub_tile_event_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);

        lv_obj_t *icon_box = lv_obj_create(tile);
        lv_obj_remove_style_all(icon_box);
        lv_obj_set_size(icon_box, 56, 56);
        lv_obj_align(icon_box, LV_ALIGN_LEFT_MID, 20, 0);
        lv_obj_set_style_bg_color(icon_box, lv_color_hex(UI_ACCENT_DARK), 0);
        lv_obj_set_style_bg_opa(icon_box, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(icon_box, 16, 0);
        lv_obj_clear_flag(icon_box, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_t *icon = lv_label_create(icon_box);
        lv_label_set_text(icon, icons[i]);
        lv_obj_set_style_text_color(icon, lv_color_hex(UI_ACCENT_GLOW), 0);
        lv_obj_set_style_text_font(icon, &p4_font_28, 0);
        lv_obj_center(icon);

        lv_obj_t *name = lv_label_create(tile);
        lv_label_set_text(name, names[i]);
        lv_obj_set_style_text_color(name, lv_color_hex(UI_TEXT), 0);
        lv_obj_set_style_text_font(name, &p4_font_18, 0);
        lv_obj_set_pos(name, 96, 18);

        hub_subtitles[i] = lv_label_create(tile);
        lv_obj_set_width(hub_subtitles[i], 286);
        lv_label_set_long_mode(hub_subtitles[i], LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_color(hub_subtitles[i], lv_color_hex(UI_TEXT_SOFT), 0);
        lv_obj_set_style_text_font(hub_subtitles[i], &p4_font_14, 0);
        label_one_line(hub_subtitles[i]);
        lv_obj_set_pos(hub_subtitles[i], 96, 52);

        lv_obj_t *arrow = lv_label_create(tile);
        lv_label_set_text(arrow, LV_SYMBOL_RIGHT);
        lv_obj_set_style_text_color(arrow, lv_color_hex(UI_TEXT_DIM), 0);
        lv_obj_align(arrow, LV_ALIGN_RIGHT_MID, -20, 0);
    }
}

static void hub_show(void);

/* BACK on a settings page: back to the Settings tiles. */
static void ui_settings_back_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    hub_show();                     /* hides the open page, shows the tiles */
}

static void ui_add_settings_back(lv_obj_t *panel)
{
    if (panel == NULL) return;
    lv_obj_t *b = create_button(panel, LV_SYMBOL_LEFT "  BACK", 150, 50);
    lv_obj_align(b, LV_ALIGN_TOP_RIGHT, -196, 14);
    lv_obj_add_event_cb(b, ui_settings_back_event_cb, LV_EVENT_CLICKED, NULL);
}

static void hub_show(void)
{
    nav_hide_panels();
    hub_refresh_locked();
    ui_show_panel(hub_overlay);
}

/* ------------------------------------------------------------- tab bar */
/* Close every full panel (the tab being opened then shows its own). */
static void nav_hide_panels(void)
{
    source_hide();
    settings_hide();
    wifi_close();
    lms_close();
    rooms_hide();
    hub_hide();
    power_hide();
    alarms_hide();
    ssp_hide();
    vsp_hide();
    csp_hide();
    if (sleep_overlay != NULL) lv_obj_add_flag(sleep_overlay, LV_OBJ_FLAG_HIDDEN);
}

static void nav_open_source(source_mode_t mode)
{
    if (source_queue == NULL) return;
    nav_hide_panels();
    source_nav_depth = 0;
    source_request_t request = { .mode = mode, .offset = 0, .push_history = false };
    xQueueSend(source_queue, &request, 0);
}

static void nav_tab_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    switch ((int)(intptr_t)lv_event_get_user_data(e)) {
        case TAB_NOW:      nav_hide_panels(); break;
        case TAB_LIBRARY:  nav_open_source(SOURCE_ROOT); break;
        case TAB_QUEUE:    nav_open_source(SOURCE_QUEUE); break;
        case TAB_ROOMS:    rooms_show(); break;
        case TAB_SETTINGS: hub_show(); break;
        default: break;
    }
}

static bool nav_visible(lv_obj_t *o) { return o != NULL && !lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN); }

/* Which tab matches what is on screen right now. */
static int nav_current_tab(void)
{
    if (nav_visible(source_overlay)) return source_current_mode == SOURCE_QUEUE ? TAB_QUEUE : TAB_LIBRARY;
    if (nav_visible(rooms_overlay) || nav_visible(power_overlay)) return TAB_ROOMS;
    if (nav_visible(hub_overlay) || nav_visible(settings_overlay) ||
        nav_visible(wifi_overlay) || nav_visible(lms_overlay) ||
        nav_visible(alarms_overlay) || nav_visible(aed_overlay) ||
        nav_visible(ssp_overlay) || nav_visible(vsp_overlay) || nav_visible(csp_overlay) ||
        nav_visible(sleep_overlay)) return TAB_SETTINGS;
    return TAB_NOW;
}

static void nav_style_tab(int i, bool on)
{
    lv_obj_t *tab = nav_tabs[i];
    lv_obj_set_style_bg_opa(tab, on ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    const uint32_t col = on ? UI_ACCENT : UI_TEXT_DIM;
    lv_obj_set_style_text_color(lv_obj_get_child(tab, 0), lv_color_hex(col), 0);
    lv_obj_set_style_text_color(lv_obj_get_child(tab, 1), lv_color_hex(col), 0);
}

/* ---- Now Playing FAV button ------------------------------------------------
 * np_fav: -1 unknown / nothing playing (hidden), 0 not a favorite, 1 favorite.
 * The status task tracks the playing URL; the browser task asks Lyrion. */
static int fav_shown = -2;
static int list_shown = -1;

static void fav_refresh_locked(void)
{
    if (fav_button == NULL) return;
    const int f = np_fav;

    /* "+ LIST" only for songs: a live radio stream has no length, and
     * adding a never-ending stream to a playlist makes no sense. */
    const int want_list = (f >= 0 && playback_valid && playback_duration_s > 0.0) ? 1 : 0;
    if (np_list_button != NULL && want_list != list_shown) {
        list_shown = want_list;
        if (want_list) lv_obj_remove_flag(np_list_button, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(np_list_button, LV_OBJ_FLAG_HIDDEN);
    }

    if (f == fav_shown) return;
    fav_shown = f;
    if (f < 0) {
        lv_obj_add_flag(fav_button, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_remove_flag(fav_button, LV_OBJ_FLAG_HIDDEN);
    lv_obj_t *label = lv_obj_get_child(fav_button, 0);
    if (label != NULL) {
        lv_label_set_text(label, f ? LV_SYMBOL_OK " FAV" : LV_SYMBOL_PLUS " FAV");
        lv_obj_set_style_text_color(label, lv_color_hex(f ? UI_ACCENT : UI_TEXT_DIM), 0);
    }
    lv_obj_set_style_border_color(fav_button, lv_color_hex(f ? UI_ACCENT : UI_EDGE), 0);
    lv_obj_set_style_bg_color(fav_button, lv_color_hex(f ? UI_ACCENT_DARK : UI_BUTTON), 0);
}

static void fav_button_event_cb(lv_event_t *e)
{
    (void)e;
    const int f = np_fav;
    if (f < 0) return;
    source_request_t request = { .mode = SOURCE_FAV_TOGGLE, .id = (f == 1) };
    xQueueSend(source_queue, &request, 0);
    np_fav = f ? 0 : 1;                  /* show it now; the worker confirms */
    fav_refresh_locked();
}

static void nav_sync_timer_cb(lv_timer_t *t)
{
    (void)t;
    const int now = nav_current_tab();
    if (now != nav_tab_shown) {
        for (int i = 0; i < TAB_COUNT; i++) nav_style_tab(i, i == now);
        nav_tab_shown = now;
    }
    nav_room_chip_refresh();
    rooms_refresh_locked();
    sleep_refresh_locked();
    fav_refresh_locked();

    /* Like the other panels, close after a minute without a touch. */
    if ((nav_visible(rooms_overlay) || nav_visible(hub_overlay) || nav_visible(alarms_overlay) ||
         nav_visible(ssp_overlay) || nav_visible(vsp_overlay) || nav_visible(csp_overlay)) &&
        lv_display_get_inactive_time(NULL) >= SOURCE_IDLE_CLOSE_MS) {
        rooms_hide();
        hub_hide();
        alarms_hide();
        ssp_hide();
        vsp_hide();
        csp_hide();
    }
    if (sleep_overlay != NULL && !lv_obj_has_flag(sleep_overlay, LV_OBJ_FLAG_HIDDEN) &&
        lv_display_get_inactive_time(NULL) >= SOURCE_IDLE_CLOSE_MS) {
        lv_obj_add_flag(sleep_overlay, LV_OBJ_FLAG_HIDDEN);
    }
}

/* Build the bar, the room list, the Settings hub and the toast.  Called once
 * after the other screens exist and before the visualizer / screensaver
 * (which cover everything, the bar included). */
static void nav_create(lv_obj_t *screen)
{
    static const char *const labels[TAB_COUNT] = { "Now Playing", "Library", "Queue", "Rooms", "Settings" };
    static const char *const icons[TAB_COUNT]  = { LV_SYMBOL_PLAY, LV_SYMBOL_DIRECTORY, LV_SYMBOL_LIST,
                                                   LV_SYMBOL_HOME, LV_SYMBOL_SETTINGS };
    rooms_create(screen);
    hub_create(screen);
    sleep_picker_create(screen);
    alarms_ui_create(screen);
    ssp_create(screen);
    vsp_create(screen);
    csp_create(screen);

    nav_bar = lv_obj_create(screen);
    lv_obj_remove_style_all(nav_bar);
    lv_obj_set_size(nav_bar, 1024, NAV_BAR_H);
    lv_obj_align(nav_bar, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(nav_bar, lv_color_hex(UI_PANEL), 0);
    lv_obj_set_style_bg_opa(nav_bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_side(nav_bar, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_width(nav_bar, 1, 0);
    lv_obj_set_style_border_color(nav_bar, lv_color_hex(UI_EDGE), 0);
    lv_obj_clear_flag(nav_bar, LV_OBJ_FLAG_SCROLLABLE);

    for (int i = 0; i < TAB_COUNT; i++) {
        lv_obj_t *tab = lv_obj_create(nav_bar);
        lv_obj_remove_style_all(tab);
        lv_obj_set_size(tab, 176, 52);
        lv_obj_set_pos(tab, i * 204 + 14, 6);
        lv_obj_set_style_radius(tab, 14, 0);
        lv_obj_set_style_bg_color(tab, lv_color_hex(UI_ACCENT_DARK), 0);
        lv_obj_set_style_bg_opa(tab, LV_OPA_TRANSP, 0);
        lv_obj_set_style_bg_color(tab, lv_color_hex(UI_ACCENT_DARK), LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(tab, LV_OPA_COVER, LV_STATE_PRESSED);
        lv_obj_clear_flag(tab, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(tab, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_ext_click_area(tab, 6);
        lv_obj_add_event_cb(tab, nav_tab_event_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);

        lv_obj_t *icon = lv_label_create(tab);
        lv_label_set_text(icon, icons[i]);
        lv_obj_set_style_text_font(icon, &p4_font_18, 0);
        lv_obj_align(icon, LV_ALIGN_TOP_MID, 0, 4);
        lv_obj_t *label = lv_label_create(tab);
        lv_label_set_text(label, labels[i]);
        lv_obj_set_style_text_font(label, &p4_font_14, 0);
        lv_obj_align(label, LV_ALIGN_BOTTOM_MID, 0, -4);
        nav_tabs[i] = tab;
    }

    toast_create(screen);
    lv_timer_create(nav_sync_timer_cb, 250, NULL);
    nav_sync_timer_cb(NULL);
}
