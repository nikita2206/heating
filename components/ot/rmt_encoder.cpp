/*
 * RMT Symbol Parser for OpenTherm Manchester Decoding
 * 
 * This implementation works both in ESP-IDF and standalone test environments.
 * It is important to keep this file cross-compatible with both environments.
 */

#include "rmt_encoder.h"
#include <cstdio>
#include <cstring>

namespace ot {

void rmtSymbolsToString(rmt_symbol_word_t* symbols, size_t num_symbols, 
                              char* buffer, size_t bufferSize)
{
    // Build a compact string showing level and duration for each symbol part
    // Format: "L:dur,H:dur,..." (L=low, H=high)
    int logPos = 0;
    for (size_t i = 0; i < num_symbols && logPos < (int)bufferSize - 12; i++) {
        rmt_symbol_word_t symbol = symbols[i];
        // Log both parts of each symbol
        for (int part = 0; part < 2 && logPos < (int)bufferSize - 12; part++) {
            uint32_t dur = (part == 0) ? symbol.duration0 : symbol.duration1;
            uint32_t level = (part == 0) ? symbol.level0 : symbol.level1;
            int written = snprintf(buffer + logPos, bufferSize - logPos,
                                   "%s%c%lu", (i > 0 || part > 0 ? "," : ""), 
                                   (level ? 'H' : 'L'), (unsigned long)dur);
            if (written > 0) logPos += written;
        }
    }
}

size_t encodeOpenThermAsRmt(unsigned long frame, rmt_symbol_word_t* symbols)
{
    // OpenTherm Manchester encoding:
    // Bit '1' = HIGH for 500μs, then LOW for 500μs (falling edge in middle)
    // Bit '0' = LOW for 500μs, then HIGH for 500μs (rising edge in middle)
    // Frame: Start bit (1) + 32 data bits (MSB first) + Stop bit (1) = 34 bits

    constexpr uint32_t HALF_BIT_US = 500;  // 500μs per half-bit

    // Determine actual levels
    // Normal: active=LOW (0), idle=HIGH (1) for OpenTherm interface
    const uint32_t activeLevel = 0;
    const uint32_t idleLevel = 1;

    size_t symbolIdx = 0;

    // Encode 34 bits: start (1) + 32 data bits + stop (1)
    // Build the full 34-bit sequence
    uint64_t fullFrame = 0;
    fullFrame |= (1ULL << 33);                    // Start bit (bit 33)
    fullFrame |= ((uint64_t)frame << 1);          // Data bits (bits 32-1)
    fullFrame |= 1ULL;                             // Stop bit (bit 0)

    for (int i = 33; i >= 0; i--) {
        bool bit = (fullFrame >> i) & 1;

        if (bit) {
            // Bit '1': active->idle (HIGH then LOW normally, but inverted per interface)
            // OpenTherm: '1' = high-to-low transition in middle of bit period
            symbols[symbolIdx].level0 = activeLevel;
            symbols[symbolIdx].duration0 = HALF_BIT_US;
            symbols[symbolIdx].level1 = idleLevel;
            symbols[symbolIdx].duration1 = HALF_BIT_US;
        } else {
            // Bit '0': idle->active (LOW then HIGH normally)
            // OpenTherm: '0' = low-to-high transition in middle of bit period
            symbols[symbolIdx].level0 = idleLevel;
            symbols[symbolIdx].duration0 = HALF_BIT_US;
            symbols[symbolIdx].level1 = activeLevel;
            symbols[symbolIdx].duration1 = HALF_BIT_US;
        }
        symbolIdx++;
    }

    return symbolIdx;  // Should be 34
}

uint32_t decodeRmtAsOpenTherm(rmt_symbol_word_t* symbols, size_t num_symbols, bool isSlave)
{
    // Manchester decoding for OpenTherm using RMT symbols
    // Single-pass: decode directly to frame without intermediate arrays
    // For Manchester: bit '1' = HIGH->LOW, bit '0' = LOW->HIGH

    #define IS_SINGLE_HALF(dur) ((dur) >= 300 && (dur) < 700) // Single half-bit duration is 500ms, +-200ms
    #define IS_DOUBLE_HALF(dur) ((dur) >= 700 && (dur) <= 1300) // Double half-bit duration is 1000ms, +-200ms

    uint32_t frame = 0;
    int bitIndex = 0;              // Bit being decoded (0=start, 1-32=data, 33=stop)
    bool inSecondHalf = false;     // Waiting for second half of Manchester bit
    bool firstHalfHigh = false;    // Level of first half (valid when inSecondHalf)
    const char* error = nullptr;   // Error reason (null = success)

    // Detect implicit HIGH start (idle HIGH merged with start bit's first half)
    bool skipFirst = num_symbols > 0 &&
                     symbols[0].level0 == 1 && symbols[0].duration0 > 0 &&
                     symbols[0].level1 == 0 && symbols[0].duration1 > 0;
    if (skipFirst) {
        firstHalfHigh = true;
        inSecondHalf = true;
    }

    // Process one half-bit: either record first half or complete a Manchester bit
    #define PROCESS_HALF_BIT() \
        if (bitIndex < 34 && !error) { \
            if (!inSecondHalf) { \
                firstHalfHigh = level; \
                inSecondHalf = true; \
            } else if (firstHalfHigh == level) { \
                error = "no transition"; \
            } else { \
                bool bit = firstHalfHigh && !level; \
                if (bitIndex == 0 && !bit) error = "bad start"; \
                else if (bitIndex == 33 && !bit) error = "bad stop"; \
                else { \
                    if (bitIndex <= 32) frame = (frame << 1) | bit; \
                    bitIndex++; \
                    inSecondHalf = false; \
                } \
            } \
        }

    // Flatten symbol parts into single stream: idx 0,1 = symbol[0], idx 2,3 = symbol[1], etc.
    for (size_t idx = skipFirst ? 1 : 0; idx < num_symbols * 2 && bitIndex < 34 && !error; idx++) {
        rmt_symbol_word_t& sym = symbols[idx >> 1];
        bool secondPart = idx & 1;

        uint32_t dur = secondPart ? sym.duration1 : sym.duration0;
        if (dur == 0 || dur < 100) continue;  // Skip noise/end markers

        int halves = IS_SINGLE_HALF(dur) ? 1 : IS_DOUBLE_HALF(dur) ? 2 : 0;
        if (halves == 0) { error = "bad duration"; break; }

        bool level = secondPart ? sym.level1 : sym.level0;

        switch (halves) {
            case 2: PROCESS_HALF_BIT(); [[fallthrough]];
            case 1: PROCESS_HALF_BIT(); break;
        }
    }

    // Infer missing final LOW (stop bit's second half produces no edge)
    if (!error && bitIndex == 33 && inSecondHalf && firstHalfHigh) bitIndex++;

    if (!error && bitIndex != 34) error = "incomplete";
    if (!error && (__builtin_popcount(frame) & 1)) error = "parity";

    if (error) {
        char logBuf[512];
        rmtSymbolsToString(symbols, num_symbols, logBuf, sizeof(logBuf));
        ESP_LOGW("OT", "%s RMT[%zu] FAILED (%s @bit %d): %s",
                 isSlave ? "T" : "B", num_symbols, error, bitIndex, logBuf);
        return 0;
    }

    return frame;
}

} // namespace ot

