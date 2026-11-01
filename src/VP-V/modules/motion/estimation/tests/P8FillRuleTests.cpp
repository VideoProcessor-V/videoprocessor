// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "CppUnitTest.h"

// Camera link K2-2: the P8 fill rule with a fake device: lane / first live / reuse / late discard / absent -> enabled = 0.
#include "CameraLinkTestSupport.h"
#include <VP-V/modules/motion/estimation/global_model/link/CameraLinkCore.h>
#include <VP-V/modules/motion/estimation/global_model/link/CameraLabelFill.h>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace VideoProcessorTest
{
	namespace
	{
		struct FakeDevice : CameraLink::ILabelDevice
		{
			int produced = 0;
			bool fail = false;
			std::vector<CameraLink::LabelTarget> targets;
			bool Produce(const CameraModel::CameraMotion&, const CameraLink::FieldCopy*, CameraLink::LabelTarget t) override
			{
				if (fail)
					return false;
				++produced;
				targets.push_back(t);
				return true;
			}
			pl_tex TextureOf(CameraLink::LabelTarget t) const override
			{
				return reinterpret_cast<pl_tex>(static_cast<uintptr_t>(0x1000 + (t.live ? 0x100 : 0) + t.index));
			}
		};

		struct Setup
		{
			CameraLinkTest::Rig rig;
			FrameAnalysis::Store store;
			CameraLink::CameraLinkCore core;
			FakeDevice device;
			CameraLink::CameraLabelFill fill;

			Setup() : core(store), fill(*rig.table, *core.Link(), device, rig.indicator.get())
			{
				CameraLink::CameraLinkSettings s;
				s.p7Wanted = true;
				s.deterministic = true;
				core.OnSettings(s);
				core.AttachGraph(rig.table.get(), rig.bytes.get(), rig.indicator.get());
				CameraLink::TimingFacts tf;
				tf.measuredSourceHz = 24;
				core.SetTimingFacts(tf);
				core.Producer()->NoteTiming();
			}

			// P7 published for seq (inputs first, then the fit).
			void PublishP7(uint64_t seq)
			{
				rig.PublishInputs(seq);
				CameraLink::FramePhaseFacts f;
				f.seq = seq;
				core.SubmitField(CameraLinkTest::PanJob(seq, 8).field, f);
				core.ProcessPending();
			}

			const std::vector<VpSkeleton::ProductFrameResult>& Seal(uint64_t seq)
			{
				const VpIndicator::Stamp st = rig.indicator->Seal(seq);
				return rig.table->Resolve(seq, st, [this](const VpSkeleton::Node& n, uint64_t s, int live, VpAncestry::Header& h, uint32_t) {
					return n.id == VpSkeleton::kP8CameraLabels ? fill.FillLive(s, live, h) : true; });
			}

			static const VpSkeleton::ProductFrameResult* Of(const std::vector<VpSkeleton::ProductFrameResult>& r, VpSkeleton::ProductId id)
			{
				for (const VpSkeleton::ProductFrameResult& x : r)
					if (x.id == id)
						return &x;
				return nullptr;
			}
		};
	}

	TEST_CLASS(P8FillRuleTests)
	{
	public:
		TEST_METHOD(LaneProducesInAnIdleGapBeforeTheSealAndTheSealAcceptsIt)
		{
			Setup t;
			t.PublishP7(1);
			VpSkeleton::IGapWork* gap = t.fill.GapWork();
			Assert::IsTrue(gap->HasWork());
			Assert::IsTrue(gap->Class() == VpSkeleton::GapClass::LaneCompute);
			gap->RunChunk(1000);
			Assert::AreEqual(1, t.device.produced);
			Assert::IsFalse(t.device.targets[0].live);
			Assert::IsFalse(gap->HasWork(), L"nothing more to do for the frame");
			const auto& res = t.Seal(1);
			const auto* p8 = Setup::Of(res, VpSkeleton::kP8CameraLabels);
			Assert::IsTrue(p8 && p8->final == VpSkeleton::SealOutcome::Sealed && !p8->liveFilled, L"lane result accepted");
			CameraLink::CameraLabelsView v;
			Assert::IsTrue(t.fill.LabelsFor(1, CameraLink::CameraConsumer::GrainMap, v));
			Assert::IsTrue(v.available && v.tex != nullptr);
			Assert::AreEqual(1, t.device.produced, L"no second production");
		}

		TEST_METHOD(FirstLiveFillThenEveryLaterConsumerReusesIt)
		{
			Setup t;
			t.PublishP7(1);   // no gap work ran: P8 not published at the seal
			const auto& res = t.Seal(1);
			const auto* p8 = Setup::Of(res, VpSkeleton::kP8CameraLabels);
			Assert::IsTrue(p8 && p8->liveFilled && p8->final == VpSkeleton::SealOutcome::Sealed, L"the seal's live fill is the ONE producer");
			Assert::AreEqual(1, t.device.produced);
			Assert::IsTrue(t.device.targets[0].live);
			CameraLink::CameraLabelsView a, b;
			Assert::IsTrue(t.fill.LabelsFor(1, CameraLink::CameraConsumer::GrainMap, a));
			Assert::IsTrue(t.fill.LabelsFor(1, CameraLink::CameraConsumer::Debander, b));
			Assert::IsTrue(a.tex == b.tex, L"same texture for every consumer");
			Assert::AreEqual(1, t.device.produced, L"reuse, never a second producer");
			Assert::AreEqual(uint64_t(2), t.fill.Counters().served.load());
		}

		TEST_METHOD(ALaneResultAfterTheSealIsDiscardedNotMerged)
		{
			Setup t;
			t.PublishP7(1);
			VpSkeleton::SlotSet* p8 = t.rig.table->Find(VpSkeleton::kP8CameraLabels);
			VpSkeleton::LaneTicket ticket;
			Assert::IsTrue(p8->Claim(1, VpAncestry::Provenance::AheadLane, ~uint64_t(0), &ticket) == VpSkeleton::ClaimResult::Claimed);
			t.Seal(1);   // the seal takes the frame while the lane is still writing
			Assert::IsTrue(p8->Publish(ticket) == VpSkeleton::PublishResult::Late, L"late result discarded by the slot protocol");
			Assert::IsTrue(p8->Counters().lateDiscards.load() >= 1);
		}

		TEST_METHOD(AbsentWhenP7WasNotSealedConsumersGetEnabledZero)
		{
			Setup t;
			t.rig.PublishInputs(1);          // P7 never published: the fit is late
			const auto& res = t.Seal(1);
			const auto* p8 = Setup::Of(res, VpSkeleton::kP8CameraLabels);
			Assert::IsTrue(p8 && p8->final != VpSkeleton::SealOutcome::Sealed, L"no live fit, no live labels");
			Assert::AreEqual(0, t.device.produced);
			CameraLink::CameraLabelsView v;
			Assert::IsFalse(t.fill.LabelsFor(1, CameraLink::CameraConsumer::GrainMap, v));
			Assert::IsFalse(v.available);
			CameraLink::CameraConsumerBinding binding(CameraLink::CameraConsumer::GrainMap, true);
			(void)binding;
			Assert::IsTrue(t.fill.Counters().absent.load() >= 1);
			Assert::IsTrue(t.fill.Counters().declined.load() >= 1);
		}

		TEST_METHOD(ADeviceFailureDeclinesTheLiveFillAndNeverClaimsLaneSuccess)
		{
			Setup t;
			t.PublishP7(1);
			t.device.fail = true;
			VpSkeleton::IGapWork* gap = t.fill.GapWork();
			gap->RunChunk(1000);
			Assert::AreEqual(uint64_t(0), t.fill.Counters().lane.load());
			Assert::AreEqual(uint64_t(1), t.fill.Counters().laneBlocked.load());
			const auto& res = t.Seal(1);
			const auto* p8 = Setup::Of(res, VpSkeleton::kP8CameraLabels);
			Assert::IsTrue(p8 && p8->final != VpSkeleton::SealOutcome::Sealed);
			CameraLink::CameraLabelsView v;
			Assert::IsFalse(t.fill.LabelsFor(1, CameraLink::CameraConsumer::GrainMap, v));
		}
	};
}
