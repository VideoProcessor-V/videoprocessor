/*
 * Copyright(C) 2026 VP-V contributors
 * SPDX-License-Identifier: GPL-3.0-only
 */
// SID r2 safety rules (E0-validated, default OFF): R2 conventional geometry only, R3 convention-pair prior, R5 asymmetric latch, R7 no latch
// into a menu, R5 ABR switch. CPU only, synthetic only, ASCII literals. The latch is tested as a pure unit (fed the same quantities the
// estimator derives from ChainEvidence), the evidence rules through ChainEvidence itself.
#include "pch.h"
#include "CppUnitTest.h"

#include <VP-V/modules/input/source_analysis/identity/chain/ChainEvidence.h>
#include <VP-V/modules/input/source_analysis/identity/chain/ChainLatch.h>
#include <VP-V/modules/input/source_analysis/identity/chain/DescaleVerifier.h>
#include <VP-V/modules/scaling/geometry/SourceGeometrySet.h>

#include <cstdint>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace Tests
{
	namespace SafetyFx
	{
		using namespace SourceIdentity::Chain;

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

		// An evaluation with a confident conventional 1920x1080 pair (3840x2160 source raster), strong evidence of that pair.
		inline LatchInput Pair1080(int64_t tSec, double llr, double latchedLlr, bool informative = true)
		{
			LatchInput in;
			in.timeUs = tSec * 1000000;
			in.c1 = 15.0;
			in.informative = informative;
			in.havePair = true;
			in.nx = 1920; in.Nx = 3840; in.ny = 1080; in.Ny = 2160;
			in.llrX = in.llrY = llr;
			in.latchedLlrX = in.latchedLlrY = latchedLlr;
			return in;
		}

		// An evaluation that carries no upscaled pair (the readers see nothing confident).
		inline LatchInput Quiet(int64_t tSec, double latchedLlr, bool informative)
		{
			LatchInput in;
			in.timeUs = tSec * 1000000;
			in.c1 = 15.0;
			in.informative = informative;
			in.latchedLlrX = in.latchedLlrY = latchedLlr;
			return in;
		}
	}

	TEST_CLASS(ChainSafetyRulesTests)
	{
	public:
		TEST_METHOD(TheConventionSetListsTheEquallRatiosAndTheNonSquarePairs)
		{
			namespace G = Scaling::GeometrySet;
			bool parked = false;
			Assert::IsTrue(G::ConventionalPair(1920, 3840, 1080, 2160, &parked) != nullptr && !parked);
			Assert::IsTrue(G::ConventionalPair(2560, 3840, 1440, 2160) != nullptr);
			Assert::IsTrue(G::ConventionalPair(720, 3840, 576, 2160) != nullptr, L"720x576 is a non-square pair");
			Assert::IsTrue(G::ConventionalPair(720, 3840, 480, 2160) != nullptr);
			Assert::IsTrue(G::ConventionalPair(1440, 3840, 1080, 2160, &parked) != nullptr && parked, L"anamorphic 1440x1080 is parked");
			Assert::IsTrue(G::ConventionalPair(1920, 3840, 720, 2160) == nullptr, L"two listed axis ratios of different geometries are no pair");
			Assert::IsTrue(G::ConventionalAxis(1920, 3840, 0));
			Assert::IsFalse(G::ConventionalAxis(2500, 3840, 0));
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
			Assert::IsFalse(a.aboveC1 && mismatch.Summary(1).aboveC1);

			ChainEvidence pair(s);
			Feed(pair, 1920, 1080, 6);
			Assert::IsTrue(pair.Summary(0).prior > 0.0, L"the listed pair earns the bonus");

			ChainEvidence nonSquare(s);
			Feed(nonSquare, 720, 576, 6);
			Assert::IsTrue(nonSquare.Summary(0).prior > 0.0, L"a non-square listed pair agrees too");

			UpscaleLatch latch(On());
			LatchInput in = Pair1080(0, 40.0, 0.0);
			in.ny = 720;
			Assert::IsFalse(latch.Step(in).latched, L"the latch refuses an unlisted pair");
		}

		// R5: an injected confident-wrong burst on a native stream releases within the bound; native evidence releases at once.
		TEST_METHOD(AConfidentWrongBurstOnNativeReleasesWithinTheBound)
		{
			using namespace SafetyFx;
			UpscaleLatch latch(On());
			Assert::IsFalse(latch.Step(Quiet(0, 0.0, true)).latched, L"native is sticky");
			LatchOutput o = latch.Step(Pair1080(1, 40.0, 40.0));   // the burst
			Assert::IsTrue(o.latched && o.event == LatchEvent::Entered);
			int64_t releasedAt = -1;
			for (int64_t t = 2; t < 200 && releasedAt < 0; ++t)
			{
				o = latch.Step(Quiet(t, 0.0, true));   // the burst ends: no evidence of the latched size, evaluations are informative
				if (!o.latched)
					releasedAt = t;
			}
			Assert::IsTrue(releasedAt > 0 && releasedAt <= 63, L"released within the 60 s bound");
			Assert::IsTrue(o.event == LatchEvent::Released);
			Assert::AreEqual(static_cast<uint64_t>(1), latch.Counters().releasesBound);

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
			// Normal content: confirmation (0.25 c1 = 3.75) often, dark / flat stretches of 100 s with no evidence at all.
			for (int64_t t = 1; t <= 600; ++t)
			{
				const bool dark = (t % 200) >= 50 && (t % 200) < 150;
				LatchOutput o = latch.Step(dark ? Quiet(t, 0.0, false) : Quiet(t, 5.0, true));
				Assert::IsTrue(o.latched, L"a true upscale is never flickered off");
			}
			Assert::AreEqual(static_cast<uint64_t>(0), latch.Counters().releases);
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
			Assert::IsTrue(at == 121, L"the hard bound releases one second past 120 s");
			Assert::IsTrue(o.event == LatchEvent::Released);
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

			UpscaleLatch off;
			LatchOutput p = off.Step(Pair1080(0, 40.0, 40.0));
			Assert::IsFalse(p.latched);
			Assert::IsTrue(p.event == LatchEvent::None && p.reasonBits == 0 && !p.heldBack);
		}
	};
}
