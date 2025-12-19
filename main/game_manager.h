#pragma once
#include <M5Unified.h>
#include <string>
#include <vector>
#include <functional>

// Forward declarations
class GUI;

// Game type enumeration
enum class GameType {
    NONE,
    MINESWEEPER,
    SUDOKU,
    WORDLE
};

// Game state
enum class GameState {
    MENU,       // Game selection menu
    PLAYING,    // Active game
    WON,        // Game won
    LOST,       // Game lost
    PAUSED      // Game paused
};

/**
 * @brief Game Manager handles all integrated games
 */
class GameManager {
public:
    static GameManager& getInstance();
    
    GameManager(const GameManager&) = delete;
    GameManager& operator=(const GameManager&) = delete;
    
    /**
     * @brief Initialize the game manager
     */
    void init();
    
    /**
     * @brief Draw the games menu
     */
    void drawGamesMenu();
    
    /**
     * @brief Handle touch in games menu
     * @return true if a game was selected
     */
    bool handleMenuTouch(int x, int y);
    
    /**
     * @brief Start a specific game
     */
    void startGame(GameType type);
    
    /**
     * @brief Stop the current game and return to menu
     */
    void stopGame();
    
    /**
     * @brief Update the current game (call in main loop)
     */
    void update();
    
    /**
     * @brief Draw the current game screen
     * @param target Optional target canvas (nullptr = draw to M5.Display)
     */
    void draw(LovyanGFX* target = nullptr);
    
    /**
     * @brief Handle touch input for the current game
     * @return true if touch was handled and redraw needed
     */
    bool handleTouch(int x, int y);
    
    /**
     * @brief Check if a game is currently active
     */
    bool isGameActive() const { return m_currentGame != GameType::NONE; }
    
    /**
     * @brief Get the current game type
     */
    GameType getCurrentGame() const { return m_currentGame; }
    
    /**
     * @brief Get game state
     */
    GameState getState() const { return m_state; }
    
    /**
     * @brief Check if we should return to main menu
     */
    bool shouldReturnToMenu() const { return m_returnToMenu; }
    void clearReturnFlag() { m_returnToMenu = false; }
    
    // Callback for when returning to main app
    using ReturnCallback = std::function<void()>;
    void setReturnCallback(ReturnCallback cb) { m_returnCallback = cb; }
    
private:
    GameManager();
    ~GameManager() = default;
    
    GameType m_currentGame = GameType::NONE;
    GameState m_state = GameState::MENU;
    bool m_returnToMenu = false;
    bool m_initialized = false;
    
    ReturnCallback m_returnCallback = nullptr;
    
    // Game-specific state
    void initMinesweeper();
    void updateMinesweeper();
    void drawMinesweeper();
    void handleMinesweeperTouch(int x, int y);
    
    void initSudoku();
    void updateSudoku();
    void drawSudoku();
    void handleSudokuTouch(int x, int y);
    
    void initWordle();
    void updateWordle();
    void drawWordle();
    void handleWordleTouch(int x, int y);
    
public:
    // Minesweeper state
    struct MinesweeperState {
        static const int GRID_WIDTH = 10;
        static const int GRID_HEIGHT = 15;
        static const int MINE_COUNT = 25;
        bool mines[15][10] = {};
        int neighborCounts[15][10] = {};
        int cellStates[15][10] = {}; // 0=hidden, 1=revealed, 2=flagged
        bool gameWon = false;
        bool gameLost = false;
        bool gameStarted = false;
        int revealedCount = 0;
    };
    
    // Sudoku state
    struct SudokuState {
        static const int GRID_SIZE = 6;
        int board[6][6] = {};
        int solution[6][6] = {};
        bool readonly[6][6] = {};
        bool gameWon = false;
        int selectedRow = -1;
        int selectedCol = -1;
        bool showingKeyboard = false;
    };
    
    // Wordle state  
    struct WordleState {
        char guesses[6][5] = {};
        char states[6][5] = {}; // 'g'=green, 'y'=yellow, 'x'=gray, 0=empty
        char answer[6] = {};
        int currentRow = 0;
        int currentCol = 0;
        bool gameWon = false;
        bool gameLost = false;
        char usedLetters[26] = {}; // State of each letter a-z
    };

private:
    MinesweeperState m_minesweeper;
    SudokuState m_sudoku;
    WordleState m_wordle;
    
    // Common drawing helpers
    void drawButton(int x, int y, int w, int h, const char* label, 
                   uint16_t bgColor, uint16_t textColor);
    void drawHeader(const char* title);
};
