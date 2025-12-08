/*
 * OpenTherm Library for ESP-IDF using RMT Peripheral
 * 
 * Implementation of OpenTherm protocol using ESP32's RMT for precise timing.
 * 
 * OpenTherm Protocol Overview:
 * - Manchester encoding at 1 kbit/s (1ms per bit, 500µs per half-bit)
 * - Frame structure: start bit + 32 data bits + stop bit = 34 bits total
 * - Manchester encoding: bit 1 = HIGH-LOW transition, bit 0 = LOW-HIGH transition
 * - Idle state: HIGH (no transmission)
 * - Start/stop bits are always '1' (HIGH-LOW)
 * 
 * RMT Configuration:
 * - Resolution: 1µs (1 MHz tick rate)
 * - Each Manchester bit uses one rmt_symbol_word_t (two level+duration pairs)
 */

#include "opentherm_rmt.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_timer.h"
#include "esp_check.h"
#include "driver/rmt_encoder.h"
#include <string.h>

static const char *TAG = "OpenThermRMT";

// ============================================================================
// OpenTherm Timing Constants
// ============================================================================

#define OT_RMT_RESOLUTION_HZ    1000000     // 1MHz = 1µs per tick
#define OT_HALF_BIT_US          500         // Half-bit duration in microseconds
#define OT_BIT_US               1000        // Full bit duration in microseconds
#define OT_TOLERANCE_US         150         // Timing tolerance for decoding
#define OT_FRAME_BITS           34          // start(1) + data(32) + stop(1)
#define OT_DATA_BITS            32          // Data bits only
#define OT_RESPONSE_TIMEOUT_MS  800         // Max wait for response (OpenTherm spec)
#define OT_GATEWAY_TIMEOUT_MS   1500        // Max wait for thermostat request

// RMT RX idle threshold: after this many ticks of no activity, frame is complete
// Set to ~3ms (3x bit period) to reliably detect end of frame
#define OT_RMT_IDLE_THRESHOLD_US 3000

// RMT memory block size (in symbols)
// OpenTherm frame: 34 bits = 34 symbols (each Manchester bit = 1 symbol)
// We need extra space for decoding, so use 64
#define OT_RMT_MEM_BLOCK_SYMBOLS 64

// RX buffer size for captured symbols
#define OT_RMT_RX_BUFFER_SIZE   128

// ============================================================================
// Manchester Encoder for TX
// ============================================================================

typedef struct {
    rmt_encoder_t base;
    rmt_encoder_handle_t copy_encoder;
    rmt_symbol_word_t symbols[OT_FRAME_BITS];
    size_t symbol_count;
    size_t symbols_written;
    int state;  // 0=sending symbols, 1=done
} opentherm_manchester_encoder_t;

/**
 * Encode a single Manchester bit into an RMT symbol
 * 
 * Manchester encoding for OpenTherm:
 * - Bit 1: First half HIGH (500µs), second half LOW (500µs) → HIGH-to-LOW transition at mid-bit
 * - Bit 0: First half LOW (500µs), second half HIGH (500µs) → LOW-to-HIGH transition at mid-bit
 */
static inline void encode_manchester_bit(rmt_symbol_word_t *symbol, bool bit_value)
{
    if (bit_value) {
        // Bit 1: HIGH then LOW
        symbol->level0 = 1;
        symbol->duration0 = OT_HALF_BIT_US;
        symbol->level1 = 0;
        symbol->duration1 = OT_HALF_BIT_US;
    } else {
        // Bit 0: LOW then HIGH
        symbol->level0 = 0;
        symbol->duration0 = OT_HALF_BIT_US;
        symbol->level1 = 1;
        symbol->duration1 = OT_HALF_BIT_US;
    }
}

/**
 * Encode a complete OpenTherm frame into RMT symbols
 * Frame structure: [start bit = 1] [32 data bits MSB first] [stop bit = 1]
 */
static void encode_opentherm_frame(rmt_symbol_word_t *symbols, uint32_t frame, size_t *count)
{
    size_t idx = 0;
    
    // Start bit (always 1)
    encode_manchester_bit(&symbols[idx++], true);
    
    // 32 data bits, MSB first
    for (int i = 31; i >= 0; i--) {
        bool bit = (frame >> i) & 1;
        encode_manchester_bit(&symbols[idx++], bit);
    }
    
    // Stop bit (always 1)
    encode_manchester_bit(&symbols[idx++], true);
    
    *count = idx;  // Should be 34
}

static size_t opentherm_encoder_encode(rmt_encoder_t *encoder, rmt_channel_handle_t channel,
                                        const void *primary_data, size_t data_size,
                                        rmt_encode_state_t *ret_state)
{
    opentherm_manchester_encoder_t *ot_encoder = __containerof(encoder, opentherm_manchester_encoder_t, base);
    rmt_encode_state_t session_state = RMT_ENCODING_RESET;
    size_t encoded_symbols = 0;
    
    if (ot_encoder->state == 0) {
        // First call - encode the frame
        if (ot_encoder->symbols_written == 0) {
            const uint32_t *frame = (const uint32_t *)primary_data;
            encode_opentherm_frame(ot_encoder->symbols, *frame, &ot_encoder->symbol_count);
        }
        
        // Copy symbols to RMT channel
        rmt_encoder_handle_t copy_encoder = ot_encoder->copy_encoder;
        encoded_symbols = copy_encoder->encode(copy_encoder, channel, 
                                                &ot_encoder->symbols[ot_encoder->symbols_written],
                                                (ot_encoder->symbol_count - ot_encoder->symbols_written) * sizeof(rmt_symbol_word_t),
                                                &session_state);
        
        ot_encoder->symbols_written += encoded_symbols;
        
        if (session_state & RMT_ENCODING_COMPLETE) {
            ot_encoder->state = 1;  // Done
        }
        if (session_state & RMT_ENCODING_MEM_FULL) {
            // Not done yet, will be called again
            *ret_state = (rmt_encode_state_t)(session_state & ~RMT_ENCODING_COMPLETE);
            return encoded_symbols;
        }
    }
    
    *ret_state = RMT_ENCODING_COMPLETE;
    return encoded_symbols;
}

static esp_err_t opentherm_encoder_reset(rmt_encoder_t *encoder)
{
    opentherm_manchester_encoder_t *ot_encoder = __containerof(encoder, opentherm_manchester_encoder_t, base);
    rmt_encoder_reset(ot_encoder->copy_encoder);
    ot_encoder->state = 0;
    ot_encoder->symbols_written = 0;
    ot_encoder->symbol_count = 0;
    return ESP_OK;
}

static esp_err_t opentherm_encoder_delete(rmt_encoder_t *encoder)
{
    opentherm_manchester_encoder_t *ot_encoder = __containerof(encoder, opentherm_manchester_encoder_t, base);
    rmt_del_encoder(ot_encoder->copy_encoder);
    free(ot_encoder);
    return ESP_OK;
}

static esp_err_t opentherm_encoder_create(rmt_encoder_handle_t *ret_encoder)
{
    opentherm_manchester_encoder_t *ot_encoder = calloc(1, sizeof(opentherm_manchester_encoder_t));
    ESP_RETURN_ON_FALSE(ot_encoder, ESP_ERR_NO_MEM, TAG, "Failed to allocate encoder");
    
    ot_encoder->base.encode = opentherm_encoder_encode;
    ot_encoder->base.reset = opentherm_encoder_reset;
    ot_encoder->base.del = opentherm_encoder_delete;
    
    // Create copy encoder for sending raw symbols
    rmt_copy_encoder_config_t copy_config = {};
    esp_err_t ret = rmt_new_copy_encoder(&copy_config, &ot_encoder->copy_encoder);
    if (ret != ESP_OK) {
        free(ot_encoder);
        return ret;
    }
    
    *ret_encoder = &ot_encoder->base;
    return ESP_OK;
}

// ============================================================================
// Manchester Decoder for RX
// ============================================================================

/**
 * Decode Manchester-encoded RMT symbols back to OpenTherm frame
 * 
 * Manchester encoding for OpenTherm:
 * - Bit 1: HIGH for first 500µs, LOW for second 500µs (mid-bit transition HIGH→LOW)
 * - Bit 0: LOW for first 500µs, HIGH for second 500µs (mid-bit transition LOW→HIGH)
 * 
 * RMT captures pulses as (level, duration) pairs. When consecutive bits differ,
 * there's an edge at the bit boundary. When they're the same, durations merge (~1000µs).
 * 
 * Decoding approach: Flatten to half-bit stream and decode based on level at each half-bit.
 * At the mid-bit point (second half-bit of each bit), the level determines the bit value:
 * - LOW at mid-bit = bit 1 (was HIGH→LOW transition)
 * - HIGH at mid-bit = bit 0 (was LOW→HIGH transition)
 */
static bool decode_manchester_symbols(const rmt_symbol_word_t *symbols, size_t symbol_count, 
                                       uint32_t *frame, bool *parity_ok)
{
    if (symbol_count < 1) {
        ESP_LOGW(TAG, "Decode: no symbols received");
        return false;
    }
    
    // Log what we received for debugging
    ESP_LOGI(TAG, "Decode: received %d symbols", symbol_count);
    for (size_t i = 0; i < symbol_count && i < 40; i++) {
        ESP_LOGI(TAG, "  [%d] l0=%d d0=%d, l1=%d d1=%d", 
                 i, symbols[i].level0, symbols[i].duration0,
                 symbols[i].level1, symbols[i].duration1);
    }
    if (symbol_count > 40) {
        ESP_LOGI(TAG, "  ... (%d more symbols)", symbol_count - 40);
    }
    
    // Flatten symbols into array of half-bit levels
    // Maximum: 34 bits * 2 half-bits = 68 half-bits, plus some margin
    uint8_t half_bits[80];
    int hb_count = 0;
    
    for (size_t i = 0; i < symbol_count && hb_count < 78; i++) {
        uint16_t durations[2] = { symbols[i].duration0, symbols[i].duration1 };
        uint8_t levels[2] = { symbols[i].level0, symbols[i].level1 };
        
        for (int part = 0; part < 2 && hb_count < 78; part++) {
            uint16_t dur = durations[part];
            uint8_t lvl = levels[part];
            
            if (dur == 0) continue;  // Skip end markers
            
            // Count how many half-bits this duration represents
            int num_half_bits;
            if (dur >= OT_HALF_BIT_US - OT_TOLERANCE_US && dur <= OT_HALF_BIT_US + OT_TOLERANCE_US) {
                num_half_bits = 1;
            } else if (dur >= OT_BIT_US - OT_TOLERANCE_US && dur <= OT_BIT_US + OT_TOLERANCE_US) {
                num_half_bits = 2;
            } else if (dur >= (OT_BIT_US + OT_HALF_BIT_US) - OT_TOLERANCE_US && 
                       dur <= (OT_BIT_US + OT_HALF_BIT_US) + OT_TOLERANCE_US) {
                num_half_bits = 3;
            } else {
                // Round to nearest half-bit
                num_half_bits = (dur + OT_HALF_BIT_US / 2) / OT_HALF_BIT_US;
                if (num_half_bits < 1) num_half_bits = 1;
                if (num_half_bits > 4) num_half_bits = 4;  // Cap at reasonable max
            }
            
            // Fill half-bit array
            for (int h = 0; h < num_half_bits && hb_count < 78; h++) {
                half_bits[hb_count++] = lvl;
            }
        }
    }
    
    ESP_LOGD(TAG, "Decode: flattened to %d half-bits", hb_count);
    
    // Now decode: look for start bit (1 = HIGH then LOW), then extract 32 data bits
    uint32_t decoded = 0;
    int bit_count = 0;
    bool found_start = false;
    
    for (int i = 0; i < hb_count - 1 && bit_count < 32; i += 2) {
        uint8_t first_half = half_bits[i];
        uint8_t second_half = half_bits[i + 1];
        
        // Manchester bit value is determined by second half level:
        // - Second half LOW (after HIGH first half) = bit 1
        // - Second half HIGH (after LOW first half) = bit 0
        // But we should also verify the transition exists
        
        bool bit_value;
        if (first_half == 1 && second_half == 0) {
            bit_value = true;  // Bit 1: HIGH→LOW
        } else if (first_half == 0 && second_half == 1) {
            bit_value = false;  // Bit 0: LOW→HIGH
        } else {
            // No transition at mid-bit - invalid Manchester, skip
            ESP_LOGD(TAG, "Decode: no mid-bit transition at half-bit %d (levels %d,%d)", 
                     i, first_half, second_half);
            continue;
        }
        
        if (!found_start) {
            // Looking for start bit (always 1)
            if (bit_value) {
                found_start = true;
                ESP_LOGD(TAG, "Decode: found start bit at half-bit %d", i);
            }
        } else {
            // Data bit
            decoded = (decoded << 1) | (bit_value ? 1 : 0);
            bit_count++;
        }
    }
    
    ESP_LOGI(TAG, "Decode: extracted %d bits, frame=0x%08lX", bit_count, (unsigned long)decoded);
    
    if (bit_count != 32) {
        ESP_LOGW(TAG, "Decode: incomplete frame, got %d bits (expected 32)", bit_count);
        return false;
    }
    
    *frame = decoded;
    *parity_ok = opentherm_rmt_check_parity(decoded);
    
    ESP_LOGI(TAG, "Decode: frame=0x%08lX parity=%s", (unsigned long)decoded, *parity_ok ? "OK" : "BAD");
    return true;
}

// ============================================================================
// RX Callback
// ============================================================================

typedef struct {
    OpenThermRmt *ot;
    OpenThermRmtInterface *iface;
    OpenThermRmtRole role;
} rx_callback_context_t;

static bool IRAM_ATTR rmt_rx_done_callback(rmt_channel_handle_t channel, 
                                            const rmt_rx_done_event_data_t *edata,
                                            void *user_ctx)
{
    BaseType_t high_task_wakeup = pdFALSE;
    rx_callback_context_t *ctx = (rx_callback_context_t *)user_ctx;
    
    if (ctx && ctx->iface) {
        // When en_partial_rx is enabled, we'd need to accumulate data until is_last is true.
        // Since we use en_partial_rx = false, each callback is a complete receive.
        // But check is_last anyway for safety.
        if (!edata->flags.is_last) {
            // Partial receive - shouldn't happen with en_partial_rx=false, but handle it
            // Just wait for the final chunk
            return high_task_wakeup == pdTRUE;
        }
        
        // Store symbol count directly in interface struct
        size_t copy_size = edata->num_symbols;
        if (copy_size > OT_RMT_RX_BUFFER_SIZE) {
            copy_size = OT_RMT_RX_BUFFER_SIZE;
        }
        ctx->iface->rx_symbol_count = copy_size;
        
        // Mark receive as complete
        ctx->iface->rx_pending = false;
        
        // Signal the waiting task
        if (ctx->iface->rx_queue) {
            xQueueSendFromISR(ctx->iface->rx_queue, &copy_size, &high_task_wakeup);
        }
    }
    
    return high_task_wakeup == pdTRUE;
}

// ============================================================================
// TX Done Callback
// ============================================================================

static bool IRAM_ATTR rmt_tx_done_callback(rmt_channel_handle_t channel,
                                            const rmt_tx_done_event_data_t *edata,
                                            void *user_ctx)
{
    BaseType_t high_task_wakeup = pdFALSE;
    OpenThermRmt *ot = (OpenThermRmt *)user_ctx;
    
    if (ot && ot->tx_done_sem) {
        xSemaphoreGiveFromISR(ot->tx_done_sem, &high_task_wakeup);
    }
    
    return high_task_wakeup == pdTRUE;
}

// ============================================================================
// Interface Initialization
// ============================================================================

static rx_callback_context_t *s_primary_rx_ctx = NULL;
static rx_callback_context_t *s_secondary_rx_ctx = NULL;

static esp_err_t init_interface(OpenThermRmt *ot, OpenThermRmtInterface *iface, 
                                 gpio_num_t rx_gpio, gpio_num_t tx_gpio,
                                 bool is_primary)
{
    esp_err_t ret;
    
    iface->rx_gpio = rx_gpio;
    iface->tx_gpio = tx_gpio;
    iface->rx_channel = NULL;
    iface->tx_channel = NULL;
    iface->encoder = NULL;
    iface->rx_queue = NULL;
    iface->rx_buffer = NULL;
    
    // ========== TX Channel Setup ==========
    rmt_tx_channel_config_t tx_config = {
        .gpio_num = tx_gpio,
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = OT_RMT_RESOLUTION_HZ,
        .mem_block_symbols = OT_RMT_MEM_BLOCK_SYMBOLS,
        .trans_queue_depth = 4,
        .flags = {
            .invert_out = false,
            .with_dma = false,
            .io_loop_back = false,
            .io_od_mode = false,  // Push-pull output
        }
    };
    
    ret = rmt_new_tx_channel(&tx_config, &iface->tx_channel);
    ESP_RETURN_ON_ERROR(ret, TAG, "Failed to create TX channel");
    
    // Create Manchester encoder
    ret = opentherm_encoder_create(&iface->encoder);
    if (ret != ESP_OK) {
        rmt_del_channel(iface->tx_channel);
        return ret;
    }
    
    // Register TX done callback (must be before enable)
    rmt_tx_event_callbacks_t tx_cbs = {
        .on_trans_done = rmt_tx_done_callback
    };
    ret = rmt_tx_register_event_callbacks(iface->tx_channel, &tx_cbs, ot);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to register TX callback: %s", esp_err_to_name(ret));
    }
    
    // Enable TX channel
    ret = rmt_enable(iface->tx_channel);
    if (ret != ESP_OK) {
        rmt_del_encoder(iface->encoder);
        rmt_del_channel(iface->tx_channel);
        return ret;
    }
    
    // ========== RX Channel Setup ==========
    rmt_rx_channel_config_t rx_config = {
        .gpio_num = rx_gpio,
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = OT_RMT_RESOLUTION_HZ,
        .mem_block_symbols = OT_RMT_MEM_BLOCK_SYMBOLS,
        .flags = {
            .invert_in = false,
            .with_dma = false,
            .io_loop_back = false,
        }
    };
    
    ret = rmt_new_rx_channel(&rx_config, &iface->rx_channel);
    if (ret != ESP_OK) {
        rmt_disable(iface->tx_channel);
        rmt_del_encoder(iface->encoder);
        rmt_del_channel(iface->tx_channel);
        return ret;
    }
    
    // Allocate RX buffer
    iface->rx_buffer = calloc(OT_RMT_RX_BUFFER_SIZE, sizeof(rmt_symbol_word_t));
    if (!iface->rx_buffer) {
        rmt_del_channel(iface->rx_channel);
        rmt_disable(iface->tx_channel);
        rmt_del_encoder(iface->encoder);
        rmt_del_channel(iface->tx_channel);
        return ESP_ERR_NO_MEM;
    }
    iface->rx_buffer_size = OT_RMT_RX_BUFFER_SIZE;
    
    // Create RX queue
    iface->rx_queue = xQueueCreate(4, sizeof(size_t));
    if (!iface->rx_queue) {
        free(iface->rx_buffer);
        rmt_del_channel(iface->rx_channel);
        rmt_disable(iface->tx_channel);
        rmt_del_encoder(iface->encoder);
        rmt_del_channel(iface->tx_channel);
        return ESP_ERR_NO_MEM;
    }
    
    // Set up RX callback context
    rx_callback_context_t *rx_ctx = calloc(1, sizeof(rx_callback_context_t));
    if (!rx_ctx) {
        vQueueDelete(iface->rx_queue);
        free(iface->rx_buffer);
        rmt_del_channel(iface->rx_channel);
        rmt_disable(iface->tx_channel);
        rmt_del_encoder(iface->encoder);
        rmt_del_channel(iface->tx_channel);
        return ESP_ERR_NO_MEM;
    }
    rx_ctx->ot = ot;
    rx_ctx->iface = iface;
    rx_ctx->role = is_primary ? OT_RMT_ROLE_MASTER : OT_RMT_ROLE_SLAVE;
    
    if (is_primary) {
        s_primary_rx_ctx = rx_ctx;
    } else {
        s_secondary_rx_ctx = rx_ctx;
    }
    
    // Register RX callback (must be before enable)
    rmt_rx_event_callbacks_t rx_cbs = {
        .on_recv_done = rmt_rx_done_callback
    };
    ret = rmt_rx_register_event_callbacks(iface->rx_channel, &rx_cbs, rx_ctx);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register RX callback: %s", esp_err_to_name(ret));
        if (is_primary) s_primary_rx_ctx = NULL; else s_secondary_rx_ctx = NULL;
        free(rx_ctx);
        vQueueDelete(iface->rx_queue);
        free(iface->rx_buffer);
        rmt_del_channel(iface->rx_channel);
        rmt_disable(iface->tx_channel);
        rmt_del_encoder(iface->encoder);
        rmt_del_channel(iface->tx_channel);
        return ret;
    }
    
    // Enable RX channel
    ret = rmt_enable(iface->rx_channel);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to enable RX channel: %s", esp_err_to_name(ret));
        if (is_primary) s_primary_rx_ctx = NULL; else s_secondary_rx_ctx = NULL;
        free(rx_ctx);
        vQueueDelete(iface->rx_queue);
        free(iface->rx_buffer);
        rmt_del_channel(iface->rx_channel);
        rmt_disable(iface->tx_channel);
        rmt_del_encoder(iface->encoder);
        rmt_del_channel(iface->tx_channel);
        return ret;
    }
    
    ESP_LOGI(TAG, "Interface initialized: RX=GPIO%d, TX=GPIO%d", rx_gpio, tx_gpio);
    return ESP_OK;
}

static void deinit_interface(OpenThermRmtInterface *iface, bool is_primary)
{
    if (iface->rx_channel) {
        rmt_disable(iface->rx_channel);
        rmt_del_channel(iface->rx_channel);
        iface->rx_channel = NULL;
    }
    
    if (iface->tx_channel) {
        rmt_disable(iface->tx_channel);
        rmt_del_channel(iface->tx_channel);
        iface->tx_channel = NULL;
    }
    
    if (iface->encoder) {
        rmt_del_encoder(iface->encoder);
        iface->encoder = NULL;
    }
    
    if (iface->rx_queue) {
        vQueueDelete(iface->rx_queue);
        iface->rx_queue = NULL;
    }
    
    if (iface->rx_buffer) {
        free(iface->rx_buffer);
        iface->rx_buffer = NULL;
    }
    
    // Free callback context
    if (is_primary && s_primary_rx_ctx) {
        free(s_primary_rx_ctx);
        s_primary_rx_ctx = NULL;
    } else if (!is_primary && s_secondary_rx_ctx) {
        free(s_secondary_rx_ctx);
        s_secondary_rx_ctx = NULL;
    }
}

// ============================================================================
// Public API Implementation
// ============================================================================

esp_err_t opentherm_rmt_init(OpenThermRmt *ot, gpio_num_t rx_pin, gpio_num_t tx_pin, OpenThermRmtRole role)
{
    ESP_RETURN_ON_FALSE(ot != NULL, ESP_ERR_INVALID_ARG, TAG, "NULL ot pointer");
    
    memset(ot, 0, sizeof(OpenThermRmt));
    
    ot->role = role;
    ot->gateway_mode = false;
    ot->status = OT_RMT_STATUS_IDLE;
    
    // Create synchronization primitives
    ot->tx_done_sem = xSemaphoreCreateBinary();
    ot->rx_done_sem = xSemaphoreCreateBinary();
    
    if (!ot->tx_done_sem || !ot->rx_done_sem) {
        if (ot->tx_done_sem) vSemaphoreDelete(ot->tx_done_sem);
        if (ot->rx_done_sem) vSemaphoreDelete(ot->rx_done_sem);
        return ESP_ERR_NO_MEM;
    }
    
    // Initialize primary interface
    esp_err_t ret = init_interface(ot, &ot->primary, rx_pin, tx_pin, true);
    if (ret != ESP_OK) {
        vSemaphoreDelete(ot->tx_done_sem);
        vSemaphoreDelete(ot->rx_done_sem);
        return ret;
    }
    
    ESP_LOGI(TAG, "OpenTherm RMT initialized (role: %s)", 
             role == OT_RMT_ROLE_MASTER ? "MASTER" : "SLAVE");
    
    return ESP_OK;
}

esp_err_t opentherm_rmt_init_gateway(OpenThermRmt *ot,
                                      gpio_num_t master_rx_pin, gpio_num_t master_tx_pin,
                                      gpio_num_t slave_rx_pin, gpio_num_t slave_tx_pin)
{
    ESP_RETURN_ON_FALSE(ot != NULL, ESP_ERR_INVALID_ARG, TAG, "NULL ot pointer");
    
    memset(ot, 0, sizeof(OpenThermRmt));
    
    ot->role = OT_RMT_ROLE_SLAVE;  // Default role (receives from master/thermostat first)
    ot->gateway_mode = true;
    ot->status = OT_RMT_STATUS_IDLE;
    ot->gateway_state = OT_RMT_GATEWAY_STATE_IDLE;
    
    // Create synchronization primitives
    ot->tx_done_sem = xSemaphoreCreateBinary();
    ot->rx_done_sem = xSemaphoreCreateBinary();
    
    if (!ot->tx_done_sem || !ot->rx_done_sem) {
        if (ot->tx_done_sem) vSemaphoreDelete(ot->tx_done_sem);
        if (ot->rx_done_sem) vSemaphoreDelete(ot->rx_done_sem);
        return ESP_ERR_NO_MEM;
    }
    
    // Initialize primary interface (master side - connects to thermostat)
    // We receive requests FROM thermostat and send responses TO thermostat
    esp_err_t ret = init_interface(ot, &ot->primary, master_rx_pin, master_tx_pin, true);
    if (ret != ESP_OK) {
        vSemaphoreDelete(ot->tx_done_sem);
        vSemaphoreDelete(ot->rx_done_sem);
        return ret;
    }
    
    // Initialize secondary interface (slave side - connects to boiler)
    // We send requests TO boiler and receive responses FROM boiler
    ret = init_interface(ot, &ot->secondary, slave_rx_pin, slave_tx_pin, false);
    if (ret != ESP_OK) {
        deinit_interface(&ot->primary, true);
        vSemaphoreDelete(ot->tx_done_sem);
        vSemaphoreDelete(ot->rx_done_sem);
        return ret;
    }
    
    ESP_LOGI(TAG, "OpenTherm RMT gateway initialized");
    ESP_LOGI(TAG, "  Master side (thermostat): RX=GPIO%d, TX=GPIO%d", master_rx_pin, master_tx_pin);
    ESP_LOGI(TAG, "  Slave side (boiler): RX=GPIO%d, TX=GPIO%d", slave_rx_pin, slave_tx_pin);
    
    return ESP_OK;
}

esp_err_t opentherm_rmt_start(OpenThermRmt *ot)
{
    ESP_RETURN_ON_FALSE(ot != NULL, ESP_ERR_INVALID_ARG, TAG, "NULL ot pointer");
    
    ot->status = OT_RMT_STATUS_IDLE;
    
    ESP_LOGI(TAG, "OpenTherm RMT started");
    return ESP_OK;
}

esp_err_t opentherm_rmt_stop(OpenThermRmt *ot)
{
    ESP_RETURN_ON_FALSE(ot != NULL, ESP_ERR_INVALID_ARG, TAG, "NULL ot pointer");
    
    // Disable channels
    if (ot->primary.rx_channel) {
        rmt_disable(ot->primary.rx_channel);
    }
    if (ot->primary.tx_channel) {
        rmt_disable(ot->primary.tx_channel);
    }
    
    if (ot->gateway_mode) {
        if (ot->secondary.rx_channel) {
            rmt_disable(ot->secondary.rx_channel);
        }
        if (ot->secondary.tx_channel) {
            rmt_disable(ot->secondary.tx_channel);
        }
    }
    
    ot->status = OT_RMT_STATUS_IDLE;
    return ESP_OK;
}

esp_err_t opentherm_rmt_deinit(OpenThermRmt *ot)
{
    ESP_RETURN_ON_FALSE(ot != NULL, ESP_ERR_INVALID_ARG, TAG, "NULL ot pointer");
    
    opentherm_rmt_stop(ot);
    
    deinit_interface(&ot->primary, true);
    
    if (ot->gateway_mode) {
        deinit_interface(&ot->secondary, false);
    }
    
    if (ot->tx_done_sem) {
        vSemaphoreDelete(ot->tx_done_sem);
        ot->tx_done_sem = NULL;
    }
    
    if (ot->rx_done_sem) {
        vSemaphoreDelete(ot->rx_done_sem);
        ot->rx_done_sem = NULL;
    }
    
    return ESP_OK;
}

void opentherm_rmt_set_message_callback(OpenThermRmt *ot, opentherm_rmt_message_callback_t callback, void *user_data)
{
    if (ot) {
        ot->message_callback = callback;
        ot->user_data = user_data;
    }
}

// ============================================================================
// Frame Transmission
// ============================================================================

esp_err_t opentherm_rmt_send_frame(OpenThermRmt *ot, uint32_t frame, OpenThermRmtInterface *iface)
{
    ESP_RETURN_ON_FALSE(ot != NULL, ESP_ERR_INVALID_ARG, TAG, "NULL ot pointer");
    ESP_RETURN_ON_FALSE(iface != NULL, ESP_ERR_INVALID_ARG, TAG, "NULL iface pointer");
    ESP_RETURN_ON_FALSE(iface->tx_channel != NULL, ESP_ERR_INVALID_STATE, TAG, "TX channel not initialized");
    ESP_RETURN_ON_FALSE(iface->encoder != NULL, ESP_ERR_INVALID_STATE, TAG, "Encoder not initialized");
    
    // Reset encoder state
    rmt_encoder_reset(iface->encoder);
    
    // Transmit configuration
    rmt_transmit_config_t tx_config = {
        .loop_count = 0,  // No looping
        .flags = {
            .eot_level = 1,  // End at HIGH (OpenTherm idle state)
            .queue_nonblocking = false,
        }
    };
    
    // Clear semaphore before transmit
    xSemaphoreTake(ot->tx_done_sem, 0);
    
    // Start transmission
    esp_err_t ret = rmt_transmit(iface->tx_channel, iface->encoder, &frame, sizeof(frame), &tx_config);
    ESP_RETURN_ON_ERROR(ret, TAG, "Failed to start RMT transmit");
    
    // Wait for transmission to complete (with timeout)
    // Frame is ~34ms, so 100ms timeout is plenty
    if (xSemaphoreTake(ot->tx_done_sem, pdMS_TO_TICKS(100)) != pdTRUE) {
        ESP_LOGW(TAG, "TX timeout waiting for completion");
        return ESP_ERR_TIMEOUT;
    }
    
    ot->tx_count++;
    return ESP_OK;
}

// ============================================================================
// Frame Reception
// ============================================================================

/**
 * Start RX and wait for a frame with timeout
 * 
 * Note: rmt_receive() is non-blocking. We must ensure the previous receive
 * has completed before starting a new one.
 */
static esp_err_t receive_frame(OpenThermRmt *ot, OpenThermRmtInterface *iface, 
                                uint32_t *frame, uint32_t timeout_ms)
{
    ESP_RETURN_ON_FALSE(iface->rx_channel != NULL, ESP_ERR_INVALID_STATE, TAG, "RX channel not initialized");
    
    // If a previous receive is still pending (e.g., from timeout), reset the channel
    if (iface->rx_pending) {
        ESP_LOGD(TAG, "Previous RX still pending, resetting channel");
        rmt_disable(iface->rx_channel);
        rmt_enable(iface->rx_channel);
        iface->rx_pending = false;
    }
    
    // Configure receive parameters
    // signal_range_min_ns: filter out glitches shorter than this (hardware filter)
    // signal_range_max_ns: pulses longer than this trigger idle detection
    rmt_receive_config_t rx_config = {
        .signal_range_min_ns = 1000,        // 1µs minimum - filter out very short glitches
        .signal_range_max_ns = 3500000,     // 3.5ms - longer than any valid Manchester pulse
        .flags = {
            .en_partial_rx = false,
        }
    };
    
    // Clear the queue and symbol count
    size_t dummy;
    while (xQueueReceive(iface->rx_queue, &dummy, 0) == pdTRUE) {}
    iface->rx_symbol_count = 0;
    
    // Mark receive as pending BEFORE calling rmt_receive
    iface->rx_pending = true;
    
    // Start receive (non-blocking - returns immediately)
    esp_err_t ret = rmt_receive(iface->rx_channel, iface->rx_buffer, 
                                 iface->rx_buffer_size * sizeof(rmt_symbol_word_t), &rx_config);
    if (ret != ESP_OK) {
        iface->rx_pending = false;
        ESP_LOGW(TAG, "Failed to start RMT receive: %s", esp_err_to_name(ret));
        return ret;
    }
    
    // Wait for frame reception (callback will clear rx_pending and send to queue)
    size_t symbol_count = 0;
    if (xQueueReceive(iface->rx_queue, &symbol_count, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        // Timeout - rx_pending is still true, will be reset on next call
        ESP_LOGD(TAG, "RX timeout after %lums", (unsigned long)timeout_ms);
        ot->timeout_count++;
        return ESP_ERR_TIMEOUT;
    }
    
    // Receive completed successfully (callback already cleared rx_pending)
    
    ESP_LOGD(TAG, "Received %d RMT symbols", symbol_count);
    
    // Debug: print received symbols
    for (size_t i = 0; i < symbol_count && i < 40; i++) {
        ESP_LOGD(TAG, "  Symbol[%d]: l0=%d d0=%d l1=%d d1=%d",
                 i, iface->rx_buffer[i].level0, iface->rx_buffer[i].duration0,
                 iface->rx_buffer[i].level1, iface->rx_buffer[i].duration1);
    }
    
    // Decode Manchester symbols
    bool parity_ok;
    if (!decode_manchester_symbols(iface->rx_buffer, symbol_count, frame, &parity_ok)) {
        ESP_LOGW(TAG, "Failed to decode Manchester frame");
        ot->error_count++;
        // Wait for any in-progress frame to complete (~40ms for full frame)
        // This helps resync with frame boundaries
        vTaskDelay(pdMS_TO_TICKS(50));
        return ESP_ERR_INVALID_RESPONSE;
    }
    
    if (!parity_ok) {
        ESP_LOGW(TAG, "Frame parity check failed: 0x%08lX", (unsigned long)*frame);
        ot->error_count++;
        vTaskDelay(pdMS_TO_TICKS(50));  // Resync delay
        return ESP_ERR_INVALID_CRC;
    }
    
    ot->rx_count++;
    ot->last_frame = *frame;
    
    return ESP_OK;
}

// ============================================================================
// High-Level Communication
// ============================================================================

esp_err_t opentherm_rmt_send_request(OpenThermRmt *ot, OpenThermRmtMessage *request,
                                      OpenThermRmtMessage *response, uint32_t timeout_ms)
{
    ESP_RETURN_ON_FALSE(ot != NULL, ESP_ERR_INVALID_ARG, TAG, "NULL ot pointer");
    ESP_RETURN_ON_FALSE(request != NULL, ESP_ERR_INVALID_ARG, TAG, "NULL request pointer");
    
    ot->status = OT_RMT_STATUS_TRANSMITTING;
    
    // Send the request frame
    esp_err_t ret = opentherm_rmt_send_frame(ot, request->data, &ot->primary);
    if (ret != ESP_OK) {
        ot->status = OT_RMT_STATUS_ERROR;
        return ret;
    }
    
    // Wait for response
    ot->status = OT_RMT_STATUS_WAITING_RESPONSE;
    
    uint32_t rx_frame = 0;
    ret = receive_frame(ot, &ot->primary, &rx_frame, timeout_ms);
    
    if (ret == ESP_OK) {
        ot->status = OT_RMT_STATUS_FRAME_READY;
        if (response) {
            response->data = rx_frame;
        }
    } else if (ret == ESP_ERR_TIMEOUT) {
        ot->status = OT_RMT_STATUS_TIMEOUT;
    } else {
        ot->status = OT_RMT_STATUS_ERROR;
    }
    
    return ret;
}

esp_err_t opentherm_rmt_send_response(OpenThermRmt *ot, OpenThermRmtMessage *response)
{
    ESP_RETURN_ON_FALSE(ot != NULL, ESP_ERR_INVALID_ARG, TAG, "NULL ot pointer");
    ESP_RETURN_ON_FALSE(response != NULL, ESP_ERR_INVALID_ARG, TAG, "NULL response pointer");
    
    return opentherm_rmt_send_frame(ot, response->data, &ot->primary);
}

// ============================================================================
// Gateway Mode
// ============================================================================

void opentherm_rmt_gateway_reset(OpenThermRmt *ot)
{
    if (ot == NULL || !ot->gateway_mode) {
        return;
    }
    
    ot->gateway_state = OT_RMT_GATEWAY_STATE_IDLE;
    ot->status = OT_RMT_STATUS_IDLE;
    ot->gateway_timeout_flag = false;
    ot->gateway_timer_start = esp_timer_get_time();
}

bool opentherm_rmt_gateway_process(OpenThermRmt *ot, OpenThermRmtMessage *request, OpenThermRmtMessage *response)
{
    if (ot == NULL || !ot->gateway_mode) {
        return false;
    }
    
    int64_t now = esp_timer_get_time();
    uint32_t frame = 0;
    esp_err_t ret;
    
    switch (ot->gateway_state) {
        case OT_RMT_GATEWAY_STATE_IDLE:
            // Start listening for thermostat request
            ot->gateway_state = OT_RMT_GATEWAY_STATE_WAITING_REQUEST;
            ot->gateway_timer_start = now;
            ot->gateway_timeout_flag = false;
            // Fall through to start receiving
            __attribute__((fallthrough));
            
        case OT_RMT_GATEWAY_STATE_WAITING_REQUEST:
            // Try to receive a frame from thermostat (master side)
            // Use longer timeout (~1.1s) to avoid resetting channel mid-frame
            // Thermostat sends requests roughly every 1 second
            // RMT idle detection will end receive when frame completes
            ret = receive_frame(ot, &ot->primary, &frame, 1100);
            
            if (ret == ESP_OK) {
                // Validate the frame
                if (!opentherm_rmt_check_parity(frame)) {
                    ESP_LOGW(TAG, "Gateway: Invalid parity in request 0x%08lX", (unsigned long)frame);
                    break;
                }
                if (!opentherm_rmt_is_valid_request_type(frame)) {
                    ESP_LOGW(TAG, "Gateway: Invalid request type in 0x%08lX", (unsigned long)frame);
                    break;
                }
                
                ot->gateway_request.data = frame;
                
                // Log the request
                if (ot->message_callback) {
                    ot->message_callback(ot, &ot->gateway_request, OT_RMT_ROLE_MASTER);
                }
                
                // Forward to boiler
                ot->gateway_state = OT_RMT_GATEWAY_STATE_FORWARDING_REQUEST;
                
            } else if ((now - ot->gateway_timer_start) > (OT_GATEWAY_TIMEOUT_MS * 1000LL)) {
                // Reset timeout
                ot->gateway_timer_start = now;
            }
            break;
            
        case OT_RMT_GATEWAY_STATE_FORWARDING_REQUEST:
            // Forward the captured request to the boiler
            ret = opentherm_rmt_send_frame(ot, ot->gateway_request.data, &ot->secondary);
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "Gateway: Failed to forward request to boiler");
                ot->gateway_state = OT_RMT_GATEWAY_STATE_WAITING_REQUEST;
                ot->gateway_timer_start = now;
                break;
            }
            
            // Now wait for boiler response
            ot->gateway_state = OT_RMT_GATEWAY_STATE_WAITING_RESPONSE;
            ot->gateway_timer_start = now;
            break;
            
        case OT_RMT_GATEWAY_STATE_WAITING_RESPONSE:
            // Try to receive response from boiler (slave side)
            // OpenTherm spec allows 800ms - use full timeout
            // RMT idle detection will end receive when frame completes
            ret = receive_frame(ot, &ot->secondary, &frame, 800);
            
            if (ret == ESP_OK) {
                // Validate the frame
                if (!opentherm_rmt_check_parity(frame)) {
                    ESP_LOGW(TAG, "Gateway: Invalid parity in response 0x%08lX", (unsigned long)frame);
                    ot->gateway_state = OT_RMT_GATEWAY_STATE_WAITING_REQUEST;
                    ot->gateway_timer_start = now;
                    break;
                }
                if (!opentherm_rmt_is_valid_response_type(frame)) {
                    ESP_LOGW(TAG, "Gateway: Invalid response type in 0x%08lX", (unsigned long)frame);
                    ot->gateway_state = OT_RMT_GATEWAY_STATE_WAITING_REQUEST;
                    ot->gateway_timer_start = now;
                    break;
                }
                
                ot->gateway_response.data = frame;
                
                // Log the response
                if (ot->message_callback) {
                    ot->message_callback(ot, &ot->gateway_response, OT_RMT_ROLE_SLAVE);
                }
                
                // Forward to thermostat
                ot->gateway_state = OT_RMT_GATEWAY_STATE_FORWARDING_RESPONSE;
                
            } else if ((now - ot->gateway_timer_start) > (OT_RESPONSE_TIMEOUT_MS * 1000LL)) {
                // Boiler didn't respond in time
                ESP_LOGW(TAG, "Gateway: Boiler response timeout");
                ot->gateway_timeout_flag = true;
                ot->timeout_count++;
                ot->gateway_state = OT_RMT_GATEWAY_STATE_WAITING_REQUEST;
                ot->gateway_timer_start = now;
            }
            break;
            
        case OT_RMT_GATEWAY_STATE_FORWARDING_RESPONSE:
            // Forward the captured response to the thermostat
            ret = opentherm_rmt_send_frame(ot, ot->gateway_response.data, &ot->primary);
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "Gateway: Failed to forward response to thermostat");
            }
            
            // Copy to output buffers
            if (request) {
                *request = ot->gateway_request;
            }
            if (response) {
                *response = ot->gateway_response;
            }
            
            // Transaction complete - go back to waiting
            ot->gateway_timeout_flag = false;
            ot->gateway_state = OT_RMT_GATEWAY_STATE_WAITING_REQUEST;
            ot->gateway_timer_start = now;
            
            return true;  // Complete transaction proxied
    }
    
    return false;
}

// ============================================================================
// Message Construction/Parsing
// ============================================================================

uint32_t opentherm_rmt_build_request(OpenThermRmtMessageType type, uint8_t id, uint16_t data)
{
    uint32_t request = ((uint32_t)type << 28) | ((uint32_t)id << 16) | data;
    
    // Calculate parity bit (even parity)
    uint8_t parity = 0;
    uint32_t temp = request;
    while (temp) {
        parity ^= (temp & 1);
        temp >>= 1;
    }
    request |= ((uint32_t)parity << 31);
    
    return request;
}

uint32_t opentherm_rmt_build_response(OpenThermRmtMessageType type, uint8_t id, uint16_t data)
{
    return opentherm_rmt_build_request(type, id, data);
}

OpenThermRmtMessageType opentherm_rmt_get_message_type(uint32_t message)
{
    return (OpenThermRmtMessageType)((message >> 28) & 0x7);
}

uint8_t opentherm_rmt_get_data_id(uint32_t message)
{
    return (uint8_t)((message >> 16) & 0xFF);
}

uint16_t opentherm_rmt_get_uint16(uint32_t message)
{
    return (uint16_t)(message & 0xFFFF);
}

float opentherm_rmt_get_float(uint32_t message)
{
    uint16_t data = opentherm_rmt_get_uint16(message);
    return (float)((int16_t)data) / 256.0f;
}

uint8_t opentherm_rmt_get_uint8_hb(uint32_t message)
{
    return (uint8_t)((message >> 8) & 0xFF);
}

uint8_t opentherm_rmt_get_uint8_lb(uint32_t message)
{
    return (uint8_t)(message & 0xFF);
}

int8_t opentherm_rmt_get_int8(uint32_t message)
{
    return (int8_t)(message & 0xFF);
}

// ============================================================================
// Validation
// ============================================================================

bool opentherm_rmt_check_parity(uint32_t frame)
{
    // Count 1 bits - should be even for valid frame
    uint32_t count = 0;
    uint32_t n = frame;
    while (n) {
        n &= (n - 1);
        count++;
    }
    return (count % 2) == 0;
}

bool opentherm_rmt_is_valid_request_type(uint32_t frame)
{
    OpenThermRmtMessageType type = opentherm_rmt_get_message_type(frame);
    return type <= OT_RMT_MSGTYPE_RESERVED;
}

bool opentherm_rmt_is_valid_response_type(uint32_t frame)
{
    OpenThermRmtMessageType type = opentherm_rmt_get_message_type(frame);
    return type >= OT_RMT_MSGTYPE_READ_ACK;
}

bool opentherm_rmt_is_valid_response(uint32_t request, uint32_t response)
{
    return opentherm_rmt_get_data_id(request) == opentherm_rmt_get_data_id(response);
}

// ============================================================================
// Utility/Debug
// ============================================================================

const char* opentherm_rmt_message_type_to_string(OpenThermRmtMessageType type)
{
    switch (type) {
        case OT_RMT_MSGTYPE_READ_DATA: return "READ_DATA";
        case OT_RMT_MSGTYPE_WRITE_DATA: return "WRITE_DATA";
        case OT_RMT_MSGTYPE_INVALID_DATA: return "INVALID_DATA";
        case OT_RMT_MSGTYPE_RESERVED: return "RESERVED";
        case OT_RMT_MSGTYPE_READ_ACK: return "READ_ACK";
        case OT_RMT_MSGTYPE_WRITE_ACK: return "WRITE_ACK";
        case OT_RMT_MSGTYPE_DATA_INVALID: return "DATA_INVALID";
        case OT_RMT_MSGTYPE_UNKNOWN_DATAID: return "UNKNOWN_DATAID";
        default: return "UNKNOWN";
    }
}

const char* opentherm_rmt_status_to_string(OpenThermRmtStatus status)
{
    switch (status) {
        case OT_RMT_STATUS_IDLE: return "IDLE";
        case OT_RMT_STATUS_TRANSMITTING: return "TRANSMITTING";
        case OT_RMT_STATUS_WAITING_RESPONSE: return "WAITING_RESPONSE";
        case OT_RMT_STATUS_RECEIVING: return "RECEIVING";
        case OT_RMT_STATUS_FRAME_READY: return "FRAME_READY";
        case OT_RMT_STATUS_TIMEOUT: return "TIMEOUT";
        case OT_RMT_STATUS_ERROR: return "ERROR";
        default: return "UNKNOWN";
    }
}

OpenThermRmtStatus opentherm_rmt_get_status(OpenThermRmt *ot)
{
    return ot ? ot->status : OT_RMT_STATUS_ERROR;
}

