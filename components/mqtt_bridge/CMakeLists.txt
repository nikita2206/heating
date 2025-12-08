idf_component_register(SRCS "mqtt_bridge.c"
                    INCLUDE_DIRS "."
                    REQUIRES mqtt esp_event nvs_flash)

