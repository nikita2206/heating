/*
 * OpenTherm C++ API Implementation
 * Direct implementation without pimpl pattern
 */

#include "opentherm.hpp"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <cstring>

namespace ot {

// Timing constants
static constexpr int OT_BIT_TIME_US = 1000;
static constexpr int OT_HALF_BIT_TIME_US = 500;

static const char* TAG = "OT_CPP";

// Frame implementation

bool Frame::isValidParityInternal(uint32_t frame) {
    uint8_t count = 0;
    for (int i = 0; i < 32; i++) {
        if (frame & (1UL << i)) {
            count++;
        }
    }
    return (count % 2) == 0;  // Even parity
}

Frame Frame::buildRequest(MessageType type, uint8_t dataId, uint16_t data) {
    uint32_t frame = ((static_cast<uint8_t>(type) << 28) |
                     (static_cast<uint32_t>(dataId) << 16) |
                     data);

    // Add parity bit if needed to make even parity
    if (!isValidParityInternal(frame)) {
        frame |= (1UL << 31);
    }

    return Frame(frame);
}

Frame Frame::buildResponse(MessageType type, uint8_t dataId, uint16_t data) {
    return buildRequest(type, dataId, data);
}

bool Frame::isValidParity() const {
    return isValidParityInternal(raw_);
}

bool Frame::isValidRequest() const {
    if (!isValidParity()) {
        return false;
    }

    MessageType msg_type = messageType();
    return (msg_type == MessageType::ReadData ||
            msg_type == MessageType::WriteData ||
            msg_type == MessageType::InvalidData);
}

bool Frame::isValidResponse() const {
    if (!isValidParity()) {
        return false;
    }

    MessageType msg_type = messageType();
    return (msg_type == MessageType::ReadAck ||
            msg_type == MessageType::WriteAck ||
            msg_type == MessageType::DataInvalid ||
            msg_type == MessageType::UnknownId);
}

// OpenTherm implementation

OpenTherm::OpenTherm(gpio_num_t inPin, gpio_num_t outPin, bool isSlave)
    : thread_handle_(nullptr)
    , pending_outgoing_(std::nullopt)
    , received_request_(std::nullopt)
    , received_response_(std::nullopt)
    , currently_sending_(false)
    , bus_stabilized_(false)
    , in_pin_(inPin)
    , out_pin_(outPin)
    , is_slave_(isSlave)
    , initialized_(false)
    , rx_state_(RxState::Idle)
    , rx_timestamp_us_(0)
    , rx_bit_index_(0)
    , rx_data_(0)
{
}

OpenTherm::~OpenTherm() {
    end();
}

esp_err_t OpenTherm::begin() {
    if (initialized_) {
        return ESP_ERR_INVALID_STATE;
    }

    // Configure output pin
    gpio_config_t out_conf = {
        .pin_bit_mask = (1ULL << out_pin_),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&out_conf);
    if (err != ESP_OK) {
        end();
        return err;
    }

    // Configure input pin
    gpio_config_t in_conf = {
        .pin_bit_mask = (1ULL << in_pin_),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_ANYEDGE,  // Detect both edges for Manchester decoding
    };
    err = gpio_config(&in_conf);
    if (err != ESP_OK) {
        end();
        return err;
    }

    // Install ISR service if not already installed
    err = gpio_install_isr_service(0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        end();
        return err;
    }

    // Add ISR handler
    err = gpio_isr_handler_add(in_pin_, OpenTherm::staticGpioIsrHandler, this);
    if (err != ESP_OK) {
        end();
        return err;
    }

    // Set line to idle state (HIGH)
    // Bus stabilization will be done by thread before first send
    gpio_set_level(out_pin_, 1);

    // Create communication thread

    initialized_ = true;
    BaseType_t result = xTaskCreate(
        communicationThread,       // Task function
        "ot_comm",                 // Task name
        2048,                      // Stack size
        this,                      // Task parameter
        5,                         // Priority
        &thread_handle_            // Task handle
    );

    if (result != pdPASS) {
        end();
        return ESP_ERR_NO_MEM;
    }
    
    ESP_LOGI(TAG, "OpenTherm initialized: in=%d, out=%d, slave=%d", in_pin_, out_pin_, is_slave_);
    return ESP_OK;
}

void OpenTherm::end() {
    if (!initialized_) return;

    initialized_ = false;

    // Stop the communication thread
    if (thread_handle_) {
        vTaskDelete(thread_handle_);
        thread_handle_ = nullptr;
    }

    // Clear pending frames and reset state
    pending_outgoing_.reset();
    received_request_.reset();
    received_response_.reset();
    bus_stabilized_ = false;

    // Remove ISR handler
    gpio_isr_handler_remove(in_pin_);

    // Reset pins
    gpio_reset_pin(in_pin_);
    gpio_reset_pin(out_pin_);
}

bool OpenTherm::isReady() const {
    return initialized_;
}

bool OpenTherm::sendRequest(Frame request) {
    if (!initialized_) {
        return false;
    }

    // If currently sending, reject (don't interrupt transmission)
    if (currently_sending_) {
        return false;
    }

    // Schedule/override the pending outgoing frame
    pending_outgoing_ = request;
    ESP_LOGI(TAG, "Request scheduled: 0x%08lX", request.raw());
    return true;
}

bool OpenTherm::sendResponse(Frame response) {
    if (!initialized_) {
        return false;
    }

    // If currently sending, reject (don't interrupt transmission)
    if (currently_sending_) {
        return false;
    }

    // Schedule/override the pending outgoing frame
    pending_outgoing_ = response;
    ESP_LOGI(TAG, "Response scheduled: 0x%08lX", response.raw());
    return true;
}


std::optional<Frame> OpenTherm::popResponse() {
    if (!received_response_) return std::nullopt;

    Frame response = *received_response_;
    received_response_.reset();
    return response;
}

std::optional<Frame> OpenTherm::popRequest() {
    if (!received_request_) return std::nullopt;

    Frame request = *received_request_;
    received_request_.reset();
    return request;
}


// Private helper methods

void OpenTherm::sendBit(bool high) {
    // Manchester encoding: bit '1' = LOW then HIGH, bit '0' = HIGH then LOW
    if (high) {
        gpio_set_level(out_pin_, 0);
    } else {
        gpio_set_level(out_pin_, 1);
    }
    esp_rom_delay_us(OT_HALF_BIT_TIME_US);

    if (high) {
        gpio_set_level(out_pin_, 1);
    } else {
        gpio_set_level(out_pin_, 0);
    }
    esp_rom_delay_us(OT_HALF_BIT_TIME_US);
}

int OpenTherm::readState() const {
    return gpio_get_level(in_pin_);
}

void IRAM_ATTR OpenTherm::staticGpioIsrHandler(void *arg) {
    auto* port = static_cast<OpenTherm*>(arg);
    if (port) {
        port->gpioIsrHandler();
    }
}

void IRAM_ATTR OpenTherm::gpioIsrHandler() {
    if (UNLIKELY(!initialized_)) return;

    int64_t now_us = esp_timer_get_time();

    if (rx_state_ == RxState::Idle) {
        // Look for start bit (HIGH edge indicates start of Manchester encoded frame)
        if (readState() == 1) {
            rx_state_ = RxState::StartBit;
            rx_timestamp_us_ = now_us;
        }
        return;
    }

    if (rx_state_ == RxState::StartBit) {
        // Start bit should transition to LOW within 750µs for valid Manchester encoding
        int64_t elapsed = now_us - rx_timestamp_us_;
        if (elapsed < 750 && readState() == 0) {
            // Valid start bit transition, begin receiving data
            rx_state_ = RxState::Receiving;
            rx_timestamp_us_ = now_us;
            rx_bit_index_ = 0;
            rx_data_ = 0;
        } else if (elapsed >= 750) {
            // Invalid start bit timing, reset
            rx_state_ = RxState::Idle;
        }
        return;
    }

    if (rx_state_ == RxState::Receiving) {
        // Sample bit at mid-point (> 750µs after last transition)
        int64_t elapsed = now_us - rx_timestamp_us_;
        if (elapsed > 750) {
            if (rx_bit_index_ < 32) {
                // Manchester: read inverted level (LOW = 1, HIGH = 0)
                bool bit = !readState();
                rx_data_ = (rx_data_ << 1) | bit;
                rx_timestamp_us_ = now_us;
                rx_bit_index_++;
            } else {
                // All 32 bits received, frame complete
                rx_state_ = RxState::Idle;

                // Signal the thread that raw frame data is ready for processing
                if (thread_handle_) {
                    BaseType_t higherPriorityTaskWoken = pdFALSE;
                    vTaskNotifyGiveFromISR(thread_handle_, &higherPriorityTaskWoken);
                    portYIELD_FROM_ISR(higherPriorityTaskWoken);
                }
            }
        }
    }
}


// Helper functions

const char* toString(MessageType type) {
    switch (type) {
        case MessageType::ReadData:    return "READ_DATA";
        case MessageType::WriteData:   return "WRITE_DATA";
        case MessageType::InvalidData: return "INVALID_DATA";
        case MessageType::Reserved:    return "RESERVED";
        case MessageType::ReadAck:     return "READ_ACK";
        case MessageType::WriteAck:    return "WRITE_ACK";
        case MessageType::DataInvalid: return "DATA_INVALID";
        case MessageType::UnknownId:   return "UNKNOWN_ID";
        default:                       return "UNKNOWN";
    }
}

const char* toString(ResponseStatus status) {
    switch (status) {
        case ResponseStatus::None:    return "NONE";
        case ResponseStatus::Success: return "SUCCESS";
        case ResponseStatus::Invalid: return "INVALID";
        case ResponseStatus::Timeout: return "TIMEOUT";
        default:                      return "UNKNOWN";
    }
}

// Thread function
void OpenTherm::communicationThread(void* arg) {
    OpenTherm* self = static_cast<OpenTherm*>(arg);
    self->runCommunicationLoop();
}

void OpenTherm::runCommunicationLoop() {
    ESP_LOGI(TAG, "Communication thread started");

    while (initialized_) {
        // Wait for ISR notification with timeout to allow sending periodic requests
        if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100)) > 0) {
            // ISR notified us that raw frame data is ready - process it
            Frame frame(rx_data_);
            if (frame.isValidParity()) {
                if (is_slave_) {
                    received_request_ = frame;
                    ESP_LOGI(TAG, "Received request: 0x%08lX", rx_data_);
                } else {
                    received_response_ = frame;
                    ESP_LOGI(TAG, "Received response: 0x%08lX", rx_data_);
                }
            } else {
                ESP_LOGW(TAG, "Received invalid frame: 0x%08lX", rx_data_);
            }
        }

        // Check if we have outgoing data to send (either response or new request)
        if (pending_outgoing_) {
            // First send after initialization requires bus stabilization
            if (!bus_stabilized_) {
                ESP_LOGI(TAG, "Performing initial bus stabilization (1 second)");
                vTaskDelay(pdMS_TO_TICKS(900)); // OpenTherm protocol requirement
                bus_stabilized_ = true;
                ESP_LOGI(TAG, "Bus stabilization complete, sending first frame");
            }
            // } else {
            //     // Small delay before sending to avoid bus conflicts
            //     vTaskDelay(pdMS_TO_TICKS(10));
            // }

            currently_sending_ = true;
            sendFrame(*pending_outgoing_);
            pending_outgoing_.reset();
            currently_sending_ = false;
        }
    }

    ESP_LOGI(TAG, "Communication thread stopped");
}

void OpenTherm::sendFrame(Frame frame) {
    ESP_LOGI(TAG, "Sending frame: 0x%08lX", frame.raw());

    // Send start bit
    sendBit(true);

    // Send 32 data bits (MSB first)
    uint32_t data = frame.raw();
    for (int i = 31; i >= 0; i--) {
        bool bit = (data >> i) & 1;
        sendBit(bit);
    }

    // Send stop bit
    sendBit(true);

    // Ensure line goes idle
    gpio_set_level(out_pin_, 1);
}


} // namespace ot
