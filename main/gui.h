#pragma once
#include <M5Unified.h>
#include "book_index.h"
#include <vector>

enum class AppState {
    LIBRARY,
    READER,
    WIFI_CONFIG,
    SETTINGS
};

class GUI {
public:
    void init();
    void update(); // Main loop
    
    void setWifiStatus(bool connected, int rssi);
    void refreshLibrary();

    void setFontSize(float size);
    void setFont(const std::string& fontName);
    float getFontSize() const { return fontSize; }
    std::string getFont() const { return currentFont; }
    
    void jumpTo(float percent);
    void jumpToChapter(int chapter);
    
private:
    AppState currentState = AppState::LIBRARY;
    AppState previousState = AppState::LIBRARY; // For returning from settings
    bool needsRedraw = true;
    BookEntry currentBook;
    struct PageInfo { int current = 1; int total = 1; };
    
    // Reader State
    size_t currentTextOffset = 0;
    std::vector<size_t> pageHistory;
    
    // Settings
    float fontSize = 1.0f; // Scale factor
    std::string currentFont = "Default";
    bool fontChanged = false;
    std::vector<uint8_t> fontData; // Buffer to hold loaded font in memory
    size_t lastPageChars = 0;
    int lastPageTotal = 1;
    
    void drawStatusBar();
    void drawLibrary();
    void drawReader();
    void drawWifiConfig();
    void drawSettings();
    
    void handleTouch();
    
    void loadFonts();
    void saveSettings();
    void loadSettings();
    void goToSleep();
    
    size_t drawPageContent(bool draw);
    size_t drawPageContentAt(size_t startOffset, bool draw);
    PageInfo calculatePageInfo();
    void resetPageInfoCache();
};
