# Conditionally select implementation based on Kconfig
if(CONFIG_OPENTHERM_IMPL_RMT)
    set(IMPL_SRCS "opentherm_rmt_impl.c")
else()
    set(IMPL_SRCS "opentherm_lib_impl.cpp")
endif()

# Register component with both potential dependencies
# Only one will be used based on which source file is compiled
idf_component_register(
    SRCS ${IMPL_SRCS}
    INCLUDE_DIRS "include"
    REQUIRES opentherm_rmt opentherm_library
    PRIV_REQUIRES opentherm_rmt opentherm_library
)

