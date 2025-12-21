/*
 * RMT Symbol Parser for OpenTherm Manchester Decoding
 * 
 * This implementation works both in ESP-IDF and standalone test environments.
 */

#include "rmt_parser.h"
#include <cstdio>
#include <cstring>

namespace ot {

void buildRMTSymbolLogString(rmt_symbol_word_t* symbols, size_t num_symbols, 
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

uint32_t parseRMTSymbols(rmt_symbol_word_t* symbols, size_t num_symbols, bool isSlave)
{
    // Manchester decoding for OpenTherm using RMT symbols
    // Each RMT symbol represents a pulse: level0 for duration0, then level1 for duration1
    // For Manchester: bit '1' = high->low, bit '0' = low->high

    constexpr int ONE_MIN = 300;      // Minimum half-bit duration
    constexpr int ONE_MAX = 700;      // Maximum half-bit duration
    constexpr int TWO_MIN = 700;      // Minimum 2 half-bits
    constexpr int TWO_MAX = 1300;     // Maximum 2 half-bits

    // Helper to log RMT symbols on error
    auto logRMTError = [&](const char* error_msg) {
        char logBuf[512];
        buildRMTSymbolLogString(symbols, num_symbols, logBuf, sizeof(logBuf));
        ESP_LOGW("OT", "%s RMT[%zu] FAILED (%s): %s", 
                 isSlave ? "T" : "B", num_symbols, error_msg, logBuf);
    };

    // We expect 68 half-bits for a complete frame (1 start + 32 data + 1 stop bits × 2)
    bool halfLevels[68];
    int halfCount = 0;
    bool skipFirstPart = false;

    // Check if we need to prepend an implicit HIGH half-bit
    // When idle is HIGH and first data symbol is LOW, the start bit's first half is merged with idle
    if (num_symbols > 0) {
        rmt_symbol_word_t first = symbols[0];
        if (first.level0 == 1 && first.duration0 > 0) {
            // First symbol starts HIGH - check if second part is LOW
            if (first.level1 == 0 && first.duration1 > 0) {
                // Idle HIGH merged with start bit - prepend implicit HIGH half
                // and skip the first H part (which is the merged idle)
                halfLevels[halfCount++] = 1;
                skipFirstPart = true;
            }
        }
    }

    // Process each RMT symbol
    for (size_t i = 0; i < num_symbols && halfCount < 68; i++) {
        rmt_symbol_word_t symbol = symbols[i];

        // Each symbol has two parts: duration0/level0 and duration1/level1
        uint32_t durations[2] = {symbol.duration0, symbol.duration1};
        uint32_t levels[2] = {symbol.level0, symbol.level1};

        // Process each part of the symbol
        for (int part = 0; part < 2 && halfCount < 68; part++) {
            // Skip the first part of first symbol if we prepended implicit HIGH
            if (i == 0 && part == 0 && skipFirstPart) {
                continue;
            }

            uint32_t dur = durations[part];
            bool level = levels[part];

            // Skip duration-0 entries (RMT end-of-frame marker or noise)
            if (dur == 0) continue;

            // Skip very short pulses (noise) but not zero
            if (dur < 100) continue;

            // Validate duration is within acceptable range
            int halves = 0;
            if (dur >= ONE_MIN && dur < ONE_MAX) {
                halves = 1;
            } else if (dur >= TWO_MIN && dur <= TWO_MAX) {
                halves = 2;
            } else {
                char errorMsg[64];
                snprintf(errorMsg, sizeof(errorMsg), "Invalid duration %lu μs at symbol %zu part %d", 
                        (unsigned long)dur, i, part);
                logRMTError(errorMsg);
                return 0;  // Invalid frame
            }

            // Skip the first part of first symbol if we prepended implicit HIGH
            // (but we still validated it above)
            if (i == 0 && part == 0 && skipFirstPart) {
                continue;
            }

            // Add the appropriate number of half-bits
            for (int k = 0; k < halves && halfCount < 68; k++) {
                halfLevels[halfCount++] = level;
            }
        }
    }

    // If we're exactly 1 short and last level was HIGH, infer final LOW half-bit
    // (stop bit second half isn't captured because signal stays LOW with no edge)
    if (halfCount == 67 && halfLevels[66] == 1) {
        halfLevels[halfCount++] = 0;  // Infer final LOW half-bit
    }

    // We need exactly 68 half-bits for a complete frame
    if (halfCount != 68) {
        char errorMsg[64];
        snprintf(errorMsg, sizeof(errorMsg), "Half count %d (expected 68)", halfCount);
        logRMTError(errorMsg);
        return 0;
    }

    // Decode Manchester: pairs of half-bits form one data bit
    uint8_t bits[34];
    for (int b = 0; b < 34; b++) {
        bool a = halfLevels[2 * b];     // First half-bit
        bool c = halfLevels[2 * b + 1]; // Second half-bit

        if (a == c) {
            char errorMsg[64];
            snprintf(errorMsg, sizeof(errorMsg), "Manchester no transition at bit %d", b);
            logRMTError(errorMsg);
            return 0;
        }

        // Manchester encoding: high->low = '1', low->high = '0'
        if (a == 1 && c == 0) bits[b] = 1;      // Falling edge = '1'
        else if (a == 0 && c == 1) bits[b] = 0; // Rising edge = '0'
        else {
            char errorMsg[64];
            snprintf(errorMsg, sizeof(errorMsg), "Manchester wrong transition at bit %d", b);
            logRMTError(errorMsg);
            return 0;  // Should not reach here
        }
    }

    // Start and stop bits must be '1'
    if (bits[0] != 1) {
        char errorMsg[64];
        snprintf(errorMsg, sizeof(errorMsg), "Invalid start bit: %d", bits[0]);
        logRMTError(errorMsg);
        return 0;
    }
    if (bits[33] != 1) {
        char errorMsg[64];
        snprintf(errorMsg, sizeof(errorMsg), "Invalid stop bit: %d", bits[33]);
        logRMTError(errorMsg);
        return 0;
    }

    // Build 32-bit frame from bits[1..32], MSB first
    uint32_t frame = 0;
    for (int b = 1; b <= 32; b++) {
        frame = (frame << 1) | (bits[b] & 1u);
    }

    // Even parity check on the 32 bits
    if (__builtin_popcount(frame) & 1) {
        char errorMsg[64];
        snprintf(errorMsg, sizeof(errorMsg), "Invalid parity (%d ones)", __builtin_popcount(frame));
        logRMTError(errorMsg);
        return 0;  // Invalid parity
    }

    return frame;
}

} // namespace ot

