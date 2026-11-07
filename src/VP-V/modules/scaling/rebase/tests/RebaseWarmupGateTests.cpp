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

		TEST_METHOD(EmptyKeyIsNotGated)
		{
			Gate gate;
			Assert::IsTrue(gate.Evaluate(true, std::string(), "?", Status::Missing, 0.0).allow);
		}
	};
}
