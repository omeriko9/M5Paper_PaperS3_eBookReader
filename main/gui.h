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
    
private:
    AppState currentState = AppState::LIBRARY;
    AppState previousState = AppState::LIBRARY; // For returning from settings
    bool needsRedraw = true;
    BookEntry currentBook;
    
    // Reader State
    size_t currentTextOffset = 0;
    std::vector<size_t> pageHistory;
    
    // Settings
    float fontSize = 1.0f; // Scale factor
    std::string currentFont = "Default";
    bool fontChanged = false;
    
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
};
