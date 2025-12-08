idf_component_register(SRCS "opentherm.c" "opentherm_rmt.c"
                    INCLUDE_DIRS "."
                    REQUIRES driver esp_timer freertos esp_driver_rmt
                    PRIV_REQUIRES hal)

