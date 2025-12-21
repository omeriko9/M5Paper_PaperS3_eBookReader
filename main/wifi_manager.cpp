#include "wifi_manager.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_task_wdt.h"
#include "nvs.h"
#include "string.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "dns_server.h"
#include <stdio.h>
#include <sys/stat.h>

static const char *TAG = "WIFI";
static bool s_connected = false;
static int s_rssi = 0;
static DnsServer dnsServer;

static void event_handler(void* arg, esp_event_base_t event_base,
                                int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        s_connected = false;
        // In a real app, we might want to limit retries or have a backoff
        esp_wifi_connect(); 
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        s_connected = true;
        wifi_ap_record_t ap_info;
        esp_wifi_sta_get_ap_info(&ap_info);
        s_rssi = ap_info.rssi;
    }
}

static bool readConfigFromSD(char* ssid, size_t ssid_len, char* pass, size_t pass_len) {
    const char* path = "/sd/config.txt";
    struct stat st;
    if (stat(path, &st) != 0) {
        ESP_LOGW(TAG, "Config file not found at %s", path);
        return false;
    }

    FILE* f = fopen(path, "r");
    if (f == NULL) {
        ESP_LOGE(TAG, "Failed to open config file at %s", path);
        return false;
    }

    ESP_LOGI(TAG, "Reading WiFi config from SD card...");
    char line[128];
    bool found = false;
    while (fgets(line, sizeof(line), f)) {
        // Remove newline
        char* pos = strchr(line, '\n');
        if (pos) *pos = '\0';
        pos = strchr(line, '\r');
        if (pos) *pos = '\0';

        if (strncmp(line, "ssid=", 5) == 0) {
            strncpy(ssid, line + 5, ssid_len - 1);
            found = true;
            ESP_LOGI(TAG, "Found SSID in config");
        } else if (strncmp(line, "password=", 9) == 0) {
            strncpy(pass, line + 9, pass_len - 1);
            ESP_LOGI(TAG, "Found Password in config");
        }
    }
    fclose(f);
    
    if (!found) {
        ESP_LOGW(TAG, "SSID not found in config file");
    }
    return found && (strlen(ssid) > 0);
}

static bool connectToAP(const char* ssid, const char* pass) {
    if (strlen(ssid) == 0) return false;

    ESP_LOGI(TAG, "Connecting to SSID: %s", ssid);
    
    // Stop any previous connection attempt
    esp_wifi_disconnect();
    s_connected = false;

    wifi_config_t wifi_config = {};
    strncpy((char*)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid));
    if (pass) {
        strncpy((char*)wifi_config.sta.password, pass, sizeof(wifi_config.sta.password));
    }

    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_set_mode failed: %s", esp_err_to_name(err));
        return false;
    }
    err = esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_set_config failed: %s", esp_err_to_name(err));
        return false;
    }
    err = esp_wifi_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_start failed: %s", esp_err_to_name(err));
        return false;
    }
    
    // Disable power save to ensure reliable connection/throughput
    esp_wifi_set_ps(WIFI_PS_NONE);

    // Wait a bit for connection
    for(int i=0; i<20; i++) {
        esp_task_wdt_reset();
        if(s_connected) return true;
        vTaskDelay(500 / portTICK_PERIOD_MS);
    }
    return s_connected;
}

bool WifiManager::init() {
    if (s_initialized) return true;

    esp_err_t err = esp_netif_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_netif_init failed: %s", esp_err_to_name(err));
        return false;
    }
    
    // Check if event loop is already created (might be by other components)
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_event_loop_create_default failed: %s", esp_err_to_name(err));
        return false;
    }
    
    esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    // Reduce WiFi static RX buffer to save memory - we don't need high throughput
    cfg.static_rx_buf_num = 4;  // Default is 10, reduce for memory savings
    cfg.dynamic_rx_buf_num = 8; // Default is 32
    cfg.tx_buf_type = 1;  // Dynamic TX buffer
    cfg.static_tx_buf_num = 0;  // Use dynamic only
    cfg.dynamic_tx_buf_num = 8;
    cfg.cache_tx_buf_num = 0;  // Disable TX caching to save memory
    cfg.ampdu_rx_enable = 0;   // Disable AMPDU to save memory
    cfg.ampdu_tx_enable = 0;
    
    err = esp_wifi_init(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_init failed: %s (free heap: %u, SPIRAM: %u)", 
                 esp_err_to_name(err),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
        s_init_failed = true;
        return false;
    }

    err = esp_event_handler_instance_register(WIFI_EVENT,
                                              ESP_EVENT_ANY_ID,
                                              &event_handler,
                                              NULL,
                                              NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register WIFI_EVENT handler: %s", esp_err_to_name(err));
        esp_wifi_deinit();
        return false;
    }
    
    err = esp_event_handler_instance_register(IP_EVENT,
                                              IP_EVENT_STA_GOT_IP,
                                              &event_handler,
                                              NULL,
                                              NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register IP_EVENT handler: %s", esp_err_to_name(err));
        esp_wifi_deinit();
        return false;
    }
    
    s_initialized = true;
    ESP_LOGI(TAG, "WiFi initialized successfully");
    return true;
}

bool WifiManager::connect() {
    if (!s_initialized) {
        if (!init()) {
            ESP_LOGW(TAG, "Cannot connect - WiFi init failed");
            return false;
        }
    }
    if (s_init_failed) {
        ESP_LOGW(TAG, "Cannot connect - WiFi init previously failed");
        return false;
    }

    // 1. Try NVS credentials
    nvs_handle_t my_handle;
    esp_err_t err = nvs_open("storage", NVS_READWRITE, &my_handle);
    
    char ssid[33] = {0};
    char pass[65] = {0};
    bool hasNvsCreds = false;

    if (err == ESP_OK) {
        size_t ssid_len = 32;
        size_t pass_len = 64;
        err = nvs_get_str(my_handle, "ssid", ssid, &ssid_len);
        if (err == ESP_OK && strlen(ssid) > 0) {
            nvs_get_str(my_handle, "pass", pass, &pass_len);
            hasNvsCreds = true;
        }
        nvs_close(my_handle);
    }

    if (hasNvsCreds) {
        ESP_LOGI(TAG, "Trying NVS credentials...");
        if (connectToAP(ssid, pass)) {
            ESP_LOGI(TAG, "Connected using NVS credentials");
            return true;
        }
        ESP_LOGW(TAG, "Failed to connect using NVS credentials");
    }

    // 2. Try SD Card credentials (fallback)
    memset(ssid, 0, sizeof(ssid));
    memset(pass, 0, sizeof(pass));
    if (readConfigFromSD(ssid, sizeof(ssid), pass, sizeof(pass))) {
        ESP_LOGI(TAG, "Trying SD card credentials...");
        if (connectToAP(ssid, pass)) {
            ESP_LOGI(TAG, "Connected using SD card credentials");
            return true;
        }
        ESP_LOGW(TAG, "Failed to connect using SD card credentials");
    }

    return false;
}
void WifiManager::disconnect() {
    if (!s_initialized) return;
    esp_wifi_stop();
    s_connected = false;
}

void WifiManager::startAP() {
    if (!s_initialized) {
        if (!init()) {
            ESP_LOGW(TAG, "Cannot start AP - WiFi init failed");
            return;
        }
    }
    if (s_init_failed) {
        ESP_LOGW(TAG, "Cannot start AP - WiFi init previously failed");
        return;
    }

    wifi_config_t wifi_config = {};
    strcpy((char*)wifi_config.ap.ssid, "M5Paper_Reader");
    wifi_config.ap.ssid_len = strlen("M5Paper_Reader");
    wifi_config.ap.max_connection = 4;
    wifi_config.ap.authmode = WIFI_AUTH_OPEN;

    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_AP);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_set_mode(AP) failed: %s", esp_err_to_name(err));
        return;
    }
    err = esp_wifi_set_config(WIFI_IF_AP, &wifi_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_set_config(AP) failed: %s", esp_err_to_name(err));
        return;
    }
    err = esp_wifi_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_start(AP) failed: %s", esp_err_to_name(err));
        return;
    }
    ESP_LOGI(TAG, "AP Started");
    
    startDNS();
}

void WifiManager::startDNS() {
    dnsServer.start();
}

void WifiManager::stopDNS() {
    dnsServer.stop();
}

bool WifiManager::isConnected() {
    return s_connected;
}

int WifiManager::getRssi() {
    return s_rssi;
}

std::vector<std::string> WifiManager::scanNetworks() {
    if (!s_initialized) init();
    
    // Ensure we are in a mode that supports scanning (STA or APSTA)
    wifi_mode_t mode;
    esp_wifi_get_mode(&mode);
    if (mode == WIFI_MODE_NULL) {
        esp_wifi_set_mode(WIFI_MODE_STA);
        esp_wifi_start();
    }

#ifdef CONFIG_IDF_TARGET_ESP32
    // On ESP32, ensure WiFi is started even if mode is not NULL
    // This fixes ESP_FAIL if WiFi was stopped but mode was preserved
    esp_err_t start_ret = esp_wifi_start();
    if (start_ret != ESP_OK) {
        ESP_LOGW(TAG, "esp_wifi_start failed: %s", esp_err_to_name(start_ret));
    } else {
        // Ensure WiFi is started (idempotent if already started, but safe)
        esp_wifi_start();
    } Ensure power save is off for scanning
    esp_wifi_set_ps(WIFI_PS_NONE);
#endif
    // Blocking scan
    wifi_scan_config_t scan_config = {
    .ssid = 0, // Scan for all SSIDs (0)
    .bssid = 0, // Scan for all BSSIDs (0)
    .channel = 0, // Scan all channels (0)
    .show_hidden = false,
    .scan_type = WIFI_SCAN_TYPE_ACTIVE, // Or WIFI_SCAN_TYPE_PASSIVE
    .scan_time = {
        .active = { .min = 100, .max = 1500 } // Time in ms per channel
    },
};
    esp_err_t err = esp_wifi_scan_start(&scan_config, true);
    std::vector<std::string> ssids;
    if (err == ESP_OK) {
        uint16_t ap_count = 0;
        esp_wifi_scan_get_ap_num(&ap_count);
        if (ap_count > 0) {
            std::vector<wifi_ap_record_t> ap_records(ap_count);
            esp_wifi_scan_get_ap_records(&ap_count, ap_records.data());
            for (const auto& ap : ap_records) {
                // Filter out empty SSIDs
                if (strlen((char*)ap.ssid) > 0) {
                    ssids.push_back(std::string((char*)ap.ssid));
                }
            }
        }
    } else {
        ESP_LOGE(TAG, "WiFi scan failed: %s", esp_err_to_name(err));
    }
    return ssids;
}

void WifiManager::saveCredentials(const char* ssid, const char* password) {
    nvs_handle_t my_handle;
    esp_err_t err = nvs_open("storage", NVS_READWRITE, &my_handle);
    if (err == ESP_OK) {
        nvs_set_str(my_handle, "ssid", ssid);
        nvs_set_str(my_handle, "pass", password);
        nvs_commit(my_handle);
        nvs_close(my_handle);
    }
}

std::string WifiManager::getIpAddress() {
    esp_netif_ip_info_t ip_info;
    esp_netif_t* netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (s_connected && netif) {
        esp_netif_get_ip_info(netif, &ip_info);
        char buf[16];
        esp_ip4addr_ntoa(&ip_info.ip, buf, sizeof(buf));
        return std::string(buf);
    }
    // If not connected as STA, maybe we are AP?
    return "192.168.4.1"; 
}
