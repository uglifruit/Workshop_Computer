// chordseq — morphing 8-oscillator chord synthesizer for the Workshop Computer
//
//   CV In 1       : V/oct pitch (root)
//   Knob X        : Root pitch (coarse)
//   Knob Y        : First interval above root — 13 steps, unison to octave
//   Main Knob     : Oscillator shape — 0=saw, mid=triangle, max=sine-ish
//   Audio Out 1   : Left  (8 oscs, evenly phase-spread)
//   Audio Out 2   : Right (same 8 oscs, each shifted by a fixed extra offset)
//
// 8 oscillators: 4 on root, 4 on root + interval.
// All oscillators share the same shape; phase offsets create width.

#include "ComputerCard.h"

// V/oct lookup: 341 entries spanning one octave, right-shifted by octave index.
// Input 0..4095 → phase increment for 48kHz.  Source: Utility Pair by Chris Johnson.
static const int32_t voct_vals[341] = {
	314964268, 315605144, 316247323, 316890810, 317535606, 318181713, 318829136,
	319477876, 320127936, 320779318, 321432026, 322086062, 322741429, 323398129,
	324056166, 324715542, 325376259, 326038320, 326701729, 327366488, 328032599,
	328700066, 329368890, 330039076, 330710625, 331383541, 332057826, 332733483,
	333410515, 334088924, 334768714, 335449887, 336132446, 336816394, 337501733,
	338188467, 338876599, 339566130, 340257065, 340949405, 341643155, 342338315,
	343034891, 343732883, 344432296, 345133132, 345835394, 346539085, 347244208,
	347950766, 348658761, 349368197, 350079076, 350791402, 351505177, 352220405,
	352937088, 353655229, 354374831, 355095898, 355818432, 356542436, 357267913,
	357994867, 358723299, 359453214, 360184614, 360917502, 361651881, 362387755,
	363125126, 363863998, 364604372, 365346254, 366089644, 366834548, 367580967,
	368328905, 369078365, 369829350, 370581862, 371335907, 372091485, 372848601,
	373607257, 374367457, 375129204, 375892500, 376657350, 377423757, 378191722,
	378961250, 379732344, 380505008, 381279243, 382055053, 382832443, 383611414,
	384391970, 385174114, 385957850, 386743180, 387530108, 388318638, 389108772,
	389900514, 390693867, 391488834, 392285418, 393083624, 393883453, 394684911,
	395487998, 396292720, 397099080, 397907080, 398716724, 399528016, 400340958,
	401155555, 401971809, 402789724, 403609303, 404430550, 405253468, 406078060,
	406904330, 407732282, 408561918, 409393242, 410226258, 411060969, 411897378,
	412735489, 413575305, 414416830, 415260068, 416105021, 416951694, 417800089,
	418650211, 419502062, 420355647, 421210969, 422068031, 422926837, 423787390,
	424649694, 425513753, 426379570, 427247149, 428116493, 428987606, 429860492,
	430735153, 431611595, 432489820, 433369831, 434251634, 435135230, 436020625,
	436907821, 437796822, 438687632, 439580255, 440474694, 441370953, 442269035,
	443168945, 444070686, 444974262, 445879677, 446786934, 447696036, 448606989,
	449519795, 450434459, 451350984, 452269373, 453189631, 454111762, 455035769,
	455961656, 456889428, 457819087, 458750637, 459684083, 460619429, 461556677,
	462495833, 463436900, 464379881, 465324781, 466271604, 467220353, 468171033,
	469123648, 470078200, 471034695, 471993136, 472953528, 473915873, 474880177,
	475846443, 476814674, 477784876, 478757053, 479731207, 480707343, 481685466,
	482665579, 483647686, 484631791, 485617899, 486606014, 487596139, 488588278,
	489582437, 490578618, 491576826, 492577066, 493579340, 494583654, 495590012,
	496598417, 497608874, 498621387, 499635961, 500652599, 501671305, 502692084,
	503714940, 504739878, 505766901, 506796014, 507827220, 508860525, 509895933,
	510933447, 511973073, 513014813, 514058674, 515104658, 516152771, 517203017,
	518255399, 519309923, 520366592, 521425412, 522486386, 523549519, 524614815,
	525682278, 526751914, 527823726, 528897719, 529973898, 531052266, 532132828,
	533215589, 534300553, 535387725, 536477109, 537568709, 538662531, 539758579,
	540856856, 541957368, 543060120, 544165115, 545272359, 546381856, 547493610,
	548607627, 549723910, 550842464, 551963295, 553086406, 554211802, 555339489,
	556469470, 557601750, 558736334, 559873227, 561012433, 562153957, 563297803,
	564443977, 565592484, 566743327, 567896512, 569052043, 570209926, 571370165,
	572532764, 573697729, 574865065, 576034775, 577206866, 578381342, 579558207,
	580737467, 581919127, 583103191, 584289664, 585478552, 586669858, 587863589,
	589059748, 590258342, 591459374, 592662850, 593868775, 595077154, 596287991,
	597501292, 598717062, 599935306, 601156029, 602379235, 603604930, 604833120,
	606063808, 607297001, 608532703, 609770919, 611011654, 612254915, 613500705,
	614749029, 615999894, 617253304, 618509265, 619767781, 621028858, 622292501,
	623558715, 624827505, 626098877, 627372836, 628649388
};

// Returns 32-bit phase increment for a given 0..4095 V/oct input value.
static int32_t ExpVoct(int32_t in)
{
	if (in > 4091) in = 4091;
	int32_t oct = in / 341;
	int32_t sub = in % 341;
	return voct_vals[sub] >> (12 - oct);
}

// Semitone ratio table: 13 entries (0..12 semitones above root).
// Each entry is ExpVoct(341 * semitone / 12) — a fixed offset in V/oct units.
// We store the ratio as a multiplier Q16: ratio[n] = 2^(n/12) * 65536.
// Used to scale the root phase increment directly.
static const int32_t semitone_ratio_q16[13] = {
	// 2^(n/12) * 65536, n = 0..12
	65536,  // unison
	69432,  // min 2nd
	73561,  // maj 2nd
	77936,  // min 3rd
	82570,  // maj 3rd
	87480,  // perfect 4th
	92681,  // tritone
	98193,  // perfect 5th
	104032, // min 6th
	110218, // maj 6th
	116768, // min 7th
	123714, // maj 7th
	131072, // octave (= 2x)
};

static constexpr int kNOscs = 8;
// Phase offsets evenly distributed across 8 oscillators (0..2pi in uint32 space).
// Osc i gets offset i * (2^32 / 8).
static constexpr uint32_t kPhaseSpread = 0xFFFFFFFFu / kNOscs;
// Additional stereo offset for right channel: half a spread step (pi/4 in this case).
static constexpr uint32_t kStereoOffset = kPhaseSpread / 2;

// Startup holdoff and fade-in (see CLAUDE.md gotchas).
static constexpr int32_t kHoldoffSamples = 9600;
static constexpr int32_t kFadeInSamples  = 480;

class ChordSeq : public ComputerCard
{
public:
	uint32_t phase[kNOscs];

	ChordSeq()
	{
		for (int i = 0; i < kNOscs; i++)
			phase[i] = uint32_t(i) * kPhaseSpread;
	}

	// Compute one sample of a morphed waveform from a 32-bit phase accumulator.
	// shape 0..4095: 0=saw, 2048=triangle, 4095=sine-ish.
	// Returns value in range -2047..2047.
	int32_t __not_in_flash_func(ShapedOsc)(uint32_t ph, int32_t shape)
	{
		// Raw sawtooth: map uint32 phase to -2047..2047
		// phase 0 → -2047, phase 0x80000000 → 0, phase 0xFFFFFFFF → ~2047
		int32_t saw = int32_t(ph >> 20) - 2048; // -2048..2047

		// Triangle: fold saw so it goes up then down
		// |saw| gives 0..2047 for both halves; scale to -2047..2047
		int32_t tri;
		if (saw >= 0)
			tri = 2047 - (saw << 1);   //  2047 → -2047 as saw goes 0→2047
		else
			tri = 2047 + (saw << 1);   // -2047 → 2047 as saw goes -2048→0
		// tri is now: 2047 at saw=0, -2047 at saw=±2047 — a triangle

		// Sine-ish: apply a polynomial softening to triangle.
		// Use a cubic: out = tri - tri^3 / (3 * 2047^2)
		// This rounds the peaks without going float.
		// tri range is -2047..2047; tri*tri <= 2047^2 = 4190209 (fits int32)
		// tri^3 / (3 * 2047^2) = tri * (tri*tri) / 12570627
		// We approximate /12570627 as >>23 (8388608) — slightly different but close enough
		int32_t tri_norm = tri >> 3; // scale down to avoid overflow: -256..255
		int32_t sine_ish = tri - ((tri * (tri_norm * tri_norm)) >> 17);
		// sine_ish stays in ~-2047..2047

		// Morph: 0..2047 = saw→triangle, 2048..4095 = triangle→sine_ish
		int32_t out;
		if (shape < 2048)
		{
			// saw (shape=0) → triangle (shape=2047)
			out = (saw * (2047 - shape) + tri * shape) >> 11;
		}
		else
		{
			int32_t t = shape - 2048; // 0..2047
			out = (tri * (2047 - t) + sine_ish * t) >> 11;
		}
		return out;
	}

	void __not_in_flash_func(ProcessSample)() override
	{
		static int32_t sampleCount = 0;
		static int32_t fadeGain = 0; // Q12 gain, ramps 0→4095 over fade-in

		// --- Pitch ---
		// Knob X sweeps C3..C6 (3 octaves = 1023 V/oct steps).
		// C3 sits at V/oct index ~2472; CV In shifts up/down by up to ~1 octave.
		static constexpr int32_t kPitchBase  = 2472; // ~C3
		static constexpr int32_t kPitchRange = 1023; // 3 octaves
		int32_t k = (KnobVal(Knob::X) * kPitchRange) >> 12;
		int32_t cv = CVIn1();
		int32_t voct_in = kPitchBase + k + cv;
		if (voct_in < 0)    voct_in = 0;
		if (voct_in > 4095) voct_in = 4095;

		int32_t root_inc = ExpVoct(voct_in);

		// --- Interval (Y knob → 13 semitone steps) ---
		// KnobVal returns 0..4095; divide into 13 equal regions
		int32_t y = KnobVal(Knob::Y);
		int32_t semitone = (y * 13) >> 12; // 0..12
		if (semitone > 12) semitone = 12;
		// Scale root increment by semitone ratio
		int32_t interval_inc = int32_t((int64_t(root_inc) * semitone_ratio_q16[semitone]) >> 16);

		// --- Shape (Main knob) ---
		int32_t shape = KnobVal(Knob::Main); // 0..4095

		// --- Advance phases and sum ---
		// Oscs 0..3: root pitch. Oscs 4..7: interval pitch.
		int32_t sum_L = 0, sum_R = 0;
		for (int i = 0; i < kNOscs; i++)
		{
			int32_t inc = (i < 4) ? root_inc : interval_inc;
			phase[i] += uint32_t(inc);

			int32_t s_L = ShapedOsc(phase[i], shape);
			int32_t s_R = ShapedOsc(phase[i] + kStereoOffset, shape);
			sum_L += s_L;
			sum_R += s_R;
		}

		// Mix down 8 oscillators: divide by 8 (>> 3) to avoid clipping
		int32_t out_L = sum_L >> 3;
		int32_t out_R = sum_R >> 3;

		// --- Holdoff and fade-in ---
		if (sampleCount < kHoldoffSamples)
		{
			sampleCount++;
			AudioOut1(0);
			AudioOut2(0);
			return;
		}

		if (fadeGain < 4095)
		{
			fadeGain += 4095 / kFadeInSamples;
			if (fadeGain > 4095) fadeGain = 4095;
			out_L = (out_L * fadeGain) >> 12;
			out_R = (out_R * fadeGain) >> 12;
		}

		// Clamp
		if (out_L >  2047) out_L =  2047;
		if (out_L < -2047) out_L = -2047;
		if (out_R >  2047) out_R =  2047;
		if (out_R < -2047) out_R = -2047;

		AudioOut1(out_L);
		AudioOut2(out_R);
	}
};

ChordSeq chordseq;

int main()
{
	set_sys_clock_khz(144000, true);
	chordseq.Run();
}
