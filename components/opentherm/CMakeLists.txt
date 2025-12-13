# OpenTherm component - queue-based thread modules
idf_component_register(
    SRCS "ot_thermostat.c" "ot_boiler.c" "ot_utils.c"
    INCLUDE_DIRS "include"
    REQUIRES opentherm_rmt freertos driver
    PRIV_REQUIRES esp_timer
)
