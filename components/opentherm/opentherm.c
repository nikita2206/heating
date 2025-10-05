/*
 * OpenTherm Library for ESP-IDF
 * Adapted from https://github.com/ihormelnyk/opentherm_library
 */

#include "opentherm.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "OpenTherm";

// OpenTherm timing constants (in microseconds)
#define OT_TIMING_BIT_PERIOD 1000
#define OT_TIMING_BIT_HALF_PERIOD 500
#define OT_TIMING_RESPONSE_TIMEOUT 800000  // 800ms

// Get current time in microseconds
static inline unsigned long IRAM_ATTR micros(void)
{
    return (unsigned long)esp_timer_get_time();
}

// GPIO interrupt handler
static void IRAM_ATTR opentherm_isr_handler(void *arg)
{
    OpenTherm *ot = (OpenTherm *)arg;
    
    if (ot->status == OT_STATUS_RESPONSE_WAITING) {
        unsigned long current_time = micros();
        
        if (ot->response_bit_index == 0) {
            // Start bit
            ot->response_start_time = current_time;
            ot->response_bit_index = 1;
            ot->bit_read_state = 0;
        } else {
            unsigned long elapsed = current_time - ot->response_start_time;
            uint8_t bit_index = (uint8_t)((elapsed - OT_TIMING_BIT_HALF_PERIOD) / OT_TIMING_BIT_PERIOD);
            
            if (bit_index < 32) {
                if (ot->bit_read_state == 0) {
                    // First edge of Manchester encoding
                    ot->bit_read_state = 1;
                } else {
                    // Second edge - read bit value
                    bool bit_value = (elapsed - (bit_index * OT_TIMING_BIT_PERIOD + OT_TIMING_BIT_HALF_PERIOD)) < OT_TIMING_BIT_HALF_PERIOD;
                    ot->response = (ot->response << 1) | (bit_value ? 1 : 0);
                    ot->bit_read_state = 0;
                    ot->response_bit_index++;
                }
            } else if (bit_index >= 32) {
                // Stop bit or complete
                ot->status = OT_STATUS_RESPONSE_READY;
                ot->response_timestamp = current_time;
            }
        }
    }
}

// Send bit using Manchester encoding
static void opentherm_send_bit(OpenTherm *ot, bool high, gpio_num_t pin)
{
    if (high) {
        // Manchester high: low then high
        gpio_set_level(pin, 1);
        esp_rom_delay_us(OT_TIMING_BIT_HALF_PERIOD);
        gpio_set_level(pin, 0);
        esp_rom_delay_us(OT_TIMING_BIT_HALF_PERIOD);
    } else {
        // Manchester low: high then low
        gpio_set_level(pin, 0);
        esp_rom_delay_us(OT_TIMING_BIT_HALF_PERIOD);
        gpio_set_level(pin, 1);
        esp_rom_delay_us(OT_TIMING_BIT_HALF_PERIOD);
    }
}

// Initialize OpenTherm instance
void opentherm_init(OpenTherm *ot, gpio_num_t in_pin, gpio_num_t out_pin, OpenThermRole role)
{
    memset(ot, 0, sizeof(OpenTherm));
    
    ot->in_pin = in_pin;
    ot->out_pin = out_pin;
    ot->role = role;
    ot->status = OT_STATUS_NONE;
    ot->gateway_mode = false;
    
    // Configure input pin
    gpio_config_t in_conf = {
        .pin_bit_mask = (1ULL << in_pin),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_ANYEDGE
    };
    gpio_config(&in_conf);
    
    // Configure output pin
    gpio_config_t out_conf = {
        .pin_bit_mask = (1ULL << out_pin),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    gpio_config(&out_conf);
    gpio_set_level(out_pin, 1);  // Idle state is high
    
    ESP_LOGI(TAG, "OpenTherm initialized (role: %s, in: %d, out: %d)", 
             role == OT_ROLE_MASTER ? "MASTER" : "SLAVE", in_pin, out_pin);
}

// Initialize gateway mode (proxy between master and slave)
void opentherm_init_gateway(OpenTherm *ot,
                            gpio_num_t in_pin_master, gpio_num_t out_pin_master,
                            gpio_num_t in_pin_slave, gpio_num_t out_pin_slave)
{
    // Initialize primary interface (master side - receives from thermostat)
    opentherm_init(ot, in_pin_master, out_pin_master, OT_ROLE_SLAVE);
    
    // Configure secondary interface (slave side - receives from boiler)
    ot->in_pin_secondary = in_pin_slave;
    ot->out_pin_secondary = out_pin_slave;
    ot->gateway_mode = true;
    
    // Configure secondary input pin
    gpio_config_t in_conf = {
        .pin_bit_mask = (1ULL << in_pin_slave),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE  // Manual polling in gateway mode
    };
    gpio_config(&in_conf);
    
    // Configure secondary output pin
    gpio_config_t out_conf = {
        .pin_bit_mask = (1ULL << out_pin_slave),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    gpio_config(&out_conf);
    gpio_set_level(out_pin_slave, 1);  // Idle state is high
    
    ESP_LOGI(TAG, "OpenTherm gateway initialized");
    ESP_LOGI(TAG, "  Master side (thermostat): in=%d, out=%d", in_pin_master, out_pin_master);
    ESP_LOGI(TAG, "  Slave side (boiler): in=%d, out=%d", in_pin_slave, out_pin_slave);
}

// Set message callback
void opentherm_set_message_callback(OpenTherm *ot, opentherm_message_callback_t callback, void *user_data)
{
    ot->message_callback = callback;
    ot->user_data = user_data;
}

// Start OpenTherm communication
void opentherm_start(OpenTherm *ot)
{
    // Install GPIO ISR handler
    gpio_install_isr_service(0);
    gpio_isr_handler_add(ot->in_pin, opentherm_isr_handler, (void *)ot);
    
    ot->status = OT_STATUS_READY;
    ESP_LOGI(TAG, "OpenTherm started");
}

// Send message frame
static void opentherm_send_frame(OpenTherm *ot, uint32_t data, gpio_num_t pin)
{
    // Send start bit
    opentherm_send_bit(ot, 1, pin);
    
    // Send 32 data bits (MSB first)
    for (int i = 31; i >= 0; i--) {
        opentherm_send_bit(ot, (data >> i) & 1, pin);
    }
    
    // Send stop bit
    opentherm_send_bit(ot, 1, pin);
    
    // Return to idle (high)
    gpio_set_level(pin, 1);
}

// Send request (master role)
bool opentherm_send_request(OpenTherm *ot, OpenThermMessage *request)
{
    if (ot->status != OT_STATUS_READY) {
        return false;
    }
    
    ot->status = OT_STATUS_REQUEST_SENDING;
    opentherm_send_frame(ot, request->data, ot->out_pin);
    ot->status = OT_STATUS_RESPONSE_WAITING;
    ot->response = 0;
    ot->response_bit_index = 0;
    
    // Wait for response with timeout
    unsigned long start_time = micros();
    while (ot->status == OT_STATUS_RESPONSE_WAITING) {
        if (micros() - start_time > OT_TIMING_RESPONSE_TIMEOUT) {
            ot->status = OT_STATUS_TIMEOUT;
            ESP_LOGW(TAG, "Response timeout");
            return false;
        }
        vTaskDelay(1);
    }
    
    if (ot->status == OT_STATUS_RESPONSE_READY) {
        ot->status = OT_STATUS_READY;
        return true;
    }
    
    return false;
}

// Send response (slave role)
bool opentherm_send_response(OpenTherm *ot, OpenThermMessage *response)
{
    if (ot->status != OT_STATUS_READY) {
        return false;
    }
    
    opentherm_send_frame(ot, response->data, ot->out_pin);
    return true;
}

// Get last response
bool opentherm_get_last_response(OpenTherm *ot, OpenThermMessage *response)
{
    if (response) {
        response->data = ot->response;
    }
    return true;
}

// Get status
OpenThermStatus opentherm_get_status(OpenTherm *ot)
{
    return ot->status;
}

// Build request message
uint32_t opentherm_build_request(OpenThermMessageType type, uint8_t id, uint16_t data)
{
    uint32_t request = ((uint32_t)type << 28) | ((uint32_t)id << 16) | data;
    
    // Calculate parity bit
    uint8_t parity = 0;
    for (int i = 0; i < 32; i++) {
        if (request & ((uint32_t)1 << i)) {
            parity ^= 1;
        }
    }
    request |= ((uint32_t)parity << 31);
    
    return request;
}

// Build response message
uint32_t opentherm_build_response(OpenThermMessageType type, uint8_t id, uint16_t data)
{
    return opentherm_build_request(type, id, data);
}

// Get message type
OpenThermMessageType opentherm_get_message_type(uint32_t message)
{
    return (OpenThermMessageType)((message >> 28) & 0x7);
}

// Get data ID
uint8_t opentherm_get_data_id(uint32_t message)
{
    return (uint8_t)((message >> 16) & 0xFF);
}

// Get uint16 value
uint16_t opentherm_get_uint16(uint32_t message)
{
    return (uint16_t)(message & 0xFFFF);
}

// Get float value (f8.8 format)
float opentherm_get_float(uint32_t message)
{
    uint16_t data = opentherm_get_uint16(message);
    return (float)((int16_t)data) / 256.0f;
}

// Get uint8 value (HB)
uint8_t opentherm_get_uint8(uint32_t message)
{
    return (uint8_t)((message >> 8) & 0xFF);
}

// Get int8 value (LB)
int8_t opentherm_get_int8(uint32_t message)
{
    return (int8_t)(message & 0xFF);
}

// Validate response
bool opentherm_is_valid_response(uint32_t request, uint32_t response)
{
    uint8_t request_id = opentherm_get_data_id(request);
    uint8_t response_id = opentherm_get_data_id(response);
    
    return request_id == response_id;
}

// Message type to string
const char* opentherm_message_type_to_string(OpenThermMessageType type)
{
    switch (type) {
        case OT_MSGTYPE_READ_DATA: return "READ_DATA";
        case OT_MSGTYPE_WRITE_DATA: return "WRITE_DATA";
        case OT_MSGTYPE_INVALID_DATA: return "INVALID_DATA";
        case OT_MSGTYPE_RESERVED: return "RESERVED";
        case OT_MSGTYPE_READ_ACK: return "READ_ACK";
        case OT_MSGTYPE_WRITE_ACK: return "WRITE_ACK";
        case OT_MSGTYPE_DATA_INVALID: return "DATA_INVALID";
        case OT_MSGTYPE_UNKNOWN_DATAID: return "UNKNOWN_DATAID";
        default: return "UNKNOWN";
    }
}

// Status to string
const char* opentherm_status_to_string(OpenThermStatus status)
{
    switch (status) {
        case OT_STATUS_NONE: return "NONE";
        case OT_STATUS_READY: return "READY";
        case OT_STATUS_REQUEST_SENDING: return "REQUEST_SENDING";
        case OT_STATUS_RESPONSE_WAITING: return "RESPONSE_WAITING";
        case OT_STATUS_RESPONSE_READY: return "RESPONSE_READY";
        case OT_STATUS_RESPONSE_INVALID: return "RESPONSE_INVALID";
        case OT_STATUS_TIMEOUT: return "TIMEOUT";
        default: return "UNKNOWN";
    }
}

