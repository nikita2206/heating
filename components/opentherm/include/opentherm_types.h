/*
 * OpenTherm Common Types
 * 
 * Shared types used by all OpenTherm implementations.
 * This is part of the abstraction layer that sits above specific backends.
 */

#ifndef OPENTHERM_TYPES_H
#define OPENTHERM_TYPES_H

#include <stdint.h>
#include <stdbool.h>
#include "driver/gpio.h"

#ifdef __cplusplus
extern "C" {
#endif

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
} ot_message_type_t;

// OpenTherm roles
typedef enum {
    OT_ROLE_MASTER = 0,  // Thermostat side - sends requests, receives responses
    OT_ROLE_SLAVE = 1    // Boiler side - receives requests, sends responses
} ot_role_t;

// OpenTherm message (32-bit frame)
typedef struct {
    uint32_t data;
} ot_message_t;

// Pin configuration for gateway mode
typedef struct {
    gpio_num_t thermostat_in_pin;   // Receive from thermostat
    gpio_num_t thermostat_out_pin;  // Transmit to thermostat
    gpio_num_t boiler_in_pin;       // Receive from boiler
    gpio_num_t boiler_out_pin;      // Transmit to boiler
} ot_pin_config_t;

// Statistics structure
typedef struct {
    uint32_t tx_count;
    uint32_t rx_count;
    uint32_t error_count;
    uint32_t timeout_count;
} ot_stats_t;

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

#ifdef __cplusplus
}
#endif

#endif // OPENTHERM_TYPES_H

