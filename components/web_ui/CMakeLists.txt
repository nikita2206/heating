idf_component_register(
    SRCS "web_ui_pages.cpp"
    INCLUDE_DIRS "."
    REQUIRES esp_http_server
)

# Enable C++20 for constexpr features
target_compile_options(${COMPONENT_LIB} PRIVATE -std=gnu++20)

