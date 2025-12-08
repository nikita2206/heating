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
    bool last_tset_valid;
    float last_tset_c;
    bool last_ch_enable_valid;
    bool last_ch_enable;
    int64_t last_update_ms;
} mqtt_bridge_state_t;

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
 * Thread-safe snapshot of state.
 */
void mqtt_bridge_get_state(mqtt_bridge_state_t *out);

#ifdef __cplusplus
}
#endif


