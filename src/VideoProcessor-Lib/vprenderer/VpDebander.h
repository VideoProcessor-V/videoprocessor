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
	};

	// The threshold in codes from the readings (spec 2d / 2b): the content's
	// level step (2^(colorDepth - effectiveBits) codes, x1.5 for the mean's
	// spread) and, for a poor bitrate quality, up to three 8-bit steps. 0 =
	// nothing to deband.
	double Strength(double effectiveBits, int colorDepth, double bitrateQuality, bool useQuality);

	class Module
	{
	public:
		Module() = default;
		Module(const Module&) = delete;
		Module& operator=(const Module&) = delete;

		// `source` with every plane debanded into this module's textures;
		// false (and `out` untouched) on any failure: the caller renders the
		// frame as it was. `slot` 0 = the main frame (keeps the hold state),
		// 1 = a de-judder extra (decides per frame, leaves the state alone).
		bool Apply(pl_gpu gpu, pl_dispatch dispatch, const pl_frame& source, const Params& params, pl_frame& out, int slot = 0);
		void Release(pl_gpu gpu);
		// A motion discontinuity (MotionValidity, plan step 1 E): the hold weights
		// belong to the frames before it; the next frame starts a new hold run.
		// A missed-capture gap keeps the sequence contiguous, so the sequence
		// check alone would carry the hold across it.
		void ForgetHold() { weightSequence = 0; prevValid = false; }
		const std::string& LastError() const { return error; }
		unsigned long long Failures() const { return failures; }
		unsigned long long HeldFrames() const { return held; }

	private:
		static constexpr int kSlots = 2;
		pl_tex out[kSlots][PL_MAX_PLANES] = {};
		pl_tex weight[2][PL_MAX_PLANES] = {};   // blur weights (slot 0): this source frame's and the previous one's
		int curIndex = 0;                        // weight[curIndex] = weightSequence's
		bool prevValid = false;                  // weight[1 - curIndex] = weightSequence - 1's
		uint64_t weightSequence = 0;
		int weightW[PL_MAX_PLANES] = {}, weightH[PL_MAX_PLANES] = {};
		std::string error;
		unsigned long long failures = 0, held = 0;
	};
}
