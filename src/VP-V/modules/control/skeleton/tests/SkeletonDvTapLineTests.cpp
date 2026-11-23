// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only

// Slice 21 W2(iii): the Ctrl+I Fallbacks line for a refused DV 12-bit tap reads the VpFrameAnalysis::DvTapRefusals counter
// (SkeletonModule.cpp, the skeleton_lane fallback block). A new file with its own minimal rig: the rig in SkeletonModuleTests.cpp
// is file-local and that file is not edited. CPU only (the lane's own worker threads run).

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include "CppUnitTest.h"

#include <VP-V/core/FallbackCatalog.h>
#include <VP-V/core/hub/HostState.h>
#include <VP-V/core/FrameAnalysisStore.h>
#include <VP-V/core/FramesInFlight.h>
#include <VP-V/core/PostPresentSpare.h>
#include <VP-V/core/VramUsers.h>
#include <VP-V/modules/control/skeleton/SkeletonModule.h>
#include <VP-V/modules/input/signal/ResyncExtension.h>
#include <VP-V/modules/input/source_analysis/SourceFrameTap.h>
#include <VP-V/modules/input/source_analysis/VpFrameAnalysis.h>
#include <VP-V/modules/input/source_analysis/VpFrameAnalysisHost.h>

#include <atomic>
#include <cstdio>
#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace SkeletonDvTapLineTestsNs
{
	namespace RE = RendererExtension;
	namespace Tap = VpFrameAnalysis::SourceFrameTap;
	using Vpv::Resync::ResyncExtension;
	using Vpv::Skeleton::SkeletonModule;
	using VpSkeleton::SourceFrameView;

	constexpr uint32_t kW = 64;
	constexpr uint32_t kH = 48;

	// The exact Ctrl+I line text for N refused frames (SkeletonModule.cpp: the W21-1 refusal entry).
	std::string RefusalLine(unsigned long long frames)
	{
		return "skeleton_lane: analysis tap: 10-bit (DV 12-bit tap refused: " + std::to_string(frames) +
			" frame(s), no memory for the 12-bit plane)";
	}

	// A configuration file in the temp directory, read fresh by the injected loader.
	struct TempCfg
	{
		std::string path;
		explicit TempCfg(const std::string& text)
		{
			static std::atomic<int> counter{ 0 };
			char dir[MAX_PATH] = {};
			GetTempPathA(MAX_PATH, dir);
			path = std::string(dir) + "vp-skeleton-dvtap-test-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(counter++) + ".cfg";
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
	};

	// One fake renderer driven through the resync extension (the seal) and the skeleton module, as in SkeletonModuleTests.
	struct Rig
	{
		Rig()
			: cfg("[vpv.skeleton]\nanalysis_lane=on\n"), sk(cfg.Loader(), [this]() { return base.load() + calls.fetch_add(1); }), store(64 * kW * kH)
		{
			uint32_t x = 777u;
			for (size_t i = 0; i < store.size(); ++i)
			{
				x = x * 1664525u + 1013904223u;
				store[i] = uint16_t((x >> 12) & 1023u);   // 10-bit sampler codes
			}
		}

		~Rig() { Life(RE::Lifecycle::GpuReleased); }

		TempCfg cfg;
		std::atomic<uint64_t> base{ 1000 };
		std::atomic<uint64_t> calls{ 0 };
		SkeletonModule sk;
		ResyncExtension resync;
		FakeRenderer r;
		std::vector<uint16_t> store;

		void Life(RE::Lifecycle kind)
		{
			RE::LifecycleFacts f;
			f.host = &r; f.kind = kind; f.code = 0; f.value = 0.0;
			sk.OnLifecycle(f);
		}

		void ResyncApply(const char* mode)
		{
			RE::LifecycleFacts f;
			f.host = &r; f.kind = RE::Lifecycle::SettingsApplied; f.subject = mode; f.code = 0; f.value = 0;
			resync.OnLifecycle(f);
		}

		SourceFrameView View(uint64_t seq) const
		{
			SourceFrameView v;
			v.seq = seq;
			v.luma = store.data() + size_t(seq % 64) * kW * kH;
			v.width = kW;
			v.height = kH;
			v.strideSamples = kW;
			v.msbShift = 6;
			return v;
		}

		void Frame(int64_t seq)
		{
			RE::IngressContext c;
			c.host = &r; c.seq = seq; c.queueGeneration = 1; c.stateKey = 0x1000; c.formatKey = 7;
			c.frameCounter = seq; c.timestamp = seq * 1000; c.ticksPerSecond = 24000; c.measuredRateHz = 24.0;
			resync.OnIngress(c);
			if (seq == 1)
				Tap::Deliver(&r, View(1));
			Tap::Deliver(&r, View(uint64_t(seq) + 1));
			base += 40000;   // 25 fps
			RE::FrameContext f;
			f.host = &r; f.phase = RE::FramePhase::Begin; f.seq = seq;
			resync.OnFrame(f);
			Assert::IsFalse(sk.OnFrame(f), L"the skeleton never takes a frame over");
			f.phase = RE::FramePhase::AfterRender;
			Assert::IsFalse(sk.OnFrame(f));
		}

		// The frames the lane needs before the status reads the fallback block (SkeletonModuleTests: 12 frames after a flush).
		void Run()
		{
			ResyncApply("on");
			Life(RE::Lifecycle::GpuCreated);
			RE::QueueFacts q; q.host = &r; q.kind = RE::QueueEvent::Flush; q.generation = 1; q.frames = 8; q.text = "start";
			resync.OnQueue(q);
			sk.OnQueue(q);
			for (int64_t s = 1; s <= 12; ++s)
				Frame(s);
		}

		std::vector<std::string> Lines(RE::StatusKind kind)
		{
			std::vector<std::string> lines;
			RE::Context c; c.host = &r;
			sk.OnStatus(c, kind, lines, nullptr, nullptr);
			return lines;
		}
	};

	bool HasExact(const std::vector<std::string>& lines, const std::string& text)
	{
		for (const std::string& l : lines)
			if (l == text)
				return true;
		return false;
	}

	bool HasPart(const std::vector<std::string>& lines, const char* part)
	{
		for (const std::string& l : lines)
			if (l.find(part) != std::string::npos)
				return true;
		return false;
	}

	// Resets the process-global refusal counter on every exit, including a failed assert.
	struct RefusalGuard
	{
		RefusalGuard() { VpFrameAnalysis::ResetDvTapRefusals(); }
		~RefusalGuard() { VpFrameAnalysis::ResetDvTapRefusals(); }
	};

	TEST_CLASS(SkeletonDvTapLineTests)
	{
	public:
		// The line reads the counter: absent at 0, exact text at 1 and at 2 (a hard-coded count or a constant line fails one of them).
		TEST_METHOD(RefusalLineReadsTheCounter)
		{
			RefusalGuard guard;
			for (unsigned long long count = 0; count <= 2; ++count)
			{
				VpFrameAnalysis::ResetDvTapRefusals();
				for (unsigned long long i = 0; i < count; ++i)
					VpFrameAnalysis::NoteDvTapRefusal();
				Assert::AreEqual<uint64_t>(count, VpFrameAnalysis::DvTapRefusals());
				Rig rig;
				rig.Run();
				const std::vector<std::string> fb = rig.Lines(RE::StatusKind::Fallbacks);
				Assert::IsTrue(HasPart(fb, "skeleton_gpu_device"), L"the skeleton_lane fallback block is active (the rig reached it)");
				if (count == 0)
					Assert::IsFalse(HasPart(fb, "analysis tap: 10-bit"), L"no refusal: no line");
				else
					Assert::IsTrue(HasExact(fb, RefusalLine(count)), L"the exact visible refusal line with the counter value");
			}
			VpFrameAnalysis::ResetDvTapRefusals();
		}
	};
}
