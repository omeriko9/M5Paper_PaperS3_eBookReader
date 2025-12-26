# M5StickMyEBookReader

## eBook Reader for M5Paper / M5PaperS3

<img width="513" height="834" alt="image" src="https://github.com/user-attachments/assets/eb9a982b-c579-43f6-a1a2-e03a690626a6" />
<img width="400" height="698" alt="image" src="https://github.com/user-attachments/assets/4f3c6d4c-f0c6-4a7d-8506-2f57a0b42538" />

## Features
- Remembers the last book/page
- Exposes WebUI for easy book selection and upload
- Highly configurable
- Quick access to favorite books
- Customizable wallpapers on sleep
- Supports Hebrew and Arabic
- Support HTML <math> tags (WIP)
- Add your own fonts

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
