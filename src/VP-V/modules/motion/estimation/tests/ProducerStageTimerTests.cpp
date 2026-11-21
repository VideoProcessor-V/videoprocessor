// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// Slice 16 T1 (refs/105 s3.6 / s4 row 0): producer stage timers and 32-sample ring via the CPU test-engine seam
// (ProcessJobForTest). Verifies queueWaitMs, engineMs, serviceMs, submitToPublishMs, ring wrap at 32 samples,
// and StagePercentile (p50 / p95).
#include "pch.h"
#include <VP-V/modules/motion/estimation/MotionAheadProducer.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include "CppUnitTest.h"

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace VideoProcessorTest
{
	TEST_CLASS(ProducerStageTimerTests)
	{
	public:
		TEST_METHOD(StageTimersRecordPerPairThroughTestEngineSeam)
		{
			MotionAhead::Producer producer;
			auto seam = std::make_shared<MotionAhead::Producer::TestEngine>();
			seam->job = [](uint64_t seq, uint64_t) {
				if (seq == 2)
					std::this_thread::sleep_for(std::chrono::milliseconds(15));
				return true;
			};
			Assert::IsTrue(producer.StartForTest(seam));
			AnalysisLumaSource empty;
			producer.Submit(1, 1, empty);
			// Wait for frame 1 to be processed (first frame produces no pair).
			const auto deadline1 = std::chrono::steady_clock::now() + std::chrono::seconds(5);
			while (producer.GetStats().frames < 1 && std::chrono::steady_clock::now() < deadline1)
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			const auto afterFirst = producer.GetStats();
			Assert::AreEqual(1ull, afterFirst.frames);
			Assert::AreEqual(0ull, afterFirst.pairs);
			Assert::AreEqual<size_t>(0, afterFirst.stageCount, L"first frame alone records no pair stage sample");

			producer.Submit(2, 1, empty);
			const auto deadline2 = std::chrono::steady_clock::now() + std::chrono::seconds(5);
			while (producer.GetStats().pairs < 1 && std::chrono::steady_clock::now() < deadline2)
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			const auto st = producer.GetStats();
			Assert::AreEqual(1ull, st.pairs);
			Assert::AreEqual<size_t>(1, st.stageCount);
			Assert::AreEqual<size_t>(1, st.stageNext);
			Assert::IsTrue(st.lastStage.engineMs >= 10.0, L"engineMs reflects the 15 ms test job sleep");
			Assert::IsTrue(st.lastStage.serviceMs >= st.lastStage.engineMs, L"serviceMs covers engineMs");
			Assert::IsTrue(st.lastStage.submitToPublishMs >= st.lastStage.serviceMs, L"submitToPublishMs >= serviceMs");
			Assert::IsTrue(st.lastStage.queueWaitMs >= 0.0);
			producer.Stop();
		}

		TEST_METHOD(QueueWaitReflectsWaitingBehindBusyJob)
		{
			MotionAhead::Producer producer;
			std::mutex m;
			std::condition_variable cv;
			bool inSecond = false;
			bool releaseSecond = false;
			auto seam = std::make_shared<MotionAhead::Producer::TestEngine>();
			seam->job = [&](uint64_t seq, uint64_t) {
				if (seq == 2)
				{
					std::unique_lock<std::mutex> lock(m);
					inSecond = true;
					cv.notify_all();
					cv.wait_for(lock, std::chrono::seconds(5), [&] { return releaseSecond; });
				}
				return true;
			};
			Assert::IsTrue(producer.StartForTest(seam));
			AnalysisLumaSource empty;
			producer.Submit(1, 1, empty);
			producer.Submit(2, 1, empty);
			{
				std::unique_lock<std::mutex> lock(m);
				Assert::IsTrue(cv.wait_for(lock, std::chrono::seconds(5), [&] { return inSecond; }));
			}
			// Submit frame 3 while frame 2 is blocked inside the worker, then hold for 25 ms.
			producer.Submit(3, 1, empty);
			std::this_thread::sleep_for(std::chrono::milliseconds(25));
			{
				std::lock_guard<std::mutex> lock(m);
				releaseSecond = true;
			}
			cv.notify_all();
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
			while (producer.GetStats().pairs < 2 && std::chrono::steady_clock::now() < deadline)
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			const auto st = producer.GetStats();
			Assert::AreEqual(2ull, st.pairs);
			Assert::AreEqual<size_t>(2, st.stageCount);
			Assert::IsTrue(st.lastStage.queueWaitMs >= 15.0, L"frame 3 waited in the queue behind blocked frame 2");
			Assert::IsTrue(st.lastStage.submitToPublishMs >= st.lastStage.queueWaitMs, L"submit->publish includes queueWait");
			producer.Stop();
		}

		TEST_METHOD(StageRingWrapsAt32AndComputesPercentiles)
		{
			MotionAhead::Producer::Stats st;
			Assert::AreEqual(0.0, st.StagePercentile(&MotionAhead::Producer::StageSample::serviceMs, 0.50), 1e-9);
			Assert::AreEqual(0.0, st.StagePercentile(&MotionAhead::Producer::StageSample::serviceMs, 0.95), 1e-9);

			// Push 40 samples: values 1..40 -> ring retains 9..40 (32 samples).
			for (int i = 1; i <= 40; ++i)
			{
				MotionAhead::Producer::StageSample s;
				s.serviceMs = static_cast<double>(i);
				s.engineMs = static_cast<double>(i) * 0.5;
				st.RecordStage(s);
			}
			Assert::AreEqual<size_t>(32, st.stageCount);
			Assert::AreEqual<size_t>(8, st.stageNext);
			Assert::AreEqual(40.0, st.lastStage.serviceMs, 1e-9);
			const double p50 = st.StagePercentile(&MotionAhead::Producer::StageSample::serviceMs, 0.50);
			const double p95 = st.StagePercentile(&MotionAhead::Producer::StageSample::serviceMs, 0.95);
			// Sorted [9..40], idx for 0.50 = round(0.5*31) = 16 -> value 25; idx for 0.95 = round(0.95*31) = 29 -> value 38.
			Assert::AreEqual(25.0, p50, 1e-9);
			Assert::AreEqual(38.0, p95, 1e-9);
			Assert::AreEqual(12.5, st.StagePercentile(&MotionAhead::Producer::StageSample::engineMs, 0.50), 1e-9);
			Assert::IsTrue(p50 <= p95);
		}
	};
}
