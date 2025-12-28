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

// Diagnostic command definition
struct DiagnosticCmd {
    uint8_t dataId;
    const char* name;
};

// Diagnostic commands to poll
static constexpr DiagnosticCmd DIAG_COMMANDS[] = {
    { OT_FRAME_T_BOILER,            "Tboiler" },
    { OT_FRAME_MAX_CH_SETPOINT,     "BoilerStatus" },
    { OT_FRAME_T_RET,               "Tret" },
    { OT_FRAME_T_DHW,               "Tdhw" },
    { OT_FRAME_TSET,                "TSet" },
    { OT_FRAME_MODULATION,          "RelModLevel" },
    { OT_FRAME_CH_PRESSURE,         "CHPressure" },
    { OT_FRAME_T_OUTSIDE,           "Toutside" },
    { OT_FRAME_T_EXHAUST,           "Texhaust" },
    { OT_FRAME_T_HEAT_EXCHANGER,    "TboilerHeatExchanger" },
    { OT_FRAME_DHW_FLOW_RATE,       "DHWFlowRate" },
    { OT_FRAME_ASF_FLAGS,           "ASFflags" },
    { OT_FRAME_OEM_DIAGNOSTIC,      "OEMDiagnosticCode" },
    { OT_FRAME_MAX_CAPACITY,        "MaxCapacityMinModLevel" },
    { OT_FRAME_FAN_SPEED,           "BoilerFanSpeed" },
    { OT_FRAME_T_DHW2,              "Tdhw2" },
    { OT_FRAME_T_FLOW_CH2,          "TflowCH2" },
    { OT_FRAME_T_STORAGE,           "Tstorage" },
    { OT_FRAME_T_COLLECTOR,         "Tcollector" },
    { OT_FRAME_CO2_EXHAUST,         "CO2exhaust" },
    { OT_FRAME_RPM_EXHAUST,         "RPMexhaust" },
    { OT_FRAME_RPM_SUPPLY,          "RPMsupply" },
    { OT_FRAME_BURNER_STARTS,       "BurnerStarts" },
    { OT_FRAME_DHW_BURNER_STARTS,   "DHWBurnerStarts" },
    { OT_FRAME_CH_PUMP_STARTS,      "CHPumpStarts" },
    { OT_FRAME_DHW_PUMP_STARTS,     "DHWPumpStarts" },
    { OT_FRAME_BURNER_HOURS,        "BurnerHours" },
    { OT_FRAME_DHW_BURNER_HOURS,    "DHWBurnerHours" },
    { OT_FRAME_CH_PUMP_HOURS,       "CHPumpHours" },
    { OT_FRAME_DHW_PUMP_HOURS,      "DHWPumpHours" },
    { OT_FRAME_SLAVE_CONFIG,        "SlaveConfig" },
    { OT_FRAME_SLAVE_VERSION,       "SlaveVersion" },
    { OT_FRAME_SLAVE_OT_VERSION,    "SlaveOTVersion" },
    { OT_FRAME_DHW_BOUNDS,          "DhwBounds" },
    { OT_FRAME_CH_BOUNDS,           "MaxTSetBounds" },
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
        thermostat_ = std::make_unique<OpenThermDriver>(
            OpenThermDriver::Config{
                .inPin = config_.thermostatInPin,
                .outPin = config_.thermostatOutPin,
                .isSlave = true
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

    const Diagnostics& diagnostics() const { return diagnostics_; }

    const BoilerState& state() const { return boilerState_; }

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
        
        s.demandTsetC = demandTsetC_;
        s.demandChEnabled = demandChEnabled_;
        s.lastDemandTime = lastDemandTime_;
        return s;
    }

    void setMode(ManagerMode mode) {
        config_.mode = mode;
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
            auto thermostatRequest = thermostat_->receive(500);

            if (!thermostatRequest.has_value()) {
                ESP_LOGI(TAG, "No request from thermostat in 500ms time");
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

            if (!boiler_->send(thermostatRequest.value())) {
                invalidFrames++;
                ESP_LOGW(TAG, "Couldn't send frame 0x%08lX to boiler, likely the TX queue is full", thermostatRequest.value().raw());
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

            ESP_LOGD(TAG, "Boiler response: 0x%08lX (took %lld ms)", boilerResponse, (t1 - t0) / 1000);

            logMessage("RESPONSE", MessageSource::ThermostatBoiler, boilerResponse.value());
            parseDiagnosticResponse(boilerResponse.value().dataId(), boilerResponse.value());

            if (thermostat_->send(boilerResponse.value())) {
                ESP_LOGI(TAG, "Response queued to be sent to thermostat");
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
        const auto& diagCmd = DIAG_COMMANDS[currentDiagIndex_];
        currentDiagIndex_ = (currentDiagIndex_ + 1) % DIAG_COMMANDS_COUNT;
        
        // 2. Send Diagnostic Request to Boiler
        OpenThermFrame diagReq = OpenThermFrame::buildRequest(OpenThermMessageType::ReadData, diagCmd.dataId, 0);

        logMessage("REQUEST", MessageSource::GatewayBoiler, diagReq);
        if (boiler_->send(diagReq)) {
            auto diagResp = boiler_->receive(250);
            if (diagResp.has_value()) {
                logMessage("RESPONSE", MessageSource::GatewayBoiler, diagResp.value());
                parseDiagnosticResponse(diagResp.value().dataId(), diagResp.value());
            } else {
                 ESP_LOGW(TAG, "Diagnostic query timeout for ID %d", diagCmd.dataId);
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
                // Bit 0 of high byte is CH Enable
                bool chEnabled = (thermostatFrame.highByte() & 0x01) != 0;
                demandChEnabled_ = chEnabled;
                lastDemandTime_ = std::chrono::milliseconds(esp_timer_get_time() / 1000);
            } else if (id == OT_FRAME_TSET) { // TSet
                demandTsetC_ = thermostatFrame.asFloat();
                lastDemandTime_ = std::chrono::milliseconds(esp_timer_get_time() / 1000);
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
                diagnostics_.tBoiler.update(floatVal);
                boilerState_.tBoiler.update(floatVal);
                publishDiag("tboiler", "Boiler Temperature", "C", diagnostics_.tBoiler);
                break;
            case OT_FRAME_MAX_CH_SETPOINT:
                floatVal = response.asFloat();
                diagnostics_.maxChWaterTemp.update(floatVal);
                boilerState_.maxChWaterTemp.update(floatVal);
                publishDiag("maxchwatertemp", "Max CH Water Temperature", "C", diagnostics_.maxChWaterTemp);
                break;
            case OT_FRAME_T_RET:
                floatVal = response.asFloat();
                diagnostics_.tReturn.update(floatVal);
                boilerState_.tReturn.update(floatVal);
                publishDiag("treturn", "Return Temperature", "C", diagnostics_.tReturn);
                break;
            case OT_FRAME_T_DHW:
                floatVal = response.asFloat();
                if (floatVal > 0) {
                    diagnostics_.tDhw.update(floatVal);
                    boilerState_.tDhw.update(floatVal);
                }
                break;
            case OT_FRAME_T_DHW2:
                floatVal = response.asFloat();
                if (floatVal > 0) {
                    diagnostics_.tDhw2.update(floatVal);
                    boilerState_.tDhw2.update(floatVal);
                }
                break;
            case OT_FRAME_T_OUTSIDE:
                floatVal = response.asFloat();
                diagnostics_.tOutside.update(floatVal);
                boilerState_.tOutside.update(floatVal);
                break;
            case OT_FRAME_T_EXHAUST:
                floatVal = static_cast<float>(static_cast<int16_t>(response.dataValue()));
                if (floatVal > -40 && floatVal < 500) {
                    diagnostics_.tExhaust.update(floatVal);
                    boilerState_.tExhaust.update(floatVal);
                    publishDiag("texhaust", "Exhaust Temperature", "C", diagnostics_.tExhaust);
                }
                break;
            case OT_FRAME_T_HEAT_EXCHANGER:
                floatVal = static_cast<float>(static_cast<int16_t>(response.dataValue()));
                if (floatVal > 0) {
                    diagnostics_.tHeatExchanger.update(floatVal);
                    boilerState_.tHeatExchanger.update(floatVal);
                }
                break;
            case OT_FRAME_T_FLOW_CH2:
                floatVal = response.asFloat();
                if (floatVal > 0) {
                    diagnostics_.tFlowCh2.update(floatVal);
                    boilerState_.tFlowCh2.update(floatVal);
                }
                break;
            case OT_FRAME_T_STORAGE:
                floatVal = response.asFloat();
                if (floatVal > 0) {
                    diagnostics_.tStorage.update(floatVal);
                    boilerState_.tStorage.update(floatVal);
                }
                break;
            case OT_FRAME_T_COLLECTOR:
                floatVal = response.asFloat();
                if (floatVal > 0) {
                    diagnostics_.tCollector.update(floatVal);
                    boilerState_.tCollector.update(floatVal);
                }
                break;
            case OT_FRAME_TSET:
                floatVal = response.asFloat();
                if (floatVal > 0 && floatVal < 100) {
                    diagnostics_.tSetpoint.update(floatVal);
                    boilerState_.tSetpoint.update(floatVal);
                    publishDiag("tset", "Boiler Setpoint", "C", diagnostics_.tSetpoint);
                }
                break;
            case OT_FRAME_MODULATION:
                floatVal = response.asFloat();
                if (floatVal >= 0 && floatVal <= 100) {
                    diagnostics_.modulationLevel.update(floatVal);
                    boilerState_.modulationLevel.update(floatVal);
                    publishDiag("modulation", "Modulation Level", "%", diagnostics_.modulationLevel);
                }
                break;
            case OT_FRAME_CH_PRESSURE:
                floatVal = response.asFloat();
                if (floatVal >= 0) {
                    diagnostics_.pressure.update(floatVal);
                    boilerState_.pressure.update(floatVal);
                    publishDiag("pressure", "CH Pressure", "bar", diagnostics_.pressure);
                }
                break;
            case OT_FRAME_DHW_FLOW_RATE:
                floatVal = response.asFloat();
                if (floatVal >= 0) {
                    diagnostics_.flowRate.update(floatVal);
                    boilerState_.flowRate.update(floatVal);
                }
                break;
            case OT_FRAME_ASF_FLAGS:
                uint8Val = response.lowByte();
                diagnostics_.faultCode.update(static_cast<float>(uint8Val));
                boilerState_.faultCode.update(static_cast<uint16_t>(uint8Val));
                publishDiag("fault", "Fault Code", "", diagnostics_.faultCode);
                break;
            case OT_FRAME_OEM_DIAGNOSTIC:
                uint16Val = response.dataValue();
                diagnostics_.diagCode.update(static_cast<float>(uint16Val));
                boilerState_.diagCode.update(uint16Val);
                break;
            case OT_FRAME_BURNER_STARTS:
                uint16Val = response.dataValue();
                diagnostics_.burnerStarts.update(static_cast<float>(uint16Val));
                boilerState_.burnerStarts.update(uint16Val);
                break;
            case OT_FRAME_DHW_BURNER_STARTS:
                uint16Val = response.dataValue();
                diagnostics_.dhwBurnerStarts.update(static_cast<float>(uint16Val));
                boilerState_.dhwBurnerStarts.update(uint16Val);
                break;
            case OT_FRAME_CH_PUMP_STARTS:
                uint16Val = response.dataValue();
                diagnostics_.chPumpStarts.update(static_cast<float>(uint16Val));
                boilerState_.chPumpStarts.update(uint16Val);
                break;
            case OT_FRAME_DHW_PUMP_STARTS:
                uint16Val = response.dataValue();
                diagnostics_.dhwPumpStarts.update(static_cast<float>(uint16Val));
                boilerState_.dhwPumpStarts.update(uint16Val);
                break;
            case OT_FRAME_BURNER_HOURS:
                uint16Val = response.dataValue();
                diagnostics_.burnerHours.update(static_cast<float>(uint16Val));
                boilerState_.burnerHours.update(uint16Val);
                break;
            case OT_FRAME_DHW_BURNER_HOURS:
                uint16Val = response.dataValue();
                diagnostics_.dhwBurnerHours.update(static_cast<float>(uint16Val));
                boilerState_.dhwBurnerHours.update(uint16Val);
                break;
            case OT_FRAME_CH_PUMP_HOURS:
                uint16Val = response.dataValue();
                diagnostics_.chPumpHours.update(static_cast<float>(uint16Val));
                boilerState_.chPumpHours.update(uint16Val);
                break;
            case OT_FRAME_DHW_PUMP_HOURS:
                uint16Val = response.dataValue();
                diagnostics_.dhwPumpHours.update(static_cast<float>(uint16Val));
                boilerState_.dhwPumpHours.update(uint16Val);
                break;
            case OT_FRAME_MAX_CAPACITY:
                diagnostics_.maxCapacity.update(static_cast<float>(response.highByte()));
                diagnostics_.minModLevel.update(static_cast<float>(response.lowByte()));
                
                boilerState_.maxCapacity.update(static_cast<uint16_t>(response.highByte()));
                boilerState_.minModLevel.update(static_cast<uint16_t>(response.lowByte()));
                break;
            case OT_FRAME_FAN_SPEED:
                diagnostics_.fanSetpoint.update(static_cast<float>(response.highByte()));
                diagnostics_.fanCurrent.update(static_cast<float>(response.lowByte()));
                
                boilerState_.fanSetpoint.update(static_cast<uint16_t>(response.highByte()));
                boilerState_.fanCurrent.update(static_cast<uint16_t>(response.lowByte()));
                break;
            case OT_FRAME_RPM_EXHAUST:
                uint16Val = response.dataValue();
                diagnostics_.fanExhaustRpm.update(static_cast<float>(uint16Val));
                boilerState_.fanExhaustRpm.update(uint16Val);
                break;
            case OT_FRAME_RPM_SUPPLY:
                uint16Val = response.dataValue();
                diagnostics_.fanSupplyRpm.update(static_cast<float>(uint16Val));
                boilerState_.fanSupplyRpm.update(uint16Val);
                break;
            case OT_FRAME_CO2_EXHAUST:
                uint16Val = response.dataValue();
                diagnostics_.co2Exhaust.update(static_cast<float>(uint16Val));
                boilerState_.co2Exhaust.update(uint16Val);
                break;
            case OT_FRAME_SLAVE_CONFIG:
                diagnostics_.slaveMemberId.update(static_cast<float>(response.lowByte()));
                diagnostics_.slaveConfigFlags.update(static_cast<float>(response.highByte()));
                
                boilerState_.slaveMemberId.update(static_cast<uint16_t>(response.lowByte()));
                boilerState_.slaveConfigFlags.update(static_cast<uint16_t>(response.highByte()));
                break;
            case OT_FRAME_SLAVE_VERSION:
                diagnostics_.slaveVersion.update(static_cast<float>(response.lowByte()));
                diagnostics_.slaveType.update(static_cast<float>(response.highByte()));
                
                boilerState_.slaveVersion.update(static_cast<uint16_t>(response.lowByte()));
                boilerState_.slaveType.update(static_cast<uint16_t>(response.highByte()));
                break;
            case OT_FRAME_SLAVE_OT_VERSION:
                floatVal = response.asFloat();
                diagnostics_.slaveOTVersion.update(floatVal);
                boilerState_.slaveOTVersion.update(floatVal);
                break;
            case OT_FRAME_DHW_BOUNDS:
                diagnostics_.dhwSetLB.update(static_cast<float>(static_cast<int8_t>(response.lowByte())));
                diagnostics_.dhwSetUB.update(static_cast<float>(static_cast<int8_t>(response.highByte())));
                
                boilerState_.dhwSetLB.update(static_cast<uint16_t>(response.lowByte()));
                boilerState_.dhwSetUB.update(static_cast<uint16_t>(response.highByte()));
                break;
            case OT_FRAME_CH_BOUNDS:
                diagnostics_.maxTSetLB.update(static_cast<float>(static_cast<int8_t>(response.lowByte())));
                diagnostics_.maxTSetUB.update(static_cast<float>(static_cast<int8_t>(response.highByte())));
                
                boilerState_.maxTSetLB.update(static_cast<uint16_t>(response.lowByte()));
                boilerState_.maxTSetUB.update(static_cast<uint16_t>(response.highByte()));
                break;
            case OT_FRAME_CUSTOM_200:
                uint16Val = response.dataValue();
                diagnostics_.custom200.update(static_cast<float>(uint16Val));
                boilerState_.custom200.update(uint16Val);
                break;
            case OT_FRAME_CUSTOM_202:
                uint16Val = response.dataValue();
                diagnostics_.custom202.update(static_cast<float>(uint16Val));
                boilerState_.custom202.update(uint16Val);
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
    std::unique_ptr<OpenThermDriver> thermostat_;
    std::unique_ptr<OpenThermDriver> boiler_;

    // Diagnostics
    Diagnostics diagnostics_;
    BoilerState boilerState_;
    // Callback (for logging)
    MessageCallback messageCallback_;
    // MQTT bridge for publishing diagnostics
    MqttBridge* mqttBridge_ = nullptr;
    size_t currentDiagIndex_ = 0;

    // Control state
    float demandTsetC_ = 0.0f;
    bool demandChEnabled_ = false;
    std::chrono::milliseconds lastDemandTime_{0};
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

const BoilerState& BoilerManager::state() const {
    return impl_->state();
}

ManagerStatus BoilerManager::status() const {
    return impl_->status();
}

void BoilerManager::setMode(ManagerMode mode) {
    impl_->setMode(mode);
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
