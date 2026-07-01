# CLAUDE.md — Workshop Computer Firmware

This file gives Claude Code context for working in this repository.

## Repository

- **Local path**: `c:\Users\andyu\Documents\WorkshopComputerFork`
- **Upstream (pull only)**: `origin` → https://github.com/TomWhitwell/Workshop_Computer
- **Personal fork (push here)**: `fork` → https://github.com/uglifruit/Workshop_Computer
- Never push to `origin` — no write access. Always push to `fork`.

## Adding New Firmware

1. Create `Demonstrations+HelloWorlds/PicoSDK/ComputerCard/examples/<name>/main.cpp`
2. Add `add_example(<name>)` to `Demonstrations+HelloWorlds/PicoSDK/ComputerCard/CMakeLists.txt`
3. Commit directly to `main` and `git push fork main` — no feature branches needed

## Build Commands (from Bash/Git Bash)

```bash
export CMAKE="/c/Users/andyu/.pico-sdk/cmake/v3.31.5/bin/cmake.exe"
export PICO_SDK_PATH="/c/Users/andyu/.pico-sdk/sdk/2.2.0"
export PICO_TOOLCHAIN_PATH="/c/Users/andyu/.pico-sdk/toolchain/14_2_Rel1"
export NINJA="/c/Users/andyu/.pico-sdk/ninja/v1.12.1/ninja.exe"
export PATH="$PICO_TOOLCHAIN_PATH/bin:$(dirname $CMAKE):$(dirname $NINJA):$PATH"

cd "Demonstrations+HelloWorlds/PicoSDK/ComputerCard/build"
$CMAKE .. -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DPICO_SDK_PATH="$PICO_SDK_PATH" \
  -DPICO_TOOLCHAIN_PATH="$PICO_TOOLCHAIN_PATH"
$CMAKE --build . --target <firmware_name>
```

Output `.uf2` lands in `Demonstrations+HelloWorlds/PicoSDK/ComputerCard/build/`.

## Publishing a Firmware

### Your fork (source + releases)
- Source lives in `examples/<name>/`
- Releases at `github.com/uglifruit/Workshop_Computer/releases` — create with:
  ```bash
  gh release create <name>-v1.0.0 path/to/firmware.uf2 --repo uglifruit/Workshop_Computer --title "..." --notes "..."
  ```

### Tom's card listing (tomwhitwell.github.io/Workshop_Computer)
- Add a folder `releases/<number>_<name>/` containing:
  - `info.yaml` — Name, Description, Language, Creator, Version, Status
  - `README.md` — full documentation
  - `<name>.uf2` — prebuilt binary
  - `main.cpp` — source
  - key image (jpg)
- Push to a branch, open PR against `TomWhitwell/Workshop_Computer` main
- Folder number: check existing folders and pick an unused number
- After initial merge, subsequent updates need a new PR

### Keeping releases/ in sync
- `releases/<number>_<name>/README.md` should always match the full docs in `examples/<name>/`
- `releases/<number>_<name>/main.cpp` and `.uf2` should always match the latest build
- Sync by checking out the release branch and copying files across before pushing a PR update

## The `examples/` folder — what to trust

`examples/` contains two very different things mixed together:

- **Tom's reference firmwares** (e.g. `sine_wave_lookup`, `reverb`, `passthrough`) — clean, canonical, good to learn from
- **Andy's stable firmwares** (`glitch/`, `chorgan/`) — finished, shipped, reflect Andy's working methodology and are fine to use as style/approach reference
- **Andy's other firmwares** (`spread/`, `markov/`, anything else) — may be in-progress, broken, or mid-refactor; do not use as reference

**For ComputerCard API patterns and DSP techniques**, use Tom's firmwares. Tom's recommended starting points: `sine_wave_lookup` (oscillator/LUT patterns), `20_reverb` (integer DSP), `00_Simple_MIDI`.

**For Andy's coding style and methodology** (state machines, smoothers, holdoff patterns, chord/sequencer logic), `glitch/` and `chorgan/` are the authoritative examples.

## ComputerCard Essentials

- **Sample rate**: 48kHz — `ProcessSample()` called every ~20.8µs
- **Clock**: always `set_sys_clock_khz(144000, true)` first in `main()`
- **Integer only** in `ProcessSample()` — float is ~360ns/op, no FPU on RP2040
- **`__not_in_flash_func`** on all hot functions — runs from RAM not flash cache
- **Global objects** — ComputerCard subclass must be global, not inside `main()` (stack is only 4KB)
- **RAM**: 256KB total. ComputerCard uses ~7KB. Budget carefully for audio buffers.
- **CVIn1/2** returns -2048..2047. Above 0V ≈ > 0 (not > 2047)
- **PulseOut / CVOut**: set once per ProcessSample — not double-buffered

## Inputs/Outputs (ComputerCard v0.3.0)

| Method | Returns | Notes |
|--------|---------|-------|
| `AudioIn1()` / `AudioIn2()` | int16_t −2048..2047 | |
| `CVIn1()` / `CVIn2()` | int16_t −2048..2047 | LPF applied |
| `PulseIn1()` / `PulseIn2()` | bool | |
| `PulseIn1RisingEdge()` | bool | one sample only |
| `KnobVal(Knob::Main/X/Y)` | int32_t 0..4095 | |
| `SwitchVal()` | Switch::Up/Middle/Down | |
| `AudioOut1/2(val)` | int16_t −2048..2047 | |
| `CVOut1/2(val)` | int16_t −2048..2047 | ~−6V to +6V |
| `PulseOut1/2(bool)` | — | ~5V high, 0V low |
| `LedOn(i, bool)` / `LedBrightness(i, val)` | — | val 0..4095 |

## Hard-Won Gotchas (read before starting any new firmware)

### ComputerCard.h — our fork has two critical fixes
The shared `Demonstrations+HelloWorlds/PicoSDK/ComputerCard/ComputerCard.h` has been modified from upstream. **Do not revert these.** Both fixes must be present in any self-contained `releases/<name>/ComputerCard.h` too.

**Fix 1 — ADC alignment race in `BufferFull()`**: Calling `adc_select_input(0)` while a conversion is in-progress lets that conversion complete on the old channel; the result lands as `ADC_Buffer[n][0]`, shifting the 8-slot burst by 1–3 positions. Knobs read as CV, CV reads as audio — causes intermittent freeze or wrong-channel assignments, especially under fast pulse/CV inputs. Fix: stop ADC → wait READY → drain FIFO → select ch0 → re-arm DMA → restart ADC.

**Fix 2 — Power-on click in `AudioWorker()`**: `SPI_Buffer[2][2]` is uninitialised; first DMA transfer sends random RAM to DAC → audible click. Fix: pre-fill both ping-pong buffers with `dacval(0, DAC_CHANNEL_A/B)` before `adc_run(true)`.

### Startup holdoff design
- Run **all smoothers from sample 0** (not after holdoff) so they are fully converged when audio starts
- Holdoff: **9600 samples (~200ms)** — covers ComputerCard's internal knob IIR convergence (~4200 samples) with margin
- Smoother τ: **`>> 6` (64 samples)** — rejects single ADC glitch samples without too much lag
- **No one-shot seed** at holdoff exit — seeding at exactly sample N is vulnerable to a rare ADC glitch at that exact sample injecting a bad value permanently
- **480-sample linear fade-in** after holdoff ends — eliminates click from oscillators starting into a non-zero output

### Switch behaviour
- `SwitchVal()` returns `Switch::Down` (= 0) at boot before ADC settles — **always wait at least 4800 samples before reading switch for mode selection**
- `SwitchVal()` is a **level** — it reads the current position every sample. Use a state machine (armed/timer) for tap vs hold detection
- `PulseIn1RisingEdge()` / `PulseIn2RisingEdge()` detect **edges** (one sample true). `PulseIn1()` / `PulseIn2()` are **levels** (true while high). Use the right one — mixing them up causes missed or repeated triggers
- **PulseIn2 boot guard**: if PU2 is high at boot, `RisingEdge()` never fires on that first edge. But if your code only watches `RisingEdge()`, a high-at-boot input will silently never arm. Add an explicit "must see low first" guard: `if (!PulseIn2()) pu2Armed = true;`
- **Switch::Middle** is the correct enum value for the centre position — NOT `Switch::Mid` (causes a compile error)
- **Switch Down is not a zone**: when the momentary switch is held Down, it is not Up or Middle — if you have zone logic based on switch position, handle `Switch::Down` explicitly or it will fall through to the wrong zone

### Switch tap vs hold — working pattern (from Chorgan)

The Switch Down button must distinguish: short tap (< 1s = cycle preset) vs long hold (≥ 1s = store chord). The working idiom:

```cpp
// Class members:
int32_t switchDownTimer = 0;  // counts samples while Down held
bool    downArmed       = true; // false until switch has been released once
int32_t pendingStoreSlot = -1;  // set at exactly 1s, committed on release

// In ProcessSample(), after boot guard (sampleCount > 4800):
Switch sw = SwitchVal();
if (sw == Switch::Down) {
    switchDownTimer++;
    if (switchDownTimer == 48000 && downArmed)  // exactly 1 second
        pendingStoreSlot = chordWriteIdx;        // mark pending (LED changes here)
} else {
    if (downArmed) {
        if (pendingStoreSlot >= 0) {
            // HOLD: commit store on release
            storeChord();
            pendingStoreSlot = -1;
        } else if (switchDownTimer > 0 && switchDownTimer < 48000) {
            // TAP: short press
            advancePreset();
        }
        downArmed = false;
    }
    if (switchDownTimer == 0) downArmed = true;  // re-arm only after full release
    switchDownTimer = 0;
}
```

Key points:
- The pending action is **committed on release**, not on the threshold crossing — this prevents accidental fires if the user holds slightly too long
- `downArmed` prevents the boot Switch Down press (for mode selection) from being treated as a tap
- LED feedback should change at the 1-second threshold so the user knows to release

### CV In 1 — 1V/oct input scaling (critical, hard-won)

This was a major sticking point in Chorgan development. The correct scaling:

- `CVIn1()` returns −2048..2047. **0V = 0**, not 2048. Above 0V is positive, below is negative.
- **1V/oct**: the hardware gives **409 counts per volt**, so **409 counts = 1 octave = 12 semitones**
- The constant in code is `kCountsPerOctave = 409`
- This was wrong for a long time (341 was used — a common mistake from confusing the 4096-count range with the V/oct scale). The symptom was intervals sounding slightly sharp/flat and the V/oct tracking drifting noticeably over several octaves.
- **To convert CVIn1 to a semitone offset**: `int32_t semi = (CVIn1() * 12) / 409`
- **To add CVIn1 to a knob-based pitch** (summing both): read both, sum, then convert once — don't convert each separately and add.
- `ExpVoct(int32_t in)` takes a value in the combined knob+CV space (0..4095 for the knob range, extended by CV). It uses a 341-entry lookup table (`voct_vals[341]`) where each entry = 1 semitone and the table covers ~C0..C8. Input is clamped before lookup.

### CV and audio scaling
- `CVOut1/2` accepts −2048..2047, maps to **±5V** (not ±6V — the ±6V figure in the API table is the rail, not the useful range)
- **1V/oct on CVOut**: 1V = ~409 counts. 1 semitone = 409/12 ≈ **34 counts**. 12 semitones = +1V = 409 counts
- `CVIn1/2` returns −2048..2047. **0V ≈ 0** (not 2047). Above 0V = positive, below 0V = negative
- Audio outputs clip at ±2047 — always clamp before writing

### LFO / triangle wave
- A naive triangle fold using `>> 17` (15-bit counter, fold at 16384) produces **asymmetric halves** — rising 0→16383, falling 32767→16384. Sounds like a descending ramp
- Correct approach: `>> 16` gives a 16-bit counter (0–65535), fold at 32768, `>> 1` to scale — both halves symmetric 0→16383→0

### White noise generator — correct bit shift

A common mistake when generating white noise from a 32-bit LFSR (`rng_next()`):

```cpp
// WRONG — produces range −1024..+31743, severe positive DC rail
int32_t white = (int32_t)(rng_next() >> 17) - 1024;

// CORRECT — produces ±1024, zero mean
int32_t white = (int32_t)(rng_next() >> 21) - 1024;
```

`>> 17` takes bits 31:17 (15 bits = 0..32767), then subtracts 1024 → range −1024..+31743. Symptom: Y knob seems to do nothing at low values, then suddenly silences audio when noise overwhelms the signal — looks like a knob scaling bug, is actually DC.

`>> 21` takes bits 31:21 (11 bits = 0..2047), subtract 1024 → ±1023, zero mean.

### IIR Hilbert quadrature pair — behavioural SSB shift

For single-sideband frequency shift without a full encode/decode pipeline, use two 4-section 2nd-order all-pass chains with a 90° phase difference. This is the technique used in OffAir for the detuning whistle/shift effect.

- Path I: 4 × 2nd-order all-pass sections (coefficients kHilbA)
- Path B: 4 × 2nd-order all-pass sections (coefficients kHilbB) + 1 sample delay
- I/Q at a given frequency f: `shifted = (I*cos(f) - Q*sin(f)) >> 7`
- Use a phase accumulator for the shift frequency; advance by `kWhistleInc` per sample
- Q14 fixed-point coefficients; each section: `y = a*(x - y_prev) + x_prev` (Schüssler structure)

Key design point: don't try to AM-encode the input and then decode at a detuned carrier — the audible result of detuning is a *combination* of whistle pitch + SSB shift + distortion that's much cheaper to synthesise directly (behavioural model).

### Baked audio — 12-bit packed format

For audio stored in flash (interference clips, broadcast recordings), two encodings work well:

**8-bit offset binary at 8kHz** — interference/noise clips. Compact (8KB/s), mono, unsigned: 128 = silence, 0 = −full, 255 = +full. Unpack: `(int32_t)byte - 128`.

**12-bit packed signed at 11025Hz** — higher-quality broadcast/station clips. 2 samples per 3 bytes (~16.5KB/s):
```
byte0 = A[11:4]
byte1 = (A[3:0]<<4) | B[11:8]
byte2 = B[7:0]
```
Unpack in C:
```cpp
int32_t a = (int32_t)((byte0<<4)|(byte1>>4)); if(a>=2048)a-=4096;
int32_t b = (int32_t)(((byte1&0xF)<<8)|byte2); if(b>=2048)b-=4096;
```

Playback: fractional accumulator per clip. Add `clip_sr` each `ProcessSample()` call; advance index by 1 when accumulator ≥ 48000; subtract 48000. Use `if`, not `while` — a clip playing at 48kHz would never advance more than once per sample anyway.

### Flash budget for baked-audio firmware

OffAir at v1.0.0 uses ~85.5% of the 2MB flash with 6 interference clips + 2 broadcast clips baked in. Rough budgeting:
- 8kHz uint8 mono: **8KB/s**
- 11025Hz 12-bit packed mono: **~16.5KB/s**
- Total flash: 2048KB. RP2040 firmware overhead + code typically ~150–250KB. Budget audio to fit in the remainder.
- `pico_add_extra_outputs` + `target_link_options(... -Wl,--print-memory-usage)` prints flash/RAM usage at link time — watch for > 90% flash as a warning.

### CV loopback — self-patching tuning

Pattern used in OffAir: **CV Out 2 = relative offset** (dial position minus current tuning smoother), so patching CV Out 2 → CV In 1 makes the module self-track to a fixed reference point on the dial.

The key constraint: CV In 1 must be **1:1 gain** (not amplified) and CV Out 2 must output a **difference**, not an absolute position. If either side has gain ≠ 1 or outputs absolute position, the loop will oscillate or not converge.

```cpp
// CV Out 2: relative offset to Station 1 dial position
int32_t b1off = clamp(dialPos[0] - smMain, -2048, 2047);
CVOut2((int16_t)b1off);

// CV In 1: 1:1 addition (no gain)
int32_t tunePos = smMain + CVIn1();
```

### Self-contained releases/ folder
Every `releases/<number>_<name>/` folder intended for Tom's card listing should be **fully self-contained** (see `releases/91_chorgan/` as the reference):
- `ComputerCard.h` — copy of **our fixed version** (not upstream's)
- `CMakeLists.txt` — adapted from `releases/60_markov/CMakeLists.txt` (change `CARD_NAME`)
- `pico_sdk_import.cmake` — copy verbatim from `releases/60_markov/`
- Add a `.gitignore` containing `build/` and `UF2/` — the self-contained build generates hundreds of intermediate files that must not be committed
- Verify by doing a **clean build from the releases folder itself** before pushing

### PR branch for Tom's repo
- Create a branch off `origin/main` (upstream tip), not off our `main`
- Add only `releases/<number>_<name>/` — no CMakeLists changes, no ComputerCard.h changes outside the release folder, no other firmware files
- Single commit, clean message — no references to other firmwares or dev history
- Branch naming convention: `add-<number>-<name>` (e.g. `add-83-chorgan`)

## Existing Firmware in This Repo

### Glitch (`examples/glitch/`) — v1.4
Clock-synced beat-repeater (Glitch mode) + breakbeat slicer (Stutter mode). Mode selected by holding switch at reset.
- Released at: `releases/57_glitch/`
- GitHub release: `uglifruit/Workshop_Computer/releases/tag/glitch-v1.4.0`
- PR to Tom's repo: #159 (open as of 2026-05-27)
- All 6 inputs and 6 outputs used
- Integer-only DSP, 224KB circular buffer (112,000 samples = 2.33s), ~89.5% RAM
- ADC settle gotcha: SwitchVal() reads 0 (= Switch::Down) at boot — wait 4800 samples before reading mode

### Chorgan (`examples/chordseq/`) — v1.1.0
6-voice morphing chord synthesizer with chord extension presets and built-in 8-chord sequencer. Two boot modes: normal (detune/chorus) and slew (portamento). Chord-triggered envelope on PU2/CV2. Audio In 1 = slew speed CV; Audio In 2 = chord inversions.
- Released at: `releases/91_chorgan/`
- GitHub release: `uglifruit/Workshop_Computer/releases/tag/chorgan-v1.1.0`
- PR to Tom's repo: #194 (merged 2026-06-22), v1.1.0 via PR #197 (merged 2026-06-24), info.yaml fix via PR #204 (merged 2026-06-27)
- Integer-only DSP, 6 phase accumulators, V/oct lookup table from Chris Johnson's Utility Pair
- Key lesson: CV In 1 V/oct scaling is 409 counts/octave (not 341) — see CV In 1 section above

### OffAir (`examples/offair/`) — v1.0.0
AM/SW/LW shortwave radio simulator. Behavioural demodulation model (no encode/decode): heterodyne whistle, SSB frequency shift via IIR Hilbert quadrature pair, AM off-tune distortion, noise swell/swish. Two live audio inputs = Station 1/2; baked altboot mode with real numbers-station recordings. Six interference clips baked in flash.
- Released at: `releases/95_offair2/`
- GitHub release: `uglifruit/Workshop_Computer/releases/tag/offair-v1.0.0`
- PR to Tom's repo: #203 (merged 2026-06-27)
- Flash: ~85.5% (baked audio clips dominate). RAM usage modest — no large audio buffers
- Key DSP lessons: SSB shift, IIR Hilbert pair, 12-bit packed audio, white noise generator bug — see sections below

### Renaissance (`examples/spread/`) — v1.0.0
6-voice harmonic spread oscillator. CV2/Knob Y morphs through stacked intervals (unison → m3 → M3 → 5ths → octaves) with landmark snapping. Knob X = detune (when CV1 patched). Main knob = timbre (sine → triangle → saw).
- Released at: `releases/73_renaissance/`
- Integer-only DSP, ~9KB RAM (3.5%)

## Workflow Learned

- **One firmware per commit to main** — no long-lived branches
- **Versioning**: bump version in `info.yaml` and create a new GitHub release for each meaningful update
- **PR updates**: after initial merge to Tom's repo, subsequent updates need a new PR from the same branch
- **GitHub PR diff**: `pull/NNN/files` shows the correct full diff; individual commit links show only that commit
- **UF2 verification**: use `sha256sum` to confirm the file in `releases/` matches the build output before publishing
