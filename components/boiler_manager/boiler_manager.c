/*
 * Boiler Manager - Diagnostic Injection and State Management
 */

#include "boiler_manager.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_err.h"
#include <string.h>

static const char *TAG = "BoilerManager";

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
            break;
        case 28:  // Tret
            float_val = opentherm_rmt_get_float(response);
            update_diagnostic_value(&bm->diagnostics.t_return, float_val, true);
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
            break;
            
        // Status readings
        case 17:  // RelModLevel
            float_val = opentherm_rmt_get_float(response);
            update_diagnostic_value(&bm->diagnostics.modulation_level, float_val, float_val >= 0 && float_val <= 100);
            break;
        case 18:  // CHPressure
            float_val = opentherm_rmt_get_float(response);
            update_diagnostic_value(&bm->diagnostics.pressure, float_val, float_val >= 0);
            break;
        case 19:  // DHWFlowRate
            float_val = opentherm_rmt_get_float(response);
            update_diagnostic_value(&bm->diagnostics.flow_rate, float_val, float_val >= 0);
            break;
            
        // Faults
        case 5:   // ASFflags
            uint8_val = opentherm_rmt_get_uint8_lb(response);
            update_diagnostic_value(&bm->diagnostics.fault_code, (float)uint8_val, true);
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
    bm->ot_instance = ot;
    bm->diag_commands = diag_commands;
    bm->diag_commands_count = DIAG_COMMANDS_COUNT;
    bm->diag_commands_index = 0;
    bm->intercepting_id0 = false;
    
    // Set interception rate (0 means intercept all, otherwise intercept every Nth frame)
    // Default to 10 if 0 is passed (intercept 1 in 10 frames)
    bm->intercept_rate = (intercept_rate == 0) ? 10 : intercept_rate;
    bm->id0_frame_counter = 0;
    
    ESP_LOGI(TAG, "Boiler manager initialized in %s mode with %d diagnostic commands, intercept rate: 1/%d",
             mode == BOILER_MANAGER_MODE_PROXY ? "PROXY" : "PASSTHROUGH",
             bm->diag_commands_count, bm->intercept_rate);
    
    return ESP_OK;
}

bool boiler_manager_request_interceptor(OpenThermRmt *ot, OpenThermRmtMessage *request)
{
    // Get boiler_manager from interceptor_data
    boiler_manager_t *bm = (boiler_manager_t *)ot->interceptor_data;
    if (!bm || bm->mode != BOILER_MANAGER_MODE_PROXY) {
        return false;  // Allow passthrough
    }
    
    // Check if this is an ID=0 (Status) request from thermostat
    if (request && opentherm_rmt_get_data_id(request->data) == OT_RMT_MSGID_STATUS) {
        // Increment counter for ID=0 frames
        bm->id0_frame_counter++;
        
        // Only intercept if we've reached the interception rate threshold
        if (bm->id0_frame_counter < bm->intercept_rate) {
            // Not time to intercept yet - allow passthrough
            ESP_LOGD(TAG, "ID=0 frame %d/%d - allowing passthrough", 
                     bm->id0_frame_counter, bm->intercept_rate);
            return false;  // Allow passthrough
        }
        
        // Reset counter and intercept this frame
        bm->id0_frame_counter = 0;
        ESP_LOGD(TAG, "Intercepting ID=0 request (1/%d), injecting diagnostic command", bm->intercept_rate);
        
        // Get next diagnostic command
        const boiler_diagnostic_cmd_t *cmd = &bm->diag_commands[bm->diag_commands_index];
        bm->diag_commands_index = (bm->diag_commands_index + 1) % bm->diag_commands_count;
        
        // Build diagnostic request
        OpenThermRmtMessage diag_request;
        diag_request.data = opentherm_rmt_build_request(OT_RMT_MSGTYPE_READ_DATA, cmd->data_id, 0);
        
        ESP_LOGI(TAG, "Injecting diagnostic command: %s (ID=%d)", cmd->name, cmd->data_id);
        
        // Send diagnostic request directly to boiler (slave side)
        esp_err_t ret = opentherm_rmt_send_frame(bm->ot_instance, diag_request.data, 
                                                  &bm->ot_instance->secondary);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Failed to send diagnostic command: %s", esp_err_to_name(ret));
            return false;  // Let normal flow continue
        }
        
        // Wait for response from boiler
        uint32_t diag_response_frame = 0;
        ret = opentherm_rmt_receive_frame(bm->ot_instance, &bm->ot_instance->secondary, &diag_response_frame, 800);
        
        if (ret == ESP_OK) {
            // Validate response
            if (opentherm_rmt_check_parity(diag_response_frame) &&
                opentherm_rmt_is_valid_response_type(diag_response_frame) &&
                opentherm_rmt_get_data_id(diag_response_frame) == cmd->data_id) {
                
                // Parse and store diagnostic result
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
        
        // Block the ID=0 request - don't forward it to boiler
        // Don't respond to thermostat either (it will timeout, which is acceptable)
        return true;  // Block this request
    }
    
    // Not ID=0 - allow passthrough
    return false;
}

bool boiler_manager_process(boiler_manager_t *bm, OpenThermRmtMessage *request, OpenThermRmtMessage *response)
{
    // This function is kept for compatibility but the interceptor callback is used instead
    (void)bm;
    (void)request;
    (void)response;
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


