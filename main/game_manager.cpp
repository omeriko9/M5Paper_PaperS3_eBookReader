#include "game_manager.h"
#include "esp_log.h"
#include "esp_random.h"
#include <algorithm>
#include <cstring>
#include <cstdlib>
#include <vector>

static const char* TAG = "GameMgr";

// Screen dimensions
static constexpr int SCREEN_WIDTH = 540;
static constexpr int SCREEN_HEIGHT = 960;
static constexpr int STATUS_BAR_HEIGHT = 44;
static constexpr int HEADER_HEIGHT = 50;

// Grayscale colors for better UI
static constexpr uint16_t GRAY_LIGHT = 0xDEF7;    // Light gray
static constexpr uint16_t GRAY_MEDIUM = 0x9492;   // Medium gray
static constexpr uint16_t GRAY_DARK = 0x4228;     // Dark gray

GameManager& GameManager::getInstance() {
    static GameManager instance;
    return instance;
}

GameManager::GameManager() {
    // Constructor
}

void GameManager::init() {
    if (m_initialized) return;
    m_initialized = true;
    ESP_LOGI(TAG, "GameManager initialized");
}

void GameManager::drawButton(LovyanGFX* target, int x, int y, int w, int h, const char* label,
                             uint16_t bgColor, uint16_t textColor) {
    LovyanGFX* gfx = target ? target : (LovyanGFX*)&M5.Display;
    gfx->fillRect(x, y, w, h, bgColor);
    gfx->drawRect(x, y, w, h, TFT_BLACK);
    if (label) {
        gfx->setTextColor(textColor, bgColor);
        gfx->setTextDatum(textdatum_t::middle_center);
        gfx->drawString(label, x + w/2, y + h/2);
        gfx->setTextDatum(textdatum_t::top_left);
    }
}

void GameManager::drawHeader(LovyanGFX* target, const char* title) {
    LovyanGFX* gfx = target ? target : (LovyanGFX*)&M5.Display;
    int headerY = STATUS_BAR_HEIGHT;
    gfx->fillRect(0, headerY, SCREEN_WIDTH, HEADER_HEIGHT, GRAY_LIGHT);
    gfx->drawLine(0, headerY + HEADER_HEIGHT - 1, SCREEN_WIDTH, headerY + HEADER_HEIGHT - 1, TFT_BLACK);
    gfx->setTextColor(TFT_BLACK, GRAY_LIGHT);
    gfx->setTextSize(1.8f);
    gfx->setTextDatum(textdatum_t::middle_center);
    gfx->drawString(title, SCREEN_WIDTH/2, headerY + HEADER_HEIGHT/2);
    gfx->setTextDatum(textdatum_t::top_left);
}

void GameManager::drawGamesMenu() {
    // This is now handled by GUI::drawGamesMenu()
    // This function is kept for backward compatibility but should not be called
}

bool GameManager::handleMenuTouch(int x, int y) {
    // This is now handled by GUI::onGamesMenuClick()
    // Return false as GUI handles the menu
    return false;
}

void GameManager::startGame(GameType type) {
    m_currentGame = type;
    m_state = GameState::PLAYING;
    
    switch (type) {
        case GameType::MINESWEEPER:
            initMinesweeper();
            break;
        case GameType::SUDOKU:
            initSudoku();
            break;
        case GameType::WORDLE:
            initWordle();
            break;
        default:
            break;
    }
    
    draw();
}

void GameManager::stopGame() {
    m_currentGame = GameType::NONE;
    m_state = GameState::MENU;
}

void GameManager::update() {
    if (m_currentGame == GameType::NONE) return;
    
    switch (m_currentGame) {
        case GameType::MINESWEEPER:
            updateMinesweeper();
            break;
        case GameType::SUDOKU:
            updateSudoku();
            break;
        case GameType::WORDLE:
            updateWordle();
            break;
        default:
            break;
    }
}

void GameManager::draw(LovyanGFX* target) {
    LovyanGFX* gfx = target ? target : (LovyanGFX*)&M5.Display;
    
    switch (m_currentGame) {
        case GameType::MINESWEEPER:
            drawMinesweeper(gfx);
            break;
        case GameType::SUDOKU:
            drawSudoku(gfx);
            break;
        case GameType::WORDLE:
            drawWordle(gfx);
            break;
        default:
            break;
    }
}

bool GameManager::handleTouch(int x, int y) {
    if (m_currentGame == GameType::NONE) {
        return false;
    }
    
    switch (m_currentGame) {
        case GameType::MINESWEEPER:
            return handleMinesweeperTouch(x, y);
        case GameType::SUDOKU:
            return handleSudokuTouch(x, y);
        case GameType::WORDLE:
            return handleWordleTouch(x, y);
        default:
            return false;
    }
}

// ============================================================================
// Minesweeper Implementation
// ============================================================================

void GameManager::initMinesweeper() {
    memset(&m_minesweeper, 0, sizeof(m_minesweeper));
    m_minesweeper.gameStarted = false;
    m_minesweeper.flagMode = false;
    m_minesweeper.flagCount = 0;
    ESP_LOGI(TAG, "Minesweeper initialized");
}

static void generateMines(GameManager::MinesweeperState& state, int firstRow, int firstCol) {
    memset(state.mines, false, sizeof(state.mines));
    memset(state.neighborCounts, 0, sizeof(state.neighborCounts));
    
    std::vector<std::pair<int,int>> positions;
    for (int i = 0; i < state.GRID_HEIGHT; i++) {
        for (int j = 0; j < state.GRID_WIDTH; j++) {
            // Exclude first click and adjacent cells
            if (abs(i - firstRow) <= 1 && abs(j - firstCol) <= 1) continue;
            positions.push_back({i, j});
        }
    }
    
    // Shuffle using Fisher-Yates
    for (int i = positions.size() - 1; i > 0; i--) {
        int j = esp_random() % (i + 1);
        std::swap(positions[i], positions[j]);
    }
    
    // Place mines
    for (int i = 0; i < state.MINE_COUNT && i < (int)positions.size(); i++) {
        state.mines[positions[i].first][positions[i].second] = true;
    }
    
    // Calculate neighbor counts
    for (int i = 0; i < state.GRID_HEIGHT; i++) {
        for (int j = 0; j < state.GRID_WIDTH; j++) {
            if (state.mines[i][j]) {
                state.neighborCounts[i][j] = -1;
                continue;
            }
            int count = 0;
            for (int di = -1; di <= 1; di++) {
                for (int dj = -1; dj <= 1; dj++) {
                    int ni = i + di, nj = j + dj;
                    if (ni >= 0 && ni < state.GRID_HEIGHT && 
                        nj >= 0 && nj < state.GRID_WIDTH && state.mines[ni][nj]) {
                        count++;
                    }
                }
            }
            state.neighborCounts[i][j] = count;
        }
    }
    state.gameStarted = true;
}

static void revealCell(GameManager::MinesweeperState& state, int row, int col) {
    if (row < 0 || row >= state.GRID_HEIGHT || col < 0 || col >= state.GRID_WIDTH) return;
    if (state.cellStates[row][col] != 0) return; // Already revealed or flagged
    if (state.gameLost || state.gameWon) return;
    
    state.cellStates[row][col] = 1; // Revealed
    state.revealedCount++;
    
    if (state.mines[row][col]) {
        state.gameLost = true;
        return;
    }
    
    // Auto-reveal neighbors if count is 0
    if (state.neighborCounts[row][col] == 0) {
        for (int di = -1; di <= 1; di++) {
            for (int dj = -1; dj <= 1; dj++) {
                revealCell(state, row + di, col + dj);
            }
        }
    }
    
    // Check win condition
    int totalCells = state.GRID_WIDTH * state.GRID_HEIGHT;
    if (state.revealedCount == totalCells - state.MINE_COUNT) {
        state.gameWon = true;
    }
}

void GameManager::updateMinesweeper() {
    // No continuous update needed
}

void GameManager::drawMinesweeperCell(LovyanGFX* target, int row, int col) {
    if (row < 0 || row >= m_minesweeper.GRID_HEIGHT || col < 0 || col >= m_minesweeper.GRID_WIDTH) {
        return;
    }

    LovyanGFX* gfx = target ? target : (LovyanGFX*)&M5.Display;
    const int CELL_SIZE = 54;
    const int GRID_START_X = 0;
    const int GRID_START_Y = SCREEN_HEIGHT - (m_minesweeper.GRID_HEIGHT * CELL_SIZE);

    int x = GRID_START_X + col * CELL_SIZE;
    int y = GRID_START_Y + row * CELL_SIZE;

    uint16_t cellBg = TFT_WHITE;
    bool isRevealed = m_minesweeper.cellStates[row][col] == 1;
    bool isFlagged = m_minesweeper.cellStates[row][col] == 2;
    bool showMine = isRevealed || (m_minesweeper.gameLost && m_minesweeper.mines[row][col]);

    if (isRevealed) {
        if (m_minesweeper.mines[row][col]) {
            cellBg = 0xF800; // Red
        } else {
            cellBg = GRAY_LIGHT;
        }
    } else if (isFlagged) {
        cellBg = TFT_YELLOW;
    }

    gfx->setTextDatum(textdatum_t::middle_center);
    gfx->fillRect(x + 1, y + 1, CELL_SIZE - 2, CELL_SIZE - 2, cellBg);
    gfx->drawRect(x, y, CELL_SIZE, CELL_SIZE, TFT_BLACK);

    if (showMine && m_minesweeper.mines[row][col]) {
        gfx->fillCircle(x + CELL_SIZE/2, y + CELL_SIZE/2, 10, TFT_BLACK);
        gfx->drawLine(x + CELL_SIZE/2 - 14, y + CELL_SIZE/2, x + CELL_SIZE/2 + 14, y + CELL_SIZE/2, TFT_BLACK);
        gfx->drawLine(x + CELL_SIZE/2, y + CELL_SIZE/2 - 14, x + CELL_SIZE/2, y + CELL_SIZE/2 + 14, TFT_BLACK);
    } else if (isRevealed && m_minesweeper.neighborCounts[row][col] > 0) {
        char num[2] = {(char)('0' + m_minesweeper.neighborCounts[row][col]), 0};
        gfx->setTextSize(1.8f);
        uint16_t numColor = TFT_BLUE;
        if (m_minesweeper.neighborCounts[row][col] == 2) numColor = TFT_DARKGREEN;
        else if (m_minesweeper.neighborCounts[row][col] == 3) numColor = TFT_RED;
        else if (m_minesweeper.neighborCounts[row][col] >= 4) numColor = TFT_MAROON;
        gfx->setTextColor(numColor, cellBg);
        gfx->drawString(num, x + CELL_SIZE/2, y + CELL_SIZE/2);
    } else if (isFlagged) {
        gfx->setTextSize(1.6f);
        gfx->setTextColor(TFT_RED, TFT_YELLOW);
        gfx->drawString("F", x + CELL_SIZE/2, y + CELL_SIZE/2);
    }

    gfx->setTextDatum(textdatum_t::top_left);
}

void GameManager::drawMinesweeperInfo(LovyanGFX* target) {
    LovyanGFX* gfx = target ? target : (LovyanGFX*)&M5.Display;
    int headerY = STATUS_BAR_HEIGHT;
    int infoY = headerY + HEADER_HEIGHT + 5;

    gfx->fillRect(0, infoY, SCREEN_WIDTH, 40, TFT_WHITE);
    gfx->setTextSize(1.3f);
    gfx->setTextColor(TFT_BLACK, TFT_WHITE);

    int minesLeft = m_minesweeper.MINE_COUNT - m_minesweeper.flagCount;
    char mineStr[32];
    snprintf(mineStr, sizeof(mineStr), "Mines: %d", minesLeft);
    gfx->setTextDatum(textdatum_t::middle_left);
    gfx->drawString(mineStr, 15, infoY + 20);

    const char* flagLabel = m_minesweeper.flagMode ? "[FLAG]" : "[DIG]";
    uint16_t flagBg = m_minesweeper.flagMode ? TFT_YELLOW : GRAY_LIGHT;
    drawButton(gfx, SCREEN_WIDTH - 100, infoY + 2, 90, 36, flagLabel, flagBg, TFT_BLACK);
    gfx->setTextDatum(textdatum_t::top_left);
}

void GameManager::drawMinesweeperGameOver(LovyanGFX* target) {
    if (!m_minesweeper.gameWon && !m_minesweeper.gameLost) {
        return;
    }

    LovyanGFX* gfx = target ? target : (LovyanGFX*)&M5.Display;
    int headerY = STATUS_BAR_HEIGHT;
    int infoY = headerY + HEADER_HEIGHT + 5;
    const int CELL_SIZE = 54;
    const int GRID_START_Y = SCREEN_HEIGHT - (m_minesweeper.GRID_HEIGHT * CELL_SIZE);
    int msgY = (infoY + 50 + GRID_START_Y) / 2;
    const char* msg = m_minesweeper.gameWon ? "YOU WIN!" : "GAME OVER";
    gfx->setTextSize(2.2f);
    gfx->setTextColor(TFT_BLACK, TFT_WHITE);
    gfx->setTextDatum(textdatum_t::middle_center);
    gfx->fillRect(SCREEN_WIDTH/2 - 100, msgY - 25, 200, 50, TFT_WHITE);
    gfx->drawRect(SCREEN_WIDTH/2 - 100, msgY - 25, 200, 50, TFT_BLACK);
    gfx->drawRect(SCREEN_WIDTH/2 - 99, msgY - 24, 198, 48, TFT_BLACK);
    gfx->drawString(msg, SCREEN_WIDTH/2, msgY);
    gfx->setTextDatum(textdatum_t::top_left);
}

void GameManager::drawMinesweeper(LovyanGFX* target) {
    LovyanGFX* gfx = target ? target : (LovyanGFX*)&M5.Display;
    gfx->fillScreen(TFT_WHITE);
    
    // Header area below status bar
    int headerY = STATUS_BAR_HEIGHT;
    gfx->fillRect(0, headerY, SCREEN_WIDTH, HEADER_HEIGHT, GRAY_LIGHT);
    gfx->drawLine(0, headerY + HEADER_HEIGHT - 1, SCREEN_WIDTH, headerY + HEADER_HEIGHT - 1, TFT_BLACK);
    
    // Back button
    gfx->setTextSize(1.4f);
    drawButton(gfx, 10, headerY + 5, 70, 40, "Back", TFT_WHITE, TFT_BLACK);
    
    // Title
    gfx->setTextColor(TFT_BLACK, GRAY_LIGHT);
    gfx->setTextDatum(textdatum_t::middle_center);
    gfx->setTextSize(1.5f);
    gfx->drawString("Minesweeper", SCREEN_WIDTH/2, headerY + HEADER_HEIGHT/2);
    
    // Restart button
    drawButton(gfx, SCREEN_WIDTH - 80, headerY + 5, 70, 40, "New", TFT_WHITE, TFT_BLACK);
    
    // Info bar: mines left and flag mode toggle
    int infoY = headerY + HEADER_HEIGHT + 5;
    gfx->setTextSize(1.3f);
    gfx->setTextColor(TFT_BLACK, TFT_WHITE);
    
    // Mines remaining
    int minesLeft = m_minesweeper.MINE_COUNT - m_minesweeper.flagCount;
    char mineStr[32];
    snprintf(mineStr, sizeof(mineStr), "Mines: %d", minesLeft);
    gfx->setTextDatum(textdatum_t::middle_left);
    gfx->drawString(mineStr, 15, infoY + 20);
    
    // Flag mode toggle button
    const char* flagLabel = m_minesweeper.flagMode ? "[FLAG]" : "[DIG]";
    uint16_t flagBg = m_minesweeper.flagMode ? TFT_YELLOW : GRAY_LIGHT;
    gfx->setTextSize(1.3f);
    drawButton(gfx, SCREEN_WIDTH - 100, infoY + 2, 90, 36, flagLabel, flagBg, TFT_BLACK);
    
    // Grid
    const int CELL_SIZE = 54;
    const int GRID_START_X = 0;
    const int GRID_START_Y = SCREEN_HEIGHT - (m_minesweeper.GRID_HEIGHT * CELL_SIZE);
    
    gfx->setTextDatum(textdatum_t::middle_center);
    
    for (int i = 0; i < m_minesweeper.GRID_HEIGHT; i++) {
        for (int j = 0; j < m_minesweeper.GRID_WIDTH; j++) {
            int x = GRID_START_X + j * CELL_SIZE;
            int y = GRID_START_Y + i * CELL_SIZE;
            
            uint16_t cellBg = TFT_WHITE;
            bool isRevealed = m_minesweeper.cellStates[i][j] == 1;
            bool isFlagged = m_minesweeper.cellStates[i][j] == 2;
            bool showMine = isRevealed || (m_minesweeper.gameLost && m_minesweeper.mines[i][j]);
            
            if (isRevealed) {
                if (m_minesweeper.mines[i][j]) {
                    // Hit a mine - red background
                    cellBg = 0xF800; // Red
                } else {
                    cellBg = GRAY_LIGHT;
                }
            } else if (isFlagged) {
                cellBg = TFT_YELLOW;
            }
            
            gfx->fillRect(x + 1, y + 1, CELL_SIZE - 2, CELL_SIZE - 2, cellBg);
            gfx->drawRect(x, y, CELL_SIZE, CELL_SIZE, TFT_BLACK);
            
            if (showMine && m_minesweeper.mines[i][j]) {
                // Draw mine
                gfx->fillCircle(x + CELL_SIZE/2, y + CELL_SIZE/2, 10, TFT_BLACK);
                // Spikes
                gfx->drawLine(x + CELL_SIZE/2 - 14, y + CELL_SIZE/2, x + CELL_SIZE/2 + 14, y + CELL_SIZE/2, TFT_BLACK);
                gfx->drawLine(x + CELL_SIZE/2, y + CELL_SIZE/2 - 14, x + CELL_SIZE/2, y + CELL_SIZE/2 + 14, TFT_BLACK);
            } else if (isRevealed && m_minesweeper.neighborCounts[i][j] > 0) {
                // Draw number
                char num[2] = {(char)('0' + m_minesweeper.neighborCounts[i][j]), 0};
                gfx->setTextSize(1.8f);
                // Color code numbers
                uint16_t numColor = TFT_BLUE;
                if (m_minesweeper.neighborCounts[i][j] == 2) numColor = TFT_DARKGREEN;
                else if (m_minesweeper.neighborCounts[i][j] == 3) numColor = TFT_RED;
                else if (m_minesweeper.neighborCounts[i][j] >= 4) numColor = TFT_MAROON;
                gfx->setTextColor(numColor, cellBg);
                gfx->drawString(num, x + CELL_SIZE/2, y + CELL_SIZE/2);
            } else if (isFlagged) {
                // Draw flag
                gfx->setTextSize(1.6f);
                gfx->setTextColor(TFT_RED, TFT_YELLOW);
                gfx->drawString("F", x + CELL_SIZE/2, y + CELL_SIZE/2);
            }
        }
    }
    
    // Game over message
    if (m_minesweeper.gameWon || m_minesweeper.gameLost) {
        int msgY = (infoY + 50 + GRID_START_Y) / 2;
        const char* msg = m_minesweeper.gameWon ? "YOU WIN!" : "GAME OVER";
        gfx->setTextSize(2.2f);
        gfx->setTextColor(TFT_BLACK, TFT_WHITE);
        gfx->fillRect(SCREEN_WIDTH/2 - 100, msgY - 25, 200, 50, TFT_WHITE);
        gfx->drawRect(SCREEN_WIDTH/2 - 100, msgY - 25, 200, 50, TFT_BLACK);
        gfx->drawRect(SCREEN_WIDTH/2 - 99, msgY - 24, 198, 48, TFT_BLACK);
        gfx->drawString(msg, SCREEN_WIDTH/2, msgY);
    }
    
    gfx->setTextDatum(textdatum_t::top_left);
    if (!target) M5.Display.display();
}

bool GameManager::handleMinesweeperTouch(int x, int y) {
    const int CELL_SIZE = 54;
    const int GRID_START_X = 0;
    const int GRID_START_Y = SCREEN_HEIGHT - (m_minesweeper.GRID_HEIGHT * CELL_SIZE);
    int headerY = STATUS_BAR_HEIGHT;
    int infoY = headerY + HEADER_HEIGHT + 5;

    // Back button
    if (x >= 10 && x <= 80 && y >= headerY + 5 && y <= headerY + 45) {
        stopGame();
        m_returnToMenu = true;
        return false;
    }

    // Restart button
    if (x >= SCREEN_WIDTH - 80 && x <= SCREEN_WIDTH - 10 && y >= headerY + 5 && y <= headerY + 45) {
        initMinesweeper();
        return true;
    }

    // Flag mode toggle
    if (x >= SCREEN_WIDTH - 100 && x <= SCREEN_WIDTH - 10 && y >= infoY + 2 && y <= infoY + 38) {
        m_minesweeper.flagMode = !m_minesweeper.flagMode;
        drawMinesweeperInfo(nullptr);
        M5.Display.display();
        return false;
    }

    // Grid clicks
    if (y >= GRID_START_Y && y < SCREEN_HEIGHT) {
        int col = (x - GRID_START_X) / CELL_SIZE;
        int row = (y - GRID_START_Y) / CELL_SIZE;

        if (row >= 0 && row < m_minesweeper.GRID_HEIGHT &&
            col >= 0 && col < m_minesweeper.GRID_WIDTH) {

            if (m_minesweeper.gameLost || m_minesweeper.gameWon) {
                initMinesweeper();
                return true;
            }

            int prevStates[GameManager::MinesweeperState::GRID_HEIGHT][GameManager::MinesweeperState::GRID_WIDTH];
            memcpy(prevStates, m_minesweeper.cellStates, sizeof(prevStates));
            bool prevLost = m_minesweeper.gameLost;
            bool prevWon = m_minesweeper.gameWon;
            int prevFlagCount = m_minesweeper.flagCount;
            bool prevFlagMode = m_minesweeper.flagMode;

            if (!m_minesweeper.gameStarted) {
                generateMines(m_minesweeper, row, col);
            }

            if (m_minesweeper.flagMode) {
                if (m_minesweeper.cellStates[row][col] == 0) {
                    m_minesweeper.cellStates[row][col] = 2; // Flag
                    m_minesweeper.flagCount++;
                } else if (m_minesweeper.cellStates[row][col] == 2) {
                    m_minesweeper.cellStates[row][col] = 0; // Unflag
                    m_minesweeper.flagCount--;
                }
            } else {
                if (m_minesweeper.cellStates[row][col] == 0) {
                    revealCell(m_minesweeper, row, col);
                }
            }

            bool showMinesChanged = (prevLost != m_minesweeper.gameLost);
            for (int i = 0; i < m_minesweeper.GRID_HEIGHT; i++) {
                for (int j = 0; j < m_minesweeper.GRID_WIDTH; j++) {
                    if (showMinesChanged || prevStates[i][j] != m_minesweeper.cellStates[i][j]) {
                        drawMinesweeperCell(nullptr, i, j);
                    }
                }
            }

            if (prevFlagCount != m_minesweeper.flagCount || prevFlagMode != m_minesweeper.flagMode) {
                drawMinesweeperInfo(nullptr);
            }

            if (prevLost != m_minesweeper.gameLost || prevWon != m_minesweeper.gameWon) {
                drawMinesweeperGameOver(nullptr);
            }

            M5.Display.display();
        }
    }

    return false;
}

// ============================================================================
// Sudoku Implementation
// ============================================================================

// Predefined puzzles for different difficulties
// Format: {puzzle, solution}
static const int SUDOKU_EASY_PUZZLES[][2][6][6] = {
    // Easy puzzle 1 - many given numbers
    {
        {
            {1, 2, 0, 4, 5, 0},
            {4, 5, 6, 1, 0, 3},
            {2, 0, 1, 6, 4, 5},
            {5, 6, 4, 0, 1, 2},
            {0, 1, 2, 5, 6, 4},
            {6, 4, 5, 2, 3, 0}
        },
        {
            {1, 2, 3, 4, 5, 6},
            {4, 5, 6, 1, 2, 3},
            {2, 3, 1, 6, 4, 5},
            {5, 6, 4, 3, 1, 2},
            {3, 1, 2, 5, 6, 4},
            {6, 4, 5, 2, 3, 1}
        }
    },
    // Easy puzzle 2
    {
        {
            {0, 2, 3, 4, 5, 6},
            {4, 5, 6, 1, 2, 0},
            {2, 3, 0, 6, 4, 5},
            {5, 6, 4, 3, 0, 2},
            {3, 1, 2, 0, 6, 4},
            {6, 0, 5, 2, 3, 1}
        },
        {
            {1, 2, 3, 4, 5, 6},
            {4, 5, 6, 1, 2, 3},
            {2, 3, 1, 6, 4, 5},
            {5, 6, 4, 3, 1, 2},
            {3, 1, 2, 5, 6, 4},
            {6, 4, 5, 2, 3, 1}
        }
    }
};

static const int SUDOKU_MEDIUM_PUZZLES[][2][6][6] = {
    // Medium puzzle 1
    {
        {
            {1, 0, 3, 0, 5, 6},
            {4, 5, 0, 1, 0, 3},
            {0, 3, 1, 6, 4, 0},
            {0, 6, 4, 0, 1, 2},
            {3, 0, 2, 5, 6, 0},
            {6, 4, 0, 2, 0, 1}
        },
        {
            {1, 2, 3, 4, 5, 6},
            {4, 5, 6, 1, 2, 3},
            {2, 3, 1, 6, 4, 5},
            {5, 6, 4, 3, 1, 2},
            {3, 1, 2, 5, 6, 4},
            {6, 4, 5, 2, 3, 1}
        }
    },
    // Medium puzzle 2
    {
        {
            {0, 2, 0, 4, 0, 6},
            {4, 0, 6, 0, 2, 0},
            {0, 3, 0, 6, 0, 5},
            {5, 0, 4, 0, 1, 0},
            {0, 1, 0, 5, 0, 4},
            {6, 0, 5, 0, 3, 0}
        },
        {
            {1, 2, 3, 4, 5, 6},
            {4, 5, 6, 1, 2, 3},
            {2, 3, 1, 6, 4, 5},
            {5, 6, 4, 3, 1, 2},
            {3, 1, 2, 5, 6, 4},
            {6, 4, 5, 2, 3, 1}
        }
    }
};

static const int SUDOKU_HARD_PUZZLES[][2][6][6] = {
    // Hard puzzle 1 - fewer given numbers
    {
        {
            {0, 0, 3, 0, 5, 0},
            {4, 0, 0, 1, 0, 0},
            {0, 3, 0, 0, 4, 0},
            {0, 6, 0, 0, 1, 0},
            {0, 0, 2, 0, 0, 4},
            {0, 4, 0, 2, 0, 0}
        },
        {
            {1, 2, 3, 4, 5, 6},
            {4, 5, 6, 1, 2, 3},
            {2, 3, 1, 6, 4, 5},
            {5, 6, 4, 3, 1, 2},
            {3, 1, 2, 5, 6, 4},
            {6, 4, 5, 2, 3, 1}
        }
    },
    // Hard puzzle 2
    {
        {
            {1, 0, 0, 0, 0, 6},
            {0, 5, 0, 0, 2, 0},
            {0, 0, 1, 6, 0, 0},
            {0, 0, 4, 3, 0, 0},
            {0, 1, 0, 0, 6, 0},
            {6, 0, 0, 0, 0, 1}
        },
        {
            {1, 2, 3, 4, 5, 6},
            {4, 5, 6, 1, 2, 3},
            {2, 3, 1, 6, 4, 5},
            {5, 6, 4, 3, 1, 2},
            {3, 1, 2, 5, 6, 4},
            {6, 4, 5, 2, 3, 1}
        }
    }
};

void GameManager::initSudoku() {
    // If difficulty not set, default to medium
    if (m_sudoku.difficulty == 0) {
        m_sudoku.difficulty = 2; // Medium
    }
    initSudokuWithDifficulty(m_sudoku.difficulty);
}

void GameManager::initSudokuWithDifficulty(int difficulty) {
    memset(&m_sudoku, 0, sizeof(m_sudoku));
    m_sudoku.difficulty = difficulty;
    m_sudoku.selectedRow = -1;
    m_sudoku.selectedCol = -1;
    
    const int (*puzzles)[2][6][6] = nullptr;
    int puzzleCount = 0;
    
    switch (difficulty) {
        case 1: // Easy
            puzzles = SUDOKU_EASY_PUZZLES;
            puzzleCount = sizeof(SUDOKU_EASY_PUZZLES) / sizeof(SUDOKU_EASY_PUZZLES[0]);
            break;
        case 2: // Medium
            puzzles = SUDOKU_MEDIUM_PUZZLES;
            puzzleCount = sizeof(SUDOKU_MEDIUM_PUZZLES) / sizeof(SUDOKU_MEDIUM_PUZZLES[0]);
            break;
        case 3: // Hard
            puzzles = SUDOKU_HARD_PUZZLES;
            puzzleCount = sizeof(SUDOKU_HARD_PUZZLES) / sizeof(SUDOKU_HARD_PUZZLES[0]);
            break;
        default:
            puzzles = SUDOKU_MEDIUM_PUZZLES;
            puzzleCount = sizeof(SUDOKU_MEDIUM_PUZZLES) / sizeof(SUDOKU_MEDIUM_PUZZLES[0]);
            break;
    }
    
    // Pick a random puzzle
    int idx = esp_random() % puzzleCount;
    
    for (int i = 0; i < 6; i++) {
        for (int j = 0; j < 6; j++) {
            m_sudoku.board[i][j] = puzzles[idx][0][i][j];
            m_sudoku.solution[i][j] = puzzles[idx][1][i][j];
            m_sudoku.readonly[i][j] = (puzzles[idx][0][i][j] != 0);
        }
    }
    
    ESP_LOGI(TAG, "Sudoku initialized with difficulty %d", difficulty);
}

void GameManager::updateSudoku() {
    // Check win condition
    bool complete = true;
    bool hasErrors = false;
    for (int i = 0; i < 6 && complete; i++) {
        for (int j = 0; j < 6 && complete; j++) {
            if (m_sudoku.board[i][j] == 0) {
                complete = false;
            } else if (m_sudoku.board[i][j] != m_sudoku.solution[i][j]) {
                hasErrors = true;
            }
        }
    }
    m_sudoku.gameWon = complete && !hasErrors;
}

void GameManager::drawSudokuCell(LovyanGFX* target, int row, int col) {
    if (row < 0 || row >= 6 || col < 0 || col >= 6) {
        return;
    }

    LovyanGFX* gfx = target ? target : (LovyanGFX*)&M5.Display;
    const int CELL_SIZE = 70;
    const int BLOCK_SPACING = 6;
    const int GRID_WIDTH_PX = 6 * CELL_SIZE + BLOCK_SPACING;
    const int GRID_X = (SCREEN_WIDTH - GRID_WIDTH_PX) / 2;
    const int GRID_Y = STATUS_BAR_HEIGHT + HEADER_HEIGHT + 20;

    int xOffset = (col >= 3) ? BLOCK_SPACING : 0;
    int yOffset = (row >= 2) ? ((row >= 4) ? BLOCK_SPACING * 2 : BLOCK_SPACING) : 0;
    int x = GRID_X + col * CELL_SIZE + xOffset;
    int y = GRID_Y + row * CELL_SIZE + yOffset;

    uint16_t bgColor = TFT_WHITE;
    if (m_sudoku.selectedRow == row && m_sudoku.selectedCol == col) {
        bgColor = 0x07FF; // Cyan for selected
    } else if (m_sudoku.readonly[row][col]) {
        bgColor = GRAY_LIGHT;
    }

    bool isError = !m_sudoku.readonly[row][col] &&
                   m_sudoku.board[row][col] != 0 &&
                   m_sudoku.board[row][col] != m_sudoku.solution[row][col];
    if (isError) {
        bgColor = 0xFD20; // Light red/orange for errors
    }

    gfx->fillRect(x, y, CELL_SIZE, CELL_SIZE, bgColor);
    gfx->drawRect(x, y, CELL_SIZE, CELL_SIZE, TFT_BLACK);

    if (m_sudoku.board[row][col] != 0) {
        uint16_t textColor = m_sudoku.readonly[row][col] ? TFT_BLACK : TFT_BLUE;
        if (isError) textColor = TFT_RED;
        gfx->setTextColor(textColor, bgColor);
        gfx->setTextDatum(textdatum_t::middle_center);
        gfx->setTextSize(2.2f);
        char num[2] = {(char)('0' + m_sudoku.board[row][col]), 0};
        gfx->drawString(num, x + CELL_SIZE/2, y + CELL_SIZE/2);
    }
    gfx->setTextDatum(textdatum_t::top_left);
}

void GameManager::drawSudokuDividers(LovyanGFX* target) {
    LovyanGFX* gfx = target ? target : (LovyanGFX*)&M5.Display;
    const int CELL_SIZE = 70;
    const int BLOCK_SPACING = 6;
    const int GRID_WIDTH_PX = 6 * CELL_SIZE + BLOCK_SPACING;
    const int GRID_HEIGHT_PX = 6 * CELL_SIZE + 2 * BLOCK_SPACING;
    const int GRID_X = (SCREEN_WIDTH - GRID_WIDTH_PX) / 2;
    const int GRID_Y = STATUS_BAR_HEIGHT + HEADER_HEIGHT + 20;

    for (int i = 0; i <= 6; i += 3) {
        int xOffset = (i >= 3) ? BLOCK_SPACING : 0;
        int x = GRID_X + i * CELL_SIZE + xOffset;
        if (i == 3) x -= BLOCK_SPACING/2;
        gfx->fillRect(x - 2, GRID_Y, 4, GRID_HEIGHT_PX, TFT_BLACK);
    }
    for (int i = 0; i <= 6; i += 2) {
        int yOffset = (i >= 2) ? ((i >= 4) ? BLOCK_SPACING * 2 : BLOCK_SPACING) : 0;
        int y = GRID_Y + i * CELL_SIZE + yOffset;
        if (i == 2 || i == 4) y -= BLOCK_SPACING/2;
        gfx->fillRect(GRID_X, y - 2, GRID_WIDTH_PX, 4, TFT_BLACK);
    }
}

void GameManager::drawSudoku(LovyanGFX* target) {
    LovyanGFX* gfx = target ? target : (LovyanGFX*)&M5.Display;
    gfx->fillScreen(TFT_WHITE);
    
    // Header area below status bar
    int headerY = STATUS_BAR_HEIGHT;
    gfx->fillRect(0, headerY, SCREEN_WIDTH, HEADER_HEIGHT, GRAY_LIGHT);
    gfx->drawLine(0, headerY + HEADER_HEIGHT - 1, SCREEN_WIDTH, headerY + HEADER_HEIGHT - 1, TFT_BLACK);
    
    // Back button
    gfx->setTextSize(1.4f);
    drawButton(gfx, 10, headerY + 5, 70, 40, "Back", TFT_WHITE, TFT_BLACK);
    
    // Title with difficulty
    const char* diffNames[] = {"", "Easy", "Medium", "Hard"};
    char title[32];
    snprintf(title, sizeof(title), "Sudoku - %s", diffNames[m_sudoku.difficulty]);
    gfx->setTextColor(TFT_BLACK, GRAY_LIGHT);
    gfx->setTextDatum(textdatum_t::middle_center);
    gfx->setTextSize(1.4f);
    gfx->drawString(title, SCREEN_WIDTH/2, headerY + HEADER_HEIGHT/2);
    
    // New button
    drawButton(gfx, SCREEN_WIDTH - 80, headerY + 5, 70, 40, "New", TFT_WHITE, TFT_BLACK);
    
    // Grid dimensions
    const int CELL_SIZE = 70;
    const int BLOCK_SPACING = 6;
    const int GRID_WIDTH_PX = 6 * CELL_SIZE + BLOCK_SPACING;
    const int GRID_HEIGHT_PX = 6 * CELL_SIZE + 2 * BLOCK_SPACING;
    const int GRID_X = (SCREEN_WIDTH - GRID_WIDTH_PX) / 2;
    const int GRID_Y = STATUS_BAR_HEIGHT + HEADER_HEIGHT + 20;
    
    gfx->setTextSize(2.2f);
    gfx->setTextDatum(textdatum_t::middle_center);
    
    for (int i = 0; i < 6; i++) {
        for (int j = 0; j < 6; j++) {
            // Calculate block spacing offsets
            int xOffset = (j >= 3) ? BLOCK_SPACING : 0;
            int yOffset = (i >= 2) ? ((i >= 4) ? BLOCK_SPACING * 2 : BLOCK_SPACING) : 0;
            
            int x = GRID_X + j * CELL_SIZE + xOffset;
            int y = GRID_Y + i * CELL_SIZE + yOffset;
            
            // Cell background
            uint16_t bgColor = TFT_WHITE;
            if (m_sudoku.selectedRow == i && m_sudoku.selectedCol == j) {
                bgColor = 0x07FF; // Cyan for selected
            } else if (m_sudoku.readonly[i][j]) {
                bgColor = GRAY_LIGHT;
            }
            
            // Check for errors (non-readonly cells that don't match solution)
            bool isError = !m_sudoku.readonly[i][j] && 
                           m_sudoku.board[i][j] != 0 && 
                           m_sudoku.board[i][j] != m_sudoku.solution[i][j];
            
            if (isError) {
                bgColor = 0xFD20; // Light red/orange for errors
            }
            
            gfx->fillRect(x, y, CELL_SIZE, CELL_SIZE, bgColor);
            gfx->drawRect(x, y, CELL_SIZE, CELL_SIZE, TFT_BLACK);
            
            // Number
            if (m_sudoku.board[i][j] != 0) {
                uint16_t textColor = m_sudoku.readonly[i][j] ? TFT_BLACK : TFT_BLUE;
                if (isError) textColor = TFT_RED;
                gfx->setTextColor(textColor, bgColor);
                char num[2] = {(char)('0' + m_sudoku.board[i][j]), 0};
                gfx->drawString(num, x + CELL_SIZE/2, y + CELL_SIZE/2);
            }
        }
    }
    
    // Draw 2x3 block dividers (thicker lines)
    for (int i = 0; i <= 6; i += 3) {
        int xOffset = (i >= 3) ? BLOCK_SPACING : 0;
        int x = GRID_X + i * CELL_SIZE + xOffset;
        if (i == 3) x -= BLOCK_SPACING/2;
        gfx->fillRect(x - 2, GRID_Y, 4, GRID_HEIGHT_PX, TFT_BLACK);
    }
    for (int i = 0; i <= 6; i += 2) {
        int yOffset = (i >= 2) ? ((i >= 4) ? BLOCK_SPACING * 2 : BLOCK_SPACING) : 0;
        int y = GRID_Y + i * CELL_SIZE + yOffset;
        if (i == 2 || i == 4) y -= BLOCK_SPACING/2;
        gfx->fillRect(GRID_X, y - 2, GRID_WIDTH_PX, 4, TFT_BLACK);
    }
    
    // Number input buttons (1-6)
    int btnY = GRID_Y + GRID_HEIGHT_PX + 25;
    int btnSize = 65;
    int btnGap = 10;
    int startX = (SCREEN_WIDTH - (6 * btnSize + 5 * btnGap)) / 2;
    
    gfx->setTextSize(2.0f);
    for (int i = 1; i <= 6; i++) {
        int x = startX + (i-1) * (btnSize + btnGap);
        drawButton(gfx, x, btnY, btnSize, btnSize, nullptr, GRAY_LIGHT, TFT_BLACK);
        char num[2] = {(char)('0' + i), 0};
        gfx->setTextColor(TFT_BLACK, GRAY_LIGHT);
        gfx->drawString(num, x + btnSize/2, btnY + btnSize/2);
    }
    
    // Clear and Hint buttons row
    int actionY = btnY + btnSize + 15;
    drawButton(gfx, startX, actionY, 100, 45, "Clear", TFT_WHITE, TFT_BLACK);
    
    // Difficulty buttons
    int diffY = actionY + 55;
    gfx->setTextSize(1.3f);
    gfx->setTextColor(TFT_BLACK, TFT_WHITE);
    gfx->setTextDatum(textdatum_t::middle_center);
    gfx->drawString("Difficulty:", SCREEN_WIDTH/2, diffY);
    
    diffY += 25;
    int diffBtnW = 80;
    int diffStartX = (SCREEN_WIDTH - (3 * diffBtnW + 2 * 10)) / 2;
    
    for (int d = 1; d <= 3; d++) {
        int x = diffStartX + (d-1) * (diffBtnW + 10);
        uint16_t bg = (m_sudoku.difficulty == d) ? TFT_DARKGREY : GRAY_LIGHT;
        uint16_t fg = (m_sudoku.difficulty == d) ? TFT_WHITE : TFT_BLACK;
        drawButton(gfx, x, diffY, diffBtnW, 40, diffNames[d], bg, fg);
    }
    
    // Win message
    if (m_sudoku.gameWon) {
        int msgY = GRID_Y + GRID_HEIGHT_PX / 2;
        gfx->setTextSize(2.0f);
        gfx->setTextColor(TFT_BLACK, TFT_WHITE);
        gfx->fillRect(SCREEN_WIDTH/2 - 100, msgY - 25, 200, 50, TFT_WHITE);
        gfx->drawRect(SCREEN_WIDTH/2 - 100, msgY - 25, 200, 50, TFT_BLACK);
        gfx->drawRect(SCREEN_WIDTH/2 - 99, msgY - 24, 198, 48, TFT_BLACK);
        gfx->drawString("COMPLETE!", SCREEN_WIDTH/2, msgY);
    }
    
    gfx->setTextDatum(textdatum_t::top_left);
    if (!target) M5.Display.display();
}

bool GameManager::handleSudokuTouch(int x, int y) {
    const int CELL_SIZE = 70;
    const int BLOCK_SPACING = 6;
    const int GRID_WIDTH_PX = 6 * CELL_SIZE + BLOCK_SPACING;
    const int GRID_HEIGHT_PX = 6 * CELL_SIZE + 2 * BLOCK_SPACING;
    const int GRID_X = (SCREEN_WIDTH - GRID_WIDTH_PX) / 2;
    const int GRID_Y = STATUS_BAR_HEIGHT + HEADER_HEIGHT + 20;
    int headerY = STATUS_BAR_HEIGHT;
    int prevSelectedRow = m_sudoku.selectedRow;
    int prevSelectedCol = m_sudoku.selectedCol;
    bool prevGameWon = m_sudoku.gameWon;
    
    // Back button
    if (x >= 10 && x <= 80 && y >= headerY + 5 && y <= headerY + 45) {
        stopGame();
        m_returnToMenu = true;
        return false;
    }
    
    // New button
    if (x >= SCREEN_WIDTH - 80 && x <= SCREEN_WIDTH - 10 && y >= headerY + 5 && y <= headerY + 45) {
        initSudokuWithDifficulty(m_sudoku.difficulty);
        return true;
    }
    
    // Number buttons
    int btnY = GRID_Y + GRID_HEIGHT_PX + 25;
    int btnSize = 65;
    int btnGap = 10;
    int startX = (SCREEN_WIDTH - (6 * btnSize + 5 * btnGap)) / 2;
    
    if (y >= btnY && y <= btnY + btnSize && m_sudoku.selectedRow >= 0) {
        for (int i = 1; i <= 6; i++) {
            int bx = startX + (i-1) * (btnSize + btnGap);
            if (x >= bx && x <= bx + btnSize) {
                if (!m_sudoku.readonly[m_sudoku.selectedRow][m_sudoku.selectedCol]) {
                    m_sudoku.board[m_sudoku.selectedRow][m_sudoku.selectedCol] = i;
                    updateSudoku();
                }
                if (prevGameWon != m_sudoku.gameWon) {
                    return true;
                }
                drawSudokuCell(nullptr, m_sudoku.selectedRow, m_sudoku.selectedCol);
                drawSudokuDividers(nullptr);
                M5.Display.display();
                return false;
            }
        }
    }
    
    // Clear button
    int actionY = btnY + btnSize + 15;
    if (y >= actionY && y <= actionY + 45 && x >= startX && x <= startX + 100) {
        if (m_sudoku.selectedRow >= 0 && !m_sudoku.readonly[m_sudoku.selectedRow][m_sudoku.selectedCol]) {
            m_sudoku.board[m_sudoku.selectedRow][m_sudoku.selectedCol] = 0;
            updateSudoku();
            if (prevGameWon != m_sudoku.gameWon) {
                return true;
            }
            drawSudokuCell(nullptr, m_sudoku.selectedRow, m_sudoku.selectedCol);
            drawSudokuDividers(nullptr);
            M5.Display.display();
        }
        return false;
    }
    
    // Difficulty buttons
    int diffY = actionY + 55 + 25;
    int diffBtnW = 80;
    int diffStartX = (SCREEN_WIDTH - (3 * diffBtnW + 2 * 10)) / 2;
    
    if (y >= diffY && y <= diffY + 40) {
        for (int d = 1; d <= 3; d++) {
            int dx = diffStartX + (d-1) * (diffBtnW + 10);
            if (x >= dx && x <= dx + diffBtnW) {
                initSudokuWithDifficulty(d);
                return true;
            }
        }
    }
    
    // Grid clicks - find which cell was clicked
    if (x >= GRID_X && x < GRID_X + GRID_WIDTH_PX && 
        y >= GRID_Y && y < GRID_Y + GRID_HEIGHT_PX) {
        
        int relX = x - GRID_X;
        int relY = y - GRID_Y;
        
        // Calculate column considering block spacing
        int col;
        if (relX < 3 * CELL_SIZE) {
            col = relX / CELL_SIZE;
        } else {
            col = (relX - BLOCK_SPACING) / CELL_SIZE;
        }
        if (col >= 6) col = 5;
        if (col < 0) col = 0;
        
        // Calculate row considering block spacing
        int row;
        if (relY < 2 * CELL_SIZE) {
            row = relY / CELL_SIZE;
        } else if (relY < 4 * CELL_SIZE + BLOCK_SPACING) {
            row = (relY - BLOCK_SPACING) / CELL_SIZE;
        } else {
            row = (relY - 2 * BLOCK_SPACING) / CELL_SIZE;
        }
        if (row >= 6) row = 5;
        if (row < 0) row = 0;
        
        // Select the cell (even readonly ones, just can't modify them)
        m_sudoku.selectedRow = row;
        m_sudoku.selectedCol = col;
        if (prevSelectedRow != m_sudoku.selectedRow || prevSelectedCol != m_sudoku.selectedCol) {
            if (prevSelectedRow >= 0 && prevSelectedCol >= 0) {
                drawSudokuCell(nullptr, prevSelectedRow, prevSelectedCol);
            }
            drawSudokuCell(nullptr, m_sudoku.selectedRow, m_sudoku.selectedCol);
            drawSudokuDividers(nullptr);
            M5.Display.display();
        }
        return false;
    }

    return false;
}

// ============================================================================
// Wordle Implementation
// ============================================================================

// Simple word list for Wordle
static const char* WORDLE_WORDS[] = {
    "PAPER", "BRAIN", "CRANE", "FLAME", "GRAPE", "HOUSE", "JUICE", "KNIFE",
    "LEMON", "MANGO", "NIGHT", "OCEAN", "PIANO", "QUEEN", "RIVER", "STONE",
    "TIGER", "UNION", "VIDEO", "WATER", "XENON", "YACHT", "ZEBRA", "APPLE",
    "BEACH", "CHAIR", "DREAM", "EARTH", "FLOOR", "GHOST", "HEART", "IMAGE",
    "AUDIO", "BLOOM", "BRAVE", "CANDY", "DANCE", "EAGLE", "FAIRY", "GIANT"
};
static const int WORDLE_WORD_COUNT = sizeof(WORDLE_WORDS) / sizeof(WORDLE_WORDS[0]);

void GameManager::initWordle() {
    memset(&m_wordle, 0, sizeof(m_wordle));
    
    // Pick random word
    int idx = esp_random() % WORDLE_WORD_COUNT;
    strncpy(m_wordle.answer, WORDLE_WORDS[idx], 5);
    m_wordle.answer[5] = '\0';
    
    ESP_LOGI(TAG, "Wordle initialized (answer: %s)", m_wordle.answer);
}

void GameManager::updateWordle() {
    // No continuous update needed
}

void GameManager::drawWordleTile(LovyanGFX* target, int row, int col) {
    if (row < 0 || row >= 6 || col < 0 || col >= 5) {
        return;
    }

    LovyanGFX* gfx = target ? target : (LovyanGFX*)&M5.Display;
    const int CELL_SIZE = 60;
    const int GAP = 6;
    const int GRID_WIDTH = 5 * CELL_SIZE + 4 * GAP;
    const int GRID_X = (SCREEN_WIDTH - GRID_WIDTH) / 2;
    const int GRID_Y = STATUS_BAR_HEIGHT + HEADER_HEIGHT + 15;

    int x = GRID_X + col * (CELL_SIZE + GAP);
    int y = GRID_Y + row * (CELL_SIZE + GAP);

    // Clear any prior highlight border
    gfx->fillRect(x - 1, y - 1, CELL_SIZE + 2, CELL_SIZE + 2, TFT_WHITE);

    uint16_t bgColor = TFT_WHITE;
    char state = m_wordle.states[row][col];
    if (state == 'g') bgColor = TFT_GREEN;
    else if (state == 'y') bgColor = TFT_YELLOW;
    else if (state == 'x') bgColor = GRAY_MEDIUM;

    if (row == m_wordle.currentRow && state == 0) {
        gfx->drawRect(x - 1, y - 1, CELL_SIZE + 2, CELL_SIZE + 2, TFT_BLUE);
    }

    gfx->fillRect(x, y, CELL_SIZE, CELL_SIZE, bgColor);
    gfx->drawRect(x, y, CELL_SIZE, CELL_SIZE, TFT_BLACK);

    if (m_wordle.guesses[row][col] != 0) {
        gfx->setTextSize(2.2f);
        gfx->setTextDatum(textdatum_t::middle_center);
        gfx->setTextColor(TFT_BLACK, bgColor);
        char letter[2] = {m_wordle.guesses[row][col], 0};
        gfx->drawString(letter, x + CELL_SIZE/2, y + CELL_SIZE/2);
    }
    gfx->setTextDatum(textdatum_t::top_left);
}

void GameManager::drawWordleKey(LovyanGFX* target, char letter) {
    LovyanGFX* gfx = target ? target : (LovyanGFX*)&M5.Display;
    const int CELL_SIZE = 60;
    const int GAP = 6;
    const int GRID_Y = STATUS_BAR_HEIGHT + HEADER_HEIGHT + 15;
    const int keyW = 46;
    const int keyH = 50;
    const int keyGap = 4;
    const int keyY = GRID_Y + 6 * (CELL_SIZE + GAP) + 15;
    const char* rows[] = {"QWERTYUIOP", "ASDFGHJKL", "ZXCVBNM"};

    for (int r = 0; r < 3; r++) {
        int rowLen = strlen(rows[r]);
        int rowWidth = rowLen * keyW + (rowLen - 1) * keyGap;
        int rowX = (SCREEN_WIDTH - rowWidth) / 2;

        for (int c = 0; c < rowLen; c++) {
            if (rows[r][c] != letter) {
                continue;
            }
            int x = rowX + c * (keyW + keyGap);
            int y = keyY + r * (keyH + keyGap);

            char letterState = m_wordle.usedLetters[letter - 'A'];
            uint16_t bgColor = GRAY_LIGHT;
            if (letterState == 'g') bgColor = TFT_GREEN;
            else if (letterState == 'y') bgColor = TFT_YELLOW;
            else if (letterState == 'x') bgColor = GRAY_DARK;

            gfx->fillRect(x, y, keyW, keyH, bgColor);
            gfx->drawRect(x, y, keyW, keyH, TFT_BLACK);

            uint16_t textColor = (letterState == 'x') ? TFT_WHITE : TFT_BLACK;
            gfx->setTextSize(1.5f);
            gfx->setTextDatum(textdatum_t::middle_center);
            gfx->setTextColor(textColor, bgColor);
            char str[2] = {letter, 0};
            gfx->drawString(str, x + keyW/2, y + keyH/2);
            gfx->setTextDatum(textdatum_t::top_left);
            return;
        }
    }
}

void GameManager::drawWordleResult(LovyanGFX* target) {
    if (!m_wordle.gameWon && !m_wordle.gameLost) {
        return;
    }

    LovyanGFX* gfx = target ? target : (LovyanGFX*)&M5.Display;
    const int CELL_SIZE = 60;
    const int GAP = 6;
    const int GRID_Y = STATUS_BAR_HEIGHT + HEADER_HEIGHT + 15;
    int msgY = GRID_Y + 3 * (CELL_SIZE + GAP);

    if (m_wordle.gameWon) {
        gfx->setTextDatum(textdatum_t::middle_center);
        gfx->setTextSize(2.0f);
        gfx->setTextColor(TFT_BLACK, TFT_WHITE);
        gfx->fillRect(SCREEN_WIDTH/2 - 100, msgY - 25, 200, 50, TFT_WHITE);
        gfx->drawRect(SCREEN_WIDTH/2 - 100, msgY - 25, 200, 50, TFT_BLACK);
        gfx->drawString("YOU WIN!", SCREEN_WIDTH/2, msgY);
    } else if (m_wordle.gameLost) {
        gfx->setTextDatum(textdatum_t::middle_center);
        gfx->setTextSize(1.6f);
        gfx->setTextColor(TFT_BLACK, TFT_WHITE);
        char msg[32];
        snprintf(msg, sizeof(msg), "Answer: %s", m_wordle.answer);
        gfx->fillRect(SCREEN_WIDTH/2 - 110, msgY - 25, 220, 50, TFT_WHITE);
        gfx->drawRect(SCREEN_WIDTH/2 - 110, msgY - 25, 220, 50, TFT_BLACK);
        gfx->drawString(msg, SCREEN_WIDTH/2, msgY);
    }
    gfx->setTextDatum(textdatum_t::top_left);
}

void GameManager::drawWordle(LovyanGFX* target) {
    LovyanGFX* gfx = target ? target : (LovyanGFX*)&M5.Display;
    gfx->fillScreen(TFT_WHITE);
    
    // Header area below status bar
    int headerY = STATUS_BAR_HEIGHT;
    gfx->fillRect(0, headerY, SCREEN_WIDTH, HEADER_HEIGHT, GRAY_LIGHT);
    gfx->drawLine(0, headerY + HEADER_HEIGHT - 1, SCREEN_WIDTH, headerY + HEADER_HEIGHT - 1, TFT_BLACK);
    
    // Back button
    gfx->setTextSize(1.4f);
    drawButton(gfx, 10, headerY + 5, 70, 40, "Back", TFT_WHITE, TFT_BLACK);
    
    // Title
    gfx->setTextColor(TFT_BLACK, GRAY_LIGHT);
    gfx->setTextDatum(textdatum_t::middle_center);
    gfx->setTextSize(1.5f);
    gfx->drawString("Wordle", SCREEN_WIDTH/2, headerY + HEADER_HEIGHT/2);
    
    // New button
    drawButton(gfx, SCREEN_WIDTH - 80, headerY + 5, 70, 40, "New", TFT_WHITE, TFT_BLACK);
    
    // Guess grid
    const int CELL_SIZE = 60;
    const int GAP = 6;
    const int GRID_WIDTH = 5 * CELL_SIZE + 4 * GAP;
    const int GRID_X = (SCREEN_WIDTH - GRID_WIDTH) / 2;
    const int GRID_Y = STATUS_BAR_HEIGHT + HEADER_HEIGHT + 15;
    
    gfx->setTextSize(2.2f);
    gfx->setTextDatum(textdatum_t::middle_center);
    
    // Draw guesses grid
    for (int row = 0; row < 6; row++) {
        for (int col = 0; col < 5; col++) {
            int x = GRID_X + col * (CELL_SIZE + GAP);
            int y = GRID_Y + row * (CELL_SIZE + GAP);
            
            uint16_t bgColor = TFT_WHITE;
            char state = m_wordle.states[row][col];
            if (state == 'g') bgColor = TFT_GREEN;
            else if (state == 'y') bgColor = TFT_YELLOW;
            else if (state == 'x') bgColor = GRAY_MEDIUM;
            
            // Highlight current row
            if (row == m_wordle.currentRow && state == 0) {
                gfx->drawRect(x - 1, y - 1, CELL_SIZE + 2, CELL_SIZE + 2, TFT_BLUE);
            }
            
            gfx->fillRect(x, y, CELL_SIZE, CELL_SIZE, bgColor);
            gfx->drawRect(x, y, CELL_SIZE, CELL_SIZE, TFT_BLACK);
            
            if (m_wordle.guesses[row][col] != 0) {
                gfx->setTextColor(TFT_BLACK, bgColor);
                char letter[2] = {m_wordle.guesses[row][col], 0};
                gfx->drawString(letter, x + CELL_SIZE/2, y + CELL_SIZE/2);
            }
        }
    }
    
    // Draw keyboard
    const char* rows[] = {"QWERTYUIOP", "ASDFGHJKL", "ZXCVBNM"};
    int keyW = 46;
    int keyH = 50;
    int keyGap = 4;
    int keyY = GRID_Y + 6 * (CELL_SIZE + GAP) + 15;
    
    gfx->setTextSize(1.5f);
    
    for (int r = 0; r < 3; r++) {
        int rowLen = strlen(rows[r]);
        int rowWidth = rowLen * keyW + (rowLen - 1) * keyGap;
        int rowX = (SCREEN_WIDTH - rowWidth) / 2;
        
        for (int c = 0; c < rowLen; c++) {
            int x = rowX + c * (keyW + keyGap);
            int y = keyY + r * (keyH + keyGap);
            
            char letter = rows[r][c];
            char letterState = m_wordle.usedLetters[letter - 'A'];
            
            uint16_t bgColor = GRAY_LIGHT;
            if (letterState == 'g') bgColor = TFT_GREEN;
            else if (letterState == 'y') bgColor = TFT_YELLOW;
            else if (letterState == 'x') bgColor = GRAY_DARK;
            
            gfx->fillRect(x, y, keyW, keyH, bgColor);
            gfx->drawRect(x, y, keyW, keyH, TFT_BLACK);
            
            uint16_t textColor = (letterState == 'x') ? TFT_WHITE : TFT_BLACK;
            gfx->setTextColor(textColor, bgColor);
            char str[2] = {letter, 0};
            gfx->drawString(str, x + keyW/2, y + keyH/2);
        }
    }
    
    // Special keys: Enter and Backspace
    int specialY = keyY + 3 * (keyH + keyGap);
    gfx->setTextSize(1.3f);
    drawButton(gfx, SCREEN_WIDTH/2 - 140, specialY, 100, 45, "ENTER", GRAY_LIGHT, TFT_BLACK);
    drawButton(gfx, SCREEN_WIDTH/2 + 40, specialY, 100, 45, "DEL", GRAY_LIGHT, TFT_BLACK);
    
    // Win/Lose message
    if (m_wordle.gameWon) {
        int msgY = GRID_Y + 3 * (CELL_SIZE + GAP);
        gfx->setTextSize(2.0f);
        gfx->setTextColor(TFT_BLACK, TFT_WHITE);
        gfx->fillRect(SCREEN_WIDTH/2 - 100, msgY - 25, 200, 50, TFT_WHITE);
        gfx->drawRect(SCREEN_WIDTH/2 - 100, msgY - 25, 200, 50, TFT_BLACK);
        gfx->drawString("YOU WIN!", SCREEN_WIDTH/2, msgY);
    } else if (m_wordle.gameLost) {
        int msgY = GRID_Y + 3 * (CELL_SIZE + GAP);
        gfx->setTextSize(1.6f);
        gfx->setTextColor(TFT_BLACK, TFT_WHITE);
        char msg[32];
        snprintf(msg, sizeof(msg), "Answer: %s", m_wordle.answer);
        gfx->fillRect(SCREEN_WIDTH/2 - 110, msgY - 25, 220, 50, TFT_WHITE);
        gfx->drawRect(SCREEN_WIDTH/2 - 110, msgY - 25, 220, 50, TFT_BLACK);
        gfx->drawString(msg, SCREEN_WIDTH/2, msgY);
    }
    
    gfx->setTextDatum(textdatum_t::top_left);
    if (!target) M5.Display.display();
}

bool GameManager::handleWordleTouch(int x, int y) {
    const int CELL_SIZE = 60;
    const int GAP = 6;
    const int GRID_Y = STATUS_BAR_HEIGHT + HEADER_HEIGHT + 15;
    int headerY = STATUS_BAR_HEIGHT;
    
    // Back button
    if (x >= 10 && x <= 80 && y >= headerY + 5 && y <= headerY + 45) {
        stopGame();
        m_returnToMenu = true;
        return false;
    }
    
    // New button
    if (x >= SCREEN_WIDTH - 80 && x <= SCREEN_WIDTH - 10 && y >= headerY + 5 && y <= headerY + 45) {
        initWordle();
        return true;
    }
    
    if (m_wordle.gameWon || m_wordle.gameLost) {
        // Tap anywhere to restart
        initWordle();
        return true;
    }
    
    // Keyboard handling
    const char* rows[] = {"QWERTYUIOP", "ASDFGHJKL", "ZXCVBNM"};
    int keyW = 46;
    int keyH = 50;
    int keyGap = 4;
    int keyY = GRID_Y + 6 * (CELL_SIZE + GAP) + 15;
    
    for (int r = 0; r < 3; r++) {
        int rowLen = strlen(rows[r]);
        int rowWidth = rowLen * keyW + (rowLen - 1) * keyGap;
        int rowX = (SCREEN_WIDTH - rowWidth) / 2;
        
        for (int c = 0; c < rowLen; c++) {
            int kx = rowX + c * (keyW + keyGap);
            int ky = keyY + r * (keyH + keyGap);
            
            if (x >= kx && x <= kx + keyW && y >= ky && y <= ky + keyH) {
                // Letter pressed
                if (m_wordle.currentCol < 5) {
                    int row = m_wordle.currentRow;
                    int col = m_wordle.currentCol;
                    m_wordle.guesses[row][col] = rows[r][c];
                    m_wordle.currentCol++;
                    drawWordleTile(nullptr, row, col);
                    M5.Display.display();
                }
                return false;
            }
        }
    }
    
    // Special keys
    int specialY = keyY + 3 * (keyH + keyGap);
    
    // Enter button
    if (x >= SCREEN_WIDTH/2 - 140 && x <= SCREEN_WIDTH/2 - 40 && 
        y >= specialY && y <= specialY + 45) {
        if (m_wordle.currentCol == 5) {
            int row = m_wordle.currentRow;
            // Check the guess
            char guess[6];
            for (int i = 0; i < 5; i++) {
                guess[i] = m_wordle.guesses[row][i];
            }
            guess[5] = '\0';
            
            // First pass: mark correct positions
            bool correct = true;
            char answerCopy[6];
            strncpy(answerCopy, m_wordle.answer, 6);
            
            for (int i = 0; i < 5; i++) {
                char c = guess[i];
                if (c == m_wordle.answer[i]) {
                    m_wordle.states[row][i] = 'g';
                    m_wordle.usedLetters[c - 'A'] = 'g';
                    answerCopy[i] = '*'; // Mark as used
                } else {
                    correct = false;
                    m_wordle.states[row][i] = 'x'; // Default to gray
                }
            }
            
            // Second pass: mark yellow (wrong position but in word)
            for (int i = 0; i < 5; i++) {
                if (m_wordle.states[row][i] != 'g') {
                    char c = guess[i];
                    for (int j = 0; j < 5; j++) {
                        if (answerCopy[j] == c) {
                            m_wordle.states[row][i] = 'y';
                            answerCopy[j] = '*'; // Mark as used
                            if (m_wordle.usedLetters[c - 'A'] != 'g') {
                                m_wordle.usedLetters[c - 'A'] = 'y';
                            }
                            break;
                        }
                    }
                    // If still gray, mark letter as used/gray
                    if (m_wordle.states[row][i] == 'x') {
                        if (m_wordle.usedLetters[c - 'A'] == 0) {
                            m_wordle.usedLetters[c - 'A'] = 'x';
                        }
                    }
                }
            }
            
            if (correct) {
                m_wordle.gameWon = true;
            } else if (m_wordle.currentRow >= 5) {
                m_wordle.gameLost = true;
            } else {
                m_wordle.currentRow++;
                m_wordle.currentCol = 0;
            }
            for (int i = 0; i < 5; i++) {
                drawWordleTile(nullptr, row, i);
            }

            bool usedLetters[26] = {};
            for (int i = 0; i < 5; i++) {
                char c = guess[i];
                if (c >= 'A' && c <= 'Z' && !usedLetters[c - 'A']) {
                    drawWordleKey(nullptr, c);
                    usedLetters[c - 'A'] = true;
                }
            }

            if (!m_wordle.gameWon && !m_wordle.gameLost) {
                for (int i = 0; i < 5; i++) {
                    drawWordleTile(nullptr, m_wordle.currentRow, i);
                }
            } else {
                drawWordleResult(nullptr);
            }

            M5.Display.display();
        }
        return false;
    }
    
    // Delete button
    if (x >= SCREEN_WIDTH/2 + 40 && x <= SCREEN_WIDTH/2 + 140 &&
        y >= specialY && y <= specialY + 45) {
        if (m_wordle.currentCol > 0) {
            m_wordle.currentCol--;
            m_wordle.guesses[m_wordle.currentRow][m_wordle.currentCol] = 0;
            drawWordleTile(nullptr, m_wordle.currentRow, m_wordle.currentCol);
            M5.Display.display();
        }
        return false;
    }

    return false;
}
