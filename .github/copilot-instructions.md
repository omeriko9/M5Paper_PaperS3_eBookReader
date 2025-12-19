# M5Stick E-Book Reader AI Coding Instructions

## Architecture Overview
This is an ESP-IDF firmware project for M5Paper/M5PaperS3 e-book readers. Core components:
- **Main loop**: Initializes hardware via M5Unified, then runs GUI event loop
- **GUI**: State machine managing menus (MAIN_MENU, LIBRARY, READER, WIFI_CONFIG, SETTINGS, GAMES_MENU, GAME_PLAYING, CHAPTER_MENU, FONT_SELECTION)
- **Book management**: `BookIndex` scans SPIFFS/SD for EPUBs, stores per-book font+size preferences, `EpubLoader` parses ZIP-based EPUBs with `ZipReader`
- **Web interface**: ESP HTTP server for book uploads and WiFi captive portal
- **Hardware abstraction**: `DeviceHAL` handles display, SD card, buzzer, IMU
- **Gesture detection**: `GestureDetector` handles swipe gestures for chapter menu and settings access
- **Games**: `GameManager` provides Minesweeper, Sudoku, and Wordle games accessible from "More" menu

Components communicate via global singleton instances (e.g., `wifiManager`, `gui`, `bookIndex`).

## Key Patterns
- **EPUB parsing**: Uses `miniz` via `ZipReader` for ZIP access, extracts XHTML chapters, images
- **Rendering**: Text rendered to M5Canvas with VLW bitmap fonts (generated from TTF via `tools/generate_vlw_fonts.py`)
- **Storage**: Books in SPIFFS partition (10MB) or SD card; fonts/images pre-built in `spiffs_image/`
- **Config**: Kconfig.projbuild defines device variants (M5Paper vs M5PaperS3), features (buzzer, IMU, SD)
- **Multilingual**: Supports Hebrew, Arabic with merged VLW fonts; RTL text rendering
- **Last Book**: Progress saved on every page turn; restores automatically on wake from sleep
- **Per-Font Sizes**: Each font maintains its own preferred size, stored in NVS
- **16-Shade Grayscale**: M5PaperS3 uses 4bpp (16 shades) for better UI quality; text boldness configurable

## New Features (v2.0)
- **Gesture-based navigation**: Swipe left→right opens chapter menu, swipe up from bottom opens settings
- **Games menu**: "More" button in main menu provides access to Minesweeper, Sudoku, Wordle
- **SD Card fonts** (S3 only): Place .otf files in `/sdcard/fonts/` for custom font selection
- **Wallpaper on sleep** (S3 only): Place images in `/sdcard/wallpaper/` to display random image before deep sleep
- **Per-book font settings**: Each book remembers which font and size was last used

## Development Workflow
- **Target switching**: `idf.py set-target esp32` or `esp32s3`; verify PSRAM mode in `menuconfig` (Octal for S3)
- **Building**: `idf.py build` creates SPIFFS image from `spiffs_image/`; `idf.py flash monitor` for deploy
- **Debugging**: Use ESP-IDF monitor; check NVS for settings, SPIFFS for books
- **Font generation**: Run `python tools/generate_vlw_fonts.py` to update VLW fonts from `regular-fonts/`
- **Testing**: Upload test books to `spiffs_image/` or `test_spiffs_dir/`; use `idf.py spiffsgen` for updates

## Code Conventions
- **Error handling**: ESP_ERROR_CHECK for critical failures; log warnings for non-fatal issues
- **Memory**: Careful with heap on ESP32; use static buffers for large allocations
- **Concurrency**: GUI runs on main task; rendering may use separate core on S3 via config
- **File paths**: Absolute paths for ESP-IDF; SPIFFS mounts at `/spiffs`, SD at `/sdcard`
- **Dependencies**: Managed via idf_component.yml; M5Stack libs auto-downloaded
- **State machine**: New states should be added to AppState enum in gui.h, with draw* and on*Click handlers

Reference: `main/main.cpp` for init sequence, `gui.cpp` for state logic, `epub_loader.cpp` for parsing, `gesture_detector.cpp` for touch gestures, `game_manager.cpp` for games.