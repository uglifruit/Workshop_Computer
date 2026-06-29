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
// PAL timing (144 MHz sys clock, SM @ 144/20.571 = 7.000 MHz):
//   1 pixel = 1 SM cycle = 20.571 sys-cycles = 142.857 ns
//   Line:    448 pixels = 64.000 µs  (target 64.000 µs, 0%)
//   Frame:   312 lines  = 19.968 ms  (target 20.000 ms = 50 Hz, -0.16%)

#include "ComputerCard.h"
#include "pico/multicore.h"
#include "hardware/pio.h"
#include "hardware/dma.h"
#include "hardware/irq.h"
#include "hardware/sync.h"
#include "composite.pio.h"
#include <cstring>

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
#define GREY_LEVELS  5                          // 0=black .. 4=white (count of lit 2×2 subpx)
static_assert(FB_WIDTH  % GREY_SCALE == 0, "GREY_SCALE must divide FB_WIDTH");
static_assert(FB_HEIGHT % GREY_SCALE == 0, "GREY_SCALE must divide FB_HEIGHT");

#define GREY_SET(buf, r, c, lvl) ((buf)[(r)*GREY_W + (c)] = (uint8_t)(lvl))

// ─── PAL line timing (7.000MHz clock, 142.857ns/tick, divider=144/7) ─────────
// Line = 64µs = 448 ticks. Segments must sum to exactly 448.
//   fp=12 + hs=33 + bp=40 + av=363 = 448 ✓
//   fp  = 1.65µs target → 12 ticks = 1.714µs
//   hs  = 4.7µs  target → 33 ticks = 4.714µs
//   bp  = 5.7µs  target → 40 ticks = 5.714µs
//   av  = 51.95µs target → 363 ticks = 51.857µs  (FB_WIDTH=360 + 3 px right pad)
#define LINE_FP_PX          12      // front porch
#define LINE_HS_PX          33      // h-sync
#define LINE_BP_PX          40      // back porch
#define LINE_AV_PX          363     // active video (FB_WIDTH=360 + 3 px right padding)
#define LINE_TOTAL_PX       448     // 12+33+40+363 = 448 ✓

// V-sync long pulse: 27.3µs → 191 ticks LOW, 257 ticks HIGH
#define VSYNC_LOW_PX        191
#define VSYNC_HIGH_PX       (LINE_TOTAL_PX - VSYNC_LOW_PX)  // 257

// Frame structure:
#define PAL_VSYNC_LINES     5
#define PAL_BLANK_TOP       33                 // picture vertical position (down vs default)
#define PAL_ACTIVE_LINES    FB_HEIGHT          // 256
#define PAL_BLANK_BOT       18                 // 5+33+256+18 = 312 ✓
#define PAL_TOTAL_LINES     312

// ─── DMA word stream ─────────────────────────────────────────────────────────
// 2 bits/pixel now. 448 px/line × 312 lines = 139776 px × 2 = 279552 bits.
// ceil(279552/32) = 8736 words/frame.  word_buf[2][8736] = 69888 B (~70KB).
#define FRAME_WORDS         8736

// Double-buffered word streams: Core 1 writes to back buffer, DMA reads from front.
static uint32_t __attribute__((aligned(4))) word_buf[2][FRAME_WORDS];
static volatile int active_buf = 0;  // which buffer DMA is currently reading

// ─── Framebuffer (written by Core 1 during vblank) ───────────────────────────
static uint8_t frame_buffer[FB_SIZE];

// ─── Grey working buffer (Core-1-private; expanded into frame_buffer each frame)
static uint8_t grey_buffer[GREY_SIZE];

// ─── Etch CV ring buffer (Core 0 pushes @48kHz, Core 1 drains each frame) ─────
// Captures sub-frame CV motion: each frame Core 1 plots every sample Core 0 queued.
// Core 0 produces 48000/50 = 960 samples per frame; the ring MUST hold ≥ that or the
// overrun guard drops the oldest ~half each frame → sparse points → visible straight
// lines between them. 2048 (≈42.7ms) covers a full frame with comfortable margin.
#define ETCH_RING_SIZE   2048
#define ETCH_RING_MASK   (ETCH_RING_SIZE - 1)
static volatile int16_t etch_x_ring[ETCH_RING_SIZE];
static volatile int16_t etch_y_ring[ETCH_RING_SIZE];
static volatile uint32_t etch_write_idx = 0;  // Core 0 increments after each push

// ─── Audio ring buffer (Core 0 pushes AudioIn1 @48kHz; scope pulls 1/column) ──
// Lets the scope draw a real waveform at fast sweep: each drawn column reads a fresh
// audio sample instead of reusing one per frame (which made bars fat at speed).
#define AUDIO_RING_SIZE  1024     // ≈21ms @48kHz — covers a full frame's worth + margin
#define AUDIO_RING_MASK  (AUDIO_RING_SIZE - 1)
static volatile int16_t  audio_ring[AUDIO_RING_SIZE];
static volatile uint32_t audio_write_idx = 0;

// ─── Shared state (Core 0 ISR → Core 1 video loop) ───────────────────────────
struct SharedState {
    volatile int32_t  audio_y;       // AudioIn1() raw: -2048..2047
    volatile int32_t  cv_x;          // CVIn1()   raw: -2048..2047
    volatile int32_t  cv_y;          // CVIn2()   raw: -2048..2047
    // Etch X/Y mapping, resolved on Core 0 (pickup hysteresis) → consumed by Core 1.
    // pos = offset + (cv * cvgain) >> 12.  offset in grey coords; cvgain is 12.12 fixed.
    volatile int32_t  etch_offset_x; // 0..GREY_W-1 base position
    volatile int32_t  etch_offset_y; // 0..GREY_H-1
    volatile int32_t  etch_cvgain_x; // CV→grey gain (>>12): scale·(GREY_W-1)·4096/(256·4095)
    volatile int32_t  etch_cvgain_y; // (>>12): scale·(GREY_H-1)·4096/(256·4095)
    volatile int32_t  scope_audio_scale; // Y knob in scope: 0..512 (256 = 1×, max 2×) audio gain
    volatile int32_t  scope_baseline;    // X knob in scope: 0..GREY_H-1 centre-line row
    volatile int32_t  knob_main;     // KnobVal(Main): 0..4095 (far-CCW=etch, else scope speed)
    volatile bool     pulse_clear;   // set on Pulse In 1 rising edge
    volatile bool     pulse_invert;  // true while Pulse In 2 is HIGH
    volatile uint8_t  sw_position;   // 0=UP(fade) 1=MID(static) 2=DOWN(wipe + X/Y=offset)
};
static SharedState shared;

// ─── Multi-function knob pickup (Core 0) ──────────────────────────────────────
// A knob's destination depends on mode+switch. When it changes, the newly-bound param
// holds its stored value until the knob is physically moved past PICKUP_THRESH
// ("pickup"), preventing a jump. Each destination keeps its own stored value.
// Destinations: SCALE (etch CV scale, UP), OFFSET (etch position, MID),
//   AUDIO (scope Y audio gain), BASELINE (scope X = centre-line vertical position).
#define PICKUP_THRESH   64       // raw knob movement (of 4095) needed to grab control
#define PICK_SCALE      0
#define PICK_OFFSET     1
#define PICK_AUDIO      2
#define PICK_BASELINE   3
struct KnobPick {
    int32_t stored_offset;       // grey coords — etch X/Y position
    int32_t stored_scale;        // 0..1024 (256 = 1×)  — etch CV scale
    int32_t stored_audio;        // 0..512 (256 = 1×, max 2×) — scope audio scale (Y only)
    int32_t stored_baseline;     // 0..GREY_H-1 — scope centre-line vertical pos (X only)
    uint8_t bound;               // current destination (PICK_*)
    int32_t bind_raw;            // raw knob value captured at last bind change
    bool    captured;            // false until knob moves past threshold after a switch
};

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

    // V-sync line: short front porch, long sync pulse, rest black
    auto emit_vsync_line = [&]() {
        emit_const(BLACK, LINE_FP_PX);
        emit_const(SYNC,  VSYNC_LOW_PX);
        emit_const(BLACK, VSYNC_HIGH_PX - LINE_FP_PX);
    };

    // V-sync lines (0 .. PAL_VSYNC_LINES-1)
    for (int l = 0; l < PAL_VSYNC_LINES; l++) {
        emit_vsync_line();
    }

    // Top blank lines
    for (int l = 0; l < PAL_BLANK_TOP; l++) {
        emit_blank_line();
    }

    // Active video. Framebuffer bit SET = white pixel, CLEAR = black pixel.
    // invert swaps WHITE/BLACK selection.
    for (int row = 0; row < PAL_ACTIVE_LINES; row++) {
        emit_const(BLACK, LINE_FP_PX);   // front porch (black)
        emit_const(SYNC,  LINE_HS_PX);   // h-sync
        emit_const(BLACK, LINE_BP_PX);   // back porch (black)
        const uint8_t *fb_row = &frame_buffer[row * FB_STRIDE];
        for (int p = 0; p < FB_WIDTH; p++) {
            bool set = (fb_row[p / 8] >> (7 - (p & 7))) & 1u;
            if (invert) set = !set;
            uint32_t pair = level_pair[set ? WHITE : BLACK] & 0x3;
            for (int s = 0; s < PIXEL_STRETCH; s++) {
                cur_word = (cur_word << 2) | pair;
                bit_pos += 2;
                if (bit_pos == 32) commit_word();
            }
        }
        emit_const(BLACK, LINE_AV_PX - FB_WIDTH);  // right-pad to fill LINE_AV_PX (363-360 = 3 px)
    }

    // Bottom blank lines
    for (int l = 0; l < PAL_BLANK_BOT; l++) {
        emit_blank_line();
    }

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

// ─────────────────────────────────────────────────────────────────────────────
// Framebuffer drawing
// Called by Core 1 during vblank. Reads from shared, writes to frame_buffer[].
// ─────────────────────────────────────────────────────────────────────────────

// ─── Performance effects (switch DOWN, momentary) ────────────────────────────
// Each DOWN press cycles to the next effect; holding DOWN runs the current one.
enum { FX_STROBE, FX_FREEZE_BLACK, FX_FREEZE_WHITE, FX_SWAP_XY, FX_SNOW, FX_CORRUPT,
       FX_COUNT };
static int  fx_index = 0;          // which effect the next DOWN-hold runs
static bool fx_prev_down = false;  // edge detect for "new DOWN press"
static bool effect_invert = false; // strobe: OR'd into build_frame_words invert
static uint32_t fx_phase = 0;      // generic per-effect frame counter

// ─── Oscilloscope sweep (knob-controlled speed, fixed-point accumulator) ──────
// scope_x is the integer sweep column. scope_acc accumulates fractional columns each
// frame (8.8 fixed point) so slow speeds (<1 col/frame) and fast (>1) both work.
static int scope_x = 0;            // current sweep column (grey-X, Core 1 private)
static uint32_t scope_acc = 0;     // fractional column accumulator (8.8 fixed)
static uint32_t audio_read_idx = 0; // Core 1's drain position in the audio ring
// Sweep speed range (cols/frame, 8.8 fixed). 180 cols, 50 fps:
//   fast = 180/(0.1*50)=36 cols/frame; slow = 180/(3.0*50)=1.2 cols/frame.
#define SCOPE_STEP_SLOW   (307u)   //  1.2 cols/frame × 256 ≈ 307  (~3.0s/sweep)
#define SCOPE_STEP_FAST   (9216u)  // 36.0 cols/frame × 256 = 9216 (~0.1s/sweep)
// Main knob: lowest 25% (0..1023) = etch (knob sets fade rate); upper 75% = scope speed.
#define ETCH_THRESH       1024     // knob_main below this = etch (CV1 vs CV2) mode

// ─── Phosphor fade ───────────────────────────────────────────────────────────
// Scope fade is LOCKED to the sweep: one global level-decrement each time the sweep
// has advanced GREY_W/(GREY_LEVELS-1) columns. Over a full pass the sweep advances
// GREY_W cols = (GREY_LEVELS-1) decrements = level (max)→0, so a freshly-drawn column
// reaches true black just as the sweep wraps back to it, at ANY speed. (fade_cols
// accumulates columns advanced since the last decrement.)
static uint32_t fade_cols = 0;     // scope: columns since last decrement; etch: frames
// Etch fade rate is set by the knob across the etch quarter: knob 0 → fastest (0.15s
// to black), knob (ETCH_THRESH-1) → slowest (2.0s). 5 levels = 4 decrements:
//   0.15s = 7.5 frames → decrement every ~2 frames; 2.0s = 100 frames → every ~25.
#define ETCH_FADE_FAST    2        // frames/decrement at knob 0    (~0.15s to black)
#define ETCH_FADE_SLOW    25       // frames/decrement at knob max  (~2.0s to black)
static uint32_t etch_read_idx = 0; // Core 1's drain position in the etch CV ring
static int etch_prev_x = 0, etch_prev_y = 0; // last etch point (for line interpolation)
static bool etch_have_prev = false;          // false until first etch sample drawn
#define ETCH_DEADBAND 1   // grey-cell jitter band: ignore moves within ±this of anchor

// ─── 2×2 dither patterns: 5 levels × 4 orientations (GREY_SCALE=2) ────────────
// dither[level][orientation][sub_row]; each value = 2-bit field (bit1=left,bit0=right).
// level = number of lit subpixels (0..4). 4 orientations per level are cycled every 2
// frames so the dither rotates and averages out spatially over time (smoother greys,
// less fixed checkerboard). L0 and L4 are identical across orientations; L1/L2/L3 differ.
// Cell layout per entry: {top_row, bottom_row}.
static const uint8_t dither[GREY_LEVELS][4][2] = {
    /*L0 0/4*/ {{0b00,0b00},{0b00,0b00},{0b00,0b00},{0b00,0b00}},
    /*L1 1/4*/ {{0b10,0b00},{0b01,0b00},{0b00,0b01},{0b00,0b10}}, // single px in TL,TR,BR,BL
    /*L2 2/4*/ {{0b10,0b01},{0b01,0b10},{0b11,0b00},{0b00,0b11}}, // diag\, diag/, top, bottom
    /*L3 3/4*/ {{0b11,0b10},{0b11,0b01},{0b01,0b11},{0b10,0b11}}, // one corner off
    /*L4 4/4*/ {{0b11,0b11},{0b11,0b11},{0b11,0b11},{0b11,0b11}},
};

// Dither orientation index, advanced every 2 frames by Core 1 (see core1_main loop).
static int dither_orient = 0;

// Right-dilate white by WHITE_DILATE pixels: every white pixel also forces the next
// WHITE_DILATE pixels to its RIGHT white. Exploits the composite DAC's rising-edge
// slew — the node only reaches full white when several adjacent pixels are white, so a
// lone white pixel reads grey. Dilating guarantees every white feature is ≥(1+N)px in
// scan order → renders full white. MSB = leftmost pixel, so "right" = toward the LSB
// (right shift). We track a rolling history of the last 8 emitted bits so the dilate
// crosses byte boundaries for any N up to 7.
#define WHITE_DILATE 4   // measured on hardware: an isolated white px needs ~5px (1+4) to reach true white

// Per-level rightward dilation in PIXELS, indexed by grey level 0..4. Higher levels
// are held on longer (→ brighter); low/fading levels get little/no dilation so dim
// pixels actually look dim. This makes dilation LEVEL-AWARE so it no longer flattens
// the greyscale (a lone L1 sub-pixel previously got the full 4px hold = looked white).
//   L0=black(0)  L1=1  L2=2  L3=3  L4=full white(4=WHITE_DILATE)
static const uint8_t dilate_amount[GREY_LEVELS] = { 0, 1, 2, 3, WHITE_DILATE };

// Level-aware right-dilation: for each grey cell, extend its lit sub-pixels rightward
// by dilate_amount[level] pixels. Seeds from a SNAPSHOT of the row's original lit bits
// (not its own writes) so chained low-level cells don't over-extend. `grow` = grey row.
static inline void dilate_white_leveled(uint8_t *fb, const uint8_t *grow) {
    uint8_t src[FB_STRIDE];
    for (int b = 0; b < FB_STRIDE; b++) src[b] = fb[b];   // snapshot original lit bits
    for (int gx = 0; gx < GREY_W; gx++) {
        int amt = dilate_amount[grow[gx]];
        if (amt == 0) continue;
        int base_p = gx * GREY_SCALE;
        for (int sx = 0; sx < GREY_SCALE; sx++) {
            int p = base_p + sx;
            // seed only if this pixel was lit by the dither (read snapshot, not fb)
            if (src[p >> 3] & (0x80u >> (p & 7))) {
                for (int d = 1; d <= amt; d++) {
                    int pp = p + d;
                    if (pp < FB_WIDTH) fb[pp >> 3] |= (0x80u >> (pp & 7));
                }
            }
        }
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
                    byte = (uint8_t)((byte << GREY_SCALE) | dither[grow[cell++]][dither_orient][sub]);
                }
                fb[b] = byte;
            }
#if WHITE_DILATE
            dilate_white_leveled(fb, grow);
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

// Decrement every grey cell toward black (one phosphor-fade step).
static inline void fade_step() {
    for (int i = 0; i < GREY_SIZE; i++) if (grey_buffer[i]) grey_buffer[i]--;
}
// Increment every grey cell toward white (one bloom step).
static inline void bloom_step() {
    for (int i = 0; i < GREY_SIZE; i++) if (grey_buffer[i] < GREY_LEVELS-1) grey_buffer[i]++;
}
// Corrupt: dramatic glitch — shift rows by random amounts, smear, and inject noise
// blocks. Rewrites grey_buffer in place each frame for a churning broken-signal look.
static void __not_in_flash_func(corrupt_step)() {
    for (int gy = 0; gy < GREY_H; gy++) {
        uint32_t r = lcg_rand();
        if ((r & 7) == 0) {                          // ~1/8 rows: blank or random-fill
            uint8_t v = (r >> 8) & 1 ? (GREY_LEVELS-1) : 0;
            for (int gx = 0; gx < GREY_W; gx++) grey_buffer[gy*GREY_W + gx] = v;
        } else {                                     // else: cyclically shift the row
            int sh = (int)((r >> 4) % GREY_W);
            uint8_t *row = &grey_buffer[gy*GREY_W];
            // rotate-by-sh via a one-pass temp on the stack (GREY_W=180)
            uint8_t tmp[GREY_W];
            for (int gx = 0; gx < GREY_W; gx++) tmp[gx] = row[(gx+sh) % GREY_W];
            for (int gx = 0; gx < GREY_W; gx++) row[gx] = tmp[gx];
            if ((r & 0x30) == 0) {                    // occasional noise speckle in the row
                for (int n = 0; n < 8; n++) row[(lcg_rand()) % GREY_W] = lcg_rand() % GREY_LEVELS;
            }
        }
    }
}

static void __not_in_flash_func(update_framebuffer)() {
    // Snapshot volatile shared state once. (CV is read per-sample from the etch
    // ring below, not from this snapshot.)
    int32_t  knob    = shared.knob_main;
    int32_t  ox      = shared.etch_offset_x;   // resolved etch X/Y mapping (Core 0 pickup)
    int32_t  oy      = shared.etch_offset_y;
    int32_t  gxn     = shared.etch_cvgain_x;
    int32_t  gyn     = shared.etch_cvgain_y;
    uint8_t  sw      = shared.sw_position;
    bool     etch    = (knob < ETCH_THRESH);   // far-CCW = etch (CV1 vs CV2)

    // Pulse In 1: clear grey buffer (atomic read-clear)
    if (shared.pulse_clear) {
        shared.pulse_clear = false;
        memset(grey_buffer, 0, GREY_SIZE);
        scope_x = 0; scope_acc = 0; fade_cols = 0;
        etch_have_prev = false;   // don't connect a line across the clear
    }

    // Switch DOWN (momentary) = PERFORMANCE EFFECT. Each new DOWN press cycles to the
    // next effect (fx_index); holding DOWN runs the current one. effect_invert (strobe)
    // is consumed in the Core 1 loop. swap_xy is honoured by the etch branch below.
    bool down = (sw == 2);
    if (down && !fx_prev_down) {            // rising edge: advance to next effect
        fx_index = (fx_index + 1) % FX_COUNT;
        fx_phase = 0;
    }
    bool swap_xy = false;
    effect_invert = false;                  // default; strobe sets it per-frame
    if (down) {
        fx_phase++;
        switch (fx_index) {
            case FX_STROBE:                 // rapid whole-screen flash; freeze drawing
                effect_invert = (fx_phase & 2);   // toggle every 2 frames (~12Hz)
                expand_grey_to_fb(); fx_prev_down = true; return;
            case FX_FREEZE_BLACK:           // freeze + fade to black ~0.3s
                if ((fx_phase & 3) == 0) fade_step();
                expand_grey_to_fb(); fx_prev_down = true; return;
            case FX_FREEZE_WHITE:           // freeze + bloom to white ~0.3s
                if ((fx_phase & 3) == 0) bloom_step();
                expand_grey_to_fb(); fx_prev_down = true; return;
            case FX_SNOW:                   // fill with random levels each frame
                for (int i = 0; i < GREY_SIZE; i++)
                    grey_buffer[i] = (uint8_t)(lcg_rand() % GREY_LEVELS);
                expand_grey_to_fb(); fx_prev_down = true; return;
            case FX_CORRUPT:                // dramatic glitch
                corrupt_step();
                expand_grey_to_fb(); fx_prev_down = true; return;
            case FX_SWAP_XY:                // transpose etch axes; keep drawing (fall through)
                swap_xy = true;
                break;
        }
    }
    fx_prev_down = down;

    if (!etch) {
        etch_have_prev = false;   // in scope mode: next etch sample starts fresh

        // Sweep speed from knob: lerp SCOPE_STEP_SLOW..FAST across knob[ETCH_THRESH..4095].
        // 8.8 fixed-point cols/frame.
        int32_t kspan = 4095 - ETCH_THRESH;
        int32_t kpos  = knob - ETCH_THRESH; if (kpos < 0) kpos = 0; if (kpos > kspan) kpos = kspan;
        uint32_t step = SCOPE_STEP_SLOW +
            (uint32_t)((int64_t)(SCOPE_STEP_FAST - SCOPE_STEP_SLOW) * kpos / kspan);

        // Centre line (0V baseline) row is set by the X knob in scope mode.
        const int mid = clamp(shared.scope_baseline, 0, GREY_H - 1);
        int32_t ascale = shared.scope_audio_scale;   // Y knob: audio gain (256 = 1×)

        // Advance the sweep by `step` (8.8). For each whole column crossed, pull a
        // FRESH audio sample (so fast sweeps draw a real waveform, not one fat bar/
        // frame), draw the bar there, and drive the sweep-locked fade.
        const uint32_t FADE_EVERY_COLS = GREY_W / (GREY_LEVELS - 1);  // 180/4 = 45
        uint32_t aw = audio_write_idx;                 // snapshot Core 0 write head
        // How many fresh audio samples are available to consume this frame.
        uint32_t avail = aw - audio_read_idx;
        if (avail > AUDIO_RING_SIZE) {                 // overran: keep only the newest
            audio_read_idx = aw - AUDIO_RING_SIZE;
            avail = AUDIO_RING_SIZE;
        }
        // Count whole columns we'll draw this frame, to spread audio samples across them.
        scope_acc += step;
        uint32_t cols_this_frame = scope_acc >> 8;
        uint32_t consumed = 0;
        while (scope_acc >= 256) {
            scope_acc -= 256;
            int gx = scope_x;
            // FX_SWAP_XY in scope = reverse sweep direction while held.
            if (swap_xy) scope_x = (scope_x + GREY_W - 1) % GREY_W;  // step left
            else         scope_x = (scope_x + 1) % GREY_W;           // step right

            // Pick this column's audio sample. If we have at least as many fresh
            // samples as columns, step through them evenly; else take the latest.
            int16_t a;
            if (avail >= cols_this_frame && cols_this_frame > 0) {
                // even decimation: map column index → sample index across the frame
                uint32_t si = audio_read_idx + (consumed * avail / cols_this_frame);
                a = audio_ring[si & AUDIO_RING_MASK];
            } else {
                a = audio_ring[(aw - 1) & AUDIO_RING_MASK];  // latest sample
            }
            consumed++;

            // Scale audio around 0 by the Y-knob gain (256 = 1×), then map to a row
            // with +ve voltage going UP (row 0 = top): gpy = mid - scaled·mid/2048.
            int32_t sc = ((int32_t)a * ascale) >> 8;        // gain applied around 0
            int gpy = mid - (int)((sc * mid) / 2048);       // invert: +ve = up
            gpy = clamp(gpy, 0, GREY_H - 1);
            int lo = gpy < mid ? gpy : mid;
            int hi = gpy < mid ? mid : gpy;

            if (sw == 1) {  // MIDDLE — static: clear column for a single clean trace
                for (int gy = 0; gy < GREY_H; gy++) GREY_SET(grey_buffer, gy, gx, 0);
            }
            for (int gy = lo; gy <= hi; gy++)
                GREY_SET(grey_buffer, gy, gx, GREY_LEVELS - 1);

            if (sw == 0) {  // UP — fade locked to sweep
                if (++fade_cols >= FADE_EVERY_COLS) { fade_cols = 0; fade_step(); }
            }
        }
        audio_read_idx = aw;   // consumed up to the snapshot head
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
            // pos = offset + (cv * cvgain) >> 12.  offset & cvgain resolved on Core 0.
            int gx = ox + (int)((rx * gxn) >> 12);
            int gy = oy + (int)((ry * gyn) >> 12);
            if (swap_xy) { int t = gx; gx = gy; gy = t; }   // FX_SWAP_XY: transpose axes
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
        // Etch fade (switch UP): rate from knob across the etch quarter — knob 0 =
        // fastest (~0.25s), knob (ETCH_THRESH-1) = slowest (~3s).
        if (sw == 0) {
            int32_t kp = knob; if (kp < 0) kp = 0; if (kp >= ETCH_THRESH) kp = ETCH_THRESH - 1;
            uint32_t interval = ETCH_FADE_FAST +
                (uint32_t)((ETCH_FADE_SLOW - ETCH_FADE_FAST) * kp / (ETCH_THRESH - 1));
            if (++fade_cols >= interval) { fade_cols = 0; fade_step(); }
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

    // Load PIO program
    uint offset = pio_add_program(pio, &video_out_program);

    // Configure SM
    pio_sm_config c = video_out_program_get_default_config(offset);
    sm_config_set_out_pins(&c, VIDEO_GPIO, 2);     // GPIO 8,9 = 2-bit video DAC
    sm_config_set_out_shift(&c, false, true, 32);  // shift left, autopull, threshold=32
    sm_config_set_clkdiv(&c, 144.0f/7.0f);         // 144 MHz / 20.571 = 7.000 MHz = 142.857 ns/pixel

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

        // Advance the dither orientation every 2 frames (rotates the 2×2 patterns so
        // greys average out over time → smoother, less fixed checkerboard).
        static uint32_t frame_counter = 0;
        frame_counter++;
        dither_orient = (frame_counter >> 1) & 3;

        // Update framebuffer with new Eurorack I/O state
        update_framebuffer();

        // Build new word stream into back buffer
        int back = 1 - active_buf;
        bool invert = shared.pulse_invert ^ effect_invert;  // strobe XORs in
        build_frame_words(back, invert);

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
        // Main knob: far-CCW = etch (CV1 vs CV2); rest = scope, slow→fast CW.
        shared.knob_main = KnobVal(Knob::Main);

        shared.audio_y      = AudioIn1();
        shared.cv_x         = CVIn1();
        shared.cv_y         = CVIn2();

        // Push this CV sample into the etch ring so Core 1 can plot every sample.
        // Write the data BEFORE advancing the index so Core 1 never reads a half-
        // updated slot (single 32-bit index write is atomic on RP2040).
        uint32_t w = etch_write_idx;
        etch_x_ring[w & ETCH_RING_MASK] = (int16_t)shared.cv_x;
        etch_y_ring[w & ETCH_RING_MASK] = (int16_t)shared.cv_y;
        etch_write_idx = w + 1;

        // Push the audio sample into the audio ring (for the scope waveform).
        uint32_t aw = audio_write_idx;
        audio_ring[aw & AUDIO_RING_MASK] = (int16_t)shared.audio_y;
        audio_write_idx = aw + 1;

        if (PulseIn1RisingEdge()) {
            shared.pulse_clear = true;
        }

        shared.pulse_invert = PulseIn2();

        Switch sw = SwitchVal();
        if      (sw == Switch::Up)     shared.sw_position = 0;
        else if (sw == Switch::Middle) shared.sw_position = 1;
        else                           shared.sw_position = 2;

        // ── Dual-function X/Y knob pickup ──────────────────────────────────────
        // DOWN → knobs edit OFFSET; UP/MIDDLE → knobs edit SCALE. Boot holdoff: the
        // switch reads Down before the ADC settles (CLAUDE.md), so don't run pickup
        // until settled, or a spurious boot-DOWN freezes both knobs uncaptured.
        static uint32_t boot_ctr = 0;
        static KnobPick pkX, pkY;
        static bool pick_init = false;
        if (!pick_init) {   // defaults: centred offset/baseline, unity scale & audio
            //     offset,        scale, audio, baseline,        bound,      bind, captured
            pkX = { (GREY_W-1)/2, 256,   256,   (GREY_H-1)/2,    PICK_SCALE, 0,    true };
            pkY = { (GREY_H-1)/2, 256,   256,   (GREY_H-1)/2,    PICK_SCALE, 0,    true };
            pick_init = true;
        }
        uint8_t swp = shared.sw_position;     // 0=UP 1=MID 2=DOWN
        bool down = (swp == 2);
        bool etch_mode = (shared.knob_main < ETCH_THRESH);
        // Knob destinations by mode + switch:
        //   ETCH + UP  → both SCALE      ETCH + MID → both OFFSET
        //   SCOPE(up/mid) → X = OFFSET (centre-line vertical pos), Y = AUDIO gain
        //   DOWN → performance effect; knobs hold their current binding (no change).
        uint8_t desiredX, desiredY;
        if (etch_mode) {
            desiredX = desiredY = (swp == 0) ? PICK_SCALE : PICK_OFFSET;  // UP=scale, MID=offset
        } else {
            desiredX = PICK_BASELINE; // scope: X = baseline vertical position
            desiredY = PICK_AUDIO;    // scope: Y = audio gain
        }
        if (down) {                   // DOWN drives the effect; freeze knob bindings
            desiredX = pkX.bound;
            desiredY = pkY.bound;
        }

        auto run_pick = [&](KnobPick &p, int32_t raw, int32_t maxpos, uint8_t desired,
                            bool invert_offset) {
            if (desired != p.bound) {          // destination changed (mode/switch)
                p.bound = desired;
                p.bind_raw = raw;              // remember physical position now
                p.captured = false;           // hold stored value until knob moves
            }
            if (!p.captured) {
                int32_t d = raw - p.bind_raw; if (d < 0) d = -d;
                if (d >= PICKUP_THRESH) p.captured = true;
            }
            if (p.captured) {
                if (p.bound == PICK_OFFSET) {
                    int32_t off = (raw * maxpos) / 4095;
                    p.stored_offset = invert_offset ? (maxpos - off) : off;  // Y flips
                } else if (p.bound == PICK_AUDIO) {
                    p.stored_audio = (raw * 512) / 4095;    // 0..2× audio gain (256 = 1×)
                } else if (p.bound == PICK_BASELINE) {
                    // scope centre-line vertical position, 0..GREY_H-1 (0V baseline row)
                    p.stored_baseline = (raw * (GREY_H - 1)) / 4095;
                } else { // PICK_SCALE: 0..1024 (256=1×) attenuate/boost CV up to 4×
                    p.stored_scale = (raw * 1024) / 4095;
                }
            }
        };

        if (boot_ctr < 4800) {
            boot_ctr++;
        } else {
            run_pick(pkX, KnobVal(Knob::X), GREY_W - 1, desiredX, false);
            run_pick(pkY, KnobVal(Knob::Y), GREY_H - 1, desiredY, true); // Y offset inverted
        }

        // Publish resolved etch mapping. cvgain is 12.12 fixed: Core 1 does
        // pos = offset + (cv * cvgain) >> 12, giving cv·(scale/256)·maxpos/4095.
        // scale: 256 = 1× (full CV swing → ≈±89 cells); up to 1024 = 4× boost.
        shared.etch_offset_x = pkX.stored_offset;
        shared.etch_offset_y = pkY.stored_offset;
        shared.etch_cvgain_x = (int32_t)((int64_t)pkX.stored_scale * (GREY_W - 1) * 4096 / (256 * 4095));
        shared.etch_cvgain_y = (int32_t)((int64_t)pkY.stored_scale * (GREY_H - 1) * 4096 / (256 * 4095));
        shared.scope_audio_scale = pkY.stored_audio;   // Y knob in scope: 0..512 (256=1×)
        shared.scope_baseline    = pkX.stored_baseline; // X knob in scope: baseline row

        bool etch = (shared.knob_main < ETCH_THRESH);
        LedOn(0, !etch);               // scope mode
        LedOn(1, etch);                // etch-a-sketch mode (far CCW)
        LedOn(2, shared.pulse_invert); // invert active
        LedOn(3, shared.sw_position == 0); // fade active
        LedOn(4, shared.sw_position == 2); // freeze & fade active (switch DOWN held)
    }
};

// Global instance — must not be on stack (ComputerCard requirement, stack = 4 KB)
CathodeRay g_card;

int main() {
    set_sys_clock_khz(144000, true);
    g_card.Run();  // blocking — never returns
}
