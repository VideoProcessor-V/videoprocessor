// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "CppUnitTest.h"

// GEM-127 (CAM-LINK-LIFECYCLE, MUST-4), TEST FIRST (lead20/plan-CE-gemini-packages.md s2): when the source sequence is RENUMBERED (the
// engine's field era, the one vectors-valid epoch, changes and the frame numbers restart lower), the three per-link latches must not hold
// the camera on the old numbering:
//   (1) the worker's render position (CameraFitWorker::Tick only moves it forward): the AHEAD FIT must judge new-era frames on time again;
//   (2) the on-time seal marks (m_onTimeMark / m_retired): the LAG-BRIDGE seal of a new-era frame must run (not be skipped or judged by an
//       old-era mark), from the FIRST frame of the new era;
//   (3) the label fill's floor (CameraLabelFill::OnFrameStart only raises it): P8 GAP WORK must resume for the new numbering.
// Plus a 10 000-reset run of (1) and (2) with nothing in flight (deterministic). These tests FAIL on the code before the GEM-127 fix, by design. CPU only.
#include "CameraLinkTestSupport.h"
#include <VP-V/modules/motion/estimation/VectorEngineInterface.h>
#include <VP-V/modules/motion/estimation/global_model/link/CameraFitFieldPod.h>
#include <VP-V/modules/motion/estimation/global_model/link/CameraLabelFill.h>
#include <VP-V/modules/motion/estimation/global_model/link/CameraLinkCore.h>

#include <atomic>
#include <memory>
#include <string>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace VideoProcessorTest
{
	namespace
	{
		using CameraLinkTest::Rig;

		// An engine whose field era (the vectors-valid epoch) the test moves; no field, no seed, no flow (the ahead source is a stub).
		struct EraEngine : VpMotion::IVectorEngine
		{
			uint64_t era = 1;
			uint64_t LatestVerifiedFieldSeq() const override { return 0; }
			uint64_t FieldEra() const override { return era; }
			bool FieldOf(uint64_t, VpMotion::VerifiedField&) const override { return false; }
			float FieldScale() const override { return 1.0f; }
			bool SeedFieldOf(uint64_t, VpMotion::SeedField& out) const override { out = VpMotion::SeedField(); return false; }
			bool GlobalFromEngine() const override { return false; }
			bool EngineGlobal(uint64_t, VpMotion::GlobalVec&) const override { return false; }
			bool PairSpansSlip(uint64_t, uint64_t) const override { return false; }
			bool FlowIsCpuResident() const override { return false; }
			bool FlowOf(uint64_t, VpMotion::FlowView&) const override { return false; }
			void SetPreEstimateHook(VpMotion::IPreEstimateHook*) override {}
		};

		VpSkeleton::LaneLeaseSource SourceOf(Rig& r)
		{
			VpSkeleton::SlotTable* t = r.table.get();
			VpSkeleton::CpuBuffers* b = r.bytes.get();
			VpIndicator::Indicator* i = r.indicator.get();
			return [t, b, i]() { return VpSkeleton::LaneLease::Borrow(t, b, i); };
		}

		std::shared_ptr<CameraLink::FieldCopy> AheadField(uint64_t seq, double tx)
		{
			auto c = std::make_shared<CameraLink::FieldCopy>(*CameraLinkTest::PanJob(seq, tx).field);
			c->origin = CameraLink::FieldOrigin::Ahead;
			return c;
		}

		// The core in ahead mode (the ahead fit needs the worker thread; every test waits for it to be idle before it reads): the bridge rig, an on-time target the test can swap (a renumbered lane), the
		// era engine. The store knows frames 1..32.
		struct AheadCore
		{
			Rig bridge;
			std::unique_ptr<Rig> onTime{ new Rig() };
			FrameAnalysis::Store store;
			EraEngine engine;
			std::unique_ptr<CameraLink::CameraLinkCore> core;

			AheadCore()
			{
				for (uint64_t seq = 1; seq <= 32; ++seq)
				{
					FrameAnalysis::Identity id;
					id.seq = seq;
					id.width = 64;
					id.height = 36;
					store.Begin(id, FrameAnalysis::kCamera);
				}
				core.reset(new CameraLink::CameraLinkCore(store));
				CameraLink::CameraLinkSettings s;
				s.p7Wanted = true;
				s.aheadFit = true;
				core->OnSettings(s);
				core->SetEngine(&engine);
				core->SetAheadSource([](uint64_t, float, CameraLink::AheadCopy&) { return CameraLink::AheadRefusal::NoPair; },
					CameraLink::PairFactsProvider());
				Rig** cur = &current;
				current = onTime.get();
				core->AttachOnTime([cur]() { return SourceOf(**cur)(); }, "test lane");
				core->AttachGraph(bridge.table.get(), bridge.bytes.get(), bridge.indicator.get());
			}

			void Frame(uint64_t seq)
			{
				CameraLink::FramePhaseFacts f;
				f.seq = f.renderSeq = seq;
				core->OnFrameStart(f);
			}

			// The renumber: the engine's epoch moves and the on-time lane is a new one (its slots hold nothing of the old numbering).
			void Renumber()
			{
				++engine.era;
				onTime.reset(new Rig());
				current = onTime.get();
			}

			uint64_t BridgeSeals() const { return core->Counters().sealedOk.load() + core->Counters().sealedLate.load() - core->Counters().onTimeSealed.load(); }

			Rig* current = nullptr;
		};

		// A link that has a usable record for every frame (the fill reads only the record from it; the field comes from the product).
		struct AnyRecordLink : CameraLink::ICameraLink
		{
			std::shared_ptr<const CameraModel::CameraMotion> rec;
			AnyRecordLink()
			{
				auto r = std::make_shared<CameraModel::CameraMotion>();
				r->valid = true;
				r->reason = CameraModel::Reason::Ok;
				r->confidence = 0.9;
				rec = r;
			}
			std::shared_ptr<const CameraModel::CameraMotion> SealedRecord(uint64_t) const override { return rec; }
			std::shared_ptr<const CameraModel::CameraMotion> CausalRecord(uint64_t) const override { return rec; }
			bool FitEntryOf(uint64_t, std::shared_ptr<const CameraModel::CameraMotion>& out) const override { out = rec; return true; }
			bool LabelsFor(uint64_t, CameraLink::CameraConsumer, CameraLink::CameraLabelsView&) override { return false; }
			std::shared_ptr<const CameraModel::GuidanceFrame> GuideFor(uint64_t) override { return nullptr; }
		};

		struct CountingDevice : CameraLink::ILabelDevice
		{
			std::vector<uint64_t> seqs;
			uint64_t next = 0;
			bool Produce(const CameraModel::CameraMotion&, const CameraLink::FieldCopy* f, CameraLink::LabelTarget) override
			{
				seqs.push_back(next);
				return f != nullptr;
			}
			pl_tex TextureOf(CameraLink::LabelTarget) const override { return nullptr; }
		};

		// Publish P7(seq) (empty record bytes: the fill takes the record from the link) and its fit field under the same stamp.
		void PublishP7AndField(Rig& r, uint64_t seq)
		{
			r.PublishInputs(seq);
			const VpIndicator::Stamp st = r.indicator->StampOf(seq);
			const VpSkeleton::ProductId ids[] = { VpSkeleton::kCameraFitField, VpSkeleton::kP7CameraFit };
			for (VpSkeleton::ProductId id : ids)
			{
				VpSkeleton::SlotSet* s = r.table->Find(id);
				VpSkeleton::LaneTicket t;
				Assert::IsTrue(s->Claim(seq, VpAncestry::Provenance::AheadLane, ~uint64_t(0), &t) == VpSkeleton::ClaimResult::Claimed, L"premise: claimed");
				VpAncestry::Header& h = s->LaneHeader(t);
				VpSkeleton::LaneInputs pins;
				r.table->LaneAncestors(id, seq, h, pins);
				if (id == VpSkeleton::kCameraFitField)
					Assert::IsTrue(CameraLink::EncodeFitFieldPod(*CameraLinkTest::PanJob(seq, 8).field, r.fieldBytes->Ring(static_cast<size_t>(t.slot)), r.fieldBytes->Bytes()) > 0);
				h.honoured = st;
				s->Publish(t);
				pins.Unpin();
			}
		}
	}

	TEST_CLASS(CameraRenumberTests)
	{
	public:
		// (1) AHEAD FIT: the worker's render position follows the renumbered sequence, so a new-era frame ahead of the render is still on
		// time (before: the position stayed at the old high-water mark and every new-era ahead fit was taken as already sealed: bridged).
		TEST_METHOD(AfterARenumberTheAheadFitJudgesFramesAgainstTheNewRenderPosition)
		{
			AheadCore c;
			for (uint64_t s = 1; s <= 20; ++s)
				c.Frame(s);
			Assert::AreEqual(uint64_t(20), c.core->Worker()->RenderPosition(), L"premise: the render position follows the frames");
			c.Renumber();
			c.Frame(3);
			Assert::AreEqual(uint64_t(3), c.core->Worker()->RenderPosition(), L"the renumbered render position is the new frame, not the old high-water mark");
			// Frame 4 is ahead of the render (3): its ahead fit is published on time into the (new) on-time lane.
			c.onTime->PublishInputs(4);
			const uint64_t onTimeBefore = c.core->Counters().aheadOnTime.load(), bridgedBefore = c.core->Counters().aheadBridged.load();
			Assert::IsTrue(c.core->SubmitAheadField(AheadField(4, 4.0)), L"premise: the ahead field is accepted");
			c.core->Worker()->WaitIdle();
			Assert::AreEqual(onTimeBefore + 1, c.core->Counters().aheadOnTime.load(), L"the new-era frame ahead of the render is published on time");
			Assert::AreEqual(bridgedBefore, c.core->Counters().aheadBridged.load(), L"not bridged as if its seal had already run");
		}

		// (2) LAG-BRIDGE SEAL: a frame the old numbering decided on time (its retired / on-time marks) does not decide the same frame number
		// of the new numbering. From the FIRST new-era frame, the bridge seals it (counted) and the on-time seal judges nothing it did not
		// publish in this era.
		TEST_METHOD(AfterARenumberTheLagBridgeSealsAReusedFrameNumberFromTheFirstNewFrame)
		{
			AheadCore c;
			c.Frame(1);
			// Era 1: frame 2 is published on time and sealed by the on-time path (it retires the bridge's seal of frame 2).
			c.onTime->PublishInputs(1);
			c.onTime->Retire(1);
			c.onTime->PublishInputs(2);
			Assert::IsTrue(c.core->SubmitAheadField(AheadField(2, 4.0)));
			c.core->Worker()->WaitIdle();
			c.onTime->table->Resolve(2, c.onTime->indicator->StampOf(2), [](const VpSkeleton::Node&, uint64_t, int, VpAncestry::Header&, uint32_t) { return false; });
			const uint64_t bridgeBefore2 = c.BridgeSeals();
			c.Frame(2);
			Assert::AreEqual(uint64_t(1), c.core->Counters().onTimeSealed.load(), L"premise: era 1 sealed frame 2 on time");
			Assert::AreEqual(bridgeBefore2, c.BridgeSeals(), L"premise: the bridge did not seal frame 2 a second time");
			for (uint64_t s = 3; s <= 6; ++s)
				c.Frame(s);
			// The renumber: frame numbers restart; frame 2 of the new numbering has nothing published on time.
			c.Renumber();
			const uint64_t bridge = c.BridgeSeals();
			const uint64_t rejected = c.core->Counters().onTimeRejected.load();
			c.Frame(1);
			c.Frame(2);
			Assert::AreEqual(bridge + 2, c.BridgeSeals(), L"the lag bridge seals new-era frames 1 and 2 (frame 2 is not skipped as retired by era 1)");
			Assert::AreEqual(rejected, c.core->Counters().onTimeRejected.load(), L"the on-time seal judges no new-era frame by an old-era mark");
			Assert::AreEqual(uint64_t(1), c.core->Counters().onTimeSealed.load(), L"nothing new was sealed on time");
		}

		// (3) P8 GAP WORK: the label fill's floor of the old numbering does not hide the new numbering's frames (the renumbered lane is a new
		// generation of the table).
		TEST_METHOD(AfterARenumberP8GapWorkResumesForTheNewNumbering)
		{
			Rig first;
			std::unique_ptr<Rig> second;
			std::atomic<int> gen{ 1 };
			Rig* cur = &first;
			AnyRecordLink link;
			CountingDevice device;
			auto lease = [&gen, &cur]() {
				VpSkeleton::LaneLease l = VpSkeleton::LaneLease::Borrow(cur->table.get(), cur->bytes.get(), cur->indicator.get());
				l.generation = static_cast<uint64_t>(gen.load());
				return l;
			};
			CameraLink::CameraLabelFill fill(lease, link, device);
			fill.SetFieldSource([&gen, &cur]() {
				VpSkeleton::LaneLease l = VpSkeleton::LaneLease::Borrow(cur->table.get(), cur->fieldBytes.get(), cur->indicator.get());
				l.generation = static_cast<uint64_t>(gen.load());
				return l;
			});
			// Old numbering: frames up to 20 rendered; P8 is produced for frame 1 (premise: the gap work runs at all).
			PublishP7AndField(first, 1);
			Assert::IsTrue(fill.GapWork()->HasWork(), L"premise: P7 and its field published, P8 gap work exists");
			fill.GapWork()->RunChunk(1000);
			Assert::AreEqual(uint64_t(1), fill.Counters().lane.load(), L"premise: produced in the gap");
			for (uint64_t s = 1; s <= 20; ++s)
				fill.OnFrameStart(s);
			// The renumber: a new lane generation, frame numbers restart at 3.
			second.reset(new Rig());
			cur = second.get();
			gen = 2;
			fill.OnFrameStart(3);
			PublishP7AndField(*second, 3);
			Assert::IsTrue(fill.GapWork()->HasWork(), L"the new numbering's frame 3 is gap work (the old floor 20 does not hide it)");
			fill.GapWork()->RunChunk(1000);
			Assert::AreEqual(uint64_t(2), fill.Counters().lane.load(), L"P8 is produced for the renumbered frame");
			Assert::AreEqual(uint64_t(1), fill.Counters().rebound.load(), L"the new generation was taken");
		}

		// (1) + (2), 10 000 renumbers (deterministic: no job is in flight, so every read is exact): after every epoch change the render position is the new frame and the bridge seals
		// every frame once (no latch accumulates across resets).
		TEST_METHOD(TenThousandRenumbersKeepTheRenderPositionAndTheBridgeSealCurrent)
		{
			AheadCore c;
			for (uint64_t s = 1; s <= 8; ++s)
				c.Frame(s);
			const uint64_t seals0 = c.BridgeSeals();
			const int kResets = 10000;
			int positionMisses = 0;
			for (int i = 0; i < kResets; ++i)
			{
				++c.engine.era;   // the epoch moves; the on-time lane keeps its table (nothing is published on time in this run)
				const uint64_t seq = 1 + static_cast<uint64_t>(i % 5);
				c.Frame(seq);
				c.Frame(seq + 1);
				if (c.core->Worker()->RenderPosition() != seq + 1)
					++positionMisses;
			}
			Assert::AreEqual(0, positionMisses, L"every renumbered frame is the render position");
			Assert::AreEqual(seals0 + 2u * kResets, c.BridgeSeals(), L"the bridge sealed every frame of every numbering exactly once");
			Assert::AreEqual(uint64_t(0), c.core->Counters().onTimeSealed.load() + c.core->Counters().onTimeRejected.load(),
				L"no on-time verdict without an on-time publish");
		}
	};
}
