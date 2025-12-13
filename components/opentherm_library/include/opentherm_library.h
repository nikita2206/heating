/*
 * OpenTherm Library for ESP-IDF
 * Adapted from: https://github.com/ihormelnyk/opentherm_library
 * Original Copyright 2023, Ihor Melnyk
 * ESP-IDF port Copyright 2024
 * Licensed under MIT license
 *
 * ISR-based software implementation using GPIO interrupts and timers
 * No RMT hardware required - works reliably on all ESP32 variants
 */

#ifndef OPENTHERM_LIBRARY_H
#define OPENTHERM_LIBRARY_H

#include <stdint.h>
#include <stdbool.h>
#include "driver/gpio.h"

#ifdef __cplusplus
extern "C" {
#endif

// OpenTherm timing constants (microseconds)
#define OT_BIT_TIME_US 1000
#define OT_HALF_BIT_TIME_US 500

// Response status
typedef enum {
    OT_RESPONSE_NONE,
    OT_RESPONSE_SUCCESS,
    OT_RESPONSE_INVALID,
    OT_RESPONSE_TIMEOUT
} ot_response_status_t;

// Message type
typedef enum {
    OTLIB_MSG_TYPE_READ_DATA    = 0b000,
    OTLIB_MSG_TYPE_WRITE_DATA   = 0b001,
    OTLIB_MSG_TYPE_INVALID_DATA = 0b010,
    OTLIB_MSG_TYPE_RESERVED     = 0b011,
    OTLIB_MSG_TYPE_READ_ACK     = 0b100,
    OTLIB_MSG_TYPE_WRITE_ACK    = 0b101,
    OTLIB_MSG_TYPE_DATA_INVALID = 0b110,
    OTLIB_MSG_TYPE_UNKNOWN_ID   = 0b111
} otlib_message_type_t;

// Internal state machine
typedef enum {
    OT_STATE_NOT_INITIALIZED,
    OT_STATE_READY,
    OT_STATE_REQUEST_SENDING,
    OT_STATE_RESPONSE_WAITING,
    OT_STATE_RESPONSE_START_BIT,
    OT_STATE_RESPONSE_RECEIVING,
    OT_STATE_RESPONSE_READY,
    OT_STATE_RESPONSE_INVALID
} ot_state_t;

// OpenTherm instance handle
typedef struct opentherm_s opentherm_t;

// Callback for response processing
typedef void (*ot_response_callback_t)(unsigned long response, ot_response_status_t status, void *user_data);

/**
 * Initialize OpenTherm instance
 *
 * @param in_pin GPIO pin for receiving (connected to thermostat/boiler RX)
 * @param out_pin GPIO pin for transmitting (connected to thermostat/boiler TX)
 * @param is_slave True if acting as boiler (slave), false if acting as thermostat (master)
 * @return OpenTherm instance handle, or NULL on error
 */
opentherm_t* opentherm_init(gpio_num_t in_pin, gpio_num_t out_pin, bool is_slave);

/**
 * Start OpenTherm communication
 * Must be called after opentherm_init() and before any send/receive operations
 *
 * @param ot OpenTherm instance
 * @return ESP_OK on success
 */
esp_err_t opentherm_begin(opentherm_t *ot);

/**
 * Set callback for async response processing
 *
 * @param ot OpenTherm instance
 * @param callback Function to call when response is received
 * @param user_data User data passed to callback
 */
void opentherm_set_response_callback(opentherm_t *ot, ot_response_callback_t callback, void *user_data);

/**
 * Check if ready to send request
 *
 * @param ot OpenTherm instance
 * @return true if ready
 */
bool opentherm_is_ready(opentherm_t *ot);

/**
 * Send synchronous request and wait for response (blocking)
 * Blocks for up to ~1 second waiting for response
 *
 * @param ot OpenTherm instance
 * @param request 32-bit OpenTherm request frame
 * @return Response frame, or 0 on timeout/error
 */
unsigned long opentherm_send_request(opentherm_t *ot, unsigned long request);

/**
 * Send asynchronous request (non-blocking)
 * Response will be delivered via callback set with opentherm_set_response_callback()
 *
 * @param ot OpenTherm instance
 * @param request 32-bit OpenTherm request frame
 * @return true if request started successfully
 */
bool opentherm_send_request_async(opentherm_t *ot, unsigned long request);

/**
 * Send response (slave/boiler mode only)
 *
 * @param ot OpenTherm instance
 * @param response 32-bit OpenTherm response frame
 * @return true on success
 */
bool opentherm_send_response(opentherm_t *ot, unsigned long response);

/**
 * Get last received response
 *
 * @param ot OpenTherm instance
 * @return Last response frame
 */
unsigned long opentherm_get_last_response(opentherm_t *ot);

/**
 * Get last response status
 *
 * @param ot OpenTherm instance
 * @return Response status
 */
ot_response_status_t opentherm_get_last_response_status(opentherm_t *ot);

/**
 * Process state machine - must be called regularly from main loop
 * Handles timeouts and async callbacks
 *
 * @param ot OpenTherm instance
 */
void opentherm_process(opentherm_t *ot);

/**
 * Stop OpenTherm communication and cleanup
 *
 * @param ot OpenTherm instance
 */
void opentherm_end(opentherm_t *ot);

/**
 * Free OpenTherm instance
 *
 * @param ot OpenTherm instance to free
 */
void opentherm_free(opentherm_t *ot);

// Helper functions for building and parsing frames

/**
 * Build request frame
 *
 * @param msg_type Message type
 * @param data_id Data ID (0-255)
 * @param data Data value (16 bits)
 * @return 32-bit request frame with parity
 */
unsigned long opentherm_build_request(otlib_message_type_t msg_type, uint8_t data_id, uint16_t data);

/**
 * Build response frame
 *
 * @param msg_type Message type
 * @param data_id Data ID (0-255)
 * @param data Data value (16 bits)
 * @return 32-bit response frame with parity
 */
unsigned long opentherm_build_response(otlib_message_type_t msg_type, uint8_t data_id, uint16_t data);

/**
 * Check frame parity
 *
 * @param frame 32-bit frame
 * @return true if parity is valid
 */
bool opentherm_parity(unsigned long frame);

/**
 * Get message type from frame
 *
 * @param frame 32-bit frame
 * @return Message type
 */
otlib_message_type_t opentherm_get_message_type(unsigned long frame);

/**
 * Get data ID from frame
 *
 * @param frame 32-bit frame
 * @return Data ID (0-255)
 */
uint8_t opentherm_get_data_id(unsigned long frame);

/**
 * Get data value from frame as uint16
 *
 * @param frame 32-bit frame
 * @return Data value (16 bits)
 */
uint16_t opentherm_get_uint16(unsigned long frame);

/**
 * Get data value from frame as float (f8.8 format)
 *
 * @param frame 32-bit frame
 * @return Float value
 */
float opentherm_get_float(unsigned long frame);

/**
 * Check if valid request frame
 *
 * @param frame 32-bit frame
 * @return true if valid
 */
bool opentherm_is_valid_request(unsigned long frame);

/**
 * Check if valid response frame
 *
 * @param frame 32-bit frame
 * @return true if valid
 */
bool opentherm_is_valid_response(unsigned long frame);

#ifdef __cplusplus
}
#endif

#endif // OPENTHERM_LIBRARY_H
