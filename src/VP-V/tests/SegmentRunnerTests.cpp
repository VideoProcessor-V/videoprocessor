#include "pch.h"
#include "CppUnitTest.h"

#include <VP-V/core/SegmentRunner.h>
#include <VP-V/segments/OutputSegments.h>
#include <VP-V/modules/output/calibration_lut/CalibrationLutStage.h>
#include <led/LedMeasurementHook.h>
#include <vprenderer/CustomHookChain.h>
#include <vprenderer/OcclusionMaskHook.h>
#include <vprenderer/OutputEncodingRepair.h>
#include <vprenderer/SigmaDeltaDomain.h>

#include <algorithm>
#include <deque>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

// DRAFT (2026-10-24): the segment runner core (order and contract checks, fallback reporting,
// timing sampling) and commit 1's byte-identity by construction: the S2/S3 adapters produce the
// SAME hook list, tags and parameter values as today's RenderLocked code (copied below as the
// reference). CPU only (fake hooks; no GPU).
namespace SegmentRunnerTestsNs
{
	using PipelineSlots::Id;
	using namespace SegmentRunner;

	std::wstring W(const std::string& s) { return std::wstring(s.begin(), s.end()); }

	// ---- fake mpv hooks with named parameters -------------------------------------------------
	struct FakeHooks
	{
		struct Made { std::unique_ptr<pl_hook> hook; std::vector<pl_hook_par> pars; std::vector<pl_var_data> data; };
		std::deque<Made> made;
		std::map<std::string, std::string> files;
		uint64_t nextSignature = 500;

		// Source text: "STAGE=PRE|OUT;PARS=a,b,c"
		const pl_hook* Parse(const std::string& source)
		{
			made.emplace_back();
			Made& m = made.back();
			m.hook.reset(new pl_hook{});
			m.hook->stages = source.find("STAGE=OUT") != std::string::npos ? PL_HOOK_OUTPUT : PL_HOOK_PRE_OUTPUT;
			m.hook->input = PL_HOOK_SIG_TEX;
			m.hook->signature = nextSignature++;
			const size_t p = source.find("PARS=");
			std::vector<std::string> names;
			if (p != std::string::npos)
			{
				std::string token;
				for (size_t i = p + 5; i <= source.size(); ++i)
					if (i == source.size() || source[i] == ',') { if (!token.empty()) names.push_back(token); token.clear(); }
					else token += source[i];
			}
			m.data.resize(names.size());
			m.pars.resize(names.size());
			static std::deque<std::string> keep;   // parameter names outlive the hook tables
			for (size_t i = 0; i < names.size(); ++i)
			{
				keep.push_back(names[i]);
				m.pars[i].name = keep.back().c_str();
				m.pars[i].type = PL_VAR_FLOAT;
				m.pars[i].data = &m.data[i];
				m.pars[i].minimum.f = -1e9f;
				m.pars[i].maximum.f = 1e9f;
			}
			m.hook->parameters = m.pars.data();
			m.hook->num_parameters = static_cast<int>(m.pars.size());
			return m.hook.get();
		}
		CustomHookChain::Loader Loader()
		{
			return {
				[this](const std::string& file, std::string& source, std::string& reason)
				{
					const auto f = files.find(file);
					if (f == files.end()) { reason = "missing"; return false; }
					source = f->second;
					return true;
				},
				[this](const std::string& source) -> const pl_hook* { return Parse(source); },
				[](const pl_hook*& hook) { hook = nullptr; }
			};
		}
	};

	ConfiguredShaderRule Rule(const char* name, const char* file)
	{
		ConfiguredShaderRule r;
		r.name = name;
		r.label = name;
		r.filename = file;
		return r;
	}

	CustomHookChain::TemplateValues Templates()
	{
		CustomHookChain::TemplateValues v;
		v.outputWidth = 2560; v.outputHeight = 1600; v.sourceWidth = 3840; v.sourceHeight = 2160;
		v.refreshHz = 60.0; v.sourceFps = 23.976;
		return v;
	}

	std::map<std::string, float> Params(const pl_hook* h)
	{
		std::map<std::string, float> m;
		for (int i = 0; i < h->num_parameters; ++i)
			m[h->parameters[i].name] = h->parameters[i].data->f;
		return m;
	}

	// The modules RenderLocked owns, built twice (reference and segment) from the same fakes.
	struct Modules
	{
		FakeHooks fakes;
		CustomHookChain::Chain tone{ fakes.Loader() }, dv{ fakes.Loader() }, comp{ fakes.Loader() }, user{ fakes.Loader() };
		LedDrive::MeasurementHook measure;
		CalibrationLutStage::Hook lut;
		pl_custom_lut lutData{};
		Modules(bool withUserPreOutput)
		{
			fakes.files["SDR tone shape.glsl"] = "STAGE=PRE;PARS=headroom,knee_start,strength,eotf,recovery,static_refresh";
			fakes.files["DV trims.glsl"] = "STAGE=PRE;PARS=dv_slope,dv_offset,dv_power,dv_chroma_weight,dv_sat_gain,dv_signal_pq,dv_gamma,dv_peak_nits,dv_kr,dv_kb";
			fakes.files["LED compensation.glsl"] = "STAGE=PRE;PARS=gain,knee,gain_domain,compensation_eotf,boost,boost_knee,boost_knee_width,shadow_lift,veil,wtw_passthrough,btb_passthrough";
			fakes.files["user.glsl"] = "STAGE=PRE;PARS=amount";
			fakes.files["sigma.glsl"] = "STAGE=OUT;PARS=levels_full,swapchain_bits,color_bits,output_float32,occlusion_mask,grain_tolerance_scale,static_refresh";
			fakes.files["after.glsl"] = "STAGE=OUT;PARS=amount";
			tone.Configure({ Rule("sdr_tone_shape", "SDR tone shape.glsl") });
			dv.Configure({ Rule("dv_trims", "DV trims.glsl") });
			comp.Configure({ Rule("led_compensation", "LED compensation.glsl") });
			std::vector<ConfiguredShaderRule> users;
			if (withUserPreOutput) users.push_back(Rule("user", "user.glsl"));
			users.push_back(Rule("sigma", "sigma.glsl"));
			user.Configure(users);
			for (CustomHookChain::Chain* c : { &tone, &dv, &comp, &user })
				c->Ensure(Templates());
			lut.SetLut(&lutData);
		}
	};

	// Today's RenderLocked S2 assembly (d0212534 24000-24330), reduced to the hook list.
	void LegacyS2(Modules& m, const OutputSegments::ColourFrame& f, std::vector<const pl_hook*>& frameHooks,
		std::vector<std::pair<const pl_hook*, Id>>& tags)
	{
		auto TagSlotHooks = [&](size_t start, Id slot) { for (size_t i = start; i < frameHooks.size(); ++i) tags.emplace_back(frameHooks[i], slot); };
		if (f.toneShape)
		{
			const size_t start = frameHooks.size();
			m.tone.Collect(false, frameHooks);
			TagSlotHooks(start, Id::ColourSdrToneShape);
			for (size_t i = start; i < frameHooks.size(); ++i)
			{
				CustomHookChain::SetHookParameter(frameHooks[i], "headroom", f.tone.headroom);
				CustomHookChain::SetHookParameter(frameHooks[i], "knee_start", f.tone.kneeStart);
				CustomHookChain::SetHookParameter(frameHooks[i], "strength", f.tone.strength);
				CustomHookChain::SetHookParameter(frameHooks[i], "eotf", f.tone.eotf);
				CustomHookChain::SetHookParameter(frameHooks[i], "recovery", f.tone.recovery);
			}
		}
		if (f.dvTrims)
		{
			const size_t start = frameHooks.size();
			m.dv.Collect(false, frameHooks);
			for (size_t i = start; i < frameHooks.size(); ++i)
			{
				CustomHookChain::SetHookParameter(frameHooks[i], "dv_slope", f.dv.slope);
				CustomHookChain::SetHookParameter(frameHooks[i], "dv_offset", f.dv.offset);
				CustomHookChain::SetHookParameter(frameHooks[i], "dv_power", f.dv.power);
				CustomHookChain::SetHookParameter(frameHooks[i], "dv_chroma_weight", f.dv.chromaWeight);
				CustomHookChain::SetHookParameter(frameHooks[i], "dv_sat_gain", f.dv.saturationGain);
				CustomHookChain::SetHookParameter(frameHooks[i], "dv_signal_pq", f.dv.signalPq);
				CustomHookChain::SetHookParameter(frameHooks[i], "dv_gamma", f.dv.gamma);
				CustomHookChain::SetHookParameter(frameHooks[i], "dv_peak_nits", f.dv.peakNits);
				CustomHookChain::SetHookParameter(frameHooks[i], "dv_kr", f.dv.kr);
				CustomHookChain::SetHookParameter(frameHooks[i], "dv_kb", f.dv.kb);
			}
		}
		if (f.ledBlock)
		{
			if (f.ledMeasure)
			{
				frameHooks.push_back(m.measure.Hook());
				TagSlotHooks(frameHooks.size() - 1, Id::DriveLedMeasure);
			}
			const size_t compensationStart = frameHooks.size();
			if (f.ledCompensation)
				m.comp.Collect(false, frameHooks);
			TagSlotHooks(compensationStart, Id::DriveLedCompensation);
			const OutputSegments::CompensationParams& c = f.compensation;
			for (size_t i = compensationStart; i < frameHooks.size(); ++i)
			{
				CustomHookChain::SetHookParameter(frameHooks[i], "gain", c.gain);
				CustomHookChain::SetHookParameter(frameHooks[i], "knee", c.knee);
				CustomHookChain::SetHookParameter(frameHooks[i], "gain_domain", c.gainDomain);
				CustomHookChain::SetHookParameter(frameHooks[i], "compensation_eotf", c.eotf);
				CustomHookChain::SetHookParameter(frameHooks[i], "boost", c.boost);
				CustomHookChain::SetHookParameter(frameHooks[i], "boost_knee", c.boostKnee);
				CustomHookChain::SetHookParameter(frameHooks[i], "boost_knee_width", c.boostKneeWidth);
				CustomHookChain::SetHookParameter(frameHooks[i], "shadow_lift", c.shadowLift);
				CustomHookChain::SetHookParameter(frameHooks[i], "veil", c.veil);
				CustomHookChain::SetHookParameter(frameHooks[i], "wtw_passthrough", c.wtwPassthrough);
				CustomHookChain::SetHookParameter(frameHooks[i], "btb_passthrough", c.btbPassthrough);
			}
		}
		m.user.Collect(false, frameHooks);
		if (m.lut.Lut())
		{
			frameHooks.push_back(m.lut.Get());
			TagSlotHooks(frameHooks.size() - 1, Id::OutputCalibrationLut);
		}
	}

	OutputSegments::ColourFrame Frame(int variant)
	{
		OutputSegments::ColourFrame f;
		f.toneShape = (variant & 1) != 0;
		f.tone = { 1.25, 0.6, 0.8, 2.4, 0.3 };
		f.dvTrims = (variant & 2) != 0;
		f.dv.slope = 1.1; f.dv.offset = 0.01; f.dv.power = 0.95; f.dv.peakNits = 120.0;
		f.ledBlock = (variant & 4) != 0;
		f.ledMeasure = (variant & 8) != 0;
		f.ledCompensation = (variant & 16) != 0;
		f.compensation.gain = 1.7; f.compensation.boost = 1.05; f.compensation.veil = 0.02;
		f.calibrationLut = (variant & 32) != 0;
		return f;
	}

	TEST_CLASS(SegmentRunnerTests)
	{
	public:
		TEST_METHOD(DesignedOrderConfiguresMergedAndReleasesFallback)
		{
			PipelineSlots::Registry registry;
			FallbackStatus::Registry fallbacks;
			Segment s2(kS2FallbackId, OutputColourStages(), OutputColourEntry());
			Segment s3(kS3FallbackId, OutputFinishStages(), OutputFinishEntry());
			Assert::IsTrue(s2.Configure(registry, true, 1, fallbacks) == Mode::Merged, W(s2.LastCheck().reason).c_str());
			Assert::IsTrue(s3.Configure(registry, true, 1, fallbacks) == Mode::Merged, W(s3.LastCheck().reason).c_str());
			Assert::IsTrue(s2.Configure(registry, false, 2, fallbacks) == Mode::Separate);
			Assert::AreEqual(std::string("Fallbacks engaged: none"), fallbacks.Heading());
		}

		TEST_METHOD(PlaceholderStagesNeverRun)
		{
			PipelineSlots::Registry registry;
			Segment s2(kS2FallbackId, OutputColourStages(), OutputColourEntry());
			for (const StageInfo& st : s2.Active(registry))
				Assert::IsTrue(st.slot != Id::ColourAppearance && st.slot != Id::DriveBlackLevel, W(st.name).c_str());
			registry.SetEnabled(Id::DriveLedMeasure, false);
			for (const StageInfo& st : s2.Active(registry))
				Assert::IsTrue(st.slot != Id::DriveLedMeasure);
		}

		TEST_METHOD(OutOfOrderStagesRejectedWithReason)
		{
			PipelineSlots::Registry registry;
			std::vector<StageInfo> stages = OutputColourStages();
			// LED compensation listed before the measurement.
			auto measure = std::find_if(stages.begin(), stages.end(), [](const StageInfo& s) { return s.slot == Id::DriveLedMeasure; });
			auto comp = std::find_if(stages.begin(), stages.end(), [](const StageInfo& s) { return s.slot == Id::DriveLedCompensation; });
			std::iter_swap(measure, comp);
			const Check c = CheckOrder(stages, registry);
			Assert::IsFalse(c.ok);
			Assert::IsTrue(c.reason.find("drive.led_measure") != std::string::npos, W(c.reason).c_str());
		}

		TEST_METHOD(NothingMayFollowTheDither)
		{
			PipelineSlots::Registry registry;
			std::vector<StageInfo> stages = OutputFinishStages();
			StageInfo after;
			after.name = "late shader";
			stages.push_back(after);
			const Check c = CheckOrder(stages, registry);
			Assert::IsFalse(c.ok);
			Assert::IsTrue(c.reason.find("after the dither") != std::string::npos, W(c.reason).c_str());
		}

		TEST_METHOD(ClippingBtbWtwBeforeTheDitherRejected)
		{
			std::vector<StageInfo> stages = OutputColourStages();
			stages[0].gives.range = Range::Clipped;
			const Check c = CheckContracts(stages, OutputColourEntry());
			Assert::IsFalse(c.ok);
			Assert::IsTrue(c.reason.find("BTB/WTW") != std::string::npos, W(c.reason).c_str());
			// The quantiser itself may end them.
			Assert::IsTrue(CheckContracts(OutputFinishStages(), OutputFinishEntry()).ok);
		}

		TEST_METHOD(ContractMismatchesRejected)
		{
			std::vector<StageInfo> stages = OutputColourStages();
			stages[1].needs.vectors = true;   // no vectors reach S2
			Assert::IsFalse(CheckContracts(stages, OutputColourEntry()).ok);
			stages = OutputColourStages();
			Flow source = OutputColourEntry();
			source.light = PipelineSlots::Light::Source;   // before colour management
			Assert::IsFalse(CheckContracts(stages, source).ok);
			stages = OutputColourStages();
			stages[0].needs.format = Format::Unorm;
			Assert::IsFalse(CheckContracts(stages, OutputColourEntry()).ok);
		}

		TEST_METHOD(FailedCheckFallsBackVisiblyAndRecovers)
		{
			PipelineSlots::Registry registry;
			FallbackStatus::Registry fallbacks;
			std::vector<StageInfo> bad = OutputColourStages();
			bad[0].gives.range = Range::Clipped;
			Segment s(kS2FallbackId, bad, OutputColourEntry());
			Assert::IsTrue(s.Configure(registry, true, 7, fallbacks) == Mode::Separate);
			const std::string heading = fallbacks.Heading();
			Assert::IsTrue(heading.find("output segment S2 (separate hooks: ") != std::string::npos, W(heading).c_str());
			Segment good(kS2FallbackId, OutputColourStages(), OutputColourEntry());
			Assert::IsTrue(good.Configure(registry, true, 8, fallbacks) == Mode::Merged);
			Assert::AreEqual(std::string("Fallbacks engaged: none"), fallbacks.Heading());
		}

		TEST_METHOD(RuntimeDowngradeHoldsUntilSettingsChange)
		{
			PipelineSlots::Registry registry;
			FallbackStatus::Registry fallbacks;
			Segment s(kS3FallbackId, OutputFinishStages(), OutputFinishEntry());
			Assert::IsTrue(s.Configure(registry, true, 3, fallbacks) == Mode::Merged);
			s.Downgrade("sigma_delta failed", fallbacks);
			Assert::IsTrue(fallbacks.Engaged(kS3FallbackId));
			// Same settings: not re-checked, stays separate (checks never run per frame).
			Assert::IsTrue(s.Configure(registry, true, 3, fallbacks) == Mode::Separate);
			// A settings change re-checks and recovers.
			Assert::IsTrue(s.Configure(registry, true, 4, fallbacks) == Mode::Merged);
			Assert::IsFalse(fallbacks.Engaged(kS3FallbackId));
		}

		TEST_METHOD(TimingSamplesOneInNAndIgnoresPendingResults)
		{
			TimingSampler t(4);
			int sampled = 0;
			for (int i = 0; i < 12; ++i)
				sampled += t.BeginRun() ? 1 : 0;
			Assert::AreEqual(3, sampled);
			t.Poll(Id::DriveLedCompensation, 0);   // not ready: ignored
			Assert::AreEqual<uint64_t>(0, t.Accumulated().Samples(Id::DriveLedCompensation));
			t.Poll(Id::DriveLedCompensation, 250000);
			Assert::AreEqual<uint64_t>(1, t.Accumulated().Samples(Id::DriveLedCompensation));
			Assert::AreEqual(0.25, t.Accumulated().MeanMs(Id::DriveLedCompensation), 1e-9);
		}

		// Commit 1: the S2 adapter equals today's assembly for every combination of stages.
		TEST_METHOD(S2SeparateFormIsTodaysHookListTagsAndParameters)
		{
			for (int withUser = 0; withUser < 2; ++withUser)
				for (int variant = 0; variant < 64; ++variant)
				{
					Modules a(withUser != 0), b(withUser != 0);
					OutputSegments::ColourFrame f = Frame(variant);
					std::vector<const pl_hook*> legacy, seg, user;
					std::vector<std::pair<const pl_hook*, Id>> legacyTags, segTags;
					if (!f.calibrationLut) { a.lut.SetLut(nullptr); b.lut.SetLut(nullptr); }
					LegacyS2(a, f, legacy, legacyTags);
					b.user.Collect(false, user);
					f.userPreOutput = &user;
					OutputSegments::OutputColourSegment s2(b.tone, b.dv, b.measure, b.comp, b.lut);
					s2.Collect(f, seg, [&](size_t start, Id slot) { for (size_t i = start; i < seg.size(); ++i) segTags.emplace_back(seg[i], slot); });
					Assert::AreEqual(legacy.size(), seg.size());
					for (size_t i = 0; i < legacy.size(); ++i)
					{
						// Same module position (the two module sets are built identically).
						Assert::AreEqual(legacy[i]->signature, seg[i]->signature);
						Assert::IsTrue(Params(legacy[i]) == Params(seg[i]), (L"parameters differ at " + std::to_wstring(i)).c_str());
					}
					Assert::AreEqual(legacyTags.size(), segTags.size());
					for (size_t i = 0; i < legacyTags.size(); ++i)
						Assert::IsTrue(legacyTags[i].second == segTags[i].second);
				}
		}

		// Commit 1: the S3 steps equal today's loops (domain, dither last, mask place, repair).
		TEST_METHOD(S3SeparateStepsAreTodaysLoops)
		{
			for (int supported = 0; supported < 2; ++supported)
				for (int lateShader = 0; lateShader < 2; ++lateShader)
				{
					Modules m(false);
					CustomHookChain::Chain late(m.fakes.Loader());
					late.Configure({ Rule("after", "after.glsl") });
					late.Ensure(Templates());
					std::vector<const pl_hook*> base;
					m.user.Collect(false, base);   // the sigma-delta
					if (lateShader) late.Collect(false, base);   // an OUTPUT shader after it
					SigmaDeltaDomain::Domain domain;
					domain.full = false; domain.sample = 10; domain.color = 10; domain.supported = supported != 0;

					// Reference: today's loops.
					std::vector<const pl_hook*> legacy = base;
					for (size_t i = 0; i < legacy.size();)
					{
						const bool sigma = CustomHookChain::SetHookParameter(legacy[i], "levels_full", 0.0);
						if (sigma && !domain.supported) { legacy.erase(legacy.begin() + static_cast<std::ptrdiff_t>(i)); continue; }
						CustomHookChain::SetHookParameter(legacy[i], "swapchain_bits", 10.0);
						CustomHookChain::SetHookParameter(legacy[i], "color_bits", 10.0);
						CustomHookChain::SetHookParameter(legacy[i], "output_float32", 1.0);
						++i;
					}
					{
						std::vector<const pl_hook*> sigmaHooks;
						bool other = false;
						for (const pl_hook* h : legacy)
						{
							if (CustomHookChain::SetHookParameter(h, "levels_full", 0.0)) sigmaHooks.push_back(h);
							else if (!sigmaHooks.empty() && (h->stages & PL_HOOK_OUTPUT)) other = true;
						}
						if (other)
						{
							for (const pl_hook* h : sigmaHooks) legacy.erase(std::find(legacy.begin(), legacy.end(), h));
							legacy.insert(legacy.end(), sigmaHooks.begin(), sigmaHooks.end());
						}
					}
					const std::vector<const pl_hook*> afterLegacy = legacy;
					const std::map<std::string, float> legacyParams = legacy.empty() ? std::map<std::string, float>() : Params(legacy.back());

					// Segment steps on a fresh copy of the same hooks (parameters reset first).
					for (const pl_hook* h : base)
						for (int i = 0; i < h->num_parameters; ++i) h->parameters[i].data->f = 0.0f;
					OutputEncodingRepair::Hook repair;
					OcclusionMask::Hook mask;
					OutputSegments::OutputFinishSegment s3(repair, mask);
					std::vector<const pl_hook*> seg = base;
					s3.BeginFrame();
					s3.PrepareDither(seg, domain, true);
					s3.KeepDitherLast(seg, false);
					Assert::IsTrue(seg == afterLegacy);
					if (!seg.empty())
						Assert::IsTrue(Params(seg.back()) == legacyParams);
					const size_t index = s3.MarkMask(seg, true, true, 2.0);
					if (index < seg.size())
					{
						s3.InsertMask(seg, index);
						Assert::IsTrue(seg[index] == mask.Get());
						Assert::IsTrue(s3.Member(mask.Get()));
					}
				}
		}

		TEST_METHOD(S2MembersExcludeTheUserEdge)
		{
			Modules m(true);
			OutputSegments::ColourFrame f = Frame(1 | 4 | 8 | 16 | 32);
			std::vector<const pl_hook*> user, hooks;
			m.user.Collect(false, user);
			f.userPreOutput = &user;
			OutputSegments::OutputColourSegment s2(m.tone, m.dv, m.measure, m.comp, m.lut);
			s2.Collect(f, hooks, [](size_t, Id) {});
			for (const pl_hook* h : user)
				Assert::IsFalse(s2.Member(h));
			Assert::IsTrue(s2.Member(m.measure.Hook()));
			Assert::IsTrue(s2.Member(m.lut.Get()));
			// Per-slot timing attribution.
			Assert::IsTrue(s2.SlotOf(m.measure.Hook()) == Id::DriveLedMeasure);
			Assert::IsTrue(s2.SlotOf(m.lut.Get()) == Id::OutputCalibrationLut);
			Assert::IsTrue(s2.SlotOf(hooks.front()) == Id::ColourSdrToneShape);
			for (const pl_hook* h : user)
				Assert::IsTrue(s2.SlotOf(h) == Id::Count);
		}
	};
}
