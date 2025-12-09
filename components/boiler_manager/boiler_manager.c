/*
 * Boiler Manager - Diagnostic Injection and State Management
 */

#include "boiler_manager.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_err.h"
#include "mqtt_bridge.h"
#include <string.h>

static const char *TAG = "BoilerManager";

#define CONTROL_APPLY_INTERVAL_MS 5000
#define CONTROL_MQTT_STALE_MS 45000
#define CONTROL_DIAG_INTERVAL_MS 1000

// Diagnostic command list - rotation order
static const boiler_diagnostic_cmd_t diag_commands[] = {
    {25, "Tboiler"},           // Boiler temperature
    {28, "Tret"},              // Return temperature
    {26, "Tdhw"},              // DHW temperature
    {1, "TSet"},               // Control setpoint (CH water temperature setpoint)
    {17, "RelModLevel"},       // Modulation level
    {18, "CHPressure"},        // Pressure
    {27, "Toutside"},          // Outside temperature
    {33, "Texhaust"},           // Exhaust temperature
    {34, "TboilerHeatExchanger"}, // Heat exchanger temperature
    {19, "DHWFlowRate"},        // Flow rate
    {5, "ASFflags"},            // Fault flags
    {115, "OEMDiagnosticCode"}, // Diagnostic code
    {15, "MaxCapacityMinModLevel"}, // Max capacity / min mod level
    {35, "BoilerFanSpeed"},     // Fan speed
    {32, "Tdhw2"},              // DHW temperature 2
    {31, "TflowCH2"},           // CH2 flow temperature
    {29, "Tstorage"},           // Solar storage temperature
    {30, "Tcollector"},         // Solar collector temperature
    {79, "CO2exhaust"},         // CO2 exhaust
    {84, "RPMexhaust"},         // Exhaust fan RPM
    {85, "RPMsupply"},          // Supply fan RPM
    {116, "BurnerStarts"},      // Burner starts
    {119, "DHWBurnerStarts"},   // DHW burner starts
    {117, "CHPumpStarts"},      // CH pump starts
    {118, "DHWPumpStarts"},     // DHW pump starts
    {120, "BurnerHours"},       // Burner hours
    {123, "DHWBurnerHours"},    // DHW burner hours
    {121, "CHPumpHours"},       // CH pump hours
    {122, "DHWPumpHours"},      // DHW pump hours
};

#define DIAG_COMMANDS_COUNT (sizeof(diag_commands) / sizeof(diag_commands[0]))

// Helper to update a diagnostic value
static void update_diagnostic_value(boiler_diagnostic_value_t *dv, float value, bool valid)
{
    dv->value = value;
    dv->timestamp_ms = esp_timer_get_time() / 1000;  // Convert to milliseconds
    dv->valid = valid;
}

static void publish_diag_value(const char *id, const char *name, const char *unit, const boiler_diagnostic_value_t *dv)
{
    if (!dv) return;
    mqtt_bridge_publish_sensor(id, name, unit, dv->value, dv->valid);
}

static uint16_t float_to_f88(float val)
{
    if (val < 0) val = 0;
    if (val > 250.0f) val = 250.0f; // safety clamp
    return (uint16_t)(val * 256.0f);
}

static uint16_t build_status_word(bool ch_on)
{
    uint16_t status = 0;
    if (ch_on) {
        status |= (1 << 0); // CH enabled (master config bit)
        status |= (1 << 1); // DHW enable (keep on to avoid disabling DHW inadvertently)
    }
    // leave other bits zero
    return status;
}

static bool refresh_control_state(boiler_manager_t *bm)
{
    if (!bm) return false;
    mqtt_bridge_state_t mqtt = {0};
    mqtt_bridge_get_state(&mqtt);

    if (mqtt.last_tset_valid) {
        bm->demand_tset_c = mqtt.last_tset_c;
        bm->last_demand_ms = mqtt.last_override_ms;
    }
    if (mqtt.last_ch_enable_valid) {
        bm->demand_ch_enabled = mqtt.last_ch_enable;
        bm->last_demand_ms = mqtt.last_override_ms;
    }

    bool mqtt_ok = mqtt.available;
    bm->control_active = bm->control_enabled && mqtt_ok;
    bm->fallback_active = bm->control_enabled && !mqtt_ok;
    return mqtt_ok;
}

static void parse_diagnostic_response(boiler_manager_t *bm, uint8_t data_id, uint32_t response);

static void poll_next_diag(boiler_manager_t *bm)
{
    if (!bm || !bm->ot_instance) return;
    const boiler_diagnostic_cmd_t *cmd = &bm->diag_commands[bm->diag_commands_index];
    bm->diag_commands_index = (bm->diag_commands_index + 1) % bm->diag_commands_count;

    OpenThermRmtMessage diag_request;
    diag_request.data = opentherm_rmt_build_request(OT_RMT_MSGTYPE_READ_DATA, cmd->data_id, 0);

    esp_err_t ret = opentherm_rmt_send_frame(bm->ot_instance, diag_request.data,
                                              &bm->ot_instance->secondary);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to send diagnostic command: %s", esp_err_to_name(ret));
        return;
    }

    uint32_t diag_response_frame = 0;
    ret = opentherm_rmt_receive_frame(bm->ot_instance, &bm->ot_instance->secondary, &diag_response_frame, 800);
    if (ret == ESP_OK) {
        if (opentherm_rmt_check_parity(diag_response_frame) &&
            opentherm_rmt_is_valid_response_type(diag_response_frame) &&
            opentherm_rmt_get_data_id(diag_response_frame) == cmd->data_id) {
            parse_diagnostic_response(bm, cmd->data_id, diag_response_frame);
        }
    }
}

// Parse diagnostic response based on data ID
static void parse_diagnostic_response(boiler_manager_t *bm, uint8_t data_id, uint32_t response)
{
    float float_val;
    uint16_t uint16_val;
    uint8_t uint8_val;
    
    switch (data_id) {
        // Temperature readings (f8.8 format)
        case 25:  // Tboiler
            float_val = opentherm_rmt_get_float(response);
            update_diagnostic_value(&bm->diagnostics.t_boiler, float_val, float_val > 0);
            publish_diag_value("tboiler", "Boiler Temperature", "°C", &bm->diagnostics.t_boiler);
            break;
        case 28:  // Tret
            float_val = opentherm_rmt_get_float(response);
            update_diagnostic_value(&bm->diagnostics.t_return, float_val, true);
            publish_diag_value("treturn", "Return Temperature", "°C", &bm->diagnostics.t_return);
            break;
        case 26:  // Tdhw
            float_val = opentherm_rmt_get_float(response);
            update_diagnostic_value(&bm->diagnostics.t_dhw, float_val, float_val > 0);
            break;
        case 32:  // Tdhw2
            float_val = opentherm_rmt_get_float(response);
            update_diagnostic_value(&bm->diagnostics.t_dhw2, float_val, float_val > 0);
            break;
        case 27:  // Toutside
            float_val = opentherm_rmt_get_float(response);
            update_diagnostic_value(&bm->diagnostics.t_outside, float_val, true);
            break;
        case 33:  // Texhaust
            float_val = (float)opentherm_rmt_get_int8(response);
            update_diagnostic_value(&bm->diagnostics.t_exhaust, float_val, float_val > -40 && float_val < 500);
            publish_diag_value("texhaust", "Exhaust Temperature", "°C", &bm->diagnostics.t_exhaust);
            break;
        case 34:  // TboilerHeatExchanger
            float_val = (float)opentherm_rmt_get_int8(response);
            update_diagnostic_value(&bm->diagnostics.t_heat_exchanger, float_val, float_val > 0);
            break;
        case 31:  // TflowCH2
            float_val = opentherm_rmt_get_float(response);
            update_diagnostic_value(&bm->diagnostics.t_flow_ch2, float_val, float_val > 0);
            break;
        case 29:  // Tstorage
            float_val = opentherm_rmt_get_float(response);
            update_diagnostic_value(&bm->diagnostics.t_storage, float_val, float_val > 0);
            break;
        case 30:  // Tcollector
            float_val = opentherm_rmt_get_float(response);
            update_diagnostic_value(&bm->diagnostics.t_collector, float_val, float_val > 0);
            break;
        case 1:   // TSet (Control Setpoint - CH water temperature setpoint)
            float_val = opentherm_rmt_get_float(response);
            update_diagnostic_value(&bm->diagnostics.t_setpoint, float_val, float_val > 0 && float_val < 100);
            publish_diag_value("tset", "Boiler Setpoint", "°C", &bm->diagnostics.t_setpoint);
            break;
            
        // Status readings
        case 17:  // RelModLevel
            float_val = opentherm_rmt_get_float(response);
            update_diagnostic_value(&bm->diagnostics.modulation_level, float_val, float_val >= 0 && float_val <= 100);
            publish_diag_value("modulation", "Modulation Level", "%", &bm->diagnostics.modulation_level);
            break;
        case 18:  // CHPressure
            float_val = opentherm_rmt_get_float(response);
            update_diagnostic_value(&bm->diagnostics.pressure, float_val, float_val >= 0);
            publish_diag_value("pressure", "CH Pressure", "bar", &bm->diagnostics.pressure);
            break;
        case 19:  // DHWFlowRate
            float_val = opentherm_rmt_get_float(response);
            update_diagnostic_value(&bm->diagnostics.flow_rate, float_val, float_val >= 0);
            break;
            
        // Faults
        case 5:   // ASFflags
            uint8_val = opentherm_rmt_get_uint8_lb(response);
            update_diagnostic_value(&bm->diagnostics.fault_code, (float)uint8_val, true);
            publish_diag_value("fault", "Fault Code", "", &bm->diagnostics.fault_code);
            break;
        case 115: // OEMDiagnosticCode
            uint16_val = opentherm_rmt_get_uint16(response);
            update_diagnostic_value(&bm->diagnostics.diag_code, (float)uint16_val, true);
            break;
            
        // Statistics - starts
        case 116: // BurnerStarts
            uint16_val = opentherm_rmt_get_uint16(response);
            update_diagnostic_value(&bm->diagnostics.burner_starts, (float)uint16_val, true);
            break;
        case 119: // DHWBurnerStarts
            uint16_val = opentherm_rmt_get_uint16(response);
            update_diagnostic_value(&bm->diagnostics.dhw_burner_starts, (float)uint16_val, true);
            break;
        case 117: // CHPumpStarts
            uint16_val = opentherm_rmt_get_uint16(response);
            update_diagnostic_value(&bm->diagnostics.ch_pump_starts, (float)uint16_val, true);
            break;
        case 118: // DHWPumpStarts
            uint16_val = opentherm_rmt_get_uint16(response);
            update_diagnostic_value(&bm->diagnostics.dhw_pump_starts, (float)uint16_val, true);
            break;
            
        // Statistics - hours
        case 120: // BurnerHours
            uint16_val = opentherm_rmt_get_uint16(response);
            update_diagnostic_value(&bm->diagnostics.burner_hours, (float)uint16_val, true);
            break;
        case 123: // DHWBurnerHours
            uint16_val = opentherm_rmt_get_uint16(response);
            update_diagnostic_value(&bm->diagnostics.dhw_burner_hours, (float)uint16_val, true);
            break;
        case 121: // CHPumpHours
            uint16_val = opentherm_rmt_get_uint16(response);
            update_diagnostic_value(&bm->diagnostics.ch_pump_hours, (float)uint16_val, true);
            break;
        case 122: // DHWPumpHours
            uint16_val = opentherm_rmt_get_uint16(response);
            update_diagnostic_value(&bm->diagnostics.dhw_pump_hours, (float)uint16_val, true);
            break;
            
        // Configuration
        case 15:  // MaxCapacityMinModLevel
            uint8_val = opentherm_rmt_get_uint8_lb(response);
            float_val = (float)(opentherm_rmt_get_uint8_hb(response));
            update_diagnostic_value(&bm->diagnostics.max_capacity, float_val, float_val > 0);
            update_diagnostic_value(&bm->diagnostics.min_mod_level, (float)uint8_val, true);
            break;
            
        // Fans
        case 35:  // BoilerFanSpeedSetpointAndActual
            uint8_val = opentherm_rmt_get_uint8_lb(response);
            update_diagnostic_value(&bm->diagnostics.fan_current, (float)uint8_val, true);
            uint8_val = opentherm_rmt_get_uint8_hb(response);
            update_diagnostic_value(&bm->diagnostics.fan_setpoint, (float)uint8_val, true);
            break;
        case 84:  // RPMexhaust
            uint16_val = opentherm_rmt_get_uint16(response);
            update_diagnostic_value(&bm->diagnostics.fan_exhaust_rpm, (float)uint16_val, true);
            break;
        case 85:  // RPMsupply
            uint16_val = opentherm_rmt_get_uint16(response);
            update_diagnostic_value(&bm->diagnostics.fan_supply_rpm, (float)uint16_val, true);
            break;
            
        // CO2
        case 79:  // CO2exhaust
            uint16_val = opentherm_rmt_get_uint16(response);
            update_diagnostic_value(&bm->diagnostics.co2_exhaust, (float)uint16_val, true);
            break;
            
        default:
            ESP_LOGD(TAG, "Unhandled diagnostic data ID: %d", data_id);
            break;
    }
}

esp_err_t boiler_manager_init(boiler_manager_t *bm, boiler_manager_mode_t mode, OpenThermRmt *ot, uint32_t intercept_rate)
{
    if (!bm || !ot) {
        return ESP_ERR_INVALID_ARG;
    }
    
    memset(bm, 0, sizeof(boiler_manager_t));
    bm->mode = mode;
    bm->control_enabled = (mode == BOILER_MANAGER_MODE_CONTROL);
    bm->control_active = false;
    bm->fallback_active = false;
    bm->ot_instance = ot;
    bm->diag_commands = diag_commands;
    bm->diag_commands_count = DIAG_COMMANDS_COUNT;
    bm->diag_commands_index = 0;
    bm->intercepting_id0 = false;
    
    // Set interception rate (0 means intercept all, otherwise intercept every Nth frame)
    // Default to 10 if 0 is passed (intercept 1 in 10 frames)
    bm->intercept_rate = (intercept_rate == 0) ? 10 : intercept_rate;
    bm->id0_frame_counter = 0;
    
    // Initialize manual write synchronization
    bm->manual_write_pending = false;
    bm->manual_write_sem = xSemaphoreCreateBinary();
    if (!bm->manual_write_sem) {
        ESP_LOGE(TAG, "Failed to create manual write semaphore");
        return ESP_ERR_NO_MEM;
    }
    
    ESP_LOGI(TAG, "Boiler manager initialized in %s mode with %d diagnostic commands, intercept rate: 1/%d",
             mode == BOILER_MANAGER_MODE_PROXY ? "PROXY" :
             (mode == BOILER_MANAGER_MODE_CONTROL ? "CONTROL" : "PASSTHROUGH"),
             bm->diag_commands_count, bm->intercept_rate);
    
    return ESP_OK;
}

bool boiler_manager_request_interceptor(OpenThermRmt *ot, OpenThermRmtMessage *request)
{
    boiler_manager_t *bm = (boiler_manager_t *)ot->interceptor_data;
    if (!bm) return false;

    bool proxy_mode = (bm->mode == BOILER_MANAGER_MODE_PROXY);
    bool control_mode = (bm->mode == BOILER_MANAGER_MODE_CONTROL);
    if (!proxy_mode && !control_mode) {
        return false; // passthrough
    }

    if (control_mode && !bm->control_enabled) {
        return false;
    }

    // Refresh control state from MQTT
    if (control_mode) {
        refresh_control_state(bm);
    }

    // PRIORITY 1: Manual writes (allowed in proxy/control)
    if (bm->manual_write_pending) {
        ESP_LOGI(TAG, "Intercepting ID=0 request, injecting manual WRITE_DATA frame: 0x%08lX",
                 (unsigned long)bm->manual_write_frame);

        esp_err_t ret = opentherm_rmt_send_frame(bm->ot_instance, bm->manual_write_frame,
                                                  &bm->ot_instance->secondary);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to send manual WRITE_DATA frame: %s", esp_err_to_name(ret));
            bm->manual_write_result = ret;
            bm->manual_write_pending = false;
            xSemaphoreGive(bm->manual_write_sem);
            return true;
        }

        uint32_t response_frame = 0;
        ret = opentherm_rmt_receive_frame(bm->ot_instance, &bm->ot_instance->secondary, &response_frame, 800);

        if (ret == ESP_OK) {
            uint8_t write_data_id = opentherm_rmt_get_data_id(bm->manual_write_frame);
            if (opentherm_rmt_check_parity(response_frame) &&
                opentherm_rmt_get_data_id(response_frame) == write_data_id) {

                OpenThermRmtMessageType response_type = opentherm_rmt_get_message_type(response_frame);
                if (response_type == OT_RMT_MSGTYPE_WRITE_ACK) {
                    ESP_LOGI(TAG, "Received WRITE_ACK for manual write: 0x%08lX", (unsigned long)response_frame);
                    bm->manual_write_response = response_frame;
                    bm->manual_write_result = ESP_OK;
                } else if (response_type == OT_RMT_MSGTYPE_DATA_INVALID) {
                    ESP_LOGW(TAG, "Boiler responded with DATA_INVALID to manual write");
                    bm->manual_write_response = response_frame;
                    bm->manual_write_result = ESP_ERR_INVALID_RESPONSE;
                } else if (response_type == OT_RMT_MSGTYPE_UNKNOWN_DATAID) {
                    ESP_LOGW(TAG, "Boiler responded with UNKNOWN_DATAID to manual write");
                    bm->manual_write_response = response_frame;
                    bm->manual_write_result = ESP_ERR_NOT_FOUND;
                } else {
                    ESP_LOGW(TAG, "Unexpected response type %d to manual write", response_type);
                    bm->manual_write_response = response_frame;
                    bm->manual_write_result = ESP_ERR_INVALID_RESPONSE;
                }
            } else {
                ESP_LOGW(TAG, "Invalid response to manual write: 0x%08lX", (unsigned long)response_frame);
                bm->manual_write_result = ESP_ERR_INVALID_RESPONSE;
            }
        } else {
            ESP_LOGW(TAG, "Timeout waiting for manual WRITE_DATA response");
            bm->manual_write_result = ESP_ERR_TIMEOUT;
        }

        bm->manual_write_pending = false;
        xSemaphoreGive(bm->manual_write_sem);
        return true;
    }

    uint8_t data_id = request ? opentherm_rmt_get_data_id(request->data) : 0xFF;

    // CONTROL MODE: stub basic replies to thermostat
    if (control_mode && bm->control_enabled && bm->control_active && request) {
        uint32_t resp_frame = 0;
        switch (data_id) {
            case OT_RMT_MSGID_STATUS: {
                uint16_t status = build_status_word(bm->demand_ch_enabled);
                resp_frame = opentherm_rmt_build_response(OT_RMT_MSGTYPE_READ_ACK, data_id, status);
                break;
            }
            case 1: { // CH setpoint
                float tset = bm->demand_tset_c > 0 ? bm->demand_tset_c : bm->diagnostics.t_setpoint.value;
                uint16_t v = float_to_f88(tset);
                resp_frame = opentherm_rmt_build_response(OT_RMT_MSGTYPE_READ_ACK, data_id, v);
                break;
            }
            case 3: // Master config
            case 17: { // relative modulation
                resp_frame = opentherm_rmt_build_response(OT_RMT_MSGTYPE_READ_ACK, data_id, 0);
                break;
            }
            default:
                break;
        }
        if (resp_frame != 0) {
            esp_err_t ret = opentherm_rmt_send_frame(bm->ot_instance, resp_frame, &bm->ot_instance->primary);
            if (ret != ESP_OK) {
                ESP_LOGW(TAG, "Failed to send stub response for ID=%d: %s", data_id, esp_err_to_name(ret));
            }
            return true; // block passthrough
        }
        // if not handled, fall through
    }

    // If control enabled but currently fallback, allow passthrough
    if (control_mode && bm->control_enabled && bm->fallback_active) {
        return false;
    }

    // Diagnostics injection (proxy or control)
    if ((proxy_mode || (control_mode && bm->control_enabled)) && request && data_id == OT_RMT_MSGID_STATUS) {
        bm->id0_frame_counter++;
        if (bm->id0_frame_counter < bm->intercept_rate) {
            ESP_LOGD(TAG, "ID=0 frame %d/%d - allowing passthrough",
                     bm->id0_frame_counter, bm->intercept_rate);
            return false;
        }

        bm->id0_frame_counter = 0;
        ESP_LOGD(TAG, "Intercepting ID=0 request (1/%d), injecting diagnostic command", bm->intercept_rate);

        const boiler_diagnostic_cmd_t *cmd = &bm->diag_commands[bm->diag_commands_index];
        bm->diag_commands_index = (bm->diag_commands_index + 1) % bm->diag_commands_count;

        OpenThermRmtMessage diag_request;
        diag_request.data = opentherm_rmt_build_request(OT_RMT_MSGTYPE_READ_DATA, cmd->data_id, 0);

        ESP_LOGI(TAG, "Injecting diagnostic command: %s (ID=%d)", cmd->name, cmd->data_id);

        esp_err_t ret = opentherm_rmt_send_frame(bm->ot_instance, diag_request.data,
                                                  &bm->ot_instance->secondary);
        if (ret == ESP_OK) {
            uint32_t diag_response_frame = 0;
            ret = opentherm_rmt_receive_frame(bm->ot_instance, &bm->ot_instance->secondary, &diag_response_frame, 800);
            if (ret == ESP_OK) {
                if (opentherm_rmt_check_parity(diag_response_frame) &&
                    opentherm_rmt_is_valid_response_type(diag_response_frame) &&
                    opentherm_rmt_get_data_id(diag_response_frame) == cmd->data_id) {
                    parse_diagnostic_response(bm, cmd->data_id, diag_response_frame);
                    ESP_LOGI(TAG, "Received diagnostic response for %s: 0x%08lX",
                             cmd->name, (unsigned long)diag_response_frame);
                } else {
                    ESP_LOGW(TAG, "Invalid diagnostic response for %s: 0x%08lX",
                             cmd->name, (unsigned long)diag_response_frame);
                }
            } else {
                ESP_LOGW(TAG, "Timeout waiting for diagnostic response for %s", cmd->name);
            }
        } else {
            ESP_LOGW(TAG, "Failed to send diagnostic command: %s", esp_err_to_name(ret));
        }

        // In control mode, also provide a stub response to thermostat for ID0
        if (control_mode && bm->control_active) {
            uint16_t status = build_status_word(bm->demand_ch_enabled);
            uint32_t resp_frame = opentherm_rmt_build_response(OT_RMT_MSGTYPE_READ_ACK, OT_RMT_MSGID_STATUS, status);
            opentherm_rmt_send_frame(bm->ot_instance, resp_frame, &bm->ot_instance->primary);
        }

        return true;
    }

    return false;
}

bool boiler_manager_process(boiler_manager_t *bm, OpenThermRmtMessage *request, OpenThermRmtMessage *response)
{
    (void)request;
    (void)response;
    if (!bm) return false;

    if (bm->mode == BOILER_MANAGER_MODE_CONTROL) {
        refresh_control_state(bm);
        if (bm->control_active) {
            int64_t now_ms = esp_timer_get_time() / 1000;
            if (bm->last_control_apply_ms == 0 || (now_ms - bm->last_control_apply_ms) >= CONTROL_APPLY_INTERVAL_MS) {
                if (bm->demand_tset_c > 0) {
                    uint16_t val = float_to_f88(bm->demand_tset_c);
                    boiler_manager_write_data(bm, 1, val, NULL);
                }
                uint16_t status = build_status_word(bm->demand_ch_enabled);
                boiler_manager_write_data(bm, OT_RMT_MSGID_STATUS, status, NULL);
                bm->last_control_apply_ms = now_ms;
            }

            if (bm->last_diag_poll_ms == 0 || (now_ms - bm->last_diag_poll_ms) >= CONTROL_DIAG_INTERVAL_MS) {
                poll_next_diag(bm);
                bm->last_diag_poll_ms = now_ms;
            }
        }
    }
    return false;
}

const boiler_diagnostics_t* boiler_manager_get_diagnostics(boiler_manager_t *bm)
{
    if (!bm) {
        return NULL;
    }
    return &bm->diagnostics;
}

esp_err_t boiler_manager_inject_command(boiler_manager_t *bm, uint8_t data_id)
{
    if (!bm) {
        return ESP_ERR_INVALID_ARG;
    }
    
    // Build request
    OpenThermRmtMessage request;
    request.data = opentherm_rmt_build_request(OT_RMT_MSGTYPE_READ_DATA, data_id, 0);
    
    // Send to boiler
    esp_err_t ret = opentherm_rmt_send_frame(bm->ot_instance, request.data, 
                                              &bm->ot_instance->secondary);
    if (ret != ESP_OK) {
        return ret;
    }
    
    // Wait for response
    uint32_t response_frame = 0;
    ret = opentherm_rmt_receive_frame(bm->ot_instance, &bm->ot_instance->secondary, &response_frame, 800);
    if (ret == ESP_OK) {
        parse_diagnostic_response(bm, data_id, response_frame);
    }
    
    return ret;
}

esp_err_t boiler_manager_write_data(boiler_manager_t *bm, uint8_t data_id, uint16_t data_value, uint32_t *response_frame)
{
    if (!bm || !bm->ot_instance) {
        return ESP_ERR_INVALID_ARG;
    }
    
    // Check if there's already a pending manual write
    if (bm->manual_write_pending) {
        ESP_LOGW(TAG, "Manual write already pending, rejecting new request");
        return ESP_ERR_INVALID_STATE;
    }
    
    // Build WRITE_DATA request
    uint32_t request = opentherm_rmt_build_request(OT_RMT_MSGTYPE_WRITE_DATA, data_id, data_value);
    
    ESP_LOGI(TAG, "Queueing manual WRITE_DATA frame: ID=%d, Value=0x%04X (0x%08lX)", 
             data_id, data_value, (unsigned long)request);
    
    // Clear semaphore (in case it was left in signaled state)
    xSemaphoreTake(bm->manual_write_sem, 0);
    
    // Queue the frame for injection via interceptor
    bm->manual_write_frame = request;
    bm->manual_write_pending = true;
    bm->manual_write_result = ESP_FAIL;  // Will be set by interceptor
    bm->manual_write_response = 0;
    
    // Wait for interceptor to inject the frame and get response
    // Timeout: thermostat sends requests every ~1 second, so wait up to 2 seconds
    // to ensure we catch at least one ID=0 frame
    const TickType_t timeout_ticks = pdMS_TO_TICKS(2000);
    
    if (xSemaphoreTake(bm->manual_write_sem, timeout_ticks) == pdTRUE) {
        // Interceptor has completed the injection
        esp_err_t ret = bm->manual_write_result;
        
        if (ret == ESP_OK && response_frame) {
            *response_frame = bm->manual_write_response;
        }
        
        ESP_LOGI(TAG, "Manual WRITE_DATA completed with result: %s", esp_err_to_name(ret));
        return ret;
    } else {
        // Timeout - interceptor didn't get a chance to inject
        ESP_LOGW(TAG, "Timeout waiting for manual WRITE_DATA injection (no ID=0 frame intercepted)");
        bm->manual_write_pending = false;
        return ESP_ERR_TIMEOUT;
    }
}

void boiler_manager_set_control_enabled(boiler_manager_t *bm, bool enabled)
{
    if (!bm) return;
    bm->control_enabled = enabled;
    bm->fallback_active = false;
    bm->control_active = false;
}

void boiler_manager_get_status(boiler_manager_t *bm, boiler_manager_status_t *out)
{
    if (!bm || !out) return;
    bool mqtt_ok = refresh_control_state(bm);
    out->control_enabled = bm->control_enabled;
    out->control_active = bm->control_active;
    out->fallback_active = bm->fallback_active;
    out->mqtt_available = mqtt_ok;
    out->demand_tset_c = bm->demand_tset_c;
    out->demand_ch_enabled = bm->demand_ch_enabled;
    out->last_demand_ms = bm->last_demand_ms;
}

void boiler_manager_set_mode(boiler_manager_t *bm, boiler_manager_mode_t mode)
{
    if (!bm) return;
    bm->mode = mode;
    bm->control_enabled = (mode == BOILER_MANAGER_MODE_CONTROL) ? bm->control_enabled : false;
    bm->control_active = false;
    bm->fallback_active = false;
}
