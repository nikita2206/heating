
#include <iostream>
#include <cstdint>

constexpr uint64_t STATE_BIT = 1ULL << 63;
constexpr uint64_t DUR_MASK  = ~STATE_BIT;

// Pack: MSB = pin level, lower 63 bits = duration in microseconds
constexpr uint64_t OT_SEG(bool levelHigh, uint64_t us) {
    return (uint64_t(levelHigh) << 63) | uint64_t(us);
}

// Use upToIndex = 65 (elements [0..64] are the captured waveform; the rest is padding)
constexpr int kUpToIndex = 65;

uint64_t interruptTimestamps[69] = {
    OT_SEG(false,  5000), // [00] LOW  for 5000 us
    OT_SEG(true ,   448), // [01] HIGH for 448 us
    OT_SEG(false,   916), // [02] LOW  for 916 us
    OT_SEG(true ,   482), // [03] HIGH for 482 us
    OT_SEG(false,   477), // [04] LOW  for 477 us
    OT_SEG(true ,   455), // [05] HIGH for 455 us
    OT_SEG(false,   446), // [06] LOW  for 446 us
    OT_SEG(true ,   559), // [07] HIGH for 559 us
    OT_SEG(false,   442), // [08] LOW  for 442 us
    OT_SEG(true ,   571), // [09] HIGH for 571 us
    OT_SEG(false,   528), // [10] LOW  for 528 us
    OT_SEG(true ,   428), // [11] HIGH for 428 us
    OT_SEG(false,   427), // [12] LOW  for 427 us
    OT_SEG(true ,   443), // [13] HIGH for 443 us
    OT_SEG(false,   475), // [14] LOW  for 475 us
    OT_SEG(true ,   479), // [15] HIGH for 479 us
    OT_SEG(false,   549), // [16] LOW  for 549 us
    OT_SEG(true ,   574), // [17] HIGH for 574 us
    OT_SEG(false,   426), // [18] LOW  for 426 us
    OT_SEG(true ,   563), // [19] HIGH for 563 us
    OT_SEG(false,   470), // [20] LOW  for 470 us
    OT_SEG(true ,   559), // [21] HIGH for 559 us
    OT_SEG(false,   527), // [22] LOW  for 527 us
    OT_SEG(true ,   476), // [23] HIGH for 476 us
    OT_SEG(false,   534), // [24] LOW  for 534 us
    OT_SEG(true ,   570), // [25] HIGH for 570 us
    OT_SEG(false,   491), // [26] LOW  for 491 us
    OT_SEG(true ,   421), // [27] HIGH for 421 us
    OT_SEG(false,   460), // [28] LOW  for 460 us
    OT_SEG(true ,   528), // [29] HIGH for 528 us
    OT_SEG(false,   507), // [30] LOW  for 507 us
    OT_SEG(true ,   491), // [31] HIGH for 491 us
    OT_SEG(false,   459), // [32] LOW  for 459 us
    OT_SEG(true ,   475), // [33] HIGH for 475 us
    OT_SEG(false,   506), // [34] LOW  for 506 us
    OT_SEG(true ,   446), // [35] HIGH for 446 us
    OT_SEG(false,   443), // [36] LOW  for 443 us
    OT_SEG(true ,   517), // [37] HIGH for 517 us
    OT_SEG(false,   444), // [38] LOW  for 444 us
    OT_SEG(true ,   511), // [39] HIGH for 511 us
    OT_SEG(false,   508), // [40] LOW  for 508 us
    OT_SEG(true ,   574), // [41] HIGH for 574 us
    OT_SEG(false,   487), // [42] LOW  for 487 us
    OT_SEG(true ,   431), // [43] HIGH for 431 us
    OT_SEG(false,   537), // [44] LOW  for 537 us
    OT_SEG(true ,  1008), // [45] HIGH for 1008 us
    OT_SEG(false,   516), // [46] LOW  for 516 us
    OT_SEG(true ,   440), // [47] HIGH for 440 us
    OT_SEG(false,  1056), // [48] LOW  for 1056 us
    OT_SEG(true ,   580), // [49] HIGH for 580 us
    OT_SEG(false,   578), // [50] LOW  for 578 us
    OT_SEG(true ,   512), // [51] HIGH for 512 us
    OT_SEG(false,   567), // [52] LOW  for 567 us
    OT_SEG(true ,   469), // [53] HIGH for 469 us
    OT_SEG(false,   437), // [54] LOW  for 437 us
    OT_SEG(true ,   431), // [55] HIGH for 431 us
    OT_SEG(false,   478), // [56] LOW  for 478 us
    OT_SEG(true ,   494), // [57] HIGH for 494 us
    OT_SEG(false,   440), // [58] LOW  for 440 us
    OT_SEG(true ,   479), // [59] HIGH for 479 us
    OT_SEG(false,   445), // [60] LOW  for 445 us
    OT_SEG(true ,   517), // [61] HIGH for 517 us
    OT_SEG(false,   491), // [62] LOW  for 491 us
    OT_SEG(true ,  1049), // [63] HIGH for 1049 us
    OT_SEG(false,   461), // [64] LOW  for 461 us
    OT_SEG(false,  7000), // [65] padding
    OT_SEG(false,  7000), // [66] padding
    OT_SEG(false,  7000), // [67] padding
    OT_SEG(false,  7000), // [68] padding
};

// Your call would look like:
// uint32_t frame = parseInterrupts(interruptTimestamps, kUpToIndex);
// Expected frame: 0b00000000000000000000001100000000


static inline bool seg_level(uint64_t packed) {
    return (packed & STATE_BIT) != 0;               // MSB is level
}
static inline uint32_t seg_dur_us(uint64_t packed) {
    return static_cast<uint32_t>(packed & DUR_MASK); // low 63 bits
}

static inline int popcount32(uint32_t v) {
#if defined(__GNUC__)
    return __builtin_popcount(v);
#else
    int c = 0;
    while (v) { c += (v & 1u); v >>= 1; }
    return c;
#endif
}

static uint32_t parseInterrupts(uint64_t interrupts[69], int upToIndex)
{
    // Need at least: idle + a bunch of segments
    if (upToIndex < 4) return 0;

    // Expect first entry to be the long idle period before the start bit
    // We'll just skip index 0 unconditionally.
    int i = 1;

    // Manchester half-bit is ~500us (bit is ~1000us).
    constexpr int HALF_US_NOM = 500;

    // Loose tolerances: classify segment as 1 half-bit (~500us) or 2 half-bits (~1000us).
    constexpr int ONE_MIN = 300;
    constexpr int ONE_MAX = 700;
    constexpr int TWO_MIN = 700;
    constexpr int TWO_MAX = 1300;

    // We want start(1) + 32 frame + stop(1) = 34 bits => 69 half-bits.
    bool halfLevels[69];
    int halfCount = 0;

    while (i < upToIndex && halfCount < 69) {
        const bool level = seg_level(interrupts[i]);
        const uint32_t dur = seg_dur_us(interrupts[i]);

        int halves = 0;
        if (dur >= ONE_MIN && dur < ONE_MAX) halves = 1;
        else if (dur >= TWO_MIN && dur <= TWO_MAX) halves = 2;
        else {
            // Allow a final trailing idle to be long; stop if we already have enough
            // or treat as invalid if it appears mid-frame.
            return 0;
        }

        for (int k = 0; k < halves && halfCount < 69; ++k) {
            halfLevels[halfCount++] = level;
        }
        ++i;
    }

    if (halfCount != 68) return 0;

    // Decode 34 Manchester bits from pairs of half-levels.
    // With your described start: idle low -> high then low indicates first bit=1,
    // we assume: active=HIGH, idle=LOW, and:
    //   bit '1' = active-to-idle = HIGH->LOW
    //   bit '0' = idle-to-active = LOW->HIGH :contentReference[oaicite:2]{index=2}
    uint8_t bits[34];
    for (int b = 0; b < 34; ++b) {
        const bool a = halfLevels[2 * b];
        const bool c = halfLevels[2 * b + 1];
        if (a == c) return 0; // invalid Manchester (no mid-bit transition)

        if (a == 1 && c == 0) bits[b] = 1;
        else if (a == 0 && c == 1) bits[b] = 0;
        else return 0;
    }

    // Start/stop bits must be '1' (outside the 32-bit frame). :contentReference[oaicite:3]{index=3}
    if (bits[0] != 1) return 0;
    if (bits[33] != 1) return 0;

    // Build the 32-bit frame from bits[1..32], MSB first.
    uint32_t frame = 0;
    for (int b = 1; b <= 32; ++b) {
        frame = (frame << 1) | (bits[b] & 1u);
    }

    // Parity: total number of '1' bits in entire 32 bits must be even. :contentReference[oaicite:4]{index=4}
    if ((popcount32(frame) & 1) != 0) return 0;

    return frame;
}

static void logU32Bin(uint32_t v)
{
    char buf[33];
    for (int i = 31; i >= 0; --i) {
        buf[31 - i] = (v & (1u << i)) ? '1' : '0';
    }
    buf[32] = '\0';
    printf("%s", buf);
}

int main()
{
    std::cout<<"Hello World";

    uint32_t frame = parseInterrupts(interruptTimestamps, kUpToIndex);
    
    logU32Bin(frame);
    
    return 0;
}