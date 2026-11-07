// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only

// SkeletonModule (modules/control/skeleton): the analysis lane activated through the hub. The module is
// driven directly (no global registry) next to a real ResyncExtension bound to a fake renderer's
// Shadow / Drive, with source frames delivered through the SourceFrameTap exactly as the frame-analysis
// worker does. Settings parse; off is inert; the Original pathway forces off; waiting reasons are
// visible; with the shared seal driving and source frames tapped, P1 / P4 are sealed or live-filled with
// the bytes of the one-shot reference and P10-12 are declined with the visible reason; hosts are
// independent. 70b G5: skeleton_gpu_gap / grain_split_storage parse; Off, Shadow, the Original pathway and "no render
// device" construct no GPU dispatcher and make no GPU call (the status says why); the GPU status texts. CPU only (the
// lane's own CPU worker threads run); the dispatcher itself is covered by SkeletonGpuDispatcherWarpTests (WARP).

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include "CppUnitTest.h"

#include <VP-V/core/FallbackCatalog.h>
#include <VP-V/core/FrameAnalysisStore.h>
#include <VP-V/core/FramesInFlight.h>
#include <VP-V/modules/control/skeleton/SkeletonModule.h>
#include <VP-V/modules/input/signal/ResyncExtension.h>
#include <VP-V/modules/input/source_analysis/SourceFrameTap.h>
#include <VP-V/modules/input/source_analysis/lane/LaneCpuProducts.h>

#include <atomic>
#include <cstdio>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace SkeletonModuleTestsNs
{
	namespace RE = RendererExtension;
	namespace Tap = VpFrameAnalysis::SourceFrameTap;
	using Vpv::Resync::ResyncExtension;
	using Vpv::Skeleton::SkeletonModule;
	using VpSkeleton::Dims;
	using VpSkeleton::SourceFrameView;

	constexpr uint32_t kW = 64;
	constexpr uint32_t kH = 48;

	bool Has(const std::vector<std::string>& lines, const char* part)
	{
		for (const std::string& l : lines)
			if (l.find(part) != std::string::npos)
				return true;
		return false;
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
			path = std::string(dir) + "vp-skeleton-test-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(counter++) + ".cfg";
			Write(text);
		}
		~TempCfg() { std::remove(path.c_str()); }
		void Write(const std::string& text)
		{
			std::ofstream out(path, std::ios::binary | std::ios::trunc);
			out << text;
		}
		Vpv::Skeleton::ConfigLoader Loader() const
		{
			const std::string p = path;
			return [p](ConfigFile& c) { return c.Load(p, ConfigFile::ReadPolicy::Fresh); };
		}
	};

	std::string Lane(const char* mode, const char* extra = "") { return std::string("[vpv.skeleton]\nanalysis_lane=") + mode + "\n" + extra; }

	struct FakeRenderer
	{
		VpResync::Shadow resync;
		VpResync::Drive resyncDrive;
		Vpv::Resync::HostBinding binding{ this, resync, resyncDrive };
	};

	// One fake renderer driven through the resync extension (the seal) and the skeleton module.
	struct Rig
	{
		explicit Rig(const std::string& cfgText, size_t frames = 64)
			: cfg(cfgText), sk(cfg.Loader(), [this]() { return base.load() + calls.fetch_add(1); }), store(frames * kW * kH), count(frames)
		{
			uint32_t x = 777u;
			for (size_t i = 0; i < store.size(); ++i)
			{
				x = x * 1664525u + 1013904223u;
				store[i] = uint16_t((x >> 12) & 1023u);   // 10-bit sampler codes
			}
		}

		~Rig()
		{
			Life(RE::Lifecycle::GpuReleased);
		}

		TempCfg cfg;
		std::atomic<uint64_t> base{ 1000 };
		std::atomic<uint64_t> calls{ 0 };
		SkeletonModule sk;
		ResyncExtension resync;
		FakeRenderer r;
		std::vector<uint16_t> store;
		size_t count;
		int64_t ingressed = 0;

		const void* Host() const { return &r; }

		void Life(RE::Lifecycle kind, int code = 0)
		{
			RE::LifecycleFacts f;
			f.host = &r; f.kind = kind; f.code = code;
			sk.OnLifecycle(f);
		}

		void ResyncApply(const char* mode)
		{
			RE::LifecycleFacts f;
			f.host = &r; f.kind = RE::Lifecycle::SettingsApplied; f.subject = mode; f.code = 0; f.value = 0;
			resync.OnLifecycle(f);
		}

		void Ingress(int64_t seq)
		{
			RE::IngressContext c;
			c.host = &r; c.seq = seq; c.queueGeneration = 1; c.stateKey = 0x1000; c.formatKey = 7;
			c.frameCounter = seq; c.timestamp = seq * 1000; c.ticksPerSecond = 24000; c.measuredRateHz = 24.0;
			resync.OnIngress(c);
		}

		SourceFrameView View(uint64_t seq) const
		{
			SourceFrameView v;
			v.seq = seq;
			v.luma = store.data() + size_t(seq % count) * kW * kH;
			v.width = kW;
			v.height = kH;
			v.strideSamples = kW;
			v.msbShift = 6;
			return v;
		}

		bool Deliver(uint64_t seq) { return Tap::Deliver(&r, View(seq)); }

		void Phase(RE::FramePhase phase, int64_t seq)
		{
			RE::FrameContext c;
			c.host = &r; c.phase = phase; c.seq = seq;
			resync.OnFrame(c);
			Assert::IsFalse(sk.OnFrame(c), L"the skeleton never takes a frame over");
		}
		void Begin(int64_t seq)
		{
			base += 40000;   // 25 fps
			Phase(RE::FramePhase::Begin, seq);
		}
		void After(int64_t seq)
		{
			RE::FrameContext c;
			c.host = &r; c.phase = RE::FramePhase::AfterRender; c.seq = seq;
			Assert::IsFalse(sk.OnFrame(c));
		}

		// One rendered frame: the source frame ahead arrives, the seal, the lane, the gap.
		void Frame(int64_t seq)
		{
			Ingress(seq);
			if (seq == 1)
				Deliver(1);
			Deliver(uint64_t(seq) + 1);
			Begin(seq);
			After(seq);
		}

		std::vector<std::string> Lines(RE::StatusKind kind)
		{
			std::vector<std::string> lines;
			RE::Context c; c.host = &r;
			sk.OnStatus(c, kind, lines, nullptr, nullptr);
			return lines;
		}

		std::string Command()
		{
			std::string reply;
			RE::Context c; c.host = &r;
			Assert::IsTrue(sk.OnCommand(c, "skeleton", "status", reply));
			return reply;
		}

		std::vector<uint8_t> Bytes(VpSkeleton::ProductId id, uint64_t seq)
		{
			std::vector<uint8_t> v;
			VpSkeleton::AnalysisLane* lane = sk.LaneOf(&r);
			Assert::IsNotNull(lane);
			const std::shared_ptr<VpSkeleton::LaneState> st = lane->State();
			Assert::IsTrue(st != nullptr);
			const int idx = st->IndexOf(id);
			if (idx < 0 || !st->bufs[size_t(idx)])
				return v;
			const VpSkeleton::ReadInfo ri = st->table->At(size_t(idx)).Read(seq);
			VpSkeleton::CpuBuffers& b = *st->bufs[size_t(idx)];
			if (ri.kind == VpSkeleton::ReadInfo::Ring)
				v.assign(b.Ring(size_t(ri.index)), b.Ring(size_t(ri.index)) + b.Bytes());
			else if (ri.kind == VpSkeleton::ReadInfo::Live)
				v.assign(b.Live(size_t(ri.index)), b.Live(size_t(ri.index)) + b.Bytes());
			return v;
		}

		std::vector<uint8_t> ExpectedHalf(uint64_t seq) const
		{
			std::vector<uint8_t> out(VpLaneProducts::HalfLumaBytes(Dims{ kW, kH }));
			Assert::IsTrue(VpLaneProducts::ComputeHalfLuma(View(seq), out.data(), out.size()));
			return out;
		}
		std::vector<uint8_t> ExpectedStats(const std::vector<uint8_t>& half) const
		{
			std::vector<uint8_t> out(size_t(VpLaneProducts::kStatsBytes), 0);
			Assert::IsTrue(VpLaneProducts::ComputeSourceStats(half.data(), Dims{ kW, kH }, out.data(), out.size()));
			return out;
		}
	};

	Vpv::Skeleton::Settings ParseText(const std::string& text)
	{
		TempCfg cfg(text);
		ConfigFile config;
		Assert::IsTrue(config.Load(cfg.path, ConfigFile::ReadPolicy::Fresh), L"temporary configuration loads");
		return Vpv::Skeleton::Parse(config);
	}

	TEST_CLASS(SkeletonSettingsTests)
	{
	public:
		TEST_METHOD(DefaultsAreOff)
		{
			const Vpv::Skeleton::Settings s = ParseText("[general]\nx=1\n");
			Assert::IsTrue(s.lane.mode == VpSkeleton::LaneSettings::Mode::Off);
			Assert::IsTrue(s.camera == Vpv::Skeleton::CameraLinkMode::Off);
			Assert::IsFalse(s.LaneWanted() || s.CameraWanted());
			Assert::IsTrue(s.laneProblems.empty() && s.cameraProblems.empty());
		}

		TEST_METHOD(ValidValuesParse)
		{
			const Vpv::Skeleton::Settings s = ParseText("[vpv.skeleton]\nanalysis_lane=shadow\nanalysis_lane_chunk_us=800\n"
				"analysis_lane_deterministic=on\nanalysis_lane_test_wait_ms=3\ncamera_link=report\n");
			Assert::IsTrue(s.lane.mode == VpSkeleton::LaneSettings::Mode::Shadow);
			Assert::AreEqual<uint32_t>(800, s.lane.chunkUs);
			Assert::IsTrue(s.lane.deterministic == VpSkeleton::LaneSettings::Det::On);
			Assert::AreEqual<uint32_t>(3000, s.lane.testWaitUs);
			Assert::IsTrue(s.camera == Vpv::Skeleton::CameraLinkMode::Report);
			Assert::IsTrue(s.LaneWanted() && s.CameraWanted());
		}

		TEST_METHOD(InvalidValuesKeepTheDefaultAndAreReported)
		{
			const Vpv::Skeleton::Settings s = ParseText("[vpv.skeleton]\nanalysis_lane=banana\nanalysis_lane_chunk_us=x\n"
				"analysis_lane_deterministic=maybe\ncamera_link=sideways\n");
			Assert::IsTrue(s.lane.mode == VpSkeleton::LaneSettings::Mode::Off);
			Assert::AreEqual<uint32_t>(0, s.lane.chunkUs);
			Assert::IsTrue(s.lane.deterministic == VpSkeleton::LaneSettings::Det::Auto);
			Assert::IsTrue(s.camera == Vpv::Skeleton::CameraLinkMode::Off);
			Assert::AreEqual<size_t>(3, s.laneProblems.size());
			Assert::AreEqual<size_t>(1, s.cameraProblems.size());
		}

		TEST_METHOD(GpuKeysDefaultAndParse)
		{
			const Vpv::Skeleton::Settings d = ParseText("[vpv.skeleton]\nanalysis_lane=on\n");
			Assert::IsTrue(d.gpuGap == Vpv::Skeleton::GpuGapMode::Off, L"skeleton_gpu_gap defaults to off");
			Assert::IsTrue(d.grainStorage == Vpv::Skeleton::GrainStorage::Full, L"grain_split_storage defaults to full");
			const Vpv::Skeleton::Settings s = ParseText("[vpv.skeleton]\nanalysis_lane=on\nskeleton_gpu_gap=After_Render\ngrain_split_storage=Compact\n");
			Assert::IsTrue(s.gpuGap == Vpv::Skeleton::GpuGapMode::AfterRender);
			Assert::IsTrue(s.grainStorage == Vpv::Skeleton::GrainStorage::Compact);
			Assert::IsTrue(s.laneProblems.empty());
			const Vpv::Skeleton::Settings o = ParseText("[vpv.skeleton]\nskeleton_gpu_gap=off\ngrain_split_storage=full\n");
			Assert::IsTrue(o.gpuGap == Vpv::Skeleton::GpuGapMode::Off && o.grainStorage == Vpv::Skeleton::GrainStorage::Full);
			Assert::IsTrue(o.laneProblems.empty());
		}

		TEST_METHOD(InvalidGpuKeysKeepTheDefaultAndAreReported)
		{
			const Vpv::Skeleton::Settings s = ParseText("[vpv.skeleton]\nskeleton_gpu_gap=sometimes\ngrain_split_storage=tiny\n");
			Assert::IsTrue(s.gpuGap == Vpv::Skeleton::GpuGapMode::Off);
			Assert::IsTrue(s.grainStorage == Vpv::Skeleton::GrainStorage::Full);
			Assert::AreEqual<size_t>(2, s.laneProblems.size());
			Assert::IsTrue(s.laneProblems[0] == "skeleton_gpu_gap 'sometimes' is not valid (off|after_render); kept off");
			Assert::IsTrue(s.laneProblems[1] == "grain_split_storage 'tiny' is not valid (full|compact); kept full");
		}

		TEST_METHOD(OriginalPathwayForcesBothOff)
		{
			const Vpv::Skeleton::Settings s = ParseText("[vpv]\npipeline_original_pathway=on\n[vpv.skeleton]\nanalysis_lane=on\ncamera_link=report\n");
			Assert::IsTrue(s.originalPathway);
			Assert::IsFalse(s.LaneWanted() || s.CameraWanted());
		}

		TEST_METHOD(AnUnreadableFileMeansDefaults)
		{
			const Vpv::Skeleton::Settings s = Vpv::Skeleton::Load([](ConfigFile&) { return false; });
			Assert::IsFalse(s.LaneWanted() || s.CameraWanted());
		}
	};

	TEST_CLASS(SkeletonModuleTests)
	{
	public:
		TEST_METHOD(OffBuildsNothingAndShowsNothing)
		{
			Rig rig("[general]\nx=1\n");
			rig.ResyncApply("on");
			rig.Life(RE::Lifecycle::GpuCreated);
			rig.Life(RE::Lifecycle::SettingsApplied);
			Assert::IsFalse(rig.sk.Active(rig.Host()));
			Assert::AreEqual<size_t>(0, rig.sk.HostStates(), L"no per-renderer state");
			Assert::IsFalse(rig.sk.TapRegistered(rig.Host()), L"no source tap");
			for (int64_t s = 1; s <= 4; ++s)
			{
				rig.Ingress(s);
				rig.Begin(s);
				rig.After(s);
			}
			Assert::IsNull(rig.sk.LaneOf(rig.Host()));
			Assert::IsTrue(rig.Lines(RE::StatusKind::Lines).empty());
			Assert::IsTrue(rig.Lines(RE::StatusKind::Fallbacks).empty());
			// The seal carries only the resync providers: nothing of ours was added.
			Assert::AreEqual<size_t>(2, ResyncExtension::SealFor(rig.Host())->ProviderCount());
			Assert::AreEqual<size_t>(2, ResyncExtension::SealFor(rig.Host())->SubscriberCount(), L"drive and metrics, nothing of ours");
			RE::Context c; c.host = rig.Host();
			std::string reply;
			Assert::IsTrue(rig.sk.OnCommand(c, "skeleton", "status", reply));
			Assert::IsTrue(reply.find("off") != std::string::npos);
		}

		TEST_METHOD(OriginalPathwayForcesOff)
		{
			Rig rig(Lane("on"));
			rig.Life(RE::Lifecycle::SettingsApplied, 1);   // code != 0: the Original pathway
			Assert::IsFalse(rig.sk.Active(rig.Host()));
			Assert::IsFalse(rig.sk.TapRegistered(rig.Host()));
			Assert::AreEqual<size_t>(0, rig.sk.HostStates());
			Assert::IsTrue(rig.Lines(RE::StatusKind::Lines).empty());
			rig.Life(RE::Lifecycle::SettingsApplied, 0);
			Assert::IsTrue(rig.sk.Active(rig.Host()));
			Assert::IsTrue(rig.sk.TapRegistered(rig.Host()));
			rig.Life(RE::Lifecycle::SettingsApplied, 1);   // and off again
			Assert::IsFalse(rig.sk.Active(rig.Host()));
			Assert::IsFalse(rig.sk.TapRegistered(rig.Host()), L"the tap is unregistered");
		}

		TEST_METHOD(AnInvalidKeyIsShownEvenWhileOff)
		{
			Rig rig(Lane("banana"));
			rig.Life(RE::Lifecycle::GpuCreated);
			Assert::IsFalse(rig.sk.Active(rig.Host()));
			Assert::IsFalse(rig.sk.TapRegistered(rig.Host()));
			Assert::IsTrue(rig.Lines(RE::StatusKind::Lines).empty());
			Assert::IsTrue(Has(rig.Lines(RE::StatusKind::Fallbacks), "skeleton_settings: analysis_lane 'banana'"));
		}

		TEST_METHOD(WithoutADrivingResyncTheLaneWaitsVisibly)
		{
			Rig rig(Lane("on"));
			rig.ResyncApply("shadow");
			rig.Life(RE::Lifecycle::GpuCreated);
			Assert::IsTrue(rig.sk.Active(rig.Host()));
			Assert::IsTrue(rig.sk.TapRegistered(rig.Host()));
			for (int64_t s = 1; s <= 3; ++s)
				rig.Frame(s);
			Assert::IsNull(rig.sk.LaneOf(rig.Host()), L"no lane without the shared seal");
			Assert::IsFalse(rig.sk.WorkersRunning(rig.Host()), L"no thread");
			Assert::IsTrue(Has(rig.Lines(RE::StatusKind::Fallbacks), "skeleton_lane: waiting: resync_event=on required (one shared seal)"));
			Assert::IsTrue(Has(rig.Lines(RE::StatusKind::Lines), "skeleton=analysis lane on: waiting"));
		}

		TEST_METHOD(WithoutSourceFramesTheLaneSaysSo)
		{
			Rig rig(Lane("on"));
			rig.ResyncApply("on");
			rig.Life(RE::Lifecycle::GpuCreated);
			for (int64_t s = 1; s <= 3; ++s)
			{
				rig.Ingress(s);
				rig.Begin(s);   // nothing was ever tapped: frame_analysis_store is off
				rig.After(s);
			}
			Assert::IsNull(rig.sk.LaneOf(rig.Host()));
			Assert::IsTrue(Has(rig.Lines(RE::StatusKind::Fallbacks), "no source frames: needs frame_analysis_store=on"));
			Assert::IsFalse(rig.sk.WorkersRunning(rig.Host()));
		}

		TEST_METHOD(ProductsAreSealedOrLiveFilledBitIdenticalAndTheRestDeclined)
		{
			Rig rig(Lane("on"));
			rig.ResyncApply("on");
			rig.Life(RE::Lifecycle::GpuCreated);
			RE::QueueFacts q; q.host = rig.Host(); q.kind = RE::QueueEvent::Flush; q.generation = 1; q.frames = 8; q.text = "start";
			rig.resync.OnQueue(q);
			rig.sk.OnQueue(q);
			for (int64_t s = 1; s <= 12; ++s)
			{
				rig.Frame(s);
				Assert::IsNotNull(rig.sk.LaneOf(rig.Host()), L"the lane exists from the first frame with source frames");
				const std::vector<uint8_t> half = rig.Bytes(VpSkeleton::kP1HalfLuma, uint64_t(s));
				Assert::IsTrue(half == rig.ExpectedHalf(uint64_t(s)), L"P1 equals the one-shot reference");
				Assert::IsTrue(rig.Bytes(VpSkeleton::kP4SourceStats, uint64_t(s)) == rig.ExpectedStats(half), L"P4 equals the one-shot reference");
			}
			Assert::IsTrue(rig.sk.LaneOf(rig.Host())->Active());
			Assert::AreEqual<uint32_t>(kW, rig.sk.DimsOf(rig.Host()).width);
			Assert::AreEqual<uint32_t>(kH, rig.sk.DimsOf(rig.Host()).height);
			Assert::IsTrue(rig.sk.WorkersRunning(rig.Host()), L"started after the producers were registered");
			Assert::IsTrue(rig.sk.LaneOf(rig.Host())->TotalDeclined() > 0, L"P10-12 have no producer and no live fill yet");
			const std::vector<std::string> fb = rig.Lines(RE::StatusKind::Fallbacks);
			Assert::IsTrue(Has(fb, "skeleton_gpu_device: GPU products: no render device"), L"no pl_gpu in a unit test: P10-12 are not made, visibly");
			Assert::IsFalse(Has(fb, "skeleton_products"), L"the old declined entry is gone");
			Assert::IsFalse(Has(fb, "skeleton_lane: waiting"), L"nothing is waiting any more");
			const std::vector<std::string> lines = rig.Lines(RE::StatusKind::Lines);
			Assert::AreEqual<size_t>(1, lines.size());
			Assert::IsTrue(lines[0].find("skeleton=analysis lane on 64x48") == 0);
			Assert::IsTrue(rig.Command().find("graph configure") != std::string::npos);
		}

		TEST_METHOD(AnAheadResultIsTheSameBytesAsTheLiveForm)
		{
			// No timing: the worker threads are not started and the test steps the CPU worker itself, so the
			// product of frame 2 is made ahead (while frame 1 renders) and accepted at the seal of frame 2.
			Rig rig(Lane("on"));
			VpSkeleton::CardProfile card;   // no render device in a unit test: a card with room for the lane
			card.vramBudgetBytes = 1ull << 30;
			card.systemRamFreeBytes = 1ull << 30;
			card.uploadGBps = 10.0;
			rig.sk.SetCardForTesting(card);
			rig.sk.SetWorkerThreadsForTesting(false);
			rig.ResyncApply("on");
			rig.Life(RE::Lifecycle::GpuCreated);
			rig.Frame(1);
			VpSkeleton::AnalysisLane* lane = rig.sk.LaneOf(rig.Host());
			Assert::IsNotNull(lane);
			Assert::IsFalse(rig.sk.WorkersRunning(rig.Host()));
			int steps = 0;
			while (lane->StepWorker(VpSkeleton::Backend::Cpu, 0) && ++steps < 1000)
			{
			}
			Assert::IsTrue(steps > 0 && steps < 1000, L"the lane made progress and went idle");
			rig.Frame(2);
			Assert::IsTrue(lane->TotalAhead() > 0, L"frame 2 was made ahead of its seal");
			const std::vector<uint8_t> half = rig.Bytes(VpSkeleton::kP1HalfLuma, 2);
			Assert::IsTrue(half == rig.ExpectedHalf(2), L"P1 equals the reference");
			Assert::IsTrue(rig.Bytes(VpSkeleton::kP4SourceStats, 2) == rig.ExpectedStats(half), L"P4 equals the reference");
		}

		TEST_METHOD(AReusedRendererAddressRebuildsTheStaleState)
		{
			// A renderer that died without GpuReleased (a failed device create) and a new one at its address.
			Rig rig(Lane("on"));
			FrameAnalysis::Store store(FrameAnalysis::RingDepth(4, 4));
			Tap::SetStore(rig.Host(), &store);
			rig.ResyncApply("on");
			rig.Life(RE::Lifecycle::GpuCreated);
			rig.Frame(1);
			Assert::IsTrue(rig.sk.TapRegistered(rig.Host()));
			// The old Host's destructor: store and sink go.
			Tap::SetStore(rig.Host(), nullptr);
			Tap::Unregister(rig.Host());
			Assert::IsFalse(rig.sk.TapRegistered(rig.Host()));
			// The new Host registers its store; the first thing the module sees is a frame.
			Tap::SetStore(rig.Host(), &store);
			rig.Frame(2);
			Assert::IsTrue(rig.sk.Active(rig.Host()), L"rebuilt from the configuration");
			Assert::IsTrue(rig.sk.TapRegistered(rig.Host()), L"the tap is registered again");
			// And through the settings path.
			Tap::SetStore(rig.Host(), nullptr);
			Tap::Unregister(rig.Host());
			Tap::SetStore(rig.Host(), &store);
			rig.Life(RE::Lifecycle::SettingsApplied);
			Assert::IsTrue(rig.sk.Active(rig.Host()) && rig.sk.TapRegistered(rig.Host()));
			Assert::AreEqual<size_t>(1, rig.sk.HostStates());
			Tap::SetStore(rig.Host(), nullptr);
		}

		TEST_METHOD(AReleasedHostLeavesTheOtherIntact)
		{
			Rig a(Lane("on"));
			a.ResyncApply("on");
			a.Life(RE::Lifecycle::GpuCreated);
			// A second renderer served by the same module and the same resync extension.
			FakeRenderer other;
			const void* bh = &other;
			auto lifeB = [&](RE::Lifecycle kind, int code = 0) {
				RE::LifecycleFacts f; f.host = bh; f.kind = kind; f.code = code;
				a.sk.OnLifecycle(f);
			};
			{
				RE::LifecycleFacts f; f.host = bh; f.kind = RE::Lifecycle::SettingsApplied; f.subject = "on"; a.resync.OnLifecycle(f);
			}
			lifeB(RE::Lifecycle::GpuCreated);
			Assert::IsTrue(a.sk.Active(a.Host()) && a.sk.Active(bh));
			auto frameB = [&](int64_t seq) {
				RE::IngressContext c; c.host = bh; c.seq = seq; c.queueGeneration = 1; c.stateKey = 0x1000; c.formatKey = 7;
				c.frameCounter = seq; c.timestamp = seq * 1000; c.ticksPerSecond = 24000; c.measuredRateHz = 24.0;
				a.resync.OnIngress(c);
				SourceFrameView v = a.View(uint64_t(seq) + 1);
				if (seq == 1) Tap::Deliver(bh, a.View(1));
				Tap::Deliver(bh, v);
				a.base += 40000;
				RE::FrameContext f; f.host = bh; f.phase = RE::FramePhase::Begin; f.seq = seq;
				a.resync.OnFrame(f); a.sk.OnFrame(f);
			};
			for (int64_t s = 1; s <= 4; ++s)
			{
				a.Frame(s);
				frameB(s);
			}
			Assert::IsNotNull(a.sk.LaneOf(a.Host()));
			Assert::IsNotNull(a.sk.LaneOf(bh));
			Assert::IsTrue(a.sk.LaneOf(a.Host()) != a.sk.LaneOf(bh), L"one lane per renderer");

			a.Life(RE::Lifecycle::GpuReleased);
			Assert::IsFalse(a.sk.Active(a.Host()));
			Assert::IsFalse(a.sk.TapRegistered(a.Host()));
			Assert::IsNull(a.sk.LaneOf(a.Host()));
			Assert::IsFalse(a.sk.WorkersRunning(a.Host()));
			Assert::IsTrue(a.sk.Active(bh) && a.sk.TapRegistered(bh), L"the other renderer is untouched");
			Assert::IsNotNull(a.sk.LaneOf(bh));
			Assert::IsTrue(a.sk.WorkersRunning(bh));
			frameB(5);
			Assert::IsTrue(a.sk.LaneOf(bh)->Active());
			lifeB(RE::Lifecycle::GpuReleased);
			Assert::IsFalse(a.sk.Active(bh));
			Assert::AreEqual<size_t>(0, a.sk.HostStates());
		}

		TEST_METHOD(SwitchingOffAtSettingsAppliedTearsTheLaneDown)
		{
			Rig rig(Lane("on"));
			rig.ResyncApply("on");
			rig.Life(RE::Lifecycle::GpuCreated);
			for (int64_t s = 1; s <= 3; ++s)
				rig.Frame(s);
			Assert::IsNotNull(rig.sk.LaneOf(rig.Host()));
			Assert::IsTrue(rig.sk.WorkersRunning(rig.Host()));
			rig.cfg.Write(Lane("off"));
			rig.Life(RE::Lifecycle::SettingsApplied);
			Assert::IsFalse(rig.sk.Active(rig.Host()));
			Assert::IsNull(rig.sk.LaneOf(rig.Host()));
			Assert::IsFalse(rig.sk.WorkersRunning(rig.Host()));
			Assert::IsFalse(rig.sk.TapRegistered(rig.Host()));
			Assert::IsTrue(rig.Lines(RE::StatusKind::Lines).empty());
			Assert::IsTrue(rig.Lines(RE::StatusKind::Fallbacks).empty());
			rig.Frame(4);
			Assert::IsNull(rig.sk.LaneOf(rig.Host()), L"frames after switch-off do nothing");
		}

		TEST_METHOD(SwitchingOnAfterTheFirstFrameIsShownAsALateProvider)
		{
			Rig rig("[general]\nx=1\n");
			rig.ResyncApply("on");
			rig.Life(RE::Lifecycle::GpuCreated);
			for (int64_t s = 1; s <= 3; ++s)
			{
				rig.Ingress(s);
				rig.Begin(s);
			}
			Assert::IsTrue(ResyncExtension::SealFor(rig.Host())->Seals() > 0);
			rig.cfg.Write(Lane("on"));
			rig.Life(RE::Lifecycle::SettingsApplied);
			Assert::IsTrue(rig.sk.Active(rig.Host()));
			Assert::IsTrue(Has(rig.Lines(RE::StatusKind::Fallbacks), "switched on after the first frame"));
		}

		TEST_METHOD(ASourceSizeChangeRebuildsTheLane)
		{
			Rig rig(Lane("on"));
			rig.ResyncApply("on");
			rig.Life(RE::Lifecycle::GpuCreated);
			for (int64_t s = 1; s <= 3; ++s)
				rig.Frame(s);
			Assert::AreEqual<uint32_t>(kW, rig.sk.DimsOf(rig.Host()).width);
			VpSkeleton::AnalysisLane* first = rig.sk.LaneOf(rig.Host());
			std::vector<uint16_t> tiny(32 * 24 * 8, 100);
			for (int64_t s = 4; s <= 6; ++s)
			{
				rig.Ingress(s);
				for (uint64_t k = uint64_t(s); k <= uint64_t(s) + 1; ++k)
				{
					SourceFrameView v;
					v.seq = k; v.luma = tiny.data() + size_t(k % 8) * 32 * 24; v.width = 32; v.height = 24; v.strideSamples = 32; v.msbShift = 6;
					Tap::Deliver(rig.Host(), v);
				}
				rig.Begin(s);
				rig.After(s);
			}
			Assert::AreEqual<uint32_t>(32, rig.sk.DimsOf(rig.Host()).width);
			Assert::AreEqual<uint32_t>(24, rig.sk.DimsOf(rig.Host()).height);
			Assert::IsTrue(rig.sk.LaneOf(rig.Host()) != first, L"a lane for the new size");
			Assert::IsTrue(rig.sk.LaneOf(rig.Host())->Active());
			Assert::IsTrue(rig.sk.WorkersRunning(rig.Host()));
		}

		TEST_METHOD(QueueDropsReachTheLaneAtTheNextFrame)
		{
			Rig rig(Lane("on"));
			rig.ResyncApply("on");
			rig.Life(RE::Lifecycle::GpuCreated);
			for (int64_t s = 1; s <= 3; ++s)
				rig.Frame(s);
			RE::QueueFacts d; d.host = rig.Host(); d.kind = RE::QueueEvent::Drop; d.seq = 6; d.frames = 2; d.cause = 1;
			rig.sk.OnQueue(d);
			rig.Frame(4);
			Assert::IsTrue(rig.Command().find("2 drops applied") != std::string::npos);
		}

		TEST_METHOD(OffAndOriginalNeverConstructAGpuDispatcher)
		{
			const unsigned long long before = SkeletonModule::GpuDispatchersCreated();
			{
				Rig off("[general]\nx=1\n");
				off.ResyncApply("on");
				off.Life(RE::Lifecycle::GpuCreated);
				off.Life(RE::Lifecycle::SettingsApplied);
				for (int64_t s = 1; s <= 3; ++s)
					off.Frame(s);
				Assert::IsNull(off.sk.GpuDispatcherFor(off.Host()), L"off: no dispatcher");
			}
			{
				Rig orig(Lane("on", "skeleton_gpu_gap=after_render\n"));
				orig.Life(RE::Lifecycle::GpuCreated, 0);
				orig.Life(RE::Lifecycle::SettingsApplied, 1);   // the Original pathway
				Assert::IsFalse(orig.sk.Active(orig.Host()));
				Assert::IsNull(orig.sk.GpuDispatcherFor(orig.Host()), L"Original: no dispatcher");
				Assert::IsNull(orig.sk.LaneOf(orig.Host()));
			}
			Assert::AreEqual(before, SkeletonModule::GpuDispatchersCreated(), L"no dispatcher was ever constructed");
		}

		TEST_METHOD(WithoutAPlGpuTheOnModeHasNoDispatcherAndSaysSo)
		{
			const unsigned long long before = SkeletonModule::GpuDispatchersCreated();
			Rig rig(Lane("on", "skeleton_gpu_gap=after_render\ngrain_split_storage=compact\n"));
			rig.ResyncApply("on");
			rig.Life(RE::Lifecycle::GpuCreated);   // f.gpu is null in a unit test
			for (int64_t s = 1; s <= 6; ++s)
				rig.Frame(s);
			Assert::IsNotNull(rig.sk.LaneOf(rig.Host()));
			Assert::IsNull(rig.sk.GpuDispatcherFor(rig.Host()));
			Assert::AreEqual(before, SkeletonModule::GpuDispatchersCreated());
			const std::vector<std::string> fb = rig.Lines(RE::StatusKind::Fallbacks);
			Assert::IsTrue(Has(fb, "skeleton_gpu_device: GPU products: no render device"));
			Assert::IsFalse(Has(fb, "GPU lane work runs ahead of the present"), L"nothing runs ahead without a device");
			Assert::IsFalse(Has(fb, "live: waits for the Entrance phase"), L"no dispatcher, no live wait");
			Assert::IsFalse(Has(fb, "shadow: GPU products skipped"));
			Assert::IsTrue(rig.sk.LaneOf(rig.Host())->TotalDeclined() > 0, L"the declining live fill is still installed without a dispatcher");
		}

		TEST_METHOD(ShadowModeSkipsTheGpuProductsVisibly)
		{
			const unsigned long long before = SkeletonModule::GpuDispatchersCreated();
			Rig rig(Lane("shadow", "skeleton_gpu_gap=after_render\n"));
			rig.ResyncApply("on");
			rig.Life(RE::Lifecycle::GpuCreated);
			for (int64_t s = 1; s <= 6; ++s)
				rig.Frame(s);
			Assert::IsNull(rig.sk.GpuDispatcherFor(rig.Host()));
			Assert::AreEqual(before, SkeletonModule::GpuDispatchersCreated());
			const std::vector<std::string> fb = rig.Lines(RE::StatusKind::Fallbacks);
			Assert::IsTrue(Has(fb, "skeleton_gpu_shadow: shadow: GPU products skipped"));
			Assert::IsFalse(Has(fb, "no render device"), L"shadow is its own reason");
		}

		TEST_METHOD(AnInvalidGpuKeyIsShownWhileTheLaneRuns)
		{
			Rig rig(Lane("on", "skeleton_gpu_gap=sometimes\n"));
			rig.ResyncApply("on");
			rig.Life(RE::Lifecycle::GpuCreated);
			for (int64_t s = 1; s <= 3; ++s)
				rig.Frame(s);
			Assert::IsTrue(Has(rig.Lines(RE::StatusKind::Fallbacks), "skeleton_settings: skeleton_gpu_gap 'sometimes' is not valid (off|after_render); kept off"));
		}

		TEST_METHOD(ACommandForAnotherModuleIsNotAnswered)
		{
			Rig rig(Lane("on"));
			rig.Life(RE::Lifecycle::GpuCreated);
			RE::Context c; c.host = rig.Host();
			std::string reply;
			Assert::IsFalse(rig.sk.OnCommand(c, "resync", "status", reply));
			Assert::IsFalse(rig.sk.OnCommand(c, "skeleton", "other", reply));
		}

		TEST_METHOD(WorkerFaultsForThisModuleAreCounted)
		{
			Rig rig(Lane("on"));
			rig.Life(RE::Lifecycle::GpuCreated);
			const unsigned long long before = SkeletonModule::ContainedFaults();
			std::vector<std::string> none;
			RE::Context c; c.host = rig.Host();
			rig.sk.OnStatus(c, RE::StatusKind::WorkerFault, none, "skeleton", "exception");
			rig.sk.OnStatus(c, RE::StatusKind::WorkerFault, none, "resync", "exception");
			Assert::AreEqual<unsigned long long>(before + 1, SkeletonModule::ContainedFaults());
		}
	};

	TEST_CLASS(SkeletonGpuStatusTexts)
	{
	public:
		TEST_METHOD(TheGpuStatusTextsAreTheAgreedOnes)
		{
			namespace FC = FallbackCatalog;
			Assert::IsTrue(FC::SkeletonGpuLive(true).engaged);
			Assert::IsTrue(FC::SkeletonGpuLive(true).reason == "live: waits for the Entrance phase (M2 P4)");
			Assert::IsFalse(FC::SkeletonGpuLive(false).engaged);
			Assert::IsTrue(FC::SkeletonGpuGapAhead(true).reason == "GPU lane work runs ahead of the present");
			Assert::IsFalse(FC::SkeletonGpuGapAhead(false).engaged);
			Assert::IsTrue(FC::SkeletonGpuDevice(true).reason.find("GPU products: no render device") == 0);
			Assert::IsFalse(FC::SkeletonGpuDevice(false).engaged);
			Assert::IsTrue(FC::SkeletonGpuShadow(true).reason.find("shadow: GPU products skipped") == 0);
			Assert::IsTrue(FC::SkeletonGpuChroma(true).reason == "P10c declined: chroma needs the source ring (M2 P10)");
			Assert::IsFalse(FC::SkeletonGpuFailed(0, "x").engaged);
			const FC::Decision failed = FC::SkeletonGpuFailed(3, "the extend pass failed");
			Assert::IsTrue(failed.engaged && failed.reason.find("3 time(s)") != std::string::npos && failed.reason.find("the extend pass failed") != std::string::npos);
			Assert::IsFalse(FC::SkeletonGpuShed("").engaged);
			const FC::Decision shed = FC::SkeletonGpuShed("light level (T1)");
			Assert::IsTrue(shed.engaged && shed.reason.find("VRAM shed: light level (T1)") == 0);
		}
	};
}
