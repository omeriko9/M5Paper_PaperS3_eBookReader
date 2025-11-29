#pragma once
#include <string>
#include <vector>
#include "esp_wifi.h"

class WifiManager {
public:
    void init();
    bool connect(); // Try to connect with stored creds
    void disconnect(); // Stop WiFi
    void startAP(); // Start Access Point for config
    bool isConnected();
    bool isInitialized() const { return s_initialized; }
    std::string getIpAddress();
    int getRssi();
    
    // Save credentials
    void saveCredentials(const char* ssid, const char* password);
    
    std::vector<std::string> scanNetworks();

    void startDNS();
    void stopDNS();

private:
    bool s_initialized = false;
};
