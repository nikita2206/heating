/*
 * WebSocket Server for OpenTherm Message Logging
 */

#include "websocket_server.h"
#include "esp_log.h"
#include "esp_http_server.h"
#include "esp_timer.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "WebSocket";

// HTML page served at root
static const char *html_page = 
    "<!DOCTYPE html><html><head><title>OpenTherm Gateway</title>"
    "<style>"
    "body{font-family:Arial,sans-serif;margin:20px;background:#f5f5f5}"
    ".container{max-width:1200px;margin:0 auto;background:white;padding:20px;border-radius:8px;box-shadow:0 2px 4px rgba(0,0,0,0.1)}"
    "h1{color:#333;border-bottom:2px solid #4CAF50;padding-bottom:10px}"
    ".status{padding:10px;margin:10px 0;border-radius:4px;font-weight:bold}"
    ".connected{background:#d4edda;color:#155724;border:1px solid #c3e6cb}"
    ".disconnected{background:#f8d7da;color:#721c24;border:1px solid #f5c6cb}"
    "#messages{height:500px;overflow-y:auto;border:1px solid #ddd;padding:10px;background:#fafafa;font-family:monospace;font-size:12px}"
    ".msg{padding:5px;margin:2px 0;border-left:3px solid #4CAF50;background:white;border-radius:2px}"
    ".msg.request{border-left-color:#2196F3}"
    ".msg.response{border-left-color:#FF9800}"
    ".msg.status{border-left-color:#9C27B0;background:#f3e5f5}"
    ".timestamp{color:#666;font-size:11px}"
    ".controls{margin:15px 0}"
    "button{padding:8px 16px;margin:5px;border:none;border-radius:4px;cursor:pointer;font-size:14px}"
    ".btn-clear{background:#f44336;color:white}"
    ".btn-clear:hover{background:#da190b}"
    "</style></head><body>"
    "<div class='container'>"
    "<h1>OpenTherm Gateway Monitor</h1>"
    "<div id='status' class='status disconnected'>Disconnected</div>"
    "<div class='controls'>"
    "<button class='btn-clear' onclick='clearMessages()'>Clear Messages</button>"
    "</div>"
    "<div id='messages'></div>"
    "</div>"
    "<script>"
    "let ws;let msgs=document.getElementById('messages');let status=document.getElementById('status');"
    "function connect(){"
    "ws=new WebSocket('ws://'+window.location.host+'/ws');"
    "ws.onopen=()=>{status.textContent='Connected';status.className='status connected'};"
    "ws.onclose=()=>{status.textContent='Disconnected';status.className='status disconnected';setTimeout(connect,2000)};"
    "ws.onmessage=(e)=>{"
    "let div=document.createElement('div');"
    "let ts=new Date().toLocaleTimeString();"
    "try{"
    "let data=JSON.parse(e.data);"
    "div.className='msg '+data.direction.toLowerCase();"
    "ts=new Date(data.timestamp).toLocaleTimeString();"
    "div.innerHTML='<span class=\"timestamp\">['+ts+']</span> <strong>'+data.direction+'</strong> '+data.msg_type+' | ID:'+data.data_id+' | Value:'+data.data_value+' (0x'+data.message.toString(16).toUpperCase()+')';"
    "}catch(err){"
    "div.className='msg status';"
    "div.innerHTML='<span class=\"timestamp\">['+ts+']</span> <strong>STATUS:</strong> '+e.data;"
    "}"
    "msgs.appendChild(div);msgs.scrollTop=msgs.scrollHeight;"
    "}}"
    "function clearMessages(){msgs.innerHTML='';}"
    "connect();"
    "</script></body></html>";

// HTTP GET handler for root
static esp_err_t root_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, html_page, strlen(html_page));
}

// WebSocket handler
static esp_err_t ws_handler(httpd_req_t *req)
{
    if (req->method == HTTP_GET) {
        ESP_LOGI(TAG, "WebSocket handshake");
        
        // Store client file descriptor immediately after handshake
        websocket_server_t *ws_server = (websocket_server_t *)req->user_ctx;
        if (ws_server) {
            ws_server->client_fd = httpd_req_to_sockfd(req);
            ws_server->client_connected = true;
            ESP_LOGI(TAG, "WebSocket client connected, fd=%d", ws_server->client_fd);
        }
        
        return ESP_OK;
    }
    
    httpd_ws_frame_t ws_pkt;
    memset(&ws_pkt, 0, sizeof(httpd_ws_frame_t));
    ws_pkt.type = HTTPD_WS_TYPE_TEXT;
    
    esp_err_t ret = httpd_ws_recv_frame(req, &ws_pkt, 0);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "httpd_ws_recv_frame failed: %s", esp_err_to_name(ret));
        return ret;
    }
    
    ESP_LOGI(TAG, "WebSocket frame received, len=%d", ws_pkt.len);
    
    return ESP_OK;
}

// Start WebSocket server
esp_err_t websocket_server_start(websocket_server_t *ws_server)
{
    memset(ws_server, 0, sizeof(websocket_server_t));
    
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_open_sockets = 7;
    config.uri_match_fn = httpd_uri_match_wildcard;
    
    ESP_LOGI(TAG, "Starting WebSocket server on port %d", config.server_port);
    
    if (httpd_start(&ws_server->server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server");
        return ESP_FAIL;
    }
    
    // Register root handler
    httpd_uri_t root_uri = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = root_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(ws_server->server, &root_uri);
    
    // Register WebSocket handler
    httpd_uri_t ws_uri = {
        .uri = "/ws",
        .method = HTTP_GET,
        .handler = ws_handler,
        .user_ctx = ws_server,
        .is_websocket = true
    };
    httpd_register_uri_handler(ws_server->server, &ws_uri);
    
    ESP_LOGI(TAG, "WebSocket server started successfully");
    return ESP_OK;
}

// Stop WebSocket server
void websocket_server_stop(websocket_server_t *ws_server)
{
    if (ws_server->server) {
        httpd_stop(ws_server->server);
        ws_server->server = NULL;
        ws_server->client_connected = false;
    }
}

// Send text message to connected client
esp_err_t websocket_server_send_text(websocket_server_t *ws_server, const char *text)
{
    if (!ws_server->server || !ws_server->client_connected) {
        ESP_LOGD(TAG, "Not sending WebSocket message: server not started or client not connected");
        return ESP_FAIL;
    }
    
    httpd_ws_frame_t ws_pkt;
    memset(&ws_pkt, 0, sizeof(httpd_ws_frame_t));
    ws_pkt.payload = (uint8_t *)text;
    ws_pkt.len = strlen(text);
    ws_pkt.type = HTTPD_WS_TYPE_TEXT;
    
    esp_err_t ret = httpd_ws_send_frame_async(ws_server->server, ws_server->client_fd, &ws_pkt);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to send WebSocket message: %s", esp_err_to_name(ret));
        ws_server->client_connected = false;
    } else {
        ESP_LOGI(TAG, "WebSocket message sent: %s", text);
    }
    
    return ret;
}

// Send formatted OpenTherm message
esp_err_t websocket_server_send_opentherm_message(websocket_server_t *ws_server,
                                                   const char *direction,
                                                   uint32_t message,
                                                   const char *msg_type,
                                                   uint8_t data_id,
                                                   uint16_t data_value)
{
    char json_buffer[512];
    int64_t timestamp = esp_timer_get_time() / 1000;  // Convert to milliseconds
    
    snprintf(json_buffer, sizeof(json_buffer),
             "{\"timestamp\":%lld,\"direction\":\"%s\",\"message\":%lu,\"msg_type\":\"%s\",\"data_id\":%u,\"data_value\":%u}",
             timestamp, direction, (unsigned long)message, msg_type, data_id, data_value);
    
    return websocket_server_send_text(ws_server, json_buffer);
}

