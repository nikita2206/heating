/*
 * OpenTherm Thermostat Thread - Software Library Implementation
 *
 * Uses ISR-based software implementation (opentherm_library)
 * instead of RMT hardware peripheral.
 */

#include "ot_thermostat.h"
#include "opentherm_library.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <string.h>

static const char *TAG = "OT_THERM";

// Thermostat context
struct ot_thermostat {
    opentherm_t *ot_handle;          // OpenTherm library handle
    ot_queues_t *queues;             // Shared queue handles
    TaskHandle_t task_handle;         // FreeRTOS task handle
    bool running;                     // Task running flag
    ot_stats_t stats;                // Statistics
    SemaphoreHandle_t response_sem;   // Semaphore for response ready
    uint32_t pending_response;        // Response from main loop
    bool has_pending_response;        // Flag for response availability
};

// ============================================================================
// Task
// ============================================================================

static void ot_thermostat_task(void *arg)
{
    ot_thermostat_t *ctx = (ot_thermostat_t *)arg;
    ot_msg_t msg;

    ESP_LOGI(TAG, "Thermostat thread started (software ISR implementation)");

    while (ctx->running) {
        // Process state machine
        opentherm_process(ctx->ot_handle);

        // Check if we received a request
        if (opentherm_get_last_response_status(ctx->ot_handle) == OT_RESPONSE_SUCCESS) {
            uint32_t request_frame = opentherm_get_last_response(ctx->ot_handle);

            // Validate request
            if (!opentherm_is_valid_request(request_frame)) {
                ESP_LOGW(TAG, "Invalid request: 0x%08lX", (unsigned long)request_frame);
                ctx->stats.error_count++;
                vTaskDelay(pdMS_TO_TICKS(10));
                continue;
            }

            ESP_LOGI(TAG, "RX: 0x%08lX", (unsigned long)request_frame);
            ctx->stats.rx_count++;

            // Put request on queue for main loop
            msg.data = request_frame;
            xQueueOverwrite(ctx->queues->thermostat_request, &msg);

            // Wait for response from main loop (up to 750ms)
            if (xQueueReceive(ctx->queues->thermostat_response, &msg, pdMS_TO_TICKS(750)) == pdTRUE) {
                uint32_t response_frame = msg.data;
                ESP_LOGI(TAG, "TX: 0x%08lX", (unsigned long)response_frame);

                // Send response to thermostat
                if (opentherm_send_response(ctx->ot_handle, response_frame)) {
                    ctx->stats.tx_count++;
                } else {
                    ESP_LOGW(TAG, "Failed to send response");
                    ctx->stats.error_count++;
                }
            } else {
                ESP_LOGD(TAG, "No response from main loop");
                ctx->stats.timeout_count++;
            }
        }
        else if (opentherm_get_last_response_status(ctx->ot_handle) == OT_RESPONSE_TIMEOUT) {
            ctx->stats.timeout_count++;
        }
        else if (opentherm_get_last_response_status(ctx->ot_handle) == OT_RESPONSE_INVALID) {
            ctx->stats.error_count++;
        }

        // Small delay to avoid busy-waiting
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    ESP_LOGI(TAG, "Thermostat thread stopped");
    vTaskDelete(NULL);
}

// ============================================================================
// Public API
// ============================================================================

ot_thermostat_t* ot_thermostat_init(const ot_thermostat_config_t *config)
{
    if (!config || !config->queues) {
        ESP_LOGE(TAG, "Invalid config");
        return NULL;
    }

    ot_thermostat_t *ctx = calloc(1, sizeof(ot_thermostat_t));
    if (!ctx) {
        ESP_LOGE(TAG, "Failed to allocate context");
        return NULL;
    }

    ctx->queues = config->queues;
    ctx->running = true;
    ctx->has_pending_response = false;
    memset(&ctx->stats, 0, sizeof(ctx->stats));

    // Initialize OpenTherm library (acting as slave/boiler to receive from thermostat)
    ctx->ot_handle = opentherm_init(config->rx_pin, config->tx_pin, true);
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
        ot_thermostat_task,
        "ot_therm",
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

void ot_thermostat_deinit(ot_thermostat_t *handle)
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

void ot_thermostat_get_stats(ot_thermostat_t *handle, ot_stats_t *stats)
{
    if (handle && stats) {
        *stats = handle->stats;
    }
}

void ot_thermostat_send_response(ot_queues_t *queues, uint32_t response)
{
    if (queues && queues->thermostat_response) {
        ot_msg_t msg = { .data = response };
        xQueueOverwrite(queues->thermostat_response, &msg);
    }
}

bool ot_thermostat_get_request(ot_queues_t *queues, uint32_t *request)
{
    if (!queues || !queues->thermostat_request || !request) {
        return false;
    }

    ot_msg_t msg;
    if (xQueueReceive(queues->thermostat_request, &msg, 0) == pdTRUE) {
        *request = msg.data;
        return true;
    }
    return false;
}
