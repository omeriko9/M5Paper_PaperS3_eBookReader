#include "gui.h"
#include "web_server.h"
#include "epub_loader.h"
#include "math_renderer.h"
#include "esp_timer.h"
#include "esp_task_wdt.h"
#include "wifi_manager.h"
#include "book_index.h"
#include "device_hal.h"
#include "image_handler.h"
#include "gesture_detector.h"
#include "game_manager.h"
#include "composer_ui.h"
#include <vector>
#include <cctype>
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
#include <esp_heap_caps.h>
#include <new>
#include "sdkconfig.h"
#include <ctime>
#include <random>
#include <sys/stat.h>

// Image placeholder character (U+E000 in UTF-8)
static const char IMAGE_PLACEHOLDER[] = "\xEE\x80\x80";
static const size_t IMAGE_PLACEHOLDER_LEN = 3;

static const char *TAG = "GUI";

// Math renderer instance
extern MathRenderer mathRenderer;

static bool takeEpubMutexWithWdt(SemaphoreHandle_t mutex, volatile bool *abort = nullptr)
{
    if (!mutex)
    {
        return false;
    }
    while (true)
    {
        if (xSemaphoreTake(mutex, pdMS_TO_TICKS(50)) == pdTRUE)
        {
            return true;
        }
        esp_task_wdt_reset();
        if (abort && *abort)
        {
            return false;
        }
    }
}
bool GUI::canJump() const
{
    return currentState == AppState::READER;
}

EpubLoader epubLoader;
extern BookIndex bookIndex;
extern DeviceHAL &deviceHAL;
bool wifiConnected = false;
int wifiRssi = 0;
extern WifiManager wifiManager;
extern WebServer webServer;

static constexpr int STATUS_BAR_HEIGHT = 44;
static constexpr int SEARCH_BAR_HEIGHT = 50;
static constexpr int SEARCH_BAR_Y = STATUS_BAR_HEIGHT + 10;
static constexpr int LIBRARY_LIST_START_Y = STATUS_BAR_HEIGHT + SEARCH_BAR_HEIGHT + 25;
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
    int row5Y;
    int row6Y; // For favorite toggle
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
    l.panelHeight = M5.Display.height() * 0.6; // Increased to fit more rows
    l.panelTop = M5.Display.height() - l.panelHeight;
    l.padding = 30;
    l.rowHeight = 50; // Reduced to fit more rows
    l.titleY = l.panelTop + 15;
    l.row1Y = l.panelTop + 50;
    l.row2Y = l.row1Y + l.rowHeight;
    l.row3Y = l.row2Y + l.rowHeight;
    l.row4Y = l.row3Y + l.rowHeight;
    l.row5Y = l.row4Y + l.rowHeight;
    l.row6Y = l.row5Y + l.rowHeight; // Favorite row
    l.closeY = l.panelTop + l.panelHeight - 60;
    l.buttonGap = 20;
    l.fontButtonW = 80;
    l.fontButtonH = 40; // Slightly smaller
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

int GUI::getCurrentChapterIndex() const
{
    int idx = 0;
    if (xSemaphoreTake(epubMutex, portMAX_DELAY))
    {
        idx = epubLoader.getCurrentChapterIndex();
        xSemaphoreGive(epubMutex);
    }
    return idx;
}

size_t GUI::getCurrentChapterSize() const
{
    if (currentState != AppState::READER)
        return 0;
    size_t size = 0;
    if (xSemaphoreTake(epubMutex, portMAX_DELAY))
    {
        size = epubLoader.getChapterSize();
        xSemaphoreGive(epubMutex);
    }
    return size;
}

size_t GUI::getCurrentOffset() const
{
    if (currentState != AppState::READER)
        return 0;
    return currentTextOffset;
}
static constexpr int PAGEINFO_ESTIMATE_MIN_CHARS = 50;
static constexpr uint64_t LIGHT_SLEEP_TO_DEEP_SLEEP_US = 5ULL * 60ULL * 1000000ULL; // 5 minutes

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

static bool isArabic(const std::string &word)
{
    for (size_t i = 0; i < word.length(); ++i)
    {
        unsigned char c = (unsigned char)word[i];
        // Arabic block (0600-06FF) starts with 0xD8-0xDB in UTF-8
        // Presentation forms (FB50-FEFF) start with 0xEF 0xAC-0xEF 0xBB
        if (c >= 0xD8 && c <= 0xDB)
            return true;
        if (c == 0xEF)
        {
            if (i + 1 < word.length())
            {
                unsigned char c2 = (unsigned char)word[i + 1];
                if (c2 >= 0xAC && c2 <= 0xBB)
                    return true;
            }
        }
    }
    return false;
}

struct ArabicForm
{
    uint16_t code;
    uint16_t isolated;
    uint16_t initial;
    uint16_t medial;
    uint16_t final;
};

static const ArabicForm arabicForms[] = {
    {0x0621, 0xFE80, 0xFE80, 0xFE80, 0xFE80}, // Hamza
    {0x0622, 0xFE81, 0xFE81, 0xFE82, 0xFE82}, // Alef with Madda
    {0x0623, 0xFE83, 0xFE83, 0xFE84, 0xFE84}, // Alef with Hamza Above
    {0x0624, 0xFE85, 0xFE85, 0xFE86, 0xFE86}, // Waw with Hamza Above
    {0x0625, 0xFE87, 0xFE87, 0xFE88, 0xFE88}, // Alef with Hamza Below
    {0x0626, 0xFE89, 0xFE8B, 0xFE8C, 0xFE8A}, // Yeh with Hamza Above
    {0x0627, 0xFE8D, 0xFE8D, 0xFE8E, 0xFE8E}, // Alef
    {0x0628, 0xFE8F, 0xFE91, 0xFE92, 0xFE90}, // Beh
    {0x0629, 0xFE93, 0xFE93, 0xFE94, 0xFE94}, // Teh Marbuta
    {0x062A, 0xFE95, 0xFE97, 0xFE98, 0xFE96}, // Teh
    {0x062B, 0xFE99, 0xFE9B, 0xFE9C, 0xFE9A}, // Theh
    {0x062C, 0xFE9D, 0xFE9F, 0xFEA0, 0xFE9E}, // Jeem
    {0x062D, 0xFEA1, 0xFEA3, 0xFEA4, 0xFEA2}, // Hah
    {0x062E, 0xFEA5, 0xFEA7, 0xFEA8, 0xFEA6}, // Khah
    {0x062F, 0xFEA9, 0xFEA9, 0xFEAA, 0xFEAA}, // Dal
    {0x0630, 0xFEAB, 0xFEAB, 0xFEAC, 0xFEAC}, // Thal
    {0x0631, 0xFEAD, 0xFEAD, 0xFEAE, 0xFEAE}, // Reh
    {0x0632, 0xFEAF, 0xFEAF, 0xFEB0, 0xFEB0}, // Zain
    {0x0633, 0xFEB1, 0xFEB3, 0xFEB4, 0xFEB2}, // Seen
    {0x0634, 0xFEB5, 0xFEB7, 0xFEB8, 0xFEB6}, // Sheen
    {0x0635, 0xFEB9, 0xFEBB, 0xFEBC, 0xFEBA}, // Sad
    {0x0636, 0xFEBD, 0xFEBF, 0xFEC0, 0xFEBE}, // Dad
    {0x0637, 0xFEC1, 0xFEC3, 0xFEC4, 0xFEC2}, // Tah
    {0x0638, 0xFEC5, 0xFEC7, 0xFEC8, 0xFEC6}, // Zah
    {0x0639, 0xFEC9, 0xFECB, 0xFECC, 0xFECA}, // Ain
    {0x063A, 0xFECD, 0xFECF, 0xFED0, 0xFECE}, // Ghain
    {0x0641, 0xFED1, 0xFED3, 0xFED4, 0xFED2}, // Feh
    {0x0642, 0xFED5, 0xFED7, 0xFED8, 0xFED6}, // Qaf
    {0x0643, 0xFED9, 0xFEDB, 0xFEDC, 0xFEDA}, // Kaf
    {0x0644, 0xFEDD, 0xFEDF, 0xFEE0, 0xFEDE}, // Lam
    {0x0645, 0xFEE1, 0xFEE3, 0xFEE4, 0xFEE2}, // Meem
    {0x0646, 0xFEE5, 0xFEE7, 0xFEE8, 0xFEE6}, // Noon
    {0x0647, 0xFEE9, 0xFEEB, 0xFEEC, 0xFEEA}, // Heh
    {0x0648, 0xFEED, 0xFEED, 0xFEEE, 0xFEEE}, // Waw
    {0x0649, 0xFEEF, 0xFEEF, 0xFEF0, 0xFEF0}, // Alef Maksura
    {0x064A, 0xFEF1, 0xFEF3, 0xFEF4, 0xFEF2}, // Yeh
};

static bool canConnectLeft(uint16_t code)
{
    for (const auto &f : arabicForms)
    {
        if (f.code == code)
        {
            return f.initial != f.isolated;
        }
    }
    return false;
}

static bool canConnectRight(uint16_t code)
{
    for (const auto &f : arabicForms)
    {
        if (f.code == code)
        {
            return f.final != f.isolated;
        }
    }
    return false;
}

static std::string reshapeArabic(const std::string &text)
{
    std::vector<uint16_t> codes;
    for (size_t i = 0; i < text.length();)
    {
        unsigned char c = (unsigned char)text[i];
        uint16_t code = 0;
        if ((c & 0x80) == 0)
        {
            code = c;
            i++;
        }
        else if ((c & 0xE0) == 0xC0)
        {
            code = ((c & 0x1F) << 6) | (text[i + 1] & 0x3F);
            i += 2;
        }
        else if ((c & 0xF0) == 0xE0)
        {
            code = ((c & 0x0F) << 12) | ((text[i + 1] & 0x3F) << 6) | (text[i + 2] & 0x3F);
            i += 3;
        }
        else
            i++; // Skip 4-byte for now
        codes.push_back(code);
    }

    std::vector<uint16_t> reshaped;
    for (size_t i = 0; i < codes.size(); i++)
    {
        uint16_t code = codes[i];
        const ArabicForm *form = nullptr;
        for (const auto &f : arabicForms)
        {
            if (f.code == code)
            {
                form = &f;
                break;
            }
        }

        if (!form)
        {
            reshaped.push_back(code);
            continue;
        }

        bool prevConnects = (i > 0 && canConnectLeft(codes[i - 1]));
        bool nextConnects = (i < codes.size() - 1 && canConnectRight(codes[i + 1]));

        if (prevConnects && nextConnects)
            reshaped.push_back(form->medial);
        else if (prevConnects)
            reshaped.push_back(form->final);
        else if (nextConnects)
            reshaped.push_back(form->initial);
        else
            reshaped.push_back(form->isolated);
    }

    std::string result;
    for (uint16_t code : reshaped)
    {
        if (code < 0x80)
            result += (char)code;
        else if (code < 0x800)
        {
            result += (char)(0xC0 | (code >> 6));
            result += (char)(0x80 | (code & 0x3F));
        }
        else
        {
            result += (char)(0xE0 | (code >> 12));
            result += (char)(0x80 | ((code >> 6) & 0x3F));
            result += (char)(0x80 | (code & 0x3F));
        }
    }
    return result;
}

// Helper to check if a character is an ASCII digit
static bool isAsciiDigit(unsigned char c)
{
    return c >= '0' && c <= '9';
}

static std::string reverseHebrewWord(const std::string &text)
{
    // When reversing Hebrew text for RTL display, we need to keep digit sequences
    // in their original order (LTR). For example, "123" in Hebrew text should
    // remain "123", not become "321".

    std::string reversed;
    reversed.reserve(text.length());

    std::string currentDigitSequence;

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

        // Check if this is an ASCII digit (single byte)
        if (charLen == 1 && isAsciiDigit(c))
        {
            // Accumulate digits
            currentDigitSequence += text[i];
        }
        else
        {
            // If we have accumulated digits, insert them as a group first
            if (!currentDigitSequence.empty())
            {
                reversed.insert(0, currentDigitSequence);
                currentDigitSequence.clear();
            }
            // Insert the current character at the beginning (reversing)
            reversed.insert(0, text.substr(i, charLen));
        }
        i += charLen;
    }

    // Don't forget any trailing digits
    if (!currentDigitSequence.empty())
    {
        reversed.insert(0, currentDigitSequence);
    }

    return reversed;
}

static std::string swapMirroredChars(const std::string &text)
{
    std::string result = text;
    for (size_t i = 0; i < result.length(); ++i)
    {
        switch (result[i])
        {
        case '(': result[i] = ')'; break;
        case ')': result[i] = '('; break;
        case '[': result[i] = ']'; break;
        case ']': result[i] = '['; break;
        case '{': result[i] = '}'; break;
        case '}': result[i] = '{'; break;
        case '<': result[i] = '>'; break;
        case '>': result[i] = '<'; break;
        }
    }
    return result;
}

static std::string processTextForDisplay(const std::string &text)
{
    if (!isHebrew(text) && !isArabic(text))
        return text;

    // Split into words (space separated)
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
        bool isHeb = isHebrew(word);
        bool isAra = isArabic(word);
        std::string processedWord = word;

        // Fix for English words with trailing punctuation in RTL context
        // e.g. "English," -> ",English" so it appears as "English ," in RTL flow
        if (!isHeb && !isAra && processedWord.length() > 1)
        {
            char lastChar = processedWord.back();
            // Check if it's punctuation (but not a closing bracket which might be swapped later)
            if (ispunct((unsigned char)lastChar) && lastChar != ')' && lastChar != ']' && lastChar != '}')
            {
                processedWord.pop_back();
                processedWord.insert(0, 1, lastChar);
            }
        }

        if (isHeb || isAra)
        {
            // Replace HTML entities before reversing
            size_t pos = 0;
            while ((pos = processedWord.find("&quot;", pos)) != std::string::npos) { processedWord.replace(pos, 6, "\""); pos += 1; }
            pos = 0;
            while ((pos = processedWord.find("&amp;", pos)) != std::string::npos) { processedWord.replace(pos, 5, "&"); pos += 1; }
            pos = 0;
            while ((pos = processedWord.find("&lt;", pos)) != std::string::npos) { processedWord.replace(pos, 4, "<"); pos += 1; }
            pos = 0;
            while ((pos = processedWord.find("&gt;", pos)) != std::string::npos) { processedWord.replace(pos, 4, ">"); pos += 1; }
            pos = 0;
            while ((pos = processedWord.find("&apos;", pos)) != std::string::npos) { processedWord.replace(pos, 6, "'"); pos += 1; }
            pos = 0;
            while ((pos = processedWord.find("&#160;", pos)) != std::string::npos) { processedWord.replace(pos, 6, " "); pos += 1; }

            if (isAra)
            {
                processedWord = reshapeArabic(processedWord);
            }

            // Swap mirrored characters
            processedWord = swapMirroredChars(processedWord);

            result += reverseHebrewWord(processedWord);
        }
        else
        {
            // Also swap mirrored characters for English words in RTL context
            // e.g. "(English)" -> ")English(" which renders as "(English)" in RTL
            processedWord = swapMirroredChars(processedWord);
            result += processedWord;
        }
    }
    return result;
}

static bool detectRTLDocument(EpubLoader &loader, size_t sampleOffset)
{
    std::string lang = loader.getLanguage();
    if (lang.find("he") != std::string::npos || lang.find("HE") != std::string::npos ||
        lang.find("ar") != std::string::npos || lang.find("AR") != std::string::npos)
    {
        return true;
    }

    // Check first 5 chapters to decide if the book is Hebrew or Arabic
    int originalChapter = loader.getCurrentChapterIndex();
    int chaptersToCheck = std::min(5, loader.getTotalChapters());
    bool foundRTL = false;

    for (int i = 0; i < chaptersToCheck; i++)
    {
        if (loader.jumpToChapter(i))
        {
            // Check first 1000 chars of the chapter
            std::string sample = loader.getText(0, 1000);
            if (isHebrew(sample) || isArabic(sample))
            {
                foundRTL = true;
                break;
            }
        }
        if (foundRTL)
            break;
    }

    // Restore original chapter
    if (loader.getCurrentChapterIndex() != originalChapter)
    {
        loader.jumpToChapter(originalChapter);
    }

    return foundRTL;
}

static void enterDeepSleepWithTouchWake()
{
    // Use HAL for device-agnostic deep sleep
    deviceHAL.enterDeepSleepWithTouchWake();
}

void GUI::enterDeepSleepShutdown()
{
    ESP_LOGI(TAG, "Entering Deep Sleep Shutdown (Stage 2)...");
    deviceHAL.enterDeepSleepShutdown();
    // HAL handles everything and does not return
}

void GUI::init(bool isWakeFromSleep)
{
    justWokeUp = isWakeFromSleep;

    // Initialize background rendering
    epubMutex = xSemaphoreCreateMutex();
    renderQueue = xQueueCreate(5, sizeof(RenderRequest));

    // Use HAL for task core pinning
#ifdef CONFIG_EBOOK_S3_DUAL_CORE_OPTIMIZATION
    xTaskCreatePinnedToCore([](void *arg)
                            { static_cast<GUI *>(arg)->renderTaskLoop(); }, "RenderTask",
                            CONFIG_EBOOK_RENDER_TASK_STACK_SIZE, this, 1, &renderTaskHandle,
                            deviceHAL.getRenderTaskCore());
#else
    xTaskCreatePinnedToCore([](void *arg)
                            { static_cast<GUI *>(arg)->renderTaskLoop(); }, "RenderTask", 8192, this, 10, &renderTaskHandle, 0);
#endif

    // Initialize NVS
    // Already done in main, but safe to call again or skip
    if (!isWakeFromSleep)
    {
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
    ensureMathFontLoaded();

    // Initialize gesture detector
    gestureDetector.init(M5.Display.width(), M5.Display.height());

    // Initialize image handler
    ImageHandler::getInstance().init();

    // Initialize gesture detector
    gestureDetector.init(M5.Display.width(), M5.Display.height());

    // Play startup sound
    deviceHAL.playStartupSound();
    bookIndexReady = false;

    resetPageInfoCache();

    M5.Display.setTextColor(TFT_BLACK);

    // Create sprites for buffering
    // Use HAL to get optimal color depth:
    // - M5Paper: 2bpp (4 levels) to save RAM
    // - M5PaperS3: 4bpp (16 levels) for better grayscale quality
    const int bufferColorDepth = deviceHAL.getCanvasColorDepth();
    const int bufW = M5.Display.width();
    const int bufH = M5.Display.height();
    const size_t bytesPerSprite = ((size_t)bufW * bufH * bufferColorDepth + 7) / 8;
    const size_t freeBeforeDefault = heap_caps_get_free_size(MALLOC_CAP_DEFAULT);
    const size_t freeBeforeSPIRAM = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    const size_t largestDefault = heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT);
    const size_t largestSPIRAM = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);

    canvasNext.setColorDepth(bufferColorDepth);
    canvasPrev.setColorDepth(bufferColorDepth);
    canvasNext.setPsram(true);
    canvasPrev.setPsram(true);

    ESP_LOGI(TAG, "Device: %s - Creating buffer sprites (w=%d h=%d depth=%dbpp ~%u bytes each, PSRAM preferred). Free heap: default=%u SPIRAM=%u largest: default=%u SPIRAM=%u",
             deviceHAL.getDeviceName(), bufW, bufH, bufferColorDepth, (unsigned)bytesPerSprite,
             (unsigned)freeBeforeDefault, (unsigned)freeBeforeSPIRAM,
             (unsigned)largestDefault, (unsigned)largestSPIRAM);

    if (canvasNext.createSprite(bufW, bufH))
    {
        ESP_LOGI(TAG, "canvasNext created successfully. Free heap now: default=%u SPIRAM=%u largest: default=%u SPIRAM=%u",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_DEFAULT),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
    }
    else
    {
        ESP_LOGE(TAG, "Failed to create canvasNext. Free heap now: default=%u SPIRAM=%u largest: default=%u SPIRAM=%u (needed ~%u bytes)",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_DEFAULT),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM),
                 (unsigned)bytesPerSprite);
    }

    if (canvasPrev.createSprite(bufW, bufH))
    {
        ESP_LOGI(TAG, "canvasPrev created successfully. Free heap now: default=%u SPIRAM=%u largest: default=%u SPIRAM=%u",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_DEFAULT),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
    }
    else
    {
        ESP_LOGE(TAG, "Failed to create canvasPrev. Free heap now: default=%u SPIRAM=%u largest: default=%u SPIRAM=%u (needed ~%u bytes)",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_DEFAULT),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM),
                 (unsigned)bytesPerSprite);
    }

    nextCanvasValid = false;
    prevCanvasValid = false;

    lastActivityTime = (uint32_t)(esp_timer_get_time() / 1000);

    // Disable WiFi power save to prevent packet loss during uploads
    esp_wifi_set_ps(WIFI_PS_NONE);

    bool loadedLastBook = loadLastBook();

    if (backgroundIndexerTaskHandle == nullptr)
    {
#ifdef CONFIG_EBOOK_S3_DUAL_CORE_OPTIMIZATION
        xTaskCreatePinnedToCore([](void *arg)
                                { static_cast<GUI *>(arg)->backgroundIndexerTaskLoop(); }, "BgIndexer", 8192, this, 1, &backgroundIndexerTaskHandle,
                                1);
#else
        xTaskCreatePinnedToCore([](void *arg)
                                { static_cast<GUI *>(arg)->backgroundIndexerTaskLoop(); }, "BgIndexer", 8192, this, 1, &backgroundIndexerTaskHandle, 0);
#endif
    }

    if (loadedLastBook)
    {
        return;
    }

    M5.Display.fillScreen(TFT_WHITE);
    needsRedraw = true;
}

void GUI::renderTaskLoop()
{
    ESP_LOGI(TAG, "RenderTask loop started");
    RenderRequest req;
    while (true)
    {
        if (xQueueReceive(renderQueue, &req, portMAX_DELAY))
        {
            renderingInProgress = true;
            esp_task_wdt_add(NULL);
            esp_task_wdt_reset();
            ESP_LOGI(TAG, "RenderTask: Processing request offset=%zu isNext=%d", req.offset, req.isNext);

            // Clear abort flag before starting
            abortRender = false;

            // Note: drawPageContentAt handles locking internally for epubLoader access.
            // We don't lock here to avoid deadlock (since drawPageContentAt takes the lock)
            // and to allow the main thread to interrupt (abort) if needed.

            req.target->fillScreen(TFT_WHITE);
            size_t chars = drawPageContentAt(req.offset, true, req.target, &abortRender);

            if (!abortRender)
            {
                // Overlay status/footers on the offscreen target so we can push once without extra redraw.
                drawStatusBar(req.target);
                drawFooter(req.target, req.offset, chars);

                if (req.isNext)
                {
                    nextCanvasCharCount = chars;
                    nextCanvasOffset = req.offset;
                    nextCanvasValid = true;
                    ESP_LOGI(TAG, "RenderTask: Next page rendered. Offset=%zu Chars=%zu", nextCanvasOffset, nextCanvasCharCount);
                }
                else
                {
                    prevCanvasCharCount = chars;
                    prevCanvasOffset = req.offset;
                    prevCanvasValid = true;
                    ESP_LOGI(TAG, "RenderTask: Prev page rendered. Offset=%zu Chars=%zu", prevCanvasOffset, prevCanvasCharCount);
                }
            }
            else
            {
                ESP_LOGW(TAG, "RenderTask: Aborted");
            }
            esp_task_wdt_delete(NULL);
            renderingInProgress = false;
        }
        else
        {
            ESP_LOGW(TAG, "RenderTask: xQueueReceive returned false");
        }
    }
}

void GUI::metricsTaskLoop()
{
    esp_task_wdt_add(NULL);
    ESP_LOGI(TAG, "MetricsTask started for book %d", metricsTaskTargetBookId);
    int targetId = metricsTaskTargetBookId;

    // Initial check and setup
    int chapters = 0;
    if (xSemaphoreTake(epubMutex, portMAX_DELAY))
    {
        // Verify we are still on the same book
        if (currentBook.id != targetId)
        {
            xSemaphoreGive(epubMutex);
            ESP_LOGW(TAG, "MetricsTask: Book changed at start, aborting");
            metricsTaskHandle = nullptr;
            esp_task_wdt_delete(NULL);
            vTaskDelete(NULL);
            return;
        }
        chapters = epubLoader.getTotalChapters();
        xSemaphoreGive(epubMutex);
    }

    if (chapters <= 0)
    {
        ESP_LOGI(TAG, "MetricsTask: No chapters or empty book");
        metricsTaskHandle = nullptr;
        esp_task_wdt_delete(NULL);
        vTaskDelete(NULL);
        return;
    }

    std::vector<size_t> sums;
    sums.resize(chapters + 1, 0);
    size_t cumulative = 0;

    for (int i = 0; i < chapters; ++i)
    {
        esp_task_wdt_reset();
        // Check if book changed
        if (currentBook.id != targetId)
        {
            ESP_LOGW(TAG, "MetricsTask: Book changed during scan, aborting");
            metricsTaskHandle = nullptr;
            esp_task_wdt_delete(NULL);
            vTaskDelete(NULL);
            return;
        }

        size_t len = 0;
        if (xSemaphoreTake(epubMutex, portMAX_DELAY))
        {
            len = epubLoader.getChapterTextLength(i);
            xSemaphoreGive(epubMutex);
        }

        cumulative += len;
        sums[i + 1] = cumulative;

        // Yield to let other tasks run
        vTaskDelay(1);
    }

    // Update shared state
    if (xSemaphoreTake(epubMutex, portMAX_DELAY))
    {
        if (currentBook.id == targetId)
        {
            chapterPrefixSums = sums;
            totalBookChars = cumulative;
            bookMetricsComputed = cumulative > 0;
        }
        xSemaphoreGive(epubMutex);
    }

    // Save to disk for future use
    if (bookMetricsComputed && currentBook.id == targetId)
    {
        bookIndex.saveBookMetrics(currentBook.id, totalBookChars, chapterPrefixSums);
    }

    ESP_LOGI(TAG, "MetricsTask finished. Total chars: %zu", totalBookChars);

    // Force redraw to show updated footer
    needsRedraw = true;

    metricsTaskHandle = nullptr;
    esp_task_wdt_delete(NULL);
    vTaskDelete(NULL);
}

void GUI::backgroundIndexerTaskLoop()
{
    esp_task_wdt_add(NULL);
    while (needsRedraw || bookOpenInProgress)
    {
        esp_task_wdt_reset();
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    ESP_LOGI(TAG, "BgIndexer started");

    auto waitForBookOpen = [this]()
    {
        while (bookOpenInProgress || renderingInProgress)
        {
            esp_task_wdt_reset();
            vTaskDelay(pdMS_TO_TICKS(50));
        }
    };

    if (!bookIndexReady)
    {
        waitForBookOpen();
        ESP_LOGI(TAG, "BgIndexer: Loading book index...");
        bookIndex.init(false, [this](int current, int total, const char *msg) {
            if (currentState == AppState::LIBRARY || currentState == AppState::MAIN_MENU) {
                needsRedraw = true;
            }
            esp_task_wdt_reset();
        });
        bookIndexReady = true;

        if (pendingBookIndexSync && pendingBookId > 0)
        {
            bookIndex.updateProgress(pendingBookId, pendingBookChapter, pendingBookOffset);
            if (!pendingBookFont.empty())
            {
                bookIndex.setBookFont(pendingBookId, pendingBookFont, pendingBookFontSize);
            }
            pendingBookIndexSync = false;
        }

        if (lastBookId > 0)
        {
            BookEntry lastBook = bookIndex.getBook(lastBookId);
            if (lastBook.id != 0 && !lastBook.title.empty())
            {
                lastBookTitle = lastBook.title;
                if (lastBookTitle.length() > 25)
                {
                    size_t pos = 22;
                    while (pos > 0 && (lastBookTitle[pos] & 0xC0) == 0x80)
                        pos--;
                    lastBookTitle = lastBookTitle.substr(0, pos) + "...";
                }
            }
        }

        if (currentState == AppState::MAIN_MENU || currentState == AppState::LIBRARY || currentState == AppState::FAVORITES)
        {
            needsRedraw = true;
        }
    }

    // Wait a bit for system to settle and user to potentially start reading
    vTaskDelay(pdMS_TO_TICKS(3000));
    esp_task_wdt_reset();

    // Step 1: Scan for new books in background
    waitForBookOpen();
    ESP_LOGI(TAG, "BgIndexer: Scanning for new books...");
    indexingScanActive = true;
    indexingCurrent = 0;
    indexingTotal = 0;
    bool newBooksFound = bookIndex.scanForNewBooks([this](int current, int total, const char *msg)
                                                   {
        // Optional: update a status bar or log
        // ESP_LOGI(TAG, "Scan progress: %d/%d - %s", current, total, msg);
        indexingCurrent = current;
        indexingTotal = total;
        
        // Throttle redraws to avoid UI freeze/flicker
        static uint32_t lastRedraw = 0;
        uint32_t now = xTaskGetTickCount();
        if (currentState == AppState::MAIN_MENU && (now - lastRedraw > pdMS_TO_TICKS(500)))
        {
            needsRedraw = true;
            lastRedraw = now;
        }
        esp_task_wdt_reset(); },
        [this]() {
            return bookOpenInProgress || renderingInProgress;
        });
    indexingScanActive = false;

    if (newBooksFound)
    {
        ESP_LOGI(TAG, "BgIndexer: New books found, refreshing list if needed");
        // If we are in library view, we might want to refresh, but for now just let the user see them next time
    }
    esp_task_wdt_reset();

    // Step 2: Calculate metrics for books that need it
    auto books = bookIndex.getBooks();
    bool indexNeedsSave = false;
    int updatesCount = 0;
    
    indexingProcessingActive = true;
    indexingTotal = books.size();
    int processedCount = 0;

    uint32_t lastRedrawTime = 0;

    for (const auto &book : books)
    {
        processedCount++;
        indexingCurrent = processedCount;
        
        // Throttle redraws
        uint32_t now = xTaskGetTickCount();
        if (currentState == AppState::MAIN_MENU && (now - lastRedrawTime > pdMS_TO_TICKS(1000))) {
            needsRedraw = true;
            lastRedrawTime = now;
        }

        if (bookOpenInProgress)
        {
            waitForBookOpen();
        }
        
        // Yield to UI task
        vTaskDelay(pdMS_TO_TICKS(10));

        if (!book.hasMetrics)
        {
            // Check if this book is currently being read (to avoid double calculation)
            if (currentState == AppState::READER && currentBook.id == book.id)
            {
                continue;
            }

            // Skip if book is marked as failed
            if (book.isFailed)
            {
                ESP_LOGI(TAG, "BgIndexer: Skipping failed book %d (%s)", book.id, book.title.c_str());
                continue;
            }

            // Optimization: Check if metrics file already exists on disk (e.g. from previous firmware version or crash)
            // This avoids re-calculating if we just lost the index flag but file is there.
            // We do this check here in the background task, not on main thread.
            size_t dummyTotal;
            std::vector<size_t> dummyOffsets;
            if (bookIndex.loadBookMetrics(book.id, dummyTotal, dummyOffsets))
            {
                ESP_LOGI(TAG, "BgIndexer: Found existing metrics for book %d (%s), updating index flag only", book.id, book.title.c_str());
                // Update in-memory flag ONLY. Do NOT save index yet.
                bookIndex.updateBookMetricsFlag(book.id, true);
                indexNeedsSave = true;
                updatesCount++;
                
                // Save periodically to avoid losing progress on crash/reboot
                if (updatesCount >= 20) {
                    ESP_LOGI(TAG, "BgIndexer: Saving intermediate index...");
                    bookIndex.save();
                    updatesCount = 0;
                    indexNeedsSave = false;
                }
                continue;
            }

            ESP_LOGI(TAG, "BgIndexer: Processing new book %d (%s)", book.id, book.title.c_str());

            if (bookOpenInProgress)
            {
                waitForBookOpen();
            }

            uint32_t bookStartTime = (uint32_t)(esp_timer_get_time() / 1000);
            EpubLoader localLoader;
            // Note: We don't take epubMutex here because we are using a separate loader instance
            // and separate file handle. SPIFFS is thread-safe.

            if (localLoader.load(book.path.c_str(), -1, false))
            {
                int chapters = localLoader.getTotalChapters();
                std::vector<size_t> sums;
                sums.resize(chapters + 1, 0);
                size_t cumulative = 0;

                for (int i = 0; i < chapters; ++i)
                {
                    esp_task_wdt_reset();
                    // Yield frequently to keep UI responsive
                    vTaskDelay(pdMS_TO_TICKS(5));

                    if (i % 10 == 0) {
                        ESP_LOGI(TAG, "BgIndexer: Processing chapter %d/%d", i, chapters);
                    }

                    // Timeout check
                    if ((uint32_t)(esp_timer_get_time() / 1000) - bookStartTime > 10 * 60 * 1000) {
                        ESP_LOGW(TAG, "BgIndexer: Book processing timed out, skipping");
                        localLoader.close();
                        goto next_book;
                    }

                    // Check for web activity and pause if needed to avoid starving the upload handler
                    uint32_t lastHttp = WebServer::getLastActivityTime();
                    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
                    if (lastHttp > 0 && (now > lastHttp) && (now - lastHttp < 5000))
                    {
                        ESP_LOGI(TAG, "BgIndexer: Pausing for web activity...");
                        while (lastHttp > 0 && (now > lastHttp) && (now - lastHttp < 5000))
                        {
                            esp_task_wdt_reset();
                            vTaskDelay(pdMS_TO_TICKS(1000));
                            lastHttp = WebServer::getLastActivityTime();
                            now = (uint32_t)(esp_timer_get_time() / 1000);
                        }
                        ESP_LOGI(TAG, "BgIndexer: Resuming...");
                    }

                    // If user started reading this specific book, abort
                    if (currentState == AppState::READER && currentBook.id == book.id)
                    {
                        ESP_LOGI(TAG, "BgIndexer: Aborting book %d because user opened it", book.id);
                        goto next_book;
                    }

                    if (bookOpenInProgress)
                    {
                        ESP_LOGI(TAG, "BgIndexer: Pausing for book open...");
                        localLoader.close();
                        waitForBookOpen();
                        goto next_book;
                    }

                    if (renderingInProgress)
                    {
                        waitForBookOpen();
                        // Re-check if we should abort in case state changed while waiting
                        if (currentState == AppState::READER && currentBook.id == book.id)
                        {
                             ESP_LOGI(TAG, "BgIndexer: Aborting book %d because user opened it (after render wait)", book.id);
                             localLoader.close();
                             goto next_book;
                        }
                    }

                    size_t len = localLoader.getChapterTextLength(i);
                    cumulative += len;
                    sums[i + 1] = cumulative;
                }

                // Save metrics but NOT the index file yet (pass false)
                bookIndex.saveBookMetrics(book.id, cumulative, sums, false);
                indexNeedsSave = true;
                updatesCount++;
                
                // Save periodically
                if (updatesCount >= 5) { // Save more frequently for expensive operations
                    ESP_LOGI(TAG, "BgIndexer: Saving intermediate index...");
                    bookIndex.save();
                    updatesCount = 0;
                    indexNeedsSave = false;
                }
                
                localLoader.close();
            }
            else
            {
                ESP_LOGW(TAG, "BgIndexer: Failed to load book %d (%s), marking as failed", book.id, book.path.c_str());
                bookIndex.markAsFailed(book.id);
                indexNeedsSave = true;
            }

        next_book:
            vTaskDelay(pdMS_TO_TICKS(100)); // Rest between books

        }
    }

    if (indexNeedsSave) {
        ESP_LOGI(TAG, "BgIndexer: Saving updated index with found metrics...");
        bookIndex.save();
    }

    indexingProcessingActive = false;
    if (currentState == AppState::MAIN_MENU) needsRedraw = true;

    ESP_LOGI(TAG, "BgIndexer finished");
    backgroundIndexerTaskHandle = nullptr;
    esp_task_wdt_delete(NULL);
    vTaskDelete(NULL);
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

// External flag for button-triggered sleep
extern volatile bool buttonSleepRequested;

void GUI::setShowImages(bool enabled)
{
    if (showImages == enabled)
        return;
    showImages = enabled;
    saveSettings();
    needsRedraw = true;
}

void GUI::update()
{
    if (currentState == AppState::MUSIC_COMPOSER)
    {
        ComposerUI::getInstance().update();
    }

    // Check for button-triggered sleep request
    if (buttonSleepRequested)
    {
        // Force sleep even if indexing is active, but try to stop it gracefully first
        if (backgroundIndexerTaskHandle != nullptr)
        {
            ESP_LOGI(TAG, "Button sleep requested - stopping indexing and sleeping");
            // We can't easily stop the task if it's stuck in a loop, but we can just sleep.
            // The deep sleep will kill all tasks.
            // Ideally we should signal it to stop, but for now let's just sleep.
        }
        
        buttonSleepRequested = false;
        ESP_LOGI(TAG, "Button sleep requested - entering sleep mode");
        goToSleep();
        lastActivityTime = (uint32_t)(esp_timer_get_time() / 1000);
        return;
    }

    // Update gesture detector
    GestureEvent gesture = gestureDetector.update();
    if (gesture.type != GestureType::NONE)
    {
        handleGesture(gesture);
        // If a gesture occurred, cancel any pending click to avoid double-action
        clickPending = false;
    }

    handleTouch();

    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    bool indexingActive = (backgroundIndexerTaskHandle != nullptr);

    // Check web server activity - if there's been recent HTTP activity, don't sleep
    uint32_t lastHttpActivity = webServer.getLastActivityTime();
    bool recentHttpActivity = (lastHttpActivity > 0) && (now - lastHttpActivity < 30 * 1000); // 30 second grace period

    if (recentHttpActivity || indexingActive)
    {
        lastActivityTime = now; // Reset idle timer if web is active
    }

    // Unified Sleep and WebServer Logic
    uint32_t idleTimeout = lightSleepMinutes * 60 * 1000; // Use user setting
    
    if (!indexingActive && now - lastActivityTime > idleTimeout && !recentHttpActivity)
    {
        if (currentState == AppState::LIBRARY && webServerEnabled)
        {
            ESP_LOGI(TAG, "Library idle timeout: Stopping WebServer");
            setWebServerEnabled(false);
        }
        goToSleep();
        // After waking up, update lastActivityTime so we don't loop immediately
        lastActivityTime = (uint32_t)(esp_timer_get_time() / 1000);
    }
    else if (currentState == AppState::LIBRARY && !webServerEnabled)
    {
        // If we are in library and not idle yet, ensure web server is on
        setWebServerEnabled(true);
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
        case AppState::MAIN_MENU:
            drawMainMenu();
            break;
        case AppState::LIBRARY:
            drawLibrary(false);
            break;
        case AppState::FAVORITES:
            drawLibrary(true); // Favorites only
            break;
        case AppState::READER:
            drawReader();
            break;
        case AppState::WIFI_CONFIG:
            drawWifiConfig();
            break;
        case AppState::SETTINGS:
            drawStandaloneSettings(); // Full-page standalone settings
            break;
        case AppState::BOOK_SETTINGS:
            drawSettings(); // In-book overlay settings
            break;
        case AppState::WIFI_SCAN:
            drawWifiScan();
            break;
        case AppState::WIFI_PASSWORD:
            drawWifiPassword();
            break;
        case AppState::IMAGE_VIEWER:
            drawImageViewer();
            break;
        case AppState::GAMES_MENU:
            drawGamesMenu();
            break;
        case AppState::GAME_PLAYING:
            drawGame();
            break;
        case AppState::MUSIC_COMPOSER:
            drawMusicComposer();
            break;
        case AppState::CHAPTER_MENU:
            drawChapterMenu();
            break;
        case AppState::FONT_SELECTION:
            drawFontSelection();
            break;
        }
        needsRedraw = false;
    }
}

void GUI::drawStatusBar(LovyanGFX *target)
{
    LovyanGFX *gfx = target ? target : (LovyanGFX *)&M5.Display;

    // Use system font for status bar to ensure it fits
    gfx->setFont(&lgfx::v1::fonts::Font2);
    gfx->setTextSize(1.6f); // Larger, easier to read

    // White background, black text, black separator line
    gfx->fillRect(0, 0, gfx->width(), STATUS_BAR_HEIGHT, TFT_WHITE);
    gfx->drawFastHLine(0, STATUS_BAR_HEIGHT - 1, gfx->width(), TFT_BLACK);
    gfx->setTextColor(TFT_BLACK, TFT_WHITE);

    const int centerY = STATUS_BAR_HEIGHT / 2;

    // Time - convert RTC (UTC) to local time using timezone
    gfx->setTextDatum(textdatum_t::middle_left);
    auto dt = M5.Rtc.getDateTime();
    char buf[64];

    // Convert RTC datetime to time_t, then to local time
    struct tm rtc_tm;
    memset(&rtc_tm, 0, sizeof(rtc_tm));
    rtc_tm.tm_year = dt.date.year - 1900;
    rtc_tm.tm_mon = dt.date.month - 1;
    rtc_tm.tm_mday = dt.date.date;
    rtc_tm.tm_hour = dt.time.hours;
    rtc_tm.tm_min = dt.time.minutes;
    rtc_tm.tm_sec = dt.time.seconds;

    // mktime treats input as local time, so we temporarily set TZ to UTC
    // to convert UTC RTC time to time_t, then restore TZ for localtime
    char *old_tz = getenv("TZ");
    std::string saved_tz = old_tz ? old_tz : "";
    setenv("TZ", "UTC0", 1);
    tzset();
    time_t utc_time = mktime(&rtc_tm);
    if (!saved_tz.empty())
    {
        setenv("TZ", saved_tz.c_str(), 1);
    }
    else
    {
        unsetenv("TZ");
    }
    tzset();

    // localtime converts time_t to local time using TZ env var
    struct tm *local_tm = localtime(&utc_time);

    snprintf(buf, sizeof(buf), "%02d:%02d", local_tm->tm_hour, local_tm->tm_min);
    gfx->drawString(buf, 10, centerY);
    int timeWidth = gfx->textWidth(buf) + 15;

    // Battery (Right aligned)
    gfx->setTextDatum(textdatum_t::middle_right);
    int bat = M5.Power.getBatteryLevel();
    snprintf(buf, sizeof(buf), "%d%%", bat);
    int batteryX = gfx->width() - 10;
    int batteryTextWidth = gfx->textWidth(buf);
    gfx->drawString(buf, batteryX, centerY);

    // Wifi (Left of Battery)
    if (wifiConnected)
    {
        snprintf(buf, sizeof(buf), "WiFi %d dBm", wifiRssi);
    }
    else
    {
        snprintf(buf, sizeof(buf), "No WiFi");
    }
    int wifiX = batteryX - batteryTextWidth - 12;
    int wifiTextWidth = gfx->textWidth(buf);
    gfx->drawString(buf, wifiX, centerY);

    // Book title (center, if in reader mode)
    if (currentState == AppState::READER && !currentBook.title.empty())
    {
        gfx->setTextDatum(textdatum_t::middle_center);
        int availableWidth = wifiX - wifiTextWidth - timeWidth - 20;
        int bookTitleX = timeWidth + availableWidth / 2;

        // Truncate title to fit
        std::string title = currentBook.title;
        gfx->setTextSize(1.0f); // Smaller for title to fit more text

        bool isHeb = isHebrew(title);
        bool isAra = isArabic(title);
        bool swapped = false;
        if (isHeb || isAra)
        {
            if (isHeb)
                ensureHebrewFontLoaded();
            else
                ensureArabicFontLoaded();

            const uint8_t *fontPtr = isHeb ? fontDataHebrew.data() : fontDataArabic.data();
            if (fontPtr && !(isHeb ? fontDataHebrew.empty() : fontDataArabic.empty()))
            {
                gfx->loadFont(fontPtr);
                swapped = true;
            }
        }

        bool truncated = false;
        while (!title.empty() && gfx->textWidth(title.c_str()) > availableWidth - 10)
        {
            size_t pos = title.length();
            if (pos > 0)
            {
                do
                {
                    pos--;
                } while (pos > 0 && (title[pos] & 0xC0) == 0x80);
                title = title.substr(0, pos);
                truncated = true;
            }
        }
        if (truncated && title.length() > 3)
        {
            title = title.substr(0, title.length() - 3) + "...";
        }

        if (isHeb || isAra)
        {
            title = processTextForDisplay(title);
        }

        gfx->setTextColor(TFT_DARKGREY, TFT_WHITE);
        gfx->drawString(title.c_str(), bookTitleX, centerY);

        if (swapped)
        {
            if (currentFont != "Default" && !fontData.empty())
            {
                gfx->loadFont(fontData.data());
            }
            else
            {
                gfx->unloadFont();
            }
        }

        gfx->setTextSize(1.6f);
        gfx->setTextColor(TFT_BLACK, TFT_WHITE);
    }

    // Restore font
    if (currentFont == "Default")
    {
        gfx->setFont(&lgfx::v1::fonts::Font2);
    }
    else
    {
        if (currentFont == "Hebrew" && !fontDataHebrew.empty())
        {
            gfx->loadFont(fontDataHebrew.data());
        }
        else if (!fontData.empty())
        {
            gfx->loadFont(fontData.data());
        }
        else
        {
            gfx->setFont(&lgfx::v1::fonts::Font2);
        }
    }

    // Reset datum to default for other drawing functions
    gfx->setTextDatum(textdatum_t::top_left);
    gfx->setTextColor(TFT_BLACK); // Transparent background for the rest of the UI
}

void GUI::drawFooter(LovyanGFX *target, size_t pageOffset, size_t charsOnPage)
{
    LovyanGFX *gfx = target ? target : (LovyanGFX *)&M5.Display;
    gfx->setTextColor(TFT_BLACK, TFT_WHITE);

    const int footerY = gfx->height() - 50;
    gfx->setTextSize(1.0f); // Reduced font size

    int chIdx = 0;
    size_t chSize = 1;
    size_t charsBeforeChapter = 0;
    bool hasBookMetrics = bookMetricsComputed && !chapterPrefixSums.empty();
    if (xSemaphoreTake(epubMutex, portMAX_DELAY))
    {
        chIdx = epubLoader.getCurrentChapterIndex();
        chSize = epubLoader.getChapterSize();
        if (hasBookMetrics && (size_t)chIdx < chapterPrefixSums.size())
        {
            charsBeforeChapter = chapterPrefixSums[chIdx];
        }
        xSemaphoreGive(epubMutex);
    }
    if (chSize == 0)
        chSize = 1;

    // Page numbers based on the page we just rendered
    size_t pageChars = charsOnPage > 0 ? charsOnPage : lastPageChars;
    if (pageChars == 0)
    {
        PageInfo p = calculatePageInfo();
        (void)p;
        pageChars = lastPageChars;
    }
    if (pageChars == 0)
        pageChars = 1;

    float chapterPercent = (float)pageOffset / chSize * 100.0f;
    if (chapterPercent > 100.0f)
        chapterPercent = 100.0f;

    size_t effectiveTotalChars = hasBookMetrics && totalBookChars > 0 ? totalBookChars : chSize;
    size_t bookOffset = charsBeforeChapter + pageOffset;
    if (effectiveTotalChars == 0)
        effectiveTotalChars = 1;
    if (bookOffset > effectiveTotalChars)
        bookOffset = effectiveTotalChars;

    float bookPercent = (float)bookOffset * 100.0f / (float)effectiveTotalChars;
    if (bookPercent > 100.0f)
        bookPercent = 100.0f;

    // Use fixed page size for stable page numbering (Issue 2)
    const size_t FIXED_PAGE_SIZE = 1024;
    size_t bookPageTotalSize = (effectiveTotalChars + FIXED_PAGE_SIZE - 1) / FIXED_PAGE_SIZE;
    int bookPageTotal = bookPageTotalSize > 0 ? (int)bookPageTotalSize : 1;
    int bookPageCurrent = (int)(bookOffset / FIXED_PAGE_SIZE) + 1;
    if (bookPageCurrent > bookPageTotal)
        bookPageCurrent = bookPageTotal;

    char totalPercentBuf[24];
    snprintf(totalPercentBuf, sizeof(totalPercentBuf), "%.1f%%", bookPercent);
    gfx->setTextDatum(textdatum_t::top_left);
    gfx->drawString(totalPercentBuf, 16, footerY);

    char chapterBuf[32];
    snprintf(chapterBuf, sizeof(chapterBuf), "Ch %d - %.1f%%", chIdx + 1, chapterPercent);
    gfx->setTextDatum(textdatum_t::top_center);
    gfx->drawString(chapterBuf, gfx->width() / 2, footerY);

    char pageBuf[24];
    snprintf(pageBuf, sizeof(pageBuf), "Pg %d/%d", bookPageCurrent, bookPageTotal);
    gfx->setTextDatum(textdatum_t::top_right);
    gfx->drawString(pageBuf, gfx->width() - 16, footerY);
    gfx->setTextDatum(textdatum_t::top_left);

    // Restore primary font size
    gfx->setTextSize(fontSize);
    gfx->setTextColor(TFT_BLACK, TFT_WHITE);
}

void GUI::drawSearchBar(LovyanGFX *target)
{
    int screenW = target->width();
    int padding = 16;
    int starBtnSize = 44;
    int searchBtnW = 80;
    int clearBtnSize = 28; // Small clear button
    int searchBoxX = padding;
    int searchBoxW = screenW - padding * 3 - starBtnSize - searchBtnW - 10;
    int searchBoxH = SEARCH_BAR_HEIGHT - 6;
    int searchBtnX = searchBoxX + searchBoxW + 8;
    int starBtnX = searchBtnX + searchBtnW + 8;

    // Use system font for search bar (independent of reader font)
    target->setFont(&lgfx::v1::fonts::Font2);

    // Draw search textbox
    target->drawRect(searchBoxX, SEARCH_BAR_Y, searchBoxW, searchBoxH, TFT_BLACK);
    target->fillRect(searchBoxX + 1, SEARCH_BAR_Y + 1, searchBoxW - 2, searchBoxH - 2, TFT_WHITE);

    // Draw search text or placeholder - use fixed font size
    target->setTextSize(1.4f);
    target->setTextDatum(textdatum_t::middle_left);
    int textY = SEARCH_BAR_Y + searchBoxH / 2;

    // Calculate clear button position (inside text box, right side)
    int clearBtnX = searchBoxX + searchBoxW - clearBtnSize - 4;
    int clearBtnY = SEARCH_BAR_Y + (searchBoxH - clearBtnSize) / 2;
    int maxTextW = searchBoxW - 20 - (searchQuery.empty() ? 0 : clearBtnSize + 4);

    if (searchQuery.empty() && !showKeyboard)
    {
        target->setTextColor(TFT_DARKGREY, TFT_WHITE);
        drawStringMixed("Search books...", searchBoxX + 10, textY, (M5Canvas *)target, 1.4f);
    }
    else
    {
        target->setTextColor(TFT_BLACK, TFT_WHITE);
        // Truncate if too long
        std::string displayText = searchQuery;

        bool isHeb = isHebrew(displayText);
        bool swapped = false;
        if (isHeb)
        {
            ensureHebrewFontLoaded();
            if (!fontDataHebrew.empty())
            {
                target->loadFont(fontDataHebrew.data());
                swapped = true;
            }
            displayText = processTextForDisplay(displayText);
        }

        while (!displayText.empty() && target->textWidth(displayText.c_str()) > maxTextW)
        {
            size_t charLen = 1;
            unsigned char c = (unsigned char)displayText[0];
            if ((c & 0x80) == 0)
                charLen = 1;
            else if ((c & 0xE0) == 0xC0)
                charLen = 2;
            else if ((c & 0xF0) == 0xE0)
                charLen = 3;
            else
                charLen = 4;
            displayText = displayText.substr(charLen);
        }
        target->drawString(displayText.c_str(), searchBoxX + 10, textY);

        // Draw cursor if keyboard is showing
        if (showKeyboard)
        {
            int cursorX = searchBoxX + 10 + target->textWidth(displayText.c_str());
            target->drawLine(cursorX, SEARCH_BAR_Y + 8, cursorX, SEARCH_BAR_Y + searchBoxH - 8, TFT_BLACK);
        }

        if (swapped)
        {
            target->unloadFont();
            target->setFont(&lgfx::v1::fonts::Font2);
            target->setTextSize(1.4f);
        }
    }

    // Draw clear (x) button if there's text
    if (!searchQuery.empty())
    {
        target->fillRect(clearBtnX, clearBtnY, clearBtnSize, clearBtnSize, TFT_LIGHTGREY);
        target->drawRect(clearBtnX, clearBtnY, clearBtnSize, clearBtnSize, TFT_DARKGREY);
        target->setTextColor(TFT_BLACK, TFT_LIGHTGREY);
        target->setTextDatum(textdatum_t::middle_center);
        target->setTextSize(1.2f);
        target->drawString("x", clearBtnX + clearBtnSize / 2, clearBtnY + clearBtnSize / 2);
        target->setTextSize(1.4f);
    }

    // Draw Search button
    target->fillRect(searchBtnX, SEARCH_BAR_Y, searchBtnW, searchBoxH, TFT_LIGHTGREY);
    target->drawRect(searchBtnX, SEARCH_BAR_Y, searchBtnW, searchBoxH, TFT_BLACK);
    target->setTextColor(TFT_BLACK, TFT_LIGHTGREY);
    target->setTextDatum(textdatum_t::middle_center);
    target->drawString("Search", searchBtnX + searchBtnW / 2, textY);

    // Draw star (favorites) toggle button
    uint16_t starFill = showFavoritesOnly ? TFT_YELLOW : TFT_WHITE;
    uint16_t starBorder = TFT_BLACK;
    target->fillRect(starBtnX, SEARCH_BAR_Y, starBtnSize, searchBoxH, starFill);
    target->drawRect(starBtnX, SEARCH_BAR_Y, starBtnSize, searchBoxH, starBorder);

    // Draw star symbol (★)
    target->setTextColor(showFavoritesOnly ? TFT_BLACK : TFT_DARKGREY, starFill);
    target->setTextDatum(textdatum_t::middle_center);
    target->setTextSize(1.8f);
    target->drawString("+", starBtnX + starBtnSize / 2, textY);

    target->setTextDatum(textdatum_t::top_left);
}

void GUI::drawKeyboard(LovyanGFX *target)
{
    int screenW = target->width();
    int screenH = target->height();

    // Hebrew keyboard layout (standard Hebrew QWERTY mapping)
    const char *hebrewRows[] = {
        "1234567890",
        // Hebrew letters mapped to QWERTY positions
        "/'קראטוןםפ", // QWERTYUIOP -> / ' ק ר א ט ו ן ם פ
        "שדגכעיחלך",  // ASDFGHJKL -> ש ד ג כ ע י ח ל ך
        "זסבהנמצ",    // ZXCVBNM -> ז ס ב ה נ מ צ
    };

    // English keyboard layout
    const char *englishRows[] = {
        "1234567890",
        "QWERTYUIOP",
        "ASDFGHJKL",
        "ZXCVBNM",
    };

    const char **rows = keyboardHebrew ? hebrewRows : englishRows;
    int numRows = 4;

    // Reduced sizes to fit on screen
    int keyboardH = 200;
    int keyboardY = screenH - keyboardH;
    int keyW = screenW / 12;
    int keyH = 36;
    int padding = 3;

    // Draw keyboard background
    target->fillRect(0, keyboardY, screenW, keyboardH, TFT_LIGHTGREY);
    target->drawLine(0, keyboardY, screenW, keyboardY, TFT_DARKGREY);

    // Use system font for keyboard
    target->setFont(&lgfx::v1::fonts::Font2);
    target->setTextSize(1.3f);
    target->setTextDatum(textdatum_t::middle_center);

    int y = keyboardY + 6;
    for (int r = 0; r < numRows; r++)
    {
        const char *row = rows[r];

        // Count UTF-8 characters properly for Hebrew
        int len = 0;
        for (const char *p = row; *p;)
        {
            unsigned char c = *p;
            if ((c & 0x80) == 0)
            {
                p++;
                len++;
            }
            else if ((c & 0xE0) == 0xC0)
            {
                p += 2;
                len++;
            }
            else if ((c & 0xF0) == 0xE0)
            {
                p += 3;
                len++;
            }
            else
            {
                p += 4;
                len++;
            }
        }

        int rowW = len * keyW + (len - 1) * padding;
        int startX = (screenW - rowW) / 2;

        // Iterate UTF-8 characters
        const char *p = row;
        int c = 0;
        while (*p)
        {
            unsigned char ch = *p;
            int charLen = 1;
            if ((ch & 0x80) == 0)
                charLen = 1;
            else if ((ch & 0xE0) == 0xC0)
                charLen = 2;
            else if ((ch & 0xF0) == 0xE0)
                charLen = 3;
            else
                charLen = 4;

            int x = startX + c * (keyW + padding);
            target->fillRect(x, y, keyW, keyH, TFT_WHITE);
            target->drawRect(x, y, keyW, keyH, TFT_BLACK);

            char chStr[5] = {0};
            strncpy(chStr, p, charLen);
            target->setTextColor(TFT_BLACK, TFT_WHITE);

            if (isHebrew(chStr))
            {
                ensureHebrewFontLoaded();
                if (!fontDataHebrew.empty())
                {
                    target->loadFont(fontDataHebrew.data());
                    target->setTextSize(1.1f); // Slightly smaller for Hebrew keys
                    target->drawString(chStr, x + keyW / 2, y + keyH / 2);
                    target->unloadFont();
                    target->setFont(&lgfx::v1::fonts::Font2);
                    target->setTextSize(1.3f);
                }
                else
                {
                    target->drawString(chStr, x + keyW / 2, y + keyH / 2);
                }
            }
            else
            {
                target->drawString(chStr, x + keyW / 2, y + keyH / 2);
            }

            p += charLen;
            c++;
        }
        y += keyH + padding;
    }

    // Bottom row: Lang, Backspace, Space, Done
    y = keyboardY + 6 + 4 * (keyH + padding);
    int langW = 55;
    int bsW = 55;
    int doneW = 60;
    int spaceW = screenW - langW - bsW - doneW - padding * 5;

    // Language toggle (Hebrew/English)
    int langX = padding;
    uint16_t langFill = keyboardHebrew ? TFT_YELLOW : TFT_WHITE;
    target->fillRect(langX, y, langW, keyH, langFill);
    target->drawRect(langX, y, langW, keyH, TFT_BLACK);
    target->setTextColor(TFT_BLACK, langFill);
    // Pass false for respectUserFont to ensure we restore to Font2
    drawStringMixed(keyboardHebrew ? "EN" : "עב", langX + langW / 2, y + keyH / 2, (M5Canvas *)target, keyboardHebrew ? 1.3f : 1.1f, false, false);

    // Backspace
    int bsX = langX + langW + padding;
    target->fillRect(bsX, y, bsW, keyH, TFT_WHITE);
    target->drawRect(bsX, y, bsW, keyH, TFT_BLACK);
    target->setTextColor(TFT_BLACK, TFT_WHITE);
    target->drawString("<-", bsX + bsW / 2, y + keyH / 2);

    // Space
    int spaceX = bsX + bsW + padding;
    target->fillRect(spaceX, y, spaceW, keyH, TFT_WHITE);
    target->drawRect(spaceX, y, spaceW, keyH, TFT_BLACK);
    target->drawString("Space", spaceX + spaceW / 2, y + keyH / 2);

    // Done
    int doneX = spaceX + spaceW + padding;
    target->fillRect(doneX, y, doneW, keyH, TFT_DARKGREY);
    target->drawRect(doneX, y, doneW, keyH, TFT_BLACK);
    target->setTextColor(TFT_WHITE, TFT_DARKGREY);
    target->drawString("Done", doneX + doneW / 2, y + keyH / 2);

    target->setTextDatum(textdatum_t::top_left);
}

void GUI::drawLibrary(bool favoritesOnly)
{
    abortRender = true; // Stop any background rendering
    // Use canvasNext as a buffer if available to speed up drawing
    M5Canvas *sprite = (canvasNext.width() > 0) ? &canvasNext : nullptr;
    LovyanGFX *target = sprite ? (LovyanGFX *)sprite : (LovyanGFX *)&M5.Display;

    target->fillScreen(TFT_WHITE);
    drawStatusBar(target);

    // Draw search bar
    drawSearchBar(target);

    // Use system font for library (consistent size, independent of reader settings)
    target->setFont(&lgfx::v1::fonts::Font2);
    target->setTextColor(TFT_BLACK);
    const float itemSize = 2.0f;
    target->setTextSize(itemSize);

    if (!bookIndexReady)
    {
        target->setCursor(20, LIBRARY_LIST_START_Y);
        target->println("Loading books...");
        if (sprite)
        {
            sprite->pushSprite(&M5.Display, 0, 0);
        }
        M5.Display.display();
        return;
    }

    // Get filtered books based on search and favorites
    // Use favoritesOnly parameter if this is FAVORITES state, otherwise use showFavoritesOnly toggle
    bool filterFavorites = favoritesOnly || showFavoritesOnly;
    auto books = bookIndex.getFilteredBooks(searchQuery, filterFavorites);

    // Paging logic
    int availableHeight = target->height() - LIBRARY_LIST_START_Y - 60; // 60 for footer
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
    target->setTextSize(itemSize);

    // Use startWrite to batch operations if supported
    target->startWrite();

    for (int i = startIdx; i < endIdx; ++i)
    {
        // Ensure font is reset to system font for each item to prevent drift
        target->setFont(&lgfx::v1::fonts ::Font2); // Font2
        target->setTextSize(itemSize);

        const auto &book = books[i];
        // Truncate display title if too long (UTF-8 aware)
        std::string displayTitle = book.title;
        if (displayTitle.length() > 45)
        {
            size_t pos = 42;
            while (pos > 0 && (displayTitle[pos] & 0xC0) == 0x80)
                pos--;
            displayTitle = displayTitle.substr(0, pos) + "...";
        }

        // Draw bullet
        target->setCursor(20, y);
        target->print("- ");

        // Draw title with mixed font support
        int titleX = 20 + target->textWidth("- ");
        // Note: drawStringMixed takes M5Canvas*, so we pass sprite (which might be null)
        // If sprite is null, drawStringMixed uses M5.Display.
        // Pass false for respectUserFont to ensure we restore to Font2 (Library font) instead of Reader font
        drawStringMixed(displayTitle, titleX, y, sprite, itemSize, false, false);

        y += LIBRARY_LINE_HEIGHT;
    }

    target->endWrite();

    // Draw Paging Controls
    if (totalPages > 1)
    {
        int footerY = target->height() - 60;
        target->setTextSize(1.5f);

        if (libraryPage > 0)
        {
            target->fillRect(20, footerY, 100, 40, TFT_LIGHTGREY);
            target->setTextColor(TFT_BLACK);
            target->drawString("Prev", 45, footerY + 10);
        }

        if (libraryPage < totalPages - 1)
        {
            target->fillRect(target->width() - 120, footerY, 100, 40, TFT_LIGHTGREY);
            target->setTextColor(TFT_BLACK);
            target->drawString("Next", target->width() - 95, footerY + 10);
        }

        char pageStr[32];
        snprintf(pageStr, sizeof(pageStr), "%d / %d", libraryPage + 1, totalPages);
        target->setTextDatum(textdatum_t::top_center);
        target->drawString(pageStr, target->width() / 2, footerY + 10);
        target->setTextDatum(textdatum_t::top_left);
    }

    if (books.empty())
    {
        target->setCursor(20, LIBRARY_LIST_START_Y);
        if (!searchQuery.empty() || showFavoritesOnly)
        {
            // Show "no results" message when filtering
            target->println("No matching books found.");
            if (showFavoritesOnly)
            {
                target->setCursor(20, LIBRARY_LIST_START_Y + LIBRARY_LINE_HEIGHT);
                target->println("No favorites yet.");
            }
        }
        else
        {
            target->println("No books found.");
            target->setCursor(20, LIBRARY_LIST_START_Y + LIBRARY_LINE_HEIGHT);
            target->println("Upload via WiFi:");
            target->setCursor(20, LIBRARY_LIST_START_Y + LIBRARY_LINE_HEIGHT * 2);
            if (wifiConnected)
            {
                target->printf("http://%s/", wifiManager.getIpAddress().c_str());
            }
            else
            {
                target->println("Connect to AP 'M5Paper_Reader'");
            }
        }
    }

    // Draw keyboard overlay if visible
    if (showKeyboard)
    {
        drawKeyboard(target);
    }

    if (sprite)
    {
        sprite->pushSprite(&M5.Display, 0, 0);
    }

    M5.Display.waitDisplay();
    M5.Display.display();
}

// Returns number of characters that fit on the page
size_t GUI::drawPageContent(bool draw)
{
    return drawPageContentAt(currentTextOffset, draw, nullptr);
}

size_t GUI::drawPageContentAt(size_t startOffset, bool draw, M5Canvas *target, volatile bool *abort)
{
    LovyanGFX *gfx = target ? (LovyanGFX *)target : (LovyanGFX *)&M5.Display;
    // ESP_LOGI(TAG, "drawPageContentAt: offset=%zu, draw=%d, target=%p, w=%d, h=%d", startOffset, draw, target, gfx->width(), gfx->height());

    // Ensure correct base font is loaded on the target
    if (currentFont == "Hebrew" && !fontDataHebrew.empty())
    {
        gfx->loadFont(fontDataHebrew.data());
    }
    else if (currentFont != "Default" && !fontData.empty())
    {
        gfx->loadFont(fontData.data());
    }
    else
    {
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
    int lineHeight = gfx->fontHeight() * lineSpacing; // Configurable line spacing

    // Fetch a large chunk
    std::string text;
    if (takeEpubMutexWithWdt(epubMutex, abort))
    {
        text = epubLoader.getText(startOffset, 3000);
        xSemaphoreGive(epubMutex);
    }
    ESP_LOGI(TAG, "Fetched text at offset %zu, length: %zu", startOffset, text.length());
    if (text.empty())
    {
        ESP_LOGW(TAG, "No text fetched at offset %zu", startOffset);
        return 0;
    }

    // Ensure consistent black text on white background
    gfx->setTextColor(TFT_BLACK, TFT_WHITE);

    if (draw)
        gfx->startWrite();

    // Clear rendered images list when drawing to main display
    if (draw && target == nullptr)
    {
        pageRenderedImages.clear();
    }

    // Store indices (start, length, isMath) instead of strings to save memory
    std::vector<std::tuple<size_t, size_t, bool>> currentLine;
    currentLine.reserve(20); // Pre-allocate to avoid reallocations

    int currentLineWidth = 0;
    int currentY = y;
    size_t i = 0;
    bool inMathMode = false;

    int debugWordsLogged = 0;
    std::string tempWord; // Reusable buffer for measurements

    auto drawLine = [&](const std::vector<std::tuple<size_t, size_t, bool>> &line)
    {
        if (!draw || line.empty())
            return;

        bool isRTL = isRTLDocument;
        if (!isRTL)
        {
            for (const auto &p : line)
            {
                if (std::get<2>(p)) continue; // Skip math for RTL check
                // Check if word is Hebrew
                size_t len = std::get<1>(p);
                if (len == 0) continue; // Skip math blocks (length=0 marks MathML)
                const char *wordStart = text.c_str() + std::get<0>(p);
                for (size_t k = 0; k < len; ++k)
                {
                    unsigned char c = (unsigned char)wordStart[k];
                    if (c >= 0xD6 && c <= 0xD7)
                    {
                        isRTL = true;
                        break;
                    }
                }
                if (isRTL)
                    break;
            }
        }

        int startX = isRTL ? (width - rightMargin) : x;
        int spaceW = gfx->textWidth(" ");
        if (spaceW == 0)
            spaceW = 5; // Fallback if font has no space width

        for (const auto &p : line)
        {
            bool isMath = std::get<2>(p);
            size_t wordOffset = std::get<0>(p);
            size_t wordLen = std::get<1>(p);
            
            // Handle inline MathML blocks (marked by isMath=true and wordLen=0)
            if (isMath && wordLen == 0) {
                // Look up the MathML content
                size_t mathTextOffset = startOffset + wordOffset;
                const EpubMath* mathBlock = nullptr;
                if (xSemaphoreTake(epubMutex, pdMS_TO_TICKS(100))) {
                    mathBlock = epubLoader.findMathAtOffset(mathTextOffset, 10);
                    xSemaphoreGive(epubMutex);
                }
                
                if (mathBlock && !mathBlock->mathml.empty()) {
                    int mathWidth = 0, mathHeight = 0;
                    // Render inline math at baseline
                    renderMathInline(mathBlock->mathml, (M5Canvas*)target, startX, currentY, maxWidth, mathWidth, mathHeight);
                    
                    if (isRTL) {
                        startX -= (mathWidth + spaceW);
                    } else {
                        startX += (mathWidth + spaceW);
                    }
                }
                continue;
            }
            
            std::string displayWord = text.substr(wordOffset, wordLen);
            
            if (isMath) {
                // Old-style math word (should not happen with new code, but keep for safety)
                if (!fontDataMath.empty()) {
                    gfx->loadFont(fontDataMath.data());
                }
            } else if (isHebrew(displayWord))
            {
                // Replace HTML entities
                size_t pos = 0;
                while ((pos = displayWord.find("&quot;", pos)) != std::string::npos)
                {
                    displayWord.replace(pos, 6, "\"");
                    pos += 1;
                }
                pos = 0;
                while ((pos = displayWord.find("&amp;", pos)) != std::string::npos)
                {
                    displayWord.replace(pos, 5, "&");
                    pos += 1;
                }
                pos = 0;
                while ((pos = displayWord.find("&lt;", pos)) != std::string::npos)
                {
                    displayWord.replace(pos, 4, "<");
                    pos += 1;
                }
                pos = 0;
                while ((pos = displayWord.find("&gt;", pos)) != std::string::npos)
                {
                    displayWord.replace(pos, 4, ">");
                    pos += 1;
                }
                pos = 0;
                while ((pos = displayWord.find("&apos;", pos)) != std::string::npos)
                {
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
                drawStringMixed(displayWord, startX - w, currentY, (M5Canvas *)target, fontSize, true);
                startX -= (w + spaceWLocal);
            }
            else
            {
                drawStringMixed(displayWord, startX, currentY, (M5Canvas *)target, fontSize, true);
                startX += (w + spaceWLocal);
            }
            
            if (isMath) {
                // Restore book font
                if (currentFont == "Hebrew" && !fontDataHebrew.empty()) {
                    gfx->loadFont(fontDataHebrew.data());
                } else if (currentFont != "Default" && !fontData.empty()) {
                    gfx->loadFont(fontData.data());
                } else {
                    gfx->unloadFont();
                }
            }
        }

    };

    int linesDrawn = 0;
    while (i < text.length())
    {
        // Check abort flag
        if (abort && *abort)
        {
            if (draw)
                gfx->endWrite();
            return 0;
        }

        // Reset watchdog periodically during long rendering
        if (linesDrawn % 5 == 0)
        {
            esp_task_wdt_reset();
        }

        // Check for image placeholder character (U+E000 = \xEE\x80\x80)
        if (i + IMAGE_PLACEHOLDER_LEN <= text.length() &&
            text.compare(i, IMAGE_PLACEHOLDER_LEN, IMAGE_PLACEHOLDER) == 0)
        {
            // If images are disabled, just skip the placeholder
            if (!showImages)
            {
                i += IMAGE_PLACEHOLDER_LEN;
                continue;
            }

            // Found image placeholder - flush current line first
            if (!currentLine.empty())
            {
                drawLine(currentLine);
                linesDrawn++;
                currentLine.clear();
                currentLineWidth = 0;
                currentY += lineHeight;
            }

            // Find the image at this offset
            const EpubImage *image = nullptr;
            size_t imageOffset = startOffset + i;
            if (takeEpubMutexWithWdt(epubMutex, abort))
            {
                image = epubLoader.findImageAtOffset(imageOffset, 10);
                xSemaphoreGive(epubMutex);
            }

            if (image && draw)
            {
                // Calculate image dimensions
                int imgMaxWidth = maxWidth;
                int imgMaxHeight = maxY - currentY;

                // For inline images, limit height more
                if (!image->isBlock)
                {
                    imgMaxHeight = std::min(imgMaxHeight, lineHeight * 4);
                    imgMaxWidth = maxWidth / 2;
                }

                // Only render if we have enough vertical space
                if (imgMaxHeight > 50)
                {
                    // Extract and render image
                    std::vector<uint8_t> imageData;
                    bool extracted = false;
                    bool resumeWrite = false;
                    if (draw && target == nullptr && gfx->isEPD())
                    {
                        // Release EPD SPI bus before SD/zip file I/O to avoid deadlock.
                        gfx->endWrite();
                        resumeWrite = true;
                    }
                    if (takeEpubMutexWithWdt(epubMutex, abort))
                    {
                        extracted = epubLoader.extractImage(image->path, imageData);
                        xSemaphoreGive(epubMutex);
                    }
                    if (resumeWrite)
                    {
                        gfx->startWrite();
                        gfx->setTextColor(TFT_BLACK, TFT_WHITE);
                    }

                    if (extracted && !imageData.empty())
                    {
                        ImageHandler &imgHandler = ImageHandler::getInstance();
                        int imgX = x;
                        int imgY = currentY;

                        ImageDecodeResult result = imgHandler.decodeAndRender(
                            imageData.data(),
                            imageData.size(),
                            (M5Canvas *)target,
                            imgX, imgY,
                            imgMaxWidth,
                            imgMaxHeight,
                            image->isBlock ? ImageDisplayMode::BLOCK : ImageDisplayMode::INLINE);

                        if (result.success)
                        {
                            // Track this rendered image for tap detection (only for main display)
                            if (target == nullptr)
                            {
                                RenderedImageInfo ri;
                                ri.x = imgX;
                                ri.y = imgY;
                                ri.width = result.scaledWidth - 20 > 0 ? result.scaledWidth - 20 : result.scaledWidth;     // Adjust for margins
                                ri.height = result.scaledHeight - 20 > 0 ? result.scaledHeight - 20 : result.scaledHeight; // Adjust for margins
                                ri.image = *image;
                                pageRenderedImages.push_back(ri);
                            }

                            // Move Y past the image
                            currentY += result.scaledHeight + 10; // Add some padding
                            ESP_LOGI(TAG, "Rendered image at Y=%d, size=%dx%d",
                                     imgY, result.scaledWidth, result.scaledHeight);
                        }
                        else
                        {
                            // Show placeholder text for failed image
                            gfx->setTextColor(TFT_DARKGRAY, TFT_WHITE);
                            gfx->setTextDatum(textdatum_t::top_left);
                            gfx->drawString("[Image]", x, currentY);
                            gfx->setTextColor(TFT_BLACK, TFT_WHITE);
                            currentY += lineHeight;
                        }
                    }
                    else
                    {
                        // Show placeholder for missing image
                        gfx->setTextColor(TFT_DARKGRAY, TFT_WHITE);
                        gfx->setTextDatum(textdatum_t::top_left);
                        gfx->drawString("[Image]", x, currentY);
                        gfx->setTextColor(TFT_BLACK, TFT_WHITE);
                        currentY += lineHeight;
                    }
                }
                else
                {
                    // Not enough space - page is full
                    break;
                }
            }
            else if (!draw)
            {
                // When measuring (not drawing), account for image space
                // Estimate a reasonable image height
                currentY += lineHeight * 3; // Reserve space for typical image
            }

            // Skip past the placeholder
            i += IMAGE_PLACEHOLDER_LEN;

            // Check if page is full after image
            if (currentY + lineHeight > maxY)
            {
                break;
            }

            continue; // Continue to next word/element
        }

        // Check for math markers - render MathML blocks properly
        if (i + MATH_MARKER_LEN <= text.length()) {
            if (text.compare(i, MATH_MARKER_LEN, MATH_START) == 0) {
                // Look up the MathML content
                size_t mathTextOffset = startOffset + i;
                const EpubMath* mathBlock = nullptr;
                if (takeEpubMutexWithWdt(epubMutex, abort)) {
                    mathBlock = epubLoader.findMathAtOffset(mathTextOffset, 10);
                    xSemaphoreGive(epubMutex);
                }
                
                if (mathBlock && !mathBlock->mathml.empty()) {
                    // Measure the math content
                    int mathWidth = 0, mathHeight = 0, mathBaseline = 0;
                    measureMath(mathBlock->mathml, mathWidth, mathHeight, mathBaseline);
                    
                    // For block math, center it on its own line
                    if (mathBlock->isBlock) {
                        // Flush current line first
                        if (!currentLine.empty()) {
                            drawLine(currentLine);
                            linesDrawn++;
                            currentLine.clear();
                            currentLineWidth = 0;
                            currentY += lineHeight;
                        }
                        
                        // Check if math block fits on page
                        if (currentY + mathHeight > maxY) {
                            break; // Page full
                        }
                        
                        // Center the math
                        int mathX = x + (maxWidth - mathWidth) / 2;
                        
                        if (draw && target) {
                            int dummy1, dummy2;
                            renderMathInline(mathBlock->mathml, (M5Canvas*)target, mathX, currentY + mathBaseline, maxWidth, dummy1, dummy2);
                        } else if (draw) {
                            int dummy1, dummy2;
                            M5Canvas* mainCanvas = nullptr;
                            renderMathInline(mathBlock->mathml, mainCanvas, mathX, currentY + mathBaseline, maxWidth, dummy1, dummy2);
                        }
                        
                        currentY += mathHeight + lineHeight / 2;  // Add some spacing
                    } else {
                        // Inline math - check if it fits on current line
                        if (currentLineWidth + mathWidth > maxWidth) {
                            // Doesn't fit - flush line first
                            drawLine(currentLine);
                            linesDrawn++;
                            currentLine.clear();
                            currentLineWidth = 0;
                            currentY += lineHeight;
                            
                            if (currentY + lineHeight > maxY) {
                                break; // Page full
                            }
                        }
                        
                        // Add as a special math entry - use negative length to mark as math block index
                        // We'll store the text offset so drawLine can find the math
                        currentLine.emplace_back(i, 0, true);  // length=0 marks as MathML block
                        currentLineWidth += mathWidth + gfx->textWidth(" ");
                    }
                }
                
                // Skip to MATH_END
                size_t mathEndPos = text.find(MATH_END, i);
                if (mathEndPos != std::string::npos) {
                    i = mathEndPos + MATH_MARKER_LEN;
                } else {
                    i += MATH_MARKER_LEN;
                }
                inMathMode = false;
                continue;
            }
            if (text.compare(i, MATH_MARKER_LEN, MATH_END) == 0) {
                inMathMode = false;
                i += MATH_MARKER_LEN;
                continue;
            }
        }

        // Find next word boundary
        size_t nextSpace = text.find_first_of(" \n", i);
        size_t nextMathStart = text.find(MATH_START, i);
        size_t nextMathEnd = text.find(MATH_END, i);
        
        size_t boundary = nextSpace;
        if (boundary == std::string::npos) boundary = text.length();
        
        if (nextMathStart != std::string::npos && nextMathStart < boundary) boundary = nextMathStart;
        if (nextMathEnd != std::string::npos && nextMathEnd < boundary) boundary = nextMathEnd;

        bool isNewline = (boundary < text.length() && text[boundary] == '\n');

        size_t wordLen = boundary - i;

        // Measure word using reusable buffer
        tempWord.assign(text, i, wordLen);

        bool wordIsHebrew = isHebrew(tempWord);
        if (wordIsHebrew)
            tempWord = reverseHebrewWord(tempWord);

        // Make sure we measure with the same font that will be used to draw the word.
        bool swappedFont = false;
        if (inMathMode && !fontDataMath.empty()) {
             gfx->loadFont(fontDataMath.data());
             gfx->setTextSize(fontSize);
             swappedFont = true;
        } else if (wordIsHebrew && !fontDataHebrew.empty() && currentFont != "Hebrew")
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
            if (currentFont == "Hebrew" && !fontDataHebrew.empty()) {
                gfx->loadFont(fontDataHebrew.data());
            } else if (currentFont != "Default" && !fontData.empty())
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

        currentLine.emplace_back(i, wordLen, inMathMode);
        currentLineWidth += w + spaceW;

        if (boundary < text.length())
        {
            if (text[boundary] == ' ' || text[boundary] == '\n') {
                i = boundary + 1;
            } else {
                // Marker
                i = boundary;
            }
        }
        else
        {
            i = boundary;
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

    size_t chapterSize = 0;
    if (xSemaphoreTake(epubMutex, portMAX_DELAY))
    {
        chapterSize = epubLoader.getChapterSize();
        xSemaphoreGive(epubMutex);
    }

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

void GUI::computeBookMetrics()
{
    if (!epubMutex)
        return;

    if (xSemaphoreTake(epubMutex, portMAX_DELAY))
    {
        computeBookMetricsLocked();
        xSemaphoreGive(epubMutex);
    }
}

void GUI::computeBookMetricsLocked()
{
    bookMetricsComputed = false;
    totalBookChars = 0;
    chapterPrefixSums.clear();

    int chapters = epubLoader.getTotalChapters();
    if (chapters <= 0)
        return;

    chapterPrefixSums.resize(chapters + 1, 0);
    size_t cumulative = 0;
    TickType_t lastYield = xTaskGetTickCount();
    for (int i = 0; i < chapters; ++i)
    {
        size_t len = epubLoader.getChapterTextLength(i);
        cumulative += len;
        chapterPrefixSums[i + 1] = cumulative;

        // Avoid starving IDLE/WDT during long scans of many chapters
        TickType_t now = xTaskGetTickCount();
        if (now - lastYield >= pdMS_TO_TICKS(50))
        {
            vTaskDelay(1);
            lastYield = xTaskGetTickCount();
        }
    }

    totalBookChars = cumulative;
    bookMetricsComputed = cumulative > 0;
}

void GUI::updateNextPrevCanvases()
{
    // Abort any ongoing background render
    abortRender = true;
    ESP_LOGI(TAG, "updateNextPrevCanvases: lastPageChars=%zu nextValid=%d prevValid=%d nextOffset=%zu prevOffset=%zu",
             lastPageChars, nextCanvasValid, prevCanvasValid, nextCanvasOffset, prevCanvasOffset);

    // Update Next
    size_t charsOnCurrent = lastPageChars;
    size_t newNextOffset = currentTextOffset + charsOnCurrent;

    size_t chSize = 0;
    if (xSemaphoreTake(epubMutex, portMAX_DELAY))
    {
        chSize = epubLoader.getChapterSize();
        xSemaphoreGive(epubMutex);
    }

    if (newNextOffset < chSize && canvasNext.width() > 0)
    {
        // Only redraw if invalid or offset changed
        if (!nextCanvasValid || nextCanvasOffset != newNextOffset)
        {
            RenderRequest req;
            req.offset = newNextOffset;
            req.target = &canvasNext;
            req.isNext = true;
            BaseType_t sent = xQueueSend(renderQueue, &req, 0);
            if (sent == pdTRUE)
            {
                ESP_LOGI(TAG, "Queued next canvas render: offset=%zu", newNextOffset);
            }
            else
            {
                ESP_LOGW(TAG, "Failed to queue next canvas render: offset=%zu (queue full?)", newNextOffset);
            }
        }
        else
        {
            ESP_LOGI(TAG, "Next canvas already valid for offset=%zu", newNextOffset);
        }
    }
    else
    {
        nextCanvasValid = false;
        ESP_LOGI(TAG, "Skipping next canvas render. newNextOffset=%zu chSize=%zu canvasWidth=%d",
                 newNextOffset, chSize, canvasNext.width());
    }

    // Update Prev
    if (!pageHistory.empty() && canvasPrev.width() > 0)
    {
        size_t newPrevOffset = pageHistory.back();
        if (!prevCanvasValid || prevCanvasOffset != newPrevOffset)
        {
            RenderRequest req;
            req.offset = newPrevOffset;
            req.target = &canvasPrev;
            req.isNext = false;
            BaseType_t sent = xQueueSend(renderQueue, &req, 0);
            if (sent == pdTRUE)
            {
                ESP_LOGI(TAG, "Queued prev canvas render: offset=%zu", newPrevOffset);
            }
            else
            {
                ESP_LOGW(TAG, "Failed to queue prev canvas render: offset=%zu (queue full?)", newPrevOffset);
            }
        }
        else
        {
            ESP_LOGI(TAG, "Prev canvas already valid for offset=%zu", newPrevOffset);
        }
    }
    else
    {
        prevCanvasValid = false;
        ESP_LOGI(TAG, "Skipping prev canvas render. pageHistory empty=%d canvasWidth=%d",
                 pageHistory.empty(), canvasPrev.width());
    }
}

void GUI::drawReader(bool flush)
{
    ESP_LOGI(TAG, "drawReader called, currentTextOffset: %zu", currentTextOffset);

    // Ensure previous update is complete
    M5.Display.waitDisplay();

    bool drawnFromBuffer = false;
    size_t charsDrawn = 0;

    // Only use buffers if they are valid (width > 0)
    if (nextCanvasValid && nextCanvasOffset == currentTextOffset && canvasNext.width() > 0)
    {
        ESP_LOGI(TAG, "Drawing from next buffer");
        canvasNext.pushSprite(&M5.Display, 0, 0);
        charsDrawn = nextCanvasCharCount;
        drawnFromBuffer = true;
    }
    else if (prevCanvasValid && prevCanvasOffset == currentTextOffset && canvasPrev.width() > 0)
    {
        ESP_LOGI(TAG, "Drawing from prev buffer");
        canvasPrev.pushSprite(&M5.Display, 0, 0);
        charsDrawn = prevCanvasCharCount;
        drawnFromBuffer = true;
    }
    else
    {
        ESP_LOGI(TAG, "Buffer not used. nextValid=%d nextOffset=%zu prevValid=%d prevOffset=%zu canvasW=%d",
                 nextCanvasValid, nextCanvasOffset, prevCanvasValid, prevCanvasOffset, canvasNext.width());
    }

    if (!drawnFromBuffer)
    {
        ESP_LOGI(TAG, "Drawing to current buffer");
        M5.Display.fillScreen(TFT_WHITE);
        // Capture the char count directly from the draw call
        charsDrawn = drawPageContentAt(currentTextOffset, true, nullptr);
    }

    // Draw Status Bar (Fresh). If we rendered from a buffer, prefer drawing the bar onto the buffer during render task.
    if (!drawnFromBuffer)
    {
        drawStatusBar();
    }

    // Update cached char count
    if (charsDrawn > 0)
    {
        lastPageChars = charsDrawn;
        size_t chapterSize = 0;
        if (xSemaphoreTake(epubMutex, portMAX_DELAY))
        {
            chapterSize = epubLoader.getChapterSize();
            xSemaphoreGive(epubMutex);
        }
        lastPageTotal = static_cast<int>((chapterSize + lastPageChars - 1) / lastPageChars);
        if (lastPageTotal < 1)
            lastPageTotal = 1;
    }

    if (!drawnFromBuffer)
    {
        drawFooter(&M5.Display, currentTextOffset, charsDrawn);
    }

    if (flush)
    {
        ESP_LOGI(TAG, "Calling M5.Display.display()");
        M5.Display.waitDisplay();
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
    // Flush to e-ink display
    M5.Display.display();
    M5.Display.waitDisplay();
}

void GUI::drawMainMenu()
{
    // Main menu with 6 large buttons in a 3x2 grid
    abortRender = true; // Stop any background rendering

    // Use canvasNext as a buffer if available
    M5Canvas *sprite = (canvasNext.width() > 0) ? &canvasNext : nullptr;
    LovyanGFX *target = sprite ? (LovyanGFX *)sprite : (LovyanGFX *)&M5.Display;

    target->fillScreen(TFT_WHITE);
    drawStatusBar(target);

    // Use system font
    target->setFont(&lgfx::v1::fonts::Font2);
    target->setTextColor(TFT_BLACK);

    int screenW = target->width();
    int screenH = target->height();

    // Calculate grid dimensions
    int availableH = screenH - STATUS_BAR_HEIGHT - 20; // Leave margin at bottom
    int availableW = screenW - 20;                     // 10px margin on each side

    int cols = (screenW > screenH) ? 3 : 2;
    int rows = (screenW > screenH) ? 2 : 3;
    int btnGap = 12;
    int btnW = (availableW - btnGap * (cols - 1)) / cols;
    int btnH = (availableH - btnGap * (rows - 1)) / rows;

    int startX = 10;
    int startY = STATUS_BAR_HEIGHT + 10;

    // Button definitions: {label, sublabel}
    struct MenuButton
    {
        const char *label;
        const char *sublabel;
        bool enabled;
        const char *icon;
    };

    // Check if we have a last book
    bool hasLastBook = (lastBookId > 0 && !lastBookTitle.empty());

    MenuButton buttons[6] = {
        {"Last Book", hasLastBook ? lastBookTitle.c_str() : "No book yet", hasLastBook, "last-book-gray.png"},
        {"Books", "Library", true, "books-gray.png"},
        {"WiFi", "Network", true, "wifi-gray.png"},
        {"Settings", "Options", true, "settings-gray.png"},
        {"Favorites", "Starred", true, "favorite-gray.png"},
        {"More", "Games", true, "games-gray.png"} // Games and extras
    };

    target->startWrite();

    const float labelTextSize = 2.4f;
    const float sublabelTextSize = 1.5f;
    ImageHandler &imgHandler = ImageHandler::getInstance();
    static constexpr int MENU_ICON_TARGET_SIZE = 96;

    struct MenuIconCache
    {
        std::string path;
        std::vector<uint8_t> data;
        bool dataLoaded = false;
    };
    static MenuIconCache iconCache[6];
    
    struct MenuIconDraw
    {
        int index;
        std::string path;
        int x;
        int y;
        int size;
    };
    std::vector<MenuIconDraw> deferredIcons;

    const bool needsIconFlatten = deviceHAL.isM5Paper();
    M5Canvas iconSprite;
    if (needsIconFlatten)
    {
        iconSprite.setColorDepth(16);
        iconSprite.setPsram(true);
    }

    auto ensureIconCache = [&](int index, const std::string &iconPath)
    {
        if (index < 0 || index >= 6)
        {
            return false;
        }
        auto &cache = iconCache[index];
        if (cache.path != iconPath)
        {
            cache.path = iconPath;
            cache.data.clear();
            cache.dataLoaded = false;
        }

        if (!cache.dataLoaded)
        {
            struct stat st;
            if (stat(iconPath.c_str(), &st) != 0)
            {
                return false;
            }
            FILE *f = fopen(iconPath.c_str(), "rb");
            if (!f)
            {
                return false;
            }
            cache.data.resize(st.st_size);
            if (fread(cache.data.data(), 1, cache.data.size(), f) != cache.data.size())
            {
                cache.data.clear();
                fclose(f);
                return false;
            }
            fclose(f);
            cache.dataLoaded = true;
        }

        return cache.dataLoaded;
    };

    auto drawMenuIcon = [&](int index, LovyanGFX *iconTarget, const std::string &iconPath, int x, int y, int size)
    {
        if (size <= 0)
        {
            return;
        }
        if (index >= 0 && index < 6)
        {
            if (ensureIconCache(index, iconPath))
            {
                imgHandler.decodeAndRender(iconCache[index].data.data(),
                                           iconCache[index].data.size(),
                                           iconTarget, x, y, size, size, ImageDisplayMode::INLINE);
                return;
            }
        }
        struct stat st;
        if (stat(iconPath.c_str(), &st) != 0)
        {
            return;
        }
        FILE *f = fopen(iconPath.c_str(), "rb");
        if (!f)
        {
            return;
        }
        size_t len = st.st_size;
        uint8_t *buf = (uint8_t *)malloc(len);
        if (!buf)
        {
            fclose(f);
            return;
        }
        if (fread(buf, 1, len, f) == len)
        {
            imgHandler.decodeAndRender(buf, len, iconTarget, x, y, size, size, ImageDisplayMode::INLINE);
        }
        free(buf);
        fclose(f);
    };

    auto drawMenuIconWithBackground = [&](int index, LovyanGFX *iconTarget, const std::string &iconPath, int x, int y, int size, uint16_t bgColor)
    {
        if (!needsIconFlatten)
        {
            drawMenuIcon(index, iconTarget, iconPath, x, y, size);
            return;
        }
        if (size <= 0)
        {
            return;
        }
        if (iconSprite.width() != size || iconSprite.height() != size)
        {
            iconSprite.deleteSprite();
            iconSprite.setColorDepth(16);
            iconSprite.setPsram(true);
            if (!iconSprite.createSprite(size, size))
            {
                drawMenuIcon(index, iconTarget, iconPath, x, y, size);
                return;
            }
        }
        // Flatten alpha on a higher-depth sprite so 2bpp targets don't turn transparency black.
        iconSprite.fillScreen(bgColor);
        drawMenuIcon(index, &iconSprite, iconPath, 0, 0, size);
        iconSprite.pushSprite(iconTarget, x, y);
    };

    for (int i = 0; i < 6; i++)
    {
        esp_task_wdt_reset();
        int row = i / cols;
        int col = i % cols;

        int bx = startX + col * (btnW + btnGap);
        int by = startY + row * (btnH + btnGap);

        // Draw button background
        uint16_t bgColor = TFT_WHITE;
        uint16_t borderColor = TFT_BLACK;

        if (!buttons[i].enabled)
        {
            bgColor = TFT_LIGHTGREY;
        }

        target->fillRect(bx, by, btnW, btnH, bgColor);
        target->drawRect(bx, by, btnW, btnH, borderColor);
        target->drawRect(bx + 1, by + 1, btnW - 2, btnH - 2, borderColor); // Double border

        // Draw label (larger font, top aligned)
        target->setTextDatum(textdatum_t::top_center);
        target->setTextSize(labelTextSize);
        target->setTextColor(buttons[i].enabled ? TFT_BLACK : TFT_DARKGREY, bgColor);

        int textY = by + 12; // Top margin
        target->drawString(buttons[i].label, bx + btnW / 2, textY);

        // Draw Icon
        if (buttons[i].icon)
        {
            std::string iconPath = "/spiffs/icons/";
            iconPath += buttons[i].icon;

            int labelBottom = textY + target->fontHeight();
            target->setTextSize(sublabelTextSize);
            int sublabelHeight = target->fontHeight();
            int sublabelBottom = by + btnH - 8;
            int sublabelTop = sublabelBottom - sublabelHeight;

            int iconTop = labelBottom + 2;
            int iconBottom = sublabelTop - 2;
            int progressBarH = 6;
            if (i == 1 && indexingScanActive)
            {
                iconBottom -= (progressBarH + 6);
            }
            int iconH = iconBottom - iconTop;
            int iconW = btnW - 12;
            int iconSize = std::min(MENU_ICON_TARGET_SIZE, std::min(iconW, iconH));
            int iconX = bx + (btnW - iconSize) / 2;
            int iconY = iconTop + (iconH - iconSize) / 2;

            if (deviceHAL.isM5PaperS3())
            {
                if (sprite)
                {
                    deferredIcons.push_back({i, iconPath, iconX, iconY, iconSize});
                }
                else
                {
                    drawMenuIcon(i, target, iconPath, iconX, iconY, iconSize);
                }
            }
            else
            {
                drawMenuIconWithBackground(i, target, iconPath, iconX, iconY, iconSize, bgColor);
            }

            if (i == 1 && indexingScanActive)
            {
                int barW = btnW - 30;
                int barX = bx + (btnW - barW) / 2;
                int barY = iconBottom + 4;
                float pct = 0.2f;
                if (indexingTotal > 0)
                {
                    pct = (float)indexingCurrent / (float)indexingTotal;
                }
                pct = std::max(0.0f, std::min(1.0f, pct));
                target->drawRect(barX, barY, barW, progressBarH, TFT_BLACK);
                int fillW = (int)((barW - 2) * pct);
                if (fillW > 0)
                {
                    target->fillRect(barX + 1, barY + 1, fillW, progressBarH - 2, TFT_BLACK);
                }
            }
        }

        // Draw sublabel (smaller font) at bottom
        if (buttons[i].sublabel && strlen(buttons[i].sublabel) > 0)
        {
            target->setTextSize(sublabelTextSize);
            target->setTextColor(buttons[i].enabled ? TFT_DARKGREY : TFT_LIGHTGREY, bgColor);
            target->setTextDatum(textdatum_t::bottom_center);

            std::string sublabel = buttons[i].sublabel;
            int subY = by + btnH - 10;

            if (i == 0 && hasLastBook)
            {
                // For last book, use mixed font support (book title may be Hebrew)
                int textW = target->textWidth(sublabel.c_str());
                drawStringMixed(sublabel, bx + btnW / 2 - textW / 2,
                                subY - 20, sprite, 1.3f, false, false);
            }
            else
            {
                target->drawString(sublabel.c_str(), bx + btnW / 2, subY);
            }
        }
    }

    // Show indexing status if active (Issue 3)
    if (indexingScanActive)
    {
        char buf[64];
        snprintf(buf, sizeof(buf), "Scanning... %d/%d", indexingCurrent, indexingTotal);
        target->setTextDatum(textdatum_t::bottom_center);
        target->setTextSize(1.0f);
        target->drawString(buf, screenW / 2, screenH - 5);
    }
    else if (indexingProcessingActive)
    {
        char buf[64];
        snprintf(buf, sizeof(buf), "Processing... %d/%d", indexingCurrent, indexingTotal);
        target->setTextDatum(textdatum_t::bottom_center);
        target->setTextSize(1.0f);
        target->drawString(buf, screenW / 2, screenH - 5);
    }

    target->endWrite();
    target->setTextDatum(textdatum_t::top_left);

    if (sprite)
    {
        sprite->pushSprite(&M5.Display, 0, 0);
    }

    if (deviceHAL.isM5PaperS3() && sprite && !deferredIcons.empty())
    {
        for (const auto &icon : deferredIcons)
        {
            drawMenuIcon(icon.index, &M5.Display, icon.path, icon.x, icon.y, icon.size);
        }
    }

    // M5.Display.waitDisplay(); // Removed to avoid blocking if not needed
    M5.Display.display();
}

void GUI::goToSleep()
{
    ESP_LOGI(TAG, "Entering goToSleep()...");
    // Save progress using the centralized helper
    saveLastBook();

    // Flush any pending display operations before sleeping
    esp_task_wdt_reset();
    M5.Display.waitDisplay();

    bool shouldReconnect = false;
    // Turn off WiFi to save power while sleeping
    if (wifiManager.isInitialized())
    {
        wifi_mode_t mode = WIFI_MODE_NULL;
        shouldReconnect = false;
        if (esp_wifi_get_mode(&mode) == ESP_OK)
        {
            shouldReconnect = (mode == WIFI_MODE_STA || mode == WIFI_MODE_APSTA);
        }
        esp_wifi_stop();
    }
    esp_task_wdt_reset();

    // Light sleep with touch wakeup handled by M5.Power. Also arm a timer so we can fall through to deep sleep.
    drawSleepSymbol("z");
    esp_task_wdt_reset();

#ifdef CONFIG_EBOOK_DEVICE_M5PAPERS3
    // M5PaperS3 specific: 5 minutes light sleep with GPIO 48 wake-up
    // Touch interrupt is connected to GPIO48 which is NOT an RTC GPIO,
    // therefore touch wakeup only works with light sleep.
    esp_sleep_enable_timer_wakeup(LIGHT_SLEEP_TO_DEEP_SLEEP_US);
    gpio_wakeup_enable((gpio_num_t)48, GPIO_INTR_LOW_LEVEL);
    esp_sleep_enable_gpio_wakeup();
    
    ESP_LOGI(TAG, "Starting light sleep (S3)...");
    fflush(stdout);
    
    esp_light_sleep_start();
    
    ESP_LOGI(TAG, "Woke from light sleep (S3)");
    gpio_wakeup_disable((gpio_num_t)48);
#else
    ESP_LOGI(TAG, "Starting light sleep (M5Paper)...");
    M5.Power.lightSleep(LIGHT_SLEEP_TO_DEEP_SLEEP_US, true);
    ESP_LOGI(TAG, "Woke from light sleep (M5Paper)");
#endif

    // Restore display state after wake from light sleep
    // drawSleepSymbol changed font, size, and datum. We must restore them.
    M5.Display.setTextDatum(textdatum_t::top_left);
    M5.Display.setTextColor(TFT_BLACK); // Restore transparent background
    if (currentFont == "Default")
    {
        M5.Display.setFont(&lgfx::v1::fonts::Font2); // Or whatever default is
    }
    else if (currentFont == "Hebrew" && !fontDataHebrew.empty())
    {
        M5.Display.loadFont(fontDataHebrew.data());
    }
    else if (!fontData.empty())
    {
        M5.Display.loadFont(fontData.data());
    }
    else
    {
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

    // If the timer woke us, show wallpaper and enter deep sleep
    auto wakeReason = esp_sleep_get_wakeup_cause();
    if (wakeReason == ESP_SLEEP_WAKEUP_TIMER)
    {
        ESP_LOGI(TAG, "Light sleep timer elapsed, showing wallpaper and entering deep sleep");
        showWallpaperAndSleep();
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
    if (settingsNeedsUnderlayRefresh)
    {
        drawReader(false); // Draw but don't flush yet
        settingsNeedsUnderlayRefresh = false;
    }

    SettingsLayout layout = computeSettingsLayout();

    // Initialize settings canvas if needed
    if (!settingsCanvasCreated)
    {
        // Use 4bpp for better visuals (gray background)
        const int depth = 4;
        const size_t bytesPerSprite = ((size_t)layout.panelWidth * layout.panelHeight * depth + 7) / 8;
        const size_t freeDef = heap_caps_get_free_size(MALLOC_CAP_DEFAULT);
        const size_t freeSpi = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
        const size_t largestDef = heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT);
        const size_t largestSpi = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);

        settingsCanvas.setColorDepth(depth);
        settingsCanvas.setPsram(true);
        ESP_LOGI(TAG, "Settings canvas alloc: w=%d h=%d depth=%dbpp ~%u bytes. Free: default=%u (largest %u) SPIRAM=%u (largest %u)",
                 layout.panelWidth, layout.panelHeight, depth, (unsigned)bytesPerSprite,
                 (unsigned)freeDef, (unsigned)largestDef,
                 (unsigned)freeSpi, (unsigned)largestSpi);
        if (settingsCanvas.createSprite(layout.panelWidth, layout.panelHeight))
        {
            // 4bpp has default grayscale palette, no need to set manually unless customizing
            ESP_LOGI(TAG, "Settings canvas created. Free now: default=%u (largest %u) SPIRAM=%u (largest %u)",
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_DEFAULT),
                     (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT),
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                     (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
            settingsCanvasCreated = true;
        }
        else
        {
            ESP_LOGE(TAG, "Failed to create settings canvas. Free now: default=%u (largest %u) SPIRAM=%u (largest %u) (needed ~%u bytes)",
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_DEFAULT),
                     (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT),
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                     (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM),
                     (unsigned)bytesPerSprite);
        }
    }

    LovyanGFX *target = settingsCanvasCreated ? (LovyanGFX *)&settingsCanvas : (LovyanGFX *)&M5.Display;
    int yOffset = settingsCanvasCreated ? -layout.panelTop : 0;

    // Use a fixed system font regardless of reader font settings
    target->setFont(&lgfx::v1::fonts::Font2);

    // Clear background for settings panel
    target->fillRect(0, layout.panelTop + yOffset, layout.panelWidth, layout.panelHeight, TFT_LIGHTGRAY);
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
    const float settingsTitleSize = 2.4f;
    target->setTextSize(settingsTitleSize);
    target->setTextDatum(textdatum_t::top_left);
    target->drawString("Settings", layout.padding, layout.titleY + yOffset);

    // Body styling
    const float bodyTextSize = 1.8f;
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
        status = "http://" + ip + "/";
    }
    target->setTextSize(bodyTextSize);
    target->setTextDatum(textdatum_t::middle_left);
    // Increased offset to avoid overlap
    target->drawString(status.c_str(), layout.padding + 160, row4CenterY + yOffset);
    target->setTextSize(bodyTextSize);

    // --- Sleep Delay ---
    int row5CenterY = layout.row5Y + layout.rowHeight / 2;
    target->drawString("Sleep Delay", layout.padding, row5CenterY + yOffset);
    char sleepBuf[16];
    snprintf(sleepBuf, sizeof(sleepBuf), "%d min", lightSleepMinutes);
    target->setTextDatum(textdatum_t::middle_right);
    target->drawString(sleepBuf, fontValueRight, row5CenterY + yOffset);
    target->setTextDatum(textdatum_t::middle_left);
    int sleepButtonY = layout.row5Y + (layout.rowHeight - layout.fontButtonH) / 2;
    drawButton(layout.fontMinusX, sleepButtonY, layout.fontButtonW, layout.fontButtonH, "-", TFT_WHITE, TFT_BLACK);
    drawButton(layout.fontPlusX, sleepButtonY, layout.fontButtonW, layout.fontButtonH, "+", TFT_WHITE, TFT_BLACK);

    // --- Favorite ---
    int row6CenterY = layout.row6Y + layout.rowHeight / 2;
    target->drawString("Favorite", layout.padding, row6CenterY + yOffset);
    bool isFav = bookIndex.isFavorite(lastBookId);
    int favButtonY = layout.row6Y + (layout.rowHeight - layout.fontButtonH) / 2;
    uint16_t favFill = isFav ? TFT_BLACK : TFT_WHITE;
    uint16_t favText = isFav ? TFT_WHITE : TFT_BLACK;
    drawButton(layout.toggleButtonX, favButtonY, layout.toggleButtonW, layout.fontButtonH,
               isFav ? "YES" : "NO", favFill, favText);

    // --- Close Button ---
    int closeX = layout.panelWidth - layout.padding - layout.closeButtonW;
    drawButton(closeX, layout.closeY, layout.closeButtonW, layout.closeButtonH, "Close", TFT_WHITE, TFT_BLACK);

    // Push the canvas to the display
    if (settingsCanvasCreated)
    {
        settingsCanvas.pushSprite(&M5.Display, 0, layout.panelTop);
    }
}

void GUI::drawStandaloneSettings()
{
    // Full-page settings (accessed from main menu, not as an overlay)
    abortRender = true;

    M5Canvas *sprite = (canvasNext.width() > 0) ? &canvasNext : nullptr;
    LovyanGFX *target = sprite ? (LovyanGFX *)sprite : (LovyanGFX *)&M5.Display;

    target->fillScreen(TFT_WHITE);
    drawStatusBar(target);

    target->setFont(&lgfx::v1::fonts::Font2);
    target->setTextColor(TFT_BLACK, TFT_WHITE);

    int screenW = target->width();
    int screenH = target->height();
    int padding = 20;
    int rowHeight = 60;
    int buttonH = 45;
    int buttonW = 140;

    auto drawSettingsButton = [&](int x, int y, int w, int h, const char *label, uint16_t fillColor, uint16_t textColor)
    {
        target->fillRect(x, y, w, h, fillColor);
        target->drawRect(x, y, w, h, TFT_BLACK);
        target->setTextDatum(textdatum_t::middle_center);
        target->setTextColor(textColor, fillColor);
        target->drawString(label, x + w / 2, y + h / 2);
        target->setTextColor(TFT_BLACK, TFT_WHITE);
        target->setTextDatum(textdatum_t::middle_left);
    };

    // Title
    target->setTextSize(2.5f);
    target->setTextDatum(textdatum_t::top_center);
    target->drawString("Settings", screenW / 2, STATUS_BAR_HEIGHT + 15);
    target->setTextDatum(textdatum_t::middle_left);

    int startY = STATUS_BAR_HEIGHT + 70;
    target->setTextSize(1.8f);

    // --- Font Size ---
    int row1Y = startY;
    int row1CenterY = row1Y + rowHeight / 2;
    target->drawString("Font Size", padding, row1CenterY);

    char sizeBuf[16];
    snprintf(sizeBuf, sizeof(sizeBuf), "%.1f", fontSize);
    int fontValueX = screenW / 2;
    target->setTextDatum(textdatum_t::middle_center);
    target->drawString(sizeBuf, fontValueX, row1CenterY);
    target->setTextDatum(textdatum_t::middle_left);

    int fontMinusX = screenW - padding - buttonW * 2 - 10;
    int fontPlusX = screenW - padding - buttonW;
    int btnY = row1Y + (rowHeight - buttonH) / 2;
    drawSettingsButton(fontMinusX, btnY, buttonW, buttonH, "-", TFT_WHITE, TFT_BLACK);
    drawSettingsButton(fontPlusX, btnY, buttonW, buttonH, "+", TFT_WHITE, TFT_BLACK);

    // --- Font Family ---
    int row2Y = startY + rowHeight;
    int row2CenterY = row2Y + rowHeight / 2;
    target->drawString("Font", padding, row2CenterY);
    target->setTextDatum(textdatum_t::middle_center);
    target->drawString(currentFont.c_str(), fontValueX, row2CenterY);
    target->setTextDatum(textdatum_t::middle_left);

    btnY = row2Y + (rowHeight - buttonH) / 2;
    drawSettingsButton(screenW - padding - buttonW, btnY, buttonW, buttonH, "Change", TFT_WHITE, TFT_BLACK);

    // --- WiFi Toggle ---
    int row3Y = startY + rowHeight * 2;
    int row3CenterY = row3Y + rowHeight / 2;
    target->drawString("WiFi", padding, row3CenterY);
    
    btnY = row3Y + (rowHeight - buttonH) / 2;
    uint16_t wifiFill = wifiEnabled ? TFT_BLACK : TFT_WHITE;
    uint16_t wifiText = wifiEnabled ? TFT_WHITE : TFT_BLACK;
    drawSettingsButton(screenW - padding - buttonW, btnY, buttonW, buttonH, 
                      wifiEnabled ? "ON" : "OFF", wifiFill, wifiText);

    // --- Sleep Delay ---
    int row4Y = startY + rowHeight * 3;
    int row4CenterY = row4Y + rowHeight / 2;
    target->drawString("Sleep Delay", padding, row4CenterY);
    
    char sleepBuf[16];
    snprintf(sleepBuf, sizeof(sleepBuf), "%d min", lightSleepMinutes);
    target->setTextDatum(textdatum_t::middle_center);
    target->drawString(sleepBuf, screenW - padding - buttonW - 50, row4CenterY);
    target->setTextDatum(textdatum_t::middle_left);
    
    btnY = row4Y + (rowHeight - buttonH) / 2;
    fontMinusX = screenW - padding - buttonW * 2 - 10;
    fontPlusX = screenW - padding - buttonW;
    drawSettingsButton(fontMinusX, btnY, buttonW, buttonH, "-", TFT_WHITE, TFT_BLACK);
    drawSettingsButton(fontPlusX, btnY, buttonW, buttonH, "+", TFT_WHITE, TFT_BLACK);

    // --- WiFi Status ---
    int row5Y = startY + rowHeight * 4;
    int row5CenterY = row5Y + rowHeight / 2;
    target->drawString("WiFi Status", padding, row5CenterY);

    std::string status = wifiEnabled ? "Connecting..." : "WiFi is OFF";
    if (wifiEnabled && wifiManager.isConnected())
    {
        std::string ip = wifiManager.getIpAddress();
        status = "http://" + ip + "/";
    }
    target->setTextSize(1.4f);
    target->setTextDatum(textdatum_t::middle_left);
    target->drawString(status.c_str(), padding + 180, row5CenterY);
    target->setTextSize(1.8f);

    // --- Buzzer (M5PaperS3 only) ---
#ifdef CONFIG_EBOOK_DEVICE_M5PAPERS3
    int row6Y = startY + rowHeight * 5;
    int row6CenterY = row6Y + rowHeight / 2;
    target->drawString("Buzzer", padding, row6CenterY);

    btnY = row6Y + (rowHeight - buttonH) / 2;
    uint16_t buzzerFill = buzzerEnabled ? TFT_BLACK : TFT_WHITE;
    uint16_t buzzerText = buzzerEnabled ? TFT_WHITE : TFT_BLACK;
    drawSettingsButton(screenW - padding - buttonW, btnY, buttonW, buttonH,
                       buzzerEnabled ? "ON" : "OFF", buzzerFill, buzzerText);

    // --- Auto Rotate ---
    int row7Y = startY + rowHeight * 6;
    int row7CenterY = row7Y + rowHeight / 2;
    target->drawString("Auto Rotate", padding, row7CenterY);

    btnY = row7Y + (rowHeight - buttonH) / 2;
    uint16_t rotateFill = autoRotateEnabled ? TFT_BLACK : TFT_WHITE;
    uint16_t rotateText = autoRotateEnabled ? TFT_WHITE : TFT_BLACK;
    drawSettingsButton(screenW - padding - buttonW, btnY, buttonW, buttonH,
                       autoRotateEnabled ? "ON" : "OFF", rotateFill, rotateText);
#endif

    // --- Back Button ---
    int backBtnY = screenH - 70;
    int backBtnW = 200;
    int backBtnX = (screenW - backBtnW) / 2;
    drawSettingsButton(backBtnX, backBtnY, backBtnW, 50, "Back to Menu", TFT_WHITE, TFT_BLACK);

    target->setTextDatum(textdatum_t::top_left);

    if (sprite)
    {
        sprite->pushSprite(&M5.Display, 0, 0);
    }

    M5.Display.display();
}

void GUI::onSettingsClick(int x, int y)
{
    // Handle clicks on standalone settings page
    int screenW = M5.Display.width();
    int screenH = M5.Display.height();
    int padding = 20;
    int rowHeight = 60;
    int buttonH = 45;
    int buttonW = 140;
    int startY = STATUS_BAR_HEIGHT + 70;

    // Font Size buttons
    int row1Y = startY;
    int fontMinusX = screenW - padding - buttonW * 2 - 10;
    int fontPlusX = screenW - padding - buttonW;
    int btnY = row1Y + (rowHeight - buttonH) / 2;

    if (y >= btnY && y <= btnY + buttonH)
    {
        if (x >= fontMinusX && x < fontMinusX + buttonW)
        {
            setFontSize(fontSize - 0.1f);
            return;
        }
        if (x >= fontPlusX && x < fontPlusX + buttonW)
        {
            setFontSize(fontSize + 0.1f);
            return;
        }
    }

    // Font Change button
    int row2Y = startY + rowHeight;
    btnY = row2Y + (rowHeight - buttonH) / 2;
    if (y >= btnY && y <= btnY + buttonH && x >= screenW - padding - buttonW && x < screenW - padding)
    {
        if (currentFont == "Default")
            setFont("Hebrew");
        else if (currentFont == "Hebrew")
            setFont("Arabic");
        else if (currentFont == "Arabic")
            setFont("Roboto");
        else
            setFont("Default");
        return;
    }

    // WiFi Toggle button
    int row3Y = startY + rowHeight * 2;
    btnY = row3Y + (rowHeight - buttonH) / 2;
    if (y >= btnY && y <= btnY + buttonH && x >= screenW - padding - buttonW && x < screenW - padding)
    {
        wifiEnabled = !wifiEnabled;
        saveSettings();

        if (wifiEnabled)
        {
            if (!wifiManager.isInitialized())
            {
                wifiManager.init();
            }
            wifiManager.connect();
            webServer.init("/spiffs");
        }
        else
        {
            webServer.stop();
            wifiManager.disconnect();
            wifiConnected = false;
        }
        needsRedraw = true;
        return;
    }

    // Sleep Delay buttons
    int row4Y = startY + rowHeight * 3;
    btnY = row4Y + (rowHeight - buttonH) / 2;
    if (y >= btnY && y <= btnY + buttonH)
    {
        if (x >= fontMinusX && x < fontMinusX + buttonW)
        {
            if (lightSleepMinutes > 1) lightSleepMinutes--;
            saveSettings();
            needsRedraw = true;
            return;
        }
        if (x >= fontPlusX && x < fontPlusX + buttonW)
        {
            if (lightSleepMinutes < 60) lightSleepMinutes++;
            saveSettings();
            needsRedraw = true;
            return;
        }
    }

#ifdef CONFIG_EBOOK_DEVICE_M5PAPERS3
    // Buzzer Toggle
    int row6Y = startY + rowHeight * 5;
    btnY = row6Y + (rowHeight - buttonH) / 2;
    if (y >= btnY && y <= btnY + buttonH && x >= screenW - padding - buttonW && x < screenW - padding)
    {
        setBuzzerEnabled(!buzzerEnabled);
        needsRedraw = true;
        return;
    }

    // Auto Rotate Toggle
    int row7Y = startY + rowHeight * 6;
    btnY = row7Y + (rowHeight - buttonH) / 2;
    if (y >= btnY && y <= btnY + buttonH && x >= screenW - padding - buttonW && x < screenW - padding)
    {
        setAutoRotateEnabled(!autoRotateEnabled);
        needsRedraw = true;
        return;
    }
#endif

    // Back button
    int backBtnY = screenH - 70;
    int backBtnW = 200;
    int backBtnX = (screenW - backBtnW) / 2;
    if (y >= backBtnY && y <= backBtnY + 50 && x >= backBtnX && x < backBtnX + backBtnW)
    {
        currentState = AppState::MAIN_MENU;
        needsRedraw = true;
        return;
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
    // First, check if tap is on a rendered image
    for (const auto &ri : pageRenderedImages)
    {
        if (x >= ri.x && x <= ri.x + ri.width &&
            y >= ri.y && y <= ri.y + ri.height)
        {
            // Tapped on an image - open full screen viewer
            ESP_LOGI(TAG, "Tap on image at (%d, %d), opening viewer", x, y);
            if (openImageViewer(ri.image))
            {
                return; // Image viewer opened, don't process as page turn
            }
        }
    }

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

    if (isDouble)
    {
        ESP_LOGI(TAG, "Double click detected! Next=%d", next);
        abortRender = true;
        bool changed = false;
        if (xSemaphoreTake(epubMutex, portMAX_DELAY))
        {
            if (next)
            {
                changed = epubLoader.nextChapter();
            }
            else
            {
                changed = epubLoader.prevChapter();
            }
            xSemaphoreGive(epubMutex);
        }

        if (changed)
        {
            currentTextOffset = 0;
            pageHistory.clear();
            resetPageInfoCache();
            needsRedraw = true;
            // Save progress on chapter change
            saveLastBook();
        }
    }
    else
    {
        // Single Click - Page Turn
        if (!next)
        {
            // Prev Page
            if (pageHistory.empty())
            {
                // Try prev chapter
                bool changed = false;
                abortRender = true;
                if (xSemaphoreTake(epubMutex, portMAX_DELAY))
                {
                    changed = epubLoader.prevChapter();
                    xSemaphoreGive(epubMutex);
                }

                if (changed)
                {
                    // We want to go to the LAST page of the previous chapter.
                    // To do this, we must simulate paging through the entire chapter
                    // to build the pageHistory and find the last offset.
                    size_t chapterSize = 0;
                    if (xSemaphoreTake(epubMutex, portMAX_DELAY))
                    {
                        chapterSize = epubLoader.getChapterSize();
                        xSemaphoreGive(epubMutex);
                    }

                    if (chapterSize == 0)
                    {
                        currentTextOffset = 0;
                        pageHistory.clear();
                    }
                    else
                    {
                        size_t scanOffset = 0;
                        std::vector<size_t> newHistory;

                        // Scan chapter to find page breaks
                        while (scanOffset < chapterSize)
                        {
                            size_t chars = drawPageContentAt(scanOffset, false);
                            if (chars == 0)
                            {
                                // Should not happen unless error, fallback to start
                                currentTextOffset = 0;
                                newHistory.clear();
                                break;
                            }

                            if (scanOffset + chars >= chapterSize)
                            {
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
            // Use cached char count if available to avoid re-measuring
            size_t charsOnPage = lastPageChars;
            if (charsOnPage == 0)
            {
                charsOnPage = drawPageContent(false);
            }

            size_t chSize = 0;
            if (xSemaphoreTake(epubMutex, portMAX_DELAY))
            {
                chSize = epubLoader.getChapterSize();
                xSemaphoreGive(epubMutex);
            }

            if (currentTextOffset + charsOnPage >= chSize)
            {
                // Next chapter
                bool changed = false;
                abortRender = true;
                if (xSemaphoreTake(epubMutex, portMAX_DELAY))
                {
                    changed = epubLoader.nextChapter();
                    xSemaphoreGive(epubMutex);
                }

                if (changed)
                {
                    currentTextOffset = 0;
                    pageHistory.clear();
                    resetPageInfoCache();
                    needsRedraw = true;
                    // Save progress on chapter change
                    saveLastBook();
                }
            }
            else
            {
                pageHistory.push_back(currentTextOffset);
                currentTextOffset += charsOnPage;
                needsRedraw = true;
                // Save progress every page turn (for robust last book tracking)
                saveLastBook();
            }
        }
    }
}

void GUI::onMainMenuClick(int x, int y)
{
    int screenW = M5.Display.width();
    int screenH = M5.Display.height();

    // Calculate grid dimensions (same as drawMainMenu)
    int availableH = screenH - STATUS_BAR_HEIGHT - 20;
    int availableW = screenW - 20;

    int cols = 2;
    int rows = 3;
    int btnGap = 12;
    int btnW = (availableW - btnGap) / cols;
    int btnH = (availableH - btnGap * 2) / rows;

    int startX = 10;
    int startY = STATUS_BAR_HEIGHT + 10;

    // Find which button was clicked
    for (int i = 0; i < 6; i++)
    {
        int row = i / cols;
        int col = i % cols;

        int bx = startX + col * (btnW + btnGap);
        int by = startY + row * (btnH + btnGap);

        if (x >= bx && x < bx + btnW && y >= by && y < by + btnH)
        {
            ESP_LOGI(TAG, "Main menu button %d clicked", i);

            switch (i)
            {
            case 0: // Last Book
                if (lastBookId > 0)
                {
                    loadLastBook();
                }
                break;

            case 1: // Books List
                currentState = AppState::LIBRARY;
                searchQuery.clear();
                showFavoritesOnly = false;
                libraryPage = 0;
                needsRedraw = true;
                break;

            case 2: // WiFi
                currentState = AppState::WIFI_SCAN;
                wifiList.clear();
                needsRedraw = true;
                break;

            case 3: // Settings
                currentState = AppState::SETTINGS;
                needsRedraw = true;
                break;

            case 4: // Favorites
                currentState = AppState::FAVORITES;
                searchQuery.clear();
                showFavoritesOnly = true; // Force favorites filter
                libraryPage = 0;
                needsRedraw = true;
                break;

            case 5: // More (Games menu)
                currentState = AppState::GAMES_MENU;
                needsRedraw = true;
                break;
            }
            return;
        }
    }
}

void GUI::handleGesture(const GestureEvent& event)
{
    int screenH = M5.Display.height();
    
    switch (event.type)
    {
    case GestureType::TAP:
        if (currentState != AppState::MUSIC_COMPOSER)
        {
            handleTap(event.endX, event.endY);
        }
        break;

    case GestureType::LONG_PRESS:
        if (currentState == AppState::MUSIC_COMPOSER)
        {
            if (ComposerUI::getInstance().handleLongPress(event.endX, event.endY))
            {
                needsRedraw = true;
            }
        }
        break;

    case GestureType::DOUBLE_TAP:
        if (currentState == AppState::READER)
        {
            processReaderTap(event.endX, event.endY, true); // true = double
        }
        else if (currentState == AppState::MUSIC_COMPOSER)
        {
            if (ComposerUI::getInstance().handleDoubleTap(event.endX, event.endY))
            {
                needsRedraw = true;
            }
        }
        else
        {
            // For other modes, treat double tap as a tap
            handleTap(event.endX, event.endY);
        }
        break;

    case GestureType::SWIPE_LEFT:
        // Handle swipe gestures in reader mode - left swipe opens chapter menu
        if (currentState == AppState::READER)
        {
            // Swipe in middle area opens chapter menu
            if (event.startY > STATUS_BAR_HEIGHT + 30 && event.startY < screenH - 80)
            {
                ESP_LOGI(TAG, "Swipe left detected - opening chapter menu");
                // Play short beep for feedback
                deviceHAL.playTone(1500, 50);
                previousState = currentState;
                currentState = AppState::CHAPTER_MENU;
                chapterMenuScrollOffset = 0;
                needsRedraw = true;
            }
        }
        else if (currentState == AppState::MUSIC_COMPOSER)
        {
            if (ComposerUI::getInstance().handleGesture(event))
            {
                needsRedraw = true;
            }
        }
        break;

    case GestureType::SWIPE_UP:
        // Swipe up from bottom area opens settings in reader mode
        if (currentState == AppState::READER)
        {
            if (event.startY > screenH - 150)  // Must start from bottom 150px
            {
                ESP_LOGI(TAG, "Swipe up detected - opening book settings");
                // Play short beep for feedback
                deviceHAL.playTone(1500, 50);
                previousState = currentState;
                currentState = AppState::BOOK_SETTINGS;
                needsRedraw = true;
            }
        }
        else if (currentState == AppState::MUSIC_COMPOSER)
        {
            if (ComposerUI::getInstance().handleGesture(event))
            {
                needsRedraw = true;
            }
        }
        break;
        
    case GestureType::SWIPE_RIGHT:
        // Swipe right in chapter menu returns to reader
        if (currentState == AppState::CHAPTER_MENU)
        {
            currentState = AppState::READER;
            needsRedraw = true;
        }
        else if (currentState == AppState::MUSIC_COMPOSER)
        {
            if (ComposerUI::getInstance().handleGesture(event))
            {
                needsRedraw = true;
            }
        }
        break;
        
    case GestureType::SWIPE_DOWN:
        if (currentState == AppState::MUSIC_COMPOSER)
        {
            if (ComposerUI::getInstance().handleGesture(event))
            {
                needsRedraw = true;
            }
        }
        break;
        
    default:
        break;
    }
}

void GUI::handleTap(int x, int y)
{
    if (justWokeUp)
    {
        ESP_LOGI(TAG, "Processing wake-up tap at %d, %d", x, y);
    }

    // Handle main menu
    if (currentState == AppState::MAIN_MENU)
    {
        onMainMenuClick(x, y);
        justWokeUp = false;
    }
    else if (currentState == AppState::LIBRARY || currentState == AppState::FAVORITES)
    {
        // Status bar tap returns to main menu
        if (y < STATUS_BAR_HEIGHT)
        {
            currentState = AppState::MAIN_MENU;
            searchQuery.clear();
            showFavoritesOnly = false;
            libraryPage = 0;
            needsRedraw = true;
            justWokeUp = false;
            return;
        }
        onLibraryClick(x, y);
        justWokeUp = false;
    }
    else if (currentState == AppState::READER)
    {
        // Top tap (status bar) to return to main menu
        if (y < STATUS_BAR_HEIGHT)
        {
            clickPending = false; // Cancel any pending
            // Save progress before exiting
            if (xSemaphoreTake(epubMutex, portMAX_DELAY))
            {
                xSemaphoreGive(epubMutex);
            }
            // Save progress before exiting using centralized helper
            saveLastBook();
            bookIndex.save(); // Save index to disk on exit (Issue 1)

            currentState = AppState::MAIN_MENU;
            epubLoader.close();
            bookMetricsComputed = false;
            totalBookChars = 0;
            chapterPrefixSums.clear();
            needsRedraw = true;
            justWokeUp = false;
            return;
        }

        // Bottom status bar area (footer) -> Book Settings overlay
        int footerY = M5.Display.height() - 50;
        if (y > footerY)
        {
            clickPending = false; // Cancel any pending
            previousState = currentState;
            currentState = AppState::BOOK_SETTINGS;
            needsRedraw = true;
            justWokeUp = false;
            return;
        }

        // Middle area - Page Turn
        // Single tap (GestureDetector handles double tap separation)
        processReaderTap(x, y, false); 
        justWokeUp = false;
    }
    else if (currentState == AppState::BOOK_SETTINGS)
    {
        // In-book settings overlay (half-page)
        SettingsLayout layout = computeSettingsLayout();

        // Close if clicked outside the panel (upper half)
        if (y < layout.panelTop)
        {
            currentState = previousState;
            needsRedraw = true;
            if (settingsCanvasCreated)
            {
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
        if (y >= fontButtonY && y <= fontButtonY + layout.fontButtonH && x >= layout.fontMinusX && x <= layout.fontMinusX + layout.fontButtonW)
        {
            setFontSize(fontSize - 0.1f);
            settingsNeedsUnderlayRefresh = true;
        }
        // Font Size [+]
        else if (y >= fontButtonY && y <= fontButtonY + layout.fontButtonH && x >= layout.fontPlusX && x <= layout.fontPlusX + layout.fontButtonW)
        {
            setFontSize(fontSize + 0.1f);
            settingsNeedsUnderlayRefresh = true;
        }
        // Font Change
        else if (y >= changeButtonY && y <= changeButtonY + layout.fontButtonH && x >= layout.changeButtonX && x <= layout.changeButtonX + layout.changeButtonW)
        {
            if (currentFont == "Default")
                setFont("Hebrew");
            else if (currentFont == "Hebrew")
                setFont("Arabic");
            else if (currentFont == "Arabic")
                setFont("Roboto");
            else
                setFont("Default");
            settingsNeedsUnderlayRefresh = true;
        }
        // WiFi Toggle
        else if (y >= toggleButtonY && y <= toggleButtonY + layout.fontButtonH && x >= layout.toggleButtonX && x <= layout.toggleButtonX + layout.toggleButtonW)
        {
            wifiEnabled = !wifiEnabled;
            saveSettings();

            if (wifiEnabled)
            {
                if (!wifiManager.isInitialized())
                {
                    wifiManager.init();
                }
                wifiManager.connect();
                webServer.init("/spiffs");
            }
            else
            {
                webServer.stop();
                wifiManager.disconnect();
                wifiConnected = false;
            }
            needsRedraw = true;
        }
        // Sleep Delay [-]
        else if (y >= layout.row5Y && y <= layout.row5Y + layout.rowHeight && x >= layout.fontMinusX && x <= layout.fontMinusX + layout.fontButtonW)
        {
            if (lightSleepMinutes > 1) lightSleepMinutes--;
            saveSettings();
            settingsNeedsUnderlayRefresh = true;
        }
        // Sleep Delay [+]
        else if (y >= layout.row5Y && y <= layout.row5Y + layout.rowHeight && x >= layout.fontPlusX && x <= layout.fontPlusX + layout.fontButtonW)
        {
            if (lightSleepMinutes < 60) lightSleepMinutes++;
            saveSettings();
            settingsNeedsUnderlayRefresh = true;
        }
        // Close
        else if (y >= closeButtonY && y <= closeButtonY + layout.closeButtonH && x >= closeX && x <= closeX + layout.closeButtonW)
        {
            currentState = previousState;
            needsRedraw = true;
            if (settingsCanvasCreated)
            {
                settingsCanvas.deleteSprite();
                settingsCanvasCreated = false;
            }
        }
    }
    else if (currentState == AppState::SETTINGS)
    {
        // Standalone settings (from main menu)
        // Status bar tap returns to main menu
        if (y < STATUS_BAR_HEIGHT)
        {
            currentState = AppState::MAIN_MENU;
            needsRedraw = true;
            justWokeUp = false;
            return;
        }
        onSettingsClick(x, y);
        justWokeUp = false;
    }
    else if (currentState == AppState::WIFI_SCAN)
    {
        // Status bar tap returns to main menu
        if (y < STATUS_BAR_HEIGHT)
        {
            currentState = AppState::MAIN_MENU;
            needsRedraw = true;
            justWokeUp = false;
            return;
        }
        onWifiScanClick(x, y);
        justWokeUp = false;
    }
    else if (currentState == AppState::WIFI_PASSWORD)
    {
        onWifiPasswordClick(x, y);
        justWokeUp = false;
    }
    else if (currentState == AppState::GAMES_MENU)
    {
        onGamesMenuClick(x, y);
        justWokeUp = false;
    }
    else if (currentState == AppState::GAME_PLAYING)
    {
        // Let game manager handle the touch
        GameManager &gm = GameManager::getInstance();
        if (gm.handleTouch(x, y))
        {
            needsRedraw = true;
        }
        // Check if game wants to return to menu
        if (gm.shouldReturnToMenu())
        {
            gm.clearReturnFlag();
            currentState = AppState::GAMES_MENU;
            needsRedraw = true;
        }
        justWokeUp = false;
    }
    else if (currentState == AppState::IMAGE_VIEWER)
    {
        onImageViewerClick(x, y);
        justWokeUp = false;
    }
    else if (currentState == AppState::CHAPTER_MENU)
    {
        onChapterMenuClick(x, y);
        justWokeUp = false;
    }
    else if (currentState == AppState::FONT_SELECTION)
    {
        onFontSelectionClick(x, y);
        justWokeUp = false;
    }
    else if (currentState == AppState::MUSIC_COMPOSER)
    {
        onMusicComposerClick(x, y);
        justWokeUp = false;
    }
}

void GUI::handleTouch()
{
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);

    // Check pending click timeout - reduced from 400ms to 150ms for faster response
    if (clickPending && (now - lastClickTime > 150))
    {
        clickPending = false;
        // Execute Single Click (Page Turn)
        processReaderTap(lastClickX, lastClickY, false); // false = single
    }

    // Check orientation periodically for auto-rotate (M5PaperS3)
#ifdef CONFIG_EBOOK_DEVICE_M5PAPERS3
    if (deviceHAL.isAutoRotateEnabled() &&
        (now - lastOrientationCheck > ORIENTATION_CHECK_INTERVAL_MS))
    {
        lastOrientationCheck = now;
        if (deviceHAL.updateOrientation())
        {
            // Rotation changed - need to redraw
            needsRedraw = true;
            // Invalidate pre-rendered canvases
            nextCanvasValid = false;
            prevCanvasValid = false;
        }
    }
#endif

    if (M5.Touch.getCount() > 0)
    {
        lastActivityTime = now;
        auto t = M5.Touch.getDetail(0);
        
        if (currentState == AppState::MUSIC_COMPOSER)
        {
            if (t.wasPressed() || (justWokeUp && t.isPressed()))
            {
#ifdef CONFIG_EBOOK_S3_ENABLE_BUZZER
                deviceHAL.playClickSound();
#endif
                ComposerUI::getInstance().handleDragStart(t.x, t.y);
            }
            
            if (t.isPressed())
            {
                ComposerUI::getInstance().handleDragMove(t.x, t.y);
            }
            
            if (t.wasReleased())
            {
                ComposerUI::getInstance().handleDragEnd(t.x, t.y);
                if (ComposerUI::getInstance().shouldExit())
                {
                    ComposerUI::getInstance().clearExitFlag();
                    ComposerUI::getInstance().exit();
                    currentState = AppState::GAMES_MENU;
                    needsRedraw = true;
                }
            }
            
            justWokeUp = false;
            return;
        }

        if (t.wasPressed() || (justWokeUp && t.isPressed()))
        {
            // Play click sound immediately on touch for instant feedback
#ifdef CONFIG_EBOOK_S3_ENABLE_BUZZER
            deviceHAL.playClickSound();
#endif

            // If gesture is in progress, don't process taps yet
            // UNLESS we are in a mode that requires instant response (non-Reader)
            // For Music Composer, we want both instant response for notes AND gestures for scrolling
            bool instantResponse = (currentState != AppState::READER && currentState != AppState::MUSIC_COMPOSER);

            if (gestureDetector.isGestureInProgress() && !instantResponse)
            {
                // For Music Composer, we still want to allow the touch to be processed by the composer
                // but we don't want to reset the gesture detector.
                if (currentState == AppState::MUSIC_COMPOSER) {
                    onMusicComposerClick(t.x, t.y);
                    justWokeUp = false;
                    return;
                }
                return;
            }

            if (instantResponse)
            {
                gestureDetector.reset();
            }

            if (justWokeUp)
            {
                ESP_LOGI(TAG, "Processing wake-up touch at %d, %d", t.x, t.y);
            }

            // Handle main menu
            if (currentState == AppState::MAIN_MENU)
            {
                onMainMenuClick(t.x, t.y);
                justWokeUp = false;
            }
            else if (currentState == AppState::LIBRARY || currentState == AppState::FAVORITES)
            {
                // Status bar tap returns to main menu
                if (t.y < STATUS_BAR_HEIGHT)
                {
                    currentState = AppState::MAIN_MENU;
                    searchQuery.clear();
                    showFavoritesOnly = false;
                    libraryPage = 0;
                    needsRedraw = true;
                    justWokeUp = false;
                    return;
                }
                onLibraryClick(t.x, t.y);
                justWokeUp = false;
            }
            else if (currentState == AppState::READER)
            {
                // Top tap (status bar) to return to main menu
                if (t.y < STATUS_BAR_HEIGHT)
                {
                    clickPending = false; // Cancel any pending
                    // Save progress before exiting
                    if (xSemaphoreTake(epubMutex, portMAX_DELAY))
                    {
                        xSemaphoreGive(epubMutex);
                    }
                    // Save progress before exiting using centralized helper
                    saveLastBook();

                    currentState = AppState::MAIN_MENU;
                    epubLoader.close();
                    bookMetricsComputed = false;
                    totalBookChars = 0;
                    chapterPrefixSums.clear();
                    needsRedraw = true;
                    justWokeUp = false;
                    return;
                }

                // Bottom status bar area (footer) -> Book Settings overlay
                // Changed from bottom 1/5 to specifically the footer area (last ~50px)
                int footerY = M5.Display.height() - 50;
                if (t.y > footerY)
                {
                    clickPending = false; // Cancel any pending
                    previousState = currentState;
                    currentState = AppState::BOOK_SETTINGS;
                    needsRedraw = true;
                    justWokeUp = false;
                    return;
                }

                // Middle area - Page Turn / Chapter Skip
                // When waking up from sleep, process the touch immediately as a page turn
                // to avoid re-rendering the current page first
                if (justWokeUp)
                {
                    clickPending = false;
                    processReaderTap(t.x, t.y, false); // Single tap = page turn
                }
                else if (clickPending)
                {
                    // Check if second click
                    bool sameSide = (t.x < M5.Display.width() / 2) == (lastClickX < M5.Display.width() / 2);
                    if (sameSide)
                    {
                        // Double Click!
                        clickPending = false;
                        processReaderTap(t.x, t.y, true); // true = double
                    }
                    else
                    {
                        // Different side - treat previous as single, and this as new pending
                        clickPending = false;
                        processReaderTap(lastClickX, lastClickY, false);

                        lastClickTime = now;
                        lastClickX = t.x;
                        lastClickY = t.y;
                        clickPending = true;
                    }
                }
                else
                {
                    // First click
                    lastClickTime = now;
                    lastClickX = t.x;
                    lastClickY = t.y;
                    clickPending = true;
                }
            }
            else if (currentState == AppState::BOOK_SETTINGS)
            {
                // In-book settings overlay (half-page)
                SettingsLayout layout = computeSettingsLayout();

                // Close if clicked outside the panel (upper half)
                if (t.y < layout.panelTop)
                {
                    currentState = previousState;
                    needsRedraw = true;
                    if (settingsCanvasCreated)
                    {
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
                    if (currentFont == "Default")
                        setFont("Hebrew");
                    else if (currentFont == "Hebrew")
                        setFont("Arabic");
                    else if (currentFont == "Arabic")
                        setFont("Roboto");
                    else
                        setFont("Default");
                    settingsNeedsUnderlayRefresh = true;
                }
                // WiFi Toggle
                else if (t.y >= toggleButtonY && t.y <= toggleButtonY + layout.fontButtonH && t.x >= layout.toggleButtonX && t.x <= layout.toggleButtonX + layout.toggleButtonW)
                {
                    wifiEnabled = !wifiEnabled;
                    saveSettings();

                    if (wifiEnabled)
                    {
                        if (!wifiManager.isInitialized())
                        {
                            wifiManager.init();
                        }
                        wifiManager.connect();
                        webServer.init("/spiffs");
                    }
                    else
                    {
                        webServer.stop();
                        wifiManager.disconnect();
                        wifiConnected = false;
                    }
                    needsRedraw = true;
                }
                // Sleep Delay [-]
                else if (t.y >= layout.row5Y && t.y <= layout.row5Y + layout.rowHeight && t.x >= layout.fontMinusX && t.x <= layout.fontMinusX + layout.fontButtonW)
                {
                    if (lightSleepMinutes > 1) lightSleepMinutes--;
                    saveSettings();
                    settingsNeedsUnderlayRefresh = true;
                }
                // Sleep Delay [+]
                else if (t.y >= layout.row5Y && t.y <= layout.row5Y + layout.rowHeight && t.x >= layout.fontPlusX && t.x <= layout.fontPlusX + layout.fontButtonW)
                {
                    if (lightSleepMinutes < 60) lightSleepMinutes++;
                    saveSettings();
                    settingsNeedsUnderlayRefresh = true;
                }
                // Favorite Toggle
                else if (t.y >= layout.row6Y && t.y <= layout.row6Y + layout.rowHeight && t.x >= layout.toggleButtonX && t.x <= layout.toggleButtonX + layout.toggleButtonW)
                {
                    bool isFav = bookIndex.isFavorite(lastBookId);
                    bookIndex.setFavorite(lastBookId, !isFav);
                    settingsNeedsUnderlayRefresh = true;
                }
                // Close
                else if (t.y >= closeButtonY && t.y <= closeButtonY + layout.closeButtonH && t.x >= closeX && t.x <= closeX + layout.closeButtonW)
                {
                    currentState = previousState;
                    needsRedraw = true;
                    if (settingsCanvasCreated)
                    {
                        settingsCanvas.deleteSprite();
                        settingsCanvasCreated = false;
                    }
                }
            }
            else if (currentState == AppState::SETTINGS)
            {
                // Standalone settings (from main menu)
                // Status bar tap returns to main menu
                if (t.y < STATUS_BAR_HEIGHT)
                {
                    currentState = AppState::MAIN_MENU;
                    needsRedraw = true;
                    justWokeUp = false;
                    return;
                }
                onSettingsClick(t.x, t.y);
                justWokeUp = false;
            }
            else if (currentState == AppState::WIFI_SCAN)
            {
                // Status bar tap returns to main menu
                if (t.y < STATUS_BAR_HEIGHT)
                {
                    currentState = AppState::MAIN_MENU;
                    needsRedraw = true;
                    justWokeUp = false;
                    return;
                }
                onWifiScanClick(t.x, t.y);
                justWokeUp = false;
            }
            else if (currentState == AppState::WIFI_PASSWORD)
            {
                onWifiPasswordClick(t.x, t.y);
                justWokeUp = false;
            }
            else if (currentState == AppState::GAMES_MENU)
            {
                onGamesMenuClick(t.x, t.y);
                justWokeUp = false;
            }
            else if (currentState == AppState::GAME_PLAYING)
            {
                // Let game manager handle the touch
                GameManager &gm = GameManager::getInstance();
                if (gm.handleTouch(t.x, t.y))
                {
                    needsRedraw = true;
                }
                // Check if game wants to return to menu
                if (gm.shouldReturnToMenu())
                {
                    gm.clearReturnFlag();
                    currentState = AppState::GAMES_MENU;
                    needsRedraw = true;
                }
                justWokeUp = false;
            }
            else if (currentState == AppState::CHAPTER_MENU)
            {
                onChapterMenuClick(t.x, t.y);
                justWokeUp = false;
            }
            else if (currentState == AppState::IMAGE_VIEWER)
            {
                onImageViewerClick(t.x, t.y);
                justWokeUp = false;
            }
            else if (currentState == AppState::MUSIC_COMPOSER)
            {
                onMusicComposerClick(t.x, t.y);
                justWokeUp = false;
            }
        }
    }
    justWokeUp = false;
}

void GUI::jumpTo(float percent)
{
    ESP_LOGI(TAG, "GUI::jumpTo called with percent=%.2f (state=%d)", percent, (int)currentState);
    if (currentState != AppState::READER)
    {
        ESP_LOGW(TAG, "jumpTo ignored - not in READER state");
        return;
    }

    size_t size = 0;
    if (xSemaphoreTake(epubMutex, portMAX_DELAY))
    {
        size = epubLoader.getChapterSize();
        xSemaphoreGive(epubMutex);
    }

    if (size == 0)
    {
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
    if (currentState != AppState::READER)
    {
        ESP_LOGW(TAG, "jumpToChapter ignored - not in READER state");
        return;
    }

    // Abort any background rendering
    abortRender = true;

    bool success = false;
    if (xSemaphoreTake(epubMutex, portMAX_DELAY))
    {
        success = epubLoader.jumpToChapter(chapter);
        xSemaphoreGive(epubMutex);
    }

    if (success)
    {
        ESP_LOGI(TAG, "jumpToChapter: loaded chapter %d size=%zu", chapter, epubLoader.getChapterSize());
        currentTextOffset = 0;
        pageHistory.clear();
        resetPageInfoCache();
        needsRedraw = true;
    }
    else
    {
        ESP_LOGW(TAG, "jumpToChapter: failed to load chapter %d", chapter);
    }
}

bool GUI::openBookById(int id)
{
    if (!bookIndexReady)
        return false;

    BookEntry book = bookIndex.getBook(id);
    if (book.id == 0)
        return false;

    bookMetricsComputed = false;
    totalBookChars = 0;
    chapterPrefixSums.clear();

    // Stop any background rendering
    abortRender = true;
    bookOpenInProgress = true;

    // Lock mutex for the entire loading process
    if (!xSemaphoreTake(epubMutex, portMAX_DELAY))
    {
        bookOpenInProgress = false;
        return false;
    }

    if (!epubLoader.load(book.path.c_str()))
    {
        xSemaphoreGive(epubMutex);
        bookOpenInProgress = false;
        return false;
    }

    currentBook = book;
    currentState = AppState::READER;

    // Auto-detect language for font
    std::string lang = epubLoader.getLanguage();
    bool isHebrew = (lang.find("he") != std::string::npos || lang.find("HE") != std::string::npos);

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

    if (bookIndex.loadBookMetrics(currentBook.id, totalBookChars, chapterPrefixSums))
    {
        bookMetricsComputed = true;
        ESP_LOGI(TAG, "Loaded cached metrics for book %d", currentBook.id);
    }
    else
    {
        // Kill any existing metrics task (e.g. from previous book)
        if (metricsTaskHandle != nullptr)
        {
            vTaskDelete(metricsTaskHandle);
            metricsTaskHandle = nullptr;
        }
        // Spawn new task
        metricsTaskTargetBookId = currentBook.id;
        xTaskCreate([](void *arg)
                    { static_cast<GUI *>(arg)->metricsTaskLoop(); }, "MetricsTask", 4096, this, 0, &metricsTaskHandle);
    }

    // Release mutex
    xSemaphoreGive(epubMutex);
    bookOpenInProgress = false;

    // Restore book-specific font settings if available
    std::string bookFont;
    float bookFontSize;
    if (bookIndex.getBookFont(id, bookFont, bookFontSize))
    {
        if (!bookFont.empty())
        {
            setFont(bookFont);
            fontSize = bookFontSize;
        }
    }
    else
    {
        // No saved font for this book - set default based on language
        if (isHebrew)
        {
            setFont("Hebrew");
            // Use Hebrew font's saved size or default 2.5
            if (fontSizes.count("Hebrew") > 0)
            {
                fontSize = fontSizes["Hebrew"];
            }
            else
            {
                fontSize = 2.5f;
                fontSizes["Hebrew"] = 2.5f;
            }
        }
        else
        {
            // Default to Roboto with size 2.5 for new books
            setFont("Roboto");
            if (fontSizes.count("Roboto") > 0)
            {
                fontSize = fontSizes["Roboto"];
            }
            else
            {
                fontSize = 2.5f;
                fontSizes["Roboto"] = 2.5f;
            }
        }
    }

    pageHistory.clear();
    needsRedraw = true;

    // Save as last book immediately
    saveLastBook();

    return true;
}

void GUI::refreshLibrary()
{
    if (currentState == AppState::LIBRARY)
    {
        needsRedraw = true;
    }

    // Trigger background indexer if not running to process any new books
    if (backgroundIndexerTaskHandle == nullptr)
    {
        xTaskCreate([](void *arg)
                    { static_cast<GUI *>(arg)->backgroundIndexerTaskLoop(); }, "BgIndexer", 8192, this, 0, &backgroundIndexerTaskHandle);
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
        nvs_set_i32(my_handle, "show_images", showImages ? 1 : 0);
        int32_t spacingInt = (int32_t)(lineSpacing * 10);
        nvs_set_i32(my_handle, "line_spacing", spacingInt);

        // Save text boldness
        nvs_set_i32(my_handle, "text_bold", textBoldness);

        // Save per-font size settings
        std::string fontSizeStr;
        for (const auto &entry : fontSizes)
        {
            if (!fontSizeStr.empty())
                fontSizeStr += ",";
            char buf[32];
            snprintf(buf, sizeof(buf), "%s:%.1f", entry.first.c_str(), entry.second);
            fontSizeStr += buf;
        }
        if (!fontSizeStr.empty())
        {
            nvs_set_str(my_handle, "font_sizes", fontSizeStr.c_str());
        }

        // Save light sleep duration
        nvs_set_i32(my_handle, "sleep_min", lightSleepMinutes);

#ifdef CONFIG_EBOOK_DEVICE_M5PAPERS3
        // Save M5PaperS3-specific settings
        nvs_set_i32(my_handle, "buzzer_en", buzzerEnabled ? 1 : 0);
        nvs_set_i32(my_handle, "auto_rotate", autoRotateEnabled ? 1 : 0);
#endif

        nvs_commit(my_handle);
        nvs_close(my_handle);
    }
}

// M5PaperS3 buzzer and auto-rotate methods
void GUI::setBuzzerEnabled(bool enabled)
{
#ifdef CONFIG_EBOOK_DEVICE_M5PAPERS3
    buzzerEnabled = enabled;
    deviceHAL.setBuzzerEnabled(enabled);
    saveSettings();
#else
    (void)enabled;
#endif
}

bool GUI::isBuzzerEnabled() const
{
#ifdef CONFIG_EBOOK_DEVICE_M5PAPERS3
    return buzzerEnabled;
#else
    return false;
#endif
}

void GUI::setAutoRotateEnabled(bool enabled)
{
#ifdef CONFIG_EBOOK_DEVICE_M5PAPERS3
    autoRotateEnabled = enabled;
    deviceHAL.setAutoRotateEnabled(enabled);
    saveSettings();
#else
    (void)enabled;
#endif
}

bool GUI::isAutoRotateEnabled() const
{
#ifdef CONFIG_EBOOK_DEVICE_M5PAPERS3
    return autoRotateEnabled;
#else
    return false;
#endif
}

void GUI::checkOrientation()
{
#ifdef CONFIG_EBOOK_DEVICE_M5PAPERS3
    if (deviceHAL.updateOrientation())
    {
        // Rotation changed
        needsRedraw = true;
        nextCanvasValid = false;
        prevCanvasValid = false;
    }
#endif
}

void GUI::setFontSize(float size)
{
    if (size < 0.5f)
        size = 0.5f;
    if (size > 5.0f)
        size = 5.0f;
    fontSize = size;
    // Store size per font
    fontSizes[currentFont] = size;
    saveSettings();
    // Save book-specific font settings if in reader
    if (currentState == AppState::READER && currentBook.id != 0)
    {
        bookIndex.setBookFont(currentBook.id, currentFont, fontSize);
    }
    resetPageInfoCache();
    needsRedraw = true;
}

float GUI::getFontSize() const
{
    return fontSize;
}

float GUI::getFontSize(const std::string& fontName) const
{
    auto it = fontSizes.find(fontName);
    if (it != fontSizes.end())
    {
        return it->second;
    }
    return 1.0f; // Default
}

void GUI::setLineSpacing(float spacing)
{
    if (spacing < 1.0f)
        spacing = 1.0f;
    if (spacing > 3.0f)
        spacing = 3.0f;
    lineSpacing = spacing;
    saveSettings();
    resetPageInfoCache();
    needsRedraw = true;
}

void GUI::setTextBoldness(int level)
{
    if (level < 0)
        level = 0;
    if (level > 3)
        level = 3;
    textBoldness = level;
    saveSettings();
    needsRedraw = true;
}

void GUI::setFont(const std::string &fontName)
{
    if (currentFont == fontName)
        return;
    currentFont = fontName;
    // Restore per-font size if available
    auto it = fontSizes.find(fontName);
    if (it != fontSizes.end())
    {
        fontSize = it->second;
    }
    saveSettings();
    // Save book-specific font settings if in reader
    if (currentState == AppState::READER && currentBook.id != 0)
    {
        bookIndex.setBookFont(currentBook.id, currentFont, fontSize);
    }
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

        int32_t showImg = 1;
        if (nvs_get_i32(my_handle, "show_images", &showImg) == ESP_OK)
        {
            showImages = (showImg != 0);
        }

        int32_t spacingInt = 11; // Default 1.1
        if (nvs_get_i32(my_handle, "line_spacing", &spacingInt) == ESP_OK)
        {
            lineSpacing = spacingInt / 10.0f;
        }

#ifdef CONFIG_EBOOK_DEVICE_M5PAPERS3
        // Load M5PaperS3-specific settings
        int32_t buzzerEn = 1;
        if (nvs_get_i32(my_handle, "buzzer_en", &buzzerEn) == ESP_OK)
        {
            buzzerEnabled = (buzzerEn != 0);
            deviceHAL.setBuzzerEnabled(buzzerEnabled);
        }

        int32_t autoRotate = 1;
        if (nvs_get_i32(my_handle, "auto_rotate", &autoRotate) == ESP_OK)
        {
            autoRotateEnabled = (autoRotate != 0);
            deviceHAL.setAutoRotateEnabled(autoRotateEnabled);
        }
#endif

        // Load text boldness setting
        int32_t boldInt = 0;
        if (nvs_get_i32(my_handle, "text_bold", &boldInt) == ESP_OK)
        {
            textBoldness = boldInt;
        }

        // Load light sleep duration
        int32_t sleepMin = 5;
        if (nvs_get_i32(my_handle, "sleep_min", &sleepMin) == ESP_OK)
        {
            lightSleepMinutes = sleepMin;
        }

        // Load per-font size settings
        size_t fontsLen = 0;
        if (nvs_get_str(my_handle, "font_sizes", NULL, &fontsLen) == ESP_OK && fontsLen > 1)
        {
            char *fontsStr = new char[fontsLen];
            if (nvs_get_str(my_handle, "font_sizes", fontsStr, &fontsLen) == ESP_OK)
            {
                // Parse format: "fontName:size,fontName:size,..."
                std::string data(fontsStr);
                size_t pos = 0;
                while (pos < data.length())
                {
                    size_t comma = data.find(',', pos);
                    if (comma == std::string::npos)
                        comma = data.length();
                    std::string entry = data.substr(pos, comma - pos);
                    size_t colon = entry.find(':');
                    if (colon != std::string::npos)
                    {
                        std::string fn = entry.substr(0, colon);
                        float sz = atof(entry.substr(colon + 1).c_str());
                        if (!fn.empty() && sz > 0)
                        {
                            fontSizes[fn] = sz;
                        }
                    }
                    pos = comma + 1;
                }
            }
            delete[] fontsStr;
        }

        nvs_close(my_handle);
    }
}

void GUI::saveLastBook()
{
    if (currentBook.id == 0)
        return;

    int chIdx = 0;
    if (xSemaphoreTake(epubMutex, portMAX_DELAY))
    {
        chIdx = epubLoader.getCurrentChapterIndex();
        xSemaphoreGive(epubMutex);
    }

    if (bookIndexReady)
    {
        // Save progress to book index
        bookIndex.updateProgress(currentBook.id, chIdx, currentTextOffset);

        // Save current font settings for this book
        bookIndex.setBookFont(currentBook.id, currentFont, fontSize);
    }
    else
    {
        pendingBookIndexSync = true;
        pendingBookId = currentBook.id;
        pendingBookChapter = chIdx;
        pendingBookOffset = currentTextOffset;
        pendingBookFont = currentFont;
        pendingBookFontSize = fontSize;
    }

    // Save last book ID and state to NVS
    nvs_handle_t my_handle;
    if (nvs_open("storage", NVS_READWRITE, &my_handle) == ESP_OK)
    {
        nvs_set_i32(my_handle, "last_book_id", currentBook.id);
        nvs_set_i32(my_handle, "last_state", (int)currentState);
        nvs_set_str(my_handle, "last_book_path", currentBook.path.c_str());
        nvs_set_str(my_handle, "last_book_title", currentBook.title.c_str());
        nvs_set_i32(my_handle, "last_book_chapter", chIdx);
        nvs_set_u32(my_handle, "last_book_offset", (uint32_t)currentTextOffset);
        nvs_set_str(my_handle, "last_book_font", currentFont.c_str());
        nvs_set_i32(my_handle, "last_book_font_size", (int32_t)(fontSize * 10));
        nvs_commit(my_handle);
        nvs_close(my_handle);
    }

    // Update last book info for main menu display
    lastBookId = currentBook.id;
    lastBookTitle = currentBook.title;
    if (lastBookTitle.length() > 25)
    {
        size_t pos = 22;
        while (pos > 0 && (lastBookTitle[pos] & 0xC0) == 0x80)
            pos--;
        lastBookTitle = lastBookTitle.substr(0, pos) + "...";
    }
}

bool GUI::loadLastBook()
{
    nvs_handle_t my_handle;
    int32_t lastId = -1;
    int32_t lastChapter = 0;
    uint32_t lastOffset = 0;
    std::string lastPath;
    std::string lastTitle;
    std::string lastFont;
    float lastFontSize = 0.0f;
    if (nvs_open("storage", NVS_READONLY, &my_handle) == ESP_OK)
    {
        nvs_get_i32(my_handle, "last_book_id", &lastId);
        nvs_get_i32(my_handle, "last_book_chapter", &lastChapter);
        nvs_get_u32(my_handle, "last_book_offset", &lastOffset);

        size_t pathLen = 0;
        if (nvs_get_str(my_handle, "last_book_path", NULL, &pathLen) == ESP_OK && pathLen > 1)
        {
            char *pathBuf = new char[pathLen];
            if (nvs_get_str(my_handle, "last_book_path", pathBuf, &pathLen) == ESP_OK)
            {
                lastPath = pathBuf;
            }
            delete[] pathBuf;
        }

        size_t titleLen = 0;
        if (nvs_get_str(my_handle, "last_book_title", NULL, &titleLen) == ESP_OK && titleLen > 1)
        {
            char *titleBuf = new char[titleLen];
            if (nvs_get_str(my_handle, "last_book_title", titleBuf, &titleLen) == ESP_OK)
            {
                lastTitle = titleBuf;
            }
            delete[] titleBuf;
        }

        size_t fontLen = 0;
        if (nvs_get_str(my_handle, "last_book_font", NULL, &fontLen) == ESP_OK && fontLen > 1)
        {
            char *fontBuf = new char[fontLen];
            if (nvs_get_str(my_handle, "last_book_font", fontBuf, &fontLen) == ESP_OK)
            {
                lastFont = fontBuf;
            }
            delete[] fontBuf;
        }

        int32_t fontSizeInt = 0;
        if (nvs_get_i32(my_handle, "last_book_font_size", &fontSizeInt) == ESP_OK)
        {
            lastFontSize = fontSizeInt / 10.0f;
        }
        nvs_close(my_handle);
    }

    if (lastId > 0)
    {
        lastBookId = lastId;
        if (!lastTitle.empty())
        {
            lastBookTitle = lastTitle;
            if (lastBookTitle.length() > 25)
            {
                size_t pos = 22;
                while (pos > 0 && (lastBookTitle[pos] & 0xC0) == 0x80)
                    pos--;
                lastBookTitle = lastBookTitle.substr(0, pos) + "...";
            }
        }
    }

    if (lastPath.empty() && lastId > 0)
    {
        char fallback[32];
        snprintf(fallback, sizeof(fallback), "/spiffs/%ld.epub", lastId);
        lastPath = fallback;
    }

    // Check if file exists
    struct stat st;
    if (!lastPath.empty() && stat(lastPath.c_str(), &st) != 0) {
        ESP_LOGW(TAG, "Last book file not found: %s", lastPath.c_str());
        // Try to recover path from ID if index is ready
        if (bookIndexReady && lastId > 0) {
            BookEntry entry = bookIndex.getBook(lastId);
            if (entry.id > 0 && !entry.path.empty()) {
                std::string newPath = entry.path;
                if (stat(newPath.c_str(), &st) == 0) {
                    ESP_LOGI(TAG, "Recovered path from index: %s", newPath.c_str());
                    lastPath = newPath;
                } else {
                    lastPath = ""; // Invalid
                }
            }
        } else {
            // If index not ready, we can't recover yet. 
            // But we shouldn't try to load a non-existent file.
            // We'll leave it empty so we don't crash/fail in loader.
            lastPath = "";
        }
    }

    // Try to sync ID with bookIndex if available
    if (bookIndexReady && !lastPath.empty()) {
        int realId = bookIndex.getBookIdByPath(lastPath);
        if (realId > 0 && realId != lastId) {
            ESP_LOGI(TAG, "Updating last book ID from %ld to %d based on path", lastId, realId);
            lastId = realId;
            lastBookId = realId;
        }
    }

    if (lastId <= 0 || lastPath.empty())
    {
        return false;
    }

    currentBook = {lastId, lastTitle, "", lastPath, lastChapter, lastOffset, 0, false, false, "", 1.0f};
    totalBookChars = 0;
    chapterPrefixSums.clear();
    bookMetricsComputed = false;

    bool loaded = false;
    bookOpenInProgress = true;
    if (xSemaphoreTake(epubMutex, portMAX_DELAY))
    {
        loaded = epubLoader.load(currentBook.path.c_str(), currentBook.currentChapter);
        if (loaded)
        {
            currentState = AppState::READER;

            // Auto-detect language
            std::string lang = epubLoader.getLanguage();
            bool isHebrew = (lang.find("he") != std::string::npos || lang.find("HE") != std::string::npos);
            if (!lastFont.empty() && lastFontSize > 0.0f)
            {
                // Restore saved font and size
                fontSizes[lastFont] = lastFontSize;
                setFont(lastFont);
                fontSize = lastFontSize;
            }
            else if (isHebrew)
            {
                // Hebrew book without saved font - use Hebrew with size 2.5
                setFont("Hebrew");
                if (fontSizes.count("Hebrew") == 0)
                {
                    fontSizes["Hebrew"] = 2.5f;
                }
                fontSize = fontSizes["Hebrew"];
            }
            else
            {
                // Default to Roboto with size 2.5
                setFont("Roboto");
                if (fontSizes.count("Roboto") == 0)
                {
                    fontSizes["Roboto"] = 2.5f;
                }
                fontSize = fontSizes["Roboto"];
            }

            // Restore progress - currentTextOffset is the exact page position
            currentTextOffset = currentBook.currentOffset;
            ESP_LOGI(TAG, "Restored book position: chapter=%d, offset=%zu", 
                     currentBook.currentChapter, currentTextOffset);
            resetPageInfoCache();

            // Quick RTL detection based on language only (skip expensive chapter scanning on restore)
            isRTLDocument = isHebrew;

            if (bookIndexReady && bookIndex.loadBookMetrics(currentBook.id, totalBookChars, chapterPrefixSums))
            {
                bookMetricsComputed = true;
                ESP_LOGI(TAG, "Loaded cached metrics for book %d", currentBook.id);
            }
            else
            {
                if (metricsTaskHandle != nullptr)
                    vTaskDelete(metricsTaskHandle);
                metricsTaskTargetBookId = currentBook.id;
                xTaskCreate([](void *arg)
                            { static_cast<GUI *>(arg)->metricsTaskLoop(); }, "MetricsTask", 4096, this, 0, &metricsTaskHandle);
            }
        }
        xSemaphoreGive(epubMutex);
    }

    bookOpenInProgress = false;

    if (!loaded)
    {
        return false;
    }

    pageHistory.clear();
    needsRedraw = true;
    return true;
}

uint8_t GUI::getGrayShade(int level) const
{
    // level 0 = white, level 15 = black
    // Return grayscale value (0-255, where 255=white, 0=black)
    if (level < 0) level = 0;
    if (level > 15) level = 15;
    return 255 - (level * 17); // 0->255, 15->0
}

uint32_t GUI::getTextColor(int brightness) const
{
    // brightness: 0=black, 15=white
    // Returns a color value suitable for e-paper display
    uint8_t gray = getGrayShade(15 - brightness); // Invert so 0=white, 15=black
    return M5.Display.color888(gray, gray, gray);
}

void GUI::loadFonts()
{
    // Clear buffers to ensure we don't hold two fonts at once if possible
    fontData.clear();
    fontDataHebrew.clear();
    fontDataArabic.clear();
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

    if (currentFont == "Arabic")
    {
        ensureArabicFontLoaded();
        if (!fontDataArabic.empty())
        {
            M5.Display.loadFont(fontDataArabic.data());
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

        // try
        //{
        fontDataHebrew.resize(size);
        //}
        // catch (const std::bad_alloc &e)
        //{
        //   ESP_LOGE(TAG, "Failed to allocate memory for Hebrew font (%u bytes)", (unsigned int)size);
        //   fclose(f);
        //   return;
        // }

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

void GUI::ensureArabicFontLoaded()
{
    if (!fontDataArabic.empty())
    {
        return;
    }

    // Clear other fonts to save memory
    if (!fontData.empty())
    {
        fontData.clear();
        M5.Display.unloadFont();
    }

    const char *path = "/spiffs/fonts/Arabic-Merged.vlw";
    ESP_LOGI(TAG, "Attempting to load Arabic font from: %s", path);
    FILE *f = fopen(path, "rb");
    if (f)
    {
        fseek(f, 0, SEEK_END);
        size_t size = ftell(f);
        fseek(f, 0, SEEK_SET);

        // try
        //{
        fontDataArabic.resize(size);
        //}
        // catch (const std::bad_alloc &e)
        //{
        //    ESP_LOGE(TAG, "Failed to allocate memory for Arabic font (%u bytes)", (unsigned int)size);
        //    fclose(f);
        //   return;
        //}

        size_t read = fread(fontDataArabic.data(), 1, size, f);
        fclose(f);
        ESP_LOGI(TAG, "Arabic font loaded: %u bytes read", (unsigned int)read);
    }
    else
    {
        ESP_LOGE(TAG, "Failed to open Arabic font file: %s (errno=%d)", path, errno);
    }
}

void GUI::ensureMathFontLoaded()
{
    if (!fontDataMath.empty())
    {
        return;
    }

    const char *path = "/spiffs/fonts/math.vlw";
    ESP_LOGI(TAG, "Attempting to load Math font from: %s", path);
    FILE *f = fopen(path, "rb");
    if (f)
    {
        fseek(f, 0, SEEK_END);
        size_t size = ftell(f);
        fseek(f, 0, SEEK_SET);
        
        fontDataMath.resize(size);
        size_t read = fread(fontDataMath.data(), 1, size, f);
        fclose(f);
        ESP_LOGI(TAG, "Math font loaded: %u bytes read", (unsigned int)read);
        
        // Initialize the math renderer with the font
        mathRenderer.setMathFont(fontDataMath.data());
        // Note: baseFontSize in init() is not used by calculateLayout() - we pass
        // our own font scale directly. Setting to 20 to match VLW native size.
        mathRenderer.init(20, M5.Display.width());
    }
    else
    {
        ESP_LOGW(TAG, "Failed to load Math font: %s", path);
    }
}

/**
 * @brief Render a MathML block inline with text
 * @param mathml The MathML string to render
 * @param canvas Target canvas (nullptr for main display)
 * @param x X position
 * @param y Y position (baseline)
 * @param maxWidth Maximum width available
 * @param outWidth Output: rendered width
 * @param outHeight Output: rendered height
 * @return true if rendered successfully
 */
bool GUI::renderMathInline(const std::string& mathml, M5Canvas* canvas, 
                           int x, int y, int maxWidth, int& outWidth, int& outHeight)
{
    if (mathml.empty()) {
        outWidth = 0;
        outHeight = 0;
        return false;
    }
    
    // Parse the MathML
    auto tree = mathRenderer.parse(mathml);
    if (!tree) {
        ESP_LOGW(TAG, "Failed to parse MathML");
        outWidth = 0;
        outHeight = 0;
        return false;
    }
    
    // Get the graphics device
    LGFX_Device* gfx = canvas ? (LGFX_Device*)canvas : (LGFX_Device*)&M5.Display;
    
    // Ensure math font is loaded
    ensureMathFontLoaded();
    if (!fontDataMath.empty()) {
        gfx->loadFont(fontDataMath.data());
        mathRenderer.setMathFont(fontDataMath.data());
    }
    
    // Calculate layout
    // IMPORTANT: GUI's fontSize is a SCALE FACTOR (1.0, 1.5, 2.0, 2.5), NOT pixels.
    // The math VLW font is generated at 20px native size.
    // setTextSize(1.0) means use native size (20px), setTextSize(2.0) means 40px.
    // 
    // To achieve reasonable math size that scales with user font preference:
    // - Base scale of 1.0 = native 20px font (good for inline math)
    // - Multiply by fontSize to scale proportionally with text
    // - But fontSize 2.5 would make math 50px which is still large,
    //   so we use a dampened scale: 1.0 + (fontSize - 1.0) * 0.4
    // This maps: fontSize 1.0 -> 1.0, fontSize 2.5 -> 1.6
    float mathFontSize = 1.0f + (fontSize - 1.0f) * 0.4f;
    mathRenderer.calculateLayout(tree.get(), gfx, mathFontSize);
    
    // Check if it fits
    outWidth = tree->box.width;
    outHeight = tree->box.height;
    
    if (outWidth > maxWidth) {
        // Scale down if too wide
        float scale = (float)maxWidth / outWidth;
        mathFontSize *= scale;
        mathRenderer.calculateLayout(tree.get(), gfx, mathFontSize);
        outWidth = tree->box.width;
        outHeight = tree->box.height;
    }
    
    // Render
    if (canvas) {
        MathRenderResult result = mathRenderer.render(tree.get(), canvas, x, y, mathFontSize, TFT_BLACK);
        return result.success;
    } else {
        // Render directly to display - create temporary canvas
        // Limit canvas size to avoid huge allocations
        if (outWidth > 2000 || outHeight > 2000) {
            ESP_LOGE(TAG, "Math render size too large: %dx%d", outWidth, outHeight);
            return false;
        }
        
        M5Canvas tempCanvas(&M5.Display);
        if (!tempCanvas.createSprite(outWidth, outHeight)) {
            ESP_LOGE(TAG, "Failed to create temp canvas for math: %dx%d", outWidth, outHeight);
            return false;
        }
        tempCanvas.fillSprite(TFT_WHITE);
        
        MathRenderResult result = mathRenderer.render(tree.get(), &tempCanvas, 0, tree->box.baseline, mathFontSize, TFT_BLACK);
        if (result.success) {
            tempCanvas.pushSprite(x, y - tree->box.baseline);
        }
        tempCanvas.deleteSprite();
        return result.success;
    }
}

/**
 * @brief Measure the dimensions of a MathML block
 * @param mathml The MathML string
 * @param outWidth Output: width
 * @param outHeight Output: height
 * @param outBaseline Output: baseline offset from top
 */
void GUI::measureMath(const std::string& mathml, int& outWidth, int& outHeight, int& outBaseline)
{
    if (mathml.empty()) {
        outWidth = outHeight = outBaseline = 0;
        return;
    }
    
    auto tree = mathRenderer.parse(mathml);
    if (!tree) {
        outWidth = outHeight = outBaseline = 0;
        return;
    }
    
    LGFX_Device* gfx = &M5.Display;
    ensureMathFontLoaded();
    if (!fontDataMath.empty()) {
        gfx->loadFont(fontDataMath.data());
        mathRenderer.setMathFont(fontDataMath.data());
    }
    
    // Use same dampened scale as renderMathInline for consistency
    float mathFontSize = 1.0f + (fontSize - 1.0f) * 0.4f;
    mathRenderer.calculateLayout(tree.get(), gfx, mathFontSize);
    
    outWidth = tree->box.width;
    outHeight = tree->box.height;
    outBaseline = tree->box.baseline;
}

void GUI::processText(std::string &text)
{
    // Currently no special processing needed
    // Placeholder for future text processing if needed
    // Replace HTML entities
    size_t pos = 0;
    while ((pos = text.find("&quot;", pos)) != std::string::npos)
    {
        text.replace(pos, 6, "\"");
        pos += 1;
    }
    pos = 0;
    while ((pos = text.find("&amp;", pos)) != std::string::npos)
    {
        text.replace(pos, 5, "&");
        pos += 1;
    }
    pos = 0;
    while ((pos = text.find("&lt;", pos)) != std::string::npos)
    {
        text.replace(pos, 4, "<");
        pos += 1;
    }
    pos = 0;
    while ((pos = text.find("&gt;", pos)) != std::string::npos)
    {
        text.replace(pos, 4, ">");
        pos += 1;
    }
    pos = 0;
    while ((pos = text.find("&apos;", pos)) != std::string::npos)
    {
        text.replace(pos, 6, "'");
        pos += 1;
    }
    pos = 0;
    while ((pos = text.find("&#160;", pos)) != std::string::npos)
    {
        text.replace(pos, 6, " ");
        pos += 1;
    }
}

void GUI::drawStringMixed(const std::string &text, int x, int y, M5Canvas *target, float size, bool isProcessed, bool respectUserFont)
{
    LovyanGFX *gfx = target ? (LovyanGFX *)target : (LovyanGFX *)&M5.Display;
    float effectiveSize = (size > 0.0f) ? size : fontSize;

    bool isHeb = isHebrew(text);
    bool isAra = isArabic(text);

    if (isHeb || isAra)
    {
        if (isHeb)
            ensureHebrewFontLoaded();
        else
            ensureArabicFontLoaded();

        const uint8_t *fontPtr = isHeb ? fontDataHebrew.data() : fontDataArabic.data();
        bool fontEmpty = isHeb ? fontDataHebrew.empty() : fontDataArabic.empty();
        const char *fontName = isHeb ? "Hebrew" : "Arabic";

        if (!fontEmpty)
        {
            bool swapped = false;
            // Only swap if not already using the correct font
            // If respectUserFont is false, we assume the current font is NOT the user font (e.g. Library mode),
            // so we MUST load the VLW font.
            if (!respectUserFont || currentFont != fontName)
            {
                gfx->loadFont(fontPtr);
                swapped = true;
            }

            // Scale down Hebrew/Arabic in UI mode (Library/Keyboard) if requested
            // User requested ~1/3 size for Library.
            // itemSize is 1.8. 1.8 * 0.4 = 0.72.
            // In Keyboard, size is 1.1 or 1.3. 1.3 * 0.4 = 0.52 (too small?).
            // Let's use a fixed scaling factor for UI mode.
            float drawSize = effectiveSize;
            if (!respectUserFont)
            {
                drawSize = effectiveSize * 0.45f;
            }

            std::string processedText = isProcessed ? text : processTextForDisplay(text);

            gfx->setTextSize(drawSize);
            gfx->drawString(processedText.c_str(), x, y);

            if (swapped)
            {
                if (respectUserFont)
                {
                    if (currentFont != "Default" && !fontData.empty())
                    {
                        gfx->loadFont(fontData.data());
                    }
                    else if (currentFont == "Hebrew" && !fontDataHebrew.empty())
                    {
                        gfx->loadFont(fontDataHebrew.data());
                    }
                    else if (currentFont == "Arabic" && !fontDataArabic.empty())
                    {
                        gfx->loadFont(fontDataArabic.data());
                    }
                    else
                    {
                        gfx->unloadFont();
                    }
                }
                else
                {
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
        if (!isProcessed)
        {
            processText(processedText);
        }

        gfx->setTextSize(effectiveSize);
        gfx->drawString(processedText.c_str(), x, y);
    }
}

void GUI::drawWifiScan()
{
    M5.Display.unloadFont(); // Use default font
    static M5Canvas wifiCanvas;
    static bool wifiCanvasReady = false;
    static bool wifiCanvasDisabled = false;

    if (!wifiCanvasDisabled)
    {
        if (!wifiCanvasReady)
        {
            wifiCanvas.setColorDepth(1); // Use 1-bit color to save RAM
            wifiCanvas.setPsram(true);
            if (wifiCanvas.createSprite(M5.Display.width(), M5.Display.height()))
            {
                wifiCanvasReady = true;
            }
            else
            {
                wifiCanvasDisabled = true;
                ESP_LOGW(TAG, "Failed to create wifi canvas - falling back to direct draw");
            }
        }

        if (wifiCanvasReady)
        {
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

            if (wifiList.empty())
            {
                wifiCanvas.setCursor(20, 100);
                wifiCanvas.println("Scanning...");
                wifiCanvas.pushSprite(&M5.Display, 0, 0);

                wifiList = wifiManager.scanNetworks();

                // If still empty after scan
                if (wifiList.empty())
                {
                    wifiCanvas.setCursor(20, 100);
                    wifiCanvas.println("No networks found.");
                    wifiCanvas.pushSprite(&M5.Display, 0, 0);
                }
                else
                {
                    // Redraw with list
                    drawWifiScan();
                    return;
                }
            }

            int y = 100;
            for (const auto &ssid : wifiList)
            {
                wifiCanvas.drawRect(20, y, wifiCanvas.width() - 40, 50, TFT_BLACK);
                wifiCanvas.drawString(ssid.c_str(), 30, y + 15);
                y += 60;
                if (y > wifiCanvas.height() - 80)
                    break; // Limit list
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

    if (wifiList.empty())
    {
        M5.Display.setCursor(20, 100);
        M5.Display.println("Scanning...");
        M5.Display.display();

        wifiList = wifiManager.scanNetworks();

        // If still empty after scan
        if (wifiList.empty())
        {
            M5.Display.setCursor(20, 100);
            M5.Display.println("No networks found.");
        }
        else
        {
            // Redraw with list
            drawWifiScan();
            return;
        }
    }

    int y = 100;
    for (const auto &ssid : wifiList)
    {
        M5.Display.drawRect(20, y, M5.Display.width() - 40, 50, TFT_BLACK);
        M5.Display.drawString(ssid.c_str(), 30, y + 15);
        y += 60;
        if (y > M5.Display.height() - 80)
            break; // Limit list
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
    // Note: Status bar click is now handled in handleTouch to go back to main menu

    // Check Cancel button
    int footerY = M5.Display.height() - 60;
    if (y >= footerY && y <= footerY + 40 && x >= 20 && x <= 170)
    {
        currentState = AppState::MAIN_MENU;
        needsRedraw = true;
        return;
    }

    // Check List
    int listY = 100;
    for (const auto &ssid : wifiList)
    {
        if (y >= listY && y <= listY + 50 && x >= 20 && x <= M5.Display.width() - 20)
        {
            selectedSSID = ssid;
            wifiPasswordInput = "";
            currentState = AppState::WIFI_PASSWORD;
            needsRedraw = true;
            return;
        }
        listY += 60;
        if (listY > M5.Display.height() - 80)
            break;
    }
}

void GUI::drawWifiPassword()
{
    // M5.Display.unloadFont(); // Use default font
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
    const char *rows[] = {
        "1234567890",
        "qwertyuiop",
        "asdfghjkl",
        "zxcvbnm"};

    int keySize = 45;
    int startY = 180;
    int startX = 20;

    M5.Display.setTextDatum(textdatum_t::middle_center);

    for (int r = 0; r < 4; r++)
    {
        int x = startX + (r * 20); // Indent
        for (int c = 0; c < strlen(rows[r]); c++)
        {
            char key[2] = {rows[r][c], 0};
            M5.Display.drawRect(x, startY + (r * keySize), keySize, keySize, TFT_BLACK);
            M5.Display.drawString(key, x + keySize / 2, startY + (r * keySize) + keySize / 2);
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

void GUI::onKeyboardClick(int x, int y)
{
    int screenW = M5.Display.width();
    int screenH = M5.Display.height();

    // Match keyboard dimensions from drawKeyboard
    int keyboardH = 200;
    int keyboardY = screenH - keyboardH;
    int keyW = screenW / 12;
    int keyH = 36;
    int padding = 3;

    // Check if click is above keyboard - dismiss keyboard
    if (y < keyboardY)
    {
        showKeyboard = false;
        needsRedraw = true;
        return;
    }

    // Hebrew keyboard layout
    const char *hebrewRows[] = {
        "1234567890",
        "/'קראטוןםפ",
        "שדגכעיחלך",
        "זסבהנמצ",
    };

    // English keyboard layout
    const char *englishRows[] = {
        "1234567890",
        "QWERTYUIOP",
        "ASDFGHJKL",
        "ZXCVBNM",
    };

    const char **rows = keyboardHebrew ? hebrewRows : englishRows;
    int numRows = 4;

    int rowY = keyboardY + 6;
    for (int r = 0; r < numRows; r++)
    {
        if (y >= rowY && y < rowY + keyH)
        {
            const char *row = rows[r];

            // Count UTF-8 characters
            int len = 0;
            for (const char *p = row; *p;)
            {
                unsigned char c = *p;
                if ((c & 0x80) == 0)
                {
                    p++;
                    len++;
                }
                else if ((c & 0xE0) == 0xC0)
                {
                    p += 2;
                    len++;
                }
                else if ((c & 0xF0) == 0xE0)
                {
                    p += 3;
                    len++;
                }
                else
                {
                    p += 4;
                    len++;
                }
            }

            int rowW = len * keyW + (len - 1) * padding;
            int startX = (screenW - rowW) / 2;

            // Find which key was pressed
            const char *p = row;
            int c = 0;
            while (*p)
            {
                unsigned char ch = *p;
                int charLen = 1;
                if ((ch & 0x80) == 0)
                    charLen = 1;
                else if ((ch & 0xE0) == 0xC0)
                    charLen = 2;
                else if ((ch & 0xF0) == 0xE0)
                    charLen = 3;
                else
                    charLen = 4;

                int kx = startX + c * (keyW + padding);
                if (x >= kx && x < kx + keyW)
                {
                    // Key pressed - add character to search
                    char chStr[5] = {0};
                    strncpy(chStr, p, charLen);

                    // Convert English uppercase to lowercase
                    if (charLen == 1 && chStr[0] >= 'A' && chStr[0] <= 'Z')
                    {
                        chStr[0] = chStr[0] + 32;
                    }

                    searchQuery += chStr;
                    needsRedraw = true;
                    return;
                }

                p += charLen;
                c++;
            }
        }
        rowY += keyH + padding;
    }

    // Bottom row: Lang, Backspace, Space, Done
    rowY = keyboardY + 6 + 4 * (keyH + padding);
    if (y >= rowY && y < rowY + keyH)
    {
        int langW = 55;
        int bsW = 55;
        int doneW = 60;
        int spaceW = screenW - langW - bsW - doneW - padding * 5;

        int langX = padding;
        int bsX = langX + langW + padding;
        int spaceX = bsX + bsW + padding;
        int doneX = spaceX + spaceW + padding;

        if (x >= langX && x < langX + langW)
        {
            // Language toggle
            keyboardHebrew = !keyboardHebrew;
            needsRedraw = true;
        }
        else if (x >= bsX && x < bsX + bsW)
        {
            // Backspace - handle UTF-8 properly
            if (!searchQuery.empty())
            {
                // Find start of last UTF-8 character
                size_t pos = searchQuery.length();
                while (pos > 0 && (searchQuery[pos - 1] & 0xC0) == 0x80)
                {
                    pos--;
                }
                if (pos > 0)
                    pos--;
                searchQuery = searchQuery.substr(0, pos);
                needsRedraw = true;
            }
        }
        else if (x >= spaceX && x < spaceX + spaceW)
        {
            // Space
            searchQuery += ' ';
            needsRedraw = true;
        }
        else if (x >= doneX && x < doneX + doneW)
        {
            // Done - close keyboard
            showKeyboard = false;
            libraryPage = 0;
            needsRedraw = true;
        }
    }
}

void GUI::onLibraryClick(int x, int y)
{
    int screenW = M5.Display.width();
    int screenH = M5.Display.height();

    // If keyboard is showing, handle keyboard clicks
    if (showKeyboard)
    {
        onKeyboardClick(x, y);
        return;
    }

    if (!bookIndexReady)
    {
        return;
    }

    // Note: Status bar click is now handled in handleTouch to go back to main menu

    // Check search bar area
    int searchBoxH = SEARCH_BAR_HEIGHT - 6;
    if (y >= SEARCH_BAR_Y && y < SEARCH_BAR_Y + searchBoxH)
    {
        int padding = 16;
        int starBtnSize = 44;
        int searchBtnW = 80;
        int clearBtnSize = 28;
        int searchBoxX = padding;
        int searchBoxW = screenW - padding * 3 - starBtnSize - searchBtnW - 10;
        int searchBtnX = searchBoxX + searchBoxW + 8;
        int starBtnX = searchBtnX + searchBtnW + 8;

        // Check clear button first (if there's text)
        if (!searchQuery.empty())
        {
            int clearBtnX = searchBoxX + searchBoxW - clearBtnSize - 4;
            int clearBtnY = SEARCH_BAR_Y + (searchBoxH - clearBtnSize) / 2;
            if (x >= clearBtnX && x < clearBtnX + clearBtnSize &&
                y >= clearBtnY && y < clearBtnY + clearBtnSize)
            {
                // Clear button pressed
                searchQuery.clear();
                libraryPage = 0;
                needsRedraw = true;
                return;
            }
        }

        if (x >= searchBoxX && x < searchBoxX + searchBoxW)
        {
            // Search textbox tapped - show keyboard
            showKeyboard = true;
            needsRedraw = true;
            return;
        }

        if (x >= searchBtnX && x < searchBtnX + searchBtnW)
        {
            // Search button tapped - perform search (already filtering, just hide keyboard)
            showKeyboard = false;
            libraryPage = 0;
            needsRedraw = true;
            return;
        }

        if (x >= starBtnX && x < starBtnX + starBtnSize)
        {
            // Star (favorites) toggle button - allow toggling on/off
            showFavoritesOnly = !showFavoritesOnly;
            libraryPage = 0;
            needsRedraw = true;
            return;
        }
    }

    // Get filtered books for position calculation
    // Use the current state to determine if we should filter by favorites
    bool filterFavorites = (currentState == AppState::FAVORITES) || showFavoritesOnly;
    auto books = bookIndex.getFilteredBooks(searchQuery, filterFavorites);
    int availableHeight = screenH - LIBRARY_LIST_START_Y - 60;
    int itemsPerPage = availableHeight / LIBRARY_LINE_HEIGHT;
    if (itemsPerPage < 1)
        itemsPerPage = 1;

    int totalPages = (books.size() + itemsPerPage - 1) / itemsPerPage;

    // Check paging buttons
    if (totalPages > 1 && y > screenH - 60)
    {
        if (x < 150 && libraryPage > 0)
        {
            libraryPage--;
            needsRedraw = true;
            return;
        }
        if (x > screenW - 150 && libraryPage < totalPages - 1)
        {
            libraryPage++;
            needsRedraw = true;
            return;
        }
    }

    // Check book list items
    int startIdx = libraryPage * itemsPerPage;
    int endIdx = std::min((int)books.size(), startIdx + itemsPerPage);

    int bookY = LIBRARY_LIST_START_Y;
    for (int i = startIdx; i < endIdx; ++i)
    {
        if (y >= bookY && y < bookY + LIBRARY_LINE_HEIGHT)
        {
            ESP_LOGI(TAG, "Touched book at index %d, ID %d", i, books[i].id);
            openBookById(books[i].id);
            return;
        }
        bookY += LIBRARY_LINE_HEIGHT;
    }
}

void GUI::onWifiPasswordClick(int x, int y)
{
    int keySize = 45;
    int startY = 180;
    int startX = 20;

    // Check Keyboard
    const char *rows[] = {
        "1234567890",
        "qwertyuiop",
        "asdfghjkl",
        "zxcvbnm"};

    bool inputChanged = false;

    for (int r = 0; r < 4; r++)
    {
        int rowX = startX + (r * 20);
        if (y >= startY + (r * keySize) && y < startY + ((r + 1) * keySize))
        {
            int col = (x - rowX) / keySize;
            if (col >= 0 && col < strlen(rows[r]))
            {
                wifiPasswordInput += rows[r][col];
                inputChanged = true;
            }
        }
    }

    int specialY = startY + (4 * keySize) + 10;
    if (y >= specialY && y <= specialY + 40)
    {
        // Backspace
        if (x >= 20 && x <= 120)
        {
            if (!wifiPasswordInput.empty())
            {
                wifiPasswordInput.pop_back();
                inputChanged = true;
            }
        }
        // Space
        else if (x >= 130 && x <= 330)
        {
            wifiPasswordInput += ' ';
            inputChanged = true;
        }
        // Cancel
        else if (x >= M5.Display.width() - 270 && x <= M5.Display.width() - 150)
        {
            currentState = AppState::WIFI_SCAN;
            needsRedraw = true;
        }
        // Connect
        else if (x >= M5.Display.width() - 140 && x <= M5.Display.width() - 20)
        {
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

    if (inputChanged)
    {
        // Update only the input box area
        M5.Display.fillRect(21, 101, M5.Display.width() - 42, 48, TFT_WHITE);
        M5.Display.setTextColor(TFT_BLACK);
        M5.Display.drawString(wifiPasswordInput.c_str(), 30, 115);
        M5.Display.display();
    }
}

// ============================================================================
// Image Viewer Implementation
// ============================================================================

void GUI::drawImageViewer()
{
    ESP_LOGI(TAG, "Drawing image viewer");

    // Clear screen with white background
    M5.Display.fillScreen(TFT_WHITE);

    if (currentImageData.empty())
    {
        // No image loaded - show error message
        M5.Display.setTextColor(TFT_BLACK, TFT_WHITE);
        M5.Display.setTextDatum(textdatum_t::middle_center);
        M5.Display.setTextSize(1.5f);
        M5.Display.drawString("Image could not be loaded", M5.Display.width() / 2, M5.Display.height() / 2);
        M5.Display.display();
        return;
    }

    // Decode and display image centered on screen
    ImageHandler &imgHandler = ImageHandler::getInstance();

    // Use full screen for rendering
    ImageDecodeResult result = imgHandler.decodeToDisplay(
        currentImageData.data(),
        currentImageData.size(),
        -1,  // Auto-center X
        -1,  // Auto-center Y
        true // Fit to screen
    );

    if (!result.success)
    {
        ESP_LOGE(TAG, "Failed to decode image: %s", result.errorMsg.c_str());
        M5.Display.setTextColor(TFT_BLACK, TFT_WHITE);
        M5.Display.setTextDatum(textdatum_t::middle_center);
        M5.Display.setTextSize(1.5f);
        M5.Display.drawString("Image decode failed", M5.Display.width() / 2, M5.Display.height() / 2);
    }
    else
    {
        ESP_LOGI(TAG, "Image displayed: %dx%d -> %dx%d",
                 result.originalWidth, result.originalHeight,
                 result.scaledWidth, result.scaledHeight);
    }

    // Draw a small "X" close indicator in the corner
    M5.Display.setTextColor(TFT_DARKGRAY, TFT_WHITE);
    M5.Display.setTextDatum(textdatum_t::top_right);
    M5.Display.setTextSize(2.0f);
    M5.Display.drawString("X", M5.Display.width() - 10, 10);

    // Draw alt text or "Tap to close" hint at bottom
    M5.Display.setTextColor(TFT_DARKGRAY, TFT_WHITE);
    M5.Display.setTextDatum(textdatum_t::bottom_center);
    M5.Display.setTextSize(1.2f);
    if (!currentImageInfo.alt.empty())
    {
        // Truncate if too long
        std::string altText = currentImageInfo.alt;
        if (altText.length() > 50)
        {
            altText = altText.substr(0, 47) + "...";
        }
        M5.Display.drawString(altText.c_str(), M5.Display.width() / 2, M5.Display.height() - 10);
    }
    else
    {
        M5.Display.drawString("Tap to close", M5.Display.width() / 2, M5.Display.height() - 10);
    }

    M5.Display.display();
}

void GUI::onImageViewerClick(int x, int y)
{
    ESP_LOGI(TAG, "Image viewer click at %d, %d", x, y);

    // Any tap closes the image viewer
    closeImageViewer();
}

bool GUI::openImageViewer(const EpubImage &image)
{
    ESP_LOGI(TAG, "Opening image viewer for: %s", image.path.c_str());

    // Store image info
    currentImageInfo = image;
    imageTextOffset = image.textOffset;

    // Extract image data from EPUB
    currentImageData.clear();

    bool success = false;
    if (xSemaphoreTake(epubMutex, portMAX_DELAY))
    {
        success = epubLoader.extractImage(image.path, currentImageData);
        xSemaphoreGive(epubMutex);
    }

    if (!success || currentImageData.empty())
    {
        ESP_LOGE(TAG, "Failed to extract image: %s", image.path.c_str());
        return false;
    }


    // Transition to image viewer state
    previousState = currentState;
    currentState = AppState::IMAGE_VIEWER;
    imageViewerActive = true;
    needsRedraw = true;

    return true;
}

void GUI::closeImageViewer()
{
    ESP_LOGI(TAG, "Closing image viewer");

    // Free image data memory
    currentImageData.clear();
    currentImageData.shrink_to_fit();

    imageViewerActive = false;

    // Return to reader state
    currentState = AppState::READER;
    needsRedraw = true;
}

bool GUI::renderImageAtOffset(size_t offset, M5Canvas *target, int x, int y, int maxWidth, int maxHeight)
{
    // Find image at this offset
    const EpubImage *image = nullptr;
    if (xSemaphoreTake(epubMutex, portMAX_DELAY))
    {
        image = epubLoader.findImageAtOffset(offset, 5);
        xSemaphoreGive(epubMutex);
    }

    if (!image)
    {
        ESP_LOGW(TAG, "No image found at offset %zu", offset);
        return false;
    }

    // Extract image data
    std::vector<uint8_t> imageData;
    bool success = false;
    if (xSemaphoreTake(epubMutex, portMAX_DELAY))
    {
        success = epubLoader.extractImage(image->path, imageData);
        xSemaphoreGive(epubMutex);
    }

    if (!success || imageData.empty())
    {
        ESP_LOGW(TAG, "Failed to extract image at offset %zu", offset);
        return false;
    }

    // Decode and render
    ImageHandler &imgHandler = ImageHandler::getInstance();
    ImageDecodeResult result = imgHandler.decodeAndRender(
        imageData.data(),
        imageData.size(),
        target,
        x, y,
        maxWidth,
        maxHeight,
        image->isBlock ? ImageDisplayMode::BLOCK : ImageDisplayMode::INLINE);

    if (!result.success)
    {
        ESP_LOGE(TAG, "Failed to decode image: %s", result.errorMsg.c_str());
        return false;
    }

    ESP_LOGI(TAG, "Rendered image at offset %zu: %dx%d", offset, result.scaledWidth, result.scaledHeight);
    return true;
}

// ============================================
// GAMES MENU
// ============================================

void GUI::drawGamesMenu()
{
    M5Canvas *sprite = (canvasNext.width() > 0) ? &canvasNext : nullptr;
    LovyanGFX *target = sprite ? (LovyanGFX *)sprite : (LovyanGFX *)&M5.Display;

    target->fillScreen(TFT_WHITE);
    drawStatusBar(target);

    target->setFont(&lgfx::v1::fonts::Font2);
    target->setTextColor(TFT_BLACK);

    int screenW = target->width();
    int screenH = target->height();

    // Title
    target->setTextDatum(textdatum_t::top_center);
    target->setTextSize(2.5f);
    target->drawString("Games", screenW / 2, STATUS_BAR_HEIGHT + 15);

    // Game buttons
    const int btnW = 200;
    const int btnH = 60;
    const int btnGap = 20;
    const int startY = STATUS_BAR_HEIGHT + 70;
    const int centerX = screenW / 2;

    const char *gameNames[] = {"Minesweeper", "Sudoku", "Wordle"};

    target->setTextSize(2.0f);
    for (int i = 0; i < 3; i++)
    {
        int by = startY + i * (btnH + btnGap);
        int bx = centerX - btnW / 2;

        target->fillRect(bx, by, btnW, btnH, TFT_WHITE);
        target->drawRect(bx, by, btnW, btnH, TFT_BLACK);
        target->drawRect(bx + 1, by + 1, btnW - 2, btnH - 2, TFT_BLACK);

        target->setTextDatum(textdatum_t::middle_center);
        target->drawString(gameNames[i], centerX, by + btnH / 2);
    }

    // Utility section
    int utilityY = startY + 3 * (btnH + btnGap) + 20;
    target->setTextDatum(textdatum_t::top_center);
    target->setTextSize(2.5f);
    target->drawString("Utility", screenW / 2, utilityY);

    int composerY = utilityY + 50;
    int cbx = centerX - btnW / 2;
    target->fillRect(cbx, composerY, btnW, btnH, TFT_WHITE);
    target->drawRect(cbx, composerY, btnW, btnH, TFT_BLACK);
    target->drawRect(cbx + 1, composerY + 1, btnW - 2, btnH - 2, TFT_BLACK);
    target->setTextSize(2.0f);
    target->setTextDatum(textdatum_t::middle_center);
    target->drawString("Music Composer", centerX, composerY + btnH / 2);

    // Back button
    int backY = screenH - 70;
    target->fillRect(centerX - 60, backY, 120, 50, TFT_LIGHTGREY);
    target->drawRect(centerX - 60, backY, 120, 50, TFT_BLACK);
    target->setTextDatum(textdatum_t::middle_center);
    target->drawString("Back", centerX, backY + 25);

    if (sprite)
    {
        sprite->pushSprite(&M5.Display, 0, 0);
    }
    M5.Display.display();
}

void GUI::onGamesMenuClick(int x, int y)
{
    int screenW = M5.Display.width();
    int screenH = M5.Display.height();

    const int btnW = 200;
    const int btnH = 60;
    const int btnGap = 20;
    const int startY = STATUS_BAR_HEIGHT + 70;
    const int centerX = screenW / 2;

    // Check game buttons
    for (int i = 0; i < 3; i++)
    {
        int by = startY + i * (btnH + btnGap);
        int bx = centerX - btnW / 2;

        if (x >= bx && x < bx + btnW && y >= by && y < by + btnH)
        {
            GameManager &gm = GameManager::getInstance();
            GameType gameType;
            switch (i)
            {
            case 0:
                gameType = GameType::MINESWEEPER;
                break;
            case 1:
                gameType = GameType::SUDOKU;
                break;
            case 2:
                gameType = GameType::WORDLE;
                break;
            default:
                return;
            }
            gm.startGame(gameType);
            currentState = AppState::GAME_PLAYING;
            needsRedraw = true;
            return;
        }
    }

    // Check Music Composer button
    int utilityY = startY + 3 * (btnH + btnGap) + 20;
    int composerY = utilityY + 50;
    int cbx = centerX - btnW / 2;
    ESP_LOGI(TAG, "onGamesMenuClick: touch=(%d,%d), composerRect=(%d,%d,%d,%d)", x, y, cbx, composerY, btnW, btnH);
    if (x >= cbx && x < cbx + btnW && y >= composerY && y < composerY + btnH)
    {
        ESP_LOGI(TAG, "Music Composer button pressed, launching composer");
        ComposerUI::getInstance().init();
        ComposerUI::getInstance().enter();
        currentState = AppState::MUSIC_COMPOSER;
        needsRedraw = true;
        ESP_LOGI(TAG, "Composer active=%d canvasCreated=%d", ComposerUI::getInstance().isActive(), ComposerUI::getInstance().isCanvasCreated());
        return;
    }

    // Check back button
    int backY = screenH - 70;
    if (x >= centerX - 60 && x < centerX + 60 && y >= backY && y < backY + 50)
    {
        currentState = AppState::MAIN_MENU;
        needsRedraw = true;
        return;
    }

    // Tap on status bar returns to main menu
    if (y < STATUS_BAR_HEIGHT)
    {
        currentState = AppState::MAIN_MENU;
        needsRedraw = true;
    }
}

void GUI::drawGame()
{
    // Draw status bar first, then let GameManager draw the game
    drawStatusBar(nullptr);
    
    GameManager &gm = GameManager::getInstance();
    gm.draw(nullptr);
}

void GUI::drawMusicComposer()
{
    ComposerUI::getInstance().draw();
}

void GUI::onMusicComposerClick(int x, int y)
{
    ComposerUI &composer = ComposerUI::getInstance();
    if (composer.handleTouch(x, y))
    {
        // Redraw is handled by ComposerUI if needed
    }

    if (composer.shouldExit())
    {
        composer.clearExitFlag();
        composer.exit();
        currentState = AppState::GAMES_MENU;
        needsRedraw = true;
    }
}

// ============================================
// CHAPTER MENU (In-book chapter navigation)
// ============================================

void GUI::drawChapterMenu()
{
    M5Canvas *sprite = (canvasNext.width() > 0) ? &canvasNext : nullptr;
    LovyanGFX *target = sprite ? (LovyanGFX *)sprite : (LovyanGFX *)&M5.Display;

    target->fillScreen(TFT_WHITE);
    drawStatusBar(target);

    target->setFont(&lgfx::v1::fonts::Font2);
    target->setTextColor(TFT_BLACK);

    int screenW = target->width();
    int screenH = target->height();

    // Title
    target->setTextDatum(textdatum_t::top_center);
    target->setTextSize(2.0f);
    target->drawString("Chapters", screenW / 2, STATUS_BAR_HEIGHT + 10);

    // Get chapter list with proper titles
    int totalChapters = 0;
    int currentChapter = 0;
    std::vector<std::string> chapterNames;

    if (xSemaphoreTake(epubMutex, portMAX_DELAY))
    {
        totalChapters = epubLoader.getTotalChapters();
        currentChapter = epubLoader.getCurrentChapterIndex();
        
        // Get chapter titles from TOC
        const auto& titles = epubLoader.getChapterTitles();
        for (int i = 0; i < totalChapters; i++)
        {
            std::string name;
            if (i < (int)titles.size() && !titles[i].empty())
            {
                name = titles[i];
            }
            else
            {
                // Fallback to numbered chapter
                char buf[32];
                snprintf(buf, sizeof(buf), "Chapter %d", i + 1);
                name = buf;
            }
            chapterNames.push_back(name);
        }
        xSemaphoreGive(epubMutex);
    }

    // Draw chapter list (scrollable)
    const int lineHeight = 45;
    const int startY = STATUS_BAR_HEIGHT + 50;
    const int visibleLines = (screenH - startY - 60) / lineHeight;

    // Calculate scroll offset to show current chapter
    if (currentChapter < chapterMenuScrollOffset)
        chapterMenuScrollOffset = currentChapter;
    if (currentChapter >= chapterMenuScrollOffset + visibleLines)
        chapterMenuScrollOffset = currentChapter - visibleLines + 1;

    target->setTextSize(1.5f);
    target->setTextDatum(textdatum_t::middle_left);

    for (int i = 0; i < visibleLines && (chapterMenuScrollOffset + i) < (int)chapterNames.size(); i++)
    {
        int idx = chapterMenuScrollOffset + i;
        int ty = startY + i * lineHeight + lineHeight / 2;

        // Highlight current chapter
        if (idx == currentChapter)
        {
            target->fillRect(10, startY + i * lineHeight, screenW - 20, lineHeight - 2, TFT_LIGHTGREY);
        }

        // Truncate long names
        std::string name = chapterNames[idx];
        if (name.length() > 35)
        {
            name = name.substr(0, 32) + "...";
        }

        // Use drawStringMixed to support Hebrew/Arabic chapter titles
        drawStringMixed(name, 20, ty, sprite, 1.5f, false, false);
    }

    // Back button
    int backY = screenH - 55;
    target->fillRect(screenW / 2 - 60, backY, 120, 45, TFT_LIGHTGREY);
    target->drawRect(screenW / 2 - 60, backY, 120, 45, TFT_BLACK);
    target->setTextDatum(textdatum_t::middle_center);
    target->setTextSize(1.8f);
    target->drawString("Back", screenW / 2, backY + 22);

    if (sprite)
    {
        sprite->pushSprite(&M5.Display, 0, 0);
    }
    M5.Display.display();
}

void GUI::onChapterMenuClick(int x, int y)
{
    int screenW = M5.Display.width();
    int screenH = M5.Display.height();

    // Back button
    int backY = screenH - 55;
    if (x >= screenW / 2 - 60 && x < screenW / 2 + 60 && y >= backY && y < backY + 45)
    {
        currentState = AppState::READER;
        needsRedraw = true;
        return;
    }

    // Status bar tap returns to reader
    if (y < STATUS_BAR_HEIGHT)
    {
        currentState = AppState::READER;
        needsRedraw = true;
        return;
    }

    // Chapter selection
    const int lineHeight = 45;
    const int startY = STATUS_BAR_HEIGHT + 50;
    const int visibleLines = (screenH - startY - 60) / lineHeight;

    int totalChapters = 0;
    if (xSemaphoreTake(epubMutex, portMAX_DELAY))
    {
        totalChapters = epubLoader.getTotalChapters();
        xSemaphoreGive(epubMutex);
    }

    // Determine which chapter was clicked
    if (y >= startY && y < startY + visibleLines * lineHeight)
    {
        int clickedLine = (y - startY) / lineHeight;
        int selectedChapter = chapterMenuScrollOffset + clickedLine;

        if (selectedChapter >= 0 && selectedChapter < totalChapters)
        {
            currentState = AppState::READER;
            jumpToChapter(selectedChapter);
            needsRedraw = true;
        }
    }
}

// ============================================
// FONT SELECTION (SD Card fonts for S3)
// ============================================

void GUI::scanSDCardFonts()
{
    // Enable SD card fonts for all devices (Issue 6)
    sdCardFonts.clear();

    DeviceHAL &hal = DeviceHAL::getInstance();
    if (!hal.isSDCardMounted())
        return;

    std::string fontsPath = std::string(hal.getSDCardMountPoint()) + "/fonts";
    DIR *dir = opendir(fontsPath.c_str());
    if (!dir)
    {
        ESP_LOGW(TAG, "No fonts/ folder on SD card");
        return;
    }

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL)
    {
        if (entry->d_type == DT_REG)
        {
            std::string fname = entry->d_name;
            // Look for .otf or .ttf files
            if (fname.length() > 4)
            {
                std::string ext = fname.substr(fname.length() - 4);
                for (char &c : ext)
                    c = tolower(c);
                if (ext == ".otf" || ext == ".ttf")
                {
                    // Store font name without extension
                    std::string fontName = fname.substr(0, fname.length() - 4);
                    sdCardFonts.push_back(fontName);
                    ESP_LOGI(TAG, "Found SD card font: %s", fontName.c_str());
                }
            }
        }
    }
    closedir(dir);
}

void GUI::drawFontSelection()
{
    M5Canvas *sprite = (canvasNext.width() > 0) ? &canvasNext : nullptr;
    LovyanGFX *target = sprite ? (LovyanGFX *)sprite : (LovyanGFX *)&M5.Display;

    target->fillScreen(TFT_WHITE);
    drawStatusBar(target);

    target->setFont(&lgfx::v1::fonts::Font2);
    target->setTextColor(TFT_BLACK);

    int screenW = target->width();
    int screenH = target->height();

    // Title
    target->setTextDatum(textdatum_t::top_center);
    target->setTextSize(2.0f);
    target->drawString("Select Font", screenW / 2, STATUS_BAR_HEIGHT + 10);

    // Built-in fonts
    std::vector<std::string> allFonts = {"Default", "Hebrew", "Arabic", "Roboto"};

    // Add SD card fonts (Issue 6)
    if (sdCardFonts.empty())
    {
        scanSDCardFonts();
    }
    for (const auto &f : sdCardFonts)
    {
        allFonts.push_back("SD:" + f);
    }

    const int lineHeight = 70;
    const int startY = STATUS_BAR_HEIGHT + 55;

    target->setTextSize(2.5f);
    target->setTextDatum(textdatum_t::middle_left);

    for (size_t i = 0; i < allFonts.size() && (int)(startY + i * lineHeight) < screenH - 70; i++)
    {
        int ty = startY + i * lineHeight + lineHeight / 2;

        // Highlight current font
        if (allFonts[i] == currentFont ||
            (allFonts[i].substr(0, 3) == "SD:" && currentFont == allFonts[i].substr(3)))
        {
            target->fillRect(10, startY + i * lineHeight, screenW - 20, lineHeight - 2, TFT_LIGHTGREY);
        }

        target->drawString(allFonts[i].c_str(), 20, ty);
    }

    // Back button
    int backY = screenH - 55;
    target->fillRect(screenW / 2 - 60, backY, 120, 45, TFT_LIGHTGREY);
    target->drawRect(screenW / 2 - 60, backY, 120, 45, TFT_BLACK);
    target->setTextDatum(textdatum_t::middle_center);
    target->drawString("Back", screenW / 2, backY + 22);

    if (sprite)
    {
        sprite->pushSprite(&M5.Display, 0, 0);
    }
    M5.Display.display();
}

void GUI::onFontSelectionClick(int x, int y)
{
    int screenW = M5.Display.width();
    int screenH = M5.Display.height();

    // Back button
    int backY = screenH - 55;
    if (x >= screenW / 2 - 60 && x < screenW / 2 + 60 && y >= backY && y < backY + 45)
    {
        currentState = previousState;
        needsRedraw = true;
        return;
    }

    // Status bar tap returns
    if (y < STATUS_BAR_HEIGHT)
    {
        currentState = previousState;
        needsRedraw = true;
        return;
    }

    // Font selection
    std::vector<std::string> allFonts = {"Default", "Hebrew", "Arabic", "Roboto"};
#ifdef CONFIG_EBOOK_DEVICE_M5PAPERS3
    for (const auto &f : sdCardFonts)
    {
        allFonts.push_back("SD:" + f);
    }
#endif

    const int lineHeight = 50;
    const int startY = STATUS_BAR_HEIGHT + 55;

    if (y >= startY)
    {
        int clickedLine = (y - startY) / lineHeight;
        if (clickedLine >= 0 && clickedLine < (int)allFonts.size())
        {
            std::string selectedFont = allFonts[clickedLine];
            if (selectedFont.substr(0, 3) == "SD:")
            {
                selectedFont = selectedFont.substr(3); // Remove "SD:" prefix
            }
            setFont(selectedFont);
            currentState = previousState;
            needsRedraw = true;
        }
    }
}

// ============================================
// WALLPAPER ON SLEEP
// ============================================

void GUI::showWallpaperAndSleep()
{
#ifdef CONFIG_EBOOK_DEVICE_M5PAPERS3
    DeviceHAL &hal = DeviceHAL::getInstance();
    
    bool showedWallpaper = false;
    
    if (hal.isSDCardMounted())
    {
        std::string wallpaperPath = std::string(hal.getSDCardMountPoint()) + "/wallpaper";
        DIR *dir = opendir(wallpaperPath.c_str());
        if (dir)
        {
            // Collect image files
            std::vector<std::string> images;
            struct dirent *entry;
            while ((entry = readdir(dir)) != NULL)
            {
                if (entry->d_type == DT_REG)
                {
                    std::string fname = entry->d_name;
                    if (fname.length() > 4)
                    {
                        std::string ext = fname.substr(fname.length() - 4);
                        for (char &c : ext)
                            c = tolower(c);
                        if (ext == ".jpg" || ext == ".png" || ext == ".bmp")
                        {
                            images.push_back(wallpaperPath + "/" + fname);
                        }
                        // Also check 5-char extension for .jpeg
                        if (fname.length() > 5)
                        {
                            std::string ext5 = fname.substr(fname.length() - 5);
                            for (char &c : ext5)
                                c = tolower(c);
                            if (ext5 == ".jpeg")
                            {
                                images.push_back(wallpaperPath + "/" + fname);
                            }
                        }
                    }
                }
            }
            closedir(dir);

            if (!images.empty())
            {
                // Pick random image using time-based seed
                uint32_t seed = (uint32_t)(esp_timer_get_time() / 1000);
                std::mt19937 gen(seed);
                std::uniform_int_distribution<> dis(0, images.size() - 1);
                std::string imagePath = images[dis(gen)];

                ESP_LOGI(TAG, "Showing wallpaper: %s", imagePath.c_str());

                // Load and display the image
                FILE *f = fopen(imagePath.c_str(), "rb");
                if (f)
                {
                    fseek(f, 0, SEEK_END);
                    size_t size = ftell(f);
                    fseek(f, 0, SEEK_SET);

                    if (size < 2 * 1024 * 1024) // Limit to 2MB images
                    {
                        std::vector<uint8_t> imageData(size);
                        if (fread(imageData.data(), 1, size, f) == size)
                        {
                            // Use ImageHandler to decode and display
                            ImageHandler &imgHandler = ImageHandler::getInstance();
                            M5.Display.fillScreen(TFT_WHITE);

                            ImageDecodeResult result = imgHandler.decodeAndRender(
                                imageData.data(),
                                imageData.size(),
                                &M5.Display,
                                0, 0,
                                M5.Display.width(),
                                M5.Display.height(),
                                ImageDisplayMode::BLOCK);

                            if (result.success)
                            {
                                M5.Display.display();
                                M5.Display.waitDisplay();
                                showedWallpaper = true;
                            }
                        }
                    }
                    fclose(f);
                }
            }
            else
            {
                ESP_LOGI(TAG, "No images in wallpaper/ folder");
            }
        }
        else
        {
            ESP_LOGI(TAG, "No wallpaper/ folder on SD card");
        }
    }
    
    if (!showedWallpaper)
    {
        // Show "Zz" sleep symbol if no wallpaper
        drawSleepSymbol("Zz");
    }

    // Enter deep sleep with touch wake so user can resume reading
    deviceHAL.enterDeepSleepWithTouchWake();
#else
    // Non-S3 devices: show sleep symbol and enter deep sleep with touch wake
    drawSleepSymbol("Zz");
    deviceHAL.enterDeepSleepWithTouchWake();
#endif
}
