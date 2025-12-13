# OpenTherm component - queue-based thread modules
# Now uses software ISR-based implementation (opentherm_library) instead of RMT
idf_component_register(
    SRCS "ot_thermostat.c" "ot_boiler.c" "ot_utils.c"
    INCLUDE_DIRS "include"
    REQUIRES opentherm_library freertos driver
    PRIV_REQUIRES esp_timer
)
