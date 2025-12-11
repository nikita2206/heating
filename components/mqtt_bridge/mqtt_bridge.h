/*
 * MQTT Bridge - receives external overrides (TSet, CH enable) via MQTT
 * Note: This module only listens and records overrides; applying them to the
 * boiler is intentionally out of scope for now.
 */

#pragma once

#include "esp_err.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool enable;
    char broker_uri[128];   // e.g., mqtt://192.168.1.10
    char client_id[64];     // optional
    char username[64];      // optional
    char password[64];      // optional
    char base_topic[64];    // e.g., ot_gateway
    char discovery_prefix[64]; // e.g., homeassistant
} mqtt_bridge_config_t;

typedef struct {
    bool connected;
    bool available;            // connected and not stale
    bool last_tset_valid;
    float last_tset_c;
    bool last_ch_enable_valid;
    bool last_ch_enable;
    int64_t last_update_ms;    // last override message (tset or ch)
    int64_t last_override_ms;  // same as above for clarity
    bool heartbeat_valid;
    float heartbeat_value;
    int64_t last_heartbeat_ms;
    bool last_control_valid;
    bool last_control_enabled;
} mqtt_bridge_state_t;

/**
 * Callback for control mode changes from MQTT.
 * @param enabled true to enable control mode, false to disable
 * @param user_data User data passed during registration
 */
typedef void (*mqtt_control_mode_callback_t)(bool enabled, void *user_data);

/**
 * Initialize and start MQTT bridge with the provided config.
 * Non-blocking; spawns the MQTT client and subscribes to command topics.
 */
esp_err_t mqtt_bridge_start(const mqtt_bridge_config_t *cfg, mqtt_bridge_state_t *state);

/**
 * Stop MQTT bridge (disconnect and destroy client).
 */
void mqtt_bridge_stop(void);

/**
 * Persist config to NVS (namespace "mqtt"). Password is stored as-is.
 */
esp_err_t mqtt_bridge_save_config(const mqtt_bridge_config_t *cfg);

/**
 * Load config from NVS; if missing, fill defaults from Kconfig.
 */
esp_err_t mqtt_bridge_load_config(mqtt_bridge_config_t *out_cfg);

/**
 * Publish a diagnostic/sensor value (retained) and discovery entry.
 * @param id short id suffix (e.g., \"tboiler\")
 * @param name friendly name
 * @param unit unit string (may be NULL/empty)
 * @param value sensor value
 * @param valid if false, do not publish a value
 */
esp_err_t mqtt_bridge_publish_sensor(const char *id, const char *name, const char *unit, float value, bool valid);

/**
 * Thread-safe snapshot of state.
 */
void mqtt_bridge_get_state(mqtt_bridge_state_t *out);

/**
 * Register callback for control mode changes from MQTT.
 * @param callback Callback function (NULL to unregister)
 * @param user_data User data passed to callback
 */
void mqtt_bridge_set_control_callback(mqtt_control_mode_callback_t callback, void *user_data);

/**
 * Publish control mode state to MQTT (for syncing UI changes).
 * @param enabled Current control mode state
 */
void mqtt_bridge_publish_control_state(bool enabled);

#ifdef __cplusplus
}
#endif


