/*
 * OTA Update Support for OpenTherm Gateway
 *
 * Provides HTTP endpoints for firmware updates:
 *   POST /ota - Upload new firmware binary
 *   GET /ota/status - Get current firmware version and OTA status
 */

#ifndef OTA_UPDATE_H
#define OTA_UPDATE_H

#include "esp_err.h"
#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Register OTA HTTP handlers with an existing HTTP server
 * 
 * @param server HTTP server handle to register handlers with
 * @return ESP_OK on success
 */
esp_err_t ota_update_register_handlers(httpd_handle_t server);

/**
 * Check OTA state and handle rollback validation on boot
 * Should be called early in app_main() before other initialization
 * 
 * @return ESP_OK if app is valid, ESP_FAIL if rollback occurred
 */
esp_err_t ota_update_validate_app(void);

/**
 * Get firmware version string
 * 
 * @return Pointer to version string (static, do not free)
 */
const char *ota_update_get_version(void);

#ifdef __cplusplus
}
#endif

#endif // OTA_UPDATE_H

