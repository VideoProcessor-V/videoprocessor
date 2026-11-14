// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// ENGINE-COMPOSED-VECTORS: depth law (ComposedVectorPlan.h) and the composed per-pixel chain (ComposedVectors.h). Pure CPU.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "CppUnitTest.h"

#include <VP-V/modules/motion/estimation/composed/ComposedVectors.h>

#include <map>
#include <string>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using namespace VpComposed;

namespace ComposedVectorsTest
{
	namespace
	{
		constexpr int kW = 128, kH = 64;

		// Adjacent pairs: uniform translation of +fwdDx px per forward frame step; the reverse pair is the negation (plus reverseErr).
		class FakeSource : public IPairSource
		{
		public:
			double fwdDx = 1.0, trust = 1.0, reverseErr = 0.0;
			std::map<std::pair<uint64_t, uint64_t>, bool> missing;
			mutable int calls = 0;
			bool Adjacent(uint64_t from, uint64_t to, PairField& out, std::string* why) const override
			{
				++calls;
				if (missing.count(std::make_pair(from, to))) {
					if (why) *why = "pair missing";
					return false;
				}
				const double dx = (to > from) ? fwdDx : (-fwdDx + reverseErr);
				out = PairField::Uniform(kW, kH, 16.0, dx, 0.0, trust);
				return true;
			}
		};

		void ActivateBoth(Product& p)
		{
			p.SetSourceSize(kW, kH);
			p.Activate(Reader::P10gGrainLevel, true);
			p.Activate(Reader::TemporalDetail, true);
			p.SetWindow(10, 20);
		}
	}

	TEST_CLASS(ComposedVectorsTests)
	{
	public:
		TEST_METHOD(PlanStandardIsEightBackFourAheadWhenGrainOnly)
		{
			PlanInput in;
			in.active[0] = true;
			in.need[0] = DefaultNeed(Reader::P10gGrainLevel, false);
			in.queuedAhead = 10;
			in.keptBehind = 20;
			const Plan p = PlanDepth(in);
			Assert::AreEqual(8, (int)p.back);
			Assert::AreEqual(4, (int)p.ahead);
			Assert::IsTrue(p.note.empty());
		}
		TEST_METHOD(PlanTemporalDetailAsksUpToCeiling)
		{
			PlanInput in;
			in.active[0] = in.active[1] = true;
			in.need[0] = DefaultNeed(Reader::P10gGrainLevel, false);
			in.need[1] = DefaultNeed(Reader::TemporalDetail, false);
			in.queuedAhead = 10;
			in.keptBehind = 20;
			const Plan p = PlanDepth(in);
			Assert::AreEqual((int)VpSkeleton::kAheadCeiling, (int)p.ahead);
			Assert::AreEqual(8, (int)p.back);
		}
		TEST_METHOD(PlanLowLatencyIsTenBackZeroAhead)
		{
			PlanInput in;
			in.active[0] = true;
			in.need[0] = DefaultNeed(Reader::P10gGrainLevel, true);
			in.lowLatency = true;
			in.queuedAhead = 10;
			in.keptBehind = 20;
			const Plan p = PlanDepth(in);
			Assert::AreEqual(10, (int)p.back);
			Assert::AreEqual(0, (int)p.ahead);
			Assert::IsTrue(p.note.empty());
		}
		TEST_METHOD(PlanQueueCapIsVisible)
		{
			PlanInput in;
			in.active[0] = true;
			in.need[0] = DefaultNeed(Reader::P10gGrainLevel, false);
			in.queuedAhead = 2;
			in.keptBehind = 20;
			const Plan p = PlanDepth(in);
			Assert::AreEqual(2, (int)p.ahead);
			Assert::IsTrue(p.aheadCap == Cap::Queue);
			Assert::IsTrue(p.note.find("2 of 4 frames ahead") != std::string::npos);
		}
		TEST_METHOD(PlanHistoryAndCeilingCaps)
		{
			PlanInput in;
			in.active[0] = true;
			in.need[0].back = 30;
			in.need[0].ahead = 9;
			in.queuedAhead = 20;
			in.keptBehind = 50;
			Plan p = PlanDepth(in);
			Assert::AreEqual((int)kBackCeiling, (int)p.back);
			Assert::IsTrue(p.backCap == Cap::Ceiling);
			Assert::AreEqual((int)VpSkeleton::kAheadCeiling, (int)p.ahead);
			Assert::IsTrue(p.aheadCap == Cap::Ceiling);
			in.keptBehind = 3;
			p = PlanDepth(in);
			Assert::AreEqual(3, (int)p.back);
			Assert::IsTrue(p.backCap == Cap::History);
		}
		TEST_METHOD(PlanNoReaderComputesNothing)
		{
			PlanInput in;
			in.queuedAhead = 10;
			in.keptBehind = 20;
			const Plan p = PlanDepth(in);
			Assert::AreEqual(0, (int)p.back);
			Assert::AreEqual(0, (int)p.ahead);
			Assert::IsTrue(p.backCap == Cap::NoReader);
		}

		TEST_METHOD(UniformTranslationComposesLinearlyBack)
		{
			FakeSource src;
			Product p(&src);
			ActivateBoth(p);
			const Reached r = p.Prepare(100);
			Assert::AreEqual(8, (int)r.back);
			Assert::AreEqual(6, (int)r.ahead);
			// Back: the pair (t-k+1 -> t-k) moves -1 px per step, so d_k = -k.
			for (uint16_t k = 1; k <= 8; ++k) {
				PlanePtr pl;
				Assert::IsTrue(p.Acquire(100, Dir::Back, k, pl) == Got::Ok);
				const Sampled s = Product::Value(pl, 64.0, 32.0);
				Assert::AreEqual(-(double)k, (double)s.dx, 1e-3);
				Assert::AreEqual(0.0, (double)s.dy, 1e-3);
				Assert::IsTrue(s.trust > 0.99f);
			}
			PlanePtr a;
			Assert::IsTrue(p.Acquire(100, Dir::Ahead, 6, a) == Got::Ok);
			Assert::AreEqual(6.0, (double)Product::Value(a, 40.0, 20.0).dx, 1e-3);
		}
		TEST_METHOD(ComputeOnceAndShared)
		{
			FakeSource src;
			Product p(&src);
			ActivateBoth(p);
			p.Prepare(100);
			const uint64_t computed = p.GetStats().planesComputed;
			Assert::AreEqual((uint64_t)14, computed);   // 8 back + 6 ahead
			const int calls = src.calls;
			p.Prepare(100);
			Assert::AreEqual(computed, p.GetStats().planesComputed);
			Assert::AreEqual(calls, src.calls);
			Assert::IsTrue(p.GetStats().planeHits >= 14);
		}
		TEST_METHOD(OutOfPlanIsRefusedAndCounted)
		{
			FakeSource src;
			Product p(&src);
			p.SetSourceSize(kW, kH);
			p.Activate(Reader::P10gGrainLevel, true);
			p.SetWindow(10, 20);
			PlanePtr pl;
			Assert::IsTrue(p.Acquire(100, Dir::Ahead, 5, pl) == Got::OutOfPlan);   // grain asks 4 ahead
			Assert::IsTrue(pl == nullptr);
			Assert::IsTrue(p.Acquire(100, Dir::Back, 9, pl) == Got::OutOfPlan);
			Assert::AreEqual((uint64_t)2, p.GetStats().outOfPlan);
			Assert::AreEqual((uint64_t)0, p.GetStats().planesComputed);
		}
		TEST_METHOD(LowLatencyComputesNothingAhead)
		{
			FakeSource src;
			Product p(&src);
			ActivateBoth(p);
			p.SetMode(true);
			const Reached r = p.Prepare(100);
			Assert::AreEqual(10, (int)r.back);
			Assert::AreEqual(0, (int)r.ahead);
			PlanePtr pl;
			Assert::IsTrue(p.Acquire(100, Dir::Ahead, 1, pl) == Got::OutOfPlan);
		}
		TEST_METHOD(ChainBreakEndsTheDirectionAndIsVisible)
		{
			FakeSource src;
			src.missing[std::make_pair((uint64_t)97, (uint64_t)96)] = true;   // back step k = 4 needs pair 97 -> 96
			Product p(&src);
			ActivateBoth(p);
			const Reached r = p.Prepare(100);
			Assert::AreEqual(3, (int)r.back);
			Assert::IsTrue(r.backWhy == Got::PairAbsent);
			Assert::AreEqual(6, (int)r.ahead);
			const Product::Status st = p.CurrentStatus();
			Assert::IsTrue(st.engaged);
			Assert::IsTrue(st.text.find("3 of 8") != std::string::npos);
			Assert::IsTrue(p.GetStats().chainBreaks >= 1);
			// Never cached as a failure: when the pair arrives the plane is computed.
			src.missing.clear();
			Assert::AreEqual(8, (int)p.Prepare(100).back);
		}
		TEST_METHOD(InconsistentPairDropsTrust)
		{
			FakeSource src;
			src.reverseErr = 3.0;   // forward/back disagree by 3 px, beyond 2 x 1 px
			Product p(&src);
			ActivateBoth(p);
			p.Prepare(100);
			PlanePtr pl;
			Assert::IsTrue(p.Acquire(100, Dir::Back, 1, pl) == Got::Ok);
			Assert::AreEqual(0.0, (double)Product::Value(pl, 64.0, 32.0).trust, 1e-6);
			Assert::IsTrue(Product::Value(pl, 64.0, 32.0).unc >= 3.0f - 1e-3f);
		}
		TEST_METHOD(LowPairTrustLowersChainTrustAndRaisesUncertainty)
		{
			FakeSource src;
			src.trust = 0.5;
			Product p(&src);
			ActivateBoth(p);
			p.Prepare(100);
			PlanePtr p1, p2;
			Assert::IsTrue(p.Acquire(100, Dir::Back, 1, p1) == Got::Ok);
			Assert::IsTrue(p.Acquire(100, Dir::Back, 2, p2) == Got::Ok);
			Assert::AreEqual(0.5, (double)Product::Value(p1, 64, 32).trust, 1e-3);
			Assert::AreEqual(0.25, (double)Product::Value(p2, 64, 32).trust, 1e-3);
			Assert::AreEqual(8.0, (double)Product::Value(p1, 64, 32).unc, 1e-3);    // (1 - 0.5) x 16 px
			Assert::AreEqual(16.0, (double)Product::Value(p2, 64, 32).unc, 1e-3);
		}
		TEST_METHOD(LeavingTheFrameKillsTrust)
		{
			FakeSource src;
			src.fwdDx = 40.0;   // 40 px per step: the right edge leaves the frame
			Product p(&src);
			ActivateBoth(p);
			p.Prepare(100);
			PlanePtr pl;
			Assert::IsTrue(p.Acquire(100, Dir::Ahead, 3, pl) == Got::Ok);
			Assert::AreEqual(0.0, (double)Product::Value(pl, kW - 8.0, 32.0).trust, 1e-6);
		}
		TEST_METHOD(InvalidateDropsEveryPlane)
		{
			FakeSource src;
			Product p(&src);
			ActivateBoth(p);
			p.Prepare(100);
			Assert::IsTrue(p.PlaneCount() > 0);
			p.Invalidate();
			Assert::AreEqual((size_t)0, p.PlaneCount());
			Assert::AreEqual((uint64_t)0, p.GetStats().bytes);
			Assert::AreEqual((uint64_t)1, p.GetStats().invalidations);
			const uint64_t before = p.GetStats().planesComputed;
			p.Prepare(100);
			Assert::IsTrue(p.GetStats().planesComputed > before);
		}
		TEST_METHOD(StepIsOneProductFinestReaderWins)
		{
			FakeSource src;
			Product p(&src);
			p.SetSourceSize(kW, kH);
			p.SetWindow(10, 20);
			p.Activate(Reader::P10gGrainLevel, true);
			p.SetReaderStep(Reader::P10gGrainLevel, 8);
			Assert::AreEqual(8, p.Step());
			p.Prepare(100);
			Assert::IsTrue(p.PlaneCount() > 0);
			p.Activate(Reader::TemporalDetail, true);
			p.SetReaderStep(Reader::TemporalDetail, 2);
			Assert::AreEqual(2, p.Step());
			Assert::AreEqual((size_t)0, p.PlaneCount());       // rebuilt at the finer step, not duplicated
			Assert::IsTrue(p.GetStats().stepRebuilds >= 1);
			p.Prepare(100);
			PlanePtr pl;
			Assert::IsTrue(p.Acquire(100, Dir::Back, 1, pl) == Got::Ok);
			Assert::AreEqual(2, pl->step);
			Assert::AreEqual(kW / 2, pl->w);
		}
		TEST_METHOD(PruneReleasesOutsideFrames)
		{
			FakeSource src;
			Product p(&src);
			ActivateBoth(p);
			p.Prepare(100);
			p.Prepare(101);
			p.Prune(101, 101);
			PlanePtr pl;
			const uint64_t before = p.GetStats().planesComputed;
			Assert::IsTrue(p.Acquire(101, Dir::Back, 1, pl) == Got::Ok);
			Assert::AreEqual(before, p.GetStats().planesComputed);
			Assert::IsTrue(p.GetStats().evictions > 0);
		}
		TEST_METHOD(NoReaderComputesNothing)
		{
			FakeSource src;
			Product p(&src);
			p.SetSourceSize(kW, kH);
			p.SetWindow(10, 20);
			const Reached r = p.Prepare(100);
			Assert::AreEqual(0, (int)r.back + (int)r.ahead);
			Assert::AreEqual(0, src.calls);
		}
		TEST_METHOD(BudgetRefusalIsVisible)
		{
			FakeSource src;
			Product::Config c;
			c.budgetBytes = 1;
			Product p(&src, c);
			ActivateBoth(p);
			const Reached r = p.Prepare(100);
			Assert::AreEqual(0, (int)r.back);
			Assert::IsTrue(r.backWhy == Got::Budget);
			Assert::IsTrue(p.GetStats().budgetRefused > 0);
			Assert::IsTrue(p.CurrentStatus().engaged);
		}
	};
}
