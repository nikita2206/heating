/*
 * Example Configuration for OpenTherm Gateway
 * 
 * Copy the values you want to customize into opentherm_gateway.h
 * or configure via `idf.py menuconfig`
 */

// WiFi Configuration
// Set via menuconfig: Component config → OpenTherm Gateway Configuration
#define EXAMPLE_WIFI_SSID      "YourNetworkName"
#define EXAMPLE_WIFI_PASSWORD  "YourNetworkPassword"

// GPIO Pin Configuration
// Pins for thermostat side (master interface)
#define EXAMPLE_OT_MASTER_IN_PIN    4   // GPIO4 - Receives from thermostat
#define EXAMPLE_OT_MASTER_OUT_PIN   5   // GPIO5 - Sends to thermostat

// Pins for boiler side (slave interface)
#define EXAMPLE_OT_SLAVE_IN_PIN     18  // GPIO18 - Receives from boiler
#define EXAMPLE_OT_SLAVE_OUT_PIN    19  // GPIO19 - Sends to boiler

// Hardware Notes:
// - All GPIO pins must support INPUT mode (with interrupt capability for IN pins)
// - Use OpenTherm adapters for voltage level conversion (3.3V ESP32 ↔ 7-15V OpenTherm)
// - Ensure proper grounding between ESP32 and OpenTherm adapters
//
// Recommended OpenTherm Adapter Schematic:
// See: https://github.com/ihormelnyk/opentherm_library
//
// Wiring:
//   Thermostat → OT Adapter 1 IN → ESP32 GPIO4 (OT_MASTER_IN_PIN)
//   ESP32 GPIO5 (OT_MASTER_OUT_PIN) → OT Adapter 1 OUT → Thermostat
//
//   Boiler → OT Adapter 2 IN → ESP32 GPIO18 (OT_SLAVE_IN_PIN)
//   ESP32 GPIO19 (OT_SLAVE_OUT_PIN) → OT Adapter 2 OUT → Boiler

