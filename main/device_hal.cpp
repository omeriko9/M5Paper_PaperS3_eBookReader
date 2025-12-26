#include "device_hal.h"
#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_sleep.h"
#include "driver/gpio.h"
#include "driver/rtc_io.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#ifdef CONFIG_EBOOK_DEVICE_M5PAPERS3
#include "driver/ledc.h"
#endif

#ifdef CONFIG_EBOOK_ENABLE_SD_CARD
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdspi_host.h"
#endif

static const char *TAG = "DeviceHAL";

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
static constexpr gpio_num_t M5PAPERS3_TOUCH_INT_PIN = GPIO_NUM_48; // GT911 touch interrupt
static constexpr gpio_num_t M5PAPERS3_MAIN_PWR_PIN = GPIO_NUM_44;  // PWROFF_PULSE_PIN
static constexpr gpio_num_t M5PAPERS3_BUZZER_PIN = GPIO_NUM_21;
static constexpr gpio_num_t M5PAPERS3_SD_CS_PIN = GPIO_NUM_47;
static constexpr gpio_num_t M5PAPERS3_SD_MOSI_PIN = GPIO_NUM_38;
static constexpr gpio_num_t M5PAPERS3_SD_MISO_PIN = GPIO_NUM_40;
static constexpr gpio_num_t M5PAPERS3_SD_CLK_PIN = GPIO_NUM_39;

// LEDC configuration for buzzer
static constexpr ledc_timer_t BUZZER_TIMER = LEDC_TIMER_0;
static constexpr ledc_channel_t BUZZER_CHANNEL = LEDC_CHANNEL_0;
static constexpr ledc_mode_t BUZZER_MODE = LEDC_LOW_SPEED_MODE;
#endif

#ifdef CONFIG_EBOOK_ENABLE_SD_CARD
static sdmmc_card_t *s_card = nullptr;
#endif

DeviceHAL &DeviceHAL::getInstance()
{
    static DeviceHAL instance;
    return instance;
}

DeviceHAL::DeviceHAL()
{
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

void DeviceHAL::init(bool isWakeFromSleep)
{
    if (m_initialized)
        return;

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
        .deconfigure = false};
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
            .output_invert = 0}};
    ledc_channel_config(&channel_conf);
#endif

    // Initialize IMU for auto-rotation
#ifdef CONFIG_EBOOK_S3_ENABLE_GYROSCOPE
    if (M5.Imu.isEnabled())
    {
        ESP_LOGI(TAG, "IMU initialized for auto-rotation");
    }
    else
    {
        ESP_LOGW(TAG, "IMU not available");
    }
#endif

#endif // CONFIG_EBOOK_DEVICE_M5PAPERS3

    m_initialized = true;
    ESP_LOGI(TAG, "DeviceHAL initialized");
}

bool DeviceHAL::isM5PaperS3() const
{
#ifdef CONFIG_EBOOK_DEVICE_M5PAPERS3
    return true;
#else
    return false;
#endif
}

bool DeviceHAL::isM5Paper() const
{
#ifdef CONFIG_EBOOK_DEVICE_M5PAPER
    return true;
#else
    return false;
#endif
}

const char *DeviceHAL::getDeviceName() const
{
#ifdef CONFIG_EBOOK_DEVICE_M5PAPERS3
    return "M5PaperS3";
#else
    return "M5Paper";
#endif
}

int DeviceHAL::getDisplayWidth() const
{
    return M5.Display.width();
}

int DeviceHAL::getDisplayHeight() const
{
    return M5.Display.height();
}

int DeviceHAL::getCanvasColorDepth() const
{
#ifdef CONFIG_EBOOK_CANVAS_COLOR_DEPTH
    return CONFIG_EBOOK_CANVAS_COLOR_DEPTH;
#elif defined(CONFIG_EBOOK_DEVICE_M5PAPERS3) && defined(CONFIG_EBOOK_S3_16_GRAYSCALE)
    return 4; // 16 shades
#else
    return 2; // 4 shades
#endif
}

int DeviceHAL::getRotation() const
{
    return m_currentRotation;
}

void DeviceHAL::setRotation(int rotation)
{
    if (rotation < 0 || rotation > 3)
        return;
    if (m_currentRotation == rotation)
        return;

    m_currentRotation = rotation;
    M5.Display.setRotation(rotation);
    ESP_LOGI(TAG, "Display rotation set to %d", rotation);
}

bool DeviceHAL::isLandscape() const
{
    return (m_currentRotation == 1 || m_currentRotation == 3);
}

// ==================== Buzzer ====================

#ifdef CONFIG_EBOOK_DEVICE_M5PAPERS3
void DeviceHAL::buzzerStopTimerCb(void *arg)
{
    auto *self = static_cast<DeviceHAL *>(arg);
    if (self)
    {
        self->stopTone();
    }
}

void DeviceHAL::ensureBuzzerTimer()
{
#ifdef CONFIG_EBOOK_S3_ENABLE_BUZZER
    if (m_buzzerStopTimer)
        return;

    const esp_timer_create_args_t timer_args = {
        .callback = &DeviceHAL::buzzerStopTimerCb,
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "buzzer_stop"};

    esp_err_t err = esp_timer_create(&timer_args, &m_buzzerStopTimer);
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "Failed to create buzzer stop timer: %s", esp_err_to_name(err));
        m_buzzerStopTimer = nullptr;
    }
#endif
}

void DeviceHAL::stopTone()
{
#ifdef CONFIG_EBOOK_S3_ENABLE_BUZZER
    // Stop tone by setting duty cycle to 0
    ledc_set_duty(BUZZER_MODE, BUZZER_CHANNEL, 0);
    ledc_update_duty(BUZZER_MODE, BUZZER_CHANNEL);
#endif
}

void DeviceHAL::playToneAsync(int frequency, int duration)
{
#ifdef CONFIG_EBOOK_S3_ENABLE_BUZZER
    if (!m_buzzerEnabled)
        return;

    ensureBuzzerTimer();
    if (m_buzzerStopTimer)
    {
        esp_timer_stop(m_buzzerStopTimer);
    }

    // Set frequency
    ledc_set_freq(BUZZER_MODE, BUZZER_TIMER, frequency);

    // Start tone (50% duty)
    ledc_set_duty(BUZZER_MODE, BUZZER_CHANNEL, 512);
    ledc_update_duty(BUZZER_MODE, BUZZER_CHANNEL);

    // Schedule stop
    if (m_buzzerStopTimer)
    {
        esp_timer_start_once(m_buzzerStopTimer, (uint64_t)duration * 1000ULL);
    }
#else
    (void)frequency;
    (void)duration;
#endif
}
#endif

bool DeviceHAL::hasBuzzer() const
{
#ifdef CONFIG_EBOOK_S3_ENABLE_BUZZER
    return true;
#else
    return false;
#endif
}

void DeviceHAL::playClickSound()
{
#ifdef CONFIG_EBOOK_S3_ENABLE_BUZZER
    if (!m_buzzerEnabled)
        return;
    // Non-blocking click so UI responsiveness isn't impacted.
    playToneAsync(CONFIG_EBOOK_S3_BUZZER_FREQUENCY, CONFIG_EBOOK_S3_BUZZER_DURATION_MS);
#endif
}

void DeviceHAL::playTone(int frequency, int duration)
{
#ifdef CONFIG_EBOOK_S3_ENABLE_BUZZER
    if (!m_buzzerEnabled)
        return;

    // Cancel any pending async stop so the blocking tone controls start/stop.
    ensureBuzzerTimer();
    if (m_buzzerStopTimer)
    {
        esp_timer_stop(m_buzzerStopTimer);
    }

    // Set frequency
    ledc_set_freq(BUZZER_MODE, BUZZER_TIMER, frequency);

    // Set duty cycle to 50% for audible tone
    ledc_set_duty(BUZZER_MODE, BUZZER_CHANNEL, 512);
    ledc_update_duty(BUZZER_MODE, BUZZER_CHANNEL);

    // Wait for duration
    vTaskDelay(pdMS_TO_TICKS(duration));

    stopTone();
#else
    (void)frequency;
    (void)duration;
#endif
}


// void DeviceHAL::playStartupSound()
// {
// #ifdef CONFIG_EBOOK_S3_ENABLE_BUZZER
//     if (!m_buzzerEnabled)
//         return;
//     // Ascending sequence
//     playTone(1000, 100);
//     vTaskDelay(pdMS_TO_TICKS(50));
//     playTone(1500, 100);
//     vTaskDelay(pdMS_TO_TICKS(50));
//     playTone(2000, 100);
// #endif
// }

void DeviceHAL::playStartupSound()
{
#ifdef CONFIG_EBOOK_S3_ENABLE_BUZZER
    if (!m_buzzerEnabled)
        return;

    const int note = 35;
    const int gap  = 25;

    // Major up arpeggio: root -> 3rd -> 5th -> octave
    playTone(880, note);   vTaskDelay(pdMS_TO_TICKS(gap)); // A
    //playTone(1109, note);  vTaskDelay(pdMS_TO_TICKS(gap)); // C#
    playTone(1319, note);  vTaskDelay(pdMS_TO_TICKS(gap)); // E
    //playTone(1568, note);  vTaskDelay(pdMS_TO_TICKS(gap)); // G
    //playTone(1865, note);  vTaskDelay(pdMS_TO_TICKS(gap)); // Bb
    playTone(1760, note);  vTaskDelay(pdMS_TO_TICKS(gap));  // A6
    playTone(2637, note);  vTaskDelay(pdMS_TO_TICKS(gap));  // E7
    //playTone(3520, note);  vTaskDelay(pdMS_TO_TICKS(gap));  // A7
    
#endif
}


void DeviceHAL::playShutdownSound()
{
#ifdef CONFIG_EBOOK_S3_ENABLE_BUZZER
    if (!m_buzzerEnabled)
        return;
    // Descending sequence ("bye bye")
    playTone(2000, 100);
    vTaskDelay(pdMS_TO_TICKS(50));
    playTone(1500, 100);
    vTaskDelay(pdMS_TO_TICKS(50));
    playTone(1000, 100);
#endif
}

void DeviceHAL::setBuzzerEnabled(bool enabled)
{
    m_buzzerEnabled = enabled;
    ESP_LOGI(TAG, "Buzzer %s", enabled ? "enabled" : "disabled");
}

bool DeviceHAL::isBuzzerEnabled() const
{
    return m_buzzerEnabled;
}

// ==================== Gyroscope/IMU ====================

bool DeviceHAL::hasGyroscope() const
{
#ifdef CONFIG_EBOOK_S3_ENABLE_GYROSCOPE
    return M5.Imu.isEnabled();
#else
    return false;
#endif
}

bool DeviceHAL::isAutoRotateEnabled() const
{
    return m_autoRotateEnabled;
}

void DeviceHAL::setAutoRotateEnabled(bool enabled)
{
    m_autoRotateEnabled = enabled;
    ESP_LOGI(TAG, "Auto-rotate %s", enabled ? "enabled" : "disabled");
}

bool DeviceHAL::updateOrientation()
{
#ifdef CONFIG_EBOOK_S3_ENABLE_GYROSCOPE
    if (!m_autoRotateEnabled || !hasGyroscope())
    {
        return false;
    }

    float ax, ay, az;
    getAcceleration(ax, ay, az);

    // Determine orientation based on accelerometer
    int newRotation = m_currentRotation;

    // Swapped logic as per user request (Landscape <-> Portrait)
    if (ay > TILT_THRESHOLD)
    {
        newRotation = 1; // Landscape (left)
    }
    else if (ay < -TILT_THRESHOLD)
    {
        newRotation = 3; // Landscape (right)
    }
    else if (ax > TILT_THRESHOLD)
    {
        newRotation = 0; // Portrait (normal)
    }
    else if (ax < -TILT_THRESHOLD)
    {
        newRotation = 2; // Portrait (upside down)
    }

    // Debounce rotation changes
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);

    if (newRotation != m_lastStableRotation)
    {
        m_lastStableRotation = newRotation;
        m_rotationChangeTime = now;
        return false;
    }

    if (newRotation != m_currentRotation &&
        (now - m_rotationChangeTime) > ROTATION_DEBOUNCE_MS)
    {
        setRotation(newRotation);
        return true;
    }
#endif
    return false;
}

void DeviceHAL::getAcceleration(float &x, float &y, float &z)
{
#ifdef CONFIG_EBOOK_S3_ENABLE_GYROSCOPE
    if (M5.Imu.isEnabled())
    {
        M5.Imu.getAccel(&x, &y, &z);
    }
    else
    {
        x = y = z = 0.0f;
    }
#else
    x = y = z = 0.0f;
#endif
}

// ==================== SD Card ====================

bool DeviceHAL::hasSDCardSlot() const
{
#ifdef CONFIG_EBOOK_ENABLE_SD_CARD
    return true;
#else
    return false;
#endif
}

bool DeviceHAL::isSDCardMounted() const
{
    return m_sdCardMounted;
}

bool DeviceHAL::mountSDCard()
{
#ifdef CONFIG_EBOOK_ENABLE_SD_CARD
    if (m_sdCardMounted)
    {
        ESP_LOGI(TAG, "SD card already mounted");
        return true;
    }

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    spi_bus_config_t bus_cfg = {};
    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    int mosi, miso, clk, cs;
    spi_host_device_t host_id;

#ifdef CONFIG_EBOOK_DEVICE_M5PAPERS3
    mosi = M5PAPERS3_SD_MOSI_PIN;
    miso = M5PAPERS3_SD_MISO_PIN;
    clk = M5PAPERS3_SD_CLK_PIN;
    cs = M5PAPERS3_SD_CS_PIN;
    host_id = SPI3_HOST;
    host.max_freq_khz = 20000;
#else
    mosi = M5PAPER_SD_MOSI_PIN;
    miso = M5PAPER_SD_MISO_PIN;
    clk = M5PAPER_SD_CLK_PIN;
    cs = M5PAPER_SD_CS_PIN;
    host_id = SPI3_HOST; // Share VSPI with EPD to avoid pin mux conflicts.
    host.max_freq_khz = 20000;
#endif

    ESP_LOGI(TAG, "Mounting SD card on pins CS=%d, CLK=%d, MOSI=%d, MISO=%d, Host=%d",
             cs, clk, mosi, miso, host_id);

    host.slot = host_id;

    bus_cfg.mosi_io_num = mosi;
    bus_cfg.miso_io_num = miso;
    bus_cfg.sclk_io_num = clk;
    bus_cfg.quadwp_io_num = -1;
    bus_cfg.quadhd_io_num = -1;
    bus_cfg.max_transfer_sz = 4096;
    bus_cfg.flags = SPICOMMON_BUSFLAG_MASTER;
    bus_cfg.isr_cpu_id = ESP_INTR_CPU_AFFINITY_AUTO;
    bus_cfg.intr_flags = 0;

    esp_err_t ret = spi_bus_initialize(host_id, &bus_cfg, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE)
    {
        ESP_LOGE(TAG, "Failed to initialize SPI bus: %s", esp_err_to_name(ret));
        return false;
    }
    ESP_LOGI(TAG, "SPI bus initialized");

    slot_config.gpio_cs = (gpio_num_t)cs;
    slot_config.host_id = host_id;

    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 10,
        .allocation_unit_size = 16 * 1024,
        .disk_status_check_enable = false};

    ESP_LOGI(TAG, "Attempting SD card mount...");
    ret = esp_vfs_fat_sdspi_mount(
        CONFIG_EBOOK_SD_MOUNT_POINT,
        &host,
        &slot_config,
        &mount_config,
        &s_card);

    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to mount SD card: %s", esp_err_to_name(ret));
        return false;
    }

    m_sdCardMounted = true;
    ESP_LOGI(TAG, "SD card mounted at %s", CONFIG_EBOOK_SD_MOUNT_POINT);

    if (s_card)
    {
        sdmmc_card_print_info(stdout, s_card);
    }

    return true;
#else
    return false;
#endif
}

void DeviceHAL::unmountSDCard()
{
#ifdef CONFIG_EBOOK_ENABLE_SD_CARD
    if (!m_sdCardMounted)
        return;

    esp_vfs_fat_sdcard_unmount(CONFIG_EBOOK_SD_MOUNT_POINT, s_card);
    s_card = nullptr;
    m_sdCardMounted = false;
    ESP_LOGI(TAG, "SD card unmounted");
#endif
}

const char *DeviceHAL::getSDCardMountPoint() const
{
#ifdef CONFIG_EBOOK_ENABLE_SD_CARD
    return CONFIG_EBOOK_SD_MOUNT_POINT;
#else
    return nullptr;
#endif
}

uint64_t DeviceHAL::getSDCardTotalSize() const
{
#ifdef CONFIG_EBOOK_ENABLE_SD_CARD
    if (!m_sdCardMounted)
        return 0;

    uint64_t totalBytes = 0;
    uint64_t freeBytes = 0;
    if (esp_vfs_fat_info(CONFIG_EBOOK_SD_MOUNT_POINT, &totalBytes, &freeBytes) == ESP_OK)
    {
        return totalBytes;
    }
    return 0;
#else
    return 0;
#endif
}

uint64_t DeviceHAL::getSDCardFreeSize() const
{
#ifdef CONFIG_EBOOK_ENABLE_SD_CARD
    if (!m_sdCardMounted)
        return 0;

    uint64_t totalBytes = 0;
    uint64_t freeBytes = 0;
    if (esp_vfs_fat_info(CONFIG_EBOOK_SD_MOUNT_POINT, &totalBytes, &freeBytes) == ESP_OK)
    {
        return freeBytes;
    }
    return 0;
#else
    return 0;
#endif
}

bool DeviceHAL::formatSDCard(std::function<void(int)> progressCallback)
{
#ifdef CONFIG_EBOOK_ENABLE_SD_CARD
    if (!hasSDCardSlot())
        return false;

    ESP_LOGW(TAG, "Formatting SD card...");

    // Unmount first if mounted
    if (m_sdCardMounted)
    {
        unmountSDCard();
    }

    if (progressCallback)
        progressCallback(10);

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    spi_bus_config_t bus_cfg = {};
    int mosi, miso, clk, cs;
    spi_host_device_t host_id;

#ifdef CONFIG_EBOOK_DEVICE_M5PAPERS3
    mosi = M5PAPERS3_SD_MOSI_PIN;
    miso = M5PAPERS3_SD_MISO_PIN;
    clk = M5PAPERS3_SD_CLK_PIN;
    cs = M5PAPERS3_SD_CS_PIN;
    host_id = SPI3_HOST;
    host.max_freq_khz = 20000;
#else
    mosi = M5PAPER_SD_MOSI_PIN;
    miso = M5PAPER_SD_MISO_PIN;
    clk = M5PAPER_SD_CLK_PIN;
    cs = M5PAPER_SD_CS_PIN;
    host_id = SPI3_HOST; // Share VSPI with EPD to avoid pin mux conflicts.
    host.max_freq_khz = 20000;
#endif

    host.slot = host_id;
    slot_config.gpio_cs = (gpio_num_t)cs;
    slot_config.host_id = host_id;

    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = true, // This will format!
        .max_files = 10,
        .allocation_unit_size = 16 * 1024,
        .disk_status_check_enable = false};

    if (progressCallback)
        progressCallback(30);

    sdmmc_card_t *tempCard = nullptr;

    // Re-initialize SPI bus (it might already be initialized)
    bus_cfg.mosi_io_num = mosi;
    bus_cfg.miso_io_num = miso;
    bus_cfg.sclk_io_num = clk;
    bus_cfg.quadwp_io_num = -1;
    bus_cfg.quadhd_io_num = -1;
    bus_cfg.max_transfer_sz = 4096;
    bus_cfg.flags = SPICOMMON_BUSFLAG_MASTER;
    bus_cfg.isr_cpu_id = ESP_INTR_CPU_AFFINITY_AUTO;
    bus_cfg.intr_flags = 0;

    spi_bus_initialize(host_id, &bus_cfg, SPI_DMA_CH_AUTO);

    if (progressCallback)
        progressCallback(50);

    esp_err_t ret = esp_vfs_fat_sdspi_mount(
        CONFIG_EBOOK_SD_MOUNT_POINT,
        &host,
        &slot_config,
        &mount_config,
        &tempCard);

    if (progressCallback)
        progressCallback(90);

    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to format/mount SD card: %s", esp_err_to_name(ret));
        return false;
    }

    s_card = tempCard;
    m_sdCardMounted = true;

    if (progressCallback)
        progressCallback(100);

    ESP_LOGI(TAG, "SD card formatted and mounted successfully");
    return true;
#else
    (void)progressCallback;
    return false;
#endif
}

// ==================== Power & Sleep ====================

gpio_num_t DeviceHAL::getTouchInterruptPin() const
{
#ifdef CONFIG_EBOOK_DEVICE_M5PAPERS3
    return M5PAPERS3_TOUCH_INT_PIN;
#else
    return M5PAPER_TOUCH_INT_PIN;
#endif
}

gpio_num_t DeviceHAL::getMainPowerPin() const
{
#ifdef CONFIG_EBOOK_DEVICE_M5PAPERS3
    return M5PAPERS3_MAIN_PWR_PIN;
#else
    return M5PAPER_MAIN_PWR_PIN;
#endif
}

void DeviceHAL::enterDeepSleepWithTouchWake()
{
    gpio_num_t touchPin = getTouchInterruptPin();
    gpio_num_t pwrPin = getMainPowerPin();

    ESP_LOGI(TAG, "Preparing for deep sleep on %s...", getDeviceName());

    // Clear any pending touch interrupt
    gpio_reset_pin(touchPin);
    gpio_set_direction(touchPin, GPIO_MODE_INPUT);

    // Wait for line to go HIGH (inactive) - it is active LOW
    int retries = 0;
    while (gpio_get_level(touchPin) == 0 && retries < 50)
    {
        M5.update();
        vTaskDelay(50 / portTICK_PERIOD_MS);
        retries++;
    }

    if (gpio_get_level(touchPin) == 0)
    {
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
    fflush(stdout);
    vTaskDelay(100 / portTICK_PERIOD_MS); // allow time for logs to flush
    esp_deep_sleep_start();
}

void DeviceHAL::enterDeepSleepShutdown()
{
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

    // Configure wake source for S3: Wake on button (GPIO 0)
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
    esp_sleep_enable_ext0_wakeup(GPIO_NUM_0, 0);
#endif

    fflush(stdout);
    vTaskDelay(100 / portTICK_PERIOD_MS); // allow time for logs to flush
    esp_deep_sleep_start();
}

// ==================== Task Pinning ====================

int DeviceHAL::getRenderTaskCore() const
{
#ifdef CONFIG_EBOOK_S3_DUAL_CORE_OPTIMIZATION
    return CONFIG_EBOOK_S3_RENDER_TASK_CORE;
#else
    return 0;
#endif
}

int DeviceHAL::getMainTaskCore() const
{
#ifdef CONFIG_EBOOK_S3_DUAL_CORE_OPTIMIZATION
    return CONFIG_EBOOK_S3_MAIN_TASK_CORE;
#else
    return 0;
#endif
}

bool DeviceHAL::isDualCoreOptimized() const
{
#ifdef CONFIG_EBOOK_S3_DUAL_CORE_OPTIMIZATION
    return true;
#else
    return false;
#endif
}
