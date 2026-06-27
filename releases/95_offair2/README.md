# OffAir

Shortwave radio simulator for the Workshop Computer.

OffAir lets you tune across a virtual band the way you tune a real shortwave
receiver. As you approach a station you hear the signature heterodyne whistle slide
in pitch, the audio pull into tune, and the background static duck away. Tune past it
and the audio garbles, the whistle rises again, and it fades back into the noise.

Rather than literally modulating and demodulating an RF carrier (which can't give
clean selectivity in cheap integer DSP), OffAir **synthesises the audible result**
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
| AM | Correct-pitch audio, distorts off-tune (envelope detection) | Mellow mid hiss, steady |
| SW | Directional pitch-shift (SSB) | Bright, busy, crackly wash that swishes |
| LW | Directional pitch-shift (SSB) | Deep slow rumble, barely swishes, heavy fade |

The dial layout (station and interference positions, and which clips play) is
re-randomised each time you change band.

## Inputs

| Jack | Function |
|------|----------|
| Audio In 1 | Station 1 source (live audio, normal boot) |
| Audio In 2 | Station 2 source (live audio, normal boot) |
| CV In 1 | Tuning — full range, bipolar. Patch an LFO or sequencer to scan the dial hands-free |
| CV In 2 | Noise level — adds to Knob Y (voltage-controlled static) |
| Pulse In 1 | Rising edge — re-randomise the station / interference layout. (In normal boot with Switch Up, this becomes the morse key instead — see Controls) |
| Pulse In 2 | Rising edge — trigger a one-shot from the curated one-shot bank |

## Outputs

| Jack | Function |
|------|----------|
| Audio Out 1 | Full mix — tuned audio, whistles, static, one-shot bursts |
| Audio Out 2 | Noise / static only |
| CV Out 1 | Signal strength — an envelope that rises as you tune onto a station |
| CV Out 2 | Broadcast 1's tuning position — patch through a slew into CV In 1 to slowly tune toward Broadcast 1 (re-hunts when Pulse In 1 re-randomises) |
| Pulse Out 1 | Gate HIGH while tuned to Broadcast 1 |
| Pulse Out 2 | Gate HIGH while tuned to Broadcast 2 |

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

**Switch Up hold — mode-dependent.** What it does depends on how you booted:

- **Broadcast mode (altboot):** mutes broadcasts 1 & 2. The interference signals carry
  on, and the broadcasts still "exist" — they keep driving CV Out 1/2 and Pulse Out
  1/2 at their dial positions — they just make no sound. Release to bring them back.
- **Normal mode:** Broadcast 2's audio is replaced by a ~600 Hz morse tone keyed by
  **Pulse In 1** — feed a rhythm or gate pattern into PU1 and it becomes an audible
  keyed signal at Broadcast 2's position (heard when tuned to it, pitch-shifting
  off-tune like any station). While held, Pulse In 1 no longer shuffles the layout.

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
  one-shot bank; broadcast clips run ~32–34 s.
- 200 ms startup holdoff before audio begins, with a short linear fade-in.

## Using the prebuilt firmware

Just flash **`offair2.uf2`** — hold BOOTSEL on the Computer, drag the file across.
No build needed; the baked audio (`clips.h`) is already compiled in.

## Making your own version with custom sounds

All the radio audio is baked into `clips.h`, generated from source audio files by
`convert_clips.py`. The audio sources themselves are **not** included in this repo —
supply your own. To build a version with your own broadcasts / interference / events:

**1. Install the Python tools** (one-off):

```
pip install miniaudio numpy
```

**2. Prepare your audio.** Any format/sample rate works (WAV, MP3, AIFF, FLAC, mono
or stereo, any rate — the script resamples and downmixes). There are three pools:

- **Broadcast** — the two main stations you tune in (altboot mode plays these).
- **Looping interference** — continuous beds (numbers, morse, data) that sit at
  dial positions and loop forever.
- **One-shots** — short isolated events (clicks, dropouts, crashes, single bursts)
  triggered by Pulse In 2. Curate these to make sense heard alone.

**3. Point the script at your files.** Edit the top of `convert_clips.py`:

- `FOLDER` — the directory holding your broadcast and looping-interference files.
- The `CLIPS` table — list each broadcast / interference file (filename, start
  second, max length). `LOOP_MAX_SEC` caps interference loop length;
  `MAX_BCAST_SEC` caps broadcast length. Keep 2 broadcast + 6 interference entries
  unless you also change the counts in `main.cpp`.
- `ONESHOT_FOLDER` — a folder of one-shot files; **all** files in it are
  auto-discovered (any count). `ONESHOT_MAX_SEC` caps each one's length.

**4. Generate and build:**

```
python convert_clips.py          # regenerates clips.h, prints sizes + flash budget
```

Then build with the Pico SDK (CMake / Ninja) from this folder and flash the new
`offair2.uf2`. The script warns if your audio exceeds the ~2 MB flash budget — trim
clip lengths or drop one-shots if so.

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
