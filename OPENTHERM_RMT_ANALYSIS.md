# OpenTherm RMT Implementation Analysis Report

**Date**: 2025-12-13
**Reference Implementation**: `otgateway/opentherm_library/src/OpenTherm.cpp`
**RMT Implementation Files**:
- `components/opentherm_rmt/opentherm_rmt.c` (encoder)
- `components/opentherm/ot_thermostat.c` (decoder + thermostat thread)
- `components/opentherm/ot_boiler.c` (decoder + boiler thread)

---

## Executive Summary

The RMT implementation has **1 hardware-dependent polarity issue** (critical only if using inverting hardware like the reference), **2 medium-severity issues** in the decoder logic, and **3 minor issues** that could cause edge-case failures. The decoder implementations in `ot_thermostat.c` and `ot_boiler.c` are identical (duplicated code).

**Key finding**: No RMT driver-level signal inversion is configured (`flags.invert_out`/`flags.invert_in` not set). The encoder/decoder are self-consistent, so loopback tests pass, but interoperability with the reference implementation depends on hardware interface design.

---

## Bug #1: Manchester Encoder Signal Polarity Difference [HARDWARE-DEPENDENT]

### Location
`components/opentherm_rmt/opentherm_rmt.c:54-69`

### Reference Implementation Behavior
```cpp
// OpenTherm.cpp:91-103
void OpenTherm::sendBit(bool high) {
    if (high)
        setActiveState();  // GPIO → LOW
    else
        setIdleState();    // GPIO → HIGH
    delayMicroseconds(500);
    if (high)
        setIdleState();    // GPIO → HIGH
    else
        setActiveState();  // GPIO → LOW
    delayMicroseconds(500);
}
```

The reference sends a logical "1" bit as:
- **First 500µs**: `setActiveState()` = GPIO **LOW**
- **Second 500µs**: `setIdleState()` = GPIO **HIGH**

This creates a **LOW→HIGH transition** at the mid-bit point for a logical "1".

### RMT Implementation Behavior
```c
// opentherm_rmt.c:54-69
static inline void encode_manchester_bit(rmt_symbol_word_t *symbol, bool bit_value) {
    if (bit_value) {
        // Bit 1: HIGH then LOW
        symbol->level0 = 1;
        symbol->duration0 = OT_HALF_BIT_US;
        symbol->level1 = 0;
        symbol->duration1 = OT_HALF_BIT_US;
    } else {
        // Bit 0: LOW then HIGH
        symbol->level0 = 0;
        symbol->duration0 = OT_HALF_BIT_US;
        symbol->level1 = 1;
        symbol->duration1 = OT_HALF_BIT_US;
    }
}
```

The RMT sends a logical "1" bit as:
- **First 500µs**: level0 = **HIGH**
- **Second 500µs**: level1 = **LOW**

This creates a **HIGH→LOW transition** at the mid-bit point for a logical "1".

### RMT Driver Configuration Check
The RMT channels are initialized **without** inversion flags:
```c
// ot_thermostat.c:333-338, ot_boiler.c:335-340
rmt_tx_channel_config_t tx_config = {
    .gpio_num = config->tx_pin,
    .clk_src = RMT_CLK_SRC_DEFAULT,
    .resolution_hz = 1000000,
    .mem_block_symbols = 64,
    .trans_queue_depth = 4,
    // NO .flags.invert_out set
};
```

The ESP-IDF RMT driver supports `flags.invert_out` for TX and `flags.invert_in` for RX, but neither is configured.

### Analysis

| Bit Value | Reference Output | RMT Output | Match? |
|-----------|-----------------|------------|--------|
| 1 | LOW→HIGH | HIGH→LOW | **NO** |
| 0 | HIGH→LOW | LOW→HIGH | **NO** |

**The polarity is inverted compared to the reference.**

### Self-Consistency Check
The RMT encoder and decoder ARE self-consistent:
- Encoder (`opentherm_rmt.c:56-58`): bit 1 = HIGH→LOW
- Decoder (`ot_thermostat.c:182-184`): HIGH→LOW = bit 1

A loopback test (TX→RX on same device) would work correctly.

### Root Cause
The reference implementation uses `setActiveState()` = GPIO LOW because its target OpenTherm interface hardware **inverts** the signal (common with optocoupler/transistor drivers). The RMT implementation follows the OpenTherm specification directly, assuming **non-inverting** hardware.

### Impact Assessment
- **If hardware inverts the signal** (like reference's hardware): All frames will be inverted → **CRITICAL BUG**
- **If hardware does NOT invert** (direct GPIO to OT bus): Implementation is correct → **NOT A BUG**
- **Loopback testing**: Will always pass (self-consistent) → **Won't detect this issue**

### Recommendation
Verify the hardware interface design:
1. If using inverting hardware: Add `flags.invert_out = true` to TX config and `flags.invert_in = true` to RX config
2. If using non-inverting hardware: Current implementation is correct
3. Consider adding a Kconfig option for polarity to support both hardware types

---

## Bug #2: Decoder Phase Alignment Error Handling [MEDIUM]

### Location
`components/opentherm/ot_thermostat.c:178-185` and `components/opentherm/ot_boiler.c:177-185`

### Code
```c
for (int i = 0; i < hb_count - 1 && bit_count < 32; i += 2) {
    uint8_t first_half = half_bits[i];
    uint8_t second_half = half_bits[i + 1];

    bool bit_value;
    if (first_half == 1 && second_half == 0) bit_value = true;
    else if (first_half == 0 && second_half == 1) bit_value = false;
    else continue;  // <-- PROBLEM
    // ...
}
```

### Reference Implementation Behavior
The reference uses a timing-based state machine (`handleInterrupt()` at lines 218-277) that samples at 750µs (3/4 bit time) after each transition. This is robust against minor timing variations.

### Issue
When the decoder encounters an invalid Manchester transition (`1,1` or `0,0`), it executes `continue` which skips to the next half-bit pair **without adjusting the phase**. This can cause permanent phase misalignment.

**Example of failure scenario:**
```
Received half_bits: [1, 0, 1, 1, 0, 1, 0, ...]
                     ^^^^        ^^^^
                     valid       SKIP (1,1)

After skip, decoder is at index 4:
                     [0, 1] = 0 (WRONG - should be [1, 0] = 1)
```

The reference implementation handles this better by using edge-triggered interrupts with timing validation, so a single glitch doesn't permanently misalign the decoder.

### Impact
- A single noise spike or timing jitter could cause all subsequent bits to be decoded incorrectly
- Could result in intermittent frame corruption that's hard to debug

### Recommendation
After encountering an invalid transition, the decoder should:
1. Try advancing by 1 instead of 2 to re-synchronize
2. Or search for the next valid start bit pattern

---

## Bug #3: No Stop Bit Verification [MEDIUM]

### Location
`components/opentherm/ot_thermostat.c:194-197` and `components/opentherm/ot_boiler.c:194-197`

### Code
```c
if (bit_count != 32) {
    ctx->stats.error_count++;
    return ESP_ERR_INVALID_RESPONSE;
}
```

### Reference Implementation Behavior
```cpp
// OpenTherm.cpp:271-274
else { // stop bit
    status = OpenThermStatus::RESPONSE_READY;
    responseTimestamp = newTs;
}
```

The reference explicitly transitions through a stop bit state after receiving 32 data bits.

### Issue
The RMT implementation only verifies that exactly 32 bits were received after the start bit. It does **not** verify that:
1. A stop bit (always "1") follows the 32 data bits
2. The stop bit has correct timing

### Impact
- Truncated frames that happen to have 32 valid bits could pass validation
- Frames with invalid stop bits (indicating transmission errors) could be accepted
- Reduced error detection capability

---

## Bug #4: Timing Tolerance Gap in Duration Classification [LOW]

### Location
`components/opentherm/ot_thermostat.c:158-162` and `components/opentherm/ot_boiler.c:158-162`

### Code
```c
int num_half_bits;
if (dur >= 350 && dur <= 650) num_half_bits = 1;
else if (dur >= 850 && dur <= 1150) num_half_bits = 2;
else num_half_bits = (dur + 250) / 500;
```

### Issue
There are undefined gaps in the timing bands:
- **Gap 1**: 651-849µs (falls through to formula)
- **Gap 2**: Durations outside defined ranges rely on rounding formula

The OpenTherm specification allows ±10% timing tolerance:
- Half-bit (500µs): 450-550µs valid range
- Full-bit (1000µs): 900-1100µs valid range

The implementation uses wider bands (350-650, 850-1150) which is more lenient, but the gaps could cause issues:

| Duration | Classification | Expected |
|----------|---------------|----------|
| 700µs | (700+250)/500 = 1.9 → 1 | Ambiguous (could be 1 or 2) |
| 800µs | (800+250)/500 = 2.1 → 2 | Ambiguous |

### Impact
- Marginal timing could be classified inconsistently
- Edge cases around 750µs may decode differently than expected

---

## Bug #5: Small Duration Noise Sensitivity [LOW]

### Location
`components/opentherm/ot_thermostat.c:163-165` and `components/opentherm/ot_boiler.c:163-165`

### Code
```c
if (num_half_bits < 1) num_half_bits = 1;
if (num_half_bits > 4) num_half_bits = 4;
```

### Issue
The fallback formula `(dur + 250) / 500` for durations < 250µs:
- 100µs: (100+250)/500 = 0 → clamped to **1 half-bit**
- 50µs: (50+250)/500 = 0 → clamped to **1 half-bit**

This means noise pulses as short as 1µs (above `signal_range_min_ns = 1000` = 1µs) could be interpreted as valid half-bits.

### Reference Implementation Behavior
The reference uses timing windows (e.g., `newTs - responseTimestamp < 750`) to validate signal edges. Spurious edges outside the expected timing are rejected.

### Impact
- High-frequency noise could inject extra half-bits into the stream
- Could cause phase misalignment (compounding Bug #2)

---

## Bug #6: Parity Check Inconsistency with Comments [INFORMATIONAL]

### Location
`components/opentherm/ot_utils.c:72-81`

### Code
```c
bool ot_check_parity(uint32_t frame) {
    uint32_t count = 0;
    uint32_t n = frame;
    while (n) {
        n &= (n - 1);
        count++;
    }
    return (count % 2) == 0;  // Returns true for EVEN parity
}
```

### In Decoder (ot_thermostat.c:200-206)
```c
// Check parity
uint32_t parity_check = decoded;
int ones = 0;
while (parity_check) { parity_check &= (parity_check - 1); ones++; }
if (ones % 2 != 0) {  // Rejects ODD parity
    ctx->stats.error_count++;
    return ESP_ERR_INVALID_CRC;
}
```

### Analysis
Both implementations correctly implement **even parity** checking, matching the OpenTherm specification and the reference implementation. The `ot_check_parity()` utility function returns `true` for valid frames (even number of 1s), while the inline decoder rejects frames with odd parity.

**This is NOT a bug** - just noting the different coding styles between the utility function and inline code.

---

## Code Quality Issues

### Issue #7: Duplicated Decoder Logic

The Manchester decoder is duplicated between:
- `ot_thermostat.c:146-211` (66 lines)
- `ot_boiler.c:145-211` (66 lines)

Both implementations are identical. This violates DRY (Don't Repeat Yourself) and means any bug fixes must be applied in two places.

### Recommendation
Extract the decoder into a shared function in `opentherm_rmt.c` or a new `opentherm_decoder.c`:
```c
esp_err_t opentherm_decode_frame(const rmt_symbol_word_t *symbols,
                                  size_t symbol_count,
                                  uint32_t *frame);
```

---

## Comparison Matrix

| Feature | Reference | RMT Implementation | Status |
|---------|-----------|-------------------|--------|
| TX Manchester encoding | LOW→HIGH = 1 | HIGH→LOW = 1 | **INVERTED** |
| RX Manchester decoding | Timing-based | Duration-based | Different approach |
| Start bit detection | State machine | First "1" bit | Simplified |
| Stop bit verification | Explicit | None | **MISSING** |
| Phase recovery | Implicit (timing) | Skip invalid | **WEAK** |
| Parity (even) | ✓ | ✓ | Match |
| Bit order (MSB first) | ✓ | ✓ | Match |
| 32-bit frame | ✓ | ✓ | Match |
| Timing tolerance | ±10% implied | ±30% explicit | More lenient |

---

## Recommendations

### Critical
1. **Fix encoder polarity** - Either:
   - Invert the RMT levels to match reference behavior (if using same hardware)
   - Document that RMT implementation requires non-inverting hardware
   - Add a configuration option for polarity

### High Priority
2. **Improve phase recovery** - When invalid Manchester transitions occur, implement re-synchronization logic instead of simple skip
3. **Add stop bit verification** - Verify the 34th bit is a valid "1"

### Medium Priority
4. **Refactor decoder** - Extract shared decoder function to eliminate code duplication
5. **Tighten timing tolerances** - Consider rejecting ambiguous durations in the 650-850µs range

### Low Priority
6. **Add noise filtering** - Reject pulses shorter than 200µs as noise
7. **Add diagnostic logging** - Log raw RMT symbols when decode fails for debugging

---

## Test Recommendations

1. **Loopback test** - Connect TX to RX on same device, verify frames round-trip correctly
2. **Polarity test** - Verify TX output with oscilloscope against OpenTherm spec
3. **Noise injection test** - Add artificial noise to verify decoder robustness
4. **Interoperability test** - Test against known-good OpenTherm devices (boiler/thermostat)

---

## Files Analyzed

| File | Lines | Purpose |
|------|-------|---------|
| `otgateway/opentherm_library/src/OpenTherm.cpp` | 579 | Reference implementation |
| `otgateway/opentherm_library/src/OpenTherm.h` | 261 | Reference header |
| `components/opentherm_rmt/opentherm_rmt.c` | 169 | RMT Manchester encoder |
| `components/opentherm_rmt/include/opentherm_rmt.h` | 56 | RMT interface header |
| `components/opentherm/ot_thermostat.c` | 506 | Thermostat thread + decoder |
| `components/opentherm/ot_boiler.c` | 512 | Boiler thread + decoder |
| `components/opentherm/ot_utils.c` | 101 | Shared utilities |
