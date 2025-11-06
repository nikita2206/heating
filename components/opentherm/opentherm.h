/*
 * OpenTherm Library for ESP-IDF
 * Adapted from https://github.com/ihormelnyk/opentherm_library
 * 
 * This library provides implementation of OpenTherm protocol for ESP32
 */

#ifndef OPENTHERM_H
#define OPENTHERM_H

#include <stdint.h>
#include <stdbool.h>
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#ifdef __cplusplus
extern "C" {
#endif

// OpenTherm message structure
typedef enum {
    OT_MSGTYPE_READ_DATA = 0,
    OT_MSGTYPE_WRITE_DATA = 1,
    OT_MSGTYPE_INVALID_DATA = 2,
    OT_MSGTYPE_RESERVED = 3,
    OT_MSGTYPE_READ_ACK = 4,
    OT_MSGTYPE_WRITE_ACK = 5,
    OT_MSGTYPE_DATA_INVALID = 6,
    OT_MSGTYPE_UNKNOWN_DATAID = 7
} OpenThermMessageType;

typedef enum {
    OT_STATUS_NONE = 0,
    OT_STATUS_READY = 1,
    OT_STATUS_REQUEST_SENDING = 2,
    OT_STATUS_RESPONSE_WAITING = 3,
    OT_STATUS_RESPONSE_READY = 4,
    OT_STATUS_RESPONSE_INVALID = 5,
    OT_STATUS_TIMEOUT = 6,
    OT_STATUS_GATEWAY_REQUEST_WAITING = 7,
    OT_STATUS_GATEWAY_REQUEST_READY = 8,
    OT_STATUS_GATEWAY_RESPONSE_WAITING = 9,
    OT_STATUS_GATEWAY_RESPONSE_READY = 10
} OpenThermStatus;

typedef enum {
    OT_ROLE_MASTER = 0,  // Thermostat side
    OT_ROLE_SLAVE = 1    // Boiler side
} OpenThermRole;

typedef enum {
    OT_GATEWAY_STATE_IDLE = 0,
    OT_GATEWAY_STATE_WAITING_REQUEST,
    OT_GATEWAY_STATE_WAITING_RESPONSE
} OpenThermGatewayState;

// OpenTherm message (32-bit)
typedef struct {
    uint32_t data;
} OpenThermMessage;

typedef struct OpenTherm OpenTherm;

typedef struct {
    OpenTherm *ot;
    gpio_num_t pin;
    OpenThermRole role;
} opentherm_isr_context_t;

// Callback for handling received messages (used in gateway mode)
typedef void (*opentherm_message_callback_t)(OpenTherm *ot, OpenThermMessage *message, OpenThermRole from_role);

// OpenTherm instance structure
struct OpenTherm {
    gpio_num_t in_pin;
    gpio_num_t out_pin;
    OpenThermRole role;
    
    volatile OpenThermStatus status;
    volatile uint32_t response;
    volatile unsigned long response_timestamp;
    volatile uint8_t response_bit_index;
    
    // For gateway mode - second interface
    gpio_num_t in_pin_secondary;
    gpio_num_t out_pin_secondary;
    bool gateway_mode;
    gpio_num_t current_receive_pin;
    OpenThermRole current_receive_role;
    OpenThermGatewayState gateway_state;
    unsigned long gateway_timer_start;
    OpenThermMessage gateway_request;
    OpenThermMessage gateway_response;
    bool gateway_timeout_flag;
    
    // Message callback for logging/processing
    opentherm_message_callback_t message_callback;
    void *user_data;
    
    // Timing
    int bit_read_state;
    unsigned long response_start_time;

    // ISR contexts
    opentherm_isr_context_t isr_primary_ctx;
    opentherm_isr_context_t isr_secondary_ctx;
    
    // Debug counters - track ISR activity for hardware diagnostics
    volatile uint32_t isr_edge_count_master;  // Count edges on master interface
    volatile uint32_t isr_edge_count_slave;   // Count edges on slave interface
};

// Initialization
void opentherm_init(OpenTherm *ot, gpio_num_t in_pin, gpio_num_t out_pin, OpenThermRole role);
void opentherm_init_gateway(OpenTherm *ot, 
                            gpio_num_t in_pin_master, gpio_num_t out_pin_master,
                            gpio_num_t in_pin_slave, gpio_num_t out_pin_slave);
void opentherm_set_message_callback(OpenTherm *ot, opentherm_message_callback_t callback, void *user_data);
void opentherm_start(OpenTherm *ot);
void opentherm_gateway_reset(OpenTherm *ot);
bool opentherm_gateway_process(OpenTherm *ot, OpenThermMessage *request, OpenThermMessage *response);

// Message handling
bool opentherm_send_request(OpenTherm *ot, OpenThermMessage *request);
bool opentherm_send_response(OpenTherm *ot, OpenThermMessage *response);
bool opentherm_get_last_response(OpenTherm *ot, OpenThermMessage *response);
OpenThermStatus opentherm_get_status(OpenTherm *ot);

// Message construction/parsing
uint32_t opentherm_build_request(OpenThermMessageType type, uint8_t id, uint16_t data);
uint32_t opentherm_build_response(OpenThermMessageType type, uint8_t id, uint16_t data);
OpenThermMessageType opentherm_get_message_type(uint32_t message);
uint8_t opentherm_get_data_id(uint32_t message);
uint16_t opentherm_get_uint16(uint32_t message);
float opentherm_get_float(uint32_t message);
uint8_t opentherm_get_uint8(uint32_t message);
int8_t opentherm_get_int8(uint32_t message);

// Utility functions
bool opentherm_is_valid_response(uint32_t request, uint32_t response);
const char* opentherm_message_type_to_string(OpenThermMessageType type);
const char* opentherm_status_to_string(OpenThermStatus status);

// Data ID constants (commonly used)
#define OT_MSGID_STATUS 0
#define OT_MSGID_TSET 1
#define OT_MSGID_MASTER_CONFIG 2
#define OT_MSGID_SLAVE_CONFIG 3
#define OT_MSGID_COMMAND 4
#define OT_MSGID_FAULT_FLAGS 5
#define OT_MSGID_REMOTE 6
#define OT_MSGID_COOLING_CONTROL 7
#define OT_MSGID_TSETCH2 8
#define OT_MSGID_TROOM_OVERRIDE 9
#define OT_MSGID_TSP 10
#define OT_MSGID_TSP_BOUNDS 11
#define OT_MSGID_MAX_REL_MOD_LEVEL_SETTING 14
#define OT_MSGID_MAX_CAPACITY_MIN_MOD_LEVEL 15
#define OT_MSGID_TROOM_SETPOINT 16
#define OT_MSGID_REL_MOD_LEVEL 17
#define OT_MSGID_CH_PRESSURE 18
#define OT_MSGID_DHW_FLOW_RATE 19
#define OT_MSGID_TROOM 24
#define OT_MSGID_TBOILER 25
#define OT_MSGID_TDHW 26
#define OT_MSGID_TOUTSIDE 27
#define OT_MSGID_TRET 28
#define OT_MSGID_TSTORAGE 29
#define OT_MSGID_TCOLLECTOR 30
#define OT_MSGID_TFLOWCH2 31
#define OT_MSGID_TDHW2 32
#define OT_MSGID_TEXHAUST 33

#ifdef __cplusplus
}
#endif

#endif // OPENTHERM_H

