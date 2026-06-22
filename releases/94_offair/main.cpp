// offair — AM/shortwave radio simulator for the Workshop Computer
//
// Two audio inputs become radio stations at fixed positions on a virtual dial.
// The Main knob tunes across them through noise, static, and interference clips.
// Real AM PWM transmission via DEBUG_1 — a nearby AM radio will receive the audio.
//
// NORMAL BOOT (switch not held down):
//   Audio In 1/2    : Station 1 / Station 2 (live audio)
//
// ALTBOOT (hold Switch Down at power-on until all LEDs flash):
//   Baked broadcast recordings replace the live audio inputs.
//
//   Main Knob       : Tuning position
//   Knob X          : Drift depth (0 = locked, max = heavy station wander)
//   Knob Y          : Noise / static level
//   CV In 1         : Fine tune offset (bipolar, ±512 counts)
//   CV In 2         : Interference clip probability (0V = off, +5V = always)
//   Pulse In 1      : Rising edge = reset station drift to zero
//   Pulse In 2      : While HIGH = freeze tuning position
//   Audio Out 1     : Full mix — stations + interference + noise
//   Audio Out 2     : Noise/interference only
//   CV Out 1        : Signal strength 0→+5V
//   CV Out 2        : Station balance (−5V = St1, 0 = mid, +5V = St2)
//   Pulse Out 1     : HIGH when cleanly receiving a station
//   Pulse Out 2     : HIGH when in noise zone (no station)
//   Switch Down tap : Cycle band AM → SW → LW → AM
//   LED 0           : Station 1 signal strength
//   LED 1           : Station 2 signal strength
//   LED 2           : SW band active
//   LED 3           : AM band active
//   LED 4           : LW band active
//   LED 5           : Tuning position (sweeps with main knob)
//
// Integer math only — no floats in the hot path.

#include "ComputerCard.h"
#include "clips.h"
#include "hardware/pwm.h"
#include "hardware/irq.h"
#include "pico/multicore.h"

// ---------------------------------------------------------------------------
// PWM globals — written by ProcessSample(), read by ISR
// ---------------------------------------------------------------------------

volatile int32_t amValue = 0, fqValue = 1024000;

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

// Two live-audio stations at 1/3 and 2/3 of dial (0..4095)
static constexpr int32_t kStation1Pos = 1365;
static constexpr int32_t kStation2Pos = 2730;

// Interference clip dial positions and clip order per band
// kIntfPos[band][slot] — 6 slots, 2 below St1, 2 between, 2 above St2
static constexpr int32_t kIntfPos[3][6] = {
    { 300,  800, 1600, 2200, 3000, 3600 },  // AM
    { 500,  700, 1500, 2400, 3200, 3750 },  // SW
    { 200,  600, 1750, 2050, 3100, 3800 },  // LW
};
// kClipOrder[band][slot] — index into kAllClips (0=POL,1=UM10,2=F03,3=XT2,4=POLY,5=MXL)
static constexpr uint8_t kClipOrder[3][6] = {
    { 0, 3, 1, 4, 2, 5 },   // AM
    { 2, 5, 0, 3, 1, 4 },   // SW
    { 4, 1, 3, 0, 5, 2 },   // LW
};

// Interference clip reception window
static constexpr int32_t kIntfBW   = 200;
static constexpr int32_t kIntfHalf = 100;

// Maximum drift at full Knob X, and probability mask (1-in-128 chance per sample)
static constexpr int32_t  kMaxDriftFull  = 96;
static constexpr uint32_t kDriftProbMask = 0xFE000000u;

// Per-band LPF alpha (Q12, applied before AM modulation)
// alpha = round(4096 * (1 - exp(-2π * fc / 48000)))
static constexpr int32_t kLpfAlpha[3] = { 374, 564, 207 };  // AM 3.5k / SW 5.5k / LW 1.8k

// Per-band demod LPF alpha (three-pole envelope detector) — kept for reference
// static constexpr int32_t kDemodAlpha[3] = { 99, 145, 62 };

// Clip sample rate ratios (fractional accumulator: add Num per call, advance when >= Den)
static constexpr int32_t kClipNum  = 8000;   // interference at 8kHz
static constexpr int32_t kClipDen  = 48000;
static constexpr int32_t kBcastNum = 13340;  // broadcast: 11025Hz source * 1.21 to correct tempo
static constexpr int32_t kBcastDen = 48000;

// ---------------------------------------------------------------------------
// Sine table — 256-entry full cycle, int8 -127..127
// ---------------------------------------------------------------------------

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

static inline int32_t __not_in_flash_func(isin)(uint32_t phase)
{
    return kSinTable[phase >> 24];
}

// Carrier frequency: dial 0..4095 → 300..3000 Hz → phase increment for 48kHz
// phase_inc = freq * 2^32 / 48000 = freq * 89478
// = (300 + dialPos * 2700/4096) * 89478 ≈ 26843400 + dialPos * 58988
static inline uint32_t __not_in_flash_func(carrierInc)(int32_t dialPos)
{
    return (uint32_t)(26843400u + (uint32_t)dialPos * 58988u);
}

static inline int32_t __not_in_flash_func(softClip)(int32_t s, int32_t threshold)
{
    if (s >  threshold) return  threshold + ((s - threshold) >> 2);
    if (s < -threshold) return -threshold + ((s + threshold) >> 2);
    return s;
}

// Flat-top piecewise-linear bell, returns 0..4095
static inline int32_t __not_in_flash_func(stationEnv)(
    int32_t tunePos, int32_t stEff, int32_t bandwidth, int32_t halfBand)
{
    int32_t dist = tunePos - stEff;
    if (dist < 0) dist = -dist;
    if (dist >= bandwidth) return 0;
    if (dist < halfBand)   return 4095;
    int32_t rem = bandwidth - dist;
    return rem * 21 + (rem >> 3);  // ≈ rem * 21.125; avoids division
}

// ---------------------------------------------------------------------------
// OffAir
// ---------------------------------------------------------------------------

class OffAir : public ComputerCard
{
    // --- Startup ---
    bool    mode_locked = false;
    int32_t mode_settle = 4800;   // samples until ADC smoother settles
    bool    altbootMode = false;
    int32_t fadeGain    = 0;      // 0..4095 linear ramp after lock

    // --- Band: 0=AM, 1=SW, 2=LW ---
    int32_t band = 0;

    // --- Switch tap ---
    int32_t switchTimer = 0;
    bool    downArmed   = true;

    // --- Knob smoothers (tau = 64 samples) ---
    int32_t smMain = 2048;
    int32_t smX    = 2048;
    int32_t smY    = 0;

    // --- Tuning freeze + carrier smoother ---
    int32_t tuneLocked     = 2048;
    int32_t tuneSmooth     = 2048;  // smoothed tunePos for carrier freq — kills zipper noise

    // --- Per-station DC block + band LPF ---
    int32_t hpState1 = 0, hpState2 = 0;
    int32_t lpfState1 = 0, lpfState2 = 0;

    // --- AM carrier phases ---
    uint32_t carrierPhase1 = 0, carrierPhase2 = 0;
    uint32_t tunerPhase    = 0;

    // --- Station drift ---
    int32_t drift1 = 0, drift2 = 0;
    int32_t driftTarget1 = 0, driftTarget2 = 0;
    bool    pu1Prev = false;

    // --- Altboot LED flash timer ---
    int32_t altbootFlash = 24000;  // samples (~0.5s at 48kHz)

    // --- Bandpass noise ---
    int32_t bpY1 = 0;

    // --- Interference clip playback ---
    uint32_t intfPos[6]   = {};
    int32_t  intfFrac[6]  = {};
    int32_t  intfSamp[6]  = {};

    // --- Broadcast clip playback ---
    uint32_t bcastPos[2]    = {};
    int32_t  bcastFrac[2]   = {};
    int32_t  bcastSample[2] = {};

    // --- RNG ---
    uint32_t rng = 1;

    // -----------------------------------------------------------------------

    inline uint32_t __not_in_flash_func(rng_next)()
    {
        rng = 1664525u * rng + 1013904223u;
        return rng;
    }

    int32_t __not_in_flash_func(stepBcast)(int ci)
    {
        bcastFrac[ci] += kBcastNum;
        if (bcastFrac[ci] >= kBcastDen) {
            bcastFrac[ci] -= kBcastDen;
            if (++bcastPos[ci] >= kBcastClips[ci].len) bcastPos[ci] = 0;
            // Unpack 12-bit signed from 3-byte pairs (2 samples per 3 bytes)
            // byte layout: [A11..A4][A3..A0 B11..B8][B7..B0]
            uint32_t p = bcastPos[ci];
            const uint8_t* d = kBcastClips[ci].data;
            int32_t s;
            if ((p & 1) == 0) {
                // even sample: upper nibble of byte1
                uint32_t b = (p >> 1) * 3;
                s = ((int32_t)d[b] << 4) | (d[b+1] >> 4);
            } else {
                // odd sample: lower nibble of byte1 + byte2
                uint32_t b = (p >> 1) * 3;
                s = (((int32_t)d[b+1] & 0xF) << 8) | d[b+2];
            }
            if (s >= 2048) s -= 4096;  // sign-extend 12-bit
            bcastSample[ci] = s;
        }
        return bcastSample[ci];
    }

    int32_t __not_in_flash_func(stepIntf)(int ci)
    {
        const ClipDesc& c = kAllClips[kClipOrder[band][ci]];
        intfFrac[ci] += kClipNum;
        if (intfFrac[ci] >= kClipDen) {
            intfFrac[ci] -= kClipDen;
            if (++intfPos[ci] >= c.len) intfPos[ci] = 0;
            intfSamp[ci] = ((int32_t)c.data[intfPos[ci]] - 128) << 3;
        }
        return intfSamp[ci];
    }

public:
    OffAir() {}

    void Init()
    {
        gpio_init(DEBUG_2);
        gpio_set_dir(DEBUG_2, GPIO_OUT);
        gpio_put(DEBUG_2, false);
    }

    void StageLed(int n) { LedBrightness(n, 4095); }

    void __not_in_flash_func(ProcessSample)() override
    {
        // Smoothers run from sample 0 so they converge during the settle period
        smMain += (KnobVal(Knob::Main) - smMain) >> 6;
        smX    += (KnobVal(Knob::X)    - smX)    >> 6;
        smY    += (KnobVal(Knob::Y)    - smY)    >> 6;

        // -------------------------------------------------------------------
        // Startup: wait for ADC smoother to settle before reading switch
        // -------------------------------------------------------------------
        if (!mode_locked) {
            if (mode_settle > 0) {
                mode_settle--;
                AudioOut1(0); AudioOut2(0);
                // LED 5 on = ProcessSample is running; LEDs 0-4 show main() stages
                LedBrightness(5, 4095);
                return;
            }
            altbootMode = (SwitchVal() == Switch::Down);
            mode_locked = true;
            downArmed   = false;  // disarm: boot press must not register as a band tap
            bcastPos[0] = 0;
            bcastPos[1] = kBcastClips[1].len / 2;
            if (altbootMode) {
                for (int i = 0; i < 6; i++) LedBrightness(i, 4095);
            }
        }

        // -------------------------------------------------------------------
        // Fade-in: 480-sample linear ramp to eliminate startup click
        // -------------------------------------------------------------------
        if (fadeGain < 4095) {
            fadeGain += 4095 / 480;
            if (fadeGain > 4095) fadeGain = 4095;
        }

        // -------------------------------------------------------------------
        // Switch tap: cycle band AM → SW → LW → AM (short press only)
        // -------------------------------------------------------------------
        Switch sw = SwitchVal();
        if (sw == Switch::Down) {
            switchTimer++;
        } else {
            if (downArmed && switchTimer > 0 && switchTimer < 48000)
                band = (band + 1) % 3;
            downArmed   = (switchTimer == 0);
            switchTimer = 0;
        }

        // -------------------------------------------------------------------
        // Tuning position (PU2 high = freeze)
        // -------------------------------------------------------------------
        int32_t cv1 = CVIn1();
        int32_t fineOffset = (cv1 * 512) / 2048;
        int32_t tunePos;
        if (PulseIn2()) {
            tunePos = tuneLocked;
        } else {
            tunePos = smMain + fineOffset;
            if (tunePos < 0)    tunePos = 0;
            if (tunePos > 4095) tunePos = 4095;
            tuneLocked = tunePos;
        }

        // -------------------------------------------------------------------
        // Drift (Knob X gates depth; PU1 rising edge resets)
        // -------------------------------------------------------------------
        bool pu1Now = PulseIn1();
        if (pu1Now && !pu1Prev)
            drift1 = drift2 = driftTarget1 = driftTarget2 = 0;
        pu1Prev = pu1Now;

        int32_t maxDrift = (smX * kMaxDriftFull) / 4095;

        uint32_t rv = rng_next();
        if ((rv & kDriftProbMask) == 0) {
            driftTarget1 += (rv & 1) ? 1 : -1;
            if (driftTarget1 >  maxDrift) driftTarget1 =  maxDrift;
            if (driftTarget1 < -maxDrift) driftTarget1 = -maxDrift;
        }
        rv = rng_next();
        if ((rv & kDriftProbMask) == 0) {
            driftTarget2 += (rv & 1) ? 1 : -1;
            if (driftTarget2 >  maxDrift) driftTarget2 =  maxDrift;
            if (driftTarget2 < -maxDrift) driftTarget2 = -maxDrift;
        }
        drift1 += (driftTarget1 - drift1) >> 7;
        drift2 += (driftTarget2 - drift2) >> 7;

        int32_t st1Eff = kStation1Pos + drift1;
        int32_t st2Eff = kStation2Pos + drift2;

        // -------------------------------------------------------------------
        // Station envelopes: flat-top bell, bw=384, half=192
        // -------------------------------------------------------------------
        int32_t env1 = stationEnv(tunePos, st1Eff, 384, 192);
        int32_t env2 = stationEnv(tunePos, st2Eff, 384, 192);

        // -------------------------------------------------------------------
        // Audio source: live inputs or broadcast clips
        // -------------------------------------------------------------------
        int32_t raw1, raw2;
        if (altbootMode) {
            raw1 = stepBcast(0);
            raw2 = stepBcast(1);
        } else {
            raw1 = (int32_t)AudioIn1();
            raw2 = (int32_t)AudioIn2();
        }

        // -------------------------------------------------------------------
        // DC block + band LPF per station
        // -------------------------------------------------------------------
        hpState1 += (raw1 - hpState1) >> 11;
        hpState2 += (raw2 - hpState2) >> 11;
        int32_t hp1 = raw1 - hpState1;
        int32_t hp2 = raw2 - hpState2;

        int32_t lpfA = kLpfAlpha[band];
        lpfState1 += ((hp1 - lpfState1) * lpfA) >> 12;
        lpfState2 += ((hp2 - lpfState2) * lpfA) >> 12;

        int32_t audio1 = lpfState1 >> 4;  // ±127
        int32_t audio2 = lpfState2 >> 4;

        // Smooth tuner frequency separately — eliminates zipper clicks when turning the knob.
        // Station carriers are fixed (drift is already slow); only the tuner needs this.
        tuneSmooth += (tunePos - tuneSmooth) >> 4;  // tau ~16 samples, ~0.3ms lag

        carrierPhase1 += carrierInc(st1Eff);
        carrierPhase2 += carrierInc(st2Eff);
        tunerPhase    += carrierInc(tuneSmooth);

        // -------------------------------------------------------------------
        // AM encode: full-carrier DSB for PWM transmitter only
        // tx signals are used for the PWM output, not for audio demod
        // -------------------------------------------------------------------
        int32_t tx1 = ((140 + audio1) * isin(carrierPhase1)) >> 7;
        int32_t tx2 = ((140 + audio2) * isin(carrierPhase2)) >> 7;

        // -------------------------------------------------------------------
        // Direct crossfade for audio output — envelope gates each station.
        // lpfState is ±2048 range; env is 0..4095; result must stay ±2047.
        // (lpfState * env) >> 12 gives correct 0..1 scaling.
        // -------------------------------------------------------------------
        int32_t stAudio1 = (lpfState1 * env1) >> 12;
        int32_t stAudio2 = (lpfState2 * env2) >> 12;
        int32_t demodOut = stAudio1 + stAudio2;

        // -------------------------------------------------------------------
        // Interference clips — always play at their dial positions.
        // CV In 2 scales their probability (0V = always, +5V = ~50% dropout).
        // Knob Y scales their level alongside the noise floor.
        // -------------------------------------------------------------------
        int32_t intfMix = 0, maxIntfEnv = 0;
        int32_t cv2 = CVIn2();
        int32_t dropThresh = cv2 > 0 ? (cv2 >> 4) : 0;  // 0..127

        for (int i = 0; i < 6; i++) {
            int32_t isamp = stepIntf(i);
            int32_t ienv  = stationEnv(tunePos, kIntfPos[band][i], kIntfBW, kIntfHalf);
            if (ienv > 0) {
                uint32_t gate = rng_next() & 0xFF;
                if ((int32_t)gate > dropThresh) {
                    intfMix += (isamp * (ienv >> 5)) >> 8;  // quieter: >>8 not >>6
                    if (ienv > maxIntfEnv) maxIntfEnv = ienv;
                }
            }
        }
        // Scale intf by Knob Y so both noise and intf ride the same fader
        intfMix = (intfMix * smY) >> 12;

        // -------------------------------------------------------------------
        // Bandpass noise — character varies by band, level follows Knob Y.
        // Between bands the noise is prominent; stations suppress it.
        //
        // Per-band LPF alpha (Q12) for the noise shaping:
        //   AM  ~1.5kHz : alpha = 200   (mellow medium-wave hiss)
        //   SW  ~5kHz   : alpha = 564   (bright shortwave crackle)
        //   LW  ~400Hz  : alpha =  55   (deep low-frequency rumble)
        // -------------------------------------------------------------------
        static constexpr int32_t kNoiseLpf[3] = { 200, 564, 55 };

        int32_t white = (int32_t)(rng_next() >> 17) - 1024;  // ±1024
        bpY1 += ((white - bpY1) * kNoiseLpf[band]) >> 12;
        if (bpY1 >  2000) bpY1 =  2000;
        if (bpY1 < -2000) bpY1 = -2000;

        // Noise ducks when a station is present
        int32_t maxEnv    = env1 > env2 ? env1 : env2;
        int32_t noiseGate = 4095 - maxEnv;
        int32_t noiseOut  = (bpY1 * ((smY * noiseGate) >> 12)) >> 11;

        // -------------------------------------------------------------------
        // Mix: station audio + interference + noise.
        // No synthetic beat whistle — tuning feel comes from envelope shape.
        // -------------------------------------------------------------------
        int32_t background = intfMix + noiseOut;
        int32_t out1 = demodOut + background;

        out1 = softClip(out1, 1800);
        if (out1 >  2047) out1 =  2047;
        if (out1 < -2048) out1 = -2048;
        if (fadeGain < 4095) out1 = (out1 * fadeGain) >> 12;
        AudioOut1((int16_t)out1);

        background = softClip(background, 1800);
        if (background >  2047) background =  2047;
        if (background < -2048) background = -2048;
        if (fadeGain < 4095) background = (background * fadeGain) >> 12;
        AudioOut2((int16_t)background);

        // -------------------------------------------------------------------
        // CV outputs: signal strength, station balance
        // -------------------------------------------------------------------
        int32_t sigStr = env1 + env2;
        if (sigStr > 4095) sigStr = 4095;
        CVOut1((int16_t)(sigStr - 2048));

        int32_t bal = (env2 - env1) >> 1;  // -2047..+2047
        CVOut2((int16_t)bal);

        // -------------------------------------------------------------------
        // Pulse outputs
        // -------------------------------------------------------------------
        PulseOut1(env1 > 3000 || env2 > 3000);
        PulseOut2(env1 < 200 && env2 < 200 && maxIntfEnv < 200);

        // -------------------------------------------------------------------
        // LEDs
        // -------------------------------------------------------------------
        if (altbootMode && altbootFlash > 0) {
            altbootFlash--;
            for (int i = 0; i < 6; i++) LedBrightness(i, 4095);
        } else {
            LedBrightness(0, (uint16_t)env1);
            LedBrightness(1, (uint16_t)env2);
            LedBrightness(2, band == 1 ? 4095 : 0);  // SW
            LedBrightness(3, band == 0 ? 4095 : 0);  // AM
            LedBrightness(4, band == 2 ? 4095 : 0);  // LW
            LedBrightness(5, (uint16_t)tunePos);
        }

        // -------------------------------------------------------------------
        // PWM transmitter: drives audio out over AM carrier on DEBUG_1 pin
        // fqValue → carrier frequency; amValue → amplitude-modulated duty cycle
        // -------------------------------------------------------------------
        // Transmit the weighted mix of both stations over AM carrier
        int32_t txMix = tx1 + tx2;
        if (txMix >  2047) txMix =  2047;
        if (txMix < -2047) txMix = -2047;
        fqValue = 563200 + (4095 - tunePos) * 280;
        amValue = (txMix + 2048) * (fqValue >> 13);
    }
};

// ---------------------------------------------------------------------------
// RF PWM ISR and setup — run on core 0, separate from ComputerCard's core 1
// Core separation avoids IRQ handler conflict: ComputerCard registers its own
// PWM_IRQ_WRAP handler for CV output; we register ours on core 0 independently.
// ---------------------------------------------------------------------------

static void __not_in_flash_func(OnRFPWMWrap)()
{
    static int32_t amError = 0, fqError = 0;
    int32_t amv = amValue, fqv = fqValue;

    uint32_t amTruncated = (uint32_t)(amv - amError) & 0xFFFFF000u;
    uint32_t fqTruncated = (uint32_t)(fqv - fqError) & 0xFFFFF000u;

    amError += (int32_t)amTruncated - amv;
    fqError += (int32_t)fqTruncated - fqv;

    pwm_hw->slice[0].cc  = amTruncated >> 12;
    pwm_hw->slice[0].top = fqTruncated >> 12;

    pwm_hw->intr = 1;  // clear IRQ flag for slice 0
}

static void SetupRFPWM()
{
    gpio_set_function(DEBUG_1, GPIO_FUNC_PWM);
    pwm_config config = pwm_get_default_config();
    pwm_config_set_wrap(&config, 220);
    pwm_init(pwm_gpio_to_slice_num(DEBUG_1), &config, true);
    pwm_set_gpio_level(DEBUG_1, 0);

    uint slice_num = pwm_gpio_to_slice_num(DEBUG_1);
    pwm_clear_irq(slice_num);
    pwm_set_irq_enabled(slice_num, true);
    irq_set_exclusive_handler(PWM_IRQ_WRAP, OnRFPWMWrap);
    irq_set_priority(PWM_IRQ_WRAP, PICO_HIGHEST_IRQ_PRIORITY);
    irq_set_enabled(PWM_IRQ_WRAP, true);
}

static OffAir card;

static void core1_entry() { card.Run(); }

int main()
{
    // Stage 0: power on, before anything
    card.StageLed(0);

    set_sys_clock_khz(220000, true);

    // Stage 1: clock set
    card.StageLed(1);

    card.Init();

    // Stage 2: Init() done
    card.StageLed(2);

    // Launch core 1 first — its ~1MHz RF PWM ISR must not starve
    // the inter-core FIFO handshake that multicore_launch_core1 needs
    multicore_launch_core1(core1_entry);

    // Stage 3: core 1 launched
    card.StageLed(3);

    // Let core 1 complete AudioWorker setup before RF PWM ISR starts firing
    sleep_ms(500);

    SetupRFPWM();

    // Stage 4: RF PWM running (ProcessSample will light LED 5 on first call)
    card.StageLed(4);

    while (1) __wfi();
}
