/*
 * Web UI Pages - HTML/CSS content for all pages
 * Formatted for readability, minified at compile-time
 */

#include "web_ui_pages.h"
#include "minify.hpp"

namespace web_ui {

// ============================================================================
// DASHBOARD PAGE
// ============================================================================

constexpr auto dashboard_styles_formatted = R"(
    .hero {
        text-align: center;
        padding: 48px 24px;
    }
    
    .hero h1 {
        font-size: 42px;
        margin-bottom: 16px;
    }
    
    .grid {
        display: grid;
        grid-template-columns: repeat(auto-fit, minmax(300px, 1fr));
        gap: 24px;
        margin-top: 32px;
    }
    
    .feature-card {
        background: var(--card);
        border: 1px solid var(--border);
        border-radius: 16px;
        padding: 32px;
        text-decoration: none;
        color: inherit;
        transition: all 0.3s;
    }
    
    .feature-card:hover {
        border-color: var(--accent);
        transform: translateY(-4px);
        box-shadow: 0 20px 40px rgba(0, 212, 170, 0.1);
    }
    
    .feature-icon {
        width: 48px;
        height: 48px;
        border-radius: 12px;
        display: flex;
        align-items: center;
        justify-content: center;
        margin-bottom: 16px;
        font-size: 24px;
    }
    
    .feature-icon.logs {
        background: linear-gradient(135deg, #3b82f6, #1d4ed8);
    }
    
    .feature-icon.ota {
        background: linear-gradient(135deg, var(--accent), #059669);
    }
    
    .feature-card h3 {
        font-size: 20px;
        margin-bottom: 8px;
    }
    
    .feature-card p {
        color: var(--muted);
        font-size: 14px;
        line-height: 1.6;
    }
    
    .stats {
        display: flex;
        gap: 16px;
        flex-wrap: wrap;
        justify-content: center;
        margin-top: 32px;
    }
    
    .stat {
        background: var(--card);
        border: 1px solid var(--border);
        border-radius: 12px;
        padding: 16px 24px;
        text-align: center;
    }
    
    .stat-value {
        font-size: 24px;
        font-weight: 700;
        color: var(--accent);
    }
    
    .stat-label {
        font-size: 12px;
        color: var(--muted);
        margin-top: 4px;
    }
)";

constexpr auto dashboard_body_formatted = R"(
    <div class='hero'>
        <h1>OpenTherm Gateway</h1>
        <p class='subtitle'>Monitor and manage your heating system</p>
        
        <div class='stats'>
            <div class='stat'>
                <div class='stat-value' id='uptime'>--</div>
                <div class='stat-label'>Uptime</div>
            </div>
            <div class='stat'>
                <div class='stat-value' id='version'>--</div>
                <div class='stat-label'>Firmware</div>
            </div>
            <div class='stat'>
                <div class='stat-value' id='partition'>--</div>
                <div class='stat-label'>Partition</div>
            </div>
            <div class='stat'>
                <div class='stat-value' id='mqtt-status'>--</div>
                <div class='stat-label'>MQTT</div>
            </div>
            <div class='stat'>
                <div class='stat-value' id='mqtt-tset'>--</div>
                <div class='stat-label'>MQTT TSet</div>
            </div>
        </div>
    </div>
    
    <div class='container'>
        <div class='grid'>
            <a href='/logs' class='feature-card'>
                <div class='feature-icon logs'>📊</div>
                <h3>Live Logs</h3>
                <p>Monitor OpenTherm messages in real-time. View requests and responses between your thermostat and boiler.</p>
            </a>
            
            <a href='/diagnostics' class='feature-card'>
                <div class='feature-icon' style='background:linear-gradient(135deg,#f59e0b,#d97706)'>🔧</div>
                <h3>Diagnostics</h3>
                <p>View real-time boiler diagnostics including temperatures, pressures, and system status.</p>
            </a>
            
            <a href='/write' class='feature-card'>
                <div class='feature-icon' style='background:linear-gradient(135deg,var(--accent2),#6d28d9)'>✏️</div>
                <h3>Manual Write</h3>
                <p>Send WRITE_DATA frames directly to the boiler. Manually control setpoints and other writable parameters.</p>
            </a>
            
            <a href='/ota' class='feature-card'>
                <div class='feature-icon ota'>⬆️</div>
                <h3>OTA Update</h3>
                <p>Upload new firmware over-the-air. View current version, manage rollbacks, and update safely.</p>
            </a>
        </div>
    </div>
    
    <script>
        function refreshOta() {
            fetch('/ota/status')
                .then(r => r.json())
                .then(d => {
                    document.getElementById('version').textContent = d.version;
                    document.getElementById('partition').textContent = d.running_partition;
                    let t = d.compile_time.split(' ');
                    document.getElementById('uptime').textContent = t[0];
                })
                .catch(() => {});
        }
        
        function refreshMqtt() {
            fetch('/api/mqtt_state')
                .then(r => r.json())
                .then(d => {
                    document.getElementById('mqtt-status').textContent = d.connected ? 'Connected' : 'Offline';
                    if (d.last_tset_valid) {
                        document.getElementById('mqtt-tset').textContent = d.last_tset.toFixed(1) + '°C';
                    } else {
                        document.getElementById('mqtt-tset').textContent = '--';
                    }
                })
                .catch(() => {
                    document.getElementById('mqtt-status').textContent = 'Offline';
                });
        }
        
        refreshOta();
        refreshMqtt();
        setInterval(refreshOta, 30000);
        setInterval(refreshMqtt, 5000);
    </script>
)";

// ============================================================================
// LOGS PAGE  
// ============================================================================

constexpr auto logs_styles_formatted = R"(
    .log-container {
        background: var(--bg);
        border: 1px solid var(--border);
        border-radius: 8px;
        height: 60vh;
        overflow-y: auto;
        font-size: 13px;
    }

    .log-entry {
        padding: 8px 12px;
        border-bottom: 1px solid var(--border);
        display: flex;
        gap: 12px;
        align-items: flex-start;
    }

    .log-entry:hover {
        background: rgba(255, 255, 255, 0.02);
    }

    .log-entry.request {
        border-left: 3px solid #3b82f6;
    }

    .log-entry.response {
        border-left: 3px solid #f59e0b;
    }

    .log-time {
        color: var(--muted);
        font-size: 11px;
        white-space: nowrap;
    }

    .log-dir {
        font-weight: 600;
        min-width: 80px;
    }

    .log-dir.request {
        color: #3b82f6;
    }

    .log-dir.response {
        color: #f59e0b;
    }

    .log-content {
        flex: 1;
        word-break: break-all;
    }

    .toolbar {
        display: flex;
        gap: 12px;
        align-items: center;
        margin-bottom: 16px;
        flex-wrap: wrap;
    }

    .status-indicator {
        display: flex;
        align-items: center;
        gap: 8px;
    }

    .status-dot {
        width: 8px;
        height: 8px;
        border-radius: 50%;
        background: #ef4444;
    }

    .status-dot.connected {
        background: var(--accent);
        box-shadow: 0 0 8px var(--accent);
    }
)";

constexpr auto logs_body_formatted = R"(
    <div class='container'>
        <h1>Live Logs</h1>
        <p class='subtitle'>Real-time OpenTherm message monitor</p>
        <div class='card'>
            <div class='toolbar'>
                <div class='status-indicator'>
                    <div class='status-dot' id='status-dot'></div>
                    <span id='status-text'>Disconnected</span>
                </div>
                <button class='btn btn-secondary' onclick='togglePause()' id='pause-btn'>Pause</button>
                <button class='btn btn-danger' onclick='clearLogs()'>Clear</button>
                <span class='msg-count'><span id='msg-count'>0</span> messages</span>
            </div>
            <div class='log-container' id='logs'></div>
        </div>
    </div>
    <script>
        let ws, logs = document.getElementById('logs'), paused = false, msgCount = 0;

        function connect() {
            ws = new WebSocket('ws://' + window.location.host + '/ws');
            ws.onopen = () => {
                document.getElementById('status-dot').classList.add('connected');
                document.getElementById('status-text').textContent = 'Connected';
            };
            ws.onclose = () => {
                document.getElementById('status-dot').classList.remove('connected');
                document.getElementById('status-text').textContent = 'Disconnected';
                setTimeout(connect, 2000);
            };
            ws.onmessage = (e) => {
                if (paused) return;
                msgCount++;
                document.getElementById('msg-count').textContent = msgCount;
                let div = document.createElement('div');
                let ts = new Date().toLocaleTimeString('en-GB', {hour12: false});
                try {
                    let d = JSON.parse(e.data);
                    div.className = 'log-entry ' + d.direction.toLowerCase();
                    div.innerHTML = '<span class="log-time">' + ts + '</span>' +
                        '<span class="log-dir ' + d.direction.toLowerCase() + '">' + d.direction + '</span>' +
                        '<span class="log-content">' + d.msg_type + ' | ID: ' + d.data_id +
                        ' | Value: ' + d.data_value +
                        ' <span class="log-raw">0x' + d.message.toString(16).toUpperCase().padStart(8, '0') +
                        '</span></span>';
                } catch (err) {
                    div.className = 'log-entry status';
                    div.innerHTML = '<span class="log-time">' + ts + '</span>' +
                        '<span class="log-content">' + e.data + '</span>';
                }
                logs.appendChild(div);
                if (logs.children.length > 500) logs.removeChild(logs.firstChild);
                logs.scrollTop = logs.scrollHeight;
            };
        }

        function togglePause() {
            paused = !paused;
            document.getElementById('pause-btn').textContent = paused ? 'Resume' : 'Pause';
        }

        function clearLogs() {
            logs.innerHTML = '';
            msgCount = 0;
            document.getElementById('msg-count').textContent = '0';
        }

        connect();
    </script>
)";

// ============================================================================
// DIAGNOSTICS PAGE
// ============================================================================

constexpr auto diagnostics_styles_formatted = R"(
    .diagnostics-grid {
        display: grid;
        grid-template-columns: repeat(auto-fit, minmax(250px, 1fr));
        gap: 16px;
        margin-top: 24px;
    }

    .diag-card {
        background: var(--card);
        border: 1px solid var(--border);
        border-radius: 8px;
        padding: 16px;
    }

    .diag-label {
        color: var(--muted);
        font-size: 12px;
        margin-bottom: 4px;
    }

    .diag-value {
        font-size: 20px;
        font-weight: 600;
        color: var(--accent);
    }

    .section-title {
        font-size: 18px;
        font-weight: 600;
        margin-top: 24px;
        margin-bottom: 12px;
        border-bottom: 1px solid var(--border);
        padding-bottom: 8px;
    }
)";

constexpr auto diagnostics_body_formatted = R"(
    <div class='container'>
        <h1>Boiler Diagnostics</h1>
        <p class='subtitle'>Real-time boiler state monitoring</p>
        <div class='card'>
            <div class='card-title'>Control Mode</div>
            <div id='control-info'>--</div>
        </div>
        <div class='section-title'>Temperatures</div>
        <div class='diagnostics-grid' id='temps'></div>
    </div>
    <script>
        function updateDiagnostics() {
            fetch('/api/diagnostics')
                .then(r => r.json())
                .then(d => {})
                .catch(err => console.error(err));
        }

        updateDiagnostics();
        setInterval(updateDiagnostics, 2000);
    </script>
)";

// ============================================================================
// MQTT PAGE
// ============================================================================

constexpr auto mqtt_styles_formatted = R"(
    .form-group {
        margin-bottom: 16px;
    }

    .form-label {
        display: block;
        color: var(--text);
        font-size: 14px;
        font-weight: 500;
        margin-bottom: 6px;
    }

    .form-input {
        padding: 10px 12px;
        border: 1px solid var(--border);
        border-radius: 8px;
        background: var(--bg);
        color: var(--text);
        font-size: 14px;
        font-family: inherit;
        width: 100%;
        box-sizing: border-box;
    }

    .form-switch {
        display: flex;
        align-items: center;
        gap: 8px;
        margin-bottom: 12px;
    }
)";

constexpr auto mqtt_body_formatted = R"(
    <div class='container'>
        <h1>MQTT Overrides</h1>
        <p class='subtitle'>Configure broker and topics</p>
        <div class='card'>
            <form id='mqtt-form'>
                <label class='form-switch'>
                    <input type='checkbox' id='enable'> Enable MQTT
                </label>
                <div class='form-group'>
                    <label class='form-label'>Broker URI</label>
                    <input class='form-input' id='broker_uri'>
                </div>
                <button type='submit' class='btn btn-primary'>Save</button>
            </form>
        </div>
    </div>
)";

// ============================================================================
// WRITE PAGE
// ============================================================================

constexpr auto write_styles_formatted = R"(
    .form-group {
        margin-bottom: 20px;
    }

    .form-label {
        display: block;
        color: var(--text);
        font-size: 14px;
        font-weight: 500;
        margin-bottom: 8px;
    }

    .form-input,
    .form-select {
        padding: 10px 12px;
        border: 1px solid var(--border);
        border-radius: 8px;
        background: var(--bg);
        color: var(--text);
        font-size: 14px;
        width: 100%;
    }
)";

constexpr auto write_body_formatted = R"(
    <div class='container'>
        <h1>Manual WRITE_DATA Frame</h1>
        <p class='subtitle'>Send WRITE_DATA frames directly to the boiler</p>
        <div class='card'>
            <form id='write-form'>
                <div class='form-group'>
                    <label class='form-label'>Data ID</label>
                    <select id='data_id' class='form-select'>
                        <option value='1'>1 - TSet</option>
                    </select>
                </div>
                <button type='submit' class='btn btn-primary'>Send</button>
            </form>
        </div>
    </div>
)";

// ============================================================================
// OTA PAGE
// ============================================================================

constexpr auto ota_styles_formatted = R"(
    .grid {
        display: grid;
        grid-template-columns: repeat(auto-fit, minmax(350px, 1fr));
        gap: 24px;
    }

    .info-grid {
        display: grid;
        grid-template-columns: 140px 1fr;
        gap: 8px 16px;
        font-size: 14px;
    }

    .upload-zone {
        border: 2px dashed var(--border);
        border-radius: 12px;
        padding: 48px 24px;
        text-align: center;
        cursor: pointer;
    }

    .partition-box {
        flex: 1;
        padding: 12px;
        border-radius: 8px;
        text-align: center;
        font-size: 12px;
    }
)";

constexpr auto ota_body_formatted = R"(
    <div class='container'>
        <h1>Firmware Update</h1>
        <p class='subtitle'>Over-the-air firmware management</p>
        <div class='grid'>
            <div class='card'>
                <div class='card-title'>Current Firmware</div>
                <div class='info-grid'>
                    <span>Version</span>
                    <span id='version'>--</span>
                </div>
            </div>
            <div class='card'>
                <div class='card-title'>Upload New Firmware</div>
                <div class='upload-zone' onclick="document.getElementById('file-input').click() ">
                    <div>Drop firmware.bin here</div>
                </div>
                <input type='file' id='file-input' style='display:none'>
            </div>
        </div>
    </div>
    <script>
        function loadStatus() {
            fetch('/ota/status')
                .then(r => r.json())
                .then(d => {
                    document.getElementById('version').textContent = d.version;
                });
        }

        loadStatus();
    </script>
)";

} // namespace web_ui

// Export minified versions for C linkage
extern "C" {
    const char* WEB_UI_DASHBOARD_STYLES = MINIFY_CSS(web_ui::dashboard_styles_formatted);
    const char* WEB_UI_DASHBOARD_BODY = MINIFY_HTML(web_ui::dashboard_body_formatted);

    const char* WEB_UI_LOGS_STYLES = MINIFY_CSS(web_ui::logs_styles_formatted);
    const char* WEB_UI_LOGS_BODY = MINIFY_HTML(web_ui::logs_body_formatted);

    const char* WEB_UI_DIAGNOSTICS_STYLES = MINIFY_CSS(web_ui::diagnostics_styles_formatted);
    const char* WEB_UI_DIAGNOSTICS_BODY = MINIFY_HTML(web_ui::diagnostics_body_formatted);

    const char* WEB_UI_MQTT_STYLES = MINIFY_CSS(web_ui::mqtt_styles_formatted);
    const char* WEB_UI_MQTT_BODY = MINIFY_HTML(web_ui::mqtt_body_formatted);

    const char* WEB_UI_WRITE_STYLES = MINIFY_CSS(web_ui::write_styles_formatted);
    const char* WEB_UI_WRITE_BODY = MINIFY_HTML(web_ui::write_body_formatted);

    const char* WEB_UI_OTA_STYLES = MINIFY_CSS(web_ui::ota_styles_formatted);
    const char* WEB_UI_OTA_BODY = MINIFY_HTML(web_ui::ota_body_formatted);
}

