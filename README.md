# M5StickMyEBookReader

An eBook reader project for M5Stack devices (M5Stick/M5Paper) using ESP-IDF.

## Building / switching targets

This repo supports both ESP32 (original M5Paper) and ESP32-S3 (M5PaperS3). If you switch targets (or you see PSRAM init errors), regenerate the config:

- `idf.py set-target esp32` or `idf.py set-target esp32s3`
- `idf.py menuconfig` (verify PSRAM mode for your board) or `idf.py defconfig` (reset to defaults)
- `idf.py build flash monitor`

If you get `quad_psram: PSRAM ID read error ... wrong PSRAM line mode` on ESP32-S3, ensure PSRAM is configured for Octal (OPI) mode via `Component config -> ESP PSRAM` (or by using `sdkconfig.defaults.esp32s3` and reconfiguring).

## Licenses

### Fonts

This project uses the following fonts:

*   **Noto Sans Hebrew**: Licensed under the [Open Font License (OFL)](https://scripts.sil.org/cms/scripts/page.php?site_id=nrsi&id=OFL).
*   **Roboto**: Licensed under the [Apache License, Version 2.0](http://www.apache.org/licenses/LICENSE-2.0).

Please ensure you comply with these licenses when distributing this project.
