# Cathode Ray — developer handbook

Internal notes for building, modifying and releasing the Cathode Ray firmware. The user-
facing docs are in `README.md`; this file is the "how it works / how to work on it" reference.

Everything lives in a single **`main.cpp`** (plus `composite.pio`, `CMakeLists.txt`, and a
self-contained `ComputerCard.h`). It's a 1-bit composite-video synth for the Music Thing
Workshop Computer (RP2040).

---

## 1. Architecture (two cores)

| Core | Role |
|------|------|
| **Core 0** | ComputerCard `ProcessSample()` ISR @ 48 kHz. Reads all Eurorack I/O, runs knob-pickup, publishes the volatile `shared` struct, pushes CV into the etch ring and audio into the audio ring, drives LEDs and (in alt mode) the CV outputs. Do **no** heavy work here. |
| **Core 1** | Dedicated video loop. Owns PIO0/SM0 + a DMA channel. Each frame: `update_framebuffer()` draws into the **grey buffer** → `expand_grey_to_fb()` dithers it into the 1-bit **framebuffer** → `build_frame_words()` packs sync+blank+active into the DMA word stream → swap buffers at vblank. All drawing/DSP that isn't per-sample happens here. |

The two cores talk **only** through the `volatile SharedState shared` struct. Core 0 writes
inputs; Core 1 reads them and (for alt-mode CV out) writes back a few fields that Core 0 reads.

### Output — the 2-bit resistor DAC
GPIO8 (Pulse Out 1) via **1 kΩ** + GPIO9 (Pulse Out 2) via **220 Ω**, summed into the RCA
centre pin; grounds common. Pulse Out 2 is consumed by video and is not a normal output.
The PIO shifts 2 bits/pixel to those pins. `level_pair[]` (near the top of main.cpp) is the
**one place** to tune polarity/levels:
- `SYNC = 0b11` → both jacks LOW → 0 V (sync tip)
- `BLACK = 0b10` → Pu1 only, via 1 kΩ → a small pedestal **above** sync (must stay > sync or
  the TV loses lock)
- `WHITE = 0b00` → both HIGH → brightest

Only 3 of the 4 levels are used → effectively a crisp 1-bit black/white picture.

---

## 2. Video geometry & the PAL / NTSC split

- **Framebuffer** 360×256, 1 bpp (`FB_WIDTH/FB_HEIGHT/FB_STRIDE/FB_SIZE`).
- **Grey buffer** 180×128, 5 levels (`GREY_SCALE=2`, `GREY_LEVELS=5`, `GREY_W/GREY_H`). All
  drawing is done here; `GREY_H` is **128 for both PAL and NTSC** — never changes.
- `expand_grey_to_fb()`: 2×2 spatial dither (5 levels, 4 orientations cycling every 2 frames
  to average the pattern) + **level-aware right white-dilation** (`dilate_amount[]`, capped
  per-frame by the global `dilate_cap`; `text_mode` disables it for crisp menus).

### PAL vs NTSC — one `#ifdef TV_NTSC` block
All timing divergence lives in a single block near the top of main.cpp. Everything else
(framebuffer, grey buffer, every mode) is identical, so **PAL edits flow to NTSC for free**.

| | PAL (default) | NTSC (`-DTV_NTSC`) |
|---|---|---|
| Line | 448 px (fp12/hs33/bp40/av363), 64.0 µs | 445 px (fp10/hs33/bp32/av370), 63.57 µs |
| Frame | 312 lines, 50 Hz | 262 lines, ~60 Hz |
| Active | 256 rows (all) | 240 rows = a centred crop of the 256-row FB (`TV_ACTIVE_ROW0=8`) |

- Both use the **same 7.000 MHz pixel clock** (clkdiv 144/7, sys 144 MHz) — no PLL change.
- Frame-structure macros are format-neutral (`TV_VSYNC_LINES`, `TV_BLANK_TOP/BOT`,
  `TV_ACTIVE_LINES`, `TV_ACTIVE_ROW0`, `TV_TOTAL_LINES`) so `build_frame_words()` is shared.
- **`FRAME_WORDS` is format-exact** and drives the DMA transfer count — it MUST equal the real
  words/frame or the refresh rate/sync is wrong. Buffers are sized to `FRAME_WORDS_MAX` (PAL).
- **The default (progressive) build uses the ORIGINAL flat broad-pulse vsync** — `emit_vsync_line`
  (BLACK / SYNC(`VSYNC_LOW_PX`) / BLACK) for `TV_VSYNC_LINES`, then `TV_BLANK_TOP` blank lines. This
  is the shipped scheme and locks ROCK-SOLID on forgiving analog TVs/CRTs. **Do not "improve" it.**
  - **Tier-1 lesson (reverted):** a "standards-shaped" vertical interval (equalising + serrated broad
    pulses) was tried on the default path. On a forgiving TV it measurably **degraded** composite
    vertical stability (occasional glitches in busy modes like Starfield/Boing) — and it did **not**
    make a strict component input lock either. Net loss, so it was **removed from the default build**
    and now lives ONLY in the interlace variant (where it has a rationale). Moral: the simple flat
    vsync is empirically better on real forgiving displays; don't reshape it without a display that
    actually needs it AND a composite-stability regression check.

### Interlace — the optional `-DTV_INTERLACE` variant (Tier-2)
For sets that demand true 2:1 interlace, build with `-DTV_INTERLACE` (the `cathode_ray_interlace` /
`cathode_ray_interlace_ntsc` CMake targets → separate uf2s; the plain targets stay progressive and
**byte-for-byte unchanged** — verified). It's off by default; everything below is compiled out unless
the flag is set.
- **Scheme = alternating field lengths.** Even field = `TV_TOTAL_LINES` lines; odd field = **one extra
  blank line** in the vertical interval → `TV_TOTAL_LINES+1`. The two fields sum to the real interlaced
  totals: **PAL 312+313 = 625** (2×312.5), **NTSC 262+263 = 525** (2×262.5). The extra odd-field line
  produces the half-line vertical offset that IS 2:1 interlace.
- **Per-field DMA transfer count (the delicate bit).** The fields differ by one line (28 words), so the
  two word-stream buffers have different lengths. `FRAME_WORDS_EVEN` / `FRAME_WORDS_ODD`; `FRAME_WORDS_MAX`
  is bumped to the odd size (8764) so both buffers fit. Core 1 records each buffer's length in
  `buf_words[]`; `dma_irq_handler` calls `dma_channel_set_trans_count(dma_chan, buf_words[active_buf], false)`
  **before** the read-addr trigger (on RP2040 the trigger reloads TRANS_COUNT from the register — so
  order = set-count-then-trigger). The core1 loop toggles `field` each frame and builds the back buffer
  for that field. Get this restart sequence wrong and there's NO picture (composite included) — it's the
  one place interlace can break everything.
- **Colour** (`colour_beam_line`) tolerates the two field lengths via `COLOUR_FRAME_WORDS_MAX`; the
  ≤1-line offset between fields is invisible at the coarse `HUE_STEPS` band resolution.
- **Still verify `FRAME_WORDS` (per field) matches** if you touch vertical timing, or the picture dies.
  Progressive remains the primary/tested path; interlace is the experimental strict-decoder build.

### The analog white-fidelity gotcha (important)
A lone ~143 ns white pixel can't slew to full white through the resistor DAC → it reads grey.
`WHITE_DILATE` holds each white pixel high for several px to the right so features reach true
white (measured: ~5 px needed). This is why single-pixel data (e.g. teletext) can't work in
firmware — it needs a hardware bandwidth fix.

---

## 3. Building

Toolchain (Bash / Git-Bash):
```
export CMAKE=/c/Users/andyu/.pico-sdk/cmake/v3.31.5/bin/cmake.exe
export PICO_SDK_PATH=/c/Users/andyu/.pico-sdk/sdk/2.2.0
export PICO_TOOLCHAIN_PATH=/c/Users/andyu/.pico-sdk/toolchain/14_2_Rel1
export NINJA=/c/Users/andyu/.pico-sdk/ninja/v1.12.1/ninja.exe
export PATH="$PICO_TOOLCHAIN_PATH/bin:$(dirname $CMAKE):$(dirname $NINJA):$PATH"
cd releases/64_cathode/build
# after any CMakeLists change, reconfigure first:
$CMAKE -G Ninja -S .. -B .
# definitive build (rm forces a real compile so the size report is accurate):
rm -f CMakeFiles/cathode_ray.dir/main.cpp.obj cathode_ray.elf cathode_ray.uf2
$CMAKE --build . --target cathode_ray            # PAL  → cathode_ray.uf2
$CMAKE --build . --target cathode_ray_ntsc       # NTSC → cathode_ray_ntsc.uf2
cp cathode_ray.uf2 ../ ; cp cathode_ray_ntsc.uf2 ../
```
Both targets are built from the same `main.cpp`; NTSC just adds `-DTV_NTSC` (see CMakeLists).
Typical footprint: FLASH ~4 %, RAM ~77 % of the RP2040. RAM is the constraint to watch — see
§6.

---

## 4. Mode systems

### Normal boot — Main knob split in thirds
`main_mode(knob)` → ETCH (< `ETCH_THRESH` 1365) / SCOPE (< `SPECTRUM_THRESH` 2730) / SPECTRUM.

**Switch UP↔MIDDLE are swapped in normal modes** (DOWN unchanged). This is done with a
normal-mode-only swapped view (`nsw` in update_framebuffer, `nswp` in the Core-0 pickup),
NOT at the source — so alt-boot's physical-UP selector and the games are unaffected. If you
add a normal-mode switch check, use `nsw`/`nswp`, not raw `sw`.

- **ETCH**: CV1/CV2 draw X/Y. MID = Knob X/Y scale the CV (+ fade); UP = X/Y offset.
- **SCOPE**: Audio In 1 waveform. MID = phosphor fade; UP = clean static. Knob Y = gain,
  Knob X = baseline (via the pickup system).
- **SPECTRUM**: 24-band integer **Goertzel** bank (`spec_coeff[]` Q13, log ~80 Hz–8 kHz) run
  once/frame over the last 512 audio samples. `spec_tilt[]` tames the bass. Bars decay (fall
  speed from the knob position within the zone). MID = radial pulsing blob (Knob X rotates it,
  grey echo trail); UP = LED-segment bargraph. A SWAP trigger reverses the bins. Spectrum
  reads `shared.knob_x/knob_y` **directly** (its own gentle gain + X-rotate), bypassing pickup.

### Performance triggers + config menu (Switch DOWN)
Three independent sources — Switch-DOWN, Pulse In 1, Pulse In 2 — each with its own
`FxState`, each assigned a behaviour in the on-screen config menu. `apply_behaviour()` /
`run_fx()`. Behaviours: INVERT, CLS, CYCLE FX, RANDOM FX, then fixed STROBE/FADE/FADEWHITE/
SNOW/SWAP/CORRUPT/ROLL. Hold DOWN + twist Main/X/Y to open the menu. `effect_invert` is the
single whole-frame invert path.

### Alt boot — selector of screensaver/performance hybrids
Hold Switch DOWN at power-on (`shared.alt_mode` latched after the ADC settles). Switch UP =
selector (Main scrolls, per-mode help from `ALT_HELP[][5]`); MID/DOWN = play.

**To add an alt-boot mode:** add a name to `ALT_NAMES[]`, a row to `ALT_HELP[][5]`, a `case`
in the dispatch `switch` in update_framebuffer's alt branch, and a `screensaver_*()` function.
That's it.

CV bridge (Core 0, only while PLAYING so the selector isn't CV-polluted):
`shared.alt_cv1` → CV Out 1 (mode-dependent), `shared.ast_gate_seq` → a CV Out 2 event pulse,
`shared.ast_fire` = PU1 latch. `PATCHTEROIDS` (index `ALT_HYBRID_PATCH`) has a special bridge
(CV1 folds into the steer knob, CVOut1 = pitch); other modes read Main/CV raw.

Modes: COMET, PATCHTEROIDS, BOING, STARFIELD, RADAR, LUNAR, 3DMAZE. See `MODES.md` for the
full per-mode control map. 3DMAZE is the heaviest (per-column z-buffer vector raycaster) —
watch video stability if you touch it.

---

## 5. Release / PR workflow (two-branch dance)

- **add-64-cathode** = dev branch (`releases/64_cathode/`). Work here; keep it shippable.
- **add-96-cathode** = the upstream PR branch (`releases/96_cathode/`; 96 because 64 is taken
  upstream). Each folder only exists on its own branch.
- Feature work can go on a branch off add-64-cathode (e.g. `spectrum`) then merge back.

To ship an update upstream:
1. Land the change on add-64-cathode (merge the feature branch).
2. `git checkout add-96-cathode` → **`git reset --hard origin/main`** (fetch first). This is
   critical: base the PR branch on CURRENT upstream, or the PR shows add/add conflicts.
3. Copy `main.cpp` / `README.md` / `composite.pio` / `CMakeLists.txt` verbatim from the 64
   folder into the 96 folder; copy `info.yaml` then **restore its `Repository:` line** to
   `https://github.com/TomWhitwell/Workshop_Computer/tree/main/releases/96_cathode`.
4. **Rebuild the uf2 inside the 96 folder** (the build path is baked into the uf2, so it must
   be built there, not copied from 64).
5. Commit, `git push fork add-96-cathode --force-with-lease`.
6. `gh pr create --repo TomWhitwell/Workshop_Computer --head uglifruit:add-96-cathode`.
   A merged PR stays merged — each new version needs a **new** PR.

The panel overlay `CATHODE - WORKSHOP SYSTEM OVERLAY.jpg` sits in the folder (the gallery
picks it up by naming convention; it isn't referenced in info.yaml).

---

## 6. Gotchas / things to keep in mind

- **RAM ~77 %**. Every alt-boot mode's state is `static` and so allocated simultaneously
  (only one runs at a time). Big new modes with large arrays will push it; if it gets tight,
  the fix is to `union` the per-mode state (only one is live). FLASH is a non-issue (~4 %).
- **`GREY_H` must stay 128** for both formats — the NTSC scheme depends on the shared
  framebuffer. Don't shrink `FB_HEIGHT`; crop at scan-out instead.
- **`FRAME_WORDS` must be format-exact** (it's the DMA count). If you change line/frame
  timing, recheck it.
- Normal-mode switch checks must use the swapped `nsw`/`nswp`, not raw `sw` — or UP/MID will
  be inconsistent with the rest.
- `dilate_cap` and `text_mode` are per-frame globals; reset at the top of update_framebuffer.
  If a screen looks too bloomed or too dim, that's the lever.
- Black must sit above sync (`level_pair`), or the TV won't lock — a lesson already paid for.

---

## 8. Colour output (YPbPr on the audio outs)

The mono picture is the **Y (luma)** of a component signal. On a **YPbPr** display, the existing
2-resistor video output feeds **Y**; **AudioOut1 → Pb**, **AudioOut2 → Pr** add colour. This is
component colour, **not** composite subcarrier colour (no burst) — so it needs a component input and
resolves as coarse bands, never per-pixel.

All of it lives in the **`COLOUR OUTPUT` block near the top of main.cpp** and a few lines in
`ProcessSample`. It is **100% Core 0** — no video-path / Core 1 change:

- **Emit:** the audio DAC is flushed once per `ProcessSample` @ 48 kHz. At the tail of ProcessSample
  we call `colour_sample()` → `AudioOut1(pb); AudioOut2(pr);`. ~960 samples/frame (PAL) ≈ 3-4 per
  scan line — the hard ceiling on colour resolution.
- **Beam position, for free:** `colour_beam_line()` reads `dma_hw->ch[dma_chan].transfer_count`
  (words left in the one-shot frame DMA) and maps it to an active line. `dma_chan` is the Core-1
  video channel (6); Core 0 only reads it, so there is **no new shared state and no Core 1 change**.
  The math uses the format-neutral `LINE_TOTAL_PX` / `TV_VSYNC_LINES` / `TV_BLANK_TOP` /
  `TV_ACTIVE_LINES` macros, so it is correct for **both PAL and NTSC automatically** (same as
  `build_frame_words`). The active-line offset assumes the frame word order VSYNC→BLANK_TOP→ACTIVE,
  which is exactly how `build_frame_words()` emits it — keep them in sync if you reorder that.
- **Scalable mode registry (mirror of the alt-boot pattern):** a `ColourMode` enum + a
  `colour_modes[]` table of `{name, generator}`. Each generator is `int gen(const ColourCtx&)`
  returning a hue index (or -1 = neutral). **To add a colour mode:** append a `COL_*` enum value,
  write a `colgen_*`, add one row to `colour_modes[]`. Selection and emit are generic over the
  table — nothing else changes. `ColourCtx` gives a generator the free-running `frame`, the current
  `line` (-1 during blanking), and the `audio` level.
- **Hue table:** `chroma_pb[]` / `chroma_pr[]` are a 12-point circle of colour-difference pairs
  (±100). More/finer hues = extend the table (`HUE_STEPS`).
- **Selection:** `ProcessSample` maps `AudioIn2()` → `shared.colour_mode` **only in normal boot**
  (`!shared.alt_mode`), with a dead-zone at 0 V (unpatched → `COLOUR_DEFAULT` = BANDS) and zone
  hysteresis. In alt boot AIN2 stays FourTrig's trigger — untouched. An LFO into AIN2 automates
  colour-mode changes.
- **Tuning (the two levers, like `level_pair[]` for luma):** `CHROMA_GAIN` (saturation) and
  `CHROMA_BIAS` (component mid-level trim); `CHROMA_MAX` clamps so a saturated hue + bias can't rail
  the ±2048 DAC. **NTSC note:** the beam-line math is already format-correct, but the *level* scaling
  of Pb/Pr differs slightly between standards — if NTSC colour looks off, adjust `CHROMA_GAIN`/`BIAS`
  (they are shared constants today; split them under `#ifdef TV_NTSC` if a set needs it). PAL tuned
  first. Cost: ~370 bytes RAM, negligible FLASH.

---

## 7. Parked work — teletext

Branch `cathode-teletext-vbi` has a complete, byte-verified World System Teletext encoder
(+ `tools/tti2h.py` to convert `.tti` pages). It does **not** decode on a TV — root cause is
analog bandwidth (a 1 px / 144 ns data pulse can't reach full white through the DAC+cable),
not the encoding. It needs a hardware fix (better cable / lower source R / active 75 Ω buffer)
and a VBI capture stick (stk1160/em28xx + zvbi on Linux) to debug further. Not a firmware
problem; parked.
