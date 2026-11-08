// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "CppUnitTest.h"

// Camera live, step 2: CameraLane (link/CameraLane.*) is the LAG BRIDGE until SK-W2, held through the SK-W LEASE path. The fit
// (CameraLinkCore::AttachLane) and the label fill hold the table through owned leases; every Build / Clear is a new generation, a job
// submitted against an older one is refused (PublishOutcome::StaleLane, counted, shown) and the holder rebinds. The bridge itself keeps
// only the lagged seal: the graph holds P7 (and P8 only when labels are wanted; the same declarations the skeleton receives), the input
// markers are published per frame, the seal runs kSealLag frames behind the render and accepts a lane-published fit, a fit after its
// seal is late and never published, a field-less frame is told apart from a late fit, and a sequence that goes backwards or jumps is
// reported so the owner rebuilds. The fit cost is posted to the motion pass-timer inbox. CPU only (a real CameraLinkCore in
// deterministic mode, no GPU).
#include "CameraLinkTestSupport.h"
#include <VP-V/core/PassTimerInbox.h>
#include <VP-V/modules/motion/estimation/global_model/CameraProducts.h>
#include <VP-V/modules/motion/estimation/global_model/link/CameraLane.h>
#include <VP-V/modules/motion/estimation/global_model/link/CameraLinkCore.h>

#include <memory>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace VideoProcessorTest
{
	namespace
	{
		const uint64_t kLag = CameraLink::CameraLane::kSealLag;

		struct LaneRig
		{
			CameraLink::CameraLane lane;
			std::shared_ptr<VpIndicator::Indicator> indicator = std::make_shared<VpIndicator::Indicator>(1, 256);
			FrameAnalysis::Store store;
			CameraLink::CameraLinkCore core{ store };
			bool labels = false;

			explicit LaneRig(bool withLabels = false) : labels(withLabels)
			{
				Assert::IsTrue(lane.Build(1920, 1080, labels, indicator), L"the camera graph builds");
				CameraLink::CameraLinkSettings on;
				on.p7Wanted = true;
				on.deterministic = true;
				core.OnSettings(on);
				core.AttachLane(lane.Source(VpSkeleton::kP7CameraFit));   // the lease path (as live), never the borrowed raw pointers
				CameraLink::TimingFacts tf;
				tf.measuredSourceHz = 24;
				core.SetTimingFacts(tf);
				core.Producer()->NoteTiming();
			}

			// One frame: the shared seal (the Indicator), then the lane's frame (markers, lagged seal). The fit rebinds by itself at
			// Submit and at its publish (CameraFitWorker::Rebind).
			bool Frame(uint64_t seq, CameraLink::CameraLane::Sealed& sealed)
			{
				indicator->Seal(seq);
				return lane.OnFrame(seq, *indicator, sealed);
			}
			// The fit of the field of `seq`, as the core's worker would run it (deterministic: on this thread).
			void Fit(uint64_t seq)
			{
				Submit(seq);
				core.ProcessPending();
			}
			void Submit(uint64_t seq)
			{
				CameraLink::FramePhaseFacts f;
				f.seq = seq;
				core.SubmitField(CameraLinkTest::PanJob(seq, 8).field, f);
			}
		};
	}

	TEST_CLASS(CameraLaneTests)
	{
	public:
		TEST_METHOD(TheGraphHoldsP7AndP8OnlyWhenLabelsAreWanted)
		{
			CameraLink::CameraLane plain;
			Assert::IsTrue(plain.Build(1920, 1080, false));
			Assert::IsTrue(plain.Table() != nullptr && plain.P7Bytes() != nullptr);
			Assert::IsTrue(plain.Table()->Find(VpSkeleton::kP7CameraFit) != nullptr, L"P7 is in the camera graph");
			Assert::IsTrue(plain.Table()->NodeOf(VpSkeleton::kP8CameraLabels) == nullptr, L"no labels consumer: no P8");
			Assert::IsFalse(plain.HasLabels());
			Assert::AreEqual(0, plain.LabelRing());

			CameraLink::CameraLane labelled;
			Assert::IsTrue(labelled.Build(1920, 1080, true));
			Assert::IsTrue(labelled.HasLabels());
			Assert::IsTrue(labelled.Table()->NodeOf(VpSkeleton::kP8CameraLabels) != nullptr, L"P8 exists once labels are wanted");
			Assert::IsTrue(labelled.LabelRing() > 0);

			labelled.Clear();
			Assert::IsFalse(labelled.Built());
			Assert::IsTrue(labelled.Table() == nullptr);
		}

		// The bridge's graph comes from the SAME declarations the camera gives the skeleton (LiveCameraDemand): the two cannot drift apart.
		TEST_METHOD(TheBridgeGraphIsBuiltFromTheDeclarationsTheSkeletonReceives)
		{
			for (int labels = 0; labels <= 1; ++labels)
			{
				std::vector<VpSkeleton::ConsumerDecl> declared;
				CameraLink::DeclareCameraConsumers(CameraLink::LiveCameraDemand(labels != 0), declared);
				Assert::AreEqual<size_t>(labels != 0 ? 2u : 1u, declared.size());
				VpSkeleton::GraphSettings gs;
				gs.dims.width = 1920;
				gs.dims.height = 1080;
				gs.renderAhead = CameraLink::CameraLane::kSealLag;
				gs.laneLead = 2;
				gs.queueDepth = 16;
				gs.ofaPresent = false;
				const VpSkeleton::Graph g = VpSkeleton::BuildGraph(VpSkeleton::SkeletonProducts(), declared, gs);
				Assert::IsTrue(g.valid);
				CameraLink::CameraLane lane;
				Assert::IsTrue(lane.Build(1920, 1080, labels != 0));
				Assert::AreEqual(g.Has(VpSkeleton::kP8CameraLabels), lane.Table()->NodeOf(VpSkeleton::kP8CameraLabels) != nullptr);
				Assert::AreEqual(g.Has(VpSkeleton::kP7CameraFit), lane.Table()->NodeOf(VpSkeleton::kP7CameraFit) != nullptr);
				Assert::AreEqual(g.Has(VpSkeleton::kP3AnalysisField), lane.Table()->NodeOf(VpSkeleton::kP3AnalysisField) != nullptr);
				Assert::AreEqual(g.Has(VpSkeleton::kP4SourceStats), lane.Table()->NodeOf(VpSkeleton::kP4SourceStats) != nullptr);
			}
		}

		TEST_METHOD(TheBridgeHandsOutOwnedLeasesWithANewGenerationAtEveryBuild)
		{
			std::shared_ptr<VpIndicator::Indicator> ind = std::make_shared<VpIndicator::Indicator>(1, 256);
			CameraLink::CameraLane lane;
			const VpSkeleton::LaneLease none = lane.Lease(VpSkeleton::kP7CameraFit);
			Assert::IsFalse(none.Valid(), L"no state: an invalid lease");
			Assert::IsTrue(lane.Build(1920, 1080, true, ind));
			const VpSkeleton::LaneLease p7 = lane.Lease(VpSkeleton::kP7CameraFit);
			Assert::IsTrue(p7.Owned() && p7.Valid(), L"an OWNED lease, as AnalysisLane::Lease gives (never a borrowed one)");
			Assert::IsTrue(p7.generation >= 1);
			Assert::IsTrue(p7.table == lane.Table());
			Assert::IsTrue(p7.bytes == lane.P7Bytes(), L"P7 is a CPU product: its bytes ring is in the lease");
			Assert::IsTrue(p7.indicator == ind.get(), L"the stamp source the fit reads after its pins");
			const VpSkeleton::LaneLease p8 = lane.Lease(VpSkeleton::kP8CameraLabels);
			Assert::IsTrue(p8.Valid() && p8.bytes == nullptr && p8.generation == p7.generation);
			Assert::AreEqual(p7.generation, lane.Source(VpSkeleton::kP7CameraFit)().generation, L"the source reads the current lease");
			// A rebuild: the old state lives on in the lease, the new one has a higher generation.
			std::weak_ptr<const void> oldState = p7.hold;
			Assert::IsTrue(lane.Build(1920, 1080, false, ind));
			const VpSkeleton::LaneLease next = lane.Lease(VpSkeleton::kP7CameraFit);
			Assert::IsTrue(next.generation > p7.generation);
			Assert::IsTrue(VpSkeleton::LaneLeaseNewer(next, p7));
			Assert::IsFalse(oldState.expired(), L"the held lease keeps the old table alive");
			Assert::IsTrue(p7.table->Find(VpSkeleton::kP7CameraFit) != nullptr, L"... and readable");
			// Clear: an invalid lease that is still newer than every lease of the dropped state.
			lane.Clear();
			const VpSkeleton::LaneLease cleared = lane.Lease(VpSkeleton::kP7CameraFit);
			Assert::IsFalse(cleared.Valid());
			Assert::IsTrue(VpSkeleton::LaneLeaseNewer(cleared, next));
		}

		TEST_METHOD(ALaneGenerationChangeRebindsTheFitAndRefusesTheStaleJobVisibly)
		{
			LaneRig rig;
			CameraLink::CameraLane::Sealed sealed;
			for (uint64_t seq = 1; seq <= 2; ++seq)
			{
				Assert::IsTrue(rig.Frame(seq, sealed));
				rig.Fit(seq);
			}
			Assert::AreEqual<uint64_t>(2, rig.core.Counters().published.load());
			const uint64_t gen1 = rig.lane.Generation();
			std::weak_ptr<const void> oldState = rig.lane.Lease(VpSkeleton::kP7CameraFit).hold;
			rig.Submit(3);   // queued against generation gen1
			Assert::IsTrue(rig.lane.Build(1920, 1080, false, rig.indicator), L"the bridge is rebuilt (a new generation)");
			Assert::IsTrue(rig.lane.Generation() > gen1);
			Assert::IsFalse(oldState.expired(), L"the worker's lease keeps the old table alive: nothing is freed under the fit");
			rig.core.ProcessPending();
			const CameraLink::FitCounters& c = rig.core.Counters();
			Assert::AreEqual<uint64_t>(1, c.staleLane.load(), L"the job of the old generation is refused");
			Assert::AreEqual<uint64_t>(1, c.rebound.load(), L"the worker rebound to the new generation");
			Assert::AreEqual<uint64_t>(2, c.published.load(), L"nothing was posted into the new graph's slots");
			Assert::IsTrue(rig.core.CausalRecord(3) == nullptr, L"a refused record is never served");
			Assert::IsTrue(CameraLink::StatusLine(c).find("1 stale lane") != std::string::npos, L"visible in the camera status line");
			Assert::IsTrue(oldState.expired(), L"once the worker rebound, the old state dies with its last holder");
			// The next frames run on the new generation: published again, no further refusal.
			for (uint64_t seq = 4; seq <= 6; ++seq)
			{
				Assert::IsTrue(rig.Frame(seq, sealed));
				rig.Fit(seq);
			}
			Assert::AreEqual<uint64_t>(1, c.staleLane.load(), L"no further refusals");
			Assert::IsTrue(c.published.load() >= 4, L"fits reach the new table");
			Assert::IsTrue(rig.core.CausalRecord(6) != nullptr);
		}

		TEST_METHOD(TheFitCostIsPostedToTheMotionPassTimerInbox)
		{
			LaneRig rig;
			{
				PassTimers::Accumulator earlier;   // whatever other tests left in the process-wide inbox
				PassTimers::Inbox::Motion().DrainInto(earlier);
			}
			const uint64_t postedBefore = PassTimers::Inbox::Motion().Posted();
			CameraLink::CameraLane::Sealed sealed;
			Assert::IsTrue(rig.Frame(1, sealed));
			rig.Fit(1);
			Assert::AreEqual<uint64_t>(1, rig.core.Counters().published.load());
			PassTimers::Accumulator acc;
			Assert::IsTrue(PassTimers::Inbox::Motion().DrainInto(acc) >= 1);
			Assert::IsTrue(PassTimers::Inbox::Motion().Posted() > postedBefore);
			const PassTimers::Stat* fit = acc.Find(PassTimers::Rows::kCameraFit);
			Assert::IsNotNull(fit, L"camera.fit reached the inbox the host drains into the pass-timer readout");
			Assert::IsTrue(fit->kind == PassTimers::Kind::CpuWork, L"the fit is work");
			Assert::IsTrue(fit->lastMs > 0.0, L"a measured cost");
			Assert::IsNull(acc.Find(PassTimers::Rows::kCameraReadback), L"no readback without a GPU ring: nothing invented");
		}

		TEST_METHOD(TheSealRunsKSealLagFramesBehindAndAcceptsALanePublishedFit)
		{
			LaneRig rig;
			uint64_t sealedFrames = 0, acceptedFits = 0;
			for (uint64_t seq = 1; seq <= 14; ++seq)
			{
				CameraLink::CameraLane::Sealed sealed;
				Assert::IsTrue(rig.Frame(seq, sealed));
				if (seq < kLag)
					Assert::IsFalse(sealed.any, L"nothing has left the window yet");
				else
				{
					Assert::IsTrue(sealed.any);
					Assert::AreEqual(seq - kLag, sealed.seq, L"exactly the frame that left the window");
					++sealedFrames;
					if (sealed.p7Published)
						++acceptedFits;
				}
				rig.Fit(seq);   // the fit of this frame's field is published long before its seal (the window)
			}
			Assert::AreEqual<uint64_t>(14 - kLag + 1, sealedFrames, L"frames 0 (the warm-up frame) .. 8");
			Assert::AreEqual<uint64_t>(14, rig.core.Counters().published.load(), L"every fit reached its P7 slot");
			Assert::AreEqual<uint64_t>(0, rig.core.Counters().late.load(), L"none was late");
			Assert::AreEqual<uint64_t>(0, rig.core.Counters().staleLane.load(), L"one generation throughout: nothing refused");
			Assert::IsTrue(acceptedFits >= sealedFrames - 2, L"the seal accepted the published fits (the warm-up frame has none)");
			Assert::AreEqual<uint64_t>(0, rig.lane.MarkerFailures(), L"no input marker failed after the warm-up frame");
		}

		TEST_METHOD(AFitThatArrivesAfterItsSealIsLateAndNeverPublished)
		{
			LaneRig rig;
			for (uint64_t seq = 1; seq <= kLag + 2; ++seq)
			{
				CameraLink::CameraLane::Sealed sealed;
				Assert::IsTrue(rig.Frame(seq, sealed));
			}
			// Frame 1 was sealed at frame kLag + 1 without a P7: its fit now is late.
			rig.Fit(1);
			Assert::AreEqual<uint64_t>(0, rig.core.Counters().published.load(), L"a late record is never served");
			Assert::AreEqual<uint64_t>(1, rig.core.Counters().late.load());
			Assert::IsTrue(rig.core.CausalRecord(1) == nullptr);
		}

		TEST_METHOD(AFrameWithoutAFieldIsToldApartFromALateFit)
		{
			LaneRig rig;
			rig.lane.NoteField(3);   // the engine had a verified field for frame 3 only (frame 2 was a cadence repeat)
			bool sawThree = false, sawTwo = false;
			for (uint64_t seq = 1; seq <= 10; ++seq)
			{
				CameraLink::CameraLane::Sealed sealed;
				Assert::IsTrue(rig.Frame(seq, sealed));
				if (sealed.any && sealed.seq == 3)
				{
					sawThree = true;
					Assert::IsTrue(sealed.hadField);
				}
				if (sealed.any && sealed.seq == 2)
				{
					sawTwo = true;
					Assert::IsFalse(sealed.hadField, L"nothing to fit there: not a late fit");
				}
			}
			Assert::IsTrue(sawThree && sawTwo);
		}

		TEST_METHOD(ABackwardsOrJumpingSequenceIsReportedForARebuild)
		{
			LaneRig rig;
			CameraLink::CameraLane::Sealed sealed;
			Assert::IsTrue(rig.Frame(10, sealed));
			Assert::IsTrue(rig.Frame(11, sealed));
			Assert::IsFalse(rig.Frame(11, sealed), L"the same frame twice");
			Assert::IsFalse(rig.Frame(5, sealed), L"backwards");
			Assert::IsFalse(rig.Frame(11 + 1000, sealed), L"a jump past the window");
			const uint64_t sealedBefore = rig.lane.FramesSealed();
			Assert::IsTrue(rig.Frame(12, sealed), L"a refused call changed nothing: the next frame is accepted");
			Assert::IsTrue(rig.lane.FramesSealed() >= sealedBefore);
		}

		TEST_METHOD(TwoHundredFramesWrapTheRingsCleanlyAndReleaseEveryPin)
		{
			LaneRig rig;
			const VpSkeleton::SlotSet* p7 = rig.lane.Table()->Find(VpSkeleton::kP7CameraFit);
			const size_t ring = p7->Ring();
			Assert::IsTrue(ring < 100, L"the test wraps the ring many times");
			uint64_t sealedCount = 0;
			for (uint64_t seq = 1; seq <= 240; ++seq)
			{
				CameraLink::CameraLane::Sealed sealed;
				Assert::IsTrue(rig.Frame(seq, sealed));
				if (sealed.any)
					++sealedCount;
				rig.Fit(seq);
			}
			// A pin that was never released would keep a slot unwritable: the next tenant's Claim / Writable would fail and the fit would be
			// late, an input marker would fail, or PinRef would give up. None of that happened over 240 frames (> 30 ring wraps).
			Assert::AreEqual<uint64_t>(240, rig.core.Counters().published.load(), L"every fit reached its slot: no slot stayed pinned");
			Assert::AreEqual<uint64_t>(0, rig.core.Counters().late.load());
			Assert::AreEqual<uint64_t>(0, rig.lane.MarkerFailures());
			Assert::AreEqual<uint64_t>(240 - kLag + 1, sealedCount, L"one frame sealed per call after the window filled");
			for (size_t i = 0; i < rig.lane.Table()->Count(); ++i)
			{
				const VpSkeleton::SlotCounters& c = rig.lane.Table()->At(i).Counters();
				Assert::AreEqual<uint64_t>(0, c.pinGiveUps.load(), L"PinRef never gave up");
				Assert::AreEqual<uint64_t>(0, c.overruns.load(), L"no overrun: no slot was reclaimed before its release");
				Assert::AreEqual<uint64_t>(0, c.doubleSeals.load());
			}
			// The newest frames are still readable (their slots are the live tenants after the wraps).
			Assert::IsTrue(rig.core.CausalRecord(240) != nullptr);
		}

		TEST_METHOD(MarkersAreOnlyPublishedForTheInputNodes)
		{
			LaneRig rig(true);
			CameraLink::CameraLane::Sealed sealed;
			Assert::IsTrue(rig.Frame(1, sealed));
			Assert::IsTrue(rig.lane.MarkersPublished() > 0);
			const VpSkeleton::SlotSet* p7 = rig.lane.Table()->Find(VpSkeleton::kP7CameraFit);
			Assert::IsTrue(p7->StateOf(1) == VpSkeleton::SlotState::Empty, L"P7 is the fit's, never a marker");
			const VpSkeleton::SlotSet* p8 = rig.lane.Table()->Find(VpSkeleton::kP8CameraLabels);
			Assert::IsTrue(p8->StateOf(1) == VpSkeleton::SlotState::Empty, L"P8 is the label fill's, never a marker");
		}
	};
}
