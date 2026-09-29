/*
 * P4-LMS - Button builders shared by every screen
 *
 * Part of main.c.  This file is #included by main.c (in order) and is not
 * compiled on its own, so it shares main.c's includes and static names.
 */

/* ============================================================
 * BUTTON CREATOR
 * ============================================================ */

static lv_obj_t *create_button(
    lv_obj_t *parent,
    const char *text,
    int32_t width,
    int32_t height)
{
    lv_obj_t *button =
        lv_button_create(parent);


    lv_obj_set_size(
        button,
        width,
        height
    );


    /* Keep every source/menu button inside the warm amber receiver theme.
     * Individual controls can still override these defaults. */
    lv_obj_set_style_bg_color(
        button,
        lv_color_hex(UI_BUTTON),
        LV_PART_MAIN
    );

    /* Flat, softly rounded look (no theme drop-shadow). */
    lv_obj_set_style_radius(button, 10, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(button, 0, LV_PART_MAIN);

    /* Press feedback that works for every button colour: the fill fades,
     * the outline lights up and the button sinks by two pixels. */
    lv_obj_set_style_bg_opa(button, LV_OPA_70, LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_border_color(button, lv_color_hex(UI_ACCENT_GLOW), LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_translate_y(button, 2, LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_border_width(
        button,
        1,
        LV_PART_MAIN
    );
    lv_obj_set_style_border_color(
        button,
        lv_color_hex(UI_ACCENT_DARK),
        LV_PART_MAIN
    );

    /* Disabled (e.g. PREV on the first page): clearly dimmed but still
     * readable, instead of LVGL's default pale grey. */
    lv_obj_set_style_bg_color(button, lv_color_hex(UI_PANEL), LV_PART_MAIN | LV_STATE_DISABLED);
    lv_obj_set_style_bg_opa(button, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DISABLED);
    lv_obj_set_style_border_color(button, lv_color_hex(UI_EDGE), LV_PART_MAIN | LV_STATE_DISABLED);
    lv_obj_set_style_text_color(button, lv_color_hex(UI_TEXT_DIM), LV_PART_MAIN | LV_STATE_DISABLED);
    lv_obj_set_style_text_opa(button, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DISABLED);
    /* LVGL's theme also lays a 50% grey wash over disabled objects; that is
     * what made them hard to read, so switch it off. */
    lv_obj_set_style_recolor_opa(button, LV_OPA_TRANSP, LV_PART_MAIN | LV_STATE_DISABLED);


    lv_obj_t *label =
        lv_label_create(button);

    lv_obj_set_style_text_color(
        label,
        lv_color_hex(UI_ACCENT_GLOW),
        0
    );


    lv_label_set_text(
        label,
        text
    );


    lv_obj_center(label);


    return button;
}


static lv_obj_t *create_source_item_button(
    lv_obj_t *parent,
    const char *text,
    int32_t width,
    int32_t height,
    bool is_folder,
    bool is_song)
{
    lv_obj_t *button = create_button(parent, text, width, height);
    lv_obj_t *label = lv_obj_get_child(button, 0);

    /* Rows are left-aligned with a fixed-width, single-line label so a long
     * album or track name ends in "..." instead of spilling over the next
     * button.  Folders and songs get a small icon in the left margin so they
     * are distinguishable without changing their names. */
    int32_t left = (is_folder || is_song) ? 48 : 20;

    if (label != NULL) {
        lv_obj_set_style_text_font(label, &p4_font_16, 0);
        lv_obj_set_width(label, width - left - 16);
        lv_obj_set_height(label, lv_font_get_line_height(&p4_font_16));
        lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_LEFT, 0);
        lv_obj_align(label, LV_ALIGN_LEFT_MID, left, 0);
    }

    if (is_folder) {
        /* Draw a simple folder from LVGL objects so it works reliably
         * with the installed fonts. */
        lv_obj_t *folder_body = lv_obj_create(button);
        lv_obj_remove_style_all(folder_body);
        lv_obj_set_size(folder_body, 24, 16);
        lv_obj_set_style_bg_color(folder_body, lv_color_hex(UI_AMBER), 0);
        lv_obj_set_style_bg_opa(folder_body, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(folder_body, 2, 0);
        lv_obj_align(folder_body, LV_ALIGN_LEFT_MID, 12, 2);

        lv_obj_t *folder_tab = lv_obj_create(button);
        lv_obj_remove_style_all(folder_tab);
        lv_obj_set_size(folder_tab, 10, 5);
        lv_obj_set_style_bg_color(folder_tab, lv_color_hex(UI_AMBER), 0);
        lv_obj_set_style_bg_opa(folder_tab, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(folder_tab, 2, 0);
        lv_obj_align(folder_tab, LV_ALIGN_LEFT_MID, 13, -7);
    } else if (is_song) {
        lv_obj_t *icon = lv_label_create(button);
        lv_label_set_text(icon, LV_SYMBOL_AUDIO);
        lv_obj_set_style_text_color(icon, lv_color_hex(UI_AMBER), 0);
        lv_obj_align(icon, LV_ALIGN_LEFT_MID, 14, 0);
    }

    return button;
}


/* ============================================================
 * PLAYER SELECTOR BUTTON CREATOR
 * ============================================================ */

static lv_obj_t *create_status_dot(lv_obj_t *button)
{
    lv_obj_set_style_pad_all(button, 0, LV_PART_MAIN);
    lv_obj_t *dot = lv_obj_create(button);
    lv_obj_remove_style_all(dot);
    lv_obj_set_size(dot, 9, 9);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(dot, lv_color_hex(0x8C857A), 0);
    lv_obj_set_style_border_width(dot, 1, 0);
    lv_obj_set_style_border_color(dot, lv_color_hex(UI_ON_ACCENT), 0);
    lv_obj_clear_flag(dot, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(dot, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(dot, LV_ALIGN_TOP_RIGHT, -5, 5);
    lv_obj_add_flag(dot, LV_OBJ_FLAG_HIDDEN);
    return dot;
}



