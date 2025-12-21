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
#include "open_therm.h"
#include "esp_err.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"

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

// Diagnostic value with timestamp
struct DiagnosticValue {
    std::optional<float> value;
    std::chrono::milliseconds timestamp{0};

    [[nodiscard]] bool isValid() const { return value.has_value(); }

    void update(float v) {
        value = v;
        timestamp = std::chrono::milliseconds(esp_timer_get_time() / 1000);
    }

    void invalidate() {
        value.reset();
    }

    [[nodiscard]] float valueOr(float defaultVal) const {
        return value.value_or(defaultVal);
    }
};

// Complete diagnostic state
struct Diagnostics {
    DiagnosticValue tBoiler;
    DiagnosticValue maxChWaterTemp;
    DiagnosticValue tReturn;
    DiagnosticValue tDhw;
    DiagnosticValue tDhw2;
    DiagnosticValue tOutside;
    DiagnosticValue tExhaust;
    DiagnosticValue tHeatExchanger;
    DiagnosticValue tFlowCh2;
    DiagnosticValue tStorage;
    DiagnosticValue tCollector;
    DiagnosticValue tSetpoint;
    DiagnosticValue modulationLevel;
    DiagnosticValue pressure;
    DiagnosticValue flowRate;
    DiagnosticValue faultCode;
    DiagnosticValue diagCode;
    DiagnosticValue burnerStarts;
    DiagnosticValue dhwBurnerStarts;
    DiagnosticValue chPumpStarts;
    DiagnosticValue dhwPumpStarts;
    DiagnosticValue burnerHours;
    DiagnosticValue dhwBurnerHours;
    DiagnosticValue chPumpHours;
    DiagnosticValue dhwPumpHours;
    DiagnosticValue maxCapacity;
    DiagnosticValue minModLevel;
    DiagnosticValue fanSetpoint;
    DiagnosticValue fanCurrent;
    DiagnosticValue fanExhaustRpm;
    DiagnosticValue fanSupplyRpm;
    DiagnosticValue co2Exhaust;
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
                                           Frame message)>;

/**
 * Configuration for boiler manager
 */
struct ManagerConfig {
    ManagerMode mode = ManagerMode::Proxy;
    uint32_t interceptRate = 10;  // Intercept every Nth ID=0 frame
    uint32_t taskStackSize = 4096;
    UBaseType_t taskPriority = 5;

    // OpenTherm pin configuration
    gpio_num_t thermostatInPin = GPIO_NUM_16;
    gpio_num_t thermostatOutPin = GPIO_NUM_17;
    gpio_num_t boilerInPin = GPIO_NUM_18;
    gpio_num_t boilerOutPin = GPIO_NUM_19;

    // Signal polarity inversion (depends on optocoupler circuit)
    bool thermostatInvertOutput = false;
    bool thermostatInvertInput = false;
    bool boilerInvertOutput = false;
    bool boilerInvertInput = false;
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
    [[nodiscard]] const Diagnostics& diagnostics() const;

    // Control mode
    void setControlEnabled(bool enabled);
    [[nodiscard]] ManagerStatus status() const;
    void setMode(ManagerMode mode);

    // Manual write to boiler (thread-safe, blocks up to timeout)
    [[nodiscard]] esp_err_t writeData(uint8_t dataId, uint16_t dataValue,
                                      std::optional<Frame>& response,
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
