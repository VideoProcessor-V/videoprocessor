#include "pch.h"
#include "CppUnitTest.h"

#include <VP-V/core/FallbackCatalog.h>
#include <VP-V/core/OutputHookOrder.h>
#include <VP-V/core/SegmentRunner.h>
#include <VP-V/segments/OutputSegments.h>
#include <VP-V/segments/OutputSegmentsMerge.h>
#include <VP-V/modules/output/calibration_lut/CalibrationLutStage.h>
#include <led/LedMeasurementHook.h>
#include <vprenderer/CustomHookChain.h>
#include <VP-V/modules/motion/consumers/OcclusionMaskHook.h>
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
		static pl_hook_res Noop(void*, const pl_hook_params*) { return pl_hook_res{}; }
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
			m.hook->hook = &Noop;   // a callable hook (the merge takes only hooks that can run)
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
			TagSlotHooks(start, Id::ColourSdrToneShape);   // 38badcb0: trims follow the colour slot order
			for (size_t i = start; i < frameHooks.size(); ++i)
			{
				for (const auto& parameter : f.dv)   // dc149284: the selection's list, ICtCp list, then the fixed five
					CustomHookChain::SetHookParameter(frameHooks[i], parameter.first, parameter.second);
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
		f.dv = { { "dv_slope", 1.1 }, { "dv_offset", 0.01 }, { "dv_power", 0.95 }, { "dv_signal_pq", 1.0 }, { "dv_peak_nits", 120.0 } };
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
			Assert::IsTrue(s.Rechecked());
			Assert::IsFalse(fallbacks.Engaged(kS3FallbackId));
		}

		TEST_METHOD(FailedCheckWithMergeOffIsNotAFallback)
		{
			// Audit L4: the separate form is today's behaviour, so with the merged form off a failed
			// check engages nothing (the reason stays readable in LastCheck()).
			PipelineSlots::Registry registry;
			FallbackStatus::Registry fallbacks;
			std::vector<StageInfo> bad = OutputColourStages();
			bad[0].gives.range = Range::Clipped;
			Segment s(kS2FallbackId, bad, OutputColourEntry());
			Assert::IsTrue(s.Configure(registry, false, 1, fallbacks) == Mode::Separate);
			Assert::IsFalse(s.LastCheck().ok);
			Assert::AreEqual(std::string("Fallbacks engaged: none"), fallbacks.Heading());
			Assert::IsTrue(s.Configure(registry, true, 2, fallbacks) == Mode::Separate);
			Assert::IsTrue(s.FallbackChanged());
			Assert::IsTrue(fallbacks.Engaged(kS2FallbackId));
		}

		TEST_METHOD(UnsetGivesKeepTheFlow)
		{
			// m2-entrance audit #1: a stage that changes nothing must not reset the flow. After the
			// repair (gives codes) and the mask (gives nothing), a stage needing codes still gets them.
			std::vector<StageInfo> stages = OutputFinishStages();
			StageInfo needsCodes;
			needsCodes.name = "needs codes";
			needsCodes.needs.light = PipelineSlots::Light::Codes;
			stages.insert(stages.begin() + 2, needsCodes);   // repair, mask, needs codes, dither
			Flow entry = OutputFinishEntry();
			entry.light = PipelineSlots::Light::Target;      // the 7.360 path: unencoded until the repair
			const Check c = CheckContracts(stages, entry);
			Assert::IsTrue(c.ok, W(c.reason).c_str());
			// And a clip is never silently re-marked as carried by a later stage.
			stages = OutputFinishStages();
			StageInfo clips;
			clips.name = "clip";
			clips.gives.range = Range::Clipped;
			stages.insert(stages.begin() + 1, clips);
			Assert::IsFalse(CheckContracts(stages, OutputFinishEntry()).ok);
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
			PipelineSlots::Registry registry;
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
					s2.Collect(f, registry, seg, [&](size_t start, Id slot) { for (size_t i = start; i < seg.size(); ++i) segTags.emplace_back(seg[i], slot); });
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
			PipelineSlots::Registry registry;
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
					s3.PrepareDither(seg, domain, true, registry);
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
					// Step 4, the repair, against today's insertion (a texture PRE_OUTPUT hook in front).
					std::vector<const pl_hook*> pre;
					m.tone.Collect(false, pre);
					std::vector<const pl_hook*> legacyR = pre;
					legacyR.insert(legacyR.end(), seg.begin(), seg.end());
					std::vector<const pl_hook*> segR = legacyR;
					const bool needed = OutputEncodingRepair::Needed(legacyR, nullptr, PL_LUT_UNKNOWN);
					if (needed)
						legacyR.insert(legacyR.begin(), repair.Get());
					Assert::AreEqual(needed, s3.Repair(segR, nullptr, PL_LUT_UNKNOWN, pl_color_repr{}));
					Assert::IsTrue(segR == legacyR);
					Assert::AreEqual(needed, s3.Member(repair.Get()));
					Assert::AreEqual(std::string("encoding_repair"), std::string(s3.StageName(repair.Get())));
				}
		}

		TEST_METHOD(S2MembersExcludeTheUserEdge)
		{
			PipelineSlots::Registry registry;
			Modules m(true);
			OutputSegments::ColourFrame f = Frame(1 | 4 | 8 | 16 | 32);
			std::vector<const pl_hook*> user, hooks;
			m.user.Collect(false, user);
			f.userPreOutput = &user;
			OutputSegments::OutputColourSegment s2(m.tone, m.dv, m.measure, m.comp, m.lut);
			s2.Collect(f, registry, hooks, [](size_t, Id) {});
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

		// Merger without a GPU: the scope swaps and restores the hooks, failures are attributed to
		// the right segment with the stage's name and returned for the host, a merged hook libplacebo
		// disabled downgrades its segment, and a settings change gives stages another chance.
		TEST_METHOD(MergerScopeRestoresAndReportsVisibly)
		{
			Modules m(false);
			PipelineSlots::Registry registry;
			FallbackStatus::Registry fallbacks;
			OutputSegments::ColourFrame f = Frame(1 | 4 | 8 | 16 | 32);
			std::vector<const pl_hook*> user, hooks;
			m.user.Collect(false, user);   // the sigma-delta (OUTPUT)
			f.userPreOutput = &user;
			OutputSegments::OutputColourSegment s2(m.tone, m.dv, m.measure, m.comp, m.lut);
			s2.Collect(f, registry, hooks, [](size_t, Id) {});
			OutputEncodingRepair::Hook repair;
			OcclusionMask::Hook mask;
			OutputSegments::OutputFinishSegment s3(repair, mask);
			s3.BeginFrame();
			SigmaDeltaDomain::Domain domain;
			s3.PrepareDither(hooks, domain, true, registry);
			s3.InsertMask(hooks, s3.MarkMask(hooks, true, false, 1.0));
			OutputSegments::Merger merger(nullptr, s2, s3);
			Assert::IsFalse(merger.Configure(registry, false, 0, fallbacks), L"switch off: nothing merges");
			{
				// The Original pathway never merges, whatever output_segments_merged says.
				PipelineSlots::Registry original;
				original.ApplyOriginalPathway();
				OutputSegments::Merger originalMerger(nullptr, s2, s3);
				Assert::IsFalse(originalMerger.Configure(original, true, 0, fallbacks), L"Original pathway: separate hooks");
			}
			Assert::IsTrue(merger.Configure(registry, true, 0, fallbacks));
			pl_render_params params{};
			params.hooks = hooks.data();
			params.num_hooks = static_cast<int>(hooks.size());
			{
				OutputSegments::Merger::Scope scope(&merger, params, nullptr);
				// tone, measure, compensation, lut -> 1; mask, sigma -> 1
				Assert::AreEqual(2, params.num_hooks);
			}
			Assert::IsTrue(params.hooks == hooks.data() && params.num_hooks == static_cast<int>(hooks.size()), L"restored");
			// The LED measurement fails inside the merged hook.
			merger.Runner().MarkFailed(m.measure.Hook(), "hook failed");
			const std::vector<uint64_t> failed = merger.Report(nullptr);
			Assert::AreEqual<size_t>(1, failed.size());
			Assert::AreEqual(m.measure.Hook()->signature, failed[0]);
			Assert::IsTrue(s2.Segment().CurrentMode() == SegmentRunner::Mode::Separate);
			Assert::IsTrue(s3.Segment().CurrentMode() == SegmentRunner::Mode::Merged, L"S3 is not blamed");
			const std::string heading = fallbacks.Heading();
			Assert::IsTrue(heading.find("output segment S2 (separate hooks: drive.led_measure: hook failed)") != std::string::npos, W(heading).c_str());
			Assert::IsTrue(merger.FallbacksChanged());
			// libplacebo disables the merged S3 hook: S3 falls back too.
			uint64_t mergedSignature = 0;
			{
				OutputSegments::Merger::Scope scope(&merger, params, nullptr);
				Assert::AreEqual(1, static_cast<int>(merger.Runner().MergedCount()));
				for (int i = 0; i < params.num_hooks; ++i)
					if (params.hooks[i]->hook && std::find(hooks.begin(), hooks.end(), params.hooks[i]) == hooks.end())
						mergedSignature = params.hooks[i]->signature;
			}
			pl_render_errors errors{};
			errors.disabled_hooks = &mergedSignature;
			errors.num_disabled_hooks = 1;
			merger.Report(&errors);
			Assert::IsTrue(s3.Segment().CurrentMode() == SegmentRunner::Mode::Separate);
			Assert::IsTrue(fallbacks.Heading().find("libplacebo disabled the merged hook (occlusion_mask, sigma_delta)") != std::string::npos,
				W(fallbacks.Heading()).c_str());
			Assert::IsFalse(merger.Active());
			// A settings change re-checks, clears the failures and merges again.
			registry.SetEnabled(Id::DriveLedMeasure, true);   // same switches: no change yet
			merger.Configure(registry, true, 0, fallbacks);
			Assert::IsFalse(merger.Active(), L"same settings: the downgrade holds");
			registry.SetEnabled(Id::RepairDeband, false);      // a real settings change
			Assert::IsTrue(merger.Configure(registry, true, 0, fallbacks));
			Assert::IsFalse(merger.Runner().Failed(m.measure.Hook()));
			Assert::AreEqual(std::string("Fallbacks engaged: none"), fallbacks.Heading());
		}

		// ---- owner rules: every S2/S3 slot switchable, order from the registry, placeholders inert ----
		static std::vector<uint64_t> Signatures(const std::vector<const pl_hook*>& v)
		{
			std::vector<uint64_t> out;
			for (const pl_hook* h : v) out.push_back(h->signature);
			return out;
		}

		// The segment's list for `registry` against today's assembly with the host's flags for the
		// same switches (Runs() decided in the host today).
		static void CompareWithLegacy(PipelineSlots::Registry& registry, const wchar_t* what, int variant = 63)
		{
			Modules a(true), b(true);
			OutputSegments::ColourFrame f = Frame(variant);
			OutputSegments::ColourFrame legacyFrame = f;
			legacyFrame.toneShape = f.toneShape && registry.Runs(Id::ColourSdrToneShape);
			legacyFrame.dvTrims = f.dvTrims && registry.Runs(Id::ColourSdrToneShape);
			legacyFrame.ledMeasure = f.ledMeasure && registry.Runs(Id::DriveLedMeasure);
			legacyFrame.ledCompensation = f.ledCompensation && registry.Runs(Id::DriveLedCompensation);
			if (!registry.Runs(Id::OutputCalibrationLut)) a.lut.SetLut(nullptr);
			std::vector<const pl_hook*> legacy, seg, user;
			std::vector<std::pair<const pl_hook*, Id>> legacyTags, segTags;
			LegacyS2(a, legacyFrame, legacy, legacyTags);
			// Today's host then puts the slot hooks in the registry's order (OrderFrameSlotHooks).
			std::vector<OutputHookOrder::Tagged<const pl_hook*>> tagged;
			for (const pl_hook* h : legacy)
			{
				OutputHookOrder::Tagged<const pl_hook*> t;
				t.hook = h;
				for (const auto& e : legacyTags)
					if (e.first == h) { t.kind = OutputHookOrder::Kind::Slot; t.slot = e.second; }
				tagged.push_back(t);
			}
			legacy = OutputHookOrder::OrderSlotsInPlace(tagged, [&registry](Id id) { return registry.Order(id); });
			b.user.Collect(false, user);
			f.userPreOutput = &user;
			OutputSegments::OutputColourSegment s2(b.tone, b.dv, b.measure, b.comp, b.lut);
			s2.Collect(f, registry, seg, [&](size_t start, Id slot) { for (size_t i = start; i < seg.size(); ++i) segTags.emplace_back(seg[i], slot); });
			Assert::IsTrue(Signatures(legacy) == Signatures(seg), what);
		}

		TEST_METHOD(EachS2SlotSwitchesOffAndLeavesTheRestUnchanged)
		{
			for (Id slot : { Id::ColourSdrToneShape, Id::DriveLedMeasure, Id::DriveLedCompensation, Id::OutputCalibrationLut })
			{
				PipelineSlots::Registry registry;
				registry.SetEnabled(slot, false);
				CompareWithLegacy(registry, W(std::string("off: ") + PipelineSlots::Get(slot).name).c_str());
				// And the slot really adds nothing.
				Modules m(false);
				OutputSegments::ColourFrame f = Frame(63);
				std::vector<const pl_hook*> hooks;
				OutputSegments::OutputColourSegment s2(m.tone, m.dv, m.measure, m.comp, m.lut);
				const OutputSegments::ColourCounts c = s2.Collect(f, registry, hooks, [](size_t, Id) {});
				const size_t added = slot == Id::ColourSdrToneShape ? c.toneShape + c.dvTrims : slot == Id::DriveLedMeasure ? c.ledMeasure :
					slot == Id::DriveLedCompensation ? c.ledCompensation : c.lut;
				Assert::AreEqual<size_t>(0, added, W(std::string("hook added while off: ") + PipelineSlots::Get(slot).name).c_str());
			}
		}

		TEST_METHOD(OriginalPathwayAndPlaceholdersAreByteIdentical)
		{
			PipelineSlots::Registry original;
			original.ApplyOriginalPathway();
			CompareWithLegacy(original, L"Original pathway");
			// Placeholders (Stage 13, black level) switched on have no component: nothing changes.
			PipelineSlots::Registry placeholders;
			placeholders.SetEnabled(Id::ColourAppearance, true);
			placeholders.SetEnabled(Id::DriveBlackLevel, true);
			Assert::IsFalse(placeholders.Runs(Id::ColourAppearance) || placeholders.Runs(Id::DriveBlackLevel));
			CompareWithLegacy(placeholders, L"placeholders on");
			PipelineSlots::Registry designed;
			CompareWithLegacy(designed, L"designed order");
		}

		TEST_METHOD(S2FollowsAValidatedCustomOrder)
		{
			PipelineSlots::Registry registry;
			std::string reason;
			// LED measurement and compensation before the SDR tone shape (Stage 13 keeps following it).
			Assert::IsTrue(registry.SetOrder({ Id::DriveLedMeasure, Id::DriveLedCompensation, Id::ColourSdrToneShape, Id::ColourAppearance }, &reason),
				W(reason).c_str());
			// Equal to today's host reorder (38badcb0 tags the DV trims, so they move with their slot).
			CompareWithLegacy(registry, L"custom order");
			Modules m(false);
			OutputSegments::ColourFrame f = Frame(1 | 2 | 4 | 8 | 16 | 32);
			std::vector<const pl_hook*> hooks;
			OutputSegments::OutputColourSegment s2(m.tone, m.dv, m.measure, m.comp, m.lut);
			s2.Collect(f, registry, hooks, [](size_t, Id) {});
			Assert::IsTrue(hooks.size() >= 5);
			Assert::IsTrue(hooks[0] == m.measure.Hook(), L"the measurement runs first in this order");
			Assert::IsTrue(s2.SlotOf(hooks[1]) == Id::DriveLedCompensation, L"compensation right after the measurement");
			Assert::IsTrue(s2.SlotOf(hooks[2]) == Id::ColourSdrToneShape && s2.SlotOf(hooks[3]) == Id::ColourSdrToneShape,
				L"tone shape and DV trims together");
			// An invalid order is refused by the validator and changes nothing.
			Assert::IsFalse(registry.SetOrder({ Id::DriveLedCompensation, Id::DriveLedMeasure }, &reason));
		}

		TEST_METHOD(DitherSlotOffRemovesTheSigmaDelta)
		{
			Modules m(false);
			std::vector<const pl_hook*> hooks;
			m.user.Collect(false, hooks);   // the sigma-delta
			const size_t before = hooks.size();
			PipelineSlots::Registry registry;
			registry.SetEnabled(Id::OutputDither, false);
			OutputEncodingRepair::Hook repair;
			OcclusionMask::Hook mask;
			OutputSegments::OutputFinishSegment s3(repair, mask);
			SigmaDeltaDomain::Domain domain;
			const OutputSegments::DitherPrep prep = s3.PrepareDither(hooks, domain, true, registry);
			Assert::IsTrue(prep.found && prep.slotOff && !prep.domainDropped, L"found; removed because the slot is off");
			Assert::AreEqual<size_t>(before - 1, hooks.size(), L"not submitted while off");
			PipelineSlots::Registry on;
			std::vector<const pl_hook*> again;
			m.user.Collect(false, again);
			s3.PrepareDither(again, domain, true, on);
			Assert::AreEqual<size_t>(before, again.size());
		}

		// Merge audit (4): a domain drop engages "dither"; output.dither switched off never does.
		TEST_METHOD(DitherFallbackOnlyForDomainOrShaderNotForSlotOff)
		{
			Modules m(false);
			OutputEncodingRepair::Hook repair;
			OcclusionMask::Hook mask;
			OutputSegments::OutputFinishSegment s3(repair, mask);
			SigmaDeltaDomain::Domain unsupported;
			unsupported.supported = false;
			auto run = [&](bool slotOn, const SigmaDeltaDomain::Domain& domain)
			{
				PipelineSlots::Registry registry;
				registry.SetEnabled(Id::OutputDither, slotOn);
				std::vector<const pl_hook*> hooks;
				m.user.Collect(false, hooks);
				return s3.PrepareDither(hooks, domain, true, registry);
			};
			auto engaged = [](const OutputSegments::DitherPrep& prep, bool quantiserProblem)
			{
				const FallbackCatalog::Decision d = FallbackCatalog::Dither(OutputSegments::DitherFallbackRequested(prep, quantiserProblem),
					prep.domainDropped ? FallbackCatalog::DitherDrop::UnsupportedDomain :
					quantiserProblem ? FallbackCatalog::DitherDrop::ShaderNotLoaded : FallbackCatalog::DitherDrop::None);
				FallbackStatus::Registry fallbacks;
				FallbackCatalog::Sync(fallbacks, d);
				return fallbacks.Engaged("dither");
			};
			// Slot on, unsupported domain: dropped and engaged.
			const OutputSegments::DitherPrep domainDrop = run(true, unsupported);
			Assert::IsTrue(domainDrop.domainDropped && !domainDrop.slotOff);
			Assert::IsTrue(engaged(domainDrop, false), L"a domain drop is a fallback");
			// Slot on, shader problem: engaged.
			const OutputSegments::DitherPrep fine = run(true, SigmaDeltaDomain::Domain());
			Assert::IsFalse(fine.domainDropped);
			Assert::IsFalse(engaged(fine, false));
			Assert::IsTrue(engaged(fine, true), L"a shader problem is a fallback");
			// Slot off (any domain, even a shader problem): a choice, never a fallback.
			const OutputSegments::DitherPrep off = run(false, unsupported);
			Assert::IsTrue(off.slotOff && !off.domainDropped);
			Assert::IsFalse(engaged(off, false), L"output.dither off is not a fallback");
			Assert::IsFalse(engaged(off, true), L"output.dither off is not a fallback");
		}

		// Modularity LOW: the merged form accepts a validated custom order instead of refusing it.
		TEST_METHOD(MergedFormAcceptsAValidatedCustomOrder)
		{
			PipelineSlots::Registry registry;
			FallbackStatus::Registry fallbacks;
			std::string reason;
			Assert::IsTrue(registry.SetOrder({ Id::DriveLedMeasure, Id::DriveLedCompensation, Id::ColourSdrToneShape, Id::ColourAppearance }, &reason),
				W(reason).c_str());
			Segment s2(kS2FallbackId, OutputColourStages(), OutputColourEntry());
			Assert::IsTrue(s2.Configure(registry, true, 1, fallbacks) == Mode::Merged, W(s2.LastCheck().reason).c_str());
			const std::vector<StageInfo> active = s2.Active(registry);
			Assert::IsTrue(active.front().slot == Id::DriveLedMeasure, L"Active follows the registry order");
			Assert::AreEqual(std::string("Fallbacks engaged: none"), fallbacks.Heading());
		}

		// Modularity LOW: a placeholder slot with a component but no S2 stage is counted, not skipped silently.
		TEST_METHOD(PlaceholderWithAComponentIsCountedNotSilent)
		{
			Modules m(false);
			PipelineSlots::Registry registry;
			registry.SetComponent(Id::ColourAppearance, true);
			registry.SetEnabled(Id::ColourAppearance, true);
			std::vector<const pl_hook*> hooks, plain;
			OutputSegments::OutputColourSegment s2(m.tone, m.dv, m.measure, m.comp, m.lut);
			const OutputSegments::ColourCounts c = s2.Collect(Frame(63), registry, hooks, [](size_t, Id) {});
			Assert::AreEqual<size_t>(1, c.unhandled);
			PipelineSlots::Registry designed;
			const OutputSegments::ColourCounts d = s2.Collect(Frame(63), designed, plain, [](size_t, Id) {});
			Assert::AreEqual<size_t>(0, d.unhandled);
			Assert::IsTrue(hooks == plain, L"no hook added for it");
		}
	};
}
