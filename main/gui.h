#pragma once
#include <M5Unified.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include "book_index.h"
#include "sdkconfig.h"

enum class AppState {
    MAIN_MENU,      // New main menu with 6 buttons
    LIBRARY,
    READER,
    WIFI_CONFIG,
    SETTINGS,       // Standalone settings (from main menu)
    BOOK_SETTINGS,  // In-book settings overlay
    WIFI_SCAN,
    WIFI_PASSWORD,
    FAVORITES       // Favorites-only book list
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

    // M5PaperS3 specific features
    void setBuzzerEnabled(bool enabled);
    bool isBuzzerEnabled() const;
    void setAutoRotateEnabled(bool enabled);
    bool isAutoRotateEnabled() const;
    void checkOrientation(); // Check and update rotation from gyroscope

    static void enterDeepSleepShutdown();

    // Public for the task to access
    void renderTaskLoop();
    void metricsTaskLoop();
    void backgroundIndexerTaskLoop();

private:
    AppState currentState = AppState::MAIN_MENU;
    AppState previousState = AppState::MAIN_MENU; // For returning from settings
    bool needsRedraw = true;
    BookEntry currentBook;
    struct PageInfo { int current = 1; int total = 1; };
    bool isRTLDocument = false;
    
    // Last book info for main menu
    int lastBookId = -1;
    std::string lastBookTitle;
    
    // Search and Filter State
    std::string searchQuery;           // Current search text
    bool showKeyboard = false;         // Keyboard visible
    bool showFavoritesOnly = false;    // Filter to favorites only
    int keyboardCursorPos = 0;         // Cursor position in search text
    bool keyboardHebrew = false;       // Hebrew keyboard mode
    
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
    std::vector<uint8_t> fontDataArabic; // Cache for Arabic font
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
    void drawMainMenu();        // New main menu with 6 buttons
    void drawLibrary(bool favoritesOnly = false);  // Book list with optional favorites filter
    void drawSearchBar(LovyanGFX* target);
    void drawKeyboard(LovyanGFX* target);
    void drawBookSettings();  // Half-page settings for book in reader (overlay)
    void drawStandaloneSettings(); // Full-page settings (from main menu)
    void drawReader(bool flush = true);
    void drawWifiConfig();
    void drawSettings();
    void drawWifiScan();
    void drawWifiPassword();
    
    void handleTouch();
    void handleButtonPress();   // Handle hardware button press (long press = shutdown)
    void processReaderTap(int x, int y, bool isDouble);
    void onMainMenuClick(int x, int y);  // Handle main menu clicks
    void onWifiScanClick(int x, int y);
    void onWifiPasswordClick(int x, int y);
    void onLibraryClick(int x, int y);
    void onKeyboardClick(int x, int y);
    void onSettingsClick(int x, int y);  // Handle standalone settings clicks

    void processText(std::string &text);
    
    void loadFonts();
    void ensureHebrewFontLoaded();
    void ensureArabicFontLoaded();
    void drawStringMixed(const std::string &text, int x, int y, M5Canvas *target = nullptr, float size = -1.0f, bool isProcessed = false, bool respectUserFont = true);
    
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

    // M5PaperS3 features
#ifdef CONFIG_EBOOK_DEVICE_M5PAPERS3
    bool buzzerEnabled = true;
    bool autoRotateEnabled = true;
    uint32_t lastOrientationCheck = 0;
    static constexpr uint32_t ORIENTATION_CHECK_INTERVAL_MS = 500;
#endif
};
