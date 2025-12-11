/*
 * WebSocket Server for OpenTherm Message Logging
 */

#include "websocket_server.h"
#include "web_ui.h"
#include "web_ui_pages.h"
#include "esp_log.h"
#include "esp_http_server.h"
#include "esp_timer.h"
#include "boiler_manager.h"
#include "opentherm_api.h"
#include "mqtt_bridge.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

static const char *TAG = "WebSocket";
static boiler_manager_t *s_boiler_mgr = NULL;
static websocket_server_t *s_ws_server = NULL;  // For callback access

// Callback for MQTT control mode changes
static void mqtt_control_mode_handler(bool enabled, void *user_data)
{
    (void)user_data;
    if (!s_boiler_mgr) return;

    ESP_LOGI(TAG, "MQTT control mode change: %s", enabled ? "ON" : "OFF");
    if (enabled) {
        boiler_manager_set_mode(s_boiler_mgr, BOILER_MANAGER_MODE_CONTROL);
        boiler_manager_set_control_enabled(s_boiler_mgr, true);
    } else {
        boiler_manager_set_control_enabled(s_boiler_mgr, false);
        boiler_manager_set_mode(s_boiler_mgr, BOILER_MANAGER_MODE_PASSTHROUGH);
    }
}

// HTTP GET handler for root (dashboard)
static esp_err_t root_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    char nav[WEB_UI_NAV_MAX_LEN];
    web_ui_build_nav(nav, sizeof(nav), WEB_NAV_DASHBOARD);
    size_t page_len = 0;
    char *page = web_ui_alloc_page(0, &page_len, "OpenTherm Gateway", WEB_UI_DASHBOARD_STYLES, nav, WEB_UI_DASHBOARD_BODY);
    if (!page) {
        return httpd_resp_send_500(req);
    }
    httpd_resp_send(req, page, page_len);
    free(page);
    return ESP_OK;
}

// HTTP GET handler for logs page
static esp_err_t logs_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    char nav[WEB_UI_NAV_MAX_LEN];
    web_ui_build_nav(nav, sizeof(nav), WEB_NAV_LOGS);
    size_t page_len = 0;
    char *page = web_ui_alloc_page(0, &page_len, "Logs - OpenTherm Gateway", WEB_UI_LOGS_STYLES, nav, WEB_UI_LOGS_BODY);
    if (!page) {
        return httpd_resp_send_500(req);
    }
    httpd_resp_send(req, page, page_len);
    free(page);
    return ESP_OK;
}

// HTTP GET handler for write page
static esp_err_t write_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    char nav[WEB_UI_NAV_MAX_LEN];
    web_ui_build_nav(nav, sizeof(nav), WEB_NAV_WRITE);
    size_t page_len = 0;
    char *page = web_ui_alloc_page(0, &page_len, "Manual Write - OpenTherm Gateway", WEB_UI_WRITE_STYLES, nav, WEB_UI_WRITE_BODY);
    if (!page) {
        return httpd_resp_send_500(req);
    }
    httpd_resp_send(req, page, page_len);
    free(page);
    return ESP_OK;
}

// MQTT state API
static esp_err_t mqtt_state_handler(httpd_req_t *req)
{
    mqtt_bridge_state_t st = {0};
    mqtt_bridge_get_state(&st);
    char buf[256];
    int len = snprintf(buf, sizeof(buf),
        "{\"connected\":%s,\"last_tset_valid\":%s,\"last_tset\":%.2f,"
        "\"last_ch_enable_valid\":%s,\"last_ch_enable\":%s,\"last_update_ms\":%lld,\"available\":%s}",
        st.connected ? "true" : "false",
        st.last_tset_valid ? "true" : "false",
        st.last_tset_c,
        st.last_ch_enable_valid ? "true" : "false",
        st.last_ch_enable ? "true" : "false",
        (long long)st.last_update_ms,
        st.available ? "true" : "false");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, buf, len);
    return ESP_OK;
}

// MQTT config API (GET/POST form-urlencoded)
static bool read_req_body(httpd_req_t *req, char *buf, size_t max)
{
    size_t off = 0;
    while (off < max - 1) {
        int ret = httpd_req_recv(req, buf + off, max - 1 - off);
        if (ret <= 0) break;
        off += ret;
        if (off >= req->content_len) break;
    }
    buf[off] = 0;
    return true;
}

static void url_decode(char *s)
{
    char *src = s, *dst = s;
    while (*src) {
        if (*src == '%' && src[1] && src[2]) {
            char hex[3] = { src[1], src[2], 0 };
            *dst++ = (char)strtol(hex, NULL, 16);
            src += 3;
        } else if (*src == '+') {
            *dst++ = ' ';
            src++;
        } else {
            *dst++ = *src++;
        }
    }
    *dst = 0;
}

static void parse_form_kv(const char *body, const char *key, char *out, size_t out_sz)
{
    const char *p = body;
    size_t key_len = strlen(key);
    while (p && *p) {
        const char *eq = strchr(p, '=');
        if (!eq) break;
        const char *amp = strchr(eq, '&');
        size_t this_key_len = (size_t)(eq - p);
        if (this_key_len == key_len && strncmp(p, key, key_len) == 0) {
            size_t val_len = amp ? (size_t)(amp - eq - 1) : strlen(eq + 1);
            if (val_len >= out_sz) val_len = out_sz - 1;
            memcpy(out, eq + 1, val_len);
            out[val_len] = 0;
            url_decode(out);
            return;
        }
        if (!amp) break;
        p = amp + 1;
    }
    if (out_sz) out[0] = 0;
}

static esp_err_t mqtt_config_get_handler(httpd_req_t *req)
{
    mqtt_bridge_config_t cfg;
    mqtt_bridge_load_config(&cfg);
    mqtt_bridge_state_t st = {0};
    mqtt_bridge_get_state(&st);

    char buf[512];
    int len = snprintf(buf, sizeof(buf),
        "{\"enable\":%s,\"broker_uri\":\"%s\",\"client_id\":\"%s\","
        "\"username\":\"%s\",\"base_topic\":\"%s\",\"discovery_prefix\":\"%s\",\"connected\":%s}",
        cfg.enable ? "true" : "false",
        cfg.broker_uri,
        cfg.client_id,
        cfg.username,
        cfg.base_topic,
        cfg.discovery_prefix,
        st.connected ? "true" : "false");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, buf, len);
    return ESP_OK;
}

static esp_err_t mqtt_config_post_handler(httpd_req_t *req)
{
    char body[512];
    read_req_body(req, body, sizeof(body));

    mqtt_bridge_config_t cfg;
    mqtt_bridge_load_config(&cfg);

    char val[128];
    parse_form_kv(body, "enable", val, sizeof(val));
    if (val[0]) cfg.enable = (strcmp(val, "on") == 0 || strcmp(val, "1") == 0 || strcasecmp(val, "true") == 0);

    parse_form_kv(body, "broker_uri", cfg.broker_uri, sizeof(cfg.broker_uri));
    parse_form_kv(body, "client_id", cfg.client_id, sizeof(cfg.client_id));
    parse_form_kv(body, "username", cfg.username, sizeof(cfg.username));
    parse_form_kv(body, "password", cfg.password, sizeof(cfg.password));
    parse_form_kv(body, "base_topic", cfg.base_topic, sizeof(cfg.base_topic));
    parse_form_kv(body, "discovery_prefix", cfg.discovery_prefix, sizeof(cfg.discovery_prefix));

    mqtt_bridge_save_config(&cfg);
    mqtt_bridge_start(&cfg, NULL);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"status\":\"ok\"}");
    return ESP_OK;
}

// Control mode API
static esp_err_t control_mode_get_handler(httpd_req_t *req)
{
    boiler_manager_status_t st = {0};
    boiler_manager_get_status(s_boiler_mgr, &st);
    char buf[256];
    int len = snprintf(buf, sizeof(buf),
        "{\"enabled\":%s,\"active\":%s,\"fallback\":%s,\"mqtt_available\":%s,"
        "\"demand_tset\":%.2f,\"demand_ch\":%s,\"last_demand_ms\":%lld}",
        st.control_enabled ? "true" : "false",
        st.control_active ? "true" : "false",
        st.fallback_active ? "true" : "false",
        st.mqtt_available ? "true" : "false",
        st.demand_tset_c,
        st.demand_ch_enabled ? "true" : "false",
        (long long)st.last_demand_ms);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, buf, len);
    return ESP_OK;
}

static esp_err_t control_mode_post_handler(httpd_req_t *req)
{
    char body[256];
    read_req_body(req, body, sizeof(body));
    char val[64];
    parse_form_kv(body, "enabled", val, sizeof(val));
    bool enable = (strcmp(val, "on") == 0 || strcmp(val, "1") == 0 || strcasecmp(val, "true") == 0);
    if (enable) {
        boiler_manager_set_mode(s_boiler_mgr, BOILER_MANAGER_MODE_CONTROL);
        boiler_manager_set_control_enabled(s_boiler_mgr, true);
    } else {
        boiler_manager_set_control_enabled(s_boiler_mgr, false);
        boiler_manager_set_mode(s_boiler_mgr, BOILER_MANAGER_MODE_PASSTHROUGH);
    }
    // Sync state to MQTT
    mqtt_bridge_publish_control_state(enable);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"status\":\"ok\"}");
    return ESP_OK;
}

// HTTP GET handler for diagnostics page
static esp_err_t diagnostics_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    char nav[WEB_UI_NAV_MAX_LEN];
    web_ui_build_nav(nav, sizeof(nav), WEB_NAV_DIAGNOSTICS);
    size_t page_len = 0;
    char *page = web_ui_alloc_page(0, &page_len, "Diagnostics - OpenTherm Gateway", WEB_UI_DIAGNOSTICS_STYLES, nav, WEB_UI_DIAGNOSTICS_BODY);
    if (!page) {
        return httpd_resp_send_500(req);
    }
    httpd_resp_send(req, page, page_len);
    free(page);
    return ESP_OK;
}

// MQTT config page handler
static esp_err_t mqtt_page_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    char nav[WEB_UI_NAV_MAX_LEN];
    web_ui_build_nav(nav, sizeof(nav), WEB_NAV_MQTT);
    size_t page_len = 0;
    char *page = web_ui_alloc_page(0, &page_len, "MQTT - OpenTherm Gateway", WEB_UI_MQTT_STYLES, nav, WEB_UI_MQTT_BODY);
    if (!page) {
        return httpd_resp_send_500(req);
    }
    httpd_resp_send(req, page, page_len);
    free(page);
    return ESP_OK;
}

// API handler for manual WRITE_DATA frame injection
static esp_err_t write_api_handler(httpd_req_t *req)
{
    if (!s_boiler_mgr) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_send(req, "{\"error\":\"Boiler manager not available\"}", -1);
        return ESP_FAIL;
    }
    
    // Read request body
    char content[256];
    int ret = httpd_req_recv(req, content, sizeof(content) - 1);
    if (ret <= 0) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_send(req, "{\"error\":\"Failed to read request body\"}", -1);
        return ESP_FAIL;
    }
    content[ret] = '\0';
    
    // Parse JSON manually (simple parsing to avoid dependencies)
    // Expected format: {"data_id": 1, "data_value": 12345, "data_type": "float"}
    uint8_t data_id = 0;
    uint16_t data_value = 0;
    bool has_data_id = false, has_data_value = false;
    bool is_float = false;
    
    // Simple JSON parsing - find data_id and data_value
    char *p = content;
    while (*p) {
        // Find "data_id"
        if (strncmp(p, "\"data_id\"", 9) == 0) {
            p += 9;
            // Skip whitespace and colon
            while (*p && (*p == ' ' || *p == ':' || *p == '\t')) p++;
            // Parse number
            if (*p >= '0' && *p <= '9') {
                data_id = (uint8_t)atoi(p);
                has_data_id = true;
            }
        }
        // Find "data_value"
        else if (strncmp(p, "\"data_value\"", 12) == 0) {
            p += 12;
            // Skip whitespace and colon
            while (*p && (*p == ' ' || *p == ':' || *p == '\t')) p++;
            // Check if it's a float (contains decimal point)
            char *value_start = p;
            if (strchr(value_start, '.')) {
                float float_val = atof(value_start);
                // Encode as f8.8 format
                data_value = (uint16_t)((int16_t)(float_val * 256.0f));
                is_float = true;
            } else {
                // Check for hex (0x prefix)
                if (value_start[0] == '0' && (value_start[1] == 'x' || value_start[1] == 'X')) {
                    data_value = (uint16_t)strtoul(value_start, NULL, 16);
                } else {
                    data_value = (uint16_t)atoi(value_start);
                }
            }
            has_data_value = true;
        }
        // Find "data_type" to determine if we should treat as float
        else if (strncmp(p, "\"data_type\"", 11) == 0) {
            p += 11;
            while (*p && (*p == ' ' || *p == ':' || *p == '\t' || *p == '"')) p++;
            if (strncmp(p, "float", 5) == 0) {
                is_float = true;
            }
        }
        p++;
    }
    
    // If data_type is float but we parsed as int, re-parse
    if (is_float && !strchr(content, '.')) {
        // Find data_value again and parse as float
        p = content;
        while (*p) {
            if (strncmp(p, "\"data_value\"", 12) == 0) {
                p += 12;
                while (*p && (*p == ' ' || *p == ':' || *p == '\t')) p++;
                float float_val = atof(p);
                data_value = (uint16_t)((int16_t)(float_val * 256.0f));
                break;
            }
            p++;
        }
    }
    
    if (!has_data_id || !has_data_value) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_send(req, "{\"error\":\"Missing data_id or data_value\"}", -1);
        return ESP_FAIL;
    }
    
    // Send WRITE_DATA frame
    uint32_t response_frame = 0;
    esp_err_t err = boiler_manager_write_data(s_boiler_mgr, data_id, data_value, &response_frame);
    
    // Build JSON response
    char json_response[512];
    if (err == ESP_OK) {
        ot_message_type_t response_type = ot_get_message_type(response_frame);
        uint16_t response_data = ot_get_uint16(response_frame);
        
        snprintf(json_response, sizeof(json_response),
                 "{\"success\":true,\"request\":{\"data_id\":%d,\"data_value\":%d},"
                 "\"response\":{\"frame\":%lu,\"type\":%d,\"data_id\":%d,\"data_value\":%d}}",
                 data_id, data_value,
                 (unsigned long)response_frame, response_type,
                 ot_get_data_id(response_frame), response_data);
    } else {
        const char *error_msg = "Unknown error";
        if (err == ESP_ERR_TIMEOUT) error_msg = "Timeout waiting for response";
        else if (err == ESP_ERR_INVALID_RESPONSE) error_msg = "Invalid response from boiler";
        else if (err == ESP_ERR_NOT_FOUND) error_msg = "Unknown data ID";
        
        snprintf(json_response, sizeof(json_response),
                 "{\"success\":false,\"error\":\"%s\",\"error_code\":%d}",
                 error_msg, err);
    }
    
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json_response, strlen(json_response));
    
    return ESP_OK;
}

// API handler for diagnostics JSON
static esp_err_t diagnostics_api_handler(httpd_req_t *req)
{
    if (!s_boiler_mgr) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_send(req, "{\"error\":\"Boiler manager not available\"}", -1);
        return ESP_FAIL;
    }
    
    const boiler_diagnostics_t *diag = boiler_manager_get_diagnostics(s_boiler_mgr);
    if (!diag) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_send(req, "{\"error\":\"Diagnostics not available\"}", -1);
        return ESP_FAIL;
    }
    
    // Calculate current time (milliseconds since boot)
    int64_t current_time_ms = esp_timer_get_time() / 1000;
    
    // Helper macro to calculate age in milliseconds
    #define CALC_AGE_MS(field) ((field.valid && field.timestamp_ms > 0) ? (current_time_ms - field.timestamp_ms) : -1)
    
    // Build JSON response manually (avoiding cJSON dependency)
    // Use heap allocation to avoid stack overflow
    const size_t json_buffer_size = 8192;
    char *json_buffer = malloc(json_buffer_size);
    if (!json_buffer) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_send(req, "{\"error\":\"Memory allocation failed\"}", -1);
        return ESP_FAIL;
    }
    
    int len = snprintf(json_buffer, json_buffer_size,
        "{"
        "\"t_boiler\":{\"value\":%.2f,\"age_ms\":%lld,\"valid\":%s},"
        "\"t_return\":{\"value\":%.2f,\"age_ms\":%lld,\"valid\":%s},"
        "\"t_dhw\":{\"value\":%.2f,\"age_ms\":%lld,\"valid\":%s},"
        "\"t_dhw2\":{\"value\":%.2f,\"age_ms\":%lld,\"valid\":%s},"
        "\"t_outside\":{\"value\":%.2f,\"age_ms\":%lld,\"valid\":%s},"
        "\"t_exhaust\":{\"value\":%.2f,\"age_ms\":%lld,\"valid\":%s},"
        "\"t_heat_exchanger\":{\"value\":%.2f,\"age_ms\":%lld,\"valid\":%s},"
        "\"t_flow_ch2\":{\"value\":%.2f,\"age_ms\":%lld,\"valid\":%s},"
        "\"t_storage\":{\"value\":%.2f,\"age_ms\":%lld,\"valid\":%s},"
        "\"t_collector\":{\"value\":%.2f,\"age_ms\":%lld,\"valid\":%s},"
        "\"t_setpoint\":{\"value\":%.2f,\"age_ms\":%lld,\"valid\":%s},"
        "\"modulation_level\":{\"value\":%.2f,\"age_ms\":%lld,\"valid\":%s},"
        "\"pressure\":{\"value\":%.2f,\"age_ms\":%lld,\"valid\":%s},"
        "\"flow_rate\":{\"value\":%.2f,\"age_ms\":%lld,\"valid\":%s},"
        "\"fault_code\":{\"value\":%.2f,\"age_ms\":%lld,\"valid\":%s},"
        "\"diag_code\":{\"value\":%.2f,\"age_ms\":%lld,\"valid\":%s},"
        "\"burner_starts\":{\"value\":%.2f,\"age_ms\":%lld,\"valid\":%s},"
        "\"dhw_burner_starts\":{\"value\":%.2f,\"age_ms\":%lld,\"valid\":%s},"
        "\"ch_pump_starts\":{\"value\":%.2f,\"age_ms\":%lld,\"valid\":%s},"
        "\"dhw_pump_starts\":{\"value\":%.2f,\"age_ms\":%lld,\"valid\":%s},"
        "\"burner_hours\":{\"value\":%.2f,\"age_ms\":%lld,\"valid\":%s},"
        "\"dhw_burner_hours\":{\"value\":%.2f,\"age_ms\":%lld,\"valid\":%s},"
        "\"ch_pump_hours\":{\"value\":%.2f,\"age_ms\":%lld,\"valid\":%s},"
        "\"dhw_pump_hours\":{\"value\":%.2f,\"age_ms\":%lld,\"valid\":%s},"
        "\"max_capacity\":{\"value\":%.2f,\"age_ms\":%lld,\"valid\":%s},"
        "\"min_mod_level\":{\"value\":%.2f,\"age_ms\":%lld,\"valid\":%s},"
        "\"fan_setpoint\":{\"value\":%.2f,\"age_ms\":%lld,\"valid\":%s},"
        "\"fan_current\":{\"value\":%.2f,\"age_ms\":%lld,\"valid\":%s},"
        "\"fan_exhaust_rpm\":{\"value\":%.2f,\"age_ms\":%lld,\"valid\":%s},"
        "\"fan_supply_rpm\":{\"value\":%.2f,\"age_ms\":%lld,\"valid\":%s},"
        "\"co2_exhaust\":{\"value\":%.2f,\"age_ms\":%lld,\"valid\":%s}"
        "}",
        diag->t_boiler.value, (long long)CALC_AGE_MS(diag->t_boiler), diag->t_boiler.valid ? "true" : "false",
        diag->t_return.value, (long long)CALC_AGE_MS(diag->t_return), diag->t_return.valid ? "true" : "false",
        diag->t_dhw.value, (long long)CALC_AGE_MS(diag->t_dhw), diag->t_dhw.valid ? "true" : "false",
        diag->t_dhw2.value, (long long)CALC_AGE_MS(diag->t_dhw2), diag->t_dhw2.valid ? "true" : "false",
        diag->t_outside.value, (long long)CALC_AGE_MS(diag->t_outside), diag->t_outside.valid ? "true" : "false",
        diag->t_exhaust.value, (long long)CALC_AGE_MS(diag->t_exhaust), diag->t_exhaust.valid ? "true" : "false",
        diag->t_heat_exchanger.value, (long long)CALC_AGE_MS(diag->t_heat_exchanger), diag->t_heat_exchanger.valid ? "true" : "false",
        diag->t_flow_ch2.value, (long long)CALC_AGE_MS(diag->t_flow_ch2), diag->t_flow_ch2.valid ? "true" : "false",
        diag->t_storage.value, (long long)CALC_AGE_MS(diag->t_storage), diag->t_storage.valid ? "true" : "false",
        diag->t_collector.value, (long long)CALC_AGE_MS(diag->t_collector), diag->t_collector.valid ? "true" : "false",
        diag->t_setpoint.value, (long long)CALC_AGE_MS(diag->t_setpoint), diag->t_setpoint.valid ? "true" : "false",
        diag->modulation_level.value, (long long)CALC_AGE_MS(diag->modulation_level), diag->modulation_level.valid ? "true" : "false",
        diag->pressure.value, (long long)CALC_AGE_MS(diag->pressure), diag->pressure.valid ? "true" : "false",
        diag->flow_rate.value, (long long)CALC_AGE_MS(diag->flow_rate), diag->flow_rate.valid ? "true" : "false",
        diag->fault_code.value, (long long)CALC_AGE_MS(diag->fault_code), diag->fault_code.valid ? "true" : "false",
        diag->diag_code.value, (long long)CALC_AGE_MS(diag->diag_code), diag->diag_code.valid ? "true" : "false",
        diag->burner_starts.value, (long long)CALC_AGE_MS(diag->burner_starts), diag->burner_starts.valid ? "true" : "false",
        diag->dhw_burner_starts.value, (long long)CALC_AGE_MS(diag->dhw_burner_starts), diag->dhw_burner_starts.valid ? "true" : "false",
        diag->ch_pump_starts.value, (long long)CALC_AGE_MS(diag->ch_pump_starts), diag->ch_pump_starts.valid ? "true" : "false",
        diag->dhw_pump_starts.value, (long long)CALC_AGE_MS(diag->dhw_pump_starts), diag->dhw_pump_starts.valid ? "true" : "false",
        diag->burner_hours.value, (long long)CALC_AGE_MS(diag->burner_hours), diag->burner_hours.valid ? "true" : "false",
        diag->dhw_burner_hours.value, (long long)CALC_AGE_MS(diag->dhw_burner_hours), diag->dhw_burner_hours.valid ? "true" : "false",
        diag->ch_pump_hours.value, (long long)CALC_AGE_MS(diag->ch_pump_hours), diag->ch_pump_hours.valid ? "true" : "false",
        diag->dhw_pump_hours.value, (long long)CALC_AGE_MS(diag->dhw_pump_hours), diag->dhw_pump_hours.valid ? "true" : "false",
        diag->max_capacity.value, (long long)CALC_AGE_MS(diag->max_capacity), diag->max_capacity.valid ? "true" : "false",
        diag->min_mod_level.value, (long long)CALC_AGE_MS(diag->min_mod_level), diag->min_mod_level.valid ? "true" : "false",
        diag->fan_setpoint.value, (long long)CALC_AGE_MS(diag->fan_setpoint), diag->fan_setpoint.valid ? "true" : "false",
        diag->fan_current.value, (long long)CALC_AGE_MS(diag->fan_current), diag->fan_current.valid ? "true" : "false",
        diag->fan_exhaust_rpm.value, (long long)CALC_AGE_MS(diag->fan_exhaust_rpm), diag->fan_exhaust_rpm.valid ? "true" : "false",
        diag->fan_supply_rpm.value, (long long)CALC_AGE_MS(diag->fan_supply_rpm), diag->fan_supply_rpm.valid ? "true" : "false",
        diag->co2_exhaust.value, (long long)CALC_AGE_MS(diag->co2_exhaust), diag->co2_exhaust.valid ? "true" : "false"
    );
    
    // Ensure buffer is null-terminated and cap length to buffer size
    if (len < 0) {
        ESP_LOGE(TAG, "snprintf failed in diagnostics_api_handler");
        free(json_buffer);
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_send(req, "{\"error\":\"Failed to format diagnostics\"}", -1);
        return ESP_FAIL;
    }
    
    // Cap length to buffer size (snprintf returns required length even if truncated)
    if ((size_t)len >= json_buffer_size) {
        len = json_buffer_size - 1;
        json_buffer[len] = '\0';  // Ensure null termination
    }
    
    httpd_resp_set_type(req, "application/json");
    esp_err_t ret = httpd_resp_send(req, json_buffer, len);
    free(json_buffer);
    
    return ret;
}

// WebSocket handler
static esp_err_t ws_handler(httpd_req_t *req)
{
    if (req->method == HTTP_GET) {
        ESP_LOGI(TAG, "WebSocket handshake");
        
        // Store client file descriptor immediately after handshake
        websocket_server_t *ws_server = (websocket_server_t *)req->user_ctx;
        if (ws_server) {
            ws_server->client_fd = httpd_req_to_sockfd(req);
            ws_server->client_connected = true;
            ESP_LOGI(TAG, "WebSocket client connected, fd=%d", ws_server->client_fd);
        }
        
        return ESP_OK;
    }
    
    httpd_ws_frame_t ws_pkt;
    memset(&ws_pkt, 0, sizeof(httpd_ws_frame_t));
    ws_pkt.type = HTTPD_WS_TYPE_TEXT;
    
    esp_err_t ret = httpd_ws_recv_frame(req, &ws_pkt, 0);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "httpd_ws_recv_frame failed: %s", esp_err_to_name(ret));
        return ret;
    }
    
    ESP_LOGI(TAG, "WebSocket frame received, len=%d", ws_pkt.len);
    
    return ESP_OK;
}

// Callback handler for boiler_manager messages (diagnostics, control responses)
static void boiler_manager_message_handler(const char *direction,
                                           ot_message_source_t source,
                                           uint32_t message,
                                           void *user_data)
{
    (void)user_data;  // We use s_ws_server instead

    if (!s_ws_server) return;

    const char *source_str;
    switch (source) {
        case OT_SOURCE_GATEWAY_BOILER:
            source_str = "GATEWAY_BOILER";
            break;
        case OT_SOURCE_THERMOSTAT_GATEWAY:
            source_str = "THERMOSTAT_GATEWAY";
            break;
        default:
            source_str = "THERMOSTAT_BOILER";
            break;
    }

    ot_message_type_t msg_type = ot_get_message_type(message);
    uint8_t data_id = ot_get_data_id(message);
    uint16_t data_value = ot_get_uint16(message);

    websocket_server_send_opentherm_message(s_ws_server,
                                            direction,
                                            message,
                                            ot_message_type_to_string(msg_type),
                                            data_id,
                                            data_value,
                                            source_str);
}

// Start WebSocket server
esp_err_t websocket_server_start(websocket_server_t *ws_server, boiler_manager_t *boiler_mgr)
{
    if (!boiler_mgr) {
        return ESP_ERR_INVALID_ARG;
    }

    s_boiler_mgr = boiler_mgr;
    s_ws_server = ws_server;  // Store for callback access
    memset(ws_server, 0, sizeof(websocket_server_t));

    // Register message callback for logging boiler_manager communications
    boiler_manager_set_message_callback(boiler_mgr, boiler_manager_message_handler, ws_server);

    // Register MQTT control mode callback
    mqtt_bridge_set_control_callback(mqtt_control_mode_handler, NULL);
    
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_open_sockets = 7;
    config.max_uri_handlers = 16;  // Increased for OTA handlers
    config.uri_match_fn = httpd_uri_match_wildcard;
    config.recv_wait_timeout = 30;  // 30 seconds - needed for OTA uploads
    config.send_wait_timeout = 30;  // 30 seconds
    config.stack_size = 8192;       // Larger stack for OTA operations
    config.lru_purge_enable = true; // Allow purging idle connections
    
    ESP_LOGI(TAG, "Starting WebSocket server on port %d", config.server_port);
    
    if (httpd_start(&ws_server->server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server");
        return ESP_FAIL;
    }
    
    // Register root handler (dashboard)
    httpd_uri_t root_uri = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = root_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(ws_server->server, &root_uri);
    
    // Register logs page handler
    httpd_uri_t logs_uri = {
        .uri = "/logs",
        .method = HTTP_GET,
        .handler = logs_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(ws_server->server, &logs_uri);
    
    // Register diagnostics page handler
    httpd_uri_t diagnostics_uri = {
        .uri = "/diagnostics",
        .method = HTTP_GET,
        .handler = diagnostics_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(ws_server->server, &diagnostics_uri);

    // Register MQTT config page handler
    httpd_uri_t mqtt_uri = {
        .uri = "/mqtt",
        .method = HTTP_GET,
        .handler = mqtt_page_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(ws_server->server, &mqtt_uri);
    
    // Register write page handler
    httpd_uri_t write_uri = {
        .uri = "/write",
        .method = HTTP_GET,
        .handler = write_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(ws_server->server, &write_uri);
    
    // Register diagnostics API handler
    httpd_uri_t diagnostics_api_uri = {
        .uri = "/api/diagnostics",
        .method = HTTP_GET,
        .handler = diagnostics_api_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(ws_server->server, &diagnostics_api_uri);

    // Register MQTT state API handler
    httpd_uri_t mqtt_state_uri = {
        .uri = "/api/mqtt_state",
        .method = HTTP_GET,
        .handler = mqtt_state_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(ws_server->server, &mqtt_state_uri);

    // Register MQTT config API handlers
    httpd_uri_t mqtt_cfg_get_uri = {
        .uri = "/api/mqtt_config",
        .method = HTTP_GET,
        .handler = mqtt_config_get_handler,
        .user_ctx = NULL
    };
    httpd_uri_t mqtt_cfg_post_uri = {
        .uri = "/api/mqtt_config",
        .method = HTTP_POST,
        .handler = mqtt_config_post_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(ws_server->server, &mqtt_cfg_get_uri);
    httpd_register_uri_handler(ws_server->server, &mqtt_cfg_post_uri);

    // Control mode API handlers
    httpd_uri_t control_get_uri = {
        .uri = "/api/control_mode",
        .method = HTTP_GET,
        .handler = control_mode_get_handler,
        .user_ctx = NULL
    };
    httpd_uri_t control_post_uri = {
        .uri = "/api/control_mode",
        .method = HTTP_POST,
        .handler = control_mode_post_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(ws_server->server, &control_get_uri);
    httpd_register_uri_handler(ws_server->server, &control_post_uri);
    
    // Register manual write API handler
    httpd_uri_t write_api_uri = {
        .uri = "/api/write",
        .method = HTTP_POST,
        .handler = write_api_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(ws_server->server, &write_api_uri);
    
    // Register WebSocket handler
    httpd_uri_t ws_uri = {
        .uri = "/ws",
        .method = HTTP_GET,
        .handler = ws_handler,
        .user_ctx = ws_server,
        .is_websocket = true
    };
    httpd_register_uri_handler(ws_server->server, &ws_uri);
    
    ESP_LOGI(TAG, "WebSocket server started successfully");
    return ESP_OK;
}

// Stop WebSocket server
void websocket_server_stop(websocket_server_t *ws_server)
{
    if (ws_server->server) {
        httpd_stop(ws_server->server);
        ws_server->server = NULL;
        ws_server->client_connected = false;
    }
}

// Send text message to connected client
esp_err_t websocket_server_send_text(websocket_server_t *ws_server, const char *text)
{
    if (!ws_server->server || !ws_server->client_connected) {
        ESP_LOGD(TAG, "Not sending WebSocket message: server not started or client not connected");
        return ESP_FAIL;
    }
    
    httpd_ws_frame_t ws_pkt;
    memset(&ws_pkt, 0, sizeof(httpd_ws_frame_t));
    ws_pkt.payload = (uint8_t *)text;
    ws_pkt.len = strlen(text);
    ws_pkt.type = HTTPD_WS_TYPE_TEXT;
    
    esp_err_t ret = httpd_ws_send_frame_async(ws_server->server, ws_server->client_fd, &ws_pkt);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to send WebSocket message: %s", esp_err_to_name(ret));
        ws_server->client_connected = false;
    } else {
        ESP_LOGD(TAG, "WebSocket message sent");
    }
    
    return ret;
}

// Send formatted OpenTherm message
esp_err_t websocket_server_send_opentherm_message(websocket_server_t *ws_server,
                                                   const char *direction,
                                                   uint32_t message,
                                                   const char *msg_type,
                                                   uint8_t data_id,
                                                   uint16_t data_value,
                                                   const char *source)
{
    char json_buffer[512];
    int64_t timestamp = esp_timer_get_time() / 1000;  // Convert to milliseconds

    snprintf(json_buffer, sizeof(json_buffer),
             "{\"timestamp\":%lld,\"direction\":\"%s\",\"source\":\"%s\",\"message\":%lu,\"msg_type\":\"%s\",\"data_id\":%u,\"data_value\":%u}",
             timestamp, direction, source ? source : "THERMOSTAT_BOILER",
             (unsigned long)message, msg_type, data_id, data_value);

    return websocket_server_send_text(ws_server, json_buffer);
}

// Get HTTP server handle for registering additional handlers
httpd_handle_t websocket_server_get_handle(websocket_server_t *ws_server)
{
    return ws_server ? ws_server->server : NULL;
}
