#include "epub_loader.h"
#include "esp_log.h"
#include <stdio.h>
#include <string.h>
#include <map>
#include <unistd.h>
#include <algorithm>
#include <cctype>

static const char *TAG = "EPUB";
static const int PAGE_SIZE = 800; // Characters per page (approx)

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

bool EpubLoader::load(const char* path) {
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
    
    currentChapterIndex = 0;
    
    // Heuristic: Skip intro chapters
    if (spine.size() > 1) {
        int maxSkip = std::min((int)spine.size() - 1, 5); 
        for (int i = 0; i < maxSkip; ++i) {
            if (isChapterSkippable(i)) {
                ESP_LOGI(TAG, "Skipping chapter %d (heuristic)", i);
                currentChapterIndex++;
            } else {
                break;
            }
        }
    }

    currentTextOffset = 0;
    loadChapter(currentChapterIndex);
    
    return true;
}

void EpubLoader::close() {
    if (isOpen) {
        zip.close();
        isOpen = false;
    }
    spine.clear();
    // Remove cache file
    unlink("/spiffs/cache.txt");
}

std::string EpubLoader::getTitle() {
    // Simplified: just return filename or parse metadata if needed
    return "Book";
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
    
    std::map<std::string, std::string> manifest;
    
    // 1. Parse Manifest
    // <item id="id" href="path" ... />
    size_t pos = 0;
    while (true) {
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
    FILE* f;
    char writeBuf[512];
    size_t writeBufPos;
    bool inTag;
    bool lastSpace;
    std::string currentTag;
};

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
                // Simple heuristic: if tag starts with p, div, br, li, h1..h6
                // or closing tags /p, /div, /li, /h1..
                std::string& tag = context->currentTag;
                bool isBlock = false;
                if (tag == "p" || tag == "/p" || tag.find("p ") == 0) isBlock = true;
                else if (tag == "div" || tag == "/div" || tag.find("div ") == 0) isBlock = true;
                else if (tag == "br" || tag == "br/" || tag.find("br ") == 0) isBlock = true;
                else if (tag == "li" || tag == "/li") isBlock = true;
                else if (tag.length() >= 2 && (tag[0] == 'h' || (tag[0] == '/' && tag[1] == 'h'))) isBlock = true;

                if (isBlock) {
                    // Insert newline if not already there
                    if (!context->lastSpace) { // Reuse lastSpace to track if we just outputted a separator
                        context->writeBuf[context->writeBufPos++] = '\n';
                        if (context->writeBufPos == sizeof(context->writeBuf)) {
                            fwrite(context->writeBuf, 1, sizeof(context->writeBuf), context->f);
                            context->writeBufPos = 0;
                        }
                        context->lastSpace = true; 
                    }
                } else {
                    // For inline tags, maybe just a space if needed? 
                    // Actually, usually no space needed for <b>, <i> etc.
                }
            } else {
                context->currentTag += c;
            }
            continue;
        }
        
        if (!context->inTag) {
            // Handle basic entities
            if (c == '&') {
                // TODO: Better entity handling
                // For now, just pass it through or maybe skip if it's complex?
                // Let's pass it through, maybe the font supports it or we render it raw
                // Actually, &nbsp; should be space.
                // Implementing a full entity decoder here is hard without lookahead.
                // Let's just output it.
            }
            // Skip newlines in HTML source as they are usually whitespace
            if (c == '\n' || c == '\r') {
                 // Treat source newlines as spaces
                 c = ' ';
            }
            
            if (c == ' ') {
                if (!context->lastSpace) {
                    context->writeBuf[context->writeBufPos++] = c;
                    if (context->writeBufPos == sizeof(context->writeBuf)) {
                        fwrite(context->writeBuf, 1, sizeof(context->writeBuf), context->f);
                        context->writeBufPos = 0;
                    }
                    context->lastSpace = true;
                }
            } else {
                context->writeBuf[context->writeBufPos++] = c;
                if (context->writeBufPos == sizeof(context->writeBuf)) {
                    fwrite(context->writeBuf, 1, sizeof(context->writeBuf), context->f);
                    context->writeBufPos = 0;
                }
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

    FILE* f = fopen("/spiffs/cache.txt", "wb");
    if (!f) {
        ESP_LOGE(TAG, "Failed to create cache file");
        return;
    }

    LoadChapterContext ctx;
    ctx.f = f;
    ctx.writeBufPos = 0;
    ctx.inTag = false;
    ctx.lastSpace = true; // Start assuming we are at start of line (no leading spaces)

    // Stream process the HTML file directly from ZIP to text
    // This avoids loading the full HTML into memory
    zip.extractFile(spine[index], loadChapterCallback, &ctx);
    
    // Flush remaining
    if (ctx.writeBufPos > 0) {
        fwrite(ctx.writeBuf, 1, ctx.writeBufPos, f);
    }

    currentChapterSize = ftell(f);
    fclose(f);
    
    ESP_LOGI(TAG, "Loaded chapter %d, size: %u", index, (unsigned)currentChapterSize);
}

bool EpubLoader::jumpToChapter(int index) {
    if (index < 0 || index >= spine.size()) return false;
    loadChapter(index);
    return true;
}

std::string EpubLoader::getText(size_t offset, size_t length) {
    if (currentChapterSize == 0) return "";
    if (offset >= currentChapterSize) return "";
    
    FILE* f = fopen("/spiffs/cache.txt", "rb");
    if (!f) return "";

    if (fseek(f, offset, SEEK_SET) != 0) {
        fclose(f);
        return "";
    }

    std::string text;
    text.resize(length);
    size_t read = fread(&text[0], 1, length, f);
    fclose(f);

    text.resize(read);
    return text;
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

