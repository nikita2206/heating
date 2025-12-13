/*
 * OpenTherm Manchester Encoder for ESP-IDF using RMT Peripheral
 *
 * This module provides Manchester encoding for OpenTherm frames using the ESP32's
 * RMT (Remote Control Transceiver) peripheral for precise timing.
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
#include "esp_check.h"
#include "driver/rmt_encoder.h"
#include <string.h>

static const char *TAG = "OpenThermRMT";

// ============================================================================
// OpenTherm Timing Constants
// ============================================================================

#define OT_HALF_BIT_US          500         // Half-bit duration in microseconds
#define OT_FRAME_BITS           34          // start(1) + data(32) + stop(1)

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

esp_err_t opentherm_encoder_create(rmt_encoder_handle_t *ret_encoder)
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
