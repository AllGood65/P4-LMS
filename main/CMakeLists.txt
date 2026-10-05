idf_component_register(
    SRCS "main.c"
         "fonts/p4_font_14.c" "fonts/p4_font_16.c" "fonts/p4_font_18.c"
         "fonts/p4_font_28.c" "fonts/p4_font_40.c" "fonts/p4_font_48.c"
         "fonts/p4_clock_montserrat.c" "fonts/p4_clock_oswald.c" "fonts/p4_clock_dseg7.c"
         "fonts/p4_clock_dmserif.c" "fonts/p4_clock_orbitron.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES
        esp_codec_dev
        esp_driver_jpeg
        json
        mbedtls
        esp_http_client
        esp_wifi
        nvs_flash
        libpng
        esp_timer
        esp_http_server
        app_update
        esp_app_format
)
