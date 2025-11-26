#include "gui.h"
#include "epub_loader.h"
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

static const char *TAG = "GUI";

EpubLoader epubLoader;
extern BookIndex bookIndex;
bool wifiConnected = false;
int wifiRssi = 0;
extern WifiManager wifiManager;

static constexpr int STATUS_BAR_HEIGHT = 44;
static constexpr int LIBRARY_LIST_START_Y = 120;
static constexpr int LIBRARY_LINE_HEIGHT = 48;
static constexpr int PAGEINFO_ESTIMATE_MIN_CHARS = 50;

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

static bool isHebrewString(const std::string& text) {
    for (size_t i = 0; i < text.length(); ++i) {
        unsigned char c = (unsigned char)text[i];
        if (c >= 0xD6 && c <= 0xD7) {
            return true;
        }
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

void GUI::update() {
    handleTouch();
    
    if (fontChanged) {
        loadFonts();
        fontChanged = false;
        resetPageInfoCache();
    }
    
    if (needsRedraw) {
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
        if (!fontData.empty()) {
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
    const float itemSize = 2.0f;
    M5.Display.setTextSize(headingSize);
    M5.Display.setCursor(16, 58);
    M5.Display.println("Library");
    
    auto books = bookIndex.getBooks();
    
    // Check if we need to switch to Hebrew font for the library list
    bool needHebrew = false;
    for (const auto& book : books) {
        if (isHebrewString(book.title)) {
            needHebrew = true;
            break;
        }
    }
    
    if (needHebrew && currentFont != "Hebrew") {
        // We need to switch font, but we can't do it inside draw loop easily without recursion issues
        // if we are not careful.
        // However, setFont sets fontChanged=true and needsRedraw=true.
        // If we return here, the main loop will pick it up.
        setFont("Hebrew");
        return;
    }

    int y = LIBRARY_LIST_START_Y;
    M5.Display.setTextSize(itemSize);
    for (const auto& book : books) {
        M5.Display.setCursor(20, y);
        // Truncate display name if too long
        std::string displayTitle = book.title;
        if (displayTitle.length() > 20) displayTitle = displayTitle.substr(0, 17) + "...";
        
        displayTitle = processTextForDisplay(displayTitle);
        
        M5.Display.printf("- %s", displayTitle.c_str());
        y += LIBRARY_LINE_HEIGHT;
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
    return drawPageContentAt(currentTextOffset, draw);
}

size_t GUI::drawPageContentAt(size_t startOffset, bool draw) {
    M5.Display.setTextSize(fontSize);
    
    int x = 20; // Margin
    int y = STATUS_BAR_HEIGHT + 12; // Top margin below status bar
    int rightMargin = 20;
    int bottomMargin = 80; // Leave room for footer/status text
    int width = M5.Display.width();
    int height = M5.Display.height();
    int maxWidth = width - rightMargin; // Absolute X limit
    int maxY = height - bottomMargin;
    int lineHeight = M5.Display.fontHeight() * 1.4; // More breathing room
    
    // Fetch a large chunk
    std::string text = epubLoader.getText(startOffset, 3000);
    if (text.empty()) return 0;
    
    // Process text for display (e.g., Hebrew reversal)
    text = processTextForDisplay(text);
    
    if (draw) M5.Display.startWrite();

    size_t i = 0;
    int currentX = x;
    int currentY = y;
    
    while (i < text.length()) {
        // Find next word boundary
        size_t nextSpace = text.find_first_of(" \n", i);
        if (nextSpace == std::string::npos) nextSpace = text.length();
        
        bool isNewline = (nextSpace < text.length() && text[nextSpace] == '\n');
        
        std::string word = text.substr(i, nextSpace - i);
        int w = M5.Display.textWidth(word.c_str());
        
        // Check if word fits
        if (currentX + w > maxWidth) {
            currentY += lineHeight;
            currentX = x;
        }
        
        // Check if line fits vertically
        if (currentY + lineHeight > maxY) {
            break; // Page full
        }
        
        if (draw) {
            M5.Display.drawString(word.c_str(), currentX, currentY);
        }
        
        currentX += w;
        
        if (isNewline) {
            currentY += lineHeight; // New paragraph
            currentX = x;
            i = nextSpace + 1;
        } else {
            // Add space
            int spaceW = M5.Display.textWidth(" ");
            currentX += spaceW;
            i = nextSpace + 1;
        }
    }
    
    if (draw) M5.Display.endWrite();
    
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

void GUI::drawReader() {
    M5.Display.fillScreen(TFT_WHITE);
    drawStatusBar();
    
    M5.Display.setTextColor(TFT_BLACK); // Clear any background color from status bar
    size_t charsDrawn = drawPageContent(true);
    if (charsDrawn > 0) {
        lastPageChars = charsDrawn;
        size_t chapterSize = epubLoader.getChapterSize();
        lastPageTotal = static_cast<int>((chapterSize + lastPageChars - 1) / lastPageChars);
        if (lastPageTotal < 1) lastPageTotal = 1;
    }
    
    PageInfo pageInfo = calculatePageInfo();

    const int footerY = M5.Display.height() - 50;
    M5.Display.setTextSize(1.4f);
    M5.Display.setCursor(16, footerY);
    M5.Display.printf("Ch %d - %.1f%%", epubLoader.getCurrentChapterIndex() + 1, 
        (float)currentTextOffset / epubLoader.getChapterSize() * 100.0f);

    char pageBuf[24];
    snprintf(pageBuf, sizeof(pageBuf), "Pg %d/%d", pageInfo.current, pageInfo.total);
    M5.Display.setTextDatum(textdatum_t::top_right);
    M5.Display.drawString(pageBuf, M5.Display.width() - 16, footerY);
    M5.Display.setTextDatum(textdatum_t::top_left);
        
    M5.Display.display();
    
    goToSleep();
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

    // Light sleep with touch wakeup handled by M5.Power
    M5.Power.lightSleep(0, true);

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
        auto t = M5.Touch.getDetail(0);
        if (t.wasPressed()) {
            if (currentState == AppState::LIBRARY) {
                auto books = bookIndex.getBooks();
                int y = LIBRARY_LIST_START_Y;
                for (const auto& book : books) {
                    if (t.y >= y && t.y < y + LIBRARY_LINE_HEIGHT) {
                        currentBook = book;
                        if (epubLoader.load(currentBook.path.c_str())) {
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
                            
                            pageHistory.clear();
                            needsRedraw = true;
                            
                            // Save as last book
                            nvs_handle_t my_handle;
                            if (nvs_open("storage", NVS_READWRITE, &my_handle) == ESP_OK) {
                                nvs_set_i32(my_handle, "last_book_id", currentBook.id);
                                nvs_commit(my_handle);
                                nvs_close(my_handle);
                            }
                        }
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
                if (t.x < M5.Display.width() / 2) {
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
    fontData.clear();
    M5.Display.unloadFont();
    
    if (currentFont == "Default") {
        // Use built-in
        return;
    }
    
    std::string path;
    if (currentFont == "Hebrew") {
        path = "/spiffs/fonts/hebrew.vlw"; 
    } else if (currentFont == "Roboto") {
        path = "/spiffs/fonts/roboto.vlw"; 
    } else {
        path = "/spiffs/fonts/" + currentFont + ".vlw";
    }
    
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
        if (currentFont != "Default") {
             currentFont = "Default";
        }
    }
}


