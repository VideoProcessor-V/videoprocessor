// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
// Built without the test project's precompiled header: keep windows.h from defining min/max.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "CppUnitTest.h"

// Skeleton S5 (src/VP-V/core/Regulator.h and the analysis-lane entries of FallbackCatalog.h): the
// regulator against a synthetic plant (slack = budget - base - in-render costs of moved/forced
// products + fixed-seed noise). Death-spiral guard, margin on render-only time, back-off and grow,
// indicator steps, pinned at minimum, deterministic runs, window length from the period, catalog.

#include <VP-V/core/FallbackCatalog.h>
#include <VP-V/core/Regulator.h>

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace SkeletonRegulatorTestsNs
{
	using namespace VpSkeleton;

	const double kPeriod = 1000.0 / 60.0;

	struct Lcg
	{
		uint32_t s = 12345u;
		double Next()   // -1 .. 1
		{
			s = s * 1664525u + 1013904223u;
			return double((s >> 8) & 0xFFFFu) / 65535.0 * 2.0 - 1.0;
		}
	};

	RegProduct P(ProductId id, Tier tier, bool identical, bool reduced, double cost, bool ofa = false)
	{
		RegProduct p;
		p.id = id;
		p.tier = tier;
		p.identical = identical;
		p.reducedMode = reduced;
		p.inRenderMs = cost;
		p.ofa = ofa;
		return p;
	}

	// Ladder (non-deterministic): T0 id0, id1; T1 id2, id4; T2 id3 = 5 steps + 3 OFA steps = 8.
	std::vector<RegProduct> Products()
	{
		std::vector<RegProduct> v;
		v.push_back(P(0, Tier::T0, true, true, 2.0));
		v.push_back(P(1, Tier::T0, true, false, 3.0));    // cannot drop: forced live when it cannot move
		v.push_back(P(2, Tier::T1, true, true, 4.0));
		v.push_back(P(3, Tier::T2, false, true, 5.0));
		v.push_back(P(4, Tier::T1, false, true, 0.0, true));
		return v;
	}

	// Feeds one window of frames with constant slack/render values.
	void Feed(Regulator& r, uint64_t& seq, double slack, double render, size_t frames = 0)
	{
		const size_t n = frames ? frames : r.WindowFrames();
		for (size_t i = 0; i < n; ++i)
			r.OnFrame(seq++, slack, render, false);
	}

	std::vector<RegTrace> AllTraces(const Regulator& r)
	{
		std::vector<RegTrace> t(Regulator::kTraceCapacity);
		t.resize(r.Traces(t.data(), t.size()));
		return t;
	}

	size_t CountAction(const Regulator& r, RegAction a)
	{
		size_t c = 0;
		for (const RegTrace& t : AllTraces(r))
			c += t.action == a ? 1 : 0;
		return c;
	}

	double ChargedMs(const Regulator& r, const std::vector<RegProduct>& prods)
	{
		double s = 0.0;
		for (const RegProduct& p : prods)
		{
			const ProductState st = r.StateOf(p.id);
			if (st == ProductState::MovedToRender || st == ProductState::ForcedLive)
				s += p.inRenderMs;
		}
		return s;
	}
	double MovedMs(const Regulator& r, const std::vector<RegProduct>& prods)
	{
		double s = 0.0;
		for (const RegProduct& p : prods)
			if (r.StateOf(p.id) == ProductState::MovedToRender)
				s += p.inRenderMs;
		return s;
	}

	struct PlantRun
	{
		size_t levelChanges = 0;
		std::vector<size_t> changeWindows;
		bool sawShed = false;
		bool moveAccountingHeld = true;
		bool noDropEver = true;
		bool noOfaEver = true;
		bool pinnedSeen = false;
	};

	// Runs `windows` windows of the plant; overloadWindows of them with `overloadBase`, the rest with `calmBase`.
	PlantRun RunPlant(Regulator& r, const std::vector<RegProduct>& prods, size_t windows, size_t overloadWindows,
		double overloadBase, double calmBase, uint64_t& seq)
	{
		PlantRun run;
		Lcg lcg;
		for (size_t w = 0; w < windows; ++w)
		{
			const double base = w < overloadWindows ? overloadBase : calmBase;
			const uint16_t before = r.Level();
			const size_t tracesBefore = AllTraces(r).size();
			const uint64_t seqBefore = seq;
			if (w < overloadWindows)
				for (const RegProduct& p : prods)
					if (r.StateOf(p.id) == ProductState::Laned && p.tier != Tier::T0)
						r.OnLaneLate(p.id, 3);
			const double charged = ChargedMs(r, prods);
			for (size_t i = 0; i < r.WindowFrames(); ++i)
			{
				const double slack = kPeriod - base - charged + 0.15 * lcg.Next();
				r.OnFrame(seq++, slack, 5.0 + 0.05 * lcg.Next(), slack < 0.0);
			}
			if (r.Level() != before)
			{
				++run.levelChanges;
				run.changeWindows.push_back(w);
			}
			if (r.PinnedAtMinimum())
				run.pinnedSeen = true;
			if (!r.Dropped().empty() || !r.ForcedLiveIds().empty())
				run.sawShed = true;
			// A move never goes into unaccounted render cost: moved products fit in slackLow - margin.
			const std::vector<RegTrace> tr = AllTraces(r);
			for (const RegTrace& t : tr)
			{
				if (t.seq < seqBefore)
					continue;
				if (t.action == RegAction::MoveToRender && MovedMs(r, prods) > double(t.slackLowMs) - double(t.marginMs) + 1e-3 &&
					r.StateOf(t.id) == ProductState::MovedToRender)
					run.moveAccountingHeld = false;
				if (t.action == RegAction::Drop)
					run.noDropEver = false;
				if (t.action == RegAction::OfaStepDown || t.action == RegAction::OfaStepUp)
					run.noOfaEver = false;
			}
			(void)tracesBefore;
		}
		return run;
	}

	TEST_CLASS(SkeletonRegulatorTests)
	{
	public:
		// 1. Death spiral: overload with late lane products, then recovery.
		TEST_METHOD(DeathSpiralNeverMovesBeyondSlackAndConverges)
		{
			const std::vector<RegProduct> prods = Products();
			Regulator r;
			r.Configure(prods, kPeriod, false);
			Assert::AreEqual(uint16_t(8), r.MaxLevel());
			Assert::AreEqual(uint16_t(8), r.Level());
			uint64_t seq = 0;
			const PlantRun run = RunPlant(r, prods, 60, 15, 16.6, 10.0, seq);
			Assert::IsTrue(run.moveAccountingHeld, L"a move went beyond slackLow - margin");
			Assert::IsTrue(run.sawShed, L"overload shed nothing");
			Assert::IsTrue(run.pinnedSeen, L"overload never reached the minimum");
			Assert::IsTrue(CountAction(r, RegAction::SkipToNext) > 0);
			// Recovered: the slack came back and the ladder grew to the top.
			Assert::AreEqual(r.MaxLevel(), r.Level());
			Assert::AreEqual(std::string(), r.Notice());
			for (const RegProduct& p : prods)
				Assert::IsTrue(r.StateOf(p.id) == ProductState::Laned);
			// No limit cycle: after convergence at most one change per 10 windows.
			size_t late = 0;
			for (size_t w : run.changeWindows)
				late += w >= 40 ? 1 : 0;
			Assert::IsTrue(late <= 2u);
			Assert::IsTrue(run.levelChanges <= 40u);
		}

		TEST_METHOD(OverloadDropsOrForcesLiveInsteadOfMoving)
		{
			const std::vector<RegProduct> prods = Products();
			Regulator r;
			r.Configure(prods, kPeriod, false);
			uint64_t seq = 0;
			RunPlant(r, prods, 10, 10, 16.6, 10.0, seq);
			Assert::AreEqual(uint16_t(0), r.Level());
			Assert::IsTrue(r.StateOf(0) == ProductState::Dropped);        // reduced mode: dropped
			Assert::IsTrue(r.StateOf(1) == ProductState::ForcedLive);     // no reduced mode: forced live
			Assert::IsTrue(r.StateOf(3) == ProductState::Dropped);
			Assert::AreEqual(uint8_t(3), r.OfaStep());
			Assert::IsTrue(r.Notice().find("forced live") != std::string::npos);
			Assert::IsTrue(r.Notice().find("charged") != std::string::npos);
			Assert::IsTrue(CountAction(r, RegAction::ForcedLive) > 0);
		}

		// 2. The margin is measured on render-only time and frozen during a back-off.
		TEST_METHOD(MarginFollowsRenderOnlyTimeAndFreezesAtBackoff)
		{
			std::vector<RegProduct> one;
			one.push_back(P(0, Tier::T0, true, true, 1.0));
			Regulator r;
			r.Configure(one, kPeriod, false);
			const size_t W = r.WindowFrames();
			uint64_t seq = 0;
			auto window = [&](double slackBase, double slackVar, double renderStep) {
				for (size_t i = 0; i < W; ++i)
					r.OnFrame(seq++, slackBase - slackVar * double(i % 5), 5.0 + renderStep * double(i % 10), false);
			};
			window(8.0, 0.0, 0.1);
			const double m0 = r.MarginMs();
			Assert::IsTrue(std::fabs(m0 - 1.0) < 1e-9);   // k * (p99 - p50) = 2 * 0.5
			window(8.0, 1.0, 0.1);                         // lane-load variation of the slack
			window(8.0, 0.0, 0.1);
			Assert::IsTrue(std::fabs(r.MarginMs() - m0) < 1e-9);
			// A bad window with a render-time spike: back-off, margin frozen.
			const uint16_t levelBefore = r.Level();
			window(0.1, 0.0, 1.0);
			Assert::IsTrue(r.Level() < levelBefore);
			Assert::IsTrue(std::fabs(r.MarginMs() - m0) < 1e-9);
			// The next window is frozen too, calm slack or not.
			window(8.0, 0.0, 1.0);
			Assert::IsTrue(std::fabs(r.MarginMs() - m0) < 1e-9);
			// Then the EMA follows the render-only spread again.
			window(8.0, 0.0, 1.0);
			Assert::IsTrue(r.MarginMs() > m0 + 0.1);
		}

		// 3. Lane back-off is multiplicative, once per window; grow +1 after two calm windows above m + hysteresis.
		// The OFA level is separate: lane back-offs never touch it.
		TEST_METHOD(BackoffMultiplicativeGrowAfterTwoCalmWindows)
		{
			Regulator r;
			r.Configure(Products(), kPeriod, false);
			uint64_t seq = 0;
			Assert::AreEqual(uint16_t(8), r.Level());
			Assert::AreEqual(uint16_t(5), r.LaneLevel());
			Feed(r, seq, 0.0, 5.0);                    // one bad window: 120 bad frames, one back-off of the lane
			Assert::AreEqual(uint16_t(2), r.LaneLevel());
			Assert::AreEqual(uint16_t(5), r.Level());
			Assert::AreEqual(size_t(1), CountAction(r, RegAction::BackOff));
			Feed(r, seq, 0.0, 5.0);
			Assert::AreEqual(uint16_t(1), r.LaneLevel());
			Feed(r, seq, 0.0, 5.0);
			Assert::AreEqual(uint16_t(0), r.LaneLevel());
			Assert::AreEqual(size_t(3), CountAction(r, RegAction::BackOff));
			Assert::AreEqual(uint8_t(0), r.OfaStep());
			Assert::AreEqual(size_t(0), CountAction(r, RegAction::OfaStepDown));
			Feed(r, seq, 8.0, 5.0);                    // calm 1
			Assert::AreEqual(uint16_t(0), r.LaneLevel());
			Feed(r, seq, 8.0, 5.0);                    // calm 2: +1 (the OFA is at full: a lane step)
			Assert::AreEqual(uint16_t(1), r.LaneLevel());
			Feed(r, seq, 0.6, 5.0);                    // between m and m + hysteresis: the dwell resets
			Feed(r, seq, 8.0, 5.0);
			Assert::AreEqual(uint16_t(1), r.LaneLevel());
			Feed(r, seq, 8.0, 5.0);
			Assert::AreEqual(uint16_t(2), r.LaneLevel());
			Assert::AreEqual(size_t(2), CountAction(r, RegAction::Grow));
		}

		// 3b. The OFA variant is the last resort: only a low window with the lane already at 0 steps it,
		// one step per window; growth restores it before any lane step.
		TEST_METHOD(OfaStepsOnlyWhenLaneIsPinnedAndRestoresFirst)
		{
			Regulator r;
			r.Configure(Products(), kPeriod, false);
			uint64_t seq = 0;
			for (int i = 0; i < 3; ++i)
				Feed(r, seq, 0.0, 5.0);
			Assert::AreEqual(uint16_t(0), r.LaneLevel());
			Assert::AreEqual(uint8_t(0), r.OfaStep());
			Feed(r, seq, 0.0, 5.0);
			Assert::AreEqual(uint8_t(1), r.OfaStep());
			Assert::AreEqual(size_t(1), CountAction(r, RegAction::OfaStepDown));
			Feed(r, seq, 0.0, 5.0);
			Assert::AreEqual(uint8_t(2), r.OfaStep());
			Feed(r, seq, 0.0, 5.0);
			Assert::AreEqual(uint8_t(3), r.OfaStep());
			Assert::AreEqual(size_t(3), CountAction(r, RegAction::OfaStepDown));
			Assert::AreEqual(uint16_t(0), r.Level());
			Assert::IsFalse(r.PinnedAtMinimum());
			Feed(r, seq, 0.0, 5.0);
			Assert::IsTrue(r.PinnedAtMinimum());
			Assert::AreEqual(uint8_t(3), r.OfaStep());
			// Recovery: the OFA steps come back first, the lane stays at 0 meanwhile.
			Feed(r, seq, 8.0, 5.0);
			Feed(r, seq, 8.0, 5.0);
			Assert::AreEqual(uint8_t(2), r.OfaStep());
			Assert::AreEqual(uint16_t(0), r.LaneLevel());
			Feed(r, seq, 8.0, 5.0);
			Feed(r, seq, 8.0, 5.0);
			Assert::AreEqual(uint8_t(1), r.OfaStep());
			Feed(r, seq, 8.0, 5.0);
			Feed(r, seq, 8.0, 5.0);
			Assert::AreEqual(uint8_t(0), r.OfaStep());
			Assert::AreEqual(uint16_t(0), r.LaneLevel());
			Feed(r, seq, 8.0, 5.0);
			Feed(r, seq, 8.0, 5.0);
			Assert::AreEqual(uint16_t(1), r.LaneLevel());
			Assert::AreEqual(size_t(3), CountAction(r, RegAction::OfaStepUp));
		}

		// 4. FULL/graph -> floor at the T0 lane steps (the OFA step is kept); MINOR and cut -> one lane
		// step down, at most once per window.
		TEST_METHOD(IndicatorFloorAndStepDowns)
		{
			using namespace VpIndicator;
			Regulator r;
			r.Configure(Products(), kPeriod, false);
			Assert::AreEqual(size_t(2), r.T0StepCount());
			r.OnIndicatorChange(Bit(kFull));
			Assert::AreEqual(uint16_t(2), r.LaneLevel());
			Assert::AreEqual(uint8_t(0), r.OfaStep());   // a floor never changes the picture non-identically
			Assert::AreEqual(uint16_t(5), r.Level());
			Assert::AreEqual(size_t(1), CountAction(r, RegAction::ConservativeFloor));
			Assert::AreEqual(size_t(0), CountAction(r, RegAction::OfaStepDown));

			Regulator g;
			g.Configure(Products(), kPeriod, false);
			g.OnIndicatorChange(Bit(kGraph) | Bit(kContinuity));
			Assert::AreEqual(uint16_t(2), g.LaneLevel());
			Assert::AreEqual(uint8_t(0), g.OfaStep());

			Regulator m;
			m.Configure(Products(), kPeriod, false);
			uint64_t seq = 0;
			m.OnIndicatorChange(Bit(kContinuity));
			Assert::AreEqual(uint16_t(4), m.LaneLevel());
			Assert::AreEqual(uint16_t(7), m.Level());
			Assert::AreEqual(size_t(1), CountAction(m, RegAction::StepDownMinor));
			m.OnIndicatorChange(Bit(kTiming));
			Assert::AreEqual(uint16_t(7), m.Level());
			m.OnCut();   // same window: suppressed
			Assert::AreEqual(uint16_t(7), m.Level());
			Assert::AreEqual(uint64_t(1), m.SuppressedStepDowns());
			Assert::AreEqual(size_t(0), CountAction(m, RegAction::StepDownCut));
			Feed(m, seq, 8.0, 5.0);   // a window passes
			m.OnCut();
			Assert::AreEqual(uint16_t(6), m.Level());
			Assert::AreEqual(size_t(1), CountAction(m, RegAction::StepDownCut));
			Assert::AreEqual(uint8_t(0), m.OfaStep());

			// Frequent slips: many events in one window step once.
			Regulator f;
			f.Configure(Products(), kPeriod, false);
			for (int i = 0; i < 6; ++i)
				i % 2 ? f.OnCut() : f.OnIndicatorChange(Bit(kContinuity));
			Assert::AreEqual(uint16_t(4), f.LaneLevel());
			Assert::AreEqual(uint64_t(5), f.SuppressedStepDowns());

			// Already below the floor: unchanged.
			Regulator low;
			low.Configure(Products(), kPeriod, false);
			low.OnIndicatorChange(Bit(kFull));
			low.OnCut();
			low.OnIndicatorChange(Bit(kGraph));
			Assert::AreEqual(uint16_t(1), low.LaneLevel());
		}

		// 5. Pinned at the minimum (lane at 0 and the OFA at its lowest step).
		TEST_METHOD(PinnedAtMinimumRaisesNotice)
		{
			Regulator r;
			r.Configure(Products(), kPeriod, false);
			uint64_t seq = 0;
			for (int i = 0; i < 6; ++i)
				Feed(r, seq, 0.0, 5.0);
			Assert::AreEqual(uint16_t(0), r.Level());
			Assert::IsFalse(r.PinnedAtMinimum());
			Feed(r, seq, 0.0, 5.0);
			Assert::IsTrue(r.PinnedAtMinimum());
			Assert::IsTrue(r.Notice().find("minimum") != std::string::npos);
			Assert::AreEqual(size_t(1), CountAction(r, RegAction::PinnedMin));
			Feed(r, seq, 8.0, 5.0);
			Assert::IsFalse(r.PinnedAtMinimum());
		}

		// 6. Deterministic runs: zero-content-change actions only.
		TEST_METHOD(DeterministicRunNeverDropsAndIsReproducible)
		{
			std::vector<RegProduct> prods;
			prods.push_back(P(0, Tier::T0, true, true, 1.0));
			prods.push_back(P(1, Tier::T1, false, true, 2.0));    // non-identical: not sheddable on a deterministic run
			prods.push_back(P(2, Tier::T1, true, true, 2.0));
			prods.push_back(P(3, Tier::T2, false, false, 3.0));   // neither droppable nor movable: never a step
			prods.push_back(P(4, Tier::T1, false, true, 0.0, true));   // OFA: no variant steps on a deterministic run

			Regulator a, b;
			a.Configure(prods, kPeriod, true);
			b.Configure(prods, kPeriod, true);
			Assert::AreEqual(uint16_t(2), a.MaxLevel());
			Assert::AreEqual(uint8_t(0), a.OfaStep());
			uint64_t sa = 0, sb = 0;
			const PlantRun ra = RunPlant(a, prods, 40, 12, 16.6, 10.0, sa);
			const PlantRun rb = RunPlant(b, prods, 40, 12, 16.6, 10.0, sb);
			Assert::IsTrue(ra.noDropEver);
			Assert::IsTrue(ra.noOfaEver);
			Assert::IsTrue(ra.moveAccountingHeld);
			Assert::IsTrue(ra.sawShed);   // identical products were forced live, never dropped
			Assert::IsTrue(a.StateOf(1) == ProductState::Laned);
			Assert::IsTrue(a.StateOf(3) == ProductState::Laned);
			Assert::IsTrue(a.StateOf(4) == ProductState::Laned);
			Assert::AreEqual(size_t(0), CountAction(a, RegAction::Drop));
			Assert::AreEqual(size_t(0), CountAction(a, RegAction::OfaStepDown));
			// Reproducible: identical traces on two runs.
			const std::vector<RegTrace> ta = AllTraces(a);
			const std::vector<RegTrace> tb = AllTraces(b);
			Assert::AreEqual(ta.size(), tb.size());
			for (size_t i = 0; i < ta.size(); ++i)
			{
				Assert::IsTrue(ta[i].seq == tb[i].seq && ta[i].action == tb[i].action && ta[i].id == tb[i].id &&
					ta[i].level == tb[i].level && ta[i].slackLowMs == tb[i].slackLowMs &&
					ta[i].marginMs == tb[i].marginMs && ta[i].predictedDeltaMs == tb[i].predictedDeltaMs);
			}
			Assert::IsTrue(ra.levelChanges == rb.levelChanges);
			// With room, a deterministic move happens only with accounting (never beyond slackLow - margin).
			Regulator c;
			c.Configure(prods, kPeriod, true);
			uint64_t sc = 0;
			Feed(c, sc, 10.0, 5.0);
			c.OnCut();   // level 2 -> 1: product 2 is shed; 10 ms of slack: the move fits
			Assert::IsTrue(c.StateOf(2) == ProductState::MovedToRender);
			Feed(c, sc, 10.0, 5.0);
			c.OnCut();
			Assert::IsTrue(c.StateOf(0) != ProductState::Dropped);
		}

		// 7. The window length follows the measured period.
		TEST_METHOD(WindowLengthDerivedFromPeriod)
		{
			const double rates[4] = { 24.0, 50.0, 60.0, 120.0 };
			for (double hz : rates)
			{
				const double period = 1000.0 / hz;
				Regulator r;
				r.Configure(Products(), period, false);
				const size_t expect = size_t(std::floor(2000.0 / period + 0.5));
				Assert::AreEqual(expect, r.WindowFrames());
				// The decision fires after exactly one window.
				uint64_t seq = 0;
				Feed(r, seq, -1.0, 5.0, expect - 1);
				Assert::AreEqual(r.MaxLevel(), r.Level());
				Feed(r, seq, -1.0, 5.0, 1);
				Assert::IsTrue(r.Level() < r.MaxLevel());
			}
			RegConfig tiny;
			tiny.windowSeconds = 0.05;
			Regulator f;
			f.Configure(Products(), 1000.0 / 24.0, false, tiny);
			Assert::AreEqual(size_t(8), f.WindowFrames());
		}

		// 8. Catalog builders.
		TEST_METHOD(CatalogAnalysisLaneEntries)
		{
			const std::vector<std::string> none;
			FallbackCatalog::Decision d = FallbackCatalog::AnalysisLane(none, none, false, false);
			Assert::IsFalse(d.engaged);
			Assert::AreEqual(std::string("analysis_lane"), d.id);
			std::vector<std::string> dropped(1, "grain map");
			std::vector<std::string> forced(1, "motion field");
			d = FallbackCatalog::AnalysisLane(dropped, forced, true, false);
			Assert::IsTrue(d.engaged);
			Assert::AreEqual(std::string("analysis_lane"), d.id);
			Assert::IsTrue(d.reason.find("grain map") != std::string::npos);
			Assert::IsTrue(d.reason.find("forced live: motion field") != std::string::npos);
			Assert::IsTrue(d.reason.find("analysis at minimum") != std::string::npos);
			Assert::IsTrue(FallbackCatalog::AnalysisLane(none, none, false, true).engaged);

			FallbackCatalog::Decision det = FallbackCatalog::AnalysisLaneDeterminism(0);
			Assert::IsFalse(det.engaged);
			Assert::AreEqual(std::string("analysis_lane_determinism"), det.id);
			det = FallbackCatalog::AnalysisLaneDeterminism(7);
			Assert::IsTrue(det.engaged);
			Assert::IsTrue(det.reason.find("7 frames missed the bounded wait") != std::string::npos);
			Assert::IsTrue(det.reason.find("not reproducible") != std::string::npos);

			Assert::IsFalse(FallbackCatalog::SourceStatsWalk(false, true).engaged);
			Assert::IsFalse(FallbackCatalog::SourceStatsWalk(true, false).engaged);
			const FallbackCatalog::Decision w = FallbackCatalog::SourceStatsWalk(true, true);
			Assert::IsTrue(w.engaged);
			Assert::AreEqual(std::string("source_stats_walk"), w.id);
			Assert::IsTrue(w.reason.find("original walk") != std::string::npos);
		}
	};
}
