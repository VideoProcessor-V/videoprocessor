#pragma once

#include <libplacebo/renderer.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>

// texture_restore: off | statistical (experimental; owner 2026-10-17; docs/VP-detail-restore.md).
// Repair towards the original: per tile the luma detail is split into grain (unstructured,
// spatially uncorrelated) and scene texture (structured). Grain is scaled towards the
// best-coded recent frame's grain level (only the grain this frame already has: nothing is
// synthesised); scene texture is scaled towards the reference texture energy only where local
// focus evidence says the tile is in focus. References follow the vectors when valid and
// restart where the picture changed without them. Runs after detail_pulse_stabilise; never
// touches the source-resolution monitor's input; never starts the matcher.
//
// The algorithm (the shader in TextureRestore.cpp and the CPU model in
// src/VideoProcessor-Test/TextureRestoreModel.h share the constants below):
//  1. Three luma bands at the plane's resolution: D1 = Y - G(s1) Y (fine), D2 = G(s1) Y - L2
//     (mid), D3 = L2 - L3 (low-mid); L2 / L3 = G(s2) / G(s3) held at half resolution (2x2 box,
//     separable Gaussian, read back bilinearly at p / 2). s1, s2, s3 = 1, 2.5, 6 px x GrainScale.
//  2. Per tile (16 px x GrainScale, 4 x 4 sub-blocks): band energies E1..E3, the low band's mean,
//     and a robust grain level per band: the mean of the 4 quietest sub-blocks, the minimum over
//     the 5 x 5 tiles around (grain sits on the film everywhere; texture is local), calibrated so
//     that pure grain reads its own energy (kGrainCal*).
//  3. Grain / texture split: texture energy is the energy above the grain level
//     (T = E - kGrainExcess x N). Per pixel the fine band is split Wiener-like by its local
//     (3 x 3) energy against the grain level: f = clamp(1 - kWienerKappa x N1 / e1, 0, 1) is
//     texture (structure standing above the noise), 1 - f is grain (noise at the grain level).
//  4. Focus evidence of the current frame: the mid band's share F = T2 / (T2 + E3), an acutance
//     ratio: a step edge (natural, step-like content) of total blur sigma_t reads 0.28 at 1 px,
//     0.18 at 2 px, 0.12 at 3 px. Coding removes the fine band (D1) and keeps edges (D2 0.83-0.94
//     of the original on the real x265 clips, D3 0.95-0.99), so F barely moves with the bitrate,
//     while optical defocus (wide edges, no fine band) lowers it.
//  5. References per tile (grain level N1, texture energies T1 and T2, focus F): a peak-hold
//     with a half-life in seconds (the measured content rate), carried along the de-judder's
//     vectors where they are trusted; restarted where the low band (L2) changed since the
//     previous frame (per pixel, along the vector when there is one) by more than the
//     coding-noise allowance and the grain's share of it, on untrusted blocks, on a focus pull (F or the mid band's texture fell beyond what coding
//     explains), on cuts / fades / Forget / non-consecutive sequences.
//  6. Gains per tile, clamped to [1, cap], with dead zones for the estimators' noise:
//     grain gG = sqrt((N1ref + fg) / ((N1 + fg)(1 + kGrainMargin))) on the grain part of D1;
//     texture gTb = sqrt((Tb_ref + ft) / (Tb + kTextureMargin x Nb + ft)) on the texture parts of
//     D1 and D2, only where the focus gate passes (current F, reference F, no pull, the current
//     and the reference tile have mid-band texture well above their grain); else 1.
//  7. Y' = Y + (gG - 1)(1 - f) D1 + (gT1 - 1) f D1 + (gT2 - 1) f D2, the tile gains bilinear
//     between tile centres. Chroma untouched. Every frame keeps its own grain: nothing is copied.
namespace TextureRestore
{
	constexpr double kDefaultCap = 1.5;
	constexpr double kCapMin = 1.0, kCapMax = 2.0;

	// --- Shared constants (shader and CPU model) ---
	constexpr double kReferenceHz = 23.976;    // the content rate when it is unknown
	constexpr double kSigma1 = 1.0, kSigma2 = 2.5, kSigma3 = 6.0;   // band splits (px at <= 2160 lines)
	constexpr double kTilePx = 16.0;           // tile size at <= 2160 lines (x GrainScale)
	constexpr int kSubBlocks = 4;              // per tile axis (16 sub-blocks)
	constexpr int kQuietSubBlocks = 4;         // the quietest sub-blocks averaged for the grain level
	constexpr int kGrainNeighbourhood = 2;     // grain level: the minimum over (2 x 2 + 1)^2 tiles
	constexpr double kGrainCal1 = 2.2, kGrainCal2 = 3.8;   // pure grain reads about its own energy
	constexpr double kGrainExcess = 1.25;      // texture = the energy above this many grain levels
	constexpr double kWienerKappa = 2.0;       // pixel split: texture where the local energy exceeds kappa x grain
	constexpr double kGrainMargin = 0.35;      // grain gain dead zone (relative: the peak-hold of a noisy estimate)
	constexpr double kTextureMargin = 0.5;     // texture gain dead zone (grain levels of the band)
	constexpr double kFocusLo = 0.15, kFocusHi = 0.21;    // F: none (step blur 2.5 px) .. full (1.7 px)
	constexpr double kPullLo = 0.6, kPullHi = 0.8;        // F / F_ref: focus pulled .. kept (restart below Lo)
	constexpr double kMidLossLo = 0.5, kMidLossHi = 0.7;  // T2 / T2_ref: beyond coding .. coding (restart below Lo)
	constexpr double kTextureSigLo = 2.0, kTextureSigHi = 5.0;   // T2 (current and reference) over the mid band grain level + coding noise energy
	constexpr double kHoldHalfLifeSeconds = 2.0;          // reference energies halve in this time
	constexpr double kGrainFloorCodes8 = 0.25, kTextureFloorCodes8 = 0.25;   // RMS floors, 8-bit codes
	constexpr double kMeanAllowance = 1.0;     // low-band change allowance (RMS), x CodingNoiseCodes8
	constexpr double kLowChangeNoise = 4.0;    // ... plus this many mid-band grain levels (the low band's grain, noisy)
	constexpr double kCutShare = 0.5;          // share of tiles changed: a cut (all references restart)
	constexpr double kFadeStepShare = 0.5;     // frame mean ramp per frame (x the allowance) over two frames: a fade
	constexpr double kEnergyEps = 1e-6;        // codes^2

	inline double Clamp(double v, double lo, double hi) { return v < lo ? lo : (v > hi ? hi : v); }
	inline double Smoothstep(double e0, double e1, double x)
	{
		const double t = Clamp((x - e0) / (e1 - e0), 0.0, 1.0);
		return t * t * (3.0 - 2.0 * t);
	}
	// Grain-sized distances grow with the plane's height above 2160 lines (GrainPreserve::GrainScale).
	inline double Scale(double planeLines) { return planeLines > 2160.0 ? planeLines / 2160.0 : 1.0; }
	// The tile in plane px: a multiple of kSubBlocks.
	inline int TilePx(double scale)
	{
		const int sub = (std::max)(1, static_cast<int>(std::lround(kTilePx * scale / kSubBlocks)));
		return sub * kSubBlocks;
	}
	inline int Radius1(double scale) { return (std::max)(1, static_cast<int>(std::ceil(3.0 * kSigma1 * scale - 1e-9))); }
	inline double Weight1(int k, double scale)
	{
		const double s = kSigma1 * scale;
		const int r = Radius1(scale);
		double sum = 0.0;
		for (int i = -r; i <= r; ++i) sum += std::exp(-0.5 * i * i / (s * s));
		return std::exp(-0.5 * k * k / (s * s)) / sum;
	}
	// The half-resolution sigma of a full-resolution split (the 2x2 box adds 0.25 px^2 per axis).
	inline double HalfSigma(double sigma) { return std::sqrt(sigma * sigma - 0.25) / 2.0; }
	inline int HalfRadius(double sigma) { return (std::max)(1, static_cast<int>(std::ceil(3.5 * HalfSigma(sigma) - 1e-9))); }
	inline double HalfWeight(int k, double sigma)
	{
		const double s = HalfSigma(sigma);
		const int r = HalfRadius(sigma);
		double sum = 0.0;
		for (int i = -r; i <= r; ++i) sum += std::exp(-0.5 * i * i / (s * s));
		return std::exp(-0.5 * k * k / (s * s)) / sum;
	}
	// One 8-bit code in the signal's codes.
	inline double EightBitStep(int colorDepth) { return std::pow(2.0, (std::max)(0, colorDepth - 8)); }
	// The expected coding-noise RMS in 8-bit codes (as GrainPreserve::CodingNoiseCodes).
	inline double CodingNoiseCodes8(double bitrateQuality)
	{
		const double q = Clamp(bitrateQuality, 0.0, 1.0);
		return 0.5 + 2.5 * (1.0 - q);
	}
	// The low-band change allowance (RMS of the change; also the fade step's unit), in the signal's codes.
	inline double MeanAllowanceCodes(double bitrateQuality, int colorDepth)
	{
		return kMeanAllowance * CodingNoiseCodes8(bitrateQuality) * EightBitStep(colorDepth);
	}
	// The per-frame decay of the reference energies.
	inline double Decay(double sourceHz)
	{
		const double hz = sourceHz > 0.0 ? sourceHz : kReferenceHz;
		return std::pow(2.0, -1.0 / (hz * kHoldHalfLifeSeconds));
	}
	inline double EffectiveCap(double cap) { return Clamp(cap, kCapMin, kCapMax); }

	struct Params
	{
		double cap = kDefaultCap;      // texture_restore_cap, clamped to [kCapMin, kCapMax]
		int colorDepth = 10;
		double planeLines = 0.0;       // the picture's lines in this plane; 0 = the luma plane's height
		double sourceHz = 0.0;         // measured content rate; 0 = unknown (reference 23.976)
		double bitrateQuality = 1.0;
		pl_tex field = nullptr;        // as DetailPulse::Params::field (valid vectors only)
		double fieldScale = 1.0;
		double fieldScaleX = 0.0;
		uint64_t sequence = 0;
	};

	class Module
	{
	public:
		Module() = default;
		~Module();
		Module(const Module&) = delete;
		Module& operator=(const Module&) = delete;

		// `source` with its luma plane restored into this module's texture (other planes pass
		// through); false (and `out` untouched) on any failure: the caller renders the frame as
		// it was. cap 1.0 returns `source` itself. A repeated sequence reuses its frame's gains
		// and never advances the references; a non-consecutive one (or 0) restarts them.
		bool Apply(pl_gpu gpu, pl_dispatch dispatch, const pl_frame& source, const Params& params, pl_frame& out);
		// A discontinuity (MotionValidity invalidation, re-base change): references restart.
		void Forget();
		void Release(pl_gpu gpu);
		const std::string& LastError() const { return error; }
		unsigned long long Failures() const { return failures; }
		unsigned long long Frames() const { return frames; }
		// The source frame the references last advanced on (0 = none or a restart pending): a
		// caller may apply this frame's decision to another picture of it (a de-judder extra)
		// as a repeat, without advancing.
		uint64_t HistorySequence() const;

	private:
		struct Impl;
		Impl* impl = nullptr;
		std::string error;
		unsigned long long failures = 0, frames = 0;
	};
}
