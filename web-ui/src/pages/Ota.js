/**
 * OTA page - Over-the-air firmware management
 */

let statusInterval = null;
let uploading = false;

export function renderOta() {
  return `
    <div class="container">
      <h1>Firmware Update</h1>
      <p class="subtitle">Over-the-air firmware management</p>
      <div id="pending-warning" class="warning" style="display:none">Warning: Firmware is pending verification. If you don't confirm it, the device will rollback on next reboot.</div>
      <div class="grid">
        <div class="card">
          <div class="card-title">Current Firmware</div>
          <div class="info-grid">
            <span class="info-label">Version</span><span class="info-value" id="version">--</span>
            <span class="info-label">Project</span><span class="info-value" id="project">--</span>
            <span class="info-label">Compiled</span><span class="info-value" id="compile-time">--</span>
            <span class="info-label">IDF Version</span><span class="info-value" id="idf-ver">--</span>
            <span class="info-label">State</span><span class="info-value"><span class="badge green" id="state">--</span></span>
          </div>
          <div class="partition-visual">
            <div class="partition-box" id="ota0-box"><div class="partition-name">ota_0</div><div class="partition-addr" id="ota0-addr">--</div></div>
            <div class="partition-box" id="ota1-box"><div class="partition-name">ota_1</div><div class="partition-addr" id="ota1-addr">--</div></div>
          </div>
          <div class="actions">
            <button class="btn btn-secondary" id="confirm-btn" style="display:none">Confirm Firmware</button>
            <button class="btn btn-danger" id="rollback-btn">Rollback</button>
          </div>
        </div>
        <div class="card">
          <div class="card-title">Upload New Firmware</div>
          <div class="upload-zone" id="upload-zone">
            <div class="upload-icon">📁</div>
            <div class="upload-text">Drop firmware.bin here or click to browse</div>
            <div class="upload-hint">Max size: ~1.7 MB (partition size)</div>
          </div>
          <input type="file" id="file-input" accept=".bin">
          <div class="progress-container" id="progress-container">
            <div class="progress-bar"><div class="progress-fill" id="progress-fill"></div></div>
            <div class="progress-text" id="progress-text">Uploading... 0%</div>
          </div>
          <div class="status-msg" id="status-msg"></div>
        </div>
      </div>
    </div>
  `;
}

function showStatus(msg, type) {
  const el = document.getElementById('status-msg');
  if (el) {
    el.textContent = msg;
    el.className = 'status-msg ' + type;
  }
}

function hideStatus() {
  const el = document.getElementById('status-msg');
  if (el) el.className = 'status-msg';
}

function loadStatus() {
  if (uploading) return;

  fetch('/ota/status')
    .then(r => r.json())
    .then(d => {
      const versionEl = document.getElementById('version');
      const projectEl = document.getElementById('project');
      const compileEl = document.getElementById('compile-time');
      const idfEl = document.getElementById('idf-ver');
      const stateEl = document.getElementById('state');
      const ota0Box = document.getElementById('ota0-box');
      const ota1Box = document.getElementById('ota1-box');
      const ota0Addr = document.getElementById('ota0-addr');
      const ota1Addr = document.getElementById('ota1-addr');
      const warningEl = document.getElementById('pending-warning');
      const confirmBtn = document.getElementById('confirm-btn');

      if (versionEl) versionEl.textContent = d.version;
      if (projectEl) projectEl.textContent = d.project_name;
      if (compileEl) compileEl.textContent = d.compile_time;
      if (idfEl) idfEl.textContent = d.idf_ver;

      if (stateEl) {
        stateEl.textContent = d.ota_state;
        let badgeClass = 'badge ';
        if (d.ota_state === 'valid') badgeClass += 'green';
        else if (d.ota_state === 'pending_verify') badgeClass += 'yellow';
        else badgeClass += 'purple';
        stateEl.className = badgeClass;
      }

      const run = d.running_partition;
      if (ota0Box) ota0Box.className = 'partition-box ' + (run === 'ota_0' ? 'active' : 'inactive');
      if (ota1Box) ota1Box.className = 'partition-box ' + (run === 'ota_1' ? 'active' : 'inactive');
      if (ota0Addr) ota0Addr.textContent = run === 'ota_0' ? 'Running' : 'Next update';
      if (ota1Addr) ota1Addr.textContent = run === 'ota_1' ? 'Running' : 'Next update';

      if (d.ota_state === 'pending_verify') {
        if (warningEl) warningEl.style.display = 'block';
        if (confirmBtn) confirmBtn.style.display = 'inline-flex';
      } else {
        if (warningEl) warningEl.style.display = 'none';
        if (confirmBtn) confirmBtn.style.display = 'none';
      }
    })
    .catch(() => {});
}

function handleFile(file) {
  if (!file || !file.name.endsWith('.bin')) {
    showStatus('Please select a .bin file', 'error');
    return;
  }
  if (file.size > 1800000) {
    showStatus('File too large for partition', 'error');
    return;
  }
  uploadFirmware(file);
}

function uploadFirmware(file) {
  if (uploading) return;
  uploading = true;

  if (statusInterval) {
    clearInterval(statusInterval);
    statusInterval = null;
  }

  const xhr = new XMLHttpRequest();
  const progress = document.getElementById('progress-container');
  const fill = document.getElementById('progress-fill');
  const text = document.getElementById('progress-text');
  const zone = document.getElementById('upload-zone');

  if (progress) progress.style.display = 'block';
  if (zone) zone.classList.add('uploading');
  hideStatus();

  xhr.upload.onprogress = (e) => {
    if (e.lengthComputable) {
      const pct = Math.round(e.loaded / e.total * 100);
      if (fill) fill.style.width = pct + '%';
      if (text) text.textContent = 'Uploading... ' + pct + '%';
    }
  };

  xhr.onload = () => {
    if (zone) zone.classList.remove('uploading');
    uploading = false;

    if (xhr.status === 200) {
      const r = JSON.parse(xhr.responseText);
      showStatus('Success: ' + r.message, 'success');
      if (text) text.textContent = 'Complete! Restarting...';
      setTimeout(() => location.reload(), 5000);
    } else {
      showStatus('Upload failed: ' + xhr.responseText, 'error');
      if (progress) progress.style.display = 'none';
      startStatusPolling();
    }
  };

  xhr.onerror = () => {
    if (zone) zone.classList.remove('uploading');
    uploading = false;
    showStatus('Network error', 'error');
    if (progress) progress.style.display = 'none';
    startStatusPolling();
  };

  xhr.open('POST', '/ota');
  xhr.timeout = 120000;
  xhr.send(file);
}

function rollback() {
  if (!confirm('Rollback to previous firmware? Device will restart.')) return;

  fetch('/ota/rollback', { method: 'POST' })
    .then(r => r.json())
    .then(d => {
      showStatus(d.message, 'success');
      setTimeout(() => location.reload(), 3000);
    })
    .catch(() => showStatus('Rollback failed', 'error'));
}

function confirmFirmware() {
  fetch('/ota/confirm', { method: 'POST' })
    .then(r => r.json())
    .then(d => {
      showStatus(d.message, 'success');
      loadStatus();
    })
    .catch(() => showStatus('Confirm failed', 'error'));
}

function startStatusPolling() {
  if (!statusInterval) {
    statusInterval = setInterval(loadStatus, 10000);
  }
}

export function initOta() {
  loadStatus();
  startStatusPolling();

  const zone = document.getElementById('upload-zone');
  const fileInput = document.getElementById('file-input');
  const rollbackBtn = document.getElementById('rollback-btn');
  const confirmBtn = document.getElementById('confirm-btn');

  if (zone) {
    zone.addEventListener('click', () => fileInput?.click());

    zone.addEventListener('dragover', (e) => {
      e.preventDefault();
      zone.classList.add('dragover');
    });

    zone.addEventListener('dragleave', () => {
      zone.classList.remove('dragover');
    });

    zone.addEventListener('drop', (e) => {
      e.preventDefault();
      zone.classList.remove('dragover');
      if (e.dataTransfer.files.length) {
        handleFile(e.dataTransfer.files[0]);
      }
    });
  }

  if (fileInput) {
    fileInput.addEventListener('change', () => {
      if (fileInput.files.length) {
        handleFile(fileInput.files[0]);
      }
    });
  }

  if (rollbackBtn) rollbackBtn.addEventListener('click', rollback);
  if (confirmBtn) confirmBtn.addEventListener('click', confirmFirmware);
}

export function destroyOta() {
  if (statusInterval) {
    clearInterval(statusInterval);
    statusInterval = null;
  }
  uploading = false;
}
