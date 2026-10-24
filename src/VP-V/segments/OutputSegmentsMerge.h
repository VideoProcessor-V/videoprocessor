#pragma once

#include "OutputSegments.h"

#include <vprenderer/HookSegment.h>

#include <cstdint>
#include <vector>

// DRAFT (2026-10-24, not integrated; commit 2 of the two-commit rule): the merged form of the
// output segments, behind output_segments_merged (default OFF).
//
// Each run of adjacent S2 hooks at PRE_OUTPUT and of S3 hooks at OUTPUT becomes ONE libplacebo
// hook that hands each stage's output texture straight to the next (vprenderer/HookSegment.h):
// with the designed chain and no user PRE_OUTPUT shader that is one PRE_OUTPUT hook (tone shape,
// DV trims, LED measurement, LED compensation, calibration LUT) and one OUTPUT hook (encoding
// repair, occlusion mask, sigma-delta last), against up to ~8 today.
//
// The merge is applied ONLY to the parameters handed to pl_render_image, inside a Scope that
// restores the unmerged list afterwards: every host rule that finds hooks by pointer (the
// de-judder extras' filter, the repair re-check, the sigma-delta vectors guard) keeps working on
// the unmerged list. The extras keep the separate form for now (follow-up: a Scope there too).
//
// Tolerance gate (central-migration-needs.md §6.3): at most 1 code at the output bit depth,
// BTB/WTW probes unchanged, sigma-delta statistics unchanged, owner acceptance. Merging changes
// pass boundaries only (the intermediates use libplacebo's FBO format), so 0 is expected.
namespace OutputSegments
{
	class Merger
	{
	public:
		Merger(pl_gpu gpu, OutputColourSegment& s2, OutputFinishSegment& s3);

		// On a settings change (`signature` covers the module order, the switch and fbo_float32):
		// checks both segments (SegmentRunner::Segment::Configure). True = a segment runs merged.
		bool Configure(const PipelineSlots::Registry& registry, bool mergedWanted, bool float32, uint32_t sampleEvery,
			uint64_t signature, FallbackStatus::Registry& fallbacks);
		bool Active() const { return active; }

		// Swaps the render parameters' hooks for the merged list for one pl_render_image call.
		class Scope
		{
		public:
			// A null merger (none created yet) changes nothing.
			Scope(Merger* merger, pl_render_params& params, const pl_render_errors* errors);
			~Scope();
			Scope(const Scope&) = delete;
			Scope& operator=(const Scope&) = delete;

		private:
			pl_render_params& params;
			const pl_hook* const* hooks;
			int count;
		};

		// After the render: an inner stage that failed is reported and its segment drops to the
		// separate form until the next settings change ("Fallbacks engaged", never log-only).
		// Timer results that arrived are recorded per slot in each segment's TimingSampler (S3's
		// repair and mask have no slot of their own: recorded under output.dither).
		void Report(FallbackStatus::Registry& fallbacks);
		const SlotTiming::Accumulator& S2Timing() const { return s2.Segment().Timing().Accumulated(); }
		const SlotTiming::Accumulator& S3Timing() const { return s3.Segment().Timing().Accumulated(); }
		// Last polled GPU time of a stage hook, ns (0 = none; SIG_TEX stages time inside the hook).
		uint64_t LastNs(const pl_hook* hook) const { return runner.LastNs(hook); }
		void Release() { runner.Release(); }

	private:
		bool Member(const pl_hook* hook) const;
		OutputColourSegment& s2;
		OutputFinishSegment& s3;
		HookSegment::Runner runner;
		std::vector<const pl_hook*> in, merged;
		std::vector<std::pair<uint64_t, bool>> lastMembers;   // signature, true = S2
		bool active = false;
	};
}
