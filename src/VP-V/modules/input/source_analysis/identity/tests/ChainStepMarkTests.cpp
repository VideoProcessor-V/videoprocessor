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

			uint64_t seq = 1;
			for (int m = 0; m < 80; ++m)
			{
				const int64_t tBase = static_cast<int64_t>(m) * 45;
				const auto raised = sm.OnAbrStep(tBase * 1000000, 0.15);
				Assert::IsTrue(raised.action == StepMarkAction::MarkPending);

				// Two non-contradicting post-t0 evaluations agreeing with 1920x1080.
				for (int e = 0; e < 2; ++e)
				{
					const double cr = 1.25 + 0.1 * static_cast<double>((m + e) % 5);
					const TentativeEvalRecord rec = EvalRec(tBase + e + 1, seq++, 1920, 1080, cr, false, false);

					// Reference path commits directly.
					refCreditX += rec.creditX;
					refCreditY += rec.creditY;
					++refCount;

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

			// Simulated accumulator with pre-t0 credits at t = 1..9 s.
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

			// Apply backward void to t0: remove all entries < actionFromTimeUs, keep only post-t0 bucket.
			committed.erase(
				std::remove_if(committed.begin(), committed.end(), [&](const Entry& e) { return e.timeUs < out.actionFromTimeUs; }),
				committed.end());
			for (const auto& b : out.bucket)
				committed.push_back({ b.timeUs, b.creditX });

			Assert::AreEqual(size_t(1), committed.size());
			Assert::AreEqual(int64_t(11 * 1000000), committed[0].timeUs);
			Assert::AreEqual(3.5, committed[0].credit);
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
	};
}
