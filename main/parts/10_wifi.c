/*
 * P4-LMS - Wi-Fi events, start-up and the header Wi-Fi status
 *
 * Part of main.c.  This file is #included by main.c (in order) and is not
 * compiled on its own, so it shares main.c's includes and static names.
 */

/* ============================================================
 * WIFI EVENT HANDLER
 * ============================================================ */

/*
 * Reconnection policy: retry after one second for the first few attempts,
 * then every ten seconds - forever.  (The old code gave up after ten
 * attempts and the player stayed offline until it was power-cycled.)
 */
static esp_timer_handle_t wifi_reconnect_timer = NULL;

static void wifi_reconnect_timer_cb(void *arg)
{
    (void)arg;

    esp_err_t err = esp_wifi_connect();

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_wifi_connect() returned %s", esp_err_to_name(err));
    }
}

static void wifi_event_handler(
    void *arg,
    esp_event_base_t event_base,
    int32_t event_id,
    void *event_data)
{
    (void)arg;

    /* --------------------------------------------------------
     * WIFI START
     * -------------------------------------------------------- */

    if (event_base == WIFI_EVENT &&
        event_id == WIFI_EVENT_STA_START) {

        ESP_LOGI(TAG, "Wi-Fi started");

        esp_err_t err = esp_wifi_connect();

        if (err != ESP_OK) {
            ESP_LOGW(TAG, "esp_wifi_connect() returned %s", esp_err_to_name(err));
        }

        return;
    }


    /* --------------------------------------------------------
     * WIFI DISCONNECTED
     * -------------------------------------------------------- */

    if (event_base == WIFI_EVENT &&
        event_id == WIFI_EVENT_STA_DISCONNECTED) {

        xEventGroupClearBits(wifi_event_group, WIFI_CONNECTED_BIT);

        wifi_event_sta_disconnected_t *disc = (wifi_event_sta_disconnected_t *)event_data;
        if (disc != NULL) {
            wifi_last_reason = disc->reason;
        }
        /* While the Wi-Fi picker is switching networks it handles retries
         * (and the fallback) itself. */
        if (wifi_manual) {
            xEventGroupSetBits(wifi_event_group, WIFI_ATTEMPT_FAIL_BIT);
            return;
        }

        wifi_retry_count++;

        ESP_LOGW(TAG, "Wi-Fi disconnected - attempt %d", wifi_retry_count);

        /* After a burst of failures tell the UI, but keep trying. */
        uint64_t delay_us = 1000000ULL;
        if (wifi_retry_count >= WIFI_MAX_RETRY) {
            xEventGroupSetBits(wifi_event_group, WIFI_FAIL_BIT);
            delay_us = 10000000ULL;
            /* Every third slow retry, move on to the next known network, so
             * the display finds its way home (or to another location) itself. */
            if (lms_profile_count > 0 && (wifi_retry_count % 3) == 0) {
                wifi_roam_next_network();
                wifi_apply_config(wifi_try_ssid, wifi_try_pass);
            }
        }

        if (wifi_reconnect_timer != NULL) {
            /* Returns an error if a retry is already pending; that is fine. */
            esp_timer_start_once(wifi_reconnect_timer, delay_us);
        }

        return;
    }


    /* --------------------------------------------------------
     * GOT IP ADDRESS
     * -------------------------------------------------------- */

    if (event_base == IP_EVENT &&
        event_id == IP_EVENT_STA_GOT_IP) {

        ip_event_got_ip_t *event =
            (ip_event_got_ip_t *)event_data;

        wifi_retry_count = 0;
        wifi_have_ip = true;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));

        if (!wifi_manual && wifi_try_ssid[0] && strcmp(wifi_try_ssid, wifi_cur_ssid) != 0) {
            ESP_LOGI(TAG, "Wi-Fi: joined known network %s", wifi_try_ssid);
            snprintf(wifi_cur_ssid, sizeof(wifi_cur_ssid), "%s", wifi_try_ssid);
            snprintf(wifi_cur_pass, sizeof(wifi_cur_pass), "%s", wifi_try_pass);
            wifi_save_creds(wifi_cur_ssid, wifi_cur_pass);
        }
        {
            const int prof = lms_find_profile(wifi_cur_ssid);
            if (!wifi_manual && prof >= 0 && prof != lms_active) {
                lms_request_restart("this network has its own Lyrion server");
            }
        }

        xEventGroupClearBits(wifi_event_group, WIFI_FAIL_BIT);
        xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT);

        /* Start the local clock once the network is available. */
        screensaver_start_ntp();

        /* Ask the status task to refresh immediately. */
        if (lyrion_refresh_event != NULL) {
            xEventGroupSetBits(lyrion_refresh_event, LYRION_REFRESH_BIT);
        }

        return;
    }
}


/* Build and apply the station config for one network.  An empty password
 * means an open network; otherwise WPA2 or better is required. */
static esp_err_t wifi_apply_config(const char *ssid, const char *pass)
{
    if (ssid != wifi_try_ssid) snprintf(wifi_try_ssid, sizeof(wifi_try_ssid), "%s", ssid);
    if (pass != wifi_try_pass) snprintf(wifi_try_pass, sizeof(wifi_try_pass), "%s", pass ? pass : "");
    wifi_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.sta.threshold.authmode = (pass != NULL && pass[0]) ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    cfg.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
    cfg.sta.pmf_cfg.capable = true;
    cfg.sta.pmf_cfg.required = false;
    /* SSIDs can be exactly 32 bytes (no terminator) and passwords 63 chars. */
    memcpy(cfg.sta.ssid, ssid, strnlen(ssid, sizeof(cfg.sta.ssid)));
    if (pass != NULL) {
        memcpy(cfg.sta.password, pass, strnlen(pass, sizeof(cfg.sta.password) - 1));
    }
    return esp_wifi_set_config(WIFI_IF_STA, &cfg);
}

/* Fill wifi_cur_* from NVS if a network was saved, else from sdkconfig. */
static bool wifi_load_saved_creds(void)
{
    snprintf(wifi_cur_ssid, sizeof(wifi_cur_ssid), "%s", CONFIG_P4_LMS_WIFI_SSID);
    snprintf(wifi_cur_pass, sizeof(wifi_cur_pass), "%s", CONFIG_P4_LMS_WIFI_PASSWORD);

    nvs_handle_t nvs;
    if (nvs_open(PLAYER_NVS_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) {
        return false;
    }
    char ssid[33] = "", pass[65] = "";
    size_t ssid_len = sizeof(ssid), pass_len = sizeof(pass);
    const bool ok = nvs_get_str(nvs, WIFI_NVS_SSID, ssid, &ssid_len) == ESP_OK && ssid[0] &&
                    nvs_get_str(nvs, WIFI_NVS_PASS, pass, &pass_len) == ESP_OK;
    nvs_close(nvs);
    if (ok) {
        snprintf(wifi_cur_ssid, sizeof(wifi_cur_ssid), "%s", ssid);
        snprintf(wifi_cur_pass, sizeof(wifi_cur_pass), "%s", pass);
    }
    return ok;
}

static bool wifi_has_saved_creds(void)
{
    nvs_handle_t nvs;
    if (nvs_open(PLAYER_NVS_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) return false;
    char ssid[33] = "";
    size_t len = sizeof(ssid);
    const bool ok = nvs_get_str(nvs, WIFI_NVS_SSID, ssid, &len) == ESP_OK && ssid[0];
    nvs_close(nvs);
    return ok;
}

static void wifi_save_creds(const char *ssid, const char *pass)
{
    nvs_handle_t nvs;
    if (nvs_open(PLAYER_NVS_NAMESPACE, NVS_READWRITE, &nvs) != ESP_OK) return;
    if (nvs_set_str(nvs, WIFI_NVS_SSID, ssid) == ESP_OK &&
        nvs_set_str(nvs, WIFI_NVS_PASS, pass) == ESP_OK) {
        nvs_commit(nvs);
    }
    nvs_close(nvs);
}

static void wifi_forget_creds(void)
{
    nvs_handle_t nvs;
    if (nvs_open(PLAYER_NVS_NAMESPACE, NVS_READWRITE, &nvs) != ESP_OK) return;
    nvs_erase_key(nvs, WIFI_NVS_SSID);
    nvs_erase_key(nvs, WIFI_NVS_PASS);
    nvs_commit(nvs);
    nvs_close(nvs);
}

/* ============================================================
 * WIFI INITIALIZATION
 * ============================================================ */

static void wifi_init(void)
{
    ESP_LOGI(
        TAG,
        "Initializing Wi-Fi"
    );


    wifi_event_group =
        xEventGroupCreate();


    ESP_ERROR_CHECK(
        wifi_event_group != NULL
            ? ESP_OK
            : ESP_ERR_NO_MEM
    );


    /* --------------------------------------------------------
     * NVS
     * -------------------------------------------------------- */

    esp_err_t ret =
        nvs_flash_init();


    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {

        ESP_ERROR_CHECK(
            nvs_flash_erase()
        );


        ret =
            nvs_flash_init();
    }


    ESP_ERROR_CHECK(ret);


    /* --------------------------------------------------------
     * NETWORK INTERFACE
     * -------------------------------------------------------- */

    ESP_ERROR_CHECK(
        esp_netif_init()
    );


    /* --------------------------------------------------------
     * DEFAULT EVENT LOOP
     * -------------------------------------------------------- */

    ESP_ERROR_CHECK(
        esp_event_loop_create_default()
    );


    /* --------------------------------------------------------
     * CREATE WIFI STATION INTERFACE
     * -------------------------------------------------------- */

    esp_netif_t *wifi_netif =
        esp_netif_create_default_wifi_sta();


    ESP_ERROR_CHECK(
        wifi_netif != NULL
            ? ESP_OK
            : ESP_FAIL
    );


    /* --------------------------------------------------------
     * WIFI DRIVER
     * -------------------------------------------------------- */

    wifi_init_config_t wifi_init_config =
        WIFI_INIT_CONFIG_DEFAULT();


    ESP_ERROR_CHECK(
        esp_wifi_init(
            &wifi_init_config
        )
    );


    /* --------------------------------------------------------
     * RECONNECT TIMER
     * -------------------------------------------------------- */

    const esp_timer_create_args_t reconnect_args = {
        .callback = wifi_reconnect_timer_cb,
        .arg = NULL,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "wifi_reconnect"
    };

    ESP_ERROR_CHECK(
        esp_timer_create(
            &reconnect_args,
            &wifi_reconnect_timer
        )
    );


    /* --------------------------------------------------------
     * WIFI EVENTS
     * -------------------------------------------------------- */

    ESP_ERROR_CHECK(
        esp_event_handler_instance_register(
            WIFI_EVENT,
            ESP_EVENT_ANY_ID,
            &wifi_event_handler,
            NULL,
            NULL
        )
    );


    /* --------------------------------------------------------
     * IP EVENTS
     * -------------------------------------------------------- */

    ESP_ERROR_CHECK(
        esp_event_handler_instance_register(
            IP_EVENT,
            IP_EVENT_STA_GOT_IP,
            &wifi_event_handler,
            NULL,
            NULL
        )
    );


    /* --------------------------------------------------------
     * STATION CONFIGURATION
     * -------------------------------------------------------- */

    /* A network chosen on the display (saved in NVS) wins; otherwise the
     * one built into the firmware from sdkconfig. */
    const bool saved = wifi_load_saved_creds();
    ESP_LOGI(TAG, "Wi-Fi SSID configured: %s (%s)", wifi_cur_ssid,
             saved ? "chosen on the display" : "built-in");

    ESP_ERROR_CHECK(
        esp_wifi_set_mode(
            WIFI_MODE_STA
        )
    );

    ESP_ERROR_CHECK(wifi_apply_config(wifi_cur_ssid, wifi_cur_pass));


    /* --------------------------------------------------------
     * START WIFI
     * -------------------------------------------------------- */

    ESP_ERROR_CHECK(
        esp_wifi_start()
    );


    ESP_LOGI(
        TAG,
        "Wi-Fi initialization complete"
    );
}


/* ============================================================
 * UPDATE WIFI STATUS ON SCREEN
 * ============================================================ */

typedef struct { char text[64]; uint32_t color; } wifi_label_msg_t;

static void update_wifi_label_job(void *arg)
{
    const wifi_label_msg_t *m = (const wifi_label_msg_t *)arg;
    ui_set_label(connection_label, m->text);
    lv_obj_set_style_text_color(connection_label, lv_color_hex(m->color), 0);
}

static bool update_wifi_label(
    const char *text,
    uint32_t color)
{
    if (connection_label == NULL) {
        return false;
    }
    wifi_label_msg_t m;
    snprintf(m.text, sizeof(m.text), "%s", text);
    m.color = color;
    return ui_post(update_wifi_label_job, &m, sizeof(m));
}


/* ============================================================
 * WIFI STATUS TASK
 *
 * Runs for the life of the program so the header follows the connection:
 * connecting -> connected (with the IP address) -> offline/retrying.
 * ============================================================ */

static void wifi_status_task(void *arg)
{
    (void)arg;

    typedef enum {
        WIFI_UI_UNKNOWN = 0,
        WIFI_UI_CONNECTING,
        WIFI_UI_CONNECTED,
        WIFI_UI_OFFLINE
    } wifi_ui_state_t;

    wifi_ui_state_t shown = WIFI_UI_UNKNOWN;
    while (true) {
        if (lms_restart_pending) {
            vTaskDelay(pdMS_TO_TICKS(2500));        /* let any message be read */
            esp_restart();
        }

        EventBits_t bits = xEventGroupGetBits(wifi_event_group);
        wifi_ui_state_t state;

        if (bits & WIFI_CONNECTED_BIT) {
            state = WIFI_UI_CONNECTED;
        }
        else if (bits & WIFI_FAIL_BIT) {
            state = WIFI_UI_OFFLINE;
        }
        else {
            state = WIFI_UI_CONNECTING;
        }

        if (state != shown) {

            bool drawn = false;

            if (state == WIFI_UI_CONNECTED) {

                esp_netif_t *netif =
                    esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");

                esp_netif_ip_info_t ip_info;
                memset(&ip_info, 0, sizeof(ip_info));

                if (netif != NULL &&
                    esp_netif_get_ip_info(netif, &ip_info) == ESP_OK) {

                    char status[64];

                    snprintf(
                        status,
                        sizeof(status),
                        LV_SYMBOL_WIFI "  " IPSTR,
                        IP2STR(&ip_info.ip)
                    );

                    drawn = update_wifi_label(status, UI_ACCENT_GLOW);

                    ESP_LOGI(
                        TAG,
                        "Wi-Fi connected - IP: " IPSTR,
                        IP2STR(&ip_info.ip)
                    );
                }
                else {
                    drawn = update_wifi_label(
                        LV_SYMBOL_WIFI "  Connected",
                        UI_ACCENT_GLOW
                    );
                }
            }
            else if (state == WIFI_UI_OFFLINE) {
                drawn = update_wifi_label(
                    LV_SYMBOL_WIFI "  Offline - retrying",
                    0xC75A5A
                );
            }
            else {
                drawn = update_wifi_label(
                    LV_SYMBOL_WIFI "  Connecting...",
                    UI_CONN_AMBER
                );
            }

            /* If the display lock was busy, try again on the next pass. */
            if (drawn) {
                shown = state;
            }
        }

        vTaskDelay(pdMS_TO_TICKS(500));
    }
}


