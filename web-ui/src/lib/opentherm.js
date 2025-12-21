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

export const ID_DECODERS = {
  0: { name: 'Status', decode: decodeStatusId0 },
  1: { name: 'Control Setpoint', decode: decodeId1ControlSetpoint },
  2: { name: 'Master Configuration', decode: decodeId2MasterConfig },
  3: { name: 'Slave Configuration', decode: decodeId3SlaveConfig },
  17: {
    name: 'Relative modulation level',
    decode: (u16) => `Relative modulation = ${f8_8ToFloat(u16).toFixed(2)} % (raw=${fmtHex(u16, 4)})`
  },
  18: {
    name: 'CH water pressure',
    decode: (u16) => `CH water pressure = ${f8_8ToFloat(u16).toFixed(2)} bar (raw=${fmtHex(u16, 4)})`
  },
  19: {
    name: 'DHW flow rate',
    decode: (u16) => `DHW flow rate = ${f8_8ToFloat(u16).toFixed(2)} l/min (raw=${fmtHex(u16, 4)})`
  },
  25: { name: 'Boiler water temp', decode: (u16) => decodeF8_8Temp('Boiler water temp', u16) },
  26: { name: 'DHW temp', decode: (u16) => decodeF8_8Temp('DHW temp', u16) },
  27: { name: 'Outside temp', decode: (u16) => decodeF8_8Temp('Outside temp', u16) },
  28: { name: 'Return water temp', decode: (u16) => decodeF8_8Temp('Return water temp', u16) }
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
