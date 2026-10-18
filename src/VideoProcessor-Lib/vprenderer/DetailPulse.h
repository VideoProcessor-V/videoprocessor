#pragma once

#include <libplacebo/renderer.h>

#include <vprenderer/GrainPreserve.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>

// detail_pulse_stabilise (owner 2026-10-17; docs/VP-detail-restore.md, research option e in
// docs/audit/detail-retrieval-research-2026-10-17.md): a starved encode codes I-frames finely
// and B-frames coarsely, so fine detail and grain pulse with the GOP. Each frame's OWN luma
// detail (two bands) is scaled up towards its best-coded recent neighbour's level; nothing is
// copied between frames, so grain stays per-frame and is never averaged. Runs after the VP
// debander on the plane libplacebo scales from; never touches the source-resolution monitor's
// input; never starts the matcher.
//
// The maths (shared by the shader and the CPU model, src/VideoProcessor-Test/DetailPulseModel.h):
//  - Bands, in codes of the signal: D1 = Y - G(s1) Y, D2 = G(s1) Y - L, where G(s1) is a
//    separable Gaussian of kSigmaFine px and L the wide low band (kSigmaWide px) held at half
//    resolution as GrainPreserve's (2x2 box, separable Gaussian, bilinear back at p / 2).
//    Both sigmas grow with the plane's height above 2160 lines (Scale()).
//  - Per tile (kTilePx x kTilePx plane px, the passes' work block): the mean of D1^2 and D2^2,
//    and the mean and spread (standard deviation) of L over its half-resolution texels.
//  - Frame energy per band: the mean over the measured tiles - D1^2 + D2^2 reaching the floor
//    (FloorEnergy: not flat, not black bars), veto weight w >= 0.5 and band energy within a
//    factor kTileEnergyJump of the tile's previous one (a changed tile's energy is new content,
//    not coding) - winsorised at kWinsorise x that mean.
//  - History: a GPU ring of (E1, E2, mean luma, flag) per source frame. Reference per band
//    R = max(E, max over the run of E_j x DecayPerFrame^j) over the last WindowFrames() frames
//    (seconds converted with the content rate), stopping at the frame that started the run
//    (a cut, a fade, a restart). Optional look-ahead: the next source frame's energies when
//    the caller measured them (MeasureAhead) and they are comparable (same mean luma within
//    the cut level, energy ratio within kAheadMaxRatio).
//  - Gain per band: Gain(R, E, cap) = 1 inside the dead zone (R / E <= 1 + kDeadZone),
//    else sqrt(R / E / (1 + kDeadZone)), never above the cap. Never below 1.
//  - Per tile veto weight w: with the de-judder's field, 1 - smoothstep(0, kVetoUntrustedShare,
//    untrusted share of the tile's pixels); without it, 1 - smoothstep(v, 2v, change) where
//    change = max(|d mean L|, |d spread L|) since the previous frame and v the coding-noise
//    allowance (VetoCodes). The tile statistics are motion-tolerant: a small displacement
//    hardly changes a tile's mean or spread, new content or a flash does. w is bilinear
//    between tile centres.
//  - Cut: more than kCutShare of the tiles changed beyond CutCodes (with a field: and more
//    than half of the tiles mostly untrusted). Fade: the frame mean luma moved by FadeCodes
//    or more in the same direction on two consecutive frames. Either restarts the history
//    (gain 1 on that frame), as do Forget(), a non-consecutive sequence or a geometry change.
//  - Output Y' = Y + w ((g1 - 1) D1 + (g2 - 1) D2); chroma planes pass through.
namespace DetailPulse
{
	constexpr double kDefaultCap = 1.35;
	constexpr double kCapMin = 1.0, kCapMax = 1.5;

	// Band sigmas in plane px at 2160 lines and below (x Scale() above).
	constexpr double kSigmaFine = 1.0;
	constexpr double kSigmaWide = 2.5;
	// The tile: the measure and apply passes' 32 x 32 work block (16 x 16 threads, 2 x 2 px each).
	constexpr int kTilePx = 32;
	// History (research-gate tuning on D:\hfr\breathing, 2026-10-17): a window of 0.17 s (4
	// frames at 23.976) keeps the I-frame gain at 1.00 on CRF 32, where a P-frame five frames
	// before the I-frame was coded better than the I-frame itself (a 0.5 s window lifted that
	// I-frame by 1.04). The decay weighs the older frames of the window down a little.
	constexpr double kWindowSeconds = 0.17;
	constexpr double kDecayPerSecond = 0.9;
	constexpr double kReferenceHz = 23.976;
	// Estimator noise: an energy ratio within 1 % is steady detail (gain exactly 1).
	constexpr double kDeadZone = 0.01;
	// Tiles below this band RMS (8-bit codes, D1 and D2 together) are not measured.
	constexpr double kFloorCodes8 = 0.25;
	constexpr double kWinsorise = 4.0;
	constexpr double kTileEnergyJump = 3.0;
	// The no-vector veto: tile change above kVetoNoiseMul x the coding-noise RMS
	// (GrainPreserve::CodingNoiseCodes) starts vetoing, fully at twice that.
	constexpr double kVetoNoiseMul = 4.0;
	// With vectors: blocks whose trust (.z) is at or below kTrustMin are untrusted; a tile with
	// kVetoUntrustedShare of its area untrusted keeps gain 1.
	constexpr double kTrustMin = 0.5;
	constexpr double kVetoUntrustedShare = 0.25;
	constexpr double kCutMul = 3.0;      // x VetoCodes
	constexpr double kCutShare = 0.5;
	constexpr double kFadeCodes8 = 0.25;
	constexpr double kAheadMaxRatio = 2.5;
	constexpr int kRingSlots = 128;      // per-frame history slots (the window is far shorter)
	// The band passes keep their neighbourhoods in shared memory, sized for the sigmas at this
	// scale (5400 lines); taller planes keep these sigmas.
	constexpr double kMaxScale = 2.5;

	inline double Scale(double planeLines) { return (std::min)(GrainPreserve::GrainScale(planeLines), kMaxScale); }
	inline int FineRadius(double scale) { return static_cast<int>(std::ceil(3.0 * kSigmaFine * scale - 1e-9)); }
	inline double FineWeight(int i, double scale)
	{
		const double s = kSigmaFine * scale;
		const int r = FineRadius(scale);
		double sum = 0.0;
		for (int k = -r; k <= r; ++k) sum += std::exp(-0.5 * k * k / (s * s));
		return std::exp(-0.5 * i * i / (s * s)) / sum;
	}
	inline double WideSigma(double scale) { return kSigmaWide * scale; }
	inline double ClampCap(double cap) { return (std::max)(kCapMin, (std::min)(kCapMax, cap)); }
	inline double RateHz(double sourceHz) { return sourceHz > 0.0 ? sourceHz : kReferenceHz; }
	inline int WindowFrames(double sourceHz)
	{
		const long n = std::lround(kWindowSeconds * RateHz(sourceHz));
		return static_cast<int>((std::max)(1L, (std::min)(static_cast<long>(kRingSlots - 2), n)));
	}
	inline double DecayPerFrame(double sourceHz) { return std::pow(kDecayPerSecond, 1.0 / RateHz(sourceHz)); }
	inline double EightBitStep(int colorDepth) { return std::pow(2.0, colorDepth - 8); }
	inline double FloorEnergy(int colorDepth)
	{
		const double f = kFloorCodes8 * EightBitStep(colorDepth);
		return f * f;
	}
	inline double VetoCodes(double bitrateQuality, int colorDepth)
	{
		return kVetoNoiseMul * GrainPreserve::CodingNoiseCodes(bitrateQuality) * EightBitStep(colorDepth);
	}
	inline double CutCodes(double bitrateQuality, int colorDepth) { return kCutMul * VetoCodes(bitrateQuality, colorDepth); }
	inline double FadeCodes(int colorDepth) { return kFadeCodes8 * EightBitStep(colorDepth); }
	// The gain law (gain_law in the shader).
	inline double Gain(double reference, double energy, double cap)
	{
		if (!(energy > 0.0)) return 1.0;
		const double r = reference / energy;
		if (r <= 1.0 + kDeadZone) return 1.0;
		return (std::max)(1.0, (std::min)(ClampCap(cap), std::sqrt(r / (1.0 + kDeadZone))));
	}
	inline double VetoWeight(double change, double vetoCodes)
	{
		return 1.0 - GrainPreserve::Smoothstep(vetoCodes, 2.0 * vetoCodes, change);
	}
	inline double TrustWeight(double untrustedShare)
	{
		return 1.0 - GrainPreserve::Smoothstep(0.0, kVetoUntrustedShare, untrustedShare);
	}

	struct Params
	{
		double cap = kDefaultCap;      // detail_pulse_cap, clamped to [kCapMin, kCapMax]
		int colorDepth = 10;           // the signal's code depth
		double planeLines = 0.0;       // the picture's lines in this plane (re-based height); 0 = the luma plane's height
		double sourceHz = 0.0;         // the measured content rate; 0 = unknown (reference 23.976)
		double bitrateQuality = 1.0;   // SourceResolutionDetector reading (coding-noise allowance)
		// Vectors (optional): the de-judder's block field of this source frame (rgba = vx, vy in
		// native luma px, trust, error; one texel per 16x16 native block), only when valid for
		// this frame (MotionValidity). Null = the no-vector fallback.
		pl_tex field = nullptr;
		double fieldScale = 1.0;       // native px per plane px down (1 / re-base factor)
		double fieldScaleX = 0.0;      // across (0 = fieldScale)
		uint64_t sequence = 0;         // this source frame; a repeat (same) never advances the history
	};

	class Module
	{
	public:
		Module() = default;
		Module(const Module&) = delete;
		Module& operator=(const Module&) = delete;
		~Module();   // frees the CPU state only: call Release(gpu) first for the textures

		// `source` with its luma plane stabilised into this module's texture (other planes
		// pass through); false (and `out` untouched) on any failure: the caller renders the
		// frame as it was.
		bool Apply(pl_gpu gpu, pl_dispatch dispatch, const pl_frame& source, const Params& params, pl_frame& out);
		// Optional look-ahead: measure the next source frame's band energies (its luma plane,
		// in the same domain as Apply's: only when no re-base is engaged) so the reference
		// can include it. Safe to skip.
		bool MeasureAhead(pl_gpu gpu, pl_dispatch dispatch, pl_tex luma, uint64_t sequence, const Params& params);
		// A discontinuity (MotionValidity invalidation, re-base factor change, cut known to
		// the caller): the history restarts at the next frame.
		void Forget();
		void Release(pl_gpu gpu);
		const std::string& LastError() const { return error; }
		unsigned long long Failures() const { return failures; }
		unsigned long long Frames() const { return frames; }
		// Diagnostics (read-only): the source frame the history last advanced on, the
		// consecutive frames behind it, and how many Apply calls used a look-ahead.
		uint64_t HistorySequence() const;
		int HistoryFrames() const;
		unsigned long long AheadFrames() const { return aheadUsed; }

	private:
		struct Impl;
		Impl* impl = nullptr;
		std::string error;
		unsigned long long failures = 0, frames = 0, aheadUsed = 0;
	};
}
