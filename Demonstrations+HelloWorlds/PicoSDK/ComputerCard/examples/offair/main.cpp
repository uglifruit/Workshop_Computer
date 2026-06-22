// offair — AM/shortwave radio simulator for the Workshop Computer
//
// NORMAL BOOT (switch not held down):
//   Audio In 1      : Station 1 source (live audio)
//   Audio In 2      : Station 2 source (live audio)
//
// ALTBOOT (hold Switch Down at power-on until LEDs flash):
//   Baked broadcast recordings replace the live audio inputs.
//   All other controls work identically.
//
//   Audio Out 1     : Full mix — stations + interference + noise
//   Audio Out 2     : Noise/interference-only output
//   Main Knob       : Tuning position — sweep across the dial
//   Knob X          : Drift depth (0 = locked, max = heavy station wander)
//   Knob Y          : Noise / static level
//   CV In 1         : Fine tune offset (bipolar, ±512 counts)
//   CV In 2         : Interference clip probability (0V = off, +5V = always on)
//   CV Out 1        : Signal strength 0→+5V
//   CV Out 2        : Station balance (−5V = St1, 0 = mid, +5V = St2)
//   Pulse In 1      : Rising edge = reset station drift to zero
//   Pulse In 2      : While HIGH = freeze tuning position
//   Pulse Out 1     : HIGH when cleanly receiving a station
//   Pulse Out 2     : HIGH when in noise zone (no station)
//   Switch Down tap : Cycle band AM → SW → LW → AM
//   LED 0           : Station 1 signal strength
//   LED 1           : Station 2 signal strength
//   LED 2           : Noise level
//   LED 3           : Station 1 drift magnitude
//   LED 4           : Station 2 drift magnitude
//   LED 5           : Tuning position (sweeps with main knob)
//
// Bands:
//   AM  — 3.5kHz LPF, medium reception window; interference: voice/numbers (POL, UM10)
//   SW  — 5.5kHz LPF, wider window; interference: data/AFSK signals (F03, XT2)
//   LW  — 1.8kHz LPF, narrow window; interference: polytones (POLY, MXL)
//
// Integer math only — no floats in the hot path.

#include "ComputerCard.h"
#include "clips.h"

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

static constexpr int32_t kHoldoffSamples = 9600;
static constexpr int32_t kFadeInSamples  = 480;

// Altboot detection: Switch Down must still be held at this sample count
// (well past the 4800-sample ADC settle, giving user time to hold deliberately)
static constexpr int32_t kAltbootDetect  = 9600; // checked at holdoff end

// Two live-audio stations at 1/3 and 2/3 of dial (0..4095)
static constexpr int32_t kStation1Pos = 1365;
static constexpr int32_t kStation2Pos = 2730;

// All six interference clips play in every band but at different dial positions
// and in different orders.  Stations sit at 1365 (St1) and 2730 (St2).
// Slots 0-1 are in the gap between the stations; slots 2-5 are outside.
//
// Clip indices: 0=POL, 1=UM10, 2=F03, 3=XT2, 4=POLY, 5=MXL
//
// kIntfPos[band][slot] — dial position for each slot
static constexpr int32_t kIntfPos[3][6] = {
    // AM:  gap=1600,2200  low-out=300,800  high-out=3000,3600
    { 1600, 2200,  300,  800, 3000, 3600 },
    // SW:  gap=1500,2400  low-out=500,700  high-out=3200,3750
    { 1500, 2400,  500,  700, 3200, 3750 },
    // LW:  gap=1750,2050  low-out=200,600  high-out=3100,3800
    { 1750, 2050,  200,  600, 3100, 3800 },
};

// kClipOrder[band][slot] — which clip (into kAllClips) plays at each slot
static constexpr uint8_t kClipOrder[3][6] = {
    { 0, 3, 1, 4, 2, 5 },   // AM:  POL, XT2, UM10, POLY, F03, MXL
    { 2, 5, 0, 3, 1, 4 },   // SW:  F03, MXL, POL, XT2, UM10, POLY
    { 4, 1, 3, 0, 5, 2 },   // LW:  POLY, UM10, XT2, POL, MXL, F03
};

// Interference clip reception window — narrower than live stations
static constexpr int32_t kIntfBW   = 200;
static constexpr int32_t kIntfHalf = 100;

// Sideband zone: station audio leaks in here before the flat-top, heavily LPF'd.
// Width beyond the station bandwidth where sideband effect is heard.
static constexpr int32_t kSidebandExtra = 300; // counts beyond kBandwidth

// Maximum drift excursion at full Knob X
static constexpr int32_t kMaxDriftFull = 96;

// 1-in-128 drift step probability (bits 31..25 of LCG output all zero)
static constexpr uint32_t kDriftProbMask = 0xFE000000u;

// Bandpass noise: second-order resonator coefficients (Q15).
// alpha = 32767 * 2*cos(2*pi*fc/48000)
// Band-dependent ranges give each band a distinct noise character:
//   AM  (300–800 Hz):   alpha 32661..32759  — soft, low rumble
//   SW  (800–2500 Hz):  alpha 30274..32169  — mid hiss, the classic SW sound
//   LW  (100–400 Hz):   alpha 32736..32765  — very low thump
static constexpr int32_t kBpAlphaTable[3][2] = {
    { 32661, 32759 },   // AM:  300–800 Hz
    { 30274, 32169 },   // SW:  800–2500 Hz
    { 32736, 32765 },   // LW:  100–400 Hz
};

// LFO for bandpass centre wander (~0.07 Hz at 48000 Hz call rate)
static constexpr uint32_t kBpLFOInc = 6257u;

// ProcessSample() is called at 48kHz (once per audio sample period).
// The 8-sample DMA burst contains 2 samples each from 4 ADC channels,
// not 8 independent audio samples — so ProcessSample rate = 48000Hz.
//
// Interference clips at 8000Hz: advance once every 6 calls.
// Use fractional accumulator: add 8000 per call, advance when >= 48000.
static constexpr int32_t kClipNum = 8000;
static constexpr int32_t kClipDen = 48000;

// Broadcast clips at 11025Hz: add 11025 per call, advance when >= 48000.
static constexpr int32_t kBcastNum = 11025;
static constexpr int32_t kBcastDen = 48000;

// Boot animation: 6 LEDs sweep left→right × 3 passes.
// kBootStepLen is in ProcessSample calls (48000Hz).  2000 calls = ~42ms per LED.
static constexpr int32_t kBootStepLen = 2000;
static constexpr int32_t kBootSteps   = 18;

// ---------------------------------------------------------------------------
// Sine table — 256-entry quarter-wave, int8 (-128..127)
// Generated: sin(i * pi/2 / 256) * 127 for i = 0..255
// ---------------------------------------------------------------------------

// Full 256-entry sine table (one complete cycle), int8 -127..127
static const int8_t kSinTable[256] = {
      0,  3,  6,  9, 12, 16, 19, 22, 25, 28, 31, 34, 37, 40, 43, 46,
     49, 52, 55, 58, 61, 64, 67, 70, 73, 75, 78, 81, 84, 86, 89, 92,
     94, 97, 99,102,104,107,109,112,114,116,118,121,123,125,126,127,
    127,127,127,127,126,125,123,121,118,116,114,112,109,107,104,102,
     99, 97, 94, 92, 89, 86, 84, 81, 78, 75, 73, 70, 67, 64, 61, 58,
     55, 52, 49, 46, 43, 40, 37, 34, 31, 28, 25, 22, 19, 16, 12,  9,
      6,  3,  0, -3, -6, -9,-12,-16,-19,-22,-25,-28,-31,-34,-37,-40,
    -43,-46,-49,-52,-55,-58,-61,-64,-67,-70,-73,-75,-78,-81,-84,-86,
    -89,-92,-94,-97,-99,-102,-104,-107,-109,-112,-114,-116,-118,-121,-123,-125,
   -126,-127,-127,-127,-127,-127,-126,-125,-123,-121,-118,-116,-114,-112,-109,-107,
   -104,-102,-99,-97,-94,-92,-89,-86,-84,-81,-78,-75,-73,-70,-67,-64,
    -61,-58,-55,-52,-49,-46,-43,-40,-37,-34,-31,-28,-25,-22,-19,-16,
    -12, -9, -6, -3,  0,  3,  6,  9, 12, 16, 19, 22, 25, 28, 31, 34,
     37, 40, 43, 46, 49, 52, 55, 58, 61, 64, 67, 70, 73, 75, 78, 81,
     84, 86, 89, 92, 94, 97, 99,102,104,107,109,112,114,116,118,121,
    123,125,126,127,127,127,127,127,126,125,123,121,118,116,114,112,
};

// Lookup sine from a uint32 phase accumulator. Returns -127..127.
static inline int32_t __not_in_flash_func(isin)(uint32_t phase)
{
    return kSinTable[phase >> 24];
}

// Carrier frequency mapping: dial position 0..4095 → 300..3000 Hz
// phase_inc = freq * 2^32 / 48000  ≈ freq * 89478
// To avoid 64-bit: phase_inc = freq * 89478; freq = pos*2700/4096 + 300
// Simplified: phase_inc = (pos * 59 + 26843) << 1  (≈ close enough)
// Exact: freq(pos) = 300 + pos * 2700 / 4096
//        phase_inc = freq * 89478
static inline uint32_t __not_in_flash_func(carrierInc)(int32_t dialPos)
{
    // freq in Hz = 300 + dialPos * 2700 / 4096
    // phase_inc = freq * 89478  (= 2^32 / 48000)
    // = (300 + dialPos * 2700/4096) * 89478
    // = 300*89478 + dialPos * (2700/4096 * 89478)
    // = 26843400  + dialPos * 58988  (integer approximation)
    return (uint32_t)(26843400u + (uint32_t)dialPos * 58988u);
}

// ---------------------------------------------------------------------------
// RNG — 32-bit LCG (from glitch/main.cpp)
// ---------------------------------------------------------------------------

static uint32_t g_rng = 1;
static inline uint32_t __not_in_flash_func(rng_next)()
{
    g_rng = 1664525u * g_rng + 1013904223u;
    return g_rng;
}

// ---------------------------------------------------------------------------
// Soft clip — simulates AM detector compression
// ---------------------------------------------------------------------------

static inline int32_t __not_in_flash_func(softClip)(int32_t s, int32_t threshold)
{
    if (s > threshold)  return threshold + ((s - threshold) >> 2);
    if (s < -threshold) return -threshold + ((s + threshold) >> 2);
    return s;
}

// ---------------------------------------------------------------------------
// Station envelope — flat-top piecewise linear bell, returns 0..4095
// ---------------------------------------------------------------------------

static inline int32_t __not_in_flash_func(stationEnv)(
    int32_t tunePos, int32_t stEff, int32_t bandwidth, int32_t halfBand)
{
    int32_t dist = tunePos - stEff;
    if (dist < 0) dist = -dist;
    if (dist >= bandwidth) return 0;
    if (dist < halfBand)   return 4095;
    int32_t rem = bandwidth - dist;
    return rem * 21 + (rem >> 3); // ≈ rem * 21.125; avoids division
}

// ---------------------------------------------------------------------------
// OffAir card
// ---------------------------------------------------------------------------

class OffAir : public ComputerCard
{
    // --- Mode flags ---
    bool    altbootMode  = false; // true when broadcast clips replace live inputs
    bool    altbootArmed = false; // set once Switch is detected held at boot

    // --- Band (0=AM, 1=SW, 2=LW), changed by Switch Down tap ---
    uint8_t band = 0;

    // Mode-dependent parameters (updated when band changes)
    int32_t lpfAlpha      = 1287;
    int32_t clipThresh    = 1500;
    int32_t bandwidth     = 384;
    int32_t halfBand      = 192;
    int32_t noiseBlend    = 128;
    int32_t bpAlphaLoBand = kBpAlphaTable[0][0];
    int32_t bpAlphaHiBand = kBpAlphaTable[0][1];

    // --- Startup ---
    int32_t sampleCount = 0;
    int32_t fadeGain    = 0;

    // Boot animation
    int32_t bootStep  = 1;
    int32_t bootTimer = 0;

    // --- Control smoothers ---
    int32_t smMain = 2048;
    int32_t smX    = 0;
    int32_t smY    = 2048;
    int32_t smCV1  = 0;
    int32_t smCV2  = 0;

    // Tuning (frozen when PU2 high)
    int32_t tuneLocked = 2048;

    // --- Drift per station ---
    int32_t driftTarget1 = 0;
    int32_t driftSmooth1 = 0;
    int32_t driftTarget2 = 0;
    int32_t driftSmooth2 = 0;

    // --- DC-block per station (tau ≈ 43ms) ---
    int32_t hpState1 = 0;
    int32_t hpState2 = 0;

    // --- AM carrier oscillators ---
    // Station carriers are fixed to their dial positions.
    // Tuner LO tracks the main knob / CV and mixes against the received signal.
    uint32_t carrierPhase1 = 0;  // station 1 transmit carrier
    uint32_t carrierPhase2 = 0;  // station 2 transmit carrier
    uint32_t tunerPhase    = 0;  // local oscillator (tuner)

    // AM demodulator LPF — must be FAST (well above carrier freq) to remove
    // the 2×carrier ripple after rectification.  Fixed high alpha regardless of band.
    // alpha=3500 ≈ 12kHz cutoff — removes 2×carrier (2600-6000Hz) cleanly.
    int32_t demodLPF = 0;
    // Second-stage audio LPF applied after demod — uses band lpfAlpha
    int32_t demodAudioLPF = 0;
    // DC block after demodulator (removes carrier DC pedestal)
    int32_t demodHP  = 0;

    // --- AM audio LPF per station (applied before modulation) ---
    int32_t lpfState1 = 0;
    int32_t lpfState2 = 0;

    // --- Heavy LPF for sideband bleed (very narrow; ~300Hz) ---
    int32_t sideLPF1 = 0;
    int32_t sideLPF2 = 0;

    // --- Crackle highpass (on white noise) ---
    int32_t crackleHP = 0;

    // --- Bandpass noise resonator ---
    int32_t  bpY1     = 0;
    int32_t  bpY2     = 0;
    int32_t  bpAlpha  = kBpAlphaTable[0][1];
    uint32_t bpLFOPhase = 0;

    // --- Interference clip state (6 clips, 8kHz) ---
    // Start positions staggered so clips don't all loop at the same time
    uint32_t clipPos[6]    = {0, 40000, 80000, 20000, 60000, 100000};
    int32_t  clipFrac[6]   = {0, 0, 0, 0, 0, 0};
    int32_t  clipSample[6] = {0, 0, 0, 0, 0, 0};
    // Per-clip LPF state — each clip gets band-appropriate AM filtering
    int32_t  clipLPF[6]    = {0, 0, 0, 0, 0, 0};

    // --- Broadcast clip state (2 clips, 11025Hz) ---
    uint32_t bcastPos[2]   = {0, 0};
    int32_t  bcastFrac[2]  = {0, 0};   // fractional accumulator (0..kBcastDen)
    int32_t  bcastSample[2] = {0, 0};

    // --- Switch tap detection (mirrors chorgan working pattern) ---
    bool    swDownArmed = false; // true once switch has been released after boot
    int32_t swDownTimer = 0;    // counts samples while Down held

    // --- Pulse In 1 rising edge ---
    bool prevPU1 = false;

    void __not_in_flash_func(applyBand)(uint8_t b)
    {
        band = b;
        if      (b == 0) { lpfAlpha = 1287; clipThresh = 1500; bandwidth = 384; noiseBlend = 128; }
        else if (b == 1) { lpfAlpha = 1717; clipThresh = 1900; bandwidth = 512; noiseBlend = 64;  }
        else             { lpfAlpha = 782;  clipThresh = 1200; bandwidth = 256; noiseBlend = 192; }
        halfBand      = bandwidth >> 1;
        bpAlphaLoBand = kBpAlphaTable[b][0];
        bpAlphaHiBand = kBpAlphaTable[b][1];
        // Reset resonator so it doesn't carry stale energy into new band
        bpY1 = 0; bpY2 = 0;
        bpAlpha = bpAlphaHiBand;
    }

    // Step a broadcast clip forward by one ProcessSample call; returns current
    // sample as signed audio -2048..2047.  Fractional accumulator: at 48kHz call
    // rate, an 11025Hz clip advances 11025/48000 samples per call (< 1, no while).
    int32_t __not_in_flash_func(stepBcast)(int ci)
    {
        bcastFrac[ci] += kBcastNum;
        if (bcastFrac[ci] >= kBcastDen)
        {
            bcastFrac[ci] -= kBcastDen;
            bcastPos[ci]++;
            if (bcastPos[ci] >= kBcastClips[ci].len)
                bcastPos[ci] = 0;
            uint8_t raw = kBcastClips[ci].data[bcastPos[ci]];
            bcastSample[ci] = ((int32_t)raw - 128) * 16; // -2048..2032
        }
        return bcastSample[ci];
    }

public:
    OffAir() { applyBand(0); }

    void __not_in_flash_func(ProcessSample)() override
    {
        // -------------------------------------------------------------------
        // Smoothers — run every sample including during holdoff
        // -------------------------------------------------------------------
        smMain += (KnobVal(Knob::Main) - smMain) >> 6;
        smX    += (KnobVal(Knob::X)    - smX)    >> 6;
        smY    += (KnobVal(Knob::Y)    - smY)    >> 6;
        smCV1  += ((int32_t)CVIn1()    - smCV1)  >> 6;
        smCV2  += ((int32_t)CVIn2()    - smCV2)  >> 6;

        // Detect Switch Down held at boot (check from sample 4800 onward)
        if (!altbootArmed && sampleCount == 4800)
            altbootArmed = (SwitchVal() == Switch::Down);

        // -------------------------------------------------------------------
        // Holdoff
        // -------------------------------------------------------------------
        if (sampleCount < kHoldoffSamples)
        {
            sampleCount++;

            // Boot animation
            if (bootStep > 0 && bootStep <= kBootSteps)
            {
                for (int i = 0; i < 6; i++) LedBrightness(i, 0);
                LedBrightness((bootStep - 1) % 6, 4095);
                bootTimer++;
                if (bootTimer >= kBootStepLen) { bootTimer = 0; bootStep++; }
            }

            AudioOut1(0); AudioOut2(0);
            CVOut1(-2048); CVOut2(0);
            PulseOut1(false); PulseOut2(false);
            return;
        }

        // At the moment holdoff ends, latch altboot and confirm with LED flash
        if (sampleCount == kHoldoffSamples)
        {
            altbootMode = altbootArmed && (SwitchVal() == Switch::Down);
            if (altbootMode)
            {
                // Stagger broadcast clip start positions so they're not in phase
                bcastPos[0] = 0;
                bcastPos[1] = kBcastClips[1].len / 2;
                // Flash all LEDs briefly to confirm altboot
                for (int i = 0; i < 6; i++) LedBrightness(i, 4095);
            }
        }
        sampleCount++;

        // -------------------------------------------------------------------
        // Fade-in
        // -------------------------------------------------------------------
        if (fadeGain < 4095)
        {
            fadeGain += 4095 / kFadeInSamples;
            if (fadeGain > 4095) fadeGain = 4095;
        }

        // -------------------------------------------------------------------
        // Switch tap: cycle band — exact chorgan working pattern
        // -------------------------------------------------------------------
        Switch sw = SwitchVal();
        if (sw == Switch::Down)
        {
            swDownTimer++;
        }
        else
        {
            if (swDownArmed)
            {
                // Short tap (< 1s): advance band
                if (swDownTimer > 0 && swDownTimer < 48000)
                    applyBand((band + 1) % 3);
                swDownArmed = false;  // disarm until fully released
            }
            if (swDownTimer == 0) swDownArmed = true;  // re-arm after full release
            swDownTimer = 0;
        }

        // -------------------------------------------------------------------
        // Pulse In 1 rising edge — reset drift
        // -------------------------------------------------------------------
        bool pu1Now = PulseIn1();
        if (pu1Now && !prevPU1) { driftTarget1 = 0; driftTarget2 = 0; }
        prevPU1 = pu1Now;

        // -------------------------------------------------------------------
        // Tuning position (Pulse In 2 = freeze)
        // -------------------------------------------------------------------
        int32_t tunePos;
        if (PulseIn2())
        {
            tunePos = tuneLocked;
        }
        else
        {
            int32_t raw = smMain + (smCV1 >> 2);
            if (raw < 0)    raw = 0;
            if (raw > 4095) raw = 4095;
            tuneLocked = raw;
            tunePos    = raw;
        }

        // -------------------------------------------------------------------
        // Drift (per station, independent)
        // -------------------------------------------------------------------
        int32_t driftMax = (smX * kMaxDriftFull) >> 12;

        {
            uint32_t r = rng_next();
            if ((r & kDriftProbMask) == 0)
            {
                int32_t step = (int32_t)((r >> 22) & 3) + 1;
                if (r & (1u << 20)) step = -step;
                driftTarget1 += step;
                if (driftTarget1 >  driftMax) driftTarget1 =  driftMax;
                if (driftTarget1 < -driftMax) driftTarget1 = -driftMax;
            }
            driftSmooth1 += (driftTarget1 - driftSmooth1) >> 7;
        }
        {
            uint32_t r = rng_next();
            if ((r & kDriftProbMask) == 0)
            {
                int32_t step = (int32_t)((r >> 22) & 3) + 1;
                if (r & (1u << 20)) step = -step;
                driftTarget2 += step;
                if (driftTarget2 >  driftMax) driftTarget2 =  driftMax;
                if (driftTarget2 < -driftMax) driftTarget2 = -driftMax;
            }
            driftSmooth2 += (driftTarget2 - driftSmooth2) >> 7;
        }

        // -------------------------------------------------------------------
        // Live station envelopes
        // -------------------------------------------------------------------
        int32_t st1Eff = kStation1Pos + driftSmooth1;
        int32_t st2Eff = kStation2Pos + driftSmooth2;

        int32_t env1 = stationEnv(tunePos, st1Eff, bandwidth, halfBand);
        int32_t env2 = stationEnv(tunePos, st2Eff, bandwidth, halfBand);
        // sigStrength drives noise gate and CV Out 1 — use whichever station is stronger
        int32_t sigStrength = env1 > env2 ? env1 : env2;

        // -------------------------------------------------------------------
        // Station audio source: live inputs or broadcast clips
        // -------------------------------------------------------------------
        int32_t raw1, raw2;
        if (altbootMode)
        {
            raw1 = stepBcast(0);
            raw2 = stepBcast(1);
        }
        else
        {
            raw1 = (int32_t)AudioIn1();
            raw2 = (int32_t)AudioIn2();
        }

        // -------------------------------------------------------------------
        // Real AM encode/decode
        // -------------------------------------------------------------------
        // 1. DC block + bandlimit audio before modulation
        hpState1 += (raw1 - hpState1) >> 11;
        hpState2 += (raw2 - hpState2) >> 11;
        int32_t hp1 = raw1 - hpState1;
        int32_t hp2 = raw2 - hpState2;

        lpfState1 += ((hp1 - lpfState1) * lpfAlpha) >> 12;
        lpfState2 += ((hp2 - lpfState2) * lpfAlpha) >> 12;

        // -------------------------------------------------------------------
        // Real AM encode + envelope detect
        // -------------------------------------------------------------------
        // Step carrier phases. Station carriers fixed to their dial positions
        // (drift included).  Tuner tracks tunePos for beat-frequency whistle.
        carrierPhase1 += carrierInc(st1Eff);
        carrierPhase2 += carrierInc(st2Eff);
        tunerPhase    += carrierInc(tunePos);

        int32_t c1 = isin(carrierPhase1);   // -127..127
        int32_t c2 = isin(carrierPhase2);

        // AM encode: tx = (A + audio) * sin(fc)
        // A=160 ensures (A + audio) > 0 always (audio in -128..127 after >>4),
        // so no overmodulation / phase reversal.  Modulation index = 127/160 = 79%.
        int32_t audio1 = lpfState1 >> 4;    // -128..127
        int32_t audio2 = lpfState2 >> 4;
        int32_t tx1 = (160 + audio1) * c1;  // -36000..36000
        int32_t tx2 = (160 + audio2) * c2;

        // Weight each station by dial proximity; keep in int32 range
        // env 0..4095 → shift to 0..127
        int32_t rx1 = (tx1 >> 8) * (env1 >> 5);   // weighted tx from station 1
        int32_t rx2 = (tx2 >> 8) * (env2 >> 5);   // weighted tx from station 2
        int32_t rx  = (rx1 + rx2) >> 2;            // combined received signal

        // Envelope detect: abs + fast LPF (alpha=3500 ≈ 14kHz)
        // removes the carrier component (1200-3000 Hz) cleanly.
        int32_t rectified = rx < 0 ? -rx : rx;
        demodLPF += ((rectified - demodLPF) * 3500) >> 12;

        // Audio LPF (band-appropriate, shapes bandwidth)
        demodAudioLPF += ((demodLPF - demodAudioLPF) * lpfAlpha) >> 12;

        // DC block removes the AM pedestal (demod always outputs positive without it)
        demodHP += (demodAudioLPF - demodHP) >> 11;
        int32_t demodAC = demodAudioLPF - demodHP;

        // Scale up and soft-clip — demodAC is proportional to audio1 (>> 4 of lpfState)
        // ×16 restores amplitude; soft clip handles any remaining hot peaks
        int32_t stationMix = softClip(demodAC << 4, clipThresh);

        // Tuning whistle: additive beat tone between station carrier and tuner LO.
        // The beat frequency = |fc_station - fc_tuner|. When on-station this is 0 Hz
        // (DC, inaudible). Off-station it oscillates — the classic AM tuning whistle.
        // We generate it as an additive sine at the beat frequency, scaled by how
        // close we are to the station (env) and how far off-tune (beat depth fades
        // to zero when exactly on-station because the phase stops evolving).
        // Use cos (phase + 90°) so at zero-beat it's +1 (steady) not oscillating.
        int32_t whistle = 0;
        if (env1 > 0)
        {
            // Beat tone amplitude: strongest midway between zero-beat and far-off
            int32_t b = isin(carrierPhase1 - tunerPhase);   // -127..127, 0 at zero-beat DC
            whistle += (b * (env1 >> 5)) >> 3;              // scale to audio range
        }
        if (env2 > 0)
        {
            int32_t b = isin(carrierPhase2 - tunerPhase);
            whistle += (b * (env2 >> 5)) >> 3;
        }
        stationMix += whistle;

        // Sideband bleed uses the same demodulated signal, heavily LPF'd
        sideLPF1 += ((stationMix - sideLPF1) * 40) >> 12;
        int32_t side1 = stationEnv(tunePos, st1Eff, bandwidth + kSidebandExtra, bandwidth >> 1);
        int32_t side2 = stationEnv(tunePos, st2Eff, bandwidth + kSidebandExtra, bandwidth >> 1);
        if (env1 > 0 || env2 > 0) { side1 = 0; side2 = 0; }
        int32_t sideMix = ((sideLPF1 * (side1 + side2)) >> 12) >> 2;


        // -------------------------------------------------------------------
        // Baked interference clips — all six in every band, reordered per band
        // -------------------------------------------------------------------
        int32_t intfCV = smCV2 > 0 ? smCV2 << 1 : 0; // 0..4094
        int32_t intfMix = 0;

        for (int ci = 0; ci < 6; ci++)
        {
            const ClipDesc& clip = kAllClips[kClipOrder[band][ci]];

            clipFrac[ci] += kClipNum;
            if (clipFrac[ci] >= kClipDen)
            {
                clipFrac[ci] -= kClipDen;
                clipPos[ci]++;
                if (clipPos[ci] >= clip.len) clipPos[ci] = 0;
                uint8_t raw = clip.data[clipPos[ci]];
                clipSample[ci] = ((int32_t)raw - 128) * 16;
            }

            // AM filter: same LPF and soft clip as live stations
            clipLPF[ci] += ((clipSample[ci] - clipLPF[ci]) * lpfAlpha) >> 12;
            int32_t clipFiltered = softClip(clipLPF[ci], clipThresh);

            int32_t intfEnv = stationEnv(tunePos, kIntfPos[band][ci], kIntfBW, kIntfHalf);
            if (intfEnv == 0) continue;

            uint32_t r = rng_next();
            int32_t prob = (int32_t)(r >> 20);
            if (prob > intfCV) continue;

            intfMix += (clipFiltered * intfEnv) >> 12;
        }

        if (intfMix >  2047) intfMix =  2047;
        if (intfMix < -2048) intfMix = -2048;

        // -------------------------------------------------------------------
        // Noise: white + crackle blend, gated by signal strength
        // -------------------------------------------------------------------
        uint32_t r = rng_next();
        int32_t white = (int32_t)(r >> 20) - 2048;

        crackleHP += (white - crackleHP) >> 3;
        int32_t crackle  = white - crackleHP;
        int32_t noiseBase = (white * noiseBlend + crackle * (256 - noiseBlend)) >> 8;
        // Attenuate noise base — white noise at full scale is very loud
        noiseBase >>= 2;

        int32_t noiseLevel = smY;
        if (noiseLevel < 0)    noiseLevel = 0;
        if (noiseLevel > 4095) noiseLevel = 4095;

        // Soft gate: cube response so noise drops off fast once signal arrives,
        // but a small floor (3%) remains so on-station is never completely dead.
        // On-station (sigStrength=4095): gate = (1/4096)^3 * 4096^3 >> 24 ≈ 0 + floor
        int32_t noiseGate = 4096 - sigStrength;          // 0..4096
        noiseGate = (noiseGate * noiseGate) >> 12;       // square
        noiseGate = (noiseGate * noiseGate) >> 12;       // cube (~x^4 total)
        if (noiseGate < 130) noiseGate = 130;            // ~3% floor

        int32_t noiseSig  = (noiseBase * noiseLevel) >> 12;
        int32_t noise     = (noiseSig  * noiseGate)  >> 12;

        // -------------------------------------------------------------------
        // Bandpass noise (drifting carrier/heterodyne interference)
        // -------------------------------------------------------------------
        bpLFOPhase += kBpLFOInc;
        uint32_t lfoTop = bpLFOPhase >> 16;
        int32_t  lfoTri = (lfoTop < 32768u) ? (int32_t)lfoTop : (int32_t)(65535u - lfoTop);

        int32_t alphaRange = bpAlphaHiBand - bpAlphaLoBand;
        bpAlpha = bpAlphaLoBand + (int32_t)((int64_t)lfoTri * alphaRange >> 15);

        int32_t bpY0 = ((int64_t)bpAlpha * bpY1 >> 15) - bpY2 + (white >> 5);
        bpY2 = bpY1;
        bpY1 = bpY0;
        if (bpY1 >  4095) bpY1 =  4095;
        if (bpY1 < -4096) bpY1 = -4096;

        int32_t midDist = tunePos - 2047;
        if (midDist < 0) midDist = -midDist;
        int32_t bpGate = (midDist < 600) ? (600 - midDist) * 6 : 0;
        bpGate = (bpGate * (noiseGate >> 2)) >> 10;

        int32_t bpNoise = (bpY1 >> 3);
        bpNoise = (bpNoise * bpGate)    >> 12;
        bpNoise = (bpNoise * noiseLevel) >> 12;

        // -------------------------------------------------------------------
        // Final mix
        // -------------------------------------------------------------------
        int32_t noiseFull = noise + bpNoise + intfMix;
        if (noiseFull >  2047) noiseFull =  2047;
        if (noiseFull < -2048) noiseFull = -2048;

        int32_t outMain = stationMix + sideMix + noiseFull;
        if (outMain >  2047) outMain =  2047;
        if (outMain < -2048) outMain = -2048;

        if (fadeGain < 4095)
        {
            outMain   = (outMain   * fadeGain) >> 12;
            noiseFull = (noiseFull * fadeGain) >> 12;
        }

        AudioOut1((int16_t)outMain);
        AudioOut2((int16_t)noiseFull);

        // -------------------------------------------------------------------
        // CV and Pulse outputs
        // -------------------------------------------------------------------
        int32_t totalSig = (env1 + env2) >> 1;
        CVOut1((int16_t)(totalSig >> 1));
        CVOut2((int16_t)((env2 - env1) >> 1));
        PulseOut1((env1 > 3072) || (env2 > 3072));
        PulseOut2(totalSig < 512);

        // -------------------------------------------------------------------
        // LEDs
        // -------------------------------------------------------------------
        // LEDs 0/1: station signal strength
        LedBrightness(0, (uint16_t)env1);
        LedBrightness(1, (uint16_t)env2);

        // LED 2: noise level (Knob Y)
        LedBrightness(2, (uint16_t)noiseLevel);

        // LEDs 3/4/5: band indicator — one LED lit per band (AM/SW/LW)
        LedBrightness(3, band == 0 ? 4095 : 0);
        LedBrightness(4, band == 1 ? 4095 : 0);
        LedBrightness(5, band == 2 ? 4095 : 0);
    }
};

static OffAir card;

int main()
{
    set_sys_clock_khz(144000, true);
    card.Run();
}
