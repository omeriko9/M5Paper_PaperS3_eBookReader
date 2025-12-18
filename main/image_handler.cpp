#include "image_handler.h"
#include "device_hal.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
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

static const char* TAG = "ImageHandler";

// JPEG decoder workspace size - balance between memory and compatibility
// Larger workspace allows decoding larger/higher quality JPEGs
#ifdef CONFIG_EBOOK_DEVICE_M5PAPERS3
static constexpr size_t JPEG_WORKSPACE_SIZE = 4096;  // S3 has more RAM
#else
static constexpr size_t JPEG_WORKSPACE_SIZE = 3100;  // Original M5Paper
#endif

// Maximum image dimension we'll attempt to decode
#ifdef CONFIG_EBOOK_DEVICE_M5PAPERS3
static constexpr int MAX_IMAGE_DIMENSION = 1920;  // Reasonable for e-reader
#else
static constexpr int MAX_IMAGE_DIMENSION = 1200;  // More constrained
#endif

// Minimum memory we should keep free after image allocation (bytes)
static constexpr size_t MIN_FREE_MEMORY = 100 * 1024;  // 100KB safety margin

// ============================================================================
// JPEG Decoder Context
// ============================================================================

struct JpegDecodeContext {
    const uint8_t* srcData;
    size_t srcSize;
    size_t srcOffset;
    
    M5Canvas* targetCanvas;
    LovyanGFX* targetDisplay;
    
    int targetX;
    int targetY;
    int maxWidth;
    int maxHeight;
    
    int scaledWidth;
    int scaledHeight;
    int scaleShift;  // 0=1:1, 1=1:2, 2=1:4, 3=1:8
    
    int colorDepth;
    bool success;
    std::string errorMsg;
};

// JPEG input callback
static size_t jpegInputCallback(JDEC* jdec, uint8_t* buff, size_t nbyte) {
    JpegDecodeContext* ctx = (JpegDecodeContext*)jdec->device;
    
    if (ctx->srcOffset >= ctx->srcSize) {
        return 0;  // EOF
    }
    
    size_t remain = ctx->srcSize - ctx->srcOffset;
    size_t toRead = (nbyte < remain) ? nbyte : remain;
    
    if (buff) {
        memcpy(buff, ctx->srcData + ctx->srcOffset, toRead);
    }
    ctx->srcOffset += toRead;
    
    return toRead;
}

// JPEG output callback - render decoded MCU block
static UINT jpegOutputCallback(JDEC* jdec, void* bitmap, JRECT* rect) {
    JpegDecodeContext* ctx = (JpegDecodeContext*)jdec->device;
    
    uint8_t* rgb = (uint8_t*)bitmap;
    int blockWidth = rect->right - rect->left + 1;
    int blockHeight = rect->bottom - rect->top + 1;
    
    LovyanGFX* gfx = ctx->targetCanvas ? (LovyanGFX*)ctx->targetCanvas : ctx->targetDisplay;
    if (!gfx) return 0;
    
    int dstX = ctx->targetX + rect->left;
    int dstY = ctx->targetY + rect->top;
    
    // Render each pixel, converting RGB to grayscale
    for (int by = 0; by < blockHeight; by++) {
        for (int bx = 0; bx < blockWidth; bx++) {
            int idx = (by * blockWidth + bx) * 3;
            uint8_t r = rgb[idx];
            uint8_t g = rgb[idx + 1];
            uint8_t b = rgb[idx + 2];
            
            // Convert to grayscale using luminance formula
            uint8_t gray = ImageHandler::rgbToGrayscale(r, g, b);
            
            // Quantize to device color depth
            uint8_t level;
            if (ctx->colorDepth <= 2) {
                // 4 levels (2bpp): 0, 85, 170, 255
                level = (gray / 64) * 85;
            } else {
                // 16 levels (4bpp): 0, 17, 34, ..., 255
                level = (gray / 16) * 17;
            }
            
            // Set pixel using grayscale color (R=G=B)
            uint32_t color = gfx->color888(level, level, level);
            gfx->drawPixel(dstX + bx, dstY + by, color);
        }
    }
    
    // Yield occasionally for long decodes
    static int blockCount = 0;
    if ((++blockCount & 0x1F) == 0) {
        vTaskDelay(1);
    }
    
    return 1;  // Continue decoding
}

// ============================================================================
// ImageHandler Implementation
// ============================================================================

ImageHandler& ImageHandler::getInstance() {
    static ImageHandler instance;
    return instance;
}

ImageHandler::ImageHandler() {
    // Constructor - init() must be called before use
}

void ImageHandler::init() {
    if (m_initialized) return;
    
    // Get optimal color depth from device HAL
    m_colorDepth = DeviceHAL::getInstance().getCanvasColorDepth();
    
    ESP_LOGI(TAG, "ImageHandler initialized (colorDepth=%dbpp, maxDim=%d)",
             m_colorDepth, MAX_IMAGE_DIMENSION);
    
    m_initialized = true;
}

ImageFormat ImageHandler::detectFormat(const uint8_t* data, size_t size) {
    if (!data || size < 8) return ImageFormat::UNKNOWN;
    
    // JPEG: FFD8FF
    if (data[0] == 0xFF && data[1] == 0xD8 && data[2] == 0xFF) {
        return ImageFormat::JPEG;
    }
    
    // PNG: 89504E47 (‰PNG)
    if (data[0] == 0x89 && data[1] == 0x50 && data[2] == 0x4E && data[3] == 0x47) {
        return ImageFormat::PNG;
    }
    
    // GIF: GIF87a or GIF89a
    if (data[0] == 'G' && data[1] == 'I' && data[2] == 'F' && data[3] == '8') {
        return ImageFormat::GIF;
    }
    
    // BMP: BM
    if (data[0] == 'B' && data[1] == 'M') {
        return ImageFormat::BMP;
    }
    
    return ImageFormat::UNKNOWN;
}

ImageFormat ImageHandler::detectFormatFromFilename(const std::string& filename) {
    // Find extension
    size_t dotPos = filename.rfind('.');
    if (dotPos == std::string::npos) return ImageFormat::UNKNOWN;
    
    std::string ext = filename.substr(dotPos + 1);
    // Convert to lowercase
    for (char& c : ext) {
        if (c >= 'A' && c <= 'Z') c += 32;
    }
    
    if (ext == "jpg" || ext == "jpeg") return ImageFormat::JPEG;
    if (ext == "png") return ImageFormat::PNG;
    if (ext == "gif") return ImageFormat::GIF;
    if (ext == "bmp") return ImageFormat::BMP;
    
    return ImageFormat::UNKNOWN;
}

uint8_t ImageHandler::rgbToGrayscale(uint8_t r, uint8_t g, uint8_t b) {
    // Standard luminance formula: Y = 0.299*R + 0.587*G + 0.114*B
    // Using integer math for speed: Y = (77*R + 150*G + 29*B) / 256
    return (uint8_t)((77 * r + 150 * g + 29 * b) >> 8);
}

uint8_t ImageHandler::quantizeGrayscale(uint8_t gray8) const {
    if (m_colorDepth <= 2) {
        // 4 levels for 2bpp
        return (gray8 / 64) * 85;
    } else {
        // 16 levels for 4bpp
        return (gray8 / 16) * 17;
    }
}

int ImageHandler::getOptimalColorDepth() const {
    return m_colorDepth;
}

void ImageHandler::getMaxImageDimensions(bool forFullscreen, int& outWidth, int& outHeight) {
    if (forFullscreen) {
        outWidth = M5.Display.width();
        outHeight = M5.Display.height();
    } else {
        // For inline/block images, use reasonable content area size
        outWidth = M5.Display.width() - 40;  // Account for margins
        outHeight = M5.Display.height() / 2; // Don't take more than half page
    }
}

size_t ImageHandler::calculateImageMemory(int width, int height, int colorDepth) const {
    // Calculate bytes needed for canvas buffer
    return ((size_t)width * height * colorDepth + 7) / 8;
}

bool ImageHandler::hasEnoughMemory(int estimatedWidth, int estimatedHeight) const {
    size_t needed = calculateImageMemory(estimatedWidth, estimatedHeight, m_colorDepth);
    size_t freeSpram = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
    size_t freeDefault = heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT);
    
    // Prefer PSRAM for images
    size_t available = (freeSpram > MIN_FREE_MEMORY) ? freeSpram - MIN_FREE_MEMORY : 0;
    if (available < needed) {
        available = (freeDefault > MIN_FREE_MEMORY) ? freeDefault - MIN_FREE_MEMORY : 0;
    }
    
    return available >= needed;
}

float ImageHandler::calculateScaleFactor(int srcW, int srcH, int maxW, int maxH) const {
    if (maxW <= 0 || maxH <= 0) return 1.0f;
    if (srcW <= 0 || srcH <= 0) return 1.0f;
    
    float scaleX = (float)maxW / srcW;
    float scaleY = (float)maxH / srcH;
    
    // Use the smaller scale to fit within bounds
    float scale = (scaleX < scaleY) ? scaleX : scaleY;
    
    // Don't upscale
    if (scale > 1.0f) scale = 1.0f;
    
    return scale;
}

// ============================================================================
// Main decode/render functions
// ============================================================================

ImageDecodeResult ImageHandler::decodeAndRender(
    const uint8_t* data, size_t size,
    M5Canvas* target, int x, int y,
    int maxWidth, int maxHeight,
    ImageDisplayMode mode
) {
    ImageDecodeResult result;
    
    if (!m_initialized) {
        result.errorMsg = "ImageHandler not initialized";
        return result;
    }
    
    if (!data || size == 0) {
        result.errorMsg = "Invalid image data";
        return result;
    }
    
    ImageFormat format = detectFormat(data, size);
    
    switch (format) {
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
    const uint8_t* data, size_t size,
    int centerX, int centerY,
    bool fitToScreen
) {
    ImageDecodeResult result;
    
    if (!m_initialized) {
        result.errorMsg = "ImageHandler not initialized";
        return result;
    }
    
    if (!data || size == 0) {
        result.errorMsg = "Invalid image data";
        return result;
    }
    
    ImageFormat format = detectFormat(data, size);
    
    switch (format) {
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
// JPEG Decoding
// ============================================================================

ImageDecodeResult ImageHandler::decodeJpeg(
    const uint8_t* data, size_t size,
    M5Canvas* target, int x, int y,
    int maxWidth, int maxHeight,
    ImageDisplayMode mode
) {
    ImageDecodeResult result;
    
    // Allocate workspace for TJpgDec
    void* workspace = heap_caps_malloc(JPEG_WORKSPACE_SIZE, MALLOC_CAP_8BIT);
    if (!workspace) {
        // Try PSRAM
        workspace = heap_caps_malloc(JPEG_WORKSPACE_SIZE, MALLOC_CAP_SPIRAM);
    }
    if (!workspace) {
        result.errorMsg = "Failed to allocate JPEG workspace";
        return result;
    }
    
    JDEC jdec;
    JpegDecodeContext ctx;
    ctx.srcData = data;
    ctx.srcSize = size;
    ctx.srcOffset = 0;
    ctx.targetCanvas = target;
    ctx.targetDisplay = nullptr;
    ctx.targetX = x;
    ctx.targetY = y;
    ctx.maxWidth = maxWidth;
    ctx.maxHeight = maxHeight;
    ctx.colorDepth = m_colorDepth;
    ctx.success = false;
    
    // Prepare decoder
    JRESULT res = jd_prepare(&jdec, jpegInputCallback, workspace, JPEG_WORKSPACE_SIZE, &ctx);
    if (res != JDR_OK) {
        heap_caps_free(workspace);
        result.errorMsg = "JPEG prepare failed: " + std::to_string(res);
        return result;
    }
    
    result.originalWidth = jdec.width;
    result.originalHeight = jdec.height;
    
    ESP_LOGI(TAG, "JPEG: %dx%d", jdec.width, jdec.height);
    
    // Determine optimal scale factor
    // TJpgDec supports 1:1, 1:2, 1:4, 1:8 hardware scaling
    float idealScale = 1.0f;
    if (maxWidth > 0 && maxHeight > 0) {
        idealScale = calculateScaleFactor(jdec.width, jdec.height, maxWidth, maxHeight);
    }
    
    // Find best hardware scale
    int scaleShift = 0;  // 0 = 1:1
    if (idealScale <= 0.125f) scaleShift = 3;      // 1:8
    else if (idealScale <= 0.25f) scaleShift = 2;  // 1:4
    else if (idealScale <= 0.5f) scaleShift = 1;   // 1:2
    // else scaleShift = 0 (1:1)
    
    ctx.scaleShift = scaleShift;
    ctx.scaledWidth = jdec.width >> scaleShift;
    ctx.scaledHeight = jdec.height >> scaleShift;
    
    result.scaledWidth = ctx.scaledWidth;
    result.scaledHeight = ctx.scaledHeight;
    
    ESP_LOGI(TAG, "JPEG scale: 1:%d -> %dx%d", (1 << scaleShift), ctx.scaledWidth, ctx.scaledHeight);
    
    // Check memory before decode
    if (!hasEnoughMemory(ctx.scaledWidth, ctx.scaledHeight)) {
        heap_caps_free(workspace);
        result.errorMsg = "Not enough memory for image";
        return result;
    }
    
    // Center within max bounds if smaller
    if (maxWidth > 0 && ctx.scaledWidth < maxWidth) {
        ctx.targetX += (maxWidth - ctx.scaledWidth) / 2;
    }
    if (maxHeight > 0 && ctx.scaledHeight < maxHeight) {
        ctx.targetY += (maxHeight - ctx.scaledHeight) / 2;
    }
    
    // Decompress
    res = jd_decomp(&jdec, jpegOutputCallback, scaleShift);
    
    heap_caps_free(workspace);
    
    if (res != JDR_OK) {
        result.errorMsg = "JPEG decompress failed: " + std::to_string(res);
        return result;
    }
    
    result.success = true;
    return result;
}

ImageDecodeResult ImageHandler::decodeJpegToDisplay(
    const uint8_t* data, size_t size,
    int centerX, int centerY,
    bool fitToScreen
) {
    ImageDecodeResult result;
    
    // Allocate workspace
    void* workspace = heap_caps_malloc(JPEG_WORKSPACE_SIZE, MALLOC_CAP_8BIT);
    if (!workspace) {
        workspace = heap_caps_malloc(JPEG_WORKSPACE_SIZE, MALLOC_CAP_SPIRAM);
    }
    if (!workspace) {
        result.errorMsg = "Failed to allocate JPEG workspace";
        return result;
    }
    
    JDEC jdec;
    JpegDecodeContext ctx;
    ctx.srcData = data;
    ctx.srcSize = size;
    ctx.srcOffset = 0;
    ctx.targetCanvas = nullptr;
    ctx.targetDisplay = &M5.Display;
    ctx.colorDepth = 4;  // Use full 16 levels for fullscreen
    ctx.success = false;
    
    // Prepare
    JRESULT res = jd_prepare(&jdec, jpegInputCallback, workspace, JPEG_WORKSPACE_SIZE, &ctx);
    if (res != JDR_OK) {
        heap_caps_free(workspace);
        result.errorMsg = "JPEG prepare failed";
        return result;
    }
    
    result.originalWidth = jdec.width;
    result.originalHeight = jdec.height;
    
    int dispW = M5.Display.width();
    int dispH = M5.Display.height();
    
    // Calculate scale
    float scale = fitToScreen ? calculateScaleFactor(jdec.width, jdec.height, dispW, dispH) : 1.0f;
    
    int scaleShift = 0;
    if (scale <= 0.125f) scaleShift = 3;
    else if (scale <= 0.25f) scaleShift = 2;
    else if (scale <= 0.5f) scaleShift = 1;
    
    ctx.scaledWidth = jdec.width >> scaleShift;
    ctx.scaledHeight = jdec.height >> scaleShift;
    ctx.scaleShift = scaleShift;
    
    result.scaledWidth = ctx.scaledWidth;
    result.scaledHeight = ctx.scaledHeight;
    
    // Calculate position
    if (centerX < 0) centerX = dispW / 2;
    if (centerY < 0) centerY = dispH / 2;
    
    ctx.targetX = centerX - ctx.scaledWidth / 2;
    ctx.targetY = centerY - ctx.scaledHeight / 2;
    
    // Clamp to screen bounds
    if (ctx.targetX < 0) ctx.targetX = 0;
    if (ctx.targetY < 0) ctx.targetY = 0;
    
    // Decompress directly to display
    M5.Display.startWrite();
    res = jd_decomp(&jdec, jpegOutputCallback, scaleShift);
    M5.Display.endWrite();
    
    heap_caps_free(workspace);
    
    if (res != JDR_OK) {
        result.errorMsg = "JPEG decompress failed";
        return result;
    }
    
    result.success = true;
    return result;
}

// ============================================================================
// PNG Decoding
// ============================================================================

// PNG decoding uses M5GFX's built-in capabilities which support streaming

ImageDecodeResult ImageHandler::decodePng(
    const uint8_t* data, size_t size,
    M5Canvas* target, int x, int y,
    int maxWidth, int maxHeight,
    ImageDisplayMode mode
) {
    ImageDecodeResult result;
    
    if (!target) {
        result.errorMsg = "No target canvas";
        return result;
    }
    
    // M5GFX has built-in PNG support via drawPng
    // First, we need to get the PNG dimensions to calculate scaling
    
    // PNG header: 8-byte signature, then IHDR chunk
    // IHDR is at offset 8 (after signature) + 8 (length + type) = width/height
    if (size < 24) {
        result.errorMsg = "PNG too small";
        return result;
    }
    
    // Read width and height from IHDR (big-endian)
    const uint8_t* ihdr = data + 16;
    int pngWidth = (ihdr[0] << 24) | (ihdr[1] << 16) | (ihdr[2] << 8) | ihdr[3];
    int pngHeight = (ihdr[4] << 24) | (ihdr[5] << 16) | (ihdr[6] << 8) | ihdr[7];
    
    result.originalWidth = pngWidth;
    result.originalHeight = pngHeight;
    
    ESP_LOGI(TAG, "PNG: %dx%d", pngWidth, pngHeight);
    
    // Calculate scale factor
    float scale = 1.0f;
    if (maxWidth > 0 && maxHeight > 0) {
        scale = calculateScaleFactor(pngWidth, pngHeight, maxWidth, maxHeight);
    }
    
    result.scaledWidth = (int)(pngWidth * scale);
    result.scaledHeight = (int)(pngHeight * scale);
    
    // Check memory
    if (!hasEnoughMemory(result.scaledWidth, result.scaledHeight)) {
        result.errorMsg = "Not enough memory for PNG";
        return result;
    }
    
    // Center if needed
    int drawX = x;
    int drawY = y;
    if (maxWidth > 0 && result.scaledWidth < maxWidth) {
        drawX += (maxWidth - result.scaledWidth) / 2;
    }
    if (maxHeight > 0 && result.scaledHeight < maxHeight) {
        drawY += (maxHeight - result.scaledHeight) / 2;
    }
    
    // Use M5GFX drawPng with scaling
    // Note: M5GFX automatically converts to grayscale when drawing to grayscale canvas
    target->drawPng(data, size, drawX, drawY, result.scaledWidth, result.scaledHeight, 0, 0, scale, scale);
    
    result.success = true;
    return result;
}

ImageDecodeResult ImageHandler::decodePngToDisplay(
    const uint8_t* data, size_t size,
    int centerX, int centerY,
    bool fitToScreen
) {
    ImageDecodeResult result;
    
    if (size < 24) {
        result.errorMsg = "PNG too small";
        return result;
    }
    
    // Read dimensions from IHDR
    const uint8_t* ihdr = data + 16;
    int pngWidth = (ihdr[0] << 24) | (ihdr[1] << 16) | (ihdr[2] << 8) | ihdr[3];
    int pngHeight = (ihdr[4] << 24) | (ihdr[5] << 16) | (ihdr[6] << 8) | ihdr[7];
    
    result.originalWidth = pngWidth;
    result.originalHeight = pngHeight;
    
    int dispW = M5.Display.width();
    int dispH = M5.Display.height();
    
    float scale = fitToScreen ? calculateScaleFactor(pngWidth, pngHeight, dispW, dispH) : 1.0f;
    
    result.scaledWidth = (int)(pngWidth * scale);
    result.scaledHeight = (int)(pngHeight * scale);
    
    // Calculate position
    if (centerX < 0) centerX = dispW / 2;
    if (centerY < 0) centerY = dispH / 2;
    
    int drawX = centerX - result.scaledWidth / 2;
    int drawY = centerY - result.scaledHeight / 2;
    
    if (drawX < 0) drawX = 0;
    if (drawY < 0) drawY = 0;
    
    // Draw directly to display
    M5.Display.startWrite();
    M5.Display.drawPng(data, size, drawX, drawY, result.scaledWidth, result.scaledHeight, 0, 0, scale, scale);
    M5.Display.endWrite();
    
    result.success = true;
    return result;
}
