/*
 * OpenTherm Boiler Thread
 *
 * Dedicated thread for communicating with the boiler (slave) side.
 * Runs in its own FreeRTOS task and communicates with the main loop via queues.
 *
 * Thread responsibilities:
 * - Wait for request from main loop (via queue)
 * - Send request to boiler (TX)
 * - Wait for response from boiler (RX)
 * - Put response on queue for main loop
 *
 * BLOCKING: This thread blocks on RMT receive/send operations and queue waits.
 * The main loop remains non-blocking.
 */

#ifndef OT_BOILER_H
#define OT_BOILER_H

#include "esp_err.h"
#include "ot_queues.h"

#ifdef __cplusplus
extern "C" {
#endif

// Opaque handle
typedef struct ot_boiler ot_boiler_t;

// Configuration for boiler thread
typedef struct {
    gpio_num_t rx_pin;              // GPIO to receive from boiler
    gpio_num_t tx_pin;              // GPIO to transmit to boiler
    ot_queues_t *queues;            // Shared queue handles
    uint32_t task_stack_size;       // Stack size for FreeRTOS task
    UBaseType_t task_priority;      // Task priority
} ot_boiler_config_t;

/**
 * Initialize and start the boiler thread
 *
 * Creates a FreeRTOS task that handles all communication with the boiler.
 * The task will:
 * 1. BLOCK waiting for request from main loop (indefinitely)
 * 2. Send request to boiler (~40ms)
 * 3. BLOCK waiting for response from boiler (up to 800ms)
 * 4. Put response on queue for main loop
 * 5. Repeat
 *
 * @param config Configuration including pins and queue handles
 * @return Handle to boiler context, NULL on failure
 */
ot_boiler_t* ot_boiler_init(const ot_boiler_config_t *config);

/**
 * Stop and deinitialize the boiler thread
 *
 * @param handle Boiler context handle
 */
void ot_boiler_deinit(ot_boiler_t *handle);

/**
 * Get statistics from the boiler interface
 *
 * @param handle Boiler context handle
 * @param stats Output: statistics structure
 */
void ot_boiler_get_stats(ot_boiler_t *handle, ot_stats_t *stats);

/**
 * Send a request to the boiler (called from main loop)
 *
 * This puts the request on the boiler_request queue.
 * Non-blocking: uses xQueueOverwrite to replace any pending request.
 *
 * @param queues Shared queue handles
 * @param request Request frame to send
 */
void ot_boiler_send_request(ot_queues_t *queues, uint32_t request);

/**
 * Check if a response is available from the boiler (called from main loop)
 *
 * Non-blocking: returns immediately if no response available.
 *
 * @param queues Shared queue handles
 * @param response Output: response frame (only valid if returns true)
 * @return true if a response was retrieved, false otherwise
 */
bool ot_boiler_get_response(ot_queues_t *queues, uint32_t *response);

#ifdef __cplusplus
}
#endif

#endif // OT_BOILER_H
