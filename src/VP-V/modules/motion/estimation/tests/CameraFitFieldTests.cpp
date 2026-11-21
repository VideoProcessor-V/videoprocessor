// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "CppUnitTest.h"

// CP-A1-D4-CAMLABEL-FIELDCOPY (0c359a5c4, 6866b8854, 1174346b9, 6dbfb9883): the P7 fit field is the declared lane product
// p7f_camera_fit_field, and the product slot IS its storage. These tests pin (1) the pod codec (link/CameraFitFieldPod.h), (2) the camera
// worker's publish of the field with P7's own stamp, BEFORE P7, and (3) every publish miss counted by cause and named in the camera-fit
// status line (audit4 MUST 1 / MUST 2, audit5). Driven synchronously (no thread). CPU only.
#include "CameraLinkTestSupport.h"
#include <VP-V/modules/motion/estimation/global_model/link/CameraFitFieldPod.h>
#include <VP-V/modules/motion/estimation/global_model/link/CameraFitStatus.h>
#include <VP-V/modules/motion/estimation/global_model/link/CameraRecordPool.h>

#include <memory>
#include <string>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace VideoProcessorTest
{
	namespace
	{
		// A field of w x h cells of `cell` px with distinct values per plane (so a plane swap or an offset is caught).
		CameraLink::FieldCopy MakeField(int w, int h, double cell, bool trust)
		{
			CameraLink::FieldCopy f;
			f.gridW = w;
			f.gridH = h;
			f.blockSize = cell;
			const size_t n = static_cast<size_t>(w) * static_cast<size_t>(h);
			f.fwdX.resize(n);
			f.fwdY.resize(n);
			for (size_t i = 0; i < n; ++i)
			{
				f.fwdX[i] = 0.25f * static_cast<float>(i % 97) - 3.0f;
				f.fwdY[i] = -0.125f * static_cast<float>(i % 89) + 1.5f;
			}
			if (trust)
			{
				f.fwdTrust.resize(n);
				for (size_t i = 0; i < n; ++i)
					f.fwdTrust[i] = static_cast<float>(i % 7) / 6.0f;
			}
			return f;
		}

		struct WorkerRig
		{
			CameraLinkTest::Rig rig;
			CameraLink::CameraRecordPool pool;
			CameraLink::FitCounters counters;
			CameraLink::CameraFitProducer producer;
			std::unique_ptr<CameraLink::CameraFitWorker> worker;

			// `withField`: the bridge target gets the field product's storage (the production wiring); false = no field source.
			explicit WorkerRig(bool withField = true, std::function<void(CameraLink::CameraFitEnv&)> edit = {})
				: pool(rig.Ring()), producer(CameraModel::Config(), &counters)
			{
				CameraLink::TimingFacts tf;
				tf.measuredSourceHz = 24.0;
				producer.SetTimingFacts(tf);
				producer.NoteTiming();
				CameraLink::CameraFitEnv env = rig.Env(&pool, &counters);
				if (!withField)
					env.fieldLeaseSource = VpSkeleton::LaneLeaseSource();
				if (edit)
					edit(env);
				worker.reset(new CameraLink::CameraFitWorker(producer, env));
			}

			VpSkeleton::SlotSet* Field() { return rig.table->Find(VpSkeleton::kCameraFitField); }
			VpSkeleton::SlotSet* P7() { return rig.table->Find(VpSkeleton::kP7CameraFit); }
			uint64_t FieldMisses() const
			{
				return counters.fieldUndersized.load() + counters.fieldEncodeFailed.load() + counters.fieldNoStorage.load()
					+ counters.fieldClaimRefused.load() + counters.fieldNoField.load();
			}
		};
	}

	TEST_CLASS(CameraFitFieldTests)
	{
	public:
		TEST_METHOD(ThePodRoundTripsWithAndWithoutTrustAndRefusesBadInputUntouched)
		{
			for (int trust = 0; trust <= 1; ++trust)
			{
				const CameraLink::FieldCopy f = MakeField(7, 5, 8.0, trust != 0);
				const size_t need = CameraLink::FitFieldPodBytes(35, trust != 0);
				Assert::AreEqual(CameraLink::kFitFieldPodHeaderBytes + 35u * 4u * (trust ? 3u : 2u), need, L"header + two or three float planes");
				std::vector<uint8_t> buf(need + 16, 0xAB);
				Assert::AreEqual(size_t(0), CameraLink::EncodeFitFieldPod(f, buf.data(), need - 1), L"one byte short: not encoded");
				Assert::AreEqual(need, CameraLink::EncodeFitFieldPod(f, buf.data(), buf.size()));
				CameraLink::FieldCopy d = MakeField(3, 3, 4.0, true);   // a reused buffer with other content
				Assert::IsTrue(CameraLink::DecodeFitFieldPod(buf.data(), need, d));
				Assert::AreEqual(7, d.gridW);
				Assert::AreEqual(5, d.gridH);
				Assert::AreEqual(8.0, d.blockSize);
				Assert::IsTrue(d.fwdX == f.fwdX && d.fwdY == f.fwdY, L"the vector planes round-trip exactly");
				Assert::IsTrue(trust ? d.fwdTrust == f.fwdTrust : d.fwdTrust.empty(), L"trust round-trips; a pod without trust clears it");
				// Refusals leave the target untouched: short buffer, bad magic, a grid the bytes cannot hold.
				const CameraLink::FieldCopy before = d;
				Assert::IsFalse(CameraLink::DecodeFitFieldPod(buf.data(), need - 1, d), L"short buffer");
				std::vector<uint8_t> bad = buf;
				bad[0] ^= 0xFF;
				Assert::IsFalse(CameraLink::DecodeFitFieldPod(bad.data(), need, d), L"bad magic");
				Assert::IsFalse(CameraLink::DecodeFitFieldPod(nullptr, need, d), L"no buffer");
				Assert::IsFalse(CameraLink::DecodeFitFieldPod(buf.data(), CameraLink::kFitFieldPodHeaderBytes - 1, d), L"shorter than the header");
				Assert::IsTrue(d.gridW == before.gridW && d.gridH == before.gridH && d.fwdX == before.fwdX && d.fwdTrust == before.fwdTrust,
					L"a refused decode leaves the target untouched");
			}
			// An invalid field (mismatched planes) is never encoded.
			CameraLink::FieldCopy broken = MakeField(4, 4, 8.0, true);
			broken.fwdY.pop_back();
			std::vector<uint8_t> buf(4096);
			Assert::AreEqual(size_t(0), CameraLink::EncodeFitFieldPod(broken, buf.data(), buf.size()));
		}

		TEST_METHOD(TheWorkerPublishesTheFieldUnderP7sStampAndItDecodesToTheFittedField)
		{
			WorkerRig w;
			Assert::IsNotNull(w.Field(), L"premise: the camera graph holds P8's declared fit field");
			w.rig.PublishInputs(1);
			const CameraLink::CameraFitJob job = CameraLinkTest::PanJob(1, 10.0);
			Assert::IsTrue(w.worker->ProcessJob(job) == CameraLink::PublishOutcome::Published);
			Assert::IsTrue(w.Field()->StateOf(1) == VpSkeleton::SlotState::Published, L"the field product is published with P7");
			Assert::IsTrue(w.P7()->StateOf(1) == VpSkeleton::SlotState::Published);
			const VpSkeleton::ReadInfo a = w.P7()->Read(1, true);
			const VpSkeleton::ReadInfo b = w.Field()->Read(1, true);
			Assert::IsTrue(a.header != nullptr && b.header != nullptr);
			Assert::IsTrue(VpIndicator::SameFor(a.header->honoured, b.header->honoured, ~VpIndicator::CounterMask(0)), L"the field carries P7's own stamp");
			CameraLink::FieldCopy d;
			Assert::IsTrue(CameraLink::DecodeFitFieldPod(w.rig.fieldBytes->Ring(static_cast<size_t>(b.index)), w.rig.fieldBytes->Bytes(), d));
			Assert::IsTrue(d.gridW == job.field->gridW && d.gridH == job.field->gridH && d.blockSize == job.field->blockSize);
			Assert::IsTrue(d.fwdX == job.field->fwdX && d.fwdY == job.field->fwdY && d.fwdTrust == job.field->fwdTrust,
				L"the slot holds exactly the field the record was fitted from");
			Assert::AreEqual(uint64_t(0), w.FieldMisses(), L"no miss counted");
			// The pool keeps provenance only (no field copy).
			CameraLink::PoolEntry e;
			Assert::IsTrue(w.pool.Get(1, e) && e.field.have);
		}

		TEST_METHOD(ARealTenEightyAheadFieldFitsItsDeclaredProduct)
		{
			// audit4 MUST 1: the ahead field is the engine's native cell grid of the rig's source size, cells of 8 px (240 x 135 at 1080).
			WorkerRig w;
			w.rig.PublishInputs(1);
			const VpSkeleton::Dims dims = CameraLinkTest::Settings().dims;
			const int cw = static_cast<int>((dims.width + 7) / 8), ch = static_cast<int>((dims.height + 7) / 8);
			CameraLink::CameraFitJob job = CameraLinkTest::PanJob(1, 10.0);
			auto f = std::make_shared<CameraLink::FieldCopy>(MakeField(cw, ch, 8.0, true));
			f->seq = 1;
			f->pairFrom = 0;
			f->pairTo = 1;
			f->stamp = job.field->stamp;
			job.field = f;
			Assert::IsTrue(w.rig.fieldBytes->Bytes() >= CameraLink::FitFieldPodBytes(static_cast<size_t>(cw) * static_cast<size_t>(ch), true),
				L"premise: the declared slot holds it");
			w.worker->ProcessJob(job);
			Assert::IsTrue(w.Field()->StateOf(1) == VpSkeleton::SlotState::Published, L"the 1080 native-grid field is published");
			Assert::AreEqual(uint64_t(0), w.counters.fieldUndersized.load());
		}

		TEST_METHOD(EveryFieldPublishMissIsCountedByCauseAndNamed)
		{
			// (a) no storage: no field source on the bridge target.
			{
				WorkerRig w(false);
				w.rig.PublishInputs(1);
				w.worker->ProcessJob(CameraLinkTest::PanJob(1, 10.0));
				Assert::IsFalse(w.Field()->StateOf(1) == VpSkeleton::SlotState::Published, L"no storage: not published");
				Assert::AreEqual(uint64_t(1), w.counters.fieldNoStorage.load());
				Assert::AreEqual(uint64_t(1), w.FieldMisses(), L"exactly one cause");
				Assert::IsTrue(CameraLink::StatusLine(w.counters).find("1 fit field without storage") != std::string::npos);
			}
			// (b) another generation than P7's lease: counted as no storage, not published.
			{
				WorkerRig w(true, [](CameraLink::CameraFitEnv& env) {
					const VpSkeleton::LaneLeaseSource inner = env.fieldLeaseSource;
					env.fieldLeaseSource = [inner]() { VpSkeleton::LaneLease l = inner(); l.generation = 7; return l; };
				});
				w.rig.PublishInputs(1);
				w.worker->ProcessJob(CameraLinkTest::PanJob(1, 10.0));
				Assert::IsFalse(w.Field()->StateOf(1) == VpSkeleton::SlotState::Published);
				Assert::AreEqual(uint64_t(1), w.counters.fieldNoStorage.load());
				Assert::AreEqual(uint64_t(1), w.FieldMisses());
			}
			// (c) undersized: a field larger than its product slot; need and slot bytes of the last miss are named.
			{
				WorkerRig w;
				w.rig.PublishInputs(1);
				CameraLink::CameraFitJob job = CameraLinkTest::PanJob(1, 10.0);
				const size_t slot = w.rig.fieldBytes->Bytes();
				const size_t perCell = CameraLink::FitFieldPodBytes(1, true) - CameraLink::kFitFieldPodHeaderBytes;   // bytes per cell with trust
				const int cells = static_cast<int>(slot / perCell) + 64;   // more cells than the slot can hold
				auto f = std::make_shared<CameraLink::FieldCopy>(MakeField(cells, 1, 8.0, true));
				f->seq = 1;
				f->stamp = job.field->stamp;
				job.field = f;
				w.worker->ProcessJob(job);
				Assert::IsFalse(w.Field()->StateOf(1) == VpSkeleton::SlotState::Published, L"too big: not published");
				Assert::AreEqual(uint64_t(1), w.counters.fieldUndersized.load());
				Assert::AreEqual(uint64_t(1), w.FieldMisses());
				Assert::AreEqual(uint64_t(CameraLink::FitFieldPodBytes(static_cast<size_t>(cells), true)), w.counters.fieldNeedBytes.load());
				Assert::AreEqual(uint64_t(slot), w.counters.fieldSlotBytes.load());
				const std::string line = CameraLink::StatusLine(w.counters);
				Assert::IsTrue(line.find("1 fit field too big for its product") != std::string::npos);
				Assert::IsTrue(line.find("slot " + std::to_string(slot) + " B") != std::string::npos, L"the slot size is in the line");
			}
			// (d) encode failed: an invalid field (no block size) although the slot is large enough; P7 still publishes its (unfitted) record.
			{
				WorkerRig w;
				w.rig.PublishInputs(1);
				CameraLink::CameraFitJob job = CameraLinkTest::PanJob(1, 10.0);
				auto f = std::make_shared<CameraLink::FieldCopy>(*job.field);
				f->blockSize = 0;
				job.field = f;
				w.worker->ProcessJob(job);
				Assert::IsFalse(w.Field()->StateOf(1) == VpSkeleton::SlotState::Published);
				Assert::AreEqual(uint64_t(1), w.counters.fieldEncodeFailed.load());
				Assert::AreEqual(uint64_t(1), w.FieldMisses(), L"counted once, by its cause");
				Assert::AreEqual(uint64_t(0), w.counters.fieldUndersized.load(), L"not called undersized");
				Assert::IsTrue(CameraLink::StatusLine(w.counters).find("1 fit field encode failed") != std::string::npos);
			}
			// (f) no field: a job without a field (the fit has nothing to store).
			{
				WorkerRig w;
				w.rig.PublishInputs(1);
				CameraLink::CameraFitJob job = CameraLinkTest::PanJob(1, 10.0);
				job.field.reset();
				w.worker->ProcessJob(job);
				Assert::IsFalse(w.Field()->StateOf(1) == VpSkeleton::SlotState::Published);
				Assert::AreEqual(uint64_t(1), w.counters.fieldNoField.load());
				Assert::AreEqual(uint64_t(1), w.FieldMisses());
				Assert::IsTrue(CameraLink::StatusLine(w.counters).find("1 fits without a field") != std::string::npos);
			}
			// (e) claim refused: the field slot of the frame is already taken.
			{
				WorkerRig w;
				w.rig.PublishInputs(1);
				VpSkeleton::LaneTicket t;
				Assert::IsTrue(w.Field()->Claim(1, VpAncestry::Provenance::AheadLane, ~uint64_t(0), &t) == VpSkeleton::ClaimResult::Claimed);
				w.worker->ProcessJob(CameraLinkTest::PanJob(1, 10.0));
				Assert::AreEqual(uint64_t(1), w.counters.fieldClaimRefused.load());
				Assert::AreEqual(uint64_t(1), w.FieldMisses());
				w.Field()->Abandon(t);
			}
			// The line names every cause, all zero on a clean worker.
			CameraLink::FitCounters clean;
			const std::string line = CameraLink::StatusLine(clean);
			for (const char* part : { "0 fit field too big for its product", "0 fit field encode failed", "0 fit field without storage",
				"0 fit field claim refused", "0 fits without a field" })
				Assert::IsTrue(line.find(part) != std::string::npos, L"each cause is named in the camera-fit line");
		}
	};
}
