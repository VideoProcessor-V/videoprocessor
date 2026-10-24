#include <pch.h>
#include "OutputSegments.h"

#include <VP-V/modules/output/calibration_lut/CalibrationLutStage.h>
#include <led/LedMeasurementHook.h>
#include <vprenderer/CustomHookChain.h>
#include <vprenderer/OcclusionMaskHook.h>
#include <vprenderer/OutputEncodingRepair.h>
#include <vprenderer/SigmaDeltaDomain.h>

#include <algorithm>

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

	ColourCounts OutputColourSegment::Collect(const ColourFrame& f, std::vector<const pl_hook*>& hooks, const TagFn& tag)
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

		// colour.sdr_tone_shape (delta 13): first at PRE_OUTPUT.
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
		// DV L2 trims (same slot switch; not tagged today, so not tagged here).
		if (f.dvTrims)
		{
			const size_t start = hooks.size();
			dvTrims.Collect(false, hooks);
			for (size_t i = start; i < hooks.size(); ++i)
			{
				SetHookParameter(hooks[i], "dv_slope", f.dv.slope);
				SetHookParameter(hooks[i], "dv_offset", f.dv.offset);
				SetHookParameter(hooks[i], "dv_power", f.dv.power);
				SetHookParameter(hooks[i], "dv_chroma_weight", f.dv.chromaWeight);
				SetHookParameter(hooks[i], "dv_sat_gain", f.dv.saturationGain);
				SetHookParameter(hooks[i], "dv_signal_pq", f.dv.signalPq);
				SetHookParameter(hooks[i], "dv_gamma", f.dv.gamma);
				SetHookParameter(hooks[i], "dv_peak_nits", f.dv.peakNits);
				SetHookParameter(hooks[i], "dv_kr", f.dv.kr);
				SetHookParameter(hooks[i], "dv_kb", f.dv.kb);
			}
			counts.dvTrims = hooks.size() - start;
			note(start, Id::ColourSdrToneShape);
		}
		// colour.appearance (Stage 13): placeholder, no component, no hook (zero-cost rule).
		if (f.ledBlock)
		{
			// drive.led_measurement: sees the picture before the gain.
			if (f.ledMeasure)
			{
				hooks.push_back(measure.Hook());
				tag(hooks.size() - 1, Id::DriveLedMeasure);
				counts.ledMeasure = 1;
				note(hooks.size() - 1, Id::DriveLedMeasure);
			}
			// drive.led_compensation (gain, gamma boost, shadow lift, veil; BTB/WTW passthrough).
			const size_t start = hooks.size();
			if (f.ledCompensation)
				compensation.Collect(false, hooks);
			tag(start, Id::DriveLedCompensation);
			for (size_t i = start; i < hooks.size(); ++i)
			{
				const CompensationParams& c = f.compensation;
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
		}
		// drive.black_level: placeholder, no hook.
		// The edge: the user's PRE_OUTPUT (and other) shaders, exactly where they are today.
		if (f.userPreOutput)
		{
			hooks.insert(hooks.end(), f.userPreOutput->begin(), f.userPreOutput->end());
			counts.user = f.userPreOutput->size();
		}
		// output.calibration_lut (VP stage, R1): VP's last colour hook.
		if (f.calibrationLut)
		{
			hooks.push_back(lut.Get());
			tag(hooks.size() - 1, Id::OutputCalibrationLut);
			counts.lut = 1;
			note(hooks.size() - 1, Id::OutputCalibrationLut);
		}
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

	bool OutputFinishSegment::PrepareDither(std::vector<const pl_hook*>& hooks, const SigmaDeltaDomain::Domain& domain,
		bool outputFloat32)
	{
		bool any = false;
		for (size_t i = 0; i < hooks.size();)
		{
			const pl_hook* hook = hooks[i];
			const bool sigma = SetHookParameter(hook, "levels_full", domain.full ? 1.0 : 0.0);
			any = any || sigma;
			if (sigma && !domain.supported)
			{
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
		return any;
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

	bool OutputFinishSegment::Member(const pl_hook* hook) const
	{
		return std::find(members.begin(), members.end(), hook) != members.end();
	}
}
