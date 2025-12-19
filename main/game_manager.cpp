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

void GameManager::drawButton(int x, int y, int w, int h, const char* label,
                             uint16_t bgColor, uint16_t textColor) {
    M5.Display.fillRect(x, y, w, h, bgColor);
    M5.Display.drawRect(x, y, w, h, TFT_BLACK);
    M5.Display.setTextColor(textColor, bgColor);
    M5.Display.setTextDatum(textdatum_t::middle_center);
    M5.Display.drawString(label, x + w/2, y + h/2);
    M5.Display.setTextDatum(textdatum_t::top_left);
}

void GameManager::drawHeader(const char* title) {
    M5.Display.fillRect(0, STATUS_BAR_HEIGHT, SCREEN_WIDTH, 60, GRAY_LIGHT);
    M5.Display.drawLine(0, STATUS_BAR_HEIGHT + 59, SCREEN_WIDTH, STATUS_BAR_HEIGHT + 59, TFT_BLACK);
    M5.Display.setTextColor(TFT_BLACK, GRAY_LIGHT);
    M5.Display.setTextSize(2.5f);
    M5.Display.setTextDatum(textdatum_t::middle_center);
    M5.Display.drawString(title, SCREEN_WIDTH/2, STATUS_BAR_HEIGHT + 30);
    M5.Display.setTextDatum(textdatum_t::top_left);
}

void GameManager::drawGamesMenu() {
    M5.Display.fillScreen(TFT_WHITE);
    
    // Header
    drawHeader("Games");
    
    M5.Display.setTextSize(1.8f);
    
    // Game buttons - centered vertically
    int btnWidth = 400;
    int btnHeight = 100;
    int btnGap = 30;
    int startY = STATUS_BAR_HEIGHT + 100;
    int btnX = (SCREEN_WIDTH - btnWidth) / 2;
    
    // Minesweeper
    drawButton(btnX, startY, btnWidth, btnHeight, "Minesweeper", GRAY_LIGHT, TFT_BLACK);
    M5.Display.setTextSize(1.2f);
    M5.Display.setTextColor(GRAY_DARK, GRAY_LIGHT);
    M5.Display.setTextDatum(textdatum_t::middle_center);
    M5.Display.drawString("Classic mine-sweeping puzzle", btnX + btnWidth/2, startY + btnHeight - 20);
    
    // Sudoku
    startY += btnHeight + btnGap;
    drawButton(btnX, startY, btnWidth, btnHeight, "Sudoku", GRAY_LIGHT, TFT_BLACK);
    M5.Display.setTextSize(1.2f);
    M5.Display.setTextColor(GRAY_DARK, GRAY_LIGHT);
    M5.Display.drawString("6x6 number puzzle", btnX + btnWidth/2, startY + btnHeight - 20);
    
    // Wordle
    startY += btnHeight + btnGap;
    drawButton(btnX, startY, btnWidth, btnHeight, "Wordle", GRAY_LIGHT, TFT_BLACK);
    M5.Display.setTextSize(1.2f);
    M5.Display.setTextColor(GRAY_DARK, GRAY_LIGHT);
    M5.Display.drawString("5-letter word guessing game", btnX + btnWidth/2, startY + btnHeight - 20);
    
    M5.Display.setTextDatum(textdatum_t::top_left);
    
    // Back button
    drawButton(20, SCREEN_HEIGHT - 70, 150, 50, "Back", TFT_WHITE, TFT_BLACK);
    
    M5.Display.display();
}

bool GameManager::handleMenuTouch(int x, int y) {
    int btnWidth = 400;
    int btnHeight = 100;
    int btnGap = 30;
    int startY = STATUS_BAR_HEIGHT + 100;
    int btnX = (SCREEN_WIDTH - btnWidth) / 2;
    
    // Minesweeper
    if (x >= btnX && x <= btnX + btnWidth && y >= startY && y <= startY + btnHeight) {
        startGame(GameType::MINESWEEPER);
        return true;
    }
    
    // Sudoku
    startY += btnHeight + btnGap;
    if (x >= btnX && x <= btnX + btnWidth && y >= startY && y <= startY + btnHeight) {
        startGame(GameType::SUDOKU);
        return true;
    }
    
    // Wordle
    startY += btnHeight + btnGap;
    if (x >= btnX && x <= btnX + btnWidth && y >= startY && y <= startY + btnHeight) {
        startGame(GameType::WORDLE);
        return true;
    }
    
    // Back button
    if (x >= 20 && x <= 170 && y >= SCREEN_HEIGHT - 70 && y <= SCREEN_HEIGHT - 20) {
        m_returnToMenu = true;
        return true;
    }
    
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
    drawGamesMenu();
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
    // For now, games draw directly to M5.Display
    // Future enhancement: use target parameter
    (void)target;
    
    switch (m_currentGame) {
        case GameType::MINESWEEPER:
            drawMinesweeper();
            break;
        case GameType::SUDOKU:
            drawSudoku();
            break;
        case GameType::WORDLE:
            drawWordle();
            break;
        default:
            drawGamesMenu();
            break;
    }
}

bool GameManager::handleTouch(int x, int y) {
    if (m_currentGame == GameType::NONE) {
        return handleMenuTouch(x, y);
    }
    
    switch (m_currentGame) {
        case GameType::MINESWEEPER:
            handleMinesweeperTouch(x, y);
            return true;
        case GameType::SUDOKU:
            handleSudokuTouch(x, y);
            return true;
        case GameType::WORDLE:
            handleWordleTouch(x, y);
            return true;
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

void GameManager::drawMinesweeper() {
    M5.Display.fillScreen(TFT_WHITE);
    
    // Header with restart button
    M5.Display.setTextSize(2.0f);
    M5.Display.setTextColor(TFT_BLACK);
    M5.Display.setTextDatum(textdatum_t::middle_center);
    M5.Display.drawString("RESTART", SCREEN_WIDTH/2, 25);
    int textW = M5.Display.textWidth("RESTART");
    M5.Display.drawLine(SCREEN_WIDTH/2 - textW/2, 45, SCREEN_WIDTH/2 + textW/2, 45, TFT_BLACK);
    
    // Grid
    const int CELL_SIZE = 54;
    const int GRID_START_X = 0;
    const int GRID_START_Y = SCREEN_HEIGHT - (m_minesweeper.GRID_HEIGHT * CELL_SIZE);
    
    for (int i = 0; i < m_minesweeper.GRID_HEIGHT; i++) {
        for (int j = 0; j < m_minesweeper.GRID_WIDTH; j++) {
            int x = GRID_START_X + j * CELL_SIZE;
            int y = GRID_START_Y + i * CELL_SIZE;
            
            M5.Display.drawRect(x, y, CELL_SIZE, CELL_SIZE, TFT_BLACK);
            
            bool showMine = m_minesweeper.cellStates[i][j] == 1 || 
                           (m_minesweeper.gameLost && m_minesweeper.mines[i][j]);
            
            if (showMine) {
                if (m_minesweeper.mines[i][j]) {
                    // Mine
                    if (m_minesweeper.gameLost) {
                        M5.Display.fillRect(x+1, y+1, CELL_SIZE-2, CELL_SIZE-2, TFT_RED);
                    }
                    M5.Display.fillCircle(x + CELL_SIZE/2, y + CELL_SIZE/2, 8, TFT_BLACK);
                } else {
                    // Revealed empty
                    M5.Display.fillRect(x+1, y+1, CELL_SIZE-2, CELL_SIZE-2, GRAY_LIGHT);
                    if (m_minesweeper.neighborCounts[i][j] > 0) {
                        char num[2] = {(char)('0' + m_minesweeper.neighborCounts[i][j]), 0};
                        M5.Display.setTextSize(2.5f);
                        M5.Display.drawString(num, x + CELL_SIZE/2, y + CELL_SIZE/2);
                    }
                }
            } else if (m_minesweeper.cellStates[i][j] == 2) {
                // Flagged
                M5.Display.fillRect(x+1, y+1, CELL_SIZE-2, CELL_SIZE-2, TFT_YELLOW);
                M5.Display.setTextSize(2.0f);
                M5.Display.drawString("F", x + CELL_SIZE/2, y + CELL_SIZE/2);
            }
        }
    }
    
    // Game over message
    if (m_minesweeper.gameWon || m_minesweeper.gameLost) {
        const char* msg = m_minesweeper.gameWon ? "YOU WIN!" : "GAME OVER";
        M5.Display.setTextSize(3.0f);
        M5.Display.fillRect(SCREEN_WIDTH/2 - 120, GRID_START_Y/2 - 30, 240, 60, TFT_WHITE);
        M5.Display.drawRect(SCREEN_WIDTH/2 - 120, GRID_START_Y/2 - 30, 240, 60, TFT_BLACK);
        M5.Display.drawString(msg, SCREEN_WIDTH/2, GRID_START_Y/2);
    }
    
    // Back button
    drawButton(20, 60, 100, 40, "Back", TFT_WHITE, TFT_BLACK);
    
    M5.Display.setTextDatum(textdatum_t::top_left);
    M5.Display.display();
}

void GameManager::handleMinesweeperTouch(int x, int y) {
    const int CELL_SIZE = 54;
    const int GRID_START_X = 0;
    const int GRID_START_Y = SCREEN_HEIGHT - (m_minesweeper.GRID_HEIGHT * CELL_SIZE);
    
    // Back button
    if (x >= 20 && x <= 120 && y >= 60 && y <= 100) {
        stopGame();
        return;
    }
    
    // Restart button (header area)
    if (y < 60 && x > SCREEN_WIDTH/2 - 80 && x < SCREEN_WIDTH/2 + 80) {
        initMinesweeper();
        draw();
        return;
    }
    
    // Grid clicks
    if (y >= GRID_START_Y) {
        int col = (x - GRID_START_X) / CELL_SIZE;
        int row = (y - GRID_START_Y) / CELL_SIZE;
        
        if (row >= 0 && row < m_minesweeper.GRID_HEIGHT && 
            col >= 0 && col < m_minesweeper.GRID_WIDTH) {
            
            if (!m_minesweeper.gameStarted) {
                generateMines(m_minesweeper, row, col);
            }
            
            if (!m_minesweeper.gameLost && !m_minesweeper.gameWon) {
                revealCell(m_minesweeper, row, col);
                draw();
            }
        }
    }
}

// ============================================================================
// Sudoku Implementation
// ============================================================================

void GameManager::initSudoku() {
    memset(&m_sudoku, 0, sizeof(m_sudoku));
    
    // Simple predefined puzzle
    int puzzle[6][6] = {
        {1, 0, 3, 0, 5, 6},
        {4, 5, 0, 1, 0, 3},
        {0, 3, 1, 6, 4, 0},
        {0, 6, 4, 0, 1, 2},
        {3, 0, 2, 5, 6, 0},
        {6, 4, 0, 2, 0, 1}
    };
    
    int solution[6][6] = {
        {1, 2, 3, 4, 5, 6},
        {4, 5, 6, 1, 2, 3},
        {2, 3, 1, 6, 4, 5},
        {5, 6, 4, 3, 1, 2},
        {3, 1, 2, 5, 6, 4},
        {6, 4, 5, 2, 3, 1}
    };
    
    for (int i = 0; i < 6; i++) {
        for (int j = 0; j < 6; j++) {
            m_sudoku.board[i][j] = puzzle[i][j];
            m_sudoku.solution[i][j] = solution[i][j];
            m_sudoku.readonly[i][j] = (puzzle[i][j] != 0);
        }
    }
    
    ESP_LOGI(TAG, "Sudoku initialized");
}

void GameManager::updateSudoku() {
    // Check win condition
    bool complete = true;
    for (int i = 0; i < 6 && complete; i++) {
        for (int j = 0; j < 6 && complete; j++) {
            if (m_sudoku.board[i][j] != m_sudoku.solution[i][j]) {
                complete = false;
            }
        }
    }
    m_sudoku.gameWon = complete;
}

void GameManager::drawSudoku() {
    M5.Display.fillScreen(TFT_WHITE);
    drawHeader("Sudoku");
    
    const int CELL_SIZE = 80;
    const int BLOCK_SPACING = 10;
    const int GRID_WIDTH = 6 * CELL_SIZE + BLOCK_SPACING;
    const int GRID_HEIGHT = 6 * CELL_SIZE + BLOCK_SPACING;
    const int GRID_X = (SCREEN_WIDTH - GRID_WIDTH) / 2;
    const int GRID_Y = STATUS_BAR_HEIGHT + 80;
    
    M5.Display.setTextSize(3.0f);
    M5.Display.setTextDatum(textdatum_t::middle_center);
    
    for (int i = 0; i < 6; i++) {
        for (int j = 0; j < 6; j++) {
            int xOffset = (j >= 3) ? BLOCK_SPACING : 0;
            int yOffset = (i >= 2) ? ((i >= 4) ? BLOCK_SPACING * 2 : BLOCK_SPACING) : 0;
            
            int x = GRID_X + j * CELL_SIZE + xOffset;
            int y = GRID_Y + i * CELL_SIZE + yOffset;
            
            // Cell background
            uint16_t bgColor = TFT_WHITE;
            if (m_sudoku.selectedRow == i && m_sudoku.selectedCol == j) {
                bgColor = TFT_YELLOW;
            } else if (m_sudoku.readonly[i][j]) {
                bgColor = GRAY_LIGHT;
            }
            
            M5.Display.fillRect(x, y, CELL_SIZE, CELL_SIZE, bgColor);
            M5.Display.drawRect(x, y, CELL_SIZE, CELL_SIZE, TFT_BLACK);
            
            // Number
            if (m_sudoku.board[i][j] != 0) {
                M5.Display.setTextColor(m_sudoku.readonly[i][j] ? TFT_BLACK : TFT_BLUE, bgColor);
                char num[2] = {(char)('0' + m_sudoku.board[i][j]), 0};
                M5.Display.drawString(num, x + CELL_SIZE/2, y + CELL_SIZE/2);
            }
        }
    }
    
    // Number input buttons (1-6)
    int btnY = GRID_Y + GRID_HEIGHT + 30;
    int btnSize = 70;
    int btnGap = 10;
    int startX = (SCREEN_WIDTH - (6 * btnSize + 5 * btnGap)) / 2;
    
    M5.Display.setTextSize(2.5f);
    for (int i = 1; i <= 6; i++) {
        int x = startX + (i-1) * (btnSize + btnGap);
        drawButton(x, btnY, btnSize, btnSize, nullptr, GRAY_LIGHT, TFT_BLACK);
        char num[2] = {(char)('0' + i), 0};
        M5.Display.setTextColor(TFT_BLACK, GRAY_LIGHT);
        M5.Display.drawString(num, x + btnSize/2, btnY + btnSize/2);
    }
    
    // Clear button
    drawButton(startX, btnY + btnSize + 20, 150, 50, "Clear", TFT_WHITE, TFT_BLACK);
    
    // Back button
    drawButton(20, SCREEN_HEIGHT - 70, 100, 50, "Back", TFT_WHITE, TFT_BLACK);
    
    // Win message
    if (m_sudoku.gameWon) {
        M5.Display.setTextSize(3.0f);
        M5.Display.setTextColor(TFT_BLACK, TFT_WHITE);
        M5.Display.drawString("COMPLETE!", SCREEN_WIDTH/2, STATUS_BAR_HEIGHT + 50);
    }
    
    M5.Display.setTextDatum(textdatum_t::top_left);
    M5.Display.display();
}

void GameManager::handleSudokuTouch(int x, int y) {
    const int CELL_SIZE = 80;
    const int BLOCK_SPACING = 10;
    const int GRID_WIDTH = 6 * CELL_SIZE + BLOCK_SPACING;
    const int GRID_HEIGHT = 6 * CELL_SIZE + BLOCK_SPACING;
    const int GRID_X = (SCREEN_WIDTH - GRID_WIDTH) / 2;
    const int GRID_Y = STATUS_BAR_HEIGHT + 80;
    
    // Back button
    if (x >= 20 && x <= 120 && y >= SCREEN_HEIGHT - 70 && y <= SCREEN_HEIGHT - 20) {
        stopGame();
        return;
    }
    
    // Grid clicks
    if (x >= GRID_X && x < GRID_X + GRID_WIDTH && 
        y >= GRID_Y && y < GRID_Y + GRID_HEIGHT) {
        
        // Find which cell was clicked (accounting for block spacing)
        int relX = x - GRID_X;
        int relY = y - GRID_Y;
        
        int col = -1, row = -1;
        
        // Calculate column
        if (relX < 3 * CELL_SIZE) {
            col = relX / CELL_SIZE;
        } else {
            col = (relX - BLOCK_SPACING) / CELL_SIZE;
            if (col >= 6) col = 5;
        }
        
        // Calculate row
        if (relY < 2 * CELL_SIZE) {
            row = relY / CELL_SIZE;
        } else if (relY < 4 * CELL_SIZE + BLOCK_SPACING) {
            row = (relY - BLOCK_SPACING) / CELL_SIZE;
        } else {
            row = (relY - 2 * BLOCK_SPACING) / CELL_SIZE;
        }
        
        if (row >= 0 && row < 6 && col >= 0 && col < 6) {
            if (!m_sudoku.readonly[row][col]) {
                m_sudoku.selectedRow = row;
                m_sudoku.selectedCol = col;
                draw();
            }
        }
        return;
    }
    
    // Number buttons
    int btnY = GRID_Y + GRID_HEIGHT + 30;
    int btnSize = 70;
    int btnGap = 10;
    int startX = (SCREEN_WIDTH - (6 * btnSize + 5 * btnGap)) / 2;
    
    if (y >= btnY && y <= btnY + btnSize && m_sudoku.selectedRow >= 0) {
        for (int i = 1; i <= 6; i++) {
            int bx = startX + (i-1) * (btnSize + btnGap);
            if (x >= bx && x <= bx + btnSize) {
                m_sudoku.board[m_sudoku.selectedRow][m_sudoku.selectedCol] = i;
                updateSudoku();
                draw();
                return;
            }
        }
    }
    
    // Clear button
    if (y >= btnY + btnSize + 20 && y <= btnY + btnSize + 70 && 
        x >= startX && x <= startX + 150 && m_sudoku.selectedRow >= 0) {
        m_sudoku.board[m_sudoku.selectedRow][m_sudoku.selectedCol] = 0;
        draw();
    }
}

// ============================================================================
// Wordle Implementation
// ============================================================================

// Simple word list for Wordle
static const char* WORDLE_WORDS[] = {
    "PAPER", "BRAIN", "CRANE", "FLAME", "GRAPE", "HOUSE", "JUICE", "KNIFE",
    "LEMON", "MANGO", "NIGHT", "OCEAN", "PIANO", "QUEEN", "RIVER", "STONE",
    "TIGER", "UNION", "VIDEO", "WATER", "XENON", "YACHT", "ZEBRA", "APPLE",
    "BEACH", "CHAIR", "DREAM", "EARTH", "FLOOR", "GHOST", "HEART", "IMAGE"
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

void GameManager::drawWordle() {
    M5.Display.fillScreen(TFT_WHITE);
    drawHeader("Wordle");
    
    const int CELL_SIZE = 80;
    const int GAP = 8;
    const int GRID_WIDTH = 5 * CELL_SIZE + 4 * GAP;
    const int GRID_X = (SCREEN_WIDTH - GRID_WIDTH) / 2;
    const int GRID_Y = STATUS_BAR_HEIGHT + 80;
    
    M5.Display.setTextSize(3.0f);
    M5.Display.setTextDatum(textdatum_t::middle_center);
    
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
            
            M5.Display.fillRect(x, y, CELL_SIZE, CELL_SIZE, bgColor);
            M5.Display.drawRect(x, y, CELL_SIZE, CELL_SIZE, TFT_BLACK);
            
            if (m_wordle.guesses[row][col] != 0) {
                M5.Display.setTextColor(TFT_BLACK, bgColor);
                char letter[2] = {m_wordle.guesses[row][col], 0};
                M5.Display.drawString(letter, x + CELL_SIZE/2, y + CELL_SIZE/2);
            }
        }
    }
    
    // Draw keyboard
    const char* rows[] = {"QWERTYUIOP", "ASDFGHJKL", "ZXCVBNM"};
    int keyW = 48;
    int keyH = 55;
    int keyGap = 4;
    int keyY = GRID_Y + 6 * (CELL_SIZE + GAP) + 20;
    
    M5.Display.setTextSize(1.8f);
    
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
            
            M5.Display.fillRect(x, y, keyW, keyH, bgColor);
            M5.Display.drawRect(x, y, keyW, keyH, TFT_BLACK);
            
            M5.Display.setTextColor(TFT_BLACK, bgColor);
            char str[2] = {letter, 0};
            M5.Display.drawString(str, x + keyW/2, y + keyH/2);
        }
    }
    
    // Special keys: Enter and Backspace
    int specialY = keyY + 3 * (keyH + keyGap);
    drawButton(SCREEN_WIDTH/2 - 160, specialY, 120, 50, "ENTER", GRAY_LIGHT, TFT_BLACK);
    drawButton(SCREEN_WIDTH/2 + 40, specialY, 120, 50, "DEL", GRAY_LIGHT, TFT_BLACK);
    
    // Back button
    drawButton(20, SCREEN_HEIGHT - 60, 100, 45, "Back", TFT_WHITE, TFT_BLACK);
    
    // Win/Lose message
    if (m_wordle.gameWon) {
        M5.Display.setTextSize(2.5f);
        M5.Display.setTextColor(TFT_BLACK, TFT_WHITE);
        M5.Display.drawString("YOU WIN!", SCREEN_WIDTH/2, STATUS_BAR_HEIGHT + 50);
    } else if (m_wordle.gameLost) {
        M5.Display.setTextSize(2.0f);
        M5.Display.setTextColor(TFT_BLACK, TFT_WHITE);
        char msg[32];
        snprintf(msg, sizeof(msg), "Answer: %s", m_wordle.answer);
        M5.Display.drawString(msg, SCREEN_WIDTH/2, STATUS_BAR_HEIGHT + 50);
    }
    
    M5.Display.setTextDatum(textdatum_t::top_left);
    M5.Display.display();
}

void GameManager::handleWordleTouch(int x, int y) {
    const int CELL_SIZE = 80;
    const int GAP = 8;
    const int GRID_Y = STATUS_BAR_HEIGHT + 80;
    
    // Back button
    if (x >= 20 && x <= 120 && y >= SCREEN_HEIGHT - 60 && y <= SCREEN_HEIGHT - 15) {
        stopGame();
        return;
    }
    
    if (m_wordle.gameWon || m_wordle.gameLost) {
        // Tap to restart
        initWordle();
        draw();
        return;
    }
    
    // Keyboard handling
    const char* rows[] = {"QWERTYUIOP", "ASDFGHJKL", "ZXCVBNM"};
    int keyW = 48;
    int keyH = 55;
    int keyGap = 4;
    int keyY = GRID_Y + 6 * (CELL_SIZE + GAP) + 20;
    
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
                    m_wordle.guesses[m_wordle.currentRow][m_wordle.currentCol] = rows[r][c];
                    m_wordle.currentCol++;
                    draw();
                }
                return;
            }
        }
    }
    
    // Special keys
    int specialY = keyY + 3 * (keyH + keyGap);
    
    // Enter button
    if (x >= SCREEN_WIDTH/2 - 160 && x <= SCREEN_WIDTH/2 - 40 && 
        y >= specialY && y <= specialY + 50) {
        if (m_wordle.currentCol == 5) {
            // Check the guess
            char guess[6];
            for (int i = 0; i < 5; i++) {
                guess[i] = m_wordle.guesses[m_wordle.currentRow][i];
            }
            guess[5] = '\0';
            
            bool correct = true;
            for (int i = 0; i < 5; i++) {
                char c = guess[i];
                if (c == m_wordle.answer[i]) {
                    m_wordle.states[m_wordle.currentRow][i] = 'g';
                    m_wordle.usedLetters[c - 'A'] = 'g';
                } else {
                    correct = false;
                    // Check if letter is elsewhere in answer
                    bool found = false;
                    for (int j = 0; j < 5; j++) {
                        if (m_wordle.answer[j] == c) {
                            found = true;
                            break;
                        }
                    }
                    if (found) {
                        m_wordle.states[m_wordle.currentRow][i] = 'y';
                        if (m_wordle.usedLetters[c - 'A'] != 'g') {
                            m_wordle.usedLetters[c - 'A'] = 'y';
                        }
                    } else {
                        m_wordle.states[m_wordle.currentRow][i] = 'x';
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
            draw();
        }
        return;
    }
    
    // Delete button
    if (x >= SCREEN_WIDTH/2 + 40 && x <= SCREEN_WIDTH/2 + 160 &&
        y >= specialY && y <= specialY + 50) {
        if (m_wordle.currentCol > 0) {
            m_wordle.currentCol--;
            m_wordle.guesses[m_wordle.currentRow][m_wordle.currentCol] = 0;
            draw();
        }
        return;
    }
}
