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
Sets the overall static floor.



**Switch Down tap — Cycle band**
A short press (under one second) steps through AM → Shortwave → Longwave → AM. Each band changes the audio bandwidth, noise character, and which interference clips are active.


## Bands

 Tap Switch Down to cycle bands live.

## Interference clips

Six real shortwave recordings are baked into the firmware at 8kHz mono. They play continuously, looped, at fixed positions on the virtual dial. 

Each band places the six interference clips at different places in the span.  two below station 1, two inbetween station 1 and 2, two abover stasion 2.

## LEDs

| LED | Function |
|-----|----------|
| 0 | Station 1 signal strength |
| 1 | Station 2 signal strength |
| 2/3/4 | SW/AM/LW |
| 5 | Tuning position — sweeps from dim (CCW) to bright (CW) |

## Boot modes

**Normal mode** — power on with Switch in any position other than Down, or release it within the first 200ms. Live audio inputs act as the two stations.

**Broadcast mode** — hold Switch Down at power-on. Keep holding until all six LEDs flash simultaneously (~200ms). The two live audio inputs are replaced by baked broadcast recordings at 11kHz. Everything else — noise, interference clips, drift, band switching — works identically.


## Technical notes

- Interference clips: 8kHz unsigned 8-bit mono, stored in flash (~938KB)
- Broadcast clips: 11025Hz unsigned 8-bit mono, stored in flash (~975KB)

- 200ms startup holdoff before audio begins, 10ms linear fade-in after holdoff

## Credits

By Andy Jenkinson ([uglifruit](https://github.com/uglifruit)), developed with [Claude Code](https://claude.ai/code).

Built on the [Workshop Computer](https://github.com/TomWhitwell/Workshop_Computer) platform by Tom Whitwell (Music Thing Modular), using the ComputerCard framework.

Inspired by [RadioMusic](https://github.com/TomWhitwell/RadioMusic) by Tom Whitwell and the [Music Thing Radio Music workshop](https://dyski.co/Tom-Whitwell-Radio-Music).

Interference recordings sourced from [Numbers & Oddities](https://www.numbersoddities.nl/files.html), a shortwave monitoring archive. Individual recordings are in the public domain.

The altboot broadcast recordings are *Protect and Survive* (UK Government, 1970s civil defence broadcast) and the *Shipping Forecast* (BBC Radio 4). These are included for personal and educational use only. *Protect and Survive* material is Crown Copyright; *Shipping Forecast* is © BBC. Neither is licensed for commercial use or redistribution. If you build a derivative work for release, replace these recordings with material you have the rights to use.

Licensed under [Creative Commons Attribution-ShareAlike 4.0](https://creativecommons.org/licenses/by-sa/4.0/) — except where third-party copyright applies as noted above.
