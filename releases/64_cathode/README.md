# Cathode Ray — Workshop Computer Program Card

A PAL composite video synthesizer. Generates a live black-and-white picture on any composite-input TV or monitor, driven entirely by Eurorack control voltages and audio signals. The picture reacts to what you patch in — sweep an audio waveform across the screen as an oscilloscope trace, or draw freely with two CV sources as X/Y coordinates.

The image is 1-bit (black/white), rendered through a small **2-bit resistor DAC** built from **Pulse Out 1** and **Pulse Out 2** so the signal has proper composite levels (separate sync, black and white). No extra hardware is needed beyond two resistors and a phono (RCA) cable.

---

## Hardware Connection

The Pulse Out jacks output approximately **6V** for a logic HIGH and **0V** for a logic LOW. A composite video input expects roughly **1V peak** into a **75Ω** load, and crucially needs **three** distinct levels: sync (0V), black (a small pedestal above sync), and white (~1V). A single on/off pin can only make two levels — which forces black and sync to the same voltage and gives a TV poor contrast or no sync lock.

Cathode Ray solves this by summing **two** pulse outputs through weighted resistors into the RCA centre pin, making a 2-bit DAC with enough levels for a clean signal.

### Resistor summing network (two resistors)

```
Pulse Out 1 (Pu1) ──[ 1kΩ  ]──┐
                              ├──── RCA centre pin ──── TV composite in
Pulse Out 2 (Pu2) ──[ 470Ω ]──┘
Workshop Computer GND ─────────────── RCA shell    ──── TV composite GND
```

- **Pu1 via 1kΩ** provides the small "black" pedestal step above sync.
- **Pu2 via 470Ω** provides the larger step up to white.
- The **75Ω** TV input is the bottom leg of the divider.
- All grounds are common: 3.5mm sleeve → RCA shell.

The firmware drives both pins together as a 2-bit symbol per pixel, producing:

| Level | Pu1 | Pu2 | Node voltage | Meaning |
|-------|-----|-----|--------------|---------|
| Sync  | low | low | 0V           | horizontal/vertical sync tips |
| Black | high (via 1kΩ) | low | small pedestal | picture black |
| White | high | high | ~1V | picture white |

### Tuning

Exact voltages depend on your resistors and the TV. If the picture works but contrast is weak, or thin/fast white details look grey, **lower the 470Ω** (white pin) toward 330Ω or 220Ω so the signal reaches full white faster. If black looks too light, raise the 1kΩ slightly. If sync drops out, the black pedestal is too small (1kΩ too high) — bring it back down. All polarity/level tuning in firmware lives in a single `level_pair[]` table in `main.cpp`.

### Building the cable

A short adapter cable is the neatest approach: a male 3.5mm mono jack into Pulse Out 1, a second into Pulse Out 2, both signal wires going through their resistors to the RCA centre pin, all grounds joined to the RCA shell. Heatshrink over the resistors, or mount them on a scrap of stripboard in a small box.

### Which TVs work

Any display with a **composite video input** (usually a yellow RCA phono socket):

- CRT televisions from the 1980s–2000s (the ideal target — phosphor persistence and scanlines suit this kind of video)
- LCD/LED TVs with a yellow composite input
- Portable monitors with composite in
- Video capture devices (Elgato Cam Link, etc.) — for recording or routing onward

The signal is **PAL** (50Hz). Displays locked to NTSC-only will not show it; most modern TVs auto-detect.

---

## Inputs

| Jack | Function |
|------|----------|
| Audio In 1 | Oscilloscope trace (scope mode). The audio level sets the vertical height of the trace as it sweeps across. |
| CV In 1 | Etch-a-sketch X offset (etch mode). Added to the Knob X base position. Sampled at full 48 kHz. |
| CV In 2 | Etch-a-sketch Y offset (etch mode). Added to the Knob Y base position. Sampled at full 48 kHz. |
| Pulse In 1 | **Clear** — any rising edge instantly clears the screen to black. |
| Pulse In 2 | **Invert** — while held HIGH, the video output is inverted (white ↔ black). |

---

## Outputs

| Jack | Function |
|------|----------|
| Pulse Out 1 | Composite video — DAC bit 0 (via 1kΩ). |
| Pulse Out 2 | Composite video — DAC bit 1 (via 470Ω). |

Both pulse outputs are consumed by the video DAC and are not available as normal pulse outputs while this firmware runs.

---

## Controls

### Main Knob — Mode Select

| Position | Mode | Behaviour |
|----------|------|-----------|
| **CCW (left half)** | Oscilloscope | Audio In 1 sets the trace height. A vertical line is drawn from screen centre to the sampled level, sweeping left→right (about 1.8s per full sweep). |
| **CW (right half)** | Etch-a-sketch | Knob X/Y set a base cursor position; CV In 1/2 add an offset around it. A 3×3 dot is drawn at every captured CV sample, so fast moves draw continuous lines. |

Mode changes take effect immediately and do not clear the screen.

### Knob X / Knob Y — Etch base position

In etch-a-sketch mode, Knob X and Knob Y set the resting cursor position (X and Y) when no CV is patched. CV In 1/2 then add a bipolar offset around that point. A centred knob with nothing patched draws at the centre of the screen.

### Switch — Background Mode

Controls what happens to pixels already on screen between frames.

| Position | Mode | Behaviour |
|----------|------|-----------|
| **UP** | Phosphor fade | Pixels gradually dissolve to true black over about 2.4 seconds. A scattered (non-directional) erase covers the whole screen evenly and always reaches full black. |
| **MIDDLE** | Static | Pixels persist. In scope mode each column is cleared just before redrawing, giving a single clean trace; in etch mode drawings accumulate. |
| **DOWN** | Snow | Random noise is XORed into the framebuffer each frame — a field of static. Works well combined with the invert gate. |

---

## LEDs

```
| LED 0   LED 1 |
| LED 2   LED 3 |
| LED 4   LED 5 |
```

| LED | Function |
|-----|----------|
| LED 0 | Lit in oscilloscope mode |
| LED 1 | Lit in etch-a-sketch mode |
| LED 2 | Lit while Pulse In 2 (invert) is HIGH |
| LED 3 | Lit in phosphor fade mode (Switch UP) |
| LED 4 | Lit in snow mode (Switch DOWN) |
| LED 5 | Unused |

---

## Signal Flow

```
Audio In 1 ─────────────────────────► trace height (scope mode)
CV In 1 ──┐
          ├─ + Knob X/Y base ────────► X/Y cursor (etch mode, 48kHz sampled)
CV In 2 ──┘

Main Knob ──────────────────────────► mode select (CCW=scope, CW=etch)
Switch ─────────────────────────────► background (UP=fade, MID=static, DOWN=snow)

Pulse In 1 (rising edge) ───────────► clear framebuffer
Pulse In 2 (gate HIGH) ─────────────► invert output

Framebuffer (360×256, 1bpp) ─► PAL word stream ─► PIO/DMA ─► Pu1+Pu2 ─► [resistor DAC] ─► TV
```

---

## Getting Started

1. Wire the resistor DAC: Pu1 via 1kΩ and Pu2 via 470Ω, both into the RCA centre pin; all grounds to the RCA shell.
2. Turn the TV on and select the composite input. You should see a stable black screen immediately. If the picture rolls or there is no sync, recheck the resistors and grounds.
3. Set the Switch to **MIDDLE** (static).
4. Turn the Main Knob fully **CCW** (oscilloscope) and patch any audio into **Audio In 1** — you'll see the waveform sweep across.
5. Try Switch **UP** (phosphor fade) for a glowing persistence trail that dissolves to black.
6. Turn the Main Knob fully **CW** (etch-a-sketch). Set Knob X/Y to centre the cursor, then patch LFOs/CV into **CV In 1** and **CV In 2** — two slightly-detuned sine LFOs draw evolving Lissajous figures.
7. Send a gate into **Pulse In 1** to clear, or into **Pulse In 2** to strobe-invert.

---

## Patch Ideas

**Lissajous figures** — Etch mode, two sine LFOs at slightly different rates into CV In 1/2. Because CV is sampled at 48 kHz, the figures draw as continuous curves, not dotted points.

**Oscilloscope with persistence** — Scope mode + Switch UP. The live sweep leaves a trail that dissolves evenly to black.

**Self-clearing snowfield** — Switch DOWN with nothing patched, then a rhythmic gate to Pulse In 1 to clear on the beat.

**Strobed inversion** — A square LFO or clock into Pulse In 2 flips the image at the LFO rate. Combine with snow for pulsing static.

---

## Technical Notes

- **Video format:** PAL composite, progressive (non-interlaced), 360×256 pixels at 50 frames per second.
- **Pixel clock:** 144 MHz ÷ (144/7) = **7.000 MHz** exactly. Each pixel is ~142.857 ns. Line period 64.000 µs (PAL spec 64.00 µs). Frame 312 lines = 50 Hz.
- **2-bit DAC output:** Each pixel is sent as a 2-bit symbol to GPIO 8 (Pu1) and GPIO 9 (Pu2) via PIO `out pins, 2`, summed externally into 3 composite levels (sync / black / white).
- **Core allocation:** Core 1 is dedicated to video (PIO + DMA) for rock-solid sync; Core 0 runs all Eurorack I/O through ComputerCard at 48 kHz and pushes CV samples to Core 1 via a ring buffer.
- **Phosphor fade:** the 1-bit framebuffer has no brightness, so fade is a deterministic scattered dissolve — each frame clears a batch of framebuffer bytes stepping by a stride coprime to the buffer size, guaranteeing every pixel reaches true black exactly once per ~2.4s cycle while looking like an even dissolve.
- **RAM usage:** ~35% of the RP2040's 256 KB, dominated by the two double-buffered PAL word streams (~70 KB) and the framebuffer (~11 KB).
- **Pixels are taller than wide** (portrait) given 360 columns over the ~52µs active line and 256 rows.
