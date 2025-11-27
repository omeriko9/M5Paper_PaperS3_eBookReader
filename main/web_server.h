#pragma once
#include "esp_http_server.h"

class WebServer {
public:
    void init(const char* basePath);
    void stop();
    bool isRunning() const { return server != NULL; }

private:
    httpd_handle_t server = NULL;
};
