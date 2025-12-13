/*
 * OpenTherm Manchester Encoder for ESP-IDF using RMT Peripheral
 *
 * This module provides Manchester encoding for OpenTherm frames using the ESP32's
 * RMT (Remote Control Transceiver) peripheral for precise timing.
 *
 * OpenTherm Protocol Overview:
 * - Manchester encoding at 1 kbit/s (1ms per bit, 500µs per half-bit)
 * - Frame structure: start bit + 32 data bits + stop bit = 34 bits total
 * - Manchester encoding: bit 1 = LOW-HIGH transition, bit 0 = HIGH-LOW transition
 * - Idle state: HIGH (no transmission)
 * - Start/stop bits are always '1' (LOW-HIGH)
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
 * - Bit 1: First half LOW (500µs), second half HIGH (500µs) → LOW-to-HIGH transition at mid-bit
 * - Bit 0: First half HIGH (500µs), second half LOW (500µs) → HIGH-to-LOW transition at mid-bit
 */
static inline void encode_manchester_bit(rmt_symbol_word_t *symbol, bool bit_value)
{
    if (bit_value) {
        // Bit 1: LOW then HIGH
        symbol->level0 = 0;
        symbol->duration0 = OT_HALF_BIT_US;
        symbol->level1 = 1;
        symbol->duration1 = OT_HALF_BIT_US;
    } else {
        // Bit 0: HIGH then LOW
        symbol->level0 = 1;
        symbol->duration0 = OT_HALF_BIT_US;
        symbol->level1 = 0;
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

// ============================================================================
// Manchester Decoder for RX
// ============================================================================

// Frame validation constants
#define OT_MIN_HALF_BITS    66      // Minimum for complete frame (34 bits * 2 - tolerance)
#define OT_MAX_HALF_BITS    80      // Maximum expected (with some idle capture)

/**
 * Find the start bit in the half-bit stream.
 *
 * Scans ONE half-bit at a time looking for the `01` pattern (bit '1').
 * This handles cases where there's idle capture or frame tail at the start.
 *
 * @return Index where start bit begins, or -1 if not found
 */
static int find_start_bit(const uint8_t *half_bits, int hb_count)
{
    // Need at least 68 half-bits from start position for a complete frame
    // So don't look too far into the stream
    int max_search = hb_count - 68;
    if (max_search < 0) max_search = 0;

    for (int i = 0; i <= max_search; i++) {
        if (i + 1 < hb_count &&
            half_bits[i] == 0 && half_bits[i + 1] == 1) {
            // Found `01` pattern = bit '1' = potential start bit
            return i;
        }
    }
    return -1;
}

static void decode_manchester_from_position(const uint8_t *half_bits, int hb_count,
                                             int start_pos, opentherm_decode_result_t *result)
{
    result->frame = 0;
    result->errors = 0;
    result->start_bit_valid = false;
    result->stop_bit_valid = false;
    result->bits_decoded = 0;

    if (start_pos < 0 || start_pos + 1 >= hb_count) {
        return;
    }

    // Verify start bit at start_pos
    if (half_bits[start_pos] != 0 || half_bits[start_pos + 1] != 1) {
        return;  // Not a valid start bit
    }

    result->start_bit_valid = true;
    result->bits_decoded = 1;  // Count start bit

    uint32_t decoded = 0;
    int data_bits = 0;

    // Decode 32 data bits starting from position after start bit
    for (int i = start_pos + 2; i + 1 < hb_count && data_bits < 32; i += 2) {
        uint8_t first_half = half_bits[i];
        uint8_t second_half = half_bits[i + 1];

        bool bit_value;
        if (first_half == 0 && second_half == 1) {
            bit_value = true;   // Bit 1: LOW-HIGH transition
        } else if (first_half == 1 && second_half == 0) {
            bit_value = false;  // Bit 0: HIGH-LOW transition
        } else {
            // Invalid Manchester transition (1,1 or 0,0)
            result->errors++;
            continue;
        }

        decoded = (decoded << 1) | (bit_value ? 1 : 0);
        data_bits++;
        result->bits_decoded++;
    }

    result->frame = decoded;

    // Check stop bit (should be at position start_pos + 2 + 64 = start_pos + 66)
    int stop_pos = start_pos + 66;  // 2 (start) + 64 (32 data bits)
    if (stop_pos + 1 < hb_count) {
        if (half_bits[stop_pos] == 0 && half_bits[stop_pos + 1] == 1) {
            result->stop_bit_valid = true;
            result->bits_decoded++;
        }
    }
}

/**
 * Check parity of a 32-bit OpenTherm frame (even parity)
 * Returns true if parity is correct (even number of 1s)
 */
static bool check_frame_parity(uint32_t frame)
{
    uint32_t n = frame;
    uint32_t count = 0;
    while (n) {
        n &= (n - 1);
        count++;
    }
    return (count % 2) == 0;
}

esp_err_t opentherm_decode_frame(const uint8_t *half_bits, int hb_count, uint32_t *frame)
{
    // Pre-check: enough half-bits for a complete frame?
    if (hb_count < OT_MIN_HALF_BITS) {
        ESP_LOGI(TAG, "Partial frame: only %d half-bits (need >=%d)", hb_count, OT_MIN_HALF_BITS);
        return ESP_ERR_INVALID_SIZE;
    }

    // Find the start bit by scanning one half-bit at a time
    // This handles idle capture or frame tail at the start of the buffer
    int start_pos = find_start_bit(half_bits, hb_count);
    if (start_pos < 0) {
        ESP_LOGI(TAG, "Decode failed: no start bit found in %d half-bits", hb_count);
        return ESP_ERR_INVALID_RESPONSE;
    }

    // Check if we have enough half-bits from the start position
    int remaining = hb_count - start_pos;
    if (remaining < 68) {
        ESP_LOGI(TAG, "Decode failed: start at %d, only %d half-bits remaining (need 68)",
                 start_pos, remaining);
        return ESP_ERR_INVALID_SIZE;
    }

    // Decode from the found start position
    opentherm_decode_result_t result;
    decode_manchester_from_position(half_bits, hb_count, start_pos, &result);

    // Validate result
    bool valid = result.start_bit_valid &&
                 result.bits_decoded >= 33 &&  // start + 32 data bits minimum
                 check_frame_parity(result.frame);

    if (valid) {
        *frame = result.frame;
        if (start_pos > 0) {
            ESP_LOGD(TAG, "Decode OK: skipped %d half-bits to find start", start_pos);
        }
        return ESP_OK;
    }

    // Decode failed - log details
    ESP_LOGI(TAG, "Decode failed: start_pos=%d, bits=%d, start=%d, stop=%d, err=%d, parity=%s",
             start_pos, result.bits_decoded, result.start_bit_valid, result.stop_bit_valid,
             result.errors, check_frame_parity(result.frame) ? "OK" : "BAD");

    return ESP_ERR_INVALID_RESPONSE;
}
