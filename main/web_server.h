#pragma once
#include "esp_http_server.h"
#include <cstdint>

class WebServer {
public:
    void init(const char* basePath);
    void stop();
    bool isRunning() const { return server != NULL; }
    
    // Track last HTTP activity time to prevent sleep during uploads
    static void updateActivityTime();
    static uint32_t getLastActivityTime();

private:
    httpd_handle_t server = NULL;
    static uint32_t lastHttpActivityTime;
};
