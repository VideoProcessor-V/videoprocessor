// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "CppUnitTest.h"

// Camera link K4-2/3: the guidance gate. d = 0: pair N reads camera(N) only; every refusal reason gives a null guidance AND a counter; all
// switches off gives nullptr on every pair (nothing bound, byte-identical); the hook binds null when refused and counts the
// escalations a frame later. CPU only.
#include "CameraLinkTestSupport.h"
#include <VP-V/modules/motion/estimation/global_model/link/CameraGuideProduct.h>
#include <VP-V/modules/motion/estimation/global_model/link/GuidanceScheduler.h>

#include <map>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace VideoProcessorTest
{
	namespace
	{
		struct FakeLink : CameraLink::ICameraLink
		{
			std::map<uint64_t, std::shared_ptr<const CameraModel::CameraMotion>> records;
			std::map<uint64_t, std::shared_ptr<const CameraLink::FieldCopy>> fields;
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

			// A real fitted pan record at `seq`.
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

			// The camera measured pair `seq` over `seconds` (the same pair the matcher is asked about: SameCameraPair).
			void Retime(uint64_t seq, double seconds)
			{
				auto r = std::make_shared<CameraModel::CameraMotion>(*records[seq]);
				r->pairSeconds = seconds;
				records[seq] = r;
			}
		};

		struct FakeSink : VpMotion::IGuidanceSink
		{
			int calls = 0;
			std::shared_ptr<const CameraModel::GuidanceFrame> last;
			void SetCameraGuidance(std::shared_ptr<const CameraModel::GuidanceFrame> f) override { ++calls; last = f; }
		};

		struct FakeCounts : CameraLink::IGuidanceCountsSource
		{
			std::vector<std::pair<uint64_t, bool>> requested;
			std::vector<Done> ready;
			bool Request(uint64_t seq, pl_tex) override { requested.push_back(std::make_pair(seq, true)); return true; }
			size_t Poll(std::vector<Done>& out) override { const size_t n = ready.size(); out = ready; ready.clear(); return n; }
		};

		CameraLink::GuidanceSettings On()
		{
			CameraLink::GuidanceSettings s;
			s.switches.modelGlobal = true;
			s.switches.narrowWindow = true;
			s.switches.e1Skip = true;
			return s;
		}
	}

	TEST_CLASS(GuidanceSchedulingTests)
	{
	public:
		TEST_METHOD(PairNReadsCameraNOnlyAndAFillPairIsNotTheCamerasPair)
		{
			using CameraLink::Refusal;
			FakeLink link;
			link.AddPan(5);   // camera(5): the fit of pair 5 itself, 1/24 s
			CameraLink::GuidanceScheduler sched(link, On());
			Assert::IsTrue(sched.Guide(5, 1.0 / 24).frame != nullptr, L"d = 0: pair 5 is guided by camera(5)");
			CameraLink::GuideResult r = sched.Guide(6, 1.0 / 24);
			Assert::IsTrue(r.frame == nullptr && r.refusal == Refusal::NotOnTime, L"camera(6) absent: unguided, never camera(5)");
			r = sched.Guide(5, 2.0 / 24);
			Assert::IsTrue(r.frame == nullptr && r.refusal == Refusal::NotOnTime && r.miss == CameraLink::OnTimeMiss::NotThePair,
				L"a fill pair spanning two camera pairs is not the camera's pair");
			Assert::IsTrue(sched.Guide(5, 1.0 / 30).frame != nullptr, L"24p in 30: the 1/30 span of the camera's 1/24 pair");
			Assert::IsTrue(sched.Guide(5, 2.0 / 30).frame != nullptr, L"24p in 30: the 2/30 span (1.6x)");
			Assert::IsTrue(sched.Guide(5, 0).frame != nullptr, L"duration unknown: accepted");
			const CameraLink::GuidanceCounters& c = sched.Counters();
			Assert::AreEqual(uint64_t(2), c.refusals[size_t(Refusal::NotOnTime)]);
			Assert::AreEqual(uint64_t(1), c.notOnTime[size_t(CameraLink::OnTimeMiss::NotThePair)]);
			Assert::AreEqual(uint64_t(1), c.notOnTime[size_t(CameraLink::OnTimeMiss::NotPublished)], L"the fake link's default cause");
			const CameraModel::CameraMotion& rec = *link.records[5];
			Assert::IsTrue(CameraLink::SameCameraPair(rec, rec.pairFrom, 99.0), L"the matcher's A, when known, decides exactly");
			Assert::IsFalse(CameraLink::SameCameraPair(rec, rec.pairFrom + 1, 1.0 / 24));
		}

		// SHOULD 6: the engine's A (EstimateContext::fromSeq) reaches the scheduler through the hook and decides SameCameraPair exactly,
		// whatever the duration says.
		TEST_METHOD(TheHookPassesTheEnginesAToTheSameCameraPairCheck)
		{
			FakeLink link;
			link.AddPan(5);
			CameraLink::GuidanceScheduler sched(link, On());
			CameraLink::CameraGuidanceHook hook(sched, nullptr, link);
			FakeSink sink;
			const uint64_t from = link.records[5]->pairFrom;
			Assert::IsTrue(from != 0, L"the record knows its A");
			VpMotion::EstimateContext c;
			c.pairSeq = 5;
			c.fromSeq = from;
			c.nextPairSeconds = 3.0 / 24;   // outside the duration ratio: only the exact A can accept it
			hook.BeforeEstimate(c, sink);
			Assert::IsTrue(sink.last != nullptr, L"the camera's own A: guided");
			hook.AfterEstimate(5, VpMotion::EstimateOutcome());
			c.fromSeq = from + 1;
			c.nextPairSeconds = 1.0 / 24;   // inside the ratio: only the A mismatch refuses it
			hook.BeforeEstimate(c, sink);
			Assert::IsTrue(sink.last == nullptr, L"another A: not the camera's pair, unguided");
			hook.AfterEstimate(5, VpMotion::EstimateOutcome());
			Assert::AreEqual(uint64_t(1), sched.Counters().notOnTime[size_t(CameraLink::OnTimeMiss::NotThePair)]);
		}

		TEST_METHOD(AllSwitchesOffGivesNullOnEveryPairAndNothingIsBound)
		{
			FakeLink link;
			for (uint64_t q = 1; q < 12; ++q)
				link.AddPan(q);
			CameraLink::GuidanceSettings off;   // every switch false
			CameraLink::GuidanceScheduler sched(link, off);
			CameraLink::CameraGuidanceHook hook(sched, nullptr, link);
			FakeSink sink;
			for (uint64_t pair = 3; pair < 12; ++pair)
			{
				VpMotion::EstimateContext c;
				c.pairSeq = pair;
				c.nextPairSeconds = 1.0 / 24;
				hook.BeforeEstimate(c, sink);
				Assert::IsTrue(sink.last == nullptr, L"nothing bound");
				hook.AfterEstimate(pair, VpMotion::EstimateOutcome());
			}
			Assert::AreEqual(uint64_t(0), sched.Counters().guided);
			Assert::AreEqual(uint64_t(9), sched.Counters().refusals[size_t(CameraLink::Refusal::AllOff)]);
			Assert::IsTrue(sched.StatusLines().empty(), L"no status noise with the feature off");
		}

		TEST_METHOD(EveryRefusalReasonGivesNullAndACounter)
		{
			using CameraLink::Refusal;
			FakeLink link;
			link.AddPan(3);   // d = 0: pair 3 reads camera(3) = P7(3)
			CameraLink::GuidanceScheduler sched(link, On());
			// guided
			CameraLink::GuideResult r = sched.Guide(3);
			Assert::IsTrue(r.frame != nullptr && r.refusal == Refusal::None, L"the happy path really guides");
			Assert::IsTrue(r.frame->active);
			// camera(4) not ready in time (absent)
			r = sched.Guide(4);
			Assert::IsTrue(r.frame == nullptr && r.refusal == Refusal::NotOnTime);
			// slip
			sched.SetContextProvider([](uint64_t, CameraLink::PairContext& c) { c.spansSlip = true; return true; });
			r = sched.Guide(3);
			Assert::IsTrue(r.frame == nullptr && r.refusal == Refusal::Slip);
			sched.SetContextProvider(nullptr);
			// not usable: ambiguous
			auto amb = std::make_shared<CameraModel::CameraMotion>(*link.records[3]);
			amb->reason = CameraModel::Reason::Ambiguous;
			link.records[3] = amb;
			r = sched.Guide(3);
			Assert::IsTrue(r.frame == nullptr && r.refusal == Refusal::NotUsable);
			// low confidence (usable by reason, below the plan floor)
			auto low = std::make_shared<CameraModel::CameraMotion>(*link.records[3]);
			low->reason = CameraModel::Reason::Ok;
			low->confidence = 0.6;
			link.records[3] = low;
			r = sched.Guide(3);
			Assert::IsTrue(r.frame == nullptr && r.refusal == Refusal::LowConfidence);
			// d = 0: pair 2 reads camera(2) only, absent (never an older or newer record)
			r = sched.Guide(2);
			Assert::IsTrue(r.frame == nullptr && r.refusal == Refusal::NotOnTime);
			const CameraLink::GuidanceCounters& c = sched.Counters();
			Assert::AreEqual(uint64_t(1), c.guided);
			Assert::AreEqual(uint64_t(2), c.refusals[size_t(Refusal::NotOnTime)]);
			Assert::AreEqual(uint64_t(1), c.refusals[size_t(Refusal::Slip)]);
			Assert::AreEqual(uint64_t(1), c.refusals[size_t(Refusal::NotUsable)]);
			Assert::AreEqual(uint64_t(1), c.refusals[size_t(Refusal::LowConfidence)]);
			Assert::IsFalse(sched.StatusLines().empty(), L"refusals are visible");
		}

		// A narrow-only configuration has no guided pair to judge while tripped: the cool-down must still run on measured pair
		// time (Guide advances it for every pair asked) and the guidance must come back.
		static void TripAndRecover(const std::vector<double>& dts)
		{
			FakeLink link;
			for (uint64_t q = 1; q <= 80; ++q)
				link.AddPan(q);
			CameraLink::GuidanceSettings s;
			s.switches.narrowWindow = true;   // narrow only
			CameraLink::GuidanceScheduler sched(link, s);
			uint64_t pair = 3;
			link.Retime(pair, dts[0]);
			Assert::IsTrue(sched.Guide(pair, dts[0]).frame != nullptr, L"guided before the trip");
			const float bad[4] = { 1.0f, 1.0f, -1.0f, -1.0f };   // far fewer accepted than planned
			sched.OnCounts(pair, bad, dts[0]);
			Assert::IsTrue(sched.Governor().Tripped(), L"the escalations tripped the governor");
			Assert::AreEqual(uint32_t(1), sched.Governor().Trips());
			double elapsed = 0, last = 0;
			size_t n = 0;
			while (sched.Governor().Tripped() && n < 200)
			{
				++pair;
				last = dts[n % dts.size()];
				link.Retime(pair, last);
				const CameraLink::GuideResult r = sched.Guide(pair, last);
				elapsed += last;
				++n;
				if (sched.Governor().Tripped())
					Assert::IsTrue(r.frame == nullptr && r.refusal == CameraLink::Refusal::Governor, L"refused while cooling");
			}
			Assert::IsFalse(sched.Governor().Tripped(), L"recovered without any guided pair");
			Assert::IsTrue(elapsed >= 1.0 - 1e-9 && elapsed - last < 1.0, L"after exactly 1 s of measured pair time");
			Assert::AreEqual(uint32_t(1), sched.Governor().Recoveries());
			link.Retime(pair + 1, last);
			Assert::IsTrue(sched.Guide(pair + 1, last).frame != nullptr, L"guidance is back");
		}

		TEST_METHOD(AGovernorTripRefusesNarrowOnlyConfigurationsAndRecoversAt24Fps) { TripAndRecover({ 1.0 / 24 }); }
		TEST_METHOD(AGovernorTripRefusesNarrowOnlyConfigurationsAndRecoversAt60Fps) { TripAndRecover({ 1.0 / 60 }); }
		TEST_METHOD(AGovernorTripRefusesNarrowOnlyConfigurationsAndRecoversWithVariableFrameRate)
		{
			TripAndRecover({ 0.1, 0.4, 0.05, 0.3, 0.2, 0.01, 0.2, 0.06 });
		}

		TEST_METHOD(TheHookBindsNullWhenRefusedAndTheFrameWhenGuidedAndParsesCountsLater)
		{
			FakeLink link;
			link.AddPan(3);
			CameraLink::GuidanceSettings s = On();
			CameraLink::GuidanceScheduler sched(link, s);
			FakeCounts counts;
			CameraLink::CameraGuidanceHook hook(sched, &counts, link);
			FakeSink sink;
			VpMotion::EstimateContext c;
			c.pairSeq = 3;
			c.nextPairSeconds = 1.0 / 24;
			hook.BeforeEstimate(c, sink);
			Assert::IsTrue(sink.last != nullptr && sink.calls == 1);
			VpMotion::EstimateOutcome outcome;
			outcome.guidanceUsed = true;
			outcome.countsTex = reinterpret_cast<pl_tex>(static_cast<uintptr_t>(0x10));
			hook.AfterEstimate(3, outcome);
			Assert::AreEqual(size_t(1), counts.requested.size(), L"the 16 B counts are read a frame later");
			Assert::AreEqual(uint64_t(1), hook.UsedPairs());
			CameraLink::IGuidanceCountsSource::Done d;
			d.pairSeq = 3;
			d.v[0] = d.v[1] = 0.0f;   // nothing accepted: every planned narrow block escalated
			d.v[2] = d.v[3] = -1.0f;
			counts.ready.push_back(d);
			hook.Poll();
			Assert::AreEqual(uint64_t(1), sched.Diagnostics().frames, L"the finished pair reached MeasurementDiagnostics");
			Assert::IsTrue(sched.Diagnostics().escalated > 0, L"escalated = planned - accepted");
		}

		TEST_METHOD(ParseCountsComputesEscalatedAsPlannedMinusAccepted)
		{
			const float c1[4] = { 90.0f, 80.0f, -1.0f, -1.0f };
			CameraLink::GuidanceOutcome o = CameraLink::ParseGuidanceCounts(c1, 100, 0);
			Assert::IsTrue(o.valid);
			Assert::AreEqual(uint32_t(200), o.narrowPlanned);
			Assert::AreEqual(uint32_t(30), o.escalated);
			const float c2[4] = { -1.0f, -1.0f, 40.0f, -1.0f };
			o = CameraLink::ParseGuidanceCounts(c2, 100, 50);
			Assert::AreEqual(uint32_t(10), o.escalated);
			const float none[4] = { -1.0f, -1.0f, -1.0f, -1.0f };
			o = CameraLink::ParseGuidanceCounts(none, 100, 50);
			Assert::IsFalse(o.valid);
			Assert::AreEqual(uint32_t(0), o.escalated);
			const float over[4] = { 120.0f, -1.0f, -1.0f, -1.0f };
			Assert::AreEqual(uint32_t(0), CameraLink::ParseGuidanceCounts(over, 100, 0).escalated, L"never negative");
		}
	};
}
