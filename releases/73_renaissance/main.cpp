/**
 * Spread / Renaissance - Workshop Computer program card
 * 6-voice harmonic spread oscillator. CV2+Knob Y morphs through stacked intervals
 * (unison → minor 3rds → major 3rds → 5ths → octaves) with soft landmark snapping.
 * Main knob morphs sine→triangle→saw. Switch selects detune amount.
 *
 * Controls:
 * - CV In 1:   Root pitch 1V/oct (0V = C4, summed with Knob X)
 * - Knob X:    Transpose ±1 octave (summed with CV1)
 * - CV In 2:   Spread CV (0V=unison, +5V=octave stack, summed with Knob Y)
 * - Knob Y:    Spread amount (summed with CV2)
 * - Main Knob: Timbre — sine (CCW) → triangle (mid) → saw (CW)
 * - Switch:    Detune — Up=none, Mid=on (step set by tapping Down)
 *
 * Outputs:
 * - Audio Out 1: 6-voice mix
 * - Audio Out 2: Same voices with slight phase offset (stereo width)
 *
 * LEDs:
 * - LEDs 0-4: Spread landmark indicator
 * - LED 5:    Timbre brightness (Switch Up) / detune step (Switch Mid/Down)
 */

#include "ComputerCard.h"
#include "pico/stdlib.h"
#include "hardware/clocks.h"
#include <cstdint>
#include <cstdlib>

#ifndef __not_in_flash_func
#define __not_in_flash_func(f) f
#endif

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

constexpr int32_t kChangeTolerance  = 64;
constexpr int32_t kCountsPerOctave  = 341;  // CV counts per octave (empirical)
constexpr int32_t kNumVoices        = 6;
constexpr int32_t kNumLandmarks     = 5;
constexpr int32_t kAmpPerVoice      = 10;   // out of 256; 6 voices * 10/256 * ±32000 >> 2 = ±1875
constexpr uint32_t kPhaseOffset     = 0x00800000u;  // 1/512 cycle stereo offset
// Detune ratio magnitudes (Q16.16 offset per voice unit, symmetric across 6 voices).
// Voice spread = ±2.5 * detuneScale / 65536 in pitch ratio terms.
// Switch Up = off (0), Switch Mid = on (cycles through steps via Down tap).
// 1 cent ≈ 655 in this unit (100 cents = 65536 * (2^(1/12)-1) / 12 ≈ 655/unit at mid-range).
// Steps: 3, 6, 10, 15, 20, 30, 40 cents on outermost voice.
constexpr int32_t kDetuneSteps[7] = { 393, 786, 1310, 1965, 2620, 3930, 5240 };
constexpr int32_t kNumDetuneSteps = 7;

// ---------------------------------------------------------------------------
// Lookup tables (from releases/18_chord_organ/lookup_tables.h)
// ---------------------------------------------------------------------------

constexpr int32_t SINE_TABLE[512] = {
    0, 392, 785, 1177, 1570, 1962, 2354, 2745,
    3136, 3527, 3917, 4306, 4695, 5083, 5470, 5857,
    6242, 6627, 7011, 7393, 7775, 8155, 8534, 8912,
    9289, 9664, 10037, 10409, 10780, 11149, 11516, 11882,
    12245, 12607, 12967, 13325, 13681, 14035, 14387, 14737,
    15084, 15429, 15772, 16113, 16451, 16786, 17119, 17450,
    17778, 18103, 18425, 18745, 19062, 19376, 19687, 19995,
    20300, 20602, 20901, 21197, 21489, 21779, 22065, 22348,
    22627, 22903, 23175, 23444, 23710, 23972, 24230, 24485,
    24736, 24983, 25227, 25466, 25702, 25934, 26162, 26386,
    26607, 26823, 27035, 27243, 27447, 27647, 27842, 28034,
    28221, 28404, 28583, 28757, 28927, 29093, 29254, 29411,
    29564, 29712, 29855, 29994, 30129, 30259, 30384, 30505,
    30622, 30733, 30840, 30943, 31041, 31134, 31222, 31306,
    31385, 31459, 31528, 31593, 31653, 31708, 31759, 31805,
    31845, 31882, 31913, 31939, 31961, 31978, 31990, 31997,
    32000, 31997, 31990, 31978, 31961, 31939, 31913, 31882,
    31845, 31805, 31759, 31708, 31653, 31593, 31528, 31459,
    31385, 31306, 31222, 31134, 31041, 30943, 30840, 30733,
    30622, 30505, 30384, 30259, 30129, 29994, 29855, 29712,
    29564, 29411, 29254, 29093, 28927, 28757, 28583, 28404,
    28221, 28034, 27842, 27647, 27447, 27243, 27035, 26823,
    26607, 26386, 26162, 25934, 25702, 25466, 25227, 24983,
    24736, 24485, 24230, 23972, 23710, 23444, 23175, 22903,
    22627, 22348, 22065, 21779, 21489, 21197, 20901, 20602,
    20300, 19995, 19687, 19376, 19062, 18745, 18425, 18103,
    17778, 17450, 17119, 16786, 16451, 16113, 15772, 15429,
    15084, 14737, 14387, 14035, 13681, 13325, 12967, 12607,
    12245, 11882, 11516, 11149, 10780, 10409, 10037, 9664,
    9289, 8912, 8534, 8155, 7775, 7393, 7011, 6627,
    6242, 5857, 5470, 5083, 4695, 4306, 3917, 3527,
    3136, 2745, 2354, 1962, 1570, 1177, 785, 392,
    0, -392, -785, -1177, -1570, -1962, -2354, -2745,
    -3136, -3527, -3917, -4306, -4695, -5083, -5470, -5857,
    -6242, -6627, -7011, -7393, -7775, -8155, -8534, -8912,
    -9289, -9664, -10037, -10409, -10780, -11149, -11516, -11882,
    -12245, -12607, -12967, -13325, -13681, -14035, -14387, -14737,
    -15084, -15429, -15772, -16113, -16451, -16786, -17119, -17450,
    -17778, -18103, -18425, -18745, -19062, -19376, -19687, -19995,
    -20300, -20602, -20901, -21197, -21489, -21779, -22065, -22348,
    -22627, -22903, -23175, -23444, -23710, -23972, -24230, -24485,
    -24736, -24983, -25227, -25466, -25702, -25934, -26162, -26386,
    -26607, -26823, -27035, -27243, -27447, -27647, -27842, -28034,
    -28221, -28404, -28583, -28757, -28927, -29093, -29254, -29411,
    -29564, -29712, -29855, -29994, -30129, -30259, -30384, -30505,
    -30622, -30733, -30840, -30943, -31041, -31134, -31222, -31306,
    -31385, -31459, -31528, -31593, -31653, -31708, -31759, -31805,
    -31845, -31882, -31913, -31939, -31961, -31978, -31990, -31997,
    -32000, -31997, -31990, -31978, -31961, -31939, -31913, -31882,
    -31845, -31805, -31759, -31708, -31653, -31593, -31528, -31459,
    -31385, -31306, -31222, -31134, -31041, -30943, -30840, -30733,
    -30622, -30505, -30384, -30259, -30129, -29994, -29855, -29712,
    -29564, -29411, -29254, -29093, -28927, -28757, -28583, -28404,
    -28221, -28034, -27842, -27647, -27447, -27243, -27035, -26823,
    -26607, -26386, -26162, -25934, -25702, -25466, -25227, -24983,
    -24736, -24485, -24230, -23972, -23710, -23444, -23175, -22903,
    -22627, -22348, -22065, -21779, -21489, -21197, -20901, -20602,
    -20300, -19995, -19687, -19376, -19062, -18745, -18425, -18103,
    -17778, -17450, -17119, -16786, -16451, -16113, -15772, -15429,
    -15084, -14737, -14387, -14035, -13681, -13325, -12967, -12607,
    -12245, -11882, -11516, -11149, -10780, -10409, -10037, -9664,
    -9289, -8912, -8534, -8155, -7775, -7393, -7011, -6627,
    -6242, -5857, -5470, -5083, -4695, -4306, -3917, -3527,
    -3136, -2745, -2354, -1962, -1570, -1177, -785, -392
};

constexpr uint32_t MIDI_PHASE_INC[128] = {
    731558u, 775058u, 821146u, 869974u, 921705u, 976512u, 1034579u, 1096098u,
    1161276u, 1230329u, 1303488u, 1380997u, 1463116u, 1550117u, 1642292u, 1739948u,
    1843410u, 1953025u, 2069158u, 2192197u, 2322552u, 2460658u, 2606976u, 2761995u,
    2926232u, 3100235u, 3284584u, 3479896u, 3686821u, 3906051u, 4138317u, 4384394u,
    4645104u, 4921316u, 5213953u, 5523991u, 5852464u, 6200470u, 6569169u, 6959792u,
    7373643u, 7812103u, 8276635u, 8768789u, 9290208u, 9842633u, 10427906u, 11047982u,
    11704929u, 12400940u, 13138339u, 13919585u, 14747287u, 15624206u, 16553270u, 17537578u,
    18580417u, 19685266u, 20855813u, 22095964u, 23409859u, 24801881u, 26276678u, 27839171u,
    29494574u, 31248413u, 33106540u, 35075157u, 37160835u, 39370533u, 41711627u, 44191929u,
    46819718u, 49603763u, 52553357u, 55678342u, 58989149u, 62496826u, 66213081u, 70150315u,
    74321670u, 78741067u, 83423254u, 88383859u, 93639437u, 99207527u, 105106714u, 111356684u,
    117978298u, 124993652u, 132426162u, 140300631u, 148643341u, 157482134u, 166846509u, 176767718u,
    187278874u, 198415055u, 210213429u, 222713369u, 235956596u, 249987305u, 264852324u, 280601262u,
    297286682u, 314964268u, 333693018u, 353535437u, 374557748u, 396830111u, 420426858u, 445426739u,
    471913192u, 499974610u, 529704648u, 561202525u, 594573364u, 629928536u, 667386036u, 707070875u,
    749115497u, 793660223u, 840853716u, 890853479u, 943826384u, 999949221u, 1059409296u, 1122405051u,
};

// ---------------------------------------------------------------------------
// Spread landmark table
// [landmark][voice] = semitone offset above root.
// Entry [5] is a sentinel duplicate of [4] to allow safe interpolation at max.
// ---------------------------------------------------------------------------
constexpr int8_t kLandmarkOffsets[6][6] = {
    {  0,  0,  0,  0,  0,  0 },  // 0: unison
    {  0,  3,  6,  9, 12, 15 },  // 1: minor 3rd stack
    {  0,  4,  8, 12, 16, 20 },  // 2: major 3rd stack
    {  0,  7, 12, 19, 24, 31 },  // 3: perfect 5th stack
    {  0, 12, 24, 36, 48, 60 },  // 4: octave stack
    {  0, 12, 24, 36, 48, 60 },  // 5: sentinel
};

// ---------------------------------------------------------------------------
// SpreadCard
// ---------------------------------------------------------------------------

class SpreadCard : public ComputerCard {
public:
    SpreadCard() {
        updateTargets();
    }

protected:
    void __not_in_flash_func(ProcessSample)() override;

private:
    // Oscillator state
    uint32_t phase[kNumVoices]          = {};
    uint32_t phaseIncBase[kNumVoices]   = {};
    uint32_t phaseIncDetune[kNumVoices] = {};

    // Quantized control state
    int32_t  rootQuant   = 60;
    int32_t  spreadZone  = 0;    // 0..4
    int32_t  spreadFrac  = 0;    // 0..1023
    int32_t  detuneScale = 0;    // Q16.16 offset magnitude per voice unit

    // Smoothed raw values (for change detection)
    int32_t  prevCvRoot    = 60;  // CV1-derived root only (so knobX doesn't mask CV1 changes)
    int32_t  prevKnobXSemi = 0;
    int32_t  prevDetune    = 0;

    // Control smoothing accumulators
    int32_t  knobXSmoothed    = 2048;
    int32_t  knobYSmoothed    = 2048;
    int32_t  cv1Smoothed      = 0;
    int32_t  cv2Smoothed      = 0;
    int32_t  mainKnobSmoothed = 2048;

    // Switch / detune state
    int32_t  detuneStep   = 0;    // 0..6, index into kDetuneSteps
    bool     downArmed    = true; // false while Down held, resets on release

    // Helpers
    void updateTargets();
    static inline uint32_t phaseIncFrac(int note_lo, int note_hi, int32_t frac);
    static inline int32_t __not_in_flash_func(sineSample)(uint32_t ph);
    static inline int32_t __not_in_flash_func(triSample)(uint32_t ph);
    static inline int32_t __not_in_flash_func(sawSample)(uint32_t ph);
    static inline int32_t __not_in_flash_func(blendWaveform)(uint32_t ph, int32_t shape);
};

// ---------------------------------------------------------------------------
// Waveform helpers
// ---------------------------------------------------------------------------

inline int32_t SpreadCard::sineSample(uint32_t ph) {
    uint32_t index = ph >> 23;
    uint32_t frac  = (ph & 0x7FFFFFu) >> 7;
    int32_t s1 = SINE_TABLE[index & 0x1FFu];
    int32_t s2 = SINE_TABLE[(index + 1u) & 0x1FFu];
    return (s2 * (int32_t)frac + s1 * (int32_t)(65536u - frac)) >> 16;
}

inline int32_t SpreadCard::triSample(uint32_t ph) {
    // Triangle: ±32767, zero-crossing at 0 and 0x80000000
    if (ph < 0x40000000u)
        return (int32_t)(ph >> 15);          // 0 → +32767
    else if (ph < 0xC0000000u)
        return 32767 - (int32_t)((ph - 0x40000000u) >> 14);  // +32767 → -32767
    else
        return (int32_t)(ph >> 15) - 131072; // -32767 → 0
}

inline int32_t SpreadCard::sawSample(uint32_t ph) {
    // Saw: ±~31250
    return ((int32_t)(ph >> 16) - 32768) * 1000 >> 10;
}

inline int32_t SpreadCard::blendWaveform(uint32_t ph, int32_t shape) {
    // shape 0..4095: 0..1365 = sine→tri, 1366..2730 = tri→saw, 2731..4095 = full saw
    if (shape < 1366) {
        const int32_t t = (shape * 3) >> 2;   // 0..1023
        const int32_t s = sineSample(ph);
        const int32_t r = triSample(ph);
        return s + ((r - s) * t >> 10);
    } else if (shape < 2731) {
        const int32_t t = ((shape - 1366) * 3) >> 2;  // 0..1023
        const int32_t s = triSample(ph);
        const int32_t r = sawSample(ph);
        return s + ((r - s) * t >> 10);
    } else {
        return sawSample(ph);
    }
}

// ---------------------------------------------------------------------------
// Pitch interpolation
// ---------------------------------------------------------------------------

inline uint32_t SpreadCard::phaseIncFrac(int note_lo, int note_hi, int32_t frac) {
    if (note_lo < 0)   note_lo = 0;
    if (note_lo > 127) note_lo = 127;
    if (note_hi < 0)   note_hi = 0;
    if (note_hi > 127) note_hi = 127;
    int32_t lo = (int32_t)MIDI_PHASE_INC[note_lo];
    int32_t hi = (int32_t)MIDI_PHASE_INC[note_hi];
    // frac 0..1023 (Q10)
    return (uint32_t)(lo + ((hi - lo) * frac >> 10));
}

// ---------------------------------------------------------------------------
// updateTargets — recompute phase increments from current quantized state.
// Called only on control change, not in the hot path.
// ---------------------------------------------------------------------------

void SpreadCard::updateTargets() {
    for (int i = 0; i < kNumVoices; i++) {
        int32_t off_lo = kLandmarkOffsets[spreadZone][i];
        int32_t off_hi = kLandmarkOffsets[spreadZone + 1][i];
        // offsetQ10 = fractional semitone offset above root (Q10, i.e. *1024)
        int32_t offsetQ10 = off_lo * 1024 + (off_hi - off_lo) * spreadFrac;
        int note_lo = rootQuant + (offsetQ10 >> 10);
        int note_hi = note_lo + 1;
        int32_t noteFrac = offsetQ10 & 0x3FF;
        phaseIncBase[i] = phaseIncFrac(note_lo, note_hi, noteFrac);
    }

    // Apply symmetric detune ratios (Q16.16). Voices spread: -5/2, -3/2, -1/2, +1/2, +3/2, +5/2 * detuneScale
    for (int i = 0; i < kNumVoices; i++) {
        // (2*i - 5) gives -5, -3, -1, +1, +3, +5
        int32_t ratioOffset = ((2 * i - 5) * detuneScale) >> 1;
        int32_t ratio = 65536 + ratioOffset;  // Q16.16
        phaseIncDetune[i] = (uint32_t)((int64_t)phaseIncBase[i] * ratio >> 16);
    }
}

// ---------------------------------------------------------------------------
// ProcessSample
// ---------------------------------------------------------------------------

void SpreadCard::ProcessSample() {
    // 1. Read controls
    int32_t mainKnob = KnobVal(Knob::Main);
    int32_t knobX    = KnobVal(Knob::X);
    int32_t knobY    = KnobVal(Knob::Y);
    int32_t cv1      = CVIn1();   // -2048..2047, 0V=0, 1V/oct=341 counts/oct
    int32_t cv2      = CVIn2();   // -2048..2047, 0V=0

    // 2. Smooth controls (IIR, ~30 sample time constant)
    mainKnobSmoothed += (mainKnob - mainKnobSmoothed) >> 5;
    knobXSmoothed    += (knobX    - knobXSmoothed)    >> 5;
    knobYSmoothed    += (knobY    - knobYSmoothed)    >> 5;
    cv1Smoothed      += (cv1      - cv1Smoothed)      >> 5;
    cv2Smoothed      += (cv2      - cv2Smoothed)      >> 5;

    // 3. Root pitch.
    // CV1: 1V/oct, 0V=MIDI 60. 341 counts/octave, quantised to nearest semitone.
    int32_t cv1Semi = cv1Smoothed * 12;
    int32_t cvRoot  = 60 + (cv1Semi + (cv1Semi >= 0 ? kCountsPerOctave/2 : -kCountsPerOctave/2))
                            / kCountsPerOctave;
    // Knob X: bipolar ±1 octave (±12 semitones). Centre=no offset.
    // (knobX - 2048) ranges -2048..2047; * 12 / 2048 gives -12..+11.
    int32_t knobXSemi = ((knobXSmoothed - 2048) * 12) >> 11;
    int32_t newRoot   = cvRoot + knobXSemi;
    if (newRoot < 0)   newRoot = 0;
    if (newRoot > 127) newRoot = 127;

    // 4. Spread: Knob Y selects one of 5 landmarks directly.
    // CV2 offsets the knob bipolarly. Combined value → 0..4095 → quantised to 0..4.
    int32_t spreadRaw = knobYSmoothed + cv2Smoothed;
    if (spreadRaw < 0)    spreadRaw = 0;
    if (spreadRaw > 4095) spreadRaw = 4095;
    // 5 equal zones of 819 counts each → landmark 0..4
    int32_t newSpreadZone = spreadRaw * 5 / 4096;
    if (newSpreadZone > 4) newSpreadZone = 4;

    // 5. Switch: Up = detune off. Mid = detune on (at current step).
    //    Tapping Down advances detuneStep (debounced, armed on release).
    Switch sw = SwitchVal();
    if (sw == Switch::Down && downArmed) {
        detuneStep = (detuneStep + 1) % kNumDetuneSteps;
        downArmed  = false;
    }
    if (sw != Switch::Down) downArmed = true;

    int32_t newDetuneScale = (sw == Switch::Up) ? 0 : kDetuneSteps[detuneStep];

    // 6. Update targets if anything changed
    bool changed = false;
    if (abs(cvRoot - prevCvRoot) > kChangeTolerance || knobXSemi != prevKnobXSemi) {
        rootQuant    = newRoot;
        prevCvRoot   = cvRoot;
        prevKnobXSemi = knobXSemi;
        changed      = true;
    }
    if (newSpreadZone != spreadZone) {
        spreadZone = newSpreadZone;
        spreadFrac = 0;
        changed    = true;
    }
    if (newDetuneScale != prevDetune) {
        detuneScale = newDetuneScale;
        prevDetune  = newDetuneScale;
        changed     = true;
    }
    if (changed) {
        updateTargets();
    }

    // 8. Oscillator loop — 6 voices
    int32_t mix1 = 0;
    int32_t mix2 = 0;
    for (int i = 0; i < kNumVoices; i++) {
        phase[i] += phaseIncDetune[i];
        int32_t s1 = blendWaveform(phase[i],                mainKnobSmoothed);
        int32_t s2 = blendWaveform(phase[i] + kPhaseOffset, mainKnobSmoothed);
        mix1 += (s1 * kAmpPerVoice) >> 8;
        mix2 += (s2 * kAmpPerVoice) >> 8;
    }

    mix1 >>= 2;
    mix2 >>= 2;
    if (mix1 >  2047) mix1 =  2047;
    if (mix1 < -2048) mix1 = -2048;
    if (mix2 >  2047) mix2 =  2047;
    if (mix2 < -2048) mix2 = -2048;

    AudioOut1((int16_t)mix1);
    AudioOut2((int16_t)mix2);

    // 9. LEDs
    // LEDs 0–4: current landmark (one LED per zone)
    for (int z = 0; z < 5; z++) {
        LedOn(z, spreadZone == z);
    }
    // LED 5: timbre brightness when detune off; detune step when on
    if (sw == Switch::Up) {
        LedBrightness(5, mainKnobSmoothed);
    } else {
        LedBrightness(5, (detuneStep * 4095) / (kNumDetuneSteps - 1));
    }
}

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------

SpreadCard card;

int main() {
    set_sys_clock_khz(144000, true);
    card.Run();
}
