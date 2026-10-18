/* ============================================================
 * mDNS: a friendly name for the web settings page
 * ============================================================
 * Instead of typing the display's number address (which the router may
 * change), phones and computers can open http://p4lms.local/ .  This uses
 * Espressif's mDNS component (main/idf_component.yml).
 *
 * If another device on the network already uses "p4lms" (a second display,
 * say), the mDNS component picks a new name by itself, e.g. "p4lms-2".  That
 * is why the name actually in use is read back for showing on the About
 * screen and the web page, rather than assuming "p4lms".
 *
 * Nothing here is essential: if mDNS fails to start, it is only logged and
 * the display carries on (the number address still works). */

#define P4_MDNS_HOSTNAME  "p4lms"
#define P4_MDNS_INSTANCE  "P4-LMS display"

static volatile bool p4_mdns_started = false;
static bool p4_mdns_tried = false;
/* The name in use, e.g. "p4lms" (without ".local"); empty = mDNS not running. */
static char p4_mdns_name[MDNS_NAME_BUF_LEN] = "";

/* Called once the display has a network address (Wi-Fi event handler).
 * Started only once: the mDNS component itself follows later reconnects. */
static void p4_mdns_start(void)
{
    if (p4_mdns_tried) return;
    p4_mdns_tried = true;

    esp_err_t err = mdns_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "mDNS: could not start (%s) - use the number address instead", esp_err_to_name(err));
        return;
    }
    err = mdns_hostname_set(P4_MDNS_HOSTNAME);
    if (err == ESP_OK) err = mdns_instance_name_set(P4_MDNS_INSTANCE);
    if (err == ESP_OK) {
        /* tells "network browsers" (and some routers) there is a web page here */
        err = mdns_service_add(P4_MDNS_INSTANCE, "_http", "_tcp", 80, NULL, 0);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "mDNS: setup failed (%s)", esp_err_to_name(err));
        return;
    }
    snprintf(p4_mdns_name, sizeof(p4_mdns_name), "%s", P4_MDNS_HOSTNAME);
    p4_mdns_started = true;
    ESP_LOGI(TAG, "mDNS: web settings also at http://%s.local/", P4_MDNS_HOSTNAME);
}

/* Copies the name in use into out ("" if mDNS is not running).  Asks the
 * mDNS component each time, so a name changed after a clash shows up. */
static void p4_mdns_name_get(char *out, size_t n)
{
    if (n == 0) return;
    out[0] = '\0';
    if (!p4_mdns_started) return;
    char name[MDNS_NAME_BUF_LEN];
    if (mdns_hostname_get(name) == ESP_OK && name[0] != '\0') {
        snprintf(p4_mdns_name, sizeof(p4_mdns_name), "%s", name);
    }
    snprintf(out, n, "%s", p4_mdns_name);
}
