// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// Producer throughput plan (refs\105 s3.3 / s5, phase 1a): the pure job chooser of the one NVOF session. CPU only.
// Rules (design-intent register 115 #57, owner 2026-11-11 07:10): core pair > on-demand request > long pair. A waiting core
// pair always goes first and is flagged when a request waited; a request goes before a long pair; a long pair only when
// nothing else waits (never while a request waits); an empty queue chooses nothing; the chooser keeps no state (no request
// burst, on an idle walk or not, changes the next choice).
#include "pch.h"
#include <VP-V/modules/motion/estimation/ProducerJobs.h>
#include "CppUnitTest.h"

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace VideoProcessorTest
{
	TEST_CLASS(ProducerJobSchedulerTests)
	{
		using Queues = MotionAhead::ProducerJobs::Queues;
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
			Assert::IsFalse(MotionAhead::ProducerJobs::NextJob(Q(0, 0, 0)).any);
		}

		TEST_METHOD(AWaitingCorePairAlwaysGoesBeforeARequest)
		{
			// Every request depth and every core depth: the core pair first, flagged because a request waited.
			for (size_t core = 1; core <= 4; ++core)
				for (size_t request = 1; request <= 4; ++request)
				{
					const auto c = MotionAhead::ProducerJobs::NextJob(Q(core, request));
					Assert::IsTrue(c.any && c.cls == Class::CorePair, L"115 #57: core > on-demand");
					Assert::IsTrue(c.requestWaited, L"counted as coreAheadOfRequests: a request was waiting");
				}
			// No streak: after any number of requests the next choice with a core pair waiting is still the core pair.
			for (int i = 0; i < 5; ++i)
				Assert::IsTrue(MotionAhead::ProducerJobs::NextJob(Q(0, 3)).cls == Class::OnDemand);
			Assert::IsTrue(MotionAhead::ProducerJobs::NextJob(Q(1, 3)).cls == Class::CorePair, L"no request streak overtakes a core pair");
		}

		TEST_METHOD(ARequestRunsWhenNoCorePairWaitsAndACorePairAloneIsNotFlagged)
		{
			const auto request = MotionAhead::ProducerJobs::NextJob(Q(0, 1));
			Assert::IsTrue(request.any && request.cls == Class::OnDemand);
			Assert::IsFalse(request.requestWaited);
			const auto core = MotionAhead::ProducerJobs::NextJob(Q(2, 0));
			Assert::IsTrue(core.any && core.cls == Class::CorePair);
			Assert::IsFalse(core.requestWaited, L"no request waited: not counted");
		}

		TEST_METHOD(ALongPairRunsOnlyWhenNothingElseWaits)
		{
			Assert::IsTrue(MotionAhead::ProducerJobs::NextJob(Q(1, 0, 1)).cls == Class::CorePair, L"a core pair first");
			Assert::IsTrue(MotionAhead::ProducerJobs::NextJob(Q(1, 1, 1)).cls == Class::CorePair, L"a core pair first");
			Assert::IsTrue(MotionAhead::ProducerJobs::NextJob(Q(0, 1, 1)).cls == Class::OnDemand, L"a request before a long pair");
			Assert::IsTrue(MotionAhead::ProducerJobs::NextJob(Q(0, 0, 1)).cls == Class::LongPair);
			// A long pair never runs while a request waits, however many requests ran before.
			for (size_t request = 3; request >= 1; --request)
				Assert::IsTrue(MotionAhead::ProducerJobs::NextJob(Q(0, request, 1)).cls == Class::OnDemand,
					L"a long pair never overtakes a waiting request");
			Assert::IsFalse(MotionAhead::ProducerJobs::NextJob(Q(0, 0, 1)).requestWaited);
		}
	};
}
