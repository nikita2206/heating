#!/usr/bin/env python3
import sys
import re
from dataclasses import dataclass
from typing import Callable, Optional, Dict, Tuple

LINE_RE = re.compile(r"OT:\s+Incoming frame from\s+([TB])\s*:\s*([01]{32})\s*$")

# OT/+ frame format (32 bits):
# [P:1][MSG_TYPE:3][SPARE:4][DATA_ID:8][DATA_VALUE:16]
# Parity: total number of '1' bits across the full 32 bits must be even. :contentReference[oaicite:1]{index=1}
# MSG-TYPE meaning depends on direction (master->slave vs slave->master). :contentReference[oaicite:2]{index=2}
MASTER_MSG_TYPES = {
    0b000: "READ-DATA",
    0b001: "WRITE-DATA",
    0b010: "INVALID-DATA",
    0b011: "RESERVED",
    0b100: "RESERVED",
    0b101: "RESERVED",
    0b110: "RESERVED",
    0b111: "RESERVED",
}
SLAVE_MSG_TYPES = {
    0b000: "RESERVED",
    0b001: "RESERVED",
    0b010: "RESERVED",
    0b011: "RESERVED",
    0b100: "READ-ACK",
    0b101: "WRITE-ACK",
    0b110: "DATA-INVALID",
    0b111: "UNKNOWN-DATAID",
}

def bits_to_u8(b: str) -> int:
    return int(b, 2)

def bits_to_u16(b: str) -> int:
    return int(b, 2)

def u16_to_s16(v: int) -> int:
    return v - 0x10000 if v & 0x8000 else v

def f8_8_to_float(u16: int) -> float:
    return u16_to_s16(u16) / 256.0

def parity_ok(frame_bits: str) -> bool:
    return (frame_bits.count("1") % 2) == 0

def fmt_hex(v: int, width: int) -> str:
    return f"0x{v:0{width}X}"

def decode_flag8(byte_val: int, bit_names: Dict[int, str]) -> str:
    parts = []
    for bit in range(7, -1, -1):
        name = bit_names.get(bit, f"bit{bit}")
        state = "1" if (byte_val >> bit) & 1 else "0"
        parts.append(f"{name}={state}")
    return ", ".join(parts)

def decode_status_id0(data_u16: int) -> str:
    hb = (data_u16 >> 8) & 0xFF  # Master status (HB)
    lb = data_u16 & 0xFF         # Slave status (LB)

    master_bits = {
        0: "CH",
        1: "DHW",
        2: "Cooling",
        3: "OTC",
        4: "CH2",
        5: "SummerMode",   # 1 = summer, 0 = winter
        6: "DHWBlocking",
        7: "reserved",
    }
    slave_bits = {
        0: "Fault",
        1: "CHMode",
        2: "DHWMode",
        3: "Flame",
        4: "CoolingMode",
        5: "CH2Mode",
        6: "Diagnostic",
        7: "ElectricityProd",
    }

    def pretty_master(bit: int, val: int) -> str:
        on = (val >> bit) & 1
        if bit == 0: return f"CH={'enabled' if on else 'disabled'}"
        if bit == 1: return f"DHW={'enabled' if on else 'disabled'}"
        if bit == 2: return f"Cooling={'enabled' if on else 'disabled'}"
        if bit == 3: return f"OTC={'active' if on else 'inactive'}"
        if bit == 4: return f"CH2={'enabled' if on else 'disabled'}"
        if bit == 5: return f"Mode={'summer' if on else 'winter'}"
        if bit == 6: return f"DHWBlocking={'blocked' if on else 'unblocked'}"
        if bit == 7: return "reserved"
        return f"{master_bits.get(bit, f'bit{bit}')}={on}"

    def pretty_slave(bit: int, val: int) -> str:
        on = (val >> bit) & 1
        if bit == 0: return f"Fault={'yes' if on else 'no'}"
        if bit == 1: return f"CHMode={'active' if on else 'inactive'}"
        if bit == 2: return f"DHWMode={'active' if on else 'inactive'}"
        if bit == 3: return f"Flame={'on' if on else 'off'}"
        if bit == 4: return f"CoolingMode={'active' if on else 'inactive'}"
        if bit == 5: return f"CH2Mode={'active' if on else 'inactive'}"
        if bit == 6: return f"Diagnostic={'event' if on else 'none'}"
        if bit == 7: return f"ElectricityProduction={'on' if on else 'off'}"
        return f"{slave_bits.get(bit, f'bit{bit}')}={on}"

    master_desc = "; ".join(pretty_master(b, hb) for b in range(0, 8))
    slave_desc  = "; ".join(pretty_slave(b, lb) for b in range(0, 8))
    return f"MasterStatus(HB): {master_desc} | SlaveStatus(LB): {slave_desc}"

def decode_id1_control_setpoint(data_u16: int) -> str:
    t = f8_8_to_float(data_u16)
    return f"Control Setpoint (Tset) = {t:.2f} °C (raw={fmt_hex(data_u16, 4)})"

def decode_id2_master_config(data_u16: int) -> str:
    hb = (data_u16 >> 8) & 0xFF
    lb = data_u16 & 0xFF
    smart_power = "implemented" if (hb & 0x01) else "not implemented"
    return f"Master Config: SmartPower={smart_power}, MemberID={lb} (raw={fmt_hex(data_u16,4)})"

def decode_id3_slave_config(data_u16: int) -> str:
    hb = (data_u16 >> 8) & 0xFF
    lb = data_u16 & 0xFF
    # Bits per spec for ID3 HB. :contentReference[oaicite:3]{index=3}
    def b(bit: int) -> int: return (hb >> bit) & 1
    dhw_present = "yes" if b(0) else "no"
    control_type = "on/off" if b(1) else "modulating"
    cooling = "supported" if b(2) else "not supported"
    dhw_cfg = "storage tank" if b(3) else "instantaneous/not specified"
    lowoff = "not allowed" if b(4) else "allowed"
    ch2 = "present" if b(5) else "not present"
    waterfill = "not available" if b(6) else "available/unknown"
    heatcool_switch = "by slave" if b(7) else "by master"
    return (
        f"Slave Config: DHWpresent={dhw_present}, ControlType={control_type}, Cooling={cooling}, "
        f"DHWcfg={dhw_cfg}, LowOff&PumpCtrl={lowoff}, CH2={ch2}, RemoteWaterFill={waterfill}, "
        f"HeatCoolSwitch={heatcool_switch}, MemberID={lb} (raw={fmt_hex(data_u16,4)})"
    )

def decode_f8_8_temp(name: str, data_u16: int) -> str:
    v = f8_8_to_float(data_u16)
    return f"{name} = {v:.2f} °C (raw={fmt_hex(data_u16,4)})"

@dataclass
class IdDecoder:
    name: str
    decode: Callable[[int], str]

ID_DECODERS: Dict[int, IdDecoder] = {
    0: IdDecoder("Status", decode_status_id0),
    1: IdDecoder("Control Setpoint", decode_id1_control_setpoint),
    2: IdDecoder("Master Configuration", decode_id2_master_config),
    3: IdDecoder("Slave Configuration", decode_id3_slave_config),

    # Common informative temps (all f8.8) :contentReference[oaicite:4]{index=4}
    25: IdDecoder("Boiler water temp", lambda u16: decode_f8_8_temp("Boiler water temp", u16)),
    26: IdDecoder("DHW temp", lambda u16: decode_f8_8_temp("DHW temp", u16)),
    27: IdDecoder("Outside temp", lambda u16: decode_f8_8_temp("Outside temp", u16)),
    28: IdDecoder("Return water temp", lambda u16: decode_f8_8_temp("Return water temp", u16)),
    17: IdDecoder("Relative modulation level", lambda u16: f"Relative modulation = {f8_8_to_float(u16):.2f} % (raw={fmt_hex(u16,4)})"),
    18: IdDecoder("CH water pressure", lambda u16: f"CH water pressure = {f8_8_to_float(u16):.2f} bar (raw={fmt_hex(u16,4)})"),
    19: IdDecoder("DHW flow rate", lambda u16: f"DHW flow rate = {f8_8_to_float(u16):.2f} l/min (raw={fmt_hex(u16,4)})"),
}

def decode_frame(src: str, frame_bits: str) -> str:
    # src: 'T' or 'B'
    # Assumption: T is master (thermostat/room unit), B is slave (boiler). :contentReference[oaicite:5]{index=5}
    direction = "T→B" if src == "T" else "B→T"
    is_master_to_slave = (src == "T")

    p_bit = bits_to_u8(frame_bits[0:1])
    msg_type = bits_to_u8(frame_bits[1:4])
    spare = bits_to_u8(frame_bits[4:8])
    data_id = bits_to_u8(frame_bits[8:16])
    data_val = bits_to_u16(frame_bits[16:32])

    msg_name = (MASTER_MSG_TYPES if is_master_to_slave else SLAVE_MSG_TYPES).get(msg_type, "UNKNOWN")
    par_ok = parity_ok(frame_bits)

    # Decode payload
    decoder = ID_DECODERS.get(data_id)
    if decoder:
        payload = decoder.decode(data_val)
        id_name = decoder.name
    else:
        hb = (data_val >> 8) & 0xFF
        lb = data_val & 0xFF
        payload = f"DATA-VALUE={fmt_hex(data_val,4)} (HB={fmt_hex(hb,2)}, LB={fmt_hex(lb,2)})"
        id_name = "Unknown/Unimplemented"

    warnings = []
    if spare != 0:
        warnings.append(f"SPARE!=0 ({fmt_hex(spare,1)})")
    if not par_ok:
        warnings.append("PARITY_ERROR")

    warn_txt = f" [{' | '.join(warnings)}]" if warnings else ""
    return (
        f"{direction} {msg_name} (msg=0b{msg_type:03b}, id={data_id} {id_name}) "
        f"{payload}{warn_txt}"
    )

def process_line(line: str) -> None:
    """Process a single line and print decoded frame if it matches."""
    m = LINE_RE.search(line.rstrip("\n"))
    if not m:
        return
    src, bits = m.group(1), m.group(2)
    try:
        print(decode_frame(src, bits))
    except Exception as e:
        print(f"ERROR decoding frame from {src}: {bits} ({e})", file=sys.stderr)

def main() -> int:
    if sys.stdin.isatty():
        # Interactive mode
        print("OpenTherm Frame Decoder - Interactive Mode")
        print("Paste lines containing OT frames (format: 'OT: Incoming frame from [TB]: [32-bit binary]')")
        print("Type 'quit', 'exit', or press Ctrl+D to exit")
        print("-" * 60)

        try:
            while True:
                try:
                    line = input("> ").strip()
                    if line.lower() in ('quit', 'exit'):
                        break
                    if line:
                        process_line(line)
                except EOFError:
                    break
        except KeyboardInterrupt:
            print("\nExiting...")
    else:
        # Streaming mode (stdin is piped)
        for line in sys.stdin:
            process_line(line)

    return 0

if __name__ == "__main__":
    raise SystemExit(main())
