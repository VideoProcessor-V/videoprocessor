// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only

// CameraExtension (link/CameraExtension.*): camera_link = report. Off is inert; report builds a
// CameraLinkCore per renderer over that renderer's frame-analysis store, allocates nothing (no vector
// engine yet: no thread, no pool, no ring), shows one status line and never feeds a consumer; a missing
// store is a visible fallback; teardown is per host. The extension is driven directly next to a real
// ResyncExtension (the shared seal). CPU only.

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include "CppUnitTest.h"

#include <VP-V/core/FrameAnalysisStore.h>
#include <VP-V/modules/input/signal/ResyncExtension.h>
#include <VP-V/modules/input/source_analysis/SourceFrameTap.h>
#include <VP-V/modules/motion/estimation/global_model/link/CameraExtension.h>
#include <VP-V/modules/motion/estimation/global_model/link/CameraLinkCore.h>

#include <atomic>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace CameraExtensionTestsNs
{
	namespace RE = RendererExtension;
	namespace Tap = VpFrameAnalysis::SourceFrameTap;
	using Vpv::Camera::CameraExtension;
	using Vpv::Resync::ResyncExtension;

	bool Has(const std::vector<std::string>& lines, const char* part)
	{
		for (const std::string& l : lines)
			if (l.find(part) != std::string::npos)
				return true;
		return false;
	}

	struct TempCfg
	{
		std::string path;
		explicit TempCfg(const std::string& text)
		{
			static std::atomic<int> counter{ 0 };
			char dir[MAX_PATH] = {};
			GetTempPathA(MAX_PATH, dir);
			path = std::string(dir) + "vp-camera-test-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(counter++) + ".cfg";
			std::ofstream out(path, std::ios::binary | std::ios::trunc);
			out << text;
		}
		~TempCfg() { std::remove(path.c_str()); }
		Vpv::Skeleton::ConfigLoader Loader() const
		{
			const std::string p = path;
			return [p](ConfigFile& c) { return c.Load(p, ConfigFile::ReadPolicy::Fresh); };
		}
	};

	struct FakeRenderer
	{
		VpResync::Shadow resync;
		VpResync::Drive resyncDrive;
		Vpv::Resync::HostBinding binding{ this, resync, resyncDrive };
		FrameAnalysis::Store store{ FrameAnalysis::RingDepth(4, 4) };
	};

	struct Rig
	{
		explicit Rig(const std::string& text, bool withStore = true) : cfg(text), ext(cfg.Loader())
		{
			if (withStore)
				Tap::SetStore(&r, &r.store);
		}
		~Rig()
		{
			Life(RE::Lifecycle::GpuReleased);
			Tap::SetStore(&r, nullptr);
		}

		TempCfg cfg;
		CameraExtension ext;
		ResyncExtension resync;
		FakeRenderer r;

		const void* Host() const { return &r; }
		void Life(RE::Lifecycle kind, int code = 0)
		{
			RE::LifecycleFacts f;
			f.host = &r; f.kind = kind; f.code = code;
			ext.OnLifecycle(f);
		}
		void ResyncApply(const char* mode)
		{
			RE::LifecycleFacts f;
			f.host = &r; f.kind = RE::Lifecycle::SettingsApplied; f.subject = mode; f.code = 0; f.value = 0;
			resync.OnLifecycle(f);
		}
		void Frame(int64_t seq)
		{
			RE::IngressContext c;
			c.host = &r; c.seq = seq; c.queueGeneration = 1; c.stateKey = 0x1000; c.formatKey = 7;
			c.frameCounter = seq; c.timestamp = seq * 1000; c.ticksPerSecond = 24000; c.measuredRateHz = 24.0;
			resync.OnIngress(c);
			RE::FrameContext f;
			f.host = &r; f.phase = RE::FramePhase::Begin; f.seq = seq;
			resync.OnFrame(f);
			Assert::IsFalse(ext.OnFrame(f), L"the camera extension never takes a frame over");
			f.phase = RE::FramePhase::AfterRender;
			Assert::IsFalse(ext.OnFrame(f));
		}
		std::vector<std::string> Lines(RE::StatusKind kind)
		{
			std::vector<std::string> lines;
			RE::Context c; c.host = &r;
			ext.OnStatus(c, kind, lines, nullptr, nullptr);
			return lines;
		}
	};

	TEST_CLASS(CameraExtensionTests)
	{
	public:
		TEST_METHOD(OffIsInert)
		{
			Rig rig("[general]\nx=1\n");
			rig.ResyncApply("on");
			rig.Life(RE::Lifecycle::GpuCreated);
			rig.Life(RE::Lifecycle::SettingsApplied);
			for (int64_t s = 1; s <= 4; ++s)
				rig.Frame(s);
			Assert::IsFalse(rig.ext.Active(rig.Host()));
			Assert::IsNull(rig.ext.CoreOf(rig.Host()));
			Assert::AreEqual<size_t>(0, rig.ext.HostStates());
			Assert::IsTrue(rig.Lines(RE::StatusKind::Lines).empty());
			Assert::IsTrue(rig.Lines(RE::StatusKind::Fallbacks).empty());
			Assert::AreEqual<size_t>(2, ResyncExtension::SealFor(rig.Host())->ProviderCount(), L"no reader provider was added");
		}

		TEST_METHOD(ReportShowsOneLineAndAllocatesNothing)
		{
			Rig rig("[vpv.skeleton]\ncamera_link=report\n");
			rig.ResyncApply("on");
			rig.Life(RE::Lifecycle::GpuCreated);
			Assert::IsTrue(rig.ext.Active(rig.Host()));
			CameraLink::CameraLinkCore* core = rig.ext.CoreOf(rig.Host());
			Assert::IsNotNull(core);
			for (int64_t s = 1; s <= 20; ++s)
				rig.Frame(s);
			Assert::IsFalse(core->Active(), L"the fit is not wanted: no vector engine yet");
			Assert::IsFalse(core->Allocated(), L"no producer, pool or ring");
			Assert::IsFalse(core->HasThread(), L"no thread");
			const std::vector<std::string> lines = rig.Lines(RE::StatusKind::Lines);
			Assert::IsTrue(Has(lines, "camera=report mode; fit idle (no vector engine yet, P5a pending); guided matching (K4) off"));
			Assert::IsTrue(rig.Lines(RE::StatusKind::Fallbacks).empty(), L"report mode with a store is not a downgrade");
			Assert::AreEqual<size_t>(3, ResyncExtension::SealFor(rig.Host())->ProviderCount(), L"the camera reader provider");
			Assert::IsNull(rig.ext.Query("vpv.camera.v1"), L"no consumer is fed");
			RE::Context c; c.host = rig.Host();
			std::string reply;
			Assert::IsTrue(rig.ext.OnCommand(c, "camera", "status", reply));
			Assert::IsTrue(reply.find("report mode") != std::string::npos);
			Assert::IsFalse(rig.ext.OnCommand(c, "skeleton", "status", reply));
		}

		TEST_METHOD(AMissingStoreIsAVisibleFallback)
		{
			Rig rig("[vpv.skeleton]\ncamera_link=report\n", false);
			rig.ResyncApply("on");
			rig.Life(RE::Lifecycle::GpuCreated);
			Assert::IsTrue(rig.ext.Active(rig.Host()));
			Assert::IsNull(rig.ext.CoreOf(rig.Host()));
			Assert::IsTrue(Has(rig.Lines(RE::StatusKind::Fallbacks), "camera_link: no frame analysis store for this renderer"));
			Assert::IsTrue(Has(rig.Lines(RE::StatusKind::Lines), "camera=report mode unavailable"));
		}

		TEST_METHOD(WithoutTheSharedSealItSaysSo)
		{
			// resync_event off: the seal is there but never driven; the provider still registers, nothing starts.
			Rig rig("[vpv.skeleton]\ncamera_link=report\n");
			rig.ResyncApply("off");
			rig.Life(RE::Lifecycle::GpuCreated);
			for (int64_t s = 1; s <= 3; ++s)
				rig.Frame(s);
			CameraLink::CameraLinkCore* core = rig.ext.CoreOf(rig.Host());
			Assert::IsNotNull(core);
			Assert::IsFalse(core->Allocated());
		}

		TEST_METHOD(OriginalPathwayForcesOff)
		{
			Rig rig("[vpv.skeleton]\ncamera_link=report\n");
			rig.Life(RE::Lifecycle::SettingsApplied, 1);
			Assert::IsFalse(rig.ext.Active(rig.Host()));
			Assert::IsNull(rig.ext.CoreOf(rig.Host()));
			Assert::AreEqual<size_t>(0, rig.ext.HostStates());
			rig.Life(RE::Lifecycle::SettingsApplied, 0);
			Assert::IsTrue(rig.ext.Active(rig.Host()));
			rig.Life(RE::Lifecycle::SettingsApplied, 1);
			Assert::IsFalse(rig.ext.Active(rig.Host()));
			Assert::IsNull(rig.ext.CoreOf(rig.Host()), L"torn down");
			Assert::IsTrue(rig.Lines(RE::StatusKind::Lines).empty());
		}

		TEST_METHOD(AnInvalidValueIsShownWhileOff)
		{
			Rig rig("[vpv.skeleton]\ncamera_link=sideways\n");
			rig.Life(RE::Lifecycle::GpuCreated);
			Assert::IsFalse(rig.ext.Active(rig.Host()));
			Assert::IsNull(rig.ext.CoreOf(rig.Host()));
			Assert::IsTrue(Has(rig.Lines(RE::StatusKind::Fallbacks), "camera_link: camera_link 'sideways'"));
			Assert::IsTrue(rig.Lines(RE::StatusKind::Lines).empty());
		}

		TEST_METHOD(TeardownIsPerHost)
		{
			Rig a("[vpv.skeleton]\ncamera_link=report\n");
			FakeRenderer other;
			const void* bh = &other;
			Tap::SetStore(bh, &other.store);
			a.ResyncApply("on");
			a.Life(RE::Lifecycle::GpuCreated);
			RE::LifecycleFacts f;
			f.host = bh; f.kind = RE::Lifecycle::GpuCreated;
			a.ext.OnLifecycle(f);
			Assert::IsTrue(a.ext.Active(a.Host()) && a.ext.Active(bh));
			Assert::IsTrue(a.ext.CoreOf(a.Host()) != a.ext.CoreOf(bh), L"one core per renderer");
			Assert::AreEqual<size_t>(2, a.ext.HostStates());

			a.Life(RE::Lifecycle::GpuReleased);
			Assert::IsFalse(a.ext.Active(a.Host()));
			Assert::IsNull(a.ext.CoreOf(a.Host()));
			Assert::IsTrue(a.ext.Active(bh));
			Assert::IsNotNull(a.ext.CoreOf(bh));
			Assert::AreEqual<size_t>(1, a.ext.HostStates());

			f.kind = RE::Lifecycle::GpuReleased;
			a.ext.OnLifecycle(f);
			Assert::AreEqual<size_t>(0, a.ext.HostStates());
			Tap::SetStore(bh, nullptr);
		}
	};
}
