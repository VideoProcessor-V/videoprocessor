// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// ENGINE-COMPOSED-VECTORS: depth law (ComposedVectorPlan.h) and the composed per-pixel chain (ComposedVectors.h). Pure CPU.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "CppUnitTest.h"

#include <VP-V/core/FallbackCatalog.h>
#include <VP-V/modules/motion/estimation/composed/ComposedVectors.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
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
			// A pixel that stays inside is NOT killed: at k = 1 the left edge lands at x = 48 and keeps full trust and the 40 px displacement.
			Assert::IsTrue(p.Acquire(100, Dir::Ahead, 1, pl) == Got::Ok);
			Assert::IsTrue(Product::Value(pl, 8.0, 32.0).trust > 0.99f);
			Assert::AreEqual(40.0, (double)Product::Value(pl, 8.0, 32.0).dx, 1e-3);
			Assert::AreEqual(0.0, (double)Product::Value(pl, kW - 8.0, 32.0).trust, 1e-6);
			// Positions outside the frame have no value.
			Assert::AreEqual(0.0, (double)Product::Value(pl, -1.0, 10.0).trust, 1e-6);
			Assert::AreEqual(0.0, (double)Product::Value(pl, kW + 5.0, 10.0).trust, 1e-6);
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
			Assert::AreEqual((size_t)14, p.PlaneCount());   // only frame 101's 8 + 6 planes remain
			Assert::AreEqual((uint64_t)14, p.GetStats().evictions);
			PlanePtr pl;
			const uint64_t before = p.GetStats().planesComputed;
			Assert::IsTrue(p.Acquire(101, Dir::Back, 1, pl) == Got::Ok);
			Assert::AreEqual(before, p.GetStats().planesComputed);   // frame 101 is shared
			Assert::IsTrue(p.Acquire(100, Dir::Back, 1, pl) == Got::Ok);
			Assert::AreEqual(before + 1, p.GetStats().planesComputed);   // frame 100 was released: recomputed
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
		TEST_METHOD(ReverseAbsentDegradesVisiblyAndIsNotCached)
		{
			FakeSource src;
			src.missing[std::make_pair((uint64_t)99, (uint64_t)100)] = true;   // back k = 1 has its forward pair 100 -> 99 but no reverse 99 -> 100
			Product p(&src);
			ActivateBoth(p);
			Reached r = p.Prepare(100);
			Assert::AreEqual(8, (int)r.back);
			Assert::AreEqual(8, (int)r.degradedBack);    // the degradation propagates down the chain
			Assert::AreEqual(0, (int)r.degradedAhead);
			PlanePtr pl;
			Assert::IsTrue(p.Acquire(100, Dir::Back, 1, pl) == Got::Ok);
			Assert::IsTrue(pl->degraded);
			Assert::AreEqual(0.5, (double)Product::Value(pl, 64, 32).trust, 1e-3);   // pair trust 1 x noReverseTrust 0.5
			Assert::AreEqual(2.0, (double)Product::Value(pl, 64, 32).unc, 1e-3);    // 2 x consistencyPx
			Assert::AreEqual(-1.0, (double)Product::Value(pl, 64, 32).dx, 1e-3);   // the displacement is still the pair's
			Assert::AreEqual((size_t)6, p.PlaneCount());                           // only the 6 ahead planes are kept
			const Product::Status st = p.CurrentStatus();
			Assert::IsTrue(st.engaged);
			Assert::IsTrue(st.text.find("without a reverse pair") != std::string::npos);
			Assert::IsTrue(p.GetStats().reverseAbsent >= 8);
			src.missing.clear();   // the reverse pair arrives
			r = p.Prepare(100);
			Assert::AreEqual(0, (int)r.degradedBack);
			Assert::IsTrue(p.Acquire(100, Dir::Back, 1, pl) == Got::Ok);
			Assert::IsFalse(pl->degraded);
			Assert::AreEqual(1.0, (double)Product::Value(pl, 64, 32).trust, 1e-3);
			Assert::IsFalse(p.CurrentStatus().engaged);
		}
		TEST_METHOD(QueueCapThroughPrepareIsVisible)
		{
			FakeSource src;
			Product p(&src);
			ActivateBoth(p);
			p.SetWindow(2, 20);   // two frames queued ahead
			const Reached r = p.Prepare(100);
			Assert::AreEqual(2, (int)r.ahead);
			Assert::AreEqual(8, (int)r.back);
			Assert::IsTrue(p.CurrentPlan().note.find("2 of 6 frames ahead") != std::string::npos);
			Assert::IsFalse(p.CurrentStatus().engaged);   // a plan cap is the plan's own note, not an absent pair
		}
		TEST_METHOD(CoarseStepPlaneIsSampledBilinearlyByFinerAndCoarserReaders)
		{
			FakeSource src;
			Product p(&src);
			p.SetSourceSize(kW, kH);
			p.SetWindow(10, 20);
			p.Activate(Reader::P10gGrainLevel, true);
			p.SetReaderStep(Reader::P10gGrainLevel, 8);
			p.Prepare(100);
			PlanePtr pl;
			Assert::IsTrue(p.Acquire(100, Dir::Back, 2, pl) == Got::Ok);
			Assert::AreEqual(8, pl->step);
			Assert::AreEqual(kW / 8, pl->w);
			// Any position inside the frame (also between cell centres and near the border) reads the uniform -2 px.
			const double xs[] = { 0.5, 3.0, 11.9, 64.0, 127.5 };
			for (const double x : xs)
				Assert::AreEqual(-2.0, (double)Product::Value(pl, x, 31.0).dx, 1e-3);
		}
		TEST_METHOD(DefaultFormIsTheBlockFieldAndWarpIsTheOneSampler)
		{
			FakeSource src;   // +1 px per forward step
			Product p(&src);
			ActivateBoth(p);
			Assert::AreEqual(kBlockStep, p.Step());   // owner R14-16: block field by default, not per pixel
			p.Prepare(100);
			PlanePtr pl;
			Assert::IsTrue(p.Acquire(100, Dir::Ahead, 3, pl) == Got::Ok);
			Assert::AreEqual(kBlockStep, pl->step);
			Assert::AreEqual(kW / kBlockStep, pl->w);
			const Plane::Warped w = pl->Warp(64.0, 32.0);
			Assert::IsTrue(w.valid);
			Assert::AreEqual(67.0, w.x, 1e-3);   // three forward steps of +1 px
			Assert::AreEqual(32.0, w.y, 1e-3);
			Assert::IsTrue(w.trust > 0.99f);
			Assert::IsFalse(pl->Warp(-1.0, 10.0).valid);                 // outside the frame
			Assert::IsFalse(pl->Warp(kW + 5.0, 10.0).valid);
		}
		TEST_METHOD(WarpIsInvalidForADeadChainAndForALandingOutsideTheFrame)
		{
			FakeSource src;
			src.fwdDx = 40.0;   // 40 px per step
			Product p(&src);
			ActivateBoth(p);
			p.Prepare(100);
			PlanePtr pl;
			Assert::IsTrue(p.Acquire(100, Dir::Ahead, 1, pl) == Got::Ok);
			Assert::IsTrue(pl->Warp(8.0, 32.0).valid);                     // lands at 48, inside
			Assert::AreEqual(48.0, pl->Warp(8.0, 32.0).x, 1e-3);
			const Plane::Warped dead = pl->Warp(kW - 8.0, 32.0);           // the chain left the frame: trust 0
			Assert::IsFalse(dead.valid);
			Assert::AreEqual(0.0, (double)dead.trust, 1e-6);
			// Landing outside the frame with a live trust: hand-built plane, +10 px at the right edge.
			Plane q;
			q.step = 8; q.srcW = kW; q.srcH = kH; q.w = kW / 8; q.h = kH / 8;
			const size_t n = static_cast<size_t>(q.w) * q.h;
			q.dx.assign(n, 10.0f); q.dy.assign(n, 0.0f); q.trust.assign(n, 1.0f); q.unc.assign(n, 2.0f);
			Assert::IsTrue(q.Warp(64.0, 32.0).valid);
			Assert::IsFalse(q.Warp(kW - 2.0, 32.0).valid);                 // lands at kW + 8
			Assert::AreEqual(0.0, (double)Product::Value(std::make_shared<const Plane>(q), kW - 2.0, 32.0).trust, 1e-6);
			Assert::AreEqual(2.0, (double)q.Warp(kW - 2.0, 32.0).unc, 1e-6);   // the uncertainty is still reported
		}
		TEST_METHOD(PartialBudgetKeepsWhatFitsAndSaysWhy)
		{
			FakeSource src;
			Product::Config c;
			c.budgetBytes = 5000;   // block form (owner R14-16): a 128 x 64 plane is 16 x 8 cells x 16 B = 2048 bytes: two fit, a third (6144) does not
			Product p(&src, c);
			ActivateBoth(p);
			const Reached r = p.Prepare(100);
			Assert::AreEqual(2, (int)r.back);
			Assert::IsTrue(r.backWhy == Got::Budget);
			Assert::AreEqual((size_t)2, p.PlaneCount());
			Assert::IsTrue(p.GetStats().bytes <= 5000);
			Assert::IsTrue(p.CurrentStatus().engaged);
		}
		TEST_METHOD(FallbackDecisionRoundTrips)
		{
			Assert::IsFalse(FallbackCatalog::ComposedVectors(false, "").engaged);
			const FallbackCatalog::Decision d = FallbackCatalog::ComposedVectors(true, "composed vectors reach 3 of 8");
			Assert::IsTrue(d.engaged);
			Assert::IsTrue(d.reason.find("3 of 8") != std::string::npos);
			Assert::IsNotNull(FallbackCatalog::FindProductEntry("product.composed_vectors"));
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
		// Owner 11-14 16:43 item 4 (S-T1 Q1): the shared neighbour-warp (image) sampler is BICUBIC. Catmull-Rom reproduces a quadratic exactly; bilinear
		// would read the chord (30.5 at x = 5.5 of x^2).
		TEST_METHOD(WarpSamplerIsBicubicNotBilinear)
		{
			struct Img
			{
				int w = 16, h = 8;
				double at(int x, int y) const { return static_cast<double>(x) * x + 0.0 * y; }
			} img;
			double v = 0.0;
			Assert::IsTrue(SampleBicubic(img, 5.5, 3.0, v));
			Assert::AreEqual(30.25, v, 1e-9);
			Assert::IsTrue(std::fabs(v - 30.5) > 0.2);
			Assert::IsTrue(SampleBicubic(img, 7.0, 3.0, v));
			Assert::AreEqual(49.0, v, 1e-12);   // on a sample: the sample itself
		}
		TEST_METHOD(WarpSamplerRampExactAndFootprintNeverClamped)
		{
			struct Ramp
			{
				int w = 16, h = 8;
				double at(int x, int y) const { return 2.0 * x + 3.0 * y; }
			} img;
			double v = 0.0;
			Assert::IsTrue(SampleBicubic(img, 6.25, 3.75, v));
			Assert::AreEqual(2.0 * 6.25 + 3.0 * 3.75, v, 1e-9);
			Assert::IsTrue(SampleBicubic(img, 13.5, 5.5, v));    // taps 12..15 x 4..7: inside
			v = -1.0;
			Assert::IsFalse(SampleBicubic(img, 0.5, 3.0, v));    // tap x = -1
			Assert::IsFalse(SampleBicubic(img, 14.0, 3.0, v));   // tap x = 16
			Assert::IsFalse(SampleBicubic(img, 6.0, 6.0, v));    // tap y = 8
			Assert::AreEqual(-1.0, v, 0.0);                       // out untouched on refusal
		}
		TEST_METHOD(SampleLandedWarpsTheImageBicubicallyAtTheLanding)
		{
			struct Ramp
			{
				int w = kW, h = kH;
				double at(int x, int y) const { return 2.0 * x + y; }
			} img;
			Plane q;
			q.step = kBlockStep; q.srcW = kW; q.srcH = kH; q.w = kW / kBlockStep; q.h = kH / kBlockStep;
			const size_t n = static_cast<size_t>(q.w) * q.h;
			q.dx.assign(n, 3.25f); q.dy.assign(n, -1.5f); q.trust.assign(n, 1.0f); q.unc.assign(n, 0.0f);
			double v = 0.0;
			Assert::IsTrue(q.SampleLanded(img, 40.0, 20.0, v));           // lands at (43.25, 18.5)
			Assert::AreEqual(2.0 * 43.25 + 18.5, v, 1e-6);
			Assert::IsFalse(q.SampleLanded(img, kW - 2.0, 20.0, v));       // landing outside the frame
			Assert::IsFalse(q.SampleLanded(img, kW - 3.5, 20.0, v));       // landing inside, footprint clipped
			q.trust.assign(n, 0.0f);
			Assert::IsFalse(q.SampleLanded(img, 40.0, 20.0, v));           // dead chain
		}
		// The status line names the reason of the direction it reports (back first), not whichever direction failed last (audit SHOULD 6).
		TEST_METHOD(StatusReasonBelongsToTheReportedDirection)
		{
			class TwoGaps : public FakeSource
			{
			public:
				bool Adjacent(uint64_t from, uint64_t to, PairField& out, std::string* why) const override
				{
					if (from == 99 && to == 98) {
						if (why) *why = "back gap";
						return false;
					}
					if (from == 101 && to == 102) {
						if (why) *why = "ahead gap";
						return false;
					}
					return FakeSource::Adjacent(from, to, out, why);
				}
			} src;
			Product p(&src);
			ActivateBoth(p);
			const Reached r = p.Prepare(100);
			Assert::AreEqual(1, (int)r.back);
			Assert::AreEqual(1, (int)r.ahead);
			Assert::IsTrue(r.backWhyText == "back gap");
			Assert::IsTrue(r.aheadWhyText == "ahead gap");
			const Product::Status st = p.CurrentStatus();
			Assert::IsTrue(st.engaged);
			Assert::IsTrue(st.text.find("back gap") != std::string::npos);
			Assert::IsTrue(st.text.find("ahead gap") == std::string::npos);
		}

		// GF-P4 S9 (THE MEASUREMENT): six AHEAD planes whose reverse pair is absent are all degraded, so none is cached. Prepare hands
		// plane k - 1 to Acquire: the chain costs K computes, where six hint-less Acquires (the old Prepare loop) cost K(K+1)/2.
		TEST_METHOD(ReverseAbsentChainComputesEachPlaneOnce)
		{
			FakeSource src;
			for (uint64_t k = 1; k <= 6; ++k)
				src.missing[std::make_pair(100 + k, 99 + k)] = true;   // the reverse pair of ahead step k: (t + k) -> (t + k - 1)
			auto setup = [](Product& product) {
				product.SetSourceSize(kW, kH);
				product.Activate(Reader::P10gGrainLevel, true);
				product.Activate(Reader::TemporalDetail, true);
				product.SetWindow(10, 0);   // back 0, ahead 6
			};

			Product p(&src);
			setup(p);
			const Reached r = p.Prepare(100);
			Assert::AreEqual(6, (int)r.ahead);
			Assert::AreEqual(6, (int)r.degradedAhead);
			Assert::AreEqual((uint64_t)6, p.GetStats().planesComputed, L"AFTER: each plane of the chain is computed once");
			Assert::AreEqual((uint64_t)6, p.GetStats().reverseAbsent);
			Assert::AreEqual((size_t)0, p.PlaneCount(), L"a degraded plane is not cached");

			// BEFORE, in the same test: the same setup asked plane by plane with no hint (what Prepare did before this fix).
			Product q(&src);
			setup(q);
			for (uint16_t k = 1; k <= 6; ++k)
			{
				PlanePtr pl;
				Assert::IsTrue(q.Acquire(100, Dir::Ahead, k, pl) == Got::Ok);
				Assert::IsTrue(pl->degraded);
			}
			Assert::AreEqual((uint64_t)21, q.GetStats().planesComputed, L"BEFORE: 1 + 2 + ... + 6 computes");

			// The measurement: counts for one frame, and the time of 1000 frames each way (the log lines are the evidence).
			const int rounds = 1000;
			Product timedAfter(&src);
			setup(timedAfter);
			const auto afterStart = std::chrono::steady_clock::now();
			for (int i = 0; i < rounds; ++i)
				timedAfter.Prepare(100);
			const double afterMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - afterStart).count();
			Product timedBefore(&src);
			setup(timedBefore);
			const auto beforeStart = std::chrono::steady_clock::now();
			for (int i = 0; i < rounds; ++i)
				for (uint16_t k = 1; k <= 6; ++k)
				{
					PlanePtr pl;
					timedBefore.Acquire(100, Dir::Ahead, k, pl);
				}
			const double beforeMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - beforeStart).count();
			const std::string line = "GF-P4 S9 measurement: planes computed per frame AFTER " + std::to_string(p.GetStats().planesComputed) +
				", BEFORE " + std::to_string(q.GetStats().planesComputed) + "; 1000 frames AFTER " + std::to_string(afterMs) + " ms (" +
				std::to_string(timedAfter.GetStats().planesComputed) + " planes), BEFORE " + std::to_string(beforeMs) + " ms (" +
				std::to_string(timedBefore.GetStats().planesComputed) + " planes)\n";
			Logger::WriteMessage(line.c_str());
			Assert::AreEqual((uint64_t)6000, timedAfter.GetStats().planesComputed, L"1000 frames, 6 computes each");
			Assert::AreEqual((uint64_t)21000, timedBefore.GetStats().planesComputed, L"1000 frames, 21 computes each");
		}

		// GF-P4 S9: a hint that is not plane k - 1 of the same (t, direction) is ignored: the recursion runs and the plane is the
		// no-hint plane. Here the hint is the AHEAD plane 1 for a BACK plane 2 (reverse pairs present: planes are cached).
		TEST_METHOD(PrevHintIgnoredWhenNotPlaneKMinus1)
		{
			FakeSource src;
			Product donor(&src);
			ActivateBoth(donor);
			PlanePtr hint;
			Assert::IsTrue(donor.Acquire(100, Dir::Ahead, 1, hint) == Got::Ok);
			Assert::IsTrue(hint != nullptr);

			Product withHint(&src);
			ActivateBoth(withHint);
			PlanePtr hinted;
			Assert::IsTrue(withHint.Acquire(100, Dir::Back, 2, hinted, nullptr, hint) == Got::Ok);
			Assert::AreEqual((uint64_t)2, withHint.GetStats().planesComputed, L"the recursion ran: back 1 and back 2");

			Product plain(&src);
			ActivateBoth(plain);
			PlanePtr unhinted;
			Assert::IsTrue(plain.Acquire(100, Dir::Back, 2, unhinted) == Got::Ok);

			const Sampled a = Product::Value(hinted, 64.0, 32.0);
			const Sampled b = Product::Value(unhinted, 64.0, 32.0);
			Assert::AreEqual(-2.0, (double)a.dx, 1e-3, L"back 2 is two pixels back");
			Assert::AreEqual((double)b.dx, (double)a.dx, 1e-9, L"equal to the no-hint result");
			Assert::AreEqual((double)b.dy, (double)a.dy, 1e-9);
			Assert::AreEqual((double)b.trust, (double)a.trust, 1e-9);
		}

		// GEM-125: Unified pixel-index convention ((i + 0.5) * cellPx - 0.5) aligns BilinearTap, Plane::At, Plane::Warp,
		// and Plane::SampleLanded (used by GrainLevel::WarpFromComposed) with SampleBicubic at step == 1 and coarse step == 16.
		TEST_METHOD(BilinearTapAndAtAlignWithPixelIndexConventionAtCellSizeOneAndCoarse)
		{
			// Part 1: cellPx = 1 per-pixel field with spatially varying displacement.
			constexpr int w1 = 16, h1 = 12;
			Plane perPx;
			perPx.step = 1;
			perPx.srcW = w1;
			perPx.srcH = h1;
			perPx.w = w1;
			perPx.h = h1;
			const size_t n1 = static_cast<size_t>(w1) * static_cast<size_t>(h1);
			perPx.dx.resize(n1);
			perPx.dy.resize(n1);
			perPx.trust.assign(n1, 1.0f);
			perPx.unc.assign(n1, 0.0f);
			for (int iy = 0; iy < h1; ++iy) {
				for (int ix = 0; ix < w1; ++ix) {
					const size_t idx = static_cast<size_t>(iy) * static_cast<size_t>(w1) + static_cast<size_t>(ix);
					perPx.dx[idx] = 0.25f * static_cast<float>(ix) + 1.0f;
					perPx.dy[idx] = -0.125f * static_cast<float>(iy) + 0.5f;
				}
			}

			for (int iy = 0; iy < h1; ++iy) {
				for (int ix = 0; ix < w1; ++ix) {
					const BilinearTap tap(static_cast<double>(ix), static_cast<double>(iy), 1.0, w1, h1);
					const size_t expectedIdx = static_cast<size_t>(iy) * static_cast<size_t>(w1) + static_cast<size_t>(ix);
					Assert::AreEqual(expectedIdx, tap.i00, L"cellPx=1 integer pixel hits exact cell");
					Assert::AreEqual(0.0, (double)tap.fx, 1e-12, L"cellPx=1 integer pixel has zero x fractional offset");
					Assert::AreEqual(0.0, (double)tap.fy, 1e-12, L"cellPx=1 integer pixel has zero y fractional offset");

					const Sampled s = perPx.At(static_cast<double>(ix), static_cast<double>(iy));
					Assert::AreEqual(0.25 * ix + 1.0, (double)s.dx, 1e-6);
					Assert::AreEqual(-0.125 * iy + 0.5, (double)s.dy, 1e-6);
					Assert::AreEqual(1.0, (double)s.trust, 1e-6);
				}
			}

			// End-to-end alignment with SampleBicubic / WarpFromComposed: querying Plane::SampleLanded at integer pixel (5, 4)
			// must use exact cell (5, 4) displacement (dx = 2.25, dy = 0.0) with zero half-pixel shift.
			struct LinearRamp
			{
				int w = 16, h = 12;
				double at(int x, int y) const { return 2.0 * x + 3.0 * y; }
			} img{ w1, h1 };
			double landedVal = 0.0, refBicubic = 0.0;
			Assert::IsTrue(perPx.SampleLanded(img, 5.0, 4.0, landedVal));
			Assert::IsTrue(SampleBicubic(img, 5.0 + 2.25, 4.0 + 0.0, refBicubic));
			Assert::AreEqual(refBicubic, landedVal, 1e-6, L"SampleLanded at integer pixel aligns with SampleBicubic with zero half-pixel shift");

			// Same-input negative control (Rule 3): pre-fix formula rawGx = x / cellPx - 0.5 shifts x = 0.0 to -0.5 (out-of-grid clamp)
			// and shifts x = 5.0 to 4.5 (half-pixel interpolation between cells 4 and 5), corrupting both dx and landed image sample.
			const double preFixGxAtZero = 0.0 / 1.0 - 0.5;
			const double unifiedGxAtZero = (0.0 + 0.5) / 1.0 - 0.5;
			Assert::AreEqual(-0.5, preFixGxAtZero, 1e-12, L"pre-fix x/cellPx - 0.5 at x=0 yields -0.5");
			Assert::AreEqual(0.0, unifiedGxAtZero, 1e-12, L"unified (x+0.5)/cellPx - 0.5 at x=0 yields exact 0.0");

			const double preFixGxAtFive = 5.0 / 1.0 - 0.5;   // 4.5 -> blends cell 4 and cell 5
			const int oldX0 = static_cast<int>(std::floor(preFixGxAtFive));
			const double oldFx = preFixGxAtFive - oldX0;
			const size_t row4 = static_cast<size_t>(4) * static_cast<size_t>(w1);
			const double preFixDxAtFive = (1.0 - oldFx) * perPx.dx[row4 + oldX0] + oldFx * perPx.dx[row4 + oldX0 + 1];
			Assert::IsTrue(std::fabs(preFixDxAtFive - (0.25 * 5.0 + 1.0)) > 0.1,
				L"negative control: pre-fix formula shifts x=5 into half-pixel interpolation and misses exact cell dx");

			// Part 2: coarse cellPx = 16 on non-multiple frame 100x70 (grid 7x5).
			constexpr int srcW = 100, srcH = 70, coarseStep = 16;
			constexpr int gridW = (srcW + coarseStep - 1) / coarseStep;   // 7
			constexpr int gridH = (srcH + coarseStep - 1) / coarseStep;   // 5
			Plane coarse;
			coarse.step = coarseStep;
			coarse.srcW = srcW;
			coarse.srcH = srcH;
			coarse.w = gridW;
			coarse.h = gridH;
			const size_t nCoarse = static_cast<size_t>(gridW) * static_cast<size_t>(gridH);
			coarse.dx.resize(nCoarse);
			coarse.dy.resize(nCoarse);
			coarse.trust.assign(nCoarse, 1.0f);
			coarse.unc.assign(nCoarse, 0.0f);
			for (int j = 0; j < gridH; ++j) {
				for (int i = 0; i < gridW; ++i) {
					const size_t idx = static_cast<size_t>(j) * static_cast<size_t>(gridW) + static_cast<size_t>(i);
					coarse.dx[idx] = 2.0f * static_cast<float>(i) + 1.0f;
					coarse.dy[idx] = 3.0f * static_cast<float>(j) - 2.0f;
				}
			}

			// Cell (0, 0) centre is (0.5 * 16 - 0.5, 0.5 * 16 - 0.5) = (7.5, 7.5): exact weight 1.0 on cell (0, 0).
			const BilinearTap c00(7.5, 7.5, static_cast<double>(coarseStep), gridW, gridH);
			Assert::AreEqual(static_cast<size_t>(0), c00.i00);
			Assert::AreEqual(0.0, (double)c00.fx, 1e-12);
			Assert::AreEqual(0.0, (double)c00.fy, 1e-12);
			const Sampled s00 = coarse.At(7.5, 7.5);
			Assert::AreEqual(1.0, (double)s00.dx, 1e-6);
			Assert::AreEqual(-2.0, (double)s00.dy, 1e-6);

			// Cell (2, 1) centre is (2.5 * 16 - 0.5, 1.5 * 16 - 0.5) = (39.5, 23.5): exact weight 1.0 on cell (2, 1).
			const BilinearTap c21(39.5, 23.5, static_cast<double>(coarseStep), gridW, gridH);
			Assert::AreEqual(static_cast<size_t>(1 * gridW + 2), c21.i00);
			Assert::AreEqual(0.0, (double)c21.fx, 1e-12);
			Assert::AreEqual(0.0, (double)c21.fy, 1e-12);
			const Sampled s21 = coarse.At(39.5, 23.5);
			Assert::AreEqual(5.0, (double)s21.dx, 1e-6);
			Assert::AreEqual(1.0, (double)s21.dy, 1e-6);

			// Negative control at coarse cell (2, 1) centre: pre-fix 39.5 / 16 - 0.5 = 1.96875 misses exact cell centre.
			const double preFixCoarseGx = 39.5 / static_cast<double>(coarseStep) - 0.5;
			const double preFixCoarseDx = (2.0 - preFixCoarseGx) * coarse.dx[1 * gridW + 1] + (preFixCoarseGx - 1.0) * coarse.dx[1 * gridW + 2];
			Assert::IsTrue(std::fabs(preFixCoarseDx - 5.0) > 0.05,
				L"negative control: pre-fix formula misses coarse cell (2,1) centre and dilutes dx");

			// Clean clamping at non-multiple frame edges (99, 69) and beyond grid extent (120, 80).
			const BilinearTap edgeTap(99.0, 69.0, static_cast<double>(coarseStep), gridW, gridH);
			Assert::IsTrue(edgeTap.i00 < nCoarse && edgeTap.i10 < nCoarse && edgeTap.i01 < nCoarse && edgeTap.i11 < nCoarse);
			const Sampled sEdge = coarse.At(99.0, 69.0);
			Assert::IsTrue(std::isfinite(sEdge.dx) && std::isfinite(sEdge.dy) && std::isfinite(sEdge.trust));
			Assert::AreEqual(1.0, (double)sEdge.trust, 1e-6);

			const BilinearTap beyondTap(120.0, 80.0, static_cast<double>(coarseStep), gridW, gridH);
			Assert::AreEqual(nCoarse - 1, beyondTap.i00);
			Assert::AreEqual(nCoarse - 1, beyondTap.i11);
			Assert::AreEqual(0.0, (double)beyondTap.fx, 1e-12);
			Assert::AreEqual(0.0, (double)beyondTap.fy, 1e-12);
		}

		// GEM-125: Plane::At masks zero-trust corners and normalises across trusted corners only so zero-trust / garbage / NaN
		// cells never dilute or poison trusted neighbours; all-zero-trust corners yield dx = dy = trust = 0 with finite unc.
		TEST_METHOD(PlaneAtMasksZeroTrustCornersWithoutDilutingTrustedVectors)
		{
			Plane q;
			q.step = 8;
			q.srcW = 16;
			q.srcH = 16;
			q.w = 2;
			q.h = 2;
			// Exact midpoint of all 4 cell centres ((3.5, 3.5) .. (11.5, 11.5)) is (7.5, 7.5): fx = fy = 0.5, all 4 weights = 0.25.
			const double midX = 7.5, midY = 7.5;
			const BilinearTap tap(midX, midY, static_cast<double>(q.step), q.w, q.h);
			Assert::AreEqual(0.5, (double)tap.fx, 1e-12);
			Assert::AreEqual(0.5, (double)tap.fy, 1e-12);

			// Case 1: 3 trusted corners (+8, -4), 1 zero-trust corner (0, 0).
			q.dx = { 8.0f, 8.0f, 8.0f, 0.0f };
			q.dy = { -4.0f, -4.0f, -4.0f, 0.0f };
			q.trust = { 1.0f, 1.0f, 1.0f, 0.0f };
			q.unc = { 1.5f, 1.5f, 1.5f, 10.0f };

			const Sampled sZero = q.At(midX, midY);
			Assert::IsTrue(std::isfinite(sZero.dx) && std::isfinite(sZero.dy) && std::isfinite(sZero.trust) && std::isfinite(sZero.unc));
			Assert::AreEqual(8.0, (double)sZero.dx, 1e-5, L"zero-trust corner must not dilute trusted dx");
			Assert::AreEqual(-4.0, (double)sZero.dy, 1e-5, L"zero-trust corner must not dilute trusted dy");
			Assert::IsTrue(sZero.trust > 0.0f);

			// Same-input negative control (Rule 3): naive unmasked bilinear average dilutes dx from 8.0 to 6.0 and dy from -4.0 to -3.0.
			const float naiveDxZero = tap.Mix(q.dx);
			const float naiveDyZero = tap.Mix(q.dy);
			Assert::AreEqual(6.0, (double)naiveDxZero, 1e-5, L"negative control: naive unmasked mix dilutes dx to 6.0");
			Assert::AreEqual(-3.0, (double)naiveDyZero, 1e-5, L"negative control: naive unmasked mix dilutes dy to -3.0");
			Assert::IsTrue(std::fabs((double)naiveDxZero - 8.0) > 1.5);

			// Case 2: zero-trust corner holds huge garbage (+999, -999).
			q.dx[3] = 999.0f;
			q.dy[3] = -999.0f;
			const Sampled sGarbage = q.At(midX, midY);
			Assert::AreEqual(8.0, (double)sGarbage.dx, 1e-5, L"zero-trust garbage corner must not corrupt trusted dx");
			Assert::AreEqual(-4.0, (double)sGarbage.dy, 1e-5, L"zero-trust garbage corner must not corrupt trusted dy");
			Assert::IsTrue(sGarbage.trust > 0.0f);
			const float naiveDxGarbage = tap.Mix(q.dx);
			Assert::IsTrue(std::fabs((double)naiveDxGarbage - 8.0) > 200.0,
				L"negative control: naive unmasked mix is severely corrupted by zero-trust garbage corner");

			// Case 3: zero-trust corner holds NaN displacement.
			const float kNan = std::numeric_limits<float>::quiet_NaN();
			q.dx[3] = kNan;
			q.dy[3] = kNan;
			const Sampled sNan = q.At(midX, midY);
			Assert::IsTrue(std::isfinite(sNan.dx) && std::isfinite(sNan.dy) && std::isfinite(sNan.trust) && std::isfinite(sNan.unc));
			Assert::AreEqual(8.0, (double)sNan.dx, 1e-5, L"zero-trust NaN corner must not poison trusted dx");
			Assert::AreEqual(-4.0, (double)sNan.dy, 1e-5, L"zero-trust NaN corner must not poison trusted dy");
			Assert::IsTrue(sNan.trust > 0.0f);
			const float naiveDxNan = tap.Mix(q.dx);
			Assert::IsFalse(std::isfinite(naiveDxNan), L"negative control: naive unmasked mix is poisoned with NaN");

			// Case 4: all 4 corners have trust == 0 -> returns dx = dy = trust = 0 with finite unc.
			q.dx = { 5.0f, -3.0f, 7.0f, 9.0f };
			q.dy = { 2.0f, 4.0f, -6.0f, 8.0f };
			q.trust = { 0.0f, 0.0f, 0.0f, 0.0f };
			q.unc = { 2.0f, 4.0f, 6.0f, 8.0f };
			const Sampled sAllZero = q.At(midX, midY);
			Assert::AreEqual(0.0, (double)sAllZero.dx, 1e-6);
			Assert::AreEqual(0.0, (double)sAllZero.dy, 1e-6);
			Assert::AreEqual(0.0, (double)sAllZero.trust, 1e-6);
			Assert::IsTrue(std::isfinite(sAllZero.unc));
			Assert::AreEqual(5.0, (double)sAllZero.unc, 1e-5, L"all-zero-trust corners still report finite interpolated unc");

			// All-zero-trust with one NaN unc corner still reports finite unc from remaining finite corners.
			q.unc = { 4.0f, 4.0f, 4.0f, kNan };
			const Sampled sAllZeroNanUnc = q.At(midX, midY);
			Assert::AreEqual(0.0, (double)sAllZeroNanUnc.dx, 1e-6);
			Assert::AreEqual(0.0, (double)sAllZeroNanUnc.dy, 1e-6);
			Assert::AreEqual(0.0, (double)sAllZeroNanUnc.trust, 1e-6);
			Assert::IsTrue(std::isfinite(sAllZeroNanUnc.unc));
			Assert::AreEqual(4.0, (double)sAllZeroNanUnc.unc, 1e-5);
		}

		// GEM-125: ComposeStep clamps partial right/bottom edge-cell anchors inside [0, srcW - 1] x [0, srcH - 1] on
		// non-multiple frame dimensions, and rejects non-finite (NaN / +Inf) trust without leaking NaN or false trust.
		TEST_METHOD(ComposeStepClampsPartialEdgeCellAndRejectsNonFiniteTrust)
		{
			constexpr int srcW = 100, srcH = 70, step = 16;
			constexpr int gridW = (srcW + step - 1) / step;   // 7
			constexpr int gridH = (srcH + step - 1) / step;   // 5

			// Part 1: Non-multiple frame 100x70 at step 16 with inward pan (-2, -1) on forward step (reverse (+2, +1)).
			class PanSource : public IPairSource
			{
			public:
				int w = 100, h = 70, cell = 16;
				double vx = -2.0, vy = -1.0;
				float fwdTrust = 1.0f;
				float revTrust = 1.0f;
				bool Adjacent(uint64_t from, uint64_t to, PairField& out, std::string* /*why*/) const override
				{
					const bool forward = to > from;
					out = PairField::Uniform(w, h, static_cast<double>(cell),
						forward ? vx : -vx, forward ? vy : -vy, 1.0);
					const float t = forward ? fwdTrust : revTrust;
					std::fill(out.trust.begin(), out.trust.end(), t);
					return true;
				}
			} src;
			src.w = srcW;
			src.h = srcH;
			src.cell = step;

			// Same-input negative control (Rule 3): verify the unclamped right/bottom edge cell centre lies strictly outside [0, srcW) x [0, srcH).
			const double unclampedRightPx = (static_cast<double>(gridW - 1) + 0.5) * step - 0.5;    // 6.5 * 16 - 0.5 = 103.5 > 99
			const double unclampedBottomPy = (static_cast<double>(gridH - 1) + 0.5) * step - 0.5;   // 4.5 * 16 - 0.5 = 71.5 > 69
			Assert::IsTrue(unclampedRightPx > static_cast<double>(srcW),
				L"negative control: unclamped partial right-edge cell centre exceeds srcW and would drop trust to 0");
			Assert::IsTrue(unclampedBottomPy > static_cast<double>(srcH),
				L"negative control: unclamped partial bottom-edge cell centre exceeds srcH and would drop trust to 0");

			Product p(&src);
			p.SetSourceSize(srcW, srcH);
			p.Activate(Reader::P10gGrainLevel, true);
			p.SetReaderStep(Reader::P10gGrainLevel, step);
			p.SetWindow(4, 4);

			PlanePtr pl;
			Assert::IsTrue(p.Acquire(100, Dir::Ahead, 1, pl) == Got::Ok);
			Assert::AreEqual(gridW, pl->w);
			Assert::AreEqual(gridH, pl->h);

			// Partial right-edge cell (6, 2), partial bottom-edge cell (3, 4), and bottom-right corner cell (6, 4) all keep full trust.
			const size_t rightIdx = static_cast<size_t>(2) * gridW + (gridW - 1);
			const size_t bottomIdx = static_cast<size_t>(gridH - 1) * gridW + 3;
			const size_t cornerIdx = static_cast<size_t>(gridH - 1) * gridW + (gridW - 1);
			const size_t edgeCells[] = { rightIdx, bottomIdx, cornerIdx };
			for (const size_t idx : edgeCells) {
				Assert::IsTrue(pl->trust[idx] > 0.99f, L"clamped partial edge cell keeps valid trust");
				Assert::AreEqual(-2.0, (double)pl->dx[idx], 1e-4);
				Assert::AreEqual(-1.0, (double)pl->dy[idx], 1e-4);
			}
			const Plane::Warped cornerWarp = pl->Warp(static_cast<double>(srcW - 1), static_cast<double>(srcH - 1));
			Assert::IsTrue(cornerWarp.valid);
			Assert::AreEqual(static_cast<double>(srcW - 1) - 2.0, cornerWarp.x, 1e-4);
			Assert::AreEqual(static_cast<double>(srcH - 1) - 1.0, cornerWarp.y, 1e-4);

			// Part 2: Non-finite trust (NaN and +Inf in ab.trust, plus NaN in ba.trust) with finite dx/dy must be rejected.
			const float kNan = std::numeric_limits<float>::quiet_NaN();
			const float kInf = std::numeric_limits<float>::infinity();

			// Same-input negative control (Rule 3): unguarded std::min(std::max(v.trust, 0.0f), 1.0f) leaks NaN for NaN input
			// and silently escalates +Inf to full trust 1.0f.
			PairField probeAb;
			src.fwdTrust = kNan;
			Assert::IsTrue(src.Adjacent(100, 101, probeAb, nullptr));
			const BilinearTap probeTap(50.0, 35.0, probeAb.cellPx, probeAb.gridW, probeAb.gridH);
			const float sampledNanTrust = probeTap.Mix(probeAb.trust);
			const float unguardedNanClamp = (std::min)((std::max)(sampledNanTrust, 0.0f), 1.0f);
			Assert::IsFalse(std::isfinite(unguardedNanClamp),
				L"negative control: unguarded clamp leaks NaN trust into output");

			src.fwdTrust = kInf;
			Assert::IsTrue(src.Adjacent(100, 101, probeAb, nullptr));
			const float sampledInfTrust = probeTap.Mix(probeAb.trust);
			const float unguardedInfClamp = (std::min)((std::max)(sampledInfTrust, 0.0f), 1.0f);
			Assert::AreEqual(1.0, (double)unguardedInfClamp, 1e-6,
				L"negative control: unguarded clamp silently promotes +Inf trust to full 1.0");

			const struct { float fwd; float rev; } badCases[] = {
				{ kNan, 1.0f },
				{ kInf, 1.0f },
				{ 1.0f, kNan },
			};
			for (const auto& tc : badCases) {
				src.fwdTrust = tc.fwd;
				src.revTrust = tc.rev;
				p.Invalidate();
				PlanePtr badPl;
				Assert::IsTrue(p.Acquire(100, Dir::Ahead, 1, badPl) == Got::Ok);
				const size_t n = static_cast<size_t>(badPl->w) * static_cast<size_t>(badPl->h);
				for (size_t idx = 0; idx < n; ++idx) {
					Assert::IsTrue(std::isfinite(badPl->dx[idx]), L"dx must remain finite");
					Assert::IsTrue(std::isfinite(badPl->dy[idx]), L"dy must remain finite");
					Assert::IsTrue(std::isfinite(badPl->trust[idx]), L"trust must remain finite");
					Assert::IsTrue(std::isfinite(badPl->unc[idx]), L"unc must remain finite");
					Assert::AreEqual(0.0, (double)badPl->trust[idx], 0.0, L"non-finite trust must be rejected to 0");
					Assert::AreEqual(0.0, (double)badPl->dx[idx], 0.0, L"rejected non-finite step must not write dx");
					Assert::AreEqual(0.0, (double)badPl->dy[idx], 0.0, L"rejected non-finite step must not write dy");
				}
			}
		}

		// GEM-134: Prepare passes plane k - 1 via GetWithPrev so degraded (uncached) chains of K = 6 hops stay strictly
		// O(K) instead of O(K^2), and Invalidate() bumps the epoch so stale prev hints from an older epoch are rejected.
		TEST_METHOD(PrepareUsesGetWithPrevForLinearWorkOnDegradedChain)
		{
			constexpr uint16_t kHops = 6;

			// Counting pair source where every reverse pair in [100 - kHops, 100 + kHops] is absent, forcing uncached degraded planes.
			class CountingDegradedSource : public FakeSource
			{
			public:
				mutable uint64_t servedCalls = 0;
				bool Adjacent(uint64_t from, uint64_t to, PairField& out, std::string* why) const override
				{
					const bool ok = FakeSource::Adjacent(from, to, out, why);
					if (ok)
						++servedCalls;
					return ok;
				}
			} src;

			for (uint64_t k = 1; k <= kHops; ++k) {
				// Back hop k needs forward (101 - k) -> (100 - k); reverse is (100 - k) -> (101 - k).
				src.missing[std::make_pair(100 - k, 101 - k)] = true;
				// Ahead hop k needs forward (99 + k) -> (100 + k); reverse is (100 + k) -> (99 + k).
				src.missing[std::make_pair(100 + k, 99 + k)] = true;
			}

			auto configure = [kHops](Product& prod) {
				prod.SetSourceSize(kW, kH);
				prod.Activate(Reader::P10gGrainLevel, true);
				prod.Activate(Reader::TemporalDetail, true);
				prod.SetWindow(kHops, kHops);   // plan: 6 back + 6 ahead
			};

			// Protected path: Prepare uses GetWithPrev to thread plane k - 1 -> strictly O(K) work.
			Product linear(&src);
			configure(linear);
			const Reached r = linear.Prepare(100);
			Assert::AreEqual((int)kHops, (int)r.back);
			Assert::AreEqual((int)kHops, (int)r.ahead);
			Assert::AreEqual((int)kHops, (int)r.degradedBack);
			Assert::AreEqual((int)kHops, (int)r.degradedAhead);
			Assert::AreEqual(static_cast<size_t>(0), linear.PlaneCount(), L"degraded planes remain uncached");
			Assert::AreEqual(static_cast<uint64_t>(2 * kHops), linear.GetStats().planesComputed,
				L"Prepare must compute strictly K back + K ahead planes (linear O(K))");
			Assert::AreEqual(static_cast<uint64_t>(2 * kHops), src.servedCalls,
				L"Prepare must evaluate strictly K forward + K backward source pairs (linear O(K))");

			// Same-input negative control (Rule 3): naive unhinted Acquire(1..K) loop on the exact same degraded chain
			// performs K*(K+1)/2 = 21 computes per direction (42 total, O(K^2)).
			src.calls = 0;
			src.servedCalls = 0;
			Product naive(&src);
			configure(naive);
			for (uint16_t k = 1; k <= kHops; ++k) {
				PlanePtr pb, pa;
				Assert::IsTrue(naive.Acquire(100, Dir::Back, k, pb) == Got::Ok);
				Assert::IsTrue(naive.Acquire(100, Dir::Ahead, k, pa) == Got::Ok);
			}
			const uint64_t quadraticPerDir = static_cast<uint64_t>(kHops) * (kHops + 1u) / 2u;   // 21
			Assert::AreEqual(2u * quadraticPerDir, naive.GetStats().planesComputed,
				L"negative control: unhinted Acquire loop does O(K^2) = 42 plane computes");
			Assert::AreEqual(2u * quadraticPerDir, src.servedCalls,
				L"negative control: unhinted Acquire loop does O(K^2) = 42 pair evals");
			Assert::IsTrue(linear.GetStats().planesComputed < naive.GetStats().planesComputed);

			// Epoch invalidation guard: Invalidate() bumps epoch so a stale prev hint from epoch 0 (matching seq, dir, k-1,
			// step, and geometry, differing ONLY in epoch) MUST be rejected and never corrupt the new epoch's composition.
			src.fwdDx = 1.0;
			Product epochProd(&src);
			configure(epochProd);
			PlanePtr stalePrev;
			Assert::IsTrue(epochProd.Acquire(100, Dir::Ahead, 1, stalePrev) == Got::Ok);
			Assert::AreEqual(static_cast<uint64_t>(0), stalePrev->epoch);
			Assert::AreEqual(1.0, (double)Product::Value(stalePrev, 64.0, 32.0).dx, 1e-3);

			// Epoch boundary: motion field changes from +1 px/step to +5 px/step and Product is invalidated.
			src.fwdDx = 5.0;
			epochProd.Invalidate();
			Assert::AreEqual(static_cast<uint64_t>(1), epochProd.GetStats().invalidations);

			const uint64_t computedBeforeStale = epochProd.GetStats().planesComputed;
			PlanePtr afterInvalid;
			Assert::IsTrue(epochProd.Acquire(100, Dir::Ahead, 2, afterInvalid, nullptr, stalePrev) == Got::Ok);
			Assert::AreEqual(computedBeforeStale + 2u, epochProd.GetStats().planesComputed,
				L"stale prev hint from pre-Invalidate epoch must be rejected, forcing fresh k=1 and k=2 computes");
			Assert::AreEqual(static_cast<uint64_t>(1), afterInvalid->epoch);
			// If stalePrev (dx=1) had been accepted, k=2 would be 1 + 5 = 6 instead of 5 + 5 = 10.
			Assert::AreEqual(10.0, (double)Product::Value(afterInvalid, 64.0, 32.0).dx, 1e-3,
				L"rejected stale hint yields fresh epoch displacement 10.0, not stale-mixed 6.0");

			// Positive control in the new epoch: a fresh epoch-1 prev hint IS accepted (only 1 compute for k=2).
			PlanePtr freshPrev;
			Assert::IsTrue(epochProd.Acquire(100, Dir::Ahead, 1, freshPrev) == Got::Ok);
			Assert::AreEqual(static_cast<uint64_t>(1), freshPrev->epoch);
			const uint64_t computedBeforeFresh = epochProd.GetStats().planesComputed;
			PlanePtr usingFresh;
			Assert::IsTrue(epochProd.Acquire(100, Dir::Ahead, 2, usingFresh, nullptr, freshPrev) == Got::Ok);
			Assert::AreEqual(computedBeforeFresh + 1u, epochProd.GetStats().planesComputed,
				L"current-epoch prev hint is accepted with strictly 1 compute");
			Assert::AreEqual(10.0, (double)Product::Value(usingFresh, 64.0, 32.0).dx, 1e-3);
		}
	};
}
