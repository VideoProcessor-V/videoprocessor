#pragma once

#include <libplacebo/renderer.h>

#include <cstdint>
#include <string>

// Stage 12 §2b / §2d (owner 2026-10-15): VP's own debander, for any
// low-bitrate source (native 4K streams included) and for 8-bit content in a
// deeper signal. libplacebo's algorithm - a random ring of four samples
// around each pixel, their mean taken where it is within a threshold of the
// pixel - reimplemented as a VP pass on the source planes, at source
// resolution, in the signal domain (so on a PQ source the threshold is in PQ
// units, and one band step means the same thing in shadows and highlights).
//
// Differences from libplacebo's (owner: its HDR black-speck bug): no grain is
// added (VP's own output dither does that job), and the sample ring is fixed
// per pixel position (no per-frame random), so a still picture never
// shimmers.
//
// Vector-informed (owner 2026-10-15; the standing fallback rule): where the
// de-judder's vectors exist for this frame and trust the block, each pixel's
// blur weight is held along the motion from the previous frame's (a one-pole
// over ~2 frames), so deband regions neither flicker nor crawl; blocks the
// grain error marks as texture are not blurred. Without vectors (or on an
// untrusted block) it decides per frame. It never starts the matcher.
namespace VpDebander
{
	struct Params
	{
		// The threshold, in codes of the signal (1023-scale for 10-bit): the
		// content's own level step and the low-bitrate allowance, already
		// combined by the caller (Strength()).
		double thresholdCodes = 0.0;
		int colorDepth = 10;        // the signal's code depth
		double radiusScale = 1.0;   // sample radius: 16 px at 1080 lines, by height
		// Vectors (optional): the de-judder's block field of this source frame
		// (rgba = vx, vy source px of the full-size luma, trust, error) and its
		// grain error (r = 8-bit codes). Null = per-frame decisions.
		pl_tex field = nullptr;
		pl_tex grainError = nullptr;
		double fieldScale = 1.0;    // source px of the field per px of the plane's full-size luma (re-base factor inverse)
		double fieldScaleX = 0.0;   // the same across, when the re-base is per axis (0 = fieldScale)
		uint64_t sequence = 0;      // this source frame (holds only across consecutive frames)
		double holdAlpha = 0.5;     // weight of this frame's decision where held
		double grainCodes = 2.0;    // grain error (8-bit codes) above which a block is texture
		// deband_vp_grain_guard (owner 2026-10-17: grain is never averaged away; grain audit:
		// without vectors the debander kept 0.59x of 0.5-code grain): a per-frame spatial
		// grain estimate - the share of pixels per 16x16 block whose Laplacian exceeds a third
		// of the threshold or that are local extrema (GrainPreserve.h) - vetoes the blur where it is consistent with grain (or texture),
		// with or without vectors. A banded gradient is flat but for its band edges and is
		// debanded as before. false = the 2026-10-16 debander, byte for byte. Integration
		// 2026-10-17: on is the struct's default as well as the profile's.
		bool grainGuard = true;
		// deband_vp_grain_guard split (hunt 2 07-scaling H1: the spatial-share veto above
		// also vetoed real coded banding that kept some noise, the usual look of a starved
		// stream): with grainGuard on, each pixel is split into a low band and a residual
		// (GrainPreserve::kSplitSigma); only the low band is debanded - its sharp step part
		// put back where coherent along the contour - and the residual (grain, noise)
		// passes untouched. No veto: noisy bands are debanded, grain keeps 0.96x or more.
		// false (with grainGuard on) = the spatial-share veto (deband_vp_grain_guard veto).
		bool grainSplit = true;
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
		const std::string& LastError() const { return error; }
		unsigned long long Failures() const { return failures; }
		unsigned long long HeldFrames() const { return held; }
		static constexpr int kSlotLookAhead = 2;
		static constexpr int kSlotCrossfade = 3;

	private:
		static constexpr int kSlots = 4;
		pl_tex out[kSlots][PL_MAX_PLANES] = {};
		pl_tex weight[2][PL_MAX_PLANES] = {};   // blur weights (slot 0): this source frame's and the previous one's
		pl_tex guard[kSlots][PL_MAX_PLANES] = {};   // grainGuard: the spatial grain share per 16x16 block
		// grainSplit: the half-resolution low band (box, across, down) and the structure
		// tensor per block of it.
		pl_tex splitHalf[kSlots][PL_MAX_PLANES] = {}, splitTmp[kSlots][PL_MAX_PLANES] = {};
		pl_tex splitLow[kSlots][PL_MAX_PLANES] = {}, splitTensor[kSlots][PL_MAX_PLANES] = {};
		bool SplitLowBand(pl_gpu gpu, pl_dispatch dispatch, pl_tex in, int slot, int plane, int ncomp, double scale, double codeUnit);
		int curIndex = 0;                        // weight[curIndex] = weightSequence's
		bool prevValid = false;                  // weight[1 - curIndex] = weightSequence - 1's
		uint64_t weightSequence = 0;
		int weightW[PL_MAX_PLANES] = {}, weightH[PL_MAX_PLANES] = {};
		std::string error;
		unsigned long long failures = 0, held = 0;
	};
}
