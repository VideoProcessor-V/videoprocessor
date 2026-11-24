// WP-15 (integration batch 1): the Dynamic Drive host adapter (DynamicDriveHost, outer-renderer
// side; DynamicDriveFrame, render side). CPU only: no projector, no GPU, no serial port is opened
// (every configuration here either keeps the drive off or has the projector slot off).
#include "pch.h"
#include "CppUnitTest.h"
#include <VP-V/modules/drive/led_compensation/DynamicDriveFrame.h>
#include <VP-V/modules/drive/led_compensation/DynamicDriveHost.h>
#include <VP-V/modules/drive/led_compensation/LedDriveModule.h>
#include <ConfigFile.h>
#include <MainConfigSchema.h>
#include <VP-V/tests/support/LedDriveTestSupport.h>
#include <VP-V/segments/OutputSegments.h>
#include <VP-V/segments/OutputSegmentsMerge.h>
#include <VP-V/modules/output/calibration_lut/CalibrationLutStage.h>
#include <VP-V/modules/motion/consumers/OcclusionMaskHook.h>
#include <VP-V/modules/output/dither/OutputEncodingRepair.h>
#include <algorithm>
#include <atomic>
#include <Windows.h>
#include <chrono>
#include <fstream>
#include <functional>
#include <thread>
#include <memory>
#include <mutex>
#include <string>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace
{
	struct AdapterConfig
	{
		std::string path;
		std::shared_ptr<ConfigFile> config = std::make_shared<ConfigFile>();
		explicit AdapterConfig(const std::string& contents)
		{
			char directory[MAX_PATH]{}, file[MAX_PATH]{};
			GetTempPathA(MAX_PATH, directory);
			GetTempFileNameA(directory, "vpd", 0, file);
			path = file;
			std::ofstream(path, std::ios::trunc) << contents;
			Assert::IsTrue(config->Load(path));
		}
		~AdapterConfig() { DeleteFileA(path.c_str()); }
	};

	struct StubObserver : QueuedFrameStatistics::IObserver
	{
		void BeginWalk(size_t) override {}
		void ObserveFrame(uint64_t, size_t, const AnalysisLumaSource&, const QueuedFrameStatistics::PictureRect&) override {}
	};

	struct FrameFixture
	{
		std::shared_ptr<QueuedFrameStatistics::IObserver> observer;
		FallbackStatus::Registry fallbacks;
		// An empty loader: "LED compensation.glsl" cannot load (the shader-missing case).
		LedDrive::DynamicDriveFrame frame{ observer, fallbacks, CustomHookChain::Loader{} };
	};

	// The facts of one 1080p SDR frame with no subtitle and no NLS.
	struct FrameFacts
	{
		pl_frame image{};
		pl_frame target{};
		pl_render_params params = pl_render_default_params;
		CustomHookChain::TemplateValues templates;
		std::string range = "limited";
		HdrPeakAnalysisCrop::TrustedPicture trusted;
		LedDrive::DynamicDriveFrameInputs in;
		FrameFacts()
		{
			image.crop = { 0.0f, 0.0f, 1920.0f, 1080.0f };
			image.color = pl_color_space_bt709;
			target.crop = image.crop;
			target.color = pl_color_space_bt709;
			in.image = &image;
			in.target = &target;
			in.screen = &target;
			in.params = &params;
			in.hookTemplates = &templates;
			in.sourceRange = &range;
			in.displayGamma = 2.2;
			in.trustedPicture = &trusted;
			in.sourceSequence = 1;
			in.compensationSlotRuns = true;
		}
	};

	std::string ReasonOf(const FallbackStatus::Registry& registry, const char* id)
	{
		for (const FallbackStatus::Entry& e : registry.Entries())
			if (e.id == id)
				return e.reason;
		return std::string();
	}

	const auto NoTag = [](size_t, PipelineSlots::Id) {};

	// Integration group C (merge audit C1): the LED hooks reach the frame only through the S2
	// segment. One frame the way the host runs it: DecideFrame -> S2 Collect -> AfterCollect.
	struct S2Rig
	{
		CustomHookChain::Chain tone{ CustomHookChain::Loader{} };
		CustomHookChain::Chain dv{ CustomHookChain::Loader{} };
		CalibrationLutStage::Hook lut;
		PipelineSlots::Registry registry;
	};

	OutputSegments::ColourCounts RunS2(LedDrive::DynamicDriveFrame& frame, const LedDrive::DynamicDriveFrameInputs& in,
		std::vector<const pl_hook*>& hooks, const OutputSegments::TagFn& tag, S2Rig* rigIn = nullptr)
	{
		S2Rig local;
		S2Rig& rig = rigIn ? *rigIn : local;
		rig.registry.SetEnabled(PipelineSlots::Id::DriveLedMeasure, in.measureSlotRuns);
		rig.registry.SetEnabled(PipelineSlots::Id::DriveLedCompensation, in.compensationSlotRuns);
		OutputSegments::OutputColourSegment s2(rig.tone, rig.dv, frame.Measurement(), frame.Compensation(), rig.lut);
		OutputSegments::ColourFrame colour;
		frame.DecideFrame(in, colour);
		colour.calibrationLut = rig.lut.Lut() != nullptr;
		const OutputSegments::ColourCounts counts = s2.Collect(colour, rig.registry, hooks, tag);
		frame.AfterCollect(in, counts.ledCompensation, counts.ledCompensationLoaded);
		return counts;
	}

	bool WaitFor(const std::function<bool()>& condition, int timeoutMs = 4000)
	{
		const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
		while (std::chrono::steady_clock::now() < end)
		{
			if (condition())
				return true;
			std::this_thread::sleep_for(std::chrono::milliseconds(5));
		}
		return condition();
	}
}

namespace VideoProcessorTest
{
	TEST_CLASS(DynamicDriveAdapterTests)
	{
	public:
		TEST_METHOD(DisabledDriveStartsNoModuleButPublishesTheBoost)
		{
			std::mutex state;
			LedDrive::DynamicDriveHost host(state);
			FrameFixture f;
			LedDrive::HostContext context;
			context.frame = &f.frame;
			AdapterConfig cfg("[led_drive.default]\nenabled: false\ngamma_boost: 2.4\nknee: 0.1\n");
			host.ApplySnapshot(cfg.config, std::string(), context);
			Assert::IsFalse(static_cast<bool>(host.Module()));
			Assert::IsFalse(static_cast<bool>(f.frame.Module()));
			Assert::AreEqual(std::string("off"), host.StatusText());
			Assert::AreEqual(2.4, host.Params().gammaBoost, 1e-12);
			Assert::AreEqual(0.1, host.Params().knee, 1e-12);
			Assert::AreEqual(size_t{ 0 }, f.frame.WantedLookaheadFrames());
			// The render side received the hook parameters (the boost applies with the drive off).
			Assert::AreEqual(2.4, f.frame.PublishedParams().gammaBoost, 1e-12);
			Assert::AreEqual(0.1, f.frame.PublishedParams().knee, 1e-12);
		}

		TEST_METHOD(HoldoverOnlyForFileSources)
		{
			std::mutex state;
			LedDrive::DynamicDriveHost host(state);
			AdapterConfig cfg("[led_drive.default]\nenabled: false\n");
			host.ApplySnapshot(cfg.config, "file", LedDrive::HostContext());
			Assert::AreEqual(static_cast<uint32_t>(MainConfigSchema::FileLedHoldoverDefaultMs), host.HoldoverMs());
			host.ApplySnapshot(cfg.config, "capture", LedDrive::HostContext());
			Assert::AreEqual(uint32_t{ 0 }, host.HoldoverMs());
		}

		TEST_METHOD(ProjectorSlotOffStartsNoModule)
		{
			std::mutex state;
			LedDrive::DynamicDriveHost host(state);
			LedDrive::HostContext context;
			context.projectorAllowed = false;   // Original pathway / pipeline_slots_off
			AdapterConfig cfg("[projector.default]\nport: COM250\n[led_drive.default]\nenabled: true\nmode: compensate\n");
			host.ApplySnapshot(cfg.config, std::string(), context);
			Assert::IsFalse(static_cast<bool>(host.Module()));
			Assert::AreEqual(std::string("off"), host.StatusText());
		}

		TEST_METHOD(InvalidConfigurationTurnsTheBoostOff)
		{
			std::mutex state;
			LedDrive::DynamicDriveHost host(state);
			AdapterConfig good("[led_drive.default]\nenabled: false\ngamma_boost: 2.4\n");
			host.ApplySnapshot(good.config, std::string(), LedDrive::HostContext());
			Assert::AreEqual(2.4, host.Params().gammaBoost, 1e-12);
			AdapterConfig bad("[led_drive.default]\nenabled: false\ngamma_boost: 9\n");
			host.ApplySnapshot(bad.config, std::string(), LedDrive::HostContext());
			Assert::AreEqual(0.0, host.Params().gammaBoost, 1e-12);
		}

		TEST_METHOD(ConnectionTestSuspendAndResumeWithoutAModule)
		{
			std::mutex state;
			LedDrive::DynamicDriveHost host(state);
			FrameFixture f;
			LedDrive::HostContext context;
			context.frame = &f.frame;
			context.lastRenderStartedTick = GetTickCount64();   // video playing, but no module
			std::string reason;
			Assert::IsFalse(host.SuspendForTest(context, reason));
			Assert::IsFalse(reason.empty());
			context.lastRenderStartedTick = 0;
			Assert::IsTrue(host.SuspendForTest(context, reason));
			Assert::IsTrue(reason.empty());
			Assert::IsTrue(host.Suspended());
			// A settings apply while suspended keeps the drive stopped (and records the settings).
			AdapterConfig cfg("[led_drive.default]\nenabled: false\ngamma_boost: 2.2\n");
			host.ApplySnapshot(cfg.config, std::string(), context);
			Assert::AreEqual(0.0, host.Params().gammaBoost, 1e-12);
			host.ResumeAfterTest(context);
			Assert::IsFalse(host.Suspended());
			Assert::AreEqual(2.2, host.Params().gammaBoost, 1e-12);
			Assert::IsTrue(host.StopForShutdown(context));
		}

		TEST_METHOD(CalibrationSuspendWithoutAModuleIsANoOp)
		{
			std::mutex state;
			LedDrive::DynamicDriveHost host(state);
			Assert::IsFalse(host.SuspendForCalibration(LedDrive::HostContext()));
			Assert::IsFalse(host.Suspended());
			host.ResumeAfterCalibration(LedDrive::HostContext());
			Assert::IsFalse(host.Suspended());
		}

		TEST_METHOD(CompensationUnavailableReasons)
		{
			using LedDrive::DynamicDriveFrame;
			Assert::AreEqual(std::string(), DynamicDriveFrame::CompensationUnavailable(true, 1, false));
			Assert::AreEqual(std::string("slot drive.led_compensation is off"),
				DynamicDriveFrame::CompensationUnavailable(false, 0, false));
			Assert::AreEqual(std::string("the LED compensation shader did not load"),
				DynamicDriveFrame::CompensationUnavailable(true, 0, false));
			Assert::AreEqual(std::string("the LED compensation shader is disabled"),
				DynamicDriveFrame::CompensationUnavailable(true, 0, true));
		}

		TEST_METHOD(PictureOnlyMasksCoverTheSubtitleAndFollowNls)
		{
			pl_frame image{};
			image.crop = { 0.0f, 0.0f, 1920.0f, 1080.0f };
			SubtitleBoxResult subtitle;
			subtitle.detected = true;
			subtitle.bounds = { 600, 900, 1320, 980 };
			subtitle.sourcePanel = { 580, 890, 1340, 990 };
			LedDrive::DynamicDriveFrameInputs in;
			in.image = &image;
			in.subtitle = &subtitle;
			in.sourceWidth = 1920;
			in.sourceHeight = 1080;
			in.subtitlePadding = SubtitleBoxPadding(0);
			LedDrive::MeasurementRect masks[LedDrive::MAX_MASK_RECTS];
			LedDrive::PixelRect source[LedDrive::MAX_MASK_RECTS];
			Assert::AreEqual(2, LedDrive::DynamicDriveFrame::PictureOnlyMasks(in, masks, source));
			Assert::AreEqual(600.0, source[0].x0, 1e-9);
			Assert::AreEqual(600.0 / 1920.0, masks[0].x0, 1e-9);
			Assert::AreEqual(900.0 / 1080.0, masks[0].y0, 1e-9);
			// An NLS shader whose map is not known: the rows across the whole width, a little taller.
			in.nlsSubmitted = true;
			in.nlsMapping = [] { return NlsMaskMapping::Mapping(); };
			Assert::AreEqual(2, LedDrive::DynamicDriveFrame::PictureOnlyMasks(in, masks, source));
			Assert::AreEqual(0.0, masks[0].x0, 1e-12);
			Assert::AreEqual(1.0, masks[0].x1, 1e-12);
			Assert::AreEqual(900.0 / 1080.0 - 0.02, masks[0].y0, 1e-9);
			// No detection: nothing masked.
			subtitle.detected = false;
			Assert::AreEqual(0, LedDrive::DynamicDriveFrame::PictureOnlyMasks(in, masks, source));
		}

		// Module findings 2026-10-24 item 1 (the WP-15 wiring): compensate holds full drive while
		// nothing compensates, and the renderer's heading shows it.
		TEST_METHOD(CompensationUnavailableHoldsFullDriveVisibly)
		{
			auto projector = std::make_shared<FakeProjector>();
			LedDrive::LedDriveConfig config;
			config.enabled = true;
			config.mode = LedDrive::Mode::Compensate;
			LedDrive::ProjectorConfig port;
			port.protocol = "barco_ascii";
			auto module = std::make_shared<LedDrive::LedDriveModule>(port, config,
				[projector] { return std::make_unique<FakePort>(projector); });
			FrameFixture f;
			f.frame.PublishModule(module);
			FrameFacts facts;
			std::vector<const pl_hook*> hooks;
			RunS2(f.frame, facts.in, hooks, NoTag);
			Assert::IsTrue(hooks.empty(), L"the shader could not load");
			Assert::IsTrue(module->HoldingFullForCompensation());
			Assert::IsTrue(ReasonOf(f.fallbacks, LedDrive::LedDriveModule::kHoldFallbackId).find("did not load") != std::string::npos);
			// The slot switched off is a reason of its own.
			facts.in.compensationSlotRuns = false;
			hooks.clear();
			RunS2(f.frame, facts.in, hooks, NoTag);
			Assert::IsTrue(ReasonOf(f.fallbacks, LedDrive::LedDriveModule::kHoldFallbackId).find("slot drive.led_compensation is off") != std::string::npos);
			// The module gone: its entries leave the heading.
			f.frame.Clear();
			hooks.clear();
			RunS2(f.frame, facts.in, hooks, NoTag);
			Assert::IsFalse(f.fallbacks.Engaged(LedDrive::LedDriveModule::kHoldFallbackId));
		}

		// Owner decision 2026-10-25: slot drive.led_measure off -> compensate held at full, visibly;
		// slot back on -> normal.
		TEST_METHOD(MeasurementSlotOffHoldsFullDrive)
		{
			static pl_hook dummy{};   // a loaded "LED compensation.glsl" stand-in: the compensation runs
			dummy.stages = PL_HOOK_PRE_OUTPUT;   // a known stage (the chain rejects a stageless hook)
			CustomHookChain::Loader loader{
				[](const std::string&, std::string& source, std::string&) { source = "//!HOOK PREOUTPUT\n"; return true; },
				[](const std::string&) -> const pl_hook* { return &dummy; },
				[](const pl_hook*& hook) { hook = nullptr; } };
			std::shared_ptr<QueuedFrameStatistics::IObserver> observer;
			FallbackStatus::Registry fallbacks;
			LedDrive::DynamicDriveFrame frame(observer, fallbacks, loader);
			auto projector = std::make_shared<FakeProjector>();
			LedDrive::LedDriveConfig config;
			config.enabled = true;
			config.mode = LedDrive::Mode::Compensate;
			LedDrive::ProjectorConfig port;
			port.protocol = "barco_ascii";
			auto module = std::make_shared<LedDrive::LedDriveModule>(port, config,
				[projector] { return std::make_unique<FakePort>(projector); });
			frame.PublishModule(module);
			LedDrive::DynamicDriveParams nonIdentity;
			nonIdentity.gammaBoost = 2.4;
			frame.PublishParams(nonIdentity);
			FrameFacts facts;
			facts.in.measureSlotRuns = false;
			std::vector<const pl_hook*> hooks;
			std::vector<std::pair<const pl_hook*, PipelineSlots::Id>> tags;
			const auto record = [&](size_t start, PipelineSlots::Id id)
			{
				for (size_t i = start; i < hooks.size(); ++i)
					tags.emplace_back(hooks[i], id);
			};
			RunS2(frame, facts.in, hooks, record);
			Assert::AreEqual(size_t{ 1 }, hooks.size(), L"the compensation hook runs, no measurement hook");
			Assert::AreEqual(size_t{ 1 }, tags.size());
			Assert::IsTrue(tags[0].second == PipelineSlots::Id::DriveLedCompensation, L"no DriveLedMeasure tag with the slot off");
			Assert::IsTrue(module->HoldingFullForCompensation());
			Assert::IsTrue(ReasonOf(fallbacks, LedDrive::LedDriveModule::kHoldFallbackId).find("held at full: measurement off") != std::string::npos);
			facts.in.measureSlotRuns = true;
			hooks.clear();
			tags.clear();
			RunS2(frame, facts.in, hooks, record);
			Assert::AreEqual(size_t{ 2 }, hooks.size(), L"measurement + compensation");
			Assert::IsTrue(hooks[0] == frame.MeasurementHookPointer(), L"measurement first");
			Assert::AreEqual(size_t{ 2 }, tags.size());
			Assert::IsTrue(tags[0].first == frame.MeasurementHookPointer() && tags[0].second == PipelineSlots::Id::DriveLedMeasure);
			Assert::IsTrue(tags[1].second == PipelineSlots::Id::DriveLedCompensation);
			Assert::IsFalse(module->HoldingFullForCompensation());
			Assert::IsFalse(fallbacks.Engaged(LedDrive::LedDriveModule::kHoldFallbackId));
		}

		// Merge audit C1: each of the LED measurement, LED compensation and VP calibration LUT hooks
		// is emitted exactly once per frame (S2 is its only emitter), with output_segments_merged
		// off and on, and zero times with its slot off.
		TEST_METHOD(S2EmitsEachLedAndLutHookExactlyOnce)
		{
			static pl_hook dummy{};
			dummy.stages = PL_HOOK_PRE_OUTPUT;
			CustomHookChain::Loader loader{
				[](const std::string&, std::string& source, std::string&) { source = "//!HOOK PREOUTPUT\n"; return true; },
				[](const std::string&) -> const pl_hook* { return &dummy; },
				[](const pl_hook*& hook) { hook = nullptr; } };
			std::shared_ptr<QueuedFrameStatistics::IObserver> observer;
			FallbackStatus::Registry fallbacks;
			LedDrive::DynamicDriveFrame frame(observer, fallbacks, loader);
			auto projector = std::make_shared<FakeProjector>();
			LedDrive::LedDriveConfig config;
			config.enabled = true;
			config.mode = LedDrive::Mode::Compensate;
			LedDrive::ProjectorConfig port;
			port.protocol = "barco_ascii";
			frame.PublishModule(std::make_shared<LedDrive::LedDriveModule>(port, config,
				[projector] { return std::make_unique<FakePort>(projector); }));
			LedDrive::DynamicDriveParams nonIdentity;
			nonIdentity.gammaBoost = 2.4;
			frame.PublishParams(nonIdentity);
			pl_custom_lut lutData{};
			for (const bool merged : { false, true })
				for (int off = 0; off < 4; ++off)   // 0 none, 1 measure, 2 compensation, 3 LUT
				{
					S2Rig rig;
					rig.lut.SetLut(off == 3 ? nullptr : &lutData);   // TakeLut leaves the stage empty with its slot off
					rig.registry.SetEnabled(PipelineSlots::Id::OutputCalibrationLut, off != 3);
					FrameFacts facts;
					facts.in.measureSlotRuns = off != 1;
					facts.in.compensationSlotRuns = off != 2;
					std::vector<const pl_hook*> hooks;
					RunS2(frame, facts.in, hooks, NoTag, &rig);
					// The list libplacebo gets: the separate hooks, or (merged) the S2 merged hook.
					std::vector<const pl_hook*> drawn = hooks;
					if (merged)
					{
						OutputSegments::OutputColourSegment seg(rig.tone, rig.dv, frame.Measurement(), frame.Compensation(), rig.lut);
						OutputSegments::ColourFrame colour;
						FrameFacts again;
						again.in.measureSlotRuns = facts.in.measureSlotRuns;
						again.in.compensationSlotRuns = facts.in.compensationSlotRuns;
						std::vector<const pl_hook*> segHooks;
						frame.DecideFrame(again.in, colour);
						colour.calibrationLut = rig.lut.Lut() != nullptr;
						const auto counts = seg.Collect(colour, rig.registry, segHooks, NoTag);
						frame.AfterCollect(again.in, counts.ledCompensation, counts.ledCompensationLoaded);
						Assert::IsTrue(segHooks == hooks, L"the same S2 list each frame");
						OutputEncodingRepair::Hook repair;
						OcclusionMask::Hook mask;
						OutputSegments::OutputFinishSegment s3(repair, mask);
						s3.BeginFrame();
						OutputSegments::Merger merger(nullptr, seg, s3);
						merger.Configure(rig.registry, true, 0, fallbacks);
						pl_render_params params{};
						params.hooks = segHooks.data();
						params.num_hooks = static_cast<int>(segHooks.size());
						OutputSegments::Merger::Scope scope(&merger, params, nullptr);
						drawn.assign(params.hooks, params.hooks + params.num_hooks);
						std::vector<uint64_t> signatures;
						for (const pl_hook* h : drawn)
							if (std::find(segHooks.begin(), segHooks.end(), h) == segHooks.end())
								signatures.push_back(h->signature);
						if (!signatures.empty())
						{
							pl_render_errors errors{};
							errors.disabled_hooks = signatures.data();
							errors.num_disabled_hooks = static_cast<int>(signatures.size());
							// The inner hooks of each merged hook stand in its place for the count.
							std::vector<const pl_hook*> expanded;
							for (const auto& run : merger.Runner().DisabledRuns(&errors))
								expanded.insert(expanded.end(), run.begin(), run.end());
							for (const pl_hook* h : drawn)
								if (std::find(segHooks.begin(), segHooks.end(), h) != segHooks.end())
									expanded.push_back(h);
							drawn = expanded;
						}
					}
					const auto count = [&](const pl_hook* h) { return std::count(drawn.begin(), drawn.end(), h); };
					const std::wstring where = std::wstring(merged ? L"merged" : L"separate") + L", slot off " + std::to_wstring(off);
					Assert::AreEqual<long long>(off == 1 ? 0 : 1, count(frame.MeasurementHookPointer()), (L"measure: " + where).c_str());
					Assert::AreEqual<long long>(off == 2 ? 0 : 1, count(&dummy), (L"compensation: " + where).c_str());
					Assert::AreEqual<long long>(off == 3 ? 0 : 1, count(rig.lut.Get()), (L"LUT: " + where).c_str());
				}
		}

		TEST_METHOD(ScriptModeIsNotHeld)
		{
			auto projector = std::make_shared<FakeProjector>();
			LedDrive::LedDriveConfig config;
			config.enabled = true;
			config.mode = LedDrive::Mode::Script;
			LedDrive::ProjectorConfig port;
			port.protocol = "barco_ascii";
			auto module = std::make_shared<LedDrive::LedDriveModule>(port, config,
				[projector] { return std::make_unique<FakePort>(projector); });
			FrameFixture f;
			f.frame.PublishModule(module);
			FrameFacts facts;
			std::vector<const pl_hook*> hooks;
			RunS2(f.frame, facts.in, hooks, NoTag);
			Assert::IsFalse(module->HoldingFullForCompensation());
			Assert::IsFalse(f.fallbacks.Engaged(LedDrive::LedDriveModule::kHoldFallbackId));
		}

		// Bug hunt 31 H1: a rejected configuration (module and gamma boost off) is visible - the
		// "Dynamic Drive" fallback entry and the status line - and a valid Apply releases it.
		TEST_METHOD(RejectedConfigurationIsVisibleAndReleased)
		{
			std::mutex state;
			LedDrive::DynamicDriveHost host(state);
			FrameFixture f;
			LedDrive::HostContext context;
			context.frame = &f.frame;
			FrameFacts facts;
			std::vector<const pl_hook*> hooks;
			AdapterConfig bad("[led_drive.default]\nenabled: false\ngamma_boost: 9\n");
			host.ApplySnapshot(bad.config, std::string(), context);
			Assert::IsFalse(host.Rejection().empty());
			Assert::IsTrue(host.StatusText().find("configuration rejected") != std::string::npos);
			RunS2(f.frame, facts.in, hooks, NoTag);
			const std::string reason = ReasonOf(f.fallbacks, LedDrive::DynamicDriveFrame::kConfigFallbackId);
			Assert::IsTrue(reason.find("configuration rejected: ") == 0, L"engaged with the validation error");
			Assert::IsTrue(reason.find("LED drive and gamma boost off") != std::string::npos);
			// A new Impl (Attach) gets the same state.
			FrameFixture g;
			LedDrive::HostContext other;
			other.frame = &g.frame;
			host.Attach(other);
			RunS2(g.frame, facts.in, hooks, NoTag);
			Assert::IsTrue(g.fallbacks.Engaged(LedDrive::DynamicDriveFrame::kConfigFallbackId));
			// Valid again: released, status "off".
			AdapterConfig good("[led_drive.default]\nenabled: false\n");
			host.ApplySnapshot(good.config, std::string(), context);
			Assert::IsTrue(host.Rejection().empty());
			Assert::AreEqual(std::string("off"), host.StatusText());
			RunS2(f.frame, facts.in, hooks, NoTag);
			Assert::IsFalse(f.fallbacks.Engaged(LedDrive::DynamicDriveFrame::kConfigFallbackId));
			Assert::AreEqual(std::string(), LedDrive::DynamicDriveFrame::RejectionReason(std::string()));
		}

		TEST_METHOD(MissingGammaBoostIsVisible)
		{
			FrameFixture f;
			LedDrive::DynamicDriveParams params;
			params.gammaBoost = 2.4;   // output gamma 2.2: exponent 2.2 / 2.4
			f.frame.PublishParams(params);
			FrameFacts facts;
			std::vector<const pl_hook*> hooks;
			RunS2(f.frame, facts.in, hooks, NoTag);
			Assert::IsTrue(ReasonOf(f.fallbacks, LedDrive::DynamicDriveFrame::kBoostFallbackId).find("did not load") != std::string::npos);
			params.gammaBoost = 0.0;
			f.frame.PublishParams(params);
			RunS2(f.frame, facts.in, hooks, NoTag);
			Assert::IsFalse(f.fallbacks.Engaged(LedDrive::DynamicDriveFrame::kBoostFallbackId));
		}

		// Module findings item 2: the dry run (no port) reaches the renderer's heading.
		TEST_METHOD(DryRunShowsTheProjectorFallback)
		{
			auto projector = std::make_shared<FakeProjector>();
			projector->openFails = true;
			LedDrive::LedDriveConfig config;
			config.enabled = true;
			config.mode = LedDrive::Mode::Script;
			LedDrive::ProjectorConfig port;
			port.protocol = "barco_ascii";
			auto module = std::make_shared<LedDrive::LedDriveModule>(port, config,
				[projector] { return std::make_unique<FakePort>(projector); });
			module->Start();
			Assert::IsTrue(WaitFor([&] { return !module->FallbackEntries().empty(); }), L"the module ran as a dry run");
			FrameFixture f;
			f.frame.PublishModule(module);
			FrameFacts facts;
			std::vector<const pl_hook*> hooks;
			RunS2(f.frame, facts.in, hooks, NoTag);
			Assert::IsTrue(ReasonOf(f.fallbacks, LedDrive::LedDriveModule::kDryRunFallbackId).find("dry run") != std::string::npos);
			module->Stop();
		}

		TEST_METHOD(ObserverPublishingFollowsTheModule)
		{
			FrameFixture f;
			f.observer = std::make_shared<StubObserver>();
			// No module: onlyWhenWanted keeps the observer, otherwise it is cleared.
			f.frame.PublishObserver(nullptr, true);
			Assert::IsTrue(static_cast<bool>(std::atomic_load(&f.observer)));
			f.frame.PublishObserver(nullptr, false);
			Assert::IsFalse(static_cast<bool>(std::atomic_load(&f.observer)));
			f.frame.Clear();
			Assert::IsFalse(static_cast<bool>(f.frame.Module()));
		}

		// DD-PERF (GEM-200 / PERF-3): identity frames (gain == 1 && boost == 1 && !(shadowLift * veil > 0))
		// skip pushing LED compensation.glsl without engaging HoldingFullForCompensation, while non-identity
		// frames push the hook and a genuinely missing shader STILL engages HoldingFullForCompensation.
		TEST_METHOD(IdentityFrameSkipsCompensationHookWithoutHoldingFull)
		{
			static pl_hook dummy{};
			dummy.stages = PL_HOOK_PRE_OUTPUT;
			CustomHookChain::Loader loadedLoader{
				[](const std::string&, std::string& source, std::string&) { source = "//!HOOK PREOUTPUT\n"; return true; },
				[](const std::string&) -> const pl_hook* { return &dummy; },
				[](const pl_hook*& hook) { hook = nullptr; } };
			auto makeModule = []()
			{
				auto projector = std::make_shared<FakeProjector>();
				LedDrive::LedDriveConfig config;
				config.enabled = true;
				config.mode = LedDrive::Mode::Compensate;
				LedDrive::ProjectorConfig port;
				port.protocol = "barco_ascii";
				return std::make_shared<LedDrive::LedDriveModule>(port, config,
					[projector] { return std::make_unique<FakePort>(projector); });
			};

			// 1) Loaded shader + identity frame: hook skipped, not held at full.
			{
				std::shared_ptr<QueuedFrameStatistics::IObserver> observer;
				FallbackStatus::Registry fallbacks;
				LedDrive::DynamicDriveFrame frame(observer, fallbacks, loadedLoader);
				auto module = makeModule();
				frame.PublishModule(module);
				FrameFacts facts;
				facts.in.measureSlotRuns = true;
				std::vector<const pl_hook*> hooks;
				const auto counts = RunS2(frame, facts.in, hooks, NoTag);
				Assert::AreEqual<size_t>(0, counts.ledCompensation, L"identity frame skips compensation hook pass");
				Assert::AreEqual<size_t>(1, counts.ledCompensationLoaded, L"loaded shader count reported separately");
				Assert::IsTrue(std::find(hooks.begin(), hooks.end(), &dummy) == hooks.end());
				Assert::IsFalse(module->HoldingFullForCompensation(), L"identity skip must NOT hold full drive");
				Assert::IsFalse(fallbacks.Engaged(LedDrive::LedDriveModule::kHoldFallbackId));

				// shadowLift > 0 with veil == 0 -> lift == 0 -> still identity (matches LED compensation.glsl:181-183).
				LedDrive::DynamicDriveParams liftZeroVeil;
				liftZeroVeil.shadowLift = 0.25;
				frame.PublishParams(liftZeroVeil);
				hooks.clear();
				const auto c2 = RunS2(frame, facts.in, hooks, NoTag);
				Assert::AreEqual<size_t>(0, c2.ledCompensation, L"shadowLift * 0 is still identity");
				Assert::AreEqual<size_t>(1, c2.ledCompensationLoaded);
				Assert::IsFalse(module->HoldingFullForCompensation());

				// Non-identity (gammaBoost != displayGamma): hook IS pushed.
				LedDrive::DynamicDriveParams boosted;
				boosted.gammaBoost = 2.4;
				frame.PublishParams(boosted);
				hooks.clear();
				const auto c3 = RunS2(frame, facts.in, hooks, NoTag);
				Assert::AreEqual<size_t>(1, c3.ledCompensation, L"non-identity frame pushes compensation hook");
				Assert::AreEqual<size_t>(1, c3.ledCompensationLoaded);
				Assert::IsTrue(std::find(hooks.begin(), hooks.end(), &dummy) != hooks.end());
				Assert::IsFalse(module->HoldingFullForCompensation());
			}

			// 2) Missing shader + identity frame: STILL holds full drive visibly (no silent fallback!).
			{
				FrameFixture f;   // empty loader: LED compensation.glsl cannot load
				auto module = makeModule();
				f.frame.PublishModule(module);
				FrameFacts facts;
				facts.in.measureSlotRuns = true;
				std::vector<const pl_hook*> hooks;
				const auto counts = RunS2(f.frame, facts.in, hooks, NoTag);
				Assert::AreEqual<size_t>(0, counts.ledCompensation);
				Assert::AreEqual<size_t>(0, counts.ledCompensationLoaded);
				Assert::IsTrue(module->HoldingFullForCompensation(), L"missing shader must still hold full drive on identity frames");
				Assert::IsTrue(ReasonOf(f.fallbacks, LedDrive::LedDriveModule::kHoldFallbackId).find("did not load") != std::string::npos);
			}
		}

		// GEM-LED-EXTRANGE-HOST: DynamicDriveFrame::DecideFrame sets exact limited-range [-60/876, 955/876]
		// vs full-range [0.0, 1.0] bounds from target.repr, and OutputColourSegment::Collect forwards BOTH
		// "extended_floor" and "extended_ceiling" into the LED compensation hook parameters.
		TEST_METHOD(ExtendedRangeBoundsFollowTargetRangeAndForwardToHook)
		{
			pl_var_data floorData{}; floorData.f = -0.25f;
			pl_var_data ceilData{}; ceilData.f = 1.50f;
			pl_hook_par pars[2]{};
			pars[0].name = "extended_floor";
			pars[0].type = PL_VAR_FLOAT;
			pars[0].data = &floorData;
			pars[0].minimum.f = -0.5f;
			pars[0].maximum.f = 0.0f;
			pars[1].name = "extended_ceiling";
			pars[1].type = PL_VAR_FLOAT;
			pars[1].data = &ceilData;
			pars[1].minimum.f = 1.0f;
			pars[1].maximum.f = 2.0f;
			pl_hook dummy{};
			dummy.stages = PL_HOOK_PRE_OUTPUT;
			dummy.parameters = pars;
			dummy.num_parameters = 2;

			CustomHookChain::Loader loader{
				[](const std::string&, std::string& source, std::string&) { source = "//!HOOK PREOUTPUT\n"; return true; },
				[&dummy](const std::string&) -> const pl_hook* { return &dummy; },
				[](const pl_hook*& hook) { hook = nullptr; } };
			std::shared_ptr<QueuedFrameStatistics::IObserver> observer;
			FallbackStatus::Registry fallbacks;
			LedDrive::DynamicDriveFrame frame(observer, fallbacks, loader);
			// Non-identity boost so OutputColourSegment::Collect emits the compensation hook and forwards parameters.
			LedDrive::DynamicDriveParams boosted;
			boosted.gammaBoost = 2.4;
			frame.PublishParams(boosted);

			const double limitedFloor = -60.0 / 876.0;
			const double limitedCeil = 955.0 / 876.0;

			// 1. Limited-range targets (explicit RGB LIMITED, and YCbCr BT.709 UNKNOWN which guesses LIMITED): exact [-60/876, 955/876].
			const std::pair<enum pl_color_system, enum pl_color_levels> limitedCases[] = {
				{ PL_COLOR_SYSTEM_RGB, PL_COLOR_LEVELS_LIMITED },
				{ PL_COLOR_SYSTEM_BT_709, PL_COLOR_LEVELS_UNKNOWN }
			};
			for (const auto& lc : limitedCases)
			{
				FrameFacts facts;
				facts.target.repr.sys = lc.first;
				facts.target.repr.levels = lc.second;
				OutputSegments::ColourFrame colour;
				frame.DecideFrame(facts.in, colour);
				Assert::AreEqual(limitedFloor, colour.compensation.extendedFloor, 1e-12, L"limited extendedFloor");
				Assert::AreEqual(limitedCeil, colour.compensation.extendedCeiling, 1e-12, L"limited extendedCeiling");

				floorData.f = 0.0f;
				ceilData.f = 1.0f;
				std::vector<const pl_hook*> hooks;
				const auto counts = RunS2(frame, facts.in, hooks, NoTag);
				Assert::AreEqual(size_t{ 1 }, counts.ledCompensation, L"compensation hook emitted");
				Assert::AreEqual(limitedFloor, static_cast<double>(floorData.f), 1e-6, L"extended_floor forwarded for limited");
				Assert::AreEqual(limitedCeil, static_cast<double>(ceilData.f), 1e-6, L"extended_ceiling forwarded for limited");
			}

			// 2. Full-range targets (explicit RGB FULL, and RGB UNKNOWN which guesses FULL): exact [0.0, 1.0].
			const std::pair<enum pl_color_system, enum pl_color_levels> fullCases[] = {
				{ PL_COLOR_SYSTEM_RGB, PL_COLOR_LEVELS_FULL },
				{ PL_COLOR_SYSTEM_RGB, PL_COLOR_LEVELS_UNKNOWN }
			};
			for (const auto& fc : fullCases)
			{
				FrameFacts facts;
				facts.target.repr.sys = fc.first;
				facts.target.repr.levels = fc.second;
				OutputSegments::ColourFrame colour;
				frame.DecideFrame(facts.in, colour);
				Assert::AreEqual(0.0, colour.compensation.extendedFloor, 1e-12, L"full-range extendedFloor must be 0.0");
				Assert::AreEqual(1.0, colour.compensation.extendedCeiling, 1e-12, L"full-range extendedCeiling must be 1.0");

				// Seed hook params with limited defaults to prove OutputColourSegment::Collect actively overwrites them.
				floorData.f = static_cast<float>(limitedFloor);
				ceilData.f = static_cast<float>(limitedCeil);
				std::vector<const pl_hook*> hooks;
				const auto counts = RunS2(frame, facts.in, hooks, NoTag);
				Assert::AreEqual(size_t{ 1 }, counts.ledCompensation, L"compensation hook emitted");
				Assert::AreEqual(0.0, static_cast<double>(floorData.f), 1e-7, L"extended_floor forwarded as 0.0 for full-range");
				Assert::AreEqual(1.0, static_cast<double>(ceilData.f), 1e-7, L"extended_ceiling forwarded as 1.0 for full-range");
			}
		}

	private:
		// RAII teardown for DynamicDriveHost tests that start a dry-run (log_only / protocol: none)
		// module, ensuring worker threads and shared references are stopped even if an assertion fails.
		struct HostCleanupGuard
		{
			LedDrive::DynamicDriveHost& host;
			LedDrive::DynamicDriveShared* shared;
			HostCleanupGuard(LedDrive::DynamicDriveHost& hostIn, LedDrive::DynamicDriveShared* sharedIn = nullptr)
				: host(hostIn), shared(sharedIn)
			{
			}
			~HostCleanupGuard()
			{
				LedDrive::HostContext ctx;
				ctx.frame = shared;
				host.StopForShutdown(ctx);
				if (shared)
					shared->Clear();
			}
		};

	public:
		// S4-B1 / GEM-187: disabling drive.projector_control live (e.g. switch to Original pathway or
		// pipeline_slots_off = drive.projector_control) must stop a running module and clear DynamicDriveShared,
		// rather than returning early via Reconfigure().
		TEST_METHOD(DisablingProjectorSlotStopsRunningDynamicDrive)
		{
			std::mutex state;
			std::shared_ptr<QueuedFrameStatistics::IObserver> walkObserver;
			LedDrive::DynamicDriveShared shared(walkObserver);
			LedDrive::DynamicDriveHost host(state);
			HostCleanupGuard cleanup(host, &shared);

			AdapterConfig cfg(
				"[projector.default]\n"
				"protocol: none\n"
				"[led_drive.default]\n"
				"enabled: on\n"
				"mode: log_only\n");

			// Negative control: exact same config with projectorAllowed = true genuinely starts and publishes the module.
			LedDrive::HostContext allowedCtx;
			allowedCtx.frame = &shared;
			allowedCtx.projectorAllowed = true;
			host.ApplySnapshot(cfg.config, std::string(), allowedCtx);
			Assert::IsNotNull(host.Module().get(), L"negative control: enabled LED drive must start when projectorAllowed is true");
			Assert::IsNotNull(shared.Module().get(), L"negative control: running module must be published to DynamicDriveShared");

			// Fix proof: exact same config with projectorAllowed = false must stop and clear the running module.
			LedDrive::HostContext disallowedCtx;
			disallowedCtx.frame = &shared;
			disallowedCtx.projectorAllowed = false;
			host.ApplySnapshot(cfg.config, std::string(), disallowedCtx);
			Assert::IsNull(host.Module().get(), L"disabling drive.projector_control must stop a running Dynamic Drive module");
			Assert::IsNull(shared.Module().get(), L"disabling drive.projector_control must clear the published DynamicDriveShared module");
			Assert::AreEqual(std::string("off"), host.StatusText(), L"status must report off after projector slot is disallowed");
		}

		// S4-B1 / GEM-187: live toggle of [led_drive.default] enabled = off stops the running module
		// and clears DynamicDriveShared even when [projector.default] is unchanged.
		TEST_METHOD(DisablingLedDriveEnabledStopsRunningDynamicDrive)
		{
			std::mutex state;
			std::shared_ptr<QueuedFrameStatistics::IObserver> walkObserver;
			LedDrive::DynamicDriveShared shared(walkObserver);
			LedDrive::DynamicDriveHost host(state);
			HostCleanupGuard cleanup(host, &shared);

			LedDrive::HostContext context;
			context.frame = &shared;
			context.projectorAllowed = true;

			AdapterConfig enabledCfg(
				"[projector.default]\n"
				"protocol: none\n"
				"[led_drive.default]\n"
				"enabled: on\n"
				"mode: log_only\n");
			host.ApplySnapshot(enabledCfg.config, std::string(), context);
			Assert::IsNotNull(host.Module().get(), L"negative control: enabled = on must start Dynamic Drive");
			Assert::IsNotNull(shared.Module().get(), L"negative control: enabled = on must publish module to DynamicDriveShared");

			AdapterConfig disabledCfg(
				"[projector.default]\n"
				"protocol: none\n"
				"[led_drive.default]\n"
				"enabled: off\n"
				"mode: log_only\n");
			host.ApplySnapshot(disabledCfg.config, std::string(), context);
			Assert::IsNull(host.Module().get(), L"enabled = off must stop the running Dynamic Drive module");
			Assert::IsNull(shared.Module().get(), L"enabled = off must clear the published DynamicDriveShared module");
			Assert::AreEqual(std::string("off"), host.StatusText(), L"status must report off after enabled = off");
		}

		// S4-B1: cold startup before m_impl / UpdatePipelineSlots exists (frame == nullptr, projectorAllowed == true)
		// must honor [vpv] pipeline_original_pathway = on from the config and never start Dynamic Drive.
		TEST_METHOD(ColdStartupOriginalPathwayInConfigNeverStartsDynamicDrive)
		{
			std::mutex state;
			LedDrive::DynamicDriveHost host(state);
			HostCleanupGuard cleanup(host);

			LedDrive::HostContext coldContext;
			coldContext.frame = nullptr;
			coldContext.projectorAllowed = true;

			const std::string baseLedConfig =
				"[projector.default]\n"
				"protocol: none\n"
				"[led_drive.default]\n"
				"enabled: on\n"
				"mode: log_only\n";

			AdapterConfig originalCfg(baseLedConfig + "[vpv]\npipeline_original_pathway: on\n");
			host.ApplySnapshot(originalCfg.config, std::string(), coldContext);
			Assert::IsNull(host.Module().get(), L"cold startup with [vpv] pipeline_original_pathway = on must never start Dynamic Drive");
			Assert::AreEqual(std::string("off"), host.StatusText(), L"status must remain off under Original pathway");

			// Negative control: exact same [projector.default] / [led_drive.default] with pipeline_original_pathway = off starts the module.
			AdapterConfig vpvCfg(baseLedConfig + "[vpv]\npipeline_original_pathway: off\n");
			host.ApplySnapshot(vpvCfg.config, std::string(), coldContext);
			Assert::IsNotNull(host.Module().get(), L"negative control: exact same LED config with pipeline_original_pathway = off must start Dynamic Drive");

			AdapterConfig disabledCfg(
				"[projector.default]\n"
				"protocol: none\n"
				"[led_drive.default]\n"
				"enabled: off\n"
				"mode: log_only\n");
			host.ApplySnapshot(disabledCfg.config, std::string(), coldContext);
			Assert::IsNull(host.Module().get(), L"cleanup must stop the started module");
		}

		// S4-B1: cold startup before m_impl / UpdatePipelineSlots exists must honor drive.projector_control
		// in [vpv] pipeline_slots_off (including case-insensitive and whitespace-padded tokens).
		TEST_METHOD(ColdStartupProjectorSlotOffInConfigNeverStartsDynamicDrive)
		{
			std::mutex state;
			LedDrive::DynamicDriveHost host(state);
			HostCleanupGuard cleanup(host);

			LedDrive::HostContext coldContext;
			coldContext.frame = nullptr;
			coldContext.projectorAllowed = true;

			const std::string baseLedConfig =
				"[projector.default]\n"
				"protocol: none\n"
				"[led_drive.default]\n"
				"enabled: on\n"
				"mode: log_only\n";

			AdapterConfig slotOffCfg(baseLedConfig + "[vpv]\npipeline_slots_off: repair.deband, drive.projector_control\n");
			host.ApplySnapshot(slotOffCfg.config, std::string(), coldContext);
			Assert::IsNull(host.Module().get(), L"cold startup with drive.projector_control in pipeline_slots_off must never start Dynamic Drive");
			Assert::AreEqual(std::string("off"), host.StatusText(), L"status must remain off when drive.projector_control is listed off");

			// Case-insensitive and whitespace-padded token matching.
			AdapterConfig mixedCaseSlotOffCfg(baseLedConfig + "[vpv]\npipeline_slots_off:  repair.deband , \t Drive.Projector_Control  \n");
			host.ApplySnapshot(mixedCaseSlotOffCfg.config, std::string(), coldContext);
			Assert::IsNull(host.Module().get(), L"case-insensitive whitespace-padded Drive.Projector_Control must never start Dynamic Drive");
			Assert::AreEqual(std::string("off"), host.StatusText(), L"status must remain off for mixed-case drive.projector_control token");

			// Negative control: unrelated slot off (NOT drive.projector_control) on the exact same LED config starts the module.
			AdapterConfig otherSlotOffCfg(baseLedConfig + "[vpv]\npipeline_slots_off: repair.deband\n");
			host.ApplySnapshot(otherSlotOffCfg.config, std::string(), coldContext);
			Assert::IsNotNull(host.Module().get(), L"negative control: unrelated slot in pipeline_slots_off must still allow Dynamic Drive to start");

			AdapterConfig disabledCfg(
				"[projector.default]\n"
				"protocol: none\n"
				"[led_drive.default]\n"
				"enabled: off\n"
				"mode: log_only\n");
			host.ApplySnapshot(disabledCfg.config, std::string(), coldContext);
			Assert::IsNull(host.Module().get(), L"cleanup must stop the started module");
		}

		// S4-B1: Attach() with projectorAllowed == false must detach and stop any running module
		// rather than publishing it to the incoming DynamicDriveShared.
		TEST_METHOD(AttachWithProjectorDisallowedDetachesRunningModuleAndNeverPublishes)
		{
			std::mutex state;
			std::shared_ptr<QueuedFrameStatistics::IObserver> walkObserver;
			LedDrive::DynamicDriveShared shared(walkObserver);
			LedDrive::DynamicDriveHost host(state);
			HostCleanupGuard cleanup(host, &shared);

			AdapterConfig enabledCfg(
				"[projector.default]\n"
				"protocol: none\n"
				"[led_drive.default]\n"
				"enabled: on\n"
				"mode: log_only\n");

			// Negative control: start module while projectorAllowed = true (no frame yet).
			LedDrive::HostContext startCtx;
			startCtx.frame = nullptr;
			startCtx.projectorAllowed = true;
			host.ApplySnapshot(enabledCfg.config, std::string(), startCtx);
			Assert::IsNotNull(host.Module().get(), L"negative control: enabled LED drive must start when projectorAllowed is true");

			// Fix proof: Attach() with projectorAllowed = false must detach/stop the running module and never publish it.
			LedDrive::HostContext attachCtx;
			attachCtx.frame = &shared;
			attachCtx.projectorAllowed = false;
			host.Attach(attachCtx);
			Assert::IsNull(shared.Module().get(), L"Attach with projectorAllowed = false must never publish a running module to DynamicDriveShared");
			Assert::IsNull(host.Module().get(), L"Attach with projectorAllowed = false must detach and stop the running module on DynamicDriveHost");
			Assert::IsFalse(static_cast<bool>(std::atomic_load(&walkObserver)), L"Attach with projectorAllowed = false must not publish a look-ahead observer");
			Assert::AreEqual(std::string("off"), host.StatusText(), L"status must report off after Attach detaches the disallowed module");
		}

		// GEM-188: StatusText() and Module() read concurrently from another thread while the host
		// toggles ApplySnapshot() under stateMutex must never data-race, crash, throw, or return torn text.
		TEST_METHOD(ConcurrentStatusTextAndUpdateNeverRaceOrCrash)
		{
			struct ThreadJoinGuard
			{
				std::atomic<bool>& stopFlag;
				std::thread& worker;
				ThreadJoinGuard(std::atomic<bool>& stopFlagIn, std::thread& workerIn)
					: stopFlag(stopFlagIn), worker(workerIn)
				{
				}
				ThreadJoinGuard(const ThreadJoinGuard&) = delete;
				ThreadJoinGuard& operator=(const ThreadJoinGuard&) = delete;
				~ThreadJoinGuard()
				{
					stopFlag.store(true, std::memory_order_release);
					if (worker.joinable())
						worker.join();
				}
			};

			std::mutex state;
			std::shared_ptr<QueuedFrameStatistics::IObserver> walkObserver;
			LedDrive::DynamicDriveShared shared(walkObserver);
			LedDrive::DynamicDriveHost host(state);
			HostCleanupGuard cleanup(host, &shared);

			LedDrive::HostContext context;
			context.frame = &shared;
			context.projectorAllowed = true;

			AdapterConfig enabledCfg(
				"[projector.default]\n"
				"protocol: none\n"
				"[led_drive.default]\n"
				"enabled: on\n"
				"mode: log_only\n");
			AdapterConfig disabledCfg(
				"[projector.default]\n"
				"protocol: none\n"
				"[led_drive.default]\n"
				"enabled: off\n"
				"mode: log_only\n");

			// Negative control / baseline on the exact same configs: verify StatusText() distinguishes
			// "off" (disabled) from an active "source ..." line (enabled).
			{
				std::lock_guard<std::mutex> lock(state);
				host.ApplySnapshot(disabledCfg.config, std::string(), context);
			}
			Assert::IsNull(host.Module().get(), L"baseline: disabled config must not start a module");
			Assert::AreEqual(std::string("off"), host.StatusText(), L"baseline: disabled config must report \"off\"");
			{
				std::lock_guard<std::mutex> lock(state);
				host.ApplySnapshot(enabledCfg.config, std::string(), context);
			}
			Assert::IsNotNull(host.Module().get(), L"baseline: enabled config must start a module");
			const std::string enabledStatus = host.StatusText();
			Assert::IsTrue(enabledStatus.find("source ") != std::string::npos,
				L"baseline: enabled config StatusText() must contain \"source \"");
			Assert::AreNotEqual(std::string("off"), enabledStatus,
				L"baseline: enabled config StatusText() must not report \"off\"");
			{
				std::lock_guard<std::mutex> lock(state);
				host.ApplySnapshot(disabledCfg.config, std::string(), context);
			}
			Assert::AreEqual(std::string("off"), host.StatusText(), L"baseline: re-disabled config must return to \"off\"");

			std::atomic<bool> stopReader{ false };
			std::atomic<bool> readerReady{ false };
			std::atomic<size_t> statusReads{ 0 };
			std::atomic<size_t> invalidStatusReads{ 0 };
			std::atomic<bool> readerThrew{ false };

			std::thread reader([&]()
			{
				try
				{
					readerReady.store(true, std::memory_order_release);
					while (!stopReader.load(std::memory_order_acquire))
					{
						const std::string status = host.StatusText();
						const bool valid = (status == "off") || (status.find("source ") != std::string::npos);
						if (!valid)
							invalidStatusReads.fetch_add(1, std::memory_order_relaxed);
						const std::shared_ptr<LedDrive::LedDriveModule> mod = host.Module();
						if (mod)
							(void)mod->DrivingProjector();
						statusReads.fetch_add(1, std::memory_order_relaxed);
					}
				}
				catch (...)
				{
					readerThrew.store(true, std::memory_order_release);
				}
			});
			ThreadJoinGuard joinGuard(stopReader, reader);

			while (!readerReady.load(std::memory_order_acquire))
				std::this_thread::yield();

			constexpr int kToggleIterations = 60;
			for (int i = 0; i < kToggleIterations; ++i)
			{
				const auto& cfg = (i % 2 == 0) ? enabledCfg.config : disabledCfg.config;
				{
					std::lock_guard<std::mutex> lock(state);
					host.ApplySnapshot(cfg, std::string(), context);
				}
				std::this_thread::yield();
			}

			stopReader.store(true, std::memory_order_release);
			reader.join();

			Assert::IsFalse(readerThrew.load(std::memory_order_acquire),
				L"reader thread must not throw during concurrent StatusText() and Module() access");
			Assert::IsTrue(statusReads.load(std::memory_order_acquire) > 0,
				L"reader thread must observe StatusText()/Module() concurrently with ApplySnapshot() toggles");
			Assert::AreEqual<size_t>(0, invalidStatusReads.load(std::memory_order_acquire),
				L"every StatusText() observed concurrently must be either \"off\" or contain \"source \"");

			// Final iteration (i = 59, odd) applied disabledCfg: verify final state matches.
			Assert::IsNull(host.Module().get(), L"final host.Module() must match final disabled snapshot");
			Assert::IsNull(shared.Module().get(), L"final shared.Module() must match final disabled snapshot");
			Assert::AreEqual(std::string("off"), host.StatusText(), L"final host.StatusText() must match final disabled snapshot");
		}

		// GEM-190: full module replacement in Update() must never unlock the caller's stateMutex,
		// must stop/release the previous module and start a distinct live replacement module, and
		// disabling / SuspendForTest / host destruction must cleanly stop and join without hanging.
		TEST_METHOD(UpdateDoesNotBlockOnStopAndOrdersReplacementStartAfterPreviousStop)
		{
			struct ThreadJoinGuard
			{
				std::atomic<bool>& stopFlag;
				std::thread& worker;
				ThreadJoinGuard(std::atomic<bool>& stopFlagIn, std::thread& workerIn)
					: stopFlag(stopFlagIn), worker(workerIn)
				{
				}
				ThreadJoinGuard(const ThreadJoinGuard&) = delete;
				ThreadJoinGuard& operator=(const ThreadJoinGuard&) = delete;
				~ThreadJoinGuard()
				{
					stopFlag.store(true, std::memory_order_release);
					if (worker.joinable())
						worker.join();
				}
			};

			AdapterConfig initialCfg(
				"[projector.default]\n"
				"protocol: none\n"
				"min_command_gap_ms: 200\n"
				"[led_drive.default]\n"
				"enabled: on\n"
				"mode: log_only\n"
				"tick_ms: 50\n"
				"knee: 0.08\n");
			AdapterConfig reconfigCfg(
				"[projector.default]\n"
				"protocol: none\n"
				"min_command_gap_ms: 200\n"
				"[led_drive.default]\n"
				"enabled: on\n"
				"mode: log_only\n"
				"tick_ms: 50\n"
				"knee: 0.12\n");
			AdapterConfig replacedCfg(
				"[projector.default]\n"
				"protocol: none\n"
				"min_command_gap_ms: 300\n"
				"[led_drive.default]\n"
				"enabled: on\n"
				"mode: log_only\n"
				"tick_ms: 50\n"
				"knee: 0.12\n");
			AdapterConfig disabledCfg(
				"[projector.default]\n"
				"protocol: none\n"
				"min_command_gap_ms: 300\n"
				"[led_drive.default]\n"
				"enabled: off\n"
				"mode: log_only\n"
				"tick_ms: 50\n");

			std::weak_ptr<LedDrive::LedDriveModule> finalWeak;
			{
				std::mutex state;
				std::shared_ptr<QueuedFrameStatistics::IObserver> walkObserver;
				LedDrive::DynamicDriveShared shared(walkObserver);
				LedDrive::DynamicDriveHost host(state);
				HostCleanupGuard cleanup(host, &shared);

				LedDrive::HostContext context;
				context.frame = &shared;
				context.projectorAllowed = true;

				// 1. Start the initial running module.
				{
					std::lock_guard<std::mutex> lock(state);
					host.ApplySnapshot(initialCfg.config, std::string(), context);
				}
				std::shared_ptr<LedDrive::LedDriveModule> firstModule = host.Module();
				Assert::IsNotNull(firstModule.get(), L"initial enabled snapshot must start a module");
				Assert::IsTrue(shared.Module().get() == firstModule.get(), L"initial module must be published to DynamicDriveShared");
				Assert::IsTrue(host.StatusText().find("source ") != std::string::npos, L"initial module must report active status");

				// 2. Negative control: a reconfigurable tuning change (knee only, same connection params)
				// must keep the exact same LedDriveModule pointer in-place.
				{
					std::lock_guard<std::mutex> lock(state);
					host.ApplySnapshot(reconfigCfg.config, std::string(), context);
				}
				std::shared_ptr<LedDrive::LedDriveModule> afterReconfig = host.Module();
				Assert::IsNotNull(afterReconfig.get(), L"negative control: reconfigured module must remain active");
				Assert::IsTrue(afterReconfig.get() == firstModule.get(),
					L"negative control: non-replacing reconfigurable tuning change must keep the exact same LedDriveModule pointer");
				Assert::IsTrue(shared.Module().get() == firstModule.get(),
					L"negative control: DynamicDriveShared must keep the exact same LedDriveModule pointer");
				Assert::AreEqual(0.12, host.Params().knee, 1e-12,
					L"negative control: reconfigurable tuning change must update published params");

				// 3. Full module replacement (non-reconfigurable connection parameter min_command_gap_ms changed):
				// probe stateMutex from another thread while caller holds stateLock across ApplySnapshot()
				// to prove Update() NEVER unlocks stateMutex.
				{
					std::unique_lock<std::mutex> stateLock(state);
					std::atomic<bool> stopProbe{ false };
					std::atomic<bool> probeReady{ false };
					std::atomic<bool> observedUnlock{ false };
					std::atomic<size_t> probeAttempts{ 0 };

					std::thread lockProbe([&]()
					{
						probeReady.store(true, std::memory_order_release);
						while (!stopProbe.load(std::memory_order_acquire))
						{
							probeAttempts.fetch_add(1, std::memory_order_relaxed);
							if (state.try_lock())
							{
								state.unlock();
								observedUnlock.store(true, std::memory_order_release);
								break;
							}
							std::this_thread::yield();
						}
					});
					ThreadJoinGuard probeGuard(stopProbe, lockProbe);

					while (!probeReady.load(std::memory_order_acquire) ||
						probeAttempts.load(std::memory_order_acquire) == 0)
						std::this_thread::yield();

					host.ApplySnapshot(replacedCfg.config, std::string(), context);

					stopProbe.store(true, std::memory_order_release);
					lockProbe.join();

					Assert::IsTrue(stateLock.owns_lock(), L"caller stateMutex lock must remain owned across ApplySnapshot()");
					Assert::IsTrue(probeAttempts.load(std::memory_order_acquire) > 0,
						L"lock probe thread must have exercised stateMutex.try_lock() across ApplySnapshot()");
					Assert::IsFalse(observedUnlock.load(std::memory_order_acquire),
						L"Update() must NOT unlock the caller's stateMutex during module replacement");
				}

				// 4. Verify replacement module is a distinct live instance and the previous module stops/releases.
				std::shared_ptr<LedDrive::LedDriveModule> replacedModule = host.Module();
				Assert::IsNotNull(replacedModule.get(), L"replacement snapshot must publish a live module");
				Assert::IsTrue(replacedModule.get() != firstModule.get(),
					L"non-reconfigurable change must replace the module with a distinct LedDriveModule pointer");
				Assert::IsTrue(shared.Module().get() == replacedModule.get(),
					L"DynamicDriveShared must publish the replacement module");
				Assert::IsTrue(host.StatusText().find("source ") != std::string::npos,
					L"replacement module must report active status");

				const std::weak_ptr<LedDrive::LedDriveModule> firstWeak = firstModule;
				afterReconfig.reset();
				firstModule.reset();
				Assert::IsTrue(WaitFor([&] { return firstWeak.expired(); }),
					L"previous module must be stopped and released after replacement");

				LedDrive::RawMeasurement raw{};
				raw.histogram[128] = 100;
				raw.sum = 100u * 128u * LedDrive::SUM_SCALE;
				raw.blocks = 100u;
				raw.maximum = 128u * LedDrive::SUM_SCALE;
				replacedModule->SubmitMeasurement(raw, 1);
				Assert::IsTrue(WaitFor([&] { return replacedModule->Ticks() > 0; }),
					L"replacement module must start and run its worker tick loop after previous stop");

				// 5. Disabling must cleanly stop and release the replacement module.
				const std::weak_ptr<LedDrive::LedDriveModule> replacedWeak = replacedModule;
				replacedModule.reset();
				{
					std::lock_guard<std::mutex> lock(state);
					host.ApplySnapshot(disabledCfg.config, std::string(), context);
				}
				Assert::IsNull(host.Module().get(), L"disabling must clear host.Module()");
				Assert::IsNull(shared.Module().get(), L"disabling must clear shared.Module()");
				Assert::AreEqual(std::string("off"), host.StatusText(), L"disabling must report \"off\"");
				Assert::IsTrue(WaitFor([&] { return replacedWeak.expired(); }),
					L"disabling must cleanly stop and release the replacement module");

				// 6. Re-enable, SuspendForTest, ResumeAfterTest, then verify destruction joins cleanly without hanging.
				{
					std::lock_guard<std::mutex> lock(state);
					host.ApplySnapshot(replacedCfg.config, std::string(), context);
				}
				std::shared_ptr<LedDrive::LedDriveModule> suspendTarget = host.Module();
				Assert::IsNotNull(suspendTarget.get(), L"re-enabling must start a fresh module");
				const std::weak_ptr<LedDrive::LedDriveModule> suspendWeak = suspendTarget;
				suspendTarget.reset();

				std::string reason;
				Assert::IsTrue(host.SuspendForTest(context, reason), L"SuspendForTest must succeed when no video is playing");
				Assert::IsTrue(reason.empty(), L"SuspendForTest must return an empty failure reason on success");
				Assert::IsTrue(host.Suspended(), L"host must report Suspended() after SuspendForTest");
				Assert::IsNull(host.Module().get(), L"SuspendForTest must clear host.Module()");
				Assert::IsNull(shared.Module().get(), L"SuspendForTest must clear shared.Module()");
				Assert::IsTrue(WaitFor([&] { return suspendWeak.expired(); }),
					L"SuspendForTest must cleanly stop and release the running module");

				host.ResumeAfterTest(context);
				Assert::IsFalse(host.Suspended(), L"ResumeAfterTest must clear Suspended()");
				std::shared_ptr<LedDrive::LedDriveModule> resumedModule = host.Module();
				Assert::IsNotNull(resumedModule.get(), L"ResumeAfterTest must restart the module");
				finalWeak = resumedModule;
				resumedModule.reset();
			}

			Assert::IsTrue(finalWeak.expired(),
				L"destroying DynamicDriveHost must cleanly stop, join, and release its module without hanging or leaking");
		}

		// GEM-185: DynamicDriveFrame::MergeModuleFallbacks must surface ALL entries from
		// LedDriveModule::FallbackEntries() (not only kHoldFallbackId and kDryRunFallbackId)
		// into FallbackStatus::Registry::Heading(), and release them when cleared or removed.
		TEST_METHOD(MergeModuleFallbacksSurfacesAllModuleFallbackEntriesIncludingNonHoldNonDryRun)
		{
			struct ModuleStopGuard
			{
				std::shared_ptr<LedDrive::LedDriveModule> mod;
				TestClock* clock;
				ModuleStopGuard(std::shared_ptr<LedDrive::LedDriveModule> modIn, TestClock* clockIn)
					: mod(std::move(modIn)), clock(clockIn)
				{
				}
				ModuleStopGuard(const ModuleStopGuard&) = delete;
				ModuleStopGuard& operator=(const ModuleStopGuard&) = delete;
				~ModuleStopGuard()
				{
					if (mod)
					{
						if (clock && !mod->DrivingProjector())
							clock->To(clock->Ms() + 60000.0);
						mod->Stop();
					}
				}
			};

			auto advanceUntilReady = [](TestClock& clock, LedDrive::LedDriveModule& mod) -> bool
			{
				const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
				while (std::chrono::steady_clock::now() < deadline)
				{
					if (mod.DrivingProjector() && clock.clock->SettledCount() >= 1)
						return true;
					if (clock.clock->SettledCount() >= 1)
						clock.To(clock.Ms() + 50.0);
					else
						std::this_thread::sleep_for(std::chrono::milliseconds(2));
				}
				return mod.DrivingProjector() && clock.clock->SettledCount() >= 1;
			};

			auto hasModuleEntry = [](const std::vector<FallbackStatus::Entry>& entries, const char* id) -> bool
			{
				return std::any_of(entries.begin(), entries.end(),
					[id](const FallbackStatus::Entry& e) { return e.id == id; });
			};

			auto makePortConfig = []()
			{
				LedDrive::ProjectorConfig port;
				port.protocol = "barco_ascii";
				port.terminator = LedDrive::Terminator::Auto;
				port.readBack = true;
				port.ackTimeoutMs = 40;
				port.minCommandGapMs = 0;
				port.maxLevelsPerCommand = 0;
				return port;
			};

			auto makeLedConfig = []()
			{
				LedDrive::LedDriveConfig cfg;
				cfg.enabled = true;
				cfg.mode = LedDrive::Mode::Script;
				return cfg;
			};

			FrameFixture f;
			FrameFacts facts;
			std::vector<const pl_hook*> hooks;

			// 1. Negative control: an acknowledging projector with known scenes has no module
			// fallback entries; MergeModuleFallbacks must keep fallbacks.Heading() clean.
			TestClock cleanClock;
			auto cleanProjector = std::make_shared<FakeProjector>();
			auto cleanModule = std::make_shared<LedDrive::LedDriveModule>(makePortConfig(), makeLedConfig(),
				[cleanProjector] { return std::make_unique<FakePort>(cleanProjector); }, cleanClock);
			Bind(cleanClock, *cleanModule, cleanProjector);
			ModuleStopGuard cleanGuard{ cleanModule, &cleanClock };
			SceneVerdicts verdicts;
			cleanModule->SetSceneReaderForTesting(verdicts.Reader());
			cleanModule->Start();
			Assert::IsTrue(advanceUntilReady(cleanClock, *cleanModule),
				L"negative control: acknowledging module must finish startup");
			Assert::IsTrue(cleanModule->FallbackEntries().empty(),
				L"negative control: acknowledging module must have empty FallbackEntries()");

			f.frame.PublishModule(cleanModule);
			RunS2(f.frame, facts.in, hooks, NoTag);
			Assert::IsTrue(f.fallbacks.Entries().empty(),
				L"negative control: clean module must engage no fallbacks in registry");
			Assert::AreEqual(std::string("Fallbacks engaged: none"), f.fallbacks.Heading(),
				L"negative control: clean module heading must report none");
			Assert::IsFalse(f.fallbacks.Engaged(LedDrive::LedDriveModule::kFramingFallbackId),
				L"negative control: clean module must not engage projector framing fallback");
			Assert::IsTrue(f.fallbacks.Heading().find("projector framing") == std::string::npos,
				L"negative control: clean module heading must not contain projector framing");

			// 2. Fix proof A (non-hold / non-dry-run cut-judge entry): engaging kCutJudgeFallbackId
			// on the module must be merged into fallbacks.Heading() and released when the module clears it.
			LedDrive::RawMeasurement raw{};
			raw.histogram[128] = 100;
			raw.sum = 100u * 128u * LedDrive::SUM_SCALE;
			raw.blocks = 100u;
			raw.maximum = 128u * LedDrive::SUM_SCALE;
			verdicts.Unknown(1, "no frame analysis store");
			cleanModule->SubmitMeasurement(raw, 1);
			Assert::IsTrue(hasModuleEntry(cleanModule->FallbackEntries(), LedDrive::LedDriveModule::kCutJudgeFallbackId),
				L"module must carry non-hold/non-dry-run cut-judge entry in FallbackEntries()");
			hooks.clear();
			RunS2(f.frame, facts.in, hooks, NoTag);
			Assert::IsTrue(f.fallbacks.Engaged(LedDrive::LedDriveModule::kCutJudgeFallbackId),
				L"MergeModuleFallbacks must engage kCutJudgeFallbackId in renderer registry");
			Assert::IsTrue(f.fallbacks.Heading().find(LedDrive::LedDriveModule::kCutJudgeFallbackId) != std::string::npos,
				L"fallbacks.Heading() must surface kCutJudgeFallbackId");

			// Recover scene knowledge on next frame -> module releases kCutJudgeFallbackId -> heading releases it.
			cleanModule->SubmitMeasurement(raw, 2);
			Assert::IsFalse(hasModuleEntry(cleanModule->FallbackEntries(), LedDrive::LedDriveModule::kCutJudgeFallbackId),
				L"module must release cut-judge entry once scene is known");
			hooks.clear();
			RunS2(f.frame, facts.in, hooks, NoTag);
			Assert::IsFalse(f.fallbacks.Engaged(LedDrive::LedDriveModule::kCutJudgeFallbackId),
				L"MergeModuleFallbacks must release kCutJudgeFallbackId once module clears it");
			Assert::AreEqual(std::string("Fallbacks engaged: none"), f.fallbacks.Heading(),
				L"fallbacks.Heading() must return to none after cut-judge entry clears");

			// 3. Fix proof B (non-hold / non-dry-run framing-downgrade entry via silent port):
			// exact same config with silent projector triggers auto-framing downgrade ->
			// FallbackEntries() carries kFramingFallbackId ("projector framing"), which
			// MergeModuleFallbacks must surface in fallbacks.Heading() and release on Clear().
			TestClock silentClock;
			auto silentProjector = std::make_shared<FakeProjector>();
			silentProjector->silent = true;
			auto silentModule = std::make_shared<LedDrive::LedDriveModule>(makePortConfig(), makeLedConfig(),
				[silentProjector] { return std::make_unique<FakePort>(silentProjector); }, silentClock);
			Bind(silentClock, *silentModule, silentProjector);
			ModuleStopGuard silentGuard{ silentModule, &silentClock };
			silentModule->Start();
			Assert::IsTrue(advanceUntilReady(silentClock, *silentModule),
				L"silent-port module must finish auto-framing downgrade startup");
			Assert::IsTrue(hasModuleEntry(silentModule->FallbackEntries(), LedDrive::LedDriveModule::kFramingFallbackId),
				L"silent-port auto-framing downgrade must populate kFramingFallbackId in FallbackEntries()");

			f.frame.PublishModule(silentModule);
			hooks.clear();
			RunS2(f.frame, facts.in, hooks, NoTag);
			Assert::IsFalse(f.fallbacks.Engaged(LedDrive::LedDriveModule::kHoldFallbackId),
				L"script-mode silent module must not rely on kHoldFallbackId");
			Assert::IsFalse(f.fallbacks.Engaged(LedDrive::LedDriveModule::kDryRunFallbackId),
				L"open silent port must not rely on kDryRunFallbackId");
			Assert::IsTrue(f.fallbacks.Engaged(LedDrive::LedDriveModule::kFramingFallbackId),
				L"MergeModuleFallbacks must engage kFramingFallbackId in renderer registry");
			Assert::IsTrue(f.fallbacks.Heading().find("projector framing") != std::string::npos,
				L"fallbacks.Heading() must contain \"projector framing\" after MergeModuleFallbacks");
			Assert::IsFalse(ReasonOf(f.fallbacks, LedDrive::LedDriveModule::kFramingFallbackId).empty(),
				L"merged projector framing fallback must carry a non-empty reason");

			// Clear the module from DynamicDriveFrame -> next frame must release "projector framing".
			f.frame.Clear();
			hooks.clear();
			RunS2(f.frame, facts.in, hooks, NoTag);
			Assert::IsFalse(f.fallbacks.Engaged(LedDrive::LedDriveModule::kFramingFallbackId),
				L"clearing the module must release kFramingFallbackId from renderer registry");
			Assert::IsTrue(f.fallbacks.Heading().find("projector framing") == std::string::npos,
				L"fallbacks.Heading() must release \"projector framing\" after Clear()");
			Assert::AreEqual(std::string("Fallbacks engaged: none"), f.fallbacks.Heading(),
				L"fallbacks.Heading() must return to none after Clear()");
		}
	};
}
