# Boiler Manager - main loop and diagnostics
idf_component_register(
    SRCS "boiler_manager.c"
    INCLUDE_DIRS "."
    REQUIRES opentherm mqtt_bridge freertos
    PRIV_REQUIRES esp_timer
)
