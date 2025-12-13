# Boiler Manager - main loop and diagnostics (C++)
idf_component_register(
    SRCS "boiler_manager.cpp"
    INCLUDE_DIRS "include"
    REQUIRES opentherm_library mqtt_bridge freertos
    PRIV_REQUIRES esp_timer
)
