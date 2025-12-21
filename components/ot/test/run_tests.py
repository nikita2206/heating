#!/usr/bin/env python3
"""
RMT Parser Test Harness

Converts ESP-IDF log format to test cases and runs them against the standalone parser.
Each test runs independently with full debug output captured.
"""

import re
import subprocess
import sys
from pathlib import Path
from typing import List, Tuple, Optional


def parse_log_line(line: str) -> Optional[Tuple[str, str, str]]:
    """
    Parse a log line from ESP-IDF format.
    
    Expected format:
    I (7437) OT: T RMT[31] -> 0x80190000: H520,L492,H521,L1002,...
    
    Returns:
        (device_type, expected_hex, symbols_string) or None if parsing fails
    """
    # Match the log format
    pattern = r'[IW]\s+\(\d+\)\s+OT:\s+([TB])\s+RMT\[\d+\]\s+->\s+(0x[0-9a-fA-F]+):\s+(.+)'
    match = re.match(pattern, line)
    
    if match:
        device_type = match.group(1)  # T=Thermostat/Slave, B=Boiler/Master
        expected_hex = match.group(2)
        symbols = match.group(3).strip()
        return device_type, expected_hex, symbols
    
    return None


def convert_symbols_to_binary_format(symbols_str: str) -> str:
    """
    Convert log format (H520,L492,...) to binary format (level,dur,level,dur;...)
    
    Args:
        symbols_str: "H520,L492,H521,L1002,..."
    
    Returns:
        "1,520,0,492;0,1002,1,521;..."
    """
    # Parse H/L pairs
    pairs = []
    tokens = symbols_str.split(',')
    
    for token in tokens:
        token = token.strip()
        if not token or token == 'L0' or token == 'H0':
            continue  # Skip trailing zeros
            
        if token.startswith('H'):
            level = 1
            duration = token[1:]
        elif token.startswith('L'):
            level = 0
            duration = token[1:]
        else:
            continue
        
        try:
            pairs.append((level, int(duration)))
        except ValueError:
            continue
    
    # Group pairs into symbols (2 parts per symbol)
    symbols = []
    for i in range(0, len(pairs), 2):
        if i + 1 < len(pairs):
            level0, dur0 = pairs[i]
            level1, dur1 = pairs[i + 1]
            symbols.append(f"{level0},{dur0},{level1},{dur1}")
        elif i < len(pairs):
            # Odd number - add final with duration1=0
            level0, dur0 = pairs[i]
            symbols.append(f"{level0},{dur0},0,0")
    
    return ';'.join(symbols)


def compile_test_binary(test_dir: Path, output_file: Path) -> bool:
    """
    Compile the standalone test binary.
    """
    source_file = test_dir / "test_harness.cpp"
    parser_file = test_dir.parent / "rmt_parser.cpp"
    
    print(f"Compiling test harness with production parser code...")
    
    cmd = [
        'g++',
        '-std=c++11',
        '-O2',
        '-Wall',
        '-I', str(test_dir.parent),  # Include parent dir for rmt_parser.h
        '-o', str(output_file),
        str(source_file),
        str(parser_file)
    ]
    
    try:
        result = subprocess.run(cmd, capture_output=True, text=True, check=True)
        print("Compilation successful!")
        return True
    except subprocess.CalledProcessError as e:
        print(f"Compilation failed!", file=sys.stderr)
        print(f"stdout: {e.stdout}", file=sys.stderr)
        print(f"stderr: {e.stderr}", file=sys.stderr)
        return False


def run_single_test(test_binary: Path, symbol_data: str, expected: int, test_num: int) -> Tuple[bool, str]:
    """
    Run a single test case.
    
    Returns:
        (success, output) - success is True if result matches expected
    """
    try:
        result = subprocess.run(
            [str(test_binary), symbol_data],
            capture_output=True,
            text=True,
            timeout=5
        )
        
        output = result.stdout + result.stderr
        
        # Parse result from last line
        result_line = None
        for line in reversed(output.split('\n')):
            if line.startswith('RESULT:'):
                result_line = line
                break
        
        if not result_line:
            return False, output + "\n[ERROR: No RESULT line found in output]"
        
        # Extract hex value
        match = re.search(r'RESULT:\s*(0x[0-9a-fA-F]+)', result_line)
        if not match:
            return False, output + "\n[ERROR: Could not parse RESULT line]"
        
        actual = int(match.group(1), 16)
        success = (actual == expected)
        
        return success, output
        
    except subprocess.TimeoutExpired:
        return False, "[ERROR: Test timed out after 5 seconds]"
    except Exception as e:
        return False, f"[ERROR: Test execution failed: {e}]"


def load_test_cases(log_file: Path) -> List[Tuple[int, str, str]]:
    """
    Load test cases from log file.
    
    Returns:
        List of (expected_value, symbol_data, original_line) tuples
    """
    test_cases = []
    
    with open(log_file) as f:
        for line_num, line in enumerate(f, 1):
            line = line.strip()
            if not line or line.startswith('#'):
                continue
            
            parsed = parse_log_line(line)
            if not parsed:
                print(f"Warning: Could not parse line {line_num}: {line[:80]}...", file=sys.stderr)
                continue
            
            device_type, expected_hex, symbols = parsed
            expected = int(expected_hex, 16)
            symbol_data = convert_symbols_to_binary_format(symbols)
            
            test_cases.append((expected, symbol_data, line[:80]))
    
    return test_cases


def main():
    # Setup paths
    script_dir = Path(__file__).parent
    test_binary = script_dir / "rmt_parser_test"
    log_file = script_dir / "test-inputs.txt"
    
    # Check if log file exists
    if not log_file.exists():
        print(f"Error: {log_file} not found!", file=sys.stderr)
        print(f"Please create test cases file at: {log_file}", file=sys.stderr)
        sys.exit(1)
    
    # Compile the test binary
    if not compile_test_binary(script_dir, test_binary):
        sys.exit(1)
    
    # Load test cases
    print(f"\nLoading test cases from {log_file}...")
    test_cases = load_test_cases(log_file)
    print(f"Found {len(test_cases)} test cases\n")
    
    if not test_cases:
        print("No test cases found!", file=sys.stderr)
        sys.exit(1)
    
    # Run tests
    passed = 0
    failed = 0
    
    for idx, (expected, symbol_data, original) in enumerate(test_cases, 1):
        success, output = run_single_test(test_binary, symbol_data, expected, idx)
        
        if success:
            print(f"Test {idx:3d}: PASS (0x{expected:08x})")
            passed += 1
        else:
            print(f"Test {idx:3d}: FAIL (expected 0x{expected:08x})")
            print(f"  Source: {original}")
            print(f"  Debug output:")
            for line in output.split('\n'):
                if line.strip():
                    print(f"    {line}")
            print()
            failed += 1
    
    # Summary
    print("\n=== Test Summary ===")
    print(f"Total:  {len(test_cases)}")
    print(f"Passed: {passed}")
    print(f"Failed: {failed}")
    
    sys.exit(0 if failed == 0 else 1)


if __name__ == '__main__':
    main()
