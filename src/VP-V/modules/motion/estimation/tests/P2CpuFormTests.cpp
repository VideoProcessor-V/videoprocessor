// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// B-prereq (a) A3: the P2 CPU form over the REAL VectorEngine's P2 slot (VectorEngine::P2RecordEndingAt + the owner fill of
// P2CpuForm.h through a SkeletonMemo). CPU only: a never-started producer, pairs published straight into the slot (the FlowOfTests
// rig). Pins: (1) a hit carries the published Flow's global / second / flags and the pair's from / to / generation exactly;
// (2) a miss declines with the frame and the slot generation; (3) a store re-key (the producer's SetGeneration) leaves no pair;
// (3') C2a-F 2: a pair stored under slot generation G while the stream generation moved to H (not re-keyed yet) is declined
// "pair from generation G (current H)", and served again after the re-key; (4) a lane slot that holds P2 but no CPU bytes
// (Absent) falls through to the owner fill; (7) FindTo takes the newest `from`; (10) the engine counts served / declined and
// an absent engine counts nothing.
#include "pch.h"
#include "CppUnitTest.h"
#include <VP-V/modules/motion/estimation/MotionAheadProducer.h>
#include <VP-V/modules/motion/estimation/VectorEngine.h>
#include <VP-V/modules/motion/estimation/P2CpuForm.h>
#include <VP-V/modules/motion/estimation/P2PairRecord.h>
#include <VP-V/modules/control/diagnostics/DiagnosticHistory.h>

#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace VideoProcessorTest
{
	namespace
	{
		// A 16 x 8 engine-size pair with a distinct global / second per `tag` (D1 format: x, y, ambiguity, w).
		MotionAhead::FlowPtr MakeFlow(float tag, bool globalSet, bool globalGeneral)
		{
			auto flow = std::make_shared<MotionAhead::Flow>();
			flow->halfW = 16;
			flow->halfH = 8;
			flow->gridW = 4;
			flow->gridH = 2;
			for (int i = 0; i < 8; ++i)
			{
				flow->forward.push_back(static_cast<int16_t>(64 + 32 * i));
				flow->forward.push_back(static_cast<int16_t>(-32));
			}
			for (int d = 0; d < 2; ++d)
				for (int k = 0; k < 4; ++k)
				{
					flow->global[d][k] = tag + 0.25f * float(d * 4 + k);
					flow->second[d][k] = -tag - 0.125f * float(d * 4 + k);
				}
			flow->globalSet = globalSet;
			flow->globalGeneral = globalGeneral;
			return flow;
		}

		// An engine with a (never started) producer, the slot keyed with the initial stream generation (1), the pair (10, 11) in it.
		struct Rig
		{
			VpMotion::VectorEngine engine;
			MotionAhead::FlowPtr flow = MakeFlow(3.0f, true, false);
			Rig()
			{
				engine.PublishProducer(std::make_shared<MotionAhead::Producer>());
				engine.p2Slot->SetGeneration(engine.motionValidity.StreamGeneration());
				engine.p2Slot->Publish(10, 11, engine.p2Slot->Generation(), flow);
			}
			VpMotion::P2EngineOf EngineOf() { VpMotion::IVectorEngine* e = &engine; return [e](const void*) { return e; }; }
		};

		bool SameGlobals(const VpMotion::P2PairRecord& rec, const MotionAhead::Flow& f)
		{
			return std::memcmp(rec.global, f.global, sizeof(rec.global)) == 0 && std::memcmp(rec.second, f.second, sizeof(rec.second)) == 0;
		}

		bool Has(const std::string& text, const char* part) { return text.find(part) != std::string::npos; }

		bool RegisterFill(VpSkeleton::SkeletonMemo& m, VpMotion::P2EngineOf engineOf)
		{
			return m.RegisterOwnerFill(VpSkeleton::kP2VectorPair, VpSkeleton::AcquireForm::Cpu, VpMotion::kP2CpuFormUnit,
				VpMotion::MakeP2CpuOwnerFill(engineOf)) == VpSkeleton::SkeletonMemo::Registration::Registered;
		}

		// A host that answers nothing (UpdateVectorEngine needs one; the record under test goes to DiagnosticHistory).
		struct QuietHost : VpMotion::IVectorEngineHost
		{
			void SyncFallback(const ::FallbackCatalog::Decision&) override {}
			void PublishStatus(bool) override {}
			bool DitherVectorsSupported() const override { return false; }
			void NoteSourceMonitorMotion(double, uint64_t) override {}
			bool IntentActive() const override { return false; }
			void NoteIntentVectors(const ::IntentInterpreter::VectorSample&) override {}
		};

		// The "vector-p2-cpu-form" decision records written since ring index `from` (this module's recorder: VectorEngine.cpp is
		// compiled into the test binary).
		std::vector<DiagnosticHistory::EventRecord> P2FormRecordsSince(uint64_t from)
		{
			std::vector<DiagnosticHistory::EventRecord> out;
			for (const auto& e : DiagnosticHistory::Global().decisions.Snapshot())
				if (e.index >= from && std::strcmp(e.record.name, "vector-p2-cpu-form") == 0)
					out.push_back(e.record);
			return out;
		}

		uint64_t DecisionsWritten()
		{
			DiagnosticHistory::RingCounters c;
			DiagnosticHistory::Global().decisions.Snapshot(&c, 1);
			return c.written;
		}
	}

	TEST_CLASS(P2CpuFormTests)
	{
	public:
		TEST_METHOD(AHitIsThePublishedGlobalAndThePairIdentity)
		{
			using namespace VpSkeleton;
			Rig rig;
			Assert::AreEqual(uint64_t(1), rig.engine.motionValidity.StreamGeneration(), L"the initial stream generation");
			VpMotion::P2PairRecord direct;
			std::string why;
			Assert::IsTrue(rig.engine.P2RecordEndingAt(11, direct, why), L"the engine's reader finds the pair ending at 11");
			Assert::IsTrue(why.empty(), L"no reason on a hit");

			SkeletonMemo m;
			Assert::IsTrue(RegisterFill(m, rig.EngineOf()));
			const AcquireResult r = m.Acquire(kP2VectorPair, 11, AcquireForm::Cpu);
			Assert::IsTrue(r.status == AcquireStatus::FilledAtRead, L"filled at read by the owner");
			Assert::IsTrue(r.source == AcquireSource::Memo);
			VpMotion::P2PairRecord rec;
			Assert::IsTrue(VpMotion::DecodeP2PairRecord(r.cpu, r.cpuBytes, rec), L"the ONE codec");
			Assert::AreEqual(uint64_t(10), rec.from);
			Assert::AreEqual(uint64_t(11), rec.to);
			Assert::AreEqual(uint64_t(1), rec.generation, L"the generation the pair was stored under");
			Assert::IsTrue(SameGlobals(rec, *rig.flow), L"global[2][4] and second[2][4] bit for bit (fwd [0], bwd [1])");
			Assert::IsTrue(rec.GlobalSet(), L"globalSet carried");
			Assert::IsFalse(rec.GlobalGeneral(), L"globalGeneral carried (false here)");
			Assert::IsTrue(SameGlobals(direct, *rig.flow) && direct.flags == rec.flags, L"the fill serves exactly the engine's record");
			Assert::AreEqual(1ull, rig.engine.P2CpuFormServed());
			Assert::AreEqual(0ull, rig.engine.P2CpuFormDeclined());
		}

		TEST_METHOD(AMissDeclinesWithTheFrameAndTheSlotGeneration)
		{
			using namespace VpSkeleton;
			Rig rig;
			SkeletonMemo m;
			Assert::IsTrue(RegisterFill(m, rig.EngineOf()));
			const AcquireResult r = m.Acquire(kP2VectorPair, 12, AcquireForm::Cpu);
			Assert::IsTrue(r.status == AcquireStatus::Unavailable);
			Assert::IsTrue(r.cause == AcquireCause::OwnerDeclined, L"an owner decline (visible cause), never NoOwnerFill");
			Assert::IsTrue(Has(m.LastCause(kP2VectorPair), "(p2 cpu form: no pair ahead for frame 12 (generation 1))"),
				L"the reason rides the cause");
			Assert::AreEqual(0ull, rig.engine.P2CpuFormServed());
			Assert::AreEqual(1ull, rig.engine.P2CpuFormDeclined());
		}

		TEST_METHOD(AStoreReKeyLeavesNoPair)
		{
			Rig rig;
			rig.engine.p2Slot->SetGeneration(2);   // the producer's re-key: the store is cleared
			VpMotion::P2PairRecord rec;
			std::string why;
			Assert::IsFalse(rig.engine.P2RecordEndingAt(11, rec, why));
			Assert::AreEqual(std::string("p2 cpu form: no pair ahead for frame 11 (generation 2)"), why);
		}

		TEST_METHOD(APairFromAnOlderStreamGenerationIsDeclinedUntilReKeyed)
		{
			using namespace VpSkeleton;
			Rig rig;
			rig.engine.motionValidity.Invalidate(::MotionValidity::Cause::Resync);   // stream generation 1 -> 2; slot still keyed 1
			Assert::AreEqual(uint64_t(2), rig.engine.motionValidity.StreamGeneration());
			Assert::AreEqual(uint64_t(1), rig.engine.p2Slot->Generation(), L"the asynchronous re-key has not happened");
			uint64_t from = 0, generation = 0;
			Assert::IsTrue(static_cast<bool>(rig.engine.P2().FindTo(11, from, generation)), L"the old pair is still in the slot");

			VpMotion::P2PairRecord rec;
			std::string why;
			Assert::IsFalse(rig.engine.P2RecordEndingAt(11, rec, why), L"C2a-F 2: never served");
			Assert::AreEqual(std::string("p2 cpu form: pair from generation 1 (current 2)"), why, L"G vs H named");

			SkeletonMemo m;
			Assert::IsTrue(RegisterFill(m, rig.EngineOf()));
			Assert::IsTrue(m.Acquire(kP2VectorPair, 11, AcquireForm::Cpu).cause == AcquireCause::OwnerDeclined);
			Assert::IsTrue(Has(m.LastCause(kP2VectorPair), "(p2 cpu form: pair from generation 1 (current 2))"), L"visible cause");
			Assert::AreEqual(1ull, rig.engine.P2CpuFormDeclined());

			// The producer re-keys and publishes the new era's pair: served again, with the new generation.
			const MotionAhead::FlowPtr fresh = MakeFlow(8.0f, true, true);
			rig.engine.p2Slot->SetGeneration(2);
			Assert::IsTrue(rig.engine.p2Slot->Publish(10, 11, 2, fresh));
			Assert::IsTrue(rig.engine.P2RecordEndingAt(11, rec, why), L"current again");
			Assert::AreEqual(uint64_t(2), rec.generation);
			Assert::IsTrue(SameGlobals(rec, *fresh), L"the new era's global, never the old one");
		}

		TEST_METHOD(FindToTakesTheNewestFrom)
		{
			Rig rig;
			const MotionAhead::FlowPtr older = MakeFlow(5.0f, true, true);
			const MotionAhead::FlowPtr newer = MakeFlow(7.0f, false, true);
			const uint64_t g = rig.engine.p2Slot->Generation();
			Assert::IsTrue(rig.engine.p2Slot->Publish(8, 12, g, older));
			Assert::IsTrue(rig.engine.p2Slot->Publish(11, 12, g, newer));
			Assert::IsTrue(rig.engine.p2Slot->Publish(9, 12, g, older));
			uint64_t from = 0, generation = 0;
			const MotionAhead::FlowPtr found = rig.engine.P2().FindTo(12, from, generation);
			Assert::IsTrue(found == newer, L"the pair with the largest from");
			Assert::AreEqual(uint64_t(11), from);
			Assert::AreEqual(g, generation);
			VpMotion::P2PairRecord rec;
			std::string why;
			Assert::IsTrue(rig.engine.P2RecordEndingAt(12, rec, why));
			Assert::AreEqual(uint64_t(11), rec.from, L"the record names the pair it came from");
			Assert::IsTrue(SameGlobals(rec, *newer));
			Assert::IsFalse(rec.GlobalSet(), L"flags of THAT pair");

			from = 99;
			generation = 99;
			Assert::IsFalse(static_cast<bool>(rig.engine.P2().FindTo(13, from, generation)), L"nothing ends at 13");
			Assert::AreEqual(uint64_t(0), from, L"a miss zeroes from");
			Assert::AreEqual(uint64_t(0), generation, L"and the generation");
			Assert::AreEqual(uint64_t(0), VpMotion::P2View().Generation(), L"an unbound view: generation 0");
			Assert::IsFalse(static_cast<bool>(VpMotion::P2View().FindTo(11, from, generation)), L"an unbound view finds nothing");
		}

		TEST_METHOD(ALaneSlotWithoutCpuBytesFallsToTheOwnerFill)
		{
			using namespace VpSkeleton;
			Rig rig;
			int host = 0;
			SkeletonMemo m;
			m.SetHost(&host);
			m.SetRenderThread(std::this_thread::get_id());
			// The lane graph holds P2 (an Ofa product) but its slot has no CPU bytes: BindLaneToMemo answers Absent for the CPU form
			// (SkeletonAcquireLane.h:56-58, "a GPU product asked in CPU form: the owner publishes that form").
			int laneReads = 0;
			LaneSlotSource src;
			src.read = [&laneReads](ProductId id, uint64_t, AcquireForm form, LaneSlotSource::Read*) {
				if (id != kP2VectorPair)
					return LaneSlotSource::State::NotLaneProduct;
				++laneReads;
				return form == AcquireForm::Cpu ? LaneSlotSource::State::Absent : LaneSlotSource::State::Present;
			};
			m.SetLaneSource(src);
			Assert::IsTrue(RegisterFill(m, rig.EngineOf()));
			const AcquireResult r = m.Acquire(kP2VectorPair, 11, AcquireForm::Cpu);
			Assert::AreEqual(1, laneReads, L"the lane was asked first");
			Assert::IsTrue(r.status == AcquireStatus::FilledAtRead, L"the owner fill served it");
			Assert::IsTrue(r.source == AcquireSource::Memo, L"from the memo, not the lane slot");
			VpMotion::P2PairRecord rec;
			Assert::IsTrue(VpMotion::DecodeP2PairRecord(r.cpu, r.cpuBytes, rec));
			Assert::IsTrue(SameGlobals(rec, *rig.flow));
			Assert::AreEqual(0ull, m.CountersOf(kP2VectorPair).reusedLane, L"no lane reuse counted");
		}

		TEST_METHOD(AnAbsentEngineCountsNothingOnTheEngine)
		{
			using namespace VpSkeleton;
			Rig rig;
			SkeletonMemo m;
			Assert::IsTrue(RegisterFill(m, [](const void*) -> VpMotion::IVectorEngine* { return nullptr; }));
			Assert::IsTrue(m.Acquire(kP2VectorPair, 11, AcquireForm::Cpu).cause == AcquireCause::OwnerDeclined);
			Assert::IsTrue(Has(m.LastCause(kP2VectorPair), "(p2 cpu form: engine absent)"));
			Assert::AreEqual(0ull, rig.engine.P2CpuFormServed(), L"the (other) engine is not counted");
			Assert::AreEqual(0ull, rig.engine.P2CpuFormDeclined());
		}

		// BBP-A-COUNTS-CTRLALTD: the form's declines reach the status segment (Config status / Ctrl+Alt+D), never log-only:
		// served + declined + the last decline's words; nothing shown while every read was served; shown without a producer too.
		TEST_METHOD(ProducerStatusSegmentNamesTheP2CpuFormDeclines)
		{
			using namespace VpSkeleton;
			Rig rig;
			SkeletonMemo m;
			Assert::IsTrue(RegisterFill(m, rig.EngineOf()));
			Assert::IsTrue(m.Acquire(kP2VectorPair, 11, AcquireForm::Cpu).status == AcquireStatus::FilledAtRead);
			Assert::AreEqual(std::string(), rig.engine.ProducerStatusSegment(), L"served only: nothing shown");
			Assert::IsTrue(m.Acquire(kP2VectorPair, 12, AcquireForm::Cpu).cause == AcquireCause::OwnerDeclined);
			Assert::AreEqual(std::string("P2 CPU form served 1, declined 1 (last: p2 cpu form: no pair ahead for frame 12 (generation 1))"),
				rig.engine.ProducerStatusSegment(), L"the decline is counted and named");
			Assert::AreEqual(rig.engine.ProducerStatusSegment(), rig.engine.P2CpuFormStatusSegment(), L"no ring miss: the form's segment alone");

			VpMotion::VectorEngine bare;   // no producer: the form's segment is not hidden behind the ring-miss early return
			const VpMotion::IVectorEngine& asInterface = bare;
			asInterface.NoteP2CpuFormRead(false, "p2 cpu form: no pair store");
			Assert::AreEqual(std::string("P2 CPU form served 0, declined 1 (last: p2 cpu form: no pair store)"), bare.ProducerStatusSegment());
			asInterface.NoteP2CpuFormRead(false);   // the one-argument form counts and keeps the last words
			Assert::AreEqual(std::string("P2 CPU form served 0, declined 2 (last: p2 cpu form: no pair store)"), bare.ProducerStatusSegment());
		}

		// BBP-A-COUNTS-CTRLALTD (gate SHOULD-3): the 5-s block's DiagnosticHistory record "vector-p2-cpu-form" is written once at the
		// first read and then only when the declines rose: served-only reads never add one, an unchanged count never adds one. The
		// name is stored whole (18 chars < EventRecord::name[24]); a/b are the served / declined counts of the same snapshot as the
		// text. vectorStatsTick = 0 opens the 5-s block on the next update (the block's own first-run rule), so no sleep is needed.
		TEST_METHOD(TheP2CpuFormHistoryRecordIsWrittenAtTheFirstReadAndOnARiseInDeclinesOnly)
		{
			Assert::IsTrue(std::strlen("vector-p2-cpu-form") < sizeof(DiagnosticHistory::EventRecord().name), L"the name fits char[24]");
			VpMotion::VectorEngine engine;
			const VpMotion::IVectorEngine& reads = engine;
			QuietHost host;
			VpMotion::VectorEngine::UpdateFacts facts;
			facts.vectorDemandSetting = true;
			VectorDemand::Input in;
			in.enabled = true;
			in.slotsRun = true;
			in.queueAheadCapacity = 4;
			in.wants[static_cast<int>(VectorDemand::Consumer::GrainMap)] = true;
			auto update = [&]() { engine.vectorStatsTick = 0; engine.UpdateVectorEngine(in, facts, host); };
			const uint64_t from = DecisionsWritten();

			update();
			Assert::IsTrue(engine.vectorDemand.mode != VectorDemand::Mode::Off, L"the 5-s block runs only while the engine is on");
			Assert::AreEqual<size_t>(0, P2FormRecordsSince(from).size(), L"no read yet: no record");

			reads.NoteP2CpuFormRead(true);
			update();
			auto recs = P2FormRecordsSince(from);
			Assert::AreEqual<size_t>(1, recs.size(), L"the first read is recorded");
			Assert::AreEqual(std::string("vector-p2-cpu-form"), std::string(recs[0].name), L"the name whole, not cut");
			Assert::AreEqual(std::string("P2 CPU form served 1, declined 0"), std::string(recs[0].detail));
			Assert::AreEqual(int64_t(1), recs[0].a, L"a = served");
			Assert::AreEqual(int64_t(0), recs[0].b, L"b = declined");

			reads.NoteP2CpuFormRead(true);
			update();
			update();
			Assert::AreEqual<size_t>(1, P2FormRecordsSince(from).size(), L"served-only reads and unchanged counts add no record");

			reads.NoteP2CpuFormRead(false, "p2 cpu form: no pair store");
			update();
			recs = P2FormRecordsSince(from);
			Assert::AreEqual<size_t>(2, recs.size(), L"a rise in declines is recorded");
			const std::string full = "P2 CPU form served 2, declined 1 (last: p2 cpu form: no pair store)";
			Assert::AreEqual(full.substr(0, sizeof(recs[1].detail) - 1), std::string(recs[1].detail), L"the status wording, cut to detail[56]");
			Assert::AreEqual(int64_t(2), recs[1].a);
			Assert::AreEqual(int64_t(1), recs[1].b);

			update();
			reads.NoteP2CpuFormRead(true);
			update();
			Assert::AreEqual<size_t>(2, P2FormRecordsSince(from).size(), L"no further decline: no further record");
		}
	};
}
