/*
 * P4-LMS - Weather for the Weather screensaver (Open-Meteo, no account needed)
 *
 * Part of main.c.  This file is #included by main.c (in order) and is not
 * compiled on its own, so it shares main.c's includes and static names.
 *
 * The location is typed on the web settings page ("Toronto", "Truro, NS",
 * a postcode...).  It is looked up once with Open-Meteo's geocoding service;
 * the current conditions are then fetched every 10 minutes while the
 * Weather screensaver is the chosen one.  Everything runs on the command
 * task (weather_tick), never on the display task.
 */

#define WX_NVS_LOC    "wx_loc"
#define WX_NVS_PLACE  "wx_place"
#define WX_NVS_LAT    "wx_lat"
#define WX_NVS_LON    "wx_lon"
#define WX_NVS_UNITS  "wx_units"
#define WX_REFRESH_MS (10LL * 60 * 1000)

static char wx_location[64] = "";            /* as typed                      */
static char wx_place[96] = "";               /* as found: "Halifax, Nova Scotia" */
static char wx_lat[16] = "", wx_lon[16] = "";
static volatile int  wx_units = 0;           /* 0 = C and km/h, 1 = F and mph */
static volatile bool wx_geocode_pending = false;
static int64_t       wx_geocode_retry_ms = 0;     /* lookup failed: not before this (ms) */
static volatile bool wx_want_now = false;    /* fetch at the next tick        */
static int64_t wx_fetched_ms = 0;

/* Latest reading (written by the command task, read by the display). */
static volatile bool  wx_have = false;
static volatile float wx_temp = 0.0f;
static volatile int   wx_humidity = 0;
static volatile float wx_wind = 0.0f;
static volatile int   wx_code = -1;
static volatile int   wx_is_day = 1;
static volatile uint32_t wx_version = 0;     /* bumped on every new reading   */
static char wx_status[160] = "No location set - type one on the web page.";

/* The next three days (tomorrow first). */
#define WX_FC_DAYS 3
static volatile int   wx_fc_n = 0;
static volatile int   wx_fc_code[WX_FC_DAYS];
static volatile float wx_fc_hi[WX_FC_DAYS], wx_fc_lo[WX_FC_DAYS];
static char wx_fc_day[WX_FC_DAYS][4];                 /* "MON" */

static void weather_load(void)
{
    nvs_handle_t nvs;
    if (nvs_open(PLAYER_NVS_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) return;
    size_t n = sizeof(wx_location);
    if (nvs_get_str(nvs, WX_NVS_LOC, wx_location, &n) != ESP_OK) wx_location[0] = '\0';
    n = sizeof(wx_place);
    if (nvs_get_str(nvs, WX_NVS_PLACE, wx_place, &n) != ESP_OK) wx_place[0] = '\0';
    n = sizeof(wx_lat);
    if (nvs_get_str(nvs, WX_NVS_LAT, wx_lat, &n) != ESP_OK) wx_lat[0] = '\0';
    n = sizeof(wx_lon);
    if (nvs_get_str(nvs, WX_NVS_LON, wx_lon, &n) != ESP_OK) wx_lon[0] = '\0';
    uint8_t u = 0;
    if (nvs_get_u8(nvs, WX_NVS_UNITS, &u) == ESP_OK) wx_units = (u != 0);
    nvs_close(nvs);
    if (wx_location[0] && !wx_lat[0]) wx_geocode_pending = true;
    if (wx_place[0]) snprintf(wx_status, sizeof(wx_status), "Weather for %s", wx_place);
}

static void weather_save(void)
{
    nvs_handle_t nvs;
    if (nvs_open(PLAYER_NVS_NAMESPACE, NVS_READWRITE, &nvs) != ESP_OK) return;
    nvs_set_str(nvs, WX_NVS_LOC, wx_location);
    nvs_set_str(nvs, WX_NVS_PLACE, wx_place);
    nvs_set_str(nvs, WX_NVS_LAT, wx_lat);
    nvs_set_str(nvs, WX_NVS_LON, wx_lon);
    nvs_set_u8(nvs, WX_NVS_UNITS, wx_units ? 1 : 0);
    nvs_commit(nvs);
    nvs_close(nvs);
}

/* Web page: a new location (looked up at the next tick) and/or units. */
static void weather_set(const char *location, int units)
{
    char loc[64];
    snprintf(loc, sizeof(loc), "%s", location ? location : "");
    size_t l = strlen(loc);
    while (l > 0 && loc[l - 1] == ' ') loc[--l] = '\0';
    const bool changed = strcmp(loc, wx_location) != 0;
    const bool units_changed = (units != wx_units);
    snprintf(wx_location, sizeof(wx_location), "%s", loc);
    wx_units = units;
    if (changed) {
        wx_place[0] = wx_lat[0] = wx_lon[0] = '\0';
        wx_have = false;
        wx_geocode_pending = (loc[0] != '\0');
        wx_geocode_retry_ms = 0;                     /* a new place: look it up now */
        snprintf(wx_status, sizeof(wx_status), loc[0] ? "Looking up %.60s..." : "No location set.", loc);
    }
    if (changed || units_changed) wx_want_now = true;
    weather_save();
}

/* Plain GET into buf (http first: no TLS memory needed; then https). */
static bool weather_get(const char *path_and_query, const char *host, char *buf, size_t size)
{
    for (int attempt = 0; attempt < 2; attempt++) {
        if (attempt == 1 && command_pending()) return false;   /* a tap first; retried later */
        char url[512];
        snprintf(url, sizeof(url), "%s://%s%s", attempt == 0 ? "http" : "https", host, path_and_query);
        esp_http_client_config_t cfg = {
            .url = url, .method = HTTP_METHOD_GET, .timeout_ms = 6000,
            .buffer_size = 2048, .buffer_size_tx = 1024,
        };
        if (attempt == 1) cfg.crt_bundle_attach = esp_crt_bundle_attach;
        esp_http_client_handle_t c = esp_http_client_init(&cfg);
        if (c == NULL) continue;
        size_t len = 0;
        bool ok = esp_http_client_open(c, 0) == ESP_OK;
        if (ok) {
            (void)esp_http_client_fetch_headers(c);
            while (len + 1 < size) {
                const int n = esp_http_client_read(c, buf + len, (int)(size - 1 - len));
                if (n <= 0) break;
                len += (size_t)n;
            }
            ok = esp_http_client_get_status_code(c) == 200 && len > 0;
            esp_http_client_close(c);
        }
        esp_http_client_cleanup(c);
        buf[len] = '\0';
        if (ok) return true;
        ESP_LOGW(TAG, "Weather: %s request failed", attempt == 0 ? "http" : "https");
    }
    return false;
}

/* One lookup: search for name, prefer a result whose province / state /
 * country matches hint.  Returns 1 found, 0 not found, -1 service unreachable. */
static int weather_try(const char *name, const char *hint, char *buf, size_t buf_size)
{
    char esc[128], path[200];
    url_escape(name, esc, sizeof(esc));
    snprintf(path, sizeof(path), "/v1/search?name=%s&count=10&language=en&format=json", esc);
    if (!weather_get(path, "geocoding-api.open-meteo.com", buf, buf_size)) return -1;
    int found = 0;
    cJSON *root = cJSON_Parse(buf);
    cJSON *res = root ? cJSON_GetObjectItem(root, "results") : NULL;
    cJSON *pick = NULL, *r = NULL;
    cJSON_ArrayForEach(r, res) {
        if (pick == NULL) pick = r;                           /* first = most likely */
        if (hint[0]) {
            const char *keys[3] = { "admin1", "country", "country_code" };
            for (int k = 0; k < 3; k++) {
                cJSON *v = cJSON_GetObjectItem(r, keys[k]);
                if (cJSON_IsString(v) && v->valuestring &&
                    (strcasecmp(v->valuestring, hint) == 0 ||
                     strncasecmp(v->valuestring, hint, strlen(hint)) == 0)) {
                    pick = r;
                    goto chosen;
                }
            }
        }
    }
chosen:
    if (pick != NULL) {
        cJSON *la = cJSON_GetObjectItem(pick, "latitude");
        cJSON *lo = cJSON_GetObjectItem(pick, "longitude");
        cJSON *nm = cJSON_GetObjectItem(pick, "name");
        cJSON *a1 = cJSON_GetObjectItem(pick, "admin1");
        cJSON *co = cJSON_GetObjectItem(pick, "country");
        if (cJSON_IsNumber(la) && cJSON_IsNumber(lo)) {
            snprintf(wx_lat, sizeof(wx_lat), "%.4f", la->valuedouble);
            snprintf(wx_lon, sizeof(wx_lon), "%.4f", lo->valuedouble);
            snprintf(wx_place, sizeof(wx_place), "%.40s%s%.30s%s%.20s",
                     cJSON_IsString(nm) ? nm->valuestring : name,
                     cJSON_IsString(a1) ? ", " : "", cJSON_IsString(a1) ? a1->valuestring : "",
                     cJSON_IsString(co) ? ", " : "", cJSON_IsString(co) ? co->valuestring : "");
            found = 1;
        }
    }
    if (root) cJSON_Delete(root);
    return found;
}

static void weather_geocode(void)
{
    wx_geocode_pending = false;
    /* "Truro, NS" -> search "Truro", and "NS" helps pick among the results. */
    char name[64], hint[64] = "";
    snprintf(name, sizeof(name), "%s", wx_location);
    char *comma = strchr(name, ',');
    if (comma != NULL) {
        *comma = '\0';
        const char *h = comma + 1;
        while (*h == ' ') h++;
        snprintf(hint, sizeof(hint), "%s", h);
    }
    char *buf = heap_caps_malloc(16384, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (buf == NULL) return;
    int got = weather_try(name, hint, buf, 16384);

    /* No comma, e.g. "Halifax Nova Scotia" or "St Johns NL": try the first
     * words as the town and the rest as the hint, longest town name first. */
    if (got == 0 && comma == NULL && strchr(name, ' ') != NULL) {
        char words[64];
        snprintf(words, sizeof(words), "%s", name);
        for (char *sp = strrchr(words, ' '); sp != NULL && got == 0; ) {
            *sp = '\0';
            const char *rest = name + (sp - words) + 1;
            while (*rest == ' ') rest++;
            char town[64];
            snprintf(town, sizeof(town), "%s", words);
            size_t tl = strlen(town);
            while (tl > 0 && town[tl - 1] == ' ') town[--tl] = '\0';
            if (tl > 0) got = weather_try(town, rest, buf, 16384);
            sp = strrchr(words, ' ');
        }
    }
    heap_caps_free(buf);

    const bool found = (got == 1);
    if (got < 0) {
        snprintf(wx_status, sizeof(wx_status), "Couldn't reach the weather service");
        wx_geocode_pending = true;                             /* try again later... */
        wx_geocode_retry_ms = esp_timer_get_time() / 1000 + 5 * 60000;   /* ...in 5 minutes */
        wx_version++;                                          /* the screensaver shows the status */
    } else {
        snprintf(wx_status, sizeof(wx_status), found ? "Weather for %.80s" : "Couldn't find \"%.60s\" - try another spelling",
                 found ? wx_place : wx_location);
    }
    ESP_LOGI(TAG, "Weather location: %s", wx_status);
    if (found) {
        weather_save();
        wx_want_now = true;
    }
}

static void weather_fetch(void)
{
    char path[384];
    snprintf(path, sizeof(path),
             "/v1/forecast?latitude=%s&longitude=%s&current=temperature_2m,relative_humidity_2m,"
             "weather_code,wind_speed_10m,is_day&daily=weather_code,temperature_2m_max,"
             "temperature_2m_min&forecast_days=4&timezone=auto&temperature_unit=%s&wind_speed_unit=%s",
             wx_lat, wx_lon, wx_units ? "fahrenheit" : "celsius", wx_units ? "mph" : "kmh");
    char *buf = heap_caps_malloc(8192, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (buf == NULL) return;
    if (weather_get(path, "api.open-meteo.com", buf, 8192)) {
        cJSON *root = cJSON_Parse(buf);
        cJSON *cur = root ? cJSON_GetObjectItem(root, "current") : NULL;
        if (cJSON_IsObject(cur)) {
            cJSON *t = cJSON_GetObjectItem(cur, "temperature_2m");
            cJSON *h = cJSON_GetObjectItem(cur, "relative_humidity_2m");
            cJSON *w = cJSON_GetObjectItem(cur, "wind_speed_10m");
            cJSON *c = cJSON_GetObjectItem(cur, "weather_code");
            cJSON *d = cJSON_GetObjectItem(cur, "is_day");
            if (cJSON_IsNumber(t)) {
                wx_temp = (float)t->valuedouble;
                wx_humidity = cJSON_IsNumber(h) ? h->valueint : -1;
                wx_wind = cJSON_IsNumber(w) ? (float)w->valuedouble : -1.0f;
                wx_code = cJSON_IsNumber(c) ? c->valueint : -1;
                wx_is_day = cJSON_IsNumber(d) ? d->valueint : 1;
                wx_have = true;
                wx_version++;
                ESP_LOGI(TAG, "Weather: %.1f%s, code %d, humidity %d%%, wind %.0f",
                         wx_temp, wx_units ? "F" : "C", wx_code, wx_humidity, wx_wind);
            }
        }
        /* daily: index 0 is today, 1..3 the next days */
        cJSON *daily = root ? cJSON_GetObjectItem(root, "daily") : NULL;
        if (cJSON_IsObject(daily)) {
            cJSON *tm = cJSON_GetObjectItem(daily, "time");
            cJSON *co = cJSON_GetObjectItem(daily, "weather_code");
            cJSON *hi = cJSON_GetObjectItem(daily, "temperature_2m_max");
            cJSON *lo = cJSON_GetObjectItem(daily, "temperature_2m_min");
            int n = 0;
            for (int k = 1; k <= WX_FC_DAYS && cJSON_IsArray(tm); k++) {
                cJSON *t1 = cJSON_GetArrayItem(tm, k);
                cJSON *c1 = cJSON_GetArrayItem(co, k);
                cJSON *h1 = cJSON_GetArrayItem(hi, k);
                cJSON *l1 = cJSON_GetArrayItem(lo, k);
                if (!cJSON_IsString(t1) || !cJSON_IsNumber(h1) || !cJSON_IsNumber(l1)) break;
                struct tm d = { 0 };
                int yy = 0, mm = 0, dd = 0;
                if (sscanf(t1->valuestring, "%d-%d-%d", &yy, &mm, &dd) != 3) break;
                d.tm_year = yy - 1900; d.tm_mon = mm - 1; d.tm_mday = dd; d.tm_hour = 12;
                mktime(&d);
                static const char *const wd[7] = { "SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT" };
                snprintf(wx_fc_day[n], sizeof(wx_fc_day[n]), "%s", wd[d.tm_wday % 7]);
                wx_fc_code[n] = cJSON_IsNumber(c1) ? c1->valueint : -1;
                wx_fc_hi[n] = (float)h1->valuedouble;
                wx_fc_lo[n] = (float)l1->valuedouble;
                n++;
            }
            wx_fc_n = n;
            wx_version++;
        }
        if (root) cJSON_Delete(root);
        wx_fetched_ms = esp_timer_get_time() / 1000;
    } else {
        wx_fetched_ms = esp_timer_get_time() / 1000 - WX_REFRESH_MS + 60000;   /* retry in a minute */
    }
    heap_caps_free(buf);
}

/* Command task, every few seconds. */
static void weather_tick(void)
{
    if (!wx_location[0]) return;
    if (!(xEventGroupGetBits(wifi_event_group) & WIFI_CONNECTED_BIT)) return;
    if (wx_geocode_pending) {
        if (esp_timer_get_time() / 1000 < wx_geocode_retry_ms) return;   /* backing off */
        weather_geocode();
        return;
    }
    if (!wx_lat[0]) return;
    const bool needed = (ss_pause_mode == 4) || wx_want_now;       /* Weather screensaver chosen */
    const int64_t now = esp_timer_get_time() / 1000;
    if (needed && (wx_want_now || wx_fetched_ms == 0 || now - wx_fetched_ms >= WX_REFRESH_MS)) {
        wx_want_now = false;
        weather_fetch();
    }
}

/* ---- words and a picture for the WMO weather code ---- */
typedef enum { WX_ICON_SUN, WX_ICON_MOON, WX_ICON_PARTLY, WX_ICON_CLOUD, WX_ICON_FOG,
               WX_ICON_RAIN, WX_ICON_SNOW, WX_ICON_STORM } wx_icon_t;

static const char *weather_words(int code, wx_icon_t *icon)
{
    const bool day = wx_is_day != 0;
    if (code == 0)                 { *icon = day ? WX_ICON_SUN : WX_ICON_MOON; return "Clear"; }
    if (code == 1)                 { *icon = day ? WX_ICON_SUN : WX_ICON_MOON; return "Mainly clear"; }
    if (code == 2)                 { *icon = WX_ICON_PARTLY; return "Partly cloudy"; }
    if (code == 3)                 { *icon = WX_ICON_CLOUD;  return "Cloudy"; }
    if (code == 45 || code == 48)  { *icon = WX_ICON_FOG;    return "Fog"; }
    if (code >= 51 && code <= 57)  { *icon = WX_ICON_RAIN;   return "Drizzle"; }
    if (code >= 61 && code <= 67)  { *icon = WX_ICON_RAIN;   return code >= 66 ? "Freezing rain" : "Rain"; }
    if (code >= 71 && code <= 77)  { *icon = WX_ICON_SNOW;   return "Snow"; }
    if (code >= 80 && code <= 82)  { *icon = WX_ICON_RAIN;   return "Showers"; }
    if (code == 85 || code == 86)  { *icon = WX_ICON_SNOW;   return "Snow showers"; }
    if (code >= 95)                { *icon = WX_ICON_STORM;  return "Thunderstorm"; }
    *icon = WX_ICON_CLOUD;
    return "";
}
