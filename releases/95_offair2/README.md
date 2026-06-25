# OffAir 2

Shortwave radio simulator for the Workshop Computer.

OffAir 2 lets you tune across a virtual band the way you tune a real shortwave
receiver. As you approach a station you hear the signature heterodyne whistle slide
in pitch, the audio pull into tune, and the background static duck away. Tune past it
and the audio garbles, the whistle rises again, and it fades back into the noise.

Rather than literally modulating and demodulating an RF carrier (which can't give
clean selectivity in cheap integer DSP), OffAir 2 **synthesises the audible result**
of detuning each station directly from how far off-tune you are:

- a **heterodyne whistle** whose pitch slides with detune — the signature SW sound;
- on **SW/LW**, the audio is **single-sideband frequency-shifted** by the detune
  amount, so harmonics break and voices go "wrong pitch" / metallic — exactly the
  near-tuned SSB sound (this is genuine product-detector behaviour: a detuned local
  oscillator shifts the recovered audio);
- on **AM**, the audio stays at correct pitch (envelope detection) but distorts as
  you detune;
- the station fades out as you move off, and at zero-beat you get clean audio with no
  whistle.

Two broadcast stations plus three interference signals (morse / data / numbers) are
scattered across the dial and **re-randomised on every band change** — so the layout
is different each time and stations sometimes overlap, like a crowded band.

## Bands

Tap the switch **Down** to cycle **AM → SW → LW**. The band sets both the noise
character and the demodulation behaviour:

| Band | Demod near a station | Noise character |
|------|----------------------|-----------------|
| AM | Correct-pitch audio, distorts off-tune (envelope detection) | Mellow mid hiss, stable |
| SW | Directional pitch-shift (SSB) | Bright crackly wash, fades |
| LW | Directional pitch-shift (SSB) | Deep rumble, heavy fade |

The dial layout (station and interference positions, and which clips play) is
re-randomised each time you change band.

## Inputs

| Jack | Function |
|------|----------|
| Audio In 1 | Station 1 source (live audio, normal boot) |
| Audio In 2 | Station 2 source (live audio, normal boot) |
| CV In 1 | Tuning — full range, bipolar. Patch an LFO or sequencer to scan the dial hands-free |
| CV In 2 | Noise level — adds to Knob Y (voltage-controlled static) |
| Pulse In 1 | Rising edge — re-randomise the station / interference layout |
| Pulse In 2 | Rising edge — trigger a one-shot from the curated one-shot bank |

## Outputs

| Jack | Function |
|------|----------|
| Audio Out 1 | Full mix — tuned audio, whistles, static, one-shot bursts |
| Audio Out 2 | Noise / static only |
| CV Out 1 | Signal strength — an envelope that rises as you tune onto a station |
| CV Out 2 | Tuning position — mirrors the dial as CV |
| Pulse Out 1 | Gate HIGH while on any station |
| Pulse Out 2 | Short trigger each time you newly lock onto a station |

## Controls

**Main Knob — Tuning.** Scans across the dial. Sums with CV In 1.

**Knob X — IF bandwidth.** Sets both how wide a station's capture window is and the
audio brightness. Fully CCW = narrow, muffled and selective (fiddly to tune); fully
CW = wide, bright and easy to find.

**Knob Y — Noise level.** Silent fully CCW; raises the static floor toward full CW.
The static slowly swells and swishes on its own (random-walk level and filter
sweeps), heaviest on LW. Noise ducks away when you tune onto a station — fully on AM,
strongly on SW/LW. CV In 2 adds to this.

**Switch Down tap — Cycle band.** AM → SW → LW, re-randomising the layout each time.

## One-shot bank

Pulse In 2 triggers a short event from a curated one-shot bank — clicks, dropouts,
crashes, single morse bursts: things that make sense heard in isolation. Each trigger
plays a random clip once through and stops; re-triggering restarts it. The one-shot
bank is separate from the looping interference clips, so the two can be curated
independently (see *Building* below). If the bank is empty the firmware falls back to
the looping interference clips.

## Boot modes

**Normal mode** — power on with the switch in any position other than Down (or release
it within the first ~200 ms). The two live audio inputs become the broadcast stations.

**Broadcast mode (altboot)** — hold the switch Down at power-on until all six LEDs
flash. The two live inputs are replaced by baked broadcast recordings, turning the
module into a self-contained radio. Everything else works identically.

## LEDs

| LED | Function |
|-----|----------|
| 0 | Station 1 signal strength |
| 1 | Station 2 signal strength |
| 2 / 3 | Band — both off = AM, LED 2 = SW, LED 3 = LW |
| 5 | Tuning position |

## Technical notes

- Behavioural demodulation: per-station detune drives a sliding heterodyne whistle, an
  SSB frequency-shifter (IIR Hilbert quadrature pair, SW/LW) or envelope detection
  with off-tune distortion (AM), and a strength fade. All processing stays in the
  audio band, so there is no aliasing.
- Integer-only DSP, runs on core 1; an RF PWM path runs on core 0.
- Interference / one-shot clips: 8 kHz unsigned 8-bit mono.
- Broadcast clips: 11025 Hz 12-bit packed signed (2 samples per 3 bytes).
- Looping interference clips are trimmed to ≤12 s to leave flash headroom for the
  one-shot bank; broadcast clips are 25 s.
- 200 ms startup holdoff before audio begins, with a short linear fade-in.

## Building

Source: `main.cpp`, `clips.h` (baked audio), `convert_clips.py` (audio generator).

To change the baked audio, edit `convert_clips.py` and re-run it to regenerate
`clips.h`:

- **Looping interference** and **broadcast** clips are listed explicitly in the
  `CLIPS` table (filename, start, length, etc.). `LOOP_MAX_SEC` caps the loop length.
- **One-shots** are auto-discovered: drop curated short files (WAV / MP3 / AIFF /
  FLAC) into the `Oneshots/` folder; the script converts them all and builds the
  one-shot bank automatically, warning if you exceed the flash budget.

Then build the firmware with the Pico SDK (CMake / Ninja) and flash the resulting
`offair2.uf2`.

## Credits

By Andy Jenkinson ([uglifruit](https://github.com/uglifruit)), developed with
[Claude Code](https://claude.ai/code).

Built on the [Workshop Computer](https://github.com/TomWhitwell/Workshop_Computer)
platform by Tom Whitwell (Music Thing Modular), using the ComputerCard framework.

Inspired by [RadioMusic](https://github.com/TomWhitwell/RadioMusic) by Tom Whitwell
and the [Music Thing Radio Music workshop](https://dyski.co/Tom-Whitwell-Radio-Music).

Interference recordings sourced from
[Numbers & Oddities](https://www.numbersoddities.nl/files.html), a shortwave
monitoring archive. Individual recordings are in the public domain.

The altboot broadcast recordings are *Protect and Survive* (UK Government, 1970s civil
defence broadcast) and the *Shipping Forecast* (BBC Radio 4). These are included for
personal and educational use only. *Protect and Survive* material is Crown Copyright;
*Shipping Forecast* is © BBC. Neither is licensed for commercial use or
redistribution. If you build a derivative work for release, replace these recordings
with material you have the rights to use.

Licensed under
[Creative Commons Attribution-ShareAlike 4.0](https://creativecommons.org/licenses/by-sa/4.0/)
— except where third-party copyright applies as noted above.
