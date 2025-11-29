#pragma once
#include <M5Unified.h>
#include "book_index.h"
#include <vector>

enum class AppState {
    LIBRARY,
    READER,
    WIFI_CONFIG,
    SETTINGS,
    WIFI_SCAN,
    WIFI_PASSWORD
};

class GUI {
public:
    void init(bool isWakeFromSleep = false);
    void update(); // Main loop
    
    void setWifiStatus(bool connected, int rssi);
    void refreshLibrary();

    void setFontSize(float size);
    void setFont(const std::string& fontName);
    float getFontSize() const { return fontSize; }
    std::string getFont() const { return currentFont; }
    
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
    
    // Library State
    int libraryPage = 0;

    // Settings
    float fontSize = 1.0f; // Scale factor
    std::string currentFont = "Default";
    std::string previousFont = "Default";
    bool fontChanged = false;
    std::vector<uint8_t> fontData; // Buffer to hold loaded font in memory
    std::vector<uint8_t> fontDataHebrew; // Cache for Hebrew font
    bool wifiEnabled = true;
    
    size_t lastPageChars = 0;
    int lastPageTotal = 1;

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

    void drawStatusBar();
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
    size_t drawPageContentAt(size_t startOffset, bool draw, M5Canvas* target = nullptr);
    PageInfo calculatePageInfo();
    void resetPageInfoCache();
    void updateNextPrevCanvases();

    uint32_t lastActivityTime = 0;
    bool webServerEnabled = true;
    bool settingsNeedsUnderlayRefresh = false;
    bool justWokeUp = false;
};
