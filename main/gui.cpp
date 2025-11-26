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

static const char *TAG = "GUI";

EpubLoader epubLoader;
extern BookIndex bookIndex;
bool wifiConnected = false;
int wifiRssi = 0;
extern WifiManager wifiManager;

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
static std::string processTextForDisplay(const std::string& text) {
    // Check if string contains Hebrew (0xD6 0x90 to 0xD7 0xAA in UTF-8)
    bool hasHebrew = false;
    for (size_t i = 0; i < text.length(); ++i) {
        unsigned char c = (unsigned char)text[i];
        if (c >= 0xD6 && c <= 0xD7) {
            hasHebrew = true;
            break;
        }
    }
    
    if (!hasHebrew) return text;
    
    // If Hebrew, reverse the string?
    // This is a very naive implementation.
    // A proper implementation would use a BiDi algorithm (like FriBidi).
    // For now, we just reverse the whole string if it contains Hebrew.
    // Note: This reverses UTF-8 characters correctly.
    
    std::string reversed;
    reversed.reserve(text.length());
    
    for (size_t i = 0; i < text.length(); ) {
        unsigned char c = (unsigned char)text[i];
        size_t charLen = 1;
        if ((c & 0x80) == 0) charLen = 1;
        else if ((c & 0xE0) == 0xC0) charLen = 2;
        else if ((c & 0xF0) == 0xE0) charLen = 3;
        else if ((c & 0xF8) == 0xF0) charLen = 4;
        
        if (i + charLen > text.length()) break; // Error
        
        reversed.insert(0, text.substr(i, charLen));
        i += charLen;
    }
    
    return reversed;
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

    M5.Display.setTextColor(TFT_BLACK, TFT_WHITE);

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
    M5.Display.fillRect(0, 0, M5.Display.width(), 40, TFT_LIGHTGREY);
    M5.Display.setCursor(10, 10);
    M5.Display.setTextSize(2);
    
    // Time
    auto dt = M5.Rtc.getDateTime();
    M5.Display.printf("%02d:%02d", dt.time.hours, dt.time.minutes);
    
    // Wifi
    M5.Display.setCursor(M5.Display.width() - 200, 10);
    if (wifiConnected) {
        M5.Display.printf("WiFi %d dBm", wifiRssi);
    } else {
        M5.Display.print("No WiFi");
    }
    
    // Battery
    int bat = M5.Power.getBatteryLevel();
    M5.Display.setCursor(M5.Display.width() - 80, 10);
    M5.Display.printf("%d%%", bat);
}

void GUI::drawLibrary() {
    M5.Display.fillScreen(TFT_WHITE);
    drawStatusBar();
    
    M5.Display.setCursor(10, 60);
    M5.Display.setTextSize(3);
    M5.Display.println("Library:");
    M5.Display.setTextSize(2);
    
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

    int y = 100;
    for (const auto& book : books) {
        M5.Display.setCursor(20, y);
        // Truncate display name if too long
        std::string displayTitle = book.title;
        if (displayTitle.length() > 20) displayTitle = displayTitle.substr(0, 17) + "...";
        
        displayTitle = processTextForDisplay(displayTitle);
        
        M5.Display.printf("- %s", displayTitle.c_str());
        y += 40;
    }
    
    if (books.empty()) {
        M5.Display.setCursor(20, 100);
        M5.Display.println("No books found.");
        M5.Display.setCursor(20, 140);
        M5.Display.println("Upload via WiFi:");
        M5.Display.setCursor(20, 180);
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
    M5.Display.setTextSize(fontSize);
    
    int x = 15; // Margin
    int y = 50; // Top margin
    int rightMargin = 15;
    int bottomMargin = 40;
    int width = M5.Display.width();
    int height = M5.Display.height();
    int maxWidth = width - rightMargin; // Absolute X limit
    int maxY = height - bottomMargin;
    int lineHeight = M5.Display.fontHeight() * 1.4; // More breathing room
    
    // Fetch a large chunk
    std::string text = epubLoader.getText(currentTextOffset, 3000);
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

void GUI::drawReader() {
    M5.Display.fillScreen(TFT_WHITE);
    drawStatusBar();
    
    drawPageContent(true);
    
    M5.Display.setTextSize(1);
    M5.Display.setCursor(10, M5.Display.height() - 25);
    M5.Display.printf("Ch %d - %.1f%%", epubLoader.getCurrentChapterIndex() + 1, 
        (float)currentTextOffset / epubLoader.getChapterSize() * 100.0f);
        
    M5.Display.display();
    
    goToSleep();
}

void GUI::goToSleep() {
    // Save progress
    bookIndex.updateProgress(currentBook.id, epubLoader.getCurrentChapterIndex(), currentTextOffset);
    
    // Enable touch wakeup (EXT0 on GPIO 36 for M5Paper)
    // Note: M5Paper touch INT is GPIO 36.
    esp_sleep_enable_ext0_wakeup(GPIO_NUM_36, 0); // 0 = Low level trigger
    
    // Use Light Sleep instead of Deep Sleep
    // This avoids rebooting, which fixes the display driver timeout issues
    // and provides a faster resume experience.
    esp_light_sleep_start();
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
    
    M5.Display.setTextColor(TFT_BLACK, TFT_WHITE); // Reset
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
                int y = 100;
                for (const auto& book : books) {
                    if (t.y >= y && t.y < y + 40) {
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
                    y += 40;
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

                // Left/Right tap
                if (t.x > M5.Display.width() / 2) {
                    // Next Page
                    size_t chars = drawPageContent(false);
                    if (chars > 0) {
                        pageHistory.push_back(currentTextOffset);
                        currentTextOffset += chars;
                        needsRedraw = true;
                    } else {
                        // Try next chapter
                        if (epubLoader.nextChapter()) {
                            pageHistory.push_back(currentTextOffset); // Save end of prev chapter? No, start of new.
                            // Actually history logic across chapters is tricky.
                            // Simplified: Clear history on chapter change or push a marker?
                            // For now, just reset offset.
                            currentTextOffset = 0;
                            needsRedraw = true;
                        }
                    }
                } else {
                    // Prev Page
                    if (!pageHistory.empty()) {
                        currentTextOffset = pageHistory.back();
                        pageHistory.pop_back();
                        needsRedraw = true;
                    } else {
                        // Prev Chapter
                        if (epubLoader.prevChapter()) {
                            currentTextOffset = 0; // Should go to end, but that requires calculating all pages.
                            // Simplified: Go to start of prev chapter.
                            needsRedraw = true;
                        }
                    }
                }
            } else if (currentState == AppState::SETTINGS) {
                // Handle Settings Clicks
                // Size -
                if (t.y >= 95 && t.y <= 125 && t.x >= 160 && t.x <= 200) {
                    setFontSize(fontSize - 0.1f);
                }
                // Size +
                if (t.y >= 95 && t.y <= 125 && t.x >= 210 && t.x <= 250) {
                    setFontSize(fontSize + 0.1f);
                }
                // Change Font
                if (t.y >= 180 && t.y <= 220 && t.x >= 40 && t.x <= 160) {
                    std::string nextFont;
                    if (currentFont == "Default") nextFont = "Hebrew";
                    else if (currentFont == "Hebrew") nextFont = "Roboto";
                    else nextFont = "Default";
                    setFont(nextFont);
                }
                // Close
                if (t.y >= 240 && t.y <= 280 && t.x >= 40 && t.x <= 140) {
                    currentState = previousState;
                    needsRedraw = true;
                }
            }
        }
    }
}

void GUI::setFontSize(float size) {
    if (size < 0.5f) size = 0.5f;
    if (size > 5.0f) size = 5.0f;
    if (fontSize != size) {
        fontSize = size;
        saveSettings();
        needsRedraw = true;
    }
}

void GUI::setFont(const std::string& fontName) {
    if (currentFont != fontName) {
        currentFont = fontName;
        fontChanged = true;
        loadFonts();
        saveSettings();
        needsRedraw = true;
    }
}

void GUI::loadSettings() {
    nvs_handle_t my_handle;
    esp_err_t err = nvs_open("storage", NVS_READWRITE, &my_handle);
    if (err == ESP_OK) {
        size_t size = 0;
        // Font Size
        uint32_t sizeInt = 20; // Default 2.0 * 10
        nvs_get_u32(my_handle, "font_size", &sizeInt);
        fontSize = sizeInt / 10.0f;
        
        // Font Name
        if (nvs_get_str(my_handle, "font_name", NULL, &size) == ESP_OK) {
            char* name = new char[size];
            nvs_get_str(my_handle, "font_name", name, &size);
            currentFont = name;
            delete[] name;
        }
        nvs_close(my_handle);
    }
}

void GUI::saveSettings() {
    nvs_handle_t my_handle;
    esp_err_t err = nvs_open("storage", NVS_READWRITE, &my_handle);
    if (err == ESP_OK) {
        nvs_set_u32(my_handle, "font_size", (uint32_t)(fontSize * 10));
        nvs_set_str(my_handle, "font_name", currentFont.c_str());
        nvs_commit(my_handle);
        nvs_close(my_handle);
    }
}

#include <sys/stat.h>

static bool fileExists(const char* path) {
    struct stat st;
    return stat(path, &st) == 0;
}

static bool hasExtension(const std::string& path, const std::string& ext) {
    if (path.length() < ext.length()) return false;
    auto start = path.length() - ext.length();
    for (size_t i = 0; i < ext.length(); ++i) {
        char a = path[start + i];
        char b = ext[i];
        if (a >= 'A' && a <= 'Z') a = a - 'A' + 'a';
        if (b >= 'A' && b <= 'Z') b = b - 'A' + 'a';
        if (a != b) return false;
    }
    return true;
}

void GUI::loadFonts() {
    M5.Display.unloadFont();
    
    if (currentFont == "Default") {
        M5.Display.setFont(&lgfx::v1::fonts::Font2);
    } else {
        std::string fontPath;
        if (currentFont == "Hebrew") {
            fontPath = "/spiffs/fonts/NotoSansHebrew-Regular.vlw";
        } else if (currentFont == "Roboto") {
            fontPath = "/spiffs/fonts/Roboto-Regular.vlw";
        } else {
            // Try to construct path if it's just a name
            if (currentFont.find("/") == std::string::npos) {
                 fontPath = "/spiffs/fonts/" + currentFont + ".vlw";
            } else {
                 fontPath = currentFont;
            }
        }

        if (!M5.Display.loadFont(fontPath.c_str())) {
             ESP_LOGE(TAG, "Failed to load font %s, falling back to default", fontPath.c_str());
             currentFont = "Default";
             M5.Display.setFont(&lgfx::v1::fonts::Font2);
        }
    }
}

void GUI::refreshLibrary() {
    if (currentState == AppState::LIBRARY) {
        needsRedraw = true;
    }
}


