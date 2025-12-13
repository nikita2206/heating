/*
 * OpenTherm Utility Functions
 *
 * Message parsing and construction utilities shared by all components.
 */

#include "ot_queues.h"

// ============================================================================
// Message Parsing
// ============================================================================

ot_msg_type_t ot_get_msg_type(uint32_t frame)
{
    return (ot_msg_type_t)((frame >> 28) & 0x7);
}

uint8_t ot_get_data_id(uint32_t frame)
{
    return (uint8_t)((frame >> 16) & 0xFF);
}

uint16_t ot_get_data_value(uint32_t frame)
{
    return (uint16_t)(frame & 0xFFFF);
}

float ot_get_float(uint32_t frame)
{
    return (float)((int16_t)ot_get_data_value(frame)) / 256.0f;
}

uint8_t ot_get_hb(uint32_t frame)
{
    return (uint8_t)((frame >> 8) & 0xFF);
}

uint8_t ot_get_lb(uint32_t frame)
{
    return (uint8_t)(frame & 0xFF);
}

// ============================================================================
// Message Construction
// ============================================================================

uint32_t ot_build_request(ot_msg_type_t type, uint8_t id, uint16_t data)
{
    uint32_t frame = ((uint32_t)type << 28) | ((uint32_t)id << 16) | data;

    // Calculate parity bit (even parity)
    uint8_t parity = 0;
    uint32_t temp = frame;
    while (temp) {
        parity ^= (temp & 1);
        temp >>= 1;
    }
    frame |= ((uint32_t)parity << 31);

    return frame;
}

uint32_t ot_build_response(ot_msg_type_t type, uint8_t id, uint16_t data)
{
    return ot_build_request(type, id, data);
}

// ============================================================================
// Validation
// ============================================================================

bool ot_check_parity(uint32_t frame)
{
    uint32_t count = 0;
    uint32_t n = frame;
    while (n) {
        n &= (n - 1);
        count++;
    }
    return (count % 2) == 0;
}

// ============================================================================
// String Conversion
// ============================================================================

const char* ot_msg_type_str(ot_msg_type_t type)
{
    switch (type) {
        case OT_MSGTYPE_READ_DATA:     return "READ_DATA";
        case OT_MSGTYPE_WRITE_DATA:    return "WRITE_DATA";
        case OT_MSGTYPE_INVALID_DATA:  return "INVALID_DATA";
        case OT_MSGTYPE_RESERVED:      return "RESERVED";
        case OT_MSGTYPE_READ_ACK:      return "READ_ACK";
        case OT_MSGTYPE_WRITE_ACK:     return "WRITE_ACK";
        case OT_MSGTYPE_DATA_INVALID:  return "DATA_INVALID";
        case OT_MSGTYPE_UNKNOWN_DATAID: return "UNKNOWN_DATAID";
        default:                        return "UNKNOWN";
    }
}
