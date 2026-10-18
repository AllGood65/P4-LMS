# Third-party notices

P4-LMS is built with, and its firmware includes, the following components. Each remains under its own license.

| Component | Used for | License |
| --- | --- | --- |
| [ESP-IDF](https://github.com/espressif/esp-idf) (Espressif Systems) | Framework, drivers, Wi-Fi, HTTP, OTA | Apache License 2.0 |
| [LVGL](https://github.com/lvgl/lvgl) | Graphics and touch interface | MIT |
| [QR Code generator library](https://www.nayuki.io/page/qr-code-generator-library) (Project Nayuki) | QR code on the About screen (`main/qr/`) | MIT |
| [mDNS](https://components.espressif.com/components/espressif/mdns) (Espressif Systems) | The `p4lms.local` web address | Apache License 2.0 |
| [cJSON](https://github.com/DaveGamble/cJSON) | JSON parsing (bundled with ESP-IDF) | MIT |
| [libpng](http://www.libpng.org/pub/png/libpng.html) / zlib | PNG cover art decoding | libpng License / zlib License |
| Espressif component registry packages (board support package, codec driver, esp_lcd drivers) | Display, touch and microphone | Apache License 2.0 (see each package) |
| [Montserrat](https://github.com/JulietaUla/Montserrat) typeface | Display fonts (converted to LVGL fonts) | SIL Open Font License 1.1 |
| [Font Awesome Free](https://fontawesome.com/) icons | Symbols in the display fonts | Icons: CC BY 4.0; fonts: SIL Open Font License 1.1 |

Services used at run time (not included in the firmware):

- [Open-Meteo](https://open-meteo.com/) weather and geocoding APIs - free for non-commercial use, data under CC BY 4.0. Weather data by Open-Meteo.com.
- [Lyrion Music Server](https://lyrion.org/) and [Home Assistant](https://www.home-assistant.io/) run on your own network.

The screenshot in `docs/` shows cover art from "CC BY: FREE TO USE - Chill/Study/Lounge Instrumentals" by 1000 Handz ([Free Music Archive](https://freemusicarchive.org/music/1000-handz/cc-by-free-to-use-chillstudylounge-instrumentals), CC BY 4.0).
