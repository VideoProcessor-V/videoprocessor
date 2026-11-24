/*
 * Copyright(C) 2026 VP-V contributors
 * SPDX-License-Identifier: GPL-3.0-only
 */
// Proving unit tests for ChainStepMark (SID Stage S1, doc 100 s2.5, FM-6).
// CPU only, synthetic only, deterministic.
#include "pch.h"
#include "CppUnitTest.h"

#include <VP-V/modules/input/source_analysis/identity/chain/ChainStepMark.h>

#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace Tests
{
	namespace StepMarkFx
	{
		using namespace SourceIdentity::Chain;

		constexpr int kRasterW = 3840;
		constexpr int kRasterH = 2160;

		inline StepMarkSettings Enabled()
		{
			StepMarkSettings s;
			s.enabled = true;
			return s;
		}

		inline StepMarkLatchContext Latched1080()
		{
			StepMarkLatchContext ctx;
			ctx.latched = true;
			ctx.nx = 1920; ctx.Nx = kRasterW;
			ctx.ny = 1080; ctx.Ny = kRasterH;
			return ctx;
		}

		inline StepMarkLatchContext Latched540()
		{
			StepMarkLatchContext ctx;
			ctx.latched = true;
			ctx.nx = 960; ctx.Nx = kRasterW;
			ctx.ny = 540; ctx.Ny = kRasterH;
			return ctx;
		}

		inline TentativeEvalRecord EvalRec(int64_t sec, uint64_t seq, int nx, int ny, double credit, bool contradicts = false, bool native = false)
		{
			TentativeEvalRecord r;
			r.timeUs = sec * 1000000;
			r.evalSeq = seq;
			r.nx = nx; r.Nx = kRasterW;
			r.ny = ny; r.Ny = kRasterH;
			r.creditX = credit;
			r.creditY = credit;
			r.chainContradicts = contradicts;
			r.chainSeesNative = native;
			r.hasChainPair = (nx > 0 && ny > 0);
			r.rasterW = kRasterW;
			r.rasterH = kRasterH;
			return r;
		}

		inline StepMarkClassicalFrame ClassicalFrame(int64_t sec, int nx, int ny, bool informative = true, bool voided = false, bool native = false)
		{
			StepMarkClassicalFrame f;
			f.timeUs = sec * 1000000;
			f.informative = informative;
			f.voided = voided;
			f.nativeReading = native;
			f.hasPair = (nx > 0 && ny > 0);
			f.nx = nx; f.Nx = kRasterW;
			f.ny = ny; f.Ny = kRasterH;
			f.rasterW = kRasterW;
			f.rasterH = kRasterH;
			return f;
		}
	}

	TEST_CLASS(ChainStepMarkTests)
	{
	public:
		TEST_METHOD(DefaultSettingsAreOffAndVisiblyDisabled)
		{
			using namespace StepMarkFx;
			ChainStepMark sm; // default settings: enabled == false
			Assert::IsFalse(sm.Settings().enabled);
			Assert::IsFalse(sm.BlocksEntryOrMove());
			Assert::IsFalse(sm.PullNextEval());

			const auto o1 = sm.OnAbrStep(10 * 1000000, 0.25);
			Assert::IsTrue(o1.action == StepMarkAction::None);
			Assert::IsTrue(o1.reason == StepMarkReason::Disabled);
			Assert::IsFalse(sm.IsPending());
			Assert::AreEqual(uint64_t(0), sm.Counters().marksRaised);

			const auto o2 = sm.OnChainEvaluation(EvalRec(11, 1, 1920, 1080, 4.0, true), Latched540());
			Assert::IsTrue(o2.reason == StepMarkReason::Disabled);

			const auto o3 = sm.OnClassicalFrame(ClassicalFrame(11, 1920, 1080), Latched540());
			Assert::IsTrue(o3.reason == StepMarkReason::Disabled);

			const auto o4 = sm.Reset(StepMarkReason::ResetOnCut);
			Assert::IsTrue(o4.reason == StepMarkReason::Disabled);
			Assert::IsTrue(std::string(StepMarkReasonText(StepMarkReason::Disabled)).find("disabled") != std::string::npos);
		}

		TEST_METHOD(BelowRatioStepAndInvalidInputRejectedVisibly)
		{
			using namespace StepMarkFx;
			ChainStepMark sm(Enabled());

			// Below minRatioStep (0.10): ignored visibly, not silent.
			const auto oSmall = sm.OnAbrStep(5 * 1000000, 0.05);
			Assert::IsTrue(oSmall.action == StepMarkAction::None);
			Assert::IsTrue(oSmall.reason == StepMarkReason::BelowRatioStep);
			Assert::AreEqual(uint64_t(1), sm.Counters().ignoredBelowRatioStep);
			Assert::IsFalse(sm.IsPending());

			// Invalid negative timeUs & NaN ratioStep: rejected visibly.
			const auto oNegT = sm.OnAbrStep(-1, 0.25);
			Assert::IsTrue(oNegT.reason == StepMarkReason::InvalidInput);
			const auto oNan = sm.OnAbrStep(5 * 1000000, std::numeric_limits<double>::quiet_NaN());
			Assert::IsTrue(oNan.reason == StepMarkReason::InvalidInput);
			Assert::AreEqual(uint64_t(2), sm.Counters().rejectedInvalidInput);
		}

		TEST_METHOD(PendingMarkBlocksEntryMoveAndCoalescesDuplicates)
		{
			using namespace StepMarkFx;
			ChainStepMark sm(Enabled());

			const auto o1 = sm.OnAbrStep(10 * 1000000, 0.25);
			Assert::IsTrue(o1.action == StepMarkAction::MarkPending);
			Assert::IsTrue(o1.reason == StepMarkReason::MarkRaised);
			Assert::IsTrue(o1.blocksEntryOrMove);
			Assert::IsTrue(o1.pullNextEval);
			Assert::IsTrue(sm.BlocksEntryOrMove());
			Assert::IsTrue(sm.PullNextEval());
			Assert::AreEqual(int64_t(10 * 1000000), o1.actionFromTimeUs);
			Assert::AreEqual(uint64_t(1), sm.Counters().marksRaised);

			// Duplicate ABR step while pending is coalesced visibly (never resets t0).
			const auto o2 = sm.OnAbrStep(11 * 1000000, -0.25);
			Assert::IsTrue(o2.action == StepMarkAction::None);
			Assert::IsTrue(o2.reason == StepMarkReason::AlreadyPending);
			Assert::AreEqual(int64_t(10 * 1000000), sm.MarkTimeUs());
			Assert::AreEqual(uint64_t(1), sm.Counters().coalescedWhilePending);
		}

		TEST_METHOD(UnconfirmedSteadyMarksMergeBucketByteIdenticallyToNoMark)
		{
			using namespace StepMarkFx;
			ChainStepMark sm(Enabled());
			const StepMarkLatchContext ctx = Latched1080();

			// Compare a reference accumulator (no marks) against a mark-guarded accumulator subjected to
			// 80 false steady-state ABR marks over a simulated hour (~80/h noise rate).
			double refCreditX = 0.0, refCreditY = 0.0;
			uint64_t refCount = 0;

			double guardedCreditX = 0.0, guardedCreditY = 0.0;
			uint64_t guardedCount = 0;
			uint64_t falseReleases = 0;

			// In-test negative control (Rule 3): a naive blind ABR-reset baseline (releasing & wiping credit on
			// every |ratioStep| >= 0.10 mark without 2-eval confirmation) on the EXACT SAME 80 steady-state marks.
			double naiveBlindCreditX = 0.0;
			uint64_t naiveFalseReleases = 0;

			uint64_t seq = 1;
			for (int m = 0; m < 80; ++m)
			{
				const int64_t tBase = static_cast<int64_t>(m) * 45;
				const auto raised = sm.OnAbrStep(tBase * 1000000, 0.15);
				Assert::IsTrue(raised.action == StepMarkAction::MarkPending);

				// Naive blind baseline immediately drops latch & clears accumulated credit on every ABR mark!
				++naiveFalseReleases;
				naiveBlindCreditX = 0.0;

				// Two non-contradicting post-t0 evaluations agreeing with 1920x1080.
				for (int e = 0; e < 2; ++e)
				{
					const double cr = 1.25 + 0.1 * static_cast<double>((m + e) % 5);
					const TentativeEvalRecord rec = EvalRec(tBase + e + 1, seq++, 1920, 1080, cr, false, false);

					// Reference path commits directly.
					refCreditX += rec.creditX;
					refCreditY += rec.creditY;
					++refCount;

					naiveBlindCreditX += rec.creditX;

					// Guarded path routes through StepMark tentative bucket.
					const auto out = sm.OnChainEvaluation(rec, ctx);
					if (out.action == StepMarkAction::ConfirmVoidAndRelease || out.action == StepMarkAction::ChallengeExpiredRelease)
						++falseReleases;

					if (e == 0)
					{
						Assert::IsTrue(sm.IsPending());
						Assert::IsTrue(out.blocksEntryOrMove);
					}
					else
					{
						Assert::IsTrue(out.action == StepMarkAction::CancelAndMergeBucket);
						Assert::IsTrue(out.reason == StepMarkReason::UnconfirmedTimeout);
						Assert::AreEqual(size_t(2), out.bucket.size());
						for (const auto& b : out.bucket)
						{
							guardedCreditX += b.creditX;
							guardedCreditY += b.creditY;
							++guardedCount;
						}
					}
				}
			}

			// Negative-control proof: naive blind ABR reset breaks catastrophically on the exact same input!
			Assert::AreEqual(uint64_t(80), naiveFalseReleases);
			Assert::IsTrue(naiveBlindCreditX < 0.05 * refCreditX);

			Assert::AreEqual(uint64_t(0), falseReleases);
			Assert::AreEqual(uint64_t(80), sm.Counters().unconfirmedTimeouts);
			Assert::AreEqual(refCount, guardedCount);
			// Exact bit-for-bit equality (same summation order in bucket merge as direct path).
			Assert::AreEqual(refCreditX, guardedCreditX);
			Assert::AreEqual(refCreditY, guardedCreditY);
		}

		TEST_METHOD(ConfirmedMarkVoidsPreStepCreditAndPreservesOnlyPostBucket)
		{
			using namespace StepMarkFx;
			ChainStepMark sm(Enabled());

			// Simulated accumulator with pre-t0 credits at t = 1..9 s (total = 18.0 >= c1 = 15.0).
			struct Entry { int64_t timeUs; double credit; };
			std::vector<Entry> committed;
			for (int s = 1; s <= 9; ++s)
				committed.push_back({ s * 1000000, 2.0 });

			const int64_t t0 = 10 * 1000000;
			sm.OnAbrStep(t0, 0.25);

			// Eval at t = 11 s contradicts pre-step 540p -> confirms mark.
			const auto out = sm.OnChainEvaluation(EvalRec(11, 10, 1920, 1080, 3.5, true), Latched540());
			Assert::IsTrue(out.action == StepMarkAction::ConfirmVoidAndRelease);
			Assert::IsTrue(out.reason == StepMarkReason::ConfirmedByChainContradiction);
			Assert::AreEqual(t0, out.actionFromTimeUs);
			Assert::AreEqual(size_t(1), out.bucket.size());
			Assert::AreEqual(int64_t(11 * 1000000), out.bucket[0].timeUs);

			// In-test negative control (Rule 3): without backward voiding to t0, stale pre-step credit leaks past c1 (15.0)!
			double naiveUnvoidedTotal = 0.0;
			for (const auto& e : committed)
				naiveUnvoidedTotal += e.credit;
			for (const auto& b : out.bucket)
				naiveUnvoidedTotal += b.creditX;
			Assert::IsTrue(naiveUnvoidedTotal >= 15.0, L"negative control: without backward voiding, stale pre-step credit crosses c1");

			// Apply backward void to t0: remove all entries < actionFromTimeUs, keep only post-t0 bucket.
			committed.erase(
				std::remove_if(committed.begin(), committed.end(), [&](const Entry& e) { return e.timeUs < out.actionFromTimeUs; }),
				committed.end());
			for (const auto& b : out.bucket)
				committed.push_back({ b.timeUs, b.creditX });

			Assert::AreEqual(size_t(1), committed.size());
			Assert::AreEqual(int64_t(11 * 1000000), committed[0].timeUs);
			Assert::AreEqual(3.5, committed[0].credit);
			Assert::IsTrue(committed[0].credit < 15.0, L"post-step credit alone does not inherit stale pre-step c1");
		}

		TEST_METHOD(ConfirmedStepToLargerTruthReleasesImmediatelyByClassicalOrChain)
		{
			using namespace StepMarkFx;
			// (a) Confirmed by 2 consecutive fresh informative classical frames at 1920x1080 (larger than 960x540).
			{
				ChainStepMark sm(Enabled());
				const StepMarkLatchContext ctx = Latched540();
				sm.OnAbrStep(20 * 1000000, 0.25);

				// Pre-t0 frame and voided frame must NOT count toward confirmation.
				const auto oPre = sm.OnClassicalFrame(ClassicalFrame(19, 1920, 1080), ctx);
				Assert::IsTrue(oPre.action == StepMarkAction::None);
				const auto oVoid = sm.OnClassicalFrame(ClassicalFrame(21, 1920, 1080, true, true), ctx);
				Assert::IsTrue(oVoid.action == StepMarkAction::None);

				// 1st valid post-t0 frame: still pending (needs 2 consecutive).
				const auto oF1 = sm.OnClassicalFrame(ClassicalFrame(21, 1920, 1080), ctx);
				Assert::IsTrue(oF1.action == StepMarkAction::None);
				Assert::IsTrue(sm.IsPending());

				// 2nd consecutive valid post-t0 frame at 1920x1080: immediate release!
				const auto oF2 = sm.OnClassicalFrame(ClassicalFrame(22, 1920, 1080), ctx);
				Assert::IsTrue(oF2.action == StepMarkAction::ConfirmVoidAndRelease);
				Assert::IsTrue(oF2.reason == StepMarkReason::ConfirmedByClassicalPair);
				Assert::IsTrue((oF2.reasonBits & kReasonLatchRelease) != 0u);
				Assert::AreEqual(uint64_t(1), sm.Counters().confirmedByClassical);
				Assert::AreEqual(uint64_t(1), sm.Counters().immediateReleases);
				Assert::IsFalse(sm.IsPending());
			}

			// (b) Confirmed step-up to native 4K (larger than 1920x1080) via chain native reading.
			{
				ChainStepMark sm(Enabled());
				sm.OnAbrStep(30 * 1000000, 0.50);
				const auto out = sm.OnChainEvaluation(EvalRec(31, 1, 0, 0, 0.0, false, true), Latched1080());
				Assert::IsTrue(out.action == StepMarkAction::ConfirmVoidAndRelease);
				Assert::AreEqual(uint64_t(1), sm.Counters().immediateReleases);
			}
		}

		TEST_METHOD(ConfirmedStepToSmallerTruthChallengesForDwellPlusEvalThenReleases)
		{
			using namespace StepMarkFx;
			ChainStepMark sm(Enabled());
			const StepMarkLatchContext ctx = Latched1080();

			sm.OnAbrStep(10 * 1000000, -0.25);
			// Step-down from 1920x1080 -> 960x540: opens challenge instead of immediate mid-shot release.
			const auto oConf = sm.OnChainEvaluation(EvalRec(11, 1, 960, 540, 2.0, true), ctx);
			Assert::IsTrue(oConf.action == StepMarkAction::ConfirmVoidAndChallenge);
			Assert::IsTrue(sm.IsChallenging());
			Assert::AreEqual(uint64_t(1), sm.Counters().challengesOpened);

			// Before 3 s dwell (t = 13 s, elapsed 2 s): still challenging.
			const auto oWait1 = sm.OnChainEvaluation(EvalRec(13, 2, 960, 540, 2.0), ctx, false);
			Assert::IsTrue(oWait1.action == StepMarkAction::None);
			Assert::IsTrue(sm.IsChallenging());

			// At dwell end (t = 14 s, elapsed 3 s): 1 extra evaluation allowed (N2 rule).
			const auto oExtra = sm.OnChainEvaluation(EvalRec(14, 3, 960, 540, 2.0), ctx, false);
			Assert::IsTrue(oExtra.action == StepMarkAction::None);
			Assert::IsTrue(sm.IsChallenging());

			// Next eval past dwell (t = 15 s): ChallengeExpiredRelease fires!
			const auto oRel = sm.OnChainEvaluation(EvalRec(15, 4, 960, 540, 2.0), ctx, false);
			Assert::IsTrue(oRel.action == StepMarkAction::ChallengeExpiredRelease);
			Assert::IsTrue(oRel.reason == StepMarkReason::ChallengeExpired);
			Assert::IsTrue((oRel.reasonBits & kReasonLatchRelease) != 0u);
			Assert::IsFalse(sm.IsChallenging());
			Assert::AreEqual(uint64_t(1), sm.Counters().challengeReleases);

			// Verify re-confirmation during challenge clears challenge without releasing.
			sm.OnAbrStep(30 * 1000000, -0.25);
			sm.OnChainEvaluation(EvalRec(31, 5, 960, 540, 2.0, true), ctx);
			Assert::IsTrue(sm.IsChallenging());
			const auto oClr = sm.OnChainEvaluation(EvalRec(32, 6, 1920, 1080, 4.0), ctx, true);
			Assert::IsTrue(oClr.action == StepMarkAction::None);
			Assert::IsTrue(oClr.reason == StepMarkReason::ChallengeClearedByReconfirm);
			Assert::IsFalse(sm.IsChallenging());
			Assert::AreEqual(uint64_t(1), sm.Counters().challengesCleared);
		}

		TEST_METHOD(MarksOnNativeStreamAddZeroFalseEntriesAndNeverRelease)
		{
			using namespace StepMarkFx;
			ChainStepMark sm(Enabled());
			StepMarkLatchContext unlatched; // latched == false

			// Unconfirmed mark on native: merges bucket back, no release.
			sm.OnAbrStep(10 * 1000000, -0.20);
			sm.OnChainEvaluation(EvalRec(11, 1, 0, 0, 0.0, false, false), unlatched);
			const auto oUnconf = sm.OnChainEvaluation(EvalRec(12, 2, 0, 0, 0.0, false, false), unlatched);
			Assert::IsTrue(oUnconf.action == StepMarkAction::CancelAndMergeBucket);
			Assert::AreEqual(uint64_t(0), sm.Counters().immediateReleases);

			// Confirmed mark on native: ConfirmVoidOnly (voids stale pre-t0, never enters/releases).
			sm.OnAbrStep(20 * 1000000, 0.25);
			const auto oConf = sm.OnChainEvaluation(EvalRec(21, 3, 1920, 1080, 1.0, true, false), unlatched);
			Assert::IsTrue(oConf.action == StepMarkAction::ConfirmVoidOnly);
			Assert::AreEqual(uint64_t(0), sm.Counters().immediateReleases);
			Assert::AreEqual(uint64_t(0), sm.Counters().challengesOpened);
		}

		TEST_METHOD(ResetOnCutDiscontinuityAndBackwardTimeIsCleanAndVisible)
		{
			using namespace StepMarkFx;
			ChainStepMark sm(Enabled());

			sm.OnAbrStep(50 * 1000000, 0.25);
			Assert::IsTrue(sm.IsPending());
			const auto oCut = sm.Reset(StepMarkReason::ResetOnCut);
			Assert::IsTrue(oCut.reason == StepMarkReason::ResetOnCut);
			Assert::IsFalse(sm.IsPending());
			Assert::AreEqual(uint64_t(1), sm.Counters().resetsOnCut);

			sm.OnAbrStep(60 * 1000000, 0.25);
			const auto oDisc = sm.Reset(StepMarkReason::ResetDiscontinuity);
			Assert::IsTrue(oDisc.reason == StepMarkReason::ResetDiscontinuity);
			Assert::IsFalse(sm.IsPending());
			Assert::AreEqual(uint64_t(1), sm.Counters().resetsDiscontinuity);

			// Backward source time while pending triggers explicit BackwardTimeReset.
			sm.OnAbrStep(100 * 1000000, 0.25);
			const auto oBack = sm.OnChainEvaluation(EvalRec(90, 1, 1920, 1080, 1.0), Latched1080());
			Assert::IsTrue(oBack.reason == StepMarkReason::BackwardTimeReset);
			Assert::IsFalse(sm.IsPending());
			Assert::AreEqual(uint64_t(1), sm.Counters().resetsBackwardTime);
		}

		TEST_METHOD(SteadyNativeStreamWithChainSeesNativeNeverFalselyConfirmsAbrMark)
		{
			using namespace StepMarkFx;
			// HIGH-1 regression: steady native stream (!ctx.latched) with chainSeesNative == true MUST cancel
			// unconfirmed after confirmWindowEvals rather than falsely confirming every ABR mark.
			ChainStepMark sm(Enabled());
			StepMarkLatchContext unlatched;

			sm.OnAbrStep(10 * 1000000, -0.20);
			const auto o1 = sm.OnChainEvaluation(EvalRec(11, 1, 0, 0, 0.0, false, true), unlatched);
			Assert::IsTrue(sm.IsPending());
			Assert::IsTrue(o1.action == StepMarkAction::None);

			const auto o2 = sm.OnChainEvaluation(EvalRec(12, 2, 0, 0, 0.0, false, true), unlatched);
			Assert::IsTrue(o2.action == StepMarkAction::CancelAndMergeBucket);
			Assert::IsTrue(o2.reason == StepMarkReason::UnconfirmedTimeout);
			Assert::AreEqual(uint64_t(0), sm.Counters().confirmedByChain);
			Assert::AreEqual(uint64_t(1), sm.Counters().unconfirmedTimeouts);
		}

		TEST_METHOD(SpuriousAbrMarkDuringActiveChallengePreservesAndResumesChallenge)
		{
			using namespace StepMarkFx;
			// HIGH-2 regression: spurious OnAbrStep during Challenging must NOT wipe or reset the active challenge;
			// when the spurious mark times out unconfirmed, the original challenge resumes with its original deadline.
			ChainStepMark sm(Enabled());
			const StepMarkLatchContext ctx = Latched1080();

			sm.OnAbrStep(10 * 1000000, -0.25);
			const auto oConf = sm.OnChainEvaluation(EvalRec(11, 1, 960, 540, 2.0, true), ctx);
			Assert::IsTrue(oConf.action == StepMarkAction::ConfirmVoidAndChallenge);
			Assert::IsTrue(sm.IsChallenging());

			// Spurious ABR mark interrupts at t = 12 s.
			const auto oSpur = sm.OnAbrStep(12 * 1000000, 0.15);
			Assert::IsTrue(oSpur.action == StepMarkAction::MarkPending);
			Assert::IsTrue(sm.IsPending());

			// Two non-contradicting evals at t = 13 s, 14 s -> cancels spurious mark & resumes original challenge!
			sm.OnChainEvaluation(EvalRec(13, 2, 1920, 1080, 1.0, false), ctx);
			const auto oResume = sm.OnChainEvaluation(EvalRec(14, 3, 1920, 1080, 1.0, false), ctx);
			Assert::IsTrue(oResume.action == StepMarkAction::CancelAndMergeBucket);
			Assert::IsTrue(oResume.state == StepMarkState::Challenging);
			Assert::IsTrue(sm.IsChallenging());
			Assert::AreEqual(uint64_t(1), sm.Counters().challengesResumedAfterUnconfirmed);

			// Because original challengeStartUs_ (11 s) was preserved, at t = 15 s (elapsed 4 s >= 3 s) it consumes
			// its 1 extra eval and releases on t = 16 s!
			const auto oExtra = sm.OnChainEvaluation(EvalRec(15, 4, 960, 540, 1.0, false), ctx);
			Assert::IsTrue(oExtra.action == StepMarkAction::None);
			Assert::IsTrue(sm.IsChallenging());
			const auto oRel = sm.OnChainEvaluation(EvalRec(16, 5, 960, 540, 1.0, false), ctx);
			Assert::IsTrue(oRel.action == StepMarkAction::ChallengeExpiredRelease);
			Assert::AreEqual(uint64_t(1), sm.Counters().challengeReleases);
		}

		TEST_METHOD(VoidedFrameResetsClassicalStreakAndUnlatchedSteady1080pDoesNotSelfVoid)
		{
			using namespace StepMarkFx;
			// MEDIUM-2a: a voided classical frame between two valid frames resets the consecutive streak.
			{
				ChainStepMark sm(Enabled());
				const StepMarkLatchContext ctx = Latched540();
				sm.OnAbrStep(10 * 1000000, 0.25);

				Assert::IsTrue(sm.OnClassicalFrame(ClassicalFrame(11, 1920, 1080), ctx).action == StepMarkAction::None);
				// Voided frame breaks streak!
				Assert::IsTrue(sm.OnClassicalFrame(ClassicalFrame(12, 1920, 1080, true, true), ctx).action == StepMarkAction::None);
				// Next single valid frame must be streak == 1 (NOT confirm yet).
				Assert::IsTrue(sm.OnClassicalFrame(ClassicalFrame(13, 1920, 1080), ctx).action == StepMarkAction::None);
				Assert::IsTrue(sm.IsPending());
				// Second consecutive valid frame confirms!
				const auto oConf = sm.OnClassicalFrame(ClassicalFrame(14, 1920, 1080), ctx);
				Assert::IsTrue(oConf.action == StepMarkAction::ConfirmVoidAndRelease);
			}

			// MEDIUM-2b: unlatched acquiring steady 1080p stream does NOT self-void on false ABR mark.
			{
				ChainStepMark sm(Enabled());
				StepMarkLatchContext unlatched; // no latched pair
				sm.OnAbrStep(20 * 1000000, 0.20);
				Assert::IsTrue(sm.OnClassicalFrame(ClassicalFrame(21, 1920, 1080), unlatched).action == StepMarkAction::None);
				Assert::IsTrue(sm.OnClassicalFrame(ClassicalFrame(22, 1920, 1080), unlatched).action == StepMarkAction::None);
				Assert::IsTrue(sm.IsPending());
				Assert::AreEqual(uint64_t(0), sm.Counters().confirmedByClassical);
			}
		}

		TEST_METHOD(AdversarialClassicalNonConventionalMismatchInfAndBucketOverflowCap)
		{
			using namespace StepMarkFx;
			const double inf = std::numeric_limits<double>::infinity();

			// 1. +Inf / -Inf ratioStep rejected visibly without raising a mark.
			{
				ChainStepMark sm(Enabled());
				Assert::IsTrue(sm.OnAbrStep(10 * 1000000, inf).reason == StepMarkReason::InvalidInput);
				Assert::IsTrue(sm.OnAbrStep(10 * 1000000, -inf).reason == StepMarkReason::InvalidInput);
				Assert::IsFalse(sm.IsPending());
				Assert::AreEqual(uint64_t(2), sm.Counters().rejectedInvalidInput);
			}

			// 2. Non-conventional (1600x900) and parked anamorphic (1440x1080) classical frames MUST NEVER confirm.
			{
				ChainStepMark sm(Enabled());
				const StepMarkLatchContext ctx = Latched540();
				sm.OnAbrStep(10 * 1000000, 0.25);
				Assert::IsTrue(sm.OnClassicalFrame(ClassicalFrame(11, 1600, 900), ctx).action == StepMarkAction::None);
				Assert::IsTrue(sm.OnClassicalFrame(ClassicalFrame(12, 1600, 900), ctx).action == StepMarkAction::None);
				Assert::IsTrue(sm.OnClassicalFrame(ClassicalFrame(13, 1440, 1080), ctx).action == StepMarkAction::None);
				Assert::IsTrue(sm.OnClassicalFrame(ClassicalFrame(14, 1440, 1080), ctx).action == StepMarkAction::None);
				Assert::IsTrue(sm.IsPending());
				Assert::AreEqual(uint64_t(0), sm.Counters().confirmedByClassical);
			}

			// 3. Classical reading matching latched pair OR alternating different pairs must NOT confirm until 2 matching consecutive frames.
			{
				ChainStepMark sm(Enabled());
				const StepMarkLatchContext ctx = Latched1080();
				sm.OnAbrStep(10 * 1000000, -0.25);
				// Same as latched (1920x1080): never confirms.
				Assert::IsTrue(sm.OnClassicalFrame(ClassicalFrame(11, 1920, 1080), ctx).action == StepMarkAction::None);
				Assert::IsTrue(sm.OnClassicalFrame(ClassicalFrame(12, 1920, 1080), ctx).action == StepMarkAction::None);
				// Alternating 1280x720 then 960x540: streak resets to 1, does NOT confirm.
				Assert::IsTrue(sm.OnClassicalFrame(ClassicalFrame(13, 1280, 720), ctx).action == StepMarkAction::None);
				Assert::IsTrue(sm.OnClassicalFrame(ClassicalFrame(14, 960, 540), ctx).action == StepMarkAction::None);
				Assert::IsTrue(sm.IsPending());
				// Second consecutive 960x540 confirms step-down challenge!
				const auto oConf = sm.OnClassicalFrame(ClassicalFrame(15, 960, 540), ctx);
				Assert::IsTrue(oConf.action == StepMarkAction::ConfirmVoidAndChallenge);
				Assert::AreEqual(uint64_t(1), sm.Counters().confirmedByClassical);
			}

			// 4. Bounded tentative bucket enforces maxBucketRecords visibly (Rule 2 + Rule 4).
			{
				StepMarkSettings cfg = Enabled();
				cfg.maxBucketRecords = 4;
				cfg.confirmWindowEvals = 6;
				ChainStepMark sm(cfg);
				const StepMarkLatchContext ctx = Latched1080();
				sm.OnAbrStep(10 * 1000000, 0.20);
				for (int e = 1; e <= 5; ++e)
					Assert::IsTrue(sm.OnChainEvaluation(EvalRec(10 + e, static_cast<uint64_t>(e), 1920, 1080, 1.0, false), ctx).action == StepMarkAction::None);
				const auto oEnd = sm.OnChainEvaluation(EvalRec(16, 6, 1920, 1080, 1.0, false), ctx);
				Assert::IsTrue(oEnd.action == StepMarkAction::CancelAndMergeBucket);
				Assert::AreEqual(size_t(4), oEnd.bucket.size());
				Assert::AreEqual(uint64_t(3), oEnd.bucket.front().evalSeq, L"oldest 2 records evicted FIFO");
				Assert::AreEqual(uint64_t(6), oEnd.bucket.back().evalSeq);
				Assert::AreEqual(uint64_t(2), sm.Counters().bucketOverflowDrops);
			}
		}

		// H1 Red-Team proving test: blind-classical native reading (nativeReading == true, positiveDetailAbove == false)
		// MUST NEVER confirm a pending ABR step mark (0 false releases across 10 spurious marks while latched),
		// with a same-input negative control proving genuine positiveDetailAbove == true on 2 consecutive fresh frames
		// DOES confirm and immediately release!
		TEST_METHOD(H1BlindClassicalNativeWithoutPositiveDetailAboveNeverConfirmsStepMark)
		{
			using namespace StepMarkFx;

			// Contract default check: StepMarkClassicalFrame::positiveDetailAbove defaults to false.
			const StepMarkClassicalFrame defFrame;
			Assert::IsFalse(defFrame.positiveDetailAbove, L"H1: StepMarkClassicalFrame::positiveDetailAbove must default to false");

			// 1. Latched 1920x1080 + 10 spurious ABR marks + steady blind-classical native (positiveDetailAbove = false):
			//    MUST produce 0 confirmations and 0 false releases!
			{
				ChainStepMark sm(Enabled());
				const StepMarkLatchContext ctx = Latched1080();
				uint64_t seq = 1;
				for (int m = 0; m < 10; ++m)
				{
					const int64_t t0 = static_cast<int64_t>(10 + m * 20);
					const auto raised = sm.OnAbrStep(t0 * 1000000, +0.50);
					Assert::IsTrue(raised.action == StepMarkAction::MarkPending);

					for (int f = 1; f <= 2; ++f)
					{
						StepMarkClassicalFrame blindNative = ClassicalFrame(t0 + f, 0, 0, true, false, true);
						blindNative.positiveDetailAbove = false; // blind classical reading native with NO positive above-band detail
						const auto oCf = sm.OnClassicalFrame(blindNative, ctx);
						Assert::IsTrue(oCf.action == StepMarkAction::None,
							L"H1: blind-classical native (positiveDetailAbove=false) must never confirm pending ABR mark");
						Assert::IsTrue(sm.IsPending());
					}

					// Two steady non-contradicting chain evals at 1920x1080 -> unconfirmed timeout, bucket merged!
					Assert::IsTrue(sm.OnChainEvaluation(EvalRec(t0 + 1, seq++, 1920, 1080, 1.5, false, false), ctx).action == StepMarkAction::None);
					const auto oEnd = sm.OnChainEvaluation(EvalRec(t0 + 2, seq++, 1920, 1080, 1.5, false, false), ctx);
					Assert::IsTrue(oEnd.action == StepMarkAction::CancelAndMergeBucket);
					Assert::IsTrue(oEnd.reason == StepMarkReason::UnconfirmedTimeout);
				}

				Assert::AreEqual(uint64_t(10), sm.Counters().marksRaised);
				Assert::AreEqual(uint64_t(10), sm.Counters().unconfirmedTimeouts);
				Assert::AreEqual(uint64_t(0), sm.Counters().confirmedByClassical);
				Assert::AreEqual(uint64_t(0), sm.Counters().immediateReleases);
			}

			// 2. Mandatory Rule 3 Negative Control on EXACT SAME latched context & native frames:
			//    setting positiveDetailAbove = true (genuine above-band detail measured) on 2 consecutive fresh frames
			//    DOES confirm via ConfirmedByClassicalPair and immediately releases!
			{
				ChainStepMark sm(Enabled());
				const StepMarkLatchContext ctx = Latched1080();
				Assert::IsTrue(sm.OnAbrStep(10 * 1000000, +0.50).action == StepMarkAction::MarkPending);

				StepMarkClassicalFrame det1 = ClassicalFrame(11, 0, 0, true, false, true);
				det1.positiveDetailAbove = true;
				const auto o1 = sm.OnClassicalFrame(det1, ctx);
				Assert::IsTrue(o1.action == StepMarkAction::None, L"1st positiveDetailAbove native frame waits for 2nd consecutive frame");
				Assert::IsTrue(sm.IsPending());

				StepMarkClassicalFrame det2 = ClassicalFrame(12, 0, 0, true, false, true);
				det2.positiveDetailAbove = true;
				const auto o2 = sm.OnClassicalFrame(det2, ctx);
				Assert::IsTrue(o2.action == StepMarkAction::ConfirmVoidAndRelease,
					L"negative control: 2 consecutive native frames WITH positiveDetailAbove=true confirm and release immediately");
				Assert::IsTrue(o2.reason == StepMarkReason::ConfirmedByClassicalPair);
				Assert::AreEqual(uint64_t(1), sm.Counters().confirmedByClassical);
				Assert::AreEqual(uint64_t(1), sm.Counters().immediateReleases);
				Assert::IsFalse(sm.IsPending());
			}

			// 3. Unlatched context (!ctx.latched): even nativeReading=true + positiveDetailAbove=true does NOT falsely confirm.
			{
				ChainStepMark sm(Enabled());
				StepMarkLatchContext unlatched;
				Assert::IsTrue(sm.OnAbrStep(50 * 1000000, +0.50).action == StepMarkAction::MarkPending);

				StepMarkClassicalFrame det1 = ClassicalFrame(51, 0, 0, true, false, true);
				det1.positiveDetailAbove = true;
				StepMarkClassicalFrame det2 = ClassicalFrame(52, 0, 0, true, false, true);
				det2.positiveDetailAbove = true;
				Assert::IsTrue(sm.OnClassicalFrame(det1, unlatched).action == StepMarkAction::None);
				Assert::IsTrue(sm.OnClassicalFrame(det2, unlatched).action == StepMarkAction::None);
				Assert::IsTrue(sm.IsPending());
				Assert::AreEqual(uint64_t(0), sm.Counters().confirmedByClassical);
			}
		}
	};
}
