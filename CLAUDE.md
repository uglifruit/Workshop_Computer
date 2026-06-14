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

### CV and audio scaling
- `CVOut1/2` accepts −2048..2047, maps to **±5V** (not ±6V — the ±6V figure in the API table is the rail, not the useful range)
- **1V/oct on CVOut**: 1V = ~409 counts. 1 semitone = 409/12 ≈ **34 counts**. 12 semitones = +1V = 409 counts
- `CVIn1/2` returns −2048..2047. **0V ≈ 0** (not 2047). Above 0V = positive, below 0V = negative
- Audio outputs clip at ±2047 — always clamp before writing

### LFO / triangle wave
- A naive triangle fold using `>> 17` (15-bit counter, fold at 16384) produces **asymmetric halves** — rising 0→16383, falling 32767→16384. Sounds like a descending ramp
- Correct approach: `>> 16` gives a 16-bit counter (0–65535), fold at 32768, `>> 1` to scale — both halves symmetric 0→16383→0

### Self-contained releases/ folder
Every `releases/<number>_<name>/` folder intended for Tom's card listing should be **fully self-contained** (see `releases/83_chorgan/` as the reference):
- `ComputerCard.h` — copy of **our fixed version** (not upstream's)
- `CMakeLists.txt` — adapted from `releases/60_markov/CMakeLists.txt` (change `CARD_NAME`)
- `pico_sdk_import.cmake` — copy verbatim from `releases/60_markov/`
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
