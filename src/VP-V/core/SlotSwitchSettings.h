#pragma once

#include "OriginalPathway.h"
#include "PipelineSlots.h"
#include "SlotContracts.h"

#include <array>
#include <cstring>
#include <string>

// Slot-level switches for the motion and queue/timing slots (owner 2026-10-25: these slots stay
// modular, switchable via pipeline_slots_off, with the Original pathway = Bill's queue policy).
//
// A slot switched off runs in its UPSTREAM form: exactly the Original pathway overrides that
// core/OriginalPathway.h lists for that slot (no new meaning per slot, one list). The order rules
// of SlotContracts decide the knock-on: a slot that needs vectors (motion.consumers) cannot run
// once their giver (motion.estimation) or their delivery (motion.vector_delivery) is off, so it is
// switched off with them and its overrides applied too. motion.vector_delivery has no settings of
// its own; switching it off is honoured through that knock-on.
//
//   input.queue_timing  off -> Bill's queue policy (QueueTimingPolicy::BillQueuePolicy, chosen by
//                              the host through UsesUpstreamQueuePolicy) + the slot's *_fixes off
//   output.presentation off -> render_ahead off (upstream presentation)
//   motion.estimation   off -> optical flow / look-ahead / history fixes off, and the consumers
//   motion.vector_delivery  -> the consumers
//   motion.consumers    off -> de-judder strength 0, interpolation off (hold), fill gaps, mask off
//
// THE RULE for these slots (audit 2026-10-25 item 4): off = the slot's upstream form (its Original
// pathway overrides) AND no pass of its own. For estimation / vector delivery the renderer also
// gates the one estimate user the overrides do not reach - scaling.output's grain map
// (MotionVectorsRun() in the host) - so no estimate pass runs at all (zero cost); every vector
// reader (scaling.output residual motion + grain field, texture restore, VP deband, output.dither's
// VPVEC) then sees no field and takes its no-vector form. output.dither's own motion search is that
// slot's pass, not motion.estimation's.
//
// The template takes the renderer's RendererSettings (fields named as in the host's
// ApplyOriginalPathwaySettings); CPU tests use a stand-in with the same fields. The key -> field
// table below is checked against OriginalPathway::Overrides() by the tests, so the two lists
// cannot drift apart. Header-only, no I/O, no locks.
namespace SlotSwitchSettings
{
	using PipelineSlots::Id;

	// The slots this header switches (their switches are honoured: PipelineSlots::SwitchHonoured).
	inline const std::array<Id, 5>& Switched()
	{
		static const std::array<Id, 5> ids = { { Id::InputQueueTiming, Id::MotionEstimation, Id::MotionVectorDelivery,
			Id::MotionConsumers, Id::OutputPresentation } };
		return ids;
	}

	inline bool IsSwitched(Id id)
	{
		for (Id s : Switched())
			if (s == id)
				return true;
		return false;
	}

	// The effective off set after the order rules (needs-vectors knock-on).
	struct OffSet
	{
		bool queueTiming = false, estimation = false, vectorDelivery = false, consumers = false, presentation = false;
		bool consumersByKnockOn = false;   // consumers off only because their vectors are off (for status)
		bool Any() const { return queueTiming || estimation || vectorDelivery || consumers || presentation; }
	};

	inline OffSet Resolve(bool queueTiming, bool estimation, bool vectorDelivery, bool consumers, bool presentation)
	{
		OffSet s;
		s.queueTiming = queueTiming;
		s.estimation = estimation;
		s.vectorDelivery = vectorDelivery;
		s.presentation = presentation;
		const bool vectorsOff = estimation || vectorDelivery;
		const bool consumersNeedVectors = PipelineSlots::ContractOf(Id::MotionConsumers).needsVectors;
		s.consumers = consumers || (vectorsOff && consumersNeedVectors);
		s.consumersByKnockOn = !consumers && s.consumers;
		return s;
	}

	// pipeline_slots_off text (names separated by ',', ' ', ';' or tabs, as the renderer parses it).
	inline OffSet FromText(const std::string& text)
	{
		bool off[PipelineSlots::kCount] = {};
		std::string token;
		auto flush = [&]()
		{
			if (token.empty())
				return;
			const Id id = PipelineSlots::FromName(token.c_str());
			if (id != Id::Count)
				off[static_cast<size_t>(id)] = true;
			token.clear();
		};
		for (char ch : text)
		{
			if (ch == ',' || ch == ' ' || ch == ';' || ch == '\t')
				flush();
			else
				token += ch;
		}
		flush();
		auto o = [&](Id id) { return off[static_cast<size_t>(id)]; };
		return Resolve(o(Id::InputQueueTiming), o(Id::MotionEstimation), o(Id::MotionVectorDelivery), o(Id::MotionConsumers),
			o(Id::OutputPresentation));
	}

	// The queue policy the host installs: Bill's whenever the slot runs in its upstream form
	// (switched off, or the Original pathway: ADR-0019, owner 2026-10-25 "Original pathway = Bill's
	// queue policy").
	inline bool UsesUpstreamQueuePolicy(const OffSet& off, bool originalPathway)
	{
		return off.queueTiming || originalPathway;
	}

	// The keys each switched slot sets, as OriginalPathway.h lists them (checked by the tests).
	inline const char* const* KeysFor(Id id, size_t& count)
	{
		static const char* const queue[] = { "queue_timing_fixes", "pulldown_fixes", "timing_continuity_fixes",
			"cadence_class_fixes", "general_rate_fixes" };
		static const char* const estimation[] = { "optical_flow", "motion_ahead_frames", "motion_history_fixes",
			"dejudder_estimate_ahead", "dejudder_est_still_zero" };
		static const char* const consumers[] = { "dejudder_strength", "dejudder_strength_even", "interpolation",
			"interpolation_fill_gaps", "occlusion_mask", "dejudder_source_warp_filter", "interpolation_fallback" };
		static const char* const presentation[] = { "render_ahead", "repeat_render_reuse" };
		switch (id)
		{
		case Id::InputQueueTiming: count = sizeof(queue) / sizeof(queue[0]); return queue;
		case Id::MotionEstimation: count = sizeof(estimation) / sizeof(estimation[0]); return estimation;
		case Id::MotionConsumers: count = sizeof(consumers) / sizeof(consumers[0]); return consumers;
		case Id::OutputPresentation: count = sizeof(presentation) / sizeof(presentation[0]); return presentation;
		default: count = 0; return nullptr;
		}
	}

	// Applies the upstream form of every slot in `off` to the renderer settings. Settings must
	// expose the fields below (RendererSettings in the renderer host).
	template <class Settings>
	void Apply(const OffSet& off, Settings& settings)
	{
		if (off.queueTiming)
		{
			settings.queueTimingFixes = false;            // queue_timing_fixes
			settings.pulldownFixes = false;               // pulldown_fixes
			settings.timingContinuityFixes = false;       // timing_continuity_fixes
			settings.cadenceClassFixes = false;           // cadence_class_fixes
			settings.generalRateFixes = false;            // general_rate_fixes
		}
		if (off.estimation)
		{
			settings.opticalFlow = false;                 // optical_flow
			settings.motionAheadFrames = 0;               // motion_ahead_frames
			settings.motionHistoryFixes = false;          // motion_history_fixes
			settings.dejudderEstimateAhead = false;       // dejudder_estimate_ahead
			settings.dejudderEstStillZero = false;        // dejudder_est_still_zero (group E, bug hunt 2026-10-25)
		}
		if (off.consumers)
		{
			using Mode = decltype(settings.interpolation);
			using Fallback = decltype(settings.interpolationFallback);
			settings.dejudderStrength = 0.0;              // dejudder_strength
			settings.dejudderStrengthEven = 0.0;          // dejudder_strength_even
			settings.interpolation = Mode::Off;           // interpolation
			settings.interpolationFillGaps = false;       // interpolation_fill_gaps
			settings.occlusionMask = false;               // occlusion_mask
			settings.dejudderSourceWarpCatmull = false;   // dejudder_source_warp_filter
			settings.interpolationFallback = Fallback::Hold;   // interpolation_fallback
		}
		if (off.presentation)
		{
			settings.renderAhead = false;                 // render_ahead
			settings.repeatRenderReuse = false;           // repeat_render_reuse (WP-24, group B)
		}
	}

	// One status line (Ctrl+I / log): which of these slots are off and what runs instead. Empty
	// when none is off. A slot switched off is the user's choice, not a fallback; the knock-on is
	// named so it is never silent.
	inline std::string Describe(const OffSet& off, bool originalPathway)
	{
		std::string text;
		auto add = [&](const char* part) { if (!text.empty()) text += "; "; text += part; };
		if (UsesUpstreamQueuePolicy(off, originalPathway))
			add(originalPathway && !off.queueTiming ? "queue policy: upstream (Original pathway)" : "queue policy: upstream (input.queue_timing off)");
		if (off.estimation) add("motion.estimation off");
		if (off.vectorDelivery) add("motion.vector_delivery off");
		if (off.consumers) add(off.consumersByKnockOn ? "motion.consumers off (no vectors)" : "motion.consumers off");
		if (off.presentation) add("output.presentation off (render-ahead off)");
		return text;
	}
}
