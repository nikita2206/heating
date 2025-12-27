// Standalone test harness for RMT parser
// Accepts RMT symbols as argument, prints result on last line

#include <cstdio>
#include <cstdlib>
#include <cstring>

// Disable ESP_PLATFORM for standalone mode
#undef ESP_PLATFORM

#include "../rmt_encoder.h"

/**
 * Parse simple symbol format: "level0,dur0,level1,dur1;level0,dur0,level1,dur1;..."
 * 
 * Example: "1,520,0,492;0,1002,1,513;..." 
 * Each semicolon separates symbols, each symbol has 4 values (level0,dur0,level1,dur1)
 */
bool parseSymbolFormat(const char* input, rmt_symbol_word_t* symbols, size_t* num_symbols) {
    const char* ptr = input;
    size_t idx = 0;
    
    while (*ptr && idx < 128) {
        // Skip whitespace
        while (*ptr == ' ' || *ptr == '\t' || *ptr == '\n' || *ptr == '\r') ptr++;
        if (*ptr == '\0') break;
        
        // Parse level0
        char* endptr;
        long level0 = strtol(ptr, &endptr, 10);
        if (ptr == endptr || (*endptr != ',')) {
            fprintf(stderr, "Error: Expected level0,duration0 at position %ld\n", (long)(ptr - input));
            return false;
        }
        ptr = endptr + 1; // Skip comma
        
        // Parse duration0
        long dur0 = strtol(ptr, &endptr, 10);
        if (ptr == endptr || (*endptr != ',')) {
            fprintf(stderr, "Error: Expected duration0 at position %ld\n", (long)(ptr - input));
            return false;
        }
        ptr = endptr + 1; // Skip comma
        
        // Parse level1
        long level1 = strtol(ptr, &endptr, 10);
        if (ptr == endptr || (*endptr != ',')) {
            fprintf(stderr, "Error: Expected level1 at position %ld\n", (long)(ptr - input));
            return false;
        }
        ptr = endptr + 1; // Skip comma
        
        // Parse duration1
        long dur1 = strtol(ptr, &endptr, 10);
        if (ptr == endptr) {
            fprintf(stderr, "Error: Expected duration1 at position %ld\n", (long)(ptr - input));
            return false;
        }
        ptr = endptr;
        
        // Store symbol
        symbols[idx].level0 = (uint32_t)level0;
        symbols[idx].duration0 = (uint32_t)dur0;
        symbols[idx].level1 = (uint32_t)level1;
        symbols[idx].duration1 = (uint32_t)dur1;
        idx++;
        
        // Skip semicolon if present
        if (*ptr == ';') ptr++;
    }
    
    *num_symbols = idx;
    return idx > 0;
}

int main(int argc, char* argv[]) {
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <symbol_data>\n", argv[0]);
        fprintf(stderr, "Format: level0,dur0,level1,dur1;level0,dur0,level1,dur1;...\n");
        fprintf(stderr, "Example: 1,520,0,492;0,1002,1,513\n");
        return 1;
    }
    
    // Parse input symbols
    rmt_symbol_word_t symbols[128];
    size_t num_symbols = 0;
    
    if (!parseSymbolFormat(argv[1], symbols, &num_symbols)) {
        fprintf(stderr, "Failed to parse symbol format\n");
        printf("RESULT: 0x00000000\n");
        return 1;
    }
    
    fprintf(stdout, "Parsed %zu RMT symbols\n", num_symbols);
    
    // Build debug log string
    char logBuf[512];
    ot::rmtSymbolsToString(symbols, num_symbols, logBuf, sizeof(logBuf));
    fprintf(stdout, "RMT symbols: %s\n", logBuf);
    
    // Run parser (this will also produce ESP_LOGI/LOGW output via mocks)
    uint32_t result = ot::decodeRmtAsOpenTherm(symbols, num_symbols, false);
    
    // Print result on last line (Python will parse this)
    printf("RESULT: 0x%08x\n", result);
    
    return (result != 0) ? 0 : 1;
}
