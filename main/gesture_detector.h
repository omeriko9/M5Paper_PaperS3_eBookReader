#pragma once
#include <M5Unified.h>
#include <functional>

// Swipe direction enumeration
enum class SwipeDirection {
    NONE,
    UP,
    DOWN,
    LEFT,
    RIGHT
};

// Gesture event types
enum class GestureType {
    NONE,
    TAP,
    DOUBLE_TAP,
    LONG_PRESS,
    SWIPE_UP,
    SWIPE_DOWN,
    SWIPE_LEFT,
    SWIPE_RIGHT
};

// Screen zone for context-aware gestures
enum class ScreenZone {
    TOP,        // Status bar area
    MIDDLE,     // Main content area
    BOTTOM,     // Footer area
    LEFT_EDGE,  // Left edge for swipe detection
    RIGHT_EDGE  // Right edge for swipe detection
};

// Gesture event data
struct GestureEvent {
    GestureType type = GestureType::NONE;
    int startX = 0;
    int startY = 0;
    int endX = 0;
    int endY = 0;
    uint32_t duration = 0;
    ScreenZone zone = ScreenZone::MIDDLE;
    float velocity = 0.0f;  // Pixels per millisecond
};

class GestureDetector {
public:
    // Configuration - optimized for responsiveness
    static constexpr int SWIPE_THRESHOLD = 40;       // Reduced from 50 for easier swipes
    static constexpr int TAP_THRESHOLD = 20;         // Slightly increased for better tap detection
    static constexpr uint32_t LONG_PRESS_MS = 400;   // Reduced from 500 for faster long-press
    static constexpr uint32_t DOUBLE_TAP_MS = 200;   // Reduced from 300 for faster double-tap
    static constexpr float MIN_SWIPE_VELOCITY = 0.08f; // Reduced from 0.1 for easier swipes
    
    // Zone boundaries (set during init based on screen size)
    int topZoneHeight = 44;     // Status bar
    int bottomZoneHeight = 50;  // Footer
    int edgeZoneWidth = 40;     // Side edges for swipe detection
    
    GestureDetector();
    
    /**
     * @brief Initialize with screen dimensions
     */
    void init(int screenWidth, int screenHeight);
    
    /**
     * @brief Process touch input and detect gestures
     * Call this every frame with touch state
     * @return GestureEvent with detected gesture (NONE if no complete gesture)
     */
    GestureEvent update();
    
    /**
     * @brief Reset gesture state (call when transitioning screens)
     */
    void reset();
    
    /**
     * @brief Check if a gesture is currently in progress
     */
    bool isGestureInProgress() const { return m_touchActive; }
    
    /**
     * @brief Get the screen zone for a point
     */
    ScreenZone getZone(int x, int y) const;
    
    // Callbacks for gesture events (optional alternative to polling)
    using GestureCallback = std::function<void(const GestureEvent&)>;
    void setSwipeCallback(GestureCallback cb) { m_swipeCallback = cb; }
    void setTapCallback(GestureCallback cb) { m_tapCallback = cb; }
    
private:
    int m_screenWidth = 540;
    int m_screenHeight = 960;
    
    // Touch tracking state
    bool m_touchActive = false;
    bool m_touchStarted = false;
    int m_startX = 0;
    int m_startY = 0;
    int m_currentX = 0;
    int m_currentY = 0;
    uint32_t m_startTime = 0;
    uint32_t m_lastTapTime = 0;
    int m_lastTapX = 0;
    int m_lastTapY = 0;
    bool m_waitingForDoubleTap = false;
    
    // Callbacks
    GestureCallback m_swipeCallback = nullptr;
    GestureCallback m_tapCallback = nullptr;
    
    SwipeDirection detectSwipeDirection(int dx, int dy) const;
    bool isValidSwipe(int dx, int dy, uint32_t duration) const;
};
