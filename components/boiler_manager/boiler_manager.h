/*
 * Boiler Manager - Diagnostic Injection and State Management
 * 
 * Intercepts ID=0 (Status) commands from thermostat and injects diagnostic
 * queries to monitor boiler state. Stores diagnostic results for UI display.
 * 
 * NOTE: Now uses the generic OpenTherm API for implementation independence.
 */

#ifndef BOILER_MANAGER_H
#define BOILER_MANAGER_H

#include <stdint.h>
#include <stdbool.h>
#include "opentherm_api.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#ifdef __cplusplus
extern "C" {
#endif

// Boiler manager operation modes
typedef enum {
    BOILER_MANAGER_MODE_PROXY,        // Proxy mode: intercept ID=0, inject diagnostics
    BOILER_MANAGER_MODE_PASSTHROUGH,  // Pass everything through unchanged
    BOILER_MANAGER_MODE_CONTROL       // Apply MQTT overrides, stub thermostat replies
} boiler_manager_mode_t;

// Message source categories for logging
typedef enum {
    OT_SOURCE_THERMOSTAT_BOILER,    // Proxied: Thermostat <-> Boiler (via gateway passthrough)
    OT_SOURCE_GATEWAY_BOILER,       // Gateway <-> Boiler (diagnostics, manual writes)
    OT_SOURCE_THERMOSTAT_GATEWAY    // Thermostat <-> Gateway (control mode responses)
} ot_message_source_t;

// Forward declaration
typedef struct boiler_manager boiler_manager_t;

// Callback for message logging (invoked when boiler_manager sends/receives messages)
typedef void (*boiler_manager_message_callback_t)(
    const char *direction,          // "REQUEST" or "RESPONSE"
    ot_message_source_t source,     // Communication pair
    uint32_t message,               // Raw 32-bit OpenTherm frame
    void *user_data
);

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
struct boiler_manager {
    boiler_manager_mode_t mode;
    boiler_diagnostics_t diagnostics;
    bool control_enabled;       // user toggle
    bool control_active;        // enabled + mqtt available
    bool fallback_active;       // mqtt unavailable
    float demand_tset_c;
    bool demand_ch_enabled;
    int64_t last_demand_ms;
    int64_t last_control_apply_ms;
    int64_t last_diag_poll_ms;

    // Diagnostic command rotation
    const boiler_diagnostic_cmd_t *diag_commands;
    size_t diag_commands_count;
    size_t diag_commands_index;

    // State for ID=0 interception
    bool intercepting_id0;
    ot_message_t pending_diag_request;
    int64_t diag_request_time_ms;

    // ID=0 interception rate control
    uint32_t intercept_rate;      // Intercept every Nth ID=0 frame (e.g., 10 = intercept 1 in 10)
    uint32_t id0_frame_counter;   // Counter for ID=0 frames seen

    // Manual write frame injection (queued, injected via interceptor)
    bool manual_write_pending;    // True if a manual write frame is queued
    uint32_t manual_write_frame;  // The frame to inject
    uint32_t manual_write_response; // Response frame (set by interceptor)
    esp_err_t manual_write_result; // Result code (set by interceptor)
    SemaphoreHandle_t manual_write_sem; // Semaphore to signal completion

    // Reference to OpenTherm instance (generic API)
    ot_handle_t *ot_instance;

    // Message logging callback
    boiler_manager_message_callback_t message_callback;
    void *message_callback_user_data;
};

typedef struct {
    bool control_enabled;
    bool control_active;
    bool fallback_active;
    bool mqtt_available;
    float demand_tset_c;
    bool demand_ch_enabled;
    int64_t last_demand_ms;
} boiler_manager_status_t;

/**
 * Initialize boiler manager
 * 
 * @param bm Boiler manager instance
 * @param mode Operation mode (PROXY or PASSTHROUGH)
 * @param ot OpenTherm instance (generic API handle)
 * @param intercept_rate Intercept every Nth ID=0 frame (e.g., 10 = intercept 1 in 10, 0 = intercept all)
 * @return ESP_OK on success
 */
esp_err_t boiler_manager_init(boiler_manager_t *bm, boiler_manager_mode_t mode, ot_handle_t *ot, uint32_t intercept_rate);

/**
 * Request interceptor callback for OpenTherm gateway
 * 
 * This is called by the gateway when a request is received.
 * Returns true to block forwarding, false to allow passthrough.
 * 
 * @param ot OpenTherm instance (generic API handle)
 * @param request Request message
 * @param user_data User data (boiler_manager_t instance)
 * @return true to block forwarding, false to allow passthrough
 */
bool boiler_manager_request_interceptor(ot_handle_t *ot, ot_message_t *request, void *user_data);

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

/**
 * Send a WRITE_DATA frame to the boiler and receive response
 * 
 * @param bm Boiler manager instance
 * @param data_id Data ID to write
 * @param data_value 16-bit data value (already encoded)
 * @param response_frame Buffer to receive response frame (can be NULL)
 * @return ESP_OK on success, ESP_ERR_TIMEOUT on timeout, ESP_FAIL on error
 */
esp_err_t boiler_manager_write_data(boiler_manager_t *bm, uint8_t data_id, uint16_t data_value, uint32_t *response_frame);

bool boiler_manager_process(boiler_manager_t *bm, ot_message_t *request, ot_message_t *response);

void boiler_manager_set_control_enabled(boiler_manager_t *bm, bool enabled);

void boiler_manager_get_status(boiler_manager_t *bm, boiler_manager_status_t *out);

void boiler_manager_set_mode(boiler_manager_t *bm, boiler_manager_mode_t mode);

/**
 * Set message callback for logging
 *
 * @param bm Boiler manager instance
 * @param callback Callback function (NULL to disable)
 * @param user_data User data passed to callback
 */
void boiler_manager_set_message_callback(boiler_manager_t *bm,
                                         boiler_manager_message_callback_t callback,
                                         void *user_data);

#ifdef __cplusplus
}
#endif

#endif // BOILER_MANAGER_H

