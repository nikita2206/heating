/*
 * OpenTherm Queue Definitions
 *
 * Shared queue handles for inter-thread communication between:
 * - Thermostat thread (ot_thermostat.c)
 * - Boiler thread (ot_boiler.c)
 * - Main loop (boiler_manager.c)
 *
 * All queues have size=1 to ensure we process one message at a time
 * and don't buffer stale data.
 */

#ifndef OT_QUEUES_H
#define OT_QUEUES_H

#include <stdint.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "driver/gpio.h"

#ifdef __cplusplus
extern "C" {
#endif

// OpenTherm message (32-bit frame)
typedef struct {
    uint32_t data;
} ot_msg_t;

// Pin configuration for gateway mode
typedef struct {
    gpio_num_t thermostat_in_pin;   // Receive from thermostat
    gpio_num_t thermostat_out_pin;  // Transmit to thermostat
    gpio_num_t boiler_in_pin;       // Receive from boiler
    gpio_num_t boiler_out_pin;      // Transmit to boiler
} ot_pins_t;

// Queue handles for inter-thread communication
typedef struct ot_queues {
    QueueHandle_t thermostat_request;   // thermostat thread → main loop (size=1)
    QueueHandle_t thermostat_response;  // main loop → thermostat thread (size=1)
    QueueHandle_t boiler_request;       // main loop → boiler thread (size=1)
    QueueHandle_t boiler_response;      // boiler thread → main loop (size=1)
} ot_queues_t;

// Statistics structure
typedef struct {
    uint32_t tx_count;
    uint32_t rx_count;
    uint32_t error_count;
    uint32_t timeout_count;
} ot_stats_t;

// OpenTherm message types
typedef enum {
    OT_MSGTYPE_READ_DATA = 0,
    OT_MSGTYPE_WRITE_DATA = 1,
    OT_MSGTYPE_INVALID_DATA = 2,
    OT_MSGTYPE_RESERVED = 3,
    OT_MSGTYPE_READ_ACK = 4,
    OT_MSGTYPE_WRITE_ACK = 5,
    OT_MSGTYPE_DATA_INVALID = 6,
    OT_MSGTYPE_UNKNOWN_DATAID = 7
} ot_msg_type_t;

// Common Data ID constants
#define OT_MSGID_STATUS 0
#define OT_MSGID_TSET 1
#define OT_MSGID_MASTER_CONFIG 2
#define OT_MSGID_SLAVE_CONFIG 3
#define OT_MSGID_COMMAND 4
#define OT_MSGID_FAULT_FLAGS 5
#define OT_MSGID_REL_MOD_LEVEL 17
#define OT_MSGID_CH_PRESSURE 18
#define OT_MSGID_DHW_FLOW_RATE 19
#define OT_MSGID_TROOM 24
#define OT_MSGID_TBOILER 25
#define OT_MSGID_TDHW 26
#define OT_MSGID_TOUTSIDE 27
#define OT_MSGID_TRET 28

// Message parsing utilities
ot_msg_type_t ot_get_msg_type(uint32_t frame);
uint8_t ot_get_data_id(uint32_t frame);
uint16_t ot_get_data_value(uint32_t frame);
float ot_get_float(uint32_t frame);
uint8_t ot_get_hb(uint32_t frame);  // High byte
uint8_t ot_get_lb(uint32_t frame);  // Low byte

// Message construction utilities
uint32_t ot_build_request(ot_msg_type_t type, uint8_t id, uint16_t data);
uint32_t ot_build_response(ot_msg_type_t type, uint8_t id, uint16_t data);

// Validation utilities
bool ot_check_parity(uint32_t frame);

// String conversion
const char* ot_msg_type_str(ot_msg_type_t type);

// Backward-compatible aliases (used by websocket_server)
typedef ot_msg_type_t ot_message_type_t;
#define ot_get_message_type ot_get_msg_type
#define ot_get_uint16 ot_get_data_value
#define ot_message_type_to_string ot_msg_type_str

#ifdef __cplusplus
}
#endif

#endif // OT_QUEUES_H
