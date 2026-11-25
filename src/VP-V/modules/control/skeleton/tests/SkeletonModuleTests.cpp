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

#include <VP-V/core/ComputeOnce.h>
#include <VP-V/core/FallbackCatalog.h>
#include <VP-V/core/hub/ExtensionHub.h>
#include <VP-V/core/hub/HostSlots.h>
#include <VP-V/core/hub/HostState.h>
#include <VP-V/core/FrameAnalysisStore.h>
#include <VP-V/core/FramesInFlight.h>
#include <VP-V/core/PostPresentSpare.h>
#include <VP-V/core/VramUsers.h>
#include <VP-V/modules/control/skeleton/SkeletonAccess.h>
#include <VP-V/modules/control/skeleton/SkeletonModule.h>
#include <VP-V/modules/input/signal/ResyncExtension.h>
#include <VP-V/modules/input/source_analysis/SourceFrameTap.h>
#include <VP-V/modules/input/source_analysis/VpFrameAnalysisHost.h>
#include <VP-V/modules/motion/estimation/MotionValidity.h>
#include <VP-V/modules/motion/estimation/VectorEngineInterface.h>
#include <VP-V/modules/motion/estimation/MotionAheadWalk.h>
#include <VP-V/modules/motion/estimation/global_model/CameraProducts.h>
#include <VP-V/modules/input/source_analysis/lane/LaneCpuProducts.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>
#if defined(_DEBUG)
#include <crtdbg.h>
#endif

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

	// A skeleton module in the hub whose scene product misses with a scripted `why` (what SceneGroupOf reads through the hub's query).
	struct MissingSceneDemand : Vpv::Hub::ISkeletonModule, Vpv::Skeleton::IExternalDemand
	{
		const char* Name() const override { return "test skeleton (scene miss)"; }
		void* Query(const char* iid) override
		{
			return iid != nullptr && std::strcmp(iid, Vpv::Skeleton::kExternalDemandQueryId) == 0 ? static_cast<Vpv::Skeleton::IExternalDemand*>(this) : nullptr;
		}
		void SetExternalDemand(const void*, const void*, const std::vector<VpSkeleton::ConsumerDecl>&, const std::vector<VpSkeleton::ProductId>&) override {}
		void ClearExternalDemand(const void*, const void*) override {}
		Vpv::Skeleton::SceneAnswer SceneOf(const void*, uint64_t) const override
		{
			Vpv::Skeleton::SceneAnswer a;   // known = false
			a.why = why;
			return a;
		}
		const char* why = "";
	};

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
			// WAIVER-P10V3-03 (decision D2): default Full -> Compact.
			Assert::IsTrue(d.grainStorage == Vpv::Skeleton::GrainStorage::Compact, L"grain_split_storage defaults to compact");
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
			Assert::IsTrue(s.grainStorage == Vpv::Skeleton::GrainStorage::Compact);   // WAIVER-P10V3-03: invalid value keeps the (now compact) default
			Assert::AreEqual<size_t>(2, s.laneProblems.size());
			Assert::IsTrue(s.laneProblems[0] == "skeleton_gpu_gap 'sometimes' is not valid (off|after_render); kept off");
			Assert::IsTrue(s.laneProblems[1] == "grain_split_storage 'tiny' is not valid (full|compact); kept compact");   // WAIVER-P10V3-03 (D2)
		}

		TEST_METHOD(OriginalPathwayForcesBothOff)
		{
			const Vpv::Skeleton::Settings s = ParseText("[vpv]\npipeline_original_pathway=on\n[vpv.skeleton]\nanalysis_lane=on\ncamera_link=report\n");
			Assert::IsTrue(s.originalPathway);
			Assert::IsFalse(s.LaneWanted() || s.CameraWanted());
		}

		TEST_METHOD(LaneTapSourceParsesDefaultsToAutoAndTheOriginalPathwayForcesCpu)
		{
			// P10 D5 (refs/118): auto | cpu, default auto; invalid keeps auto and is listed; Original reads cpu and feeds no ring.
			const Vpv::Skeleton::Settings d = ParseText("[vpv.skeleton]\nanalysis_lane=on\n");
			Assert::IsTrue(d.tapSource == Vpv::Skeleton::TapSourceMode::Auto);
			Assert::IsTrue(d.TapRingWanted(), L"lane on + auto: the source ring is wanted");
			const Vpv::Skeleton::Settings c = ParseText("[vpv.skeleton]\nanalysis_lane=on\nlane_tap_source=CPU\n");
			Assert::IsTrue(c.tapSource == Vpv::Skeleton::TapSourceMode::Cpu);
			Assert::IsFalse(c.TapRingWanted());
			Assert::IsTrue(c.laneProblems.empty());
			const Vpv::Skeleton::Settings bad = ParseText("[vpv.skeleton]\nanalysis_lane=on\nlane_tap_source=gpu\n");
			Assert::IsTrue(bad.tapSource == Vpv::Skeleton::TapSourceMode::Auto);
			Assert::AreEqual<size_t>(1, bad.laneProblems.size());
			Assert::IsTrue(bad.laneProblems[0] == "lane_tap_source 'gpu' is not valid (auto|cpu); kept auto");
			const Vpv::Skeleton::Settings o = ParseText("[vpv]\npipeline_original_pathway=on\n[vpv.skeleton]\nanalysis_lane=on\nlane_tap_source=auto\n");
			Assert::IsTrue(o.EffectiveTapSource() == Vpv::Skeleton::TapSourceMode::Cpu);
			Assert::IsFalse(o.TapRingWanted());
			const Vpv::Skeleton::Settings off = ParseText("[vpv.skeleton]\nlane_tap_source=auto\n");
			Assert::IsFalse(off.TapRingWanted(), L"lane off: no ring");
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

		// B3-Z Z2 under rule 32 (refs/107 S5): the ONE R12 AfterPresent seam drives the renderer's spare slot through this module
		// (no per-feature host site). Kind -> trigger; one run per SOURCE frame (a repeat call of the frame and a cadence repeat count
		// AlreadyRan); a renderer without an active lane (Off / Original) has no slot and the seam writes nothing.
		TEST_METHOD(AfterPresentSeamDrivesTheSpareSlotOncePerSourceFrame)
		{
			Rig off("[general]\nx=1\n");
			off.Life(RE::Lifecycle::GpuCreated);
			off.Life(RE::Lifecycle::SettingsApplied);
			Assert::IsNull(off.sk.SpareOf(off.Host()), L"no lane, no slot");
			{
				RE::FrameContext c; c.host = off.Host(); c.seq = 1; c.phase = RE::FramePhase::AfterPresent;
				RE::PresentContext pc; pc.kind = RE::AfterPresentKind::NoExtras;
				off.sk.OnAfterPresent(c, pc);
				Assert::IsFalse(pc.outcome.extrasRan || pc.outcome.committed || pc.outcome.spentSeconds != 0.0, L"inert: nothing written");
			}

			Rig rig(Lane("on"));
			rig.ResyncApply("on");
			rig.Life(RE::Lifecycle::GpuCreated);
			Assert::IsTrue(rig.sk.Active(rig.Host()));
			VpSkeleton::SpareHost* spare = rig.sk.SpareOf(rig.Host());
			Assert::IsNotNull(spare);
			std::vector<uint64_t> ranKeys;
			VpSkeleton::FunctionSpareWork work(1, VpSkeleton::SpareClass::Core, 1);
			work.hasWork = []() { return true; };
			work.run = [&](uint32_t) -> double { ranKeys.push_back(spare->Key()); return 0.5; };
			spare->Slot().Register(&work);
			spare->SetBudget(8.0);
			const auto present = [&](int64_t seq, RE::AfterPresentKind kind, bool repeat) {
				RE::FrameContext c; c.host = rig.Host(); c.seq = seq; c.phase = RE::FramePhase::AfterPresent;
				RE::PresentContext pc; pc.kind = kind; pc.cadenceRepeat = repeat;
				rig.sk.OnAfterPresent(c, pc);
				return spare->LastReport();
			};
			VpSkeleton::SpareReport r = present(1, RE::AfterPresentKind::NoExtras, false);
			Assert::IsTrue(r.outcome == VpSkeleton::SpareOutcome::Ran);
			Assert::IsTrue(r.trigger == VpSkeleton::SpareTrigger::PostPresentNoExtras);
			r = present(1, RE::AfterPresentKind::Plain, false);
			Assert::IsTrue(r.outcome == VpSkeleton::SpareOutcome::AlreadyRan, L"a second call of the same source frame");
			Assert::IsTrue(r.trigger == VpSkeleton::SpareTrigger::BeforeDequeueWait, L"Plain = nothing presented");
			r = present(2, RE::AfterPresentKind::AfterLastExtra, true);
			Assert::IsTrue(r.outcome == VpSkeleton::SpareOutcome::AlreadyRan, L"a cadence repeat keeps its film frame's key");
			Assert::IsTrue(r.trigger == VpSkeleton::SpareTrigger::AfterLastExtra);
			r = present(3, RE::AfterPresentKind::AfterLastExtra, false);
			Assert::IsTrue(r.outcome == VpSkeleton::SpareOutcome::Ran);
			Assert::AreEqual<size_t>(2, ranKeys.size(), L"once per source frame");
			Assert::AreEqual<uint64_t>(1, ranKeys[0]);
			Assert::AreEqual<uint64_t>(3, ranKeys[1]);
			present(0, RE::AfterPresentKind::NoExtras, false);   // no sequence: ignored
			Assert::AreEqual<size_t>(2, ranKeys.size());
			spare->Slot().Unregister(&work);
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

		// P13-DECL: the reserve rows come from the planning size, so a status refresh with no bundle and no lane still prints them.
		TEST_METHOD(TheReserveLinesAreShownWithoutABundleOrALane)
		{
			Rig rig(Lane("on"));
			rig.ResyncApply("shadow");
			rig.Life(RE::Lifecycle::GpuCreated);
			Assert::IsNull(rig.sk.LaneOf(rig.Host()), L"no lane");
			const std::string text = rig.Command();
			Assert::IsTrue(text.find("Skeleton reserve, full-budget tier (reserved, not allocated") != std::string::npos);
			Assert::IsTrue(text.find("Skeleton reserve, reduced-budget tier (reserved, not allocated") != std::string::npos);
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

		// P15-DECL: a test frame-role producer (never selected by a graph without a consumer; it only has to be a registered object).
		struct QpTestProducer : VpSkeleton::ILaneProducer
		{
			VpSkeleton::ProductId id;
			explicit QpTestProducer(VpSkeleton::ProductId p) : id(p) {}
			VpSkeleton::ProductId Product() const override { return id; }
			bool Begin(const VpSkeleton::LaneJobContext&) override { return false; }
			VpSkeleton::ChunkResult RunChunk(const VpSkeleton::LaneJobContext&, uint32_t) override { return VpSkeleton::ChunkResult::Failed; }
		};

		TEST_METHOD(TheFrameRolePluginSlotIsInertWithoutAProducer)
		{
			Rig rig(Lane("on"));
			rig.ResyncApply("on");
			rig.Life(RE::Lifecycle::GpuCreated);
			for (int64_t s = 1; s <= 4; ++s)
				rig.Frame(s);
			VpSkeleton::AnalysisLane* lane = rig.sk.LaneOf(rig.Host());
			Assert::IsNotNull(lane);
			// no producer set: the lane holds exactly the producers it always had (P1 and P4 here; the GPU jobs need a device), byte-identical
			Assert::AreEqual<size_t>(2, lane->ProducerCount());
			Assert::IsTrue(rig.sk.WorkersRunning(rig.Host()));
		}

		TEST_METHOD(AFrameRoleProducerIsRegisteredOncePerLaneBeforeTheThreadsStart)
		{
			QpTestProducer p15(VpSkeleton::kQualityPattern), p15c(VpSkeleton::kQualityPatternCausal);
			Rig rig(Lane("on"));
			rig.sk.SetQualityPatternProducers(&p15, &p15c);
			rig.ResyncApply("on");
			rig.Life(RE::Lifecycle::GpuCreated);
			for (int64_t s = 1; s <= 4; ++s)
				rig.Frame(s);
			VpSkeleton::AnalysisLane* lane = rig.sk.LaneOf(rig.Host());
			Assert::IsNotNull(lane);
			Assert::AreEqual<size_t>(4, lane->ProducerCount(), L"p1, p4 and the two frame-role producers, each exactly once");
			Assert::IsTrue(lane->HasProducer(&p15) && lane->HasProducer(&p15c));
			Assert::IsTrue(rig.sk.WorkersRunning(rig.Host()), L"the threads started after the registration (RegisterProducer asserts otherwise)");
			// more frames and a reconfigure do not register them again
			for (int64_t s = 5; s <= 8; ++s)
				rig.Frame(s);
			Assert::AreEqual<size_t>(4, rig.sk.LaneOf(rig.Host())->ProducerCount());
			// a null second slot is allowed
			rig.sk.SetQualityPatternProducers(nullptr, nullptr);
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
			FeedQueueTerms(rig);   // GF-P2a RIG: the queue window is fed, so no "skeleton_queue_window: not fed" line is left over
			rig.Life(RE::Lifecycle::GpuCreated);
			for (int64_t s = 1; s <= 3; ++s)
			{
				rig.Frame(s);
				PresentFacts(rig, s);
			}
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
			{
				rig.Frame(s);
				PresentFacts(rig, s);   // GF-P2a RIG: the present-lag term is fed each frame; the three queue terms stay unfed until delivered below
			}
			Assert::IsTrue(Has(rig.Lines(RE::StatusKind::Fallbacks),
				"skeleton_queue: record history: queue not yet wired (render-ahead depth, lane lead, look-ahead walk); floors in use"));
			// The look-ahead walk arrives with the host's SettingsApplied call (value = the motion look-ahead depth): that term leaves the line.
			rig.Life(RE::Lifecycle::SettingsApplied, 0, 4.0);
			Assert::AreEqual(4, int(rig.sk.CurrentQueue().lookAheadWalk));
			Assert::AreEqual(0, int(rig.sk.CurrentQueue().renderAhead));
			Assert::AreEqual(1, int(rig.sk.CurrentQueue().laneLead));
			rig.Frame(4);
			PresentFacts(rig, 4);
			Assert::IsTrue(Has(rig.Lines(RE::StatusKind::Fallbacks),
				"skeleton_queue: record history: queue not yet wired (render-ahead depth, lane lead); floors in use"));
			SkeletonModule::QueueShape q;
			q.renderAhead = 1; q.laneLead = 2; q.lookAheadWalk = 3;
			rig.sk.SetQueueForTesting(q);
			rig.Frame(5);
			Assert::IsFalse(Has(rig.Lines(RE::StatusKind::Fallbacks), "skeleton_queue"), L"all three delivered: nothing to show");
			Assert::IsFalse(HasId(rig.Lines(RE::StatusKind::Fallbacks), "skeleton_queue"), L"the entry itself is gone (matched by id, not by substring)");
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
			// P13-DECL: the VRAM reserve is shown per budget tier as "reserved, not allocated" (shown, none taken).
			Assert::IsTrue(rig.Command().find("Skeleton reserve, full-budget tier (reserved, not allocated") != std::string::npos);
			Assert::IsTrue(rig.Command().find("Skeleton reserve, reduced-budget tier (reserved, not allocated") != std::string::npos);
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
			Assert::IsTrue(g.Has(VpSkeleton::kP4SourceStats) && g.Has(VpSkeleton::kP1HalfLuma),
				L"the inputs of P7 / P8 are in the graph (P8 -> P7 -> P4 -> P1)");
			// C10 15a K2 (F2) + CP-A1-D4-CAMLABEL-FIELDCOPY: P8 declares P7 + its fit field (p7f_camera_fit_field) only, so the field node is
			// in the camera graph (before P8) and neither P3 nor a motion pair is pulled into a camera-only graph.
			Assert::IsTrue(g.Has(VpSkeleton::kCameraFitField), L"P8's declared fit field is in the camera graph");
			Assert::IsTrue(g.IndexOf(VpSkeleton::kCameraFitField) < g.IndexOf(VpSkeleton::kP8CameraLabels), L"the fit field before P8");
			Assert::IsFalse(g.Has(VpSkeleton::kP3AnalysisField), L"P8 declares P7 + its fit field only (C10 15a K2, F2)");
			Assert::IsFalse(g.Has(VpSkeleton::kP2mMatcherPair) || g.Has(VpSkeleton::kP2VectorPair), L"no motion pair without a P3 reader");
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
			Assert::IsFalse(rig.sk.LaneOf(rig.Host())->State()->graph.Has(VpSkeleton::kCameraFitField), L"and without the fit field");
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
			// C10 15a K2: P8 no longer declares P3, so the camera demand alone holds no P2m / P3 slot to publish. Add a P3 reader (GrainMap:
			// P8 + P3) so the graph holds the field slots this test is about; the assertions below are unchanged.
			std::vector<VpSkeleton::ConsumerDecl> feedConsumers = CameraConsumers();
			feedConsumers.push_back(CameraLink::DeclarationOf(CameraLink::CameraConsumer::GrainMap));
			rig.sk.SetExternalDemand(rig.Host(), &owner, feedConsumers, { VpSkeleton::kP7CameraFit, VpSkeleton::kP8CameraLabels });
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
			// U7-accuracy (WAIVER-U7ACC-SKELETON-SCENE): the frame's own verdict is cutHere; `cut` is the delivered signal.
			Assert::AreEqual(f.cut, rec.scene.cutHere, L"the stored verdict is the classification");
			Assert::AreEqual(f.confirmed, rec.scene.confirmed);
			Assert::IsFalse(rec.scene.confirmed, L"frame 5 was not sealed at the seal of 4: provisional");
			Assert::IsFalse(rec.scene.cut, L"a provisional verdict delivers no cut (and noise frames never cut)");
			Assert::AreEqual(f.fade, rec.scene.fade);
			// The reader: from the store, nothing recomputed (no second write, no live fill).
			const Vpv::Skeleton::SceneAnswer a = rig.sk.SceneOf(rig.Host(), 4);
			Assert::IsTrue(a.known);
			Assert::IsTrue(a.from == Vpv::Skeleton::SceneAnswer::From::Store);
			Assert::AreEqual(f.cut, a.cutHere);
			Assert::AreEqual(rec.scene.cut, a.cut);
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
			Assert::AreEqual(f.cut, live.cutHere);   // WAIVER-U7ACC-SKELETON-SCENE: the frame's verdict; live.cut is the delivered signal
			Assert::AreEqual(f.confirmed && f.cut, live.cut && !live.cutLate);
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

		// U7-accuracy (WAIVER-U7ACC-SKELETON-SCENE): the v2 sequence law needs the frames around a cut, and the delivered cut comes one frame
		// late when the frame's successor was not sealed at its seal (FrameAnalysis::Scene::cut). Two still shots: frames 1 .. 11 a left-to-right
		// ramp, frames 12 .. 63 a top-to-bottom ramp the other way (every pair inside a shot is identical, the pair across 11 -> 12 differs in
		// every cell and in the histogram). Call before any frame is delivered.
		static constexpr uint64_t kCutFrame = 12;
		static void ShapeTwoStillShots(Rig& rig)
		{
			for (size_t s = 0; s < rig.count; ++s)
				for (uint32_t y = 0; y < kH; ++y)
					for (uint32_t x = 0; x < kW; ++x)
					{
						const bool second = s >= kCutFrame;
						const uint32_t v = second ? 900u - (y * 700u) / kH + ((x * 3u + y) & 7u) : 150u + (x * 700u) / kW + ((x + y) & 7u);
						rig.store[s * kW * kH + size_t(y) * kW + x] = uint16_t(v);
					}
		}

		// Frames 1 .. kCutFrame - 1 rendered, then the seal of kCutFrame (its successor delivered, not sealed): the cut frame's verdict is
		// provisional here. `opened` (may be null) gets the records of kCutFrame and kCutFrame + 1 opened before their seals.
		static void RunToTheSealOfTheCut(Rig& rig, FrameAnalysis::Store* opened)
		{
			rig.ResyncApply("on");
			rig.Life(RE::Lifecycle::GpuCreated);
			for (int64_t s = 1; s < int64_t(kCutFrame); ++s)
				rig.Frame(s);
			if (opened)
			{
				opened->Begin(SceneTestId(kCutFrame), FrameAnalysis::kScene);
				opened->Begin(SceneTestId(kCutFrame + 1), FrameAnalysis::kScene);
			}
			rig.Ingress(int64_t(kCutFrame));
			rig.Deliver(kCutFrame + 1);
			rig.Begin(int64_t(kCutFrame));
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
			// U7-accuracy (WAIVER-U7ACC-SKELETON-SCENE). 1. Store on, the records open at their seals. At the seal of the cut frame its
			// successor is not sealed: the verdict is provisional and delivers nothing. At the next seal the cut is CONFIRMED: the cut
			// frame's group is rewritten (cutHere, confirmed) and the cut is delivered once, one frame late (cut + cutLate on the next frame).
			Rig stored(Lane("on"));
			ShapeTwoStillShots(stored);
			FrameAnalysis::Store store(FrameAnalysis::RingDepth(4, 4));
			Tap::SetStore(stored.Host(), &store);
			SceneStoreGuard g1(stored.Host());
			RunToTheSealOfTheCut(stored, &store);
			FrameAnalysis::Scene atSeal;
			Assert::IsTrue(store.PeekScene(kCutFrame, atSeal));
			Assert::IsFalse(atSeal.confirmed, L"the successor was not sealed: provisional");
			Assert::IsFalse(atSeal.cut, L"a provisional verdict never delivers a cut");
			stored.After(int64_t(kCutFrame));
			stored.Frame(int64_t(kCutFrame + 1));
			FrameAnalysis::Scene cutGroup, nextGroup;
			Assert::IsTrue(store.PeekScene(kCutFrame, cutGroup));
			Assert::IsTrue(store.PeekScene(kCutFrame + 1, nextGroup));
			Assert::IsTrue(cutGroup.confirmed, L"rewritten when frame 13 confirmed it");
			Assert::IsTrue(cutGroup.cutHere, L"two still shots: the frame starting the second is a cut");
			Assert::IsFalse(cutGroup.cut, L"the delivered cut is not rewritten (exactly once)");
			Assert::IsTrue(nextGroup.cut && nextGroup.cutLate, L"delivered one frame late");
			Assert::IsFalse(nextGroup.cutHere, L"inside the second shot");
			Assert::IsTrue(stored.sk.SceneCountersOf(stored.Host()).republished >= 1u);
			const Vpv::Skeleton::SceneAnswer fromStore = stored.sk.SceneOf(stored.Host(), kCutFrame);
			Assert::IsTrue(fromStore.known);
			Assert::IsTrue(fromStore.from == Vpv::Skeleton::SceneAnswer::From::Store);
			Assert::IsTrue(fromStore.cutHere && fromStore.confirmed);
			Assert::IsTrue(stored.sk.SceneOf(stored.Host(), kCutFrame + 1).cut);
			// Q5: the vector-validity seam answers the confirmed cut (the store is the only one registered here).
			const MotionValidity::State::ConfirmedCutFn seam = MotionValidity::State::ConfirmedCutReader().load();
			Assert::IsTrue(seam != nullptr, L"the skeleton sets the scene seam of the P5a facade");
			Assert::IsTrue(Tap::SingleStore() == &store);
			Assert::IsTrue(seam(kCutFrame), L"a confirmed cut refuses the pair into it");
			Assert::IsFalse(seam(kCutFrame + 1));
			Assert::IsFalse(seam(kCutFrame - 1), L"no group: nothing refused");
			stored.After(int64_t(kCutFrame + 1));

			// 1b. Store on, the records open AFTER the seals (WAIVER-U7ACC-SKELETON-SCENE: the v1 case 2, restored with the v2 expectations):
			// the seals write nothing, the reader classifies live, gets the same confirmed cut, delivered at the cut frame itself (its
			// successor was sealed when asked), writes it into the record and is served from the store after that; the next frame does not
			// deliver it again.
			Rig late(Lane("on"));
			ShapeTwoStillShots(late);
			FrameAnalysis::Store lateStore(FrameAnalysis::RingDepth(4, 4));
			Tap::SetStore(late.Host(), &lateStore);
			SceneStoreGuard g2(late.Host());
			RunToTheSealOfTheCut(late, nullptr);
			FrameAnalysis::Scene none;
			Assert::IsFalse(lateStore.PeekScene(kCutFrame, none), L"no record at the seal: nothing written");
			late.After(int64_t(kCutFrame));
			late.Frame(int64_t(kCutFrame + 1));
			lateStore.Begin(SceneTestId(kCutFrame), FrameAnalysis::kScene);
			lateStore.Begin(SceneTestId(kCutFrame + 1), FrameAnalysis::kScene);
			const Vpv::Skeleton::SceneAnswer fromLive = late.sk.SceneOf(late.Host(), kCutFrame);
			Assert::IsTrue(fromLive.known);
			Assert::IsTrue(fromLive.from == Vpv::Skeleton::SceneAnswer::From::Live);
			Assert::IsTrue(fromLive.cutHere && fromLive.confirmed, L"the same confirmed verdict as the seal path");
			Assert::IsTrue(fromLive.cut && !fromLive.cutLate, L"delivered at the cut frame: its successor was sealed when asked");
			Assert::AreEqual(fromStore.fade, fromLive.fade);
			FrameAnalysis::Scene written;
			Assert::IsTrue(lateStore.PeekScene(kCutFrame, written), L"the live fill wrote the record that opened late");
			Assert::IsTrue(written.cut && written.cutHere && written.confirmed);
			Assert::IsTrue(late.sk.SceneOf(late.Host(), kCutFrame).from == Vpv::Skeleton::SceneAnswer::From::Store);
			const Vpv::Skeleton::SceneAnswer lateNext = late.sk.SceneOf(late.Host(), kCutFrame + 1);
			Assert::IsTrue(lateNext.known);
			Assert::IsFalse(lateNext.cut, L"delivered once");
			Assert::AreEqual(uint64_t(0), late.sk.SceneCountersOf(late.Host()).late, L"nothing went out late");
			late.After(int64_t(kCutFrame + 1));
			Tap::SetStore(late.Host(), nullptr);

			// 2. Store off, the reader asks after the successor was sealed: the verdict is confirmed when asked, so the cut is delivered at
			// the cut frame itself and not again on the next frame (exactly once).
			Rig off(Lane("on"));
			ShapeTwoStillShots(off);
			RunToTheSealOfTheCut(off, nullptr);
			off.After(int64_t(kCutFrame));
			off.Frame(int64_t(kCutFrame + 1));
			const Vpv::Skeleton::SceneAnswer storeOff = off.sk.SceneOf(off.Host(), kCutFrame);
			Assert::IsTrue(storeOff.known);
			Assert::IsTrue(storeOff.from == Vpv::Skeleton::SceneAnswer::From::Live);
			Assert::IsTrue(storeOff.cutHere && storeOff.confirmed);
			Assert::IsTrue(storeOff.cut && !storeOff.cutLate);
			const Vpv::Skeleton::SceneAnswer offNext = off.sk.SceneOf(off.Host(), kCutFrame + 1);
			Assert::IsTrue(offNext.known);
			Assert::IsFalse(offNext.cut, L"delivered once");
			Assert::AreEqual(fromStore.fade, storeOff.fade);

			// 3. A store that is registered but DISABLED (what frame_analysis_store=false leaves): the same as the store off.
			Rig disabled(Lane("on"));
			ShapeTwoStillShots(disabled);
			FrameAnalysis::Store offStore(FrameAnalysis::RingDepth(4, 4));
			offStore.SetEnabled(false);
			Tap::SetStore(disabled.Host(), &offStore);
			SceneStoreGuard g3(disabled.Host());
			RunToTheSealOfTheCut(disabled, nullptr);
			disabled.After(int64_t(kCutFrame));
			disabled.Frame(int64_t(kCutFrame + 1));
			const Vpv::Skeleton::SceneAnswer fromDisabled = disabled.sk.SceneOf(disabled.Host(), kCutFrame);
			Assert::IsTrue(fromDisabled.known);
			Assert::IsTrue(fromDisabled.cutHere && fromDisabled.confirmed && fromDisabled.cut);
			Assert::AreEqual(storeOff.fade, fromDisabled.fade);

			// No other frame of the two shots is a cut (still shots: every other pair is identical).
			for (uint64_t s = 2; s < kCutFrame; ++s)
				Assert::IsFalse(off.sk.SceneOf(off.Host(), s).cutHere, L"no cut inside a still shot");
		}

		// WAIVER-U7ACC-DELIVERY-GAP (audit U7ACC MUST 3, a new test): the frame after the cut has no scene group (its record never opened:
		// unsealed / dropped from the store). The cut is confirmed at the next seal that has a group and is delivered there, exactly once,
		// counted as more than one frame late. Before the fix only frame seq - 1 was looked at and this cut was lost.
		TEST_METHOD(ACutWhoseNextFrameHasNoSceneGroupIsDeliveredOnceWithTheNextGroup)
		{
			Rig rig(Lane("on"));
			ShapeTwoStillShots(rig);
			FrameAnalysis::Store store(FrameAnalysis::RingDepth(4, 4));
			Tap::SetStore(rig.Host(), &store);
			SceneStoreGuard guard(rig.Host());
			rig.ResyncApply("on");
			rig.Life(RE::Lifecycle::GpuCreated);
			for (int64_t s = 1; s < int64_t(kCutFrame); ++s)
				rig.Frame(s);
			store.Begin(SceneTestId(kCutFrame), FrameAnalysis::kScene);
			rig.Ingress(int64_t(kCutFrame));
			rig.Deliver(kCutFrame + 1);
			rig.Begin(int64_t(kCutFrame));
			FrameAnalysis::Scene atSeal;
			Assert::IsTrue(store.PeekScene(kCutFrame, atSeal));
			Assert::IsFalse(atSeal.confirmed || atSeal.cut, L"provisional at its seal: nothing delivered");
			rig.After(int64_t(kCutFrame));
			rig.Frame(int64_t(kCutFrame + 1));   // no record: no group for the frame after the cut
			FrameAnalysis::Scene none;
			Assert::IsFalse(store.PeekScene(kCutFrame + 1, none));
			store.Begin(SceneTestId(kCutFrame + 2), FrameAnalysis::kScene);
			rig.Frame(int64_t(kCutFrame + 2));
			FrameAnalysis::Scene cutGroup, carrier;
			Assert::IsTrue(store.PeekScene(kCutFrame, cutGroup));
			Assert::IsTrue(cutGroup.confirmed && cutGroup.cutHere, L"the cut frame's group was confirmed later");
			Assert::IsFalse(cutGroup.cut, L"the delivered cut is not rewritten");
			Assert::IsTrue(store.PeekScene(kCutFrame + 2, carrier));
			Assert::IsTrue(carrier.cut && carrier.cutLate, L"delivered with the next frame that has a group");
			Assert::IsFalse(carrier.cutHere);
			const Vpv::Skeleton::SceneCounters c = rig.sk.SceneCountersOf(rig.Host());
			Assert::AreEqual(uint64_t(1), c.late);
			Assert::AreEqual(uint64_t(1), c.lateGap, L"counted: more than one frame late");
			// A reader of the frame in between gets no second delivery; the carrier keeps it on a re-ask.
			const Vpv::Skeleton::SceneAnswer between = rig.sk.SceneOf(rig.Host(), kCutFrame + 1);
			Assert::IsTrue(between.known);
			Assert::IsFalse(between.cut, L"exactly once");
			Assert::IsTrue(rig.sk.SceneOf(rig.Host(), kCutFrame + 2).cut);
			for (int64_t s = int64_t(kCutFrame + 3); s <= int64_t(kCutFrame + 6); ++s)
			{
				store.Begin(SceneTestId(uint64_t(s)), FrameAnalysis::kScene);
				rig.Frame(s);
				FrameAnalysis::Scene later;
				Assert::IsTrue(store.PeekScene(uint64_t(s), later));
				Assert::IsFalse(later.cut, L"no further delivery");
			}
			Assert::AreEqual(uint64_t(1), rig.sk.SceneCountersOf(rig.Host()).late, L"one cut, one delivery");
		}

		// Audit U7ACC MUST 4 (WAIVER-U7ACC-VECTOR-VETO, a new test): the vector cut veto's reach is shown, never silent. One enabled store:
		// the veto is on ("Skeleton scene:" line); a registered but disabled store: skeleton_scene_veto is engaged with the reason.
		TEST_METHOD(TheVectorCutVetoStateIsShown)
		{
			{
				Rig rig(Lane("on"));
				FrameAnalysis::Store store(FrameAnalysis::RingDepth(4, 4));
				Tap::SetStore(rig.Host(), &store);
				SceneStoreGuard guard(rig.Host());
				rig.ResyncApply("on");
				rig.Life(RE::Lifecycle::GpuCreated);
				for (int64_t s = 1; s <= 4; ++s)
					rig.Frame(s);
				Assert::IsTrue(Tap::SingleStore() == &store);
				Assert::IsFalse(HasId(rig.Lines(RE::StatusKind::Fallbacks), "skeleton_scene_veto"), L"one enabled store: the veto works");
				RE::Context c; c.host = rig.Host();
				std::string reply;
				Assert::IsTrue(rig.sk.OnCommand(c, "skeleton", "status", reply));
				Assert::IsTrue(reply.find("Skeleton scene: ") != std::string::npos, L"the scene line is on the status");
				Assert::IsTrue(reply.find("vector cut veto on") != std::string::npos);
			}
			{
				Rig rig(Lane("on"));
				FrameAnalysis::Store store(FrameAnalysis::RingDepth(4, 4));
				store.SetEnabled(false);
				Tap::SetStore(rig.Host(), &store);
				SceneStoreGuard guard(rig.Host());
				rig.ResyncApply("on");
				rig.Life(RE::Lifecycle::GpuCreated);
				for (int64_t s = 1; s <= 4; ++s)
					rig.Frame(s);
				Assert::IsTrue(HasId(rig.Lines(RE::StatusKind::Fallbacks), "skeleton_scene_veto"), L"store off: the veto is off, visibly");
				RE::Context c; c.host = rig.Host();
				std::string reply;
				Assert::IsTrue(rig.sk.OnCommand(c, "skeleton", "status", reply));
				Assert::IsTrue(reply.find("vector cut veto off (frame-analysis store off") != std::string::npos);
			}
		}

		// Frame n - 1 FLAT and dark, frame n bright texture (a cut at n, as ShapeACutAtFrame4). Call before any frame is delivered.
		static void ShapeACutAtFrame(Rig& rig, uint64_t n)
		{
			uint32_t x = 4242u;
			for (uint32_t i = 0; i < kW * kH; ++i)
			{
				rig.store[size_t((n - 1) % rig.count) * kW * kH + i] = 100;
				x = x * 1664525u + 1013904223u;
				rig.store[size_t(n % rig.count) * kW * kH + i] = uint16_t(600u + ((x >> 12) % 400u));
			}
		}

		static std::wstring WhyOf(const Vpv::Skeleton::SceneAnswer& a)
		{
			const std::string why = a.why != nullptr ? a.why : "";
			return L"why: " + std::wstring(why.begin(), why.end());
		}

		// Sealed 1..3; frames 5 and 6 arrived (queued) but are NOT sealed: frame 6 is two frames ahead of the next seal (4).
		static void QueueTwoAheadOfTheSeal(Rig& rig)
		{
			rig.ResyncApply("on");
			rig.Life(RE::Lifecycle::GpuCreated);
			for (int64_t s = 1; s <= 3; ++s)
				rig.Frame(s);
			rig.Ingress(4);
			rig.Deliver(5);
			rig.Ingress(5);
			rig.Deliver(6);
		}

		static void SealThrough6(Rig& rig)
		{
			rig.Begin(4);
			rig.After(4);
			rig.Begin(5);
			rig.After(5);
			rig.Ingress(6);
			rig.Deliver(7);
		}

		// GF-P2a RIG (UNION RED 4; compiles in the union only: ConsumerDecl::aheadDepth comes from GF-P1). The QKP-B queue window is derived
		// from the fed terms at every Begin. These rigs need the window to hold what the test asks for:
		//  - the presented frame, each frame (AfterPresent: the present-lag term),
		//  - the three queue terms through the publish API (render-ahead, lane lead, look-ahead walk), the lane lead left at the default 1
		//    so the lane still publishes nothing ahead by itself (an ahead record is made only by a reader's ask),
		//  - an ACTIVE ahead consumer: an external demand whose ConsumerDecl::aheadDepth reaches past the frames asked ahead.
		// Until a term is fed the status line "skeleton_queue_window: not fed" shows it (tests that look at the status feed all of them).
		static void PresentFacts(Rig& rig, int64_t seq)
		{
			RE::FrameContext c;
			c.host = rig.Host();
			c.seq = seq;
			c.phase = RE::FramePhase::AfterPresent;
			RE::PresentContext pc;
			pc.kind = RE::AfterPresentKind::NoExtras;
			rig.sk.OnAfterPresent(c, pc);
		}

		static void FeedQueueTerms(Rig& rig)
		{
			rig.sk.PublishRenderAhead(1.0);
			rig.sk.PublishLaneLead(1.0);
			rig.sk.PublishLookAheadWalk(8.0);
		}

		static void DeclareAheadConsumer(Rig& rig, uint8_t aheadDepth)
		{
			VpSkeleton::ConsumerDecl d;
			d.id = 0x0610;
			d.name = "ahead scene walk (test)";
			VpSkeleton::AddConsume(d, VpSkeleton::kP4SourceStats, 0);
			VpSkeleton::AddConsume(d, VpSkeleton::kP4SourceStats, 1);
			d.aheadDepth = aheadDepth;
			rig.sk.SetExternalDemand(rig.Host(), &rig, std::vector<VpSkeleton::ConsumerDecl>(1, d), std::vector<VpSkeleton::ProductId>());
		}

		// QueueTwoAheadOfTheSeal with the queue window fed (see above): the same frames, the same arrivals.
		static void QueueTwoAheadOfTheSealFed(Rig& rig)
		{
			rig.ResyncApply("on");
			FeedQueueTerms(rig);
			DeclareAheadConsumer(rig, 6);
			rig.Life(RE::Lifecycle::GpuCreated);
			for (int64_t s = 1; s <= 3; ++s)
			{
				rig.Frame(s);
				PresentFacts(rig, s);
			}
			rig.Ingress(4);
			rig.Deliver(5);
			rig.Ingress(5);
			rig.Deliver(6);
		}

		// FIX-LED-CUT-U7 / C-1 (get-or-compute-and-publish, OD 11-12 17:26 / 11-13 13:34): the scene of a frame AHEAD of the seal is
		// answered NOW (its two P4 records acquired through the renderer's memo), written into the frame's record, and the later seal
		// finds it there (computed once: alreadyPresent + 1, published + 0) with the verdict the seal-time classification gives.
		TEST_METHOD(AnAheadFramesSceneIsGotNowAndTheSealFindsItComputedOnce)
		{
			Rig rig(Lane("on"));
			ShapeACutAtFrame(rig, 6);
			FrameAnalysis::Store store(FrameAnalysis::RingDepth(4, 4));
			Tap::SetStore(rig.Host(), &store);
			SceneStoreGuard guard(rig.Host());
			QueueTwoAheadOfTheSealFed(rig);   // GF-P2a RIG: window fed
			store.Begin(SceneTestId(6), FrameAnalysis::kScene);   // the record the source tap opens at ARRIVAL
			const Vpv::Skeleton::SceneCounters before = rig.sk.SceneCountersOf(rig.Host());
			const Vpv::Skeleton::SceneAnswer ahead = rig.sk.SceneOf(rig.Host(), 6);
			Assert::IsTrue(ahead.known, (L"an ahead frame is answered, not 'no record yet'; " + WhyOf(ahead)).c_str());
			Assert::IsTrue(ahead.from == Vpv::Skeleton::SceneAnswer::From::Live);
			Assert::IsTrue(ahead.cut, L"flat then bright texture is a cut, judged two frames ahead of the seal");
			FrameAnalysis::Scene written;
			Assert::IsTrue(store.PeekScene(6, written), L"published into the ahead frame's record");
			Assert::AreEqual(ahead.cut, written.cut);
			Assert::IsTrue(written.origin == FrameAnalysis::SceneOrigin::Vp);
			// U7 v2: frame 7 has not arrived, so frame 6 is judged without its successor: its group is PROVISIONAL (the cut it answers is
			// frame 5's confirmed cut, carried late by its natural successor).
			Assert::IsFalse(written.confirmed, L"judged ahead of its successor: a provisional group");
			Assert::IsTrue(written.cutLate, L"the cut answered ahead is frame 5's, delivered late by frame 6");
			const Vpv::Skeleton::SceneCounters got = rig.sk.SceneCountersOf(rig.Host());
			Assert::AreEqual(before.liveFills + 1, got.liveFills, L"classified once, now");
			Assert::AreEqual(before.published, got.published, L"not a seal write");
			// A second reader before the seal: from the store, nothing recomputed.
			const Vpv::Skeleton::SceneAnswer again = rig.sk.SceneOf(rig.Host(), 6);
			Assert::IsTrue(again.known && again.from == Vpv::Skeleton::SceneAnswer::From::Store);
			Assert::AreEqual(got.liveFills, rig.sk.SceneCountersOf(rig.Host()).liveFills);

			SealThrough6(rig);
			const Vpv::Skeleton::SceneCounters atSealBefore = rig.sk.SceneCountersOf(rig.Host());
			rig.Begin(6);
			const Vpv::Skeleton::SceneCounters atSeal = rig.sk.SceneCountersOf(rig.Host());
			// U7 v2 (the seal, SkeletonModule.cpp "a provisional group of this module's own ... is judged again"): the seal finds the ahead
			// group PROVISIONAL, so it is not "already present": the seal judges it once more and that is its one write; a confirmed ahead
			// group would have been found (alreadyPresent), never rewritten.
			Assert::AreEqual(atSealBefore.alreadyPresent, atSeal.alreadyPresent, L"a provisional ahead group is not taken as the seal's verdict");
			Assert::AreEqual(atSealBefore.published + 1, atSeal.published, L"the seal's one write: the provisional group judged again");
			FrameAnalysis::Scene sealedGroup;
			Assert::IsTrue(store.PeekScene(6, sealedGroup) && sealedGroup.origin == FrameAnalysis::SceneOrigin::Vp, L"the frame's group, written by the seal");
			Assert::AreEqual(written.cut, sealedGroup.cut, L"the delivered cut is not delivered twice nor lost (frame 5's, carried by 6)");
			Assert::AreEqual(written.cutLate, sealedGroup.cutLate);
			Vpv::Skeleton::SceneFacts f;
			Assert::IsTrue(rig.sk.SceneFactsOf(rig.Host(), 6, f));
			Assert::AreEqual(f.cut, ahead.cutHere, L"the ahead verdict is the seal-time classification of the same pair");
			Assert::AreEqual(f.fade, ahead.fade);
			rig.After(6);
		}

		// The same with the store off: the ahead frame is answered (live), nothing is published; the answer after the seal is the
		// same. A caller OFF the render thread gets the lane's published copy or a NAMED miss (never a silent one).
		TEST_METHOD(AnAheadFramesSceneIsAnsweredWithTheStoreOffAndOffTheRenderThreadNeverSilently)
		{
			Rig rig(Lane("on"));
			ShapeACutAtFrame(rig, 6);
			QueueTwoAheadOfTheSealFed(rig);   // GF-P2a RIG: window fed
			const Vpv::Skeleton::SceneAnswer ahead = rig.sk.SceneOf(rig.Host(), 6);
			Assert::IsTrue(ahead.known, (L"answered with the store off; " + WhyOf(ahead)).c_str());
			Assert::IsTrue(ahead.from == Vpv::Skeleton::SceneAnswer::From::Live);
			Assert::IsTrue(ahead.cut);
			Assert::AreEqual(uint64_t(0), rig.sk.SceneCountersOf(rig.Host()).published, L"no store: nothing written");

			Vpv::Skeleton::SceneAnswer offThread;
			std::thread other([&] { offThread = rig.sk.SceneOf(rig.Host(), 5); });
			other.join();
			Assert::IsTrue(offThread.known || (offThread.why != nullptr && offThread.why[0] != 0),
				L"off the render thread: the published copy, or a miss that names its cause");
			// GF-P2a MUST-9 / S14.2b: a look-ahead miss off the render thread is a PEEK of the lane's published bytes, never the memo's
			// Acquire: it raises no skeleton-acquire decision (no false "p4_source_stats" Fallbacks entry).
			VpSkeleton::SkeletonMemo* memoOfHost = rig.sk.MemoOf(rig.Host());
			Assert::IsNotNull(memoOfHost);
			Assert::IsTrue(memoOfHost->Decisions().empty(), L"the off-thread SceneOf raised no skeleton-acquire decision");

			SealThrough6(rig);
			rig.Begin(6);
			const Vpv::Skeleton::SceneAnswer sealed = rig.sk.SceneOf(rig.Host(), 6);
			Assert::IsTrue(sealed.known);
			Assert::AreEqual(ahead.cut, sealed.cut, L"the same verdict ahead and after the seal");
			Assert::AreEqual(ahead.fade, sealed.fade);
			Assert::AreEqual(uint64_t(0), rig.sk.SceneCountersOf(rig.Host()).published);
			rig.After(6);
		}

		// A static picture, then a 50-frame slow fade (every frame of the rig's store). Call before any frame is delivered.
		// (Moved unchanged out of ASlowFadeThroughTheSceneProductEqualsTheStreamFold, which now calls it; GF-P2a T4R.)
		static void ShapeASlowFade(Rig& rig)
		{
			for (int s = 0; s < int(rig.count); ++s)
			{
				const double g = s <= 4 ? 1.0 : (std::max)(0.25, 1.0 - 0.75 * (s - 4) / 50.0);   // static, then a 50-frame fade
				for (uint32_t y = 0; y < kH; ++y)
					for (uint32_t x = 0; x < kW; ++x)
					{
						const uint32_t h = (x * 73856093u) ^ (y * 19349663u) ^ (uint32_t(s) * 83492791u);
						const double v = 64.0 + g * (double(128 + 12 * x) - 64.0) + double(int(h % 17u) - 8);
						rig.store[size_t(s) * kW * kH + y * kW + x] = uint16_t((std::min)(1023.0, (std::max)(0.0, std::floor(v + 0.5))));
					}
			}
		}

		// GF-P2a: the queue window the ahead asks need. The window's ahead depth is lane lead + look-ahead walk; the computed ahead depth
		// needs an ACTIVE ahead consumer (an owner fill declaring aheadDepth). The lane lead stays 1 (the default the older tests ran
		// with) so the lane publishes nothing ahead by itself: only a reader's ask makes an ahead record.
		static void FeedAheadWindow(Rig& rig, uint16_t walk)
		{
			SkeletonModule::QueueShape q;
			q.renderAhead = 1;
			q.laneLead = 1;
			q.lookAheadWalk = walk;
			rig.sk.SetQueueForTesting(q);
			VpSkeleton::OwnerFill fill;
			fill.aheadDepth = VpSkeleton::kAheadCeiling;   // an ahead consumer is active (the ceiling: the most the window computes ahead)
			fill.fill = [](const VpSkeleton::OwnerFillRequest&, VpSkeleton::SkeletonMemo&, VpSkeleton::OwnerFillOutput&) { return false; };
			rig.sk.RegisterOwnerFill(VpSkeleton::kP10GrainSplit, VpSkeleton::AcquireForm::Cpu, "repair/grain_split", fill);
		}

		// The look-ahead walk makes the P4 record of an ahead frame before it asks the scene (render thread; the memo's get-or-compute).
		static void WalkP4(Rig& rig, uint64_t seq)
		{
			VpSkeleton::SkeletonMemo* memo = rig.sk.MemoOf(rig.Host());
			Assert::IsNotNull(memo);
			memo->Acquire(VpSkeleton::kP4SourceStats, seq, VpSkeleton::AcquireForm::Cpu);   // the record stays in the lane's slot; the result is not needed
		}

		// GF-P2a MUST-7 (theft): a cut at N is provisional at N's seal. An ahead scene ask of N + 3 must not claim N's late cut; frame N + 1's
		// own seal delivers it (cut + cutLate on N + 1's group), exactly once.
		TEST_METHOD(AnAheadFrameNeverClaimsAnEarlierFramesLateCut)
		{
			Rig rig(Lane("on"));
			ShapeTwoStillShots(rig);
			FrameAnalysis::Store store(FrameAnalysis::RingDepth(4, 4));
			Tap::SetStore(rig.Host(), &store);
			SceneStoreGuard guard(rig.Host());
			FeedAheadWindow(rig, 8);
			RunToTheSealOfTheCut(rig, &store);   // the cut frame sealed, its group out provisional; frame kCutFrame + 1 delivered, not sealed
			rig.After(int64_t(kCutFrame));
			FrameAnalysis::Scene atSeal;
			Assert::IsTrue(store.PeekScene(kCutFrame, atSeal));
			Assert::IsFalse(atSeal.confirmed || atSeal.cut, L"provisional at its seal");
			const uint64_t n1 = kCutFrame + 1, n2 = kCutFrame + 2, n3 = kCutFrame + 3;
			rig.Ingress(int64_t(n1));
			rig.Deliver(n2);
			rig.Ingress(int64_t(n2));
			rig.Deliver(n3);
			store.Begin(SceneTestId(n3), FrameAnalysis::kScene);
			WalkP4(rig, n1);   // the walk reaches the frames in order: their P4 records exist
			WalkP4(rig, n2);
			WalkP4(rig, n3);
			const uint64_t lateBefore = rig.sk.SceneCountersOf(rig.Host()).late;
			const Vpv::Skeleton::SceneAnswer ahead = rig.sk.SceneOf(rig.Host(), n3);
			Assert::IsTrue(ahead.known, (L"the ahead frame is answered; " + WhyOf(ahead)).c_str());
			Assert::IsFalse(ahead.cutLate, L"an ahead frame never carries an earlier frame's late cut");
			FrameAnalysis::Scene aheadGroup;
			Assert::IsTrue(store.PeekScene(n3, aheadGroup), L"the ahead frame's own group was published");
			Assert::IsFalse(aheadGroup.cutLate);
			Assert::AreEqual(lateBefore, rig.sk.SceneCountersOf(rig.Host()).late, L"nothing was delivered by the ahead fill");
			rig.Begin(int64_t(n1));   // the render-order seal of frame kCutFrame + 1
			rig.After(int64_t(n1));
			FrameAnalysis::Scene carrier, cutGroup;
			Assert::IsTrue(store.PeekScene(n1, carrier));
			Assert::IsTrue(carrier.cut && carrier.cutLate, L"the cut is delivered with its successor's own seal");
			Assert::IsTrue(store.PeekScene(kCutFrame, cutGroup));
			Assert::IsTrue(cutGroup.confirmed && cutGroup.cutHere);
			Assert::IsFalse(cutGroup.cut, L"the delivered cut is not rewritten");
			Assert::IsTrue(store.PeekScene(n3, aheadGroup));
			Assert::IsFalse(aheadGroup.cutLate, L"still none on the ahead frame");
			Assert::AreEqual(lateBefore + 1, rig.sk.SceneCountersOf(rig.Host()).late, L"one cut, one delivery");
		}

		// GF-P2a MUST-7 (stranded): the cut frame N is sealed and its group is out provisional. An ahead ask of N + 1 confirms N, whose Vp
		// group the seal of N + 1 will skip (the seal finds N confirmed), so the cut could never be delivered: the ahead N + 1 carries it
		// (its natural predecessor's late cut), once.
		TEST_METHOD(AnAheadSuccessorOfTheCutFrameCarriesTheCutOnceInsteadOfStrandingIt)
		{
			Rig rig(Lane("on"));
			ShapeTwoStillShots(rig);
			FrameAnalysis::Store store(FrameAnalysis::RingDepth(4, 4));
			Tap::SetStore(rig.Host(), &store);
			SceneStoreGuard guard(rig.Host());
			FeedAheadWindow(rig, 8);
			RunToTheSealOfTheCut(rig, &store);
			rig.After(int64_t(kCutFrame));
			FrameAnalysis::Scene cutGroup, carrier;
			Assert::IsTrue(store.PeekScene(kCutFrame, cutGroup));
			Assert::IsFalse(cutGroup.confirmed || cutGroup.cut, L"the cut frame's group is out provisional");
			const uint64_t n1 = kCutFrame + 1, n2 = kCutFrame + 2;
			store.Begin(SceneTestId(n2), FrameAnalysis::kScene);
			rig.Ingress(int64_t(n1));
			rig.Deliver(n2);
			const uint64_t lateBefore = rig.sk.SceneCountersOf(rig.Host()).late;
			const Vpv::Skeleton::SceneAnswer ahead = rig.sk.SceneOf(rig.Host(), n1);   // ahead of the seal (kCutFrame): confirms the cut frame
			Assert::IsTrue(ahead.known, (L"the ahead successor is answered; " + WhyOf(ahead)).c_str());
			Assert::IsTrue(store.PeekScene(n1, carrier));
			Assert::IsTrue(carrier.cut && carrier.cutLate, L"the ahead successor carries the confirmed cut of its natural predecessor");
			Assert::IsTrue(store.PeekScene(kCutFrame, cutGroup));
			Assert::IsTrue(cutGroup.confirmed && cutGroup.cutHere, L"the cut frame's group was confirmed");
			Assert::IsFalse(cutGroup.cut, L"the delivered cut is not rewritten");
			Assert::AreEqual(lateBefore + 1, rig.sk.SceneCountersOf(rig.Host()).late);
			// The walk goes on to N + 2: that confirms N + 1's group (the seal then finds it present), and delivers nothing more.
			Assert::IsTrue(rig.sk.SceneOf(rig.Host(), n2).known);
			const Vpv::Skeleton::SceneCounters before = rig.sk.SceneCountersOf(rig.Host());
			rig.Begin(int64_t(n1));
			const Vpv::Skeleton::SceneCounters after = rig.sk.SceneCountersOf(rig.Host());
			Assert::AreEqual(before.alreadyPresent + 1, after.alreadyPresent, L"the seal found the ahead group present");
			Assert::AreEqual(before.late, after.late, L"no second delivery");
			Assert::IsTrue(store.PeekScene(n1, carrier));
			Assert::IsTrue(carrier.cut && carrier.cutLate, L"the carried cut stays on the group");
			FrameAnalysis::Scene next;
			Assert::IsTrue(store.PeekScene(n2, next));
			Assert::IsFalse(next.cut, L"exactly once");
			rig.After(int64_t(n1));
		}

		// GF-P2a MUST-7 (cache): the scene cache is the render frame's classification. An ahead ask between two asks of the render frame
		// must not overwrite it: the second ask of the render frame is still a cache hit with the same answer.
		TEST_METHOD(AnAheadAskDoesNotEvictTheRenderFramesSceneCache)
		{
			Rig rig(Lane("on"));
			FeedAheadWindow(rig, 8);
			RunToTheSealOfFrame4(rig);
			rig.Ingress(5);
			rig.Deliver(6);
			rig.Ingress(6);
			rig.Deliver(7);
			rig.Ingress(7);
			rig.Deliver(8);
			rig.Begin(4);
			const Vpv::Skeleton::SceneAnswer first = rig.sk.SceneOf(rig.Host(), 4);
			Assert::IsTrue(first.known, (L"the render frame is answered; " + WhyOf(first)).c_str());
			const Vpv::Skeleton::SceneAnswer ahead = rig.sk.SceneOf(rig.Host(), 8);
			Assert::IsTrue(ahead.known, (L"the ahead frame is answered; " + WhyOf(ahead)).c_str());
			const Vpv::Skeleton::SceneCounters before = rig.sk.SceneCountersOf(rig.Host());
			const Vpv::Skeleton::SceneAnswer second = rig.sk.SceneOf(rig.Host(), 4);
			const Vpv::Skeleton::SceneCounters after = rig.sk.SceneCountersOf(rig.Host());
			Assert::IsTrue(second.known);
			Assert::AreEqual(before.cacheHits + 1, after.cacheHits, L"the second ask of the render frame is a cache hit");
			Assert::AreEqual(before.liveFills, after.liveFills, L"and nothing was classified again");
			Assert::AreEqual(first.cut, second.cut);
			Assert::AreEqual(first.fade, second.fade);
			Assert::AreEqual(first.cutLate, second.cutLate);
			Assert::AreEqual(first.cutHere, second.cutHere);
			Assert::AreEqual(first.confirmed, second.confirmed);
			rig.After(4);
		}

		// GF-P2a S2b (T4 corrected after eec4406dd): the look-ahead walk keeps ONE new call at the front of the window, N + kAheadCeiling,
		// each render frame; the store holds a record for every arrived frame, so the front's group is published ahead and the seal of
		// every frame finds it (alreadyPresent). The ahead fills never keep their fade run in the 8-entry run ring and the render frames'
		// runs are never rebuilt with reduced evidence: the whole-run sceneRunReduced delta is 0 once the walk is primed. (The front is
		// the window's computed ahead depth: a first fill beyond it is NotPlannedAhead.) The published fadeKeep of a late fade frame
		// equals a store-off, render-order reference run's.
		TEST_METHOD(ALookAheadWalkNeverForcesTheRenderFramesFadeRunsToBeRebuiltReduced)
		{
			constexpr uint64_t kC = VpSkeleton::kAheadCeiling;
			constexpr int64_t kLast = 40;
			constexpr int64_t kLate = kLast - 2;   // a late fade frame (> 8 + kAheadCeiling)
			Rig rig(Lane("on"));
			ShapeASlowFade(rig);
			FrameAnalysis::Store store(FrameAnalysis::RingDepth(4, 4));
			Tap::SetStore(rig.Host(), &store);
			SceneStoreGuard guard(rig.Host());
			FeedAheadWindow(rig, uint16_t(kC));
			rig.ResyncApply("on");
			rig.Life(RE::Lifecycle::GpuCreated);
			uint64_t delivered = 0;
			uint64_t reducedAfterPrime = 0;
			FrameAnalysis::Scene lateGroup;
			bool haveLate = false;
			for (int64_t s = 1; s <= kLast; ++s)
			{
				rig.Ingress(s);
				while (delivered < uint64_t(s) + kC + 1)
				{
					++delivered;
					rig.Deliver(delivered);
					store.Begin(SceneTestId(delivered), FrameAnalysis::kScene);   // the record opens at ARRIVAL, ahead frames included
				}
				const Vpv::Skeleton::SceneCounters before = rig.sk.SceneCountersOf(rig.Host());
				rig.Begin(s);
				const Vpv::Skeleton::SceneCounters afterSeal = rig.sk.SceneCountersOf(rig.Host());
				PresentFacts(rig, s);
				if (s == 1)
				{
					for (uint64_t f = 2; f <= kC; ++f)   // prime the walk in order: frames 2 .. kC (the front of frame 1 is 1 + kC, below)
						Assert::IsTrue(rig.sk.SceneOf(rig.Host(), f).known, (L"priming frame " + std::to_wstring(f)).c_str());
				}
				else
				{
					Assert::AreEqual(before.alreadyPresent + 1, afterSeal.alreadyPresent, (L"the seal of frame " + std::to_wstring(s) + L" found the ahead group").c_str());
					Assert::AreEqual(before.liveFills, afterSeal.liveFills, L"the seal classified nothing itself");
				}
				const Vpv::Skeleton::SceneAnswer front = rig.sk.SceneOf(rig.Host(), uint64_t(s) + kC);
				Assert::IsTrue(front.known, (L"the walk front N + kAheadCeiling is answered; " + WhyOf(front)).c_str());
				if (s == 1)
					reducedAfterPrime = rig.sk.SceneCountersOf(rig.Host()).fadeRunReduced;
				if (s == kLate)
					haveLate = store.PeekScene(uint64_t(s), lateGroup);
				rig.After(s);
			}
			Assert::AreEqual(reducedAfterPrime, rig.sk.SceneCountersOf(rig.Host()).fadeRunReduced, L"no fade run was rebuilt with reduced evidence");
			Assert::IsTrue(haveLate, L"the late fade frame's group is in the store");

			// The reference: store off, render order (Begin(s), then SceneOf(s)); the same picture shape.
			Rig ref(Lane("on"));
			ShapeASlowFade(ref);
			ref.ResyncApply("on");
			ref.Life(RE::Lifecycle::GpuCreated);
			Vpv::Skeleton::SceneAnswer refLate;
			for (int64_t s = 1; s <= kLate; ++s)
			{
				ref.Ingress(s);
				if (s == 1)
					ref.Deliver(1);
				ref.Deliver(uint64_t(s) + 1);
				ref.Begin(s);
				const Vpv::Skeleton::SceneAnswer a = ref.sk.SceneOf(ref.Host(), uint64_t(s));
				if (s == kLate)
					refLate = a;
				ref.After(s);
			}
			Assert::IsTrue(refLate.known, (L"the reference answers the late frame; " + WhyOf(refLate)).c_str());
			Assert::AreEqual(refLate.fade, lateGroup.fade, L"the walked run's fade equals the render-order reference");
			Assert::AreEqual(double(refLate.fadeKeep), double(lateGroup.fadeKeep), 1e-6, L"and so does the published fadeKeep");
		}

		// C5-SKREDS MUST-1 (gate R4 on a23f8c3b3; one computation per frame, consolidation rule): a walk front N + kAheadCeiling is beyond
		// the lane's slots, so P4 and (through the Acquire chain) P1 of that frame are made by the owner fill in the memo's spot. When the
		// frame later fits the lane, the lane WORKER claims it: it must ADOPT that publication (copy, counted workerAdopted), never compute
		// P1 / P4 of the frame a second time. The worker law is the real one, stepped on this thread (threads off: deterministic, no seal
		// can cancel a running job, so the compute-once detector's per-frame count is exact). Every lane compute path notes "lane:<id>"
		// (worker, seal live, ahead claim, owner form) and the kernels note U3 (P1) / U6 (P4): a duplicate frame in any of those rows is a
		// second computation. Not vacuous: the worker made frames itself, the memo made walk fronts, and the worker met and adopted them.
		TEST_METHOD(AWalkFrontTheMemoMadeIsAdoptedByTheLaneWorkerNotComputedTwice)
		{
			constexpr uint64_t kC = VpSkeleton::kAheadCeiling;
			constexpr int64_t kLast = 24;
			ComputeOnce::SetEnabled(true);
			ComputeOnce::Global().Reset();
			struct DetectorOff
			{
				~DetectorOff()
				{
					ComputeOnce::Global().Reset();
					ComputeOnce::SetEnabled(false);
				}
			} detectorOff;
			Rig rig(Lane("on"));
			VpSkeleton::CardProfile card;   // no render device in a unit test: a card with room for the lane (the CPU products are laned)
			card.vramBudgetBytes = 1ull << 30;
			card.systemRamFreeBytes = 1ull << 30;
			card.uploadGBps = 10.0;
			rig.sk.SetCardForTesting(card);
			rig.sk.SetWorkerThreadsForTesting(false);   // the worker law is stepped below
			FeedAheadWindow(rig, uint16_t(kC));
			rig.ResyncApply("on");
			rig.Life(RE::Lifecycle::GpuCreated);
			uint64_t delivered = 0;
			for (int64_t s = 1; s <= kLast; ++s)
			{
				rig.Ingress(s);
				while (delivered < uint64_t(s) + kC + 1)
					rig.Deliver(++delivered);
				rig.Begin(s);
				PresentFacts(rig, s);
				VpSkeleton::AnalysisLane* lane = rig.sk.LaneOf(rig.Host());
				Assert::IsNotNull(lane);
				int steps = 0;
				while (lane->StepWorker(VpSkeleton::Backend::Cpu, 0) && ++steps < 10000)
				{
				}
				Assert::IsTrue(steps < 10000, L"the lane worker went idle");
				WalkP4(rig, uint64_t(s) + kC);   // the walk front: the memo's owner fill when the lane cannot hold it
				rig.After(s);
			}
			VpSkeleton::AnalysisLane* lane = rig.sk.LaneOf(rig.Host());
			Assert::IsNotNull(lane);
			const std::shared_ptr<VpSkeleton::LaneState> st = lane->State();
			Assert::IsTrue(st != nullptr && st->runner != nullptr, L"the lane state");
			const VpSkeleton::LaneProductCounters& c1 = st->runner->Counters(VpSkeleton::kP1HalfLuma);
			const VpSkeleton::LaneProductCounters& c4 = st->runner->Counters(VpSkeleton::kP4SourceStats);
			Assert::IsTrue(c1.published.load() > c1.workerAdopted.load() && c4.published.load() > c4.workerAdopted.load(),
				L"the lane worker computed frames itself (not vacuous)");
			VpSkeleton::SkeletonMemo* memo = rig.sk.MemoOf(rig.Host());
			Assert::IsNotNull(memo);
			Assert::IsTrue(memo->CountersOf(VpSkeleton::kP4SourceStats).filledAtRead > 0 && memo->CountersOf(VpSkeleton::kP1HalfLuma).filledAtRead > 0,
				L"the owner fill made walk fronts the lane could not hold (not vacuous)");
			Assert::IsTrue(c1.workerAdopted.load() > 0 && c4.workerAdopted.load() > 0, L"the worker met frames the owner fill had made, and adopted them");
			Assert::IsTrue(memo->CountersOf(VpSkeleton::kP1HalfLuma).workerAdopted > 0 && memo->CountersOf(VpSkeleton::kP4SourceStats).workerAdopted > 0,
				L"counted in the memo's ledger too");
			const std::string units[] = { "lane:" + std::to_string(int(VpSkeleton::kP1HalfLuma)), "lane:" + std::to_string(int(VpSkeleton::kP4SourceStats)),
				"U3", "U6" };
			int seen = 0;
			for (const ComputeOnce::UnitRow& r : ComputeOnce::Global().Rows())
				for (const std::string& u : units)
					if (r.unit == u)
					{
						++seen;
						Assert::IsTrue(r.frames > 0, L"the detector saw the unit's frames");
						Assert::AreEqual(uint64_t(0), r.dupFrames, (L"one computation per frame: " + std::wstring(u.begin(), u.end())).c_str());
					}
			Assert::IsTrue(seen >= 4, L"every lane compute row of P1 and P4 was seen");
		}

		// GF-P2a MUST-8 (order): one thread fills frame s (provisional: s + 1 not judged yet) while the render thread fills s + 1, which
		// confirms s and rewrites its group. Whatever the interleaving, the group of s ends CONFIRMED (a late provisional publish never
		// lands over, or after, the confirmation).
		TEST_METHOD(AProvisionalFillOnAnotherThreadNeverLeavesAConfirmedFramesGroupProvisional)
		{
			constexpr int kIter = 200;
			Rig rig(Lane("on"), 220);
			FrameAnalysis::Store store(FrameAnalysis::RingDepth(4, 4));
			Tap::SetStore(rig.Host(), &store);
			SceneStoreGuard guard(rig.Host());
			FeedAheadWindow(rig, 8);
			rig.ResyncApply("on");
			rig.Life(RE::Lifecycle::GpuCreated);
			for (int64_t s = 1; s <= 3; ++s)
				rig.Frame(s);
			uint64_t delivered = 4;   // Frame(1..3) delivered 1 .. 4
			int misses = 0;
			for (int i = 0; i < kIter; ++i)
			{
				const int64_t s = 5 + i;
				while (delivered < uint64_t(s) + 1)
					rig.Deliver(++delivered);
				rig.Ingress(s - 1);
				rig.Begin(s - 1);
				rig.After(s - 1);
				store.Begin(SceneTestId(uint64_t(s)), FrameAnalysis::kScene);
				WalkP4(rig, uint64_t(s));
				WalkP4(rig, uint64_t(s) + 1);
				std::atomic<bool> go{ false };
				Vpv::Skeleton::SceneAnswer other;
				std::thread t([&] {
					while (!go.load())
						std::this_thread::yield();
					other = rig.sk.SceneOf(rig.Host(), uint64_t(s));
				});
				go = true;
				const Vpv::Skeleton::SceneAnswer render = rig.sk.SceneOf(rig.Host(), uint64_t(s) + 1);
				t.join();
				Assert::IsTrue(render.known, (L"iteration " + std::to_wstring(i) + L": the render thread's ask is answered").c_str());
				FrameAnalysis::Scene group;
				if (!store.PeekScene(uint64_t(s), group))
				{
					++misses;   // neither fill wrote the group: the off-thread ask missed and the render fill had nothing to rewrite
					continue;
				}
				Assert::IsTrue(group.confirmed, (L"iteration " + std::to_wstring(i) + L": frame " + std::to_wstring(s) + L" ends confirmed").c_str());
			}
			Assert::IsTrue(misses < kIter / 2, L"the other thread's fill really wrote groups (else this tests nothing)");
		}

		// GF-P2a S12: a scene miss shows the skeleton's own reason (SceneAnswer::why, an AcquireCause token or the like); only when it
		// gives none does SceneGroupOf fall back to the generic text.
		TEST_METHOD(ASceneGroupMissNamesTheSkeletonsOwnReasonAndFallsBackToTheGenericText)
		{
			MissingSceneDemand* fake = new MissingSceneDemand();
			Vpv::Hub::ExtensionHub::Instance().ResetModulesForTesting();
			Vpv::Hub::ExtensionHub::Instance().AddModule(std::unique_ptr<Vpv::Hub::ISkeletonModule>(fake));
			struct HubReset
			{
				~HubReset() { Vpv::Hub::ExtensionHub::Instance().ResetModulesForTesting(); }
			} hubReset;
			int host = 0;
			FrameAnalysis::Scene scene;
			const char* why = nullptr;
			fake->why = "not_planned_ahead";
			Assert::IsFalse(Vpv::Skeleton::SceneGroupOf(&host, 7, scene, &why));
			Assert::IsNotNull(why);
			Assert::AreEqual(std::string("not_planned_ahead"), std::string(why), L"the skeleton's own token is the reason");
			fake->why = "";
			why = nullptr;
			Assert::IsFalse(Vpv::Skeleton::SceneGroupOf(&host, 7, scene, &why));
			Assert::IsNotNull(why);
			Assert::IsTrue(std::string(why).find("the skeleton could not classify the frame") == 0, L"no reason given: the generic text");
			fake->why = nullptr;   // a null reason is no reason either
			why = nullptr;
			Assert::IsFalse(Vpv::Skeleton::SceneGroupOf(&host, 7, scene, &why));
			Assert::IsTrue(why != nullptr && std::string(why).find("the skeleton could not classify the frame") == 0);
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
			// The same pair classification the shared unit gives for those two records (U7): SceneFactsOf derives the fade from it and keeps
			// the v1 pair cut / flash / cutPossible as pair* (WAIVER-U7ACC-SKELETON-SCENE: cut / flash / cutPossible are now the v2 sequence
			// law's verdict: the same as the shared CutTracker gives over the readable P4 records up to frame 4, judged without frame 5.
			// WAIVER-U7ACC-NOISE-VERDICT: the earlier "no cut on noise" line failed because P4 kept one previous record only, so the judge
			// had no context; the skeleton now declares its P4 read at the scene judge's lag, and the line holds again, pinned below).
			VpSkeleton::AnalysisLane* lane = rig.sk.LaneOf(rig.Host());
			const VpSkeleton::AnalysisLane::ProductRead cur = lane->ReadOf(VpSkeleton::kP4SourceStats, 4);
			const VpSkeleton::AnalysisLane::ProductRead prev = lane->ReadOf(VpSkeleton::kP4SourceStats, 3);
			VpLaneProducts::SourceStatsRecord a, c;
			Assert::IsTrue(VpLaneProducts::ReadSourceStats(prev.cpu, prev.cpuBytes, &a) && VpLaneProducts::ReadSourceStats(cur.cpu, cur.cpuBytes, &c));
			const VpLaneProducts::EventSignals e = VpLaneProducts::ClassifyEvents(a, c, false, false);
			Assert::AreEqual(e.cut, f.pairCut);
			Assert::AreEqual(e.flash, f.pairFlash);
			Assert::AreEqual(e.fade, f.fade);
			Assert::AreEqual(e.cutPossible, f.pairCutPossible);
			Assert::IsFalse(f.confirmed, L"frame 5 is not sealed: provisional");
			VpUnit::CutFadeJudge::CutTracker tracker;
			tracker.Reset();
			VpLaneProducts::SourceStatsRecord r;
			int readable = 0;
			for (int64_t s = 1; s <= 4; ++s)
			{
				const VpSkeleton::AnalysisLane::ProductRead read = lane->ReadOf(VpSkeleton::kP4SourceStats, uint64_t(s));
				if (read.cpu != nullptr && VpLaneProducts::ReadSourceStats(read.cpu, read.cpuBytes, &r))
				{
					tracker.Push(uint64_t(s), r);
					++readable;
				}
			}
			// The skeleton declares its P4 read at lag kScenePrimeFrames (the scene judge's window), so frames 1 .. 4 are all kept.
			Assert::AreEqual(4, readable, L"P4 records 1 .. 4 are readable at the seal of 4 (the scene judge's lag)");
			const VpUnit::CutFadeJudge::CutVerdict expected = tracker.Provisional();
			Assert::IsTrue(expected.known && expected.seq == 4u);
			Assert::AreEqual(expected.cut, f.cut, L"the shared sequence law's verdict, not a private one");
			// Pinned: with its context frames (L of the pairs 1->2 and 2->3 about 21-24) frame 4 (L about 21.3, spike bar 43.7) is no spike.
			// The BINDING margin is the hard tier: H 0.457 against kHardH 0.5 (G about 37 >= kHardG), so a lower kHardH or a coarser
			// histogram makes this a cut. A later law or window change shows up here.
			Assert::IsFalse(expected.cut, L"noise frames with their context: no sequence-law cut");
			Assert::AreEqual(expected.flash, f.flash);
			Assert::AreEqual(expected.candidate, f.cutPossible);
			Assert::AreEqual(expected.gridLaw, f.gridLaw);
			Assert::AreEqual(e.histDistance, f.histDistance);
			Assert::IsTrue(f.histDistance > 0u, L"two different noise frames never have identical histograms");
			Assert::AreEqual(c.mean, f.curMean);
			Assert::AreEqual(a.mean, f.prevMean);
			Assert::IsTrue(f.curSpread > 0u);
			rig.After(4);
			Assert::IsFalse(rig.sk.SceneFactsOf(rig.Host(), 1, f), L"there is no frame before 1");
		}

		// U7 v3 (102b U7-MULTIFRAME-FADE-LAW): the scene product's fade is the pair law OR the multi-frame run, and the run the
		// skeleton keeps in its ring equals one fold over the stream (the D8 harness's copy). Then one frame is not classified (a gap): the
		// next frame rebuilds the run from the lane's older P4 records, counted as exact or reduced, and an exact rebuild changes nothing.
		TEST_METHOD(ASlowFadeThroughTheSceneProductEqualsTheStreamFold)
		{
			const int frames = 60, gap = 40;
			Rig rig(Lane("on"), size_t(frames) + 4);
			ShapeASlowFade(rig);
			rig.ResyncApply("on");
			rig.Life(RE::Lifecycle::GpuCreated);
			VpUnit::CutFadeJudge::FadeRun stream;
			int compared = 0;
			for (int64_t s = 1; s <= frames; ++s)
			{
				rig.Ingress(s);
				if (s == 1)
					rig.Deliver(1);
				rig.Deliver(uint64_t(s) + 1);
				rig.Begin(s);
				if (s >= 2)
				{
					VpSkeleton::AnalysisLane* lane = rig.sk.LaneOf(rig.Host());
					const VpSkeleton::AnalysisLane::ProductRead cur = lane->ReadOf(VpSkeleton::kP4SourceStats, uint64_t(s));
					const VpSkeleton::AnalysisLane::ProductRead prev = lane->ReadOf(VpSkeleton::kP4SourceStats, uint64_t(s) - 1);
					VpLaneProducts::SourceStatsRecord a, c;
					if (prev.cpu && cur.cpu && VpLaneProducts::ReadSourceStats(prev.cpu, prev.cpuBytes, &a) &&
						VpLaneProducts::ReadSourceStats(cur.cpu, cur.cpuBytes, &c))
					{
						const VpLaneProducts::EventSignals e = VpLaneProducts::ClassifyEvents(a, c, false, false);
						const bool expected = VpUnit::CutFadeJudge::FoldFadeRun(a, c, e, stream) || e.fade;
						if (s != gap)
						{
							const Vpv::Skeleton::SceneCounters before = rig.sk.SceneCountersOf(rig.Host());
							Vpv::Skeleton::SceneFacts f;
							Assert::IsTrue(rig.sk.SceneFactsOf(rig.Host(), uint64_t(s), f));
							Assert::AreEqual(e.cut, f.pairCut, L"the pair cut is the pair law's (the delivered cut is the sequence law's since U7ACC)");
							Assert::AreEqual(e.flash, f.pairFlash);
							const Vpv::Skeleton::SceneCounters after = rig.sk.SceneCountersOf(rig.Host());
							const uint64_t rebuilt = after.fadeRunRebuilt - before.fadeRunRebuilt;
							const uint64_t reduced = after.fadeRunReduced - before.fadeRunReduced;
							if (s == gap + 1)
							{
								Assert::AreEqual(uint64_t(1), rebuilt + reduced, L"the frame after the gap missed its ring entry: counted once");
								if (rebuilt == 1)
									Assert::AreEqual(expected, f.fade, L"an exact rebuild equals the stream fold");
							}
							else
							{
								Assert::AreEqual(uint64_t(0), rebuilt + reduced, L"in order, the ring is the path");
								if (s < gap)
									Assert::AreEqual(expected, f.fade, L"the ring path equals one fold over the stream");
							}
							++compared;
						}
					}
				}
				rig.After(s);
			}
			Assert::IsTrue(compared >= frames / 2, L"the lane served the P4 pairs");
			const Vpv::Skeleton::SceneCounters end = rig.sk.SceneCountersOf(rig.Host());
			Assert::AreEqual(uint64_t(0), end.published + end.notReady, L"no store: only this test classified (the gap is real)");
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
				// REG-1 (doc 96 s3.4 option B): this lane holds only core products (P1, P4; no GPU gap, no device), which are never
				// ladder steps, so there is nothing to shed (was MaxLevel() > 0); the gates are told apart by the overload pin instead.
				Assert::AreEqual(0, int(lane->GetRegulator().MaxLevel()), L"REG-1: a core-only lane has no ladder");
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
					Assert::IsFalse(lane->GetRegulator().PinnedAtMinimum(), L"observe: never judged overloaded");
					Assert::IsTrue(Has(rig.Lines(RE::StatusKind::Fallbacks), "host-measured render-only frame(s) recorded"), L"visible: recorded, not fed");
					Assert::IsTrue(rig.Command().find("Skeleton regulator gate: observe") != std::string::npos, L"Ctrl+Alt+D");
				}
				else
				{
					// ORDER-DEPENDENT (audit SHOULD 3): frames 151..170 above carry no host-measured span, so they are UNFED. With S3 (unfed
					// recovery; P6-rest audit S3: ramps like the fed path, one step per dwell of unfed windows) the regulator starts restoring its
					// level after one window of unfed frames (WindowFrames(), 48 today); the 20-frame
					// status refresh must stay shorter than that window or the step-downs asserted here are already undone.
					Assert::IsTrue(size_t(20) < lane->GetRegulator().WindowFrames(), L"the status refresh is shorter than one regulator window");
					Assert::AreEqual(n, t.fed, L"act: fed as before");
					Assert::IsTrue(t.measured);
					// REG-1: was Level() < MaxLevel() and backOffs > 0; with nothing sheddable the fed overload pins at the minimum (the
					// overload alarm) and the core keeps running.
					Assert::IsTrue(lane->GetRegulator().PinnedAtMinimum(), L"act: the fed overload raises the alarm");
					(void)backOffs;
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


		// GEM-W1A S2 (skeleton MUST-6): the module's host tables (gpuSlots_, memoHosts_) are HostSlots. A released host's slot claimed by
		// another host never answers the first host's Aux with the second's; a re-claim of the first starts with no aux of its own.
		TEST_METHOD(HostSlotsAuxOfAReleasedAndReclaimedHostIsNeverAnotherHostsAux)
		{
			struct Entry { const void* host = nullptr; };
			Vpv::Hub::HostSlots<Entry, 2> slots;
			int a = 0, b = 0, auxA = 0, auxB = 0;
			Assert::IsTrue(slots.Claim(&a), L"A claimed");
			slots.SetAux(&a, &auxA);
			Assert::IsTrue(slots.Aux(&a) == &auxA, L"A's aux");
			slots.Remove(&a, true);   // A released: slot and aux cleared
			Assert::IsTrue(slots.Aux(&a) == nullptr, L"a released host has no aux");
			Assert::IsTrue(slots.Claim(&b), L"B claims (the freed slot first)");
			slots.SetAux(&b, &auxB);
			Assert::IsTrue(slots.Aux(&a) == nullptr, L"A never reads B's aux");
			Assert::IsTrue(slots.Aux(&b) == &auxB, L"B's own aux");
			Assert::IsTrue(slots.Claim(&a), L"A re-claims (another slot)");
			Assert::IsTrue(slots.Aux(&a) == nullptr, L"the re-claim has no aux before its own SetAux");
			Assert::IsTrue(slots.Aux(&b) == &auxB, L"B is untouched");
			slots.SetAux(&a, &auxA);
			Assert::IsTrue(slots.Aux(&a) == &auxA && slots.Aux(&b) == &auxB && slots.Used() == 2u, L"each host its own aux");
		}

		// GF-P2b S14.2c: the same guarantee under concurrency. A reader loops Aux(A) while a writer releases A, lets B claim the freed slot and
		// set its aux, releases B and re-claims A with its own aux. The reader must never see B's aux through A. Probabilistic by nature:
		// 200000 iterations on each thread (a violation is counted, so a pass is not proof, a failure is).
		TEST_METHOD(HostSlotsAuxOfAHostIsNeverAnotherHostsAuxWhileTheSlotIsReusedConcurrently)
		{
			struct Entry { const void* host = nullptr; };
			constexpr int kIterations = 200000;
			Vpv::Hub::HostSlots<Entry, 2> slots;
			int a = 0, b = 0, aAux = 0, bAux = 0;
			Assert::IsTrue(slots.Claim(&a), L"A claimed");
			slots.SetAux(&a, &aAux);
			std::atomic<bool> go{ false };
			std::atomic<int> violations{ 0 };
			std::thread reader([&] {
				while (!go.load())
					std::this_thread::yield();
				for (int i = 0; i < kIterations; ++i)
					if (slots.Aux(&a) == static_cast<void*>(&bAux))
						violations.fetch_add(1);
			});
			std::thread writer([&] {
				while (!go.load())
					std::this_thread::yield();
				for (int i = 0; i < kIterations; ++i)
				{
					slots.Remove(&a, true);
					slots.Claim(&b);
					slots.SetAux(&b, &bAux);
					slots.Remove(&b, true);
					slots.Claim(&a);
					slots.SetAux(&a, &aAux);
				}
			});
			go = true;
			reader.join();
			writer.join();
			Assert::AreEqual(0, violations.load(), L"A's aux was never B's");
		}

		// GEM-W1A S1 (skeleton MUST-3): a lane-bound memo is planned only at the lane's Configure. Its GPU need follows its window and its
		// lazily registered GPU fills, neither in the queue shape: a need that moves after the last Configure re-plans the memo's grant
		// (rate-limited like the declared VRAM users), so the lane's memo is granted from the same ledger instead of declining for ever.
		TEST_METHOD(ALaneBoundMemoGrantFollowsARaisedNeed)
		{
			VpSkeleton::VramUsers& registry = VpSkeleton::VramUsers::Instance();
			registry.ResetForTesting();
			const uint64_t mb = 1024ull * 1024ull;
			{
				Rig rig(Lane("on"));
				VpSkeleton::CardProfile card;
				card.key.luid = 78;
				card.vramBudgetBytes = 1024 * mb;
				card.vramUsageBytes = 0;
				rig.sk.SetCardForTesting(card);
				rig.ResyncApply("on");
				rig.Life(RE::Lifecycle::GpuCreated);
				int64_t s = 1;
				for (; s <= 150; ++s)   // past every first-period and queue re-plan: the graph is steady
					rig.Frame(s);
				const Vpv::Skeleton::FrameMemo fm = rig.sk.MemoFor(rig.Host(), uint64_t(s));
				Assert::IsTrue(fm.Valid() && fm.memo->LaneBound(), L"the lane's memo");
				Assert::AreEqual(uint64_t(0), fm.memo->GpuNeedBytes(), L"no GPU fill yet: no need");
				Assert::IsFalse(fm.memo->GpuGranted(), L"nothing to grant");
				VpSkeleton::OwnerFill g;
				g.reserveBytes = 1u << 20;
				g.fill = [](const VpSkeleton::OwnerFillRequest&, VpSkeleton::SkeletonMemo&, VpSkeleton::OwnerFillOutput& out) {
					static int tex = 0;
					out.gpu.resource = &tex;
					out.gpu.bytes = 1u << 20;
					return true;
				};
				Assert::IsTrue(rig.sk.RegisterOwnerFill(VpSkeleton::kP10GrainSplit, VpSkeleton::AcquireForm::Gpu, "repair/grain_split", g)
					== VpSkeleton::SkeletonMemo::Registration::Registered, L"a GPU owner fill registers lazily, after the lane's Configure");
				const uint64_t need = fm.memo->GpuNeedBytes();
				Assert::IsTrue(need >= (1u << 20), L"the memo's need rose");
				for (const int64_t end = s + 2; s <= end; ++s)
					rig.Frame(s);
				Assert::IsTrue(fm.memo->GpuGranted(), L"re-planned: the lane-bound memo is granted");
				Assert::IsTrue(fm.memo->GpuBudget() >= fm.memo->GpuNeedBytes(), L"the grant covers the raised need");
				// GF-P2b MUST-4 (2): the re-plan ran on the kept graph (a users re-plan) or rebuilt the state because its plan moved; which one is the probe's call.
				Assert::IsTrue(rig.sk.LaneOf(rig.Host())->UsersReplans() >= 1u || rig.sk.LaneOf(rig.Host())->PlanMovedRebuilds() >= 1u,
					L"the grant moved through the lane's re-plan");
			}
			registry.ResetForTesting();
		}

		// GF-P2b S10 (module level): a lane-bound memo's grant is the memo's own entry (kUserSkeletonMemo) of the lane's current users plan.
		TEST_METHOD(TheLaneBoundMemoGrantIsTheMemosOwnEntryOfTheUsersPlan)
		{
			VpSkeleton::VramUsers& registry = VpSkeleton::VramUsers::Instance();
			registry.ResetForTesting();
			const uint64_t mb = 1024ull * 1024ull;
			{
				Rig rig(Lane("on"));
				VpSkeleton::CardProfile card;
				card.key.luid = 79;
				card.vramBudgetBytes = 1024 * mb;
				card.vramUsageBytes = 0;
				rig.sk.SetCardForTesting(card);
				rig.ResyncApply("on");
				rig.Life(RE::Lifecycle::GpuCreated);
				int64_t s = 1;
				for (; s <= 150; ++s)
					rig.Frame(s);
				const Vpv::Skeleton::FrameMemo fm = rig.sk.MemoFor(rig.Host(), uint64_t(s));
				Assert::IsTrue(fm.Valid() && fm.memo->LaneBound(), L"the lane's memo");
				VpSkeleton::OwnerFill g;
				g.reserveBytes = 1u << 20;
				g.fill = [](const VpSkeleton::OwnerFillRequest&, VpSkeleton::SkeletonMemo&, VpSkeleton::OwnerFillOutput& out) {
					static int tex = 0;
					out.gpu.resource = &tex;
					out.gpu.bytes = 1u << 20;
					return true;
				};
				Assert::IsTrue(rig.sk.RegisterOwnerFill(VpSkeleton::kP10GrainSplit, VpSkeleton::AcquireForm::Gpu, "repair/grain_split", g)
					== VpSkeleton::SkeletonMemo::Registration::Registered);
				for (const int64_t end = s + 2; s <= end; ++s)
					rig.Frame(s);
				Assert::IsTrue(fm.memo->GpuGranted(), L"granted");
				const std::shared_ptr<const VpSkeleton::GovernorPlan> plan = rig.sk.LaneOf(rig.Host())->CurrentGovernorPlan();
				Assert::IsTrue(plan != nullptr);
				const VpSkeleton::UserGrant* u = plan->Find(VpSkeleton::kUserSkeletonMemo);
				Assert::IsNotNull(u, L"the memo's own entry is in the lane's users plan");
				Assert::AreEqual(uint64_t(u->bytes), uint64_t(fm.memo->GpuBudget()), L"the memo's grant is that entry's bytes");
			}
			registry.ResetForTesting();
		}

		// A GpuCreated with a fake render device: a zero-filled pl_gpu_t (log null), never used for a texture or a pass. With the lane on,
		// Reconcile constructs a SkeletonGpuDispatcher on it, whose pl_dispatch_create(gpu->log, gpu) only stores the two pointers.
		static void GpuCreatedWithDevice(Rig& rig, pl_gpu_t* fakeGpu)
		{
			RE::LifecycleFacts f;
			f.host = rig.Host();
			f.kind = RE::Lifecycle::GpuCreated;
			f.gpu = fakeGpu;
			rig.sk.OnLifecycle(f);
		}

		// GF-P2b S10b / MUST-4 (3): a memo that began with no lane (its ahead plan made by the no-lane grant, nothing fits) and then gets a
		// lane keeps no stale plan: the lane's Configure re-sets the memo's AheadPlan, so an ahead ask inside the window is not
		// NotPlannedAhead.
		TEST_METHOD(ALaneSwitchedOnAfterANoLaneMemoReplacesItsAheadPlan)
		{
			VpSkeleton::VramUsers& registry = VpSkeleton::VramUsers::Instance();
			registry.ResetForTesting();
			const uint64_t mb = 1024ull * 1024ull;
			{
				Rig rig(Lane("off"));
				pl_gpu_t fakeGpu{};
				VpSkeleton::CardProfile tight;
				tight.key.luid = 80;
				tight.vramBudgetBytes = 8 * mb;
				tight.vramUsageBytes = 7 * mb;
				rig.sk.SetCardForTesting(tight);
				rig.ResyncApply("on");
				GpuCreatedWithDevice(rig, &fakeGpu);
				// A no-lane memo exists only once a consumer asks for one (MemoFor makes it; a lane-off Begin never does): the ask a
				// consumer's frame hook makes in production.
				Assert::IsTrue(rig.sk.MemoFor(rig.Host(), 1).Valid(), L"a consumer's ask makes the no-lane memo");
				VpSkeleton::OwnerFill g;
				g.reserveBytes = 1u << 20;
				g.aheadDepth = 2;   // an ahead consumer is active: the window computes ahead
				g.fill = [](const VpSkeleton::OwnerFillRequest&, VpSkeleton::SkeletonMemo&, VpSkeleton::OwnerFillOutput& out) {
					static int tex = 0;
					out.gpu.resource = &tex;
					out.gpu.bytes = 1u << 20;
					return true;
				};
				Assert::IsTrue(rig.sk.RegisterOwnerFill(VpSkeleton::kP10GrainSplit, VpSkeleton::AcquireForm::Gpu, "repair/grain_split", g)
					== VpSkeleton::SkeletonMemo::Registration::Registered);
				SkeletonModule::QueueShape q;
				q.renderAhead = 1;
				q.laneLead = 1;
				q.lookAheadWalk = 6;
				rig.sk.SetQueueForTesting(q);
				int64_t s = 1;
				for (; s <= 8; ++s)
				{
					rig.Frame(s);
					PresentFacts(rig, s);
				}
				Assert::IsNull(rig.sk.LaneOf(rig.Host()), L"no lane yet");
				// The lane comes on with an ample card.
				VpSkeleton::CardProfile ample;
				ample.key.luid = 80;
				ample.vramBudgetBytes = 1024 * mb;
				ample.vramUsageBytes = 0;
				rig.sk.SetCardForTesting(ample);
				rig.cfg.Write(Lane("on"));
				rig.Life(RE::Lifecycle::SettingsApplied);
				VpSkeleton::SkeletonMemo* memo = rig.sk.MemoOf(rig.Host());
				Assert::IsNotNull(memo);
				for (int i = 0; i < 200 && !(memo->LaneBound() && memo->GpuGranted()); ++i, ++s)
				{
					rig.Frame(s);
					PresentFacts(rig, s);
				}
				Assert::IsTrue(memo->LaneBound() && memo->GpuGranted(), L"the lane-bound memo is granted");
				const VpSkeleton::AcquireResult r = memo->Acquire(VpSkeleton::kP10GrainSplit, uint64_t(s - 1) + 1u, VpSkeleton::AcquireForm::Gpu);
				Assert::IsTrue(r.cause != VpSkeleton::AcquireCause::NotPlannedAhead, L"the no-lane plan did not survive the lane's Configure");
			}
			registry.ResetForTesting();
		}

		// A non-view GPU owner fill (P10) registered after the lane bound, and the cause an Acquire of it gives at frame seq (render thread).
		static void RegisterGpuOwnerFill(Rig& rig)
		{
			VpSkeleton::OwnerFill g;
			g.reserveBytes = 1u << 20;
			g.fill = [](const VpSkeleton::OwnerFillRequest&, VpSkeleton::SkeletonMemo&, VpSkeleton::OwnerFillOutput& out) {
				static int tex = 0;
				out.gpu.resource = &tex;
				out.gpu.bytes = 1u << 20;
				return true;
			};
			Assert::IsTrue(rig.sk.RegisterOwnerFill(VpSkeleton::kP10GrainSplit, VpSkeleton::AcquireForm::Gpu, "repair/grain_split", g)
				== VpSkeleton::SkeletonMemo::Registration::Registered);
		}

		static VpSkeleton::AcquireCause GpuOwnerFillCause(Rig& rig, int64_t seq)
		{
			VpSkeleton::SkeletonMemo* memo = rig.sk.MemoOf(rig.Host());
			Assert::IsNotNull(memo);
			return memo->Acquire(VpSkeleton::kP10GrainSplit, uint64_t(seq), VpSkeleton::AcquireForm::Gpu).cause;
		}

		// GF-P2b MUST-3 (attach side), shadow lane: the lane has no GPU dispatcher, yet the attached device serves a non-view GPU owner fill.
		TEST_METHOD(AShadowLaneBoundMemoWithADeviceIsNotWithoutADispatcher)
		{
			Rig rig(Lane("shadow"));
			pl_gpu_t fakeGpu{};
			rig.ResyncApply("on");
			GpuCreatedWithDevice(rig, &fakeGpu);
			for (int64_t s = 1; s <= 4; ++s)
				rig.Frame(s);
			Assert::IsTrue(rig.sk.MemoOf(rig.Host()) != nullptr && rig.sk.MemoOf(rig.Host())->LaneBound(), L"the shadow lane is bound");
			RegisterGpuOwnerFill(rig);   // after the lane bound
			Assert::IsTrue(GpuOwnerFillCause(rig, 4) != VpSkeleton::AcquireCause::NoGpuDispatcher);
		}

		// The same with the lane switched off by configuration and RAISED by an external demand.
		TEST_METHOD(ARaisedLaneBoundMemoWithADeviceIsNotWithoutADispatcher)
		{
			Rig rig(Lane("off"));
			pl_gpu_t fakeGpu{};
			rig.ResyncApply("on");
			GpuCreatedWithDevice(rig, &fakeGpu);
			VpSkeleton::ConsumerDecl c;
			c.id = 0x0611;
			c.name = "raise test";
			VpSkeleton::AddConsume(c, VpSkeleton::kP4SourceStats, 0);
			int owner = 0;
			rig.sk.SetRaisingExternalDemand(rig.Host(), &owner, std::vector<VpSkeleton::ConsumerDecl>(1, c), std::vector<VpSkeleton::ProductId>(), "test");
			for (int64_t s = 1; s <= 4; ++s)
				rig.Frame(s);
			Assert::IsTrue(rig.sk.MemoOf(rig.Host()) != nullptr && rig.sk.MemoOf(rig.Host())->LaneBound(), L"the raised lane is bound");
			RegisterGpuOwnerFill(rig);
			Assert::IsTrue(GpuOwnerFillCause(rig, 4) != VpSkeleton::AcquireCause::NoGpuDispatcher);
			rig.sk.ClearExternalDemand(rig.Host(), &owner);
		}

		// GF-P2b MUST-3 (detach / attach after bind): a second GpuCreated without a device detaches it (no dispatcher left: the visible
		// cause NoGpuDispatcher); a GpuCreated with a device attaches it again.
		TEST_METHOD(ADeviceDetachedAfterTheBindLeavesNoDispatcherAndAReattachRestoresIt)
		{
			Rig rig(Lane("on"));
			pl_gpu_t fakeGpu{};
			rig.ResyncApply("on");
			GpuCreatedWithDevice(rig, &fakeGpu);
			for (int64_t s = 1; s <= 4; ++s)
				rig.Frame(s);
			Assert::IsTrue(rig.sk.MemoOf(rig.Host()) != nullptr && rig.sk.MemoOf(rig.Host())->LaneBound(), L"the lane is bound");
			RegisterGpuOwnerFill(rig);
			Assert::IsTrue(GpuOwnerFillCause(rig, 4) != VpSkeleton::AcquireCause::NoGpuDispatcher, L"attached: not without a dispatcher");
			RE::LifecycleFacts none;
			none.host = rig.Host();
			none.kind = RE::Lifecycle::GpuCreated;
			none.gpu = nullptr;   // the device goes, none comes
			rig.sk.OnLifecycle(none);
			rig.Frame(5);
			Assert::IsTrue(GpuOwnerFillCause(rig, 5) == VpSkeleton::AcquireCause::NoGpuDispatcher, L"detached: the visible cause");
			GpuCreatedWithDevice(rig, &fakeGpu);
			rig.Frame(6);
			Assert::IsTrue(GpuOwnerFillCause(rig, 6) != VpSkeleton::AcquireCause::NoGpuDispatcher, L"attached again");
		}

		// GF-P2b E2 (test first): on a fresh renderer the order GpuCreated (the device attaches) -> SettingsApplied (the store registers, the
		// tap generation moves) -> Begin must leave the memo its device. The dead-renderer branch of Reconcile used to detach it.
		TEST_METHOD(AStoreRegisteringAfterGpuCreatedDoesNotDetachTheNewRenderersDevice)
		{
			Rig rig(Lane("on"));
			pl_gpu_t fakeGpu{};
			rig.ResyncApply("on");
			// The renderer's memo exists before its device is known (a consumer's MemoFor makes it; neither GpuCreated nor Reconcile
			// does), so GpuCreated's own attach is what gives it the device.
			Assert::IsTrue(rig.sk.MemoFor(rig.Host(), 1).Valid(), L"a consumer's ask makes the memo");
			Assert::IsFalse(rig.sk.MemoOf(rig.Host())->HasGpuDevice(), L"no device before GpuCreated");
			GpuCreatedWithDevice(rig, &fakeGpu);
			VpSkeleton::SkeletonMemo* memo = rig.sk.MemoOf(rig.Host());
			Assert::IsNotNull(memo);
			Assert::IsTrue(memo->HasGpuDevice(), L"attached by GpuCreated");
			const uint64_t generationBefore = Tap::Generation(rig.Host());
			FrameAnalysis::Store store(FrameAnalysis::RingDepth(4, 4));
			Tap::SetStore(rig.Host(), &store);   // the store registers at the first SettingsApplied of a fresh renderer
			SceneStoreGuard guard(rig.Host());
			Assert::IsTrue(Tap::Generation(rig.Host()) != generationBefore, L"the tap generation moved");
			rig.Life(RE::Lifecycle::SettingsApplied);
			rig.Frame(1);   // Begin
			memo = rig.sk.MemoOf(rig.Host());
			Assert::IsNotNull(memo);
			Assert::IsTrue(memo->HasGpuDevice(), L"the new renderer's device is still attached after the Begin");
			Assert::IsTrue(rig.sk.MemoDeviceGpuForTesting(rig.Host()) == static_cast<const void*>(&fakeGpu), L"and it is the device of the current pl_gpu");
		}

		// GF-P2b E2 counter-case through the PUBLIC lifecycle (R2b): the dead-renderer Reconcile keeps only a device that is on the host's
		// CURRENT pl_gpu. A device not on it cannot be set up without a seam (every GpuCreated attaches its own pl_gpu as the current one), so
		// the reachable counterpart is pinned: after GpuReleased the renderer has no device and no current pl_gpu, a store registering and a
		// Begin make a new memo WITHOUT a device (nothing is resurrected), and a later GpuCreated attaches one.
		TEST_METHOD(AStoreRegisteringAfterGpuReleasedDoesNotResurrectADevice)
		{
			Rig rig(Lane("on"));
			pl_gpu_t fakeGpu{};
			rig.ResyncApply("on");
			GpuCreatedWithDevice(rig, &fakeGpu);
			rig.Frame(1);
			Assert::IsTrue(rig.sk.MemoOf(rig.Host()) != nullptr && rig.sk.MemoOf(rig.Host())->HasGpuDevice(), L"attached");
			rig.Life(RE::Lifecycle::GpuReleased);
			Assert::IsNull(rig.sk.MemoOf(rig.Host()), L"the memo went with the renderer");
			FrameAnalysis::Store store(FrameAnalysis::RingDepth(4, 4));
			Tap::SetStore(rig.Host(), &store);
			SceneStoreGuard guard(rig.Host());
			rig.Life(RE::Lifecycle::SettingsApplied);
			rig.Frame(2);
			const Vpv::Skeleton::FrameMemo fm = rig.sk.MemoFor(rig.Host(), 3);
			Assert::IsTrue(fm.Valid(), L"a memo is made again");
			Assert::IsFalse(fm.memo->HasGpuDevice(), L"but no device: the released one is not resurrected");
			GpuCreatedWithDevice(rig, &fakeGpu);
			Assert::IsTrue(rig.sk.MemoOf(rig.Host()) != nullptr && rig.sk.MemoOf(rig.Host())->HasGpuDevice(), L"a GpuCreated attaches one");
		}

		// GF-P2b E2 design behaviour (99ade1854): a renderer that died WITHOUT GpuReleased leaves its device on the host's current pl_gpu; the
		// dead-renderer Reconcile cannot tell it from a new renderer's, so it is kept until the next GpuCreated, which abandons it and
		// attaches the new renderer's own device.
		TEST_METHOD(ADeadRenderersStaleDeviceIsKeptUntilTheNextGpuCreated)
		{
			Rig rig(Lane("on"));
			pl_gpu_t staleGpu{}, newGpu{};
			rig.ResyncApply("on");
			GpuCreatedWithDevice(rig, &staleGpu);
			rig.Frame(1);
			VpSkeleton::SkeletonMemo* memo = rig.sk.MemoOf(rig.Host());
			Assert::IsNotNull(memo);
			Assert::IsTrue(memo->HasGpuDevice(), L"the old renderer's device");
			// The renderer dies without GpuReleased; a new one at the same address registers its store (the generation moves).
			FrameAnalysis::Store store(FrameAnalysis::RingDepth(4, 4));
			Tap::SetStore(rig.Host(), &store);
			SceneStoreGuard guard(rig.Host());
			rig.Life(RE::Lifecycle::SettingsApplied);
			rig.Frame(2);
			memo = rig.sk.MemoOf(rig.Host());
			Assert::IsNotNull(memo);
			Assert::IsTrue(memo->HasGpuDevice(), L"kept: indistinguishable from the new renderer's until its GpuCreated");
			Assert::IsTrue(rig.sk.MemoDeviceGpuForTesting(rig.Host()) == static_cast<const void*>(&staleGpu), L"the kept device is the stale one");
			GpuCreatedWithDevice(rig, &newGpu);   // abandons the stale device and attaches the new one
			memo = rig.sk.MemoOf(rig.Host());
			Assert::IsNotNull(memo);
			Assert::IsTrue(memo->HasGpuDevice(), L"the new renderer's own device is attached");
			Assert::IsTrue(rig.sk.MemoDeviceGpuForTesting(rig.Host()) == static_cast<const void*>(&newGpu), L"the stale device was abandoned and replaced");
		}

		// GF-P2b S1: a live switch to the Original pathway removes the renderer's memo host (nothing of the skeleton is left to seal or show);
		// switching back makes a NEW memo.
		TEST_METHOD(ALiveSwitchToTheOriginalPathwayRemovesTheMemoAndBackMakesANewOne)
		{
			Rig rig(Lane("on"));
			rig.ResyncApply("on");
			rig.Life(RE::Lifecycle::GpuCreated);
			for (int64_t s = 1; s <= 4; ++s)
				rig.Frame(s);
			const Vpv::Skeleton::FrameMemo before = rig.sk.MemoFor(rig.Host(), 5);
			Assert::IsTrue(before.Valid(), L"a memo with the lane on");
			const std::shared_ptr<VpSkeleton::SkeletonMemo> oldMemo = before.memo;
			const uint64_t sealsBefore = oldMemo->Seals();
			rig.Life(RE::Lifecycle::SettingsApplied, 1);   // code 1: the Original pathway
			Assert::IsNull(rig.sk.MemoOf(rig.Host()), L"the memo host is gone");
			Assert::IsFalse(rig.sk.MemoFor(rig.Host(), 5).Valid(), L"no memo for a frame");
			rig.Frame(5);
			Assert::AreEqual(sealsBefore, oldMemo->Seals(), L"a Begin does not seal the old memo");
			const std::vector<std::string> fb = rig.Lines(RE::StatusKind::Fallbacks);
			Assert::IsFalse(Has(fb, "skeleton acquire"), L"no memo line");
			Assert::IsFalse(Has(fb, "memo window"));
			Assert::IsFalse(Has(fb, "skeleton_queue_window"), L"no window line");
			rig.Life(RE::Lifecycle::SettingsApplied, 0);   // back to the VP-V pathway
			rig.Frame(6);
			const Vpv::Skeleton::FrameMemo after = rig.sk.MemoFor(rig.Host(), 7);
			Assert::IsTrue(after.Valid(), L"a memo again");
			Assert::IsTrue(after.memo.get() != oldMemo.get(), L"a NEW memo, not the old one");
		}

		// GF-P2b S3 (1), skeleton side: GpuReleased erases a host's external demand and the erase is COUNTED (ExternalErasures); a second
		// release with no entry left does not move the count.
		TEST_METHOD(AnExternalDemandEraseIsCountedOncePerErasedEntry)
		{
			Rig rig(Lane("on"));
			rig.ResyncApply("on");
			rig.Life(RE::Lifecycle::GpuCreated);
			for (int64_t s = 1; s <= 2; ++s)
				rig.Frame(s);
			VpSkeleton::ConsumerDecl c;
			c.id = 0x0612;
			c.name = "erase test";
			VpSkeleton::AddConsume(c, VpSkeleton::kP4SourceStats, 0);
			int owner = 0;
			Vpv::Skeleton::IExternalDemand* demand = &rig.sk;
			Assert::AreEqual(uint64_t(0), demand->ExternalErasures(rig.Host()));
			rig.sk.SetRaisingExternalDemand(rig.Host(), &owner, std::vector<VpSkeleton::ConsumerDecl>(1, c), std::vector<VpSkeleton::ProductId>(), "test");
			Assert::AreEqual(size_t(1), rig.sk.ExternalEntriesOf(rig.Host()));
			rig.Life(RE::Lifecycle::GpuReleased);
			Assert::AreEqual(uint64_t(1), demand->ExternalErasures(rig.Host()), L"one entry erased, once counted");
			Assert::AreEqual(size_t(0), rig.sk.ExternalEntriesOf(rig.Host()));
			rig.Life(RE::Lifecycle::GpuReleased);
			Assert::AreEqual(uint64_t(1), demand->ExternalErasures(rig.Host()), L"nothing left to erase: the count does not move");
		}

		static bool HasAheadDemand(VpSkeleton::SkeletonMemo* memo, uint16_t depth, const char* by)
		{
			for (const VpSkeleton::SkeletonMemo::AheadDemand& d : memo->ActiveAheadDemands())
				if (d.id == uint16_t(VpSkeleton::kP4SourceStats) && d.form == 0 && d.depth == depth && d.by != nullptr && std::string(d.by) == by)
					return true;
			return false;
		}

		static void DeclareT11Reader(Rig& rig, uint8_t aheadDepth, int* owner)
		{
			VpSkeleton::ConsumerDecl c;
			c.id = 0x0613;
			c.name = "t11 reader";
			VpSkeleton::AddConsume(c, VpSkeleton::kP4SourceStats, 0);
			c.aheadDepth = aheadDepth;
			rig.sk.SetExternalDemand(rig.Host(), owner, std::vector<VpSkeleton::ConsumerDecl>(1, c), std::vector<VpSkeleton::ProductId>());
		}

		// GF-P2b AHEAD-DECL call sites (gate G6 C2): a consumer's declared ahead depth reaches the lane fills (the lane is re-bound, once per
		// change), the requester is named, and a depth-only change neither rebuilds the lane state nor re-binds again when repeated. UNION
		// test: ConsumerDecl::aheadDepth is GF-P1's. The window is read after the NEXT Begin in (b) / (c): it is derived at the start of
		// a Begin, before that Begin's re-bind.
		TEST_METHOD(ADeclaredAheadDepthReachesTheLaneFillsAndTheWindowNamesTheRequester)
		{
			Rig rig(Lane("on"));
			rig.ResyncApply("on");
			FeedQueueTerms(rig);
			rig.Life(RE::Lifecycle::GpuCreated);
			int64_t s = 1;
			for (; s <= 4; ++s)
			{
				rig.Frame(s);
				PresentFacts(rig, s);
			}
			VpSkeleton::SkeletonMemo* memo = rig.sk.MemoOf(rig.Host());
			Assert::IsNotNull(memo);
			int owner = 0;
			// (a)
			const uint64_t binds0 = memo->LaneBinds();
			DeclareT11Reader(rig, 3, &owner);
			rig.Frame(s);
			PresentFacts(rig, s++);
			Assert::IsTrue(HasAheadDemand(memo, 3, "t11 reader"), L"(a) P4's lane fill declares depth 3 by the reader");
			Assert::AreEqual(binds0 + 1, memo->LaneBinds(), L"(a) one re-bind");
			// (b) only the depth changes
			const VpSkeleton::LaneState* stateBefore = rig.sk.LaneOf(rig.Host())->State().get();
			const uint64_t binds1 = memo->LaneBinds();
			DeclareT11Reader(rig, 9, &owner);
			rig.Frame(s);
			PresentFacts(rig, s++);
			Assert::IsTrue(rig.sk.LaneOf(rig.Host())->State().get() == stateBefore, L"(b) the lane state is the same");
			Assert::AreEqual(binds1 + 1, memo->LaneBinds(), L"(b) one re-bind");
			Assert::IsTrue(HasAheadDemand(memo, 9, "t11 reader"), L"(b) the uncapped request is the demand");
			// (c) the same declaration again
			const uint64_t binds2 = memo->LaneBinds();
			DeclareT11Reader(rig, 9, &owner);
			rig.Frame(s);
			PresentFacts(rig, s++);
			Assert::AreEqual(binds2, memo->LaneBinds(), L"(c) nothing changed: no re-bind");
			Assert::IsTrue(rig.sk.Active(rig.Host()), L"(c) the lane is still active");
			const VpSkeleton::QueueWindow w = memo->Window();   // derived at the Begin after the depth-9 re-bind
			Assert::IsTrue(w.aheadBinding == VpSkeleton::AheadBinding::Ceiling, L"(b) the ceiling binds");
			Assert::IsTrue(w.aheadBy != nullptr && std::string(w.aheadBy) == "t11 reader", L"(b) the window names the requester");
			const std::string text = VpSkeleton::AheadComputeText(w);
			Assert::IsTrue(text.find("by t11 reader") != std::string::npos && text.find("ceiling") != std::string::npos,
				std::wstring(text.begin(), text.end()).c_str());
			rig.sk.ClearExternalDemand(rig.Host(), &owner);
		}

		// (d) an owner fill that declares an aheadDepth but names no requester reads "owner fill (CPU)" in the window.
		TEST_METHOD(AnUnnamedAheadRequestKeepsTheFormWordingInTheWindow)
		{
			Rig rig(Lane("on"));
			FeedAheadWindow(rig, 8);   // a P10 CPU owner fill with an aheadDepth and no aheadBy
			rig.ResyncApply("on");
			rig.Life(RE::Lifecycle::GpuCreated);
			for (int64_t s = 1; s <= 5; ++s)
			{
				rig.Frame(s);
				PresentFacts(rig, s);
			}
			VpSkeleton::SkeletonMemo* memo = rig.sk.MemoOf(rig.Host());
			Assert::IsNotNull(memo);
			const VpSkeleton::QueueWindow w = memo->Window();
			Assert::IsTrue(w.aheadBy != nullptr && std::string(w.aheadBy) == "owner fill (CPU)");
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

		// W23-Phase4 D10: SceneRunBefore rebuild timing telemetry (fadeRunRebuildUsTotal / Max / Last)
		// and Ctrl+Alt+D / OnCommand("skeleton", "status") wiring.
		// Includes in-test negative control (normal sequential ring-hit stream -> all counters 0, no rebuild-time line)
		// followed by two real ring-miss gaps (exact rebuild + reduced-evidence rebuild) verifying cumulative/max/last
		// accounting and exact status line formatting.
		BEGIN_TEST_METHOD_ATTRIBUTE(D10SceneRunRebuildTimingIsCountedAndShownInStatus)
			TEST_METHOD_ATTRIBUTE(L"Category", L"Cpu")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(D10SceneRunRebuildTimingIsCountedAndShownInStatus)
		{
			auto FindLineContaining = [](const std::string& text, const char* needle) -> std::string {
				size_t pos = 0;
				while (pos < text.size())
				{
					const size_t eol = text.find('\n', pos);
					const std::string line = (eol == std::string::npos) ? text.substr(pos) : text.substr(pos, eol - pos);
					if (line.find(needle) != std::string::npos)
						return line;
					if (eol == std::string::npos)
						break;
					pos = eol + 1;
				}
				return std::string();
			};

			Rig rig(Lane("on"), 40);
			ShapeASlowFade(rig);
			rig.ResyncApply("on");
			rig.Life(RE::Lifecycle::GpuCreated);

			auto StepFrame = [&](int64_t s, bool classify) {
				rig.Ingress(s);
				if (s == 1)
					rig.Deliver(1);
				rig.Deliver(uint64_t(s) + 1);
				rig.Begin(s);
				if (classify && s >= 2)
				{
					Vpv::Skeleton::SceneFacts f;
					Assert::IsTrue(rig.sk.SceneFactsOf(rig.Host(), uint64_t(s), f), L"SceneFactsOf must succeed on delivered frame");
				}
				rig.After(s);
			};

			RE::Context ctx;
			ctx.host = rig.Host();

			// 1. Negative control: normal sequential ring-hit stream (frames 1..15 classified in order).
			for (int64_t s = 1; s <= 15; ++s)
				StepFrame(s, true);

			const Vpv::Skeleton::SceneCounters c0 = rig.sk.SceneCountersOf(rig.Host());
			Assert::AreEqual(uint64_t(0), c0.fadeRunRebuilt, L"Negative control: sequential ring-hit stream has 0 exact rebuilds");
			Assert::AreEqual(uint64_t(0), c0.fadeRunReduced, L"Negative control: sequential ring-hit stream has 0 reduced rebuilds");
			Assert::AreEqual(uint64_t(0), c0.fadeRunRebuildUsTotal, L"Negative control: 0 rebuilds must yield 0 total rebuild us");
			Assert::AreEqual(uint64_t(0), c0.fadeRunRebuildUsMax, L"Negative control: 0 rebuilds must yield 0 max rebuild us");
			Assert::AreEqual(uint64_t(0), c0.fadeRunRebuildUsLast, L"Negative control: 0 rebuilds must yield 0 last rebuild us");
			Assert::AreEqual(uint64_t(0), c0.published + c0.notReady, L"no store: only this test classified (gaps below are real)");

			std::string status0;
			Assert::IsTrue(rig.sk.OnCommand(ctx, "skeleton", "status", status0), L"skeleton status command must succeed");
			Assert::IsTrue(status0.find("rebuild time mean ") == std::string::npos,
				L"Negative control: sequential stream status must NOT contain 'rebuild time mean '");
			Assert::IsTrue(status0.find("Skeleton scene: U7 fade run missing for ") == std::string::npos,
				L"Negative control: sequential stream status must NOT contain fade-run-missing line");

			// 2. Positive case 1: 3-frame gap (16..18 skipped) -> frame 19 triggers a real ring-miss rebuild.
			//    Advance one sequential frame (20) so Refresh updates cached status text with zero extra rebuilds.
			for (int64_t s = 16; s <= 18; ++s)
				StepFrame(s, false);
			StepFrame(19, true);
			StepFrame(20, true);

			const Vpv::Skeleton::SceneCounters c1 = rig.sk.SceneCountersOf(rig.Host());
			Assert::AreEqual(uint64_t(1), c1.fadeRunRebuilt + c1.fadeRunReduced, L"first ring-miss gap must trigger exactly 1 rebuild (0 extra on sequential frame 20)");
			Assert::AreEqual(c1.fadeRunRebuildUsLast, c1.fadeRunRebuildUsTotal, L"after 1 rebuild, total us must equal last us");
			Assert::AreEqual(c1.fadeRunRebuildUsLast, c1.fadeRunRebuildUsMax, L"after 1 rebuild, max us must equal last us");

			std::string status1;
			Assert::IsTrue(rig.sk.OnCommand(ctx, "skeleton", "status", status1), L"skeleton status command must succeed after gap 1");
			const std::string line1 = FindLineContaining(status1, "Skeleton scene: U7 fade run missing for ");
			Assert::IsFalse(line1.empty(), L"status must include the 'Skeleton scene: U7 fade run missing for ' line after rebuild");
			Assert::IsTrue(line1.find("Skeleton scene: U7 fade run missing for 1 frame(s): ") != std::string::npos,
				L"status line must report 1 missing frame after gap 1");
			Assert::IsTrue(line1.find("rebuild time mean ") != std::string::npos,
				L"status line must report 'rebuild time mean '");
			Assert::IsTrue(line1.find(", max ") != std::string::npos,
				L"status line must report ', max '");
			Assert::IsTrue(line1.find(", last ") != std::string::npos,
				L"status line must report ', last '");

			// 3. Positive case 2: 10-frame gap (21..30 skipped, > 7-pair ring depth) -> frame 31 triggers a second rebuild
			// with reduced evidence; advance sequential frame 32 so Refresh updates cached status text with zero extra rebuilds.
			for (int64_t s = 21; s <= 30; ++s)
				StepFrame(s, false);
			StepFrame(31, true);
			StepFrame(32, true);

			const Vpv::Skeleton::SceneCounters c2 = rig.sk.SceneCountersOf(rig.Host());
			Assert::AreEqual(uint64_t(2), c2.fadeRunRebuilt + c2.fadeRunReduced, L"second ring-miss gap must increment rebuild count to 2 (0 extra on sequential frame 32)");
			Assert::IsTrue(c2.fadeRunReduced >= uint64_t(1), L"10-frame gap beyond 7-pair ring must record reduced-evidence rebuild");
			Assert::AreEqual(c1.fadeRunRebuildUsTotal + c2.fadeRunRebuildUsLast, c2.fadeRunRebuildUsTotal,
				L"cumulative rebuild total us must equal previous total + last rebuild us");
			Assert::IsTrue(c2.fadeRunRebuildUsMax >= c2.fadeRunRebuildUsLast,
				L"max rebuild us must be >= last rebuild us");
			Assert::IsTrue(c2.fadeRunRebuildUsMax >= c1.fadeRunRebuildUsMax,
				L"max rebuild us must never decrease");
			Assert::IsTrue(c2.fadeRunRebuildUsTotal >= c2.fadeRunRebuildUsMax,
				L"cumulative rebuild total us must be >= max single rebuild us");
			Assert::IsTrue(c2.fadeRunRebuildUsTotal > uint64_t(0),
				L"7-pair ring-miss rebuild must record non-zero elapsed microseconds (catches broken/zeroed timer)");
			Assert::IsTrue(c2.fadeRunRebuildUsMax > uint64_t(0),
				L"7-pair ring-miss rebuild must record non-zero max microseconds");

			std::string status2;
			Assert::IsTrue(rig.sk.OnCommand(ctx, "skeleton", "status", status2), L"skeleton status command must succeed after gap 2");
			const std::string line2 = FindLineContaining(status2, "Skeleton scene: U7 fade run missing for ");
			Assert::IsTrue(line2.find("Skeleton scene: U7 fade run missing for 2 frame(s): ") != std::string::npos,
				L"status line must report 2 missing frames after gap 2");
			Assert::IsTrue(line2.find("rebuild time mean ") != std::string::npos &&
				line2.find(", max ") != std::string::npos &&
				line2.find(", last ") != std::string::npos,
				L"status line after gap 2 must retain mean/max/last rebuild timing fields");
		}

		// W23-Phase4 D6: A provisional cut verdict withdrawn on confirmation by the fade-in-from-black veto
		// increments SceneCounters::provisionalCutWithdrawn and appears on the Ctrl+Alt+D "Skeleton scene:" status line.
		// Includes in-test negative control: a genuine hard cut from black into a constant-brightness panning shot
		// stays confirmed with provisionalCutWithdrawn == 0.
		BEGIN_TEST_METHOD_ATTRIBUTE(ProvisionalCutWithdrawnByFadeInVetoIsCountedAndShown)
			TEST_METHOD_ATTRIBUTE(L"Category", L"Cpu")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(ProvisionalCutWithdrawnByFadeInVetoIsCountedAndShown)
		{
			auto FillPanFrame = [](Rig& rig, uint64_t seq, double gain, double shiftPx) {
				uint16_t* dst = rig.store.data() + size_t(seq % rig.count) * kW * kH;
				for (uint32_t y = 0; y < kH; ++y)
					for (uint32_t x = 0; x < kW; ++x)
					{
						if (gain <= 0.0)
						{
							dst[size_t(y) * kW + x] = 64u;
						}
						else
						{
							const double sx = double(x) + shiftPx;
							const double sy = double(y);
							const double pat = 0.50
								+ 0.25 * std::sin(sx * 0.35 + sy * 0.15)
								+ 0.18 * std::cos(sx * 0.19 - sy * 0.27);
							const double v = 64.0 + gain * pat * 760.0;
							dst[size_t(y) * kW + x] = static_cast<uint16_t>((std::min)(1023.0, (std::max)(0.0, std::floor(v + 0.5))));
						}
					}
			};

			// 1. Positive case: 8 black frames -> 10-frame fade-in from black with simultaneous pan -> 4 panning frames.
			//    Deliver each frame only when it becomes current so SceneFactsOf(9) is guaranteed provisional before 10 arrives.
			{
				Rig rig(Lane("on"), 32);
				for (uint64_t s = 1; s <= 8; ++s)
					FillPanFrame(rig, s, 0.0, 0.0);
				for (uint64_t s = 9; s <= 18; ++s)
					FillPanFrame(rig, s, double(s - 8) / 10.0, 2.0 * double(s - 8));
				for (uint64_t s = 19; s <= 22; ++s)
					FillPanFrame(rig, s, 1.0, 2.0 * double(s - 8));

				rig.ResyncApply("on");
				rig.Life(RE::Lifecycle::GpuCreated);

				bool frame9ProvisionalCut = false;
				Vpv::Skeleton::SceneFacts f9After;
				bool haveF9After = false;
				for (int64_t s = 1; s <= 22; ++s)
				{
					rig.Ingress(s);
					rig.Deliver(uint64_t(s));
					rig.Begin(s);
					if (s >= 2)
					{
						Vpv::Skeleton::SceneFacts f;
						Assert::IsTrue(rig.sk.SceneFactsOf(rig.Host(), uint64_t(s), f), L"SceneFactsOf must succeed on frame");
						if (s == 9)
						{
							Assert::IsFalse(f.confirmed, L"frame 9 must be provisional before frame 10 is delivered");
							frame9ProvisionalCut = f.cut;
						}
						else if (s == 10)
						{
							haveF9After = rig.sk.SceneFactsOf(rig.Host(), 9u, f9After);
						}
					}
					rig.After(s);
				}
				Assert::IsTrue(frame9ProvisionalCut, L"precondition: frame 9 emergence from black is provisionally flagged as a cut");
				Assert::IsTrue(haveF9After, L"frame 9 must still be readable right after frame 10 confirms it");
				Assert::IsTrue(f9After.confirmed && !f9After.cut,
					L"frame 9 confirmed verdict must be vetoed (!cut) once frame 10 confirms the brightening fade-in");

				const Vpv::Skeleton::SceneCounters sc = rig.sk.SceneCountersOf(rig.Host());
				Assert::IsTrue(sc.provisionalCutWithdrawn >= uint64_t(1),
					L"withdrawn provisional cut on pan fade-in from black must increment provisionalCutWithdrawn");

				RE::Context ctx;
				ctx.host = rig.Host();
				std::string status;
				Assert::IsTrue(rig.sk.OnCommand(ctx, "skeleton", "status", status));
				Assert::IsTrue(status.find("provisional cut(s) withdrawn on confirmation") != std::string::npos,
					L"status line must surface 'provisional cut(s) withdrawn on confirmation'");
				Assert::IsTrue(status.find("1 provisional cut(s) withdrawn on confirmation") != std::string::npos,
					L"status line must show non-zero withdrawal count after fade-in veto");
			}

			// 2. Negative control: 8 black frames -> hard cut into constant-brightness panning shot (gain = 0.20).
			//    Frame 9 is provisionally a cut AND stays confirmed as a cut -> provisionalCutWithdrawn == 0.
			{
				Rig rig(Lane("on"), 32);
				for (uint64_t s = 1; s <= 8; ++s)
					FillPanFrame(rig, s, 0.0, 0.0);
				for (uint64_t s = 9; s <= 18; ++s)
					FillPanFrame(rig, s, 0.20, 2.0 * double(s - 8));

				rig.ResyncApply("on");
				rig.Life(RE::Lifecycle::GpuCreated);

				bool frame9ProvisionalCut = false;
				Vpv::Skeleton::SceneFacts f9After;
				bool haveF9After = false;
				for (int64_t s = 1; s <= 18; ++s)
				{
					rig.Ingress(s);
					rig.Deliver(uint64_t(s));
					rig.Begin(s);
					if (s >= 2)
					{
						Vpv::Skeleton::SceneFacts f;
						Assert::IsTrue(rig.sk.SceneFactsOf(rig.Host(), uint64_t(s), f));
						if (s == 9)
						{
							Assert::IsFalse(f.confirmed, L"Negative control: frame 9 is provisional before frame 10 arrives");
							frame9ProvisionalCut = f.cut;
						}
						else if (s == 10)
						{
							haveF9After = rig.sk.SceneFactsOf(rig.Host(), 9u, f9After);
						}
					}
					rig.After(s);
				}
				Assert::IsTrue(frame9ProvisionalCut, L"Negative control: hard cut from black into pan is provisionally a cut");
				Assert::IsTrue(haveF9After, L"Negative control: frame 9 must still be readable right after frame 10 confirms it");
				Assert::IsTrue(f9After.confirmed && f9After.cut,
					L"Negative control: frame 9 hard cut from black into pan must remain a confirmed cut");

				const Vpv::Skeleton::SceneCounters sc = rig.sk.SceneCountersOf(rig.Host());
				Assert::AreEqual(uint64_t(0), sc.provisionalCutWithdrawn,
					L"Negative control: confirmed hard cut from black into pan must keep provisionalCutWithdrawn == 0");

				RE::Context ctx;
				ctx.host = rig.Host();
				std::string status;
				Assert::IsTrue(rig.sk.OnCommand(ctx, "skeleton", "status", status));
				Assert::IsTrue(status.find("0 provisional cut(s) withdrawn on confirmation") != std::string::npos,
					L"Negative control: status line must report 0 provisional cuts withdrawn");
			}
		}

		// Prod MED-1 regression: per-host renderTid isolates two no-lane hosts initialized on distinct render
		// threads and survives off-thread Shutdown of hostA without locking out hostB's render thread or allowing
		// the Shutdown thread to trigger PlanNoLaneGpu on hostB.
		BEGIN_TEST_METHOD_ATTRIBUTE(PerHostRenderTidIsolatesTwoHostsAndSurvivesOffThreadShutdown)
			TEST_METHOD_ATTRIBUTE(L"Category", L"Cpu")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(PerHostRenderTidIsolatesTwoHostsAndSurvivesOffThreadShutdown)
		{
			struct VramUsersGuard
			{
				VramUsersGuard() { VpSkeleton::VramUsers::Instance().ResetForTesting(); }
				~VramUsersGuard() { VpSkeleton::VramUsers::Instance().ResetForTesting(); }
			} vramGuard;

			const unsigned long long faultsBefore = SkeletonModule::ContainedFaults();
			SkeletonModule sk([](ConfigFile&) { return false; });   // analysis_lane off (no-lane memo path)
			VpSkeleton::CardProfile ample;
			ample.vramBudgetBytes = 1ull << 30;
			ample.vramUsageBytes = 0;
			sk.SetCardForTesting(ample);

			VpSkeleton::OwnerFill gpuFill;
			gpuFill.reserveBytes = 1u << 20;
			gpuFill.fill = [](const VpSkeleton::OwnerFillRequest&, VpSkeleton::SkeletonMemo&, VpSkeleton::OwnerFillOutput& out) {
				static int tex = 0;
				out.gpu.resource = &tex;
				out.gpu.bytes = 1u << 20;
				return true;
			};
			Assert::IsTrue(
				sk.RegisterOwnerFill(VpSkeleton::kP10GrainSplit, VpSkeleton::AcquireForm::Gpu, "repair/grain_split", gpuFill)
					== VpSkeleton::SkeletonMemo::Registration::Registered,
				L"P10 GPU owner fill registered");

			int hostA = 0;
			int hostB = 0;
			pl_gpu_t fakeGpuA{};
			pl_gpu_t fakeGpuB{};

			auto sendLifecycle = [&sk](const void* host, RE::Lifecycle kind, pl_gpu gpu) {
				RE::LifecycleFacts f;
				f.host = host;
				f.kind = kind;
				f.gpu = gpu;
				sk.OnLifecycle(f);
			};

			// Keep all three threads alive simultaneously behind explicit startup barriers and RAII join guards
			// so Windows cannot reuse a thread ID across phases and early assertion failures never leak threads.
			struct StepThread
			{
				std::atomic<bool> ready{ false };
				std::atomic<uint32_t> tid{ 0 };
				std::atomic<int> cmd{ 0 };
				std::atomic<int> done{ 0 };
				std::thread t;

				explicit StepThread(std::function<void(int)> fn)
				{
					t = std::thread([this, fn]() mutable {
						tid.store(static_cast<uint32_t>(GetCurrentThreadId()), std::memory_order_release);
						ready.store(true, std::memory_order_release);
						for (;;)
						{
							const int c = cmd.load(std::memory_order_acquire);
							if (c < 0)
								return;
							if (c == 0)
							{
								std::this_thread::yield();
								continue;
							}
							cmd.store(0, std::memory_order_release);
							fn(c);
							done.store(c, std::memory_order_release);
						}
					});
					while (!ready.load(std::memory_order_acquire))
						std::this_thread::yield();
				}

				~StepThread()
				{
					cmd.store(-1, std::memory_order_release);
					if (t.joinable())
						t.join();
				}

				void RunStep(int step)
				{
					cmd.store(step, std::memory_order_release);
					while (done.load(std::memory_order_acquire) != step)
						std::this_thread::yield();
				}
			};

			Vpv::Skeleton::FrameMemo fmA_onA;
			Vpv::Skeleton::FrameMemo fmB_onA;
			VpSkeleton::AcquireResult rB_onA;
			Vpv::Skeleton::FrameMemo fmB_onB;
			VpSkeleton::AcquireResult rB_onB;
			Vpv::Skeleton::FrameMemo fmB_onC;
			Vpv::Skeleton::FrameMemo fmB_onB_tight;
			Vpv::Skeleton::FrameMemo fmB_onB_restored;

			StepThread threadA([&](int step) {
				if (step == 1)
				{
					sendLifecycle(&hostA, RE::Lifecycle::GpuCreated, &fakeGpuA);
				}
				else if (step == 2)
				{
					fmA_onA = sk.MemoFor(&hostA, 1);
					fmB_onA = sk.MemoFor(&hostB, 1);   // negative control: threadA is wrong thread for hostB
					if (fmB_onA.Valid())
						rB_onA = fmB_onA.memo->Acquire(VpSkeleton::kP10GrainSplit, 1, VpSkeleton::AcquireForm::Gpu);
				}
			});

			StepThread threadB([&](int step) {
				if (step == 1)
				{
					sendLifecycle(&hostB, RE::Lifecycle::GpuCreated, &fakeGpuB);
				}
				else if (step == 2)
				{
					fmB_onB = sk.MemoFor(&hostB, 1);
					if (fmB_onB.Valid())
						rB_onB = fmB_onB.memo->Acquire(VpSkeleton::kP10GrainSplit, 1, VpSkeleton::AcquireForm::Gpu);
				}
				else if (step == 3)
				{
					fmB_onB_tight = sk.MemoFor(&hostB, 2);
				}
				else if (step == 4)
				{
					fmB_onB_restored = sk.MemoFor(&hostB, 3);
				}
				else if (step == 5)
				{
					sendLifecycle(&hostB, RE::Lifecycle::Shutdown, nullptr);
				}
			});

			StepThread threadC([&](int step) {
				if (step == 1)
				{
					sendLifecycle(&hostA, RE::Lifecycle::Shutdown, nullptr);
				}
				else if (step == 2)
				{
					fmB_onC = sk.MemoFor(&hostB, 2);
				}
			});

			const uint32_t tidA = threadA.tid.load(std::memory_order_acquire);
			const uint32_t tidB = threadB.tid.load(std::memory_order_acquire);
			const uint32_t tidC = threadC.tid.load(std::memory_order_acquire);
			Assert::IsTrue(tidA != tidB && tidB != tidC && tidA != tidC, L"three distinct Win32 thread IDs");

			// Step 1 & 2: hostA gets GpuCreated on threadA, then hostB gets GpuCreated on threadB.
			threadA.RunStep(1);
			threadB.RunStep(1);

			// Step 3: threadA plans hostA (must succeed despite hostB's GpuCreated on threadB) and probes hostB (negative control).
			threadA.RunStep(2);
			Assert::IsTrue(fmA_onA.Valid() && fmA_onA.memo->GpuGranted(),
				L"threadA (hostA's render thread) must grant GPU on hostA even after hostB registered on threadB");
			Assert::IsTrue(fmB_onA.Valid(), L"threadA lazily created hostB memo");
			Assert::IsFalse(fmB_onA.memo->GpuGranted(),
				L"negative control: threadA (wrong thread for hostB) MUST NOT grant GPU on hostB");
			Assert::IsTrue(
				rB_onA.status == VpSkeleton::AcquireStatus::Unavailable && rB_onA.cause == VpSkeleton::AcquireCause::NoGpuDispatcher,
				L"negative control: off-thread MemoFor on hostB must leave GPU dispatcher ungranted");

			// Step 4: threadB plans hostB and succeeds.
			threadB.RunStep(2);
			Assert::IsTrue(fmB_onB.Valid() && fmB_onB.memo->GpuGranted(),
				L"threadB (hostB's render thread) must grant GPU on hostB");
			Assert::IsTrue(rB_onB.status == VpSkeleton::AcquireStatus::FilledAtRead && rB_onB.gpu.Valid(),
				L"hostB GPU fill succeeds on threadB");

			// Step 5: threadC runs Shutdown(&hostA). Then force a pending replan (tight card + second GPU owner fill)
			// to prove threadC STILL cannot trigger PlanNoLaneGpu(&hostB) while threadB STILL owns hostB.
			threadC.RunStep(1);
			Assert::IsNull(sk.MemoOf(&hostA), L"Shutdown cleaned up hostA");

			VpSkeleton::CardProfile tight;
			tight.vramBudgetBytes = 1ull << 20;
			tight.vramUsageBytes = 1ull << 20;   // 0 free VRAM -> replan must revoke GpuGranted
			sk.SetCardForTesting(tight);

			VpSkeleton::OwnerFill secondGpuFill;
			secondGpuFill.reserveBytes = 2u << 20;
			secondGpuFill.fill = gpuFill.fill;
			Assert::IsTrue(
				sk.RegisterOwnerFill(VpSkeleton::kP1HalfLuma, VpSkeleton::AcquireForm::Gpu, "input/source_analysis/lane", secondGpuFill)
					== VpSkeleton::SkeletonMemo::Registration::Registered,
				L"second GPU fill registered to move hostB's plannedNeed");

			threadC.RunStep(2);
			Assert::IsTrue(fmB_onC.Valid() && fmB_onC.memo->GpuGranted(),
				L"negative control: threadC (after hostA Shutdown) MUST NOT trigger PlanNoLaneGpu(&hostB)");

			threadB.RunStep(3);
			Assert::IsTrue(fmB_onB_tight.Valid() && !fmB_onB_tight.memo->GpuGranted(),
				L"hostB still recognizes threadB as its render thread and replans against tight card");

			sk.SetCardForTesting(ample);
			Assert::IsTrue(sk.UnregisterOwnerFill(VpSkeleton::kP1HalfLuma, VpSkeleton::AcquireForm::Gpu, "input/source_analysis/lane"));
			threadB.RunStep(4);
			Assert::IsTrue(fmB_onB_restored.Valid() && fmB_onB_restored.memo->GpuGranted(),
				L"threadB continues to grant GPU on hostB after ample card restored");

			threadB.RunStep(5);
			Assert::IsNull(sk.MemoOf(&hostB), L"Shutdown cleaned up hostB");
			Assert::AreEqual(faultsBefore, SkeletonModule::ContainedFaults(), L"zero contained faults");
		}

		// Part 2 AC#4 (N2 + Skeleton MUST-2/3/8): live worker threads join cleanly on source-size rebuild, explicit Shutdown,
		// and direct ~SkeletonModule() without GpuReleased; lane-bound memo GPU presence follows attached device vs lane
		// dispatcher (MUST-3); concurrent ahead vs render SceneOf preserves monotonic confirmation with a pre-successor
		// provisional negative control (MUST-8).
		BEGIN_TEST_METHOD_ATTRIBUTE(BundleDestructorAndRebuildJoinWorkersBeforeUnbind)
			TEST_METHOD_ATTRIBUTE(L"Category", L"Cpu")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(BundleDestructorAndRebuildJoinWorkersBeforeUnbind)
		{
			struct VramUsersGuard
			{
				VramUsersGuard() { VpSkeleton::VramUsers::Instance().ResetForTesting(); }
				~VramUsersGuard() { VpSkeleton::VramUsers::Instance().ResetForTesting(); }
			} vramGuard;

			const unsigned long long faultsBefore = SkeletonModule::ContainedFaults();

			// ---- 1. N2 / MUST-2: live worker threads across dims rebuild, Shutdown, and direct ~SkeletonModule() ----
			{
				Rig rig(Lane("on"));
				rig.sk.SetWorkerThreadsForTesting(true);
				rig.ResyncApply("on");
				rig.Life(RE::Lifecycle::GpuCreated);
				for (int64_t s = 1; s <= 6; ++s)
					rig.Frame(s);

				Assert::IsTrue(rig.sk.WorkersRunning(rig.Host()), L"MUST-2: lane worker threads actively running");
				Assert::AreEqual<uint32_t>(kW, rig.sk.DimsOf(rig.Host()).width);
				Assert::AreEqual<uint32_t>(kH, rig.sk.DimsOf(rig.Host()).height);
				const std::shared_ptr<VpSkeleton::SkeletonMemo> heldMemo = rig.sk.MemoFor(rig.Host(), 6).memo;
				Assert::IsTrue(heldMemo != nullptr && heldMemo->LaneBound(), L"memo lane-bound before rebuild");
				const uint64_t bindsBefore = heldMemo->LaneBinds();

				// Trigger a source-size rebuild (new dims 96x64) while workers are running.
				constexpr uint32_t kW2 = 96;
				constexpr uint32_t kH2 = 64;
				std::vector<uint16_t> bigPlane(size_t(kW2) * kH2);
				for (uint32_t y = 0; y < kH2; ++y)
					for (uint32_t x = 0; x < kW2; ++x)
						bigPlane[size_t(y) * kW2 + x] = static_cast<uint16_t>(64u + ((x * 7u + y * 11u) % 700u));

				auto makeView2 = [&](uint64_t seq) {
					SourceFrameView v;
					v.seq = seq;
					v.luma = bigPlane.data();
					v.width = kW2;
					v.height = kH2;
					v.strideSamples = kW2;
					v.msbShift = 6;
					return v;
				};

				for (int64_t s = 7; s <= 10; ++s)
				{
					rig.Ingress(s);
					Tap::Deliver(rig.Host(), makeView2(uint64_t(s)));
					Tap::Deliver(rig.Host(), makeView2(uint64_t(s) + 1u));
					rig.Begin(s);
					rig.After(s);
				}

				Assert::AreEqual<uint32_t>(kW2, rig.sk.DimsOf(rig.Host()).width, L"rebuild updated width to 96");
				Assert::AreEqual<uint32_t>(kH2, rig.sk.DimsOf(rig.Host()).height, L"rebuild updated height to 64");
				Assert::IsTrue(rig.sk.WorkersRunning(rig.Host()), L"rebuilt bundle started its worker threads");
				Assert::IsTrue(heldMemo->LaneBound() && heldMemo->LaneBinds() > bindsBefore,
					L"dims rebuild joined old workers and rebound memo to new lane");
				std::vector<uint8_t> expectedHalf2(VpLaneProducts::HalfLumaBytes(Dims{ kW2, kH2 }));
				Assert::IsTrue(VpLaneProducts::ComputeHalfLuma(makeView2(10), expectedHalf2.data(), expectedHalf2.size()));
				Assert::IsTrue(rig.Bytes(VpSkeleton::kP1HalfLuma, 10) == expectedHalf2,
					L"rebuilt lane produces bit-identical P1 for new dims");

				// Explicit Shutdown while workers are running joins workers and unbinds memo.
				rig.Life(RE::Lifecycle::Shutdown);
				Assert::IsFalse(rig.sk.WorkersRunning(rig.Host()), L"Shutdown joined worker threads");
				Assert::IsNull(rig.sk.LaneOf(rig.Host()), L"Shutdown dropped lane bundle");
				Assert::IsNull(rig.sk.MemoOf(rig.Host()), L"Shutdown removed host memo");
				Assert::IsFalse(heldMemo->LaneBound(), L"Shutdown cleanly unbound memo from old lane");
			}

			// Direct ~SkeletonModule() destruction WITHOUT explicit GpuReleased / Shutdown while workers are running.
			{
				FakeRenderer directHost;
				ResyncExtension directResync;
				TempCfg directCfg(Lane("on"));
				struct TapCleanupGuard
				{
					const void* host;
					~TapCleanupGuard()
					{
						Tap::SetStore(host, nullptr);
						Tap::Unregister(host);
					}
				} tapGuard{ &directHost };

				std::vector<uint16_t> directPixels(size_t(kW) * kH, 320u);
				std::shared_ptr<VpSkeleton::SkeletonMemo> directMemo;
				{
					SkeletonModule skDirect(directCfg.Loader());
					skDirect.SetWorkerThreadsForTesting(true);
					{
						RE::LifecycleFacts rf;
						rf.host = &directHost;
						rf.kind = RE::Lifecycle::SettingsApplied;
						rf.subject = "on";
						directResync.OnLifecycle(rf);
					}
					{
						RE::LifecycleFacts gf;
						gf.host = &directHost;
						gf.kind = RE::Lifecycle::GpuCreated;
						skDirect.OnLifecycle(gf);
					}
					for (int64_t s = 1; s <= 6; ++s)
					{
						RE::IngressContext ic;
						ic.host = &directHost;
						ic.seq = s;
						ic.queueGeneration = 1;
						ic.stateKey = 0x1000;
						ic.formatKey = 7;
						ic.frameCounter = s;
						ic.timestamp = s * 1000;
						ic.ticksPerSecond = 24000;
						ic.measuredRateHz = 24.0;
						directResync.OnIngress(ic);

						auto deliverSeq = [&](uint64_t seq) {
							SourceFrameView v;
							v.seq = seq;
							v.luma = directPixels.data();
							v.width = kW;
							v.height = kH;
							v.strideSamples = kW;
							v.msbShift = 6;
							Tap::Deliver(&directHost, v);
						};
						if (s == 1)
							deliverSeq(1);
						deliverSeq(uint64_t(s) + 1u);

						RE::FrameContext fc;
						fc.host = &directHost;
						fc.phase = RE::FramePhase::Begin;
						fc.seq = s;
						directResync.OnFrame(fc);
						Assert::IsFalse(skDirect.OnFrame(fc));
						fc.phase = RE::FramePhase::AfterRender;
						Assert::IsFalse(skDirect.OnFrame(fc));
					}
					Assert::IsTrue(skDirect.WorkersRunning(&directHost),
						L"workers actively running immediately prior to direct ~SkeletonModule()");
					directMemo = skDirect.MemoFor(&directHost, 6).memo;
					Assert::IsTrue(directMemo != nullptr && directMemo->LaneBound(),
						L"memo lane-bound immediately prior to direct ~SkeletonModule()");
				}
				Assert::IsFalse(directMemo->LaneBound(),
					L"direct ~SkeletonModule() joined workers and unbound memo without explicit GpuReleased");
				Assert::IsFalse(Tap::Wants(&directHost),
					L"direct ~SkeletonModule() unregistered SourceFrameTap cleanly");
			}

			// ---- 2. MUST-3: lane-bound memo GPU presence follows attached device vs lane dispatcher ----
			{
				Rig rig(Lane("shadow"));
				pl_gpu_t fakeGpu{};
				rig.ResyncApply("on");
				rig.Life(RE::Lifecycle::GpuCreated);   // null pl_gpu: shadow lane has no lane GPU dispatcher and no attached device
				for (int64_t s = 1; s <= 3; ++s)
					rig.Frame(s);

				VpSkeleton::SkeletonMemo* memo = rig.sk.MemoOf(rig.Host());
				Assert::IsTrue(memo != nullptr && memo->LaneBound(), L"shadow lane bound to memo");
				RegisterGpuOwnerFill(rig);

				// Negative control: no attached device AND no lane GPU dispatcher -> NoGpuDispatcher.
				Assert::IsFalse(memo->HasGpuDevice(), L"MUST-3 negative control: no owner GPU device attached");
				Assert::IsNull(rig.sk.GpuDispatcherFor(rig.Host()), L"MUST-3 negative control: shadow lane has no lane GPU dispatcher");
				Assert::IsTrue(GpuOwnerFillCause(rig, 3) == VpSkeleton::AcquireCause::NoGpuDispatcher,
					L"MUST-3 negative control: absent device + absent lane dispatcher returns NoGpuDispatcher");

				// Attach owner GPU device after bind -> GPU presence becomes true despite shadow lane having no lane dispatcher.
				GpuCreatedWithDevice(rig, &fakeGpu);
				Assert::IsTrue(memo->LaneBound() && memo->HasGpuDevice(), L"device attached to bound shadow lane");
				Assert::IsNull(rig.sk.GpuDispatcherFor(rig.Host()), L"shadow lane still has no lane GPU dispatcher");
				Assert::IsTrue(GpuOwnerFillCause(rig, 3) != VpSkeleton::AcquireCause::NoGpuDispatcher,
					L"MUST-3: attached owner GPU device grants GPU presence on shadow lane without a lane dispatcher");

				// Detach device via null-pl_gpu GpuCreated -> falls back to lane dispatcher (none) -> NoGpuDispatcher.
				RE::LifecycleFacts detach;
				detach.host = rig.Host();
				detach.kind = RE::Lifecycle::GpuCreated;
				detach.gpu = nullptr;
				rig.sk.OnLifecycle(detach);
				rig.Frame(4);
				Assert::IsFalse(memo->HasGpuDevice(), L"owner GPU device detached");
				Assert::IsTrue(GpuOwnerFillCause(rig, 4) == VpSkeleton::AcquireCause::NoGpuDispatcher,
					L"MUST-3: detaching owner GPU device drops presence back to NoGpuDispatcher when no lane dispatcher exists");
			}

			// ---- 3. MUST-8: concurrent SceneOf preserves monotonic confirmation + pre-successor provisional negative control ----
			{
				Rig rig(Lane("on"), 128);
				FrameAnalysis::Store store(FrameAnalysis::RingDepth(4, 4));
				Tap::SetStore(rig.Host(), &store);
				SceneStoreGuard storeGuard(rig.Host());
				FeedAheadWindow(rig, 8);
				rig.ResyncApply("on");
				rig.Life(RE::Lifecycle::GpuCreated);
				for (int64_t s = 1; s <= 3; ++s)
					rig.Frame(s);

				// In-test negative control: frame 4 sealed BEFORE frame 5 is delivered/sealed MUST be provisional.
				rig.Ingress(4);
				store.Begin(SceneTestId(4), FrameAnalysis::kScene);
				rig.Begin(4);
				rig.After(4);
				const Vpv::Skeleton::SceneAnswer preSucc = rig.sk.SceneOf(rig.Host(), 4);
				Assert::IsTrue(preSucc.known, L"MUST-8 negative control: frame 4 scene is known");
				Assert::IsFalse(preSucc.confirmed,
					L"MUST-8 negative control: pre-successor query of frame 4 MUST be provisional before frame 5 seals");

				// Delivering and sealing frame 5 transitions frame 4 to confirmed.
				rig.Deliver(5);
				rig.Ingress(5);
				store.Begin(SceneTestId(5), FrameAnalysis::kScene);
				rig.Begin(5);
				rig.After(5);
				const Vpv::Skeleton::SceneAnswer postSucc = rig.sk.SceneOf(rig.Host(), 4);
				Assert::IsTrue(postSucc.known && postSucc.confirmed,
					L"frame 4 transitions from provisional to confirmed once frame 5 seals");

				// Concurrent SceneOf(s) on worker thread vs ahead SceneOf(s + 1) on render thread across 48 iterations:
				// once confirmed, frame s MUST NEVER downgrade to provisional.
				constexpr int kIter = 48;
				uint64_t delivered = 5;
				int confirmedGroups = 0;
				for (int i = 0; i < kIter; ++i)
				{
					const int64_t s = 7 + i;
					while (delivered < uint64_t(s) + 1u)
						rig.Deliver(++delivered);
					rig.Ingress(s - 1);
					rig.Begin(s - 1);
					rig.After(s - 1);
					store.Begin(SceneTestId(uint64_t(s)), FrameAnalysis::kScene);
					WalkP4(rig, uint64_t(s));
					WalkP4(rig, uint64_t(s) + 1u);

					std::atomic<bool> workerReady{ false };
					std::atomic<bool> go{ false };
					Vpv::Skeleton::SceneAnswer workerAns;
					struct JoinGuard
					{
						std::atomic<bool>& goFlag;
						std::thread t;
						~JoinGuard()
						{
							goFlag.store(true, std::memory_order_release);
							if (t.joinable())
								t.join();
						}
					} worker{ go, std::thread([&]() {
						workerReady.store(true, std::memory_order_release);
						while (!go.load(std::memory_order_acquire))
							std::this_thread::yield();
						workerAns = rig.sk.SceneOf(rig.Host(), uint64_t(s));
					}) };
					while (!workerReady.load(std::memory_order_acquire))
						std::this_thread::yield();
					go.store(true, std::memory_order_release);
					const Vpv::Skeleton::SceneAnswer aheadAns = rig.sk.SceneOf(rig.Host(), uint64_t(s) + 1u);
					if (worker.t.joinable())
						worker.t.join();

					Assert::IsTrue(aheadAns.known,
						(L"MUST-8 iter " + std::to_wstring(i) + L": render-thread ahead SceneOf(s+1) answered").c_str());
					FrameAnalysis::Scene group;
					if (store.PeekScene(uint64_t(s), group))
					{
						Assert::IsTrue(group.confirmed,
							(L"MUST-8 iter " + std::to_wstring(i) + L": confirmed frame s group must never downgrade to provisional").c_str());
						const Vpv::Skeleton::SceneAnswer monotonic = rig.sk.SceneOf(rig.Host(), uint64_t(s));
						Assert::IsTrue(monotonic.known && monotonic.confirmed,
							(L"MUST-8 iter " + std::to_wstring(i) + L": subsequent SceneOf(s) preserves monotonic confirmation").c_str());
						++confirmedGroups;
					}
				}
				Assert::IsTrue(confirmedGroups >= kIter / 2,
					L"MUST-8: concurrent SceneOf exercised real store confirmation rewrites (not vacuous)");
			}

			Assert::AreEqual(faultsBefore, SkeletonModule::ContainedFaults(),
				L"zero contained faults across N2 / MUST-2 / MUST-3 / MUST-8");
		}

		// K#9 Part A: per-host QueueShape isolates two hosts sharing one SkeletonModule across SettingsApplied,
		// QueueDepth, CollectAhead, Begin, DeriveMemoWindow, and UnpublishRenderAhead.
		BEGIN_TEST_METHOD_ATTRIBUTE(TwoHostsHaveIndependentQueueShapesAndKeepWindows)
			TEST_METHOD_ATTRIBUTE(L"Category", L"Cpu")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(TwoHostsHaveIndependentQueueShapesAndKeepWindows)
		{
			struct VramUsersGuard
			{
				VramUsersGuard() { VpSkeleton::VramUsers::Instance().ResetForTesting(); }
				~VramUsersGuard() { VpSkeleton::VramUsers::Instance().ResetForTesting(); }
			} vramGuard;

			TempCfg cfg(Lane("on"));
			std::atomic<uint64_t> clockUs{ 1000u };
			SkeletonModule sk(cfg.Loader(), [&clockUs]() { return clockUs.fetch_add(10u); });
			ResyncExtension resync;
			FakeRenderer hostA;
			FakeRenderer hostB;

			struct HostsCleanupGuard
			{
				SkeletonModule& sk;
				const void* a;
				const void* b;
				~HostsCleanupGuard()
				{
					RE::LifecycleFacts f;
					f.kind = RE::Lifecycle::Shutdown;
					f.host = a;
					sk.OnLifecycle(f);
					f.host = b;
					sk.OnLifecycle(f);
					Tap::SetStore(a, nullptr);
					Tap::Unregister(a);
					Tap::SetStore(b, nullptr);
					Tap::Unregister(b);
				}
			} cleanup{ sk, &hostA, &hostB };

			std::vector<uint16_t> pixels(size_t(kW) * kH, 420u);
			auto deliver = [&](const void* h, uint64_t seq) {
				SourceFrameView v;
				v.seq = seq;
				v.luma = pixels.data();
				v.width = kW;
				v.height = kH;
				v.strideSamples = kW;
				v.msbShift = 6;
				Tap::Deliver(h, v);
			};

			auto resyncOn = [&](const void* h) {
				RE::LifecycleFacts f;
				f.host = h;
				f.kind = RE::Lifecycle::SettingsApplied;
				f.subject = "on";
				resync.OnLifecycle(f);
			};
			auto sendLife = [&](const void* h, RE::Lifecycle kind, int code, double value) {
				RE::LifecycleFacts f;
				f.host = h;
				f.kind = kind;
				f.code = code;
				f.value = value;
				sk.OnLifecycle(f);
			};
			auto sendCollect = [&](const void* h, size_t queueSize) {
				RE::QueueView qv;
				qv.size = queueSize;
				RE::FrameContext fc;
				fc.host = h;
				fc.phase = RE::FramePhase::CollectAhead;
				fc.phaseData = &qv;
				Assert::IsFalse(sk.OnFrame(fc));
			};
			auto driveFrame = [&](const void* h, int64_t seq, double renderAheadVal) {
				clockUs.fetch_add(40000u);
				sendLife(h, RE::Lifecycle::QueueDepth, 0, renderAheadVal);
				RE::IngressContext ic;
				ic.host = h;
				ic.seq = seq;
				ic.queueGeneration = 1;
				ic.stateKey = 0x1000;
				ic.formatKey = 7;
				ic.frameCounter = seq;
				ic.timestamp = seq * 1000;
				ic.ticksPerSecond = 24000;
				ic.measuredRateHz = 24.0;
				resync.OnIngress(ic);
				if (seq == 1)
					deliver(h, 1u);
				deliver(h, uint64_t(seq) + 1u);
				RE::FrameContext fc;
				fc.host = h;
				fc.seq = seq;
				fc.phase = RE::FramePhase::Begin;
				resync.OnFrame(fc);
				Assert::IsFalse(sk.OnFrame(fc));
				fc.phase = RE::FramePhase::AfterRender;
				Assert::IsFalse(sk.OnFrame(fc));
			};

			resyncOn(&hostA);
			resyncOn(&hostB);
			sendLife(&hostA, RE::Lifecycle::GpuCreated, 0, 0.0);
			sendLife(&hostB, RE::Lifecycle::GpuCreated, 0, 0.0);

			// Deliver distinct queue terms per host through real lifecycle / frame callbacks.
			sendLife(&hostA, RE::Lifecycle::SettingsApplied, 0, 2.0);
			sendLife(&hostA, RE::Lifecycle::QueueDepth, 0, 1.0);
			sendCollect(&hostA, 2u);

			sendLife(&hostB, RE::Lifecycle::SettingsApplied, 0, 10.0);
			sendLife(&hostB, RE::Lifecycle::QueueDepth, 0, 4.0);
			sendCollect(&hostB, 6u);

			const SkeletonModule::QueueShape qA = sk.CurrentQueue(&hostA);
			const SkeletonModule::QueueShape qB = sk.CurrentQueue(&hostB);
			Assert::AreEqual<int>(1, int(qA.renderAhead), L"hostA renderAhead == 1");
			Assert::AreEqual<int>(2, int(qA.laneLead), L"hostA laneLead == 2");
			Assert::AreEqual<int>(2, int(qA.lookAheadWalk), L"hostA lookAheadWalk == 2");
			Assert::AreEqual<int>(SkeletonModule::QueueShape::kAll, int(qA.wired));
			Assert::AreEqual<int>(4, int(qB.renderAhead), L"hostB renderAhead == 4");
			Assert::AreEqual<int>(6, int(qB.laneLead), L"hostB laneLead == 6");
			Assert::AreEqual<int>(10, int(qB.lookAheadWalk), L"hostB lookAheadWalk == 10");
			Assert::AreEqual<int>(SkeletonModule::QueueShape::kAll, int(qB.wired));

			for (int64_t s = 1; s <= 6; ++s)
			{
				driveFrame(&hostA, s, 1.0);
				driveFrame(&hostB, s, 4.0);
			}

			const SkeletonModule::QueueShape usedA = sk.QueueInUseOf(&hostA).used;
			const SkeletonModule::QueueShape usedB = sk.QueueInUseOf(&hostB).used;
			Assert::IsTrue(usedA != usedB, L"QueueInUseOf differs between hostA and hostB");
			Assert::IsTrue(usedA.renderAhead == 1 && usedA.laneLead == 2 && usedA.lookAheadWalk == 2,
				L"hostA QueueInUse matches hostA terms");
			Assert::IsTrue(usedB.renderAhead == 4 && usedB.laneLead == 6 && usedB.lookAheadWalk == 10,
				L"hostB QueueInUse matches hostB terms");

			// In-test negative control: hostA's QueueFramesFor is strictly smaller than hostB's (distinct windows).
			const unsigned framesA = VpSkeleton::QueueFramesFor(usedA.renderAhead, usedA.laneLead, usedA.lookAheadWalk);
			const unsigned framesB = VpSkeleton::QueueFramesFor(usedB.renderAhead, usedB.laneLead, usedB.lookAheadWalk);
			Assert::IsTrue(framesA < framesB, L"negative control: hostA queue frames strictly smaller than hostB");
			Assert::AreEqual(5u, framesA);
			Assert::AreEqual(20u, framesB);

			// Verify lane graph settings are strictly independent per host.
			Assert::IsNotNull(sk.LaneOf(&hostA));
			Assert::IsNotNull(sk.LaneOf(&hostB));
			const VpSkeleton::GraphSettings gsA = sk.LaneOf(&hostA)->State()->graph.settings;
			const VpSkeleton::GraphSettings gsB = sk.LaneOf(&hostB)->State()->graph.settings;
			Assert::IsTrue(gsA.renderAhead == 1 && gsA.laneLead == 2 && gsA.lookAheadWalk == 2,
				L"hostA lane graph settings isolated");
			Assert::IsTrue(gsB.renderAhead == 4 && gsB.laneLead == 6 && gsB.lookAheadWalk == 10,
				L"hostB lane graph settings isolated");

			// Verify WindowBoard keep windows (back / ahead) are strictly independent per host.
			const VpSkeleton::QueueWindowBoard* boardA = sk.WindowBoardOf(&hostA);
			const VpSkeleton::QueueWindowBoard* boardB = sk.WindowBoardOf(&hostB);
			Assert::IsNotNull(boardA);
			Assert::IsNotNull(boardB);
			VpSkeleton::QueueWindow wA, wB;
			Assert::IsTrue(boardA->Read(&wA));
			Assert::IsTrue(boardB->Read(&wB));
			Assert::AreEqual<uint16_t>(1u, wA.back, L"hostA back == 1");
			Assert::AreEqual<uint16_t>(4u, wB.back, L"hostB back == 4");
			Assert::AreEqual<uint16_t>(4u, wA.ahead, L"hostA ahead == L(2) + W(2) == 4");
			Assert::AreEqual<uint16_t>(16u, wB.ahead, L"hostB ahead == L(6) + W(10) == 16");
			Assert::IsTrue(wA.back != wB.back && wA.ahead != wB.ahead, L"independent keep windows");

			// UnpublishRenderAhead(&hostA) unwires ONLY hostA; hostB's kRenderAhead stays wired with renderAhead == 4.
			sk.UnpublishRenderAhead(&hostA);
			const SkeletonModule::QueueShape qAAfter = sk.CurrentQueue(&hostA);
			const SkeletonModule::QueueShape qBAfter = sk.CurrentQueue(&hostB);
			Assert::AreEqual<int>(0, int(qAAfter.wired & SkeletonModule::QueueShape::kRenderAhead),
				L"UnpublishRenderAhead(&hostA) unwires hostA's kRenderAhead");
			Assert::AreEqual<int>(0, int(qAAfter.renderAhead));
			Assert::IsTrue((qBAfter.wired & SkeletonModule::QueueShape::kRenderAhead) != 0,
				L"hostB's kRenderAhead remains wired after UnpublishRenderAhead(&hostA)");
			Assert::AreEqual<int>(4, int(qBAfter.renderAhead));
		}

		// K#9 Part B: per-host ConfirmedCutHostReader isolates confirmed-cut vetoes across two hosts with
		// separate enabled FrameAnalysis::Stores while neither host engages skeleton_scene_veto.
		BEGIN_TEST_METHOD_ATTRIBUTE(TwoHostsConfirmedCutVetoIsIsolatedPerHost)
			TEST_METHOD_ATTRIBUTE(L"Category", L"Cpu")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(TwoHostsConfirmedCutVetoIsIsolatedPerHost)
		{
			Rig rigA(Lane("on"));
			ShapeTwoStillShots(rigA);   // confirmed cut at kCutFrame (12) on hostA

			FakeRenderer hostB;
			struct HostBShutdownGuard
			{
				SkeletonModule& sk;
				const void* host;
				~HostBShutdownGuard()
				{
					RE::LifecycleFacts f;
					f.host = host;
					f.kind = RE::Lifecycle::Shutdown;
					sk.OnLifecycle(f);
					Tap::Unregister(host);
				}
			} hostBGuard{ rigA.sk, &hostB };

			FrameAnalysis::Store storeA(FrameAnalysis::RingDepth(4, 4));
			FrameAnalysis::Store storeB(FrameAnalysis::RingDepth(4, 4));
			Tap::SetStore(rigA.Host(), &storeA);
			SceneStoreGuard guardA(rigA.Host());
			Tap::SetStore(&hostB, &storeB);
			SceneStoreGuard guardB(&hostB);

			// Setup resync & GpuCreated on both hosts.
			rigA.ResyncApply("on");
			rigA.Life(RE::Lifecycle::GpuCreated);
			{
				RE::LifecycleFacts rf;
				rf.host = &hostB;
				rf.kind = RE::Lifecycle::SettingsApplied;
				rf.subject = "on";
				rigA.resync.OnLifecycle(rf);
				RE::LifecycleFacts gf;
				gf.host = &hostB;
				gf.kind = RE::Lifecycle::GpuCreated;
				rigA.sk.OnLifecycle(gf);
			}

			// Flat/still picture for hostB (zero cuts across every frame).
			std::vector<uint16_t> flatB(size_t(kW) * kH, 400u);
			auto deliverB = [&](uint64_t seq) {
				SourceFrameView v;
				v.seq = seq;
				v.luma = flatB.data();
				v.width = kW;
				v.height = kH;
				v.strideSamples = kW;
				v.msbShift = 6;
				Tap::Deliver(&hostB, v);
			};
			auto frameB = [&](int64_t seq) {
				RE::IngressContext ic;
				ic.host = &hostB;
				ic.seq = seq;
				ic.queueGeneration = 1;
				ic.stateKey = 0x1000;
				ic.formatKey = 7;
				ic.frameCounter = seq;
				ic.timestamp = seq * 1000;
				ic.ticksPerSecond = 24000;
				ic.measuredRateHz = 24.0;
				rigA.resync.OnIngress(ic);
				if (seq == 1)
					deliverB(1u);
				deliverB(uint64_t(seq) + 1u);
				rigA.base += 40000u;
				RE::FrameContext fc;
				fc.host = &hostB;
				fc.seq = seq;
				fc.phase = RE::FramePhase::Begin;
				rigA.resync.OnFrame(fc);
				Assert::IsFalse(rigA.sk.OnFrame(fc));
				fc.phase = RE::FramePhase::AfterRender;
				Assert::IsFalse(rigA.sk.OnFrame(fc));
			};

			// Drive frames 1 .. kCutFrame + 2 on both hosts (opening kCutFrame & kCutFrame + 1 records in both stores).
			for (int64_t s = 1; s <= int64_t(kCutFrame) + 2; ++s)
			{
				if (s == int64_t(kCutFrame) || s == int64_t(kCutFrame) + 1)
				{
					storeA.Begin(SceneTestId(uint64_t(s)), FrameAnalysis::kScene);
					storeB.Begin(SceneTestId(uint64_t(s)), FrameAnalysis::kScene);
				}
				rigA.Frame(s);
				frameB(s);
			}

			FrameAnalysis::Scene sceneA, sceneB;
			Assert::IsTrue(storeA.PeekScene(kCutFrame, sceneA) && sceneA.confirmed && sceneA.cutHere,
				L"precondition: hostA has a confirmed cut at kCutFrame");
			Assert::IsTrue(storeB.PeekScene(kCutFrame, sceneB) && sceneB.confirmed && !sceneB.cutHere,
				L"precondition: hostB has a confirmed non-cut at kCutFrame");

			// In-test negative control: 2 stores registered -> Tap::SingleStore() is nullptr and legacy seam fails to veto hostA.
			Assert::IsNull(Tap::SingleStore(), L"negative control: 2 stores registered -> Tap::SingleStore() is nullptr");
			const MotionValidity::State::ConfirmedCutFn legacySeam =
				MotionValidity::State::ConfirmedCutReader().load(std::memory_order_acquire);
			Assert::IsNotNull(legacySeam);
			Assert::IsFalse(legacySeam(kCutFrame),
				L"negative control: legacy single-store ConfirmedCutReader returns false with 2 hosts (would miss hostA cut)");
			Assert::IsNotNull(MotionValidity::State::ConfirmedCutHostReader().load(std::memory_order_acquire),
				L"K#9 Part B: per-host ConfirmedCutHostReader is bound");

			// Per-host MotionValidity::State isolation.
			MotionValidity::State mvA, mvB;
			mvA.SetHost(rigA.Host());
			mvB.SetHost(&hostB);
			Assert::IsTrue(mvA.IsConfirmedCut(kCutFrame), L"hostA sees confirmed cut at kCutFrame");
			Assert::IsFalse(mvB.IsConfirmedCut(kCutFrame), L"hostB sees NO cut at kCutFrame");
			Assert::IsFalse(mvA.PairValidAt(kCutFrame - 1u, kCutFrame),
				L"K#9 Part B: hostA pair across confirmed cut is vetoed");
			Assert::AreEqual<uint64_t>(1u, mvA.ConfirmedCutRefusals(), L"hostA recorded 1 confirmed-cut refusal");
			Assert::IsTrue(mvB.PairValidAt(kCutFrame - 1u, kCutFrame),
				L"K#9 Part B: hostB pair is NOT falsely vetoed by hostA's cut");
			Assert::AreEqual<uint64_t>(0u, mvB.ConfirmedCutRefusals(), L"hostB recorded 0 confirmed-cut refusals");

			// Neither host engages skeleton_scene_veto fallback with per-host ConfirmedCutHostReader active.
			Assert::IsFalse(HasId(rigA.Lines(RE::StatusKind::Fallbacks), "skeleton_scene_veto"),
				L"hostA does not engage skeleton_scene_veto with 2 hosts");
			std::vector<std::string> fbB;
			RE::Context ctxB;
			ctxB.host = &hostB;
			rigA.sk.OnStatus(ctxB, RE::StatusKind::Fallbacks, fbB, nullptr, nullptr);
			Assert::IsFalse(HasId(fbB, "skeleton_scene_veto"),
				L"hostB does not engage skeleton_scene_veto with 2 hosts");
		}

		// K#10: SkeletonMemo::HasAheadDemands() (including laneOwners_ shadowing), steady-state QueueWindow
		// short-circuit parity, immediate SceneVetoOffCode change detection on non-due frames, and zero-alloc
		// steady-state queries under Debug CRT alloc hook.
		BEGIN_TEST_METHOD_ATTRIBUTE(SteadyStateNoAheadDemandsShortCircuitAndZeroAllocRefresh)
			TEST_METHOD_ATTRIBUTE(L"Category", L"Cpu")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(SteadyStateNoAheadDemandsShortCircuitAndZeroAllocRefresh)
		{
			// ---- 1. Direct SkeletonMemo::HasAheadDemands() + laneOwners_ shadowing negative control ----
			{
				VpSkeleton::SkeletonMemo memo;
				Assert::IsFalse(memo.HasAheadDemands(), L"negative control: fresh memo has no ahead demands");

				VpSkeleton::OwnerFill modFill;
				modFill.aheadDepth = 3u;
				modFill.fill = [](const VpSkeleton::OwnerFillRequest&, VpSkeleton::SkeletonMemo&, VpSkeleton::OwnerFillOutput&) {
					return false;
				};
				Assert::IsTrue(
					memo.RegisterOwnerFill(VpSkeleton::kP10GrainSplit, VpSkeleton::AcquireForm::Cpu, "repair/grain_split", modFill)
						== VpSkeleton::SkeletonMemo::Registration::Registered);
				Assert::IsTrue(memo.HasAheadDemands(), L"module owner fill with aheadDepth=3 sets HasAheadDemands() true");

				// Bind a lane shadowing (kP10GrainSplit, Cpu) with aheadDepth = 0 -> must shadow owners_ and yield false.
				VpSkeleton::LaneSlotSource src;
				src.token = &memo;
				std::map<std::pair<VpSkeleton::ProductId, VpSkeleton::AcquireForm>, VpSkeleton::OwnerFill> laneFills;
				VpSkeleton::OwnerFill shadowFill;
				shadowFill.aheadDepth = 0u;
				laneFills[std::make_pair(VpSkeleton::kP10GrainSplit, VpSkeleton::AcquireForm::Cpu)] = shadowFill;
				memo.BindLane(src, laneFills);
				Assert::IsFalse(memo.HasAheadDemands(),
					L"K#10: laneOwners_ shadowing owners_ with aheadDepth=0 makes HasAheadDemands() false");

				Assert::IsTrue(memo.UnbindLane(&memo));
				Assert::IsTrue(memo.HasAheadDemands(), L"unbinding lane restores underlying module aheadDepth=3");

				Assert::IsTrue(memo.UnregisterOwnerFill(VpSkeleton::kP10GrainSplit, VpSkeleton::AcquireForm::Cpu, "repair/grain_split"));
				Assert::IsFalse(memo.HasAheadDemands(), L"unregistering owner fill returns HasAheadDemands() to false");
			}

			// ---- 2. Steady-state Rig past kEarlyRefreshFrames: QueueWindow parity + non-due SceneVetoOffCode toggle ----
			Rig rig(Lane("on"), 64);
			FrameAnalysis::Store store(FrameAnalysis::RingDepth(4, 4));
			Tap::SetStore(rig.Host(), &store);
			SceneStoreGuard storeGuard(rig.Host());
			rig.ResyncApply("on");
			rig.Life(RE::Lifecycle::GpuCreated);

			for (int64_t s = 1; s <= 18; ++s)
				rig.Frame(s);

			VpSkeleton::SkeletonMemo* memo = rig.sk.MemoOf(rig.Host());
			Assert::IsNotNull(memo);
			Assert::IsFalse(memo->HasAheadDemands(), L"steady-state lane without ahead consumers has HasAheadDemands() == false");

			const VpSkeleton::QueueWindowBoard* board = rig.sk.WindowBoardOf(rig.Host());
			Assert::IsNotNull(board);
			VpSkeleton::QueueWindow w;
			Assert::IsTrue(board->Read(&w), L"window board published");
			Assert::IsTrue((w.wired & VpSkeleton::QueueWindowTerms::kAheadNeed) != 0u,
				L"K#10: short-circuit preserves kAheadNeed wired bit (exact parity with SetAheadConsumers(empty))");
			Assert::AreEqual<uint16_t>(0u, w.aheadCompute, L"aheadCompute == 0 with no ahead demands");
			Assert::IsTrue(w.aheadBinding == VpSkeleton::AheadBinding::NoConsumer, L"aheadBinding == NoConsumer");
			Assert::IsFalse(HasId(rig.Lines(RE::StatusKind::Fallbacks), "skeleton_scene_veto"),
				L"enabled single store has scene veto active");

			// Negative control: toggling store off/on on non-kRefreshEvery frames (19, 20) immediately triggers Refresh.
			store.SetEnabled(false);
			rig.Frame(19);
			Assert::IsTrue(HasId(rig.Lines(RE::StatusKind::Fallbacks), "skeleton_scene_veto"),
				L"negative control: disabling store immediately engages skeleton_scene_veto on non-due frame 19");

			store.SetEnabled(true);
			rig.Frame(20);
			Assert::IsFalse(HasId(rig.Lines(RE::StatusKind::Fallbacks), "skeleton_scene_veto"),
				L"negative control: re-enabling store immediately releases skeleton_scene_veto on non-due frame 20");

			for (int64_t s = 21; s <= 32; ++s)
				rig.Frame(s);

			// ---- 3. Debug CRT alloc-hook check on zero-alloc steady-state query primitives ----
#if defined(_DEBUG)
			struct DebugAllocCounter
			{
				static std::atomic<uint64_t>& Count()
				{
					static std::atomic<uint64_t> c{ 0 };
					return c;
				}
				static std::atomic<bool>& Active()
				{
					static std::atomic<bool> a{ false };
					return a;
				}
				static std::thread::id& TargetTid()
				{
					static std::thread::id tid;
					return tid;
				}
				static int __cdecl Hook(int allocType, void*, size_t, int blockType, long, const unsigned char*, int)
				{
					if (blockType != _CRT_BLOCK
						&& (allocType == _HOOK_ALLOC || allocType == _HOOK_REALLOC)
						&& Active().load(std::memory_order_acquire)
						&& std::this_thread::get_id() == TargetTid())
					{
						Count().fetch_add(1, std::memory_order_relaxed);
					}
					return 1;
				}
				_CRT_ALLOC_HOOK prev = nullptr;
				DebugAllocCounter()
				{
					TargetTid() = std::this_thread::get_id();
					Count().store(0u, std::memory_order_relaxed);
					Active().store(true, std::memory_order_release);
					prev = _CrtSetAllocHook(&Hook);
				}
				~DebugAllocCounter()
				{
					Active().store(false, std::memory_order_release);
					_CrtSetAllocHook(prev);
				}
			} allocCounter;

			// Negative control: verify the CRT hook observes a >SSO heap allocation.
			{
				std::string probe(64u, 'x');
				Assert::AreEqual(size_t(64), probe.size());
				Assert::IsTrue(DebugAllocCounter::Count().load(std::memory_order_relaxed) >= 1u,
					L"negative control: CRT alloc hook observes 64-byte std::string allocation");
			}
			DebugAllocCounter::Count().store(0u, std::memory_order_relaxed);

			bool anyAhead = false;
			uint32_t wiredSum = 0;
			for (int i = 0; i < 1000; ++i)
			{
				anyAhead = anyAhead || memo->HasAheadDemands();
				const SkeletonModule::QueueShape qs = rig.sk.CurrentQueue(rig.Host());
				VpSkeleton::QueueWindow curW;
				if (board->Read(&curW))
					wiredSum += uint32_t(curW.wired) + uint32_t(qs.wired);
			}
			const uint64_t queryAllocs = DebugAllocCounter::Count().load(std::memory_order_relaxed);
			DebugAllocCounter::Active().store(false, std::memory_order_release);

			Assert::AreEqual(uint64_t(0), queryAllocs,
				L"K#10: 1,000 steady-state HasAheadDemands / CurrentQueue / WindowBoard::Read queries incur ZERO heap allocations");
			Assert::IsFalse(anyAhead);
			Assert::IsTrue(wiredSum > 0u);
#endif
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
			struct   // P11b: the walk reads these through impl.vectorEngine; the flat names below are references so the test bodies read unchanged
			{
				std::atomic<int> motionAheadDepth{ 0 };   // batch B fold: motionAheadBackward is retired (the walk no longer reads it)
				std::shared_ptr<FeedProducer> motionProducer = std::make_shared<FeedProducer>();
			} vectorEngine;
			std::atomic<int>& motionAheadDepth = vectorEngine.motionAheadDepth;
			std::shared_ptr<FeedProducer>& motionProducer = vectorEngine.motionProducer;
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

				// 20 queued ahead: the motion lead stops at kMaxLead (the ahead-compute ceiling, OD 11-14; was the pair ring's 13); the
				// lane is fed the real 20 and caps only to its own ring-slot budget (shown by the skeleton when it binds).
				u = Feed(rig, impl, 4, 20, seq);
				Assert::AreEqual(MotionAhead::kMaxLead, impl.motionProducer->lead);
				Assert::AreEqual(int(VpSkeleton::kAheadCeiling), impl.motionProducer->lead, L"the motion lead is held to THE ceiling");
				Assert::AreEqual(20, int(u.fed.laneLead));
				Assert::AreEqual(int((std::min)(20u, SkeletonModule::LaneLeadCap(0))), int(u.used.laneLead));
				Assert::IsTrue(int(u.used.laneLead) > impl.motionProducer->lead,
					L"OD 11-14: the queue term L still counts the queued frames; only the compute depth is capped");

				// No producer (motion off): the feed still runs.
				impl.motionProducer.reset();
				u = Feed(rig, impl, 4, 3, seq);
				Assert::AreEqual(3, int(u.used.laneLead), L"the lane lead does not depend on the motion producer");
				RE::ResetForTesting();
			}
		}
	};

	// P11a-0 pin (refs/110 s2.1): the S8 state anchor Vpv::HostState is DATA ONLY. Until the P11a moves land
	// (s2.2-2.6) every sub-struct is empty; each move that adds fields to one MUST replace its is_empty line
	// below with assertions on the new fields (rule 28: never delete the pin, tighten it).
	TEST_CLASS(HostStateAnchorPinTests)
	{
	public:
		TEST_METHOD(AnchorHoldsOnlyTheMovedFieldsAndTheOthersAreEmptyUntilFieldsLand)
		{
			// Plain-data pins (P11a-0 pins, tightened to what holds): the anchor as a whole is no longer standard-layout nor
			// trivially destructible (s2.2 put std::string/std::mutex/shared_ptr in it; s2.6 put SourceResolutionMonitor::Monitor,
			// whose data members have mixed access, in SourceAnalysis): WAIVER-P11-ANCHOR-PINS (coordinator D3):
			// whole-anchor is_standard_layout and is_trivially_destructible dropped, sizeof and per-sub-struct pins stay. Every sub-struct that
			// can still be standard-layout stays pinned as such.
			static_assert(std::is_standard_layout<Vpv::HostState::Motion>::value, "motion is plain data");
			static_assert(std::is_standard_layout<Vpv::HostState::Led>::value, "led is plain data");
			static_assert(std::is_standard_layout<Vpv::HostState::Calibration>::value, "calibration is plain data");
			static_assert(std::is_standard_layout<Vpv::HostState::Status>::value, "status is plain data");
			static_assert(std::is_trivially_destructible<Vpv::HostState::Calibration>::value, "calibration holds no resources");
			static_assert(std::is_trivially_destructible<Vpv::HostState::Status>::value, "status holds no resources");
			// Hidden-member pin (the old 'exactly five bytes' sizeof pin, tightened with the fields): the anchor is exactly its
			// five sub-structs, so any further member or base changes its size.
			struct FiveMembers { Vpv::HostState::Motion m; Vpv::HostState::Led l; Vpv::HostState::Calibration c; Vpv::HostState::SourceAnalysis s; Vpv::HostState::Status st; };
			static_assert(sizeof(Vpv::HostState) == sizeof(FiveMembers), "the anchor holds exactly motion, led, calibration, sourceAnalysis, status");
			// P11a-f (s2.2), P11a-d (s2.4), P11a-e (s2.5), P11a-f (s2.6) landed fields; status holds no state (s2.3).
			Assert::IsTrue(std::is_empty<Vpv::HostState::Status>::value);
			Assert::IsFalse(std::is_empty<Vpv::HostState::Calibration>::value);
			Assert::IsFalse(std::is_empty<Vpv::HostState::SourceAnalysis>::value);
			Assert::IsFalse(std::is_empty<Vpv::HostState::Led::SdrLift>::value);
			Assert::IsFalse(std::is_empty<Vpv::HostState::Motion::Intent>::value);
			Assert::IsFalse(std::is_empty<Vpv::HostState::Led::Intent>::value);
			// The moved fields keep their old types.
			Assert::IsTrue(std::is_same<decltype(Vpv::HostState::Motion::Intent::intentInterpreter),
				const std::shared_ptr<IntentInterpreter::Observer>>::value);
			Assert::IsTrue(std::is_same<decltype(Vpv::HostState::Motion::Intent::blackShareEase), MotionIntent::ShotEase>::value);
			Assert::IsTrue(std::is_same<decltype(Vpv::HostState::Motion::Intent::blackShareApplied), double>::value);
			Assert::IsTrue(std::is_same<decltype(Vpv::HostState::Motion::Intent::blackShareShot), uint64_t>::value);
			Assert::IsTrue(std::is_same<decltype(Vpv::HostState::Motion::Intent::blackShareValid), bool>::value);
			Assert::IsTrue(std::is_same<decltype(Vpv::HostState::Motion::Intent::blackShareTime),
				std::chrono::steady_clock::time_point>::value);
			Assert::IsTrue(std::is_same<decltype(Vpv::HostState::Led::Intent::intentMutex), std::mutex>::value);
			Assert::IsTrue(std::is_same<decltype(Vpv::HostState::Led::Intent::ambientSettings), AmbientAdapt::Settings>::value);
			Assert::IsTrue(std::is_same<decltype(Vpv::HostState::Led::Intent::ambientFilter), AmbientAdapt::Filter>::value);
			Assert::IsTrue(std::is_same<decltype(Vpv::HostState::Led::Intent::ambientAppliedBlackNits), double>::value);
			Assert::IsTrue(std::is_same<decltype(Vpv::HostState::Led::Intent::blackIntentStatus), BlackIntentStatus>::value);
			Assert::IsTrue(std::is_same<decltype(Vpv::HostState::Led::Intent::displayIntentStatus), DisplayIntentStatus>::value);
			Assert::IsTrue(std::is_same<decltype(Vpv::HostState::Led::Intent::intentBlackPublished), BlackIntentMode>::value);
			Assert::IsTrue(std::is_same<decltype(Vpv::HostState::Led::Intent::intentDisplayPublished), bool>::value);
			Assert::IsTrue(std::is_same<decltype(Vpv::HostState::Led::Intent::ambientApply), IntentInterpreter::AmbientApplyState>::value);
			Assert::IsTrue(std::is_same<decltype(Vpv::HostState::Led::Intent::ambientCutsSeen), uint64_t>::value);
			Assert::IsTrue(std::is_same<decltype(Vpv::HostState::Led::Intent::ledIntentLimitsSent), IntentInterpreter::LedIntentLimits>::value);
			Assert::IsTrue(std::is_same<decltype(Vpv::HostState::Led::Intent::blackIntentLogged), int>::value);
			Assert::IsTrue(std::is_same<decltype(Vpv::HostState::Led::Intent::displayIntentLogged), std::string>::value);
			Assert::IsTrue(std::is_same<decltype(Vpv::HostState::Led::SdrLift::ambientSdrLift), bool>::value);
			Assert::IsTrue(std::is_same<decltype(Vpv::HostState::Led::SdrLift::ambientSdrContrast), float>::value);
			Assert::IsTrue(std::is_same<decltype(Vpv::HostState::Led::SdrLift::ambientSdrBrightness), float>::value);
			Assert::IsTrue(std::is_same<decltype(Vpv::HostState::Led::SdrLift::ambientSdrLogged), std::string>::value);
			Assert::IsTrue(std::is_same<decltype(Vpv::HostState::Calibration::calibrationStatusAvailable), bool>::value);
			Assert::IsTrue(std::is_same<decltype(Vpv::HostState::Calibration::lastRenderedTargetTransfer), LibplaceboOutput::SdrTransfer>::value);
			Assert::IsTrue(std::is_same<decltype(Vpv::HostState::Calibration::calibrationLutAttached), bool>::value);
			Assert::IsTrue(std::is_same<decltype(Vpv::HostState::SourceAnalysis::sourceMonitor), SourceResolutionMonitor::Monitor>::value);
			Assert::IsTrue(std::is_same<decltype(Vpv::HostState::SourceAnalysis::sourceMonitorSequence), uint64_t>::value);
			Assert::IsTrue(std::is_same<decltype(Vpv::HostState::SourceAnalysis::sourceMonitorHistorySet), bool>::value);
			Assert::IsTrue(std::is_same<decltype(Vpv::HostState::SourceAnalysis::sourceMonitorDvLogged), bool>::value);
			// The initial values are the old Impl members' (the anchor is constructed with the Impl).
			Vpv::HostState state;
			Assert::IsTrue(state.motion.intent.intentInterpreter != nullptr);
			Assert::IsFalse(state.motion.intent.blackShareValid);
			Assert::AreEqual(0.0, state.motion.intent.blackShareApplied);
			Assert::AreEqual(uint64_t(0), state.motion.intent.blackShareShot);
			Assert::AreEqual(0.0, state.led.intent.ambientAppliedBlackNits);
			Assert::IsTrue(state.led.intent.intentBlackPublished == BlackIntentMode::Off);
			Assert::IsFalse(state.led.intent.intentDisplayPublished);
			Assert::AreEqual(uint64_t(0), state.led.intent.ambientCutsSeen);
			Assert::AreEqual(-1, state.led.intent.blackIntentLogged);
			Assert::IsTrue(state.led.intent.displayIntentLogged.empty());
			Assert::IsFalse(state.led.sdrLift.ambientSdrLift);
			Assert::AreEqual(1.0f, state.led.sdrLift.ambientSdrContrast);
			Assert::AreEqual(0.0f, state.led.sdrLift.ambientSdrBrightness);
			Assert::IsTrue(state.led.sdrLift.ambientSdrLogged.empty());
			Assert::IsFalse(state.calibration.calibrationStatusAvailable);
			Assert::IsTrue(state.calibration.lastRenderedTargetTransfer == LibplaceboOutput::SdrTransfer::UNKNOWN);
			Assert::IsFalse(state.calibration.calibrationLutAttached);
			Assert::AreEqual(uint64_t(0), state.sourceAnalysis.sourceMonitorSequence);
			Assert::IsFalse(state.sourceAnalysis.sourceMonitorHistorySet);
			Assert::IsFalse(state.sourceAnalysis.sourceMonitorDvLogged);
		}
	};

	// 15c-c8 (coordinator ruling 2026-11-15 13:53 EST, log:831; the TD fill rule's shape): an ENABLED LED drive's demand turns on the P4
	// analysis lane and the frame-analysis store it needs, visibly on Ctrl+I, only while it is registered; nothing turns on without a
	// consumer; never in the Original pathway; the configuration is never changed.
	TEST_CLASS(SkeletonDemandRaiseTests)
	{
	public:
		// The LED drive's declaration (LedDriveModule::DeclareSceneDemand): P4 at lag 0 and lag 1.
		static std::vector<VpSkeleton::ConsumerDecl> LedConsumers()
		{
			VpSkeleton::ConsumerDecl d;
			d.id = 0x0600;
			d.name = "LED look-ahead cut";
			VpSkeleton::AddConsume(d, VpSkeleton::kP4SourceStats, 0);
			VpSkeleton::AddConsume(d, VpSkeleton::kP4SourceStats, 1);
			return std::vector<VpSkeleton::ConsumerDecl>(1, d);
		}

		static Vpv::Skeleton::IExternalDemand* DemandOf(Rig& rig)
		{
			Vpv::Skeleton::IExternalDemand* demand =
				static_cast<Vpv::Skeleton::IExternalDemand*>(rig.sk.Query(Vpv::Skeleton::kExternalDemandQueryId));
			Assert::IsNotNull(demand);
			return demand;
		}

		TEST_METHOD(AnEnabledLedDemandTurnsAnOffLaneAndItsStoreOnVisiblyAndItsWithdrawalTurnsThemOff)
		{
			Rig rig(Lane("off"));
			VpFrameAnalysis::Host store;   // the renderer's store, registered before the module knows the renderer (as at start-up)
			store.Owner(rig.Host()).Configure(false);   // frame_analysis_store off
			rig.ResyncApply("on");
			rig.Life(RE::Lifecycle::GpuCreated);
			rig.Life(RE::Lifecycle::SettingsApplied, 0);
			for (int64_t s = 1; s <= 2; ++s)
				rig.Frame(s);
			Assert::IsNull(rig.sk.LaneOf(rig.Host()), L"analysis_lane off and no consumer: no lane");
			Assert::IsFalse(Has(rig.Lines(RE::StatusKind::Lines), "skeleton="), L"off: no lane line");
			Assert::IsFalse(store.Enabled(), L"frame_analysis_store off and no consumer: no store");

			int led = 0;
			DemandOf(rig)->SetRaisingExternalDemand(rig.Host(), &led, LedConsumers(), {}, "LED drive");
			Assert::AreEqual(std::string("LED drive"), rig.sk.RaiseReasonOf(rig.Host()));
			for (int64_t s = 3; s <= 8; ++s)
				rig.Frame(s);
			VpSkeleton::AnalysisLane* lane = rig.sk.LaneOf(rig.Host());
			Assert::IsNotNull(lane, L"the LED's demand turned the lane on");
			const std::shared_ptr<VpSkeleton::LaneState> laneState = lane->State();
			Assert::IsTrue(laneState != nullptr && laneState->graph.valid);
			Assert::IsTrue(laneState->graph.Has(VpSkeleton::kP4SourceStats), L"the P4 the LED reads is in the graph");
			Assert::IsFalse(laneState->graph.Has(VpSkeleton::kP10GrainSplit) || laneState->graph.Has(VpSkeleton::kP11RegionBlockStats) ||
				laneState->graph.Has(VpSkeleton::kP12LightLevel), L"no consumer of the GPU products: none is made");
			Assert::IsTrue(store.Enabled(), L"and the store that feeds it");
			Assert::IsTrue(store.RaisedByDemand(), L"on only for the demand (the configuration stays off)");
			const std::vector<std::string> ctrlI = rig.Lines(RE::StatusKind::Lines);
			Assert::IsTrue(Has(ctrlI, "skeleton=analysis lane on (demand: LED drive)"), L"Ctrl+I names the consumer");
			Assert::IsTrue(Has(ctrlI, "frame-analysis store on (demand: LED drive)"), L"and the store raised for it");
			const std::string full = rig.Command();
			Assert::IsTrue(full.find("Analysis lane: on (demand: LED drive; analysis_lane = off in the configuration, unchanged)") != std::string::npos);
			Assert::IsTrue(full.find("Frame-analysis store: on (demand: LED drive") != std::string::npos);
			Assert::IsFalse(HasId(rig.Lines(RE::StatusKind::Fallbacks), "skeleton_gpu_device"), L"no GPU products by design, not a missing device");
			Assert::IsFalse(HasId(rig.Lines(RE::StatusKind::Fallbacks), "skeleton_engine_feed"), L"P4 only: no engine feed is started for it");

			DemandOf(rig)->ClearExternalDemand(rig.Host(), &led);   // the drive is disabled (LedDriveModule::Stop)
			rig.Frame(9);
			Assert::IsNull(rig.sk.LaneOf(rig.Host()), L"withdrawn: back to the configured off");
			Assert::IsFalse(Has(rig.Lines(RE::StatusKind::Lines), "skeleton="));
			Assert::IsFalse(store.Enabled(), L"and the store too");
			Assert::IsFalse(Tap::StoreDemanded(rig.Host()));
			Assert::IsTrue(rig.sk.RaiseReasonOf(rig.Host()).empty());
		}

		TEST_METHOD(NoConsumerOrAPlainDeclarationTurnsNothingOn)
		{
			Rig rig(Lane("off"));
			rig.ResyncApply("on");
			rig.Life(RE::Lifecycle::GpuCreated);
			rig.Life(RE::Lifecycle::SettingsApplied, 0);
			int empty = 0, plain = 0;
			DemandOf(rig)->SetRaisingExternalDemand(rig.Host(), &empty, {}, {}, "LED drive");   // a raise with no consumer
			DemandOf(rig)->SetExternalDemand(rig.Host(), &plain, LedConsumers(), {});           // a consumer that does not raise
			for (int64_t s = 1; s <= 6; ++s)
				rig.Frame(s);
			Assert::IsNull(rig.sk.LaneOf(rig.Host()), L"nothing turns on without a raising consumer");
			Assert::IsFalse(Has(rig.Lines(RE::StatusKind::Lines), "skeleton="));
			Assert::IsFalse(Tap::StoreDemanded(rig.Host()), L"no store demand either");
			Assert::IsTrue(rig.sk.RaiseReasonOf(rig.Host()).empty());
			DemandOf(rig)->ClearExternalDemand(rig.Host(), &empty);
			DemandOf(rig)->ClearExternalDemand(rig.Host(), &plain);
		}

		TEST_METHOD(TheOriginalPathwayIsNeverRaised)
		{
			Rig rig(Lane("off"));
			rig.ResyncApply("on");
			rig.Life(RE::Lifecycle::GpuCreated);
			rig.Life(RE::Lifecycle::SettingsApplied, 1);   // the Original pathway
			int led = 0;
			DemandOf(rig)->SetRaisingExternalDemand(rig.Host(), &led, LedConsumers(), {}, "LED drive");
			for (int64_t s = 1; s <= 6; ++s)
				rig.Frame(s);
			Assert::IsNull(rig.sk.LaneOf(rig.Host()), L"Original: no lane, whatever is declared");
			Assert::IsTrue(rig.Lines(RE::StatusKind::Lines).empty(), L"Original: no line");
			Assert::IsFalse(Tap::StoreDemanded(rig.Host()), L"Original: no store demand");
			rig.Life(RE::Lifecycle::GpuCreated);   // a reconcile without the renderer's word keeps the last word (Original)
			rig.Frame(7);
			Assert::IsNull(rig.sk.LaneOf(rig.Host()));
			Assert::IsFalse(Tap::StoreDemanded(rig.Host()));

			rig.Life(RE::Lifecycle::SettingsApplied, 0);   // back to the VP-V pathway: the registered demand raises now
			for (int64_t s = 8; s <= 13; ++s)
				rig.Frame(s);
			Assert::IsNotNull(rig.sk.LaneOf(rig.Host()));
			Assert::IsTrue(Tap::StoreDemanded(rig.Host()));
			rig.Life(RE::Lifecycle::SettingsApplied, 1);   // Original again: dropped at once
			Assert::IsNull(rig.sk.LaneOf(rig.Host()));
			Assert::IsFalse(Tap::StoreDemanded(rig.Host()));
			rig.Frame(14);
			Assert::IsNull(rig.sk.LaneOf(rig.Host()));
			DemandOf(rig)->ClearExternalDemand(rig.Host(), &led);
		}

		TEST_METHOD(AConfiguredLaneKeepsItsModeAndAShadowLaneIsRaisedToOn)
		{
			{
				Rig rig(Lane("on"));
				rig.ResyncApply("on");
				rig.Life(RE::Lifecycle::GpuCreated);
				rig.Life(RE::Lifecycle::SettingsApplied, 0);
				int led = 0;
				DemandOf(rig)->SetRaisingExternalDemand(rig.Host(), &led, LedConsumers(), {}, "LED drive");
				for (int64_t s = 1; s <= 6; ++s)
					rig.Frame(s);
				VpSkeleton::AnalysisLane* lane = rig.sk.LaneOf(rig.Host());
				Assert::IsNotNull(lane);
				Assert::IsTrue(lane->State()->graph.Has(VpSkeleton::kP10GrainSplit), L"configured on: the report is unchanged");
				Assert::IsFalse(Has(rig.Lines(RE::StatusKind::Lines), "(demand: LED drive)"), L"the lane is on by configuration, not raised");
				Assert::IsTrue(Tap::StoreDemanded(rig.Host()), L"the store is still demanded (it may be off in the configuration)");
				DemandOf(rig)->ClearExternalDemand(rig.Host(), &led);
				rig.Frame(7);
				Assert::IsNotNull(rig.sk.LaneOf(rig.Host()), L"withdrawn: the configured lane stays");
				Assert::IsFalse(Tap::StoreDemanded(rig.Host()));
			}
			{
				Rig rig(Lane("shadow"));
				rig.ResyncApply("on");
				rig.Life(RE::Lifecycle::GpuCreated);
				rig.Life(RE::Lifecycle::SettingsApplied, 0);
				int led = 0;
				DemandOf(rig)->SetRaisingExternalDemand(rig.Host(), &led, LedConsumers(), {}, "LED drive");
				for (int64_t s = 1; s <= 6; ++s)
					rig.Frame(s);
				Assert::IsNotNull(rig.sk.LaneOf(rig.Host()));
				Assert::IsFalse(rig.sk.LaneOf(rig.Host())->ShadowOnly(), L"shadow results are not used: raised to on");
				Assert::IsTrue(Has(rig.Lines(RE::StatusKind::Lines), "skeleton=analysis lane on (demand: LED drive)"));
				Assert::IsTrue(rig.Command().find("analysis_lane = shadow in the configuration, unchanged") != std::string::npos);
				DemandOf(rig)->ClearExternalDemand(rig.Host(), &led);
				rig.Frame(7);
				Assert::IsTrue(rig.sk.LaneOf(rig.Host()) == nullptr || rig.sk.LaneOf(rig.Host())->ShadowOnly(), L"withdrawn: shadow again");
			}
		}

		// W23-4p (G-T1): a raising consumer that needs GPU products (P10/P11/P12, e.g. dither_need) demands GPU
		// dispatch even when analysis_lane=off (visibly reporting skeleton_gpu_device without a pl_gpu and adding
		// P10/P11/P12 to the graph), while LED-only P4 raising demand keeps zero GPU demand (in-test negative control).
		BEGIN_TEST_METHOD_ATTRIBUTE(GpuRaisingConsumerDemandsGpuWhileLedOnlyDoesNot)
			TEST_METHOD_ATTRIBUTE(L"Category", L"Cpu")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(GpuRaisingConsumerDemandsGpuWhileLedOnlyDoesNot)
		{
			auto gpuConsumers = []()
			{
				VpSkeleton::ConsumerDecl d;
				d.id = 0x0620;
				d.name = "output.dither (dither need live)";
				VpSkeleton::AddConsume(d, VpSkeleton::kP10GrainSplit, 0);
				VpSkeleton::AddConsume(d, VpSkeleton::kP11RegionBlockStats, 0);
				VpSkeleton::AddConsume(d, VpSkeleton::kP12LightLevel, 0);
				return std::vector<VpSkeleton::ConsumerDecl>(1, d);
			};

			Rig rig(Lane("off"));
			VpFrameAnalysis::Host store;
			store.Owner(rig.Host()).Configure(false);
			rig.ResyncApply("on");
			rig.Life(RE::Lifecycle::GpuCreated);   // null pl_gpu in CPU unit test
			rig.Life(RE::Lifecycle::SettingsApplied, 0);
			int64_t seq = 1;

			// In-test negative control: LED-only P4 raising demand must NOT demand GPU products or skeleton_gpu_device.
			int led = 0;
			DemandOf(rig)->SetRaisingExternalDemand(rig.Host(), &led, LedConsumers(), {}, "LED drive");
			for (int i = 0; i < 4; ++i) rig.Frame(seq++);
			Assert::IsNotNull(rig.sk.LaneOf(rig.Host()));
			Assert::IsFalse(rig.sk.LaneOf(rig.Host())->State()->graph.Has(VpSkeleton::kP10GrainSplit),
				L"Negative control: LED-only raise must not pull P10 into graph");
			Assert::IsFalse(HasId(rig.Lines(RE::StatusKind::Fallbacks), "skeleton_gpu_device"),
				L"Negative control: LED-only raise must not report skeleton_gpu_device");
			DemandOf(rig)->ClearExternalDemand(rig.Host(), &led);
			rig.Frame(seq++);
			Assert::IsNull(rig.sk.LaneOf(rig.Host()), L"LED withdrawal returns off lane to off");

			// GPU raising demand (dither need): raises lane with P10/P11/P12 AND demands GPU dispatcher.
			int dn = 0;
			DemandOf(rig)->SetRaisingExternalDemand(rig.Host(), &dn, gpuConsumers(), {}, "dither need");
			for (int i = 0; i < 4; ++i) rig.Frame(seq++);
			VpSkeleton::AnalysisLane* lane = rig.sk.LaneOf(rig.Host());
			Assert::IsNotNull(lane, L"GPU raising demand must turn on an off lane");
			Assert::AreEqual(std::string("dither need"), rig.sk.RaiseReasonOf(rig.Host()));
			Assert::IsTrue(lane->State()->graph.Has(VpSkeleton::kP10GrainSplit) &&
				lane->State()->graph.Has(VpSkeleton::kP11RegionBlockStats) &&
				lane->State()->graph.Has(VpSkeleton::kP12LightLevel),
				L"GPU raising consumer must place P10/P11/P12 in the lane graph");
			Assert::IsTrue(HasId(rig.Lines(RE::StatusKind::Fallbacks), "skeleton_gpu_device"),
				L"GPU raising demand without pl_gpu must visibly engage skeleton_gpu_device (proving GPU dispatcher was demanded)");

			// Original pathway MUST suppress GPU raising demand completely.
			rig.Life(RE::Lifecycle::SettingsApplied, 1);
			rig.Frame(seq++);
			Assert::IsNull(rig.sk.LaneOf(rig.Host()), L"Original pathway must never raise GPU lane");
			Assert::IsTrue(rig.Lines(RE::StatusKind::Lines).empty());
			rig.Life(RE::Lifecycle::SettingsApplied, 0);

			// Withdrawal tears everything down cleanly.
			DemandOf(rig)->ClearExternalDemand(rig.Host(), &dn);
			rig.Frame(seq++);
			Assert::IsNull(rig.sk.LaneOf(rig.Host()), L"withdrawing GPU raising demand returns lane to configured off");
			Assert::IsFalse(HasId(rig.Lines(RE::StatusKind::Fallbacks), "skeleton_gpu_device"));
		}

		// W23-4p: ReadDitherNeedGpuInputs without a live GPU dispatcher / Entrance fill returns a clean empty snapshot.
		BEGIN_TEST_METHOD_ATTRIBUTE(ReadDitherNeedGpuInputsWithoutDeviceReturnsEmptyCleanly)
			TEST_METHOD_ATTRIBUTE(L"Category", L"Cpu")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(ReadDitherNeedGpuInputsWithoutDeviceReturnsEmptyCleanly)
		{
			Rig rig(Lane("on"));
			rig.ResyncApply("on");
			rig.Life(RE::Lifecycle::GpuCreated);
			rig.Frame(1);
			const Vpv::Skeleton::DitherNeedGpuInputs gi = DemandOf(rig)->ReadDitherNeedGpuInputs(rig.Host(), 1);
			Assert::IsFalse(gi.haveGrain);
			Assert::IsFalse(gi.haveEvidence);
			Assert::IsNull(gi.gsBlocks);
			for (size_t i = 0; i < Vpv::Skeleton::DitherNeedGpuInputs::kPrecursors; ++i)
				Assert::IsNull(gi.precursors[i]);
		}
	};
}
