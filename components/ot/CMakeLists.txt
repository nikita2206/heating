idf_component_register(
    SRCS "open_therm.cpp" "rmt_encoder.cpp"
    INCLUDE_DIRS "include" "."
    REQUIRES driver esp_timer freertos
)

if(BUILD_TESTING)
    add_subdirectory(test)
endif()
