#include "gesture_detector.h"
#include "esp_timer.h"
#include "esp_log.h"
#include <cmath>

static const char* TAG = "Gesture";

GestureDetector::GestureDetector() {
    // Default initialization
}

void GestureDetector::init(int screenWidth, int screenHeight) {
    m_screenWidth = screenWidth;
    m_screenHeight = screenHeight;
    reset();
    ESP_LOGI(TAG, "GestureDetector initialized: %dx%d", screenWidth, screenHeight);
}

void GestureDetector::reset() {
    m_touchActive = false;
    m_touchStarted = false;
    m_startX = 0;
    m_startY = 0;
    m_currentX = 0;
    m_currentY = 0;
    m_startTime = 0;
    m_waitingForDoubleTap = false;
}

ScreenZone GestureDetector::getZone(int x, int y) const {
    // Check vertical zones first
    if (y < topZoneHeight) {
        return ScreenZone::TOP;
    }
    if (y > m_screenHeight - bottomZoneHeight) {
        return ScreenZone::BOTTOM;
    }
    
    // Check horizontal edges
    if (x < edgeZoneWidth) {
        return ScreenZone::LEFT_EDGE;
    }
    if (x > m_screenWidth - edgeZoneWidth) {
        return ScreenZone::RIGHT_EDGE;
    }
    
    return ScreenZone::MIDDLE;
}

SwipeDirection GestureDetector::detectSwipeDirection(int dx, int dy) const {
    int absDx = abs(dx);
    int absDy = abs(dy);
    
    // Determine primary direction
    if (absDx > absDy) {
        // Horizontal swipe
        return (dx > 0) ? SwipeDirection::RIGHT : SwipeDirection::LEFT;
    } else {
        // Vertical swipe
        return (dy > 0) ? SwipeDirection::DOWN : SwipeDirection::UP;
    }
}

bool GestureDetector::isValidSwipe(int dx, int dy, uint32_t duration) const {
    int distance = (int)sqrt(dx * dx + dy * dy);
    
    // Must move enough distance
    if (distance < SWIPE_THRESHOLD) {
        return false;
    }
    
    // Check velocity (must be fast enough to be intentional)
    if (duration > 0) {
        float velocity = (float)distance / (float)duration;
        if (velocity < MIN_SWIPE_VELOCITY) {
            return false;
        }
    }
    
    return true;
}

GestureEvent GestureDetector::update() {
    GestureEvent event;
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    
    // Check for double-tap timeout
    if (m_waitingForDoubleTap && (now - m_lastTapTime > DOUBLE_TAP_MS)) {
        // Double tap window expired, emit single tap
        event.type = GestureType::TAP;
        event.startX = m_lastTapX;
        event.startY = m_lastTapY;
        event.endX = m_lastTapX;
        event.endY = m_lastTapY;
        event.zone = getZone(m_lastTapX, m_lastTapY);
        m_waitingForDoubleTap = false;
        
        if (m_tapCallback) {
            m_tapCallback(event);
        }
        return event;
    }
    
    if (M5.Touch.getCount() > 0) {
        auto t = M5.Touch.getDetail(0);
        
        if (t.wasPressed() && !m_touchActive) {
            // Touch started
            m_touchActive = true;
            m_touchStarted = true;
            m_startX = t.x;
            m_startY = t.y;
            m_currentX = t.x;
            m_currentY = t.y;
            m_startTime = now;
        } else if (m_touchActive) {
            // Touch continuing
            m_currentX = t.x;
            m_currentY = t.y;
        }
        
        if (t.wasReleased() && m_touchActive) {
            // Touch ended - analyze the gesture
            m_touchActive = false;
            
            int dx = m_currentX - m_startX;
            int dy = m_currentY - m_startY;
            uint32_t duration = now - m_startTime;
            
            event.startX = m_startX;
            event.startY = m_startY;
            event.endX = m_currentX;
            event.endY = m_currentY;
            event.duration = duration;
            event.zone = getZone(m_startX, m_startY);
            
            // Calculate velocity
            int distance = (int)sqrt(dx * dx + dy * dy);
            event.velocity = (duration > 0) ? (float)distance / (float)duration : 0.0f;
            
            // Check if it's a swipe
            if (isValidSwipe(dx, dy, duration)) {
                SwipeDirection dir = detectSwipeDirection(dx, dy);
                switch (dir) {
                    case SwipeDirection::UP:
                        event.type = GestureType::SWIPE_UP;
                        break;
                    case SwipeDirection::DOWN:
                        event.type = GestureType::SWIPE_DOWN;
                        break;
                    case SwipeDirection::LEFT:
                        event.type = GestureType::SWIPE_LEFT;
                        break;
                    case SwipeDirection::RIGHT:
                        event.type = GestureType::SWIPE_RIGHT;
                        break;
                    default:
                        break;
                }
                
                if (m_swipeCallback) {
                    m_swipeCallback(event);
                }
                
                // Cancel any pending double tap
                m_waitingForDoubleTap = false;
            }
            // Check if it's a tap
            else if (distance < TAP_THRESHOLD) {
                // Check for long press
                if (duration >= LONG_PRESS_MS) {
                    event.type = GestureType::LONG_PRESS;
                    if (m_tapCallback) {
                        m_tapCallback(event);
                    }
                    m_waitingForDoubleTap = false;
                }
                // Check for double tap
                else if (m_waitingForDoubleTap) {
                    // Check if same location
                    int tapDx = abs(m_currentX - m_lastTapX);
                    int tapDy = abs(m_currentY - m_lastTapY);
                    if (tapDx < TAP_THRESHOLD * 3 && tapDy < TAP_THRESHOLD * 3) {
                        event.type = GestureType::DOUBLE_TAP;
                        if (m_tapCallback) {
                            m_tapCallback(event);
                        }
                        m_waitingForDoubleTap = false;
                    } else {
                        // Different location - emit previous tap and start waiting again
                        GestureEvent prevTap;
                        prevTap.type = GestureType::TAP;
                        prevTap.startX = m_lastTapX;
                        prevTap.startY = m_lastTapY;
                        prevTap.endX = m_lastTapX;
                        prevTap.endY = m_lastTapY;
                        prevTap.zone = getZone(m_lastTapX, m_lastTapY);
                        if (m_tapCallback) {
                            m_tapCallback(prevTap);
                        }
                        
                        // Start waiting for potential double tap on new location
                        m_lastTapX = m_currentX;
                        m_lastTapY = m_currentY;
                        m_lastTapTime = now;
                        m_waitingForDoubleTap = true;
                    }
                }
                else {
                    // First tap - wait for potential double tap
                    m_lastTapX = m_currentX;
                    m_lastTapY = m_currentY;
                    m_lastTapTime = now;
                    m_waitingForDoubleTap = true;
                    // Don't return event yet, wait for double tap window
                }
            }
            
            return event;
        }
    } else if (m_touchActive) {
        // Touch ended (no count but was active)
        m_touchActive = false;
    }
    
    return event;
}
