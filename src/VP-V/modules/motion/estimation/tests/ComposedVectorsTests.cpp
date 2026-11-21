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

#include <chrono>
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
	};
}
