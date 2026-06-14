# Chorgan — 6-voice chord organ and chord sequencer for Workshop Computer

Six oscillator voices tuned to a chord you shape in real time. Knob X and CV1 sets the root pitch,Knob Y sets the interval between the root and a second voice in semitone steps; four more voices fill out extensions (5ths, 7ths, 9ths, octaves) that you cycle through with the switch. The main knob morphs timbre from sine through triangle to saw. A detune control adds beating and chorus across all voices.

A built-in chord sequencer stores up to eight chords and plays them back on rising edges at Pulse In 2.

## Installation

1. Connect the Workshop Computer via a data-capable USB cable
2. Follow usual protocol for mounting as a drive called RPI-RP2
3. Drag and drop `chorgan.uf2` onto the drive
4. The Workshop Computer reboots automatically and Chorgan is running

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
| Pulse Out 1 | Square wave one octave below root voice |
| Pulse Out 2 | PWM square at the same frequency — duty cycle sweeps 30–70% at a rate set by detune amount |
| CV Out 1 | Root pitch + voiced interval (1V/oct) — follows X knob, CV In 1, and chord override |
| CV Out 2 | Triangle LFO at the same rate as Pulse Out 2 — ±5V, 0V when detune is off |

## Controls

**Knob X + CV In 1 — Root pitch**
Knob X is a master tune control, sweeping ±12 semitones continuously. CV In 1 tracks 1V/oct on top of that. Both are summed and applied uniformly to all six voices.

**Knob Y — Interval**
Sets the interval between voice 1 (root) and voice 2 in audible integer semitone steps, 0 (unison) to 12 (octave).  (Voices 3–6 are chord extensions above the root, chosen by the current preset - see Tap Switch Down - Cycle Preset).

**Main Knob + CV In 2 — Timbre**
Morphs the waveform of all six voices simultaneously. The curve is a V-shape: the centre position gives the fullest, brightest sound; the edges give the smoothest.

| Position | Waveform |
|----------|----------|
| Fully CCW | Sine |
| 9 o'clock | Triangle |
| 12 o'clock | Saw (fullest) |
| 3 o'clock | Triangle |
| Fully CW | Sine |

CV In 2 offsets the knob position bipolarly but cannot push the timbre into a different detune zone (see below) — the detune zone is always determined by the physical knob position.

**Switch + Main Knob position — Detune**
Four detune levels selected by switch position and whether the main knob is CCW or CW of centre:

| Switch | Knob position | Outer voice detune |
|--------|--------------|-------------------|
| Mid | CCW of centre | 0 cents (clean) |
| Mid | CW of centre | 6 cents |
| Up | CCW of centre | 12 cents |
| Up | CW of centre | 18 cents |

Detune is applied symmetrically — outer voices detune most, inner voices least. At unison interval with detune on, the card produces a thick chorus cluster.

**Tap Switch Down — Cycle preset**
A short tap (less than one second) advances the chord extension preset for voices 3–6. There are 6 presets per interval, cycling through different harmonic choices (triads, 7ths, 9ths, open voicings). A rising edge on Pulse In 1 does the same thing.



## Chord sequencer

**Hold Switch Down (1 second) — Store chord**
Hold switch Down until the LEDs change (one second). Release immediately — the chord is written to the next slot. The LED pattern shows which slot number was just stored (LEDs 0, 2, 4 encode the slot in binary; LEDs 1, 3, 5 are full bright as a visual anchor).

**Recalling chords**
After that, each rising edge at Pulse In 2 recalls the next chord in sequence, stepping through slots in the order they were stored, wrapping around. When a chord is held, the LEDs show the held pattern (LEDs 0, 2, 4 full bright; LEDs 1, 3, 5 encode the slot in binary).

Note: Pulse In 2 must be seen low at least once before it will respond (boot guard). 

**Breaking out of a held chord**
Move Knob X or Knob Y by more than one semitone from where they were when the chord was recalled. The override releases immediately and you return to manual control.

**Advancing preset while a chord is held**
Tapping switch Down or sending a rising edge to Pulse In 1 advances the preset while the chord override is active — it does not break the override.

## LEDs

| State | LED pattern |
|-------|-------------|
| Storing | LEDs 1, 3, 5 full bright; LEDs 0, 2, 4 = slot number in binary |
| Chord held | LEDs 0, 2, 4 full bright; LEDs 1, 3, 5 = recalled slot in binary |
| Normal | LEDs 0–4: interval position; LED 5: preset brightness |

In normal mode, LEDs 0–4 show where the Y knob is across the 0–12 semitone range (one LED lit nearest the current position). LED 5 brightness indicates the preset: dim = preset 0, bright = preset 5.

## Voice layout

- Voice 1: always root
- Voice 2: root + Y interval (0–12 semitones)
- Voices 3–6: chord extensions, cycled by tapping switch Down or rising edge on Pulse In 1

## Chord extension presets

Each of the 13 Y positions (0–12 semitones) has 6 extension presets to explore. A few examples:

| Y (interval) | Preset | Character |
|-------------|--------|-----------|
| 0 (unison) | 0 | Root + 5th + octave stack |
| 0 (unison) | 2 | Open 5ths |
| 4 (major 3rd) | 0 | Major triad + octave |
| 4 (major 3rd) | 1 | Major 7th |
| 7 (perfect 5th) | 0 | Power chord wide |
| 7 (perfect 5th) | 1 | Major triad |
| 7 (perfect 5th) | 2 | Dominant 7th |
| 11 (major 7th) | 4 | Maj7#11 — lydian colour |

## Quick start

**Self-contained pad**
1. Patch Audio Out 1 to a mixer or effects
2. Knob Y to 7 (perfect 5th), Main Knob to 12 o'clock (saw — fullest sound)
3. Switch to Mid, Main Knob slightly CW for gentle 6-cent detune

**Pitched chord voice**
1. Patch 1V/oct into CV In 1
2. Knob Y to 4 (major 3rd), tap switch Down to cycle presets
3. Main Knob to taste — 12 o'clock for saw, fully CCW/CW for clean sine

**Slow timbre sweep**
1. Patch a slow LFO into CV In 2
2. The timbre morphs symmetrically around the current Main knob position — from whatever the knob sets toward sine at both extremes of the LFO

**Chord sequence from a clock**
1. Patch a clock or gate sequence into Pulse In 2
2. Hold switch Down for 1 second at each chord you want to store (set Pitch (X+CV1), Interval (Y) and chord extension (switch Down taps) first)
3. The sequence steps through your stored chords on each rising edge on Pulse In 2
4. Move Knob X or Y to drop back to manual between steps

**Sub-bass layer**
1. Patch Pulse Out 1 to a VCA or filter — clean square one octave below root
2. Patch Pulse Out 2 for a PWM version — duty cycle animates with detune amount; at 0 cents detune it sits static at 50%

**Interval-keyed filter or oscillator**
1. Patch CV Out 1 to a filter cutoff or a second oscillator's 1V/oct input
2. As you tap through presets or trigger chord recall, the CV steps in semitones tracking the voiced interval — use it to transpose an external voice in harmony with whatever Chorgan is playing

**Chorus depth animation**
1. Patch CV Out 2 to a VCA or delay time CV input
2. The triangle LFO rate scales with your detune setting (faster at higher detune), and is at 0V when detune is off — the modulation automatically breathes with the sound

**Chord drone with timbre automation**
1. Store 3–4 different chords (different X, Y positions and presets)
2. Patch a slow clock into Pulse In 2 to step through them
3. Patch a separate LFO into CV In 2 for continuous timbre movement across chord changes
4. Patch a gate sequence into Pulse In 1 to add chord extensions

## Technical notes

- 6-phase-accumulator oscillators, all integer DSP, runs from RAM (`__not_in_flash_func`)
- Tuning ratio applied uniformly to all voices — no per-voice stepping on X knob or CV1
- Detune zone (0/6/12/18 cents) determined by physical main knob position; CV In 2 cannot cross zone boundaries
- Stereo width via per-voice phase offsets on Out 2 (0°, 15°, 30°, 45°, 60°, 75°)
- Pulse Out 2 PWM: LFO rate proportional to detune amount — at 0 cents detune, duty is static at 50%
- CV Out 1: (X knob + CV In 1 tuning offset) + (voiced interval) in 1V/oct — 34 counts/semitone, clamped to ±5V; follows chord override for the interval component
- CV Out 2: symmetric triangle LFO sharing the same phase accumulator as Pulse Out 2 — 0V when detuneAmt = 0, ±5V peak at maximum detune; both outputs always track each other's rate
- Chord sequencer stores tuning ratio, interval, and preset — up to 8 chords
- PulseIn2 arm guard: must be seen low before first rising edge is accepted (prevents boot glitch)
- 200ms startup holdoff before audio begins — eliminates power-on glitch


Full source: https://github.com/uglifruit/Workshop_Computer/tree/main/Demonstrations%2BHelloWorlds/PicoSDK/ComputerCard/examples/chorgan

## Credits

By Andy Jenkinson ([uglifruit](https://github.com/uglifruit)), developed with Claude Code.

Built on the [Workshop Computer](https://github.com/TomWhitwell/Workshop_Computer) platform by Tom Whitwell, using the ComputerCard framework.

Waveform morphing approach inspired in part by [Mutable Instruments Braids](https://mutable-instruments.net/modules/braids/).

---
Licensed under [Creative Commons Attribution-ShareAlike 4.0](https://creativecommons.org/licenses/by-sa/4.0/).
