idf_component_register(SRCS "api_server.cpp"
                    INCLUDE_DIRS "."
                    REQUIRES esp_http_server esp_timer mqtt_bridge web_ui boiler_manager ot)
