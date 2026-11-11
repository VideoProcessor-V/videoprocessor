/*
 * Copyright(C) 2026 VP-V contributors
 * SPDX-License-Identifier: GPL-3.0-only
 */
// SID r2 safety rules (E0-validated, default OFF): R2 conventional geometry only, R3 convention-pair prior, R5 asymmetric latch, R7 no latch
// into a menu, R5 ABR switch. CPU only, synthetic only, ASCII literals. The latch is tested as a pure unit (fed the same quantities the
// estimator derives from ChainEvidence: the per-evaluation CREDITS of the latched size, r2 fix M1), the evidence rules through ChainEvidence
// itself, and the latch over a real ChainEvidence (M1). The estimator-level tests (H1, off identity, R4 neutrality, MenuHold coalescing,
// the visible resets) are ChainSafetyEstimatorTests in ChainUnitTests.cpp (they share its synthetic scenes).
#include "pch.h"
#include "CppUnitTest.h"

#include <VP-V/modules/input/source_analysis/identity/chain/ChainEvidence.h>
#include <VP-V/modules/input/source_analysis/identity/chain/ChainLatch.h>
#include <VP-V/modules/input/source_analysis/identity/chain/DescaleVerifier.h>
#include <VP-V/modules/scaling/geometry/SourceGeometrySet.h>

#include <cstdint>
#include <string>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace Tests
{
	namespace SafetyFx
	{
		using namespace SourceIdentity::Chain;

		constexpr int kRasterW = 3840, kRasterH = 2160;

		// Hand-made verifier result: 32 clean dip lines of detail `base` codes (same shape as the evidence tests).
		inline CandidateResult Cand(int n, double lineStat, double base)
		{
			CandidateResult c;
			c.nProposed = n;
			c.n = n;
			c.valid = true;
			c.exactDone = true;
			c.kernel = 5;   // Lanczos3 in the kernel bank (ChainUnitTests.cpp names it the same way)
			c.family = static_cast<int>(Family::Lanczos);
			c.stat = lineStat;
			c.lineCount = 32;
			c.segCount = 16;
			c.lineStat.assign(32, lineStat);
			c.lineBase.assign(32, base);
			c.lineRms.assign(32, 0.3);
			c.segSum.assign(32 * 16, 1.0);
			c.segRows.assign(32 * 16, kSegPx);
			c.segStat.assign(32 * 16, lineStat);
			c.segBase.assign(32 * 16, base);
			return c;
		}

		inline AxisObservation Obs(const VerifyResult& vr, int axisLen, uint32_t shot, int64_t timeUs)
		{
			AxisObservation o;
			o.verify = &vr;
			o.axisLen = axisLen;
			o.floorCodes = 0.29;
			o.probeRan = true;
			o.probeSeen = false;
			o.shotId = shot;
			o.timeUs = timeUs;
			o.rasterW = kRasterW;   // the delivered UHD raster (R2 / R3 ratios, L3)
			o.rasterH = kRasterH;
			return o;
		}

		// Both axes read one candidate each over `shots` shots of three evaluations: x on a 3840 axis, y on a 2160 axis.
		inline void Feed(ChainEvidence& ev, int nx, int ny, int shots)
		{
			VerifyResult vx, vy;
			vx.candidates.push_back(Cand(nx, -1.2, 5.0));
			vy.candidates.push_back(Cand(ny, -1.2, 5.0));
			EvidenceUpdate u;
			int64_t t = 0;
			for (int shot = 1; shot <= shots; ++shot)
				for (int e = 0; e < 3; ++e)
				{
					ev.Submit(0, Obs(vx, 3840, static_cast<uint32_t>(shot), t), u);
					ev.Submit(1, Obs(vy, 2160, static_cast<uint32_t>(shot), t), u);
					t += 1000000;
				}
		}

		inline LatchSettings On()
		{
			LatchSettings s;
			s.enabled = true;
			return s;
		}

		// An evaluation with a confident pair nx x ny on the 3840x2160 raster, evidence `llr` on each axis; `gain` = the NEW evidence this
		// evaluation credited to the latched size (M1; positive raw increments behind it, dated now).
		inline LatchInput Pair(int64_t tSec, int nx, int ny, double llr, double gain, bool informative = true)
		{
			LatchInput in;
			in.timeUs = tSec * 1000000;
			in.c1 = 15.0;
			in.informative = informative;
			in.havePair = true;
			in.nx = nx; in.Nx = 3840; in.ny = ny; in.Ny = 2160;
			in.llrX = in.llrY = llr;
			in.rasterW = kRasterW;
			in.rasterH = kRasterH;
			in.gainX = in.gainY = gain;
			in.rawX = in.rawY = gain > 0.0 ? gain : 0.0;
			in.gainTimeX = in.gainTimeY = gain > 0.0 ? in.timeUs : -1;
			return in;
		}

		inline LatchInput Pair1080(int64_t tSec, double llr, double gain, bool informative = true)
		{
			return Pair(tSec, 1920, 1080, llr, gain, informative);
		}

		// An evaluation that carries no upscaled pair (the readers see nothing confident); `gain` as in Pair.
		inline LatchInput Quiet(int64_t tSec, double gain, bool informative)
		{
			LatchInput in;
			in.timeUs = tSec * 1000000;
			in.c1 = 15.0;
			in.informative = informative;
			in.rasterW = kRasterW;
			in.rasterH = kRasterH;
			in.gainX = in.gainY = gain;
			in.rawX = in.rawY = gain > 0.0 ? gain : 0.0;
			in.gainTimeX = in.gainTimeY = gain > 0.0 ? in.timeUs : -1;
			return in;
		}

		// The latch over a REAL ChainEvidence, wired the way ChainEstimator::ApplyLatch wires it (the pair from the two summaries, the
		// credits of the latched size from CreditAtN).
		struct EvidenceLatch
		{
			ChainEvidence ev;
			UpscaleLatch latch;
			VerifyResult vx, vy;

			EvidenceLatch(const EvidenceSettings& s, int nx, int ny) : ev(s), latch(On())
			{
				vx.candidates.push_back(Cand(nx, -1.2, 5.0));
				vy.candidates.push_back(Cand(ny, -1.2, 5.0));
			}

			// `boundary`: a cut / black / fade was consumed with this evaluation (as ApplyLatch sets it from the snapshot's event flags).
			LatchOutput Step(int64_t tSec, uint32_t shot, bool boundary = false)
			{
				const int64_t t = tSec * 1000000;
				ev.ClearCredits();
				EvidenceUpdate ux, uy;
				ev.Submit(0, Obs(vx, 3840, shot, t), ux);
				ev.Submit(1, Obs(vy, 2160, shot, t), uy);
				LatchInput in;
				in.timeUs = t;
				in.c1 = ev.GetThresholds().c1;
				in.informative = ux.informative || uy.informative;
				in.rasterW = kRasterW;
				in.rasterH = kRasterH;
				in.shotBoundary = boundary;
				const AxisSummary sx = ev.Summary(0), sy = ev.Summary(1);
				in.havePair = sx.aboveC1 && sy.aboveC1;
				if (in.havePair)
				{
					in.nx = sx.n; in.Nx = 3840; in.ny = sy.n; in.Ny = 2160;
					in.llrX = sx.llrWithPrior;
					in.llrY = sy.llrWithPrior;
				}
				if (latch.Latched())
				{
					in.gainX = ev.CreditAtN(0, latch.LatchedNx(), &in.gainTimeX, &in.rawX);
					in.gainY = ev.CreditAtN(1, latch.LatchedNy(), &in.gainTimeY, &in.rawY);
				}
				return latch.Step(in);
			}
		};
	}

	TEST_CLASS(ChainSafetyRulesTests)
	{
	public:
		TEST_METHOD(TheConventionSetListsTheEqualRatiosAndTheNonSquarePairs)
		{
			namespace G = Scaling::GeometrySet;
			bool parked = false;
			Assert::IsTrue(G::ConventionalPair(1920, 3840, 1080, 2160, 3840, 2160, &parked) != nullptr && !parked);
			Assert::IsTrue(G::ConventionalPair(2560, 3840, 1440, 2160, 3840, 2160) != nullptr);
			Assert::IsTrue(G::ConventionalPair(720, 3840, 576, 2160, 3840, 2160) != nullptr, L"720x576 is a non-square pair");
			Assert::IsTrue(G::ConventionalPair(720, 3840, 480, 2160, 3840, 2160) != nullptr);
			Assert::IsTrue(G::ConventionalPair(1440, 3840, 1080, 2160, 3840, 2160, &parked) != nullptr && parked, L"anamorphic 1440x1080 is parked");
			Assert::IsTrue(G::ConventionalPair(1920, 3840, 720, 2160, 3840, 2160) == nullptr, L"two listed axis ratios of different geometries are no pair");
			Assert::IsTrue(G::ConventionalAxis(1920, 3840, 0, 3840, 2160));
			Assert::IsFalse(G::ConventionalAxis(2500, 3840, 0, 3840, 2160));
		}

		// L3: the sizes are absolute, the ratios come from the delivered raster (a 1080p raster lists other ratios than a UHD one).
		TEST_METHOD(TheRatiosFollowTheDeliveredRaster)
		{
			namespace G = Scaling::GeometrySet;
			Assert::IsTrue(G::ConventionalPair(1280, 1920, 720, 1080, 1920, 1080) != nullptr, L"1280x720 on a 1080p raster (2/3)");
			Assert::IsTrue(G::ConventionalPair(960, 1920, 540, 1080, 1920, 1080) != nullptr, L"960x540 on a 1080p raster (1/2)");
			Assert::IsTrue(G::ConventionalPair(720, 1920, 576, 1080, 1920, 1080) != nullptr, L"720x576 on a 1080p raster (0.375 x 0.533)");
			Assert::IsTrue(G::ConventionalPair(1280, 3840, 720, 2160, 1920, 1080) == nullptr, L"1/3 is no conventional ratio on a 1080p raster");
			Assert::IsTrue(G::ConventionalPair(1280, 3840, 720, 2160, 3840, 2160) != nullptr, L"... but is on a UHD raster");
			Assert::IsFalse(G::ConventionalAxis(1280, 1920, 0, 1920, 0), L"an unknown raster: nothing is conventional");
			Assert::IsTrue(G::ConventionalPair(1920, 3840, 1080, 2160, 0, 0) == nullptr, L"an unknown raster: no pair");
			// A letterboxed scope picture: the measured vertical axis is the active height, the ratio pair is still one geometry.
			Assert::IsTrue(G::ConventionalPair(1998, 3840, 1080, 2076, 3840, 2160) != nullptr, L"DCI flat letterboxed on a UHD raster");
			double rx = 0.0, ry = 0.0;
			std::size_t count = 0;
			const G::Geometry* list = G::Geometries(count);
			for (std::size_t i = 0; i < count; ++i)
				if (list[i].w == 2560 && list[i].h == 1440)
					Assert::IsFalse(G::RatiosFor(list[i], 1920, 1080, rx, ry), L"2560x1440 is no upscaled source on a 1080p raster");
		}

		// L2: the parked anamorphic entry is not single-axis evidence unless asked for; the listed 720 stays.
		TEST_METHOD(AParkedEntryIsNoAxisEvidence)
		{
			namespace G = Scaling::GeometrySet;
			Assert::IsFalse(G::ConventionalAxis(1440, 3840, 0, 3840, 2160), L"0.375 is only the parked 1440x1080's ratio");
			Assert::IsTrue(G::ConventionalAxis(1440, 3840, 0, 3840, 2160, true), L"admitted when parked entries are allowed");
			Assert::IsTrue(G::ConventionalAxis(720, 3840, 0, 3840, 2160), L"720 (720x576 / 720x480) stays conventional");
		}

		// R2: a non-conventional candidate is no evidence; the latch layer reports it unsure (heldBack, visible).
		TEST_METHOD(ANonConventionalCandidateGivesUnsure)
		{
			using namespace SafetyFx;
			EvidenceSettings s;
			s.conventionOnly = true;
			ChainEvidence ev(s);
			Feed(ev, 2500, 1400, 6);
			AxisSummary sx = ev.Summary(0);
			Assert::IsFalse(sx.aboveC1, L"no published geometry: never confident");
			Assert::IsTrue(sx.llr <= 0.0 || !sx.hasBest);

			ChainEvidence plain;
			Feed(plain, 2500, 1400, 6);
			Assert::IsTrue(plain.Summary(0).hasBest && plain.Summary(0).n == 2500, L"switch off: the old behaviour");

			UpscaleLatch latch(On());
			LatchInput in = Pair1080(0, 40.0, 0.0);
			in.nx = 2500; in.ny = 1400;
			LatchOutput o = latch.Step(in);
			Assert::IsFalse(o.latched);
			Assert::IsTrue(o.heldBack && o.nonConventional);
			Assert::IsTrue((o.reasonBits & kReasonNonConventional) != 0);
			Assert::AreEqual(static_cast<uint64_t>(1), latch.Counters().nonConventional);

			UpscaleLatch noRaster(On());
			LatchInput blind = Pair1080(0, 40.0, 0.0);
			blind.rasterW = blind.rasterH = 0;
			Assert::IsFalse(noRaster.Step(blind).latched, L"L3: an unknown raster has no conventional pair");
		}

		// R3: per-axis false readings (each a listed ratio alone) never form a pair; the prior is the convention pair.
		TEST_METHOD(NativePerAxisFalseReadingsNeverFormAPair)
		{
			using namespace SafetyFx;
			EvidenceSettings s;
			s.pairPrior = true;
			ChainEvidence mismatch(s);
			Feed(mismatch, 1920, 720, 6);   // x 1/2, y 1/3: each a listed ratio on its own axis, not one geometry
			AxisSummary a = mismatch.Summary(0);
			Assert::IsTrue(a.prior < 0.0, L"a mismatched pair is counted against");
			Assert::AreEqual(a.llr + a.prior, a.llrWithPrior, 1e-9);
			// M5 (restored): whatever the per-axis strength, a mismatched pair is never confident on BOTH axes.
			Assert::IsFalse(a.aboveC1 && mismatch.Summary(1).aboveC1);

			ChainEvidence pair(s);
			Feed(pair, 1920, 1080, 6);
			Assert::IsTrue(pair.Summary(0).prior > 0.0, L"the listed pair earns the bonus");
			Assert::IsTrue(pair.Summary(0).llrWithPrior > mismatch.Summary(0).llrWithPrior + 3.0, L"a listed pair outweighs a mismatched one by the full prior swing");

			ChainEvidence nonSquare(s);
			Feed(nonSquare, 720, 576, 6);
			Assert::IsTrue(nonSquare.Summary(0).prior > 0.0, L"a non-square listed pair agrees too");

			UpscaleLatch latch(On());
			LatchInput in = Pair1080(0, 40.0, 0.0);
			in.ny = 720;
			Assert::IsFalse(latch.Step(in).latched, L"the latch refuses an unlisted pair");
		}

		// M5: strong per-axis evidence of a mismatched pair (each axis above c1 + bonus on its own) is vetoed on both axes.
		TEST_METHOD(StrongMismatchedEvidenceIsVetoedOnBothAxes)
		{
			using namespace SafetyFx;
			EvidenceSettings s;
			s.pairPrior = true;
			ChainEvidence ev(s);
			Feed(ev, 1920, 720, 12);
			const AxisSummary x = ev.Summary(0), y = ev.Summary(1);
			Assert::IsFalse(x.aboveC1, L"the veto: not confident");
			Assert::IsFalse(y.aboveC1);
			Assert::IsTrue(x.llrWithPrior >= ev.GetThresholds().c1 && y.llrWithPrior >= ev.GetThresholds().c1,
				L"fixture: each axis alone is strong enough that only the veto holds it back");
		}

		// L8: the R3 prior is the convention PAIR, not the old ratio agreement: equal unlisted ratios agreed before, never with the rule.
		TEST_METHOD(EqualUnlistedRatiosAgreedBeforeButNotUnderThePairPrior)
		{
			using namespace SafetyFx;
			ChainEvidence old;
			Feed(old, 1440, 810, 6);   // 0.375 x 0.375: one ratio on both axes, no listed geometry
			Assert::IsTrue(old.Summary(0).prior > 0.0, L"the old ratio agreement counted it for");

			EvidenceSettings s;
			s.pairPrior = true;
			ChainEvidence rule(s);
			Feed(rule, 1440, 810, 6);
			Assert::IsTrue(rule.Summary(0).prior < 0.0, L"the pair prior counts it against");
		}

		// L1: the parked anamorphic pair is listed but does not agree (no prior bonus, no latch).
		TEST_METHOD(TheParkedPairEarnsNoBonusAndNoLatch)
		{
			using namespace SafetyFx;
			EvidenceSettings s;
			s.pairPrior = true;
			ChainEvidence ev(s);
			Feed(ev, 1440, 1080, 6);
			Assert::IsTrue(ev.Summary(0).prior < 0.0, L"1440x1080 is parked: counted against");
			Assert::IsFalse(ev.Summary(0).aboveC1 && ev.Summary(1).aboveC1);

			UpscaleLatch latch(On());
			const LatchOutput o = latch.Step(Pair(0, 1440, 1080, 40.0, 0.0));
			Assert::IsFalse(o.latched);
			Assert::IsTrue(o.nonConventional, L"held back visibly");
		}

		// R5: an injected confident-wrong burst on a native stream reaches the bound within 60 s; N1: its release waits for the next
		// cut / black / fade and happens at the end of the new shot's 3 s dwell (no re-confirmation); native evidence releases at once.
		TEST_METHOD(AConfidentWrongBurstOnNativeReleasesWithinTheBound)
		{
			using namespace SafetyFx;
			UpscaleLatch latch(On());
			Assert::IsFalse(latch.Step(Quiet(0, 0.0, true)).latched, L"native is sticky");
			LatchOutput o = latch.Step(Pair1080(1, 40.0, 40.0));   // the burst
			Assert::IsTrue(o.latched && o.event == LatchEvent::Entered);
			int64_t deferredAt = -1;
			for (int64_t t = 2; t < 200 && deferredAt < 0; ++t)
			{
				o = latch.Step(Quiet(t, 0.0, true));   // the burst ends: no new evidence of the latched size, evaluations are informative
				Assert::IsTrue(o.latched, L"N1: the bound never changes the picture by itself");
				if (o.deferredNow)
					deferredAt = t;
			}
			Assert::IsTrue(deferredAt > 0 && deferredAt <= 63, L"the 60 s bound is reached (and counted) within the bound");
			Assert::IsTrue(o.deferred && latch.Deferred() && o.event == LatchEvent::None);
			Assert::AreEqual(static_cast<uint64_t>(1), latch.Counters().releasesBound, L"the bound reached is counted at once");
			Assert::AreEqual(static_cast<uint64_t>(1), latch.Counters().deferred);
			Assert::AreEqual(static_cast<uint64_t>(0), latch.Counters().releases);
			for (int64_t t = deferredAt + 1; t < deferredAt + 7; ++t)
				Assert::IsTrue(latch.Step(Quiet(t, 0.0, true)).latched, L"held until a shot boundary");
			LatchInput cut = Quiet(deferredAt + 7, 0.0, true);
			cut.shotBoundary = true;
			o = latch.Step(cut);
			Assert::IsTrue(o.latched && latch.Dwelling(), L"the cut opens the new shot's dwell");
			int64_t releasedAt = -1;
			for (int64_t t = deferredAt + 8; t < deferredAt + 20 && releasedAt < 0; ++t)
			{
				o = latch.Step(Quiet(t, 0.0, true));
				if (!o.latched)
					releasedAt = t;
			}
			// N2 (LOW-b): the dwell's end (deferredAt + 10, the first credited evaluation of the new shot) plus one evaluation (1 s steps).
			Assert::IsTrue(releasedAt == deferredAt + 11, L"released one evaluation after the end of the 3 s dwell (no re-confirmation)");
			Assert::IsTrue(o.event == LatchEvent::Released && (o.reasonBits & kReasonLatchRelease) != 0);
			Assert::AreEqual(static_cast<uint64_t>(1), latch.Counters().releasesAtCut);
			Assert::AreEqual(static_cast<uint64_t>(1), latch.Counters().releases);
			Assert::AreEqual(static_cast<uint64_t>(1), latch.Counters().releasesBound, L"the release itself is not counted a second time");
			Assert::IsFalse(latch.Deferred());

			UpscaleLatch fast(On());
			fast.Step(Pair1080(0, 40.0, 40.0));
			LatchInput nat = Quiet(1, 0.0, true);
			nat.nativeBoth = true;
			Assert::IsTrue(fast.Step(nat).event == LatchEvent::ReleasedNative);
			Assert::AreEqual(static_cast<uint64_t>(1), fast.Counters().releasesNative);
		}

		TEST_METHOD(ATrueUpscaleStaysLatchedThroughNormalContent)
		{
			using namespace SafetyFx;
			UpscaleLatch latch(On());
			Assert::IsTrue(latch.Step(Pair1080(0, 40.0, 40.0)).latched);
			// Normal content: new evidence (1.0 per evaluation: a confirmation at 0.25 c1 = 3.75 every 4 s), dark / flat stretches of 100 s
			// with no evidence at all.
			for (int64_t t = 1; t <= 600; ++t)
			{
				const bool dark = (t % 200) >= 50 && (t % 200) < 150;
				LatchOutput o = latch.Step(dark ? Quiet(t, 0.0, false) : Quiet(t, 1.0, true));
				Assert::IsTrue(o.latched, L"a true upscale is never flickered off");
			}
			Assert::AreEqual(static_cast<uint64_t>(0), latch.Counters().releases);
			Assert::IsTrue(latch.Counters().confirms > 50, L"re-confirmed by its new evidence");
		}

		// M1: evidence that merely still sits in the window never re-confirms; only NEW (credited) evidence does.
		TEST_METHOD(OnlyNewEvidenceReconfirms)
		{
			using namespace SafetyFx;
			UpscaleLatch latch(On());
			latch.Step(Pair1080(0, 40.0, 40.0));
			LatchOutput o;
			for (int64_t t = 1; t <= 3; ++t)
			{
				o = latch.Step(Quiet(t, 1.0, true));
				Assert::IsFalse(o.confirmed, L"1 + 1 + 1 < 3.75");
			}
			o = latch.Step(Quiet(4, 1.0, true));
			Assert::IsTrue(o.confirmed, L"the fourth increment confirms");
			Assert::AreEqual(static_cast<uint64_t>(1), latch.Counters().confirms);
			// A large windowed total but no new credit: the old rule (windowed total >= 0.25 c1) re-confirmed every evaluation.
			LatchInput stale = Pair1080(5, 40.0, 0.0);
			o = latch.Step(stale);
			Assert::IsFalse(o.confirmed, L"llr in the window is not new evidence");
			// A negative credit is a debt: it must be earned back first.
			latch.Step(Quiet(6, -3.0, true));
			o = latch.Step(Quiet(7, 4.0, true));
			Assert::IsFalse(o.confirmed, L"-3 + 4 < 3.75");
			o = latch.Step(Quiet(8, 3.0, true));
			Assert::IsTrue(o.confirmed);
		}

		// M1 over a real ChainEvidence: a true upscale inside one very long shot. The shot cap absorbs its increments, so after the first
		// seconds nothing new is credited although the window still holds the shot's evidence; the release clock runs slow (consistent
		// evidence), and the hard bound releases about 120 s after the last NEW evidence.
		TEST_METHOD(ALongCappedShotIsNotNewEvidenceAndTheHardBoundReleases)
		{
			using namespace SafetyFx;
			EvidenceSettings s;
			s.conventionOnly = true;
			s.pairPrior = true;
			EvidenceLatch el(s, 1920, 1080);
			const double c1 = el.ev.GetThresholds().c1;
			int64_t t = 0;
			bool entered = false;
			for (uint32_t shot = 1; shot <= 6; ++shot)
				for (int e = 0; e < 3; ++e, ++t)
					entered = el.Step(t, shot).latched || entered;
			Assert::IsTrue(entered && el.latch.Latched(), L"fixture: six shots enter the latch");
			int64_t lastConfirmAt = -1, releasedAt = -1, deferredAt = -1;
			LatchOutput o;
			bool checkedMid = false;
			for (; t < 18 + 400 && releasedAt < 0; ++t)
			{
				o = el.Step(t, 7u);   // one shot of 400 s
				if (o.confirmed)
					lastConfirmAt = t;
				if (o.deferredNow)
					deferredAt = t;
				if (!o.latched)
					releasedAt = t;
				if (t == 118)   // 100 s into the long shot
				{
					checkedMid = true;
					Assert::AreEqual(0.0, el.ev.CreditAtN(0, 1920), 1e-12, L"nothing new is credited inside the capped shot");
					Assert::IsTrue(el.ev.LlrAtN(0, 1920) >= 0.25 * c1, L"although the window still holds more than the old confirm level");
					Assert::IsTrue(o.latched, L"still latched at 100 s into the shot");
				}
			}
			Assert::IsTrue(checkedMid);
			Assert::IsTrue(lastConfirmAt >= 18 && lastConfirmAt <= 30, L"the new shot confirmed once its dwell passed, then never again");
			Assert::IsTrue(deferredAt - lastConfirmAt >= 115 && deferredAt - lastConfirmAt <= 125, L"the hard bound is reached ~120 s after the newest new evidence");
			Assert::AreEqual(static_cast<uint64_t>(1), el.latch.Counters().releasesHardBound);
			Assert::AreEqual(static_cast<uint64_t>(0), el.latch.Counters().releasesBound, L"the slow clock did not reach its bound first");
			// N1: no cut follows inside the shot, so the no-cut ceiling releases 60 s later (still: no lock without release).
			Assert::IsTrue(releasedAt > 0, L"the latch is released (no lock without release)");
			Assert::IsTrue(o.event == LatchEvent::ReleasedCeiling, L"by the no-cut ceiling");
			Assert::IsTrue(releasedAt - deferredAt == 60, L"60 s after the bound");
			Assert::IsTrue(releasedAt - lastConfirmAt <= 185, L"at most ~180 s after the newest new evidence");
			Assert::AreEqual(static_cast<uint64_t>(1), el.latch.Counters().releasesCeiling);
			Assert::AreEqual(static_cast<uint64_t>(1), el.latch.Counters().releases);
		}

		// N1 over a real ChainEvidence: a TRUE upscale in one very long capped shot reaches the hard bound; the next cut opens the new
		// shot's dwell, the new shot's evidence (credited once ChainEvidence's 3 s shot dwell passed) re-confirms it, and the deferred
		// release is CANCELLED: the picture never changes. Without deferral (the pre-N1 rule) it would have been released mid-shot.
		TEST_METHOD(ATrueUpscaleReconfirmsInTheNewShotAndTheDeferredReleaseIsCancelled)
		{
			using namespace SafetyFx;
			EvidenceSettings s;
			s.conventionOnly = true;
			s.pairPrior = true;
			EvidenceLatch el(s, 1920, 1080);
			int64_t t = 0;
			for (uint32_t shot = 1; shot <= 6; ++shot)
				for (int e = 0; e < 3; ++e, ++t)
					el.Step(t, shot, e == 0);
			Assert::IsTrue(el.latch.Latched(), L"fixture: six shots enter the latch");
			int64_t deferredAt = -1;
			for (; t < 18 + 300 && deferredAt < 0; ++t)
			{
				const LatchOutput o = el.Step(t, 7u, t == 18);
				Assert::IsTrue(o.latched, L"never released mid-shot");
				if (o.deferredNow)
					deferredAt = t;
			}
			Assert::IsTrue(deferredAt > 0 && el.latch.Deferred(), L"the hard bound is reached inside the long shot");
			for (int64_t k = 0; k < 20; ++k, ++t)
				Assert::IsTrue(el.Step(t, 7u).latched, L"the long shot goes on: held, deferred");
			bool cancelled = false;
			for (int64_t k = 0; k < 30; ++k, ++t)
			{
				const LatchOutput o = el.Step(t, 8u, k == 0);   // a cut: shot 8 (the same true upscale)
				Assert::IsTrue(o.latched, L"a true upscale never changes the picture");
				cancelled = o.deferCancelled || cancelled;
			}
			Assert::IsTrue(cancelled && !el.latch.Deferred(), L"re-confirmed in the new shot's dwell: the deferred release is cancelled");
			Assert::AreEqual(static_cast<uint64_t>(1), el.latch.Counters().deferCancelled);
			Assert::AreEqual(static_cast<uint64_t>(0), el.latch.Counters().releases);
			Assert::AreEqual(static_cast<uint64_t>(1), el.latch.Counters().releasesHardBound, L"the bound reached stays counted");
		}

		// N1: native, menu, ABR-step and reset releases stay immediate while a starvation release is deferred (and end the deferral);
		// deferStarvation = false is the pre-N1 rule (released at the bound).
		TEST_METHOD(TheOtherReleasesStayImmediateWhileDeferredAndDeferralCanBeOff)
		{
			using namespace SafetyFx;
			auto deferredLatch = [](UpscaleLatch& l) {
				l.Step(Pair1080(0, 40.0, 40.0));
				for (int64_t t = 1; t <= 70 && !l.Deferred(); ++t)
					l.Step(Quiet(t, 0.0, true));
				return l.Deferred();
			};
			{
				UpscaleLatch l(On());
				Assert::IsTrue(deferredLatch(l));
				LatchInput nat = Quiet(80, 0.0, true);
				nat.nativeBoth = true;
				const LatchOutput o = l.Step(nat);
				Assert::IsTrue(!o.latched && o.event == LatchEvent::ReleasedNative && !l.Deferred(), L"native: at once");
				Assert::AreEqual(static_cast<uint64_t>(1), l.Counters().releasesNative);
			}
			{
				UpscaleLatch l(On());
				Assert::IsTrue(deferredLatch(l));
				LatchInput menu = Quiet(80, 0.0, true);
				menu.menuHold = true;
				const LatchOutput o = l.Step(menu);
				Assert::IsTrue(!o.latched && o.event == LatchEvent::ReleasedMenu && !l.Deferred(), L"menu: at once");
			}
			{
				LatchSettings s = On();
				s.abrStepTrigger = true;
				UpscaleLatch l(s);
				Assert::IsTrue(deferredLatch(l));
				LatchInput abr = Quiet(80, 0.0, true);
				abr.abrStep = true;
				const LatchOutput o = l.Step(abr);
				Assert::IsTrue(!o.latched && o.event == LatchEvent::ReleasedAbr && !l.Deferred(), L"ABR step: at once");
			}
			{
				UpscaleLatch l(On());
				Assert::IsTrue(deferredLatch(l));
				const LatchOutput o = l.Reset();
				Assert::IsTrue(!o.latched && o.event == LatchEvent::ReleasedReset && !l.Deferred(), L"reset: at once");
			}
			{
				LatchSettings s = On();
				s.deferStarvation = false;
				UpscaleLatch l(s);
				l.Step(Pair1080(0, 40.0, 40.0));
				int64_t at = -1;
				LatchOutput o;
				for (int64_t t = 1; t <= 70 && at < 0; ++t)
				{
					o = l.Step(Quiet(t, 0.0, true));
					Assert::IsFalse(o.deferredNow);
					if (!o.latched)
						at = t;
				}
				Assert::IsTrue(at > 0 && at <= 63 && o.event == LatchEvent::Released, L"deferral off: released at the bound");
				Assert::AreEqual(static_cast<uint64_t>(1), l.Counters().releasesBound);
				Assert::AreEqual(static_cast<uint64_t>(0), l.Counters().deferred);
			}
			Assert::IsTrue(LatchSettings().deferStarvation, L"deferral is the default of the rules");
			Assert::AreEqual(3.0, LatchSettings().deferDwellSec, 1e-12);
			Assert::AreEqual(1, LatchSettings().deferDwellExtraEvaluations);
			Assert::AreEqual(60.0, LatchSettings().deferCeilingSec, 1e-12);
		}

		// N1: a boundary consumed with the very evaluation that reaches the bound opens the dwell at once; a re-confirmation BEFORE any
		// boundary also cancels the deferral (new evidence is new evidence).
		TEST_METHOD(ABoundaryOnTheBoundEvaluationOpensTheDwellAndAnEarlyConfirmCancels)
		{
			using namespace SafetyFx;
			UpscaleLatch a(On());
			a.Step(Pair1080(0, 40.0, 40.0));
			int64_t t = 1;
			for (; t <= 70 && !a.Deferred(); ++t)
			{
				LatchInput in = Quiet(t, 0.0, true);
				in.shotBoundary = true;   // every evaluation carries a cut
				a.Step(in);
			}
			Assert::IsTrue(a.Deferred() && a.Dwelling(), L"the bound's own boundary opens the dwell");
			const int64_t at = t - 1;
			Assert::IsTrue(a.Step(Quiet(at + 2, 0.0, true)).latched);
			Assert::IsTrue(a.Step(Quiet(at + 3, 0.0, true)).latched, L"N2: the dwell's end (3 s) is not yet the release");
			Assert::IsFalse(a.Step(Quiet(at + 4, 0.0, true)).latched, L"released one evaluation after the 3 s dwell from the bound evaluation");
			Assert::AreEqual(static_cast<uint64_t>(1), a.Counters().releasesAtCut);

			UpscaleLatch b(On());
			b.Step(Pair1080(0, 40.0, 40.0));
			for (t = 1; t <= 70 && !b.Deferred(); ++t)
				b.Step(Quiet(t, 0.0, true));
			Assert::IsTrue(b.Deferred() && !b.Dwelling());
			const LatchOutput o = b.Step(Quiet(t + 5, 10.0, true));   // new evidence of the latched size, no boundary yet
			Assert::IsTrue(o.latched && o.confirmed && o.deferCancelled && !b.Deferred());
			Assert::AreEqual(static_cast<uint64_t>(1), b.Counters().deferCancelled);
		}

		// N2 (LOW-a): every new shot inside the dwell restarts it (the release does not land inside the next of a run of short shots);
		// restarts stop once the ceiling (60 s after the bound) has passed, so cuts every 2 s still release, at the cut's dwell end.
		TEST_METHOD(ASecondCutInsideTheDwellRestartsItAndTheCeilingEndsTheRestarts)
		{
			using namespace SafetyFx;
			auto deferredAt = [](UpscaleLatch& l) {
				l.Step(Pair1080(0, 40.0, 40.0));
				int64_t t = 1;
				for (; t <= 70 && !l.Deferred(); ++t)
					l.Step(Quiet(t, 0.0, true));
				return l.Deferred() ? t - 1 : -1;
			};
			auto cutAt = [](int64_t t) {
				LatchInput in = Quiet(t, 0.0, true);
				in.shotBoundary = true;
				return in;
			};
			{
				UpscaleLatch l(On());
				const int64_t d = deferredAt(l);
				Assert::IsTrue(d > 0 && !l.Dwelling());
				Assert::IsTrue(l.Step(cutAt(d + 1)).latched && l.Dwelling(), L"the first cut opens the dwell");
				Assert::IsTrue(l.Step(Quiet(d + 2, 0.0, true)).latched);
				Assert::IsTrue(l.Step(cutAt(d + 3)).latched, L"a second cut 2 s into the dwell restarts it");
				// Without the restart: dwell end d+4, release d+5. With it: dwell end d+3+3 = d+6, release one evaluation later, d+7.
				for (int64_t t = d + 4; t <= d + 6; ++t)
					Assert::IsTrue(l.Step(Quiet(t, 0.0, true)).latched, (L"held in the restarted dwell (t = d + " + std::to_wstring(t - d) + L")").c_str());
				const LatchOutput o = l.Step(Quiet(d + 7, 0.0, true));
				Assert::IsTrue(!o.latched && o.event == LatchEvent::Released, L"released at the restarted dwell's end + one evaluation");
				Assert::AreEqual(static_cast<uint64_t>(1), l.Counters().releasesAtCut);
			}
			{
				UpscaleLatch l(On());
				const int64_t d = deferredAt(l);
				int64_t releasedAt = -1;
				LatchOutput o;
				for (int64_t t = d + 2; t <= d + 120 && releasedAt < 0; ++t)
				{
					o = l.Step((t - d) % 2 == 0 ? cutAt(t) : Quiet(t, 0.0, true));   // a cut every 2 s from d + 2: no dwell ever completes
					if (!o.latched)
						releasedAt = t;
				}
				// The last restart is the cut at d + 58 (58 < 60); the cut at d + 60 comes after the ceiling (60 >= 60) and is ignored, so
				// the dwell from d + 58 ends at d + 61 and releases one evaluation later: d + 62, at a cut's dwell (not the ceiling).
				Assert::IsTrue(releasedAt == d + 62, (L"released by the last pre-ceiling dwell; at d + " + std::to_wstring(releasedAt - d)).c_str());
				Assert::IsTrue(o.event == LatchEvent::Released);
				Assert::AreEqual(static_cast<uint64_t>(1), l.Counters().releasesAtCut);
				Assert::AreEqual(static_cast<uint64_t>(0), l.Counters().releasesCeiling);
			}
		}

		// N1 / N2: a backward source-time step (L5) during a deferral re-anchors the ceiling and an open dwell to the new time.
		TEST_METHOD(ABackwardStepDuringADeferralReanchorsTheCeilingAndTheDwell)
		{
			using namespace SafetyFx;
			auto deferredAt = [](UpscaleLatch& l) {
				l.Step(Pair1080(0, 40.0, 40.0));
				int64_t t = 1;
				for (; t <= 70 && !l.Deferred(); ++t)
					l.Step(Quiet(t, 0.0, true));
				return l.Deferred() ? t - 1 : -1;
			};
			{
				// No boundary: the ceiling runs from the step-back time b. Without the re-anchor it would run from d (60 s before d + 60,
				// which source time reaches only 90 s after b = d - 30).
				UpscaleLatch l(On());
				const int64_t d = deferredAt(l);
				Assert::IsTrue(d > 30);
				Assert::IsTrue(l.Step(Quiet(d + 5, 0.0, true)).latched);
				const int64_t b = d - 30;
				Assert::IsTrue(l.Step(Quiet(b, 0.0, true)).latched && l.Deferred(), L"the backward step keeps the deferral");
				int64_t at = -1;
				LatchOutput o;
				for (int64_t t = b + 1; t <= b + 120 && at < 0; ++t)
				{
					o = l.Step(Quiet(t, 0.0, true));
					if (!o.latched)
						at = t;
				}
				Assert::IsTrue(at == b + 60, (L"the ceiling 60 s after the re-anchored time; at b + " + std::to_wstring(at - b)).c_str());
				Assert::IsTrue(o.event == LatchEvent::ReleasedCeiling);
			}
			{
				// An open dwell: the cut at d + 1 opens it; the step back to b = d - 20 re-anchors it, so it ends at b + 3 and releases at b + 4
				// (without the re-anchor the dwell would not reach 3 s until d + 4 = b + 24).
				UpscaleLatch l(On());
				const int64_t d = deferredAt(l);
				LatchInput cut = Quiet(d + 1, 0.0, true);
				cut.shotBoundary = true;
				Assert::IsTrue(l.Step(cut).latched && l.Dwelling());
				const int64_t b = d - 20;
				Assert::IsTrue(l.Step(Quiet(b, 0.0, true)).latched && l.Dwelling(), L"the backward step keeps the open dwell");
				for (int64_t t = b + 1; t <= b + 3; ++t)
					Assert::IsTrue(l.Step(Quiet(t, 0.0, true)).latched);
				const LatchOutput o = l.Step(Quiet(b + 4, 0.0, true));
				Assert::IsTrue(!o.latched && o.event == LatchEvent::Released, L"released at the re-anchored dwell's end + one evaluation");
				Assert::AreEqual(static_cast<uint64_t>(1), l.Counters().releasesAtCut);
			}
		}

		// R5b: through no-evidence stretches the release clock runs at 1/4 speed, but 120 s without confirmation always releases.
		TEST_METHOD(TheClockSlowsThroughNoEvidenceAndTheHardBoundAlwaysReleases)
		{
			using namespace SafetyFx;
			UpscaleLatch latch(On());
			latch.Step(Pair1080(0, 40.0, 40.0));
			for (int64_t t = 1; t <= 100; ++t)
				Assert::IsTrue(latch.Step(Quiet(t, 0.0, false)).latched);
			Assert::IsTrue(latch.ReleaseClockSec() < 26.0 && latch.ReleaseClockSec() > 24.0, L"100 s at a quarter speed");
			LatchOutput o;
			int64_t at = -1;
			for (int64_t t = 101; t <= 140 && at < 0; ++t)
			{
				o = latch.Step(Quiet(t, 0.0, false));
				if (!o.latched)
					at = t;
			}
			Assert::IsTrue(at < 0 && latch.Deferred(), L"N1: the hard bound is reached but deferred (no cut)");
			Assert::AreEqual(static_cast<uint64_t>(1), latch.Counters().releasesHardBound, L"reached one second past 120 s, counted as the hard bound");
			Assert::AreEqual(static_cast<uint64_t>(0), latch.Counters().releasesBound, L"not as a clock release");
			for (int64_t t = 141; t <= 200 && at < 0; ++t)
			{
				o = latch.Step(Quiet(t, 0.0, false));
				if (!o.latched)
					at = t;
			}
			Assert::IsTrue(at == 181, L"the no-cut ceiling releases 60 s after the hard bound (121 + 60)");
			Assert::IsTrue(o.event == LatchEvent::ReleasedCeiling);
			Assert::AreEqual(static_cast<uint64_t>(1), latch.Counters().releasesCeiling);
			Assert::AreEqual(static_cast<uint64_t>(1), latch.Counters().releases);
		}

		// L5: a backward source-time step without a reset re-anchors (neither the bar nor the hard bound freezes on a negative age).
		TEST_METHOD(ABackwardTimeStepReanchors)
		{
			using namespace SafetyFx;
			UpscaleLatch latch(On());
			Assert::IsTrue(latch.Step(Pair1080(100, 40.0, 40.0)).latched);
			latch.Step(Quiet(110, 0.0, false));
			Assert::IsTrue(latch.Step(Quiet(5, 0.0, false)).latched, L"the step back keeps the latch");
			Assert::AreEqual(15.0, latch.Bar(5000000, 15.0), 1e-9, L"the bar's stable time restarts at the new time");
			Assert::AreEqual(15.0 * (1.0 + 30.0 / 120.0), latch.Bar(65000000, 15.0), 1e-9);
			LatchOutput o;
			int64_t at = -1;
			int64_t boundAt = -1;
			for (int64_t t = 6; t <= 220 && at < 0; ++t)
			{
				o = latch.Step(Quiet(t, 0.0, false));
				if (o.deferredNow)
					boundAt = t;
				if (!o.latched)
					at = t;
			}
			Assert::IsTrue(boundAt == 126, L"the hard bound runs from the re-anchored time");
			Assert::AreEqual(static_cast<uint64_t>(1), latch.Counters().releasesHardBound);
			Assert::IsTrue(at == 186 && o.event == LatchEvent::ReleasedCeiling, L"N1: no cut, so the ceiling 60 s later");
		}

		// Moves between upscaled sizes: the same bar, a listed non-parked pair; anything else keeps the latched size.
		TEST_METHOD(AMoveNeedsTheBarAndAListedPair)
		{
			using namespace SafetyFx;
			UpscaleLatch latch(On());
			Assert::IsTrue(latch.Step(Pair1080(0, 40.0, 40.0)).latched);
			LatchOutput o = latch.Step(Pair(10, 1280, 720, 40.0, 0.0));
			Assert::IsTrue(o.event == LatchEvent::Moved && o.nx == 1280 && o.ny == 720, L"a strong listed pair moves");
			Assert::AreEqual(static_cast<uint64_t>(1), latch.Counters().moves);
			o = latch.Step(Pair(20, 1280, 1080, 40.0, 0.0));
			Assert::IsTrue(o.event == LatchEvent::None && o.nx == 1280 && o.ny == 720, L"an unlisted pair never moves the latch");
			o = latch.Step(Pair(25, 1440, 1080, 40.0, 0.0));
			Assert::IsTrue(o.event == LatchEvent::None && o.nx == 1280, L"nor does the parked pair");
			// 40 s stable since the move: the bar is 15 x (1 + 10 / 120) = 16.25.
			o = latch.Step(Pair(50, 2560, 1440, 16.0, 0.0));
			Assert::IsTrue(o.event == LatchEvent::None && o.nx == 1280, L"below the grown bar: no move");
			o = latch.Step(Pair(51, 2560, 1440, 17.0, 0.0));
			Assert::IsTrue(o.event == LatchEvent::Moved && o.nx == 2560 && o.ny == 1440, L"at the grown bar: a move");
			Assert::AreEqual(static_cast<uint64_t>(2), latch.Counters().moves);
			o = latch.Step(Pair(52, 2561, 1441, 40.0, 0.0));
			Assert::IsTrue(o.event == LatchEvent::None, L"a 1 px difference is the same size");
		}

		TEST_METHOD(TheBarGrowsWithStableTimeAndAnAbrStepIsASwitchOffByDefault)
		{
			using namespace SafetyFx;
			UpscaleLatch latch(On());
			latch.Step(Quiet(0, 0.0, true));
			Assert::AreEqual(15.0, latch.Bar(0, 15.0), 1e-9);
			Assert::AreEqual(15.0, latch.Bar(30000000, 15.0), 1e-9);
			Assert::AreEqual(30.0, latch.Bar(150000000, 15.0), 1e-9, L"the bar at most doubles");
			Assert::AreEqual(30.0, latch.Bar(900000000, 15.0), 1e-9);
			Assert::IsFalse(LatchSettings().abrStepTrigger);
			Assert::IsFalse(LatchSettings().enabled);

			UpscaleLatch off(On());
			off.Step(Pair1080(0, 40.0, 40.0));
			LatchInput abr = Pair1080(1, 0.0, 40.0);
			abr.havePair = false;
			abr.abrStep = true;
			Assert::IsTrue(off.Step(abr).latched, L"switch off: an ABR step does not release");

			LatchSettings s = On();
			s.abrStepTrigger = true;
			UpscaleLatch on(s);
			on.Step(Pair1080(0, 40.0, 40.0));
			LatchOutput o = on.Step(abr);
			Assert::IsFalse(o.latched);
			Assert::IsTrue(o.event == LatchEvent::ReleasedAbr && (o.reasonBits & kReasonAbrStep) != 0);
			Assert::AreEqual(static_cast<uint64_t>(1), on.Counters().releasesAbr);
		}

		// R7: the latch is not carried into a menu, nothing latches during one, and the bar restarts afterwards.
		TEST_METHOD(AMenuIsNotLatched)
		{
			using namespace SafetyFx;
			UpscaleLatch latch(On());
			latch.Step(Pair1080(0, 40.0, 40.0));
			LatchInput menu = Pair1080(1, 40.0, 40.0);
			menu.menuHold = true;
			LatchOutput o = latch.Step(menu);
			Assert::IsFalse(o.latched);
			Assert::IsTrue(o.event == LatchEvent::ReleasedMenu && (o.reasonBits & kReasonMenuHold) != 0);
			menu.timeUs = 2000000;
			Assert::IsFalse(latch.Step(menu).latched, L"nothing enters during the menu");
			Assert::AreEqual(static_cast<uint64_t>(1), latch.Counters().releasesMenu);
			Assert::AreEqual(static_cast<uint64_t>(0), latch.Counters().entries - 1);
			Assert::IsTrue(latch.Step(Pair1080(3, 40.0, 0.0)).latched, L"after the menu a strong pair latches again");
		}

		TEST_METHOD(AResetReleasesAndTheLayerIsInertWhenOff)
		{
			using namespace SafetyFx;
			UpscaleLatch latch(On());
			latch.Step(Pair1080(0, 40.0, 40.0));
			LatchOutput o = latch.Reset();
			Assert::IsFalse(o.latched);
			Assert::IsTrue(o.event == LatchEvent::ReleasedReset);
			Assert::AreEqual(static_cast<uint64_t>(1), latch.Counters().releasesReset);

			UpscaleLatch off;
			LatchOutput p = off.Step(Pair1080(0, 40.0, 40.0));
			Assert::IsFalse(p.latched);
			Assert::IsTrue(p.event == LatchEvent::None && p.reasonBits == 0 && !p.heldBack);
		}
	};
}
