/*
 * OpenTherm RMT Backend Implementation
 * 
 * Uses the RMT peripheral-based implementation to provide
 * the generic OpenTherm API.
 */

#include "opentherm_api.h"
#include "opentherm_rmt.h"
#include "esp_log.h"
#include <stdlib.h>
#include <string.h>

static const char *TAG = "OT_RMT";

// Context structure wrapping OpenThermRmt
// Note: ot_context_t is typedef'd as "struct ot_context" in opentherm_api.h
struct ot_context {
    OpenThermRmt rmt;
    
    // Callbacks
    ot_message_callback_t message_callback;
    void *message_callback_data;
    ot_request_interceptor_t request_interceptor;
    void *interceptor_data;
};

// Wrapper for message callback
static void rmt_message_callback_wrapper(OpenThermRmt *ot, OpenThermRmtMessage *message, OpenThermRmtRole from_role) {
    if (!ot) return;
    
    // Find the context that owns this OpenThermRmt instance
    struct ot_context *ctx = (struct ot_context*)((char*)ot - offsetof(struct ot_context, rmt));
    
    if (ctx->message_callback) {
        ot_message_t msg = {message->data};
        ot_role_t role = (from_role == OT_RMT_ROLE_MASTER) ? OT_ROLE_MASTER : OT_ROLE_SLAVE;
        ctx->message_callback(ctx, &msg, role, ctx->message_callback_data);
    }
}

// Wrapper for request interceptor
static bool rmt_request_interceptor_wrapper(OpenThermRmt *ot, OpenThermRmtMessage *request) {
    if (!ot) return false;
    
    // Find the context that owns this OpenThermRmt instance
    struct ot_context *ctx = (struct ot_context*)((char*)ot - offsetof(struct ot_context, rmt));
    
    if (ctx->request_interceptor) {
        ot_message_t msg = {request->data};
        return ctx->request_interceptor(ctx, &msg, ctx->interceptor_data);
    }
    
    return false;
}

ot_handle_t* ot_init(ot_pin_config_t config) {
    ESP_LOGI(TAG, "Initializing OpenTherm RMT backend");
    ESP_LOGI(TAG, "Thermostat side: RX=GPIO%d, TX=GPIO%d", 
             config.thermostat_in_pin, config.thermostat_out_pin);
    ESP_LOGI(TAG, "Boiler side: RX=GPIO%d, TX=GPIO%d", 
             config.boiler_in_pin, config.boiler_out_pin);
    
    struct ot_context *ctx = (struct ot_context*)malloc(sizeof(struct ot_context));
    if (!ctx) {
        ESP_LOGE(TAG, "Failed to allocate context");
        return NULL;
    }
    
    memset(ctx, 0, sizeof(struct ot_context));
    
    // Initialize OpenTherm RMT in gateway mode
    esp_err_t ret = opentherm_rmt_init_gateway(&ctx->rmt,
                                                config.thermostat_in_pin,
                                                config.thermostat_out_pin,
                                                config.boiler_in_pin,
                                                config.boiler_out_pin);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize OpenTherm RMT gateway: %s", esp_err_to_name(ret));
        free(ctx);
        return NULL;
    }
    
    ESP_LOGI(TAG, "OpenTherm RMT backend initialized");
    return ctx;
}

esp_err_t ot_start(ot_handle_t* handle) {
    if (!handle) return ESP_ERR_INVALID_ARG;
    
    ESP_LOGI(TAG, "Starting OpenTherm RMT");
    
    esp_err_t ret = opentherm_rmt_start(&handle->rmt);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start OpenTherm RMT: %s", esp_err_to_name(ret));
        return ret;
    }
    
    opentherm_rmt_gateway_reset(&handle->rmt);
    
    ESP_LOGI(TAG, "OpenTherm RMT started");
    return ESP_OK;
}

esp_err_t ot_stop(ot_handle_t* handle) {
    if (!handle) return ESP_ERR_INVALID_ARG;
    
    return opentherm_rmt_stop(&handle->rmt);
}

void ot_deinit(ot_handle_t* handle) {
    if (!handle) return;
    
    opentherm_rmt_deinit(&handle->rmt);
    free(handle);
}

void ot_reset(ot_handle_t* handle) {
    if (!handle) return;
    
    opentherm_rmt_gateway_reset(&handle->rmt);
}

bool ot_process(ot_handle_t* handle, ot_message_t* request, ot_message_t* response) {
    if (!handle) return false;
    
    OpenThermRmtMessage req, resp;
    bool result = opentherm_rmt_gateway_process(&handle->rmt, &req, &resp);
    
    if (result) {
        if (request) request->data = req.data;
        if (response) response->data = resp.data;
    }
    
    return result;
}

void ot_get_stats(ot_handle_t* handle, ot_stats_t* stats) {
    if (!handle || !stats) return;
    
    stats->tx_count = handle->rmt.tx_count;
    stats->rx_count = handle->rmt.rx_count;
    stats->error_count = handle->rmt.error_count;
    stats->timeout_count = handle->rmt.timeout_count;
}

bool ot_get_timeout_flag(ot_handle_t* handle) {
    if (!handle) return false;
    
    return handle->rmt.gateway_timeout_flag;
}

void ot_set_message_callback(ot_handle_t* handle, ot_message_callback_t callback, void *user_data) {
    if (!handle) return;
    
    handle->message_callback = callback;
    handle->message_callback_data = user_data;
    
    // Set the wrapper callback on the RMT instance
    if (callback) {
        opentherm_rmt_set_message_callback(&handle->rmt, rmt_message_callback_wrapper, NULL);
    } else {
        opentherm_rmt_set_message_callback(&handle->rmt, NULL, NULL);
    }
}

void ot_set_request_interceptor(ot_handle_t* handle, ot_request_interceptor_t interceptor, void *user_data) {
    if (!handle) return;
    
    handle->request_interceptor = interceptor;
    handle->interceptor_data = user_data;
    
    // Set the wrapper interceptor on the RMT instance
    if (interceptor) {
        opentherm_rmt_set_request_interceptor(&handle->rmt, rmt_request_interceptor_wrapper, NULL);
    } else {
        opentherm_rmt_set_request_interceptor(&handle->rmt, NULL, NULL);
    }
}

// ============================================================================
// Message Construction/Parsing Utilities
// ============================================================================

uint32_t ot_build_request(ot_message_type_t type, uint8_t id, uint16_t data) {
    return opentherm_rmt_build_request((OpenThermRmtMessageType)type, id, data);
}

uint32_t ot_build_response(ot_message_type_t type, uint8_t id, uint16_t data) {
    return opentherm_rmt_build_response((OpenThermRmtMessageType)type, id, data);
}

ot_message_type_t ot_get_message_type(uint32_t message) {
    return (ot_message_type_t)opentherm_rmt_get_message_type(message);
}

uint8_t ot_get_data_id(uint32_t message) {
    return opentherm_rmt_get_data_id(message);
}

uint16_t ot_get_uint16(uint32_t message) {
    return opentherm_rmt_get_uint16(message);
}

float ot_get_float(uint32_t message) {
    return opentherm_rmt_get_float(message);
}

uint8_t ot_get_uint8_hb(uint32_t message) {
    return opentherm_rmt_get_uint8_hb(message);
}

uint8_t ot_get_uint8_lb(uint32_t message) {
    return opentherm_rmt_get_uint8_lb(message);
}

bool ot_check_parity(uint32_t frame) {
    return opentherm_rmt_check_parity(frame);
}

bool ot_is_valid_response(uint32_t request, uint32_t response) {
    return opentherm_rmt_is_valid_response(request, response);
}

const char* ot_message_type_to_string(ot_message_type_t type) {
    return opentherm_rmt_message_type_to_string((OpenThermRmtMessageType)type);
}

// ============================================================================
// Advanced: Direct Message Sending
// ============================================================================

esp_err_t ot_send_request_to_boiler(ot_handle_t* handle, ot_message_t request, 
                                     ot_message_t* response, uint32_t timeout_ms) {
    if (!handle) return ESP_ERR_INVALID_ARG;
    
    ESP_LOGD(TAG, "Sending out-of-band request to boiler: 0x%08lX", request.data);
    
    // Send request directly to the boiler (secondary interface)
    esp_err_t ret = opentherm_rmt_send_frame(&handle->rmt, request.data, &handle->rmt.secondary);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to send out-of-band request to boiler: %s", esp_err_to_name(ret));
        return ret;
    }
    
    // Wait for response
    uint32_t response_frame = 0;
    ret = opentherm_rmt_receive_frame(&handle->rmt, &handle->rmt.secondary, &response_frame, timeout_ms);
    
    if (ret == ESP_OK) {
        ESP_LOGD(TAG, "Received out-of-band response from boiler: 0x%08lX", response_frame);
        if (response) {
            response->data = response_frame;
        }
    } else {
        ESP_LOGW(TAG, "Out-of-band boiler request timeout/error: %s", esp_err_to_name(ret));
    }
    
    return ret;
}

esp_err_t ot_send_response_to_thermostat(ot_handle_t* handle, ot_message_t response) {
    if (!handle) return ESP_ERR_INVALID_ARG;
    
    ESP_LOGD(TAG, "Sending direct response to thermostat: 0x%08lX", response.data);
    
    // Send response directly to the thermostat (primary interface)
    esp_err_t ret = opentherm_rmt_send_frame(&handle->rmt, response.data, &handle->rmt.primary);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to send response to thermostat: %s", esp_err_to_name(ret));
    }
    
    return ret;
}

