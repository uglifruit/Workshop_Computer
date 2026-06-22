# OffAir

AM/shortwave radio simulator for the Workshop Computer.

Two audio inputs become radio stations at fixed positions on a virtual dial. The Main knob tunes across them through noise, static, and drifting carrier interference. Baked recordings of real numbers stations and data signals appear at their own positions across the band. Turn the drift up and the stations wander.

Three bands are available — AM, Shortwave, and Longwave — each with different audio bandwidth and noise character. A second boot mode replaces the live inputs with looped broadcast recordings, turning the module into a self-contained radio.

## Inputs

| Jack | Function |
|------|----------|
| Audio In 1 | Station 1 source (live audio) |
| Audio In 2 | Station 2 source (live audio) |
| CV In 1 | Fine tune offset — bipolar, ±12% of dial range |
| CV In 2 | Interference probability — 0V = off, +5V = always present |
| Pulse In 1 | Rising edge resets station drift to zero |
| Pulse In 2 | While HIGH, freezes the tuning position |

## Outputs

| Jack | Function |
|------|----------|
| Audio Out 1 | Full mix — stations, interference, and static |
| Audio Out 2 | Noise and interference only — useful for external layering |
| CV Out 1 | Signal strength — 0V (no signal) to +5V (strong reception) |
| CV Out 2 | Station balance — −5V (Station 1), 0V (mid), +5V (Station 2) |
| Pulse Out 1 | HIGH when receiving either station cleanly |
| Pulse Out 2 | HIGH when in a noise zone (no station) |

## Controls

**Main Knob — Tuning**
Scans across the dial. Station 1 sits at one-third of the range, Station 2 at two-thirds. The baked interference clips sit at asymmetric positions around them — each band has a different layout.

**CV In 1 — Fine tune**
Adds a bipolar offset to the tuning position. Useful for slow automated scanning or subtle position shifts from an LFO or sequencer.

**Knob X — Drift**
Controls how much the two stations wander from their nominal positions. At minimum, stations are locked. At maximum, each station drifts independently up to ±96 dial counts, making precise tuning difficult.

**Knob Y — Noise level**
Sets the overall static floor. Noise is loudest in gaps between stations and suppressed at station centres — quieter when you're locked on, louder when you're off between transmissions.

**CV In 2 — Interference probability**
At 0V, the baked interference clips are silent. As CV rises toward +5V, they appear with increasing probability when the dial passes over their positions. Each trigger is independently gated, so the clips flutter in and out rather than switching cleanly — this is the right behaviour.

**Switch Down tap — Cycle band**
A short press (under one second) steps through AM → Shortwave → Longwave → AM. Each band changes the audio bandwidth, noise character, and which interference clips are active.

**Pulse In 1 — Reset drift**
A rising edge snaps both stations back to their nominal positions instantly.

**Pulse In 2 — Freeze tuning**
While Pulse In 2 is high, the tuning position is held regardless of the Main knob or CV In 1. Release to resume.

## Bands

| Band | Switch | LPF cutoff | Reception window | Noise character | Interference clips |
|------|--------|-----------|------------------|-----------------|--------------------|
| AM | Up (or free) at boot | ~3.5kHz | Medium | 50% white / 50% crackle | Polish numbers station, UM10 voice signal |
| SW | Mid at boot | ~5.5kHz | Wide | 25% white / 75% crackle | AFSK data burst, XT2 unidentified digital |
| LW | Down at boot | ~1.8kHz | Narrow | 75% white / 25% crackle | Unidentified polytone, MX-L tone sequence |

The boot switch position sets the starting band. Tap Switch Down to cycle bands live.

## Interference clips

Six real shortwave recordings are baked into the firmware at 8kHz mono. They play continuously, looped, at fixed positions on the virtual dial. CV In 2 controls how often they surface as you tune past them — at low CV they are faint and occasional; at high CV they intrude more aggressively.

Each band has two clips at different dial positions, placed asymmetrically so the band has varied texture across its range.

## Static and carrier noise

Between stations, two types of noise fill the gaps:

**Crackle and white static** — a blend of white noise and highpass-filtered noise, mixed according to the active band. Gated so it suppresses automatically when you lock onto a station.

**Bandpass carrier noise** — a resonant filter fed by white noise, producing a narrow drifting band of noise that sounds like a heterodyne whistle or nearby untuned carrier. The centre frequency wanders slowly, so it never sounds the same twice. It appears in the zone between the two stations and fades at the station centres.

## LEDs

| LED | Function |
|-----|----------|
| 0 | Station 1 signal strength |
| 1 | Station 2 signal strength |
| 2 | Noise level |
| 3 | Station 1 drift magnitude |
| 4 | Station 2 drift magnitude |
| 5 | Tuning position — sweeps from dim (CCW) to bright (CW) |

## Boot modes

**Normal mode** — power on with Switch in any position other than Down, or release it within the first 200ms. Live audio inputs act as the two stations.

**Broadcast mode** — hold Switch Down at power-on. Keep holding until all six LEDs flash simultaneously (~200ms). The two live audio inputs are replaced by baked broadcast recordings at 11kHz. Everything else — noise, interference clips, drift, band switching — works identically.

In broadcast mode the module is self-contained: patch Audio Out 1 to a speaker or recorder and tune across the dial.

## Quick start

**Scanning a shortwave band**
1. Patch two different audio sources into Audio In 1 and 2
2. Set Knob Y (noise) to taste — start at noon
3. Sweep the Main knob slowly from CCW to CW
4. Raise CV In 2 to bring in the interference clips
5. Add Knob X drift for instability

**Self-contained broadcast (no inputs needed)**
1. Hold Switch Down at power-on until LEDs flash — broadcast mode
2. Patch Audio Out 1 to a mixer or recorder
3. Tune with the Main knob

**Animated tuning from a sequencer**
1. Patch a slow CV sequence into CV In 1
2. Set the Main knob to the centre of the range you want
3. The sequence offsets around the knob position — stepping between stations and noise zones

**Freeze on a station**
1. Tune to a station manually
2. Patch a gate high into Pulse In 2 — tuning locks
3. CV Out 1 can then follow the locked signal strength as a gate or modulation source

**Reset drift mid-performance**
1. Raise Knob X for heavy drift
2. Send a trigger to Pulse In 1 to snap both stations back to nominal instantly

## Technical notes

- 48kHz sample rate, integer arithmetic throughout (RP2040 has no FPU)
- Interference clips: 8kHz unsigned 8-bit mono, stored in flash (~938KB)
- Broadcast clips: 11025Hz unsigned 8-bit mono, stored in flash (~975KB)
- Total audio in flash: ~1.9MB of 2MB available (94.5%)
- Station envelopes: piecewise linear flat-top bell (no division in hot path)
- Bandpass noise: second-order IIR resonator, centre frequency wandering ~0.07Hz via LFO
- Crackle: one-pole highpass applied to white noise; blend ratio is band-dependent
- Station drift: independent random walks per station, IIR-smoothed, clamped by Knob X
- DC blocking per station before AM lowpass — removes input DC offset cleanly
- 200ms startup holdoff before audio begins, 10ms linear fade-in after holdoff

## Credits

By Andy Jenkinson ([uglifruit](https://github.com/uglifruit)), developed with [Claude Code](https://claude.ai/code).

Built on the [Workshop Computer](https://github.com/TomWhitwell/Workshop_Computer) platform by Tom Whitwell (Music Thing Modular), using the ComputerCard framework.

Inspired by [RadioMusic](https://github.com/TomWhitwell/RadioMusic) by Tom Whitwell and the [Music Thing Radio Music workshop](https://dyski.co/Tom-Whitwell-Radio-Music).

Interference recordings sourced from [Numbers & Oddities](https://www.numbersoddities.nl/files.html), a shortwave monitoring archive. Individual recordings are in the public domain.

The altboot broadcast recordings are *Protect and Survive* (UK Government, 1970s civil defence broadcast) and the *Shipping Forecast* (BBC Radio 4). These are included for personal and educational use only. *Protect and Survive* material is Crown Copyright; *Shipping Forecast* is © BBC. Neither is licensed for commercial use or redistribution. If you build a derivative work for release, replace these recordings with material you have the rights to use.

Licensed under [Creative Commons Attribution-ShareAlike 4.0](https://creativecommons.org/licenses/by-sa/4.0/) — except where third-party copyright applies as noted above.
