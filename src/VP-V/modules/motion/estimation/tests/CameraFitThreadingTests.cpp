// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "CppUnitTest.h"

// Camera link K1-5 (F2): the reader hooks only bump atomics; the worker thread owns the Fitter. 10 000 reset requests
// from the "render thread" during fits leave a deterministic, race-free sequence; the single thread drains the queue.
#include "CameraLinkTestSupport.h"
#include <VP-V/modules/motion/estimation/global_model/CameraReaders.h>

#include <atomic>
#include <thread>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace VideoProcessorTest
{
	TEST_CLASS(CameraFitThreadingTests)
	{
	public:
		TEST_METHOD(TenThousandResetRequestsDuringFitsAreSafeAndResetsAreCoalesced)
		{
			CameraLinkTest::Rig rig(2);
			CameraLink::CameraRecordPool pool(rig.Ring());
			CameraLink::FitCounters counters;
			CameraLink::CameraFitProducer producer(CameraModel::Config(), &counters);
			CameraLink::CameraFitWorker worker(producer, rig.Env(&pool, &counters, 8));
			Assert::IsTrue(worker.Start());
			Assert::IsFalse(worker.Start(), L"one thread only");
			// The Indicator reader path: reset hooks run on this thread and only bump an atomic.
			CameraLink::CameraReaderHooks hooks;
			hooks.reset = [&producer]() { producer.NoteReset(); };
			hooks.relatchTiming = [&producer]() { producer.NoteTiming(); };
			int submitted = 0;
			for (int i = 1; i <= 24; ++i)
			{
				rig.PublishInputs(static_cast<uint64_t>(i));
				if (worker.Submit(CameraLinkTest::PanJob(static_cast<uint64_t>(i), 6.0 + i * 0.1)))
					++submitted;
				for (int k = 0; k < 10000 / 24 + 1; ++k)
				{
					hooks.reset();
					hooks.relatchTiming();
				}
				worker.WaitIdle();
				rig.Retire(static_cast<uint64_t>(i) - 1);
			}
			worker.Stop();
			Assert::AreEqual(24, submitted, L"WaitIdle after every pair: nothing dropped");
			Assert::AreEqual(uint64_t(24), counters.fitted.load());
			Assert::AreEqual(uint64_t(24), counters.published.load());
			Assert::IsTrue(counters.resets.load() <= 24, L"resets are coalesced: at most one per fit");
			Assert::IsTrue(counters.resets.load() >= 1);
		}

		TEST_METHOD(ASeparateRunWithTheSameScheduleGivesTheSameHistory)
		{
			uint64_t hashes[2] = { 0, 0 };
			for (int run = 0; run < 2; ++run)
			{
				CameraLinkTest::Rig rig(2);
				CameraLink::CameraRecordPool pool(rig.Ring());
				CameraLink::FitCounters counters;
				CameraLink::CameraFitProducer producer(CameraModel::Config(), &counters);
				CameraLink::CameraFitWorker worker(producer, rig.Env(&pool, &counters, 8));
				worker.Start();
				for (int i = 1; i <= 12; ++i)
				{
					rig.PublishInputs(static_cast<uint64_t>(i));
					if (i == 7)
						producer.NoteReset();
					worker.Submit(CameraLinkTest::PanJob(static_cast<uint64_t>(i), 6.0 + i * 0.1));
					worker.WaitIdle();
					rig.Retire(static_cast<uint64_t>(i) - 1);
				}
				worker.Stop();
				hashes[run] = producer.HistoryHash();
			}
			Assert::AreEqual(hashes[0], hashes[1]);
		}

		TEST_METHOD(StopWithQueuedJobsJoinsTheThread)
		{
			CameraLinkTest::Rig rig(2);
			CameraLink::CameraRecordPool pool(rig.Ring());
			CameraLink::FitCounters counters;
			CameraLink::CameraFitProducer producer(CameraModel::Config(), &counters);
			{
				CameraLink::CameraFitWorker worker(producer, rig.Env(&pool, &counters, 8));
				worker.Start();
				for (int i = 1; i <= 4; ++i)
				{
					rig.PublishInputs(static_cast<uint64_t>(i));
					worker.Submit(CameraLinkTest::PanJob(static_cast<uint64_t>(i), 5.0));
				}
			}   // the destructor stops and joins: no crash, no hang
			Assert::IsTrue(true);
		}
		// Resets and relatches are stamped into the job at SUBMIT: a reset requested between two queued jobs applies before the
		// second job's Fit, whenever the worker gets to the queue. Fill the queue first (no WaitIdle per submit), then run it by
		// (a) the thread started afterwards, (b) ProcessQueued, (c) a one-at-a-time synchronous schedule: identical histories.
		static uint64_t QueuedHistory(int mode)
		{
			CameraLinkTest::Rig rig(2);
			CameraLink::CameraRecordPool pool(rig.Ring());
			CameraLink::FitCounters counters;
			CameraLink::CameraFitProducer producer(CameraModel::Config(), &counters);
			CameraLink::TimingFacts tf;
			tf.measuredSourceHz = 24.0;
			producer.SetTimingFacts(tf);
			producer.NoteTiming();
			CameraLink::CameraFitWorker worker(producer, rig.Env(&pool, &counters, 8));
			for (uint64_t i = 1; i <= 4; ++i)
				rig.PublishInputs(i);
			for (uint64_t i = 1; i <= 4; ++i)
			{
				CameraLink::CameraFitJob job = CameraLinkTest::PanJob(i, 6.0 + i, 0.0);
				if (i == 3)
					producer.NoteReset();   // between job 2 and job 3
				Assert::IsTrue(worker.Submit(job));
				if (mode == 2)
				{
					worker.ProcessQueued();   // one at a time: the reference schedule
				}
			}
			if (mode == 0)
			{
				Assert::IsTrue(worker.Start());   // the thread starts AFTER everything is queued
				worker.WaitIdle();
				worker.Stop();
			}
			else if (mode == 1)
				Assert::AreEqual(size_t(4), worker.ProcessQueued());
			Assert::AreEqual(uint64_t(1), counters.resets.load(), L"exactly one reset, applied before job 3");
			return producer.HistoryHash();
		}

		TEST_METHOD(AResetBetweenQueuedJobsAppliesBeforeThatJobWhateverTheWorkerSchedule)
		{
			const uint64_t thread = QueuedHistory(0);
			const uint64_t queued = QueuedHistory(1);
			const uint64_t oneByOne = QueuedHistory(2);
			Assert::AreEqual(oneByOne, queued);
			Assert::AreEqual(oneByOne, thread);
			// ... and the reset really mattered: without it the history differs.
			CameraLinkTest::Rig rig(2);
			CameraLink::CameraRecordPool pool(rig.Ring());
			CameraLink::FitCounters counters;
			CameraLink::CameraFitProducer producer(CameraModel::Config(), &counters);
			CameraLink::TimingFacts tf;
			tf.measuredSourceHz = 24.0;
			producer.SetTimingFacts(tf);
			producer.NoteTiming();
			CameraLink::CameraFitWorker worker(producer, rig.Env(&pool, &counters, 8));
			for (uint64_t i = 1; i <= 4; ++i)
			{
				rig.PublishInputs(i);
				worker.Submit(CameraLinkTest::PanJob(i, 6.0 + i, 0.0));
			}
			worker.ProcessQueued();
			Assert::IsTrue(producer.HistoryHash() != oneByOne);
		}
	};
}
