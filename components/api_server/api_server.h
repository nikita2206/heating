/*
 * API Server for OpenTherm Message Logging (C++)
 */

#ifndef API_SERVER_H
#define API_SERVER_H

#include "esp_err.h"
#include "esp_http_server.h"
#include <stdint.h>

#ifdef __cplusplus
namespace ot {
    class BoilerManager;
    class MqttBridge;
}
#endif

#ifdef __cplusplus
extern "C" {
#endif

// API server handle
typedef struct {
    httpd_handle_t server;
    int client_fd;
    bool client_connected;
} api_server_t;

#ifdef __cplusplus
// Initialize and start API server (C++ version)
esp_err_t api_server_start(api_server_t *api_server, ot::BoilerManager *boiler_mgr);

// Set MQTT bridge instance
void api_server_set_mqtt(ot::MqttBridge *mqtt);
#endif

// Stop API server
void api_server_stop(api_server_t *api_server);

// Send text message to all connected clients
esp_err_t api_server_send_text(api_server_t *api_server, const char *text);

// Send JSON formatted OpenTherm message
esp_err_t api_server_send_opentherm_message(api_server_t *api_server,
                                                   const char *direction,
                                                   uint32_t message,
                                                   const char *msg_type,
                                                   uint8_t data_id,
                                                   uint16_t data_value,
                                                   const char *source);

// Get HTTP server handle (for registering additional handlers like OTA)
httpd_handle_t api_server_get_handle(api_server_t *api_server);

#ifdef __cplusplus
}
#endif

#endif // API_SERVER_H