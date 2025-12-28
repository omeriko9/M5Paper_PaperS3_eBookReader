/**
 * @file drawing_app.cpp
 * @brief Implementation of the Drawing Application
 */

#include "drawing_app.h"
#include "device_hal.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <dirent.h>
#include <sys/stat.h>
#include <ctime>

static const char* TAG = "DrawingApp";

// Static member initialization
constexpr int DrawingApp::THICKNESS_PIXELS[6];

DrawingApp& DrawingApp::getInstance() {
    static DrawingApp instance;
    return instance;
}

DrawingApp::DrawingApp() {
    // Constructor
}

void DrawingApp::init() {
    if (m_initialized) return;
    
    ESP_LOGI(TAG, "Initializing Drawing App");
    
    // Create drawings directory on SD card if it doesn't exist
    struct stat st;
    if (stat(DRAWINGS_DIR, &st) != 0) {
        if (mkdir(DRAWINGS_DIR, 0755) != 0) {
            ESP_LOGW(TAG, "Failed to create drawings directory: %s", DRAWINGS_DIR);
        } else {
            ESP_LOGI(TAG, "Created drawings directory: %s", DRAWINGS_DIR);
        }
    }
    
    m_initialized = true;
}

void DrawingApp::deinit() {
    if (!m_initialized) return;
    
    if (m_canvasCreated) {
        m_canvas.deleteSprite();
        m_canvasCreated = false;
    }
    
    m_initialized = false;
}

void DrawingApp::enter() {
    ESP_LOGI(TAG, "Entering Drawing App");
    m_active = true;
    m_shouldExit = false;
    m_needsFullRefresh = true;
    m_state = DrawingState::DRAWING;
    m_isDrawing = false;
    m_lastX = -1;
    m_lastY = -1;
    
    // Create canvas for drawing if needed
    if (!m_canvasCreated) {
        int canvasW = SCREEN_WIDTH;
        int canvasH = CANVAS_HEIGHT;
        
        // Use 4-bit grayscale for better quality
        m_canvas.setColorDepth(4);
        m_canvas.setPsram(true);
        
        if (m_canvas.createSprite(canvasW, canvasH)) {
            m_canvasCreated = true;
            m_canvas.fillScreen(COLOR_WHITE);
            ESP_LOGI(TAG, "Created drawing canvas %dx%d", canvasW, canvasH);
        } else {
            // Try with internal RAM
            m_canvas.setPsram(false);
            if (m_canvas.createSprite(canvasW, canvasH)) {
                m_canvasCreated = true;
                m_canvas.fillScreen(COLOR_WHITE);
                ESP_LOGI(TAG, "Created drawing canvas in internal RAM %dx%d", canvasW, canvasH);
            } else {
                ESP_LOGE(TAG, "Failed to create drawing canvas");
            }
        }
    } else {
        // Canvas already exists, just clear it for a new session
        // Don't clear if we want to preserve drawing
    }
    
    draw();
}

void DrawingApp::exit() {
    ESP_LOGI(TAG, "Exiting Drawing App");
    m_active = false;
}

void DrawingApp::update() {
    if (!m_active) return;
    
    // Batch partial refreshes during drawing
    if (m_strokeDirty && !m_isDrawing) {
        uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
        if (now - m_lastRefreshTime > REFRESH_INTERVAL_MS) {
            // Refresh the dirty region
            int margin = THICKNESS_PIXELS[m_thickness - 1] + 2;
            int rx = std::max(0, m_strokeMinX - margin);
            int ry = std::max(0, m_strokeMinY - margin);
            int rw = std::min(SCREEN_WIDTH, m_strokeMaxX + margin) - rx;
            int rh = std::min((int)CANVAS_HEIGHT, m_strokeMaxY + margin - (int)CANVAS_Y) - (ry - CANVAS_Y);
            
            if (rw > 0 && rh > 0) {
                partialRefreshRegion(rx, ry, rw, rh);
            }
            
            m_strokeDirty = false;
            m_lastRefreshTime = now;
        }
    }
}

void DrawingApp::draw() {
    LovyanGFX* gfx = (LovyanGFX*)&M5.Display;
    
    if (m_state == DrawingState::FILE_DIALOG) {
        drawFileDialog(gfx);
        M5.Display.display();
        return;
    }
    
    if (m_state == DrawingState::CONFIRM_DIALOG) {
        drawConfirmDialog(gfx);
        M5.Display.display();
        return;
    }
    
    // Main drawing screen
    gfx->fillScreen(COLOR_WHITE);
    
    // Draw status bar area (black line)
    gfx->drawLine(0, STATUS_BAR_HEIGHT - 1, SCREEN_WIDTH, STATUS_BAR_HEIGHT - 1, COLOR_BLACK);
    
    drawTopBar(gfx);
    drawCanvas(gfx);
    drawBottomToolbar(gfx);
    
    M5.Display.display();
    m_needsFullRefresh = false;
}

void DrawingApp::drawTopBar(LovyanGFX* target) {
    LovyanGFX* gfx = target ? target : (LovyanGFX*)&M5.Display;
    int y = STATUS_BAR_HEIGHT;
    
    // Background
    gfx->fillRect(0, y, SCREEN_WIDTH, TOP_BAR_HEIGHT, COLOR_LIGHT_GRAY);
    gfx->drawLine(0, y + TOP_BAR_HEIGHT - 1, SCREEN_WIDTH, y + TOP_BAR_HEIGHT - 1, COLOR_BLACK);
    
    gfx->setTextColor(COLOR_BLACK, COLOR_LIGHT_GRAY);
    gfx->setTextSize(1.4f);
    
    // Back button
    drawButton(gfx, 10, y + 5, 70, 40, "Back", false, true);
    
    // Title
    gfx->setTextDatum(textdatum_t::middle_center);
    gfx->drawString("Draw!", SCREEN_WIDTH / 2, y + TOP_BAR_HEIGHT / 2);
    
    // Clear button
    drawButton(gfx, 180, y + 5, 70, 40, "Clear", false, true);
    
    // Save button
    drawButton(gfx, 260, y + 5, 70, 40, "Save", false, true);
    
    // Load button
    drawButton(gfx, 340, y + 5, 70, 40, "Load", false, true);
    
    // New button
    drawButton(gfx, 420, y + 5, 70, 40, "New", false, true);
    
    gfx->setTextDatum(textdatum_t::top_left);
}

void DrawingApp::drawBottomToolbar(LovyanGFX* target) {
    LovyanGFX* gfx = target ? target : (LovyanGFX*)&M5.Display;
    int y = SCREEN_HEIGHT - BOTTOM_TOOLBAR_HEIGHT;
    
    // Background
    gfx->fillRect(0, y, SCREEN_WIDTH, BOTTOM_TOOLBAR_HEIGHT, COLOR_LIGHT_GRAY);
    gfx->drawLine(0, y, SCREEN_WIDTH, y, COLOR_BLACK);
    
    gfx->setTextColor(COLOR_BLACK, COLOR_LIGHT_GRAY);
    gfx->setTextSize(1.2f);
    
    // Tool selection - left side
    int toolY = y + 10;
    gfx->setTextDatum(textdatum_t::middle_left);
    gfx->drawString("Tool:", 15, toolY + 30);
    
    // Brush button
    bool brushSelected = (m_currentTool == DrawingTool::BRUSH);
    drawButton(gfx, 70, toolY, 80, 60, "Brush", brushSelected, true);
    
    // Eraser button
    bool eraserSelected = (m_currentTool == DrawingTool::ERASER);
    drawButton(gfx, 160, toolY, 80, 60, "Eraser", eraserSelected, true);
    
    // Thickness selection - right side
    gfx->setTextDatum(textdatum_t::middle_left);
    gfx->drawString("Size:", 260, toolY + 30);
    
    // Draw 6 thickness buttons
    int thickX = 310;
    int thickSpacing = 36;
    for (int i = 1; i <= MAX_THICKNESS; i++) {
        int btnX = thickX + (i - 1) * thickSpacing;
        drawThicknessButton(gfx, btnX, toolY + 10, 32, i, (m_thickness == i));
    }
    
    gfx->setTextDatum(textdatum_t::top_left);
}

void DrawingApp::drawCanvas(LovyanGFX* target) {
    if (!m_canvasCreated) return;
    
    // Push the canvas to display
    m_canvas.pushSprite(&M5.Display, 0, CANVAS_Y);
}

void DrawingApp::drawButton(LovyanGFX* gfx, int x, int y, int w, int h, 
                            const char* label, bool selected, bool enabled) {
    uint16_t bgColor = selected ? COLOR_DARK_GRAY : COLOR_WHITE;
    uint16_t textColor = selected ? COLOR_WHITE : COLOR_BLACK;
    
    if (!enabled) {
        bgColor = COLOR_LIGHT_GRAY;
        textColor = COLOR_MEDIUM_GRAY;
    }
    
    gfx->fillRect(x, y, w, h, bgColor);
    gfx->drawRect(x, y, w, h, COLOR_BLACK);
    if (selected) {
        gfx->drawRect(x + 1, y + 1, w - 2, h - 2, COLOR_BLACK);
    }
    
    gfx->setTextColor(textColor, bgColor);
    gfx->setTextDatum(textdatum_t::middle_center);
    gfx->drawString(label, x + w / 2, y + h / 2);
}

void DrawingApp::drawThicknessButton(LovyanGFX* gfx, int x, int y, int size, int thickness, bool selected) {
    uint16_t bgColor = selected ? COLOR_DARK_GRAY : COLOR_WHITE;
    uint16_t dotColor = selected ? COLOR_WHITE : COLOR_BLACK;
    
    gfx->fillRect(x, y, size, size + 8, bgColor);
    gfx->drawRect(x, y, size, size + 8, COLOR_BLACK);
    
    // Draw a dot representing the thickness
    int dotRadius = THICKNESS_PIXELS[thickness - 1] / 2;
    if (dotRadius < 1) dotRadius = 1;
    if (dotRadius > size / 2 - 2) dotRadius = size / 2 - 2;
    
    gfx->fillCircle(x + size / 2, y + size / 2, dotRadius, dotColor);
    
    // Draw thickness number below
    gfx->setTextColor(selected ? COLOR_WHITE : COLOR_BLACK, bgColor);
    gfx->setTextSize(0.9f);
    gfx->setTextDatum(textdatum_t::top_center);
    char numStr[2] = {(char)('0' + thickness), '\0'};
    gfx->drawString(numStr, x + size / 2, y + size - 2);
}

void DrawingApp::clearCanvas() {
    if (!m_canvasCreated) return;
    
    m_canvas.fillScreen(COLOR_WHITE);
    m_needsFullRefresh = true;
}

void DrawingApp::drawStroke(int x0, int y0, int x1, int y1) {
    if (!m_canvasCreated) return;
    
    // Convert screen coordinates to canvas coordinates
    int cy0 = y0 - CANVAS_Y;
    int cy1 = y1 - CANVAS_Y;
    
    // Check bounds
    if (cy0 < 0 && cy1 < 0) return;
    if (cy0 >= (int)CANVAS_HEIGHT && cy1 >= (int)CANVAS_HEIGHT) return;
    
    uint16_t color = (m_currentTool == DrawingTool::ERASER) ? COLOR_WHITE : COLOR_BLACK;
    int thickness = THICKNESS_PIXELS[m_thickness - 1];
    
    // Draw line with thickness using filled circles (Bresenham + circles)
    int dx = abs(x1 - x0);
    int dy = abs(y1 - y0);
    int sx = (x0 < x1) ? 1 : -1;
    int sy = (y0 < y1) ? 1 : -1;
    int err = dx - dy;
    
    while (true) {
        // Draw filled circle at current point
        int cy = y0 - CANVAS_Y;
        if (cy >= -thickness && cy < (int)CANVAS_HEIGHT + thickness) {
            if (x0 >= -thickness && x0 < SCREEN_WIDTH + thickness) {
                m_canvas.fillCircle(x0, cy, thickness / 2, color);
            }
        }
        
        // Update dirty region
        m_strokeMinX = std::min(m_strokeMinX, x0);
        m_strokeMinY = std::min(m_strokeMinY, y0);
        m_strokeMaxX = std::max(m_strokeMaxX, x0);
        m_strokeMaxY = std::max(m_strokeMaxY, y0);
        
        if (x0 == x1 && y0 == y1) break;
        
        int e2 = 2 * err;
        if (e2 > -dy) {
            err -= dy;
            x0 += sx;
        }
        if (e2 < dx) {
            err += dx;
            y0 += sy;
        }
    }
    
    m_strokeDirty = true;
}

void DrawingApp::drawPoint(int x, int y) {
    if (!m_canvasCreated) return;
    
    int cy = y - CANVAS_Y;
    if (cy < 0 || cy >= (int)CANVAS_HEIGHT) return;
    if (x < 0 || x >= SCREEN_WIDTH) return;
    
    uint16_t color = (m_currentTool == DrawingTool::ERASER) ? COLOR_WHITE : COLOR_BLACK;
    int thickness = THICKNESS_PIXELS[m_thickness - 1];
    
    m_canvas.fillCircle(x, cy, thickness / 2, color);
    
    // Update dirty region
    m_strokeMinX = std::min(m_strokeMinX, x);
    m_strokeMinY = std::min(m_strokeMinY, y);
    m_strokeMaxX = std::max(m_strokeMaxX, x);
    m_strokeMaxY = std::max(m_strokeMaxY, y);
    m_strokeDirty = true;
}

void DrawingApp::partialRefreshRegion(int x, int y, int w, int h) {
    if (!m_canvasCreated) return;
    
    // Clamp to canvas bounds and adjust for canvas offset
    int canvasX = std::max(0, x);
    int canvasY = std::max(0, y - CANVAS_Y);
    int canvasW = std::min(w, SCREEN_WIDTH - canvasX);
    int canvasH = std::min(h, (int)CANVAS_HEIGHT - canvasY);
    
    if (canvasW <= 0 || canvasH <= 0) return;
    
    // Push partial region to display
    // Create a temporary sprite for the region
    M5Canvas tempSprite;
    tempSprite.setColorDepth(4);
    tempSprite.setPsram(true);
    
    if (tempSprite.createSprite(canvasW, canvasH)) {
        // Copy region from main canvas
        for (int py = 0; py < canvasH; py++) {
            for (int px = 0; px < canvasW; px++) {
                uint32_t pixel = m_canvas.readPixel(canvasX + px, canvasY + py);
                tempSprite.drawPixel(px, py, pixel);
            }
        }
        
        // Push to display at the correct screen position
        int screenY = canvasY + CANVAS_Y;
        tempSprite.pushSprite(&M5.Display, canvasX, screenY);
        
        // Use fast EPD mode for partial refresh
        M5.Display.setEpdMode(lgfx::epd_mode_t::epd_fastest);
        M5.Display.display();
        M5.Display.setEpdMode(lgfx::epd_mode_t::epd_quality);
        
        tempSprite.deleteSprite();
    } else {
        // Fallback: push entire canvas
        m_canvas.pushSprite(&M5.Display, 0, CANVAS_Y);
        M5.Display.display();
    }
}

void DrawingApp::fullRefresh() {
    draw();
}

// File operations

bool DrawingApp::saveDrawing(const std::string& filename) {
    if (!m_canvasCreated) {
        ESP_LOGE(TAG, "Cannot save: no canvas");
        return false;
    }
    
    std::string fullPath = std::string(DRAWINGS_DIR) + "/" + filename;
    
    // Save as raw bitmap format
    // Format: 4 bytes width, 4 bytes height, then pixel data (4bpp packed)
    FILE* f = fopen(fullPath.c_str(), "wb");
    if (!f) {
        ESP_LOGE(TAG, "Failed to open file for writing: %s", fullPath.c_str());
        return false;
    }
    
    uint32_t width = m_canvas.width();
    uint32_t height = m_canvas.height();
    
    // Write header
    fwrite(&width, sizeof(uint32_t), 1, f);
    fwrite(&height, sizeof(uint32_t), 1, f);
    
    // Write pixel data row by row
    // Each pixel is 4 bits, so 2 pixels per byte
    uint8_t* rowBuffer = new uint8_t[(width + 1) / 2];
    
    for (uint32_t y = 0; y < height; y++) {
        for (uint32_t x = 0; x < width; x += 2) {
            uint8_t p1 = 0, p2 = 0;
            
            // Get pixel values (0-15 grayscale)
            uint32_t c1 = m_canvas.readPixel(x, y);
            p1 = (c1 == COLOR_BLACK) ? 0 : 15;  // Simplified: black or white
            
            if (x + 1 < width) {
                uint32_t c2 = m_canvas.readPixel(x + 1, y);
                p2 = (c2 == COLOR_BLACK) ? 0 : 15;
            }
            
            rowBuffer[x / 2] = (p1 << 4) | p2;
        }
        fwrite(rowBuffer, 1, (width + 1) / 2, f);
    }
    
    delete[] rowBuffer;
    fclose(f);
    
    ESP_LOGI(TAG, "Saved drawing to %s (%ux%u)", fullPath.c_str(), width, height);
    return true;
}

bool DrawingApp::loadDrawing(const std::string& filename) {
    std::string fullPath = std::string(DRAWINGS_DIR) + "/" + filename;
    
    FILE* f = fopen(fullPath.c_str(), "rb");
    if (!f) {
        ESP_LOGE(TAG, "Failed to open file for reading: %s", fullPath.c_str());
        return false;
    }
    
    uint32_t width, height;
    fread(&width, sizeof(uint32_t), 1, f);
    fread(&height, sizeof(uint32_t), 1, f);
    
    // Validate dimensions
    if (width == 0 || height == 0 || width > 2000 || height > 2000) {
        ESP_LOGE(TAG, "Invalid dimensions in file: %ux%u", width, height);
        fclose(f);
        return false;
    }
    
    // Recreate canvas if needed
    if (!m_canvasCreated || m_canvas.width() != (int)width || m_canvas.height() != (int)height) {
        if (m_canvasCreated) {
            m_canvas.deleteSprite();
        }
        
        m_canvas.setColorDepth(4);
        m_canvas.setPsram(true);
        
        if (!m_canvas.createSprite(width, height)) {
            ESP_LOGE(TAG, "Failed to create canvas for loaded image");
            fclose(f);
            return false;
        }
        m_canvasCreated = true;
    }
    
    // Read pixel data
    uint8_t* rowBuffer = new uint8_t[(width + 1) / 2];
    
    for (uint32_t y = 0; y < height; y++) {
        fread(rowBuffer, 1, (width + 1) / 2, f);
        
        for (uint32_t x = 0; x < width; x += 2) {
            uint8_t packed = rowBuffer[x / 2];
            uint8_t p1 = (packed >> 4) & 0x0F;
            uint8_t p2 = packed & 0x0F;
            
            m_canvas.drawPixel(x, y, (p1 < 8) ? COLOR_BLACK : COLOR_WHITE);
            if (x + 1 < width) {
                m_canvas.drawPixel(x + 1, y, (p2 < 8) ? COLOR_BLACK : COLOR_WHITE);
            }
        }
    }
    
    delete[] rowBuffer;
    fclose(f);
    
    m_currentFilename = filename;
    ESP_LOGI(TAG, "Loaded drawing from %s (%ux%u)", fullPath.c_str(), width, height);
    
    m_needsFullRefresh = true;
    return true;
}

void DrawingApp::scanDrawingFiles() {
    m_fileList.clear();
    
    DIR* dir = opendir(DRAWINGS_DIR);
    if (!dir) {
        ESP_LOGW(TAG, "Cannot open drawings directory: %s", DRAWINGS_DIR);
        return;
    }
    
    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        if (entry->d_type == DT_REG) {
            std::string name = entry->d_name;
            // Only show .drw files
            if (name.length() > 4 && name.substr(name.length() - 4) == ".drw") {
                m_fileList.push_back(name);
            }
        }
    }
    closedir(dir);
    
    // Sort alphabetically
    std::sort(m_fileList.begin(), m_fileList.end());
    
    ESP_LOGI(TAG, "Found %d drawing files", (int)m_fileList.size());
}

std::string DrawingApp::generateFilename() {
    // Generate filename based on timestamp
    time_t now = time(nullptr);
    struct tm* t = localtime(&now);
    
    char buf[64];
    snprintf(buf, sizeof(buf), "drawing_%04d%02d%02d_%02d%02d%02d.drw",
             t->tm_year + 1900, t->tm_mon + 1, t->tm_mday,
             t->tm_hour, t->tm_min, t->tm_sec);
    
    return std::string(buf);
}

// Touch handling

bool DrawingApp::handleTouch(int x, int y) {
    if (!m_active) return false;
    
    if (m_state == DrawingState::FILE_DIALOG) {
        return handleFileDialogTouch(x, y);
    }
    
    if (m_state == DrawingState::CONFIRM_DIALOG) {
        return handleConfirmDialogTouch(x, y);
    }
    
    // Check which area was touched
    if (y < STATUS_BAR_HEIGHT + TOP_BAR_HEIGHT) {
        return handleTopBarTouch(x, y);
    }
    
    if (y >= SCREEN_HEIGHT - BOTTOM_TOOLBAR_HEIGHT) {
        return handleToolbarTouch(x, y);
    }
    
    // Canvas area - handled by touch start/move/end
    return handleCanvasTouch(x, y);
}

void DrawingApp::handleTouchStart(int x, int y) {
    if (!m_active || m_state != DrawingState::DRAWING) return;
    
    // Only handle touches in canvas area
    if (y < CANVAS_Y || y >= SCREEN_HEIGHT - BOTTOM_TOOLBAR_HEIGHT) {
        return;
    }
    
    m_isDrawing = true;
    m_lastX = x;
    m_lastY = y;
    
    // Reset dirty region
    m_strokeMinX = x;
    m_strokeMinY = y;
    m_strokeMaxX = x;
    m_strokeMaxY = y;
    
    // Draw initial point
    drawPoint(x, y);
}

void DrawingApp::handleTouchMove(int x, int y) {
    if (!m_active || !m_isDrawing || m_state != DrawingState::DRAWING) return;
    
    // Clamp to canvas area
    y = std::max((int)CANVAS_Y, std::min(y, (int)(SCREEN_HEIGHT - BOTTOM_TOOLBAR_HEIGHT - 1)));
    x = std::max(0, std::min(x, (int)(SCREEN_WIDTH - 1)));
    
    if (m_lastX >= 0 && m_lastY >= 0) {
        drawStroke(m_lastX, m_lastY, x, y);
    }
    
    m_lastX = x;
    m_lastY = y;
    
    // Immediate partial refresh for smooth drawing
    // Push affected region to display
    if (m_strokeDirty) {
        int margin = THICKNESS_PIXELS[m_thickness - 1] + 4;
        int rx = std::max(0, m_strokeMinX - margin);
        int screenRy = std::max((int)CANVAS_Y, m_strokeMinY - margin);
        int rw = std::min((int)SCREEN_WIDTH, m_strokeMaxX + margin) - rx + 1;
        int rh = std::min((int)(SCREEN_HEIGHT - BOTTOM_TOOLBAR_HEIGHT), m_strokeMaxY + margin) - screenRy + 1;
        
        if (rw > 0 && rh > 0) {
            // Use fast EPD mode for drawing
            M5.Display.setEpdMode(lgfx::epd_mode_t::epd_fastest);
            m_canvas.pushSprite(&M5.Display, 0, CANVAS_Y);
            M5.Display.display();
        }
        
        // Reset dirty tracking for next segment
        m_strokeMinX = x;
        m_strokeMinY = y;
        m_strokeMaxX = x;
        m_strokeMaxY = y;
    }
}

void DrawingApp::handleTouchEnd(int x, int y) {
    if (!m_active) return;
    
    if (m_isDrawing) {
        m_isDrawing = false;
        
        // Final refresh with better quality
        M5.Display.setEpdMode(lgfx::epd_mode_t::epd_fast);
        m_canvas.pushSprite(&M5.Display, 0, CANVAS_Y);
        M5.Display.display();
        M5.Display.setEpdMode(lgfx::epd_mode_t::epd_quality);
    }
    
    m_lastX = -1;
    m_lastY = -1;
    m_strokeDirty = false;
}

bool DrawingApp::handleTopBarTouch(int x, int y) {
    int barY = STATUS_BAR_HEIGHT;
    
    // Back button (10, barY+5, 70, 40)
    if (x >= 10 && x < 80 && y >= barY + 5 && y < barY + 45) {
        m_shouldExit = true;
        return true;
    }
    
    // Clear button (180, barY+5, 70, 40)
    if (x >= 180 && x < 250 && y >= barY + 5 && y < barY + 45) {
        m_confirmType = DrawingConfirmType::CLEAR;
        m_confirmMessage = "Clear the drawing?";
        m_state = DrawingState::CONFIRM_DIALOG;
        draw();
        return true;
    }
    
    // Save button (260, barY+5, 70, 40)
    if (x >= 260 && x < 330 && y >= barY + 5 && y < barY + 45) {
        m_fileMode = DrawingFileMode::SAVE;
        scanDrawingFiles();
        m_selectedFileIndex = -1;
        m_fileListScroll = 0;
        m_state = DrawingState::FILE_DIALOG;
        draw();
        return true;
    }
    
    // Load button (340, barY+5, 70, 40)
    if (x >= 340 && x < 410 && y >= barY + 5 && y < barY + 45) {
        m_fileMode = DrawingFileMode::LOAD;
        scanDrawingFiles();
        m_selectedFileIndex = -1;
        m_fileListScroll = 0;
        m_state = DrawingState::FILE_DIALOG;
        draw();
        return true;
    }
    
    // New button (420, barY+5, 70, 40)
    if (x >= 420 && x < 490 && y >= barY + 5 && y < barY + 45) {
        m_confirmType = DrawingConfirmType::NEW_DRAWING;
        m_confirmMessage = "Start a new drawing?";
        m_state = DrawingState::CONFIRM_DIALOG;
        draw();
        return true;
    }
    
    return false;
}

bool DrawingApp::handleToolbarTouch(int x, int y) {
    int toolY = SCREEN_HEIGHT - BOTTOM_TOOLBAR_HEIGHT + 10;
    
    // Brush button (70, toolY, 80, 60)
    if (x >= 70 && x < 150 && y >= toolY && y < toolY + 60) {
        m_currentTool = DrawingTool::BRUSH;
        drawBottomToolbar(nullptr);
        M5.Display.display();
        return true;
    }
    
    // Eraser button (160, toolY, 80, 60)
    if (x >= 160 && x < 240 && y >= toolY && y < toolY + 60) {
        m_currentTool = DrawingTool::ERASER;
        drawBottomToolbar(nullptr);
        M5.Display.display();
        return true;
    }
    
    // Thickness buttons (310 + (i-1)*36, toolY+10, 32, 40)
    int thickX = 310;
    int thickSpacing = 36;
    for (int i = 1; i <= MAX_THICKNESS; i++) {
        int btnX = thickX + (i - 1) * thickSpacing;
        if (x >= btnX && x < btnX + 32 && y >= toolY + 10 && y < toolY + 50) {
            m_thickness = i;
            drawBottomToolbar(nullptr);
            M5.Display.display();
            return true;
        }
    }
    
    return false;
}

bool DrawingApp::handleCanvasTouch(int x, int y) {
    // Canvas touch is handled by touchStart/Move/End
    return false;
}

// File dialog

void DrawingApp::drawFileDialog(LovyanGFX* target) {
    LovyanGFX* gfx = target ? target : (LovyanGFX*)&M5.Display;
    
    gfx->fillScreen(COLOR_WHITE);
    
    // Header
    int headerY = STATUS_BAR_HEIGHT;
    gfx->fillRect(0, headerY, SCREEN_WIDTH, TOP_BAR_HEIGHT, COLOR_LIGHT_GRAY);
    gfx->drawLine(0, headerY + TOP_BAR_HEIGHT - 1, SCREEN_WIDTH, headerY + TOP_BAR_HEIGHT - 1, COLOR_BLACK);
    
    gfx->setTextColor(COLOR_BLACK, COLOR_LIGHT_GRAY);
    gfx->setTextSize(1.5f);
    gfx->setTextDatum(textdatum_t::middle_center);
    
    const char* title = (m_fileMode == DrawingFileMode::SAVE) ? "Save Drawing" : "Load Drawing";
    gfx->drawString(title, SCREEN_WIDTH / 2, headerY + TOP_BAR_HEIGHT / 2);
    
    // Back button
    drawButton(gfx, 10, headerY + 5, 70, 40, "Cancel", false, true);
    
    // File list area
    int listY = headerY + TOP_BAR_HEIGHT + 10;
    int listH = SCREEN_HEIGHT - listY - 120;
    
    gfx->setTextSize(1.2f);
    gfx->setTextDatum(textdatum_t::middle_left);
    
    if (m_fileList.empty() && m_fileMode == DrawingFileMode::LOAD) {
        gfx->setTextColor(COLOR_MEDIUM_GRAY, COLOR_WHITE);
        gfx->setTextDatum(textdatum_t::middle_center);
        gfx->drawString("No drawings found", SCREEN_WIDTH / 2, listY + listH / 2);
    } else {
        int itemH = 50;
        int visibleItems = listH / itemH;
        
        for (int i = 0; i < visibleItems && (i + m_fileListScroll) < (int)m_fileList.size(); i++) {
            int idx = i + m_fileListScroll;
            int itemY = listY + i * itemH;
            
            bool selected = (idx == m_selectedFileIndex);
            if (selected) {
                gfx->fillRect(10, itemY, SCREEN_WIDTH - 20, itemH - 2, COLOR_LIGHT_GRAY);
            }
            
            gfx->setTextColor(COLOR_BLACK, selected ? COLOR_LIGHT_GRAY : COLOR_WHITE);
            gfx->setTextDatum(textdatum_t::middle_left);
            
            // Show filename without extension
            std::string name = m_fileList[idx];
            if (name.length() > 4) {
                name = name.substr(0, name.length() - 4);
            }
            gfx->drawString(name.c_str(), 20, itemY + itemH / 2);
            
            gfx->drawLine(10, itemY + itemH - 2, SCREEN_WIDTH - 10, itemY + itemH - 2, COLOR_LIGHT_GRAY);
        }
    }
    
    // Scroll indicators
    if (m_fileListScroll > 0) {
        gfx->fillTriangle(SCREEN_WIDTH / 2, listY - 5, 
                          SCREEN_WIDTH / 2 - 15, listY + 10,
                          SCREEN_WIDTH / 2 + 15, listY + 10, COLOR_BLACK);
    }
    
    int maxScroll = std::max(0, (int)m_fileList.size() - (listH / 50));
    if (m_fileListScroll < maxScroll) {
        int bottomY = listY + listH;
        gfx->fillTriangle(SCREEN_WIDTH / 2, bottomY + 5,
                          SCREEN_WIDTH / 2 - 15, bottomY - 10,
                          SCREEN_WIDTH / 2 + 15, bottomY - 10, COLOR_BLACK);
    }
    
    // Bottom buttons
    int btnY = SCREEN_HEIGHT - 100;
    
    if (m_fileMode == DrawingFileMode::SAVE) {
        // Save as new file button
        drawButton(gfx, SCREEN_WIDTH / 2 - 100, btnY, 200, 50, "Save New", false, true);
        
        // Overwrite selected (if file selected)
        if (m_selectedFileIndex >= 0) {
            drawButton(gfx, SCREEN_WIDTH / 2 - 100, btnY + 55, 200, 40, "Overwrite", false, true);
        }
    } else {
        // Load button (only if file selected)
        bool canLoad = (m_selectedFileIndex >= 0);
        drawButton(gfx, SCREEN_WIDTH / 2 - 60, btnY, 120, 50, "Load", false, canLoad);
    }
    
    gfx->setTextDatum(textdatum_t::top_left);
}

bool DrawingApp::handleFileDialogTouch(int x, int y) {
    int headerY = STATUS_BAR_HEIGHT;
    
    // Cancel button
    if (x >= 10 && x < 80 && y >= headerY + 5 && y < headerY + 45) {
        m_state = DrawingState::DRAWING;
        draw();
        return true;
    }
    
    // File list area
    int listY = headerY + TOP_BAR_HEIGHT + 10;
    int listH = SCREEN_HEIGHT - listY - 120;
    int itemH = 50;
    
    if (y >= listY && y < listY + listH) {
        int itemIdx = (y - listY) / itemH + m_fileListScroll;
        if (itemIdx >= 0 && itemIdx < (int)m_fileList.size()) {
            m_selectedFileIndex = itemIdx;
            draw();
            return true;
        }
    }
    
    // Scroll up
    if (y < listY && m_fileListScroll > 0) {
        m_fileListScroll--;
        draw();
        return true;
    }
    
    // Scroll down
    int maxScroll = std::max(0, (int)m_fileList.size() - (listH / itemH));
    if (y >= listY + listH && y < listY + listH + 30 && m_fileListScroll < maxScroll) {
        m_fileListScroll++;
        draw();
        return true;
    }
    
    // Bottom buttons
    int btnY = SCREEN_HEIGHT - 100;
    
    if (m_fileMode == DrawingFileMode::SAVE) {
        // Save new button
        if (x >= SCREEN_WIDTH / 2 - 100 && x < SCREEN_WIDTH / 2 + 100 &&
            y >= btnY && y < btnY + 50) {
            std::string filename = generateFilename();
            if (saveDrawing(filename)) {
                m_currentFilename = filename;
            }
            m_state = DrawingState::DRAWING;
            draw();
            return true;
        }
        
        // Overwrite button
        if (m_selectedFileIndex >= 0 &&
            x >= SCREEN_WIDTH / 2 - 100 && x < SCREEN_WIDTH / 2 + 100 &&
            y >= btnY + 55 && y < btnY + 95) {
            std::string filename = m_fileList[m_selectedFileIndex];
            if (saveDrawing(filename)) {
                m_currentFilename = filename;
            }
            m_state = DrawingState::DRAWING;
            draw();
            return true;
        }
    } else {
        // Load button
        if (m_selectedFileIndex >= 0 &&
            x >= SCREEN_WIDTH / 2 - 60 && x < SCREEN_WIDTH / 2 + 60 &&
            y >= btnY && y < btnY + 50) {
            loadDrawing(m_fileList[m_selectedFileIndex]);
            m_state = DrawingState::DRAWING;
            draw();
            return true;
        }
    }
    
    return false;
}

// Confirm dialog

void DrawingApp::drawConfirmDialog(LovyanGFX* target) {
    LovyanGFX* gfx = target ? target : (LovyanGFX*)&M5.Display;
    
    // Dim background
    gfx->fillScreen(COLOR_LIGHT_GRAY);
    
    // Dialog box
    int dialogW = 400;
    int dialogH = 200;
    int dialogX = (SCREEN_WIDTH - dialogW) / 2;
    int dialogY = (SCREEN_HEIGHT - dialogH) / 2;
    
    gfx->fillRect(dialogX, dialogY, dialogW, dialogH, COLOR_WHITE);
    gfx->drawRect(dialogX, dialogY, dialogW, dialogH, COLOR_BLACK);
    gfx->drawRect(dialogX + 1, dialogY + 1, dialogW - 2, dialogH - 2, COLOR_BLACK);
    
    // Message
    gfx->setTextColor(COLOR_BLACK, COLOR_WHITE);
    gfx->setTextSize(1.4f);
    gfx->setTextDatum(textdatum_t::middle_center);
    gfx->drawString(m_confirmMessage.c_str(), SCREEN_WIDTH / 2, dialogY + 70);
    
    // Buttons
    int btnY = dialogY + dialogH - 70;
    drawButton(gfx, dialogX + 40, btnY, 100, 50, "Cancel", false, true);
    drawButton(gfx, dialogX + dialogW - 140, btnY, 100, 50, "OK", false, true);
    
    gfx->setTextDatum(textdatum_t::top_left);
}

bool DrawingApp::handleConfirmDialogTouch(int x, int y) {
    int dialogW = 400;
    int dialogH = 200;
    int dialogX = (SCREEN_WIDTH - dialogW) / 2;
    int dialogY = (SCREEN_HEIGHT - dialogH) / 2;
    int btnY = dialogY + dialogH - 70;
    
    // Cancel button
    if (x >= dialogX + 40 && x < dialogX + 140 && y >= btnY && y < btnY + 50) {
        m_state = DrawingState::DRAWING;
        draw();
        return true;
    }
    
    // OK button
    if (x >= dialogX + dialogW - 140 && x < dialogX + dialogW - 40 && y >= btnY && y < btnY + 50) {
        switch (m_confirmType) {
            case DrawingConfirmType::CLEAR:
                clearCanvas();
                m_currentFilename.clear();
                break;
            case DrawingConfirmType::NEW_DRAWING:
                clearCanvas();
                m_currentFilename.clear();
                break;
            case DrawingConfirmType::OVERWRITE_SAVE:
                // Handled elsewhere
                break;
        }
        m_state = DrawingState::DRAWING;
        draw();
        return true;
    }
    
    return false;
}
