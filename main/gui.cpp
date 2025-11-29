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
#include "driver/rtc_io.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include <dirent.h>
#include <errno.h>
#include <string.h>

static const char *TAG = "GUI";
bool GUI::canJump() const {
    return currentState == AppState::READER;
}

EpubLoader epubLoader;
extern BookIndex bookIndex;
bool wifiConnected = false;
int wifiRssi = 0;
extern WifiManager wifiManager;
extern WebServer webServer;

static constexpr int STATUS_BAR_HEIGHT = 44;
static constexpr int LIBRARY_LIST_START_Y = 120;
static constexpr int LIBRARY_LINE_HEIGHT = 48;


struct SettingsLayout
{
    int panelTop;
    int panelHeight;
    int panelWidth;
    int padding;
    int rowHeight;
    int titleY;
    int row1Y;
    int row2Y;
    int row3Y;
    int row4Y;
    int closeY;
    int buttonGap;
    int fontButtonW;
    int fontButtonH;
    int fontMinusX;
    int fontPlusX;
    int changeButtonX;
    int changeButtonW;
    int toggleButtonX;
    int toggleButtonW;
    int closeButtonW;
    int closeButtonH;
};

static SettingsLayout computeSettingsLayout()
{
    SettingsLayout l{};
    l.panelWidth = M5.Display.width();
    l.panelHeight = M5.Display.height() / 2;
    l.panelTop = M5.Display.height() - l.panelHeight;
    l.padding = 30;
    l.rowHeight = 60;
    l.titleY = l.panelTop + 20;
    l.row1Y = l.panelTop + 60;
    l.row2Y = l.row1Y + l.rowHeight;
    l.row3Y = l.row2Y + l.rowHeight;
    l.row4Y = l.row3Y + l.rowHeight;
    l.closeY = l.panelTop + l.panelHeight - 70;
    l.buttonGap = 20;
    l.fontButtonW = 80;
    l.fontButtonH = 45;
    l.fontPlusX = l.panelWidth - l.padding - l.fontButtonW;
    l.fontMinusX = l.fontPlusX - l.buttonGap - l.fontButtonW;
    l.changeButtonW = 180;
    l.changeButtonX = l.panelWidth - l.padding - l.changeButtonW;
    l.toggleButtonW = 180;
    l.toggleButtonX = l.panelWidth - l.padding - l.toggleButtonW;
    l.closeButtonW = 200;
    l.closeButtonH = 50;
    return l;
}

int GUI::getCurrentChapterIndex() const {
    return epubLoader.getCurrentChapterIndex();
}

size_t GUI::getCurrentChapterSize() const {
    if (currentState != AppState::READER) return 0;
    return epubLoader.getChapterSize();
}

size_t GUI::getCurrentOffset() const {
    if (currentState != AppState::READER) return 0;
    return currentTextOffset;
}
static constexpr int PAGEINFO_ESTIMATE_MIN_CHARS = 50;
static constexpr uint64_t LIGHT_SLEEP_TO_DEEP_SLEEP_US = 2ULL * 60ULL * 1000000ULL; // 2 minutes

// Helper to split string by delimiters but keep delimiters if needed
// For word wrapping, we split by space.
std::vector<std::string> splitWords(const std::string &text)
{
    std::vector<std::string> words;
    std::string currentWord;
    for (char c : text)
    {
        if (c == ' ' || c == '\n')
        {
            if (!currentWord.empty())
            {
                words.push_back(currentWord);
                currentWord.clear();
            }
            // Add delimiter as separate token if it's a newline,
            // or just handle space implicitly?
            // Let's handle newline explicitly. Space is implicit separator.
            if (c == '\n')
            {
                words.push_back("\n");
            }
        }
        else
        {
            currentWord += c;
        }
    }
    if (!currentWord.empty())
        words.push_back(currentWord);
    return words;
}

// Simple Hebrew detection and reversal for visual display
static bool isHebrew(const std::string &word)
{
    for (size_t i = 0; i < word.length(); ++i)
    {
        unsigned char c = (unsigned char)word[i];
        if (c >= 0xD6 && c <= 0xD7)
            return true;
    }
    return false;
}

static std::string reverseHebrewWord(const std::string &text)
{
    std::string reversed;
    reversed.reserve(text.length());
    for (size_t i = 0; i < text.length();)
    {
        unsigned char c = (unsigned char)text[i];
        size_t charLen = 1;
        if ((c & 0x80) == 0)
            charLen = 1;
        else if ((c & 0xE0) == 0xC0)
            charLen = 2;
        else if ((c & 0xF0) == 0xE0)
            charLen = 3;
        else if ((c & 0xF8) == 0xF0)
            charLen = 4;

        if (i + charLen > text.length())
            break;
        reversed.insert(0, text.substr(i, charLen));
        i += charLen;
    }
    return reversed;
}

static std::string processTextForDisplay(const std::string &text)
{
    if (!isHebrew(text))
        return text;

    // Split into words (space separated)
    // We want to reverse the order of words for RTL.
    // And for each word, if it is Hebrew, reverse the letters.
    // If it is English/Number, keep letters as is.

    std::vector<std::string> words;
    std::string current;
    for (char c : text)
    {
        if (c == ' ' || c == '\n')
        {
            if (!current.empty())
                words.push_back(current);
            std::string delim(1, c);
            words.push_back(delim);
            current.clear();
        }
        else
        {
            current += c;
        }
    }
    if (!current.empty())
        words.push_back(current);

    // Reverse word order
    std::reverse(words.begin(), words.end());

    std::string result;
    for (const auto &word : words)
    {
        if (isHebrew(word))
        {
            // Replace HTML entities before reversing
            std::string processedWord = word;
            size_t pos = 0;
            while ((pos = processedWord.find("&quot;", pos)) != std::string::npos) {
                processedWord.replace(pos, 6, "\"");
                pos += 1;
            }
            pos = 0;
            while ((pos = processedWord.find("&amp;", pos)) != std::string::npos) {
                processedWord.replace(pos, 5, "&");
                pos += 1;
            }
            pos = 0;
            while ((pos = processedWord.find("&lt;", pos)) != std::string::npos) {
                processedWord.replace(pos, 4, "<");
                pos += 1;
            }
            pos = 0;
            while ((pos = processedWord.find("&gt;", pos)) != std::string::npos) {
                processedWord.replace(pos, 4, ">");
                pos += 1;
            }
            pos = 0;
            while ((pos = processedWord.find("&apos;", pos)) != std::string::npos) {
                processedWord.replace(pos, 6, "'");
                pos += 1;
            }
            result += reverseHebrewWord(processedWord);
        }
        else
        {
            result += word;
        }
    }
    return result;
}

static bool detectRTLDocument(EpubLoader &loader, size_t sampleOffset)
{
    std::string lang = loader.getLanguage();
    if (lang.find("he") != std::string::npos || lang.find("HE") != std::string::npos)
    {
        return true;
    }

    // Check first 5 chapters to decide if the book is Hebrew
    int originalChapter = loader.getCurrentChapterIndex();
    int chaptersToCheck = std::min(5, loader.getTotalChapters());
    bool foundHebrew = false;

    for (int i = 0; i < chaptersToCheck; i++)
    {
        if (loader.jumpToChapter(i))
        {
            // Check first 1000 chars of the chapter
            std::string sample = loader.getText(0, 1000);
            for (char c : sample)
            {
                std::string s(1, c);
                if (isHebrew(s))
                {
                    foundHebrew = true;
                    break;
                }
            }
        }
        if (foundHebrew)
            break;
    }

    // Restore original chapter
    if (loader.getCurrentChapterIndex() != originalChapter)
    {
        loader.jumpToChapter(originalChapter);
    }

    return foundHebrew;
}

static void enterDeepSleepWithTouchWake()
{
    // Only needed on M5Paper: touch INT is GPIO36 (RTC pin) and needs the rail + pull-ups alive.
    if (M5.getBoard() != m5::board_t::board_M5Paper)
    {
        esp_deep_sleep_start();
        return;
    }

    const gpio_num_t TOUCH_INT_PIN = GPIO_NUM_36;
    const gpio_num_t MAIN_PWR_PIN = GPIO_NUM_2;

    ESP_LOGI(TAG, "Preparing for deep sleep...");

    // 1. Clear any pending touch interrupt:
    // Ensure the pin is configured as input
    gpio_reset_pin(TOUCH_INT_PIN);
    gpio_set_direction(TOUCH_INT_PIN, GPIO_MODE_INPUT);
    
    // Wait for line to go HIGH (inactive) - it is active LOW
    int retries = 0;
    while (gpio_get_level(TOUCH_INT_PIN) == 0 && retries < 50) {
        M5.update(); 
        vTaskDelay(50 / portTICK_PERIOD_MS);
        retries++;
    }
    if (gpio_get_level(TOUCH_INT_PIN) == 0) {
        ESP_LOGW(TAG, "Touch interrupt pin still LOW after flushing");
    }

    // 2. Configure wakeup on touch interrupt (active LOW)
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
    esp_sleep_enable_ext0_wakeup(TOUCH_INT_PIN, 0); // 0 = LOW

    // 3. Keep main power rail ON during deep sleep
    // This is critical for M5Paper touch controller to stay alive
    gpio_reset_pin(MAIN_PWR_PIN);
    gpio_set_direction(MAIN_PWR_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level(MAIN_PWR_PIN, 1);
    gpio_hold_en(MAIN_PWR_PIN);
    gpio_deep_sleep_hold_en();

    // 4. Put display to sleep cleanly
    M5.Display.sleep();
    M5.Display.waitDisplay();

    // 5. Enter deep sleep
    ESP_LOGI(TAG, "Entering deep sleep now");
    esp_deep_sleep_start();
}

void GUI::init(bool isWakeFromSleep)
{
    justWokeUp = isWakeFromSleep;

    // Initialize NVS
    // Already done in main, but safe to call again or skip
    if (!isWakeFromSleep) {
        esp_err_t ret = nvs_flash_init();
        if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
        {
            ESP_ERROR_CHECK(nvs_flash_erase());
            ret = nvs_flash_init();
        }
        ESP_ERROR_CHECK(ret);
    }

    
    loadSettings();
    loadFonts();    
    bookIndex.init(isWakeFromSleep);
    resetPageInfoCache();

    M5.Display.setTextColor(TFT_BLACK);

    // Skip canvas sprite creation - they consistently fail due to memory constraints
    // and aren't necessary for the restore path which renders directly to display.
    // For page turns, we can use partial refresh or accept a brief redraw delay.
    // This also eliminates the error messages on startup.
    
    // Mark canvases as invalid so rendering path knows to draw directly
    nextCanvasValid = false;
    prevCanvasValid = false;

    lastActivityTime = (uint32_t)(esp_timer_get_time() / 1000);

    // Always check for state restoration, regardless of wake reason.
    // This handles cases where the device was reset or power-cycled manually.
    {
        // Try to load last book
        nvs_handle_t my_handle;
        int32_t lastId = -1;
        int32_t lastState = (int)AppState::LIBRARY;
        if (nvs_open("storage", NVS_READONLY, &my_handle) == ESP_OK)
        {
            nvs_get_i32(my_handle, "last_book_id", &lastId);
            nvs_get_i32(my_handle, "last_state", &lastState);
            nvs_close(my_handle);
        }

        // Only restore if we were in READER state
        if (lastId != -1 && lastState == (int)AppState::READER)
        {
            currentBook = bookIndex.getBook(lastId);
            // Pass the saved chapter index to load() to skip heuristics and jump directly
            if (currentBook.id != 0 && epubLoader.load(currentBook.path.c_str(), currentBook.currentChapter))
            {
                currentState = AppState::READER;

                // Auto-detect language
                std::string lang = epubLoader.getLanguage();
                bool isHebrew = (lang.find("he") != std::string::npos || lang.find("HE") != std::string::npos);
                if (isHebrew)
                {
                    setFont("Hebrew");
                }
                // Restore progress
                currentTextOffset = currentBook.currentOffset;
                resetPageInfoCache();
                
                // No need to loop nextChapter() anymore because we loaded the correct chapter directly
                
                // Quick RTL detection based on language only (skip expensive chapter scanning on restore)
                isRTLDocument = isHebrew;

                pageHistory.clear();
                needsRedraw = true; // Force redraw to clear sleep symbol
                return;
            }
        }
    }

    M5.Display.fillScreen(TFT_WHITE);
    needsRedraw = true;
}

void GUI::setWifiStatus(bool connected, int rssi)
{
    if (wifiConnected != connected || (connected && abs(wifiRssi - rssi) > 10))
    {
        wifiConnected = connected;
        wifiRssi = rssi;
        // Only redraw status bar if we are not in a critical animation
        // For POC, just force redraw if in library
        if (currentState == AppState::LIBRARY)
            needsRedraw = true;
    }
}

void GUI::setWebServerEnabled(bool enabled)
{
    if (webServerEnabled == enabled)
        return;
    webServerEnabled = enabled;
    if (enabled)
    {
        webServer.init("/spiffs");
    }
    else
    {
        webServer.stop();
    }
}

void GUI::update()
{
    handleTouch();

    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);

    // Library Mode Logic
    if (currentState == AppState::LIBRARY)
    {
        // WebServer active for 5 minutes
        if (now - lastActivityTime > 5 * 60 * 1000)
        {
            if (webServerEnabled)
            {
                ESP_LOGI(TAG, "Library idle timeout: Stopping WebServer");
                setWebServerEnabled(false);
            }
            // Also go to sleep if idle for 5 minutes
            goToSleep();
            lastActivityTime = (uint32_t)(esp_timer_get_time() / 1000);
        }
        else
        {
            if (!webServerEnabled)
            {
                setWebServerEnabled(true);
            }
        }
    }
    // Reader Mode Logic
    else if (currentState == AppState::READER || currentState == AppState::SETTINGS)
    {
        // Active for 60 seconds
        if (now - lastActivityTime > 60 * 1000)
        {
            goToSleep();
            // After waking up, update lastActivityTime so we don't loop immediately
            lastActivityTime = (uint32_t)(esp_timer_get_time() / 1000);
        }
    }

    if (fontChanged)
    {
        loadFonts();
        fontChanged = false;
        resetPageInfoCache();
    }

    if (needsRedraw)
    {
        ESP_LOGI(TAG, "Needs redraw, current state: %d", (int)currentState);
        switch (currentState)
        {
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
        case AppState::WIFI_SCAN:
            drawWifiScan();
            break;
        case AppState::WIFI_PASSWORD:
            drawWifiPassword();
            break;
        }
        needsRedraw = false;
    }
}

void GUI::drawStatusBar()
{
    // Use system font for status bar to ensure it fits
    M5.Display.setFont(&lgfx::v1::fonts::Font2);
    M5.Display.setTextSize(1.6f); // Larger, easier to read

    // White background, black text, black separator line
    M5.Display.fillRect(0, 0, M5.Display.width(), STATUS_BAR_HEIGHT, TFT_WHITE);
    M5.Display.drawFastHLine(0, STATUS_BAR_HEIGHT - 1, M5.Display.width(), TFT_BLACK);
    M5.Display.setTextColor(TFT_BLACK, TFT_WHITE);
    
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
    if (wifiConnected)
    {
        snprintf(buf, sizeof(buf), "WiFi %d dBm", wifiRssi);
    }
    else
    {
        snprintf(buf, sizeof(buf), "No WiFi");
    }
    // Give enough space for battery reading
    int wifiX = batteryX - batteryTextWidth - 12;
    M5.Display.drawString(buf, wifiX, centerY);

    // Restore font
    if (currentFont == "Default")
    {
        M5.Display.setFont(&lgfx::v1::fonts::Font2);
    }
    else
    {
        if (currentFont == "Hebrew" && !fontDataHebrew.empty())
        {
            M5.Display.loadFont(fontDataHebrew.data());
        }
        else if (!fontData.empty())
        {
            M5.Display.loadFont(fontData.data());
        }
        else
        {
            M5.Display.setFont(&lgfx::v1::fonts::Font2);
        }
    }

    // Reset datum to default for other drawing functions
    M5.Display.setTextDatum(textdatum_t::top_left);
    M5.Display.setTextColor(TFT_BLACK); // Transparent background for the rest of the UI
}

void GUI::drawLibrary()
{
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
    if (itemsPerPage < 1)
        itemsPerPage = 1;

    int totalPages = (books.size() + itemsPerPage - 1) / itemsPerPage;
    if (libraryPage >= totalPages)
        libraryPage = totalPages - 1;
    if (libraryPage < 0)
        libraryPage = 0;

    int startIdx = libraryPage * itemsPerPage;
    int endIdx = std::min((int)books.size(), startIdx + itemsPerPage);

    int y = LIBRARY_LIST_START_Y;
    M5.Display.setTextSize(itemSize);
    for (int i = startIdx; i < endIdx; ++i)
    {
        const auto &book = books[i];
        // Truncate display title if too long
        std::string displayTitle = book.title;
        if (displayTitle.length() > 45)
            displayTitle = displayTitle.substr(0, 42) + "...";

        displayTitle = processTextForDisplay(displayTitle);

        // Draw bullet
        M5.Display.setCursor(20, y);
        M5.Display.print("- ");

        // Draw title with mixed font support
        int titleX = 20 + M5.Display.textWidth("- ");
        drawStringMixed(displayTitle, titleX, y, nullptr, itemSize);

        y += LIBRARY_LINE_HEIGHT;
    }

    // Draw Paging Controls
    if (totalPages > 1)
    {
        int footerY = M5.Display.height() - 60;
        M5.Display.setTextSize(1.5f);

        if (libraryPage > 0)
        {
            M5.Display.fillRect(20, footerY, 100, 40, TFT_LIGHTGREY);
            M5.Display.setTextColor(TFT_BLACK);
            M5.Display.drawString("Prev", 45, footerY + 10);
        }

        if (libraryPage < totalPages - 1)
        {
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

    if (books.empty())
    {
        M5.Display.setCursor(20, LIBRARY_LIST_START_Y);
        M5.Display.println("No books found.");
        M5.Display.setCursor(20, LIBRARY_LIST_START_Y + LIBRARY_LINE_HEIGHT);
        M5.Display.println("Upload via WiFi:");
        M5.Display.setCursor(20, LIBRARY_LIST_START_Y + LIBRARY_LINE_HEIGHT * 2);
        if (wifiConnected)
        {
            M5.Display.printf("http://%s/", wifiManager.getIpAddress().c_str());
        }
        else
        {
            M5.Display.println("Connect to AP 'M5Paper_Reader'");
        }
    }

    M5.Display.display();
}

// Returns number of characters that fit on the page
size_t GUI::drawPageContent(bool draw)
{
    return drawPageContentAt(currentTextOffset, draw, nullptr);
}

size_t GUI::drawPageContentAt(size_t startOffset, bool draw, M5Canvas *target)
{
    LovyanGFX *gfx = target ? (LovyanGFX *)target : (LovyanGFX *)&M5.Display;
    // ESP_LOGI(TAG, "drawPageContentAt: offset=%zu, draw=%d, target=%p, w=%d, h=%d", startOffset, draw, target, gfx->width(), gfx->height());
    
    // Ensure correct base font is loaded on the target
    if (currentFont == "Hebrew" && !fontDataHebrew.empty()) {
        gfx->loadFont(fontDataHebrew.data());
    } else if (currentFont != "Default" && !fontData.empty()) {
        gfx->loadFont(fontData.data());
    } else {
        gfx->unloadFont();
    }
    gfx->setTextSize(fontSize);

    int x = 20;                     // Margin
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
    if (text.empty())
    {
        ESP_LOGW(TAG, "No text fetched at offset %zu", startOffset);
        return 0;
    }

    if (draw)
        gfx->startWrite();

    // Store indices (start, length) instead of strings to save memory
    std::vector<std::pair<size_t, size_t>> currentLine;
    currentLine.reserve(20); // Pre-allocate to avoid reallocations
    
    int currentLineWidth = 0;
    int currentY = y;
    size_t i = 0;

    int debugWordsLogged = 0;
    std::string tempWord; // Reusable buffer for measurements

    auto drawLine = [&](const std::vector<std::pair<size_t, size_t>> &line)
    {
        if (!draw || line.empty())
            return;

        bool isRTL = isRTLDocument;
        if (!isRTL)
        {
            for (const auto &p : line)
            {
                // Check if word is Hebrew
                const char* wordStart = text.c_str() + p.first;
                for(size_t k=0; k<p.second; ++k) {
                    unsigned char c = (unsigned char)wordStart[k];
                    if (c >= 0xD6 && c <= 0xD7) {
                        isRTL = true;
                        break;
                    }
                }
                if (isRTL) break;
            }
        }

        int startX = isRTL ? (width - rightMargin) : x;
        int spaceW = gfx->textWidth(" ");
        if (spaceW == 0)
            spaceW = 5; // Fallback if font has no space width

        for (const auto &p : line)
        {
            std::string displayWord = text.substr(p.first, p.second);
            if (isHebrew(displayWord))
            {
                // Replace HTML entities
                size_t pos = 0;
                while ((pos = displayWord.find("&quot;", pos)) != std::string::npos) {
                    displayWord.replace(pos, 6, "\"");
                    pos += 1;
                }
                pos = 0;
                while ((pos = displayWord.find("&amp;", pos)) != std::string::npos) {
                    displayWord.replace(pos, 5, "&");
                    pos += 1;
                }
                pos = 0;
                while ((pos = displayWord.find("&lt;", pos)) != std::string::npos) {
                    displayWord.replace(pos, 4, "<");
                    pos += 1;
                }
                pos = 0;
                while ((pos = displayWord.find("&gt;", pos)) != std::string::npos) {
                    displayWord.replace(pos, 4, ">");
                    pos += 1;
                }
                pos = 0;
                while ((pos = displayWord.find("&apos;", pos)) != std::string::npos) {
                    displayWord.replace(pos, 6, "'");
                    pos += 1;
                }
                displayWord = reverseHebrewWord(displayWord);
            }

            int w = gfx->textWidth(displayWord.c_str());
            int spaceWLocal = gfx->textWidth(" ");
            if (spaceWLocal == 0)
                spaceWLocal = 5;
            
            if (debugWordsLogged < 6)
            {
                ESP_LOGI(TAG, "drawLine: word='%s' w=%d space=%d isRTL=%d startX=%d y=%d", displayWord.c_str(), w, spaceWLocal, isRTL, startX, currentY);
                debugWordsLogged++;
            }

            if (isRTL)
            {
                drawStringMixed(displayWord, startX - w, currentY, (M5Canvas *)target, fontSize);
                startX -= (w + spaceWLocal);
            }
            else
            {
                drawStringMixed(displayWord, startX, currentY, (M5Canvas *)target, fontSize);
                startX += (w + spaceWLocal);
            }
        }
    };

    int linesDrawn = 0;
    while (i < text.length())
    {
        // Find next word boundary
        size_t nextSpace = text.find_first_of(" \n", i);
        if (nextSpace == std::string::npos)
            nextSpace = text.length();

        bool isNewline = (nextSpace < text.length() && text[nextSpace] == '\n');

        size_t wordLen = nextSpace - i;
        
        // Measure word using reusable buffer
        tempWord.assign(text, i, wordLen);
        
        bool wordIsHebrew = isHebrew(tempWord);
        if (wordIsHebrew)
            tempWord = reverseHebrewWord(tempWord);

        // Make sure we measure with the same font that will be used to draw the word.
        bool swappedFont = false;
        if (wordIsHebrew && !fontDataHebrew.empty() && currentFont != "Hebrew")
        {
            gfx->loadFont(fontDataHebrew.data());
            gfx->setTextSize(fontSize);
            swappedFont = true;
        }

        int w = gfx->textWidth(tempWord.c_str());
        int spaceW = gfx->textWidth(" ");
        if (spaceW == 0)
            spaceW = 5; // Fallback

        // Restore the display font
        if (swappedFont)
        {
            if (currentFont != "Default" && !fontData.empty())
            {
                gfx->loadFont(fontData.data());
            }
            else
            {
                gfx->unloadFont();
            }
            gfx->setTextSize(fontSize);
        }

        // Check if word fits
        if (currentLineWidth + w > maxWidth)
        {
            drawLine(currentLine);
            linesDrawn++;
            currentLine.clear();
            currentLineWidth = 0;
            currentY += lineHeight;

            if (currentY + lineHeight > maxY)
            {
                break; // Page full
            }
        }

        currentLine.push_back({i, wordLen});
        currentLineWidth += w + spaceW;

        if (nextSpace < text.length())
        {
            i = nextSpace + 1;
        }
        else
        {
            i = nextSpace;
        }

        if (isNewline)
        {
            drawLine(currentLine);
            linesDrawn++;
            currentLine.clear();
            currentLineWidth = 0;
            currentY += lineHeight;
            if (currentY + lineHeight > maxY)
            {
                break; // Page full
            }
        }
    }

    // Flush remaining line
    if (!currentLine.empty() && currentY + lineHeight <= maxY)
    {
        drawLine(currentLine);
        linesDrawn++;
    }

    if (draw)
    {
        // ESP_LOGI(TAG, "drawPageContentAt: lines drawn: %d", linesDrawn);
        gfx->endWrite();
    }

    return i;
}

GUI::PageInfo GUI::calculatePageInfo()
{
    // Current page is tracked via history stack
    PageInfo info{};
    info.current = static_cast<int>(pageHistory.size()) + 1;

    // Ensure we have a recent char count for this font/layout
    if (lastPageChars < PAGEINFO_ESTIMATE_MIN_CHARS)
    {
        size_t chars = drawPageContentAt(currentTextOffset, false);
        if (chars > 0)
        {
            lastPageChars = chars;
        }
    }

    size_t chapterSize = epubLoader.getChapterSize();
    if (lastPageChars > 0)
    {
        lastPageTotal = static_cast<int>((chapterSize + lastPageChars - 1) / lastPageChars);
    }
    else
    {
        lastPageTotal = 1;
    }

    if (lastPageTotal < 1)
        lastPageTotal = 1;
    if (info.current > lastPageTotal)
        info.current = lastPageTotal;
    info.total = lastPageTotal;
    return info;
}

void GUI::updateNextPrevCanvases()
{
    // Update Next
    size_t charsOnCurrent = drawPageContentAt(currentTextOffset, false);
    nextCanvasOffset = currentTextOffset + charsOnCurrent;

    if (nextCanvasOffset < epubLoader.getChapterSize() && canvasNext.width() > 0)
    {
        canvasNext.fillScreen(TFT_WHITE);
        drawPageContentAt(nextCanvasOffset, true, &canvasNext);
        nextCanvasValid = true;
    }
    else
    {
        nextCanvasValid = false;
    }

    // Update Prev
    if (!pageHistory.empty() && canvasPrev.width() > 0)
    {
        prevCanvasOffset = pageHistory.back();
        canvasPrev.fillScreen(TFT_WHITE);
        drawPageContentAt(prevCanvasOffset, true, &canvasPrev);
        prevCanvasValid = true;
    }
    else
    {
        prevCanvasValid = false;
    }
}

void GUI::drawReader(bool flush)
{
    ESP_LOGI(TAG, "drawReader called, currentTextOffset: %zu", currentTextOffset);

    // Ensure previous update is complete
    M5.Display.waitDisplay();

    bool drawnFromBuffer = false;

    // Only use buffers if they are valid (width > 0)
    if (nextCanvasValid && nextCanvasOffset == currentTextOffset && canvasNext.width() > 0)
    {
        ESP_LOGI(TAG, "Drawing from next buffer");
        canvasNext.pushSprite(&M5.Display, 0, 0);
        drawnFromBuffer = true;
    }
    else if (prevCanvasValid && prevCanvasOffset == currentTextOffset && canvasPrev.width() > 0)
    {
        ESP_LOGI(TAG, "Drawing from prev buffer");
        canvasPrev.pushSprite(&M5.Display, 0, 0);
        drawnFromBuffer = true;
    }

    if (!drawnFromBuffer)
    {
        ESP_LOGI(TAG, "Drawing to current buffer");
        M5.Display.fillScreen(TFT_WHITE);
        drawPageContentAt(currentTextOffset, true, nullptr);
    }

    // Draw Status Bar (Fresh)
    drawStatusBar();

    // Get chars drawn from the actual drawing (no redundant measurement)
    size_t charsDrawn = lastPageChars; // Use cached value if drawing from buffer
    if (!drawnFromBuffer) {
        // If we drew fresh, we need to measure
        charsDrawn = drawPageContentAt(currentTextOffset, false);
        ESP_LOGI(TAG, "Chars drawn: %zu", charsDrawn);
        if (charsDrawn > 0)
        {
            lastPageChars = charsDrawn;
            size_t chapterSize = epubLoader.getChapterSize();
            lastPageTotal = static_cast<int>((chapterSize + lastPageChars - 1) / lastPageChars);
            if (lastPageTotal < 1)
                lastPageTotal = 1;
        }
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

    if (flush) {
        ESP_LOGI(TAG, "Calling M5.Display.display()");
        M5.Display.display();
    }
    ESP_LOGI(TAG, "drawReader completed");

    // Update buffers for next interaction
    updateNextPrevCanvases();
}

void GUI::drawSleepSymbol(const char *symbol)
{
    M5.Display.setFont(&lgfx::v1::fonts::Font4); // Large font
    M5.Display.setTextSize(1.0f);
    M5.Display.setTextColor(TFT_BLACK, TFT_WHITE);
    M5.Display.setTextDatum(textdatum_t::top_center);
    // Draw in the middle of status bar or top center
    M5.Display.drawString(symbol, M5.Display.width() / 2, 5);
}

void GUI::goToSleep()
{
    // Save progress
    bookIndex.updateProgress(currentBook.id, epubLoader.getCurrentChapterIndex(), currentTextOffset);
    // Ensure we always remember the last-opened book for deep sleep wake restores
    {
        nvs_handle_t my_handle;
        if (nvs_open("storage", NVS_READWRITE, &my_handle) == ESP_OK)
        {
            nvs_set_i32(my_handle, "last_book_id", currentBook.id);
            nvs_set_i32(my_handle, "last_state", (int)currentState);
            nvs_commit(my_handle);
            nvs_close(my_handle);
        }
    }

    // Flush any pending display operations before sleeping
    M5.Display.waitDisplay();

    // Turn off WiFi to save power while sleeping
    wifi_mode_t mode = WIFI_MODE_NULL;
    bool shouldReconnect = false;
    if (esp_wifi_get_mode(&mode) == ESP_OK)
    {
        shouldReconnect = (mode == WIFI_MODE_STA || mode == WIFI_MODE_APSTA);
    }
    esp_wifi_stop();

    // Light sleep with touch wakeup handled by M5.Power. Also arm a timer so we can fall through to deep sleep.
    drawSleepSymbol("z");
    M5.Power.lightSleep(LIGHT_SLEEP_TO_DEEP_SLEEP_US, true);

    // Restore display state after wake from light sleep
    // drawSleepSymbol changed font, size, and datum. We must restore them.
    M5.Display.setTextDatum(textdatum_t::top_left);
    M5.Display.setTextColor(TFT_BLACK); // Restore transparent background
    if (currentFont == "Default") {
        M5.Display.setFont(&lgfx::v1::fonts::Font2); // Or whatever default is
    } else if (currentFont == "Hebrew" && !fontDataHebrew.empty()) {
        M5.Display.loadFont(fontDataHebrew.data());
    } else if (!fontData.empty()) {
        M5.Display.loadFont(fontData.data());
    } else {
        M5.Display.unloadFont();
    }
    M5.Display.setTextSize(fontSize);
    
    // Check for touch immediately after wake to process the wake-up interaction
    M5.update();
    justWokeUp = true;
    handleTouch();

    // Force redraw to clear the "z" symbol and show content immediately
    needsRedraw = true;
    lastActivityTime = (uint32_t)(esp_timer_get_time() / 1000);

    // If the timer woke us, hand off to deep sleep so the next touch resumes straight into the last page.
    auto wakeReason = esp_sleep_get_wakeup_cause();
    if (wakeReason == ESP_SLEEP_WAKEUP_TIMER)
    {
        ESP_LOGI(TAG, "Light sleep timer elapsed, entering deep sleep");
        drawSleepSymbol("Zz");
        
        enterDeepSleepWithTouchWake();
        return;
    }

    // Resume WiFi after wake
    esp_wifi_start();
    if (shouldReconnect)
    {
        esp_wifi_connect();
    }
}

void GUI::resetPageInfoCache()
{
    lastPageChars = 0;
    lastPageTotal = 1;
    pageHistory.clear();
    nextCanvasValid = false;
    prevCanvasValid = false;
}

void GUI::drawSettings()
{
    // Only redraw the reader content if specifically requested (e.g. font change)
    // Otherwise, we draw the settings panel on top of the existing screen content.
    if (settingsNeedsUnderlayRefresh) {
        drawReader(false); // Draw but don't flush yet
        settingsNeedsUnderlayRefresh = false;
    }

    SettingsLayout layout = computeSettingsLayout();

    // Initialize settings canvas if needed
    if (!settingsCanvasCreated) {
        settingsCanvas.setColorDepth(4); // Use 4-bit color to save RAM
        if (settingsCanvas.createSprite(layout.panelWidth, layout.panelHeight)) {
            settingsCanvasCreated = true;
        } else {
            ESP_LOGE(TAG, "Failed to create settings canvas - falling back to direct draw");
        }
    }

    LovyanGFX* target = settingsCanvasCreated ? (LovyanGFX*)&settingsCanvas : (LovyanGFX*)&M5.Display;
    int yOffset = settingsCanvasCreated ? -layout.panelTop : 0;

    // Clear background for settings panel
    target->fillRect(0, layout.panelTop + yOffset, layout.panelWidth, layout.panelHeight, TFT_WHITE);
    target->drawRect(0, layout.panelTop + yOffset, layout.panelWidth, layout.panelHeight, TFT_BLACK);
    target->drawLine(0, layout.panelTop + yOffset, layout.panelWidth, layout.panelTop + yOffset, TFT_BLACK); // Top border

    auto drawButton = [&](int x, int y, int w, int h, const char *label, uint16_t fillColor, uint16_t textColor)
    {
        // Adjust Y coordinates
        int drawY = y + yOffset;
        target->fillRect(x, drawY, w, h, fillColor);
        target->drawRect(x, drawY, w, h, TFT_BLACK);
        target->setTextDatum(textdatum_t::middle_center);
        target->setTextColor(textColor, fillColor);
        target->drawString(label, x + w / 2, drawY + h / 2);
        target->setTextColor(TFT_BLACK, TFT_WHITE);
        target->setTextDatum(textdatum_t::middle_left);
    };

    target->setTextColor(TFT_BLACK, TFT_WHITE);

    // Title
    target->setTextSize(1.6f);
    target->setTextDatum(textdatum_t::top_left);
    target->drawString("Settings", layout.padding, layout.titleY + yOffset);

    // Body styling
    const float bodyTextSize = 1.2f;
    target->setTextSize(bodyTextSize);
    target->setTextDatum(textdatum_t::middle_left);

    // --- Font Size ---
    int row1CenterY = layout.row1Y + layout.rowHeight / 2;
    target->drawString("Font Size", layout.padding, row1CenterY + yOffset);
    char sizeBuf[16];
    snprintf(sizeBuf, sizeof(sizeBuf), "%.1f", fontSize);
    int fontValueRight = layout.fontMinusX - 10;
    target->setTextDatum(textdatum_t::middle_right);
    target->drawString(sizeBuf, fontValueRight, row1CenterY + yOffset);
    target->setTextDatum(textdatum_t::middle_left);
    int fontButtonY = layout.row1Y + (layout.rowHeight - layout.fontButtonH) / 2;
    drawButton(layout.fontMinusX, fontButtonY, layout.fontButtonW, layout.fontButtonH, "-", TFT_WHITE, TFT_BLACK);
    drawButton(layout.fontPlusX, fontButtonY, layout.fontButtonW, layout.fontButtonH, "+", TFT_WHITE, TFT_BLACK);

    // --- Font Family ---
    int row2CenterY = layout.row2Y + layout.rowHeight / 2;
    target->drawString("Font", layout.padding, row2CenterY + yOffset);
    int fontLabelRight = layout.changeButtonX - 12;
    target->setTextDatum(textdatum_t::middle_right);
    target->drawString(currentFont.c_str(), fontLabelRight, row2CenterY + yOffset);
    target->setTextDatum(textdatum_t::middle_left);
    int changeButtonY = layout.row2Y + (layout.rowHeight - layout.fontButtonH) / 2;
    drawButton(layout.changeButtonX, changeButtonY, layout.changeButtonW, layout.fontButtonH, "Change", TFT_WHITE, TFT_BLACK);

    // --- WiFi ---
    int row3CenterY = layout.row3Y + layout.rowHeight / 2;
    target->drawString("WiFi", layout.padding, row3CenterY + yOffset);

    // Toggle Button
    int toggleButtonY = layout.row3Y + (layout.rowHeight - layout.fontButtonH) / 2;
    uint16_t wifiFill = wifiEnabled ? TFT_BLACK : TFT_WHITE;
    uint16_t wifiText = wifiEnabled ? TFT_WHITE : TFT_BLACK;
    drawButton(layout.toggleButtonX, toggleButtonY, layout.toggleButtonW, layout.fontButtonH,
               wifiEnabled ? "WiFi: ON" : "WiFi: OFF", wifiFill, wifiText);

    // --- WiFi Status Row ---
    int row4CenterY = layout.row4Y + layout.rowHeight / 2;
    target->drawString("WiFi Status", layout.padding, row4CenterY + yOffset);
    std::string status = wifiEnabled ? "Connecting..." : "WiFi is OFF";
    if (wifiEnabled && wifiManager.isConnected())
    {
        std::string ip = wifiManager.getIpAddress();
        status = "URL: http://" + ip + "/";
    }
    target->setTextSize(1.0f);
    target->setTextDatum(textdatum_t::middle_left);
    // Increased offset to avoid overlap
    target->drawString(status.c_str(), layout.padding + 180, row4CenterY + yOffset);
    target->setTextSize(bodyTextSize);

    // --- Close Button ---
    int closeX = layout.panelWidth - layout.padding - layout.closeButtonW;
    drawButton(closeX, layout.closeY, layout.closeButtonW, layout.closeButtonH, "Close", TFT_WHITE, TFT_BLACK);

    // Push the canvas to the display
    if (settingsCanvasCreated) {
        settingsCanvas.pushSprite(0, layout.panelTop);
    }
}

void GUI::drawWifiConfig()
{
    M5.Display.fillScreen(TFT_WHITE);
    M5.Display.setCursor(10, 50);
    M5.Display.println("WiFi Config Not Implemented in GUI");
    M5.Display.println("Use Web Interface");
    M5.Display.display();
}

void GUI::processReaderTap(int x, int y, bool isDouble)
{
    bool isRTL = isRTLDocument;
    bool next = false;

    if (x < M5.Display.width() / 2)
    {
        // Left side
        next = isRTL; // If RTL, left is next. If LTR, left is prev.
    }
    else
    {
        // Right side
        next = !isRTL; // If RTL, right is prev. If LTR, right is next.
    }

    if (isDouble) {
        ESP_LOGI(TAG, "Double click detected! Next=%d", next);
        if (next) {
            // Next Chapter
            if (epubLoader.nextChapter()) {
                currentTextOffset = 0;
                pageHistory.clear();
                resetPageInfoCache();
                needsRedraw = true;
            }
        } else {
            // Prev Chapter
            if (epubLoader.prevChapter()) {
                currentTextOffset = 0;
                pageHistory.clear();
                resetPageInfoCache();
                needsRedraw = true;
            }
        }
    } else {
        // Single Click - Page Turn
        if (!next)
        {
            // Prev Page
            if (pageHistory.empty())
            {
                // Try prev chapter
                if (epubLoader.prevChapter())
                {
                    // We want to go to the LAST page of the previous chapter.
                    // To do this, we must simulate paging through the entire chapter
                    // to build the pageHistory and find the last offset.
                    size_t chapterSize = epubLoader.getChapterSize();
                    if (chapterSize == 0) {
                        currentTextOffset = 0;
                        pageHistory.clear();
                    } else {
                        size_t scanOffset = 0;
                        std::vector<size_t> newHistory;
                        
                        // Scan chapter to find page breaks
                        while (scanOffset < chapterSize) {
                            size_t chars = drawPageContentAt(scanOffset, false);
                            if (chars == 0) {
                                // Should not happen unless error, fallback to start
                                currentTextOffset = 0;
                                newHistory.clear();
                                break;
                            }
                            
                            if (scanOffset + chars >= chapterSize) {
                                // This is the last page
                                currentTextOffset = scanOffset;
                                break;
                            }
                            
                            newHistory.push_back(scanOffset);
                            scanOffset += chars;
                        }
                        pageHistory = newHistory;
                    }
                    
                    resetPageInfoCache();
                    needsRedraw = true;
                }
            }
            else
            {
                currentTextOffset = pageHistory.back();
                pageHistory.pop_back();
                needsRedraw = true;
            }
        }
        else
        {
            // Next Page
            size_t charsOnPage = drawPageContent(false);
            if (currentTextOffset + charsOnPage >= epubLoader.getChapterSize())
            {
                // Next chapter
                if (epubLoader.nextChapter())
                {
                    currentTextOffset = 0;
                    pageHistory.clear();
                    resetPageInfoCache();
                    needsRedraw = true;
                }
            }
            else
            {
                pageHistory.push_back(currentTextOffset);
                currentTextOffset += charsOnPage;
                needsRedraw = true;
            }
        }
    }
}

void GUI::handleTouch()
{
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);

    // Check pending click timeout
    if (clickPending && (now - lastClickTime > 400)) // 400ms wait
    {
        clickPending = false;
        // Execute Single Click (Page Turn)
        processReaderTap(lastClickX, lastClickY, false); // false = single
    }

    if (M5.Touch.getCount() > 0)
    {
        lastActivityTime = now;
        auto t = M5.Touch.getDetail(0);
        if (t.wasPressed() || (justWokeUp && t.isPressed()))
        {
            if (justWokeUp) {
                ESP_LOGI(TAG, "Processing wake-up touch at %d, %d", t.x, t.y);
            }
            
            if (currentState == AppState::LIBRARY)
            {
                // Check for status bar tap
                if (t.y < STATUS_BAR_HEIGHT) {
                    currentState = AppState::WIFI_SCAN;
                    wifiList.clear();
                    needsRedraw = true;
                    justWokeUp = false;
                    return;
                }

                auto books = bookIndex.getBooks();
                int availableHeight = M5.Display.height() - LIBRARY_LIST_START_Y - 60;
                int itemsPerPage = availableHeight / LIBRARY_LINE_HEIGHT;
                if (itemsPerPage < 1)
                    itemsPerPage = 1;

                int totalPages = (books.size() + itemsPerPage - 1) / itemsPerPage;

                // Check paging buttons
                if (totalPages > 1 && t.y > M5.Display.height() - 60)
                {
                    if (t.x < 150 && libraryPage > 0)
                    {
                        libraryPage--;
                        needsRedraw = true;
                        justWokeUp = false;
                        return;
                    }
                    if (t.x > M5.Display.width() - 150 && libraryPage < totalPages - 1)
                    {
                        libraryPage++;
                        needsRedraw = true;
                        justWokeUp = false;
                        return;
                    }
                }

                int startIdx = libraryPage * itemsPerPage;
                int endIdx = std::min((int)books.size(), startIdx + itemsPerPage);

                int y = LIBRARY_LIST_START_Y;
                for (int i = startIdx; i < endIdx; ++i)
                {
                    if (t.y >= y && t.y < y + LIBRARY_LINE_HEIGHT)
                    {
                        ESP_LOGI(TAG, "Touched book at index %d, ID %d", i, books[i].id);
                        openBookById(books[i].id);
                        break;
                    }
                    y += LIBRARY_LINE_HEIGHT;
                }
            }
            else if (currentState == AppState::READER)
            {
                // Top tap to exit
                if (t.y < 50)
                {
                    clickPending = false; // Cancel any pending
                    currentState = AppState::LIBRARY;
                    epubLoader.close();
                    needsRedraw = true;
                    justWokeUp = false;
                    return;
                }

                // Bottom 1/5 -> Settings
                if (t.y > M5.Display.height() * 4 / 5)
                {
                    clickPending = false; // Cancel any pending
                    previousState = currentState;
                    currentState = AppState::SETTINGS;
                    needsRedraw = true;
                    justWokeUp = false;
                    return;
                }

                // Middle area - Page Turn / Chapter Skip
                if (clickPending) {
                    // Check if second click
                    bool sameSide = (t.x < M5.Display.width() / 2) == (lastClickX < M5.Display.width() / 2);
                    if (sameSide) {
                        // Double Click!
                        clickPending = false;
                        processReaderTap(t.x, t.y, true); // true = double
                    } else {
                        // Different side - treat previous as single, and this as new pending
                        clickPending = false;
                        processReaderTap(lastClickX, lastClickY, false);
                        
                        lastClickTime = now;
                        lastClickX = t.x;
                        lastClickY = t.y;
                        clickPending = true;
                    }
                } else {
                    // First click
                    lastClickTime = now;
                    lastClickX = t.x;
                    lastClickY = t.y;
                    clickPending = true;
                }
            }
            else if (currentState == AppState::SETTINGS)
            {
                SettingsLayout layout = computeSettingsLayout();
                
                // Close if clicked outside the panel (upper half)
                if (t.y < layout.panelTop)
                {
                    currentState = previousState;
                    needsRedraw = true;
                    if (settingsCanvasCreated) {
                        settingsCanvas.deleteSprite();
                        settingsCanvasCreated = false;
                    }
                    return;
                }

                int fontButtonY = layout.row1Y + (layout.rowHeight - layout.fontButtonH) / 2;
                int changeButtonY = layout.row2Y + (layout.rowHeight - layout.fontButtonH) / 2;
                int toggleButtonY = layout.row3Y + (layout.rowHeight - layout.fontButtonH) / 2;
                int closeX = layout.panelWidth - layout.padding - layout.closeButtonW;
                int closeButtonY = layout.closeY;

                // Font Size [-]
                if (t.y >= fontButtonY && t.y <= fontButtonY + layout.fontButtonH && t.x >= layout.fontMinusX && t.x <= layout.fontMinusX + layout.fontButtonW)
                {
                    setFontSize(fontSize - 0.1f);
                    settingsNeedsUnderlayRefresh = true;
                }
                // Font Size [+]
                else if (t.y >= fontButtonY && t.y <= fontButtonY + layout.fontButtonH && t.x >= layout.fontPlusX && t.x <= layout.fontPlusX + layout.fontButtonW)
                {
                    setFontSize(fontSize + 0.1f);
                    settingsNeedsUnderlayRefresh = true;
                }
                // Font Change
                else if (t.y >= changeButtonY && t.y <= changeButtonY + layout.fontButtonH && t.x >= layout.changeButtonX && t.x <= layout.changeButtonX + layout.changeButtonW)
                {
                    if (currentFont == "Default") setFont("Hebrew");
                    else if (currentFont == "Hebrew") setFont("Roboto");
                    else setFont("Default");
                    settingsNeedsUnderlayRefresh = true;
                }
                // WiFi Toggle
                else if (t.y >= toggleButtonY && t.y <= toggleButtonY + layout.fontButtonH && t.x >= layout.toggleButtonX && t.x <= layout.toggleButtonX + layout.toggleButtonW)
                {
                    wifiEnabled = !wifiEnabled;
                    saveSettings();
                    
                    if (wifiEnabled) {
                        if (!wifiManager.isInitialized()) {
                            wifiManager.init();
                        }
                        wifiManager.connect();
                        webServer.init("/spiffs");
                    } else {
                        webServer.stop();
                        wifiManager.disconnect();
                        wifiConnected = false;
                    }
                    needsRedraw = true;
                }
                // Close
                else if (t.y >= closeButtonY && t.y <= closeButtonY + layout.closeButtonH && t.x >= closeX && t.x <= closeX + layout.closeButtonW)
                {
                    currentState = previousState;
                    needsRedraw = true;
                    if (settingsCanvasCreated) {
                        settingsCanvas.deleteSprite();
                        settingsCanvasCreated = false;
                    }
                }
            }
            else if (currentState == AppState::WIFI_SCAN)
            {
                onWifiScanClick(t.x, t.y);
            }
            else if (currentState == AppState::WIFI_PASSWORD)
            {
                onWifiPasswordClick(t.x, t.y);
            }
        }
    }
    justWokeUp = false;
}

void GUI::jumpTo(float percent)
{
    ESP_LOGI(TAG, "GUI::jumpTo called with percent=%.2f (state=%d)", percent, (int)currentState);
    if (currentState != AppState::READER) {
        ESP_LOGW(TAG, "jumpTo ignored - not in READER state");
        return;
    }

    size_t size = epubLoader.getChapterSize();
    if (size == 0) {
        ESP_LOGW(TAG, "jumpTo: current chapter size is 0, ignoring");
        return;
    }

    if (percent < 0)
        percent = 0;
    if (percent > 100)
        percent = 100;

    size_t newOffset = (size_t)((percent / 100.0f) * size);
    ESP_LOGI(TAG, "GUI::jumpTo -> percent=%.2f => offset=%zu / chapterSize=%zu", percent, newOffset, size);
    currentTextOffset = newOffset;

    pageHistory.clear();
    resetPageInfoCache();
    needsRedraw = true;
}

void GUI::jumpToChapter(int chapter)
{
    ESP_LOGI(TAG, "GUI::jumpToChapter called chapter=%d (state=%d)", chapter, (int)currentState);
    if (currentState != AppState::READER) {
        ESP_LOGW(TAG, "jumpToChapter ignored - not in READER state");
        return;
    }

    if (epubLoader.jumpToChapter(chapter))
    {
        ESP_LOGI(TAG, "jumpToChapter: loaded chapter %d size=%zu", chapter, epubLoader.getChapterSize());
        currentTextOffset = 0;
        pageHistory.clear();
        resetPageInfoCache();
        needsRedraw = true;
    } else {
        ESP_LOGW(TAG, "jumpToChapter: failed to load chapter %d", chapter);
    }
}

bool GUI::openBookById(int id)
{
    BookEntry book = bookIndex.getBook(id);
    if (book.id == 0)
        return false;

    if (!epubLoader.load(book.path.c_str()))
    {
        return false;
    }

    currentBook = book;
    currentState = AppState::READER;

    // Auto-detect language for font
    std::string lang = epubLoader.getLanguage();
    if (lang.find("he") != std::string::npos || lang.find("HE") != std::string::npos)
    {
        setFont("Hebrew");
    }

    // Restore progress
    currentTextOffset = currentBook.currentOffset;
    resetPageInfoCache();
    if (currentBook.currentChapter > 0)
    {
        while (epubLoader.getCurrentChapterIndex() < currentBook.currentChapter)
        {
            if (!epubLoader.nextChapter())
                break;
        }
    }
    isRTLDocument = detectRTLDocument(epubLoader, currentTextOffset);

    pageHistory.clear();
    needsRedraw = true;

    // Save as last book
    nvs_handle_t my_handle;
    if (nvs_open("storage", NVS_READWRITE, &my_handle) == ESP_OK)
    {
        nvs_set_i32(my_handle, "last_book_id", currentBook.id);
        nvs_commit(my_handle);
        nvs_close(my_handle);
    }

    return true;
}

void GUI::refreshLibrary()
{
    if (currentState == AppState::LIBRARY)
    {
        needsRedraw = true;
    }
}

void GUI::saveSettings()
{
    nvs_handle_t my_handle;
    if (nvs_open("storage", NVS_READWRITE, &my_handle) == ESP_OK)
    {
        int32_t sizeInt = (int32_t)(fontSize * 10);
        nvs_set_i32(my_handle, "font_size", sizeInt);
        nvs_set_str(my_handle, "font_name", currentFont.c_str());
        nvs_set_i32(my_handle, "wifi_enabled", wifiEnabled ? 1 : 0);
        nvs_commit(my_handle);
        nvs_close(my_handle);
    }
}

void GUI::setFontSize(float size)
{
    if (size < 0.5f)
        size = 0.5f;
    if (size > 5.0f)
        size = 5.0f;
    fontSize = size;
    saveSettings();
    resetPageInfoCache();
    needsRedraw = true;
}

void GUI::setFont(const std::string &fontName)
{
    if (currentFont == fontName)
        return;
    currentFont = fontName;
    saveSettings();
    fontChanged = true;
    needsRedraw = true;
}

void GUI::loadSettings()
{
    nvs_handle_t my_handle;
    if (nvs_open("storage", NVS_READONLY, &my_handle) == ESP_OK)
    {
        int32_t sizeInt = 10;
        if (nvs_get_i32(my_handle, "font_size", &sizeInt) == ESP_OK)
        {
            fontSize = sizeInt / 10.0f;
        }

        size_t required_size;
        if (nvs_get_str(my_handle, "font_name", NULL, &required_size) == ESP_OK)
        {
            char *fontName = new char[required_size];
            nvs_get_str(my_handle, "font_name", fontName, &required_size);
            currentFont = fontName;
            delete[] fontName;
        }

        int32_t wifiEn = 1;
        if (nvs_get_i32(my_handle, "wifi_enabled", &wifiEn) == ESP_OK)
        {
            wifiEnabled = (wifiEn != 0);
        }

        nvs_close(my_handle);
    }
}

void GUI::loadFonts()
{
    // Clear buffers to ensure we don't hold two fonts at once if possible
    fontData.clear();
    fontDataHebrew.clear();
    M5.Display.unloadFont();

    if (currentFont == "Default")
    {
        return;
    }

    if (currentFont == "Hebrew")
    {
        ensureHebrewFontLoaded();
        if (!fontDataHebrew.empty())
        {
            M5.Display.loadFont(fontDataHebrew.data());
        }
        return;
    }

    // Load other font (e.g. Roboto)
    std::string path = "/spiffs/fonts/" + currentFont + ".vlw";
    if (currentFont == "Roboto")
        path = "/spiffs/fonts/Roboto-Regular.vlw";

    ESP_LOGI(TAG, "Loading font: %s", path.c_str());
    FILE *f = fopen(path.c_str(), "rb");
    if (f)
    {
        fseek(f, 0, SEEK_END);
        size_t size = ftell(f);
        fseek(f, 0, SEEK_SET);
        fontData.resize(size);
        fread(fontData.data(), 1, size, f);
        fclose(f);
        M5.Display.loadFont(fontData.data());
    }
    else
    {
        ESP_LOGW(TAG, "Failed to load font: %s", path.c_str());
        currentFont = "Default";
    }
}

void GUI::ensureHebrewFontLoaded()
{
    if (!fontDataHebrew.empty())
    {
        // ESP_LOGI(TAG, "Hebrew font already loaded");
        return;
    }

    // CRITICAL: Free other font memory before loading Hebrew to avoid OOM
    if (!fontData.empty())
    {
        ESP_LOGI(TAG, "Unloading primary font to make room for Hebrew");
        fontData.clear();
        M5.Display.unloadFont();
    }

    const char *path = "/spiffs/fonts/Hebrew-Merged.vlw";
    ESP_LOGI(TAG, "Attempting to load Hebrew font from: %s", path);
    FILE *f = fopen(path, "rb");
    if (f)
    {
        fseek(f, 0, SEEK_END);
        size_t size = ftell(f);
        fseek(f, 0, SEEK_SET);

        // Try to allocate in PSRAM first if available
        // We use resize() which uses default allocator, but we can reserve first?
        // std::vector doesn't support custom allocators easily without changing type.
        // But we can try to force heap caps if we used a custom allocator.
        // For now, let's just rely on system malloc. If PSRAM is configured, large blocks go there.
        // But if not, we might fail.

        try
        {
            fontDataHebrew.resize(size);
        }
        catch (const std::bad_alloc &e)
        {
            ESP_LOGE(TAG, "Failed to allocate memory for Hebrew font (%u bytes)", (unsigned int)size);
            fclose(f);
            return;
        }

        size_t read = fread(fontDataHebrew.data(), 1, size, f);
        fclose(f);
        ESP_LOGI(TAG, "Hebrew font loaded: %u bytes read (expected %u)", (unsigned int)read, (unsigned int)size);

        // Verify the font data starts with a valid header
        if (read >= 24)
        {
            uint32_t glyph_count = (fontDataHebrew[0] << 24) | (fontDataHebrew[1] << 16) |
                                   (fontDataHebrew[2] << 8) | fontDataHebrew[3];
            ESP_LOGI(TAG, "Hebrew font header: glyph_count=%u", glyph_count);
        }
    }
    else
    {
        ESP_LOGE(TAG, "Failed to open Hebrew font file: %s (errno=%d)", path, errno);
    }
}

void GUI::drawStringMixed(const std::string &text, int x, int y, M5Canvas *target, float size)
{
    LovyanGFX *gfx = target ? (LovyanGFX *)target : (LovyanGFX *)&M5.Display;
    float effectiveSize = (size > 0.0f) ? size : fontSize;

    if (isHebrew(text))
    {
        ensureHebrewFontLoaded();
        if (!fontDataHebrew.empty())
        {
            bool swapped = false;
            // Only swap if not already using Hebrew font
            if (currentFont != "Hebrew") {
                gfx->loadFont(fontDataHebrew.data());
                swapped = true;
            }
            
            std::string processedText = text;
            // HTML entity replacements are done before reversing in the calling functions     

            gfx->setTextSize(effectiveSize);
            gfx->drawString(processedText.c_str(), x, y);

            if (swapped) {
                if (currentFont != "Default" && !fontData.empty()) {
                    gfx->loadFont(fontData.data());
                } else {
                    gfx->unloadFont();
                }
                // Restore size to effectiveSize to ensure consistency for caller
                gfx->setTextSize(effectiveSize);
            }
        }
        else
        {
            // Fallback
            gfx->setTextSize(effectiveSize);
            gfx->drawString(text.c_str(), x, y);
        }
    }
    else
    {
        // Non-Hebrew text - use current font
        // ESP_LOGI(TAG, "Drawing non-Hebrew text: %s, font size: %.2f", text.c_str(), effectiveSize);
        std::string processedText = text;
        // Replace HTML entities
        size_t pos = 0;
        while ((pos = processedText.find("&quot;", pos)) != std::string::npos) {
            processedText.replace(pos, 6, "\"");
            pos += 1;
        }
        pos = 0;
        while ((pos = processedText.find("&amp;", pos)) != std::string::npos) {
            processedText.replace(pos, 5, "&");
            pos += 1;
        }
        pos = 0;
        while ((pos = processedText.find("&lt;", pos)) != std::string::npos) {
            processedText.replace(pos, 4, "<");
            pos += 1;
        }
        pos = 0;
        while ((pos = processedText.find("&gt;", pos)) != std::string::npos) {
            processedText.replace(pos, 4, ">");
            pos += 1;
        }
        pos = 0;
        while ((pos = processedText.find("&apos;", pos)) != std::string::npos) {
            processedText.replace(pos, 6, "'");
            pos += 1;
        }
        gfx->setTextSize(effectiveSize);
        gfx->drawString(processedText.c_str(), x, y);
    }
}

void GUI::drawWifiScan()
{
    M5.Display.unloadFont(); // Use default font
    {
        M5Canvas wifiCanvas;
        wifiCanvas.setColorDepth(1); // Use 1-bit color to save RAM
        if (wifiCanvas.createSprite(M5.Display.width(), M5.Display.height())) {
            wifiCanvas.fillScreen(TFT_WHITE);
            // Draw status bar to canvas
            wifiCanvas.setFont(&lgfx::v1::fonts::Font2);
            wifiCanvas.setTextSize(2.0f);
            wifiCanvas.fillRect(0, 0, wifiCanvas.width(), STATUS_BAR_HEIGHT, TFT_WHITE);
            wifiCanvas.drawFastHLine(0, STATUS_BAR_HEIGHT - 1, wifiCanvas.width(), TFT_BLACK);
            wifiCanvas.setTextColor(TFT_BLACK, TFT_WHITE);
            const int centerY = STATUS_BAR_HEIGHT / 2;
            wifiCanvas.setTextDatum(textdatum_t::middle_left);
            auto dt = M5.Rtc.getDateTime();
            char buf[32];
            snprintf(buf, sizeof(buf), "%02d:%02d", dt.time.hours, dt.time.minutes);
            wifiCanvas.drawString(buf, 10, centerY);
            wifiCanvas.setTextDatum(textdatum_t::middle_right);
            int bat = M5.Power.getBatteryLevel();
            snprintf(buf, sizeof(buf), "%d%%", bat);
            int batteryX = wifiCanvas.width() - 10;
            int batteryTextWidth = wifiCanvas.textWidth(buf);
            wifiCanvas.drawString(buf, batteryX, centerY);
            if (wifiConnected)
            {
                snprintf(buf, sizeof(buf), "WiFi %d dBm", wifiRssi);
            }
            else
            {
                snprintf(buf, sizeof(buf), "No WiFi");
            }
            int wifiX = batteryX - batteryTextWidth - 12;
            wifiCanvas.drawString(buf, wifiX, centerY);
            wifiCanvas.setTextDatum(textdatum_t::top_left);
            wifiCanvas.setTextColor(TFT_BLACK);
            
            wifiCanvas.setTextSize(2.0f);
            wifiCanvas.setTextColor(TFT_BLACK);
            wifiCanvas.setCursor(20, 60);
            wifiCanvas.println("Select WiFi Network:");
            
            if (wifiList.empty()) {
                wifiCanvas.setCursor(20, 100);
                wifiCanvas.println("Scanning...");
                wifiCanvas.pushSprite(&M5.Display, 0, 0);
                
                wifiList = wifiManager.scanNetworks();
                
                // If still empty after scan
                if (wifiList.empty()) {
                    wifiCanvas.setCursor(20, 100);
                    wifiCanvas.println("No networks found.");
                    wifiCanvas.pushSprite(&M5.Display, 0, 0);
                } else {
                    // Redraw with list
                    drawWifiScan();
                    return;
                }
            }
            
            int y = 100;
            for (const auto& ssid : wifiList) {
                wifiCanvas.drawRect(20, y, wifiCanvas.width() - 40, 50, TFT_BLACK);
                wifiCanvas.drawString(ssid.c_str(), 30, y + 15);
                y += 60;
                if (y > wifiCanvas.height() - 80) break; // Limit list
            }
            
            // Cancel Button
            int footerY = wifiCanvas.height() - 60;
            wifiCanvas.fillRect(20, footerY, 150, 40, TFT_LIGHTGREY);
            wifiCanvas.drawRect(20, footerY, 150, 40, TFT_BLACK);
            wifiCanvas.setTextColor(TFT_BLACK);
            wifiCanvas.setTextDatum(textdatum_t::middle_center);
            wifiCanvas.drawString("Cancel", 95, footerY + 20);
            wifiCanvas.setTextDatum(textdatum_t::top_left);
            
            wifiCanvas.pushSprite(&M5.Display, 0, 0);
            return;
        }
    }
    // Fallback to direct draw
    ESP_LOGE(TAG, "Failed to create wifi canvas - falling back to direct draw");
    M5.Display.fillScreen(TFT_WHITE);
    // Draw status bar directly without loading custom fonts
    M5.Display.setFont(&lgfx::v1::fonts::Font2);
    M5.Display.setTextSize(2.0f);
    M5.Display.fillRect(0, 0, M5.Display.width(), STATUS_BAR_HEIGHT, TFT_WHITE);
    M5.Display.drawFastHLine(0, STATUS_BAR_HEIGHT - 1, M5.Display.width(), TFT_BLACK);
    M5.Display.setTextColor(TFT_BLACK, TFT_WHITE);
    const int centerY = STATUS_BAR_HEIGHT / 2;
    M5.Display.setTextDatum(textdatum_t::middle_left);
    auto dt = M5.Rtc.getDateTime();
    char buf[32];
    snprintf(buf, sizeof(buf), "%02d:%02d", dt.time.hours, dt.time.minutes);
    M5.Display.drawString(buf, 10, centerY);
    M5.Display.setTextDatum(textdatum_t::middle_right);
    int bat = M5.Power.getBatteryLevel();
    snprintf(buf, sizeof(buf), "%d%%", bat);
    int batteryX = M5.Display.width() - 10;
    int batteryTextWidth = M5.Display.textWidth(buf);
    M5.Display.drawString(buf, batteryX, centerY);
    if (wifiConnected)
    {
        snprintf(buf, sizeof(buf), "WiFi %d dBm", wifiRssi);
    }
    else
    {
        snprintf(buf, sizeof(buf), "No WiFi");
    }
    int wifiX = batteryX - batteryTextWidth - 12;
    M5.Display.drawString(buf, wifiX, centerY);
    M5.Display.setTextDatum(textdatum_t::top_left);
    M5.Display.setTextColor(TFT_BLACK);
    
    M5.Display.setTextSize(2.0f);
    M5.Display.setTextColor(TFT_BLACK);
    M5.Display.setCursor(20, 60);
    M5.Display.println("Select WiFi Network:");
    
    if (wifiList.empty()) {
        M5.Display.setCursor(20, 100);
        M5.Display.println("Scanning...");
        M5.Display.display();
        
        wifiList = wifiManager.scanNetworks();
        
        // If still empty after scan
        if (wifiList.empty()) {
            M5.Display.setCursor(20, 100);
            M5.Display.println("No networks found.");
        } else {
            // Redraw with list
            drawWifiScan();
            return;
        }
    }
    
    int y = 100;
    for (const auto& ssid : wifiList) {
        M5.Display.drawRect(20, y, M5.Display.width() - 40, 50, TFT_BLACK);
        M5.Display.drawString(ssid.c_str(), 30, y + 15);
        y += 60;
        if (y > M5.Display.height() - 80) break; // Limit list
    }
    
    // Cancel Button
    int footerY = M5.Display.height() - 60;
    M5.Display.fillRect(20, footerY, 150, 40, TFT_LIGHTGREY);
    M5.Display.drawRect(20, footerY, 150, 40, TFT_BLACK);
    M5.Display.setTextColor(TFT_BLACK);
    M5.Display.setTextDatum(textdatum_t::middle_center);
    M5.Display.drawString("Cancel", 95, footerY + 20);
    M5.Display.setTextDatum(textdatum_t::top_left);
    
    M5.Display.display();
}

void GUI::onWifiScanClick(int x, int y)
{
    // Check Cancel
    int footerY = M5.Display.height() - 60;
    if (y >= footerY && y <= footerY + 40 && x >= 20 && x <= 170) {
        currentState = AppState::LIBRARY;
        needsRedraw = true;
        return;
    }
    
    // Check List
    int listY = 100;
    for (const auto& ssid : wifiList) {
        if (y >= listY && y <= listY + 50 && x >= 20 && x <= M5.Display.width() - 20) {
            selectedSSID = ssid;
            wifiPasswordInput = "";
            currentState = AppState::WIFI_PASSWORD;
            needsRedraw = true;
            return;
        }
        listY += 60;
        if (listY > M5.Display.height() - 80) break;
    }
}


void GUI::drawWifiPassword()
{
    //M5.Display.unloadFont(); // Use default font
    M5.Display.fillScreen(TFT_WHITE);
    previousFont = std::string(currentFont);
    this->setFont("Default");
    drawStatusBar();
    
    M5.Display.setTextSize(2.0f);
    M5.Display.setTextColor(TFT_BLACK);
    M5.Display.setCursor(20, 60);
    M5.Display.printf("Enter Password for %s:", selectedSSID.c_str());
    
    // Input Box
    M5.Display.drawRect(20, 100, M5.Display.width() - 40, 50, TFT_BLACK);
    M5.Display.drawString(wifiPasswordInput.c_str(), 30, 115);
    
    // Keyboard (Simple QWERTY)
    const char* rows[] = {
        "1234567890",
        "qwertyuiop",
        "asdfghjkl",
        "zxcvbnm"
    };
    
    int keySize = 45;
    int startY = 180;
    int startX = 20;
    
    M5.Display.setTextDatum(textdatum_t::middle_center);
    
    for (int r = 0; r < 4; r++) {
        int x = startX + (r * 20); // Indent
        for (int c = 0; c < strlen(rows[r]); c++) {
            char key[2] = {rows[r][c], 0};
            M5.Display.drawRect(x, startY + (r * keySize), keySize, keySize, TFT_BLACK);
            M5.Display.drawString(key, x + keySize/2, startY + (r * keySize) + keySize/2);
            x += keySize;
        }
    }
    
    // Special Keys: Backspace, Space, Clear
    int specialY = startY + (4 * keySize) + 10;
    
    // Backspace
    M5.Display.drawRect(20, specialY, 100, 40, TFT_BLACK);
    M5.Display.drawString("Bksp", 70, specialY + 20);
    
    // Space
    M5.Display.drawRect(130, specialY, 200, 40, TFT_BLACK);
    M5.Display.drawString("Space", 230, specialY + 20);
    
    // Connect
    M5.Display.fillRect(M5.Display.width() - 140, specialY, 120, 40, TFT_BLACK);
    M5.Display.setTextColor(TFT_WHITE);
    M5.Display.drawString("Connect", M5.Display.width() - 80, specialY + 20);
    
    // Cancel
    M5.Display.setTextColor(TFT_BLACK);
    M5.Display.drawRect(M5.Display.width() - 270, specialY, 120, 40, TFT_BLACK);
    M5.Display.drawString("Cancel", M5.Display.width() - 210, specialY + 20);
    
    M5.Display.setTextDatum(textdatum_t::top_left);
    M5.Display.display();
}

void GUI::onWifiPasswordClick(int x, int y)
{
    int keySize = 45;
    int startY = 180;
    int startX = 20;
    
    // Check Keyboard
    const char* rows[] = {
        "1234567890",
        "qwertyuiop",
        "asdfghjkl",
        "zxcvbnm"
    };
    
    bool inputChanged = false;
    
    for (int r = 0; r < 4; r++) {
        int rowX = startX + (r * 20);
        if (y >= startY + (r * keySize) && y < startY + ((r+1) * keySize)) {
            int col = (x - rowX) / keySize;
            if (col >= 0 && col < strlen(rows[r])) {
                wifiPasswordInput += rows[r][col];
                inputChanged = true;
            }
        }
    }
    
    int specialY = startY + (4 * keySize) + 10;
    if (y >= specialY && y <= specialY + 40) {
        // Backspace
        if (x >= 20 && x <= 120) {
            if (!wifiPasswordInput.empty()) {
                wifiPasswordInput.pop_back();
                inputChanged = true;
            }
        }
        // Space
        else if (x >= 130 && x <= 330) {
            wifiPasswordInput += ' ';
            inputChanged = true;
        }
        // Cancel
        else if (x >= M5.Display.width() - 270 && x <= M5.Display.width() - 150) {
            currentState = AppState::WIFI_SCAN;
            needsRedraw = true;
        }
        // Connect
        else if (x >= M5.Display.width() - 140 && x <= M5.Display.width() - 20) {
            M5.Display.fillScreen(TFT_WHITE);
            M5.Display.setCursor(20, 100);
            M5.Display.setTextSize(2.0f);
            M5.Display.println("Connecting...");
            M5.Display.display();
            
            wifiManager.saveCredentials(selectedSSID.c_str(), wifiPasswordInput.c_str());
            wifiManager.connect();
                   
            loadSettings();
            loadFonts();

            currentState = AppState::LIBRARY;
            needsRedraw = true;
        }
    }
    
    if (inputChanged) {
        // Update only the input box area
        M5.Display.fillRect(21, 101, M5.Display.width() - 42, 48, TFT_WHITE);
        M5.Display.setTextColor(TFT_BLACK);
        M5.Display.drawString(wifiPasswordInput.c_str(), 30, 115);
        M5.Display.display();
    }
}

