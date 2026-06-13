/**
 * Chorgan - Workshop Computer program card
 * 6-voice harmonic oscillator — experimental sandbox branched from Renaissance.
 *
 * Controls:
 * - CV In 1:   Root pitch 1V/oct (0V = C4, summed with Knob X)
 * - Knob X:    Master tune ±12 semitones (continuous, summed with CV1)
 * - Knob Y:    Interval — root note to second note, 0 (unison) → 12 (octave) semitones
 * - CV In 2:   Timbre offset — bipolar, offsets the Main knob position
 * - Main Knob: Timbre — SAW (CCW) → TRI → SINE (centre) → TRI → SAW (CW)
 * - Switch Up: detune level (CCW=10c, CW=16c)
 * - Switch Mid: detune level (CCW=0c, CW=5c)
 * - Tap Switch Down: cycle chord extensions (voices 3–6) for current interval
 *
 * Outputs:
 * - Audio Out 1: 6-voice mix
 * - Audio Out 2: Same voices with per-voice phase offsets (stereo width)
 * - Pulse Out 1: Sub-octave square wave (one octave below root)
 * - Pulse Out 2: PWM square at same frequency, duty swept by LFO
 *
 * Voice layout:
 * - Voice 1: always root
 * - Voice 2: always root + Y interval
 * - Voices 3–6: chord extensions, cycled by tapping switch Down or Pulse In 1
 *
 * LEDs:
 * - LEDs 0–4: interval indicator (maps 0–12 semitones across 5 LEDs)
 * - LED 5:    extension preset index (dim = preset 0, bright = preset 5)
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

constexpr int32_t  kCountsPerOctave  = 341;
constexpr int32_t  kMaxChords        = 8;
constexpr uint32_t kHoldSamples      = 48000;
constexpr int32_t  kKnobMaxSlew      = 200;    // max counts/sample — blocks ADC corruption jumps
constexpr int32_t kNumVoices        = 6;
constexpr int32_t kNumIntervals     = 13;   // 0..12 semitones
constexpr int32_t kNumPresets       = 6;    // extension presets per interval
constexpr int32_t kAmpPerVoice      = 10;   // out of 256; sum of 6 voices >> 2 stays in ±2047
// Per-voice phase offsets for Out2 — each voice offset by a different amount
// so the two outputs have genuinely different phase content. Spread across ~90°.
// Voice i gets i * 1/24 cycle offset (0, 15, 30, 45, 60, 75 degrees).
constexpr uint32_t kStereoOffsets[6] = {
    0x00000000u,  // voice 0:  0°
    0x0AAAAAA0u,  // voice 1: 15°
    0x15555540u,  // voice 2: 30°
    0x20000000u,  // voice 3: 45°
    0x2AAAAAA0u,  // voice 4: 60°
    0x35555540u,  // voice 5: 75°
};

// ---------------------------------------------------------------------------
// Lookup tables
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
// Chord extension table
// kExtensions[interval][preset][voice_ext_index] — semitone offsets above root
// for voices 2–5 (indices 0..3 here). Voice 0 = root (0), Voice 1 = root+interval.
// Presets cycle through harmonic options for each two-note interval.
// ---------------------------------------------------------------------------

// interval 0: unison — build upward with 5ths and octaves
constexpr int8_t kExt_0[6][4] = {
    {  7, 12, 19, 24 },  // root + 5th + oct + 5th+oct
    { 12, 19, 24, 31 },  // octave stack
    {  7, 12, 24, 31 },  // open 5ths
    {  7, 19, 24, 36 },  // spread 5ths
    { 12, 24, 36, 48 },  // octave stack wide
    {  5,  7, 12, 17 },  // sus cluster
};

// interval 1: minor 2nd — dissonant, use spread to open it up
constexpr int8_t kExt_1[6][4] = {
    { 12, 13, 24, 25 },  // octave doubling of the cluster
    {  7, 12, 19, 24 },  // add 5ths — opens it up
    { 12, 19, 24, 25 },  // 5th + oct + top crunch
    {  7, 13, 19, 24 },  // 5th framing the m2
    { 12, 24, 25, 36 },  // wide with top crunch
    {  1,  7, 12, 13 },  // cluster below and above
};

// interval 2: major 2nd — sus2 family
constexpr int8_t kExt_2[6][4] = {
    {  7, 12, 14, 19 },  // sus2 with 5th
    {  7, 14, 19, 24 },  // open sus2
    {  2,  7, 12, 14 },  // add below
    { 14, 19, 21, 26 },  // 9th chord upper structure
    {  7, 12, 21, 26 },  // 9th with 5th
    {  2, 14, 19, 26 },  // stacked 9ths
};

// interval 3: minor 3rd — minor chords
constexpr int8_t kExt_3[6][4] = {
    {  7, 12, 19, 24 },  // minor triad + oct
    {  7, 10, 12, 19 },  // min7
    {  7, 12, 15, 19 },  // min add11 / quartal
    {  3,  7, 10, 12 },  // min7 close
    {  7, 12, 19, 22 },  // min7 open
    {  7, 15, 19, 24 },  // min11
};

// interval 4: major 3rd — major chords
constexpr int8_t kExt_4[6][4] = {
    {  7, 12, 16, 19 },  // major triad + oct
    {  7, 11, 12, 16 },  // maj7
    {  4,  7, 12, 16 },  // major triad doubling
    {  7, 11, 16, 19 },  // maj7 open
    {  4, 12, 16, 19 },  // add9
    {  7, 14, 16, 19 },  // maj9
};

// interval 5: perfect 4th — sus4 / quartal
constexpr int8_t kExt_5[6][4] = {
    {  7, 12, 17, 19 },  // sus4 with 5th
    {  5,  7, 12, 17 },  // quartal
    {  7, 12, 17, 24 },  // sus4 open
    {  5, 12, 17, 22 },  // quartal stack
    {  7, 17, 19, 24 },  // sus4 spread
    {  5,  7, 17, 24 },  // wide quartal
};

// interval 6: tritone — augmented / diminished
constexpr int8_t kExt_6[6][4] = {
    {  3,  6,  9, 12 },  // diminished 7th
    {  6,  9, 12, 15 },  // dim7 inversion
    {  6, 12, 18, 24 },  // tritone stack
    {  4,  6, 10, 12 },  // dom7b5
    {  6, 12, 15, 18 },  // dim + oct
    {  3,  6, 12, 15 },  // half-dim
};

// interval 7: perfect 5th — power / major / modal
constexpr int8_t kExt_7[6][4] = {
    { 12, 19, 24, 31 },  // power chord wide
    {  4,  7, 12, 16 },  // major triad
    {  7, 11, 12, 19 },  // dom7
    {  7, 12, 14, 19 },  // add9
    {  4,  7, 16, 19 },  // major spread
    {  3,  7, 10, 12 },  // min7 on root
};

// interval 8: minor 6th — minor inversions / augmented
constexpr int8_t kExt_8[6][4] = {
    {  3,  7, 12, 15 },  // minor + 5th
    {  4,  8, 12, 15 },  // augmented
    {  8, 12, 15, 20 },  // aug inversion
    {  7, 12, 15, 19 },  // min add11
    {  3,  8, 15, 20 },  // aug / dim spread
    {  8, 15, 19, 24 },  // wide aug
};

// interval 9: major 6th — 6th chords / major
constexpr int8_t kExt_9[6][4] = {
    {  4,  7, 12, 16 },  // major triad
    {  4,  7,  9, 16 },  // maj6
    {  7,  9, 12, 16 },  // maj6 open
    {  4,  9, 16, 21 },  // 6/9
    {  7, 12, 16, 21 },  // maj6 wide
    {  2,  4,  9, 16 },  // add9/6
};

// interval 10: minor 7th — dominant 7th / blues
constexpr int8_t kExt_10[6][4] = {
    {  4,  7, 12, 16 },  // dom7 (major triad + min7)
    {  4,  7, 10, 14 },  // dom9
    {  2,  4,  7, 10 },  // dom9 close
    {  7, 10, 12, 19 },  // dom7 open
    {  4, 10, 12, 16 },  // dom7 alt
    {  6,  7, 10, 13 },  // dom7b5b9 — altered
};

// interval 11: major 7th — maj7 / lydian
constexpr int8_t kExt_11[6][4] = {
    {  4,  7, 12, 16 },  // maj7 (add 5th + 3rd)
    {  4,  7, 11, 16 },  // maj7 close
    {  7, 11, 12, 16 },  // maj7 open
    {  2,  4,  7, 11 },  // maj9
    {  4, 11, 16, 18 },  // maj7#11 (lydian)
    {  6, 11, 16, 18 },  // lydian spread
};

// interval 12: octave — octave stacks / power
constexpr int8_t kExt_12[6][4] = {
    {  7, 12, 19, 24 },  // add 5ths
    { 12, 19, 24, 31 },  // octave + 5th stack
    {  7, 19, 24, 36 },  // wide open 5ths
    { 12, 24, 31, 36 },  // oct stack with 5th
    {  4,  7, 12, 16 },  // major triad
    {  5,  7, 12, 17 },  // sus/quartal
};

// Pointer table — index by interval
constexpr const int8_t* kExtensions[13] = {
    &kExt_0[0][0],
    &kExt_1[0][0],
    &kExt_2[0][0],
    &kExt_3[0][0],
    &kExt_4[0][0],
    &kExt_5[0][0],
    &kExt_6[0][0],
    &kExt_7[0][0],
    &kExt_8[0][0],
    &kExt_9[0][0],
    &kExt_10[0][0],
    &kExt_11[0][0],
    &kExt_12[0][0],
};

// ---------------------------------------------------------------------------
// ChorganCard
// ---------------------------------------------------------------------------

class ChorganCard : public ComputerCard {
public:
    ChorganCard() {
        updateTargets();
    }

protected:
    void __not_in_flash_func(ProcessSample)() override;

private:
    // Oscillator state
    uint32_t phase[kNumVoices]          = {};
    uint32_t phaseIncBase[kNumVoices]   = {};

    // Control state
    // Chord shape: all voices computed relative to MIDI 60, stored in phaseIncBase[].
    // Tuning: a single Q16.16 ratio (X+CV1) multiplied onto every phaseIncBase[] uniformly.
    uint32_t tuningRatio     = 65536;  // Q16.16, 1.0 = MIDI 60
    uint32_t prevTuningRatio = 65536;
    int32_t  intervalSemi    = 0;    // integer semitone 0..12, Y knob
    int32_t  prevIntervalSemi = 0;
    int32_t  preset          = 0;    // 0..5, which extension voicing
    int32_t  prevPreset      = -1;   // force initial compute

    // Control smoothing accumulators
    int32_t  knobXSmoothed    = 2048;
    int32_t  knobYSmoothed    = 2048;
    int32_t  cv1Smoothed      = 0;
    int32_t  cv2Smoothed      = 0;
    int32_t  mainKnobSmoothed = 2048;

    // Slew-rate limiters — protect against single-sample ADC corruption jumps
    int32_t  knobXPrev         = 2048;
    int32_t  knobYPrev         = 2048;
    int32_t  mainKnobPrev      = 2048;

    // Switch state
    bool     downArmed    = true;

    // Pulse Out 2 PWM LFO
    uint32_t pwmPhase     = 0;

    // Chord sequencer
    struct ChordState {
        uint32_t tuningRatio;
        int32_t  intervalSemi;
        int32_t  preset;
    };
    ChordState chordSeq[kMaxChords] = {};
    int32_t    chordCount    = 0;
    int32_t    chordWriteIdx = 0;
    int32_t    chordPlayIdx  = 0;

    // Override state (set when a chord is recalled via PulseIn2)
    bool       chordOverride      = false;
    uint32_t   overrideTuning     = 65536;  // recalled chord's tuning
    int32_t    overrideInterval   = 0;      // recalled chord's interval
    int32_t    overridePreset     = 0;      // recalled chord's preset
    int32_t    overrideSlot       = 0;      // slot index just recalled (for LED display)
    uint32_t   overrideBaseTuning = 65536;  // X/CV1 voltage at moment of recall
    int32_t    overrideBaseInterval = 0;    // Y voltage at moment of recall (semitones)

    // Hold detection — switch
    bool       holdArmed   = false;
    uint32_t   holdTimer   = 0;

    // Chord store state — tracks pending store slot for LED feedback
    int32_t    pendingStoreSlot = -1;  // slot being stored into (-1 = none)

    // PulseIn2 arm guard — stays false until PU2 seen low (prevents boot glitch)
    bool       pu2Armed = false;

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

inline int32_t ChorganCard::sineSample(uint32_t ph) {
    uint32_t index = ph >> 23;
    uint32_t frac  = (ph & 0x7FFFFFu) >> 7;
    int32_t s1 = SINE_TABLE[index & 0x1FFu];
    int32_t s2 = SINE_TABLE[(index + 1u) & 0x1FFu];
    return (int32_t)(((int64_t)s2 * frac + (int64_t)s1 * (65536u - frac)) >> 16);
}

inline int32_t ChorganCard::triSample(uint32_t ph) {
    if (ph < 0x40000000u)
        return (int32_t)(ph >> 15);
    else if (ph < 0xC0000000u)
        return 32767 - (int32_t)((ph - 0x40000000u) >> 15);
    else
        return (int32_t)(ph >> 15) - 131072;
}

inline int32_t ChorganCard::sawSample(uint32_t ph) {
    return ((int32_t)(ph >> 16) - 32768) * 1000 >> 10;
}

inline int32_t ChorganCard::blendWaveform(uint32_t ph, int32_t shape) {
    // shape 0..4095, centre (2048) = pure SAW, fully CCW or CW = pure SINE.
    // Fold into distance from centre (0=centre/SAW, 2047=edge/SINE), then invert.
    // Zones: 0..511 = SINE→TRI (512 wide), 512..2047 = TRI→SAW (1536 wide)
    int32_t d = shape - 2048;
    if (d < 0) d = -d;
    if (d > 2047) d = 2047;
    int32_t s = 2047 - d;  // 0 = edge (SINE), 2047 = centre (SAW)

    if (s < 512) {
        const int32_t t = s * 2;
        const int32_t a = sineSample(ph);
        const int32_t b = triSample(ph);
        return a + ((b - a) * t >> 10);
    } else {
        const int32_t t = ((s - 512) * 683) >> 10;
        const int32_t a = triSample(ph);
        const int32_t b = sawSample(ph);
        return a + ((b - a) * t >> 10);
    }
}

// ---------------------------------------------------------------------------
// Pitch interpolation
// ---------------------------------------------------------------------------

inline uint32_t ChorganCard::phaseIncFrac(int note_lo, int note_hi, int32_t frac) {
    if (note_lo < 0)   note_lo = 0;
    if (note_lo > 127) note_lo = 127;
    if (note_hi < 0)   note_hi = 0;
    if (note_hi > 127) note_hi = 127;
    int32_t lo = (int32_t)MIDI_PHASE_INC[note_lo];
    int32_t hi = (int32_t)MIDI_PHASE_INC[note_hi];
    return (uint32_t)(lo + ((hi - lo) * frac >> 10));
}

// ---------------------------------------------------------------------------
// updateTargets
// ---------------------------------------------------------------------------

void ChorganCard::updateTargets() {
    // Step 1: compute chord shape relative to MIDI 60 (no tuning applied yet).
    phaseIncBase[0] = MIDI_PHASE_INC[60];
    phaseIncBase[1] = MIDI_PHASE_INC[60 + intervalSemi];

    const int8_t* ext = kExtensions[intervalSemi] + preset * 4;
    for (int i = 0; i < 4; i++) {
        int32_t n = 60 + (int32_t)ext[i];
        phaseIncBase[2 + i] = phaseIncFrac(n, n + 1, 0);
    }

    // Step 2: apply tuning ratio uniformly to all voices.
    for (int i = 0; i < kNumVoices; i++) {
        phaseIncBase[i] = (uint32_t)((uint64_t)phaseIncBase[i] * tuningRatio >> 16);
    }
}

// ---------------------------------------------------------------------------
// ProcessSample
// ---------------------------------------------------------------------------

void ChorganCard::ProcessSample() {
    // 1. Read controls
    int32_t mainKnob = KnobVal(Knob::Main);
    int32_t knobX    = KnobVal(Knob::X);
    int32_t knobY    = KnobVal(Knob::Y);
    int32_t cv1      = CVIn1();
    int32_t cv2      = CVIn2();

    // 1b. Slew-rate limiter — clamps single-sample jumps caused by ADC/mux corruption
    // from fast CV/pulse inputs. Max 200 counts/sample = full sweep in ~20ms, faster
    // than any real knob movement. A corruption jump of 2000+ counts gets clamped.
    {
        int32_t dX = knobX    - knobXPrev;    if (dX >  kKnobMaxSlew) dX =  kKnobMaxSlew; if (dX < -kKnobMaxSlew) dX = -kKnobMaxSlew; knobX    = knobXPrev    + dX; knobXPrev    = knobX;
        int32_t dY = knobY    - knobYPrev;    if (dY >  kKnobMaxSlew) dY =  kKnobMaxSlew; if (dY < -kKnobMaxSlew) dY = -kKnobMaxSlew; knobY    = knobYPrev    + dY; knobYPrev    = knobY;
        int32_t dM = mainKnob - mainKnobPrev; if (dM >  kKnobMaxSlew) dM =  kKnobMaxSlew; if (dM < -kKnobMaxSlew) dM = -kKnobMaxSlew; mainKnob = mainKnobPrev + dM; mainKnobPrev = mainKnob;
    }

    // 2. Smooth controls
    mainKnobSmoothed += (mainKnob - mainKnobSmoothed) >> 5;
    knobXSmoothed    += (knobX    - knobXSmoothed)    >> 5;
    knobYSmoothed    += (knobY    - knobYSmoothed)    >> 5;
    cv1Smoothed      += (cv1      - cv1Smoothed)      >> 5;
    cv2Smoothed      += (cv2      - cv2Smoothed)      >> 5;

    // CV2 offsets timbre (main knob) bipolarly.
    int32_t timbre = mainKnobSmoothed + cv2Smoothed;
    if (timbre < 0)    timbre = 0;
    if (timbre > 4095) timbre = 4095;

    // 3. Tuning: CV1 + KnobX as Q10 semitone offset from MIDI 60.
    int32_t tuneQ10 = (cv1Smoothed * 12288) / kCountsPerOctave
                    + (knobXSmoothed - 2048) * 6;
    if (tuneQ10 < -60 * 1024) tuneQ10 = -60 * 1024;
    if (tuneQ10 >  60 * 1024) tuneQ10 =  60 * 1024;
    int32_t  tuneNote = 60 + (tuneQ10 >> 10);
    int32_t  tuneFrac = tuneQ10 & 0x3FF;
    if (tuneQ10 < 0) { tuneNote = 60 + (tuneQ10 >> 10) - 1; tuneFrac = (1024 + (tuneQ10 & 0x3FF)) & 0x3FF; }
    uint32_t rootInc  = phaseIncFrac(tuneNote, tuneNote + 1, tuneFrac);
    uint32_t newTuningRatio = (uint32_t)(((uint64_t)rootInc << 16) / MIDI_PHASE_INC[60]);

    // 4. Interval: Knob Y only, discrete semitone steps.
    int32_t intervalRaw = knobYSmoothed;
    if (intervalRaw < 0)    intervalRaw = 0;
    if (intervalRaw > 4095) intervalRaw = 4095;
    int32_t newIntervalSemi = (intervalRaw * 13) >> 12;
    if (newIntervalSemi > 12) newIntervalSemi = 12;

    // 5. Switch: short tap (< 1s) advances preset; hold (>= 1s) stores chord.
    Switch sw = SwitchVal();
    if (sw == Switch::Down) {
        holdTimer++;
        if (holdTimer == kHoldSamples && downArmed) {
            pendingStoreSlot = chordWriteIdx;
        }
    } else {
        if (holdTimer > 0 && downArmed) {
            if (pendingStoreSlot >= 0) {
                chordSeq[chordWriteIdx] = { newTuningRatio, newIntervalSemi, preset };
                chordWriteIdx = (chordWriteIdx + 1) % kMaxChords;
                if (chordCount < kMaxChords) chordCount++;
                pendingStoreSlot = -1;
            } else {
                // Short tap — advance preset; does NOT break chord override
                preset = (preset + 1) % kNumPresets;
                overridePreset = preset;  // sync so override block doesn't overwrite it
            }
            downArmed = false;
        }
        if (holdTimer == 0) downArmed = true;
        holdTimer = 0;
    }

    // 5b. PulseIn1: rising edge advances preset; does NOT break chord override.
    if (PulseIn1RisingEdge()) {
        preset = (preset + 1) % kNumPresets;
        overridePreset = preset;  // sync so override block doesn't overwrite it
    }

    // 5c. PulseIn2: arm once seen low, then on rising edge recall next chord.
    if (!PulseIn2()) pu2Armed = true;
    if (pu2Armed && PulseIn2RisingEdge() && chordCount > 0) {
        int32_t idx      = chordPlayIdx % chordCount;
        overrideTuning   = chordSeq[idx].tuningRatio;
        overrideInterval = chordSeq[idx].intervalSemi;
        overridePreset   = chordSeq[idx].preset;
        overrideSlot     = idx;
        // Latch the physical knob/CV positions NOW as the break baseline
        overrideBaseTuning   = newTuningRatio;
        overrideBaseInterval = newIntervalSemi;
        chordOverride        = true;
        chordPlayIdx         = (chordPlayIdx + 1) % chordCount;
    }

    // 5d. Override: substitute recalled chord. Break when physical controls move >1 semitone from baseline.
    if (chordOverride) {
        int32_t tuningDiff = (int32_t)newTuningRatio - (int32_t)overrideBaseTuning;
        if (tuningDiff < 0) tuningDiff = -tuningDiff;
        // 1 semitone in Q16.16 ratio ≈ 3898 units
        if (tuningDiff > 3898 || newIntervalSemi != overrideBaseInterval) {
            chordOverride = false;
        } else {
            newTuningRatio  = overrideTuning;
            newIntervalSemi = overrideInterval;
            if (preset != overridePreset) {
                preset = overridePreset;
            }
        }
    }

    // 6. Update targets if anything changed
    bool changed = false;
    if (newTuningRatio != prevTuningRatio) {
        tuningRatio     = newTuningRatio;
        prevTuningRatio = newTuningRatio;
        changed         = true;
    }
    if (newIntervalSemi != prevIntervalSemi) {
        intervalSemi     = newIntervalSemi;
        prevIntervalSemi = newIntervalSemi;
        changed          = true;
    }
    if (preset != prevPreset) {
        prevPreset = preset;
        changed    = true;
    }
    if (changed) updateTargets();

    // 7. Oscillator loop — 6 voices
    bool knobCW = (mainKnobSmoothed >= 2048);
    int32_t detuneAmt;
    if (sw == Switch::Up) {
        detuneAmt = knobCW ? 243 : 152;
    } else {
        detuneAmt = knobCW ? 76 : 0;
    }
    int32_t mix1 = 0, mix2 = 0;
    for (int i = 0; i < kNumVoices; i++) {
        int32_t detune = ((2 * i - 5) * detuneAmt) >> 1;
        uint32_t inc = (uint32_t)((int64_t)phaseIncBase[i] + ((int64_t)phaseIncBase[i] * detune >> 16));
        phase[i] += inc;
        mix1 += (blendWaveform(phase[i],                     timbre) * kAmpPerVoice) >> 8;
        mix2 += (blendWaveform(phase[i] + kStereoOffsets[i], timbre) * kAmpPerVoice) >> 8;
    }

    mix1 >>= 2;
    mix2 >>= 2;
    if (mix1 >  2047) mix1 =  2047;
    if (mix1 < -2048) mix1 = -2048;
    if (mix2 >  2047) mix2 =  2047;
    if (mix2 < -2048) mix2 = -2048;

    AudioOut1((int16_t)mix1);
    AudioOut2((int16_t)mix2);

    // 8. Pulse outputs
    PulseOut1(phase[0] & 0x40000000u);

    pwmPhase += (uint32_t)(detuneAmt * 900);
    uint32_t pwmTop  = pwmPhase >> 17;
    int32_t  pwmTri  = (pwmTop < 16384)
                     ? (int32_t)pwmTop
                     : (int32_t)(32767 - (pwmTop - 16384));
    uint32_t duty = 0x4CCCCCCC + (uint32_t)(pwmTri * 0x199A);
    PulseOut2((phase[0] >> 1) < duty);

    // 9. LEDs
    if (pendingStoreSlot >= 0) {
        // Storing: LEDs 1,3,5 full bright; LEDs 0,2,4 = slot index in binary
        LedBrightness(0, (pendingStoreSlot & 1) ? 4095 : 0);
        LedBrightness(1, 4095);
        LedBrightness(2, (pendingStoreSlot & 2) ? 4095 : 0);
        LedBrightness(3, 4095);
        LedBrightness(4, (pendingStoreSlot & 4) ? 4095 : 0);
        LedBrightness(5, 4095);
    } else if (chordOverride) {
        // Chord held: LEDs 0,2,4 solid; LEDs 1,3,5 = overrideSlot in binary
        LedBrightness(0, 4095);
        LedBrightness(1, (overrideSlot & 1) ? 4095 : 0);
        LedBrightness(2, 4095);
        LedBrightness(3, (overrideSlot & 2) ? 4095 : 0);
        LedBrightness(4, 4095);
        LedBrightness(5, (overrideSlot & 4) ? 4095 : 0);
    } else {
        // Normal: interval indicator + preset level
        int32_t displayInterval = chordOverride ? overrideInterval : intervalSemi;
        int32_t displayPreset   = chordOverride ? overridePreset   : preset;
        int32_t ledPos = (displayInterval * 4 + 6) / 12;
        if (ledPos > 4) ledPos = 4;
        for (int z = 0; z < 5; z++) LedOn(z, ledPos == z);
        LedBrightness(5, (displayPreset * 4095) / (kNumPresets - 1));
    }
}

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------

ChorganCard card;

int main() {
    set_sys_clock_khz(144000, true);
    card.Run();
}
