/*
 * WebSocket Server for OpenTherm Message Logging
 */

#ifndef WEBSOCKET_SERVER_H
#define WEBSOCKET_SERVER_H

#include "esp_err.h"
#include "esp_http_server.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// WebSocket server handle
typedef struct {
    httpd_handle_t server;
    int client_fd;
    bool client_connected;
} websocket_server_t;

// Initialize and start WebSocket server
esp_err_t websocket_server_start(websocket_server_t *ws_server);

// Stop WebSocket server
void websocket_server_stop(websocket_server_t *ws_server);

// Send text message to all connected clients
esp_err_t websocket_server_send_text(websocket_server_t *ws_server, const char *text);

// Send JSON formatted OpenTherm message
esp_err_t websocket_server_send_opentherm_message(websocket_server_t *ws_server, 
                                                   const char *direction,
                                                   uint32_t message,
                                                   const char *msg_type,
                                                   uint8_t data_id,
                                                   uint16_t data_value);

#ifdef __cplusplus
}
#endif

#endif // WEBSOCKET_SERVER_H

