/*
 * P4-LMS - Power screen, browser rows (+ / trash / remove), home grid, HTTP helper
 *
 * Part of main.c.  This file is #included by main.c (in order) and is not
 * compiled on its own, so it shares main.c's includes and static names.
 */

/* ------------------------------------------------------------------------
 * POWER SCREEN - turn the current room's amp / pre-amp on and off
 * ------------------------------------------------------------------------ */
static lv_obj_t *power_overlay = NULL;
static lv_obj_t *power_title_label = NULL;

typedef struct {
    bool amp_known, amp_on, preamp_known, preamp_on;
    char status[80];
} ha_state_msg_t;

static void ha_state_job(void *arg)
{
    const ha_state_msg_t *m = (const ha_state_msg_t *)arg;
    if (!ha_overlay_open) return;        /* closed while we were on the network */
    ha_apply_state_locked(m->amp_known, m->amp_on, m->preamp_known, m->preamp_on);
    if (ha_status_label != NULL) {
        ui_set_label(ha_status_label, m->status);
    }
}

static void alarm_amp_tick(void);        /* 18c_alarms.c */
static void sleep_amp_tick(void);        /* 18b_navigation.c */
static void weather_tick(void);          /* 18w_weather.c */

static void lyrion_command_task(void *arg)
{
    (void)arg;

    ESP_LOGI(TAG, "Lyrion command task started (also handles Home Assistant)");

    while (1) {
        QueueSetMemberHandle_t activated = NULL;

        /* Wait on both queues at once.  A timeout (NULL) is how the old
         * dedicated Home Assistant task used to wake up for its periodic
         * poll, so HA_POLL_MS keeps that same cadence here. */
        if (command_queue_set != NULL) {
            activated = xQueueSelectFromSet(command_queue_set, ha_seq_wait(pdMS_TO_TICKS(HA_POLL_MS)));
        } else {
            /* Queue set failed to create (should not happen) - fall back to
             * Lyrion-only behaviour rather than lock up. */
            vTaskDelay(pdMS_TO_TICKS(HA_POLL_MS));
        }

        if (activated == lyrion_command_queue) {
            lyrion_command_t command;
            if (xQueueReceive(lyrion_command_queue, &command, 0) == pdTRUE) {
                lyrion_send_command(&command);
                /* Give Lyrion a moment to update its state before requesting
                 * a fresh status. */
                vTaskDelay(pdMS_TO_TICKS(150));
                if (lyrion_refresh_event != NULL) {
                    xEventGroupSetBits(lyrion_refresh_event, LYRION_REFRESH_BIT);
                }
            }
        } else if (activated == ha_command_queue) {
            ha_command_t ha_command;
            if (xQueueReceive(ha_command_queue, &ha_command, 0) == pdTRUE) {
                if (ha_command.system) {
                    ha_power_sequence(ha_switches_for_index(ha_command.room), ha_command.turn_on);
                    vTaskDelay(pdMS_TO_TICKS(200));
                } else if (ha_command.entity[0]) {
                    ha_seq_cancel(ha_command.entity);          /* a manual tap overrides a waiting step */
                    ha_switch_set_retry(ha_command.entity, ha_command.turn_on);
                    vTaskDelay(pdMS_TO_TICKS(200));      /* give HA a moment before the next poll */
                }
            }
        }
        ha_seq_tick();                       /* second half of any amp/pre-amp sequence */

        /* Whether we handled a command or just timed out, refresh the Rooms
         * cards and the Power screen's switches whenever they are open. */
        alarm_amp_tick();                    /* "Amp on for alarms" (18c_alarms.c) */
        sleep_amp_tick();                    /* "Amp off with the sleep timer" (18b) */
        weather_tick();                      /* Weather screensaver (18w) */
        if (rooms_open) {
            rooms_poll_all();
        }
        if (!ha_overlay_open) {
            continue;
        }

        bool amp_known = false, preamp_known = false, amp_on = false, preamp_on = false;
        const char *status_text = "";

        if (ha_switches_for_current()->amp_entity == NULL) {
            status_text = "No power switches are set up for this player.";
        } else if (!HA_TOKEN_SET) {
            status_text = "Enter the Home Assistant token on the web settings page (address in SETTINGS).";
        } else {
            const ha_player_switches_t *sw = ha_switches_for_current();
            amp_known = ha_switch_get_state(sw->amp_entity, &amp_on);
            preamp_known = (sw->preamp_entity != NULL) &&
                ha_switch_get_state(sw->preamp_entity, &preamp_on);
            if (!amp_known && !preamp_known) {
                status_text = "Could not reach Home Assistant - see the serial log";
            }
        }

        ha_state_msg_t m = { .amp_known = amp_known, .amp_on = amp_on,
                             .preamp_known = preamp_known, .preamp_on = preamp_on };
        snprintf(m.status, sizeof(m.status), "%s", status_text);
        ui_post(ha_state_job, &m, sizeof(m));
    }
}

static void power_amp_button_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    const ha_player_switches_t *sw = ha_switches_for_current();
    const bool want_on = !(ha_amp_pending ? ha_amp_pending_on : (ha_amp_state_applied == 1));

    ha_amp_pending = true;
    ha_amp_pending_on = want_on;
    ha_amp_pending_tick = lv_tick_get();
    ha_apply_row_locked(ha_amp_button, ha_amp_label, &ha_amp_state_applied, "AMP", true, want_on, false, false);

    queue_ha_command(sw->amp_entity, want_on);
}

static void power_preamp_button_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    const ha_player_switches_t *sw = ha_switches_for_current();
    const bool want_on = !(ha_preamp_pending ? ha_preamp_pending_on : (ha_preamp_state_applied == 1));

    ha_preamp_pending = true;
    ha_preamp_pending_on = want_on;
    ha_preamp_pending_tick = lv_tick_get();
    ha_apply_row_locked(ha_preamp_button, ha_preamp_label, &ha_preamp_state_applied, "PRE-AMP", true, want_on, false, false);

    queue_ha_command(sw->preamp_entity, want_on);
}

static void power_show(void)
{
    if (power_overlay == NULL) {
        return;
    }

    /* Rebuilt for the currently selected room each time it opens. */
    const ha_player_switches_t *sw = ha_switches_for_current();
    char title[48];
    snprintf(title, sizeof(title), "POWER  -  %s", lyrion_players[current_player_index].short_name);
    lv_label_set_text(power_title_label, title);

    ha_amp_state_applied = -1;
    ha_preamp_state_applied = -1;
    ha_amp_pending = false;
    ha_preamp_pending = false;
    lv_label_set_text(ha_amp_label, "AMP: ?");
    lv_obj_set_style_bg_color(ha_amp_button, lv_color_hex(UI_BUTTON), LV_PART_MAIN);
    lv_obj_set_style_border_color(ha_amp_button, lv_color_hex(UI_EDGE), LV_PART_MAIN);
    /* Players at another location have no switches: hide the AMP row. */
    if (sw->amp_entity == NULL) lv_obj_add_flag(ha_amp_button, LV_OBJ_FLAG_HIDDEN);
    else                        lv_obj_clear_flag(ha_amp_button, LV_OBJ_FLAG_HIDDEN);

    if (sw->preamp_entity != NULL) {
        lv_label_set_text(ha_preamp_label, "PRE-AMP: ?");
        lv_obj_set_style_bg_color(ha_preamp_button, lv_color_hex(UI_BUTTON), LV_PART_MAIN);
        lv_obj_set_style_border_color(ha_preamp_button, lv_color_hex(UI_EDGE), LV_PART_MAIN);
        lv_obj_clear_flag(ha_preamp_button, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(ha_preamp_button, LV_OBJ_FLAG_HIDDEN);
    }

    if (ha_status_label != NULL) {
        lv_label_set_text(ha_status_label, sw->amp_entity == NULL
                          ? "No power switches are set up for this player." : "");
    }

    ha_overlay_open = true;                 /* the background task starts polling */
    lv_obj_clear_flag(power_overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(power_overlay);
}

static void power_hide(void)
{
    if (power_overlay != NULL) {
        ha_overlay_open = false;
        lv_obj_add_flag(power_overlay, LV_OBJ_FLAG_HIDDEN);
        ui_schedule_repaint(lv_screen_active());
    }
}


static void power_close_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) power_hide();
}

static void create_power(lv_obj_t *screen)
{
    power_overlay = lv_obj_create(screen);
    lv_obj_remove_style_all(power_overlay);
    lv_obj_set_size(power_overlay, 560, 340);
    lv_obj_set_pos(power_overlay, 232, 130);
    lv_obj_set_style_bg_color(power_overlay, lv_color_hex(UI_PANEL), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(power_overlay, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(power_overlay, 2, LV_PART_MAIN);
    lv_obj_set_style_border_color(power_overlay, lv_color_hex(UI_AMBER), LV_PART_MAIN);
    lv_obj_set_style_radius(power_overlay, 16, LV_PART_MAIN);
    lv_obj_clear_flag(power_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(power_overlay, LV_OBJ_FLAG_CLICKABLE);

    power_title_label = lv_label_create(power_overlay);
    lv_label_set_text(power_title_label, "POWER");
    lv_obj_set_style_text_color(power_title_label, lv_color_hex(UI_TEXT), 0);
    lv_obj_set_style_text_font(power_title_label, &p4_font_28, 0);
    lv_obj_align(power_title_label, LV_ALIGN_TOP_LEFT, 24, 18);

    lv_obj_t *close = create_button(power_overlay, "CLOSE", 120, 46);
    lv_obj_align(close, LV_ALIGN_TOP_RIGHT, -20, 12);
    lv_obj_add_event_cb(close, power_close_event_cb, LV_EVENT_CLICKED, NULL);

    ha_amp_button = lv_obj_create(power_overlay);
    lv_obj_remove_style_all(ha_amp_button);
    lv_obj_set_size(ha_amp_button, 512, 68);
    lv_obj_set_pos(ha_amp_button, 24, 88);
    lv_obj_set_style_bg_opa(ha_amp_button, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(ha_amp_button, lv_color_hex(UI_BUTTON), LV_PART_MAIN);
    lv_obj_set_style_border_width(ha_amp_button, 2, LV_PART_MAIN);
    lv_obj_set_style_border_color(ha_amp_button, lv_color_hex(UI_EDGE), LV_PART_MAIN);
    lv_obj_set_style_radius(ha_amp_button, 10, LV_PART_MAIN);
    lv_obj_clear_flag(ha_amp_button, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(ha_amp_button, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(ha_amp_button, power_amp_button_event_cb, LV_EVENT_CLICKED, NULL);

    ha_amp_label = lv_label_create(ha_amp_button);
    lv_label_set_text(ha_amp_label, "AMP: ?");
    lv_obj_set_style_text_color(ha_amp_label, lv_color_hex(UI_TEXT), 0);
    lv_obj_set_style_text_font(ha_amp_label, &p4_font_18, 0);
    lv_obj_center(ha_amp_label);

    ha_preamp_button = lv_obj_create(power_overlay);
    lv_obj_remove_style_all(ha_preamp_button);
    lv_obj_set_size(ha_preamp_button, 512, 68);
    lv_obj_set_pos(ha_preamp_button, 24, 172);
    lv_obj_set_style_bg_opa(ha_preamp_button, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(ha_preamp_button, lv_color_hex(UI_BUTTON), LV_PART_MAIN);
    lv_obj_set_style_border_width(ha_preamp_button, 2, LV_PART_MAIN);
    lv_obj_set_style_border_color(ha_preamp_button, lv_color_hex(UI_EDGE), LV_PART_MAIN);
    lv_obj_set_style_radius(ha_preamp_button, 10, LV_PART_MAIN);
    lv_obj_clear_flag(ha_preamp_button, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(ha_preamp_button, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(ha_preamp_button, power_preamp_button_event_cb, LV_EVENT_CLICKED, NULL);

    ha_preamp_label = lv_label_create(ha_preamp_button);
    lv_label_set_text(ha_preamp_label, "PRE-AMP: ?");
    lv_obj_set_style_text_color(ha_preamp_label, lv_color_hex(UI_TEXT), 0);
    lv_obj_set_style_text_font(ha_preamp_label, &p4_font_18, 0);
    lv_obj_center(ha_preamp_label);

    ha_status_label = lv_label_create(power_overlay);
    lv_label_set_text(ha_status_label, "");
    lv_obj_set_width(ha_status_label, 512);
    lv_obj_set_style_text_color(ha_status_label, lv_color_hex(UI_RED), 0);
    lv_obj_set_style_text_font(ha_status_label, &p4_font_16, 0);
    lv_obj_set_pos(ha_status_label, 24, 258);

    lv_obj_t *note = lv_label_create(power_overlay);
    lv_label_set_text(note, "Tap a switch to toggle it.");
    lv_obj_set_style_text_color(note, lv_color_hex(UI_TEXT_DIM), 0);
    lv_obj_set_style_text_font(note, &p4_font_14, 0);
    lv_obj_set_pos(note, 24, 292);

    lv_obj_add_flag(power_overlay, LV_OBJ_FLAG_HIDDEN);
}

static void source_idle_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    const uint32_t idle = lv_display_get_inactive_time(NULL);

    if (source_overlay != NULL && !lv_obj_has_flag(source_overlay, LV_OBJ_FLAG_HIDDEN) &&
        idle >= SOURCE_IDLE_CLOSE_MS) {
        source_hide();
    }
    if (settings_overlay != NULL && !lv_obj_has_flag(settings_overlay, LV_OBJ_FLAG_HIDDEN) &&
        !theme_applying && idle >= SOURCE_IDLE_CLOSE_MS) {
        settings_hide();
    }
    if (wifi_overlay != NULL && !lv_obj_has_flag(wifi_overlay, LV_OBJ_FLAG_HIDDEN) &&
        !wifi_busy && idle >= SOURCE_IDLE_CLOSE_MS) {
        wifi_close();
    }
    if (lms_overlay != NULL && !lv_obj_has_flag(lms_overlay, LV_OBJ_FLAG_HIDDEN) &&
        !wifi_busy && idle >= SOURCE_IDLE_CLOSE_MS) {
        lms_close();
    }
    if (power_overlay != NULL && !lv_obj_has_flag(power_overlay, LV_OBJ_FLAG_HIDDEN) &&
        idle >= SOURCE_IDLE_CLOSE_MS) {
        power_hide();
    }
}

static void source_show(void)
{
    if (source_overlay != NULL) {
        ui_show_panel(source_overlay);
    }
}

/* Which "play" action a browse row stands for, so its "+" can queue it:
 * a song, a playlist or a favourite as-is, and a whole album / artist /
 * genre / folder for rows that would otherwise open that collection. */
static source_mode_t source_queue_mode_for(source_mode_t action)
{
    switch (action) {
        case SOURCE_LOAD_TRACK:
        case SOURCE_LOAD_PLAYLIST:
        case SOURCE_LOAD_FAVORITE:
        case SOURCE_LOAD_FAVORITE_URL: return action;
        case SOURCE_ALBUM_TRACKS:      return SOURCE_LOAD_ALBUM;
        case SOURCE_ARTIST_ALBUMS:     return SOURCE_LOAD_ARTIST;
        case SOURCE_GENRE_ALBUMS:      return SOURCE_LOAD_GENRE;
        case SOURCE_FOLDER_CONTENT:    return SOURCE_LOAD_FOLDER;
        /* App results (a YouTube video, ...) can be queued; radio stations
         * never end, so they only play. */
        case SOURCE_RADIO_PLAY:        return source_menu_is_apps ? SOURCE_RADIO_PLAY : SOURCE_ROOT;
        default:                       return SOURCE_ROOT;   /* nothing to queue */
    }
}

/* Brief confirmation on the "+" button, then back to "+". */
static void source_plus_restore_cb(lv_timer_t *timer)
{
    lv_obj_t *label = (lv_obj_t *)lv_timer_get_user_data(timer);
    if (label != NULL && lv_obj_is_valid(label)) {
        /* Songs now in the queue keep their tick; everything else goes back to "+". */
        lv_label_set_text(label, lv_obj_get_user_data(label) ? LV_SYMBOL_OK : LV_SYMBOL_PLUS);
    }
}

/* Tap "+" = add to the end of the queue; press and hold = play next. */
static void source_style_queued_button(lv_obj_t *b);
static void source_row_show_queued(lv_obj_t *row, int slots);

static void source_plus_event_cb(lv_event_t *e)
{
    const lv_event_code_t code = lv_event_get_code(e);
    if (code != LV_EVENT_SHORT_CLICKED && code != LV_EVENT_LONG_PRESSED) return;

    const int index = (int)(intptr_t)lv_event_get_user_data(e);
    if (index < 0 || index >= source_item_count || source_queue == NULL) return;

    source_item_t *item = &source_items[index];
    const source_mode_t mode = source_queue_mode_for(item->action);
    if (mode == SOURCE_ROOT) return;

    /* A song that is already queued ignores a plain tap, so it can't be added
     * twice by accident.  Holding still adds it again as "play next". */
    if (item->queued && code == LV_EVENT_SHORT_CLICKED) return;

    source_request_t request = {
        .mode = mode,
        .id = item->id,
        .offset = 0,
        .push_history = false,
        .queue_op = (code == LV_EVENT_LONG_PRESSED) ? QUEUE_OP_INSERT : QUEUE_OP_ADD
    };
    snprintf(request.item_id, sizeof(request.item_id), "%s", item->item_id);
    if ((item->action == SOURCE_LOAD_FAVORITE_URL || item->action == SOURCE_RADIO_PLAY) &&
        item->radio_play_json[0]) {
        snprintf(request.radio_action, sizeof(request.radio_action), "%s", item->radio_play_json);
    }
    xQueueSend(source_queue, &request, 0);
    {
        char msg[96];
        snprintf(msg, sizeof(msg), "%s:  %.60s",
                 code == LV_EVENT_LONG_PRESSED ? "Playing next" : "Added to queue", item->title);
        ui_toast(msg);
    }

    lv_obj_t *btn = (lv_obj_t *)lv_event_get_target(e);
    lv_obj_t *label = lv_obj_get_child(btn, 0);
    if (item->action == SOURCE_LOAD_TRACK) {
        item->queued = true;                       /* the tick stays */
        if (label != NULL) lv_obj_set_user_data(label, (void *)1);
        source_style_queued_button(btn);
        source_row_show_queued(lv_obj_get_parent(btn), 1);
    }
    if (label != NULL) {
        lv_label_set_text(label, code == LV_EVENT_LONG_PRESSED ? "NEXT" : LV_SYMBOL_OK);
        lv_timer_t *t = lv_timer_create(source_plus_restore_cb, 1100, label);
        if (t != NULL) lv_timer_set_repeat_count(t, 1);
    }
}

/* "x" on a queue row removes that song. */
static void source_remove_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    const int index = (int)(intptr_t)lv_event_get_user_data(e);
    if (index < 0 || index >= source_item_count || source_queue == NULL) return;

    source_request_t request = {
        .mode = SOURCE_QUEUE_REMOVE,
        .id = source_items[index].id,
        .offset = source_page_offset,
        .push_history = false
    };
    /* Removing the only song on the last page: show the page before it. */
    if (source_item_count == 1 && source_page_offset >= SOURCE_QUEUE_PAGE) {
        request.offset = source_page_offset - SOURCE_QUEUE_PAGE;
    }
    xQueueSend(source_queue, &request, 0);
    ui_toast("Removed from the queue");
}

/* ---- Queue move controls ------------------------------------------------
 * Each queue row has a "..." button; tapping it swaps in  ^  v  NEXT  x  for
 * that song (one row at a time).  A moved song stays selected, so it can be
 * nudged several places with repeated taps. */
#define QUEUE_MOVE_UP   0
#define QUEUE_MOVE_DOWN 1
#define QUEUE_MOVE_NEXT 2

/* Show the controls on the selected row and "..." on the others. */
static void queue_rows_apply_selection(void)
{
    for (int i = 0; i < source_item_count && i < SOURCE_QUEUE_PAGE; i++) {
        lv_obj_t *row = source_item_buttons[i];
        if (row == NULL || queue_row_more[i] == NULL || queue_row_strip[i] == NULL) continue;
        const bool sel = (source_items[i].id == source_queue_sel);
        if (sel) {
            lv_obj_add_flag(queue_row_more[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(queue_row_strip[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_clear_flag(queue_row_more[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(queue_row_strip[i], LV_OBJ_FLAG_HIDDEN);
        }
        lv_obj_set_style_bg_color(row, lv_color_hex(sel ? UI_ACCENT_DARK : UI_BUTTON), LV_PART_MAIN);
        lv_obj_set_style_border_color(row, lv_color_hex(sel ? UI_ACCENT : UI_ACCENT_DARK), LV_PART_MAIN);
        lv_obj_set_style_border_width(row, sel ? 2 : 1, LV_PART_MAIN);
        lv_obj_t *label = lv_obj_get_child(row, 0);
        if (label != NULL) {
            /* row 870 wide, icon margin 48; leave room for "..." or the strip */
            const int strip_w = (source_current_mode == SOURCE_PLAYLIST_TRACKS) ? 200 : 316;
            lv_obj_set_width(label, 870 - 48 - 16 - (sel ? strip_w : 50));
        }
    }
}

static void queue_more_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    const int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i < 0 || i >= source_item_count) return;
    source_queue_sel = (source_queue_sel == source_items[i].id) ? -1 : source_items[i].id;
    queue_rows_apply_selection();
}

/* "4. Artist - Title" -> "Artist - Title" for messages. */
static const char *queue_row_name(const char *text)
{
    const char *p = strstr(text, ". ");
    return p != NULL ? p + 2 : text;
}

static void queue_move_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    const int packed = (int)(intptr_t)lv_event_get_user_data(e);
    const int i = packed >> 2, kind = packed & 3;
    if (i < 0 || i >= source_item_count || source_queue == NULL || source_queue_busy) return;

    const int from = source_items[i].id;
    int to;
    if (kind == QUEUE_MOVE_UP) {
        to = from - 1;
    } else if (kind == QUEUE_MOVE_DOWN) {
        to = from + 1;
    } else {
        if (source_queue_current < 0 || from == source_queue_current) {
            ui_toast("That song is playing now");
            return;
        }
        /* Right after the playing song.  Taking an earlier song out first
         * shifts the playing one up a place, hence the two cases. */
        to = (from > source_queue_current) ? source_queue_current + 1 : source_queue_current;
    }
    if (to < 0 || to >= source_queue_total || to == from) {
        if (kind == QUEUE_MOVE_NEXT) ui_toast("Already playing next");
        return;
    }

    source_queue_busy = true;
    source_request_t request = {
        .mode = SOURCE_QUEUE_MOVE,
        .id = from,
        .queue_op = to,
        .offset = (to / SOURCE_QUEUE_PAGE) * SOURCE_QUEUE_PAGE,   /* follow the song */
        .push_history = false
    };
    if (xQueueSend(source_queue, &request, 0) != pdTRUE) {
        source_queue_busy = false;             /* not sent: don't lock the controls */
        return;
    }
    if (kind == QUEUE_MOVE_NEXT) {
        char msg[96];
        snprintf(msg, sizeof(msg), "Plays next:  %.70s", queue_row_name(source_items[i].title));
        ui_toast(msg);
    }
}

/* One control button inside a row's strip. */
static lv_obj_t *queue_ctrl_button(lv_obj_t *parent, const char *text, int x, int w,
                                   uint32_t color, lv_event_cb_t cb, int user)
{
    lv_obj_t *b = lv_obj_create(parent);
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, w, 36);
    lv_obj_align(b, LV_ALIGN_LEFT_MID, x, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_30, LV_STATE_DISABLED);
    lv_obj_set_style_radius(b, 8, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(UI_ACCENT_GLOW), LV_STATE_PRESSED);
    lv_obj_clear_flag(b, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(b, 4);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, (void *)(intptr_t)user);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_color(l, lv_color_hex(UI_ON_ACCENT), 0);
    lv_obj_set_style_text_font(l, &p4_font_14, 0);
    lv_obj_center(l);
    return b;
}

static void source_fav_send(source_mode_t mode, int id, const char *title, lv_obj_t *btn, bool wide);
static void pl_picker_open(void);                    /* 18f_playlists.c */

/* Queue row list button: this song into one of the saved playlists. */
static void queue_pl_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    const int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i < 0 || i >= source_item_count) return;
    const source_item_t *it = &source_items[i];
    memset(&pl_pending, 0, sizeof(pl_pending));
    if (it->radio_play_json[0]) {
        pl_pending.kind = PL_KIND_URL;
        snprintf(pl_pending.payload, sizeof(pl_pending.payload), "%s", it->radio_play_json);
    } else if (it->artwork_track_id > 0) {
        pl_pending.kind = PL_KIND_TRACKS;
        snprintf(pl_pending.payload, sizeof(pl_pending.payload), "%d", it->artwork_track_id);
    } else {
        ui_toast("This one can't go in a playlist");
        return;
    }
    pl_pending.count = 1;
    snprintf(pl_pending.what, sizeof(pl_pending.what), "%s", queue_row_name(it->title));
    pl_picker_open();
}

/* Queue row star: this one song to Favorites. */
static void queue_fav_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    const int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i < 0 || i >= source_item_count || source_items[i].artwork_track_id <= 0) return;
    ESP_LOGI(TAG, "Queue star: track %d", source_items[i].artwork_track_id);
    source_fav_send(SOURCE_FAV_ADD_TRACK, source_items[i].artwork_track_id,
                    queue_row_name(source_items[i].title), (lv_obj_t *)lv_event_get_target(e), false);
}

/* Add "..." and the (hidden) move strip to queue row i. */
static void queue_row_add_controls(lv_obj_t *row, int i)
{
    const int idx = source_items[i].id;

    lv_obj_t *more = queue_ctrl_button(row, LV_SYMBOL_BARS, 0, 40, UI_BUTTON, queue_more_event_cb, i);
    lv_obj_align(more, LV_ALIGN_RIGHT_MID, -5, 0);
    lv_obj_set_style_border_width(more, 1, 0);
    lv_obj_set_style_border_color(more, lv_color_hex(UI_EDGE), 0);
    lv_obj_set_style_text_color(lv_obj_get_child(more, 0), lv_color_hex(UI_TEXT_SOFT), 0);
    queue_row_more[i] = more;

    /* The strip takes stray taps itself, so a near miss never jumps playback. */
    lv_obj_t *strip = lv_obj_create(row);
    lv_obj_remove_style_all(strip);
    lv_obj_set_size(strip, 308, 40);
    lv_obj_align(strip, LV_ALIGN_RIGHT_MID, -4, 0);
    lv_obj_clear_flag(strip, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(strip, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(strip, LV_OBJ_FLAG_HIDDEN);
    queue_row_strip[i] = strip;

    lv_obj_t *up   = queue_ctrl_button(strip, LV_SYMBOL_UP,   0,   42, UI_ACCENT, queue_move_event_cb, (i << 2) | QUEUE_MOVE_UP);
    lv_obj_t *down = queue_ctrl_button(strip, LV_SYMBOL_DOWN, 50,  42, UI_ACCENT, queue_move_event_cb, (i << 2) | QUEUE_MOVE_DOWN);
    lv_obj_t *next = queue_ctrl_button(strip, "NEXT",         100, 58, UI_ACCENT, queue_move_event_cb, (i << 2) | QUEUE_MOVE_NEXT);
    lv_obj_t *fav = queue_ctrl_button(strip, P4_SYMBOL_STAR, 166, 42, UI_ACCENT, queue_fav_event_cb, i);
    queue_ctrl_button(strip, LV_SYMBOL_LIST "+", 216, 42, UI_ACCENT, queue_pl_event_cb, i);
    queue_ctrl_button(strip, LV_SYMBOL_CLOSE, 266, 42, UI_RED, source_remove_event_cb, i);
    if (source_items[i].artwork_track_id <= 0) lv_obj_add_state(fav, LV_STATE_DISABLED);   /* a stream */

    if (idx <= 0) lv_obj_add_state(up, LV_STATE_DISABLED);
    if (idx >= source_queue_total - 1) lv_obj_add_state(down, LV_STATE_DISABLED);
    if (source_queue_current < 0 || idx == source_queue_current || idx == source_queue_current + 1) {
        lv_obj_add_state(next, LV_STATE_DISABLED);
    }
}

/* Delete a saved playlist: first tap arms (red, "SURE?"), a second tap on the
 * same button within 3 seconds deletes.  Anything else leaves it alone. */
static lv_obj_t *trash_armed_btn = NULL;
static uint32_t  trash_armed_tick = 0;

static void trash_disarm(lv_obj_t *btn)
{
    if (btn != NULL && lv_obj_is_valid(btn)) {
        lv_obj_set_style_bg_color(btn, lv_color_hex(UI_ACCENT), 0);
        lv_obj_t *l = lv_obj_get_child(btn, 0);
        if (l != NULL) lv_label_set_text(l, LV_SYMBOL_TRASH);
    }
    if (trash_armed_btn == btn) trash_armed_btn = NULL;
}

static void trash_timeout_cb(lv_timer_t *timer)
{
    lv_obj_t *btn = (lv_obj_t *)lv_timer_get_user_data(timer);
    if (btn != NULL && btn == trash_armed_btn) trash_disarm(btn);
}

/* Star on a song row / the album view: add it to Favorites. */
static void source_fav_flash_restore_cb(lv_timer_t *t)
{
    lv_obj_t *label = (lv_obj_t *)lv_timer_get_user_data(t);
    if (label != NULL && lv_obj_is_valid(label)) {
        const char *txt = lv_label_get_text(label);
        const bool wide = txt != NULL && strstr(txt, "FAV") != NULL;
        lv_label_set_text(label, wide ? P4_SYMBOL_STAR "  FAV" : P4_SYMBOL_STAR);
    }
}

static void source_fav_send(source_mode_t mode, int id, const char *title, lv_obj_t *btn, bool wide)
{
    if (source_queue == NULL || id <= 0) return;
    source_request_t r = { .mode = mode, .id = id, .push_history = false };
    snprintf(r.title, sizeof(r.title), "%s", title ? title : "");
    xQueueSend(source_queue, &r, 0);
    lv_obj_t *l = btn ? lv_obj_get_child(btn, 0) : NULL;
    if (l != NULL) {
        lv_label_set_text(l, wide ? LV_SYMBOL_OK "  FAV" : LV_SYMBOL_OK);
        lv_timer_t *t = lv_timer_create(source_fav_flash_restore_cb, 1500, l);
        if (t != NULL) lv_timer_set_repeat_count(t, 1);
    }
}

static void __attribute__((unused)) source_fav_star_event_cb(lv_event_t *e)   /* kept for a future per-row star */
{
    if (lv_event_get_code(e) != LV_EVENT_SHORT_CLICKED) return;      /* same events as "+" */
    const int index = (int)(intptr_t)lv_event_get_user_data(e);
    if (index < 0 || index >= source_item_count) return;
    ESP_LOGI(TAG, "Star tapped: track %d (%s)", source_items[index].id, source_items[index].title);
    source_fav_send(SOURCE_FAV_ADD_TRACK, source_items[index].id, source_items[index].title,
                    (lv_obj_t *)lv_event_get_target(e), false);
}

static void source_album_fav_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    source_fav_send(SOURCE_FAV_ADD_ALBUM, source_parent_id, source_current_title,
                    (lv_obj_t *)lv_event_get_target(e), true);
}

static void source_playlist_trash_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED || source_queue == NULL) return;
    const int index = (int)(intptr_t)lv_event_get_user_data(e);
    if (index < 0 || index >= source_item_count) return;
    lv_obj_t *btn = (lv_obj_t *)lv_event_get_target(e);

    if (trash_armed_btn != btn || lv_tick_elaps(trash_armed_tick) > 3000) {
        if (trash_armed_btn != NULL) trash_disarm(trash_armed_btn);   /* only one armed at a time */
        trash_armed_btn = btn;
        trash_armed_tick = lv_tick_get();
        lv_obj_set_style_bg_color(btn, lv_color_hex(UI_RED), 0);
        lv_obj_t *l = lv_obj_get_child(btn, 0);
        if (l != NULL) lv_label_set_text(l, "SURE?");
        lv_timer_t *t = lv_timer_create(trash_timeout_cb, 3000, btn);
        if (t != NULL) lv_timer_set_repeat_count(t, 1);
        return;
    }

    trash_armed_btn = NULL;
    if (source_current_mode == SOURCE_FAVORITES || source_current_mode == SOURCE_FAVORITE_CONTENT) {
        source_request_t fr = {
            .mode = SOURCE_FAV_DELETE,
            .offset = source_page_offset,
            .push_history = false,
            .queue_op = (int)source_current_mode              /* the view to redraw */
        };
        snprintf(fr.item_id, sizeof(fr.item_id), "%s", source_items[index].fav_id);
        if (!fr.item_id[0]) {
            snprintf(fr.radio_action, sizeof(fr.radio_action), "%s", source_items[index].radio_play_json);
        }
        snprintf(fr.title, sizeof(fr.title), "%s", source_parent_item_id);
        if (source_item_count == 1 && source_page_offset >= SOURCE_MAX_ITEMS) {
            fr.offset = source_page_offset - SOURCE_MAX_ITEMS;
        }
        lv_obj_t *fl = lv_obj_get_child(btn, 0);
        if (fl != NULL) lv_label_set_text(fl, "...");
        xQueueSend(source_queue, &fr, 0);
        return;
    }
    source_request_t request = {
        .mode = SOURCE_PLAYLIST_DELETE,
        .id = source_items[index].id,
        .offset = source_page_offset,
        .push_history = false
    };
    snprintf(request.title, sizeof(request.title), "%s", source_items[index].title);
    /* Deleting the only playlist on the last page: show the page before it. */
    if (source_item_count == 1 && source_page_offset >= SOURCE_MAX_ITEMS) {
        request.offset = source_page_offset - SOURCE_MAX_ITEMS;
    }
    lv_obj_t *l = lv_obj_get_child(btn, 0);
    if (l != NULL) lv_label_set_text(l, "...");
    xQueueSend(source_queue, &request, 0);
}

/* A small square button on the right edge of a row. */
/* slot 0 is the right-most button, slot 1 sits just to its left. */
static lv_obj_t *source_add_row_button(lv_obj_t *row, const char *text, lv_event_cb_t cb,
                                       int index, bool long_press_too, int slot)
{
    /* Read the sizes as set: the rows were only just created, so LVGL has
     * not laid them out yet and their measured size would still be 0. */
    const int32_t h = lv_obj_get_style_height(row, LV_PART_MAIN);
    const int32_t size = h - 10;

    lv_obj_t *label = lv_obj_get_child(row, 0);
    if (label != NULL) {
        const int32_t w = lv_obj_get_style_width(label, LV_PART_MAIN);
        if (w > size + 40) {
            lv_obj_set_width(label, w - size - 8);
        }
    }

    lv_obj_t *b = lv_obj_create(row);
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, size, size);
    lv_obj_align(b, LV_ALIGN_RIGHT_MID, -5 - slot * (size + 12), 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(UI_ACCENT), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(b, 8, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(UI_ACCENT_DARK), LV_STATE_PRESSED);
    lv_obj_clear_flag(b, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(b, 6);

    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_color(l, lv_color_hex(UI_ON_ACCENT), 0);
    lv_obj_set_style_text_font(l, &p4_font_14, 0);
    lv_obj_center(l);

    if (long_press_too) {
        lv_obj_add_event_cb(b, cb, LV_EVENT_SHORT_CLICKED, (void *)(intptr_t)index);
        lv_obj_add_event_cb(b, cb, LV_EVENT_LONG_PRESSED, (void *)(intptr_t)index);
    } else {
        lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, (void *)(intptr_t)index);
    }
    return b;
}


/* ---- In-queue marks, stations and SELECT ------------------------------- */

/* A favorite that is an internet stream (not a file or a library item). */
static bool source_favorite_is_stream(const source_item_t *it)
{
    if (it->action != SOURCE_LOAD_FAVORITE_URL) return false;
    const char *u = it->radio_play_json[0] ? it->radio_play_json : it->item_id;
    return !(strncmp(u, "file:", 5) == 0 || strncmp(u, "db:", 3) == 0 || u[0] == '\0');
}

/* The tick of a song that is already queued: outlined instead of filled. */
static void source_style_queued_button(lv_obj_t *b)
{
    if (b == NULL) return;
    lv_obj_set_style_bg_color(b, lv_color_hex(UI_ACCENT_DARK), 0);
    lv_obj_set_style_border_width(b, 1, 0);
    lv_obj_set_style_border_color(b, lv_color_hex(UI_ACCENT), 0);
    lv_obj_t *l = lv_obj_get_child(b, 0);
    if (l != NULL) lv_obj_set_style_text_color(l, lv_color_hex(UI_ACCENT), 0);
}

/* Amber title and a small "IN QUEUE" left of the row's buttons. */
static void source_row_show_queued(lv_obj_t *row, int slots)
{
    if (row == NULL || lv_obj_get_user_data(row) == (void *)2) return;   /* already marked */
    lv_obj_set_user_data(row, (void *)2);
    lv_obj_t *title = lv_obj_get_child(row, 0);
    const int32_t size = lv_obj_get_style_height(row, LV_PART_MAIN) - 10;
    if (title != NULL) {
        lv_obj_set_style_text_color(title, lv_color_hex(UI_ACCENT_GLOW), 0);
        const int32_t w = lv_obj_get_style_width(title, LV_PART_MAIN);
        if (w > 160) lv_obj_set_width(title, w - 78);
    }
    lv_obj_t *tag = lv_label_create(row);
    lv_label_set_text(tag, "IN QUEUE");
    lv_obj_set_style_text_font(tag, &p4_font_14, 0);
    lv_obj_set_style_text_color(tag, lv_color_hex(UI_ACCENT_GLOW), 0);
    lv_obj_align(tag, LV_ALIGN_RIGHT_MID, -5 - slots * (size + 12) - 4, 0);
}

static int source_view_key(void)
{
    return (int)source_current_mode * 1000003 + source_parent_id * 7 + source_page_offset;
}

static bool source_sel_has(int id)
{
    for (int k = 0; k < source_sel_count; k++) if (source_sel_ids[k] == id) return true;
    return false;
}

static void source_select_refresh_bar(void)
{
    if (source_select_count_label == NULL) return;
    char t[32];
    snprintf(t, sizeof(t), "%d selected", source_sel_count);
    lv_label_set_text(source_select_count_label, t);
    for (uint32_t k = 1; k < lv_obj_get_child_count(source_select_bar); k++) {
        lv_obj_t *b = lv_obj_get_child(source_select_bar, (int32_t)k);
        if (lv_obj_get_user_data(b) == (void *)9) continue;              /* DONE */
        if (source_sel_count > 0) lv_obj_clear_state(b, LV_STATE_DISABLED);
        else                      lv_obj_add_state(b, LV_STATE_DISABLED);
    }
}

static void source_render_items(void);

/* Turn SELECT on or off (off also forgets the ticks) and redraw. */
static void source_select_set(bool on)
{
    source_select_mode = on;
    source_sel_count = 0;
    source_sel_view_key = on ? source_view_key() : -1;
    if (source_select_bar != NULL) {
        if (on) { lv_obj_clear_flag(source_select_bar, LV_OBJ_FLAG_HIDDEN); lv_obj_move_foreground(source_select_bar); }
        else    lv_obj_add_flag(source_select_bar, LV_OBJ_FLAG_HIDDEN);
    }
    source_select_refresh_bar();
}

/* Round tick at the right of a row in SELECT mode. */
static void source_add_select_circle(lv_obj_t *row, bool on)
{
    const int32_t h = lv_obj_get_style_height(row, LV_PART_MAIN);
    const int32_t size = h - 14;
    lv_obj_t *title = lv_obj_get_child(row, 0);
    if (title != NULL) {
        const int32_t w = lv_obj_get_style_width(title, LV_PART_MAIN);
        if (w > size + 40) lv_obj_set_width(title, w - size - 12);
    }
    lv_obj_t *c = lv_obj_create(row);
    lv_obj_remove_style_all(c);
    lv_obj_set_size(c, size, size);
    lv_obj_align(c, LV_ALIGN_RIGHT_MID, -9, 0);
    lv_obj_set_style_radius(c, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(c, 2, 0);
    lv_obj_clear_flag(c, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *l = lv_label_create(c);
    lv_label_set_text(l, LV_SYMBOL_OK);
    lv_obj_set_style_text_font(l, &p4_font_14, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(UI_ON_ACCENT), 0);
    lv_obj_center(l);
    lv_obj_set_user_data(c, (void *)7);                       /* marks the circle */
    /* colours */
    lv_obj_set_style_bg_opa(c, on ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_color(c, lv_color_hex(UI_ACCENT), 0);
    lv_obj_set_style_border_color(c, lv_color_hex(on ? UI_ACCENT : UI_TEXT_DIM), 0);
    if (on) lv_obj_clear_flag(l, LV_OBJ_FLAG_HIDDEN); else lv_obj_add_flag(l, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_bg_color(row, lv_color_hex(on ? UI_ACCENT_DARK : UI_BUTTON), LV_PART_MAIN);
    lv_obj_set_style_border_color(row, lv_color_hex(on ? UI_ACCENT : UI_EDGE), LV_PART_MAIN);
}

/* SELECT: a tap on a song row ticks / unticks it. */
static void source_select_toggle(lv_obj_t *row, int index)
{
    const int id = source_items[index].id;
    bool on;
    if (source_sel_has(id)) {
        for (int k = 0; k < source_sel_count; k++) {
            if (source_sel_ids[k] == id) { source_sel_ids[k] = source_sel_ids[--source_sel_count]; break; }
        }
        on = false;
    } else {
        if (source_sel_count >= SOURCE_SEL_MAX) { ui_toast("That's the most songs at once (100)"); return; }
        source_sel_ids[source_sel_count++] = id;
        on = true;
    }
    for (uint32_t k = 0; k < lv_obj_get_child_count(row); k++) {
        lv_obj_t *c = lv_obj_get_child(row, (int32_t)k);
        if (lv_obj_get_user_data(c) != (void *)7) continue;
        lv_obj_set_style_bg_opa(c, on ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_color(c, lv_color_hex(on ? UI_ACCENT : UI_TEXT_DIM), 0);
        lv_obj_t *l = lv_obj_get_child(c, 0);
        if (l != NULL) { if (on) lv_obj_clear_flag(l, LV_OBJ_FLAG_HIDDEN); else lv_obj_add_flag(l, LV_OBJ_FLAG_HIDDEN); }
    }
    lv_obj_set_style_bg_color(row, lv_color_hex(on ? UI_ACCENT_DARK : UI_BUTTON), LV_PART_MAIN);
    lv_obj_set_style_border_color(row, lv_color_hex(on ? UI_ACCENT : UI_EDGE), LV_PART_MAIN);
    source_select_refresh_bar();
}

/* "12,13,14" from the ticked songs. */
static void source_sel_id_list(char *out, size_t out_size)
{
    size_t n = 0;
    out[0] = '\0';
    for (int k = 0; k < source_sel_count && n + 12 < out_size; k++) {
        n += (size_t)snprintf(out + n, out_size - n, "%s%d", k ? "," : "", source_sel_ids[k]);
    }
}

static void source_select_action_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED || source_queue == NULL) return;
    const int what = (int)(intptr_t)lv_event_get_user_data(e);   /* 0 queue, 1 next, 2 fav, 3 playlist, 9 done */
    if (what == 9 || source_sel_count == 0) {
        source_select_set(false);
        source_render_items();
        return;
    }
    if (what == 3) {                                   /* choose a playlist for them */
        memset(&pl_pending, 0, sizeof(pl_pending));
        pl_pending.kind = PL_KIND_TRACKS;
        pl_pending.count = source_sel_count;
        source_sel_id_list(pl_pending.payload, sizeof(pl_pending.payload));
        snprintf(pl_pending.what, sizeof(pl_pending.what), "%d song%s", source_sel_count,
                 source_sel_count == 1 ? "" : "s");
        source_select_set(false);
        source_render_items();
        pl_picker_open();
        return;
    }
    source_request_t r = { .mode = (what == 2) ? SOURCE_SELECT_FAV : SOURCE_SELECT_QUEUE,
                           .id = source_sel_count, .push_history = false,
                           .queue_op = (what == 1) ? QUEUE_OP_INSERT : QUEUE_OP_ADD };
    source_sel_id_list(r.radio_action, sizeof(r.radio_action));
    xQueueSend(source_queue, &r, 0);
    char msg[64];
    if (what == 2) snprintf(msg, sizeof(msg), "Adding %d song%s to Favorites...", source_sel_count, source_sel_count == 1 ? "" : "s");
    else snprintf(msg, sizeof(msg), "%s %d song%s", what == 1 ? "Playing next:" : "Added to queue:",
                  source_sel_count, source_sel_count == 1 ? "" : "s");
    ui_toast(msg);
    if (what != 2) {                                   /* they're queued now */
        for (int i = 0; i < source_item_count; i++) {
            if (source_items[i].action == SOURCE_LOAD_TRACK && source_sel_has(source_items[i].id)) {
                source_items[i].queued = true;
            }
        }
    }
    source_select_set(false);
    source_render_items();
}

static void source_select_button_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    source_select_set(true);
    source_render_items();
}

/* Build the SELECT button and bar inside the bottom bar (called once). */
static void source_select_create(lv_obj_t *nav_bar)
{
    source_select_button = create_button(nav_bar, LV_SYMBOL_OK "  SELECT", 200, 44);
    lv_obj_set_pos(source_select_button, 705, 8);
    lv_obj_add_event_cb(source_select_button, source_select_button_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_flag(source_select_button, LV_OBJ_FLAG_HIDDEN);

    source_select_bar = lv_obj_create(nav_bar);
    lv_obj_remove_style_all(source_select_bar);
    lv_obj_set_size(source_select_bar, 930, 76);
    lv_obj_set_pos(source_select_bar, 0, 1);
    lv_obj_set_style_bg_color(source_select_bar, lv_color_hex(UI_PANEL), 0);
    lv_obj_set_style_bg_opa(source_select_bar, LV_OPA_COVER, 0);
    lv_obj_clear_flag(source_select_bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(source_select_bar, LV_OBJ_FLAG_CLICKABLE);      /* nothing below reacts */

    source_select_count_label = lv_label_create(source_select_bar);
    lv_obj_set_style_text_font(source_select_count_label, &p4_font_18, 0);
    lv_obj_set_style_text_color(source_select_count_label, lv_color_hex(UI_TEXT), 0);
    lv_obj_set_pos(source_select_count_label, 20, 26);

    static const char *const names[5] = { LV_SYMBOL_PLUS "  QUEUE", LV_SYMBOL_PLAY "  NEXT",
                                          P4_SYMBOL_STAR "  FAV", LV_SYMBOL_LIST "  PLAYLIST", "DONE" };
    static const int xs[5] = { 170, 328, 462, 596, 770 }, ws[5] = { 148, 124, 124, 164, 140 },
                     ids[5] = { 0, 1, 2, 3, 9 };
    for (int k = 0; k < 5; k++) {
        lv_obj_t *b = create_button(source_select_bar, names[k], ws[k], 44);
        lv_obj_set_pos(b, xs[k], 7);
        lv_obj_set_user_data(b, (void *)(intptr_t)ids[k]);
        lv_obj_add_event_cb(b, source_select_action_event_cb, LV_EVENT_CLICKED, (void *)(intptr_t)ids[k]);
        if (k == 0) {
            lv_obj_set_style_bg_color(b, lv_color_hex(UI_ACCENT), LV_PART_MAIN);
            lv_obj_set_style_text_color(b, lv_color_hex(UI_ON_ACCENT), LV_PART_MAIN);
            lv_obj_set_style_text_color(b, lv_color_hex(UI_TEXT_DIM), LV_PART_MAIN | LV_STATE_DISABLED);
            button_text_follows_state(b);
        }
    }
    lv_obj_add_flag(source_select_bar, LV_OBJ_FLAG_HIDDEN);
}

static void wp_open(int stage, const char *ssid, int auth);   /* keyboard prompt, 16_wifi_picker.c */

static void source_item_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;

    int index = (int)(intptr_t)lv_event_get_user_data(e);
    if (index < 0 || index >= source_item_count) return;

    source_item_t item = source_items[index];
    if (source_queue == NULL) return;

    if (source_select_mode && item.action == SOURCE_LOAD_TRACK && item.id > 0) {
        source_select_toggle((lv_obj_t *)lv_event_get_target(e), index);
        return;
    }

    /* Sorting the queue or a playlist: while one song's move controls are
     * open, tapping another song moves the controls to it (instead of
     * playing it), and tapping the open song closes them again. */
    if ((source_current_mode == SOURCE_QUEUE || source_current_mode == SOURCE_PLAYLIST_TRACKS) &&
        source_queue_sel >= 0) {
        bool open_here = false;
        for (int k = 0; k < source_item_count && k < SOURCE_QUEUE_PAGE; k++) {
            if (source_items[k].id == source_queue_sel) { open_here = true; break; }
        }
        if (open_here) {
            source_queue_sel = (item.id == source_queue_sel) ? -1 : item.id;
            queue_rows_apply_selection();
            return;
        }
    }

    /* Library home "Search" tile: ask what to look for (keyboard). */
    if (item.action == SOURCE_SEARCH_PROMPT) {
        wp_open(3, "", -1);
        return;
    }

    /* Opening a list/folder/album pushes the current view on the history
     * stack (done by the browser task once the new view loads). */
    source_request_t request = {
        .mode = item.action,
        .id = item.id,
        .offset = 0,
        .push_history = true
    };
    snprintf(request.item_id, sizeof(request.item_id), "%s", item.item_id);
    snprintf(request.title, sizeof(request.title), "%s", item.title);
    if (item.action == SOURCE_LOAD_FAVORITE_URL && item.radio_play_json[0]) {
        /* the full address (item_id holds only the first 95 characters) */
        snprintf(request.radio_action, sizeof(request.radio_action), "%s", item.radio_play_json);
    }
    /* Search results carry an "Artist:" / "Album:" prefix; the view opened
     * from them should show just the name. */
    if (source_current_mode == SOURCE_SEARCH) {
        static const char *const prefixes[] = { "Artist:  ", "Album:  " };
        for (int k = 0; k < 2; k++) {
            const size_t pl = strlen(prefixes[k]);
            if (strncmp(request.title, prefixes[k], pl) == 0) {
                memmove(request.title, request.title + pl, strlen(request.title + pl) + 1);
            }
        }
    }

    if (item.action == SOURCE_RADIO_PLAY) {
        /* Radio play actions can be much longer than a normal Favorites item_id.
         * Keep the full LMS action in the dedicated radio_action buffer. */
        request.push_history = false;
        snprintf(request.radio_action, sizeof(request.radio_action), "%s",
                 item.radio_play_json);
        xQueueSend(source_queue, &request, 0);
        return;
    } else if (item.action == SOURCE_RADIO_MENU && item.needs_input && item.radio_go_json[0]) {
        /* A search inside an app (YouTube search, ...): ask for the words first. */
        snprintf(source_app_input_action, sizeof(source_app_input_action), "%s", item.radio_go_json);
        wp_open(4, item.title, -1);
        return;
    } else if (item.action == SOURCE_RADIO_MENU) {
        if (!item.radio_go_json[0]) {
            /* A Library tile: Internet Radio or Apps. */
            source_menu_is_apps = (item.id == 2);
        }
        /* Radio plugin menus are delivered using SlimBrowse action objects.
         * Keep the action exactly as LMS sent it, and let the source task
         * translate it into a JSON-RPC request.  Radio sub-menus have their
         * own back-stack (source_radio_parent_commands). */
        if (item.radio_go_json[0]) {
            request.push_history = false;
            if (source_radio_depth < 6) {
                snprintf(source_radio_parent_commands[source_radio_depth],
                         sizeof(source_radio_parent_commands[0]),
                         "%s", source_radio_current_action);
                source_radio_depth++;
            }
            snprintf(source_radio_current_action, sizeof(source_radio_current_action),
                     "%s", item.radio_go_json);
            snprintf(request.radio_action, sizeof(request.radio_action), "%s", item.radio_go_json);
            xQueueSend(source_queue, &request, 0);
            return;
        }
    } else if (item.action == SOURCE_QUEUE_JUMP) {
        request.push_history = false;
        request.offset = source_page_offset;      /* redraw the same page after */
    } else if (item.action == SOURCE_LOAD_PLAYLIST && item.id > 0) {
        /* A saved playlist opens to show its songs (its "+" still queues it all). */
        request.mode = SOURCE_PLAYLIST_TRACKS;
        request.push_history = true;
    } else if (item.action == SOURCE_PL_PLAY_FROM) {
        /* A song in a playlist: play the playlist from here. */
        request.id = source_parent_id;
        request.queue_op = item.id;
        request.push_history = false;
    } else if (item.action == SOURCE_LOAD_PLAYLIST ||
               item.action == SOURCE_LOAD_TRACK ||
               item.action == SOURCE_LOAD_FAVORITE ||
               item.action == SOURCE_LOAD_FAVORITE_URL) {
        /* Loading something to play closes the browser; nothing to go back to. */
        request.push_history = false;
    }

    xQueueSend(source_queue, &request, 0);
}

static void source_back_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (source_queue == NULL) return;

    if (source_current_mode == SOURCE_ROOT) {
        source_hide();
        return;
    }

    source_request_t request;
    memset(&request, 0, sizeof(request));
    request.mode = SOURCE_ROOT;
    request.push_history = false;

    if (source_current_mode == SOURCE_RADIO_MENU && source_radio_depth > 0) {
        /* Step back through the radio sub-menus first. */
        source_radio_depth--;
        request.mode = SOURCE_RADIO_MENU;
        snprintf(source_radio_current_action, sizeof(source_radio_current_action),
                 "%s", source_radio_parent_commands[source_radio_depth]);
        snprintf(request.radio_action, sizeof(request.radio_action),
                 "%s", source_radio_current_action);
    } else if (source_nav_depth > 0) {
        /* Return to the exact view the user came from. */
        const source_nav_frame_t *frame = &source_nav_stack[--source_nav_depth];
        request.mode = frame->mode;
        request.id = frame->id;
        request.offset = frame->offset;
        snprintf(request.item_id, sizeof(request.item_id), "%s", frame->item_id);
        snprintf(request.title, sizeof(request.title), "%s", frame->title);
        if (request.mode != SOURCE_RADIO_MENU) {
            source_radio_current_action[0] = '\0';
            source_radio_depth = 0;
        }
    } else {
        source_radio_current_action[0] = '\0';
        source_radio_depth = 0;
    }

    xQueueSend(source_queue, &request, 0);
}

static void source_close_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        source_hide();
    }
}

/* The album/folder detail view uses a name label, a caption, a track count and
 * a scrolling list.  Every other screen must hide them: previously only the
 * normal list screen did, so leaving an album view by way of the SOURCE home
 * screen (or an error message) left the album name showing behind the
 * buttons. */
static void source_hide_detail_widgets(void)
{
    if (source_detail_album_caption != NULL) lv_obj_add_flag(source_detail_album_caption, LV_OBJ_FLAG_HIDDEN);
    if (source_detail_album_label != NULL)   lv_obj_add_flag(source_detail_album_label, LV_OBJ_FLAG_HIDDEN);
    if (source_detail_fav_button != NULL)    lv_obj_add_flag(source_detail_fav_button, LV_OBJ_FLAG_HIDDEN);
    if (source_detail_track_count != NULL)   lv_obj_add_flag(source_detail_track_count, LV_OBJ_FLAG_HIDDEN);
    if (source_track_list != NULL)           lv_obj_add_flag(source_track_list, LV_OBJ_FLAG_HIDDEN);
    source_clear_art();
}

static void source_diag_log(const char *what)
{
#if SRC_DIAG
    ESP_LOGI(TAG,
             "SRCDIAG %s: mode=%d items=%d overlay_children=%u list_children=%u "
             "internal_free=%u psram_free=%u",
             what, (int)source_current_mode, source_item_count,
             source_overlay ? (unsigned)lv_obj_get_child_count(source_overlay) : 0U,
             source_track_list ? (unsigned)lv_obj_get_child_count(source_track_list) : 0U,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
#else
    (void)what;
#endif
}

static void source_show_root(void)
{
    source_current_mode = SOURCE_ROOT;
    source_parent_id = 0;
    source_parent_item_id[0] = '\0';
    source_nav_depth = 0;
    source_radio_depth = 0;
    source_radio_current_action[0] = '\0';
    source_page_offset = 0;
    source_has_next = false;

    source_set_title("MUSIC SOURCE");

    source_hide_detail_widgets();

    source_clear_items();

    static const struct {
        const char *name;
        source_mode_t mode;
    } entries[] = {
        { "Artists",        SOURCE_ARTISTS },
        { "Albums",         SOURCE_ALBUMS },
        { "Genres",         SOURCE_GENRES },
        { "Music Folders",  SOURCE_FOLDERS },
        { "Favorites",      SOURCE_FAVORITES },
#if SOURCE_ROOT_SHOW_PLAYLISTS
        { "Playlists",      SOURCE_PLAYLISTS },
#endif
#if SOURCE_ROOT_SHOW_RADIO
        { "Internet Radio", SOURCE_RADIO_MENU },
#endif
        { "Apps",           SOURCE_RADIO_MENU },       /* Lyrion's apps: YouTube, ... */
        { "Search",         SOURCE_SEARCH_PROMPT },
    };
    const int entry_count = (int)(sizeof(entries) / sizeof(entries[0]));

    for (int i = 0; i < entry_count; i++) {
        memset(&source_items[i], 0, sizeof(source_items[i]));
        source_items[i].action = entries[i].mode;
        source_items[i].id = (strcmp(entries[i].name, "Apps") == 0) ? 2 : 0;   /* 2 = the Apps menu */
        snprintf(source_items[i].title, SOURCE_TEXT_MAX, "%s", entries[i].name);
    }
    source_item_count = entry_count;

    for (int i = 0; i < source_item_count; i++) {
        int col = i % 2;
        int row = i / 2;

        source_item_buttons[i] = create_button(
            source_overlay,
            source_items[i].title,
            SOURCE_BUTTON_W,
            SOURCE_BUTTON_H
        );

        lv_obj_set_pos(
            source_item_buttons[i],
            SOURCE_GRID_X0 + col * SOURCE_GRID_DX,
            100 + row * 88
        );

        /* Large, easy-to-hit home buttons. */
        lv_obj_set_style_text_font(
            lv_obj_get_child(source_item_buttons[i], 0),
            &p4_font_28,
            0
        );

        lv_obj_add_event_cb(
            source_item_buttons[i],
            source_item_event_cb,
            LV_EVENT_CLICKED,
            (void *)(intptr_t)i
        );
    }

    if (source_empty_label != NULL) {
        lv_obj_add_flag(source_empty_label, LV_OBJ_FLAG_HIDDEN);
    }

    if (source_nav_bar != NULL) {
        lv_obj_move_foreground(source_nav_bar);
    }

    source_update_page_controls();
    source_show();
    source_diag_log("root");
    ui_schedule_repaint(source_overlay);
}

static bool http_post_json(const char *url, const char *command_json,
                           char *response_buffer, size_t response_size)
{
    if (command_json == NULL || response_buffer == NULL || response_size < 2) {
        return false;
    }

    /* The capacity matters: the shared HTTP handler used to cap every reply at
     * LYRION_RESPONSE_MAX (8 KB) even when a bigger buffer was supplied, which
     * silently truncated long folder/album/radio listings. */
    lyrion_response_t response = {
        .data = response_buffer,
        .length = 0,
        .capacity = response_size,
        .truncated = false
    };

    response_buffer[0] = '\0';

    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .event_handler = lyrion_http_event_handler,
        .user_data = &response,
        .timeout_ms = 20000       /* app searches (YouTube, ...) can take a while */
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) return false;

    esp_err_t err = esp_http_client_set_header(
        client, "Content-Type", "application/json"
    );

    if (err == ESP_OK) {
        err = esp_http_client_set_post_field(
            client, command_json, strlen(command_json)
        );
    }

    if (err == ESP_OK) {
        err = esp_http_client_perform(client);
    }

    int status_code =
        err == ESP_OK ? esp_http_client_get_status_code(client) : 0;

    esp_http_client_cleanup(client);

    if (err != ESP_OK || status_code < 200 || status_code >= 300) {
        ESP_LOGW(TAG, "Source query failed: %s HTTP=%d",
                 esp_err_to_name(err), status_code);
        return false;
    }

    if (response.truncated) {
        ESP_LOGW(TAG, "Source reply larger than the %u byte buffer",
                 (unsigned)response_size);
        return false;
    }

    return response.length > 0;
}

/* POST a JSON-RPC request to the current location's Lyrion server. */
static bool source_http_query(int player_index, const char *command_json,
                              char *response_buffer, size_t response_size)
{
    (void)player_index;
    return http_post_json(LYRION_SERVER_URL, command_json, response_buffer, response_size);
}

static int source_json_int(const cJSON *item)
{
    if (item == NULL) return 0;
    if (cJSON_IsNumber(item)) return item->valueint;
    if (cJSON_IsString(item) && item->valuestring != NULL) {
        char *end = NULL;
        long value = strtol(item->valuestring, &end, 10);
        if (end != item->valuestring) return (int)value;
    }
    return 0;
}

static void source_add_item(
    int id,
    source_mode_t action,
    const char *title)
{
    if (source_item_count >= SOURCE_MAX_ITEMS || title == NULL) return;

    source_item_t *item = &source_items[source_item_count++];
    item->id = id;
    item->action = action;
    item->queued = false;
    item->artwork_track_id = 0;
    item->subtitle[0] = '\0';
    item->cover_key[0] = '\0';
    item->item_id[0] = '\0';
    snprintf(item->title, SOURCE_TEXT_MAX, "%s", title);
}

static void source_add_favorite_item(
    const char *item_id,
    source_mode_t action,
    const char *title)
{
    if (source_item_count >= SOURCE_MAX_ITEMS || title == NULL || item_id == NULL) return;

    source_item_t *item = &source_items[source_item_count++];
    item->id = 0;
    item->action = action;
    item->queued = false;
    item->fav_id[0] = '\0';
    item->radio_play_json[0] = '\0';
    snprintf(item->item_id, sizeof(item->item_id), "%s", item_id);
    snprintf(item->title, SOURCE_TEXT_MAX, "%s", title);
}

/* Characters in main/fonts/p4_font_14..28 (besides ASCII). */
static bool source_font_has(uint32_t cp)
{
    if (cp >= 0xA0 && cp <= 0x17F) return cp != 0xAD;
    switch (cp) {
        case 0x2013: case 0x2014: case 0x2018: case 0x2019: case 0x201A:
        case 0x201C: case 0x201D: case 0x201E: case 0x2020: case 0x2022:
        case 0x2026: case 0x2039: case 0x203A: case 0x20AC: case 0x2122:
            return true;
        default:
            return false;
    }
}

static void source_clean_display_text(const char *input, char *output, size_t output_size)
{
    if (output == NULL || output_size == 0) return;
    output[0] = '\0';
    if (input == NULL) return;

    /* Display-only UTF-8 normalization. The built-in Montserrat fonts do not
     * contain every Unicode glyph found in music metadata, so LVGL can show
     * a missing-glyph box. Convert common Latin/European characters and
     * punctuation to safe ASCII. Actual LMS metadata is never modified. */
    size_t out = 0;
    const unsigned char *p = (const unsigned char *)input;

    while (*p != 0 && out + 1 < output_size) {
        uint32_t cp = 0;
        size_t consumed = 1;

        if (p[0] < 0x80) {
            cp = p[0];
        } else if (p[0] >= 0xC2 && p[0] <= 0xDF &&
                   p[1] >= 0x80 && p[1] <= 0xBF) {
            cp = ((uint32_t)(p[0] & 0x1F) << 6) |
                 (uint32_t)(p[1] & 0x3F);
            consumed = 2;
        } else if (p[0] >= 0xE0 && p[0] <= 0xEF &&
                   p[1] >= 0x80 && p[1] <= 0xBF &&
                   p[2] >= 0x80 && p[2] <= 0xBF) {
            cp = ((uint32_t)(p[0] & 0x0F) << 12) |
                 ((uint32_t)(p[1] & 0x3F) << 6) |
                 (uint32_t)(p[2] & 0x3F);
            consumed = 3;
        } else if (p[0] >= 0xF0 && p[0] <= 0xF4 &&
                   p[1] >= 0x80 && p[1] <= 0xBF &&
                   p[2] >= 0x80 && p[2] <= 0xBF &&
                   p[3] >= 0x80 && p[3] <= 0xBF) {
            cp = 0xFFFD;
            consumed = 4;
        } else {
            p++;
            continue;
        }

        /* The p4 fonts (14-28 px) carry Latin-1, Latin Extended-A and common
         * punctuation: keep those characters exactly as Lyrion sent them. */
        if (source_font_has(cp)) {
            if (out + consumed >= output_size) break;
            memcpy(output + out, p, consumed);
            out += consumed;
            p += consumed;
            continue;
        }

        char replacement = 0;

        switch (cp) {
            /* Punctuation. */
            case 0x00A0: replacement = ' '; break;
            case 0x00A3: replacement = 'L'; break;
            case 0x2018: case 0x2019: replacement = '\''; break;
            case 0x201C: case 0x201D: replacement = '"'; break;
            case 0x2013: case 0x2014: case 0x2212: replacement = '-'; break;

            /* Latin-1 uppercase. */
            case 0x00C0: case 0x00C1: case 0x00C2: case 0x00C3:
            case 0x00C4: case 0x00C5: case 0x00C6: replacement = 'A'; break;
            case 0x00C7: replacement = 'C'; break;
            case 0x00C8: case 0x00C9: case 0x00CA: case 0x00CB: replacement = 'E'; break;
            case 0x00CC: case 0x00CD: case 0x00CE: case 0x00CF: replacement = 'I'; break;
            case 0x00D0: replacement = 'D'; break;
            case 0x00D1: replacement = 'N'; break;
            case 0x00D2: case 0x00D3: case 0x00D4: case 0x00D5:
            case 0x00D6: case 0x00D8: replacement = 'O'; break;
            case 0x00D9: case 0x00DA: case 0x00DB: case 0x00DC: replacement = 'U'; break;
            case 0x00DD: replacement = 'Y'; break;
            case 0x00DE: replacement = 'T'; break;

            /* Latin-1 lowercase. */
            case 0x00DF: replacement = 's'; break;
            case 0x00E0: case 0x00E1: case 0x00E2: case 0x00E3:
            case 0x00E4: case 0x00E5: case 0x00E6: replacement = 'a'; break;
            case 0x00E7: replacement = 'c'; break;
            case 0x00E8: case 0x00E9: case 0x00EA: case 0x00EB: replacement = 'e'; break;
            case 0x00EC: case 0x00ED: case 0x00EE: case 0x00EF: replacement = 'i'; break;
            case 0x00F0: replacement = 'd'; break;
            case 0x00F1: replacement = 'n'; break;
            case 0x00F2: case 0x00F3: case 0x00F4: case 0x00F5:
            case 0x00F6: case 0x00F8: replacement = 'o'; break;
            case 0x00F9: case 0x00FA: case 0x00FB: case 0x00FC: replacement = 'u'; break;
            case 0x00FD: case 0x00FF: replacement = 'y'; break;
            case 0x00FE: replacement = 't'; break;

            /* Latin Extended-A: Central/Eastern European characters. */
            case 0x0100: case 0x0102: case 0x0104: replacement = 'A'; break;
            case 0x0101: case 0x0103: case 0x0105: replacement = 'a'; break;
            case 0x0106: case 0x0108: case 0x010A: case 0x010C: replacement = 'C'; break;
            case 0x0107: case 0x0109: case 0x010B: case 0x010D: replacement = 'c'; break;
            case 0x010E: case 0x0110: replacement = 'D'; break;
            case 0x010F: case 0x0111: replacement = 'd'; break;
            case 0x0112: case 0x0114: case 0x0116: case 0x0118: case 0x011A: replacement = 'E'; break;
            case 0x0113: case 0x0115: case 0x0117: case 0x0119: case 0x011B: replacement = 'e'; break;
            case 0x011C: case 0x011E: case 0x0120: case 0x0122: case 0x0124: replacement = 'G'; break;
            case 0x011D: case 0x011F: case 0x0121: case 0x0123: case 0x0125: replacement = 'g'; break;
            case 0x0126: replacement = 'H'; break; case 0x0127: replacement = 'h'; break;
            case 0x0128: case 0x012A: case 0x012C: case 0x012E: case 0x0130: replacement = 'I'; break;
            case 0x0129: case 0x012B: case 0x012D: case 0x012F: case 0x0131: replacement = 'i'; break;
            case 0x0132: replacement = 'I'; break; case 0x0133: replacement = 'i'; break;
            case 0x0134: replacement = 'J'; break; case 0x0135: replacement = 'j'; break;
            case 0x0136: replacement = 'K'; break; case 0x0137: case 0x0138: replacement = 'k'; break;
            case 0x0139: case 0x013B: case 0x013D: case 0x013F: case 0x0141: replacement = 'L'; break;
            case 0x013A: case 0x013C: case 0x013E: case 0x0140: case 0x0142: replacement = 'l'; break;
            case 0x0143: case 0x0145: case 0x0147: case 0x014A: replacement = 'N'; break;
            case 0x0144: case 0x0146: case 0x0148: case 0x014B: replacement = 'n'; break;
            case 0x014C: case 0x014E: case 0x0150: replacement = 'O'; break;
            case 0x014D: case 0x014F: case 0x0151: replacement = 'o'; break;
            case 0x0152: replacement = 'O'; break; case 0x0153: replacement = 'o'; break;
            case 0x0154: case 0x0156: case 0x0158: replacement = 'R'; break;
            case 0x0155: case 0x0157: case 0x0159: replacement = 'r'; break;
            case 0x015A: case 0x015C: case 0x015E: case 0x0160: replacement = 'S'; break;
            case 0x015B: case 0x015D: case 0x015F: case 0x0161: replacement = 's'; break;
            case 0x0162: case 0x0164: case 0x0166: replacement = 'T'; break;
            case 0x0163: case 0x0165: case 0x0167: replacement = 't'; break;
            case 0x0168: case 0x016A: case 0x016C: case 0x016E: case 0x0170: case 0x0172: replacement = 'U'; break;
            case 0x0169: case 0x016B: case 0x016D: case 0x016F: case 0x0171: case 0x0173: replacement = 'u'; break;
            case 0x0174: replacement = 'W'; break; case 0x0175: replacement = 'w'; break;
            case 0x0176: case 0x0178: replacement = 'Y'; break; case 0x0177: replacement = 'y'; break;
            case 0x0179: case 0x017B: case 0x017D: replacement = 'Z'; break;
            case 0x017A: case 0x017C: case 0x017E: replacement = 'z'; break;
            default: break;
        }

        if (cp == 0x2026) {
            if (out + 3 >= output_size) break;
            output[out++] = '.';
            output[out++] = '.';
            output[out++] = '.';
        } else if (replacement != 0) {
            output[out++] = replacement;
        } else if (cp < 0x80) {
            output[out++] = (char)cp;
        } else {
            /* Unknown Unicode: '?' is preferable to an LVGL missing-glyph box. */
            output[out++] = '?';
        }

        p += consumed;
    }

    output[out] = '\0';
}

static void source_strip_audio_extension(const char *input, char *output, size_t output_size)
{
    if (output == NULL || output_size == 0) return;
    output[0] = '\0';
    if (input == NULL) return;

    char cleaned[SOURCE_TEXT_MAX];
    source_clean_display_text(input, cleaned, sizeof(cleaned));
    snprintf(output, output_size, "%s", cleaned);

    char *dot = strrchr(output, '.');
    if (dot == NULL || dot == output) return;

    /* Only hide extensions that are normally used by music files.
     * This is display-only; the actual LMS item id/path is unchanged. */
    const char *ext = dot + 1;
    if (strcasecmp(ext, "mp3")  == 0 ||
        strcasecmp(ext, "flac") == 0 ||
        strcasecmp(ext, "wav")  == 0 ||
        strcasecmp(ext, "m4a")  == 0 ||
        strcasecmp(ext, "aac")  == 0 ||
        strcasecmp(ext, "ogg")  == 0 ||
        strcasecmp(ext, "oga")  == 0 ||
        strcasecmp(ext, "opus") == 0 ||
        strcasecmp(ext, "wma")  == 0 ||
        strcasecmp(ext, "aif")  == 0 ||
        strcasecmp(ext, "aiff") == 0 ||
        strcasecmp(ext, "ape")  == 0 ||
        strcasecmp(ext, "alac") == 0) {
        *dot = '\0';
    }
}

/* Resets "TAP AGAIN" on the CLEAR QUEUE button after the confirm window. */
static void source_clear_confirm_reset_cb(lv_timer_t *timer)
{
    (void)timer;
    if (source_play_context_button != NULL && source_current_mode == SOURCE_QUEUE) {
        lv_obj_t *label = lv_obj_get_child(source_play_context_button, 0);
        if (label != NULL) lv_label_set_text(label, "CLEAR QUEUE");
    }
}

