/*
 * OpenTherm Gateway with WiFi and WebSocket Logging
 * 
 * This gateway sits between a thermostat and boiler, proxying all
 * OpenTherm messages while logging them via WebSocket for analysis.
 */

#ifndef OPENTHERM_GATEWAY_H
#define OPENTHERM_GATEWAY_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// These pins are sort of reversed, the MASTER pins are for the board that acts as a master and that connects to the boiler, therefore MASTER pins are the pins that allow this program to act as a master.
// The SLAVE pins are for the board that acts as a slave and that connects to the thermostat, therefore SLAVE pins are the pins that allow this program to act as a slave.

// GPIO Configuration
// Pins connected to thermostat (master side)
#define OT_MASTER_IN_PIN    GPIO_NUM_25  
#define OT_MASTER_OUT_PIN   GPIO_NUM_26  

// Pins connected to boiler (slave side)
#define OT_SLAVE_IN_PIN     GPIO_NUM_13
#define OT_SLAVE_OUT_PIN    GPIO_NUM_14

// WiFi Configuration (update these with your credentials)
#define WIFI_SSID      CONFIG_ESP_WIFI_SSID
#define WIFI_PASSWORD  CONFIG_ESP_WIFI_PASSWORD
#define WIFI_MAXIMUM_RETRY  5

// Application Configuration
#define OT_GATEWAY_TASK_STACK_SIZE  4096
#define OT_GATEWAY_TASK_PRIORITY    5

// Initialize console (if using USB Serial JTAG)
esp_err_t opentherm_gateway_console_init(void);

#ifdef __cplusplus
}
#endif

#endif // OPENTHERM_GATEWAY_H

