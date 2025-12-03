#pragma once
#include <M5Unified.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include "book_index.h"

enum class AppState {
    LIBRARY,
    READER,
    WIFI_CONFIG,
    SETTINGS,
    WIFI_SCAN,
    WIFI_PASSWORD
};

struct RenderRequest {
    size_t offset;
    M5Canvas* target;
    bool isNext; // true for next, false for prev
};

class GUI {
public:
    void init(bool isWakeFromSleep = false);
    void update(); // Main loop
    
    void setWifiStatus(bool connected, int rssi);
    void refreshLibrary();

    void setFontSize(float size);
    void setFont(const std::string& fontName);
    void setLineSpacing(float spacing);
    float getFontSize() const { return fontSize; }
    std::string getFont() const { return currentFont; }
    float getLineSpacing() const { return lineSpacing; }
    
    void jumpTo(float percent);
    void jumpToChapter(int chapter);
    bool openBookById(int id);
    // Introspection helpers for external callers (eg. web server)
    bool canJump() const; // true when in READER state
    size_t getCurrentChapterSize() const;
    size_t getCurrentOffset() const;
    int getCurrentChapterIndex() const;
    
    void setWebServerEnabled(bool enabled);
    bool isWifiEnabled() const { return wifiEnabled; }

    // Public for the task to access
    void renderTaskLoop();
    void metricsTaskLoop();
    void backgroundIndexerTaskLoop();

private:
    AppState currentState = AppState::LIBRARY;
    AppState previousState = AppState::LIBRARY; // For returning from settings
    bool needsRedraw = true;
    BookEntry currentBook;
    struct PageInfo { int current = 1; int total = 1; };
    bool isRTLDocument = false;
    
    // Reader State
    size_t currentTextOffset = 0;
    std::vector<size_t> pageHistory;
    
    // Buffering
    M5Canvas canvasCurrent;
    M5Canvas canvasNext;
    M5Canvas canvasPrev;
    bool nextCanvasValid = false;
    bool prevCanvasValid = false;
    size_t nextCanvasOffset = 0;
    size_t prevCanvasOffset = 0;
    size_t nextCanvasCharCount = 0;
    size_t prevCanvasCharCount = 0;
    
    // Background Rendering
    TaskHandle_t renderTaskHandle = nullptr;
    TaskHandle_t metricsTaskHandle = nullptr;
    TaskHandle_t backgroundIndexerTaskHandle = nullptr;
    int metricsTaskTargetBookId = 0;
    QueueHandle_t renderQueue = nullptr;
    SemaphoreHandle_t epubMutex = nullptr;
    volatile bool abortRender = false;
    
    // Library State
    int libraryPage = 0;

    // Settings
    float fontSize = 1.0f; // Scale factor
    float lineSpacing = 1.1f; // Line height multiplier (1.0 = tight, 2.0 = double spaced)
    std::string currentFont = "Default";
    std::string previousFont = "Default";
    bool fontChanged = false;
    std::vector<uint8_t> fontData; // Buffer to hold loaded font in memory
    std::vector<uint8_t> fontDataHebrew; // Cache for Hebrew font
    bool wifiEnabled = true;
    
    size_t lastPageChars = 0;
    int lastPageTotal = 1;
    size_t totalBookChars = 0;
    std::vector<size_t> chapterPrefixSums;
    bool bookMetricsComputed = false;

    // Double click detection
    uint32_t lastClickTime = 0;
    int lastClickX = 0;
    int lastClickY = 0;
    bool clickPending = false;

    // Settings UI buffering
    M5Canvas settingsCanvas;
    bool settingsCanvasCreated = false;

    // WiFi Scan
    std::vector<std::string> wifiList;
    std::string wifiPasswordInput;
    std::string selectedSSID;

    void drawStatusBar(LovyanGFX* target = nullptr);
    void drawFooter(LovyanGFX* target, size_t pageOffset, size_t charsOnPage);
    void drawSleepSymbol(const char* symbol);
    void drawLibrary();
    void drawReader(bool flush = true);
    void drawWifiConfig();
    void drawSettings();
    void drawWifiScan();
    void drawWifiPassword();
    
    void handleTouch();
    void processReaderTap(int x, int y, bool isDouble);
    void onWifiScanClick(int x, int y);
    void onWifiPasswordClick(int x, int y);
    
    void loadFonts();
    void ensureHebrewFontLoaded();
    void drawStringMixed(const std::string& text, int x, int y, M5Canvas* target = nullptr, float size = -1.0f);
    
    void saveSettings();
    void loadSettings();
    void goToSleep();
    
    size_t drawPageContent(bool draw);
    size_t drawPageContentAt(size_t startOffset, bool draw, M5Canvas* target = nullptr, volatile bool* abort = nullptr);
    PageInfo calculatePageInfo();
    void resetPageInfoCache();
    void computeBookMetrics();
    void computeBookMetricsLocked();
    void updateNextPrevCanvases();

    uint32_t lastActivityTime = 0;
    bool webServerEnabled = true;
    bool settingsNeedsUnderlayRefresh = false;
    bool justWokeUp = false;
};
