/* ============================================================
 * ABOUT page (Settings > About) and the one-time tip
 * ============================================================
 * Shows where the web settings page is (number address and the
 * "p4lms.local" name), the firmware version, a QR code a phone can scan to
 * open the web page, and a short list of the "hidden" taps on the screen.
 *
 * The page is built each time it opens and thrown away when it closes, so
 * its addresses are always current and the QR picture (about 80 KB) only
 * takes memory while it is on screen.  LVGL task only (event callbacks).
 *
 * The QR code comes from Project Nayuki's QR Code generator (main/qr/p4qr.c,
 * MIT licence, taken from LVGL).  LVGL's own QR widget is switched off in
 * this project's LVGL settings, so this does not depend on it. */

static lv_obj_t *about_overlay = NULL;
static uint8_t  *about_qr_pixels = NULL;        /* RGB565, PSRAM */
static lv_image_dsc_t about_qr_dsc;

#define ABOUT_QR_PX         6       /* screen pixels per QR square */
#define ABOUT_QR_QUIET      4       /* white border, in squares (the QR standard asks for 4) */
#define ABOUT_QR_MAX_VER    10      /* far more than a web address needs */

/* The panel is being deleted: free the QR picture with it. */
static void about_delete_event_cb(lv_event_t *e)
{
    (void)e;
    upd_about_forget();                        /* its update line and button go too */
    if (about_qr_pixels != NULL) {
        heap_caps_free(about_qr_pixels);
        about_qr_pixels = NULL;
    }
}

/* Close the About page (also used by BACK, the tab bar and the idle timer,
 * through nav_hide_panels). */
static void about_hide(void)
{
    if (about_overlay == NULL) return;
    lv_obj_add_flag(about_overlay, LV_OBJ_FLAG_HIDDEN);
    /* "async": this may run inside a tap on one of the panel's own buttons,
     * and an object must not be deleted while its event is still running. */
    lv_obj_delete_async(about_overlay);
    about_overlay = NULL;
}

static void about_close_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) about_hide();
}

/* Draws `text` as a QR code into a new image on `parent`.  Returns NULL if
 * the code could not be made (no memory, or text far too long). */
static lv_obj_t *about_qr_create(lv_obj_t *parent, const char *text)
{
    /* static: 2 x ~400 bytes is more than the LVGL task's stack should carry */
    static uint8_t qr[p4qr_BUFFER_LEN_FOR_VERSION(ABOUT_QR_MAX_VER)];
    static uint8_t tmp[p4qr_BUFFER_LEN_FOR_VERSION(ABOUT_QR_MAX_VER)];
    if (!p4qr_encodeText(text, tmp, qr, p4qr_Ecc_MEDIUM, p4qr_VERSION_MIN, ABOUT_QR_MAX_VER,
                         p4qr_Mask_AUTO, true)) {
        return NULL;
    }
    const int modules = p4qr_getSize(qr);
    const int side_mod = modules + 2 * ABOUT_QR_QUIET;
    const int side = side_mod * ABOUT_QR_PX;                  /* e.g. 33 x 6 = 198 px */
    const size_t size = (size_t)side * (size_t)side * 2U;

    if (about_qr_pixels != NULL) heap_caps_free(about_qr_pixels);
    about_qr_pixels = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (about_qr_pixels == NULL) return NULL;

    /* One row of squares at a time: work out a row of pixels, then copy it
     * ABOUT_QR_PX times (each square is ABOUT_QR_PX pixels tall). */
    uint16_t *px = (uint16_t *)about_qr_pixels;
    for (int my = 0; my < side_mod; my++) {
        uint16_t *row = px + (size_t)my * ABOUT_QR_PX * side;
        for (int mx = 0; mx < side_mod; mx++) {
            const int qx = mx - ABOUT_QR_QUIET, qy = my - ABOUT_QR_QUIET;
            const bool dark = (qx >= 0 && qy >= 0 && qx < modules && qy < modules) &&
                              p4qr_getModule(qr, qx, qy);
            const uint16_t c = dark ? 0x0000 : 0xFFFF;         /* black on white */
            for (int k = 0; k < ABOUT_QR_PX; k++) row[mx * ABOUT_QR_PX + k] = c;
        }
        for (int k = 1; k < ABOUT_QR_PX; k++) {
            memcpy(row + (size_t)k * side, row, (size_t)side * 2U);
        }
    }

    memset(&about_qr_dsc, 0, sizeof(about_qr_dsc));
    about_qr_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    about_qr_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
    about_qr_dsc.header.w = side;
    about_qr_dsc.header.h = side;
    about_qr_dsc.header.stride = side * 2U;
    about_qr_dsc.data_size = size;
    about_qr_dsc.data = about_qr_pixels;

    lv_obj_t *img = lv_image_create(parent);
    lv_image_set_src(img, &about_qr_dsc);
    lv_obj_clear_flag(img, LV_OBJ_FLAG_CLICKABLE);
    return img;
}

/* The taps that aren't obvious from the screen itself. */
static const char *const about_tips[] = {
    "Tap the album cover to open the album",
    "Tap the song title to see the queue",
    "Tap the VU meters for the full-screen visualizer",
    "Hold + on a song to play it next",
    "Tap the speaker icon to mute",
    "Tap the song length to see the time left",
};

static void about_show(void)
{
    nav_hide_panels();                         /* also closes an old About page */
    if (hub_overlay == NULL) return;
    lv_obj_t *screen = lv_obj_get_parent(hub_overlay);

    about_overlay = nav_panel_create(screen, "ABOUT", about_close_event_cb);
    ui_add_settings_back(about_overlay);
    lv_obj_add_event_cb(about_overlay, about_delete_event_cb, LV_EVENT_DELETE, NULL);

    /* Where the web page is */
    char addr[48], line[112], mdns_name[MDNS_NAME_BUF_LEN];
    hub_web_address(addr, sizeof(addr));            /* "http://<IP>/" or "(not connected)" */
    const bool online = strncmp(addr, "http://", 7) == 0;
    p4_mdns_name_get(mdns_name, sizeof(mdns_name));

    ui_text(about_overlay, "Web settings and updates - open on a phone or computer:", &p4_font_16, UI_TEXT_SOFT, 30, 86, 0);
    ui_text(about_overlay, addr, &p4_font_28, UI_ACCENT, 30, 114, 0);
    if (online && mdns_name[0]) {
        snprintf(line, sizeof(line), "or  http://%s.local/", mdns_name);
        ui_text(about_overlay, line, &p4_font_18, UI_TEXT, 30, 156, 0);
    }
    snprintf(line, sizeof(line), "Firmware %s", esp_app_get_description()->version);
    ui_text(about_overlay, line, &p4_font_16, UI_TEXT_DIM, 30, 190, 0);
    upd_about_add(about_overlay, 214);          /* updates from GitHub (18u_updates.c) */

    /* Tips */
    ui_text(about_overlay, "TIPS", &p4_font_18, UI_ACCENT, 30, 240, 0);
    for (size_t i = 0; i < sizeof(about_tips) / sizeof(about_tips[0]); i++) {
        snprintf(line, sizeof(line), LV_SYMBOL_BULLET "  %s", about_tips[i]);
        lv_obj_t *l = ui_text(about_overlay, line, &p4_font_18, UI_TEXT, 30, 274 + (int)i * 34, 0);
        lv_obj_set_width(l, 580);
        lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
    }

    /* QR code of the number address (it works even where ".local" names
     * don't, e.g. on some Android phones). */
    if (online) {
        lv_obj_t *qr = about_qr_create(about_overlay, addr);
        if (qr != NULL) {
            lv_obj_align(qr, LV_ALIGN_TOP_RIGHT, -40, 100);
            lv_obj_t *cap = lv_label_create(about_overlay);
            lv_label_set_text(cap, "Scan to open settings");
            lv_obj_set_style_text_font(cap, &p4_font_14, 0);
            lv_obj_set_style_text_color(cap, lv_color_hex(UI_TEXT_SOFT), 0);
            lv_obj_align_to(cap, qr, LV_ALIGN_OUT_BOTTOM_MID, 0, 10);
        }
    } else {
        lv_obj_t *l = ui_text(about_overlay, "Connect to Wi-Fi\nto get a QR code", &p4_font_16, UI_TEXT_DIM, 0, 0, 0);
        lv_obj_align(l, LV_ALIGN_TOP_RIGHT, -60, 160);
    }

    ui_show_panel(about_overlay);
}

/* ------------------------------------------------- one-time tip toast */
/* Shown once, on the first start after this feature arrived, so the owner
 * learns that the cover can be tapped and where the other tips live.  The
 * "already shown" mark is kept in the settings storage (NVS). */
#define ABOUT_NVS_TIP_SHOWN "tip_cover1"

static void about_tip_timer_cb(lv_timer_t *t)
{
    (void)t;                                   /* one-shot: LVGL deletes it */
    ui_toast_long("Tip: tap the cover to open the album - more tips in Settings > About", 8000);
    nvs_handle_t nvs;
    if (nvs_open(PLAYER_NVS_NAMESPACE, NVS_READWRITE, &nvs) == ESP_OK) {
        nvs_set_u8(nvs, ABOUT_NVS_TIP_SHOWN, 1);
        nvs_commit(nvs);
        nvs_close(nvs);
    }
}

/* Called once at start-up, after the screen is built (LVGL locked). */
static void about_first_tip_schedule(void)
{
    uint8_t shown = 0;
    nvs_handle_t nvs;
    if (nvs_open(PLAYER_NVS_NAMESPACE, NVS_READONLY, &nvs) == ESP_OK) {
        nvs_get_u8(nvs, ABOUT_NVS_TIP_SHOWN, &shown);
        nvs_close(nvs);
    }
    if (shown) return;
    /* a few seconds in, once the first song and cover are on screen */
    lv_timer_t *t = lv_timer_create(about_tip_timer_cb, 6000, NULL);
    lv_timer_set_repeat_count(t, 1);
}
