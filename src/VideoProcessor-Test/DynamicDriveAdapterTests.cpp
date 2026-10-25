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
#include "LedDriveTestSupport.h"
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
			f.frame.PrepareFrame(facts.in, hooks, NoTag);
			Assert::IsTrue(hooks.empty(), L"the shader could not load");
			Assert::IsTrue(module->HoldingFullForCompensation());
			Assert::IsTrue(ReasonOf(f.fallbacks, LedDrive::LedDriveModule::kHoldFallbackId).find("did not load") != std::string::npos);
			// The slot switched off is a reason of its own.
			facts.in.compensationSlotRuns = false;
			hooks.clear();
			f.frame.PrepareFrame(facts.in, hooks, NoTag);
			Assert::IsTrue(ReasonOf(f.fallbacks, LedDrive::LedDriveModule::kHoldFallbackId).find("slot drive.led_compensation is off") != std::string::npos);
			// The module gone: its entries leave the heading.
			f.frame.Clear();
			hooks.clear();
			f.frame.PrepareFrame(facts.in, hooks, NoTag);
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
			FrameFacts facts;
			facts.in.measureSlotRuns = false;
			std::vector<const pl_hook*> hooks;
			std::vector<std::pair<const pl_hook*, PipelineSlots::Id>> tags;
			const auto record = [&](size_t start, PipelineSlots::Id id)
			{
				for (size_t i = start; i < hooks.size(); ++i)
					tags.emplace_back(hooks[i], id);
			};
			frame.PrepareFrame(facts.in, hooks, record);
			Assert::AreEqual(size_t{ 1 }, hooks.size(), L"the compensation hook runs, no measurement hook");
			Assert::AreEqual(size_t{ 1 }, tags.size());
			Assert::IsTrue(tags[0].second == PipelineSlots::Id::DriveLedCompensation, L"no DriveLedMeasure tag with the slot off");
			Assert::IsTrue(module->HoldingFullForCompensation());
			Assert::IsTrue(ReasonOf(fallbacks, LedDrive::LedDriveModule::kHoldFallbackId).find("held at full: measurement off") != std::string::npos);
			facts.in.measureSlotRuns = true;
			hooks.clear();
			tags.clear();
			frame.PrepareFrame(facts.in, hooks, record);
			Assert::AreEqual(size_t{ 2 }, hooks.size(), L"measurement + compensation");
			Assert::IsTrue(hooks[0] == frame.MeasurementHookPointer(), L"measurement first");
			Assert::AreEqual(size_t{ 2 }, tags.size());
			Assert::IsTrue(tags[0].first == frame.MeasurementHookPointer() && tags[0].second == PipelineSlots::Id::DriveLedMeasure);
			Assert::IsTrue(tags[1].second == PipelineSlots::Id::DriveLedCompensation);
			Assert::IsFalse(module->HoldingFullForCompensation());
			Assert::IsFalse(fallbacks.Engaged(LedDrive::LedDriveModule::kHoldFallbackId));
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
			f.frame.PrepareFrame(facts.in, hooks, NoTag);
			Assert::IsFalse(module->HoldingFullForCompensation());
			Assert::IsFalse(f.fallbacks.Engaged(LedDrive::LedDriveModule::kHoldFallbackId));
		}

		TEST_METHOD(MissingGammaBoostIsVisible)
		{
			FrameFixture f;
			LedDrive::DynamicDriveParams params;
			params.gammaBoost = 2.4;   // output gamma 2.2: exponent 2.2 / 2.4
			f.frame.PublishParams(params);
			FrameFacts facts;
			std::vector<const pl_hook*> hooks;
			f.frame.PrepareFrame(facts.in, hooks, NoTag);
			Assert::IsTrue(ReasonOf(f.fallbacks, LedDrive::DynamicDriveFrame::kBoostFallbackId).find("did not load") != std::string::npos);
			params.gammaBoost = 0.0;
			f.frame.PublishParams(params);
			f.frame.PrepareFrame(facts.in, hooks, NoTag);
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
			f.frame.PrepareFrame(facts.in, hooks, NoTag);
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
	};
}
