/**
 * Diagnostics page - Real-time boiler state monitoring
 */

let diagInterval = null;
let controlInterval = null;

export function renderDiagnostics() {
  return `
    <div class="container">
      <h1>Boiler Diagnostics</h1>
      <p class="subtitle">Real-time boiler state monitoring</p>
      <div class="card">
        <div class="card-title">Control Mode</div>
        <div id="control-info">--</div>
      </div>
      <div class="section-title">Temperatures</div>
      <div class="diagnostics-grid" id="temps"></div>
      <div class="section-title">Status</div>
      <div class="diagnostics-grid" id="status"></div>
      <div class="section-title">Faults</div>
      <div class="diagnostics-grid" id="faults"></div>
      <div class="section-title">Statistics</div>
      <div class="diagnostics-grid" id="stats"></div>
      <div class="section-title">Fans & CO2</div>
      <div class="diagnostics-grid" id="fans"></div>
      <div class="section-title">All Values</div>
      <table class="all-values-table" id="all-values"></table>
    </div>
  `;
}

function formatValue(val, valid, unit = '') {
  if (!valid || val === undefined || val === null) {
    return '<span class="diag-value invalid">--</span>';
  }
  return `<span class="diag-value">${val.toFixed(1)}${unit}</span>`;
}

function formatTimestamp(ageMs) {
  if (!ageMs || ageMs < 0) return '<span class="diag-timestamp">Never</span>';
  if (ageMs < 60000) return `<span class="diag-timestamp">${(ageMs / 1000).toFixed(0)}s ago</span>`;
  if (ageMs < 3600000) return `<span class="diag-timestamp">${(ageMs / 60000).toFixed(0)}m ago</span>`;
  return `<span class="diag-timestamp">${(ageMs / 3600000).toFixed(1)}h ago</span>`;
}

function formatAge(ageMs) {
  if (!ageMs || ageMs < 0) return 'Never';
  if (ageMs < 60000) return `${(ageMs / 1000).toFixed(0)}s ago`;
  if (ageMs < 3600000) return `${(ageMs / 60000).toFixed(0)}m ago`;
  return `${(ageMs / 3600000).toFixed(1)}h ago`;
}

function renderDiagCard(label, val, unit) {
  return `
    <div class="diag-card">
      <div class="diag-label">${label}</div>
      ${formatValue(val?.value, val?.valid, unit)}
      ${formatTimestamp(val?.age_ms)}
    </div>
  `;
}

function updateDiagnostics() {
  fetch('/api/diagnostics')
    .then(r => r.json())
    .then(d => {
      const temps = document.getElementById('temps');
      const status = document.getElementById('status');
      const faults = document.getElementById('faults');
      const stats = document.getElementById('stats');
      const fans = document.getElementById('fans');
      const allValues = document.getElementById('all-values');

      if (!temps || !status || !faults || !stats || !fans || !allValues) return;

      temps.innerHTML = '';
      status.innerHTML = '';
      faults.innerHTML = '';
      stats.innerHTML = '';
      fans.innerHTML = '';
      allValues.innerHTML = '';

      // Temperatures
      if (d.t_boiler) temps.innerHTML += renderDiagCard('Boiler Temp', d.t_boiler, '°C');
      if (d.t_return) temps.innerHTML += renderDiagCard('Return Temp', d.t_return, '°C');
      if (d.t_dhw) temps.innerHTML += renderDiagCard('DHW Temp', d.t_dhw, '°C');
      if (d.t_outside) temps.innerHTML += renderDiagCard('Outside Temp', d.t_outside, '°C');
      if (d.t_exhaust) temps.innerHTML += renderDiagCard('Exhaust Temp', d.t_exhaust, '°C');
      if (d.t_setpoint) temps.innerHTML += renderDiagCard('Setpoint Temp', d.t_setpoint, '°C');

      // Status
      if (d.modulation_level) status.innerHTML += renderDiagCard('Modulation Level', d.modulation_level, '%');
      if (d.pressure) status.innerHTML += renderDiagCard('Pressure', d.pressure, 'bar');
      if (d.flow_rate) status.innerHTML += renderDiagCard('DHW Flow Rate', d.flow_rate, 'L/min');
      if (d.flame_on) {
        const flameStatus = d.flame_on.value > 0.5 ? '🔥 ON' : 'OFF';
        status.innerHTML += `
          <div class="diag-card">
            <div class="diag-label">Flame Status</div>
            <span class="diag-value">${flameStatus}</span>
            ${formatTimestamp(d.flame_on.age_ms)}
          </div>
        `;
      }
      if (d.ch_mode) {
        const chStatus = d.ch_mode.value > 0.5 ? 'ACTIVE' : 'OFF';
        status.innerHTML += `
          <div class="diag-card">
            <div class="diag-label">CH Mode</div>
            <span class="diag-value">${chStatus}</span>
            ${formatTimestamp(d.ch_mode.age_ms)}
          </div>
        `;
      }
      if (d.dhw_mode) {
        const dhwStatus = d.dhw_mode.value > 0.5 ? 'ACTIVE' : 'OFF';
        status.innerHTML += `
          <div class="diag-card">
            <div class="diag-label">DHW Mode</div>
            <span class="diag-value">${dhwStatus}</span>
            ${formatTimestamp(d.dhw_mode.age_ms)}
          </div>
        `;
      }

      // Faults
      if (d.fault_code) faults.innerHTML += renderDiagCard('Fault Code', d.fault_code, '');
      if (d.diag_code) faults.innerHTML += renderDiagCard('Diagnostic Code', d.diag_code, '');

      // Statistics
      if (d.burner_starts) stats.innerHTML += renderDiagCard('Burner Starts', d.burner_starts, '');
      if (d.burner_hours) stats.innerHTML += renderDiagCard('Burner Hours', d.burner_hours, 'h');

      // Fans & CO2
      if (d.fan_current) fans.innerHTML += renderDiagCard('Fan Speed', d.fan_current, '%');
      if (d.co2_exhaust) fans.innerHTML += renderDiagCard('CO2 Exhaust', d.co2_exhaust, 'ppm');

      // All values table
      const allItems = [
        { key: 'Boiler Temp', val: d.t_boiler, unit: '°C' },
        { key: 'Return Temp', val: d.t_return, unit: '°C' },
        { key: 'DHW Temp', val: d.t_dhw, unit: '°C' },
        { key: 'DHW Temp 2', val: d.t_dhw2, unit: '°C' },
        { key: 'Outside Temp', val: d.t_outside, unit: '°C' },
        { key: 'Exhaust Temp', val: d.t_exhaust, unit: '°C' },
        { key: 'Heat Exchanger Temp', val: d.t_heat_exchanger, unit: '°C' },
        { key: 'CH2 Flow Temp', val: d.t_flow_ch2, unit: '°C' },
        { key: 'Storage Temp', val: d.t_storage, unit: '°C' },
        { key: 'Collector Temp', val: d.t_collector, unit: '°C' },
        { key: 'Setpoint Temp', val: d.t_setpoint, unit: '°C' },
        { key: 'Modulation Level', val: d.modulation_level, unit: '%' },
        { key: 'Pressure', val: d.pressure, unit: 'bar' },
        { key: 'DHW Flow Rate', val: d.flow_rate, unit: 'L/min' },
        { key: 'Flame Status', val: d.flame_on, unit: '', format: (v) => v > 0.5 ? '🔥 ON' : 'OFF' },
        { key: 'CH Mode', val: d.ch_mode, unit: '', format: (v) => v > 0.5 ? 'ACTIVE' : 'OFF' },
        { key: 'DHW Mode', val: d.dhw_mode, unit: '', format: (v) => v > 0.5 ? 'ACTIVE' : 'OFF' },
        { key: 'Fault Code', val: d.fault_code, unit: '' },
        { key: 'Diagnostic Code', val: d.diag_code, unit: '' },
        { key: 'Burner Starts', val: d.burner_starts, unit: '' },
        { key: 'DHW Burner Starts', val: d.dhw_burner_starts, unit: '' },
        { key: 'CH Pump Starts', val: d.ch_pump_starts, unit: '' },
        { key: 'DHW Pump Starts', val: d.dhw_pump_starts, unit: '' },
        { key: 'Burner Hours', val: d.burner_hours, unit: 'h' },
        { key: 'DHW Burner Hours', val: d.dhw_burner_hours, unit: 'h' },
        { key: 'CH Pump Hours', val: d.ch_pump_hours, unit: 'h' },
        { key: 'DHW Pump Hours', val: d.dhw_pump_hours, unit: 'h' },
        { key: 'Max Capacity', val: d.max_capacity, unit: 'kW' },
        { key: 'Min Mod Level', val: d.min_mod_level, unit: '%' },
        { key: 'Fan Setpoint', val: d.fan_setpoint, unit: '%' },
        { key: 'Fan Current', val: d.fan_current, unit: '%' },
        { key: 'Fan Exhaust RPM', val: d.fan_exhaust_rpm, unit: 'rpm' },
        { key: 'Fan Supply RPM', val: d.fan_supply_rpm, unit: 'rpm' },
        { key: 'CO2 Exhaust', val: d.co2_exhaust, unit: 'ppm' }
      ];

      allItems.forEach(item => {
        if (item.val && item.val.valid) {
          const row = allValues.insertRow();
          row.insertCell(0).textContent = item.key;
          const valueText = item.format 
            ? `${item.format(item.val.value)} (${formatAge(item.val.age_ms)})`
            : `${item.val.value.toFixed(1)}${item.unit} (${formatAge(item.val.age_ms)})`;
          row.insertCell(1).textContent = valueText;
        }
      });
    })
    .catch(err => {});
}

function updateControlStatus() {
  fetch('/api/control_mode')
    .then(r => r.json())
    .then(d => {
      const el = document.getElementById('control-info');
      if (!el) return;

      let text = 'Control disabled';
      if (d.enabled) {
        const statusStr = d.active ? 'ACTIVE' : (d.fallback ? 'Fallback (passthrough)' : 'Idle');
        const tsetStr = d.demand_tset ? d.demand_tset.toFixed(1) + '°C' : '--';
        const chStr = d.demand_ch ? 'ON' : 'OFF';
        text = `Control ${statusStr} | TSet: ${tsetStr} | CH: ${chStr}`;
      }
      el.textContent = text;
    })
    .catch(() => {});
}

export function initDiagnostics() {
  updateDiagnostics();
  updateControlStatus();
  diagInterval = setInterval(updateDiagnostics, 2000);
  controlInterval = setInterval(updateControlStatus, 5000);
}

export function destroyDiagnostics() {
  if (diagInterval) {
    clearInterval(diagInterval);
    diagInterval = null;
  }
  if (controlInterval) {
    clearInterval(controlInterval);
    controlInterval = null;
  }
}
