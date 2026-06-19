# ChordSeq

8-oscillator chord synthesizer with morphing waveform shape.

Eight oscillators are split into two groups of four — four play the root pitch, four play a selectable interval above the root. The waveform morphs continuously from sawtooth through triangle to sine, and phase offsets between oscillators create stereo width.

## Controls

| Control | Function |
|---------|----------|
| Knob X | Root pitch (C3–C6) |
| Knob Y | First interval above root — 13 steps, unison to octave |
| Main Knob | Oscillator shape — CCW = sawtooth, mid = triangle, CW = sine-ish |
| CV In 1 | V/oct pitch offset |
| Audio Out 1 | Left channel |
| Audio Out 2 | Right channel (same oscillators, extra phase offset for width) |

## How it works

- **8 oscillators**: oscs 0–3 play the root, oscs 4–7 play root + interval
- **Stereo**: all 8 oscillators appear on both outputs with evenly distributed phase offsets; the right channel adds an additional half-step offset for width
- **Shape morph**: Main knob 0→mid crossfades saw→triangle; mid→max crossfades triangle→sine-ish (integer cubic softening)
- **Interval**: Y knob selects one of 13 equal semitone steps (unison, min 2nd … octave)
- **V/oct**: CV In 1 tracks standard 1V/oct

## Status

Early release — chord voicing beyond the first interval is not yet implemented.
