/*
 * P4-LMS - Play/pause, previous, next and volume
 *
 * Part of main.c.  This file is #included by main.c (in order) and is not
 * compiled on its own, so it shares main.c's includes and static names.
 */

/* ============================================================
 * PLAY / PAUSE BUTTON
 * ============================================================ */

static void play_button_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) !=
        LV_EVENT_CLICKED) {
        return;
    }

    /*
     * Toggle based on the state Lyrion last reported, and flip the button
     * immediately so it feels instant; the next status poll confirms it.
     * (This callback runs inside LVGL, so the UI can be touched directly.)
     */
    bool want_play = !current_player_playing;

    ESP_LOGI(
        TAG,
        "%s requested for %s",
        want_play ? "Play" : "Pause",
        lyrion_players[
            current_player_index
        ].name
    );

    queue_lyrion_command(
        want_play
            ? LYRION_COMMAND_PLAY
            : LYRION_COMMAND_PAUSE,
        0
    );

    current_player_playing = want_play;
    /* Restart the local clock from "now" so resuming does not jump ahead. */
    playback_elapsed_s = playback_elapsed_s +
        (want_play ? 0.0 : (double)lv_tick_elaps(playback_sync_tick) / 1000.0);
    playback_sync_tick = lv_tick_get();
    ui_apply_play_state_locked(want_play);
    ui_apply_house_status_locked();
}


/* ============================================================
 * PREVIOUS BUTTON
 * ============================================================ */

static void previous_button_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) !=
        LV_EVENT_CLICKED) {

        return;
    }


    ESP_LOGI(
        TAG,
        "Previous requested for %s",
        lyrion_players[
            current_player_index
        ].name
    );


    queue_lyrion_command(
        LYRION_COMMAND_PREVIOUS,
        0
    );
}


/* ============================================================
 * NEXT BUTTON
 * ============================================================ */

static void next_button_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) !=
        LV_EVENT_CLICKED) {

        return;
    }


    ESP_LOGI(
        TAG,
        "Next requested for %s",
        lyrion_players[
            current_player_index
        ].name
    );


    queue_lyrion_command(
        LYRION_COMMAND_NEXT,
        0
    );
}


/* ============================================================
 * VOLUME SLIDER
 * ============================================================ */

static void shuffle_button_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;

    const int next = (shuffle_mode + 1) % 3;      /* off -> songs -> albums */
    shuffle_mode = next;
    mode_user_touched = true;
    mode_user_tick = lv_tick_get();
    ui_apply_mode_buttons_locked();
    queue_lyrion_command(LYRION_COMMAND_SHUFFLE, next);
}

static void repeat_button_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;

    /* off -> whole playlist -> this song -> off */
    const int next = (repeat_mode == 0) ? 2 : (repeat_mode == 2 ? 1 : 0);
    repeat_mode = next;
    mode_user_touched = true;
    mode_user_tick = lv_tick_get();
    ui_apply_mode_buttons_locked();
    queue_lyrion_command(LYRION_COMMAND_REPEAT, next);
}

/* Tap or drag on the progress bar to jump within the track. */
static void progress_seek_event_cb(lv_event_t *e)
{
    const lv_event_code_t code = lv_event_get_code(e);

    if (code == LV_EVENT_PRESS_LOST) {
        seek_dragging = false;
        return;
    }
    if (progress_bar == NULL || !playback_valid || playback_duration_s <= 0.0) {
        seek_dragging = false;              /* radio / nothing playing */
        return;
    }

    lv_indev_t *indev = lv_indev_active();
    if (indev == NULL) return;

    lv_point_t point;
    lv_indev_get_point(indev, &point);

    lv_area_t area;
    lv_obj_get_coords(progress_bar, &area);
    const int width = lv_area_get_width(&area);
    if (width <= 0) return;

    double fraction = (double)(point.x - area.x1) / (double)width;
    if (fraction < 0.0) fraction = 0.0;
    if (fraction > 1.0) fraction = 1.0;
    const double target = fraction * playback_duration_s;

    if (code == LV_EVENT_RELEASED) {
        seek_dragging = false;
        seek_release_pending = true;
        seek_release_tick = lv_tick_get();
        playback_elapsed_s = target;
        playback_sync_tick = lv_tick_get();
        playback_render_locked(target, playback_duration_s);
        queue_lyrion_command(LYRION_COMMAND_SEEK, (int)target);
    } else {                                 /* pressed / dragging */
        seek_dragging = true;
        playback_render_locked(target, playback_duration_s);
    }
}

static void volume_slider_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);

    if (code != LV_EVENT_VALUE_CHANGED &&
        code != LV_EVENT_RELEASED) {
        return;
    }

    /*
     * Ignore the event caused by the status task updating
     * the slider to match the real Lyrion volume.
     */
    if (updating_volume_from_server) {
        return;
    }

    lv_obj_t *slider =
        lv_event_get_target(e);

    int32_t value =
        lv_slider_get_value(slider);

    /* Remember that the user is in control, so status polls leave the
     * knob alone for a moment. */
    volume_user_touched = true;
    volume_last_user_tick = lv_tick_get();

    if (code == LV_EVENT_VALUE_CHANGED) {

        /* Show the new value immediately instead of waiting for the server. */
        char volume_text[16];
        snprintf(volume_text, sizeof(volume_text), "%ld%%", (long)value);
        ui_set_label(volume_value_label, volume_text);

        /*
         * While dragging, send at most one update per 150 ms so the command
         * queue is not flooded; the final value is always sent on release.
         */
        if (lv_tick_elaps(volume_last_sent_tick) < 150) {
            return;
        }
    }

    volume_last_sent_tick = lv_tick_get();

    ESP_LOGI(
        TAG,
        "Volume requested: %ld%% for %s",
        (long)value,
        lyrion_players[
            current_player_index
        ].name
    );

    /*
     * The value is queued rather than sent directly from
     * the LVGL task.
     */
    queue_lyrion_command(
        LYRION_COMMAND_VOLUME,
        (int)value
    );
}




/* ============================================================
 * SYSTEM POWER BUTTON (Now Playing)
 *
 * Switches the current room's amp + pre-amp on or off, in the room's order
 * (the same as "System power" on the Rooms screen).  Lit when everything it
 * switches is on; hidden for rooms without power switches.  The command
 * task reads the switches every few seconds (np_power_poll).
 * ============================================================ */
static lv_obj_t *np_power_button = NULL;
static int       np_power_state = -1;          /* -1 not known yet, 0 off, 1 on */
static bool      np_power_want = false;        /* what the last tap asked for   */
static uint32_t  np_power_touch = 0;
static uint32_t  np_power_hold = 0;            /* ignore polls this long after a tap */
static volatile bool np_power_poll_now = true;


static void np_power_apply_locked(void)
{
    if (np_power_button == NULL) return;
    const ha_player_switches_t *sw = ha_switches_for_current();
    if ((sw->amp_entity == NULL && sw->preamp_entity == NULL) || !HA_TOKEN_SET) {
        lv_obj_add_flag(np_power_button, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_clear_flag(np_power_button, LV_OBJ_FLAG_HIDDEN);

    int st = np_power_state;
    if (np_power_hold > 0 && lv_tick_elaps(np_power_touch) < np_power_hold) st = np_power_want ? 1 : 0;
    const bool on = (st == 1);
    lv_obj_set_style_bg_color(np_power_button, lv_color_hex(on ? UI_ACCENT : UI_BUTTON), LV_PART_MAIN);
    lv_obj_set_style_border_color(np_power_button, lv_color_hex(on ? UI_ACCENT_GLOW : UI_EDGE), LV_PART_MAIN);
    lv_obj_t *label = lv_obj_get_child(np_power_button, 0);
    if (label != NULL) {
        lv_obj_set_style_text_color(label, lv_color_hex(on ? UI_ON_ACCENT : UI_TEXT_DIM), 0);
    }
}

/* A different room was chosen: its state isn't known yet. */
static void np_power_room_changed(void)
{
    np_power_state = -1;
    np_power_hold = 0;
    np_power_poll_now = true;
    np_power_apply_locked();
}

static void np_power_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    const int room = current_player_index;
    const ha_player_switches_t *hs = ha_switches_for_current();
    const bool shown_on = (np_power_hold > 0 && lv_tick_elaps(np_power_touch) < np_power_hold)
                          ? np_power_want : (np_power_state == 1);
    const bool on = !shown_on;
    if ((hs->amp_entity == NULL && hs->preamp_entity == NULL) || !HA_TOKEN_SET || !queue_ha_system(room, on)) {
        ui_toast(HA_TOKEN_SET ? "Couldn't reach Home Assistant" : "Set the Home Assistant token on the web page");
        return;
    }
    /* Show the new state straight away; polls can't flip it back until the
     * second switch has had time to follow. */
    const bool both = (hs->amp_entity != NULL && hs->preamp_entity != NULL);
    np_power_want = on;
    np_power_touch = lv_tick_get();
    np_power_hold = (both ? (uint32_t)hs->delay_s * 1000u : 0u) + 5000u;
    np_power_apply_locked();

    const bool pre_leads = (on != hs->amp_first);
    char msg[96];
    if (!both) {
        snprintf(msg, sizeof(msg), "%s: %s %s", lyrion_players[room].short_name,
                 hs->amp_entity != NULL ? "amp" : "pre-amp", on ? "on" : "off");
    } else if (hs->delay_s == 0) {
        snprintf(msg, sizeof(msg), "%s: %s, then %s %s", lyrion_players[room].short_name,
                 pre_leads ? "pre-amp" : "amp", pre_leads ? "amp" : "pre-amp", on ? "on" : "off");
    } else {
        snprintf(msg, sizeof(msg), "%s: %s %s, %s in %u s", lyrion_players[room].short_name,
                 pre_leads ? "pre-amp" : "amp", on ? "on" : "off", pre_leads ? "amp" : "pre-amp",
                 (unsigned)hs->delay_s);
    }
    ui_toast(msg);
}

typedef struct { int room; int state; } np_power_msg_t;

static void np_power_job(void *arg)
{
    const np_power_msg_t *m = (const np_power_msg_t *)arg;
    if (m->room != current_player_index) return;      /* the room changed meanwhile */
    if (m->state >= 0) np_power_state = m->state;
    np_power_apply_locked();
}

/* Command task: read the current room's switches every few seconds (not
 * while the screensaver is up; much less often if Home Assistant can't be
 * reached, so Lyrion commands are never held up for long). */
static void np_power_poll(void)
{
    static TickType_t next = 0;
    const TickType_t now = xTaskGetTickCount();
    if (np_power_button == NULL || screensaver_active || !wifi_have_ip) return;
    if (!np_power_poll_now && (int32_t)(now - next) < 0) return;
    np_power_poll_now = false;

    const int room = current_player_index;
    const ha_player_switches_t *sw = ha_switches_for_index(room);
    if ((sw->amp_entity == NULL && sw->preamp_entity == NULL) || !HA_TOKEN_SET) {
        next = now + pdMS_TO_TICKS(8000);
        np_power_msg_t m = { .room = room, .state = -1 };
        ui_post(np_power_job, &m, sizeof(m));          /* hides the button */
        return;
    }
    bool ok = true, all_on = true, st = false;
    if (sw->amp_entity != NULL) {
        if (ha_switch_get_state(sw->amp_entity, &st)) all_on = all_on && st; else ok = false;
    }
    if (ok && sw->preamp_entity != NULL) {
        if (ha_switch_get_state(sw->preamp_entity, &st)) all_on = all_on && st; else ok = false;
    }
    next = now + pdMS_TO_TICKS(ok ? 8000 : 60000);
    np_power_msg_t m = { .room = room, .state = ok ? (all_on ? 1 : 0) : -1 };
    ui_post(np_power_job, &m, sizeof(m));
}
