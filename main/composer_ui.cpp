/**
 * @file composer_ui.cpp
 * @brief Implementation of the Music Composer UI
 */

#include "composer_ui.h"
#include "device_hal.h"
#include "midi_io.h"
#include "project_io.h"
#include "esp_log.h"
#include <algorithm>
#include <cmath>

static const char* TAG = "ComposerUI";

// Colors for 16-shade grayscale (M5PaperS3)
static constexpr uint16_t COLOR_WHITE = 0xFFFF;
static constexpr uint16_t COLOR_BLACK = 0x0000;
static constexpr uint16_t COLOR_LIGHT_GRAY = 0xDEF7;
static constexpr uint16_t COLOR_MEDIUM_GRAY = 0x9492;
static constexpr uint16_t COLOR_DARK_GRAY = 0x4228;

static int findNoteIndexAtPosition(const std::vector<NoteEvent>& notes, int step, int pitch, bool includeDisabled) {
    for (size_t i = 0; i < notes.size(); ++i) {
        const auto& note = notes[i];
        if (!includeDisabled && !note.enabled) continue;
        if (note.pitch != pitch) continue;
        if (step >= (int)note.startStep && step < (int)(note.startStep + note.duration)) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

ComposerUI& ComposerUI::getInstance() {
    static ComposerUI instance;
    return instance;
}

ComposerUI::ComposerUI()
    : m_active(false)
    , m_shouldExit(false)
    , m_needsFullRefresh(true)
    , m_initialized(false)
    , m_state(ComposerUIState::MAIN)
    , m_singleChannelMode(false)
    , m_selectedChannel(0)
    , m_selectedNoteIndex(-1)
    , m_defaultPitch(60)
    , m_defaultVelocity(100)
    , m_defaultDuration(1)
    , m_viewStartStep(0)
    , m_viewStartPitch(48)
    , m_pitchRange(24)
    , m_stepsVisible(64)
    , m_instrumentPickerChannel(0)
    , m_lastPlayheadStep(0)
    , m_lastPlayheadX(-1)
    , m_isDragging(false)
    , m_dragStartX(0)
    , m_dragStartY(0)
    , m_dragNoteIndex(-1)
    , m_dragChannel(-1)
    , m_dragMode(DragMode::NONE)
    , m_dragOriginalDuration(1)
    , m_dragOriginalPitch(60)
    , m_ignoreTap(false)
    , m_fileDialogMode(FileDialogMode::LOAD_PROJECT)
    , m_fileListScroll(0)
    , m_selectedFileIndex(-1)
    , m_lastFullRefreshTime(0)
    , m_canvasCreated(false)
{
}

void ComposerUI::init() {
    if (m_initialized) return;
    
    ESP_LOGI(TAG, "Initializing Composer UI");
    computeLayout();
    
    // Initialize engine
    SequencerEngine::getInstance().init();
    
    // Set step callback
    SequencerEngine::getInstance().setStepCallback([this](uint16_t step) {
        this->onStepChange(step);
    });
    
    m_initialized = true;
}

void ComposerUI::deinit() {
    if (!m_initialized) return;
    
    SequencerEngine::getInstance().stop();
    SequencerEngine::getInstance().deinit();
    
    if (m_canvasCreated) {
        m_canvas.deleteSprite();
        m_canvasCreated = false;
    }
    
    m_initialized = false;
}

void ComposerUI::computeLayout() {
    m_layout.screenW = M5.Display.width();
    m_layout.screenH = M5.Display.height();
    
    // Ensure landscape
    if (m_layout.screenW < m_layout.screenH) {
        std::swap(m_layout.screenW, m_layout.screenH);
    }
    
    m_layout.topBarH = 50;
    m_layout.bottomBarH = 70;

    m_layout.pianoKeysW = 80;
    m_layout.gridX = m_layout.pianoKeysW + 10;
    m_layout.gridY = m_layout.topBarH + 10;
    m_layout.gridW = m_layout.screenW - m_layout.gridX - 20;
    m_layout.gridH = m_layout.screenH - m_layout.topBarH - m_layout.bottomBarH - 20;
    
    const auto& comp = SequencerEngine::getInstance().getComposition();
    int stepsPerBar = comp.stepsPerBar();
    if (stepsPerBar <= 0) stepsPerBar = 16;
    
    int barsVisible = 4;
    const int minCellW = 8;
    while (barsVisible > 1 && (m_layout.gridW / (stepsPerBar * barsVisible)) < minCellW) {
        barsVisible--;
    }
    
    m_layout.stepsVisible = stepsPerBar * barsVisible;
    m_layout.cellW = std::max(4, m_layout.gridW / m_layout.stepsVisible);
    
    int targetPitches = 24;
    int maxPitches = std::max(12, m_layout.gridH / 14);
    if (targetPitches > maxPitches) {
        targetPitches = maxPitches - (maxPitches % 12);
        if (targetPitches < 12) targetPitches = 12;
    }
    
    m_layout.pitchesVisible = targetPitches;
    m_layout.cellH = std::max(10, m_layout.gridH / m_layout.pitchesVisible);
    
    m_stepsVisible = m_layout.stepsVisible;
    m_pitchRange = m_layout.pitchesVisible;
    
    if (m_viewStartPitch + m_pitchRange > 128) {
        m_viewStartPitch = (m_pitchRange > 128) ? 0 : (128 - m_pitchRange);
    }
    
    uint16_t totalSteps = comp.totalSteps();
    if (totalSteps > 0 && m_viewStartStep + m_stepsVisible > totalSteps) {
        m_viewStartStep = (totalSteps > m_stepsVisible) ? (totalSteps - m_stepsVisible) : 0;
    }
}

void ComposerUI::enter() {
    ESP_LOGI(TAG, "Entering Composer");
    m_active = true;
    m_shouldExit = false;
    m_needsFullRefresh = true;
    m_state = ComposerUIState::MAIN;
    m_isDragging = false;
    m_ignoreTap = false;
    
    // Ensure landscape mode
    DeviceHAL::getInstance().setRotation(1);
    computeLayout();
    
    // Create canvas if needed
    if (!m_canvasCreated || m_canvas.width() != m_layout.screenW || m_canvas.height() != m_layout.screenH) {
        if (m_canvasCreated) {
            m_canvas.deleteSprite();
            m_canvasCreated = false;
        }

        int targetDepth = DeviceHAL::getInstance().getCanvasColorDepth();
        size_t neededBytes = ((size_t)m_layout.screenW * m_layout.screenH * targetDepth + 7) / 8;
        size_t freeDefault = heap_caps_get_free_size(MALLOC_CAP_DEFAULT);
        size_t freeSPIRAM = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
        size_t largestDefault = heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT);
        size_t largestSPIRAM = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);

        ESP_LOGI(TAG, "ComposerUI: attempting canvas create (w=%d h=%d depth=%dbpp ~%u bytes). Free: default=%u SPIRAM=%u largest: default=%u SPIRAM=%u",
                 m_layout.screenW, m_layout.screenH, targetDepth, (unsigned)neededBytes,
                 (unsigned)freeDefault, (unsigned)freeSPIRAM, (unsigned)largestDefault, (unsigned)largestSPIRAM);

        // Try preferred: PSRAM allocation
        m_canvas.setColorDepth(targetDepth);
        m_canvas.setPsram(true);
        if (m_canvas.createSprite(m_layout.screenW, m_layout.screenH)) {
            m_canvasCreated = true;
            ESP_LOGI(TAG, "ComposerUI: created canvas in PSRAM %dx%d depth=%d", m_layout.screenW, m_layout.screenH, targetDepth);
        } else {
            // Try internal RAM
            ESP_LOGW(TAG, "ComposerUI: PSRAM allocation failed, trying internal RAM");
            m_canvas.setPsram(false);
            if (m_canvas.createSprite(m_layout.screenW, m_layout.screenH)) {
                m_canvasCreated = true;
                ESP_LOGI(TAG, "ComposerUI: created canvas in internal RAM %dx%d depth=%d", m_layout.screenW, m_layout.screenH, targetDepth);
            } else if (targetDepth > 2) {
                // Reduce color depth and retry (lower memory footprint)
                ESP_LOGW(TAG, "ComposerUI: internal RAM allocation failed, retrying with reduced color depth (2bpp)");
                targetDepth = 2;
                m_canvas.setColorDepth(targetDepth);
                m_canvas.setPsram(true);
                if (m_canvas.createSprite(m_layout.screenW, m_layout.screenH)) {
                    m_canvasCreated = true;
                    ESP_LOGI(TAG, "ComposerUI: created canvas in PSRAM with reduced depth %dx%d depth=%d", m_layout.screenW, m_layout.screenH, targetDepth);
                } else {
                    ESP_LOGW(TAG, "ComposerUI: PSRAM allocation with reduced depth failed, trying internal RAM");
                    m_canvas.setPsram(false);
                    if (m_canvas.createSprite(m_layout.screenW, m_layout.screenH)) {
                        m_canvasCreated = true;
                        ESP_LOGI(TAG, "ComposerUI: created canvas in internal RAM with reduced depth %dx%d depth=%d", m_layout.screenW, m_layout.screenH, targetDepth);
                    } else {
                        ESP_LOGE(TAG, "ComposerUI: all canvas allocation attempts failed (needed ~%u bytes). Falling back to direct display drawing.", (unsigned)neededBytes);
                        m_canvasCreated = false;
                    }
                }
            } else {
                ESP_LOGE(TAG, "ComposerUI: canvas allocation failed and no fallback available. Falling back to direct display drawing.");
                m_canvasCreated = false;
            }
        }
    }

    // If canvas couldn't be created, we'll draw directly to the display (slower, but functional)
    if (!m_canvasCreated) {
        ESP_LOGW(TAG, "ComposerUI: operating in direct-draw mode (no sprite)");
    }

    draw();
}

void ComposerUI::exit() {
    ESP_LOGI(TAG, "Exiting Composer");
    SequencerEngine::getInstance().stop();
    m_active = false;
}

void ComposerUI::onStepChange(uint16_t step) {
    // This is called from the timer task, so we just mark for update
    // The update() call in main loop will handle the actual drawing
    getInstance().m_playheadDirty.isDirty = true;
}

void ComposerUI::update() {
    if (!m_active) return;
    
    // Handle playhead movement during playback
    if (SequencerEngine::getInstance().isPlaying() || m_playheadDirty.isDirty) {
        updatePlayheadPartial();
    }
    
    if (m_needsFullRefresh) {
        draw();
        m_needsFullRefresh = false;
        m_lastFullRefreshTime = (uint32_t)(esp_timer_get_time() / 1000);
    }
    
    // Flush any other dirty regions (e.g. note toggles)
    flushDirtyRegions();
}

void ComposerUI::draw() {
    if (!m_active) return;

    if (m_canvasCreated) {
        m_canvas.fillScreen(COLOR_WHITE);

        drawTopBar(&m_canvas);
        drawGrid(&m_canvas);
        drawBottomBar(&m_canvas);
        drawPlayhead(&m_canvas);

        if (m_state == ComposerUIState::FILE_DIALOG) {
            drawFileDialog(&m_canvas);
        } else if (m_state == ComposerUIState::CONFIRM_DIALOG) {
            drawConfirmDialog(&m_canvas);
        } else if (m_state == ComposerUIState::PITCH_PICKER) {
            drawPitchPicker(&m_canvas);
        } else if (m_state == ComposerUIState::DURATION_PICKER) {
            drawDurationPicker(&m_canvas);
        } else if (m_state == ComposerUIState::VELOCITY_PICKER) {
            drawVelocityPicker(&m_canvas);
        } else if (m_state == ComposerUIState::DIVISION_PICKER) {
            drawDivisionPicker(&m_canvas);
        } else if (m_state == ComposerUIState::TEMPO_INPUT) {
            drawTempoInput(&m_canvas);
        } else if (m_state == ComposerUIState::CONTEXT_MENU) {
            drawContextMenu(&m_canvas);
        } else if (m_state == ComposerUIState::INSTRUMENT_PICKER) {
            drawInstrumentPicker(&m_canvas);
        }

        m_canvas.pushSprite(&M5.Display, 0, 0);
        M5.Display.display();
    } else {
        // Fallback: draw directly to display (no sprite available)
        M5.Display.fillScreen(COLOR_WHITE);
        drawTopBar(&M5.Display);
        drawGrid(&M5.Display);
        drawBottomBar(&M5.Display);
        drawPlayhead(&M5.Display);

        if (m_state == ComposerUIState::FILE_DIALOG) {
            drawFileDialog(&M5.Display);
        } else if (m_state == ComposerUIState::CONFIRM_DIALOG) {
            drawConfirmDialog(&M5.Display);
        } else if (m_state == ComposerUIState::PITCH_PICKER) {
            drawPitchPicker(&M5.Display);
        } else if (m_state == ComposerUIState::DURATION_PICKER) {
            drawDurationPicker(&M5.Display);
        } else if (m_state == ComposerUIState::VELOCITY_PICKER) {
            drawVelocityPicker(&M5.Display);
        } else if (m_state == ComposerUIState::DIVISION_PICKER) {
            drawDivisionPicker(&M5.Display);
        } else if (m_state == ComposerUIState::TEMPO_INPUT) {
            drawTempoInput(&M5.Display);
        } else if (m_state == ComposerUIState::CONTEXT_MENU) {
            drawContextMenu(&M5.Display);
        } else if (m_state == ComposerUIState::INSTRUMENT_PICKER) {
            drawInstrumentPicker(&M5.Display);
        }

        M5.Display.display();
    }

    m_gridDirty.clear();
    m_topDirty.clear();
    m_bottomDirty.clear();
    m_playheadDirty.clear();
    m_lastFullRefreshTime = (uint32_t)(esp_timer_get_time() / 1000);
}

void ComposerUI::drawButton(LovyanGFX* gfx, int x, int y, int w, int h, 
                            const char* label, bool selected, bool enabled) {
    uint16_t fill = enabled ? (selected ? COLOR_DARK_GRAY : COLOR_WHITE) : COLOR_LIGHT_GRAY;
    uint16_t text = enabled ? (selected ? COLOR_WHITE : COLOR_BLACK) : COLOR_BLACK;
    
    gfx->fillRect(x, y, w, h, fill);
    gfx->drawRect(x, y, w, h, COLOR_BLACK);
    gfx->setTextDatum(textdatum_t::middle_center);
    gfx->setTextColor(text);
    gfx->drawString(label, x + w / 2, y + h / 2);
}

void ComposerUI::drawTopBar(LovyanGFX* target) {
    LovyanGFX* gfx = target ? target : &m_canvas;
    int y = 0;
    int h = m_layout.topBarH;
    int w = m_layout.screenW;
    
    gfx->fillRect(0, y, w, h, COLOR_LIGHT_GRAY);
    gfx->drawRect(0, y, w, h, COLOR_BLACK);
    
    gfx->setTextDatum(textdatum_t::middle_center);
    gfx->setTextSize(1.4f);
    
    int btnY = 5;
    int btnH = h - 10;
    int x = 10;
    
    drawButton(gfx, x, btnY, 70, btnH, "Back");
    x += 80;
    
    bool playing = SequencerEngine::getInstance().isPlaying();
    drawButton(gfx, x, btnY, 70, btnH, playing ? "Stop" : "Play");
    x += 80;
    
    char buf[32];
    snprintf(buf, sizeof(buf), "BPM %d", SequencerEngine::getInstance().getTempo());
    drawButton(gfx, x, btnY, 120, btnH, buf);
    x += 130;
    
    int div = static_cast<int>(SequencerEngine::getInstance().getDivision());
    snprintf(buf, sizeof(buf), "1/%d", div);
    drawButton(gfx, x, btnY, 90, btnH, buf);
    x += 100;
    
    drawButton(gfx, x, btnY, 90, btnH, m_singleChannelMode ? "1ch" : "2ch");
    
    int refreshW = 90;
    int refreshX = w - refreshW - 10;
    int channelW = 150;
    int channelX = refreshX - channelW - 10;
    
    drawButton(gfx, channelX, btnY, channelW / 2, btnH, "Ch1", m_selectedChannel == 0, true);
    drawButton(gfx, channelX + channelW / 2, btnY, channelW / 2, btnH, "Ch2", m_selectedChannel == 1, true);
    
    drawButton(gfx, refreshX, btnY, refreshW, btnH, "Refresh");
}

void ComposerUI::drawGrid(LovyanGFX* target) {
    LovyanGFX* gfx = target ? target : &m_canvas;
    const auto& comp = SequencerEngine::getInstance().getComposition();
    
    int gx = m_layout.gridX;
    int gy = m_layout.gridY;
    int gw = m_layout.gridW;
    int gh = m_layout.gridH;
    
    drawPianoKeys(gfx);
    
    // Draw grid background
    gfx->fillRect(gx, gy, gw, gh, COLOR_WHITE);
    gfx->drawRect(gx, gy, gw, gh, COLOR_BLACK);
    
    // Draw position indicator
    char posStr[32];
    int stepsPerBar = std::max(static_cast<uint16_t>(1), comp.stepsPerBar());
    int currentBar = m_viewStartStep / stepsPerBar + 1;
    int totalBars = comp.lengthBars;
    int barsVisible = std::max(1, m_layout.stepsVisible / stepsPerBar);
    snprintf(posStr, sizeof(posStr), "Bar %d-%d/%d", currentBar, 
             std::min(currentBar + barsVisible - 1, totalBars), totalBars);
    gfx->setTextSize(1.0f);
    gfx->setTextColor(COLOR_BLACK);
    gfx->setTextDatum(textdatum_t::middle_center);
    gfx->drawString(posStr, gx + gw / 2, gy - 8);
    
    // Draw vertical lines (steps and bars)
    int stepsPerBeat = (comp.timeSignatureNum > 0) ? (stepsPerBar / comp.timeSignatureNum) : 0;
    for (int i = 0; i <= m_layout.stepsVisible; i++) {
        int x = gx + i * m_layout.cellW;
        if (i % stepsPerBar == 0) {
            gfx->drawFastVLine(x, gy, gh, COLOR_BLACK);
            gfx->drawFastVLine(x + 1, gy, gh, COLOR_BLACK); // Thicker bar line
        } else if (stepsPerBeat > 0 && i % stepsPerBeat == 0) {
            gfx->drawFastVLine(x, gy, gh, COLOR_MEDIUM_GRAY);
        } else {
            // Light dotted line or very light gray
            for (int dy = 0; dy < gh; dy += 4) {
                gfx->drawPixel(x, gy + dy, COLOR_LIGHT_GRAY);
            }
        }
    }
    
    // Draw horizontal pitch lines
    for (int row = 0; row < m_pitchRange; row++) {
        int y = gy + row * m_layout.cellH;
        uint8_t pitch = static_cast<uint8_t>(m_viewStartPitch + (m_pitchRange - 1 - row));
        uint16_t lineColor = (pitch % 12 == 0) ? COLOR_MEDIUM_GRAY : COLOR_LIGHT_GRAY;
        gfx->drawFastHLine(gx, y, gw, lineColor);
    }
    
    // Draw notes with selection highlighting
    auto drawNotes = [&](const std::vector<NoteEvent>& notes, int channel) {
        uint16_t baseColor = (channel == 0) ? COLOR_DARK_GRAY : COLOR_MEDIUM_GRAY;
        for (size_t i = 0; i < notes.size(); i++) {
            const auto& note = notes[i];
            
            // Check if note is visible
            if (note.startStep + note.duration <= m_viewStartStep) continue;
            if (note.startStep >= m_viewStartStep + m_layout.stepsVisible) continue;
            if (note.pitch < m_viewStartPitch || note.pitch >= m_viewStartPitch + m_pitchRange) continue;
            
            int startInView = std::max((int)note.startStep, (int)m_viewStartStep);
            int endInView = std::min((int)(note.startStep + note.duration), 
                                     (int)(m_viewStartStep + m_layout.stepsVisible));
            
            int nx = gx + (startInView - m_viewStartStep) * m_layout.cellW;
            int nw = (endInView - startInView) * m_layout.cellW;
            if (nw <= 0) continue;
            
            int ny = pitchToGridY(note.pitch);
            if (ny < 0) continue;
            
            bool isSelected = (channel == m_selectedChannel && (int)i == m_selectedNoteIndex);
            uint16_t fillColor = note.enabled ? baseColor : COLOR_LIGHT_GRAY;
            int padX = std::min(2, m_layout.cellW / 3);
            int padY = std::min(2, m_layout.cellH / 4);
            int drawW = std::max(2, nw - padX * 2);
            int drawH = std::max(2, m_layout.cellH - padY * 2);
            
            gfx->fillRect(nx + padX, ny + padY, drawW, drawH, fillColor);
            gfx->drawRect(nx + padX, ny + padY, drawW, drawH, COLOR_BLACK);
            
            if (isSelected) {
                gfx->drawRect(nx + padX - 1, ny + padY - 1, drawW + 2, drawH + 2, COLOR_BLACK);
            }
            
            if (isSelected && m_layout.cellW >= 16) {
                gfx->setTextColor(COLOR_WHITE);
                gfx->setTextSize(1.0f);
                gfx->setTextDatum(textdatum_t::middle_center);
                int labelX = nx + padX + drawW / 2;
                gfx->drawString(SequencerEngine::midiToNoteName(note.pitch), labelX, ny + m_layout.cellH / 2);
            }
        }
    };
    
    if (m_singleChannelMode) {
        if (m_selectedChannel == 0) {
            drawNotes(comp.channel1, 0);
        } else {
            drawNotes(comp.channel2, 1);
        }
    } else {
        drawNotes(comp.channel1, 0);
        drawNotes(comp.channel2, 1);
    }
    
    // Reset text color
    gfx->setTextColor(COLOR_BLACK);
}

void ComposerUI::drawPianoKeys(LovyanGFX* target) {
    LovyanGFX* gfx = target ? target : &m_canvas;
    int x = 0;
    int y = m_layout.gridY;
    int w = m_layout.pianoKeysW;
    int h = m_layout.gridH;
    
    gfx->fillRect(x, y, w, h, COLOR_WHITE);
    gfx->drawRect(x, y, w, h, COLOR_BLACK);
    
    static const bool isBlack[12] = {false, true, false, true, false, false, true, false, true, false, true, false};
    
    gfx->setTextDatum(textdatum_t::middle_left);
    for (int row = 0; row < m_pitchRange; row++) {
        int ky = y + row * m_layout.cellH;
        uint8_t pitch = static_cast<uint8_t>(m_viewStartPitch + (m_pitchRange - 1 - row));
        bool black = isBlack[pitch % 12];
        uint16_t fill = black ? COLOR_DARK_GRAY : COLOR_WHITE;
        uint16_t text = black ? COLOR_WHITE : COLOR_BLACK;
        
        gfx->fillRect(x, ky, w, m_layout.cellH, fill);
        gfx->drawRect(x, ky, w, m_layout.cellH, COLOR_BLACK);
        
        if ((pitch % 12 == 0) || m_layout.cellH >= 18) {
            gfx->setTextColor(text);
            gfx->setTextSize(1.0f);
            gfx->drawString(SequencerEngine::midiToNoteName(pitch), x + 6, ky + m_layout.cellH / 2);
        }
    }
}

void ComposerUI::drawBottomBar(LovyanGFX* target) {
    LovyanGFX* gfx = target ? target : &m_canvas;
    int y = m_layout.screenH - m_layout.bottomBarH;
    int w = m_layout.screenW;
    int h = m_layout.bottomBarH;
    
    gfx->fillRect(0, y, w, h, COLOR_LIGHT_GRAY);
    gfx->drawRect(0, y, w, h, COLOR_BLACK);
    
    gfx->setTextDatum(textdatum_t::middle_center);
    gfx->setTextSize(1.3f);
    
    // Note editing buttons (only active if note selected)
    bool noteSelected = (m_selectedNoteIndex != -1);
    
    int btnY = y + 10;
    int btnH = h - 20;
    int btnW = 85;
    int gap = 6;
    int x0 = 10;
    
    auto bx = [&](int index) { return x0 + index * (btnW + gap); };
    
    drawButton(gfx, bx(0), btnY, btnW, btnH, "Pitch", false, noteSelected);
    drawButton(gfx, bx(1), btnY, btnW, btnH, "Dur", false, noteSelected);
    drawButton(gfx, bx(2), btnY, btnW, btnH, "Vel", false, noteSelected);
    drawButton(gfx, bx(3), btnY, btnW, btnH, "Del", false, noteSelected);
    drawButton(gfx, bx(4), btnY, btnW, btnH, "Clear");
    drawButton(gfx, bx(5), btnY, btnW, btnH, "Load");
    drawButton(gfx, bx(6), btnY, btnW, btnH, "MIDI");
    drawButton(gfx, bx(7), btnY, btnW, btnH, "Save");
    drawButton(gfx, bx(8), btnY, btnW, btnH, "Export");
    
    const auto& comp = SequencerEngine::getInstance().getComposition();
    uint8_t inst = (m_selectedChannel == 0) ? comp.channel1Instrument : comp.channel2Instrument;
    char instLabel[12];
    snprintf(instLabel, sizeof(instLabel), "I%d", inst);
    drawButton(gfx, bx(9), btnY, btnW, btnH, instLabel);
}

void ComposerUI::drawPlayhead(LovyanGFX* target) {
    LovyanGFX* gfx = target ? target : &m_canvas;
    uint16_t step = SequencerEngine::getInstance().getPosition();
    
    if (step >= m_viewStartStep && step < m_viewStartStep + m_layout.stepsVisible) {
        int x = stepToGridX(step);
        
        // Draw playhead line
        gfx->drawFastVLine(x, m_layout.gridY, m_layout.gridH, COLOR_BLACK);
        gfx->drawFastVLine(x + 1, m_layout.gridY, m_layout.gridH, COLOR_BLACK);
        
        // Draw playhead triangle at top
        gfx->fillTriangle(x - 5, m_layout.gridY - 10, x + 5, m_layout.gridY - 10, x, m_layout.gridY, COLOR_BLACK);
        
        m_lastPlayheadX = x;
        m_lastPlayheadStep = step;
    }
}

void ComposerUI::pushCanvasRect(int dx, int dy, int sx, int sy, int sw, int sh) {
    if (!m_canvasCreated) return;
    M5.Display.setClipRect(dx, dy, sw, sh);
    m_canvas.pushSprite(&M5.Display, dx - sx, dy - sy);
    M5.Display.clearClipRect();
}

void ComposerUI::updatePlayheadPartial() {
    // If no canvas is available (allocation failed), fall back to full redraw
    if (!m_canvasCreated) {
        draw();
        m_playheadDirty.isDirty = false;
        return;
    }

    uint16_t step = SequencerEngine::getInstance().getPosition();
    if (step == m_lastPlayheadStep && !m_playheadDirty.isDirty) return;
    
    // Calculate old and new positions
    int oldX = m_lastPlayheadX;
    int newX = stepToGridX(step);
    
    // If playhead moved out of view, we might need a full redraw or scroll
    if (step < m_viewStartStep || step >= m_viewStartStep + m_layout.stepsVisible) {
        // For now, just wrap around or scroll by 4 bars
        m_viewStartStep = (step / m_layout.stepsVisible) * m_layout.stepsVisible;
        m_needsFullRefresh = true;
        return;
    }
    
    // Partial refresh:
    // 1. Erase old playhead (restore grid)
    if (oldX >= m_layout.gridX) {
        // Push the area from canvas back to display to "erase" the playhead
        pushCanvasRect(oldX - 5, m_layout.gridY - 10, oldX - 5, m_layout.gridY - 10, 11, m_layout.gridH + 10);
    }
    
    // 2. Draw new playhead
    m_lastPlayheadStep = step;
    m_lastPlayheadX = newX;
    
    // Draw to display directly for speed
    M5.Display.drawFastVLine(newX, m_layout.gridY, m_layout.gridH, COLOR_BLACK);
    M5.Display.drawFastVLine(newX + 1, m_layout.gridY, m_layout.gridH, COLOR_BLACK);
    M5.Display.fillTriangle(newX - 5, m_layout.gridY - 10, newX + 5, m_layout.gridY - 10, newX, m_layout.gridY, COLOR_BLACK);
    
    m_playheadDirty.isDirty = false;
}

void ComposerUI::flushDirtyRegions() {
    if (!m_canvasCreated) {
        if (m_gridDirty.isDirty || m_topDirty.isDirty || m_bottomDirty.isDirty) {
            draw();
            m_gridDirty.clear();
            m_topDirty.clear();
            m_bottomDirty.clear();
        }
        return;
    }
    
    if (m_gridDirty.isDirty) {
        pushCanvasRect(m_gridDirty.x, m_gridDirty.y, m_gridDirty.x, m_gridDirty.y, m_gridDirty.w, m_gridDirty.h);
        m_gridDirty.clear();
    }
    
    if (m_topDirty.isDirty) {
        pushCanvasRect(m_topDirty.x, m_topDirty.y, m_topDirty.x, m_topDirty.y, m_topDirty.w, m_topDirty.h);
        m_topDirty.clear();
    }
    
    if (m_bottomDirty.isDirty) {
        pushCanvasRect(m_bottomDirty.x, m_bottomDirty.y, m_bottomDirty.x, m_bottomDirty.y, m_bottomDirty.w, m_bottomDirty.h);
        m_bottomDirty.clear();
    }
}

void ComposerUI::markGridCellDirty(int step, int pitch) {
    if (!m_canvasCreated) return;
    int x = stepToGridX(step);
    int y = pitchToGridY(pitch);
    if (x < m_layout.gridX || y < m_layout.gridY) return;
    if (x >= m_layout.gridX + m_layout.gridW || y >= m_layout.gridY + m_layout.gridH) return;
    m_gridDirty.mark(x - 2, y - 2, m_layout.cellW + 4, m_layout.cellH + 4);
}

bool ComposerUI::handleTouch(int x, int y) {
    if (!m_active) return false;
    
    // Handle dialogs first
    if (m_state == ComposerUIState::FILE_DIALOG) {
        return handleFileDialogTouch(x, y);
    } else if (m_state == ComposerUIState::CONFIRM_DIALOG) {
        return handleConfirmDialogTouch(x, y);
    } else if (m_state == ComposerUIState::CONTEXT_MENU) {
        return handleContextMenuTouch(x, y);
    } else if (m_state == ComposerUIState::INSTRUMENT_PICKER) {
        return handleInstrumentPickerTouch(x, y);
    } else if (m_state == ComposerUIState::PITCH_PICKER || 
               m_state == ComposerUIState::DURATION_PICKER ||
               m_state == ComposerUIState::VELOCITY_PICKER ||
               m_state == ComposerUIState::DIVISION_PICKER ||
               m_state == ComposerUIState::TEMPO_INPUT) {
        return handlePickerTouch(x, y);
    }
    
    return handleMainTouch(x, y);
}

bool ComposerUI::handleGesture(const GestureEvent& event) {
    if (!m_active || m_state != ComposerUIState::MAIN) return false;
    
    // Only handle swipes in the grid area
    if (event.startY < m_layout.topBarH || event.startY > m_layout.screenH - m_layout.bottomBarH) {
        return false;
    }
    
    if (event.type == GestureType::SWIPE_LEFT) {
        // Scroll forward
        uint16_t totalSteps = SequencerEngine::getInstance().getComposition().totalSteps();
        m_viewStartStep += m_layout.stepsVisible;
        if (totalSteps > 0 && m_viewStartStep + m_layout.stepsVisible > totalSteps) {
            m_viewStartStep = (totalSteps > m_layout.stepsVisible) ? (totalSteps - m_layout.stepsVisible) : 0;
        }
        m_needsFullRefresh = true;
        return true;
    } else if (event.type == GestureType::SWIPE_RIGHT) {
        // Scroll backward
        if (m_viewStartStep >= (uint16_t)m_layout.stepsVisible) {
            m_viewStartStep -= m_layout.stepsVisible;
        } else {
            m_viewStartStep = 0;
        }
        m_needsFullRefresh = true;
        return true;
    } else if (event.type == GestureType::SWIPE_UP) {
        if (m_viewStartPitch + 12 + m_pitchRange <= 128) {
            m_viewStartPitch += 12;
            m_needsFullRefresh = true;
        }
        return true;
    } else if (event.type == GestureType::SWIPE_DOWN) {
        if (m_viewStartPitch >= 12) {
            m_viewStartPitch -= 12;
        } else {
            m_viewStartPitch = 0;
        }
        m_needsFullRefresh = true;
        return true;
    } else if (event.type == GestureType::LONG_PRESS) {
        return handleLongPress(event.endX, event.endY);
    }
    
    return false;
}

bool ComposerUI::handleLongPress(int x, int y) {
    if (!m_active || m_state != ComposerUIState::MAIN) return false;
    
    if (!isInGrid(x, y)) return false;
    
    int step = gridXToStep(x);
    int pitch = gridYToPitch(y);
    if (step < 0 || pitch < 0 || pitch > 127) return false;
    
    int channel = m_selectedChannel;
    int noteIdx = -1;
    auto& comp = SequencerEngine::getInstance().getComposition();
    if (m_singleChannelMode) {
        const auto& notes = (channel == 0) ? comp.channel1 : comp.channel2;
        noteIdx = findNoteIndexAtPosition(notes, step, pitch, true);
    } else {
        noteIdx = findNoteIndexAtPosition(comp.channel1, step, pitch, true);
        if (noteIdx >= 0) {
            channel = 0;
        } else {
            noteIdx = findNoteIndexAtPosition(comp.channel2, step, pitch, true);
            if (noteIdx >= 0) channel = 1;
        }
    }
    
    if (noteIdx >= 0) {
        m_selectedChannel = channel;
        m_selectedNoteIndex = noteIdx;
        if (m_canvasCreated) {
            drawGrid(&m_canvas);
            m_gridDirty.mark(m_layout.gridX, m_layout.gridY, m_layout.gridW, m_layout.gridH);
            drawBottomBar(&m_canvas);
            m_bottomDirty.mark(0, m_layout.screenH - m_layout.bottomBarH, m_layout.screenW, m_layout.bottomBarH);
        } else {
            m_needsFullRefresh = true;
        }
        showNoteContextMenu(x, y, channel, noteIdx);
    } else {
        showEmptyCellContextMenu(x, y, step, pitch);
    }
    
    m_ignoreTap = true;
    return true;
}

bool ComposerUI::handleDoubleTap(int x, int y) {
    if (!m_active || m_state != ComposerUIState::MAIN) return false;
    if (!isInGrid(x, y)) return false;
    
    int step = gridXToStep(x);
    int pitch = gridYToPitch(y);
    if (step < 0 || pitch < 0 || pitch > 127) return false;
    
    auto& comp = SequencerEngine::getInstance().getCompositionMut();
    int channel = m_selectedChannel;
    int noteIdx = -1;
    
    if (m_singleChannelMode) {
        auto& notes = (channel == 0) ? comp.channel1 : comp.channel2;
        noteIdx = findNoteIndexAtPosition(notes, step, pitch, true);
    } else {
        noteIdx = findNoteIndexAtPosition(comp.channel1, step, pitch, true);
        if (noteIdx >= 0) {
            channel = 0;
        } else {
            noteIdx = findNoteIndexAtPosition(comp.channel2, step, pitch, true);
            if (noteIdx >= 0) channel = 1;
        }
    }
    
    if (noteIdx >= 0) {
        auto& notes = (channel == 0) ? comp.channel1 : comp.channel2;
        m_selectedChannel = channel;
        m_selectedNoteIndex = noteIdx;
        notes[noteIdx].enabled = !notes[noteIdx].enabled;
        if (m_canvasCreated) {
            drawGrid(&m_canvas);
            m_gridDirty.mark(m_layout.gridX, m_layout.gridY, m_layout.gridW, m_layout.gridH);
            drawBottomBar(&m_canvas);
            m_bottomDirty.mark(0, m_layout.screenH - m_layout.bottomBarH, m_layout.screenW, m_layout.bottomBarH);
        } else {
            m_needsFullRefresh = true;
        }
    } else {
        SequencerEngine::getInstance().setPosition(step);
        m_playheadDirty.isDirty = true;
    }
    
    m_ignoreTap = true;
    return true;
}

bool ComposerUI::handleTopBarTouch(int x, int y) {
    int btnY = 5;
    int btnH = m_layout.topBarH - 10;
    int bx = 10;
    
    if (x >= bx && x < bx + 70) {
        m_shouldExit = true;
        return true;
    }
    bx += 80;
    
    if (x >= bx && x < bx + 70) {
        SequencerEngine::getInstance().togglePlayback();
        if (m_canvasCreated) {
            drawTopBar(&m_canvas);
            m_topDirty.mark(0, 0, m_layout.screenW, m_layout.topBarH);
        } else {
            m_needsFullRefresh = true;
        }
        return true;
    }
    bx += 80;
    
    if (x >= bx && x < bx + 120) {
        m_state = ComposerUIState::TEMPO_INPUT;
        m_needsFullRefresh = true;
        return true;
    }
    bx += 130;
    
    if (x >= bx && x < bx + 90) {
        m_state = ComposerUIState::DIVISION_PICKER;
        m_needsFullRefresh = true;
        return true;
    }
    bx += 100;
    
    if (x >= bx && x < bx + 90) {
        m_singleChannelMode = !m_singleChannelMode;
        if (m_canvasCreated) {
            drawTopBar(&m_canvas);
            drawGrid(&m_canvas);
            m_topDirty.mark(0, 0, m_layout.screenW, m_layout.topBarH);
            m_gridDirty.mark(m_layout.gridX, m_layout.gridY, m_layout.gridW, m_layout.gridH);
        } else {
            m_needsFullRefresh = true;
        }
        return true;
    }
    
    int refreshW = 90;
    int refreshX = m_layout.screenW - refreshW - 10;
    int channelW = 150;
    int channelX = refreshX - channelW - 10;
    
    if (x >= channelX && x < channelX + channelW / 2 && y >= btnY && y < btnY + btnH) {
        m_selectedChannel = 0;
        m_selectedNoteIndex = -1;
        if (m_canvasCreated) {
            drawTopBar(&m_canvas);
            drawGrid(&m_canvas);
            drawBottomBar(&m_canvas);
            m_topDirty.mark(0, 0, m_layout.screenW, m_layout.topBarH);
            m_gridDirty.mark(m_layout.gridX, m_layout.gridY, m_layout.gridW, m_layout.gridH);
            m_bottomDirty.mark(0, m_layout.screenH - m_layout.bottomBarH, m_layout.screenW, m_layout.bottomBarH);
        } else {
            m_needsFullRefresh = true;
        }
        return true;
    }
    
    if (x >= channelX + channelW / 2 && x < channelX + channelW && y >= btnY && y < btnY + btnH) {
        m_selectedChannel = 1;
        m_selectedNoteIndex = -1;
        if (m_canvasCreated) {
            drawTopBar(&m_canvas);
            drawGrid(&m_canvas);
            drawBottomBar(&m_canvas);
            m_topDirty.mark(0, 0, m_layout.screenW, m_layout.topBarH);
            m_gridDirty.mark(m_layout.gridX, m_layout.gridY, m_layout.gridW, m_layout.gridH);
            m_bottomDirty.mark(0, m_layout.screenH - m_layout.bottomBarH, m_layout.screenW, m_layout.bottomBarH);
        } else {
            m_needsFullRefresh = true;
        }
        return true;
    }
    
    if (x >= refreshX && x < refreshX + refreshW && y >= btnY && y < btnY + btnH) {
        m_needsFullRefresh = true;
        return true;
    }
    
    return false;
}

bool ComposerUI::handleGridTouch(int x, int y) {
    if (!isInGrid(x, y)) return false;
    
    auto& comp = SequencerEngine::getInstance().getCompositionMut();
    int step = gridXToStep(x);
    int pitch = gridYToPitch(y);
    uint16_t totalSteps = comp.totalSteps();
    
    if (step < 0 || pitch < 0 || pitch > 127) return false;
    if (totalSteps > 0 && step >= (int)totalSteps) return false;
    
    int channel = m_selectedChannel;
    int noteIdx = -1;
    
    if (m_singleChannelMode) {
        const auto& notes = (channel == 0) ? comp.channel1 : comp.channel2;
        noteIdx = findNoteIndexAtPosition(notes, step, pitch, true);
    } else {
        noteIdx = findNoteIndexAtPosition(comp.channel1, step, pitch, true);
        if (noteIdx >= 0) {
            channel = 0;
        } else {
            noteIdx = findNoteIndexAtPosition(comp.channel2, step, pitch, true);
            if (noteIdx >= 0) channel = 1;
        }
    }
    
    if (noteIdx >= 0) {
        m_selectedChannel = channel;
        m_selectedNoteIndex = noteIdx;
        auto& notes = (channel == 0) ? comp.channel1 : comp.channel2;
        m_defaultPitch = notes[noteIdx].pitch;
        m_defaultDuration = notes[noteIdx].duration;
        m_defaultVelocity = notes[noteIdx].velocity;
        
        if (m_canvasCreated) {
            drawGrid(&m_canvas);
            m_gridDirty.mark(m_layout.gridX, m_layout.gridY, m_layout.gridW, m_layout.gridH);
            drawBottomBar(&m_canvas);
            m_bottomDirty.mark(0, m_layout.screenH - m_layout.bottomBarH, m_layout.screenW, m_layout.bottomBarH);
        } else {
            m_needsFullRefresh = true;
        }
    } else {
        NoteEvent newNote(step, m_defaultDuration, static_cast<uint8_t>(pitch), m_defaultVelocity);
        int idx = SequencerEngine::getInstance().addNote(channel, newNote);
        if (idx < 0) {
            return false;
        }
        m_selectedChannel = channel;
        m_selectedNoteIndex = idx;
        m_defaultPitch = static_cast<uint8_t>(pitch);
        
        if (m_canvasCreated) {
            drawGrid(&m_canvas);
            m_gridDirty.mark(m_layout.gridX, m_layout.gridY, m_layout.gridW, m_layout.gridH);
            drawBottomBar(&m_canvas);
            m_bottomDirty.mark(0, m_layout.screenH - m_layout.bottomBarH, m_layout.screenW, m_layout.bottomBarH);
        } else {
            m_needsFullRefresh = true;
        }
    }
    
    return true;
}

bool ComposerUI::handleBottomBarTouch(int x, int y) {
    bool noteSelected = (m_selectedNoteIndex != -1);
    
    int btnW = 85;
    int gap = 6;
    int x0 = 10;
    int btnH = m_layout.bottomBarH - 20;
    int btnY = m_layout.screenH - m_layout.bottomBarH + 10;
    
    auto hit = [&](int index) {
        int bx = x0 + index * (btnW + gap);
        return x >= bx && x < bx + btnW && y >= btnY && y < btnY + btnH;
    };
    
    if (hit(0) && noteSelected) {
        m_state = ComposerUIState::PITCH_PICKER;
        m_needsFullRefresh = true;
        return true;
    }
    
    if (hit(1) && noteSelected) {
        m_state = ComposerUIState::DURATION_PICKER;
        m_needsFullRefresh = true;
        return true;
    }
    
    if (hit(2) && noteSelected) {
        m_state = ComposerUIState::VELOCITY_PICKER;
        m_needsFullRefresh = true;
        return true;
    }
    
    if (hit(3) && noteSelected) {
        SequencerEngine::getInstance().removeNote(m_selectedChannel, m_selectedNoteIndex);
        m_selectedNoteIndex = -1;
        if (m_canvasCreated) {
            drawGrid(&m_canvas);
            m_gridDirty.mark(m_layout.gridX, m_layout.gridY, m_layout.gridW, m_layout.gridH);
            drawBottomBar(&m_canvas);
            m_bottomDirty.mark(0, m_layout.screenH - m_layout.bottomBarH, m_layout.screenW, m_layout.bottomBarH);
        } else {
            m_needsFullRefresh = true;
        }
        return true;
    }
    
    if (hit(4)) {
        SequencerEngine::getInstance().clearAll();
        m_selectedNoteIndex = -1;
        m_needsFullRefresh = true;
        return true;
    }
    
    if (hit(5)) {
        m_fileDialogMode = FileDialogMode::LOAD_PROJECT;
        loadFileList();
        m_state = ComposerUIState::FILE_DIALOG;
        m_needsFullRefresh = true;
        return true;
    }
    
    if (hit(6)) {
        m_fileDialogMode = FileDialogMode::LOAD_MIDI;
        loadFileList();
        m_state = ComposerUIState::FILE_DIALOG;
        m_needsFullRefresh = true;
        return true;
    }
    
    if (hit(7)) {
        m_fileDialogMode = FileDialogMode::SAVE_PROJECT;
        m_inputFilename = "project1.mcs";
        m_state = ComposerUIState::FILE_DIALOG;
        m_needsFullRefresh = true;
        return true;
    }
    
    if (hit(8)) {
        m_fileDialogMode = FileDialogMode::EXPORT_MIDI;
        m_inputFilename = "export1.mid";
        m_state = ComposerUIState::FILE_DIALOG;
        m_needsFullRefresh = true;
        return true;
    }
    
    if (hit(9)) {
        m_instrumentPickerChannel = m_selectedChannel;
        m_state = ComposerUIState::INSTRUMENT_PICKER;
        m_needsFullRefresh = true;
        return true;
    }
    
    return false;
}

bool ComposerUI::handleMainTouch(int x, int y) {
    if (y < m_layout.topBarH) {
        return handleTopBarTouch(x, y);
    }
    if (y > m_layout.screenH - m_layout.bottomBarH) {
        return handleBottomBarTouch(x, y);
    }
    
    if (y >= m_layout.gridY && y < m_layout.gridY + m_layout.gridH && x < m_layout.gridX) {
        int pitch = gridYToPitch(y);
        if (pitch >= 0 && pitch <= 127) {
            m_defaultPitch = static_cast<uint8_t>(pitch);
            if (m_selectedNoteIndex >= 0) {
                SequencerEngine::getInstance().setNotePitch(m_selectedChannel, m_selectedNoteIndex, m_defaultPitch);
            }
            if (m_canvasCreated) {
                drawGrid(&m_canvas);
                m_gridDirty.mark(m_layout.gridX, m_layout.gridY, m_layout.gridW, m_layout.gridH);
            } else {
                m_needsFullRefresh = true;
            }
            return true;
        }
    }
    
    return handleGridTouch(x, y);
}

void ComposerUI::handleDragStart(int x, int y) {
    if (!m_active) return;
    
    m_isDragging = true;
    m_dragStartX = x;
    m_dragStartY = y;
    m_dragNoteIndex = -1;
    m_dragChannel = -1;
    m_dragMode = DragMode::NONE;
    
    if (m_state != ComposerUIState::MAIN) return;
    if (!isInGrid(x, y)) return;
    
    int step = gridXToStep(x);
    int pitch = gridYToPitch(y);
    if (pitch < 0 || pitch > 127) return;
    
    auto& comp = SequencerEngine::getInstance().getCompositionMut();
    int channel = m_selectedChannel;
    int noteIdx = -1;
    
    if (m_singleChannelMode) {
        const auto& notes = (channel == 0) ? comp.channel1 : comp.channel2;
        noteIdx = findNoteIndexAtPosition(notes, step, pitch, true);
    } else {
        noteIdx = findNoteIndexAtPosition(comp.channel1, step, pitch, true);
        if (noteIdx >= 0) {
            channel = 0;
        } else {
            noteIdx = findNoteIndexAtPosition(comp.channel2, step, pitch, true);
            if (noteIdx >= 0) channel = 1;
        }
    }
    
    if (noteIdx >= 0) {
        m_selectedChannel = channel;
        m_selectedNoteIndex = noteIdx;
        m_dragChannel = channel;
        m_dragNoteIndex = noteIdx;
        const auto& notes = (channel == 0) ? comp.channel1 : comp.channel2;
        m_dragOriginalDuration = notes[noteIdx].duration;
        m_dragOriginalPitch = notes[noteIdx].pitch;
        m_defaultPitch = notes[noteIdx].pitch;
        m_defaultDuration = notes[noteIdx].duration;
        m_defaultVelocity = notes[noteIdx].velocity;
        if (m_canvasCreated) {
            drawGrid(&m_canvas);
            m_gridDirty.mark(m_layout.gridX, m_layout.gridY, m_layout.gridW, m_layout.gridH);
            drawBottomBar(&m_canvas);
            m_bottomDirty.mark(0, m_layout.screenH - m_layout.bottomBarH, m_layout.screenW, m_layout.bottomBarH);
        }
    }
}

void ComposerUI::handleDragMove(int x, int y) {
    if (!m_isDragging || m_state != ComposerUIState::MAIN) return;
    if (m_dragStartY < m_layout.gridY || m_dragStartY >= m_layout.gridY + m_layout.gridH) return;
    
    int dx = x - m_dragStartX;
    int dy = y - m_dragStartY;
    const int threshold = 6;
    
    if (m_dragNoteIndex >= 0) {
        if (m_dragMode == DragMode::NONE && std::abs(dx) > threshold) {
            m_dragMode = DragMode::DURATION;
        }
        if (m_dragMode == DragMode::DURATION && m_layout.cellW > 0) {
            int deltaSteps = dx / m_layout.cellW;
            auto& comp = SequencerEngine::getInstance().getCompositionMut();
            auto& notes = (m_dragChannel == 0) ? comp.channel1 : comp.channel2;
            if (m_dragNoteIndex >= 0 && m_dragNoteIndex < (int)notes.size()) {
                uint16_t maxDur = comp.totalSteps() > notes[m_dragNoteIndex].startStep ?
                    (comp.totalSteps() - notes[m_dragNoteIndex].startStep) : notes[m_dragNoteIndex].duration;
                uint16_t newDur = static_cast<uint16_t>(std::max(1, (int)m_dragOriginalDuration + deltaSteps));
                if (maxDur > 0 && newDur > maxDur) newDur = maxDur;
                if (newDur != notes[m_dragNoteIndex].duration) {
                    SequencerEngine::getInstance().setNoteDuration(m_dragChannel, m_dragNoteIndex, newDur);
                    m_defaultDuration = newDur;
                    if (m_canvasCreated) {
                        drawGrid(&m_canvas);
                        m_gridDirty.mark(m_layout.gridX, m_layout.gridY, m_layout.gridW, m_layout.gridH);
                    } else {
                        m_needsFullRefresh = true;
                    }
                }
            }
        }
        return;
    }
    
    if (m_dragMode == DragMode::NONE && (std::abs(dx) > threshold || std::abs(dy) > threshold)) {
        m_dragMode = (std::abs(dy) > std::abs(dx)) ? DragMode::OCTAVE_SCROLL : DragMode::STEP_SCROLL;
    }
    
    if (m_dragMode == DragMode::OCTAVE_SCROLL && m_layout.cellH > 0) {
        int semitoneDelta = (m_dragStartY - y) / m_layout.cellH;
        if (semitoneDelta != 0) {
            int newStart = static_cast<int>(m_viewStartPitch) + semitoneDelta;
            int maxStart = 128 - m_pitchRange;
            if (newStart < 0) newStart = 0;
            if (newStart > maxStart) newStart = maxStart;
            m_viewStartPitch = static_cast<uint8_t>(newStart);
            m_dragStartY += semitoneDelta * m_layout.cellH;
            m_needsFullRefresh = true;
        }
    } else if (m_dragMode == DragMode::STEP_SCROLL && m_layout.cellW > 0) {
        int stepDelta = (m_dragStartX - x) / m_layout.cellW;
        if (stepDelta != 0) {
            uint16_t totalSteps = SequencerEngine::getInstance().getComposition().totalSteps();
            int newStart = static_cast<int>(m_viewStartStep) + stepDelta;
            int maxStart = (totalSteps > m_stepsVisible) ? (totalSteps - m_stepsVisible) : 0;
            if (newStart < 0) newStart = 0;
            if (newStart > maxStart) newStart = maxStart;
            m_viewStartStep = static_cast<uint16_t>(newStart);
            m_dragStartX += stepDelta * m_layout.cellW;
            m_needsFullRefresh = true;
        }
    }
}

void ComposerUI::handleDragEnd(int x, int y) {
    if (!m_isDragging) return;
    bool wasDrag = (m_dragMode != DragMode::NONE);
    
    m_isDragging = false;
    m_dragMode = DragMode::NONE;
    m_dragNoteIndex = -1;
    m_dragChannel = -1;
    
    if (m_ignoreTap) {
        m_ignoreTap = false;
        return;
    }
    
    if (!wasDrag) {
        handleTouch(x, y);
    }
}

// Dialog and Picker implementations (simplified for brevity)

void ComposerUI::drawFileDialog(LovyanGFX* target) {
    LovyanGFX* gfx = target ? target : &m_canvas;
    int w = 600;
    int h = 400;
    int x = (m_layout.screenW - w) / 2;
    int y = (m_layout.screenH - h) / 2;
    
    gfx->fillRect(x, y, w, h, COLOR_WHITE);
    gfx->drawRect(x, y, w, h, COLOR_BLACK);
    gfx->drawRect(x+2, y+2, w-4, h-4, COLOR_BLACK);
    
    gfx->setTextColor(COLOR_BLACK);
    gfx->setTextSize(1.5f);
    gfx->setTextDatum(textdatum_t::top_center);
    
    const char* title = "File Dialog";
    if (m_fileDialogMode == FileDialogMode::LOAD_PROJECT) title = "Load Project";
    else if (m_fileDialogMode == FileDialogMode::SAVE_PROJECT) title = "Save Project";
    else if (m_fileDialogMode == FileDialogMode::LOAD_MIDI) title = "Load MIDI";
    else if (m_fileDialogMode == FileDialogMode::EXPORT_MIDI) title = "Export MIDI";
    
    gfx->drawString(title, x + w/2, y + 10);
    
    // Draw file list
    gfx->setTextDatum(textdatum_t::top_left);
    for (int i = 0; i < 5 && (i + m_fileListScroll) < (int)m_fileList.size(); i++) {
        int idx = i + m_fileListScroll;
        int fy = y + 60 + i * 50;
        if (idx == m_selectedFileIndex) {
            gfx->fillRect(x + 10, fy, w - 20, 45, COLOR_MEDIUM_GRAY);
        }
        gfx->drawRect(x + 10, fy, w - 20, 45, COLOR_BLACK);
        gfx->drawString(m_fileList[idx].c_str(), x + 20, fy + 10);
    }
    
    // Buttons
    gfx->setTextDatum(textdatum_t::middle_center);
    gfx->drawRect(x + 100, y + h - 60, 150, 50, COLOR_BLACK);
    gfx->drawString("Cancel", x + 175, y + h - 35);
    
    gfx->drawRect(x + w - 250, y + h - 60, 150, 50, COLOR_BLACK);
    gfx->drawString("OK", x + w - 175, y + h - 35);
}

bool ComposerUI::handleFileDialogTouch(int x, int y) {
    int dw = 600;
    int dh = 400;
    int dx = (m_layout.screenW - dw) / 2;
    int dy = (m_layout.screenH - dh) / 2;
    
    if (x < dx || x >= dx + dw || y < dy || y >= dy + dh) {
        m_state = ComposerUIState::MAIN;
        m_needsFullRefresh = true;
        return true;
    }
    
    // Cancel
    if (x >= dx + 100 && x < dx + 250 && y >= dy + dh - 60) {
        m_state = ComposerUIState::MAIN;
        m_needsFullRefresh = true;
        return true;
    }
    
    // OK
    if (x >= dx + dw - 250 && x < dx + dw - 100 && y >= dy + dh - 60) {
        if (m_fileDialogMode == FileDialogMode::LOAD_PROJECT && m_selectedFileIndex >= 0) {
            doLoadProject(m_fileList[m_selectedFileIndex]);
        } else if (m_fileDialogMode == FileDialogMode::SAVE_PROJECT) {
            doSaveProject(m_inputFilename);
        } else if (m_fileDialogMode == FileDialogMode::LOAD_MIDI && m_selectedFileIndex >= 0) {
            doLoadMidi(m_fileList[m_selectedFileIndex]);
        } else if (m_fileDialogMode == FileDialogMode::EXPORT_MIDI) {
            doExportMidi(m_inputFilename);
        }
        if (m_state == ComposerUIState::FILE_DIALOG) {
            m_state = ComposerUIState::MAIN;
            m_needsFullRefresh = true;
        }
        return true;
    }
    
    // File selection
    for (int i = 0; i < 5; i++) {
        int fy = dy + 60 + i * 50;
        if (y >= fy && y < fy + 45) {
            int idx = i + m_fileListScroll;
            if (idx < (int)m_fileList.size()) {
                m_selectedFileIndex = idx;
                m_needsFullRefresh = true;
                return true;
            }
        }
    }
    
    return false;
}

void ComposerUI::loadFileList() {
    m_fileList.clear();
    m_selectedFileIndex = -1;
    m_fileListScroll = 0;
    
    if (m_fileDialogMode == FileDialogMode::LOAD_PROJECT || m_fileDialogMode == FileDialogMode::SAVE_PROJECT) {
        ProjectIO::listProjectFiles(ProjectIO::getProjectDirectory(), m_fileList);
    } else {
        MidiIO::listMidiFiles(ProjectIO::getProjectDirectory(), m_fileList);
    }
}

void ComposerUI::doLoadProject(const std::string& filename) {
    std::string path = ProjectIO::getProjectDirectory() + "/" + filename;
    ProjectResult result = ProjectIO::loadProject(path, SequencerEngine::getInstance().getCompositionMut());
    if (result != ProjectResult::OK) {
        m_confirmMessage = std::string("Load failed: ") + projectResultToString(result);
        m_confirmAction = nullptr;
        m_state = ComposerUIState::CONFIRM_DIALOG;
        m_needsFullRefresh = true;
        return;
    }
    computeLayout();
    m_selectedNoteIndex = -1;
    m_needsFullRefresh = true;
}

void ComposerUI::doSaveProject(const std::string& filename) {
    std::string path = ProjectIO::getProjectDirectory() + "/" + filename;
    ProjectResult result = ProjectIO::saveProject(path, SequencerEngine::getInstance().getComposition());
    if (result != ProjectResult::OK) {
        m_confirmMessage = std::string("Save failed: ") + projectResultToString(result);
        m_confirmAction = nullptr;
        m_state = ComposerUIState::CONFIRM_DIALOG;
        m_needsFullRefresh = true;
    }
}

void ComposerUI::doLoadMidi(const std::string& filename) {
    std::string path = ProjectIO::getProjectDirectory() + "/" + filename;
    MidiResult result = MidiIO::importFile(path, SequencerEngine::getInstance().getCompositionMut(),
                                           SequencerEngine::getInstance().getDivision());
    if (result != MidiResult::OK) {
        m_confirmMessage = std::string("MIDI load failed: ") + midiResultToString(result);
        m_confirmAction = nullptr;
        m_state = ComposerUIState::CONFIRM_DIALOG;
        m_needsFullRefresh = true;
        return;
    }
    computeLayout();
    m_selectedNoteIndex = -1;
    m_needsFullRefresh = true;
}

void ComposerUI::doExportMidi(const std::string& filename) {
    std::string path = ProjectIO::getProjectDirectory() + "/" + filename;
    MidiResult result = MidiIO::exportFile(path, SequencerEngine::getInstance().getComposition());
    if (result != MidiResult::OK) {
        m_confirmMessage = std::string("Export failed: ") + midiResultToString(result);
        m_confirmAction = nullptr;
        m_state = ComposerUIState::CONFIRM_DIALOG;
        m_needsFullRefresh = true;
    }
}

void ComposerUI::showContextMenu(int x, int y, const std::vector<ContextMenuItem>& items) {
    m_contextMenuItems = items;
    m_contextMenuX = x;
    m_contextMenuY = y;
    
    int menuW = 240;
    int menuH = static_cast<int>(items.size()) * 40 + 10;
    
    if (m_contextMenuX + menuW > m_layout.screenW) {
        m_contextMenuX = m_layout.screenW - menuW - 5;
    }
    if (m_contextMenuY + menuH > m_layout.screenH) {
        m_contextMenuY = m_layout.screenH - menuH - 5;
    }
    if (m_contextMenuX < 5) m_contextMenuX = 5;
    if (m_contextMenuY < 5) m_contextMenuY = 5;
    
    m_state = ComposerUIState::CONTEXT_MENU;
    m_needsFullRefresh = true;
}

void ComposerUI::showNoteContextMenu(int x, int y, int channel, int noteIndex) {
    auto& comp = SequencerEngine::getInstance().getCompositionMut();
    auto& notes = (channel == 0) ? comp.channel1 : comp.channel2;
    if (noteIndex < 0 || noteIndex >= (int)notes.size()) return;
    
    bool enabled = notes[noteIndex].enabled;
    
    std::vector<ContextMenuItem> items;
    items.push_back({"Pitch...", [this, channel, noteIndex]() {
        m_selectedChannel = channel;
        m_selectedNoteIndex = noteIndex;
        m_state = ComposerUIState::PITCH_PICKER;
        m_needsFullRefresh = true;
    }});
    items.push_back({"Length...", [this, channel, noteIndex]() {
        m_selectedChannel = channel;
        m_selectedNoteIndex = noteIndex;
        m_state = ComposerUIState::DURATION_PICKER;
        m_needsFullRefresh = true;
    }});
    items.push_back({"Velocity...", [this, channel, noteIndex]() {
        m_selectedChannel = channel;
        m_selectedNoteIndex = noteIndex;
        m_state = ComposerUIState::VELOCITY_PICKER;
        m_needsFullRefresh = true;
    }});
    items.push_back({enabled ? "Mute" : "Unmute", [this, channel, noteIndex]() {
        auto& compMut = SequencerEngine::getInstance().getCompositionMut();
        auto& notesMut = (channel == 0) ? compMut.channel1 : compMut.channel2;
        if (noteIndex >= 0 && noteIndex < (int)notesMut.size()) {
            notesMut[noteIndex].enabled = !notesMut[noteIndex].enabled;
        }
        if (m_canvasCreated) {
            drawGrid(&m_canvas);
            m_gridDirty.mark(m_layout.gridX, m_layout.gridY, m_layout.gridW, m_layout.gridH);
        } else {
            m_needsFullRefresh = true;
        }
    }});
    items.push_back({"Duplicate", [this, channel, noteIndex]() {
        auto& compMut = SequencerEngine::getInstance().getCompositionMut();
        auto& notesMut = (channel == 0) ? compMut.channel1 : compMut.channel2;
        if (noteIndex >= 0 && noteIndex < (int)notesMut.size()) {
            NoteEvent copy = notesMut[noteIndex];
            uint16_t totalSteps = compMut.totalSteps();
            uint16_t newStart = copy.startStep + copy.duration;
            if (totalSteps == 0 || newStart < totalSteps) {
                copy.startStep = newStart;
                SequencerEngine::getInstance().addNote(channel, copy);
            }
        }
        if (m_canvasCreated) {
            drawGrid(&m_canvas);
            m_gridDirty.mark(m_layout.gridX, m_layout.gridY, m_layout.gridW, m_layout.gridH);
        } else {
            m_needsFullRefresh = true;
        }
    }});
    items.push_back({"Delete", [this, channel, noteIndex]() {
        SequencerEngine::getInstance().removeNote(channel, noteIndex);
        m_selectedNoteIndex = -1;
        if (m_canvasCreated) {
            drawGrid(&m_canvas);
            m_gridDirty.mark(m_layout.gridX, m_layout.gridY, m_layout.gridW, m_layout.gridH);
            drawBottomBar(&m_canvas);
            m_bottomDirty.mark(0, m_layout.screenH - m_layout.bottomBarH, m_layout.screenW, m_layout.bottomBarH);
        } else {
            m_needsFullRefresh = true;
        }
    }});
    
    showContextMenu(x, y, items);
}

void ComposerUI::showEmptyCellContextMenu(int x, int y, int step, int pitch) {
    std::vector<ContextMenuItem> items;
    items.push_back({"Add Note", [this, step, pitch]() {
        NoteEvent newNote(step, m_defaultDuration, static_cast<uint8_t>(pitch), m_defaultVelocity);
        int idx = SequencerEngine::getInstance().addNote(m_selectedChannel, newNote);
        if (idx < 0) {
            return;
        }
        m_selectedNoteIndex = idx;
        m_defaultPitch = static_cast<uint8_t>(pitch);
        if (m_canvasCreated) {
            drawGrid(&m_canvas);
            m_gridDirty.mark(m_layout.gridX, m_layout.gridY, m_layout.gridW, m_layout.gridH);
            drawBottomBar(&m_canvas);
            m_bottomDirty.mark(0, m_layout.screenH - m_layout.bottomBarH, m_layout.screenW, m_layout.bottomBarH);
        } else {
            m_needsFullRefresh = true;
        }
    }});
    items.push_back({"Default Pitch", [this, pitch]() {
        m_defaultPitch = static_cast<uint8_t>(pitch);
        if (m_canvasCreated) {
            drawGrid(&m_canvas);
            m_gridDirty.mark(m_layout.gridX, m_layout.gridY, m_layout.gridW, m_layout.gridH);
        } else {
            m_needsFullRefresh = true;
        }
    }});
    items.push_back({"Default Dur", [this]() {
        m_state = ComposerUIState::DURATION_PICKER;
        m_needsFullRefresh = true;
    }});
    items.push_back({"Default Vel", [this]() {
        m_state = ComposerUIState::VELOCITY_PICKER;
        m_needsFullRefresh = true;
    }});
    
    showContextMenu(x, y, items);
}

void ComposerUI::drawContextMenu(LovyanGFX* target) {
    LovyanGFX* gfx = target ? target : &m_canvas;
    int menuW = 240;
    int itemH = 40;
    int menuH = static_cast<int>(m_contextMenuItems.size()) * itemH + 10;
    
    gfx->fillRect(m_contextMenuX, m_contextMenuY, menuW, menuH, COLOR_WHITE);
    gfx->drawRect(m_contextMenuX, m_contextMenuY, menuW, menuH, COLOR_BLACK);
    
    gfx->setTextSize(1.2f);
    gfx->setTextDatum(textdatum_t::middle_left);
    for (size_t i = 0; i < m_contextMenuItems.size(); i++) {
        int iy = m_contextMenuY + 5 + (int)i * itemH;
        gfx->drawRect(m_contextMenuX + 5, iy, menuW - 10, itemH - 5, COLOR_BLACK);
        gfx->setTextColor(COLOR_BLACK);
        gfx->drawString(m_contextMenuItems[i].label.c_str(), m_contextMenuX + 12, iy + (itemH - 5) / 2);
    }
}

bool ComposerUI::handleContextMenuTouch(int x, int y) {
    int menuW = 240;
    int itemH = 40;
    int menuH = static_cast<int>(m_contextMenuItems.size()) * itemH + 10;
    
    if (x < m_contextMenuX || x >= m_contextMenuX + menuW ||
        y < m_contextMenuY || y >= m_contextMenuY + menuH) {
        m_state = ComposerUIState::MAIN;
        m_needsFullRefresh = true;
        return true;
    }
    
    int index = (y - (m_contextMenuY + 5)) / itemH;
    if (index >= 0 && index < (int)m_contextMenuItems.size()) {
        auto action = m_contextMenuItems[index].action;
        if (action) {
            action();
        }
        if (m_state == ComposerUIState::CONTEXT_MENU) {
            m_state = ComposerUIState::MAIN;
            m_needsFullRefresh = true;
        }
        return true;
    }
    
    return false;
}

void ComposerUI::drawInstrumentPicker(LovyanGFX* target) {
    LovyanGFX* gfx = target ? target : &m_canvas;
    int w = 520;
    int h = 430;
    int x = (m_layout.screenW - w) / 2;
    int y = (m_layout.screenH - h) / 2;
    
    gfx->fillRect(x, y, w, h, COLOR_WHITE);
    gfx->drawRect(x, y, w, h, COLOR_BLACK);
    gfx->drawRect(x + 2, y + 2, w - 4, h - 4, COLOR_BLACK);
    
    gfx->setTextColor(COLOR_BLACK);
    gfx->setTextSize(1.4f);
    gfx->setTextDatum(textdatum_t::top_center);
    gfx->drawString(m_instrumentPickerChannel == 0 ? "Instrument Ch1" : "Instrument Ch2", x + w / 2, y + 8);
    
    int cols = 2;
    int rows = 13;
    int cellW = (w - 40) / cols;
    int cellH = 24;
    int startX = x + 20;
    int startY = y + 45;
    
    const auto& comp = SequencerEngine::getInstance().getComposition();
    uint8_t current = (m_instrumentPickerChannel == 0) ? comp.channel1Instrument : comp.channel2Instrument;
    
    gfx->setTextSize(1.0f);
    gfx->setTextDatum(textdatum_t::middle_left);
    for (int i = 0; i <= 25; i++) {
        int col = i / rows;
        int row = i % rows;
        int bx = startX + col * cellW;
        int by = startY + row * cellH;
        bool selected = (i == current);
        uint16_t fill = selected ? COLOR_MEDIUM_GRAY : COLOR_WHITE;
        uint16_t text = selected ? COLOR_WHITE : COLOR_BLACK;
        
        gfx->fillRect(bx, by, cellW - 4, cellH - 2, fill);
        gfx->drawRect(bx, by, cellW - 4, cellH - 2, COLOR_BLACK);
        
        char label[32];
        snprintf(label, sizeof(label), "%2d %s", i, getInstrumentName(i));
        gfx->setTextColor(text);
        gfx->drawString(label, bx + 6, by + (cellH - 2) / 2);
    }
    
    gfx->setTextSize(1.2f);
    drawButton(gfx, x + (w - 140) / 2, y + h - 55, 140, 40, "Close");
}

bool ComposerUI::handleInstrumentPickerTouch(int x, int y) {
    int w = 520;
    int h = 430;
    int dx = (m_layout.screenW - w) / 2;
    int dy = (m_layout.screenH - h) / 2;
    
    if (x < dx || x >= dx + w || y < dy || y >= dy + h) {
        m_confirmAction = nullptr;
        m_state = ComposerUIState::MAIN;
        m_needsFullRefresh = true;
        return true;
    }
    
    if (x >= dx + (w - 140) / 2 && x < dx + (w + 140) / 2 && y >= dy + h - 55 && y < dy + h - 15) {
        m_state = ComposerUIState::MAIN;
        m_needsFullRefresh = true;
        return true;
    }
    
    int cols = 2;
    int rows = 13;
    int cellW = (w - 40) / cols;
    int cellH = 24;
    int startX = dx + 20;
    int startY = dy + 45;
    
    if (y >= startY && y < startY + rows * cellH) {
        int col = (x - startX) / cellW;
        int row = (y - startY) / cellH;
        if (col >= 0 && col < cols && row >= 0 && row < rows) {
            int index = col * rows + row;
            if (index >= 0 && index <= 25) {
                auto& comp = SequencerEngine::getInstance().getCompositionMut();
                if (m_instrumentPickerChannel == 0) {
                    comp.channel1Instrument = static_cast<uint8_t>(index);
                } else {
                    comp.channel2Instrument = static_cast<uint8_t>(index);
                }
                m_state = ComposerUIState::MAIN;
                m_needsFullRefresh = true;
                return true;
            }
        }
    }
    
    return false;
}

// ==================== Pitch Picker ====================
void ComposerUI::drawPitchPicker(LovyanGFX* target) {
    LovyanGFX* gfx = target ? target : &m_canvas;
    int w = 500;
    int h = 340;
    int x = (m_layout.screenW - w) / 2;
    int y = (m_layout.screenH - h) / 2;
    
    gfx->fillRect(x, y, w, h, COLOR_WHITE);
    gfx->drawRect(x, y, w, h, COLOR_BLACK);
    gfx->drawRect(x + 2, y + 2, w - 4, h - 4, COLOR_BLACK);
    
    gfx->setTextColor(COLOR_BLACK);
    gfx->setTextSize(1.5f);
    gfx->setTextDatum(textdatum_t::top_center);
    gfx->drawString("Select Pitch", x + w / 2, y + 10);
    
    // Get current pitch
    uint8_t currentPitch = m_defaultPitch;
    if (m_selectedNoteIndex >= 0) {
        auto& ch = (m_selectedChannel == 0) ? 
            SequencerEngine::getInstance().getComposition().channel1 :
            SequencerEngine::getInstance().getComposition().channel2;
        if (m_selectedNoteIndex < (int)ch.size()) {
            currentPitch = ch[m_selectedNoteIndex].pitch;
        }
    }
    
    // Draw piano keyboard layout - 2 octaves (C3-B4 = notes 48-71)
    const char* noteNames[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    const bool isBlack[] = {false, true, false, true, false, false, true, false, true, false, true, false};
    
    int keyW = 38;
    int keyH = 100;
    int startX = x + 20;
    int startY = y + 50;
    int octave = (currentPitch / 12);
    
    // Draw octave selector
    gfx->setTextSize(1.2f);
    gfx->setTextDatum(textdatum_t::middle_center);
    gfx->drawRect(x + 20, y + 270, 60, 40, COLOR_BLACK);
    gfx->drawString("-", x + 50, y + 290);
    
    char octStr[16];
    snprintf(octStr, sizeof(octStr), "Oct %d", octave);
    gfx->drawString(octStr, x + w / 2, y + 290);
    
    gfx->drawRect(x + w - 80, y + 270, 60, 40, COLOR_BLACK);
    gfx->drawString("+", x + w - 50, y + 290);
    
    // Draw white keys first
    int whiteKeyIdx = 0;
    for (int i = 0; i < 12; i++) {
        if (!isBlack[i]) {
            int kx = startX + whiteKeyIdx * keyW;
            uint8_t notePitch = octave * 12 + i;
            bool selected = (notePitch == currentPitch);
            
            gfx->fillRect(kx, startY, keyW - 2, keyH, selected ? COLOR_MEDIUM_GRAY : COLOR_WHITE);
            gfx->drawRect(kx, startY, keyW - 2, keyH, COLOR_BLACK);
            
            gfx->setTextColor(selected ? COLOR_WHITE : COLOR_BLACK);
            gfx->setTextSize(1.0f);
            gfx->drawString(noteNames[i], kx + keyW / 2 - 1, startY + keyH - 15);
            whiteKeyIdx++;
        }
    }
    
    // Draw black keys on top
    whiteKeyIdx = 0;
    gfx->setTextColor(COLOR_WHITE);
    for (int i = 0; i < 12; i++) {
        if (!isBlack[i]) {
            whiteKeyIdx++;
        } else {
            int kx = startX + (whiteKeyIdx - 1) * keyW + keyW * 2 / 3;
            uint8_t notePitch = octave * 12 + i;
            bool selected = (notePitch == currentPitch);
            
            gfx->fillRect(kx, startY, keyW * 2 / 3, keyH * 2 / 3, selected ? COLOR_MEDIUM_GRAY : COLOR_BLACK);
            if (selected) {
                gfx->drawRect(kx, startY, keyW * 2 / 3, keyH * 2 / 3, COLOR_BLACK);
            }
        }
    }
    
    // Cancel/OK buttons
    gfx->setTextSize(1.2f);
    gfx->drawRect(x + 100, y + h - 50, 120, 40, COLOR_BLACK);
    gfx->drawString("Cancel", x + 160, y + h - 30);
    
    gfx->drawRect(x + w - 220, y + h - 50, 120, 40, COLOR_BLACK);
    gfx->drawString("OK", x + w - 160, y + h - 30);
}

// ==================== Duration Picker ====================
void ComposerUI::drawDurationPicker(LovyanGFX* target) {
    LovyanGFX* gfx = target ? target : &m_canvas;
    int w = 400;
    int h = 280;
    int x = (m_layout.screenW - w) / 2;
    int y = (m_layout.screenH - h) / 2;
    
    gfx->fillRect(x, y, w, h, COLOR_WHITE);
    gfx->drawRect(x, y, w, h, COLOR_BLACK);
    gfx->drawRect(x + 2, y + 2, w - 4, h - 4, COLOR_BLACK);
    
    gfx->setTextColor(COLOR_BLACK);
    gfx->setTextSize(1.5f);
    gfx->setTextDatum(textdatum_t::top_center);
    gfx->drawString("Select Duration", x + w / 2, y + 10);
    
    // Get current duration
    uint16_t currentDur = m_defaultDuration;
    if (m_selectedNoteIndex >= 0) {
        auto& ch = (m_selectedChannel == 0) ? 
            SequencerEngine::getInstance().getComposition().channel1 :
            SequencerEngine::getInstance().getComposition().channel2;
        if (m_selectedNoteIndex < (int)ch.size()) {
            currentDur = ch[m_selectedNoteIndex].duration;
        }
    }
    
    // Duration options (in steps)
    const int durations[] = {1, 2, 4, 8, 16};
    const char* durNames[] = {"1/16", "1/8", "1/4", "1/2", "1"};
    
    int btnW = 70;
    int btnH = 50;
    int startX = x + 20;
    int startY = y + 50;
    
    gfx->setTextSize(1.2f);
    gfx->setTextDatum(textdatum_t::middle_center);
    
    for (int i = 0; i < 5; i++) {
        int bx = startX + i * (btnW + 5);
        bool selected = (durations[i] == currentDur);
        
        gfx->fillRect(bx, startY, btnW, btnH, selected ? COLOR_MEDIUM_GRAY : COLOR_WHITE);
        gfx->drawRect(bx, startY, btnW, btnH, COLOR_BLACK);
        gfx->setTextColor(selected ? COLOR_WHITE : COLOR_BLACK);
        gfx->drawString(durNames[i], bx + btnW / 2, startY + btnH / 2);
    }
    
    // Custom duration with +/- buttons
    gfx->setTextColor(COLOR_BLACK);
    gfx->drawString("Custom:", x + 60, startY + 80);
    
    gfx->drawRect(x + 120, startY + 60, 50, 40, COLOR_BLACK);
    gfx->drawString("-", x + 145, startY + 80);
    
    char durStr[16];
    snprintf(durStr, sizeof(durStr), "%d", currentDur);
    gfx->drawString(durStr, x + w / 2, startY + 80);
    
    gfx->drawRect(x + w - 170, startY + 60, 50, 40, COLOR_BLACK);
    gfx->drawString("+", x + w - 145, startY + 80);
    
    // Cancel/OK buttons
    gfx->drawRect(x + 60, y + h - 50, 120, 40, COLOR_BLACK);
    gfx->drawString("Cancel", x + 120, y + h - 30);
    
    gfx->drawRect(x + w - 180, y + h - 50, 120, 40, COLOR_BLACK);
    gfx->drawString("OK", x + w - 120, y + h - 30);
}

// ==================== Velocity Picker ====================
void ComposerUI::drawVelocityPicker(LovyanGFX* target) {
    LovyanGFX* gfx = target ? target : &m_canvas;
    int w = 400;
    int h = 250;
    int x = (m_layout.screenW - w) / 2;
    int y = (m_layout.screenH - h) / 2;
    
    gfx->fillRect(x, y, w, h, COLOR_WHITE);
    gfx->drawRect(x, y, w, h, COLOR_BLACK);
    gfx->drawRect(x + 2, y + 2, w - 4, h - 4, COLOR_BLACK);
    
    gfx->setTextColor(COLOR_BLACK);
    gfx->setTextSize(1.5f);
    gfx->setTextDatum(textdatum_t::top_center);
    gfx->drawString("Select Velocity", x + w / 2, y + 10);
    
    // Get current velocity
    uint8_t currentVel = m_defaultVelocity;
    if (m_selectedNoteIndex >= 0) {
        auto& ch = (m_selectedChannel == 0) ? 
            SequencerEngine::getInstance().getComposition().channel1 :
            SequencerEngine::getInstance().getComposition().channel2;
        if (m_selectedNoteIndex < (int)ch.size()) {
            currentVel = ch[m_selectedNoteIndex].velocity;
        }
    }
    
    // Velocity presets
    const int velocities[] = {32, 64, 80, 100, 127};
    const char* velNames[] = {"pp", "p", "mp", "mf", "ff"};
    
    int btnW = 70;
    int btnH = 50;
    int startX = x + 20;
    int startY = y + 50;
    
    gfx->setTextSize(1.2f);
    gfx->setTextDatum(textdatum_t::middle_center);
    
    for (int i = 0; i < 5; i++) {
        int bx = startX + i * (btnW + 5);
        bool selected = (velocities[i] == currentVel);
        
        gfx->fillRect(bx, startY, btnW, btnH, selected ? COLOR_MEDIUM_GRAY : COLOR_WHITE);
        gfx->drawRect(bx, startY, btnW, btnH, COLOR_BLACK);
        gfx->setTextColor(selected ? COLOR_WHITE : COLOR_BLACK);
        gfx->drawString(velNames[i], bx + btnW / 2, startY + btnH / 2);
    }
    
    // Current value display with +/- buttons
    gfx->setTextColor(COLOR_BLACK);
    gfx->drawString("Value:", x + 60, startY + 80);
    
    gfx->drawRect(x + 120, startY + 60, 50, 40, COLOR_BLACK);
    gfx->drawString("-", x + 145, startY + 80);
    
    char velStr[16];
    snprintf(velStr, sizeof(velStr), "%d", currentVel);
    gfx->drawString(velStr, x + w / 2, startY + 80);
    
    gfx->drawRect(x + w - 170, startY + 60, 50, 40, COLOR_BLACK);
    gfx->drawString("+", x + w - 145, startY + 80);
    
    // Cancel/OK buttons
    gfx->drawRect(x + 60, y + h - 50, 120, 40, COLOR_BLACK);
    gfx->drawString("Cancel", x + 120, y + h - 30);
    
    gfx->drawRect(x + w - 180, y + h - 50, 120, 40, COLOR_BLACK);
    gfx->drawString("OK", x + w - 120, y + h - 30);
}

// ==================== Division Picker ====================
void ComposerUI::drawDivisionPicker(LovyanGFX* target) {
    LovyanGFX* gfx = target ? target : &m_canvas;
    int w = 400;
    int h = 220;
    int x = (m_layout.screenW - w) / 2;
    int y = (m_layout.screenH - h) / 2;
    
    gfx->fillRect(x, y, w, h, COLOR_WHITE);
    gfx->drawRect(x, y, w, h, COLOR_BLACK);
    gfx->drawRect(x + 2, y + 2, w - 4, h - 4, COLOR_BLACK);
    
    gfx->setTextColor(COLOR_BLACK);
    gfx->setTextSize(1.5f);
    gfx->setTextDatum(textdatum_t::top_center);
    gfx->drawString("Select Division", x + w / 2, y + 10);
    
    StepDivision currentDiv = SequencerEngine::getInstance().getDivision();
    
    // Division options
    const StepDivision divisions[] = {
        StepDivision::QUARTER, StepDivision::EIGHTH, 
        StepDivision::SIXTEENTH, StepDivision::THIRTY_SECOND
    };
    const char* divNames[] = {"1/4", "1/8", "1/16", "1/32"};
    
    int btnW = 80;
    int btnH = 50;
    int startX = x + 30;
    int startY = y + 60;
    
    gfx->setTextSize(1.3f);
    gfx->setTextDatum(textdatum_t::middle_center);
    
    for (int i = 0; i < 4; i++) {
        int bx = startX + i * (btnW + 10);
        bool selected = (divisions[i] == currentDiv);
        
        gfx->fillRect(bx, startY, btnW, btnH, selected ? COLOR_MEDIUM_GRAY : COLOR_WHITE);
        gfx->drawRect(bx, startY, btnW, btnH, COLOR_BLACK);
        gfx->setTextColor(selected ? COLOR_WHITE : COLOR_BLACK);
        gfx->drawString(divNames[i], bx + btnW / 2, startY + btnH / 2);
    }
    
    // Cancel/OK buttons
    gfx->setTextColor(COLOR_BLACK);
    gfx->setTextSize(1.2f);
    gfx->drawRect(x + 60, y + h - 50, 120, 40, COLOR_BLACK);
    gfx->drawString("Cancel", x + 120, y + h - 30);
    
    gfx->drawRect(x + w - 180, y + h - 50, 120, 40, COLOR_BLACK);
    gfx->drawString("OK", x + w - 120, y + h - 30);
}

// ==================== Tempo Input ====================
void ComposerUI::drawTempoInput(LovyanGFX* target) {
    LovyanGFX* gfx = target ? target : &m_canvas;
    int w = 400;
    int h = 200;
    int x = (m_layout.screenW - w) / 2;
    int y = (m_layout.screenH - h) / 2;
    
    gfx->fillRect(x, y, w, h, COLOR_WHITE);
    gfx->drawRect(x, y, w, h, COLOR_BLACK);
    gfx->drawRect(x + 2, y + 2, w - 4, h - 4, COLOR_BLACK);
    
    gfx->setTextColor(COLOR_BLACK);
    gfx->setTextSize(1.5f);
    gfx->setTextDatum(textdatum_t::top_center);
    gfx->drawString("Set Tempo (BPM)", x + w / 2, y + 10);
    
    uint16_t currentTempo = SequencerEngine::getInstance().getTempo();
    
    gfx->setTextSize(1.3f);
    gfx->setTextDatum(textdatum_t::middle_center);
    
    // Large -/+ buttons
    gfx->drawRect(x + 30, y + 60, 60, 50, COLOR_BLACK);
    gfx->drawString("-10", x + 60, y + 85);
    
    gfx->drawRect(x + 100, y + 60, 50, 50, COLOR_BLACK);
    gfx->drawString("-1", x + 125, y + 85);
    
    // Current tempo display
    gfx->setTextSize(2.0f);
    char tempoStr[16];
    snprintf(tempoStr, sizeof(tempoStr), "%d", currentTempo);
    gfx->drawString(tempoStr, x + w / 2, y + 85);
    
    gfx->setTextSize(1.3f);
    gfx->drawRect(x + w - 150, y + 60, 50, 50, COLOR_BLACK);
    gfx->drawString("+1", x + w - 125, y + 85);
    
    gfx->drawRect(x + w - 90, y + 60, 60, 50, COLOR_BLACK);
    gfx->drawString("+10", x + w - 60, y + 85);
    
    // Cancel/OK buttons
    gfx->setTextSize(1.2f);
    gfx->drawRect(x + 60, y + h - 50, 120, 40, COLOR_BLACK);
    gfx->drawString("Cancel", x + 120, y + h - 30);
    
    gfx->drawRect(x + w - 180, y + h - 50, 120, 40, COLOR_BLACK);
    gfx->drawString("OK", x + w - 120, y + h - 30);
}

// ==================== Picker Touch Handlers ====================
bool ComposerUI::handlePickerTouch(int x, int y) {
    switch (m_state) {
        case ComposerUIState::PITCH_PICKER:
            return handlePitchPickerTouch(x, y);
        case ComposerUIState::DURATION_PICKER:
            return handleDurationPickerTouch(x, y);
        case ComposerUIState::VELOCITY_PICKER:
            return handleVelocityPickerTouch(x, y);
        case ComposerUIState::DIVISION_PICKER:
            return handleDivisionPickerTouch(x, y);
        case ComposerUIState::TEMPO_INPUT:
            return handleTempoInputTouch(x, y);
        default:
            m_state = ComposerUIState::MAIN;
            m_needsFullRefresh = true;
            return true;
    }
}

bool ComposerUI::handlePitchPickerTouch(int x, int y) {
    int w = 500;
    int h = 340;
    int dx = (m_layout.screenW - w) / 2;
    int dy = (m_layout.screenH - h) / 2;
    
    if (x < dx || x >= dx + w || y < dy || y >= dy + h) {
        m_state = ComposerUIState::MAIN;
        m_needsFullRefresh = true;
        return true;
    }
    
    // Cancel button
    if (x >= dx + 100 && x < dx + 220 && y >= dy + h - 50 && y < dy + h - 10) {
        m_state = ComposerUIState::MAIN;
        m_needsFullRefresh = true;
        return true;
    }
    
    // OK button
    if (x >= dx + w - 220 && x < dx + w - 100 && y >= dy + h - 50 && y < dy + h - 10) {
        // Apply pitch change
        if (m_selectedNoteIndex >= 0) {
            SequencerEngine::getInstance().setNotePitch(m_selectedChannel, m_selectedNoteIndex, m_defaultPitch);
        }
        m_state = ComposerUIState::MAIN;
        m_needsFullRefresh = true;
        return true;
    }
    
    // Octave -/+ buttons
    uint8_t currentPitch = m_defaultPitch;
    int octave = currentPitch / 12;
    int noteInOctave = currentPitch % 12;
    
    if (x >= dx + 20 && x < dx + 80 && y >= dy + 270 && y < dy + 310) {
        // Decrease octave
        if (octave > 2) {
            m_defaultPitch = (octave - 1) * 12 + noteInOctave;
            m_needsFullRefresh = true;
        }
        return true;
    }
    
    if (x >= dx + w - 80 && x < dx + w - 20 && y >= dy + 270 && y < dy + 310) {
        // Increase octave
        if (octave < 8) {
            m_defaultPitch = (octave + 1) * 12 + noteInOctave;
            m_needsFullRefresh = true;
        }
        return true;
    }
    
    // Piano key touches
    int keyW = 38;
    int keyH = 100;
    int startX = dx + 20;
    int startY = dy + 50;
    
    // Check black keys first (they're on top)
    const bool isBlack[] = {false, true, false, true, false, false, true, false, true, false, true, false};
    
    if (y >= startY && y < startY + keyH * 2 / 3) {
        // Could be black key
        int relX = x - startX;
        int whiteKeyIdx = relX / keyW;
        int posInKey = relX % keyW;
        
        // Check if in black key zone (right side of white key)
        if (posInKey > keyW * 2 / 3 && whiteKeyIdx < 7) {
            // Map white key index to note, check if next note is black
            int whiteNotes[] = {0, 2, 4, 5, 7, 9, 11};
            if (whiteKeyIdx < 6 && isBlack[whiteNotes[whiteKeyIdx] + 1]) {
                m_defaultPitch = octave * 12 + whiteNotes[whiteKeyIdx] + 1;
                m_needsFullRefresh = true;
                return true;
            }
        }
    }
    
    // White keys
    if (y >= startY && y < startY + keyH && x >= startX && x < startX + 7 * keyW) {
        int whiteKeyIdx = (x - startX) / keyW;
        int whiteNotes[] = {0, 2, 4, 5, 7, 9, 11};
        if (whiteKeyIdx >= 0 && whiteKeyIdx < 7) {
            m_defaultPitch = octave * 12 + whiteNotes[whiteKeyIdx];
            m_needsFullRefresh = true;
        }
        return true;
    }
    
    return false;
}

bool ComposerUI::handleDurationPickerTouch(int x, int y) {
    int w = 400;
    int h = 280;
    int dx = (m_layout.screenW - w) / 2;
    int dy = (m_layout.screenH - h) / 2;
    
    if (x < dx || x >= dx + w || y < dy || y >= dy + h) {
        m_state = ComposerUIState::MAIN;
        m_needsFullRefresh = true;
        return true;
    }
    
    // Cancel button
    if (x >= dx + 60 && x < dx + 180 && y >= dy + h - 50 && y < dy + h - 10) {
        m_state = ComposerUIState::MAIN;
        m_needsFullRefresh = true;
        return true;
    }
    
    // OK button
    if (x >= dx + w - 180 && x < dx + w - 60 && y >= dy + h - 50 && y < dy + h - 10) {
        // Apply duration change
        if (m_selectedNoteIndex >= 0) {
            SequencerEngine::getInstance().setNoteDuration(m_selectedChannel, m_selectedNoteIndex, m_defaultDuration);
        }
        m_state = ComposerUIState::MAIN;
        m_needsFullRefresh = true;
        return true;
    }
    
    // Preset buttons
    const int durations[] = {1, 2, 4, 8, 16};
    int btnW = 70;
    int btnH = 50;
    int startX = dx + 20;
    int startY = dy + 50;
    
    for (int i = 0; i < 5; i++) {
        int bx = startX + i * (btnW + 5);
        if (x >= bx && x < bx + btnW && y >= startY && y < startY + btnH) {
            m_defaultDuration = durations[i];
            m_needsFullRefresh = true;
            return true;
        }
    }
    
    // Custom -/+ buttons
    if (x >= dx + 120 && x < dx + 170 && y >= startY + 60 && y < startY + 100) {
        // Decrease
        if (m_defaultDuration > 1) {
            m_defaultDuration--;
            m_needsFullRefresh = true;
        }
        return true;
    }
    
    if (x >= dx + w - 170 && x < dx + w - 120 && y >= startY + 60 && y < startY + 100) {
        // Increase
        if (m_defaultDuration < 32) {
            m_defaultDuration++;
            m_needsFullRefresh = true;
        }
        return true;
    }
    
    return false;
}

bool ComposerUI::handleVelocityPickerTouch(int x, int y) {
    int w = 400;
    int h = 250;
    int dx = (m_layout.screenW - w) / 2;
    int dy = (m_layout.screenH - h) / 2;
    
    if (x < dx || x >= dx + w || y < dy || y >= dy + h) {
        m_state = ComposerUIState::MAIN;
        m_needsFullRefresh = true;
        return true;
    }
    
    // Cancel button
    if (x >= dx + 60 && x < dx + 180 && y >= dy + h - 50 && y < dy + h - 10) {
        m_state = ComposerUIState::MAIN;
        m_needsFullRefresh = true;
        return true;
    }
    
    // OK button
    if (x >= dx + w - 180 && x < dx + w - 60 && y >= dy + h - 50 && y < dy + h - 10) {
        // Apply velocity change
        if (m_selectedNoteIndex >= 0) {
            SequencerEngine::getInstance().setNoteVelocity(m_selectedChannel, m_selectedNoteIndex, m_defaultVelocity);
        }
        m_state = ComposerUIState::MAIN;
        m_needsFullRefresh = true;
        return true;
    }
    
    // Preset buttons
    const int velocities[] = {32, 64, 80, 100, 127};
    int btnW = 70;
    int btnH = 50;
    int startX = dx + 20;
    int startY = dy + 50;
    
    for (int i = 0; i < 5; i++) {
        int bx = startX + i * (btnW + 5);
        if (x >= bx && x < bx + btnW && y >= startY && y < startY + btnH) {
            m_defaultVelocity = velocities[i];
            m_needsFullRefresh = true;
            return true;
        }
    }
    
    // Custom -/+ buttons
    if (x >= dx + 120 && x < dx + 170 && y >= startY + 60 && y < startY + 100) {
        // Decrease
        if (m_defaultVelocity > 1) {
            m_defaultVelocity -= 10;
            if (m_defaultVelocity < 1) m_defaultVelocity = 1;
            m_needsFullRefresh = true;
        }
        return true;
    }
    
    if (x >= dx + w - 170 && x < dx + w - 120 && y >= startY + 60 && y < startY + 100) {
        // Increase
        if (m_defaultVelocity < 127) {
            m_defaultVelocity += 10;
            if (m_defaultVelocity > 127) m_defaultVelocity = 127;
            m_needsFullRefresh = true;
        }
        return true;
    }
    
    return false;
}

bool ComposerUI::handleDivisionPickerTouch(int x, int y) {
    int w = 400;
    int h = 220;
    int dx = (m_layout.screenW - w) / 2;
    int dy = (m_layout.screenH - h) / 2;
    
    if (x < dx || x >= dx + w || y < dy || y >= dy + h) {
        m_state = ComposerUIState::MAIN;
        m_needsFullRefresh = true;
        return true;
    }
    
    // Cancel button
    if (x >= dx + 60 && x < dx + 180 && y >= dy + h - 50 && y < dy + h - 10) {
        m_state = ComposerUIState::MAIN;
        m_needsFullRefresh = true;
        return true;
    }
    
    // OK button - close without action (division is applied immediately)
    if (x >= dx + w - 180 && x < dx + w - 60 && y >= dy + h - 50 && y < dy + h - 10) {
        m_state = ComposerUIState::MAIN;
        m_needsFullRefresh = true;
        return true;
    }
    
    // Division buttons
    const StepDivision divisions[] = {
        StepDivision::QUARTER, StepDivision::EIGHTH, 
        StepDivision::SIXTEENTH, StepDivision::THIRTY_SECOND
    };
    int btnW = 80;
    int btnH = 50;
    int startX = dx + 30;
    int startY = dy + 60;
    
    for (int i = 0; i < 4; i++) {
        int bx = startX + i * (btnW + 10);
        if (x >= bx && x < bx + btnW && y >= startY && y < startY + btnH) {
            SequencerEngine::getInstance().setDivision(divisions[i]);
            computeLayout();
            m_needsFullRefresh = true;
            return true;
        }
    }
    
    return false;
}

bool ComposerUI::handleTempoInputTouch(int x, int y) {
    int w = 400;
    int h = 200;
    int dx = (m_layout.screenW - w) / 2;
    int dy = (m_layout.screenH - h) / 2;
    
    if (x < dx || x >= dx + w || y < dy || y >= dy + h) {
        m_state = ComposerUIState::MAIN;
        m_needsFullRefresh = true;
        return true;
    }
    
    uint16_t currentTempo = SequencerEngine::getInstance().getTempo();
    
    // Cancel button
    if (x >= dx + 60 && x < dx + 180 && y >= dy + h - 50 && y < dy + h - 10) {
        m_state = ComposerUIState::MAIN;
        m_needsFullRefresh = true;
        return true;
    }
    
    // OK button
    if (x >= dx + w - 180 && x < dx + w - 60 && y >= dy + h - 50 && y < dy + h - 10) {
        m_state = ComposerUIState::MAIN;
        m_needsFullRefresh = true;
        return true;
    }
    
    // -10 button
    if (x >= dx + 30 && x < dx + 90 && y >= dy + 60 && y < dy + 110) {
        SequencerEngine::getInstance().setTempo(currentTempo - 10);
        m_needsFullRefresh = true;
        return true;
    }
    
    // -1 button
    if (x >= dx + 100 && x < dx + 150 && y >= dy + 60 && y < dy + 110) {
        SequencerEngine::getInstance().setTempo(currentTempo - 1);
        m_needsFullRefresh = true;
        return true;
    }
    
    // +1 button
    if (x >= dx + w - 150 && x < dx + w - 100 && y >= dy + 60 && y < dy + 110) {
        SequencerEngine::getInstance().setTempo(currentTempo + 1);
        m_needsFullRefresh = true;
        return true;
    }
    
    // +10 button
    if (x >= dx + w - 90 && x < dx + w - 30 && y >= dy + 60 && y < dy + 110) {
        SequencerEngine::getInstance().setTempo(currentTempo + 10);
        m_needsFullRefresh = true;
        return true;
    }
    
    return false;
}

void ComposerUI::drawConfirmDialog(LovyanGFX* target) {
    LovyanGFX* gfx = target ? target : &m_canvas;
    int w = 500;
    int h = 220;
    int x = (m_layout.screenW - w) / 2;
    int y = (m_layout.screenH - h) / 2;
    
    gfx->fillRect(x, y, w, h, COLOR_WHITE);
    gfx->drawRect(x, y, w, h, COLOR_BLACK);
    gfx->drawRect(x + 2, y + 2, w - 4, h - 4, COLOR_BLACK);
    
    gfx->setTextColor(COLOR_BLACK);
    gfx->setTextSize(1.4f);
    gfx->setTextDatum(textdatum_t::top_center);
    gfx->drawString(m_confirmMessage.c_str(), x + w / 2, y + 20);
    
    gfx->setTextDatum(textdatum_t::middle_center);
    gfx->setTextSize(1.2f);
    
    if (m_confirmAction) {
        drawButton(gfx, x + 80, y + h - 60, 140, 45, "Cancel");
        drawButton(gfx, x + w - 220, y + h - 60, 140, 45, "OK");
    } else {
        drawButton(gfx, x + (w - 140) / 2, y + h - 60, 140, 45, "OK");
    }
}

bool ComposerUI::handleConfirmDialogTouch(int x, int y) {
    int w = 500;
    int h = 220;
    int dx = (m_layout.screenW - w) / 2;
    int dy = (m_layout.screenH - h) / 2;
    
    if (x < dx || x >= dx + w || y < dy || y >= dy + h) {
        m_state = ComposerUIState::MAIN;
        m_needsFullRefresh = true;
        return true;
    }
    
    if (m_confirmAction) {
        if (x >= dx + 80 && x < dx + 220 && y >= dy + h - 60 && y < dy + h - 15) {
            m_confirmAction = nullptr;
            m_state = ComposerUIState::MAIN;
            m_needsFullRefresh = true;
            return true;
        }
        if (x >= dx + w - 220 && x < dx + w - 80 && y >= dy + h - 60 && y < dy + h - 15) {
            m_confirmAction();
            m_confirmAction = nullptr;
            m_state = ComposerUIState::MAIN;
            m_needsFullRefresh = true;
            return true;
        }
    } else {
        if (x >= dx + (w - 140) / 2 && x < dx + (w + 140) / 2 && y >= dy + h - 60 && y < dy + h - 15) {
            m_confirmAction = nullptr;
            m_state = ComposerUIState::MAIN;
            m_needsFullRefresh = true;
            return true;
        }
    }
    
    return false;
}

int ComposerUI::stepToGridX(uint16_t step) const {
    return m_layout.gridX + (static_cast<int>(step) - static_cast<int>(m_viewStartStep)) * m_layout.cellW;
}

int ComposerUI::pitchToGridY(uint8_t pitch) const {
    int start = static_cast<int>(m_viewStartPitch);
    int rel = static_cast<int>(pitch) - start;
    if (rel < 0 || rel >= m_pitchRange) return -1;
    int row = (m_pitchRange - 1) - rel;
    return m_layout.gridY + row * m_layout.cellH;
}

int ComposerUI::gridXToStep(int x) const {
    return m_viewStartStep + (x - m_layout.gridX) / m_layout.cellW;
}

int ComposerUI::gridYToPitch(int y) const {
    int row = (y - m_layout.gridY) / m_layout.cellH;
    if (row < 0 || row >= m_pitchRange) return -1;
    int pitch = static_cast<int>(m_viewStartPitch) + (m_pitchRange - 1 - row);
    return pitch;
}

bool ComposerUI::isInGrid(int x, int y) const {
    return x >= m_layout.gridX && x < m_layout.gridX + m_layout.gridW &&
           y >= m_layout.gridY && y < m_layout.gridY + m_layout.gridH;
}

const char* ComposerUI::getInstrumentName(uint8_t program) {
    static const char* names[] = {
        "Piano", "Bright Piano", "E. Grand", "Honky-tonk",
        "E. Piano 1", "E. Piano 2", "Harpsichord", "Clavinet",
        "Celesta", "Glockenspiel", "Music Box", "Vibraphone",
        "Marimba", "Xylophone", "Tubular Bells", "Dulcimer",
        "Drawbar Org", "Perc. Org", "Rock Org", "Church Org",
        "Reed Org", "Accordion", "Harmonica", "Tango Acc",
        "Guitar Nyl", "Guitar Steel"
    };
    if (program < (sizeof(names) / sizeof(names[0]))) {
        return names[program];
    }
    return "Instrument";
}
