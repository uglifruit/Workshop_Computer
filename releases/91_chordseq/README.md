# Chorgan

Six-voice morphing chord synthesizer with built-in chord sequencer for the Workshop Computer.

Knob X and CV In 1 set the root pitch. Knob Y selects an interval above the root in semitone steps. Four extension voices fill out the chord — the extensions are drawn from the same harmonic world as the chosen interval, so a perfect 5th stays open and powerful, a minor 3rd stays dark, a major 3rd stays bright. The Main knob morphs all six voices through sine, triangle, saw, and narrow pulse. A built-in chord sequencer stores up to eight chords and steps through them on rising edges at Pulse In 2.

Two modes are available, selected at boot:

- **Normal mode** (default): detune/chorus across the six voices, animated by an LFO
- **Slew mode** (hold Switch Down at power-on): chord changes glide — all six voices portamento independently to their new pitches

## Inputs

| Jack | Function |
|------|----------|
| CV In 1 | Root pitch 1V/oct (0V = C4) — summed with Knob X |
| CV In 2 | Timbre offset — bipolar, offsets the Main knob position |
| Pulse In 1 | Rising edge advances the chord extension preset |
| Pulse In 2 | Rising edge recalls the next stored chord |

## Outputs

| Jack | Function |
|------|----------|
| Audio Out 1 | 6-voice mix |
| Audio Out 2 | Same 6 voices with per-voice phase offsets — stereo width |
| Pulse Out 1 | Square wave one octave below root |
| Pulse Out 2 | PWM square at root frequency — duty cycle sweeps 30–70% at a rate set by detune amount |
| CV Out 1 | Root pitch + voiced interval (1V/oct) — follows X knob, CV In 1, and chord override |
| CV Out 2 | Triangle LFO at the same rate as Pulse Out 2 — ±5V, 0V when detune is off |

## Controls

**Knob X + CV In 1 — Root pitch**
Knob X sweeps from C3 to C6. CV In 1 tracks 1V/oct on top of that. Both are summed and applied uniformly to all six voices.

**Knob Y — Interval**
Sets the interval between voice 1 (root) and voice 2 in audible integer semitone steps, 0 (unison) to 12 (octave). Voices 3–6 are chord extensions above the root, chosen by the current preset.

**Main Knob + CV In 2 — Timbre**
Morphs the waveform of all six voices simultaneously. The curve is a W-shape:

| Position | Waveform |
|----------|----------|
| Fully CCW | Narrow pulse |
| 9 o'clock | Sine |
| 12 o'clock | Saw (fullest) |
| 3 o'clock | Sine |
| Fully CW | Narrow pulse |

CV In 2 offsets the knob position bipolarly but cannot push the timbre into a different detune/slew zone — that is always determined by the physical knob position.

**Switch + Main Knob position — Detune (normal mode) / Slew rate (slew mode)**

In normal mode, four detune levels are selected by switch position and whether the Main knob is CCW or CW of centre:

| Switch | Knob position | Outer voice detune |
|--------|--------------|-------------------|
| Mid | CCW of centre | 0 cents (clean) |
| Mid | CW of centre | 5 cents |
| Up | CCW of centre | 15 cents |
| Up | CW of centre | 10 cents |

In slew mode, the same four zones set the portamento rate:

| Switch | Knob position | Slew rate |
|--------|--------------|-----------|
| Mid | CCW of centre | Instant |
| Mid | CW of centre | Fast (~10ms) |
| Up | CW of centre | Slow (~85ms) |
| Up | CCW of centre | Glacial (~340ms) |

**Tap Switch Down — Cycle preset**
A short tap (less than one second) advances the chord extension preset for voices 3–6. There are 6 presets per interval, cycling through different harmonic choices. A rising edge on Pulse In 1 does the same thing.

## Chord extension presets

Each of the 13 Y positions (0–12 semitones) has 6 extension presets. Extensions are chosen to reinforce the harmonic character of the chosen interval — no minor 3rds if you've picked a major 3rd, no 3rds at all if you've picked a 5th or unison.

| Y | Interval | Harmonic world |
|---|----------|----------------|
| 0 | Unison | 5ths and octaves only |
| 1 | Min 2nd | Clusters — semitone stacks, tritones, m7ths |
| 2 | Maj 2nd | Sus2 — 2nds, 4ths, 5ths, 9ths |
| 3 | Min 3rd | Minor — m3, P5, m7 |
| 4 | Maj 3rd | Major — M3, P5, M7, 9th |
| 5 | Perf 4th | Sus4/quartal — 4ths, 5ths, octaves |
| 6 | Tritone | Dim/aug — tritones, m3, M6 |
| 7 | Perf 5th | Power chord — 5ths, octaves, 9th |
| 8 | Min 6th | Minor/Phrygian — m3, P5, m6, m7 |
| 9 | Maj 6th | Major/pentatonic — M3, P5, M6 |
| 10 | Min 7th | Dominant/blues — P5, m7, 9th |
| 11 | Maj 7th | Lydian/maj7 — M3, P5, M7, #4 |
| 12 | Octave | Quartal/suspended — 4ths, 5ths, 2nds |

## Boot modes

**Normal mode** — power on with Switch in any position except Down. LEDs sweep 0→5 at startup.

**Slew mode** — hold Switch Down before powering on, hold until LEDs flash (all on, then all off, ~200ms). Detune is disabled; chord changes glide instead.

## Chord sequencer

**Hold Switch Down (1 second) — Store chord**
Hold Switch Down until the LEDs change (one second). Release immediately — the chord is written to the next slot. The LED pattern shows which slot was just stored (LEDs 1, 3, 5 full bright; LEDs 0, 2, 4 encode the slot number in binary).

Up to eight chords can be stored. Storing more than eight overwrites from the beginning.

**Recalling chords — Pulse In 2**
Each rising edge at Pulse In 2 recalls the next chord in sequence, stepping through slots in the order they were stored and wrapping at the end. When a chord is held, LEDs 0, 2, 4 are full bright and LEDs 1, 3, 5 encode the recalled slot in binary.

Note: Pulse In 2 must be seen low at least once before it will respond (boot guard).

**Breaking out of a held chord**
Move Knob X or CV In 1 by more than one semitone from where they were when the chord was recalled, or move Knob Y to a different step. The override releases immediately and you return to manual control.

**Advancing preset while a chord is held**
Tapping Switch Down or sending a rising edge to Pulse In 1 advances the preset while the chord override is active — it does not break the override.

## LEDs

| State | LED pattern |
|-------|-------------|
| Boot (normal mode) | Single LED sweeps 0→5 |
| Boot (slew mode) | All LEDs flash on then off |
| Storing (hold active) | LEDs 1, 3, 5 full bright; LEDs 0, 2, 4 = slot number in binary |
| Chord held | LEDs 0, 2, 4 full bright; LEDs 1, 3, 5 = recalled slot in binary |
| Normal | LEDs 0–4: interval position; LED 5: preset brightness |

In normal operation, LEDs 0–4 show where Knob Y is across the 0–12 semitone range. LED 5 brightness indicates the current preset (dim = preset 0, bright = preset 5).

## Voice layout

- Voice 1: always root
- Voice 2: root + Y interval (0–12 semitones)
- Voices 3–6: chord extensions above root, selected by current preset

## Quick start

**Self-contained pad**
1. Patch Audio Out 1 to a mixer or effects
2. Knob Y to 7 (perfect 5th), Main Knob to 12 o'clock (saw — fullest sound)
3. Switch Mid, Main Knob slightly CW for gentle 5-cent detune

**Pitched chord voice**
1. Patch 1V/oct into CV In 1
2. Knob Y to 4 (major 3rd), tap Switch Down to cycle presets
3. Main Knob to taste — 12 o'clock for saw, 9/3 o'clock for sine, fully CCW/CW for pulse

**Slow timbre sweep**
1. Patch a slow LFO into CV In 2
2. The timbre morphs symmetrically around the current Main knob position

**Chord sequence from a clock**
1. Set Pitch (X + CV In 1), Interval (Y), and preset (Switch Down taps) for your first chord
2. Hold Switch Down for one second to store it
3. Repeat for up to eight chords
4. Patch a clock or gate sequence into Pulse In 2 — steps through your stored chords on each rising edge
5. Move Knob X or Y by more than a semitone to drop back to manual

**Portamento chord changes (slew mode)**
1. Boot with Switch Down held
2. Set Switch Mid, Knob CW of centre for fast slew; Switch Up, Knob CCW for glacial
3. Tap Switch Down or trigger Pulse In 1 to change preset — pitches glide to the new chord

**Sub-bass layer**
1. Patch Pulse Out 1 to a VCA or filter — clean square one octave below root
2. Patch Pulse Out 2 for a PWM version — duty cycle animates with detune amount

**Interval-keyed filter or oscillator**
1. Patch CV Out 1 to a filter cutoff or a second oscillator's 1V/oct input
2. The CV tracks the voiced interval in semitones — use it to transpose an external voice in harmony

**Chorus depth animation**
1. Patch CV Out 2 to a VCA or delay time CV input
2. The triangle LFO rate scales with your detune setting and is 0V when detune is off

## Technical notes

- 6-phase-accumulator oscillators, integer arithmetic throughout (RP2040 has no FPU)
- Tuning ratio applied uniformly to all voices — no per-voice stepping on X knob or CV In 1
- Detune zone determined by physical Main knob position; CV In 2 cannot cross zone boundaries
- Stereo width via per-voice phase offsets on Audio Out 2 (0°, 15°, 30°, 45°, 60°, 75°)
- Waveform: W-shape — pulse (CCW extreme) → sine (9 o'clock) → saw (12 o'clock) → sine (3 o'clock) → pulse (CW extreme)
- Pulse Out 2 PWM: LFO rate proportional to detune amount — at 0 cents detune, duty is static at 50%
- CV Out 1: (X knob + CV In 1) + voiced interval in 1V/oct — clamped to ±5V; follows chord override
- CV Out 2: triangle LFO sharing phase with Pulse Out 2 — 0V at zero detune, ±5V peak at maximum
- Chord sequencer: stores pitch, interval, and preset — up to 8 chords; override breaks on >1 semitone movement from position at recall time
- Slew mode: per-voice IIR portamento on phase increment; rates: instant / ~10ms / ~85ms / ~340ms
- 200ms startup holdoff before audio begins — eliminates power-on click

## Credits

By Andy Jenkinson ([uglifruit](https://github.com/uglifruit)), developed with [Claude Code](https://claude.ai/code).

Built on the [Workshop Computer](https://github.com/TomWhitwell/Workshop_Computer) platform by Tom Whitwell (Music Thing Modular), using the ComputerCard framework.

V/oct lookup table (`voct_vals`) from [Utility Pair](https://github.com/chrisgjohnson/Utility-Pair) by Chris Johnson, used with permission under the original licence.


Waveform morphing concept inspired by [Mutable Instruments Braids](https://mutable-instruments.net/modules/braids/) (Émilie Gillet).

Name and concept inspired by the [Music Thing Modular Chord Organ](https://github.com/TomWhitwell/Chord-Organ).

---
Licensed under [Creative Commons Attribution-ShareAlike 4.0](https://creativecommons.org/licenses/by-sa/4.0/).
