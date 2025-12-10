/*
 * OTA Update Support for OpenTherm Gateway
 */

#include "ota_update.h"
#include "web_ui.h"
#include "web_ui_pages.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_app_format.h"
#include "esp_partition.h"
#include "esp_flash_partitions.h"
#include "esp_system.h"
#include <string.h>
#include <inttypes.h>

static const char *TAG = "OTA";

// OTA state
static esp_ota_handle_t ota_handle = 0;
static const esp_partition_t *update_partition = NULL;
static bool ota_in_progress = false;
static size_t ota_bytes_written = 0;


/**
 * GET /ota - OTA management page
 */
static esp_err_t ota_page_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    char nav[256];
    char *page = malloc(12288);  // 12KB should be enough
    if (page) {
        web_ui_build_nav(nav, sizeof(nav), WEB_NAV_OTA);
        web_ui_render_page(page, 12288, "OTA Update - OpenTherm Gateway", WEB_UI_OTA_STYLES, nav, WEB_UI_OTA_BODY);
        httpd_resp_send(req, page, strlen(page));
        free(page);
        return ESP_OK;
    }
    return httpd_resp_send_500(req);
}

/**
 * POST /ota - Upload firmware binary
 * 
 * Expects raw binary data in request body.
 * Response: JSON with status
 */
static esp_err_t ota_upload_handler(httpd_req_t *req)
{
    esp_err_t err;
    char buf[4096];  // Larger buffer for efficient flash writes
    int received;
    int remaining = req->content_len;
    bool first_chunk = true;
    
    ESP_LOGI(TAG, "OTA update started, content length: %d", req->content_len);
    
    // Don't allow concurrent OTA updates
    if (ota_in_progress) {
        ESP_LOGW(TAG, "OTA already in progress");
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "OTA already in progress");
        return ESP_FAIL;
    }
    
    ota_in_progress = true;
    ota_bytes_written = 0;
    
    // Get the next OTA partition to write to
    update_partition = esp_ota_get_next_update_partition(NULL);
    if (update_partition == NULL) {
        ESP_LOGE(TAG, "No OTA partition found");
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No OTA partition available");
        ota_in_progress = false;
        return ESP_FAIL;
    }
    
    ESP_LOGI(TAG, "Writing to partition subtype %d at offset 0x%" PRIx32,
             update_partition->subtype, update_partition->address);
    
    // Receive and write firmware chunks
    while (remaining > 0) {
        int to_read = remaining > sizeof(buf) ? sizeof(buf) : remaining;
        
        received = httpd_req_recv(req, buf, to_read);
        if (received <= 0) {
            if (received == HTTPD_SOCK_ERR_TIMEOUT) {
                continue;  // Retry on timeout
            }
            ESP_LOGE(TAG, "File receive failed");
            if (ota_handle) {
                esp_ota_abort(ota_handle);
                ota_handle = 0;
            }
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to receive file");
            ota_in_progress = false;
            return ESP_FAIL;
        }
        
        // On first chunk, validate the image header and start OTA
        if (first_chunk) {
            first_chunk = false;
            
            // Validate header
            if (received < sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t) + sizeof(esp_app_desc_t)) {
                ESP_LOGE(TAG, "First chunk too small for header validation");
                httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid firmware image");
                ota_in_progress = false;
                return ESP_FAIL;
            }
            
            // Check new firmware version
            esp_app_desc_t new_app_info;
            memcpy(&new_app_info, 
                   buf + sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t),
                   sizeof(esp_app_desc_t));
            ESP_LOGI(TAG, "New firmware version: %s", new_app_info.version);
            
            // Log current version
            const esp_partition_t *running = esp_ota_get_running_partition();
            esp_app_desc_t running_app_info;
            if (esp_ota_get_partition_description(running, &running_app_info) == ESP_OK) {
                ESP_LOGI(TAG, "Running firmware version: %s", running_app_info.version);
            }
            
            // Check if this version was previously marked invalid
            const esp_partition_t *last_invalid = esp_ota_get_last_invalid_partition();
            if (last_invalid != NULL) {
                esp_app_desc_t invalid_app_info;
                if (esp_ota_get_partition_description(last_invalid, &invalid_app_info) == ESP_OK) {
                    if (memcmp(invalid_app_info.version, new_app_info.version, sizeof(new_app_info.version)) == 0) {
                        ESP_LOGW(TAG, "Rejecting firmware version %s - previously marked invalid", new_app_info.version);
                        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, 
                            "This firmware version was previously rejected after failed validation");
                        ota_in_progress = false;
                        return ESP_FAIL;
                    }
                }
            }
            
            // Start OTA
            err = esp_ota_begin(update_partition, OTA_WITH_SEQUENTIAL_WRITES, &ota_handle);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "esp_ota_begin failed: %s", esp_err_to_name(err));
                httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to begin OTA");
                ota_in_progress = false;
                return ESP_FAIL;
            }
            ESP_LOGI(TAG, "OTA begin succeeded");
        }
        
        // Write chunk to flash
        err = esp_ota_write(ota_handle, buf, received);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_ota_write failed: %s", esp_err_to_name(err));
            esp_ota_abort(ota_handle);
            ota_handle = 0;
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to write firmware");
            ota_in_progress = false;
            return ESP_FAIL;
        }
        
        ota_bytes_written += received;
        remaining -= received;
        
        // Log progress periodically
        if (ota_bytes_written % (64 * 1024) == 0 || remaining == 0) {
            ESP_LOGI(TAG, "Written %zu bytes, %d remaining", ota_bytes_written, remaining);
        }
    }
    
    // Finalize OTA
    err = esp_ota_end(ota_handle);
    ota_handle = 0;
    
    if (err != ESP_OK) {
        if (err == ESP_ERR_OTA_VALIDATE_FAILED) {
            ESP_LOGE(TAG, "Image validation failed");
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Image validation failed");
        } else {
            ESP_LOGE(TAG, "esp_ota_end failed: %s", esp_err_to_name(err));
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to finalize OTA");
        }
        ota_in_progress = false;
        return ESP_FAIL;
    }
    
    // Set boot partition
    err = esp_ota_set_boot_partition(update_partition);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition failed: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to set boot partition");
        ota_in_progress = false;
        return ESP_FAIL;
    }
    
    ESP_LOGI(TAG, "OTA update successful! Written %zu bytes. Preparing to restart...", ota_bytes_written);
    
    // Send success response
    httpd_resp_set_type(req, "application/json");
    char response[256];
    snprintf(response, sizeof(response),
             "{\"status\":\"success\",\"message\":\"OTA update complete, restarting...\",\"bytes_written\":%zu}",
             ota_bytes_written);
    httpd_resp_send(req, response, strlen(response));
    
    ota_in_progress = false;
    
    // Restart after a short delay to allow response to be sent
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
    
    return ESP_OK;
}

/**
 * GET /ota/status - Get OTA and firmware status
 */
static esp_err_t ota_status_handler(httpd_req_t *req)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *boot = esp_ota_get_boot_partition();
    const esp_partition_t *next_update = esp_ota_get_next_update_partition(NULL);
    
    esp_app_desc_t app_info;
    esp_ota_get_partition_description(running, &app_info);
    
    esp_ota_img_states_t ota_state;
    esp_ota_get_state_partition(running, &ota_state);
    
    const char *state_str = "unknown";
    switch (ota_state) {
        case ESP_OTA_IMG_NEW: state_str = "new"; break;
        case ESP_OTA_IMG_PENDING_VERIFY: state_str = "pending_verify"; break;
        case ESP_OTA_IMG_VALID: state_str = "valid"; break;
        case ESP_OTA_IMG_INVALID: state_str = "invalid"; break;
        case ESP_OTA_IMG_ABORTED: state_str = "aborted"; break;
        case ESP_OTA_IMG_UNDEFINED: state_str = "undefined"; break;
    }
    
    char response[512];
    snprintf(response, sizeof(response),
             "{"
             "\"version\":\"%s\","
             "\"project_name\":\"%s\","
             "\"compile_time\":\"%s %s\","
             "\"idf_ver\":\"%s\","
             "\"running_partition\":\"%s\","
             "\"running_offset\":\"0x%" PRIx32 "\","
             "\"boot_partition\":\"%s\","
             "\"next_update_partition\":\"%s\","
             "\"ota_state\":\"%s\","
             "\"ota_in_progress\":%s"
             "}",
             app_info.version,
             app_info.project_name,
             app_info.date, app_info.time,
             app_info.idf_ver,
             running->label,
             running->address,
             boot ? boot->label : "none",
             next_update ? next_update->label : "none",
             state_str,
             ota_in_progress ? "true" : "false");
    
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, response, strlen(response));
}

/**
 * POST /ota/rollback - Manually trigger rollback to previous firmware
 */
static esp_err_t ota_rollback_handler(httpd_req_t *req)
{
    if (!esp_ota_check_rollback_is_possible()) {
        ESP_LOGW(TAG, "Rollback not possible");
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Rollback not possible - no valid previous firmware");
        return ESP_FAIL;
    }
    
    ESP_LOGI(TAG, "Manual rollback requested, restarting...");
    
    httpd_resp_set_type(req, "application/json");
    const char *response = "{\"status\":\"success\",\"message\":\"Rolling back and restarting...\"}";
    httpd_resp_send(req, response, strlen(response));
    
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_ota_mark_app_invalid_rollback_and_reboot();
    
    return ESP_OK;
}

/**
 * POST /ota/confirm - Confirm current firmware is working (cancels rollback)
 */
static esp_err_t ota_confirm_handler(httpd_req_t *req)
{
    esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to confirm app: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to confirm firmware");
        return ESP_FAIL;
    }
    
    ESP_LOGI(TAG, "Firmware confirmed as valid");
    
    httpd_resp_set_type(req, "application/json");
    const char *response = "{\"status\":\"success\",\"message\":\"Firmware confirmed as valid\"}";
    return httpd_resp_send(req, response, strlen(response));
}

esp_err_t ota_update_register_handlers(httpd_handle_t server)
{
    ESP_LOGI(TAG, "Registering OTA HTTP handlers");
    
    // GET /ota - OTA management page
    httpd_uri_t ota_page_uri = {
        .uri = "/ota",
        .method = HTTP_GET,
        .handler = ota_page_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &ota_page_uri);
    
    // POST /ota - Upload firmware
    httpd_uri_t ota_upload = {
        .uri = "/ota",
        .method = HTTP_POST,
        .handler = ota_upload_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &ota_upload);
    
    // GET /ota/status - Get status
    httpd_uri_t ota_status = {
        .uri = "/ota/status",
        .method = HTTP_GET,
        .handler = ota_status_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &ota_status);
    
    // POST /ota/rollback - Manual rollback
    httpd_uri_t ota_rollback = {
        .uri = "/ota/rollback",
        .method = HTTP_POST,
        .handler = ota_rollback_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &ota_rollback);
    
    // POST /ota/confirm - Confirm firmware
    httpd_uri_t ota_confirm = {
        .uri = "/ota/confirm",
        .method = HTTP_POST,
        .handler = ota_confirm_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &ota_confirm);
    
    ESP_LOGI(TAG, "OTA handlers registered: GET/POST /ota, GET /ota/status, POST /ota/rollback, POST /ota/confirm");
    
    return ESP_OK;
}

esp_err_t ota_update_validate_app(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t ota_state;
    
    ESP_LOGI(TAG, "Running from partition: %s at offset 0x%" PRIx32, 
             running->label, running->address);
    
    if (esp_ota_get_state_partition(running, &ota_state) == ESP_OK) {
        if (ota_state == ESP_OTA_IMG_PENDING_VERIFY) {
            ESP_LOGI(TAG, "OTA state: pending verification");
            ESP_LOGI(TAG, "New firmware booted successfully, marking as valid...");
            
            // For this project, we auto-confirm after successful boot
            // In a more critical application, you might want to run diagnostics first
            esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Failed to mark app valid: %s", esp_err_to_name(err));
                return ESP_FAIL;
            }
            
            ESP_LOGI(TAG, "Firmware marked as valid, rollback cancelled");
        } else {
            ESP_LOGI(TAG, "OTA state: %d (not pending verification)", ota_state);
        }
    }
    
    return ESP_OK;
}

const char *ota_update_get_version(void)
{
    static esp_app_desc_t app_info;
    static bool initialized = false;
    
    if (!initialized) {
        const esp_partition_t *running = esp_ota_get_running_partition();
        esp_ota_get_partition_description(running, &app_info);
        initialized = true;
    }
    
    return app_info.version;
}

