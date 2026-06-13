# Chorgan — 6-voice harmonic chord oscillator for Workshop Computer

Six oscillator voices tuned to a chord you shape in real time. The Y knob sets the interval between the root and a second voice in semitone steps; the other four voices fill out extensions (5ths, 7ths, 9ths, octaves) that you cycle through with the switch. The main knob morphs the timbre of all voices simultaneously from saw through triangle to sine. A detune control adds beating and chorus across all voices.

## Installation

1. Connect the Workshop Computer via a data-capable USB cable
2. Power cycle
3. Hold the secret button under the knob, press the reset button, then release — it mounts as a drive called RPI-RP2
4. Drag and drop `chorgan.uf2` onto the drive
5. The Workshop Computer reboots automatically and Chorgan is running

## Inputs

| Jack | Function |
|------|----------|
| CV In 1 | Root pitch 1V/oct (0V = C4) — summed with Knob X |
| CV In 2 | Timbre offset — bipolar, offsets the Main knob position |
| Pulse In 1 | Rising edge advances the chord extension preset |

## Outputs

| Jack | Function |
|------|----------|
| Audio Out 1 | 6-voice mix |
| Audio Out 2 | Same 6 voices with per-voice phase offsets — stereo width |
| Pulse Out 1 | Square wave one octave below root voice |
| Pulse Out 2 | PWM square at same frequency — duty cycle sweeps 30–70% at a rate set by detune amount |

## Controls

**Knob X + CV In 1 — Root pitch**
Knob X is a master tune control, sweeping ±12 semitones continuously. CV In 1 tracks 1V/oct on top of that. Both are summed and applied uniformly to all six voices — tuning never causes any voice to step independently.

**Knob Y — Interval**
Sets the interval between voice 1 (root) and voice 2 in integer semitone steps, 0 (unison) to 12 (octave). Voices 3–6 are chord extensions above the root, chosen by the current preset.

**Main Knob + CV In 2 — Timbre**
Morphs the waveform of all six voices simultaneously. CV In 2 offsets the knob position bipolarly but cannot push the timbre into a different detune zone — the detune zone is always determined by the physical knob position.

| Position | Waveform |
|----------|----------|
| Fully CCW | Saw |
| 9 o'clock | Triangle |
| 12 o'clock | Sine |
| 3 o'clock | Triangle |
| Fully CW | Saw |

**Switch + Main Knob position — Detune**
Four detune levels selected by switch position and whether the main knob is CCW or CW of centre:

| Switch | Knob position | Outer voice detune |
|--------|--------------|-------------------|
| Mid | CCW of centre | 0 cents (clean) |
| Mid | CW of centre | 5 cents |
| Up | CCW of centre | 10 cents |
| Up | CW of centre | 16 cents |

Detune is applied symmetrically — outer voices detune most, inner voices least. At unison interval with detune on, the card produces a thick chorus cluster.

**Tap Switch Down — Cycle preset**
Advances the chord extension preset for voices 3–6. There are 6 presets per interval, cycling through different harmonic choices (triads, 7ths, 9ths, open voicings, etc.). LED 5 shows the current preset brightness. A rising edge on Pulse In 1 does the same thing.

## LEDs

| LED | Function |
|-----|----------|
| 0–4 | Interval indicator — lights the LED nearest the current Y knob semitone position |
| 5 | Preset level — dim = preset 0, bright = preset 5 |

## Voice layout

- Voice 1: always root
- Voice 2: root + Y interval (0–12 semitones)
- Voices 3–6: chord extensions, cycled by tapping switch Down or a rising edge on Pulse In 1

## Chord extension presets

Each of the 13 Y positions (0–12 semitones) has 6 extension presets. A few examples:

| Y (interval) | Preset | Character |
|-------------|--------|-----------|
| 0 (unison) | 0 | Root + 5th + octave stack |
| 0 (unison) | 1 | Wide octave stack |
| 4 (major 3rd) | 0 | Major triad + octave |
| 4 (major 3rd) | 1 | Major 7th |
| 7 (perfect 5th) | 0 | Power chord wide |
| 7 (perfect 5th) | 1 | Major triad |
| 7 (perfect 5th) | 2 | Dominant 7th |
| 11 (major 7th) | 4 | Maj7#11 — lydian colour |

## Quick start

**Self-contained pad**
1. Patch Audio Out 1 to a mixer or effects
2. Knob Y to 7 semitones (perfect 5th), Main Knob to 12 o'clock (sine)
3. Switch to Mid, Knob X slightly CW for gentle detune — lush open chord

**Pitched chord voice**
1. Patch 1V/oct into CV In 1
2. Knob Y to 4 (major 3rd), tap switch Down to cycle presets until you find the voicing you want
3. Main Knob to taste — saw for fullness, sine for clean

**Pulse sub-bass**
1. Patch Pulse Out 1 to a VCA or filter — clean square one octave below root
2. Patch Pulse Out 2 for a PWM version of the same — duty cycle animates with detune amount

**Slow timbre sweep**
1. Patch a slow LFO into CV In 2
2. As it sweeps, the timbre morphs through saw/triangle/sine and back while the chord stays fixed

## Technical notes

- 6-phase-accumulator oscillators, all integer DSP, runs from RAM (`__not_in_flash_func`)
- Tuning ratio applied uniformly to all voices — no per-voice stepping on X knob or CV1
- Detune zone (0/5/10/16 cents) determined by physical main knob position; CV In 2 cannot cross zone boundaries
- Stereo width via per-voice phase offsets on Out 2 (0°, 15°, 30°, 45°, 60°, 75°)
- Pulse Out 2 PWM: LFO rate proportional to detune amount — at 0 cents detune, duty is static at 50%
- RAM usage: ~9.6KB (3.8% of 256KB)

Full source: https://github.com/uglifruit/Workshop_Computer/tree/main/Demonstrations%2BHelloWorlds/PicoSDK/ComputerCard/examples/chorgan
