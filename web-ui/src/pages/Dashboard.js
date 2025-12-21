/**
 * Dashboard page
 */

let otaInterval = null;
let mqttInterval = null;

export function renderDashboard() {
  return `
    <div class="hero">
      <h1>OpenTherm Gateway</h1>
      <p class="subtitle">Monitor and manage your heating system</p>

      <div class="stats">
        <div class="stat">
          <div class="stat-value" id="uptime">--</div>
          <div class="stat-label">Uptime</div>
        </div>
        <div class="stat">
          <div class="stat-value" id="version">--</div>
          <div class="stat-label">Firmware</div>
        </div>
        <div class="stat">
          <div class="stat-value" id="partition">--</div>
          <div class="stat-label">Partition</div>
        </div>
        <div class="stat">
          <div class="stat-value" id="mqtt-status">--</div>
          <div class="stat-label">MQTT</div>
        </div>
        <div class="stat">
          <div class="stat-value" id="mqtt-tset">--</div>
          <div class="stat-label">MQTT TSet</div>
        </div>
      </div>
    </div>

    <div class="container">
      <div class="grid">
        <a href="#/logs" class="feature-card">
          <div class="feature-icon logs">📊</div>
          <h3>Live Logs</h3>
          <p>Monitor OpenTherm messages in real-time. View requests and responses between your thermostat and boiler.</p>
        </a>

        <a href="#/diagnostics" class="feature-card">
          <div class="feature-icon" style="background:linear-gradient(135deg,#f59e0b,#d97706)">🔧</div>
          <h3>Diagnostics</h3>
          <p>View real-time boiler diagnostics including temperatures, pressures, and system status.</p>
        </a>

        <a href="#/write" class="feature-card">
          <div class="feature-icon" style="background:linear-gradient(135deg,var(--accent2),#6d28d9)">✏️</div>
          <h3>Manual Write</h3>
          <p>Send WRITE_DATA frames directly to the boiler. Manually control setpoints and other writable parameters.</p>
        </a>

        <a href="#/ota" class="feature-card">
          <div class="feature-icon ota">⬆️</div>
          <h3>OTA Update</h3>
          <p>Upload new firmware over-the-air. View current version, manage rollbacks, and update safely.</p>
        </a>
      </div>
    </div>
  `;
}

function refreshOta() {
  fetch('/ota/status')
    .then(r => r.json())
    .then(d => {
      const versionEl = document.getElementById('version');
      const partitionEl = document.getElementById('partition');
      const uptimeEl = document.getElementById('uptime');

      if (versionEl) versionEl.textContent = d.version;
      if (partitionEl) partitionEl.textContent = d.running_partition;
      if (uptimeEl && d.compile_time) {
        const t = d.compile_time.split(' ');
        uptimeEl.textContent = t[0];
      }
    })
    .catch(() => {});
}

function refreshMqtt() {
  fetch('/api/mqtt_state')
    .then(r => r.json())
    .then(d => {
      const statusEl = document.getElementById('mqtt-status');
      const tsetEl = document.getElementById('mqtt-tset');

      if (statusEl) {
        statusEl.textContent = d.connected ? 'Connected' : 'Offline';
      }
      if (tsetEl) {
        if (d.last_tset_valid) {
          tsetEl.textContent = d.last_tset.toFixed(1) + '°C';
        } else {
          tsetEl.textContent = '--';
        }
      }
    })
    .catch(() => {
      const statusEl = document.getElementById('mqtt-status');
      if (statusEl) statusEl.textContent = 'Offline';
    });
}

export function initDashboard() {
  refreshOta();
  refreshMqtt();
  otaInterval = setInterval(refreshOta, 30000);
  mqttInterval = setInterval(refreshMqtt, 5000);
}

export function destroyDashboard() {
  if (otaInterval) {
    clearInterval(otaInterval);
    otaInterval = null;
  }
  if (mqttInterval) {
    clearInterval(mqttInterval);
    mqttInterval = null;
  }
}
