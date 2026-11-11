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
#include <VP-V/core/VramUsers.h>
#include <VP-V/modules/control/skeleton/SkeletonModule.h>
#include <VP-V/modules/input/signal/ResyncExtension.h>
#include <VP-V/modules/input/source_analysis/SourceFrameTap.h>
#include <VP-V/modules/motion/estimation/VectorEngineInterface.h>
#include <VP-V/modules/motion/estimation/MotionAheadWalk.h>
#include <VP-V/modules/motion/estimation/global_model/CameraProducts.h>
#include <VP-V/modules/input/source_analysis/lane/LaneCpuProducts.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
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

	// True when the status carries an engaged entry for this reason id ("<id>: <reason>"). The audit rule: assert on the id,
	// never on the status prose (the lane's own card note may also say "no render device").
	bool HasId(const std::vector<std::string>& lines, const char* id)
	{
		const std::string prefix = std::string(id) + ": ";
		for (const std::string& l : lines)
			if (l.compare(0, prefix.size(), prefix) == 0)
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

		void Life(RE::Lifecycle kind, int code = 0, double value = 0.0)
		{
			RE::LifecycleFacts f;
			f.host = &r; f.kind = kind; f.code = code; f.value = value;
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

	// The CollectAhead phase as P5a's call site raises it: the input queue's size in a QueueView.
	void Collect(Rig& rig, size_t size)
	{
		rig.base += 40000;   // one call per 25 fps frame: the lane-lead window is REAL time (0.5 s), so the test clock must move
		RE::QueueView view;
		view.size = size;
		RE::FrameContext c;
		c.host = rig.Host(); c.phase = RE::FramePhase::CollectAhead; c.phaseData = &view;
		Assert::IsFalse(rig.sk.OnFrame(c), L"the skeleton never takes a phase over");
	}

	// A scripted vector engine (P5a's IVectorEngine): what the feed reads.
	struct FakeEngine : VpMotion::IVectorEngine
	{
		uint64_t latest = 0, era = 1;
		uint64_t pairFrom = 0;
		bool slip = false;
		bool global = true;
		bool fitWanted = false;
		int fitCalls = 0;
		VpIndicator::Stamp stamp;
		uint64_t LatestVerifiedFieldSeq() const override { return latest; }
		uint64_t FieldEra() const override { return era; }
		bool FieldOf(uint64_t seq, VpMotion::VerifiedField& out) const override
		{
			if (seq != latest || seq == 0)
				return false;
			out = VpMotion::VerifiedField();
			out.seq = seq;
			out.pairFrom = pairFrom;
			out.pairTo = seq;
			out.gridW = 4;
			out.gridH = 3;
			out.blockSize = 16;
			out.stamp = stamp;
			return true;
		}
		float FieldScale() const override { return 1.0f; }
		bool GlobalFromEngine() const override { return global; }
		bool EngineGlobal(uint64_t, VpMotion::GlobalVec& out) const override
		{
			out.valid = global;
			out.x = 1.5;
			out.y = -0.5;
			return global;
		}
		bool PairSpansSlip(uint64_t, uint64_t) const override { return slip; }
		bool FlowIsCpuResident() const override { return false; }
		bool FlowOf(uint64_t, VpMotion::FlowView&) const override { return false; }
		void SetPreEstimateHook(VpMotion::IPreEstimateHook*) override {}
		void SetCameraFitWanted(bool wanted) override { fitWanted = wanted; ++fitCalls; }
	};

	// Camera consumers as the camera extension declares them: the fit (P7 at lag 1) and the diagnostics (P7 + P8).
	std::vector<VpSkeleton::ConsumerDecl> CameraConsumers()
	{
		CameraLink::CameraDemandInput in;
		in.active[static_cast<int>(CameraLink::CameraConsumer::Measurement)] = true;
		in.active[static_cast<int>(CameraLink::CameraConsumer::Diagnostics)] = true;
		std::vector<VpSkeleton::ConsumerDecl> out;
		CameraLink::DeclareCameraConsumers(in, out);
		return out;
	}


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

		TEST_METHOD(DispatchKeysDefaultAndParse)
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

		TEST_METHOD(InvalidDispatchKeysKeepTheDefaultAndAreReported)
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
			// The seal carries only the resync providers (drive, DV, motion: W1-C2a A1): nothing of ours was added.
			Assert::AreEqual<size_t>(3, ResyncExtension::SealFor(rig.Host())->ProviderCount());
			Assert::IsTrue(ResyncExtension::SealFor(rig.Host())->HasProvider("motion"), L"the third provider is motion (WAIVER-C2A-01)");
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

		static VpSkeleton::Graph GraphFor(VpSkeleton::ProductId asked, double periodMs)
		{
			VpSkeleton::GraphSettings gs;
			gs.dims.width = 64;
			gs.dims.height = 48;
			gs.queueDepth = 6;
			gs.laneLead = 1;
			VpSkeleton::ConsumerDecl c;
			c.id = 1;
			c.name = "history cap test";
			VpSkeleton::AddConsume(c, asked);
			return VpSkeleton::BuildGraph(VpSkeleton::SkeletonProducts(false, false, periodMs), { c }, gs);
		}

		TEST_METHOD(TheHistoryCapIsShownOnlyWhenACappedProductIsInTheGraph)
		{
			const double fast = 1000.0 / 240.0;   // 2 s of quality pattern history = 480 frames: capped at kRecordHistoryMax
			const VpSkeleton::Graph withQp = GraphFor(VpSkeleton::kQualityPattern, fast);
			Assert::IsTrue(withQp.valid);
			const FallbackCatalog::Decision on = SkeletonModule::HistoryCapDecision(withQp, fast);
			Assert::IsTrue(on.engaged);
			Assert::AreEqual(std::string("skeleton_history"), on.id);
			Assert::IsTrue(on.reason.find("quality pattern history capped at 223 frames (source rate above 111.5 fps)") != std::string::npos);
			// Capped rate but the closure holds no quality pattern: nothing to report (the trail summary needs ~892 fps).
			const VpSkeleton::Graph noQp = GraphFor(VpSkeleton::kP1HalfLuma, fast);
			Assert::IsTrue(noQp.valid && !noQp.Has(VpSkeleton::kQualityPattern) && !noQp.Has(VpSkeleton::kQualityPatternCausal));
			Assert::IsFalse(SkeletonModule::HistoryCapDecision(noQp, fast).engaged);
			Assert::IsFalse(SkeletonModule::HistoryCapDecision(GraphFor(VpSkeleton::kTdTrailSummary, fast), fast).engaged);
			// In the graph but not capped (24 p, unmeasured), and an invalid graph.
			Assert::IsFalse(SkeletonModule::HistoryCapDecision(GraphFor(VpSkeleton::kQualityPattern, 1001.0 / 24.0), 1001.0 / 24.0).engaged);
			Assert::IsFalse(SkeletonModule::HistoryCapDecision(GraphFor(VpSkeleton::kQualityPattern, 0.0), 0.0).engaged);
			Assert::IsFalse(SkeletonModule::HistoryCapDecision(VpSkeleton::Graph(), fast).engaged);
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
			Assert::IsFalse(Has(fb, "skeleton_history"), L"no capped record history is in this graph: no cap note");
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

		TEST_METHOD(AQueueChangeReconfiguresTheGraphAtTheNextFrame)
		{
			// The record histories follow R + L + look-ahead: a queue change rebuilds the graph like a moved source period.
			Rig rig(Lane("on"));
			rig.ResyncApply("on");
			rig.Life(RE::Lifecycle::GpuCreated);
			auto configures = [&rig]() {
				const std::string text = rig.Command();
				const size_t k = text.find(" graph configure");
				Assert::IsTrue(k != std::string::npos);
				const size_t b = text.rfind(' ', k - 1);
				return std::stoi(text.substr(b + 1, k - b - 1));
			};
			for (int64_t s = 1; s <= 3; ++s)
				rig.Frame(s);
			const int before = configures();
			for (int64_t s = 4; s <= 6; ++s)
				rig.Frame(s);
			Assert::AreEqual(before, configures(), L"an unchanged queue does not reconfigure");
			Assert::AreEqual(1, int(rig.sk.LaneOf(rig.Host())->State()->graph.settings.laneLead));
			SkeletonModule::QueueShape q;
			q.renderAhead = 4;
			q.laneLead = 3;
			q.lookAheadWalk = 5;
			rig.sk.SetQueueForTesting(q);
			rig.Frame(7);
			Assert::AreEqual(before + 1, configures(), L"the queue change reconfigured the graph once");
			const VpSkeleton::GraphSettings gs = rig.sk.LaneOf(rig.Host())->State()->graph.settings;
			Assert::AreEqual(4, int(gs.renderAhead));
			Assert::AreEqual(3, int(gs.laneLead));
			Assert::AreEqual(5, int(gs.lookAheadWalk));
			rig.Frame(8);
			Assert::AreEqual(before + 1, configures(), L"and only once");
		}

		TEST_METHOD(ALongerConfiguredQueueGivesALongerHistory)
		{
			// The module's queue snapshot is what SkeletonProducts is built from: floor on a short queue, deeper on a long one.
			const double p24 = 1001.0 / 24.0;
			SkeletonModule::QueueShape shortQ;
			shortQ.renderAhead = 2; shortQ.laneLead = 3; shortQ.lookAheadWalk = 2; shortQ.wired = SkeletonModule::QueueShape::kAll;
			SkeletonModule::QueueShape longQ;
			longQ.renderAhead = 8; longQ.laneLead = 4; longQ.lookAheadWalk = 88; longQ.wired = SkeletonModule::QueueShape::kAll;
			SkeletonModule sk;
			sk.PublishQueue(shortQ);
			const SkeletonModule::QueueShape gotShort = sk.CurrentQueue();
			const unsigned shortFrames = VpSkeleton::QueueFramesFor(gotShort.renderAhead, gotShort.laneLead, gotShort.lookAheadWalk);
			sk.PublishQueue(longQ);
			const SkeletonModule::QueueShape got = sk.CurrentQueue();
			Assert::IsTrue(got == longQ, L"the snapshot round-trips");
			const unsigned longFrames = VpSkeleton::QueueFramesFor(got.renderAhead, got.laneLead, got.lookAheadWalk);
			Assert::AreEqual(unsigned(7), shortFrames);
			Assert::AreEqual(unsigned(100), longFrames);
			const int floorQp = int(VpSkeleton::SkeletonProducts(false, false, p24, shortFrames).Get(VpSkeleton::kQualityPattern).history);
			const int longQp = int(VpSkeleton::SkeletonProducts(false, false, p24, longFrames).Get(VpSkeleton::kQualityPattern).history);
			Assert::AreEqual(48, floorQp, L"a short queue keeps the 2 s floor");
			Assert::AreEqual(100, longQp, L"a long queue gives a longer history");
			// An undelivered term keeps its default (0 / 1 / 0) whatever value a feed passes for it.
			SkeletonModule::QueueShape partial;
			partial.renderAhead = 9; partial.laneLead = 5; partial.lookAheadWalk = 7; partial.wired = SkeletonModule::QueueShape::kLaneLead;
			sk.PublishQueue(partial);
			Assert::AreEqual(0, int(sk.CurrentQueue().renderAhead));
			Assert::AreEqual(5, int(sk.CurrentQueue().laneLead));
			Assert::AreEqual(0, int(sk.CurrentQueue().lookAheadWalk));
		}

		TEST_METHOD(AnUnwiredQueueIsShownAsAFallbackUntilItIsDelivered)
		{
			Rig rig(Lane("on"));
			rig.ResyncApply("on");
			rig.Life(RE::Lifecycle::GpuCreated);
			for (int64_t s = 1; s <= 3; ++s)
				rig.Frame(s);
			Assert::IsTrue(Has(rig.Lines(RE::StatusKind::Fallbacks),
				"skeleton_queue: record history: queue not yet wired (render-ahead depth, lane lead, look-ahead walk); floors in use"));
			// The look-ahead walk arrives with the host's SettingsApplied call (value = the motion look-ahead depth): that term leaves the line.
			rig.Life(RE::Lifecycle::SettingsApplied, 0, 4.0);
			Assert::AreEqual(4, int(rig.sk.CurrentQueue().lookAheadWalk));
			Assert::AreEqual(0, int(rig.sk.CurrentQueue().renderAhead));
			Assert::AreEqual(1, int(rig.sk.CurrentQueue().laneLead));
			rig.Frame(4);
			Assert::IsTrue(Has(rig.Lines(RE::StatusKind::Fallbacks),
				"skeleton_queue: record history: queue not yet wired (render-ahead depth, lane lead); floors in use"));
			SkeletonModule::QueueShape q;
			q.renderAhead = 1; q.laneLead = 2; q.lookAheadWalk = 3;
			rig.sk.SetQueueForTesting(q);
			rig.Frame(5);
			Assert::IsFalse(Has(rig.Lines(RE::StatusKind::Fallbacks), "skeleton_queue"), L"all three delivered: nothing to show");
			Assert::IsFalse(SkeletonModule::QueueDecision(SkeletonModule::QueueShape::kAll).engaged);
			Assert::AreEqual(std::string("lane lead, look-ahead walk"), SkeletonModule::QueueNotWiredNote(SkeletonModule::QueueShape::kRenderAhead));
		}

		TEST_METHOD(TheQueueFeedsEachMergeOnlyTheirOwnTerm)
		{
			// FEED RULE (owner decision 2026-11-07): three feeds on three threads, each compare-and-swapping ITS term into the one snapshot.
			// None may erase another's: a wired term never un-wires, every value stays in its feed's domain, and the last write of each
			// feed is what the snapshot holds at the end.
			SkeletonModule sk;
			const int kIter = 20000;
			std::atomic<bool> go{ false };
			std::atomic<int> bad{ 0 };
			auto spin = [&go]() { while (!go.load()) { std::this_thread::yield(); } };
			std::thread a([&]() { spin(); for (int i = 0; i < kIter; ++i) sk.PublishRenderAhead(double(i % 9)); });
			std::thread b([&]() { spin(); for (int i = 0; i < kIter; ++i) sk.PublishLaneLead(double(i % 200)); });
			std::thread c([&]() { spin(); for (int i = 0; i < kIter; ++i) sk.PublishLookAheadWalk(double(i % 90)); });
			std::thread r([&]() {
				spin();
				uint8_t seen = 0;
				for (int i = 0; i < kIter * 2; ++i)
				{
					const SkeletonModule::QueueShape q = sk.CurrentQueue();
					if ((seen & ~q.wired) != 0)
						++bad;   // a term that was delivered went missing
					seen = uint8_t(seen | q.wired);
					if (q.renderAhead > 8 || q.laneLead > 199 || q.lookAheadWalk > 89)
						++bad;   // a value outside its own feed's domain: another feed's value landed in it
				}
			});
			go = true;
			a.join(); b.join(); c.join(); r.join();
			Assert::AreEqual(0, bad.load());
			const SkeletonModule::QueueShape q = sk.CurrentQueue();
			Assert::AreEqual(int(SkeletonModule::QueueShape::kAll), int(q.wired));
			Assert::AreEqual((kIter - 1) % 9, int(q.renderAhead));
			Assert::AreEqual((kIter - 1) % 200, int(q.laneLead));
			Assert::AreEqual((kIter - 1) % 90, int(q.lookAheadWalk));
			// Single-threaded: each feed leaves the other terms as they are; values are clamped, never wrapped.
			SkeletonModule one;
			one.PublishLookAheadWalk(5.0);
			one.PublishRenderAhead(3.2);
			Assert::AreEqual(5, int(one.CurrentQueue().lookAheadWalk));
			Assert::AreEqual(4, int(one.CurrentQueue().renderAhead), L"a partial frame holds a slot: rounded up");
			Assert::AreEqual(1, int(one.CurrentQueue().laneLead), L"undelivered term keeps its default");
			one.PublishLaneLead(300.0);
			Assert::AreEqual(255, int(one.CurrentQueue().laneLead));
			one.PublishLaneLead(-1.0);
			Assert::AreEqual(0, int(one.CurrentQueue().laneLead));
			one.PublishRenderAhead(1000.0);
			Assert::AreEqual(255, int(one.CurrentQueue().renderAhead));
			one.PublishRenderAhead(std::nan(""));
			Assert::AreEqual(0, int(one.CurrentQueue().renderAhead));
			Assert::AreEqual(int(SkeletonModule::QueueShape::kAll), int(one.CurrentQueue().wired));
		}

		TEST_METHOD(TheLaneLeadIsTheQueueTheHostCollectedAheadAndIsShown)
		{
			// The input-queue term: QueueView::size at the CollectAhead phase. Ctrl+I prints the lead the lane runs with; the 0 and 255 cases.
			Rig rig(Lane("on"));
			rig.ResyncApply("on");
			rig.Life(RE::Lifecycle::GpuCreated);
			for (int64_t s = 1; s <= 3; ++s)
				rig.Frame(s);
			Assert::AreEqual(0, int(rig.sk.CurrentQueue().wired & SkeletonModule::QueueShape::kLaneLead), L"nothing fed yet");
			Collect(rig, 0);
			rig.Frame(4);
			SkeletonModule::QueueInUse u = rig.sk.QueueInUseOf(rig.Host());
			Assert::AreEqual(0, int(u.fed.laneLead));
			Assert::AreEqual(0, int(u.used.laneLead));
			Assert::IsTrue((u.fed.wired & SkeletonModule::QueueShape::kLaneLead) != 0);
			Assert::AreEqual(0, int(rig.sk.LaneOf(rig.Host())->State()->graph.settings.laneLead));
			Assert::IsTrue(Has(rig.Lines(RE::StatusKind::Lines), "lane lead 0"));
			const std::vector<std::string> fb0 = rig.Lines(RE::StatusKind::Fallbacks);
			Assert::IsTrue(Has(fb0, "queue not yet wired (render-ahead depth, look-ahead walk)"), L"the lane lead left the unwired list");
			Assert::IsFalse(HasId(fb0, "skeleton_lane_lead"));
			// 255 frames queued: the window keeps the LOWEST size over half a second of source time, so the 0 must age out first.
			for (int i = 0; i < 30; ++i)
				Collect(rig, 255);
			rig.Frame(5);
			u = rig.sk.QueueInUseOf(rig.Host());
			Assert::AreEqual(255, int(u.fed.laneLead));
			Assert::AreEqual(int(SkeletonModule::LaneLeadCap(0)), int(u.used.laneLead), L"capped to the ring-slot budget");
			Assert::IsTrue(u.used.laneLead < 255);
			Assert::AreEqual(int(u.used.laneLead), int(rig.sk.LaneOf(rig.Host())->State()->graph.settings.laneLead));
			Assert::IsTrue(rig.sk.LaneOf(rig.Host())->Active(), L"the capped graph is valid: the lane is not lost");
			Assert::IsTrue(Has(rig.Lines(RE::StatusKind::Lines), (std::string("lane lead ") + std::to_string(int(u.used.laneLead)) + " (fed 255, capped)").c_str()));
			Assert::IsTrue(HasId(rig.Lines(RE::StatusKind::Fallbacks), "skeleton_lane_lead"), L"the cap is shown, never silent");
			// A queue inside the budget is printed exactly as fed.
			for (int i = 0; i < 30; ++i)
				Collect(rig, 6);
			rig.Frame(6);
			u = rig.sk.QueueInUseOf(rig.Host());
			Assert::AreEqual(6, int(u.used.laneLead));
			Assert::AreEqual(6, int(u.fed.laneLead));
			Assert::IsTrue(Has(rig.Lines(RE::StatusKind::Lines), "lane lead 6"));
			Assert::IsFalse(Has(rig.Lines(RE::StatusKind::Lines), "capped"));
			Assert::IsFalse(HasId(rig.Lines(RE::StatusKind::Fallbacks), "skeleton_lane_lead"), L"the cap released by itself");
		}

		TEST_METHOD(TheThreeFeedsTogetherWireTheWholeQueue)
		{
			// Walk (SettingsApplied), render-ahead depth (the host's generic QueueDepth fact), lane lead (CollectAhead): each merges its own term.
			Rig rig(Lane("on"));
			rig.ResyncApply("on");
			rig.Life(RE::Lifecycle::GpuCreated);
			for (int64_t s = 1; s <= 3; ++s)
				rig.Frame(s);
			rig.Life(RE::Lifecycle::SettingsApplied, 0, 4.0);
			Collect(rig, 6);
			rig.Life(RE::Lifecycle::QueueDepth, 0, 2.2);   // a partial frame holds a slot: 3
			const SkeletonModule::QueueShape q = rig.sk.CurrentQueue();
			Assert::AreEqual(3, int(q.renderAhead));
			Assert::AreEqual(6, int(q.laneLead));
			Assert::AreEqual(4, int(q.lookAheadWalk));
			Assert::AreEqual(int(SkeletonModule::QueueShape::kAll), int(q.wired));
			rig.Frame(4);
			Assert::IsFalse(HasId(rig.Lines(RE::StatusKind::Fallbacks), "skeleton_queue"), L"all three delivered: nothing unwired to show");
			Assert::IsTrue(rig.Command().find("render-ahead 3, lane lead 6, look-ahead walk 4; queued ahead of output 9 frame(s)") != std::string::npos);
			const VpSkeleton::GraphSettings gs = rig.sk.LaneOf(rig.Host())->State()->graph.settings;
			Assert::AreEqual(3, int(gs.renderAhead));
			Assert::AreEqual(6, int(gs.laneLead));
			Assert::AreEqual(4, int(gs.lookAheadWalk));
		}

		TEST_METHOD(AQueueStarveIsShownAndRecovers)
		{
			Rig rig(Lane("on"));
			rig.ResyncApply("on");
			rig.Life(RE::Lifecycle::GpuCreated);
			for (int64_t s = 1; s <= 3; ++s)
				rig.Frame(s);
			for (int i = 0; i < 30; ++i)
				Collect(rig, 5);
			rig.Frame(4);
			Assert::AreEqual(5, int(rig.sk.QueueInUseOf(rig.Host()).used.laneLead));
			Assert::IsFalse(HasId(rig.Lines(RE::StatusKind::Fallbacks), "skeleton_queue_starve"));
			Collect(rig, 3);   // the queue ran short: the lowest size in the window is taken at once
			rig.Frame(5);
			const SkeletonModule::QueueInUse u = rig.sk.QueueInUseOf(rig.Host());
			Assert::AreEqual(3, int(u.used.laneLead));
			Assert::AreEqual(5, int(u.leadPeak));
			Assert::AreEqual(uint64_t(1), u.starveEvents);
			const std::vector<std::string> fb = rig.Lines(RE::StatusKind::Fallbacks);
			Assert::IsTrue(HasId(fb, "skeleton_queue_starve"), L"a queue-starve drop is visible, not log-only");
			Assert::IsTrue(Has(fb, "queue starve: lane lead 3 (was 5)"));
			Assert::IsTrue(rig.Command().find("1 lead fall(s)") != std::string::npos);
			// Recovery: the whole window must be above the dip before the lead rises again, then the entry releases by itself.
			for (int i = 0; i < 30; ++i)
				Collect(rig, 5);
			rig.Frame(6);
			Assert::AreEqual(5, int(rig.sk.QueueInUseOf(rig.Host()).used.laneLead));
			Assert::IsFalse(HasId(rig.Lines(RE::StatusKind::Fallbacks), "skeleton_queue_starve"));
		}

		TEST_METHOD(TheQueuePhaseIsInertWhenTheLaneIsOffOrOriginal)
		{
			Rig off("[general]\nx=1\n");
			off.ResyncApply("on");
			off.Life(RE::Lifecycle::GpuCreated);
			Collect(off, 9);
			off.Life(RE::Lifecycle::QueueDepth, 0, 3.0);
			Assert::AreEqual(0, int(off.sk.CurrentQueue().wired), L"off: nothing is published");
			Rig orig(Lane("on"));
			orig.Life(RE::Lifecycle::GpuCreated, 0);
			orig.Life(RE::Lifecycle::SettingsApplied, 1);   // the Original pathway
			Collect(orig, 9);
			orig.Life(RE::Lifecycle::QueueDepth, 0, 3.0);
			Assert::AreEqual(0, int(orig.sk.CurrentQueue().wired & (SkeletonModule::QueueShape::kLaneLead | SkeletonModule::QueueShape::kRenderAhead)), L"Original: nothing is published");
			Assert::AreEqual(0, int(orig.sk.TimingFeedOf(orig.Host()).frames));
		}

		TEST_METHOD(TheRegulatorGetsTheDerivedSlackEveryFrameAndSaysSo)
		{
			// S5: slack = measured period - render-only time (the module's own clock between Begin and AfterRender), missed = render above the period.
			Rig rig(Lane("on"));
			rig.ResyncApply("on");
			rig.Life(RE::Lifecycle::GpuCreated);
			auto frame = [&rig](int64_t seq, uint64_t renderUs) {
				rig.Ingress(seq);
				if (seq == 1)
					rig.Deliver(1);
				rig.Deliver(uint64_t(seq) + 1);
				rig.Begin(seq);   // the clock moves 40 ms per Begin
				rig.base += renderUs;
				rig.After(seq);
				rig.base -= renderUs;   // the cadence stays at 40 ms
			};
			for (int64_t s = 1; s <= 6; ++s)
				frame(s, 10000);
			SkeletonModule::TimingFeed t = rig.sk.TimingFeedOf(rig.Host());
			Assert::IsTrue(t.frames >= 4, L"every frame with a measured period is fed");
			Assert::IsTrue(t.skipped >= 1, L"the first frame has no period yet: nothing fed, counted");
			Assert::AreEqual(10.0, t.lastRenderOnlyMs, 0.1);
			Assert::AreEqual(40.0, t.lastPeriodMs, 0.5);
			Assert::AreEqual(t.lastPeriodMs - t.lastRenderOnlyMs, t.lastSlackMs, 1e-9, L"slack = period - render time");
			Assert::AreEqual(30.0, t.lastSlackMs, 0.6, L"plausible: 10 ms of render in a 40 ms cadence leaves about 30 ms");
			Assert::IsFalse(t.lastMissed);
			Assert::AreEqual(uint64_t(0), t.missed);
			// A spike the smoothed period has not caught up with: a missed deadline, negative slack.
			frame(7, 90000);
			t = rig.sk.TimingFeedOf(rig.Host());
			Assert::IsTrue(t.lastMissed);
			Assert::AreEqual(uint64_t(1), t.missed);
			Assert::IsTrue(t.lastSlackMs < 0.0);
			Assert::AreEqual(90.0, t.lastRenderOnlyMs, 0.1);
			// Visible, not log-only: the derived form is a Fallbacks entry and a Ctrl+Alt+D line.
			Assert::IsTrue(HasId(rig.Lines(RE::StatusKind::Fallbacks), "skeleton_regulator"));
			Assert::IsTrue(rig.Command().find("Skeleton regulator input: derived from render time") != std::string::npos);
			// A frame that never had its own Begin feeds nothing.
			const uint64_t fed = t.frames;
			rig.After(99);
			Assert::AreEqual(fed, rig.sk.TimingFeedOf(rig.Host()).frames);
		}

		TEST_METHOD(DeclaredVramUsersAreInThePlanTheLedgerAndTheGuard)
		{
			// MV7: the users outside the product catalog reach the skeleton's tier plan as the governor's own usage, are listed in the
			// Ctrl+Alt+D ledger, and an active module that declared nothing is a visible Fallbacks entry (skeleton_vram_users).
			VpSkeleton::VramUsers& registry = VpSkeleton::VramUsers::Instance();
			registry.ResetForTesting();
			int engineOwner = 0, cameraOwner = 0;
			registry.DeclareFixed(&engineOwner, VpSkeleton::kUserNvofProducer, 200ull * 1024ull * 1024ull);
			registry.MarkActive(&cameraOwner, VpSkeleton::kUserCameraModel, true);   // active, but nothing declared
			{
				Rig rig(Lane("on"));
				rig.ResyncApply("on");
				rig.Life(RE::Lifecycle::GpuCreated);
				for (int64_t s = 1; s <= 3; ++s)
					rig.Frame(s);
				const std::shared_ptr<VpSkeleton::LaneState> ls = rig.sk.LaneOf(rig.Host())->State();
				Assert::IsTrue(ls != nullptr);
				const VpSkeleton::UserGrant* g = ls->plan.governor.Find(VpSkeleton::kUserNvofProducer);
				Assert::IsNotNull(g, L"the declared user is in the plan");
				Assert::AreEqual(uint64_t(200) * 1024ull * 1024ull, g->bytes);
				Assert::IsTrue(rig.Command().find("VRAM T0 optical-flow engines: 200.0 MiB level 0") != std::string::npos);
				const std::vector<std::string> fb = rig.Lines(RE::StatusKind::Fallbacks);
				Assert::IsTrue(HasId(fb, "skeleton_vram_users"), L"an active module without a declaration is shown");
				Assert::IsTrue(Has(fb, "camera model"));
				registry.DeclareFixed(&cameraOwner, VpSkeleton::kUserCameraModel, 1024ull * 1024ull);
				rig.Frame(4);
				Assert::IsFalse(HasId(rig.Lines(RE::StatusKind::Fallbacks), "skeleton_vram_users"), L"declared: the entry releases");
			}
			registry.ResetForTesting();
		}

		TEST_METHOD(ExternalCameraConsumersJoinTheLaneGraphAndStartTheEngine)
		{
			// SK-W stage D: the camera link's consumers (P7 / P8 with P2 / P3 / P4 as inputs) are in the SKELETON lane graph, and declaring
			// them starts the vector engine (IVectorEngine::SetCameraFitWanted); clearing them takes both away again.
			FakeEngine eng;
			Rig rig(Lane("on"));
			rig.sk.SetEngineProviderForTesting([&eng](const void*) { return &eng; });
			rig.ResyncApply("on");
			rig.Life(RE::Lifecycle::GpuCreated);
			for (int64_t s = 1; s <= 2; ++s)
				rig.Frame(s);
			Assert::IsFalse(rig.sk.LaneOf(rig.Host())->State()->graph.Has(VpSkeleton::kP7CameraFit), L"no camera demand: no camera product");
			int owner = 0;
			rig.sk.SetExternalDemand(rig.Host(), &owner, CameraConsumers(), { VpSkeleton::kP7CameraFit, VpSkeleton::kP8CameraLabels });
			for (int64_t s = 3; s <= 4; ++s)
				rig.Frame(s);
			const std::shared_ptr<VpSkeleton::LaneState> laneState = rig.sk.LaneOf(rig.Host())->State();
			Assert::IsTrue(laneState != nullptr);
			const VpSkeleton::Graph& g = laneState->graph;
			Assert::IsTrue(g.valid);
			Assert::IsTrue(g.Has(VpSkeleton::kP7CameraFit) && g.Has(VpSkeleton::kP8CameraLabels));
			Assert::IsTrue(g.Has(VpSkeleton::kP3AnalysisField) && g.Has(VpSkeleton::kP2mMatcherPair) && g.Has(VpSkeleton::kP4SourceStats),
				L"the inputs of P7 / P8 are in the graph (P2 resolves to the matcher pair without an engine session)");
			Assert::IsTrue(eng.fitWanted, L"the camera fit asked the engine to run");
			const std::string text = rig.Command();
			Assert::IsTrue(text.find("Skeleton engine feed:") != std::string::npos);
			Assert::IsTrue(text.find("camera fit wanted") != std::string::npos);
			Assert::IsTrue(text.find("products published outside the lane:") != std::string::npos);
			Assert::IsFalse(HasId(rig.Lines(RE::StatusKind::Fallbacks), "skeleton_engine_feed"), L"an engine exists: nothing to show");
			// A second owner repeating an id is counted, the first wins.
			int other = 0;
			rig.sk.SetExternalDemand(rig.Host(), &other, CameraConsumers(), {});
			rig.Frame(5);
			Assert::IsTrue(rig.Command().find("2 duplicate id(s)") != std::string::npos);
			rig.sk.ClearExternalDemand(rig.Host(), &other);
			rig.sk.ClearExternalDemand(rig.Host(), &owner);
			rig.Frame(6);
			Assert::IsFalse(rig.sk.LaneOf(rig.Host())->State()->graph.Has(VpSkeleton::kP7CameraFit), L"withdrawn: the graph is rebuilt without it");
			Assert::IsFalse(eng.fitWanted, L"and the engine is told");
		}

		// A renderer released (or shut down) and recreated at the same address never inherits the old one's external demand.
		TEST_METHOD(AReleasedHostLosesItsExternalDemandSoARecreatedOneStartsClean)
		{
			FakeEngine eng;
			Rig rig(Lane("on"));
			rig.sk.SetEngineProviderForTesting([&eng](const void*) { return &eng; });
			rig.ResyncApply("on");
			rig.Life(RE::Lifecycle::GpuCreated);
			int ownerA = 0, ownerB = 0, otherHost = 0;
			rig.sk.SetExternalDemand(rig.Host(), &ownerA, CameraConsumers(), { VpSkeleton::kP7CameraFit });
			rig.sk.SetExternalDemand(rig.Host(), &ownerB, {}, {});
			rig.sk.SetExternalDemand(&otherHost, &ownerA, CameraConsumers(), {});
			for (int64_t s = 1; s <= 3; ++s)
				rig.Frame(s);
			Assert::AreEqual<size_t>(2, rig.sk.ExternalEntriesOf(rig.Host()));
			Assert::IsTrue(rig.sk.LaneOf(rig.Host())->State()->graph.Has(VpSkeleton::kP7CameraFit));
			// The declaring components never cleared (they died with the renderer): the module erases every entry of the host itself.
			rig.Life(RE::Lifecycle::GpuReleased);
			Assert::AreEqual<size_t>(0, rig.sk.ExternalEntriesOf(rig.Host()), L"all entries of the released host are gone");
			Assert::AreEqual<size_t>(1, rig.sk.ExternalEntriesOf(&otherHost), L"another renderer's entry is untouched");
			rig.Life(RE::Lifecycle::GpuCreated);   // a renderer at the same pointer
			Assert::AreEqual<size_t>(0, rig.sk.ExternalEntriesOf(rig.Host()), L"nothing inherited");
			// Shutdown erases too.
			rig.sk.SetExternalDemand(rig.Host(), &ownerA, CameraConsumers(), {});
			Assert::AreEqual<size_t>(1, rig.sk.ExternalEntriesOf(rig.Host()));
			rig.Life(RE::Lifecycle::Shutdown);
			Assert::AreEqual<size_t>(0, rig.sk.ExternalEntriesOf(rig.Host()));
			rig.sk.ClearExternalDemand(&otherHost, &ownerA);
			Assert::AreEqual<size_t>(0, rig.sk.ExternalEntriesOf(&otherHost));
		}

		// camera-live step 2: the demand entry a component reaches through the hub query (without including this module), on a REAL module.
		TEST_METHOD(TheExternalDemandQueryGivesTheModulesOwnSetAndClear)
		{
			FakeEngine eng;
			Rig rig(Lane("on"));
			rig.sk.SetEngineProviderForTesting([&eng](const void*) { return &eng; });
			Assert::IsNull(rig.sk.Query("vpv.something.else"), L"other ids: nothing");
			Assert::IsNull(rig.sk.Query(nullptr));
			Vpv::Skeleton::IExternalDemand* demand =
				static_cast<Vpv::Skeleton::IExternalDemand*>(rig.sk.Query(Vpv::Skeleton::kExternalDemandQueryId));
			Assert::IsNotNull(demand);
			Assert::IsTrue(static_cast<Vpv::Skeleton::IExternalDemand*>(&rig.sk) == demand, L"the module itself");
			rig.ResyncApply("on");
			rig.Life(RE::Lifecycle::GpuCreated);
			for (int64_t s = 1; s <= 2; ++s)
				rig.Frame(s);
			int owner = 0;
			demand->SetExternalDemand(rig.Host(), &owner, CameraConsumers(), { VpSkeleton::kP7CameraFit, VpSkeleton::kP8CameraLabels });
			for (int64_t s = 3; s <= 12; ++s)
				rig.Frame(s);
			VpSkeleton::AnalysisLane* lane = rig.sk.LaneOf(rig.Host());
			Assert::IsTrue(lane->State()->graph.Has(VpSkeleton::kP7CameraFit), L"declared through the interface: the graph holds P7");
			Assert::IsTrue(eng.fitWanted, L"and the engine is asked for the fit");
			// Why the camera does NOT declare yet (until SK-W2's lagged seal): nothing published P7 before the seal of its frame, so the
			// skeleton holds it absent for every frame.
			Assert::IsTrue(lane->ReadOf(VpSkeleton::kP7CameraFit, 11).state == VpSkeleton::AnalysisLane::ProductRead::None);
			demand->ClearExternalDemand(rig.Host(), &owner);
			rig.Frame(13);
			Assert::IsFalse(rig.sk.LaneOf(rig.Host())->State()->graph.Has(VpSkeleton::kP7CameraFit), L"cleared through the interface");
			Assert::IsFalse(eng.fitWanted, L"and the engine is told");
		}

		// Camera ahead fit (G-A1): the lease a publisher of an external product takes through the interface, on a REAL module. Empty
		// without a lane or without the product in the graph; the lane's own lease (same table, generation) once the demand holds P7.
		TEST_METHOD(LeaseForGivesTheLanesLeaseOfADeclaredProductAndEmptyOtherwise)
		{
			FakeEngine eng;
			Rig rig(Lane("on"));
			rig.sk.SetEngineProviderForTesting([&eng](const void*) { return &eng; });
			Vpv::Skeleton::IExternalDemand* demand =
				static_cast<Vpv::Skeleton::IExternalDemand*>(rig.sk.Query(Vpv::Skeleton::kExternalDemandQueryId));
			Assert::IsNotNull(demand);
			Assert::IsNull(demand->LeaseFor(rig.Host(), VpSkeleton::kP7CameraFit).table, L"no lane yet: empty");
			int other = 0;
			Assert::IsNull(demand->LeaseFor(&other, VpSkeleton::kP7CameraFit).table, L"an unknown host: empty");
			rig.ResyncApply("on");
			rig.Life(RE::Lifecycle::GpuCreated);
			for (int64_t s = 1; s <= 2; ++s)
				rig.Frame(s);
			Assert::IsNull(demand->LeaseFor(rig.Host(), VpSkeleton::kP7CameraFit).table, L"no demand: the graph has no P7, empty");
			int owner = 0;
			demand->SetExternalDemand(rig.Host(), &owner, CameraConsumers(), { VpSkeleton::kP7CameraFit });
			for (int64_t s = 3; s <= 6; ++s)
				rig.Frame(s);
			const VpSkeleton::LaneLease l = demand->LeaseFor(rig.Host(), VpSkeleton::kP7CameraFit);
			const VpSkeleton::LaneLease own = rig.sk.LaneOf(rig.Host())->Lease(VpSkeleton::kP7CameraFit);
			Assert::IsNotNull(l.table);
			Assert::IsTrue(l.table == own.table && l.generation == own.generation && l.generation >= 1, L"the lane's own current lease");
			Assert::IsNotNull(l.table->Find(VpSkeleton::kP7CameraFit));
			Assert::IsNotNull(l.bytes, L"with P7's bytes ring");
			demand->ClearExternalDemand(rig.Host(), &owner);
			rig.Frame(7);
			Assert::IsNull(demand->LeaseFor(rig.Host(), VpSkeleton::kP7CameraFit).table, L"cleared: empty again");
			Assert::IsNotNull(l.table->Find(VpSkeleton::kP7CameraFit), L"a held lease keeps its (old) state alive");
		}

		TEST_METHOD(WithoutAnEngineTheFeedSaysSo)
		{
			Rig rig(Lane("on"));
			rig.sk.SetEngineProviderForTesting([](const void*) -> VpMotion::IVectorEngine* { return nullptr; });
			rig.ResyncApply("on");
			rig.Life(RE::Lifecycle::GpuCreated);
			int owner = 0;
			rig.sk.SetExternalDemand(rig.Host(), &owner, CameraConsumers(), {});
			for (int64_t s = 1; s <= 4; ++s)
				rig.Frame(s);
			Assert::IsTrue(HasId(rig.Lines(RE::StatusKind::Fallbacks), "skeleton_engine_feed"), L"visible, not log-only");
			Assert::IsTrue(rig.sk.FeedCountersOf(rig.Host()).noEngine > 0);
			Assert::AreEqual(uint64_t(0), rig.sk.FeedCountersOf(rig.Host()).published);
		}

		TEST_METHOD(TheEngineFeedPublishesTheFieldSlotsWithTheFieldsOwnStamp)
		{
			FakeEngine eng;
			Rig rig(Lane("on"));
			// Observed root cause of this test's failure (cdb on the built test, 2026-11-08): with no card the lane grants NO product a lane (LaneRunner::Step
			// answered NotLaned for P1 and P4 on every call), so P1(5) is only made by the live fill at the seal of frame 5, after the feed's 8 attempts.
			// There is no render device in a unit test: give the lane a card with room (as AnAheadResultIsTheSameBytesAsTheLiveForm does), and step the
			// CPU worker by hand (no threads) so the P1 inputs of the field are there deterministically.
			VpSkeleton::CardProfile card;
			card.vramBudgetBytes = 1ull << 30;
			card.systemRamFreeBytes = 1ull << 30;
			card.uploadGBps = 10.0;
			rig.sk.SetCardForTesting(card);
			rig.sk.SetWorkerThreadsForTesting(false);
			rig.sk.SetEngineProviderForTesting([&eng](const void*) { return &eng; });
			rig.ResyncApply("on");
			rig.Life(RE::Lifecycle::GpuCreated);
			int owner = 0;
			rig.sk.SetExternalDemand(rig.Host(), &owner, CameraConsumers(), { VpSkeleton::kP7CameraFit, VpSkeleton::kP8CameraLabels });
			for (int64_t s = 1; s <= 3; ++s)
				rig.Frame(s);
			// The engine made the field of the pair (4, 5) while frame 4 renders: the stamp it was computed under is the Indicator's stamp of 5
			// (MotionValidity::StampAt). The module publishes P2m / P3 of frame 5 at the AfterRender of frame 4.
			std::shared_ptr<VpIndicator::Indicator> ind = Vpv::Resync::ResyncExtension::For(rig.Host())->seal.Current();
			Assert::IsTrue(ind != nullptr);
			eng.latest = 5;
			eng.pairFrom = 4;
			eng.stamp = ind->StampOf(5);
			rig.Frame(4);
			VpSkeleton::AnalysisLane* feedLane = rig.sk.LaneOf(rig.Host());
			Assert::IsNotNull(feedLane);
			for (int i = 0; i < 6 && rig.sk.FeedCountersOf(rig.Host()).published == 0; ++i)
			{
				int steps = 0;
				while (feedLane->StepWorker(VpSkeleton::Backend::Cpu, 0) && ++steps < 1000)
				{
				}
				rig.sk.RetryEngineFeedForTesting(rig.Host());   // the P1 inputs were made by the CPU worker stepped above
			}
			const Vpv::Skeleton::FeedCounters fc = rig.sk.FeedCountersOf(rig.Host());
			Assert::AreEqual(uint64_t(1), fc.published);
			Assert::AreEqual(uint64_t(5), fc.lastSeq);
			Vpv::Skeleton::EngineFieldFacts f;
			Assert::IsTrue(rig.sk.EngineFieldFactsOf(rig.Host(), 5, f));
			Assert::AreEqual(uint64_t(4), f.pairFrom);
			Assert::AreEqual(uint64_t(5), f.pairTo);
			Assert::AreEqual(4, f.gridW);
			Assert::AreEqual(16, f.blockSize);
			Assert::IsTrue(f.globalValid);
			Assert::AreEqual(1.5, f.globalX, 1e-12);
			Assert::IsTrue(f.p2mPublished && f.p3Published);
			Assert::IsFalse(f.p2Published, L"no optical-flow session in this graph: the pair is P2m");
			Assert::IsFalse(rig.sk.EngineFieldFactsOf(rig.Host(), 4, f), L"no field was made for frame 4");
			// The seal of frame 5 accepts what was published under the stamp the field was made under: a sealed slot is readable.
			rig.Frame(5);
			VpSkeleton::AnalysisLane* lane = rig.sk.LaneOf(rig.Host());
			Assert::IsTrue(lane->ReadOf(VpSkeleton::kP3AnalysisField, 5).state != VpSkeleton::AnalysisLane::ProductRead::None, L"P3(5) sealed");
			Assert::IsTrue(lane->ReadOf(VpSkeleton::kP2mMatcherPair, 5).state != VpSkeleton::AnalysisLane::ProductRead::None, L"P2m(5) sealed");
			// A field whose pair spans a slip is not published (the engine's facade says so) and is counted.
			eng.latest = 7;
			eng.pairFrom = 6;
			eng.slip = true;
			rig.Frame(6);
			Assert::AreEqual(uint64_t(1), rig.sk.FeedCountersOf(rig.Host()).slip);
			Assert::IsFalse(rig.sk.EngineFieldFactsOf(rig.Host(), 7, f));
			// The engine's numbering restarted (seek / resync): counted, nothing carried over.
			eng.slip = false;
			eng.era = 2;
			eng.latest = 0;
			rig.Frame(7);
			Assert::AreEqual(uint64_t(1), rig.sk.FeedCountersOf(rig.Host()).eraResets);
			Assert::IsTrue(rig.Command().find("1 slipped") != std::string::npos);
		}

		// W1-C7: the scene is a skeleton product. The classification is still the shared unit's; the module now also writes it once per frame
		// into the frame-analysis store's scene group (at the seal) and every reader asks SceneOf.
		static FrameAnalysis::Identity SceneTestId(uint64_t seq)
		{
			FrameAnalysis::Identity id;
			id.seq = seq;
			return id;
		}

		// Takes the store off the tap registry again however the test ends (the registry keeps the raw pointer by renderer address).
		struct SceneStoreGuard
		{
			const void* host;
			explicit SceneStoreGuard(const void* h) : host(h) {}
			~SceneStoreGuard() { Tap::SetStore(host, nullptr); }
		};

		TEST_METHOD(ASceneIsPublishedOnceAtTheSealAndServedFromTheStore)
		{
			Rig rig(Lane("on"));
			FrameAnalysis::Store store(FrameAnalysis::RingDepth(4, 4));
			Tap::SetStore(rig.Host(), &store);
			SceneStoreGuard guard(rig.Host());
			rig.ResyncApply("on");
			rig.Life(RE::Lifecycle::GpuCreated);
			for (int64_t s = 1; s <= 3; ++s)
				rig.Frame(s);
			rig.Ingress(4);
			rig.Deliver(5);
			store.Begin(SceneTestId(4), FrameAnalysis::kScene);   // the record the source tap opens at arrival
			const Vpv::Skeleton::SceneCounters before = rig.sk.SceneCountersOf(rig.Host());
			rig.Begin(4);
			const Vpv::Skeleton::SceneCounters afterSeal = rig.sk.SceneCountersOf(rig.Host());
			Assert::AreEqual(before.published + 1, afterSeal.published, L"the seal wrote the scene group once");
			FrameAnalysis::Record rec;
			Assert::IsTrue(store.Read(4, FrameAnalysis::Use::SingleFrame, FrameAnalysis::kScene, rec) == FrameAnalysis::ReadStatus::Ok);
			Assert::IsTrue(rec.scene.origin == FrameAnalysis::SceneOrigin::Vp);
			Vpv::Skeleton::SceneFacts f;
			Assert::IsTrue(rig.sk.SceneFactsOf(rig.Host(), 4, f));
			Assert::AreEqual(f.cut, rec.scene.cut, L"the stored value is the classification");
			Assert::AreEqual(f.fade, rec.scene.fade);
			// The reader: from the store, nothing recomputed (no second write, no live fill).
			const Vpv::Skeleton::SceneAnswer a = rig.sk.SceneOf(rig.Host(), 4);
			Assert::IsTrue(a.known);
			Assert::IsTrue(a.from == Vpv::Skeleton::SceneAnswer::From::Store);
			Assert::AreEqual(f.cut, a.cut);
			Assert::AreEqual(f.fade, a.fade);
			const Vpv::Skeleton::SceneCounters done = rig.sk.SceneCountersOf(rig.Host());
			Assert::AreEqual(afterSeal.published, done.published);
			Assert::AreEqual(afterSeal.liveFills, done.liveFills);
			Assert::AreEqual(afterSeal.storeReads + 1, done.storeReads);
			rig.After(4);
		}

		TEST_METHOD(ASceneAlreadyInTheStoreIsNotComputedAgain)
		{
			Rig rig(Lane("on"));
			FrameAnalysis::Store store(FrameAnalysis::RingDepth(4, 4));
			Tap::SetStore(rig.Host(), &store);
			SceneStoreGuard guard(rig.Host());
			rig.ResyncApply("on");
			rig.Life(RE::Lifecycle::GpuCreated);
			for (int64_t s = 1; s <= 3; ++s)
				rig.Frame(s);
			rig.Ingress(4);
			rig.Deliver(5);
			store.Begin(SceneTestId(4), FrameAnalysis::kScene);
			FrameAnalysis::Scene given;   // a value another producer wrote first (labelled): the skeleton must not overwrite it
			given.cut = true;
			given.origin = FrameAnalysis::SceneOrigin::BillCopied;
			Assert::IsTrue(store.PublishScene(4, given));
			const Vpv::Skeleton::SceneCounters before = rig.sk.SceneCountersOf(rig.Host());
			rig.Begin(4);
			const Vpv::Skeleton::SceneCounters after = rig.sk.SceneCountersOf(rig.Host());
			Assert::AreEqual(before.alreadyPresent + 1, after.alreadyPresent);
			Assert::AreEqual(before.published, after.published, L"compute once / skip if present");
			FrameAnalysis::Scene kept;
			Assert::IsTrue(store.PeekScene(4, kept));
			Assert::IsTrue(kept.origin == FrameAnalysis::SceneOrigin::BillCopied);
			Assert::IsTrue(kept.cut);
			rig.After(4);
		}

		TEST_METHOD(ASceneOfGivesTheSameAnswerWithTheStoreOffAndFillsALateRecord)
		{
			// Store off: every answer is the live fill, same cut / fade as the stored product.
			Rig off(Lane("on"));
			off.ResyncApply("on");
			off.Life(RE::Lifecycle::GpuCreated);
			for (int64_t s = 1; s <= 3; ++s)
				off.Frame(s);
			off.Ingress(4);
			off.Deliver(5);
			off.Begin(4);
			const Vpv::Skeleton::SceneAnswer live = off.sk.SceneOf(off.Host(), 4);
			Assert::IsTrue(live.known);
			Assert::IsTrue(live.from == Vpv::Skeleton::SceneAnswer::From::Live);
			Vpv::Skeleton::SceneFacts f;
			Assert::IsTrue(off.sk.SceneFactsOf(off.Host(), 4, f));
			Assert::AreEqual(f.cut, live.cut);
			Assert::AreEqual(f.fade, live.fade);
			const Vpv::Skeleton::SceneCounters c = off.sk.SceneCountersOf(off.Host());
			Assert::AreEqual(uint64_t(0), c.published, L"no store: nothing written, no work at the seal");
			Assert::AreEqual(uint64_t(0), c.notReady);
			Assert::AreEqual(uint64_t(1), c.liveFills);
			Assert::IsFalse(off.sk.SceneOf(off.Host(), 1).known, L"there is no frame before 1");
			Assert::AreEqual(uint64_t(1), off.sk.SceneCountersOf(off.Host()).unknown);
			off.After(4);

			// Store on, but the frame's record opens AFTER the seal: the seal found no record to write (nothing stored), the reader fills it
			// live (same answer) and the next reader is served from the store.
			Rig on(Lane("on"));
			FrameAnalysis::Store store(FrameAnalysis::RingDepth(4, 4));
			Tap::SetStore(on.Host(), &store);
			SceneStoreGuard guard(on.Host());
			on.ResyncApply("on");
			on.Life(RE::Lifecycle::GpuCreated);
			for (int64_t s = 1; s <= 3; ++s)
				on.Frame(s);
			on.Ingress(4);
			on.Deliver(5);
			on.Begin(4);
			store.Begin(SceneTestId(4), FrameAnalysis::kScene);
			const Vpv::Skeleton::SceneAnswer first = on.sk.SceneOf(on.Host(), 4);
			Assert::IsTrue(first.known);
			Assert::IsTrue(first.from == Vpv::Skeleton::SceneAnswer::From::Live);
			const Vpv::Skeleton::SceneAnswer second = on.sk.SceneOf(on.Host(), 4);
			Assert::IsTrue(second.from == Vpv::Skeleton::SceneAnswer::From::Store);
			Assert::AreEqual(first.cut, second.cut);
			Assert::AreEqual(first.fade, second.fade);
			Assert::AreEqual(live.cut, first.cut, L"the same answer with the store on and off");
			Assert::AreEqual(live.fade, first.fade);
			on.After(4);
		}

		// Frame 3 is FLAT and dark, frame 4 is bright texture: a picture change with a spread jump (a cut, not a flash). The random-noise
		// frames the rig starts with never cut (every pair is alike), so the cut equivalence needs frames made on purpose. Call before any frame
		// is delivered.
		static void ShapeACutAtFrame4(Rig& rig)
		{
			uint32_t x = 4242u;
			for (uint32_t i = 0; i < kW * kH; ++i)
			{
				rig.store[size_t(3 % rig.count) * kW * kH + i] = 100;
				x = x * 1664525u + 1013904223u;
				rig.store[size_t(4 % rig.count) * kW * kH + i] = uint16_t(600u + ((x >> 12) % 400u));
			}
		}

		static void RunToTheSealOfFrame4(Rig& rig)
		{
			rig.ResyncApply("on");
			rig.Life(RE::Lifecycle::GpuCreated);
			for (int64_t s = 1; s <= 3; ++s)
				rig.Frame(s);
			rig.Ingress(4);
			rig.Deliver(5);
		}

		TEST_METHOD(ARealCutGivesTheSameAnswerFromTheStoreTheLiveFillAndWithTheStoreOff)
		{
			// 1. Store on, the record open at the seal: published, then served from the store.
			Rig stored(Lane("on"));
			ShapeACutAtFrame4(stored);
			FrameAnalysis::Store store(FrameAnalysis::RingDepth(4, 4));
			Tap::SetStore(stored.Host(), &store);
			SceneStoreGuard g1(stored.Host());
			RunToTheSealOfFrame4(stored);
			store.Begin(SceneTestId(4), FrameAnalysis::kScene);
			stored.Begin(4);
			const Vpv::Skeleton::SceneAnswer fromStore = stored.sk.SceneOf(stored.Host(), 4);
			Assert::IsTrue(fromStore.known);
			Assert::IsTrue(fromStore.from == Vpv::Skeleton::SceneAnswer::From::Store);
			Assert::IsTrue(fromStore.cut, L"flat then bright texture is a cut");
			FrameAnalysis::Scene inStore;
			Assert::IsTrue(store.PeekScene(4, inStore));
			Assert::IsTrue(inStore.cut, L"what the seal wrote is a cut too");
			stored.After(4);

			// 2. Store on, the record opens AFTER the seal: the reader classifies live (same answer).
			Rig late(Lane("on"));
			ShapeACutAtFrame4(late);
			FrameAnalysis::Store lateStore(FrameAnalysis::RingDepth(4, 4));
			Tap::SetStore(late.Host(), &lateStore);
			SceneStoreGuard g2(late.Host());
			RunToTheSealOfFrame4(late);
			late.Begin(4);
			lateStore.Begin(SceneTestId(4), FrameAnalysis::kScene);
			const Vpv::Skeleton::SceneAnswer fromLive = late.sk.SceneOf(late.Host(), 4);
			Assert::IsTrue(fromLive.known);
			Assert::IsTrue(fromLive.from == Vpv::Skeleton::SceneAnswer::From::Live);
			Assert::IsTrue(fromLive.cut);
			late.After(4);

			// 3. No store at all.
			Rig off(Lane("on"));
			ShapeACutAtFrame4(off);
			RunToTheSealOfFrame4(off);
			off.Begin(4);
			const Vpv::Skeleton::SceneAnswer storeOff = off.sk.SceneOf(off.Host(), 4);
			Assert::IsTrue(storeOff.known);
			Assert::IsTrue(storeOff.from == Vpv::Skeleton::SceneAnswer::From::Live);
			Assert::IsTrue(storeOff.cut);
			Assert::AreEqual(fromStore.fade, fromLive.fade);
			Assert::AreEqual(fromLive.fade, storeOff.fade);

			// 4. A store that is registered but DISABLED (what frame_analysis_store=false leaves): the same answer again.
			Rig disabled(Lane("on"));
			ShapeACutAtFrame4(disabled);
			FrameAnalysis::Store offStore(FrameAnalysis::RingDepth(4, 4));
			offStore.SetEnabled(false);
			Tap::SetStore(disabled.Host(), &offStore);
			SceneStoreGuard g4(disabled.Host());
			RunToTheSealOfFrame4(disabled);
			disabled.Begin(4);
			const Vpv::Skeleton::SceneAnswer fromDisabled = disabled.sk.SceneOf(disabled.Host(), 4);
			Assert::IsTrue(fromDisabled.known);
			Assert::IsTrue(fromDisabled.cut);
			Assert::AreEqual(fromLive.fade, fromDisabled.fade);
			disabled.After(4);

		}

		TEST_METHOD(ARegisteredButDisabledStoreCostsNothingAtTheSeal)
		{
			Rig rig(Lane("on"));
			FrameAnalysis::Store store(FrameAnalysis::RingDepth(4, 4));
			store.SetEnabled(false);   // the Host registers its store even with frame_analysis_store=false, then disabled
			Tap::SetStore(rig.Host(), &store);
			SceneStoreGuard guard(rig.Host());
			RunToTheSealOfFrame4(rig);
			rig.Begin(4);
			const Vpv::Skeleton::SceneCounters c = rig.sk.SceneCountersOf(rig.Host());
			Assert::AreEqual(uint64_t(0), c.liveFills, L"no classification at the seal");
			Assert::AreEqual(uint64_t(0), c.published);
			Assert::AreEqual(uint64_t(0), c.notReady);
			Assert::AreEqual(uint64_t(0), c.alreadyPresent);
			Assert::AreEqual(uint64_t(0), c.cacheHits);
			// A reader still gets the live answer, classified once for the frame.
			const Vpv::Skeleton::SceneAnswer a = rig.sk.SceneOf(rig.Host(), 4);
			Assert::IsTrue(a.known);
			Assert::IsTrue(a.from == Vpv::Skeleton::SceneAnswer::From::Live);
			const Vpv::Skeleton::SceneAnswer b = rig.sk.SceneOf(rig.Host(), 4);
			Assert::AreEqual(a.cut, b.cut);
			const Vpv::Skeleton::SceneCounters d = rig.sk.SceneCountersOf(rig.Host());
			Assert::AreEqual(uint64_t(1), d.liveFills, L"classified once");
			Assert::AreEqual(uint64_t(1), d.cacheHits, L"the second call used the frame's classification");
			Assert::AreEqual(uint64_t(0), d.published, L"nothing written into a disabled store");
			FrameAnalysis::Scene scene;
			Assert::IsFalse(store.PeekScene(4, scene));
			rig.After(4);
		}

		TEST_METHOD(ASealWithNoRecordToWriteIntoCountsItAndClassifiesNothing)
		{
			Rig rig(Lane("on"));
			FrameAnalysis::Store store(FrameAnalysis::RingDepth(4, 4));   // enabled, but the frame's record never opens
			Tap::SetStore(rig.Host(), &store);
			SceneStoreGuard guard(rig.Host());
			RunToTheSealOfFrame4(rig);
			const uint64_t notReadyBefore = rig.sk.SceneCountersOf(rig.Host()).notReady;
			rig.Begin(4);
			const Vpv::Skeleton::SceneCounters c = rig.sk.SceneCountersOf(rig.Host());
			Assert::AreEqual(notReadyBefore + 1, c.notReady, L"counted once at the seal");
			Assert::AreEqual(uint64_t(0), c.liveFills, L"and not classified for nothing");
			Assert::AreEqual(uint64_t(0), c.published);
			// Readers: classified once, then the frame's answer is reused (no re-classification per call).
			Assert::IsTrue(rig.sk.SceneOf(rig.Host(), 4).known);
			Assert::IsTrue(rig.sk.SceneOf(rig.Host(), 4).known);
			Assert::IsTrue(rig.sk.SceneOf(rig.Host(), 4).known);
			const Vpv::Skeleton::SceneCounters d = rig.sk.SceneCountersOf(rig.Host());
			Assert::AreEqual(uint64_t(1), d.liveFills);
			Assert::AreEqual(uint64_t(2), d.cacheHits);
			Assert::AreEqual(c.notReady, d.notReady, L"the seal's count does not grow with the readers");
			rig.After(4);
		}

		TEST_METHOD(ASceneIsDerivedFromTwoP4Records)
		{
			Rig rig(Lane("on"));
			rig.ResyncApply("on");
			rig.Life(RE::Lifecycle::GpuCreated);
			for (int64_t s = 1; s <= 3; ++s)
				rig.Frame(s);
			rig.Ingress(4);
			rig.Deliver(5);
			rig.Begin(4);   // P4(4) is sealed; P4(3) is still in its ring
			Vpv::Skeleton::SceneFacts f;
			Assert::IsTrue(rig.sk.SceneFactsOf(rig.Host(), 4, f));
			Assert::IsTrue(f.valid);
			Assert::IsFalse(f.fullBetween, L"no resync event between the frames");
			// The same classification the shared unit gives for those two records (U7): SceneFactsOf derives it.
			VpSkeleton::AnalysisLane* lane = rig.sk.LaneOf(rig.Host());
			const VpSkeleton::AnalysisLane::ProductRead cur = lane->ReadOf(VpSkeleton::kP4SourceStats, 4);
			const VpSkeleton::AnalysisLane::ProductRead prev = lane->ReadOf(VpSkeleton::kP4SourceStats, 3);
			VpLaneProducts::SourceStatsRecord a, c;
			Assert::IsTrue(VpLaneProducts::ReadSourceStats(prev.cpu, prev.cpuBytes, &a) && VpLaneProducts::ReadSourceStats(cur.cpu, cur.cpuBytes, &c));
			const VpLaneProducts::EventSignals e = VpLaneProducts::ClassifyEvents(a, c, false, false);
			Assert::AreEqual(e.cut, f.cut);
			Assert::AreEqual(e.flash, f.flash);
			Assert::AreEqual(e.fade, f.fade);
			Assert::AreEqual(e.cutPossible, f.cutPossible);
			Assert::AreEqual(e.histDistance, f.histDistance);
			Assert::IsTrue(f.histDistance > 0u, L"two different noise frames never have identical histograms");
			Assert::AreEqual(c.mean, f.curMean);
			Assert::AreEqual(a.mean, f.prevMean);
			Assert::IsTrue(f.curSpread > 0u);
			rig.After(4);
			Assert::IsFalse(rig.sk.SceneFactsOf(rig.Host(), 1, f), L"there is no frame before 1");
		}

		TEST_METHOD(AClaimFailureIsExplainedAndTheBackwardModeSaysItIsExpected)
		{
			Assert::IsTrue(Vpv::Skeleton::ClaimFailedNote(0, true).empty(), L"nothing to explain");
			Assert::IsTrue(Vpv::Skeleton::ClaimFailedNote(0, false).empty());
			const std::string backward = Vpv::Skeleton::ClaimFailedNote(3, true);
			Assert::IsTrue(backward.find("3 pair(s) arrived after the seal") != std::string::npos, L"the count and the cause");
			Assert::IsTrue(backward.find("backward / low-latency") != std::string::npos);
			Assert::IsTrue(backward.find("expected in this mode") != std::string::npos);
			const std::string ahead = Vpv::Skeleton::ClaimFailedNote(2, false);
			Assert::IsTrue(ahead.find("2 field(s) not published") != std::string::npos);
			Assert::IsTrue(ahead.find("left the ring") != std::string::npos);
			Assert::IsTrue(ahead.find("expected in this mode") == std::string::npos, L"with a lead it is not the mode's normal behaviour");
		}

		TEST_METHOD(AnObservedSpanWithAWaitInsideNeverShedsAnything)
		{
			// Fix 3 of the stage A-C audit: the Begin..AfterRender span includes the render lock wait and pacing sleeps, so it is NOT a render-only time.
			// Every frame here spends 5 periods inside the span (a sleep / lock wait): the regulator must not be fed and must shed nothing.
			Rig rig(Lane("on"));
			rig.ResyncApply("on");
			rig.Life(RE::Lifecycle::GpuCreated);
			auto frame = [&rig](int64_t seq, uint64_t spanUs) {
				rig.Ingress(seq);
				if (seq == 1)
					rig.Deliver(1);
				rig.Deliver(uint64_t(seq) + 1);
				rig.Begin(seq);
				rig.base += spanUs;   // the wait inside the span
				rig.After(seq);
				rig.base -= spanUs;
			};
			for (int64_t s = 1; s <= 150; ++s)
				frame(s, 200000);
			SkeletonModule::TimingFeed t = rig.sk.TimingFeedOf(rig.Host());
			Assert::IsTrue(t.frames > 100, L"the span is observed every frame");
			Assert::IsTrue(t.missed > 100, L"and is far above the period");
			Assert::AreEqual(uint64_t(0), t.fed, L"but the regulator is not fed with it");
			Assert::IsFalse(t.measured);
			VpSkeleton::AnalysisLane* lane = rig.sk.LaneOf(rig.Host());
			Assert::AreEqual(int(lane->GetRegulator().MaxLevel()), int(lane->GetRegulator().Level()), L"nothing stepped down");
			Assert::IsTrue(lane->GetRegulator().Dropped().empty());
			VpSkeleton::RegTrace traces[64];
			const size_t n = lane->RegulatorTraces(traces, 64);
			for (size_t i = 0; i < n; ++i)
			{
				Assert::IsTrue(traces[i].action != VpSkeleton::RegAction::BackOff && traces[i].action != VpSkeleton::RegAction::Drop &&
					traces[i].action != VpSkeleton::RegAction::MoveToRender && traces[i].action != VpSkeleton::RegAction::ForcedLive,
					L"no shedding action on an observed span");
			}
			Assert::IsTrue(HasId(rig.Lines(RE::StatusKind::Fallbacks), "skeleton_regulator"), L"visible: observe only, shedding off");
			Assert::IsTrue(rig.Command().find("(none: shedding off)") != std::string::npos);
			// A span the HOST measured is what the regulator takes once the gate is set to act (B4 S2: the default observes): counted, and the
			// observe-only entry releases at the next status refresh.
			Assert::IsFalse(rig.sk.NoteRenderOnlyMs(rig.Host(), 150, -1.0), L"a negative time is no measurement");
			int other = 0;
			Assert::IsFalse(rig.sk.NoteRenderOnlyMs(&other, 150, 5.0), L"no lane for that host");
			rig.sk.SetRegulatorGate(SkeletonModule::RegulatorGate::Act);
			for (int i = 0; i < 3; ++i)
				Assert::IsTrue(rig.sk.NoteRenderOnlyMs(rig.Host(), uint64_t(150 + i), 5.0));
			t = rig.sk.TimingFeedOf(rig.Host());
			Assert::AreEqual(uint64_t(3), t.fed);
			Assert::AreEqual(uint64_t(3), t.noted);
			Assert::IsTrue(t.measured);
			for (int64_t s = 151; s <= 170; ++s)
				frame(s, 1000);
			Assert::IsFalse(HasId(rig.Lines(RE::StatusKind::Fallbacks), "skeleton_regulator"), L"a measured span is in use");
		}

		// B4 S2 (doc 96 section 4): calling NoteRenderOnlyMs used to switch shedding on. With the default gate (observe) a host-measured span far
		// above the period is recorded and shown but the regulator is never fed and sheds nothing; the act gate sheds as before.
		TEST_METHOD(TheObserveGateRecordsTheMeasuredSpanAndNeverSheds)
		{
			for (int act = 0; act < 2; ++act)
			{
				Rig rig(Lane("on"));
				rig.ResyncApply("on");
				rig.Life(RE::Lifecycle::GpuCreated);
				auto frame = [&rig](int64_t s) {
					rig.Ingress(s);
					if (s == 1)
						rig.Deliver(1);
					rig.Deliver(uint64_t(s) + 1);
					rig.Begin(s);
					rig.After(s);
				};
				for (int64_t s = 1; s <= 150; ++s)
					frame(s);
				Assert::IsTrue(rig.sk.GetRegulatorGate() == SkeletonModule::RegulatorGate::Observe, L"the default observes");
				if (act)
					rig.sk.SetRegulatorGate(SkeletonModule::RegulatorGate::Act);
				VpSkeleton::AnalysisLane* lane = rig.sk.LaneOf(rig.Host());
				Assert::IsTrue(lane != nullptr);
				Assert::IsTrue(lane->GetRegulator().MaxLevel() > 0, L"something to shed");
				const uint64_t n = 4 * uint64_t(lane->GetRegulator().WindowFrames());
				for (uint64_t i = 0; i < n; ++i)
					Assert::IsTrue(rig.sk.NoteRenderOnlyMs(rig.Host(), 151 + i, 1.0e6), L"a span far above the period");
				for (int64_t s = 151; s <= 170; ++s)
					frame(s);   // the next status refresh
				const SkeletonModule::TimingFeed t = rig.sk.TimingFeedOf(rig.Host());
				Assert::AreEqual(n, t.noted, L"the counter moves in both gates");
				Assert::AreEqual(1.0e6, t.lastNotedMs);
				VpSkeleton::RegTrace traces[256];
				const size_t k = lane->RegulatorTraces(traces, 256);
				size_t backOffs = 0;
				for (size_t i = 0; i < k; ++i)
					backOffs += traces[i].action == VpSkeleton::RegAction::BackOff ? 1u : 0u;
				if (!act)
				{
					Assert::AreEqual(uint64_t(0), t.fed, L"observe: the regulator is not fed");
					Assert::IsFalse(t.measured);
					Assert::AreEqual(int(lane->GetRegulator().MaxLevel()), int(lane->GetRegulator().Level()), L"observe: nothing shed");
					Assert::AreEqual(size_t(0), backOffs);
					Assert::IsTrue(Has(rig.Lines(RE::StatusKind::Fallbacks), "host-measured render-only frame(s) recorded"), L"visible: recorded, not fed");
					Assert::IsTrue(rig.Command().find("Skeleton regulator gate: observe") != std::string::npos, L"Ctrl+Alt+D");
				}
				else
				{
					Assert::AreEqual(n, t.fed, L"act: fed as before");
					Assert::IsTrue(t.measured);
					Assert::IsTrue(lane->GetRegulator().Level() < lane->GetRegulator().MaxLevel(), L"act: the regulator sheds as today");
					Assert::IsTrue(backOffs > 0);
					Assert::IsTrue(rig.Command().find("Skeleton regulator gate: act") != std::string::npos);
				}
			}
		}

		TEST_METHOD(TheRenderAheadTermIsNeverLeftWiredWithAnOldValue)
		{
			// Fix 2: the host calls QueueDepth only while a render-ahead plan runs. No value (a negative one: no source period) unwires at once; a plan that
			// stops (render-ahead switched off) is noticed after 8 source periods and unwires the term too.
			Rig rig(Lane("on"));
			rig.ResyncApply("on");
			rig.Life(RE::Lifecycle::GpuCreated);
			for (int64_t s = 1; s <= 3; ++s)
				rig.Frame(s);
			rig.Life(RE::Lifecycle::QueueDepth, 0, 2.2);
			Assert::AreEqual(3, int(rig.sk.CurrentQueue().renderAhead));
			Assert::IsTrue((rig.sk.CurrentQueue().wired & SkeletonModule::QueueShape::kRenderAhead) != 0);
			rig.Life(RE::Lifecycle::QueueDepth, 0, -1.0);   // sourcePeriod <= 0 on the host
			Assert::IsTrue((rig.sk.CurrentQueue().wired & SkeletonModule::QueueShape::kRenderAhead) == 0, L"no value: unwired, not zero-wired");
			Assert::AreEqual(0, int(rig.sk.CurrentQueue().renderAhead));
			rig.Life(RE::Lifecycle::QueueDepth, 0, 2.2);
			for (int64_t s = 4; s <= 6; ++s)
				rig.Frame(s);
			Assert::IsTrue((rig.sk.CurrentQueue().wired & SkeletonModule::QueueShape::kRenderAhead) != 0, L"a plan 3 frames ago is current");
			for (int64_t s = 7; s <= 20; ++s)
				rig.Frame(s);   // the plan stopped: 14 periods without a QueueDepth fact
			Assert::IsTrue((rig.sk.CurrentQueue().wired & SkeletonModule::QueueShape::kRenderAhead) == 0, L"stale: unwired");
			Assert::IsTrue(Has(rig.Lines(RE::StatusKind::Fallbacks), "render-ahead depth"), L"and the unwired term is shown again");
		}

		TEST_METHOD(ALaneLeadThatStaysLowIsTheNewNormalAfterTheHoldTime)
		{
			// Fix 5: the starve peak decays. A dip is shown while it is recent; a lead that stays low for 10 s of real time stops being a starve.
			Rig rig(Lane("on"));
			rig.ResyncApply("on");
			rig.Life(RE::Lifecycle::GpuCreated);
			for (int64_t s = 1; s <= 3; ++s)
				rig.Frame(s);
			for (int i = 0; i < 30; ++i)
				Collect(rig, 5);
			rig.Frame(4);
			Collect(rig, 3);
			for (int i = 0; i < 30; ++i)
				Collect(rig, 3);   // the window is 3 from here on
			rig.Frame(5);
			Assert::IsTrue(HasId(rig.Lines(RE::StatusKind::Fallbacks), "skeleton_queue_starve"), L"a recent dip is shown");
			for (int64_t s = 6; s <= 300; ++s)
			{
				Collect(rig, 3);
				rig.Frame(s);   // 12 s at 25 fps
			}
			const SkeletonModule::QueueInUse u = rig.sk.QueueInUseOf(rig.Host());
			Assert::AreEqual(3, int(u.used.laneLead));
			Assert::AreEqual(3u, u.leadPeak, L"the peak decayed to the lead in use");
			Assert::IsFalse(HasId(rig.Lines(RE::StatusKind::Fallbacks), "skeleton_queue_starve"), L"released by itself");
		}

		TEST_METHOD(TheLaneLeadWindowIsRealTimeWhateverTheCallRate)
		{
			// Fix 4: samples are timestamped. Ten calls inside one frame and one call per frame give the same window: the 0 ages out 0.5 s after it was seen.
			Rig rig(Lane("on"));
			rig.ResyncApply("on");
			rig.Life(RE::Lifecycle::GpuCreated);
			for (int64_t s = 1; s <= 3; ++s)
				rig.Frame(s);
			RE::QueueView view;
			RE::FrameContext c;
			c.host = rig.Host(); c.phase = RE::FramePhase::CollectAhead; c.phaseData = &view;
			view.size = 0;
			rig.sk.OnFrame(c);   // a 0 at t
			view.size = 9;
			for (int i = 0; i < 40; ++i)
				rig.sk.OnFrame(c);   // forty calls with no time between (a burst): the 0 is still inside the window
			Assert::AreEqual(0, int(rig.sk.CurrentQueue().laneLead));
			rig.base += 600000;   // 0.6 s later
			rig.sk.OnFrame(c);
			Assert::AreEqual(9, int(rig.sk.CurrentQueue().laneLead), L"the 0 is older than 0.5 s of real time: gone");
		}

		TEST_METHOD(TheKnownUndeclaredVramUsersAreAStandingLine)
		{
			Rig rig(Lane("on"));
			rig.ResyncApply("on");
			rig.Life(RE::Lifecycle::GpuCreated);
			for (int64_t s = 1; s <= 3; ++s)
				rig.Frame(s);
			const std::vector<std::string> fb = rig.Lines(RE::StatusKind::Fallbacks);
			Assert::IsTrue(HasId(fb, "skeleton_vram_known"), L"MV7 is never invisible: the gaps are named");
			Assert::IsTrue(Has(fb, "temporal detail") && Has(fb, "texture restore"));
		}

		TEST_METHOD(AShedUserThatStillHoldsItsTextureIsPlannedAsTransient)
		{
			// Fix 9: the plan sheds a stepped user but its module has not freed the texture. The bytes it still holds above its grant are planned as transient
			// until the module Withdraws, so the products are not granted occupied memory; the pending figure follows the real free, never the plan.
			VpSkeleton::VramUsers& registry = VpSkeleton::VramUsers::Instance();
			registry.ResetForTesting();
			const uint64_t mb = 1024ull * 1024ull;
			VpSkeleton::VramUser u;
			u.id = VpSkeleton::kUserBasicChroma;
			u.name = VpSkeleton::VramUserName(VpSkeleton::kUserBasicChroma);
			u.tier = VpSkeleton::Tier::T1;
			u.levels.push_back(200 * mb);
			u.levelText.push_back(nullptr);
			u.levels.push_back(0);
			u.levelText.push_back("off");
			int owner = 0;
			registry.Declare(&owner, u, 200 * mb);
			{
				Rig rig(Lane("on"));
				VpSkeleton::CardProfile card;
				card.key.luid = 77;
				card.vramBudgetBytes = 100 * mb;   // less than the user alone
				card.vramUsageBytes = 200 * mb;    // and the user's 200 MiB are inside the usage (VP's own)
				rig.sk.SetCardForTesting(card);
				rig.ResyncApply("on");
				rig.Life(RE::Lifecycle::GpuCreated);
				for (int64_t s = 1; s <= 4; ++s)
					rig.Frame(s);
				const std::shared_ptr<VpSkeleton::LaneState> ls = rig.sk.LaneOf(rig.Host())->State();
				Assert::IsTrue(ls != nullptr);
				const VpSkeleton::UserGrant* g = ls->plan.governor.Find(VpSkeleton::kUserBasicChroma);
				Assert::IsNotNull(g);
				Assert::IsTrue(g->shed, L"over budget: the stepped user is planned off");
				Assert::AreEqual(200 * mb, rig.sk.VramPendingFreeOf(rig.Host()), L"but it still holds 200 MiB: pending");
				Assert::AreEqual(200 * mb, registry.Take().heldBytes, L"the held bytes follow the module, not the plan");
				registry.Withdraw(&owner, VpSkeleton::kUserBasicChroma);   // the module frees its texture
				for (int64_t s = 5; s <= 70; ++s)
					rig.Frame(s);
				Assert::AreEqual(uint64_t(0), rig.sk.VramPendingFreeOf(rig.Host()), L"freed: nothing pending");
			}
			registry.ResetForTesting();
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

		TEST_METHOD(OffAndOriginalNeverConstructADispatcher)
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

		TEST_METHOD(WithoutARenderDeviceTheOnModeHasNoDispatcherAndSaysSo)
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
			Assert::IsTrue(HasId(fb, "skeleton_gpu_device"), L"no render device is its own reason id");
			Assert::IsFalse(HasId(fb, "skeleton_gpu_gap"), L"nothing runs ahead without a device");
			Assert::IsFalse(HasId(fb, "skeleton_gpu_live"), L"no dispatcher, no live wait");
			Assert::IsFalse(HasId(fb, "skeleton_gpu_shadow"), L"not shadow: no shadow entry");
			Assert::IsTrue(rig.sk.LaneOf(rig.Host())->TotalDeclined() > 0, L"the declining live fill is still installed without a dispatcher");
		}

		TEST_METHOD(ShadowModeSkipsTheDeviceProductsVisibly)
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
			Assert::IsTrue(HasId(fb, "skeleton_gpu_shadow"), L"shadow is reported under its own reason id");
			Assert::IsFalse(HasId(fb, "skeleton_gpu_device"), L"shadow is not reported as a missing render device");
			Assert::IsFalse(HasId(fb, "skeleton_gpu_gap"), L"shadow runs no GPU work ahead");
			Assert::IsFalse(HasId(fb, "skeleton_gpu_live"), L"shadow has no live GPU wait");
		}

		TEST_METHOD(AnInvalidDispatchKeyIsShownWhileTheLaneRuns)
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
		TEST_METHOD(TheDispatcherStatusTextsAreTheAgreedOnes)
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

	// B1 amend (doc 91 C tail; refs/93-td-readiness-audit.md section 3 + (a)1): the host's look-ahead copy (MotionAhead::CollectQueueAhead,
	// the render loop's call site) raises the CollectAhead phase with the SAME count the motion lead reads (queuedFutureFrames), so the
	// skeleton lane's lead is the frames queued ahead. A forwarding extension maps the walk's host (the fake Impl) to the rig's renderer.
	TEST_CLASS(SkeletonLaneLeadFeedTests)
	{
		struct FeedFrame { void SourceBufferAddRef() {} };
		struct FeedQueued { FeedFrame frame; std::shared_ptr<int> state = std::make_shared<int>(1); bool cadenceRepeat = false; unsigned long long sourceSequence = 0; };
		struct FeedProducer
		{
			int lead = -1;
			unsigned long long LastSubmitted() const { return 0; }
			void SetLead(int l) { lead = l; }
		};
		struct FeedImpl
		{
			std::atomic<size_t> queuedFutureFrames{ 0 };
			std::atomic<int> motionAheadDepth{ 0 };
			std::atomic<bool> motionAheadBackward{ false };
			std::shared_ptr<FeedProducer> motionProducer = std::make_shared<FeedProducer>();
		};
		struct Forward : RE::IRendererExtension
		{
			const void* from; const void* to; SkeletonModule* sk; int calls = 0;
			Forward(const void* f, const void* t, SkeletonModule* s) : from(f), to(t), sk(s) {}
			bool OnFrame(RE::FrameContext& c) override
			{
				if (c.phase != RE::FramePhase::CollectAhead || c.host != from) return false;
				++calls;
				RE::FrameContext mapped = c;
				mapped.host = to;
				return sk->OnFrame(mapped);
			}
		};

		// `passes` render-loop passes (40 ms apart on the rig's clock, so the 0.5 s lane-lead window ages out) of a queue holding the
		// current frame plus `queued` ahead, at motion depth `depth`; then one frame so the lane rebuilds with the fed queue.
		static SkeletonModule::QueueInUse Feed(Rig& rig, FeedImpl& impl, int depth, size_t queued, int64_t& seq, int passes = 30)
		{
			std::vector<FeedQueued> queue(queued + 1);
			for (size_t i = 0; i < queue.size(); ++i)
				queue[i].sourceSequence = static_cast<unsigned long long>(i + 1);
			impl.queuedFutureFrames.store(queued);
			impl.motionAheadDepth.store(depth);
			std::vector<FeedQueued> taken;
			for (int i = 0; i < passes; ++i)
			{
				rig.base += 40000;
				taken.clear();
				MotionAhead::CollectQueueAhead(&impl, queue, taken);
			}
			rig.Frame(seq++);
			return rig.sk.QueueInUseOf(rig.Host());
		}

		TEST_METHOD(TheLaneLeadIsTheSameQueuedCountTheMotionLeadReads)
		{
			struct RegistryGuard { RegistryGuard() { RE::ResetForTesting(); } ~RegistryGuard() { RE::ResetForTesting(); } } guard;   // the extension never outlives the rig
			{
				Rig rig(Lane("on"));
				rig.ResyncApply("on");
				rig.Life(RE::Lifecycle::GpuCreated);
				int64_t seq = 1;
				for (; seq <= 3; ++seq)
					rig.Frame(seq);
				FeedImpl impl;
				Forward* fwd = new Forward(&impl, rig.Host(), &rig.sk);
				RE::Register(std::unique_ptr<RE::IRendererExtension>(fwd));
				Assert::AreEqual(0, int(rig.sk.CurrentQueue().wired & SkeletonModule::QueueShape::kLaneLead), L"nothing fed before the walk runs");

				// Depth 4, 6 queued ahead: the motion lead is 6 and the lane lead is the same 6.
				SkeletonModule::QueueInUse u = Feed(rig, impl, 4, 6, seq);
				Assert::IsTrue(fwd->calls > 0, L"the walk raised CollectAhead");
				Assert::IsTrue((u.fed.wired & SkeletonModule::QueueShape::kLaneLead) != 0, L"the lane lead is wired");
				Assert::AreEqual(6, impl.motionProducer->lead);
				Assert::AreEqual(6, int(u.fed.laneLead));
				Assert::AreEqual(impl.motionProducer->lead, int(u.used.laneLead), L"lane lead == motion lead");
				Assert::AreEqual(6, int(rig.sk.LaneOf(rig.Host())->State()->graph.settings.laneLead));

				// Depth 0 (look-ahead off): the motion lead is 0, the lane lead still follows the queue (never forced to 0 by the depth).
				u = Feed(rig, impl, 0, 5, seq);
				Assert::AreEqual(0, impl.motionProducer->lead);
				Assert::AreEqual(5, int(u.used.laneLead));
				Assert::IsTrue(u.used.laneLead >= 1);

				// 20 queued ahead: the motion lead stops at kMaxLead (13, its pair ring); the lane is fed the real 20 and caps only to
				// its own ring-slot budget (shown by the skeleton when it binds).
				u = Feed(rig, impl, 4, 20, seq);
				Assert::AreEqual(MotionAhead::kMaxLead, impl.motionProducer->lead);
				Assert::AreEqual(20, int(u.fed.laneLead));
				Assert::AreEqual(int((std::min)(20u, SkeletonModule::LaneLeadCap(0))), int(u.used.laneLead));

				// No producer (motion off): the feed still runs.
				impl.motionProducer.reset();
				u = Feed(rig, impl, 4, 3, seq);
				Assert::AreEqual(3, int(u.used.laneLead), L"the lane lead does not depend on the motion producer");
				RE::ResetForTesting();
			}
		}
	};
}
