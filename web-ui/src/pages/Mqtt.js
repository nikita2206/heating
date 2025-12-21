/**
 * MQTT page - Configure broker and topics for external overrides
 */

export function renderMqtt() {
  return `
    <div class="container">
      <h1>MQTT Overrides</h1>
      <p class="subtitle">Configure broker and topics for external overrides</p>
      <div class="card">
        <div class="status-chip" id="mqtt-chip">--</div>
        <form id="mqtt-form">
          <label class="form-switch">
            <input type="checkbox" id="enable"> <span>Enable MQTT bridge</span>
          </label>
          <div class="form-group">
            <label class="form-label">Broker URI</label>
            <input class="form-input" id="broker_uri" placeholder="mqtt://host:1883">
          </div>
          <div class="form-group">
            <label class="form-label">Client ID</label>
            <input class="form-input" id="client_id" placeholder="ot-gateway">
          </div>
          <div class="form-group">
            <label class="form-label">Username</label>
            <input class="form-input" id="username" placeholder="(optional)">
          </div>
          <div class="form-group">
            <label class="form-label">Password</label>
            <input class="form-input" id="password" type="password" placeholder="(optional)">
          </div>
          <div class="form-group">
            <label class="form-label">Base Topic</label>
            <input class="form-input" id="base_topic" placeholder="ot_gateway">
          </div>
          <div class="form-group">
            <label class="form-label">Discovery Prefix</label>
            <input class="form-input" id="discovery_prefix" placeholder="homeassistant">
          </div>
          <button type="submit" class="btn btn-primary">Save & Restart MQTT</button>
          <small>Changes are stored in NVS and applied immediately.</small>
        </form>
      </div>
      <div class="card">
        <h3 style="margin-bottom:12px">Control Mode</h3>
        <form id="control-form">
          <label class="form-switch">
            <input type="checkbox" id="control_enable"> <span>Enable control mode (MQTT overrides)</span>
          </label>
          <div class="form-group">
            <div class="status-chip" id="control-chip">--</div>
            <div id="control-demand" class="form-help"></div>
          </div>
          <button type="submit" class="btn btn-secondary">Apply</button>
        </form>
      </div>
    </div>
  `;
}

function loadConfig() {
  fetch('/api/mqtt_config')
    .then(r => r.json())
    .then(d => {
      const enableEl = document.getElementById('enable');
      const brokerEl = document.getElementById('broker_uri');
      const clientEl = document.getElementById('client_id');
      const userEl = document.getElementById('username');
      const prefixEl = document.getElementById('discovery_prefix');
      const topicEl = document.getElementById('base_topic');
      const chip = document.getElementById('mqtt-chip');

      if (enableEl) enableEl.checked = d.enable;
      if (brokerEl) brokerEl.value = d.broker_uri || '';
      if (clientEl) clientEl.value = d.client_id || '';
      if (userEl) userEl.value = d.username || '';
      if (prefixEl) prefixEl.value = d.discovery_prefix || 'homeassistant';
      if (topicEl) topicEl.value = d.base_topic || '';

      if (chip) {
        if (d.connected) {
          chip.textContent = 'Connected';
          chip.className = 'status-chip ok';
        } else {
          chip.textContent = 'Offline';
          chip.className = 'status-chip bad';
        }
      }
    })
    .catch(() => {});
}

function loadControl() {
  fetch('/api/control_mode')
    .then(r => r.json())
    .then(d => {
      const enableEl = document.getElementById('control_enable');
      const chip = document.getElementById('control-chip');
      const demandEl = document.getElementById('control-demand');

      if (enableEl) enableEl.checked = d.enabled;

      if (chip) {
        let status = 'Offline';
        let cls = 'status-chip bad';
        if (d.enabled) {
          status = d.active ? 'Active' : (d.fallback ? 'Fallback (passthrough)' : 'Idle');
          cls = d.active ? 'status-chip ok' : (d.fallback ? 'status-chip bad' : 'status-chip');
        }
        chip.textContent = status;
        chip.className = cls;
      }

      if (demandEl) {
        const tsetStr = d.demand_tset ? d.demand_tset.toFixed(1) + '°C' : '--';
        const chStr = d.demand_ch ? 'ON' : 'OFF';
        demandEl.textContent = `Demanded TSet: ${tsetStr}, CH: ${chStr}`;
      }
    })
    .catch(() => {});
}

function handleMqttSubmit(e) {
  e.preventDefault();

  const params = new URLSearchParams();
  params.append('enable', document.getElementById('enable').checked ? 'on' : 'off');
  params.append('broker_uri', document.getElementById('broker_uri').value);
  params.append('client_id', document.getElementById('client_id').value);
  params.append('username', document.getElementById('username').value);
  params.append('password', document.getElementById('password').value);
  params.append('base_topic', document.getElementById('base_topic').value);

  fetch('/api/mqtt_config', {
    method: 'POST',
    headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
    body: params.toString()
  })
    .then(r => r.json())
    .then(() => {
      loadConfig();
      alert('Saved and restarted');
    })
    .catch(() => alert('Failed to save'));
}

function handleControlSubmit(e) {
  e.preventDefault();

  const params = new URLSearchParams();
  params.append('enabled', document.getElementById('control_enable').checked ? 'on' : 'off');

  fetch('/api/control_mode', {
    method: 'POST',
    headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
    body: params.toString()
  })
    .then(() => loadControl())
    .catch(() => alert('Failed to toggle control mode'));
}

export function initMqtt() {
  loadConfig();
  loadControl();

  const mqttForm = document.getElementById('mqtt-form');
  const controlForm = document.getElementById('control-form');

  if (mqttForm) mqttForm.addEventListener('submit', handleMqttSubmit);
  if (controlForm) controlForm.addEventListener('submit', handleControlSubmit);
}

export function destroyMqtt() {
  // No intervals to clean up
}
