/*
 * P4-LMS - SETTINGS screen (theme picker) and early NVS start-up
 *
 * Part of main.c.  This file is #included by main.c (in order) and is not
 * compiled on its own, so it shares main.c's includes and static names.
 */

/* ------------------------------------------------------------------------
 * SETTINGS: colour theme picker
 * ------------------------------------------------------------------------ */
#define THEME_NVS_KEY "theme"

static lv_obj_t *settings_overlay = NULL;
static lv_obj_t *settings_note_label = NULL;
static lv_obj_t *settings_list = NULL;
static lv_obj_t *settings_active_card = NULL;

/* Order the themes are shown in (indices into ui_themes[]).  The table order
 * itself never changes, because the saved choice is stored as a table index. */
static const uint8_t theme_display_order[] = {
    0,                                  /* AMBER                                        */
    16, 17,                             /* RECEIVER BLUE, BLUE METERS (hi-fi inspired)  */
    7, 8, 9, 10, 11, 12, 13, 14, 15,    /* VINTAGE ... ROSE GOLD (multi-colour)        */
    18, 19, 20,                         /* PATINA, COCOA, CYBER                         */
    1, 2, 3, 4, 5, 6                    /* ICE, TEAL, PHOSPHOR, SYNTH, MONO, NIGHT      */
};
_Static_assert(sizeof(theme_display_order) == 21, "theme_display_order must list every theme once");
static bool      theme_applying = false;

/* Read the saved theme (called once, before any screen is built). */
static void ui_theme_load(void)
{
    nvs_handle_t nvs;
    uint8_t value = 0;

    if (nvs_open(PLAYER_NVS_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) {
        return;
    }
    esp_err_t ret = nvs_get_u8(nvs, THEME_NVS_KEY, &value);
    nvs_close(nvs);

    if (ret == ESP_OK && (int)value < UI_THEME_COUNT) {
        ui_theme = &ui_themes[value];
        ESP_LOGI(TAG, "Theme: %s", ui_theme->name);
    }
}

static bool ui_theme_save(int index)
{
    nvs_handle_t nvs;
    bool ok = false;
    if (nvs_open(PLAYER_NVS_NAMESPACE, NVS_READWRITE, &nvs) == ESP_OK) {
        ok = (nvs_set_u8(nvs, THEME_NVS_KEY, (uint8_t)index) == ESP_OK) &&
             (nvs_commit(nvs) == ESP_OK);
        nvs_close(nvs);
    }
    return ok;
}

/* The saved theme and visualizer style must be known BEFORE any widget is
 * built, so NVS is opened at the very start of app_main instead of waiting for
 * wifi_init().  (Calling nvs_flash_init() a second time later is harmless.) */
static void early_nvs_init(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
}

static void theme_button_event_cb(lv_event_t *e);

/* How full LVGL's own memory pool is (it has a fixed size). */
static void lvgl_mem_log(const char *when)
{
    lv_mem_monitor_t mon;
    lv_mem_monitor(&mon);
    ESP_LOGI(TAG, "LVGL memory (%s): %u%% used, %u bytes free, largest free block %u",
             when, (unsigned)mon.used_pct, (unsigned)mon.free_size, (unsigned)mon.free_biggest_size);
}

/* The colour preview squares are painted by the card itself (one object) instead
 * of five separate objects per card. */
static void theme_card_draw_cb(lv_event_t *e)
{
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_obj_t *obj = (lv_obj_t *)lv_event_get_target(e);
    const int index = (int)(intptr_t)lv_event_get_user_data(e);
    if (layer == NULL || obj == NULL || index < 0 || index >= UI_THEME_COUNT) return;

    const ui_theme_t *t = &ui_themes[index];
    const uint32_t swatch[5] = { t->accent, t->amber, t->tertiary, t->accent_glow, t->text };

    lv_area_t card;
    lv_obj_get_coords(obj, &card);

    for (int k = 0; k < 5; k++) {
        lv_draw_rect_dsc_t dsc;
        lv_draw_rect_dsc_init(&dsc);
        dsc.bg_color = lv_color_hex(swatch[k]);
        dsc.bg_opa = LV_OPA_COVER;
        dsc.radius = 7;
        lv_area_t a;
        a.x1 = card.x1 + 18 + k * 30;
        a.y1 = card.y1 + 30;
        a.x2 = a.x1 + 23;
        a.y2 = a.y1 + 23;
        lv_draw_rect(layer, &dsc, &a);
    }
}

/* Rough worst case per card: the card object + its label + the styles LVGL
 * allocates for the border/bg colours - measured generously, not exactly. */
#define SETTINGS_BYTES_PER_CARD 420
#define SETTINGS_MIN_FREE_BLOCK 512

static void settings_build_cards(void)
{
    if (settings_list == NULL) {
        return;
    }
    settings_active_card = NULL;

    lv_mem_monitor_t mon;
    lv_mem_monitor(&mon);
    const size_t needed = (size_t)UI_THEME_COUNT * SETTINGS_BYTES_PER_CARD;

    /* lv_mem_monitor() only reports real numbers when LVGL is using its own
     * built-in allocator.  Switched to the "Standard C library" allocator
     * (the fix for the earlier memory crash), it always reports everything
     * as zero - LVGL just calls malloc()/free() directly and keeps no book
     * of its own.  Treat that as "cannot tell, so don't refuse" rather than
     * "definitely full", or this would block the theme list forever. */
    const bool monitor_supported = (mon.total_size != 0);

    if (monitor_supported && (mon.free_size < needed || mon.free_biggest_size < SETTINGS_MIN_FREE_BLOCK)) {
        ESP_LOGW(TAG,
                 "SETTINGS: not enough free LVGL memory for the theme list "
                 "(free=%u biggest_block=%u needed~=%u) - refusing to build it "
                 "rather than risk a lock-up. Raise LV_MEM_SIZE_KILOBYTES in "
                 "menuconfig (Component config -> LVGL configuration -> Memory Settings).",
                 (unsigned)mon.free_size, (unsigned)mon.free_biggest_size, (unsigned)needed);
        if (settings_note_label != NULL) {
            lv_label_set_text(settings_note_label,
                "Not enough free display memory right now to show the theme "
                "list.  Close this and try again, or increase LV_MEM_SIZE_KILOBYTES "
                "in menuconfig (see the serial log).");
            lv_obj_set_style_text_color(settings_note_label, lv_color_hex(UI_RED), 0);
        }
        lvgl_mem_log("SETTINGS refused (low memory)");
        return;
    }

    for (int pos = 0; pos < UI_THEME_COUNT; pos++) {
        const int i = theme_display_order[pos];
        const ui_theme_t *t = &ui_themes[i];
        const bool current = (t == ui_theme);

        lv_obj_t *btn = lv_obj_create(settings_list);
        lv_obj_remove_style_all(btn);
        lv_obj_set_size(btn, 425, 84);
        lv_obj_set_pos(btn, 10 + (pos % 2) * 445, 6 + (pos / 2) * 96);
        lv_obj_set_style_bg_color(btn, lv_color_hex(t->panel_alt), 0);
        lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(btn, 12, 0);
        lv_obj_set_style_border_width(btn, current ? 4 : 2, 0);
        lv_obj_set_style_border_color(btn, lv_color_hex(current ? t->accent_glow : t->amber), 0);
        lv_obj_set_style_bg_color(btn, lv_color_hex(t->button), LV_STATE_PRESSED);
        lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(btn, theme_button_event_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_obj_add_event_cb(btn, theme_card_draw_cb, LV_EVENT_DRAW_MAIN_END, (void *)(intptr_t)i);
        if (current) {
            settings_active_card = btn;
        }

        lv_obj_t *name = lv_label_create(btn);
        char text[40];
        snprintf(text, sizeof(text), "%s%s", current ? LV_SYMBOL_OK "  " : "", t->name);
        lv_label_set_text(name, text);
        lv_obj_set_style_text_color(name, lv_color_hex(t->text), 0);
        /* Longer names ("RECEIVER BLUE", "BLUE METERS") get a smaller font so
         * they stay inside the card instead of running past its right edge. */
        lv_obj_set_style_text_font(name,
            strlen(t->name) > 9 ? &p4_font_18 : &p4_font_28, 0);
        lv_obj_set_width(name, 425 - 190 - 12);
        lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
        lv_obj_align(name, LV_ALIGN_LEFT_MID, 190, 0);
    }
    lvgl_mem_log("theme cards");
}

/* Runs after the click that closed SETTINGS has finished (never delete a card
 * from inside its own click handler). */
static void settings_clean_cb(void *unused)
{
    (void)unused;
    if (settings_overlay != NULL && settings_list != NULL &&
        lv_obj_has_flag(settings_overlay, LV_OBJ_FLAG_HIDDEN)) {
        lv_obj_clean(settings_list);
        settings_active_card = NULL;
    }
}

static void settings_show(void)
{
    if (settings_overlay != NULL) {
        if (settings_active_card == NULL) {
            lv_obj_clean(settings_list);            /* start from an empty list */
            if (settings_note_label != NULL) {
                /* Also point to the web settings page. */
                char note[200];
                esp_netif_ip_info_t ip;
                memset(&ip, 0, sizeof(ip));
                esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
                if (netif != NULL && esp_netif_get_ip_info(netif, &ip) == ESP_OK && ip.ip.addr != 0) {
                    snprintf(note, sizeof(note),
                             "Tap a theme to apply it - the display restarts (about 5 seconds).  "
                             "More settings in a browser:  http://" IPSTR "/", IP2STR(&ip.ip));
                } else {
                    snprintf(note, sizeof(note),
                             "Tap a theme to apply it - the display restarts (about 5 seconds).  "
                             "Scroll for more themes.");
                }
                lv_label_set_text(settings_note_label, note);
                lv_obj_set_style_text_color(settings_note_label, lv_color_hex(UI_TEXT_SOFT), 0);
            }
            settings_build_cards();
        }
        ui_show_panel(settings_overlay);
        if (settings_active_card != NULL) {
            lv_obj_scroll_to_view(settings_active_card, LV_ANIM_OFF);   /* show the active theme */
        }
    }
}

static void settings_hide(void)
{
    if (settings_overlay != NULL) {
        lv_obj_add_flag(settings_overlay, LV_OBJ_FLAG_HIDDEN);
        ui_schedule_repaint(lv_screen_active());
        lv_async_call(settings_clean_cb, NULL);
    }
}


static void settings_close_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_CLICKED && !theme_applying) settings_hide();
}

static void theme_restart_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    esp_restart();
}

static void theme_button_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED || theme_applying) return;

    const int index = (int)(intptr_t)lv_event_get_user_data(e);
    if (index < 0 || index >= UI_THEME_COUNT) return;

    if (&ui_themes[index] == ui_theme) {      /* already the active theme */
        settings_hide();
        return;
    }
    if (!ui_theme_save(index)) {
        if (settings_note_label != NULL) {
            lv_label_set_text(settings_note_label, "Could not save the theme - try again.");
        }
        return;
    }

    theme_applying = true;
    if (settings_note_label != NULL) {
        char text[80];
        snprintf(text, sizeof(text), "Applying %s ... the display is restarting.", ui_themes[index].name);
        lv_label_set_text(settings_note_label, text);
        lv_obj_set_style_text_color(settings_note_label, lv_color_hex(UI_ACCENT_GLOW), 0);
    }
    lv_timer_t *timer = lv_timer_create(theme_restart_timer_cb, 900, NULL);
    if (timer != NULL) {
        lv_timer_set_repeat_count(timer, 1);
    }
}

static void create_settings(lv_obj_t *screen)
{
    settings_overlay = lv_obj_create(screen);
    lv_obj_remove_style_all(settings_overlay);
    lv_obj_set_size(settings_overlay, PANEL_W, PANEL_H);
    lv_obj_set_pos(settings_overlay, PANEL_X, PANEL_Y);
    lv_obj_set_style_bg_color(settings_overlay, lv_color_hex(UI_PANEL), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(settings_overlay, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(settings_overlay, 2, LV_PART_MAIN);
    lv_obj_set_style_border_color(settings_overlay, lv_color_hex(UI_AMBER), LV_PART_MAIN);
    lv_obj_set_style_radius(settings_overlay, 16, LV_PART_MAIN);
    lv_obj_clear_flag(settings_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(settings_overlay, LV_OBJ_FLAG_CLICKABLE);      /* swallow stray touches */

    lv_obj_t *title = lv_label_create(settings_overlay);
    lv_label_set_text(title, "SETTINGS  -  COLOUR THEME");
    lv_obj_set_style_text_color(title, lv_color_hex(UI_TEXT), 0);
    lv_obj_set_style_text_font(title, &p4_font_28, 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 30, 20);

    lv_obj_t *rule = lv_obj_create(settings_overlay);
    lv_obj_remove_style_all(rule);
    lv_obj_set_size(rule, 870, 1);
    lv_obj_set_pos(rule, 30, 76);
    lv_obj_set_style_bg_color(rule, lv_color_hex(UI_EDGE_SOFT), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(rule, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_clear_flag(rule, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *close = create_button(settings_overlay, "CLOSE", 150, 50);
    lv_obj_align(close, LV_ALIGN_TOP_RIGHT, -30, 14);
    ui_add_settings_back(settings_overlay);
    lv_obj_add_event_cb(close, settings_close_event_cb, LV_EVENT_CLICKED, NULL);

    /* Scrolling list of theme cards (drag up / down to see them all). */
    settings_list = lv_obj_create(settings_overlay);
    lv_obj_remove_style_all(settings_list);
    lv_obj_set_size(settings_list, 890, PANEL_H - 88 - 76);
    lv_obj_set_pos(settings_list, 20, 88);
    lv_obj_set_style_bg_opa(settings_list, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_add_flag(settings_list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(settings_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(settings_list, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_style_bg_color(settings_list, lv_color_hex(UI_ACCENT), LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(settings_list, LV_OPA_70, LV_PART_SCROLLBAR);
    lv_obj_set_style_width(settings_list, 6, LV_PART_SCROLLBAR);
    lv_obj_set_style_radius(settings_list, 3, LV_PART_SCROLLBAR);

    /* The theme cards are built when SETTINGS is opened and removed again when it
     * closes, so they cost no LVGL memory while the display is just playing music. */

    settings_note_label = lv_label_create(settings_overlay);
    lv_label_set_text(settings_note_label,
                      "Tap a theme to apply it - the display restarts (about 5 seconds).  Scroll for more themes.");
    lv_obj_set_width(settings_note_label, 870);
    lv_obj_set_style_text_color(settings_note_label, lv_color_hex(UI_TEXT_SOFT), 0);
    lv_obj_set_style_text_font(settings_note_label, &p4_font_16, 0);
    lv_obj_set_pos(settings_note_label, 30, PANEL_H - 60);

    lv_obj_add_flag(settings_overlay, LV_OBJ_FLAG_HIDDEN);
}

/* Runs once a second: if SOURCE is open and nobody has touched the screen for a
 * while, return to the now-playing screen. */
