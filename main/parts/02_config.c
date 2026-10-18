/*
 * P4-LMS - Lyrion server / players, Home Assistant settings, remembered player
 *
 * Part of main.c.  This file is #included by main.c (in order) and is not
 * compiled on its own, so it shares main.c's includes and static names.
 */

/* ============================================================
 * LYRION MUSIC SERVER
 * ============================================================ */

/* Starting Lyrion server.  You normally don't need to edit this: on the
 * display, Settings > Lyrion server > FIND SERVERS finds yours and loads its
 * players (saved per Wi-Fi network).  Or put your server's address here. */
#define LMS_HOME_SERVER "http://192.168.1.100:9000"
static char lyrion_server_base[64] = LMS_HOME_SERVER;
static char lyrion_server_url[80]  = LMS_HOME_SERVER "/jsonrpc.js";
#define LYRION_SERVER_BASE lyrion_server_base
#define LYRION_SERVER_URL  lyrion_server_url

#define LYRION_RESPONSE_MAX 8192

/* Clock time zone until one is chosen on the web page: an index into
 * tz_choices[] (07_declarations_audio_screensaver.c). */
#define P4_DEFAULT_TZ 19     /* UTC - choose yours on the web page */

/* How often the selected player is polled. */
#define LYRION_POLL_MS      2000

/* Shared display text buffer size. */
#define SOURCE_TEXT_MAX        96

#define ALBUM_ART_WIDTH 368
#define ALBUM_ART_HEIGHT 368
#define ALBUM_ART_MAX_JPEG_SIZE (2 * 1024 * 1024)

/* 1 = ask Lyrion for PNG covers (decoded in software, slightly slower);
 * 0 = ask for JPEG (decoded by the P4 hardware JPEG engine, faster).
 * Whichever is chosen, the other one is tried if the first attempt fails. */
#define ALBUM_ART_FORCE_PNG 1

typedef struct {
    char name[48];            /* full Lyrion player name             */
    char short_name[20];      /* label shown on the header button    */
    char id[18];              /* player MAC address                  */
} lyrion_player_t;

/* Up to four players fit across the header. */
#define LMS_MAX_PLAYERS 4

/* The players of the current location (your home ones to begin with).  At
 * start-up these are replaced by the saved profile for the Wi-Fi network in
 * use, so they never change while the display is running. */
/* A placeholder until FIND SERVERS loads your real players. */
static lyrion_player_t lyrion_players[LMS_MAX_PLAYERS] = {
    { "Player", "Player", "00:00:00:00:00:00" },
};
static const int lms_home_player_count = 1;
static int lyrion_player_count = 1;

#define LYRION_PLAYER_COUNT lyrion_player_count

/* ------------------------------------------------------------------------
 * HOME ASSISTANT - stereo power switches
 * ------------------------------------------------------------------------
 * The Home Assistant address, its access token and each player's amp /
 * pre-amp switch names are set on the display's web settings page: open
 * http://<display IP address> in a browser (the address is shown in the
 * display's SETTINGS screen).  They are saved on the board, so no token ever
 * needs to be pasted into this file.  The values below are only the
 * starting defaults.
 * -------------------------------------------------------------------- */
#define HA_DEFAULT_BASE  ""          /* set on the web page */
#define HA_RESPONSE_MAX  4096    /* a state reply carries all its attributes */
#define HA_POLL_MS       3000     /* command task's idle wake-up (amp/alarm/weather/rooms checks) */

typedef struct {
    const char *player_id;        /* Lyrion player MAC these switches belong to     */
    const char *amp_entity;       /* switch entity id, e.g. "switch.office_amplifier" */
    const char *preamp_entity;    /* NULL if this room has no separate pre-amp switch */
    uint8_t     delay_s;          /* seconds between the two switches (amp + pre-amp) */
    bool        amp_first;        /* false: pre-amp on first / off last (the usual order) */
} ha_player_switches_t;

#define HA_SEQ_DEFAULT_DELAY_S 3
#define HA_SEQ_MAX_DELAY_S     15

/* Starting switch names for your home players (matched by player MAC). */
/* Switch names are set per player on the web page; none are built in.
 * (Example entry: { "aa:bb:cc:dd:ee:ff", "switch.living_room_amp",
 *                   "switch.living_room_preamp", HA_SEQ_DEFAULT_DELAY_S, false }) */
static const ha_player_switches_t ha_switch_defaults[] = {
    { NULL, NULL, NULL, 0, false },
};
static const ha_player_switches_t ha_no_switches = { NULL, NULL, NULL, 0, false };

/* The settings in use (loaded at start-up, changed from the web page). */
static char ha_base[96]   = HA_DEFAULT_BASE;
static char ha_token[320] = "";
#define HA_TOKEN_SET   (ha_token[0] != '\0')

/* Switch names per player MAC, for every location's players. */
#define HA_MAP_MAX 12
typedef struct { char id[18]; char amp[64]; char pre[64]; } ha_map_entry_v1_t;   /* as saved before stage 16 */
typedef struct { char id[18]; char amp[64]; char pre[64]; uint8_t delay_s; uint8_t amp_first; } ha_map_entry_t;
static ha_map_entry_t       ha_map[HA_MAP_MAX];
static ha_player_switches_t ha_map_view[HA_MAP_MAX];   /* same, as the lookup returns it */
static int                  ha_map_count = 0;

/* The web page changes the address, token and switches while other tasks use
 * them: changes are made on a copy and swapped in under this lock, and
 * readers copy the address and token out under it. */
static portMUX_TYPE ha_cfg_mux = portMUX_INITIALIZER_UNLOCKED;

/* Every slot always points at its own entry (an unused one is an empty
 * name), so a lookup racing a change never meets a NULL name. */
static void ha_map_rebuild_views(void)
{
    for (int k = 0; k < HA_MAP_MAX; k++) {
        ha_map_view[k].player_id = ha_map[k].id;
        ha_map_view[k].amp_entity = ha_map[k].amp[0] ? ha_map[k].amp : NULL;
        ha_map_view[k].preamp_entity = ha_map[k].pre[0] ? ha_map[k].pre : NULL;
        ha_map_view[k].delay_s = ha_map[k].delay_s <= HA_SEQ_MAX_DELAY_S ? ha_map[k].delay_s : HA_SEQ_DEFAULT_DELAY_S;
        ha_map_view[k].amp_first = ha_map[k].amp_first != 0;
    }
}

static void ha_map_load_defaults(void)
{
    ha_map_count = 0;
    for (size_t k = 0; k < sizeof(ha_switch_defaults) / sizeof(ha_switch_defaults[0]) &&
                       ha_map_count < HA_MAP_MAX; k++) {
        if (ha_switch_defaults[k].player_id == NULL) continue;     /* the empty entry */
        ha_map_entry_t *e = &ha_map[ha_map_count++];
        memset(e, 0, sizeof(*e));
        snprintf(e->id, sizeof(e->id), "%s", ha_switch_defaults[k].player_id);
        snprintf(e->amp, sizeof(e->amp), "%s", ha_switch_defaults[k].amp_entity ? ha_switch_defaults[k].amp_entity : "");
        snprintf(e->pre, sizeof(e->pre), "%s", ha_switch_defaults[k].preamp_entity ? ha_switch_defaults[k].preamp_entity : "");
        e->delay_s = ha_switch_defaults[k].delay_s;
        e->amp_first = ha_switch_defaults[k].amp_first ? 1 : 0;
    }
    ha_map_rebuild_views();
}



/*
 * Currently selected player.
 *
 * Start with the first player.
 */
static volatile int current_player_index = 0;

static const ha_player_switches_t *ha_switches_for_current(void)
{
    const int i = current_player_index;
    if (i < 0 || i >= lyrion_player_count) return &ha_no_switches;
    for (int k = 0; k < ha_map_count; k++) {
        if (strcasecmp(ha_map_view[k].player_id, lyrion_players[i].id) == 0) {
            return ha_map_view[k].amp_entity != NULL ? &ha_map_view[k] : &ha_no_switches;
        }
    }
    return &ha_no_switches;
}


/* ============================================================
 * REMEMBER LAST SELECTED PLAYER
 * ============================================================ */

#define PLAYER_NVS_NAMESPACE "p4lms"
#define PLAYER_NVS_KEY       "player"

static void load_saved_player(void)
{
    nvs_handle_t nvs;
    uint8_t saved_index = 0;

    esp_err_t ret = nvs_open(
        PLAYER_NVS_NAMESPACE,
        NVS_READONLY,
        &nvs
    );

    if (ret != ESP_OK) {
        ESP_LOGI(TAG, "No saved player yet - using the first one");
        current_player_index = 0;
        return;
    }

    ret = nvs_get_u8(nvs, PLAYER_NVS_KEY, &saved_index);
    nvs_close(nvs);

    if (ret == ESP_OK && saved_index < LYRION_PLAYER_COUNT) {
        current_player_index = (int)saved_index;
        ESP_LOGI(TAG, "Restored last selected player: %s",
                 lyrion_players[current_player_index].name);
    } else {
        current_player_index = 0;
        ESP_LOGI(TAG, "No valid saved player - using the first one");
    }
}

static void save_selected_player(int index)
{
    if (index < 0 || index >= (int)LYRION_PLAYER_COUNT) {
        return;
    }

    nvs_handle_t nvs;
    esp_err_t ret = nvs_open(
        PLAYER_NVS_NAMESPACE,
        NVS_READWRITE,
        &nvs
    );

    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Could not open NVS to save player: %s",
                 esp_err_to_name(ret));
        return;
    }

    ret = nvs_set_u8(nvs, PLAYER_NVS_KEY, (uint8_t)index);
    if (ret == ESP_OK) {
        ret = nvs_commit(nvs);
    }

    nvs_close(nvs);

    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Could not save selected player: %s",
                 esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "Saved selected player: %s",
                 lyrion_players[index].name);
    }
}


