/**
 * Write page - Manual WRITE_DATA frame submission
 */

export function renderWrite() {
  return `
    <div class="container">
      <h1>Manual WRITE_DATA Frame</h1>
      <p class="subtitle">Send WRITE_DATA frames directly to the boiler</p>
      <div class="card">
        <div class="preset-buttons" id="presets">
          <button class="preset-btn" data-id="1" data-type="float" data-value="20.0">TSet (20°C)</button>
          <button class="preset-btn" data-id="1" data-type="float" data-value="30.0">TSet (30°C)</button>
          <button class="preset-btn" data-id="1" data-type="float" data-value="40.0">TSet (40°C)</button>
          <button class="preset-btn" data-id="1" data-type="float" data-value="50.0">TSet (50°C)</button>
          <button class="preset-btn" data-id="8" data-type="float" data-value="30.0">TSetCH2 (30°C)</button>
          <button class="preset-btn" data-id="16" data-type="float" data-value="20.0">Troom Setpoint (20°C)</button>
          <button class="preset-btn" data-id="14" data-type="uint16" data-value="50">Max Mod Level (50%)</button>
        </div>
        <form id="write-form">
          <div class="form-row">
            <div class="form-group">
              <label class="form-label">Data ID</label>
              <select id="data_id" class="form-select" required>
                <option value="1">1 - TSet (Control Setpoint)</option>
                <option value="2">2 - Master Configuration</option>
                <option value="4">4 - Command</option>
                <option value="6">6 - Remote Override</option>
                <option value="7">7 - Cooling Control</option>
                <option value="8">8 - TSetCH2</option>
                <option value="9">9 - Troom Override</option>
                <option value="10">10 - TSP (Setpoint Override)</option>
                <option value="14">14 - Max Rel Mod Level Setting</option>
                <option value="16">16 - Troom Setpoint</option>
                <option value="custom">Custom...</option>
              </select>
              <div class="form-help">Select the data ID to write</div>
            </div>
            <div class="form-group" id="custom_id_group" style="display:none">
              <label class="form-label">Custom Data ID</label>
              <input type="number" id="custom_data_id" class="form-input" min="0" max="255" placeholder="0-255">
            </div>
          </div>
          <div class="form-row">
            <div class="form-group">
              <label class="form-label">Data Type</label>
              <select id="data_type" class="form-select" required>
                <option value="float">Float (f8.8) - Temperature</option>
                <option value="uint16">Uint16 - Raw value</option>
                <option value="flags">Flags - Bit field</option>
              </select>
              <div class="form-help">Format of the data value</div>
            </div>
            <div class="form-group">
              <label class="form-label">Data Value</label>
              <input type="text" id="data_value" class="form-input" required placeholder="e.g., 20.5 or 12345">
              <div class="form-help">For float: temperature in °C. For uint16: 0-65535. For flags: hex (0x1234)</div>
            </div>
          </div>
          <button type="submit" class="btn btn-primary">Send WRITE_DATA Frame</button>
        </form>
        <div id="response" class="response-box" style="display:none"></div>
      </div>
    </div>
  `;
}

function setPreset(id, type, value) {
  const dataIdEl = document.getElementById('data_id');
  const dataTypeEl = document.getElementById('data_type');
  const dataValueEl = document.getElementById('data_value');
  const customGroup = document.getElementById('custom_id_group');

  if (dataIdEl) dataIdEl.value = id.toString();
  if (dataTypeEl) dataTypeEl.value = type;
  if (dataValueEl) dataValueEl.value = value.toString();
  if (customGroup) customGroup.style.display = 'none';
}

function handleDataIdChange(e) {
  const customGroup = document.getElementById('custom_id_group');
  if (customGroup) {
    customGroup.style.display = e.target.value === 'custom' ? 'block' : 'none';
  }
}

function handleWriteSubmit(e) {
  e.preventDefault();

  let dataId = document.getElementById('data_id').value;
  if (dataId === 'custom') {
    dataId = document.getElementById('custom_data_id').value;
  }

  const dataType = document.getElementById('data_type').value;
  const dataValue = document.getElementById('data_value').value;
  const responseBox = document.getElementById('response');

  if (responseBox) {
    responseBox.style.display = 'block';
    responseBox.className = 'response-box';
    responseBox.textContent = 'Sending...';
  }

  let parsedValue;
  if (dataType === 'float') {
    parsedValue = parseFloat(dataValue);
  } else if (dataType === 'flags') {
    parsedValue = parseInt(dataValue, 16);
  } else {
    parsedValue = parseInt(dataValue);
  }

  const payload = {
    data_id: parseInt(dataId),
    data_value: parsedValue,
    data_type: dataType
  };

  fetch('/api/write', {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify(payload)
  })
    .then(r => r.json())
    .then(d => {
      if (!responseBox) return;

      if (d.success) {
        responseBox.className = 'response-box success';
        const frameHex = d.response.frame.toString(16).toUpperCase().padStart(8, '0');
        responseBox.textContent = `Success!
Request: Data ID=${d.request.data_id}, Value=${d.request.data_value}
Response Frame: 0x${frameHex}
Response Type: ${d.response.type}
Response Data ID: ${d.response.data_id}
Response Data Value: ${d.response.data_value}`;
      } else {
        responseBox.className = 'response-box error';
        responseBox.textContent = `Error: ${d.error} (code: ${d.error_code})`;
      }
    })
    .catch(err => {
      if (responseBox) {
        responseBox.className = 'response-box error';
        responseBox.textContent = `Network error: ${err.message}`;
      }
    });
}

export function initWrite() {
  const dataIdEl = document.getElementById('data_id');
  const writeForm = document.getElementById('write-form');
  const presetsEl = document.getElementById('presets');

  if (dataIdEl) dataIdEl.addEventListener('change', handleDataIdChange);
  if (writeForm) writeForm.addEventListener('submit', handleWriteSubmit);

  // Set up preset buttons
  if (presetsEl) {
    presetsEl.addEventListener('click', (e) => {
      if (e.target.classList.contains('preset-btn')) {
        const id = e.target.dataset.id;
        const type = e.target.dataset.type;
        const value = e.target.dataset.value;
        setPreset(id, type, value);
      }
    });
  }
}

export function destroyWrite() {
  // No intervals to clean up
}
