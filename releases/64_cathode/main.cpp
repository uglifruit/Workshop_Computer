// cathode_ray — PAL composite video synthesizer for Workshop Computer
// (1-bit image rendered through a 2-bit resistor DAC on Pulse Out 1 + 2)
//
// Core 0: ComputerCard framework (ProcessSample ISR @ 48 kHz).
//         Reads all Eurorack I/O, writes to volatile shared state.
// Core 1: Dedicated video loop. Owns PIO0/SM0 + DMA. Runs at full speed.
//         Builds frame word-stream, updates framebuffer during vblank.
//
// Video output: 2-bit resistor DAC on GPIO 8 (Pulse Out 1) + GPIO 9 (Pulse Out 2).
//   Pu1 ──[1k ]──┐
//                ├── RCA centre ── composite in (TV internal 75Ω to GND)
//   Pu2 ──[470R]──┘
//   GND ───────────── RCA shell
// 4 summed levels (3 used): sync 0V / black ~0.3V / white ~1.0V. Pulse Out 2 is
// now consumed by video — it is no longer a usable normal pulse output.
//
// PAL timing (111 MHz sys clock, SM @ 111/16 = 6.93750 MHz = teletext bit rate exact):
//   1 pixel = 1 SM cycle = 16 sys-cycles = 144.144 ns (1 teletext bit = 1 pixel)
//   Integer clkdiv = no jitter (essential for a teletext decoder to lock).
//   Line:    444 pixels = 64.000 µs.  Interlaced: 625 lines/frame, 50 fields/s.

#include "ComputerCard.h"
#include "pico/multicore.h"
#include "hardware/pio.h"
#include "hardware/dma.h"
#include "hardware/irq.h"
#include "hardware/sync.h"
#include "composite.pio.h"
#include <cstring>
#include "ttx_page.h"     // generated teletext page data (tools/tti2h.py)

// ─── TEST MODE ────────────────────────────────────────────────────────────────
// 1 = static hardware test-pattern rig (four quadrants of geometric patterns,
//     Main knob tunes "on" pulse width) to characterise the DAC's analog edge
//     behaviour. Bypasses all picture/teletext/input code and dither/dilation.
// 0 = normal Cathode Ray firmware.
#define TEST_PATTERN        0

// ─── Hardware pin macros ──────────────────────────────────────────────────────
#define VIDEO_GPIO          8       // PULSE_1_RAW_OUT — video DAC bit 0 (Pu1).
                                    // GPIO 9 (PULSE_2_RAW_OUT) = video DAC bit 1 (Pu2).
                                    // PIO drives both via 'out pins,2' (consecutive 8,9).
#define PULSE_IN_1_GPIO     2       // PULSE_1_INPUT   — rising edge = clear frame
#define PULSE_IN_2_GPIO     3       // PULSE_2_INPUT   — high = invert video output

// ─── Framebuffer ─────────────────────────────────────────────────────────────
#define FB_WIDTH            360     // pixels per row (360 × 144ns = 51.84µs active)
#define FB_HEIGHT           256     // rows (fills most of visible PAL height)
#define FB_STRIDE           45      // bytes per row (360/8 = 45, exact)
#define FB_SIZE             11520   // total bytes (45 × 256)
#define PIXEL_STRETCH       1       // each pixel = 1 clock tick (~144ns)

// Pixel set/clear helpers. Row r, column c (0-based, left=MSB).
#define FB_SET(buf, r, c)   ((buf)[(r)*FB_STRIDE + (c)/8] |=  (0x80u >> ((c)&7)))
#define FB_CLEAR(buf, r, c) ((buf)[(r)*FB_STRIDE + (c)/8] &= ~(0x80u >> ((c)&7)))

// ─── Greyscale working buffer ────────────────────────────────────────────────
// All drawing happens in a half-resolution grey buffer where each cell holds a
// brightness 0..GREY_LEVELS-1. Each frame it is expanded into the 1-bit
// frame_buffer via a GREY_SCALE×GREY_SCALE spatial dither, giving fake greyscale
// on a 1-bit display (ZX-Spectrum-style). The scan-out path (build_frame_words)
// is unchanged — it still reads frame_buffer bit by bit.
//
// GREY_SCALE is the downscale factor. It MUST divide both FB_WIDTH and FB_HEIGHT.
// Legal values (common divisors of 360 and 256): 1, 2, 4, 8. Start at 2 (180×128);
// raise toward 1 (full res) later only after checking RAM (grey buffer = GREY_SIZE).
#define GREY_SCALE   2
#define GREY_W       (FB_WIDTH  / GREY_SCALE)   // 180 at scale 2
#define GREY_H       (FB_HEIGHT / GREY_SCALE)   // 128 at scale 2
#define GREY_SIZE    (GREY_W * GREY_H)          // 23040 bytes at scale 2
#define GREY_LEVELS  3                          // 0=black, 1=checker grey, 2=white
static_assert(FB_WIDTH  % GREY_SCALE == 0, "GREY_SCALE must divide FB_WIDTH");
static_assert(FB_HEIGHT % GREY_SCALE == 0, "GREY_SCALE must divide FB_HEIGHT");

#define GREY_SET(buf, r, c, lvl) ((buf)[(r)*GREY_W + (c)] = (uint8_t)(lvl))

// ─── PAL line timing (6.9375MHz clock = teletext bit rate, 144.144ns/tick) ───
// Pixel clock = teletext's 6.9375MHz EXACT via 111MHz sys / integer clkdiv 16, so
// 1 teletext bit = 1 pixel and there is zero divider jitter (decoder PLL locks).
// At this rate a 64µs line = exactly 444 px (teletext = 444 × line freq).
// Line = 64µs = 444 ticks. Segments must sum to exactly 444.
//   fp=11 + hs=33 + bp=40 + av=360 = 444 ✓
//   fp  = 1.65µs target → 11 ticks = 1.586µs
//   hs  = 4.7µs  target → 33 ticks = 4.757µs
//   bp  = 5.7µs  target → 40 ticks = 5.766µs
//   av  = 51.95µs target → 360 ticks = 51.895µs  (FB_WIDTH=360, no padding)
#define LINE_FP_PX          11      // front porch
#define LINE_HS_PX          33      // h-sync
#define LINE_BP_PX          40      // back porch
#define LINE_AV_PX          360     // active video (= FB_WIDTH, exact)
#define LINE_TOTAL_PX       444     // 11+33+40+360 = 444 ✓

// Frame structure: INTERLACED PAL (625 lines = 2 fields of 312.5 lines).
// CANONICAL vertical sync: each field = 5 pre-eq + 5 broad + 5 post-eq half-line
// pulses (15 half-lines = 7.5 lines), IDENTICAL for both fields. Each field totals
// 625 half-lines (= 312.5 lines) — an ODD number of half-lines, so field 2's line
// grid lands automatically offset by half a line from field 1. THAT half-line
// offset IS the interlace and gives a decoder valid field identification.
//   field = 15 half (vsync) + 305 full lines (610 half) = 625 half = 312.5 lines
//   305 content lines = 16 teletext/top-blank + 256 active + 33 bottom-blank
// Teletext sits on the first content lines (≈ PAL lines 8-23, inside the 7-22 window).
#define PAL_BLANK_TOP       16                 // VBI blank lines (carry teletext)
#define PAL_ACTIVE_LINES    FB_HEIGHT          // 256
#define PAL_BLANK_BOT       33                 // 16+256+33 = 305 content lines/field

// Vsync pulse widths (per half-line of 222px @144.15ns):
#define HALF_LINE_PX        222                // 32µs
#define EQ_LOW_PX           14                 // 2µs pre/post equalizing low
#define BROAD_LOW_PX        208                // 30µs broad sync low (222−14)
// Canonical PAL: 5 + 5 + 5 = 15 half-lines per field (same both fields).
#define VS_PRE_EQ  5
#define VS_BROAD   5
#define VS_POST_EQ 5

// ─── Teletext VBI insertion ──────────────────────────────────────────────────
// Teletext data must start ~12.0µs after the line sync reference. At 144.144ns/px,
// 12µs ≈ 83 px from line start. 360 data bits = 360 px → ends at 83+360 = 443 of
// 444. Bit clock = pixel clock = 6.93750MHz (integer clkdiv) = teletext rate exactly.
#define TTX_START_PX        83       // px from line start to first teletext bit (~12µs)
#define TTX_DATA_BITS       360      // 45 bytes × 8 (ends at 83+360=443 of 444)
// Which VBI lines carry teletext, and which page row each sends. We place them in
// the top blank region (after vsync). Field-1 teletext lives ~lines 7..22; here we
// map a contiguous block of blank lines to page rows 0..N.
#define TTX_FIRST_BLANK_LINE 0       // index into PAL_BLANK_TOP to start (PAL line ~9)
#define TTX_NUM_LINES        16      // how many VBI lines to fill with teletext rows

// Teletext debug view (switch DOWN, was snow): render the actual teletext packets
// onto VISIBLE scanlines so the run-in/framing/data show as pixels. Each page row
// is drawn TTX_DEBUG_ZOOM scanlines tall. Crisp even stripes at the far left (the
// 0x55 run-in) = good data eye; smeared/grey = the 144ns slew problem made visible.
#define TTX_DEBUG_ZOOM      8        // scanlines per page row in the debug view

// ─── DMA word stream ─────────────────────────────────────────────────────────
// 2 bits/pixel, INTERLACED frame = 625 lines.
//   field1 = 16 half-lines vsync + (26+256+23)*444 = 16*222 + 305*444 = 138972 px
//   field2 = 14 half-lines vsync + (26+256+23)*444 = 14*222 + 305*444 = 138528 px
//   frame  = 277500 px × 2 bits = 555000 bits → ceil/32 = 17344 words.
// word_buf[2][17344] = 138752 B (~136KB). Total RAM ~70%.
#define FRAME_WORDS         17344

// Double-buffered word streams: Core 1 writes to back buffer, DMA reads from front.
static uint32_t __attribute__((aligned(4))) word_buf[2][FRAME_WORDS];
static volatile int active_buf = 0;  // which buffer DMA is currently reading

// ─── Framebuffer (written by Core 1 during vblank) ───────────────────────────
static uint8_t frame_buffer[FB_SIZE];

// ─── Grey working buffer (Core-1-private; expanded into frame_buffer each frame)
static uint8_t grey_buffer[GREY_SIZE];

// ─── Etch CV ring buffer (Core 0 pushes @48kHz, Core 1 drains each frame) ─────
// Captures sub-frame CV motion: each frame Core 1 plots every sample Core 0 queued.
// Power-of-two size for cheap masking. 512 samples ≈ 10.7ms of motion at 48kHz.
#define ETCH_RING_SIZE   512
#define ETCH_RING_MASK   (ETCH_RING_SIZE - 1)
static volatile int16_t etch_x_ring[ETCH_RING_SIZE];
static volatile int16_t etch_y_ring[ETCH_RING_SIZE];
static volatile uint32_t etch_write_idx = 0;  // Core 0 increments after each push

// ─── Shared state (Core 0 ISR → Core 1 video loop) ───────────────────────────
struct SharedState {
    volatile int32_t  audio_y;       // AudioIn1() raw: -2048..2047
    volatile int32_t  cv_x;          // CVIn1()   raw: -2048..2047
    volatile int32_t  cv_y;          // CVIn2()   raw: -2048..2047
    volatile int32_t  knob_x_scale;  // KnobVal(X): 0..4095
    volatile int32_t  knob_y_scale;  // KnobVal(Y): 0..4095
    volatile uint8_t  mode;          // 0=oscilloscope, 1=etch-a-sketch
    volatile bool     pulse_clear;   // set on Pulse In 1 rising edge
    volatile bool     pulse_invert;  // true while Pulse In 2 is HIGH
    volatile uint8_t  sw_position;   // 0=UP(fade) 1=MID(static) 2=DOWN(snow)
    volatile int32_t  knob_main;     // KnobVal(Main): 0..4095 (test mode: on-width)
};
static SharedState shared;

// ─── DMA channel ─────────────────────────────────────────────────────────────
static int dma_chan = -1;
static volatile bool vblank_ready = false;  // set by DMA IRQ at frame end

// ─── LCG random (Core 1 only, no locking needed) ─────────────────────────────
static uint32_t lcg_state = 0xDEADBEEF;
static inline uint32_t lcg_rand() {
    lcg_state = lcg_state * 1664525u + 1013904223u;
    return lcg_state;
}

// ─────────────────────────────────────────────────────────────────────────────
// Word-stream builder
// Packs the PAL frame (sync lines + active video lines) into word_buf[back].
// The invert flag is applied here: active pixel bytes are XOR'd with 0xFF.
// ─────────────────────────────────────────────────────────────────────────────

// ─── Output levels (2-bit resistor DAC) ──────────────────────────────────────
// Pulse outputs are INVERTED and idle high (~6V); driven = ~0.3V. Two pins (Pu1=GPIO8,
// Pu2=GPIO9) sum through weighted resistors into the RCA centre pin → 4 levels.
//
// level_pair[] is the ONE place polarity + resistor weighting lives. Each entry is a
// 2-bit field: bit0 → GPIO8/Pu1, bit1 → GPIO9/Pu2. PIO bit 1 → jack LOW, bit 0 → jack HIGH.
// CRITICAL: BLACK must sit ABOVE sync (0V) but below white. The TV's sync separator
// slices at a threshold BETWEEN black and sync — if BLACK == SYNC the TV loses sync
// and shows no picture. So BLACK needs a small non-zero pedestal.
//   SYNC  = 0b11 → both pins LOW  → 0V (sync tip)
//   BLACK = 0b10 → Pu1 HIGH via 1kΩ only → smallest lift above sync = black pedestal
//   WHITE = 0b00 → both pins HIGH → brightest
// (0b01 = Pu2 via 470Ω = a BRIGHTER grey — was the old too-light "black".)
// Retune the DAC by editing ONLY this table. If the scope shows GPIO8/9 swapped,
// swap the two bits within each entry — never touch the packing loop.
enum Level { SYNC = 0, BLACK = 1, WHITE = 2 };
//                                  bit1=Pu2(GPIO9), bit0=Pu1(GPIO8)
static const uint8_t level_pair[3] = {
    /*SYNC */ 0b11,   // both jack LOW  → 0V
    /*BLACK*/ 0b10,   // Pu1 HIGH via 1kΩ only → small pedestal above sync
    /*WHITE*/ 0b00,   // both jack HIGH → brightest
};

// ─────────────────────────────────────────────────────────────────────────────
// Teletext (World System Teletext) VBI encoder
//
// A data line = 45 bytes = 360 bits, transmitted LSB-first at 6.9375 MHz (our pixel
// clock is set to exactly this, so 1 teletext bit = 1 pixel). Structure:
//   [0]   0x55  clock run-in   (1010...)
//   [1]   0x55  clock run-in
//   [2]   0x27  framing code   (transmission-order 11100100)
//   [3]   Hamming 8/4: magazine (b1..b3) + row bit b4   (low nibble of addr)
//   [4]   Hamming 8/4: row bits b5..b8 ... (high nibble of addr)
//   [5..] for row 0 (header): 8 Hamming bytes (page units/tens, subcode, control),
//         then 32 odd-parity character bytes.
//         for rows 1..24: 40 odd-parity character bytes.
// We build each line into a 45-byte buffer (true byte values), then the scan-out
// packs it LSB-first into pixels (bit=1 → WHITE, bit=0 → BLACK).
// ─────────────────────────────────────────────────────────────────────────────

// Hamming 8/4 encode table: 4 data bits → 8-bit protected byte (transmission order).
// Standard WST table (ETS 300 706 Table 8). Index = 4-bit data value 0..15.
static const uint8_t ttx_hamming[16] = {
    0x15, 0x02, 0x49, 0x5E, 0x64, 0x73, 0x38, 0x2F,
    0xD0, 0xC7, 0x8C, 0x9B, 0xA1, 0xB6, 0xFD, 0xEA,
};

// Odd parity: set b8 (0x80) so the total number of set bits is odd.
static inline uint8_t ttx_parity(uint8_t v) {
    v &= 0x7F;
    uint8_t p = v;
    p ^= p >> 4; p ^= p >> 2; p ^= p >> 1;   // p&1 = parity of the 7 bits
    return (p & 1) ? v : (uint8_t)(v | 0x80); // odd → already odd; even → set b8
}

// Build a 45-byte teletext packet for a display row (1..24) into out[45].
static void ttx_build_row(uint8_t *out, int magazine, int row, const uint8_t *data40) {
    out[0] = 0x55; out[1] = 0x55; out[2] = 0x27;   // run-in + framing
    // Address: 5-bit (mag low 3 bits + row low 2 bits) in first Hamming byte's data,
    // remaining row bits in the second. Packing per WST: addr = (row<<3)|magazine,
    // split into two 4-bit Hamming-coded nibbles, low nibble first.
    int addr = ((row & 0x1F) << 3) | (magazine & 0x07);
    out[3] = ttx_hamming[addr & 0x0F];
    out[4] = ttx_hamming[(addr >> 4) & 0x0F];
    for (int i = 0; i < 40; i++) out[5 + i] = ttx_parity(data40[i]);
}

// Build the page-header packet (row 0) into out[45]. Page units/tens are Hamming
// coded; control bits + subcode follow; then 32 character bytes (cols 8..39).
static void ttx_build_header(uint8_t *out, int magazine, int page,
                             const uint8_t *data40) {
    out[0] = 0x55; out[1] = 0x55; out[2] = 0x27;
    int addr = (0 << 3) | (magazine & 0x07);       // row 0
    out[3] = ttx_hamming[addr & 0x0F];
    out[4] = ttx_hamming[(addr >> 4) & 0x0F];
    // Page units/tens + subcode S1-S4 + control bits C4-C14, all Hamming 8/4 coded.
    // Matches vbit2 Packet::Header() exactly. subcode=0, control=0 → a clean valid
    // header (all-zero subcode/control bytes = ttx_hamming[0] = 0x15).
    int units = page & 0x0F;
    int tens  = (page >> 4) & 0x0F;
    out[5]  = ttx_hamming[units];                  // page units
    out[6]  = ttx_hamming[tens];                   // page tens
    out[7]  = ttx_hamming[0x00];                   // S1 (subcode bits 0-3)
    out[8]  = ttx_hamming[0x00];                   // S2 (3 bits) + C4 (erase)
    out[9]  = ttx_hamming[0x00];                   // S3 (subcode bits)
    out[10] = ttx_hamming[0x00];                   // S4 (2 bits) + C5,C6
    out[11] = ttx_hamming[0x00];                   // C7-C10
    out[12] = ttx_hamming[0x00];                   // C11-C14 (C11=0 → parallel mode)
    // Columns 8..39 of the header row carry display chars (cols 0..7 = page clock).
    for (int i = 8; i < 40; i++) out[5 + i] = ttx_parity(data40[i]);
}

// Pre-encoded packets for the whole page (built once at init). 45 bytes/row.
static uint8_t ttx_packets[TTX_ROWS][45];

static void ttx_encode_page() {
    ttx_build_header(ttx_packets[0], TTX_MAGAZINE, TTX_PAGE, ttx_page[0]);
    for (int r = 1; r < TTX_ROWS; r++) {
        ttx_build_row(ttx_packets[r], TTX_MAGAZINE, r, ttx_page[r]);
    }
}

static void build_frame_words(int back, bool invert) {
    uint32_t *buf = word_buf[back];
    int word_idx = 0;
    uint32_t cur_word = 0;
    int bit_pos = 0;  // counts BITS (advances by 2 per pixel); word commits at 32

    auto commit_word = [&]() {
        buf[word_idx++] = cur_word;
        cur_word = 0;
        bit_pos = 0;
    };

    // emit_const: emit `count` PIXELS all at the given level (2 bits each).
    // 32-bit threshold / 2 = 16 pixels per word; pairs never straddle a word boundary.
    auto emit_const = [&](Level lvl, int count) {
        uint32_t pair = level_pair[lvl] & 0x3;
        // 16×-replicated full word for the fast path
        uint32_t fill = pair;
        for (int i = 1; i < 16; i++) fill = (fill << 2) | pair;
        while (bit_pos > 0 && count > 0) {
            cur_word = (cur_word << 2) | pair;
            bit_pos += 2;
            count--;
            if (bit_pos == 32) commit_word();
        }
        while (count >= 16) {
            buf[word_idx++] = fill;
            count -= 16;
        }
        while (count > 0) {
            cur_word = (cur_word << 2) | pair;
            bit_pos += 2;
            count--;
            if (bit_pos == 32) commit_word();
        }
    };

    // Blank line: fp=black, hs=sync, rest=black (blanking pedestal sits at black)
    auto emit_blank_line = [&]() {
        emit_const(BLACK, LINE_FP_PX);
        emit_const(SYNC,  LINE_HS_PX);
        emit_const(BLACK, LINE_BP_PX + LINE_AV_PX);
    };

    // Half-line vsync pulses (each 224px = 32µs):
    //   equalizing: 2µs low + 30µs high   (short narrow pulse)
    //   broad sync: 30µs low + 2µs high   (wide pulse, serrated)
    auto emit_eq_half    = [&]() { emit_const(SYNC, EQ_LOW_PX);    emit_const(BLACK, HALF_LINE_PX - EQ_LOW_PX); };
    auto emit_broad_half = [&]() { emit_const(SYNC, BROAD_LOW_PX); emit_const(BLACK, HALF_LINE_PX - BROAD_LOW_PX); };

    // Teletext VBI line: normal sync, then 360 data bits starting at TTX_START_PX.
    // Each teletext bit → 1 pixel: bit=1 → WHITE, bit=0 → BLACK. Bytes are sent
    // LSB-first (b1 first) per the WST spec. packet = 45 bytes already encoded.
    // DATA BITS ARE SACROSANCT: emitted 1:1, NO dither and NO white-dilation (writes
    // word_buf directly, bypassing frame_buffer). If a lone WHITE bit can't slew to
    // full white in 144ns through the resistor DAC, that's an analog/level issue to
    // fix in hardware — never by widening or altering the bit pattern.
    auto emit_ttx_line = [&](const uint8_t *packet) {
        // Sync + lead-in: front porch black, h-sync, then black up to data start.
        emit_const(BLACK, LINE_FP_PX);
        emit_const(SYNC,  LINE_HS_PX);
        emit_const(BLACK, TTX_START_PX - LINE_FP_PX - LINE_HS_PX);
        // 360 data bits, LSB-first within each byte.
        for (int byte = 0; byte < 45; byte++) {
            uint8_t b = packet[byte];
            for (int bit = 0; bit < 8; bit++) {
                uint32_t pair = (b & 1u) ? level_pair[WHITE] : level_pair[BLACK];
                cur_word = (cur_word << 2) | (pair & 0x3);
                bit_pos += 2;
                if (bit_pos == 32) commit_word();
                b >>= 1;
            }
        }
        // Pad remainder of the line to LINE_TOTAL_PX with black.
        emit_const(BLACK, LINE_TOTAL_PX - TTX_START_PX - TTX_DATA_BITS);
    };

    // One active picture line from the framebuffer.
    auto emit_active_line = [&](int row) {
        emit_const(BLACK, LINE_FP_PX);   // front porch (black)
        emit_const(SYNC,  LINE_HS_PX);   // h-sync
        emit_const(BLACK, LINE_BP_PX);   // back porch (black)
        const uint8_t *fb_row = &frame_buffer[row * FB_STRIDE];
        for (int p = 0; p < FB_WIDTH; p++) {
            bool set = (fb_row[p / 8] >> (7 - (p & 7))) & 1u;
            if (invert) set = !set;
            uint32_t pair = level_pair[set ? WHITE : BLACK] & 0x3;
            cur_word = (cur_word << 2) | pair;
            bit_pos += 2;
            if (bit_pos == 32) commit_word();
        }
        emit_const(BLACK, LINE_AV_PX - FB_WIDTH);  // right-pad (0 px at FB_WIDTH=360)
    };

    // Build one field: canonical 5-5-5 half-line vsync + top-blank (carrying
    // teletext) + active picture + bottom-blank. Both fields IDENTICAL; each is
    // 625 half-lines (odd) so field 2 lands half a line offset → interlace.
    // Teletext goes on BOTH fields (matches raspi-teletext).
    auto build_field = [&]() {
        for (int i = 0; i < VS_PRE_EQ;  i++) emit_eq_half();
        for (int i = 0; i < VS_BROAD;   i++) emit_broad_half();
        for (int i = 0; i < VS_POST_EQ; i++) emit_eq_half();
        for (int l = 0; l < PAL_BLANK_TOP; l++) {
            int ttx_row = l - TTX_FIRST_BLANK_LINE;
            if (ttx_row >= 0 && ttx_row < TTX_NUM_LINES && ttx_row < TTX_ROWS)
                emit_ttx_line(ttx_packets[ttx_row]);
            else
                emit_blank_line();
        }
        // Switch DOWN = teletext debug view: render the actual teletext packets onto
        // visible scanlines (same faithful bitstream the VBI uses, no dither/dilation)
        // so the run-in/framing/data can be seen and judged. Each page row is drawn
        // TTX_DEBUG_ZOOM scanlines tall so it's easy to see. Lets us diagnose without
        // a capture stick: crisp even left-edge stripes (run-in) = good data eye.
        bool ttx_debug = (shared.sw_position == 2);
        for (int row = 0; row < PAL_ACTIVE_LINES; row++) {
            if (ttx_debug) {
                int drow = row / TTX_DEBUG_ZOOM;       // which page row this line shows
                if (drow < TTX_ROWS) { emit_ttx_line(ttx_packets[drow]); continue; }
            }
            emit_active_line(row);
        }
        for (int l = 0; l < PAL_BLANK_BOT; l++) emit_blank_line();
    };

    // Two identical fields. Each = 15 half (vsync) + 305 full lines = 625 half-lines
    // = 312.5 lines. The odd half-line count offsets field 2 → proper interlace.
    build_field();
    build_field();

    // Flush remaining partial word, padding LSBs with BLACK pairs
    if (bit_pos > 0) {
        uint32_t blk = level_pair[BLACK] & 0x3;
        while (bit_pos < 32) {
            cur_word = (cur_word << 2) | blk;
            bit_pos += 2;
        }
        commit_word();
    }

    // Fill any remaining words with the 16×BLACK blanking-level constant
    uint32_t black_fill = level_pair[BLACK] & 0x3;
    for (int i = 1; i < 16; i++) black_fill = (black_fill << 2) | (level_pair[BLACK] & 0x3);
    while (word_idx < FRAME_WORDS) {
        buf[word_idx++] = black_fill;
    }
}

#if TEST_PATTERN
// ─────────────────────────────────────────────────────────────────────────────
// Hardware test-pattern rig. Writes geometric patterns DIRECTLY into frame_buffer
// (raw bits — no grey buffer, no dither, no dilation) so we see the true analog
// edge behaviour of the DAC. Four quadrants of 180×128:
//   TL: 1px vertical lines    TR: 1px horizontal lines
//   BL: 1×1 checkerboard      BR: width ramp (1,2,3,4,...px runs)
// Main knob → ON_STRETCH (0..8 px): each "on" pixel extended rightward, live, so we
// can watch when thin white snaps to full white. (Horizontal lines are the baseline,
// unaffected by horizontal stretch.)
// ─────────────────────────────────────────────────────────────────────────────
static void __not_in_flash_func(draw_test_pattern)() {
    const int HW = FB_WIDTH / 2;    // 180  quadrant width
    const int HH = FB_HEIGHT / 2;   // 128  quadrant height
    int stretch = (int)(shared.knob_main * 8 / 4095);   // 0..8 extra px per "on"

    memset(frame_buffer, 0, FB_SIZE);

    // Helper: set pixel (x,y) plus `stretch` pixels to its right (clipped).
    auto put = [&](int x, int y) {
        for (int k = 0; k <= stretch; k++) {
            int xx = x + k;
            if (xx >= 0 && xx < FB_WIDTH && y >= 0 && y < FB_HEIGHT)
                FB_SET(frame_buffer, y, xx);
        }
    };

    for (int y = 0; y < FB_HEIGHT; y++) {
        for (int x = 0; x < FB_WIDTH; x++) {
            bool left = (x < HW), top = (y < HH);
            bool on = false;
            if (top && left) {
                on = (x & 1) == 0;                       // TL: vertical 1px lines
            } else if (top && !left) {
                on = (y & 1) == 0;                       // TR: horizontal 1px lines
            } else if (!top && left) {
                on = ((x ^ y) & 1) == 0;                 // BL: 1×1 checkerboard
            } else {
                // BR: width ramp. Pattern period = run(w) on + w off, w cycling 1..8.
                int lx = x - HW;
                int w = 1, pos = lx;
                while (pos >= 2 * w) { pos -= 2 * w; w++; if (w > 8) w = 1; }
                on = (pos < w);                          // w px on, then w px off
            }
            if (on) put(x, y);   // (stretch handled in put)
        }
    }
    // NOTE: put() applies stretch by OR-ing pixels right; for the base patterns above
    // we only call put at the pattern's "on" origin columns, so stretch widens runs.
}
#endif

// ─────────────────────────────────────────────────────────────────────────────
// Framebuffer drawing
// Called by Core 1 during vblank. Reads from shared, writes to frame_buffer[].
// ─────────────────────────────────────────────────────────────────────────────

static int scope_x = 0;          // oscilloscope sweep X counter (grey-X, Core 1 private)
static uint32_t etch_read_idx = 0; // Core 1's drain position in the etch CV ring
static int etch_prev_x = 0, etch_prev_y = 0; // last etch point (for line interpolation)
static bool etch_have_prev = false;          // false until first etch sample drawn
#define ETCH_DEADBAND 1   // grey-cell jitter band: ignore moves within ±this of anchor

// ─── Phosphor fade (switch UP) ───────────────────────────────────────────────
// Each grey cell's brightness is decremented toward 0 (true black). With 3 levels
// this is only 2 visible steps, so apply it every FADE_EVERY_N frames to stretch
// the lifetime. lifetime ≈ (GREY_LEVELS-1) × FADE_EVERY_N × 20ms.
//   FADE_EVERY_N = 60 → 2 × 60 × 20ms = 2.4s
#define FADE_EVERY_N 60
static uint32_t fade_div = 0;    // frame counter for the fade divider

// ─── 2×2 dither patterns (GREY_SCALE=2) ──────────────────────────────────────
// Per grey level, the GREY_SCALE-wide bit field for each sub-row (MSB=left).
//   L0 = 00/00 (black)   L1 = 01/10 (checker grey)   L2 = 11/11 (white)
// dither[level][sub_row] — only the GREY_SCALE low bits are used.
static const uint8_t dither[GREY_LEVELS][2] = {
    /*L0*/ {0b00, 0b00},
    /*L1*/ {0b01, 0b10},
    /*L2*/ {0b11, 0b11},
};

// Right-dilate white by WHITE_DILATE pixels: every white pixel also forces the next
// WHITE_DILATE pixels to its RIGHT white. Exploits the composite DAC's rising-edge
// slew — the node only reaches full white when several adjacent pixels are white, so a
// lone white pixel reads grey. Dilating guarantees every white feature is ≥(1+N)px in
// scan order → renders full white. MSB = leftmost pixel, so "right" = toward the LSB
// (right shift). We track a rolling history of the last 8 emitted bits so the dilate
// crosses byte boundaries for any N up to 7.
#define WHITE_DILATE 4   // measured: an isolated white px needs ~5px (1+4) to reach true white
static inline void dilate_white_right(uint8_t *fb) {
    // 'spill' holds the WHITE_DILATE rightmost source bits of the previous byte,
    // left-aligned into the top bits, ready to flow into this byte's MSBs.
    uint16_t prev = 0;   // previous source byte (for cross-byte carry)
    for (int b = 0; b < FB_STRIDE; b++) {
        uint8_t v = fb[b];
        // Build a 16-bit window: [prev_byte][this_byte], MSB-first. OR in right-shifts
        // 1..N of the window, then take this byte's 8 bits.
        uint16_t win = (uint16_t)((prev << 8) | v);
        uint16_t out = win;
        for (int s = 1; s <= WHITE_DILATE; s++) out |= (win >> s);
        fb[b] = (uint8_t)(out & 0xFF);
        prev = v;
    }
}

// Expand the grey buffer into the 1-bit frame_buffer using the dither. Driven by
// output byte: each frame_buffer byte = 8 horizontal pixels = (8/GREY_SCALE) grey
// cells on one grey row. Must run before build_frame_words() each frame.
static void __not_in_flash_func(expand_grey_to_fb)() {
    const int cells_per_byte = 8 / GREY_SCALE;       // 4 at scale 2
    for (int gy = 0; gy < GREY_H; gy++) {
        const uint8_t *grow = &grey_buffer[gy * GREY_W];
        for (int sub = 0; sub < GREY_SCALE; sub++) {
            int fb_row = gy * GREY_SCALE + sub;
            uint8_t *fb = &frame_buffer[fb_row * FB_STRIDE];
            int cell = 0;
            for (int b = 0; b < FB_STRIDE; b++) {
                uint8_t byte = 0;
                for (int k = 0; k < cells_per_byte; k++) {
                    byte = (uint8_t)((byte << GREY_SCALE) | dither[grow[cell++]][sub]);
                }
                fb[b] = byte;
            }
#if WHITE_DILATE
            dilate_white_right(fb);
#endif
        }
    }
}

// Plot a dot in the GREY buffer at grey-cell (gx,gy), value level, clipped.
// Each dot is DOT_W×DOT_H grey cells, anchored at (gx,gy) and extending right/down.
// DOT_W=2 makes every etch point ≥4px wide (≥6px after dilation), so single points
// render robustly full-white regardless of where they fall vs the TV's sample clock
// (a 1-cell/2px point lands on a "bad" phase at some X and reads grey).
#define DOT_W 2
#define DOT_H 1
static inline void plot_dot(int gx, int gy, uint8_t level) {
    for (int dy = 0; dy < DOT_H; dy++) {
        for (int dx = 0; dx < DOT_W; dx++) {
            int rx = gx + dx, ry = gy + dy;
            if (rx >= 0 && rx < GREY_W && ry >= 0 && ry < GREY_H) {
                GREY_SET(grey_buffer, ry, rx, level);
            }
        }
    }
}

// Draw a line from (x0,y0) to (x1,y1) in the grey buffer (Bresenham), so fast CV
// motion draws a continuous curve instead of sparse dots. Endpoints are grey cells.
static void __not_in_flash_func(draw_line)(int x0, int y0, int x1, int y1, uint8_t level) {
    int adx = x1 - x0; if (adx < 0) adx = -adx;
    int ady = y1 - y0; if (ady < 0) ady = -ady;
    int dx =  adx, sx = x0 < x1 ? 1 : -1;
    int dy = -ady, sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        plot_dot(x0, y0, level);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

static inline int32_t clamp(int32_t v, int32_t lo, int32_t hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

// Map value from [-2048..2047] to [0..max_out]
static inline int32_t map_adc(int32_t val, int32_t max_out) {
    // val range: -2048..2047 (4096 steps)
    // output: 0..max_out
    int32_t shifted = val + 2048;          // 0..4095
    return (shifted * max_out) / 4095;
}

// Etch-a-sketch position: knob sets a base position, CV adds an offset around it.
// knob 0..4095 maps directly to 0..max_out (the resting position with no CV).
// cv -2048..2047 adds a bipolar offset (full CV swing = ±half the screen).
// So a centred knob with no patch = centre of screen, not the corner.
static inline int32_t map_knob_offset_cv(int32_t cv, int32_t knob, int32_t max_out) {
    int32_t base   = (knob * max_out) / 4095;   // 0..max_out from knob
    int32_t offset = (cv * max_out) / 4095;     // ±max_out/2 from CV (cv is ±2048)
    return base + offset;
}

[[maybe_unused]] static void __not_in_flash_func(update_framebuffer)() {
    // Snapshot volatile shared state once. (CV is read per-sample from the etch
    // ring below, not from this snapshot.)
    uint8_t  mode    = shared.mode;
    int32_t  audio_y = shared.audio_y;
    int32_t  kx      = shared.knob_x_scale;
    int32_t  ky      = shared.knob_y_scale;
    uint8_t  sw      = shared.sw_position;

    // Pulse In 1: clear grey buffer (atomic read-clear)
    if (shared.pulse_clear) {
        shared.pulse_clear = false;
        memset(grey_buffer, 0, GREY_SIZE);
        scope_x = 0;
        etch_have_prev = false;   // don't connect a line across the clear
    }

    // Switch: apply background effect (in grey domain) before drawing
    switch (sw) {
        case 0: // UP — phosphor fade: decrement each grey cell toward black, every
                // FADE_EVERY_N frames. Guaranteed true black; tunable lifetime.
            if (++fade_div >= FADE_EVERY_N) {
                fade_div = 0;
                for (int i = 0; i < GREY_SIZE; i++) {
                    if (grey_buffer[i]) grey_buffer[i]--;
                }
            }
            break;
        case 1: // MIDDLE — static, no modification
            break;
        case 2: // DOWN — teletext debug view (build_field overwrites active lines
                // with teletext packets). Nothing to do to the grey buffer here.
            break;
    }

    // Draw based on mode (all in grey-cell coordinates)
    if (mode == 0) {
        etch_have_prev = false;   // in scope mode: next etch sample starts fresh
        // Oscilloscope: sweep a fixed-width bar/frame, drawn from mid-row to the
        // sampled audio level at white (L2). Bars are SCOPE_BAR_W grey cells wide so
        // each white feature is wide enough for the DAC to charge to FULL white —
        // a 1-cell (2px) bar can't rise to white through the resistor network in
        // ~285ns, so it reads grey. SCOPE_BAR_W=2 → 4px bars charge fully.
        #define SCOPE_BAR_W 2
        const int mid = (GREY_H - 1) / 2;
        int gpy = map_adc(audio_y, GREY_H - 1);
        gpy = clamp(gpy, 0, GREY_H - 1);
        int lo = gpy < mid ? gpy : mid;
        int hi = gpy < mid ? mid : gpy;
        for (int n = 0; n < SCOPE_BAR_W; n++) {
            int gx = scope_x;
            scope_x = (scope_x + 1) % GREY_W;

            // Static (sw==1): clear the grey column for a single clean trace.
            // Fade (sw==0) leaves the trail to the fade; snow leaves noise.
            if (sw == 1) {
                for (int gy = 0; gy < GREY_H; gy++) {
                    GREY_SET(grey_buffer, gy, gx, 0);
                }
            }
            for (int gy = lo; gy <= hi; gy++) {
                GREY_SET(grey_buffer, gy, gx, GREY_LEVELS - 1);
            }
        }
    } else {
        // Etch-a-sketch: drain every CV sample Core 0 queued and plot at white (L2).
        uint32_t w = etch_write_idx;        // snapshot Core 0's write head
        uint32_t avail = w - etch_read_idx; // unsigned wrap-safe count
        if (avail > ETCH_RING_SIZE) {       // overran: only the last RING_SIZE survive
            etch_read_idx = w - ETCH_RING_SIZE;
        }
        while (etch_read_idx != w) {
            uint32_t slot = etch_read_idx & ETCH_RING_MASK;
            int32_t rx = etch_x_ring[slot];
            int32_t ry = etch_y_ring[slot];
            int gx = (int)map_knob_offset_cv(rx, kx, GREY_W - 1);
            int gy = (int)map_knob_offset_cv(ry, ky, GREY_H - 1);
            gx = clamp(gx, 0, GREY_W - 1);
            gy = clamp(gy, 0, GREY_H - 1);
            etch_read_idx++;

            // Deadband: ignore samples within ETCH_DEADBAND cells of the last anchor.
            // CV In has ±1–2 LSB jitter; at half-res that flips the cell and, with the
            // line interpolation accumulating in static mode, paints a parallel ghost
            // line. Only commit a new anchor once CV genuinely moves beyond the band —
            // and DON'T update the anchor when skipping, so wobble can't drift it.
            if (etch_have_prev) {
                int adx = gx - etch_prev_x; if (adx < 0) adx = -adx;
                int ady = gy - etch_prev_y; if (ady < 0) ady = -ady;
                if (adx <= ETCH_DEADBAND && ady <= ETCH_DEADBAND) continue;
                draw_line(etch_prev_x, etch_prev_y, gx, gy, GREY_LEVELS - 1);
            } else {
                plot_dot(gx, gy, GREY_LEVELS - 1);
            }
            etch_prev_x = gx;
            etch_prev_y = gy;
            etch_have_prev = true;
        }
    }

    // Expand grey buffer → 1-bit frame_buffer (must finish before build_frame_words)
    expand_grey_to_fb();
}

// ─────────────────────────────────────────────────────────────────────────────
// DMA IRQ handler (Core 1)
// Fires when the DMA finishes the current frame word stream.
// We restart the DMA immediately (to avoid any gap in video output),
// then signal Core 1's update loop via vblank_ready.
// ─────────────────────────────────────────────────────────────────────────────
static void __not_in_flash_func(dma_irq_handler)() {
    dma_hw->ints1 = 1u << dma_chan;  // clear IRQ flag on DMA_IRQ_1

    // Restart DMA from the same (now-completed) active buffer — next frame uses same data
    // until Core 1 swaps in a new back buffer. This ensures video never glitches.
    dma_channel_set_read_addr(dma_chan, word_buf[active_buf], true);

    vblank_ready = true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Core 1 entry point
// ─────────────────────────────────────────────────────────────────────────────
static void __not_in_flash_func(core1_main)() {
    // Claim PIO0 SM0
    PIO pio = pio0;
    uint sm  = 0;

    // Override GPIO 8 (Pu1) and GPIO 9 (Pu2) from ComputerCard's gpio_out to PIO control.
    // Both pins form the 2-bit video DAC; Pulse Out 2 is no longer a normal output.
    gpio_set_function(VIDEO_GPIO,     GPIO_FUNC_PIO0);
    gpio_set_function(VIDEO_GPIO + 1, GPIO_FUNC_PIO0);

    // Max drive (12mA) + fast slew on BOTH DAC pins, configured IDENTICALLY so the two
    // legs switch with matched edge speed. The PIO already drives both bits on the same
    // clock edge (out pins,2), so this removes any pad-level asymmetry and stiffens the
    // outputs to charge the node faster — sharper black↔white edges (helps the teletext
    // run-in crispness and the picture). (The residual edge slew asymmetry from the
    // unequal 1kΩ/220Ω resistors is inherent to the 2-bit DAC and is a hardware matter.)
    gpio_set_drive_strength(VIDEO_GPIO,     GPIO_DRIVE_STRENGTH_12MA);
    gpio_set_drive_strength(VIDEO_GPIO + 1, GPIO_DRIVE_STRENGTH_12MA);
    gpio_set_slew_rate(VIDEO_GPIO,     GPIO_SLEW_RATE_FAST);
    gpio_set_slew_rate(VIDEO_GPIO + 1, GPIO_SLEW_RATE_FAST);

    // Load PIO program
    uint offset = pio_add_program(pio, &video_out_program);

    // Configure SM
    pio_sm_config c = video_out_program_get_default_config(offset);
    sm_config_set_out_pins(&c, VIDEO_GPIO, 2);     // GPIO 8,9 = 2-bit video DAC
    sm_config_set_out_shift(&c, false, true, 32);  // shift left, autopull, threshold=32
    sm_config_set_clkdiv(&c, 16.0f);               // 111MHz/16 = 6.93750 MHz EXACT, INTEGER divider (no jitter)

    pio_sm_set_consecutive_pindirs(pio, sm, VIDEO_GPIO, 2, true);  // GPIO 8,9 output
    pio_sm_init(pio, sm, offset, &c);

    // Claim DMA channel (use a channel not used by ComputerCard)
    // ComputerCard uses channels for ADC DMA (dma_claim_unused_channel) and SPI DMA.
    // We claim one explicitly: channel 4 (ComputerCard uses 0,1,2,3 typically).
    // Safe approach: use dma_claim_unused_channel at startup before ComputerCard inits.
    // Since Core 1 starts after Run(), we use channel 6 (well above CC's range).
    dma_chan = 6;
    dma_channel_claim(dma_chan);

    // Build initial frame (back buffer = 1, active = 0)
    ttx_encode_page();            // pre-encode all teletext packets once
    memset(grey_buffer, 0, GREY_SIZE);
    memset(frame_buffer, 0, FB_SIZE);
    expand_grey_to_fb();          // ensure frame_buffer is valid before first pack
    build_frame_words(0, false);
    build_frame_words(1, false);
    active_buf = 0;

    // Configure DMA: read from word_buf[0], write to PIO TX FIFO, loop
    dma_channel_config dc = dma_channel_get_default_config(dma_chan);
    channel_config_set_transfer_data_size(&dc, DMA_SIZE_32);
    channel_config_set_read_increment(&dc, true);
    channel_config_set_write_increment(&dc, false);
    channel_config_set_dreq(&dc, pio_get_dreq(pio, sm, true));  // pace to PIO TX FIFO

    dma_channel_configure(
        dma_chan, &dc,
        &pio->txf[sm],           // write to PIO SM TX FIFO
        word_buf[active_buf],    // read from front buffer
        FRAME_WORDS,             // transfer count
        false                    // don't start yet
    );

    // Enable DMA IRQ on channel 6, use DMA_IRQ_1 (Core 1 owns it; Core 0's ComputerCard uses DMA_IRQ_0)
    dma_channel_set_irq1_enabled(dma_chan, true);
    irq_set_exclusive_handler(DMA_IRQ_1, dma_irq_handler);
    irq_set_enabled(DMA_IRQ_1, true);

    // Start PIO SM and DMA
    pio_sm_set_enabled(pio, sm, true);
    dma_channel_start(dma_chan);

    // Main Core 1 loop: update framebuffer each vblank
    while (1) {
        // Wait for DMA IRQ to signal frame completion
        while (!vblank_ready) {
            tight_loop_contents();
        }
        vblank_ready = false;

#if TEST_PATTERN
        // Static test rig: write raw patterns straight into frame_buffer (no grey,
        // no dither, no dilation). build_frame_words just scans frame_buffer.
        draw_test_pattern();
        int back = 1 - active_buf;
        build_frame_words(back, false);
#else
        // Update framebuffer with new Eurorack I/O state
        update_framebuffer();

        // Build new word stream into back buffer
        int back = 1 - active_buf;
        bool invert = shared.pulse_invert;
        build_frame_words(back, invert);
#endif

        // Swap buffers: next DMA IRQ handler will restart using the new active_buf.
        // The current DMA is already running (restarted in IRQ handler) using the OLD active_buf.
        // We update active_buf now so the NEXT restart uses the new one.
        active_buf = back;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// ComputerCard subclass — Core 0
// ─────────────────────────────────────────────────────────────────────────────
class CathodeRay : public ComputerCard {
public:
    CathodeRay() {
        memset((void *)&shared, 0, sizeof(shared));
        // Core 1 is launched here (constructor body), before Run() is called.
        // ComputerCard's hardware init runs in its constructor (which has already
        // completed by this point), so GPIO 8 is set up as gpio_out.
        // Core 1 will override it to PIO immediately on entry.
        // ComputerCard claims its DMA channels inside AudioWorker() (called from Run()),
        // which happens after this constructor returns — so Core 1's dma_channel_claim
        // for channel 6 races with nothing: CC uses channels 0 and 1.
        multicore_launch_core1(core1_trampoline);
    }

    static void core1_trampoline() {
        core1_main();
    }

    void __not_in_flash_func(ProcessSample)() override {
#if TEST_PATTERN
        // Test mode: only the Main knob matters (tunes on-pulse width). All other
        // input handling is set aside.
        shared.knob_main = KnobVal(Knob::Main);
        return;
#else
        // Mode: main knob ≥ mid selects etch-a-sketch, below = oscilloscope
        shared.mode = (KnobVal(Knob::Main) >= 2048) ? 1 : 0;

        shared.audio_y      = AudioIn1();
        shared.cv_x         = CVIn1();
        shared.cv_y         = CVIn2();
        shared.knob_x_scale = KnobVal(Knob::X);
        shared.knob_y_scale = KnobVal(Knob::Y);

        // Push this CV sample into the etch ring so Core 1 can plot every sample.
        // Write the data BEFORE advancing the index so Core 1 never reads a half-
        // updated slot (single 32-bit index write is atomic on RP2040).
        uint32_t w = etch_write_idx;
        etch_x_ring[w & ETCH_RING_MASK] = (int16_t)shared.cv_x;
        etch_y_ring[w & ETCH_RING_MASK] = (int16_t)shared.cv_y;
        etch_write_idx = w + 1;

        if (PulseIn1RisingEdge()) {
            shared.pulse_clear = true;
        }

        shared.pulse_invert = PulseIn2();

        Switch sw = SwitchVal();
        if      (sw == Switch::Up)     shared.sw_position = 0;
        else if (sw == Switch::Middle) shared.sw_position = 1;
        else                           shared.sw_position = 2;

        LedOn(0, shared.mode == 0);    // oscilloscope mode
        LedOn(1, shared.mode == 1);    // etch-a-sketch mode
        LedOn(2, shared.pulse_invert); // invert active
        LedOn(3, shared.sw_position == 0); // fade active
        LedOn(4, shared.sw_position == 2); // teletext debug view active
#endif
    }
};

// Global instance — must not be on stack (ComputerCard requirement, stack = 4 KB)
CathodeRay g_card;

int main() {
    // 111.000 MHz = exact integer multiple of the 6.9375 MHz teletext bit rate
    // (clkdiv 16, PLL fbdiv 74 /2/4). Gives a jitter-free pixel/bit clock — a
    // fractional PIO divider smears the teletext data eye and decoders won't lock.
    // (138.75/277.5 MHz from the spec notes are NOT RP2040 PLL-achievable.)
    set_sys_clock_khz(111000, true);
    g_card.Run();  // blocking — never returns
}
