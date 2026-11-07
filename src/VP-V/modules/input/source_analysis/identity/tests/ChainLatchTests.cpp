/*
 * Copyright(C) 2026 VP-V contributors
 * SPDX-License-Identifier: GPL-3.0-only
 */
// ChainLatch (SID-C2a), every transition of doc 78 section 5 with hand-made evidence: acquisition (PROVISIONAL, CANDIDATE, LATCHED
// after >= 4 informative evaluations over >= 2 shots or >= 5 s), revert only on sustained contrary evidence after T_min, the regime
// boundary toward native bypassing T_min (and only toward native, only for a hypothesis that STARTS after the boundary), HOLD,
// ABSTAIN (absence of evidence keeps the value), UNSTABLE (alternation applies the higher n), kernel changes while n holds,
// discontinuity and active-area reset, repeats, determinism. The levels come from the shipped evidence settings. CPU only.
#include "pch.h"
#include "CppUnitTest.h"

#include "GeometryFixtures.h"

#include <cmath>
#include <cstdint>
#include <string>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace Tests
{
	TEST_CLASS(ChainLatchTests)
	{
	public:
		typedef GeoFx::C::ChainLatch Latch;
		typedef GeoFx::C::LatchOutput Out;
		typedef GeoFx::C::LatchReason Reason;
		typedef GeoFx::G::LatchState State;

		static const int kN = 2160;

		static int64_t Sec(double s) { return static_cast<int64_t>(s * 1e6); }
		static Latch Make() { return Latch(GeoFx::C::LatchSettings(), GeoFx::Levels()); }

		static Out Step(Latch& l, const GeoFx::Ev& ev, double sec, bool cut = false)
		{
			return l.Step(ev.e, GeoFx::Ctx(Sec(sec), cut));
		}

		// Latches `n` (kernel / confidence as given) with a cut in the second evaluation; returns the second of the latch evaluation.
		static double LatchAt(Latch& l, int n, int kernel, double kconf)
		{
			const double c1 = l.Levels().c1;
			double latchedAt = -1.0;
			for (int i = 1; i <= 6; ++i)
			{
				GeoFx::Ev ev(kN);
				ev.Best(n, c1 + 1.0).Inc(n, 2.0, kernel, kconf);
				const Out o = Step(l, ev, i, i == 2);
				if (o.reason == Reason::Latched)
					latchedAt = i;
			}
			return latchedAt;
		}

		// Latches native (the native hypothesis alone, with proven power).
		static double LatchNativeAt(Latch& l)
		{
			const double c1 = l.Levels().c1;
			double latchedAt = -1.0;
			for (int i = 1; i <= 6; ++i)
			{
				GeoFx::Ev ev(kN);
				ev.Native(c1 + 1.0, i).NativeInc(2.0);
				const Out o = Step(l, ev, i, i == 2);
				if (o.reason == Reason::Latched)
					latchedAt = i;
			}
			return latchedAt;
		}

		TEST_METHOD(TheLevelsAreDerivedFromTheEvidenceSettings)
		{
			const GeoFx::C::LatchLevels lv = GeoFx::Levels();
			Assert::IsTrue(lv.c1 > 14.0 && lv.c1 < 16.0);
			Assert::IsTrue(std::abs(lv.c2 - (lv.c1 + 3.0)) < 1e-9);
			Assert::IsTrue(std::abs(lv.shotCap - 0.5 * lv.c1) < 1e-9);
			const GeoFx::C::LatchSettings ls;
			Assert::IsTrue(ls.TMinUs() == 10000000, L"T_min = max(2 x dwell, 10 s floor)");
		}

		TEST_METHOD(AcquisitionRunsUnknownProvisionalCandidateLatched)
		{
			Latch l = Make();
			const double c1 = l.Levels().c1;
			Out o = Step(l, GeoFx::Ev(kN).Best(1080, 3.0).Inc(1080, 2.0, 5, 0.9), 1.0);
			Assert::IsTrue(o.state == State::Provisional && o.reason == Reason::Acquiring);
			Assert::IsTrue(o.measured.n == 1080 && o.measured.N == kN && o.measured.ratio == 0.5f && o.measuredChanged);
			Assert::IsTrue(o.applied.state == State::Unknown && o.applied.n == 0 && !o.appliedChanged, L"nothing applied yet");
			o = Step(l, GeoFx::Ev(kN).Best(1080, c1 + 1.0).Inc(1080, 2.0, 5, 0.9), 2.0);
			Assert::IsTrue(o.state == State::Candidate && o.reason == Reason::Candidate && o.stateChanged);
			o = Step(l, GeoFx::Ev(kN).Best(1080, c1 + 1.0).Inc(1080, 2.0, 5, 0.9), 3.0);
			Assert::IsTrue(o.state == State::Candidate);
			o = Step(l, GeoFx::Ev(kN).Best(1080, c1 + 1.0).Inc(1080, 2.0, 5, 0.9), 4.0);
			Assert::IsTrue(o.state == State::Candidate, L"4 evaluations over 3 s in one shot: not yet (needs 2 shots or 5 s)");
			o = Step(l, GeoFx::Ev(kN).Best(1080, c1 + 1.0).Inc(1080, 2.0, 5, 0.9), 6.5);
			Assert::IsTrue(o.state == State::Candidate, L"4 evaluations at c1 over 4.5 s (the clock runs from the first evaluation AT c1, t = 2)");
			o = Step(l, GeoFx::Ev(kN).Best(1080, c1 + 1.0).Inc(1080, 2.0, 5, 0.9), 7.0);
			Assert::IsTrue(o.state == State::Latched && o.reason == Reason::Latched, L"5.0 s at c1: LATCHED");
			Assert::IsTrue(o.applied.state == State::Latched && o.applied.n == 1080 && o.appliedChanged && o.stateChanged);
			Assert::IsTrue(o.applied.kernelIndex == 5 && o.applied.kernelConf > 0.8f, L"the kernel was verified at the latch");
			Assert::IsTrue(l.Stats().latches == 1 && l.HasApplied() && l.AppliedN() == 1080 && !l.AppliedNative());
		}

		TEST_METHOD(TwoShotsLatchBeforeFiveSecondsButNeverBeforeFourEvaluations)
		{
			Latch l = Make();
			const double c1 = l.Levels().c1;
			Out o = Step(l, GeoFx::Ev(kN).Best(1080, c1 + 1.0), 1.0);
			o = Step(l, GeoFx::Ev(kN).Best(1080, c1 + 1.0), 1.2, true);   // a cut: the second shot
			o = Step(l, GeoFx::Ev(kN).Best(1080, c1 + 1.0), 1.4, true);   // and a third
			Assert::IsTrue(o.state == State::Candidate, L"3 evaluations over 3 shots: the evaluation count (4) is not met");
			o = Step(l, GeoFx::Ev(kN).Best(1080, c1 + 1.0), 1.6);
			Assert::IsTrue(o.state == State::Latched, L"the fourth evaluation over 2+ shots latches inside 1 s");
		}

		TEST_METHOD(BelowC1NeverLatches)
		{
			Latch l = Make();
			const double c1 = l.Levels().c1;
			Out o;
			for (int i = 1; i <= 40; ++i)
				o = Step(l, GeoFx::Ev(kN).Best(1080, c1 - 1.0), i, i % 5 == 0);
			Assert::IsTrue(o.state == State::Provisional && !l.HasApplied());
			Assert::IsTrue(l.Stats().latches == 0);
		}

		TEST_METHOD(ARunRestartsWhenTheBestHypothesisChanges)
		{
			Latch l = Make();
			const double c1 = l.Levels().c1;
			Step(l, GeoFx::Ev(kN).Best(1080, c1 + 1.0), 1.0);
			Step(l, GeoFx::Ev(kN).Best(1080, c1 + 1.0), 2.0);
			Step(l, GeoFx::Ev(kN).Best(1080, c1 + 1.0), 3.0);
			Out o = Step(l, GeoFx::Ev(kN).Best(1440, c1 + 1.0), 7.0);   // another size takes over: its own run starts
			Assert::IsTrue(o.state == State::Candidate && o.measured.n == 1440, L"6 s after the first run began, but the new run is 1 evaluation old");
			o = Step(l, GeoFx::Ev(kN).Best(1441, c1 + 1.0), 8.0);   // +-1 is the same hypothesis
			Assert::IsTrue(o.state == State::Candidate);
		}

		TEST_METHOD(NativeCanBeLatchedWhenTheProbeSawItsInjection)
		{
			Latch l = Make();
			const double at = LatchNativeAt(l);
			Assert::IsTrue(at > 0.0);
			Assert::IsTrue(l.AppliedNative() && l.AppliedN() == kN);
			Assert::IsTrue(l.Last().applied.ratio == 1.0f && l.Last().applied.kernelIndex == -1);
		}

		TEST_METHOD(TheKernelIsAppliedOnlyWhenVerifiedAndAdoptedLaterWithoutRelatching)
		{
			Latch l = Make();
			Assert::IsTrue(LatchAt(l, 1080, 5, 0.2) > 0.0, L"latched with an unsure kernel");
			Assert::IsTrue(l.Last().applied.kernelIndex == -1 && l.Last().applied.kernelFamily == GeoFx::G::kKernelFamilyUnknown,
				L"n known, kernel unsure: the applied kernel is empty");
			Assert::IsTrue(l.Last().measured.kernelIndex == 5, L"while the measured side shows the provisional kernel");
			const uint64_t latches = l.Stats().latches;
			// A verified kernel arrives: adopted after 3 consecutive evaluations over 3 s (not on the first), n and the state do not move.
			Out o = Step(l, GeoFx::Ev(kN).Inc(1080, 1.0, 5, 0.9), 7.0);
			Assert::IsTrue(o.reason != Reason::KernelAdopted && o.applied.kernelIndex == -1, L"one evaluation is not enough");
			o = Step(l, GeoFx::Ev(kN).Inc(1080, 1.0, 5, 0.9), 8.0);
			o = Step(l, GeoFx::Ev(kN).Inc(1080, 1.0, 5, 0.9), 9.0);
			Assert::IsTrue(o.applied.kernelIndex == -1, L"3 evaluations over 2 s: not yet");
			o = Step(l, GeoFx::Ev(kN).Inc(1080, 1.0, 5, 0.9), 10.0);
			Assert::IsTrue(o.reason == Reason::KernelAdopted && o.kernelChanged && o.appliedChanged);
			Assert::IsTrue(o.applied.kernelIndex == 5 && o.applied.n == 1080 && o.state == State::Latched);
			Assert::IsTrue(l.Stats().latches == latches && l.Stats().kernelChanges == 1);
		}

		TEST_METHOD(AKernelChangeNeverRelatchesWhileNHolds)
		{
			Latch l = Make();
			const double at = LatchAt(l, 1080, 5, 0.9);
			Assert::IsTrue(at > 0.0 && l.Last().applied.kernelIndex == 5);
			const GeoFx::C::LatchStats before = l.Stats();
			// Another kernel is best for 3 evaluations over 3 s but T_min since the latch has not passed: held.
			Out o;
			for (double t = at + 1.0; t <= at + 5.0; t += 1.0)
				o = Step(l, GeoFx::Ev(kN).Inc(1080, 1.0, 4, 0.9), t);
			Assert::IsTrue(o.applied.kernelIndex == 5, L"inside T_min the applied kernel stays");
			// After T_min and a sustained run the kernel changes; n, the state and the latch counters do not.
			for (double t = at + 6.0; t <= at + 14.0; t += 1.0)
				o = Step(l, GeoFx::Ev(kN).Inc(1080, 1.0, 4, 0.9), t);
			Assert::IsTrue(o.applied.kernelIndex == 4 && o.applied.n == 1080 && o.state == State::Latched);
			Assert::IsTrue(l.Stats().kernelChanges >= 1 && l.Stats().latches == before.latches && l.Stats().reverts == before.reverts);
		}

		TEST_METHOD(AMeasuredPhaseMoveBelowTheToleranceIsNoChange)
		{
			Latch l = Make();
			const double c1 = l.Levels().c1;
			GeoFx::Ev a(kN);
			a.Best(1080, 3.0);
			a.e.inc[a.e.incCount].n = 1080; a.e.inc[a.e.incCount].increment = 1.0; a.e.inc[a.e.incCount].phase = 0.10; ++a.e.incCount;
			Out o = Step(l, a, 1.0);
			GeoFx::Ev b(kN);
			b.Best(1080, 3.5);
			b.e.inc[b.e.incCount].n = 1080; b.e.inc[b.e.incCount].increment = 1.0; b.e.inc[b.e.incCount].phase = 0.20; ++b.e.incCount;
			o = Step(l, b, 2.0);
			Assert::IsFalse(o.measuredChanged, L"0.1 of a latent sample is below the phase tolerance");
			GeoFx::Ev c(kN);
			c.Best(1080, 4.0);
			c.e.inc[c.e.incCount].n = 1080; c.e.inc[c.e.incCount].increment = 1.0; c.e.inc[c.e.incCount].phase = 0.60; ++c.e.incCount;
			o = Step(l, c, 3.0);
			Assert::IsTrue(o.measuredChanged, L"0.4 is a phase move");
			Assert::IsTrue(c1 > 0.0);
		}

		TEST_METHOD(ContraryEvidenceRevertsOnlyAfterC2ThreeEvaluationsThreeSecondsAndTMin)
		{
			Latch l = Make();
			const double tL = LatchAt(l, 1080, 5, 0.9);
			Assert::IsTrue(tL > 0.0);
			// Another size (720) accumulates +5 per evaluation; cuts at 7 and 10 s (the per-shot cap needs three shots to reach c2).
			Out o;
			bool sawBuilding = false;
			for (int t = 5; t <= 13; ++t)
			{
				o = Step(l, GeoFx::Ev(kN).Inc(1080, -3.0, 5, 0.9).Inc(720, 5.0, 5, 0.9), t, t == 7 || t == 10);
				Assert::IsTrue(o.applied.n == 1080, L"no revert yet");
				if (o.reason == Reason::ContraryBuilding)
					sawBuilding = true;
			}
			Assert::IsTrue(sawBuilding, L"building contrary evidence is said");
			Assert::IsTrue(o.reason == Reason::HeldMinHold && o.state == State::Latched, L"c2 over 3 evaluations and 3 s at 13 s, but T_min (10 s) since the latch has not passed");
			Assert::IsTrue(l.Stats().tMinHolds >= 1);
			Assert::IsTrue(o.measured.n == 720, L"the measured geometry already follows the contrary hypothesis");
			o = Step(l, GeoFx::Ev(kN).Inc(720, 5.0, 5, 0.9), 14.0);
			Assert::IsTrue(o.reason == Reason::Reverted && o.applied.n == 720 && o.appliedChanged && !o.bypassedTMin);
			Assert::IsTrue(l.Stats().reverts == 1 && l.AppliedN() == 720);
		}

		TEST_METHOD(OneShotOfContraryEvidenceNeverReverts)
		{
			Latch l = Make();
			const double tL = LatchAt(l, 1080, 5, 0.9);
			Assert::IsTrue(tL > 0.0);
			Out o;
			for (int t = 20; t <= 60; ++t)
				o = Step(l, GeoFx::Ev(kN).Inc(720, 5.0, 5, 0.9), t);   // no cut: one shot, capped at c1 / 2 < c2
			Assert::IsTrue(o.applied.n == 1080 && l.Stats().reverts == 0, L"a single shot cannot reach c2 (per-shot cap)");
		}

		TEST_METHOD(ContraryEvidenceThatFallsBackDoesNotAccumulateAcrossGaps)
		{
			Latch l = Make();
			LatchAt(l, 1080, 5, 0.9);
			// Three shots of contrary evidence, but with the contrary hypothesis contradicted in between (negative increments).
			Out o;
			for (int t = 20; t <= 50; ++t)
			{
				const double inc = (t % 10 < 4) ? 5.0 : -6.0;
				o = Step(l, GeoFx::Ev(kN).Inc(720, inc, 5, 0.9), t, t % 10 == 0);
			}
			Assert::IsTrue(o.applied.n == 1080 && l.Stats().reverts == 0, L"the CUSUM falls back to 0 on contradicting evidence");
		}

		TEST_METHOD(ARegimeBoundaryTowardNativeBypassesTMin)
		{
			Latch l = Make();
			const double tL = LatchAt(l, 1080, 5, 0.9);   // 4 s
			Assert::IsTrue(tL > 0.0);
			const GeoFx::C::LatchLevels lv = l.Levels();
			Out o;
			// A menu at 8 s (a cut): native evidence +2 per evaluation (probe seen), the picture's dip evidence gone.
			for (int t = 8; t <= 10; ++t)
			{
				o = Step(l, GeoFx::Ev(kN).Inc(1080, -3.0, 5, 0.9).NativeInc(2.0), t, t == 8);
				Assert::IsTrue(o.applied.n == 1080, L"3 evaluations but below one saturated shot: still the latched value");
			}
			o = Step(l, GeoFx::Ev(kN).Inc(1080, -3.0, 5, 0.9).NativeInc(2.0), 11.0);
			Assert::IsTrue(o.reason == Reason::RegimeNative, L"a hypothesis that started after the boundary, >= 3 evaluations, at the per-shot cap");
			Assert::IsTrue(o.applied.n == kN && l.AppliedNative() && o.bypassedTMin, L"7 s after the latch: inside T_min, bypassed");
			Assert::IsTrue(l.Stats().regimeBypasses == 1 && l.Stats().reverts == 1);
			Assert::IsTrue(lv.shotCap > 7.0);
		}

		TEST_METHOD(NativeEvidenceThatStopsAtTheBoundaryNeverBypassesTMin)
		{
			Latch l = Make();
			const double tL = LatchAt(l, 1080, 5, 0.9);   // 4 s
			Assert::IsTrue(tL > 0.0);
			Out o;
			for (int t = 5; t <= 7; ++t)   // native evidence builds before the boundary ...
				o = Step(l, GeoFx::Ev(kN).Inc(1080, -3.0, 5, 0.9).NativeInc(2.0), t);
			for (int t = 8; t <= 14; ++t)   // ... the cut comes at 8 s and the evidence is gone: nothing counts after it
				o = Step(l, GeoFx::Ev(kN).Inc(1080, 1.0, 5, 0.9), t, t == 8);
			Assert::IsTrue(o.applied.n == 1080 && l.Stats().regimeBypasses == 0 && l.Stats().reverts == 0);
		}

		TEST_METHOD(AResidualScoreBeforeTheBoundaryDoesNotBlockTheRegimeSwitch)
		{
			Latch l = Make();
			const double tL = LatchAt(l, 1080, 5, 0.9);   // 4 s
			Assert::IsTrue(tL > 0.0);
			Out o;
			for (int t = 5; t <= 7; ++t)   // native evidence builds to a score of 6 before the boundary
				o = Step(l, GeoFx::Ev(kN).Inc(1080, -3.0, 5, 0.9).NativeInc(2.0), t);
			Assert::IsTrue(o.applied.n == 1080);
			// The menu cut at 8 s: a full per-shot gain of native evidence AFTER the flag counts, whatever the score was before it.
			for (int t = 8; t <= 10; ++t)
			{
				o = Step(l, GeoFx::Ev(kN).Inc(1080, -3.0, 5, 0.9).NativeInc(2.0), t, t == 8);
				Assert::IsTrue(o.applied.n == 1080, L"gain after the flag is still below one saturated shot");
			}
			o = Step(l, GeoFx::Ev(kN).Inc(1080, -3.0, 5, 0.9).NativeInc(2.0), 11.0);
			Assert::IsTrue(o.reason == Reason::RegimeNative && l.AppliedNative() && o.bypassedTMin);
		}

		TEST_METHOD(TowardTheRebaseNothingBypassesTMin)
		{
			Latch l = Make();
			Assert::IsTrue(LatchNativeAt(l) > 0.0);   // native, latched at 4 s
			Out o;
			for (int t = 8; t <= 13; ++t)   // a cut at 8 s, then a re-based hypothesis building within T_min
				o = Step(l, GeoFx::Ev(kN).Inc(1080, 5.0, 5, 0.9), t, t == 8 || t == 9 || t == 10);
			Assert::IsTrue(l.AppliedNative() && l.Stats().regimeBypasses == 0, L"the slow direction waits for c2 and T_min");
		}

		TEST_METHOD(HoldFreezesTheStateAndKeepsTheLatchedValue)
		{
			Latch l = Make();
			const double tL = LatchAt(l, 1080, 5, 0.9);
			Assert::IsTrue(tL > 0.0);
			Out o = Step(l, GeoFx::Ev(kN).Hold(), 7.0);
			Assert::IsTrue(o.state == State::Hold && o.reason == Reason::HeldMixed && o.stateChanged);
			Assert::IsTrue(o.applied.state == State::Latched && o.applied.n == 1080, L"the claim stands under an overlay");
			for (int t = 8; t <= 40; ++t)
				o = Step(l, GeoFx::Ev(kN).Hold(), t, t % 3 == 0);
			Assert::IsTrue(o.applied.n == 1080 && o.state == State::Hold && l.Stats().reverts == 0);
			Assert::IsTrue(l.Stats().holds >= 30);
			o = Step(l, GeoFx::Ev(kN).Inc(1080, 1.0, 5, 0.9), 41.0);
			Assert::IsTrue(o.state == State::Latched && o.stateChanged, L"the overlay lifts with the next clean evaluation");
		}

		TEST_METHOD(HoldClearsBuiltContraryEvidence)
		{
			Latch l = Make();
			LatchAt(l, 1080, 5, 0.9);
			for (int t = 5; t <= 11; ++t)
				Step(l, GeoFx::Ev(kN).Inc(720, 5.0, 5, 0.9), t, t == 7 || t == 10);
			Step(l, GeoFx::Ev(kN).Hold(), 12.0);   // an overlay appears: the run is not consecutive any more
			Out o;
			for (int t = 13; t <= 17; ++t)
				o = Step(l, GeoFx::Ev(kN).Inc(720, 5.0, 5, 0.9), t);
			Assert::IsTrue(o.applied.n == 1080 && l.Stats().reverts == 0, L"evidence from before the overlay is not carried over");
		}

		TEST_METHOD(AbsenceOfEvidenceKeepsTheLatchedValue)
		{
			Latch l = Make();
			const double tL = LatchAt(l, 1080, 5, 0.9);
			Assert::IsTrue(tL > 0.0);
			Out o;
			for (int t = 7; t <= 120; ++t)
				o = Step(l, GeoFx::Ev(kN).NotInformative(GeoFx::C::kReasonDark | GeoFx::C::kReasonNoPower), t);
			Assert::IsTrue(o.state == State::Abstain && o.reason == Reason::HeldNoEvidence);
			Assert::IsTrue(o.applied.n == 1080 && o.applied.state == State::Latched);
			Assert::IsTrue((o.reasonBits & GeoFx::C::kReasonDark) != 0, L"the reason bits are carried for the status");
			Assert::IsTrue(l.Stats().abstains > 100 && l.Stats().reverts == 0);
			o = Step(l, GeoFx::Ev(kN).Inc(1080, 1.0, 5, 0.9), 121.0);
			Assert::IsTrue(o.state == State::Latched, L"evidence returns: LATCHED again, nothing changed");
		}

		TEST_METHOD(AcquisitionWithoutEvidenceStaysUnknownWithTheReason)
		{
			Latch l = Make();
			const Out o = Step(l, GeoFx::Ev(kN).NotInformative(GeoFx::C::kReasonFlat), 1.0);
			Assert::IsTrue(o.state == State::Abstain && o.reason == Reason::NoEvidence && !l.HasApplied());
			Assert::IsTrue(o.measured.n == 0 && o.applied.state == State::Unknown);
		}

		TEST_METHOD(TwoSizesAlternatingWithinAMinuteAreUnstableAndTheHigherSizeIsApplied)
		{
			Latch l = Make();
			Assert::IsTrue(LatchAt(l, 1080, 5, 0.9) > 0.0);   // A = 1080 at 4 s
			Out o;
			for (int t = 5; t <= 14; ++t)   // B = 720: reverted at 14 s
				o = Step(l, GeoFx::Ev(kN).Inc(720, 5.0, 5, 0.9), t, t == 7 || t == 10);
			Assert::IsTrue(o.reason == Reason::Reverted && o.applied.n == 720);
			for (int t = 15; t <= 24; ++t)   // back to A within 60 s: the alternation
				o = Step(l, GeoFx::Ev(kN).Inc(1080, 5.0, 5, 0.9), t, t == 17 || t == 20);
			Assert::IsTrue(o.state == State::Unstable && o.reason == Reason::Unstable, L"UNSTABLE");
			Assert::IsTrue(o.applied.n == 1080 && o.applied.state == State::Unstable, L"the higher n is applied");
			Assert::IsTrue(l.Stats().unstableEntries == 1);
			// The lower partner keeps coming back: the higher n stands, no further change.
			for (int t = 25; t <= 40; ++t)
				o = Step(l, GeoFx::Ev(kN).Inc(720, 5.0, 5, 0.9), t, t == 27 || t == 30);
			Assert::IsTrue(o.applied.n == 1080 && o.state == State::Unstable, L"still the higher n, still visible");
		}

		TEST_METHOD(ADiscontinuityResetsEverythingAndAskForAnEvidenceReset)
		{
			Latch l = Make();
			Assert::IsTrue(LatchAt(l, 1080, 5, 0.9) > 0.0);
			GeoFx::C::EvalContext c = GeoFx::Ctx(Sec(10.0));
			c.discontinuity = true;
			Out o = l.Step(GeoFx::Ev(kN).Best(1080, 40.0).e, c);
			Assert::IsTrue(o.state == State::Unknown && o.reason == Reason::Discontinuity && o.resetEvidence);
			Assert::IsTrue(o.applied.state == State::Unknown && o.applied.n == 0 && o.appliedChanged && o.measuredChanged && o.stateChanged);
			Assert::IsFalse(l.HasApplied());
			// Acquisition starts from scratch (and needs its dwell again).
			const double c1 = l.Levels().c1;
			o = Step(l, GeoFx::Ev(kN).Best(1080, c1 + 1.0), 11.0);
			Assert::IsTrue(o.state == State::Candidate);
			c = GeoFx::Ctx(Sec(12.0));
			c.activeAreaChanged = true;
			o = l.Step(GeoFx::Ev(1608).e, c);
			Assert::IsTrue(o.state == State::Unknown && o.reason == Reason::ActiveArea && o.resetEvidence && o.measured.N == 1608);
		}

		TEST_METHOD(ARepeatChangesNothing)
		{
			Latch l = Make();
			Assert::IsTrue(LatchAt(l, 1080, 5, 0.9) > 0.0);
			GeoFx::C::EvalContext c = GeoFx::Ctx(Sec(9.0));
			c.repeat = true;
			const uint64_t reverts = l.Stats().reverts;
			const Out o = l.Step(GeoFx::Ev(kN).Inc(720, 5.0).e, c);
			Assert::IsTrue(o.reason == Reason::Repeat && !o.appliedChanged && !o.measuredChanged && !o.stateChanged);
			Assert::IsTrue(o.applied.n == 1080 && l.Stats().reverts == reverts);
		}

		TEST_METHOD(ADipBelowC1RestartsTheDwellClock)
		{
			Latch l = Make();
			const double c1 = l.Levels().c1;
			Out o;
			for (int t = 1; t <= 3; ++t)
				o = Step(l, GeoFx::Ev(kN).Best(1080, c1 + 1.0), t);
			o = Step(l, GeoFx::Ev(kN).Best(1080, c1 - 1.0), 4.0);   // the evidence dips below c1
			Assert::IsTrue(o.state == State::Provisional);
			for (int t = 5; t <= 8; ++t)
				o = Step(l, GeoFx::Ev(kN).Best(1080, c1 + 1.0), t);
			Assert::IsTrue(o.state == State::Candidate, L"4 evaluations at c1 again, but only 3 s: the clock restarted at the dip");
			for (int t = 9; t <= 10; ++t)
				o = Step(l, GeoFx::Ev(kN).Best(1080, c1 + 1.0), t);
			Assert::IsTrue(o.state == State::Latched, L"5 s at c1");
		}

		TEST_METHOD(EachCutIsAShotWhenTheCallSiteCountsThem)
		{
			Latch l = Make();
			const double c1 = l.Levels().c1;
			Out o;
			for (int i = 1; i <= 4; ++i)
			{
				GeoFx::C::EvalContext c = GeoFx::Ctx(Sec(1.0 + 0.1 * i));
				if (i == 2)
					c.cuts = 2;   // two cuts since the previous evaluation
				o = l.Step(GeoFx::Ev(kN).Best(1080, c1 + 1.0).e, c);
			}
			Assert::IsTrue(o.state == State::Latched, L"4 evaluations over 3 shots inside half a second");
		}

		TEST_METHOD(ContraryRunsAreConsecutiveEvaluations)
		{
			Latch l = Make();
			Assert::IsTrue(LatchAt(l, 1080, 5, 0.9) > 0.0);   // 4 s
			Out o;
			for (int t = 5; t <= 10; ++t)   // c2 is reached at 9 s (cuts at 6 and 8)
				o = Step(l, GeoFx::Ev(kN).Inc(720, 5.0, 5, 0.9), t, t == 6 || t == 8);
			o = Step(l, GeoFx::Ev(kN).Inc(1080, -1.0, 5, 0.9), 11.0);   // an evaluation WITHOUT the contrary hypothesis breaks the run
			for (int t = 12; t <= 14; ++t)
			{
				o = Step(l, GeoFx::Ev(kN).Inc(720, 5.0, 5, 0.9), t);
				Assert::IsTrue(o.applied.n == 1080, L"the run restarted at 12 s: 3 evaluations but only 2 s");
			}
			o = Step(l, GeoFx::Ev(kN).Inc(720, 5.0, 5, 0.9), 15.0);
			Assert::IsTrue(o.reason == Reason::Reverted && o.applied.n == 720, L"4 evaluations over 3 s since the restart");
		}

		TEST_METHOD(AMeasuredPhaseDriftIsCaughtAgainstTheLastReportedValue)
		{
			Latch l = Make();
			bool moved = false;
			for (int i = 1; i <= 6; ++i)
			{
				GeoFx::Ev e(kN);
				e.Best(1080, 3.0);
				e.e.inc[e.e.incCount].n = 1080; e.e.inc[e.e.incCount].increment = 1.0; e.e.inc[e.e.incCount].phase = 0.1 * i; ++e.e.incCount;
				const Out o = Step(l, e, i);
				if (i >= 2 && o.measuredChanged)
					moved = true;
			}
			Assert::IsTrue(moved, L"0.1 per evaluation adds up to a phase move against the last reported value");
		}

		TEST_METHOD(AWithinClassFlipOfTheBestKernelNeverChangesTheAppliedKernel)
		{
			Latch l = Make();
			const double c1 = l.Levels().c1;
			const uint32_t cls = (1u << 1) | (1u << 2);   // bicubic 0/0.5 and 0/0.6: indistinguishable
			for (int i = 1; i <= 6; ++i)
				Step(l, GeoFx::Ev(kN).Best(1080, c1 + 1.0).Inc(1080, 2.0, 1, 1.0, cls), i, i == 2);
			Assert::IsTrue(l.Last().applied.kernelIndex == 1, L"fixture: bicubic 0/0.5 applied");
			Out o;
			for (int t = 7; t <= 60; ++t)   // the best member flips to 0/0.6 and stays there: still the same class
				o = Step(l, GeoFx::Ev(kN).Inc(1080, 2.0, 2, 1.0, cls), t);
			Assert::IsTrue(o.applied.kernelIndex == 1 && l.Stats().kernelChanges == 0, L"no kernel change inside the class");
			// A class that no longer contains the applied kernel is a real change (after T2 and T_min).
			for (int t = 61; t <= 70; ++t)
				o = Step(l, GeoFx::Ev(kN).Inc(1080, 2.0, 5, 1.0, 1u << 5), t);
			Assert::IsTrue(o.applied.kernelIndex == 5 && l.Stats().kernelChanges == 1);
		}

		TEST_METHOD(TheKernelClassPicksTheBestInvertibleMemberOrIsUnsure)
		{
			namespace C = GeoFx::C;
			const C::LatchSettings ls;
			C::EvidenceUpdate u;
			u.informative = true;
			u.candidateCount = 1;
			u.cand[0].n = 1080; u.cand[0].informative = true; u.cand[0].increment = 4.0; u.cand[0].kernel = 0; u.cand[0].familyMargin = 0.0;
			C::AxisSummary sum;
			sum.hasBest = true; sum.n = 1080; sum.llrWithPrior = 5.0;
			C::VerifyResult vr;
			vr.candidates.resize(1);
			C::CandidateResult& cr = vr.candidates[0];
			cr.n = 1080; cr.valid = true; cr.exactDone = true; cr.kernel = 0; cr.stat = -0.40; cr.phase = 0.0;
			// 2x: bilinear and bicubic 0/0.5 are tied to within the class margin, lanczos3 is clearly worse.
			cr.kernelMask = (1u << 0) | (1u << 1) | (1u << 5);
			cr.kernelT[0] = -0.40; cr.kernelT[1] = -0.395; cr.kernelT[5] = -0.20;
			C::FieldEvidence e = C::MakeFieldEvidence(sum, u, kN, ls, &vr);
			Assert::IsTrue(e.incCount == 1 && e.inc[0].kernelClass == ((1u << 0) | (1u << 1)), L"the class = the kernels within the margin of the best");
			Assert::IsTrue(e.inc[0].kernelIndex == 0 && e.inc[0].kernelConf == 1.0, L"bilinear (best, invertible): familyMargin 0 no longer means unsure");
			// The best member has no classical equivalent (spline): the best INVERTIBLE member of the class is used.
			cr.kernelMask = (1u << 1) | (1u << 8);
			cr.kernelT[1] = -0.38; cr.kernelT[8] = -0.40; cr.stat = -0.40;
			e = C::MakeFieldEvidence(sum, u, kN, ls, &vr);
			Assert::IsTrue(e.inc[0].kernelIndex == 1 && e.inc[0].kernelConf == 1.0, L"a spline in the class: bicubic is the member the re-base can invert");
			// A class without an invertible member: kernel unsure.
			cr.kernelMask = (1u << 7) | (1u << 8);
			cr.kernelT[7] = -0.39; cr.kernelT[8] = -0.40; cr.stat = -0.40;
			e = C::MakeFieldEvidence(sum, u, kN, ls, &vr);
			Assert::IsTrue(e.inc[0].kernelIndex == -1 && e.inc[0].kernelConf == 0.0 && e.inc[0].kernelClass == ((1u << 7) | (1u << 8)));
			// No kernel dips at all (a non-LTI scaler): unsure whatever the class.
			cr.kernelMask = (1u << 0) | (1u << 1);
			cr.kernelT[0] = -0.05; cr.kernelT[1] = -0.05; cr.stat = -0.05;
			e = C::MakeFieldEvidence(sum, u, kN, ls, &vr);
			Assert::IsTrue(e.inc[0].kernelIndex == -1 && e.inc[0].kernelConf == 0.0, L"no LTI kernel verifies");
			// Without per-kernel T a kernel without an inverse is never reported as the kernel.
			u.cand[0].kernel = 8;
			e = C::MakeFieldEvidence(sum, u, kN, ls, nullptr);
			Assert::IsTrue(e.inc[0].kernelIndex == -1 && e.inc[0].kernelConf == 0.0);
		}

		TEST_METHOD(AMenuPeekRoundTripWithTheLowerRungSustainedEndsLatchedAtTheLowerSize)
		{
			Latch l = Make();
			Assert::IsTrue(LatchAt(l, 1080, 5, 0.9) > 0.0);   // A = 1080 at 4 s
			Out o;
			for (int t = 5; t <= 14; ++t)   // B = 720: reverted at 14 s
				o = Step(l, GeoFx::Ev(kN).Inc(720, 5.0, 5, 0.9), t, t == 7 || t == 10);
			Assert::IsTrue(o.reason == Reason::Reverted && o.applied.n == 720);
			for (int t = 15; t <= 24; ++t)   // back to A inside the window: UNSTABLE, A applied
				o = Step(l, GeoFx::Ev(kN).Inc(1080, 5.0, 5, 0.9), t, t == 17 || t == 20);
			Assert::IsTrue(o.state == State::Unstable && o.applied.n == 1080);
			// The lower rung (720) is the truth from now on and persists for 100 s: UNSTABLE ends and 720 is applied.
			for (int t = 25; t <= 125; ++t)
				o = Step(l, GeoFx::Ev(kN).Inc(720, 5.0, 5, 0.9), t, t % 3 == 0);
			Assert::IsTrue(o.state == State::Latched && o.applied.n == 720 && o.applied.state == State::Latched,
				L"the lower rung persisted past the window: no permanent UNSTABLE");
			Assert::IsTrue(l.Stats().unstableEntries == 1);
		}

		TEST_METHOD(TheAdapterBuildsTheLatchEvidenceFromChainEvidence)
		{
			GeoFx::C::EvidenceUpdate u;
			u.informative = true;
			u.reason = GeoFx::C::kReasonMasked;
			u.candidateCount = 2;
			u.cand[0].n = 1080; u.cand[0].informative = true; u.cand[0].increment = 3.5; u.cand[0].kernel = 5; u.cand[0].familyMargin = 0.2;
			u.cand[1].n = 720; u.cand[1].informative = false;
			u.nativeCounted = true;
			u.nativeIncrement = -2.0;
			GeoFx::C::AxisSummary s;
			s.hasBest = true; s.n = 1080; s.llrWithPrior = 9.0; s.nativeLlr = -4.0; s.nativeEvals = 3;
			const GeoFx::C::LatchSettings ls;
			const GeoFx::C::FieldEvidence e = GeoFx::C::MakeFieldEvidence(s, u, kN, ls);
			Assert::IsTrue(e.evaluated && e.informative && !e.hold && e.axisLen == kN);
			Assert::IsTrue(e.hasBest && e.bestN == 1080 && e.bestLlr == 9.0 && e.nativeLlr == -4.0 && e.nativeEvals == 3);
			Assert::IsTrue(e.incCount == 1, L"only informative candidates carry an increment");
			Assert::IsTrue(e.inc[0].n == 1080 && e.inc[0].increment == 3.5 && e.inc[0].kernelIndex == 5);
			Assert::IsTrue(std::abs(e.inc[0].kernelConf - (1.0 - std::exp(-0.2 / ls.kernelMarginScale))) < 1e-12);
			Assert::IsTrue(e.nativeCounted && e.nativeIncrement == -2.0);
			Assert::IsTrue(GeoFx::C::KernelConfidence(0.0, 0.05) == 0.0 && GeoFx::C::KernelConfidence(5.0, 0.05) > 0.99);
			Assert::IsTrue(GeoFx::C::ConfFromLlr(0.0) == 0.0 && GeoFx::C::ConfFromLlr(4.6) > 0.98 && GeoFx::C::ConfFromLlr(100.0) == 1.0);
		}

		TEST_METHOD(TheSameEvidenceGivesTheSameOutputOnEveryRun)
		{
			auto run = []() {
				Latch l = Make();
				std::string trace;
				const double c1 = l.Levels().c1;
				for (int t = 1; t <= 40; ++t)
				{
					GeoFx::Ev ev(kN);
					if (t < 12)
						ev.Best(1080, c1 + 1.0).Inc(1080, 2.0, 5, 0.9);
					else
						ev.Inc(720, 5.0, 5, 0.9);
					const Out o = Step(l, ev, t, t % 3 == 0);
					trace += std::to_string(static_cast<int>(o.state)) + "," + std::to_string(o.applied.n) + "," +
						std::to_string(o.measured.n) + "," + std::to_string(static_cast<int>(o.reason)) + ";";
				}
				return trace;
			};
			Assert::IsTrue(run() == run());
		}
	};
}
