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

#ifdef __cplusplus
}
#endif

#endif // OPENTHERM_RMT_H
