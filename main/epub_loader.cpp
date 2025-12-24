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

// Forward declarations
static std::string decodeHtmlEntities(const std::string& str);

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

bool EpubLoader::load(const char* path, int restoreChapterIndex, bool loadFirstChapter) {
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

    if (loadFirstChapter) {
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
    }
    
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
    chapterTitles.clear();
    chapterTextLengths.clear();
    currentChapterContent.clear();
    currentChapterContent.shrink_to_fit();
    currentChapterImages.clear();
    currentChapterMath.clear();
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
    
    // 1. Parse Manifest
    // <item id="id" href="path" ... />
    struct ManifestItem {
        std::string id;
        std::string href;
    };
    std::vector<ManifestItem> manifest;

    size_t pos = 0;
    
    // Pre-count items to reserve memory and avoid reallocations in internal RAM
    size_t itemCount = 0;
    size_t countPos = 0;
    while ((countPos = xml.find("<item ", countPos)) != std::string::npos) {
        itemCount++;
        countPos += 6;
    }
    manifest.reserve(itemCount);

    pos = 0;
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
            manifest.push_back({id, href});
        }
        
        pos = endTag;
    }

    // Sort manifest by ID for binary search
    std::sort(manifest.begin(), manifest.end(), [](const ManifestItem& a, const ManifestItem& b) {
        return a.id < b.id;
    });
    
    // 2. Find TOC (NCX for EPUB2 or NAV for EPUB3)
    std::string tocPath;
    bool isNcx = false;
    
    // Look for NCX file first (EPUB2 standard)
    for (const auto& item : manifest) {
        std::string hrefLower = item.href;
        std::transform(hrefLower.begin(), hrefLower.end(), hrefLower.begin(), 
                       [](unsigned char c){ return std::tolower(c); });
        if (hrefLower.find(".ncx") != std::string::npos) {
            tocPath = rootDir + item.href;
            isNcx = true;
            break;
        }
    }
    
    // If no NCX, look for NAV document (EPUB3)
    if (tocPath.empty()) {
        for (const auto& item : manifest) {
            // NAV documents typically have "nav" in the id or properties="nav"
            std::string idLower = item.id;
            std::transform(idLower.begin(), idLower.end(), idLower.begin(), 
                           [](unsigned char c){ return std::tolower(c); });
            if (idLower.find("nav") != std::string::npos) {
                tocPath = rootDir + item.href;
                isNcx = false;
                break;
            }
        }
    }
    
    // 3. Parse Spine
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
            
            auto it = std::lower_bound(manifest.begin(), manifest.end(), idref, [](const ManifestItem& item, const std::string& id) {
                return item.id < id;
            });
            if (it != manifest.end() && it->id == idref) {
                spine.push_back(rootDir + it->href);
            }
        }
        pos = endTag;
    }
    
    // 4. Parse TOC to get chapter titles
    if (!tocPath.empty()) {
        parseTOC(tocPath, isNcx);
    }
    
    // If no titles from TOC, generate default titles
    if (chapterTitles.size() < spine.size()) {
        chapterTitles.resize(spine.size());
        for (size_t i = 0; i < spine.size(); i++) {
            if (chapterTitles[i].empty()) {
                // Extract filename without extension
                std::string filename = spine[i];
                size_t lastSlash = filename.rfind('/');
                if (lastSlash != std::string::npos) {
                    filename = filename.substr(lastSlash + 1);
                }
                size_t lastDot = filename.rfind('.');
                if (lastDot != std::string::npos) {
                    filename = filename.substr(0, lastDot);
                }
                // Clean up common prefixes
                if (filename.find("chapter") == 0 || filename.find("Chapter") == 0) {
                    chapterTitles[i] = "Chapter " + std::to_string(i + 1);
                } else if (filename.find("part") == 0 || filename.find("Part") == 0) {
                    chapterTitles[i] = filename;
                } else {
                    // Use filename as-is but capitalize first letter
                    if (!filename.empty() && filename[0] >= 'a' && filename[0] <= 'z') {
                        filename[0] = filename[0] - 32;
                    }
                    chapterTitles[i] = filename;
                }
            }
        }
    }
    
    return !spine.empty();
}

void EpubLoader::parseTOC(const std::string& tocPath, bool isNcx) {
    std::string xml = readFileFromZip(tocPath);
    if (xml.empty()) {
        ESP_LOGW(TAG, "Failed to read TOC file: %s", tocPath.c_str());
        return;
    }
    
    chapterTitles.clear();
    chapterTitles.resize(spine.size());
    
    // Create a map from content href to spine index
    std::map<std::string, int> hrefToIndex;
    for (size_t i = 0; i < spine.size(); i++) {
        std::string href = spine[i];
        // Remove fragment identifier if present (e.g., chapter1.html#section1)
        size_t hashPos = href.find('#');
        if (hashPos != std::string::npos) {
            href = href.substr(0, hashPos);
        }
        hrefToIndex[href] = i;
        
        // Also add version without rootDir for matching
        if (href.find(rootDir) == 0) {
            hrefToIndex[href.substr(rootDir.length())] = i;
        }
    }
    
    if (isNcx) {
        // Parse NCX format (EPUB2)
        // <navPoint id="...">
        //   <navLabel><text>Chapter Title</text></navLabel>
        //   <content src="chapter1.html"/>
        // </navPoint>
        
        size_t pos = 0;
        while ((pos = xml.find("<navPoint", pos)) != std::string::npos) {
            esp_task_wdt_reset();
            
            // Find the closing </navPoint> or next <navPoint>
            size_t endNavPoint = xml.find("</navPoint>", pos);
            size_t nextNavPoint = xml.find("<navPoint", pos + 9);
            
            // Get the section up to nested navPoint or end
            size_t sectionEnd = (nextNavPoint != std::string::npos && nextNavPoint < endNavPoint) 
                                ? nextNavPoint : endNavPoint;
            if (sectionEnd == std::string::npos) break;
            
            std::string section = xml.substr(pos, sectionEnd - pos);
            
            // Extract title from <navLabel><text>...</text></navLabel>
            std::string title;
            size_t textStart = section.find("<text>");
            if (textStart != std::string::npos) {
                textStart += 6;
                size_t textEnd = section.find("</text>", textStart);
                if (textEnd != std::string::npos) {
                    title = section.substr(textStart, textEnd - textStart);
                    // Decode HTML entities
                    title = decodeHtmlEntities(title);
                    // Trim whitespace
                    while (!title.empty() && (title.front() == ' ' || title.front() == '\n' || title.front() == '\r')) {
                        title.erase(0, 1);
                    }
                    while (!title.empty() && (title.back() == ' ' || title.back() == '\n' || title.back() == '\r')) {
                        title.pop_back();
                    }
                }
            }
            
            // Extract content src
            size_t srcStart = section.find("src=\"");
            if (srcStart != std::string::npos) {
                srcStart += 5;
                size_t srcEnd = section.find("\"", srcStart);
                if (srcEnd != std::string::npos) {
                    std::string src = section.substr(srcStart, srcEnd - srcStart);
                    src = decodeHtmlEntities(src);
                    
                    // Remove fragment
                    size_t hashPos = src.find('#');
                    if (hashPos != std::string::npos) {
                        src = src.substr(0, hashPos);
                    }
                    
                    // Try to find in spine
                    std::string fullSrc = rootDir + src;
                    auto it = hrefToIndex.find(fullSrc);
                    if (it == hrefToIndex.end()) {
                        it = hrefToIndex.find(src);
                    }
                    
                    if (it != hrefToIndex.end() && !title.empty()) {
                        chapterTitles[it->second] = title;
                    }
                }
            }
            
            pos = sectionEnd;
        }
    } else {
        // Parse NAV format (EPUB3)
        // <nav epub:type="toc">
        //   <ol>
        //     <li><a href="chapter1.html">Chapter Title</a></li>
        //   </ol>
        // </nav>
        
        // Find the TOC nav section
        size_t navStart = xml.find("epub:type=\"toc\"");
        if (navStart == std::string::npos) {
            navStart = xml.find("type=\"toc\"");
        }
        if (navStart == std::string::npos) {
            // Just look for any nav with ol/li structure
            navStart = xml.find("<nav");
        }
        
        if (navStart != std::string::npos) {
            size_t pos = navStart;
            while ((pos = xml.find("<a ", pos)) != std::string::npos) {
                esp_task_wdt_reset();
                
                size_t tagEnd = xml.find("</a>", pos);
                if (tagEnd == std::string::npos) break;
                
                std::string aTag = xml.substr(pos, tagEnd - pos + 4);
                
                // Extract href
                size_t hrefStart = aTag.find("href=\"");
                if (hrefStart == std::string::npos) {
                    pos = tagEnd;
                    continue;
                }
                hrefStart += 6;
                size_t hrefEnd = aTag.find("\"", hrefStart);
                std::string href = aTag.substr(hrefStart, hrefEnd - hrefStart);
                href = decodeHtmlEntities(href);
                
                // Remove fragment
                size_t hashPos = href.find('#');
                if (hashPos != std::string::npos) {
                    href = href.substr(0, hashPos);
                }
                
                // Extract title (text between > and </a>)
                size_t titleStart = aTag.find('>');
                if (titleStart != std::string::npos) {
                    titleStart++;
                    size_t titleEnd = aTag.find("</a>");
                    if (titleEnd != std::string::npos) {
                        std::string title = aTag.substr(titleStart, titleEnd - titleStart);
                        // Remove any nested tags
                        std::string cleanTitle;
                        bool inTag = false;
                        for (char c : title) {
                            if (c == '<') inTag = true;
                            else if (c == '>') inTag = false;
                            else if (!inTag) cleanTitle += c;
                        }
                        title = decodeHtmlEntities(cleanTitle);
                        
                        // Trim
                        while (!title.empty() && (title.front() == ' ' || title.front() == '\n')) {
                            title.erase(0, 1);
                        }
                        while (!title.empty() && (title.back() == ' ' || title.back() == '\n')) {
                            title.pop_back();
                        }
                        
                        // Try to find in spine
                        std::string fullHref = rootDir + href;
                        auto it = hrefToIndex.find(fullHref);
                        if (it == hrefToIndex.end()) {
                            it = hrefToIndex.find(href);
                        }
                        
                        if (it != hrefToIndex.end() && !title.empty()) {
                            chapterTitles[it->second] = title;
                        }
                    }
                }
                
                pos = tagEnd;
            }
        }
    }
    
    ESP_LOGI(TAG, "Parsed TOC, found %d chapter titles", 
             (int)std::count_if(chapterTitles.begin(), chapterTitles.end(), 
                               [](const std::string& s) { return !s.empty(); }));
}

struct LoadChapterContext {
    std::string* content;
    std::vector<EpubImage>* images;
    std::vector<EpubMath>* mathBlocks;  // Store MathML blocks
    std::string chapterDir;  // Directory of current chapter for relative path resolution
    std::string rootDir;     // EPUB root directory
    bool inTag;
    bool lastSpace;
    std::string currentTag;
    bool inMath; // Track if we are inside a math tag
    int mathDepth;  // Track nested math tags
    std::string currentMathML;  // Accumulate MathML content
    size_t mathStartOffset;  // Where the math placeholder starts
    bool inStyle = false;
    bool inScript = false;
};

struct ChapterLengthContext {
    size_t length = 0;
    bool inTag = false;
    bool lastSpace = true;
    std::string currentTag;
    size_t processedSinceYield = 0;
    TickType_t lastYieldTick = 0;
    bool inStyle = false;
    bool inScript = false;
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
                } else if (tag == "style" || tag.find("style ") == 0) {
                    context->inStyle = true;
                } else if (tag == "/style") {
                    context->inStyle = false;
                } else if (tag == "script" || tag.find("script ") == 0) {
                    context->inScript = true;
                } else if (tag == "/script") {
                    context->inScript = false;
                }
            } else {
                context->currentTag += c;
            }
            continue;
        }

        // Outside of tag
        if (context->inStyle || context->inScript) {
            continue;
        }

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
                else if (entity == "nbsp" || entity == "160") result += ' ';
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
                
                std::string& tag = context->currentTag;

                // Handle MathML tags - extract full MathML content
                if (tag == "math" || tag.find("math ") == 0) {
                    context->inMath = true;
                    context->mathDepth = 1;
                    context->currentMathML = "<" + tag + ">";
                    context->mathStartOffset = context->content->length();
                    // Add placeholder for math - will be replaced by renderer
                    context->content->append(MATH_START);
                    context->lastSpace = false;
                } else if (context->inMath) {
                    // Inside math - accumulate all content
                    context->currentMathML += "<" + tag + ">";
                    
                    if (tag == "/math") {
                        context->mathDepth--;
                        if (context->mathDepth <= 0) {
                            // End of math block - store the MathML
                            context->inMath = false;
                            
                            // Create math block entry
                            if (context->mathBlocks) {
                                EpubMath mathBlock;
                                mathBlock.textOffset = context->mathStartOffset;
                                mathBlock.mathml = context->currentMathML;
                                mathBlock.isBlock = false;  // TODO: detect display math
                                context->mathBlocks->push_back(mathBlock);
                            }
                            
                            context->currentMathML.clear();
                            context->content->append(MATH_END);
                            context->lastSpace = false;
                        }
                    } else if (tag == "math" || tag.find("math ") == 0) {
                        context->mathDepth++;
                    }
                }

                // Check for block tags to insert newlines
                bool isBlock = false;
                if (tag == "p" || tag == "/p" || tag.find("p ") == 0) isBlock = true;
                else if (tag == "div" || tag == "/div" || tag.find("div ") == 0) isBlock = true;
                else if (tag == "br" || tag == "br/" || tag.find("br ") == 0) isBlock = true;
                else if (tag == "li" || tag == "/li") isBlock = true;
                else if (tag.length() >= 2 && (tag[0] == 'h' || (tag[0] == '/' && tag[1] == 'h'))) isBlock = true;

                if (tag == "style" || tag.find("style ") == 0) {
                    // Skip style content
                    // We need to find the end of style tag in the stream.
                    // Since we are streaming char by char, we need a state for "inStyle"
                    // But here we are in a callback.
                    // For simplicity, let's just set a flag in context to ignore content until </style>
                    // Wait, we don't have an "inStyle" flag in context.
                    // Let's add it to LoadChapterContext.
                    ((LoadChapterContext*)ctx)->inStyle = true;
                } else if (tag == "/style") {
                    ((LoadChapterContext*)ctx)->inStyle = false;
                } else if (tag == "script" || tag.find("script ") == 0) {
                    ((LoadChapterContext*)ctx)->inScript = true;
                } else if (tag == "/script") {
                    ((LoadChapterContext*)ctx)->inScript = false;
                }

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
                    img.isInlineMath = false;
                    img.verticalAlign = 0;
                    
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
                        
                        // Extract CSS class - important for detecting inline math images
                        img.cssClass = extractAttribute(tag, "class");
                        
                        // Extract dimensions if specified
                        std::string widthStr = extractAttribute(tag, "width");
                        std::string heightStr = extractAttribute(tag, "height");
                        img.width = widthStr.empty() ? -1 : atoi(widthStr.c_str());
                        img.height = heightStr.empty() ? -1 : atoi(heightStr.c_str());
                        
                        // Detect inline math images by class name patterns
                        // Common patterns: "img2", "inline", "math", "symbol", "formula"
                        std::string lowerClass = img.cssClass;
                        for (auto& c : lowerClass) c = tolower(c);
                        bool hasInlineClass = (lowerClass.find("img2") != std::string::npos ||
                                               lowerClass.find("inline") != std::string::npos ||
                                               lowerClass.find("math") != std::string::npos ||
                                               lowerClass.find("symbol") != std::string::npos ||
                                               lowerClass.find("formula-inline") != std::string::npos);
                        
                        // Also check path for math/formula patterns
                        std::string lowerPath = img.path;
                        for (auto& c : lowerPath) c = tolower(c);
                        bool isMathPath = (lowerPath.find("/r.jpg") != std::string::npos ||  // ℝ symbol
                                          lowerPath.find("/l.jpg") != std::string::npos ||   // L symbol
                                          (lowerPath.find("/images/") != std::string::npos && 
                                              img.width > 0 && img.width < 80 && img.height > 0 && img.height < 40));
                        
                        // Mark as inline math if small size or has inline-related class
                        img.isInlineMath = hasInlineClass || isMathPath || 
                                          (img.width > 0 && img.width < 100 && img.height > 0 && img.height < 50);
                        
                        // Determine if block-level
                        // Block = large images OR not detected as inline math
                        if (img.isInlineMath) {
                            img.isBlock = false;
                        } else {
                            img.isBlock = (img.width > 200 || img.height > 200 || 
                                          (img.width == -1 && img.height == -1));  // Unknown size = assume block
                        }
                        
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
                    img.isInlineMath = false;
                    img.verticalAlign = 0;
                    img.cssClass = "";
                    
                    std::string href = extractAttribute(tag, "xlink:href");
                    if (href.empty()) {
                        href = extractAttribute(tag, "href");
                    }
                    
                    if (!href.empty()) {
                        href = decodeHtmlEntities(href);
                        img.path = resolveRelativePath(context->chapterDir, href);
                        img.alt = "";
                        
                        std::string widthStr = extractAttribute(tag, "width");
                        std::string heightStr = extractAttribute(tag, "height");
                        img.width = widthStr.empty() ? -1 : atoi(widthStr.c_str());
                        img.height = heightStr.empty() ? -1 : atoi(heightStr.c_str());
                        
                        // SVG images are typically block unless small
                        img.isBlock = !(img.width > 0 && img.width < 100 && img.height > 0 && img.height < 50);
                        
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
            // If inside math, accumulate to MathML string instead of main content
            if (context->inMath) {
                // Accumulate text content for MathML
                context->currentMathML += c;
                continue;
            }
            
            if (context->inStyle || context->inScript) {
                continue;
            }
            
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
    currentChapterMath.clear();
    
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
    ctx->mathBlocks = &currentChapterMath;
    ctx->chapterDir = currentChapterDir;
    ctx->rootDir = rootDir;
    ctx->inTag = false;
    ctx->lastSpace = true; // Start assuming we are at start of line (no leading spaces)
    ctx->inMath = false;
    ctx->mathDepth = 0;
    ctx->mathStartOffset = 0;
    ctx->inStyle = false;
    ctx->inScript = false;

    // Stream process the HTML file directly from ZIP to text
    // This avoids loading the full HTML into memory
    zip.extractFile(spine[index], loadChapterCallback, ctx);

    delete ctx;

    currentChapterSize = currentChapterContent.length();
    if (chapterTextLengths.size() == spine.size()) {
        chapterTextLengths[currentChapterIndex] = currentChapterSize;
    }
    
    ESP_LOGI(TAG, "Loaded chapter %d, size: %u, images: %zu, math: %zu", 
             index, (unsigned)currentChapterSize, currentChapterImages.size(), currentChapterMath.size());
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
    ESP_LOGI(TAG, "Extracting image: %s", imagePath.c_str());
    std::string finalPath = "";
    
    // Try the path as-is first
    if (zip.fileExists(imagePath)) {
        finalPath = imagePath;
        ESP_LOGI(TAG, "Found image at: %s", finalPath.c_str());
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
    ESP_LOGI(TAG, "Peeking image header for dimension check: %s", finalPath.c_str());
    size_t peekSize = zip.peekFile(finalPath, header, sizeof(header));
    ESP_LOGI(TAG, "Peeked %u bytes from image header", (unsigned)peekSize);
    if (peekSize > 0) {
        int w, h;
        ESP_LOGI(TAG, "Checking image dimensions");
        if (ImageHandler::getInstance().getDimensions(header, peekSize, w, h)) {
            // If image is extremely large (e.g. > 4000x4000), skip it to avoid OOM or watchdog
            // Note: ImageHandler will allow up to MAX_IMAGE_DIMENSION (1920) *after scaling*
            // But we need to be careful about the extraction buffer too.
            // ZipReader::extractBinary limits to 5MB.
            
            if (w > 4000 || h > 4000) {
                ESP_LOGW(TAG, "Image too large to process: %dx%d (%s)", w, h, finalPath.c_str());
                return false;
            }
            ESP_LOGI(TAG, "Image dimensions: %dx%d", w, h);
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

const EpubMath* EpubLoader::findMathAtOffset(size_t textOffset, size_t tolerance) const {
    for (const auto& math : currentChapterMath) {
        if (textOffset >= math.textOffset && textOffset <= math.textOffset + tolerance) {
            return &math;
        }
        if (math.textOffset >= textOffset && math.textOffset <= textOffset + tolerance) {
            return &math;
        }
    }
    return nullptr;
}

std::string EpubLoader::resolveImagePath(const std::string& src) {
    // Already handled by resolveRelativePath in the callback
    return src;
}