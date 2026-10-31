/*
 * P4-LMS - Updates from GitHub
 *
 * Part of main.c.  This file is #included by main.c (in order) and is not
 * compiled on its own, so it shares main.c's includes and static names.
 *
 * About once a day the display asks GitHub for the latest P4-LMS release and
 * compares its version with its own.  A newer one is announced once (a short
 * message on the screen) and then waits on Settings > About and on the web
 * page, with an INSTALL button.  Nothing installs by itself.
 *
 * INSTALL downloads the update file meant for this board straight from
 * GitHub - P4-LMS.bin, or P4-LMS-chip-v3.bin on a chip v3.x board - over a
 * checked secure connection, into the spare firmware slot, and restarts.  As
 * with an update from the web page, a new version that doesn't start
 * properly is replaced by the previous one by itself (19_web_settings.c).
 *
 * Memory: the board's internal RAM is scarce, and the Wi-Fi link to the
 * ESP32-C6 needs it, so the update task's stack, the secure connection and
 * the downloaded file all live in PSRAM.  A task with its stack in PSRAM must
 * not touch the flash chip (settings or firmware), so the settings are read
 * and written on the LVGL task (upd_ui_job), and the firmware is written by
 * a small helper task with an internal stack (upd_writer_task), started only
 * for that.  The screen hears about everything through ui_post.
 */
#include "esp_ota_ops.h"
#include "esp_app_desc.h"
#include "esp_chip_info.h"

#define UPD_REPO            "AllGood65/P4-LMS"
#define UPD_API_URL         "https://api.github.com/repos/" UPD_REPO "/releases/latest"
#define UPD_FILE_V1         "P4-LMS.bin"             /* chip v1.x boards */
#define UPD_FILE_V3         "P4-LMS-chip-v3.bin"     /* chip v3.x boards */
#define UPD_NVS_SEEN        "upd_seen"               /* the version already announced */
#define UPD_FIRST_DELAY_S   90                       /* first look, after start-up settles */
#define UPD_EVERY_S         (24 * 3600)              /* then once a day */
#define UPD_RETRY_S         3600                     /* GitHub not reached: try again in an hour */
#define UPD_JSON_MAX        (96 * 1024)
#define UPD_FILE_MAX        (4 * 1024 * 1024)        /* a firmware slot */

#define UPD_CMD_CHECK       BIT0
#define UPD_CMD_INSTALL     BIT1

typedef enum {
    UPD_IDLE = 0,       /* not looked yet */
    UPD_CHECKING,
    UPD_LATEST,         /* this is the newest */
    UPD_AVAILABLE,      /* a newer one is waiting */
    UPD_CHECK_FAILED,   /* GitHub couldn't be reached */
    UPD_INSTALLING,
    UPD_INSTALL_FAILED,
} upd_state_t;

/* One firmware update at a time, from GitHub or the web page (19). */
static volatile bool ota_busy = false;
static portMUX_TYPE ota_mux = portMUX_INITIALIZER_UNLOCKED;

/* Claim the spare slot for an update; false if one is already running. */
static bool ota_claim(void)
{
    bool ok = false;
    portENTER_CRITICAL(&ota_mux);
    if (!ota_busy) { ota_busy = true; ok = true; }
    portEXIT_CRITICAL(&ota_mux);
    return ok;
}

static volatile upd_state_t upd_state = UPD_IDLE;
static volatile int upd_percent = 0;
static SemaphoreHandle_t upd_lock = NULL;
static char upd_latest[24] = "";                 /* "2.8", newer than this one */
static char upd_url[320] = "";                   /* its file for this board */
static char upd_error[96] = "";                  /* why an install failed */
static TaskHandle_t upd_task_handle = NULL;

/* -------------------------------------------------------------- versions */

/* "v2.10.1" -> {2,10,1}; anything after the numbers is ignored. */
static int upd_parse_version(const char *s, int out[4])
{
    int n = 0;
    out[0] = out[1] = out[2] = out[3] = 0;
    if (s == NULL) return 0;
    if (*s == 'v' || *s == 'V') s++;
    while (n < 4 && isdigit((unsigned char)*s)) {
        int v = 0;
        while (isdigit((unsigned char)*s)) { v = v * 10 + (*s - '0'); if (v > 9999) v = 9999; s++; }
        out[n++] = v;
        if (*s != '.') break;
        s++;
    }
    return n;
}

/* true when version a is newer than version b. */
static bool upd_newer(const char *a, const char *b)
{
    int x[4], y[4];
    if (upd_parse_version(a, x) == 0 || upd_parse_version(b, y) == 0) return false;
    for (int i = 0; i < 4; i++) {
        if (x[i] != y[i]) return x[i] > y[i];
    }
    return false;
}

/* This board's update file name: chip v3.x boards need their own build. */
static const char *upd_file_name(void)
{
    esp_chip_info_t ci;
    esp_chip_info(&ci);
    return ci.revision >= 300 ? UPD_FILE_V3 : UPD_FILE_V1;     /* e.g. 302 = v3.2 */
}

/* A copy of the state for the screen or the web page. */
typedef struct {
    upd_state_t state;
    int percent;
    char latest[24];
    char error[96];
} upd_snapshot_t;

static void upd_snapshot(upd_snapshot_t *s)
{
    memset(s, 0, sizeof(*s));
    if (upd_lock != NULL) xSemaphoreTake(upd_lock, portMAX_DELAY);
    s->state = upd_state;
    s->percent = upd_percent;
    snprintf(s->latest, sizeof(s->latest), "%s", upd_latest);
    snprintf(s->error, sizeof(s->error), "%s", upd_error);
    if (upd_lock != NULL) xSemaphoreGive(upd_lock);
}

/* --------------------------------------------------------------- screen */
/* Settings > About shows a line and a button (18g_about.c builds the page
 * and calls upd_about_add); a full-screen card shows an install's progress. */
static lv_obj_t *upd_about_label = NULL;
static lv_obj_t *upd_about_btn = NULL;
static bool upd_install_armed = false;           /* INSTALL tapped once: tap again to go */
static volatile int64_t upd_checked_us = 0;      /* when GitHub last answered (esp_timer) */
static lv_timer_t *upd_arm_timer = NULL;
static lv_obj_t *upd_card = NULL;
static lv_obj_t *upd_card_label = NULL;
static lv_obj_t *upd_card_bar = NULL;

static void upd_request(uint32_t cmd)
{
    if (upd_task_handle != NULL) xTaskNotify(upd_task_handle, cmd, eSetBits);
}

/* The About line and button, as the state is now (LVGL task). */
static void upd_about_refresh_locked(void)
{
    if (upd_about_label == NULL || upd_about_btn == NULL) return;
    upd_snapshot_t s;
    upd_snapshot(&s);
    char line[128];
    const char *btn = "CHECK FOR UPDATES";
    uint32_t color = UI_TEXT_DIM;
    bool show_btn = true;
    switch (s.state) {
        case UPD_CHECKING:
            snprintf(line, sizeof(line), "Looking for updates...");
            show_btn = false;
            break;
        case UPD_LATEST:
            snprintf(line, sizeof(line), "This is the latest version");
            break;
        case UPD_AVAILABLE:
            snprintf(line, sizeof(line), "Version %s is available", s.latest);
            color = UI_ACCENT;
            btn = upd_install_armed ? "TAP AGAIN TO INSTALL" : "INSTALL";
            break;
        case UPD_CHECK_FAILED:
            snprintf(line, sizeof(line), "Couldn't reach GitHub to look for updates");
            break;
        case UPD_INSTALLING:
            snprintf(line, sizeof(line), "Installing %s... %d%%", s.latest, s.percent);
            color = UI_ACCENT;
            show_btn = false;
            break;
        case UPD_INSTALL_FAILED:
            snprintf(line, sizeof(line), "%s", s.error[0] ? s.error : "The update didn't install");
            color = UI_AMBER;
            btn = "TRY AGAIN";
            break;
        default:
            line[0] = '\0';
            break;
    }
    lv_label_set_text(upd_about_label, line);
    lv_obj_set_style_text_color(upd_about_label, lv_color_hex(color), 0);
    lv_obj_t *bl = lv_obj_get_child(upd_about_btn, 0);
    if (bl != NULL) lv_label_set_text(bl, btn);
    if (show_btn) lv_obj_clear_flag(upd_about_btn, LV_OBJ_FLAG_HIDDEN);
    else          lv_obj_add_flag(upd_about_btn, LV_OBJ_FLAG_HIDDEN);
}

static void upd_arm_timeout_cb(lv_timer_t *t)
{
    (void)t;                                     /* one-shot: LVGL deletes it */
    upd_arm_timer = NULL;
    upd_install_armed = false;
    upd_about_refresh_locked();
}

static void upd_about_btn_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    const upd_state_t st = upd_state;
    if (st == UPD_AVAILABLE || (st == UPD_INSTALL_FAILED && upd_latest[0])) {
        /* An install restarts the display: the first tap asks, the second goes. */
        if (!upd_install_armed) {
            upd_install_armed = true;
            if (upd_arm_timer != NULL) lv_timer_delete(upd_arm_timer);
            upd_arm_timer = lv_timer_create(upd_arm_timeout_cb, 5000, NULL);
            lv_timer_set_repeat_count(upd_arm_timer, 1);
            upd_about_refresh_locked();
            return;
        }
        upd_install_armed = false;
        if (upd_arm_timer != NULL) { lv_timer_delete(upd_arm_timer); upd_arm_timer = NULL; }
        upd_request(UPD_CMD_INSTALL);
    } else {
        upd_request(UPD_CMD_CHECK);
    }
}

/* Settings > About: the update line under the firmware version, and its
 * button (LVGL task, while the page is built). */
static void upd_about_add(lv_obj_t *panel, int32_t y)
{
    upd_install_armed = false;
    /* left of the QR code, which sits at the right from x ~ 670 */
    upd_about_label = ui_text(panel, "", &p4_font_16, UI_TEXT_DIM, 30, y, 0);
    lv_obj_set_width(upd_about_label, 360);
    lv_label_set_long_mode(upd_about_label, LV_LABEL_LONG_DOT);
    upd_about_btn = create_button(panel, "CHECK FOR UPDATES", 240, 44);
    lv_obj_set_pos(upd_about_btn, 400, y - 30);
    lv_obj_add_event_cb(upd_about_btn, upd_about_btn_cb, LV_EVENT_CLICKED, NULL);
    upd_about_refresh_locked();
    /* The answer shown may be hours old (a newer release since): opening
     * About looks again if the last look was over 10 minutes ago. */
    if ((upd_state == UPD_AVAILABLE || upd_state == UPD_LATEST) &&
        esp_timer_get_time() - upd_checked_us > 10LL * 60 * 1000000) {
        upd_request(UPD_CMD_CHECK);
    }
}

/* The About page is going away (18g_about.c): forget its pieces. */
static void upd_about_forget(void)
{
    upd_about_label = NULL;
    upd_about_btn = NULL;
    upd_install_armed = false;
    if (upd_arm_timer != NULL) { lv_timer_delete(upd_arm_timer); upd_arm_timer = NULL; }
}

/* The install card: over everything (even the screensaver), until restart. */
static void upd_card_show_locked(const char *text, int percent)
{
    if (upd_card == NULL) {
        upd_card = lv_obj_create(lv_layer_top());
        lv_obj_remove_style_all(upd_card);
        lv_obj_set_size(upd_card, LV_PCT(100), LV_PCT(100));
        lv_obj_set_style_bg_color(upd_card, lv_color_hex(0x000000), 0);
        lv_obj_set_style_bg_opa(upd_card, LV_OPA_80, 0);
        lv_obj_add_flag(upd_card, LV_OBJ_FLAG_CLICKABLE);        /* taps stop here */
        lv_obj_clear_flag(upd_card, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *box = lv_obj_create(upd_card);
        lv_obj_remove_style_all(box);
        lv_obj_set_size(box, 560, 200);
        lv_obj_center(box);
        lv_obj_set_style_bg_color(box, lv_color_hex(UI_PANEL), 0);
        lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(box, 2, 0);
        lv_obj_set_style_border_color(box, lv_color_hex(UI_AMBER), 0);
        lv_obj_set_style_radius(box, 16, 0);
        lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *title = lv_label_create(box);
        lv_label_set_text(title, "UPDATING P4-LMS");
        lv_obj_set_style_text_font(title, &p4_font_28, 0);
        lv_obj_set_style_text_color(title, lv_color_hex(UI_TEXT), 0);
        lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 22);

        upd_card_label = lv_label_create(box);
        lv_obj_set_width(upd_card_label, 500);
        lv_obj_set_style_text_align(upd_card_label, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_font(upd_card_label, &p4_font_18, 0);
        lv_obj_set_style_text_color(upd_card_label, lv_color_hex(UI_TEXT_SOFT), 0);
        lv_obj_align(upd_card_label, LV_ALIGN_TOP_MID, 0, 74);

        upd_card_bar = lv_bar_create(box);
        lv_obj_set_size(upd_card_bar, 480, 16);
        lv_obj_align(upd_card_bar, LV_ALIGN_BOTTOM_MID, 0, -34);
        lv_bar_set_range(upd_card_bar, 0, 100);
        lv_obj_set_style_bg_color(upd_card_bar, lv_color_hex(UI_TRACK), LV_PART_MAIN);
        lv_obj_set_style_bg_color(upd_card_bar, lv_color_hex(UI_ACCENT), LV_PART_INDICATOR);
    }
    lv_label_set_text(upd_card_label, text);
    lv_bar_set_value(upd_card_bar, percent < 0 ? 0 : percent, LV_ANIM_OFF);
    lv_obj_clear_flag(upd_card, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(upd_card);
}

static void upd_card_hide_locked(void)
{
    if (upd_card == NULL) return;
    lv_obj_delete(upd_card);
    upd_card = NULL;
    upd_card_label = NULL;
    upd_card_bar = NULL;
}

/* From the update task: what to show now. */
typedef struct {
    char toast[96];         /* a short message, or "" */
    char card[120];         /* the install card's line, or "" to take it away */
    int  percent;
    char announce[24];      /* a newer version to announce once, or "" */
} upd_ui_msg_t;

/* A newer version is announced once, remembered across restarts (settings
 * storage: LVGL task, see the note at the top). */
static void upd_announce_locked(const char *ver)
{
    char seen[24] = "";
    size_t sz = sizeof(seen);
    nvs_handle_t nvs;
    if (nvs_open(PLAYER_NVS_NAMESPACE, NVS_READWRITE, &nvs) != ESP_OK) return;
    if (nvs_get_str(nvs, UPD_NVS_SEEN, seen, &sz) != ESP_OK) seen[0] = '\0';
    if (strcmp(seen, ver) != 0) {
        nvs_set_str(nvs, UPD_NVS_SEEN, ver);
        nvs_commit(nvs);
        char t[96];
        snprintf(t, sizeof(t), "P4-LMS %s is available - see Settings > About", ver);
        ui_toast_long(t, 8000);
    }
    nvs_close(nvs);
}

static void upd_ui_job(void *arg)
{
    const upd_ui_msg_t *m = (const upd_ui_msg_t *)arg;
    if (m->card[0]) upd_card_show_locked(m->card, m->percent);
    else            upd_card_hide_locked();
    if (m->toast[0]) ui_toast_long(m->toast, 8000);
    if (m->announce[0]) upd_announce_locked(m->announce);
    upd_about_refresh_locked();
}

static void upd_ui_post_full(const char *toast, const char *card, int percent, const char *announce)
{
    upd_ui_msg_t m;
    snprintf(m.toast, sizeof(m.toast), "%s", toast ? toast : "");
    snprintf(m.card, sizeof(m.card), "%s", card ? card : "");
    m.percent = percent;
    snprintf(m.announce, sizeof(m.announce), "%s", announce ? announce : "");
    ui_post(upd_ui_job, &m, sizeof(m));
}

static void upd_ui_post(const char *toast, const char *card, int percent)
{
    upd_ui_post_full(toast, card, percent, NULL);
}

/* ------------------------------------------------------------ the check */

static void upd_set_state(upd_state_t st)
{
    if (upd_lock != NULL) xSemaphoreTake(upd_lock, portMAX_DELAY);
    upd_state = st;
    if (upd_lock != NULL) xSemaphoreGive(upd_lock);
}

/* Ask GitHub for the latest release.  True if it answered. */
static bool upd_check(void)
{
    upd_set_state(UPD_CHECKING);
    upd_ui_post(NULL, NULL, 0);

    char *buf = heap_caps_malloc(UPD_JSON_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (buf == NULL) { upd_set_state(UPD_CHECK_FAILED); upd_ui_post(NULL, NULL, 0); return false; }

    esp_http_client_config_t cfg = {
        .url = UPD_API_URL,
        .method = HTTP_METHOD_GET,
        .timeout_ms = 10000,
        .buffer_size = 4096,
        .buffer_size_tx = 1024,
        .user_agent = "P4-LMS",
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    size_t len = 0;
    int status = 0;
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (c != NULL) {
        esp_http_client_set_header(c, "Accept", "application/vnd.github+json");
        if (esp_http_client_open(c, 0) == ESP_OK) {
            (void)esp_http_client_fetch_headers(c);
            status = esp_http_client_get_status_code(c);
            while (len + 1 < UPD_JSON_MAX) {
                const int n = esp_http_client_read(c, buf + len, (int)(UPD_JSON_MAX - 1 - len));
                if (n <= 0) break;
                len += (size_t)n;
            }
            esp_http_client_close(c);
        }
        esp_http_client_cleanup(c);
    }
    buf[len] = '\0';

    bool ok = false;
    char tag[24] = "";
    char url[sizeof(upd_url)] = "";
    if (status == 200 && len > 0) {
        cJSON *root = cJSON_Parse(buf);
        const cJSON *t = root ? cJSON_GetObjectItem(root, "tag_name") : NULL;
        const cJSON *assets = root ? cJSON_GetObjectItem(root, "assets") : NULL;
        if (cJSON_IsString(t) && t->valuestring != NULL) {
            ok = true;
            const char *v = t->valuestring;
            if (*v == 'v' || *v == 'V') v++;
            snprintf(tag, sizeof(tag), "%.20s", v);
            const char *want = upd_file_name();
            const cJSON *a = NULL;
            cJSON_ArrayForEach(a, assets) {
                const cJSON *n = cJSON_GetObjectItem(a, "name");
                const cJSON *u = cJSON_GetObjectItem(a, "browser_download_url");
                if (cJSON_IsString(n) && n->valuestring && strcmp(n->valuestring, want) == 0 &&
                    cJSON_IsString(u) && u->valuestring &&
                    strncmp(u->valuestring, "https://github.com/" UPD_REPO "/", 20 + sizeof(UPD_REPO) - 1) == 0 &&
                    strlen(u->valuestring) < sizeof(url)) {
                    snprintf(url, sizeof(url), "%s", u->valuestring);
                    break;
                }
            }
        }
        if (root) cJSON_Delete(root);
    }
    heap_caps_free(buf);

    if (!ok) {
        ESP_LOGW(TAG, "Updates: GitHub didn't answer (HTTP %d)", status);
        upd_set_state(UPD_CHECK_FAILED);
        upd_ui_post(NULL, NULL, 0);
        return false;
    }

    const char *mine = esp_app_get_description()->version;
    const bool newer = url[0] && upd_newer(tag, mine);
    if (upd_lock != NULL) xSemaphoreTake(upd_lock, portMAX_DELAY);
    if (newer) {
        snprintf(upd_latest, sizeof(upd_latest), "%s", tag);
        snprintf(upd_url, sizeof(upd_url), "%s", url);
        upd_state = UPD_AVAILABLE;
    } else {
        upd_latest[0] = upd_url[0] = '\0';
        upd_state = UPD_LATEST;
    }
    upd_checked_us = esp_timer_get_time();
    if (upd_lock != NULL) xSemaphoreGive(upd_lock);
    ESP_LOGI(TAG, "Updates: latest release %s, this is %s%s", tag, mine,
             newer ? " - an update is available" : (url[0] ? "" : " (no file for this board)"));

    upd_ui_post_full(NULL, NULL, 0, newer ? tag : NULL);    /* announced once, on the LVGL task */
    return true;
}

/* ---------------------------------------------------------- the install */

static void upd_install_failed(const char *why)
{
    ESP_LOGW(TAG, "Updates: install failed - %s", why);
    if (upd_lock != NULL) xSemaphoreTake(upd_lock, portMAX_DELAY);
    snprintf(upd_error, sizeof(upd_error), "%s", why);
    upd_state = UPD_INSTALL_FAILED;
    if (upd_lock != NULL) xSemaphoreGive(upd_lock);
    upd_ui_post(why, NULL, 0);
}

/* Firmware writer: a small task with its stack in internal RAM (flash work
 * isn't allowed from a PSRAM stack).  It writes the downloaded image into the
 * spare slot, checks it, sets it to start, and reports back. */
typedef struct {
    const uint8_t *data;
    size_t len;
    char ver[24];
    TaskHandle_t notify;    /* told the result: 0 = installed */
} upd_write_job_t;

static upd_write_job_t upd_write_job;

static void upd_writer_task(void *arg)
{
    upd_write_job_t *j = (upd_write_job_t *)arg;
    uint32_t result = 1;
    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    esp_ota_handle_t h = 0;
    if (part != NULL && j->len <= part->size && esp_ota_begin(part, j->len, &h) == ESP_OK) {
        result = 0;
        int shown = -1;
        for (size_t off = 0; off < j->len && result == 0; off += 4096) {
            const size_t n = (j->len - off) < 4096 ? (j->len - off) : 4096;
            if (esp_ota_write(h, j->data + off, n) != ESP_OK) { result = 2; break; }
            const int pct = 80 + (int)((uint64_t)(off + n) * 20 / j->len);     /* 80..100 */
            if (pct >= shown + 2) {
                shown = pct;
                upd_percent = pct;
                char line[120];
                snprintf(line, sizeof(line), "Installing %s... %d%%\nThe display restarts when it's done.", j->ver, pct);
                upd_ui_post(NULL, line, pct);
            }
        }
        if (result == 0) {
            const esp_err_t err = esp_ota_end(h);
            if (err == ESP_ERR_OTA_VALIDATE_FAILED) result = 3;
            else if (err != ESP_OK || esp_ota_set_boot_partition(part) != ESP_OK) result = 4;
        } else {
            esp_ota_abort(h);
        }
    }
    xTaskNotify(j->notify, result, eSetValueWithOverwrite);
    vTaskDelete(NULL);
}

/* Download the file for this board into PSRAM, with a progress line.  GitHub
 * answers with a redirect to where the file really is.  Returns the buffer
 * (heap_caps_free it) or NULL with *why set. */
static uint8_t *upd_download(const char *url, const char *ver, size_t *out_len, const char **why)
{
    *out_len = 0;
    *why = "Couldn't download the update - check the internet connection";
    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_GET,
        .timeout_ms = 15000,
        .buffer_size = 4096,
        .buffer_size_tx = 4096,              /* the redirect's long signed link */
        .user_agent = "P4-LMS",
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (c == NULL) return NULL;

    int64_t total = -1;
    int status = 0;
    bool open = false;
    for (int hop = 0; hop < 5; hop++) {
        if (esp_http_client_open(c, 0) != ESP_OK) break;
        open = true;
        total = esp_http_client_fetch_headers(c);
        status = esp_http_client_get_status_code(c);
        if (status == 301 || status == 302 || status == 303 || status == 307 || status == 308) {
            esp_http_client_set_redirection(c);
            esp_http_client_close(c);
            open = false;
            continue;
        }
        break;
    }
    uint8_t *buf = NULL;
    if (open && status == 200 && total > 65536 && total <= UPD_FILE_MAX) {
        buf = heap_caps_malloc((size_t)total, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (buf == NULL) *why = "Not enough memory for the download";
    } else if (open && status == 200) {
        *why = "The download is the wrong size for P4-LMS firmware";
    }
    size_t len = 0;
    if (buf != NULL) {
        int shown = -1;
        while (len < (size_t)total) {
            const int n = esp_http_client_read(c, (char *)buf + len, (int)((size_t)total - len));
            if (n <= 0) break;
            len += (size_t)n;
            const int pct = (int)((uint64_t)len * 80 / (uint64_t)total);       /* 0..80 */
            if (pct >= shown + 2) {
                shown = pct;
                upd_percent = pct;
                char line[120];
                snprintf(line, sizeof(line), "Downloading %s... %d%%", ver, pct);
                upd_ui_post(NULL, line, pct);
            }
        }
        if (len != (size_t)total) {
            *why = "The download stopped part-way - the current version is untouched";
            heap_caps_free(buf);
            buf = NULL;
        }
    }
    if (open) esp_http_client_close(c);
    esp_http_client_cleanup(c);
    if (buf == NULL) {
        ESP_LOGW(TAG, "Updates: download failed (HTTP %d, %lld bytes expected, %u read)",
                 status, (long long)total, (unsigned)len);
        return NULL;
    }
    *out_len = len;
    return buf;
}

/* Is this a P4-LMS firmware image for this board?  NULL if so, else why not.
 * (The same checks as an update from the web page.) */
static const char *upd_image_problem(const uint8_t *b, size_t len)
{
    if (len < 128 || b[0] != 0xE9) return "The download isn't a firmware file - nothing was changed";
    const uint32_t desc_magic = (uint32_t)b[32] | ((uint32_t)b[33] << 8) |
                                ((uint32_t)b[34] << 16) | ((uint32_t)b[35] << 24);
    char project[33];
    memcpy(project, b + 80, 32);
    project[32] = '\0';
    if (desc_magic != 0xABCD5432U || strcmp(project, esp_app_get_description()->project_name) != 0) {
        return "The download isn't a P4-LMS update - nothing was changed";
    }
    const unsigned min_rev = (unsigned)b[15] | ((unsigned)b[16] << 8);
    const unsigned max_rev = (unsigned)b[17] | ((unsigned)b[18] << 8);
    esp_chip_info_t ci;
    esp_chip_info(&ci);
    if (ci.revision < min_rev || (max_rev != 0 && max_rev != 0xFFFF && ci.revision > max_rev)) {
        return "The download was built for the other kind of board - nothing was changed";
    }
    return NULL;
}

static void upd_install(void)
{
    char url[sizeof(upd_url)];
    char ver[sizeof(upd_latest)];
    if (upd_lock != NULL) xSemaphoreTake(upd_lock, portMAX_DELAY);
    snprintf(url, sizeof(url), "%s", upd_url);
    snprintf(ver, sizeof(ver), "%s", upd_latest);
    if (upd_lock != NULL) xSemaphoreGive(upd_lock);
    if (url[0] == '\0') { upd_install_failed("No update to install - look for updates first"); return; }
    if (!ota_claim()) { upd_install_failed("An update is already running"); return; }
    if (upd_lock != NULL) xSemaphoreTake(upd_lock, portMAX_DELAY);
    upd_state = UPD_INSTALLING;
    upd_percent = 0;
    upd_error[0] = '\0';
    if (upd_lock != NULL) xSemaphoreGive(upd_lock);

    char line[120];
    snprintf(line, sizeof(line), "Downloading %s from GitHub...", ver);
    upd_ui_post(NULL, line, 0);
    ESP_LOGI(TAG, "Updates: installing %s from %s", ver, url);

    size_t len = 0;
    const char *fail = NULL;
    uint8_t *img = upd_download(url, ver, &len, &fail);
    if (img != NULL) fail = upd_image_problem(img, len);

    if (img != NULL && fail == NULL) {
        /* write it with the helper task, and wait for its answer */
        upd_write_job.data = img;
        upd_write_job.len = len;
        snprintf(upd_write_job.ver, sizeof(upd_write_job.ver), "%s", ver);
        upd_write_job.notify = xTaskGetCurrentTaskHandle();
        xTaskNotifyStateClear(NULL);
        if (xTaskCreate(upd_writer_task, "upd_write", 4096, &upd_write_job, 3, NULL) != pdPASS) {
            fail = "Not enough memory to install - nothing was changed";
        } else {
            uint32_t result = 1;
            xTaskNotifyWait(0, UINT32_MAX, &result, portMAX_DELAY);
            if (result == 3)      fail = "The update file is damaged - the current version is untouched";
            else if (result != 0) fail = "Couldn't finish installing - the current version is untouched";
        }
    }
    if (img != NULL) heap_caps_free(img);
    ota_busy = false;

    if (fail != NULL) {
        upd_install_failed(fail);
        return;
    }
    snprintf(line, sizeof(line), "Installed %s - restarting now.", ver);
    upd_ui_post(NULL, line, 100);
    ESP_LOGI(TAG, "Updates: %s installed", ver);
    vTaskDelay(pdMS_TO_TICKS(1500));
    lms_request_restart("update installed from GitHub");
}

/* ---------------------------------------------------------------- task */

static void upd_task(void *arg)
{
    (void)arg;
    TickType_t wait = pdMS_TO_TICKS(UPD_FIRST_DELAY_S * 1000);
    for (;;) {
        uint32_t cmd = 0;
        xTaskNotifyWait(0, UINT32_MAX, &cmd, wait);
        /* no network yet: look again once there is */
        if ((xEventGroupGetBits(wifi_event_group) & WIFI_CONNECTED_BIT) == 0) {
            if (cmd & UPD_CMD_INSTALL) upd_install_failed("Not connected to Wi-Fi");
            wait = pdMS_TO_TICKS(60 * 1000);
            continue;
        }
        if (cmd & UPD_CMD_INSTALL) {
            /* Look once more first, so it's the newest release that goes
             * in (not one found hours ago and since replaced). */
            if (!upd_check()) {
                upd_install_failed("Couldn't reach GitHub - nothing was changed");
            } else if (upd_state == UPD_AVAILABLE) {
                upd_install();
            }                                   /* else: already the latest - shown */
            wait = pdMS_TO_TICKS(UPD_EVERY_S * 1000ULL);
            continue;
        }
        if (upd_state == UPD_INSTALLING) continue;
        wait = upd_check() ? pdMS_TO_TICKS(UPD_EVERY_S * 1000ULL) : pdMS_TO_TICKS(UPD_RETRY_S * 1000);
    }
}

/* Start-up, once (19_web_settings.c starts it with the web page). */
static void upd_start(void)
{
    if (upd_task_handle != NULL) return;
    upd_lock = xSemaphoreCreateMutex();
    /* TLS needs a roomy stack: in PSRAM, so it costs no internal RAM (see the
     * note at the top: this task never touches the flash chip itself) */
    if (xTaskCreatePinnedToCoreWithCaps(upd_task, "updates", 12288, NULL, 3, &upd_task_handle,
                                        tskNO_AFFINITY, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        upd_task_handle = NULL;
        ESP_LOGW(TAG, "Updates: task could not start");
    }
}

/* ------------------------------------------------------------ web page */
/* The "From GitHub" part of the Firmware update section (19_web_settings.c). */
static void upd_web_section_text(char *out, size_t n, bool *can_install, bool *busy)
{
    upd_snapshot_t s;
    upd_snapshot(&s);
    *can_install = false;
    *busy = false;
    switch (s.state) {
        case UPD_CHECKING:     snprintf(out, n, "Looking for updates on GitHub..."); *busy = true; break;
        case UPD_LATEST:       snprintf(out, n, "This is the latest version."); break;
        case UPD_AVAILABLE:    snprintf(out, n, "Version %s is available on GitHub.", s.latest); *can_install = true; break;
        case UPD_CHECK_FAILED: snprintf(out, n, "Couldn't reach GitHub to look for updates."); break;
        case UPD_INSTALLING:   snprintf(out, n, "Installing %s... %d%% - the display restarts when it's done.",
                                        s.latest, s.percent); *busy = true; break;
        case UPD_INSTALL_FAILED:
            snprintf(out, n, "%s.", s.error[0] ? s.error : "The update didn't install");
            *can_install = s.latest[0] != '\0';
            break;
        default:               snprintf(out, n, "Not checked yet - the display looks once a day."); break;
    }
}
