/*
 * Web UI Pages - HTML/CSS content for all pages
 * Formatted for readability, minified at compile-time
 */

#include "web_ui_pages.h"
#include "minify.hpp"

namespace web_ui {

// Use /* */ comments in JS code always, because // comments are not supported by the minifier

// ============================================================================
// DASHBOARD PAGE
// ============================================================================

constexpr char dashboard_styles_formatted[] = R"(
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

constexpr char dashboard_body_formatted[] = R"(
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

constexpr char logs_styles_formatted[] = R"(
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

    .log-entry.status {
        border-left: 3px solid var(--accent2);
        background: rgba(124, 58, 237, 0.05);
    }

    .log-entry.gateway-boiler {
        border-left: 3px solid #10b981;
    }

    .log-entry.thermostat-gateway {
        border-left: 3px solid #8b5cf6;
    }

    .log-time {
        color: var(--muted);
        font-size: 11px;
        white-space: nowrap;
    }

    .log-source {
        font-size: 10px;
        padding: 2px 6px;
        border-radius: 4px;
        white-space: nowrap;
    }

    .log-source.thermostat-boiler {
        background: rgba(59, 130, 246, 0.2);
        color: #3b82f6;
    }

    .log-source.gateway-boiler {
        background: rgba(16, 185, 129, 0.2);
        color: #10b981;
    }

    .log-source.thermostat-gateway {
        background: rgba(139, 92, 246, 0.2);
        color: #8b5cf6;
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

    .log-raw {
        color: var(--muted);
        font-size: 11px;
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

    .filter-group {
        display: flex;
        gap: 8px;
        align-items: center;
    }

    .filter-select {
        padding: 6px 10px;
        border: 1px solid var(--border);
        border-radius: 6px;
        background: var(--bg);
        color: var(--text);
        font-size: 13px;
    }

    .filter-input {
        padding: 6px 10px;
        border: 1px solid var(--border);
        border-radius: 6px;
        background: var(--bg);
        color: var(--text);
        font-size: 13px;
        width: 80px;
    }

    .msg-count {
        color: var(--muted);
        font-size: 13px;
        margin-left: auto;
    }
)";

constexpr char logs_body_formatted[] = R"JS(
    <div class='container'>
        <h1>Live Logs</h1>
        <p class='subtitle'>Real-time OpenTherm message monitor</p>
        <div class='card'>
            <div class='toolbar'>
                <div class='status-indicator'>
                    <div class='status-dot' id='status-dot'></div>
                    <span id='status-text'>Disconnected</span>
                </div>
                <div class='filter-group'>
                    <select class='filter-select' id='source-filter' onchange='applyFilters()'>
                        <option value='all'>All Sources</option>
                        <option value='THERMOSTAT_BOILER'>T&#x2194;B Proxied</option>
                        <option value='GATEWAY_BOILER'>G&#x2194;B Gateway</option>
                        <option value='THERMOSTAT_GATEWAY'>T&#x2194;G Control</option>
                    </select>
                    <input type='number' class='filter-input' id='dataid-filter' placeholder='ID' min='0' max='255' onchange='applyFilters()'>
                </div>
                <button class='btn btn-secondary' onclick='togglePause()' id='pause-btn'>Pause</button>
                <button class='btn btn-danger' onclick='clearLogs()'>Clear</button>
                <span class='msg-count'><span id='msg-count'>0</span>&nbsp;messages</span>
            </div>
            <div class='log-container' id='logs'></div>
        </div>
    </div>
    <script>
        let ws, logs, paused = false, msgCount = 0;
        let allMessages = [];
        let sourceFilter = 'all';
        let dataIdFilter = null;

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
                try {
                    let d = JSON.parse(e.data);
                    allMessages.push({data: d, timestamp: new Date()});
                    if (allMessages.length > 500) allMessages.shift();
                    if (!passesFilter(d)) return;
                    appendLogEntry(d);
                } catch (err) {
                    let div = document.createElement('div');
                    let ts = new Date().toLocaleTimeString('en-GB', {hour12: false});
                    div.className = 'log-entry status';
                    div.innerHTML = '<span class="log-time">' + ts + '</span><span class="log-content">' + e.data + '</span>';
                    logs.appendChild(div);
                    if (logs.children.length > 500) logs.removeChild(logs.firstChild);
                    logs.scrollTop = logs.scrollHeight;
                }
            };
        }

        function passesFilter(d) {
            if (sourceFilter !== 'all' && d.source !== sourceFilter) return false;
            if (dataIdFilter !== null && d.data_id !== dataIdFilter) return false;
            return true;
        }


        const MASTER_MSG_TYPES = {
            0: "READ-DATA",
            1: "WRITE-DATA",
            2: "INVALID-DATA",
            3: "RESERVED",
            4: "RESERVED",
            5: "RESERVED",
            6: "RESERVED",
            7: "RESERVED"
        };

        const SLAVE_MSG_TYPES = {
            0: "RESERVED",
            1: "RESERVED",
            2: "RESERVED",
            3: "RESERVED",
            4: "READ-ACK",
            5: "WRITE-ACK",
            6: "DATA-INVALID",
            7: "UNKNOWN-DATAID"
        };

        function u16_to_s16(v) {
            return (v & 0x8000) ? v - 0x10000 : v;
        }

        function f8_8_to_float(u16) {
            return u16_to_s16(u16) / 256.0;
        }

        function fmt_hex(v, width) {
            return "0x" + v.toString(16).toUpperCase().padStart(width, '0');
        }

        function parity_ok(frame_bits) {
            let count = 0;
            for (let bit of frame_bits) {
                if (bit === '1') count++;
            }
            return (count % 2) === 0;
        }

        function decode_status_id0(data_u16) {
            let hb = (data_u16 >> 8) & 0xFF; 
            let lb = data_u16 & 0xFF;        

            function pretty_master(bit, val) {
                let on = (val >> bit) & 1;
                if (bit === 0) return "CH=" + (on ? "enabled" : "disabled");
                if (bit === 1) return "DHW=" + (on ? "enabled" : "disabled");
                if (bit === 2) return "Cooling=" + (on ? "enabled" : "disabled");
                if (bit === 3) return "OTC=" + (on ? "active" : "inactive");
                if (bit === 4) return "CH2=" + (on ? "enabled" : "disabled");
                if (bit === 5) return "Mode=" + (on ? "summer" : "winter");
                if (bit === 6) return "DHWBlocking=" + (on ? "blocked" : "unblocked");
                if (bit === 7) return "reserved";
                return "bit" + bit + "=" + on;
            }

            function pretty_slave(bit, val) {
                let on = (val >> bit) & 1;
                if (bit === 0) return "Fault=" + (on ? "yes" : "no");
                if (bit === 1) return "CHMode=" + (on ? "active" : "inactive");
                if (bit === 2) return "DHWMode=" + (on ? "active" : "inactive");
                if (bit === 3) return "Flame=" + (on ? "on" : "off");
                if (bit === 4) return "CoolingMode=" + (on ? "active" : "inactive");
                if (bit === 5) return "CH2Mode=" + (on ? "active" : "inactive");
                if (bit === 6) return "Diagnostic=" + (on ? "event" : "none");
                if (bit === 7) return "ElectricityProduction=" + (on ? "on" : "off");
                return "bit" + bit + "=" + on;
            }

            let master_desc = [];
            for (let b = 0; b < 8; b++) master_desc.push(pretty_master(b, hb));
            let slave_desc = [];
            for (let b = 0; b < 8; b++) slave_desc.push(pretty_slave(b, lb));

            return "MasterStatus(HB): " + master_desc.join("; ") + " | SlaveStatus(LB): " + slave_desc.join("; ");
        }

        function decode_id1_control_setpoint(data_u16) {
            let t = f8_8_to_float(data_u16);
            return "Control Setpoint (Tset) = " + t.toFixed(2) + " °C (raw=" + fmt_hex(data_u16, 4) + ")";
        }

        function decode_id2_master_config(data_u16) {
            let hb = (data_u16 >> 8) & 0xFF;
            let lb = data_u16 & 0xFF;
            let smart_power = (hb & 0x01) ? "implemented" : "not implemented";
            return "Master Config: SmartPower=" + smart_power + ", MemberID=" + lb + " (raw=" + fmt_hex(data_u16, 4) + ")";
        }

        function decode_id3_slave_config(data_u16) {
            let hb = (data_u16 >> 8) & 0xFF;
            let lb = data_u16 & 0xFF;
            function b(bit) { return (hb >> bit) & 1; }
            let dhw_present = b(0) ? "yes" : "no";
            let control_type = b(1) ? "on/off" : "modulating";
            let cooling = b(2) ? "supported" : "not supported";
            let dhw_cfg = b(3) ? "storage tank" : "instantaneous/not specified";
            let lowoff = b(4) ? "not allowed" : "allowed";
            let ch2 = b(5) ? "present" : "not present";
            let waterfill = b(6) ? "not available" : "available/unknown";
            let heatcool_switch = b(7) ? "by slave" : "by master";
            return "Slave Config: DHWpresent=" + dhw_present + ", ControlType=" + control_type + ", Cooling=" + cooling +
                   ", DHWcfg=" + dhw_cfg + ", LowOff&PumpCtrl=" + lowoff + ", CH2=" + ch2 + ", RemoteWaterFill=" + waterfill +
                   ", HeatCoolSwitch=" + heatcool_switch + ", MemberID=" + lb + " (raw=" + fmt_hex(data_u16, 4) + ")";
        }

        function decode_f8_8_temp(name, data_u16) {
            let v = f8_8_to_float(data_u16);
            return name + " = " + v.toFixed(2) + " °C (raw=" + fmt_hex(data_u16, 4) + ")";
        }

        const ID_DECODERS = {
            0: { name: "Status", decode: decode_status_id0 },
            1: { name: "Control Setpoint", decode: decode_id1_control_setpoint },
            2: { name: "Master Configuration", decode: decode_id2_master_config },
            3: { name: "Slave Configuration", decode: decode_id3_slave_config },
            17: { name: "Relative modulation level", decode: function(u16) {
                return "Relative modulation = " + f8_8_to_float(u16).toFixed(2) + " % (raw=" + fmt_hex(u16, 4) + ")";
            }},
            18: { name: "CH water pressure", decode: function(u16) {
                return "CH water pressure = " + f8_8_to_float(u16).toFixed(2) + " bar (raw=" + fmt_hex(u16, 4) + ")";
            }},
            19: { name: "DHW flow rate", decode: function(u16) {
                return "DHW flow rate = " + f8_8_to_float(u16).toFixed(2) + " l/min (raw=" + fmt_hex(u16, 4) + ")";
            }},
            25: { name: "Boiler water temp", decode: function(u16) { return decode_f8_8_temp("Boiler water temp", u16); } },
            26: { name: "DHW temp", decode: function(u16) { return decode_f8_8_temp("DHW temp", u16); } },
            27: { name: "Outside temp", decode: function(u16) { return decode_f8_8_temp("Outside temp", u16); } },
            28: { name: "Return water temp", decode: function(u16) { return decode_f8_8_temp("Return water temp", u16); } }
        };

        function decode_frame_from_message(d) {
            let frame_bits = d.message.toString(2).padStart(32, '0');
            let is_master_to_slave = (d.direction === 'REQUEST');

            /* OpenTherm frame layout:
               [31] ?, [30:28] MSG_TYPE, [27:24] ?, [23:16] DATA_ID, [15:0] DATA_VALUE */
            let msg_type = parseInt(frame_bits.substr(3, 3), 2);  /* bits 28-30 */
            let data_id = parseInt(frame_bits.substr(8, 8), 2);   /* bits 16-23 */
            let data_val = parseInt(frame_bits.substr(16, 16), 2); /* bits 0-15 */


            let msg_name = (is_master_to_slave ? MASTER_MSG_TYPES : SLAVE_MSG_TYPES)[msg_type] || "UNKNOWN";
            let par_ok = parity_ok(frame_bits);

            /* Decode payload */
            let decoder = ID_DECODERS[data_id];
            let payload, id_name;
            if (decoder) {
                payload = decoder.decode(data_val);
                id_name = decoder.name;
            } else {
                let hb = (data_val >> 8) & 0xFF;
                let lb = data_val & 0xFF;
                payload = "DATA-VALUE=" + fmt_hex(data_val, 4) + " (HB=" + fmt_hex(hb, 2) + ", LB=" + fmt_hex(lb, 2) + ")";
                id_name = "Unknown/Unimplemented";
            }

            let warnings = [];
            if (!par_ok) {
                warnings.push("PARITY_ERROR");
            }

            let warn_txt = warnings.length > 0 ? " [" + warnings.join(" | ") + "]" : "";
            return msg_name + " (msg=" + msg_type + ", id=" + data_id + " " + id_name + ") " + payload + warn_txt;
        }

        function appendLogEntry(d) {
            let div = document.createElement('div');
            let ts = new Date().toLocaleTimeString('en-GB', {hour12: false});
            let src = (d.source || 'THERMOSTAT_BOILER').toLowerCase().replace(/_/g, '-');
            let srcLabel = {'thermostat-boiler':'T&#x2194;B', 'gateway-boiler':'G&#x2194;B', 'thermostat-gateway':'T&#x2194;G'}[src] || 'T&#x2194;B';
            div.className = 'log-entry ' + (d.direction || '').toLowerCase() + ' ' + src;
            let decoded_content = decode_frame_from_message(d);
            div.innerHTML = '<span class="log-time">' + ts + '</span>' +
                '<span class="log-source ' + src + '">' + srcLabel + '</span>' +
                '<span class="log-dir ' + (d.direction || '').toLowerCase() + '">' + (d.direction || '') + '</span>' +
                '<span class="log-content">' + decoded_content + '</span>';
            logs.appendChild(div);
            if (logs.children.length > 500) logs.removeChild(logs.firstChild);
            logs.scrollTop = logs.scrollHeight;
        }

        function applyFilters() {
            sourceFilter = document.getElementById('source-filter').value;
            let idVal = document.getElementById('dataid-filter').value;
            dataIdFilter = idVal !== '' ? parseInt(idVal) : null;
            logs.innerHTML = '';
            allMessages.forEach(msg => { if (passesFilter(msg.data)) appendLogEntry(msg.data); });
        }

        function togglePause() {
            paused = !paused;
            document.getElementById('pause-btn').textContent = paused ? 'Resume' : 'Pause';
        }

        function clearLogs() {
            logs.innerHTML = '';
            allMessages = [];
            msgCount = 0;
            document.getElementById('msg-count').textContent = '0';
        }

        /* Wait for DOM to be ready */
        function init() {
            try {
                logs = document.getElementById('logs');
                connect();
            } catch (e) {
                console.error('Failed to initialize WebSocket connection:', e);
            }
        }

        if (document.readyState === 'loading') {
            document.addEventListener('DOMContentLoaded', init);
        } else {
            init();
        }
    </script>
)JS";

// ============================================================================
// DIAGNOSTICS PAGE
// ============================================================================

constexpr char diagnostics_styles_formatted[] = R"(
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

    .diag-value.invalid {
        color: var(--muted);
    }

    .diag-timestamp {
        color: var(--muted);
        font-size: 11px;
        margin-top: 4px;
    }

    .section-title {
        font-size: 18px;
        font-weight: 600;
        margin-top: 24px;
        margin-bottom: 12px;
        border-bottom: 1px solid var(--border);
        padding-bottom: 8px;
    }

    .all-values-table {
        width: 100%;
        border-collapse: collapse;
        margin-top: 16px;
    }

    .all-values-table td {
        padding: 8px 12px;
        border-bottom: 1px solid var(--border);
    }

    .all-values-table td:first-child {
        color: var(--muted);
        font-size: 13px;
        width: 40%;
    }

    .all-values-table td:last-child {
        text-align: right;
        font-family: monospace;
        font-size: 13px;
    }

    .all-values-table tr:hover {
        background: var(--card-hover);
    }
)";

constexpr char diagnostics_body_formatted[] = R"(
    <div class='container'>
        <h1>Boiler Diagnostics</h1>
        <p class='subtitle'>Real-time boiler state monitoring</p>
        <div class='card'>
            <div class='card-title'>Control Mode</div>
            <div id='control-info'>--</div>
        </div>
        <div class='section-title'>Temperatures</div>
        <div class='diagnostics-grid' id='temps'></div>
        <div class='section-title'>Status</div>
        <div class='diagnostics-grid' id='status'></div>
        <div class='section-title'>Faults</div>
        <div class='diagnostics-grid' id='faults'></div>
        <div class='section-title'>Statistics</div>
        <div class='diagnostics-grid' id='stats'></div>
        <div class='section-title'>Fans & CO2</div>
        <div class='diagnostics-grid' id='fans'></div>
        <div class='section-title'>All Values</div>
        <table class='all-values-table' id='all-values'></table>
    </div>
    <script>
        function formatValue(val, valid, unit) {
            unit = unit || '';
            if (!valid || val === undefined || val === null) return '<span class="diag-value invalid">--</span>';
            return '<span class="diag-value">' + val.toFixed(1) + unit + '</span>';
        }
        function formatTimestamp(ageMs) {
            if (!ageMs || ageMs < 0) return '<span class="diag-timestamp">Never</span>';
            if (ageMs < 60000) return '<span class="diag-timestamp">' + (ageMs / 1000).toFixed(0) + 's ago</span>';
            if (ageMs < 3600000) return '<span class="diag-timestamp">' + (ageMs / 60000).toFixed(0) + 'm ago</span>';
            return '<span class="diag-timestamp">' + (ageMs / 3600000).toFixed(1) + 'h ago</span>';
        }
        function formatAge(ageMs) {
            if (!ageMs || ageMs < 0) return 'Never';
            if (ageMs < 60000) return (ageMs / 1000).toFixed(0) + 's ago';
            if (ageMs < 3600000) return (ageMs / 60000).toFixed(0) + 'm ago';
            return (ageMs / 3600000).toFixed(1) + 'h ago';
        }
        function updateDiagnostics() {
            fetch('/api/diagnostics').then(r => r.json()).then(d => {
                let temps = document.getElementById('temps'),
                    status = document.getElementById('status'),
                    faults = document.getElementById('faults'),
                    stats = document.getElementById('stats'),
                    fans = document.getElementById('fans'),
                    allValues = document.getElementById('all-values');
                temps.innerHTML = ''; status.innerHTML = ''; faults.innerHTML = '';
                stats.innerHTML = ''; fans.innerHTML = ''; allValues.innerHTML = '';
                if (d.t_boiler) temps.innerHTML += '<div class="diag-card"><div class="diag-label">Boiler Temp</div>' + formatValue(d.t_boiler.value, d.t_boiler.valid, '°C') + formatTimestamp(d.t_boiler.age_ms) + '</div>';
                if (d.t_return) temps.innerHTML += '<div class="diag-card"><div class="diag-label">Return Temp</div>' + formatValue(d.t_return.value, d.t_return.valid, '°C') + formatTimestamp(d.t_return.age_ms) + '</div>';
                if (d.t_dhw) temps.innerHTML += '<div class="diag-card"><div class="diag-label">DHW Temp</div>' + formatValue(d.t_dhw.value, d.t_dhw.valid, '°C') + formatTimestamp(d.t_dhw.age_ms) + '</div>';
                if (d.t_outside) temps.innerHTML += '<div class="diag-card"><div class="diag-label">Outside Temp</div>' + formatValue(d.t_outside.value, d.t_outside.valid, '°C') + formatTimestamp(d.t_outside.age_ms) + '</div>';
                if (d.t_exhaust) temps.innerHTML += '<div class="diag-card"><div class="diag-label">Exhaust Temp</div>' + formatValue(d.t_exhaust.value, d.t_exhaust.valid, '°C') + formatTimestamp(d.t_exhaust.age_ms) + '</div>';
                if (d.t_setpoint) temps.innerHTML += '<div class="diag-card"><div class="diag-label">Setpoint Temp</div>' + formatValue(d.t_setpoint.value, d.t_setpoint.valid, '°C') + formatTimestamp(d.t_setpoint.age_ms) + '</div>';
                if (d.modulation_level) status.innerHTML += '<div class="diag-card"><div class="diag-label">Modulation Level</div>' + formatValue(d.modulation_level.value, d.modulation_level.valid, '%') + formatTimestamp(d.modulation_level.age_ms) + '</div>';
                if (d.pressure) status.innerHTML += '<div class="diag-card"><div class="diag-label">Pressure</div>' + formatValue(d.pressure.value, d.pressure.valid, 'bar') + formatTimestamp(d.pressure.age_ms) + '</div>';
                if (d.flow_rate) status.innerHTML += '<div class="diag-card"><div class="diag-label">DHW Flow Rate</div>' + formatValue(d.flow_rate.value, d.flow_rate.valid, 'L/min') + formatTimestamp(d.flow_rate.age_ms) + '</div>';
                if (d.fault_code) faults.innerHTML += '<div class="diag-card"><div class="diag-label">Fault Code</div>' + formatValue(d.fault_code.value, d.fault_code.valid, '') + formatTimestamp(d.fault_code.age_ms) + '</div>';
                if (d.diag_code) faults.innerHTML += '<div class="diag-card"><div class="diag-label">Diagnostic Code</div>' + formatValue(d.diag_code.value, d.diag_code.valid, '') + formatTimestamp(d.diag_code.age_ms) + '</div>';
                if (d.burner_starts) stats.innerHTML += '<div class="diag-card"><div class="diag-label">Burner Starts</div>' + formatValue(d.burner_starts.value, d.burner_starts.valid, '') + formatTimestamp(d.burner_starts.age_ms) + '</div>';
                if (d.burner_hours) stats.innerHTML += '<div class="diag-card"><div class="diag-label">Burner Hours</div>' + formatValue(d.burner_hours.value, d.burner_hours.valid, 'h') + formatTimestamp(d.burner_hours.age_ms) + '</div>';
                if (d.fan_current) fans.innerHTML += '<div class="diag-card"><div class="diag-label">Fan Speed</div>' + formatValue(d.fan_current.value, d.fan_current.valid, '%') + formatTimestamp(d.fan_current.age_ms) + '</div>';
                if (d.co2_exhaust) fans.innerHTML += '<div class="diag-card"><div class="diag-label">CO2 Exhaust</div>' + formatValue(d.co2_exhaust.value, d.co2_exhaust.valid, 'ppm') + formatTimestamp(d.co2_exhaust.age_ms) + '</div>';
                let allItems = [
                    {key: 'Boiler Temp', val: d.t_boiler, unit: '°C'}, {key: 'Return Temp', val: d.t_return, unit: '°C'},
                    {key: 'DHW Temp', val: d.t_dhw, unit: '°C'}, {key: 'DHW Temp 2', val: d.t_dhw2, unit: '°C'},
                    {key: 'Outside Temp', val: d.t_outside, unit: '°C'}, {key: 'Exhaust Temp', val: d.t_exhaust, unit: '°C'},
                    {key: 'Heat Exchanger Temp', val: d.t_heat_exchanger, unit: '°C'}, {key: 'CH2 Flow Temp', val: d.t_flow_ch2, unit: '°C'},
                    {key: 'Storage Temp', val: d.t_storage, unit: '°C'}, {key: 'Collector Temp', val: d.t_collector, unit: '°C'},
                    {key: 'Setpoint Temp', val: d.t_setpoint, unit: '°C'}, {key: 'Modulation Level', val: d.modulation_level, unit: '%'},
                    {key: 'Pressure', val: d.pressure, unit: 'bar'}, {key: 'DHW Flow Rate', val: d.flow_rate, unit: 'L/min'},
                    {key: 'Fault Code', val: d.fault_code, unit: ''}, {key: 'Diagnostic Code', val: d.diag_code, unit: ''},
                    {key: 'Burner Starts', val: d.burner_starts, unit: ''}, {key: 'DHW Burner Starts', val: d.dhw_burner_starts, unit: ''},
                    {key: 'CH Pump Starts', val: d.ch_pump_starts, unit: ''}, {key: 'DHW Pump Starts', val: d.dhw_pump_starts, unit: ''},
                    {key: 'Burner Hours', val: d.burner_hours, unit: 'h'}, {key: 'DHW Burner Hours', val: d.dhw_burner_hours, unit: 'h'},
                    {key: 'CH Pump Hours', val: d.ch_pump_hours, unit: 'h'}, {key: 'DHW Pump Hours', val: d.dhw_pump_hours, unit: 'h'},
                    {key: 'Max Capacity', val: d.max_capacity, unit: 'kW'}, {key: 'Min Mod Level', val: d.min_mod_level, unit: '%'},
                    {key: 'Fan Setpoint', val: d.fan_setpoint, unit: '%'}, {key: 'Fan Current', val: d.fan_current, unit: '%'},
                    {key: 'Fan Exhaust RPM', val: d.fan_exhaust_rpm, unit: 'rpm'}, {key: 'Fan Supply RPM', val: d.fan_supply_rpm, unit: 'rpm'},
                    {key: 'CO2 Exhaust', val: d.co2_exhaust, unit: 'ppm'}
                ];
                allItems.forEach(item => {
                    if (item.val && item.val.valid) {
                        let row = allValues.insertRow();
                        row.insertCell(0).textContent = item.key;
                        row.insertCell(1).textContent = item.val.value.toFixed(1) + item.unit + ' (' + formatAge(item.val.age_ms) + ')';
                    }
                });
            }).catch(err => console.error('Failed to fetch diagnostics:', err));
        }
        function updateControlStatus() {
            fetch('/api/control_mode').then(r => r.json()).then(d => {
                let text = 'Control disabled';
                if (d.enabled) {
                    text = 'Control ' + (d.active ? 'ACTIVE' : (d.fallback ? 'Fallback (passthrough)' : 'Idle')) +
                           ' | TSet: ' + (d.demand_tset ? d.demand_tset.toFixed(1) + '°C' : '--') +
                           ' | CH: ' + (d.demand_ch ? 'ON' : 'OFF');
                }
                document.getElementById('control-info').textContent = text;
            }).catch(() => {});
        }
        updateDiagnostics(); setInterval(updateDiagnostics, 2000);
        updateControlStatus(); setInterval(updateControlStatus, 5000);
    </script>
)";

// ============================================================================
// MQTT PAGE
// ============================================================================

constexpr char mqtt_styles_formatted[] = R"(
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

    .form-input:focus {
        outline: none;
        border-color: var(--accent);
    }

    .form-switch {
        display: flex;
        align-items: center;
        gap: 8px;
        margin-bottom: 12px;
    }

    .status-chip {
        display: inline-flex;
        align-items: center;
        gap: 8px;
        padding: 6px 10px;
        border-radius: 999px;
        font-size: 12px;
        border: 1px solid var(--border);
    }

    .status-chip.ok {
        color: var(--accent);
        border-color: var(--accent);
    }

    .status-chip.bad {
        color: #ef4444;
        border-color: #ef4444;
    }

    .card small {
        color: var(--muted);
    }

    .form-help {
        color: var(--muted);
        font-size: 12px;
        margin-top: 4px;
    }
)";

constexpr char mqtt_body_formatted[] = R"(
    <div class='container'>
        <h1>MQTT Overrides</h1>
        <p class='subtitle'>Configure broker and topics for external overrides</p>
        <div class='card'>
            <div class='status-chip' id='mqtt-chip'>--</div>
            <form id='mqtt-form'>
                <label class='form-switch'>
                    <input type='checkbox' id='enable'> <span>Enable MQTT bridge</span>
                </label>
                <div class='form-group'>
                    <label class='form-label'>Broker URI</label>
                    <input class='form-input' id='broker_uri' placeholder='mqtt://host:1883'>
                </div>
                <div class='form-group'>
                    <label class='form-label'>Client ID</label>
                    <input class='form-input' id='client_id' placeholder='ot-gateway'>
                </div>
                <div class='form-group'>
                    <label class='form-label'>Username</label>
                    <input class='form-input' id='username' placeholder='(optional)'>
                </div>
                <div class='form-group'>
                    <label class='form-label'>Password</label>
                    <input class='form-input' id='password' type='password' placeholder='(optional)'>
                </div>
                <div class='form-group'>
                    <label class='form-label'>Base Topic</label>
                    <input class='form-input' id='base_topic' placeholder='ot_gateway'>
                </div>
                <div class='form-group'>
                    <label class='form-label'>Discovery Prefix</label>
                    <input class='form-input' id='discovery_prefix' placeholder='homeassistant'>
                </div>
                <button type='submit' class='btn btn-primary'>Save & Restart MQTT</button>
                <small>Changes are stored in NVS and applied immediately.</small>
            </form>
        </div>
        <div class='card'>
            <h3 style='margin-bottom:12px'>Control Mode</h3>
            <form id='control-form'>
                <label class='form-switch'>
                    <input type='checkbox' id='control_enable'> <span>Enable control mode (MQTT overrides)</span>
                </label>
                <div class='form-group'>
                    <div class='status-chip' id='control-chip'>--</div>
                    <div id='control-demand' class='form-help'></div>
                </div>
                <button type='submit' class='btn btn-secondary'>Apply</button>
            </form>
        </div>
    </div>
    <script>
        function loadConfig() {
            fetch('/api/mqtt_config').then(r => r.json()).then(d => {
                document.getElementById('enable').checked = d.enable;
                document.getElementById('broker_uri').value = d.broker_uri || '';
                document.getElementById('client_id').value = d.client_id || '';
                document.getElementById('username').value = d.username || '';
                document.getElementById('discovery_prefix').value = d.discovery_prefix || 'homeassistant';
                document.getElementById('base_topic').value = d.base_topic || '';
                const chip = document.getElementById('mqtt-chip');
                if (d.connected) { chip.textContent = 'Connected'; chip.className = 'status-chip ok'; }
                else { chip.textContent = 'Offline'; chip.className = 'status-chip bad'; }
            }).catch(() => {});
        }
        function loadControl() {
            fetch('/api/control_mode').then(r => r.json()).then(d => {
                document.getElementById('control_enable').checked = d.enabled;
                const chip = document.getElementById('control-chip');
                let status = 'Offline', cls = 'status-chip bad';
                if (d.enabled) {
                    status = d.active ? 'Active' : (d.fallback ? 'Fallback (passthrough)' : 'Idle');
                    cls = d.active ? 'status-chip ok' : (d.fallback ? 'status-chip bad' : 'status-chip');
                }
                chip.textContent = status; chip.className = cls;
                let demand = 'Demanded TSet: ' + (d.demand_tset ? d.demand_tset.toFixed(1) + '°C' : '--') + ', CH: ' + (d.demand_ch ? 'ON' : 'OFF');
                document.getElementById('control-demand').textContent = demand;
            }).catch(() => {});
        }
        document.getElementById('mqtt-form').addEventListener('submit', function(e) {
            e.preventDefault();
            let params = new URLSearchParams();
            params.append('enable', document.getElementById('enable').checked ? 'on' : 'off');
            params.append('broker_uri', document.getElementById('broker_uri').value);
            params.append('client_id', document.getElementById('client_id').value);
            params.append('username', document.getElementById('username').value);
            params.append('password', document.getElementById('password').value);
            params.append('base_topic', document.getElementById('base_topic').value);
            fetch('/api/mqtt_config', {method: 'POST', headers: {'Content-Type': 'application/x-www-form-urlencoded'}, body: params.toString()})
                .then(r => r.json()).then(() => { loadConfig(); alert('Saved and restarted'); }).catch(() => alert('Failed to save'));
        });
        document.getElementById('control-form').addEventListener('submit', function(e) {
            e.preventDefault();
            let params = new URLSearchParams();
            params.append('enabled', document.getElementById('control_enable').checked ? 'on' : 'off');
            fetch('/api/control_mode', {method: 'POST', headers: {'Content-Type': 'application/x-www-form-urlencoded'}, body: params.toString()})
                .then(() => loadControl()).catch(() => alert('Failed to toggle control mode'));
        });
        loadConfig();
        loadControl();
    </script>
)";

// ============================================================================
// WRITE PAGE
// ============================================================================

constexpr char write_styles_formatted[] = R"(
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
        font-family: inherit;
        width: 100%;
        box-sizing: border-box;
    }

    .form-input:focus,
    .form-select:focus {
        outline: none;
        border-color: var(--accent);
    }

    .form-row {
        display: grid;
        grid-template-columns: 1fr 1fr;
        gap: 16px;
    }

    .form-help {
        color: var(--muted);
        font-size: 12px;
        margin-top: 4px;
    }

    .response-box {
        background: var(--bg);
        border: 1px solid var(--border);
        border-radius: 8px;
        padding: 16px;
        margin-top: 24px;
        font-family: monospace;
        font-size: 13px;
        white-space: pre-wrap;
        word-break: break-all;
    }

    .response-box.success {
        border-color: var(--accent);
        background: rgba(0, 212, 170, 0.05);
    }

    .response-box.error {
        border-color: #ef4444;
        background: rgba(239, 68, 68, 0.05);
    }

    .preset-buttons {
        display: flex;
        gap: 8px;
        flex-wrap: wrap;
        margin-bottom: 16px;
    }

    .preset-btn {
        padding: 6px 12px;
        border: 1px solid var(--border);
        border-radius: 6px;
        background: var(--card);
        color: var(--text);
        font-size: 12px;
        cursor: pointer;
        transition: all 0.2s;
    }

    .preset-btn:hover {
        background: var(--border);
        border-color: var(--accent);
    }
)";

constexpr char write_body_formatted[] = R"(
    <div class='container'>
        <h1>Manual WRITE_DATA Frame</h1>
        <p class='subtitle'>Send WRITE_DATA frames directly to the boiler</p>
        <div class='card'>
            <div class='preset-buttons'>
                <button class='preset-btn' onclick='setPreset(1,"float",20.0)'>TSet (20°C)</button>
                <button class='preset-btn' onclick='setPreset(1,"float",30.0)'>TSet (30°C)</button>
                <button class='preset-btn' onclick='setPreset(1,"float",40.0)'>TSet (40°C)</button>
                <button class='preset-btn' onclick='setPreset(1,"float",50.0)'>TSet (50°C)</button>
                <button class='preset-btn' onclick='setPreset(8,"float",30.0)'>TSetCH2 (30°C)</button>
                <button class='preset-btn' onclick='setPreset(16,"float",20.0)'>Troom Setpoint (20°C)</button>
                <button class='preset-btn' onclick='setPreset(14,"uint16",50)'>Max Mod Level (50%)</button>
            </div>
            <form id='write-form' onsubmit='sendWrite(event)'>
                <div class='form-row'>
                    <div class='form-group'>
                        <label class='form-label'>Data ID</label>
                        <select id='data_id' class='form-select' required>
                            <option value='1'>1 - TSet (Control Setpoint)</option>
                            <option value='2'>2 - Master Configuration</option>
                            <option value='4'>4 - Command</option>
                            <option value='6'>6 - Remote Override</option>
                            <option value='7'>7 - Cooling Control</option>
                            <option value='8'>8 - TSetCH2</option>
                            <option value='9'>9 - Troom Override</option>
                            <option value='10'>10 - TSP (Setpoint Override)</option>
                            <option value='14'>14 - Max Rel Mod Level Setting</option>
                            <option value='16'>16 - Troom Setpoint</option>
                            <option value='custom'>Custom...</option>
                        </select>
                        <div class='form-help'>Select the data ID to write</div>
                    </div>
                    <div class='form-group' id='custom_id_group' style='display:none'>
                        <label class='form-label'>Custom Data ID</label>
                        <input type='number' id='custom_data_id' class='form-input' min='0' max='255' placeholder='0-255'>
                    </div>
                </div>
                <div class='form-row'>
                    <div class='form-group'>
                        <label class='form-label'>Data Type</label>
                        <select id='data_type' class='form-select' required>
                            <option value='float'>Float (f8.8) - Temperature</option>
                            <option value='uint16'>Uint16 - Raw value</option>
                            <option value='flags'>Flags - Bit field</option>
                        </select>
                        <div class='form-help'>Format of the data value</div>
                    </div>
                    <div class='form-group'>
                        <label class='form-label'>Data Value</label>
                        <input type='text' id='data_value' class='form-input' required placeholder='e.g., 20.5 or 12345'>
                        <div class='form-help'>For float: temperature in °C. For uint16: 0-65535. For flags: hex (0x1234)</div>
                    </div>
                </div>
                <button type='submit' class='btn btn-primary'>Send WRITE_DATA Frame</button>
            </form>
            <div id='response' class='response-box' style='display:none'></div>
        </div>
    </div>
    <script>
        document.getElementById('data_id').addEventListener('change', function(e) {
            document.getElementById('custom_id_group').style.display = e.target.value === 'custom' ? 'block' : 'none';
        });
        function setPreset(id, type, value) {
            document.getElementById('data_id').value = id.toString();
            document.getElementById('data_type').value = type;
            document.getElementById('data_value').value = value.toString();
            document.getElementById('custom_id_group').style.display = 'none';
        }
        function sendWrite(e) {
            e.preventDefault();
            let dataId = document.getElementById('data_id').value;
            if (dataId === 'custom') { dataId = document.getElementById('custom_data_id').value; }
            let dataType = document.getElementById('data_type').value;
            let dataValue = document.getElementById('data_value').value;
            let responseBox = document.getElementById('response');
            responseBox.style.display = 'block';
            responseBox.className = 'response-box';
            responseBox.textContent = 'Sending...';
            let payload = {
                data_id: parseInt(dataId),
                data_value: dataType === 'float' ? parseFloat(dataValue) : dataType === 'flags' ? parseInt(dataValue, 16) : parseInt(dataValue),
                data_type: dataType
            };
            fetch('/api/write', {method: 'POST', headers: {'Content-Type': 'application/json'}, body: JSON.stringify(payload)})
                .then(r => r.json()).then(d => {
                    if (d.success) {
                        responseBox.className = 'response-box success';
                        responseBox.textContent = 'Success!\nRequest: Data ID=' + d.request.data_id + ', Value=' + d.request.data_value + '\n' +
                            'Response Frame: 0x' + d.response.frame.toString(16).toUpperCase().padStart(8, '0') + '\n' +
                            'Response Type: ' + d.response.type + '\n' +
                            'Response Data ID: ' + d.response.data_id + '\n' +
                            'Response Data Value: ' + d.response.data_value;
                    } else {
                        responseBox.className = 'response-box error';
                        responseBox.textContent = 'Error: ' + d.error + ' (code: ' + d.error_code + ')';
                    }
                }).catch(err => {
                    responseBox.className = 'response-box error';
                    responseBox.textContent = 'Network error: ' + err.message;
                });
        }
    </script>
)";

// ============================================================================
// OTA PAGE
// ============================================================================

constexpr char ota_styles_formatted[] = R"(
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

    .info-label {
        color: var(--muted);
    }

    .info-value {
        font-weight: 500;
    }

    .upload-zone {
        border: 2px dashed var(--border);
        border-radius: 12px;
        padding: 48px 24px;
        text-align: center;
        transition: all 0.3s;
        cursor: pointer;
    }

    .upload-zone:hover,
    .upload-zone.dragover {
        border-color: var(--accent);
        background: rgba(0, 212, 170, 0.05);
    }

    .upload-zone.uploading {
        border-color: var(--accent2);
        background: rgba(124, 58, 237, 0.05);
    }

    .upload-icon {
        font-size: 48px;
        margin-bottom: 16px;
        opacity: 0.5;
    }

    .upload-text {
        color: var(--muted);
        margin-bottom: 8px;
    }

    .upload-hint {
        font-size: 12px;
        color: var(--muted);
    }

    #file-input {
        display: none;
    }

    .progress-container {
        margin-top: 24px;
        display: none;
    }

    .progress-bar {
        height: 8px;
        background: var(--border);
        border-radius: 4px;
        overflow: hidden;
    }

    .progress-fill {
        height: 100%;
        background: linear-gradient(90deg, var(--accent), var(--accent2));
        width: 0%;
        transition: width 0.3s;
    }

    .progress-text {
        margin-top: 8px;
        font-size: 13px;
        color: var(--muted);
        text-align: center;
    }

    .status-msg {
        margin-top: 16px;
        padding: 12px 16px;
        border-radius: 8px;
        font-size: 14px;
        display: none;
    }

    .status-msg.success {
        display: block;
        background: rgba(0, 212, 170, 0.15);
        color: var(--accent);
        border: 1px solid rgba(0, 212, 170, 0.3);
    }

    .status-msg.error {
        display: block;
        background: rgba(239, 68, 68, 0.15);
        color: #ef4444;
        border: 1px solid rgba(239, 68, 68, 0.3);
    }

    .actions {
        display: flex;
        gap: 12px;
        margin-top: 24px;
        flex-wrap: wrap;
    }

    .partition-visual {
        display: flex;
        gap: 8px;
        margin-top: 16px;
    }

    .partition-box {
        flex: 1;
        padding: 12px;
        border-radius: 8px;
        text-align: center;
        font-size: 12px;
    }

    .partition-box.active {
        background: rgba(0, 212, 170, 0.15);
        border: 1px solid var(--accent);
    }

    .partition-box.inactive {
        background: var(--bg);
        border: 1px solid var(--border);
    }

    .partition-name {
        font-weight: 600;
        margin-bottom: 4px;
    }

    .partition-addr {
        color: var(--muted);
        font-size: 11px;
    }

    .warning {
        background: rgba(250, 204, 21, 0.1);
        border: 1px solid rgba(250, 204, 21, 0.3);
        border-radius: 8px;
        padding: 12px 16px;
        margin-bottom: 24px;
        font-size: 13px;
        color: #facc15;
    }
)";

constexpr char ota_body_formatted[] = R"(
    <div class='container'>
        <h1>Firmware Update</h1>
        <p class='subtitle'>Over-the-air firmware management</p>
        <div id='pending-warning' class='warning' style='display:none'>Warning: Firmware is pending verification. If you don't confirm it, the device will rollback on next reboot.</div>
        <div class='grid'>
            <div class='card'>
                <div class='card-title'>Current Firmware</div>
                <div class='info-grid'>
                    <span class='info-label'>Version</span><span class='info-value' id='version'>--</span>
                    <span class='info-label'>Project</span><span class='info-value' id='project'>--</span>
                    <span class='info-label'>Compiled</span><span class='info-value' id='compile-time'>--</span>
                    <span class='info-label'>IDF Version</span><span class='info-value' id='idf-ver'>--</span>
                    <span class='info-label'>State</span><span class='info-value'><span class='badge green' id='state'>--</span></span>
                </div>
                <div class='partition-visual'>
                    <div class='partition-box' id='ota0-box'><div class='partition-name'>ota_0</div><div class='partition-addr' id='ota0-addr'>--</div></div>
                    <div class='partition-box' id='ota1-box'><div class='partition-name'>ota_1</div><div class='partition-addr' id='ota1-addr'>--</div></div>
                </div>
                <div class='actions'>
                    <button class='btn btn-secondary' onclick='confirmFirmware()' id='confirm-btn' style='display:none'>Confirm Firmware</button>
                    <button class='btn btn-danger' onclick='rollback()' id='rollback-btn'>Rollback</button>
                </div>
            </div>
            <div class='card'>
                <div class='card-title'>Upload New Firmware</div>
                <div class='upload-zone' id='upload-zone' onclick="document.getElementById('file-input').click() ">
                    <div class='upload-icon'>📁</div>
                    <div class='upload-text'>Drop firmware.bin here or click to browse</div>
                    <div class='upload-hint'>Max size: ~1.7 MB (partition size)</div>
                </div>
                <input type='file' id='file-input' accept='.bin' onchange='handleFile(this.files[0])'>
                <div class='progress-container' id='progress-container'>
                    <div class='progress-bar'><div class='progress-fill' id='progress-fill'></div></div>
                    <div class='progress-text' id='progress-text'>Uploading... 0%</div>
                </div>
                <div class='status-msg' id='status-msg'></div>
            </div>
        </div>
    </div>
    <script>
        let zone = document.getElementById('upload-zone'), uploading = false, statusInterval = null;
        zone.addEventListener('dragover', e => { e.preventDefault(); zone.classList.add('dragover'); });
        zone.addEventListener('dragleave', () => zone.classList.remove('dragover'));
        zone.addEventListener('drop', e => { e.preventDefault(); zone.classList.remove('dragover'); if (e.dataTransfer.files.length) handleFile(e.dataTransfer.files[0]); });
        function handleFile(file) {
            if (!file || !file.name.endsWith('.bin')) { showStatus('Please select a .bin file', 'error'); return; }
            if (file.size > 1800000) { showStatus('File too large for partition', 'error'); return; }
            uploadFirmware(file);
        }
        function uploadFirmware(file) {
            if (uploading) return;
            uploading = true;
            if (statusInterval) { clearInterval(statusInterval); statusInterval = null; }
            let xhr = new XMLHttpRequest();
            let progress = document.getElementById('progress-container');
            let fill = document.getElementById('progress-fill');
            let text = document.getElementById('progress-text');
            progress.style.display = 'block'; zone.classList.add('uploading'); hideStatus();
            xhr.upload.onprogress = e => { if (e.lengthComputable) { let pct = Math.round(e.loaded / e.total * 100); fill.style.width = pct + '%'; text.textContent = 'Uploading... ' + pct + '%'; } };
            xhr.onload = () => {
                zone.classList.remove('uploading'); uploading = false;
                if (xhr.status === 200) {
                    let r = JSON.parse(xhr.responseText);
                    showStatus('Success: ' + r.message, 'success');
                    text.textContent = 'Complete! Restarting...';
                    setTimeout(() => location.reload(), 5000);
                } else {
                    showStatus('Upload failed: ' + xhr.responseText, 'error');
                    progress.style.display = 'none'; startStatusPolling();
                }
            };
            xhr.onerror = () => { zone.classList.remove('uploading'); uploading = false; showStatus('Network error', 'error'); progress.style.display = 'none'; startStatusPolling(); };
            xhr.open('POST', '/ota'); xhr.timeout = 120000; xhr.send(file);
        }
        function showStatus(msg, type) { let el = document.getElementById('status-msg'); el.textContent = msg; el.className = 'status-msg ' + type; }
        function hideStatus() { document.getElementById('status-msg').className = 'status-msg'; }
        function rollback() {
            if (!confirm('Rollback to previous firmware? Device will restart.')) return;
            fetch('/ota/rollback', {method: 'POST'}).then(r => r.json()).then(d => { showStatus(d.message, 'success'); setTimeout(() => location.reload(), 3000); }).catch(e => showStatus('Rollback failed', 'error'));
        }
        function confirmFirmware() {
            fetch('/ota/confirm', {method: 'POST'}).then(r => r.json()).then(d => { showStatus(d.message, 'success'); loadStatus(); }).catch(e => showStatus('Confirm failed', 'error'));
        }
        function loadStatus() {
            if (uploading) return;
            fetch('/ota/status').then(r => r.json()).then(d => {
                document.getElementById('version').textContent = d.version;
                document.getElementById('project').textContent = d.project_name;
                document.getElementById('compile-time').textContent = d.compile_time;
                document.getElementById('idf-ver').textContent = d.idf_ver;
                let state = document.getElementById('state');
                state.textContent = d.ota_state;
                state.className = 'badge ' + (d.ota_state === 'valid' ? 'green' : d.ota_state === 'pending_verify' ? 'yellow' : 'purple');
                let run = d.running_partition;
                document.getElementById('ota0-box').className = 'partition-box ' + (run === 'ota_0' ? 'active' : 'inactive');
                document.getElementById('ota1-box').className = 'partition-box ' + (run === 'ota_1' ? 'active' : 'inactive');
                document.getElementById('ota0-addr').textContent = run === 'ota_0' ? 'Running' : 'Next update';
                document.getElementById('ota1-addr').textContent = run === 'ota_1' ? 'Running' : 'Next update';
                if (d.ota_state === 'pending_verify') {
                    document.getElementById('pending-warning').style.display = 'block';
                    document.getElementById('confirm-btn').style.display = 'inline-flex';
                } else {
                    document.getElementById('pending-warning').style.display = 'none';
                    document.getElementById('confirm-btn').style.display = 'none';
                }
            }).catch(() => {});
        }
        function startStatusPolling() { if (!statusInterval) statusInterval = setInterval(loadStatus, 10000); }
        loadStatus(); startStatusPolling();
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

