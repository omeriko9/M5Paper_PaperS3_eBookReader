#pragma once
#include <string>
#include <vector>
#include "esp_wifi.h"
#include "esp_heap_caps.h"

class WifiManager {
public:
    bool init(); // Returns false if init failed (e.g., out of memory)
    bool connect(); // Try to connect with stored creds
    void disconnect(); // Stop WiFi
    void startAP(); // Start Access Point for config
    bool isConnected();
    bool isInitialized() const { return s_initialized; }
    bool hasInitFailed() const { return s_init_failed; }
    std::string getIpAddress();
    int getRssi();
    
    // Save credentials
    void saveCredentials(const char* ssid, const char* password);
    
    std::vector<std::string> scanNetworks();

    void startDNS();
    void stopDNS();

private:
    bool s_initialized = false;
    bool s_init_failed = false;
};
