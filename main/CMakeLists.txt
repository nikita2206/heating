idf_component_register(SRCS "opentherm_gateway.c"
                    PRIV_REQUIRES esp_driver_usb_serial_jtag esp_netif vfs esp_wifi 
                                  nvs_flash esp_event opentherm websocket_server ota_update
                    INCLUDE_DIRS ".")
