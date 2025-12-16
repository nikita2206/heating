#!/usr/bin/env python3
"""
OpenTherm Manchester decoder - mirrors the C++ parseInterrupts logic
"""

import sys
import re
import os

# Import functions from humanize_serial_log.py for detailed frame decoding
sys.path.append(os.path.dirname(__file__))
from humanize_serial_log import process_line

# Constants matching C++ code
ONE_MIN = 300
ONE_MAX = 700
TWO_MIN = 700
TWO_MAX = 1300
GLITCH_MAX_US = 200

def parse_irq_string(s):
    """Parse IRQ string like 'L506219,H505,L508,...' into list of (level, duration)"""
    entries = []
    for part in s.split(','):
        part = part.strip()
        if not part:
            continue
        level = 1 if part[0] == 'H' else 0
        dur = int(part[1:])
        entries.append((level, dur))
    return entries

def parse_input_line(line):
    """Parse input line - can be interrupt string, hex frame, or binary frame"""
    line = line.strip()
    if not line:
        return None

    # Check for command keywords first
    if line.lower() in ('quit', 'exit'):
        return 'command', line.lower()

    # Check if it's a hex frame (0x12345678)
    if line.startswith('0x') or line.startswith('0X'):
        try:
            frame = int(line, 16)
            if 0 <= frame <= 0xFFFFFFFF:
                return 'frame', frame
        except ValueError:
            pass

    # Check if it's a 32-bit binary string
    if len(line) == 32 and all(c in '01' for c in line):
        try:
            frame = int(line, 2)
            return 'frame', frame
        except ValueError:
            pass

    # Assume it's an interrupt string
    return 'irq', line

def process_decoded_frame(frame):
    """Process a decoded 32-bit frame using humanize_serial_log.py capabilities"""
    format_frame_with_humanize(frame)

def decode_opentherm(interrupts):
    """Decode OpenTherm frame from interrupt timestamps"""
    half_levels = []

    # Check if start bit first half merged with idle
    # When idle is HIGH and start bit first half is also HIGH, they merge
    if len(interrupts) > 1 and interrupts[0][0] == 1 and interrupts[1][0] == 0:
        half_levels.append(1)  # Implicit start bit first half (HIGH)
        print(f"  [Start fix] Idle was HIGH, first data is LOW -> prepend implicit HIGH half")

    i = 1  # Skip index 0 (idle)
    while i < len(interrupts) and len(half_levels) < 68:
        level, dur = interrupts[i]

        # Handle glitches
        if dur < GLITCH_MAX_US:
            # Check if restoring this glitch would prevent a Manchester error
            if len(half_levels) > 0 and i + 1 < len(interrupts):
                prev_half_level = half_levels[-1]
                next_level, next_dur = interrupts[i + 1]

                if prev_half_level == next_level and next_dur >= ONE_MIN and level != prev_half_level:
                    # Check if previous segment was "stretched" (borderline 2-half that should be 1-half)
                    # This happens when a glitch squishes: the time gets redistributed to neighbors
                    # Pattern: ~900µs + ~75µs glitch + ~500µs = ~1500µs total (should be 3 half-bits)
                    if i > 1:
                        prev_seg_dur = interrupts[i - 1][1]
                        # If prev segment was 700-1100µs (stretched 1-half counted as 2), remove one half
                        if 700 <= prev_seg_dur <= 1100 and len(half_levels) >= 2 and half_levels[-2] == prev_half_level:
                            half_levels.pop()  # Remove one of the stretched segment's halves
                            print(f"  [Glitch fix] Removed 1 stretched half from previous segment ({prev_seg_dur}us)")

                    half_levels.append(level)
                    print(f"  [Glitch restore] idx={i} {'H' if level else 'L'}{dur}us restored as 1 half (prev={prev_half_level}, next={next_level})")
                else:
                    print(f"  [Glitch skip] idx={i} {'H' if level else 'L'}{dur}us skipped")
            else:
                print(f"  [Glitch skip] idx={i} {'H' if level else 'L'}{dur}us skipped (no context)")
            i += 1
            continue

        # Merge consecutive same-level entries (ISR timing glitches)
        # E.g., L506,L5,H502 -> L511,H502
        # BUT: check for pattern where a glitch separates same-level segments that shouldn't merge.
        # E.g., L987,L514,H4,L482 -> the L514 was misread and should be H514 (flip and merge with H4)
        orig_i = i
        while i + 1 < len(interrupts) and interrupts[i + 1][0] == level:
            next_dur = interrupts[i + 1][1]

            # Check for flip pattern: next segment is same level, followed by glitch of opposite level,
            # followed by segment of same level as current
            if (ONE_MIN <= next_dur <= ONE_MAX and
                i + 2 < len(interrupts) and
                interrupts[i + 2][1] < GLITCH_MAX_US and  # i+2 is a glitch
                interrupts[i + 2][0] != level and         # glitch has opposite level
                i + 3 < len(interrupts) and
                interrupts[i + 3][0] == level):           # i+3 back to original level
                # Don't merge - stop here and let the flip logic handle it
                print(f"  [Pattern] Detected flip pattern at idx={i+1}, stopping merge")
                break

            i += 1
            dur += interrupts[i][1]

        if i != orig_i:
            print(f"  [Merge] idx={orig_i}-{i} merged to {'H' if level else 'L'}{dur}us")

        # After processing current segment, check if next should be flipped.
        # Pattern: we just processed L987, now check if L514,H4 at i+1,i+2 should become H518
        if (i + 2 < len(interrupts) and
            interrupts[i + 1][0] == level and                    # i+1 same level as current
            ONE_MIN <= interrupts[i + 1][1] <= ONE_MAX and       # i+1 is ~1 half-bit
            interrupts[i + 2][1] < GLITCH_MAX_US and             # i+2 is glitch
            interrupts[i + 2][0] != level):                      # glitch is opposite level

            glitch_level = interrupts[i + 2][0]
            flipped_dur = interrupts[i + 1][1] + interrupts[i + 2][1]

            # First, add halves for current segment
            if ONE_MIN <= dur < ONE_MAX:
                halves = 1
            elif TWO_MIN <= dur <= TWO_MAX:
                halves = 2
            else:
                print(f"  [ERROR] Invalid duration: {dur}us at index {i}")
                return None

            for _ in range(halves):
                if len(half_levels) < 68:
                    half_levels.append(level)

            print(f"  idx={orig_i}: {'H' if level else 'L'}{dur}us -> {halves} half(s) -> half_count={len(half_levels)}")

            # Now add the flipped segment
            if ONE_MIN <= flipped_dur < ONE_MAX:
                half_levels.append(glitch_level)
                print(f"  [Flip] idx={i+1},{i+2} {'H' if interrupts[i+1][0] else 'L'}{interrupts[i+1][1]}+{'H' if glitch_level else 'L'}{interrupts[i+2][1]} -> {'H' if glitch_level else 'L'}{flipped_dur}us -> 1 half -> half_count={len(half_levels)}")
            elif TWO_MIN <= flipped_dur <= TWO_MAX:
                half_levels.append(glitch_level)
                if len(half_levels) < 68:
                    half_levels.append(glitch_level)
                print(f"  [Flip] idx={i+1},{i+2} {'H' if interrupts[i+1][0] else 'L'}{interrupts[i+1][1]}+{'H' if glitch_level else 'L'}{interrupts[i+2][1]} -> {'H' if glitch_level else 'L'}{flipped_dur}us -> 2 halves -> half_count={len(half_levels)}")

            # Skip both the misread segment and the glitch
            i += 3
            continue

        # Classify duration
        if ONE_MIN <= dur < ONE_MAX:
            halves = 1
        elif TWO_MIN <= dur <= TWO_MAX:
            halves = 2
        else:
            print(f"  [ERROR] Invalid duration: {dur}us at index {i}")
            return None

        for _ in range(halves):
            if len(half_levels) < 68:
                half_levels.append(level)

        print(f"  idx={orig_i}: {'H' if level else 'L'}{dur}us -> {halves} half(s) -> half_count={len(half_levels)}")
        i += 1

    # Infer final half-bit if needed
    if len(half_levels) == 67 and half_levels[66] == 1:
        half_levels.append(0)
        print(f"  [End fix] Inferred final LOW half-bit")

    if len(half_levels) != 68:
        print(f"  [ERROR] Half count mismatch: {len(half_levels)} (expected 68)")
        return None

    # Decode Manchester bits
    bits = []
    for b in range(34):
        a = half_levels[2 * b]
        c = half_levels[2 * b + 1]

        if a == c:
            print(f"  [ERROR] Invalid Manchester (two same halves) at bit {b}: {a},{c}")
            return None

        if a == 1 and c == 0:
            bits.append(1)
        elif a == 0 and c == 1:
            bits.append(0)
        else:
            print(f"  [ERROR] Invalid Manchester at bit {b}")
            return None

    # Check start/stop bits
    if bits[0] != 1:
        print(f"  [ERROR] Invalid start bit: {bits[0]}")
        return None
    if bits[33] != 1:
        print(f"  [ERROR] Invalid stop bit: {bits[33]}")
        return None

    # Build 32-bit frame
    frame = 0
    for b in range(1, 33):
        frame = (frame << 1) | bits[b]

    # Check parity
    if bin(frame).count('1') % 2 != 0:
        print(f"  [ERROR] Invalid parity")
        return None

    return frame, bits

def format_frame_with_humanize(frame):
    """Format frame using humanize_serial_log.py decoding capabilities"""
    # Convert 32-bit frame to binary string (32 bits)
    frame_bits = f"{frame:032b}"

    # Determine source: T (master) for msg_types 0-3, B (slave) for 4-7
    msg_type = (frame >> 28) & 0x7
    src = 'T' if msg_type <= 3 else 'B'

    # Create line in format expected by humanize_serial_log.py
    line = f"OT: Incoming frame from {src}: {frame_bits}"

    # Use process_line to get the detailed decoding
    print(f"Decoded frame:")
    process_line(line)

def process_input(input_str):
    """Process a single input string (interrupt string or pre-decoded frame)"""
    parsed = parse_input_line(input_str)
    if parsed is None:
        return None

    input_type, data = parsed

    if input_type == 'command':
        return data  # Return the command

    if input_type == 'frame':
        # Already decoded frame
        frame = data
        print(f"Input: Pre-decoded frame 0x{frame:08X}")
        print()
        process_decoded_frame(frame)

    elif input_type == 'irq':
        # Interrupt string - need to decode
        irq_string = data
        print(f"Input: {irq_string[:80]}{'...' if len(irq_string) > 80 else ''}")
        print()

        interrupts = parse_irq_string(irq_string)
        print(f"Parsed {len(interrupts)} interrupt entries")
        print()

        print("Decoding Manchester signal:")
        result = decode_opentherm(interrupts)
        print()

        if result:
            frame, bits = result
            bits_str = ''.join(str(b) for b in bits[1:33])  # 32 data bits
            print("Manchester decoding successful!")
            print(f"  Binary: {bits_str}")
            print(f"  Hex: 0x{frame:08X}")
            print()
            process_decoded_frame(frame)
        else:
            print("Failed to decode Manchester signal to frame")

    return None

def main():
    # Check for command line arguments (backward compatibility)
    if len(sys.argv) > 1:
        # Command line mode - process the argument as input
        input_str = sys.argv[1]
        process_input(input_str)
        return

    if sys.stdin.isatty():
        # Interactive mode
        print("OpenTherm Manchester Decoder - Interactive Mode")
        print("Supported input formats:")
        print("  - Interrupt strings: 'L506219,H505,L508,...' (Manchester encoded)")
        print("  - Hex frames: '0x12345678' (32-bit OpenTherm frames)")
        print("  - Binary frames: '01000000100100000000000000000000' (32-bit binary)")
        print("Type 'quit', 'exit', or press Ctrl+D to exit")
        print("-" * 70)

        try:
            while True:
                try:
                    line = input("> ").strip()
                    if line:
                        result = process_input(line)
                        if result in ('quit', 'exit'):
                            break
                        print()  # Add blank line between inputs
                except EOFError:
                    break
        except KeyboardInterrupt:
            print("\nExiting...")
    else:
        # Streaming mode (stdin is piped)
        for line in sys.stdin:
            line = line.strip()
            if line:
                process_input(line)
                print()  # Add blank line between inputs

if __name__ == "__main__":
    main()
