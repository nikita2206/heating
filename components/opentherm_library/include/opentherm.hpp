/*
 * OpenTherm C++ API for ESP-IDF
 * Modern C++17 wrapper around the ISR-based library
 */

#pragma once

#include <cstdint>
#include <memory>
#include <functional>
#include <optional>
#include <queue>
#include "driver/gpio.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

namespace ot {

#define LIKELY(x)   __builtin_expect(!!(x), 1)
#define UNLIKELY(x) __builtin_expect(!!(x), 0)

// Response status from OpenTherm communication
enum class ResponseStatus {
    None,
    Success,
    Invalid,
    Timeout
};

// OpenTherm message types (3-bit field in frame)
enum class MessageType : uint8_t {
    ReadData     = 0b000,
    WriteData    = 0b001,
    InvalidData  = 0b010,
    Reserved     = 0b011,
    ReadAck      = 0b100,
    WriteAck     = 0b101,
    DataInvalid  = 0b110,
    UnknownId    = 0b111
};

// Common OpenTherm Data IDs
namespace DataId {
    constexpr uint8_t Status         = 0;
    constexpr uint8_t TSet           = 1;
    constexpr uint8_t MasterConfig   = 2;
    constexpr uint8_t SlaveConfig    = 3;
    constexpr uint8_t FaultFlags     = 5;
    constexpr uint8_t RelModLevel    = 17;
    constexpr uint8_t CHPressure     = 18;
    constexpr uint8_t TBoiler        = 25;
    constexpr uint8_t TDhw           = 26;
    constexpr uint8_t TOutside       = 27;
    constexpr uint8_t TReturn        = 28;
}

/**
 * OpenTherm 32-bit frame wrapper
 *
 * Frame format: [parity:1][msg_type:3][spare:4][data_id:8][data_value:16]
 * - Bit 31: Parity (even)
 * - Bits 28-30: Message type
 * - Bits 24-27: Spare (always 0)
 * - Bits 16-23: Data ID
 * - Bits 0-15: Data value
 */
class Frame {
public:
    constexpr Frame() : raw_(0) {}
    constexpr explicit Frame(uint32_t raw) : raw_(raw) {}

    // Build request/response with automatic parity calculation
    [[nodiscard]] static Frame buildRequest(MessageType type, uint8_t dataId, uint16_t data);
    [[nodiscard]] static Frame buildResponse(MessageType type, uint8_t dataId, uint16_t data);

    // Accessors
    [[nodiscard]] constexpr uint32_t raw() const { return raw_; }
    [[nodiscard]] constexpr MessageType messageType() const {
        return static_cast<MessageType>((raw_ >> 28) & 0x7);
    }
    [[nodiscard]] constexpr uint8_t dataId() const {
        return static_cast<uint8_t>((raw_ >> 16) & 0xFF);
    }
    [[nodiscard]] constexpr uint16_t dataValue() const {
        return static_cast<uint16_t>(raw_ & 0xFFFF);
    }
    [[nodiscard]] constexpr uint8_t highByte() const {
        return static_cast<uint8_t>((raw_ >> 8) & 0xFF);
    }
    [[nodiscard]] constexpr uint8_t lowByte() const {
        return static_cast<uint8_t>(raw_ & 0xFF);
    }

    // Convert data value to f8.8 fixed-point float
    [[nodiscard]] float asFloat() const {
        return static_cast<int16_t>(dataValue()) / 256.0f;
    }

    // Validation
    [[nodiscard]] bool isValidParity() const;
    [[nodiscard]] bool isValidRequest() const;
    [[nodiscard]] bool isValidResponse() const;

    // Boolean conversion (true if non-zero frame)
    [[nodiscard]] constexpr explicit operator bool() const { return raw_ != 0; }

    // Comparison
    [[nodiscard]] constexpr bool operator==(const Frame& other) const { return raw_ == other.raw_; }
    [[nodiscard]] constexpr bool operator!=(const Frame& other) const { return raw_ != other.raw_; }

private:
    // Internal parity check
    [[nodiscard]] static bool isValidParityInternal(uint32_t frame);

private:
    uint32_t raw_;
};

/**
 * OpenTherm communication port
 *
 * Manages GPIO configuration, ISR handlers, and Manchester encoding/decoding.
 * ISR-based implementation with Manchester encoding.
 */
class OpenTherm {
public:
    explicit OpenTherm(gpio_num_t inPin, gpio_num_t outPin, bool isSlave);
    ~OpenTherm();

    // Non-copyable
    OpenTherm(const OpenTherm&) = delete;
    OpenTherm& operator=(const OpenTherm&) = delete;

    // Non-movable (due to ISR registration)
    OpenTherm(OpenTherm&& other) = delete;
    OpenTherm& operator=(OpenTherm&& other) = delete;

    // Lifecycle
    [[nodiscard]] esp_err_t begin();
    void end();

    // State queries
    [[nodiscard]] bool isReady() const;
    [[nodiscard]] bool isInitialized() const { return initialized_; }

    // Send frames (non-blocking, queued for sending)
    [[nodiscard]] bool sendRequest(Frame request);
    [[nodiscard]] bool sendResponse(Frame response);

    // Receive frames (non-blocking, from queue)
    [[nodiscard]] std::optional<Frame> popRequest();
    [[nodiscard]] std::optional<Frame> popResponse();

    // ISR handler (public for static ISR to access)
    void gpioIsrHandler();

private:
    // Thread
    TaskHandle_t thread_handle_;

    // Pending frames (single slot each - override on new)
    std::optional<Frame> pending_outgoing_;    // Next frame to send
    std::optional<Frame> received_request_;    // Last received request (slave mode)
    std::optional<Frame> received_response_;   // Last received response (master mode)

    // Sending state
    volatile bool currently_sending_;          // True when actively transmitting
    bool bus_stabilized_;                     // True after initial 1-second stabilization

    // GPIO and configuration
    gpio_num_t in_pin_;
    gpio_num_t out_pin_;
    bool is_slave_;
    bool initialized_;

    // ISR state machine for reception
    enum class RxState {
        Idle,
        StartBit,
        Receiving,
        Complete
    };
    volatile RxState rx_state_;
    volatile int64_t rx_timestamp_us_;
    volatile uint8_t rx_bit_index_;
    volatile uint32_t rx_data_;

    // Thread function
    static void communicationThread(void* arg);
    void runCommunicationLoop();

    // Communication helpers
    void sendFrame(Frame frame);

    // GPIO helpers
    void sendBit(bool high);
    int readState() const;

    // Static ISR handler
    static void staticGpioIsrHandler(void* arg);
};

// Helper functions
[[nodiscard]] const char* toString(MessageType type);
[[nodiscard]] const char* toString(ResponseStatus status);

} // namespace ot
