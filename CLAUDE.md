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
