/**
 * @file composer_ui.h
 * @brief Music Composer UI for M5PaperS3 e-paper display
 * 
 * UI Design for E-paper:
 * - Landscape mode only (960x540)
 * - Piano-roll style grid: X = time (steps), Y = pitch (notes)
 * - Partial refresh during playback (playhead + changed notes only)
 * - Full refresh on screen entry, dialog open
 * - Manual refresh button for ghosting cleanup
 * 
 * Layout:
 * - Top bar (50px): Back, Play/Stop, BPM, Division, Mode, Channel, Refresh
 * - Main area: Piano-roll grid (time X, pitch Y)
 * - Bottom bar (70px): Load, Save, MIDI, Export, Instruments, Clear
 * 
 * Touch interactions:
 * - Tap empty cell: Add note
 * - Tap note: Select for editing
 * - Double tap note: Toggle enable/mute
 * - Drag note left/right: Extend/shorten duration
 * - Drag grid up/down: Scroll octaves
 * - Long press: Context menu
 */

#pragma once

#include <M5Unified.h>
#include "sequencer_engine.h"
#include "gesture_detector.h"
#include <algorithm>
#include <string>
#include <vector>
#include <functional>

/**
 * @brief UI state for the composer
 */
enum class ComposerUIState {
    MAIN,           // Main sequencer view
    FILE_DIALOG,    // Load/Save/Export file browser
    CONFIRM_DIALOG, // Confirmation (clear, overwrite, etc.)
    PITCH_PICKER,   // Pitch selection popup (simplified)
    DURATION_PICKER,// Duration selection popup
    VELOCITY_PICKER,// Velocity selection popup
    DIVISION_PICKER,// Step division selection popup
    TEMPO_INPUT,    // BPM input
    CONTEXT_MENU,   // Long-press context menu
    INSTRUMENT_PICKER // MIDI instrument selection per channel
};

/**
 * @brief File dialog mode
 */
enum class FileDialogMode {
    LOAD_PROJECT,
    SAVE_PROJECT,
    LOAD_MIDI,
    EXPORT_MIDI
};

/**
 * @brief Context menu option
 */
struct ContextMenuItem {
    std::string label;
    std::function<void()> action;
};

/**
 * @brief Dirty region tracking for partial refresh
 */
struct DirtyRegion {
    int x, y, w, h;
    bool isDirty;
    
    DirtyRegion() : x(0), y(0), w(0), h(0), isDirty(false) {}
    
    void mark(int _x, int _y, int _w, int _h) {
        if (!isDirty) {
            x = _x; y = _y; w = _w; h = _h;
            isDirty = true;
        } else {
            // Expand region
            int x2 = std::max(x + w, _x + _w);
            int y2 = std::max(y + h, _y + _h);
            x = std::min(x, _x);
            y = std::min(y, _y);
            w = x2 - x;
            h = y2 - y;
        }
    }
    
    void clear() { isDirty = false; }
};

/**
 * @brief Music Composer UI class
 */
class ComposerUI {
public:
    static ComposerUI& getInstance();
    
    ComposerUI(const ComposerUI&) = delete;
    ComposerUI& operator=(const ComposerUI&) = delete;
    
    void init();
    void deinit();
    bool isActive() const { return m_active; }
    void enter();
    void exit();
    bool handleTouch(int x, int y);
    bool handleGesture(const GestureEvent& event);
    bool handleLongPress(int x, int y);
    bool handleDoubleTap(int x, int y);
    void update();
    void draw();
    bool shouldExit() const { return m_shouldExit; }
    void clearExitFlag() { m_shouldExit = false; }
    bool isCanvasCreated() const { return m_canvasCreated; }
    void forceFullRefresh() { m_needsFullRefresh = true; }
    
    // Drag handling
    void handleDragStart(int x, int y);
    void handleDragMove(int x, int y);
    void handleDragEnd(int x, int y);

private:
    ComposerUI();
    ~ComposerUI() = default;
    
    // Drawing functions
    void drawTopBar(LovyanGFX* target = nullptr);
    void drawGrid(LovyanGFX* target = nullptr);
    void drawPianoKeys(LovyanGFX* target = nullptr);
    void drawBottomBar(LovyanGFX* target = nullptr);
    void drawPlayhead(LovyanGFX* target = nullptr);
    void drawFileDialog(LovyanGFX* target = nullptr);
    void drawConfirmDialog(LovyanGFX* target = nullptr);
    void drawPitchPicker(LovyanGFX* target = nullptr);
    void drawDurationPicker(LovyanGFX* target = nullptr);
    void drawVelocityPicker(LovyanGFX* target = nullptr);
    void drawDivisionPicker(LovyanGFX* target = nullptr);
    void drawTempoInput(LovyanGFX* target = nullptr);
    void drawContextMenu(LovyanGFX* target = nullptr);
    void drawInstrumentPicker(LovyanGFX* target = nullptr);
    void drawButton(LovyanGFX* gfx, int x, int y, int w, int h, 
                    const char* label, bool selected = false, bool enabled = true);
    
    // Touch handlers
    bool handleMainTouch(int x, int y);
    bool handleTopBarTouch(int x, int y);
    bool handleGridTouch(int x, int y);
    bool handleBottomBarTouch(int x, int y);
    bool handleFileDialogTouch(int x, int y);
    bool handleConfirmDialogTouch(int x, int y);
    bool handlePickerTouch(int x, int y);
    bool handlePitchPickerTouch(int x, int y);
    bool handleDurationPickerTouch(int x, int y);
    bool handleVelocityPickerTouch(int x, int y);
    bool handleDivisionPickerTouch(int x, int y);
    bool handleTempoInputTouch(int x, int y);
    bool handleContextMenuTouch(int x, int y);
    bool handleInstrumentPickerTouch(int x, int y);
    
    // Partial refresh helpers
    void updatePlayheadPartial();
    void markGridCellDirty(int step, int pitch);
    void flushDirtyRegions();
    void pushCanvasRect(int dx, int dy, int sx, int sy, int sw, int sh);
    
    // File operations
    void loadFileList();
    void doLoadProject(const std::string& filename);
    void doSaveProject(const std::string& filename);
    void doLoadMidi(const std::string& filename);
    void doExportMidi(const std::string& filename);
    
    // Context menu
    void showContextMenu(int x, int y, const std::vector<ContextMenuItem>& items);
    void showNoteContextMenu(int x, int y, int channel, int noteIndex);
    void showEmptyCellContextMenu(int x, int y, int step, int pitch);
    
    // Grid coordinate helpers
    int stepToGridX(uint16_t step) const;
    int pitchToGridY(uint8_t pitch) const;
    int gridXToStep(int x) const;
    int gridYToPitch(int y) const;
    bool isInGrid(int x, int y) const;
    
    // Step callback from engine
    static void onStepChange(uint16_t step);
    
    // UI state
    bool m_active;
    bool m_shouldExit;
    bool m_needsFullRefresh;
    bool m_initialized;
    ComposerUIState m_state;
    
    // View mode
    bool m_singleChannelMode;    // true = show only selected channel expanded
    int m_selectedChannel;       // 0 or 1
    int m_selectedNoteIndex;     // -1 if no note selected
    
    // Default values for new notes
    uint8_t m_defaultPitch;
    uint8_t m_defaultVelocity;
    uint16_t m_defaultDuration;
    
    // Grid view - Piano roll style
    uint16_t m_viewStartStep;    // First visible step (X scroll)
    uint8_t m_viewStartPitch;    // Lowest visible pitch (Y scroll / octave)
    uint8_t m_pitchRange;        // Number of visible pitches
    uint16_t m_stepsVisible;     // Number of visible steps
    
    // MIDI instruments per channel (GM program numbers 0-127)
    int m_instrumentPickerChannel; // Which channel we're editing
    
    // Playhead tracking
    uint16_t m_lastPlayheadStep;
    int m_lastPlayheadX;
    
    // Drag state
    bool m_isDragging;
    int m_dragStartX, m_dragStartY;
    int m_dragNoteIndex;
    int m_dragChannel;
    enum class DragMode { NONE, DURATION, OCTAVE_SCROLL, STEP_SCROLL } m_dragMode;
    uint16_t m_dragOriginalDuration;
    uint8_t m_dragOriginalPitch;
    bool m_ignoreTap;
    
    // File dialog
    FileDialogMode m_fileDialogMode;
    std::vector<std::string> m_fileList;
    int m_fileListScroll;
    int m_selectedFileIndex;
    std::string m_inputFilename;
    
    // Confirm dialog
    std::string m_confirmMessage;
    std::function<void()> m_confirmAction;
    
    // Context menu
    std::vector<ContextMenuItem> m_contextMenuItems;
    int m_contextMenuX, m_contextMenuY;
    
    // Dirty tracking for partial refresh
    DirtyRegion m_playheadDirty;
    DirtyRegion m_gridDirty;
    DirtyRegion m_topDirty;
    DirtyRegion m_bottomDirty;
    
    // Auto refresh disabled when not playing
    uint32_t m_lastFullRefreshTime;
    
    // Canvas for buffered drawing
    M5Canvas m_canvas;
    bool m_canvasCreated;
    
    // Layout constants (computed for landscape)
    struct Layout {
        int screenW, screenH;
        int topBarH;
        int bottomBarH;
        int pianoKeysW;      // Width of piano keys on left
        int gridX, gridY, gridW, gridH;
        int cellW;           // Width per step
        int cellH;           // Height per pitch
        int pitchesVisible;  // Number of pitches shown
        int stepsVisible;    // Number of steps shown
    };
    Layout m_layout;
    
    void computeLayout();
    
    // GM instrument names (first 16 for simplicity)
    static const char* getInstrumentName(uint8_t program);
};
