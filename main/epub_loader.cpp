#include "epub_loader.h"
#include "image_handler.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include <stdio.h>
#include <string.h>
#include <map>
#include <unistd.h>
#include <algorithm>
#include <cctype>
#include <limits>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "EPUB";
static const int PAGE_SIZE = 800; // Characters per page (approx)
static const size_t INVALID_CHAPTER_LENGTH = std::numeric_limits<size_t>::max();

struct SkipCheckContext {
    std::string buffer;
};

static bool skipCheckCallback(const char* data, size_t len, void* ctx) {
    SkipCheckContext* c = (SkipCheckContext*)ctx;
    c->buffer.append(data, len);
    if (c->buffer.length() > 2048) {
        return false; // Stop reading
    }
    return true;
}

bool EpubLoader::isChapterSkippable(int index) {
    if (index < 0 || index >= spine.size()) return false;
    
    SkipCheckContext ctx;
    
    // Use the callback version to read only the beginning
    zip.extractFile(spine[index], skipCheckCallback, &ctx);
    
    std::string& lower = ctx.buffer;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c){ return std::tolower(c); });
    
    // Heuristics
    if (lower.find("cover img") != std::string::npos) return true;
    if (lower.find("copyright") != std::string::npos) return true;
    if (lower.find("table of contents") != std::string::npos) return true;
    if (lower.find("contents") != std::string::npos && lower.length() < 500) return true; // Short "Contents" page
    if (lower.find("dedication") != std::string::npos && lower.length() < 500) return true;
    
    return false;
}

bool EpubLoader::load(const char* path, int restoreChapterIndex) {
    close();
    currentPath = path;
    ESP_LOGI(TAG, "Loading %s", path);
    
    if (!zip.open(path)) {
        ESP_LOGE(TAG, "Failed to open ZIP");
        return false;
    }
    isOpen = true;
    
    if (!parseContainer()) {
        ESP_LOGE(TAG, "Failed to parse container.xml");
        close();
        return false;
    }
    
    if (spine.empty()) {
        ESP_LOGE(TAG, "Empty spine");
        close();
        return false;
    }

    chapterTextLengths.assign(spine.size(), INVALID_CHAPTER_LENGTH);
    
    currentChapterIndex = 0;
    
    if (restoreChapterIndex != -1) {
        ESP_LOGI(TAG, "Restoring directly to chapter %d", restoreChapterIndex);
        currentChapterIndex = restoreChapterIndex;
        // Validate index
        if (currentChapterIndex >= spine.size()) currentChapterIndex = 0;
    } else {
        // Heuristic: Skip intro chapters
        if (spine.size() > 1) {
            int maxSkip = std::min((int)spine.size() - 1, 5); 
            for (int i = 0; i < maxSkip; ++i) {
                esp_task_wdt_reset();
                if (isChapterSkippable(i)) {
                    ESP_LOGI(TAG, "Skipping chapter %d (heuristic)", i);
                    currentChapterIndex++;
                } else {
                    break;
                }
            }
        }
    }

    currentTextOffset = 0;
    loadChapter(currentChapterIndex);
    esp_task_wdt_reset();

    // If the loaded chapter is very small (likely empty or just an image wrapper), try next
    // Limit this to a few chapters to avoid infinite loop
    // Only do this if NOT restoring, or if the restored chapter is weirdly empty (though if we saved it, it was probably fine)
    if (restoreChapterIndex == -1) {
        int retries = 0;
        while (currentChapterSize < 50 && currentChapterIndex < spine.size() - 1 && retries < 5) {
            esp_task_wdt_reset();
            ESP_LOGI(TAG, "Chapter %d is too small (%u bytes), skipping...", currentChapterIndex, (unsigned)currentChapterSize);
            currentChapterIndex++;
            loadChapter(currentChapterIndex);
            retries++;
        }
    }
    esp_task_wdt_reset();
    
    return true;
}

bool EpubLoader::loadMetadataOnly(const char* path) {
    close();
    currentPath = path;
    ESP_LOGI(TAG, "Loading metadata only for %s", path);

    if (!zip.open(path)) {
        ESP_LOGE(TAG, "Failed to open ZIP (metadata)");
        return false;
    }
    isOpen = true;
    esp_task_wdt_reset();

    if (!parseContainer()) {
        ESP_LOGE(TAG, "Failed to parse container.xml (metadata)");
        close();
        return false;
    }

    // Metadata is parsed inside parseOPF -> title/language are now set.
    close(); // We don't need to keep the ZIP open for metadata.
    return true;
}

void EpubLoader::close() {
    if (isOpen) {
        zip.close();
        isOpen = false;
    }
    spine.clear();
    chapterTextLengths.clear();
    currentChapterContent.clear();
    currentChapterContent.shrink_to_fit();
    currentChapterImages.clear();
    manifest.clear();
}

std::string EpubLoader::getTitle() {
    if (title.empty()) return "Unknown Title";
    return title;
}

std::string EpubLoader::getAuthor() {
    if (author.empty()) return "";
    return author;
}

std::string EpubLoader::readFileFromZip(const std::string& path) {
    return zip.extractFile(path);
}

bool EpubLoader::parseContainer() {
    std::string xml = readFileFromZip("META-INF/container.xml");
    if (xml.empty()) return false;
    
    // Find full-path attribute
    const char* key = "full-path=\"";
    size_t pos = xml.find(key);
    if (pos == std::string::npos) return false;
    
    pos += strlen(key);
    size_t end = xml.find("\"", pos);
    if (end == std::string::npos) return false;
    
    std::string opfPath = xml.substr(pos, end - pos);
    
    // Set root dir
    size_t lastSlash = opfPath.rfind('/');
    if (lastSlash != std::string::npos) {
        rootDir = opfPath.substr(0, lastSlash + 1);
    } else {
        rootDir = "";
    }
    
    return parseOPF(opfPath);
}

bool EpubLoader::parseOPF(const std::string& opfPath) {
    std::string xml = readFileFromZip(opfPath);
    if (xml.empty()) return false;
    
    // Parse Title
    title = "";
    size_t titlePos = xml.find("<dc:title");
    if (titlePos != std::string::npos) {
        size_t close = xml.find(">", titlePos);
        if (close != std::string::npos) {
            size_t end = xml.find("</dc:title>", close);
            if (end != std::string::npos) {
                title = xml.substr(close + 1, end - (close + 1));
            }
        }
    }
    if (title.empty()) title = "Unknown Title";

    // Parse Author (dc:creator)
    author = "";
    size_t authorPos = xml.find("<dc:creator");
    if (authorPos != std::string::npos) {
        size_t close = xml.find(">", authorPos);
        if (close != std::string::npos) {
            size_t end = xml.find("</dc:creator>", close);
            if (end != std::string::npos) {
                author = xml.substr(close + 1, end - (close + 1));
            }
        }
    }

    // Parse Language
    language = "en"; // Default
    size_t langPos = xml.find("<dc:language");
    if (langPos != std::string::npos) {
        size_t close = xml.find(">", langPos);
        if (close != std::string::npos) {
            size_t end = xml.find("</dc:language>", close);
            if (end != std::string::npos) {
                language = xml.substr(close + 1, end - (close + 1));
            }
        }
    }
    
    // Clear and rebuild manifest
    manifest.clear();
    
    // 1. Parse Manifest
    // <item id="id" href="path" ... />
    size_t pos = 0;
    while (true) {
        esp_task_wdt_reset();
        pos = xml.find("<item ", pos);
        if (pos == std::string::npos) break;
        
        size_t endTag = xml.find(">", pos);
        if (endTag == std::string::npos) break;
        
        std::string tag = xml.substr(pos, endTag - pos);
        
        // Extract ID
        std::string id, href;
        
        size_t idPos = tag.find("id=\"");
        if (idPos != std::string::npos) {
            size_t idEnd = tag.find("\"", idPos + 4);
            id = tag.substr(idPos + 4, idEnd - (idPos + 4));
        }
        
        size_t hrefPos = tag.find("href=\"");
        if (hrefPos != std::string::npos) {
            size_t hrefEnd = tag.find("\"", hrefPos + 6);
            href = tag.substr(hrefPos + 6, hrefEnd - (hrefPos + 6));
        }
        
        if (!id.empty() && !href.empty()) {
            manifest[id] = href;
        }
        
        pos = endTag;
    }
    
    // 2. Parse Spine
    // <itemref idref="id" />
    pos = xml.find("<spine");
    if (pos == std::string::npos) return false;
    
    while (true) {
        esp_task_wdt_reset();
        pos = xml.find("<itemref ", pos);
        if (pos == std::string::npos) break;
        
        size_t endTag = xml.find(">", pos);
        std::string tag = xml.substr(pos, endTag - pos);
        
        size_t idrefPos = tag.find("idref=\"");
        if (idrefPos != std::string::npos) {
            size_t idEnd = tag.find("\"", idrefPos + 7);
            std::string idref = tag.substr(idrefPos + 7, idEnd - (idrefPos + 7));
            
            if (manifest.count(idref)) {
                spine.push_back(rootDir + manifest[idref]);
            }
        }
        pos = endTag;
    }
    
    return !spine.empty();
}

struct LoadChapterContext {
    std::string* content;
    std::vector<EpubImage>* images;
    std::string chapterDir;  // Directory of current chapter for relative path resolution
    std::string rootDir;     // EPUB root directory
    bool inTag;
    bool lastSpace;
    std::string currentTag;
};

struct ChapterLengthContext {
    size_t length = 0;
    bool inTag = false;
    bool lastSpace = true;
    std::string currentTag;
    size_t processedSinceYield = 0;
    TickType_t lastYieldTick = 0;
};

static void appendSeparatorIfNeeded(ChapterLengthContext* ctx) {
    if (!ctx) return;
    if (!ctx->lastSpace) {
        ctx->length++;
        ctx->lastSpace = true;
    }
}

static bool chapterLengthCallback(const char* data, size_t len, void* ctx) {
    ChapterLengthContext* context = static_cast<ChapterLengthContext*>(ctx);
    if (!context) return false;

    for (size_t i = 0; i < len; ++i) {
        char c = data[i];
        if (c == '<') {
            context->inTag = true;
            context->currentTag.clear();
            continue;
        }

        if (context->inTag) {
            if (c == '>') {
                context->inTag = false;

                std::string& tag = context->currentTag;
                bool isBlock = false;
                if (tag == "p" || tag == "/p" || tag.find("p ") == 0) isBlock = true;
                else if (tag == "div" || tag == "/div" || tag.find("div ") == 0) isBlock = true;
                else if (tag == "br" || tag == "br/" || tag.find("br ") == 0) isBlock = true;
                else if (tag == "li" || tag == "/li") isBlock = true;
                else if (tag.length() >= 2 && (tag[0] == 'h' || (tag[0] == '/' && tag[1] == 'h'))) isBlock = true;

                if (isBlock) {
                    appendSeparatorIfNeeded(context);
                } else if (tag == "img" || tag.find("img ") == 0) {
                    context->length += 7; // "[Image]"
                    context->lastSpace = false;
                }
            } else {
                context->currentTag += c;
            }
            continue;
        }

        // Outside of tag
        if (c == '\n' || c == '\r') {
            c = ' ';
        }

        if (c == ' ') {
            if (!context->lastSpace) {
                context->length++;
                context->lastSpace = true;
            }
        } else {
            context->length++;
            context->lastSpace = false;
        }

        // Give FreeRTOS a chance to run idle/other tasks during long scans
        context->processedSinceYield++;
        TickType_t now = xTaskGetTickCount();
        if (context->processedSinceYield >= 2048 || (now - context->lastYieldTick) >= pdMS_TO_TICKS(20)) {
            vTaskDelay(1);
            context->processedSinceYield = 0;
            context->lastYieldTick = xTaskGetTickCount();
        }
    }
    return true;
}

// Helper to extract attribute value from tag string
static std::string extractAttribute(const std::string& tag, const std::string& attr) {
    std::string search = attr + "=\"";
    size_t pos = tag.find(search);
    if (pos == std::string::npos) {
        // Try single quotes
        search = attr + "='";
        pos = tag.find(search);
    }
    if (pos == std::string::npos) return "";
    
    pos += search.length();
    char quote = (search.back() == '"') ? '"' : '\'';
    size_t end = tag.find(quote, pos);
    if (end == std::string::npos) return "";
    
    return tag.substr(pos, end - pos);
}

// Helper to decode HTML entities in a string
static std::string decodeHtmlEntities(const std::string& str) {
    std::string result;
    result.reserve(str.length());
    
    for (size_t i = 0; i < str.length(); ++i) {
        if (str[i] == '&') {
            // Look for entity
            size_t end = str.find(';', i);
            if (end != std::string::npos && end - i < 10) {
                std::string entity = str.substr(i + 1, end - i - 1);
                if (entity == "amp") result += '&';
                else if (entity == "lt") result += '<';
                else if (entity == "gt") result += '>';
                else if (entity == "quot") result += '"';
                else if (entity == "apos") result += '\'';
                else if (entity == "nbsp") result += ' ';
                else if (entity[0] == '#') {
                    // Numeric entity
                    int code = 0;
                    if (entity[1] == 'x' || entity[1] == 'X') {
                        code = strtol(entity.c_str() + 2, nullptr, 16);
                    } else {
                        code = atoi(entity.c_str() + 1);
                    }
                    if (code > 0 && code < 128) result += (char)code;
                    else result += '?';
                } else {
                    // Unknown entity, keep as-is
                    result += str.substr(i, end - i + 1);
                }
                i = end;
                continue;
            }
        }
        result += str[i];
    }
    return result;
}

// Helper to resolve relative image path
static std::string resolveRelativePath(const std::string& basePath, const std::string& relativePath) {
    // Handle absolute paths (starting with /)
    if (!relativePath.empty() && relativePath[0] == '/') {
        return relativePath.substr(1);  // Remove leading /
    }
    
    // Handle ../ navigation
    std::string base = basePath;
    std::string rel = relativePath;
    
    while (rel.length() >= 3 && rel.substr(0, 3) == "../") {
        rel = rel.substr(3);
        // Remove last component from base
        size_t lastSlash = base.rfind('/');
        if (lastSlash != std::string::npos) {
            base = base.substr(0, lastSlash);
            // Try to find another slash
            lastSlash = base.rfind('/');
            if (lastSlash != std::string::npos) {
                base = base.substr(0, lastSlash + 1);
            } else {
                base = "";
            }
        }
    }
    
    return base + rel;
}

static bool loadChapterCallback(const char* data, size_t len, void* ctx) {
    LoadChapterContext* context = (LoadChapterContext*)ctx;
    for (size_t i = 0; i < len; ++i) {
        char c = data[i];
        if (c == '<') {
            context->inTag = true;
            context->currentTag.clear();
            continue;
        }
        
        if (context->inTag) {
            if (c == '>') {
                context->inTag = false;
                
                // Check for block tags to insert newlines
                std::string& tag = context->currentTag;
                bool isBlock = false;
                if (tag == "p" || tag == "/p" || tag.find("p ") == 0) isBlock = true;
                else if (tag == "div" || tag == "/div" || tag.find("div ") == 0) isBlock = true;
                else if (tag == "br" || tag == "br/" || tag.find("br ") == 0) isBlock = true;
                else if (tag == "li" || tag == "/li") isBlock = true;
                else if (tag.length() >= 2 && (tag[0] == 'h' || (tag[0] == '/' && tag[1] == 'h'))) isBlock = true;

                if (isBlock) {
                    // Insert newline if not already there
                    if (!context->lastSpace) {
                        context->content->push_back('\n');
                        context->lastSpace = true; 
                    }
                } else if (tag == "img" || tag.find("img ") == 0) {
                    // Parse image tag and store reference
                    EpubImage img;
                    img.textOffset = context->content->length();
                    
                    // Extract src attribute
                    std::string src = extractAttribute(tag, "src");
                    if (src.empty()) {
                        // Try xlink:href for SVG images
                        src = extractAttribute(tag, "xlink:href");
                    }
                    
                    if (!src.empty()) {
                        // Decode HTML entities in src
                        src = decodeHtmlEntities(src);
                        
                        // Resolve relative path
                        img.path = resolveRelativePath(context->chapterDir, src);
                        
                        // Extract alt text
                        img.alt = extractAttribute(tag, "alt");
                        if (!img.alt.empty()) {
                            img.alt = decodeHtmlEntities(img.alt);
                        }
                        
                        // Extract dimensions if specified
                        std::string widthStr = extractAttribute(tag, "width");
                        std::string heightStr = extractAttribute(tag, "height");
                        img.width = widthStr.empty() ? -1 : atoi(widthStr.c_str());
                        img.height = heightStr.empty() ? -1 : atoi(heightStr.c_str());
                        
                        // Determine if block-level (heuristic: large images or in figure/div)
                        // For now, treat images > 200px as block-level
                        img.isBlock = (img.width > 200 || img.height > 200);
                        
                        // Store image reference
                        if (context->images) {
                            context->images->push_back(img);
                        }
                    }
                    
                    // Output placeholder in text
                    context->content->append("\xEE\x80\x80");  // Custom placeholder character (U+E000)
                    context->lastSpace = false;
                } else if (tag.find("image ") == 0 || tag == "image") {
                    // SVG <image> tag
                    EpubImage img;
                    img.textOffset = context->content->length();
                    
                    std::string href = extractAttribute(tag, "xlink:href");
                    if (href.empty()) {
                        href = extractAttribute(tag, "href");
                    }
                    
                    if (!href.empty()) {
                        href = decodeHtmlEntities(href);
                        img.path = resolveRelativePath(context->chapterDir, href);
                        img.alt = "";
                        img.isBlock = true;
                        
                        std::string widthStr = extractAttribute(tag, "width");
                        std::string heightStr = extractAttribute(tag, "height");
                        img.width = widthStr.empty() ? -1 : atoi(widthStr.c_str());
                        img.height = heightStr.empty() ? -1 : atoi(heightStr.c_str());
                        
                        if (context->images) {
                            context->images->push_back(img);
                        }
                    }
                    
                    context->content->append("\xEE\x80\x80");
                    context->lastSpace = false;
                }
                // For inline tags like <b>, <i>, no action needed
            } else {
                context->currentTag += c;
            }
            continue;
        }
        
        if (!context->inTag) {
            // Skip newlines in HTML source
            if (c == '\n' || c == '\r') {
                c = ' ';
            }
            
            if (c == ' ') {
                if (!context->lastSpace) {
                    context->content->push_back(c);
                    context->lastSpace = true;
                }
            } else {
                context->content->push_back(c);
                context->lastSpace = false;
            }
        }
    }
    return true;
}

void EpubLoader::loadChapter(int index) {
    if (index < 0 || index >= spine.size()) return;
    
    currentChapterIndex = index;
    currentTextOffset = 0;
    currentChapterSize = 0;
    currentChapterContent.clear();
    currentChapterImages.clear();
    
    // Determine chapter directory for relative path resolution
    const std::string& chapterPath = spine[index];
    size_t lastSlash = chapterPath.rfind('/');
    if (lastSlash != std::string::npos) {
        currentChapterDir = chapterPath.substr(0, lastSlash + 1);
    } else {
        currentChapterDir = "";
    }
    
    // Reserve memory based on uncompressed size to avoid reallocations
    uint32_t size = zip.getUncompressedSize(spine[index]);
    if (size > 0) {
        // Reserve full size to ensure contiguous allocation in PSRAM (if enabled)
        // We might use less due to tag stripping, but over-reservation is safer than reallocation
        ESP_LOGI(TAG, "Reserving %u bytes for chapter", (unsigned)size);
        currentChapterContent.reserve(size);
    } else {
        currentChapterContent.reserve(8192); 
    } 

    LoadChapterContext* ctx = new LoadChapterContext();
    ctx->content = &currentChapterContent;
    ctx->images = &currentChapterImages;
    ctx->chapterDir = currentChapterDir;
    ctx->rootDir = rootDir;
    ctx->inTag = false;
    ctx->lastSpace = true; // Start assuming we are at start of line (no leading spaces)

    // Stream process the HTML file directly from ZIP to text
    // This avoids loading the full HTML into memory
    zip.extractFile(spine[index], loadChapterCallback, ctx);

    delete ctx;

    currentChapterSize = currentChapterContent.length();
    if (chapterTextLengths.size() == spine.size()) {
        chapterTextLengths[currentChapterIndex] = currentChapterSize;
    }
    
    ESP_LOGI(TAG, "Loaded chapter %d, size: %u, images: %zu", index, (unsigned)currentChapterSize, currentChapterImages.size());
}

bool EpubLoader::jumpToChapter(int index) {
    if (index < 0 || index >= spine.size()) return false;
    loadChapter(index);
    return true;
}

std::string EpubLoader::getText(size_t offset, size_t length) {
    if (currentChapterSize == 0) return "";
    if (offset >= currentChapterSize) return "";
    
    if (offset + length > currentChapterSize) {
        length = currentChapterSize - offset;
    }
    
    return currentChapterContent.substr(offset, length);
}

size_t EpubLoader::getChapterTextLength(int index) {
    if (index < 0 || index >= spine.size()) return 0;
    if (!isOpen) return 0;

    if (chapterTextLengths.size() != spine.size()) {
        chapterTextLengths.assign(spine.size(), INVALID_CHAPTER_LENGTH);
    }

    if (index == currentChapterIndex && currentChapterSize > 0) {
        chapterTextLengths[index] = currentChapterSize;
        return currentChapterSize;
    }

    if (chapterTextLengths[index] != INVALID_CHAPTER_LENGTH) {
        return chapterTextLengths[index];
    }

    ChapterLengthContext ctx;
    ctx.lastYieldTick = xTaskGetTickCount();
    zip.extractFile(spine[index], chapterLengthCallback, &ctx);
    chapterTextLengths[index] = ctx.length;
    return ctx.length;
}

bool EpubLoader::nextChapter() {
    if (currentChapterIndex < spine.size() - 1) {
        loadChapter(currentChapterIndex + 1);
        return true;
    }
    return false;
}

bool EpubLoader::prevChapter() {
    if (currentChapterIndex > 0) {
        loadChapter(currentChapterIndex - 1);
        return true;
    }
    return false;
}

// ============================================================================
// Image Support
// ============================================================================

bool EpubLoader::extractImage(const std::string& imagePath, std::vector<uint8_t>& outData) {
    if (!isOpen) {
        ESP_LOGE(TAG, "EPUB not open");
        return false;
    }
    
    std::string finalPath = "";
    
    // Try the path as-is first
    if (zip.fileExists(imagePath)) {
        finalPath = imagePath;
    } else {
        // Try with rootDir prefix
        std::string fullPath = rootDir + imagePath;
        if (zip.fileExists(fullPath)) {
            finalPath = fullPath;
        } else {
            // Try without any prefix (image might be at root)
            size_t lastSlash = imagePath.rfind('/');
            if (lastSlash != std::string::npos) {
                std::string filename = imagePath.substr(lastSlash + 1);
                // Search in common image directories
                std::vector<std::string> searchPaths = {
                    "images/" + filename,
                    "Images/" + filename,
                    "OEBPS/images/" + filename,
                    "OEBPS/Images/" + filename,
                    rootDir + "images/" + filename,
                    rootDir + "Images/" + filename,
                    filename
                };
                
                for (const auto& path : searchPaths) {
                    if (zip.fileExists(path)) {
                        finalPath = path;
                        ESP_LOGI(TAG, "Found image at: %s (searched for: %s)", path.c_str(), imagePath.c_str());
                        break;
                    }
                }
            }
        }
    }
    
    if (finalPath.empty()) {
        ESP_LOGW(TAG, "Image not found: %s", imagePath.c_str());
        return false;
    }

    // Peek header to check dimensions before extracting full binary
    uint8_t header[64]; // Enough for PNG and most JPEGs
    size_t peekSize = zip.peekFile(finalPath, header, sizeof(header));
    if (peekSize > 0) {
        int w, h;
        if (ImageHandler::getInstance().getDimensions(header, peekSize, w, h)) {
            // If image is extremely large (e.g. > 4000x4000), skip it to avoid OOM or watchdog
            // Note: ImageHandler will allow up to MAX_IMAGE_DIMENSION (1920) *after scaling*
            // But we need to be careful about the extraction buffer too.
            // ZipReader::extractBinary limits to 5MB.
            
            if (w > 4000 || h > 4000) {
                ESP_LOGW(TAG, "Image too large to process: %dx%d (%s)", w, h, finalPath.c_str());
                return false;
            }
        }
    }
    
    return zip.extractBinary(finalPath, outData);
}

const EpubImage* EpubLoader::findImageAtOffset(size_t textOffset, size_t tolerance) const {
    for (const auto& img : currentChapterImages) {
        if (textOffset >= img.textOffset && textOffset <= img.textOffset + tolerance) {
            return &img;
        }
        if (img.textOffset >= textOffset && img.textOffset <= textOffset + tolerance) {
            return &img;
        }
    }
    return nullptr;
}

bool EpubLoader::hasImageAtOffset(size_t textOffset) const {
    for (const auto& img : currentChapterImages) {
        if (img.textOffset == textOffset) {
            return true;
        }
    }
    return false;
}

std::string EpubLoader::resolveImagePath(const std::string& src) {
    // Already handled by resolveRelativePath in the callback
    return src;
}