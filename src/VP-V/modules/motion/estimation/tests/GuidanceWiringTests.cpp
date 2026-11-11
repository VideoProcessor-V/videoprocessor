// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "CppUnitTest.h"

// Camera link K4 wiring (prepared before P5a): the one refusal gate, the d = 0 on-time rule, the guide lane job (lane == live),
// the PairContext provider and the flow resampler, the skeleton-visible status (FallbackCatalog decisions) and the PassTimers
// rows. CPU only; every piece is driven through fakes of the engine and the link. No test name carries the word for the device.
#include "CameraLinkTestSupport.h"
#include <VP-V/modules/motion/estimation/global_model/link/CameraLinkCore.h>
#include <VP-V/modules/motion/estimation/global_model/link/GuideLaneJob.h>
#include <VP-V/modules/motion/estimation/global_model/link/GuidanceAdapter.h>
#include <VP-V/modules/motion/estimation/global_model/link/GuidanceContext.h>
#include <VP-V/modules/motion/estimation/global_model/link/GuidanceGate.h>
#include <VP-V/modules/motion/estimation/global_model/link/GuidanceScheduler.h>
#include <VP-V/modules/motion/estimation/global_model/link/GuidanceStatus.h>
#include <VP-V/modules/motion/estimation/global_model/link/GuidanceTimers.h>
#include <VP-V/core/PassTimerInbox.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace VideoProcessorTest
{
	namespace
	{
		struct WireLink : CameraLink::ICameraLink
		{
			std::map<uint64_t, std::shared_ptr<const CameraModel::CameraMotion>> records;
			std::map<uint64_t, std::shared_ptr<const CameraLink::FieldCopy>> fields;
			std::map<uint64_t, CameraLink::OnTimeMiss> late;       // camera(seq) published but NOT on time, with this cause
			std::map<uint64_t, CameraLink::SealVerdict> seals;     // the on-time seal's verdict (absent: Pending)
			std::shared_ptr<const CameraModel::CameraMotion> OnTimeRecord(uint64_t s, CameraLink::OnTimeCheck check, CameraLink::OnTimeMiss& why) const override
			{
				auto it = late.find(s);
				if (it != late.end())
				{
					why = it->second;
					return nullptr;
				}
				return CameraLink::ICameraLink::OnTimeRecord(s, check, why);
			}
			CameraLink::SealVerdict OnTimeSealVerdict(uint64_t s) const override
			{
				auto it = seals.find(s);
				return it == seals.end() ? CameraLink::SealVerdict::Pending : it->second;
			}
			std::shared_ptr<const CameraModel::CameraMotion> SealedRecord(uint64_t s) const override { return CausalRecord(s); }
			std::shared_ptr<const CameraModel::CameraMotion> CausalRecord(uint64_t s) const override
			{
				auto it = records.find(s);
				return it == records.end() ? nullptr : it->second;
			}
			std::shared_ptr<const CameraLink::FieldCopy> FieldOf(uint64_t s) const override
			{
				auto it = fields.find(s);
				return it == fields.end() ? nullptr : it->second;
			}
			bool LabelsFor(uint64_t, CameraLink::CameraConsumer, CameraLink::CameraLabelsView&) override { return false; }
			std::shared_ptr<const CameraModel::GuidanceFrame> GuideFor(uint64_t) override { return nullptr; }

			void AddPan(uint64_t seq, double tx = 12)
			{
				CameraSynth::Field f(CameraSynth::Grid4K());
				f.FromModel(CameraSynth::PanModel(tx, 0));
				f.AddNoise(0.05, 7 + seq);
				auto copy = CameraLinkTest::CopyOf(f, seq);
				CameraModel::PairInput in = f.Input(1.0 / 24, seq);
				CameraModel::Fitter fitter;
				records[seq] = std::make_shared<const CameraModel::CameraMotion>(fitter.Fit(in));
				fields[seq] = copy;
			}

			// camera(seq) measured its pair over `seconds` (SameCameraPair compares it with the matcher's pair duration).
			void Retime(uint64_t seq, double seconds)
			{
				auto r = std::make_shared<CameraModel::CameraMotion>(*records[seq]);
				r->pairSeconds = seconds;
				records[seq] = r;
			}
		};

		void PansAt(WireLink& l, std::initializer_list<uint64_t> seqs)
		{
			for (uint64_t q : seqs)
				l.AddPan(q);
		}

		int64_t& FakeClockNow() { static int64_t now = 0; return now; }
		int64_t FakeClock() { FakeClockNow() += 3000; return FakeClockNow(); }

		struct WireEngine : VpMotion::IVectorEngine
		{
			bool slip = false, cpuFlow = false, flowAvailable = true;
			std::vector<float> fx, fy;
			VpMotion::FlowView view;
			mutable uint64_t askedFrom = 0, askedTo = 0;
			mutable int slipCalls = 0;
			uint64_t LatestVerifiedFieldSeq() const override { return 0; }
			uint64_t FieldEra() const override { return 1; }
			bool FieldOf(uint64_t, VpMotion::VerifiedField&) const override { return false; }
			float FieldScale() const override { return 1.0f; }
			bool GlobalFromEngine() const override { return false; }
			bool EngineGlobal(uint64_t, VpMotion::GlobalVec&) const override { return false; }
			bool PairSpansSlip(uint64_t from, uint64_t to) const override { askedFrom = from; askedTo = to; ++slipCalls; return slip; }
			bool FlowIsCpuResident() const override { return cpuFlow; }
			bool FlowOf(uint64_t, VpMotion::FlowView& out) const override { out = view; return flowAvailable; }
			VpMotion::IPreEstimateHook* hook = nullptr;
			int hookClears = 0;
			void SetPreEstimateHook(VpMotion::IPreEstimateHook* h) override { hook = h; if (!h) ++hookClears; }
		};

		struct WireSink : VpMotion::IGuidanceSink
		{
			std::shared_ptr<const CameraModel::GuidanceFrame> last;
			void SetCameraGuidance(std::shared_ptr<const CameraModel::GuidanceFrame> f) override { last = f; }
		};

		struct WireCounts : CameraLink::IGuidanceCountsSource
		{
			int requested = 0;
			std::vector<Done> ready;
			bool Request(uint64_t, pl_tex) override { ++requested; return true; }
			size_t Poll(std::vector<Done>& out) override { const size_t n = ready.size(); out = ready; ready.clear(); return n; }
		};

		pl_tex FakeTex() { return reinterpret_cast<pl_tex>(static_cast<uintptr_t>(0x10)); }

		CameraModel::CameraMotion Record(double confidence, CameraModel::Reason reason = CameraModel::Reason::Ok, bool valid = true)
		{
			CameraModel::CameraMotion m;
			m.valid = valid;
			m.confidence = confidence;
			m.reason = reason;
			return m;
		}

		CameraLink::GuidanceSettings GuideOn()
		{
			CameraLink::GuidanceSettings s;
			s.switches.modelGlobal = true;
			s.switches.narrowWindow = true;
			s.switches.e1Skip = true;
			return s;
		}

		bool IsEngaged(const std::vector<FallbackCatalog::Decision>& d, const char* id)
		{
			for (const FallbackCatalog::Decision& x : d)
				if (x.id == id)
					return x.engaged;
			Assert::Fail(L"decision id missing from the list");
			return false;
		}
	}

	TEST_CLASS(GuidanceWiringTests)
	{
	public:
		// ---- K4-3: the refusal gate -------------------------------------------------------------------------------------

		TEST_METHOD(TheGateJudgesTheReasonsInTheDocumentedOrder)
		{
			using namespace CameraLink;
			CameraModel::MeasurementSwitches off, on;
			on.modelGlobal = true;
			const CameraModel::MeasurementConfig cfg = CameraModel::MeasurementConfig();
			const CameraModel::CameraMotion good = Record(0.9);
			Assert::IsTrue(GuidanceRefusal(off, cfg, &good, false, false) == Refusal::AllOff);
			Assert::IsTrue(GuidanceRefusal(off, cfg, nullptr, true, true) == Refusal::AllOff, L"all off is judged first");
			Assert::IsTrue(GuidanceRefusal(on, cfg, nullptr, false, false) == Refusal::NotOnTime, L"camera(N) not ready in time");
			Assert::IsTrue(GuidanceRefusal(on, cfg, &good, true, false) == Refusal::Slip);
			Assert::IsTrue(GuidanceRefusal(on, cfg, &good, false, true) == Refusal::Slip, L"an ancient stamp counts as a slip");
			const CameraModel::CameraMotion amb = Record(0.9, CameraModel::Reason::Ambiguous);
			Assert::IsTrue(GuidanceRefusal(on, cfg, &amb, false, false) == Refusal::NotUsable);
			Assert::IsTrue(GuidanceRefusal(on, cfg, &amb, true, false) == Refusal::Slip, L"slip is judged before usability");
			const CameraModel::CameraMotion invalid = Record(0.9, CameraModel::Reason::Ok, false);
			Assert::IsTrue(GuidanceRefusal(on, cfg, &invalid, false, false) == Refusal::NotUsable);
			const CameraModel::CameraMotion weak = Record(0.6);
			Assert::IsTrue(Usable(weak), L"0.6 is usable (floor 0.5) but below the guidance floor 0.7");
			Assert::IsTrue(GuidanceRefusal(on, cfg, &weak, false, false) == Refusal::LowConfidence);
			const CameraModel::CameraMotion edge = Record(0.7);
			Assert::IsTrue(GuidanceRefusal(on, cfg, &edge, false, false) == Refusal::None, L"exactly the floor is accepted");
			Assert::IsTrue(GuidanceRefusal(on, cfg, &good, false, false) == Refusal::None);
		}

		TEST_METHOD(TheStaticGateAloneNeedsPointEightFiveAndWithOtherSwitchesTheFloorIsPointSeven)
		{
			using namespace CameraLink;
			const CameraModel::MeasurementConfig cfg = CameraModel::MeasurementConfig();
			CameraModel::MeasurementSwitches onlyStatic;
			onlyStatic.staticGate = true;
			CameraModel::MeasurementSwitches mixed = onlyStatic;
			mixed.narrowWindow = true;
			const CameraModel::CameraMotion c80 = Record(0.80), c85 = Record(0.85);
			Assert::IsTrue(OnlyStaticGate(onlyStatic) && !OnlyStaticGate(mixed));
			Assert::IsTrue(GuidanceRefusal(onlyStatic, cfg, &c80, false, false) == Refusal::LowConfidence);
			Assert::IsTrue(GuidanceRefusal(onlyStatic, cfg, &c85, false, false) == Refusal::None);
			Assert::IsTrue(GuidanceRefusal(mixed, cfg, &c80, false, false) == Refusal::None, L"the plan applies 0.85 to its static step");
			Assert::AreEqual(0.85, ConfidenceFloor(onlyStatic, cfg), 1e-12);
			Assert::AreEqual(0.70, ConfidenceFloor(mixed, cfg), 1e-12);
			CameraModel::MeasurementConfig loose;
			loose.minConfidence = 0.5;
			Assert::AreEqual(0.85, ConfidenceFloor(onlyStatic, loose), 1e-12, L"never below the static confidence");
			Assert::AreEqual(0.5, ConfidenceFloor(mixed, loose), 1e-12);
		}

		TEST_METHOD(BuildGuideRefusesExactlyWhereTheGateDoes)
		{
			using namespace CameraLink;
			const CameraModel::MeasurementConfig cfg = CameraModel::MeasurementConfig();
			CameraModel::MeasurementSwitches sw;
			sw.modelGlobal = true;
			sw.narrowWindow = true;
			const CameraModel::CameraMotion amb = Record(0.9, CameraModel::Reason::Ambiguous), weak = Record(0.6);
			struct Case { const CameraModel::CameraMotion* rec; bool slip, ancient; };
			const Case cases[] = { { nullptr, false, false }, { &amb, false, false }, { &weak, false, false }, { &weak, true, false },
				{ &amb, false, true } };
			for (const Case& c : cases)
			{
				GuideInputs in;
				in.record = c.rec;
				in.ctx.spansSlip = c.slip;
				in.ctx.ancientStamp = c.ancient;
				const GuideResult r = BuildGuide(sw, cfg, in);
				Assert::IsTrue(r.refusal == GuidanceRefusal(sw, cfg, c.rec, c.slip, c.ancient));
				Assert::IsTrue(r.frame == nullptr, L"a refused pair binds nothing");
			}
			// A refused pair still reports the record's own confidence and reason (bench rows, status).
			GuideInputs in;
			in.record = &weak;
			const GuideResult r = BuildGuide(sw, cfg, in);
			Assert::AreEqual(0.6, r.confidence, 1e-12);
		}

		// ---- K4-2: the guide lane job ---------------------------------------------------------------------------------

		TEST_METHOD(TheFittedPairIsWantedAndStaysWantedUntilP4Arrives)
		{
			WireLink link;
			PansAt(link, { 1, 2 });
			CameraLink::GuidanceScheduler sched(link, GuideOn());
			bool p4Published = false;
			sched.SetContextProvider([&p4Published](uint64_t, CameraLink::PairContext&) { return p4Published; });
			CameraLink::GuideLaneJob job(sched, link);
			job.OnFitDone(2);
			Assert::AreEqual(size_t(1), job.Pending(), L"d = 0: the pair the camera just fitted is wanted");
			Assert::AreEqual(size_t(0), job.Pump(), L"P4(2) is not published yet");
			Assert::AreEqual(size_t(1), job.Pending(), L"still wanted");
			Assert::AreEqual(uint64_t(1), job.GetStats().retried);
			p4Published = true;
			Assert::AreEqual(size_t(1), job.Pump());
			Assert::AreEqual(size_t(0), job.Pending());
			Assert::AreEqual(uint64_t(1), sched.Counters().lanePublished);
			Assert::IsTrue(sched.Guide(2).fromLane, L"the render used the lane's result");
			job.OnFitDone(2);
			Assert::AreEqual(size_t(0), job.Pending(), L"a judged pair is not wanted again");
		}

		TEST_METHOD(AFitThatIsNotOnTimeWantsNothingAndThePairRunsUnguidedWithItsCause)
		{
			WireLink link;
			PansAt(link, { 1, 2 });
			link.late[2] = CameraLink::OnTimeMiss::Bridged;
			CameraLink::GuidanceScheduler sched(link, GuideOn());
			CameraLink::GuideLaneJob job(sched, link);
			job.OnFitDone(2);
			Assert::AreEqual(size_t(0), job.Pending(), L"camera(2) was not on time: no lane work");
			Assert::AreEqual(uint64_t(0), job.GetStats().wanted);
			const CameraLink::GuideResult r = sched.Guide(2, 1.0 / 24);
			Assert::IsTrue(r.frame == nullptr && r.refusal == CameraLink::Refusal::NotOnTime && r.miss == CameraLink::OnTimeMiss::Bridged);
			Assert::IsTrue(sched.Guide(1, 1.0 / 24).frame != nullptr, L"camera(1) guides pair 1 only");
			const CameraLink::GuidanceCounters& c = sched.Counters();
			Assert::AreEqual(uint64_t(1), c.notOnTime[size_t(CameraLink::OnTimeMiss::Bridged)]);
			Assert::AreEqual(uint64_t(1), c.guided);
		}

		TEST_METHOD(EveryNotOnTimeCauseIsCountedSeparatelyAndNamedInTheStatusLine)
		{
			using CameraLink::OnTimeMiss;
			WireLink link;
			PansAt(link, { 1, 2, 3, 4, 5, 6 });
			link.late[1] = OnTimeMiss::NoAheadPair;
			link.late[2] = OnTimeMiss::NotPublished;
			link.late[3] = OnTimeMiss::Bridged;
			link.late[4] = OnTimeMiss::Stale;
			CameraLink::GuidanceScheduler sched(link, GuideOn());
			for (uint64_t q = 1; q <= 4; ++q)
				Assert::IsTrue(sched.Guide(q, 1.0 / 24).miss == link.late[q], L"causes a to d");
			Assert::IsTrue(sched.Guide(5, 2.0 / 24).miss == OnTimeMiss::NotThePair, L"cause e: a fill pair over two camera pairs");
			Assert::IsTrue(sched.Guide(6, 1.0 / 24).frame != nullptr);
			const CameraLink::GuidanceCounters& c = sched.Counters();
			for (size_t m = size_t(OnTimeMiss::NoAheadPair); m <= size_t(OnTimeMiss::NotThePair); ++m)
				Assert::AreEqual(uint64_t(1), c.notOnTime[m]);
			Assert::AreEqual(uint64_t(5), c.refusals[size_t(CameraLink::Refusal::NotOnTime)]);
			bool line = false;
			for (const std::string& l : sched.StatusLines())
			{
				line = line || l == "camera guidance (d=0): 1 guided on time (17% of pairs), 5 unguided - camera(N) not ready: "
					"a 1, b 1, c 1, d 1, e 1; R 0 guided then rejected at the seal";
				Assert::IsTrue(l.find("refused: camera(N)") == std::string::npos, L"not ready in time is not listed as a refusal");
			}
			Assert::IsTrue(line, L"the NotOnTime counter is visible, per cause");
			Assert::IsFalse(IsEngaged(sched.CollectDecisions(), CameraLink::kIdGuidanceRefused), L"expected at d = 0: no held refusal entry");
		}

		TEST_METHOD(WithGapsInTheNumberingPairNStillReadsCameraNOnlyAndLaneEqualsLive)
		{
			WireLink link;
			PansAt(link, { 1, 2, 3, 5, 6 });   // 4 was a repeat (24p in 30): no field, no record
			CameraLink::GuidanceScheduler live(link, GuideOn());
			CameraLink::GuidanceScheduler lane(link, GuideOn());
			const CameraLink::GuideResult b = live.Guide(6);
			Assert::IsTrue(b.frame != nullptr && b.refusal == CameraLink::Refusal::None);
			CameraLink::GuideLaneJob job(lane, link);
			job.OnFitDone(6);
			Assert::AreEqual(size_t(1), job.Pump());
			const CameraLink::GuideResult a = lane.Guide(6);
			Assert::IsTrue(a.fromLane && a.frame != nullptr);
			Assert::IsTrue(CameraLink::SameGuide(*a.frame, *b.frame), L"lane == live");
			// No record is ever replaced by another: the gap and a not-yet-fitted pair are both "not ready in time".
			Assert::IsTrue(live.Guide(4).refusal == CameraLink::Refusal::NotOnTime, L"no camera(4): not replaced by 3 or 5");
			Assert::IsTrue(live.Guide(7).refusal == CameraLink::Refusal::NotOnTime, L"camera(7) not fitted: not replaced by 6");
		}

		TEST_METHOD(AStoredLaneResultIsUsedOnlyForTheVeryRecordTheRenderReads)
		{
			WireLink link;
			PansAt(link, { 1, 2 });
			CameraLink::GuidanceScheduler sched(link, GuideOn());
			CameraLink::GuideLaneJob job(sched, link);
			job.OnFitDone(2);
			Assert::AreEqual(size_t(1), job.Pump());
			link.records[2] = std::make_shared<const CameraModel::CameraMotion>(*link.records[2]);   // camera(2) re-published: another record
			const CameraLink::GuideResult r = sched.Guide(2);
			Assert::IsFalse(r.fromLane, L"another record: the render computes live");
			Assert::IsTrue(r.frame != nullptr);
			Assert::AreEqual(uint64_t(0), sched.Counters().fromLane);
		}

		TEST_METHOD(AGuidedPairRejectedAtItsSealAfterwardsIsCountedAsRAndShownAsUnguided)
		{
			WireLink link;
			PansAt(link, { 3, 4 });
			CameraLink::GuidanceScheduler sched(link, GuideOn());
			Assert::IsTrue(sched.Guide(3, 1.0 / 24).frame != nullptr);
			Assert::IsTrue(sched.Guide(4, 1.0 / 24).frame != nullptr);
			sched.PollSeals();
			Assert::AreEqual(uint64_t(0), sched.Counters().rejectedAtSeal, L"both seals pending");
			Assert::IsFalse(IsEngaged(sched.CollectDecisions(), CameraLink::kIdGuidanceUnguided));
			link.seals[3] = CameraLink::SealVerdict::Accepted;
			link.seals[4] = CameraLink::SealVerdict::Rejected;
			sched.PollSeals();
			sched.PollSeals();
			Assert::AreEqual(uint64_t(1), sched.Counters().rejectedAtSeal, L"counted once");
			const std::vector<FallbackCatalog::Decision> d = sched.CollectDecisions();
			Assert::IsTrue(IsEngaged(d, CameraLink::kIdGuidanceUnguided));
			for (const FallbackCatalog::Decision& x : d)
				if (x.id == CameraLink::kIdGuidanceUnguided)
					Assert::AreEqual(std::string("camera_guidance: unguided - guided pairs rejected at the seal"), x.reason);
			bool line = false;
			for (const std::string& l : sched.StatusLines())
				line = line || l.find("R 1 guided then rejected at the seal") != std::string::npos;
			Assert::IsTrue(line);
			sched.ResetForDiscontinuity();
			link.seals.clear();
			Assert::IsTrue(sched.Guide(3, 1.0 / 24).frame != nullptr);
			sched.ResetForDiscontinuity();
			link.seals[3] = CameraLink::SealVerdict::Rejected;
			sched.PollSeals();
			Assert::AreEqual(uint64_t(1), sched.Counters().rejectedAtSeal, L"a discontinuity forgets the pairs awaiting their seal");
		}

		TEST_METHOD(ALaneResultAfterTheRenderAskedIsDiscardedAndCounted)
		{
			WireLink link;
			PansAt(link, { 1, 2 });
			CameraLink::GuidanceScheduler sched(link, GuideOn());
			CameraLink::GuideLaneJob job(sched, link);
			const CameraLink::GuideResult live = sched.Guide(2);
			Assert::IsTrue(live.frame != nullptr && !live.fromLane);
			job.OnFitDone(2);
			Assert::AreEqual(size_t(0), job.Pump());
			Assert::AreEqual(size_t(0), job.Pending(), L"judged: it does not stay wanted");
			Assert::AreEqual(uint64_t(1), job.GetStats().discarded);
			Assert::AreEqual(uint64_t(1), sched.Counters().laneDiscarded);
		}

		TEST_METHOD(WithAllSwitchesOffTheLaneJobWantsNothingAndStoresNothing)
		{
			WireLink link;
			PansAt(link, { 1, 2 });
			CameraLink::GuidanceScheduler sched(link, CameraLink::GuidanceSettings());
			CameraLink::GuideLaneJob job(sched, link);
			Assert::IsFalse(sched.LaneWanted());
			job.OnFitDone(2);
			Assert::AreEqual(size_t(0), job.Pending());
			Assert::AreEqual(size_t(0), job.Pump());
			Assert::AreEqual(uint64_t(0), sched.Counters().lanePublished);
			Assert::AreEqual(uint64_t(0), job.GetStats().wanted);
		}

		TEST_METHOD(TheWantedSetIsBoundedDuplicatesAreIgnoredAndAResetForgets)
		{
			WireLink link;
			CameraLink::GuidanceScheduler sched(link, GuideOn());
			sched.SetContextProvider([](uint64_t, CameraLink::PairContext&) { return false; });
			CameraLink::GuideLaneJob job(sched, link, 2);
			link.AddPan(1);
			job.OnFitDone(1);
			job.OnFitDone(1);
			Assert::AreEqual(size_t(1), job.Pending(), L"the same pair is wanted once");
			link.AddPan(2);
			job.OnFitDone(2);
			link.AddPan(3);
			job.OnFitDone(3);
			Assert::AreEqual(size_t(2), job.Pending(), L"bounded");
			Assert::AreEqual(uint64_t(1), job.GetStats().dropped, L"the oldest was dropped and counted");
			job.Reset();
			Assert::AreEqual(size_t(0), job.Pending());
			job.OnFitDone(3);
			Assert::AreEqual(size_t(1), job.Pending(), L"after a reset the same pair can be wanted again");
		}

		TEST_METHOD(AStoredLaneResultIsDroppedWhenTheSettingsChange)
		{
			WireLink link;
			PansAt(link, { 1, 2 });
			CameraLink::GuidanceScheduler sched(link, GuideOn());
			CameraLink::GuideLaneJob job(sched, link);
			job.OnFitDone(2);
			Assert::AreEqual(size_t(1), job.Pump());
			CameraLink::GuidanceSettings s = GuideOn();
			s.config.narrowRadiusSigmas = 6.0;   // a threshold only: the switches are unchanged
			sched.SetSettings(s);
			Assert::IsFalse(sched.Guide(2).fromLane, L"a plan built under the old thresholds is never used");
			// The same holds when the thresholds change without SetSettings clearing the store (the key itself differs).
			job.Reset();
			link.AddPan(3);
			job.OnFitDone(3);
			Assert::AreEqual(size_t(1), job.Pump());
			Assert::IsTrue(sched.Guide(3).fromLane);
		}

		TEST_METHOD(TheThresholdKeyAloneRejectsAStoredLaneResultBuiltUnderOtherThresholds)
		{
			WireLink link;
			PansAt(link, { 1, 2 });
			CameraLink::GuidanceSettings s = GuideOn();
			CameraLink::GuidanceScheduler sched(link, s);
			// A real guide for pair 2 (the same function the lane runs), stored WITHOUT SetSettings: nothing clears the store.
			CameraLink::GuidanceScheduler maker(link, s);
			const CameraLink::GuideResult made = maker.Guide(2);
			Assert::IsTrue(made.frame != nullptr);
			// Control: stored as built under the scheduler's own thresholds, it is used.
			sched.StoreLaneForTest(2, made, s.config);
			Assert::IsTrue(sched.Guide(2).fromLane, L"same thresholds: the stored result is consumed");
			// Same switches, same record, same pair: only one threshold differs in what it was built under.
			CameraModel::MeasurementConfig other = s.config;
			other.narrowRadiusSigmas += 1.0;
			sched.StoreLaneForTest(2, made, other);
			const CameraLink::GuideResult r = sched.Guide(2);
			Assert::IsFalse(r.fromLane, L"the threshold key alone rejects it: the render computes live");
			Assert::IsTrue(r.frame != nullptr);
			Assert::AreEqual(uint64_t(1), sched.Counters().fromLane, L"only the control was consumed from the lane");
		}

		TEST_METHOD(ASlipThatAppearsAfterTheLaneStoredItIsSeenAtConsumption)
		{
			WireLink link;
			PansAt(link, { 1, 2 });
			CameraLink::GuidanceScheduler sched(link, GuideOn());
			CameraLink::GuideLaneJob job(sched, link);
			job.OnFitDone(2);
			Assert::AreEqual(size_t(1), job.Pump());
			uint64_t from = 0, to = 0;
			sched.SetSlipCheck([&](uint64_t recordSeq, uint64_t pairSeq) { from = recordSeq; to = pairSeq; return true; });
			const CameraLink::GuideResult r = sched.Guide(2);
			Assert::IsTrue(r.frame == nullptr && r.refusal == CameraLink::Refusal::Slip);
			Assert::AreEqual(link.records[2]->pairFrom, from, L"camera(2)'s own pair start");
			Assert::AreEqual(uint64_t(2), to);
			Assert::AreEqual(uint64_t(1), sched.Counters().refusals[size_t(CameraLink::Refusal::Slip)]);
		}

		// The render asks (or a discontinuity arrives) while the lane is in the middle of building the guide: the lane's result is
		// then from the old numbering / too late and must be dropped, whatever the timing. A barrier makes the interleaving exact.
		static void LaneMidComputeThen(const std::function<void(CameraLink::GuidanceScheduler&)>& whileLaneIsInside, bool expectDiscardedCounter)
		{
			WireLink link;
			PansAt(link, { 1, 2 });
			CameraLink::GuidanceScheduler sched(link, GuideOn());
			std::atomic<int> calls(0);
			std::promise<void> inside, release;
			std::shared_future<void> releaseFuture = release.get_future().share();
			sched.SetContextProvider([&](uint64_t, CameraLink::PairContext&)
			{
				if (calls.fetch_add(1) == 0)
				{
					inside.set_value();      // the lane thread is in the middle of the compute ...
					releaseFuture.wait();    // ... and stays there until the main thread lets it go
				}
				return true;
			});
			std::atomic<int> outcome(-1);
			std::thread lane([&]() { outcome = static_cast<int>(sched.TryPublishLane(2)); });
			const bool entered = inside.get_future().wait_for(std::chrono::seconds(20)) == std::future_status::ready;
			if (!entered)
				release.set_value();
			if (entered)
			{
				whileLaneIsInside(sched);
				release.set_value();
			}
			lane.join();
			Assert::IsTrue(entered, L"the lane thread reached the barrier");
			Assert::IsTrue(outcome.load() == static_cast<int>(CameraLink::GuidanceScheduler::LaneOutcome::Discarded), L"the lane result was discarded");
			Assert::AreEqual(uint64_t(0), sched.Counters().lanePublished, L"nothing was stored");
			Assert::AreEqual(expectDiscardedCounter ? uint64_t(1) : uint64_t(0), sched.Counters().laneDiscarded);
			const CameraLink::GuideResult r = sched.Guide(2);
			Assert::IsFalse(r.fromLane, L"the render computes live");
			Assert::IsTrue(r.frame != nullptr);
		}

		TEST_METHOD(TheRenderAskingWhileTheLaneIsMidComputeWinsAndTheLaneResultIsDiscarded)
		{
			LaneMidComputeThen([](CameraLink::GuidanceScheduler& sched)
			{
				const CameraLink::GuideResult live = sched.Guide(2);   // the render asks now (the provider's later calls pass straight through)
				Assert::IsTrue(live.frame != nullptr && !live.fromLane);
			}, true);
		}

		TEST_METHOD(ADiscontinuityWhileTheLaneIsMidComputeKeepsItsResultOut)
		{
			LaneMidComputeThen([](CameraLink::GuidanceScheduler& sched) { sched.ResetForDiscontinuity(); }, false);
		}

		TEST_METHOD(TheCoreCallsTheFitObserverAfterThePublishAndKeepsTheTimersHonest)
		{
			CameraLinkTest::Rig rig;
			FrameAnalysis::Store store;
			CameraLink::CameraLinkCore core(store);
			CameraLink::CameraLinkSettings on;
			on.p7Wanted = true;
			on.deterministic = true;
			FakeClockNow() = 0;
			on.fit.clock = &FakeClock;   // every fit "costs" 3 ms of the injected clock (read twice per Fit: start and end)
			core.OnSettings(on);
			core.AttachGraph(rig.table.get(), rig.bytes.get(), rig.indicator.get());
			CameraLink::TimingFacts tf;
			tf.measuredSourceHz = 24;
			core.SetTimingFacts(tf);
			core.Producer()->NoteTiming();
			CameraLink::GuidanceScheduler sched(*core.Link(), GuideOn());
			CameraLink::GuideLaneJob job(sched, *core.Link());
			std::vector<uint64_t> seen;
			bool recordWasThere = false;
			core.SetFitDoneObserver([&](uint64_t seq)
			{
				seen.push_back(seq);
				recordWasThere = core.CausalRecord(seq) != nullptr;   // the pool entry is committed before the observer runs
				job.OnFitDone(seq);
				job.Pump();
			});
			CameraLink::FramePhaseFacts f;
			f.seq = 1;
			rig.PublishInputs(1);
			{
				PassTimers::Accumulator earlier;   // what other tests left in the process-wide inbox
				PassTimers::Inbox::Motion().DrainInto(earlier);
			}
			Assert::IsTrue(core.SubmitField(CameraLinkTest::PanJob(1, 8).field, f));
			Assert::AreEqual(size_t(1), core.ProcessPending());
			Assert::AreEqual(size_t(1), seen.size());
			Assert::AreEqual(uint64_t(1), seen[0]);
			Assert::IsTrue(recordWasThere, L"the observer runs after the publish");
			// camera-live step 2: the fit cost goes to the motion pass-timer inbox (the camera-private timer queue is gone). The inbox is
			// process-wide, so the test drains it before the fit (in the rig setup above nothing fitted yet) and reads what this fit posted.
			PassTimers::Accumulator acc;
			Assert::IsTrue(PassTimers::Inbox::Motion().DrainInto(acc) >= 1);
			const PassTimers::Stat* fitRow = acc.Find(CameraLink::kTimerFit);
			Assert::IsNotNull(fitRow, L"camera.fit is fed");
			Assert::AreEqual(3.0, fitRow->lastMs, 1e-9);
			Assert::IsTrue(fitRow->kind == PassTimers::Kind::CpuWork, L"the fit is work");
			PassTimers::Accumulator again;
			PassTimers::Inbox::Motion().DrainInto(again);
			Assert::IsNull(again.Find(CameraLink::kTimerFit), L"drained");
			// Clearing the observer returns only when no call is in flight; afterwards a fit calls nothing.
			core.SetFitDoneObserver(nullptr);
			Assert::AreEqual(size_t(1), seen.size());
		}

		TEST_METHOD(TheCameraRowsReachThePassTimersAccumulatorAsWorkAndAsAWait)
		{
			// camera-live step 2: the rows go through the motion pass-timer inbox (the host drains it into the readout's accumulator).
			PassTimers::Accumulator acc;
			PassTimers::Inbox inbox;
			inbox.PostCameraFit(3.5);
			inbox.PostCameraReadback(12.0);
			inbox.DrainInto(acc);
			const PassTimers::Stat* fit = acc.Find("camera.fit");
			const PassTimers::Stat* readback = acc.Find("camera.readback");
			Assert::IsNotNull(fit);
			Assert::IsNotNull(readback);
			Assert::IsTrue(fit->kind == PassTimers::Kind::CpuWork);
			Assert::IsTrue(readback->kind == PassTimers::Kind::CpuWait, L"a latency is a wait, never counted as work");
			Assert::AreEqual(3.5, fit->lastMs, 1e-12);
			Assert::AreEqual(12.0, readback->lastMs, 1e-12);
		}

		// ---- K4-3: the PairContext provider ------------------------------------------------------------------------------

		TEST_METHOD(TheProviderFailsWhileP4IsNotPublishedAndCopiesTheFactsWhenItIs)
		{
			WireLink link;
			WireEngine engine;
			bool published = false;
			CameraLink::PairContext facts;
			facts.cut = true; facts.fade = true; facts.flash = true;
			facts.fadeGain = 0.5; facts.fadeOffset = 3; facts.nextPairSeconds = 0.04; facts.ancientStamp = true;
			auto provider = CameraLink::MakeContextProvider(&engine, link,
				[&](uint64_t, CameraLink::PairContext& out) { if (!published) return false; out = facts; return true; },
				[]() { return true; });
			CameraLink::PairContext ctx;
			ctx.recordSeq = 7;
			Assert::IsFalse(provider(10, ctx), L"P4(N) not published: the lane asks again later");
			published = true;
			Assert::IsTrue(provider(10, ctx));
			Assert::IsTrue(ctx.cut && ctx.fade && ctx.flash && ctx.ancientStamp);
			Assert::AreEqual(0.5, ctx.fadeGain, 0.0);
			Assert::AreEqual(3.0, ctx.fadeOffset, 0.0);
			Assert::AreEqual(0.04, ctx.nextPairSeconds, 0.0);
			Assert::AreEqual(uint64_t(7), ctx.recordSeq, L"the record the scheduler chose survives the reader");
			Assert::AreEqual(uint64_t(7), engine.askedFrom, L"PairSpansSlip(recordSeq, N)");
			Assert::AreEqual(uint64_t(10), engine.askedTo);
			Assert::IsFalse(ctx.spansSlip);
			engine.slip = true;
			Assert::IsTrue(provider(10, ctx));
			Assert::IsTrue(ctx.spansSlip);
			// d = 0: the slip is judged over camera(N)'s own pair, from its pairFrom.
			link.AddPan(10);
			ctx.recordSeq = 10;
			Assert::IsTrue(provider(10, ctx));
			Assert::AreEqual(link.records[10]->pairFrom, engine.askedFrom);
			Assert::AreEqual(uint64_t(10), engine.askedTo);
		}

		TEST_METHOD(OnTheCameraWorkerTheEngineIsNeverAskedAndAMaybeResidentFlowMakesThePairNotReady)
		{
			WireLink link;
			WireEngine engine;
			engine.slip = true;
			bool resident = false;
			auto provider = CameraLink::MakeContextProvider(&engine, link,
				[](uint64_t, CameraLink::PairContext&) { return true; },
				[]() { return false; },             // not the render thread
				[&resident]() { return resident; });
			CameraLink::PairContext ctx;
			ctx.recordSeq = 7;
			Assert::IsTrue(provider(10, ctx), L"no CPU flow: the lane may build it");
			Assert::AreEqual(0, engine.slipCalls, L"the engine is render-thread only");
			Assert::IsFalse(ctx.spansSlip, L"the slip is judged at consumption");
			resident = true;
			Assert::IsFalse(provider(10, ctx), L"a CPU flow may exist: the live fill adds it, so the lane stores nothing");
			auto unknown = CameraLink::MakeContextProvider(&engine, link, [](uint64_t, CameraLink::PairContext&) { return true; });
			Assert::IsFalse(unknown(10, ctx), L"no thread information: never asked, never stored");
			Assert::AreEqual(0, engine.slipCalls);
		}

		TEST_METHOD(AProviderWithoutAnEngineStillDescribesThePair)
		{
			WireLink link;
			auto provider = CameraLink::MakeContextProvider(nullptr, link,
				[](uint64_t, CameraLink::PairContext& out) { out.nextPairSeconds = 0.02; return true; });
			CameraLink::PairContext ctx;
			Assert::IsTrue(provider(5, ctx));
			Assert::IsFalse(ctx.spansSlip || ctx.flowCpuResident);
			Assert::AreEqual(0.02, ctx.nextPairSeconds, 0.0);
		}

		TEST_METHOD(TheFlowIsResampledOntoTheRecordGridOnlyWhenItIsCpuResident)
		{
			WireLink link;
			link.AddPan(1);   // 4K grid: 240 x 135 blocks of 16 source px
			const CameraModel::BlockData* bd = link.records[1]->blocks.get();
			Assert::IsNotNull(bd);
			WireEngine engine;
			engine.fx.assign(4 * 2, 0.0f);
			engine.fy.assign(4 * 2, 0.0f);
			for (int i = 0; i < 8; ++i)
			{
				engine.fx[i] = float(i);
				engine.fy[i] = float(-i);
			}
			engine.view.x = engine.fx.data();
			engine.view.y = engine.fy.data();
			engine.view.gridW = 4;
			engine.view.gridH = 2;
			engine.view.blockSize = bd->gridW * bd->blockSize / 4.0;   // the 4 x 2 cells cover the picture
			auto provider = CameraLink::MakeContextProvider(&engine, link,
				[](uint64_t, CameraLink::PairContext&) { return true; }, []() { return true; });
			CameraLink::PairContext ctx;
			ctx.recordSeq = 1;
			engine.cpuFlow = false;
			Assert::IsTrue(provider(3, ctx));
			Assert::IsFalse(ctx.flowCpuResident);
			Assert::IsTrue(ctx.nvofX.empty() && ctx.nvofY.empty(), L"a GPU-only flow gives no temporal-seed input");
			engine.cpuFlow = true;
			ctx.recordSeq = 1;
			Assert::IsTrue(provider(3, ctx));
			Assert::IsTrue(ctx.flowCpuResident);
			Assert::AreEqual(size_t(bd->gridW) * size_t(bd->gridH), ctx.nvofX.size());
			Assert::AreEqual(ctx.nvofX.size(), ctx.nvofY.size());
			engine.flowAvailable = false;
			ctx.recordSeq = 1;
			Assert::IsTrue(provider(3, ctx));
			Assert::IsFalse(ctx.flowCpuResident, L"no flow for this pair: nothing guessed");
			Assert::IsTrue(ctx.nvofX.empty());
		}

		TEST_METHOD(ResamplingTakesTheCellUnderEachBlockCentreAndRejectsUnusableViews)
		{
			std::vector<float> cx(8), cy(8);
			for (int i = 0; i < 8; ++i)
			{
				cx[i] = float(i);
				cy[i] = float(100 + i);
			}
			VpMotion::FlowView v;
			v.x = cx.data();
			v.y = cy.data();
			v.gridW = 4;
			v.gridH = 2;
			v.blockSize = 32;   // source px per cell
			std::vector<float> x, y;
			Assert::IsTrue(CameraLink::ResampleFlowToGrid(v, 8, 4, 16, x, y));   // 8 x 4 blocks of 16 px = the same 128 x 64 area
			Assert::AreEqual(size_t(32), x.size());
			for (int by = 0; by < 4; ++by)
				for (int bx = 0; bx < 8; ++bx)
				{
					const int cell = (by / 2) * 4 + (bx / 2);
					Assert::AreEqual(float(cell), x[by * 8 + bx], 0.0f);
					Assert::AreEqual(float(100 + cell), y[by * 8 + bx], 0.0f);
				}
			// A grid larger than the flow clamps to the edge cell.
			Assert::IsTrue(CameraLink::ResampleFlowToGrid(v, 10, 6, 16, x, y));
			Assert::AreEqual(float(7), x[5 * 10 + 9], 0.0f);
			VpMotion::FlowView bad = v;
			bad.x = nullptr;
			Assert::IsFalse(CameraLink::ResampleFlowToGrid(bad, 8, 4, 16, x, y));
			Assert::IsTrue(x.empty() && y.empty());
			bad = v;
			bad.blockSize = 0;
			Assert::IsFalse(CameraLink::ResampleFlowToGrid(bad, 8, 4, 16, x, y));
			Assert::IsFalse(CameraLink::ResampleFlowToGrid(v, 0, 4, 16, x, y));
		}

		TEST_METHOD(LaneAndLiveAgreeThroughTheProviderIncludingACutSkip)
		{
			WireLink link;
			PansAt(link, { 1, 2 });
			WireEngine engine;
			CameraLink::GuidanceSettings s = GuideOn();
			s.switches.cutSkip = true;
			CameraLink::GuidanceScheduler lane(link, s), live(link, s);
			bool render = false;
			for (CameraLink::GuidanceScheduler* g : { &lane, &live })
				g->SetContextProvider(CameraLink::MakeContextProvider(&engine, link,
					[](uint64_t, CameraLink::PairContext& out) { out.cut = true; out.nextPairSeconds = 1.0 / 24; return true; },
					[&render]() { return render; }, []() { return false; }));
			CameraLink::GuideLaneJob job(lane, link);
			job.OnFitDone(2);
			Assert::AreEqual(size_t(1), job.Pump(), L"the lane (worker side: no engine facts) stored it");
			render = true;
			const CameraLink::GuideResult a = lane.Guide(2, 1.0 / 24), b = live.Guide(2, 1.0 / 24);
			Assert::IsTrue(a.fromLane && !b.fromLane);
			Assert::IsTrue(a.frame != nullptr && b.frame != nullptr);
			Assert::IsTrue(a.frame->skipMatcher && b.frame->skipMatcher, L"the cut flag of P4(N) reached both");
			Assert::IsTrue(CameraLink::SameGuide(*a.frame, *b.frame));
		}

		// ---- K4: the adapter on P5a's vector engine -----------------------------------------------------------------------

		TEST_METHOD(AdapterWithAllSwitchesOffRegistersNothingAndBindsNothing)
		{
			FrameAnalysis::Store store;
			CameraLink::CameraLinkCore core(store);
			WireEngine engine;
			CameraLink::GuidanceAdapterHooks hooks;
			hooks.p4 = [](uint64_t, CameraLink::PairContext&) { return true; };
			hooks.onRenderThread = []() { return true; };
			CameraLink::CameraGuidanceAdapter adapter(core, CameraLink::GuidanceSettings(), hooks);
			adapter.Attach(&engine, std::unique_ptr<CameraLink::IGuidanceCountsSource>(new WireCounts()));
			Assert::IsTrue(adapter.Attached());
			Assert::IsFalse(adapter.Hooked());
			Assert::IsTrue(engine.hook == nullptr, L"no hook registered: the engine runs its Estimate only (byte-identical)");
			adapter.OnFrame();
			std::vector<std::string> lines;
			adapter.CollectStatus(lines);
			Assert::IsTrue(lines.empty(), L"nothing to show with the feature off");
		}

		TEST_METHOD(AdapterRegistersTheHookClearsItFirstOnDetachAndFollowsTheSwitches)
		{
			FrameAnalysis::Store store;
			CameraLink::CameraLinkCore core(store);
			WireEngine engine;
			CameraLink::GuidanceAdapterHooks hooks;
			hooks.p4 = [](uint64_t, CameraLink::PairContext&) { return true; };
			hooks.onRenderThread = []() { return true; };
			{
				CameraLink::CameraGuidanceAdapter adapter(core, GuideOn(), hooks);
				adapter.Attach(&engine, nullptr);
				Assert::IsTrue(adapter.Hooked());
				Assert::IsTrue(engine.hook == &adapter.Hook(), L"one hook on the engine");
				adapter.SetSettings(CameraLink::GuidanceSettings());
				Assert::IsTrue(engine.hook == nullptr, L"all off again: unregistered");
				Assert::IsFalse(adapter.Hooked());
				adapter.SetSettings(GuideOn());
				Assert::IsTrue(engine.hook == &adapter.Hook());
				const int clearsBefore = engine.hookClears;
				adapter.Detach();
				Assert::IsTrue(engine.hook == nullptr);
				Assert::AreEqual(clearsBefore + 1, engine.hookClears, L"the engine's hook is cleared by Detach");
				adapter.Attach(&engine, nullptr);
				Assert::IsTrue(engine.hook != nullptr);
			}
			Assert::IsTrue(engine.hook == nullptr, L"the destructor detaches: the engine never keeps a dead hook");
		}

		TEST_METHOD(AdapterDrivesGuidedEstimatesThroughTheEnginesHookAndCountsANotReadyCamera)
		{
			FrameAnalysis::Store store;
			CameraLink::CameraLinkCore core(store);
			WireEngine engine;
			CameraLink::GuidanceAdapterHooks hooks;
			hooks.p4 = [](uint64_t, CameraLink::PairContext& c) { c.nextPairSeconds = 1.0 / 24; return true; };
			hooks.onRenderThread = []() { return true; };
			CameraLink::CameraGuidanceAdapter adapter(core, GuideOn(), hooks);
			WireCounts* counts = new WireCounts();
			adapter.Attach(&engine, std::unique_ptr<CameraLink::IGuidanceCountsSource>(counts));
			// The engine calls the hook as VectorEngine::Estimate does.
			WireSink sink;
			VpMotion::EstimateContext ec;
			ec.pairSeq = 9;
			ec.nextPairSeconds = 1.0 / 24;
			engine.hook->BeforeEstimate(ec, sink);
			Assert::IsTrue(sink.last == nullptr, L"camera(9) not ready in time: unguided, an explicit null is bound");
			engine.hook->AfterEstimate(9, VpMotion::EstimateOutcome());
			const CameraLink::GuidanceCounters& c = adapter.Scheduler().Counters();
			Assert::AreEqual(uint64_t(1), c.refusals[size_t(CameraLink::Refusal::NotOnTime)]);
			uint64_t causes = 0;
			for (size_t m = 0; m < size_t(CameraLink::OnTimeMiss::Count); ++m)
				causes += c.notOnTime[m];
			Assert::AreEqual(uint64_t(1), causes, L"the miss is counted under exactly one cause");
			std::vector<FallbackCatalog::Decision> d = adapter.CollectDecisions();
			Assert::IsFalse(IsEngaged(d, CameraLink::kIdGuidanceRefused), L"expected at d = 0: counted, no held refusal entry");
			std::vector<std::string> lines;
			adapter.CollectStatus(lines);
			bool d0 = false;
			for (const std::string& l : lines)
				d0 = d0 || l.find("camera guidance (d=0): 0 guided on time") != std::string::npos;
			Assert::IsTrue(d0, L"the NotOnTime counter is visible through the adapter");
			adapter.OnDiscontinuity();
			adapter.OnFrame();
		}

		// ---- K4-4: the status through the skeleton's FallbackCatalog -------------------------------------------------------

		TEST_METHOD(AllSwitchesOffShowsNoGuidanceEntryEvenAfterManyPairs)
		{
			WireLink link;
			CameraLink::GuidanceScheduler sched(link, CameraLink::GuidanceSettings());
			for (uint64_t p = 3; p < 20; ++p)
				sched.Guide(p, 1.0 / 24);
			const std::vector<FallbackCatalog::Decision> d = sched.CollectDecisions();
			Assert::AreEqual(size_t(4), d.size(), L"every id is always listed so a Sync releases it");
			Assert::IsFalse(IsEngaged(d, CameraLink::kIdGuidanceRefused));
			Assert::IsFalse(IsEngaged(d, CameraLink::kIdGuidanceEscalations));
			Assert::IsFalse(IsEngaged(d, CameraLink::kIdGuidanceNarrowIdle));
			Assert::IsFalse(IsEngaged(d, CameraLink::kIdGuidanceUnguided));
		}

		static void RefusalShowsForHalfAMeasuredSecond(const std::vector<double>& dts)
		{
			WireLink link;
			link.AddPan(3);
			const std::shared_ptr<const CameraModel::CameraMotion> good = link.records[3];
			auto low = std::make_shared<CameraModel::CameraMotion>(*good);
			low->confidence = 0.6;
			link.records[3] = low;
			const uint64_t from = good->pairFrom;   // the matcher's A: the same pair whatever the measured duration
			CameraLink::GuidanceScheduler sched(link, GuideOn());
			Assert::IsTrue(sched.Guide(3, dts[0], from).refusal == CameraLink::Refusal::LowConfidence);
			std::vector<FallbackCatalog::Decision> d = sched.CollectDecisions();
			Assert::IsTrue(IsEngaged(d, CameraLink::kIdGuidanceRefused));
			for (const FallbackCatalog::Decision& x : d)
				if (x.id == CameraLink::kIdGuidanceRefused)
					Assert::IsTrue(x.reason.find("camera confidence too low") != std::string::npos, L"the reason is named");
			link.records[3] = good;
			double elapsed = 0, last = 0;
			size_t n = 0;
			while (IsEngaged(sched.CollectDecisions(), CameraLink::kIdGuidanceRefused) && n < 400)
			{
				last = dts[n % dts.size()];
				Assert::IsTrue(sched.Guide(3, last, from).frame != nullptr);
				elapsed += last;
				++n;
			}
			Assert::IsFalse(IsEngaged(sched.CollectDecisions(), CameraLink::kIdGuidanceRefused), L"released without any other action");
			Assert::IsTrue(elapsed >= CameraLink::kGuidanceRefusalHoldSeconds - 1e-9, L"not before 0.5 s of measured pair time");
			Assert::IsTrue(elapsed - last < CameraLink::kGuidanceRefusalHoldSeconds + 1e-9, L"and not later than that");
		}

		TEST_METHOD(ARefusalIsShownForHalfAMeasuredSecondAt24Fps) { RefusalShowsForHalfAMeasuredSecond({ 1.0 / 24 }); }
		TEST_METHOD(ARefusalIsShownForHalfAMeasuredSecondAt50Fps) { RefusalShowsForHalfAMeasuredSecond({ 1.0 / 50 }); }
		TEST_METHOD(ARefusalIsShownForHalfAMeasuredSecondAt60Fps) { RefusalShowsForHalfAMeasuredSecond({ 1.0 / 60 }); }
		TEST_METHOD(ARefusalIsShownForHalfAMeasuredSecondWithVariableFrameRate)
		{
			RefusalShowsForHalfAMeasuredSecond({ 0.1, 0.4, 0.05, 0.3, 0.2, 0.01, 0.2, 0.06 });
		}

		TEST_METHOD(AnUnknownPairDurationLetsTheRefusalHoldRunOutAtTheFirstGuidedPair)
		{
			CameraLink::RefusalHold hold;
			hold.Observe(CameraLink::Refusal::Slip, 0);
			Assert::IsTrue(hold.Engaged());
			Assert::IsTrue(hold.Last() == CameraLink::Refusal::Slip);
			hold.Observe(CameraLink::Refusal::None, 0);
			Assert::IsFalse(hold.Engaged(), L"no stuck entry when the duration was never measured");
			hold.Observe(CameraLink::Refusal::AllOff, 0.01);
			Assert::IsFalse(hold.Engaged(), L"the feature being off is not a fallback");
			hold.Observe(CameraLink::Refusal::Governor, 0.01);
			Assert::IsFalse(hold.Engaged(), L"the governor has its own entry");
			hold.Observe(CameraLink::Refusal::NotOnTime, 0.01);
			Assert::IsFalse(hold.Engaged(), L"d = 0: not ready in time is expected per pair, counted per cause, not held");
		}

		TEST_METHOD(ATrippedGovernorShowsTheEscalationsEntryAndItReleasesOnRecovery)
		{
			WireLink link;
			for (uint64_t q = 1; q <= 80; ++q)
				link.AddPan(q);
			CameraLink::GuidanceSettings s;
			s.switches.narrowWindow = true;
			CameraLink::GuidanceScheduler sched(link, s);
			uint64_t pair = 3;
			sched.Guide(pair, 1.0 / 24);
			const float bad[4] = { 1.0f, 1.0f, -1.0f, -1.0f };
			sched.OnCounts(pair, bad, 1.0 / 24);
			std::vector<FallbackCatalog::Decision> d = sched.CollectDecisions();
			Assert::IsTrue(IsEngaged(d, CameraLink::kIdGuidanceEscalations));
			for (const FallbackCatalog::Decision& x : d)
				if (x.id == CameraLink::kIdGuidanceEscalations)
					Assert::AreEqual(std::string("camera guidance: escalations high, full search"), x.reason);
			size_t n = 0;
			while (IsEngaged(sched.CollectDecisions(), CameraLink::kIdGuidanceEscalations) && n < 400)
			{
				sched.Guide(++pair, 1.0 / 50);
				++n;
			}
			Assert::IsFalse(IsEngaged(sched.CollectDecisions(), CameraLink::kIdGuidanceEscalations));
			Assert::IsTrue(n >= 49 && n <= 51, L"1 s of measured pair time at 50 fps");
		}

		TEST_METHOD(TheCountsAreOnlyReadWhenAPassThatWritesThemRan)
		{
			WireLink link;
			link.AddPan(3);
			CameraLink::GuidanceScheduler sched(link, GuideOn());
			WireCounts counts;
			CameraLink::CameraGuidanceHook hook(sched, &counts, link);
			WireSink sink;
			VpMotion::EstimateContext c;
			c.pairSeq = 3;
			c.nextPairSeconds = 1.0 / 24;
			hook.BeforeEstimate(c, sink);
			Assert::IsTrue(sink.last != nullptr);
			// The model global alone ran: the 1x1 counts texture was NOT written this pair (it holds an older pair's numbers).
			CameraLink::EstimateOutcome o;
			o.guidanceUsed = true;
			o.counts = FakeTex();
			hook.AfterEstimateEx(3, o);
			Assert::AreEqual(0, counts.requested, L"stale counts are never read");
			Assert::AreEqual(uint64_t(1), hook.UsedPairs(), L"but the pair was guided");
			// The E1 check ran (the only counting pass in NVOF seed mode): the counts are read a frame later.
			hook.BeforeEstimate(c, sink);
			o.e1Pass = true;
			hook.AfterEstimateEx(3, o);
			Assert::AreEqual(1, counts.requested);
		}

		TEST_METHOD(NarrowIdleUnderTheEngineIsShownForHalfAMeasuredSecondAndNoSeedIsDropped)
		{
			WireLink link;
			link.AddPan(3);
			link.Retime(3, 1.0 / 50);   // camera(3) measured the 50p pair the matcher is asked about
			CameraLink::GuidanceScheduler sched(link, GuideOn());
			CameraLink::CameraGuidanceHook hook(sched, nullptr, link);
			WireSink sink;
			VpMotion::EstimateContext c;
			c.pairSeq = 3;
			c.nextPairSeconds = 1.0 / 50;
			hook.BeforeEstimate(c, sink);
			CameraLink::EstimateOutcome o;
			o.guidanceUsed = true;
			o.e1Pass = true;                     // seed mode: the E1 check runs on the engine's vectors
			o.narrowDroppedEngine = true;        // the narrow window does not
			o.gpuNotApplied = "guided candidate: CPU plan only (GPU follow-up)";
			o.rejected = "camera guidance: record grid does not match the picture";
			o.gpuMs = 2.5;
			hook.AfterEstimateEx(3, o);
			Assert::IsTrue(IsEngaged(sched.CollectDecisions(), CameraLink::kIdGuidanceNarrowIdle));
			bool idleLine = false, notAppliedLine = false, rejectedLine = false;
			for (const std::string& l : sched.StatusLines())
			{
				idleLine = idleLine || l.find("engine field used, narrow idle") != std::string::npos;
				notAppliedLine = notAppliedLine || l.find("CPU plan only") != std::string::npos;
				rejectedLine = rejectedLine || l.find("record grid does not match") != std::string::npos;
			}
			Assert::IsTrue(idleLine && notAppliedLine && rejectedLine, L"every downgrade has a status line");
			Assert::AreEqual(uint64_t(1), sched.Counters().narrowIdleEngine);
			Assert::AreEqual(uint64_t(1), sched.Counters().rejectedByMatcher);
			double elapsed = 0;
			size_t n = 0;
			while (IsEngaged(sched.CollectDecisions(), CameraLink::kIdGuidanceNarrowIdle) && n < 400)
			{
				sched.Guide(4, 1.0 / 50);
				elapsed += 1.0 / 50;
				++n;
			}
			Assert::IsFalse(IsEngaged(sched.CollectDecisions(), CameraLink::kIdGuidanceNarrowIdle), L"released on its own");
			Assert::IsTrue(elapsed >= CameraLink::kGuidanceRefusalHoldSeconds - 1e-9 && elapsed < CameraLink::kGuidanceRefusalHoldSeconds + 0.03,
				L"after half a measured second (25 pairs at 50 fps)");
		}

		TEST_METHOD(TheBenchRowOfAGuidedPairCarriesTheEstimateMilliseconds)
		{
			const std::filesystem::path p = std::filesystem::temp_directory_path() / "camera-k4prep-bench-test.csv";
			{
				WireLink link;
				link.AddPan(3);
				CameraLink::MotionBenchWriter bench;
				bench.Open(p.string().c_str());
				CameraLink::GuidanceScheduler sched(link, GuideOn());
				sched.SetBench(&bench);
				Assert::IsTrue(sched.Guide(3, 1.0 / 24).frame != nullptr);
				sched.NoteGpuMs(3, 1.5);
				const float ok[4] = { 1e9f, 1e9f, -1.0f, -1.0f };
				sched.OnCounts(3, ok, 1.0 / 24);
				bench.Close();
			}
			std::ifstream in(p);
			std::string header, row, none;
			std::getline(in, header);
			std::getline(in, row);
			std::getline(in, none);
			in.close();
			std::filesystem::remove(p);
			Assert::IsTrue(row.size() > 7 && row.compare(row.size() - 7, 7, ",1.5000") == 0, L"the gpu_ms cell holds the noted figure");
			Assert::IsTrue(none.empty(), L"one row for the guided pair");
		}

		// ---- K4-5: the PassTimers rows --------------------------------------------------------------------------------------

		TEST_METHOD(TheRealPassDescriptionsFeedTheRightMotionRows)
		{
			using namespace CameraLink;
			// The descriptions DejudderWarp.cpp passes to Dispatch (a renamed pass must fail here, not vanish from a row).
			Assert::IsTrue(MotionRowOf("VideoProcessor de-judder: camera guided narrow search (plan 19)") == MotionRow::GuidedPass);
			Assert::IsTrue(MotionRowOf("VideoProcessor de-judder: camera guided E1 check (plan 19)") == MotionRow::GuidedPass);
			Assert::IsTrue(MotionRowOf("VideoProcessor de-judder: camera guidance counts (plan 19)") == MotionRow::GuidedPass);
			Assert::IsTrue(MotionRowOf("VideoProcessor de-judder: global motion (D1)") == MotionRow::D1);
			Assert::IsTrue(MotionRowOf("VideoProcessor de-judder: global motion pick (D1)") == MotionRow::D1);
			Assert::IsTrue(MotionRowOf("VideoProcessor de-judder: global motion (camera model)") == MotionRow::D1);
			Assert::IsTrue(MotionRowOf("VideoProcessor de-judder: global motion (engine)") == MotionRow::D1);
			Assert::IsTrue(MotionRowOf("VideoProcessor de-judder: block search") == MotionRow::BlockSearch);
			Assert::IsTrue(MotionRowOf("VideoProcessor de-judder: second sweep (11a E1)") == MotionRow::BlockSearch);
			Assert::IsTrue(MotionRowOf("VideoProcessor de-judder: consistency") == MotionRow::EstimateOnly);
			Assert::IsTrue(MotionRowOf("VideoProcessor de-judder: searches skipped (camera model: cut / flash)") == MotionRow::EstimateOnly);
			Assert::IsTrue(MotionRowOf("VideoProcessor de-judder: warp luma") == MotionRow::EstimateOnly);
		}

		TEST_METHOD(TheRowsSumTheCompletedPassTimersAndLeaveOutRowsWithNoTime)
		{
			using namespace CameraLink;
			std::map<std::string, double> ms;
			ms["VideoProcessor de-judder: global motion (D1)"] = 0.20;
			ms["VideoProcessor de-judder: global motion pick (D1)"] = 0.05;
			ms["VideoProcessor de-judder: block search"] = 1.00;
			ms["VideoProcessor de-judder: second sweep (11a E1)"] = 0.50;
			ms["VideoProcessor de-judder: camera guided narrow search (plan 19)"] = 0.30;
			ms["VideoProcessor de-judder: consistency"] = 0.25;
			ms["VideoProcessor de-judder: frame trust"] = 0.0;   // not completed yet: not a sample
			const std::map<std::string, double> rows = MotionTimerRows(ms);
			Assert::AreEqual(0.25, rows.at(kTimerD1), 1e-12);
			Assert::AreEqual(1.50, rows.at(kTimerBlockSearch), 1e-12);
			Assert::AreEqual(0.30, rows.at(kTimerGuidedPass), 1e-12);
			Assert::AreEqual(2.30, rows.at(kTimerEstimate), 1e-12);
			Assert::AreEqual(2.30, EstimateGpuMs(rows), 1e-12);
			// A matcher-only Estimate has no guided row (the guide is off: nothing to report).
			ms.erase("VideoProcessor de-judder: camera guided narrow search (plan 19)");
			Assert::AreEqual(size_t(0), MotionTimerRows(ms).count(kTimerGuidedPass));
			Assert::AreEqual(-1.0, EstimateGpuMs(MotionTimerRows(std::map<std::string, double>())), 0.0, L"nothing measured: no figure");
			Assert::AreEqual(std::string("motion.estimate"), std::string(kTimerEstimate));
			Assert::AreEqual(std::string("motion.block_search"), std::string(kTimerBlockSearch));
			Assert::AreEqual(std::string("motion.d1"), std::string(kTimerD1));
		}
	};
}
