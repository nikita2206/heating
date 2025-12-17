/*
 * Boiler Manager Implementation (C++)
 */

#include "boiler_manager.hpp"
#include "mqtt_bridge.hpp"
#include "OpenTherm.h"
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

// Helper to convert float to f8.8 format
static uint16_t floatToF88(float val) {
    if (val < 0) val = 0;
    if (val > 250.0f) val = 250.0f;
    return static_cast<uint16_t>(val * 256.0f);
}

// Helper to build status word
static uint16_t buildStatusWord(bool chOn) {
    uint16_t status = 0;
    if (chOn) {
        status |= (1 << 0);  // CH enable
        status |= (1 << 1);  // DHW enable
    }
    return status;
}

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
            config_.thermostatInPin, config_.thermostatOutPin, true,
            config_.thermostatInvertOutput);
        boiler_ = std::make_unique<OpenTherm>(
            config_.boilerInPin, config_.boilerOutPin, false,
            config_.boilerInvertOutput);

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
        ESP_LOGI(TAG, "Thermostat: invertOut=%d invertIn=%d, Boiler: invertOut=%d invertIn=%d",
                 config_.thermostatInvertOutput, config_.thermostatInvertInput,
                 config_.boilerInvertOutput, config_.boilerInvertInput);
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

private:
    static void taskEntry(void* arg) {
        auto* self = static_cast<Impl*>(arg);
        self->taskFunction();
        vTaskDelete(nullptr);
    }

    // void taskFunction() {
    //     ESP_LOGI(TAG, "Main loop task started");

    //     while (running_.load()) {
    //         int64_t nowMs = esp_timer_get_time() / 1000;

    //         // 1. Check for new thermostat request
    //         Frame request;
    //         if (ThermostatTask::getRequest(*queues_, request)) {
    //             ESP_LOGD(TAG, "Got thermostat request: 0x%08lX",
    //                      static_cast<unsigned long>(request.raw()));
    //             logMessage("REQUEST", MessageSource::ThermostatBoiler, request);

    //             uint8_t dataId = request.dataId();

    //             // Refresh MQTT control state
    //             if (controlEnabled_) {
    //                 refreshControlState();
    //             }

    //             // CONTROL MODE: send synthetic responses
    //             if (controlEnabled_ && controlActive_) {
    //                 auto syntheticResponse = handleControlMode(dataId);
    //                 if (syntheticResponse) {
    //                     logMessage("RESPONSE", MessageSource::ThermostatGateway, *syntheticResponse);
    //                     ThermostatTask::sendResponse(*queues_, *syntheticResponse);
    //                     continue;
    //                 }
    //             }

    //             // DIAGNOSTIC INJECTION: Intercept every Nth ID=0 request
    //             if (dataId == DataId::Status && config_.mode == ManagerMode::Proxy) {
    //                 id0FrameCounter_++;
    //                 if (id0FrameCounter_ >= config_.interceptRate) {
    //                     id0FrameCounter_ = 0;

    //                     // Send diagnostic query instead
    //                     const auto& cmd = DIAG_COMMANDS[diagIndex_];
    //                     diagIndex_ = (diagIndex_ + 1) % DIAG_COMMANDS_COUNT;

    //                     Frame diagRequest = Frame::buildRequest(MessageType::ReadData, cmd.dataId, 0);
    //                     ESP_LOGI(TAG, "Intercepting for diagnostic: %s (ID=%d)", cmd.name, cmd.dataId);
    //                     logMessage("REQUEST", MessageSource::GatewayBoiler, diagRequest);

    //                     BoilerTask::sendRequest(*queues_, diagRequest);
    //                     loopState_ = LoopState::WaitDiagResponse;
    //                     pendingRequest_ = request;
    //                     continue;
    //                 }
    //             }

    //             // NORMAL PASSTHROUGH: Forward request to boiler
    //             ESP_LOGD(TAG, "Forwarding request to boiler");
    //             BoilerTask::sendRequest(*queues_, request);
    //             loopState_ = LoopState::WaitBoilerResponse;
    //             pendingPassthrough_ = true;
    //         }

    //         // 2. Check for boiler response
    //         Frame response;
    //         if (BoilerTask::getResponse(*queues_, response)) {
    //             ESP_LOGD(TAG, "Got boiler response: 0x%08lX",
    //                      static_cast<unsigned long>(response.raw()));
    //             logMessage("RESPONSE", MessageSource::ThermostatBoiler, response);

    //             if (loopState_ == LoopState::WaitDiagResponse) {
    //                 // Diagnostic response
    //                 if (response.isValidParity()) {
    //                     parseDiagnosticResponse(response.dataId(), response);
    //                     ESP_LOGD(TAG, "Diagnostic response parsed: ID=%d", response.dataId());
    //                 }
    //                 loopState_ = LoopState::Idle;
    //             }
    //             else if (loopState_ == LoopState::WaitBoilerResponse && pendingPassthrough_) {
    //                 // Normal passthrough - forward to thermostat
    //                 ThermostatTask::sendResponse(*queues_, response);
    //                 pendingPassthrough_ = false;
    //                 loopState_ = LoopState::Idle;
    //             }
    //             else {
    //                 // Unexpected - still forward
    //                 ThermostatTask::sendResponse(*queues_, response);
    //                 loopState_ = LoopState::Idle;
    //             }
    //         }

    //         // 3. Handle manual writes
    //         if (manualWritePending_ && loopState_ == LoopState::Idle) {
    //             ESP_LOGI(TAG, "Processing manual write: 0x%08lX",
    //                      static_cast<unsigned long>(manualWriteFrame_.raw()));
    //             logMessage("REQUEST", MessageSource::GatewayBoiler, manualWriteFrame_);

    //             BoilerTask::sendRequest(*queues_, manualWriteFrame_);

    //             // Wait for response (blocking)
    //             int waitCount = 0;
    //             while (waitCount < 100) {
    //                 Frame resp;
    //                 if (BoilerTask::getResponse(*queues_, resp)) {
    //                     logMessage("RESPONSE", MessageSource::GatewayBoiler, resp);

    //                     if (resp.isValidParity()) {
    //                         if (resp.messageType() == MessageType::WriteAck) {
    //                             manualWriteResult_ = ESP_OK;
    //                         } else {
    //                             manualWriteResult_ = ESP_ERR_INVALID_RESPONSE;
    //                         }
    //                         manualWriteResponse_ = resp;
    //                     } else {
    //                         manualWriteResult_ = ESP_ERR_INVALID_CRC;
    //                     }
    //                     break;
    //                 }
    //                 vTaskDelay(pdMS_TO_TICKS(10));
    //                 waitCount++;
    //             }

    //             if (waitCount >= 100) {
    //                 manualWriteResult_ = ESP_ERR_TIMEOUT;
    //             }

    //             manualWritePending_ = false;
    //             xSemaphoreGive(manualWriteSem_);
    //         }

    //         // 4. Periodic diagnostics polling (control mode)
    //         if (controlEnabled_ && controlActive_ && loopState_ == LoopState::Idle) {
    //             if (nowMs - lastDiagPollMs_ >= 1000) {
    //                 const auto& cmd = DIAG_COMMANDS[diagIndex_];
    //                 diagIndex_ = (diagIndex_ + 1) % DIAG_COMMANDS_COUNT;

    //                 Frame diagRequest = Frame::buildRequest(MessageType::ReadData, cmd.dataId, 0);
    //                 logMessage("REQUEST", MessageSource::GatewayBoiler, diagRequest);

    //                 BoilerTask::sendRequest(*queues_, diagRequest);
    //                 loopState_ = LoopState::WaitDiagResponse;
    //                 lastDiagPollMs_ = nowMs;
    //             }
    //         }

    //         vTaskDelay(pdMS_TO_TICKS(1));
    //     }

    //     ESP_LOGI(TAG, "Main loop task stopped");
    // }


    void taskFunction() {
        ESP_LOGI(TAG, "Main loop task started");
        uint32_t loopCount = 0;
        uint32_t validFrames = 0;
        uint32_t invalidFrames = 0;

        // Rewrite the loop into a task waiting for notifications from the OpenTherm instances
        // Rewrite OpenTherm to use notifications, and try to use RMT instead of interrupts
        while (running_.load()) {
            thermostat_->process([this, &loopCount, &validFrames, &invalidFrames](unsigned long request, OpenThermResponseStatus status) {
                if (status == OpenThermResponseStatus::TIMEOUT) {
                    // Don't log timeouts - they're normal when no data
                    return;

                    validFrames++;

                    if (status != OpenThermResponseStatus::SUCCESS) {
                        Frame reqFrame(request);
                        ESP_LOGW(TAG, "Thermostat frame %s: ID=%d type=%s raw=0x%08lX",
                                OpenTherm::statusToString(status), reqFrame.dataId(),
                                toString(reqFrame.messageType()), request);
                        return;
                    }

                }

                Frame reqFrame(request);
                uint8_t dataId = reqFrame.dataId();
                auto msgType = reqFrame.messageType();

                int64_t t0 = esp_timer_get_time();

                if (loopCount % 15 == 0 || request == 0 || status == OpenThermResponseStatus::INVALID || dataId == 0) {
                    logMessage("DISCARDED_REQUEST", MessageSource::ThermostatBoiler, reqFrame);
                    request = boiler_->buildSetBoilerStatusRequest(true, true, false, false, false);
                    ESP_LOGI(TAG, "Swapping boiler request to Status frame: 0x%08lX", request);
                    logMessage("REQUEST", MessageSource::GatewayBoiler, Frame(request));
                } else {
                    Frame reqFrame(request);
                    ESP_LOGI(TAG, "Forwarding ID=%d request 0x%08lX to boiler", reqFrame.dataId(), request);
                    logMessage("REQUEST", MessageSource::ThermostatBoiler, reqFrame);
                }

                auto boilerResponse = boiler_->sendRequest(request);
                int64_t t1 = esp_timer_get_time();

                if (!boilerResponse) {
                    ESP_LOGW(TAG, "Failed to send request to boiler (took %lld ms)", (t1 - t0) / 1000);
                    return;
                }

                Frame respFrame(boilerResponse);

                ESP_LOGI(TAG, "Boiler response: 0x%08lX (took %lld ms)", boilerResponse, (t1 - t0) / 1000);

                logMessage("RESPONSE", MessageSource::ThermostatBoiler, respFrame);
                bool sent = thermostat_->sendResponse(boilerResponse);
                int64_t t2 = esp_timer_get_time();
                ESP_LOGI(TAG, "Response sent to thermostat: %s (took %lld ms total)", sent ? "OK" : "FAILED", (t2 - t0) / 1000);

                parseDiagnosticResponse(respFrame.dataId(), respFrame);
            });

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
            case 25:
                floatVal = response.asFloat();
                diagnostics_.tBoiler.update(floatVal);
                publishDiag("tboiler", "Boiler Temperature", "C", diagnostics_.tBoiler);
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
        // Note: In a full refactor, MqttBridge would be injected
        // For now, we leave this as a stub since the global mqtt_bridge
        // singleton pattern from C doesn't translate directly
        (void)id;
        (void)name;
        (void)unit;
        (void)dv;
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
