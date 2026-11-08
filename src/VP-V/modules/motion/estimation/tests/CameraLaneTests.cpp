// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "CppUnitTest.h"

// Camera live wiring: CameraLane (link/CameraLane.*), the camera's own product table. The graph holds P7 (and P8 only when labels are
// wanted), the input markers are published per frame, the seal runs kSealLag frames behind the render and accepts a lane-published fit,
// a fit that arrives after its seal is late and never published, a field-less frame is told apart from a late fit, and a sequence that
// goes backwards or jumps is reported so the owner rebuilds. CPU only (a real CameraLinkCore in deterministic mode, no GPU).
#include "CameraLinkTestSupport.h"
#include <VP-V/modules/motion/estimation/global_model/link/CameraLane.h>
#include <VP-V/modules/motion/estimation/global_model/link/CameraLinkCore.h>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace VideoProcessorTest
{
	namespace
	{
		const uint64_t kLag = CameraLink::CameraLane::kSealLag;

		struct LaneRig
		{
			CameraLink::CameraLane lane;
			VpIndicator::Indicator indicator{ 1, 256 };
			FrameAnalysis::Store store;
			CameraLink::CameraLinkCore core{ store };

			explicit LaneRig(bool labels = false)
			{
				Assert::IsTrue(lane.Build(1920, 1080, labels), L"the camera graph builds");
				CameraLink::CameraLinkSettings on;
				on.p7Wanted = true;
				on.deterministic = true;
				core.OnSettings(on);
				core.AttachGraph(lane.Table(), lane.P7Bytes(), &indicator);
				CameraLink::TimingFacts tf;
				tf.measuredSourceHz = 24;
				core.SetTimingFacts(tf);
				core.Producer()->NoteTiming();
			}

			// One frame: the shared seal (the Indicator), then the lane's frame (markers, lagged seal).
			bool Frame(uint64_t seq, CameraLink::CameraLane::Sealed& sealed)
			{
				indicator.Seal(seq);
				return lane.OnFrame(seq, indicator, sealed);
			}
			// The fit of the field of `seq`, as the core's worker would run it (deterministic: on this thread).
			void Fit(uint64_t seq)
			{
				CameraLink::FramePhaseFacts f;
				f.seq = seq;
				core.SubmitField(CameraLinkTest::PanJob(seq, 8).field, f);
				core.ProcessPending();
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

		TEST_METHOD(TheSealRunsKSealLagFramesBehindAndAcceptsALanePublishedFit)
		{
			LaneRig rig;
			uint64_t sealedFrames = 0, acceptedFits = 0;
			for (uint64_t seq = 1; seq <= 14; ++seq)
			{
				CameraLink::CameraLane::Sealed sealed;
				Assert::IsTrue(rig.Frame(seq, sealed));
				if (seq < 1 + kLag)
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
			Assert::AreEqual<uint64_t>(14 - kLag, sealedFrames);
			Assert::AreEqual<uint64_t>(14, rig.core.Counters().published.load(), L"every fit reached its P7 slot");
			Assert::AreEqual<uint64_t>(0, rig.core.Counters().late.load(), L"none was late");
			Assert::IsTrue(acceptedFits >= sealedFrames - 1, L"the seal accepted the published fits (the first frame has no lag-1 inputs)");
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
