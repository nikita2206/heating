/*
 * RMT Symbol Parser for OpenTherm Manchester Decoding
 * 
 * This file contains the core parsing logic extracted for testability.
 * It can be compiled both for ESP-IDF (production) and standalone (testing).
 */

#ifndef RMT_PARSER_H
#define RMT_PARSER_H

#include <cstdint>
#include <cstddef>

// Conditional compilation for ESP-IDF vs standalone
#ifdef ESP_PLATFORM
    #include "driver/rmt_types.h"
    #include "esp_log.h"
#else
    // Standalone mode - mock the types
    struct rmt_symbol_word_t {
        uint32_t duration0 : 15;
        uint32_t level0 : 1;
        uint32_t duration1 : 15;
        uint32_t level1 : 1;
    };
    
    // Mock logging for standalone - actually print for test visibility
    #include <cstdio>
    #define ESP_LOGW(tag, fmt, ...) fprintf(stderr, "W (%s): " fmt "\n", tag, ##__VA_ARGS__)
    #define ESP_LOGI(tag, fmt, ...) fprintf(stdout, "I (%s): " fmt "\n", tag, ##__VA_ARGS__)
#endif

namespace ot {

/**
 * Parse RMT symbols into an OpenTherm frame
 * 
 * Decodes Manchester-encoded OpenTherm data from RMT peripheral symbols.
 * 
 * @param symbols Array of RMT symbols (max 128)
 * @param num_symbols Number of symbols in the array
 * @param isSlave Whether this is slave (thermostat) mode (affects logging only)
 * @return Decoded 32-bit frame, or 0 if parsing failed
 */
uint32_t parseRMTSymbols(rmt_symbol_word_t* symbols, size_t num_symbols, bool isSlave = false);

/**
 * Build a compact log string from RMT symbols for debugging
 * 
 * @param symbols Array of RMT symbols
 * @param num_symbols Number of symbols
 * @param buffer Output buffer
 * @param bufferSize Size of output buffer
 */
void buildRMTSymbolLogString(rmt_symbol_word_t* symbols, size_t num_symbols, 
                              char* buffer, size_t bufferSize);

} // namespace ot

#endif // RMT_PARSER_H

