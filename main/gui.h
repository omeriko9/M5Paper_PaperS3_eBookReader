#pragma once
#include <M5Unified.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include "book_index.h"
#include "sdkconfig.h"
#include "epub_loader.h"
#include "gesture_detector.h"
#include <map>

enum class AppState {
    MAIN_MENU,      // Main menu with 6 buttons
    LIBRARY,
    READER,
    WIFI_CONFIG,
    SETTINGS,       // Standalone settings (from main menu)
    BOOK_SETTINGS,  // In-book settings overlay
    WIFI_SCAN,
    WIFI_PASSWORD,
    FAVORITES,      // Favorites-only book list
    IMAGE_VIEWER,   // Full-screen image viewing mode
    GAMES_MENU,     // Games selection menu
    GAME_PLAYING,   // Active game
    CHAPTER_MENU,   // In-book chapter jump menu
    FONT_SELECTION, // Font selection screen (S3 only)
    MUSIC_COMPOSER  // Music sequencer screen
};

struct RenderRequest {
    size_t offset;
    M5Canvas* target;
    bool isNext; // true for next, false for prev
};

// Tracks a rendered image position on the page for tap detection
struct RenderedImageInfo {
    int x, y;           // Top-left corner
    int width, height;  // Dimensions
    EpubImage image;    // Image metadata
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
    float getFontSize() const;  // Returns size for current font
    float getFontSize(const std::string& fontName) const;  // Get size for specific font
    std::string getFont() const { return currentFont; }
    float getLineSpacing() const { return lineSpacing; }
    
    // Text boldness (0-3, uses grayscale shades)
    void setTextBoldness(int level);
    int getTextBoldness() const { return textBoldness; }
    
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
    
    void setShowImages(bool enabled);
    bool isShowImages() const { return showImages; }

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
    volatile bool needsRedraw = true;
    volatile bool bookOpenInProgress = false;
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

    // Settings - Per-font sizes stored in map
    std::map<std::string, float> fontSizes;  // Font name -> size
    float fontSize = 1.0f; // Current font size
    float lineSpacing = 1.1f; // Line height multiplier (1.0 = tight, 2.0 = double spaced)
    int lightSleepMinutes = 5; // Time before deep sleep (minutes)
    std::string currentFont = "Default";
    std::string previousFont = "Default";
    bool fontChanged = false;
    std::vector<uint8_t> fontData; // Buffer to hold loaded font in memory
    std::vector<uint8_t> fontDataHebrew; // Cache for Hebrew font
    std::vector<uint8_t> fontDataArabic; // Cache for Arabic font
    bool wifiEnabled = true;
    bool showImages = true;
    int textBoldness = 0;  // 0-3, controls grayscale intensity
    
    // SD card fonts (S3 only)
    std::vector<std::string> sdCardFonts;  // List of .otf fonts on SD card
    int fontSelectionPage = 0;
    
    // Gesture detection
    GestureDetector gestureDetector;
    
    size_t lastPageChars = 0;
    int lastPageTotal = 1;
    size_t totalBookChars = 0;
    std::vector<size_t> chapterPrefixSums;
    bool bookMetricsComputed = false;
    int chapterMenuScrollOffset = 0;
    bool bookIndexReady = false;
    bool pendingBookIndexSync = false;
    int pendingBookId = 0;
    int pendingBookChapter = 0;
    size_t pendingBookOffset = 0;
    std::string pendingBookFont;
    float pendingBookFontSize = 1.0f;
    volatile int indexingCurrent = 0;
    volatile int indexingTotal = 0;
    volatile bool indexingScanActive = false;

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
    
    // Image Viewer State
    std::vector<uint8_t> currentImageData;  // Binary data of currently viewed image
    EpubImage currentImageInfo;              // Info about current image
    bool imageViewerActive = false;
    int imageViewerRotation = -1;           // Rotation override (-1 = use current)
    size_t imageTextOffset = 0;             // Text offset where image was clicked
    
    // Rendered images on current page (for tap detection)
    std::vector<RenderedImageInfo> pageRenderedImages;

    void drawStatusBar(LovyanGFX* target = nullptr);
    void drawFooter(LovyanGFX* target, size_t pageOffset, size_t charsOnPage);
    void drawSleepSymbol(const char* symbol);
    void drawMainMenu();        // Main menu with 6 buttons
    void drawLibrary(bool favoritesOnly = false);  // Book list with optional favorites filter
    void drawSearchBar(LovyanGFX* target);
    void drawKeyboard(LovyanGFX* target);
    void drawBookSettings();  // Half-page settings for book in reader (overlay)
    void drawStandaloneSettings(); // Full-page settings (from main menu)
    void drawReader(bool flush = true);
    void drawWifiConfig();
    void drawSettings();
    void drawWifiScan();
    void drawChapterMenu();     // In-book chapter jump menu
    void drawGamesMenu();       // Games selection menu  
    void drawGame();            // Current game rendering
    void drawMusicComposer();   // Music sequencer rendering
    void drawFontSelection();   // Font selection screen (S3 only)
    void drawWifiPassword();
    void drawImageViewer();   // Full-screen image display
    
    void handleTouch();
    void handleTap(int x, int y);
    void handleGesture(const GestureEvent& event);  // Process detected gestures
    void handleButtonPress();   // Handle hardware button press (long press = shutdown)
    void processReaderTap(int x, int y, bool isDouble);
    void onMainMenuClick(int x, int y);  // Handle main menu clicks
    void onWifiScanClick(int x, int y);
    void onWifiPasswordClick(int x, int y);
    void onLibraryClick(int x, int y);
    void onKeyboardClick(int x, int y);
    void onSettingsClick(int x, int y);  // Handle standalone settings clicks
    void onImageViewerClick(int x, int y);  // Handle image viewer clicks
    void onChapterMenuClick(int x, int y);  // Handle chapter menu clicks
    void onGamesMenuClick(int x, int y);    // Handle games menu clicks
    void onMusicComposerClick(int x, int y); // Handle music composer clicks
    void onFontSelectionClick(int x, int y); // Handle font selection clicks
    
    // Image rendering helpers
    bool renderImageAtOffset(size_t offset, M5Canvas* target, int x, int y, int maxWidth, int maxHeight);
    bool openImageViewer(const EpubImage& image);
    void closeImageViewer();

    void processText(std::string &text);
    
    void loadFonts();
    void ensureHebrewFontLoaded();
    void ensureArabicFontLoaded();
    void scanSDCardFonts();  // Scan SD card for .otf fonts
    void drawStringMixed(const std::string &text, int x, int y, M5Canvas *target = nullptr, float size = -1.0f, bool isProcessed = false, bool respectUserFont = true);
    
    void saveSettings();
    void loadSettings();
    void saveLastBook();     // Save current book and page to NVS
    bool loadLastBook();     // Load and open last book, returns success
    void goToSleep();
    void showWallpaperAndSleep();  // Show random wallpaper before sleep (S3 only)
    
    size_t drawPageContent(bool draw);
    size_t drawPageContentAt(size_t startOffset, bool draw, M5Canvas* target = nullptr, volatile bool* abort = nullptr);
    PageInfo calculatePageInfo();
    void resetPageInfoCache();
    void computeBookMetrics();
    void computeBookMetricsLocked();
    void updateNextPrevCanvases();
    
    // UI color helpers for 16-shade grayscale
    uint32_t getTextColor(int brightness = 0) const;  // Returns color based on brightness (0=black)
    uint8_t getGrayShade(int level) const;  // Get grayscale value (0=white, 15=black)

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
