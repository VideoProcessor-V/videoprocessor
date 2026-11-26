#pragma once

#include <VP-V/modules/repair/RepairStackOrder.h>
#include <VP-V/modules/repair/temporal_detail/products/TdProductReads.h>

#include <libplacebo/renderer.h>
#include <libplacebo/shaders/custom.h>

#include <cstdint>
#include <string>
#include <vector>

namespace VpSkeleton { class SkeletonMemo; }

// Stage 12 §2b / §2d (owner 2026-10-15): VP's HDR-safe debander in signal domain (fixed
// sample ring, no added grain, optional vector-informed weight hold, spatial grain guard or
// low-band grain split via kBandMemo with visible BandMemo::GlslOf fallback).
namespace VpDebander
{
	struct Params
	{
		double thresholdCodes = 0.0, radiusScale = 1.0;
		int colorDepth = 10;
		pl_tex field = nullptr, grainError = nullptr;
		double fieldScale = 1.0, fieldScaleX = 0.0;
		uint64_t sequence = 0;
		double holdAlpha = 0.5, grainCodes = 2.0;
		bool grainGuard = true, grainSplit = true;
		VpSkeleton::SkeletonMemo* memo = nullptr;
	};

	// The threshold in codes from the readings (spec 2d / 2b): the content's
	// level step (2^(colorDepth - effectiveBits) codes, x1.5 for the mean's
	// spread) and, for a poor bitrate quality, up to three 8-bit steps. 0 =
	// nothing to deband.
	// snapLevels (source_coding_fixes on, hunt 2 sweep): the occupancy reading counts a few
	// in-between codes (an upscale's ramps, a PQ remap) and so reads 8-bit content in a 10-bit
	// signal at ~8.8 bits, which would give 1.5 x 2^1.2 = 3.4 codes: below the real 4-code
	// step. Content has a whole number of bits, so the step is taken at floor(bits + 0.1)
	// (8.8 -> 8 -> 6 codes), and readings within half a bit of the signal depth are the
	// signal's own depth (true 10-bit with grain reads 9.6 or more). false = the earlier formula.
	double Strength(double effectiveBits, int colorDepth, double bitrateQuality, bool useQuality, bool snapLevels = false);

	class Module
	{
	public:
		Module() = default;
		Module(const Module&) = delete;
		Module& operator=(const Module&) = delete;

		// `source` with every plane debanded into this module's textures;
		// false (and `out` untouched) on any failure: the caller renders the
		// frame as it was. `slot` 0 = the main frame (keeps the hold state),
		// 1 = a de-judder extra (decides per frame, leaves the state alone); hunt 3:
		// kSlotLookAhead = the detail pulse's look-ahead picture, kSlotCrossfade = the
		// crossfade's path being left (both per frame, the state left alone; their own
		// textures, so the frame a lower slot returned stays valid).
		bool Apply(pl_gpu gpu, pl_dispatch dispatch, const pl_frame& source, const Params& params, pl_frame& out, int slot = 0);
		void Release(pl_gpu gpu);
		// A motion discontinuity (MotionValidity, plan step 1 E): the hold weights
		// belong to the frames before it; the next frame starts a new hold run.
		// A missed-capture gap keeps the sequence contiguous, so the sequence
		// check alone would carry the hold across it.
		void ForgetHold() { weightSequence = 0; prevValid = false; }
		// Interpolation Stage B: a source frame no refresh showed (a source faster than the
		// display). The hold carries over it - the last weights stand for it - so the next
		// shown frame still holds; a broken run (ForgetHold on an invalidation, a hole) does
		// not carry.
		void CarryHold(uint64_t sequence)
		{
			if (weightSequence != 0 && sequence == weightSequence + 1)
			{
				weightSequence = sequence;
				prevValid = false;
			}
		}
		uint64_t HoldSequence() const { return weightSequence; }
		void SetHoldSequenceForTest(uint64_t sequence) { weightSequence = sequence; }
		// The grain guard's busy-share texture of a slot's plane (grainGuard on, grainSplit off; null otherwise): the guard parity test.
		pl_tex GuardTextureForTest(int slot, int plane) const { return (slot >= 0 && slot < kSlots && plane >= 0 && plane < PL_MAX_PLANES) ? guard[slot][plane] : nullptr; }
		// Fallback coarse tensor texture (allocated only when !useP11) and private low-band texture (allocated only when P10/kBandMemo absent).
		pl_tex SplitTensorTextureForTest(int slot, int plane) const { return (slot >= 0 && slot < kSlots && plane >= 0 && plane < PL_MAX_PLANES) ? splitTensor[slot][plane] : nullptr; } pl_tex SplitLowTextureForTest(int slot, int plane) const { return (slot >= 0 && slot < kSlots && plane >= 0 && plane < PL_MAX_PLANES) ? splitLow[slot][plane] : nullptr; }
		const std::string& LastError() const { return error; }
		const std::string& LastFailureError() const { return lastFailureError; }
		// R1-W1-RES (doc 72 item 2): the debander is the FALLBACK LAYER under temporal detail (order TD -> deband -> CR). Its read path
		// of td.maps (validity, stability) and coding_evidence: reset 1 = not requested, the debander decides exactly as today
		// everywhere; a requested engine with a product absent keeps today's deband everywhere and says so (engaged decision).
		const TdProducts::ReadResult& TemporalDetailRead() const { return tdRead; }
		// WP9 (wave S2): repair stack order is TD -> deband -> contrast recovery (fallback layer under TD).
		// Built on status refresh (not per-frame); when TD is requested but unusable the line says so and why,
		// appended by the host to the "VP deband" status line (recovers by itself).
		RepairStack::Plan StackPlan() const { return RepairStack::Decide(tdRead.requested, tdRead.usable, tdRead.reason); }
		std::string StackStatusLine() const { return StackPlan().status; }
		unsigned long long Failures() const { return failures; }
		unsigned long long HeldFrames() const { return held; }
		unsigned long long BandMemoHits() const;
		unsigned long long BandMemoFallbacks() const;
		const std::string& LastBandMemoFallback() const;
		unsigned long long GrainGuardHits() const;
		unsigned long long GrainGuardFallbacks() const;
		const std::string& LastGrainGuardFallback() const;
		static constexpr int kSlotLookAhead = 2;
		static constexpr int kSlotCrossfade = 3;

	private:
		static constexpr int kSlots = 4;
		pl_tex out[kSlots][PL_MAX_PLANES] = {};
		pl_tex weight[2][PL_MAX_PLANES] = {};   // blur weights (slot 0): this source frame's and the previous one's
		pl_tex weightDummy = nullptr;           // 1x1 r32f sink bound on non-main slots (reuses the compiled shader, no cold compile)
		pl_tex guard[kSlots][PL_MAX_PLANES] = {};   // grainGuard: the spatial grain share per 16x16 block
		pl_tex splitHalf[kSlots][PL_MAX_PLANES] = {}, splitTmp[kSlots][PL_MAX_PLANES] = {};
		pl_tex splitLow[kSlots][PL_MAX_PLANES] = {}, splitTensor[kSlots][PL_MAX_PLANES] = {};
		bool SplitLowBand(pl_gpu gpu, pl_dispatch dispatch, pl_tex in, int slot, int plane, double scale,
			const float activeRect[4], pl_tex acquiredLow, pl_tex& splitLowOut);
		int curIndex = 0;                        // weight[curIndex] = weightSequence's
		bool prevValid = false;                  // weight[1 - curIndex] = weightSequence - 1's
		uint64_t weightSequence = 0;
		int weightW[PL_MAX_PLANES] = {}, weightH[PL_MAX_PLANES] = {};
		std::string error, lastFailureError, lastBandMemoFallback, lastGrainGuardFallback, headerScratch;
		std::string cachedBandWeights[PL_MAX_PLANES];
		std::vector<pl_shader_desc> descScratch;
		double cachedBandScale[PL_MAX_PLANES] = { -1.0, -1.0, -1.0, -1.0 };
		TdProducts::ReadResult tdRead;
		unsigned long long failures = 0, held = 0, bandMemoHits = 0, bandMemoFallbacks = 0, grainGuardHits = 0, grainGuardFallbacks = 0;
	};
}
