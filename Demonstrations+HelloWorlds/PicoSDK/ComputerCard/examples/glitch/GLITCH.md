# Glitch — Workshop Computer Program Card

A clock-synced beat-repeater and audio degradation effect. Glitch continuously records incoming audio into a circular buffer, then — when triggered — replays a frozen slice of that buffer at a subdivided rate, with optional reversal and lo-fi degradation.

---

## Inputs

### Audio
| Jack | Function |
|------|----------|
| Audio In 1 | Main audio input — continuously recorded into the buffer |

### Pulse
| Jack | Function |
|------|----------|
| Pulse In 1 | **Clock input** — rising edge sets the beat length. Each new pulse measures the time since the last pulse (in samples) to define MasterLoopLength. Maximum buffer is 0.5 seconds; longer beats are capped. |
| Pulse In 2 | **External gate** — used only when Switch is in MID position. While HIGH, forces glitching on the current slice. |

### CV
| Jack | Function |
|------|----------|
| CV In 1 | **Freeze** — treated as a comparator. When the voltage is above ~0V, the buffer stops recording. The frozen content loops continuously. Below ~0V, recording resumes. |
| CV In 2 | **Degradation modulation** — bipolar input added to both Knob X and Knob Y values. Positive voltage increases degradation amount and probability; negative voltage reduces them. |

---

## Outputs

| Jack | Function |
|------|----------|
| Audio Out 1 | Main output — glitched or pass-through audio |
| Audio Out 2 | Identical to Audio Out 1 (same signal, both channels driven) |

---

## Controls

### Main Knob — Ratchet Zone + Probability

The Main Knob does two things simultaneously, hidden within a single sweep.

The knob range (0–4095) is divided into **5 equal zones**. The zone you are in selects the **ratchet division** — how many times the captured beat slice is subdivided and repeated:

| Zone | Ratchet | Effect |
|------|---------|--------|
| 1 (fully CCW) | ÷1 | Whole beat repeated once |
| 2 | ÷2 | Beat cut in half, each half repeated |
| 3 | ÷3 | Beat cut into thirds |
| 4 | ÷4 | Beat cut into quarters |
| 5 (fully CW) | ÷6 | Beat cut into sixths |

**Hidden within each zone:** the position of the knob *within* that zone sets the **Reverse Probability Threshold**. At the bottom of a zone there is no reversal; approaching the top of the zone, random reversal becomes increasingly likely. This probability also controls whether glitching fires at all in Probabilistic mode (Switch UP).

LED 5 (bottom right) shows the current probability threshold as brightness — use it to navigate within zones.

---

### Knob X — Degradation Amount

Controls the depth of lo-fi degradation applied to glitched slices, introduced in two stages across the knob's travel.

**First half (CCW to centre) — Decimation (sample-rate reduction):** quantises the read position so the same sample plays multiple times before advancing. Creates a stepped, aliased texture. Fully CCW is clean; at centre the signal advances in steps of 16 samples, giving a heavily lo-fi character.

**Second half (centre to CW) — Bitcrushing added:** decimation stays at maximum while bitcrushing is progressively introduced, reducing the bit depth of the audio. Low bits are masked off, adding increasing quantisation grit up to extremely coarse distortion at full CW.

Fully CCW is completely clean regardless of Knob Y. CV In 2 offsets this value — positive voltage pushes further into degradation.

---

### Knob Y — Degradation Probability

Sets **how often** the degradation (bitcrush + decimation) actually applies to a given slice. This is evaluated fresh at every slice boundary.

- Fully CCW (0): degradation never applies, even if Knob X is turned up — the audio is always clean
- Mid-range: degradation fires roughly half the time, alternating between clean and degraded slices
- Fully CW (4095): degradation always applies

Combine Knob X (amount) and Knob Y (probability) to dial in anything from subtle occasional grit to relentless destruction.

---

### Switch — Glitch Trigger Mode

The switch selects how glitching (ratcheting + reversal) is activated. Glitch state is locked in for the full duration of each slice, then re-evaluated at the next slice boundary.

| Position | Mode | Behaviour |
|----------|------|-----------|
| **UP** (latching) | Probabilistic | At each slice boundary, a random number is compared to the Main Knob's hidden remainder threshold. If the random value falls below the threshold, glitching fires. Low threshold = rare glitches. High threshold = frequent glitches. |
| **MID** | External Gate | Glitching is active whenever Pulse In 2 is HIGH. Use an external gate, envelope, or sequencer to control exactly when glitching occurs. |
| **DOWN** (momentary) | Force | Glitching is always on, regardless of probability or gate. Useful for manual performance — hold down to freeze into the glitch, release to return to pass-through. |

When **not glitching**, the module passes live audio from Audio In 1 directly to the outputs. No buffer playback, no degradation.

---

## LEDs

```
| LED 0   LED 1 |
| LED 2   LED 3 |
| LED 4   LED 5 |
```

| LED | Function |
|-----|----------|
| LED 0 | Lit when Main Knob is in Zone 1 (ratchet ÷1) |
| LED 1 | Lit when Main Knob is in Zone 2 (ratchet ÷2) |
| LED 2 | Lit when Main Knob is in Zone 3 (ratchet ÷3) |
| LED 3 | Lit when Main Knob is in Zone 4 (ratchet ÷4) |
| LED 4 | Lit when Main Knob is in Zone 5 (ratchet ÷6) |
| LED 5 | **Brightness** = reverse probability threshold (the hidden remainder within the current zone). Dim = low probability, bright = high probability. |

Only one of LEDs 0–4 is lit at a time, showing your current ratchet selection at a glance.

---

## Signal Flow

```
Audio In 1 ──► Circular Buffer (0.5s) ──► [if glitching] Slice Playback ──► Audio Out 1+2
                     ▲                           │
               CV In 1 (Freeze)          Ratchet / Reverse
                                         Decimate / Bitcrush

               Pulse In 1 (Clock) ──► MasterLoopLength
               Pulse In 2 (Gate) ──► [Switch MID only] force glitch
               CV In 2 (bipolar) ──► offsets Knob X + Knob Y
```

When not glitching, Audio In 1 passes straight to the outputs, bypassing the buffer entirely.

---

## Getting Started

1. Patch a clock or LFO square wave into **Pulse In 1**
2. Patch audio into **Audio In 1**
3. Listen on **Audio Out 1**
4. Set Switch to **DOWN** — you should immediately hear the beat repeating
5. Turn the **Main Knob** clockwise to hear higher ratchet subdivisions
6. Move Switch to **UP** and explore the probability zone within each ratchet setting
7. Bring up **Knob X** for degradation; use **Knob Y** to control how often it fires
8. Patch a gate into **CV In 1** (above 0V) to freeze the buffer at a particular moment

---

## Notes

- Before the first clock pulse arrives, the module passes audio through without glitching
- Beat lengths longer than 0.5 seconds are automatically capped at the buffer size
- Very short beats (faster than ~2ms per subdivision) use the minimum slice of 1 sample with no fade
- The buffer records even while glitching — on the next beat, fresh audio is available
- Degradation is evaluated independently of glitch mode — you can have degraded pass-through if Knob Y is high and Switch is DOWN with no gate signal
