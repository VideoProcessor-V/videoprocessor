// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// N#5 (Z3):
//   1. SkeletonModule::FeedEngine queries IVectorEngine::AheadFieldSeqs and publishes ALL multi-entry
//      ahead pairs in ascending order in one pass, with fallback to LatestVerifiedFieldSeq when
//      AheadFieldSeqs returns 0.
//   2. FallbackCatalog::MotionFieldAhead ("motion_field_ahead"): released when healthy (negative control),
//      engaged with visible reason when degraded (overwritten / abandoned), verified both directly and
//      end-to-end through SkeletonModule::OnStatus(StatusKind::Fallbacks).
//   3. VectorEngine::DeclareAheadRingVram / ReleaseAheadRingVram: declares exact derived
//      LaneAhead::RingVramBytes under kUserMotionMatcher in VramUsers, clamps depth > kAheadCeilingLead
//      (negative control), and releases cleanly on explicit ReleaseAheadRingVram() and ~VectorEngine().
// Pure CPU; built without PCH.

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include "CppUnitTest.h"

#include <VP-V/core/FallbackCatalog.h>
#include <VP-V/core/VramUsers.h>
#include <VP-V/modules/control/skeleton/SkeletonModule.h>
#include <VP-V/modules/input/signal/ResyncExtension.h>
#include <VP-V/modules/input/source_analysis/SourceFrameTap.h>
#include <VP-V/modules/motion/estimation/LaneAheadPolicy.h>
#include <VP-V/modules/motion/estimation/VectorEngine.h>
#include <VP-V/modules/motion/estimation/VectorEngineInterface.h>

#include <atomic>
#include <cstdio>
#include <fstream>
#include <map>
#include <memory>
#include <string>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace MotionFieldAheadZ3TestsNs
{
	namespace RE = RendererExtension;
	namespace Tap = VpFrameAnalysis::SourceFrameTap;
	using Vpv::Resync::ResyncExtension;
	using Vpv::Skeleton::EngineFieldFacts;
	using Vpv::Skeleton::FeedCounters;
	using Vpv::Skeleton::SkeletonModule;
	using VpSkeleton::SourceFrameView;

	constexpr uint32_t kW = 64;
	constexpr uint32_t kH = 48;

	struct TempCfg
	{
		std::string path;
		explicit TempCfg(const std::string& text)
		{
			static std::atomic<int> counter{ 0 };
			char dir[MAX_PATH] = {};
			GetTempPathA(MAX_PATH, dir);
			path = std::string(dir) + "vp-z3-ahead-test-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(counter++) + ".cfg";
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

	struct Z3Rig
	{
		explicit Z3Rig(size_t frames = 32)
			: cfg("[vpv.skeleton]\nanalysis_lane=on\n")
			, sk(cfg.Loader(), [this]() { return base.load() + calls.fetch_add(1); })
			, store(frames * kW * kH)
			, count(frames)
		{
			sk.SetWorkerThreadsForTesting(false);
			uint32_t x = 9001u;
			for (size_t i = 0; i < store.size(); ++i)
			{
				x = x * 1664525u + 1013904223u;
				store[i] = uint16_t((x >> 12) & 1023u);
			}
		}

		~Z3Rig()
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

		const void* Host() const { return &r; }

		void Life(RE::Lifecycle kind)
		{
			RE::LifecycleFacts f;
			f.host = &r;
			f.kind = kind;
			sk.OnLifecycle(f);
		}

		void ResyncApply(const char* mode)
		{
			RE::LifecycleFacts f;
			f.host = &r;
			f.kind = RE::Lifecycle::SettingsApplied;
			f.subject = mode;
			resync.OnLifecycle(f);
		}

		void Ingress(int64_t seq)
		{
			RE::IngressContext c;
			c.host = &r;
			c.seq = seq;
			c.queueGeneration = 1;
			c.stateKey = 0x1000;
			c.formatKey = 7;
			c.frameCounter = seq;
			c.timestamp = seq * 1000;
			c.ticksPerSecond = 24000;
			c.measuredRateHz = 24.0;
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

		void Begin(int64_t seq)
		{
			base += 40000;
			RE::FrameContext c;
			c.host = &r;
			c.phase = RE::FramePhase::Begin;
			c.seq = seq;
			resync.OnFrame(c);
			Assert::IsFalse(sk.OnFrame(c));
		}

		void After(int64_t seq)
		{
			RE::FrameContext c;
			c.host = &r;
			c.phase = RE::FramePhase::AfterRender;
			c.seq = seq;
			Assert::IsFalse(sk.OnFrame(c));
		}

		void Frame(int64_t seq)
		{
			Ingress(seq);
			if (seq == 1)
				Deliver(1);
			Deliver(uint64_t(seq) + 1);
			Begin(seq);
			After(seq);
		}

		void PumpCpuWorkers()
		{
			VpSkeleton::AnalysisLane* lane = sk.LaneOf(&r);
			Assert::IsNotNull(lane);
			for (int i = 0; i < 2000 && lane->StepWorker(VpSkeleton::Backend::Cpu, 0); ++i) {}
		}

		std::vector<std::string> FallbackLines()
		{
			std::vector<std::string> lines;
			RE::Context ctx;
			ctx.host = &r;
			sk.OnStatus(ctx, RE::StatusKind::Fallbacks, lines, nullptr, nullptr);
			return lines;
		}
	};

	// Fake IVectorEngine exposing multi-entry AheadFieldSeqs and per-seq VerifiedField/GlobalVec records.
	class MultiAheadFakeEngine : public VpMotion::IVectorEngine
	{
	public:
		uint64_t era = 1;
		uint64_t latest = 0;
		std::vector<uint64_t> aheadSeqs;
		std::map<uint64_t, VpMotion::VerifiedField> fields;
		std::map<uint64_t, VpMotion::GlobalVec> globals;
		pl_tex_t dummyTex{};

		MultiAheadFakeEngine()
		{
			dummyTex.params.w = 4;
			dummyTex.params.h = 3;
		}

		void AddField(uint64_t from, uint64_t to, const VpIndicator::Stamp& stamp, double gx = 1.5, double gy = -0.5)
		{
			VpMotion::VerifiedField vf;
			vf.seq = to;
			vf.pairFrom = from;
			vf.pairTo = to;
			vf.fwd = &dummyTex;
			vf.gridW = 4;
			vf.gridH = 3;
			vf.blockSize = 16;
			vf.scaleToSourcePx = 1.0f;
			vf.stamp = stamp;
			fields[to] = vf;

			VpMotion::GlobalVec gv;
			gv.valid = true;
			gv.x = gx;
			gv.y = gy;
			globals[to] = gv;
		}

		uint64_t LatestVerifiedFieldSeq() const override { return latest; }
		uint64_t FieldEra() const override { return era; }
		bool FieldOf(uint64_t seq, VpMotion::VerifiedField& out) const override
		{
			auto it = fields.find(seq);
			if (it == fields.end())
				return false;
			out = it->second;
			return true;
		}
		float FieldScale() const override { return 1.0f; }
		bool GlobalFromEngine() const override { return true; }
		bool EngineGlobal(uint64_t seq, VpMotion::GlobalVec& out) const override
		{
			auto it = globals.find(seq);
			if (it == globals.end())
				return false;
			out = it->second;
			return true;
		}
		bool PairSpansSlip(uint64_t, uint64_t) const override { return false; }
		bool FlowIsCpuResident() const override { return false; }
		bool FlowOf(uint64_t, VpMotion::FlowView&) const override { return false; }
		void SetPreEstimateHook(VpMotion::IPreEstimateHook*) override {}
		size_t AheadFieldSeqs(uint64_t* out, size_t cap) const override
		{
			if (!out || cap == 0)
				return 0;
			const size_t n = aheadSeqs.size() < cap ? aheadSeqs.size() : cap;
			for (size_t i = 0; i < n; ++i)
				out[i] = aheadSeqs[i];
			return n;
		}
	};

	std::vector<VpSkeleton::ConsumerDecl> P3Demand()
	{
		VpSkeleton::ConsumerDecl c;
		c.id = 91;
		c.name = "z3_test_p3";
		VpSkeleton::AddConsume(c, VpSkeleton::kP3AnalysisField);
		std::vector<VpSkeleton::ConsumerDecl> out;
		out.push_back(c);
		return out;
	}

	bool HasFallbackPrefix(const std::vector<std::string>& lines, const char* id, std::string* matched = nullptr)
	{
		const std::string prefix = std::string(id) + ": ";
		for (const std::string& l : lines)
		{
			if (l.compare(0, prefix.size(), prefix) == 0)
			{
				if (matched)
					*matched = l;
				return true;
			}
		}
		return false;
	}

	// RAII cleanup guard for process-wide VramUsers singleton.
	struct VramUsersGuard
	{
		VramUsersGuard() { VpSkeleton::VramUsers::Instance().ResetForTesting(); }
		~VramUsersGuard() { VpSkeleton::VramUsers::Instance().ResetForTesting(); }
	};

	uint64_t HeldBytesFor(const VpSkeleton::VramUsers::Snapshot& snap, uint16_t id)
	{
		for (const auto& kv : snap.heldById)
			if (kv.first == id)
				return kv.second;
		return 0;
	}

	TEST_CLASS(MotionFieldAheadZ3Tests)
	{
	public:
		TEST_METHOD(FeedEnginePublishesAllAheadFieldSeqsInOnePassAndFallsBackToLatestWhenZero)
		{
			Z3Rig rig;
			MultiAheadFakeEngine eng;
			rig.sk.SetEngineProviderForTesting([&eng](const void*) { return &eng; });

			// Give the lane enough lead to hold frames 5..8 ahead of frame 3 without TooEarly.
			SkeletonModule::QueueShape q;
			q.renderAhead = 0;
			q.laneLead = 6;
			q.lookAheadWalk = 0;
			q.wired = SkeletonModule::QueueShape::kAll;
			rig.sk.SetQueueForTesting(rig.Host(), q);

			rig.ResyncApply("on");
			rig.Life(RE::Lifecycle::GpuCreated);

			int ownerTag = 0;
			rig.sk.SetExternalDemand(rig.Host(), &ownerTag, P3Demand(), {});

			// Render frames 1..3 so the graph holds P1/P2m/P3 with laneLead == 6.
			for (int64_t s = 1; s <= 3; ++s)
				rig.Frame(s);

			// Deliver source frames 5..8 ahead and pump CPU workers so P1(4..8) are published in the lane.
			for (uint64_t s = 5; s <= 8; ++s)
				Assert::IsTrue(rig.Deliver(s));
			rig.PumpCpuWorkers();

			std::shared_ptr<VpIndicator::Indicator> ind =
				Vpv::Resync::ResyncExtension::For(rig.Host())->seal.Current();
			Assert::IsTrue(ind != nullptr);

			// Expose 3 ahead fields (5, 6, 7) simultaneously via AheadFieldSeqs.
			eng.AddField(4, 5, ind->StampOf(5), 1.0, 0.25);
			eng.AddField(5, 6, ind->StampOf(6), 2.0, 0.50);
			eng.AddField(6, 7, ind->StampOf(7), 3.0, 0.75);
			eng.aheadSeqs = { 5, 6, 7 };
			eng.latest = 7;

			// ONE FeedEngine pass must publish ALL 3 multi-entry ahead pairs in ascending order!
			rig.sk.RetryEngineFeedForTesting(rig.Host());
			const FeedCounters afterMulti = rig.sk.FeedCountersOf(rig.Host());
			Assert::AreEqual(uint64_t(3), afterMulti.published,
				L"FeedEngine must publish all 3 AheadFieldSeqs entries in a single pass");
			Assert::AreEqual(uint64_t(7), afterMulti.lastSeq);
			Assert::AreEqual(uint64_t(0), afterMulti.overwritten);
			Assert::AreEqual(uint64_t(0), afterMulti.abandoned);

			for (uint64_t seq = 5; seq <= 7; ++seq)
			{
				EngineFieldFacts ff;
				Assert::IsTrue(rig.sk.EngineFieldFactsOf(rig.Host(), seq, ff),
					L"every multi-entry ahead pair must be recorded in EngineFieldFacts");
				Assert::AreEqual(seq - 1, ff.pairFrom);
				Assert::AreEqual(seq, ff.pairTo);
				Assert::IsTrue(ff.p2mPublished && ff.p3Published);
				Assert::IsTrue(ff.globalValid);
				Assert::AreEqual(double(seq - 4), ff.globalX, 1e-12);
			}

			// Idempotence: re-running FeedEngine with the same AheadFieldSeqs publishes 0 duplicates.
			rig.sk.RetryEngineFeedForTesting(rig.Host());
			Assert::AreEqual(uint64_t(3), rig.sk.FeedCountersOf(rig.Host()).published);

			// Fallback path: when AheadFieldSeqs returns 0 (e.g. legacy fake), FeedEngine falls back to LatestVerifiedFieldSeq.
			eng.aheadSeqs.clear();
			eng.AddField(7, 8, ind->StampOf(8), 4.0, 1.0);
			eng.latest = 8;
			rig.sk.RetryEngineFeedForTesting(rig.Host());
			const FeedCounters afterFallback = rig.sk.FeedCountersOf(rig.Host());
			Assert::AreEqual(uint64_t(4), afterFallback.published,
				L"FeedEngine must fall back to LatestVerifiedFieldSeq when AheadFieldSeqs returns 0");
			Assert::AreEqual(uint64_t(8), afterFallback.lastSeq);
			EngineFieldFacts ff8;
			Assert::IsTrue(rig.sk.EngineFieldFactsOf(rig.Host(), 8, ff8));
			Assert::AreEqual(uint64_t(7), ff8.pairFrom);
			Assert::AreEqual(uint64_t(8), ff8.pairTo);
		}

		TEST_METHOD(MotionFieldAheadFallbackEngagesWhenDegradedAndReleasesWhenHealthy)
		{
			// 1. Direct FallbackCatalog::MotionFieldAhead contract + healthy negative control.
			const FallbackCatalog::Decision healthy = FallbackCatalog::MotionFieldAhead(false);
			Assert::AreEqual(std::string("motion_field_ahead"), healthy.id);
			Assert::IsFalse(healthy.engaged, L"negative control: healthy state must release motion_field_ahead");

			const FallbackCatalog::Decision degraded = FallbackCatalog::MotionFieldAhead(
				true, "2 ahead P3 field(s) overwritten before publish");
			Assert::AreEqual(std::string("motion_field_ahead"), degraded.id);
			Assert::IsTrue(degraded.engaged);
			Assert::IsTrue(degraded.reason.find("overwritten") != std::string::npos);

			const FallbackCatalog::Decision defaultDegraded = FallbackCatalog::MotionFieldAhead(true);
			Assert::AreEqual(std::string("motion_field_ahead"), defaultDegraded.id);
			Assert::IsTrue(defaultDegraded.engaged);
			Assert::IsFalse(defaultDegraded.reason.empty());

			// 2. End-to-end SkeletonModule: healthy state releases motion_field_ahead; abandoned ahead pair engages it!
			Z3Rig rig;
			MultiAheadFakeEngine eng;
			rig.sk.SetEngineProviderForTesting([&eng](const void*) { return &eng; });

			SkeletonModule::QueueShape q;
			q.laneLead = 4;
			q.wired = SkeletonModule::QueueShape::kAll;
			rig.sk.SetQueueForTesting(rig.Host(), q);

			rig.ResyncApply("on");
			rig.Life(RE::Lifecycle::GpuCreated);
			int ownerTag = 0;
			rig.sk.SetExternalDemand(rig.Host(), &ownerTag, P3Demand(), {});
			for (int64_t s = 1; s <= 3; ++s)
				rig.Frame(s);

			// Healthy negative control: before any overwritten/abandoned ahead entries, motion_field_ahead is NOT engaged.
			Assert::IsFalse(HasFallbackPrefix(rig.FallbackLines(), "motion_field_ahead"),
				L"negative control: healthy SkeletonModule must not engage motion_field_ahead");

			// Degrade feed: advertise an ahead pair (49, 50) whose P1 inputs are never published, then exhaust retries (8).
			std::shared_ptr<VpIndicator::Indicator> ind =
				Vpv::Resync::ResyncExtension::For(rig.Host())->seal.Current();
			Assert::IsTrue(ind != nullptr);
			eng.AddField(49, 50, ind->StampOf(50));
			eng.aheadSeqs = { 50 };
			eng.latest = 50;

			for (int attempt = 0; attempt < 10; ++attempt)
				rig.sk.RetryEngineFeedForTesting(rig.Host());

			const FeedCounters fc = rig.sk.FeedCountersOf(rig.Host());
			Assert::IsTrue(fc.abandoned >= 1u, L"unpublishable ahead pair must become abandoned after retry budget");

			rig.Frame(4);   // advance one frame so Begin(4) runs Refresh(*st) and updates fallbackLines

			std::string matchedLine;
			Assert::IsTrue(HasFallbackPrefix(rig.FallbackLines(), "motion_field_ahead", &matchedLine),
				L"degraded SkeletonModule must engage motion_field_ahead fallback entry visibly");
			Assert::IsTrue(matchedLine.find("abandoned") != std::string::npos,
				L"motion_field_ahead fallback reason must visibly name abandoned ahead fields");
		}

		TEST_METHOD(VectorEngineAheadRingVramDeclaresClampsAndReleasesCleanly)
		{
			VramUsersGuard vramGuard;
			VpSkeleton::VramUsers& vram = VpSkeleton::VramUsers::Instance();

			constexpr uint32_t kWidth = 1920;
			constexpr uint32_t kHeight = 1080;
			constexpr int kDepth = 3;
			const uint64_t expectedDepth3 = VpMotion::LaneAhead::RingVramBytes(kWidth, kHeight, kDepth);
			Assert::IsTrue(expectedDepth3 > 0u);

			// 1. Exact derived RingVramBytes declared under kUserMotionMatcher.
			{
				VpMotion::VectorEngine engine;
				Assert::AreEqual(uint64_t(0), engine.AheadRingVramBytes());

				engine.DeclareAheadRingVram(kWidth, kHeight, kDepth);
				Assert::AreEqual(expectedDepth3, engine.AheadRingVramBytes());

				const VpSkeleton::VramUsers::Snapshot snap = vram.Take();
				Assert::AreEqual(expectedDepth3, HeldBytesFor(snap, VpSkeleton::kUserMotionMatcher),
					L"VramUsers must record exact derived RingVramBytes under kUserMotionMatcher");
				Assert::AreEqual(expectedDepth3, snap.heldBytes);
				Assert::IsTrue(snap.unpopulated.empty(), L"declared active kUserMotionMatcher must not appear unpopulated");

				// In-test negative control: depth > kAheadCeilingLead is strictly clamped to kAheadCeilingLead.
				const int overDepth = VpMotion::LaneAhead::kAheadCeilingLead + 4;
				const uint64_t expectedCap = VpMotion::LaneAhead::RingVramBytes(
					kWidth, kHeight, VpMotion::LaneAhead::kAheadCeilingLead);
				const uint64_t unclampedBytes = VpMotion::LaneAhead::RingVramBytes(kWidth, kHeight, overDepth);
				Assert::IsTrue(unclampedBytes > expectedCap,
					L"negative control precondition: unclamped depth exceeds ceiling bytes");

				engine.DeclareAheadRingVram(kWidth, kHeight, overDepth);
				Assert::AreEqual(expectedCap, engine.AheadRingVramBytes(),
					L"DeclareAheadRingVram must clamp depth to kAheadCeilingLead");
				Assert::AreEqual(expectedCap, HeldBytesFor(vram.Take(), VpSkeleton::kUserMotionMatcher));

				// Explicit release drops held bytes to 0 and clears active mark.
				engine.ReleaseAheadRingVram();
				Assert::AreEqual(uint64_t(0), engine.AheadRingVramBytes());
				const VpSkeleton::VramUsers::Snapshot afterRelease = vram.Take();
				Assert::AreEqual(uint64_t(0), HeldBytesFor(afterRelease, VpSkeleton::kUserMotionMatcher));
				Assert::AreEqual(uint64_t(0), afterRelease.heldBytes);
				Assert::IsTrue(afterRelease.unpopulated.empty());

				// Zero-width / zero-depth negative control also releases cleanly without leaking bytes.
				engine.DeclareAheadRingVram(kWidth, kHeight, 2);
				Assert::IsTrue(engine.AheadRingVramBytes() > 0u);
				engine.DeclareAheadRingVram(0, kHeight, 2);
				Assert::AreEqual(uint64_t(0), engine.AheadRingVramBytes());
				Assert::AreEqual(uint64_t(0), HeldBytesFor(vram.Take(), VpSkeleton::kUserMotionMatcher));
			}

			// 2. Destructor RAII release: ~VectorEngine() releases declared VRAM automatically.
			{
				const uint64_t expected4k = VpMotion::LaneAhead::RingVramBytes(3840, 2160, 2);
				Assert::IsTrue(expected4k > 0u);
				{
					VpMotion::VectorEngine scopedEngine;
					scopedEngine.DeclareAheadRingVram(3840, 2160, 2);
					Assert::AreEqual(expected4k, HeldBytesFor(vram.Take(), VpSkeleton::kUserMotionMatcher));
				}
				const VpSkeleton::VramUsers::Snapshot afterDtor = vram.Take();
				Assert::AreEqual(uint64_t(0), HeldBytesFor(afterDtor, VpSkeleton::kUserMotionMatcher),
					L"~VectorEngine must release ahead ring VRAM from VramUsers");
				Assert::AreEqual(uint64_t(0), afterDtor.heldBytes);
				Assert::IsTrue(afterDtor.unpopulated.empty());
			}
		}
	};
}
