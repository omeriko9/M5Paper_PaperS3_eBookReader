#include "zip_reader.h"
#include "esp_log.h"
#include <stdio.h>
#include <string.h>
#include <vector>
#include <memory>
#include <string_view>
#include <zlib.h>
#include <algorithm>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_task_wdt.h"

static const char *TAG = "ZIP";

// Signatures
static const uint32_t EOCD_SIGNATURE = 0x06054b50;
static const uint32_t CD_SIGNATURE = 0x02014b50;
static const uint32_t LFH_SIGNATURE = 0x04034b50;
static const size_t MAX_INFLATE_OUTPUT = 2 * 1024 * 1024; // Safety cap for total output (streamed)

static inline uint16_t readLE16(const uint8_t *p) {
    return uint16_t(p[0]) | (uint16_t(p[1]) << 8);
}

static inline uint32_t readLE32(const uint8_t *p) {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}

static uint8_t* allocZipBuffer(size_t size) {
    uint8_t* buf = (uint8_t*)heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) {
        buf = (uint8_t*)heap_caps_malloc(size, MALLOC_CAP_8BIT);
    }
    return buf;
}

bool ZipReader::open(const char* path) {
    close();
    filePath = path;
    return parseCentralDirectory();
}

void ZipReader::close() {
    files.clear();
    cdBuffer.reset();
    filePath = "";
}

bool ZipReader::fileExists(const std::string& filename) {
    return findFile(filename) != nullptr;
}

bool ZipReader::parseCentralDirectory() {
    FILE* f = fopen(filePath.c_str(), "rb");
    if (!f) {
        ESP_LOGE(TAG, "Failed to open file: %s", filePath.c_str());
        return false;
    }

    // Find EOCD
    fseek(f, 0, SEEK_END);
    long fileSize = ftell(f);
    
    // Search backwards for EOCD signature
    // EOCD is at least 22 bytes
    if (fileSize < 22) {
        fclose(f);
        return false;
    }

    uint32_t eocdOffset = 0;
    
    // Limit search to last 64KB (comment max size is 65535)
    long searchLimit = (fileSize > 65536) ? fileSize - 65536 : 0;
    
    // Use a smaller buffer to avoid OOM (4KB)
    const size_t BUF_SIZE = 4096;
    uint8_t* searchBuf = (uint8_t*)heap_caps_malloc(BUF_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!searchBuf) {
        // Fallback to internal RAM if SPIRAM alloc fails
        searchBuf = (uint8_t*)malloc(BUF_SIZE);
    }
    
    if (!searchBuf) {
        ESP_LOGE(TAG, "Failed to allocate search buffer");
        fclose(f);
        return false;
    }

    long currentPos = fileSize;
    while (currentPos > searchLimit) {
        esp_task_wdt_reset();
        long readSize = BUF_SIZE;
        if (currentPos - readSize < searchLimit) {
            readSize = currentPos - searchLimit;
        }
        
        long readStart = currentPos - readSize;
        fseek(f, readStart, SEEK_SET);
        if (fread(searchBuf, 1, readSize, f) != readSize) {
            ESP_LOGE(TAG, "Failed to read search area");
            free(searchBuf);
            fclose(f);
            return false;
        }

        // Search backwards in buffer
        // We need to be careful about the boundary. 
        // If signature is split between chunks, we might miss it.
        // But since we are searching for EOCD which is at the END, 
        // and we iterate backwards, we just need to ensure we overlap or handle it.
        // Easiest is to overlap by 3 bytes.
        
        for (long i = readSize - 4; i >= 0; i--) {
            uint32_t sig = searchBuf[i] | (searchBuf[i+1] << 8) | (searchBuf[i+2] << 16) | (searchBuf[i+3] << 24);
            if (sig == EOCD_SIGNATURE) {
                eocdOffset = readStart + i;
                break;
            }
        }
        
        if (eocdOffset != 0) break;
        
        // Move back, overlapping by 3 bytes to catch split signatures
        currentPos -= (readSize - 3);
        if (currentPos <= searchLimit + 3) break; // Avoid infinite loop if stuck
    }
    
    free(searchBuf);

    if (eocdOffset == 0) {
        ESP_LOGE(TAG, "EOCD not found");
        fclose(f);
        return false;
    }

    // Read CD offset and count
    fseek(f, eocdOffset + 10, SEEK_SET);
    uint16_t numEntries;
    fread(&numEntries, 1, 2, f); // Total entries

    fseek(f, eocdOffset + 12, SEEK_SET);
    uint32_t cdSize;
    fread(&cdSize, 1, 4, f);
    
    fseek(f, eocdOffset + 16, SEEK_SET);
    uint32_t cdOffset;
    fread(&cdOffset, 1, 4, f);

    // Sanity check to avoid parsing garbage
    if (cdOffset + cdSize > (uint32_t)fileSize) {
        ESP_LOGE(TAG, "Central directory outside file");
        fclose(f);
        return false;
    }

    // Read the entire central directory in one go to avoid many small SPIFFS seeks
    cdBuffer.reset(allocZipBuffer(cdSize));
    if (!cdBuffer) {
        ESP_LOGE(TAG, "Failed to allocate %u bytes for central directory", (unsigned)cdSize);
        fclose(f);
        return false;
    }

    fseek(f, cdOffset, SEEK_SET);
    if (fread(cdBuffer.get(), 1, cdSize, f) != cdSize) {
        ESP_LOGE(TAG, "Failed to read central directory");
        cdBuffer.reset();
        fclose(f);
        return false;
    }

    fclose(f);

    // Parse CD from memory
    files.clear();
    files.reserve(numEntries);

    const uint8_t *p = cdBuffer.get();
    const uint8_t *end = cdBuffer.get() + cdSize;
    for (int i = 0; i < numEntries && (p + 46) <= end; i++) {
        uint32_t sig = readLE32(p);
        if (sig != CD_SIGNATURE) break;

        ZipFileInfo info;
        info.compressionMethod = readLE16(p + 10);
        info.compressedSize = readLE32(p + 20);
        info.uncompressedSize = readLE32(p + 24);

        uint16_t nameLen = readLE16(p + 28);
        uint16_t extraLen = readLE16(p + 30);
        uint16_t commentLen = readLE16(p + 32);
        info.localHeaderOffset = readLE32(p + 42);

        const uint8_t *nameStart = p + 46;
        if (nameStart + nameLen > end) break;

        info.nameOffset = (uint32_t)(nameStart - cdBuffer.get());
        info.nameLen = nameLen;
        files.push_back(info);

        // Move to next entry
        p += 46 + nameLen + extraLen + commentLen;

        // Yield occasionally so the idle task can reset the watchdog when reading large EPUBs
        if ((i & 0x0F) == 0) {
            esp_task_wdt_reset();
            vTaskDelay(1);
        }
    }

    // Sort files by name for binary search
    std::sort(files.begin(), files.end(), [this](const ZipFileInfo& a, const ZipFileInfo& b) {
        return std::string_view((char*)cdBuffer.get() + a.nameOffset, a.nameLen) < 
               std::string_view((char*)cdBuffer.get() + b.nameOffset, b.nameLen);
    });

    return true;
}

const ZipFileInfo* ZipReader::findFile(const std::string& filename) const {
    if (!cdBuffer) return nullptr;
    
    auto it = std::lower_bound(files.begin(), files.end(), filename, [this](const ZipFileInfo& info, const std::string& name) {
        return std::string_view((char*)cdBuffer.get() + info.nameOffset, info.nameLen) < name;
    });
    if (it != files.end()) {
        if (it->nameLen == filename.length() && 
            memcmp(cdBuffer.get() + it->nameOffset, filename.c_str(), it->nameLen) == 0) {
            return &(*it);
        }
    }
    return nullptr;
}

std::string ZipReader::extractFile(const std::string& filename) {
    const ZipFileInfo* infoPtr = findFile(filename);
    if (!infoPtr) {
        ESP_LOGW(TAG, "File not found in zip: %s", filename.c_str());
        return "";
    }

    const ZipFileInfo& info = *infoPtr;
    FILE* f = fopen(filePath.c_str(), "rb");
    if (!f) return "";

    // Go to Local File Header
    fseek(f, info.localHeaderOffset, SEEK_SET);
    
    uint32_t sig;
    fread(&sig, 1, 4, f);
    if (sig != LFH_SIGNATURE) {
        fclose(f);
        return "";
    }

    // Skip to name length in LFH
    fseek(f, 22, SEEK_CUR); // Skip to name len
    uint16_t nameLen, extraLen;
    fread(&nameLen, 1, 2, f);
    fread(&extraLen, 1, 2, f);
    
    // Skip name and extra field to get to data
    fseek(f, nameLen + extraLen, SEEK_CUR);
    
    size_t compressedSize = info.compressedSize;
    if (compressedSize == 0) {
        fclose(f);
        return "";
    }

    std::string result;
    if (info.uncompressedSize > 0) {
        result.reserve(info.uncompressedSize);
    }
    constexpr size_t IN_CHUNK = 2048;
    constexpr size_t OUT_CHUNK = 4096;
    std::unique_ptr<uint8_t, decltype(&heap_caps_free)> inBuf(
        (uint8_t*)allocZipBuffer(IN_CHUNK),
        heap_caps_free);
    std::unique_ptr<uint8_t, decltype(&heap_caps_free)> outBuf(
        (uint8_t*)allocZipBuffer(OUT_CHUNK),
        heap_caps_free);
    if (!inBuf || !outBuf) {
        ESP_LOGE(TAG, "OOM allocating IO buffers");
        fclose(f);
        return "";
    }

    if (info.compressionMethod == 0) {
        // Stored (no compression) - stream append to avoid large allocs
        size_t remaining = compressedSize;
        while (remaining > 0) {
            esp_task_wdt_reset();
            size_t toRead = remaining > IN_CHUNK ? IN_CHUNK : remaining;
            size_t bytesRead = fread(inBuf.get(), 1, toRead, f);
            if (bytesRead == 0) break;
            result.append(reinterpret_cast<const char*>(inBuf.get()), bytesRead);
            remaining -= bytesRead;
            if (result.size() > MAX_INFLATE_OUTPUT) {
                ESP_LOGE(TAG, "Stored entry too large for buffer: %u bytes", (unsigned)result.size());
                result.clear();
                break;
            }
            if ((remaining & 0x3FFF) == 0) vTaskDelay(1); // yield occasionally
        }
    } else if (info.compressionMethod == 8) {
        // Deflate streaming
        z_stream strm;
        memset(&strm, 0, sizeof(strm));
        if (inflateInit2(&strm, -MAX_WBITS) != Z_OK) {
            fclose(f);
            return "";
        }

        size_t remaining = compressedSize;
        int ret = Z_OK;

        while (ret != Z_STREAM_END) {
            esp_task_wdt_reset();
            if (strm.avail_in == 0 && remaining > 0) {
                size_t toRead = remaining > IN_CHUNK ? IN_CHUNK : remaining;
                size_t bytesRead = fread(inBuf.get(), 1, toRead, f);
                if (bytesRead == 0) {
                    ESP_LOGE(TAG, "Unexpected EOF while inflating '%s'", filename.c_str());
                    break;
                }
                remaining -= bytesRead;
                strm.next_in = inBuf.get();
                strm.avail_in = bytesRead;
            }

            strm.next_out = outBuf.get();
            strm.avail_out = OUT_CHUNK;
            ret = inflate(&strm, remaining ? Z_NO_FLUSH : Z_FINISH);

            size_t have = OUT_CHUNK - strm.avail_out;
            if (have) {
                result.append(reinterpret_cast<const char*>(outBuf.get()), have);
                if (result.size() > MAX_INFLATE_OUTPUT) {
                    ESP_LOGE(TAG, "Inflated data too large for '%s' (%u bytes)", filename.c_str(), (unsigned)result.size());
                    result.clear();
                    break;
                }
            }

            if (ret == Z_STREAM_END) break;
            if (ret != Z_OK && ret != Z_BUF_ERROR) {
                ESP_LOGE(TAG, "Inflate failed: %d", ret);
                result.clear();
                break;
            }

            if ((result.size() & 0x3FFF) == 0) vTaskDelay(1); // yield occasionally
        }

        inflateEnd(&strm);
    } else {
        ESP_LOGE(TAG, "Unsupported compression method: %u", info.compressionMethod);
    }

    fclose(f);
    return result;
}

bool ZipReader::extractFile(const std::string& filename, bool (*callback)(const char*, size_t, void*), void* context) {
    esp_task_wdt_reset();
    const ZipFileInfo* infoPtr = findFile(filename);
    if (!infoPtr) {
        ESP_LOGW(TAG, "File not found in zip: %s", filename.c_str());
        return false;
    }

    const ZipFileInfo& info = *infoPtr;
    FILE* f = fopen(filePath.c_str(), "rb");
    if (!f) return false;

    esp_task_wdt_reset();
    // Go to Local File Header
    fseek(f, info.localHeaderOffset, SEEK_SET);
    
    uint32_t sig;
    fread(&sig, 1, 4, f);
    if (sig != LFH_SIGNATURE) {
        fclose(f);
        return false;
    }

    // Skip to name length in LFH
    fseek(f, 22, SEEK_CUR); // Skip to name len
    uint16_t nameLen, extraLen;
    fread(&nameLen, 1, 2, f);
    fread(&extraLen, 1, 2, f);
    
    // Skip name and extra field to get to data
    fseek(f, nameLen + extraLen, SEEK_CUR);
    
    size_t compressedSize = info.compressedSize;
    if (compressedSize == 0) {
        fclose(f);
        return true;
    }

    constexpr size_t IN_CHUNK = 2048;
    constexpr size_t OUT_CHUNK = 4096;
    std::unique_ptr<uint8_t, decltype(&heap_caps_free)> inBuf(
        (uint8_t*)allocZipBuffer(IN_CHUNK),
        heap_caps_free);
    std::unique_ptr<uint8_t, decltype(&heap_caps_free)> outBuf(
        (uint8_t*)allocZipBuffer(OUT_CHUNK),
        heap_caps_free);
    if (!inBuf || !outBuf) {
        ESP_LOGE(TAG, "OOM allocating IO buffers");
        fclose(f);
        return false;
    }

    if (info.compressionMethod == 0) {
        // Stored (no compression)
        size_t remaining = compressedSize;
        while (remaining > 0) {
            size_t toRead = remaining > IN_CHUNK ? IN_CHUNK : remaining;
            size_t bytesRead = fread(inBuf.get(), 1, toRead, f);
            if (bytesRead == 0) break;
            
            if (!callback(reinterpret_cast<const char*>(inBuf.get()), bytesRead, context)) {
                break;
            }

            remaining -= bytesRead;
            if ((remaining & 0xFFF) == 0) { // Yield every 4KB
                esp_task_wdt_reset();
                vTaskDelay(1); // yield occasionally
            }
        }
    } else if (info.compressionMethod == 8) {
        // Deflate streaming
        z_stream strm;
        memset(&strm, 0, sizeof(strm));
        if (inflateInit2(&strm, -MAX_WBITS) != Z_OK) {
            fclose(f);
            return false;
        }

        size_t remaining = compressedSize;
        int ret = Z_OK;

        while (ret != Z_STREAM_END) {
            if (strm.avail_in == 0 && remaining > 0) {
                size_t toRead = remaining > IN_CHUNK ? IN_CHUNK : remaining;
                size_t bytesRead = fread(inBuf.get(), 1, toRead, f);
                if (bytesRead == 0) {
                    ESP_LOGE(TAG, "Unexpected EOF while inflating '%s'", filename.c_str());
                    break;
                }
                remaining -= bytesRead;
                strm.next_in = inBuf.get();
                strm.avail_in = bytesRead;
            }

            strm.next_out = outBuf.get();
            strm.avail_out = OUT_CHUNK;
            ret = inflate(&strm, remaining ? Z_NO_FLUSH : Z_FINISH);

            size_t have = OUT_CHUNK - strm.avail_out;
            if (have) {
                if (!callback(reinterpret_cast<const char*>(outBuf.get()), have, context)) {
                    inflateEnd(&strm);
                    fclose(f);
                    return true; // User aborted, but not an error
                }
            }

            if (ret == Z_STREAM_END) break;
            if (ret != Z_OK && ret != Z_BUF_ERROR) {
                ESP_LOGE(TAG, "Inflate failed: %d", ret);
                break;
            }

            // Yield occasionally
            if ((strm.total_out & 0xFFF) == 0) { // Yield every 4KB
                esp_task_wdt_reset();
                vTaskDelay(1);
            }
        }

        inflateEnd(&strm);
    } else {
        ESP_LOGE(TAG, "Unsupported compression method: %u", info.compressionMethod);
    }

    fclose(f);
    return true;
}

uint32_t ZipReader::getUncompressedSize(const std::string& filename) {
    const ZipFileInfo* info = findFile(filename);
    return info ? info->uncompressedSize : 0;
}

bool ZipReader::extractBinary(const std::string& filename, std::vector<uint8_t>& outData) {
    outData.clear();
    
    const ZipFileInfo* infoPtr = findFile(filename);
    if (!infoPtr) {
        ESP_LOGW(TAG, "File not found in zip: %s", filename.c_str());
        return false;
    }

    const ZipFileInfo& info = *infoPtr;
    
    // Safety limit: don't extract files larger than 5MB to avoid OOM
    if (info.uncompressedSize > 5 * 1024 * 1024) {
        ESP_LOGW(TAG, "File too large to extract: %s (%u bytes)", filename.c_str(), (unsigned)info.uncompressedSize);
        return false;
    }

    // Pre-allocate for efficiency
    if (info.uncompressedSize > 0) {
        outData.reserve(info.uncompressedSize);
    }
    
    // Use callback-based extraction for streaming
    struct BinaryContext {
        std::vector<uint8_t>* data;
    };
    
    BinaryContext ctx;
    ctx.data = &outData;
    
    bool success = extractFile(filename, [](const char* data, size_t len, void* context) -> bool {
        BinaryContext* ctx = (BinaryContext*)context;
        const uint8_t* bytes = (const uint8_t*)data;
        ctx->data->insert(ctx->data->end(), bytes, bytes + len);
        return true;
    }, &ctx);
    
    if (!success || outData.empty()) {
        ESP_LOGE(TAG, "Failed to extract binary: %s", filename.c_str());
        outData.clear();
        return false;
    }
    
    ESP_LOGI(TAG, "Extracted binary '%s': %u bytes", filename.c_str(), (unsigned)outData.size());
    return true;
}

size_t ZipReader::peekFile(const std::string& filename, uint8_t* outData, size_t size) {
    
    const ZipFileInfo* infoPtr = findFile(filename);
    if (!infoPtr) {
        return 0;
    }

    const ZipFileInfo& info = *infoPtr;
    FILE* f = fopen(filePath.c_str(), "rb");
    if (!f) return 0;

    // Go to Local File Header
    fseek(f, info.localHeaderOffset, SEEK_SET);
    
    uint32_t sig;
    fread(&sig, 1, 4, f);
    if (sig != LFH_SIGNATURE) {
        fclose(f);
        return 0;
    }

    // Skip to name length in LFH
    fseek(f, 22, SEEK_CUR); // Skip to name len
    uint16_t nameLen, extraLen;
    fread(&nameLen, 1, 2, f);
    fread(&extraLen, 1, 2, f);
    
    // Skip name and extra field to get to data
    fseek(f, nameLen + extraLen, SEEK_CUR);
    
    size_t bytesRead = 0;
    
    if (info.compressionMethod == 0) {
        // Stored
        size_t toRead = size;
        if (toRead > info.compressedSize) toRead = info.compressedSize;
        bytesRead = fread(outData, 1, toRead, f);
    } else if (info.compressionMethod == 8) {
        // Deflate - need to inflate just enough
        // We can't easily peek compressed data without inflating
        // But we only need a few bytes, so we can use a small buffer
        
        constexpr size_t IN_CHUNK = 256; // Small chunk for header
        uint8_t inBuf[IN_CHUNK];
        
        z_stream strm;
        memset(&strm, 0, sizeof(strm));
        if (inflateInit2(&strm, -MAX_WBITS) == Z_OK) {
            strm.next_out = outData;
            strm.avail_out = size;
            
            size_t remaining = info.compressedSize;
            int ret = Z_OK;
            
            while (ret != Z_STREAM_END && strm.avail_out > 0) {
                if (strm.avail_in == 0 && remaining > 0) {
                    size_t toRead = remaining > IN_CHUNK ? IN_CHUNK : remaining;
                    size_t r = fread(inBuf, 1, toRead, f);
                    if (r == 0) break;
                    remaining -= r;
                    strm.next_in = inBuf;
                    strm.avail_in = r;
                }
                
                ret = inflate(&strm, Z_NO_FLUSH);
                if (ret != Z_OK && ret != Z_STREAM_END) break;
            }

            bytesRead = size - strm.avail_out;
            inflateEnd(&strm);
        }
    }
    
    fclose(f);
    return bytesRead;
}

std::vector<std::string> ZipReader::listFiles() const {
    std::vector<std::string> result;
    result.reserve(files.size());
    for (const auto& info : files) {
        result.push_back(std::string((char*)cdBuffer.get() + info.nameOffset, info.nameLen));
    }
    return result;
}
