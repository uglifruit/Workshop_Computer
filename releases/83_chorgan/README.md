# Chorgan — 6-voice harmonic chord oscillator for Workshop Computer

Experimental sandbox branched from Renaissance. Same core engine — 6-voice oscillator with chord extensions — used as a workbench for new ideas.

## Installation

1. Connect the Workshop Computer via a data-capable USB cable
2. Power cycle
3. Hold the secret button under the knob, press the reset button, then release — it mounts as a drive called RPI-RP2
4. Drag and drop `chorgan.uf2` onto the drive
5. The Workshop Computer reboots automatically

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
Knob X sweeps ±12 semitones continuously. CV In 1 tracks 1V/oct on top. Both summed and applied uniformly to all six voices.

**Knob Y — Interval**
Sets the interval between voice 1 (root) and voice 2 in integer semitone steps, 0 (unison) to 12 (octave).

**Main Knob + CV In 2 — Timbre**
Morphs the waveform of all six voices. CV In 2 offsets bipolarly but the detune zone is always determined by the physical knob position.

| Position | Waveform |
|----------|----------|
| Fully CCW | Saw |
| 9 o'clock | Triangle |
| 12 o'clock | Sine |
| 3 o'clock | Triangle |
| Fully CW | Saw |

**Switch + Main Knob — Detune**

| Switch | Knob | Outer voice detune |
|--------|------|-------------------|
| Mid | CCW | 0 cents |
| Mid | CW | 5 cents |
| Up | CCW | 10 cents |
| Up | CW | 16 cents |

**Tap Switch Down — Cycle preset**
Advances the chord extension preset for voices 3–6 (6 presets per interval). Pulse In 1 does the same.

## LEDs

| LED | Function |
|-----|----------|
| 0–4 | Interval indicator |
| 5 | Preset level — dim = preset 0, bright = preset 5 |

## Technical notes

- 6-phase-accumulator oscillators, all integer DSP, runs from RAM
- Branched from Renaissance v1.0.0 — experimental, subject to change

Full source: https://github.com/uglifruit/Workshop_Computer/tree/main/Demonstrations%2BHelloWorlds/PicoSDK/ComputerCard/examples/chorgan
