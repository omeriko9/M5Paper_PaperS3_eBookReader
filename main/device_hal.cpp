#include "device_hal.h"
#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_sleep.h"
#include "driver/gpio.h"
#include "driver/rtc_io.h"

#ifdef CONFIG_EBOOK_DEVICE_M5PAPERS3
#include "driver/ledc.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdspi_host.h"
#endif

static const char* TAG = "DeviceHAL";

// Pin definitions for M5Paper (original)
#ifdef CONFIG_EBOOK_DEVICE_M5PAPER
static constexpr gpio_num_t M5PAPER_TOUCH_INT_PIN = GPIO_NUM_36;
static constexpr gpio_num_t M5PAPER_MAIN_PWR_PIN = GPIO_NUM_2;
static constexpr gpio_num_t M5PAPER_SD_CS_PIN = GPIO_NUM_4;
static constexpr gpio_num_t M5PAPER_SD_MOSI_PIN = GPIO_NUM_12;
static constexpr gpio_num_t M5PAPER_SD_MISO_PIN = GPIO_NUM_13;
static constexpr gpio_num_t M5PAPER_SD_CLK_PIN = GPIO_NUM_14;
#endif

// Pin definitions for M5PaperS3
#ifdef CONFIG_EBOOK_DEVICE_M5PAPERS3
static constexpr gpio_num_t M5PAPERS3_TOUCH_INT_PIN = GPIO_NUM_36; // Placeholder, verify if needed
static constexpr gpio_num_t M5PAPERS3_MAIN_PWR_PIN = GPIO_NUM_2;
static constexpr gpio_num_t M5PAPERS3_BUZZER_PIN = GPIO_NUM_21;
static constexpr gpio_num_t M5PAPERS3_SD_CS_PIN = GPIO_NUM_47;
static constexpr gpio_num_t M5PAPERS3_SD_MOSI_PIN = GPIO_NUM_38;
static constexpr gpio_num_t M5PAPERS3_SD_MISO_PIN = GPIO_NUM_40;
static constexpr gpio_num_t M5PAPERS3_SD_CLK_PIN = GPIO_NUM_39;

// LEDC configuration for buzzer
static constexpr ledc_timer_t BUZZER_TIMER = LEDC_TIMER_0;
static constexpr ledc_channel_t BUZZER_CHANNEL = LEDC_CHANNEL_0;
static constexpr ledc_mode_t BUZZER_MODE = LEDC_LOW_SPEED_MODE;

static sdmmc_card_t* s_card = nullptr;
#endif

DeviceHAL& DeviceHAL::getInstance() {
    static DeviceHAL instance;
    return instance;
}

DeviceHAL::DeviceHAL() {
#ifdef CONFIG_EBOOK_S3_BUZZER_DEFAULT_ENABLED
    m_buzzerEnabled = true;
#else
    m_buzzerEnabled = false;
#endif

#ifdef CONFIG_EBOOK_S3_AUTO_ROTATE
    m_autoRotateEnabled = true;
#else
    m_autoRotateEnabled = false;
#endif
}

void DeviceHAL::init(bool isWakeFromSleep) {
    if (m_initialized) return;

    ESP_LOGI(TAG, "Initializing DeviceHAL for %s", getDeviceName());

#ifdef CONFIG_EBOOK_DEVICE_M5PAPERS3
    // Initialize buzzer
#ifdef CONFIG_EBOOK_S3_ENABLE_BUZZER
    ESP_LOGI(TAG, "Initializing buzzer on pin %d", M5PAPERS3_BUZZER_PIN);
    
    ledc_timer_config_t timer_conf = {
        .speed_mode = BUZZER_MODE,
        .duty_resolution = LEDC_TIMER_10_BIT,
        .timer_num = BUZZER_TIMER,
        .freq_hz = CONFIG_EBOOK_S3_BUZZER_FREQUENCY,
        .clk_cfg = LEDC_AUTO_CLK,
        .deconfigure = false
    };
    ledc_timer_config(&timer_conf);

    ledc_channel_config_t channel_conf = {
        .gpio_num = M5PAPERS3_BUZZER_PIN,
        .speed_mode = BUZZER_MODE,
        .channel = BUZZER_CHANNEL,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = BUZZER_TIMER,
        .duty = 0,
        .hpoint = 0,
        .flags = {
            .output_invert = 0
        }
    };
    ledc_channel_config(&channel_conf);
#endif

    // Initialize IMU for auto-rotation
#ifdef CONFIG_EBOOK_S3_ENABLE_GYROSCOPE
    if (M5.Imu.isEnabled()) {
        ESP_LOGI(TAG, "IMU initialized for auto-rotation");
    } else {
        ESP_LOGW(TAG, "IMU not available");
    }
#endif

#endif // CONFIG_EBOOK_DEVICE_M5PAPERS3

    m_initialized = true;
    ESP_LOGI(TAG, "DeviceHAL initialized");
}

bool DeviceHAL::isM5PaperS3() const {
#ifdef CONFIG_EBOOK_DEVICE_M5PAPERS3
    return true;
#else
    return false;
#endif
}

bool DeviceHAL::isM5Paper() const {
#ifdef CONFIG_EBOOK_DEVICE_M5PAPER
    return true;
#else
    return false;
#endif
}

const char* DeviceHAL::getDeviceName() const {
#ifdef CONFIG_EBOOK_DEVICE_M5PAPERS3
    return "M5PaperS3";
#else
    return "M5Paper";
#endif
}

int DeviceHAL::getDisplayWidth() const {
    return M5.Display.width();
}

int DeviceHAL::getDisplayHeight() const {
    return M5.Display.height();
}

int DeviceHAL::getCanvasColorDepth() const {
#ifdef CONFIG_EBOOK_CANVAS_COLOR_DEPTH
    return CONFIG_EBOOK_CANVAS_COLOR_DEPTH;
#elif defined(CONFIG_EBOOK_DEVICE_M5PAPERS3) && defined(CONFIG_EBOOK_S3_16_GRAYSCALE)
    return 4; // 16 shades
#else
    return 2; // 4 shades
#endif
}

int DeviceHAL::getRotation() const {
    return m_currentRotation;
}

void DeviceHAL::setRotation(int rotation) {
    if (rotation < 0 || rotation > 3) return;
    if (m_currentRotation == rotation) return;

    m_currentRotation = rotation;
    M5.Display.setRotation(rotation);
    ESP_LOGI(TAG, "Display rotation set to %d", rotation);
}

bool DeviceHAL::isLandscape() const {
    return (m_currentRotation == 1 || m_currentRotation == 3);
}

// ==================== Buzzer ====================

bool DeviceHAL::hasBuzzer() const {
#ifdef CONFIG_EBOOK_S3_ENABLE_BUZZER
    return true;
#else
    return false;
#endif
}

void DeviceHAL::playClickSound() {
#ifdef CONFIG_EBOOK_S3_ENABLE_BUZZER
    if (!m_buzzerEnabled) return;
    playTone(CONFIG_EBOOK_S3_BUZZER_FREQUENCY, CONFIG_EBOOK_S3_BUZZER_DURATION_MS);
#endif
}

void DeviceHAL::playTone(int frequency, int duration) {
#ifdef CONFIG_EBOOK_S3_ENABLE_BUZZER
    if (!m_buzzerEnabled) return;

    // Set frequency
    ledc_set_freq(BUZZER_MODE, BUZZER_TIMER, frequency);
    
    // Set duty cycle to 50% for audible tone
    ledc_set_duty(BUZZER_MODE, BUZZER_CHANNEL, 512);
    ledc_update_duty(BUZZER_MODE, BUZZER_CHANNEL);
    
    // Wait for duration
    vTaskDelay(pdMS_TO_TICKS(duration));
    
    // Stop tone
    ledc_set_duty(BUZZER_MODE, BUZZER_CHANNEL, 0);
    ledc_update_duty(BUZZER_MODE, BUZZER_CHANNEL);
#else
    (void)frequency;
    (void)duration;
#endif
}

void DeviceHAL::setBuzzerEnabled(bool enabled) {
    m_buzzerEnabled = enabled;
    ESP_LOGI(TAG, "Buzzer %s", enabled ? "enabled" : "disabled");
}

bool DeviceHAL::isBuzzerEnabled() const {
    return m_buzzerEnabled;
}

// ==================== Gyroscope/IMU ====================

bool DeviceHAL::hasGyroscope() const {
#ifdef CONFIG_EBOOK_S3_ENABLE_GYROSCOPE
    return M5.Imu.isEnabled();
#else
    return false;
#endif
}

bool DeviceHAL::isAutoRotateEnabled() const {
    return m_autoRotateEnabled;
}

void DeviceHAL::setAutoRotateEnabled(bool enabled) {
    m_autoRotateEnabled = enabled;
    ESP_LOGI(TAG, "Auto-rotate %s", enabled ? "enabled" : "disabled");
}

bool DeviceHAL::updateOrientation() {
#ifdef CONFIG_EBOOK_S3_ENABLE_GYROSCOPE
    if (!m_autoRotateEnabled || !hasGyroscope()) {
        return false;
    }

    float ax, ay, az;
    getAcceleration(ax, ay, az);

    // Determine orientation based on accelerometer
    int newRotation = m_currentRotation;
    
    if (ay > TILT_THRESHOLD) {
        newRotation = 0; // Portrait (normal)
    } else if (ay < -TILT_THRESHOLD) {
        newRotation = 2; // Portrait (upside down)
    } else if (ax > TILT_THRESHOLD) {
        newRotation = 1; // Landscape (left)
    } else if (ax < -TILT_THRESHOLD) {
        newRotation = 3; // Landscape (right)
    }

    // Debounce rotation changes
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    
    if (newRotation != m_lastStableRotation) {
        m_lastStableRotation = newRotation;
        m_rotationChangeTime = now;
        return false;
    }

    if (newRotation != m_currentRotation && 
        (now - m_rotationChangeTime) > ROTATION_DEBOUNCE_MS) {
        setRotation(newRotation);
        return true;
    }
#endif
    return false;
}

void DeviceHAL::getAcceleration(float& x, float& y, float& z) {
#ifdef CONFIG_EBOOK_S3_ENABLE_GYROSCOPE
    if (M5.Imu.isEnabled()) {
        M5.Imu.getAccel(&x, &y, &z);
    } else {
        x = y = z = 0.0f;
    }
#else
    x = y = z = 0.0f;
#endif
}

// ==================== SD Card ====================

bool DeviceHAL::hasSDCardSlot() const {
#ifdef CONFIG_EBOOK_S3_ENABLE_SD_CARD
    return true;
#else
    return false;
#endif
}

bool DeviceHAL::isSDCardMounted() const {
    return m_sdCardMounted;
}

bool DeviceHAL::mountSDCard() {
#ifdef CONFIG_EBOOK_S3_ENABLE_SD_CARD
    if (m_sdCardMounted) {
        ESP_LOGI(TAG, "SD card already mounted");
        return true;
    }

    ESP_LOGI(TAG, "Mounting SD card...");

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SPI2_HOST;
    host.max_freq_khz = 20000; // 20 MHz for stability

    spi_bus_config_t bus_cfg = {
        .mosi_io_num = M5PAPERS3_SD_MOSI_PIN,
        .miso_io_num = M5PAPERS3_SD_MISO_PIN,
        .sclk_io_num = M5PAPERS3_SD_CLK_PIN,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4096,
        .flags = SPICOMMON_BUSFLAG_MASTER,
        .isr_cpu_id = ESP_INTR_CPU_AFFINITY_AUTO,
        .intr_flags = 0
    };

    esp_err_t ret = spi_bus_initialize((spi_host_device_t)host.slot, &bus_cfg, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "Failed to initialize SPI bus: %s", esp_err_to_name(ret));
        return false;
    }

    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.gpio_cs = M5PAPERS3_SD_CS_PIN;
    slot_config.host_id = (spi_host_device_t)host.slot;

    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 10,
        .allocation_unit_size = 16 * 1024,
        .disk_status_check_enable = false
    };

    ret = esp_vfs_fat_sdspi_mount(
        CONFIG_EBOOK_S3_SD_MOUNT_POINT,
        &host,
        &slot_config,
        &mount_config,
        &s_card
    );

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to mount SD card: %s", esp_err_to_name(ret));
        return false;
    }

    m_sdCardMounted = true;
    ESP_LOGI(TAG, "SD card mounted at %s", CONFIG_EBOOK_S3_SD_MOUNT_POINT);
    
    if (s_card) {
        sdmmc_card_print_info(stdout, s_card);
    }
    
    return true;
#else
    return false;
#endif
}

void DeviceHAL::unmountSDCard() {
#ifdef CONFIG_EBOOK_S3_ENABLE_SD_CARD
    if (!m_sdCardMounted) return;

    esp_vfs_fat_sdcard_unmount(CONFIG_EBOOK_S3_SD_MOUNT_POINT, s_card);
    s_card = nullptr;
    m_sdCardMounted = false;
    ESP_LOGI(TAG, "SD card unmounted");
#endif
}

const char* DeviceHAL::getSDCardMountPoint() const {
#ifdef CONFIG_EBOOK_S3_ENABLE_SD_CARD
    return CONFIG_EBOOK_S3_SD_MOUNT_POINT;
#else
    return nullptr;
#endif
}

uint64_t DeviceHAL::getSDCardTotalSize() const {
#ifdef CONFIG_EBOOK_S3_ENABLE_SD_CARD
    if (!m_sdCardMounted || !s_card) return 0;
    return (uint64_t)s_card->csd.capacity * s_card->csd.sector_size;
#else
    return 0;
#endif
}

uint64_t DeviceHAL::getSDCardFreeSize() const {
#ifdef CONFIG_EBOOK_S3_ENABLE_SD_CARD
    if (!m_sdCardMounted) return 0;

    FATFS* fs;
    DWORD fre_clust;
    
    if (f_getfree(CONFIG_EBOOK_S3_SD_MOUNT_POINT, &fre_clust, &fs) != FR_OK) {
        return 0;
    }
    
    uint64_t freeBytes = (uint64_t)fre_clust * fs->csize * fs->ssize;
    return freeBytes;
#else
    return 0;
#endif
}

bool DeviceHAL::formatSDCard(std::function<void(int)> progressCallback) {
#ifdef CONFIG_EBOOK_S3_ENABLE_SD_CARD
    if (!hasSDCardSlot()) return false;

    ESP_LOGW(TAG, "Formatting SD card...");
    
    // Unmount first if mounted
    if (m_sdCardMounted) {
        unmountSDCard();
    }

    if (progressCallback) progressCallback(10);

    // Mount with format_if_mount_failed = true to trigger format
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SPI2_HOST;
    host.max_freq_khz = 20000;

    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.gpio_cs = M5PAPERS3_SD_CS_PIN;
    slot_config.host_id = (spi_host_device_t)host.slot;

    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = true,  // This will format!
        .max_files = 10,
        .allocation_unit_size = 16 * 1024,
        .disk_status_check_enable = false
    };

    if (progressCallback) progressCallback(30);

    // First, we need to try to force-unmount and reinitialize
    // Use a temporary card pointer
    sdmmc_card_t* tempCard = nullptr;

    // Re-initialize SPI bus (it might already be initialized)
    spi_bus_config_t bus_cfg = {
        .mosi_io_num = M5PAPERS3_SD_MOSI_PIN,
        .miso_io_num = M5PAPERS3_SD_MISO_PIN,
        .sclk_io_num = M5PAPERS3_SD_CLK_PIN,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4096,
        .flags = SPICOMMON_BUSFLAG_MASTER,
        .isr_cpu_id = ESP_INTR_CPU_AFFINITY_AUTO,
        .intr_flags = 0
    };
    spi_bus_initialize((spi_host_device_t)host.slot, &bus_cfg, SPI_DMA_CH_AUTO);

    if (progressCallback) progressCallback(50);

    esp_err_t ret = esp_vfs_fat_sdspi_mount(
        CONFIG_EBOOK_S3_SD_MOUNT_POINT,
        &host,
        &slot_config,
        &mount_config,
        &tempCard
    );

    if (progressCallback) progressCallback(90);

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to format/mount SD card: %s", esp_err_to_name(ret));
        return false;
    }

    s_card = tempCard;
    m_sdCardMounted = true;

    if (progressCallback) progressCallback(100);

    ESP_LOGI(TAG, "SD card formatted and mounted successfully");
    return true;
#else
    (void)progressCallback;
    return false;
#endif
}

// ==================== Power & Sleep ====================

gpio_num_t DeviceHAL::getTouchInterruptPin() const {
#ifdef CONFIG_EBOOK_DEVICE_M5PAPERS3
    return M5PAPERS3_TOUCH_INT_PIN;
#else
    return M5PAPER_TOUCH_INT_PIN;
#endif
}

gpio_num_t DeviceHAL::getMainPowerPin() const {
#ifdef CONFIG_EBOOK_DEVICE_M5PAPERS3
    return M5PAPERS3_MAIN_PWR_PIN;
#else
    return M5PAPER_MAIN_PWR_PIN;
#endif
}

void DeviceHAL::enterDeepSleepWithTouchWake() {
    gpio_num_t touchPin = getTouchInterruptPin();
    gpio_num_t pwrPin = getMainPowerPin();

    ESP_LOGI(TAG, "Preparing for deep sleep on %s...", getDeviceName());

    // Clear any pending touch interrupt
    gpio_reset_pin(touchPin);
    gpio_set_direction(touchPin, GPIO_MODE_INPUT);

    // Wait for line to go HIGH (inactive) - it is active LOW
    int retries = 0;
    while (gpio_get_level(touchPin) == 0 && retries < 50) {
        M5.update();
        vTaskDelay(50 / portTICK_PERIOD_MS);
        retries++;
    }

    if (gpio_get_level(touchPin) == 0) {
        ESP_LOGW(TAG, "Touch interrupt pin still LOW after flushing");
    }

    // Configure wakeup on touch interrupt (active LOW)
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
    esp_sleep_enable_ext0_wakeup(touchPin, 0);

    // Keep main power rail ON during deep sleep
#ifdef CONFIG_EBOOK_DEVICE_M5PAPER
    gpio_reset_pin(pwrPin);
    gpio_set_direction(pwrPin, GPIO_MODE_OUTPUT);
    gpio_set_level(pwrPin, 1);
    gpio_hold_en(pwrPin);
    gpio_deep_sleep_hold_en();
#endif

    // Put display to sleep
    M5.Display.sleep();
    M5.Display.waitDisplay();

    ESP_LOGI(TAG, "Entering deep sleep now");
    esp_deep_sleep_start();
}

void DeviceHAL::enterDeepSleepShutdown() {
    ESP_LOGI(TAG, "Entering deep sleep shutdown on %s...", getDeviceName());

    gpio_num_t pwrPin = getMainPowerPin();

#ifdef CONFIG_EBOOK_DEVICE_M5PAPER
    // Turn OFF main power rail
    gpio_hold_dis(pwrPin);
    gpio_reset_pin(pwrPin);
    gpio_set_direction(pwrPin, GPIO_MODE_OUTPUT);
    gpio_set_level(pwrPin, 0);
    gpio_hold_en(pwrPin);
    gpio_deep_sleep_hold_en();

    // Wake on button press
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
    const uint64_t BUTTON_MASK = (1ULL << 38);
    esp_sleep_enable_ext1_wakeup(BUTTON_MASK, ESP_EXT1_WAKEUP_ALL_LOW);
#endif

#ifdef CONFIG_EBOOK_DEVICE_M5PAPERS3
    // M5PaperS3 power management
    M5.Display.sleep();
    M5.Display.waitDisplay();
    
    // Configure wake source for S3
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
    esp_sleep_enable_ext0_wakeup(getTouchInterruptPin(), 0);
#endif

    esp_deep_sleep_start();
}

// ==================== Task Pinning ====================

int DeviceHAL::getRenderTaskCore() const {
#ifdef CONFIG_EBOOK_S3_DUAL_CORE_OPTIMIZATION
    return CONFIG_EBOOK_S3_RENDER_TASK_CORE;
#else
    return 0;
#endif
}

int DeviceHAL::getMainTaskCore() const {
#ifdef CONFIG_EBOOK_S3_DUAL_CORE_OPTIMIZATION
    return CONFIG_EBOOK_S3_MAIN_TASK_CORE;
#else
    return 0;
#endif
}

bool DeviceHAL::isDualCoreOptimized() const {
#ifdef CONFIG_EBOOK_S3_DUAL_CORE_OPTIMIZATION
    return true;
#else
    return false;
#endif
}
