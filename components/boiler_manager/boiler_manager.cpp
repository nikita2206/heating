/*
 * Boiler Manager Implementation (C++)
 */

#include "boiler_manager.hpp"
#include "mqtt_bridge.hpp"
#include "open_therm.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <atomic>
#include <cstring>

static const char* TAG = "BoilerMgr";

namespace ot {

// Diagnostic command definition
struct DiagnosticCmd {
    uint8_t dataId;
    const char* name;
};

// Diagnostic commands to poll
static constexpr DiagnosticCmd DIAG_COMMANDS[] = {
    {25, "Tboiler"},
    {57, "BoilerStatus"},
    {28, "Tret"},
    {26, "Tdhw"},
    {1, "TSet"},
    {17, "RelModLevel"},
    {18, "CHPressure"},
    {27, "Toutside"},
    {33, "Texhaust"},
    {34, "TboilerHeatExchanger"},
    {19, "DHWFlowRate"},
    {5, "ASFflags"},
    {115, "OEMDiagnosticCode"},
    {15, "MaxCapacityMinModLevel"},
    {35, "BoilerFanSpeed"},
    {32, "Tdhw2"},
    {31, "TflowCH2"},
    {29, "Tstorage"},
    {30, "Tcollector"},
    {79, "CO2exhaust"},
    {84, "RPMexhaust"},
    {85, "RPMsupply"},
    {116, "BurnerStarts"},
    {119, "DHWBurnerStarts"},
    {117, "CHPumpStarts"},
    {118, "DHWPumpStarts"},
    {120, "BurnerHours"},
    {123, "DHWBurnerHours"},
    {121, "CHPumpHours"},
    {122, "DHWPumpHours"},
};

static constexpr size_t DIAG_COMMANDS_COUNT = sizeof(DIAG_COMMANDS) / sizeof(DIAG_COMMANDS[0]);

// Loop states
enum class LoopState {
    Idle,
    WaitBoilerResponse,
    WaitDiagResponse
};

class BoilerManager::Impl {
public:
    explicit Impl(const ManagerConfig& config)
        : config_(config)
    {
    }

    ~Impl() {
        stop();
    }

    esp_err_t start() {
        // Create OpenTherm instances with configured pins
        thermostat_ = std::make_unique<OpenTherm>(
            config_.thermostatInPin, config_.thermostatOutPin, true);
        boiler_ = std::make_unique<OpenTherm>(
            config_.boilerInPin, config_.boilerOutPin, false);

        // Initialize OpenTherm instances
        thermostat_->begin();
        boiler_->begin();

        running_ = true;

        BaseType_t ret = xTaskCreate(
            &Impl::taskEntry,
            "bm_main",
            config_.taskStackSize > 0 ? config_.taskStackSize : 4096,
            this,
            config_.taskPriority > 0 ? config_.taskPriority : 5,
            &taskHandle_
        );

        if (ret != pdPASS) {
            running_ = false;
            return ESP_FAIL;
        }

        ESP_LOGI(TAG, "Main loop started in %s mode", toString(config_.mode));
        return ESP_OK;
    }

    void stop() {
        running_ = false;
        if (thermostat_) thermostat_->end();
        if (boiler_) boiler_->end();
        if (taskHandle_) {
            vTaskDelay(pdMS_TO_TICKS(150));
            taskHandle_ = nullptr;
        }
    }

    bool isRunning() const { return running_.load(); }

    const Diagnostics& diagnostics() const { return diagnostics_; }

    void setControlEnabled(bool enabled) {
        // No-op in passthrough mode
        (void)enabled;
    }

    ManagerStatus status() const {
        // Return default status for passthrough mode
        ManagerStatus s;
        s.controlEnabled = false;
        s.controlActive = false;
        s.fallbackActive = false;
        s.demandTsetC = 0.0f;
        s.demandChEnabled = false;
        s.lastDemandTime = std::chrono::milliseconds{0};
        s.mqttAvailable = false;
        return s;
    }

    void setMode(ManagerMode mode) {
        config_.mode = mode;
    }

    esp_err_t writeData(uint8_t dataId, uint16_t dataValue,
                        std::optional<Frame>& response,
                        std::chrono::milliseconds timeout) {
        Frame request = Frame::buildRequest(MessageType::WriteData, dataId, dataValue);

        // Send request to boiler
        auto boilerResponse = boiler_->sendRequest(request.raw());
        if (boilerResponse == 0) {
            return ESP_ERR_INVALID_STATE; // Boiler busy
        }
        response = Frame(boilerResponse);
        return ESP_OK;
    }

    void setMessageCallback(MessageCallback callback) {
        messageCallback_ = std::move(callback);
    }

    void setMqttBridge(MqttBridge* mqtt) {
        mqttBridge_ = mqtt;
    }

private:
    static void taskEntry(void* arg) {
        auto* self = static_cast<Impl*>(arg);
        self->taskFunction();
        vTaskDelete(nullptr);
    }

    void taskFunction() {
        ESP_LOGI(TAG, "Main loop task started");
        uint32_t loopCount = 0;
        uint32_t validFrames = 0;
        uint32_t invalidFrames = 0;

        // Rewrite the loop into a task waiting for notifications from the OpenTherm instances
        // Rewrite OpenTherm to use notifications, and try to use RMT instead of interrupts
        while (running_.load()) {
            auto thermostatRequest = thermostat_->waitForFrame(100);

            if (thermostatRequest.status == OpenThermResponseStatus::TIMEOUT) {
                vTaskDelay(pdMS_TO_TICKS(1));
                continue;
            }

            if (thermostatRequest.status == OpenThermResponseStatus::INVALID) {
                invalidFrames++;
                logMessage("DISCARDED_REQUEST", MessageSource::ThermostatBoiler, thermostatRequest.frame);
                vTaskDelay(pdMS_TO_TICKS(1));
                continue;
            }

            logMessage("REQUEST", MessageSource::ThermostatBoiler, thermostatRequest.frame);

            if (thermostatRequest.status != OpenThermResponseStatus::SUCCESS) {
                invalidFrames++;
                ESP_LOGW(TAG, "Thermostat frame invalid: 0x%08lX", thermostatRequest.frame);
                vTaskDelay(pdMS_TO_TICKS(1));
                continue;
            }

            int64_t t0 = esp_timer_get_time();

            auto boilerRequestStatus = boiler_->sendFrame(thermostatRequest.frame);
            if (boilerRequestStatus != OpenThermResponseStatus::SUCCESS) {
                invalidFrames++;
                ESP_LOGW(TAG, "Couldn't send frame 0x%08lX to boiler, got status %s", thermostatRequest.frame.raw(), OpenTherm::statusToString(boilerRequestStatus));
                vTaskDelay(pdMS_TO_TICKS(1));
                continue;
            }

            auto boilerResponse = boiler_->waitForFrame(250);
            if (boilerResponse.status != OpenThermResponseStatus::SUCCESS) {
                invalidFrames++;
                ESP_LOGW(TAG, "Couldn't get response from boiler, got status %s", OpenTherm::statusToString(boilerResponse.status));
                vTaskDelay(pdMS_TO_TICKS(1));
                continue;
            }

            int64_t t1 = esp_timer_get_time();

            ESP_LOGD(TAG, "Boiler response: 0x%08lX (took %lld ms)", boilerResponse, (t1 - t0) / 1000);

            logMessage("RESPONSE", MessageSource::ThermostatBoiler, boilerResponse.frame);

            auto thermostatResponseStatus = thermostat_->sendFrame(boilerResponse.frame);
            int64_t t2 = esp_timer_get_time();
            ESP_LOGI(TAG, "Response sent to thermostat: %s (took %lld ms total)", thermostatResponseStatus == OpenThermResponseStatus::SUCCESS ? "OK" : "FAILED", (t2 - t0) / 1000);

            parseDiagnosticResponse(boilerResponse.frame.dataId(), boilerResponse.frame);

            // Periodic status logging
            loopCount++;
            if (loopCount % 3000 == 0) {
                ESP_LOGI(TAG, "Heartbeat: valid=%lu invalid=%lu gpio=%d",
                         (unsigned long)validFrames,
                         (unsigned long)invalidFrames,
                         gpio_get_level(config_.thermostatInPin));
            }

            // Small delay to prevent busy looping
            vTaskDelay(pdMS_TO_TICKS(1));
        }

        ESP_LOGI(TAG, "Main loop task stopped");
    }


    void logMessage(std::string_view direction, MessageSource source, Frame message) {
        if (messageCallback_) {
            messageCallback_(direction, source, message);
        }
    }

    void parseDiagnosticResponse(uint8_t dataId, Frame response) {
        float floatVal;
        uint16_t uint16Val;
        uint8_t uint8Val;

        switch (dataId) {
            case 0:
                // Status message - extract slave status flags (low byte)
                {
                    uint8_t slaveStatus = response.lowByte();
                    // Bit 1: CH mode
                    bool chActive = (slaveStatus & 0x02) != 0;
                    diagnostics_.chMode.update(chActive ? 1.0f : 0.0f);
                    publishBinaryDiag("ch_mode", "CH Mode", chActive);
                    
                    // Bit 2: DHW mode
                    bool dhwActive = (slaveStatus & 0x04) != 0;
                    diagnostics_.dhwMode.update(dhwActive ? 1.0f : 0.0f);
                    publishBinaryDiag("dhw_mode", "DHW Mode", dhwActive);
                    
                    // Bit 3: Flame indicator
                    bool flame = (slaveStatus & 0x08) != 0;
                    diagnostics_.flameOn.update(flame ? 1.0f : 0.0f);
                    publishBinaryDiag("flame", "Flame Status", flame);
                }
                break;
            case 25:
                floatVal = response.asFloat();
                diagnostics_.tBoiler.update(floatVal);
                publishDiag("tboiler", "Boiler Temperature", "C", diagnostics_.tBoiler);
                break;
            case 57:
                floatVal = response.asFloat();
                diagnostics_.maxChWaterTemp.update(floatVal);
                publishDiag("maxchwatertemp", "Max CH Water Temperature", "C", diagnostics_.maxChWaterTemp);
                break;
            case 28:
                floatVal = response.asFloat();
                diagnostics_.tReturn.update(floatVal);
                publishDiag("treturn", "Return Temperature", "C", diagnostics_.tReturn);
                break;
            case 26:
                floatVal = response.asFloat();
                if (floatVal > 0) diagnostics_.tDhw.update(floatVal);
                break;
            case 32:
                floatVal = response.asFloat();
                if (floatVal > 0) diagnostics_.tDhw2.update(floatVal);
                break;
            case 27:
                floatVal = response.asFloat();
                diagnostics_.tOutside.update(floatVal);
                break;
            case 33:
                floatVal = static_cast<float>(static_cast<int16_t>(response.dataValue()));
                if (floatVal > -40 && floatVal < 500) {
                    diagnostics_.tExhaust.update(floatVal);
                    publishDiag("texhaust", "Exhaust Temperature", "C", diagnostics_.tExhaust);
                }
                break;
            case 34:
                floatVal = static_cast<float>(static_cast<int16_t>(response.dataValue()));
                if (floatVal > 0) diagnostics_.tHeatExchanger.update(floatVal);
                break;
            case 31:
                floatVal = response.asFloat();
                if (floatVal > 0) diagnostics_.tFlowCh2.update(floatVal);
                break;
            case 29:
                floatVal = response.asFloat();
                if (floatVal > 0) diagnostics_.tStorage.update(floatVal);
                break;
            case 30:
                floatVal = response.asFloat();
                if (floatVal > 0) diagnostics_.tCollector.update(floatVal);
                break;
            case 1:
                floatVal = response.asFloat();
                if (floatVal > 0 && floatVal < 100) {
                    diagnostics_.tSetpoint.update(floatVal);
                    publishDiag("tset", "Boiler Setpoint", "C", diagnostics_.tSetpoint);
                }
                break;
            case 17:
                floatVal = response.asFloat();
                if (floatVal >= 0 && floatVal <= 100) {
                    diagnostics_.modulationLevel.update(floatVal);
                    publishDiag("modulation", "Modulation Level", "%", diagnostics_.modulationLevel);
                }
                break;
            case 18:
                floatVal = response.asFloat();
                if (floatVal >= 0) {
                    diagnostics_.pressure.update(floatVal);
                    publishDiag("pressure", "CH Pressure", "bar", diagnostics_.pressure);
                }
                break;
            case 19:
                floatVal = response.asFloat();
                if (floatVal >= 0) diagnostics_.flowRate.update(floatVal);
                break;
            case 5:
                uint8Val = response.lowByte();
                diagnostics_.faultCode.update(static_cast<float>(uint8Val));
                publishDiag("fault", "Fault Code", "", diagnostics_.faultCode);
                break;
            case 115:
                uint16Val = response.dataValue();
                diagnostics_.diagCode.update(static_cast<float>(uint16Val));
                break;
            case 116:
                uint16Val = response.dataValue();
                diagnostics_.burnerStarts.update(static_cast<float>(uint16Val));
                break;
            case 119:
                uint16Val = response.dataValue();
                diagnostics_.dhwBurnerStarts.update(static_cast<float>(uint16Val));
                break;
            case 117:
                uint16Val = response.dataValue();
                diagnostics_.chPumpStarts.update(static_cast<float>(uint16Val));
                break;
            case 118:
                uint16Val = response.dataValue();
                diagnostics_.dhwPumpStarts.update(static_cast<float>(uint16Val));
                break;
            case 120:
                uint16Val = response.dataValue();
                diagnostics_.burnerHours.update(static_cast<float>(uint16Val));
                break;
            case 123:
                uint16Val = response.dataValue();
                diagnostics_.dhwBurnerHours.update(static_cast<float>(uint16Val));
                break;
            case 121:
                uint16Val = response.dataValue();
                diagnostics_.chPumpHours.update(static_cast<float>(uint16Val));
                break;
            case 122:
                uint16Val = response.dataValue();
                diagnostics_.dhwPumpHours.update(static_cast<float>(uint16Val));
                break;
            case 15:
                diagnostics_.maxCapacity.update(static_cast<float>(response.highByte()));
                diagnostics_.minModLevel.update(static_cast<float>(response.lowByte()));
                break;
            case 35:
                diagnostics_.fanSetpoint.update(static_cast<float>(response.highByte()));
                diagnostics_.fanCurrent.update(static_cast<float>(response.lowByte()));
                break;
            case 84:
                uint16Val = response.dataValue();
                diagnostics_.fanExhaustRpm.update(static_cast<float>(uint16Val));
                break;
            case 85:
                uint16Val = response.dataValue();
                diagnostics_.fanSupplyRpm.update(static_cast<float>(uint16Val));
                break;
            case 79:
                uint16Val = response.dataValue();
                diagnostics_.co2Exhaust.update(static_cast<float>(uint16Val));
                break;
            default:
                break;
        }
    }

    void publishDiag(const char* id, const char* name, const char* unit, const DiagnosticValue& dv) {
        if (mqttBridge_ && dv.isValid()) {
            mqttBridge_->publishSensor(id, name, unit, dv.valueOr(0.0f), true);
        }
    }

    void publishBinaryDiag(const char* id, const char* name, bool state) {
        if (mqttBridge_) {
            mqttBridge_->publishBinarySensor(id, name, state, true);
        }
    }

    ManagerConfig config_;
    TaskHandle_t taskHandle_ = nullptr;
    std::atomic<bool> running_{false};

    // OpenTherm instances for thermostat (master) and boiler (slave)
    // Will be constructed in start() method with proper pins
    std::unique_ptr<OpenTherm> thermostat_;
    std::unique_ptr<OpenTherm> boiler_;

    // Diagnostics
    Diagnostics diagnostics_;
    // Callback (for logging)
    MessageCallback messageCallback_;
    // MQTT bridge for publishing diagnostics
    MqttBridge* mqttBridge_ = nullptr;
};

// BoilerManager implementation

BoilerManager::BoilerManager(const ManagerConfig& config)
    : impl_(std::make_unique<Impl>(config))
{
    ESP_LOGI(TAG, "Initialized in %s mode, intercept rate: 1/%lu",
             toString(config.mode), static_cast<unsigned long>(config.interceptRate));
}

BoilerManager::~BoilerManager() = default;

esp_err_t BoilerManager::start() {
    return impl_->start();
}

void BoilerManager::stop() {
    impl_->stop();
}

bool BoilerManager::isRunning() const {
    return impl_->isRunning();
}

const Diagnostics& BoilerManager::diagnostics() const {
    return impl_->diagnostics();
}

void BoilerManager::setControlEnabled(bool enabled) {
    impl_->setControlEnabled(enabled);
}

ManagerStatus BoilerManager::status() const {
    return impl_->status();
}

void BoilerManager::setMode(ManagerMode mode) {
    impl_->setMode(mode);
}

esp_err_t BoilerManager::writeData(uint8_t dataId, uint16_t dataValue,
                                   std::optional<Frame>& response,
                                   std::chrono::milliseconds timeout) {
    return impl_->writeData(dataId, dataValue, response, timeout);
}

void BoilerManager::setMessageCallback(MessageCallback callback) {
    impl_->setMessageCallback(std::move(callback));
}

void BoilerManager::setMqttBridge(MqttBridge* mqtt) {
    impl_->setMqttBridge(mqtt);
}

// Helper functions

const char* toString(ManagerMode mode) {
    switch (mode) {
        case ManagerMode::Proxy:       return "PROXY";
        case ManagerMode::Passthrough: return "PASSTHROUGH";
        case ManagerMode::Control:     return "CONTROL";
        default:                       return "UNKNOWN";
    }
}

const char* toString(MessageSource source) {
    switch (source) {
        case MessageSource::ThermostatBoiler:  return "THERMOSTAT_BOILER";
        case MessageSource::GatewayBoiler:     return "GATEWAY_BOILER";
        case MessageSource::ThermostatGateway: return "THERMOSTAT_GATEWAY";
        default:                               return "UNKNOWN";
    }
}

} // namespace ot
