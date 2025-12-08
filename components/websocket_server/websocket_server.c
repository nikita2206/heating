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

// Common CSS styles shared across pages
static const char *common_styles = 
    ":root{--bg:#0f0f13;--card:#1a1a24;--accent:#00d4aa;--accent2:#7c3aed;--text:#e4e4e7;--muted:#71717a;--border:#27272a}"
    "*{box-sizing:border-box;margin:0;padding:0}"
    "body{font-family:'SF Mono',Monaco,Consolas,monospace;background:var(--bg);color:var(--text);min-height:100vh}"
    ".container{max-width:1200px;margin:0 auto;padding:24px}"
    "nav{background:var(--card);border-bottom:1px solid var(--border);padding:16px 24px;display:flex;align-items:center;gap:32px}"
    ".logo{font-size:18px;font-weight:700;color:var(--accent);text-decoration:none;display:flex;align-items:center;gap:8px}"
    ".logo svg{width:24px;height:24px}"
    ".nav-links{display:flex;gap:24px}"
    ".nav-links a{color:var(--muted);text-decoration:none;font-size:14px;transition:color 0.2s}"
    ".nav-links a:hover,.nav-links a.active{color:var(--accent)}"
    "h1{font-size:28px;margin-bottom:8px;background:linear-gradient(135deg,var(--accent),var(--accent2));-webkit-background-clip:text;-webkit-text-fill-color:transparent}"
    ".subtitle{color:var(--muted);font-size:14px;margin-bottom:32px}"
    ".card{background:var(--card);border:1px solid var(--border);border-radius:12px;padding:24px;margin-bottom:24px}"
    ".card-title{font-size:16px;font-weight:600;margin-bottom:16px;display:flex;align-items:center;gap:8px}"
    ".badge{display:inline-flex;align-items:center;padding:4px 10px;border-radius:999px;font-size:12px;font-weight:500}"
    ".badge.green{background:rgba(0,212,170,0.15);color:var(--accent)}"
    ".badge.yellow{background:rgba(250,204,21,0.15);color:#facc15}"
    ".badge.red{background:rgba(239,68,68,0.15);color:#ef4444}"
    ".badge.purple{background:rgba(124,58,237,0.15);color:var(--accent2)}"
    "button,.btn{padding:10px 20px;border:none;border-radius:8px;cursor:pointer;font-size:14px;font-family:inherit;font-weight:500;transition:all 0.2s}"
    ".btn-primary{background:var(--accent);color:var(--bg)}"
    ".btn-primary:hover{background:#00e6b8;transform:translateY(-1px)}"
    ".btn-danger{background:#dc2626;color:white}"
    ".btn-danger:hover{background:#ef4444}"
    ".btn-secondary{background:var(--border);color:var(--text)}"
    ".btn-secondary:hover{background:#3f3f46}";

// Main dashboard page
static const char *dashboard_page = 
    "<!DOCTYPE html><html><head><meta charset='UTF-8'><meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>OpenTherm Gateway</title><style>%s"
    ".hero{text-align:center;padding:48px 24px}"
    ".hero h1{font-size:42px;margin-bottom:16px}"
    ".grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(300px,1fr));gap:24px;margin-top:32px}"
    ".feature-card{background:var(--card);border:1px solid var(--border);border-radius:16px;padding:32px;text-decoration:none;color:inherit;transition:all 0.3s}"
    ".feature-card:hover{border-color:var(--accent);transform:translateY(-4px);box-shadow:0 20px 40px rgba(0,212,170,0.1)}"
    ".feature-icon{width:48px;height:48px;border-radius:12px;display:flex;align-items:center;justify-content:center;margin-bottom:16px;font-size:24px}"
    ".feature-icon.logs{background:linear-gradient(135deg,#3b82f6,#1d4ed8)}"
    ".feature-icon.ota{background:linear-gradient(135deg,var(--accent),#059669)}"
    ".feature-card h3{font-size:20px;margin-bottom:8px}"
    ".feature-card p{color:var(--muted);font-size:14px;line-height:1.6}"
    ".stats{display:flex;gap:16px;flex-wrap:wrap;justify-content:center;margin-top:32px}"
    ".stat{background:var(--card);border:1px solid var(--border);border-radius:12px;padding:16px 24px;text-align:center}"
    ".stat-value{font-size:24px;font-weight:700;color:var(--accent)}"
    ".stat-label{font-size:12px;color:var(--muted);margin-top:4px}"
    "</style></head><body>"
    "<nav><a href='/' class='logo'><svg viewBox='0 0 24 24' fill='none' stroke='currentColor' stroke-width='2'><path d='M12 2L2 7l10 5 10-5-10-5zM2 17l10 5 10-5M2 12l10 5 10-5'/></svg>OT Gateway</a>"
    "<div class='nav-links'><a href='/' class='active'>Dashboard</a><a href='/logs'>Logs</a><a href='/ota'>OTA Update</a></div></nav>"
    "<div class='hero'><h1>OpenTherm Gateway</h1><p class='subtitle'>Monitor and manage your heating system</p>"
    "<div class='stats'><div class='stat'><div class='stat-value' id='uptime'>--</div><div class='stat-label'>Uptime</div></div>"
    "<div class='stat'><div class='stat-value' id='version'>--</div><div class='stat-label'>Firmware</div></div>"
    "<div class='stat'><div class='stat-value' id='partition'>--</div><div class='stat-label'>Partition</div></div></div></div>"
    "<div class='container'><div class='grid'>"
    "<a href='/logs' class='feature-card'><div class='feature-icon logs'>📊</div><h3>Live Logs</h3><p>Monitor OpenTherm messages in real-time. View requests and responses between your thermostat and boiler.</p></a>"
    "<a href='/ota' class='feature-card'><div class='feature-icon ota'>⬆️</div><h3>OTA Update</h3><p>Upload new firmware over-the-air. View current version, manage rollbacks, and update safely.</p></a>"
    "</div></div>"
    "<script>fetch('/ota/status').then(r=>r.json()).then(d=>{"
    "document.getElementById('version').textContent=d.version;"
    "document.getElementById('partition').textContent=d.running_partition;"
    "}).catch(()=>{});"
    "setInterval(()=>{fetch('/ota/status').then(r=>r.json()).then(d=>{"
    "let t=d.compile_time.split(' ');document.getElementById('uptime').textContent=t[0];"
    "}).catch(()=>{});},30000);</script></body></html>";

// Logs page (moved from root)
static const char *logs_page = 
    "<!DOCTYPE html><html><head><meta charset='UTF-8'><meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>Logs - OpenTherm Gateway</title><style>%s"
    ".log-container{background:var(--bg);border:1px solid var(--border);border-radius:8px;height:60vh;overflow-y:auto;font-size:13px}"
    ".log-entry{padding:8px 12px;border-bottom:1px solid var(--border);display:flex;gap:12px;align-items:flex-start}"
    ".log-entry:hover{background:rgba(255,255,255,0.02)}"
    ".log-entry.request{border-left:3px solid #3b82f6}"
    ".log-entry.response{border-left:3px solid #f59e0b}"
    ".log-entry.status{border-left:3px solid var(--accent2);background:rgba(124,58,237,0.05)}"
    ".log-time{color:var(--muted);font-size:11px;white-space:nowrap}"
    ".log-dir{font-weight:600;min-width:80px}"
    ".log-dir.request{color:#3b82f6}"
    ".log-dir.response{color:#f59e0b}"
    ".log-content{flex:1;word-break:break-all}"
    ".log-raw{color:var(--muted);font-size:11px}"
    ".toolbar{display:flex;gap:12px;align-items:center;margin-bottom:16px;flex-wrap:wrap}"
    ".status-indicator{display:flex;align-items:center;gap:8px}"
    ".status-dot{width:8px;height:8px;border-radius:50%;background:#ef4444}"
    ".status-dot.connected{background:var(--accent);box-shadow:0 0 8px var(--accent)}"
    ".msg-count{color:var(--muted);font-size:13px;margin-left:auto}"
    "</style></head><body>"
    "<nav><a href='/' class='logo'><svg viewBox='0 0 24 24' fill='none' stroke='currentColor' stroke-width='2'><path d='M12 2L2 7l10 5 10-5-10-5zM2 17l10 5 10-5M2 12l10 5 10-5'/></svg>OT Gateway</a>"
    "<div class='nav-links'><a href='/'>Dashboard</a><a href='/logs' class='active'>Logs</a><a href='/ota'>OTA Update</a></div></nav>"
    "<div class='container'><h1>Live Logs</h1><p class='subtitle'>Real-time OpenTherm message monitor</p>"
    "<div class='card'><div class='toolbar'>"
    "<div class='status-indicator'><div class='status-dot' id='status-dot'></div><span id='status-text'>Disconnected</span></div>"
    "<button class='btn btn-secondary' onclick='togglePause()' id='pause-btn'>Pause</button>"
    "<button class='btn btn-danger' onclick='clearLogs()'>Clear</button>"
    "<span class='msg-count'><span id='msg-count'>0</span> messages</span></div>"
    "<div class='log-container' id='logs'></div></div></div>"
    "<script>"
    "let ws,logs=document.getElementById('logs'),paused=false,msgCount=0;"
    "function connect(){"
    "ws=new WebSocket('ws://'+window.location.host+'/ws');"
    "ws.onopen=()=>{document.getElementById('status-dot').classList.add('connected');document.getElementById('status-text').textContent='Connected'};"
    "ws.onclose=()=>{document.getElementById('status-dot').classList.remove('connected');document.getElementById('status-text').textContent='Disconnected';setTimeout(connect,2000)};"
    "ws.onmessage=(e)=>{if(paused)return;msgCount++;document.getElementById('msg-count').textContent=msgCount;"
    "let div=document.createElement('div'),ts=new Date().toLocaleTimeString('en-GB',{hour12:false});"
    "try{let d=JSON.parse(e.data);div.className='log-entry '+d.direction.toLowerCase();"
    "div.innerHTML='<span class=\"log-time\">'+ts+'</span><span class=\"log-dir '+d.direction.toLowerCase()+'\">'+d.direction+'</span><span class=\"log-content\">'+d.msg_type+' | ID: '+d.data_id+' | Value: '+d.data_value+' <span class=\"log-raw\">0x'+d.message.toString(16).toUpperCase().padStart(8,'0')+'</span></span>';"
    "}catch(err){div.className='log-entry status';div.innerHTML='<span class=\"log-time\">'+ts+'</span><span class=\"log-content\">'+e.data+'</span>';}"
    "logs.appendChild(div);if(logs.children.length>500)logs.removeChild(logs.firstChild);logs.scrollTop=logs.scrollHeight;}};"
    "function togglePause(){paused=!paused;document.getElementById('pause-btn').textContent=paused?'Resume':'Pause';document.getElementById('pause-btn').classList.toggle('btn-primary',paused);document.getElementById('pause-btn').classList.toggle('btn-secondary',!paused);}"
    "function clearLogs(){logs.innerHTML='';msgCount=0;document.getElementById('msg-count').textContent='0';}"
    "connect();</script></body></html>";

// HTTP GET handler for root (dashboard)
static esp_err_t root_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    char *page = malloc(8192);
    if (page) {
        snprintf(page, 8192, dashboard_page, common_styles);
        httpd_resp_send(req, page, strlen(page));
        free(page);
        return ESP_OK;
    }
    return httpd_resp_send_500(req);
}

// HTTP GET handler for logs page
static esp_err_t logs_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    char *page = malloc(8192);
    if (page) {
        snprintf(page, 8192, logs_page, common_styles);
        httpd_resp_send(req, page, strlen(page));
        free(page);
        return ESP_OK;
    }
    return httpd_resp_send_500(req);
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
    config.max_uri_handlers = 16;  // Increased for OTA handlers
    config.uri_match_fn = httpd_uri_match_wildcard;
    config.recv_wait_timeout = 30;  // 30 seconds - needed for OTA uploads
    config.send_wait_timeout = 30;  // 30 seconds
    config.stack_size = 8192;       // Larger stack for OTA operations
    config.lru_purge_enable = true; // Allow purging idle connections
    
    ESP_LOGI(TAG, "Starting WebSocket server on port %d", config.server_port);
    
    if (httpd_start(&ws_server->server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server");
        return ESP_FAIL;
    }
    
    // Register root handler (dashboard)
    httpd_uri_t root_uri = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = root_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(ws_server->server, &root_uri);
    
    // Register logs page handler
    httpd_uri_t logs_uri = {
        .uri = "/logs",
        .method = HTTP_GET,
        .handler = logs_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(ws_server->server, &logs_uri);
    
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
        ESP_LOGD(TAG, "WebSocket message sent");
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

// Get HTTP server handle for registering additional handlers
httpd_handle_t websocket_server_get_handle(websocket_server_t *ws_server)
{
    return ws_server ? ws_server->server : NULL;
}
