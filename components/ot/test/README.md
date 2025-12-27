# RMT Encoder Regression Tests

Tests the OpenTherm RMT symbol parser/encoder using real-world data captured from ESP32.

## Quick Start

```bash
# 1. Collect test data from ESP32
ot.setRMTDebugLogging(true);  // Logs: I (time) OT: T/B RMT[n] -> 0xHEX: H520,L492,...

# 2. Append log lines to test-inputs.txt

# 3. Run tests
./run_tests.py
```

## Architecture

- **`../rmt_encoder.{h,cpp}`** - Production parser/encoder code (single source of truth)
- **`test_harness.cpp`** - Minimal C++ runner (~100 lines)
  - Accepts CLI arg: `level0,dur0,level1,dur1;...`
  - Calls production parser
  - Prints `RESULT: 0xHEXVALUE`
- **`run_tests.py`** - Python orchestrator
  - Parses ESP-IDF logs
  - Runs each test independently
  - Shows debug output only on failure

## Test Flow

```
ESP-IDF Log → Python parses → Simple format → C++ binary → Compare result
                ↓                    ↓              ↓
            test-inputs.txt    "1,520,0,492;"   RESULT: 0x...
```

## Example Output

**Success:**
```
Test   1: PASS (0x80190000)
Test   2: PASS (0x4019209e)
```

**Failure (with debug output):**
```
Test   5: FAIL (expected 0x80190000)
  Source: I (7437) OT: T RMT[2] -> 0x80190000: H1500,L492
  Debug output:
    W (OT): B RMT[1] FAILED (Invalid duration 1500 μs at symbol 0 part 0): H1500,L492
    RESULT: 0x00000000
```
