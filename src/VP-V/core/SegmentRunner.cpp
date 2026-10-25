// DRAFT (2026-10-24, not integrated): see SegmentRunner.h. Pure C++14, no precompiled header.
#include "SegmentRunner.h"

namespace SegmentRunner
{
	namespace
	{
		const char* FormatName(uint8_t f)
		{
			return f == Format::Fp16 ? "fp16" : f == Format::Fp32 ? "fp32" : f == Format::Unorm ? "unorm" : "float";
		}
		std::string StageName(const StageInfo& s)
		{
			return s.slot == Id::Count ? std::string(s.name) : std::string(PipelineSlots::Get(s.slot).name) +
				(s.name && *s.name ? std::string(" (") + s.name + ")" : std::string());
		}
	}

	Check CheckOrder(const std::vector<StageInfo>& stages, const PipelineSlots::Registry& registry)
	{
		Check c;
		std::string reason;
		if (!PipelineSlots::Validate(registry.CurrentOrder(), &reason))
		{
			c.ok = false;
			c.reason = "module order rejected: " + reason;
			return c;
		}
		int lastRank = -1;
		const StageInfo* lastStage = nullptr;
		bool quantiserSeen = false;
		for (const StageInfo& s : stages)
		{
			if (quantiserSeen)
			{
				c.ok = false;
				c.reason = StageName(s) + " would run after the dither (the dither must be the last processing)";
				return c;
			}
			if (s.role == Role::Quantiser)
				quantiserSeen = true;
			if (s.role == Role::Edge || s.slot == Id::Count)
				continue;
			const int rank = registry.Order(s.slot);
			if (rank < lastRank)
			{
				c.ok = false;
				c.reason = StageName(s) + " is listed after " + StageName(*lastStage) +
					" but the module order runs it first";
				return c;
			}
			lastRank = rank;
			lastStage = &s;
		}
		return c;
	}

	Check CheckContracts(const std::vector<StageInfo>& stages, const Flow& entry)
	{
		Check c;
		Flow f = entry;
		for (size_t i = 0; i < stages.size(); ++i)
		{
			const StageInfo& s = stages[i];
			if (s.role == Role::Edge)
			{
				// User shaders: unknown contract; libplacebo's FBO format, range unknown (assumed kept).
				continue;
			}
			const std::string name = StageName(s);
			auto fail = [&](const std::string& why) { c.ok = false; c.reason = name + ": " + why; return c; };
			if (!(s.needs.res & f.res))
				return fail(std::string("needs ") + PipelineSlots::ResNeedName(s.needs.res) + ", gets " + PipelineSlots::ResName(f.res));
			if (!(s.needs.light & f.light))
				return fail(std::string("needs ") + PipelineSlots::LightName(s.needs.light) + ", gets " + PipelineSlots::LightName(f.light));
			if (!(s.needs.format & f.format))
				return fail(std::string("cannot take ") + FormatName(f.format) + " intermediates");
			if (!(s.needs.range & f.range))
				return fail("needs a clipped signal but BTB/WTW are carried at this point");
			if (s.needs.vectors && !f.vectorsValid)
				return fail("needs valid motion vectors, none are delivered to this segment");
			if (s.gives.res) f.res = s.gives.res;
			if (s.gives.light) f.light = s.gives.light;
			if (s.gives.format) f.format = s.gives.format;
			if (s.gives.range)
			{
				// BTB/WTW are kept in all cases (owner): only the quantiser may end them.
				if (s.gives.range == Range::Clipped && f.range == Range::Extended && s.role != Role::Quantiser)
					return fail("would clip BTB/WTW before the dither");
				f.range = s.gives.range;
			}
		}
		return c;
	}

	std::vector<StageInfo> OutputColourStages()
	{
		namespace Light = PipelineSlots::Light;
		std::vector<StageInfo> s;
		auto add = [&s](Id slot, const char* name, Role role, uint8_t needLight, uint8_t giveLight, bool modifies)
		{
			StageInfo info;
			info.slot = slot;
			info.name = name;
			info.role = role;
			info.needs.light = needLight;
			info.gives = StageContract{ 0, giveLight, 0, 0, false };
			info.modifies = modifies;
			s.push_back(info);
		};
		// RenderLocked's PRE_OUTPUT list today (fd60b650 / d0212534 24000-24330), in order.
		add(Id::ColourSdrToneShape, "sdr_tone_shape", Role::Vp, Light::Target, 0, true);
		add(Id::ColourSdrToneShape, "dv_trims", Role::Vp, Light::Target, 0, true);    // DV L2 trims share the slot switch
		add(Id::ColourAppearance, "stage13", Role::Vp, Light::Target, 0, true);       // placeholder: no component, never runs
		add(Id::DriveLedMeasure, "led_measure", Role::Vp, Light::Target, 0, false);
		add(Id::DriveLedCompensation, "led_compensation", Role::Vp, Light::Target, 0, true);  // adaptive gamma / boost
		add(Id::DriveBlackLevel, "black_level", Role::Vp, Light::Target, 0, true);    // placeholder
		add(Id::Count, "user PRE_OUTPUT shaders", Role::Edge, Light::Any, 0, true);
		add(Id::OutputCalibrationLut, "calibration_lut", Role::Vp, Light::Target, Light::Codes, true);
		return s;
	}

	std::vector<StageInfo> OutputFinishStages()
	{
		namespace Light = PipelineSlots::Light;
		std::vector<StageInfo> s;
		StageInfo repair;
		repair.name = "encoding_repair";   // libplacebo 7.360 workaround; Needed() decides per frame
		repair.needs.light = Light::Any;
		repair.gives.light = Light::Codes;   // the repair encodes the range: BTB/WTW stay outside the nominal codes
		s.push_back(repair);
		StageInfo mask;
		mask.name = "occlusion_mask";   // writes alpha for the dither (de-judder 6c / grain map)
		mask.needs.light = Light::Any;
		mask.needs.vectors = false;     // the mask is made from the vectors upstream; gated there
		s.push_back(mask);
		StageInfo dither;
		dither.slot = Id::OutputDither;
		dither.name = "sigma_delta";
		dither.role = Role::Quantiser;
		dither.needs.light = Light::Any;
		dither.gives.range = Range::Clipped;
		dither.gives.format = Format::Unorm;
		s.push_back(dither);
		return s;
	}

	Flow OutputColourEntry()
	{
		Flow f;
		f.res = PipelineSlots::Res::Output;
		f.light = PipelineSlots::Light::Target;
		f.format = Format::Float;   // libplacebo FBO: rgba32f (fbo_float32 on) or rgba16f
		f.range = Range::Extended;
		f.vectorsValid = false;
		return f;
	}

	Flow OutputFinishEntry()
	{
		Flow f = OutputColourEntry();
		f.light = PipelineSlots::Light::Target | PipelineSlots::Light::Codes;   // repaired (7.360) or encoded (377)
		return f;
	}
}
