/*
 * OpenTherm Manchester Encoder for ESP-IDF using RMT Peripheral
 *
 * This module provides Manchester encoding for OpenTherm frames using the ESP32's
 * RMT (Remote Control Transceiver) peripheral for precise timing.
 *
 * Benefits over GPIO interrupt-based approach:
 * - Hardware-timed pulse generation (no CPU jitter during transmission)
 * - Built-in noise filtering
 */

#ifndef OPENTHERM_RMT_H
#define OPENTHERM_RMT_H

#include <stdint.h>
#include <stdbool.h>
#include "driver/gpio.h"
#include "driver/rmt_tx.h"
#include "driver/rmt_rx.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

#ifdef __cplusplus
extern "C" {
#endif

// RMT interface for one direction (TX + RX on a single OpenTherm bus)
// Used by ot_thermostat and ot_boiler threads
typedef struct {
    gpio_num_t rx_gpio;
    gpio_num_t tx_gpio;
    rmt_channel_handle_t rx_channel;
    rmt_channel_handle_t tx_channel;
    rmt_encoder_handle_t encoder;
    QueueHandle_t rx_queue;           // Queue for received frames
    rmt_symbol_word_t *rx_buffer;     // Buffer for RX symbols
    size_t rx_buffer_size;
    volatile bool rx_pending;         // True if rmt_receive() is active
    size_t rx_symbol_count;           // Symbol count from last receive
} OpenThermRmtInterface;

/**
 * Create a Manchester encoder for OpenTherm frames
 *
 * @param ret_encoder Output: encoder handle
 * @return ESP_OK on success
 */
esp_err_t opentherm_encoder_create(rmt_encoder_handle_t *ret_encoder);

// ============================================================================
// Manchester Decoder
// ============================================================================

/**
 * Result of Manchester decoding attempt
 */
typedef struct {
    uint32_t frame;        // 32-bit decoded data
    int errors;            // Invalid Manchester transitions encountered
    bool start_bit_valid;  // Was start bit = 1?
    bool stop_bit_valid;   // Was stop bit = 1? (34th bit)
    int bits_decoded;      // Total bits successfully decoded (including start)
} opentherm_decode_result_t;

/**
 * Decode Manchester-encoded half-bits into an OpenTherm frame.
 *
 * This function tries both phase alignments (offset 0 and 1) and validates
 * using start bit, stop bit, and parity to select the correct result.
 *
 * @param half_bits  Array of half-bit values (0 or 1, one per 500µs)
 * @param hb_count   Number of elements in half_bits array
 * @param frame      Output: decoded 32-bit frame
 * @return ESP_OK on success
 *         ESP_ERR_INVALID_SIZE if not enough half-bits for complete frame
 *         ESP_ERR_INVALID_RESPONSE if decode failed (parity, start/stop bit)
 */
esp_err_t opentherm_decode_frame(const uint8_t *half_bits, int hb_count, uint32_t *frame);

#ifdef __cplusplus
}
#endif

#endif // OPENTHERM_RMT_H
