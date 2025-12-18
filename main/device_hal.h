#pragma once

#include "sdkconfig.h"
#include <M5Unified.h>
#include <string>
#include <functional>

/**
 * @brief Hardware Abstraction Layer for M5Paper devices
 * 
 * This class provides a unified interface for hardware features that differ
 * between M5Paper (original) and M5PaperS3.
 */
class DeviceHAL {
public:
    // Singleton access
    static DeviceHAL& getInstance();

    // Delete copy/move constructors
    DeviceHAL(const DeviceHAL&) = delete;
    DeviceHAL& operator=(const DeviceHAL&) = delete;

    /**
     * @brief Initialize the hardware abstraction layer
     * @param isWakeFromSleep Whether this is a wake from deep sleep
     */
    void init(bool isWakeFromSleep = false);

    /**
     * @brief Check if running on M5PaperS3
     */
    bool isM5PaperS3() const;

    /**
     * @brief Check if running on original M5Paper
     */
    bool isM5Paper() const;

    /**
     * @brief Get device name string
     */
    const char* getDeviceName() const;

    // ==================== Display ====================

    /**
     * @brief Get display width
     */
    int getDisplayWidth() const;

    /**
     * @brief Get display height
     */
    int getDisplayHeight() const;

    /**
     * @brief Get optimal color depth for canvases (bpp)
     */
    int getCanvasColorDepth() const;

    /**
     * @brief Get current rotation (0-3)
     */
    int getRotation() const;

    /**
     * @brief Set display rotation
     * @param rotation 0=portrait, 1=landscape (90°), 2=portrait (180°), 3=landscape (270°)
     */
    void setRotation(int rotation);

    /**
     * @brief Check if currently in landscape mode
     */
    bool isLandscape() const;

    // ==================== Buzzer (M5PaperS3 only) ====================

    /**
     * @brief Check if buzzer is available
     */
    bool hasBuzzer() const;

    /**
     * @brief Play a short click sound (if buzzer is enabled)
     */
    void playClickSound();

    /**
     * @brief Play a custom tone
     * @param frequency Frequency in Hz
     * @param duration Duration in ms
     */
    void playTone(int frequency, int duration);

    /**
     * @brief Set buzzer enabled state
     */
    void setBuzzerEnabled(bool enabled);

    /**
     * @brief Check if buzzer is enabled
     */
    bool isBuzzerEnabled() const;

    // ==================== Gyroscope/IMU (M5PaperS3 only) ====================

    /**
     * @brief Check if gyroscope/IMU is available
     */
    bool hasGyroscope() const;

    /**
     * @brief Check if auto-rotation is enabled
     */
    bool isAutoRotateEnabled() const;

    /**
     * @brief Set auto-rotation enabled state
     */
    void setAutoRotateEnabled(bool enabled);

    /**
     * @brief Update orientation from gyroscope and auto-rotate if needed
     * Call this periodically from the main loop
     * @return true if rotation changed
     */
    bool updateOrientation();

    /**
     * @brief Get raw accelerometer data
     */
    void getAcceleration(float& x, float& y, float& z);

    // ==================== SD Card ====================

    /**
     * @brief Check if SD card slot is available
     */
    bool hasSDCardSlot() const;

    /**
     * @brief Check if SD card is mounted
     */
    bool isSDCardMounted() const;

    /**
     * @brief Mount the SD card
     * @return true if successful
     */
    bool mountSDCard();

    /**
     * @brief Unmount the SD card
     */
    void unmountSDCard();

    /**
     * @brief Get SD card mount point
     */
    const char* getSDCardMountPoint() const;

    /**
     * @brief Get SD card total size in bytes
     */
    uint64_t getSDCardTotalSize() const;

    /**
     * @brief Get SD card free space in bytes
     */
    uint64_t getSDCardFreeSize() const;

    /**
     * @brief Format the SD card (FAT32)
     * @param progressCallback Optional callback for progress updates (0-100)
     * @return true if successful
     */
    bool formatSDCard(std::function<void(int)> progressCallback = nullptr);

    // ==================== Power & Sleep ====================

    /**
     * @brief Get GPIO pin for touch interrupt (for wake)
     */
    gpio_num_t getTouchInterruptPin() const;

    /**
     * @brief Get GPIO pin for main power control
     */
    gpio_num_t getMainPowerPin() const;

    /**
     * @brief Enter deep sleep with touch wake capability
     */
    void enterDeepSleepWithTouchWake();

    /**
     * @brief Enter deep sleep shutdown (power off peripherals)
     */
    void enterDeepSleepShutdown();

    // ==================== Task Pinning (ESP32-S3) ====================

    /**
     * @brief Get the core to pin render tasks to
     */
    int getRenderTaskCore() const;

    /**
     * @brief Get the core to pin the main task to
     */
    int getMainTaskCore() const;

    /**
     * @brief Check if dual-core optimization is enabled
     */
    bool isDualCoreOptimized() const;

private:
    DeviceHAL();
    ~DeviceHAL() = default;

    bool m_initialized = false;
    bool m_buzzerEnabled = false;
    bool m_autoRotateEnabled = false;
    bool m_sdCardMounted = false;
    int m_currentRotation = 0;
    int m_lastStableRotation = 0;
    uint32_t m_rotationChangeTime = 0;

    // Rotation debouncing
    static constexpr uint32_t ROTATION_DEBOUNCE_MS = 500;
    static constexpr float TILT_THRESHOLD = 0.6f;
};

// Convenience macro
//#define deviceHAL DeviceHAL::getInstance()
