#pragma once

#include <libplacebo/dispatch.h>
#include <libplacebo/gpu.h>
#include <libplacebo/log.h>

#include <VP-V/modules/motion/estimation/matcher/DejudderWarp.h>
#include <VP-V/modules/motion/consumers/InterpolationPolicy.h>

#include <cstdint>
#include <string>

namespace VpSkeleton { class SkeletonMemo; }

// VP's HDR-safe optical-flow frame interpolator (docs/VP-interpolator-50-60-plan.md step 3;
// docs/VP-nvof-ahead.md). Given two consecutive source frames A and B (Dejudder::Planes),
// the A->B block field (MotionWarp::Field()), and phase t in [0, 1], writes the picture at t:
// t <= 0 is A, t >= 1 is B; trusted positions warp bidirectionally (A alone on B occlusion);
// untrusted positions hold/blend (or B on disocclusion); cuts hold nearest frame. HDR-safe
// (keeps input plane formats/sizes, BTB/WTW untouched).
// Film grain (SetGrain, default Preserve): blends only the low band (sigma 3 at half-res,
// acquired via kBandMemo when available, visible fallback to BandMemo::GlslOf) and takes the
// energy-matched Catmull-Rom high band from the nearer frame. Blend = whole-signal blend.
namespace Interpolator
{
	struct Motion
	{
		pl_tex field = nullptr;        // A->B block field (nullptr = no valid vectors: fallback)
		pl_tex frameTrust = nullptr;   // 1x1, r = share of trusted blocks
		pl_tex subField = nullptr;     // optional: split sub-field
		pl_tex occlusionB = nullptr;   // optional: B's occlusion mask, r = 1 seen in A
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

		// Picture at phase t between A and B. `out` is A or B at t <= 0 / t >= 1, else internal planes.
		// estimateWidth/Height > 0 for processed planes at another size than the field's estimate frame.
		// memo / seqA / seqB (optional): acquire shared kBandMemo GPU low bands for luma before fallback.
		bool Run(const Dejudder::Planes& a, const Dejudder::Planes& b, const Motion& motion, float t,
			InterpolationPolicy::Fallback fallback, Dejudder::Planes& out, int estimateWidth = 0, int estimateHeight = 0,
			VpSkeleton::SkeletonMemo* memo = nullptr, uint64_t seqA = 0, uint64_t seqB = 0);

		void SetGrain(InterpolationPolicy::Grain value) { grain = value; }
		InterpolationPolicy::Grain GetGrain() const { return grain; }

		const std::string& LastError() const { return error; }
		unsigned long long Runs() const { return runs; }
		unsigned long long GrainPreservedRuns() const { return grainPreservedRuns; }
		unsigned long long Passthroughs() const { return passthroughs; }
		unsigned long long BandMemoHits() const;
		unsigned long long BandMemoFallbacks() const;
		const std::string& LastBandMemoFallback() const;
		void Release();

	private:
		struct GrainBands { pl_tex half = nullptr, tmp = nullptr, lowA = nullptr, lowB = nullptr; };
		bool Ensure(pl_tex& tex, pl_tex like);
		bool EnsureFloat(pl_tex& tex, int w, int h, int components);
		bool LowBand(const char* description, pl_tex src, pl_tex half, pl_tex tmp, pl_tex low);
		bool PrepareFallbackLow(int index, bool isB, pl_tex src, pl_tex& lowOut);
		bool Plane(const char* description, pl_tex srcA, pl_tex srcB, pl_tex dst, const Motion& motion, float t,
			const float planeScale[2], const std::string& header, pl_tex lowA, pl_tex lowB);
		InterpolationPolicy::Grain grain = InterpolationPolicy::Grain::Preserve;
		GrainBands grainBands[2];   // luma, chroma
		unsigned long long grainPreservedRuns = 0, bandMemoHits = 0, bandMemoFallbacks = 0;
		pl_log log = nullptr;
		pl_gpu gpu = nullptr;
		pl_dispatch dispatch = nullptr;
		bool hostReadable = false;
		pl_tex outLuma = nullptr, outChroma = nullptr, noField = nullptr, noTrust = nullptr;
		uint64_t vramDeclared = ~uint64_t(0);
		bool EnsureNoVectors();
		std::string error, lastBandMemoFallback;
		unsigned long long runs = 0, passthroughs = 0;
	};
}
