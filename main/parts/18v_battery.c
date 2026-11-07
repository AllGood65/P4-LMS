/* ------------------------------------------------------------ battery level
 * Waveshare ESP32-P4-WIFI6-Touch-LCD-7B: the battery socket (BAT) is wired
 * through R92 200k / R93 100k to GPIO20 (ADC1), so the pin sees a third of
 * the battery voltage: 4.2 V full -> 1.40 V, 3.3 V empty -> 1.10 V.
 * (The charger's STAT output only drives the board's green LED, so whether
 * it is charging can't be read.)
 *
 * TEST STAGE: this only measures and reports - in the log every 10 s and on
 * Settings > About - so we can see what a board with no battery reads, and
 * check the numbers against a real battery, before an icon goes in. */

#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"

#define BAT_GPIO          20             /* GPIO number */
#define BAT_DIVIDER       3              /* (200k + 100k) / 100k */
#define BAT_SAMPLES       16             /* averaged per reading */
#define BAT_READ_MS       1000           /* one reading a second */
#define BAT_LOG_EVERY     10             /* readings between log lines */

static volatile int bat_pin_mv = -1;     /* at GPIO20, averaged (-1 = not read yet) */
static volatile int bat_mv     = -1;     /* battery = pin x 3 */
static volatile int bat_lo_mv  = -1;     /* lowest / highest battery reading over */
static volatile int bat_hi_mv  = -1;     /* the last BAT_LOG_EVERY seconds */
static bool bat_calibrated = false;

static adc_oneshot_unit_handle_t bat_adc = NULL;
static adc_cali_handle_t bat_cali = NULL;
static adc_channel_t bat_channel;

static lv_obj_t *bat_about_label = NULL;
static lv_timer_t *bat_about_timer = NULL;

/* One averaged pin voltage in mV, or -1. */
static int bat_read_pin_mv(void)
{
    long sum = 0;
    int got = 0;
    for (int i = 0; i < BAT_SAMPLES; i++) {
        int raw = 0;
        if (adc_oneshot_read(bat_adc, bat_channel, &raw) != ESP_OK) continue;
        int mv = 0;
        if (bat_calibrated) {
            if (adc_cali_raw_to_voltage(bat_cali, raw, &mv) != ESP_OK) continue;
        } else {
            mv = raw * 3300 / 4095;                   /* rough, uncalibrated */
        }
        sum += mv;
        got++;
    }
    return got ? (int)(sum / got) : -1;
}

static void bat_task(void *arg)
{
    (void)arg;
    int n = 0, lo = 0, hi = 0;
    for (;;) {
        const int pin = bat_read_pin_mv();
        if (pin >= 0) {
            const int bat = pin * BAT_DIVIDER;
            bat_pin_mv = pin;
            bat_mv = bat;
            if (n == 0) { lo = hi = bat; }
            if (bat < lo) lo = bat;
            if (bat > hi) hi = bat;
            if (++n >= BAT_LOG_EVERY) {
                bat_lo_mv = lo;
                bat_hi_mv = hi;
                ESP_LOGI(TAG, "Battery test: %d.%02d V (pin %d mV), last %d s %d.%02d-%d.%02d V%s",
                         bat / 1000, (bat % 1000) / 10, pin, BAT_LOG_EVERY,
                         lo / 1000, (lo % 1000) / 10, hi / 1000, (hi % 1000) / 10,
                         bat_calibrated ? "" : " (uncalibrated)");
                n = 0;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(BAT_READ_MS));
    }
}

static void bat_start(void)
{
    adc_unit_t unit;
    if (adc_oneshot_io_to_channel(BAT_GPIO, &unit, &bat_channel) != ESP_OK) {
        ESP_LOGW(TAG, "Battery test: GPIO%d isn't an ADC pin", (int)BAT_GPIO);
        return;
    }
    adc_oneshot_unit_init_cfg_t unit_cfg = { .unit_id = unit };
    if (adc_oneshot_new_unit(&unit_cfg, &bat_adc) != ESP_OK) {
        ESP_LOGW(TAG, "Battery test: couldn't start the ADC");
        return;
    }
    adc_oneshot_chan_cfg_t chan_cfg = { .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT };
    if (adc_oneshot_config_channel(bat_adc, bat_channel, &chan_cfg) != ESP_OK) {
        ESP_LOGW(TAG, "Battery test: couldn't set up GPIO%d", (int)BAT_GPIO);
        return;
    }
#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    adc_cali_curve_fitting_config_t cali_cfg = {
        .unit_id = unit, .chan = bat_channel,
        .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    bat_calibrated = adc_cali_create_scheme_curve_fitting(&cali_cfg, &bat_cali) == ESP_OK;
#endif
    /* Only reads the ADC (no flash writes), so its stack can live in PSRAM
     * and leave the scarce internal RAM to Wi-Fi. */
    if (xTaskCreatePinnedToCoreWithCaps(bat_task, "battery", 3072, NULL, 2, NULL,
                                        tskNO_AFFINITY, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        ESP_LOGW(TAG, "Battery test: couldn't start its task");
        return;
    }
    ESP_LOGI(TAG, "Battery test: reading GPIO%d (ADC%d channel %d)%s", (int)BAT_GPIO,
             (int)unit + 1, (int)bat_channel, bat_calibrated ? ", calibrated" : "");
}

/* ---- Settings > About line (updated once a second while it's open) ---- */
static void bat_about_text(char *out, size_t len)
{
    /* short: it sits between the firmware line and the update button */
    const int pin = bat_pin_mv, bat = bat_mv;
    if (pin < 0) snprintf(out, len, "Battery: reading...");
    else         snprintf(out, len, "Battery %d.%02d V (pin %d)", bat / 1000, (bat % 1000) / 10, pin);
}

static void bat_about_timer_cb(lv_timer_t *t)
{
    (void)t;
    if (bat_about_label == NULL) return;
    char line[96];
    bat_about_text(line, sizeof(line));
    lv_label_set_text(bat_about_label, line);
}

static void bat_about_add(lv_obj_t *panel, int32_t x, int32_t y)
{
    char line[96];
    bat_about_text(line, sizeof(line));
    bat_about_label = ui_text(panel, line, &p4_font_16, UI_TEXT_DIM, x, y, 0);
    if (bat_about_timer == NULL) bat_about_timer = lv_timer_create(bat_about_timer_cb, 1000, NULL);
}

/* The About page is going away (its delete callback). */
static void bat_about_forget(void)
{
    if (bat_about_timer != NULL) {
        lv_timer_delete(bat_about_timer);
        bat_about_timer = NULL;
    }
    bat_about_label = NULL;
}
