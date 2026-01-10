let saveBtnHandler = null;

export function renderSettings() {
  return `
    <div class="page-container">
      <h2>Settings</h2>
      
      <div class="card">
        <h3>Heating Configuration</h3>
        
        <div class="form-group">
          <label for="max-setpoint">Max CH Setpoint Proxy Limit (°C)</label>
          <div class="input-group">
            <input type="number" id="max-setpoint" step="0.5" min="20" max="100" placeholder="60.0">
            <button id="save-max-setpoint" class="btn">Save</button>
          </div>
          <small>The gateway will cap any request from the thermostat above this value before sending it to the boiler.</small>
        </div>
      </div>
    </div>
  `;
}

export function initSettings() {
  const input = document.getElementById('max-setpoint');
  const saveBtn = document.getElementById('save-max-setpoint');

  if (!input || !saveBtn) return;

  // Load current value
  fetch('/api/settings')
    .then(res => res.json())
    .then(data => {
      if (data.max_setpoint) {
        input.value = data.max_setpoint;
      }
    })
    .catch(err => console.error('Failed to load settings:', err));

  saveBtnHandler = () => {
    const val = parseFloat(input.value);
    if (isNaN(val) || val < 0 || val > 100) {
      alert('Please enter a valid temperature (0-100)');
      return;
    }

    saveBtn.disabled = true;
    saveBtn.textContent = 'Saving...';

    fetch('/api/settings', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ max_setpoint: val })
    })
    .then(res => {
      if (!res.ok) throw new Error('Failed to save');
      return res.json();
    })
    .then(() => {
      alert('Settings saved');
    })
    .catch(err => {
      alert('Error saving settings: ' + err.message);
    })
    .finally(() => {
      saveBtn.disabled = false;
      saveBtn.textContent = 'Save';
    });
  };

  saveBtn.addEventListener('click', saveBtnHandler);
}

export function destroySettings() {
  const saveBtn = document.getElementById('save-max-setpoint');
  if (saveBtn && saveBtnHandler) {
    saveBtn.removeEventListener('click', saveBtnHandler);
    saveBtnHandler = null;
  }
}