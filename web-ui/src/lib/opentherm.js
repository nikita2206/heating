/**
 * OpenTherm protocol decoder
 */

export const MASTER_MSG_TYPES = {
  0: 'READ-DATA',
  1: 'WRITE-DATA',
  2: 'INVALID-DATA',
  3: 'RESERVED',
  4: 'RESERVED',
  5: 'RESERVED',
  6: 'RESERVED',
  7: 'RESERVED'
};

export const SLAVE_MSG_TYPES = {
  0: 'RESERVED',
  1: 'RESERVED',
  2: 'RESERVED',
  3: 'RESERVED',
  4: 'READ-ACK',
  5: 'WRITE-ACK',
  6: 'DATA-INVALID',
  7: 'UNKNOWN-DATAID'
};

export function u16ToS16(v) {
  return (v & 0x8000) ? v - 0x10000 : v;
}

export function f8_8ToFloat(u16) {
  return u16ToS16(u16) / 256.0;
}

export function fmtHex(v, width) {
  return '0x' + v.toString(16).toUpperCase().padStart(width, '0');
}

export function parityOk(frameBits) {
  let count = 0;
  for (const bit of frameBits) {
    if (bit === '1') count++;
  }
  return (count % 2) === 0;
}

function decodeStatusId0(dataU16) {
  const hb = (dataU16 >> 8) & 0xFF;
  const lb = dataU16 & 0xFF;

  function prettyMaster(bit, val) {
    const on = (val >> bit) & 1;
    const labels = [
      ['CH', 'enabled', 'disabled'],
      ['DHW', 'enabled', 'disabled'],
      ['Cooling', 'enabled', 'disabled'],
      ['OTC', 'active', 'inactive'],
      ['CH2', 'enabled', 'disabled'],
      ['Mode', 'summer', 'winter'],
      ['DHWBlocking', 'blocked', 'unblocked'],
      ['reserved', '', '']
    ];
    const [name, onLabel, offLabel] = labels[bit] || [`bit${bit}`, '1', '0'];
    return `${name}=${on ? onLabel : offLabel}`;
  }

  function prettySlave(bit, val) {
    const on = (val >> bit) & 1;
    const labels = [
      ['Fault', 'yes', 'no'],
      ['CHMode', 'active', 'inactive'],
      ['DHWMode', 'active', 'inactive'],
      ['Flame', 'on', 'off'],
      ['CoolingMode', 'active', 'inactive'],
      ['CH2Mode', 'active', 'inactive'],
      ['Diagnostic', 'event', 'none'],
      ['ElectricityProduction', 'on', 'off']
    ];
    const [name, onLabel, offLabel] = labels[bit] || [`bit${bit}`, '1', '0'];
    return `${name}=${on ? onLabel : offLabel}`;
  }

  const masterDesc = [];
  for (let b = 0; b < 8; b++) masterDesc.push(prettyMaster(b, hb));
  const slaveDesc = [];
  for (let b = 0; b < 8; b++) slaveDesc.push(prettySlave(b, lb));

  return `MasterStatus(HB): ${masterDesc.join('; ')} | SlaveStatus(LB): ${slaveDesc.join('; ')}`;
}

function decodeId1ControlSetpoint(dataU16) {
  const t = f8_8ToFloat(dataU16);
  return `Control Setpoint (Tset) = ${t.toFixed(2)} °C (raw=${fmtHex(dataU16, 4)})`;
}

function decodeId2MasterConfig(dataU16) {
  const hb = (dataU16 >> 8) & 0xFF;
  const lb = dataU16 & 0xFF;
  const smartPower = (hb & 0x01) ? 'implemented' : 'not implemented';
  return `Master Config: SmartPower=${smartPower}, MemberID=${lb} (raw=${fmtHex(dataU16, 4)})`;
}

function decodeId3SlaveConfig(dataU16) {
  const hb = (dataU16 >> 8) & 0xFF;
  const lb = dataU16 & 0xFF;
  const b = (bit) => (hb >> bit) & 1;
  const dhwPresent = b(0) ? 'yes' : 'no';
  const controlType = b(1) ? 'on/off' : 'modulating';
  const cooling = b(2) ? 'supported' : 'not supported';
  const dhwCfg = b(3) ? 'storage tank' : 'instantaneous/not specified';
  const lowoff = b(4) ? 'not allowed' : 'allowed';
  const ch2 = b(5) ? 'present' : 'not present';
  const waterfill = b(6) ? 'not available' : 'available/unknown';
  const heatcoolSwitch = b(7) ? 'by slave' : 'by master';
  return `Slave Config: DHWpresent=${dhwPresent}, ControlType=${controlType}, Cooling=${cooling}, DHWcfg=${dhwCfg}, LowOff&PumpCtrl=${lowoff}, CH2=${ch2}, RemoteWaterFill=${waterfill}, HeatCoolSwitch=${heatcoolSwitch}, MemberID=${lb} (raw=${fmtHex(dataU16, 4)})`;
}

function decodeF8_8Temp(name, dataU16) {
  const v = f8_8ToFloat(dataU16);
  return `${name} = ${v.toFixed(2)} °C (raw=${fmtHex(dataU16, 4)})`;
}

function decodeF8_8Percent(name, dataU16) {
  const v = f8_8ToFloat(dataU16);
  return `${name} = ${v.toFixed(2)} % (raw=${fmtHex(dataU16, 4)})`;
}

function decodeU16(name, dataU16, unit = '') {
  const unitStr = unit ? ` ${unit}` : '';
  return `${name} = ${dataU16}${unitStr} (raw=${fmtHex(dataU16, 4)})`;
}

function decodeS16(name, dataU16, unit = '') {
  const v = u16ToS16(dataU16);
  const unitStr = unit ? ` ${unit}` : '';
  return `${name} = ${v}${unitStr} (raw=${fmtHex(dataU16, 4)})`;
}

function decodeId4RemoteRequest(dataU16) {
  const hb = (dataU16 >> 8) & 0xFF;
  const lb = dataU16 & 0xFF;
  const requests = [
    'normal_mode', 'boiler_lockout_reset', 'CH_water_filling', 'service_max_power',
    'service_min_power', 'spark_test', 'fan_max_speed', 'fan_min_speed',
    '3way_valve_to_CH', '3way_valve_to_DHW', 'reset_service_request', 'service_test_1',
    'automatic_hydronic_air_purge'
  ];
  const reqName = requests[hb] || `reserved(${hb})`;
  const respStatus = lb >= 128 ? 'ACCEPTED' : 'REFUSED';
  return `Remote Request: ${reqName}, Response=${respStatus} (code=${lb}) (raw=${fmtHex(dataU16, 4)})`;
}

function decodeId5FaultInfo(dataU16) {
  const hb = (dataU16 >> 8) & 0xFF;
  const lb = dataU16 & 0xFF;
  const flags = [];
  if (hb & 0x01) flags.push('Service_request');
  if (hb & 0x02) flags.push('Lockout_reset_enabled');
  if (hb & 0x04) flags.push('Low_water_pressure_fault');
  if (hb & 0x08) flags.push('Gas/Flame_fault');
  if (hb & 0x10) flags.push('Air_pressure_fault');
  if (hb & 0x20) flags.push('Water_overtemp_fault');
  const asfDesc = flags.length > 0 ? flags.join(', ') : 'none';
  return `Fault Info: ASF=[${asfDesc}], OEM_fault_code=${lb} (raw=${fmtHex(dataU16, 4)})`;
}

function decodeId6RemoteBoilerParams(dataU16) {
  const hb = (dataU16 >> 8) & 0xFF;
  const lb = dataU16 & 0xFF;
  const params = [];
  if (hb & 0x01) params.push('DHW_setpoint');
  if (hb & 0x02) params.push('Max_CH_setpoint');
  const rw = [];
  if (lb & 0x01) rw.push('DHW_setpoint=RW');
  else if (hb & 0x01) rw.push('DHW_setpoint=RO');
  if (lb & 0x02) rw.push('Max_CH_setpoint=RW');
  else if (hb & 0x02) rw.push('Max_CH_setpoint=RO');
  return `Remote Boiler Params: enabled=[${params.join(', ')}], access=[${rw.join(', ')}] (raw=${fmtHex(dataU16, 4)})`;
}

function decodeId10TSPCount(dataU16) {
  const hb = (dataU16 >> 8) & 0xFF;
  return `TSP Count: ${hb} transparent slave parameters (raw=${fmtHex(dataU16, 4)})`;
}

function decodeId11TSP(dataU16) {
  const hb = (dataU16 >> 8) & 0xFF;
  const lb = dataU16 & 0xFF;
  return `TSP[${hb}] = ${lb} (raw=${fmtHex(dataU16, 4)})`;
}

function decodeId12FHBSize(dataU16) {
  const hb = (dataU16 >> 8) & 0xFF;
  return `Fault History Buffer Size: ${hb} entries (raw=${fmtHex(dataU16, 4)})`;
}

function decodeId13FHBEntry(dataU16) {
  const hb = (dataU16 >> 8) & 0xFF;
  const lb = dataU16 & 0xFF;
  return `Fault History[${hb}] = ${lb} (raw=${fmtHex(dataU16, 4)})`;
}

function decodeId15MaxCapacity(dataU16) {
  const hb = (dataU16 >> 8) & 0xFF;
  const lb = dataU16 & 0xFF;
  return `Max boiler capacity: ${hb} kW, Min modulation: ${lb} % (raw=${fmtHex(dataU16, 4)})`;
}

function decodeId20DayTime(dataU16) {
  const hb = (dataU16 >> 8) & 0xFF;
  const lb = dataU16 & 0xFF;
  const day = (hb >> 5) & 0x07;
  const hours = hb & 0x1F;
  const minutes = lb;
  const days = ['?', 'Mon', 'Tue', 'Wed', 'Thu', 'Fri', 'Sat', 'Sun'];
  const dayName = days[day] || '?';
  return `Day/Time: ${dayName} ${hours.toString().padStart(2, '0')}:${minutes.toString().padStart(2, '0')} (raw=${fmtHex(dataU16, 4)})`;
}

function decodeId21Date(dataU16) {
  const hb = (dataU16 >> 8) & 0xFF;
  const lb = dataU16 & 0xFF;
  return `Date: month=${hb}, day=${lb} (raw=${fmtHex(dataU16, 4)})`;
}

function decodeId35FanSpeed(dataU16) {
  const hb = (dataU16 >> 8) & 0xFF;
  const lb = dataU16 & 0xFF;
  return `Fan Speed: setpoint=${hb} Hz, actual=${lb} Hz (raw=${fmtHex(dataU16, 4)})`;
}

function decodeId48DHWBounds(dataU16) {
  const hb = (dataU16 >> 8) & 0xFF;
  const lb = dataU16 & 0xFF;
  const upper = hb > 127 ? hb - 256 : hb;
  const lower = lb > 127 ? lb - 256 : lb;
  return `DHW Setpoint Bounds: ${lower}°C to ${upper}°C (raw=${fmtHex(dataU16, 4)})`;
}

function decodeId49CHBounds(dataU16) {
  const hb = (dataU16 >> 8) & 0xFF;
  const lb = dataU16 & 0xFF;
  const upper = hb > 127 ? hb - 256 : hb;
  const lower = lb > 127 ? lb - 256 : lb;
  return `Max CH Setpoint Bounds: ${lower}°C to ${upper}°C (raw=${fmtHex(dataU16, 4)})`;
}

function decodeId70VentStatus(dataU16) {
  const hb = (dataU16 >> 8) & 0xFF;
  const lb = dataU16 & 0xFF;
  const masterFlags = [];
  if (hb & 0x01) masterFlags.push('Vent_enable');
  if (hb & 0x02) masterFlags.push('Bypass_open');
  if (hb & 0x04) masterFlags.push('Bypass_auto');
  if (hb & 0x08) masterFlags.push('Free_vent_mode');
  const slaveFlags = [];
  if (lb & 0x01) slaveFlags.push('Fault');
  if (lb & 0x02) slaveFlags.push('Vent_active');
  if (lb & 0x04) slaveFlags.push('Bypass_open');
  if (lb & 0x08) slaveFlags.push('Bypass_auto');
  if (lb & 0x10) slaveFlags.push('Free_vent_active');
  if (lb & 0x40) slaveFlags.push('Diagnostic');
  return `Vent Status: Master=[${masterFlags.join(', ')}], Slave=[${slaveFlags.join(', ')}] (raw=${fmtHex(dataU16, 4)})`;
}

function decodeId71VentControl(dataU16) {
  const lb = dataU16 & 0xFF;
  return `Vent Control Setpoint: ${lb} % (raw=${fmtHex(dataU16, 4)})`;
}

function decodeId72VentASF(dataU16) {
  const hb = (dataU16 >> 8) & 0xFF;
  const lb = dataU16 & 0xFF;
  const flags = [];
  if (hb & 0x01) flags.push('Service_request');
  if (hb & 0x02) flags.push('Exhaust_fan_fault');
  if (hb & 0x04) flags.push('Inlet_fan_fault');
  if (hb & 0x08) flags.push('Frost_protection');
  const asfDesc = flags.length > 0 ? flags.join(', ') : 'none';
  return `Vent ASF: [${asfDesc}], OEM_code=${lb} (raw=${fmtHex(dataU16, 4)})`;
}

function decodeId74VentConfig(dataU16) {
  const hb = (dataU16 >> 8) & 0xFF;
  const lb = dataU16 & 0xFF;
  const systemType = (hb & 0x01) ? 'heat-recovery' : 'central_exhaust';
  const bypassPresent = (hb & 0x02) ? 'yes' : 'no';
  const speedControl = (hb & 0x04) ? 'variable' : '3-speed';
  return `Vent Config: type=${systemType}, bypass=${bypassPresent}, speed_control=${speedControl}, MemberID=${lb} (raw=${fmtHex(dataU16, 4)})`;
}

function decodeId77RelativeVent(dataU16) {
  const lb = dataU16 & 0xFF;
  return `Relative Ventilation: ${lb} % (raw=${fmtHex(dataU16, 4)})`;
}

function decodeId78RHExhaust(dataU16) {
  const lb = dataU16 & 0xFF;
  return `RH Exhaust: ${lb} % (raw=${fmtHex(dataU16, 4)})`;
}

function decodeId86VentRemoteParams(dataU16) {
  const hb = (dataU16 >> 8) & 0xFF;
  const lb = dataU16 & 0xFF;
  const enabled = (hb & 0x01) ? 'yes' : 'no';
  const rw = (lb & 0x01) ? 'RW' : 'RO';
  return `Vent Remote Params: Nominal_vent_value enabled=${enabled}, access=${rw} (raw=${fmtHex(dataU16, 4)})`;
}

function decodeId87NominalVent(dataU16) {
  const hb = (dataU16 >> 8) & 0xFF;
  return `Nominal Ventilation Value: ${hb} % (raw=${fmtHex(dataU16, 4)})`;
}

function decodeId93BrandChar(dataU16) {
  const hb = (dataU16 >> 8) & 0xFF;
  const lb = dataU16 & 0xFF;
  const char = lb >= 32 && lb < 127 ? String.fromCharCode(lb) : `[0x${lb.toString(16)}]`;
  return `Brand String[${hb}] = '${char}' (raw=${fmtHex(dataU16, 4)})`;
}

function decodeId94VersionChar(dataU16) {
  const hb = (dataU16 >> 8) & 0xFF;
  const lb = dataU16 & 0xFF;
  const char = lb >= 32 && lb < 127 ? String.fromCharCode(lb) : `[0x${lb.toString(16)}]`;
  return `Version String[${hb}] = '${char}' (raw=${fmtHex(dataU16, 4)})`;
}

function decodeId95SerialChar(dataU16) {
  const hb = (dataU16 >> 8) & 0xFF;
  const lb = dataU16 & 0xFF;
  const char = lb >= 32 && lb < 127 ? String.fromCharCode(lb) : `[0x${lb.toString(16)}]`;
  return `Serial String[${hb}] = '${char}' (raw=${fmtHex(dataU16, 4)})`;
}

function decodeId98RFSensorStatus(dataU16) {
  const hb = (dataU16 >> 8) & 0xFF;
  const lb = dataU16 & 0xFF;
  const sensorIndex = hb & 0x0F;
  const sensorType = (hb >> 4) & 0x0F;
  const typeNames = ['room_temp_ctrl', 'room_temp_sensor', 'outside_temp_sensor'];
  const typeName = sensorType === 15 ? 'not-defined' : (typeNames[sensorType] || `reserved(${sensorType})`);
  const battery = ['no_indication', 'low', 'nearly_low', 'ok'][lb & 0x03];
  const rfStrength = (lb >> 2) & 0x07;
  return `RF Sensor[${sensorIndex}]: type=${typeName}, battery=${battery}, RF_strength=${rfStrength} (raw=${fmtHex(dataU16, 4)})`;
}

function decodeId99OperatingMode(dataU16) {
  const hb = (dataU16 >> 8) & 0xFF;
  const lb = dataU16 & 0xFF;
  const modes = ['no_override', 'auto', 'comfort', 'precomfort', 'reduced', 'protection', 'off'];
  const hc1Mode = modes[lb & 0x0F] || `reserved(${lb & 0x0F})`;
  const hc2Mode = modes[(lb >> 4) & 0x0F] || `reserved(${(lb >> 4) & 0x0F})`;
  const dhwMode = modes[hb & 0x0F] || `reserved(${hb & 0x0F})`;
  const manualDHW = (hb & 0x10) ? 'yes' : 'no';
  return `Operating Mode: HC1=${hc1Mode}, HC2=${hc2Mode}, DHW=${dhwMode}, Manual_DHW_push=${manualDHW} (raw=${fmtHex(dataU16, 4)})`;
}

function decodeId100RemoteOverrideFunc(dataU16) {
  const lb = dataU16 & 0xFF;
  const manualPriority = (lb & 0x01) ? 'yes' : 'no';
  const programPriority = (lb & 0x02) ? 'yes' : 'no';
  return `Remote Override Function: manual_change_priority=${manualPriority}, program_change_priority=${programPriority} (raw=${fmtHex(dataU16, 4)})`;
}

function decodeId101SolarStatus(dataU16) {
  const hb = (dataU16 >> 8) & 0xFF;
  const lb = dataU16 & 0xFF;
  const masterMode = hb & 0x07;
  const slaveMode = (lb >> 1) & 0x07;
  const modeNames = ['off', 'DHW_eco', 'DHW_comfort', 'single_boost', 'continuous_boost'];
  const masterModeName = modeNames[masterMode] || `reserved(${masterMode})`;
  const slaveModeName = modeNames[slaveMode] || `reserved(${slaveMode})`;
  const fault = (lb & 0x01) ? 'yes' : 'no';
  const statusCode = (lb >> 4) & 0x03;
  const statusNames = ['standby', 'loading_by_sun', 'loading_by_boiler', 'anti-legionella'];
  const statusName = statusNames[statusCode];
  return `Solar Status: Master_mode=${masterModeName}, Slave_mode=${slaveModeName}, status=${statusName}, fault=${fault} (raw=${fmtHex(dataU16, 4)})`;
}

function decodeId103SolarConfig(dataU16) {
  const hb = (dataU16 >> 8) & 0xFF;
  const lb = dataU16 & 0xFF;
  const systemType = (lb & 0x01) ? 'DHW_parallel' : 'DHW_preheat';
  return `Solar Config: system_type=${systemType}, MemberID=${lb} (raw=${fmtHex(dataU16, 4)})`;
}

function decodeId104ProductVersion(name, dataU16) {
  const hb = (dataU16 >> 8) & 0xFF;
  const lb = dataU16 & 0xFF;
  return `${name} Product: type=${hb}, version=${lb} (raw=${fmtHex(dataU16, 4)})`;
}

function decodeId126MasterProductVersion(dataU16) {
  return decodeId104ProductVersion('Master', dataU16);
}

function decodeId127SlaveProductVersion(dataU16) {
  return decodeId104ProductVersion('Slave', dataU16);
}

export const ID_DECODERS = {
  0: { name: 'Status', decode: decodeStatusId0 },
  1: { name: 'Control Setpoint (CH)', decode: decodeId1ControlSetpoint },
  2: { name: 'Master Configuration', decode: decodeId2MasterConfig },
  3: { name: 'Slave Configuration', decode: decodeId3SlaveConfig },
  4: { name: 'Remote Request', decode: decodeId4RemoteRequest },
  5: { name: 'Fault Info', decode: decodeId5FaultInfo },
  6: { name: 'Remote Boiler Parameters', decode: decodeId6RemoteBoilerParams },
  7: { name: 'Cooling Control Signal', decode: (u16) => decodeF8_8Percent('Cooling control', u16) },
  8: { name: 'Control Setpoint CH2', decode: (u16) => decodeF8_8Temp('TsetCH2', u16) },
  9: { name: 'Remote Override Room Setpoint', decode: (u16) => decodeF8_8Temp('Remote override setpoint', u16) },
  10: { name: 'TSP Count', decode: decodeId10TSPCount },
  11: { name: 'Transparent Slave Parameter', decode: decodeId11TSP },
  12: { name: 'FHB Size', decode: decodeId12FHBSize },
  13: { name: 'Fault History Entry', decode: decodeId13FHBEntry },
  14: { name: 'Max Relative Modulation', decode: (u16) => decodeF8_8Percent('Max relative modulation', u16) },
  15: { name: 'Max Capacity & Min Modulation', decode: decodeId15MaxCapacity },
  16: { name: 'Room Setpoint', decode: (u16) => decodeF8_8Temp('Room setpoint', u16) },
  17: { name: 'Relative Modulation Level', decode: (u16) => decodeF8_8Percent('Relative modulation', u16) },
  18: { name: 'CH Water Pressure', decode: (u16) => `CH water pressure = ${f8_8ToFloat(u16).toFixed(2)} bar (raw=${fmtHex(u16, 4)})` },
  19: { name: 'DHW Flow Rate', decode: (u16) => `DHW flow rate = ${f8_8ToFloat(u16).toFixed(2)} l/min (raw=${fmtHex(u16, 4)})` },
  20: { name: 'Day of Week & Time', decode: decodeId20DayTime },
  21: { name: 'Date', decode: decodeId21Date },
  22: { name: 'Year', decode: (u16) => decodeU16('Year', u16) },
  23: { name: 'Room Setpoint CH2', decode: (u16) => decodeF8_8Temp('Room setpoint CH2', u16) },
  24: { name: 'Room Temperature', decode: (u16) => decodeF8_8Temp('Room temp', u16) },
  25: { name: 'Boiler Flow Water Temp', decode: (u16) => decodeF8_8Temp('Boiler flow temp', u16) },
  26: { name: 'DHW Temperature', decode: (u16) => decodeF8_8Temp('DHW temp', u16) },
  27: { name: 'Outside Temperature', decode: (u16) => decodeF8_8Temp('Outside temp', u16) },
  28: { name: 'Return Water Temperature', decode: (u16) => decodeF8_8Temp('Return water temp', u16) },
  29: { name: 'Solar Storage Temperature', decode: (u16) => decodeF8_8Temp('Solar storage temp', u16) },
  30: { name: 'Solar Collector Temperature', decode: (u16) => decodeS16('Solar collector temp', u16, '°C') },
  31: { name: 'Flow Temperature CH2', decode: (u16) => decodeF8_8Temp('Flow temp CH2', u16) },
  32: { name: 'DHW2 Temperature', decode: (u16) => decodeF8_8Temp('DHW2 temp', u16) },
  33: { name: 'Exhaust Temperature', decode: (u16) => decodeS16('Exhaust temp', u16, '°C') },
  34: { name: 'Boiler Heat Exchanger Temp', decode: (u16) => decodeF8_8Temp('Heat exchanger temp', u16) },
  35: { name: 'Boiler Fan Speed', decode: decodeId35FanSpeed },
  36: { name: 'Flame Current', decode: (u16) => `Flame current = ${f8_8ToFloat(u16).toFixed(2)} µA (raw=${fmtHex(u16, 4)})` },
  37: { name: 'Room Temperature CH2', decode: (u16) => decodeF8_8Temp('Room temp CH2', u16) },
  38: { name: 'Relative Humidity', decode: (u16) => decodeF8_8Percent('Relative humidity', u16) },
  39: { name: 'Remote Override Setpoint 2', decode: (u16) => decodeF8_8Temp('Remote override setpoint 2', u16) },
  48: { name: 'DHW Setpoint Bounds', decode: decodeId48DHWBounds },
  49: { name: 'Max CH Setpoint Bounds', decode: decodeId49CHBounds },
  56: { name: 'DHW Setpoint', decode: (u16) => decodeF8_8Temp('DHW setpoint', u16) },
  57: { name: 'Max CH Water Setpoint', decode: (u16) => decodeF8_8Temp('Max CH setpoint', u16) },
  // Ventilation / Heat Recovery (70-91)
  70: { name: 'Ventilation Status', decode: decodeId70VentStatus },
  71: { name: 'Ventilation Control Setpoint', decode: decodeId71VentControl },
  72: { name: 'Ventilation ASF & OEM Code', decode: decodeId72VentASF },
  73: { name: 'Ventilation OEM Diagnostic', decode: (u16) => decodeU16('Vent OEM diagnostic code', u16) },
  74: { name: 'Ventilation Configuration', decode: decodeId74VentConfig },
  75: { name: 'Ventilation OpenTherm Version', decode: (u16) => `Vent OT version = ${f8_8ToFloat(u16).toFixed(2)} (raw=${fmtHex(u16, 4)})` },
  76: { name: 'Ventilation Product Version', decode: (u16) => decodeId104ProductVersion('Vent', u16) },
  77: { name: 'Relative Ventilation', decode: decodeId77RelativeVent },
  78: { name: 'RH Exhaust', decode: decodeId78RHExhaust },
  79: { name: 'CO2 Exhaust', decode: (u16) => decodeU16('CO2 exhaust', u16, 'ppm') },
  80: { name: 'Supply Inlet Temperature', decode: (u16) => decodeF8_8Temp('Supply inlet temp', u16) },
  81: { name: 'Supply Outlet Temperature', decode: (u16) => decodeF8_8Temp('Supply outlet temp', u16) },
  82: { name: 'Exhaust Inlet Temperature', decode: (u16) => decodeF8_8Temp('Exhaust inlet temp', u16) },
  83: { name: 'Exhaust Outlet Temperature', decode: (u16) => decodeF8_8Temp('Exhaust outlet temp', u16) },
  84: { name: 'Actual Exhaust Fan Speed', decode: (u16) => decodeU16('Exhaust fan speed', u16, 'rpm') },
  85: { name: 'Actual Inlet Fan Speed', decode: (u16) => decodeU16('Inlet fan speed', u16, 'rpm') },
  86: { name: 'Ventilation Remote Parameters', decode: decodeId86VentRemoteParams },
  87: { name: 'Nominal Ventilation Value', decode: decodeId87NominalVent },
  88: { name: 'Ventilation TSP Count', decode: (u16) => { const hb = (u16 >> 8) & 0xFF; return `Vent TSP count: ${hb} (raw=${fmtHex(u16, 4)})`; } },
  89: { name: 'Ventilation TSP', decode: decodeId11TSP },
  90: { name: 'Ventilation FHB Size', decode: (u16) => { const hb = (u16 >> 8) & 0xFF; return `Vent FHB size: ${hb} (raw=${fmtHex(u16, 4)})`; } },
  91: { name: 'Ventilation FHB Entry', decode: decodeId13FHBEntry },
  // Brand strings
  93: { name: 'Brand String Character', decode: decodeId93BrandChar },
  94: { name: 'Brand Version String Character', decode: decodeId94VersionChar },
  95: { name: 'Brand Serial String Character', decode: decodeId95SerialChar },
  // Counters / Stats
  96: { name: 'Cooling Operation Hours', decode: (u16) => decodeU16('Cooling hours', u16) },
  97: { name: 'Power Cycles', decode: (u16) => decodeU16('Power cycles', u16) },
  98: { name: 'RF Sensor Status', decode: decodeId98RFSensorStatus },
  99: { name: 'Operating Mode Override', decode: decodeId99OperatingMode },
  100: { name: 'Remote Override Function', decode: decodeId100RemoteOverrideFunc },
  // Solar Storage
  101: { name: 'Solar Storage Status', decode: decodeId101SolarStatus },
  102: { name: 'Solar ASF & OEM Code', decode: (u16) => { const lb = u16 & 0xFF; return `Solar OEM code: ${lb} (raw=${fmtHex(u16, 4)})`; } },
  103: { name: 'Solar Configuration', decode: decodeId103SolarConfig },
  104: { name: 'Solar Product Version', decode: (u16) => decodeId104ProductVersion('Solar', u16) },
  105: { name: 'Solar TSP Count', decode: (u16) => { const hb = (u16 >> 8) & 0xFF; return `Solar TSP count: ${hb} (raw=${fmtHex(u16, 4)})`; } },
  106: { name: 'Solar TSP', decode: decodeId11TSP },
  107: { name: 'Solar FHB Size', decode: (u16) => { const hb = (u16 >> 8) & 0xFF; return `Solar FHB size: ${hb} (raw=${fmtHex(u16, 4)})`; } },
  108: { name: 'Solar FHB Entry', decode: decodeId13FHBEntry },
  // Electricity Production
  109: { name: 'Electricity Producer Starts', decode: (u16) => decodeU16('Electricity producer starts', u16) },
  110: { name: 'Electricity Producer Hours', decode: (u16) => decodeU16('Electricity producer hours', u16) },
  111: { name: 'Current Electricity Production', decode: (u16) => decodeU16('Current electricity production', u16, 'W') },
  112: { name: 'Cumulative Electricity Production', decode: (u16) => decodeU16('Cumulative electricity production', u16, 'kWh') },
  // Burner/Pump Counters
  113: { name: 'Unsuccessful Burner Starts', decode: (u16) => decodeU16('Unsuccessful burner starts', u16) },
  114: { name: 'Flame Signal Too Low Count', decode: (u16) => decodeU16('Flame signal too low count', u16) },
  115: { name: 'OEM Diagnostic Code', decode: (u16) => decodeU16('OEM diagnostic code', u16) },
  116: { name: 'Successful Burner Starts', decode: (u16) => decodeU16('Successful burner starts', u16) },
  117: { name: 'CH Pump Starts', decode: (u16) => decodeU16('CH pump starts', u16) },
  118: { name: 'DHW Pump/Valve Starts', decode: (u16) => decodeU16('DHW pump/valve starts', u16) },
  119: { name: 'DHW Burner Starts', decode: (u16) => decodeU16('DHW burner starts', u16) },
  120: { name: 'Burner Operation Hours', decode: (u16) => decodeU16('Burner hours', u16) },
  121: { name: 'CH Pump Operation Hours', decode: (u16) => decodeU16('CH pump hours', u16) },
  122: { name: 'DHW Pump/Valve Operation Hours', decode: (u16) => decodeU16('DHW pump/valve hours', u16) },
  123: { name: 'DHW Burner Operation Hours', decode: (u16) => decodeU16('DHW burner hours', u16) },
  // Protocol/Product Identity
  124: { name: 'OpenTherm Version Master', decode: (u16) => `Master OT version = ${f8_8ToFloat(u16).toFixed(2)} (raw=${fmtHex(u16, 4)})` },
  125: { name: 'OpenTherm Version Slave', decode: (u16) => `Slave OT version = ${f8_8ToFloat(u16).toFixed(2)} (raw=${fmtHex(u16, 4)})` },
  126: { name: 'Master Product Version', decode: decodeId126MasterProductVersion },
  127: { name: 'Slave Product Version', decode: decodeId127SlaveProductVersion }
};

export function decodeFrame(message, direction) {
  const frameBits = message.toString(2).padStart(32, '0');
  const isMasterToSlave = direction === 'REQUEST';

  /* OpenTherm frame layout:
     [31] ?, [30:28] MSG_TYPE, [27:24] ?, [23:16] DATA_ID, [15:0] DATA_VALUE */
  const msgType = parseInt(frameBits.substr(3, 3), 2);
  const dataId = parseInt(frameBits.substr(8, 8), 2);
  const dataVal = parseInt(frameBits.substr(16, 16), 2);

  const msgName = (isMasterToSlave ? MASTER_MSG_TYPES : SLAVE_MSG_TYPES)[msgType] || 'UNKNOWN';
  const parOk = parityOk(frameBits);

  /* Decode payload */
  const decoder = ID_DECODERS[dataId];
  let payload, idName;
  if (decoder) {
    payload = decoder.decode(dataVal);
    idName = decoder.name;
  } else {
    const hb = (dataVal >> 8) & 0xFF;
    const lb = dataVal & 0xFF;
    payload = `DATA-VALUE=${fmtHex(dataVal, 4)} (HB=${fmtHex(hb, 2)}, LB=${fmtHex(lb, 2)})`;
    idName = 'Unknown/Unimplemented';
  }

  const warnings = [];
  if (!parOk) {
    warnings.push('PARITY_ERROR');
  }

  const warnTxt = warnings.length > 0 ? ` [${warnings.join(' | ')}]` : '';
  return `${msgName} (msg=${msgType}, id=${dataId} ${idName}) ${payload}${warnTxt}`;
}
