// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "CppUnitTest.h"

// Camera link K1-5 (F2): the reader hooks only bump atomics; the worker thread owns the Fitter. 10 000 reset requests
// from the "render thread" during fits leave a deterministic, race-free sequence; the single thread drains the queue.
#include "CameraLinkTestSupport.h"
#include <VP-V/core/AnalysisLane.h>
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

		// ---- the lane lease: a rebuild of the analysis lane while the camera worker holds its objects ----
		// (SK-W fix: the worker used to hold a raw SlotTable*; AnalysisLane::Configure swaps the whole LaneState at every rebuild.)
		struct LaneRig
		{
			VpIndicator::Indicator ind;
			std::atomic<uint64_t> clock{ 0 };
			VpSkeleton::AnalysisLane lane;
			VpSkeleton::Registry reg;
			std::vector<VpSkeleton::ConsumerDecl> consumers;
			VpSkeleton::GraphSettings gs;
			VpSkeleton::CardProfile card;
			VpSkeleton::TierInput tier;
			VpSkeleton::LaneSettings ls;

			LaneRig()
				: ind(1, 256)
				, lane(&ind, [this]() { return clock.fetch_add(3) + 3; })
				, reg(VpSkeleton::SkeletonProducts())
				, consumers(CameraLinkTest::Consumers({ CameraLink::CameraConsumer::Diagnostics }))
				, gs(CameraLinkTest::Settings(2))
			{
				card.vramBudgetBytes = 1ull << 33;
				card.systemRamFreeBytes = 1ull << 33;
				card.uploadGBps = 10.0;
				card.computeScale = 1.0;
				card.ofaPresent = true;
				card.ofaMsPerPairRef = 2.0;
				tier.framePeriodMs = 40.0;
				ls.mode = VpSkeleton::LaneSettings::Mode::On;
			}
			uint32_t Configure() { return lane.Configure(ls, reg, consumers, gs, card, tier, 0, true); }
			// A rebuild: a different lane lead is a different graph (the same trigger class as an external-demand or period change).
			void Rebuild() { gs.laneLead = gs.laneLead == 2 ? uint8_t(3) : uint8_t(2); Configure(); }
			VpSkeleton::LaneLeaseSource Source() { return [this]() { return lane.Lease(VpSkeleton::kP7CameraFit); }; }
		};

		TEST_METHOD(ALaneRebuildWhileTheWorkerHoldsALeaseKeepsTheOldStateAliveAndRefusesTheStalePostVisibly)
		{
			LaneRig rig;
			rig.Configure();
			Assert::IsTrue(rig.lane.Active(), L"the lane built a state with P7");
			const uint64_t gen1 = rig.lane.Generation();
			Assert::IsTrue(gen1 >= 1);
			CameraLink::FitCounters counters;
			CameraLink::CameraFitProducer producer(CameraModel::Config(), &counters);
			std::vector<CameraLink::PublishOutcome> outcomes;
			std::weak_ptr<const void> oldState;
			std::unique_ptr<CameraLink::CameraFitWorker> worker;
			CameraLink::CameraRecordPool* poolRaw = nullptr;
			std::unique_ptr<CameraLink::CameraRecordPool> pool;
			{
				VpSkeleton::LaneLease first = rig.Source()();
				Assert::IsTrue(first.Owned() && first.Valid() && first.generation == gen1);
				Assert::IsTrue(first.bytes != nullptr, L"P7 is a CPU product: its bytes ring is in the lease");
				oldState = first.hold;
				pool.reset(new CameraLink::CameraRecordPool(first.table->Find(VpSkeleton::kP7CameraFit)->Ring()));
				poolRaw = pool.get();
				CameraLink::CameraFitEnv env;
				env.lease = first;
				env.leaseSource = rig.Source();
				env.pool = poolRaw;
				env.counters = &counters;
				env.queueCapacity = 4;
				env.onResult = [&outcomes](const CameraLink::CameraFitJob&, const CameraLink::CameraFitResult&, CameraLink::PublishOutcome o) { outcomes.push_back(o); };
				worker.reset(new CameraLink::CameraFitWorker(producer, env));
			}   // the test keeps no hold of its own: only the worker's lease keeps generation 1 alive
			Assert::AreEqual(gen1, worker->LaneGeneration());
			Assert::IsTrue(worker->Submit(CameraLinkTest::PanJob(1, 6.0)), L"queued against generation 1");
			rig.Rebuild();
			const uint64_t gen2 = rig.lane.Generation();
			Assert::IsTrue(gen2 > gen1, L"the rebuild produced a new LaneState generation");
			Assert::IsFalse(oldState.expired(), L"the lane dropped the old state, the worker's lease still owns it: nothing is freed under the worker");
			// The old table is still readable through the lease (this is the access that was a use-after-free with a raw pointer).
			const VpSkeleton::LaneLease held = worker->Lease();
			Assert::IsTrue(held.table != nullptr && held.table->Find(VpSkeleton::kP7CameraFit) != nullptr);
			Assert::IsTrue(held.table->Find(VpSkeleton::kP7CameraFit)->Ring() >= 1u);
			Assert::AreEqual(gen1, held.generation);
			Assert::AreEqual(size_t(1), worker->ProcessQueued());
			Assert::AreEqual(size_t(1), outcomes.size());
			Assert::IsTrue(outcomes[0] == CameraLink::PublishOutcome::StaleLane, L"a post into the old generation is refused");
			Assert::AreEqual(uint64_t(1), counters.staleLane.load(), L"... and counted");
			Assert::AreEqual(uint64_t(0), counters.published.load());
			Assert::AreEqual(uint64_t(1), counters.rebound.load(), L"the worker rebound to the new generation");
			Assert::AreEqual(gen2, worker->LaneGeneration());
			Assert::IsTrue(CameraLink::StatusLine(counters).find("1 stale lane") != std::string::npos, L"and visible in the camera status line");
			// A post stamped with the NEW generation is not refused as stale (it may still be late: no inputs are published here).
			Assert::IsTrue(worker->Submit(CameraLinkTest::PanJob(2, 6.0)));
			worker->ProcessQueued();
			Assert::AreEqual(size_t(2), outcomes.size());
			Assert::IsTrue(outcomes[1] != CameraLink::PublishOutcome::StaleLane);
			Assert::AreEqual(uint64_t(1), counters.staleLane.load(), L"no further refusals");
		}

		TEST_METHOD(RebuildingTheLaneRepeatedlyUnderARunningWorkerThreadIsSafe)
		{
			LaneRig rig;
			rig.Configure();
			Assert::IsTrue(rig.lane.Active());
			CameraLink::FitCounters counters;
			CameraLink::CameraFitProducer producer(CameraModel::Config(), &counters);
			VpSkeleton::LaneLease first = rig.Source()();
			CameraLink::CameraRecordPool pool(first.table->Find(VpSkeleton::kP7CameraFit)->Ring());
			CameraLink::CameraFitEnv env;
			env.lease = first;
			env.leaseSource = rig.Source();
			env.pool = &pool;
			env.counters = &counters;
			env.queueCapacity = 8;
			first = VpSkeleton::LaneLease();
			CameraLink::CameraFitWorker worker(producer, env);
			env.lease = VpSkeleton::LaneLease();
			Assert::IsTrue(worker.Start());
			int submitted = 0;
			for (int i = 1; i <= 40; ++i)
			{
				if (worker.Submit(CameraLinkTest::PanJob(static_cast<uint64_t>(i), 6.0 + i * 0.1)))
					++submitted;
				if (i % 4 == 0)
					rig.Rebuild();   // the render thread swaps the LaneState while the worker thread fits and publishes
				if (i % 8 == 0)
					worker.WaitIdle();
			}
			worker.WaitIdle();
			worker.Stop();
			Assert::IsTrue(counters.fitted.load() >= 1 && counters.fitted.load() <= uint64_t(submitted), L"pairs were fitted, none invented");
			Assert::IsTrue(counters.rebound.load() >= 1, L"the rebuilds were noticed");
			Assert::IsTrue(counters.staleLane.load() + counters.late.load() + counters.published.load() <= uint64_t(submitted));
		}
	};
}
