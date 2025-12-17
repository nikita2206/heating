/*
 * MQTT Bridge Implementation (C++)
 */

#include "mqtt_bridge.hpp"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "mqtt_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs_flash.h"
#include "sdkconfig.h"
#include <cstring>
#include <cstdlib>

static const char* TAG = "MQTT_BRIDGE";

namespace ot {

// Heartbeat timeout for availability check
static constexpr int64_t HEARTBEAT_TIMEOUT_MS = 90000;

class MqttBridge::Impl {
public:
    explicit Impl(const MqttConfig& config)
        : config_(config)
        , mutex_(xSemaphoreCreateMutex())
    {
        buildTopics();
    }

    ~Impl() {
        stop();
        if (mutex_) {
            vSemaphoreDelete(mutex_);
        }
    }

    esp_err_t start() {
        if (!config_.enable) {
            stop();
            return ESP_OK;  // Disabled is not an error
        }

        if (!mutex_) {
            return ESP_ERR_NO_MEM;
        }

        stop();  // Stop previous instance if any

        esp_mqtt_client_config_t mqttCfg = {};
        mqttCfg.broker.address.uri = config_.brokerUri.c_str();
        mqttCfg.broker.address.port = 0;  // Use URI port
        mqttCfg.credentials.username = config_.username.empty() ? nullptr : config_.username.c_str();
        mqttCfg.credentials.authentication.password = config_.password.empty() ? nullptr : config_.password.c_str();
        mqttCfg.credentials.client_id = config_.clientId.empty() ? nullptr : config_.clientId.c_str();
        mqttCfg.session.keepalive = 30;

        client_ = esp_mqtt_client_init(&mqttCfg);
        if (!client_) {
            ESP_LOGE(TAG, "Failed to init MQTT client");
            return ESP_FAIL;
        }

        ESP_ERROR_CHECK(esp_mqtt_client_register_event(
            client_, static_cast<esp_mqtt_event_id_t>(ESP_EVENT_ANY_ID), &Impl::eventHandler, this));

        esp_err_t err = esp_mqtt_client_start(client_);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to start MQTT client: %s", esp_err_to_name(err));
            return err;
        }

        running_ = true;
        ESP_LOGI(TAG, "MQTT bridge started (broker=%s, base=%s)",
                 config_.brokerUri.c_str(), config_.baseTopic.c_str());
        return ESP_OK;
    }

    void stop() {
        running_ = false;
        if (client_) {
            esp_mqtt_client_stop(client_);
            esp_mqtt_client_destroy(client_);
            client_ = nullptr;
        }
        setConnected(false);
    }

    bool isRunning() const { return running_; }

    esp_err_t reconfigure(const MqttConfig& config) {
        stop();
        config_ = config;
        buildTopics();
        return start();
    }

    MqttState state() const {
        MqttState result;
        int64_t nowMs = esp_timer_get_time() / 1000;

        if (mutex_ && xSemaphoreTake(mutex_, pdMS_TO_TICKS(50)) == pdTRUE) {
            result.connected = state_.connected;
            result.lastTsetC = state_.lastTsetC;
            result.lastChEnable = state_.lastChEnable;
            result.lastControlEnabled = state_.lastControlEnabled;
            result.lastUpdateTime = state_.lastUpdateTime;
            result.lastHeartbeatTime = state_.lastHeartbeatTime;
            result.heartbeatValue = state_.heartbeatValue;

            // Calculate availability
            bool hbFresh = state_.heartbeatValue.has_value() &&
                          state_.lastHeartbeatTime.count() > 0 &&
                          (nowMs - state_.lastHeartbeatTime.count()) <= HEARTBEAT_TIMEOUT_MS;
            result.available = state_.connected && hbFresh;

            xSemaphoreGive(mutex_);
        }
        return result;
    }

    esp_err_t publishSensor(std::string_view id, std::string_view name,
                            std::string_view unit, float value, bool valid) {
        if (!client_ || !state_.connected) {
            return ESP_ERR_INVALID_STATE;
        }

        // Publish discovery
        publishSensorDiscovery(id, name, unit);

        // Build state topic
        std::string topic = config_.baseTopic + "/diag/" + std::string(id) + "/state";

        if (!valid) {
            return publishState(topic, "");  // Clear value
        }

        char buf[32];
        snprintf(buf, sizeof(buf), "%.2f", value);
        return publishState(topic, buf);
    }

    void setControlCallback(ControlModeCallback callback) {
        controlCallback_ = std::move(callback);
    }

    void publishControlState(bool enabled) {
        if (!client_ || !state_.connected) {
            return;
        }

        // Update internal state
        if (mutex_ && xSemaphoreTake(mutex_, pdMS_TO_TICKS(50)) == pdTRUE) {
            state_.lastControlEnabled = enabled;
            xSemaphoreGive(mutex_);
        }

        publishState(topicControlState_, enabled ? "ON" : "OFF");
    }

private:
    void buildTopics() {
        const std::string& base = config_.baseTopic.empty() ? "ot_gateway" : config_.baseTopic;

        topicTsetCmd_ = base + "/tset/set";
        topicTsetState_ = base + "/tset/state";
        topicChEnableCmd_ = base + "/ch_enable/set";
        topicChEnableState_ = base + "/ch_enable/state";
        topicHbCmd_ = base + "/heartbeat/set";
        topicHbState_ = base + "/heartbeat/state";
        topicControlCmd_ = base + "/control/set";
        topicControlState_ = base + "/control/state";
    }

    void setConnected(bool connected) {
        if (mutex_ && xSemaphoreTake(mutex_, pdMS_TO_TICKS(50)) == pdTRUE) {
            state_.connected = connected;
            xSemaphoreGive(mutex_);
        }
    }

    void setTset(float value) {
        if (mutex_ && xSemaphoreTake(mutex_, pdMS_TO_TICKS(50)) == pdTRUE) {
            state_.lastTsetC = value;
            state_.lastUpdateTime = std::chrono::milliseconds(esp_timer_get_time() / 1000);
            xSemaphoreGive(mutex_);
        }
    }

    void setChEnable(bool enabled) {
        if (mutex_ && xSemaphoreTake(mutex_, pdMS_TO_TICKS(50)) == pdTRUE) {
            state_.lastChEnable = enabled;
            state_.lastUpdateTime = std::chrono::milliseconds(esp_timer_get_time() / 1000);
            xSemaphoreGive(mutex_);
        }
    }

    void setHeartbeat(float value) {
        if (mutex_ && xSemaphoreTake(mutex_, pdMS_TO_TICKS(50)) == pdTRUE) {
            state_.heartbeatValue = value;
            state_.lastHeartbeatTime = std::chrono::milliseconds(esp_timer_get_time() / 1000);
            xSemaphoreGive(mutex_);
        }
    }

    void setControl(bool enabled) {
        if (mutex_ && xSemaphoreTake(mutex_, pdMS_TO_TICKS(50)) == pdTRUE) {
            state_.lastControlEnabled = enabled;
            xSemaphoreGive(mutex_);
        }
        // Invoke callback outside mutex
        if (controlCallback_) {
            controlCallback_(enabled);
        }
    }

    esp_err_t publishState(const std::string& topic, const std::string& payload) {
        int msgId = esp_mqtt_client_publish(client_, topic.c_str(), payload.c_str(), 0, 1, 1);
        return (msgId >= 0) ? ESP_OK : ESP_FAIL;
    }

    void publishDiscovery() {
        const std::string& disc = config_.discoveryPrefix.empty() ? "homeassistant" : config_.discoveryPrefix;
        const std::string& base = config_.baseTopic.empty() ? "ot_gateway" : config_.baseTopic;

        // TSet number
        std::string topicTset = disc + "/number/" + base + "_tset/config";
        char payloadTset[512];
        snprintf(payloadTset, sizeof(payloadTset),
            R"({"name":"OT TSet","uniq_id":"%s_tset","cmd_t":"%s","stat_t":"%s",)"
            R"("unit_of_meas":"°C","min":10,"max":100,"step":0.5,"retain":true,)"
            R"("dev":{"ids":["%s"],"name":"OpenTherm Gateway","mf":"OT Gateway","mdl":"ESP32"}})",
            base.c_str(), topicTsetCmd_.c_str(), topicTsetState_.c_str(), base.c_str());
        publishState(topicTset, payloadTset);

        // CH Enable switch
        std::string topicCh = disc + "/switch/" + base + "_ch/config";
        char payloadCh[512];
        snprintf(payloadCh, sizeof(payloadCh),
            R"({"name":"OT CH Enable","uniq_id":"%s_ch_enable","cmd_t":"%s","stat_t":"%s",)"
            R"("pl_on":"ON","pl_off":"OFF","retain":true,)"
            R"("dev":{"ids":["%s"],"name":"OpenTherm Gateway","mf":"OT Gateway","mdl":"ESP32"}})",
            base.c_str(), topicChEnableCmd_.c_str(), topicChEnableState_.c_str(), base.c_str());
        publishState(topicCh, payloadCh);

        // Control Mode switch
        std::string topicControl = disc + "/switch/" + base + "_control/config";
        char payloadControl[512];
        snprintf(payloadControl, sizeof(payloadControl),
            R"({"name":"OT Control Mode","uniq_id":"%s_control","cmd_t":"%s","stat_t":"%s",)"
            R"("pl_on":"ON","pl_off":"OFF","retain":true,)"
            R"("dev":{"ids":["%s"],"name":"OpenTherm Gateway","mf":"OT Gateway","mdl":"ESP32"}})",
            base.c_str(), topicControlCmd_.c_str(), topicControlState_.c_str(), base.c_str());
        publishState(topicControl, payloadControl);

        // Heartbeat number
        std::string topicHb = disc + "/number/" + base + "_hb/config";
        char payloadHb[512];
        snprintf(payloadHb, sizeof(payloadHb),
            R"({"name":"OT Heartbeat","uniq_id":"%s_hb","cmd_t":"%s","stat_t":"%s",)"
            R"("min":0,"max":1000000,"step":1,"retain":true,)"
            R"("dev":{"ids":["%s"],"name":"OpenTherm Gateway","mf":"OT Gateway","mdl":"ESP32"}})",
            base.c_str(), topicHbCmd_.c_str(), topicHbState_.c_str(), base.c_str());
        publishState(topicHb, payloadHb);
    }

    void publishSensorDiscovery(std::string_view id, std::string_view name, std::string_view unit) {
        const std::string& disc = config_.discoveryPrefix.empty() ? "homeassistant" : config_.discoveryPrefix;
        const std::string& base = config_.baseTopic.empty() ? "ot_gateway" : config_.baseTopic;

        std::string topic = disc + "/sensor/" + base + "_" + std::string(id) + "/config";
        std::string stateTopic = base + "/diag/" + std::string(id) + "/state";

        char payload[512];
        if (!unit.empty()) {
            snprintf(payload, sizeof(payload),
                R"({"name":"%.*s","uniq_id":"%s_%.*s","stat_t":"%s","unit_of_meas":"%.*s","retain":true,)"
                R"("dev":{"ids":["%s"],"name":"OpenTherm Gateway","mf":"OT Gateway","mdl":"ESP32"}})",
                static_cast<int>(name.size()), name.data(),
                base.c_str(), static_cast<int>(id.size()), id.data(),
                stateTopic.c_str(),
                static_cast<int>(unit.size()), unit.data(),
                base.c_str());
        } else {
            snprintf(payload, sizeof(payload),
                R"({"name":"%.*s","uniq_id":"%s_%.*s","stat_t":"%s","retain":true,)"
                R"("dev":{"ids":["%s"],"name":"OpenTherm Gateway","mf":"OT Gateway","mdl":"ESP32"}})",
                static_cast<int>(name.size()), name.data(),
                base.c_str(), static_cast<int>(id.size()), id.data(),
                stateTopic.c_str(),
                base.c_str());
        }
        publishState(topic, payload);
    }

    void handleMessage(esp_mqtt_event_handle_t event) {
        if (!event || !event->topic || !event->data) {
            return;
        }

        std::string topic(event->topic, event->topic_len);
        std::string payload(event->data, event->data_len);

        if (topic == topicTsetCmd_) {
            float val = std::strtof(payload.c_str(), nullptr);
            setTset(val);
            ESP_LOGI(TAG, "Received TSet override: %.2f C", val);
            publishState(topicTsetState_, payload);
        }
        else if (topic == topicChEnableCmd_) {
            bool on = (strcasecmp(payload.c_str(), "on") == 0 ||
                      payload == "1" ||
                      strcasecmp(payload.c_str(), "true") == 0);
            setChEnable(on);
            ESP_LOGI(TAG, "Received CH enable override: %s", on ? "ON" : "OFF");
            publishState(topicChEnableState_, on ? "ON" : "OFF");
        }
        else if (topic == topicHbCmd_) {
            float hb = std::strtof(payload.c_str(), nullptr);
            setHeartbeat(hb);
            publishState(topicHbState_, payload);
        }
        else if (topic == topicControlCmd_) {
            bool on = (strcasecmp(payload.c_str(), "on") == 0 ||
                      payload == "1" ||
                      strcasecmp(payload.c_str(), "true") == 0);
            setControl(on);
            ESP_LOGI(TAG, "Received Control Mode override: %s", on ? "ON" : "OFF");
            publishState(topicControlState_, on ? "ON" : "OFF");
        }
    }

    static void eventHandler(void* handlerArgs, esp_event_base_t base,
                            int32_t eventId, void* eventData) {
        auto* self = static_cast<Impl*>(handlerArgs);
        auto* event = static_cast<esp_mqtt_event_handle_t>(eventData);

        switch (static_cast<esp_mqtt_event_id_t>(eventId)) {
            case MQTT_EVENT_CONNECTED:
                ESP_LOGI(TAG, "MQTT connected");
                self->setConnected(true);
                esp_mqtt_client_subscribe(self->client_, self->topicTsetCmd_.c_str(), 1);
                esp_mqtt_client_subscribe(self->client_, self->topicChEnableCmd_.c_str(), 1);
                esp_mqtt_client_subscribe(self->client_, self->topicHbCmd_.c_str(), 1);
                esp_mqtt_client_subscribe(self->client_, self->topicControlCmd_.c_str(), 1);
                self->publishDiscovery();
                break;

            case MQTT_EVENT_DISCONNECTED:
                ESP_LOGW(TAG, "MQTT disconnected");
                self->setConnected(false);
                break;

            case MQTT_EVENT_DATA:
                self->handleMessage(event);
                break;

            default:
                break;
        }
    }

    MqttConfig config_;
    MqttState state_;
    SemaphoreHandle_t mutex_ = nullptr;
    esp_mqtt_client_handle_t client_ = nullptr;
    bool running_ = false;
    ControlModeCallback controlCallback_;

    // Topics
    std::string topicTsetCmd_;
    std::string topicTsetState_;
    std::string topicChEnableCmd_;
    std::string topicChEnableState_;
    std::string topicHbCmd_;
    std::string topicHbState_;
    std::string topicControlCmd_;
    std::string topicControlState_;
};

// MqttBridge implementation

MqttBridge::MqttBridge(const MqttConfig& config)
    : impl_(std::make_unique<Impl>(config))
{
}

MqttBridge::~MqttBridge() = default;

esp_err_t MqttBridge::start() {
    return impl_->start();
}

void MqttBridge::stop() {
    impl_->stop();
}

bool MqttBridge::isRunning() const {
    return impl_->isRunning();
}

esp_err_t MqttBridge::reconfigure(const MqttConfig& config) {
    return impl_->reconfigure(config);
}

MqttState MqttBridge::state() const {
    return impl_->state();
}

esp_err_t MqttBridge::publishSensor(std::string_view id, std::string_view name,
                                    std::string_view unit, float value, bool valid) {
    return impl_->publishSensor(id, name, unit, value, valid);
}

void MqttBridge::setControlCallback(ControlModeCallback callback) {
    impl_->setControlCallback(std::move(callback));
}

void MqttBridge::publishControlState(bool enabled) {
    impl_->publishControlState(enabled);
}

// Static config utilities

esp_err_t MqttBridge::loadConfig(MqttConfig& config) {
    // Fill defaults from Kconfig
    config.enable = CONFIG_OT_MQTT_ENABLE;
    config.brokerUri = CONFIG_OT_MQTT_BROKER_URI;
    config.clientId = CONFIG_OT_MQTT_CLIENT_ID;
    config.username = CONFIG_OT_MQTT_USERNAME;
    config.password = CONFIG_OT_MQTT_PASSWORD;
    config.baseTopic = CONFIG_OT_MQTT_BASE_TOPIC;
    config.discoveryPrefix = "homeassistant";

    nvs_handle_t nvs;
    esp_err_t err = nvs_open("mqtt", NVS_READONLY, &nvs);
    if (err != ESP_OK) {
        return ESP_OK;  // Defaults already filled
    }

    char buf[128];
    size_t len;

    len = sizeof(buf);
    if (nvs_get_str(nvs, "broker", buf, &len) == ESP_OK) {
        config.brokerUri = buf;
    }

    len = sizeof(buf);
    if (nvs_get_str(nvs, "client_id", buf, &len) == ESP_OK) {
        config.clientId = buf;
    }

    len = sizeof(buf);
    if (nvs_get_str(nvs, "username", buf, &len) == ESP_OK) {
        config.username = buf;
    }

    len = sizeof(buf);
    if (nvs_get_str(nvs, "password", buf, &len) == ESP_OK) {
        config.password = buf;
    }

    len = sizeof(buf);
    if (nvs_get_str(nvs, "base_topic", buf, &len) == ESP_OK) {
        config.baseTopic = buf;
    }

    len = sizeof(buf);
    if (nvs_get_str(nvs, "disc_prefix", buf, &len) == ESP_OK) {
        config.discoveryPrefix = buf;
    }

    uint8_t enable = config.enable ? 1 : 0;
    if (nvs_get_u8(nvs, "enable", &enable) == ESP_OK) {
        config.enable = enable != 0;
    }

    nvs_close(nvs);
    return ESP_OK;
}

esp_err_t MqttBridge::saveConfig(const MqttConfig& config) {
    nvs_handle_t nvs;
    esp_err_t err = nvs_open("mqtt", NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_set_str(nvs, "broker", config.brokerUri.c_str());
    err |= nvs_set_str(nvs, "client_id", config.clientId.c_str());
    err |= nvs_set_str(nvs, "username", config.username.c_str());
    err |= nvs_set_str(nvs, "password", config.password.c_str());
    err |= nvs_set_str(nvs, "base_topic", config.baseTopic.c_str());
    err |= nvs_set_str(nvs, "disc_prefix", config.discoveryPrefix.c_str());
    err |= nvs_set_u8(nvs, "enable", config.enable ? 1 : 0);

    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }

    nvs_close(nvs);
    return err;
}

} // namespace ot
