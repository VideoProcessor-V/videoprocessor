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
// repair, occlusion mask, sigma-delta last).
//
// Status (audit 2026-10-25, M2): every S2/S3 stage takes and returns a texture, and libplacebo
// already chains such hooks without extra passes, so this form saves NO GPU pass today and costs a
// second set of intermediates. It stays OFF until stages are actually fused into fewer shaders;
// do not spend the tolerance gate or owner time on it before that, and measure before claiming.
//
// The merge is applied ONLY to the parameters handed to pl_render_image, inside a Scope that
// restores the unmerged list afterwards: every host rule that finds hooks by pointer (the
// de-judder extras' filter, the repair re-check, the sigma-delta vectors guard) keeps working on
// the unmerged list. The main draw and every de-judder extra use a Scope (8b4806a0).
//
// Failures are visible and take the host's own paths (audit H2, M4):
// - a stage failing inside a merged hook leaves the merge at once (it runs separately at its own
//   place, HookSegment), its segment drops to the separate form until the next settings change
//   ("Fallbacks engaged: output segment S2 (separate hooks: <stage>: <reason>)"), and Report()
//   returns its signature so the host treats it like a hook libplacebo disabled;
// - libplacebo disabling a merged hook downgrades its segment the same way (checked at every
//   Scope and Report, so the remaining extras of a refresh fall back too).
//
// Tolerance gate (central-migration-needs.md §6.3) before any default change: at most 1 code at the
// output bit depth, BTB/WTW probes unchanged, sigma-delta statistics unchanged, owner acceptance.
namespace OutputSegments
{
	class Merger
	{
	public:
		Merger(pl_gpu gpu, OutputColourSegment& s2, OutputFinishSegment& s3);

		// Every frame (cheap: a signature over the module order and switches; the checks rerun only
		// when it changes). True = a segment runs merged. `fallbacks` is kept for Scope/Report.
		bool Configure(const PipelineSlots::Registry& registry, bool mergedWanted, uint32_t sampleEvery,
			FallbackStatus::Registry& fallbacks);
		bool Active() const { return active; }
		// True when the last Configure / Scope / Report changed the "Fallbacks engaged" heading
		// (the host logs the heading through its own path). Cleared by Configure.
		bool FallbacksChanged() const { return fallbacksChanged; }

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

		// After the main render: downgrades segments whose stages failed (or whose merged hook
		// libplacebo disabled), records arrived timer results per slot (S3's repair and mask have
		// no slot of their own: recorded under output.dither), and returns the signatures of the
		// stages that failed inside a merged hook, for the host's disabled-hook handling.
		std::vector<uint64_t> Report(const pl_render_errors* errors);
		const SlotTiming::Accumulator& S2Timing() const { return s2.Segment().Timing().Accumulated(); }
		const SlotTiming::Accumulator& S3Timing() const { return s3.Segment().Timing().Accumulated(); }
		uint64_t LastNs(const pl_hook* hook) const { return runner.LastNs(hook); }
		HookSegment::Runner& Runner() { return runner; }   // tests and diagnostics
		void Release() { runner.Release(); }

	private:
		bool Member(const pl_hook* hook) const;
		void CheckDisabled(const pl_render_errors* errors);
		std::string Describe(const pl_hook* hook) const;
		SegmentRunner::Segment& Owner(const pl_hook* hook);
		void Downgrade(SegmentRunner::Segment& segment, const std::string& reason);

		OutputColourSegment& s2;
		OutputFinishSegment& s3;
		HookSegment::Runner runner;
		FallbackStatus::Registry* fallbacks = nullptr;
		std::vector<const pl_hook*> in, merged;
		bool active = false;
		bool fallbacksChanged = false;
	};
}
