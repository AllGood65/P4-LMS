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


