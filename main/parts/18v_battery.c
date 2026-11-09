/* ------------------------------------------------------------ battery level
 * Waveshare ESP32-P4-WIFI6-Touch-LCD-7B: the battery socket (BAT) is wired
 * through R92 200k / R93 100k to GPIO20 (ADC1 channel 4), so the pin sees a
 * third of the battery voltage: 4.2 V full -> 1.40 V, 3.3 V empty -> 1.10 V.
 *
 * Two things the board can't tell us:
 *  - whether it's charging (the charger's STAT output only drives the green
 *    LED), and
 *  - whether a battery is there at all: with no battery, the charger holds
 *    the battery line at its charging voltage (a bare board reads ~4.15 V,
 *    the same as a full battery on USB).
 * So the level is only shown when "Battery installed" is turned on (web page,
 * Theme section) - off by default, so wall-powered displays don't show a
 * battery that isn't there.  4.10 V and up shows as full (also "on USB"). */

#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"

#define BAT_GPIO          20             /* GPIO number */
#define BAT_DIVIDER       3              /* (200k + 100k) / 100k */
#define BAT_SAMPLES       16             /* averaged per reading */
#define BAT_READ_MS       1000           /* one reading a second */
#define BAT_LOG_EVERY     60             /* readings between log lines */
#define BAT_LOW_PCT       10             /* red icon + one warning below this */
#define BAT_LOW_RESET_PCT 20             /* warn again only after rising above this */

static volatile bool ui_battery_on = false;   /* "Battery installed" setting */

static volatile int bat_pin_mv    = -1;  /* at GPIO20, averaged (-1 = not read yet) */
static volatile int bat_mv        = -1;  /* battery = pin x 3 */
static volatile int bat_smooth_mv = -1;  /* slowly smoothed, for the level shown */
static bool bat_calibrated = false;

static adc_oneshot_unit_handle_t bat_adc = NULL;
static adc_cali_handle_t bat_cali = NULL;
static adc_channel_t bat_channel;

/* A single-cell LiPo, resting voltage -> charge (most of its life is spent
 * between 3.7 and 3.9 V, so a straight line would be badly off). */
static const struct { int mv; int pct; } bat_curve[] = {
    { 4100, 100 }, { 4060, 95 }, { 4020, 90 }, { 3980, 85 }, { 3950, 80 },
    { 3910, 75 }, { 3870, 70 }, { 3850, 65 }, { 3840, 60 }, { 3820, 55 },
    { 3800, 50 }, { 3790, 45 }, { 3770, 40 }, { 3750, 35 }, { 3730, 30 },
    { 3710, 25 }, { 3690, 20 }, { 3670, 15 }, { 3640, 10 }, { 3600, 5 },
    { 3300, 0 },
};

static int bat_percent(int mv)
{
    const int n = (int)(sizeof(bat_curve) / sizeof(bat_curve[0]));
    if (mv >= bat_curve[0].mv) return 100;
    if (mv <= bat_curve[n - 1].mv) return 0;
    for (int i = 1; i < n; i++) {
        if (mv >= bat_curve[i].mv) {
            const int hi_mv = bat_curve[i - 1].mv, lo_mv = bat_curve[i].mv;
            const int hi_p = bat_curve[i - 1].pct, lo_p = bat_curve[i].pct;
            return lo_p + (mv - lo_mv) * (hi_p - lo_p) / (hi_mv - lo_mv);
        }
    }
    return 0;
}

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
    int n = 0;
    for (;;) {
        const int pin = bat_read_pin_mv();
        if (pin >= 0) {
            const int bat = pin * BAT_DIVIDER;
            bat_pin_mv = pin;
            bat_mv = bat;
            /* smoothed over ~8 s: the level shouldn't jump with every
             * burst of Wi-Fi or backlight current */
            const int s = bat_smooth_mv;
            bat_smooth_mv = (s < 0) ? bat : s + (bat - s) / 8;
            if (++n >= BAT_LOG_EVERY) {
                n = 0;
                ESP_LOGI(TAG, "Battery: %d.%02d V (pin %d mV) -> %d%%%s", bat / 1000, (bat % 1000) / 10,
                         pin, bat_percent(bat_smooth_mv), ui_battery_on ? "" : " (not shown: Battery installed is off)");
            }
        }
        vTaskDelay(pdMS_TO_TICKS(BAT_READ_MS));
    }
}

static void bat_start(void)
{
    adc_unit_t unit;
    if (adc_oneshot_io_to_channel(BAT_GPIO, &unit, &bat_channel) != ESP_OK) {
        ESP_LOGW(TAG, "Battery: GPIO%d isn't an ADC pin", BAT_GPIO);
        return;
    }
    adc_oneshot_unit_init_cfg_t unit_cfg = { .unit_id = unit };
    if (adc_oneshot_new_unit(&unit_cfg, &bat_adc) != ESP_OK) {
        ESP_LOGW(TAG, "Battery: couldn't start the ADC");
        return;
    }
    adc_oneshot_chan_cfg_t chan_cfg = { .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT };
    if (adc_oneshot_config_channel(bat_adc, bat_channel, &chan_cfg) != ESP_OK) {
        ESP_LOGW(TAG, "Battery: couldn't set up GPIO%d", BAT_GPIO);
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
        ESP_LOGW(TAG, "Battery: couldn't start its task");
        return;
    }
    ESP_LOGI(TAG, "Battery: reading GPIO%d (ADC%d channel %d)%s - shown: %s", BAT_GPIO,
             (int)unit + 1, (int)bat_channel, bat_calibrated ? ", calibrated" : "",
             ui_battery_on ? "yes" : "no (Battery installed is off)");
}

/* "4.14 V" etc. for the web page and About (blank before the first reading). */
static void bat_volts_text(char *out, size_t len)
{
    const int bat = bat_mv;
    if (bat < 0) snprintf(out, len, "-");
    else         snprintf(out, len, "%d.%02d V", bat / 1000, (bat % 1000) / 10);
}

/* ---- the header indicator (LVGL task) ---- */
static lv_obj_t *bat_header_label = NULL;
static int bat_shown_pct = -1;          /* what's on screen (-1 = hidden) */
static bool bat_low_warned = false;

static void bat_header_timer_cb(lv_timer_t *t)
{
    (void)t;
    if (bat_header_label == NULL) return;
    const int s = bat_smooth_mv;
    if (!ui_battery_on || s < 0) {
        if (bat_shown_pct != -1) {
            lv_obj_add_flag(bat_header_label, LV_OBJ_FLAG_HIDDEN);
            bat_shown_pct = -1;
        }
        return;
    }
    const int p = bat_percent(s);
    /* a change of 2 % or more, or reaching full/empty, before the number
     * moves - so it doesn't flicker between two values */
    if (bat_shown_pct >= 0 && abs(p - bat_shown_pct) < 2 && p != 100 && p != 0) return;
    if (p == bat_shown_pct) return;

    const char *sym = p >= 80 ? LV_SYMBOL_BATTERY_FULL : p >= 55 ? LV_SYMBOL_BATTERY_3 :
                      p >= 30 ? LV_SYMBOL_BATTERY_2 : p >= 10 ? LV_SYMBOL_BATTERY_1 :
                      LV_SYMBOL_BATTERY_EMPTY;
    char text[40];
    snprintf(text, sizeof(text), "%s %d%%", sym, p);
    lv_label_set_text(bat_header_label, text);
    lv_obj_set_style_text_color(bat_header_label,
                                lv_color_hex(p < BAT_LOW_PCT ? UI_RED : UI_TEXT_SOFT), 0);
    lv_obj_clear_flag(bat_header_label, LV_OBJ_FLAG_HIDDEN);
    bat_shown_pct = p;

    if (p < BAT_LOW_PCT && !bat_low_warned) {
        bat_low_warned = true;
        snprintf(text, sizeof(text), "Battery low - %d%%", p);
        ui_toast(text);
    } else if (p > BAT_LOW_RESET_PCT) {
        bat_low_warned = false;
    }
}

/* In the header, between the room chip and the clock. */
static void bat_header_create(lv_obj_t *header)
{
    bat_header_label = lv_label_create(header);
    lv_label_set_text(bat_header_label, "");
    lv_obj_set_style_text_font(bat_header_label, &p4_font_18, 0);
    lv_obj_set_style_text_color(bat_header_label, lv_color_hex(UI_TEXT_SOFT), 0);
    lv_obj_align(bat_header_label, LV_ALIGN_LEFT_MID, 286, 0);
    lv_obj_add_flag(bat_header_label, LV_OBJ_FLAG_HIDDEN);
    lv_timer_create(bat_header_timer_cb, 2000, NULL);
}

/* ---- Settings > About line (updated once a second while it's open) ---- */
static lv_obj_t *bat_about_label = NULL;
static lv_timer_t *bat_about_timer = NULL;

static void bat_about_text(char *out, size_t len)
{
    /* short: it sits between the firmware line and the update button */
    const int bat = bat_mv;
    if (bat < 0) snprintf(out, len, "Battery: reading...");
    else if (!ui_battery_on) snprintf(out, len, "Battery line %d.%02d V", bat / 1000, (bat % 1000) / 10);
    else snprintf(out, len, "Battery %d.%02d V  %d%%", bat / 1000, (bat % 1000) / 10, bat_percent(bat_smooth_mv));
}

static void bat_about_timer_cb(lv_timer_t *t)
{
    (void)t;
    if (bat_about_label == NULL) return;
    char line[64];
    bat_about_text(line, sizeof(line));
    lv_label_set_text(bat_about_label, line);
}

static void bat_about_add(lv_obj_t *panel, int32_t x, int32_t y)
{
    char line[64];
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
