/**
 * Logs page - Real-time OpenTherm message monitor
 */

import { ID_DECODERS, fmtHex } from '../lib/opentherm.js';

let ws = null;
let logsContainer = null;
let paused = false;
let msgCount = 0;
let allMessages = [];
let sourceFilter = 'all';
let dataIdFilter = null;

const SOURCE_LABELS = {
  'thermostat-boiler': 'T↔B',
  'gateway-boiler': 'G↔B',
  'thermostat-gateway': 'T↔G'
};

export function renderLogs() {
  return `
    <div class="container">
      <h1>Live Logs</h1>
      <p class="subtitle">Real-time OpenTherm message monitor</p>
      <div class="card">
        <div class="toolbar">
          <div class="status-indicator">
            <div class="status-dot" id="status-dot"></div>
            <span id="status-text">Disconnected</span>
          </div>
          <div class="filter-group">
            <select class="filter-select" id="source-filter">
              <option value="all">All Sources</option>
              <option value="THERMOSTAT_BOILER">T↔B Proxied</option>
              <option value="GATEWAY_BOILER">G↔B Gateway</option>
              <option value="THERMOSTAT_GATEWAY">T↔G Control</option>
            </select>
            <input type="number" class="filter-input" id="dataid-filter" placeholder="ID" min="0" max="255">
          </div>
          <button class="btn btn-secondary" id="pause-btn">Pause</button>
          <button class="btn btn-danger" id="clear-btn">Clear</button>
          <span class="msg-count"><span id="msg-count">0</span>&nbsp;messages</span>
        </div>
        <div class="log-container" id="logs"></div>
      </div>
    </div>
  `;
}

function passesFilter(d) {
  if (sourceFilter !== 'all' && d.source !== sourceFilter) return false;
  if (dataIdFilter !== null && d.data_id !== dataIdFilter) return false;
  return true;
}

function formatMessage(d) {
  // Use the pre-parsed data from the websocket instead of re-parsing
  const msgType = d.msg_type || 'UNKNOWN';
  const dataId = d.data_id;
  const dataValue = d.data_value;

  // Decode the payload using the ID decoder if available
  const decoder = ID_DECODERS[dataId];
  let payload, idName;
  if (decoder) {
    payload = decoder.decode(dataValue);
    idName = decoder.name;
  } else {
    const hb = (dataValue >> 8) & 0xFF;
    const lb = dataValue & 0xFF;
    payload = `DATA-VALUE=${fmtHex(dataValue, 4)} (HB=${fmtHex(hb, 2)}, LB=${fmtHex(lb, 2)})`;
    idName = 'Unknown/Unimplemented';
  }

  return `${msgType} (id=${dataId} ${idName}) ${payload}`;
}

function appendLogEntry(d) {
  if (!logsContainer) return;

  const div = document.createElement('div');
  const ts = new Date().toLocaleTimeString('en-GB', { hour12: false });
  const src = (d.source || 'THERMOSTAT_BOILER').toLowerCase().replace(/_/g, '-');
  const srcLabel = SOURCE_LABELS[src] || 'T↔B';

  div.className = `log-entry ${(d.direction || '').toLowerCase()} ${src}`;

  const decodedContent = formatMessage(d);

  div.innerHTML = `
    <span class="log-time">${ts}</span>
    <span class="log-source ${src}">${srcLabel}</span>
    <span class="log-dir ${(d.direction || '').toLowerCase()}">${d.direction || ''}</span>
    <span class="log-content">${decodedContent}</span>
  `;

  logsContainer.appendChild(div);

  if (logsContainer.children.length > 500) {
    logsContainer.removeChild(logsContainer.firstChild);
  }

  logsContainer.scrollTop = logsContainer.scrollHeight;
}

function connect() {
  ws = new WebSocket('ws://' + window.location.host + '/ws');

  ws.onopen = () => {
    const dot = document.getElementById('status-dot');
    const text = document.getElementById('status-text');
    if (dot) dot.classList.add('connected');
    if (text) text.textContent = 'Connected';
  };

  ws.onclose = () => {
    const dot = document.getElementById('status-dot');
    const text = document.getElementById('status-text');
    if (dot) dot.classList.remove('connected');
    if (text) text.textContent = 'Disconnected';
    setTimeout(connect, 2000);
  };

  ws.onmessage = (e) => {
    if (paused) return;

    msgCount++;
    const countEl = document.getElementById('msg-count');
    if (countEl) countEl.textContent = msgCount;

    try {
      const d = JSON.parse(e.data);
      allMessages.push({ data: d, timestamp: new Date() });
      if (allMessages.length > 500) allMessages.shift();
      if (!passesFilter(d)) return;
      appendLogEntry(d);
    } catch (err) {
      const div = document.createElement('div');
      const ts = new Date().toLocaleTimeString('en-GB', { hour12: false });
      div.className = 'log-entry status';
      div.innerHTML = `<span class="log-time">${ts}</span><span class="log-content">${e.data}</span>`;
      if (logsContainer) {
        logsContainer.appendChild(div);
        if (logsContainer.children.length > 500) {
          logsContainer.removeChild(logsContainer.firstChild);
        }
        logsContainer.scrollTop = logsContainer.scrollHeight;
      }
    }
  };
}

function applyFilters() {
  const sourceSelect = document.getElementById('source-filter');
  const idInput = document.getElementById('dataid-filter');

  sourceFilter = sourceSelect ? sourceSelect.value : 'all';
  const idVal = idInput ? idInput.value : '';
  dataIdFilter = idVal !== '' ? parseInt(idVal) : null;

  if (logsContainer) {
    logsContainer.innerHTML = '';
    allMessages.forEach(msg => {
      if (passesFilter(msg.data)) appendLogEntry(msg.data);
    });
  }
}

function togglePause() {
  paused = !paused;
  const btn = document.getElementById('pause-btn');
  if (btn) btn.textContent = paused ? 'Resume' : 'Pause';
}

function clearLogs() {
  if (logsContainer) logsContainer.innerHTML = '';
  allMessages = [];
  msgCount = 0;
  const countEl = document.getElementById('msg-count');
  if (countEl) countEl.textContent = '0';
}

export function initLogs() {
  logsContainer = document.getElementById('logs');

  // Set up event listeners
  const sourceSelect = document.getElementById('source-filter');
  const idInput = document.getElementById('dataid-filter');
  const pauseBtn = document.getElementById('pause-btn');
  const clearBtn = document.getElementById('clear-btn');

  if (sourceSelect) sourceSelect.addEventListener('change', applyFilters);
  if (idInput) idInput.addEventListener('change', applyFilters);
  if (pauseBtn) pauseBtn.addEventListener('click', togglePause);
  if (clearBtn) clearBtn.addEventListener('click', clearLogs);

  connect();
}

export function destroyLogs() {
  if (ws) {
    ws.onclose = null; // Prevent reconnection
    ws.close();
    ws = null;
  }
  logsContainer = null;
  paused = false;
  msgCount = 0;
  allMessages = [];
  sourceFilter = 'all';
  dataIdFilter = null;
}
