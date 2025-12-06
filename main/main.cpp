#include "nvs_flash.h"
#include "nvs.h"
#include "esp_spiffs.h"
#include "esp_log.h"
#include "esp_system.h"
#include "M5Unified.h"
#include "wifi_manager.h"
#include "web_server.h"
#include "gui.h"
#include "book_index.h"
#include "esp_vfs_fat.h"
#include "driver/sdspi_host.h"
#include "sdmmc_cmd.h"
#include <dirent.h>
#include <errno.h>
#include "driver/gpio.h"
#include <time.h>
#include "esp_sleep.h"

#if __has_include(<esp_sntp.h>)
#include "esp_sntp.h"
#define SNTP_CLIENT_AVAILABLE 1
#elif __has_include(<sntp.h>)
#include "sntp.h"
#define SNTP_CLIENT_AVAILABLE 1
#else
#define SNTP_CLIENT_AVAILABLE 0
#endif

#if SNTP_CLIENT_AVAILABLE
// Provide a weak stub for SDK variants that lack sntp_stop; strong symbol (if present) will override.
extern "C" void sntp_stop(void) __attribute__((weak));
extern "C" void sntp_stop(void) {}
#endif

static const char *TAG = "MAIN";

WifiManager wifiManager;
WebServer webServer;
GUI gui;
BookIndex bookIndex; // Global instance

static inline void stopSntpClient()
{
#if SNTP_CLIENT_AVAILABLE
    sntp_stop();
#endif
}

void syncRtcFromNtp()
{
    // Load TZ from NVS
    char tz[64] = {0};
    nvs_handle_t my_handle;
    esp_err_t err = nvs_open("storage", NVS_READONLY, &my_handle);
    if (err == ESP_OK) {
        size_t required_size = sizeof(tz);
        if (nvs_get_str(my_handle, "timezone", tz, &required_size) == ESP_OK) {
            ESP_LOGI(TAG, "Loaded timezone from NVS: %s", tz);
            setenv("TZ", tz, 1);
            tzset();
        } else {
            // Default to Jerusalem if not set
            const char* defaultTz = "IST-2IDT,M3.4.4/26,M10.5.0";
            ESP_LOGI(TAG, "Timezone not set in NVS, using default: %s", defaultTz);
            setenv("TZ", defaultTz, 1);
            tzset();
        }
        nvs_close(my_handle);
    } else {
         // Default to Jerusalem if NVS fails
        const char* defaultTz = "IST-2IDT,M3.4.4/26,M10.5.0";
        ESP_LOGI(TAG, "NVS open failed, using default timezone: %s", defaultTz);
        setenv("TZ", defaultTz, 1);
        tzset();
    }

    // Configure SNTP
    sntp_setoperatingmode(SNTP_OPMODE_POLL);
    sntp_setservername(0, "pool.ntp.org");
    sntp_init();

    time_t now = 0;
    struct tm timeinfo = {0};
    const int maxRetries = 20;

    // Wait for time to be set
    for (int i = 0; i < maxRetries; ++i)
    {
        // Check if SNTP has synced
        if (sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED)
        {
            break;
        }

        time(&now);
        localtime_r(&now, &timeinfo);
        if (timeinfo.tm_year > (2020 - 1900))
        {
            break;
        }
        ESP_LOGI(TAG, "Waiting for NTP sync... (%d/%d)", i + 1, maxRetries);
        vTaskDelay(1000 / portTICK_PERIOD_MS);
    }

    time(&now);
    localtime_r(&now, &timeinfo);

    ESP_LOGI(TAG, "NTP Sync Debug: UTC Time: %ld", now);
    ESP_LOGI(TAG, "NTP Sync Debug: Local Time: %04d-%02d-%02d %02d:%02d:%02d",
             timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday,
             timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);

    if (timeinfo.tm_year > (2020 - 1900))
    {
        M5.Rtc.setDateTime(&timeinfo);
        ESP_LOGI(TAG, "RTC synced from NTP: %04d-%02d-%02d %02d:%02d:%02d",
                 timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday,
                 timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
    }
    else
    {
        ESP_LOGW(TAG, "Failed to sync RTC from NTP");
    }

    // Stop SNTP to avoid background polling
    stopSntpClient();
}

extern "C" void app_main(void)
{
    // Check wake reason FIRST
    auto wakeup_reason = esp_sleep_get_wakeup_cause();

    // Handle Timer Wakeup (Stage 1 -> Stage 2 transition)
    if (wakeup_reason == ESP_SLEEP_WAKEUP_TIMER)
    {
        ESP_LOGI(TAG, "Woke from Stage 1 Deep Sleep (Timer) - Entering Stage 2 Shutdown");
        // We need to initialize M5 to access board info inside enterDeepSleepShutdown, 
        // but we can skip display init to save time/power.
        // Actually, enterDeepSleepShutdown checks M5.getBoard().
        // M5.begin() is needed? M5.getBoard() might work if we just call M5.begin() minimally.
        // But to be safe and simple:
        M5.begin(); 
        GUI::enterDeepSleepShutdown();
        return; // Should not reach here
    }

    bool is_wake_from_sleep = (wakeup_reason == ESP_SLEEP_WAKEUP_EXT0 || wakeup_reason == ESP_SLEEP_WAKEUP_EXT1);
    
    if (is_wake_from_sleep) {
        ESP_LOGI(TAG, "Woke from deep sleep - fast path");
    } else {
        ESP_LOGI(TAG, "Cold boot - full initialization");
    }
    
    // Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Initialize SD Card BEFORE M5Unified to avoid SPI conflicts
    // M5Paper SD card pins: MOSI=12, MISO=13, CLK=14, CS=4
    // ESP_LOGI(TAG, "Initializing SD card");

    // // Power cycle the CS line to reset the card
    // gpio_set_direction(GPIO_NUM_4, GPIO_MODE_OUTPUT);
    // gpio_set_level(GPIO_NUM_4, 0);
    // vTaskDelay(10 / portTICK_PERIOD_MS);
    // gpio_set_level(GPIO_NUM_4, 1);
    // vTaskDelay(100 / portTICK_PERIOD_MS);

    // // Configure GPIO pullups for better signal integrity
    // gpio_set_pull_mode(GPIO_NUM_12, GPIO_PULLUP_ONLY); // MOSI
    // gpio_set_pull_mode(GPIO_NUM_13, GPIO_PULLUP_ONLY); // MISO
    // gpio_set_pull_mode(GPIO_NUM_14, GPIO_PULLUP_ONLY); // CLK
    // gpio_set_pull_mode(GPIO_NUM_4, GPIO_PULLUP_ONLY);  // CS

    // sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    // host.slot = SPI2_HOST;          // Use SPI2 to avoid conflict with M5Unified's SPI3
    // host.max_freq_khz = 400;        // SD card standard init frequency
    // host.command_timeout_ms = 5000; // Long timeout for slow cards
    // host.io_voltage = 3.3f;         // M5Paper uses 3.3V

    // spi_bus_config_t bus_cfg = {
    //     .mosi_io_num = GPIO_NUM_12,
    //     .miso_io_num = GPIO_NUM_13,
    //     .sclk_io_num = GPIO_NUM_14,
    //     .quadwp_io_num = -1,
    //     .quadhd_io_num = -1,
    //     .max_transfer_sz = 4000,
    //     .flags = SPICOMMON_BUSFLAG_MASTER,
    // };

    // ret = spi_bus_initialize((spi_host_device_t)host.slot, &bus_cfg, SPI_DMA_CH_AUTO);
    // if (ret != ESP_OK)
    // {
    //     ESP_LOGE(TAG, "Failed to initialize SPI bus: %s", esp_err_to_name(ret));
    // }
    // else
    // {
    //     ESP_LOGI(TAG, "SPI bus initialized successfully");

    //     // Long delay for card stabilization
    //     vTaskDelay(500 / portTICK_PERIOD_MS);

    //     sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    //     slot_config.gpio_cs = GPIO_NUM_4;
    //     slot_config.host_id = (spi_host_device_t)host.slot;

    //     esp_vfs_fat_sdmmc_mount_config_t mount_config = {
    //         .format_if_mount_failed = false,
    //         .max_files = 5,
    //         .allocation_unit_size = 0,        // Auto-detect
    //         .disk_status_check_enable = false // Disable status check for compatibility
    //     };

    //     ESP_LOGI(TAG, "Attempting to mount SD card...");
    //     sdmmc_card_t *card = NULL;
    //     ret = esp_vfs_fat_sdspi_mount("/sd", &host, &slot_config, &mount_config, &card);

    //     if (ret == ESP_OK)
    //     {
    //         ESP_LOGI(TAG, "SD Card mounted at /sd");

    //         if (card)
    //         {
    //             sdmmc_card_print_info(stdout, card);
    //             ESP_LOGI(TAG, "Card capacity: %llu MB", ((uint64_t)card->csd.capacity) * card->csd.sector_size / (1024 * 1024));
    //         }

    //         DIR *dir = opendir("/sd");
    //         if (dir)
    //         {
    //             ESP_LOGI(TAG, "/sd directory is accessible");

    //             // List files to verify
    //             struct dirent *entry;
    //             int file_count = 0;
    //             while ((entry = readdir(dir)) != NULL && file_count < 5)
    //             {
    //                 ESP_LOGI(TAG, "  - %s", entry->d_name);
    //                 file_count++;
    //             }
    //             if (file_count > 0)
    //             {
    //                 ESP_LOGI(TAG, "SD card has %d+ files", file_count);
    //             }
    //             else
    //             {
    //                 ESP_LOGI(TAG, "SD card is empty or root dir has no files");
    //             }

    //             closedir(dir);
    //         }
    //         else
    //         {
    //             ESP_LOGW(TAG, "/sd directory NOT accessible: %s (errno: %d)", strerror(errno), errno);
    //         }
    //     }
    //     else
    //     {
    //         ESP_LOGE(TAG, "Failed to mount SD Card: %s (0x%x)", esp_err_to_name(ret), ret);
    //         if (ret == ESP_FAIL)
    //         {
    //             ESP_LOGE(TAG, "This usually means:");
    //             ESP_LOGE(TAG, "  - Card not properly inserted");
    //             ESP_LOGE(TAG, "  - Card not formatted as FAT32");
    //             ESP_LOGE(TAG, "  - Bad electrical connection");
    //             ESP_LOGE(TAG, "  - Card needs to be re-seated");
    //         }
    //         // Continue without SD card
    //     }
    // }

    // Initialize M5Unified AFTER SD card is mounted
    auto cfg = M5.config();
    cfg.clear_display = !is_wake_from_sleep; // Don't clear on wake - saves time
    cfg.output_power = true; // keep main power rail
    
    // Use cached board type to skip autodetect (saves ~1 second)
    if (is_wake_from_sleep) {
        // We know it's M5Paper, set directly
        cfg.internal_imu = false;
        cfg.internal_rtc = true;
        cfg.internal_spk = false;
        cfg.internal_mic = false;
    }
    
    M5.begin(cfg);
    M5.Display.setRotation(0); // Portrait
    M5.Display.setTextSize(3);
    
    // Skip "Initializing..." message on wake - saves ~100ms and we want fast restore
    if (!is_wake_from_sleep) {
        M5.Display.setTextDatum(textdatum_t::middle_center);
        M5.Display.drawString("Initializing...", M5.Display.width() / 2, M5.Display.height() / 2);
    }
    M5.Display.setTextDatum(textdatum_t::top_left); // Reset datum

    // Initialize SPIFFS
    esp_vfs_spiffs_conf_t conf = {
        .base_path = "/spiffs",
        .partition_label = "storage",
        .max_files = 10,
        .format_if_mount_failed = true};

    // Check if partition exists
    const esp_partition_t *partition = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, "storage");
    if (!partition)
    {
        ESP_LOGE(TAG, "SPIFFS partition 'storage' not found!");
        M5.Display.println("Partition Error");
    }
    else
    {
        ret = esp_vfs_spiffs_register(&conf);
        if (ret != ESP_OK)
        {
            ESP_LOGE(TAG, "Failed to initialize SPIFFS (%s)", esp_err_to_name(ret));
            M5.Display.println("SPIFFS Error");
        }
        else
        {
            // Skip expensive partition info query on wake - saves ~1.2 seconds
            if (!is_wake_from_sleep) {
                size_t total = 0, used = 0;
                ret = esp_spiffs_info("storage", &total, &used);
                if (ret != ESP_OK)
                {
                    ESP_LOGE(TAG, "Failed to get SPIFFS partition information (%s)", esp_err_to_name(ret));
                }
                else
                {
                    ESP_LOGI(TAG, "Partition size: total: %d, used: %d", total, used);
                }
            }
        }
    }

    gui.init(is_wake_from_sleep);
    
    // Initialize WiFi - either on cold boot or on wake from deep sleep if WiFi is enabled
    if (!is_wake_from_sleep) {
        // Cold boot: Always init wifi manager so we can enable it later if needed
        wifiManager.init();

        if (gui.isWifiEnabled()) {
            // Try to connect, if fails, start AP
            bool wifiOk = wifiManager.connect();
            if (!wifiOk)
            {
                wifiManager.startAP();
            }
            else
            {
                syncRtcFromNtp();
            }
            webServer.init("/spiffs");
        } else {
            ESP_LOGI(TAG, "WiFi disabled by settings");
        }
    } else {
        // Wake from deep sleep: Init WiFi if it was enabled in settings
        if (gui.isWifiEnabled()) {
            ESP_LOGI(TAG, "Wake from deep sleep with WiFi enabled - connecting");
            wifiManager.init();
            bool wifiOk = wifiManager.connect();
            if (!wifiOk) {
                wifiManager.startAP();
            }
            webServer.init("/spiffs");
        } else {
            ESP_LOGI(TAG, "Wake from deep sleep - WiFi disabled in settings");
        }
    }

    while (1)
    {
        M5.update();

        // Check buttons for restart
        if (M5.BtnA.wasPressed() || M5.BtnB.wasPressed() || M5.BtnC.wasPressed() || M5.BtnPWR.wasClicked())
        {
            ESP_LOGI(TAG, "Button pressed - Restarting...");
            M5.Display.clear();
            M5.Display.setCursor(0, 100);
            M5.Display.println("Restarting...");
            vTaskDelay(200 / portTICK_PERIOD_MS);
            esp_restart();
        }

        gui.setWifiStatus(wifiManager.isConnected(), wifiManager.getRssi());
        gui.update();
        vTaskDelay(20 / portTICK_PERIOD_MS);
    }
}
