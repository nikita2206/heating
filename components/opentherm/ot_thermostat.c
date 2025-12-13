/*
 * OpenTherm Thermostat Thread
 *
 * Dedicated thread for communicating with the thermostat (master) side.
 * RMT captures end at idle (2ms timeout). If capture started mid-frame,
 * the next capture will be synchronized to frame start.
 *
 * Thread loop:
 * 1. BLOCKS waiting for request from thermostat (RMT RX)
 * 2. Puts received request on queue for main loop
 * 3. BLOCKS waiting for response from main loop (up to 750ms)
 * 4. BLOCKS sending response to thermostat (~40ms)
 * 5. Repeat
 */

#include "ot_thermostat.h"
#include "opentherm_rmt.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "OT_THERM";

// ============================================================================
// Constants
// ============================================================================

#define OT_HALF_BIT_US      500
#define OT_BIT_US           1000
#define OT_FRAME_BITS       34      // start + 32 data + stop

// ============================================================================
// Internal Context
// ============================================================================

struct ot_thermostat {
    OpenThermRmtInterface iface;        // RMT interface
    ot_queues_t *queues;                // Shared queue handles
    TaskHandle_t task_handle;           // FreeRTOS task handle
    SemaphoreHandle_t tx_done_sem;      // TX completion semaphore
    bool running;                       // Task running flag
    ot_stats_t stats;                   // Statistics
};

// RMT RX callback context
typedef struct {
    ot_thermostat_t *ctx;
} thermostat_rx_ctx_t;

static thermostat_rx_ctx_t *s_rx_ctx = NULL;

// ============================================================================
// Manchester Decoder
// ============================================================================

/**
 * Decode Manchester-encoded symbols into a 32-bit frame.
 *
 * This decoder works directly with RMT symbols, using timing to determine
 * bit boundaries. It's more robust than the half-bit approach because it
 * uses actual edge timing rather than reconstructed levels.
 */
static esp_err_t decode_manchester_symbols(const rmt_symbol_word_t *syms, int sym_count,
                                            uint32_t *frame)
{
    if (sym_count < 2) {
        return ESP_ERR_INVALID_SIZE;
    }

    // Convert symbols to half-bit stream for decoding
    uint8_t half_bits[160];
    int hb_count = 0;

    for (int i = 0; i < sym_count && hb_count < 156; i++) {
        uint16_t durations[2] = { syms[i].duration0, syms[i].duration1 };
        uint8_t levels[2] = { syms[i].level0, syms[i].level1 };

        for (int part = 0; part < 2 && hb_count < 156; part++) {
            uint16_t dur = durations[part];
            uint8_t lvl = levels[part];
            if (dur == 0) continue;

            // Skip very long durations (idle periods)
            if (dur > 1500) continue;

            int num_half_bits;
            if (dur >= 350 && dur <= 650) num_half_bits = 1;
            else if (dur >= 850 && dur <= 1150) num_half_bits = 2;
            else num_half_bits = (dur + 250) / 500;

            if (num_half_bits < 1) num_half_bits = 1;
            if (num_half_bits > 2) num_half_bits = 2;

            for (int h = 0; h < num_half_bits && hb_count < 156; h++) {
                half_bits[hb_count++] = lvl;
            }
        }
    }

    // Find start bit: scan for first '01' pattern (bit '1' = LOW-HIGH)
    int start_pos = -1;
    for (int i = 0; i < hb_count - 68; i++) {
        if (half_bits[i] == 0 && half_bits[i + 1] == 1) {
            start_pos = i;
            break;
        }
    }

    if (start_pos < 0) {
        ESP_LOGD(TAG, "No start bit found in %d half-bits", hb_count);
        return ESP_ERR_INVALID_RESPONSE;
    }

    // Check we have enough data
    if (start_pos + 68 > hb_count) {
        ESP_LOGD(TAG, "Not enough data after start: pos=%d, hb=%d", start_pos, hb_count);
        return ESP_ERR_INVALID_SIZE;
    }

    // Decode 32 data bits (skip start bit at start_pos)
    uint32_t decoded = 0;
    int bit_count = 0;

    for (int i = start_pos + 2; i + 1 < hb_count && bit_count < 32; i += 2) {
        uint8_t first = half_bits[i];
        uint8_t second = half_bits[i + 1];

        if (first == 0 && second == 1) {
            decoded = (decoded << 1) | 1;  // Bit '1'
            bit_count++;
        } else if (first == 1 && second == 0) {
            decoded = (decoded << 1) | 0;  // Bit '0'
            bit_count++;
        } else {
            // Invalid Manchester - skip
            continue;
        }
    }

    if (bit_count != 32) {
        ESP_LOGD(TAG, "Only decoded %d bits (need 32)", bit_count);
        return ESP_ERR_INVALID_RESPONSE;
    }

    // Check parity (even parity - total 1s should be even)
    uint32_t parity = decoded;
    int ones = 0;
    while (parity) {
        parity &= (parity - 1);
        ones++;
    }
    if (ones % 2 != 0) {
        ESP_LOGD(TAG, "Parity error");
        return ESP_ERR_INVALID_CRC;
    }

    *frame = decoded;
    return ESP_OK;
}

// ============================================================================
// RMT Callbacks
// ============================================================================

/**
 * RMT RX done callback - called when symbols are received
 */
static bool IRAM_ATTR thermostat_rx_callback(rmt_channel_handle_t channel,
                                              const rmt_rx_done_event_data_t *edata,
                                              void *user_ctx)
{
    BaseType_t high_task_wakeup = pdFALSE;
    thermostat_rx_ctx_t *rx_ctx = (thermostat_rx_ctx_t *)user_ctx;

    if (!rx_ctx || !rx_ctx->ctx) {
        return false;
    }

    ot_thermostat_t *ctx = rx_ctx->ctx;
    OpenThermRmtInterface *iface = &ctx->iface;

    // Copy symbol count to context
    size_t n = edata->num_symbols;
    if (n > 128) n = 128;
    iface->rx_symbol_count = n;
    iface->rx_pending = false;

    // Signal the task that symbols are ready
    if (iface->rx_queue) {
        xQueueSendFromISR(iface->rx_queue, &n, &high_task_wakeup);
    }

    return high_task_wakeup == pdTRUE;
}

/**
 * RMT TX done callback
 */
static bool IRAM_ATTR thermostat_tx_callback(rmt_channel_handle_t channel,
                                              const rmt_tx_done_event_data_t *edata,
                                              void *user_ctx)
{
    BaseType_t high_task_wakeup = pdFALSE;
    ot_thermostat_t *ctx = (ot_thermostat_t *)user_ctx;

    if (ctx && ctx->tx_done_sem) {
        xSemaphoreGiveFromISR(ctx->tx_done_sem, &high_task_wakeup);
    }

    return high_task_wakeup == pdTRUE;
}

/**
 * Start RMT receive.
 *
 * Uses 3ms idle timeout - long enough to not truncate stop bit.
 */
static esp_err_t start_rx(ot_thermostat_t *ctx)
{
    OpenThermRmtInterface *iface = &ctx->iface;

    if (iface->rx_pending) {
        return ESP_OK;  // Already receiving
    }

    // Always re-enable channel (RMT disables after receive completes)
    rmt_disable(iface->rx_channel);
    rmt_enable(iface->rx_channel);

    rmt_receive_config_t rx_config = {
        .signal_range_min_ns = 1000,        // 1µs minimum pulse
        .signal_range_max_ns = 3000000,     // 3ms idle ends capture
    };

    iface->rx_pending = true;
    return rmt_receive(iface->rx_channel, iface->rx_buffer,
                       iface->rx_buffer_size * sizeof(rmt_symbol_word_t), &rx_config);
}

/**
 * Receive a frame from the thermostat.
 *
 * Strategy: Each RMT capture ends at idle (2ms timeout). If a capture started
 * mid-frame, it will be incomplete - but after it ends, we're synchronized.
 * The NEXT capture will start at frame beginning and be complete.
 *
 * BLOCKING: This function blocks until a valid frame is received or timeout.
 */
static esp_err_t thermostat_receive_frame(ot_thermostat_t *ctx, uint32_t *frame,
                                           uint32_t timeout_ms)
{
    OpenThermRmtInterface *iface = &ctx->iface;
    TickType_t start_time = xTaskGetTickCount();
    TickType_t timeout_ticks = pdMS_TO_TICKS(timeout_ms);

    while ((xTaskGetTickCount() - start_time) < timeout_ticks) {
        // Start receiving
        esp_err_t ret = start_rx(ctx);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Failed to start RX: %s", esp_err_to_name(ret));
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        // Wait for symbols
        TickType_t elapsed = xTaskGetTickCount() - start_time;
        TickType_t remaining = (elapsed < timeout_ticks) ? (timeout_ticks - elapsed) : 0;

        size_t sym_count = 0;
        if (xQueueReceive(iface->rx_queue, &sym_count, remaining) != pdTRUE) {
            iface->rx_pending = false;
            continue;
        }

        iface->rx_pending = false;

        if (sym_count < 20) {
            ESP_LOGD(TAG, "Too few symbols: %d", sym_count);
            continue;
        }

        // Log first and last symbols for debugging
        ESP_LOGI(TAG, "RX %d syms: [0] L0=%d D0=%lu L1=%d D1=%lu  [%d] L0=%d D0=%lu L1=%d D1=%lu",
                 sym_count,
                 iface->rx_buffer[0].level0, (unsigned long)iface->rx_buffer[0].duration0,
                 iface->rx_buffer[0].level1, (unsigned long)iface->rx_buffer[0].duration1,
                 sym_count - 1,
                 iface->rx_buffer[sym_count-1].level0, (unsigned long)iface->rx_buffer[sym_count-1].duration0,
                 iface->rx_buffer[sym_count-1].level1, (unsigned long)iface->rx_buffer[sym_count-1].duration1);

        // Try to decode
        uint32_t decoded_frame;
        esp_err_t decode_ret = decode_manchester_symbols(
            iface->rx_buffer, sym_count, &decoded_frame);

        if (decode_ret == ESP_OK) {
            *frame = decoded_frame;
            ctx->stats.rx_count++;
            return ESP_OK;
        }

        // Decode failed - capture probably started mid-frame
        // Next capture will be synchronized (we just ended at idle)
        ESP_LOGD(TAG, "Decode failed: %s (will retry)", esp_err_to_name(decode_ret));
    }

    ctx->stats.timeout_count++;
    return ESP_ERR_TIMEOUT;
}

/**
 * Send a frame to the thermostat
 */
static esp_err_t thermostat_send_frame(ot_thermostat_t *ctx, uint32_t frame)
{
    OpenThermRmtInterface *iface = &ctx->iface;

    // Stop RX while transmitting to avoid capturing our own signal
    if (iface->rx_pending) {
        rmt_disable(iface->rx_channel);
        iface->rx_pending = false;
    }

    // Reset encoder
    rmt_encoder_reset(iface->encoder);

    rmt_transmit_config_t tx_config = {
        .loop_count = 0,
        .flags.eot_level = 1,  // Idle HIGH after transmission
        .flags.queue_nonblocking = false,
    };

    xSemaphoreTake(ctx->tx_done_sem, 0);

    esp_err_t ret = rmt_transmit(iface->tx_channel, iface->encoder, &frame, sizeof(frame), &tx_config);
    if (ret != ESP_OK) {
        rmt_enable(iface->rx_channel);
        return ret;
    }

    // BLOCKING: Wait for TX complete (~40ms)
    if (xSemaphoreTake(ctx->tx_done_sem, pdMS_TO_TICKS(100)) != pdTRUE) {
        rmt_enable(iface->rx_channel);
        return ESP_ERR_TIMEOUT;
    }

    // Small delay to let line settle before re-enabling RX
    vTaskDelay(pdMS_TO_TICKS(5));

    // Re-enable RX
    rmt_enable(iface->rx_channel);

    ctx->stats.tx_count++;
    return ESP_OK;
}

// ============================================================================
// Task
// ============================================================================

static void ot_thermostat_task(void *arg)
{
    ot_thermostat_t *ctx = (ot_thermostat_t *)arg;
    uint32_t request_frame, response_frame;
    ot_msg_t msg;

    ESP_LOGI(TAG, "Thermostat thread started");

    while (ctx->running) {
        // 1. BLOCKING: Wait for request from thermostat (up to 1100ms)
        esp_err_t ret = thermostat_receive_frame(ctx, &request_frame, 1100);

        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "RX: 0x%08lX", (unsigned long)request_frame);

            // 2. Put request on queue for main loop
            msg.data = request_frame;
            xQueueOverwrite(ctx->queues->thermostat_request, &msg);

            // 3. BLOCKING: Wait for response from main loop (up to 750ms)
            if (xQueueReceive(ctx->queues->thermostat_response, &msg, pdMS_TO_TICKS(750)) == pdTRUE) {
                response_frame = msg.data;
                ESP_LOGI(TAG, "TX: 0x%08lX", (unsigned long)response_frame);

                // 4. BLOCKING: Send response to thermostat (~40ms)
                ret = thermostat_send_frame(ctx, response_frame);
                if (ret != ESP_OK) {
                    ESP_LOGW(TAG, "TX failed: %s", esp_err_to_name(ret));
                }
            } else {
                ESP_LOGD(TAG, "No response from main loop");
            }
        } else if (ret != ESP_ERR_TIMEOUT) {
            ESP_LOGD(TAG, "RX error: %s", esp_err_to_name(ret));
            ctx->stats.error_count++;
        }
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
    if (opentherm_encoder_create(&ctx->iface.encoder) != ESP_OK) {
        rmt_del_channel(ctx->iface.tx_channel);
        vSemaphoreDelete(ctx->tx_done_sem);
        free(ctx);
        return NULL;
    }

    // Register TX callback
    rmt_tx_event_callbacks_t tx_cbs = { .on_trans_done = thermostat_tx_callback };
    rmt_tx_register_event_callbacks(ctx->iface.tx_channel, &tx_cbs, ctx);
    rmt_enable(ctx->iface.tx_channel);

    // Initialize RMT RX channel
    // Note: invert_in compensates for inverting hardware interface
    rmt_rx_channel_config_t rx_config = {
        .gpio_num = config->rx_pin,
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 1000000,
        .mem_block_symbols = 64,
        .flags.invert_in = true,      // Hardware inverts signal
        .flags.with_dma = false,
        .flags.io_loop_back = false,
    };

    // Enable internal pull-up for stable idle HIGH state
    gpio_set_pull_mode(config->rx_pin, GPIO_PULLUP_ONLY);

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
    s_rx_ctx = calloc(1, sizeof(thermostat_rx_ctx_t));
    s_rx_ctx->ctx = ctx;

    rmt_rx_event_callbacks_t rx_cbs = { .on_recv_done = thermostat_rx_callback };
    rmt_rx_register_event_callbacks(ctx->iface.rx_channel, &rx_cbs, s_rx_ctx);
    rmt_enable(ctx->iface.rx_channel);

    ctx->iface.rx_gpio = config->rx_pin;
    ctx->iface.tx_gpio = config->tx_pin;

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
