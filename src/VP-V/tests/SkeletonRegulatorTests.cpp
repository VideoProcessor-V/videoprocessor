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

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <initializer_list>
#include <type_traits>
#include <utility>
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

	// REG-1: only optional fill is sheddable, so the ladder fixtures use the optional fill ids (before REG-1 they were the
	// raw ids 0..4 = P0..P3, core products that may no longer be shed). F(i) = the i-th optional fill product.
	ProductId F(int i)
	{
		const ProductId fill[5] = { kP5MonitorTiles, kP6EntranceAhead, kP9CameraSmoothed, kP10cGrainSplitChroma, kP12LightLevel };
		return fill[i];
	}

	RegProduct P(ProductId id, Tier tier, bool identical, bool reduced, double cost)
	{
		RegProduct p;
		p.id = id;
		p.tier = tier;
		p.identical = identical;
		p.reducedMode = reduced;
		p.inRenderMs = cost;
		return p;
	}

	// Ladder (non-deterministic): T0 F0, F1; T1 F2, F4; T2 F3 = 5 steps (REG-1: no OFA steps; F4 was the OFA product).
	std::vector<RegProduct> Products()
	{
		std::vector<RegProduct> v;
		v.push_back(P(F(0), Tier::T0, true, true, 2.0));
		v.push_back(P(F(1), Tier::T0, true, false, 3.0));    // cannot drop: forced live when it cannot move
		v.push_back(P(F(2), Tier::T1, true, true, 4.0));
		v.push_back(P(F(3), Tier::T2, false, true, 5.0));
		v.push_back(P(F(4), Tier::T1, false, true, 0.0));
		return v;
	}

	// REG-1 OfaStepIsGone: compile-time probes for the removed optical-flow step-down members.
	template <class T, class = void> struct HasOfaSteps : std::false_type {};
	template <class T> struct HasOfaSteps<T, decltype(void(std::declval<T&>().ofaSteps))> : std::true_type {};
	template <class T, class = void> struct HasOfaFlag : std::false_type {};
	template <class T> struct HasOfaFlag<T, decltype(void(std::declval<T&>().ofa))> : std::true_type {};
	template <class T, class = void> struct HasOfaStepQuery : std::false_type {};
	template <class T> struct HasOfaStepQuery<T, decltype(void(std::declval<const T&>().OfaStep()))> : std::true_type {};

	// A shedding action names a product whose state the regulator changed (SkipToNext is lateness, not shedding).
	bool IsSheddingAction(RegAction a)
	{
		return a == RegAction::MoveToRender || a == RegAction::MoveToLane || a == RegAction::Drop || a == RegAction::Restore ||
			a == RegAction::ForcedLive;
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
			Assert::AreEqual(uint16_t(5), r.MaxLevel());   // REG-1: 5 lane steps, the 3 OFA steps are gone (was 8)
			Assert::AreEqual(uint16_t(5), r.Level());
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
			Assert::IsTrue(r.StateOf(F(0)) == ProductState::Dropped);        // reduced mode: dropped
			Assert::IsTrue(r.StateOf(F(1)) == ProductState::ForcedLive);     // no reduced mode: forced live
			Assert::IsTrue(r.StateOf(F(3)) == ProductState::Dropped);
			Assert::IsTrue(r.Notice().find("forced live") != std::string::npos);
			Assert::IsTrue(r.Notice().find("charged") != std::string::npos);
			Assert::IsTrue(CountAction(r, RegAction::ForcedLive) > 0);
		}

		// 2. The margin is measured on render-only time and frozen during a back-off.
		TEST_METHOD(MarginFollowsRenderOnlyTimeAndFreezesAtBackoff)
		{
			std::vector<RegProduct> one;
			one.push_back(P(F(0), Tier::T0, true, true, 1.0));
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
		// REG-1: Level() is the lane level (no OFA level).
		TEST_METHOD(BackoffMultiplicativeGrowAfterTwoCalmWindows)
		{
			Regulator r;
			r.Configure(Products(), kPeriod, false);
			uint64_t seq = 0;
			Assert::AreEqual(uint16_t(5), r.Level());   // REG-1: was 8 (lane 5 + OFA 3)
			Assert::AreEqual(uint16_t(5), r.LaneLevel());
			Feed(r, seq, 0.0, 5.0);                    // one bad window: 120 bad frames, one back-off of the lane
			Assert::AreEqual(uint16_t(2), r.LaneLevel());
			Assert::AreEqual(uint16_t(2), r.Level());   // REG-1: was 5 (lane 2 + OFA 3)
			Assert::AreEqual(size_t(1), CountAction(r, RegAction::BackOff));
			Feed(r, seq, 0.0, 5.0);
			Assert::AreEqual(uint16_t(1), r.LaneLevel());
			Feed(r, seq, 0.0, 5.0);
			Assert::AreEqual(uint16_t(0), r.LaneLevel());
			Assert::AreEqual(size_t(3), CountAction(r, RegAction::BackOff));
			Feed(r, seq, 8.0, 5.0);                    // calm 1
			Assert::AreEqual(uint16_t(0), r.LaneLevel());
			Feed(r, seq, 8.0, 5.0);                    // calm 2: +1
			Assert::AreEqual(uint16_t(1), r.LaneLevel());
			Feed(r, seq, 0.6, 5.0);                    // between m and m + hysteresis: the dwell resets
			Feed(r, seq, 8.0, 5.0);
			Assert::AreEqual(uint16_t(1), r.LaneLevel());
			Feed(r, seq, 8.0, 5.0);
			Assert::AreEqual(uint16_t(2), r.LaneLevel());
			Assert::AreEqual(size_t(2), CountAction(r, RegAction::Grow));
		}

		// REG-1 (doc 91 C 07:45; doc 96 s3.1/s3.4 option B): every product id of the skeleton, all identical with a reduced
		// mode (the most sheddable a product can be), every tier: only the optional fill ids are ladder steps, and through
		// floors, cuts, MINOR steps, late windows, overload to the minimum and recovery no core / td.* / P10 product ever
		// leaves Laned or appears in a shedding trace. Both run kinds.
		TEST_METHOD(CoreAndTdProductsNeverAppearInTheLadderAtAnyLevel)
		{
			using namespace VpIndicator;
			std::vector<RegProduct> all;
			size_t fill = 0;
			for (ProductId id = 0; id < kSkeletonProductEnd; ++id)
			{
				const Tier t = id % 3 == 0 ? Tier::T0 : (id % 3 == 1 ? Tier::T1 : Tier::T2);
				all.push_back(P(id, t, true, true, 0.5));
				fill += IsOptionalFill(id) ? 1u : 0u;
			}
			Assert::AreEqual(size_t(5), fill, L"P5, P6, P9, P10c, P12");
			for (int det = 0; det <= 1; ++det)
			{
				Regulator r;
				r.Configure(all, kPeriod, det != 0);
				Assert::AreEqual(uint16_t(fill), r.MaxLevel(), L"the ladder holds the optional fill only");
				uint64_t seq = 0;
				auto coreUntouched = [&](const wchar_t* where) {
					for (const RegProduct& p : all)
						if (!IsOptionalFill(p.id))
							Assert::IsTrue(r.StateOf(p.id) == ProductState::Laned, where);
					for (const RegTrace& t : AllTraces(r))
						if (IsSheddingAction(t.action))
							Assert::IsTrue(IsOptionalFill(t.id), where);
				};
				coreUntouched(L"full");
				r.OnIndicatorChange(Bit(kFull));
				coreUntouched(L"FULL floor");
				Feed(r, seq, 8.0, 5.0);
				r.OnCut();
				coreUntouched(L"cut");
				Feed(r, seq, 8.0, 5.0);
				r.OnIndicatorChange(Bit(kContinuity));
				coreUntouched(L"MINOR");
				for (int w = 0; w < 8; ++w)   // overload with every product late every window, down to the minimum
				{
					for (const RegProduct& p : all)
						r.OnLaneLate(p.id, 3);
					Feed(r, seq, 0.0, 5.0);
					coreUntouched(L"overload");
				}
				Assert::AreEqual(uint16_t(0), r.Level());
				Assert::IsTrue(r.PinnedAtMinimum());
				for (const RegProduct& p : all)
					if (IsOptionalFill(p.id))
						Assert::IsTrue(r.StateOf(p.id) != ProductState::Laned, L"every fill product shed at the minimum");
				for (int w = 0; w < 4 * int(fill) + 4; ++w)
				{
					Feed(r, seq, 8.0, 5.0);
					coreUntouched(L"recovery");
				}
				Assert::AreEqual(r.MaxLevel(), r.Level(), L"recovered");
				for (int w = 0; w < 4; ++w)
				{
					r.OnUnfedFrame(false);
					r.OnCut();
				}
				coreUntouched(L"unfed");
			}
		}

		// REG-1: no optical-flow quality step-down (doc 91 C 07:45 "NO optical-flow quality step-down"): RegConfig::ofaSteps,
		// RegProduct::ofa and Regulator::OfaStep() are gone; the level is the lane level only, so the minimum is reached
		// and pinned with no hidden steps below the lane (before REG-1: 3 OFA steps after lane 0).
		TEST_METHOD(OfaStepIsGone)
		{
			Assert::IsFalse(HasOfaSteps<RegConfig>::value, L"RegConfig::ofaSteps absent");
			Assert::IsFalse(HasOfaFlag<RegProduct>::value, L"RegProduct::ofa absent");
			Assert::IsFalse(HasOfaStepQuery<Regulator>::value, L"Regulator::OfaStep() absent");
			Regulator r;
			r.Configure(Products(), kPeriod, false);
			Assert::AreEqual(uint16_t(5), r.MaxLevel(), L"5 lane steps, no OFA steps");
			uint64_t seq = 0;
			for (int i = 0; i < 3; ++i)
				Feed(r, seq, 0.0, 5.0);
			Assert::AreEqual(uint16_t(0), r.Level());
			Assert::IsFalse(r.PinnedAtMinimum());
			Feed(r, seq, 0.0, 5.0);
			Assert::IsTrue(r.PinnedAtMinimum(), L"the next low window pins: nothing below the lane");
			Assert::IsTrue(r.Notice().find("optical-flow") == std::string::npos);
			Assert::IsTrue(r.Notice().find("vector engine") == std::string::npos);
		}

		// REG-1 (101 row 17): the optional fill set is exactly P5, P6, P9, P10c, P12 (the long pair is a motion service,
		// not a regulator product); in a mixed set under overload only those are shed, core P2/P4, P10 and td.blocks stay.
		TEST_METHOD(OptionalFillOnlyIsShed)
		{
			std::vector<ProductId> fill;
			for (ProductId id = 0; id < kSkeletonProductEnd; ++id)
				if (IsOptionalFill(id))
					fill.push_back(id);
			const std::vector<ProductId> want = { kP5MonitorTiles, kP6EntranceAhead, kP9CameraSmoothed, kP10cGrainSplitChroma, kP12LightLevel };
			Assert::IsTrue(fill == want, L"the optional fill set");
			Assert::IsFalse(IsOptionalFill(kNoProduct));

			std::vector<RegProduct> prods;
			prods.push_back(P(kP2VectorPair, Tier::T0, false, true, 2.0));   // core: reduced mode, would have been dropped
			prods.push_back(P(kP4SourceStats, Tier::T0, true, false, 1.0));  // core: would have been forced live
			prods.push_back(P(kP10GrainSplit, Tier::T0, true, true, 1.0));   // TD's input: never shed
			prods.push_back(P(kTdBlocks, Tier::T1, true, true, 1.0));        // td.*: never shed
			prods.push_back(P(kP5MonitorTiles, Tier::T1, true, true, 1.0));  // fill
			prods.push_back(P(kP12LightLevel, Tier::T2, false, true, 1.0));  // fill
			Regulator r;
			r.Configure(prods, kPeriod, false);
			Assert::AreEqual(uint16_t(2), r.MaxLevel());
			uint64_t seq = 0;
			RunPlant(r, prods, 10, 10, 16.6, 10.0, seq);
			Assert::AreEqual(uint16_t(0), r.Level());
			const std::vector<ProductId> dropped = r.Dropped();
			const std::vector<ProductId> expect = { kP5MonitorTiles, kP12LightLevel };
			Assert::IsTrue(dropped == expect, L"only the fill was dropped");
			Assert::IsTrue(r.ForcedLiveIds().empty());
			for (ProductId id : std::initializer_list<ProductId>{ kP2VectorPair, kP4SourceStats, kP10GrainSplit, kTdBlocks })
				Assert::IsTrue(r.StateOf(id) == ProductState::Laned, L"core stays laned");
		}
		// 4. FULL/graph -> floor at the T0 lane steps; MINOR and cut -> one lane
		// step down, at most once per window.
		TEST_METHOD(IndicatorFloorAndStepDowns)
		{
			using namespace VpIndicator;
			Regulator r;
			r.Configure(Products(), kPeriod, false);
			Assert::AreEqual(size_t(2), r.T0StepCount());
			r.OnIndicatorChange(Bit(kFull));
			Assert::AreEqual(uint16_t(2), r.LaneLevel());
			Assert::AreEqual(uint16_t(2), r.Level());   // REG-1: Level() = the lane level (was 5 = lane 2 + OFA 3)
			Assert::AreEqual(size_t(1), CountAction(r, RegAction::ConservativeFloor));

			Regulator g;
			g.Configure(Products(), kPeriod, false);
			g.OnIndicatorChange(Bit(kGraph) | Bit(kContinuity));
			Assert::AreEqual(uint16_t(2), g.LaneLevel());

			Regulator m;
			m.Configure(Products(), kPeriod, false);
			uint64_t seq = 0;
			m.OnIndicatorChange(Bit(kContinuity));
			Assert::AreEqual(uint16_t(4), m.LaneLevel());
			Assert::AreEqual(uint16_t(4), m.Level());   // REG-1: was 7 (lane 4 + OFA 3)
			Assert::AreEqual(size_t(1), CountAction(m, RegAction::StepDownMinor));
			m.OnIndicatorChange(Bit(kTiming));
			Assert::AreEqual(uint16_t(4), m.Level());
			m.OnCut();   // same window: suppressed
			Assert::AreEqual(uint16_t(4), m.Level());
			Assert::AreEqual(uint64_t(1), m.SuppressedStepDowns());
			Assert::AreEqual(size_t(0), CountAction(m, RegAction::StepDownCut));
			Feed(m, seq, 8.0, 5.0);   // a window passes
			m.OnCut();
			Assert::AreEqual(uint16_t(3), m.Level());   // REG-1: was 6 (lane 3 + OFA 3)
			Assert::AreEqual(size_t(1), CountAction(m, RegAction::StepDownCut));

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

		// 5. Pinned at the minimum (lane at 0; REG-1: no OFA steps below it, so 3 back-off windows reach 0, was 6).
		TEST_METHOD(PinnedAtMinimumRaisesNotice)
		{
			Regulator r;
			r.Configure(Products(), kPeriod, false);
			uint64_t seq = 0;
			for (int i = 0; i < 3; ++i)
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
			prods.push_back(P(F(0), Tier::T0, true, true, 1.0));
			prods.push_back(P(F(1), Tier::T1, false, true, 2.0));    // non-identical: not sheddable on a deterministic run
			prods.push_back(P(F(2), Tier::T1, true, true, 2.0));
			prods.push_back(P(F(3), Tier::T2, false, false, 3.0));   // neither droppable nor movable: never a step
			prods.push_back(P(F(4), Tier::T1, false, true, 0.0));   // non-identical (was the OFA product): no step on a deterministic run

			Regulator a, b;
			a.Configure(prods, kPeriod, true);
			b.Configure(prods, kPeriod, true);
			Assert::AreEqual(uint16_t(2), a.MaxLevel());
			uint64_t sa = 0, sb = 0;
			const PlantRun ra = RunPlant(a, prods, 40, 12, 16.6, 10.0, sa);
			const PlantRun rb = RunPlant(b, prods, 40, 12, 16.6, 10.0, sb);
			Assert::IsTrue(ra.noDropEver);
			Assert::IsTrue(ra.moveAccountingHeld);
			Assert::IsTrue(ra.sawShed);   // identical products were forced live, never dropped
			Assert::IsTrue(a.StateOf(F(1)) == ProductState::Laned);
			Assert::IsTrue(a.StateOf(F(3)) == ProductState::Laned);
			Assert::IsTrue(a.StateOf(F(4)) == ProductState::Laned);
			Assert::AreEqual(size_t(0), CountAction(a, RegAction::Drop));
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
			Assert::IsTrue(c.StateOf(F(2)) == ProductState::MovedToRender);
			Feed(c, sc, 10.0, 5.0);
			c.OnCut();
			Assert::IsTrue(c.StateOf(F(0)) != ProductState::Dropped);
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
			// REG-1: the optical-flow input step-down is gone, so is its catalog argument (OfaStepIsGone).

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

		// B4 S3 (doc 96 section 1 FINDING): a FULL change with no feed floors the ladder with no window (budget 0), so an
		// identical product is forced live; only EndWindow grows, and it never runs unfed. OnUnfedFrame restores it after a
		// window of frames without a feed, never while a hold is in force.
		TEST_METHOD(AnUnfedStepDownRecoversAfterAWindowWithoutTheFeed)
		{
			using namespace VpIndicator;
			Regulator r;
			r.Configure(Products(), kPeriod, true);   // deterministic: no drop, so the shed identical product is forced live
			Assert::AreEqual(size_t(2), r.T0StepCount());
			const uint16_t full = r.LaneLevel();
			Assert::AreEqual(uint16_t(3), full);
			r.OnIndicatorChange(Bit(kFull));
			Assert::AreEqual(uint16_t(2), r.LaneLevel());
			Assert::IsTrue(r.StateOf(F(2)) == ProductState::ForcedLive, L"no window: budget 0, the identical product cannot move");
			Assert::IsTrue(r.Notice().find("forced live") != std::string::npos, L"the notice the defect left standing");
			for (size_t i = 0; i < 3 * r.WindowFrames(); ++i)
				r.OnUnfedFrame(true);
			Assert::AreEqual(uint16_t(2), r.LaneLevel(), L"a hold in force keeps the floor");
			for (size_t i = 0; i + 1 < r.WindowFrames(); ++i)
				r.OnUnfedFrame(false);
			Assert::AreEqual(uint16_t(2), r.LaneLevel(), L"not before a whole window");
			r.OnUnfedFrame(false);
			// P6-rest audit S3: one step per dwell (2 calm windows) like the fed path; before: restored after ONE window.
			Assert::AreEqual(uint16_t(2), r.LaneLevel(), L"one unfed window is one calm window: dwell not reached");
			for (size_t i = 0; i < r.WindowFrames(); ++i)
				r.OnUnfedFrame(false);
			Assert::AreEqual(full, r.LaneLevel(), L"restored one step after two windows without the feed");
			Assert::IsTrue(r.StateOf(F(2)) == ProductState::Laned);
			Assert::IsTrue(r.ForcedLiveIds().empty());
			Assert::IsTrue(r.Notice().empty(), L"no stale forced-live notice");
			Assert::AreEqual(size_t(1), CountAction(r, RegAction::Restore));
		}

		// Audit SHOULD 1: the once-per-window step-down latch also clears when an unfed window elapses, so a second cut one window
		// after the first steps down again (before: suppressed forever without the feed); within one window it is still suppressed.
		TEST_METHOD(AnUnfedCutAWindowLaterStepsDownAgain)
		{
			Regulator r;
			r.Configure(Products(), kPeriod, true);
			const uint16_t full = r.LaneLevel();
			Assert::AreEqual(uint16_t(3), full);
			r.OnCut();
			Assert::AreEqual(uint16_t(full - 1), r.LaneLevel(), L"the first cut steps down");
			r.OnCut();
			Assert::AreEqual(uint16_t(full - 1), r.LaneLevel(), L"a second cut in the same window is suppressed");
			for (size_t i = 0; i < r.WindowFrames(); ++i)
				r.OnUnfedFrame(false);
			// P6-rest audit S3: one unfed window no longer restores to full (dwell 2, like the fed path); before: full here,
			// and the later cuts landed on full - 1. The latch clearing under test is unchanged.
			Assert::AreEqual(uint16_t(full - 1), r.LaneLevel(), L"one unfed window: no grow yet (dwell)");
			r.OnCut();
			Assert::AreEqual(uint16_t(full - 2), r.LaneLevel(), L"a cut one window later steps down again");
			for (size_t i = 0; i < r.WindowFrames(); ++i)
				r.OnUnfedFrame(false);
			r.OnCut();
			Assert::AreEqual(uint16_t(full - 3), r.LaneLevel(), L"and again a window after that");
		}

		// P6-rest audit S3 (lead decision): from the T0 floor the unfed recovery climbs one lane step per dwell of calm
		// windows, the same ramp as the fed path; both reach full after the same number of windows, never in one jump.
		TEST_METHOD(AnUnfedRecoveryRampsLikeTheFedPath)
		{
			using namespace VpIndicator;
			Regulator fed;
			Regulator unfed;
			fed.Configure(Products(), kPeriod, false);
			unfed.Configure(Products(), kPeriod, false);
			uint64_t seq = 0;
			Feed(fed, seq, 8.0, 5.0);      // one calm window: the EMA is initialised
			fed.OnIndicatorChange(Bit(kFull));
			unfed.OnIndicatorChange(Bit(kFull));
			Assert::AreEqual(uint16_t(2), fed.LaneLevel(), L"fed floored at T0");
			Assert::AreEqual(uint16_t(2), unfed.LaneLevel(), L"unfed floored at T0");
			const size_t growsBefore = CountAction(unfed, RegAction::Grow);
			for (int w = 1; w <= 6; ++w)
			{
				Feed(fed, seq, 8.0, 5.0);
				for (size_t i = 0; i < unfed.WindowFrames(); ++i)
					unfed.OnUnfedFrame(false);
				const uint16_t want = uint16_t(std::min(5, 2 + w / 2));   // one step per 2 calm windows
				Assert::AreEqual(want, fed.LaneLevel(), L"fed ramp");
				Assert::AreEqual(want, unfed.LaneLevel(), L"unfed ramp = fed ramp");
			}
			Assert::AreEqual(unfed.MaxLevel(), unfed.LaneLevel(), L"full after the ramp");
			Assert::AreEqual(size_t(3), CountAction(unfed, RegAction::Grow) - growsBefore, L"three single steps, no jump");
		}

		TEST_METHOD(AFedRegulatorIsNeverRestoredByTheUnfedPath)
		{
			using namespace VpIndicator;
			Regulator r;
			r.Configure(Products(), kPeriod, true);
			uint64_t seq = 0;
			r.OnIndicatorChange(Bit(kFull));
			for (size_t i = 0; i < 3 * r.WindowFrames(); ++i)
			{
				r.OnUnfedFrame(false);
				r.OnFrame(seq++, 0.0, 5.0, false);   // fed, every window low
				Assert::IsTrue(r.UnfedFrames() == 0);
			}
			Assert::IsTrue(r.LaneLevel() < 2, L"the fed windows back off as before");
			Assert::AreEqual(size_t(0), CountAction(r, RegAction::Grow), L"nothing restored by the unfed path");
		}
	};
}
