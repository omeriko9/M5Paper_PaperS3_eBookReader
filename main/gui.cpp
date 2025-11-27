#include "gui.h"
#include "web_server.h"
#include "epub_loader.h"
#include "esp_timer.h"
#include "wifi_manager.h"
#include "book_index.h"
#include <vector>
#include <nvs_flash.h>
#include <nvs.h>
#include <algorithm>
#include <lgfx/v1/lgfx_fonts.hpp>
#include "esp_sleep.h" // Added include
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include <dirent.h>
#include <errno.h>
#include <string.h>

static const char *TAG = "GUI";

EpubLoader epubLoader;
extern BookIndex bookIndex;
bool wifiConnected = false;
int wifiRssi = 0;
extern WifiManager wifiManager;
extern WebServer webServer;

static constexpr int STATUS_BAR_HEIGHT = 44;
static constexpr int LIBRARY_LIST_START_Y = 120;
static constexpr int LIBRARY_LINE_HEIGHT = 48;
static constexpr int PAGEINFO_ESTIMATE_MIN_CHARS = 50;
static constexpr uint64_t LIGHT_SLEEP_TO_DEEP_SLEEP_US = 2ULL * 60ULL * 1000000ULL; // 2 minutes

// Helper to split string by delimiters but keep delimiters if needed
// For word wrapping, we split by space.
std::vector<std::string> splitWords(const std::string& text) {
    std::vector<std::string> words;
    std::string currentWord;
    for (char c : text) {
        if (c == ' ' || c == '\n') {
            if (!currentWord.empty()) {
                words.push_back(currentWord);
                currentWord.clear();
            }
            // Add delimiter as separate token if it's a newline, 
            // or just handle space implicitly?
            // Let's handle newline explicitly. Space is implicit separator.
            if (c == '\n') {
                words.push_back("\n");
            }
        } else {
            currentWord += c;
        }
    }
    if (!currentWord.empty()) words.push_back(currentWord);
    return words;
}

// Simple Hebrew detection and reversal for visual display
static bool isHebrew(const std::string& word) {
    for (size_t i = 0; i < word.length(); ++i) {
        unsigned char c = (unsigned char)word[i];
        if (c >= 0xD6 && c <= 0xD7) return true;
    }
    return false;
}

static std::string reverseHebrewWord(const std::string& text) {
    std::string reversed;
    reversed.reserve(text.length());
    for (size_t i = 0; i < text.length(); ) {
        unsigned char c = (unsigned char)text[i];
        size_t charLen = 1;
        if ((c & 0x80) == 0) charLen = 1;
        else if ((c & 0xE0) == 0xC0) charLen = 2;
        else if ((c & 0xF0) == 0xE0) charLen = 3;
        else if ((c & 0xF8) == 0xF0) charLen = 4;
        
        if (i + charLen > text.length()) break;
        reversed.insert(0, text.substr(i, charLen));
        i += charLen;
    }
    return reversed;
}

static std::string processTextForDisplay(const std::string& text) {
    if (!isHebrew(text)) return text;

    // Split into words (space separated)
    // We want to reverse the order of words for RTL.
    // And for each word, if it is Hebrew, reverse the letters.
    // If it is English/Number, keep letters as is.
    
    std::vector<std::string> words;
    std::string current;
    for (char c : text) {
        if (c == ' ' || c == '\n') {
            if (!current.empty()) words.push_back(current);
            std::string delim(1, c);
            words.push_back(delim);
            current.clear();
        } else {
            current += c;
        }
    }
    if (!current.empty()) words.push_back(current);
    
    // Reverse word order
    std::reverse(words.begin(), words.end());
    
    std::string result;
    for (const auto& word : words) {
        if (isHebrew(word)) {
            result += reverseHebrewWord(word);
        } else {
            result += word;
        }
    }
    return result;
}

static bool detectRTLDocument(EpubLoader& loader, size_t sampleOffset) {
    std::string lang = loader.getLanguage();
    if (lang.find("he") != std::string::npos || lang.find("HE") != std::string::npos) {
        return true;
    }

    std::string sample = loader.getText(sampleOffset, 400);
    for (char c : sample) {
        std::string s(1, c);
        if (isHebrew(s)) return true;
    }
    return false;
}

void GUI::init() {
    // Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
      ESP_ERROR_CHECK(nvs_flash_erase());
      ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    loadSettings();
    loadFonts();
    bookIndex.init();
    resetPageInfoCache();

    M5.Display.setTextColor(TFT_BLACK);
    
    // Initialize Canvases for buffering
    // M5Paper has PSRAM, so we should use it for full-screen buffers
    canvasCurrent.setPsram(true);
    canvasCurrent.setColorDepth(16); // Explicitly set 16-bit color depth
    if (canvasCurrent.createSprite(M5.Display.width(), M5.Display.height()) == nullptr) {
        ESP_LOGE(TAG, "Failed to create canvasCurrent");
    }

    canvasNext.setPsram(true);
    canvasNext.setColorDepth(16);
    if (canvasNext.createSprite(M5.Display.width(), M5.Display.height()) == nullptr) {
        ESP_LOGE(TAG, "Failed to create canvasNext");
    }

    canvasPrev.setPsram(true);
    canvasPrev.setColorDepth(16);
    if (canvasPrev.createSprite(M5.Display.width(), M5.Display.height()) == nullptr) {
        ESP_LOGE(TAG, "Failed to create canvasPrev");
    }

    lastActivityTime = (uint32_t)(esp_timer_get_time() / 1000);

    // Check wakeup
    esp_sleep_wakeup_cause_t wakeup_reason = esp_sleep_get_wakeup_cause();
    if (wakeup_reason == ESP_SLEEP_WAKEUP_EXT0 || wakeup_reason == ESP_SLEEP_WAKEUP_TIMER) {
        // Try to load last book
        nvs_handle_t my_handle;
        int32_t lastId = -1;
        if (nvs_open("storage", NVS_READONLY, &my_handle) == ESP_OK) {
            nvs_get_i32(my_handle, "last_book_id", &lastId);
            nvs_close(my_handle);
        }
        
        if (lastId != -1) {
            currentBook = bookIndex.getBook(lastId);
            if (currentBook.id != 0 && epubLoader.load(currentBook.path.c_str())) {
                currentState = AppState::READER;
                
                // Auto-detect language
                std::string lang = epubLoader.getLanguage();
                if (lang.find("he") != std::string::npos || lang.find("HE") != std::string::npos) {
                    setFont("Hebrew");
                }
                // Restore progress
                currentTextOffset = currentBook.currentOffset;
                resetPageInfoCache();
                if (currentBook.currentChapter > 0) {
                    while(epubLoader.getCurrentChapterIndex() < currentBook.currentChapter) {
                        if (!epubLoader.nextChapter()) break;
                    }
                }
                isRTLDocument = detectRTLDocument(epubLoader, currentTextOffset);
                
                pageHistory.clear();
                needsRedraw = false; // Don't redraw, assume E-Ink persisted
                return; 
            }
        }
    }

    M5.Display.fillScreen(TFT_WHITE);
    needsRedraw = true;
}

void GUI::setWifiStatus(bool connected, int rssi) {
    if (wifiConnected != connected || (connected && abs(wifiRssi - rssi) > 10)) {
        wifiConnected = connected;
        wifiRssi = rssi;
        // Only redraw status bar if we are not in a critical animation
        // For POC, just force redraw if in library
        if (currentState == AppState::LIBRARY) needsRedraw = true;
    }
}

void GUI::setWebServerEnabled(bool enabled) {
    if (webServerEnabled == enabled) return;
    webServerEnabled = enabled;
    if (enabled) {
        webServer.init("/spiffs");
    } else {
        webServer.stop();
    }
}

void GUI::update() {
    handleTouch();
    
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    
    // Library Mode Logic
    if (currentState == AppState::LIBRARY) {
        // WebServer active for 5 minutes
        if (now - lastActivityTime > 5 * 60 * 1000) {
            if (webServerEnabled) {
                ESP_LOGI(TAG, "Library idle timeout: Stopping WebServer");
                setWebServerEnabled(false);
            }
        } else {
            if (!webServerEnabled) {
                setWebServerEnabled(true);
            }
        }
    }
    // Reader Mode Logic
    else if (currentState == AppState::READER) {
        // Active for 60 seconds
        if (now - lastActivityTime > 60 * 1000) {
             goToSleep();
             // After waking up, update lastActivityTime so we don't loop immediately
             lastActivityTime = (uint32_t)(esp_timer_get_time() / 1000);
        }
    }
    
    if (fontChanged) {
        loadFonts();
        fontChanged = false;
        resetPageInfoCache();
    }
    
    if (needsRedraw) {
        ESP_LOGI(TAG, "Needs redraw, current state: %d", (int)currentState);
        switch(currentState) {
            case AppState::LIBRARY:
                drawLibrary();
                break;
            case AppState::READER:
                drawReader();
                break;
            case AppState::WIFI_CONFIG:
                drawWifiConfig();
                break;
            case AppState::SETTINGS:
                drawSettings();
                break;
        }
        needsRedraw = false;
    }
}

void GUI::drawStatusBar() {
    // Use system font for status bar to ensure it fits
    M5.Display.setFont(&lgfx::v1::fonts::Font2);
    M5.Display.setTextSize(1.6f); // Larger, easier to read

    M5.Display.fillRect(0, 0, M5.Display.width(), STATUS_BAR_HEIGHT, TFT_LIGHTGREY);
    M5.Display.setTextColor(TFT_BLACK, TFT_LIGHTGREY);
    const int centerY = STATUS_BAR_HEIGHT / 2;
    
    // Time
    M5.Display.setTextDatum(textdatum_t::middle_left);
    auto dt = M5.Rtc.getDateTime();
    char buf[32];
    snprintf(buf, sizeof(buf), "%02d:%02d", dt.time.hours, dt.time.minutes);
    M5.Display.drawString(buf, 10, centerY);
    
    // Battery (Right aligned)
    M5.Display.setTextDatum(textdatum_t::middle_right);
    int bat = M5.Power.getBatteryLevel();
    snprintf(buf, sizeof(buf), "%d%%", bat);
    int batteryX = M5.Display.width() - 10;
    int batteryTextWidth = M5.Display.textWidth(buf);
    M5.Display.drawString(buf, batteryX, centerY);
    
    // Wifi (Left of Battery)
    if (wifiConnected) {
        snprintf(buf, sizeof(buf), "WiFi %d dBm", wifiRssi);
    } else {
        snprintf(buf, sizeof(buf), "No WiFi");
    }
    // Give enough space for battery reading
    int wifiX = batteryX - batteryTextWidth - 12;
    M5.Display.drawString(buf, wifiX, centerY);
    
    // Restore font
    if (currentFont == "Default") {
        M5.Display.setFont(&lgfx::v1::fonts::Font2);
    } else {
        if (currentFont == "Hebrew" && !fontDataHebrew.empty()) {
            M5.Display.loadFont(fontDataHebrew.data());
        } else if (!fontData.empty()) {
            M5.Display.loadFont(fontData.data());
        } else {
            M5.Display.setFont(&lgfx::v1::fonts::Font2);
        }
    }
    
    // Reset datum to default for other drawing functions
    M5.Display.setTextDatum(textdatum_t::top_left);
    M5.Display.setTextColor(TFT_BLACK); // Transparent background for the rest of the UI
}

void GUI::drawLibrary() {
    M5.Display.fillScreen(TFT_WHITE);
    drawStatusBar();
    
    M5.Display.setTextColor(TFT_BLACK);
    const float headingSize = 2.4f;
    const float itemSize = 1.6f; // Reduced size
    M5.Display.setTextSize(headingSize);
    M5.Display.setCursor(16, 58);
    M5.Display.println("Library");
    
    auto books = bookIndex.getBooks();
    
    // Paging logic
    int availableHeight = M5.Display.height() - LIBRARY_LIST_START_Y - 60; // 60 for footer
    int itemsPerPage = availableHeight / LIBRARY_LINE_HEIGHT;
    if (itemsPerPage < 1) itemsPerPage = 1;

    int totalPages = (books.size() + itemsPerPage - 1) / itemsPerPage;
    if (libraryPage >= totalPages) libraryPage = totalPages - 1;
    if (libraryPage < 0) libraryPage = 0;
    
    int startIdx = libraryPage * itemsPerPage;
    int endIdx = std::min((int)books.size(), startIdx + itemsPerPage);

    int y = LIBRARY_LIST_START_Y;
    M5.Display.setTextSize(itemSize);
    for (int i = startIdx; i < endIdx; ++i) {
        const auto& book = books[i];
        // Truncate display title if too long
        std::string displayTitle = book.title;
        if (displayTitle.length() > 35) displayTitle = displayTitle.substr(0, 32) + "...";
        
        displayTitle = processTextForDisplay(displayTitle);
        
        // Draw bullet
        M5.Display.setCursor(20, y);
        M5.Display.print("- ");
        
        // Draw title with mixed font support
        int titleX = 20 + M5.Display.textWidth("- ");
        drawStringMixed(displayTitle, titleX, y, nullptr);
        
        y += LIBRARY_LINE_HEIGHT;
    }
    
    // Draw Paging Controls
    if (totalPages > 1) {
        int footerY = M5.Display.height() - 60;
        M5.Display.setTextSize(1.5f);
        
        if (libraryPage > 0) {
            M5.Display.fillRect(20, footerY, 100, 40, TFT_LIGHTGREY);
            M5.Display.setTextColor(TFT_BLACK);
            M5.Display.drawString("Prev", 45, footerY + 10);
        }
        
        if (libraryPage < totalPages - 1) {
            M5.Display.fillRect(M5.Display.width() - 120, footerY, 100, 40, TFT_LIGHTGREY);
            M5.Display.setTextColor(TFT_BLACK);
            M5.Display.drawString("Next", M5.Display.width() - 95, footerY + 10);
        }
        
        char pageStr[32];
        snprintf(pageStr, sizeof(pageStr), "%d / %d", libraryPage + 1, totalPages);
        M5.Display.setTextDatum(textdatum_t::top_center);
        M5.Display.drawString(pageStr, M5.Display.width() / 2, footerY + 10);
        M5.Display.setTextDatum(textdatum_t::top_left);
    }
    
    if (books.empty()) {
        M5.Display.setCursor(20, LIBRARY_LIST_START_Y);
        M5.Display.println("No books found.");
        M5.Display.setCursor(20, LIBRARY_LIST_START_Y + LIBRARY_LINE_HEIGHT);
        M5.Display.println("Upload via WiFi:");
        M5.Display.setCursor(20, LIBRARY_LIST_START_Y + LIBRARY_LINE_HEIGHT * 2);
        if (wifiConnected) {
            M5.Display.printf("http://%s/", wifiManager.getIpAddress().c_str());
        } else {
            M5.Display.println("Connect to AP 'M5Paper_Reader'");
        }
    }
    
    M5.Display.display();
}

// Returns number of characters that fit on the page
size_t GUI::drawPageContent(bool draw) {
    return drawPageContentAt(currentTextOffset, draw, nullptr);
}

size_t GUI::drawPageContentAt(size_t startOffset, bool draw, M5Canvas* target) {
    LovyanGFX* gfx = target ? (LovyanGFX*)target : (LovyanGFX*)&M5.Display;
    ESP_LOGI(TAG, "drawPageContentAt: offset=%zu, draw=%d, target=%p, w=%d, h=%d", startOffset, draw, target, gfx->width(), gfx->height());
    gfx->setTextSize(fontSize);
    
    int x = 20; // Margin
    int y = STATUS_BAR_HEIGHT + 12; // Top margin below status bar
    int rightMargin = 20;
    int bottomMargin = 80; // Leave room for footer/status text
    int width = gfx->width();
    int height = gfx->height();
    int maxWidth = width - rightMargin - x; // Width available for text
    int maxY = height - bottomMargin;
    int lineHeight = gfx->fontHeight() * 1.4; // More breathing room
    
    // Fetch a large chunk
    std::string text = epubLoader.getText(startOffset, 3000);
    ESP_LOGI(TAG, "Fetched text at offset %zu, length: %zu", startOffset, text.length());
    if (text.empty()) {
        ESP_LOGW(TAG, "No text fetched at offset %zu", startOffset);
        return 0;
    }
    
    if (draw) gfx->startWrite();

    std::vector<std::string> currentLine;
    int currentLineWidth = 0;
    int currentY = y;
    size_t i = 0;
    
    int debugWordsLogged = 0;
    auto drawLine = [&](const std::vector<std::string>& line) {
        if (!draw || line.empty()) return;
        
        bool isRTL = isRTLDocument;
        if (!isRTL) {
            for (const auto& w : line) {
                if (isHebrew(w)) { isRTL = true; break; }
            }
        }
        
        int startX = isRTL ? (width - rightMargin) : x;
        int spaceW = gfx->textWidth(" ");
        if (spaceW == 0) spaceW = 5; // Fallback if font has no space width
        
        for (const auto& word : line) {
            std::string displayWord = word;
            if (isHebrew(displayWord)) {
                displayWord = reverseHebrewWord(displayWord);
            }
            
            int w = gfx->textWidth(displayWord.c_str());
            int spaceWLocal = gfx->textWidth(" ");
            if (spaceWLocal == 0) spaceWLocal = 5;
            // Emit debug info for first few words on the page so we can inspect spacing
            if (debugWordsLogged < 6) {
                ESP_LOGI(TAG, "drawLine: word='%s' w=%d space=%d isRTL=%d startX=%d y=%d", displayWord.c_str(), w, spaceWLocal, isRTL, startX, currentY);
                debugWordsLogged++;
            }
            
            if (isRTL) {
                drawStringMixed(displayWord, startX - w, currentY, (M5Canvas*)target);
                startX -= (w + spaceWLocal);
            } else {
                drawStringMixed(displayWord, startX, currentY, (M5Canvas*)target);
                startX += (w + spaceWLocal);
            }
        }
    };
    
    int linesDrawn = 0;
    while (i < text.length()) {
        // Find next word boundary
        size_t nextSpace = text.find_first_of(" \n", i);
        if (nextSpace == std::string::npos) nextSpace = text.length();
        
        bool isNewline = (nextSpace < text.length() && text[nextSpace] == '\n');
        
        std::string word = text.substr(i, nextSpace - i);
        
        // Measure word (using reversed version for accuracy if needed)
        std::string measureWord = word;
        if (isHebrew(measureWord)) measureWord = reverseHebrewWord(measureWord);

        // Make sure we measure with the same font that will be used to draw the word.
        // If this word is Hebrew and we have a Hebrew font loaded, temporarily switch
        // the gfx font to the Hebrew font so textWidth() returns accurate metrics.
        bool swappedFont = false;
        std::string restoreFont = currentFont; // keep track of what should be active
        if (isHebrew(measureWord) && !fontDataHebrew.empty()) {
            ESP_LOGI(TAG, "drawPageContentAt: measuring Hebrew word using Hebrew font");
            gfx->loadFont(fontDataHebrew.data());
            gfx->setTextSize(fontSize);
            swappedFont = true;
        }

        int w = gfx->textWidth(measureWord.c_str());
        int spaceW = gfx->textWidth(" ");
        if (spaceW == 0) spaceW = 5; // Fallback

        // Restore the display font to whatever the GUI currently wants
        if (swappedFont) {
            if (restoreFont == "Hebrew" && !fontDataHebrew.empty()) {
                gfx->loadFont(fontDataHebrew.data());
            } else if (restoreFont != "Default" && !fontData.empty()) {
                gfx->loadFont(fontData.data());
            } else {
                gfx->unloadFont();
            }
            gfx->setTextSize(fontSize);
        }
        
        // Check if word fits
        if (currentLineWidth + w > maxWidth) {
            drawLine(currentLine);
            linesDrawn++;
            currentLine.clear();
            currentLineWidth = 0;
            currentY += lineHeight;
            
            if (currentY + lineHeight > maxY) {
                break; // Page full
            }
        }
        
        currentLine.push_back(word);
        currentLineWidth += w + spaceW;
        
        if (nextSpace < text.length()) {
            i = nextSpace + 1;
        } else {
            i = nextSpace;
        }
        
        if (isNewline) {
            drawLine(currentLine);
            linesDrawn++;
            currentLine.clear();
            currentLineWidth = 0;
            currentY += lineHeight;
            if (currentY + lineHeight > maxY) {
                break; // Page full
            }
        }
    }
    
    // Flush remaining line
    if (!currentLine.empty() && currentY + lineHeight <= maxY) {
        drawLine(currentLine);
        linesDrawn++;
    }
    
    if (draw) {
        ESP_LOGI(TAG, "drawPageContentAt: lines drawn: %d", linesDrawn);
        gfx->endWrite();
    }
    
    return i;
}

GUI::PageInfo GUI::calculatePageInfo() {
    // Current page is tracked via history stack
    PageInfo info{};
    info.current = static_cast<int>(pageHistory.size()) + 1;

    // Ensure we have a recent char count for this font/layout
    if (lastPageChars < PAGEINFO_ESTIMATE_MIN_CHARS) {
        size_t chars = drawPageContentAt(currentTextOffset, false);
        if (chars > 0) {
            lastPageChars = chars;
        }
    }

    size_t chapterSize = epubLoader.getChapterSize();
    if (lastPageChars > 0) {
        lastPageTotal = static_cast<int>((chapterSize + lastPageChars - 1) / lastPageChars);
    } else {
        lastPageTotal = 1;
    }

    if (lastPageTotal < 1) lastPageTotal = 1;
    if (info.current > lastPageTotal) info.current = lastPageTotal;
    info.total = lastPageTotal;
    return info;
}

void GUI::updateNextPrevCanvases() {
    // Update Next
    size_t charsOnCurrent = drawPageContentAt(currentTextOffset, false);
    nextCanvasOffset = currentTextOffset + charsOnCurrent;
    
    if (nextCanvasOffset < epubLoader.getChapterSize() && canvasNext.width() > 0) {
        canvasNext.fillScreen(TFT_WHITE);
        drawPageContentAt(nextCanvasOffset, true, &canvasNext);
        nextCanvasValid = true;
    } else {
        nextCanvasValid = false;
    }
    
    // Update Prev
    if (!pageHistory.empty() && canvasPrev.width() > 0) {
        prevCanvasOffset = pageHistory.back();
        canvasPrev.fillScreen(TFT_WHITE);
        drawPageContentAt(prevCanvasOffset, true, &canvasPrev);
        prevCanvasValid = true;
    } else {
        prevCanvasValid = false;
    }
}

void GUI::drawReader() {
    ESP_LOGI(TAG, "drawReader called, currentTextOffset: %zu", currentTextOffset);
    
    // Ensure previous update is complete
    M5.Display.waitDisplay();
    
    bool drawnFromBuffer = false;
    
    // Only use buffers if they are valid (width > 0)
    if (nextCanvasValid && nextCanvasOffset == currentTextOffset && canvasNext.width() > 0) {
        ESP_LOGI(TAG, "Drawing from next buffer");
        canvasNext.pushSprite(&M5.Display, 0, 0);
        drawnFromBuffer = true;
    } else if (prevCanvasValid && prevCanvasOffset == currentTextOffset && canvasPrev.width() > 0) {
        ESP_LOGI(TAG, "Drawing from prev buffer");
        canvasPrev.pushSprite(&M5.Display, 0, 0);
        drawnFromBuffer = true;
    }
    
    if (!drawnFromBuffer) {
        ESP_LOGI(TAG, "Drawing to current buffer");
        M5.Display.fillScreen(TFT_WHITE);
        drawPageContentAt(currentTextOffset, true, nullptr);
    }
    
    // Draw Status Bar (Fresh)
    drawStatusBar();
    
    size_t charsDrawn = drawPageContentAt(currentTextOffset, false);
    ESP_LOGI(TAG, "Chars drawn: %zu", charsDrawn);
    if (charsDrawn > 0) {
        lastPageChars = charsDrawn;
        size_t chapterSize = epubLoader.getChapterSize();
        lastPageTotal = static_cast<int>((chapterSize + lastPageChars - 1) / lastPageChars);
        if (lastPageTotal < 1) lastPageTotal = 1;
    }
    
    PageInfo pageInfo = calculatePageInfo();

    const int footerY = M5.Display.height() - 50;
    M5.Display.setTextSize(1.0f); // Reduced font size
    M5.Display.setCursor(16, footerY);
    M5.Display.printf("Ch %d - %.1f%%", epubLoader.getCurrentChapterIndex() + 1, 
        (float)currentTextOffset / epubLoader.getChapterSize() * 100.0f);

    char pageBuf[24];
    snprintf(pageBuf, sizeof(pageBuf), "Pg %d/%d", pageInfo.current, pageInfo.total);
    M5.Display.setTextDatum(textdatum_t::top_right);
    M5.Display.drawString(pageBuf, M5.Display.width() - 16, footerY);
    M5.Display.setTextDatum(textdatum_t::top_left);
        
    ESP_LOGI(TAG, "Calling M5.Display.display()");
    M5.Display.display();
    ESP_LOGI(TAG, "drawReader completed");
    
    // Update buffers for next interaction
    updateNextPrevCanvases();
}

void GUI::drawSleepSymbol(const char* symbol) {
    M5.Display.setFont(&lgfx::v1::fonts::Font4); // Large font
    M5.Display.setTextSize(1.0f);
    M5.Display.setTextColor(TFT_BLACK, TFT_WHITE);
    M5.Display.setTextDatum(textdatum_t::top_center);
    // Draw in the middle of status bar or top center
    M5.Display.drawString(symbol, M5.Display.width() / 2, 5);
}

void GUI::goToSleep() {
    // Save progress
    bookIndex.updateProgress(currentBook.id, epubLoader.getCurrentChapterIndex(), currentTextOffset);

    // Flush any pending display operations before sleeping
    M5.Display.waitDisplay();

    // Turn off WiFi to save power while sleeping
    wifi_mode_t mode = WIFI_MODE_NULL;
    bool shouldReconnect = false;
    if (esp_wifi_get_mode(&mode) == ESP_OK) {
        shouldReconnect = (mode == WIFI_MODE_STA || mode == WIFI_MODE_APSTA);
    }
    esp_wifi_stop();

    // Light sleep with touch wakeup handled by M5.Power. Also arm a timer so we can fall through to deep sleep.
    drawSleepSymbol("z");
    M5.Power.lightSleep(LIGHT_SLEEP_TO_DEEP_SLEEP_US, true);

    // If the timer woke us, hand off to deep sleep so the next touch resumes straight into the last page.
    auto wakeReason = esp_sleep_get_wakeup_cause();
    if (wakeReason == ESP_SLEEP_WAKEUP_TIMER) {
        ESP_LOGI(TAG, "Light sleep timer elapsed, entering deep sleep");
        drawSleepSymbol("Zz");
        M5.Power.deepSleep(0, true);
        return;
    }

    // Resume WiFi after wake
    esp_wifi_start();
    if (shouldReconnect) {
        esp_wifi_connect();
    }
}

void GUI::resetPageInfoCache() {
    lastPageChars = 0;
    lastPageTotal = 1;
    pageHistory.clear();
}

void GUI::drawSettings() {
    // Overlay
    M5.Display.fillRect(20, 40, M5.Display.width() - 40, M5.Display.height() - 80, TFT_LIGHTGREY);
    M5.Display.drawRect(20, 40, M5.Display.width() - 40, M5.Display.height() - 80, TFT_BLACK);
    
    M5.Display.setTextSize(2);
    M5.Display.setTextColor(TFT_BLACK, TFT_LIGHTGREY);
    M5.Display.setCursor(40, 60);
    M5.Display.println("Settings");
    
    // Font Size
    M5.Display.setCursor(40, 100);
    M5.Display.printf("Size: %.1f", fontSize);
    
    // Buttons for Size
    M5.Display.fillRect(160, 95, 40, 30, TFT_WHITE);
    M5.Display.drawRect(160, 95, 40, 30, TFT_BLACK);
    M5.Display.drawString("-", 175, 100);
    
    M5.Display.fillRect(210, 95, 40, 30, TFT_WHITE);
    M5.Display.drawRect(210, 95, 40, 30, TFT_BLACK);
    M5.Display.drawString("+", 225, 100);
    
    // Font Family
    M5.Display.setCursor(40, 150);
    M5.Display.printf("Font: %s", currentFont.c_str());
    
    // Button for Font
    M5.Display.fillRect(40, 180, 120, 40, TFT_WHITE);
    M5.Display.drawRect(40, 180, 120, 40, TFT_BLACK);
    M5.Display.drawString("Change Font", 50, 190);
    
    // Close
    M5.Display.fillRect(40, 240, 100, 40, TFT_WHITE);
    M5.Display.drawRect(40, 240, 100, 40, TFT_BLACK);
    M5.Display.drawString("Close", 60, 250);
    
    M5.Display.setTextColor(TFT_BLACK); // Reset
    M5.Display.display();
}

void GUI::drawWifiConfig() {
    M5.Display.fillScreen(TFT_WHITE);
    M5.Display.setCursor(10, 50);
    M5.Display.println("WiFi Config Not Implemented in GUI");
    M5.Display.println("Use Web Interface");
    M5.Display.display();
}

void GUI::handleTouch() {
    if (M5.Touch.getCount() > 0) {
        lastActivityTime = (uint32_t)(esp_timer_get_time() / 1000);
        auto t = M5.Touch.getDetail(0);
        if (t.wasPressed()) {
            if (currentState == AppState::LIBRARY) {
                auto books = bookIndex.getBooks();
                int availableHeight = M5.Display.height() - LIBRARY_LIST_START_Y - 60;
                int itemsPerPage = availableHeight / LIBRARY_LINE_HEIGHT;
                if (itemsPerPage < 1) itemsPerPage = 1;

                int totalPages = (books.size() + itemsPerPage - 1) / itemsPerPage;
                
                // Check paging buttons
                if (totalPages > 1 && t.y > M5.Display.height() - 60) {
                    if (t.x < 150 && libraryPage > 0) {
                        libraryPage--;
                        needsRedraw = true;
                        return;
                    }
                    if (t.x > M5.Display.width() - 150 && libraryPage < totalPages - 1) {
                        libraryPage++;
                        needsRedraw = true;
                        return;
                    }
                }
                
                int startIdx = libraryPage * itemsPerPage;
                int endIdx = std::min((int)books.size(), startIdx + itemsPerPage);
                
                int y = LIBRARY_LIST_START_Y;
                for (int i = startIdx; i < endIdx; ++i) {
                    if (t.y >= y && t.y < y + LIBRARY_LINE_HEIGHT) {
                        ESP_LOGI(TAG, "Touched book at index %d, ID %d", i, books[i].id);
                        openBookById(books[i].id);
                        break;
                    }
                    y += LIBRARY_LINE_HEIGHT;
                }
            } else if (currentState == AppState::READER) {
                // Top tap to exit
                if (t.y < 50) {
                    currentState = AppState::LIBRARY;
                    epubLoader.close();
                    needsRedraw = true;
                    return;
                }
                
                // Bottom 1/5 -> Settings
                if (t.y > M5.Display.height() * 4 / 5) {
                    previousState = currentState;
                    currentState = AppState::SETTINGS;
                    needsRedraw = true;
                    return;
                }
                
                // Left/Right tap for page turn
                bool isRTL = isRTLDocument;
                bool next = false;
                
                if (t.x < M5.Display.width() / 2) {
                    // Left side
                    next = isRTL; // If RTL, left is next. If LTR, left is prev.
                } else {
                    // Right side
                    next = !isRTL; // If RTL, right is prev. If LTR, right is next.
                }

                if (!next) {
                    // Prev Page
                    if (pageHistory.empty()) {
                        // Try prev chapter
                        if (epubLoader.prevChapter()) {
                            currentTextOffset = epubLoader.getChapterSize(); // Go to end? No, usually start.
                            // Actually, usually when going back to prev chapter, we want to go to the END of that chapter.
                            // But for now let's go to start or we need to calculate pages.
                            // Let's go to start for simplicity or 0.
                            currentTextOffset = 0; 
                            pageHistory.clear();
                            resetPageInfoCache();
                            needsRedraw = true;
                        }
                    } else {
                        currentTextOffset = pageHistory.back();
                        pageHistory.pop_back();
                        needsRedraw = true;
                    }
                } else {
                    // Next Page
                    size_t charsOnPage = drawPageContent(false);
                    if (currentTextOffset + charsOnPage >= epubLoader.getChapterSize()) {
                        // Next chapter
                        if (epubLoader.nextChapter()) {
                            currentTextOffset = 0;
                            pageHistory.clear();
                            resetPageInfoCache();
                            needsRedraw = true;
                        }
                    } else {
                        pageHistory.push_back(currentTextOffset);
                        currentTextOffset += charsOnPage;
                        needsRedraw = true;
                    }
                }
            } else if (currentState == AppState::SETTINGS) {
                // Check buttons
                // -
                if (t.x >= 160 && t.x <= 200 && t.y >= 95 && t.y <= 125) {
                    setFontSize(fontSize - 0.1f);
                }
                // +
                else if (t.x >= 210 && t.x <= 250 && t.y >= 95 && t.y <= 125) {
                    setFontSize(fontSize + 0.1f);
                }
                // Font
                else if (t.x >= 40 && t.x <= 160 && t.y >= 180 && t.y <= 220) {
                    // Cycle fonts
                    if (currentFont == "Default") setFont("Hebrew");
                    else if (currentFont == "Hebrew") setFont("Roboto");
                    else setFont("Default");
                }
                // Close
                else if (t.x >= 40 && t.x <= 140 && t.y >= 240 && t.y <= 280) {
                    currentState = previousState;
                    needsRedraw = true;
                }
            }
        }
    }
}

void GUI::jumpTo(float percent) {
    if (currentState != AppState::READER) return;
    
    size_t size = epubLoader.getChapterSize();
    if (size == 0) return;
    
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    
    currentTextOffset = (size_t)((percent / 100.0f) * size);
    
    pageHistory.clear();
    resetPageInfoCache();
    needsRedraw = true;
}

void GUI::jumpToChapter(int chapter) {
    if (currentState != AppState::READER) return;
    
    if (epubLoader.jumpToChapter(chapter)) {
        currentTextOffset = 0;
        pageHistory.clear();
        resetPageInfoCache();
        needsRedraw = true;
    }
}

bool GUI::openBookById(int id) {
    BookEntry book = bookIndex.getBook(id);
    if (book.id == 0) return false;

    if (!epubLoader.load(book.path.c_str())) {
        return false;
    }

    currentBook = book;
    currentState = AppState::READER;

    // Auto-detect language for font
    std::string lang = epubLoader.getLanguage();
    if (lang.find("he") != std::string::npos || lang.find("HE") != std::string::npos) {
        setFont("Hebrew");
    }

    // Restore progress
    currentTextOffset = currentBook.currentOffset;
    resetPageInfoCache();
    if (currentBook.currentChapter > 0) {
        while (epubLoader.getCurrentChapterIndex() < currentBook.currentChapter) {
            if (!epubLoader.nextChapter()) break;
        }
    }
    isRTLDocument = detectRTLDocument(epubLoader, currentTextOffset);

    pageHistory.clear();
    needsRedraw = true;

    // Save as last book
    nvs_handle_t my_handle;
    if (nvs_open("storage", NVS_READWRITE, &my_handle) == ESP_OK) {
        nvs_set_i32(my_handle, "last_book_id", currentBook.id);
        nvs_commit(my_handle);
        nvs_close(my_handle);
    }

    return true;
}

void GUI::refreshLibrary() {
    if (currentState == AppState::LIBRARY) {
        needsRedraw = true;
    }
}

void GUI::saveSettings() {
    nvs_handle_t my_handle;
    if (nvs_open("storage", NVS_READWRITE, &my_handle) == ESP_OK) {
        int32_t sizeInt = (int32_t)(fontSize * 10);
        nvs_set_i32(my_handle, "font_size", sizeInt);
        nvs_set_str(my_handle, "font_name", currentFont.c_str());
        nvs_commit(my_handle);
        nvs_close(my_handle);
    }
}

void GUI::setFontSize(float size) {
    if (size < 0.5f) size = 0.5f;
    if (size > 5.0f) size = 5.0f;
    fontSize = size;
    saveSettings();
    resetPageInfoCache();
    needsRedraw = true;
}

void GUI::setFont(const std::string& fontName) {
    if (currentFont == fontName) return;
    currentFont = fontName;
    saveSettings();
    fontChanged = true;
    needsRedraw = true;
}

void GUI::loadSettings() {
    nvs_handle_t my_handle;
    if (nvs_open("storage", NVS_READONLY, &my_handle) == ESP_OK) {
        int32_t sizeInt = 10;
        if (nvs_get_i32(my_handle, "font_size", &sizeInt) == ESP_OK) {
            fontSize = sizeInt / 10.0f;
        }
        
        size_t required_size;
        if (nvs_get_str(my_handle, "font_name", NULL, &required_size) == ESP_OK) {
            char* fontName = new char[required_size];
            nvs_get_str(my_handle, "font_name", fontName, &required_size);
            currentFont = fontName;
            delete[] fontName;
        }
        nvs_close(my_handle);
    }
}

void GUI::loadFonts() {
    // Clear buffers to ensure we don't hold two fonts at once if possible
    fontData.clear();
    fontDataHebrew.clear();
    M5.Display.unloadFont();
    
    if (currentFont == "Default") {
        return;
    }
    
    if (currentFont == "Hebrew") {
        ensureHebrewFontLoaded();
        if (!fontDataHebrew.empty()) {
            M5.Display.loadFont(fontDataHebrew.data());
        }
        return;
    }
    
    // Load other font (e.g. Roboto)
    std::string path = "/spiffs/fonts/" + currentFont + ".vlw";
    if (currentFont == "Roboto") path = "/spiffs/fonts/Roboto-Regular.vlw";
    
    ESP_LOGI(TAG, "Loading font: %s", path.c_str());
    FILE* f = fopen(path.c_str(), "rb");
    if (f) {
        fseek(f, 0, SEEK_END);
        size_t size = ftell(f);
        fseek(f, 0, SEEK_SET);
        fontData.resize(size);
        fread(fontData.data(), 1, size, f);
        fclose(f);
        M5.Display.loadFont(fontData.data());
    } else {
        ESP_LOGW(TAG, "Failed to load font: %s", path.c_str());
        currentFont = "Default";
    }
}

void GUI::ensureHebrewFontLoaded() {
    if (!fontDataHebrew.empty()) {
        ESP_LOGI(TAG, "Hebrew font already loaded");
        return;
    }
    
    // CRITICAL: Free other font memory before loading Hebrew to avoid OOM
    if (!fontData.empty()) {
        ESP_LOGI(TAG, "Unloading primary font to make room for Hebrew");
        fontData.clear();
        M5.Display.unloadFont();
    }
    
    const char* path = "/spiffs/fonts/Hebrew-Merged.vlw";
    ESP_LOGI(TAG, "Attempting to load Hebrew font from: %s", path);
    FILE* f = fopen(path, "rb");
    if (f) {
        fseek(f, 0, SEEK_END);
        size_t size = ftell(f);
        fseek(f, 0, SEEK_SET);
        
        // Try to allocate in PSRAM first if available
        // We use resize() which uses default allocator, but we can reserve first?
        // std::vector doesn't support custom allocators easily without changing type.
        // But we can try to force heap caps if we used a custom allocator.
        // For now, let's just rely on system malloc. If PSRAM is configured, large blocks go there.
        // But if not, we might fail.
        
        try {
            fontDataHebrew.resize(size);
        } catch (const std::bad_alloc& e) {
            ESP_LOGE(TAG, "Failed to allocate memory for Hebrew font (%u bytes)", (unsigned int)size);
            fclose(f);
            return;
        }
        
        size_t read = fread(fontDataHebrew.data(), 1, size, f);
        fclose(f);
        ESP_LOGI(TAG, "Hebrew font loaded: %u bytes read (expected %u)", (unsigned int)read, (unsigned int)size);
        
        // Verify the font data starts with a valid header
        if (read >= 24) {
            uint32_t glyph_count = (fontDataHebrew[0] << 24) | (fontDataHebrew[1] << 16) | 
                                   (fontDataHebrew[2] << 8) | fontDataHebrew[3];
            ESP_LOGI(TAG, "Hebrew font header: glyph_count=%u", glyph_count);
        }
    } else {
        ESP_LOGE(TAG, "Failed to open Hebrew font file: %s (errno=%d)", path, errno);
    }
}

void GUI::drawStringMixed(const std::string& text, int x, int y, M5Canvas* target) {
    LovyanGFX* gfx = target ? (LovyanGFX*)target : (LovyanGFX*)&M5.Display;
    // ESP_LOGI(TAG, "drawStringMixed: '%s' at %d,%d", text.c_str(), x, y); // Too verbose for every word

    // Simplified: if text contains Hebrew, use Hebrew font for the whole string
    // This avoids complex font-switching mid-string which can cause rendering issues
    
    if (isHebrew(text)) {
        // Load Hebrew font
        ensureHebrewFontLoaded();
        if (!fontDataHebrew.empty()) {
            // Use the Hebrew font directly and draw at the current text size
            ESP_LOGI(TAG, "drawStringMixed: drawing Hebrew word at %d,%d (len=%zu)", x, y, text.length());
            gfx->loadFont(fontDataHebrew.data());
            gfx->setTextSize(fontSize);
            gfx->drawString(text.c_str(), x, y);
        } else {
            ESP_LOGW(TAG, "Hebrew font not loaded, using default");
            ESP_LOGW(TAG, "drawStringMixed: Hebrew font not available, falling back to default for '%s'", text.c_str());
            gfx->unloadFont();
            gfx->drawString(text.c_str(), x, y);
        }
        
        // Restore previous font (keep GUI's selected font active)
        if (currentFont == "Hebrew" && !fontDataHebrew.empty()) {
            gfx->loadFont(fontDataHebrew.data());
        } else if (currentFont != "Default" && !fontData.empty()) {
            gfx->loadFont(fontData.data());
        } else {
            gfx->unloadFont();
        }
    } else {
        // Non-Hebrew text - use current font
        gfx->drawString(text.c_str(), x, y);
    }
}


