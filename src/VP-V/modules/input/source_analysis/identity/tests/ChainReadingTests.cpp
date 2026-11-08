/*
 * Copyright(C) 2026 VP-V contributors
 * SPDX-License-Identifier: GPL-3.0-only
 */
// ChainReading (SID train r2, doc 78 section 5 owner decision): the per-axis reading with hand-made evidence. CONFIDENT = the best
// hypothesis above c1 on an informative evaluation (kernel only when verified); everything else is UNSURE with a reason word (no
// evidence, below c1, axes disagree, mixed (HOLD), no power) and publishes NO applied value; nothing is sticky (no dwell, no minimum
// hold, no contrary-evidence timer: a confident reading follows the evidence at once and drops it at once). Discontinuity and
// active-area reset, repeats, the kernel equivalence class, the adapter from ChainEvidence, determinism. CPU only.
#include "pch.h"
#include "CppUnitTest.h"

#include "GeometryFixtures.h"

#include <cmath>
#include <cstdint>
#include <string>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace Tests
{
	TEST_CLASS(ChainReadingTests)
	{
	public:
		typedef GeoFx::C::AxisReader Reader;
		typedef GeoFx::C::ReadingOutput Out;
		typedef GeoFx::C::ReadingReason Reason;
		typedef GeoFx::G::LatchState State;

		static const int kN = 2160;

		static int64_t Sec(double s) { return static_cast<int64_t>(s * 1e6); }
		static Reader Make() { return Reader(GeoFx::C::ReadingSettings(), GeoFx::Levels()); }

		static Out Step(Reader& r, const GeoFx::Ev& ev, double sec)
		{
			return r.Step(ev.e, GeoFx::Ctx(Sec(sec)));
		}

		TEST_METHOD(TheLevelIsDerivedFromTheEvidenceSettings)
		{
			const GeoFx::C::ReadingLevels lv = GeoFx::Levels();
			Assert::IsTrue(lv.c1 > 14.0 && lv.c1 < 16.0);
		}

		TEST_METHOD(NoEvidenceYetIsUnknownWithTheReason)
		{
			Reader r = Make();
			const Out o = Step(r, GeoFx::Ev(kN), 1.0);
			Assert::IsTrue(o.state == State::Unknown && o.reason == Reason::NoEvidence && !r.HasConfident());
			Assert::IsTrue(o.measured.n == 0 && o.applied.state == State::Unknown && o.applied.n == 0);
		}

		TEST_METHOD(BelowC1IsUnsureWithAReasonAndAppliesNothing)
		{
			Reader r = Make();
			const double c1 = r.Levels().c1;
			Out o = Step(r, GeoFx::Ev(kN).Best(1080, 3.0).Inc(1080, 2.0, 5, 0.9), 1.0);
			Assert::IsTrue(o.state == State::Provisional && o.reason == Reason::BelowC1);
			Assert::IsTrue(o.measured.n == 1080 && o.measured.N == kN && o.measured.ratio == 0.5f && o.measuredChanged, L"the diagnostic best is published");
			Assert::IsTrue(o.applied.state == State::Unknown && o.applied.n == 0 && !o.appliedChanged, L"UNSURE: nothing applied");
			for (int i = 2; i <= 40; ++i)
				o = Step(r, GeoFx::Ev(kN).Best(1080, c1 - 1.0), i);
			Assert::IsTrue(o.state == State::Provisional && !r.HasConfident(), L"however long it stays below c1");
			Assert::IsTrue(r.Stats().confident == 0 && r.Stats().unsure == 40);
		}

		TEST_METHOD(AboveC1IsConfidentAtOnceWithTheVerifiedKernel)
		{
			Reader r = Make();
			const double c1 = r.Levels().c1;
			const Out o = Step(r, GeoFx::Ev(kN).Best(1080, c1 + 1.0).Inc(1080, 2.0, 5, 0.9), 1.0);
			Assert::IsTrue(o.state == State::Latched && o.reason == Reason::Confident, L"no dwell: the first informative evaluation above c1 reads confident");
			Assert::IsTrue(o.applied.state == State::Latched && o.applied.n == 1080 && o.appliedChanged && o.stateChanged);
			Assert::IsTrue(o.applied.kernelIndex == 5 && o.applied.kernelConf > 0.8f, L"the kernel was verified");
			Assert::IsTrue(o.applied.conf == 1.0f, L"confidence = llr / c1 capped at 1");
			Assert::IsTrue(r.HasConfident() && r.ConfidentN() == 1080 && !r.ConfidentNative());
		}

		TEST_METHOD(ConfidenceIsTheLlrRelativeToC1CappedAtOne)
		{
			Assert::IsTrue(GeoFx::C::ConfidenceOfLlr(0.0, 15.0) == 0.0 && GeoFx::C::ConfidenceOfLlr(-2.0, 15.0) == 0.0);
			Assert::IsTrue(std::abs(GeoFx::C::ConfidenceOfLlr(7.5, 15.0) - 0.5) < 1e-12);
			Assert::IsTrue(GeoFx::C::ConfidenceOfLlr(15.0, 15.0) == 1.0 && GeoFx::C::ConfidenceOfLlr(90.0, 15.0) == 1.0);
			Assert::IsTrue(GeoFx::C::ConfidenceOfLlr(5.0, 0.0) == 0.0);
		}

		TEST_METHOD(AKernelBelowTheVerificationConfidenceIsKernelUnsureButTheSizeIsConfident)
		{
			Reader r = Make();
			const double c1 = r.Levels().c1;
			const Out o = Step(r, GeoFx::Ev(kN).Best(1080, c1 + 1.0).Inc(1080, 2.0, 5, 0.3), 1.0);
			Assert::IsTrue(o.applied.state == State::Latched && o.applied.n == 1080);
			Assert::IsTrue(o.applied.kernelIndex == -1, L"n known, kernel unsure: no kernel is applied");
			Assert::IsTrue(o.measured.kernelIndex == 5, L"the measured geometry still shows the best kernel");
		}

		TEST_METHOD(NativeIsConfidentOnlyAboveC1AndCarriesNoKernel)
		{
			Reader r = Make();
			const double c1 = r.Levels().c1;
			Out o = Step(r, GeoFx::Ev(kN).Native(c1 - 2.0, 4).NativeInc(2.0), 1.0);
			Assert::IsTrue(o.state == State::Provisional && o.applied.state == State::Unknown && o.measured.n == kN);
			o = Step(r, GeoFx::Ev(kN).Native(c1 + 1.0, 5).NativeInc(2.0), 2.0);
			Assert::IsTrue(o.state == State::Latched && o.applied.n == kN && o.applied.ratio == 1.0f && o.applied.kernelIndex == -1);
			Assert::IsTrue(r.HasConfident() && r.ConfidentNative());
		}

		TEST_METHOD(ARisingSizeBeatsNativeWhenItHasMoreEvidence)
		{
			Reader r = Make();
			const double c1 = r.Levels().c1;
			const Out o = Step(r, GeoFx::Ev(kN).Best(1080, c1 + 6.0).Native(c1 + 1.0, 5).Inc(1080, 2.0, 5, 0.9), 1.0);
			Assert::IsTrue(o.applied.n == 1080 && !r.ConfidentNative());
		}

		TEST_METHOD(ADisagreementThatKeepsTheBestBelowC1IsNamed)
		{
			Reader r = Make();
			const double c1 = r.Levels().c1;
			Out o = Step(r, GeoFx::Ev(kN).Best(1080, c1 - 1.0).Prior(-2.0), 1.0);
			Assert::IsTrue(o.reason == Reason::AxesDisagree && o.state == State::Provisional && o.applied.state == State::Unknown,
				L"the penalty alone keeps it below c1");
			o = Step(r, GeoFx::Ev(kN).Best(1080, c1 - 6.0).Prior(-2.0), 2.0);
			Assert::IsTrue(o.reason == Reason::BelowC1, L"below c1 even without the penalty: plain below c1");
		}

		TEST_METHOD(NothingIsStickyTheReadingFollowsTheEvidenceAtOnce)
		{
			Reader r = Make();
			const double c1 = r.Levels().c1;
			Out o = Step(r, GeoFx::Ev(kN).Best(1080, c1 + 1.0).Inc(1080, 2.0, 5, 0.9), 1.0);
			Assert::IsTrue(o.applied.n == 1080);
			// The very next evaluation prefers another size: confident in it at once (no minimum hold, no contrary timer).
			o = Step(r, GeoFx::Ev(kN).Best(720, c1 + 2.0).Inc(720, 3.0, 5, 0.9), 2.0);
			Assert::IsTrue(o.applied.n == 720 && o.appliedChanged && o.reason == Reason::Confident);
			// And it drops at once when the evidence falls below c1.
			o = Step(r, GeoFx::Ev(kN).Best(720, c1 - 1.0), 3.0);
			Assert::IsTrue(o.applied.state == State::Unknown && o.applied.n == 0 && o.appliedChanged && !r.HasConfident());
			Assert::IsTrue(o.measured.n == 720, L"while the measured geometry keeps the diagnostic best");
		}

		TEST_METHOD(HoldIsUnsureAndPublishesNoValueEvenAfterAConfidentReading)
		{
			Reader r = Make();
			const double c1 = r.Levels().c1;
			Step(r, GeoFx::Ev(kN).Best(1080, c1 + 1.0).Inc(1080, 2.0, 5, 0.9), 1.0);
			Out o = Step(r, GeoFx::Ev(kN).Hold(), 2.0);
			Assert::IsTrue(o.state == State::Hold && o.reason == Reason::HeldMixed && o.stateChanged);
			Assert::IsTrue(o.applied.state == State::Unknown && o.applied.n == 0 && o.appliedChanged, L"UNSURE publishes no value under an overlay");
			for (int t = 3; t <= 30; ++t)
				o = Step(r, GeoFx::Ev(kN).Hold(), t);
			Assert::IsTrue(o.state == State::Hold && r.Stats().holds >= 29);
			o = Step(r, GeoFx::Ev(kN).Best(1080, c1 + 1.0).Inc(1080, 2.0, 5, 0.9), 31.0);
			Assert::IsTrue(o.state == State::Latched && o.applied.n == 1080, L"the overlay lifts with the next clean evaluation");
		}

		TEST_METHOD(AnEvaluationWithoutPowerIsUnsureWithTheReasonBits)
		{
			Reader r = Make();
			const double c1 = r.Levels().c1;
			Step(r, GeoFx::Ev(kN).Best(1080, c1 + 1.0).Inc(1080, 2.0, 5, 0.9), 1.0);
			Out o;
			for (int t = 2; t <= 120; ++t)
				o = Step(r, GeoFx::Ev(kN).NotInformative(GeoFx::C::kReasonDark | GeoFx::C::kReasonNoPower), t);
			Assert::IsTrue(o.state == State::Abstain && o.reason == Reason::NoPower);
			Assert::IsTrue(o.applied.state == State::Unknown && o.applied.n == 0, L"dark: this evaluation says nothing, so no value");
			Assert::IsTrue((o.reasonBits & GeoFx::C::kReasonDark) != 0, L"the reason bits are carried for the status");
			Assert::IsTrue(r.Stats().abstains >= 119);
			o = Step(r, GeoFx::Ev(kN).Best(1080, c1 + 1.0).Inc(1080, 2.0, 5, 0.9), 121.0);
			Assert::IsTrue(o.state == State::Latched && o.applied.n == 1080, L"evidence returns: confident again");
		}

		TEST_METHOD(AcquisitionWithoutPowerStaysUnsureWithTheReason)
		{
			Reader r = Make();
			const Out o = Step(r, GeoFx::Ev(kN).NotInformative(GeoFx::C::kReasonFlat), 1.0);
			Assert::IsTrue(o.state == State::Abstain && o.reason == Reason::NoPower && !r.HasConfident());
			Assert::IsTrue(o.measured.n == 0 && o.applied.state == State::Unknown);
		}

		TEST_METHOD(ADiscontinuityResetsTheReadingAndAsksTheWorkerToResetTheEvidence)
		{
			Reader r = Make();
			const double c1 = r.Levels().c1;
			Step(r, GeoFx::Ev(kN).Best(1080, c1 + 1.0).Inc(1080, 2.0, 5, 0.9), 1.0);
			GeoFx::C::EvalContext ctx = GeoFx::Ctx(Sec(2.0));
			ctx.discontinuity = true;
			GeoFx::Ev e(kN);
			const Out o = r.Step(e.e, ctx);
			Assert::IsTrue(o.reason == Reason::Discontinuity && o.resetEvidence && o.state == State::Unknown);
			Assert::IsTrue(o.applied.state == State::Unknown && o.appliedChanged && o.stateChanged && o.kernelChanged);
			Assert::IsTrue(!r.HasConfident() && r.Stats().discontinuities == 1);
		}

		TEST_METHOD(AnActiveAreaChangeResetsTheReading)
		{
			Reader r = Make();
			const double c1 = r.Levels().c1;
			Step(r, GeoFx::Ev(kN).Best(1080, c1 + 1.0).Inc(1080, 2.0, 5, 0.9), 1.0);
			GeoFx::C::EvalContext ctx = GeoFx::Ctx(Sec(2.0));
			ctx.activeAreaChanged = true;
			GeoFx::Ev e(kN);
			const Out o = r.Step(e.e, ctx);
			Assert::IsTrue(o.reason == Reason::ActiveArea && o.resetEvidence && !r.HasConfident());
		}

		TEST_METHOD(ARepeatChangesNothing)
		{
			Reader r = Make();
			const double c1 = r.Levels().c1;
			const Out a = Step(r, GeoFx::Ev(kN).Best(1080, c1 + 1.0).Inc(1080, 2.0, 5, 0.9), 1.0);
			GeoFx::C::EvalContext ctx = GeoFx::Ctx(Sec(2.0));
			ctx.repeat = true;
			GeoFx::Ev e(kN);
			const Out o = r.Step(e.e, ctx);
			Assert::IsTrue(o.reason == Reason::Repeat && !o.stateChanged && !o.measuredChanged && !o.appliedChanged && !o.kernelChanged);
			Assert::IsTrue(o.applied.n == a.applied.n && r.HasConfident(), L"a cadence repeat keeps the last reading");
		}

		TEST_METHOD(AMeasuredPhaseDriftIsCaughtAgainstTheLastReportedValue)
		{
			Reader r = Make();
			bool moved = false;
			for (int i = 1; i <= 6; ++i)
			{
				GeoFx::Ev e(kN);
				e.Best(1080, 3.0);
				e.e.inc[e.e.incCount].n = 1080; e.e.inc[e.e.incCount].increment = 1.0; e.e.inc[e.e.incCount].phase = 0.1 * i; ++e.e.incCount;
				const Out o = Step(r, e, i);
				if (i >= 2 && o.measuredChanged)
					moved = true;
			}
			Assert::IsTrue(moved, L"0.1 per evaluation adds up to a phase move against the last reported value");
		}

		TEST_METHOD(TheKernelClassPicksTheBestInvertibleMemberOrIsUnsure)
		{
			namespace C = GeoFx::C;
			const C::ReadingSettings rs;
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
			C::FieldEvidence e = C::MakeFieldEvidence(sum, u, kN, rs, &vr);
			Assert::IsTrue(e.incCount == 1 && e.inc[0].kernelClass == ((1u << 0) | (1u << 1)), L"the class = the kernels within the margin of the best");
			Assert::IsTrue(e.inc[0].kernelIndex == 0 && e.inc[0].kernelConf == 1.0, L"bilinear (best, invertible): familyMargin 0 no longer means unsure");
			// The best member has no classical equivalent (spline): the best INVERTIBLE member of the class is used.
			cr.kernelMask = (1u << 1) | (1u << 8);
			cr.kernelT[1] = -0.38; cr.kernelT[8] = -0.40; cr.stat = -0.40;
			e = C::MakeFieldEvidence(sum, u, kN, rs, &vr);
			Assert::IsTrue(e.inc[0].kernelIndex == 1 && e.inc[0].kernelConf == 1.0, L"a spline in the class: bicubic is the member the re-base can invert");
			// A class without an invertible member: kernel unsure.
			cr.kernelMask = (1u << 7) | (1u << 8);
			cr.kernelT[7] = -0.39; cr.kernelT[8] = -0.40; cr.stat = -0.40;
			e = C::MakeFieldEvidence(sum, u, kN, rs, &vr);
			Assert::IsTrue(e.inc[0].kernelIndex == -1 && e.inc[0].kernelConf == 0.0 && e.inc[0].kernelClass == ((1u << 7) | (1u << 8)));
			// No kernel dips at all (a non-LTI scaler): unsure whatever the class.
			cr.kernelMask = (1u << 0) | (1u << 1);
			cr.kernelT[0] = -0.05; cr.kernelT[1] = -0.05; cr.stat = -0.05;
			e = C::MakeFieldEvidence(sum, u, kN, rs, &vr);
			Assert::IsTrue(e.inc[0].kernelIndex == -1 && e.inc[0].kernelConf == 0.0, L"no LTI kernel verifies");
			// Without per-kernel T a kernel without an inverse is never reported as the kernel.
			u.cand[0].kernel = 8;
			e = C::MakeFieldEvidence(sum, u, kN, rs, nullptr);
			Assert::IsTrue(e.inc[0].kernelIndex == -1 && e.inc[0].kernelConf == 0.0);
		}

		TEST_METHOD(TheAdapterBuildsTheReadingEvidenceFromChainEvidence)
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
			s.hasBest = true; s.n = 1080; s.llr = 11.0; s.prior = -2.0; s.llrWithPrior = 9.0; s.nativeLlr = -4.0; s.nativeEvals = 3;
			const GeoFx::C::ReadingSettings rs;
			const GeoFx::C::FieldEvidence e = GeoFx::C::MakeFieldEvidence(s, u, kN, rs);
			Assert::IsTrue(e.evaluated && e.informative && !e.hold && e.axisLen == kN);
			Assert::IsTrue(e.hasBest && e.bestN == 1080 && e.bestLlr == 9.0 && e.prior == -2.0 && e.nativeLlr == -4.0 && e.nativeEvals == 3);
			Assert::IsTrue(e.incCount == 1, L"only informative candidates carry an increment");
			Assert::IsTrue(e.inc[0].n == 1080 && e.inc[0].increment == 3.5 && e.inc[0].kernelIndex == 5);
			Assert::IsTrue(std::abs(e.inc[0].kernelConf - (1.0 - std::exp(-0.2 / rs.kernelMarginScale))) < 1e-12);
			Assert::IsTrue(e.nativeCounted && e.nativeIncrement == -2.0);
			Assert::IsTrue(GeoFx::C::KernelConfidence(0.0, 0.05) == 0.0 && GeoFx::C::KernelConfidence(5.0, 0.05) > 0.99);
			Assert::IsTrue(GeoFx::C::ConfFromLlr(0.0) == 0.0 && GeoFx::C::ConfFromLlr(4.6) > 0.98 && GeoFx::C::ConfFromLlr(100.0) == 1.0);
		}

		TEST_METHOD(TheSameEvidenceGivesTheSameOutputOnEveryRun)
		{
			auto run = []() {
				Reader r = Make();
				std::string trace;
				const double c1 = r.Levels().c1;
				for (int t = 1; t <= 40; ++t)
				{
					GeoFx::Ev ev(kN);
					if (t < 12)
						ev.Best(1080, c1 + 1.0).Inc(1080, 2.0, 5, 0.9);
					else if (t % 7 == 0)
						ev.Hold();
					else
						ev.Best(720, c1 - 2.0 + (t % 5)).Inc(720, 5.0, 5, 0.9);
					const Out o = Step(r, ev, t);
					trace += std::to_string(static_cast<int>(o.state)) + "," + std::to_string(o.applied.n) + "," +
						std::to_string(o.measured.n) + "," + std::to_string(static_cast<int>(o.reason)) + ";";
				}
				return trace;
			};
			Assert::IsTrue(run() == run());
		}
	};
}
