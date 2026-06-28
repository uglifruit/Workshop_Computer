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
#define GREY_LEVELS  3                          // 0=black, 1=checker grey, 2=white
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
#define PAL_BLANK_TOP       29                 // +12 lines pushes picture down
#define PAL_ACTIVE_LINES    FB_HEIGHT          // 256
#define PAL_BLANK_BOT       22                 // 5+29+256+22 = 312 ✓
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
#define WHITE_DILATE 2
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

static void __not_in_flash_func(update_framebuffer)() {
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
        case 2: // DOWN — snow: write a random grey level into every cell
            for (int i = 0; i < GREY_SIZE; i++) {
                grey_buffer[i] = (uint8_t)(lcg_rand() % GREY_LEVELS);
            }
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

        // Update framebuffer with new Eurorack I/O state
        update_framebuffer();

        // Build new word stream into back buffer
        int back = 1 - active_buf;
        bool invert = shared.pulse_invert;
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
        LedOn(4, shared.sw_position == 2); // snow active
    }
};

// Global instance — must not be on stack (ComputerCard requirement, stack = 4 KB)
CathodeRay g_card;

int main() {
    set_sys_clock_khz(144000, true);
    g_card.Run();  // blocking — never returns
}
