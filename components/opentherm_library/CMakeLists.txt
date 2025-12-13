idf_component_register(
    SRCS "opentherm.cpp"
    INCLUDE_DIRS "include"
    REQUIRES driver esp_timer freertos
)
