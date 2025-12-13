/*
 * Boiler Manager - Main Loop and Diagnostic Injection
 *
 * Runs the main control loop, coordinating communication between
 * thermostat and boiler threads via queues.
 *
 * REFACTORED: Now uses queue-based architecture instead of callbacks.
 */

#ifndef BOILER_MANAGER_H
#define BOILER_MANAGER_H

#include <stdint.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Forward declarations
typedef struct ot_queues ot_queues_t;
typedef void* ot_handle_t;  // Legacy, not used

// Operation modes
typedef enum {
    BOILER_MANAGER_MODE_PROXY,        // Proxy mode: intercept ID=0, inject diagnostics
    BOILER_MANAGER_MODE_PASSTHROUGH,  // Pass everything through unchanged
    BOILER_MANAGER_MODE_CONTROL       // Apply MQTT overrides, stub thermostat replies
} boiler_manager_mode_t;

// Message source categories for logging
typedef enum {
    OT_SOURCE_THERMOSTAT_BOILER,    // Proxied: Thermostat <-> Boiler
    OT_SOURCE_GATEWAY_BOILER,       // Gateway <-> Boiler (diagnostics)
    OT_SOURCE_THERMOSTAT_GATEWAY    // Thermostat <-> Gateway (control mode)
} ot_message_source_t;

// Forward declaration
typedef struct boiler_manager boiler_manager_t;

// Callback for message logging
typedef void (*boiler_manager_message_callback_t)(
    const char *direction,
    ot_message_source_t source,
    uint32_t message,
    void *user_data
);

// Diagnostic value with timestamp
typedef struct {
    float value;
    int64_t timestamp_ms;
    bool valid;
} boiler_diagnostic_value_t;

// Diagnostic command
typedef struct {
    uint8_t data_id;
    const char *name;
} boiler_diagnostic_cmd_t;

// Complete diagnostic state
typedef struct {
    boiler_diagnostic_value_t t_boiler;
    boiler_diagnostic_value_t t_return;
    boiler_diagnostic_value_t t_dhw;
    boiler_diagnostic_value_t t_dhw2;
    boiler_diagnostic_value_t t_outside;
    boiler_diagnostic_value_t t_exhaust;
    boiler_diagnostic_value_t t_heat_exchanger;
    boiler_diagnostic_value_t t_flow_ch2;
    boiler_diagnostic_value_t t_storage;
    boiler_diagnostic_value_t t_collector;
    boiler_diagnostic_value_t t_setpoint;
    boiler_diagnostic_value_t modulation_level;
    boiler_diagnostic_value_t pressure;
    boiler_diagnostic_value_t flow_rate;
    boiler_diagnostic_value_t fault_code;
    boiler_diagnostic_value_t diag_code;
    boiler_diagnostic_value_t burner_starts;
    boiler_diagnostic_value_t dhw_burner_starts;
    boiler_diagnostic_value_t ch_pump_starts;
    boiler_diagnostic_value_t dhw_pump_starts;
    boiler_diagnostic_value_t burner_hours;
    boiler_diagnostic_value_t dhw_burner_hours;
    boiler_diagnostic_value_t ch_pump_hours;
    boiler_diagnostic_value_t dhw_pump_hours;
    boiler_diagnostic_value_t max_capacity;
    boiler_diagnostic_value_t min_mod_level;
    boiler_diagnostic_value_t fan_setpoint;
    boiler_diagnostic_value_t fan_current;
    boiler_diagnostic_value_t fan_exhaust_rpm;
    boiler_diagnostic_value_t fan_supply_rpm;
    boiler_diagnostic_value_t co2_exhaust;
} boiler_diagnostics_t;

// Boiler manager instance
struct boiler_manager {
    boiler_manager_mode_t mode;
    boiler_diagnostics_t diagnostics;
    bool control_enabled;
    bool control_active;
    bool fallback_active;
    float demand_tset_c;
    bool demand_ch_enabled;
    int64_t last_demand_ms;
    int64_t last_control_apply_ms;
    int64_t last_diag_poll_ms;

    const boiler_diagnostic_cmd_t *diag_commands;
    size_t diag_commands_count;
    size_t diag_commands_index;

    bool intercepting_id0;
    uint32_t intercept_rate;
    uint32_t id0_frame_counter;

    bool manual_write_pending;
    uint32_t manual_write_frame;
    uint32_t manual_write_response;
    esp_err_t manual_write_result;
    SemaphoreHandle_t manual_write_sem;

    // Legacy - not used in queue-based design
    void *ot_instance;

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
 * @param mode Operation mode
 * @param ot Legacy parameter, ignored (pass NULL)
 * @param intercept_rate Intercept every Nth ID=0 frame
 */
esp_err_t boiler_manager_init(boiler_manager_t *bm, boiler_manager_mode_t mode,
                              ot_handle_t *ot, uint32_t intercept_rate);

/**
 * Start the boiler manager main loop task
 *
 * Creates a FreeRTOS task that runs the main control loop.
 * The task polls queues and coordinates thermostat/boiler communication.
 *
 * @param bm Boiler manager instance (must be initialized)
 * @param queues Queue handles for inter-thread communication
 * @param stack_size Task stack size (0 for default)
 * @param priority Task priority (0 for default)
 */
esp_err_t boiler_manager_start(boiler_manager_t *bm, ot_queues_t *queues,
                               uint32_t stack_size, UBaseType_t priority);

/**
 * Get diagnostic state
 */
const boiler_diagnostics_t* boiler_manager_get_diagnostics(boiler_manager_t *bm);

/**
 * Send a WRITE_DATA frame to the boiler
 */
esp_err_t boiler_manager_write_data(boiler_manager_t *bm, uint8_t data_id,
                                    uint16_t data_value, uint32_t *response_frame);

void boiler_manager_set_control_enabled(boiler_manager_t *bm, bool enabled);
void boiler_manager_get_status(boiler_manager_t *bm, boiler_manager_status_t *out);
void boiler_manager_set_mode(boiler_manager_t *bm, boiler_manager_mode_t mode);

void boiler_manager_set_message_callback(boiler_manager_t *bm,
                                         boiler_manager_message_callback_t callback,
                                         void *user_data);

#ifdef __cplusplus
}
#endif

#endif // BOILER_MANAGER_H
