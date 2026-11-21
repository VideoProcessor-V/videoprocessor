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
#include <VP-V/modules/motion/estimation/global_model/link/CameraFitFieldPod.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <functional>
#include <memory>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace VideoProcessorTest
{
	namespace
	{
		struct FakeDevice : CameraLink::ILabelDevice
		{
			int produced = 0;
			bool fail = false;
			std::function<void()> onProduce;   // test hook: runs at the start of every production (a rebuild "during" it)
			std::vector<CameraLink::LabelTarget> targets;
			bool Produce(const CameraModel::CameraMotion&, const CameraLink::FieldCopy*, CameraLink::LabelTarget t) override
			{
				if (onProduce)
					onProduce();
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

			// `workerField` false: the camera worker gets no field storage (P7 is published, its fit field is not).
			explicit Setup(bool workerField = true) : core(store), fill(*rig.table, *core.Link(), device, rig.indicator.get())
			{
				CameraLink::CameraLinkSettings s;
				s.p7Wanted = true;
				s.deterministic = true;
				core.OnSettings(s);
				// CP-A1-D4-CAMLABEL-FIELDCOPY: the fit field product's storage, for the worker (publishes it with P7) and the fill (P8's only
				// field read). Without it the worker counts fieldNoStorage and P8 has no labels.
				core.SetFieldSources(workerField ? rig.FieldSource() : VpSkeleton::LaneLeaseSource(), VpSkeleton::LaneLeaseSource());
				fill.SetFieldSource(rig.FieldSource());
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
				const auto& res = rig.table->Resolve(seq, st, [this](const VpSkeleton::Node& n, uint64_t s, int live, VpAncestry::Header& h, uint32_t) {
					return n.id == VpSkeleton::kP8CameraLabels ? fill.FillLive(s, live, h) : true; });
				return res;
			}

			// Publish the fit field of `seq` by hand (as the worker would), with `honoured` and `bytes` as its content.
			bool PublishField(uint64_t seq, const VpIndicator::Stamp& honoured, const std::vector<uint8_t>& bytes)
			{
				VpSkeleton::SlotSet* pf = rig.table->Find(VpSkeleton::kCameraFitField);
				VpSkeleton::LaneTicket t;
				if (!pf || pf->Claim(seq, VpAncestry::Provenance::AheadLane, ~uint64_t(0), &t) != VpSkeleton::ClaimResult::Claimed)
					return false;
				VpAncestry::Header& h = pf->LaneHeader(t);
				VpSkeleton::LaneInputs pins;
				rig.table->LaneAncestors(VpSkeleton::kCameraFitField, seq, h, pins);
				uint8_t* dst = rig.fieldBytes->Ring(static_cast<size_t>(t.slot));
				std::memset(dst, 0, rig.fieldBytes->Bytes());
				std::memcpy(dst, bytes.data(), (std::min)(bytes.size(), rig.fieldBytes->Bytes()));
				h.honoured = honoured;
				pf->Publish(t);
				pins.Unpin();
				return true;
			}

			VpIndicator::Stamp P7Stamp(uint64_t seq)
			{
				const VpSkeleton::ReadInfo r = rig.table->Find(VpSkeleton::kP7CameraFit)->Read(seq, true);
				return r.header ? r.header->honoured : VpIndicator::Stamp();
			}

			static std::vector<uint8_t> PodOf(const CameraLink::FieldCopy& f)
			{
				std::vector<uint8_t> b(CameraLink::FitFieldPodBytes(f.Blocks(), f.fwdTrust.size() == f.Blocks()));
				CameraLink::EncodeFitFieldPod(f, b.data(), b.size());
				return b;
			}

			uint64_t FieldMisses() const
			{
				return fill.Counters().fieldMissing.load() + fill.Counters().fieldStampMismatch.load() + fill.Counters().fieldUnreadable.load();
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
		// SK-W fix: the fill used to keep a raw SlotTable&; the lane swaps its whole state at every rebuild.
		TEST_METHOD(ARebuildDuringALaneProductionRefusesThePublishCountsItAndKeepsTheOldStateAliveUntilReleased)
		{
			Setup t;
			t.PublishP7(1);
			std::shared_ptr<int> oldState = std::make_shared<int>(1);
			const std::shared_ptr<int> newState = std::make_shared<int>(2);
			std::weak_ptr<int> weakOld = oldState;
			std::atomic<uint64_t> gen{ 1 };
			VpSkeleton::SlotTable* table = t.rig.table.get();
			VpSkeleton::CpuBuffers* bytes = t.rig.bytes.get();
			VpIndicator::Indicator* ind = t.rig.indicator.get();
			const std::shared_ptr<int> firstState = oldState;
			CameraLink::CameraLabelFill fill(
				[&gen, table, bytes, ind, firstState, newState]() {
					VpSkeleton::LaneLease l = VpSkeleton::LaneLease::Borrow(table, bytes, ind);
					l.generation = gen.load();
					l.hold = gen.load() == 1 ? std::shared_ptr<const void>(firstState) : std::shared_ptr<const void>(newState);
					return l;
				},
				*t.core.Link(), t.device);
			VpSkeleton::CpuBuffers* fieldBytes = t.rig.fieldBytes.get();
			fill.SetFieldSource([&gen, table, fieldBytes, ind]() {   // the field product's lease follows the same generation as P8's
				VpSkeleton::LaneLease l = VpSkeleton::LaneLease::Borrow(table, fieldBytes, ind);
				l.generation = gen.load();
				return l;
			});
			oldState.reset();
			Assert::AreEqual(uint64_t(1), fill.LaneGeneration());
			Assert::IsFalse(weakOld.expired(), L"generation 1 is owned by the fill's lease (and the source's capture)");
			t.device.onProduce = [&gen]() { gen = 2; };   // the lane rebuilds while the device is producing
			VpSkeleton::IGapWork* gap = fill.GapWork();
			Assert::IsTrue(gap->HasWork());
			gap->RunChunk(1000);
			Assert::AreEqual(1, t.device.produced, L"the device ran against the held (old) table: no access to freed memory");
			Assert::AreEqual(uint64_t(1), fill.Counters().staleLane.load(), L"the publish into the old generation was refused and counted");
			Assert::AreEqual(uint64_t(0), fill.Counters().lane.load());
			Assert::AreEqual(uint64_t(0), fill.Counters().laneLate.load());
			Assert::AreEqual(uint64_t(2), fill.LaneGeneration(), L"the fill rebound to the new generation");
			Assert::AreEqual(uint64_t(1), fill.Counters().rebound.load());
			Assert::IsTrue(CameraLink::LabelFillStatusLine(fill.Counters()).find("1 stale lane") != std::string::npos, L"visible in the status line");
			Assert::IsFalse(weakOld.expired(), L"the source's own capture still holds it here; the fill's lease and the production's copy are gone");
		}

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

		// CP-A1-D4-CAMLABEL-FIELDCOPY: P8 reads its field ONLY from the declared product p7f_camera_fit_field. Each miss gives no labels, is
		// counted by its cause (camera-labels status line) and never falls back to another copy: the device is never called.
		TEST_METHOD(AMissingFitFieldGivesNoLabelsCountedOncePerFrameOnTheLaneAndAtTheSeal)
		{
			Setup t(false);   // P7 is published, its fit field is not
			t.PublishP7(1);
			Assert::IsTrue(t.rig.table->Find(VpSkeleton::kP7CameraFit)->StateOf(1) == VpSkeleton::SlotState::Published, L"premise: P7 published");
			Assert::IsFalse(t.rig.table->Find(VpSkeleton::kCameraFitField)->StateOf(1) == VpSkeleton::SlotState::Published, L"premise: no field");
			Assert::AreEqual(uint64_t(1), t.core.Counters().fieldNoStorage.load(), L"premise: the worker counted why it did not publish it");
			VpSkeleton::IGapWork* gap = t.fill.GapWork();
			Assert::IsFalse(gap->HasWork(), L"the lane has nothing to produce without the field");
			Assert::IsFalse(gap->HasWork());
			Assert::AreEqual(uint64_t(1), t.fill.Counters().fieldMissing.load(), L"the missing field is counted once for the frame, not per poll");
			const auto& res = t.Seal(1);
			const auto* p8 = Setup::Of(res, VpSkeleton::kP8CameraLabels);
			Assert::IsTrue(p8 && p8->final != VpSkeleton::SealOutcome::Sealed, L"no labels for the frame");
			Assert::AreEqual(0, t.device.produced, L"no fallback to any other field copy: the device never ran");
			Assert::IsTrue(t.fill.Counters().fieldMissing.load() >= 1);
			Assert::AreEqual(uint64_t(0), t.fill.Counters().fieldStampMismatch.load() + t.fill.Counters().fieldUnreadable.load(), L"named by its cause");
			const std::string line = CameraLink::LabelFillStatusLine(t.fill.Counters());
			Assert::IsTrue(line.find("fit field missing (no labels)") != std::string::npos);
			CameraLink::CameraLabelsView v;
			Assert::IsFalse(t.fill.LabelsFor(1, CameraLink::CameraConsumer::GrainMap, v) || v.available);
		}

		TEST_METHOD(AFitFieldWithAnotherStampThanP7GivesNoLabelsAndIsCounted)
		{
			Setup t(false);
			t.PublishP7(1);
			VpIndicator::Stamp other = t.P7Stamp(1);
			other.v[0] += 1;   // a resync between the two: the field is not P7's
			Assert::IsTrue(t.PublishField(1, other, Setup::PodOf(*CameraLinkTest::PanJob(1, 8).field)), L"premise: the field is published");
			VpSkeleton::IGapWork* gap = t.fill.GapWork();
			Assert::IsTrue(gap->HasWork(), L"premise: both products are published, the lane tries");
			gap->RunChunk(1000);
			Assert::AreEqual(0, t.device.produced, L"no labels from a field of another frame state");
			Assert::AreEqual(uint64_t(1), t.fill.Counters().fieldStampMismatch.load());
			Assert::AreEqual(uint64_t(0), t.fill.Counters().lane.load());
			Assert::IsTrue(t.fill.Counters().laneBlocked.load() >= 1);
			// Control: the same content under P7's own stamp gives labels.
			Setup c(false);
			c.PublishP7(1);
			Assert::IsTrue(c.PublishField(1, c.P7Stamp(1), Setup::PodOf(*CameraLinkTest::PanJob(1, 8).field)));
			c.fill.GapWork()->RunChunk(1000);
			Assert::AreEqual(1, c.device.produced, L"control: P7's stamp and a valid pod give labels");
			Assert::AreEqual(uint64_t(0), c.FieldMisses());
		}

		TEST_METHOD(AnUnreadableFitFieldGivesNoLabelsAndIsCounted)
		{
			// (a) the fill has no field storage at all.
			{
				Setup t;
				t.fill.SetFieldSource(VpSkeleton::LaneLeaseSource());
				t.PublishP7(1);
				t.fill.GapWork()->RunChunk(1000);
				Assert::AreEqual(0, t.device.produced);
				Assert::AreEqual(uint64_t(1), t.fill.Counters().fieldUnreadable.load());
			}
			// (b) the field storage is of another generation than P8's lease.
			{
				Setup t;
				const VpSkeleton::LaneLeaseSource inner = t.rig.FieldSource();
				t.fill.SetFieldSource([inner]() { VpSkeleton::LaneLease l = inner(); l.generation = 9; return l; });
				t.PublishP7(1);
				t.fill.GapWork()->RunChunk(1000);
				Assert::AreEqual(0, t.device.produced);
				Assert::AreEqual(uint64_t(1), t.fill.Counters().fieldUnreadable.load());
			}
			// (c) the slot is published under P7's stamp but holds no valid pod.
			{
				Setup t(false);
				t.PublishP7(1);
				Assert::IsTrue(t.PublishField(1, t.P7Stamp(1), std::vector<uint8_t>(64, 0x5A)));
				t.fill.GapWork()->RunChunk(1000);
				Assert::AreEqual(0, t.device.produced);
				Assert::AreEqual(uint64_t(1), t.fill.Counters().fieldUnreadable.load());
				Assert::AreEqual(uint64_t(0), t.fill.Counters().fieldMissing.load() + t.fill.Counters().fieldStampMismatch.load());
				Assert::IsTrue(CameraLink::LabelFillStatusLine(t.fill.Counters()).find("1 fit field unreadable (no labels)") != std::string::npos);
			}
		}

		TEST_METHOD(TheLabelsReadTheFieldFromTheProductSlotAndNothingElse)
		{
			// The field the device receives is the decoded slot content: a marked field published into the slot is what the device sees.
			struct Capture : CameraLink::ILabelDevice
			{
				std::vector<float> fx;
				int calls = 0;
				bool Produce(const CameraModel::CameraMotion&, const CameraLink::FieldCopy* f, CameraLink::LabelTarget) override
				{
					++calls;
					if (f)
						fx = f->fwdX;
					return true;
				}
				pl_tex TextureOf(CameraLink::LabelTarget) const override { return nullptr; }
			};
			Setup t(false);
			t.PublishP7(1);
			CameraLink::FieldCopy mark = *CameraLinkTest::PanJob(1, 8).field;
			for (float& x : mark.fwdX)
				x = 42.5f;
			Assert::IsTrue(t.PublishField(1, t.P7Stamp(1), Setup::PodOf(mark)));
			Capture cap;
			CameraLink::CameraLabelFill fill(*t.rig.table, *t.core.Link(), cap, t.rig.indicator.get());
			fill.SetFieldSource(t.rig.FieldSource());
			fill.GapWork()->RunChunk(1000);
			Assert::AreEqual(1, cap.calls);
			Assert::IsTrue(!cap.fx.empty() && cap.fx.front() == 42.5f && cap.fx.back() == 42.5f, L"the device got the slot's field, not the fit's input");
		}
	};
}
