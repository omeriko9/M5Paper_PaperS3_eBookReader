idf_component_register(SRCS "main.cpp" "wifi_manager.cpp" "web_server.cpp" "epub_loader.cpp" "gui.cpp" "dns_server.cpp" "book_index.cpp" "zip_reader.cpp"
                    INCLUDE_DIRS "."
                    REQUIRES m5unified esp_http_server spiffs nvs_flash esp_wifi lwip zlib fatfs sdmmc)
