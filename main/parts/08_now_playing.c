/*
 * P4-LMS - Player buttons, HTTP handler, album art, Now Playing updates, status request
 *
 * Part of main.c.  This file is #included by main.c (in order) and is not
 * compiled on its own, so it shares main.c's includes and static names.
 */

/* ============================================================
 * LYRION PLAYER BUTTON VISUALS
 * ============================================================ */

/*
 * Update player-button appearance while the caller already owns the
 * LVGL/display lock.  LVGL event callbacks execute inside the LVGL task,
 * so acquiring bsp_display_lock() again from an event callback can cause
 * a non-recursive lock acquisition failure.
 */
static void update_player_button_styles_locked(void)
{
    for (int i = 0;
         i < (int)LYRION_PLAYER_COUNT;
         i++) {

        if (player_buttons[i] == NULL) {
            continue;
        }

        lv_obj_t *label =
            lv_obj_get_child(
                player_buttons[i],
                0
            );

        if (i == current_player_index) {
            lv_obj_set_style_bg_color(
                player_buttons[i],
                lv_color_hex(UI_ACCENT),
                LV_PART_MAIN
            );

            if (label != NULL) {
                lv_obj_set_style_text_color(
                    label,
                    lv_color_hex(UI_ON_ACCENT),
                    0
                );
            }
        } else {
            lv_obj_set_style_bg_color(
                player_buttons[i],
                lv_color_hex(UI_BUTTON),
                LV_PART_MAIN
            );

            if (label != NULL) {
                lv_obj_set_style_text_color(
                    label,
                    lv_color_hex(UI_TEXT),
                    0
                );
            }
        }
    }
}

/* ============================================================
 * LYRION PLAYER SELECTOR
 * ============================================================ */

/* Make `index` the selected player (LVGL task). */
static void player_select(int index)
{
    if (index < 0 ||
        index >= (int)LYRION_PLAYER_COUNT) {

        return;
    }


    current_player_index = index;

    save_selected_player(index);

    /* Force the next status response to load artwork for the newly selected
     * player, even if artist/title/album are identical on both players. */
    last_album_art_key[0] = '\0';

    ESP_LOGI(
        TAG,
        "Selected player: %s",
        lyrion_players[index].name
    );


    /* This callback already runs in LVGL's locked context.  Do not try to
     * acquire the display mutex again here. */
    update_player_button_styles_locked();
    np_power_room_changed();


    /*
     * Force the status task to query the newly selected
     * player immediately instead of waiting for the next
     * three-second polling interval.
     */
    if (lyrion_refresh_event != NULL) {
        xEventGroupSetBits(
            lyrion_refresh_event,
            LYRION_REFRESH_BIT
        );
    }
}



/* ============================================================
 * LYRION HTTP RESPONSE EVENT HANDLER
 * ============================================================ */

static esp_err_t lyrion_http_event_handler(
    esp_http_client_event_t *evt)
{
    lyrion_response_t *response =
        (lyrion_response_t *)evt->user_data;


    if (response == NULL) {
        return ESP_OK;
    }


    switch (evt->event_id) {

        case HTTP_EVENT_ON_DATA:

            if (evt->data_len > 0 &&
                response->data != NULL &&
                response->capacity > 1 &&
                response->length <
                    response->capacity - 1) {

                size_t space_left =
                    (response->capacity - 1) -
                    response->length;


                size_t copy_len =
                    (size_t)evt->data_len;


                if (copy_len > space_left) {
                    copy_len = space_left;
                    response->truncated = true;
                }


                memcpy(
                    response->data +
                        response->length,
                    evt->data,
                    copy_len
                );


                response->length += copy_len;


                response->data[
                    response->length
                ] = '\0';
            }

            break;


        default:
            break;
    }


    return ESP_OK;
}


/* ============================================================
 * FORMAT TIME
 * ============================================================ */

static void format_time_value(
    double seconds,
    char *buffer,
    size_t buffer_size)
{
    if (seconds < 0) {
        seconds = 0;
    }


    int total_seconds =
        (int)(seconds + 0.5);


    int minutes =
        total_seconds / 60;


    int secs =
        total_seconds % 60;


    snprintf(
        buffer,
        buffer_size,
        "%d:%02d",
        minutes,
        secs
    );
}


/* ============================================================
 * ALBUM ART
 * ============================================================ */

/* HTTP status of the last cover download (404: the server has no picture). */
static int album_art_last_status = 0;

static void clear_album_art_job(void *arg)
{
    (void)arg;

    if (album_image != NULL) {
        lv_image_set_src(album_image, NULL);
        lv_obj_add_flag(album_image, LV_OBJ_FLAG_HIDDEN);
    }

    if (album_placeholder != NULL) {
        lv_label_set_text(album_placeholder, "ALBUM ART");
        lv_obj_clear_flag(album_placeholder, LV_OBJ_FLAG_HIDDEN);
    }
    screensaver_cover_changed(false);
}

static void clear_album_art(void)
{
    ui_post(clear_album_art_job, NULL, 0);
}


/* True for progressive JPEGs, which the hardware decoder cannot handle. */
static bool jpeg_is_progressive(const uint8_t *d, size_t n)
{
    size_t i = 2;
    while (i + 4 <= n) {
        if (d[i] != 0xFF) { i++; continue; }
        uint8_t m = d[i + 1];
        if (m == 0xFF) { i++; continue; }
        if (m == 0xD8 || m == 0x01 || (m >= 0xD0 && m <= 0xD7)) { i += 2; continue; }
        if (m == 0xC2 || m == 0xC6 || m == 0xCA || m == 0xCE) return true;
        if (m >= 0xC0 && m <= 0xCF && m != 0xC4 && m != 0xC8 && m != 0xCC) return false;
        if (m == 0xDA || m == 0xD9) return false;
        size_t len = ((size_t)d[i + 2] << 8) | d[i + 3];
        if (len < 2) return false;
        i += 2 + len;
    }
    return false;
}

static bool update_album_art_fetch(const char *player_id, bool want_png)
{
    if (player_id == NULL || player_id[0] == '\0') {
        return false;
    }

    /*
     * Lyrion provides a player-specific current-cover URL.  The
     * resize suffix keeps the downloaded image close to the size
     * of our 370x370 display area.
     */
    char url[256];
    snprintf(
        url,
        sizeof(url),
        "%s/music/current/cover_%dx%d.%s?player=%s",
        LYRION_SERVER_BASE,
        ALBUM_ART_WIDTH,
        ALBUM_ART_HEIGHT,
        want_png ? "png" : "jpg",
        player_id
    );

    uint8_t *jpeg_buffer = NULL;
    size_t jpeg_buffer_size = 0;

    jpeg_decode_memory_alloc_cfg_t input_mem_cfg = {
        .buffer_direction = JPEG_DEC_ALLOC_INPUT_BUFFER,
    };

    /* One download buffer, kept: allocating (and freeing) 2 MB of DMA memory
     * for every cover fragmented PSRAM.  Only the status task gets here. */
    static uint8_t *art_in_buf = NULL;
    static size_t   art_in_size = 0;
    if (art_in_buf == NULL) {
        art_in_buf = (uint8_t *)jpeg_alloc_decoder_mem(ALBUM_ART_MAX_JPEG_SIZE, &input_mem_cfg, &art_in_size);
    }
    jpeg_buffer = art_in_buf;
    jpeg_buffer_size = art_in_size;

    if (jpeg_buffer == NULL) {
        ESP_LOGE(TAG, "Could not allocate JPEG input buffer");
        return false;
    }

    typedef struct {
        uint8_t *data;
        size_t length;
        size_t capacity;
    } album_http_response_t;

    album_http_response_t response = {
        .data = jpeg_buffer,
        .length = 0,
        .capacity = jpeg_buffer_size,
    };

    /* Local HTTP event handler for the JPEG stream. */
    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_GET,
        .timeout_ms = 5000,          /* the server is on the LAN: a slow cover gives up sooner */
        .buffer_size = 4096,
        .buffer_size_tx = 1024,
        .disable_auto_redirect = false,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        ESP_LOGE(TAG, "Could not create album-art HTTP client");
        return false;
    }

    /* Use the client's open/fetch API so the response can be copied into
     * the JPEG-DMA-compatible buffer without another large allocation. */
    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Album-art HTTP open failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return false;
    }

    int content_length = esp_http_client_fetch_headers(client);
    if (content_length > (int)response.capacity) {
        ESP_LOGW(TAG, "Album art is too large: %d bytes", content_length);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return false;
    }

    while (1) {
        int read_len = esp_http_client_read(
            client,
            (char *)response.data + response.length,
            response.capacity - response.length
        );

        if (read_len < 0) {
            ESP_LOGW(TAG, "Album-art HTTP read failed");
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            return false;
        }

        if (read_len == 0) {
            break;
        }

        response.length += (size_t)read_len;

        if (response.length >= response.capacity) {
            ESP_LOGW(TAG, "Album-art JPEG exceeded %u bytes", (unsigned)response.capacity);
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            return false;
        }
    }

    int status_code = esp_http_client_get_status_code(client);

    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    album_art_last_status = status_code;
    if (status_code != 200 || response.length < 4) {
        ESP_LOGW(
            TAG,
            "Album-art HTTP status=%d size=%u",
            status_code,
            (unsigned)response.length
        );
        return false;
    }

    /* Lyrion can return PNG data even when the URL ends in .jpg.
     * Detect the actual file signature before invoking the JPEG decoder. */
    if (response.length >= 8 &&
        response.data[0] == 0x89 && response.data[1] == 0x50 &&
        response.data[2] == 0x4E && response.data[3] == 0x47 &&
        response.data[4] == 0x0D && response.data[5] == 0x0A &&
        response.data[6] == 0x1A && response.data[7] == 0x0A) {
        ESP_LOGI(TAG, "Album art format: PNG");
        bool ok = decode_png_to_rgb565(response.data, response.length);
        return ok;
    }

    /* Sanity-check the JPEG before it reaches the hardware decoder: a cut-off
     * download or an unsupported (progressive) file decodes into garbage. */
    {
        const uint8_t *d = response.data;
        const size_t n = response.length;
        bool sane = (n > 4 && d[0] == 0xFF && d[1] == 0xD8);

        if (sane && content_length > 0 && (size_t)content_length != n) {
            ESP_LOGW(TAG, "Album-art JPEG incomplete: got %u of %d bytes",
                     (unsigned)n, content_length);
            sane = false;
        }
        if (sane) {
            size_t e = n;
            int pad = 0;
            while (e > 2 && d[e - 1] == 0x00 && pad < 16) { e--; pad++; }
            if (!(d[e - 2] == 0xFF && d[e - 1] == 0xD9)) {
                ESP_LOGW(TAG, "Album-art JPEG has no end marker (cut-off download?)");
                sane = false;
            }
        }
        if (sane && jpeg_is_progressive(d, n)) {
            ESP_LOGW(TAG, "Album-art JPEG is progressive; hardware decoder cannot use it");
            sane = false;
        }
        if (!sane) {
            return false;
        }
        ESP_LOGI(TAG, "Album-art JPEG: %u bytes", (unsigned)n);
    }

    jpeg_decode_picture_info_t picture_info;
    memset(&picture_info, 0, sizeof(picture_info));

    err = jpeg_decoder_get_info(
        jpeg_buffer,
        (uint32_t)response.length,
        &picture_info
    );

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "JPEG header decode failed: %s", esp_err_to_name(err));
        return false;
    }

    ESP_LOGI(
        TAG,
        "JPEG reported dimensions: %ux%u",
        (unsigned)picture_info.width,
        (unsigned)picture_info.height
    );

    if (picture_info.width == 0 ||
        picture_info.height == 0 ||
        picture_info.width > 1024 ||
        picture_info.height > 1024) {

        ESP_LOGW(
            TAG,
            "Rejecting invalid JPEG dimensions: %ux%u",
            (unsigned)picture_info.width,
            (unsigned)picture_info.height
        );

        return false;
    }

    uint32_t padded_width = (picture_info.width + 15U) & ~15U;
    uint32_t padded_height = (picture_info.height + 15U) & ~15U;
    size_t required_pixels =
        (size_t)padded_width * (size_t)padded_height * 2U;

    jpeg_decode_memory_alloc_cfg_t output_mem_cfg = {
        .buffer_direction = JPEG_DEC_ALLOC_OUTPUT_BUFFER,
    };

    size_t output_allocated = 0;
    uint8_t *new_pixels = (uint8_t *)jpeg_alloc_decoder_mem(
        required_pixels,
        &output_mem_cfg,
        &output_allocated
    );

    if (new_pixels == NULL) {
        ESP_LOGW(
            TAG,
            "Could not allocate JPEG output buffer (%u bytes)",
            (unsigned)required_pixels
        );
        return false;
    }

    /* The LVGL display uses RGB565, so decode directly to RGB565. */
    jpeg_decode_cfg_t decode_cfg = {
    .output_format = JPEG_DECODE_OUT_FORMAT_RGB565,
    .rgb_order = JPEG_DEC_RGB_ELEMENT_ORDER_BGR,
};

    jpeg_decoder_handle_t decoder = NULL;
    jpeg_decode_engine_cfg_t engine_cfg = {
        .intr_priority = 0,
        .timeout_ms = 100,
    };

    err = jpeg_new_decoder_engine(&engine_cfg, &decoder);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Could not create JPEG decoder: %s", esp_err_to_name(err));
        heap_caps_free(new_pixels);
        return false;
    }

    uint32_t decoded_size = 0;

    err = jpeg_decoder_process(
        decoder,
        &decode_cfg,
        jpeg_buffer,
        (uint32_t)response.length,
        new_pixels,
        (uint32_t)output_allocated,
        &decoded_size
    );

    jpeg_del_decoder_engine(decoder);

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "JPEG decode failed: %s", esp_err_to_name(err));
        heap_caps_free(new_pixels);
        return false;
    }

    /* Replace the previous decoded image only after a complete successful
     * decode. This prevents a bad download from destroying the last good cover. */
    if (!album_art_show(new_pixels, picture_info.width, picture_info.height,
                        padded_width, decoded_size)) {
        heap_caps_free(new_pixels);
        return false;
    }

    ESP_LOGI(
        TAG,
        "Album art loaded: %ux%u (%u bytes)",
        (unsigned)picture_info.width,
        (unsigned)picture_info.height,
        (unsigned)decoded_size
    );

    return true;
}

/* Fetch the current cover, falling back to the other image format if the first
 * attempt fails (network hiccup, truncated file, unsupported JPEG...). */
static void favorites_request_check(void);   /* in part 15 */

static bool update_album_art(const char *player_id)
{
    const bool first_png = (ALBUM_ART_FORCE_PNG != 0);

    if (update_album_art_fetch(player_id, first_png)) {
        return true;
    }
    ESP_LOGW(TAG, "Album art: retrying as %s", first_png ? "JPEG" : "PNG");
    return update_album_art_fetch(player_id, !first_png);
}


/* ============================================================
 * UPDATE LYRION UI
 * ============================================================ */

static void source_clean_display_text(const char *input, char *output, size_t output_size);

/*
 * Play/pause button and the PLAYING/PAUSED chip.  The button shows the action
 * it will perform (pause icon while playing, play icon while paused).
 *
 * The caller must already hold the LVGL lock (or be inside an LVGL callback).
 */
static void ui_apply_play_state_locked(bool playing)
{
    int wanted = playing ? 1 : 0;

    if (ui_play_state_applied == wanted) {
        return;
    }
    ui_play_state_applied = wanted;

    if (play_button != NULL) {
        lv_obj_t *label = lv_obj_get_child(play_button, 0);

        if (label != NULL) {
            lv_label_set_text(label, playing ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
            lv_obj_set_style_text_color(
                label,
                lv_color_hex(playing ? UI_ON_ACCENT : UI_ACCENT_BRIGHT),
                0
            );
        }

        lv_obj_set_style_bg_color(
            play_button,
            lv_color_hex(playing ? UI_ACCENT : UI_ACCENT_DARK),
            LV_PART_MAIN
        );
    }

    if (play_state_label != NULL) {
        lv_label_set_text(
            play_state_label,
            playing ? LV_SYMBOL_PLAY "  PLAYING" : LV_SYMBOL_PAUSE "  PAUSED"
        );
        lv_obj_set_style_text_color(
            play_state_label,
            lv_color_hex(playing ? UI_TERTIARY : UI_TEXT_DIM),
            0
        );
    }
}


/* Shuffle / repeat button looks (LVGL lock held by the caller). */
static void ui_apply_mode_buttons_locked(void)
{
    const int sh = shuffle_mode;
    const int rp = repeat_mode;

    if (shuffle_button != NULL && ui_shuffle_applied != sh) {
        ui_shuffle_applied = sh;
        const bool on = (sh != 0);
        lv_obj_t *label = lv_obj_get_child(shuffle_button, 0);
        if (label != NULL) {
            lv_label_set_text(label, sh == 2 ? LV_SYMBOL_SHUFFLE " A" : LV_SYMBOL_SHUFFLE);
            lv_obj_set_style_text_color(label, lv_color_hex(on ? UI_ON_ACCENT : UI_TEXT_DIM), 0);
        }
        lv_obj_set_style_bg_color(shuffle_button, lv_color_hex(on ? UI_ACCENT : UI_BUTTON), LV_PART_MAIN);
        lv_obj_set_style_border_color(shuffle_button, lv_color_hex(on ? UI_ACCENT_GLOW : UI_EDGE), LV_PART_MAIN);
    }

    if (repeat_button != NULL && ui_repeat_applied != rp) {
        ui_repeat_applied = rp;
        const bool on = (rp != 0);
        lv_obj_t *label = lv_obj_get_child(repeat_button, 0);
        if (label != NULL) {
            lv_label_set_text(label, rp == 1 ? LV_SYMBOL_LOOP " 1" : LV_SYMBOL_LOOP);
            lv_obj_set_style_text_color(label, lv_color_hex(on ? UI_ON_ACCENT : UI_TEXT_DIM), 0);
        }
        lv_obj_set_style_bg_color(repeat_button, lv_color_hex(on ? UI_ACCENT : UI_BUTTON), LV_PART_MAIN);
        lv_obj_set_style_border_color(repeat_button, lv_color_hex(on ? UI_ACCENT_GLOW : UI_EDGE), LV_PART_MAIN);
    }
}

/* Small status dot on each player button: green = playing, grey = on but idle,
 * none = off or unreachable (LVGL lock held by the caller). */
static void ui_apply_house_status_locked(void)
{
    for (int i = 0; i < (int)LYRION_PLAYER_COUNT; i++) {
        lv_obj_t *dot = player_status_dots[i];
        if (dot == NULL) {
            continue;
        }

        int state = house_state[i];
        /* The selected player is polled directly and more often. */
        if (i == current_player_index && state != 0 && lyrion_fail_count < 2) {
            state = current_player_playing ? 2 : 1;
        }

        if (ui_house_applied[i] == state) {
            continue;
        }
        ui_house_applied[i] = state;

        if (state == 0) {
            lv_obj_add_flag(dot, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_set_style_bg_color(dot, lv_color_hex(state == 2 ? 0x5FD068 : 0x8C857A), 0);
            lv_obj_clear_flag(dot, LV_OBJ_FLAG_HIDDEN);
        }
    }
}


/* Elapsed / total labels and progress bar (LVGL lock held by the caller). */
static void playback_render_locked(double elapsed, double duration)
{
    char text[24];                           /* room for "-" + the longest time */

    format_time_value(elapsed, text, sizeof(text));
    if (duration > 0.0) {
        ui_set_label(elapsed_label, text);
    } else {
        /* Radio (no length): "LIVE" and the time listened, no bar. */
        char live[40];
        snprintf(live, sizeof(live), "LIVE  %s", text);
        ui_set_label(elapsed_label, ss_have_track ? live : "");
    }

    /* The right-hand time counts down what's left of the song. */
    if (duration > 0.0) {
        double left = duration - elapsed;
        if (left < 0.0) left = 0.0;
        char t[16];
        format_time_value(left, t, sizeof(t));
        snprintf(text, sizeof(text), "-%s", t);
        ui_set_label(total_label, text);
    } else {
        ui_set_label(total_label, "");
    }

    if (progress_bar != NULL) {
        int permille = 0;

        if (duration > 0.0) {
            permille = (int)((elapsed / duration) * 1000.0);
            if (permille < 0) permille = 0;
            if (permille > 1000) permille = 1000;
        }

        if (lv_bar_get_value(progress_bar) != permille) {
            lv_bar_set_value(progress_bar, permille, LV_ANIM_OFF);
        }
        if (duration > 0.0) lv_obj_clear_flag(progress_bar, LV_OBJ_FLAG_HIDDEN);
        else                lv_obj_add_flag(progress_bar, LV_OBJ_FLAG_HIDDEN);

        /* Handle follows the bar; hidden when there's no length (radio). */
        if (progress_knob != NULL) {
            if (duration > 0.0) {
                const int32_t bx = lv_obj_get_style_x(progress_bar, LV_PART_MAIN);
                const int32_t by = lv_obj_get_style_y(progress_bar, LV_PART_MAIN);
                const int32_t bw = lv_obj_get_style_width(progress_bar, LV_PART_MAIN);
                const int32_t bh = lv_obj_get_style_height(progress_bar, LV_PART_MAIN);
                const int32_t kx = bx + (int32_t)((int64_t)bw * permille / 1000) - PROGRESS_KNOB_PX / 2;
                const int32_t ky = by + bh / 2 - PROGRESS_KNOB_PX / 2;
                if (lv_obj_get_style_x(progress_knob, LV_PART_MAIN) != kx ||
                    lv_obj_get_style_y(progress_knob, LV_PART_MAIN) != ky) {
                    lv_obj_set_pos(progress_knob, kx, ky);
                }
                lv_obj_clear_flag(progress_knob, LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_add_flag(progress_knob, LV_OBJ_FLAG_HIDDEN);
            }
        }
    }
}


/* Runs inside LVGL every 250 ms: advance the clock between server polls. */
static void playback_tick_cb(lv_timer_t *timer)
{
    (void)timer;

    if (!current_player_playing || !playback_valid || seek_dragging ||
        visualizer_visible || screensaver_active) {
        return;
    }

    double elapsed = playback_elapsed_s +
                     (double)lv_tick_elaps(playback_sync_tick) / 1000.0;

    if (playback_duration_s > 0.0 && elapsed > playback_duration_s) {
        elapsed = playback_duration_s;
    }

    playback_render_locked(elapsed, playback_duration_s);
}


static void volume_mute_sync_locked(int server_volume);   /* 11_transport_controls.c */

/* Sync group of the room on screen, from its status (18b_navigation.c). */
typedef struct { int room; int mask; } np_group_msg_t;
static void nav_room_group_job(void *arg);

/* Runs on the LVGL task (queued by update_lyrion_ui below). */
static void update_lyrion_ui_now(
    const char *artist,
    const char *title,
    const char *album,
    const char *mode,
    double elapsed,
    double duration,
    int volume,
    int album_id)
{

    char clean_artist[SOURCE_TEXT_MAX];
    char clean_title[SOURCE_TEXT_MAX];
    char clean_album[SOURCE_TEXT_MAX];
    source_clean_display_text(artist, clean_artist, sizeof(clean_artist));
    source_clean_display_text(title, clean_title, sizeof(clean_title));
    source_clean_display_text(album, clean_album, sizeof(clean_album));

    bool have_title  = (title  != NULL && title[0]  != '\0');
    bool have_artist = (artist != NULL && artist[0] != '\0');
    bool have_album  = (album  != NULL && album[0]  != '\0');

    /* --------------------------------------------------------
     * TRACK TEXT
     * -------------------------------------------------------- */
    /* Nothing to show means the room's queue is empty: say what to do. */
    const char *new_title = have_title ? clean_title : "Queue empty \u2013 tap Library";
    const bool title_changed = song_label != NULL &&
                               strcmp(lv_label_get_text(song_label), new_title) != 0;
    ui_set_label(song_label, new_title);
    ui_set_label(artist_label,
                 have_artist ? clean_artist : (have_title ? "Unknown Artist" : ""));
    ui_set_label(album_name_label, have_album ? clean_album : "");
    if (title_changed) {                    /* next song: the text fades in */
        ui_fade_in_text(song_label);
        ui_fade_in_text(artist_label);
        ui_fade_in_text(album_name_label);
    }

    playback_album_id = album_id;         /* what a tap on the album art opens */
    screensaver_set_track(have_title ? clean_title : NULL, have_artist ? clean_artist : "",
                          have_album ? clean_album : "", mode);

    if (visualizer_track_label != NULL) {
        char line[SOURCE_TEXT_MAX * 2 + 8];

        if (have_title && have_artist) {
            snprintf(line, sizeof(line), "%s  -  %s", clean_artist, clean_title);
        } else if (have_title) {
            snprintf(line, sizeof(line), "%s", clean_title);
        } else {
            line[0] = '\0';
        }

        ui_set_label(visualizer_track_label, line);
    }

    /* --------------------------------------------------------
     * PLAYBACK STATE
     * -------------------------------------------------------- */
    bool new_playing = (mode != NULL && strcmp(mode, "play") == 0);

    if (new_playing != current_player_playing) {
        screensaver_last_state_change_ms = esp_log_timestamp();

        if (new_playing && screensaver_active) {
            screensaver_hide();
        }
    }

    current_player_playing = new_playing;
    ui_apply_play_state_locked(new_playing);

    /* --------------------------------------------------------
     * ELAPSED / TOTAL / PROGRESS
     * -------------------------------------------------------- */
    /* While the user is dragging the progress bar (or has just let go), keep
     * showing their position instead of the server's older one. */
    const bool seek_hold = seek_dragging ||
        (seek_release_pending && lv_tick_elaps(seek_release_tick) < 1200);
    if (!seek_hold) {
        seek_release_pending = false;
    }

    playback_duration_s = duration;
    playback_valid = true;
    if (!seek_hold) {
        playback_elapsed_s = elapsed;
        playback_sync_tick = lv_tick_get();
        playback_render_locked(elapsed, duration);
    }

    /* --------------------------------------------------------
     * VOLUME
     *
     * Leave the slider alone while the user is dragging it, or has just
     * let go: the server may not have applied the newest value yet and the
     * knob would jump backwards.
     * -------------------------------------------------------- */
    bool user_is_busy =
        (volume_slider != NULL && lv_slider_is_dragged(volume_slider)) ||
        (volume_user_touched && lv_tick_elaps(volume_last_user_tick) < 1500);

    if (!user_is_busy) {
        if (volume < 0) {
            volume = 0;
        }

        if (volume > 100) {
            volume = 100;
        }

        /* Suppress the slider callback while we update the slider from the
         * actual Lyrion volume. */
        updating_volume_from_server = true;

        if (volume_slider != NULL &&
            lv_slider_get_value(volume_slider) != volume) {
            lv_slider_set_value(volume_slider, volume, LV_ANIM_OFF);
        }

        if (volume_value_label != NULL) {
            char volume_text[16];
            snprintf(volume_text, sizeof(volume_text), "%d%%", volume);
            ui_set_label(volume_value_label, volume_text);
        }

        updating_volume_from_server = false;
        volume_mute_sync_locked(volume);
    }

    ui_apply_house_status_locked();

}

typedef struct {
    char artist[256];
    char title[256];
    char album[256];
    char mode[16];
    double elapsed;
    double duration;
    int volume;
    int album_id;
} now_playing_msg_t;

static void update_lyrion_ui_job(void *arg)
{
    const now_playing_msg_t *m = (const now_playing_msg_t *)arg;
    update_lyrion_ui_now(m->artist, m->title, m->album, m->mode,
                         m->elapsed, m->duration, m->volume, m->album_id);
}

/* Called by the status task: hand the new track info to the LVGL task. */
static void update_lyrion_ui(
    const char *player_name,
    const char *artist,
    const char *title,
    const char *album,
    const char *mode,
    double elapsed,
    double duration,
    int volume,
    int album_id)
{
    (void)player_name;
    now_playing_msg_t m;
    snprintf(m.artist, sizeof(m.artist), "%s", artist ? artist : "");
    snprintf(m.title, sizeof(m.title), "%s", title ? title : "");
    snprintf(m.album, sizeof(m.album), "%s", album ? album : "");
    snprintf(m.mode, sizeof(m.mode), "%s", mode ? mode : "");
    m.elapsed = elapsed;
    m.duration = duration;
    m.volume = volume;
    m.album_id = album_id;
    ui_post(update_lyrion_ui_job, &m, sizeof(m));
}


/* ============================================================
 * SHOW LYRION OFFLINE STATE
 * ============================================================ */

static void update_lyrion_offline_ui_job(void *arg)
{
    (void)arg;

    ui_set_label(song_label, "Lyrion offline");
    if (format_label != NULL) {
        ui_set_label(format_label, "");
    }
    for (int i = 0; i < (int)LYRION_PLAYER_COUNT; i++) {
        house_state[i] = 0;
    }
    ui_apply_house_status_locked();
    ui_set_label(artist_label, "Waiting for server...");
    ui_set_label(album_name_label, "");

    playback_valid = false;
    current_player_playing = false;
    ui_play_state_applied = -1;
    ui_apply_play_state_locked(false);
    playback_album_id = 0;

    if (play_state_label != NULL) {
        ui_set_label(play_state_label, LV_SYMBOL_CLOSE "  OFFLINE");
        lv_obj_set_style_text_color(play_state_label, lv_color_hex(UI_RED), 0);
    }

    /* Force the chip to be redrawn the next time a status arrives. */
    ui_play_state_applied = -1;
}

static void update_lyrion_offline_ui(void)
{
    ui_post(update_lyrion_offline_ui_job, NULL, 0);
    last_album_art_key[0] = '\0';
    clear_album_art();
}


/* ============================================================
 * LYRION STATUS REQUEST
 * ============================================================ */

/* ------------------------------------------------------------------------
 * Audio-format badge: "FLAC  |  44.1 kHz  |  16-bit  |  987kbps VBR"
 * ------------------------------------------------------------------------ */
static void format_append(char *out, size_t out_size, const char *part)
{
    if (part == NULL || part[0] == '\0') return;
    size_t len = strlen(out);
    if (len > 0) {
        snprintf(out + len, out_size - len, "  |  %s", part);
    } else {
        snprintf(out, out_size, "%s", part);
    }
}

static void format_build_text(const cJSON *track, char *out, size_t out_size, bool *lossless)
{
    out[0] = '\0';
    *lossless = false;
    if (track == NULL) return;

    /* Container / codec: first word of the type, e.g. "flc" or "MP3 Radio". */
    char tok[12] = "";
    const cJSON *type_j = cJSON_GetObjectItem(track, "type");
    if (cJSON_IsString(type_j) && type_j->valuestring != NULL) {
        size_t n = 0;
        for (const char *p = type_j->valuestring; *p && n < sizeof(tok) - 1; p++) {
            if (isalnum((unsigned char)*p)) {
                tok[n++] = (char)tolower((unsigned char)*p);
            } else if (n > 0) {
                break;
            }
        }
        tok[n] = '\0';
    }

    static const struct { const char *tok; const char *name; bool lossless; } codecs[] = {
        { "flc", "FLAC", true },   { "flac", "FLAC", true },
        { "alc", "ALAC", true },   { "alac", "ALAC", true },
        { "wav", "WAV", true },    { "wave", "WAV", true },  { "pcm", "PCM", true },
        { "aif", "AIFF", true },   { "aiff", "AIFF", true },
        { "ape", "APE", true },    { "wvp", "WavPack", true }, { "wavpack", "WavPack", true },
        { "dsf", "DSD", true },    { "dff", "DSD", true },   { "dsd", "DSD", true },
        { "mp3", "MP3", false },   { "aac", "AAC", false },  { "mp4", "AAC", false },
        { "ogg", "OGG", false },   { "ogf", "OGG", false },  { "opus", "Opus", false },
        { "wma", "WMA", false },
    };
    char name_buf[12] = "";
    const char *codec = NULL;
    for (size_t i = 0; i < sizeof(codecs) / sizeof(codecs[0]); i++) {
        if (strcmp(tok, codecs[i].tok) == 0) {
            codec = codecs[i].name;
            *lossless = codecs[i].lossless;
            break;
        }
    }
    if (codec == NULL && tok[0] != '\0') {
        for (size_t i = 0; tok[i] != '\0' && i < sizeof(name_buf) - 1; i++) {
            name_buf[i] = (char)toupper((unsigned char)tok[i]);
        }
        codec = name_buf;
    }
    format_append(out, out_size, codec);

    /* Sample rate and bit depth (LMS sends numbers or numeric strings). */
    const cJSON *sr_j = cJSON_GetObjectItem(track, "samplerate");
    double rate = 0.0;
    if (cJSON_IsNumber(sr_j)) rate = sr_j->valuedouble;
    else if (cJSON_IsString(sr_j) && sr_j->valuestring) rate = atof(sr_j->valuestring);
    if (rate >= 1000.0) {
        char part[24];
        if (rate >= 1000000.0) {
            snprintf(part, sizeof(part), "%.1f MHz", rate / 1000000.0);
        } else {
            double khz = rate / 1000.0;
            if (fabs(khz - floor(khz + 0.5)) < 0.05) {
                snprintf(part, sizeof(part), "%d kHz", (int)(khz + 0.5));
            } else {
                snprintf(part, sizeof(part), "%.1f kHz", khz);
            }
        }
        format_append(out, out_size, part);
    }

    const cJSON *ss_j = cJSON_GetObjectItem(track, "samplesize");
    int bits = 0;
    if (cJSON_IsNumber(ss_j)) bits = ss_j->valueint;
    else if (cJSON_IsString(ss_j) && ss_j->valuestring) bits = atoi(ss_j->valuestring);
    if (bits > 0 && bits <= 64) {
        char part[16];
        snprintf(part, sizeof(part), "%d-bit", bits);
        format_append(out, out_size, part);
    }

    const cJSON *br_j = cJSON_GetObjectItem(track, "bitrate");
    if (cJSON_IsString(br_j) && br_j->valuestring != NULL && br_j->valuestring[0] != '\0') {
        char part[20];
        snprintf(part, sizeof(part), "%.18s", br_j->valuestring);
        format_append(out, out_size, part);
    }
}

/* Format badge + shuffle / repeat state, after each status poll. */
static void update_lyrion_extras_ui_now(const char *format_text, bool lossless,
                                        int shuffle, int repeat, bool live)
{
    /* A live stream has no order to shuffle or repeat: hide both. */
    lv_obj_t *mode_btns[2] = { shuffle_button, repeat_button };
    for (int i = 0; i < 2; i++) {
        if (mode_btns[i] == NULL) continue;
        if (live) lv_obj_add_flag(mode_btns[i], LV_OBJ_FLAG_HIDDEN);
        else      lv_obj_clear_flag(mode_btns[i], LV_OBJ_FLAG_HIDDEN);
    }

    if (format_label != NULL) {
        ui_set_label(format_label, format_text != NULL ? format_text : "");
        if (ui_format_lossless != (lossless ? 1 : 0)) {
            ui_format_lossless = lossless ? 1 : 0;
            lv_obj_set_style_text_color(
                format_label, lv_color_hex(lossless ? UI_ACCENT : UI_TEXT_DIM), 0);
        }
    }

    /* Do not undo a tap that the server has not applied yet. */
    const bool user_busy = mode_user_touched && lv_tick_elaps(mode_user_tick) < 1500;
    if (!user_busy) {
        if (shuffle >= 0 && shuffle <= 2) shuffle_mode = shuffle;
        if (repeat >= 0 && repeat <= 2) repeat_mode = repeat;
    }
    ui_apply_mode_buttons_locked();

}

typedef struct { char format[96]; bool lossless; int shuffle; int repeat; bool live; } extras_msg_t;

static void update_lyrion_extras_ui_job(void *arg)
{
    const extras_msg_t *m = (const extras_msg_t *)arg;
    update_lyrion_extras_ui_now(m->format, m->lossless, m->shuffle, m->repeat, m->live);
}

static void update_lyrion_extras_ui(const char *format_text, bool lossless,
                                    int shuffle, int repeat, bool live)
{
    extras_msg_t m;
    m.live = live;
    snprintf(m.format, sizeof(m.format), "%s", format_text ? format_text : "");
    m.lossless = lossless;
    m.shuffle = shuffle;
    m.repeat = repeat;
    ui_post(update_lyrion_extras_ui_job, &m, sizeof(m));
}

static void queue_mark_playing_job(void *arg);   /* 14_power_and_browser_rows.c */

static bool http_post_json_ex(const char *url, const char *command_json,
                              char *response_buffer, size_t response_size,
                              int timeout_ms);   /* 14 */

static bool lyrion_get_status(void)
{
    int player_index =
        current_player_index;


    if (player_index < 0 ||
        player_index >=
            (int)LYRION_PLAYER_COUNT) {

        player_index = 0;
        current_player_index = 0;
    }


    const char *player_id =
        lyrion_players[
            player_index
        ].id;


    const char *player_name =
        lyrion_players[
            player_index
        ].name;


    /* --------------------------------------------------------
     * JSON REQUEST
     * -------------------------------------------------------- */

    char json_request[512];


    snprintf(
        json_request,
        sizeof(json_request),

        "{"
        "\"id\":1,"
        "\"method\":\"slim.request\","
        "\"params\":["
        "\"%s\","
        "[\"status\",\"-\",\"1\",\"tags:aclKNorTIeu\"]"
        "]"
        "}",

        player_id
    );


    /* POST it (5 s: the server is on the LAN; a dead one shouldn't stall the poll) */
    if (!http_post_json_ex(LYRION_SERVER_URL, json_request,
                           lyrion_response_buffer, LYRION_RESPONSE_MAX, 5000)) {
        return false;
    }


    /* --------------------------------------------------------
     * PARSE JSON
     * -------------------------------------------------------- */

    cJSON *root =
        cJSON_Parse(
            lyrion_response_buffer
        );


    if (root == NULL) {

        ESP_LOGE(
            TAG,
            "Could not parse Lyrion JSON"
        );




        return false;
    }


    cJSON *result =
        cJSON_GetObjectItem(
            root,
            "result"
        );


    if (result == NULL ||
        !cJSON_IsObject(result)) {

        ESP_LOGE(
            TAG,
            "Lyrion response has no result object"
        );


        cJSON_Delete(root);



        return false;
    }


    /* --------------------------------------------------------
     * PLAYER NAME
     * -------------------------------------------------------- */

    const char *returned_player_name =
        NULL;


    cJSON *player_name_json =
        cJSON_GetObjectItem(
            result,
            "player_name"
        );


    if (player_name_json != NULL &&
        cJSON_IsString(player_name_json)) {

        returned_player_name =
            player_name_json->valuestring;
    }


    /* --------------------------------------------------------
     * MODE
     * -------------------------------------------------------- */

    const char *mode =
        "pause";


    cJSON *mode_json =
        cJSON_GetObjectItem(
            result,
            "mode"
        );


    if (mode_json != NULL &&
        cJSON_IsString(mode_json)) {

        mode =
            mode_json->valuestring;
    }


    /* --------------------------------------------------------
     * TIME
     * -------------------------------------------------------- */

    double elapsed = 0.0;


    cJSON *time_json =
        cJSON_GetObjectItem(
            result,
            "time"
        );


    if (time_json != NULL &&
        cJSON_IsNumber(time_json)) {

        elapsed =
            time_json->valuedouble;
    }


    /* --------------------------------------------------------
     * DURATION
     * -------------------------------------------------------- */

    double duration = 0.0;


    cJSON *duration_json =
        cJSON_GetObjectItem(
            result,
            "duration"
        );


    if (duration_json != NULL &&
        cJSON_IsNumber(duration_json)) {

        duration =
            duration_json->valuedouble;
    }

    /* Right after Play, Lyrion sometimes answers once with the song at its
     * very end (e.g. 249/249) before reporting the real position.  If the
     * previous reading for this player and song was well before the end,
     * keep that one instead of jumping the progress bar to the end. */
    {
        static int last_player = -1;
        static double last_duration = -1.0, last_elapsed = -1.0;
        static bool skipped = false;
        const bool same = (last_player == current_player_index && last_duration == duration);
        if (same && !skipped && duration > 20.0 && elapsed >= duration - 0.5 &&
            last_elapsed >= 0.0 && last_elapsed < duration - 15.0) {
            elapsed = last_elapsed;
            skipped = true;               /* only once in a row */
        } else {
            skipped = false;
        }
        last_player = current_player_index;
        last_duration = duration;
        last_elapsed = elapsed;
    }


    /* --------------------------------------------------------
     * VOLUME
     * -------------------------------------------------------- */

    int volume = 0;


    cJSON *volume_json =
        cJSON_GetObjectItem(
            result,
            "mixer volume"
        );


    if (volume_json != NULL &&
        cJSON_IsNumber(volume_json)) {

        volume =
            volume_json->valueint;
    }

    /* Sleep timer: "will_sleep_in" (seconds) is only there while one runs. */
    {
        cJSON *sleep_json = cJSON_GetObjectItem(result, "will_sleep_in");
        const double left = cJSON_IsNumber(sleep_json) ? sleep_json->valuedouble : 0.0;
        sleep_left_s = left > 0.0 ? (int)(left + 0.5) : 0;
        sleep_left_at_ms = esp_timer_get_time() / 1000;
        sleep_left_player = current_player_index;

        /* Amp off with the timer: follow timers set or cancelled anywhere
         * (this display, Lyrion's web page, a phone app). */
        const int64_t now_ms = esp_timer_get_time() / 1000;
        if (sleep_left_s > 0) {
            sleep_amp_room = current_player_index;
            sleep_amp_due_ms = now_ms + (int64_t)sleep_left_s * 1000 + 3000;
        } else if (sleep_amp_room == current_player_index &&
                   sleep_amp_due_ms - now_ms > 15000) {
            sleep_amp_room = -1;                 /* cancelled before it ran out */
        }
    }


    /* --------------------------------------------------------
     * PLAYING TOGETHER (sync group)
     *
     * When this room plays in sync with others, Lyrion's status has
     * "sync_master" (one player id) and "sync_slaves" (comma-separated ids).
     * Turn them into a "which rooms" bit mask so the room button on Now
     * Playing can say e.g. "Kitchen +2".
     * -------------------------------------------------------- */
    {
        int mask = 0;
        char ids[300] = "";
        cJSON *sm = cJSON_GetObjectItem(result, "sync_master");
        cJSON *ss = cJSON_GetObjectItem(result, "sync_slaves");
        if (cJSON_IsString(sm) && sm->valuestring != NULL) {
            snprintf(ids, sizeof(ids), "%s", sm->valuestring);
        }
        if (cJSON_IsString(ss) && ss->valuestring != NULL) {
            const size_t used = strlen(ids);
            snprintf(ids + used, sizeof(ids) - used, ",%s", ss->valuestring);
        }
        char *save = NULL;
        for (char *tok = strtok_r(ids, ",", &save); tok != NULL; tok = strtok_r(NULL, ",", &save)) {
            for (int i = 0; i < lyrion_player_count; i++) {
                if (strcasecmp(tok, lyrion_players[i].id) == 0) mask |= 1 << i;
            }
        }
        if (mask != 0) mask |= 1 << player_index;     /* the group includes this room */
        else           mask = 1 << player_index;      /* on its own */

        /* Only tell the screen when it changes. */
        static int last_room = -1, last_mask = -1;
        if (player_index == current_player_index &&
            (player_index != last_room || mask != last_mask)) {
            np_group_msg_t gm = { .room = player_index, .mask = mask };
            if (ui_post(nav_room_group_job, &gm, sizeof(gm))) {   /* dropped: sent again next poll */
                last_room = player_index;
                last_mask = mask;
            }
        }
    }


    /* --------------------------------------------------------
     * CURRENT TRACK
     *
     * Lyrion returns the current playlist entry in the
     * "playlist_loop" array.  "playlist shuffle" is a
     * scalar shuffle-state field, not the track array.
     * -------------------------------------------------------- */

    const char *artist = NULL;
    const char *title = NULL;
    const char *album = NULL;

    /* Artwork identity.  Consecutive tracks of one album share a coverid, so
     * the cover is only downloaded and decoded once per album. */
    char cover_key[96];
    cover_key[0] = '\0';
    uint32_t art_url_hash = 0;      /* radio: the station's / show's picture address */

    char format_text[96];
    format_text[0] = '\0';
    bool format_lossless = false;
    int album_id = 0;
    char fav_url[sizeof(np_fav_url)];
    char fav_title[sizeof(np_fav_title)];
    fav_url[0] = '\0';
    fav_title[0] = '\0';
    int shuffle_now = -1;
    int repeat_now = -1;
    {
        cJSON *sh_j = cJSON_GetObjectItem(result, "playlist shuffle");
        cJSON *rp_j = cJSON_GetObjectItem(result, "playlist repeat");
        if (cJSON_IsNumber(sh_j)) shuffle_now = sh_j->valueint;
        else if (cJSON_IsString(sh_j) && sh_j->valuestring) shuffle_now = atoi(sh_j->valuestring);
        if (cJSON_IsNumber(rp_j)) repeat_now = rp_j->valueint;
        else if (cJSON_IsString(rp_j) && rp_j->valuestring) repeat_now = atoi(rp_j->valuestring);

        /* The open Queue follows the playing song. */
        cJSON *ci_j = cJSON_GetObjectItem(result, "playlist_cur_index");
        int cur_index = -1;
        if (cJSON_IsNumber(ci_j)) cur_index = ci_j->valueint;
        else if (cJSON_IsString(ci_j) && ci_j->valuestring) cur_index = atoi(ci_j->valuestring);
        if (cur_index >= 0) ui_post(queue_mark_playing_job, &cur_index, sizeof(cur_index));
    }


    cJSON *playlist_json =
        cJSON_GetObjectItem(
            result,
            "playlist_loop"
        );


    if (playlist_json != NULL &&
        cJSON_IsArray(playlist_json)) {

        cJSON *track =
            cJSON_GetArrayItem(
                playlist_json,
                0
            );


        if (track != NULL &&
            cJSON_IsObject(track)) {

            cJSON *artist_json =
                cJSON_GetObjectItem(
                    track,
                    "artist"
                );


            cJSON *title_json =
                cJSON_GetObjectItem(
                    track,
                    "title"
                );


            cJSON *album_json =
                cJSON_GetObjectItem(
                    track,
                    "album"
                );


            if (artist_json != NULL &&
                cJSON_IsString(artist_json)) {

                artist =
                    artist_json->valuestring;
            }


            if (title_json != NULL &&
                cJSON_IsString(title_json)) {

                title =
                    title_json->valuestring;
            }


            if (album_json != NULL &&
                cJSON_IsString(album_json)) {

                album =
                    album_json->valuestring;
            }

            format_build_text(track, format_text, sizeof(format_text), &format_lossless);
            /* For the FAV button: the playing URL and a name to save it under
             * (the station name for radio, else the song title). */
            {
                cJSON *u = cJSON_GetObjectItem(track, "url");
                cJSON *rt = cJSON_GetObjectItem(track, "remote_title");
                if (cJSON_IsString(u) && u->valuestring) snprintf(fav_url, sizeof(fav_url), "%s", u->valuestring);
                if (cJSON_IsString(rt) && rt->valuestring && rt->valuestring[0]) {
                    snprintf(fav_title, sizeof(fav_title), "%s", rt->valuestring);
                } else if (cJSON_IsString(title_json) && title_json->valuestring) {
                    snprintf(fav_title, sizeof(fav_title), "%s", title_json->valuestring);
                }
            }
            cJSON *album_id_json = cJSON_GetObjectItem(track, "album_id");
            if (album_id_json != NULL && cJSON_IsNumber(album_id_json)) {
                album_id = album_id_json->valueint;
            } else if (album_id_json != NULL && cJSON_IsString(album_id_json) &&
                       album_id_json->valuestring != NULL) {
                album_id = atoi(album_id_json->valuestring);
            }
            cJSON *coverid_json =
                cJSON_GetObjectItem(
                    track,
                    "coverid"
                );

            cJSON *artwork_url_json =
                cJSON_GetObjectItem(
                    track,
                    "artwork_url"
                );
            if (cJSON_IsString(artwork_url_json) && artwork_url_json->valuestring != NULL) {
                art_url_hash = 2166136261u;                       /* FNV-1a */
                for (const char *c = artwork_url_json->valuestring; *c; c++) {
                    art_url_hash = (art_url_hash ^ (uint8_t)*c) * 16777619u;
                }
            }

            if (coverid_json != NULL &&
                cJSON_IsString(coverid_json) &&
                coverid_json->valuestring != NULL &&
                coverid_json->valuestring[0] != '\0') {

                snprintf(cover_key, sizeof(cover_key),
                         "cid:%s", coverid_json->valuestring);
            }
            else if (coverid_json != NULL &&
                     cJSON_IsNumber(coverid_json)) {

                snprintf(cover_key, sizeof(cover_key),
                         "cid:%.0f", coverid_json->valuedouble);
            }
            else if (artwork_url_json != NULL &&
                     cJSON_IsString(artwork_url_json) &&
                     artwork_url_json->valuestring != NULL &&
                     artwork_url_json->valuestring[0] != '\0') {

                /* Internet radio: the artwork URL changes with the show/track. */
                snprintf(cover_key, sizeof(cover_key),
                         "url:%s", artwork_url_json->valuestring);
            }
        }
    }


    /* If the user changed players while this HTTP request was in flight,
     * discard the old player's response.  Otherwise an older response can
     * briefly overwrite the newly selected player and make switching look
     * unreliable. The refresh event will immediately query the new player. */
    if (player_index != current_player_index) {
        ESP_LOGI(TAG,
                 "Discarding stale status for %s; selected player changed",
                 player_name);
        cJSON_Delete(root);
        return true;
    }


    /* --------------------------------------------------------
     * UPDATE DISPLAY IMMEDIATELY
     *
     * Do this before downloading/decoding album artwork.  Artwork
     * can take several seconds (especially PNG), and the track
     * information should not wait for it.
     * -------------------------------------------------------- */

    update_lyrion_ui(
        returned_player_name,
        artist,
        title,
        album,
        mode,
        elapsed,
        duration,
        volume,
        album_id
    );

    /* Live radio: a stream with no length that isn't a file from the library. */
    const bool live_stream = duration <= 0.0 && fav_url[0] != '\0' &&
                             strncmp(fav_url, "file:", 5) != 0 && strncmp(fav_url, "db:", 3) != 0;
    update_lyrion_extras_ui(format_text, format_lossless, shuffle_now, repeat_now, live_stream);

    /* FAV button: when the song / stream changes, ask (on the browser task)
     * whether it is already in Favorites. */
    {
        static char last_fav_key[sizeof(np_fav_url) + 40] = "";
        char key[sizeof(last_fav_key)];
        snprintf(key, sizeof(key), "%.32s|%s", player_id, fav_url);
        if (strcmp(key, last_fav_key) != 0) {
            snprintf(last_fav_key, sizeof(last_fav_key), "%s", key);
            snprintf(np_fav_url, sizeof(np_fav_url), "%s", fav_url);
            snprintf(np_fav_title, sizeof(np_fav_title), "%s", fav_title);
            np_fav = -1;
            if (fav_url[0] != '\0') favorites_request_check();
        }
    }

    /* --------------------------------------------------------
     * ALBUM ART
     * -------------------------------------------------------- */

    char album_art_key[256];

    if (cover_key[0] != '\0') {
        /* Radio: also the stream and its picture address, so a new station
         * (or show) always counts as a new picture. */
        uint32_t stream_hash = 2166136261u;
        for (const char *c = fav_url; *c; c++) stream_hash = (stream_hash ^ (uint8_t)*c) * 16777619u;
        snprintf(
            album_art_key,
            sizeof(album_art_key),
            "%s|%s|%08lx%08lx",
            player_id,
            cover_key,
            (unsigned long)(duration > 0.0 ? 0 : stream_hash),
            (unsigned long)art_url_hash
        );
    }
    else {
        snprintf(
            album_art_key,
            sizeof(album_art_key),
            "%s|%s|%s|%s",
            player_id,
            artist != NULL ? artist : "",
            title != NULL ? title : "",
            album != NULL ? album : ""
        );
    }

    /* A cover that failed to download is tried again after 30 s, not on
     * every poll. */
    static char     art_failed_key[sizeof(album_art_key)] = "";
    static char     art_shown_stream[sizeof(fav_url)] = "";   /* radio stream the shown picture came from */
    static uint32_t art_failed_ms = 0;
    static uint32_t art_recheck_ms = 0;    /* new station: look again a few seconds later */
    const uint32_t  art_now_ms = esp_log_timestamp();

    /* Right after a station change the server can still hand out the last
     * station's picture, so a new station's picture is fetched once more. */
    if (art_recheck_ms != 0 && (int32_t)(art_now_ms - art_recheck_ms) >= 0) {
        art_recheck_ms = 0;
        if (strcmp(album_art_key, last_album_art_key) == 0) {
            ESP_LOGI(TAG, "Radio: checking the new station's picture again");
            last_album_art_key[0] = '\0';
        }
    }

    if (strcmp(album_art_key, last_album_art_key) != 0 &&
        !(strcmp(album_art_key, art_failed_key) == 0 && art_now_ms - art_failed_ms < 30000U)) {
        ESP_LOGI(TAG, "Album-art change detected: [%s]", album_art_key);

        /* Keep the current cover until the replacement downloads and decodes successfully. */
        album_art_last_status = 0;
        if (update_album_art(player_id)) {
            strncpy(
                last_album_art_key,
                album_art_key,
                sizeof(last_album_art_key) - 1
            );
            last_album_art_key[sizeof(last_album_art_key) - 1] = '\0';
            art_failed_key[0] = '\0';
            if (duration <= 0.0 && strcmp(art_shown_stream, fav_url) != 0) {
                art_recheck_ms = art_now_ms + 4000U;       /* a different station than before */
                if (art_recheck_ms == 0) art_recheck_ms = 1;
            }
            snprintf(art_shown_stream, sizeof(art_shown_stream), "%s", duration > 0.0 ? "" : fav_url);
        } else if (album_art_last_status == 404) {
            /* The server has no picture for this one.  On the same radio
             * station (an advert, or a song without a picture): keep the
             * station's last picture.  Anything else: the empty frame, not
             * the previous album's cover.  No retry either way. */
            if (duration <= 0.0 && fav_url[0] != '\0' && strcmp(art_shown_stream, fav_url) == 0) {
                ESP_LOGI(TAG, "Radio: no picture for this one; keeping the station's last one");
            } else {
                ESP_LOGI(TAG, "No picture for this one");
                clear_album_art();
                art_shown_stream[0] = '\0';
            }
            snprintf(last_album_art_key, sizeof(last_album_art_key), "%s", album_art_key);
            art_failed_key[0] = '\0';
        } else {
            ESP_LOGW(TAG, "Album-art update failed; keeping previous artwork (retry in 30 s)");
            snprintf(art_failed_key, sizeof(art_failed_key), "%s", album_art_key);
            art_failed_ms = art_now_ms;
        }
    }


    /* --------------------------------------------------------
     * LOG - only when the room, mode, song or volume changes, not every
     * 2-second poll (the elapsed time alone doesn't earn a line)
     * -------------------------------------------------------- */

    static uint32_t last_log_hash = 0;
    uint32_t log_hash = 2166136261u;
    {
        char key[200];
        snprintf(key, sizeof(key), "%d|%s|%s|%s|%d", player_index, mode,
                 artist != NULL ? artist : "", title != NULL ? title : "", volume);
        for (const char *k = key; *k; k++) log_hash = (log_hash ^ (uint8_t)*k) * 16777619u;
    }
    if (log_hash != last_log_hash) {
    last_log_hash = log_hash;
    ESP_LOGI(
        TAG,
        "%s | %s | %s - %s | %.0f/%.0f s | vol %d | %s",
        returned_player_name != NULL ? returned_player_name : player_name,
        mode,
        artist != NULL ? artist : "(none)",
        title != NULL ? title : "(none)",
        elapsed,
        duration,
        volume,
        format_text[0] ? format_text : "-"
    );
    }

    cJSON_Delete(root);




    return true;
}


