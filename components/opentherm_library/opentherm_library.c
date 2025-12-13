/*
 * OpenTherm Library for ESP-IDF
 * Adapted from: https://github.com/ihormelnyk/opentherm_library
 * ISR-based software implementation using GPIO interrupts
 */

#include "opentherm_library.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "OT_LIB";

// OpenTherm instance structure
struct opentherm_s {
    gpio_num_t in_pin;
    gpio_num_t out_pin;
    bool is_slave;

    // State machine
    volatile ot_state_t state;
    volatile unsigned long response;
    volatile ot_response_status_t response_status;
    volatile int64_t response_timestamp_us;
    volatile uint8_t response_bit_index;

    // Callback
    ot_response_callback_t callback;
    void *callback_user_data;
};

// Forward declarations
static void gpio_isr_handler(void *arg);
static void send_bit(opentherm_t *ot, bool high);
static inline int read_state(opentherm_t *ot);
static inline void set_active_state(opentherm_t *ot);
static inline void set_idle_state(opentherm_t *ot);

// ============================================================================
// Initialization and cleanup
// ============================================================================

opentherm_t* opentherm_init(gpio_num_t in_pin, gpio_num_t out_pin, bool is_slave)
{
    opentherm_t *ot = calloc(1, sizeof(opentherm_t));
    if (!ot) {
        ESP_LOGE(TAG, "Failed to allocate OpenTherm instance");
        return NULL;
    }

    ot->in_pin = in_pin;
    ot->out_pin = out_pin;
    ot->is_slave = is_slave;
    ot->state = OT_STATE_NOT_INITIALIZED;
    ot->response = 0;
    ot->response_status = OT_RESPONSE_NONE;
    ot->callback = NULL;
    ot->callback_user_data = NULL;

    return ot;
}

esp_err_t opentherm_begin(opentherm_t *ot)
{
    if (!ot) {
        return ESP_ERR_INVALID_ARG;
    }

    // Configure output pin
    gpio_config_t out_conf = {
        .pin_bit_mask = (1ULL << ot->out_pin),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&out_conf);

    // Configure input pin
    gpio_config_t in_conf = {
        .pin_bit_mask = (1ULL << ot->in_pin),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_ANYEDGE,  // Trigger on both edges
    };
    gpio_config(&in_conf);

    // Install ISR service if not already installed
    gpio_install_isr_service(0);

    // Add ISR handler
    gpio_isr_handler_add(ot->in_pin, gpio_isr_handler, ot);

    // Set line to idle state (HIGH) and wait for stabilization
    set_idle_state(ot);
    vTaskDelay(pdMS_TO_TICKS(1000));  // 1 second activation delay

    ot->state = OT_STATE_READY;
    ESP_LOGI(TAG, "OpenTherm initialized: in=%d, out=%d, slave=%d",
             ot->in_pin, ot->out_pin, ot->is_slave);

    return ESP_OK;
}

void opentherm_set_response_callback(opentherm_t *ot, ot_response_callback_t callback, void *user_data)
{
    if (ot) {
        ot->callback = callback;
        ot->callback_user_data = user_data;
    }
}

void opentherm_end(opentherm_t *ot)
{
    if (!ot) return;

    // Remove ISR handler
    gpio_isr_handler_remove(ot->in_pin);

    // Reset pins
    gpio_reset_pin(ot->in_pin);
    gpio_reset_pin(ot->out_pin);

    ot->state = OT_STATE_NOT_INITIALIZED;
}

void opentherm_free(opentherm_t *ot)
{
    if (ot) {
        opentherm_end(ot);
        free(ot);
    }
}

// ============================================================================
// GPIO control functions
// ============================================================================

static inline void set_active_state(opentherm_t *ot)
{
    gpio_set_level(ot->out_pin, 0);  // LOW = active
}

static inline void set_idle_state(opentherm_t *ot)
{
    gpio_set_level(ot->out_pin, 1);  // HIGH = idle
}

static inline int read_state(opentherm_t *ot)
{
    return gpio_get_level(ot->in_pin);
}

static void send_bit(opentherm_t *ot, bool high)
{
    // Manchester encoding: bit '1' = LOW then HIGH, bit '0' = HIGH then LOW
    if (high) {
        set_active_state(ot);   // First half: LOW
    } else {
        set_idle_state(ot);     // First half: HIGH
    }
    esp_rom_delay_us(OT_HALF_BIT_TIME_US);

    if (high) {
        set_idle_state(ot);     // Second half: HIGH
    } else {
        set_active_state(ot);   // Second half: LOW
    }
    esp_rom_delay_us(OT_HALF_BIT_TIME_US);
}

// ============================================================================
// Send functions
// ============================================================================

bool opentherm_is_ready(opentherm_t *ot)
{
    return ot && (ot->state == OT_STATE_READY);
}

bool opentherm_send_request_async(opentherm_t *ot, unsigned long request)
{
    if (!ot || !opentherm_is_ready(ot)) {
        return false;
    }

    // Disable interrupts during state change
    portDISABLE_INTERRUPTS();
    ot->state = OT_STATE_REQUEST_SENDING;
    ot->response = 0;
    ot->response_status = OT_RESPONSE_NONE;
    portENABLE_INTERRUPTS();

    // Suspend scheduler to get accurate timing
    vTaskSuspendAll();

    // Send frame: start bit + 32 data bits + stop bit
    send_bit(ot, true);  // Start bit (always '1')

    for (int i = 31; i >= 0; i--) {
        bool bit = (request >> i) & 1;
        send_bit(ot, bit);
    }

    send_bit(ot, true);  // Stop bit (always '1')
    set_idle_state(ot);

    // Mark time and start waiting for response
    ot->response_timestamp_us = esp_timer_get_time();
    ot->state = OT_STATE_RESPONSE_WAITING;

    xTaskResumeAll();

    ESP_LOGI(TAG, "Sent request: 0x%08lX, now waiting for response", request);
    return true;
}

unsigned long opentherm_send_request(opentherm_t *ot, unsigned long request)
{
    if (!opentherm_send_request_async(ot, request)) {
        return 0;
    }

    // Block waiting for response (with timeout)
    while (!opentherm_is_ready(ot)) {
        opentherm_process(ot);
        vTaskDelay(pdMS_TO_TICKS(1));
    }

    return ot->response;
}

bool opentherm_send_response(opentherm_t *ot, unsigned long response)
{
    if (!ot || !opentherm_is_ready(ot)) {
        return false;
    }

    portDISABLE_INTERRUPTS();
    ot->state = OT_STATE_REQUEST_SENDING;
    ot->response = 0;
    ot->response_status = OT_RESPONSE_NONE;
    portENABLE_INTERRUPTS();

    vTaskSuspendAll();

    // Send frame
    send_bit(ot, true);  // Start bit

    for (int i = 31; i >= 0; i--) {
        bool bit = (response >> i) & 1;
        send_bit(ot, bit);
    }

    send_bit(ot, true);  // Stop bit
    set_idle_state(ot);

    ot->state = OT_STATE_READY;

    xTaskResumeAll();

    ESP_LOGD(TAG, "Sent response: 0x%08lX", response);
    return true;
}

// ============================================================================
// Receive ISR handler
// ============================================================================

static void IRAM_ATTR gpio_isr_handler(void *arg)
{
    opentherm_t *ot = (opentherm_t *)arg;

    // If ready and we're a slave, start listening on HIGH edge
    if (ot->state == OT_STATE_READY) {
        if (ot->is_slave && read_state(ot) == 1) {
            ot->state = OT_STATE_RESPONSE_WAITING;
        } else {
            return;
        }
    }

    int64_t now_us = esp_timer_get_time();

    if (ot->state == OT_STATE_RESPONSE_WAITING) {
        // Look for start bit HIGH
        if (read_state(ot) == 1) {
            ot->state = OT_STATE_RESPONSE_START_BIT;
            ot->response_timestamp_us = now_us;
        } else {
            ot->state = OT_STATE_RESPONSE_INVALID;
            ot->response_timestamp_us = now_us;
        }
    }
    else if (ot->state == OT_STATE_RESPONSE_START_BIT) {
        // Start bit should transition to LOW within 750µs
        int64_t elapsed = now_us - ot->response_timestamp_us;
        if (elapsed < 750 && read_state(ot) == 0) {
            ot->state = OT_STATE_RESPONSE_RECEIVING;
            ot->response_timestamp_us = now_us;
            ot->response_bit_index = 0;
            ot->response = 0;
        } else if (elapsed >= 750) {
            ot->state = OT_STATE_RESPONSE_INVALID;
            ot->response_timestamp_us = now_us;
        }
    }
    else if (ot->state == OT_STATE_RESPONSE_RECEIVING) {
        // Sample bit at mid-point (> 750µs after last transition)
        int64_t elapsed = now_us - ot->response_timestamp_us;
        if (elapsed > 750) {
            if (ot->response_bit_index < 32) {
                // Manchester: read inverted level (LOW = 1, HIGH = 0)
                bool bit = !read_state(ot);
                ot->response = (ot->response << 1) | bit;
                ot->response_timestamp_us = now_us;
                ot->response_bit_index++;
            } else {
                // All 32 bits received, this is stop bit
                ot->state = OT_STATE_RESPONSE_READY;
                ot->response_timestamp_us = now_us;
            }
        }
    }
}

// ============================================================================
// Process function (called from main loop)
// ============================================================================

void opentherm_process(opentherm_t *ot)
{
    if (!ot) return;

    portDISABLE_INTERRUPTS();
    ot_state_t st = ot->state;
    int64_t ts = ot->response_timestamp_us;
    portENABLE_INTERRUPTS();

    if (st == OT_STATE_READY || st == OT_STATE_NOT_INITIALIZED) {
        return;
    }

    int64_t now_us = esp_timer_get_time();
    int64_t elapsed = now_us - ts;

    // Timeout after 1 second
    if (elapsed > 1000000) {
        ot->state = OT_STATE_READY;
        ot->response_status = OT_RESPONSE_TIMEOUT;
        if (ot->callback) {
            ot->callback(ot->response, ot->response_status, ot->callback_user_data);
        }
        ESP_LOGI(TAG, "Response timeout in state %d after %lld us", st, elapsed);
    }
    else if (st == OT_STATE_RESPONSE_INVALID) {
        ot->state = OT_STATE_READY;
        ot->response_status = OT_RESPONSE_INVALID;
        if (ot->callback) {
            ot->callback(ot->response, ot->response_status, ot->callback_user_data);
        }
        ESP_LOGD(TAG, "Response invalid");
    }
    else if (st == OT_STATE_RESPONSE_READY) {
        ot->state = OT_STATE_READY;

        // Validate response
        if (opentherm_parity(ot->response) &&
            (ot->is_slave ? opentherm_is_valid_request(ot->response)
                          : opentherm_is_valid_response(ot->response))) {
            ot->response_status = OT_RESPONSE_SUCCESS;
            ESP_LOGD(TAG, "Response received: 0x%08lX", (unsigned long)ot->response);
        } else {
            ot->response_status = OT_RESPONSE_INVALID;
            ESP_LOGD(TAG, "Response invalid: parity or type check failed");
        }

        if (ot->callback) {
            ot->callback(ot->response, ot->response_status, ot->callback_user_data);
        }

        // Small delay before next request
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

// ============================================================================
// Getters
// ============================================================================

unsigned long opentherm_get_last_response(opentherm_t *ot)
{
    return ot ? ot->response : 0;
}

ot_response_status_t opentherm_get_last_response_status(opentherm_t *ot)
{
    return ot ? ot->response_status : OT_RESPONSE_NONE;
}

// ============================================================================
// Frame building and parsing helpers
// ============================================================================

bool opentherm_parity(unsigned long frame)
{
    uint8_t count = 0;
    for (int i = 0; i < 32; i++) {
        if (frame & (1UL << i)) {
            count++;
        }
    }
    return (count % 2) == 0;  // Even parity
}

unsigned long opentherm_build_request(otlib_message_type_t msg_type, uint8_t data_id, uint16_t data)
{
    unsigned long frame = ((unsigned long)msg_type << 28) |
                          ((unsigned long)data_id << 16) |
                          data;

    // Add parity bit if needed to make even parity
    if (!opentherm_parity(frame)) {
        frame |= (1UL << 31);
    }

    return frame;
}

unsigned long opentherm_build_response(otlib_message_type_t msg_type, uint8_t data_id, uint16_t data)
{
    return opentherm_build_request(msg_type, data_id, data);
}

otlib_message_type_t opentherm_get_message_type(unsigned long frame)
{
    return (otlib_message_type_t)((frame >> 28) & 0x7);
}

uint8_t opentherm_get_data_id(unsigned long frame)
{
    return (uint8_t)((frame >> 16) & 0xFF);
}

uint16_t opentherm_get_uint16(unsigned long frame)
{
    return (uint16_t)(frame & 0xFFFF);
}

float opentherm_get_float(unsigned long frame)
{
    uint16_t data = opentherm_get_uint16(frame);
    float value = (int16_t)data / 256.0f;
    return value;
}

bool opentherm_is_valid_request(unsigned long frame)
{
    if (!opentherm_parity(frame)) {
        return false;
    }

    otlib_message_type_t msg_type = opentherm_get_message_type(frame);
    return (msg_type == OTLIB_MSG_TYPE_READ_DATA ||
            msg_type == OTLIB_MSG_TYPE_WRITE_DATA ||
            msg_type == OTLIB_MSG_TYPE_INVALID_DATA);
}

bool opentherm_is_valid_response(unsigned long frame)
{
    if (!opentherm_parity(frame)) {
        return false;
    }

    otlib_message_type_t msg_type = opentherm_get_message_type(frame);
    return (msg_type == OTLIB_MSG_TYPE_READ_ACK ||
            msg_type == OTLIB_MSG_TYPE_WRITE_ACK ||
            msg_type == OTLIB_MSG_TYPE_DATA_INVALID ||
            msg_type == OTLIB_MSG_TYPE_UNKNOWN_ID);
}
