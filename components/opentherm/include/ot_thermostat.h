/*
 * OpenTherm Thermostat Thread
 *
 * Dedicated thread for communicating with the thermostat (master) side.
 * Runs in its own FreeRTOS task and communicates with the main loop via queues.
 *
 * Thread responsibilities:
 * - Receive requests from thermostat (RX)
 * - Put received requests on queue for main loop
 * - Wait for response from main loop (via queue)
 * - Send response back to thermostat (TX)
 *
 * BLOCKING: This thread blocks on RMT receive/send operations.
 * The main loop remains non-blocking.
 */

#ifndef OT_THERMOSTAT_H
#define OT_THERMOSTAT_H

#include "esp_err.h"
#include "ot_queues.h"

#ifdef __cplusplus
extern "C" {
#endif

// Opaque handle
typedef struct ot_thermostat ot_thermostat_t;

// Configuration for thermostat thread
typedef struct {
    gpio_num_t rx_pin;              // GPIO to receive from thermostat
    gpio_num_t tx_pin;              // GPIO to transmit to thermostat
    ot_queues_t *queues;            // Shared queue handles
    uint32_t task_stack_size;       // Stack size for FreeRTOS task
    UBaseType_t task_priority;      // Task priority
} ot_thermostat_config_t;

/**
 * Initialize and start the thermostat thread
 *
 * Creates a FreeRTOS task that handles all communication with the thermostat.
 * The task will:
 * 1. BLOCK waiting for request from thermostat (up to 1100ms)
 * 2. Put request on thermostat_request queue
 * 3. BLOCK waiting for response from main loop (up to 750ms)
 * 4. Send response to thermostat
 * 5. Repeat
 *
 * @param config Configuration including pins and queue handles
 * @return Handle to thermostat context, NULL on failure
 */
ot_thermostat_t* ot_thermostat_init(const ot_thermostat_config_t *config);

/**
 * Stop and deinitialize the thermostat thread
 *
 * @param handle Thermostat context handle
 */
void ot_thermostat_deinit(ot_thermostat_t *handle);

/**
 * Get statistics from the thermostat interface
 *
 * @param handle Thermostat context handle
 * @param stats Output: statistics structure
 */
void ot_thermostat_get_stats(ot_thermostat_t *handle, ot_stats_t *stats);

/**
 * Send a response to the thermostat (called from main loop)
 *
 * This puts the response on the thermostat_response queue.
 * Non-blocking: uses xQueueOverwrite to replace any pending response.
 *
 * @param queues Shared queue handles
 * @param response Response frame to send
 */
void ot_thermostat_send_response(ot_queues_t *queues, uint32_t response);

/**
 * Check if a new request is available from the thermostat (called from main loop)
 *
 * Non-blocking: returns immediately if no request available.
 *
 * @param queues Shared queue handles
 * @param request Output: request frame (only valid if returns true)
 * @return true if a request was retrieved, false otherwise
 */
bool ot_thermostat_get_request(ot_queues_t *queues, uint32_t *request);

#ifdef __cplusplus
}
#endif

#endif // OT_THERMOSTAT_H
