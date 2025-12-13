idf_component_register(
    SRCS "OpenTherm.cpp"
    INCLUDE_DIRS "include"
    REQUIRES driver esp_timer freertos
)
