// Built without the test project's precompiled header: keep windows.h from defining min/max.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "CppUnitTest.h"

// Slot switches for the motion and queue/timing slots (owner 2026-10-25): a slot switched off runs
// in its upstream form (the Original pathway's overrides for that slot), consumers follow their
// vectors (SlotContracts), and the Original pathway / input.queue_timing off select Bill's queue
// policy. CPU only.

#include <VP-V/core/OriginalPathway.h>
#include <VP-V/core/SlotSwitchSettings.h>
#include <VP-V/modules/input/queue_timing/VpFixedBudgetPolicy.h>
#include <VP-V/modules/motion/consumers/InterpolationPolicy.h>
#include <vprenderer/QueueTimingPolicy.h>

#include <set>
#include <string>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace
{
	// The renderer settings' fields the switches touch, with VP's defaults (everything on).
	struct FakeSettings
	{
		bool queueTimingFixes = true, pulldownFixes = true, timingContinuityFixes = true, cadenceClassFixes = true,
			generalRateFixes = true;
		bool opticalFlow = true;
		int motionAheadFrames = 4;
		bool motionHistoryFixes = true, vectorDemand = true, dejudderEstimateAhead = true, dejudderEstStillZero = true;
		double dejudderStrength = 1.0, dejudderStrengthEven = 0.5;
		InterpolationPolicy::Mode interpolation = InterpolationPolicy::Mode::Auto;
		bool interpolationFillGaps = true, occlusionMask = true, dejudderSourceWarpCatmull = true;
		InterpolationPolicy::Fallback interpolationFallback = InterpolationPolicy::Fallback::Blend;
		bool renderAhead = true, repeatRenderReuse = true;
		bool unrelated = true;   // a field no switch may touch

		bool QueueOn() const { return queueTimingFixes && pulldownFixes && timingContinuityFixes && cadenceClassFixes && generalRateFixes; }
		bool QueueOff() const { return !queueTimingFixes && !pulldownFixes && !timingContinuityFixes && !cadenceClassFixes && !generalRateFixes; }
		bool EstimationOn() const { return opticalFlow && motionAheadFrames == 4 && motionHistoryFixes && vectorDemand && dejudderEstimateAhead && dejudderEstStillZero; }
		bool EstimationOff() const { return !opticalFlow && motionAheadFrames == 0 && !motionHistoryFixes && !vectorDemand && !dejudderEstimateAhead && !dejudderEstStillZero; }
		bool ConsumersOn() const
		{
			return dejudderStrength == 1.0 && dejudderStrengthEven == 0.5 && interpolation == InterpolationPolicy::Mode::Auto &&
				interpolationFillGaps && occlusionMask && dejudderSourceWarpCatmull &&
				interpolationFallback == InterpolationPolicy::Fallback::Blend;
		}
		bool ConsumersOff() const
		{
			return dejudderStrength == 0.0 && dejudderStrengthEven == 0.0 && interpolation == InterpolationPolicy::Mode::Off &&
				!interpolationFillGaps && !occlusionMask && !dejudderSourceWarpCatmull &&
				interpolationFallback == InterpolationPolicy::Fallback::Hold;
		}
	};

	std::wstring W(const std::string& s) { return std::wstring(s.begin(), s.end()); }
}

namespace VpvTests
{
	TEST_CLASS(SlotSwitchSettings)
	{
	public:
		TEST_METHOD(KeysAreTheOriginalPathwayOverridesOfEachSlot)
		{
			// One list: a switched-off slot sets exactly what the Original pathway sets for it.
			for (PipelineSlots::Id id : ::SlotSwitchSettings::Switched())
			{
				std::set<std::string> want, got;
				for (const OriginalPathway::Override& o : OriginalPathway::Overrides())
					if (PipelineSlots::FromName(o.slot) == id)
						want.insert(o.key);
				size_t n = 0;
				const char* const* keys = ::SlotSwitchSettings::KeysFor(id, n);
				for (size_t i = 0; i < n; ++i)
					got.insert(keys[i]);
				Assert::IsTrue(want == got, W(std::string("keys differ for ") + PipelineSlots::Get(id).name).c_str());
			}
		}

		TEST_METHOD(ValuesAreTheOriginalPathwayValues)
		{
			// Audit 2026-10-25 item 3: not only the keys - every value Apply sets is the value the
			// Original pathway table names for that key (read with the same meaning the renderer's
			// parser gives it: on/off, numbers, bilinear = no Catmull-Rom, hold).
			FakeSettings s;
			::SlotSwitchSettings::Apply(::SlotSwitchSettings::Resolve(true, true, true, true, true), s);
			size_t checked = 0;
			for (const OriginalPathway::Override& o : OriginalPathway::Overrides())
			{
				const PipelineSlots::Id slot = PipelineSlots::FromName(o.slot);
				if (!::SlotSwitchSettings::IsSwitched(slot))
					continue;
				const std::string k = o.key, v = o.value;
				auto flag = [&](bool field) { Assert::IsTrue(v == "off" && !field, W(k + " = " + v).c_str()); };
				auto number = [&](double field) { Assert::AreEqual(std::stod(v), field, W(k).c_str()); };
				if (k == "queue_timing_fixes") flag(s.queueTimingFixes);
				else if (k == "pulldown_fixes") flag(s.pulldownFixes);
				else if (k == "timing_continuity_fixes") flag(s.timingContinuityFixes);
				else if (k == "cadence_class_fixes") flag(s.cadenceClassFixes);
				else if (k == "general_rate_fixes") flag(s.generalRateFixes);
				else if (k == "optical_flow") flag(s.opticalFlow);
				else if (k == "motion_ahead_frames") number(s.motionAheadFrames);
				else if (k == "motion_history_fixes") flag(s.motionHistoryFixes);
				else if (k == "vector_demand") flag(s.vectorDemand);
				else if (k == "dejudder_estimate_ahead") flag(s.dejudderEstimateAhead);
				else if (k == "dejudder_est_still_zero") flag(s.dejudderEstStillZero);
				else if (k == "dejudder_strength") number(s.dejudderStrength);
				else if (k == "dejudder_strength_even") number(s.dejudderStrengthEven);
				else if (k == "interpolation")
					Assert::IsTrue(InterpolationPolicy::ParseMode(v) == s.interpolation, W(k).c_str());
				else if (k == "interpolation_fill_gaps") flag(s.interpolationFillGaps);
				else if (k == "occlusion_mask") flag(s.occlusionMask);
				else if (k == "dejudder_source_warp_filter")
					Assert::IsTrue(v == "bilinear" && !s.dejudderSourceWarpCatmull, W(k + " = " + v).c_str());
				else if (k == "interpolation_fallback")
					Assert::IsTrue(v == "hold" && s.interpolationFallback == InterpolationPolicy::Fallback::Hold, W(k).c_str());
				else if (k == "render_ahead") flag(s.renderAhead);
				else if (k == "repeat_render_reuse") flag(s.repeatRenderReuse);
				else
					Assert::Fail(W("no value check for " + k).c_str());
				++checked;
			}
			Assert::AreEqual(size_t{ 20 }, checked);
		}

		TEST_METHOD(NothingOffChangesNothing)
		{
			FakeSettings s;
			::SlotSwitchSettings::Apply(::SlotSwitchSettings::FromText(""), s);
			Assert::IsTrue(s.QueueOn() && s.EstimationOn() && s.ConsumersOn() && s.renderAhead && s.unrelated);
			// Slots outside this header (and unknown names) are not its business.
			::SlotSwitchSettings::Apply(::SlotSwitchSettings::FromText("repair.deband, colour.appearance, nonsense"), s);
			Assert::IsTrue(s.QueueOn() && s.EstimationOn() && s.ConsumersOn() && s.renderAhead && s.unrelated);
			Assert::IsTrue(::SlotSwitchSettings::Describe(::SlotSwitchSettings::FromText(""), false).empty());
		}

		TEST_METHOD(EachSlotOffIsItsUpstreamFormOnly)
		{
			{
				FakeSettings s;
				::SlotSwitchSettings::Apply(::SlotSwitchSettings::FromText("input.queue_timing"), s);
				Assert::IsTrue(s.QueueOff() && s.EstimationOn() && s.ConsumersOn() && s.renderAhead && s.unrelated);
			}
			{
				FakeSettings s;
				::SlotSwitchSettings::Apply(::SlotSwitchSettings::FromText("output.presentation"), s);
				Assert::IsTrue(s.QueueOn() && s.EstimationOn() && s.ConsumersOn() && !s.renderAhead && s.unrelated);
			}
			{
				FakeSettings s;
				::SlotSwitchSettings::Apply(::SlotSwitchSettings::FromText("motion.consumers"), s);
				Assert::IsTrue(s.QueueOn() && s.EstimationOn() && s.ConsumersOff() && s.renderAhead && s.unrelated);
			}
		}

		TEST_METHOD(ConsumersFollowTheirVectors)
		{
			// SlotContracts: motion.consumers needs vectors; without their giver or their delivery
			// they cannot run, and the status names the knock-on (never silent).
			for (const char* off : { "motion.estimation", "motion.vector_delivery" })
			{
				const auto set = ::SlotSwitchSettings::FromText(off);
				Assert::IsTrue(set.consumers && set.consumersByKnockOn);
				FakeSettings s;
				::SlotSwitchSettings::Apply(set, s);
				Assert::IsTrue(s.ConsumersOff() && s.QueueOn() && s.renderAhead);
				Assert::AreEqual(std::string(off) == "motion.estimation", s.EstimationOff());
				Assert::IsTrue(::SlotSwitchSettings::Describe(set, false).find("motion.consumers off (no vectors)") != std::string::npos);
			}
			// Consumers off alone leave the estimation as configured (other vector users keep it).
			Assert::IsFalse(::SlotSwitchSettings::FromText("motion.consumers").estimation);
			Assert::IsFalse(::SlotSwitchSettings::FromText("motion.consumers").consumersByKnockOn);
		}

		TEST_METHOD(SeparatorsAsTheRendererParsesThem)
		{
			const auto set = ::SlotSwitchSettings::FromText("input.queue_timing;output.presentation\tmotion.consumers ,");
			Assert::IsTrue(set.queueTiming && set.presentation && set.consumers && !set.estimation && !set.vectorDelivery);
		}

		TEST_METHOD(OriginalPathwayAndQueueSlotOffSelectBillsPolicy)
		{
			// Owner 2026-10-25: Original pathway = Bill's queue policy; input.queue_timing off = the same.
			using ::SlotSwitchSettings::UsesUpstreamQueuePolicy;
			Assert::IsFalse(UsesUpstreamQueuePolicy(::SlotSwitchSettings::FromText(""), false));
			Assert::IsTrue(UsesUpstreamQueuePolicy(::SlotSwitchSettings::FromText(""), true));
			Assert::IsTrue(UsesUpstreamQueuePolicy(::SlotSwitchSettings::FromText("input.queue_timing"), false));
			Assert::IsFalse(UsesUpstreamQueuePolicy(::SlotSwitchSettings::FromText("motion.consumers"), false));
			Assert::IsTrue(VpQueueTiming::Select(UsesUpstreamQueuePolicy(::SlotSwitchSettings::FromText(""), true), true, true) ==
				VpQueueTiming::Selection::Upstream);
			Assert::IsTrue(::SlotSwitchSettings::Describe(::SlotSwitchSettings::FromText(""), true) == "queue policy: upstream (Original pathway)");
			Assert::IsTrue(::SlotSwitchSettings::Describe(::SlotSwitchSettings::FromText("input.queue_timing"), false) ==
				"queue policy: upstream (input.queue_timing off)");
		}

		TEST_METHOD(EveryOffSlotHasAContractAndIsNotUpstreamOnly)
		{
			for (PipelineSlots::Id id : ::SlotSwitchSettings::Switched())
			{
				Assert::IsTrue(PipelineSlots::Get(id).origin != PipelineSlots::Origin::Upstream);
				Assert::IsTrue(PipelineSlots::Get(id).status != PipelineSlots::Status::Placeholder);
			}
		}
	};
}
