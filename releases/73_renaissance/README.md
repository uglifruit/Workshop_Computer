# Renaissance — 6-voice harmonic spread oscillator for Workshop Computer

A 6-voice oscillator that morphs continuously from tight unison chorus through stacked musical intervals to wide octave stacks. A single CV or knob sweeps through the whole range; the spread lands on musical landmarks (minor 3rds, major 3rds, perfect 5ths, octaves) with soft detents that make them easy to target by hand or with slow CV. A separate detune control adds micro-pitch spread and chorus within whatever interval you've landed on. The timbre knob morphs sine to triangle to saw across all six voices simultaneously.

## Installation

1. Connect the Workshop Computer via a data-capable USB cable
2. Power cycle
3. Hold the secret button under the knob on the Workshop Computer, press the reset button, then release the secret button — it mounts as a drive called RPI-RP2
4. Drag and drop `renaissance.uf2` onto the drive
5. The Workshop Computer reboots automatically and Renaissance is running

## Inputs

| Jack | Function |
|------|----------|
| CV In 1 | Root pitch 1V/oct (0V = C4, MIDI 60) — summed with Knob X |
| CV In 2 | Spread amount (0V = unison, +5V = octave stack) — summed with Knob Y |

## Outputs

| Jack | Function |
|------|----------|
| Audio Out 1 | 6-voice mix |
| Audio Out 2 | Same 6 voices with a slight phase offset — subtle stereo width |

## Controls

**CV In 1 + Knob X — Root pitch**
CV In 1 tracks 1V/oct (0V = C4, MIDI 60). Knob X adds a 0–4 octave transpose on top — fully CCW adds nothing, fully CW adds 4 octaves. Use Knob X alone as a coarse pitch control with no CV patched, or keep Knob X at minimum and let CV In 1 drive pitch precisely.

**Knob Y + CV In 2 — Spread**
Knob Y sets the spread position directly (0..4095). CV In 2 is a bipolar offset: 0V = no change, +5V pushes spread up, −5V pulls it down. The combined value sweeps through five interval landmarks with soft flat zones that make each easy to land on:

| Knob Y position | Spread | Sound |
|-----------------|--------|-------|
| Fully CCW (0) | Unison | All voices at same pitch — pure chorus/drone with Switch detune |
| 9 o'clock (¼) | Minor 3rd stack | Voices at 0, 3, 6, 9, 12, 15 semitones — minor colour |
| 12 o'clock (½) | Major 3rd stack | Voices at 0, 4, 8, 12, 16, 20 semitones — bright and open |
| 3 o'clock (¾) | Perfect 5th stack | Voices at 0, 7, 12, 19, 24, 31 semitones — hollow, modal |
| Fully CW (max) | Octave stack | Each voice one octave above the previous — organ-like |

Between landmarks the spread interpolates smoothly; the flat zones give the feeling of soft detents.

**Main Knob — Timbre**
Morphs the waveform of all six voices simultaneously:

| Position | Waveform |
|----------|----------|
| Fully CCW | Sine — smooth, clean |
| 12 o'clock | Triangle — slightly brighter, softer harmonic edge |
| Fully CW | Saw — full harmonic content, buzzy |

**Switch — Detune**
Controls micro-pitch spread across all six voices. Outer voices detune most, inner voices least.

- **Switch Up** — detune off, all voices at exact intervals
- **Switch Mid** — detune on, at the current step level
- **Tap Switch Down** — advances the detune amount one step (cycles through 7 levels). LED 5 shows the current step brightness.

| Step | Outer voice detune |
|------|--------------------|
| 1 | 3 cents |
| 2 | 6 cents |
| 3 | 10 cents |
| 4 | 15 cents |
| 5 | 20 cents |
| 6 | 30 cents |
| 7 | 40 cents |

At unison spread (Knob Y fully CCW) with detune on, the card produces a thick chorus cluster. At octave spread with a low detune step, it adds a gentle shimmer to the stacked octaves.

## LEDs

| LED | Function |
|-----|----------|
| 0–4 | Landmark indicator — the LED nearest the current spread position lights |
| 5 | Switch Up: timbre brightness (dim = sine, bright = saw). Switch Mid/Down: detune step level (dim = step 1, bright = step 7) |

## Quick start

**Drone/pad — no CV needed**
1. Patch Audio Out 1 to a mixer or effects
2. Set Knob Y to 12 o'clock (major 3rd stack), Main Knob to 9 o'clock (triangle)
3. Switch to Mid for light chorus — self-contained lush pad

**Pitched chord voice**
1. Patch a 1V/oct source into CV In 1, leave Knob X centred
2. Knob Y to ¾ (perfect 5th stack) for a modal, open sound
3. Switch Mid for gentle chorus, Main Knob to taste

**Slow spread morphing**
1. Patch a slow LFO or envelope into CV In 2
2. As it rises, the card sweeps from unison through 3rds and 5ths to octaves
3. Set Switch Mid so each landmark sounds lush rather than clinical

**Unison bass**
1. Knob Y fully CCW (unison), Main Knob fully CW (saw), Switch Down
2. Root pitch via CV In 1, Knob X to transpose — thick detuned unison bass or lead

## Technical notes

- 6-phase-accumulator oscillator, all integer DSP, runs from RAM (`__not_in_flash_func`)
- CV1+KnobX and CV2+KnobY are summed directly — no jack detection, no fallback switching
- Spread interpolates phase increments directly between MIDI pitch table entries — the exponential pitch curve is handled correctly at all registers
- Detune uses Q16.16 ratio multiplication, symmetric around each voice's nominal pitch
- AudioOut2 uses a fixed 1/512-cycle phase offset on all voices for stereo width without any pitch deviation
- RAM usage: ~9KB (3.5% of 256KB)

Full source: https://github.com/uglifruit/Workshop_Computer/tree/main/Demonstrations%2BHelloWorlds/PicoSDK/ComputerCard/examples/spread
