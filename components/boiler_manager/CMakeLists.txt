idf_component_register(SRCS "boiler_manager.c"
                    INCLUDE_DIRS "."
                    REQUIRES opentherm mqtt_bridge
                    PRIV_REQUIRES mqtt_bridge)

