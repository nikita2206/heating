/*
 * WebSocket Server for OpenTherm Message Logging (C++)
 */

#include "websocket_server.h"
#include "boiler_manager.hpp"
#include "mqtt_bridge.hpp"
#include "open_therm.h"

extern "C" {
#include "web_ui.h"
}

#include "esp_log.h"
#include "esp_http_server.h"
#include "esp_timer.h"
#include <cstring>
#include <cstdio>
#include <cstdlib>

static const char* TAG = "WebSocket";
static ot::BoilerManager* s_boiler_mgr = nullptr;
static ot::MqttBridge* s_mqtt = nullptr;
static websocket_server_t* s_ws_server = nullptr;

// Callback for MQTT control mode changes
static void mqtt_control_mode_handler(bool enabled) {
    if (!s_boiler_mgr) return;

    ESP_LOGI(TAG, "MQTT control mode change: %s", enabled ? "ON" : "OFF");
    if (enabled) {
        s_boiler_mgr->setMode(ot::ManagerMode::Control);
        s_boiler_mgr->setControlEnabled(true);
    } else {
        s_boiler_mgr->setControlEnabled(false);
        s_boiler_mgr->setMode(ot::ManagerMode::Passthrough);
    }
}

// ============================================================================
// SPA File Handlers (gzipped)
// ============================================================================

// Serve gzipped index.html for all page routes (SPA routing)
static esp_err_t spa_handler(httpd_req_t* req) {
    size_t len;
    const uint8_t* data = web_ui_get_index_html_gz(&len);

    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    httpd_resp_send(req, reinterpret_cast<const char*>(data), len);
    return ESP_OK;
}

// Serve gzipped JS bundle
static esp_err_t js_handler(httpd_req_t* req) {
    size_t len;
    const uint8_t* data = web_ui_get_index_js_gz(&len);

    httpd_resp_set_type(req, "application/javascript");
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    httpd_resp_set_hdr(req, "Cache-Control", "max-age=31536000, immutable");
    httpd_resp_send(req, reinterpret_cast<const char*>(data), len);
    return ESP_OK;
}

// Serve gzipped CSS bundle
static esp_err_t css_handler(httpd_req_t* req) {
    size_t len;
    const uint8_t* data = web_ui_get_index_css_gz(&len);

    httpd_resp_set_type(req, "text/css");
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    httpd_resp_set_hdr(req, "Cache-Control", "max-age=31536000, immutable");
    httpd_resp_send(req, reinterpret_cast<const char*>(data), len);
    return ESP_OK;
}

// ============================================================================
// API Handlers
// ============================================================================

// MQTT state API
static esp_err_t mqtt_state_handler(httpd_req_t* req) {
    ot::MqttState st;
    if (s_mqtt) {
        st = s_mqtt->state();
    }

    char buf[256];
    int len = snprintf(buf, sizeof(buf),
        "{\"connected\":%s,\"last_tset_valid\":%s,\"last_tset\":%.2f,"
        "\"last_ch_enable_valid\":%s,\"last_ch_enable\":%s,\"last_update_ms\":%lld,\"available\":%s}",
        st.connected ? "true" : "false",
        st.lastTsetC.has_value() ? "true" : "false",
        st.lastTsetC.value_or(0.0f),
        st.lastChEnable.has_value() ? "true" : "false",
        st.lastChEnable.value_or(false) ? "true" : "false",
        static_cast<long long>(st.lastUpdateTime.count()),
        st.available ? "true" : "false");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, buf, len);
    return ESP_OK;
}

// MQTT config API helpers
static bool read_req_body(httpd_req_t* req, char* buf, size_t max) {
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

static void url_decode(char* s) {
    char* src = s;
    char* dst = s;
    while (*src) {
        if (*src == '%' && src[1] && src[2]) {
            char hex[3] = { src[1], src[2], 0 };
            *dst++ = static_cast<char>(strtol(hex, nullptr, 16));
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

static void parse_form_kv(const char* body, const char* key, char* out, size_t out_sz) {
    const char* p = body;
    size_t key_len = strlen(key);
    while (p && *p) {
        const char* eq = strchr(p, '=');
        if (!eq) break;
        const char* amp = strchr(eq, '&');
        size_t this_key_len = static_cast<size_t>(eq - p);
        if (this_key_len == key_len && strncmp(p, key, key_len) == 0) {
            size_t val_len = amp ? static_cast<size_t>(amp - eq - 1) : strlen(eq + 1);
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

static esp_err_t mqtt_config_get_handler(httpd_req_t* req) {
    ot::MqttConfig cfg;
    (void)ot::MqttBridge::loadConfig(cfg);
    ot::MqttState st;
    if (s_mqtt) {
        st = s_mqtt->state();
    }

    char buf[512];
    int len = snprintf(buf, sizeof(buf),
        "{\"enable\":%s,\"broker_uri\":\"%s\",\"client_id\":\"%s\","
        "\"username\":\"%s\",\"base_topic\":\"%s\",\"discovery_prefix\":\"%s\",\"connected\":%s}",
        cfg.enable ? "true" : "false",
        cfg.brokerUri.c_str(),
        cfg.clientId.c_str(),
        cfg.username.c_str(),
        cfg.baseTopic.c_str(),
        cfg.discoveryPrefix.c_str(),
        st.connected ? "true" : "false");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, buf, len);
    return ESP_OK;
}

static esp_err_t mqtt_config_post_handler(httpd_req_t* req) {
    char body[512];
    read_req_body(req, body, sizeof(body));

    ot::MqttConfig cfg;
    (void)ot::MqttBridge::loadConfig(cfg);

    char val[128];
    parse_form_kv(body, "enable", val, sizeof(val));
    if (val[0]) cfg.enable = (strcmp(val, "on") == 0 || strcmp(val, "1") == 0 || strcasecmp(val, "true") == 0);

    char broker_uri[128], client_id[64], username[64], password[64], base_topic[64], disc_prefix[64];
    parse_form_kv(body, "broker_uri", broker_uri, sizeof(broker_uri));
    if (broker_uri[0]) cfg.brokerUri = broker_uri;
    parse_form_kv(body, "client_id", client_id, sizeof(client_id));
    if (client_id[0]) cfg.clientId = client_id;
    parse_form_kv(body, "username", username, sizeof(username));
    if (username[0]) cfg.username = username;
    parse_form_kv(body, "password", password, sizeof(password));
    if (password[0]) cfg.password = password;
    parse_form_kv(body, "base_topic", base_topic, sizeof(base_topic));
    if (base_topic[0]) cfg.baseTopic = base_topic;
    parse_form_kv(body, "discovery_prefix", disc_prefix, sizeof(disc_prefix));
    if (disc_prefix[0]) cfg.discoveryPrefix = disc_prefix;

    (void)ot::MqttBridge::saveConfig(cfg);
    if (s_mqtt) {
        (void)s_mqtt->reconfigure(cfg);
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"status\":\"ok\"}");
    return ESP_OK;
}

// Control mode API
static esp_err_t control_mode_get_handler(httpd_req_t* req) {
    ot::ManagerStatus st;
    if (s_boiler_mgr) {
        st = s_boiler_mgr->status();
    }

    char buf[256];
    int len = snprintf(buf, sizeof(buf),
        "{\"enabled\":%s,\"active\":%s,\"fallback\":%s,\"mqtt_available\":%s,"
        "\"demand_tset\":%.2f,\"demand_ch\":%s,\"last_demand_ms\":%lld}",
        st.controlEnabled ? "true" : "false",
        st.controlActive ? "true" : "false",
        st.fallbackActive ? "true" : "false",
        st.mqttAvailable ? "true" : "false",
        st.demandTsetC,
        st.demandChEnabled ? "true" : "false",
        static_cast<long long>(st.lastDemandTime.count()));
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, buf, len);
    return ESP_OK;
}

static esp_err_t control_mode_post_handler(httpd_req_t* req) {
    char body[256];
    read_req_body(req, body, sizeof(body));
    char val[64];
    parse_form_kv(body, "enabled", val, sizeof(val));
    bool enable = (strcmp(val, "on") == 0 || strcmp(val, "1") == 0 || strcasecmp(val, "true") == 0);

    if (s_boiler_mgr) {
        if (enable) {
            s_boiler_mgr->setMode(ot::ManagerMode::Control);
            s_boiler_mgr->setControlEnabled(true);
        } else {
            s_boiler_mgr->setControlEnabled(false);
            s_boiler_mgr->setMode(ot::ManagerMode::Passthrough);
        }
    }

    // Sync state to MQTT
    if (s_mqtt) {
        s_mqtt->publishControlState(enable);
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"status\":\"ok\"}");
    return ESP_OK;
}

// API handler for manual WRITE_DATA frame injection
static esp_err_t write_api_handler(httpd_req_t* req) {
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

    // Parse JSON manually
    uint8_t data_id = 0;
    uint16_t data_value = 0;
    bool has_data_id = false, has_data_value = false;
    bool is_float = false;

    char* p = content;
    while (*p) {
        if (strncmp(p, "\"data_id\"", 9) == 0) {
            p += 9;
            while (*p && (*p == ' ' || *p == ':' || *p == '\t')) p++;
            if (*p >= '0' && *p <= '9') {
                data_id = static_cast<uint8_t>(atoi(p));
                has_data_id = true;
            }
        } else if (strncmp(p, "\"data_value\"", 12) == 0) {
            p += 12;
            while (*p && (*p == ' ' || *p == ':' || *p == '\t')) p++;
            char* value_start = p;
            if (strchr(value_start, '.')) {
                float float_val = static_cast<float>(atof(value_start));
                data_value = static_cast<uint16_t>(static_cast<int16_t>(float_val * 256.0f));
                is_float = true;
            } else {
                if (value_start[0] == '0' && (value_start[1] == 'x' || value_start[1] == 'X')) {
                    data_value = static_cast<uint16_t>(strtoul(value_start, nullptr, 16));
                } else {
                    data_value = static_cast<uint16_t>(atoi(value_start));
                }
            }
            has_data_value = true;
        } else if (strncmp(p, "\"data_type\"", 11) == 0) {
            p += 11;
            while (*p && (*p == ' ' || *p == ':' || *p == '\t' || *p == '"')) p++;
            if (strncmp(p, "float", 5) == 0) {
                is_float = true;
            }
        }
        p++;
    }

    // Re-parse as float if needed
    if (is_float && !strchr(content, '.')) {
        p = content;
        while (*p) {
            if (strncmp(p, "\"data_value\"", 12) == 0) {
                p += 12;
                while (*p && (*p == ' ' || *p == ':' || *p == '\t')) p++;
                float float_val = static_cast<float>(atof(p));
                data_value = static_cast<uint16_t>(static_cast<int16_t>(float_val * 256.0f));
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
    std::optional<ot::Frame> response;
    esp_err_t err = s_boiler_mgr->writeData(data_id, data_value, response);

    // Build JSON response
    char json_response[512];
    if (err == ESP_OK && response.has_value()) {
        auto resp_type = response->messageType();
        uint16_t response_data = response->dataValue();

        snprintf(json_response, sizeof(json_response),
                 "{\"success\":true,\"request\":{\"data_id\":%d,\"data_value\":%d},"
                 "\"response\":{\"frame\":%lu,\"type\":\"%s\",\"data_id\":%d,\"data_value\":%d}}",
                 data_id, data_value,
                 static_cast<unsigned long>(response->raw()),
                 ot::toString(resp_type),
                 response->dataId(), response_data);
    } else {
        const char* error_msg = "Unknown error";
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

// Helper to format diagnostic value
static int format_diag_value(char* buf, size_t buf_size, const char* name,
                              const ot::DiagnosticValue& val, int64_t current_time_ms) {
    int64_t age_ms = (val.isValid() && val.timestamp.count() > 0)
                     ? (current_time_ms - val.timestamp.count()) : -1;
    return snprintf(buf, buf_size,
        "\"%s\":{\"value\":%.2f,\"age_ms\":%lld,\"valid\":%s}",
        name, val.valueOr(0.0f), static_cast<long long>(age_ms),
        val.isValid() ? "true" : "false");
}

// API handler for diagnostics JSON
static esp_err_t diagnostics_api_handler(httpd_req_t* req) {
    if (!s_boiler_mgr) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_send(req, "{\"error\":\"Boiler manager not available\"}", -1);
        return ESP_FAIL;
    }

    const auto& diag = s_boiler_mgr->diagnostics();
    int64_t current_time_ms = esp_timer_get_time() / 1000;

    // Build JSON response
    const size_t json_buffer_size = 8192;
    char* json_buffer = static_cast<char*>(malloc(json_buffer_size));
    if (!json_buffer) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_send(req, "{\"error\":\"Memory allocation failed\"}", -1);
        return ESP_FAIL;
    }

    char* p = json_buffer;
    size_t remaining = json_buffer_size;
    int written;

    *p++ = '{';
    remaining--;

    // Format all diagnostic values
    struct { const char* name; const ot::DiagnosticValue& val; } fields[] = {
        {"t_boiler", diag.tBoiler}, {"t_return", diag.tReturn},
        {"t_dhw", diag.tDhw}, {"t_dhw2", diag.tDhw2},
        {"t_outside", diag.tOutside}, {"t_exhaust", diag.tExhaust},
        {"t_heat_exchanger", diag.tHeatExchanger}, {"t_flow_ch2", diag.tFlowCh2},
        {"t_storage", diag.tStorage}, {"t_collector", diag.tCollector},
        {"t_setpoint", diag.tSetpoint}, {"modulation_level", diag.modulationLevel},
        {"pressure", diag.pressure}, {"flow_rate", diag.flowRate},
        {"fault_code", diag.faultCode}, {"diag_code", diag.diagCode},
        {"burner_starts", diag.burnerStarts}, {"dhw_burner_starts", diag.dhwBurnerStarts},
        {"ch_pump_starts", diag.chPumpStarts}, {"dhw_pump_starts", diag.dhwPumpStarts},
        {"burner_hours", diag.burnerHours}, {"dhw_burner_hours", diag.dhwBurnerHours},
        {"ch_pump_hours", diag.chPumpHours}, {"dhw_pump_hours", diag.dhwPumpHours},
        {"max_capacity", diag.maxCapacity}, {"min_mod_level", diag.minModLevel},
        {"fan_setpoint", diag.fanSetpoint}, {"fan_current", diag.fanCurrent},
        {"fan_exhaust_rpm", diag.fanExhaustRpm}, {"fan_supply_rpm", diag.fanSupplyRpm},
        {"co2_exhaust", diag.co2Exhaust}
    };

    for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); i++) {
        if (i > 0) {
            *p++ = ',';
            remaining--;
        }
        written = format_diag_value(p, remaining, fields[i].name, fields[i].val, current_time_ms);
        if (written > 0 && static_cast<size_t>(written) < remaining) {
            p += written;
            remaining -= written;
        }
    }

    *p++ = '}';
    *p = '\0';

    httpd_resp_set_type(req, "application/json");
    esp_err_t ret = httpd_resp_send(req, json_buffer, p - json_buffer);
    free(json_buffer);

    return ret;
}

// ============================================================================
// WebSocket Handler
// ============================================================================

static esp_err_t ws_handler(httpd_req_t* req) {
    if (req->method == HTTP_GET) {
        ESP_LOGI(TAG, "WebSocket handshake");

        auto* ws_server = static_cast<websocket_server_t*>(req->user_ctx);
        if (ws_server) {
            ws_server->client_fd = httpd_req_to_sockfd(req);
            ws_server->client_connected = true;
            ESP_LOGI(TAG, "WebSocket client connected, fd=%d", ws_server->client_fd);
        }

        return ESP_OK;
    }

    httpd_ws_frame_t ws_pkt = {};
    ws_pkt.type = HTTPD_WS_TYPE_TEXT;

    esp_err_t ret = httpd_ws_recv_frame(req, &ws_pkt, 0);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "httpd_ws_recv_frame failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "WebSocket frame received, len=%d", ws_pkt.len);

    return ESP_OK;
}

// Message callback for boiler_manager
static void boiler_manager_message_handler(std::string_view direction,
                                            ot::MessageSource source,
                                            ot::Frame message) {
    if (!s_ws_server) return;

    const char* source_str = ot::toString(source);
    const char* type_str = ot::toString(message.messageType());

    websocket_server_send_opentherm_message(s_ws_server,
                                            std::string(direction).c_str(),
                                            message.raw(),
                                            type_str,
                                            message.dataId(),
                                            message.dataValue(),
                                            source_str);
}

// ============================================================================
// Public API
// ============================================================================

extern "C" esp_err_t websocket_server_start(websocket_server_t* ws_server,
                                             ot::BoilerManager* boiler_mgr) {
    s_boiler_mgr = boiler_mgr;
    s_ws_server = ws_server;
    memset(ws_server, 0, sizeof(websocket_server_t));

    if (!s_boiler_mgr) {
        ESP_LOGW(TAG, "No boiler manager available - starting without it");
    }

    // Register message callback for logging
    if (s_boiler_mgr) {
        s_boiler_mgr->setMessageCallback(boiler_manager_message_handler);
    }

    // Register MQTT control mode callback
    if (s_mqtt) {
        s_mqtt->setControlCallback(mqtt_control_mode_handler);
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_open_sockets = 7;
    config.max_uri_handlers = 20;
    config.uri_match_fn = httpd_uri_match_wildcard;
    config.recv_wait_timeout = 30;
    config.send_wait_timeout = 30;
    config.stack_size = 8192;
    config.lru_purge_enable = true;

    ESP_LOGI(TAG, "Starting WebSocket server on port %d", config.server_port);

    if (httpd_start(&ws_server->server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server");
        return ESP_FAIL;
    }

    // Register SPA handlers for all page routes
    httpd_uri_t spa_routes[] = {
        { "/", HTTP_GET, spa_handler, nullptr, false, false, nullptr },
        { "/logs", HTTP_GET, spa_handler, nullptr, false, false, nullptr },
        { "/diagnostics", HTTP_GET, spa_handler, nullptr, false, false, nullptr },
        { "/mqtt", HTTP_GET, spa_handler, nullptr, false, false, nullptr },
        { "/write", HTTP_GET, spa_handler, nullptr, false, false, nullptr },
        { "/ota", HTTP_GET, spa_handler, nullptr, false, false, nullptr },
    };
    for (const auto& uri : spa_routes) {
        httpd_register_uri_handler(ws_server->server, &uri);
    }

    // Register asset handlers
    httpd_uri_t js_uri = { "/assets/index.js", HTTP_GET, js_handler, nullptr, false, false, nullptr };
    httpd_uri_t css_uri = { "/assets/index.css", HTTP_GET, css_handler, nullptr, false, false, nullptr };
    httpd_register_uri_handler(ws_server->server, &js_uri);
    httpd_register_uri_handler(ws_server->server, &css_uri);

    // Register API handlers
    httpd_uri_t diagnostics_api_uri = { "/api/diagnostics", HTTP_GET, diagnostics_api_handler, nullptr, false, false, nullptr };
    httpd_register_uri_handler(ws_server->server, &diagnostics_api_uri);

    httpd_uri_t mqtt_state_uri = { "/api/mqtt_state", HTTP_GET, mqtt_state_handler, nullptr, false, false, nullptr };
    httpd_register_uri_handler(ws_server->server, &mqtt_state_uri);

    httpd_uri_t mqtt_cfg_get_uri = { "/api/mqtt_config", HTTP_GET, mqtt_config_get_handler, nullptr, false, false, nullptr };
    httpd_uri_t mqtt_cfg_post_uri = { "/api/mqtt_config", HTTP_POST, mqtt_config_post_handler, nullptr, false, false, nullptr };
    httpd_register_uri_handler(ws_server->server, &mqtt_cfg_get_uri);
    httpd_register_uri_handler(ws_server->server, &mqtt_cfg_post_uri);

    httpd_uri_t control_get_uri = { "/api/control_mode", HTTP_GET, control_mode_get_handler, nullptr, false, false, nullptr };
    httpd_uri_t control_post_uri = { "/api/control_mode", HTTP_POST, control_mode_post_handler, nullptr, false, false, nullptr };
    httpd_register_uri_handler(ws_server->server, &control_get_uri);
    httpd_register_uri_handler(ws_server->server, &control_post_uri);

    httpd_uri_t write_api_uri = { "/api/write", HTTP_POST, write_api_handler, nullptr, false, false, nullptr };
    httpd_register_uri_handler(ws_server->server, &write_api_uri);

    httpd_uri_t ws_uri = { "/ws", HTTP_GET, ws_handler, ws_server, true, false, nullptr };
    httpd_register_uri_handler(ws_server->server, &ws_uri);

    ESP_LOGI(TAG, "WebSocket server started successfully");
    return ESP_OK;
}

extern "C" void websocket_server_set_mqtt(ot::MqttBridge* mqtt) {
    s_mqtt = mqtt;
    if (s_mqtt) {
        s_mqtt->setControlCallback(mqtt_control_mode_handler);
    }
}

extern "C" void websocket_server_stop(websocket_server_t* ws_server) {
    if (ws_server->server) {
        httpd_stop(ws_server->server);
        ws_server->server = nullptr;
        ws_server->client_connected = false;
    }
}

extern "C" esp_err_t websocket_server_send_text(websocket_server_t* ws_server, const char* text) {
    if (!ws_server->server || !ws_server->client_connected) {
        ESP_LOGD(TAG, "Not sending WebSocket message: server not started or client not connected");
        return ESP_FAIL;
    }

    httpd_ws_frame_t ws_pkt = {};
    ws_pkt.payload = reinterpret_cast<uint8_t*>(const_cast<char*>(text));
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

extern "C" esp_err_t websocket_server_send_opentherm_message(websocket_server_t* ws_server,
                                                              const char* direction,
                                                              uint32_t message,
                                                              const char* msg_type,
                                                              uint8_t data_id,
                                                              uint16_t data_value,
                                                              const char* source) {
    char json_buffer[512];
    int64_t timestamp = esp_timer_get_time() / 1000;

    snprintf(json_buffer, sizeof(json_buffer),
             "{\"timestamp\":%lld,\"direction\":\"%s\",\"source\":\"%s\",\"message\":%lu,\"msg_type\":\"%s\",\"data_id\":%u,\"data_value\":%u}",
             static_cast<long long>(timestamp), direction, source ? source : "THERMOSTAT_BOILER",
             static_cast<unsigned long>(message), msg_type, data_id, data_value);

    return websocket_server_send_text(ws_server, json_buffer);
}

extern "C" httpd_handle_t websocket_server_get_handle(websocket_server_t* ws_server) {
    return ws_server ? ws_server->server : nullptr;
}
