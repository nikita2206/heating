/*
 * OpenTherm API - Abstraction Layer
 * 
 * Generic interface for OpenTherm gateway implementations.
 * Allows swapping between different backends:
 * - opentherm_rmt: RMT peripheral-based implementation
 * - opentherm_library: Arduino library-based implementation
 * 
 * The implementation is selected at build time via Kconfig.
 */

#ifndef OPENTHERM_API_H
#define OPENTHERM_API_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "opentherm_types.h"

#ifdef __cplusplus
extern "C" {
#endif

// Opaque handle to OpenTherm context
typedef struct ot_context ot_handle_t;

// Callback for handling received messages (used for logging)
typedef void (*ot_message_callback_t)(ot_handle_t *handle, ot_message_t *message, ot_role_t from_role, void *user_data);

// Callback for intercepting requests before forwarding (returns true to block forwarding)
typedef bool (*ot_request_interceptor_t)(ot_handle_t *handle, ot_message_t *request, void *user_data);

/**
 * Initialize OpenTherm gateway in MITM proxy mode
 * 
 * @param config Pin configuration for thermostat and boiler sides
 * @return Handle to OpenTherm context, NULL on failure
 */
ot_handle_t* ot_init(ot_pin_config_t config);

/**
 * Start OpenTherm communication (enables RX/TX)
 * 
 * @param handle OpenTherm context handle
 * @return ESP_OK on success
 */
esp_err_t ot_start(ot_handle_t* handle);

/**
 * Stop OpenTherm communication
 * 
 * @param handle OpenTherm context handle
 * @return ESP_OK on success
 */
esp_err_t ot_stop(ot_handle_t* handle);

/**
 * Deinitialize and free resources
 * 
 * @param handle OpenTherm context handle
 */
void ot_deinit(ot_handle_t* handle);

/**
 * Reset gateway to idle state
 * 
 * @param handle OpenTherm context handle
 */
void ot_reset(ot_handle_t* handle);

/**
 * Process gateway state machine (call from main loop)
 * 
 * @param handle OpenTherm context handle
 * @param request Buffer to receive captured request (can be NULL)
 * @param response Buffer to receive captured response (can be NULL)
 * @return true if a complete request/response transaction was proxied
 */
bool ot_process(ot_handle_t* handle, ot_message_t* request, ot_message_t* response);

/**
 * Get statistics
 * 
 * @param handle OpenTherm context handle
 * @param stats Buffer to receive statistics
 */
void ot_get_stats(ot_handle_t* handle, ot_stats_t* stats);

/**
 * Get timeout flag
 * 
 * @param handle OpenTherm context handle
 * @return true if timeout condition is active
 */
bool ot_get_timeout_flag(ot_handle_t* handle);

/**
 * Set message callback for logging
 * 
 * @param handle OpenTherm context handle
 * @param callback Callback function
 * @param user_data User data passed to callback
 */
void ot_set_message_callback(ot_handle_t* handle, ot_message_callback_t callback, void *user_data);

/**
 * Set request interceptor callback
 * 
 * @param handle OpenTherm context handle
 * @param interceptor Interceptor function (returns true to block forwarding)
 * @param user_data User data passed to interceptor
 */
void ot_set_request_interceptor(ot_handle_t* handle, ot_request_interceptor_t interceptor, void *user_data);

// ============================================================================
// Advanced: Direct Message Sending (for out-of-band queries and control)
// ============================================================================

/**
 * Send a request directly to the boiler and wait for response
 * 
 * This allows sending out-of-band queries to the boiler without going through
 * the normal gateway flow. Useful for diagnostics and manual commands.
 * 
 * @param handle OpenTherm context handle
 * @param request Request message to send
 * @param response Buffer to receive response (can be NULL)
 * @param timeout_ms Timeout in milliseconds (typically 800ms for OpenTherm)
 * @return ESP_OK on success, ESP_ERR_TIMEOUT on timeout, ESP_ERR_INVALID_STATE if busy
 */
esp_err_t ot_send_request_to_boiler(ot_handle_t* handle, ot_message_t request, 
                                     ot_message_t* response, uint32_t timeout_ms);

/**
 * Send a response directly to the thermostat
 * 
 * This allows injecting responses to the thermostat without normal flow.
 * Useful for control mode where we stub out the thermostat's requests.
 * 
 * @param handle OpenTherm context handle
 * @param response Response message to send
 * @return ESP_OK on success, ESP_ERR_INVALID_STATE if busy
 */
esp_err_t ot_send_response_to_thermostat(ot_handle_t* handle, ot_message_t response);

// ============================================================================
// Message Construction/Parsing Utilities
// ============================================================================

uint32_t ot_build_request(ot_message_type_t type, uint8_t id, uint16_t data);
uint32_t ot_build_response(ot_message_type_t type, uint8_t id, uint16_t data);

ot_message_type_t ot_get_message_type(uint32_t message);
uint8_t ot_get_data_id(uint32_t message);
uint16_t ot_get_uint16(uint32_t message);
float ot_get_float(uint32_t message);
uint8_t ot_get_uint8_hb(uint32_t message);  // High byte
uint8_t ot_get_uint8_lb(uint32_t message);  // Low byte

bool ot_check_parity(uint32_t frame);
bool ot_is_valid_response(uint32_t request, uint32_t response);

const char* ot_message_type_to_string(ot_message_type_t type);

#ifdef __cplusplus
}
#endif

#endif // OPENTHERM_API_H

