# OpenTherm component - queue-based thread modules
# C++ implementation with RAII patterns
idf_component_register(
    SRCS "ot_queues.cpp" "ot_thermostat.cpp" "ot_boiler.cpp"
    INCLUDE_DIRS "include"
    REQUIRES opentherm_library freertos driver
    PRIV_REQUIRES esp_timer
)
