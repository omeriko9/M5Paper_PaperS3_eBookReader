idf_component_register(SRCS "drawing_app.cpp" "math_renderer.cpp" "composer_ui.cpp" "project_io.cpp" "midi_io.cpp" "sequencer_engine.cpp" "main.cpp" "wifi_manager.cpp" "web_server.cpp" "epub_loader.cpp" "gui.cpp" "dns_server.cpp" "book_index.cpp" "zip_reader.cpp" "device_hal.cpp" "image_handler.cpp" "gesture_detector.cpp" "game_manager.cpp" "html_utils.cpp"
                    INCLUDE_DIRS "."
                    REQUIRES m5unified esp_http_server spiffs nvs_flash esp_wifi lwip zlib fatfs sdmmc driver)
