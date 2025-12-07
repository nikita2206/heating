idf_component_register(SRCS "opentherm.c"
                    INCLUDE_DIRS "."
                    REQUIRES driver esp_timer freertos
                    PRIV_REQUIRES hal)

