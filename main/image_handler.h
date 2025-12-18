#pragma once

#include <M5Unified.h>
#include <string>
#include <vector>
#include <functional>
#include "sdkconfig.h"

/**
 * @brief Image format detected from file header
 */
enum class ImageFormat {
    UNKNOWN,
    JPEG,
    PNG,
    GIF,    // First frame only
    BMP
};

/**
 * @brief Display mode for images
 */
enum class ImageDisplayMode {
    INLINE,      // Small image inline with text
    BLOCK,       // Block-level image (full width)
    FULLSCREEN   // Full-screen view with touch interaction
};

/**
 * @brief Result of image decoding operation
 */
struct ImageDecodeResult {
    bool success = false;
    int originalWidth = 0;
    int originalHeight = 0;
    int scaledWidth = 0;
    int scaledHeight = 0;
    std::string errorMsg;
};

/**
 * @brief Memory-efficient image handler for e-ink displays
 * 
 * Designed for ESP32/ESP32-S3 with e-ink displays supporting 4-16 grayscale levels.
 * Uses streaming decompression where possible to minimize peak memory usage.
 * 
 * Memory considerations:
 * - M5Paper (ESP32): ~4MB PSRAM, recommend max ~400KB per image buffer
 * - M5PaperS3 (ESP32-S3): ~8MB PSRAM (OPI mode), can handle larger buffers
 * 
 * Display specs:
 * - Both devices: 540x960 pixels, 16 grayscale levels hardware
 * - M5Paper buffer: 2bpp (4 levels) to save RAM
 * - M5PaperS3 buffer: 4bpp (16 levels) for full quality
 */
class ImageHandler {
public:
    // Singleton access
    static ImageHandler& getInstance();

    // Delete copy/move constructors
    ImageHandler(const ImageHandler&) = delete;
    ImageHandler& operator=(const ImageHandler&) = delete;

    /**
     * @brief Initialize the image handler
     */
    void init();

    /**
     * @brief Detect image format from raw data
     * @param data Pointer to image data (at least 8 bytes needed)
     * @param size Size of data buffer
     * @return Detected image format
     */
    ImageFormat detectFormat(const uint8_t* data, size_t size);

    /**
     * @brief Detect image format from filename extension
     * @param filename Filename or path
     * @return Detected image format (may be UNKNOWN)
     */
    ImageFormat detectFormatFromFilename(const std::string& filename);

    /**
     * @brief Decode image and render to canvas at specified position
     * 
     * Uses streaming decode to minimize memory. Image is converted to grayscale
     * and optionally scaled to fit within maxWidth/maxHeight.
     * 
     * @param data Raw image data (JPEG, PNG, etc.)
     * @param size Size of image data
     * @param target Target canvas to render to
     * @param x X position on canvas
     * @param y Y position on canvas
     * @param maxWidth Maximum width (0 = no limit)
     * @param maxHeight Maximum height (0 = no limit)
     * @param mode Display mode (affects scaling strategy)
     * @return Decode result with dimensions and status
     */
    ImageDecodeResult decodeAndRender(
        const uint8_t* data, 
        size_t size,
        M5Canvas* target,
        int x, 
        int y,
        int maxWidth = 0,
        int maxHeight = 0,
        ImageDisplayMode mode = ImageDisplayMode::BLOCK
    );

    /**
     * @brief Decode image directly to display (for fullscreen mode)
     * 
     * Renders directly to display without intermediate buffer for maximum
     * efficiency. Uses full 16 grayscale levels when available.
     * 
     * @param data Raw image data
     * @param size Size of image data
     * @param centerX Center X position (or -1 for auto-center)
     * @param centerY Center Y position (or -1 for auto-center)
     * @param fitToScreen Scale to fit screen while maintaining aspect ratio
     * @return Decode result
     */
    ImageDecodeResult decodeToDisplay(
        const uint8_t* data,
        size_t size,
        int centerX = -1,
        int centerY = -1,
        bool fitToScreen = true
    );

    /**
     * @brief Get optimal maximum image dimensions based on device
     * @param forFullscreen If true, returns screen dimensions
     * @param outWidth Output width
     * @param outHeight Output height
     */
    void getMaxImageDimensions(bool forFullscreen, int& outWidth, int& outHeight);

    /**
     * @brief Get optimal color depth for image canvas
     * @return Bits per pixel (2 for M5Paper, 4 for M5PaperS3)
     */
    int getOptimalColorDepth() const;

    /**
     * @brief Calculate memory needed for an image at given dimensions
     * @param width Image width
     * @param height Image height
     * @param colorDepth Bits per pixel
     * @return Bytes needed
     */
    size_t calculateImageMemory(int width, int height, int colorDepth) const;

    /**
     * @brief Check if we have enough memory to decode an image
     * @param estimatedWidth Estimated decoded width
     * @param estimatedHeight Estimated decoded height
     * @return true if enough memory available
     */
    bool hasEnoughMemory(int estimatedWidth, int estimatedHeight) const;

    /**
     * @brief Convert RGB to grayscale value
     * @param r Red component (0-255)
     * @param g Green component (0-255)
     * @param b Blue component (0-255)
     * @return Grayscale value (0-255)
     */
    static uint8_t rgbToGrayscale(uint8_t r, uint8_t g, uint8_t b);

    /**
     * @brief Convert 8-bit grayscale to device-appropriate level
     * @param gray8 8-bit grayscale (0-255)
     * @return Device grayscale level (depends on color depth)
     */
    uint8_t quantizeGrayscale(uint8_t gray8) const;

private:
    ImageHandler();
    ~ImageHandler() = default;

    bool m_initialized = false;
    int m_colorDepth = 4;  // Default to 4bpp

    // JPEG decode helpers
    ImageDecodeResult decodeJpeg(
        const uint8_t* data, size_t size,
        M5Canvas* target, int x, int y,
        int maxWidth, int maxHeight,
        ImageDisplayMode mode
    );

    ImageDecodeResult decodeJpegToDisplay(
        const uint8_t* data, size_t size,
        int centerX, int centerY,
        bool fitToScreen
    );

    // PNG decode helpers
    ImageDecodeResult decodePng(
        const uint8_t* data, size_t size,
        M5Canvas* target, int x, int y,
        int maxWidth, int maxHeight,
        ImageDisplayMode mode
    );

    ImageDecodeResult decodePngToDisplay(
        const uint8_t* data, size_t size,
        int centerX, int centerY,
        bool fitToScreen
    );

    // Calculate scale factor to fit within bounds while maintaining aspect ratio
    float calculateScaleFactor(int srcW, int srcH, int maxW, int maxH) const;

    // Memory-efficient row-by-row rendering callback type
    using RowCallback = std::function<void(int y, const uint8_t* grayRow, int width)>;
};

// Convenience macro
#define imageHandler ImageHandler::getInstance()
