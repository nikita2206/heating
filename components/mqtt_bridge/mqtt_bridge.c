/*
 * MQTT Bridge - receives external overrides (TSet, CH enable) via MQTT
 * This module only listens and records overrides for now (no boiler writes).
 */

#include "mqtt_bridge.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "mqtt_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs_flash.h"
#include "sdkconfig.h"
#include <string.h>
#include <strings.h>

static const char *TAG = "MQTT_BRIDGE";

// Topics (built at start)
static char topic_tset_cmd[128];
static char topic_tset_state[128];
static char topic_ch_enable_cmd[128];
static char topic_ch_enable_state[128];

static mqtt_bridge_state_t s_state;
static SemaphoreHandle_t s_state_mutex;
static esp_mqtt_client_handle_t s_client;
static mqtt_bridge_config_t s_cfg;
static char topic_discovery_tset[160];
static char topic_discovery_ch[160];

static void publish_discovery(esp_mqtt_client_handle_t client);

static void state_set_connected(bool connected)
{
    if (xSemaphoreTake(s_state_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        s_state.connected = connected;
        xSemaphoreGive(s_state_mutex);
    }
}

static void state_set_tset(float tset_c)
{
    if (xSemaphoreTake(s_state_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        s_state.last_tset_valid = true;
        s_state.last_tset_c = tset_c;
        s_state.last_update_ms = esp_timer_get_time() / 1000;
        xSemaphoreGive(s_state_mutex);
    }
}

static void state_set_ch_enable(bool ch_on)
{
    if (xSemaphoreTake(s_state_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        s_state.last_ch_enable_valid = true;
        s_state.last_ch_enable = ch_on;
        s_state.last_update_ms = esp_timer_get_time() / 1000;
        xSemaphoreGive(s_state_mutex);
    }
}

static esp_err_t publish_state(esp_mqtt_client_handle_t client, const char *topic, const char *payload)
{
    int msg_id = esp_mqtt_client_publish(client, topic, payload, 0, 1, 1); // qos1, retain
    return (msg_id >= 0) ? ESP_OK : ESP_FAIL;
}

static void handle_message(esp_mqtt_event_handle_t event)
{
    if (!event || !event->topic || !event->data) {
        return;
    }

    // Ensure null-terminated copies
    char topic[128];
    size_t tlen = event->topic_len < sizeof(topic) - 1 ? event->topic_len : sizeof(topic) - 1;
    memcpy(topic, event->topic, tlen);
    topic[tlen] = 0;

    char payload[64];
    size_t plen = event->data_len < sizeof(payload) - 1 ? event->data_len : sizeof(payload) - 1;
    memcpy(payload, event->data, plen);
    payload[plen] = 0;

    if (strcmp(topic, topic_tset_cmd) == 0) {
        float val = strtof(payload, NULL);
        state_set_tset(val);
        ESP_LOGI(TAG, "Received TSet override: %.2f C", val);
        publish_state(event->client, topic_tset_state, payload);
    } else if (strcmp(topic, topic_ch_enable_cmd) == 0) {
        bool on = (strcasecmp(payload, "on") == 0 || strcmp(payload, "1") == 0 || strcasecmp(payload, "true") == 0);
        state_set_ch_enable(on);
        ESP_LOGI(TAG, "Received CH enable override: %s", on ? "ON" : "OFF");
        publish_state(event->client, topic_ch_enable_state, on ? "ON" : "OFF");
    }
}

static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    esp_mqtt_event_handle_t event = event_data;
    esp_mqtt_client_handle_t client = event->client;

    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "MQTT connected");
        state_set_connected(true);
        esp_mqtt_client_subscribe(client, topic_tset_cmd, 1);
        esp_mqtt_client_subscribe(client, topic_ch_enable_cmd, 1);
        publish_discovery(client);
        break;
    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "MQTT disconnected");
        state_set_connected(false);
        break;
    case MQTT_EVENT_DATA:
        handle_message(event);
        break;
    default:
        break;
    }
}

static void build_topic(char *buf, size_t buf_sz, const char *base, const char *suffix)
{
    snprintf(buf, buf_sz, "%s/%s", base, suffix);
}

static void publish_discovery(esp_mqtt_client_handle_t client)
{
    const char *disc = (s_cfg.discovery_prefix[0] ? s_cfg.discovery_prefix : "homeassistant");
    const char *base = (s_cfg.base_topic[0] ? s_cfg.base_topic : "ot_gateway");
    int base_len = strnlen(base, 48); // cap to keep payload within buffer

    // Number (TSet)
    snprintf(topic_discovery_tset, sizeof(topic_discovery_tset), "%s/number/%s_tset/config", disc, base);
    char payload_tset[512];
    snprintf(payload_tset, sizeof(payload_tset),
        "{"
        "\"name\":\"OT TSet\","
        "\"uniq_id\":\"%.*s_tset\","
        "\"cmd_t\":\"%s\","
        "\"stat_t\":\"%s\","
        "\"unit_of_meas\":\"°C\","
        "\"min\":10,\"max\":100,\"step\":0.5,"
        "\"retain\":true,"
        "\"dev\":{"
            "\"ids\":[\"%.*s\"],"
            "\"name\":\"OpenTherm Gateway\","
            "\"mf\":\"OT Gateway\","
            "\"mdl\":\"ESP32\""
        "}"
        "}",
        base_len, base, topic_tset_cmd, topic_tset_state, base_len, base);
    publish_state(client, topic_discovery_tset, payload_tset);

    // Switch (CH enable)
    snprintf(topic_discovery_ch, sizeof(topic_discovery_ch), "%s/switch/%s_ch/config", disc, base);
    char payload_ch[512];
    snprintf(payload_ch, sizeof(payload_ch),
        "{"
        "\"name\":\"OT CH Enable\","
        "\"uniq_id\":\"%.*s_ch_enable\","
        "\"cmd_t\":\"%s\","
        "\"stat_t\":\"%s\","
        "\"pl_on\":\"ON\",\"pl_off\":\"OFF\","
        "\"retain\":true,"
        "\"dev\":{"
            "\"ids\":[\"%.*s\"],"
            "\"name\":\"OpenTherm Gateway\","
            "\"mf\":\"OT Gateway\","
            "\"mdl\":\"ESP32\""
        "}"
        "}",
        base_len, base, topic_ch_enable_cmd, topic_ch_enable_state, base_len, base);
    publish_state(client, topic_discovery_ch, payload_ch);
}

static void fill_defaults(mqtt_bridge_config_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->enable = CONFIG_OT_MQTT_ENABLE;
    strncpy(cfg->broker_uri, CONFIG_OT_MQTT_BROKER_URI, sizeof(cfg->broker_uri) - 1);
    strncpy(cfg->client_id, CONFIG_OT_MQTT_CLIENT_ID, sizeof(cfg->client_id) - 1);
    strncpy(cfg->username, CONFIG_OT_MQTT_USERNAME, sizeof(cfg->username) - 1);
    strncpy(cfg->password, CONFIG_OT_MQTT_PASSWORD, sizeof(cfg->password) - 1);
    strncpy(cfg->base_topic, CONFIG_OT_MQTT_BASE_TOPIC, sizeof(cfg->base_topic) - 1);
    strncpy(cfg->discovery_prefix, "homeassistant", sizeof(cfg->discovery_prefix) - 1);
}

esp_err_t mqtt_bridge_load_config(mqtt_bridge_config_t *out_cfg)
{
    if (!out_cfg) {
        return ESP_ERR_INVALID_ARG;
    }
    fill_defaults(out_cfg);

    nvs_handle_t nvs;
    esp_err_t err = nvs_open("mqtt", NVS_READONLY, &nvs);
    if (err != ESP_OK) {
        return ESP_OK; // defaults already filled
    }

    uint8_t enable = out_cfg->enable ? 1 : 0;
    size_t len;
    len = sizeof(out_cfg->broker_uri);
    nvs_get_str(nvs, "broker", out_cfg->broker_uri, &len);
    len = sizeof(out_cfg->client_id);
    nvs_get_str(nvs, "client_id", out_cfg->client_id, &len);
    len = sizeof(out_cfg->username);
    nvs_get_str(nvs, "username", out_cfg->username, &len);
    len = sizeof(out_cfg->password);
    nvs_get_str(nvs, "password", out_cfg->password, &len);
    len = sizeof(out_cfg->base_topic);
    nvs_get_str(nvs, "base_topic", out_cfg->base_topic, &len);
    len = sizeof(out_cfg->discovery_prefix);
    nvs_get_str(nvs, "disc_prefix", out_cfg->discovery_prefix, &len);
    nvs_get_u8(nvs, "enable", &enable);
    out_cfg->enable = enable != 0;
    nvs_close(nvs);
    return ESP_OK;
}

esp_err_t mqtt_bridge_save_config(const mqtt_bridge_config_t *cfg)
{
    if (!cfg) return ESP_ERR_INVALID_ARG;
    nvs_handle_t nvs;
    esp_err_t err = nvs_open("mqtt", NVS_READWRITE, &nvs);
    if (err != ESP_OK) return err;
    err |= nvs_set_str(nvs, "broker", cfg->broker_uri);
    err |= nvs_set_str(nvs, "client_id", cfg->client_id);
    err |= nvs_set_str(nvs, "username", cfg->username);
    err |= nvs_set_str(nvs, "password", cfg->password);
    err |= nvs_set_str(nvs, "base_topic", cfg->base_topic);
    err |= nvs_set_str(nvs, "disc_prefix", cfg->discovery_prefix);
    uint8_t enable = cfg->enable ? 1 : 0;
    err |= nvs_set_u8(nvs, "enable", enable);
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    return err;
}

void mqtt_bridge_stop(void)
{
    if (s_client) {
        esp_mqtt_client_stop(s_client);
        esp_mqtt_client_destroy(s_client);
        s_client = NULL;
    }
    state_set_connected(false);
}

esp_err_t mqtt_bridge_start(const mqtt_bridge_config_t *cfg, mqtt_bridge_state_t *state)
{
    if (!cfg) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!cfg->enable) {
        mqtt_bridge_stop();
        return ESP_OK; // Disabled
    }

    memset(&s_state, 0, sizeof(s_state));
    if (state) {
        *state = s_state;
    }

    if (!s_state_mutex) {
        s_state_mutex = xSemaphoreCreateMutex();
    }
    if (!s_state_mutex) {
        return ESP_ERR_NO_MEM;
    }

    mqtt_bridge_stop(); // stop previous instance if any
    s_cfg = *cfg;

    // Build topics
    const char *base = (cfg->base_topic[0] != 0) ? cfg->base_topic : "ot_gateway";
    build_topic(topic_tset_cmd, sizeof(topic_tset_cmd), base, "tset/set");
    build_topic(topic_tset_state, sizeof(topic_tset_state), base, "tset/state");
    build_topic(topic_ch_enable_cmd, sizeof(topic_ch_enable_cmd), base, "ch_enable/set");
    build_topic(topic_ch_enable_state, sizeof(topic_ch_enable_state), base, "ch_enable/state");

    esp_mqtt_client_config_t mqtt_cfg = {
        .broker = {
            .address.uri = cfg->broker_uri,
            .address.port = 0, // use URI port if provided
        },
        .credentials = {
            .username = cfg->username[0] ? cfg->username : NULL,
            .authentication.password = cfg->password[0] ? cfg->password : NULL,
            .client_id = cfg->client_id[0] ? cfg->client_id : NULL,
        },
        .session = {
            .keepalive = 30,
        },
    };

    esp_mqtt_client_handle_t client = esp_mqtt_client_init(&mqtt_cfg);
    if (!client) {
        ESP_LOGE(TAG, "Failed to init MQTT client");
        return ESP_FAIL;
    }

    ESP_ERROR_CHECK(esp_mqtt_client_register_event(client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL));

    esp_err_t err = esp_mqtt_client_start(client);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start MQTT client: %s", esp_err_to_name(err));
        return err;
    }

    s_client = client;
    ESP_LOGI(TAG, "MQTT bridge started (broker=%s, base=%s)", cfg->broker_uri, base);
    return ESP_OK;
}

void mqtt_bridge_get_state(mqtt_bridge_state_t *out)
{
    if (!out || !s_state_mutex) {
        return;
    }
    if (xSemaphoreTake(s_state_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        *out = s_state;
        xSemaphoreGive(s_state_mutex);
    }
}


