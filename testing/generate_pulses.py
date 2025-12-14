#!/usr/bin/env python3
"""
Generate OpenTherm Manchester pulses suitable for a C++ test vector.

Input: 32-bit OpenTherm frame as integer (includes parity bit as MSB of the 32-bit frame).
Output: C++ array entries OT_SEG(level, duration_us)

Two stages:
  1) emit_halfbit_stream(): emits (level, duration) per half-bit (typically 500us each)
  2) collapse_runs(): collapses adjacent same-level entries into longer durations

Assumptions (match the parse logic you described earlier):
  - Line is idle LOW before frame
  - Manchester encoding:
      bit 1 => HIGH then LOW (per bit; i.e. two half-bits: [1,0])
      bit 0 => LOW then HIGH (two half-bits: [0,1])
  - Full transmission: start bit '1', then 32 frame bits MSB->LSB, then stop bit '1'
"""

from __future__ import annotations
from dataclasses import dataclass
from typing import Iterable, List, Tuple
import random

Level = bool  # False=LOW, True=HIGH
Pulse = Tuple[Level, int]  # (level, duration_us)


def emit_halfbit_stream(
    frame: int,
    *,
    half_us: int = 500,
    idle_us: int = 5000,
    jitter_us: int = 0,
    seed: int | None = 1,
) -> List[Pulse]:
    """
    Stage 1: Emit a stream of (level, duration_us) where every half-bit is one pulse.

    Includes:
      - leading idle (LOW, idle_us)
      - start bit = 1
      - 32 frame bits MSB->LSB
      - stop bit = 1

    jitter_us applies to each half-bit duration: duration += randint(-jitter_us, +jitter_us)
    """
    if seed is not None:
        random.seed(seed)

    if not (0 <= frame <= 0xFFFFFFFF):
        raise ValueError("frame must fit in 32 bits")

    pulses: List[Pulse] = []

    # Leading idle (LOW). No jitter on idle by default.
    pulses.append((False, idle_us))

    def half(level: Level) -> None:
        dur = half_us
        if jitter_us:
            dur += random.randint(-jitter_us, jitter_us)
            if dur < 1:
                dur = 1
        pulses.append((level, dur))

    def emit_bit(bit: int) -> None:
        # Manchester per-bit (2 half-bits)
        if bit == 1:
            half(True)
            half(False)
        else:
            half(False)
            half(True)

    # Start bit (1)
    emit_bit(1)

    # 32-bit frame, MSB->LSB
    for i in range(31, -1, -1):
        emit_bit((frame >> i) & 1)

    # Stop bit (1)
    emit_bit(1)

    return pulses


def collapse_runs(pulses: Iterable[Pulse]) -> List[Pulse]:
    """
    Stage 2: Collapse adjacent pulses with the same level.
      (False, 500), (False, 500) -> (False, 1000)
    """
    out: List[Pulse] = []
    for level, dur in pulses:
        if dur <= 0:
            raise ValueError("duration must be positive")
        if not out or out[-1][0] != level:
            out.append((level, dur))
        else:
            out[-1] = (level, out[-1][1] + dur)
    return out


def to_cpp_array(
    pulses: List[Pulse],
    *,
    array_name: str = "interruptTimestamps",
    total_size: int = 69,
    pad_level: Level = False,
    pad_us: int = 7000,
) -> str:
    """
    Emit C++ code for:
      - OT_SEG helper
      - upToIndex
      - uint64_t array with packed MSB=level
    """
    up_to_index = len(pulses)
    if up_to_index > total_size:
        raise ValueError(f"Need {up_to_index} elements but total_size is {total_size}")

    lines: List[str] = []
    lines.append('#include <cstdint>\n')
    lines.append('// Pack: MSB = pin level (1=HIGH), lower 63 bits = duration in microseconds\n')
    lines.append('constexpr uint64_t OT_SEG(bool levelHigh, uint32_t us) {\n')
    lines.append('    return (uint64_t(levelHigh) << 63) | uint64_t(us);\n')
    lines.append('}\n\n')
    lines.append(f'constexpr int kUpToIndex = {up_to_index};\n\n')
    lines.append(f'uint64_t {array_name}[{total_size}] = {{\n')

    for idx, (lvl, dur) in enumerate(pulses):
        lvl_s = "true " if lvl else "false"
        lines.append(f'    OT_SEG({lvl_s}, {dur:5d}), // [{idx:02d}] {"HIGH" if lvl else "LOW "} for {dur} us\n')

    # Padding
    for idx in range(up_to_index, total_size):
        lvl_s = "true " if pad_level else "false"
        lines.append(f'    OT_SEG({lvl_s}, {pad_us:5d}), // [{idx:02d}] padding\n')

    lines.append('};\n')
    return "".join(lines)


def generate_cpp_test_vector(
    frame: int,
    *,
    half_us: int = 500,
    idle_us: int = 5000,
    jitter_us: int = 0,
    seed: int | None = 1,
    collapse: bool = True,
    total_size: int = 69,
) -> str:
    """
    Convenience wrapper: stage1 -> stage2 -> C++ code.
    """
    stage1 = emit_halfbit_stream(
        frame,
        half_us=half_us,
        idle_us=idle_us,
        jitter_us=jitter_us,
        seed=seed,
    )
    stage2 = collapse_runs(stage1) if collapse else stage1
    return to_cpp_array(stage2, total_size=total_size)


import sys

def parse_frame_arg(arg: str) -> int:
    """
    Parse input argument for frame as int.
    Accepts: hex (0x...), binary (0b...), or 32 consecutive 0/1 chars.
    """
    s = arg.strip().lower()
    if s.startswith("0x"):
        return int(s, 16)
    elif s.startswith("0b"):
        return int(s, 2)
    elif all(c in "01" for c in s) and len(s) == 32:
        return int(s, 2)
    else:
        raise ValueError(
            f"Unable to parse frame argument: {arg}\n"
            "Expected format: 0xHEX, 0bBINARY, or 32 bits like 010101..."
        )

if __name__ == "__main__":
    if len(sys.argv) != 2:
        print("Usage: python generate_pulses.py <frame>")
        print("  <frame> can be 0xHEX, 0bBINARY, or a 32-character binary string (010101...)")
        sys.exit(1)

    try:
        frame = parse_frame_arg(sys.argv[1])
    except Exception as e:
        print(e)
        sys.exit(1)

    cpp = generate_cpp_test_vector(
        frame,
        half_us=500,
        idle_us=5000,
        jitter_us=80,
        seed=42,
        collapse=True,
        total_size=69,
    )
    print(cpp)
