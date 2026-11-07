// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "CppUnitTest.h"

// FX-RB: the re-base shader warm-up gate (VP-V/modules/scaling/rebase/RebaseWarmup.h). CPU only, no
// clock: the caller passes seconds. A wanted re-base whose shader variants are not compiled stays on
// the current path (allow = false) with a visible line; it proceeds only when the worker says Ready.

#include <VP-V/modules/scaling/rebase/RebaseWarmup.h>

#include <string>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using namespace VpRebaseWarmup;

namespace RebaseWarmupTest
{
	TEST_CLASS(RebaseWarmupGateTests)
	{
	public:
		TEST_METHOD(NothingWantedChangesNothing)
		{
			Gate gate;
			const Step step = gate.Evaluate(false, "k", "1920x1080", Status::Missing, 1.0);
			Assert::IsTrue(step.allow);
			Assert::IsFalse(step.submit);
			Assert::IsFalse(step.pending);
			Assert::IsTrue(step.log.empty());
			Assert::IsTrue(step.status.empty());
		}

		TEST_METHOD(UncompiledCandidateHoldsSubmitsOnceAndAnnounces)
		{
			Gate gate;
			const Step first = gate.Evaluate(true, "k", "1920x1080", Status::Missing, 10.0);
			Assert::IsFalse(first.allow, L"held on the current path");
			Assert::IsTrue(first.submit);
			Assert::IsTrue(first.pending);
			Assert::IsTrue(first.log.find("pending shader warm-up") != std::string::npos);
			Assert::IsTrue(first.status.find("1920x1080") != std::string::npos);
			const Step second = gate.Evaluate(true, "k", "1920x1080", Status::Pending, 10.5);
			Assert::IsFalse(second.allow);
			Assert::IsFalse(second.submit, L"submitted once");
			Assert::IsTrue(second.log.empty(), L"announced once");
			Assert::IsFalse(second.status.empty(), L"but still visible");
		}

		TEST_METHOD(ReadyReleasesAndRemembers)
		{
			Gate gate;
			gate.Evaluate(true, "k", "1920x1080", Status::Missing, 10.0);
			const Step ready = gate.Evaluate(true, "k", "1920x1080", Status::Ready, 12.5);
			Assert::IsTrue(ready.allow);
			Assert::IsTrue(ready.log.find("ready") != std::string::npos);
			Assert::IsTrue(gate.IsKnownReady("k"));
			// Even if the worker's publication was replaced, a remembered key stays allowed.
			const Step later = gate.Evaluate(true, "k", "1920x1080", Status::Missing, 20.0);
			Assert::IsTrue(later.allow);
			Assert::IsFalse(later.submit);
		}

		TEST_METHOD(AlreadyCompiledCandidateIsNeverHeldOrAnnounced)
		{
			Gate gate;
			const Step step = gate.Evaluate(true, "k", "1920x1080", Status::Ready, 1.0);
			Assert::IsTrue(step.allow);
			Assert::IsTrue(step.log.empty());
			Assert::IsTrue(step.status.empty());
		}

		TEST_METHOD(NewCandidateRestartsTheWait)
		{
			Gate gate;
			gate.Evaluate(true, "a", "1920x1080", Status::Pending, 0.0);
			const Step step = gate.Evaluate(true, "b", "1280x720", Status::Missing, 1.0);
			Assert::IsFalse(step.allow);
			Assert::IsTrue(step.submit);
			Assert::IsTrue(step.status.find("1280x720") != std::string::npos);
		}

		TEST_METHOD(FailureHoldsThenRetriesWithBackoffThenLetsThroughVisibly)
		{
			Gate gate;
			gate.Evaluate(true, "k", "1920x1080", Status::Missing, 0.0);
			const Step s1 = gate.Evaluate(true, "k", "1920x1080", Status::Failed, 1.0);
			Assert::IsFalse(s1.allow);
			Assert::IsTrue(s1.failed);
			Assert::IsTrue(s1.submit, L"first retry at once");
			const Step s2 = gate.Evaluate(true, "k", "1920x1080", Status::Failed, 2.0);
			Assert::IsFalse(s2.allow);
			Assert::IsFalse(s2.submit, L"back-off: not before 5 s");
			const Step s3 = gate.Evaluate(true, "k", "1920x1080", Status::Failed, 6.5);
			Assert::IsFalse(s3.allow);
			Assert::IsTrue(s3.submit, L"second retry after the back-off");
			const Step s4 = gate.Evaluate(true, "k", "1920x1080", Status::Failed, 17.0);
			Assert::IsTrue(s4.allow, L"after three failures the re-base compiles on demand");
			Assert::IsTrue(s4.log.find("on demand") != std::string::npos);
			Assert::IsTrue(s4.status.find("on demand") != std::string::npos);
			const Step s5 = gate.Evaluate(true, "k", "1920x1080", Status::Failed, 18.0);
			Assert::IsTrue(s5.allow);
			Assert::IsTrue(s5.log.empty(), L"logged once");
			Assert::IsFalse(s5.status.empty(), L"status stays visible");
		}

		TEST_METHOD(SlowWarmupKeepsHoldingAndSaysSoOnce)
		{
			Gate gate;
			gate.Evaluate(true, "k", "1920x1080", Status::Missing, 0.0);
			const Step slow = gate.Evaluate(true, "k", "1920x1080", Status::Pending, Gate::kTimeoutSeconds + 1.0);
			Assert::IsFalse(slow.allow);
			Assert::IsTrue(slow.failed);
			Assert::IsTrue(slow.log.find("taking too long") != std::string::npos);
			const Step again = gate.Evaluate(true, "k", "1920x1080", Status::Pending, Gate::kTimeoutSeconds + 2.0);
			Assert::IsFalse(again.allow);
			Assert::IsTrue(again.log.empty());
			const Step done = gate.Evaluate(true, "k", "1920x1080", Status::Ready, Gate::kTimeoutSeconds + 3.0);
			Assert::IsTrue(done.allow, L"a late completion is still honoured");
		}

		TEST_METHOD(ResetForgetsCompiledKeys)
		{
			Gate gate;
			gate.NoteReady("k");
			Assert::IsTrue(gate.IsKnownReady("k"));
			gate.Reset();
			Assert::IsFalse(gate.IsKnownReady("k"));
			Assert::IsFalse(gate.Evaluate(true, "k", "1920x1080", Status::Missing, 0.0).allow);
		}

		TEST_METHOD(ReadyKeysAreBounded)
		{
			Gate gate;
			for (int i = 0; i < 40; ++i) gate.NoteReady("k" + std::to_string(i));
			Assert::IsFalse(gate.IsKnownReady("k0"));
			Assert::IsTrue(gate.IsKnownReady("k39"));
		}

		TEST_METHOD(EmptyKeyIsNotGatedAndSaysSoOnce)
		{
			Gate gate;
			const Step first = gate.Evaluate(true, std::string(), "?", Status::Missing, 0.0);
			Assert::IsTrue(first.allow);
			Assert::IsTrue(first.bypass);
			Assert::IsFalse(first.log.empty(), L"never silent");
			Assert::IsFalse(first.status.empty());
			const Step second = gate.Evaluate(true, std::string(), "?", Status::Missing, 1.0);
			Assert::IsTrue(second.allow);
			Assert::IsTrue(second.log.empty(), L"logged once");
			Assert::IsFalse(second.status.empty(), L"status stays visible");
		}

		TEST_METHOD(SkipForAKnownReasonIsVisibleAndLoggedOncePerReason)
		{
			Gate gate;
			const Step first = gate.Skip("the shader cache is disabled", "1920x1080");
			Assert::IsTrue(first.allow);
			Assert::IsTrue(first.bypass);
			Assert::IsTrue(first.log.find("shader cache is disabled") != std::string::npos);
			Assert::IsTrue(first.status.find("on demand") != std::string::npos);
			Assert::IsTrue(gate.Skip("the shader cache is disabled", "1920x1080").log.empty());
			Assert::IsFalse(gate.Skip("another reason", "1920x1080").log.empty());
		}

		TEST_METHOD(TimeoutWordingDoesNotClaimNativeFrames)
		{
			Gate gate;
			gate.Evaluate(true, "k", "1920x1080", Status::Missing, 0.0);
			const Step slow = gate.Evaluate(true, "k", "1920x1080", Status::Pending, Gate::kTimeoutSeconds + 1.0);
			Assert::IsTrue(slow.status.find("native") == std::string::npos);
			Assert::IsTrue(slow.status.find("current path") != std::string::npos);
			Assert::IsTrue(slow.log.find("1 of 3") != std::string::npos);
		}

		// Finding 1(a): a warm-up that stays Pending is bounded; timeouts count as failures.
		TEST_METHOD(PendingForeverIsLetThroughVisiblyAfterTheBudget)
		{
			Gate gate;
			gate.Evaluate(true, "k", "1920x1080", Status::Missing, 0.0);
			for (double t = 10.0; t <= 130.0; t += 10.0)
				Assert::IsFalse(gate.Evaluate(true, "k", "1920x1080", Status::Pending, t).allow, L"held inside the budget");
			const Step late = gate.Evaluate(true, "k", "1920x1080", Status::Pending, 140.0);
			Assert::IsTrue(late.allow);
			Assert::IsTrue(late.bypass);
			Assert::IsTrue(late.status.find("on demand") != std::string::npos);
			Assert::IsTrue(late.log.find("on demand") != std::string::npos);
			const Step next = gate.Evaluate(true, "k", "1920x1080", Status::Pending, 141.0);
			Assert::IsTrue(next.allow);
			Assert::IsTrue(next.log.empty(), L"logged once");
			Assert::IsFalse(next.status.empty(), L"visible every frame");
		}

		TEST_METHOD(ThreeTimeoutsCountAsFailuresAndHandTheRebaseBackOnDemand)
		{
			Gate gate;
			gate.Evaluate(true, "k", "1920x1080", Status::Missing, 0.0);
			const Step t1 = gate.Evaluate(true, "k", "1920x1080", Status::Pending, 46.0);
			Assert::IsFalse(t1.allow);
			Assert::IsTrue(t1.failed);
			const Step t2 = gate.Evaluate(true, "k", "1920x1080", Status::Pending, 92.0);
			Assert::IsFalse(t2.allow);
			Assert::IsTrue(t2.log.find("2 of 3") != std::string::npos);
			const Step t3 = gate.Evaluate(true, "k", "1920x1080", Status::Pending, 130.0);
			Assert::IsFalse(t3.allow, L"third wait not over yet");
			const Step t4 = gate.Evaluate(true, "k", "1920x1080", Status::Pending, 138.0);
			Assert::IsTrue(t4.allow);
			Assert::IsTrue(t4.bypass);
		}

		// Finding 1(b): a key the worker never accepts (Missing for ever) is bounded too.
		TEST_METHOD(MissingForeverIsLetThroughVisiblyAfterTheBudget)
		{
			Gate gate;
			for (double t = 0.0; t <= 134.0; t += 1.0)
			{
				const Step held = gate.Evaluate(true, "k", "1920x1080", Status::Missing, t);
				Assert::IsFalse(held.allow, L"held inside the budget");
				Assert::IsTrue(held.submit, L"offered to the worker again");
			}
			const Step late = gate.Evaluate(true, "k", "1920x1080", Status::Missing, 136.0);
			Assert::IsTrue(late.allow);
			Assert::IsTrue(late.bypass);
			Assert::IsFalse(late.submit);
			Assert::IsFalse(late.status.empty());
		}

		// Finding 1(c): key churn restarts a key's own wait but not the hold budget.
		TEST_METHOD(KeyChurnCannotHoldForever)
		{
			Gate gate;
			const char* keys[] = { "a", "b", "c" };
			int i = 0;
			for (double t = 0.0; t <= 134.0; t += 1.0)
				Assert::IsFalse(gate.Evaluate(true, keys[i++ % 3], "1920x1080", Status::Missing, t).allow);
			const Step late = gate.Evaluate(true, "d", "1920x1080", Status::Missing, 136.0);
			Assert::IsTrue(late.allow);
			Assert::IsTrue(late.bypass);
			Assert::IsFalse(late.log.empty());
			const Step another = gate.Evaluate(true, "e", "1280x720", Status::Missing, 137.0);
			Assert::IsTrue(another.allow, L"the epoch stays bypassed across keys");
			Assert::IsTrue(another.log.empty(), L"logged once per epoch");
			Assert::IsFalse(another.status.empty());
		}

		TEST_METHOD(ReadyEndsTheEpochSoTheNextRebaseGetsAFreshBudget)
		{
			Gate gate;
			gate.Evaluate(true, "a", "1920x1080", Status::Missing, 0.0);
			for (double t = 10.0; t <= 90.0; t += 10.0)
				gate.Evaluate(true, "a", "1920x1080", Status::Pending, t);
			Assert::IsTrue(gate.Evaluate(true, "a", "1920x1080", Status::Ready, 100.0).allow);
			Assert::IsFalse(gate.Evaluate(true, "b", "1280x720", Status::Missing, 101.0).allow);
			for (double t = 111.0; t <= 231.0; t += 10.0)
				Assert::IsFalse(gate.Evaluate(true, "b", "1280x720", Status::Pending, t).allow, L"a fresh budget from 101 s");
			const Step late = gate.Evaluate(true, "b", "1280x720", Status::Pending, 240.0);
			Assert::IsTrue(late.allow);
			Assert::IsTrue(late.bypass);
		}

		TEST_METHOD(LateReadyAfterABypassIsHonouredAndClearsTheStatus)
		{
			Gate gate;
			gate.Evaluate(true, "k", "1920x1080", Status::Missing, 0.0);
			for (double t = 10.0; t <= 140.0; t += 10.0)
				gate.Evaluate(true, "k", "1920x1080", Status::Pending, t);
			const Step ready = gate.Evaluate(true, "k", "1920x1080", Status::Ready, 150.0);
			Assert::IsTrue(ready.allow);
			Assert::IsFalse(ready.bypass);
			Assert::IsTrue(ready.status.empty());
		}

		TEST_METHOD(ShortGapInWantedKeepsTheBudgetLongGapRestartsIt)
		{
			Gate flicker;
			for (double t = 0.0; t <= 140.0; t += 4.0)
			{
				// one step with the re-base not wanted: an 8 s gap, shorter than the grace
				if (t == 60.0) flicker.Evaluate(false, "k", "1920x1080", Status::Missing, t);
				else flicker.Evaluate(true, "k", "1920x1080", Status::Missing, t);
			}
			Assert::IsTrue(flicker.Evaluate(true, "k", "1920x1080", Status::Missing, 144.0).bypass, L"flicker keeps the epoch");

			Gate gap;
			for (double t = 0.0; t <= 100.0; t += 5.0)
				gap.Evaluate(true, "k", "1920x1080", Status::Missing, t);
			gap.Evaluate(false, "k", "1920x1080", Status::Missing, 105.0);
			const Step resumed = gap.Evaluate(true, "k", "1920x1080", Status::Missing, 125.0);   // 25 s later
			Assert::IsFalse(resumed.allow, L"a long gap starts a new budget");
			Assert::IsFalse(gap.Evaluate(true, "k", "1920x1080", Status::Missing, 160.0).bypass, L"35 s into the new budget (135 s in the old one)");
		}

		TEST_METHOD(FallbackDecisionFollowsTheStatus)
		{
			const FallbackCatalog::Decision released = FallbackDecision(std::string());
			Assert::IsFalse(released.engaged);
			Assert::AreEqual(std::string("re-base warm-up"), released.id);
			const FallbackCatalog::Decision engaged = FallbackDecision("re-base pending shader warm-up for 1920x1080 (current path continues)");
			Assert::IsTrue(engaged.engaged);
			Assert::AreEqual(std::string("re-base warm-up"), engaged.id);
			Assert::IsTrue(engaged.reason.find("1920x1080") != std::string::npos);
		}

		// Finding 2: factors in force are only kept across a hold on the SAME contract.
		TEST_METHOD(HoldKeepsFactorsInForceOnAnUnchangedContract)
		{
			FactorsInForce inForce;
			inForce.Allowed(0.5, 0.5, 11ULL, true);
			const FactorsInForce::Factors keep = inForce.Hold(0.5, 0.5, 11ULL);
			Assert::AreEqual(0.5, keep.x);
			Assert::AreEqual(0.5, keep.y);
			Assert::IsTrue(inForce.Covers(0.5, 0.5, 11ULL));
			Assert::IsFalse(inForce.Covers(0.6, 0.5, 11ULL), L"another candidate is not covered");
		}

		TEST_METHOD(HoldAfterASourceChangeIsNativeNotTheStaleFactors)
		{
			FactorsInForce inForce;
			inForce.Allowed(0.5, 0.5, 11ULL, true);
			const FactorsInForce::Factors keep = inForce.Hold(0.5, 0.5, 22ULL);   // the contract changed
			Assert::AreEqual(1.0, keep.x);
			Assert::AreEqual(1.0, keep.y);
			Assert::IsFalse(inForce.Covers(0.5, 0.5, 11ULL), L"and nothing is in force any more");
			// Back to the old contract: still native until a warm-up is allowed again.
			const FactorsInForce::Factors again = inForce.Hold(1.0, 1.0, 11ULL);
			Assert::AreEqual(1.0, again.x);
		}

		TEST_METHOD(HoldWithNothingInForceIsNativeAndBypassIsNeverCovered)
		{
			FactorsInForce inForce;
			Assert::AreEqual(1.0, inForce.Hold(1.0, 1.0, 1ULL).x);
			inForce.Allowed(0.5, 0.5, 1ULL, false);   // a visible bypass: asked again every frame
			Assert::IsFalse(inForce.Covers(0.5, 0.5, 1ULL));
			inForce.Allowed(0.5, 0.5, 1ULL, true);
			inForce.Clear();
			Assert::IsFalse(inForce.Covers(0.5, 0.5, 1ULL));
		}
	};
}
