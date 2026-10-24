/*
 * P4-LMS - Web settings page
 *
 * Part of main.c.  This file is #included by main.c (in order) and is not
 * compiled on its own, so it shares main.c's includes and static names.
 *
 * Open http://<display IP> in a browser on the same network to set:
 *   - the Home Assistant address and access token,
 *   - each player's amp / pre-amp switch names (stored by player MAC, so
 *     every location keeps its own),
 *   - screensaver timing and brightness.
 * Everything is saved on the board and applies straight away.
 *
 * The page has no password: anyone on your home network could open it.  The
 * token itself is never sent back to the browser - the page only shows that
 * one is set and its last four characters.
 */
#include "esp_http_server.h"
#include "esp_ota_ops.h"
#include "esp_app_desc.h"
#include "esp_chip_info.h"
#include <stdarg.h>

#define WEB_NVS_HA_BASE  "ha_base"
#define WEB_NVS_HA_TOKEN "ha_token"
#define WEB_NVS_HA_MAP   "ha_map"
#define WEB_NVS_HA_MAPN  "ha_mapn"
#define WEB_NVS_SS_TO    "ss_to"
#define WEB_NVS_SS_DD    "ss_dd"
#define WEB_NVS_BL_SB    "bl_sb"
#define WEB_NVS_BL_DP    "bl_dp"
#define WEB_NVS_SS_PM    "ss_pm"
#define WEB_NVS_SS_PS    "ss_ps"
#define WEB_NVS_BL_ART   "bl_art"
#define WEB_NVS_NP_DIM   "np_dim"
#define WEB_NVS_SS_NET   "ss_net"
#define WEB_NVS_ANIM     "anim"
#define WEB_NVS_TZ       "tz"
#define WEB_NVS_CLK_FONT "clk_font"
#define WEB_NVS_CLK_SIZE "clk_size"
#define WEB_NVS_CLK_FMT  "clk_fmt"
#define WEB_NVS_CLK_DATE "clk_date"
#define WEB_NVS_CLK_COL  "clk_col"

/* Copy text into a fixed-size field, cutting it off if it's too long. */
static void web_copy(char *dst, size_t dst_size, const char *src)
{
    size_t i = 0;
    if (dst_size == 0) return;
    for (; src != NULL && src[i] && i + 1 < dst_size; i++) dst[i] = src[i];
    dst[i] = '\0';
}

/* ---------------------------------------------------------------- storage */
static void web_settings_load(void)
{
    ha_map_load_defaults();
    weather_load();

    nvs_handle_t nvs;
    if (nvs_open(PLAYER_NVS_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) return;

    size_t len = sizeof(ha_base);
    char tmp[sizeof(ha_token)];
    if (nvs_get_str(nvs, WEB_NVS_HA_BASE, tmp, &len) == ESP_OK && tmp[0]) {
        web_copy(ha_base, sizeof(ha_base), tmp);
    }
    len = sizeof(tmp);
    if (nvs_get_str(nvs, WEB_NVS_HA_TOKEN, tmp, &len) == ESP_OK) {
        web_copy(ha_token, sizeof(ha_token), tmp);
    }

    /* The switch list.  Saves from before the amp/pre-amp sequence settings
     * were added are a little shorter per entry: those are converted, with
     * the usual order and the default delay. */
    uint8_t n = 0;
    size_t blob = sizeof(ha_map);
    uint8_t *raw = heap_caps_malloc(sizeof(ha_map), MALLOC_CAP_SPIRAM);
    bool loaded = false;
    if (raw != NULL && nvs_get_u8(nvs, WEB_NVS_HA_MAPN, &n) == ESP_OK && n <= HA_MAP_MAX &&
        nvs_get_blob(nvs, WEB_NVS_HA_MAP, raw, &blob) == ESP_OK) {
        if (blob == sizeof(ha_map_entry_t) * n) {
            memcpy(ha_map, raw, blob);
            loaded = true;
        } else if (blob == sizeof(ha_map_entry_v1_t) * n) {
            const ha_map_entry_v1_t *old = (const ha_map_entry_v1_t *)raw;
            for (int k = 0; k < n; k++) {
                memset(&ha_map[k], 0, sizeof(ha_map[k]));
                memcpy(ha_map[k].id, old[k].id, sizeof(ha_map[k].id));
                memcpy(ha_map[k].amp, old[k].amp, sizeof(ha_map[k].amp));
                memcpy(ha_map[k].pre, old[k].pre, sizeof(ha_map[k].pre));
                ha_map[k].delay_s = HA_SEQ_DEFAULT_DELAY_S;
                ha_map[k].amp_first = 0;
            }
            loaded = true;
        }
    }
    if (raw != NULL) heap_caps_free(raw);
    if (loaded) {
        ha_map_count = n;
        ha_map_rebuild_views();
    } else {
        ha_map_load_defaults();          /* nothing saved (or unreadable): defaults */
    }

    uint8_t v;
    if (nvs_get_u8(nvs, WEB_NVS_SS_TO, &v) == ESP_OK && v >= 1) ss_timeout_min = v;
    if (nvs_get_u8(nvs, WEB_NVS_SS_DD, &v) == ESP_OK && v >= 1) ss_deep_min = v;
    if (nvs_get_u8(nvs, WEB_NVS_BL_SB, &v) == ESP_OK && v >= 1 && v <= 100) bl_standby_pct = v;
    if (nvs_get_u8(nvs, WEB_NVS_BL_DP, &v) == ESP_OK && v >= 1 && v <= 100) bl_deep_pct = v;
    if (nvs_get_u8(nvs, WEB_NVS_SS_PM, &v) == ESP_OK && v <= 4) ss_play_mode = v;
    if (nvs_get_u8(nvs, WEB_NVS_SS_PS, &v) == ESP_OK && v <= 4) ss_pause_mode = v;
    if (nvs_get_u8(nvs, WEB_NVS_BL_ART, &v) == ESP_OK && v >= 1 && v <= 100) bl_art_pct = v;
    if (nvs_get_u8(nvs, WEB_NVS_NP_DIM, &v) == ESP_OK && v <= 120) np_dim_min = v;
    if (nvs_get_u8(nvs, WEB_NVS_SS_NET, &v) == ESP_OK && v <= 24) ss_net_hours = v;
    if (nvs_get_u8(nvs, WEB_NVS_ANIM, &v) == ESP_OK) ui_anim_on = (v != 0);
    if (nvs_get_u8(nvs, WEB_NVS_TZ, &v) == ESP_OK && v < TZ_CHOICES) { tz_index = v; time_apply_timezone(); }
    if (nvs_get_u8(nvs, WEB_NVS_CLK_FONT, &v) == ESP_OK && v < SS_CLK_FONTS) ss_clk_font = v;
    if (nvs_get_u8(nvs, WEB_NVS_CLK_SIZE, &v) == ESP_OK && v < SS_CLK_SIZES) ss_clk_size = v;
    if (nvs_get_u8(nvs, WEB_NVS_CLK_FMT,  &v) == ESP_OK && v < SS_CLK_FMTS)  ss_clk_fmt  = v;
    if (nvs_get_u8(nvs, WEB_NVS_CLK_DATE, &v) == ESP_OK && v < SS_CLK_DATES) ss_clk_date = v;
    if (nvs_get_u8(nvs, WEB_NVS_CLK_COL,  &v) == ESP_OK && v < SS_CLK_COLS)  ss_clk_col  = v;
    nvs_close(nvs);

    ESP_LOGI(TAG, "Settings: Home Assistant %s (token %s), %d switch set(s), standby %u min / deep %u min",
             ha_base, ha_token[0] ? "set" : "not set", ha_map_count,
             (unsigned)ss_timeout_min, (unsigned)ss_deep_min);
}

static bool web_settings_save(void)
{
    nvs_handle_t nvs;
    if (nvs_open(PLAYER_NVS_NAMESPACE, NVS_READWRITE, &nvs) != ESP_OK) return false;
    bool ok = nvs_set_str(nvs, WEB_NVS_HA_BASE, ha_base) == ESP_OK &&
              nvs_set_str(nvs, WEB_NVS_HA_TOKEN, ha_token) == ESP_OK &&
              nvs_set_blob(nvs, WEB_NVS_HA_MAP, ha_map, sizeof(ha_map_entry_t) * ha_map_count) == ESP_OK &&
              nvs_set_u8(nvs, WEB_NVS_HA_MAPN, (uint8_t)ha_map_count) == ESP_OK &&
              nvs_set_u8(nvs, WEB_NVS_SS_TO, (uint8_t)ss_timeout_min) == ESP_OK &&
              nvs_set_u8(nvs, WEB_NVS_SS_DD, (uint8_t)ss_deep_min) == ESP_OK &&
              nvs_set_u8(nvs, WEB_NVS_BL_SB, (uint8_t)bl_standby_pct) == ESP_OK &&
              nvs_set_u8(nvs, WEB_NVS_BL_DP, (uint8_t)bl_deep_pct) == ESP_OK &&
              nvs_set_u8(nvs, WEB_NVS_SS_PM, (uint8_t)ss_play_mode) == ESP_OK &&
              nvs_set_u8(nvs, WEB_NVS_SS_PS, (uint8_t)ss_pause_mode) == ESP_OK &&
              nvs_set_u8(nvs, WEB_NVS_BL_ART, (uint8_t)bl_art_pct) == ESP_OK &&
              nvs_set_u8(nvs, WEB_NVS_NP_DIM, (uint8_t)np_dim_min) == ESP_OK &&
              nvs_set_u8(nvs, WEB_NVS_SS_NET, (uint8_t)ss_net_hours) == ESP_OK &&
              nvs_set_u8(nvs, WEB_NVS_ANIM, ui_anim_on ? 1 : 0) == ESP_OK &&
              nvs_set_u8(nvs, WEB_NVS_TZ, (uint8_t)tz_index) == ESP_OK &&
              nvs_set_u8(nvs, WEB_NVS_CLK_FONT, (uint8_t)ss_clk_font) == ESP_OK &&
              nvs_set_u8(nvs, WEB_NVS_CLK_SIZE, (uint8_t)ss_clk_size) == ESP_OK &&
              nvs_set_u8(nvs, WEB_NVS_CLK_FMT,  (uint8_t)ss_clk_fmt)  == ESP_OK &&
              nvs_set_u8(nvs, WEB_NVS_CLK_DATE, (uint8_t)ss_clk_date) == ESP_OK &&
              nvs_set_u8(nvs, WEB_NVS_CLK_COL,  (uint8_t)ss_clk_col)  == ESP_OK;
    if (ok) ok = nvs_commit(nvs) == ESP_OK;
    nvs_close(nvs);
    return ok;
}

/* Switch changes from the web page are made here first, then swapped in all
 * at once (ha_map_edit_commit), so the command task never sees a half-made
 * entry. */
static ha_map_entry_t *ha_map_edit = NULL;          /* HA_MAP_MAX entries, in PSRAM */
static int             ha_map_edit_count = 0;

static void ha_map_edit_begin(void)
{
    if (ha_map_edit == NULL) {
        ha_map_edit = heap_caps_calloc(HA_MAP_MAX, sizeof(ha_map_entry_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    if (ha_map_edit == NULL) return;
    memcpy(ha_map_edit, ha_map, sizeof(ha_map));
    ha_map_edit_count = ha_map_count;
}

static void ha_map_edit_commit(void)
{
    if (ha_map_edit == NULL) return;                    /* no memory: switches left as they were */
    /* unused slots are blank, so they never match a room */
    memset(&ha_map_edit[ha_map_edit_count], 0, sizeof(ha_map_edit[0]) * (HA_MAP_MAX - ha_map_edit_count));
    taskENTER_CRITICAL(&ha_cfg_mux);
    memcpy(ha_map, ha_map_edit, sizeof(ha_map));
    ha_map_rebuild_views();
    ha_map_count = ha_map_edit_count;
    taskEXIT_CRITICAL(&ha_cfg_mux);
}

/* Set (or clear, with both names empty) the switch names for one player. */
static void ha_map_set(const char *id, const char *amp, const char *pre, int delay_s, bool amp_first)
{
    if (ha_map_edit == NULL) return;
    int k;
    for (k = 0; k < ha_map_edit_count; k++) {
        if (strcasecmp(ha_map_edit[k].id, id) == 0) break;
    }
    if (!amp[0] && !pre[0]) {                                  /* remove */
        if (k < ha_map_edit_count) {
            memmove(&ha_map_edit[k], &ha_map_edit[k + 1], sizeof(ha_map_edit[0]) * (ha_map_edit_count - k - 1));
            ha_map_edit_count--;
        }
        return;
    }
    if (k == ha_map_edit_count) {
        if (ha_map_edit_count >= HA_MAP_MAX) {                 /* full: drop the oldest */
            memmove(&ha_map_edit[0], &ha_map_edit[1], sizeof(ha_map_edit[0]) * (HA_MAP_MAX - 1));
            k = HA_MAP_MAX - 1;
        } else {
            k = ha_map_edit_count++;
        }
    }
    memset(&ha_map_edit[k], 0, sizeof(ha_map_edit[k]));
    web_copy(ha_map_edit[k].id, sizeof(ha_map_edit[k].id), id);
    web_copy(ha_map_edit[k].amp, sizeof(ha_map_edit[k].amp), amp);
    web_copy(ha_map_edit[k].pre, sizeof(ha_map_edit[k].pre), pre);
    ha_map_edit[k].delay_s = (uint8_t)delay_s;
    ha_map_edit[k].amp_first = amp_first ? 1 : 0;
}

/* Form field name for a room's position: "rp" + its MAC without colons, so a
 * position always belongs to the same room even if the list order changed. */
static void web_room_field(char *out, size_t n, const char *mac)
{
    size_t o = (size_t)snprintf(out, n, "rp");
    for (const char *c = mac; *c && o + 1 < n; c++) {
        if (isxdigit((unsigned char)*c)) out[o++] = (char)tolower((unsigned char)*c);
    }
    out[o] = '\0';
}

/* Tick-box name for "show this room": "rk" + its MAC without colons. */
static void web_room_pick_field(char *out, size_t n, const char *mac)
{
    web_room_field(out, n, mac);
    if (n > 1 && out[0]) out[1] = 'k';
}

/* Every room the page can offer: this display's rooms first (in their
 * order), then the server's other players, connected ones first.  Only the
 * web server task uses these, one request at a time. */
#define WEB_MAX_ROOMS 32
static lyrion_player_t web_rooms[WEB_MAX_ROOMS];
static uint8_t web_rooms_online[WEB_MAX_ROOMS];

/* Returns how many rooms, or -1 when the server could not be asked (the
 * page then leaves the room choice out rather than guess). */
static int web_rooms_load(void)
{
    static lyrion_player_t all[WEB_MAX_ROOMS];
    static uint8_t on[WEB_MAX_ROOMS];
    if (lms_active < 0 || !lyrion_server_base[0]) return -1;
    const int na = lms_fetch_players_ex(lyrion_server_base, all, WEB_MAX_ROOMS, on, 4000);
    if (na < 0) return -1;
    int n = 0;
    for (int i = 0; i < lyrion_player_count && n < WEB_MAX_ROOMS; i++) {
        web_rooms[n] = lyrion_players[i];
        web_rooms_online[n] = 0;
        for (int k = 0; k < na; k++) {
            if (strcasecmp(all[k].id, lyrion_players[i].id) == 0) {
                web_rooms_online[n] = on[k];
                break;
            }
        }
        n++;
    }
    for (int k = 0; k < na && n < WEB_MAX_ROOMS; k++) {
        bool mine = false;
        for (int i = 0; i < lyrion_player_count; i++) {
            if (strcasecmp(all[k].id, lyrion_players[i].id) == 0) { mine = true; break; }
        }
        if (mine) continue;
        web_rooms[n] = all[k];
        web_rooms_online[n] = on[k];
        n++;
    }
    return n;
}

/* ------------------------------------------------------------ page helpers */
typedef struct { char *buf; size_t len, cap; } web_out_t;

static void web_add(web_out_t *o, const char *fmt, ...)
{
    if (o->buf == NULL || o->len + 1 >= o->cap) return;
    va_list ap;
    va_start(ap, fmt);
    const int n = vsnprintf(o->buf + o->len, o->cap - o->len, fmt, ap);
    va_end(ap);
    if (n > 0) o->len += ((size_t)n < o->cap - o->len) ? (size_t)n : o->cap - o->len - 1;
}

/* Text safe to place inside HTML or an attribute value. */
static void web_add_escaped(web_out_t *o, const char *s)
{
    for (; s && *s; s++) {
        switch (*s) {
            case '&':  web_add(o, "&amp;");  break;
            case '<':  web_add(o, "&lt;");   break;
            case '>':  web_add(o, "&gt;");   break;
            case '"':  web_add(o, "&quot;"); break;
            case '\'': web_add(o, "&#39;");  break;
            default:   web_add(o, "%c", *s); break;
        }
    }
}

static void web_css_color(char *out, size_t n, uint32_t c) { snprintf(out, n, "#%06X", (unsigned)(c & 0xFFFFFF)); }

static void web_my_ip(char *out, size_t n)
{
    esp_netif_ip_info_t ip;
    memset(&ip, 0, sizeof(ip));
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif != NULL && esp_netif_get_ip_info(netif, &ip) == ESP_OK && ip.ip.addr != 0) {
        snprintf(out, n, IPSTR, IP2STR(&ip.ip));
    } else {
        snprintf(out, n, "?");
    }
}

static const ha_map_entry_t *ha_map_find(const char *id)
{
    for (int k = 0; k < ha_map_count; k++) {
        if (strcasecmp(ha_map[k].id, id) == 0) return &ha_map[k];
    }
    return NULL;
}

/* ------------------------------------------------------- firmware update */
/* Page section.  Sent with "%s" so the % signs in the script are left alone. */
static const char web_update_section[] =
    "<section style='margin-top:14px'><h2>Firmware update</h2>"
    /* Two kinds of user: most download the update file from GitHub Releases,
     * a few build it themselves - so name the file for both. */
    "<div class='small'>Choose the update file. From the GitHub Releases page: "
    "<b>P4-LMS-update-vX.Y.bin</b> (or <b>P4-LMS-update-vX.Y-chip-v3.bin</b> for chip v3 boards). "
    "If you build it yourself: <b>build\\P4-LMS.bin</b> (<b>build-v3\\P4-LMS.bin</b> for chip v3). "
    "The display checks the file, installs it and restarts. If the new version fails to start "
    "properly it goes back to this one by itself.</div>"
    "<label>Update file</label><input type='file' id='fw' accept='.bin'>"
    "<div style='margin-top:10px'><button type='button' onclick='fwUp()'>Install</button></div>"
    "<div style='margin-top:10px;height:10px;border-radius:5px;background:rgba(127,127,127,.25)'>"
    "<div id='fwbar' style='height:10px;width:0;border-radius:5px;background:currentColor'></div></div>"
    "<div id='fwmsg' class='small' style='margin-top:6px'></div></section>"
    "<script>function fwUp(){var f=document.getElementById('fw').files[0],"
    "b=document.getElementById('fwbar'),m=document.getElementById('fwmsg');"
    "if(!f){m.textContent='Choose the update file first.';return;}"
    "if(!confirm('Install '+f.name+' ('+Math.round(f.size/1024)+' KB) and restart the display?'))return;"
    "var x=new XMLHttpRequest();x.open('POST','/update');"
    "x.upload.onprogress=function(e){if(e.lengthComputable){var p=Math.round(e.loaded*100/e.total);"
    "b.style.width=p+'%';m.textContent='Sending '+p+'%...';}};"
    "x.onload=function(){m.textContent=x.responseText;"
    "if(x.status==200){setTimeout(function(){location.href='/';},25000);}};"
    "x.onerror=function(){m.textContent='The upload was interrupted - nothing was changed. Try again.';};"
    "x.send(f);}</script>";

static const char web_backup_section[] =
    "<section style='margin-top:14px'><h2>Settings backup</h2>"
    "<div class='small'>Saves every setting on this display to a file: theme, screensaver, weather, "
    "power switches, Wi-Fi networks and Lyrion servers. Settings survive normal firmware updates; "
    "the backup is for a full erase, a replacement board or a second display. <b>The file contains "
    "your Wi-Fi passwords and Home Assistant token - keep it private.</b></div>"
    "<div style='margin-top:10px'><a href='/backup' download='p4lms-settings.json'>"
    "<button type='button'>Download backup</button></a></div>"
    "<label>Restore from a backup file</label><input type='file' id='bk' accept='.json'>"
    "<div style='margin-top:10px'><button type='button' class='plain' onclick='bkUp()'>Restore</button></div>"
    "<div id='bkmsg' class='small' style='margin-top:6px'></div></section>"
    "<script>function bkUp(){var f=document.getElementById('bk').files[0],"
    "m=document.getElementById('bkmsg');"
    "if(!f){m.textContent='Choose a backup file first.';return;}"
    "if(!confirm('Replace ALL settings on this display with '+f.name+' and restart it?'))return;"
    "var x=new XMLHttpRequest();x.open('POST','/restore');"
    "x.onload=function(){m.textContent=x.responseText;"
    "if(x.status==200){setTimeout(function(){location.href='/';},20000);}};"
    "x.onerror=function(){m.textContent='The upload was interrupted - nothing was changed.';};"
    "m.textContent='Sending...';x.send(f);}</script>";

static volatile bool ota_busy = false;

static esp_err_t web_update_fail(httpd_req_t *req, const char *msg)
{
    ESP_LOGW(TAG, "Firmware update refused: %s", msg);
    httpd_resp_set_status(req, "400 Bad Request");
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_sendstr(req, msg);
}

/* The upload is the raw .bin file.  Nothing is written until the first bytes
 * show it really is a P4-LMS firmware image, and the new slot is only chosen
 * for start-up once the whole image has arrived and passed ESP-IDF's checks. */
static esp_err_t web_post_update(httpd_req_t *req)
{
    if (ota_busy) return web_update_fail(req, "An update is already running.");

    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    if (part == NULL) {
        return web_update_fail(req, "This display has no spare firmware slot yet - flash the full "
                                    "firmware over USB once, then updates work from this page.");
    }
    const int total = req->content_len;
    if (total < 65536 || (uint32_t)total > part->size) {
        return web_update_fail(req, "That file is the wrong size for P4-LMS firmware.");
    }

    uint8_t *buf = heap_caps_malloc(4096, MALLOC_CAP_8BIT);
    if (buf == NULL) return web_update_fail(req, "Not enough memory - try again.");

    ota_busy = true;
    esp_ota_handle_t h = 0;
    bool begun = false;
    int received = 0, have = 0;
    char new_version[33] = "";
    const char *fail = NULL;

    int stalls = 0;
    while (received < total && fail == NULL) {
        const int n = httpd_req_recv(req, (char *)buf + have, 4096 - have);
        /* a slow moment is fine; a sender that has gone quiet (a laptop that
         * slept mid-upload) gives up after about 45 s instead of tying up
         * the web page for good */
        if (n == HTTPD_SOCK_ERR_TIMEOUT && ++stalls < 3) continue;
        if (n <= 0) { fail = "The upload was interrupted - nothing was changed."; break; }
        received += n;
        have += n;
        stalls = 0;

        if (!begun) {
            if (have < 128 && received < total) continue;          /* need the header first */
            /* Image header: magic 0xE9; app description at byte 32 with its
             * own magic, version at byte 48 and project name at byte 80. */
            const uint32_t desc_magic = (uint32_t)buf[32] | ((uint32_t)buf[33] << 8) |
                                        ((uint32_t)buf[34] << 16) | ((uint32_t)buf[35] << 24);
            char project[33];
            memcpy(project, buf + 80, 32);
            project[32] = '\0';
            if (have < 128 || buf[0] != 0xE9 || desc_magic != 0xABCD5432U ||
                strcmp(project, esp_app_get_description()->project_name) != 0) {
                fail = "That file isn't a P4-LMS update file - nothing was changed.";
                break;
            }
            memcpy(new_version, buf + 48, 32);
            new_version[32] = '\0';
            /* Built for the other kind of board?  ESP32-P4 chips v1.x and v3.x
             * need different builds; say so instead of "damaged" later. */
            {
                /* image header: min_chip_rev_full at byte 15, max_chip_rev_full at 17 */
                const unsigned min_rev = (unsigned)buf[15] | ((unsigned)buf[16] << 8);
                const unsigned max_rev = (unsigned)buf[17] | ((unsigned)buf[18] << 8);
                esp_chip_info_t ci;
                esp_chip_info(&ci);
                const unsigned chip = ci.revision;                 /* e.g. 302 = v3.2 */
                if (chip < min_rev || (max_rev != 0 && max_rev != 0xFFFF && chip > max_rev)) {
                    static char rev_msg[260];
                    snprintf(rev_msg, sizeof(rev_msg),
                             "That file was built for the other board: this display has chip v%u.%u. "
                             "Use %s - nothing was changed.", chip / 100, chip % 100,
                             chip >= 300 ? "the chip-v3 update file (P4-LMS-update-vX.Y-chip-v3.bin, or build-v3\\P4-LMS.bin)"
                                         : "the normal update file (P4-LMS-update-vX.Y.bin, or build\\P4-LMS.bin)");
                    fail = rev_msg;
                    break;
                }
            }
            if (esp_ota_begin(part, (size_t)total, &h) != ESP_OK) {
                fail = "Could not prepare the spare slot - nothing was changed.";
                break;
            }
            begun = true;
            ESP_LOGI(TAG, "Firmware update: receiving %s (%d KB) into %s", new_version, total / 1024, part->label);
        }
        if (esp_ota_write(h, buf, (size_t)have) != ESP_OK) {
            fail = "Writing the new firmware failed - the current version is untouched.";
            break;
        }
        have = 0;
    }
    heap_caps_free(buf);

    if (fail == NULL && begun) {
        const esp_err_t err = esp_ota_end(h);
        begun = false;
        if (err == ESP_ERR_OTA_VALIDATE_FAILED) {
            fail = "The file is damaged or incomplete - the current version is untouched.";
        } else if (err != ESP_OK || esp_ota_set_boot_partition(part) != ESP_OK) {
            fail = "Could not finish installing - the current version is untouched.";
        }
    }
    if (begun) esp_ota_abort(h);
    ota_busy = false;
    if (fail != NULL) return web_update_fail(req, fail);

    char msg[160];
    snprintf(msg, sizeof(msg), "Installed %s. Restarting now - this page reloads in about 25 seconds.",
             new_version);
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, msg);
    ESP_LOGI(TAG, "Firmware update: %s installed in %s", new_version, part->label);
    lms_request_restart("new firmware installed");
    return ESP_OK;
}

/* A freshly installed version is only kept if it runs properly: once it has
 * been up for 30 s with Wi-Fi connected it confirms itself.  If it crashes
 * before that, the bootloader goes back to the previous version (this needs
 * "Enable app rollback support" in menuconfig; without it nothing happens). */
static esp_timer_handle_t ota_confirm_timer = NULL;

static void ota_confirm_cb(void *arg)
{
    (void)arg;
    const esp_partition_t *run = esp_ota_get_running_partition();
    esp_ota_img_states_t st = ESP_OTA_IMG_UNDEFINED;
    if (run == NULL || esp_ota_get_state_partition(run, &st) != ESP_OK ||
        st != ESP_OTA_IMG_PENDING_VERIFY) {
        return;                                     /* nothing waiting to be confirmed */
    }
    if (xEventGroupGetBits(wifi_event_group) & WIFI_CONNECTED_BIT) {
        esp_ota_mark_app_valid_cancel_rollback();
        ESP_LOGI(TAG, "Firmware update: this version runs properly - keeping it");
    } else {
        esp_timer_start_once(ota_confirm_timer, 15ULL * 1000000ULL);     /* check again */
    }
}

/* ------------------------------------------- Home Assistant switch list */
/* Every time the settings page is built, ask Home Assistant once for the
 * names of everything that can be switched on and off (switches, lights and
 * "input_boolean" helpers).  The answer does two jobs:
 *   - the browser offers those names while you type an amp / pre-amp switch
 *     (an HTML <datalist>; typing any other name still works), and
 *   - whether it worked tells you if the address and token are right, shown
 *     under the token box.  The "Test" button just reloads the page, which
 *     runs this check again.
 * One small "template" request does it: Home Assistant fills in the template
 * and sends back plain text, "switch.a,light.b,...", instead of the full
 * (very large) state of every device.  If Home Assistant can't be reached,
 * the page takes a few seconds longer to appear (the request's timeout). */
#define WEB_HA_LIST_MAX   (16 * 1024)    /* reply buffer, in PSRAM */
#define WEB_HA_ENTS_MAX   400            /* more than this are left out */

/* Switches only: the power code turns things on and off with Home
 * Assistant's "switch" service, which ignores lights and other kinds. */
static const char web_ha_template_body[] =
    "{\"template\":\"{{ states.switch | map(attribute='entity_id') | join(',') }}\"}";

typedef struct {
    char *list;          /* the reply, a comma-separated list (PSRAM); may be NULL */
    int   count;         /* how many names */
    int   status;        /* HTTP status from Home Assistant, 0 = no answer */
    bool  tried;         /* false: no address or token yet */
    bool  ok;            /* connected and the token accepted */
    bool  no_list;       /* connected, but the token may not read the list (not an admin user) */
} web_ha_check_t;

/* The last check, kept for 10 minutes so the page doesn't wait on Home
 * Assistant every time it's opened; TEST (/?test) or a new address or token
 * checks again straight away. */
static web_ha_check_t web_ha_last;
static int64_t        web_ha_last_ms = 0;
static uint32_t       web_ha_last_key = 0;      /* which address + token it was for */

/* Skips the separators between names (commas, spaces, line ends, quotes). */
static const char *web_ha_skip_sep(const char *p)
{
    while (*p == ',' || *p == ' ' || *p == '\n' || *p == '\r' || *p == '"') p++;
    return p;
}

/* Runs on the web server's task.  ha_request() only uses its own local
 * variables plus a locked copy of the address and token, so it is safe here
 * as well as on the command task; this check brings its own reply buffer. */
static void web_ha_check(web_ha_check_t *c, bool force)
{
    char base[sizeof(ha_base)];
    uint32_t key = 2166136261u;                       /* a fingerprint of address + token */
    bool token_set;
    taskENTER_CRITICAL(&ha_cfg_mux);
    memcpy(base, ha_base, sizeof(base));
    token_set = HA_TOKEN_SET;
    for (const char *k = ha_base; *k; k++)  key = (key ^ (uint8_t)*k) * 16777619u;
    for (const char *k = ha_token; *k; k++) key = (key ^ (uint8_t)*k) * 16777619u;
    taskEXIT_CRITICAL(&ha_cfg_mux);
    base[sizeof(base) - 1] = '\0';

    const int64_t now = esp_timer_get_time() / 1000;
    if (!force && web_ha_last_ms != 0 && key == web_ha_last_key && now - web_ha_last_ms < 10 * 60000) {
        *c = web_ha_last;                             /* recent enough */
        return;
    }

    /* the list buffer is kept between checks (one 16 KB block in PSRAM) */
    char *list = web_ha_last.list;
    memset(c, 0, sizeof(*c));
    if (base[0] == '\0' || !token_set) {
        c->list = list;
        if (list) list[0] = '\0';
        web_ha_last = *c;
        web_ha_last_ms = 0;                           /* nothing to keep */
        return;
    }
    c->tried = true;
    if (list == NULL) list = heap_caps_malloc(WEB_HA_LIST_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    c->list = list;
    if (list == NULL) return;
    list[0] = '\0';

    /* 1. Address and token: /api/ answers any user's token. */
    char small[128];
    c->ok = ha_request("/api/", NULL, small, sizeof(small), &c->status) && c->status == 200;
    if (!c->ok) {
        web_ha_last = *c;
        web_ha_last_ms = now;
        web_ha_last_key = key;
        return;
    }
    /* 2. The switch names (a POST, so it tries even while background checks
     *    pause).  Home Assistant only fills in templates for admin users. */
    int st = 0;
    if (!ha_request("/api/template", web_ha_template_body, list, WEB_HA_LIST_MAX, &st) || st != 200) {
        list[0] = '\0';
        c->no_list = true;
    } else {
        /* A reply that filled the whole buffer was cut short: drop the last,
         * probably half, name. */
        if (strlen(list) >= WEB_HA_LIST_MAX - 1) {
            char *last = strrchr(list, ',');
            if (last != NULL) *last = '\0';
        }
        for (const char *p = web_ha_skip_sep(list); *p && c->count < WEB_HA_ENTS_MAX; p = web_ha_skip_sep(p)) {
            c->count++;
            while (*p && *p != ',') p++;
        }
    }
    web_ha_last = *c;
    web_ha_last_ms = now;
    web_ha_last_key = key;
}

/* The <datalist> the amp / pre-amp boxes point at (list='haents'). */
static void web_ha_datalist(web_out_t *o, const web_ha_check_t *c)
{
    web_add(o, "<datalist id='haents'>");
    if (c->ok && c->list != NULL) {
        int n = 0;
        for (const char *p = web_ha_skip_sep(c->list); *p && n < WEB_HA_ENTS_MAX; p = web_ha_skip_sep(p)) {
            const char *end = p;
            while (*end && *end != ',') end++;
            size_t l = (size_t)(end - p);
            while (l > 0 && (p[l - 1] == ' ' || p[l - 1] == '\n' || p[l - 1] == '\r' || p[l - 1] == '"')) l--;
            char name[96];
            if (l > 0 && l < sizeof(name)) {          /* longer ones can't be real entity names */
                memcpy(name, p, l);
                name[l] = '\0';
                web_add(o, "<option value='");
                web_add_escaped(o, name);             /* HTML-safe */
                web_add(o, "'>");
                n++;
            }
            p = end;
        }
    }
    web_add(o, "</datalist>");
}

/* The status line under the token box, plus the Test button. */
static void web_ha_status(web_out_t *o, const web_ha_check_t *c, const char *ok_color)
{
    web_add(o, "<div class='row' style='align-items:center;margin-top:10px'><div style='color:%s'>",
            (c->tried && c->ok) ? ok_color : "#E0453A");
    if (!c->tried) {
        web_add(o, "Enter the address and token, then Save");
    } else if (c->ok && c->no_list) {
        web_add(o, "Connected to Home Assistant &#10003; (the switch list needs an admin user's token - "
                   "type the switch names instead)");
    } else if (c->ok) {
        web_add(o, "Connected to Home Assistant &#10003; (%d switch%s found)", c->count,
                c->count == 1 ? "" : "es");
    } else if (c->status == 401 || c->status == 403) {
        web_add(o, "Home Assistant didn't accept the token (%d)", c->status);
    } else if (c->list == NULL) {
        web_add(o, "Not enough memory to check Home Assistant");
    } else if (c->status == 0) {
        char base[sizeof(ha_base)];
        taskENTER_CRITICAL(&ha_cfg_mux);
        memcpy(base, ha_base, sizeof(base));
        taskEXIT_CRITICAL(&ha_cfg_mux);
        base[sizeof(base) - 1] = '\0';
        web_add(o, "Can't reach Home Assistant at ");
        web_add_escaped(o, base);
    } else {
        web_add(o, "Home Assistant answered with an error (HTTP %d)", c->status);
    }
    /* A plain button (not part of the form): loads the page again, which
     * runs the check again.  Save first to test a new address or token. */
    web_add(o, "</div><div><button type='button' class='plain' onclick=\"location.href='/?test'\">Test</button> "
               "<span class='small'>checks the saved address and token</span></div></div>");
}

/* ------------------------------------------------------------------ pages */
static esp_err_t web_send_page(httpd_req_t *req, const char *notice, bool notice_ok)
{
    /* 64 KB (in PSRAM): the Home Assistant switch list can add ~20 KB. */
    web_out_t o = { .buf = heap_caps_malloc(65536, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT), .len = 0, .cap = 65536 };
    if (o.buf == NULL) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No memory");
    o.buf[0] = '\0';

    /* Home Assistant: connected? and its switch names (kept for 10 minutes;
     * TEST checks again now). */
    web_ha_check_t hac;
    web_ha_check(&hac, strstr(req->uri, "test") != NULL);

    char bg[8], panel[8], text[8], soft[8], accent[8], edge[8], onacc[8];
    web_css_color(bg, sizeof(bg), UI_BG);
    web_css_color(panel, sizeof(panel), UI_PANEL);
    web_css_color(text, sizeof(text), UI_TEXT);
    web_css_color(soft, sizeof(soft), UI_TEXT_SOFT);
    web_css_color(accent, sizeof(accent), UI_ACCENT);
    web_css_color(edge, sizeof(edge), UI_EDGE);
    web_css_color(onacc, sizeof(onacc), UI_ON_ACCENT);

    web_add(&o,
        "<!doctype html><html><head><meta charset='utf-8'>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<title>P4-LMS settings</title><style>"
        "body{font-family:system-ui,sans-serif;background:%s;color:%s;margin:0;padding:16px}"
        "main{max-width:720px;margin:auto}h1{font-size:1.4em;margin:.2em 0 .6em}"
        "section{background:%s;border:1px solid %s;border-radius:12px;padding:14px 16px;margin:0 0 14px}"
        "h2{font-size:1.05em;margin:0 0 10px;color:%s}h3{font-size:.95em;margin:16px 0 0}"
        "label{display:block;margin:10px 0 4px;color:%s;font-size:.9em}"
        "input[type=text],input[type=password],input[type=number],select{width:100%%;box-sizing:border-box;"
        "padding:9px;border-radius:8px;border:1px solid %s;background:%s;color:%s;font-size:1em}"
        ".row{display:grid;grid-template-columns:1fr 1fr;gap:10px}.small{font-size:.82em;color:%s}"
        "@media(max-width:560px){.row{grid-template-columns:1fr}}"
        "button{background:%s;color:%s;border:0;border-radius:8px;padding:11px 20px;font-size:1em;cursor:pointer}"
        "button.plain{background:transparent;color:%s;border:1px solid %s}"
        ".note{padding:10px 12px;border-radius:8px;margin:0 0 14px;border:1px solid %s}"
        "td{padding:3px 10px 3px 0;vertical-align:top}</style></head><body><main>",
        bg, text, panel, edge, accent, soft, edge, bg, text, soft, accent, onacc, text, edge, edge);
    /* The heading shows which display this is (its network address), handy
     * when there is more than one in the house. */
    {
        char my_ip[20];
        web_my_ip(my_ip, sizeof(my_ip));
        web_add(&o, "<h1>P4-LMS &middot; settings <span class='small'>%s</span></h1>",
                strcmp(my_ip, "?") == 0 ? "" : my_ip);
    }

    if (notice != NULL) {
        web_add(&o, "<div class='note' style='color:%s'>", notice_ok ? accent : "#E0453A");
        web_add_escaped(&o, notice);
        web_add(&o, "</div>");
    }

    /* Sections follow the Settings tiles on the display. */
    web_add(&o, "<form method='post' action='/save'>");

    /* Theme */
    web_add(&o, "<section><h2>Theme</h2><div class='row'><div><label>Colours</label><div>");
    web_add_escaped(&o, ui_theme->name);
    web_add(&o, " <span class='small'>(choose on the display: Settings &rsaquo; Theme)</span></div></div>"
                "<div><label>Song-change fade</label><select name='anim'>"
                "<option value='1'%s>On</option><option value='0'%s>Off</option></select></div></div></section>",
            ui_anim_on ? " selected" : "", ui_anim_on ? "" : " selected");

    /* Visualizer */
    web_add(&o, "<section><h2>Visualizer</h2><div class='row'><div><label>Style</label><div>%s "
                "<span class='small'>(choose on the display)</span></div></div>"
                "<div><label>Mic sensitivity (meters and visualizers)</label><select name='mic_gain'>"
                "<option value='0'%s>Normal</option><option value='1'%s>+6 dB</option>"
                "<option value='2'%s>+12 dB</option><option value='3'%s>+18 dB</option></select></div>"
                "</div></section>",
            vis_style_names[vis_style_selected],
            mic_gain_step == 0 ? " selected" : "", mic_gain_step == 1 ? " selected" : "",
            mic_gain_step == 2 ? " selected" : "", mic_gain_step == 3 ? " selected" : "");

    /* Wi-Fi and Lyrion server: shown here, changed on the display */
    {
        char ip[20];
        web_my_ip(ip, sizeof(ip));
        web_add(&o, "<section><h2>Wi-Fi</h2><div>");
        web_add_escaped(&o, wifi_cur_ssid);
        web_add(&o, " &middot; %s <span class='small'>(change on the display)</span></div></section>", ip);
        web_add(&o, "<section><h2>Lyrion server</h2><div>");
        web_add_escaped(&o, lyrion_server_base);
        web_add(&o, " &middot; %s &middot; ", lms_events_connected ? "live updates" : "checking every few seconds");
        for (int i = 0; i < lyrion_player_count; i++) {
            if (i) web_add(&o, ", ");
            web_add_escaped(&o, lyrion_players[i].name);
        }
        web_add(&o, "</div>");
        /* Which rooms this display shows: a tick box for every player on the
         * server, the current ones ticked, at most LMS_MAX_PLAYERS. */
        const int nr = web_rooms_load();
        if (nr > 1) {
            web_add(&o, "<h3>Rooms on this display</h3><div class='small'>Tick up to %d rooms to show on the "
                        "display. Saving a change restarts the display.</div>"
                        "<input type='hidden' name='rk_on' value='1'><div class='row'>", LMS_MAX_PLAYERS);
            for (int i = 0; i < nr; i++) {
                char fname[24];
                web_room_pick_field(fname, sizeof(fname), web_rooms[i].id);
                web_add(&o, "<label style='margin:6px 0'><input type='checkbox' class='rk' name='%s' value='1'%s "
                            "onchange='rkMax(this)'> ",
                        fname, i < lyrion_player_count ? " checked" : "");
                web_add_escaped(&o, web_rooms[i].name);
                if (!web_rooms_online[i]) web_add(&o, " <span class='small'>(not connected)</span>");
                web_add(&o, "</label>");
            }
            web_add(&o, "</div><script>function rkMax(c){var n=0,b=document.querySelectorAll('.rk');"
                        "for(var i=0;i<b.length;i++)if(b[i].checked)n++;"
                        "if(n>%d){c.checked=false;alert('The display can show up to %d rooms.');}"
                        "if(n==0){c.checked=true;alert('Keep at least one room.');}}</script>",
                    LMS_MAX_PLAYERS, LMS_MAX_PLAYERS);
        }
        /* Room order: a position for each room (saved for this location). */
        if (lyrion_player_count > 1 && lms_active >= 0) {
            web_add(&o, "<h3>Room order</h3><div class='small'>The order rooms appear on the display. "
                        "Saving a new order restarts the display.</div><div class='row'>");
            for (int i = 0; i < lyrion_player_count; i++) {
                web_add(&o, "<div><label>");
                web_add_escaped(&o, lyrion_players[i].name);
                char fname[24];
                web_room_field(fname, sizeof(fname), lyrion_players[i].id);
                web_add(&o, "</label><select name='%s'>", fname);
                for (int k = 1; k <= lyrion_player_count; k++) {
                    web_add(&o, "<option value='%d'%s>%d%s</option>", k, k == i + 1 ? " selected" : "", k,
                            k == 1 ? "st" : k == 2 ? "nd" : k == 3 ? "rd" : "th");
                }
                web_add(&o, "</select></div>");
            }
            web_add(&o, "</div>");
        }
        web_add(&o, "</section>");
    }

    /* Screen: brightness, idle dimming, screensaver choice, then clock and
     * weather.  Each group is its own web_add call so the values handed in
     * after each piece of text line up with that text's %d / %u / %s marks. */
    web_add(&o, "<section><h2>Screen &amp; screensaver</h2>"
                "<h3>Screen brightness</h3><div class='row'>"
                "<div><label>Brightness while playing (%%)</label>"
                "<input type='number' name='bl_art' min='1' max='100' value='%d'></div>"
                "<div><label>Standby brightness (%%)</label>"
                "<input type='number' name='bl_sb' min='1' max='100' value='%d'></div>"
                "<div><label>Deep dim brightness (%%)</label>"
                "<input type='number' name='bl_dp' min='1' max='100' value='%d'></div>"
                "</div>",
            bl_art_pct, bl_standby_pct, bl_deep_pct);

    web_add(&o, "<h3>When the screen is idle</h3><div class='row'>"
                "<div><label>Dim the screen after (minutes without a touch)</label>"
                "<input type='number' name='ss_to' min='1' max='120' value='%u'></div>"
                "<div><label>Dim further after (more minutes)</label>"
                "<input type='number' name='ss_dd' min='1' max='240' value='%u'></div>"
                "<div><label>While music plays, dim after (minutes, 0 = never)</label>"
                "<input type='number' name='np_dim' min='0' max='120' value='%u'></div>"
                "<div><label>Start the screensaver even while music plays after (hours, 0 = never)</label>"
                "<input type='number' name='ss_net' min='0' max='24' value='%u'></div>"
                "</div>",
            (unsigned)ss_timeout_min, (unsigned)ss_deep_min,
            (unsigned)np_dim_min, (unsigned)ss_net_hours);

    web_add(&o, "<h3>Screensaver</h3><div class='row'>"
                "<div><label>While music plays</label><select name='ss_pm'>"
                "<option value='0'%s>Off (stay on Now Playing)</option>"
                "<option value='1'%s>Album art</option>"
                "<option value='2'%s>Cover wall</option>"
                "<option value='3'%s>Big text</option>"
                "<option value='4'%s>VU meters</option></select></div>"
                "<div><label>When paused or stopped</label><select name='ss_ps'>"
                "<option value='0'%s>Clock</option>"
                "<option value='1'%s>Clock + cover</option>"
                "<option value='2'%s>Analog clock</option>"
                "<option value='3'%s>Cover wall</option>"
                "<option value='4'%s>Weather</option></select></div>"
                "</div>",
            ss_play_mode == 0 ? " selected" : "", ss_play_mode == 1 ? " selected" : "",
            ss_play_mode == 2 ? " selected" : "", ss_play_mode == 3 ? " selected" : "",
            ss_play_mode == 4 ? " selected" : "",
            ss_pause_mode == 0 ? " selected" : "", ss_pause_mode == 1 ? " selected" : "",
            ss_pause_mode == 2 ? " selected" : "", ss_pause_mode == 3 ? " selected" : "",
            ss_pause_mode == 4 ? " selected" : "");

    /* clock and weather (for the clock and Weather screensavers) */
    web_add(&o, "<h3>Clock &amp; weather</h3><div class='row'>"
                "<div><label>Time zone (for the clocks)</label><select name='tz'>");
    for (int z = 0; z < TZ_CHOICES; z++) {
        web_add(&o, "<option value='%d'%s>%s</option>", z, z == tz_index ? " selected" : "", tz_choices[z].name);
    }
    web_add(&o, "</select></div><div><label>Weather units</label><select name='wx_units'>"
                "<option value='0'%s>&deg;C, km/h</option><option value='1'%s>&deg;F, mph</option></select></div>"
                "</div>", wx_units ? "" : " selected", wx_units ? " selected" : "");
    web_add(&o, "<label>Weather location (town, or \"town, province\")</label>"
                "<input type='text' name='wx_loc' maxlength='60' placeholder='e.g. Toronto, Ontario' value='");
    web_add_escaped(&o, wx_location);
    web_add(&o, "'><p class='small'>");
    web_add_escaped(&o, wx_status);
    web_add(&o, " &middot; weather from Open-Meteo (free, no account).</p></section>");


    /* Alarms and sleep timer: the amp switching */
    web_add(&o, "<section><h2>Alarms</h2><div class='row'><div><label>Amp on for alarms</label>"
                "<select name='alarm_amp'><option value='1'%s>On - a minute before</option>"
                "<option value='0'%s>Off</option></select></div>"
                "<div class='small' style='align-self:end'>Alarms themselves are set on the display "
                "(Settings &rsaquo; Alarms).</div></div></section>",
            alarm_amp_enabled ? " selected" : "", alarm_amp_enabled ? "" : " selected");
    web_add(&o, "<section><h2>Sleep timer</h2><div class='row'><div><label>Switch the amp off when it ends</label>"
                "<select name='sleep_amp'><option value='1'%s>Yes</option><option value='0'%s>No</option>"
                "</select></div></div></section>",
            sleep_amp_enabled ? " selected" : "", sleep_amp_enabled ? "" : " selected");

    /* Home Assistant */
    web_add(&o, "<section id='ha'><h2>Home Assistant</h2><label>Address</label>"
                "<input type='text' name='ha_base' value='");
    web_add_escaped(&o, ha_base);
    web_add(&o, "' placeholder='http://homeassistant.local:8123'>");
    const size_t tl = strlen(ha_token);
    web_add(&o, "<label>Long-lived access token</label>"
                "<input type='password' name='ha_token' autocomplete='off' placeholder='%s'>",
                tl ? "Set - leave empty to keep it" : "Not set - paste your token here");
    if (tl >= 4) {
        web_add(&o, "<div class='small'>A token is set (ending &hellip;");
        web_add_escaped(&o, ha_token + tl - 4);
        web_add(&o, "). <label style='display:inline'><input type='checkbox' name='ha_clear' value='1'> remove it</label></div>");
    }
    web_add(&o, "<div class='small'>Create one in Home Assistant: your profile &rarr; Security &rarr; "
                "Long-lived access tokens &rarr; Create token.</div>");
    web_ha_status(&o, &hac, accent);
    web_add(&o, "</section>");
    web_ha_datalist(&o, &hac);      /* the names the switch boxes below offer */
    hac.list = NULL;                         /* the buffer stays with the kept result */

    /* switches for this location's players */
    web_add(&o, "<section><h2>Power switches</h2><div class='small'>Home Assistant switch names for each "
                "player here, e.g. <code>switch.office_amplifier</code>. Leave both empty for a player "
                "without switches. Each room's <b>System power</b> switch (on its Rooms card) turns both "
                "on and off in the order and with the gap set here, so the speakers don't thump; the "
                "sleep timer and alarms use the same order. With one switch, order and gap don't apply.</div>");
    for (int i = 0; i < lyrion_player_count; i++) {
        const ha_map_entry_t *e = ha_map_find(lyrion_players[i].id);
        /* Each room gets its own small heading and labelled boxes; the field
         * names (id/amp/pre/ord/dly + number) are what the save step reads. */
        web_add(&o, "<h3>");
        web_add_escaped(&o, lyrion_players[i].name);
        web_add(&o, "</h3><input type='hidden' name='id%d' value='", i);
        web_add_escaped(&o, lyrion_players[i].id);
        web_add(&o, "'><div class='row'><div><label>Amp switch</label>"
                    "<input type='text' name='amp%d' list='haents' autocomplete='off' placeholder='switch.room_amplifier' value='", i);
        web_add_escaped(&o, e ? e->amp : "");
        web_add(&o, "'></div><div><label>Pre-amp switch (optional)</label>"
                    "<input type='text' name='pre%d' list='haents' autocomplete='off' placeholder='switch.room_preamp' value='", i);
        web_add_escaped(&o, e ? e->pre : "");
        web_add(&o, "'></div></div>");
        const int dly = e ? (e->delay_s <= HA_SEQ_MAX_DELAY_S ? e->delay_s : HA_SEQ_DEFAULT_DELAY_S)
                          : HA_SEQ_DEFAULT_DELAY_S;
        const bool af = e ? e->amp_first != 0 : false;
        web_add(&o, "<div class='row'><div><label>Turn-on order</label><select name='ord%d'>"
                    "<option value='0'%s>Pre-amp turns on first (recommended)</option>"
                    "<option value='1'%s>Amp turns on first</option></select></div>"
                    "<div><label>Gap between them</label><select name='dly%d'>",
                i, af ? "" : " selected", af ? " selected" : "", i);
        for (int d = 0; d <= HA_SEQ_MAX_DELAY_S; d++) {
            web_add(&o, "<option value='%d'%s>%d second%s between them</option>",
                    d, d == dly ? " selected" : "", d, d == 1 ? "" : "s");
        }
        web_add(&o, "</select></div></div>");
    }
    web_add(&o, "</section>");

    web_add(&o, "<button type='submit'>Save</button></form>");

    /* About: status, restart, firmware update */
    {
        char ip[20];
        web_my_ip(ip, sizeof(ip));
    web_add(&o, "<section><h2>About</h2><table>");
    web_add(&o, "<tr><td class='small'>Wi-Fi</td><td>"); web_add_escaped(&o, wifi_cur_ssid);
    web_add(&o, " &middot; %s</td></tr>", ip);
    {
        char mdns_name[MDNS_NAME_BUF_LEN];
        p4_mdns_name_get(mdns_name, sizeof(mdns_name));
        if (mdns_name[0]) {
            web_add(&o, "<tr><td class='small'>Also at</td><td>http://");
            web_add_escaped(&o, mdns_name);
            web_add(&o, ".local/</td></tr>");
        }
    }
    web_add(&o, "<tr><td class='small'>Lyrion server</td><td>"); web_add_escaped(&o, lyrion_server_base);
    web_add(&o, " &middot; %s</td></tr>", lms_events_connected ? "live updates" : "checking every few seconds");
    web_add(&o, "<tr><td class='small'>Players</td><td>");
    for (int i = 0; i < lyrion_player_count; i++) {
        if (i) web_add(&o, ", ");
        web_add_escaped(&o, lyrion_players[i].name);
    }
    {
        const esp_app_desc_t *app = esp_app_get_description();
        const esp_partition_t *run = esp_ota_get_running_partition();
        esp_ota_img_states_t st = ESP_OTA_IMG_UNDEFINED;
        if (run != NULL) esp_ota_get_state_partition(run, &st);
        web_add(&o, "</td></tr><tr><td class='small'>Firmware</td><td>");
        web_add_escaped(&o, app->version);
        web_add(&o, " &middot; built ");
        web_add_escaped(&o, app->date);
        web_add(&o, " ");
        web_add_escaped(&o, app->time);
        web_add(&o, " &middot; slot ");
        web_add_escaped(&o, run ? run->label : "?");
        web_add(&o, "%s</td></tr></table></section>",
                st == ESP_OTA_IMG_PENDING_VERIFY ? " (new - being checked)" : "");
    }

    }
    web_add(&o, "<form method='post' action='/restart' style='margin-top:14px' "
                "onsubmit=\"return confirm('Restart the display?')\">"
                "<button class='plain' type='submit'>Restart display</button></form>");
    web_add(&o, "%s", web_update_section);
    web_add(&o, "%s", web_backup_section);
    web_add(&o, "<p class='small'>This page has no password - anyone on your network can open it. "
                "The token is never shown here.</p></main>"
                /* after the Test button, scroll back to the Home Assistant part */
                "<script>if(location.search=='?test'){var h=document.getElementById('ha');"
                "if(h)h.scrollIntoView();}</script></body></html>");

    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    const esp_err_t err = httpd_resp_send(req, o.buf, o.len);
    heap_caps_free(o.buf);
    return err;
}

static esp_err_t web_get_root(httpd_req_t *req)
{
    char q[16] = "";
    const bool got = httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK;
    const char *notice = NULL;
    if (got && strcmp(q, "saved=1") == 0) notice = "Saved. Changes are active now.";
    if (got && strcmp(q, "saved=2") == 0) notice = "Saved. The display is restarting with the new rooms - "
                                                   "reload this page in about 15 seconds.";
    return web_send_page(req, notice, true);
}

/* In place: '+' -> space, %XX -> byte. */
static void web_url_decode(char *s)
{
    char *w = s;
    for (char *r = s; *r; r++) {
        if (*r == '+') {
            *w++ = ' ';
        } else if (*r == '%' && isxdigit((unsigned char)r[1]) && isxdigit((unsigned char)r[2])) {
            char hex[3] = { r[1], r[2], 0 };
            *w++ = (char)strtol(hex, NULL, 16);
            r += 2;
        } else {
            *w++ = *r;
        }
    }
    *w = '\0';
}

/* Value of `key` from a urlencoded body (decoded, trimmed); "" if absent. */
static void web_field(const char *body, const char *key, char *out, size_t out_size)
{
    out[0] = '\0';
    const size_t kl = strlen(key);
    for (const char *p = body; p && *p; ) {
        const char *amp = strchr(p, '&');
        const size_t seg = amp ? (size_t)(amp - p) : strlen(p);
        if (seg > kl && strncmp(p, key, kl) == 0 && p[kl] == '=') {
            const size_t vl = seg - kl - 1;
            char tmp[400];
            snprintf(tmp, sizeof(tmp), "%.*s", (int)(vl < sizeof(tmp) - 1 ? vl : sizeof(tmp) - 1), p + kl + 1);
            web_url_decode(tmp);
            char *t = tmp;
            while (*t == ' ') t++;
            size_t n = strlen(t);
            while (n > 0 && (t[n - 1] == ' ' || t[n - 1] == '\r' || t[n - 1] == '\n')) t[--n] = '\0';
            snprintf(out, out_size, "%s", t);
            return;
        }
        p = amp ? amp + 1 : NULL;
    }
}

static int web_clamp_int(const char *s, int lo, int hi, int fallback)
{
    if (s == NULL || !s[0]) return fallback;
    const int v = atoi(s);
    return v < lo ? lo : (v > hi ? hi : v);
}

static esp_err_t web_post_save(httpd_req_t *req)
{
    const int total = req->content_len;
    if (total <= 0 || total > 6000) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad form");
    }
    char *body = heap_caps_malloc(total + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (body == NULL) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No memory");
    int got = 0, stalls = 0;
    while (got < total) {
        const int n = httpd_req_recv(req, body + got, total - got);
        /* a slow moment is fine; a sender that has gone quiet (a laptop that
         * slept mid-upload) gives up after about 45 s instead of tying up
         * the web page for good */
        if (n == HTTPD_SOCK_ERR_TIMEOUT && ++stalls < 3) continue;
        if (n <= 0) {
            heap_caps_free(body);
            return ESP_FAIL;
        }
        got += n;
        stalls = 0;
    }
    body[got] = '\0';

    char v[400];
    /* address and token: worked out on copies, swapped in under the lock */
    char new_base[sizeof(ha_base)], new_token[sizeof(ha_token)];
    taskENTER_CRITICAL(&ha_cfg_mux);
    memcpy(new_base, ha_base, sizeof(new_base));
    memcpy(new_token, ha_token, sizeof(new_token));
    taskEXIT_CRITICAL(&ha_cfg_mux);
    web_field(body, "ha_base", v, sizeof(v));
    if (v[0]) {
        if (strncasecmp(v, "http://", 7) != 0 && strncasecmp(v, "https://", 8) != 0) {
            char with[sizeof(ha_base) + 8];
            snprintf(with, sizeof(with), "http://%.80s", v);
            web_copy(new_base, sizeof(new_base), with);
        } else {
            web_copy(new_base, sizeof(new_base), v);
        }
        size_t bl = strlen(new_base);
        while (bl > 0 && new_base[bl - 1] == '/') new_base[--bl] = '\0';
    }
    web_field(body, "ha_clear", v, sizeof(v));
    if (v[0] == '1') {
        new_token[0] = '\0';
    } else {
        web_field(body, "ha_token", v, sizeof(v));
        if (v[0]) web_copy(new_token, sizeof(new_token), v);
    }
    taskENTER_CRITICAL(&ha_cfg_mux);
    memcpy(ha_base, new_base, sizeof(ha_base));
    memcpy(ha_token, new_token, sizeof(ha_token));
    taskEXIT_CRITICAL(&ha_cfg_mux);

    ha_map_edit_begin();

    for (int i = 0; i < LMS_MAX_PLAYERS; i++) {
        char key[8], id[24], amp[72], pre[72];
        snprintf(key, sizeof(key), "id%d", i);
        web_field(body, key, id, sizeof(id));
        if (!id[0]) continue;
        snprintf(key, sizeof(key), "amp%d", i);
        web_field(body, key, amp, sizeof(amp));
        snprintf(key, sizeof(key), "pre%d", i);
        web_field(body, key, pre, sizeof(pre));
        amp[63] = pre[63] = '\0';
        char dv[8], ov[8];
        snprintf(key, sizeof(key), "dly%d", i);
        web_field(body, key, dv, sizeof(dv));
        snprintf(key, sizeof(key), "ord%d", i);
        web_field(body, key, ov, sizeof(ov));
        ha_map_set(id, amp, pre, web_clamp_int(dv, 0, HA_SEQ_MAX_DELAY_S, HA_SEQ_DEFAULT_DELAY_S),
                   web_clamp_int(ov, 0, 1, 0) == 1);
    }
    ha_map_edit_commit();

    web_field(body, "tz", v, sizeof(v));
    if (v[0]) { tz_index = web_clamp_int(v, 0, TZ_CHOICES - 1, tz_index); time_apply_timezone(); }
    web_field(body, "ss_to", v, sizeof(v));  ss_timeout_min = (uint32_t)web_clamp_int(v, 1, 120, (int)ss_timeout_min);
    web_field(body, "ss_dd", v, sizeof(v));  ss_deep_min    = (uint32_t)web_clamp_int(v, 1, 240, (int)ss_deep_min);
    web_field(body, "bl_sb", v, sizeof(v));  bl_standby_pct = web_clamp_int(v, 1, 100, bl_standby_pct);
    web_field(body, "bl_dp", v, sizeof(v));  bl_deep_pct    = web_clamp_int(v, 1, 100, bl_deep_pct);
    web_field(body, "ss_pm", v, sizeof(v));  ss_play_mode   = web_clamp_int(v, 0, 4, ss_play_mode);
    web_field(body, "ss_ps", v, sizeof(v));  ss_pause_mode  = web_clamp_int(v, 0, 4, ss_pause_mode);
    web_field(body, "bl_art", v, sizeof(v)); bl_art_pct     = web_clamp_int(v, 1, 100, bl_art_pct);
    web_field(body, "np_dim", v, sizeof(v)); if (v[0]) np_dim_min = (uint32_t)web_clamp_int(v, 0, 120, (int)np_dim_min);
    web_field(body, "ss_net", v, sizeof(v)); if (v[0]) ss_net_hours = (uint32_t)web_clamp_int(v, 0, 24, (int)ss_net_hours);
    web_field(body, "anim", v, sizeof(v));   ui_anim_on     = web_clamp_int(v, 0, 1, ui_anim_on ? 1 : 0) != 0;
    web_field(body, "mic_gain", v, sizeof(v));
    if (v[0]) { mic_gain_step = web_clamp_int(v, 0, 3, mic_gain_step); mic_gain_save(); }
    web_field(body, "alarm_amp", v, sizeof(v));
    if (v[0]) {
        alarm_amp_enabled = web_clamp_int(v, 0, 1, alarm_amp_enabled) != 0;
        alarm_sched_dirty = true;
        alarm_amp_save(alarm_amp_enabled);
    }
    web_field(body, "sleep_amp", v, sizeof(v));
    if (v[0]) {
        sleep_amp_enabled = web_clamp_int(v, 0, 1, sleep_amp_enabled) != 0;
        nvs_handle_t sn;
        if (nvs_open(PLAYER_NVS_NAMESPACE, NVS_READWRITE, &sn) == ESP_OK) {
            nvs_set_u8(sn, SLEEP_NVS_AMP, sleep_amp_enabled ? 1 : 0);
            nvs_commit(sn);
            nvs_close(sn);
        }
    }
    {
        char loc[64];
        web_field(body, "wx_loc", loc, sizeof(loc));
        web_field(body, "wx_units", v, sizeof(v));
        weather_set(loc, web_clamp_int(v, 0, 1, wx_units));
    }
    /* Room order: sort by the chosen positions.  A room moved up goes ahead
     * of the one that kept that number, and a room moved down goes after it,
     * so "Basement to 1st" or "Kitchen to 3rd" work without renumbering the
     * others. */
    int order[LMS_MAX_PLAYERS];
    bool reorder = false;
    const int np = lyrion_player_count;
    if (np > 1 && lms_active >= 0 && lms_active < lms_profile_count) {
        int key[LMS_MAX_PLAYERS];
        for (int i = 0; i < np; i++) {
            char k[24];
            web_room_field(k, sizeof(k), lyrion_players[i].id);
            web_field(body, k, v, sizeof(v));
            const int pos = web_clamp_int(v, 1, np, i + 1);
            key[i] = pos * 64 + (pos < i + 1 ? 0 : pos == i + 1 ? 16 : 32) + i;
            order[i] = i;
        }
        for (int a = 1; a < np; a++) {                 /* insertion sort by key */
            const int t = order[a];
            int b = a - 1;
            while (b >= 0 && key[order[b]] > key[t]) { order[b + 1] = order[b]; b--; }
            order[b + 1] = t;
        }
        for (int i = 0; i < np; i++) if (order[i] != i) reorder = true;
    } else {
        for (int i = 0; i < np && i < LMS_MAX_PLAYERS; i++) order[i] = i;
    }

    /* Which rooms to show: the ticked ones, in the order above for rooms
     * already on the display, then newly ticked rooms in the server's order.
     * Nothing ticked, or a page without the tick boxes, leaves them alone. */
    lyrion_player_t picked[LMS_MAX_PLAYERS];
    int npick = 0;
    bool repick = false;
    web_field(body, "rk_on", v, sizeof(v));
    if (v[0] == '1' && lms_active >= 0 && lms_active < lms_profile_count) {
        const int nr = web_rooms_load();
        bool tick[WEB_MAX_ROOMS] = { false };
        int nt = 0;
        for (int i = 0; i < nr; i++) {
            char k[24];
            web_room_pick_field(k, sizeof(k), web_rooms[i].id);
            web_field(body, k, v, sizeof(v));
            tick[i] = v[0] == '1';
            if (tick[i]) nt++;
        }
        if (nr > 0 && nt > 0) {
            /* web_rooms starts with this display's rooms, so index i < np
             * is lyrion_players[i]. */
            for (int i = 0; i < np && npick < LMS_MAX_PLAYERS; i++) {
                if (order[i] < nr && tick[order[i]]) picked[npick++] = lyrion_players[order[i]];
            }
            for (int i = np; i < nr && npick < LMS_MAX_PLAYERS; i++) {
                if (tick[i]) picked[npick++] = web_rooms[i];
            }
            int kept = 0;
            for (int i = 0; i < np; i++) if (i < nr && tick[i]) kept++;
            repick = npick > 0 && (npick != np || kept != np);
        }
    }
    heap_caps_free(body);

    if (repick) {
        lms_profile_t *pr = &lms_profiles[lms_active];
        int new_sel = 0;
        const char *cur = (current_player_index >= 0 && current_player_index < np)
                              ? lyrion_players[current_player_index].id : "";
        for (int i = 0; i < npick; i++) {
            if (strcasecmp(picked[i].id, cur) == 0) new_sel = i;
        }
        memset(pr->players, 0, sizeof(pr->players));
        memcpy(pr->players, picked, sizeof(lyrion_player_t) * npick);
        pr->count = (uint8_t)npick;
        lms_profiles_save();
        save_selected_player(new_sel);          /* same room if it is still shown */
        ESP_LOGI(TAG, "Rooms changed to %d on the web page - restarting to apply it", npick);
        reorder = true;                         /* same restart and notice below */
    } else if (reorder) {
        lms_profile_t *pr = &lms_profiles[lms_active];
        lyrion_player_t sorted[LMS_MAX_PLAYERS];
        int new_sel = 0;
        for (int i = 0; i < np; i++) {
            sorted[i] = lyrion_players[order[i]];
            if (order[i] == current_player_index) new_sel = i;
        }
        memcpy(pr->players, sorted, sizeof(lyrion_player_t) * np);
        lms_profiles_save();
        save_selected_player(new_sel);          /* stay on the same room */
        ESP_LOGI(TAG, "Room order changed - restarting to apply it");
    }

    if (!web_settings_save()) {
        return web_send_page(req, "Could not save to the board's storage - try again.", false);
    }
    ESP_LOGI(TAG, "Settings saved from the web page");
    if (reorder) {
        lms_request_restart("rooms changed on the web page");
    } else {
        ui_toast_post("Settings saved from the web page");
    }
    /* Back to the page, so a browser refresh doesn't post the form again. */
    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", reorder ? "/?saved=2" : "/?saved=1");
    return httpd_resp_send(req, NULL, 0);
}

/* ------------------------------------------------------- settings backup */
/* Every saved setting (the whole "p4lms" storage area) as a JSON file, and
 * back again.  The file holds the Wi-Fi passwords and the Home Assistant
 * token, so it is for keeping somewhere private. */


#define WEB_BACKUP_MAX 65536

static const char *web_nvs_type_name(nvs_type_t t)
{
    switch (t) {
        case NVS_TYPE_U8:  return "u8";   case NVS_TYPE_I8:  return "i8";
        case NVS_TYPE_U16: return "u16";  case NVS_TYPE_I16: return "i16";
        case NVS_TYPE_U32: return "u32";  case NVS_TYPE_I32: return "i32";
        case NVS_TYPE_U64: return "u64";  case NVS_TYPE_I64: return "i64";
        case NVS_TYPE_STR: return "str";  case NVS_TYPE_BLOB: return "blob";
        default: return NULL;
    }
}

/* One stored value as a JSON item {"k","t","v"}; NULL if it can't be read. */
static cJSON *web_backup_item(nvs_handle_t nvs, const nvs_entry_info_t *info)
{
    const char *tn = web_nvs_type_name(info->type);
    if (tn == NULL) return NULL;
    cJSON *it = cJSON_CreateObject();
    if (it == NULL) return NULL;
    cJSON_AddStringToObject(it, "k", info->key);
    cJSON_AddStringToObject(it, "t", tn);
    esp_err_t e = ESP_FAIL;
    char num[24];
    switch (info->type) {
        case NVS_TYPE_U8:  { uint8_t v;  e = nvs_get_u8(nvs, info->key, &v);  if (e == ESP_OK) cJSON_AddNumberToObject(it, "v", v); break; }
        case NVS_TYPE_I8:  { int8_t v;   e = nvs_get_i8(nvs, info->key, &v);  if (e == ESP_OK) cJSON_AddNumberToObject(it, "v", v); break; }
        case NVS_TYPE_U16: { uint16_t v; e = nvs_get_u16(nvs, info->key, &v); if (e == ESP_OK) cJSON_AddNumberToObject(it, "v", v); break; }
        case NVS_TYPE_I16: { int16_t v;  e = nvs_get_i16(nvs, info->key, &v); if (e == ESP_OK) cJSON_AddNumberToObject(it, "v", v); break; }
        case NVS_TYPE_U32: { uint32_t v; e = nvs_get_u32(nvs, info->key, &v); if (e == ESP_OK) cJSON_AddNumberToObject(it, "v", v); break; }
        case NVS_TYPE_I32: { int32_t v;  e = nvs_get_i32(nvs, info->key, &v); if (e == ESP_OK) cJSON_AddNumberToObject(it, "v", v); break; }
        case NVS_TYPE_U64: { uint64_t v; e = nvs_get_u64(nvs, info->key, &v);
                             if (e == ESP_OK) { snprintf(num, sizeof(num), "%llu", (unsigned long long)v); cJSON_AddStringToObject(it, "v", num); } break; }
        case NVS_TYPE_I64: { int64_t v;  e = nvs_get_i64(nvs, info->key, &v);
                             if (e == ESP_OK) { snprintf(num, sizeof(num), "%lld", (long long)v); cJSON_AddStringToObject(it, "v", num); } break; }
        case NVS_TYPE_STR: {
            size_t len = 0;
            e = nvs_get_str(nvs, info->key, NULL, &len);
            if (e != ESP_OK) break;
            char *v = heap_caps_malloc(len + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (v == NULL) { e = ESP_ERR_NO_MEM; break; }
            e = nvs_get_str(nvs, info->key, v, &len);
            if (e == ESP_OK) cJSON_AddStringToObject(it, "v", v);
            heap_caps_free(v);
            break;
        }
        case NVS_TYPE_BLOB: {
            size_t len = 0;
            e = nvs_get_blob(nvs, info->key, NULL, &len);
            if (e != ESP_OK) break;
            uint8_t *v = heap_caps_malloc(len ? len : 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            char *hex = heap_caps_malloc(len * 2 + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (v == NULL || hex == NULL) e = ESP_ERR_NO_MEM;
            else e = nvs_get_blob(nvs, info->key, v, &len);
            if (e == ESP_OK) {
                static const char hx[] = "0123456789abcdef";
                for (size_t i = 0; i < len; i++) { hex[2 * i] = hx[v[i] >> 4]; hex[2 * i + 1] = hx[v[i] & 15]; }
                hex[len * 2] = '\0';
                cJSON_AddStringToObject(it, "v", hex);
            }
            if (v) heap_caps_free(v);
            if (hex) heap_caps_free(hex);
            break;
        }
        default: break;
    }
    if (e != ESP_OK) { cJSON_Delete(it); return NULL; }
    return it;
}

static esp_err_t web_get_backup(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *list = root ? cJSON_AddArrayToObject(root, "settings") : NULL;
    nvs_handle_t nvs;
    if (list == NULL || nvs_open(PLAYER_NVS_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) {
        if (root) cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Could not read the settings");
        return ESP_FAIL;
    }
    cJSON_AddNumberToObject(root, "p4lms_backup", 1);
    int n = 0;
    nvs_iterator_t iter = NULL;
    esp_err_t e = nvs_entry_find(NVS_DEFAULT_PART_NAME, PLAYER_NVS_NAMESPACE, NVS_TYPE_ANY, &iter);
    while (e == ESP_OK) {
        nvs_entry_info_t info;
        if (nvs_entry_info(iter, &info) == ESP_OK) {
            cJSON *it = web_backup_item(nvs, &info);
            if (it != NULL) { cJSON_AddItemToArray(list, it); n++; }
        }
        e = nvs_entry_next(&iter);
    }
    nvs_release_iterator(iter);
    nvs_close(nvs);

    char *txt = cJSON_Print(root);
    cJSON_Delete(root);
    if (txt == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Not enough memory");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Settings backup: %d values, %u bytes", n, (unsigned)strlen(txt));
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Content-Disposition", "attachment; filename=\"p4lms-settings.json\"");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    const esp_err_t err = httpd_resp_send(req, txt, strlen(txt));
    cJSON_free(txt);
    return err;
}

static int web_hex_val(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Checks (write == false) or writes (write == true) one backup item. */
static bool web_restore_item(nvs_handle_t nvs, const cJSON *it, bool write)
{
    const cJSON *k = cJSON_GetObjectItem(it, "k"), *t = cJSON_GetObjectItem(it, "t"),
                *v = cJSON_GetObjectItem(it, "v");
    if (!cJSON_IsString(k) || !cJSON_IsString(t) || v == NULL) return false;
    const char *key = k->valuestring, *type = t->valuestring;
    if (key[0] == '\0' || strlen(key) > 15) return false;
    if (strcmp(type, "str") == 0) {
        if (!cJSON_IsString(v)) return false;
        return !write || nvs_set_str(nvs, key, v->valuestring) == ESP_OK;
    }
    if (strcmp(type, "blob") == 0) {
        if (!cJSON_IsString(v)) return false;
        const size_t hl = strlen(v->valuestring);
        if (hl % 2) return false;
        uint8_t *b = heap_caps_malloc(hl / 2 + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (b == NULL) return false;
        bool ok = true;
        for (size_t i = 0; i < hl / 2 && ok; i++) {
            const int hi = web_hex_val(v->valuestring[2 * i]), lo = web_hex_val(v->valuestring[2 * i + 1]);
            if (hi < 0 || lo < 0) ok = false;
            else b[i] = (uint8_t)(hi << 4 | lo);
        }
        if (ok && write) ok = nvs_set_blob(nvs, key, b, hl / 2) == ESP_OK;
        heap_caps_free(b);
        return ok;
    }
    if (strcmp(type, "u64") == 0 || strcmp(type, "i64") == 0) {
        if (!cJSON_IsString(v)) return false;
        char *end = NULL;
        if (type[0] == 'u') {
            const unsigned long long x = strtoull(v->valuestring, &end, 10);
            if (end == v->valuestring || *end) return false;
            return !write || nvs_set_u64(nvs, key, (uint64_t)x) == ESP_OK;
        }
        const long long x = strtoll(v->valuestring, &end, 10);
        if (end == v->valuestring || *end) return false;
        return !write || nvs_set_i64(nvs, key, (int64_t)x) == ESP_OK;
    }
    if (!cJSON_IsNumber(v)) return false;
    const double d = v->valuedouble;
    if (d != (double)(long long)d) return false;
    const long long x = (long long)d;
    if (strcmp(type, "u8") == 0)  { if (x < 0 || x > UINT8_MAX) return false;             return !write || nvs_set_u8(nvs, key, (uint8_t)x) == ESP_OK; }
    if (strcmp(type, "i8") == 0)  { if (x < INT8_MIN || x > INT8_MAX) return false;       return !write || nvs_set_i8(nvs, key, (int8_t)x) == ESP_OK; }
    if (strcmp(type, "u16") == 0) { if (x < 0 || x > UINT16_MAX) return false;            return !write || nvs_set_u16(nvs, key, (uint16_t)x) == ESP_OK; }
    if (strcmp(type, "i16") == 0) { if (x < INT16_MIN || x > INT16_MAX) return false;     return !write || nvs_set_i16(nvs, key, (int16_t)x) == ESP_OK; }
    if (strcmp(type, "u32") == 0) { if (x < 0 || x > (long long)UINT32_MAX) return false; return !write || nvs_set_u32(nvs, key, (uint32_t)x) == ESP_OK; }
    if (strcmp(type, "i32") == 0) { if (x < INT32_MIN || x > INT32_MAX) return false;     return !write || nvs_set_i32(nvs, key, (int32_t)x) == ESP_OK; }
    return false;
}

static esp_err_t web_restore_reply(httpd_req_t *req, const char *status, const char *msg)
{
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_sendstr(req, msg);
}

static esp_err_t web_post_restore(httpd_req_t *req)
{
    const int total = req->content_len;
    if (total <= 0 || total > WEB_BACKUP_MAX) {
        return web_restore_reply(req, "400 Bad Request", "That file is not a P4-LMS settings backup.");
    }
    char *buf = heap_caps_malloc(total + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (buf == NULL) return web_restore_reply(req, "500 Internal Server Error", "Not enough memory - try again.");
    int got = 0, stalls = 0;
    while (got < total) {
        const int n = httpd_req_recv(req, buf + got, total - got);
        /* a slow moment is fine; a sender that has gone quiet (a laptop that
         * slept mid-upload) gives up after about 45 s instead of tying up
         * the web page for good */
        if (n == HTTPD_SOCK_ERR_TIMEOUT && ++stalls < 3) continue;
        if (n <= 0) { heap_caps_free(buf); return ESP_FAIL; }
        got += n;
        stalls = 0;
    }
    buf[total] = '\0';
    /* A real backup is nested 3 deep; the parser is recursive and the web
     * task's stack is small, so anything much deeper is refused unread. */
    int depth = 0, deepest = 0;
    bool in_str = false;
    for (int i = 0; i < total; i++) {
        const char ch = buf[i];
        if (in_str) { if (ch == '\\') i++; else if (ch == '"') in_str = false; continue; }
        if (ch == '"') in_str = true;
        else if (ch == '{' || ch == '[') { if (++depth > deepest) deepest = depth; }
        else if (ch == '}' || ch == ']') depth--;
    }
    cJSON *root = (deepest <= 8) ? cJSON_Parse(buf) : NULL;
    heap_caps_free(buf);

    const cJSON *mark = root ? cJSON_GetObjectItem(root, "p4lms_backup") : NULL;
    const cJSON *list = root ? cJSON_GetObjectItem(root, "settings") : NULL;
    const char *fail = NULL;
    if (!cJSON_IsNumber(mark) || !cJSON_IsArray(list) || cJSON_GetArraySize(list) == 0) {
        fail = "That file is not a P4-LMS settings backup.";
    }
    /* Check everything first, so a bad file changes nothing. */
    const cJSON *it;
    if (fail == NULL) {
        cJSON_ArrayForEach(it, list) {
            if (!web_restore_item(0, it, false)) { fail = "The backup file is damaged - nothing was changed."; break; }
        }
    }
    nvs_handle_t nvs;
    if (fail == NULL && nvs_open(PLAYER_NVS_NAMESPACE, NVS_READWRITE, &nvs) != ESP_OK) {
        fail = "Could not open the settings storage - nothing was changed.";
    }
    if (fail != NULL) {
        if (root) cJSON_Delete(root);
        ESP_LOGW(TAG, "Settings restore refused: %s", fail);
        return web_restore_reply(req, "400 Bad Request", fail);
    }
    /* Replace: clear what's there so settings not in the backup go back to
     * their defaults, then write the backup's values. */
    bool ok = nvs_erase_all(nvs) == ESP_OK;
    int n = 0;
    cJSON_ArrayForEach(it, list) {
        if (ok && web_restore_item(nvs, it, true)) n++;
        else ok = false;
    }
    if (ok) ok = nvs_commit(nvs) == ESP_OK;
    nvs_close(nvs);
    cJSON_Delete(root);
    if (!ok) {
        ESP_LOGE(TAG, "Settings restore failed part-way (%d values written)", n);
        return web_restore_reply(req, "500 Internal Server Error",
                                 "Writing the settings failed part-way. Try the restore again.");
    }
    ESP_LOGI(TAG, "Settings restored: %d values", n);
    web_restore_reply(req, "200 OK", "Settings restored - the display is restarting. "
                                     "This page reloads in about 20 seconds.");
    lms_request_restart("settings restored from a backup");
    return ESP_OK;
}

static esp_err_t web_post_restart(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_sendstr(req, "<!doctype html><meta name='viewport' content='width=device-width'>"
                            "<body style='font-family:system-ui;padding:24px'>Restarting the display... "
                            "<a href='/'>back to settings</a> in about 15 seconds.</body>");
    lms_request_restart("restart requested from the web page");
    return ESP_OK;
}

static void web_settings_start(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.stack_size = 8192;
    cfg.max_uri_handlers = 8;
    cfg.recv_wait_timeout = 15;             /* firmware uploads: allow slow Wi-Fi moments */
    httpd_handle_t server = NULL;
    if (httpd_start(&server, &cfg) != ESP_OK) {
        ESP_LOGW(TAG, "Web settings page could not start");
        return;
    }
    static const httpd_uri_t root    = { .uri = "/",        .method = HTTP_GET,  .handler = web_get_root };
    static const httpd_uri_t save    = { .uri = "/save",    .method = HTTP_POST, .handler = web_post_save };
    static const httpd_uri_t restart = { .uri = "/restart", .method = HTTP_POST, .handler = web_post_restart };
    httpd_register_uri_handler(server, &root);
    httpd_register_uri_handler(server, &save);
    httpd_register_uri_handler(server, &restart);
    static const httpd_uri_t update  = { .uri = "/update",  .method = HTTP_POST, .handler = web_post_update };
    httpd_register_uri_handler(server, &update);
    static const httpd_uri_t backup  = { .uri = "/backup",  .method = HTTP_GET,  .handler = web_get_backup };
    static const httpd_uri_t restore = { .uri = "/restore", .method = HTTP_POST, .handler = web_post_restore };
    httpd_register_uri_handler(server, &backup);
    httpd_register_uri_handler(server, &restore);

    const esp_timer_create_args_t targs = { .callback = ota_confirm_cb, .name = "ota_confirm" };
    if (esp_timer_create(&targs, &ota_confirm_timer) == ESP_OK) {
        esp_timer_start_once(ota_confirm_timer, 30ULL * 1000000ULL);
    }
    ESP_LOGI(TAG, "Web settings page ready: open http://<display IP>/");
}
