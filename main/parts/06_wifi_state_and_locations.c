/*
 * P4-LMS - Wi-Fi state and the per-network location profiles
 *
 * Part of main.c.  This file is #included by main.c (in order) and is not
 * compiled on its own, so it shares main.c's includes and static names.
 */

/* ============================================================
 * WIFI
 * ============================================================ */

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1
#define WIFI_MAX_RETRY     10

static EventGroupHandle_t wifi_event_group = NULL;

/* Wi-Fi picker: the network in use (saved on the display, or the one built
 * into the firmware), and switching state. */
#define WIFI_ATTEMPT_FAIL_BIT BIT2          /* a manual connect attempt failed */
#define WIFI_NVS_SSID "wifi_ssid"
#define WIFI_NVS_PASS "wifi_pass"
static char wifi_cur_ssid[33] = "";
static char wifi_cur_pass[65] = "";
static volatile bool wifi_manual = false;   /* true while the picker is switching */
static volatile uint8_t wifi_last_reason = 0;
/* ========================================================================
 * LOCATION PROFILES
 * One per Wi-Fi network: its password, its Lyrion server and that server's
 * players.  At start-up the profile for the network in use is loaded, so each
 * location gets its own server and player buttons automatically.  Switching
 * location saves the profile and restarts the display (like theme changes),
 * so the player list never changes underneath the running tasks.
 * ======================================================================== */
#define LMS_MAX_PROFILES 4
#define LMS_NVS_BLOB     "lms_prof"
#define LMS_NVS_COUNT    "lms_cnt"
#define LMS_NVS_LAST     "lms_last"

typedef struct {
    char ssid[33];
    char pass[65];
    char server[64];                 /* "http://host:port" */
    uint8_t count;
    lyrion_player_t players[LMS_MAX_PLAYERS];
} lms_profile_t;

static lms_profile_t lms_profiles[LMS_MAX_PROFILES];
static int lms_profile_count = 0;
static int lms_active = -1;
static volatile bool lms_restart_pending = false;

static void lms_profiles_save(void)
{
    nvs_handle_t nvs;
    if (nvs_open(PLAYER_NVS_NAMESPACE, NVS_READWRITE, &nvs) != ESP_OK) return;
    if (nvs_set_blob(nvs, LMS_NVS_BLOB, lms_profiles, sizeof(lms_profile_t) * lms_profile_count) == ESP_OK &&
        nvs_set_u8(nvs, LMS_NVS_COUNT, (uint8_t)lms_profile_count) == ESP_OK) {
        nvs_commit(nvs);
    }
    nvs_close(nvs);
}

/* Your home setup, exactly as it was before profiles existed. */
static void lms_fill_home(lms_profile_t *p)
{
    memset(p, 0, sizeof(*p));
    snprintf(p->ssid, sizeof(p->ssid), "%s", CONFIG_P4_LMS_WIFI_SSID);
    snprintf(p->pass, sizeof(p->pass), "%s", CONFIG_P4_LMS_WIFI_PASSWORD);
    snprintf(p->server, sizeof(p->server), "%s", LMS_HOME_SERVER);
    p->count = (uint8_t)lms_home_player_count;
    memcpy(p->players, lyrion_players, sizeof(lyrion_player_t) * lms_home_player_count);
}

static void lms_profiles_load(void)
{
    lms_profile_count = 0;
    nvs_handle_t nvs;
    if (nvs_open(PLAYER_NVS_NAMESPACE, NVS_READONLY, &nvs) == ESP_OK) {
        uint8_t n = 0;
        size_t len = sizeof(lms_profiles);
        if (nvs_get_u8(nvs, LMS_NVS_COUNT, &n) == ESP_OK && n > 0 && n <= LMS_MAX_PROFILES &&
            nvs_get_blob(nvs, LMS_NVS_BLOB, lms_profiles, &len) == ESP_OK &&
            len == sizeof(lms_profile_t) * n) {
            lms_profile_count = n;
        }
        nvs_close(nvs);
    }
    if (lms_profile_count == 0) {              /* first run: start with home */
        lms_fill_home(&lms_profiles[0]);
        lms_profile_count = 1;
        lms_profiles_save();
    }
}

static int lms_find_profile(const char *ssid)
{
    for (int i = 0; i < lms_profile_count; i++) {
        if (strcmp(lms_profiles[i].ssid, ssid) == 0) return i;
    }
    return -1;
}

/* Make profile i the one the display runs on. */
static void lms_apply_profile(int i)
{
    if (i < 0 || i >= lms_profile_count) return;
    const lms_profile_t *p = &lms_profiles[i];
    snprintf(lyrion_server_base, sizeof(lyrion_server_base), "%s", p->server);
    snprintf(lyrion_server_url, sizeof(lyrion_server_url), "%s/jsonrpc.js", p->server);
    lyrion_player_count = (p->count > 0 && p->count <= LMS_MAX_PLAYERS) ? p->count : 0;
    memcpy(lyrion_players, p->players, sizeof(lyrion_player_t) * LMS_MAX_PLAYERS);
    lms_active = i;

    nvs_handle_t nvs;
    if (nvs_open(PLAYER_NVS_NAMESPACE, NVS_READWRITE, &nvs) == ESP_OK) {
        nvs_set_u8(nvs, LMS_NVS_LAST, (uint8_t)i);
        nvs_commit(nvs);
        nvs_close(nvs);
    }
    ESP_LOGI(TAG, "Location: %s -> %s (%d player%s)", p->ssid, p->server,
             lyrion_player_count, lyrion_player_count == 1 ? "" : "s");
}

/* At start-up: the profile for this Wi-Fi network, else the last one used. */
static void lms_select_at_boot(const char *ssid)
{
    int i = lms_find_profile(ssid);
    if (i < 0) {
        uint8_t last = 0;
        nvs_handle_t nvs;
        if (nvs_open(PLAYER_NVS_NAMESPACE, NVS_READONLY, &nvs) == ESP_OK) {
            nvs_get_u8(nvs, LMS_NVS_LAST, &last);
            nvs_close(nvs);
        }
        i = (last < lms_profile_count) ? last : 0;
    }
    lms_apply_profile(i);
}

/* Add or update the profile for a network; returns its index. */
static int lms_save_profile(const char *ssid, const char *pass, const char *server,
                            const lyrion_player_t *players, int count)
{
    int i = lms_find_profile(ssid);
    if (i < 0) {
        if (lms_profile_count < LMS_MAX_PROFILES) {
            i = lms_profile_count++;
        } else {
            /* Full: reuse the oldest slot that isn't the one in use. */
            i = (lms_active == LMS_MAX_PROFILES - 1) ? LMS_MAX_PROFILES - 2 : LMS_MAX_PROFILES - 1;
        }
    }
    lms_profile_t *p = &lms_profiles[i];
    memset(p, 0, sizeof(*p));
    snprintf(p->ssid, sizeof(p->ssid), "%s", ssid);
    snprintf(p->pass, sizeof(p->pass), "%s", pass);
    snprintf(p->server, sizeof(p->server), "%s", server);
    p->count = (uint8_t)(count > LMS_MAX_PLAYERS ? LMS_MAX_PLAYERS : count);
    memcpy(p->players, players, sizeof(lyrion_player_t) * p->count);
    lms_profiles_save();
    return i;
}

static void wifi_save_creds(const char *ssid, const char *pass);
static esp_err_t wifi_apply_config(const char *ssid, const char *pass);

static void lms_request_restart(const char *why)
{
    ESP_LOGW(TAG, "Restarting: %s", why);
    lms_restart_pending = true;          /* wifi_status_task restarts shortly */
}

/* Roaming: after the current network has failed for a while, try the other
 * networks that have a saved profile, in turn. */
static char wifi_try_ssid[33] = "";
static char wifi_try_pass[65] = "";
static int  wifi_roam_next = 0;

static void wifi_roam_next_network(void)
{
    /* Candidates: the saved network first, then every profile's network. */
    const int total = 1 + lms_profile_count;
    for (int step = 0; step < total; step++) {
        const int k = (wifi_roam_next + step) % total;
        const char *ssid = (k == 0) ? wifi_cur_ssid : lms_profiles[k - 1].ssid;
        const char *pass = (k == 0) ? wifi_cur_pass : lms_profiles[k - 1].pass;
        if (k > 0 && strcmp(ssid, wifi_cur_ssid) == 0) continue;     /* same as k == 0 */
        wifi_roam_next = (k + 1) % total;
        if (strcmp(ssid, wifi_try_ssid) != 0) {
            ESP_LOGI(TAG, "Wi-Fi: trying known network %s", ssid);
        }
        snprintf(wifi_try_ssid, sizeof(wifi_try_ssid), "%s", ssid);
        snprintf(wifi_try_pass, sizeof(wifi_try_pass), "%s", pass);
        return;
    }
}

/* The Wi-Fi screen (defined further down) - declared here for the idle timer. */
static lv_obj_t *wifi_overlay;
static volatile bool wifi_busy;
static void wifi_close(void);
static lv_obj_t *lms_overlay;
static void lms_close(void);
static int wifi_retry_count = 0;

static lv_obj_t *connection_label = NULL;


