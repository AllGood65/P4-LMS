/*
 * P4-LMS - Sending Lyrion commands, Home Assistant client, status task
 *
 * Part of main.c.  This file is #included by main.c (in order) and is not
 * compiled on its own, so it shares main.c's includes and static names.
 */

/* ============================================================
 * SEND LYRION COMMAND
 * ============================================================ */

static bool lyrion_send_command(
    const lyrion_command_t *command)
{
    if (command == NULL) {
        return false;
    }


    int player_index =
        command->player_index;


    if (player_index < 0 ||
        player_index >=
            (int)LYRION_PLAYER_COUNT) {

        return false;
    }


    const char *player_id =
        lyrion_players[
            player_index
        ].id;


    const char *player_name =
        lyrion_players[
            player_index
        ].name;


    char command_json[128];


    memset(
        command_json,
        0,
        sizeof(command_json)
    );


    /* --------------------------------------------------------
     * BUILD COMMAND
     * -------------------------------------------------------- */

    switch (command->type) {

        case LYRION_COMMAND_PLAY:

            snprintf(
                command_json,
                sizeof(command_json),

                "{"
                "\"id\":1,"
                "\"method\":\"slim.request\","
                "\"params\":["
                "\"%s\","
                "[\"play\"]"
                "]"
                "}",

                player_id
            );

            break;


        case LYRION_COMMAND_PAUSE:

            snprintf(
                command_json,
                sizeof(command_json),

                "{"
                "\"id\":1,"
                "\"method\":\"slim.request\","
                "\"params\":["
                "\"%s\","
                "[\"pause\",\"1\"]"
                "]"
                "}",

                player_id
            );

            break;


        case LYRION_COMMAND_PREVIOUS:

            snprintf(
                command_json,
                sizeof(command_json),

                "{"
                "\"id\":1,"
                "\"method\":\"slim.request\","
                "\"params\":["
                "\"%s\","
                "[\"playlist\",\"index\",\"-1\"]"
                "]"
                "}",

                player_id
            );

            break;


        case LYRION_COMMAND_NEXT:

            snprintf(
                command_json,
                sizeof(command_json),

                "{"
                "\"id\":1,"
                "\"method\":\"slim.request\","
                "\"params\":["
                "\"%s\","
                "[\"playlist\",\"index\",\"+1\"]"
                "]"
                "}",

                player_id
            );

            break;


        case LYRION_COMMAND_SHUFFLE:
            if (command->volume < 0 || command->volume > 2) {
                return false;
            }
            snprintf(command_json, sizeof(command_json),
                     "{\"id\":1,\"method\":\"slim.request\",\"params\":"
                     "[\"%s\",[\"playlist\",\"shuffle\",\"%d\"]]}",
                     player_id, command->volume);
            break;
        case LYRION_COMMAND_REPEAT:
            if (command->volume < 0 || command->volume > 2) {
                return false;
            }
            snprintf(command_json, sizeof(command_json),
                     "{\"id\":1,\"method\":\"slim.request\",\"params\":"
                     "[\"%s\",[\"playlist\",\"repeat\",\"%d\"]]}",
                     player_id, command->volume);
            break;
        case LYRION_COMMAND_SLEEP:
            if (command->volume < 0) {
                return false;
            }
            snprintf(command_json, sizeof(command_json),
                     "{\"id\":1,\"method\":\"slim.request\",\"params\":"
                     "[\"%s\",[\"sleep\",\"%d\"]]}",
                     player_id, command->volume);
            break;
        case LYRION_COMMAND_SYNC:
            /* "<A> sync <B>": B stops and joins A's group, A keeps playing. */
            if (command->volume < 0 || command->volume >= lyrion_player_count ||
                command->volume == command->player_index) {
                return false;
            }
            snprintf(command_json, sizeof(command_json),
                     "{\"id\":1,\"method\":\"slim.request\",\"params\":"
                     "[\"%s\",[\"sync\",\"%s\"]]}",
                     player_id, lyrion_players[command->volume].id);
            break;
        case LYRION_COMMAND_UNSYNC:
            snprintf(command_json, sizeof(command_json),
                     "{\"id\":1,\"method\":\"slim.request\",\"params\":"
                     "[\"%s\",[\"sync\",\"-\"]]}",
                     player_id);
            break;
        case LYRION_COMMAND_SEEK:
            if (command->volume < 0) {
                return false;
            }
            snprintf(command_json, sizeof(command_json),
                     "{\"id\":1,\"method\":\"slim.request\",\"params\":"
                     "[\"%s\",[\"time\",\"%d\"]]}",
                     player_id, command->volume);
            break;
        case LYRION_COMMAND_VOLUME:
            if (command->volume < 0 ||
                command->volume > 100) {

                return false;
            }


            snprintf(
                command_json,
                sizeof(command_json),

                "{"
                "\"id\":1,"
                "\"method\":\"slim.request\","
                "\"params\":["
                "\"%s\","
                "[\"mixer\",\"volume\",\"%d\"]"
                "]"
                "}",

                player_id,
                command->volume
            );

            break;


        default:

            return false;
    }


    ESP_LOGI(
        TAG,
        "Sending command to %s: %s",
        player_name,
        command_json
    );


    /* --------------------------------------------------------
     * HTTP CLIENT
     *
     * No response buffer is needed for commands.
     * -------------------------------------------------------- */

    esp_http_client_config_t config = {
        .url =
            LYRION_SERVER_URL,

        .method =
            HTTP_METHOD_POST,

        .timeout_ms =
            5000
    };


    esp_http_client_handle_t client =
        esp_http_client_init(
            &config
        );


    if (client == NULL) {

        ESP_LOGE(
            TAG,
            "Failed to create Lyrion command client"
        );

        return false;
    }


    esp_err_t err =
        esp_http_client_set_header(
            client,
            "Content-Type",
            "application/json"
        );


    if (err != ESP_OK) {

        ESP_LOGE(
            TAG,
            "Failed to set command header: %s",
            esp_err_to_name(err)
        );


        esp_http_client_cleanup(client);


        return false;
    }


    err =
        esp_http_client_set_post_field(
            client,
            command_json,
            strlen(command_json)
        );


    if (err != ESP_OK) {

        ESP_LOGE(
            TAG,
            "Failed to set command POST field: %s",
            esp_err_to_name(err)
        );


        esp_http_client_cleanup(client);


        return false;
    }


    err =
        esp_http_client_perform(
            client
        );


    if (err != ESP_OK) {

        ESP_LOGE(
            TAG,
            "Lyrion command failed: %s",
            esp_err_to_name(err)
        );


        esp_http_client_cleanup(client);


        return false;
    }


    int status_code =
        esp_http_client_get_status_code(
            client
        );


    ESP_LOGI(
        TAG,
        "Lyrion command HTTP status: %d",
        status_code
    );


    esp_http_client_cleanup(client);


    return (
        status_code >= 200 &&
        status_code < 300
    );
}


/* ============================================================
 * LYRION COMMAND TASK
 * ============================================================ */



/* ============================================================
 * QUEUE A LYRION COMMAND
 * ============================================================ */

/* ------------------------------------------------------------------------
 * HOME ASSISTANT - HTTP helpers, queue and background task
 * ------------------------------------------------------------------------ */
typedef struct {
    char entity[64];
    bool turn_on;
    bool system;          /* true: the room's amp + pre-amp in order (entity unused) */
    int8_t room;          /* player index, for system commands */
} ha_command_t;

static QueueHandle_t ha_command_queue = NULL;
/* Home Assistant used to get its own background task.  That extra ~6 KB task
 * stack was enough to push the board's small internal (non-PSRAM) memory over
 * the edge: the source browser's task (which needs a large 32 KB stack) then
 * failed to start at all, and general Wi-Fi/HTTP traffic started failing
 * intermittently too (visible as "esp-tls select() timeout" and SDIO
 * "mempool OOM" in the log).  Home Assistant commands are now handled by the
 * existing Lyrion command task instead, using a FreeRTOS queue set so that
 * task can wait on either queue at once - no second task, no extra stack. */
static QueueSetHandle_t command_queue_set = NULL;
static volatile bool ha_overlay_open = false;

static lv_obj_t *ha_amp_button = NULL;
static lv_obj_t *ha_amp_label = NULL;
static lv_obj_t *ha_preamp_button = NULL;
static lv_obj_t *ha_preamp_label = NULL;
static lv_obj_t *ha_status_label = NULL;
static int ha_amp_state_applied = -1;       /* -1 unknown, 0 off, 1 on, 2 pending (see below) */
static int ha_preamp_state_applied = -1;

/* A tap flips the button instantly, but Wemo switches in particular can take
 * a couple of seconds to actually report their new state back to Home
 * Assistant.  Without this, the very next poll (a fraction of a second
 * later) would read the OLD state and immediately flip the button back,
 * which looked like the tap had not registered.  For HA_COMMAND_GRACE_MS
 * after a tap, the button keeps showing what was requested (with "..." on
 * it) instead of being overwritten by a poll that has not caught up yet. */
#define HA_COMMAND_GRACE_MS 4000
static bool ha_amp_pending = false;
static bool ha_amp_pending_on = false;
static uint32_t ha_amp_pending_tick = 0;
static bool ha_preamp_pending = false;
static bool ha_preamp_pending_on = false;
static uint32_t ha_preamp_pending_tick = 0;

/* GET or POST to Home Assistant's REST API.  post_body == NULL means GET. */
static bool ha_request(const char *path, const char *post_body,
                       char *response, size_t response_size, int *out_status)
{
    char url[192];
    snprintf(url, sizeof(url), "%s%s", HA_SERVER_BASE, path);

    esp_http_client_config_t config = {
        .url = url,
        .method = (post_body != NULL) ? HTTP_METHOD_POST : HTTP_METHOD_GET,
        .timeout_ms = 4000,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        ESP_LOGE(TAG, "Failed to create Home Assistant client");
        return false;
    }

    char auth[sizeof(ha_token) + 8];
    snprintf(auth, sizeof(auth), "Bearer %s", ha_token);
    esp_http_client_set_header(client, "Authorization", auth);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    if (post_body != NULL) {
        esp_http_client_set_post_field(client, post_body, strlen(post_body));
    }

    const int post_len = (post_body != NULL) ? (int)strlen(post_body) : 0;
    bool ok = (esp_http_client_open(client, post_len) == ESP_OK);
    if (!ok) {
        /* Could not even connect: wrong IP/port, HA not running, or blocked by
         * a firewall.  esp_http_client itself already logs the low-level
         * reason (connection refused, timeout, DNS, ...) just above this. */
        ESP_LOGW(TAG, "Home Assistant: could not open a connection for %s", path);
        esp_http_client_cleanup(client);
        return false;
    }

    /* open() only announces how many bytes are coming - the body still has to
     * be written explicitly (unlike esp_http_client_perform(), which does
     * this step for you).  Skipping it silently sends an empty POST body,
     * which is why turn_on/turn_off looked like they worked but did nothing:
     * Home Assistant received a request with no entity_id in it. */
    if (post_body != NULL) {
        if (esp_http_client_write(client, post_body, post_len) != post_len) {
            ESP_LOGW(TAG, "Home Assistant: failed to send the request body for %s", path);
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            return false;
        }
    }

    const int content_length = esp_http_client_fetch_headers(client);
    (void)content_length;

    /* Always keep a copy of the body (even when the caller does not want it)
     * so a failure can be logged with Home Assistant's own error message. */
    char diag[160];
    size_t total = 0;
    char *dest = (response != NULL && response_size > 0) ? response : diag;
    size_t dest_size = (response != NULL && response_size > 0) ? response_size : sizeof(diag);

    while (total < dest_size - 1) {
        const int n = esp_http_client_read(client, dest + total, (int)(dest_size - 1 - total));
        if (n < 0) { ok = false; break; }
        if (n == 0) break;
        total += (size_t)n;
    }
    dest[total] = '\0';
    /* Keep draining past what fit in the buffer, so the connection is left
     * clean for reuse. */
    if (ok) {
        char scratch[64];
        int n;
        do { n = esp_http_client_read(client, scratch, sizeof(scratch)); } while (n > 0);
    }

    const int status = esp_http_client_get_status_code(client);
    if (out_status != NULL) {
        *out_status = status;
    }
    if (ok && (status < 200 || status >= 300)) {
        const char *why = "unexpected status";
        if (status == 401 || status == 403) why = "unauthorized - check the token on the web settings page";
        else if (status == 404) why = "not found - check the switch names and address on the web settings page";
        ESP_LOGW(TAG, "Home Assistant %s (HTTP %d) for %s: %s", why, status, path, dest);
        ok = false;
    }

    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return ok;
}

static bool ha_switch_set(const char *entity, bool turn_on)
{
    if (entity == NULL || entity[0] == '\0') {
        return false;
    }
    char body[96];
    snprintf(body, sizeof(body), "{\"entity_id\":\"%s\"}", entity);

    char path[64];
    snprintf(path, sizeof(path), "/api/services/switch/turn_%s", turn_on ? "on" : "off");

    return ha_request(path, body, NULL, 0, NULL);
}

/* Returns true if the state was read; *on is only valid then. */
static bool ha_switch_get_state(const char *entity, bool *on)
{
    if (entity == NULL || entity[0] == '\0') {
        return false;
    }
    char path[80];
    snprintf(path, sizeof(path), "/api/states/%s", entity);

    static char response[HA_RESPONSE_MAX];
    int status = 0;
    if (!ha_request(path, NULL, response, sizeof(response), &status)) {
        return false;
    }

    cJSON *root = cJSON_Parse(response);
    if (root == NULL) {
        return false;
    }
    cJSON *state = cJSON_GetObjectItem(root, "state");
    bool ok = cJSON_IsString(state) && state->valuestring != NULL;
    if (ok) {
        *on = (strcmp(state->valuestring, "on") == 0);
    }
    cJSON_Delete(root);
    return ok;
}

static bool queue_ha_command(const char *entity, bool turn_on)
{
    if (ha_command_queue == NULL || entity == NULL) {
        return false;
    }
    ha_command_t command;
    memset(&command, 0, sizeof(command));
    snprintf(command.entity, sizeof(command.entity), "%s", entity);
    command.turn_on = turn_on;
    return xQueueSend(ha_command_queue, &command, 0) == pdTRUE;
}

/* Amp + pre-amp together, in order (from the UI; the command task does it). */
static bool queue_ha_system(int room, bool turn_on)
{
    if (ha_command_queue == NULL || room < 0 || room >= LMS_MAX_PLAYERS) return false;
    ha_command_t command;
    memset(&command, 0, sizeof(command));
    command.system = true;
    command.room = (int8_t)room;
    command.turn_on = turn_on;
    return xQueueSend(ha_command_queue, &command, 0) == pdTRUE;
}

/* ------------------------------------------------------------------------
 * POWER SEQUENCE - a room's amp and pre-amp switched in a safe order
 *
 * Usual order (to avoid a thump in the speakers): pre-amp on, wait, amp on;
 * amp off, wait, pre-amp off.  "Amp first" reverses it.  The first switch is
 * set straight away and the second is remembered with a due time, so the
 * command task never sits in a long delay (Lyrion commands keep flowing).
 * Everything here runs on the command task only.
 * ------------------------------------------------------------------------ */
#define HA_SEQ_SLOTS 4
typedef struct { bool active; bool on; TickType_t due; char entity[64]; } ha_seq_step_t;
static ha_seq_step_t ha_seq_steps[HA_SEQ_SLOTS];

/* Set one switch, with one retry. */
static void ha_switch_set_retry(const char *entity, bool on)
{
    if (entity == NULL || !entity[0]) return;
    char name[64];                                   /* local copy: the caller's may be a slot */
    snprintf(name, sizeof(name), "%s", entity);
    if (!ha_switch_set(name, on)) {
        ESP_LOGW(TAG, "Home Assistant: failed to set %s, retrying once", name);
        vTaskDelay(pdMS_TO_TICKS(300));
        if (!ha_switch_set(name, on)) {
            ESP_LOGW(TAG, "Home Assistant: retry also failed for %s", name);
        }
    }
}

/* Forget any waiting step for this switch (a newer request wins). */
static void ha_seq_cancel(const char *entity)
{
    if (entity == NULL) return;
    for (int k = 0; k < HA_SEQ_SLOTS; k++) {
        if (ha_seq_steps[k].active && strcmp(ha_seq_steps[k].entity, entity) == 0) {
            ha_seq_steps[k].active = false;
        }
    }
}

static void ha_power_sequence(const ha_player_switches_t *sw, bool on)
{
    if (sw == NULL) return;
    const char *amp = sw->amp_entity, *pre = sw->preamp_entity;
    ha_seq_cancel(amp);
    ha_seq_cancel(pre);
    if (amp == NULL || pre == NULL) {                   /* only one switch: nothing to order */
        if (amp != NULL)      ha_switch_set_retry(amp, on);
        else if (pre != NULL) ha_switch_set_retry(pre, on);
        return;
    }
    /* Which goes first: the pre-amp when switching on and the amp when
     * switching off, unless this room is set to "amp first". */
    const bool pre_leads = (on != sw->amp_first);
    const char *first = pre_leads ? pre : amp;
    const char *second = pre_leads ? amp : pre;
    ESP_LOGI(TAG, "Power sequence %s: %s, then %s after %u s", on ? "on" : "off",
             first, second, (unsigned)sw->delay_s);
    ha_switch_set_retry(first, on);
    if (sw->delay_s == 0) {
        ha_switch_set_retry(second, on);
        return;
    }
    int slot = 0;
    for (int k = 0; k < HA_SEQ_SLOTS; k++) {
        if (!ha_seq_steps[k].active) { slot = k; break; }
        if ((int32_t)(ha_seq_steps[k].due - ha_seq_steps[slot].due) < 0) slot = k;   /* all busy: soonest */
    }
    if (ha_seq_steps[slot].active) ha_switch_set_retry(ha_seq_steps[slot].entity, ha_seq_steps[slot].on);
    ha_seq_steps[slot].active = true;
    ha_seq_steps[slot].on = on;
    ha_seq_steps[slot].due = xTaskGetTickCount() + pdMS_TO_TICKS(sw->delay_s * 1000u);
    snprintf(ha_seq_steps[slot].entity, sizeof(ha_seq_steps[slot].entity), "%s", second);
}

/* Do any second steps that are due. */
static void ha_seq_tick(void)
{
    const TickType_t now = xTaskGetTickCount();
    for (int k = 0; k < HA_SEQ_SLOTS; k++) {
        if (ha_seq_steps[k].active && (int32_t)(now - ha_seq_steps[k].due) >= 0) {
            ha_seq_steps[k].active = false;
            ha_switch_set_retry(ha_seq_steps[k].entity, ha_seq_steps[k].on);
        }
    }
}

/* How long the command task may wait before the next step is due. */
static TickType_t ha_seq_wait(TickType_t longest)
{
    const TickType_t now = xTaskGetTickCount();
    TickType_t wait = longest;
    for (int k = 0; k < HA_SEQ_SLOTS; k++) {
        if (!ha_seq_steps[k].active) continue;
        const int32_t left = (int32_t)(ha_seq_steps[k].due - now);
        const TickType_t t = left <= 0 ? 1 : (TickType_t)left;
        if (t < wait) wait = t;
    }
    return wait;
}

/* Updates one toggle button.  "pending" means a tap was just sent and the
 * grace window has not run out yet, so what is SHOWN is what was REQUESTED
 * (marked with "...") rather than whatever the last poll happened to read.
 * state_cache is one of ha_amp_state_applied / ha_preamp_state_applied, used
 * purely to skip redundant redraws (-1 unknown, 0 off, 1 on, 2 pending). */
static void ha_apply_row_locked(lv_obj_t *button, lv_obj_t *label, int *state_cache,
                                const char *name, bool pending, bool pending_on,
                                bool known, bool on)
{
    if (button == NULL) {
        return;
    }
    const bool shown_on = pending ? pending_on : on;
    const bool shown_known = pending || known;
    const int state = pending ? 2 : (shown_known ? (shown_on ? 1 : 0) : -1);

    if (*state_cache == state) {
        return;
    }
    *state_cache = state;

    char text[24];
    if (!shown_known) {
        snprintf(text, sizeof(text), "%s: ?", name);
    } else {
        snprintf(text, sizeof(text), "%s: %s%s", name, shown_on ? "ON" : "OFF", pending ? " ..." : "");
    }
    lv_label_set_text(label, text);
    lv_obj_set_style_bg_color(button,
        lv_color_hex((shown_known && shown_on) ? UI_ACCENT : UI_BUTTON), LV_PART_MAIN);
    lv_obj_set_style_border_color(button,
        lv_color_hex(pending ? UI_ACCENT_BRIGHT : ((shown_known && shown_on) ? UI_ACCENT_GLOW : UI_EDGE)),
        LV_PART_MAIN);
}

/* Updates both toggle buttons (LVGL lock held by the caller). */
static void ha_apply_state_locked(bool amp_known, bool amp_on, bool preamp_known, bool preamp_on)
{
    const bool amp_grace_left = ha_amp_pending && lv_tick_elaps(ha_amp_pending_tick) < HA_COMMAND_GRACE_MS;
    const bool preamp_grace_left = ha_preamp_pending && lv_tick_elaps(ha_preamp_pending_tick) < HA_COMMAND_GRACE_MS;

    /* Once the real state agrees with what was requested, or the grace
     * window runs out, stop overriding the poll - either it is confirmed, or
     * (rarely, e.g. an unreliable Wemo) it genuinely failed and the button
     * should show what is actually true rather than get stuck. */
    if (ha_amp_pending && (!amp_grace_left || (amp_known && amp_on == ha_amp_pending_on))) {
        ha_amp_pending = false;
    }
    if (ha_preamp_pending && (!preamp_grace_left || (preamp_known && preamp_on == ha_preamp_pending_on))) {
        ha_preamp_pending = false;
    }

    ha_apply_row_locked(ha_amp_button, ha_amp_label, &ha_amp_state_applied, "AMP",
                        ha_amp_pending, ha_amp_pending_on, amp_known, amp_on);
    ha_apply_row_locked(ha_preamp_button, ha_preamp_label, &ha_preamp_state_applied, "PRE-AMP",
                        ha_preamp_pending, ha_preamp_pending_on, preamp_known, preamp_on);
}


static bool queue_lyrion_command(
    lyrion_command_type_t type,
    int volume)
{
    if (lyrion_command_queue == NULL) {
        return false;
    }


    lyrion_command_t command = {
        .player_index =
            current_player_index,

        .type =
            type,

        .volume =
            volume
    };


    if (xQueueSend(
            lyrion_command_queue,
            &command,
            0
        ) != pdTRUE) {

        ESP_LOGW(
            TAG,
            "Lyrion command queue full"
        );

        return false;
    }


    return true;
}


/* ============================================================
 * LYRION STATUS TASK
 * ============================================================ */

/* One "serverstatus" request lists every player and whether it is on / playing,
 * so the buttons can show what the whole house is doing. */
typedef struct { int state[LMS_MAX_PLAYERS]; } house_msg_t;

static void house_status_job(void *arg)
{
    const house_msg_t *m = (const house_msg_t *)arg;
    for (int i = 0; i < (int)LYRION_PLAYER_COUNT; i++) {
        house_state[i] = m->state[i];
    }
    ui_apply_house_status_locked();
}

static bool lyrion_get_house_status(void)
{
    static const char request[] =
        "{\"id\":1,\"method\":\"slim.request\","
        "\"params\":[\"\",[\"serverstatus\",0,20]]}";

    if (!source_http_query(0, request, lyrion_response_buffer, LYRION_RESPONSE_MAX)) {
        return false;
    }

    cJSON *root = cJSON_Parse(lyrion_response_buffer);
    if (root == NULL) {
        return false;
    }

    int state[LMS_MAX_PLAYERS];
    for (int i = 0; i < (int)LYRION_PLAYER_COUNT; i++) {
        state[i] = 0;
    }

    cJSON *result = cJSON_GetObjectItem(root, "result");
    cJSON *loop = cJSON_IsObject(result) ? cJSON_GetObjectItem(result, "players_loop") : NULL;
    cJSON *entry = NULL;
    cJSON_ArrayForEach(entry, loop) {
        cJSON *id_j = cJSON_GetObjectItem(entry, "playerid");
        if (!cJSON_IsString(id_j) || id_j->valuestring == NULL) continue;

        for (int i = 0; i < (int)LYRION_PLAYER_COUNT; i++) {
            if (strcasecmp(id_j->valuestring, lyrion_players[i].id) != 0) continue;

            cJSON *playing_j = cJSON_GetObjectItem(entry, "isplaying");
            cJSON *power_j = cJSON_GetObjectItem(entry, "power");
            cJSON *conn_j = cJSON_GetObjectItem(entry, "connected");
            const bool playing = cJSON_IsNumber(playing_j) ? playing_j->valueint != 0
                               : (cJSON_IsString(playing_j) && playing_j->valuestring && atoi(playing_j->valuestring) != 0);
            const bool powered = cJSON_IsNumber(power_j) ? power_j->valueint != 0 : true;
            const bool connected = cJSON_IsNumber(conn_j) ? conn_j->valueint != 0 : true;

            state[i] = (!connected || !powered) ? 0 : (playing ? 2 : 1);
            break;
        }
    }
    cJSON_Delete(root);

    house_msg_t m;
    memcpy(m.state, state, sizeof(m.state));
    ui_post(house_status_job, &m, sizeof(m));
    return true;
}

static bool house_refresh_due = false;   /* another player changed */

static void lyrion_status_task(void *arg)
{
    (void)arg;


    ESP_LOGI(
        TAG,
        "Lyrion status task started"
    );


    while (1) {

        /*
         * Wait for Wi-Fi.
         */
        EventBits_t wifi_bits =
            xEventGroupWaitBits(
                wifi_event_group,
                WIFI_CONNECTED_BIT,
                pdFALSE,
                pdFALSE,
                pdMS_TO_TICKS(1000)
            );


        if ((wifi_bits &
             WIFI_CONNECTED_BIT) == 0) {

            vTaskDelay(
                pdMS_TO_TICKS(500)
            );

            continue;
        }


        /*
         * Query selected player.
         */
        bool ok =
            lyrion_get_status();


        if (ok) {
            lyrion_fail_count = 0;

            /* The other players' dots: when push updates say one changed,
             * otherwise every other poll. */
            static unsigned house_poll_counter = 0;
            if (house_refresh_due || (house_poll_counter++ % 2U) == 0U) {
                house_refresh_due = false;
                lyrion_get_house_status();
            }
        }
        else {

            ESP_LOGW(
                TAG,
                "Lyrion status unavailable (%d)",
                lyrion_fail_count + 1
            );


            /* One hiccup (a slow reply, a Wi-Fi blip) must not blank the
             * screen; show the offline state on the second failure. */
            if (++lyrion_fail_count >= 2) {

                update_lyrion_offline_ui();
            }
        }


        /*
         * Wait either for:
         *
         * 1. player selection / command requiring
         *    an immediate refresh
         *
         * OR
         *
         * 2. normal two-second polling interval (elapsed time is
         *    advanced locally between polls).
         */
        if (lyrion_refresh_event != NULL) {

            /* With push updates (see 09b_lyrion_events.c) the server says
             * when something changed, so the timed poll is only a safety
             * net; without them, poll as often as before. */
            const uint32_t poll_ms = lms_events_connected ? LYRION_POLL_PUSH_MS : LYRION_POLL_MS;
            const EventBits_t why = xEventGroupWaitBits(
                lyrion_refresh_event,
                LYRION_REFRESH_BIT | LYRION_HOUSE_BIT,
                pdTRUE,
                pdFALSE,
                pdMS_TO_TICKS(poll_ms)
            );
            if (why & (LYRION_REFRESH_BIT | LYRION_HOUSE_BIT)) {
                /* A change usually arrives as a burst of several lines (new
                 * song, playlist, time...): let it finish, then refresh once. */
                vTaskDelay(pdMS_TO_TICKS(120));
                const EventBits_t more = xEventGroupClearBits(lyrion_refresh_event,
                                                              LYRION_REFRESH_BIT | LYRION_HOUSE_BIT);
                if ((why | more) & LYRION_HOUSE_BIT) house_refresh_due = true;
            }
        }
        else {

            vTaskDelay(
                pdMS_TO_TICKS(LYRION_POLL_MS)
            );
        }
    }
}




/* ========================================================================
 * ROOMS - what every room is doing, for the Rooms cards (18b_navigation.c)
 * ========================================================================
 * While the Rooms tab is open, the command task (which already wakes every
 * HA_POLL_MS) asks Lyrion for each room's status and Home Assistant for its
 * switches, and fetches a small cover whenever a room's album changes.  The
 * results reach the screen as UI jobs. */
static volatile bool rooms_open = false;

#define ROOM_COVER_PX 96
#define ROOM_SWITCH_NONE   (-2)    /* this room has no such switch  */
#define ROOM_SWITCH_UNKNOWN (-1)   /* couldn't read it this time     */

typedef struct {
    int  idx;
    int  state;            /* 0 off / unreachable, 1 paused or stopped, 2 playing */
    char title[96];
    char artist[96];
    int  volume;           /* -1 unknown */
    int  amp;              /* ROOM_SWITCH_NONE / _UNKNOWN / 0 / 1 */
    int  pre;
    int  group_mask;       /* rooms playing in sync with this one (bit per room), 0 = none */
} room_msg_t;

typedef struct { int idx; uint8_t *pixels; uint32_t w, h, padded; size_t size; } room_cover_msg_t;

static void rooms_card_job(void *arg);     /* in 18b_navigation.c */
static void rooms_cover_job(void *arg);

static char rooms_cover_key[LMS_MAX_PLAYERS][64];

/* Switch names for room i (by player MAC), or the empty set. */
static const ha_player_switches_t *ha_switches_for_index(int i)
{
    if (i < 0 || i >= lyrion_player_count) return &ha_no_switches;
    for (int k = 0; k < ha_map_count; k++) {
        if (strcasecmp(ha_map_view[k].player_id, lyrion_players[i].id) == 0) return &ha_map_view[k];
    }
    return &ha_no_switches;
}

/* Queue a command for any room, not just the selected one. */
static bool queue_lyrion_command_for(int player_index, lyrion_command_type_t type, int value)
{
    if (lyrion_command_queue == NULL || player_index < 0 || player_index >= lyrion_player_count) return false;
    lyrion_command_t command = { .player_index = player_index, .type = type, .volume = value };
    return xQueueSend(lyrion_command_queue, &command, 0) == pdTRUE;
}

/* Download a small file (a room cover) into buf. */
static bool rooms_http_get(const char *url, uint8_t *buf, size_t max, size_t *out_len)
{
    esp_http_client_config_t cfg = {
        .url = url, .method = HTTP_METHOD_GET, .timeout_ms = 5000,
        .buffer_size = 4096, .buffer_size_tx = 1024,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client == NULL) return false;
    size_t len = 0;
    bool ok = (esp_http_client_open(client, 0) == ESP_OK);
    if (ok) {
        (void)esp_http_client_fetch_headers(client);
        while (ok) {
            const int n = esp_http_client_read(client, (char *)buf + len, (int)(max - len));
            if (n < 0) { ok = false; break; }
            if (n == 0) break;
            len += (size_t)n;
            if (len >= max) { ok = false; break; }
        }
        if (ok && esp_http_client_get_status_code(client) != 200) ok = false;
        esp_http_client_close(client);
    }
    esp_http_client_cleanup(client);
    *out_len = len;
    return ok;
}

static void rooms_fetch_cover(int i)
{
    char url[192];
    snprintf(url, sizeof(url), "%s/music/current/cover_%dx%d.png?player=%s",
             LYRION_SERVER_BASE, ROOM_COVER_PX, ROOM_COVER_PX, lyrion_players[i].id);
    const size_t max = 256 * 1024;
    uint8_t *buf = heap_caps_malloc(max, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (buf == NULL) return;
    size_t len = 0;
    static const uint8_t png_sig[8] = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
    room_cover_msg_t m = { .idx = i };
    const bool ok = rooms_http_get(url, buf, max, &len) && len >= 8 && memcmp(buf, png_sig, 8) == 0 &&
                    png_decode_rgb565_buffer(buf, len, &m.pixels, &m.w, &m.h, &m.padded, &m.size);
    heap_caps_free(buf);
    if (!ok) return;
    if (!ui_post(rooms_cover_job, &m, sizeof(m))) heap_caps_free(m.pixels);
}

/* One pass over every room: status, switches, and covers that changed. */
/* Which rooms play in sync: one "syncgroups" query for the whole house.
 * masks[i] = the rooms in room i's group (itself included), 0 if alone. */
static void rooms_read_groups(char *buf, size_t buf_size, int masks[LMS_MAX_PLAYERS])
{
    for (int i = 0; i < LMS_MAX_PLAYERS; i++) masks[i] = 0;
    static const char req[] = "{\"id\":1,\"method\":\"slim.request\",\"params\":[\"\",[\"syncgroups\",\"?\"]]}";
    if (!source_http_query(0, req, buf, buf_size)) return;
    cJSON *root = cJSON_Parse(buf);
    cJSON *res = root ? cJSON_GetObjectItem(root, "result") : NULL;
    cJSON *loop = cJSON_IsObject(res) ? cJSON_GetObjectItem(res, "syncgroups_loop") : NULL;
    cJSON *g = NULL;
    cJSON_ArrayForEach(g, loop) {
        cJSON *mem = cJSON_GetObjectItem(g, "sync_members");
        if (!cJSON_IsString(mem) || mem->valuestring == NULL) continue;
        int mask = 0;
        char ids[256];
        snprintf(ids, sizeof(ids), "%s", mem->valuestring);
        for (char *tok = strtok(ids, ","); tok != NULL; tok = strtok(NULL, ",")) {
            for (int i = 0; i < lyrion_player_count; i++) {
                if (strcasecmp(tok, lyrion_players[i].id) == 0) mask |= 1 << i;
            }
        }
        for (int i = 0; i < lyrion_player_count; i++) {
            if (mask & (1 << i)) masks[i] = mask;
        }
    }
    if (root) cJSON_Delete(root);
}

static void rooms_poll_all(void)
{
    char *buf = heap_caps_malloc(16384, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (buf == NULL) return;

    int group_masks[LMS_MAX_PLAYERS];
    rooms_read_groups(buf, 16384, group_masks);

    for (int i = 0; i < lyrion_player_count && rooms_open; i++) {
        room_msg_t m = { .idx = i, .state = 0, .volume = -1,
                         .amp = ROOM_SWITCH_NONE, .pre = ROOM_SWITCH_NONE,
                         .group_mask = group_masks[i] };
        char json[192];
        snprintf(json, sizeof(json),
                 "{\"id\":1,\"method\":\"slim.request\",\"params\":[\"%s\",[\"status\",\"-\",1,\"tags:alc\"]]}",
                 lyrion_players[i].id);
        if (source_http_query(i, json, buf, 16384)) {
            cJSON *root = cJSON_Parse(buf);
            cJSON *res = root ? cJSON_GetObjectItem(root, "result") : NULL;
            if (cJSON_IsObject(res)) {
                cJSON *mode = cJSON_GetObjectItem(res, "mode");
                cJSON *power = cJSON_GetObjectItem(res, "power");
                cJSON *conn = cJSON_GetObjectItem(res, "player_connected");
                cJSON *vol = cJSON_GetObjectItem(res, "mixer volume");
                const bool on = (!cJSON_IsNumber(power) || power->valueint != 0) &&
                                (!cJSON_IsNumber(conn) || conn->valueint != 0);
                const bool playing = cJSON_IsString(mode) && mode->valuestring &&
                                     strcmp(mode->valuestring, "play") == 0;
                m.state = !on ? 0 : (playing ? 2 : 1);
                if (cJSON_IsNumber(vol)) m.volume = vol->valueint < 0 ? -vol->valueint : vol->valueint;

                char key[64] = "";
                cJSON *loop = cJSON_GetObjectItem(res, "playlist_loop");
                cJSON *track = cJSON_IsArray(loop) ? cJSON_GetArrayItem(loop, 0) : NULL;
                if (cJSON_IsObject(track)) {
                    cJSON *t = cJSON_GetObjectItem(track, "title");
                    cJSON *a = cJSON_GetObjectItem(track, "artist");
                    cJSON *c = cJSON_GetObjectItem(track, "coverid");
                    if (cJSON_IsString(t) && t->valuestring) snprintf(m.title, sizeof(m.title), "%s", t->valuestring);
                    if (cJSON_IsString(a) && a->valuestring) snprintf(m.artist, sizeof(m.artist), "%s", a->valuestring);
                    if (cJSON_IsString(c) && c->valuestring) snprintf(key, sizeof(key), "c:%.60s", c->valuestring);
                    else snprintf(key, sizeof(key), "t:%.60s", m.title);
                }
                if (key[0] && strcmp(key, rooms_cover_key[i]) != 0) {
                    snprintf(rooms_cover_key[i], sizeof(rooms_cover_key[i]), "%s", key);
                    rooms_fetch_cover(i);
                }
            }
            if (root) cJSON_Delete(root);
        }

        const ha_player_switches_t *sw = ha_switches_for_index(i);
        if (sw->amp_entity != NULL) {
            bool st = false;
            m.amp = (HA_TOKEN_SET && ha_switch_get_state(sw->amp_entity, &st)) ? (st ? 1 : 0) : ROOM_SWITCH_UNKNOWN;
        }
        if (sw->preamp_entity != NULL) {
            bool st = false;
            m.pre = (HA_TOKEN_SET && ha_switch_get_state(sw->preamp_entity, &st)) ? (st ? 1 : 0) : ROOM_SWITCH_UNKNOWN;
        }
        ui_post(rooms_card_job, &m, sizeof(m));
    }
    heap_caps_free(buf);
}

/* Opening Rooms: wake the command task so the cards fill in straight away
 * (an empty Home Assistant command means "just refresh"). */
static void rooms_wake_worker(void)
{
    if (ha_command_queue == NULL) return;
    ha_command_t nudge;
    memset(&nudge, 0, sizeof(nudge));
    xQueueSend(ha_command_queue, &nudge, 0);
}
