/*
 * OpenTherm Boiler Thread - Software Library Implementation
 *
 * Uses ISR-based software implementation (opentherm_library)
 * instead of RMT hardware peripheral.
 */

#include "ot_boiler.h"
#include "opentherm_library.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "OT_BOILER";

// Boiler context
struct ot_boiler {
    opentherm_t *ot_handle;      // OpenTherm library handle
    ot_queues_t *queues;         // Shared queue handles
    TaskHandle_t task_handle;     // FreeRTOS task handle
    bool running;                 // Task running flag
    ot_stats_t stats;            // Statistics
};

// ============================================================================
// Task
// ============================================================================

static void ot_boiler_task(void *arg)
{
    ot_boiler_t *ctx = (ot_boiler_t *)arg;
    ot_msg_t msg;

    ESP_LOGI(TAG, "Boiler thread started (software ISR implementation)");

    while (ctx->running) {
        // 1. BLOCKING: Wait for request from main loop
        if (xQueueReceive(ctx->queues->boiler_request, &msg, pdMS_TO_TICKS(100)) != pdTRUE) {
            continue;
        }

        uint32_t request_frame = msg.data;
        ESP_LOGI(TAG, "Sending request to boiler: 0x%08lX", (unsigned long)request_frame);

        // 2. Send request to boiler and wait for response (blocks ~1 second max)
        uint32_t response_frame = opentherm_send_request(ctx->ot_handle, request_frame);

        ESP_LOGI(TAG, "Response status: %d, response: 0x%08lX",
                 opentherm_get_last_response_status(ctx->ot_handle),
                 (unsigned long)response_frame);

        if (response_frame == 0 ||
            opentherm_get_last_response_status(ctx->ot_handle) != OT_RESPONSE_SUCCESS) {
            // Timeout or error
            ESP_LOGW(TAG, "Boiler response timeout or error");
            ctx->stats.timeout_count++;

            // Send error response to main loop
            msg.data = 0;  // 0 indicates error
            xQueueOverwrite(ctx->queues->boiler_response, &msg);
            continue;
        }

        // Validate response
        if (!opentherm_is_valid_response(response_frame)) {
            ESP_LOGW(TAG, "Invalid response from boiler: 0x%08lX",
                     (unsigned long)response_frame);
            ctx->stats.error_count++;

            msg.data = 0;
            xQueueOverwrite(ctx->queues->boiler_response, &msg);
            continue;
        }

        ESP_LOGD(TAG, "Received response from boiler: 0x%08lX",
                 (unsigned long)response_frame);
        ctx->stats.rx_count++;
        ctx->stats.tx_count++;

        // 3. Put response on queue for main loop
        msg.data = response_frame;
        xQueueOverwrite(ctx->queues->boiler_response, &msg);
    }

    ESP_LOGI(TAG, "Boiler thread stopped");
    vTaskDelete(NULL);
}

// ============================================================================
// Public API
// ============================================================================

ot_boiler_t* ot_boiler_init(const ot_boiler_config_t *config)
{
    if (!config || !config->queues) {
        ESP_LOGE(TAG, "Invalid config");
        return NULL;
    }

    ot_boiler_t *ctx = calloc(1, sizeof(ot_boiler_t));
    if (!ctx) {
        ESP_LOGE(TAG, "Failed to allocate context");
        return NULL;
    }

    ctx->queues = config->queues;
    ctx->running = true;
    memset(&ctx->stats, 0, sizeof(ctx->stats));

    // Initialize OpenTherm library (acting as master to send to boiler slave)
    ctx->ot_handle = opentherm_init(config->rx_pin, config->tx_pin, false);
    if (!ctx->ot_handle) {
        ESP_LOGE(TAG, "Failed to initialize OpenTherm library");
        free(ctx);
        return NULL;
    }

    // Start OpenTherm
    if (opentherm_begin(ctx->ot_handle) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start OpenTherm");
        opentherm_free(ctx->ot_handle);
        free(ctx);
        return NULL;
    }

    // Create the task
    BaseType_t ret = xTaskCreate(
        ot_boiler_task,
        "ot_boiler",
        config->task_stack_size > 0 ? config->task_stack_size : 4096,
        ctx,
        config->task_priority > 0 ? config->task_priority : 5,
        &ctx->task_handle
    );

    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create task");
        opentherm_free(ctx->ot_handle);
        free(ctx);
        return NULL;
    }

    ESP_LOGI(TAG, "Initialized: RX=GPIO%d, TX=GPIO%d",
             config->rx_pin, config->tx_pin);
    return ctx;
}

void ot_boiler_deinit(ot_boiler_t *handle)
{
    if (!handle) return;

    handle->running = false;

    // Wait for task to finish
    if (handle->task_handle) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    // Clean up OpenTherm library
    if (handle->ot_handle) {
        opentherm_free(handle->ot_handle);
    }

    free(handle);
}

void ot_boiler_get_stats(ot_boiler_t *handle, ot_stats_t *stats)
{
    if (handle && stats) {
        *stats = handle->stats;
    }
}

void ot_boiler_send_request(ot_queues_t *queues, uint32_t request)
{
    if (queues && queues->boiler_request) {
        ot_msg_t msg = { .data = request };
        xQueueOverwrite(queues->boiler_request, &msg);
    }
}

bool ot_boiler_get_response(ot_queues_t *queues, uint32_t *response)
{
    if (!queues || !queues->boiler_response || !response) {
        return false;
    }

    ot_msg_t msg;
    if (xQueueReceive(queues->boiler_response, &msg, 0) == pdTRUE) {
        *response = msg.data;
        return true;
    }
    return false;
}
