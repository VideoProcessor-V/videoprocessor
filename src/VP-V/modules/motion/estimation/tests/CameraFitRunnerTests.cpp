// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "CppUnitTest.h"

// Camera link K1-5: the P7 producer and its worker driven synchronously (no thread): publish through the SlotSet protocol,
// a late fit is FITTED but not published (same fit history), a full queue drops visibly and never blocks, the record POD
// codec, deterministic history. CPU only.
#include "CameraLinkTestSupport.h"
#include <VP-V/modules/motion/estimation/global_model/link/CameraRecordPool.h>
#include <VP-V/modules/motion/estimation/global_model/link/CameraUsable.h>

#include <chrono>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace VideoProcessorTest
{
	TEST_CLASS(CameraFitRunnerTests)
	{
	public:
		// Runs `frames` consecutive pairs; lateMask bit i = the render sealed frame (i+1) before the fit arrived.
		struct Run
		{
			uint64_t hash = 0;
			uint64_t published = 0, late = 0;
			std::vector<bool> inPool;
		};

		static Run Execute(int frames, unsigned lateMask)
		{
			CameraLinkTest::Rig rig;
			CameraLink::CameraRecordPool pool(rig.Ring());
			CameraLink::FitCounters counters;
			CameraLink::CameraFitProducer producer(CameraModel::Config(), &counters);
			CameraLink::TimingFacts tf;
			tf.measuredSourceHz = 24.0;
			producer.SetTimingFacts(tf);
			producer.NoteTiming();
			CameraLink::CameraFitWorker worker(producer, rig.Env(&pool, &counters));
			Run r;
			for (int i = 0; i < frames; ++i)
			{
				const uint64_t seq = static_cast<uint64_t>(i + 1);
				rig.PublishInputs(seq);
				if ((lateMask >> i) & 1u)
					rig.SealP7Empty(seq);
				CameraLink::CameraFitJob job = CameraLinkTest::PanJob(seq, 8.0 + 0.5 * i, 0.0);
				job.facts.pairSeconds = 0;   // derived from the latched timing
				worker.ProcessJob(job);
				CameraLink::PoolEntry e;
				r.inPool.push_back(pool.Get(seq, e));
				rig.Retire(seq - 1);
			}
			r.hash = producer.HistoryHash();
			r.published = counters.published.load();
			r.late = counters.late.load();
			return r;
		}

		TEST_METHOD(PublishesThroughTheSlotProtocolAndTheSealAccepts)
		{
			CameraLinkTest::Rig rig;
			CameraLink::CameraRecordPool pool(rig.Ring());
			CameraLink::FitCounters counters;
			CameraLink::CameraFitProducer producer(CameraModel::Config(), &counters);
			CameraLink::CameraFitWorker worker(producer, rig.Env(&pool, &counters));
			rig.PublishInputs(1);
			const CameraLink::PublishOutcome o = worker.ProcessJob(CameraLinkTest::PanJob(1, 10.0));
			Assert::IsTrue(o == CameraLink::PublishOutcome::Published, L"published");
			Assert::AreEqual(uint64_t(1), counters.published.load());
			VpSkeleton::SlotSet* set = rig.table->Find(VpSkeleton::kP7CameraFit);
			Assert::IsTrue(set->StateOf(1) == VpSkeleton::SlotState::Published);
			const VpSkeleton::SealResult sr = set->Seal(1, [](const VpAncestry::Header& h) {
				return h.ancestorCount == 3 ? VpSkeleton::SealReason::Accepted : VpSkeleton::SealReason::AncestryIncomplete; });
			Assert::IsTrue(sr.outcome == VpSkeleton::SealOutcome::Sealed, L"ancestors recorded for the seal");
			CameraLink::PoolEntry e;
			Assert::IsTrue(pool.Get(1, e) && e.record && e.field);
			Assert::IsTrue(e.record->valid && e.record->reason == CameraModel::Reason::Ok);
			// The POD in the slot bytes decodes to the same record.
			CameraModel::CameraMotion d;
			Assert::IsTrue(CameraLink::DecodeRecordPod(rig.bytes->Ring(set->SlotOf(1)), CameraLink::kRecordPodMaxBytes, d));
			Assert::AreEqual(e.record->confidence, d.confidence);
			Assert::AreEqual(e.record->params.h[2], d.params.h[2]);
			Assert::AreEqual(e.record->seq, d.seq);
		}

		TEST_METHOD(PodCodecRoundTripsAndStaysWithinTheDeclaredKilobyte)
		{
			CameraModel::CameraMotion m;
			m.seq = 77; m.pairFrom = 76; m.pairTo = 77; m.pairSeconds = 0.0417;
			m.resyncGeneration = 0xDEADBEEFull; m.valid = true;
			m.params.type = CameraModel::ModelType::Homography;
			for (int i = 0; i < 9; ++i) m.params.h[i] = 0.5 + i;
			m.rates.tx = 3.25; m.confidence = 0.8125; m.reason = CameraModel::Reason::PostCutSeeded;
			m.signals.inlierShare = 0.9; m.signals.haveBackward = true;
			m.bridged = true; m.labelsOnGpu = true;
			m.samples = 1296; m.costMicros = 321;
			m.picture.right = 3840; m.picture.bottom = 2160;
			uint8_t buf[CameraLink::kRecordPodMaxBytes];
			const size_t n = CameraLink::EncodeRecordPod(m, buf, sizeof(buf));
			Assert::IsTrue(n > 0 && n <= CameraLink::kRecordPodMaxBytes);
			CameraModel::CameraMotion d;
			Assert::IsTrue(CameraLink::DecodeRecordPod(buf, n, d));
			Assert::AreEqual(m.seq, d.seq);
			Assert::AreEqual(m.resyncGeneration, d.resyncGeneration);
			Assert::AreEqual(m.params.h[8], d.params.h[8]);
			Assert::IsTrue(d.params.type == CameraModel::ModelType::Homography);
			Assert::IsTrue(d.reason == CameraModel::Reason::PostCutSeeded && d.bridged && d.labelsOnGpu && d.signals.haveBackward);
			Assert::AreEqual(m.costMicros, d.costMicros);
			Assert::AreEqual(3840, d.picture.right);
			Assert::IsFalse(CameraLink::DecodeRecordPod(buf, n - 1, d), L"short buffer");
			buf[0] ^= 1;
			Assert::IsFalse(CameraLink::DecodeRecordPod(buf, n, d), L"bad magic");
			uint8_t tiny[8];
			Assert::AreEqual(size_t(0), CameraLink::EncodeRecordPod(m, tiny, sizeof(tiny)));
		}

		TEST_METHOD(ALateFitIsFittedNotPublishedAndTheHistoryIsUnchanged)
		{
			const int frames = 6;
			const Run clean = Execute(frames, 0);
			const Run late = Execute(frames, (1u << 1) | (1u << 3));
			Assert::AreEqual(uint64_t(frames), clean.published);
			Assert::AreEqual(uint64_t(0), clean.late);
			Assert::AreEqual(uint64_t(frames - 2), late.published);
			Assert::AreEqual(uint64_t(2), late.late);
			Assert::IsFalse(late.inPool[1] || late.inPool[3], L"a late record is never served");
			Assert::IsTrue(late.inPool[0] && late.inPool[2] && late.inPool[4] && late.inPool[5]);
			Assert::AreEqual(clean.hash, late.hash, L"fit history hash equal with and without lateness");
		}

		TEST_METHOD(RepeatedRunsAreDeterministic)
		{
			Assert::AreEqual(Execute(6, 0).hash, Execute(6, 0).hash);
			Assert::IsTrue(Execute(6, 0).hash != Execute(5, 0).hash, L"the hash really covers the history");
		}

		TEST_METHOD(AFullQueueDropsVisiblyRequestsAResetAndNeverBlocks)
		{
			CameraLinkTest::Rig rig;
			CameraLink::CameraRecordPool pool(rig.Ring());
			CameraLink::FitCounters counters;
			CameraLink::CameraFitProducer producer(CameraModel::Config(), &counters);
			CameraLink::CameraFitWorker worker(producer, rig.Env(&pool, &counters, 2));
			const uint64_t gen0 = producer.ResetGen();
			const auto t0 = std::chrono::steady_clock::now();
			Assert::IsTrue(worker.Submit(CameraLinkTest::PanJob(1, 5)));
			Assert::IsTrue(worker.Submit(CameraLinkTest::PanJob(2, 5)));
			Assert::IsFalse(worker.Submit(CameraLinkTest::PanJob(3, 5)), L"third job: queue of two is full");
			const auto dt = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
			Assert::IsTrue(dt < 1000, L"Submit never waits for a fit");
			Assert::AreEqual(uint64_t(1), counters.queueFull.load(), L"visible");
			Assert::IsTrue(producer.ResetGen() != gen0, L"the hole in the pair chain resets the next fit");
			Assert::AreEqual(size_t(2), worker.Pending());
			Assert::IsFalse(worker.Running(), L"no thread was started");
		}

		TEST_METHOD(ResetIsConsumedBeforeTheNextFitByTheFittingThread)
		{
			CameraLink::FitCounters counters;
			CameraLink::CameraFitProducer producer(CameraModel::Config(), &counters);
			producer.NoteReset();
			producer.NoteReset();
			Assert::AreEqual(uint64_t(0), counters.resets.load(), L"the reader only bumps an atomic");
			CameraLink::CameraFitJob j = CameraLinkTest::PanJob(1, 6);
			const CameraLink::CameraFitResult r = producer.Fit(j);
			Assert::IsTrue(r.record && r.record->valid);
			Assert::AreEqual(uint64_t(1), counters.resets.load(), L"consumed once, then the fit");
			producer.Fit(CameraLinkTest::PanJob(2, 6));
			Assert::AreEqual(uint64_t(1), counters.resets.load());
		}

		TEST_METHOD(ASlipPairIsNotFittedAndPublishedAsAnUnusableRecord)
		{
			CameraLinkTest::Rig rig;
			CameraLink::CameraRecordPool pool(rig.Ring());
			CameraLink::FitCounters counters;
			CameraLink::CameraFitProducer producer(CameraModel::Config(), &counters);
			CameraLink::CameraFitWorker worker(producer, rig.Env(&pool, &counters));
			rig.PublishInputs(1);
			CameraLink::CameraFitJob j = CameraLinkTest::PanJob(1, 6);
			j.facts.spansSlip = true;
			Assert::IsTrue(worker.ProcessJob(j) == CameraLink::PublishOutcome::Published);
			CameraLink::PoolEntry e;
			Assert::IsTrue(pool.Get(1, e));
			Assert::IsTrue(e.record->reason == CameraModel::Reason::GenerationReset && !CameraLink::Usable(*e.record));
			Assert::AreEqual(uint64_t(1), counters.notFittedSlip.load());
			Assert::AreEqual(uint64_t(0), counters.fitted.load());
			Assert::AreEqual(uint64_t(1), counters.unusable.load());
		}

		TEST_METHOD(UnknownTimingGivesNoInputAndIsCounted)
		{
			CameraLink::FitCounters counters;
			CameraLink::CameraFitProducer producer(CameraModel::Config(), &counters);
			CameraLink::CameraFitJob j = CameraLinkTest::PanJob(1, 6, 0.0);   // no pair seconds, no timing latched
			const CameraLink::CameraFitResult r = producer.Fit(j);
			Assert::IsTrue(r.timingUnknown);
			Assert::IsTrue(r.record->reason == CameraModel::Reason::NoInput && !r.record->valid);
			Assert::AreEqual(uint64_t(1), counters.timingUnknown.load());
		}
		TEST_METHOD(AnUncommittedPoolEntryIsNeverServedButTheSealObserverMaySeeIt)
		{
			CameraLink::CameraRecordPool pool(4);
			auto rec = std::make_shared<CameraModel::CameraMotion>();
			rec->seq = 9;
			pool.Put(9, rec, nullptr);
			CameraLink::PoolEntry e;
			Assert::IsFalse(pool.Get(9, e), L"before the slot publish succeeded: not served");
			Assert::IsTrue(pool.Get(9, e, true), L"the seal observer accepts a Published slot");
			pool.Commit(9);
			Assert::IsTrue(pool.Get(9, e));
			pool.Put(9, rec, nullptr);   // a refill of the slot starts uncommitted again
			Assert::IsFalse(pool.Get(9, e));
			pool.Erase(9);
			Assert::IsFalse(pool.Get(9, e, true));
		}
	};
}
