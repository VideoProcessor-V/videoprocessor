#pragma once

#include <VP-V/core/FallbackStatus.h>
#include <VP-V/core/PipelineSlots.h>
#include <VP-V/core/SegmentRunner.h>
#include <VP-V/core/SlotRegistry.h>

#include <libplacebo/renderer.h>
#include <libplacebo/shaders/custom.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

namespace CustomHookChain { class Chain; }
namespace LedDrive { class MeasurementHook; }
namespace CalibrationLutStage { class Hook; }
namespace OutputEncodingRepair { class Hook; }
namespace OcclusionMask { class Hook; }
namespace SigmaDeltaDomain { struct Domain; }
namespace HookSegment { class Runner; }

// DRAFT (2026-10-24, not integrated): the OUTPUT-CHAIN segments S2 and S3 as stage adapters
// around the existing modules (central-migration-needs.md §6; core/SegmentRunner.h).
//
//   S2 OutputColourSegment (PRE_OUTPUT): SDR tone shape -> DV trims -> [Stage 13: placeholder]
//      -> LED measurement -> LED compensation (gamma boost / adaptive gamma) -> [black level:
//      placeholder] -> | user PRE_OUTPUT shaders (an edge) | -> calibration LUT (VP stage)
//   S3 OutputFinishSegment (OUTPUT): encoding repair -> occlusion mask -> dither (sigma-delta) LAST
//
// The calibration LUT stays the LAST stage of S2 (not the first of S3): it must act on target
// light before libplacebo's output encoding (Beta 377 encodes between PRE_OUTPUT and OUTPUT),
// and the de-judder rendered-picture cache, a PRE_OUTPUT hook after it, keeps the LUT-mapped
// picture for the extras. S3 can take it only once VP owns the encoding (§6.2).
//
// Separate form (commit 1, the default): the adapters append EXACTLY today's hooks with today's
// parameters in today's order; nothing about a pass changes, so the output is byte-identical.
// The maths stays in the modules (the GLSL files, LedMeasurementHook, CalibrationLutStage,
// OutputEncodingRepair, OcclusionMaskHook); the adapters only own which hooks run, in which
// order, with which parameters.
//
// Merged form (commit 2, output_segments_merged = on, default off): Fuse() replaces each run of
// adjacent segment hooks at one stage with one libplacebo hook (vprenderer/HookSegment.h) that
// hands each stage's output texture straight to the next. Gated by a tolerance test.
namespace OutputSegments
{
	// ---- S2 -------------------------------------------------------------------------------
	struct ToneShapeParams { double headroom = 0, kneeStart = 0, strength = 0, eotf = 2.2, recovery = 0; };
	struct DvTrimParams
	{
		double slope = 1, offset = 0, power = 1, chromaWeight = 0, saturationGain = 1;
		double signalPq = 0, gamma = 2.2, peakNits = 100, kr = 0.2126, kb = 0.0722;
	};
	struct CompensationParams
	{
		double gain = 1, knee = 0.08, gainDomain = 0, eotf = 2.2, boost = 1, boostKnee = 0, boostKneeWidth = 0.05;
		double shadowLift = 0, veil = 0, wtwPassthrough = 1, btbPassthrough = 1;
	};

	// What the host decided for this frame (the decisions, logging and fallbacks stay in the host
	// until the b3-b7 extractions move them into the stages).
	struct ColourFrame
	{
		bool toneShape = false;
		ToneShapeParams tone;
		bool dvTrims = false;
		DvTrimParams dv;
		bool ledMeasure = false;        // led && pipelineSlots.Runs(DriveLedMeasure)
		bool ledBlock = false;          // led || boost != 1 || shadow lift > 0 (the compensation block)
		bool ledCompensation = false;   // pipelineSlots.Runs(DriveLedCompensation)
		CompensationParams compensation;
		const std::vector<const pl_hook*>* userPreOutput = nullptr;   // the edge (customHooks.Collect)
		bool calibrationLut = false;    // calibrationLutStage.Lut() != nullptr
	};

	// How many hooks each stage contributed (the host's "NOT applied" states read these).
	struct ColourCounts { size_t toneShape = 0, dvTrims = 0, ledMeasure = 0, ledCompensation = 0, user = 0, lut = 0; };

	using TagFn = std::function<void(size_t start, PipelineSlots::Id slot)>;

	class OutputColourSegment
	{
	public:
		OutputColourSegment(CustomHookChain::Chain& toneShape, CustomHookChain::Chain& dvTrims,
			LedDrive::MeasurementHook& measure, CustomHookChain::Chain& compensation, CalibrationLutStage::Hook& lut);

		// Separate form: appends today's S2 hooks to `hooks` (tagging slot hooks through `tag`, the
		// host's TagSlotHooks) and records them as this frame's S2 members.
		ColourCounts Collect(const ColourFrame& frame, std::vector<const pl_hook*>& hooks, const TagFn& tag);
		bool Member(const pl_hook* hook) const;
		SegmentRunner::Segment& Segment() { return segment; }

	private:
		CustomHookChain::Chain& toneShape;
		CustomHookChain::Chain& dvTrims;
		LedDrive::MeasurementHook& measure;
		CustomHookChain::Chain& compensation;
		CalibrationLutStage::Hook& lut;
		std::vector<const pl_hook*> members;
		SegmentRunner::Segment segment;
	};

	// ---- S3 -------------------------------------------------------------------------------
	// The OUTPUT end. In the separate form it is four steps at today's four places, because
	// host hooks (NLS, the ExtendedRange restores, the de-judder cache) are placed between them;
	// the merged form is the one exit.
	class OutputFinishSegment
	{
	public:
		OutputFinishSegment(OutputEncodingRepair::Hook& repair, OcclusionMask::Hook& mask);

		void BeginFrame() { members.clear(); }
		// 1. The dither's code domain (today 24539-24567); an unsupported domain removes it.
		//    Returns true when a dither hook was found.
		bool PrepareDither(std::vector<const pl_hook*>& hooks, const SigmaDeltaDomain::Domain& domain, bool outputFloat32);
		// 2. The dither runs last at OUTPUT (24572-24593). Returns true when it was moved.
		bool KeepDitherLast(std::vector<const pl_hook*>& hooks, bool levelsFull);
		// 3. The mask parameters (24637-24644): returns the dither's index (hooks.size() = none).
		size_t MarkMask(std::vector<const pl_hook*>& hooks, bool install, bool grainInstall, double grainScale);
		//    and the mask hook just before the dither (24671).
		void InsertMask(std::vector<const pl_hook*>& hooks, size_t ditherIndex);
		// 4. The libplacebo 7.360 encoding repair, first at OUTPUT (24786-24799). True = inserted.
		bool Repair(std::vector<const pl_hook*>& hooks, const pl_render_errors* errors, enum pl_lut_type targetLut,
			const pl_color_repr& targetRepr);
		bool Member(const pl_hook* hook) const;
		SegmentRunner::Segment& Segment() { return segment; }

	private:
		void Note(const pl_hook* hook);
		OutputEncodingRepair::Hook& repair;
		OcclusionMask::Hook& mask;
		std::vector<const pl_hook*> members;
		SegmentRunner::Segment segment;
	};
}
