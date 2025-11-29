#include "wifi_manager.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include "nvs.h"
#include "string.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "dns_server.h"

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

void WifiManager::init() {
    if (s_initialized) return;

    ESP_ERROR_CHECK(esp_netif_init());
    // Check if event loop is already created (might be by other components)
    esp_err_t err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_ERROR_CHECK(err);
    }
    
    esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &event_handler,
                                                        NULL,
                                                        NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &event_handler,
                                                        NULL,
                                                        NULL));
    s_initialized = true;
}

bool WifiManager::connect() {
    if (!s_initialized) init();

    // Load creds from NVS
    nvs_handle_t my_handle;
    esp_err_t err = nvs_open("storage", NVS_READWRITE, &my_handle);
    if (err != ESP_OK) return false;

    size_t ssid_len = 32;
    size_t pass_len = 64;
    char ssid[32] = {0};
    char pass[64] = {0};

    err = nvs_get_str(my_handle, "ssid", ssid, &ssid_len);
    if (err != ESP_OK) { nvs_close(my_handle); return false; }
    err = nvs_get_str(my_handle, "pass", pass, &pass_len);
    nvs_close(my_handle);

    if (err != ESP_OK) return false;
    
    if (strlen(ssid) == 0) return false;

    wifi_config_t wifi_config = {};
    memcpy(wifi_config.sta.ssid, ssid, ssid_len);
    memcpy(wifi_config.sta.password, pass, pass_len);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    // Wait a bit for connection (simplified)
    for(int i=0; i<20; i++) {
        if(s_connected) return true;
        vTaskDelay(500 / portTICK_PERIOD_MS);
    }
    return s_connected;
}

void WifiManager::disconnect() {
    if (!s_initialized) return;
    esp_wifi_stop();
    s_connected = false;
}

void WifiManager::startAP() {
    if (!s_initialized) init();

    wifi_config_t wifi_config = {};
    strcpy((char*)wifi_config.ap.ssid, "M5Paper_Reader");
    wifi_config.ap.ssid_len = strlen("M5Paper_Reader");
    wifi_config.ap.max_connection = 4;
    wifi_config.ap.authmode = WIFI_AUTH_OPEN;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
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

    wifi_scan_config_t scan_config = {0};
    scan_config.show_hidden = true;
    
    // Blocking scan
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
