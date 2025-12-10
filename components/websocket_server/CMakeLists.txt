idf_component_register(SRCS "websocket_server.c"
                    INCLUDE_DIRS "."
                    REQUIRES esp_http_server esp_timer mqtt_bridge
                    PRIV_REQUIRES boiler_manager)

