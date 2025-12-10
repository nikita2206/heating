/*
 * OpenTherm Library Backend Implementation
 * 
 * Uses the ESP-IDF ported OpenTherm library (based on ihormelnyk/opentherm_library)
 * to implement the generic OpenTherm API.
 */

#include "opentherm_api.h"
#include "OpenTherm.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "OT_LIB";

// Context structure holding two OpenTherm instances
struct ot_context {
    // Thermostat side (slave mode - receives requests, sends responses)
    OpenTherm *thermostat_if;
    int thermostat_in_pin;
    int thermostat_out_pin;
    
    // Boiler side (master mode - sends requests, receives responses)
    OpenTherm *boiler_if;
    int boiler_in_pin;
    int boiler_out_pin;
    
    // State
    volatile bool request_pending;
    volatile uint32_t pending_request;
    volatile bool timeout_flag;
    
    // Statistics
    uint32_t tx_count;
    uint32_t rx_count;
    uint32_t error_count;
    uint32_t timeout_count;
    
    // Callbacks
    ot_message_callback_t message_callback;
    void *message_callback_data;
    ot_request_interceptor_t request_interceptor;
    void *interceptor_data;
    
    // Synchronization
    SemaphoreHandle_t mutex;
};

// Interrupt handlers (must be static with C linkage)
static ot_context *g_ctx = NULL;  // Global pointer for ISR access

static void IRAM_ATTR thermostat_interrupt_handler() {
    if (g_ctx && g_ctx->thermostat_if) {
        g_ctx->thermostat_if->handleInterrupt();
    }
}

static void IRAM_ATTR boiler_interrupt_handler() {
    if (g_ctx && g_ctx->boiler_if) {
        g_ctx->boiler_if->handleInterrupt();
    }
}

// Response callback for boiler side
static void boiler_response_callback(unsigned long response, OpenThermResponseStatus status) {
    // This is called when boiler responds
    // In gateway mode, we handle this in ot_process()
}

ot_handle_t* ot_init(ot_pin_config_t config) {
    ESP_LOGI(TAG, "Initializing OpenTherm Library backend (ESP-IDF native)");
    ESP_LOGI(TAG, "Thermostat side (SLAVE): RX=GPIO%d, TX=GPIO%d", 
             config.thermostat_in_pin, config.thermostat_out_pin);
    ESP_LOGI(TAG, "Boiler side (MASTER): RX=GPIO%d, TX=GPIO%d", 
             config.boiler_in_pin, config.boiler_out_pin);
    
    ot_context *ctx = (ot_context*)malloc(sizeof(ot_context));
    if (!ctx) {
        ESP_LOGE(TAG, "Failed to allocate context");
        return NULL;
    }
    
    memset(ctx, 0, sizeof(ot_context));
    
    ctx->thermostat_in_pin = config.thermostat_in_pin;
    ctx->thermostat_out_pin = config.thermostat_out_pin;
    ctx->boiler_in_pin = config.boiler_in_pin;
    ctx->boiler_out_pin = config.boiler_out_pin;
    
    ctx->mutex = xSemaphoreCreateMutex();
    if (!ctx->mutex) {
        ESP_LOGE(TAG, "Failed to create mutex");
        free(ctx);
        return NULL;
    }
    
    // Create OpenTherm instances
    // Thermostat side: slave mode (isSlave=true)
    ctx->thermostat_if = new OpenTherm(config.thermostat_in_pin, 
                                       config.thermostat_out_pin, 
                                       true);  // isSlave=true
    
    // Boiler side: master mode (isSlave=false)
    ctx->boiler_if = new OpenTherm(config.boiler_in_pin, 
                                   config.boiler_out_pin, 
                                   false);  // isSlave=false
    
    if (!ctx->thermostat_if || !ctx->boiler_if) {
        ESP_LOGE(TAG, "Failed to create OpenTherm instances");
        if (ctx->thermostat_if) delete ctx->thermostat_if;
        if (ctx->boiler_if) delete ctx->boiler_if;
        vSemaphoreDelete(ctx->mutex);
        free(ctx);
        return NULL;
    }
    
    // Set global context for ISR handlers
    g_ctx = ctx;
    
    ESP_LOGI(TAG, "OpenTherm Library backend initialized");
    return ctx;
}

esp_err_t ot_start(ot_handle_t* handle) {
    if (!handle) return ESP_ERR_INVALID_ARG;
    
    ESP_LOGI(TAG, "Starting OpenTherm communication");
    
    // Begin thermostat interface (slave mode)
    handle->thermostat_if->begin(thermostat_interrupt_handler);
    
    // Begin boiler interface (master mode) with response callback
    handle->boiler_if->begin(boiler_interrupt_handler, boiler_response_callback);
    
    ESP_LOGI(TAG, "OpenTherm communication started");
    return ESP_OK;
}

esp_err_t ot_stop(ot_handle_t* handle) {
    if (!handle) return ESP_ERR_INVALID_ARG;
    
    handle->thermostat_if->end();
    handle->boiler_if->end();
    
    return ESP_OK;
}

void ot_deinit(ot_handle_t* handle) {
    if (!handle) return;
    
    ot_stop(handle);
    
    if (handle->thermostat_if) delete handle->thermostat_if;
    if (handle->boiler_if) delete handle->boiler_if;
    if (handle->mutex) vSemaphoreDelete(handle->mutex);
    
    if (g_ctx == handle) {
        g_ctx = NULL;
    }
    
    free(handle);
}

void ot_reset(ot_handle_t* handle) {
    if (!handle) return;
    
    xSemaphoreTake(handle->mutex, portMAX_DELAY);
    handle->request_pending = false;
    handle->timeout_flag = false;
    xSemaphoreGive(handle->mutex);
}

bool ot_process(ot_handle_t* handle, ot_message_t* request, ot_message_t* response) {
    if (!handle) return false;
    
    bool transaction_complete = false;
    
    // Process both interfaces
    handle->thermostat_if->process();
    handle->boiler_if->process();
    
    xSemaphoreTake(handle->mutex, portMAX_DELAY);
    
    // Check if thermostat sent a request
    if (handle->thermostat_if->isReady() && !handle->request_pending) {
        // In slave mode, check if we received a request
        OpenThermStatus status = handle->thermostat_if->status;
        if (status == OpenThermStatus::RESPONSE_READY) {
            unsigned long req = handle->thermostat_if->getLastResponse();
            OpenThermResponseStatus req_status = handle->thermostat_if->getLastResponseStatus();
            
            if (req_status == OpenThermResponseStatus::SUCCESS) {
                handle->pending_request = req;
                handle->request_pending = true;
                handle->rx_count++;
                
                ESP_LOGD(TAG, "Received request from thermostat: 0x%08lX", req);
                
                // Call message callback
                if (handle->message_callback) {
                    ot_message_t msg = {req};
                    handle->message_callback(handle, &msg, OT_ROLE_MASTER, handle->message_callback_data);
                }
            }
        }
    }
    
    // If we have a pending request, forward it to boiler
    if (handle->request_pending && handle->boiler_if->isReady()) {
        bool should_forward = true;
        
        // Check interceptor
        if (handle->request_interceptor) {
            ot_message_t msg = {handle->pending_request};
            should_forward = !handle->request_interceptor(handle, &msg, handle->interceptor_data);
        }
        
        if (should_forward) {
            ESP_LOGD(TAG, "Forwarding request to boiler: 0x%08lX", handle->pending_request);
            
            // Send request to boiler and wait for response
            unsigned long boiler_response = handle->boiler_if->sendRequest(handle->pending_request);
            OpenThermResponseStatus resp_status = handle->boiler_if->getLastResponseStatus();
            
            handle->tx_count++;
            
            if (resp_status == OpenThermResponseStatus::SUCCESS) {
                handle->rx_count++;
                
                ESP_LOGD(TAG, "Received response from boiler: 0x%08lX", boiler_response);
                
                // Call message callback
                if (handle->message_callback) {
                    ot_message_t msg = {boiler_response};
                    handle->message_callback(handle, &msg, OT_ROLE_SLAVE, handle->message_callback_data);
                }
                
                // Send response back to thermostat
                handle->thermostat_if->sendResponse(boiler_response);
                handle->tx_count++;
                
                // Fill output buffers
                if (request) request->data = handle->pending_request;
                if (response) response->data = boiler_response;
                
                transaction_complete = true;
                handle->timeout_flag = false;
                
            } else if (resp_status == OpenThermResponseStatus::TIMEOUT) {
                ESP_LOGW(TAG, "Boiler response timeout");
                handle->timeout_count++;
                handle->timeout_flag = true;
                handle->error_count++;
                
                // Send error response to thermostat
                unsigned long error_resp = OpenTherm::buildResponse(
                    OpenThermMessageType::DATA_INVALID,
                    (OpenThermMessageID)((handle->pending_request >> 16) & 0xFF),
                    0);
                handle->thermostat_if->sendResponse(error_resp);
                handle->tx_count++;
                
            } else {
                ESP_LOGE(TAG, "Boiler response error");
                handle->error_count++;
                
                // Send error response to thermostat
                unsigned long error_resp = OpenTherm::buildResponse(
                    OpenThermMessageType::DATA_INVALID,
                    (OpenThermMessageID)((handle->pending_request >> 16) & 0xFF),
                    0);
                handle->thermostat_if->sendResponse(error_resp);
                handle->tx_count++;
            }
        }
        
        handle->request_pending = false;
    }
    
    xSemaphoreGive(handle->mutex);
    
    return transaction_complete;
}

void ot_get_stats(ot_handle_t* handle, ot_stats_t* stats) {
    if (!handle || !stats) return;
    
    xSemaphoreTake(handle->mutex, portMAX_DELAY);
    stats->tx_count = handle->tx_count;
    stats->rx_count = handle->rx_count;
    stats->error_count = handle->error_count;
    stats->timeout_count = handle->timeout_count;
    xSemaphoreGive(handle->mutex);
}

bool ot_get_timeout_flag(ot_handle_t* handle) {
    if (!handle) return false;
    
    bool flag;
    xSemaphoreTake(handle->mutex, portMAX_DELAY);
    flag = handle->timeout_flag;
    xSemaphoreGive(handle->mutex);
    
    return flag;
}

void ot_set_message_callback(ot_handle_t* handle, ot_message_callback_t callback, void *user_data) {
    if (!handle) return;
    
    xSemaphoreTake(handle->mutex, portMAX_DELAY);
    handle->message_callback = callback;
    handle->message_callback_data = user_data;
    xSemaphoreGive(handle->mutex);
}

void ot_set_request_interceptor(ot_handle_t* handle, ot_request_interceptor_t interceptor, void *user_data) {
    if (!handle) return;
    
    xSemaphoreTake(handle->mutex, portMAX_DELAY);
    handle->request_interceptor = interceptor;
    handle->interceptor_data = user_data;
    xSemaphoreGive(handle->mutex);
}

// ============================================================================
// Message Construction/Parsing Utilities
// ============================================================================

uint32_t ot_build_request(ot_message_type_t type, uint8_t id, uint16_t data) {
    return OpenTherm::buildRequest((OpenThermMessageType)type, (OpenThermMessageID)id, data);
}

uint32_t ot_build_response(ot_message_type_t type, uint8_t id, uint16_t data) {
    return OpenTherm::buildResponse((OpenThermMessageType)type, (OpenThermMessageID)id, data);
}

ot_message_type_t ot_get_message_type(uint32_t message) {
    return (ot_message_type_t)OpenTherm::getMessageType(message);
}

uint8_t ot_get_data_id(uint32_t message) {
    return (uint8_t)OpenTherm::getDataID(message);
}

uint16_t ot_get_uint16(uint32_t message) {
    return OpenTherm::getUInt(message);
}

float ot_get_float(uint32_t message) {
    return OpenTherm::getFloat(message);
}

uint8_t ot_get_uint8_hb(uint32_t message) {
    return (message >> 8) & 0xFF;
}

uint8_t ot_get_uint8_lb(uint32_t message) {
    return message & 0xFF;
}

bool ot_check_parity(uint32_t frame) {
    return OpenTherm::parity(frame);
}

bool ot_is_valid_response(uint32_t request, uint32_t response) {
    // Check if response data ID matches request data ID
    uint8_t req_id = ot_get_data_id(request);
    uint8_t resp_id = ot_get_data_id(response);
    return (req_id == resp_id) && OpenTherm::isValidResponse(response);
}

const char* ot_message_type_to_string(ot_message_type_t type) {
    return OpenTherm::messageTypeToString((OpenThermMessageType)type);
}

// ============================================================================
// Advanced: Direct Message Sending
// ============================================================================

esp_err_t ot_send_request_to_boiler(ot_handle_t* handle, ot_message_t request, 
                                     ot_message_t* response, uint32_t timeout_ms) {
    if (!handle) return ESP_ERR_INVALID_ARG;
    
    xSemaphoreTake(handle->mutex, portMAX_DELAY);
    
    // Check if we're busy with a pending request
    if (handle->request_pending) {
        xSemaphoreGive(handle->mutex);
        ESP_LOGW(TAG, "Cannot send out-of-band request: gateway is busy");
        return ESP_ERR_INVALID_STATE;
    }
    
    // Check if boiler interface is ready
    if (!handle->boiler_if->isReady()) {
        xSemaphoreGive(handle->mutex);
        ESP_LOGW(TAG, "Cannot send out-of-band request: boiler interface not ready");
        return ESP_ERR_INVALID_STATE;
    }
    
    ESP_LOGD(TAG, "Sending out-of-band request to boiler: 0x%08lX", request.data);
    
    // Send request to boiler
    unsigned long boiler_response = handle->boiler_if->sendRequest(request.data);
    OpenThermResponseStatus resp_status = handle->boiler_if->getLastResponseStatus();
    
    handle->tx_count++;
    
    xSemaphoreGive(handle->mutex);
    
    if (resp_status == OpenThermResponseStatus::SUCCESS) {
        handle->rx_count++;
        ESP_LOGD(TAG, "Received out-of-band response from boiler: 0x%08lX", boiler_response);
        
        if (response) {
            response->data = boiler_response;
        }
        return ESP_OK;
        
    } else if (resp_status == OpenThermResponseStatus::TIMEOUT) {
        ESP_LOGW(TAG, "Out-of-band boiler request timeout");
        handle->timeout_count++;
        return ESP_ERR_TIMEOUT;
        
    } else {
        ESP_LOGE(TAG, "Out-of-band boiler request error: %s", 
                 OpenTherm::statusToString(resp_status));
        handle->error_count++;
        return ESP_FAIL;
    }
}

esp_err_t ot_send_response_to_thermostat(ot_handle_t* handle, ot_message_t response) {
    if (!handle) return ESP_ERR_INVALID_ARG;
    
    xSemaphoreTake(handle->mutex, portMAX_DELAY);
    
    // Check if thermostat interface is ready
    if (!handle->thermostat_if->isReady()) {
        xSemaphoreGive(handle->mutex);
        ESP_LOGW(TAG, "Cannot send response to thermostat: interface not ready");
        return ESP_ERR_INVALID_STATE;
    }
    
    ESP_LOGD(TAG, "Sending direct response to thermostat: 0x%08lX", response.data);
    
    // Send response to thermostat
    bool success = handle->thermostat_if->sendResponse(response.data);
    
    if (success) {
        handle->tx_count++;
        xSemaphoreGive(handle->mutex);
        return ESP_OK;
    } else {
        handle->error_count++;
        xSemaphoreGive(handle->mutex);
        ESP_LOGE(TAG, "Failed to send response to thermostat");
        return ESP_FAIL;
    }
}

