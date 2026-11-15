// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// Producer throughput plan (refs\105 s3.3 / s5, phase 1a): the pure job chooser of the one NVOF session. CPU only.
// Rules: requests first, at most the streak bound in a row WHILE a job waits; a core pair under two service times of slack goes
// first; a long pair only when nothing else waits; the streak is not used up by requests served on an idle walk (the C5b
// dequeue-stall fix); an empty queue chooses nothing.
#include "pch.h"
#include <VP-V/modules/motion/estimation/ProducerJobs.h>
#include "CppUnitTest.h"

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace VideoProcessorTest
{
	TEST_CLASS(ProducerJobSchedulerTests)
	{
		using Queues = MotionAhead::ProducerJobs::Queues;
		using State = MotionAhead::ProducerJobs::ChooserState;
		using Class = MotionAhead::ProducerJobs::JobClass;

		static Queues Q(size_t core, size_t request, size_t longPair = 0)
		{
			Queues q;
			q.corePairs = core;
			q.onDemand = request;
			q.longPairs = longPair;
			return q;
		}

	public:
		TEST_METHOD(AnEmptyQueueChoosesNothing)
		{
			State state;
			Assert::IsFalse(MotionAhead::ProducerJobs::NextJob(Q(0, 0, 0), state, 2).any);
		}

		TEST_METHOD(ARequestGoesFirstAndACoreJobRunsAfterTheStreakBound)
		{
			State state;
			const auto a = MotionAhead::ProducerJobs::NextJob(Q(1, 3), state, 2);
			const auto b = MotionAhead::ProducerJobs::NextJob(Q(1, 2), state, 2);
			const auto c = MotionAhead::ProducerJobs::NextJob(Q(1, 1), state, 2);
			Assert::IsTrue(a.cls == Class::OnDemand && b.cls == Class::OnDemand);
			Assert::IsTrue(c.cls == Class::CorePair, L"the third choice is the waiting core job");
			Assert::IsTrue(c.fairness, L"counted as a fairness job: a request was waiting");
			const auto d = MotionAhead::ProducerJobs::NextJob(Q(0, 1), state, 2);
			Assert::IsTrue(d.cls == Class::OnDemand);
			Assert::AreEqual(0, state.requestStreak, L"the job reset the streak; a request with no job waiting does not count");
		}

		TEST_METHOD(RequestsServedOnAnIdleWalkDoNotUseUpTheStreak)
		{
			State state;
			for (int i = 0; i < 5; ++i)
			{
				const auto c = MotionAhead::ProducerJobs::NextJob(Q(0, 5 - static_cast<size_t>(i)), state, 2);
				Assert::IsTrue(c.cls == Class::OnDemand);
			}
			Assert::AreEqual(0, state.requestStreak);
			// A job and a request now arrive together: the request still goes first (the old counter had run up and the job overtook it).
			const auto first = MotionAhead::ProducerJobs::NextJob(Q(1, 1), state, 2);
			Assert::IsTrue(first.cls == Class::OnDemand, L"dequeue-stall fix: the request keeps its priority after an idle burst");
		}

		TEST_METHOD(ACoreJobWithLittleSlackGoesBeforeARequest)
		{
			State state;
			const auto urgent = MotionAhead::ProducerJobs::NextJob(Q(1, 1), state, 2, 5.0, 4.0);   // slack 5 ms < 2 x 4 ms
			Assert::IsTrue(urgent.cls == Class::CorePair && urgent.urgent && !urgent.fairness);
			State fresh_relaxed;   // a fresh chooser state per sub-check (no streak carried over)
			const auto relaxed = MotionAhead::ProducerJobs::NextJob(Q(1, 1), fresh_relaxed, 2, 8.0, 4.0);  // slack 8 ms is not < 8 ms
			Assert::IsTrue(relaxed.cls == Class::OnDemand);
			State fresh_unknown;   // a fresh chooser state per sub-check (no streak carried over)
			const auto unknown = MotionAhead::ProducerJobs::NextJob(Q(1, 1), fresh_unknown, 2, -1.0, 4.0);
			Assert::IsTrue(unknown.cls == Class::OnDemand, L"unknown slack is never urgent");
			State fresh_noService;   // a fresh chooser state per sub-check (no streak carried over)
			const auto noService = MotionAhead::ProducerJobs::NextJob(Q(1, 1), fresh_noService, 2, 1.0, 0.0);
			Assert::IsTrue(noService.cls == Class::OnDemand, L"no measured service time: never urgent");
		}

		TEST_METHOD(ALongPairRunsOnlyWhenNothingElseWaits)
		{
			State state;
			Assert::IsTrue(MotionAhead::ProducerJobs::NextJob(Q(1, 0, 1), state, 2).cls == Class::CorePair);
			Assert::IsTrue(MotionAhead::ProducerJobs::NextJob(Q(0, 1, 1), state, 2).cls == Class::OnDemand, L"a request first");
			Assert::IsTrue(MotionAhead::ProducerJobs::NextJob(Q(0, 0, 1), state, 2).cls == Class::LongPair);
			// A long job behind a request, past the streak bound, gets its turn like a core job (never starved).
			MotionAhead::ProducerJobs::NextJob(Q(0, 3, 1), state, 2);
			MotionAhead::ProducerJobs::NextJob(Q(0, 2, 1), state, 2);
			Assert::IsTrue(MotionAhead::ProducerJobs::NextJob(Q(0, 1, 1), state, 2).cls == Class::LongPair);
		}
	};
}
