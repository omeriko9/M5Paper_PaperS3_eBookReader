# M5Paper/S3 eBook Reader

| <img width="513" height="834" alt="M5Paper" src="https://github.com/user-attachments/assets/eb9a982b-c579-43f6-a1a2-e03a690626a6" /> | <img width="400" height="698" alt="M5PaperS3" src="https://github.com/user-attachments/assets/4f3c6d4c-f0c6-4a7d-8506-2f57a0b42538" /> |
|----------------------------------------------------------------------------------------------------------------|----------------------------------------------------------------------------------------------------------------|

## Features
- Remembers the last book and page position, automatically restoring on wake
- Web-based interface for book uploads and WiFi configuration with captive portal
- Highly configurable settings via menuconfig
- Quick access to favorite books
- Customizable wallpapers displayed before deep sleep (S3 only)
- Support for Hebrew and Arabic with right-to-left (RTL) text rendering
- Built-in games: Minesweeper, Sudoku, and Wordle accessible from the "More" menu
- Gesture-based navigation: swipe left→right to open chapter menu, swipe up from bottom for settings
- Per-book font and size preferences stored in NVS
- EPUB parsing with support for images and chapters
- Hardware abstraction for display, SD card, buzzer, and IMU
- SD card support for additional storage and custom fonts (S3 only)
- 16-shade grayscale display for improved UI quality on M5PaperS3
- Support for HTML <math> tags (Work in Progress)
- Custom font support with VLW bitmap fonts

## Custom Fonts

### Generating VLW Fonts from TTF/OTF

The device uses VLW (VLW Font) bitmap fonts for rendering text. To create custom fonts:

1. Place your TTF or OTF font files in the `regular-fonts/` directory
2. Run the font generation script:
   ```bash
   python tools/generate_vlw_fonts.py
   ```
3. This will generate VLW files in the `spiffs_image/fonts/` directory
4. Rebuild the SPIFFS image and flash the device

### SD Card Support (M5PaperS3 only)

The M5PaperS3 supports SD card for additional storage and customization. The following folders and files are expected on the SD card:

#### Expected Folders:
- **`fonts/`**: Place OTF font files here for additional font options in the font selection menu
- **`wallpaper/`**: Place image files here to be randomly displayed before deep sleep
- **`books/`**: Place EPUB files here for additional book storage (scanned automatically)
- **`music/`**: Directory for music composition projects (used by the composer functionality)

#### Configuration File:
- **`config.txt`**: WiFi configuration file with the following format:
  ```
  ssid=YourWiFiNetworkName
  password=YourWiFiPassword
  ```
  This file is read automatically on startup to connect to WiFi without using the captive portal.

#### Data Files:
- **`lastbooks.txt`**: Automatically created file storing information about recently read books

### Using SD Card Fonts (M5PaperS3 only)

For additional fonts without rebuilding:

1. Place OTF font files in `/sdcard/fonts/` on the SD card
2. The fonts will be available in the font selection menu
3. Note: SD card fonts are loaded at runtime and may be slower than built-in VLW fonts

## Project Overview

This is an ESP-IDF firmware project for M5Paper and M5PaperS3 e-book readers. The project structure is as follows:

### Key Directories
- `main/`: Core source code files
  - `main.cpp`: Application entry point and initialization
  - `gui.cpp/h`: GUI state machine managing menus and screens
  - `epub_loader.cpp/h`: EPUB parsing and content extraction
  - `book_index.cpp/h`: Book scanning and management
  - `device_hal.cpp/h`: Hardware abstraction layer for display, SD card, buzzer, IMU
  - `web_server.cpp/h`: HTTP server for web interface and uploads
  - `wifi_manager.cpp/h`: WiFi connectivity and captive portal
  - `game_manager.cpp/h`: Built-in games (Minesweeper, Sudoku, Wordle)
  - `gesture_detector.cpp/h`: Touch gesture recognition
  - `zip_reader.cpp/h`: ZIP file handling for EPUBs
  - Other utilities: `html_utils.cpp/h`, `image_handler.cpp/h`, etc.
- `spiffs_image/`: SPIFFS filesystem content
  - Books (EPUB files)
  - Pre-built fonts (VLW format)
  - Icons and images
  - Web interface files (`index.html`)
- `tools/`: Development utilities and book preparation tools

### Development Tools
- `generate_vlw_fonts.py`: Converts TTF/OTF fonts to VLW bitmap format for the device
- `prepare_epubs.py`: Pre-processes EPUB files for optimal device performance
- `rename_books.py`: Renames EPUB files to a standardized format (book_1.epub, book_2.epub, etc.)
- `resize_icons.py`: Resizes icon images to the correct dimensions (96x96 pixels)
- `spiffsgen_debug.py`: SPIFFS filesystem image generation tool
- `test_spiffs_gen.py`: Testing utility for SPIFFS image generation
- `build/`: Build artifacts and ESP-IDF build files
- `managed_components/`: ESP-IDF managed components (M5Stack libraries, zlib)

### Configuration Files
- `CMakeLists.txt`: ESP-IDF project configuration
- `sdkconfig*`: Build configuration files
- `Kconfig.projbuild`: Project-specific configuration options
- `partitions.csv`: Flash partition layout

## Development Tools

The `tools/` directory contains various utilities for preparing content and building the project.

### prepare_epubs.py

This is a comprehensive EPUB preprocessing tool that optimizes books for the e-book reader's limited resources and provides significant performance improvements.

**What it does in detail:**

1. **Image Optimization**:
   - **Downscaling mode** (default): Resizes images larger than 960x540 pixels to fit the device's display resolution while maintaining aspect ratio
   - **Remove mode**: Completely strips all images from EPUBs for minimal file size
   - Updates OPF manifest to remove references to deleted images

2. **Font Cleanup**:
   - Removes all embedded fonts (TTF/OTF/WOFF) from EPUBs to save storage space
   - Cleans up OPF references to removed fonts

3. **Smart Renaming**:
   - Extracts title and author from EPUB metadata (OPF file)
   - Renames files to "Title - Author.epub" format
   - Falls back to original filename if metadata is missing

4. **Performance Metrics Generation**:
   - Creates `m_filename.bin` files containing pre-calculated chapter navigation data
   - Calculates text length for each chapter using the same algorithm as the device
   - Stores cumulative character offsets for instant chapter jumping
   - Format: version, total_chars, chapter_count, [chapter_offsets...]

5. **Book Index Creation**:
   - Generates `index.txt` with book metadata for fast loading
   - Format: `id|title|chapter|offset|path|size|hasMetrics|author|isFavorite|lastFont|lastFontSize`

**Why run it before uploading to SD card:**

- **10x faster chapter navigation**: Pre-calculated metrics enable instant jumping to any chapter
- **50-80% smaller files**: Image downscaling and font removal significantly reduce EPUB sizes
- **Instant book loading**: Index file allows immediate display of book list without parsing
- **Better display**: Images are optimized for the device's 960x540 resolution
- **Reduced memory usage**: Smaller images load faster and use less RAM
- **Progress tracking**: Metrics enable accurate reading progress calculation

**Without preprocessing:**
- Device must parse entire EPUB on first load (slow)
- Large images cause display issues and slow loading
- No chapter navigation until book is fully indexed
- Higher memory usage and potential crashes

**Usage examples:**

```bash
# Process all EPUBs in current directory (recommended for most users)
python tools/prepare_epubs.py

# Process EPUBs in specific directory
python tools/prepare_epubs.py /path/to/my/books

# Minimal size mode - remove all images (for very limited storage)
python tools/prepare_epubs.py --mode=remove /path/to/books

# Process and prepare for SD card upload
python tools/prepare_epubs.py /path/to/books
# Then copy the processed EPUBs, .bin files, and index.txt to SD card
```

**Output files:**
- `filename.epub` - Optimized EPUB file
- `m_filename.bin` - Navigation metrics (binary)
- `index.txt` - Book index for fast loading

**Website alternative:**
Some online EPUB tools can strip images, but they won't generate the device-specific metrics files that enable fast navigation and progress tracking. The local script provides complete optimization for this specific e-reader.

### Other Tools

- **`generate_vlw_fonts.py`**: Converts TrueType/OpenType fonts to VLW bitmap format used by the device. Run this when adding new fonts to `regular-fonts/`.
- **`rename_books.py`**: Renames EPUB files to a simple numbered format (book_1.epub, book_2.epub, etc.) for consistent naming.
- **`resize_icons.py`**: Resizes icon images in `spiffs_image/icons/` to 96x96 pixels for proper display.
- **`spiffsgen_debug.py`**: Advanced SPIFFS filesystem image generator with debugging capabilities.
- **`test_spiffs_gen.py`**: Test utility for validating SPIFFS image generation with sample content.

## Building / switching targets

This repo supports both ESP32 (original M5Paper) and ESP32-S3 (M5PaperS3). If you switch targets (or you see PSRAM init errors), regenerate the config:

- `idf.py set-target esp32` or `idf.py set-target esp32s3`
- `idf.py menuconfig` (verify PSRAM mode for your board) or `idf.py defconfig` (reset to defaults)
- `idf.py build flash monitor`

If you get `quad_psram: PSRAM ID read error ... wrong PSRAM line mode` on ESP32-S3, ensure PSRAM is configured for Octal (OPI) mode via `Component config -> ESP PSRAM` (or by using `sdkconfig.defaults.esp32s3` and reconfiguring).

## Licenses

### Project Code

This project is licensed under the MIT License. See the [LICENSE](LICENSE) file for details.

### Third-party Components

This project uses the following third-party components and libraries:

*   **ESP-IDF**: Licensed under the [Apache License, Version 2.0](https://www.apache.org/licenses/LICENSE-2.0)
*   **M5Stack Libraries** (M5Unified, M5GFX): Licensed under the [MIT License](https://opensource.org/licenses/MIT)
*   **miniz** (via zlib): Licensed under the [zlib License](https://zlib.net/zlib_license.html)

### Fonts

This project uses the following fonts:

*   **Noto Sans Hebrew**: Licensed under the [Open Font License (OFL)](https://scripts.sil.org/cms/scripts/page.php?site_id=nrsi&id=OFL).
*   **Roboto**: Licensed under the [Apache License, Version 2.0](http://www.apache.org/licenses/LICENSE-2.0).

Please ensure you comply with these licenses when distributing this project.
