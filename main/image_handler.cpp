#include "image_handler.h"
#include "device_hal.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_task_wdt.h"
#include <algorithm>
#include <cstring>

// TJpgDec - Tiny JPEG Decompressor available in ESP-ROM
#if CONFIG_IDF_TARGET_ESP32S3
#include "esp32s3/rom/tjpgd.h"
#elif CONFIG_IDF_TARGET_ESP32
#include "esp32/rom/tjpgd.h"
#else
#include "rom/tjpgd.h"
#endif

// For PNG we use the built-in M5GFX/LovyanGFX PNG decoder
// which is already integrated into drawPng methods

static const char *TAG = "ImageHandler";

// JPEG decoder workspace size - balance between memory and compatibility
// Larger workspace allows decoding larger/higher quality JPEGs
#ifdef CONFIG_EBOOK_DEVICE_M5PAPERS3
static constexpr size_t JPEG_WORKSPACE_SIZE = 4096; // S3 has more RAM
#else
static constexpr size_t JPEG_WORKSPACE_SIZE = 3100; // Original M5Paper
#endif

// Maximum image dimension we'll attempt to decode
#ifdef CONFIG_EBOOK_DEVICE_M5PAPERS3
static constexpr int MAX_IMAGE_DIMENSION = 1920; // Reasonable for e-reader
#else
static constexpr int MAX_IMAGE_DIMENSION = 1200; // More constrained
#endif

// Minimum memory we should keep free after image allocation (bytes)
static constexpr size_t MIN_FREE_MEMORY = 100 * 1024; // 100KB safety margin

// ============================================================================
// JPEG Decoder Context (used only for getting dimensions via TJpgDec)
// ============================================================================

struct JpegDecodeContext
{
    const uint8_t *srcData;
    size_t srcSize;
    size_t srcOffset;
};

// JPEG input callback (for reading dimensions only)
static size_t jpegInputCallback(JDEC *jdec, uint8_t *buff, size_t nbyte)
{
    JpegDecodeContext *ctx = (JpegDecodeContext *)jdec->device;

    if (ctx->srcOffset >= ctx->srcSize)
    {
        return 0; // EOF
    }

    size_t remain = ctx->srcSize - ctx->srcOffset;
    size_t toRead = (nbyte < remain) ? nbyte : remain;

    if (buff)
    {
        memcpy(buff, ctx->srcData + ctx->srcOffset, toRead);
    }
    ctx->srcOffset += toRead;

    return toRead;
}

// ============================================================================
// ImageHandler Implementation
// ============================================================================

ImageHandler &ImageHandler::getInstance()
{
    static ImageHandler instance;
    return instance;
}

ImageHandler::ImageHandler()
{
    // Constructor - init() must be called before use
}

void ImageHandler::init()
{
    if (m_initialized)
        return;

    // Get optimal color depth from device HAL
    m_colorDepth = DeviceHAL::getInstance().getCanvasColorDepth();

    ESP_LOGI(TAG, "ImageHandler initialized (colorDepth=%dbpp, maxDim=%d)",
             m_colorDepth, MAX_IMAGE_DIMENSION);

    m_initialized = true;
}

ImageFormat ImageHandler::detectFormat(const uint8_t *data, size_t size)
{
    if (!data || size < 8)
        return ImageFormat::UNKNOWN;

    // JPEG: FFD8FF
    if (data[0] == 0xFF && data[1] == 0xD8 && data[2] == 0xFF)
    {
        return ImageFormat::JPEG;
    }

    // PNG: 89504E47 (‰PNG)
    if (data[0] == 0x89 && data[1] == 0x50 && data[2] == 0x4E && data[3] == 0x47)
    {
        return ImageFormat::PNG;
    }

    // GIF: GIF87a or GIF89a
    if (data[0] == 'G' && data[1] == 'I' && data[2] == 'F' && data[3] == '8')
    {
        return ImageFormat::GIF;
    }

    // BMP: BM
    if (data[0] == 'B' && data[1] == 'M')
    {
        return ImageFormat::BMP;
    }

    return ImageFormat::UNKNOWN;
}

ImageFormat ImageHandler::detectFormatFromFilename(const std::string &filename)
{
    // Find extension
    size_t dotPos = filename.rfind('.');
    if (dotPos == std::string::npos)
        return ImageFormat::UNKNOWN;

    std::string ext = filename.substr(dotPos + 1);
    // Convert to lowercase
    for (char &c : ext)
    {
        if (c >= 'A' && c <= 'Z')
            c += 32;
    }

    if (ext == "jpg" || ext == "jpeg")
        return ImageFormat::JPEG;
    if (ext == "png")
        return ImageFormat::PNG;
    if (ext == "gif")
        return ImageFormat::GIF;
    if (ext == "bmp")
        return ImageFormat::BMP;

    return ImageFormat::UNKNOWN;
}

bool ImageHandler::getDimensions(const uint8_t *data, size_t size, int &outWidth, int &outHeight)
{
    ImageFormat format = detectFormat(data, size);

    if (format == ImageFormat::PNG)
    {
        if (size < 24)
            return false;
        const uint8_t *ihdr = data + 16;
        outWidth = (ihdr[0] << 24) | (ihdr[1] << 16) | (ihdr[2] << 8) | ihdr[3];
        outHeight = (ihdr[4] << 24) | (ihdr[5] << 16) | (ihdr[6] << 8) | ihdr[7];
        return true;
    }
    else if (format == ImageFormat::JPEG)
    {
        // Simple JPEG parser to find SOF0 (0xC0) or SOF2 (0xC2)
        size_t pos = 2;
        while (pos < size - 9)
        {
            if (data[pos] != 0xFF)
                break;
            uint8_t marker = data[pos + 1];
            uint16_t len = (data[pos + 2] << 8) | data[pos + 3];

            if (marker == 0xC0 || marker == 0xC2)
            { // SOF0 or SOF2
                outHeight = (data[pos + 5] << 8) | data[pos + 6];
                outWidth = (data[pos + 7] << 8) | data[pos + 8];
                return true;
            }

            pos += 2 + len;
        }
    }

    return false;
}

uint8_t ImageHandler::rgbToGrayscale(uint8_t r, uint8_t g, uint8_t b)
{
    // Standard luminance formula: Y = 0.299*R + 0.587*G + 0.114*B
    // Using integer math for speed: Y = (77*R + 150*G + 29*B) / 256
    return (uint8_t)((77 * r + 150 * g + 29 * b) >> 8);
}

uint8_t ImageHandler::quantizeGrayscale(uint8_t gray8) const
{
    if (m_colorDepth <= 2)
    {
        // 4 levels for 2bpp
        return (gray8 / 64) * 85;
    }
    else
    {
        // 16 levels for 4bpp
        return (gray8 / 16) * 17;
    }
}

int ImageHandler::getOptimalColorDepth() const
{
    return m_colorDepth;
}

void ImageHandler::getMaxImageDimensions(bool forFullscreen, int &outWidth, int &outHeight)
{
    if (forFullscreen)
    {
        outWidth = M5.Display.width();
        outHeight = M5.Display.height();
    }
    else
    {
        // For inline/block images, use reasonable content area size
        outWidth = M5.Display.width() - 40;  // Account for margins
        outHeight = M5.Display.height() / 2; // Don't take more than half page
    }
}

size_t ImageHandler::calculateImageMemory(int width, int height, int colorDepth) const
{
    // Calculate bytes needed for canvas buffer
    return ((size_t)width * height * colorDepth + 7) / 8;
}

bool ImageHandler::hasEnoughMemory(int estimatedWidth, int estimatedHeight) const
{
    size_t needed = calculateImageMemory(estimatedWidth, estimatedHeight, m_colorDepth);
    size_t freeSpram = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
    size_t freeDefault = heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT);

    // Prefer PSRAM for images
    size_t available = (freeSpram > MIN_FREE_MEMORY) ? freeSpram - MIN_FREE_MEMORY : 0;
    if (available < needed)
    {
        available = (freeDefault > MIN_FREE_MEMORY) ? freeDefault - MIN_FREE_MEMORY : 0;
    }

    return available >= needed;
}

float ImageHandler::calculateScaleFactor(int srcW, int srcH, int maxW, int maxH) const
{
    if (maxW <= 0 || maxH <= 0)
        return 1.0f;
    if (srcW <= 0 || srcH <= 0)
        return 1.0f;

    float scaleX = (float)maxW / srcW;
    float scaleY = (float)maxH / srcH;

    // Use the smaller scale to fit within bounds
    float scale = (scaleX < scaleY) ? scaleX : scaleY;

    // Don't upscale
    if (scale > 1.0f)
        scale = 1.0f;

    return scale;
}

// ============================================================================
// Main decode/render functions
// ============================================================================

ImageDecodeResult ImageHandler::decodeAndRender(
    const uint8_t *data, size_t size,
    LovyanGFX *target, int x, int y,
    int maxWidth, int maxHeight,
    ImageDisplayMode mode)
{
    ImageDecodeResult result;

    if (!m_initialized)
    {
        result.errorMsg = "ImageHandler not initialized";
        return result;
    }

    if (!data || size == 0)
    {
        result.errorMsg = "Invalid image data";
        return result;
    }

    ImageFormat format = detectFormat(data, size);

    switch (format)
    {
    case ImageFormat::JPEG:
        return decodeJpeg(data, size, target, x, y, maxWidth, maxHeight, mode);

    case ImageFormat::PNG:
        return decodePng(data, size, target, x, y, maxWidth, maxHeight, mode);

    case ImageFormat::GIF:
        // GIF support - extract first frame, treat as PNG-like
        result.errorMsg = "GIF format not yet supported";
        return result;

    case ImageFormat::BMP:
        // BMP is relatively simple but rarely used in EPUBs
        result.errorMsg = "BMP format not yet supported";
        return result;

    default:
        result.errorMsg = "Unknown image format";
        return result;
    }
}

ImageDecodeResult ImageHandler::decodeToDisplay(
    const uint8_t *data, size_t size,
    int centerX, int centerY,
    bool fitToScreen)
{
    ImageDecodeResult result;

    if (!m_initialized)
    {
        result.errorMsg = "ImageHandler not initialized";
        return result;
    }

    if (!data || size == 0)
    {
        result.errorMsg = "Invalid image data";
        return result;
    }

    ImageFormat format = detectFormat(data, size);

    switch (format)
    {
    case ImageFormat::JPEG:
        return decodeJpegToDisplay(data, size, centerX, centerY, fitToScreen);

    case ImageFormat::PNG:
        return decodePngToDisplay(data, size, centerX, centerY, fitToScreen);

    default:
        result.errorMsg = "Unsupported format for fullscreen display";
        return result;
    }
}

// ============================================================================
// JPEG Decoding - Using M5GFX built-in decoder for efficiency
// ============================================================================

ImageDecodeResult ImageHandler::decodeJpeg(
    const uint8_t *data, size_t size,
    LovyanGFX *target, int x, int y,
    int maxWidth, int maxHeight,
    ImageDisplayMode mode)
{
    ImageDecodeResult result;

    // Get the graphics target - either canvas or main display
    LovyanGFX *gfx = target ? target : (LovyanGFX *)&M5.Display;

    if (target && target->width() == 0)
    {
        result.errorMsg = "Target canvas not initialized or OOM";
        ESP_LOGE(TAG, "PNG: %s", result.errorMsg.c_str());
        return result;
    }

    // Use TJpgDec just to get dimensions first
    void *workspace = heap_caps_malloc(JPEG_WORKSPACE_SIZE, MALLOC_CAP_8BIT);
    if (!workspace)
    {
        workspace = heap_caps_malloc(JPEG_WORKSPACE_SIZE, MALLOC_CAP_SPIRAM);
    }
    if (!workspace)
    {
        result.errorMsg = "Failed to allocate JPEG workspace";
        return result;
    }

    JDEC jdec;
    JpegDecodeContext ctx;
    ctx.srcData = data;
    ctx.srcSize = size;
    ctx.srcOffset = 0;

    // Just prepare to get dimensions, don't decompress with TJpgDec
    JRESULT res = jd_prepare(&jdec, jpegInputCallback, workspace, JPEG_WORKSPACE_SIZE, &ctx);

    if (res != JDR_OK)
    {
        heap_caps_free(workspace);
        result.errorMsg = "JPEG prepare failed: " + std::to_string(res);
        return result;
    }

    result.originalWidth = jdec.width;
    result.originalHeight = jdec.height;
    heap_caps_free(workspace);

    ESP_LOGI(TAG, "JPEG: %dx%d (target=%s)", jdec.width, jdec.height, target ? "canvas" : "display");

    // Check dimensions
    bool tooLarge = (jdec.width > MAX_IMAGE_DIMENSION || jdec.height > MAX_IMAGE_DIMENSION);

    // Calculate scale factor
    float scale = 1.0f;
    if (maxWidth > 0 && maxHeight > 0)
    {
        scale = calculateScaleFactor(jdec.width, jdec.height, maxWidth, maxHeight);
    }

    if (tooLarge)
    {
        int scaledW = (int)(jdec.width * scale);
        int scaledH = (int)(jdec.height * scale);
        if (scaledW <= MAX_IMAGE_DIMENSION && scaledH <= MAX_IMAGE_DIMENSION)
        {
            ESP_LOGI(TAG, "Large JPEG %dx%d will be scaled to %dx%d", jdec.width, jdec.height, scaledW, scaledH);
        }
        else
        {
            result.errorMsg = "JPEG too large: " + std::to_string(jdec.width) + "x" + std::to_string(jdec.height);
            ESP_LOGW(TAG, "%s", result.errorMsg.c_str());
            return result;
        }
    }

    result.scaledWidth = (int)(jdec.width * scale);
    result.scaledHeight = (int)(jdec.height * scale);

    // Check memory before decode
    if (!hasEnoughMemory(result.scaledWidth, result.scaledHeight))
    {
        result.errorMsg = "Not enough memory for image";
        return result;
    }

    // Calculate position - center within max bounds
    int drawX = x;
    int drawY = y;
    if (maxWidth > 0 && result.scaledWidth < maxWidth)
    {
        drawX += (maxWidth - result.scaledWidth) / 2;
    }
    if (maxHeight > 0 && result.scaledHeight < maxHeight)
    {
        drawY += (maxHeight - result.scaledHeight) / 2;
    }

    // Use M5GFX's built-in drawJpg with scaling
    // This is much more efficient than manual pixel-by-pixel decoding
    // M5GFX automatically handles grayscale conversion for grayscale displays/canvases
    esp_task_wdt_reset();
    gfx->drawJpg(data, size, drawX, drawY, result.scaledWidth, result.scaledHeight, 0, 0, scale, scale);
    esp_task_wdt_reset();

    result.success = true;
    return result;
}

ImageDecodeResult ImageHandler::decodeJpegToDisplay(
    const uint8_t *data, size_t size,
    int centerX, int centerY,
    bool fitToScreen)
{
    ImageDecodeResult result;

    // Use TJpgDec just to get dimensions
    void *workspace = heap_caps_malloc(JPEG_WORKSPACE_SIZE, MALLOC_CAP_8BIT);
    if (!workspace)
    {
        workspace = heap_caps_malloc(JPEG_WORKSPACE_SIZE, MALLOC_CAP_SPIRAM);
    }
    if (!workspace)
    {
        result.errorMsg = "Failed to allocate JPEG workspace";
        return result;
    }

    JDEC jdec;
    JpegDecodeContext ctx;
    ctx.srcData = data;
    ctx.srcSize = size;
    ctx.srcOffset = 0;

    // Prepare to get dimensions
    JRESULT res = jd_prepare(&jdec, jpegInputCallback, workspace, JPEG_WORKSPACE_SIZE, &ctx);
    if (res != JDR_OK)
    {
        heap_caps_free(workspace);
        result.errorMsg = "JPEG prepare failed";
        return result;
    }

    result.originalWidth = jdec.width;
    result.originalHeight = jdec.height;
    heap_caps_free(workspace);

    int dispW = M5.Display.width();
    int dispH = M5.Display.height();

    // Calculate scale
    float scale = fitToScreen ? calculateScaleFactor(jdec.width, jdec.height, dispW, dispH) : 1.0f;

    result.scaledWidth = (int)(jdec.width * scale);
    result.scaledHeight = (int)(jdec.height * scale);

    // Calculate position
    if (centerX < 0)
        centerX = dispW / 2;
    if (centerY < 0)
        centerY = dispH / 2;

    int drawX = centerX - result.scaledWidth / 2;
    int drawY = centerY - result.scaledHeight / 2;

    // Clamp to screen bounds
    if (drawX < 0)
        drawX = 0;
    if (drawY < 0)
        drawY = 0;

    // Use M5GFX's drawJpg
    M5.Display.startWrite();
    esp_task_wdt_reset();
    M5.Display.drawJpg(data, size, drawX, drawY, result.scaledWidth, result.scaledHeight, 0, 0, scale, scale);
    esp_task_wdt_reset();
    M5.Display.endWrite();

    result.success = true;
    return result;
}

// ============================================================================
// PNG Decoding - Using M5GFX built-in decoder
// ============================================================================

ImageDecodeResult ImageHandler::decodePng(
    const uint8_t *data, size_t size,
    LovyanGFX *target, int x, int y,
    int maxWidth, int maxHeight,
    ImageDisplayMode mode)
{
    ImageDecodeResult result;

    // Get the graphics target - either canvas or main display
    LovyanGFX *gfx = target ? target : (LovyanGFX *)&M5.Display;

    if (target && target->width() == 0)
    {
        result.errorMsg = "Target canvas not initialized or OOM";
        ESP_LOGE(TAG, "PNG: %s", result.errorMsg.c_str());
        return result;
    }

    // M5GFX has built-in PNG support via drawPng
    // First, we need to get the PNG dimensions to calculate scaling

    // PNG header: 8-byte signature, then IHDR chunk
    // IHDR is at offset 8 (after signature) + 8 (length + type) = width/height
    if (size < 24)
    {
        result.errorMsg = "PNG too small";
        return result;
    }

    // Read width and height from IHDR (big-endian)
    const uint8_t *ihdr = data + 16;
    int pngWidth = (ihdr[0] << 24) | (ihdr[1] << 16) | (ihdr[2] << 8) | ihdr[3];
    int pngHeight = (ihdr[4] << 24) | (ihdr[5] << 16) | (ihdr[6] << 8) | ihdr[7];

    result.originalWidth = pngWidth;
    result.originalHeight = pngHeight;

    ESP_LOGI(TAG, "PNG: %dx%d (target=%s)", pngWidth, pngHeight, target ? "canvas" : "display");

    // Check dimensions
    bool tooLarge = (pngWidth > MAX_IMAGE_DIMENSION || pngHeight > MAX_IMAGE_DIMENSION);

    // Calculate scale factor
    float scale = 1.0f;
    if (maxWidth > 0 && maxHeight > 0)
    {
        scale = calculateScaleFactor(pngWidth, pngHeight, maxWidth, maxHeight);
    }

    if (tooLarge)
    {
        int scaledW = (int)(pngWidth * scale);
        int scaledH = (int)(pngHeight * scale);
        if (scaledW <= MAX_IMAGE_DIMENSION && scaledH <= MAX_IMAGE_DIMENSION)
        {
            ESP_LOGI(TAG, "Large PNG %dx%d will be scaled to %dx%d", pngWidth, pngHeight, scaledW, scaledH);
        }
        else
        {
            result.errorMsg = "PNG too large: " + std::to_string(pngWidth) + "x" + std::to_string(pngHeight);
            ESP_LOGW(TAG, "%s", result.errorMsg.c_str());
            return result;
        }
    }

    result.scaledWidth = (int)(pngWidth * scale);
    result.scaledHeight = (int)(pngHeight * scale);

    // Check memory
    if (!hasEnoughMemory(result.scaledWidth, result.scaledHeight))
    {
        result.errorMsg = "Not enough memory for PNG";
        return result;
    }

    // Center if needed
    int drawX = x;
    int drawY = y;
    if (maxWidth > 0 && result.scaledWidth < maxWidth)
    {
        drawX += (maxWidth - result.scaledWidth) / 2;
    }
    if (maxHeight > 0 && result.scaledHeight < maxHeight)
    {
        drawY += (maxHeight - result.scaledHeight) / 2;
    }

    // Use M5GFX drawPng with scaling
    // Note: M5GFX automatically converts to grayscale when drawing to grayscale canvas
    esp_task_wdt_reset();
    gfx->drawPng(data, size, drawX, drawY, result.scaledWidth, result.scaledHeight, 0, 0, scale, scale);
    esp_task_wdt_reset();

    result.success = true;
    return result;
}

ImageDecodeResult ImageHandler::decodePngToDisplay(
    const uint8_t *data, size_t size,
    int centerX, int centerY,
    bool fitToScreen)
{
    ImageDecodeResult result;

    if (size < 24)
    {
        result.errorMsg = "PNG too small";
        return result;
    }

    // Read dimensions from IHDR
    const uint8_t *ihdr = data + 16;
    int pngWidth = (ihdr[0] << 24) | (ihdr[1] << 16) | (ihdr[2] << 8) | ihdr[3];
    int pngHeight = (ihdr[4] << 24) | (ihdr[5] << 16) | (ihdr[6] << 8) | ihdr[7];

    result.originalWidth = pngWidth;
    result.originalHeight = pngHeight;

    // Check dimensions
    bool tooLarge = (pngWidth > MAX_IMAGE_DIMENSION || pngHeight > MAX_IMAGE_DIMENSION);

    int dispW = M5.Display.width();
    int dispH = M5.Display.height();

    float scale = fitToScreen ? calculateScaleFactor(pngWidth, pngHeight, dispW, dispH) : 1.0f;

    if (tooLarge)
    {
        int scaledW = (int)(pngWidth * scale);
        int scaledH = (int)(pngHeight * scale);
        if (scaledW <= MAX_IMAGE_DIMENSION && scaledH <= MAX_IMAGE_DIMENSION)
        {
            ESP_LOGI(TAG, "Large PNG %dx%d will be scaled to %dx%d", pngWidth, pngHeight, scaledW, scaledH);
        }
        else
        {
            result.errorMsg = "PNG too large: " + std::to_string(pngWidth) + "x" + std::to_string(pngHeight);
            ESP_LOGW(TAG, "%s", result.errorMsg.c_str());
            return result;
        }
    }

    result.scaledWidth = (int)(pngWidth * scale);
    result.scaledHeight = (int)(pngHeight * scale);

    // Calculate position
    if (centerX < 0)
        centerX = dispW / 2;
    if (centerY < 0)
        centerY = dispH / 2;

    int drawX = centerX - result.scaledWidth / 2;
    int drawY = centerY - result.scaledHeight / 2;

    if (drawX < 0)
        drawX = 0;
    if (drawY < 0)
        drawY = 0;

    // Draw directly to display
    M5.Display.startWrite();
    esp_task_wdt_reset();
    M5.Display.drawPng(data, size, drawX, drawY, result.scaledWidth, result.scaledHeight, 0, 0, scale, scale);
    esp_task_wdt_reset();
    M5.Display.endWrite();

    result.success = true;
    return result;
}
