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
#include <Windows.h>
#include <fstream>
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
		LedDrive::DynamicDriveFrame frame{ observer, fallbacks, CustomHookChain::Loader{} };
	};
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
