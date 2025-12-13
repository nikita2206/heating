idf_component_register(SRCS "opentherm_gateway.cpp"
                    PRIV_REQUIRES esp_driver_usb_serial_jtag esp_netif vfs esp_wifi 
                                  nvs_flash esp_event opentherm_library websocket_server ota_update boiler_manager mqtt_bridge
                    INCLUDE_DIRS ".")
