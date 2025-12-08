/*
 * OpenTherm Library for ESP-IDF using RMT Peripheral
 * 
 * This library provides an OpenTherm implementation using the ESP32's RMT
 * (Remote Control Transceiver) peripheral for precise Manchester encoding/decoding.
 * 
 * Benefits over GPIO interrupt-based approach:
 * - Hardware-timed pulse generation (no CPU jitter during transmission)
 * - Automatic edge timestamping (no ISR latency affecting timing measurements)
 * - Built-in noise filtering
 * - DMA support for larger buffers if needed
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

// OpenTherm message types (same as original)
typedef enum {
    OT_RMT_MSGTYPE_READ_DATA = 0,
    OT_RMT_MSGTYPE_WRITE_DATA = 1,
    OT_RMT_MSGTYPE_INVALID_DATA = 2,
    OT_RMT_MSGTYPE_RESERVED = 3,
    OT_RMT_MSGTYPE_READ_ACK = 4,
    OT_RMT_MSGTYPE_WRITE_ACK = 5,
    OT_RMT_MSGTYPE_DATA_INVALID = 6,
    OT_RMT_MSGTYPE_UNKNOWN_DATAID = 7
} OpenThermRmtMessageType;

typedef enum {
    OT_RMT_STATUS_IDLE = 0,
    OT_RMT_STATUS_TRANSMITTING,
    OT_RMT_STATUS_WAITING_RESPONSE,
    OT_RMT_STATUS_RECEIVING,
    OT_RMT_STATUS_FRAME_READY,
    OT_RMT_STATUS_TIMEOUT,
    OT_RMT_STATUS_ERROR
} OpenThermRmtStatus;

typedef enum {
    OT_RMT_ROLE_MASTER = 0,  // Thermostat side - sends requests, receives responses
    OT_RMT_ROLE_SLAVE = 1    // Boiler side - receives requests, sends responses
} OpenThermRmtRole;

typedef enum {
    OT_RMT_GATEWAY_STATE_IDLE = 0,
    OT_RMT_GATEWAY_STATE_WAITING_REQUEST,
    OT_RMT_GATEWAY_STATE_FORWARDING_REQUEST,
    OT_RMT_GATEWAY_STATE_WAITING_RESPONSE,
    OT_RMT_GATEWAY_STATE_FORWARDING_RESPONSE
} OpenThermRmtGatewayState;

// OpenTherm message (32-bit frame)
typedef struct {
    uint32_t data;
} OpenThermRmtMessage;

// Forward declaration
typedef struct OpenThermRmt OpenThermRmt;

// Callback for handling received messages (used in gateway mode)
typedef void (*opentherm_rmt_message_callback_t)(OpenThermRmt *ot, OpenThermRmtMessage *message, OpenThermRmtRole from_role);

// Callback for intercepting requests before forwarding (returns true to block forwarding)
typedef bool (*opentherm_rmt_request_interceptor_t)(OpenThermRmt *ot, OpenThermRmtMessage *request);

// RMT interface for one direction (TX + RX on a single OpenTherm bus)
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

// Main OpenTherm RMT instance structure
struct OpenThermRmt {
    // Primary interface (for single-role or master side of gateway)
    OpenThermRmtInterface primary;
    
    // Secondary interface (for slave side of gateway)
    OpenThermRmtInterface secondary;
    
    // Configuration
    OpenThermRmtRole role;
    bool gateway_mode;
    
    // State
    volatile OpenThermRmtStatus status;
    volatile OpenThermRmtGatewayState gateway_state;
    volatile uint32_t last_frame;
    volatile bool frame_ready;
    
    // Gateway state
    OpenThermRmtMessage gateway_request;
    OpenThermRmtMessage gateway_response;
    volatile bool gateway_timeout_flag;
    int64_t gateway_timer_start;
    
    // Callback for message logging
    opentherm_rmt_message_callback_t message_callback;
    void *user_data;
    
    // Callback for request interception (returns true to block forwarding)
    opentherm_rmt_request_interceptor_t request_interceptor;
    void *interceptor_data;
    
    // Synchronization
    SemaphoreHandle_t tx_done_sem;
    SemaphoreHandle_t rx_done_sem;
    
    // Debug/diagnostics
    volatile uint32_t tx_count;
    volatile uint32_t rx_count;
    volatile uint32_t error_count;
    volatile uint32_t timeout_count;
};

// ============================================================================
// Initialization Functions
// ============================================================================

/**
 * Initialize OpenTherm RMT in single-role mode (master or slave)
 * 
 * @param ot      Pointer to OpenThermRmt instance
 * @param rx_pin  GPIO for receiving (input from OpenTherm bus)
 * @param tx_pin  GPIO for transmitting (output to OpenTherm bus)
 * @param role    OT_RMT_ROLE_MASTER or OT_RMT_ROLE_SLAVE
 * @return ESP_OK on success
 */
esp_err_t opentherm_rmt_init(OpenThermRmt *ot, gpio_num_t rx_pin, gpio_num_t tx_pin, OpenThermRmtRole role);

/**
 * Initialize OpenTherm RMT in gateway mode (MITM proxy)
 * 
 * @param ot              Pointer to OpenThermRmt instance
 * @param master_rx_pin   GPIO to receive from thermostat
 * @param master_tx_pin   GPIO to transmit to thermostat  
 * @param slave_rx_pin    GPIO to receive from boiler
 * @param slave_tx_pin    GPIO to transmit to boiler
 * @return ESP_OK on success
 */
esp_err_t opentherm_rmt_init_gateway(OpenThermRmt *ot,
                                      gpio_num_t master_rx_pin, gpio_num_t master_tx_pin,
                                      gpio_num_t slave_rx_pin, gpio_num_t slave_tx_pin);

/**
 * Start OpenTherm communication (enables RX channels)
 */
esp_err_t opentherm_rmt_start(OpenThermRmt *ot);

/**
 * Stop OpenTherm communication
 */
esp_err_t opentherm_rmt_stop(OpenThermRmt *ot);

/**
 * Deinitialize and free resources
 */
esp_err_t opentherm_rmt_deinit(OpenThermRmt *ot);

// ============================================================================
// Message Handling
// ============================================================================

/**
 * Set callback for received messages (useful for logging)
 */
void opentherm_rmt_set_message_callback(OpenThermRmt *ot, opentherm_rmt_message_callback_t callback, void *user_data);

/**
 * Set callback for request interception (returns true to block forwarding)
 */
void opentherm_rmt_set_request_interceptor(OpenThermRmt *ot, opentherm_rmt_request_interceptor_t interceptor, void *user_data);

/**
 * Send a request (master role) and wait for response
 * 
 * @param ot        OpenTherm instance
 * @param request   Request message to send
 * @param response  Buffer to receive response (can be NULL)
 * @param timeout_ms Timeout in milliseconds
 * @return ESP_OK on success, ESP_ERR_TIMEOUT on timeout
 */
esp_err_t opentherm_rmt_send_request(OpenThermRmt *ot, OpenThermRmtMessage *request, 
                                      OpenThermRmtMessage *response, uint32_t timeout_ms);

/**
 * Send a response (slave role)
 */
esp_err_t opentherm_rmt_send_response(OpenThermRmt *ot, OpenThermRmtMessage *response);

/**
 * Send a raw frame on specified interface
 */
esp_err_t opentherm_rmt_send_frame(OpenThermRmt *ot, uint32_t frame, OpenThermRmtInterface *iface);

/**
 * Receive a frame from specified interface (for advanced use cases like diagnostic injection)
 * 
 * @param ot OpenTherm instance
 * @param iface Interface to receive from
 * @param frame Buffer to receive frame
 * @param timeout_ms Timeout in milliseconds
 * @return ESP_OK on success, ESP_ERR_TIMEOUT on timeout
 */
esp_err_t opentherm_rmt_receive_frame(OpenThermRmt *ot, OpenThermRmtInterface *iface, 
                                      uint32_t *frame, uint32_t timeout_ms);

// ============================================================================
// Gateway Functions
// ============================================================================

/**
 * Reset gateway to idle state
 */
void opentherm_rmt_gateway_reset(OpenThermRmt *ot);

/**
 * Process gateway state machine (call from main loop)
 * 
 * @param ot       OpenTherm instance
 * @param request  Buffer to receive captured request (can be NULL)
 * @param response Buffer to receive captured response (can be NULL)
 * @return true if a complete request/response transaction was proxied
 */
bool opentherm_rmt_gateway_process(OpenThermRmt *ot, OpenThermRmtMessage *request, OpenThermRmtMessage *response);

// ============================================================================
// Message Construction/Parsing
// ============================================================================

uint32_t opentherm_rmt_build_request(OpenThermRmtMessageType type, uint8_t id, uint16_t data);
uint32_t opentherm_rmt_build_response(OpenThermRmtMessageType type, uint8_t id, uint16_t data);

OpenThermRmtMessageType opentherm_rmt_get_message_type(uint32_t message);
uint8_t opentherm_rmt_get_data_id(uint32_t message);
uint16_t opentherm_rmt_get_uint16(uint32_t message);
float opentherm_rmt_get_float(uint32_t message);
uint8_t opentherm_rmt_get_uint8_hb(uint32_t message);  // High byte
uint8_t opentherm_rmt_get_uint8_lb(uint32_t message);  // Low byte
int8_t opentherm_rmt_get_int8(uint32_t message);

// ============================================================================
// Validation
// ============================================================================

bool opentherm_rmt_check_parity(uint32_t frame);
bool opentherm_rmt_is_valid_request_type(uint32_t frame);
bool opentherm_rmt_is_valid_response_type(uint32_t frame);
bool opentherm_rmt_is_valid_response(uint32_t request, uint32_t response);

// ============================================================================
// Utility/Debug
// ============================================================================

const char* opentherm_rmt_message_type_to_string(OpenThermRmtMessageType type);
const char* opentherm_rmt_status_to_string(OpenThermRmtStatus status);
OpenThermRmtStatus opentherm_rmt_get_status(OpenThermRmt *ot);

// ============================================================================
// Data ID Constants (commonly used)
// ============================================================================

#define OT_RMT_MSGID_STATUS 0
#define OT_RMT_MSGID_TSET 1
#define OT_RMT_MSGID_MASTER_CONFIG 2
#define OT_RMT_MSGID_SLAVE_CONFIG 3
#define OT_RMT_MSGID_COMMAND 4
#define OT_RMT_MSGID_FAULT_FLAGS 5
#define OT_RMT_MSGID_REMOTE 6
#define OT_RMT_MSGID_COOLING_CONTROL 7
#define OT_RMT_MSGID_TSETCH2 8
#define OT_RMT_MSGID_TROOM_OVERRIDE 9
#define OT_RMT_MSGID_TSP 10
#define OT_RMT_MSGID_TSP_BOUNDS 11
#define OT_RMT_MSGID_MAX_REL_MOD_LEVEL_SETTING 14
#define OT_RMT_MSGID_MAX_CAPACITY_MIN_MOD_LEVEL 15
#define OT_RMT_MSGID_TROOM_SETPOINT 16
#define OT_RMT_MSGID_REL_MOD_LEVEL 17
#define OT_RMT_MSGID_CH_PRESSURE 18
#define OT_RMT_MSGID_DHW_FLOW_RATE 19
#define OT_RMT_MSGID_TROOM 24
#define OT_RMT_MSGID_TBOILER 25
#define OT_RMT_MSGID_TDHW 26
#define OT_RMT_MSGID_TOUTSIDE 27
#define OT_RMT_MSGID_TRET 28
#define OT_RMT_MSGID_TSTORAGE 29
#define OT_RMT_MSGID_TCOLLECTOR 30
#define OT_RMT_MSGID_TFLOWCH2 31
#define OT_RMT_MSGID_TDHW2 32
#define OT_RMT_MSGID_TEXHAUST 33

#ifdef __cplusplus
}
#endif

#endif // OPENTHERM_RMT_H

