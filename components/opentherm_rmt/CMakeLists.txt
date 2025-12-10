idf_component_register(SRCS "opentherm.c" "opentherm_rmt.c"
                    INCLUDE_DIRS "include"
                    REQUIRES driver esp_timer freertos
                    PRIV_REQUIRES hal esp_driver_rmt)

