/*
 * OpenTherm Boiler Thread
 *
 * Dedicated thread for communicating with the boiler (slave) side.
 * This thread runs in its own FreeRTOS task and handles all blocking
 * RMT operations for the boiler interface.
 *
 * Thread loop:
 * 1. BLOCKS waiting for request from main loop (indefinitely)
 * 2. BLOCKS sending request to boiler (~40ms)
 * 3. BLOCKS waiting for response from boiler (up to 800ms)
 * 4. Puts received response on queue for main loop
 * 5. Repeat
 */

#include "ot_boiler.h"
#include "opentherm_rmt.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "OT_BOILER";

// Internal context structure
struct ot_boiler {
    OpenThermRmtInterface iface;        // RMT interface for this side
    ot_queues_t *queues;                // Shared queue handles
    TaskHandle_t task_handle;           // FreeRTOS task handle
    SemaphoreHandle_t tx_done_sem;      // TX completion semaphore
    bool running;                       // Task running flag
    ot_stats_t stats;                   // Statistics
};

// RMT RX callback context
typedef struct {
    ot_boiler_t *ctx;
} boiler_rx_ctx_t;

static boiler_rx_ctx_t *s_rx_ctx = NULL;

/**
 * RMT RX done callback - signals when a frame is received
 *
 * Called from ISR context when RMT hardware detects frame completion.
 */
static bool IRAM_ATTR boiler_rx_callback(rmt_channel_handle_t channel,
                                          const rmt_rx_done_event_data_t *edata,
                                          void *user_ctx)
{
    BaseType_t high_task_wakeup = pdFALSE;
    boiler_rx_ctx_t *ctx = (boiler_rx_ctx_t *)user_ctx;

    if (ctx && ctx->ctx) {
        OpenThermRmtInterface *iface = &ctx->ctx->iface;

        if (!edata->flags.is_last) {
            return high_task_wakeup == pdTRUE;
        }

        size_t copy_size = edata->num_symbols;
        if (copy_size > 128) copy_size = 128;
        iface->rx_symbol_count = copy_size;
        iface->rx_pending = false;

        if (iface->rx_queue) {
            xQueueSendFromISR(iface->rx_queue, &copy_size, &high_task_wakeup);
        }
    }

    return high_task_wakeup == pdTRUE;
}

/**
 * RMT TX done callback - signals when transmission is complete
 */
static bool IRAM_ATTR boiler_tx_callback(rmt_channel_handle_t channel,
                                          const rmt_tx_done_event_data_t *edata,
                                          void *user_ctx)
{
    BaseType_t high_task_wakeup = pdFALSE;
    ot_boiler_t *ctx = (ot_boiler_t *)user_ctx;

    if (ctx && ctx->tx_done_sem) {
        xSemaphoreGiveFromISR(ctx->tx_done_sem, &high_task_wakeup);
    }

    return high_task_wakeup == pdTRUE;
}

/**
 * Receive a frame from the boiler
 *
 * BLOCKING: This function blocks for up to timeout_ms waiting for a complete
 * OpenTherm frame from the boiler.
 *
 * @param ctx Boiler context
 * @param frame Output: received 32-bit frame
 * @param timeout_ms Maximum time to wait
 * @return ESP_OK on success, ESP_ERR_TIMEOUT if no frame, ESP_ERR_INVALID_RESPONSE on decode error
 */
static esp_err_t boiler_receive_frame(ot_boiler_t *ctx, uint32_t *frame, uint32_t timeout_ms)
{
    OpenThermRmtInterface *iface = &ctx->iface;

    // Reset channel if previous receive timed out
    if (iface->rx_pending) {
        rmt_disable(iface->rx_channel);
        rmt_enable(iface->rx_channel);
        iface->rx_pending = false;
    }

    // Configure receive
    rmt_receive_config_t rx_config = {
        .signal_range_min_ns = 1000,
        .signal_range_max_ns = 3500000,
        .flags.en_partial_rx = false,
    };

    // Clear queue
    size_t dummy;
    while (xQueueReceive(iface->rx_queue, &dummy, 0) == pdTRUE) {}
    iface->rx_symbol_count = 0;
    iface->rx_pending = true;

    // Start receive (non-blocking)
    esp_err_t ret = rmt_receive(iface->rx_channel, iface->rx_buffer,
                                 iface->rx_buffer_size * sizeof(rmt_symbol_word_t), &rx_config);
    if (ret != ESP_OK) {
        iface->rx_pending = false;
        return ret;
    }

    // BLOCKING: Wait for frame
    size_t symbol_count = 0;
    if (xQueueReceive(iface->rx_queue, &symbol_count, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        ctx->stats.timeout_count++;
        return ESP_ERR_TIMEOUT;
    }

    // Decode the frame
    if (symbol_count < 1) {
        ctx->stats.error_count++;
        return ESP_ERR_INVALID_RESPONSE;
    }

    // Flatten symbols into half-bit stream
    uint8_t half_bits[80];
    int hb_count = 0;

    for (size_t i = 0; i < symbol_count && hb_count < 78; i++) {
        uint16_t durations[2] = { iface->rx_buffer[i].duration0, iface->rx_buffer[i].duration1 };
        uint8_t levels[2] = { iface->rx_buffer[i].level0, iface->rx_buffer[i].level1 };

        for (int part = 0; part < 2 && hb_count < 78; part++) {
            uint16_t dur = durations[part];
            uint8_t lvl = levels[part];
            if (dur == 0) continue;

            int num_half_bits;
            if (dur >= 350 && dur <= 650) num_half_bits = 1;
            else if (dur >= 850 && dur <= 1150) num_half_bits = 2;
            else num_half_bits = (dur + 250) / 500;

            if (num_half_bits < 1) num_half_bits = 1;
            if (num_half_bits > 4) num_half_bits = 4;

            for (int h = 0; h < num_half_bits && hb_count < 78; h++) {
                half_bits[hb_count++] = lvl;
            }
        }
    }

    // Decode Manchester
    uint32_t decoded = 0;
    int bit_count = 0;
    bool found_start = false;

    for (int i = 0; i < hb_count - 1 && bit_count < 32; i += 2) {
        uint8_t first_half = half_bits[i];
        uint8_t second_half = half_bits[i + 1];

        bool bit_value;
        if (first_half == 1 && second_half == 0) bit_value = true;
        else if (first_half == 0 && second_half == 1) bit_value = false;
        else continue;

        if (!found_start) {
            if (bit_value) found_start = true;
        } else {
            decoded = (decoded << 1) | (bit_value ? 1 : 0);
            bit_count++;
        }
    }

    if (bit_count != 32) {
        ctx->stats.error_count++;
        return ESP_ERR_INVALID_RESPONSE;
    }

    // Check parity
    uint32_t parity_check = decoded;
    int ones = 0;
    while (parity_check) { parity_check &= (parity_check - 1); ones++; }
    if (ones % 2 != 0) {
        ctx->stats.error_count++;
        return ESP_ERR_INVALID_CRC;
    }

    *frame = decoded;
    ctx->stats.rx_count++;
    return ESP_OK;
}

/**
 * Send a frame to the boiler
 *
 * BLOCKING: This function blocks for approximately 40ms while the frame
 * is transmitted via RMT hardware.
 *
 * @param ctx Boiler context
 * @param frame 32-bit frame to send
 * @return ESP_OK on success
 */
static esp_err_t boiler_send_frame(ot_boiler_t *ctx, uint32_t frame)
{
    OpenThermRmtInterface *iface = &ctx->iface;

    // Reset encoder
    rmt_encoder_reset(iface->encoder);

    rmt_transmit_config_t tx_config = {
        .loop_count = 0,
        .flags.eot_level = 1,
        .flags.queue_nonblocking = false,
    };

    xSemaphoreTake(ctx->tx_done_sem, 0);

    esp_err_t ret = rmt_transmit(iface->tx_channel, iface->encoder, &frame, sizeof(frame), &tx_config);
    if (ret != ESP_OK) return ret;

    // BLOCKING: Wait for TX complete (~40ms)
    if (xSemaphoreTake(ctx->tx_done_sem, pdMS_TO_TICKS(100)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    ctx->stats.tx_count++;
    return ESP_OK;
}

/**
 * Boiler thread main loop
 *
 * This task runs continuously, handling all communication with the boiler.
 * Each iteration:
 * 1. Waits for a request from main loop (BLOCKS indefinitely)
 * 2. Sends the request to boiler (BLOCKS ~40ms)
 * 3. Waits for response from boiler (BLOCKS up to 800ms)
 * 4. Puts response on queue for main loop
 */
static void ot_boiler_task(void *arg)
{
    ot_boiler_t *ctx = (ot_boiler_t *)arg;
    uint32_t request_frame, response_frame;
    ot_msg_t msg;

    ESP_LOGI(TAG, "Boiler thread started");

    while (ctx->running) {
        // 1. BLOCKING: Wait for request from main loop (indefinitely)
        if (xQueueReceive(ctx->queues->boiler_request, &msg, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        if (!ctx->running) break;

        request_frame = msg.data;
        ESP_LOGD(TAG, "Sending request to boiler: 0x%08lX", (unsigned long)request_frame);

        // 2. BLOCKING: Send request to boiler (~40ms)
        esp_err_t ret = boiler_send_frame(ctx, request_frame);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Failed to send request to boiler: %s", esp_err_to_name(ret));
            continue;
        }

        // 3. BLOCKING: Wait for response from boiler (up to 800ms per OT spec)
        ret = boiler_receive_frame(ctx, &response_frame, 800);

        if (ret == ESP_OK) {
            ESP_LOGD(TAG, "Received response from boiler: 0x%08lX", (unsigned long)response_frame);

            // 4. Put response on queue for main loop (non-blocking, overwrites if full)
            msg.data = response_frame;
            xQueueOverwrite(ctx->queues->boiler_response, &msg);
        } else if (ret == ESP_ERR_TIMEOUT) {
            ESP_LOGW(TAG, "Boiler response timeout");
        } else {
            ESP_LOGW(TAG, "Boiler receive error: %s", esp_err_to_name(ret));
        }
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

    // Create TX semaphore
    ctx->tx_done_sem = xSemaphoreCreateBinary();
    if (!ctx->tx_done_sem) {
        free(ctx);
        return NULL;
    }

    // Initialize RMT TX channel
    rmt_tx_channel_config_t tx_config = {
        .gpio_num = config->tx_pin,
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 1000000,
        .mem_block_symbols = 64,
        .trans_queue_depth = 4,
    };

    if (rmt_new_tx_channel(&tx_config, &ctx->iface.tx_channel) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create TX channel");
        vSemaphoreDelete(ctx->tx_done_sem);
        free(ctx);
        return NULL;
    }

    // Create Manchester encoder
    // extern esp_err_t opentherm_encoder_create(rmt_encoder_handle_t *ret_encoder);
    if (opentherm_encoder_create(&ctx->iface.encoder) != ESP_OK) {
        rmt_del_channel(ctx->iface.tx_channel);
        vSemaphoreDelete(ctx->tx_done_sem);
        free(ctx);
        return NULL;
    }

    // Register TX callback
    rmt_tx_event_callbacks_t tx_cbs = { .on_trans_done = boiler_tx_callback };
    rmt_tx_register_event_callbacks(ctx->iface.tx_channel, &tx_cbs, ctx);
    rmt_enable(ctx->iface.tx_channel);

    // Initialize RMT RX channel
    rmt_rx_channel_config_t rx_config = {
        .gpio_num = config->rx_pin,
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 1000000,
        .mem_block_symbols = 64,
    };

    if (rmt_new_rx_channel(&rx_config, &ctx->iface.rx_channel) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create RX channel");
        rmt_del_encoder(ctx->iface.encoder);
        rmt_disable(ctx->iface.tx_channel);
        rmt_del_channel(ctx->iface.tx_channel);
        vSemaphoreDelete(ctx->tx_done_sem);
        free(ctx);
        return NULL;
    }

    // Allocate RX buffer
    ctx->iface.rx_buffer = calloc(128, sizeof(rmt_symbol_word_t));
    ctx->iface.rx_buffer_size = 128;
    ctx->iface.rx_queue = xQueueCreate(4, sizeof(size_t));

    if (!ctx->iface.rx_buffer || !ctx->iface.rx_queue) {
        if (ctx->iface.rx_buffer) free(ctx->iface.rx_buffer);
        if (ctx->iface.rx_queue) vQueueDelete(ctx->iface.rx_queue);
        rmt_del_channel(ctx->iface.rx_channel);
        rmt_del_encoder(ctx->iface.encoder);
        rmt_disable(ctx->iface.tx_channel);
        rmt_del_channel(ctx->iface.tx_channel);
        vSemaphoreDelete(ctx->tx_done_sem);
        free(ctx);
        return NULL;
    }

    // Set up RX callback
    s_rx_ctx = calloc(1, sizeof(boiler_rx_ctx_t));
    s_rx_ctx->ctx = ctx;

    rmt_rx_event_callbacks_t rx_cbs = { .on_recv_done = boiler_rx_callback };
    rmt_rx_register_event_callbacks(ctx->iface.rx_channel, &rx_cbs, s_rx_ctx);
    rmt_enable(ctx->iface.rx_channel);

    ctx->iface.rx_gpio = config->rx_pin;
    ctx->iface.tx_gpio = config->tx_pin;

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
        free(s_rx_ctx);
        s_rx_ctx = NULL;
        free(ctx->iface.rx_buffer);
        vQueueDelete(ctx->iface.rx_queue);
        rmt_disable(ctx->iface.rx_channel);
        rmt_del_channel(ctx->iface.rx_channel);
        rmt_del_encoder(ctx->iface.encoder);
        rmt_disable(ctx->iface.tx_channel);
        rmt_del_channel(ctx->iface.tx_channel);
        vSemaphoreDelete(ctx->tx_done_sem);
        free(ctx);
        return NULL;
    }

    ESP_LOGI(TAG, "Initialized: RX=GPIO%d, TX=GPIO%d", config->rx_pin, config->tx_pin);
    return ctx;
}

void ot_boiler_deinit(ot_boiler_t *handle)
{
    if (!handle) return;

    handle->running = false;

    // Send dummy message to unblock the queue wait
    ot_msg_t dummy = {0};
    xQueueOverwrite(handle->queues->boiler_request, &dummy);

    // Wait for task to finish
    if (handle->task_handle) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    // Clean up RMT resources
    if (handle->iface.rx_channel) {
        rmt_disable(handle->iface.rx_channel);
        rmt_del_channel(handle->iface.rx_channel);
    }
    if (handle->iface.tx_channel) {
        rmt_disable(handle->iface.tx_channel);
        rmt_del_channel(handle->iface.tx_channel);
    }
    if (handle->iface.encoder) {
        rmt_del_encoder(handle->iface.encoder);
    }
    if (handle->iface.rx_buffer) {
        free(handle->iface.rx_buffer);
    }
    if (handle->iface.rx_queue) {
        vQueueDelete(handle->iface.rx_queue);
    }
    if (handle->tx_done_sem) {
        vSemaphoreDelete(handle->tx_done_sem);
    }
    if (s_rx_ctx) {
        free(s_rx_ctx);
        s_rx_ctx = NULL;
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
