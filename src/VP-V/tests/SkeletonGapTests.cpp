// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
// Built without the test project's precompiled header: keep windows.h from defining min/max.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "CppUnitTest.h"

// Skeleton S4 (src/VP-V/core/GapScheduler.h): fixed class priority, lane order, fit rule and backfill,
// the render-ahead depth sweep, overrun learning and the derived chunk cap. Fake works and clock.

#include <VP-V/core/GapScheduler.h>

#include <memory>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace SkeletonGapTestsNs
{
	using namespace VpSkeleton;

	struct Clock {
		uint64_t t = 0;
		std::function<uint64_t()> Fn() { return [this]() { return t; }; }
	};

	// A work with `chunksLeft` chunks (negative: endless) of scripted cost; predict may differ from cost.
	struct FakeWork : IGapWork {
		Clock* clk; GapClass cls; uint16_t id; Tier tier; double heavy;
		uint32_t predict; uint32_t cost; int chunksLeft;
		std::vector<uint16_t>* log;
		uint32_t runs = 0;
		FakeWork(Clock* c, GapClass k, uint16_t i, uint32_t p, uint32_t cst, int n,
		         std::vector<uint16_t>* l, Tier t = Tier::T0, double h = 0.0)
			: clk(c), cls(k), id(i), tier(t), heavy(h), predict(p), cost(cst), chunksLeft(n), log(l) {}
		GapClass Class() const override { return cls; }
		uint16_t Id() const override { return id; }
		Tier WorkTier() const override { return tier; }
		double HeavyMs() const override { return heavy; }
		bool HasWork() const override { return chunksLeft != 0; }
		uint32_t PredictUs() const override { return predict; }
		void RunChunk(uint32_t) override
		{
			clk->t += cost;
			++runs;
			if (log) log->push_back(id);
			if (chunksLeft > 0) --chunksLeft;
		}
	};

	// Render-ahead extra: `left` us of demand, chunks of at most `chunk` us.
	struct RaWork : IGapWork {
		Clock* clk; uint32_t left; uint32_t chunk;
		RaWork(Clock* c, uint32_t demand, uint32_t ch) : clk(c), left(demand), chunk(ch) {}
		GapClass Class() const override { return GapClass::RenderAheadExtra; }
		uint16_t Id() const override { return 1; }
		bool HasWork() const override { return left > 0; }
		uint32_t PredictUs() const override { return left < chunk ? left : chunk; }
		void RunChunk(uint32_t) override { const uint32_t c = PredictUs(); clk->t += c; left -= c; }
	};

	TEST_CLASS(SkeletonGapTests)
	{
	public:
		TEST_METHOD(FixedClassPriority)
		{
			Clock clk; std::vector<uint16_t> log;
			FakeWork lane(&clk, GapClass::LaneCompute, 1, 10, 10, 1, &log);
			FakeWork early(&clk, GapClass::EarlyUploadWrap, 5, 10, 10, 1, &log);
			FakeWork ra(&clk, GapClass::RenderAheadExtra, 9, 10, 10, 1, &log);
			GapScheduler s;
			s.Add(&lane); s.Add(&early); s.Add(&ra);
			const GapReport r = s.RunGap(1000, clk.Fn());
			Assert::AreEqual<size_t>(3, log.size());
			Assert::AreEqual<int>(9, log[0]);
			Assert::AreEqual<int>(5, log[1]);
			Assert::AreEqual<int>(1, log[2]);
			Assert::AreEqual<uint32_t>(3, r.chunks);
			Assert::AreEqual<uint64_t>(10, r.usedUs[0]);
			Assert::AreEqual<uint64_t>(10, r.usedUs[1]);
			Assert::AreEqual<uint64_t>(10, r.usedUs[2]);
			Assert::IsFalse(r.laneStarved);
		}

		static std::vector<uint16_t> RunLaneOrder()
		{
			Clock clk; std::vector<uint16_t> log;
			FakeWork a(&clk, GapClass::LaneCompute, 3, 10, 10, 1, &log, Tier::T1, 1.0);
			FakeWork b(&clk, GapClass::LaneCompute, 1, 10, 10, 1, &log, Tier::T0, 1.0);
			FakeWork c(&clk, GapClass::LaneCompute, 2, 10, 10, 1, &log, Tier::T0, 5.0);
			FakeWork d(&clk, GapClass::LaneCompute, 4, 10, 10, 1, &log, Tier::T0, 5.0);
			FakeWork e(&clk, GapClass::LaneCompute, 0, 10, 10, 1, &log, Tier::T2, 99.0);
			GapScheduler s;
			s.Add(&e); s.Add(&a); s.Add(&d); s.Add(&b); s.Add(&c);
			s.RunGap(1000, clk.Fn());
			return log;
		}

		TEST_METHOD(LaneOrderTierHeavyIdAndDeterminism)
		{
			const std::vector<uint16_t> first = RunLaneOrder();
			const uint16_t expect[] = { 2, 4, 1, 3, 0 };
			Assert::AreEqual<size_t>(5, first.size());
			for (size_t i = 0; i < 5; ++i) Assert::AreEqual<int>(expect[i], first[i]);
			for (int k = 0; k < 20; ++k) Assert::IsTrue(first == RunLaneOrder());
		}

		TEST_METHOD(NeverStartsNonFittingChunkAndBackfills)
		{
			Clock clk; std::vector<uint16_t> log;
			FakeWork big(&clk, GapClass::RenderAheadExtra, 1, 150, 150, 5, &log);
			FakeWork small(&clk, GapClass::LaneCompute, 2, 40, 40, -1, &log);
			GapScheduler s;
			s.Add(&big); s.Add(&small);
			const GapReport r = s.RunGap(100, clk.Fn());
			Assert::AreEqual<uint32_t>(0, big.runs);
			Assert::AreEqual<uint32_t>(2, small.runs);          // 40 + 40, then 20 left: stop
			Assert::AreEqual<uint64_t>(80, clk.t);
			Assert::AreEqual<uint32_t>(1, r.renderAheadNoFit);
			Assert::AreEqual<uint32_t>(0, r.overruns);
			Assert::IsFalse(r.laneStarved);
		}

		TEST_METHOD(RenderAheadDepthSweep)
		{
			const uint32_t X = 200, gap = 2000;
			const uint32_t depths[] = { 0, 1, 2, 3, 5 };
			uint64_t prevLane = ~uint64_t(0);
			for (uint32_t depth : depths) {
				Clock clk; std::vector<uint16_t> log;
				RaWork ra(&clk, depth * X, 500);
				FakeWork lane(&clk, GapClass::LaneCompute, 7, 300, 300, -1, &log);
				GapScheduler s;
				s.SetChunkCapUs(1000);
				s.Add(&lane); s.Add(&ra);
				const GapReport r = s.RunGap(gap, clk.Fn());
				Assert::AreEqual<uint64_t>(depth * X, r.usedUs[0]);   // RA always served in full
				Assert::AreEqual<uint32_t>(0, ra.left);
				Assert::IsFalse(r.laneStarved);
				Assert::IsTrue(r.usedUs[2] <= prevLane);              // lane gets the remainder
				Assert::AreEqual<uint64_t>(((gap - depth * X) / 300) * 300, r.usedUs[2]);
				prevLane = r.usedUs[2];
			}
			// Depth 5 with a small gap: lane starved, RA never starved by lane work.
			Clock clk; std::vector<uint16_t> log;
			RaWork ra(&clk, 5 * X, 500);
			FakeWork lane(&clk, GapClass::LaneCompute, 7, 300, 300, -1, &log);
			GapScheduler s;
			s.SetChunkCapUs(1000);
			s.Add(&lane); s.Add(&ra);
			const GapReport r = s.RunGap(1000, clk.Fn());
			Assert::AreEqual<uint64_t>(5 * X, r.usedUs[0]);
			Assert::AreEqual<uint32_t>(0, ra.left);
			Assert::AreEqual<uint32_t>(0, lane.runs);
			Assert::IsTrue(r.laneStarved);
			Assert::AreEqual<uint32_t>(1, s.Totals().laneStarvedGaps);
			Assert::AreEqual<uint32_t>(0, r.renderAheadNoFit);
		}

		TEST_METHOD(OverrunCountedAndLearned)
		{
			Clock clk; std::vector<uint16_t> log;
			FakeWork w(&clk, GapClass::LaneCompute, 3, 100, 500, -1, &log);
			GapScheduler s;
			s.SetChunkCapUs(1000);
			s.Add(&w);
			GapReport r = s.RunGap(clk.t + 300, clk.Fn());       // predicted 100 fits in 300; took 500
			Assert::AreEqual<uint32_t>(1, r.overruns);
			Assert::AreEqual<uint64_t>(200, r.overrunUs);
			Assert::AreEqual<uint64_t>(200, s.LearnedUs(3));     // 100 + (500-100)/4
			r = s.RunGap(clk.t + 250, clk.Fn());                 // learned 200 now fits 250; takes 500
			Assert::AreEqual<uint32_t>(1, r.overruns);
			Assert::AreEqual<uint64_t>(275, s.LearnedUs(3));
			const uint32_t runsBefore = w.runs;
			r = s.RunGap(clk.t + 250, clk.Fn());                 // 275 > 250: no longer picked
			Assert::AreEqual<uint32_t>(runsBefore, w.runs);
			Assert::AreEqual<uint32_t>(0, r.chunks);
			Assert::IsTrue(r.laneStarved);
			Assert::AreEqual<uint32_t>(2, s.Totals().overruns);
		}

		// M4: a learned cost above the chunk cap decays once per gap, so the work is not starved for good.
		TEST_METHOD(OverCapLearnedCostDecaysAndIsScheduledAgain)
		{
			Clock clk; std::vector<uint16_t> log;
			FakeWork w(&clk, GapClass::LaneCompute, 4, 1000, 1500, -1, &log);   // overruns to 1.5x the cap
			GapScheduler s;
			s.SetChunkCapUs(1000);
			s.Add(&w);
			GapReport r = s.RunGap(clk.t + 5000, clk.Fn());
			Assert::AreEqual<uint32_t>(1, r.chunks);
			Assert::AreEqual<uint32_t>(1, r.capNoFit);                 // learned 1125 > cap: skipped, counted once
			Assert::AreEqual<uint64_t>(1000, s.LearnedUs(4));          // and decayed toward the prediction
			const uint32_t runsBefore = w.runs;
			uint32_t gaps = 0;
			while (w.runs == runsBefore && gaps < 5) {
				s.RunGap(clk.t + 5000, clk.Fn());
				++gaps;
			}
			Assert::IsTrue(w.runs > runsBefore && gaps <= 2);          // scheduled again within a few gaps
			Assert::IsTrue(s.Totals().capNoFit >= 1);
			// Merely too little time left is not a cap miss.
			Clock c2; FakeWork v(&c2, GapClass::LaneCompute, 5, 1000, 1000, -1, nullptr);
			GapScheduler t;
			t.SetChunkCapUs(1000);
			t.Add(&v);
			r = t.RunGap(c2.t + 300, c2.Fn());
			Assert::AreEqual<uint32_t>(0, r.chunks);
			Assert::AreEqual<uint32_t>(0, r.capNoFit);
		}

		TEST_METHOD(DerivedChunkCap)
		{
			Assert::AreEqual<uint32_t>(1000, GapScheduler::DerivedChunkCapUs(1000.0 / 24.0, 0));
			Assert::AreEqual<uint32_t>(1000, GapScheduler::DerivedChunkCapUs(1000.0 / 60.0, 0));
			Assert::AreEqual<uint32_t>(1000, GapScheduler::DerivedChunkCapUs(1000.0 / 120.0, 0));
			Assert::AreEqual<uint32_t>(520, GapScheduler::DerivedChunkCapUs(1000.0 / 240.0, 0));
			Assert::AreEqual<uint32_t>(125, GapScheduler::DerivedChunkCapUs(1.0, 0));
			Assert::AreEqual<uint32_t>(100, GapScheduler::DerivedChunkCapUs(0.1, 0));
			Assert::AreEqual<uint32_t>(750, GapScheduler::DerivedChunkCapUs(1000.0 / 240.0, 750));
			Assert::AreEqual<uint32_t>(2500, GapScheduler::DerivedChunkCapUs(1000.0 / 24.0, 2500));
		}
	};
}
