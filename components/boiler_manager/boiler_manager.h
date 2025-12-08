/*
 * Boiler Manager - Diagnostic Injection and State Management
 * 
 * Intercepts ID=0 (Status) commands from thermostat and injects diagnostic
 * queries to monitor boiler state. Stores diagnostic results for UI display.
 */

#ifndef BOILER_MANAGER_H
#define BOILER_MANAGER_H

#include <stdint.h>
#include <stdbool.h>
#include "opentherm_rmt.h"

#ifdef __cplusplus
extern "C" {
#endif

// Boiler manager operation modes
typedef enum {
    BOILER_MANAGER_MODE_PROXY,        // Proxy mode: intercept ID=0, inject diagnostics
    BOILER_MANAGER_MODE_PASSTHROUGH   // Future: pass everything through unchanged
} boiler_manager_mode_t;

// Diagnostic value with timestamp
typedef struct {
    float value;
    int64_t timestamp_ms;
    bool valid;
} boiler_diagnostic_value_t;

// Diagnostic command ID and name
typedef struct {
    uint8_t data_id;
    const char *name;
} boiler_diagnostic_cmd_t;

// Complete diagnostic state
typedef struct {
    // Temperatures
    boiler_diagnostic_value_t t_boiler;           // ID 25
    boiler_diagnostic_value_t t_return;           // ID 28
    boiler_diagnostic_value_t t_dhw;             // ID 26
    boiler_diagnostic_value_t t_dhw2;            // ID 32
    boiler_diagnostic_value_t t_outside;         // ID 27
    boiler_diagnostic_value_t t_exhaust;         // ID 33
    boiler_diagnostic_value_t t_heat_exchanger;  // ID 34
    boiler_diagnostic_value_t t_flow_ch2;        // ID 31
    boiler_diagnostic_value_t t_storage;         // ID 29
    boiler_diagnostic_value_t t_collector;       // ID 30
    boiler_diagnostic_value_t t_setpoint;        // ID 1 (TSet - CH water temperature setpoint)
    
    // Status
    boiler_diagnostic_value_t modulation_level;  // ID 17
    boiler_diagnostic_value_t pressure;          // ID 18
    boiler_diagnostic_value_t flow_rate;         // ID 19
    
    // Faults
    boiler_diagnostic_value_t fault_code;        // ID 5 (low byte)
    boiler_diagnostic_value_t diag_code;         // ID 115
    
    // Statistics - starts
    boiler_diagnostic_value_t burner_starts;        // ID 116
    boiler_diagnostic_value_t dhw_burner_starts;     // ID 119
    boiler_diagnostic_value_t ch_pump_starts;        // ID 117
    boiler_diagnostic_value_t dhw_pump_starts;       // ID 118
    
    // Statistics - hours
    boiler_diagnostic_value_t burner_hours;         // ID 120
    boiler_diagnostic_value_t dhw_burner_hours;     // ID 123
    boiler_diagnostic_value_t ch_pump_hours;         // ID 121
    boiler_diagnostic_value_t dhw_pump_hours;        // ID 122
    
    // Configuration
    boiler_diagnostic_value_t max_capacity;          // ID 15 (high byte)
    boiler_diagnostic_value_t min_mod_level;         // ID 15 (low byte)
    
    // Fans
    boiler_diagnostic_value_t fan_setpoint;         // ID 35 (high byte)
    boiler_diagnostic_value_t fan_current;         // ID 35 (low byte)
    boiler_diagnostic_value_t fan_exhaust_rpm;      // ID 84
    boiler_diagnostic_value_t fan_supply_rpm;       // ID 85
    
    // CO2
    boiler_diagnostic_value_t co2_exhaust;          // ID 79
} boiler_diagnostics_t;

// Boiler manager instance
typedef struct boiler_manager {
    boiler_manager_mode_t mode;
    boiler_diagnostics_t diagnostics;
    
    // Diagnostic command rotation
    const boiler_diagnostic_cmd_t *diag_commands;
    size_t diag_commands_count;
    size_t diag_commands_index;
    
    // State for ID=0 interception
    bool intercepting_id0;
    OpenThermRmtMessage pending_diag_request;
    int64_t diag_request_time_ms;
    
    // ID=0 interception rate control
    uint32_t intercept_rate;      // Intercept every Nth ID=0 frame (e.g., 10 = intercept 1 in 10)
    uint32_t id0_frame_counter;   // Counter for ID=0 frames seen
    
    // Reference to OpenTherm RMT instance
    OpenThermRmt *ot_instance;
} boiler_manager_t;

/**
 * Initialize boiler manager
 * 
 * @param bm Boiler manager instance
 * @param mode Operation mode (PROXY or PASSTHROUGH)
 * @param ot OpenTherm RMT instance for sending commands
 * @param intercept_rate Intercept every Nth ID=0 frame (e.g., 10 = intercept 1 in 10, 0 = intercept all)
 * @return ESP_OK on success
 */
esp_err_t boiler_manager_init(boiler_manager_t *bm, boiler_manager_mode_t mode, OpenThermRmt *ot, uint32_t intercept_rate);

/**
 * Request interceptor callback for OpenTherm RMT gateway
 * 
 * This is called by the gateway when a request is received.
 * Returns true to block forwarding, false to allow passthrough.
 * 
 * @param ot OpenTherm RMT instance
 * @param request Request message
 * @return true to block forwarding, false to allow passthrough
 */
bool boiler_manager_request_interceptor(OpenThermRmt *ot, OpenThermRmtMessage *request);

/**
 * Get current diagnostic state (for UI)
 * 
 * @param bm Boiler manager instance
 * @return Pointer to diagnostics structure
 */
const boiler_diagnostics_t* boiler_manager_get_diagnostics(boiler_manager_t *bm);

/**
 * Inject a diagnostic command directly (for manual testing)
 * 
 * @param bm Boiler manager instance
 * @param data_id Data ID to query
 * @return ESP_OK on success
 */
esp_err_t boiler_manager_inject_command(boiler_manager_t *bm, uint8_t data_id);

#ifdef __cplusplus
}
#endif

#endif // BOILER_MANAGER_H

