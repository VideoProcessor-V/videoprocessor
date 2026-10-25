#include <pch.h>
#include "OutputSegments.h"

#include <VP-V/modules/output/calibration_lut/CalibrationLutStage.h>
#include <led/LedMeasurementHook.h>
#include <vprenderer/CustomHookChain.h>
#include <vprenderer/OcclusionMaskHook.h>
#include <vprenderer/OutputEncodingRepair.h>
#include <vprenderer/SigmaDeltaDomain.h>

#include <algorithm>
#include <iterator>

namespace OutputSegments
{
	using PipelineSlots::Id;
	using CustomHookChain::SetHookParameter;

	OutputColourSegment::OutputColourSegment(CustomHookChain::Chain& toneShapeIn, CustomHookChain::Chain& dvTrimsIn,
		LedDrive::MeasurementHook& measureIn, CustomHookChain::Chain& compensationIn, CalibrationLutStage::Hook& lutIn)
		: toneShape(toneShapeIn), dvTrims(dvTrimsIn), measure(measureIn), compensation(compensationIn), lut(lutIn),
		segment(SegmentRunner::kS2FallbackId, SegmentRunner::OutputColourStages(), SegmentRunner::OutputColourEntry())
	{
	}

	ColourCounts OutputColourSegment::Collect(const ColourFrame& f, const PipelineSlots::Registry& registry,
		std::vector<const pl_hook*>& hooks, const TagFn& tag)
	{
		ColourCounts counts;
		members.clear();
		memberSlots.clear();
		auto note = [this, &hooks](size_t start, Id slot)
		{
			for (size_t i = start; i < hooks.size(); ++i)
			{
				members.push_back(hooks[i]);
				memberSlots.push_back(slot);
			}
		};

		// colour.sdr_tone_shape (delta 13) and the DV trims (same slot switch and tag).
		auto toneStage = [&]()
		{
			if (f.toneShape)
			{
				const size_t start = hooks.size();
				toneShape.Collect(false, hooks);
				tag(start, Id::ColourSdrToneShape);
				for (size_t i = start; i < hooks.size(); ++i)
				{
					SetHookParameter(hooks[i], "headroom", f.tone.headroom);
					SetHookParameter(hooks[i], "knee_start", f.tone.kneeStart);
					SetHookParameter(hooks[i], "strength", f.tone.strength);
					SetHookParameter(hooks[i], "eotf", f.tone.eotf);
					SetHookParameter(hooks[i], "recovery", f.tone.recovery);
				}
				counts.toneShape = hooks.size() - start;
				note(start, Id::ColourSdrToneShape);
			}
			if (f.dvTrims)
			{
				const size_t start = hooks.size();
				dvTrims.Collect(false, hooks);
				tag(start, Id::ColourSdrToneShape);   // as the host since 38badcb0: the trims follow the colour slot
				for (size_t i = start; i < hooks.size(); ++i)
					for (const auto& parameter : f.dv)
						SetHookParameter(hooks[i], parameter.first, parameter.second);
				counts.dvTrims = hooks.size() - start;
				note(start, Id::ColourSdrToneShape);
			}
		};
		// drive.led_measure: sees the picture before the gain.
		auto measureStage = [&]()
		{
			if (!f.ledBlock || !f.ledMeasure)
				return;
			hooks.push_back(measure.Hook());
			tag(hooks.size() - 1, Id::DriveLedMeasure);
			counts.ledMeasure = 1;
			note(hooks.size() - 1, Id::DriveLedMeasure);
		};
		// drive.led_compensation (gain, gamma boost, shadow lift, veil; BTB/WTW passthrough).
		auto compensationStage = [&]()
		{
			if (!f.ledBlock)
				return;
			const size_t start = hooks.size();
			if (f.ledCompensation)
				compensation.Collect(false, hooks);
			tag(start, Id::DriveLedCompensation);
			const CompensationParams& c = f.compensation;
			for (size_t i = start; i < hooks.size(); ++i)
			{
				SetHookParameter(hooks[i], "gain", c.gain);
				SetHookParameter(hooks[i], "knee", c.knee);
				SetHookParameter(hooks[i], "gain_domain", c.gainDomain);
				SetHookParameter(hooks[i], "compensation_eotf", c.eotf);
				SetHookParameter(hooks[i], "boost", c.boost);
				SetHookParameter(hooks[i], "boost_knee", c.boostKnee);
				SetHookParameter(hooks[i], "boost_knee_width", c.boostKneeWidth);
				SetHookParameter(hooks[i], "shadow_lift", c.shadowLift);
				SetHookParameter(hooks[i], "veil", c.veil);
				SetHookParameter(hooks[i], "wtw_passthrough", c.wtwPassthrough);
				SetHookParameter(hooks[i], "btb_passthrough", c.btbPassthrough);
			}
			counts.ledCompensation = hooks.size() - start;
			note(start, Id::DriveLedCompensation);
		};
		// The edge: the user's shaders. They run before VP's calibration LUT (R1), wherever the
		// module order puts the LUT, exactly as today.
		bool edgeDone = false;
		auto edge = [&]()
		{
			if (edgeDone)
				return;
			edgeDone = true;
			if (f.userPreOutput)
			{
				hooks.insert(hooks.end(), f.userPreOutput->begin(), f.userPreOutput->end());
				counts.user = f.userPreOutput->size();
			}
		};
		// output.calibration_lut (VP stage, R1): VP's last colour hook in the designed order.
		auto lutStage = [&]()
		{
			edge();
			if (!f.calibrationLut)
				return;
			hooks.push_back(lut.Get());
			tag(hooks.size() - 1, Id::OutputCalibrationLut);
			counts.lut = 1;
			note(hooks.size() - 1, Id::OutputCalibrationLut);
		};

		// The stages in the REGISTRY's effective order (validated by OrderValidator against
		// SlotContracts), each only when its slot runs (pipeline_slots_off, Original pathway): a slot
		// that is off adds no hook. colour.appearance (Stage 13) and drive.black_level are
		// placeholders: no component, so they never run and cost nothing.
		struct Stage { Id slot; int order; int kind; };
		Stage stages[] = {
			{ Id::ColourSdrToneShape, registry.Order(Id::ColourSdrToneShape), 0 },
			{ Id::ColourAppearance, registry.Order(Id::ColourAppearance), 4 },   // placeholder (Stage 13)
			{ Id::DriveLedMeasure, registry.Order(Id::DriveLedMeasure), 1 },
			{ Id::DriveLedCompensation, registry.Order(Id::DriveLedCompensation), 2 },
			{ Id::DriveBlackLevel, registry.Order(Id::DriveBlackLevel), 4 },     // placeholder
			{ Id::OutputCalibrationLut, registry.Order(Id::OutputCalibrationLut), 3 },
		};
		std::stable_sort(std::begin(stages), std::end(stages), [](const Stage& x, const Stage& y) { return x.order < y.order; });
		for (const Stage& st : stages)
		{
			if (!registry.Runs(st.slot))
			{
				if (st.kind == 3)
					edge();   // the user's shaders still run where the LUT would be
				continue;
			}
			switch (st.kind)
			{
			case 0: toneStage(); break;
			case 1: measureStage(); break;
			case 2: compensationStage(); break;
			case 3: lutStage(); break;
			default:
				// A placeholder slot with a component but no S2 stage yet: counted for the host to report
				// (it never runs silently, and never runs half-wired).
				++counts.unhandled;
				break;
			}
		}
		edge();
		return counts;
	}

	bool OutputColourSegment::Member(const pl_hook* hook) const
	{
		return std::find(members.begin(), members.end(), hook) != members.end();
	}

	Id OutputColourSegment::SlotOf(const pl_hook* hook) const
	{
		for (size_t i = 0; i < members.size(); ++i)
			if (members[i] == hook)
				return memberSlots[i];
		return Id::Count;
	}

	OutputFinishSegment::OutputFinishSegment(OutputEncodingRepair::Hook& repairIn, OcclusionMask::Hook& maskIn)
		: repair(repairIn), mask(maskIn),
		segment(SegmentRunner::kS3FallbackId, SegmentRunner::OutputFinishStages(), SegmentRunner::OutputFinishEntry())
	{
	}

	void OutputFinishSegment::Note(const pl_hook* hook)
	{
		if (!Member(hook))
			members.push_back(hook);
	}

	DitherPrep OutputFinishSegment::PrepareDither(std::vector<const pl_hook*>& hooks, const SigmaDeltaDomain::Domain& domain,
		bool outputFloat32, const PipelineSlots::Registry& registry)
	{
		// output.dither off (pipeline_slots_off): the sigma-delta is not submitted, as for a code
		// domain it does not support; libplacebo's own dither then runs as configured. Only the
		// domain case is reported as a fallback (DitherPrep).
		const bool ditherRuns = registry.Runs(Id::OutputDither);
		DitherPrep prep;
		prep.slotOff = !ditherRuns;
		for (size_t i = 0; i < hooks.size();)
		{
			const pl_hook* hook = hooks[i];
			const bool sigma = SetHookParameter(hook, "levels_full", domain.full ? 1.0 : 0.0);
			prep.found = prep.found || sigma;
			if (sigma && (!domain.supported || !ditherRuns))
			{
				if (ditherRuns)
					prep.domainDropped = true;
				hooks.erase(hooks.begin() + static_cast<std::ptrdiff_t>(i));
				continue;
			}
			if (sigma)
				Note(hook);
			SetHookParameter(hook, "swapchain_bits", static_cast<double>(domain.sample));
			SetHookParameter(hook, "color_bits", static_cast<double>(domain.color));
			SetHookParameter(hook, "output_float32", outputFloat32 ? 1.0 : 0.0);
			++i;
		}
		return prep;
	}

	bool OutputFinishSegment::KeepDitherLast(std::vector<const pl_hook*>& hooks, bool levelsFull)
	{
		std::vector<const pl_hook*> sigmaHooks;
		bool otherOutputAfterSigma = false;
		for (const pl_hook* hook : hooks)
		{
			const bool sigma = SetHookParameter(hook, "levels_full", levelsFull ? 1.0 : 0.0);
			if (sigma)
				sigmaHooks.push_back(hook);
			else if (!sigmaHooks.empty() && hook && (hook->stages & PL_HOOK_OUTPUT))
				otherOutputAfterSigma = true;
		}
		if (!otherOutputAfterSigma)
			return false;
		for (const pl_hook* hook : sigmaHooks)
			hooks.erase(std::find(hooks.begin(), hooks.end(), hook));
		hooks.insert(hooks.end(), sigmaHooks.begin(), sigmaHooks.end());
		return true;
	}

	size_t OutputFinishSegment::MarkMask(std::vector<const pl_hook*>& hooks, bool install, bool grainInstall, double grainScale)
	{
		size_t sigmaIndex = hooks.size();
		for (size_t i = 0; i < hooks.size(); ++i)
			if (SetHookParameter(hooks[i], "occlusion_mask", install ? 1.0 : 0.0) && sigmaIndex == hooks.size())
				sigmaIndex = i;
		for (size_t i = 0; i < hooks.size(); ++i)
			SetHookParameter(hooks[i], "grain_tolerance_scale", grainInstall && sigmaIndex < hooks.size() ? grainScale : 1.0);
		return sigmaIndex;
	}

	void OutputFinishSegment::InsertMask(std::vector<const pl_hook*>& hooks, size_t ditherIndex)
	{
		hooks.insert(hooks.begin() + static_cast<std::ptrdiff_t>(ditherIndex), mask.Get());
		Note(mask.Get());
	}

	bool OutputFinishSegment::Repair(std::vector<const pl_hook*>& hooks, const pl_render_errors* errors,
		enum pl_lut_type targetLut, const pl_color_repr& targetRepr)
	{
		if (!OutputEncodingRepair::Needed(hooks, errors, targetLut))
			return false;
		repair.SetTarget(targetRepr);
		hooks.insert(hooks.begin(), repair.Get());
		Note(repair.Get());
		return true;
	}

	const char* OutputFinishSegment::StageName(const pl_hook* hook) const
	{
		return hook == repair.Get() ? "encoding_repair" : hook == mask.Get() ? "occlusion_mask" : "sigma_delta";
	}

	bool OutputFinishSegment::Member(const pl_hook* hook) const
	{
		return std::find(members.begin(), members.end(), hook) != members.end();
	}
}
