#!/usr/bin/env python3
import asyncio
import json
import sys
from dataclasses import dataclass
from datetime import datetime

# --------- WebSocket URL handling ---------

DEFAULT_WS_URL = "ws://localhost/ws"  # change if needed


# --------- OpenTherm primitives ---------

MSG_TYPES = {
    0: "READ_DATA",
    1: "WRITE_DATA",
    2: "INVALID_DATA",
    3: "RESERVED",
    4: "READ_ACK",
    5: "WRITE_ACK",
    6: "DATA_INVALID",
    7: "UNKNOWN_DATA_ID",
}

@dataclass
class OTFrame:
    raw: int
    parity: int
    msg_type: int
    data_id: int
    data: int


def decode_frame(raw: int) -> OTFrame:
    parity = (raw >> 31) & 0x1
    msg_type = (raw >> 28) & 0x7
    data_id = (raw >> 16) & 0xFF
    data = raw & 0xFFFF
    return OTFrame(raw=raw, parity=parity, msg_type=msg_type,
                   data_id=data_id, data=data)


def f8_8(value: int) -> float:
    return value / 256.0


def s16(value: int) -> int:
    if value & 0x8000:
        return value - 0x10000
    return value


def s8(value: int) -> int:
    if value & 0x80:
        return value - 0x100
    return value


# --------- ID-specific decoders (same idea as previous script) ---------

def decode_status(data: int) -> str:
    master = (data >> 8) & 0xFF
    slave = data & 0xFF

    m_ch_enable  = bool(master & (1 << 0))
    m_dhw_enable = bool(master & (1 << 1))
    m_cooling    = bool(master & (1 << 2))
    m_otc        = bool(master & (1 << 3))
    m_ch2_enable = bool(master & (1 << 4))

    s_fault      = bool(slave & (1 << 0))
    s_ch_mode    = bool(slave & (1 << 1))
    s_dhw_mode   = bool(slave & (1 << 2))
    s_flame      = bool(slave & (1 << 3))
    s_cooling    = bool(slave & (1 << 4))
    s_ch2_mode   = bool(slave & (1 << 5))
    s_diag       = bool(slave & (1 << 6))

    parts = []
    parts.append(
        "master: CH_enable={}, DHW_enable={}, cooling={}, OTC_active={}, CH2_enable={}"
        .format(int(m_ch_enable), int(m_dhw_enable), int(m_cooling),
                int(m_otc), int(m_ch2_enable))
    )
    parts.append(
        "slave: fault={}, CH_mode={}, DHW_mode={}, flame={}, cooling={}, CH2={}, diag={}"
        .format(int(s_fault), int(s_ch_mode), int(s_dhw_mode),
                int(s_flame), int(s_cooling), int(s_ch2_mode), int(s_diag))
    )
    return " | ".join(parts)


def decode_master_config(data: int) -> str:
    hb = (data >> 8) & 0xFF
    member_id = data & 0xFF
    return f"Master configuration flags=0x{hb:02X} (reserved), Master MemberID={member_id}"


def decode_slave_config(data: int) -> str:
    hb = (data >> 8) & 0xFF
    member_id = data & 0xFF

    dhw_present   = bool(hb & (1 << 0))
    control_type  = bool(hb & (1 << 1))  # 0=modulating, 1=on/off
    cooling_sup   = bool(hb & (1 << 2))
    dhw_storage   = bool(hb & (1 << 3))
    lowoff_pump   = bool(hb & (1 << 4))
    ch2_present   = bool(hb & (1 << 5))

    return (
        "Slave configuration: DHW_present={}, control_type={}, cooling_supported={}, "
        "DHW_storage_tank={}, master_lowoff_pump_allowed={}, CH2_present={}, "
        "Slave MemberID={}"
    ).format(
        int(dhw_present),
        "on/off" if control_type else "modulating",
        int(cooling_sup),
        int(dhw_storage),
        int(lowoff_pump),
        int(ch2_present),
        member_id,
    )


def decode_app_faults(data: int) -> str:
    hb = (data >> 8) & 0xFF
    oem_code = data & 0xFF

    service_req   = bool(hb & (1 << 0))
    lockout_reset = bool(hb & (1 << 1))
    low_water     = bool(hb & (1 << 2))
    gas_flame     = bool(hb & (1 << 3))
    air_press     = bool(hb & (1 << 4))
    water_overtemp= bool(hb & (1 << 5))

    return (
        "App fault flags: service_req={}, lockout_reset={}, low_water_pressure={}, "
        "gas/flame_fault={}, air_pressure_fault={}, water_overtemp_fault={}, "
        "OEM_fault_code={}"
    ).format(
        int(service_req), int(lockout_reset), int(low_water),
        int(gas_flame), int(air_press), int(water_overtemp), oem_code
    )


def decode_remote_param_flags(data: int) -> str:
    hb = (data >> 8) & 0xFF
    lb = data & 0xFF
    dhw_transfer   = bool(hb & (1 << 0))
    maxch_transfer = bool(hb & (1 << 1))
    dhw_rw         = bool(lb & (1 << 0))
    maxch_rw       = bool(lb & (1 << 1))
    return (
        "Remote parameter flags: DHW_transfer_en={}, MaxCH_transfer_en={}, "
        "DHW_setpoint_rw={}, MaxCH_setpoint_rw={}"
    ).format(
        int(dhw_transfer), int(maxch_transfer),
        int(dhw_rw), int(maxch_rw)
    )


def decode_command(data: int) -> str:
    cmd = (data >> 8) & 0xFF
    resp = data & 0xFF
    status = "command completed" if resp >= 128 else "command failed"
    return f"Remote command: code={cmd}, response={resp} ({status})"


def decode_datetime(data: int) -> str:
    hb = (data >> 8) & 0xFF
    lb = data & 0xFF
    day = (hb >> 5) & 0x7
    hour = hb & 0x1F
    minute = lb
    dow = {
        0: "no-info",
        1: "Mon",
        2: "Tue",
        3: "Wed",
        4: "Thu",
        5: "Fri",
        6: "Sat",
        7: "Sun",
    }.get(day, f"{day}")
    return f"Day/time: DoW={dow}({day}), {hour:02d}:{minute:02d}"


def decode_date(data: int) -> str:
    month = (data >> 8) & 0xFF
    day = data & 0xFF
    return f"Date: month={month}, day={day}"


def decode_year(data: int) -> str:
    return f"Year: {data}"


def decode_remote_override_flags(data: int) -> str:
    hb = (data >> 8) & 0xFF
    lb = data & 0xFF
    manual_prio = bool(lb & (1 << 0))
    program_prio = bool(lb & (1 << 1))
    return (
        "Remote override flags: manual_change_priority={}, program_change_priority={}, "
        "HB(raw)=0x{:02X}"
    ).format(int(manual_prio), int(program_prio), hb)


def decode_data(data_id: int, data: int) -> str:
    hb = (data >> 8) & 0xFF
    lb = data & 0xFF

    # --- Status / control ---

    if data_id == 0:
        return f"Status: {decode_status(data)}"

    if data_id == 1:
        return f"Control setpoint (CH water) ≈ {f8_8(data):.2f} °C"

    if data_id == 2:
        return f"Master configuration: {decode_master_config(data)}"

    if data_id == 3:
        return decode_slave_config(data)

    if data_id == 4:
        return decode_command(data)

    if data_id == 5:
        return decode_app_faults(data)

    if data_id == 6:
        return decode_remote_param_flags(data)

    if data_id == 8:
        return f"CH2 control setpoint (TsetCH2) ≈ {f8_8(data):.2f} °C"

    if data_id == 9:
        return f"Remote override room setpoint ≈ {f8_8(data):.2f} °C (0 = no override)"

    if data_id == 14:
        return f"Max relative modulation setting ≈ {f8_8(data):.2f} %"

    if data_id == 15:
        max_cap_kw = hb
        min_mod_pct = lb
        return (
            f"Boiler capacity & min modulation: "
            f"max_capacity={max_cap_kw} kW, min_modulation={min_mod_pct} %"
        )

    # --- Sensor / info 16–39 ---

    if data_id == 16:
        return f"Room setpoint ≈ {f8_8(data):.2f} °C"

    if data_id == 17:
        return f"Relative modulation level ≈ {f8_8(data):.2f} %"

    if data_id == 18:
        return f"CH water pressure ≈ {f8_8(data):.3f} bar"

    if data_id == 19:
        return f"DHW flow rate ≈ {f8_8(data):.3f} l/min"

    if data_id == 20:
        return decode_datetime(data)

    if data_id == 21:
        return decode_date(data)

    if data_id == 22:
        return decode_year(data)

    if data_id == 23:
        return f"Room setpoint CH2 ≈ {f8_8(data):.2f} °C"

    if data_id == 24:
        return f"Room temperature ≈ {f8_8(data):.2f} °C"

    if data_id == 25:
        return f"Boiler water temperature ≈ {f8_8(data):.2f} °C"

    if data_id == 26:
        return f"DHW temperature ≈ {f8_8(data):.2f} °C"

    if data_id == 27:
        return f"Outside temperature ≈ {f8_8(data):.2f} °C"

    if data_id == 28:
        return f"Return water temperature ≈ {f8_8(data):.2f} °C"

    if data_id == 29:
        return f"Solar storage temperature ≈ {f8_8(data):.2f} °C"

    if data_id == 30:
        return f"Solar collector temperature ≈ {s16(data):.2f} °C"

    if data_id == 31:
        return f"Flow temperature CH2 ≈ {f8_8(data):.2f} °C"

    if data_id == 32:
        return f"DHW2 temperature ≈ {f8_8(data):.2f} °C"

    if data_id == 33:
        return f"Boiler exhaust temperature ≈ {s16(data):.2f} °C"

    if data_id == 34:
        return f"Boiler heat exchanger temperature ≈ {f8_8(data):.2f} °C"

    if data_id == 35:
        return f"Boiler fan speed setpoint={hb}, actual={lb}"

    if data_id == 36:
        return f"Flame current ≈ {f8_8(data):.3f} µA"

    if data_id == 37:
        return f"Room temperature CH2 ≈ {f8_8(data):.2f} °C"

    if data_id == 38:
        return f"Relative humidity ≈ {f8_8(data):.2f} %"

    if data_id == 39:
        return f"Remote override room setpoint 2 ≈ {f8_8(data):.2f} °C"

    # --- Remote boiler params ---

    if data_id == 48:
        ub = s8(hb); lb_ = s8(lb)
        return f"DHW setpoint bounds: upper={ub} °C, lower={lb_} °C"

    if data_id == 49:
        ub = s8(hb); lb_ = s8(lb)
        return f"Max CH water setpoint bounds: upper={ub} °C, lower={lb_} °C"

    if data_id == 50:
        ub = s8(hb); lb_ = s8(lb)
        return f"OTC heat curve ratio bounds: upper={ub}, lower={lb_}"

    if data_id == 56:
        return f"DHW setpoint ≈ {f8_8(data):.2f} °C"

    if data_id == 57:
        return f"Max CH water setpoint ≈ {f8_8(data):.2f} °C"

    if data_id == 58:
        return f"OTC heat curve ratio ≈ {f8_8(data):.2f}"

    # --- Overrides / diagnostics / versions ---

    if data_id == 100:
        return decode_remote_override_flags(data)

    if data_id == 115:
        return f"OEM diagnostic/service code: {data} (0x{data:04X})"

    if data_id == 124:
        return f"OpenTherm version (master) ≈ {f8_8(data):.2f}"

    if data_id == 125:
        return f"OpenTherm version (slave) ≈ {f8_8(data):.2f}"

    if data_id == 126:
        return f"Master product info: type={hb}, version={lb}"

    if data_id == 127:
        return f"Slave product info: type={hb}, version={lb}"

    # Manufacturer specific
    if data_id >= 128:
        return (
            f"Manufacturer-specific ID {data_id}: raw=0x{data:04X} "
            f"(HB=0x{hb:02X}, LB=0x{lb:02X}, as SF8.8 ≈ {f8_8(data):.2f})"
        )

    # Fallback
    return (
        f"Unknown/unsupported ID {data_id}, raw=0x{data:04X}, "
        f"as SF8.8 ≈ {f8_8(data):.2f}"
    )


# --------- Formatting helpers ---------

def format_timestamp(ts):
    if ts is None:
        return datetime.now().strftime("%H:%M:%S")
    # try numeric epoch (ms or s)
    if isinstance(ts, (int, float)):
        if ts > 10_000_000_000:  # assume ms
            ts = ts / 1000.0
        return datetime.fromtimestamp(ts).strftime("%H:%M:%S")
    if isinstance(ts, str):
        # just show as-is if parsing is annoying
        try:
            # try ISO
            return datetime.fromisoformat(ts.replace("Z", "+00:00")).strftime("%H:%M:%S")
        except Exception:
            return ts
    return str(ts)


def format_decoded(direction, msg_type_str, data_id, frame: OTFrame, human_data: str, ts) -> str:
    msg_type_name = MSG_TYPES.get(frame.msg_type, f"UNKNOWN({frame.msg_type})")
    t = format_timestamp(ts)
    return (
        f"[{t}] {direction} {msg_type_name} (server said {msg_type_str}) | "
        f"ID:{data_id} | raw=0x{frame.raw:08X} parity={frame.parity} "
        f"data=0x{frame.data:04X} ({frame.data}) | {human_data}"
    )


# --------- Dedup logic: request/response pair ---------

class PairDeduper:
    """
    Keep track of last request/response pair.
    If the next pair is identical (same direction/msg_type/data_id/message on both),
    suppress logging.
    """
    def __init__(self):
        self.last_pair_key = None
        self.pending_req = None  # (printable_line, keydict)
        # keydict: (direction, msg_type, data_id, message)

    def _make_key(self, msg):
        """
        msg: dict with at least direction, msg_type, data_id, message
        """
        return (
            msg.get("direction"),
            msg.get("msg_type"),
            msg.get("data_id"),
            msg.get("message"),
        )

    def handle_message(self, msg, printable_line):
        """
        msg: parsed JSON dict
        printable_line: string for this message if printed alone
        Returns: list of lines to actually print (0, 1, or 2 lines).
        """

        direction = msg.get("direction", "")
        # If it's not a REQUEST or RESPONSE, just print it
        if direction not in ("REQUEST", "RESPONSE"):
            # flush any pending request alone
            lines = []
            if self.pending_req:
                lines.append(self.pending_req[0])
                self.pending_req = None
            lines.append(printable_line)
            return lines

        if direction == "REQUEST":
            lines = []
            # If there is a pending request with no response yet, print it alone
            if self.pending_req:
                lines.append(self.pending_req[0])
            # store new pending request (but don't print yet)
            self.pending_req = (printable_line, self._make_key(msg))
            return lines

        # direction == "RESPONSE"
        if not self.pending_req:
            # No pending request; just print response
            return [printable_line]

        # We have a pending request, pair them
        req_line, req_key = self.pending_req
        resp_key = self._make_key(msg)
        pair_key = (req_key, resp_key)

        self.pending_req = None

        if pair_key == self.last_pair_key:
            # exactly same pair as last time -> suppress
            return []

        # new pair -> print both, remember
        self.last_pair_key = pair_key
        return [req_line, printable_line]


# --------- WebSocket client ---------

async def run(ws_url: str):
    import websockets  # requires: pip install websockets

    deduper = PairDeduper()

    print(f"Connecting to {ws_url} ...", file=sys.stderr)

    async for ws in websockets.connect(ws_url, ping_interval=20, ping_timeout=20):
        try:
            print("Connected.", file=sys.stderr)
            async for message in ws:
                try:
                    data = json.loads(message)
                except json.JSONDecodeError:
                    # plain text status from server
                    ts = None
                    line = f"[{format_timestamp(ts)}] STATUS: {message}"
                    for out in deduper.handle_message(
                        {"direction": "STATUS"}, line
                    ):
                        print(out)
                    continue

                # Expected fields per your JS: direction, msg_type, data_id, data_value, message, timestamp
                direction = data.get("direction", "UNKNOWN")
                msg_type_str = data.get("msg_type", "UNKNOWN")
                data_id = data.get("data_id", -1)
                full_value = data.get("message")  # 32-bit integer frame
                ts = data.get("timestamp", None)

                if full_value is None:
                    # nothing to decode
                    line = f"[{format_timestamp(ts)}] {direction} {msg_type_str} (no raw message field)"
                    for out in deduper.handle_message(data, line):
                        print(out)
                    continue

                try:
                    frame = decode_frame(int(full_value))
                    human_data = decode_data(frame.data_id, frame.data)
                    line = format_decoded(direction, msg_type_str, data_id, frame, human_data, ts)
                except Exception as e:
                    line = f"[{format_timestamp(ts)}] {direction} {msg_type_str} | ID:{data_id} | raw={full_value} | DECODE ERROR: {e}"

                for out in deduper.handle_message(data, line):
                    print(out)

        except websockets.ConnectionClosed:
            print("Disconnected. Reconnecting...", file=sys.stderr)
            await asyncio.sleep(2)
            continue


def main():
    if len(sys.argv) > 1:
        url = sys.argv[1]
    else:
        url = DEFAULT_WS_URL

    asyncio.run(run(url))


if __name__ == "__main__":
    main()
