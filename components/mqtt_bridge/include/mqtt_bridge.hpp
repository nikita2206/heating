/*
 * MQTT Bridge (C++)
 *
 * Receives external overrides (TSet, CH enable) via MQTT.
 * Publishes diagnostic sensors with Home Assistant discovery.
 */

#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <optional>
#include <functional>
#include <chrono>
#include <memory>
#include "esp_err.h"

namespace ot {

/**
 * MQTT configuration
 */
struct MqttConfig {
    bool enable = false;
    std::string brokerUri;
    std::string clientId;
    std::string username;
    std::string password;
    std::string baseTopic = "ot_gateway";
    std::string discoveryPrefix = "homeassistant";
};

/**
 * MQTT state snapshot (thread-safe copy)
 */
struct MqttState {
    bool connected = false;
    bool available = false;  // connected and heartbeat fresh

    std::optional<float> lastTsetC;
    std::optional<bool> lastChEnable;
    std::optional<bool> lastControlEnabled;

    std::chrono::milliseconds lastUpdateTime{0};
    std::chrono::milliseconds lastHeartbeatTime{0};

    // Heartbeat value (for monitoring)
    std::optional<float> heartbeatValue;
};

// Callback for control mode changes
using ControlModeCallback = std::function<void(bool enabled)>;

/**
 * RAII MQTT client wrapper
 *
 * Connects to MQTT broker, subscribes to command topics,
 * and publishes sensor values with Home Assistant discovery.
 */
class MqttBridge {
public:
    explicit MqttBridge(const MqttConfig& config);
    ~MqttBridge();

    // Non-copyable
    MqttBridge(const MqttBridge&) = delete;
    MqttBridge& operator=(const MqttBridge&) = delete;

    // Lifecycle
    [[nodiscard]] esp_err_t start();
    void stop();
    [[nodiscard]] bool isRunning() const;

    // Thread-safe state access
    [[nodiscard]] MqttState state() const;

    // Publish sensor value with Home Assistant discovery
    [[nodiscard]] esp_err_t publishSensor(std::string_view id, std::string_view name,
                                          std::string_view unit, float value, bool valid);

    // Control mode callback
    void setControlCallback(ControlModeCallback callback);

    // Publish control state (for UI sync)
    void publishControlState(bool enabled);

    // Configuration persistence (static utilities)
    [[nodiscard]] static esp_err_t loadConfig(MqttConfig& config);
    [[nodiscard]] static esp_err_t saveConfig(const MqttConfig& config);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace ot
