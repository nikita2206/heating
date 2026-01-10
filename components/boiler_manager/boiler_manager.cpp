/*
 * Boiler Manager Implementation (C++)
 */

#include "boiler_manager.hpp"
#include "mqtt_bridge.hpp"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "opentherm_drv.h"
#include <atomic>
#include <cstring>

static const char* TAG = "BoilerMgr";

namespace ot {

// Diagnostic commands to poll
static constexpr uint8_t DIAG_COMMANDS[] = {
    OT_FRAME_T_BOILER,
    OT_FRAME_MAX_CH_SETPOINT,
    OT_FRAME_T_RET,
    OT_FRAME_T_DHW,
    OT_FRAME_TSET,
    OT_FRAME_MODULATION,
    OT_FRAME_CH_PRESSURE,
    OT_FRAME_T_OUTSIDE,
    OT_FRAME_T_EXHAUST,
    OT_FRAME_T_HEAT_EXCHANGER,
    OT_FRAME_DHW_FLOW_RATE,
    OT_FRAME_ASF_FLAGS,
    OT_FRAME_OEM_DIAGNOSTIC,
    OT_FRAME_MAX_CAPACITY,
    OT_FRAME_FAN_SPEED,
    OT_FRAME_T_DHW2,
    OT_FRAME_T_FLOW_CH2,
    OT_FRAME_T_STORAGE,
    OT_FRAME_T_COLLECTOR,
    OT_FRAME_CO2_EXHAUST,
    OT_FRAME_RPM_EXHAUST,
    OT_FRAME_RPM_SUPPLY,
    OT_FRAME_BURNER_STARTS,
    OT_FRAME_DHW_BURNER_STARTS,
    OT_FRAME_CH_PUMP_STARTS,
    OT_FRAME_DHW_PUMP_STARTS,
    OT_FRAME_BURNER_HOURS,
    OT_FRAME_DHW_BURNER_HOURS,
    OT_FRAME_CH_PUMP_HOURS,
    OT_FRAME_DHW_PUMP_HOURS,
    OT_FRAME_SLAVE_CONFIG,
    OT_FRAME_SLAVE_VERSION,
    OT_FRAME_SLAVE_OT_VERSION,
    OT_FRAME_DHW_BOUNDS,
    OT_FRAME_CH_BOUNDS,
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
        // Initialize OpenTherm instances
        // Note: minTxIntervalMs is set to 25ms for the thermostat (Slave) driver.
        // OpenTherm requires Slaves to respond between 20ms and 115ms.
        // The default 100ms would force a response at T+100ms, which is too close
        // to the 115ms limit when accounting for the round-trip delay to the real boiler.
        thermostat_ = std::make_unique<OpenThermDriver>(
            OpenThermDriver::Config{
                .inPin = config_.thermostatInPin,
                .outPin = config_.thermostatOutPin,
                .isSlave = true,
                .minTxIntervalMs = 25
            });
        boiler_ = std::make_unique<OpenThermDriver>(
            OpenThermDriver::Config{
                .inPin = config_.boilerInPin,
                .outPin = config_.boilerOutPin,
                .isSlave = false
            });

        // Initialize OpenTherm instances
        thermostat_->start();
        boiler_->start();

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
        if (thermostat_) thermostat_->stop();
        if (boiler_) boiler_->stop();
        if (taskHandle_) {
            vTaskDelay(pdMS_TO_TICKS(150));
            taskHandle_ = nullptr;
        }
    }

    bool isRunning() const { return running_.load(); }

    const BoilerState& state() const { return boilerState_; }
    const ThermostatState& thermostatState() const { return thermostatState_; }

    ManagerStatus status() const {
        ManagerStatus s;
        // Control is enabled if mode is set to Control
        s.controlEnabled = (config_.mode == ManagerMode::Control);
        
        // Check if MQTT is available (connected)
        s.mqttAvailable = (mqttBridge_ && mqttBridge_->state().available);
        
        // Fallback is active if we want control but MQTT is not available
        s.fallbackActive = s.controlEnabled && !s.mqttAvailable;
        
        // Control is active only if enabled and not in fallback
        s.controlActive = s.controlEnabled && !s.fallbackActive;
        
        s.demandTsetC = thermostatState_.tSet.asFloatOr(0.0f);
        s.demandChEnabled = thermostatState_.chEnable;
        s.lastDemandTime = thermostatState_.lastUpdate;
        return s;
    }

    void setMode(ManagerMode mode) {
        config_.mode = mode;
    }

    void setMaxSetpoint(float maxTemp) {
        config_.maxSetpoint = maxTemp;
    }

    float getMaxSetpoint() const {
        return config_.maxSetpoint;
    }

    esp_err_t writeData(uint8_t dataId, uint16_t dataValue,
                        std::optional<OpenThermFrame>& response,
                        std::chrono::milliseconds timeout) {
        OpenThermFrame request = OpenThermFrame::buildRequest(OpenThermMessageType::WriteData, dataId, dataValue);

        // Send request to boiler
        auto requestSent = boiler_->send(request);
        if (!requestSent) {
            return ESP_ERR_INVALID_STATE; // Boiler busy
        }

        auto boilerResponse = boiler_->receive(timeout.count());
        if (!boilerResponse.has_value()) {
            return ESP_ERR_TIMEOUT;
        }

        response = boilerResponse;
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

        while (running_.load()) {
            // Wait up to 2000ms for a request (standard OT interval is ~1s)
            auto thermostatRequest = thermostat_->receive(2000);

            if (!thermostatRequest.has_value()) {
                ESP_LOGI(TAG, "No request from thermostat in 2000ms time");
                continue;
            }

            logMessage("REQUEST", MessageSource::ThermostatBoiler, thermostatRequest.value());

            // Capture demand from thermostat request
            captureDemand(thermostatRequest.value());

            // Intercept logic (Proxy Mode)
            if (config_.mode == ManagerMode::Proxy &&
                validFrames > 0 &&
                (validFrames % config_.interceptRate == 0)) {

                if (processInterception(thermostatRequest.value(), validFrames)) {
                    continue; // Skip normal forwarding
                }
            }

            int64_t t0 = esp_timer_get_time();

            OpenThermFrame frameToSend = thermostatRequest.value();

            // Enforce max setpoint in Proxy mode
            if (config_.mode == ManagerMode::Proxy && frameToSend.dataId() == OT_FRAME_TSET) {
                float tset = frameToSend.asFloat();
                if (tset > config_.maxSetpoint) {
                    ESP_LOGI(TAG, "Capping TSet %.2f to %.2f", tset, config_.maxSetpoint);
                    frameToSend = OpenThermFrame::buildRequest(
                        frameToSend.messageType(),
                        frameToSend.dataId(),
                        OpenThermFrame::fromFloat(config_.maxSetpoint)
                    );
                }
            }

            if (!boiler_->send(frameToSend)) {
                invalidFrames++;
                ESP_LOGW(TAG, "Couldn't send frame 0x%08lX to boiler, likely the TX queue is full", frameToSend.raw());
                continue;
            }

            auto boilerResponse = boiler_->receive(250);
            if (!boilerResponse.has_value()) {
                invalidFrames++;
                ESP_LOGW(TAG, "Couldn't get response from boiler in time 250ms");
                logMessage("RESPONSE", MessageSource::ThermostatBoiler, OpenThermFrame(0));
                continue;
            }

            int64_t t1 = esp_timer_get_time();

            ESP_LOGD(TAG, "Boiler response: 0x%08lX (took %lld ms)", boilerResponse.value().raw(), (t1 - t0) / 1000);

            parseDiagnosticResponse(boilerResponse.value().dataId(), boilerResponse.value());

            OpenThermFrame responseToSend = boilerResponse.value();

            // If we capped the TSet, we want to spoof the response to the thermostat
            // so it thinks the boiler accepted the requested high temperature.
            if (config_.mode == ManagerMode::Proxy &&
                thermostatRequest.value().dataId() == OT_FRAME_TSET &&
                boilerResponse.value().messageType() == OpenThermMessageType::WriteAck) {
                
                responseToSend = OpenThermFrame::buildResponse(
                    OpenThermMessageType::WriteAck,
                    OT_FRAME_TSET,
                    thermostatRequest.value().dataValue()
                );
            }

            logMessage("RESPONSE", MessageSource::ThermostatBoiler, responseToSend);

            if (thermostat_->send(responseToSend)) {
                validFrames++;
            } else {
                ESP_LOGW(TAG, "Couldn't send response to thermostat, likely the TX queue is full");
                continue;
            }

            // Periodic status logging
            loopCount++;
            if (loopCount % 3000 == 0) {
                ESP_LOGI(TAG, "Heartbeat: valid=%lu invalid=%lu gpio=%d",
                         (unsigned long)validFrames,
                         (unsigned long)invalidFrames,
                         gpio_get_level(config_.thermostatInPin));
            }
        }

        ESP_LOGI(TAG, "Main loop task stopped");
    }

    bool processInterception(const OpenThermFrame& request, uint32_t& validFrames) {
        if (!isInterceptable(request)) {
            return false;
        }

        // 1. Pick diagnostic command
        uint8_t dataId = DIAG_COMMANDS[currentDiagIndex_];
        currentDiagIndex_ = (currentDiagIndex_ + 1) % DIAG_COMMANDS_COUNT;
        
        // 2. Send Diagnostic Request to Boiler
        OpenThermFrame diagReq = OpenThermFrame::buildRequest(OpenThermMessageType::ReadData, dataId, 0);

        logMessage("REQUEST", MessageSource::GatewayBoiler, diagReq);
        if (boiler_->send(diagReq)) {
            auto diagResp = boiler_->receive(250);
            if (diagResp.has_value()) {
                logMessage("RESPONSE", MessageSource::GatewayBoiler, diagResp.value());
                parseDiagnosticResponse(diagResp.value().dataId(), diagResp.value());
            } else {
                 ESP_LOGW(TAG, "Diagnostic query timeout for ID %d", dataId);
            }
        }
        
        // 3. Fake response to Thermostat
        OpenThermFrame fakeResponse = prepareThermostatResponse(request);
        logMessage("RESPONSE", MessageSource::ThermostatBoiler, fakeResponse);
        if (thermostat_->send(fakeResponse)) {
            validFrames++;
        } else {
            ESP_LOGW(TAG, "Couldn't send fake response to thermostat");
        }
        
        return true;
    }

    bool isInterceptable(const OpenThermFrame& request) {
        if (request.messageType() != OpenThermMessageType::ReadData) {
            return false;
        }

        // List of interceptable IDs: 18, 202, 200, 19, 26, 17
        switch (request.dataId()) {
            case OT_FRAME_MODULATION:
            case OT_FRAME_CH_PRESSURE:
            case OT_FRAME_DHW_FLOW_RATE:
            case OT_FRAME_T_DHW:
            case OT_FRAME_CUSTOM_200:
            case OT_FRAME_CUSTOM_202:
                return true;
            default:
                return false;
        }
    }

    OpenThermFrame prepareThermostatResponse(const OpenThermFrame& request) {
        uint8_t id = request.dataId();
        const BoilerStateValue* value = nullptr;

        // Map ID to the corresponding BoilerStateValue
        switch (id) {
            case OT_FRAME_MODULATION:  value = &boilerState_.modulationLevel; break;
            case OT_FRAME_CH_PRESSURE:  value = &boilerState_.pressure;        break;
            case OT_FRAME_DHW_FLOW_RATE:  value = &boilerState_.flowRate;        break;
            case OT_FRAME_T_DHW:  value = &boilerState_.tDhw;            break;
            case OT_FRAME_CUSTOM_200: value = &boilerState_.custom200;       break;
            case OT_FRAME_CUSTOM_202: value = &boilerState_.custom202;       break;
            default: break;
        }

        if (value && value->isValid()) {
            return OpenThermFrame::buildResponse(OpenThermMessageType::ReadAck, id, value->raw());
        } else {
            return OpenThermFrame::buildResponse(OpenThermMessageType::ReadAck, 0, 0);
        }
    }

    void captureDemand(const OpenThermFrame& thermostatFrame) {
        if (thermostatFrame.messageType() == OpenThermMessageType::ReadData || 
            thermostatFrame.messageType() == OpenThermMessageType::WriteData) {
            
            uint8_t id = thermostatFrame.dataId();
            if (id == OT_FRAME_STATUS) { // Status
                uint8_t masterStatus = thermostatFrame.highByte();
                // Bit 0: CH Enable
                thermostatState_.chEnable = (masterStatus & 0x01) != 0;
                // Bit 1: DHW Enable
                thermostatState_.dhwEnable = (masterStatus & 0x02) != 0;
                // Bit 2: Cooling Enable
                thermostatState_.coolingEnable = (masterStatus & 0x04) != 0;
                // Bit 3: OTC Active
                thermostatState_.otcActive = (masterStatus & 0x08) != 0;
                // Bit 4: CH2 Enable
                thermostatState_.ch2Enable = (masterStatus & 0x10) != 0;
                
                thermostatState_.lastUpdate = std::chrono::milliseconds(esp_timer_get_time() / 1000);
            } else if (id == OT_FRAME_TSET) { // TSet
                thermostatState_.tSet.update(thermostatFrame.asFloat());
                thermostatState_.lastUpdate = std::chrono::milliseconds(esp_timer_get_time() / 1000);
            } else if (id == OT_FRAME_MAX_CH_SETPOINT) {
                thermostatState_.maxChSet.update(thermostatFrame.asFloat());
                thermostatState_.lastUpdate = std::chrono::milliseconds(esp_timer_get_time() / 1000);
            } else if (id == OT_FRAME_TR) { // Room Temp
                thermostatState_.tRoom.update(thermostatFrame.asFloat());
                thermostatState_.lastUpdate = std::chrono::milliseconds(esp_timer_get_time() / 1000);
            }
        }
    }

    void logMessage(std::string_view direction, MessageSource source, OpenThermFrame message) {
        if (messageCallback_) {
            messageCallback_(direction, source, message);
        }
    }

    void parseDiagnosticResponse(uint8_t dataId, OpenThermFrame response) {
        float floatVal;
        uint16_t uint16Val;
        uint8_t uint8Val;

        switch (dataId) {
            case OT_FRAME_STATUS:
                // Status message - extract slave status flags (low byte)
                {
                    uint8_t slaveStatus = response.lowByte();
                    // Bit 1: CH mode
                    bool chActive = (slaveStatus & 0x02) != 0;
                    publishBinaryDiag("ch_mode", "CH Mode", chActive);
                    
                    // Bit 2: DHW mode
                    bool dhwActive = (slaveStatus & 0x04) != 0;
                    publishBinaryDiag("dhw_mode", "DHW Mode", dhwActive);
                    
                    // Bit 3: Flame indicator
                    bool flame = (slaveStatus & 0x08) != 0;
                    publishBinaryDiag("flame", "Flame Status", flame);

                    // BoilerState updates
                    boilerState_.fault = (slaveStatus & 0x01) != 0;
                    boilerState_.chActive = chActive;
                    boilerState_.dhwActive = dhwActive;
                    boilerState_.flameOn = flame;
                    boilerState_.coolingActive = (slaveStatus & 0x10) != 0;
                    boilerState_.ch2Active = (slaveStatus & 0x20) != 0;
                    boilerState_.diagnosticEvent = (slaveStatus & 0x40) != 0;
                }
                break;
            case OT_FRAME_T_BOILER:
                floatVal = response.asFloat();
                boilerState_.tBoiler.update(floatVal);
                publishDiag("tboiler", "Boiler Temperature", "C", boilerState_.tBoiler);
                break;
            case OT_FRAME_MAX_CH_SETPOINT:
                floatVal = response.asFloat();
                boilerState_.maxChWaterTemp.update(floatVal);
                publishDiag("maxchwatertemp", "Max CH Water Temperature", "C", boilerState_.maxChWaterTemp);
                break;
            case OT_FRAME_T_RET:
                floatVal = response.asFloat();
                boilerState_.tReturn.update(floatVal);
                publishDiag("treturn", "Return Temperature", "C", boilerState_.tReturn);
                break;
            case OT_FRAME_T_DHW:
                floatVal = response.asFloat();
                if (floatVal > 0) {
                    boilerState_.tDhw.update(floatVal);
                }
                break;
            case OT_FRAME_T_DHW2:
                floatVal = response.asFloat();
                if (floatVal > 0) {
                    boilerState_.tDhw2.update(floatVal);
                }
                break;
            case OT_FRAME_T_OUTSIDE:
                floatVal = response.asFloat();
                boilerState_.tOutside.update(floatVal);
                break;
            case OT_FRAME_T_EXHAUST:
                floatVal = static_cast<float>(static_cast<int16_t>(response.dataValue()));
                if (floatVal > -40 && floatVal < 500) {
                    boilerState_.tExhaust.update(floatVal);
                    publishDiag("texhaust", "Exhaust Temperature", "C", boilerState_.tExhaust);
                }
                break;
            case OT_FRAME_T_HEAT_EXCHANGER:
                floatVal = static_cast<float>(static_cast<int16_t>(response.dataValue()));
                if (floatVal > 0) {
                    boilerState_.tHeatExchanger.update(floatVal);
                }
                break;
            case OT_FRAME_T_FLOW_CH2:
                floatVal = response.asFloat();
                if (floatVal > 0) {
                    boilerState_.tFlowCh2.update(floatVal);
                }
                break;
            case OT_FRAME_T_STORAGE:
                floatVal = response.asFloat();
                if (floatVal > 0) {
                    boilerState_.tStorage.update(floatVal);
                }
                break;
            case OT_FRAME_T_COLLECTOR:
                floatVal = response.asFloat();
                if (floatVal > 0) {
                    boilerState_.tCollector.update(floatVal);
                }
                break;
            case OT_FRAME_TSET:
                floatVal = response.asFloat();
                if (floatVal > 0 && floatVal < 100) {
                    boilerState_.tSetpoint.update(floatVal);
                    publishDiag("tset", "Boiler Setpoint", "C", boilerState_.tSetpoint);
                }
                break;
            case OT_FRAME_MODULATION:
                floatVal = response.asFloat();
                if (floatVal >= 0 && floatVal <= 100) {
                    boilerState_.modulationLevel.update(floatVal);
                    publishDiag("modulation", "Modulation Level", "%", boilerState_.modulationLevel);
                }
                break;
            case OT_FRAME_CH_PRESSURE:
                floatVal = response.asFloat();
                if (floatVal >= 0) {
                    boilerState_.pressure.update(floatVal);
                    publishDiag("pressure", "CH Pressure", "bar", boilerState_.pressure);
                }
                break;
            case OT_FRAME_DHW_FLOW_RATE:
                floatVal = response.asFloat();
                if (floatVal >= 0) {
                    boilerState_.flowRate.update(floatVal);
                }
                break;
            case OT_FRAME_ASF_FLAGS:
                uint8Val = response.lowByte();
                boilerState_.faultCode.update(static_cast<uint16_t>(uint8Val));
                publishDiag("fault", "Fault Code", "", boilerState_.faultCode);
                break;
            case OT_FRAME_OEM_DIAGNOSTIC:
                uint16Val = response.dataValue();
                boilerState_.diagCode.update(uint16Val);
                break;
            case OT_FRAME_BURNER_STARTS:
                uint16Val = response.dataValue();
                boilerState_.burnerStarts.update(uint16Val);
                break;
            case OT_FRAME_DHW_BURNER_STARTS:
                uint16Val = response.dataValue();
                boilerState_.dhwBurnerStarts.update(uint16Val);
                break;
            case OT_FRAME_CH_PUMP_STARTS:
                uint16Val = response.dataValue();
                boilerState_.chPumpStarts.update(uint16Val);
                break;
            case OT_FRAME_DHW_PUMP_STARTS:
                uint16Val = response.dataValue();
                boilerState_.dhwPumpStarts.update(uint16Val);
                break;
            case OT_FRAME_BURNER_HOURS:
                uint16Val = response.dataValue();
                boilerState_.burnerHours.update(uint16Val);
                break;
            case OT_FRAME_DHW_BURNER_HOURS:
                uint16Val = response.dataValue();
                boilerState_.dhwBurnerHours.update(uint16Val);
                break;
            case OT_FRAME_CH_PUMP_HOURS:
                uint16Val = response.dataValue();
                boilerState_.chPumpHours.update(uint16Val);
                break;
            case OT_FRAME_DHW_PUMP_HOURS:
                uint16Val = response.dataValue();
                boilerState_.dhwPumpHours.update(uint16Val);
                break;
            case OT_FRAME_MAX_CAPACITY:
                boilerState_.maxCapacity.update(static_cast<uint16_t>(response.highByte()));
                boilerState_.minModLevel.update(static_cast<uint16_t>(response.lowByte()));
                break;
            case OT_FRAME_FAN_SPEED:
                boilerState_.fanSetpoint.update(static_cast<uint16_t>(response.highByte()));
                boilerState_.fanCurrent.update(static_cast<uint16_t>(response.lowByte()));
                break;
            case OT_FRAME_RPM_EXHAUST:
                uint16Val = response.dataValue();
                boilerState_.fanExhaustRpm.update(uint16Val);
                break;
            case OT_FRAME_RPM_SUPPLY:
                uint16Val = response.dataValue();
                boilerState_.fanSupplyRpm.update(uint16Val);
                break;
            case OT_FRAME_CO2_EXHAUST:
                uint16Val = response.dataValue();
                boilerState_.co2Exhaust.update(uint16Val);
                break;
            case OT_FRAME_SLAVE_CONFIG:
                boilerState_.slaveMemberId.update(static_cast<uint16_t>(response.lowByte()));
                boilerState_.slaveConfigFlags.update(static_cast<uint16_t>(response.highByte()));
                break;
            case OT_FRAME_SLAVE_VERSION:
                boilerState_.slaveVersion.update(static_cast<uint16_t>(response.lowByte()));
                boilerState_.slaveType.update(static_cast<uint16_t>(response.highByte()));
                break;
            case OT_FRAME_SLAVE_OT_VERSION:
                floatVal = response.asFloat();
                boilerState_.slaveOTVersion.update(floatVal);
                break;
            case OT_FRAME_DHW_BOUNDS:
                boilerState_.dhwSetLB.update(static_cast<uint16_t>(response.lowByte()));
                boilerState_.dhwSetUB.update(static_cast<uint16_t>(response.highByte()));
                break;
            case OT_FRAME_CH_BOUNDS:
                boilerState_.maxTSetLB.update(static_cast<uint16_t>(response.lowByte()));
                boilerState_.maxTSetUB.update(static_cast<uint16_t>(response.highByte()));
                break;
            case OT_FRAME_CUSTOM_200:
                uint16Val = response.dataValue();
                boilerState_.custom200.update(uint16Val);
                break;
            case OT_FRAME_CUSTOM_202:
                uint16Val = response.dataValue();
                boilerState_.custom202.update(uint16Val);
                break;
            default:
                break;
        }
    }

    void publishDiag(const char* id, const char* name, const char* unit, const BoilerStateValue& dv) {
        if (mqttBridge_ && dv.isValid()) {
            mqttBridge_->publishSensor(id, name, unit, dv.asFloatOr(0.0f), true);
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
    std::unique_ptr<OpenThermDriver> thermostat_;
    std::unique_ptr<OpenThermDriver> boiler_;

    BoilerState boilerState_;
    ThermostatState thermostatState_;
    // Callback (for logging)
    MessageCallback messageCallback_;
    // MQTT bridge for publishing diagnostics
    MqttBridge* mqttBridge_ = nullptr;
    size_t currentDiagIndex_ = 0;
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

const BoilerState& BoilerManager::state() const {
    return impl_->state();
}

const ThermostatState& BoilerManager::thermostatState() const {
    return impl_->thermostatState();
}

ManagerStatus BoilerManager::status() const {
    return impl_->status();
}

void BoilerManager::setMode(ManagerMode mode) {
    impl_->setMode(mode);
}

void BoilerManager::setMaxSetpoint(float maxTemp) {
    impl_->setMaxSetpoint(maxTemp);
}

float BoilerManager::getMaxSetpoint() const {
    return impl_->getMaxSetpoint();
}

esp_err_t BoilerManager::writeData(uint8_t dataId, uint16_t dataValue,
                                   std::optional<OpenThermFrame>& response,
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
