#pragma once

#include <libplacebo/renderer.h>

#include <VP-V/core/LumaWeights.h>
#include <VP-V/core/Ramp.h>

#include <vprenderer/GrainPreserve.h>
#include <VP-V/modules/repair/deband/VpDebander.h>
#include <VP-V/modules/repair/grain_split/shared/BandFromBase.h>
#include <VP-V/modules/repair/texture_restore/SceneRestart.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace VpSkeleton { class SkeletonMemo; }

// detail_pulse_stabilise (owner 2026-10-17; docs/VP-detail-restore.md): scales each frame's
// own luma detail (two bands: fine D1 = Y - G(s1)Y, mid D2 = G(s1)Y - L, where wide L is
// acquired via kBandMemo when available with visible BandMemo::GlslOf fallback) towards its
// best-coded recent neighbour. Nothing is copied between frames (grain stays per-frame).
// Runs after VpDebander; never touches the source-resolution monitor or starts the matcher.
// Shared maths with DetailPulseModel.h: 32x32 tiles, winsorised frame energy, GPU history ring
// with decay + optional look-ahead, dead-zone capped gain, vector/change tile veto, U7 scene
// cut restart + fade/soft-reset partial keep, output Y' = Y + w((g1 - 1)D1 + (g2 - 1)D2).
namespace DetailPulse
{
	constexpr double kDefaultCap = 1.35;
	constexpr double kCapMin = 1.0, kCapMax = 1.5;

	// Band sigmas in plane px at 2160 lines and below (x Scale() above), imported from shared unit U2.
	using VpUnit::BandFromBase::kSigmaFine;
	// The tile: the measure and apply passes' 32 x 32 work block (16 x 16 threads, 2 x 2 px each).
	constexpr int kTilePx = 32;
	constexpr bool kDefaultFast = true;   // detail_pulse_fast
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
	constexpr double kAheadMaxRatio = 2.5;
	constexpr int kRingSlots = 128;      // per-frame history slots (the window is far shorter)
	// The band passes keep their neighbourhoods in shared memory, sized for the sigmas at this
	// scale (5400 lines); taller planes keep these sigmas.
	constexpr double kMaxScale = 2.5;

	inline double Scale(double planeLines) { return (std::min)(GrainPreserve::GrainScale(planeLines), kMaxScale); }
	// The fine band's Gaussian is the U2 unit's one band Gaussian (ND03: VpUnit::BandFromBase::BandRadius, ceil(3 sigma), at
	// least one tap; FIX-B2-ND03: the same radius as the old ceil(3 sigma - 1e-9), no minimum, at every scale this module runs).
	inline int FineRadius(double scale) { return VpUnit::BandFromBase::BandRadius(VpUnit::BandFromBase::kSigmaFine * scale); }
	inline double FineWeight(int i, double scale) { return VpUnit::BandFromBase::BandWeight(i, VpUnit::BandFromBase::kSigmaFine * scale); }
	inline double WideSigma(double scale) { return VpUnit::BandFromBase::kSigmaBase * scale; }
	struct CpuBandResult { std::vector<float> samples; bool fromBandMemo = false; std::string fallbackReason; };
	CpuBandResult AcquireFineCpuBand(const std::vector<float>& plane, int w, int h, double scale,
		VpSkeleton::SkeletonMemo* memo = nullptr, uint64_t seq = 0);
	CpuBandResult AcquireWideCpuBand(const std::vector<float>& halfPlane, int hw, int hh, double scale,
		VpSkeleton::SkeletonMemo* memo = nullptr, uint64_t seq = 0);
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
		return 1.0 - RampMath::Smoothstep<double>(vetoCodes, 2.0 * vetoCodes, change, RampMath::SmoothRule::Unguarded);
	}
	inline double TrustWeight(double untrustedShare)
	{
		return 1.0 - RampMath::Smoothstep<double>(0.0, kVetoUntrustedShare, untrustedShare, RampMath::SmoothRule::Unguarded);
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
		// Hunt 3 F8/F9: a one-plane RGB(A) picture (an RGB capture, the subtitle move's composed
		// picture) is stabilised on its luma Y = Kr R + Kg G + Kb B, the same delta added to R, G
		// and B (adding d to R, G and B adds d to Y and leaves Cb, Cr unchanged, so it is the
		// YCbCr pulse exactly). lumaSystem: the YCbCr system the picture was decoded from (its
		// Kr, Kb; unknown = BT.2020 NC for BT.2020 primaries, else BT.709). codeLevels: the code
		// domain a float RGB picture's luma is measured in (the YCbCr source's levels, so a
		// composed picture reads the same codes as the planes it was made from; unknown = the
		// frame's own levels). A UNORM RGB plane reads its own codes.
		enum pl_color_system lumaSystem = PL_COLOR_SYSTEM_UNKNOWN;
		enum pl_color_levels codeLevels = PL_COLOR_LEVELS_UNKNOWN;
		// detail_pulse_fast (2026-10-18, default on): the same maths in fewer texture reads (2 x 2
		// quads by textureGather, the measure pass's low band reused by a new frame's apply pass,
		// blocks whose weight or gains are exactly 1 copied). Off = the original passes (A/B).
		bool fast = kDefaultFast;
		// Bug hunt 33 F1 (2026-10-31): the active picture (black bars left out), normalised over
		// the plane (x0, y0, x1, y1; the raster's trusted picture over its size: the re-base scales
		// the whole raster); empty = the whole plane. Only pixels wholly inside are measured and
		// changed, every code read is clamped to it (its edge acts as the plane's border, so the
		// bar edge is no unsharp-mask edge: no undershoot into the bars, no overshoot beside them),
		// and the bars come out as the source. PictureBounds() has the rounding.
		pl_rect2df picture{};
		// The reader of the source frame's scene group (the shared cut / fade verdict, U7 via P4); null = the skeleton's store.
		VpDetailRestore::SceneReaderFn sceneReader = nullptr;
		// cut_soft_reset (default on): the soft reset on a high-confidence non-cut candidate (SceneRestart.h; the bar is
		// CutFadeJudge::kSoftResetBar, provisional). Off = no soft reset; cuts restart and fades reduce exactly as before.
		bool softReset = VpDetailRestore::kDefaultCutSoftReset;
		VpSkeleton::SkeletonMemo* memo = nullptr;   // optional: acquire shared kBandMemo GPU wide low band
	};

	// The active picture in plane px, inclusive (x0, y0, x1, y1): the pixels wholly inside the
	// normalised rectangle. False (out = the whole plane) when it is empty, covers the plane, or
	// no pixel is wholly inside.
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

	// Hunt 3 F8: Kr, Kg, Kb of a YCbCr system (the luma of an RGB picture). Inline: adaptive
	// scaling uses it too (bug hunt 33 F3).
	// The table lives in core/LumaWeights.h (ND02); this forwards to it.
	inline void LumaWeights(enum pl_color_system system, enum pl_color_primaries primaries, double out[3])
	{
		LumaCoeff::ForSystem(system, primaries, out);
	}

	// Diagnostics (tests, one GPU at a time): GPU time per pass kind - 0 measure, 1 reduce,
	// 2 apply, 3 low band (detail_pulse_fast; look-ahead and other-geometry passes are not
	// timed) - averaged over the completed passes since the last reset.
	void SetPassTimers(pl_gpu gpu, bool on);
	void PassTimes(double ms[4], bool reset);

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
		// A repeat (the same sequence again) applies the stored decision to the picture given,
		// with that picture's own bands (hunt 3 F6: a Stage B phase picture, a gap fill or a
		// de-judder extra is another picture than A).
		bool Apply(pl_gpu gpu, pl_dispatch dispatch, const pl_frame& source, const Params& params, pl_frame& out);
		// Hunt 3 (the crossfade's path being left): the current decision (HistorySequence())
		// on a picture of another plane geometry (another re-base factor). Its own bands at its
		// own scale (params.planeLines), the tile weights read at the same picture position;
		// never advances or restarts the history; its own output texture, so the frame Apply
		// returned stays valid. False when no decision is held.
		bool ApplyForeign(pl_gpu gpu, pl_dispatch dispatch, const pl_frame& source, const Params& params, pl_frame& out);
		// Optional look-ahead: measure the next source frame's band energies (its luma plane,
		// in the same domain as Apply's: only when no re-base is engaged) so the reference
		// can include it. Safe to skip.
		bool MeasureAhead(pl_gpu gpu, pl_dispatch dispatch, pl_tex luma, uint64_t sequence, const Params& params);
		// Hunt 3 F4: the look-ahead in Apply's domain. Apply measures the frame after VP's
		// debander; the next frame is debanded the same way first (per picture, the debander's
		// look-ahead slot, its hold left alone) when `debander` and a threshold are given.
		bool MeasureAhead(pl_gpu gpu, pl_dispatch dispatch, const pl_frame& next, uint64_t sequence, const Params& params,
			VpDebander::Module* debander, const VpDebander::Params* deband);
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
		// Frames restarted because no scene group was readable (never judged privately).
		unsigned long long SceneMissing() const { return sceneMissing; }
		// The last judged frame had no scene record and restarted (shown in Ctrl+I / Config status, never silent).
		bool SceneJudgeUnavailable() const { return sceneUnavailable; }
		const char* SceneJudgeWhy() const { return sceneWhy ? sceneWhy : VpDetailRestore::SceneUnavailableWhy(); }
		// U7 v3 partial reset on fades (SceneRestart.h): fade frames whose history was reduced (no restart), fade frames restarted
		// because their group had no keep (shown through SceneJudgeUnavailable / SceneJudgeWhy), and the last judged frame's keep
		// (amplitude, 1 = no fade, 0 = restart).
		unsigned long long ScenePartials() const { return scenePartials; }
		unsigned long long SceneFadeRestarts() const { return sceneFadeRestarts; }
		float LastHistoryKeep() const { return lastHistoryKeep; }
		// cut_soft_reset: frames whose history was reduced by a soft reset (no restart; 0 with Params::softReset off), and the
		// sum of their strengths (the mean graded strength = SceneSoftStrengthSum() / SceneSoftResets()).
		unsigned long long SceneSoftResets() const { return sceneSoftResets; }
		double SceneSoftStrengthSum() const { return sceneSoftStrengthSum; }
		// cut_soft_reset, no silent fallbacks (M3): the scene groups read lately were all provisional, so the switch cannot act
		// (VpDetailRestore::SoftResetWatch; the status line says "soft reset: on (inactive: provisional verdicts only)"), and the
		// frames the rule would have fired on had their verdict been confirmed.
		bool SceneSoftResetInactive() const { return softWatch.Inactive(); }
		unsigned long long SceneSoftWaiting() const { return softWatch.waiting; }
		unsigned long long BandMemoHits() const { return bandMemoHits; }
		unsigned long long BandMemoFallbacks() const { return bandMemoFallbacks; }
		const std::string& LastBandMemoFallback() const { return lastBandMemoFallback; }

	private:
		struct Impl;
		Impl* impl = nullptr;
		std::string error, lastBandMemoFallback;
		unsigned long long failures = 0, frames = 0, aheadUsed = 0, sceneMissing = 0, scenePartials = 0, sceneFadeRestarts = 0;
		unsigned long long sceneSoftResets = 0, bandMemoHits = 0, bandMemoFallbacks = 0;
		double sceneSoftStrengthSum = 0.0;
		VpDetailRestore::SoftResetWatch softWatch;
		bool sceneUnavailable = false;
		const char* sceneWhy = nullptr;
		float lastHistoryKeep = 1.0f;
	};
}
