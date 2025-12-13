idf_component_register(
    SRCS "opentherm_library.c" "opentherm_port.cpp"
    INCLUDE_DIRS "include"
    REQUIRES driver esp_timer freertos
)
