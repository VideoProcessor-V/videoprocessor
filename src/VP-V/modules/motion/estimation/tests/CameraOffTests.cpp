// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "CppUnitTest.h"

// Camera link K1-6: with no consumer of P7 the link is inert: zero threads, no pool / producer / worker / ring, nothing
// published, no status noise (byte-identical to not having the camera). Plus the status ids and the ambiguity tracker. CPU only.
#include "CameraLinkTestSupport.h"
#include <VP-V/modules/motion/estimation/global_model/link/CameraLinkCore.h>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace VideoProcessorTest
{
	TEST_CLASS(CameraOffTests)
	{
	public:
		TEST_METHOD(NoP7ConsumerMeansNoThreadAndNoAllocation)
		{
			FrameAnalysis::Store store;
			CameraLink::CameraLinkCore core(store);
			const int threadsBefore = CameraLink::CameraFitWorker::ThreadsEverStarted();
			CameraLink::CameraLinkSettings off;   // p7Wanted false
			core.OnSettings(off);
			// A graph that does not contain P7 (no camera consumer): the link does not even look at it.
			VpSkeleton::Graph g = VpSkeleton::BuildGraph(VpSkeleton::SkeletonProducts(), {}, CameraLinkTest::Settings());
			VpSkeleton::SlotTable table(g);
			core.AttachGraph(&table, nullptr, nullptr);
			CameraLink::FramePhaseFacts f;
			for (uint64_t s = 1; s <= 20; ++s)
			{
				f.seq = s;
				core.OnFrameStart(f);
				core.OnPostSubmitGap();
			}
			core.OnGpuCreated(nullptr);
			core.OnGpuReleased();
			Assert::IsFalse(core.Active());
			Assert::IsFalse(core.Allocated(), L"no producer, pool or worker exists");
			Assert::IsFalse(core.HasThread());
			Assert::IsTrue(core.Pool() == nullptr && core.Worker() == nullptr);
			Assert::AreEqual(threadsBefore, CameraLink::CameraFitWorker::ThreadsEverStarted(), L"zero threads started");
			std::vector<std::string> lines;
			core.CollectStatus(lines);
			Assert::AreEqual(size_t(0), lines.size(), L"no status noise when off");
			Assert::AreEqual(uint64_t(0), core.Counters().sealedOk.load() + core.Counters().sealedLate.load());
			Assert::IsTrue(core.SealedRecord(3) == nullptr && core.CausalRecord(3) == nullptr && core.FieldOf(3) == nullptr);
			CameraLink::CameraLabelsView v;
			Assert::IsFalse(core.LabelsFor(3, CameraLink::CameraConsumer::GrainMap, v) || v.available);
			Assert::IsTrue(core.GuideFor(3) == nullptr);
		}

		TEST_METHOD(WantedButGraphHasNoP7StaysInert)
		{
			FrameAnalysis::Store store;
			CameraLink::CameraLinkCore core(store);
			CameraLink::CameraLinkSettings on;
			on.p7Wanted = true;
			core.OnSettings(on);
			VpSkeleton::Graph g = VpSkeleton::BuildGraph(VpSkeleton::SkeletonProducts(), {}, CameraLinkTest::Settings());
			VpSkeleton::SlotTable table(g);
			const int threadsBefore = CameraLink::CameraFitWorker::ThreadsEverStarted();
			core.AttachGraph(&table, nullptr, nullptr);
			Assert::IsFalse(core.Allocated());
			Assert::AreEqual(threadsBefore, CameraLink::CameraFitWorker::ThreadsEverStarted());
		}

		TEST_METHOD(WithP7TheOneThreadStartsAndStopsWithTheGraph)
		{
			CameraLinkTest::Rig rig;
			FrameAnalysis::Store store;
			CameraLink::CameraLinkCore core(store);
			CameraLink::CameraLinkSettings on;
			on.p7Wanted = true;
			core.OnSettings(on);
			const int threadsBefore = CameraLink::CameraFitWorker::ThreadsEverStarted();
			core.AttachGraph(rig.table.get(), rig.bytes.get(), rig.indicator.get());
			Assert::IsTrue(core.Allocated() && core.HasThread());
			Assert::AreEqual(threadsBefore + 1, CameraLink::CameraFitWorker::ThreadsEverStarted());
			core.OnSettings(CameraLink::CameraLinkSettings());   // consumers gone
			Assert::IsFalse(core.Allocated() || core.HasThread(), L"torn down with the last consumer");
		}

		TEST_METHOD(DeterministicModeStartsNoThreadAndSealsAreCounted)
		{
			CameraLinkTest::Rig rig;
			FrameAnalysis::Store store;
			CameraLink::CameraLinkCore core(store);
			CameraLink::CameraLinkSettings on;
			on.p7Wanted = true;
			on.deterministic = true;
			core.OnSettings(on);
			const int threadsBefore = CameraLink::CameraFitWorker::ThreadsEverStarted();
			core.AttachGraph(rig.table.get(), rig.bytes.get(), rig.indicator.get());
			Assert::IsTrue(core.Allocated());
			Assert::IsFalse(core.HasThread());
			Assert::AreEqual(threadsBefore, CameraLink::CameraFitWorker::ThreadsEverStarted());
			CameraLink::TimingFacts tf;
			tf.measuredSourceHz = 24;
			core.SetTimingFacts(tf);
			core.Producer()->NoteTiming();
			CameraLink::FramePhaseFacts f;
			f.seq = 1;
			rig.PublishInputs(1);
			CameraLinkTest::PanJob(1, 8);   // building a job exercises the helper
			auto copy = CameraLinkTest::PanJob(1, 8).field;
			Assert::IsTrue(core.SubmitField(copy, f));
			Assert::AreEqual(size_t(1), core.ProcessPending());
			f.p7SealedPublished = true;
			core.OnFrameStart(f);   // the seal observer hands the sealed record to the store (no Begin: refused, counted)
			Assert::AreEqual(uint64_t(1), core.Counters().published.load());
			std::vector<std::string> lines;
			core.CollectStatus(lines);
			Assert::IsTrue(lines.size() >= 1, L"status is visible when the camera is on");
			Assert::IsNotNull(core.CausalRecord(1).get());
		}

		TEST_METHOD(AmbiguousEngagesAfterHalfAMeasuredSecondAndReleasesOnTheFirstOtherRecord)
		{
			CameraLink::AmbiguousTracker t;
			const double dt = 1.0 / 24;
			int frames = 0;
			while (!t.Observe(CameraModel::Reason::Ambiguous, dt))
				++frames;
			Assert::IsTrue((frames + 1) * dt >= CameraLink::kAmbiguousEngageSeconds - 1e-12);
			Assert::IsTrue((frames) * dt < CameraLink::kAmbiguousEngageSeconds, L"not before 0.5 s measured");
			Assert::IsFalse(t.Observe(CameraModel::Reason::Ok, dt), L"auto release");
			// At 60 fps the same 0.5 s takes 30 frames: measured seconds, never a frame count.
			CameraLink::AmbiguousTracker u;
			int n = 0;
			while (!u.Observe(CameraModel::Reason::Ambiguous, 1.0 / 60))
				++n;
			Assert::IsTrue(n + 1 >= 29 && n + 1 <= 31);
		}

		TEST_METHOD(EveryNoModelIdIsDeclaredAndReleasedWhenNothingIsWrong)
		{
			CameraLink::StatusFacts quiet;
			quiet.graphHasP7 = true;
			for (const FallbackCatalog::Decision& d : CameraLink::BuildDecisions(quiet))
				Assert::IsFalse(d.engaged);
			CameraLink::StatusFacts bad = quiet;
			bad.lateNow = bad.ambiguousNow = bad.timingUnknownNow = bad.readbackFailedNow = bad.slipNow = bad.laneDepthZero = bad.queueFullNow = true;
			const std::vector<FallbackCatalog::Decision> ds = CameraLink::BuildDecisions(bad);
			Assert::AreEqual(size_t(7), ds.size());
			const char* ids[] = { CameraLink::kIdNoModelLate, CameraLink::kIdNoModelAmbiguous, CameraLink::kIdNoModelTiming,
				CameraLink::kIdNoModelReadback, CameraLink::kIdNoModelSlip, CameraLink::kIdNoModelQueue, CameraLink::kIdNoModelDepth0 };
			for (size_t i = 0; i < 7; ++i)
			{
				Assert::AreEqual(std::string(ids[i]), ds[i].id);
				Assert::IsTrue(ds[i].engaged && !ds[i].reason.empty());
			}
			// Without P7 in the graph nothing is ever engaged.
			bad.graphHasP7 = false;
			for (const FallbackCatalog::Decision& d : CameraLink::BuildDecisions(bad))
				Assert::IsFalse(d.engaged);
		}
		static bool Engaged(CameraLink::CameraLinkCore& core, const char* id)
		{
			for (const FallbackCatalog::Decision& d : core.CollectDecisions())
				if (d.id == id)
					return d.engaged;
			return false;
		}

		TEST_METHOD(AFallbackIsHeldForAMeasuredIntervalNotPerCall)
		{
			CameraLinkTest::Rig rig;
			FrameAnalysis::Store store;
			CameraLink::CameraLinkCore core(store);
			CameraLink::CameraLinkSettings on;
			on.p7Wanted = true;
			on.deterministic = true;
			core.OnSettings(on);
			core.AttachGraph(rig.table.get(), rig.bytes.get(), rig.indicator.get());
			CameraLink::FramePhaseFacts f;
			f.displayPeriodSeconds = 0.1;   // a measured 10 Hz output
			f.seq = 1;
			f.p7SealedPublished = false;
			core.OnFrameStart(f);
			Assert::IsTrue(Engaged(core, CameraLink::kIdNoModelLate), L"engaged by the late seal");
			f.p7SealedPublished = true;
			int held = 0;
			for (uint64_t s = 2; s < 20; ++s)
			{
				f.seq = s;
				core.OnFrameStart(f);
				if (!Engaged(core, CameraLink::kIdNoModelLate))
					break;
				++held;
			}
			Assert::IsTrue(held >= 3 && held <= 5, L"held for 0.5 s of measured time (4 frames at 0.1 s), then released");
			Assert::IsFalse(Engaged(core, CameraLink::kIdNoModelLate));
		}

		TEST_METHOD(AQueueFullDropEngagesTheQueueIdAndIsHeld)
		{
			CameraLinkTest::Rig rig;
			FrameAnalysis::Store store;
			CameraLink::CameraLinkCore core(store);
			CameraLink::CameraLinkSettings on;
			on.p7Wanted = true;
			on.deterministic = true;
			on.queueCapacity = 1;
			core.OnSettings(on);
			core.AttachGraph(rig.table.get(), rig.bytes.get(), rig.indicator.get());
			CameraLink::FramePhaseFacts f;
			f.displayPeriodSeconds = 0.1;
			f.p7SealedPublished = true;
			auto copy = CameraLinkTest::PanJob(1, 8).field;
			core.Producer()->SetTimingFacts(CameraLink::TimingFacts());
			Assert::IsTrue(core.SubmitField(copy, f));
			Assert::IsFalse(core.SubmitField(copy, f), L"capacity 1: the second is dropped");
			f.seq = 1;
			core.OnFrameStart(f);   // deterministic: also drains the queue
			Assert::IsTrue(Engaged(core, CameraLink::kIdNoModelQueue));
			Assert::IsTrue(core.Counters().queueFull.load() >= 1);
			for (uint64_t s = 2; s < 4; ++s)
			{
				f.seq = s;
				core.OnFrameStart(f);
			}
			Assert::IsTrue(Engaged(core, CameraLink::kIdNoModelQueue), L"still held 0.2 s later");
		}

		struct EraEngine : VpMotion::IVectorEngine
		{
			uint64_t era = 1, latest = 0;
			uint64_t LatestVerifiedFieldSeq() const override { return latest; }
			uint64_t FieldEra() const override { return era; }
			bool FieldOf(uint64_t, VpMotion::VerifiedField&) const override { return false; }
			float FieldScale() const override { return 1; }
			bool GlobalFromEngine() const override { return false; }
			bool EngineGlobal(uint64_t, VpMotion::GlobalVec&) const override { return false; }
			bool PairSpansSlip(uint64_t, uint64_t) const override { return false; }
			bool FlowIsCpuResident() const override { return false; }
			bool FlowOf(uint64_t, VpMotion::FlowView&) const override { return false; }
			void SetPreEstimateHook(VpMotion::IPreEstimateHook*) override {}
		};

		TEST_METHOD(AFieldEraChangeForgetsTheLastRequestedField)
		{
			CameraLinkTest::Rig rig;
			FrameAnalysis::Store store;
			CameraLink::CameraLinkCore core(store);
			CameraLink::CameraLinkSettings on;
			on.p7Wanted = true;
			on.deterministic = true;
			core.OnSettings(on);
			core.AttachGraph(rig.table.get(), rig.bytes.get(), rig.indicator.get());
			EraEngine engine;
			core.SetEngine(&engine);
			CameraLink::FramePhaseFacts f;
			f.p7SealedPublished = true;
			core.OnFrameStart(f);
			Assert::AreEqual(uint64_t(0), core.LastRequestedField());
			core.DebugSetLastRequestedField(7);   // the request loop needs a GPU ring; the era bookkeeping does not
			core.OnFrameStart(f);
			Assert::AreEqual(uint64_t(7), core.LastRequestedField(), L"same era: kept");
			engine.era = 2;
			core.OnFrameStart(f);
			Assert::AreEqual(uint64_t(0), core.LastRequestedField(), L"the era changed: forgotten");
		}
	};
}
