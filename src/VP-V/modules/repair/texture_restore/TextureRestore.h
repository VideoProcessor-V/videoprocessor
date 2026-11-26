#pragma once

#include <VP-V/core/ScratchArena.h>
#include <VP-V/modules/control/skeleton/PlTexBackend.h>
#include <VP-V/modules/repair/grain_split/GrainSplit.h>
#include <VP-V/modules/repair/grain_split/shared/BandFromBase.h>
#include <VP-V/modules/repair/texture_restore/SceneRestart.h>

#include <libplacebo/renderer.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace VpSkeleton { class SkeletonMemo; }

// texture_restore: off | statistical (owner 2026-10-17; docs/VP-detail-restore.md).
// Splits luma detail into grain (Wiener-separated, scaled towards best-coded recent grain
// level; never synthesised) and focus-gated scene texture, using three U2 bands (s1/s2/s3 =
// 1/2.5/6 px x GrainScale), motion-carried peak-hold references, and U7 cut/fade handling.
// Runs after detail_pulse_stabilise; chroma untouched; never starts the matcher.
namespace TextureRestore
{
	constexpr double kDefaultCap = 1.5;
	constexpr double kCapMin = 1.0, kCapMax = 2.0;

	// --- Shared constants (shader and CPU model) ---
	constexpr double kReferenceHz = 23.976;    // the content rate when it is unknown
	// ND03: the band sigmas are the U2 unit's table (VpUnit::BandFromBase::kSigma*: 1 / 2.5 / 6 px). ND04: the grain
	// constants are repair.grain_split's ONE law (GrainSplit::DefaultParams: 16 px tile, 4 sub-blocks, 4 quiet, 2,
	// 2.2 / 3.8, kappa 2). Read here by name, never copied; the values are unchanged.
	constexpr GrainSplit::Params kGrainLaw = GrainSplit::DefaultParams();
	constexpr double kSigma1 = VpUnit::BandFromBase::kSigmaFine, kSigma2 = VpUnit::BandFromBase::kSigmaBase, kSigma3 = VpUnit::BandFromBase::kSigmaLow;   // band splits (px at <= 2160 lines)
	constexpr double kTilePx = kGrainLaw.blockPx;                     // tile size at <= 2160 lines (x GrainScale)
	constexpr int kSubBlocks = kGrainLaw.subBlocks;                   // per tile axis (16 sub-blocks)
	constexpr int kQuietSubBlocks = kGrainLaw.quietSubBlocks;         // the quietest sub-blocks averaged for the grain level
	constexpr int kGrainNeighbourhood = kGrainLaw.neighbourhood;      // grain level: the minimum over (2 x 2 + 1)^2 tiles
	constexpr double kGrainCal1 = kGrainLaw.grainCal, kGrainCal2 = kGrainLaw.grainCal2;   // pure grain reads about its own energy
	constexpr double kGrainExcess = 1.25;      // texture = the energy above this many grain levels
	constexpr double kWienerKappa = kGrainLaw.kappa;                  // pixel split: texture where the local energy exceeds kappa x grain
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
	constexpr double kEnergyEps = 1e-6;        // codes^2

	inline double Clamp(double v, double lo, double hi) { return v < lo ? lo : (v > hi ? hi : v); }
	// Grain-sized distances grow with the plane's height above 2160 lines (GrainPreserve::GrainScale).
	inline double Scale(double planeLines) { return planeLines > 2160.0 ? planeLines / 2160.0 : 1.0; }
	// The tile in plane px: a multiple of kSubBlocks.
	inline int TilePx(double scale)
	{
		const int sub = (std::max)(1, static_cast<int>(std::lround(kTilePx * scale / kSubBlocks)));
		return sub * kSubBlocks;
	}
	// Thin compatibility forwarders to U2 (VpUnit::BandFromBase) for existing tests/models.
	inline int Radius1(double scale) { return VpUnit::BandFromBase::BandRadius(VpUnit::BandFromBase::kSigmaFine * scale); }
	inline double Weight1(int k, double scale) { return VpUnit::BandFromBase::BandWeight(k, VpUnit::BandFromBase::kSigmaFine * scale); }
	// The half-resolution sigma of a full-resolution split (the 2x2 box adds 0.25 px^2 per axis).
	inline double HalfSigma(double sigma) { return VpUnit::BandFromBase::HalfSigma(sigma); }
	inline int HalfRadius(double sigma) { return VpUnit::BandFromBase::Radius(VpUnit::BandFromBase::HalfSigma(sigma), 3.5, true, 1e-9); }
	inline double HalfWeight(int k, double sigma) { return VpUnit::BandFromBase::Weight(k, VpUnit::BandFromBase::HalfSigma(sigma), HalfRadius(sigma)); }
	struct CpuBandResult
	{
		std::vector<float> samples;
		bool fromBandMemo = false;
		std::string fallbackReason;
	};
	CpuBandResult AcquireFineCpuBand(const std::vector<float>& plane, int w, int h, double scale,
		VpSkeleton::SkeletonMemo* memo = nullptr, uint64_t seq = 0);
	CpuBandResult AcquireHalfCpuBand(const std::vector<float>& halfPlane, int hw, int hh, double fullSigmaPx,
		VpSkeleton::SkeletonMemo* memo = nullptr, uint64_t seq = 0);
	// One 8-bit code in the signal's codes.
	inline double EightBitStep(int colorDepth) { return std::pow(2.0, (std::max)(0, colorDepth - 8)); }
	// The expected coding-noise RMS in 8-bit codes (as GrainPreserve::CodingNoiseCodes).
	inline double CodingNoiseCodes8(double bitrateQuality)
	{
		const double q = Clamp(bitrateQuality, 0.0, 1.0);
		return 0.5 + 2.5 * (1.0 - q);
	}
	// The low-band change allowance (RMS of the change), in the signal's codes.
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
		VpSkeleton::ScratchArena<VpSkeleton::PlTexBackend>* arena = nullptr;  // SA1: null = private textures as before
		// W23-IR-H1: active picture (black bars left out), normalised over the plane [x0, y0, x1, y1]; empty = full plane.
		pl_rect2df picture{};
		// The reader of the source frame's scene group (the shared cut / fade verdict, U7 via P4); null = the skeleton's store.
		VpDetailRestore::SceneReaderFn sceneReader = nullptr;
		// cut_soft_reset (default on): soft reset on high-confidence non-cut candidate (SceneRestart.h).
		bool softReset = VpDetailRestore::kDefaultCutSoftReset;
		bool vectorVeto = false;  // W23-IR-H2: default OFF per SceneRestart.h:7 and 115:1435
		VpDetailRestore::VectorTrustReaderFn vectorTrustReader = nullptr;  // null = query memo P7/P2
		VpSkeleton::SkeletonMemo* memo = nullptr;  // optional: acquire shared P11 band energies
	};

	// W23-IR-H1: active picture in plane px, inclusive (x0, y0, x1, y1), matching DetailPulse::PictureBounds.
	inline bool PictureBounds(const pl_rect2df& r, int w, int h, int out[4])
	{
		out[0] = 0; out[1] = 0; out[2] = w - 1; out[3] = h - 1;
		const float x0 = (std::min)(r.x0, r.x1), x1 = (std::max)(r.x0, r.x1);
		const float y0 = (std::min)(r.y0, r.y1), y1 = (std::max)(r.y0, r.y1);
		if (!(x1 > x0) || !(y1 > y0) || w <= 0 || h <= 0)
			return false;
		const double eps = 1e-4;
		const int lx = (std::max)(0, static_cast<int>(std::ceil(x0 * static_cast<double>(w) - eps)));
		const int ly = (std::max)(0, static_cast<int>(std::ceil(y0 * static_cast<double>(h) - eps)));
		const int hx = (std::min)(w - 1, static_cast<int>(std::floor(x1 * static_cast<double>(w) + eps)) - 1);
		const int hy = (std::min)(h - 1, static_cast<int>(std::floor(y1 * static_cast<double>(h) + eps)) - 1);
		if (hx < lx || hy < ly || (lx == 0 && ly == 0 && hx == w - 1 && hy == h - 1))
			return false;
		out[0] = lx; out[1] = ly; out[2] = hx; out[3] = hy;
		return true;
	}

	// SA1: the scratch arena scopes (one per Apply / ApplyForeign call) and the temporaries they ask for.
	constexpr uint16_t kArenaScopeApply = 0x0101, kArenaScopeForeign = 0x0102;
	// Tier T1. Apply: the half-resolution `half` (one float-1 texture) and `tmp` (one float-2 texture).
	// With foreignW > 0 also ApplyForeign's trio at half of the foreign dims (one float-1, two float-2).
	// The formats are the ones Apply uses (FloatFormat). Empty when a format does not map.
	std::vector<VpSkeleton::ArenaRequest> ArenaRequests(pl_gpu gpu, int planeW, int planeH, int foreignW = 0, int foreignH = 0);

	class Module
	{
	public:
		Module() = default;
		~Module();
		Module(const Module&) = delete;
		Module& operator=(const Module&) = delete;

		// Restores luma plane of `source` into this module's texture (other planes pass through); false on failure.
		// cap 1.0 returns `source`; repeated sequence reuses gains with its own low bands (F7); non-consecutive restarts.
		bool Apply(pl_gpu gpu, pl_dispatch dispatch, const pl_frame& source, const Params& params, pl_frame& out);
		// Applies current frame's gains (HistorySequence()) on a picture of another geometry without advancing references.
		bool ApplyForeign(pl_gpu gpu, pl_dispatch dispatch, const pl_frame& source, const Params& params, pl_frame& out);
		// A discontinuity (MotionValidity invalidation, re-base change): references restart.
		void Forget();
		void Release(pl_gpu gpu);
		const std::string& LastError() const { return error; }
		unsigned long long Failures() const { return failures; }
		unsigned long long Frames() const { return frames; }
		// True when the LAST Apply / ApplyForeign failed because the scratch arena had no texture.
		bool ScratchShort() const { return scratchShort; }
		// Frames restarted because no scene group was readable (never judged privately).
		unsigned long long SceneMissing() const { return sceneMissing; }
		// The last judged frame had no scene record and restarted (shown in Ctrl+I / Config status, never silent).
		bool SceneJudgeUnavailable() const { return sceneUnavailable; }
		const char* SceneJudgeWhy() const { return sceneWhy ? sceneWhy : VpDetailRestore::SceneUnavailableWhy(); }
		// U7 v3 partial reset on fades (SceneRestart.h): fade frames whose references were reduced (no restart), fade frames restarted
		// because their group had no keep (shown through SceneJudgeUnavailable / SceneJudgeWhy), and the last judged frame's keep
		// (amplitude, 1 = no fade, 0 = restart).
		unsigned long long ScenePartials() const { return scenePartials; }
		unsigned long long SceneFadeRestarts() const { return sceneFadeRestarts; }
		float LastHistoryKeep() const { return lastHistoryKeep; }
		// cut_soft_reset: frames whose references were reduced by a soft reset (no restart; 0 with Params::softReset off), and
		// the sum of their strengths (the mean graded strength = SceneSoftStrengthSum() / SceneSoftResets()).
		unsigned long long SceneSoftResets() const { return sceneSoftResets; }
		double SceneSoftStrengthSum() const { return sceneSoftStrengthSum; }
		// cut_soft_reset, no silent fallbacks (M3): the scene groups read lately were all provisional, so the switch cannot act
		// (VpDetailRestore::SoftResetWatch; the status line says "soft reset: on (inactive: provisional verdicts only)"), and the
		// frames the rule would have fired on had their verdict been confirmed.
		bool SceneSoftResetInactive() const { return softWatch.Inactive(); }
		unsigned long long SceneSoftWaiting() const { return softWatch.waiting; }
		uint64_t HistorySequence() const;
		unsigned long long P11EnergyHits() const;
		unsigned long long P11EnergyFallbacks() const;
		const std::string& LastP11EnergyFallback() const;
		unsigned long long SceneVectorVetoes() const;

	private:
		struct Impl;
		Impl* impl = nullptr;
		std::string error, lastP11EnergyFallback;
		unsigned long long failures = 0, frames = 0, sceneMissing = 0, scenePartials = 0, sceneFadeRestarts = 0;
		unsigned long long sceneSoftResets = 0, sceneVectorVetoes = 0, p11EnergyHits = 0, p11EnergyFallbacks = 0;
		double sceneSoftStrengthSum = 0.0;
		VpDetailRestore::SoftResetWatch softWatch;
		bool sceneUnavailable = false;
		const char* sceneWhy = nullptr;
		float lastHistoryKeep = 1.0f;
		bool scratchShort = false;
	};
}
