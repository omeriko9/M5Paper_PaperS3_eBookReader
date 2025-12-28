/**
 * @file drawing_app.h
 * @brief Drawing Application for M5Paper e-paper display
 * 
 * Features:
 * - 6 thickness levels for brushes (1-6)
 * - Eraser tool with same thickness levels
 * - Partial refresh for smooth drawing on EPD
 * - Stylus support (uses touch coordinates)
 * - Save/Load drawings to SD card
 * - Clear entire drawing
 * 
 * Layout (Portrait 540x960):
 * - Top bar (50px): Back, Clear, Save, Load
 * - Canvas area: Main drawing area
 * - Bottom toolbar (80px): Tool selection, thickness
 */

#pragma once

#include <M5Unified.h>
#include <string>
#include <vector>
#include <functional>

/**
 * @brief Drawing tool types
 */
enum class DrawingTool {
    BRUSH,      // Black brush
    ERASER      // White eraser
};

/**
 * @brief Drawing app state
 */
enum class DrawingState {
    DRAWING,        // Main drawing mode
    FILE_DIALOG,    // Load/Save file browser
    CONFIRM_DIALOG  // Confirmation dialog (clear, overwrite)
};

/**
 * @brief File dialog mode for drawing app
 */
enum class DrawingFileMode {
    SAVE,
    LOAD
};

/**
 * @brief Confirm dialog type
 */
enum class DrawingConfirmType {
    CLEAR,          // Clear the canvas
    OVERWRITE_SAVE, // Overwrite existing file
    NEW_DRAWING     // Create new drawing (discard current)
};

/**
 * @brief Stroke point for undo/redo (not implemented yet, for future)
 */
struct StrokePoint {
    int16_t x;
    int16_t y;
    uint8_t thickness;
    bool isEraser;
};

/**
 * @brief Drawing Application class
 */
class DrawingApp {
public:
    static DrawingApp& getInstance();
    
    DrawingApp(const DrawingApp&) = delete;
    DrawingApp& operator=(const DrawingApp&) = delete;
    
    /**
     * @brief Initialize the drawing app
     */
    void init();
    
    /**
     * @brief Deinitialize and cleanup
     */
    void deinit();
    
    /**
     * @brief Enter the drawing app
     */
    void enter();
    
    /**
     * @brief Exit the drawing app
     */
    void exit();
    
    /**
     * @brief Check if app is active
     */
    bool isActive() const { return m_active; }
    
    /**
     * @brief Draw the current screen
     */
    void draw();
    
    /**
     * @brief Update (called in main loop)
     */
    void update();
    
    /**
     * @brief Handle touch input
     * @return true if touch was handled
     */
    bool handleTouch(int x, int y);
    
    /**
     * @brief Handle touch press (finger down)
     */
    void handleTouchStart(int x, int y);
    
    /**
     * @brief Handle touch move (finger dragging)
     */
    void handleTouchMove(int x, int y);
    
    /**
     * @brief Handle touch release (finger up)
     */
    void handleTouchEnd(int x, int y);
    
    /**
     * @brief Check if should return to menu
     */
    bool shouldExit() const { return m_shouldExit; }
    void clearExitFlag() { m_shouldExit = false; }
    
    // Thickness levels (1-6)
    static constexpr int MIN_THICKNESS = 1;
    static constexpr int MAX_THICKNESS = 6;
    
    // Actual pixel sizes for each thickness level
    static constexpr int THICKNESS_PIXELS[6] = {2, 4, 8, 12, 18, 26};
    
private:
    DrawingApp();
    ~DrawingApp() = default;
    
    // Drawing functions
    void drawTopBar(LovyanGFX* target = nullptr);
    void drawBottomToolbar(LovyanGFX* target = nullptr);
    void drawCanvas(LovyanGFX* target = nullptr);
    void drawFileDialog(LovyanGFX* target = nullptr);
    void drawConfirmDialog(LovyanGFX* target = nullptr);
    void drawButton(LovyanGFX* gfx, int x, int y, int w, int h, 
                    const char* label, bool selected = false, bool enabled = true);
    void drawThicknessButton(LovyanGFX* gfx, int x, int y, int size, int thickness, bool selected);
    
    // Canvas operations
    void clearCanvas();
    void drawStroke(int x0, int y0, int x1, int y1);
    void drawPoint(int x, int y);
    void partialRefreshRegion(int x, int y, int w, int h);
    void fullRefresh();
    
    // File operations
    bool saveDrawing(const std::string& filename);
    bool loadDrawing(const std::string& filename);
    void scanDrawingFiles();
    std::string generateFilename();
    
    // Touch handlers
    bool handleTopBarTouch(int x, int y);
    bool handleToolbarTouch(int x, int y);
    bool handleCanvasTouch(int x, int y);
    bool handleFileDialogTouch(int x, int y);
    bool handleConfirmDialogTouch(int x, int y);
    
    // Layout constants
    static constexpr int SCREEN_WIDTH = 540;
    static constexpr int SCREEN_HEIGHT = 960;
    static constexpr int STATUS_BAR_HEIGHT = 44;
    static constexpr int TOP_BAR_HEIGHT = 50;
    static constexpr int BOTTOM_TOOLBAR_HEIGHT = 100;
    static constexpr int CANVAS_Y = STATUS_BAR_HEIGHT + TOP_BAR_HEIGHT;
    static constexpr int CANVAS_HEIGHT = SCREEN_HEIGHT - CANVAS_Y - BOTTOM_TOOLBAR_HEIGHT;
    
    // Colors (grayscale for EPD)
    static constexpr uint16_t COLOR_WHITE = 0xFFFF;
    static constexpr uint16_t COLOR_BLACK = 0x0000;
    static constexpr uint16_t COLOR_LIGHT_GRAY = 0xDEF7;
    static constexpr uint16_t COLOR_MEDIUM_GRAY = 0x9492;
    static constexpr uint16_t COLOR_DARK_GRAY = 0x4228;
    
    // State
    bool m_active = false;
    bool m_initialized = false;
    bool m_shouldExit = false;
    bool m_canvasCreated = false;
    bool m_needsFullRefresh = true;
    DrawingState m_state = DrawingState::DRAWING;
    DrawingTool m_currentTool = DrawingTool::BRUSH;
    int m_thickness = 3;  // 1-6, default middle
    
    // Touch tracking for drawing
    bool m_isDrawing = false;
    int m_lastX = -1;
    int m_lastY = -1;
    int m_strokeMinX = 0;
    int m_strokeMinY = 0;
    int m_strokeMaxX = 0;
    int m_strokeMaxY = 0;
    bool m_strokeDirty = false;
    uint32_t m_lastRefreshTime = 0;
    static constexpr uint32_t REFRESH_INTERVAL_MS = 100;  // Partial refresh batching
    
    // Canvas sprite (stores the actual drawing)
    M5Canvas m_canvas;
    
    // File dialog state
    DrawingFileMode m_fileMode = DrawingFileMode::SAVE;
    std::vector<std::string> m_fileList;
    int m_fileListScroll = 0;
    int m_selectedFileIndex = -1;
    std::string m_currentFilename;
    
    // Confirm dialog state
    DrawingConfirmType m_confirmType = DrawingConfirmType::CLEAR;
    std::string m_confirmMessage;
    
    // Save directory
    static constexpr const char* DRAWINGS_DIR = "/sdcard/drawings";
};
