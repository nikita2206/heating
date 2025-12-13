/*
 * OpenTherm C++ API for ESP-IDF
 * Modern C++17 wrapper around the ISR-based library
 */

#pragma once

#include <cstdint>
#include <memory>
#include <functional>
#include <optional>
#include "driver/gpio.h"
#include "esp_err.h"

namespace ot {

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
    uint32_t raw_;
};

// Callback type for async response handling
using ResponseCallback = std::function<void(Frame response, ResponseStatus status)>;

/**
 * OpenTherm port configuration
 */
struct PortConfig {
    gpio_num_t inPin;
    gpio_num_t outPin;
    bool isSlave;  // true = boiler mode, false = thermostat/master mode
};

/**
 * RAII wrapper for OpenTherm communication port
 *
 * Manages GPIO configuration, ISR handlers, and Manchester encoding/decoding.
 * The underlying implementation uses the ISR-based C library.
 */
class Port {
public:
    explicit Port(const PortConfig& config);
    explicit Port(gpio_num_t inPin, gpio_num_t outPin, bool isSlave);
    ~Port();

    // Non-copyable
    Port(const Port&) = delete;
    Port& operator=(const Port&) = delete;

    // Movable
    Port(Port&& other) noexcept;
    Port& operator=(Port&& other) noexcept;

    // Lifecycle
    [[nodiscard]] esp_err_t begin();
    void end();

    // State queries
    [[nodiscard]] bool isReady() const;
    [[nodiscard]] bool isInitialized() const { return impl_ != nullptr; }

    // Synchronous communication (blocking, ~1 second timeout)
    [[nodiscard]] Frame sendRequest(Frame request);

    // Asynchronous communication
    [[nodiscard]] bool sendRequestAsync(Frame request);
    void setResponseCallback(ResponseCallback callback);

    // Response (slave mode)
    [[nodiscard]] bool sendResponse(Frame response);

    // Main loop processing - must call regularly
    void process();

    // Last response access
    [[nodiscard]] Frame lastResponse() const;
    [[nodiscard]] ResponseStatus lastResponseStatus() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

// Helper functions
[[nodiscard]] const char* toString(MessageType type);
[[nodiscard]] const char* toString(ResponseStatus status);

} // namespace ot
