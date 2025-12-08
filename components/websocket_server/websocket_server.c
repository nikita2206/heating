/*
 * WebSocket Server for OpenTherm Message Logging
 */

#include "websocket_server.h"
#include "esp_log.h"
#include "esp_http_server.h"
#include "esp_timer.h"
#include "boiler_manager.h"
#include "opentherm_gateway.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

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
    "<div class='nav-links'><a href='/' class='active'>Dashboard</a><a href='/logs'>Logs</a><a href='/diagnostics'>Diagnostics</a><a href='/ota'>OTA Update</a></div></nav>"
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
    "<div class='nav-links'><a href='/'>Dashboard</a><a href='/logs'>Logs</a><a href='/diagnostics'>Diagnostics</a><a href='/ota'>OTA Update</a></div></nav>"
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

// Diagnostics page HTML
static const char *diagnostics_page = 
    "<!DOCTYPE html><html><head><meta charset='UTF-8'><meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>Diagnostics - OpenTherm Gateway</title><style>%s"
    ".diagnostics-grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(250px,1fr));gap:16px;margin-top:24px}"
    ".diag-card{background:var(--card);border:1px solid var(--border);border-radius:8px;padding:16px}"
    ".diag-label{color:var(--muted);font-size:12px;margin-bottom:4px}"
    ".diag-value{font-size:20px;font-weight:600;color:var(--accent)}"
    ".diag-value.invalid{color:var(--muted)}"
    ".diag-timestamp{color:var(--muted);font-size:11px;margin-top:4px}"
    ".section-title{font-size:18px;font-weight:600;margin-top:24px;margin-bottom:12px;border-bottom:1px solid var(--border);padding-bottom:8px}"
    "</style></head><body>"
    "<nav><a href='/' class='logo'><svg viewBox='0 0 24 24' fill='none' stroke='currentColor' stroke-width='2'><path d='M12 2L2 7l10 5 10-5-10-5zM2 17l10 5 10-5M2 12l10 5 10-5'/></svg>OT Gateway</a>"
    "<div class='nav-links'><a href='/'>Dashboard</a><a href='/logs'>Logs</a><a href='/diagnostics' class='active'>Diagnostics</a><a href='/ota'>OTA Update</a></div></nav>"
    "<div class='container'><h1>Boiler Diagnostics</h1><p class='subtitle'>Real-time boiler state monitoring</p>"
    "<div class='section-title'>Temperatures</div>"
    "<div class='diagnostics-grid' id='temps'></div>"
    "<div class='section-title'>Status</div>"
    "<div class='diagnostics-grid' id='status'></div>"
    "<div class='section-title'>Faults</div>"
    "<div class='diagnostics-grid' id='faults'></div>"
    "<div class='section-title'>Statistics</div>"
    "<div class='diagnostics-grid' id='stats'></div>"
    "<div class='section-title'>Fans & CO2</div>"
    "<div class='diagnostics-grid' id='fans'></div>"
    "</div>"
    "<script>"
    "function formatValue(val,valid,unit=''){"
    "if(!valid||val===undefined||val===null)return'<span class=\"diag-value invalid\">--</span>';"
    "return'<span class=\"diag-value\">'+val.toFixed(1)+unit+'</span>';"
    "}"
    "function formatTimestamp(ageMs){"
    "if(!ageMs||ageMs<0)return'<span class=\"diag-timestamp\">Never</span>';"
    "if(ageMs<60000)return'<span class=\"diag-timestamp\">'+(ageMs/1000).toFixed(0)+'s ago</span>';"
    "if(ageMs<3600000)return'<span class=\"diag-timestamp\">'+(ageMs/60000).toFixed(0)+'m ago</span>';"
    "return'<span class=\"diag-timestamp\">'+(ageMs/3600000).toFixed(1)+'h ago</span>';"
    "}"
    "function updateDiagnostics(){"
    "fetch('/api/diagnostics').then(r=>r.json()).then(d=>{"
    "let temps=document.getElementById('temps'),status=document.getElementById('status'),"
    "faults=document.getElementById('faults'),stats=document.getElementById('stats'),"
    "fans=document.getElementById('fans');"
    "temps.innerHTML='';status.innerHTML='';faults.innerHTML='';stats.innerHTML='';fans.innerHTML='';"
    "if(d.t_boiler){temps.innerHTML+='<div class=\"diag-card\"><div class=\"diag-label\">Boiler Temp</div>'+formatValue(d.t_boiler.value,d.t_boiler.valid,'°C')+formatTimestamp(d.t_boiler.age_ms)+'</div>';}"
    "if(d.t_return){temps.innerHTML+='<div class=\"diag-card\"><div class=\"diag-label\">Return Temp</div>'+formatValue(d.t_return.value,d.t_return.valid,'°C')+formatTimestamp(d.t_return.age_ms)+'</div>';}"
    "if(d.t_dhw){temps.innerHTML+='<div class=\"diag-card\"><div class=\"diag-label\">DHW Temp</div>'+formatValue(d.t_dhw.value,d.t_dhw.valid,'°C')+formatTimestamp(d.t_dhw.age_ms)+'</div>';}"
    "if(d.t_outside){temps.innerHTML+='<div class=\"diag-card\"><div class=\"diag-label\">Outside Temp</div>'+formatValue(d.t_outside.value,d.t_outside.valid,'°C')+formatTimestamp(d.t_outside.age_ms)+'</div>';}"
    "if(d.t_exhaust){temps.innerHTML+='<div class=\"diag-card\"><div class=\"diag-label\">Exhaust Temp</div>'+formatValue(d.t_exhaust.value,d.t_exhaust.valid,'°C')+formatTimestamp(d.t_exhaust.age_ms)+'</div>';}"
    "if(d.t_setpoint){temps.innerHTML+='<div class=\"diag-card\"><div class=\"diag-label\">Setpoint Temp</div>'+formatValue(d.t_setpoint.value,d.t_setpoint.valid,'°C')+formatTimestamp(d.t_setpoint.age_ms)+'</div>';}"
    "if(d.modulation_level){status.innerHTML+='<div class=\"diag-card\"><div class=\"diag-label\">Modulation Level</div>'+formatValue(d.modulation_level.value,d.modulation_level.valid,'%%')+formatTimestamp(d.modulation_level.age_ms)+'</div>';}"
    "if(d.pressure){status.innerHTML+='<div class=\"diag-card\"><div class=\"diag-label\">Pressure</div>'+formatValue(d.pressure.value,d.pressure.valid,'bar')+formatTimestamp(d.pressure.age_ms)+'</div>';}"
    "if(d.flow_rate){status.innerHTML+='<div class=\"diag-card\"><div class=\"diag-label\">DHW Flow Rate</div>'+formatValue(d.flow_rate.value,d.flow_rate.valid,'L/min')+formatTimestamp(d.flow_rate.age_ms)+'</div>';}"
    "if(d.fault_code){faults.innerHTML+='<div class=\"diag-card\"><div class=\"diag-label\">Fault Code</div>'+formatValue(d.fault_code.value,d.fault_code.valid,'')+formatTimestamp(d.fault_code.age_ms)+'</div>';}"
    "if(d.diag_code){faults.innerHTML+='<div class=\"diag-card\"><div class=\"diag-label\">Diagnostic Code</div>'+formatValue(d.diag_code.value,d.diag_code.valid,'')+formatTimestamp(d.diag_code.age_ms)+'</div>';}"
    "if(d.burner_starts){stats.innerHTML+='<div class=\"diag-card\"><div class=\"diag-label\">Burner Starts</div>'+formatValue(d.burner_starts.value,d.burner_starts.valid,'')+formatTimestamp(d.burner_starts.age_ms)+'</div>';}"
    "if(d.burner_hours){stats.innerHTML+='<div class=\"diag-card\"><div class=\"diag-label\">Burner Hours</div>'+formatValue(d.burner_hours.value,d.burner_hours.valid,'h')+formatTimestamp(d.burner_hours.age_ms)+'</div>';}"
    "if(d.fan_current){fans.innerHTML+='<div class=\"diag-card\"><div class=\"diag-label\">Fan Speed</div>'+formatValue(d.fan_current.value,d.fan_current.valid,'%%')+formatTimestamp(d.fan_current.age_ms)+'</div>';}"
    "if(d.co2_exhaust){fans.innerHTML+='<div class=\"diag-card\"><div class=\"diag-label\">CO2 Exhaust</div>'+formatValue(d.co2_exhaust.value,d.co2_exhaust.valid,'ppm')+formatTimestamp(d.co2_exhaust.age_ms)+'</div>';}"
    "}).catch(err=>console.error('Failed to fetch diagnostics:',err));"
    "}"
    "updateDiagnostics();setInterval(updateDiagnostics,2000);"
    "</script></body></html>";

// HTTP GET handler for diagnostics page
static esp_err_t diagnostics_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    char *page = malloc(16384);
    if (page) {
        snprintf(page, 16384, diagnostics_page, common_styles);
        httpd_resp_send(req, page, strlen(page));
        free(page);
        return ESP_OK;
    }
    return httpd_resp_send_500(req);
}

// API handler for diagnostics JSON
static esp_err_t diagnostics_api_handler(httpd_req_t *req)
{
    boiler_manager_t *bm = (boiler_manager_t *)opentherm_gateway_get_boiler_manager();
    if (!bm) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_send(req, "{\"error\":\"Boiler manager not available\"}", -1);
        return ESP_FAIL;
    }
    
    const boiler_diagnostics_t *diag = boiler_manager_get_diagnostics(bm);
    if (!diag) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_send(req, "{\"error\":\"Diagnostics not available\"}", -1);
        return ESP_FAIL;
    }
    
    // Calculate current time (milliseconds since boot)
    int64_t current_time_ms = esp_timer_get_time() / 1000;
    
    // Helper macro to calculate age in milliseconds
    #define CALC_AGE_MS(field) ((field.valid && field.timestamp_ms > 0) ? (current_time_ms - field.timestamp_ms) : -1)
    
    // Build JSON response manually (avoiding cJSON dependency)
    // Use heap allocation to avoid stack overflow
    const size_t json_buffer_size = 8192;
    char *json_buffer = malloc(json_buffer_size);
    if (!json_buffer) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_send(req, "{\"error\":\"Memory allocation failed\"}", -1);
        return ESP_FAIL;
    }
    
    int len = snprintf(json_buffer, json_buffer_size,
        "{"
        "\"t_boiler\":{\"value\":%.2f,\"age_ms\":%lld,\"valid\":%s},"
        "\"t_return\":{\"value\":%.2f,\"age_ms\":%lld,\"valid\":%s},"
        "\"t_dhw\":{\"value\":%.2f,\"age_ms\":%lld,\"valid\":%s},"
        "\"t_dhw2\":{\"value\":%.2f,\"age_ms\":%lld,\"valid\":%s},"
        "\"t_outside\":{\"value\":%.2f,\"age_ms\":%lld,\"valid\":%s},"
        "\"t_exhaust\":{\"value\":%.2f,\"age_ms\":%lld,\"valid\":%s},"
        "\"t_heat_exchanger\":{\"value\":%.2f,\"age_ms\":%lld,\"valid\":%s},"
        "\"t_flow_ch2\":{\"value\":%.2f,\"age_ms\":%lld,\"valid\":%s},"
        "\"t_storage\":{\"value\":%.2f,\"age_ms\":%lld,\"valid\":%s},"
        "\"t_collector\":{\"value\":%.2f,\"age_ms\":%lld,\"valid\":%s},"
        "\"t_setpoint\":{\"value\":%.2f,\"age_ms\":%lld,\"valid\":%s},"
        "\"modulation_level\":{\"value\":%.2f,\"age_ms\":%lld,\"valid\":%s},"
        "\"pressure\":{\"value\":%.2f,\"age_ms\":%lld,\"valid\":%s},"
        "\"flow_rate\":{\"value\":%.2f,\"age_ms\":%lld,\"valid\":%s},"
        "\"fault_code\":{\"value\":%.2f,\"age_ms\":%lld,\"valid\":%s},"
        "\"diag_code\":{\"value\":%.2f,\"age_ms\":%lld,\"valid\":%s},"
        "\"burner_starts\":{\"value\":%.2f,\"age_ms\":%lld,\"valid\":%s},"
        "\"dhw_burner_starts\":{\"value\":%.2f,\"age_ms\":%lld,\"valid\":%s},"
        "\"ch_pump_starts\":{\"value\":%.2f,\"age_ms\":%lld,\"valid\":%s},"
        "\"dhw_pump_starts\":{\"value\":%.2f,\"age_ms\":%lld,\"valid\":%s},"
        "\"burner_hours\":{\"value\":%.2f,\"age_ms\":%lld,\"valid\":%s},"
        "\"dhw_burner_hours\":{\"value\":%.2f,\"age_ms\":%lld,\"valid\":%s},"
        "\"ch_pump_hours\":{\"value\":%.2f,\"age_ms\":%lld,\"valid\":%s},"
        "\"dhw_pump_hours\":{\"value\":%.2f,\"age_ms\":%lld,\"valid\":%s},"
        "\"max_capacity\":{\"value\":%.2f,\"age_ms\":%lld,\"valid\":%s},"
        "\"min_mod_level\":{\"value\":%.2f,\"age_ms\":%lld,\"valid\":%s},"
        "\"fan_setpoint\":{\"value\":%.2f,\"age_ms\":%lld,\"valid\":%s},"
        "\"fan_current\":{\"value\":%.2f,\"age_ms\":%lld,\"valid\":%s},"
        "\"fan_exhaust_rpm\":{\"value\":%.2f,\"age_ms\":%lld,\"valid\":%s},"
        "\"fan_supply_rpm\":{\"value\":%.2f,\"age_ms\":%lld,\"valid\":%s},"
        "\"co2_exhaust\":{\"value\":%.2f,\"age_ms\":%lld,\"valid\":%s}"
        "}",
        diag->t_boiler.value, (long long)CALC_AGE_MS(diag->t_boiler), diag->t_boiler.valid ? "true" : "false",
        diag->t_return.value, (long long)CALC_AGE_MS(diag->t_return), diag->t_return.valid ? "true" : "false",
        diag->t_dhw.value, (long long)CALC_AGE_MS(diag->t_dhw), diag->t_dhw.valid ? "true" : "false",
        diag->t_dhw2.value, (long long)CALC_AGE_MS(diag->t_dhw2), diag->t_dhw2.valid ? "true" : "false",
        diag->t_outside.value, (long long)CALC_AGE_MS(diag->t_outside), diag->t_outside.valid ? "true" : "false",
        diag->t_exhaust.value, (long long)CALC_AGE_MS(diag->t_exhaust), diag->t_exhaust.valid ? "true" : "false",
        diag->t_heat_exchanger.value, (long long)CALC_AGE_MS(diag->t_heat_exchanger), diag->t_heat_exchanger.valid ? "true" : "false",
        diag->t_flow_ch2.value, (long long)CALC_AGE_MS(diag->t_flow_ch2), diag->t_flow_ch2.valid ? "true" : "false",
        diag->t_storage.value, (long long)CALC_AGE_MS(diag->t_storage), diag->t_storage.valid ? "true" : "false",
        diag->t_collector.value, (long long)CALC_AGE_MS(diag->t_collector), diag->t_collector.valid ? "true" : "false",
        diag->t_setpoint.value, (long long)CALC_AGE_MS(diag->t_setpoint), diag->t_setpoint.valid ? "true" : "false",
        diag->modulation_level.value, (long long)CALC_AGE_MS(diag->modulation_level), diag->modulation_level.valid ? "true" : "false",
        diag->pressure.value, (long long)CALC_AGE_MS(diag->pressure), diag->pressure.valid ? "true" : "false",
        diag->flow_rate.value, (long long)CALC_AGE_MS(diag->flow_rate), diag->flow_rate.valid ? "true" : "false",
        diag->fault_code.value, (long long)CALC_AGE_MS(diag->fault_code), diag->fault_code.valid ? "true" : "false",
        diag->diag_code.value, (long long)CALC_AGE_MS(diag->diag_code), diag->diag_code.valid ? "true" : "false",
        diag->burner_starts.value, (long long)CALC_AGE_MS(diag->burner_starts), diag->burner_starts.valid ? "true" : "false",
        diag->dhw_burner_starts.value, (long long)CALC_AGE_MS(diag->dhw_burner_starts), diag->dhw_burner_starts.valid ? "true" : "false",
        diag->ch_pump_starts.value, (long long)CALC_AGE_MS(diag->ch_pump_starts), diag->ch_pump_starts.valid ? "true" : "false",
        diag->dhw_pump_starts.value, (long long)CALC_AGE_MS(diag->dhw_pump_starts), diag->dhw_pump_starts.valid ? "true" : "false",
        diag->burner_hours.value, (long long)CALC_AGE_MS(diag->burner_hours), diag->burner_hours.valid ? "true" : "false",
        diag->dhw_burner_hours.value, (long long)CALC_AGE_MS(diag->dhw_burner_hours), diag->dhw_burner_hours.valid ? "true" : "false",
        diag->ch_pump_hours.value, (long long)CALC_AGE_MS(diag->ch_pump_hours), diag->ch_pump_hours.valid ? "true" : "false",
        diag->dhw_pump_hours.value, (long long)CALC_AGE_MS(diag->dhw_pump_hours), diag->dhw_pump_hours.valid ? "true" : "false",
        diag->max_capacity.value, (long long)CALC_AGE_MS(diag->max_capacity), diag->max_capacity.valid ? "true" : "false",
        diag->min_mod_level.value, (long long)CALC_AGE_MS(diag->min_mod_level), diag->min_mod_level.valid ? "true" : "false",
        diag->fan_setpoint.value, (long long)CALC_AGE_MS(diag->fan_setpoint), diag->fan_setpoint.valid ? "true" : "false",
        diag->fan_current.value, (long long)CALC_AGE_MS(diag->fan_current), diag->fan_current.valid ? "true" : "false",
        diag->fan_exhaust_rpm.value, (long long)CALC_AGE_MS(diag->fan_exhaust_rpm), diag->fan_exhaust_rpm.valid ? "true" : "false",
        diag->fan_supply_rpm.value, (long long)CALC_AGE_MS(diag->fan_supply_rpm), diag->fan_supply_rpm.valid ? "true" : "false",
        diag->co2_exhaust.value, (long long)CALC_AGE_MS(diag->co2_exhaust), diag->co2_exhaust.valid ? "true" : "false"
    );
    
    // Ensure buffer is null-terminated and cap length to buffer size
    if (len < 0) {
        ESP_LOGE(TAG, "snprintf failed in diagnostics_api_handler");
        free(json_buffer);
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_send(req, "{\"error\":\"Failed to format diagnostics\"}", -1);
        return ESP_FAIL;
    }
    
    // Cap length to buffer size (snprintf returns required length even if truncated)
    if ((size_t)len >= json_buffer_size) {
        len = json_buffer_size - 1;
        json_buffer[len] = '\0';  // Ensure null termination
    }
    
    httpd_resp_set_type(req, "application/json");
    esp_err_t ret = httpd_resp_send(req, json_buffer, len);
    free(json_buffer);
    
    return ret;
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
    
    // Register diagnostics page handler
    httpd_uri_t diagnostics_uri = {
        .uri = "/diagnostics",
        .method = HTTP_GET,
        .handler = diagnostics_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(ws_server->server, &diagnostics_uri);
    
    // Register diagnostics API handler
    httpd_uri_t diagnostics_api_uri = {
        .uri = "/api/diagnostics",
        .method = HTTP_GET,
        .handler = diagnostics_api_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(ws_server->server, &diagnostics_api_uri);
    
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
