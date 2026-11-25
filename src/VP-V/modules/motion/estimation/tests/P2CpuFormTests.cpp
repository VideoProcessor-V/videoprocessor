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
#include <VP-V/modules/motion/estimation/global_model/PairGlobalFill.h>
#include <VP-V/core/FallbackPlainText.h>

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

		// The process-wide confirmed-cut seam for one scope, restored after (the seam is shared by every test).
		struct CutSeamScope
		{
			::MotionValidity::State::ConfirmedCutFn before;
			explicit CutSeamScope(::MotionValidity::State::ConfirmedCutFn fn)
				: before(::MotionValidity::State::ConfirmedCutReader().load())
			{
				::MotionValidity::State::ConfirmedCutReader().store(fn);
			}
			~CutSeamScope() { ::MotionValidity::State::ConfirmedCutReader().store(before); }
			CutSeamScope(const CutSeamScope&) = delete;
			CutSeamScope& operator=(const CutSeamScope&) = delete;
		};
		bool CutAtEleven(uint64_t seq) { return seq == 11; }
		bool CutAtTen(uint64_t seq) { return seq == 10; }
		bool CutNowhere(uint64_t) { return false; }

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

		// RAII join guard so off-render-thread tests always join cleanly even on assertion failure (Rule 2).
		struct PairGlobalThreadJoinGuard
		{
			std::thread thread;
			~PairGlobalThreadJoinGuard()
			{
				if (thread.joinable())
					thread.join();
			}
			PairGlobalThreadJoinGuard() = default;
			PairGlobalThreadJoinGuard(const PairGlobalThreadJoinGuard&) = delete;
			PairGlobalThreadJoinGuard& operator=(const PairGlobalThreadJoinGuard&) = delete;
		};

		// Local IExternalDemand double forwarding RegisterOwnerFill to a standalone SkeletonMemo (zero global hub mutation).
		struct FakePairGlobalDemand : Vpv::Skeleton::IExternalDemand
		{
			VpSkeleton::SkeletonMemo* memo = nullptr;
			int registerCalls = 0;

			explicit FakePairGlobalDemand(VpSkeleton::SkeletonMemo* target) : memo(target) {}
			~FakePairGlobalDemand() {}

			void SetExternalDemand(const void*, const void*, const std::vector<VpSkeleton::ConsumerDecl>&,
				const std::vector<VpSkeleton::ProductId>&) override {}
			void ClearExternalDemand(const void*, const void*) override {}
			VpSkeleton::SkeletonMemo::Registration RegisterOwnerFill(VpSkeleton::ProductId id, VpSkeleton::AcquireForm form,
				const std::string& unit, VpSkeleton::OwnerFill fill) override
			{
				++registerCalls;
				if (memo == nullptr)
					return VpSkeleton::SkeletonMemo::Registration::NotInCatalog;
				return memo->RegisterOwnerFill(id, form, unit, std::move(fill));
			}
		};

		VpMotion::PairGlobal MakePairGlobalSample(VpMotion::PairGlobalSource src, bool valid, uint64_t epoch, float base)
		{
			VpMotion::PairGlobal r;
			r.src = src;
			r.valid = valid;
			r.p2Epoch = epoch;
			for (int d = 0; d < 2; ++d)
				for (int k = 0; k < 4; ++k)
				{
					r.vec[d][k] = base + 0.375f * float(d * 4 + k) * (d == 0 ? 1.0f : -1.0f);
					r.second[d][k] = -base - 0.625f * float(d * 4 + k) + 0.125f;
				}
			return r;
		}

		bool SamePairGlobal(const VpMotion::PairGlobal& a, const VpMotion::PairGlobal& b)
		{
			return a.src == b.src
				&& a.p2Epoch == b.p2Epoch
				&& a.valid == b.valid
				&& std::memcmp(a.vec, b.vec, sizeof(a.vec)) == 0
				&& std::memcmp(a.second, b.second, sizeof(a.second)) == 0;
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

		// GF-P4 S7 (a): the directory's ReadP2Record runs the whole read and its count under the directory lock for a REAL engine
		// registered for its host; an unknown host answers "engine absent" and counts nothing.
		TEST_METHOD(DirectoryReadRunsUnderTheLockAndCounts)
		{
			int hostTag = 0, otherHost = 0;
			VpMotion::VectorEngine engine(&hostTag);   // registered in the directory for &hostTag
			engine.PublishProducer(std::make_shared<MotionAhead::Producer>());
			engine.p2Slot->SetGeneration(engine.motionValidity.StreamGeneration());
			engine.p2Slot->Publish(10, 11, engine.p2Slot->Generation(), MakeFlow(3.0f, true, false));

			VpMotion::P2PairRecord rec;
			std::string why;
			Assert::IsTrue(VpMotion::VectorEngine::Directory().ReadP2Record(&hostTag, 11, rec, why), L"the registered engine's pair is read");
			Assert::AreEqual(uint64_t(10), rec.from);
			Assert::AreEqual(uint64_t(11), rec.to);
			Assert::AreEqual(1ull, engine.P2CpuFormServed(), L"the read is counted served, once");
			Assert::AreEqual(0ull, engine.P2CpuFormDeclined());

			VpMotion::P2PairRecord none;
			std::string noneWhy;
			Assert::IsFalse(VpMotion::VectorEngine::Directory().ReadP2Record(&otherHost, 11, none, noneWhy), L"no engine for another host");
			Assert::AreEqual(std::string("p2 cpu form: engine absent"), noneWhy);
			Assert::AreEqual(1ull, engine.P2CpuFormServed(), L"an unknown host changes no count");
			Assert::AreEqual(0ull, engine.P2CpuFormDeclined());
		}

		// GF-P4 S7 (b): an ADJACENT pair whose end is a confirmed cut is declined through the one stamp form (PairValidAt), counted
		// once there, and the reason says so.
		TEST_METHOD(AdjacentPairAcrossAConfirmedCutIsDeclined)
		{
			Rig rig;   // the pair 10 -> 11
			const CutSeamScope scope(&CutAtEleven);
			VpMotion::P2PairRecord rec;
			std::string why;
			Assert::AreEqual(0ull, static_cast<unsigned long long>(rig.engine.motionValidity.ConfirmedCutRefusals()));
			Assert::IsFalse(rig.engine.P2RecordEndingAt(11, rec, why), L"the pair ending on a confirmed cut is declined");
			Assert::IsTrue(Has(why, "refused (stamps or confirmed cut)"), L"the reason names the refusal");
			Assert::AreEqual(1ull, static_cast<unsigned long long>(rig.engine.motionValidity.ConfirmedCutRefusals()), L"counted once, in PairValidAt");
		}

		// GF-P4 S7 (b): a GAP pair (from + 1 < to) is declined when any frame in (from, to] is a confirmed cut, with the frame named;
		// with no confirmed cut it is served.
		TEST_METHOD(GapPairAcrossAConfirmedCutIsDeclined)
		{
			VpMotion::VectorEngine engine;   // only the gap pair 8 -> 11 (no adjacent 10 -> 11 for the reader to prefer)
			engine.PublishProducer(std::make_shared<MotionAhead::Producer>());
			engine.p2Slot->SetGeneration(engine.motionValidity.StreamGeneration());
			engine.p2Slot->Publish(8, 11, engine.p2Slot->Generation(), MakeFlow(3.0f, true, false));
			VpMotion::P2PairRecord rec;
			std::string why;
			{
				const CutSeamScope scope(&CutAtTen);
				Assert::IsFalse(engine.P2RecordEndingAt(11, rec, why), L"the gap pair crosses the confirmed cut at 10");
				Assert::IsTrue(Has(why, "crosses a confirmed cut at 10"), L"the cut frame is named");
			}
			{
				const CutSeamScope scope(&CutNowhere);
				why.clear();
				Assert::IsTrue(engine.P2RecordEndingAt(11, rec, why), L"no confirmed cut: the gap pair is served");
				Assert::AreEqual(uint64_t(8), rec.from, L"the record names the gap pair");
				Assert::AreEqual(uint64_t(11), rec.to);
			}
		}

		// GF-P4 S-2 FIX (f4916b359): the gap read's refusal at a confirmed cut is COUNTED like PairValidAt's adjacent refusal: once in this
		// state (ConfirmedCutRefusals) and once process-wide (ConfirmedCutRefusalsAll). With no confirmed cut the same read is served
		// and neither count moves.
		TEST_METHOD(GapRefusalAtAConfirmedCutIsCounted)
		{
			VpMotion::VectorEngine engine;   // only the gap pair 8 -> 11
			engine.PublishProducer(std::make_shared<MotionAhead::Producer>());
			engine.p2Slot->SetGeneration(engine.motionValidity.StreamGeneration());
			engine.p2Slot->Publish(8, 11, engine.p2Slot->Generation(), MakeFlow(3.0f, true, false));
			VpMotion::P2PairRecord rec;
			std::string why;
			{
				const CutSeamScope scope(&CutAtTen);
				const unsigned long long r0 = engine.motionValidity.ConfirmedCutRefusals();
				const unsigned long long a0 = ::MotionValidity::State::ConfirmedCutRefusalsAll().load();
				Assert::IsFalse(engine.P2RecordEndingAt(11, rec, why), L"the gap pair crosses the confirmed cut at 10");
				Assert::IsTrue(Has(why, "crosses a confirmed cut at 10"));
				Assert::AreEqual(r0 + 1ull, static_cast<unsigned long long>(engine.motionValidity.ConfirmedCutRefusals()), L"counted once in this state");
				Assert::AreEqual(a0 + 1ull, static_cast<unsigned long long>(::MotionValidity::State::ConfirmedCutRefusalsAll().load()), L"and once process-wide");
			}
			{
				const CutSeamScope scope(&CutNowhere);
				why.clear();
				const unsigned long long r1 = engine.motionValidity.ConfirmedCutRefusals();
				const unsigned long long a1 = ::MotionValidity::State::ConfirmedCutRefusalsAll().load();
				Assert::IsTrue(engine.P2RecordEndingAt(11, rec, why), L"no confirmed cut: the same read is served");
				Assert::AreEqual(r1, static_cast<unsigned long long>(engine.motionValidity.ConfirmedCutRefusals()), L"nothing counted");
				Assert::AreEqual(a1, static_cast<unsigned long long>(::MotionValidity::State::ConfirmedCutRefusalsAll().load()));
			}
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

		// M1#2 (U10 C1, PairGlobalFill.h): deterministic 80-byte little-endian POD codec round-trip for every PairGlobalSource
		// and valid flag, plus corruption negative controls proving every malformed wire buffer is rejected with `out` untouched.
		TEST_METHOD(PairGlobalCodecRoundTripAndCorruptionNegativeControls)
		{
			Assert::AreEqual(size_t(80), VpMotion::kPairGlobalRecordBytes, L"PairGlobal wire format is exactly 80 bytes");
			Assert::AreEqual(std::string("cleared"), std::string(VpMotion::PairGlobalSourceName(VpMotion::PairGlobalSource::Cleared)));
			Assert::AreEqual(std::string("camera_model"), std::string(VpMotion::PairGlobalSourceName(VpMotion::PairGlobalSource::CameraModel)));
			Assert::AreEqual(std::string("engine"), std::string(VpMotion::PairGlobalSourceName(VpMotion::PairGlobalSource::Engine)));
			Assert::AreEqual(std::string("gap_seed"), std::string(VpMotion::PairGlobalSourceName(VpMotion::PairGlobalSource::GapSeed)));
			Assert::AreEqual(std::string("matcher_d1"), std::string(VpMotion::PairGlobalSourceName(VpMotion::PairGlobalSource::MatcherD1)));
			Assert::AreEqual(std::string("?"), std::string(VpMotion::PairGlobalSourceName(static_cast<VpMotion::PairGlobalSource>(99))));

			const VpMotion::PairGlobalSource allSources[] = {
				VpMotion::PairGlobalSource::Cleared,
				VpMotion::PairGlobalSource::CameraModel,
				VpMotion::PairGlobalSource::Engine,
				VpMotion::PairGlobalSource::GapSeed,
				VpMotion::PairGlobalSource::MatcherD1
			};
			uint64_t idx = 0;
			for (VpMotion::PairGlobalSource src : allSources)
			{
				for (int validFlag = 0; validFlag <= 1; ++validFlag)
				{
					const VpMotion::PairGlobal in = MakePairGlobalSample(src, validFlag != 0,
						0x0123456789ABCDEFULL + idx * 0x01010101ULL, 2.5f + float(idx));
					uint8_t wire[VpMotion::kPairGlobalRecordBytes] = {};
					Assert::AreEqual(VpMotion::kPairGlobalRecordBytes,
						VpMotion::EncodePairGlobal(in, wire, sizeof(wire)), L"encode returns exact wire byte count");
					// Pin little-endian magic "PPG1" and header layout.
					Assert::AreEqual(int('P'), int(wire[0]));
					Assert::AreEqual(int('P'), int(wire[1]));
					Assert::AreEqual(int('G'), int(wire[2]));
					Assert::AreEqual(int('1'), int(wire[3]));
					Assert::AreEqual(int(static_cast<uint8_t>(src)), int(wire[4]));
					Assert::AreEqual(int(validFlag != 0 ? VpMotion::kPairGlobalFlagValid : 0u), int(wire[5]));
					Assert::AreEqual(0, int(wire[6]), L"reserved byte 0 must be zero");
					Assert::AreEqual(0, int(wire[7]), L"reserved byte 1 must be zero");

					VpMotion::PairGlobal decoded = MakePairGlobalSample(VpMotion::PairGlobalSource::GapSeed, false, 999u, -99.0f);
					Assert::IsTrue(VpMotion::DecodePairGlobal(wire, sizeof(wire), decoded), L"valid wire record decodes");
					Assert::IsTrue(SamePairGlobal(in, decoded), L"bit-exact round-trip of vec, second, src, p2Epoch, valid");
					++idx;
				}
			}

			// Encode negative controls: null buffer, truncated capacity, and unknown PairGlobalSource enum value.
			const VpMotion::PairGlobal good = MakePairGlobalSample(VpMotion::PairGlobalSource::CameraModel, true, 0xFEDCBA9876543210ULL, 4.25f);
			uint8_t goodWire[VpMotion::kPairGlobalRecordBytes] = {};
			Assert::AreEqual(size_t(0), VpMotion::EncodePairGlobal(good, nullptr, sizeof(goodWire)), L"null output refused");
			Assert::AreEqual(size_t(0), VpMotion::EncodePairGlobal(good, goodWire, sizeof(goodWire) - 1u), L"short buffer refused");
			VpMotion::PairGlobal badEnum = good;
			badEnum.src = static_cast<VpMotion::PairGlobalSource>(static_cast<uint8_t>(VpMotion::PairGlobalSource::MatcherD1) + 1u);
			Assert::AreEqual(size_t(0), VpMotion::EncodePairGlobal(badEnum, goodWire, sizeof(goodWire)), L"out-of-range enum refused on encode");
			badEnum.src = static_cast<VpMotion::PairGlobalSource>(0xFFu);
			Assert::AreEqual(size_t(0), VpMotion::EncodePairGlobal(badEnum, goodWire, sizeof(goodWire)), L"0xFF enum refused on encode");
			Assert::AreEqual(VpMotion::kPairGlobalRecordBytes, VpMotion::EncodePairGlobal(good, goodWire, sizeof(goodWire)));

			// Decode corruption negative controls on the exact same valid wire buffer: every rejection leaves sentinel `out` untouched.
			const VpMotion::PairGlobal sentinel = MakePairGlobalSample(VpMotion::PairGlobalSource::GapSeed, false, 0xCAFEBABE11223344ULL, -13.5f);
			auto assertRejectedUntouched = [&](const uint8_t* buf, size_t len, const wchar_t* msg) {
				VpMotion::PairGlobal out = sentinel;
				Assert::IsFalse(VpMotion::DecodePairGlobal(buf, len, out), msg);
				Assert::IsTrue(SamePairGlobal(sentinel, out), L"failed decode must leave output untouched");
			};

			assertRejectedUntouched(nullptr, sizeof(goodWire), L"null input rejected");
			assertRejectedUntouched(goodWire, sizeof(goodWire) - 1u, L"truncated 79-byte buffer rejected");
			uint8_t overWire[VpMotion::kPairGlobalRecordBytes + 1u] = {};
			std::memcpy(overWire, goodWire, sizeof(goodWire));
			assertRejectedUntouched(overWire, sizeof(overWire), L"oversized 81-byte buffer rejected");

			uint8_t corrupt[VpMotion::kPairGlobalRecordBytes] = {};
			auto testMutation = [&](size_t byteIndex, uint8_t newValue, const wchar_t* msg) {
				std::memcpy(corrupt, goodWire, sizeof(goodWire));
				corrupt[byteIndex] = newValue;
				assertRejectedUntouched(corrupt, sizeof(corrupt), msg);
			};
			testMutation(0u, uint8_t(goodWire[0] ^ 0x01u), L"corrupt magic byte 0 rejected");
			testMutation(3u, uint8_t(goodWire[3] ^ 0x80u), L"corrupt magic byte 3 rejected");
			testMutation(4u, uint8_t(static_cast<uint8_t>(VpMotion::PairGlobalSource::MatcherD1) + 1u), L"unknown PairGlobalSource (5) rejected");
			testMutation(4u, 0xFFu, L"unknown PairGlobalSource (0xFF) rejected");
			testMutation(5u, 0x02u, L"unknown flag bit (0x02) rejected");
			testMutation(5u, 0x03u, L"valid + unknown flag bit (0x03) rejected");
			testMutation(6u, 0x01u, L"non-zero reserved byte 0 rejected");
			testMutation(7u, 0x80u, L"non-zero reserved byte 1 rejected");

			// Same-input positive control: the exact uncorrupted buffer decodes and replaces sentinel.
			VpMotion::PairGlobal outGood = sentinel;
			Assert::IsTrue(VpMotion::DecodePairGlobal(goodWire, sizeof(goodWire), outGood), L"uncorrupted buffer decodes");
			Assert::IsTrue(SamePairGlobal(good, outGood), L"uncorrupted buffer matches source record");
		}

		// M1#2 (U10 C1, PairGlobalFill.h): VariantKey axis contract (kAxisParam0 only, param0 == 1 required).
		// Default variant and param0 != 1 decline visibly with "param0 must be 1"; undeclared axes return VariantUndeclared.
		TEST_METHOD(PairGlobalVariantKeyAndParam0Enforcement)
		{
			using namespace VpSkeleton;
			const VariantKey key = VpMotion::PairGlobalKey();
			Assert::AreEqual(uint16_t(1), key.param0, L"PairGlobalKey sets param0 == 1");
			Assert::AreEqual(uint32_t(VariantKey::kAxisParam0), key.Axes(), L"PairGlobalKey uses kAxisParam0 only");
			Assert::IsFalse(key.IsDefault(), L"PairGlobalKey is non-default");
			Assert::AreEqual(uint8_t(0), key.plane);
			Assert::AreEqual(uint16_t(0), key.param1);

			int sentinelTex = 7;
			const pl_tex fakeTex = reinterpret_cast<pl_tex>(&sentinelTex);
			int cpuCalls = 0;
			int gpuCalls = 0;

			SkeletonMemo memo;
			memo.SetRenderThread(std::this_thread::get_id());
			Assert::IsTrue(memo.RegisterOwnerFill(kP7CameraFit, AcquireForm::Cpu, VpMotion::kPairGlobalUnit,
				VpMotion::MakeP7PairGlobalCpuOwnerFill([&](const OwnerFillRequest&, SkeletonMemo&, VpMotion::PairGlobal& out, std::string&) {
					++cpuCalls;
					out = MakePairGlobalSample(VpMotion::PairGlobalSource::CameraModel, true, 12u, 1.0f);
					return true;
				})) == SkeletonMemo::Registration::Registered);
			Assert::IsTrue(memo.RegisterOwnerFill(kP7CameraFit, AcquireForm::Gpu, VpMotion::kPairGlobalUnit,
				VpMotion::MakeP7PairGlobalGpuOwnerFill([&](const OwnerFillRequest&, SkeletonMemo&, VpMotion::PairGlobalGpuView& out, std::string&) {
					++gpuCalls;
					out.globalVector = fakeTex;
					out.record = MakePairGlobalSample(VpMotion::PairGlobalSource::MatcherD1, true, 12u, 1.0f);
					return true;
				})) == SkeletonMemo::Registration::Registered);

			// Positive control: param0 == 1 succeeds for both CPU and GPU forms.
			Assert::IsTrue(memo.Acquire(kP7CameraFit, 11, AcquireForm::Cpu, key).status == AcquireStatus::FilledAtRead);
			Assert::IsTrue(memo.Acquire(kP7CameraFit, 11, AcquireForm::Gpu, key).status == AcquireStatus::FilledAtRead);
			Assert::AreEqual(1, cpuCalls);
			Assert::AreEqual(1, gpuCalls);

			// Negative control 1: default VariantKey{} (param0 == 0) declines visibly with "param0 must be 1" before provider runs.
			for (AcquireForm form : { AcquireForm::Cpu, AcquireForm::Gpu })
			{
				const AcquireResult r = memo.Acquire(kP7CameraFit, 12, form, VariantKey{});
				Assert::IsTrue(r.status == AcquireStatus::Unavailable, L"default variant must decline");
				Assert::IsTrue(r.cause == AcquireCause::OwnerDeclined, L"default variant is OwnerDeclined");
				Assert::IsTrue(Has(memo.LastCause(kP7CameraFit), "param0 must be 1"), L"visible decline reason names param0 == 1");
			}
			Assert::AreEqual(1, cpuCalls, L"CPU provider must not run for param0 == 0");
			Assert::AreEqual(1, gpuCalls, L"GPU provider must not run for param0 == 0");

			// Negative control 2: param0 == 2 declines visibly with "param0 must be 1" before provider runs.
			VariantKey badParam0;
			badParam0.param0 = 2u;
			for (AcquireForm form : { AcquireForm::Cpu, AcquireForm::Gpu })
			{
				const AcquireResult r = memo.Acquire(kP7CameraFit, 13, form, badParam0);
				Assert::IsTrue(r.status == AcquireStatus::Unavailable, L"param0 == 2 must decline");
				Assert::IsTrue(r.cause == AcquireCause::OwnerDeclined, L"param0 == 2 is OwnerDeclined");
				Assert::IsTrue(Has(memo.LastCause(kP7CameraFit), "param0 must be 1"), L"visible decline reason names param0 == 1");
			}
			Assert::AreEqual(1, cpuCalls, L"CPU provider must not run for param0 == 2");
			Assert::AreEqual(1, gpuCalls, L"GPU provider must not run for param0 == 2");

			// Negative control 3: undeclared axes (plane = 1 alone, plane = 1 with param0 = 1, param1 = 1 with param0 = 1) -> VariantUndeclared.
			VariantKey planeOnly;
			planeOnly.plane = 1u;
			VariantKey planeAndParam0 = key;
			planeAndParam0.plane = 1u;
			VariantKey param1AndParam0 = key;
			param1AndParam0.param1 = 1u;
			const VariantKey undeclared[] = { planeOnly, planeAndParam0, param1AndParam0 };
			for (const VariantKey& badKey : undeclared)
			{
				for (AcquireForm form : { AcquireForm::Cpu, AcquireForm::Gpu })
				{
					const AcquireResult r = memo.Acquire(kP7CameraFit, 14, form, badKey);
					Assert::IsTrue(r.status == AcquireStatus::Unavailable, L"undeclared axis must fail");
					Assert::IsTrue(r.cause == AcquireCause::VariantUndeclared, L"undeclared axis must be VariantUndeclared");
				}
			}
			Assert::AreEqual(1, cpuCalls, L"CPU provider must not run for undeclared axis");
			Assert::AreEqual(1, gpuCalls, L"GPU provider must not run for undeclared axis");
		}

		// M1#2 (U10 C1, PairGlobalFill.h + FallbackCatalog::PairGlobalUnavailable + FallbackPlainText):
		// Default CPU fill reads kP2VectorPair via nested memo.Acquire, publishes Engine PairGlobal once (reused on 2nd read),
		// works off-render-thread, and declines visibly with "motion.pair_global_unavailable" when P2 is absent, GlobalSet() is false,
		// or a custom PairGlobalCpuFn fails.
		TEST_METHOD(PairGlobalCpuOwnerFillDefaultP2NestedFallbackAndNegativeControls)
		{
			using namespace VpSkeleton;

			// Positive path: real VectorEngine + P2CpuForm registered -> default MakeP7PairGlobalCpuOwnerFill() reads nested P2.
			Rig rig;   // pair (10, 11), generation 1, globalSet = true
			SkeletonMemo memo;
			memo.SetRenderThread(std::this_thread::get_id());
			Assert::IsTrue(RegisterFill(memo, rig.EngineOf()));
			Assert::IsTrue(memo.RegisterOwnerFill(kP7CameraFit, AcquireForm::Cpu, VpMotion::kPairGlobalUnit,
				VpMotion::MakeP7PairGlobalCpuOwnerFill()) == SkeletonMemo::Registration::Registered);

			VpMotion::PairGlobal rec1;
			AcquireResult res1;
			FallbackCatalog::Decision fb1;
			Assert::IsTrue(VpMotion::AcquirePairGlobalCpu(memo, 11, rec1, &res1, &fb1), L"default CPU fill serves from nested P2");
			Assert::IsTrue(res1.status == AcquireStatus::FilledAtRead, L"first read fills at read");
			Assert::IsTrue(res1.source == AcquireSource::Memo);
			Assert::AreEqual(VpMotion::kPairGlobalRecordBytes, res1.cpuBytes);
			Assert::IsTrue(rec1.valid, L"published engine record is valid");
			Assert::IsTrue(rec1.src == VpMotion::PairGlobalSource::Engine, L"default nested P2 fill tags PairGlobalSource::Engine");
			Assert::AreEqual(uint64_t(1), rec1.p2Epoch, L"p2Epoch carries P2 generation");
			Assert::AreEqual(0, std::memcmp(rec1.vec, rig.flow->global, sizeof(rec1.vec)), L"vec[2][4] matches P2 global bit-for-bit");
			Assert::AreEqual(0, std::memcmp(rec1.second, rig.flow->second, sizeof(rec1.second)), L"second[2][4] matches P2 second bit-for-bit");
			Assert::IsFalse(fb1.engaged, L"success releases motion.pair_global_unavailable");
			Assert::AreEqual(std::string("motion.pair_global_unavailable"), fb1.id);
			Assert::IsTrue(fb1.reason.empty(), L"released fallback has empty reason");
			Assert::AreEqual(1ull, rig.engine.P2CpuFormServed(), L"nested memo.Acquire read P2 once");

			// 2nd acquire on same seq = 11: Reused (compute-once).
			VpMotion::PairGlobal rec2;
			AcquireResult res2;
			FallbackCatalog::Decision fb2;
			Assert::IsTrue(VpMotion::AcquirePairGlobalCpu(memo, 11, rec2, &res2, &fb2));
			Assert::IsTrue(res2.status == AcquireStatus::Reused, L"2nd acquire reuses memo spot");
			Assert::IsTrue(SamePairGlobal(rec1, rec2), L"reused record matches first read bit-for-bit");
			Assert::IsFalse(fb2.engaged);
			Assert::AreEqual(1ull, memo.CountersOf(kP7CameraFit).ownerFillCalls, L"compute-once: P7 owner fill ran once");
			Assert::AreEqual(1ull, rig.engine.P2CpuFormServed(), L"compute-once: P2 not re-acquired");

			// CPU form is any-thread (renderThreadOnly = false): off-render-thread Acquire succeeds.
			VpMotion::PairGlobal recWorker;
			AcquireResult resWorker;
			FallbackCatalog::Decision fbWorker;
			bool workerOk = false;
			{
				PairGlobalThreadJoinGuard guard;
				guard.thread = std::thread([&]() {
					workerOk = VpMotion::AcquirePairGlobalCpu(memo, 11, recWorker, &resWorker, &fbWorker);
				});
			}
			Assert::IsTrue(workerOk, L"CPU pair-global acquire succeeds off render thread");
			Assert::IsTrue(resWorker.status == AcquireStatus::Reused);
			Assert::IsTrue(SamePairGlobal(rec1, recWorker));
			Assert::IsFalse(fbWorker.engaged);

			// Negative control 1 (EXACT SAME seq = 11, P2 absent): declines visibly and engages motion.pair_global_unavailable.
			{
				SkeletonMemo memoNoP2;
				Assert::IsTrue(memoNoP2.RegisterOwnerFill(kP7CameraFit, AcquireForm::Cpu, VpMotion::kPairGlobalUnit,
					VpMotion::MakeP7PairGlobalCpuOwnerFill()) == SkeletonMemo::Registration::Registered);
				VpMotion::PairGlobal recMissing = rec1;
				AcquireResult resMissing;
				FallbackCatalog::Decision fbMissing;
				Assert::IsFalse(VpMotion::AcquirePairGlobalCpu(memoNoP2, 11, recMissing, &resMissing, &fbMissing),
					L"negative control: absent P2 must fail AcquirePairGlobalCpu");
				Assert::IsTrue(resMissing.status == AcquireStatus::Unavailable);
				Assert::IsTrue(resMissing.cause == AcquireCause::OwnerDeclined);
				Assert::IsFalse(recMissing.valid, L"failed acquire clears output record");
				Assert::IsTrue(recMissing.src == VpMotion::PairGlobalSource::Cleared);
				Assert::AreEqual(uint64_t(0), recMissing.p2Epoch);
				Assert::IsTrue(fbMissing.engaged, L"absent P2 engages fallback");
				Assert::AreEqual(std::string("motion.pair_global_unavailable"), fbMissing.id);
				Assert::IsFalse(fbMissing.reason.empty(), L"fallback reason must be non-empty");
				Assert::IsTrue(Has(fbMissing.reason, "pair global: unavailable"), L"fallback reason carries decline detail");
				Assert::IsTrue(Has(fbMissing.reason, "motion estimation aborted for this frame"));
			}

			// Negative control 2 (EXACT SAME seq = 11, real VectorEngine + P2CpuForm present, BUT GlobalSet() == false):
			// P2 IS readable, yet default PairGlobal fill must decline visibly instead of publishing an unset global.
			{
				VpMotion::VectorEngine unsetEngine;
				unsetEngine.PublishProducer(std::make_shared<MotionAhead::Producer>());
				unsetEngine.p2Slot->SetGeneration(unsetEngine.motionValidity.StreamGeneration());
				unsetEngine.p2Slot->Publish(10, 11, unsetEngine.p2Slot->Generation(), MakeFlow(3.0f, /*globalSet=*/false, false));

				VpMotion::P2PairRecord directP2;
				std::string p2Why;
				Assert::IsTrue(unsetEngine.P2RecordEndingAt(11, directP2, p2Why), L"precondition: P2 record itself exists");
				Assert::IsFalse(directP2.GlobalSet(), L"precondition: P2 record has GlobalSet() == false");

				SkeletonMemo memoUnset;
				VpMotion::IVectorEngine* enginePtr = &unsetEngine;
				Assert::IsTrue(RegisterFill(memoUnset, [enginePtr](const void*) { return enginePtr; }));
				Assert::IsTrue(memoUnset.RegisterOwnerFill(kP7CameraFit, AcquireForm::Cpu, VpMotion::kPairGlobalUnit,
					VpMotion::MakeP7PairGlobalCpuOwnerFill()) == SkeletonMemo::Registration::Registered);

				VpMotion::PairGlobal recUnset = rec1;
				AcquireResult resUnset;
				FallbackCatalog::Decision fbUnset;
				Assert::IsFalse(VpMotion::AcquirePairGlobalCpu(memoUnset, 11, recUnset, &resUnset, &fbUnset),
					L"negative control: GlobalSet() == false must decline");
				Assert::IsTrue(resUnset.status == AcquireStatus::Unavailable);
				Assert::IsTrue(resUnset.cause == AcquireCause::OwnerDeclined);
				Assert::AreEqual(1ull, unsetEngine.P2CpuFormServed(), L"nested P2 was actually read before declining");
				Assert::IsFalse(recUnset.valid);
				Assert::IsTrue(fbUnset.engaged, L"unset P2 global engages motion.pair_global_unavailable");
				Assert::AreEqual(std::string("motion.pair_global_unavailable"), fbUnset.id);
				Assert::IsTrue(Has(fbUnset.reason, "pair global: unavailable"));
			}

			// Custom PairGlobalCpuFn: success, custom failure reason, empty failure reason, and invalid-enum encode failure + FallbackPlainText.
			{
				const VpMotion::PairGlobal customOk = MakePairGlobalSample(VpMotion::PairGlobalSource::CameraModel, true, 77u, -5.5f);
				SkeletonMemo memoCustom;
				Assert::IsTrue(memoCustom.RegisterOwnerFill(kP7CameraFit, AcquireForm::Cpu, VpMotion::kPairGlobalUnit,
					VpMotion::MakeP7PairGlobalCpuOwnerFill([&](const OwnerFillRequest& req, SkeletonMemo&, VpMotion::PairGlobal& out, std::string& why) {
						if (req.seq == 20)
						{
							out = customOk;
							return true;
						}
						if (req.seq == 21)
						{
							why = "camera fit rank-deficient";
							return false;
						}
						if (req.seq == 22)
						{
							why.clear();
							return false;
						}
						out = customOk;
						out.src = static_cast<VpMotion::PairGlobalSource>(99);
						return true;
					})) == SkeletonMemo::Registration::Registered);

				VpMotion::PairGlobal got;
				AcquireResult res;
				FallbackCatalog::Decision fb;
				Assert::IsTrue(VpMotion::AcquirePairGlobalCpu(memoCustom, 20, got, &res, &fb));
				Assert::IsTrue(SamePairGlobal(customOk, got));
				Assert::IsFalse(fb.engaged);

				Assert::IsFalse(VpMotion::AcquirePairGlobalCpu(memoCustom, 21, got, &res, &fb), L"custom provider failure declines");
				Assert::IsTrue(res.cause == AcquireCause::OwnerDeclined);
				Assert::IsTrue(fb.engaged);
				Assert::AreEqual(std::string("motion.pair_global_unavailable"), fb.id);
				Assert::IsTrue(Has(fb.reason, "camera fit rank-deficient"), L"custom why must appear in fallback reason");
				Assert::IsTrue(Has(memoCustom.LastCause(kP7CameraFit), "camera fit rank-deficient"));

				Assert::IsFalse(VpMotion::AcquirePairGlobalCpu(memoCustom, 22, got, &res, &fb), L"empty why still produces visible reason");
				Assert::IsTrue(fb.engaged && Has(fb.reason, "pair global: unavailable"));

				Assert::IsFalse(VpMotion::AcquirePairGlobalCpu(memoCustom, 23, got, &res, &fb), L"unencodable enum declines visibly");
				Assert::IsTrue(fb.engaged && Has(fb.reason, "pair global: record not encoded"));

				// FallbackPlainText plain English sentence exists and is used by FallbackNoticeText (never generic unknown-id text).
				const std::string sentence = FallbackPlainText::Sentence("motion.pair_global_unavailable");
				Assert::IsFalse(sentence.empty(), L"FallbackPlainText must have a plain-English sentence for motion.pair_global_unavailable");
				const std::string notice = FallbackPlainText::FallbackNoticeText("motion.pair_global_unavailable", fb.reason);
				Assert::IsTrue(notice.find(sentence) == 0u, L"FallbackNoticeText must lead with the catalog plain-English sentence");
				Assert::IsTrue(Has(notice, fb.reason.c_str()), L"FallbackNoticeText must include the detail reason");

				const FallbackCatalog::Decision directEngaged = FallbackCatalog::PairGlobalUnavailable(true, "unit check");
				Assert::IsTrue(directEngaged.engaged && directEngaged.id == "motion.pair_global_unavailable" && Has(directEngaged.reason, "unit check"));
				const FallbackCatalog::Decision directReleased = FallbackCatalog::PairGlobalUnavailable(false);
				Assert::IsFalse(directReleased.engaged);
				Assert::AreEqual(std::string("motion.pair_global_unavailable"), directReleased.id);
			}
		}

		// M1#2 (U10 C1, PairGlobalFill.h): GPU zero-copy frame-scoped view (gpu.bytes == 0, 1x1, view = true, frameScoped = true),
		// compute-once reuse, OnSeal eviction with same-input non-frame-scoped stale-handle negative control, off-render-thread
		// WrongThread enforcement (RAII thread join guard), and null/declining/absent provider fallback visibility.
		TEST_METHOD(PairGlobalGpuOwnerFillZeroCopyFrameScopedAndNegativeControls)
		{
			using namespace VpSkeleton;

			int sentinelMem[4] = { 101, 202, 303, 404 };
			const pl_tex texVecA = reinterpret_cast<pl_tex>(&sentinelMem[0]);
			const pl_tex texSecA = reinterpret_cast<pl_tex>(&sentinelMem[1]);
			const pl_tex texVecB = reinterpret_cast<pl_tex>(&sentinelMem[2]);
			const pl_tex texSecB = reinterpret_cast<pl_tex>(&sentinelMem[3]);

			const VpMotion::PairGlobal recA = MakePairGlobalSample(VpMotion::PairGlobalSource::MatcherD1, true, 55u, 1.5f);
			const VpMotion::PairGlobal recB = MakePairGlobalSample(VpMotion::PairGlobalSource::MatcherD1, true, 56u, -2.25f);

			pl_tex curVec = texVecA;
			pl_tex curSec = texSecA;
			VpMotion::PairGlobal curRec = recA;
			int providerCalls = 0;

			auto provider = [&](const OwnerFillRequest& req, SkeletonMemo&, VpMotion::PairGlobalGpuView& outView, std::string& outWhy) -> bool {
				++providerCalls;
				if (req.seq == 13)
				{
					outView.globalVector = nullptr;
					outView.globalSecond = curSec;
					outView.record = curRec;
					return true;   // returns true but with null globalVector: must still decline visibly
				}
				if (req.seq == 14)
				{
					outWhy = "matcher global tex unallocated";
					return false;
				}
				outView.globalVector = curVec;
				outView.globalSecond = curSec;
				outView.record = curRec;
				return true;
			};

			// Zero-copy view (view = true, gpu.bytes = 0) works without GPU dispatcher or VRAM ledger grant.
			SkeletonMemo memo;
			memo.SetRenderThread(std::this_thread::get_id());
			Assert::IsTrue(memo.RegisterOwnerFill(kP7CameraFit, AcquireForm::Gpu, VpMotion::kPairGlobalUnit,
				VpMotion::MakeP7PairGlobalGpuOwnerFill(provider)) == SkeletonMemo::Registration::Registered);

			// 1st acquire: FilledAtRead, zero-copy 1x1 view, exact PairGlobalGpuView payload, fallback released.
			VpMotion::PairGlobalGpuView view1;
			AcquireResult res1;
			FallbackCatalog::Decision fb1;
			Assert::IsTrue(VpMotion::AcquirePairGlobalGpu(memo, 11, view1, &res1, &fb1), L"1st GPU acquire succeeds");
			Assert::IsTrue(res1.status == AcquireStatus::FilledAtRead);
			Assert::IsTrue(res1.source == AcquireSource::Memo);
			Assert::AreEqual(uint64_t(0), res1.gpu.bytes, L"zero-copy GPU view allocates 0 bytes");
			Assert::AreEqual(uint32_t(1), res1.gpu.width, L"1x1 global texture width");
			Assert::AreEqual(uint32_t(1), res1.gpu.height, L"1x1 global texture height");
			Assert::IsTrue(res1.gpu.resource == static_cast<const void*>(texVecA), L"gpu.resource carries globalVector handle");
			Assert::IsTrue(res1.gpu.textureSet == VpMotion::PairGlobalGpuTag());
			const VpMotion::PairGlobalGpuView* directView = VpMotion::PairGlobalGpuOf(res1);
			Assert::IsNotNull(directView, L"PairGlobalGpuOf extracts view from tagged result");
			Assert::IsTrue(directView->globalVector == texVecA && directView->globalSecond == texSecA);
			Assert::IsTrue(SamePairGlobal(recA, directView->record));
			Assert::IsTrue(view1.globalVector == texVecA && view1.globalSecond == texSecA);
			Assert::IsTrue(SamePairGlobal(recA, view1.record));
			Assert::IsFalse(fb1.engaged);
			Assert::AreEqual(std::string("motion.pair_global_unavailable"), fb1.id);
			Assert::AreEqual(1, providerCalls);

			// 2nd acquire on same seq = 11: Reused (compute-once).
			VpMotion::PairGlobalGpuView view2;
			AcquireResult res2;
			FallbackCatalog::Decision fb2;
			Assert::IsTrue(VpMotion::AcquirePairGlobalGpu(memo, 11, view2, &res2, &fb2));
			Assert::IsTrue(res2.status == AcquireStatus::Reused, L"2nd GPU acquire reuses frame spot");
			Assert::IsTrue(view2.globalVector == texVecA && view2.globalSecond == texSecA);
			Assert::IsFalse(fb2.engaged);
			Assert::AreEqual(1, providerCalls, L"compute-once: GPU provider called once");

			std::weak_ptr<void> weakReal = res1.gpu.keepAlive;
			res1 = AcquireResult();
			res2 = AcquireResult();
			Assert::IsFalse(weakReal.expired(), L"memo holds view keep-alive prior to OnSeal");
			Assert::AreEqual(size_t(1), memo.MemoCount());

			// Same-input negative control (Rule 3): a naive non-frame-scoped GPU view on the EXACT SAME seq = 11
			// survives OnSeal(11) and keeps returning the STALE texVecA handle after the matcher rotates to texVecB.
			{
				pl_tex naiveVec = texVecA;
				pl_tex naiveSec = texSecA;
				int naiveCalls = 0;
				OwnerFill baseFill = VpMotion::MakeP7PairGlobalGpuOwnerFill(
					[&](const OwnerFillRequest&, SkeletonMemo&, VpMotion::PairGlobalGpuView& outView, std::string&) {
						++naiveCalls;
						outView.globalVector = naiveVec;
						outView.globalSecond = naiveSec;
						outView.record = recA;
						return true;
					});
				OwnerFill naiveFill = baseFill;
				naiveFill.frameScoped = false;
				naiveFill.fill = [inner = baseFill.fill](const OwnerFillRequest& req, SkeletonMemo& m, OwnerFillOutput& out) {
					const bool ok = inner(req, m, out);
					if (ok)
						out.scope = OwnerFillOutput::Scope::Window;
					return ok;
				};
				SkeletonMemo naiveMemo;
				naiveMemo.SetRenderThread(std::this_thread::get_id());
				Assert::IsTrue(naiveMemo.RegisterOwnerFill(kP7CameraFit, AcquireForm::Gpu, VpMotion::kPairGlobalUnit, naiveFill)
					== SkeletonMemo::Registration::Registered);
				VpMotion::PairGlobalGpuView nv1, nv2;
				AcquireResult nr1, nr2;
				Assert::IsTrue(VpMotion::AcquirePairGlobalGpu(naiveMemo, 11, nv1, &nr1));
				Assert::IsTrue(nv1.globalVector == texVecA);
				std::weak_ptr<void> weakNaive = nr1.gpu.keepAlive;
				nr1 = AcquireResult();
				naiveMemo.OnSeal(11);
				naiveVec = texVecB;
				naiveSec = texSecB;
				Assert::AreEqual(0ull, naiveMemo.CountersOf(kP7CameraFit).released,
					L"negative control: naive non-frame-scoped view is NOT evicted at OnSeal(11)");
				Assert::IsFalse(weakNaive.expired(), L"negative control: naive view leaks keep-alive past OnSeal(11)");
				Assert::IsTrue(VpMotion::AcquirePairGlobalGpu(naiveMemo, 11, nv2, &nr2));
				Assert::IsTrue(nr2.status == AcquireStatus::Reused && nv2.globalVector == texVecA && naiveCalls == 1,
					L"negative control: naive view returns stale texVecA handle instead of rotated texVecB");
			}

			// Protected path: MakeP7PairGlobalGpuOwnerFill evicts the frame-scoped view at OnSeal(11).
			memo.OnSeal(11);
			Assert::AreEqual(size_t(0), memo.MemoCount(), L"frame-scoped GPU view evicted at OnSeal(11)");
			Assert::AreEqual(1ull, memo.CountersOf(kP7CameraFit).released, L"eviction counted as released");
			Assert::IsTrue(weakReal.expired(), L"keep-alive dropped once frame-scoped view is evicted");

			curVec = texVecB;
			curSec = texSecB;
			curRec = recB;
			VpMotion::PairGlobalGpuView view3;
			AcquireResult res3;
			FallbackCatalog::Decision fb3;
			Assert::IsTrue(VpMotion::AcquirePairGlobalGpu(memo, 11, view3, &res3, &fb3));
			Assert::IsTrue(res3.status == AcquireStatus::FilledAtRead, L"post-OnSeal acquire does not reuse evicted view");
			Assert::IsTrue(view3.globalVector == texVecB && view3.globalSecond == texSecB, L"fresh rotated handles served, never stale texVecA");
			Assert::IsTrue(SamePairGlobal(recB, view3.record));
			Assert::IsFalse(fb3.engaged);
			Assert::AreEqual(2, providerCalls);

			// Off-render-thread GPU acquire: Unavailable(WrongThread), fallback engaged, provider never invoked (RAII thread guard).
			VpMotion::PairGlobalGpuView workerView;
			AcquireResult workerRes;
			FallbackCatalog::Decision workerFb;
			bool workerOk = true;
			{
				PairGlobalThreadJoinGuard guard;
				guard.thread = std::thread([&]() {
					workerOk = VpMotion::AcquirePairGlobalGpu(memo, 12, workerView, &workerRes, &workerFb);
				});
			}
			Assert::IsFalse(workerOk, L"GPU acquire off render thread must fail");
			Assert::IsTrue(workerRes.status == AcquireStatus::Unavailable);
			Assert::IsTrue(workerRes.cause == AcquireCause::WrongThread, L"off-render-thread GPU acquire is WrongThread");
			Assert::IsNull(workerView.globalVector);
			Assert::IsTrue(workerFb.engaged && workerFb.id == "motion.pair_global_unavailable");
			Assert::IsFalse(workerFb.reason.empty());
			Assert::AreEqual(2, providerCalls, L"GPU provider must not run off render thread");

			// Advancing seal to 12 retires frame 11: FrameReleased, never recomputed.
			memo.OnSeal(12);
			VpMotion::PairGlobalGpuView retiredView;
			AcquireResult retiredRes;
			FallbackCatalog::Decision retiredFb;
			Assert::IsFalse(VpMotion::AcquirePairGlobalGpu(memo, 11, retiredView, &retiredRes, &retiredFb));
			Assert::IsTrue(retiredRes.status == AcquireStatus::Unavailable && retiredRes.cause == AcquireCause::FrameReleased);
			Assert::IsTrue(retiredFb.engaged && retiredFb.id == "motion.pair_global_unavailable");
			Assert::AreEqual(2, providerCalls, L"retired frame never recomputes");

			// Null globalVector returned with true -> visibly declined + fallback engaged.
			VpMotion::PairGlobalGpuView nullView;
			AcquireResult nullRes;
			FallbackCatalog::Decision nullFb;
			Assert::IsFalse(VpMotion::AcquirePairGlobalGpu(memo, 13, nullView, &nullRes, &nullFb),
				L"null globalVector must decline visibly");
			Assert::IsTrue(nullRes.status == AcquireStatus::Unavailable && nullRes.cause == AcquireCause::OwnerDeclined);
			Assert::IsNull(VpMotion::PairGlobalGpuOf(nullRes));
			Assert::IsNull(nullView.globalVector);
			Assert::IsTrue(nullFb.engaged && nullFb.id == "motion.pair_global_unavailable");
			Assert::IsTrue(Has(nullFb.reason, "pair global: unavailable"));

			// Provider explicit decline with custom reason -> surfaces in fallback decision.
			VpMotion::PairGlobalGpuView decView;
			AcquireResult decRes;
			FallbackCatalog::Decision decFb;
			Assert::IsFalse(VpMotion::AcquirePairGlobalGpu(memo, 14, decView, &decRes, &decFb));
			Assert::IsTrue(decRes.status == AcquireStatus::Unavailable && decRes.cause == AcquireCause::OwnerDeclined);
			Assert::IsTrue(decFb.engaged && decFb.id == "motion.pair_global_unavailable");
			Assert::IsTrue(Has(decFb.reason, "matcher global tex unallocated"));

			// Default MakeP7PairGlobalGpuOwnerFill() with null provider -> declines visibly with "gpu provider absent".
			{
				SkeletonMemo memoNoGpuFn;
				memoNoGpuFn.SetRenderThread(std::this_thread::get_id());
				Assert::IsTrue(memoNoGpuFn.RegisterOwnerFill(kP7CameraFit, AcquireForm::Gpu, VpMotion::kPairGlobalUnit,
					VpMotion::MakeP7PairGlobalGpuOwnerFill()) == SkeletonMemo::Registration::Registered);
				VpMotion::PairGlobalGpuView v;
				AcquireResult r;
				FallbackCatalog::Decision fb;
				Assert::IsFalse(VpMotion::AcquirePairGlobalGpu(memoNoGpuFn, 15, v, &r, &fb));
				Assert::IsTrue(r.status == AcquireStatus::Unavailable && r.cause == AcquireCause::OwnerDeclined);
				Assert::IsTrue(fb.engaged && fb.id == "motion.pair_global_unavailable");
				Assert::IsTrue(Has(fb.reason, "pair global: gpu provider absent"));
			}
		}

		// M1#2 (U10 C1, PairGlobalFill.h): TryRegisterPairGlobalFills idempotency, catalog wrong-unit refusal (NotOwner),
		// refusing demand visibility, and PairGlobalGpuOf foreign textureSet tag rejection.
		TEST_METHOD(PairGlobalRegistrationIdempotencyOwnerGuardAndTagGuard)
		{
			using namespace VpSkeleton;

			Assert::AreEqual(std::string(VpMotion::kPairGlobalUnit), SkeletonMemo::OwnerUnitOf(kP7CameraFit),
				L"kP7CameraFit catalog owner must match kPairGlobalUnit");

			SkeletonMemo memo;
			memo.SetRenderThread(std::this_thread::get_id());

			// Negative control: registering kP7CameraFit under non-owning units (including sibling "motion/estimation") is refused (NotOwner).
			Assert::IsTrue(memo.RegisterOwnerFill(kP7CameraFit, AcquireForm::Cpu, "wrong/unit",
				VpMotion::MakeP7PairGlobalCpuOwnerFill()) == SkeletonMemo::Registration::NotOwner,
				L"wrong unit must be refused for kP7CameraFit CPU");
			Assert::IsTrue(memo.RegisterOwnerFill(kP7CameraFit, AcquireForm::Gpu, "motion/estimation",
				VpMotion::MakeP7PairGlobalGpuOwnerFill()) == SkeletonMemo::Registration::NotOwner,
				L"sibling motion/estimation unit must be refused for kP7CameraFit GPU");
			Assert::IsFalse(memo.HasOwnerFill(kP7CameraFit, AcquireForm::Cpu));
			Assert::IsFalse(memo.HasOwnerFill(kP7CameraFit, AcquireForm::Gpu));

			// Negative control: refusing IExternalDemand (NotInCatalog) -> TryRegisterPairGlobalFills returns false.
			FakePairGlobalDemand refusingDemand(nullptr);
			Assert::IsFalse(VpMotion::TryRegisterPairGlobalFills(&refusingDemand),
				L"TryRegisterPairGlobalFills must return false when demand refuses registration");
			Assert::AreEqual(2, refusingDemand.registerCalls, L"attempted both CPU and GPU registrations");

			// Positive path + idempotency: memo-backed IExternalDemand registers both CPU & GPU fills once and stays true on repeat.
			int sentinelMem[2] = { 17, 29 };
			const pl_tex fakeTex = reinterpret_cast<pl_tex>(&sentinelMem[0]);
			const VpMotion::PairGlobal expectedRec = MakePairGlobalSample(VpMotion::PairGlobalSource::GapSeed, true, 88u, 3.75f);

			auto cpuFn = [expectedRec](const OwnerFillRequest&, SkeletonMemo&, VpMotion::PairGlobal& out, std::string&) {
				out = expectedRec;
				return true;
			};
			auto gpuFn = [fakeTex, expectedRec](const OwnerFillRequest&, SkeletonMemo&, VpMotion::PairGlobalGpuView& out, std::string&) {
				out.globalVector = fakeTex;
				out.record = expectedRec;
				return true;
			};

			FakePairGlobalDemand memoDemand(&memo);
			Assert::IsTrue(VpMotion::TryRegisterPairGlobalFills(&memoDemand, cpuFn, gpuFn), L"first registration succeeds");
			Assert::IsTrue(memo.HasOwnerFill(kP7CameraFit, AcquireForm::Cpu));
			Assert::IsTrue(memo.HasOwnerFill(kP7CameraFit, AcquireForm::Gpu));
			Assert::IsTrue(VpMotion::TryRegisterPairGlobalFills(&memoDemand, cpuFn, gpuFn), L"duplicate registration is idempotent");
			Assert::AreEqual(4, memoDemand.registerCalls);

			VpMotion::PairGlobal gotCpu;
			AcquireResult resCpu;
			FallbackCatalog::Decision fbCpu;
			Assert::IsTrue(VpMotion::AcquirePairGlobalCpu(memo, 30, gotCpu, &resCpu, &fbCpu));
			Assert::IsTrue(resCpu.status == AcquireStatus::FilledAtRead && SamePairGlobal(expectedRec, gotCpu) && !fbCpu.engaged);

			VpMotion::PairGlobalGpuView gotGpu;
			AcquireResult resGpu;
			FallbackCatalog::Decision fbGpu;
			Assert::IsTrue(VpMotion::AcquirePairGlobalGpu(memo, 30, gotGpu, &resGpu, &fbGpu));
			Assert::IsTrue(resGpu.status == AcquireStatus::FilledAtRead && gotGpu.globalVector == fakeTex && !fbGpu.engaged);
			Assert::IsNotNull(VpMotion::PairGlobalGpuOf(resGpu), L"real tagged GPU result yields non-null PairGlobalGpuView");

			// Tag guard negative controls: foreign textureSet tag and Unavailable result with matching tag both yield nullptr.
			AcquireResult foreign;
			foreign.status = AcquireStatus::FilledAtRead;
			foreign.gpu.resource = const_cast<void*>(static_cast<const void*>(fakeTex));
			foreign.gpu.textureSet = &sentinelMem[1];
			foreign.gpu.keepAlive = std::make_shared<int>(12345);
			Assert::IsNull(VpMotion::PairGlobalGpuOf(foreign), L"PairGlobalGpuOf must reject foreign textureSet tag");

			AcquireResult unavailableTagged;
			unavailableTagged.status = AcquireStatus::Unavailable;
			unavailableTagged.gpu.textureSet = VpMotion::PairGlobalGpuTag();
			unavailableTagged.gpu.keepAlive = std::make_shared<VpMotion::PairGlobalGpuView>();
			Assert::IsNull(VpMotion::PairGlobalGpuOf(unavailableTagged), L"PairGlobalGpuOf must reject Unavailable results");
		}
	};

	namespace
	{
		struct PairGlobalDispatchCall
		{
			std::string description;
			std::string dstName;
			pl_desc_type dstType = PL_DESC_INVALID;
			pl_tex dstTex = nullptr;
			pl_tex lastDstTex = nullptr;
			int numDescriptors = 0;
			std::string varName;
			bool hasVec4 = false;
			float vec4[4] = {};
			int w = 0;
			int h = 0;
			int threadsX = 0;
			int threadsY = 0;
		};

		struct PairGlobalDispatchRecorder
		{
			std::vector<PairGlobalDispatchCall> calls;
			int failOnCall = 0;
			std::string* errorSink = nullptr;
			std::string failError;

			void Reset(int failCall = 0, std::string* sink = nullptr, const char* err = nullptr)
			{
				calls.clear();
				failOnCall = failCall;
				errorSink = sink;
				failError = err ? err : "";
			}

			VpMotion::MatcherGlobalD1::DispatchFn MakeDispatch()
			{
				return [this](const char* description, const char*, const char*,
					const pl_shader_desc* descriptors, int numDescriptors,
					const pl_shader_var* variables, int numVariables,
					int w, int h, int group) -> bool
				{
					PairGlobalDispatchCall c;
					c.description = description ? description : "";
					c.numDescriptors = numDescriptors;
					if (descriptors != nullptr && numDescriptors >= 1)
					{
						c.dstName = descriptors[0].desc.name ? descriptors[0].desc.name : "";
						c.dstType = descriptors[0].desc.type;
						c.dstTex = static_cast<pl_tex>(descriptors[0].binding.object);
						c.lastDstTex = static_cast<pl_tex>(descriptors[numDescriptors - 1].binding.object);
					}
					if (variables != nullptr && numVariables >= 1 && variables[0].data != nullptr)
					{
						c.varName = variables[0].var.name ? variables[0].var.name : "";
						std::memcpy(c.vec4, variables[0].data, sizeof(c.vec4));
						c.hasVec4 = true;
					}
					c.w = w;
					c.h = h;
					c.threadsX = group;
					c.threadsY = group;
					calls.push_back(c);
					if (failOnCall > 0 && static_cast<int>(calls.size()) == failOnCall)
					{
						if (errorSink != nullptr && !failError.empty())
							*errorSink = failError;
						return false;
					}
					return true;
				};
			}

			VpMotion::MatcherGlobalD1::DispatchPerBlockFn MakeDispatchPerBlock()
			{
				return [this](const char* description, const char*, const char*,
					const pl_shader_desc* descriptors, int numDescriptors,
					const pl_shader_var* variables, int numVariables,
					int w, int h, int threadsX, int threadsY) -> bool
				{
					PairGlobalDispatchCall c;
					c.description = description ? description : "";
					c.numDescriptors = numDescriptors;
					if (descriptors != nullptr && numDescriptors >= 1)
					{
						c.dstName = descriptors[0].desc.name ? descriptors[0].desc.name : "";
						c.dstType = descriptors[0].desc.type;
						c.dstTex = static_cast<pl_tex>(descriptors[0].binding.object);
						c.lastDstTex = static_cast<pl_tex>(descriptors[numDescriptors - 1].binding.object);
					}
					if (variables != nullptr && numVariables >= 1 && variables[0].data != nullptr)
					{
						c.varName = variables[0].var.name ? variables[0].var.name : "";
						std::memcpy(c.vec4, variables[0].data, sizeof(c.vec4));
						c.hasVec4 = true;
					}
					c.w = w;
					c.h = h;
					c.threadsX = threadsX;
					c.threadsY = threadsY;
					calls.push_back(c);
					if (failOnCall > 0 && static_cast<int>(calls.size()) == failOnCall)
					{
						if (errorSink != nullptr && !failError.empty())
							*errorSink = failError;
						return false;
					}
					return true;
				};
			}
		};

		bool Vec4AllZero(const float v[4])
		{
			const float zero[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
			return std::memcmp(v, zero, sizeof(zero)) == 0;
		}

		bool PairVecAllZero(const float v[2][4])
		{
			return Vec4AllZero(v[0]) && Vec4AllZero(v[1]);
		}

		MotionAhead::FlowPtr MakeClearGlobalFlow(float dx, float dy, bool globalSet)
		{
			MotionAhead::FlowPtr flow = MakeFlow(1.0f, globalSet, false);
			flow->global[0][0] = dx;
			flow->global[0][1] = dy;
			flow->global[0][2] = 0.25f;
			flow->global[0][3] = 1.0f;
			flow->global[1][0] = -dx;
			flow->global[1][1] = -dy;
			flow->global[1][2] = 0.30f;
			flow->global[1][3] = 1.0f;
			return flow;
		}
	}

	// M1#4 (U10 C3, PairGlobalFill.h): pure CPU selection branch precedence, LoadEngineGlobalFromP2 strict memo vs direct
	// fallback contract, ClearPairGlobalGpu, and ExecutePairGlobalGpu constant-write dispatch parity.
	TEST_CLASS(PairGlobalFillBranchOrderTests)
	{
	public:
		// Strict precedence cascade: cutOrFlash > CameraModel > Engine (clear) > GapSeed (holds) > MatcherD1 > Cleared,
		// with same-input negative controls for OpticalFlowTrust::GlobalUsable / opticalFlowGlobalFallback and gapSeedHolds rejection.
		TEST_METHOD(StrictPrecedenceCascadeAndNegativeControls)
		{
			using namespace VpMotion;

			int gapSeedHoldsCalls = 0;
			bool gapSeedHoldsReturn = true;

			PairGlobalSelectionInput base;
			base.cutOrFlash = true;
			base.globalPrior = true;
			base.useModelGlobal = true;
			base.modelFwd[0] = 16.0f;
			base.modelFwd[1] = -24.0f;
			base.modelBwd[0] = -8.0f;
			base.modelBwd[1] = 32.0f;
			base.modelInputScale = 0.5f;   // exact dyadic D1 scaling: fwd = {1.0, -1.5, 0.5, 1.0}, bwd = {-0.5, 2.0, 0.5, 1.0}
			base.opticalFlowGlobal = true;
			base.opticalFlowGlobalFallback = true;
			base.seeded = true;
			base.engineValid = true;
			base.engineEpoch = 42u;
			base.engineVec[0][0] = 2.25f;  base.engineVec[0][1] = -1.75f; base.engineVec[0][2] = 0.35f; base.engineVec[0][3] = 1.0f;
			base.engineVec[1][0] = -2.25f; base.engineVec[1][1] = 1.75f;  base.engineVec[1][2] = 0.40f; base.engineVec[1][3] = 1.0f;
			base.gapSeedCandidate = true;
			base.gapSeedVec[0][0] = 3.5f;  base.gapSeedVec[0][1] = 0.75f;  base.gapSeedVec[0][2] = 0.25f; base.gapSeedVec[0][3] = 1.0f;
			base.gapSeedVec[1][0] = -3.5f; base.gapSeedVec[1][1] = -0.75f; base.gapSeedVec[1][2] = 0.30f; base.gapSeedVec[1][3] = 1.0f;
			base.gapSeedHolds = [&]() -> bool {
				++gapSeedHoldsCalls;
				return gapSeedHoldsReturn;
			};

			Assert::IsTrue(OpticalFlowTrust::GlobalUsable(base.engineVec[0]), L"precondition: base engine fwd vector is clear");
			Assert::IsTrue(OpticalFlowTrust::GlobalUsable(base.engineVec[1]), L"precondition: base engine bwd vector is clear");

			auto runSelect = [&](const PairGlobalSelectionInput& in, PairGlobal& rec, PairGlobalSelectionOutcome& out)
			{
				// Pre-dirty outputs to verify SelectPairGlobal resets every field on every call.
				rec = MakePairGlobalSample(PairGlobalSource::MatcherD1, true, 9999u, 19.0f);
				out.source = PairGlobalSource::MatcherD1;
				out.modelGlobalUsed = true;
				out.globalFromEngine = true;
				out.engineGlobalFallback = true;
				out.searchSeedUsed = true;
				out.searchSeedRejected = true;
				out.needsD1Search = true;
				out.clearSecond = true;
				gapSeedHoldsCalls = 0;
				SelectPairGlobal(in, rec, out);
			};

			PairGlobal rec;
			PairGlobalSelectionOutcome out;

			// Step 1: cutOrFlash = true wins over EVERY candidate (CameraModel, Engine, GapSeed, MatcherD1).
			{
				runSelect(base, rec, out);
				Assert::IsTrue(out.source == PairGlobalSource::Cleared, L"Step 1: cutOrFlash -> Cleared");
				Assert::IsTrue(rec.src == PairGlobalSource::Cleared);
				Assert::IsFalse(rec.valid, L"Step 1: cutOrFlash record is invalid");
				Assert::AreEqual(uint64_t(0), rec.p2Epoch);
				Assert::IsTrue(out.clearSecond, L"Step 1: cutOrFlash clears second");
				Assert::IsFalse(out.modelGlobalUsed);
				Assert::IsFalse(out.globalFromEngine);
				Assert::IsFalse(out.engineGlobalFallback);
				Assert::IsFalse(out.searchSeedUsed);
				Assert::IsFalse(out.searchSeedRejected);
				Assert::IsFalse(out.needsD1Search);
				Assert::AreEqual(0, gapSeedHoldsCalls, L"Step 1: cutOrFlash must not invoke gapSeedHolds");
				Assert::IsTrue(PairVecAllZero(rec.vec), L"Step 1: vec zeroed");
				Assert::IsTrue(PairVecAllZero(rec.second), L"Step 1: second zeroed");
			}

			// Step 2: drop cutOrFlash = false on exact same input -> CameraModel wins.
			PairGlobalSelectionInput step2 = base;
			step2.cutOrFlash = false;
			{
				runSelect(step2, rec, out);
				Assert::IsTrue(out.source == PairGlobalSource::CameraModel, L"Step 2: CameraModel wins");
				Assert::IsTrue(rec.src == PairGlobalSource::CameraModel);
				Assert::IsTrue(rec.valid);
				Assert::AreEqual(uint64_t(0), rec.p2Epoch);
				Assert::IsTrue(out.modelGlobalUsed);
				Assert::IsTrue(out.clearSecond);
				Assert::IsFalse(out.globalFromEngine);
				Assert::IsFalse(out.engineGlobalFallback);
				Assert::IsFalse(out.searchSeedUsed);
				Assert::IsFalse(out.searchSeedRejected);
				Assert::IsFalse(out.needsD1Search);
				Assert::AreEqual(0, gapSeedHoldsCalls, L"Step 2: CameraModel must not invoke gapSeedHolds");
				Assert::AreEqual(1.0f, rec.vec[0][0], 0.0f, L"Step 2: fwd x = modelFwd[0] * scale / 8");
				Assert::AreEqual(-1.5f, rec.vec[0][1], 0.0f, L"Step 2: fwd y = modelFwd[1] * scale / 8");
				Assert::AreEqual(0.5f, rec.vec[0][2], 0.0f, L"Step 2: fwd z = 0.5");
				Assert::AreEqual(1.0f, rec.vec[0][3], 0.0f, L"Step 2: fwd w = 1.0");
				Assert::AreEqual(-0.5f, rec.vec[1][0], 0.0f, L"Step 2: bwd x = modelBwd[0] * scale / 8");
				Assert::AreEqual(2.0f, rec.vec[1][1], 0.0f, L"Step 2: bwd y = modelBwd[1] * scale / 8");
				Assert::AreEqual(0.5f, rec.vec[1][2], 0.0f, L"Step 2: bwd z = 0.5");
				Assert::AreEqual(1.0f, rec.vec[1][3], 0.0f, L"Step 2: bwd w = 1.0");
				Assert::IsTrue(PairVecAllZero(rec.second), L"Step 2: second zeroed");

				// Same-input check: even when engineVec is ambiguous, CameraModel suppresses engineGlobalFallback.
				PairGlobalSelectionInput step2Ambig = step2;
				step2Ambig.engineVec[1][2] = 0.95f;
				Assert::IsFalse(OpticalFlowTrust::GlobalUsable(step2Ambig.engineVec[1]));
				runSelect(step2Ambig, rec, out);
				Assert::IsTrue(out.source == PairGlobalSource::CameraModel);
				Assert::IsTrue(out.modelGlobalUsed);
				Assert::IsFalse(out.engineGlobalFallback, L"Step 2: CameraModel must suppress engineGlobalFallback");
				Assert::AreEqual(0, gapSeedHoldsCalls);
			}

			// Step 3: drop useModelGlobal = false on exact same input -> Engine wins (clear engineVec).
			PairGlobalSelectionInput step3 = step2;
			step3.useModelGlobal = false;
			{
				runSelect(step3, rec, out);
				Assert::IsTrue(out.source == PairGlobalSource::Engine, L"Step 3: clear Engine wins");
				Assert::IsTrue(rec.src == PairGlobalSource::Engine);
				Assert::IsTrue(rec.valid);
				Assert::AreEqual(uint64_t(42), rec.p2Epoch, L"Step 3: p2Epoch propagated from engineEpoch");
				Assert::IsTrue(out.globalFromEngine);
				Assert::IsFalse(out.clearSecond, L"Step 3: Engine must NOT clear globalSecond");
				Assert::IsFalse(out.modelGlobalUsed);
				Assert::IsFalse(out.engineGlobalFallback);
				Assert::IsFalse(out.searchSeedUsed);
				Assert::IsFalse(out.searchSeedRejected);
				Assert::IsFalse(out.needsD1Search);
				Assert::AreEqual(0, gapSeedHoldsCalls, L"Step 3: Engine must not invoke gapSeedHolds");
				Assert::AreEqual(0, std::memcmp(rec.vec, step3.engineVec, sizeof(rec.vec)), L"Step 3: vec bit-equal to engineVec");
				Assert::IsTrue(PairVecAllZero(rec.second));
			}

			// Step 4: Same-input negative control for OpticalFlowTrust::GlobalUsable / opticalFlowGlobalFallback.
			{
				PairGlobalSelectionInput step4 = step3;
				step4.engineVec[1][2] = 0.95f;   // ambiguous (>= 0.9) on exact same input
				Assert::IsTrue(OpticalFlowTrust::GlobalUsable(step4.engineVec[0]));
				Assert::IsFalse(OpticalFlowTrust::GlobalUsable(step4.engineVec[1]), L"precondition: bwd engineVec is ambiguous");

				// 4a: opticalFlowGlobalFallback = true -> Engine rejected, engineGlobalFallback raised, falls through to GapSeed.
				step4.opticalFlowGlobalFallback = true;
				runSelect(step4, rec, out);
				Assert::IsTrue(out.engineGlobalFallback, L"Step 4a: ambiguous engine raises engineGlobalFallback");
				Assert::IsFalse(out.globalFromEngine, L"Step 4a: ambiguous engine rejected");
				Assert::AreEqual(1, gapSeedHoldsCalls, L"Step 4a: gapSeedHolds invoked once on fallthrough");
				Assert::IsTrue(out.source == PairGlobalSource::GapSeed, L"Step 4a: falls through to GapSeed");
				Assert::IsTrue(rec.src == PairGlobalSource::GapSeed);
				Assert::IsTrue(rec.valid);
				Assert::AreEqual(uint64_t(0), rec.p2Epoch);
				Assert::IsTrue(out.searchSeedUsed);
				Assert::IsFalse(out.searchSeedRejected);
				Assert::IsTrue(out.clearSecond);
				Assert::IsFalse(out.needsD1Search);
				Assert::AreEqual(0, std::memcmp(rec.vec, step4.gapSeedVec, sizeof(rec.vec)), L"Step 4a: vec bit-equal to gapSeedVec");
				Assert::IsTrue(PairVecAllZero(rec.second));

				// 4b: opticalFlowGlobalFallback = false on EXACT SAME ambiguous engineVec -> Engine accepted.
				step4.opticalFlowGlobalFallback = false;
				runSelect(step4, rec, out);
				Assert::IsTrue(out.source == PairGlobalSource::Engine, L"Step 4b: fallback disabled accepts ambiguous engine");
				Assert::IsTrue(rec.src == PairGlobalSource::Engine);
				Assert::IsTrue(rec.valid);
				Assert::AreEqual(uint64_t(42), rec.p2Epoch);
				Assert::IsTrue(out.globalFromEngine);
				Assert::IsFalse(out.engineGlobalFallback, L"Step 4b: no engineGlobalFallback when fallback disabled");
				Assert::IsFalse(out.clearSecond);
				Assert::AreEqual(0, gapSeedHoldsCalls, L"Step 4b: gapSeedHolds not called when engine accepted");
				Assert::AreEqual(0, std::memcmp(rec.vec, step4.engineVec, sizeof(rec.vec)));

				// 4c: validity-channel boundary (w == 0.5 <= 0.5 with clear z == 0.2) fails GlobalUsable when fallback enabled.
				PairGlobalSelectionInput step4LowW = step3;
				step4LowW.engineVec[0][2] = 0.20f;
				step4LowW.engineVec[0][3] = 0.50f;
				Assert::IsFalse(OpticalFlowTrust::GlobalUsable(step4LowW.engineVec[0]));
				step4LowW.opticalFlowGlobalFallback = true;
				runSelect(step4LowW, rec, out);
				Assert::IsTrue(out.engineGlobalFallback && out.source == PairGlobalSource::GapSeed && gapSeedHoldsCalls == 1,
					L"Step 4c: w <= 0.5 rejected when opticalFlowGlobalFallback = true");
				step4LowW.opticalFlowGlobalFallback = false;
				runSelect(step4LowW, rec, out);
				Assert::IsTrue(!out.engineGlobalFallback && out.source == PairGlobalSource::Engine && gapSeedHoldsCalls == 0,
					L"Step 4c: w <= 0.5 accepted when opticalFlowGlobalFallback = false");

				// 4d: engine eligibility gates (opticalFlowGlobal = false, seeded = false) suppress engineGlobalFallback even on ambiguous vec.
				PairGlobalSelectionInput step4NotSeeded = step3;
				step4NotSeeded.engineVec[1][2] = 0.95f;
				step4NotSeeded.seeded = false;
				runSelect(step4NotSeeded, rec, out);
				Assert::IsFalse(out.engineGlobalFallback, L"Step 4d: unseeded engine must not set engineGlobalFallback");
				Assert::IsTrue(out.source == PairGlobalSource::GapSeed && gapSeedHoldsCalls == 1);

				PairGlobalSelectionInput step4FlowOff = step3;
				step4FlowOff.engineVec[1][2] = 0.95f;
				step4FlowOff.opticalFlowGlobal = false;
				runSelect(step4FlowOff, rec, out);
				Assert::IsFalse(out.engineGlobalFallback, L"Step 4d: opticalFlowGlobal = false must not set engineGlobalFallback");
				Assert::IsTrue(out.source == PairGlobalSource::GapSeed && gapSeedHoldsCalls == 1);
			}

			// Step 5: Same-input negative control for gapSeedHolds rejection -> falls through to MatcherD1.
			{
				PairGlobalSelectionInput step5 = step3;
				step5.engineValid = false;
				gapSeedHoldsReturn = false;
				runSelect(step5, rec, out);
				Assert::AreEqual(1, gapSeedHoldsCalls, L"Step 5: gapSeedHolds invoked once");
				Assert::IsTrue(out.searchSeedRejected, L"Step 5: searchSeedRejected set");
				Assert::IsFalse(out.searchSeedUsed, L"Step 5: searchSeedUsed false");
				Assert::IsTrue(out.source == PairGlobalSource::MatcherD1, L"Step 5: rejected GapSeed falls through to MatcherD1");
				Assert::IsTrue(rec.src == PairGlobalSource::MatcherD1);
				Assert::IsFalse(rec.valid);
				Assert::IsTrue(out.needsD1Search);
				Assert::IsFalse(out.clearSecond);
				Assert::IsFalse(out.engineGlobalFallback);

				// Combined fallthrough: ambiguous engine + rejected GapSeed keeps both engineGlobalFallback and searchSeedRejected.
				PairGlobalSelectionInput step5Both = step3;
				step5Both.engineVec[1][2] = 0.95f;
				step5Both.opticalFlowGlobalFallback = true;
				gapSeedHoldsReturn = false;
				runSelect(step5Both, rec, out);
				Assert::AreEqual(1, gapSeedHoldsCalls);
				Assert::IsTrue(out.engineGlobalFallback && out.searchSeedRejected && out.needsD1Search && out.source == PairGlobalSource::MatcherD1,
					L"Step 5: ambiguous engine + rejected gap seed both visible on MatcherD1 fallthrough");

				gapSeedHoldsReturn = true;
			}

			// Step 6: gapSeedCandidate = false, globalPrior = true -> MatcherD1 without calling gapSeedHolds.
			{
				PairGlobalSelectionInput step6 = step3;
				step6.engineValid = false;
				step6.gapSeedCandidate = false;
				runSelect(step6, rec, out);
				Assert::IsTrue(out.source == PairGlobalSource::MatcherD1, L"Step 6: MatcherD1 selected");
				Assert::IsTrue(rec.src == PairGlobalSource::MatcherD1);
				Assert::IsFalse(rec.valid);
				Assert::IsTrue(out.needsD1Search);
				Assert::IsFalse(out.searchSeedRejected);
				Assert::IsFalse(out.searchSeedUsed);
				Assert::IsFalse(out.clearSecond);
				Assert::AreEqual(0, gapSeedHoldsCalls, L"Step 6: gapSeedHolds not called when gapSeedCandidate = false");
			}

			// Step 7: globalPrior = false (with cutOrFlash = false and all other candidates enabled, even ambiguous engineVec) -> Cleared.
			{
				PairGlobalSelectionInput step7 = step2;
				step7.globalPrior = false;
				step7.engineVec[1][2] = 0.95f;
				runSelect(step7, rec, out);
				Assert::IsTrue(out.source == PairGlobalSource::Cleared, L"Step 7: !globalPrior -> Cleared");
				Assert::IsTrue(rec.src == PairGlobalSource::Cleared);
				Assert::IsFalse(rec.valid);
				Assert::IsFalse(out.clearSecond, L"Step 7: !globalPrior does not request clearSecond");
				Assert::IsFalse(out.needsD1Search);
				Assert::IsFalse(out.modelGlobalUsed);
				Assert::IsFalse(out.globalFromEngine);
				Assert::IsFalse(out.engineGlobalFallback);
				Assert::IsFalse(out.searchSeedUsed);
				Assert::IsFalse(out.searchSeedRejected);
				Assert::AreEqual(0, gapSeedHoldsCalls, L"Step 7: !globalPrior must not call gapSeedHolds");
				Assert::IsTrue(PairVecAllZero(rec.vec));
				Assert::IsTrue(PairVecAllZero(rec.second));
			}
		}

		// LoadEngineGlobalFromP2 strict contract: memo != nullptr reads kP2VectorPair ONLY (never silently falls back to
		// directFallback on miss/decline/unset); memo == nullptr reads directFallback when GlobalSet() is true.
		TEST_METHOD(LoadEngineGlobalFromP2MemoVsDirectFallbackContract)
		{
			using namespace VpMotion;
			using namespace VpSkeleton;

			// Real VectorEngine + P2CpuForm serving seq 11 with generation 1 and clear global vectors.
			VectorEngine engine;
			engine.PublishProducer(std::make_shared<MotionAhead::Producer>());
			engine.p2Slot->SetGeneration(engine.motionValidity.StreamGeneration());
			const MotionAhead::FlowPtr memoFlow = MakeClearGlobalFlow(2.5f, -1.25f, /*globalSet=*/true);
			Assert::IsTrue(engine.p2Slot->Publish(10, 11, engine.p2Slot->Generation(), memoFlow));

			SkeletonMemo memo;
			IVectorEngine* enginePtr = &engine;
			Assert::IsTrue(RegisterFill(memo, [enginePtr](const void*) { return enginePtr; }));

			// Distinct directFallback with completely different values and epoch so any silent leak is immediately caught.
			const MotionAhead::FlowPtr directFlow = MakeClearGlobalFlow(-6.0f, 4.5f, /*globalSet=*/true);
			const P2PairRecord directFallback = MakeP2PairRecord(10, 11, 77u, directFlow->global, directFlow->second, true, false);
			Assert::IsTrue(directFallback.GlobalSet());
			Assert::IsTrue(std::memcmp(memoFlow->global, directFallback.global, sizeof(memoFlow->global)) != 0,
				L"precondition: memo global differs from directFallback global");

			auto dirtyEngineFields = [](PairGlobalSelectionInput& in)
			{
				in.engineValid = true;
				in.engineEpoch = 999u;
				for (int d = 0; d < 2; ++d)
					for (int k = 0; k < 4; ++k)
						in.engineVec[d][k] = -88.0f;
			};

			// Case 1: memo != nullptr hit -> populates from memo kP2VectorPair and ignores directFallback.
			{
				PairGlobalSelectionInput in;
				dirtyEngineFields(in);
				Assert::IsTrue(LoadEngineGlobalFromP2(&memo, 11, &directFallback, in), L"memo hit returns true");
				Assert::IsTrue(in.engineValid);
				Assert::AreEqual(uint64_t(1), in.engineEpoch, L"epoch must come from memo P2PairRecord, not directFallback");
				Assert::AreEqual(0, std::memcmp(in.engineVec, memoFlow->global, sizeof(in.engineVec)),
					L"engineVec must match memo P2PairRecord bit-for-bit, ignoring directFallback");
			}

			// Case 2a (Negative control — no silent fallback when memo has no kP2VectorPair fill registered):
			{
				SkeletonMemo emptyMemo;
				PairGlobalSelectionInput in;
				dirtyEngineFields(in);
				Assert::IsFalse(LoadEngineGlobalFromP2(&emptyMemo, 11, &directFallback, in),
					L"negative control: memo without P2 fill must NOT silently read directFallback");
				Assert::IsFalse(in.engineValid, L"engineValid must be false on memo miss");
				Assert::AreEqual(uint64_t(0), in.engineEpoch, L"stale engineEpoch must be cleared");
				Assert::IsTrue(PairVecAllZero(in.engineVec), L"stale engineVec must be cleared");
			}

			// Case 2b (Negative control — no silent fallback when memo P2 fill declines missing seq 12):
			{
				PairGlobalSelectionInput in;
				dirtyEngineFields(in);
				Assert::IsFalse(LoadEngineGlobalFromP2(&memo, 12, &directFallback, in),
					L"negative control: memo OwnerDeclined must NOT silently read directFallback");
				Assert::IsFalse(in.engineValid);
				Assert::AreEqual(uint64_t(0), in.engineEpoch);
				Assert::IsTrue(PairVecAllZero(in.engineVec));
			}

			// Case 2c (Negative control — no silent fallback when memo P2 record has GlobalSet() == false):
			{
				VectorEngine unsetEngine;
				unsetEngine.PublishProducer(std::make_shared<MotionAhead::Producer>());
				unsetEngine.p2Slot->SetGeneration(unsetEngine.motionValidity.StreamGeneration());
				Assert::IsTrue(unsetEngine.p2Slot->Publish(10, 11, unsetEngine.p2Slot->Generation(),
					MakeClearGlobalFlow(2.5f, -1.25f, /*globalSet=*/false)));
				SkeletonMemo unsetMemo;
				IVectorEngine* unsetPtr = &unsetEngine;
				Assert::IsTrue(RegisterFill(unsetMemo, [unsetPtr](const void*) { return unsetPtr; }));

				PairGlobalSelectionInput in;
				dirtyEngineFields(in);
				Assert::IsFalse(LoadEngineGlobalFromP2(&unsetMemo, 11, &directFallback, in),
					L"negative control: memo GlobalSet() == false must NOT silently read directFallback");
				Assert::IsFalse(in.engineValid);
				Assert::AreEqual(uint64_t(0), in.engineEpoch);
				Assert::IsTrue(PairVecAllZero(in.engineVec));
			}

			// Case 3: memo == nullptr (standalone unit-test path) reads directFallback iff GlobalSet() is true.
			{
				PairGlobalSelectionInput in;
				dirtyEngineFields(in);
				Assert::IsTrue(LoadEngineGlobalFromP2(nullptr, 11, &directFallback, in),
					L"standalone path (memo == nullptr) reads valid directFallback");
				Assert::IsTrue(in.engineValid);
				Assert::AreEqual(uint64_t(77), in.engineEpoch);
				Assert::AreEqual(0, std::memcmp(in.engineVec, directFallback.global, sizeof(in.engineVec)));

				P2PairRecord unsetDirect = directFallback;
				unsetDirect.flags &= ~kP2PairFlagGlobalSet;
				Assert::IsFalse(unsetDirect.GlobalSet());
				dirtyEngineFields(in);
				Assert::IsFalse(LoadEngineGlobalFromP2(nullptr, 11, &unsetDirect, in),
					L"standalone path rejects directFallback with GlobalSet() == false");
				Assert::IsFalse(in.engineValid);
				Assert::AreEqual(uint64_t(0), in.engineEpoch);
				Assert::IsTrue(PairVecAllZero(in.engineVec));

				dirtyEngineFields(in);
				Assert::IsFalse(LoadEngineGlobalFromP2(nullptr, 11, nullptr, in),
					L"standalone path with null directFallback returns false");
				Assert::IsFalse(in.engineValid);
				Assert::AreEqual(uint64_t(0), in.engineEpoch);
				Assert::IsTrue(PairVecAllZero(in.engineVec));
			}
		}

		// ClearPairGlobalGpu exact pass label / null-skipping / failure propagation, and ExecutePairGlobalGpu constant-write
		// dispatch counts, labels, and gvec payloads for CameraModel, Engine, GapSeed, and cutOrFlash.
		TEST_METHOD(ClearPairGlobalGpuAndConstantWriteDispatchParity)
		{
			using namespace VpMotion;

			int sentinels[4] = { 11, 22, 33, 44 };
			pl_tex vecTex[2] = { reinterpret_cast<pl_tex>(&sentinels[0]), reinterpret_cast<pl_tex>(&sentinels[1]) };
			pl_tex secTex[2] = { reinterpret_cast<pl_tex>(&sentinels[2]), reinterpret_cast<pl_tex>(&sentinels[3]) };

			const char* const kClearLabel = "VideoProcessor de-judder: global motion cleared (camera model: cut / flash)";
			PairGlobalDispatchRecorder recMock;

			// ClearPairGlobalGpu: all 4 non-null textures dispatched in exact order with exact label and zero fillv.
			{
				recMock.Reset();
				PairGlobal outRec = MakePairGlobalSample(PairGlobalSource::Engine, true, 5u, 2.0f);
				Assert::IsTrue(ClearPairGlobalGpu(vecTex, secTex, recMock.MakeDispatch(), outRec));
				Assert::IsTrue(outRec.src == PairGlobalSource::Cleared);
				Assert::IsFalse(outRec.valid);
				Assert::AreEqual(size_t(4), recMock.calls.size(), L"all 4 non-null textures cleared");
				const pl_tex expectedTargets[4] = { vecTex[0], vecTex[1], secTex[0], secTex[1] };
				for (size_t i = 0; i < 4; ++i)
				{
					Assert::AreEqual(std::string(kClearLabel), recMock.calls[i].description, L"exact cut/flash clear pass label");
					Assert::AreEqual(std::string("dst"), recMock.calls[i].dstName);
					Assert::IsTrue(recMock.calls[i].dstType == PL_DESC_STORAGE_IMG);
					Assert::IsTrue(recMock.calls[i].dstTex == expectedTargets[i], L"exact clear target order");
					Assert::AreEqual(std::string("fillv"), recMock.calls[i].varName);
					Assert::IsTrue(recMock.calls[i].hasVec4 && Vec4AllZero(recMock.calls[i].vec4), L"fillv is all zero");
					Assert::AreEqual(1, recMock.calls[i].w);
					Assert::AreEqual(1, recMock.calls[i].h);
					Assert::AreEqual(1, recMock.calls[i].threadsX);
				}
			}

			// ClearPairGlobalGpu: skips null entries without dispatching them.
			{
				recMock.Reset();
				pl_tex partialVec[2] = { vecTex[0], nullptr };
				pl_tex partialSec[2] = { nullptr, secTex[1] };
				PairGlobal outRec;
				Assert::IsTrue(ClearPairGlobalGpu(partialVec, partialSec, recMock.MakeDispatch(), outRec));
				Assert::AreEqual(size_t(2), recMock.calls.size(), L"null textures skipped");
				Assert::IsTrue(recMock.calls[0].dstTex == vecTex[0]);
				Assert::IsTrue(recMock.calls[1].dstTex == secTex[1]);

				recMock.Reset();
				Assert::IsTrue(ClearPairGlobalGpu(vecTex, nullptr, recMock.MakeDispatch(), outRec));
				Assert::AreEqual(size_t(2), recMock.calls.size(), L"null globalSecond array skipped");
				Assert::IsTrue(recMock.calls[0].dstTex == vecTex[0]);
				Assert::IsTrue(recMock.calls[1].dstTex == vecTex[1]);
			}

			// ClearPairGlobalGpu negative controls: null callback and mid-clear dispatch failure return false immediately.
			{
				PairGlobal outRec;
				Assert::IsFalse(ClearPairGlobalGpu(vecTex, secTex, MatcherGlobalD1::DispatchFn(), outRec),
					L"empty dispatch callback returns false");
				Assert::IsTrue(outRec.src == PairGlobalSource::Cleared && !outRec.valid);

				recMock.Reset(/*failCall=*/2);
				Assert::IsFalse(ClearPairGlobalGpu(vecTex, secTex, recMock.MakeDispatch(), outRec),
					L"failed clear dispatch returns false");
				Assert::AreEqual(size_t(2), recMock.calls.size(), L"stops on first failing clear dispatch");
			}

			// ExecutePairGlobalGpu constant-write parity: CameraModel, Engine, GapSeed, cutOrFlash, and !globalPrior.
			PairGlobalGpuContext ctx;
			ctx.twoCandidates = true;
			ctx.globalVector = vecTex;
			ctx.globalSecond = secTex;
			ctx.dispatch = recMock.MakeDispatch();

			// CameraModel with twoCandidates = true -> 4 dispatches (vec0, sec0, vec1, sec1).
			{
				recMock.Reset();
				PairGlobalSelectionInput in;
				in.globalPrior = true;
				in.useModelGlobal = true;
				in.modelFwd[0] = 24.0f; in.modelFwd[1] = -16.0f;
				in.modelBwd[0] = -16.0f; in.modelBwd[1] = 8.0f;
				in.modelInputScale = 0.5f;   // fwd = {1.5, -1.0, 0.5, 1.0}, bwd = {-1.0, 0.5, 0.5, 1.0}

				PairGlobal outRec;
				PairGlobalSelectionOutcome outOutcome;
				FallbackCatalog::Decision outFb;
				std::string outErr = "stale";
				Assert::IsTrue(ExecutePairGlobalGpu(in, ctx, outRec, outOutcome, outFb, outErr));
				Assert::IsTrue(outErr.empty());
				Assert::IsFalse(outFb.engaged);
				Assert::AreEqual(std::string("motion.pair_global_unavailable"), outFb.id);
				Assert::AreEqual(size_t(4), recMock.calls.size(), L"CameraModel + twoCandidates dispatches 4 passes");

				Assert::AreEqual(std::string("VideoProcessor de-judder: global motion (camera model)"), recMock.calls[0].description);
				Assert::IsTrue(recMock.calls[0].dstTex == vecTex[0] && recMock.calls[0].varName == "gvec");
				Assert::AreEqual(0, std::memcmp(recMock.calls[0].vec4, outRec.vec[0], sizeof(outRec.vec[0])));

				Assert::AreEqual(std::string("VideoProcessor de-judder: global motion second (camera model)"), recMock.calls[1].description);
				Assert::IsTrue(recMock.calls[1].dstTex == secTex[0] && recMock.calls[1].varName == "gvec" && Vec4AllZero(recMock.calls[1].vec4));

				Assert::AreEqual(std::string("VideoProcessor de-judder: global motion (camera model)"), recMock.calls[2].description);
				Assert::IsTrue(recMock.calls[2].dstTex == vecTex[1] && recMock.calls[2].varName == "gvec");
				Assert::AreEqual(0, std::memcmp(recMock.calls[2].vec4, outRec.vec[1], sizeof(outRec.vec[1])));

				Assert::AreEqual(std::string("VideoProcessor de-judder: global motion second (camera model)"), recMock.calls[3].description);
				Assert::IsTrue(recMock.calls[3].dstTex == secTex[1] && recMock.calls[3].varName == "gvec" && Vec4AllZero(recMock.calls[3].vec4));

				// Same CameraModel input with twoCandidates = false -> 2 dispatches only (no second clear).
				PairGlobalGpuContext ctxOne = ctx;
				ctxOne.twoCandidates = false;
				recMock.Reset();
				Assert::IsTrue(ExecutePairGlobalGpu(in, ctxOne, outRec, outOutcome, outFb, outErr));
				Assert::AreEqual(size_t(2), recMock.calls.size(), L"CameraModel without twoCandidates dispatches 2 passes");
				Assert::IsTrue(recMock.calls[0].dstTex == vecTex[0] && recMock.calls[1].dstTex == vecTex[1]);

				// Negative control: failing second-candidate write aborts visibly with exact label in error and fallback.
				recMock.Reset(/*failCall=*/2);
				Assert::IsFalse(ExecutePairGlobalGpu(in, ctx, outRec, outOutcome, outFb, outErr));
				Assert::AreEqual(size_t(2), recMock.calls.size());
				Assert::IsTrue(Has(outErr, "dispatch failed: VideoProcessor de-judder: global motion second (camera model)"));
				Assert::IsTrue(outFb.engaged && outFb.id == "motion.pair_global_unavailable" && Has(outFb.reason, outErr.c_str()));
			}

			// Engine with twoCandidates = true -> 2 dispatches ONLY (Engine does NOT clear globalSecond!).
			{
				recMock.Reset();
				PairGlobalSelectionInput in;
				in.globalPrior = true;
				in.opticalFlowGlobal = true;
				in.seeded = true;
				in.engineValid = true;
				in.engineEpoch = 19u;
				in.engineVec[0][0] = 1.25f;  in.engineVec[0][1] = 0.5f;  in.engineVec[0][2] = 0.3f; in.engineVec[0][3] = 1.0f;
				in.engineVec[1][0] = -1.25f; in.engineVec[1][1] = -0.5f; in.engineVec[1][2] = 0.35f; in.engineVec[1][3] = 1.0f;

				PairGlobal outRec;
				PairGlobalSelectionOutcome outOutcome;
				FallbackCatalog::Decision outFb;
				std::string outErr;
				Assert::IsTrue(ExecutePairGlobalGpu(in, ctx, outRec, outOutcome, outFb, outErr));
				Assert::IsFalse(outFb.engaged);
				Assert::AreEqual(std::string("motion.pair_global_unavailable"), outFb.id);
				Assert::AreEqual(size_t(2), recMock.calls.size(), L"Engine must dispatch 2 passes only (never clears globalSecond)");
				Assert::AreEqual(std::string("VideoProcessor de-judder: global motion (engine)"), recMock.calls[0].description);
				Assert::IsTrue(recMock.calls[0].dstTex == vecTex[0] && recMock.calls[0].varName == "gvec");
				Assert::AreEqual(0, std::memcmp(recMock.calls[0].vec4, in.engineVec[0], sizeof(in.engineVec[0])));
				Assert::AreEqual(std::string("VideoProcessor de-judder: global motion (engine)"), recMock.calls[1].description);
				Assert::IsTrue(recMock.calls[1].dstTex == vecTex[1] && recMock.calls[1].varName == "gvec");
				Assert::AreEqual(0, std::memcmp(recMock.calls[1].vec4, in.engineVec[1], sizeof(in.engineVec[1])));
			}

			// GapSeed with twoCandidates = true -> 4 dispatches (vec0, sec0, vec1, sec1).
			{
				recMock.Reset();
				PairGlobalSelectionInput in;
				in.globalPrior = true;
				in.gapSeedCandidate = true;
				in.gapSeedVec[0][0] = -2.0f; in.gapSeedVec[0][1] = 1.5f;  in.gapSeedVec[0][2] = 0.2f; in.gapSeedVec[0][3] = 1.0f;
				in.gapSeedVec[1][0] = 2.0f;  in.gapSeedVec[1][1] = -1.5f; in.gapSeedVec[1][2] = 0.25f; in.gapSeedVec[1][3] = 1.0f;
				in.gapSeedHolds = []() { return true; };

				PairGlobal outRec;
				PairGlobalSelectionOutcome outOutcome;
				FallbackCatalog::Decision outFb;
				std::string outErr;
				Assert::IsTrue(ExecutePairGlobalGpu(in, ctx, outRec, outOutcome, outFb, outErr));
				Assert::IsFalse(outFb.engaged);
				Assert::AreEqual(std::string("motion.pair_global_unavailable"), outFb.id);
				Assert::AreEqual(size_t(4), recMock.calls.size(), L"GapSeed + twoCandidates dispatches 4 passes");
				Assert::AreEqual(std::string("VideoProcessor de-judder: global motion (gap seed)"), recMock.calls[0].description);
				Assert::IsTrue(recMock.calls[0].dstTex == vecTex[0] && std::memcmp(recMock.calls[0].vec4, in.gapSeedVec[0], sizeof(in.gapSeedVec[0])) == 0);
				Assert::AreEqual(std::string("VideoProcessor de-judder: global motion second (gap seed)"), recMock.calls[1].description);
				Assert::IsTrue(recMock.calls[1].dstTex == secTex[0] && Vec4AllZero(recMock.calls[1].vec4));
				Assert::AreEqual(std::string("VideoProcessor de-judder: global motion (gap seed)"), recMock.calls[2].description);
				Assert::IsTrue(recMock.calls[2].dstTex == vecTex[1] && std::memcmp(recMock.calls[2].vec4, in.gapSeedVec[1], sizeof(in.gapSeedVec[1])) == 0);
				Assert::AreEqual(std::string("VideoProcessor de-judder: global motion second (gap seed)"), recMock.calls[3].description);
				Assert::IsTrue(recMock.calls[3].dstTex == secTex[1] && Vec4AllZero(recMock.calls[3].vec4));
			}

			// cutOrFlash via ExecutePairGlobalGpu (success vs clear failure) and !globalPrior (0 dispatches).
			{
				PairGlobalSelectionInput cutIn;
				cutIn.cutOrFlash = true;
				cutIn.globalPrior = true;
				PairGlobal outRec;
				PairGlobalSelectionOutcome outOutcome;
				FallbackCatalog::Decision outFb;
				std::string outErr;

				recMock.Reset();
				Assert::IsTrue(ExecutePairGlobalGpu(cutIn, ctx, outRec, outOutcome, outFb, outErr));
				Assert::AreEqual(size_t(4), recMock.calls.size());
				Assert::AreEqual(std::string(kClearLabel), recMock.calls[0].description);
				Assert::IsFalse(outFb.engaged);
				Assert::AreEqual(std::string("motion.pair_global_unavailable"), outFb.id);

				recMock.Reset(/*failCall=*/1);
				Assert::IsFalse(ExecutePairGlobalGpu(cutIn, ctx, outRec, outOutcome, outFb, outErr));
				Assert::IsTrue(outFb.engaged && outFb.id == "motion.pair_global_unavailable" && Has(outErr, "clear dispatch failed"));

				PairGlobalSelectionInput noPriorIn;
				noPriorIn.globalPrior = false;
				recMock.Reset();
				Assert::IsTrue(ExecutePairGlobalGpu(noPriorIn, ctx, outRec, outOutcome, outFb, outErr));
				Assert::AreEqual(size_t(0), recMock.calls.size(), L"!globalPrior performs 0 dispatches");
				Assert::IsFalse(outFb.engaged);
			}
		}
	};

	// M1#4 (U10 C3, PairGlobalFill.h): Ruling 2 abort + visible FallbackCatalog::PairGlobalUnavailable on D1 failure,
	// clean release via FallbackCatalog::Sync on same-input recovery, and AcquireOrFillPairGlobalGpu skeleton-wired
	// failure vs success vs placeholder fallthrough.
	TEST_CLASS(PairGlobalUnavailableIsVisibleTests)
	{
	public:
		// Ruling 2 abort + visible "motion.pair_global_unavailable" on D1 failure (parallel missing GPU / missing scratch /
		// failing dispatchPerBlock; serial failing dispatch), plus exact-same-context positive control releasing the registry.
		TEST_METHOD(PairGlobalUnavailableIsVisible)
		{
			using namespace VpMotion;

			int sentinels[11] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11 };
			pl_tex l1A = reinterpret_cast<pl_tex>(&sentinels[0]);
			pl_tex l1B = reinterpret_cast<pl_tex>(&sentinels[1]);
			pl_tex l2A = reinterpret_cast<pl_tex>(&sentinels[2]);
			pl_tex l2B = reinterpret_cast<pl_tex>(&sentinels[3]);
			pl_tex vecTex[2] = { reinterpret_cast<pl_tex>(&sentinels[4]), reinterpret_cast<pl_tex>(&sentinels[5]) };
			pl_tex secTex[2] = { reinterpret_cast<pl_tex>(&sentinels[6]), reinterpret_cast<pl_tex>(&sentinels[7]) };
			pl_tex scratchSad2 = reinterpret_cast<pl_tex>(&sentinels[8]);
			pl_tex scratchSad1 = reinterpret_cast<pl_tex>(&sentinels[9]);
			pl_tex scratchPick = reinterpret_cast<pl_tex>(&sentinels[10]);

			PairGlobalSelectionInput d1In;
			d1In.globalPrior = true;   // selects MatcherD1 (no camera model, engine, or gap seed)

			PairGlobalDispatchRecorder recMock;

			// Case A1: Parallel D1 with gpu == nullptr and null scratch textures -> Ruling 2 abort with "global motion: missing GPU".
			{
				pl_tex nullSad2 = nullptr, nullSad1 = nullptr, nullPick = nullptr;
				PairGlobalGpuContext ctx;
				ctx.gpu = nullptr;
				ctx.serialReference = false;
				ctx.twoCandidates = true;
				ctx.l1A = l1A; ctx.l1B = l1B; ctx.l2A = l2A; ctx.l2B = l2B;
				ctx.globalVector = vecTex;
				ctx.globalSecond = secTex;
				ctx.bothSad2 = &nullSad2;
				ctx.bothSad1 = &nullSad1;
				ctx.bothPick = &nullPick;
				ctx.dispatchPerBlock = recMock.MakeDispatchPerBlock();

				PairGlobal outRec;
				PairGlobalSelectionOutcome outOutcome;
				FallbackCatalog::Decision outFb;
				std::string outErr;
				recMock.Reset();
				Assert::IsFalse(ExecutePairGlobalGpu(d1In, ctx, outRec, outOutcome, outFb, outErr),
					L"Case A1: missing GPU on D1 must abort (return false)");
				Assert::IsTrue(outOutcome.source == PairGlobalSource::MatcherD1 && outOutcome.needsD1Search);
				Assert::IsFalse(outRec.valid, L"Case A1: aborted D1 must never mark record valid");
				Assert::AreEqual(size_t(0), recMock.calls.size(), L"Case A1: 0 dispatches on missing GPU");
				Assert::AreEqual(std::string("global motion: missing GPU"), outErr);
				Assert::IsTrue(outFb.engaged, L"Case A1: fallback must engage visibly");
				Assert::AreEqual(std::string("motion.pair_global_unavailable"), outFb.id);
				Assert::IsTrue(Has(outFb.reason, outErr.c_str()), L"Case A1: fallback reason contains exact outError");
				Assert::IsTrue(Has(outFb.reason, "motion estimation aborted"), L"Case A1: fallback reason names abort");
				const std::string plain = FallbackPlainText::Sentence(outFb.id);
				Assert::IsFalse(plain.empty(), L"Case A1: FallbackPlainText has plain English sentence");
			}

			// Case A2: Parallel D1 with null scratch pointer handles -> Ruling 2 abort.
			{
				PairGlobalGpuContext ctx;
				ctx.serialReference = false;
				ctx.l1A = l1A; ctx.l1B = l1B; ctx.l2A = l2A; ctx.l2B = l2B;
				ctx.globalVector = vecTex;
				ctx.dispatchPerBlock = recMock.MakeDispatchPerBlock();

				PairGlobal outRec;
				PairGlobalSelectionOutcome outOutcome;
				FallbackCatalog::Decision outFb;
				std::string outErr;
				recMock.Reset();
				Assert::IsFalse(ExecutePairGlobalGpu(d1In, ctx, outRec, outOutcome, outFb, outErr),
					L"Case A2: missing scratch handles must abort");
				Assert::IsFalse(outRec.valid);
				Assert::IsFalse(outErr.empty());
				Assert::IsTrue(outFb.engaged && outFb.id == "motion.pair_global_unavailable");
				Assert::IsTrue(Has(outFb.reason, outErr.c_str()) && Has(outFb.reason, "motion estimation aborted"));
			}

			// Case A3: Parallel D1 with failing DispatchPerBlockFn vs exact-same-context positive control.
			{
				PairGlobalGpuContext ctx;
				ctx.serialReference = false;
				ctx.twoCandidates = true;
				ctx.l1A = l1A; ctx.l1B = l1B; ctx.l2A = l2A; ctx.l2B = l2B;
				ctx.globalVector = vecTex;
				ctx.globalSecond = secTex;
				ctx.bothSad2 = &scratchSad2;
				ctx.bothSad1 = &scratchSad1;
				ctx.bothPick = &scratchPick;

				PairGlobal outRec;
				PairGlobalSelectionOutcome outOutcome;
				FallbackCatalog::Decision outFb;
				std::string outErr;
				recMock.Reset(/*failCall=*/2, &outErr, "shader rejected: global motion pick (D1)");
				ctx.dispatchPerBlock = recMock.MakeDispatchPerBlock();

				Assert::IsFalse(ExecutePairGlobalGpu(d1In, ctx, outRec, outOutcome, outFb, outErr),
					L"Case A3: failing parallel D1 dispatch must abort");
				Assert::IsFalse(outRec.valid);
				Assert::AreEqual(size_t(2), recMock.calls.size());
				Assert::AreEqual(std::string("shader rejected: global motion pick (D1)"), outErr);
				Assert::IsTrue(outFb.engaged && outFb.id == "motion.pair_global_unavailable");
				Assert::IsTrue(Has(outFb.reason, outErr.c_str()) && Has(outFb.reason, "motion estimation aborted"));

				// Same-input positive control on exact same parallel ctx.
				recMock.Reset();
				ctx.dispatchPerBlock = recMock.MakeDispatchPerBlock();
				Assert::IsTrue(ExecutePairGlobalGpu(d1In, ctx, outRec, outOutcome, outFb, outErr),
					L"Case A3 positive control: succeeding parallel D1 returns true");
				Assert::AreEqual(size_t(4), recMock.calls.size(), L"RunBoth dispatches 4 D1 stages");
				Assert::IsTrue(outRec.valid && outErr.empty());
				Assert::IsFalse(outFb.engaged);
				Assert::AreEqual(std::string("motion.pair_global_unavailable"), outFb.id);
			}

			// Case A4 + Case B: Serial D1 failing DispatchFn engages registry visibly; exact-same-context succeeding
			// serial DispatchFn returns true and releases registry cleanly to "Fallbacks engaged: none".
			{
				FallbackStatus::Registry reg;
				Assert::AreEqual(std::string("Fallbacks engaged: none"), reg.Heading());

				PairGlobalGpuContext serialCtx;
				serialCtx.gpu = nullptr;
				serialCtx.serialReference = true;
				serialCtx.twoCandidates = false;
				serialCtx.l1A = l1A; serialCtx.l1B = l1B; serialCtx.l2A = l2A; serialCtx.l2B = l2B;
				serialCtx.globalVector = vecTex;
				serialCtx.globalSecond = secTex;

				PairGlobal outRec;
				PairGlobalSelectionOutcome outOutcome;
				FallbackCatalog::Decision outFb;
				std::string outErr;
				recMock.Reset(/*failCall=*/2, &outErr, "D1 serial dispatch failed on bwd");
				serialCtx.dispatch = recMock.MakeDispatch();

				Assert::IsFalse(ExecutePairGlobalGpu(d1In, serialCtx, outRec, outOutcome, outFb, outErr),
					L"Case A4: failing serial D1 must abort (Ruling 2)");
				Assert::IsFalse(outRec.valid, L"Case A4: aborted serial D1 never marks record valid");
				Assert::AreEqual(size_t(2), recMock.calls.size(), L"fwd succeeded, bwd failed");
				Assert::AreEqual(std::string("D1 serial dispatch failed on bwd"), outErr);
				Assert::IsTrue(outFb.engaged);
				Assert::AreEqual(std::string("motion.pair_global_unavailable"), outFb.id);
				Assert::IsTrue(Has(outFb.reason, outErr.c_str()), L"Case A4: reason preserves exact outError");
				Assert::IsTrue(Has(outFb.reason, "motion estimation aborted"), L"Case A4: reason names abort");

				const std::string sentence = FallbackPlainText::Sentence(outFb.id);
				Assert::IsFalse(sentence.empty(), L"Case A4: FallbackPlainText sentence must be non-empty plain English");
				const std::string notice = FallbackPlainText::FallbackNoticeText(outFb.id, outFb.reason);
				Assert::IsTrue(notice.find(sentence) == 0u && Has(notice, outErr.c_str()),
					L"Case A4: plain-English notice wraps exact technical reason");

				Assert::IsTrue(FallbackCatalog::Sync(reg, outFb), L"Case A4: Sync transitions registry to engaged");
				Assert::IsTrue(reg.Engaged("motion.pair_global_unavailable"));
				Assert::IsTrue(Has(reg.Heading(), "motion.pair_global_unavailable"));
				Assert::IsTrue(Has(reg.Heading(), "D1 serial dispatch failed on bwd"));

				// Case B (Same-input positive control + release): re-run exact same PairGlobalGpuContext with succeeding serial DispatchFn.
				recMock.Reset();
				serialCtx.dispatch = recMock.MakeDispatch();
				Assert::IsTrue(ExecutePairGlobalGpu(d1In, serialCtx, outRec, outOutcome, outFb, outErr),
					L"Case B: succeeding serial D1 on exact same context returns true");
				Assert::AreEqual(size_t(2), recMock.calls.size(), L"Case B: fwd + bwd serial D1 dispatches");
				Assert::AreEqual(std::string("VideoProcessor de-judder: global motion (D1, serial reference)"), recMock.calls[0].description);
				Assert::AreEqual(std::string("VideoProcessor de-judder: global motion (D1, serial reference)"), recMock.calls[1].description);
				Assert::IsTrue(recMock.calls[0].lastDstTex == vecTex[0] && recMock.calls[1].lastDstTex == vecTex[1]);
				Assert::IsTrue(outRec.valid && outErr.empty());
				Assert::IsFalse(outFb.engaged, L"Case B: success releases motion.pair_global_unavailable");
				Assert::AreEqual(std::string("motion.pair_global_unavailable"), outFb.id);

				Assert::IsTrue(FallbackCatalog::Sync(reg, outFb), L"Case B: Sync transitions registry back to released");
				Assert::IsFalse(reg.Engaged("motion.pair_global_unavailable"));
				Assert::AreEqual(std::string("Fallbacks engaged: none"), reg.Heading(),
					L"Case B: registry returns cleanly to Fallbacks engaged: none");
			}
		}

		// AcquireOrFillPairGlobalGpu skeleton-wired behavior: real GPU owner-fill error aborts (Ruling 2, no silent direct
		// fallthrough), valid GPU owner-fill propagates view+outcome and releases fallback, skeleton placeholder falls through
		// cleanly to ExecutePairGlobalGpu, and the P2 convenience overload respects memo vs standalone direct fallback.
		TEST_METHOD(AcquireOrFillPairGlobalGpuSkeletonFailureSuccessAndPlaceholderFallthrough)
		{
			using namespace VpMotion;
			using namespace VpSkeleton;

			int sentinels[4] = { 101, 102, 103, 104 };
			pl_tex vecTex[2] = { reinterpret_cast<pl_tex>(&sentinels[0]), reinterpret_cast<pl_tex>(&sentinels[1]) };
			pl_tex secTex[2] = { reinterpret_cast<pl_tex>(&sentinels[2]), reinterpret_cast<pl_tex>(&sentinels[3]) };

			PairGlobalDispatchRecorder recMock;
			PairGlobalGpuContext ctx;
			ctx.twoCandidates = true;
			ctx.globalVector = vecTex;
			ctx.globalSecond = secTex;
			ctx.dispatch = recMock.MakeDispatch();

			PairGlobalSelectionInput camIn;
			camIn.globalPrior = true;
			camIn.useModelGlobal = true;
			camIn.modelFwd[0] = 16.0f; camIn.modelFwd[1] = 8.0f;
			camIn.modelBwd[0] = -16.0f; camIn.modelBwd[1] = -8.0f;
			camIn.modelInputScale = 0.5f;

			FallbackStatus::Registry reg;

			// Case 1: SkeletonMemo GPU owner fill declines with a real error ("global motion: no 32-bit float storage format") ->
			// Ruling 2 abort, NEVER falls through to direct ExecutePairGlobalGpu even though ctx.dispatch would succeed!
			{
				SkeletonMemo memoFail;
				memoFail.SetRenderThread(std::this_thread::get_id());
				Assert::IsTrue(memoFail.RegisterOwnerFill(kP7CameraFit, AcquireForm::Gpu, kPairGlobalUnit,
					MakeP7PairGlobalGpuOwnerFill([](const OwnerFillRequest&, SkeletonMemo&, PairGlobalGpuView&, std::string& outWhy) {
						outWhy = "global motion: no 32-bit float storage format";
						return false;
					})) == SkeletonMemo::Registration::Registered);

				PairGlobal outRec = MakePairGlobalSample(PairGlobalSource::CameraModel, true, 1u, 1.0f);
				PairGlobalSelectionOutcome outOutcome;
				FallbackCatalog::Decision outFb;
				std::string outErr;
				bool usedMemoGpu = true;
				recMock.Reset();

				Assert::IsFalse(AcquireOrFillPairGlobalGpu(&memoFail, 20, camIn, ctx, outRec, outOutcome, outFb, outErr, &usedMemoGpu),
					L"Case 1: real GPU owner-fill error must abort (Ruling 2)");
				Assert::IsFalse(usedMemoGpu);
				Assert::AreEqual(size_t(0), recMock.calls.size(),
					L"Case 1: real GPU owner-fill failure must NOT silently fall through to direct fill");
				Assert::IsFalse(outRec.valid);
				Assert::IsTrue(outRec.src == PairGlobalSource::Cleared);
				Assert::IsTrue(outFb.engaged);
				Assert::AreEqual(std::string("motion.pair_global_unavailable"), outFb.id);
				Assert::IsTrue(Has(outFb.reason, "no 32-bit float storage format"),
					L"Case 1: fallback reason preserves real GPU error");
				Assert::IsTrue(Has(outFb.reason, "motion estimation aborted"));
				Assert::IsTrue(Has(outErr, "no 32-bit float storage format"));

				Assert::IsTrue(FallbackCatalog::Sync(reg, outFb));
				Assert::IsTrue(reg.Engaged("motion.pair_global_unavailable"));
			}

			// Case 2: SkeletonMemo GPU owner fill succeeds -> propagates view + outcome (both hasOutcome = true and false)
			// and releases "motion.pair_global_unavailable" without invoking direct dispatch.
			{
				const PairGlobal expectedCamRec = MakePairGlobalSample(PairGlobalSource::CameraModel, true, 0u, 3.0f);
				const PairGlobal expectedEngRec = MakePairGlobalSample(PairGlobalSource::Engine, true, 55u, -1.5f);

				SkeletonMemo memoOk;
				memoOk.SetRenderThread(std::this_thread::get_id());
				Assert::IsTrue(memoOk.RegisterOwnerFill(kP7CameraFit, AcquireForm::Gpu, kPairGlobalUnit,
					MakeP7PairGlobalGpuOwnerFill([&](const OwnerFillRequest& req, SkeletonMemo&, PairGlobalGpuView& outView, std::string&) {
						outView.globalVector = vecTex[0];
						outView.globalSecond = secTex[0];
						if (req.seq == 21)
						{
							outView.record = expectedCamRec;
							outView.hasOutcome = true;
							outView.outcome.source = PairGlobalSource::CameraModel;
							outView.outcome.modelGlobalUsed = true;
							outView.outcome.clearSecond = true;
						}
						else
						{
							outView.record = expectedEngRec;
							outView.hasOutcome = false;
						}
						return true;
					})) == SkeletonMemo::Registration::Registered);

				PairGlobal outRec;
				PairGlobalSelectionOutcome outOutcome;
				FallbackCatalog::Decision outFb;
				std::string outErr = "previous error";
				bool usedMemoGpu = false;
				recMock.Reset();

				Assert::IsTrue(AcquireOrFillPairGlobalGpu(&memoOk, 21, camIn, ctx, outRec, outOutcome, outFb, outErr, &usedMemoGpu),
					L"Case 2: valid GPU owner fill returns true");
				Assert::IsTrue(usedMemoGpu, L"Case 2: served via SkeletonMemo GPU path");
				Assert::AreEqual(size_t(0), recMock.calls.size(), L"Case 2: direct ExecutePairGlobalGpu not invoked");
				Assert::IsTrue(SamePairGlobal(expectedCamRec, outRec), L"Case 2: view record propagated bit-for-bit");
				Assert::IsTrue(outOutcome.source == PairGlobalSource::CameraModel);
				Assert::IsTrue(outOutcome.modelGlobalUsed && outOutcome.clearSecond && !outOutcome.globalFromEngine);
				Assert::IsTrue(outErr.empty());
				Assert::IsFalse(outFb.engaged, L"Case 2: fallback released on success");
				Assert::AreEqual(std::string("motion.pair_global_unavailable"), outFb.id);

				Assert::IsTrue(FallbackCatalog::Sync(reg, outFb));
				Assert::AreEqual(std::string("Fallbacks engaged: none"), reg.Heading(),
					L"Case 2: registry released cleanly to none");

				// hasOutcome = false synthesizes outcome flags from outView.record.src.
				usedMemoGpu = false;
				Assert::IsTrue(AcquireOrFillPairGlobalGpu(&memoOk, 22, camIn, ctx, outRec, outOutcome, outFb, outErr, &usedMemoGpu));
				Assert::IsTrue(usedMemoGpu && recMock.calls.empty());
				Assert::IsTrue(SamePairGlobal(expectedEngRec, outRec));
				Assert::IsTrue(outOutcome.source == PairGlobalSource::Engine && outOutcome.globalFromEngine && !outOutcome.clearSecond);
				Assert::IsFalse(outFb.engaged);
			}

			// Case 3: SkeletonMemo placeholder GPU owner fill ("pair global: gpu provider absent") -> falls through cleanly
			// to ExecutePairGlobalGpu (verified with mock DispatchFn).
			{
				SkeletonMemo memoPlaceholder;
				memoPlaceholder.SetRenderThread(std::this_thread::get_id());
				Assert::IsTrue(memoPlaceholder.RegisterOwnerFill(kP7CameraFit, AcquireForm::Gpu, kPairGlobalUnit,
					MakeP7PairGlobalGpuOwnerFill()) == SkeletonMemo::Registration::Registered);

				PairGlobal outRec;
				PairGlobalSelectionOutcome outOutcome;
				FallbackCatalog::Decision outFb;
				std::string outErr;
				bool usedMemoGpu = true;
				recMock.Reset();

				Assert::IsTrue(AcquireOrFillPairGlobalGpu(&memoPlaceholder, 23, camIn, ctx, outRec, outOutcome, outFb, outErr, &usedMemoGpu),
					L"Case 3: gpu provider absent placeholder falls through cleanly to ExecutePairGlobalGpu");
				Assert::IsFalse(usedMemoGpu, L"Case 3: direct fill executed after placeholder decline");
				Assert::AreEqual(size_t(4), recMock.calls.size(), L"Case 3: mock DispatchFn ran all 4 CameraModel passes");
				Assert::IsTrue(outOutcome.source == PairGlobalSource::CameraModel && outOutcome.modelGlobalUsed && outRec.valid);
				Assert::IsFalse(outFb.engaged);
				Assert::AreEqual(std::string("motion.pair_global_unavailable"), outFb.id);
			}

			// Case 4: Convenience overload loads kP2VectorPair when memo != nullptr (ignoring directP2Fallback) and uses
			// directP2Fallback when memo == nullptr.
			{
				VectorEngine engine;
				engine.PublishProducer(std::make_shared<MotionAhead::Producer>());
				engine.p2Slot->SetGeneration(engine.motionValidity.StreamGeneration());
				const MotionAhead::FlowPtr memoFlow = MakeClearGlobalFlow(3.25f, -0.75f, true);
				Assert::IsTrue(engine.p2Slot->Publish(10, 11, engine.p2Slot->Generation(), memoFlow));

				SkeletonMemo memoP2;
				memoP2.SetRenderThread(std::this_thread::get_id());
				IVectorEngine* enginePtr = &engine;
				Assert::IsTrue(RegisterFill(memoP2, [enginePtr](const void*) { return enginePtr; }));
				Assert::IsTrue(memoP2.RegisterOwnerFill(kP7CameraFit, AcquireForm::Gpu, kPairGlobalUnit,
					MakeP7PairGlobalGpuOwnerFill()) == SkeletonMemo::Registration::Registered);

				const MotionAhead::FlowPtr directFlow = MakeClearGlobalFlow(-5.5f, 2.25f, true);
				const P2PairRecord directP2 = MakeP2PairRecord(10, 11, 88u, directFlow->global, directFlow->second, true, false);

				PairGlobalSelectionInput engIn;
				engIn.globalPrior = true;
				engIn.opticalFlowGlobal = true;
				engIn.seeded = true;

				PairGlobal outRec;
				PairGlobalSelectionOutcome outOutcome;
				FallbackCatalog::Decision outFb;
				std::string outErr;

				recMock.Reset();
				Assert::IsTrue(AcquireOrFillPairGlobalGpu(&memoP2, 11, &directP2, engIn, ctx, outRec, outOutcome, outFb, outErr));
				Assert::IsTrue(outOutcome.source == PairGlobalSource::Engine && outOutcome.globalFromEngine);
				Assert::AreEqual(uint64_t(1), outRec.p2Epoch, L"Case 4: memo path uses kP2VectorPair epoch, not directP2");
				Assert::AreEqual(size_t(2), recMock.calls.size());
				Assert::AreEqual(0, std::memcmp(recMock.calls[0].vec4, memoFlow->global[0], sizeof(memoFlow->global[0])));
				Assert::AreEqual(0, std::memcmp(recMock.calls[1].vec4, memoFlow->global[1], sizeof(memoFlow->global[1])));

				recMock.Reset();
				Assert::IsTrue(AcquireOrFillPairGlobalGpu(nullptr, 11, &directP2, engIn, ctx, outRec, outOutcome, outFb, outErr));
				Assert::IsTrue(outOutcome.source == PairGlobalSource::Engine && outOutcome.globalFromEngine);
				Assert::AreEqual(uint64_t(88), outRec.p2Epoch, L"Case 4: standalone path uses directP2 epoch");
				Assert::AreEqual(size_t(2), recMock.calls.size());
				Assert::AreEqual(0, std::memcmp(recMock.calls[0].vec4, directP2.global[0], sizeof(directP2.global[0])));
				Assert::AreEqual(0, std::memcmp(recMock.calls[1].vec4, directP2.global[1], sizeof(directP2.global[1])));
			}
		}
	};
}
