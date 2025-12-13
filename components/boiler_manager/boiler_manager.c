/*
 * Boiler Manager - Refactored Main Loop
 *
 * This module runs as the main control loop, coordinating between:
 * - Thermostat thread (receives requests, sends responses)
 * - Boiler thread (sends requests, receives responses)
 *
 * The main loop is NON-BLOCKING - it polls queues and makes decisions.
 * All blocking operations are handled by the dedicated thermostat/boiler threads.
 */

#include "boiler_manager.h"
#include "ot_queues.h"
#include "ot_thermostat.h"
#include "ot_boiler.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "mqtt_bridge.h"
#include <string.h>

static const char *TAG = "BoilerMgr";

// ============================================================================
// Diagnostic command list
// ============================================================================

static const boiler_diagnostic_cmd_t diag_commands[] = {
    {25, "Tboiler"},
    {28, "Tret"},
    {26, "Tdhw"},
    {1, "TSet"},
    {17, "RelModLevel"},
    {18, "CHPressure"},
    {27, "Toutside"},
    {33, "Texhaust"},
    {34, "TboilerHeatExchanger"},
    {19, "DHWFlowRate"},
    {5, "ASFflags"},
    {115, "OEMDiagnosticCode"},
    {15, "MaxCapacityMinModLevel"},
    {35, "BoilerFanSpeed"},
    {32, "Tdhw2"},
    {31, "TflowCH2"},
    {29, "Tstorage"},
    {30, "Tcollector"},
    {79, "CO2exhaust"},
    {84, "RPMexhaust"},
    {85, "RPMsupply"},
    {116, "BurnerStarts"},
    {119, "DHWBurnerStarts"},
    {117, "CHPumpStarts"},
    {118, "DHWPumpStarts"},
    {120, "BurnerHours"},
    {123, "DHWBurnerHours"},
    {121, "CHPumpHours"},
    {122, "DHWPumpHours"},
};

#define DIAG_COMMANDS_COUNT (sizeof(diag_commands) / sizeof(diag_commands[0]))

// ============================================================================
// Internal state for the main loop
// ============================================================================

typedef enum {
    BM_STATE_IDLE,              // Waiting for thermostat request
    BM_STATE_WAIT_BOILER_RESP,  // Forwarded request, waiting for boiler response
    BM_STATE_WAIT_DIAG_RESP,    // Sent diagnostic, waiting for boiler response
} bm_loop_state_t;

// Extended boiler manager context with loop state
typedef struct {
    boiler_manager_t base;          // Public interface
    ot_queues_t *queues;            // Queue handles
    bm_loop_state_t state;          // Main loop state
    uint32_t pending_request;       // Request we're waiting on (for passthrough)
    bool pending_passthrough;       // True if we forwarded a passthrough request
    int64_t last_heartbeat_ms;      // Last heartbeat time
    int64_t last_diag_poll_ms;      // Last diagnostic poll time
} bm_context_t;

static bm_context_t *s_ctx = NULL;

// ============================================================================
// Helper functions
// ============================================================================

static void update_diagnostic_value(boiler_diagnostic_value_t *dv, float value, bool valid)
{
    dv->value = value;
    dv->timestamp_ms = esp_timer_get_time() / 1000;
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
    if (val > 250.0f) val = 250.0f;
    return (uint16_t)(val * 256.0f);
}

static uint16_t build_status_word(bool ch_on)
{
    uint16_t status = 0;
    if (ch_on) {
        status |= (1 << 0);
        status |= (1 << 1);
    }
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

// Inline helper to invoke message callback
static inline void log_message(boiler_manager_t *bm, const char *direction,
                               ot_message_source_t source, uint32_t message) {
    if (bm && bm->message_callback) {
        bm->message_callback(direction, source, message, bm->message_callback_user_data);
    }
}

// ============================================================================
// Message parsing utilities (copied from ot_queues to avoid circular deps)
// ============================================================================

static ot_msg_type_t get_msg_type(uint32_t frame) {
    return (ot_msg_type_t)((frame >> 28) & 0x7);
}

static uint8_t get_data_id(uint32_t frame) {
    return (uint8_t)((frame >> 16) & 0xFF);
}

static uint16_t get_data_value(uint32_t frame) {
    return (uint16_t)(frame & 0xFFFF);
}

static float get_float(uint32_t frame) {
    return (float)((int16_t)get_data_value(frame)) / 256.0f;
}

static uint8_t get_hb(uint32_t frame) {
    return (uint8_t)((frame >> 8) & 0xFF);
}

static uint8_t get_lb(uint32_t frame) {
    return (uint8_t)(frame & 0xFF);
}

static bool check_parity(uint32_t frame) {
    uint32_t count = 0;
    uint32_t n = frame;
    while (n) { n &= (n - 1); count++; }
    return (count % 2) == 0;
}

static uint32_t build_request(ot_msg_type_t type, uint8_t id, uint16_t data) {
    uint32_t frame = ((uint32_t)type << 28) | ((uint32_t)id << 16) | data;
    uint8_t parity = 0;
    uint32_t temp = frame;
    while (temp) { parity ^= (temp & 1); temp >>= 1; }
    frame |= ((uint32_t)parity << 31);
    return frame;
}

static uint32_t build_response(ot_msg_type_t type, uint8_t id, uint16_t data) {
    return build_request(type, id, data);
}

// ============================================================================
// Diagnostic response parsing
// ============================================================================

static void parse_diagnostic_response(boiler_manager_t *bm, uint8_t data_id, uint32_t response)
{
    float float_val;
    uint16_t uint16_val;
    uint8_t uint8_val;

    switch (data_id) {
        case 25:
            float_val = get_float(response);
            update_diagnostic_value(&bm->diagnostics.t_boiler, float_val, float_val > 0);
            publish_diag_value("tboiler", "Boiler Temperature", "C", &bm->diagnostics.t_boiler);
            break;
        case 28:
            float_val = get_float(response);
            update_diagnostic_value(&bm->diagnostics.t_return, float_val, true);
            publish_diag_value("treturn", "Return Temperature", "C", &bm->diagnostics.t_return);
            break;
        case 26:
            float_val = get_float(response);
            update_diagnostic_value(&bm->diagnostics.t_dhw, float_val, float_val > 0);
            break;
        case 32:
            float_val = get_float(response);
            update_diagnostic_value(&bm->diagnostics.t_dhw2, float_val, float_val > 0);
            break;
        case 27:
            float_val = get_float(response);
            update_diagnostic_value(&bm->diagnostics.t_outside, float_val, true);
            break;
        case 33:
            float_val = (float)((int16_t)get_data_value(response));
            update_diagnostic_value(&bm->diagnostics.t_exhaust, float_val, float_val > -40 && float_val < 500);
            publish_diag_value("texhaust", "Exhaust Temperature", "C", &bm->diagnostics.t_exhaust);
            break;
        case 34:
            float_val = (float)((int16_t)get_data_value(response));
            update_diagnostic_value(&bm->diagnostics.t_heat_exchanger, float_val, float_val > 0);
            break;
        case 31:
            float_val = get_float(response);
            update_diagnostic_value(&bm->diagnostics.t_flow_ch2, float_val, float_val > 0);
            break;
        case 29:
            float_val = get_float(response);
            update_diagnostic_value(&bm->diagnostics.t_storage, float_val, float_val > 0);
            break;
        case 30:
            float_val = get_float(response);
            update_diagnostic_value(&bm->diagnostics.t_collector, float_val, float_val > 0);
            break;
        case 1:
            float_val = get_float(response);
            update_diagnostic_value(&bm->diagnostics.t_setpoint, float_val, float_val > 0 && float_val < 100);
            publish_diag_value("tset", "Boiler Setpoint", "C", &bm->diagnostics.t_setpoint);
            break;
        case 17:
            float_val = get_float(response);
            update_diagnostic_value(&bm->diagnostics.modulation_level, float_val, float_val >= 0 && float_val <= 100);
            publish_diag_value("modulation", "Modulation Level", "%", &bm->diagnostics.modulation_level);
            break;
        case 18:
            float_val = get_float(response);
            update_diagnostic_value(&bm->diagnostics.pressure, float_val, float_val >= 0);
            publish_diag_value("pressure", "CH Pressure", "bar", &bm->diagnostics.pressure);
            break;
        case 19:
            float_val = get_float(response);
            update_diagnostic_value(&bm->diagnostics.flow_rate, float_val, float_val >= 0);
            break;
        case 5:
            uint8_val = get_lb(response);
            update_diagnostic_value(&bm->diagnostics.fault_code, (float)uint8_val, true);
            publish_diag_value("fault", "Fault Code", "", &bm->diagnostics.fault_code);
            break;
        case 115:
            uint16_val = get_data_value(response);
            update_diagnostic_value(&bm->diagnostics.diag_code, (float)uint16_val, true);
            break;
        case 116:
            uint16_val = get_data_value(response);
            update_diagnostic_value(&bm->diagnostics.burner_starts, (float)uint16_val, true);
            break;
        case 119:
            uint16_val = get_data_value(response);
            update_diagnostic_value(&bm->diagnostics.dhw_burner_starts, (float)uint16_val, true);
            break;
        case 117:
            uint16_val = get_data_value(response);
            update_diagnostic_value(&bm->diagnostics.ch_pump_starts, (float)uint16_val, true);
            break;
        case 118:
            uint16_val = get_data_value(response);
            update_diagnostic_value(&bm->diagnostics.dhw_pump_starts, (float)uint16_val, true);
            break;
        case 120:
            uint16_val = get_data_value(response);
            update_diagnostic_value(&bm->diagnostics.burner_hours, (float)uint16_val, true);
            break;
        case 123:
            uint16_val = get_data_value(response);
            update_diagnostic_value(&bm->diagnostics.dhw_burner_hours, (float)uint16_val, true);
            break;
        case 121:
            uint16_val = get_data_value(response);
            update_diagnostic_value(&bm->diagnostics.ch_pump_hours, (float)uint16_val, true);
            break;
        case 122:
            uint16_val = get_data_value(response);
            update_diagnostic_value(&bm->diagnostics.dhw_pump_hours, (float)uint16_val, true);
            break;
        case 15:
            uint8_val = get_lb(response);
            float_val = (float)get_hb(response);
            update_diagnostic_value(&bm->diagnostics.max_capacity, float_val, float_val > 0);
            update_diagnostic_value(&bm->diagnostics.min_mod_level, (float)uint8_val, true);
            break;
        case 35:
            uint8_val = get_lb(response);
            update_diagnostic_value(&bm->diagnostics.fan_current, (float)uint8_val, true);
            uint8_val = get_hb(response);
            update_diagnostic_value(&bm->diagnostics.fan_setpoint, (float)uint8_val, true);
            break;
        case 84:
            uint16_val = get_data_value(response);
            update_diagnostic_value(&bm->diagnostics.fan_exhaust_rpm, (float)uint16_val, true);
            break;
        case 85:
            uint16_val = get_data_value(response);
            update_diagnostic_value(&bm->diagnostics.fan_supply_rpm, (float)uint16_val, true);
            break;
        case 79:
            uint16_val = get_data_value(response);
            update_diagnostic_value(&bm->diagnostics.co2_exhaust, (float)uint16_val, true);
            break;
        default:
            break;
    }
}

// ============================================================================
// Main Loop Task
// ============================================================================

/**
 * Main loop task - runs the control logic
 *
 * This task is NON-BLOCKING - it polls queues with zero timeout.
 * All blocking RMT operations are in the thermostat/boiler threads.
 */
static void boiler_manager_task(void *arg)
{
    bm_context_t *ctx = (bm_context_t *)arg;
    boiler_manager_t *bm = &ctx->base;
    uint32_t request, response;

    ESP_LOGI(TAG, "Main loop started");

    while (1) {
        int64_t now_ms = esp_timer_get_time() / 1000;

        // 1. Check for new thermostat request (NON-BLOCKING)
        if (ot_thermostat_get_request(ctx->queues, &request)) {
            ESP_LOGD(TAG, "Got thermostat request: 0x%08lX", (unsigned long)request);
            log_message(bm, "REQUEST", OT_SOURCE_THERMOSTAT_BOILER, request);

            uint8_t data_id = get_data_id(request);

            // Refresh MQTT control state
            if (bm->control_enabled) {
                refresh_control_state(bm);
            }

            // Decision: What to do with this request?

            // CONTROL MODE: send synthetic responses for certain IDs
            if (bm->control_enabled && bm->control_active) {
                uint32_t resp_frame = 0;
                switch (data_id) {
                    case OT_MSGID_STATUS: {
                        uint16_t status = build_status_word(bm->demand_ch_enabled);
                        resp_frame = build_response(OT_MSGTYPE_READ_ACK, data_id, status);
                        break;
                    }
                    case 1: {
                        float tset = bm->demand_tset_c > 0 ? bm->demand_tset_c : bm->diagnostics.t_setpoint.value;
                        uint16_t v = float_to_f88(tset);
                        resp_frame = build_response(OT_MSGTYPE_READ_ACK, data_id, v);
                        break;
                    }
                    case 3:
                    case 17: {
                        resp_frame = build_response(OT_MSGTYPE_READ_ACK, data_id, 0);
                        break;
                    }
                    default:
                        break;
                }
                if (resp_frame != 0) {
                    log_message(bm, "RESPONSE", OT_SOURCE_THERMOSTAT_GATEWAY, resp_frame);
                    ot_thermostat_send_response(ctx->queues, resp_frame);
                    continue;  // Don't forward to boiler
                }
            }

            // DIAGNOSTIC INJECTION: Intercept every Nth ID=0 request
            if (data_id == OT_MSGID_STATUS && bm->mode == BOILER_MANAGER_MODE_PROXY) {
                bm->id0_frame_counter++;
                if (bm->id0_frame_counter >= bm->intercept_rate) {
                    bm->id0_frame_counter = 0;

                    // Send diagnostic query instead of forwarding
                    const boiler_diagnostic_cmd_t *cmd = &bm->diag_commands[bm->diag_commands_index];
                    bm->diag_commands_index = (bm->diag_commands_index + 1) % bm->diag_commands_count;

                    uint32_t diag_request = build_request(OT_MSGTYPE_READ_DATA, cmd->data_id, 0);
                    ESP_LOGI(TAG, "Intercepting for diagnostic: %s (ID=%d)", cmd->name, cmd->data_id);
                    log_message(bm, "REQUEST", OT_SOURCE_GATEWAY_BOILER, diag_request);

                    ot_boiler_send_request(ctx->queues, diag_request);
                    ctx->state = BM_STATE_WAIT_DIAG_RESP;
                    ctx->pending_request = request;  // Save original for later
                    continue;  // Don't send thermostat response yet
                }
            }

            // NORMAL PASSTHROUGH: Forward request to boiler
            ESP_LOGD(TAG, "Forwarding request to boiler");
            ot_boiler_send_request(ctx->queues, request);
            ctx->state = BM_STATE_WAIT_BOILER_RESP;
            ctx->pending_passthrough = true;
        }

        // 2. Check for boiler response (NON-BLOCKING)
        if (ot_boiler_get_response(ctx->queues, &response)) {
            ESP_LOGD(TAG, "Got boiler response: 0x%08lX", (unsigned long)response);
            log_message(bm, "RESPONSE", OT_SOURCE_THERMOSTAT_BOILER, response);

            if (ctx->state == BM_STATE_WAIT_DIAG_RESP) {
                // This is a diagnostic response
                uint8_t diag_id = get_data_id(response);
                if (check_parity(response)) {
                    parse_diagnostic_response(bm, diag_id, response);
                    ESP_LOGD(TAG, "Diagnostic response parsed: ID=%d", diag_id);
                }

                // Thermostat will retry its request - we don't forward anything
                ctx->state = BM_STATE_IDLE;
            }
            else if (ctx->state == BM_STATE_WAIT_BOILER_RESP && ctx->pending_passthrough) {
                // Normal passthrough response - forward to thermostat
                ot_thermostat_send_response(ctx->queues, response);
                ctx->pending_passthrough = false;
                ctx->state = BM_STATE_IDLE;
            }
            else {
                // Unexpected response - still forward it
                ot_thermostat_send_response(ctx->queues, response);
                ctx->state = BM_STATE_IDLE;
            }
        }

        // 3. Handle manual writes (check if pending)
        if (bm->manual_write_pending && ctx->state == BM_STATE_IDLE) {
            ESP_LOGI(TAG, "Processing manual write: 0x%08lX", (unsigned long)bm->manual_write_frame);
            log_message(bm, "REQUEST", OT_SOURCE_GATEWAY_BOILER, bm->manual_write_frame);

            ot_boiler_send_request(ctx->queues, bm->manual_write_frame);

            // Wait for response (blocking in this case - but it's a manual operation)
            int wait_count = 0;
            while (wait_count < 100) {  // ~1 second max wait
                if (ot_boiler_get_response(ctx->queues, &response)) {
                    log_message(bm, "RESPONSE", OT_SOURCE_GATEWAY_BOILER, response);

                    if (check_parity(response)) {
                        ot_msg_type_t resp_type = get_msg_type(response);
                        if (resp_type == OT_MSGTYPE_WRITE_ACK) {
                            bm->manual_write_result = ESP_OK;
                        } else {
                            bm->manual_write_result = ESP_ERR_INVALID_RESPONSE;
                        }
                        bm->manual_write_response = response;
                    } else {
                        bm->manual_write_result = ESP_ERR_INVALID_CRC;
                    }
                    break;
                }
                vTaskDelay(pdMS_TO_TICKS(10));
                wait_count++;
            }

            if (wait_count >= 100) {
                bm->manual_write_result = ESP_ERR_TIMEOUT;
            }

            bm->manual_write_pending = false;
            if (bm->manual_write_sem) {
                xSemaphoreGive(bm->manual_write_sem);
            }
        }

        // 4. Periodic diagnostics polling (control mode)
        if (bm->control_enabled && bm->control_active && ctx->state == BM_STATE_IDLE) {
            if (now_ms - ctx->last_diag_poll_ms >= 1000) {
                const boiler_diagnostic_cmd_t *cmd = &bm->diag_commands[bm->diag_commands_index];
                bm->diag_commands_index = (bm->diag_commands_index + 1) % bm->diag_commands_count;

                uint32_t diag_request = build_request(OT_MSGTYPE_READ_DATA, cmd->data_id, 0);
                log_message(bm, "REQUEST", OT_SOURCE_GATEWAY_BOILER, diag_request);

                ot_boiler_send_request(ctx->queues, diag_request);
                ctx->state = BM_STATE_WAIT_DIAG_RESP;
                ctx->last_diag_poll_ms = now_ms;
            }
        }

        // Small delay to prevent busy-loop
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

// ============================================================================
// Public API
// ============================================================================

esp_err_t boiler_manager_init(boiler_manager_t *bm, boiler_manager_mode_t mode,
                              ot_handle_t *ot_unused, uint32_t intercept_rate)
{
    (void)ot_unused;  // No longer used in queue-based design

    if (!bm) return ESP_ERR_INVALID_ARG;

    memset(bm, 0, sizeof(boiler_manager_t));
    bm->mode = mode;
    bm->control_enabled = (mode == BOILER_MANAGER_MODE_CONTROL);
    bm->diag_commands = diag_commands;
    bm->diag_commands_count = DIAG_COMMANDS_COUNT;
    bm->intercept_rate = (intercept_rate == 0) ? 10 : intercept_rate;

    bm->manual_write_sem = xSemaphoreCreateBinary();
    if (!bm->manual_write_sem) {
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "Initialized in %s mode, intercept rate: 1/%d",
             mode == BOILER_MANAGER_MODE_PROXY ? "PROXY" :
             (mode == BOILER_MANAGER_MODE_CONTROL ? "CONTROL" : "PASSTHROUGH"),
             bm->intercept_rate);

    return ESP_OK;
}

esp_err_t boiler_manager_start(boiler_manager_t *bm, ot_queues_t *queues,
                               uint32_t stack_size, UBaseType_t priority)
{
    if (!bm || !queues) return ESP_ERR_INVALID_ARG;

    // Allocate extended context
    bm_context_t *ctx = calloc(1, sizeof(bm_context_t));
    if (!ctx) return ESP_ERR_NO_MEM;

    memcpy(&ctx->base, bm, sizeof(boiler_manager_t));
    ctx->queues = queues;
    ctx->state = BM_STATE_IDLE;
    s_ctx = ctx;

    // Create main loop task
    BaseType_t ret = xTaskCreate(
        boiler_manager_task,
        "bm_main",
        stack_size > 0 ? stack_size : 4096,
        ctx,
        priority > 0 ? priority : 5,
        NULL
    );

    if (ret != pdPASS) {
        free(ctx);
        s_ctx = NULL;
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Main loop task started");
    return ESP_OK;
}

// Keep the rest of the public API compatible
const boiler_diagnostics_t* boiler_manager_get_diagnostics(boiler_manager_t *bm)
{
    if (!bm) return NULL;
    return &bm->diagnostics;
}

esp_err_t boiler_manager_write_data(boiler_manager_t *bm, uint8_t data_id,
                                    uint16_t data_value, uint32_t *response_frame)
{
    if (!bm) return ESP_ERR_INVALID_ARG;
    if (bm->manual_write_pending) return ESP_ERR_INVALID_STATE;

    uint32_t request = build_request(OT_MSGTYPE_WRITE_DATA, data_id, data_value);

    xSemaphoreTake(bm->manual_write_sem, 0);
    bm->manual_write_frame = request;
    bm->manual_write_pending = true;
    bm->manual_write_result = ESP_FAIL;

    if (xSemaphoreTake(bm->manual_write_sem, pdMS_TO_TICKS(2000)) == pdTRUE) {
        if (response_frame) *response_frame = bm->manual_write_response;
        return bm->manual_write_result;
    }

    bm->manual_write_pending = false;
    return ESP_ERR_TIMEOUT;
}

void boiler_manager_set_control_enabled(boiler_manager_t *bm, bool enabled)
{
    if (!bm) return;
    bm->control_enabled = enabled;
}

void boiler_manager_get_status(boiler_manager_t *bm, boiler_manager_status_t *out)
{
    if (!bm || !out) return;
    refresh_control_state(bm);
    out->control_enabled = bm->control_enabled;
    out->control_active = bm->control_active;
    out->fallback_active = bm->fallback_active;
    out->demand_tset_c = bm->demand_tset_c;
    out->demand_ch_enabled = bm->demand_ch_enabled;
    out->last_demand_ms = bm->last_demand_ms;
}

void boiler_manager_set_mode(boiler_manager_t *bm, boiler_manager_mode_t mode)
{
    if (!bm) return;
    bm->mode = mode;
}

void boiler_manager_set_message_callback(boiler_manager_t *bm,
                                         boiler_manager_message_callback_t callback,
                                         void *user_data)
{
    if (!bm) return;
    bm->message_callback = callback;
    bm->message_callback_user_data = user_data;
}
