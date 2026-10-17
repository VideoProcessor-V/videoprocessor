#pragma once

#include <libplacebo/dispatch.h>
#include <libplacebo/gpu.h>
#include <libplacebo/log.h>

#include <vprenderer/DejudderWarp.h>
#include <vprenderer/InterpolationPolicy.h>

#include <string>

// VP's HDR-safe optical-flow frame interpolator (docs/VP-interpolator-50-60-plan.md
// step 3; docs/VP-nvof-ahead.md "The HDR-compatible version"). A standalone GPU
// component in MotionWarp's dispatch style.
//
// Given two consecutive source frames A and B as the renderer's upload planes
// (a Y plane and an interleaved CbCr plane; the same layout as the de-judder's
// Dejudder::Planes), the A->B block field (MotionWarp::Field(): one texel per
// 16x16 source block, rgba = vx, vy in source pixels, trust, match error; from
// the matcher, seeded by the optical-flow engine or computed ahead) and a phase
// t in [0, 1], it writes the picture at t:
//   - t <= 0 is A exactly and t >= 1 is B exactly (the input planes themselves:
//     nothing is dispatched);
//   - a trusted position takes the vector of the content that is there at t
//     (the field's own rule, FieldVectorGlsl: bilinear between trusted block
//     centres, the position's block decides; one fixed-point step) and blends
//     A at p - t v with B at p + (1 - t) v by (1 - t, t) - a bidirectional warp;
//     where B's occlusion mask (optional) says B's content at the landing is new
//     (not seen in A), A's content goes behind it (occlusion): the A sample alone;
//   - an untrusted position (an occlusion hole or a failed match) takes the
//     fallback: hold = the nearest frame's own pixel (t < 0.5: A, else B; the
//     de-judder's "never a blend"), blend = (1 - t) A + t B (smooth motion's
//     frame-mix parity); but where B's own block there is new and a trusted
//     neighbouring block moves away from the position (disocclusion: uncovered
//     between A and B; at least 2 source pixels at this phase), B's pixel at any
//     phase - only B has it. New in B with nothing moving away is a bad match:
//     the fallback stays;
//   - a cut (the frame trust below min_trusted_share) holds the nearest frame
//     whole, whatever the fallback.
// The output planes have the input planes' formats and sizes, so everything
// downstream (re-base, debanding, scaling, tone mapping, VP's dither) runs on
// them exactly as on a source frame: HDR (PQ / HLG, 10 to 16 bits) keeps its
// precision; no 8-bit step. Values are never clamped (BTB / WTW kept).
//
// Film grain (owner 2026-10-17: never averaged or interpolated away; SetGrain):
// preserve blends only the low band (Gaussian sigma 3, held at half resolution) of the warped A / B by
// phase and takes the high band from the nearer frame (A where the occlusion
// forces the A sample alone), sampled with Catmull-Rom and energy-matched per
// 16x16 block to that frame's own high band at the same pixels (gain 1..2; exactly 1
// at a whole-pixel position): one frame's grain at
// full strength at every phase, and no more frame-to-frame correlation than a
// hold. blend is the whole-signal blend above, byte for byte (the object's
// default, so a caller that never sets it gets the established output; the
// renderer sets it from interpolation_grain, default preserve).
namespace Interpolator
{
	struct Motion
	{
		// A->B block field (MotionWarp::Field() or its snapshot). nullptr: no valid vectors (an
		// invalidation, no estimate): every position takes the fallback (hold the nearest frame,
		// or blend), the frame-trust test passed.
		pl_tex field = nullptr;
		pl_tex frameTrust = nullptr;   // 1x1, r = the share of trusted blocks (MotionWarp::FrameTrust())
		pl_tex subField = nullptr;     // optional: 11a E2's split sub-field (MotionWarp::SubField())
		pl_tex occlusionB = nullptr;   // optional: B's occlusion mask (MotionWarp::Occlusion(slot)), r = 1 seen in A
		float minTrustedShare = 0.25f; // below it the frame is a cut
	};

	class FrameInterpolator
	{
	public:
		// hostReadable: the output planes can be downloaded (tests only).
		FrameInterpolator(pl_log log, pl_gpu gpu, bool hostReadable = false);
		~FrameInterpolator();
		FrameInterpolator(const FrameInterpolator&) = delete;
		FrameInterpolator& operator=(const FrameInterpolator&) = delete;

		// The picture at phase t between A and B. `out` is A's or B's planes at
		// t <= 0 / t >= 1, else this object's planes (valid until the next Run).
		// A and B must share sizes and formats; the plane formats must be storable.
		bool Run(const Dejudder::Planes& a, const Dejudder::Planes& b, const Motion& motion, float t,
			InterpolationPolicy::Fallback fallback, Dejudder::Planes& out);

		// interpolation_grain (InterpolationPolicy::Grain); takes effect on the next Run.
		void SetGrain(InterpolationPolicy::Grain value) { grain = value; }
		InterpolationPolicy::Grain GetGrain() const { return grain; }

		const std::string& LastError() const { return error; }
		unsigned long long Runs() const { return runs; }               // dispatched pictures
		unsigned long long GrainPreservedRuns() const { return grainPreservedRuns; }
		unsigned long long Passthroughs() const { return passthroughs; } // t at 0 or 1
		void Release();

	private:
		// interpolation_grain preserve, per plane: the low band of A and B at half resolution
		// (32-bit float); half = the 2x2 average, tmp = the across pass.
		struct GrainBands
		{
			pl_tex half = nullptr, tmp = nullptr, lowA = nullptr, lowB = nullptr;
		};
		bool Ensure(pl_tex& tex, pl_tex like);
		bool EnsureFloat(pl_tex& tex, int w, int h, int components);
		bool LowBand(const char* description, pl_tex src, pl_tex half, pl_tex tmp, pl_tex low);
		bool PrepareBands(int index, pl_tex srcA, pl_tex srcB);
		bool Plane(const char* description, pl_tex srcA, pl_tex srcB, pl_tex dst, const Motion& motion, float t,
			const float planeScale[2], const std::string& header, const GrainBands* bands);
		InterpolationPolicy::Grain grain = InterpolationPolicy::Grain::Blend;
		GrainBands grainBands[2];   // luma, chroma
		unsigned long long grainPreservedRuns = 0;
		pl_log log = nullptr;
		pl_gpu gpu = nullptr;
		pl_dispatch dispatch = nullptr;
		bool hostReadable = false;
		pl_tex outLuma = nullptr, outChroma = nullptr;
		pl_tex noField = nullptr, noTrust = nullptr;   // the no-vector fallback's 1x1 field and trust
		bool EnsureNoVectors();
		std::string error;
		unsigned long long runs = 0, passthroughs = 0;
	};
}
