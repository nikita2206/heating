/*
 * Boiler Manager (C++)
 *
 * Main control loop coordinating thermostat/boiler communication.
 * Provides diagnostic injection, MQTT control, and message logging.
 */

#pragma once

#include <cstdint>
#include <chrono>
#include <optional>
#include <functional>
#include <memory>
#include <string_view>
#include "esp_err.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "opentherm_drv.h"

namespace ot {

// Operation modes
enum class ManagerMode {
    Proxy,       // Intercept ID=0, inject diagnostics
    Passthrough, // Pass everything through unchanged
    Control      // Apply MQTT overrides, stub thermostat replies
};

// Message source categories for logging
enum class MessageSource {
    ThermostatBoiler,   // Proxied: Thermostat <-> Boiler
    GatewayBoiler,      // Gateway <-> Boiler (diagnostics)
    ThermostatGateway   // Thermostat <-> Gateway (control mode)
};

// Polymorphic value for boiler state
struct BoilerStateValue {
    enum class Type { None, Float, UInt16 };
    
    Type type{Type::None};
    union {
        float fVal;
        uint16_t uVal;
    };
    std::chrono::milliseconds timestamp{0};

    [[nodiscard]] bool isValid() const { return type != Type::None; }

    void update(float v) {
        type = Type::Float;
        fVal = v;
        updateTimestamp();
    }

    void update(uint16_t v) {
        type = Type::UInt16;
        uVal = v;
        updateTimestamp();
    }

    [[nodiscard]] float asFloatOr(float defaultVal) const {
        switch (type) {
            case Type::Float: return fVal;
            case Type::UInt16: return static_cast<float>(uVal);
            default: return defaultVal;
        }
    }

    [[nodiscard]] uint16_t raw() const {
        switch (type) {
            // OpenTherm f8.8 format is a 16-bit signed fixed-point number.
            // High byte = integer part, Low byte = fractional part.
            // Multiplying by 256.0f shifts the float so it fits into this representation.
            case Type::Float: return static_cast<uint16_t>(static_cast<int16_t>(fVal * 256.0f));
            case Type::UInt16: return uVal;
            default: return 0;
        }
    }

private:
    void updateTimestamp() {
        timestamp = std::chrono::milliseconds(esp_timer_get_time() / 1000);
    }
};

// New boiler state structure
struct BoilerState {
    // ID=0 Status Flags (Slave Status)
    bool fault{false};
    bool chActive{false};
    bool dhwActive{false};
    bool flameOn{false};
    bool coolingActive{false};
    bool ch2Active{false};
    bool diagnosticEvent{false};

    // Properties
    BoilerStateValue tBoiler;
    BoilerStateValue maxChWaterTemp;
    BoilerStateValue tReturn;
    BoilerStateValue tDhw;
    BoilerStateValue tDhw2;
    BoilerStateValue tOutside;
    BoilerStateValue tExhaust;
    BoilerStateValue tHeatExchanger;
    BoilerStateValue tFlowCh2;
    BoilerStateValue tStorage;
    BoilerStateValue tCollector;
    BoilerStateValue tSetpoint;
    BoilerStateValue modulationLevel;
    BoilerStateValue pressure;
    BoilerStateValue flowRate;
    BoilerStateValue faultCode;
    BoilerStateValue diagCode;
    BoilerStateValue burnerStarts;
    BoilerStateValue dhwBurnerStarts;
    BoilerStateValue chPumpStarts;
    BoilerStateValue dhwPumpStarts;
    BoilerStateValue burnerHours;
    BoilerStateValue dhwBurnerHours;
    BoilerStateValue chPumpHours;
    BoilerStateValue dhwPumpHours;
    BoilerStateValue maxCapacity;
    BoilerStateValue minModLevel;
    BoilerStateValue fanSetpoint;
    BoilerStateValue fanCurrent;
    BoilerStateValue fanExhaustRpm;
    BoilerStateValue fanSupplyRpm;
    BoilerStateValue co2Exhaust;
    BoilerStateValue slaveMemberId;
    BoilerStateValue slaveConfigFlags;
    BoilerStateValue slaveVersion;
    BoilerStateValue slaveType;
    BoilerStateValue slaveOTVersion;
    BoilerStateValue dhwSetUB;
    BoilerStateValue dhwSetLB;
    BoilerStateValue maxTSetUB;
    BoilerStateValue maxTSetLB;
    BoilerStateValue custom200;
    BoilerStateValue custom202;
};

// Thermostat state structure
struct ThermostatState {
    // ID=0 Master Status Flags
    bool chEnable{false};
    bool dhwEnable{false};
    bool coolingEnable{false};
    bool otcActive{false};
    bool ch2Enable{false};
    
    // Properties
    BoilerStateValue tSet;      // Control Setpoint (ID 1)
    BoilerStateValue tRoom;     // Room Temperature (ID 24)
    BoilerStateValue maxChSet;  // Max CH Setpoint (ID 57)
    
    // Timestamp of last valid command from thermostat
    std::chrono::milliseconds lastUpdate{0};
};

// Status snapshot for external queries
struct ManagerStatus {
    bool controlEnabled = false;
    bool controlActive = false;
    bool fallbackActive = false;
    bool mqttAvailable = false;
    float demandTsetC = 0.0f;
    bool demandChEnabled = false;
    std::chrono::milliseconds lastDemandTime{0};
};

// Message callback type
using MessageCallback = std::function<void(std::string_view direction,
                                           MessageSource source,
                                           OpenThermFrame message)>;

/**
 * Configuration for boiler manager
 */
struct ManagerConfig {
    ManagerMode mode = ManagerMode::Proxy;
    uint32_t interceptRate = 10;  // Intercept every Nth frame that matches interceptable IDs
    uint32_t taskStackSize = 4096;
    UBaseType_t taskPriority = 5;

    // OpenTherm pin configuration
    gpio_num_t thermostatInPin = GPIO_NUM_16;
    gpio_num_t thermostatOutPin = GPIO_NUM_17;
    gpio_num_t boilerInPin = GPIO_NUM_18;
    gpio_num_t boilerOutPin = GPIO_NUM_19;

    // Maximum allowed setpoint (default 100.0 - effectively disabled)
    float maxSetpoint = 100.0f;
};

/**
 * RAII boiler manager with main control loop
 *
 * Coordinates communication between thermostat and boiler:
 * - Runs in its own FreeRTOS task
 * - Polls queues for requests/responses
 * - Injects diagnostic queries
 * - Handles MQTT control overrides
 */
class BoilerManager {
public:
    explicit BoilerManager(const ManagerConfig& config = {});
    ~BoilerManager();

    // Non-copyable
    BoilerManager(const BoilerManager&) = delete;
    BoilerManager& operator=(const BoilerManager&) = delete;

    // Start the main loop task
    [[nodiscard]] esp_err_t start();
    void stop();
    [[nodiscard]] bool isRunning() const;

    // Diagnostics access
    [[nodiscard]] const BoilerState& state() const;
    [[nodiscard]] const ThermostatState& thermostatState() const;

    // Control mode
    [[nodiscard]] ManagerStatus status() const;
    void setMode(ManagerMode mode);
    void setMaxSetpoint(float maxTemp);
    [[nodiscard]] float getMaxSetpoint() const;

    // Manual write to boiler (thread-safe, blocks up to timeout)
    [[nodiscard]] esp_err_t writeData(uint8_t dataId, uint16_t dataValue,
                                      std::optional<OpenThermFrame>& response,
                                      std::chrono::milliseconds timeout = std::chrono::seconds(2));

    // Message callback for logging
    void setMessageCallback(MessageCallback callback);

    // Set MQTT bridge for diagnostics publishing
    void setMqttBridge(class MqttBridge* mqtt);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

// Helper functions
[[nodiscard]] const char* toString(ManagerMode mode);
[[nodiscard]] const char* toString(MessageSource source);

} // namespace ot
