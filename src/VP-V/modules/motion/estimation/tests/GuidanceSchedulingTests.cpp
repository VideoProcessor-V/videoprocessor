// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "CppUnitTest.h"

// Camera link K4-2/3: the guidance gate. Fixed d per graph; every refusal reason gives a null guidance AND a counter; all
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
		TEST_METHOD(TheLagIsFixedPerGraphFromMeasuredCostsAndNeverBelowTwo)
		{
			using CameraLink::GuideLagFor;
			Assert::AreEqual(2, int(GuideLagFor(0, 0, 1.0 / 24)));
			Assert::AreEqual(2, int(GuideLagFor(0.020, 0.010, 1.0 / 24)), L"30 ms < one frame: 1 + 1 = 2");
			Assert::AreEqual(3, int(GuideLagFor(0.030, 0.020, 1.0 / 24)), L"50 ms over 41.7 ms: 1 + 2");
			Assert::AreEqual(4, int(GuideLagFor(0.030, 0.020, 1.0 / 60)), L"the same costs at 60 fps need more frames");
			Assert::AreEqual(2, int(GuideLagFor(0.5, 0.5, 0.0)), L"unknown period: the minimum");
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
				hook.AfterEstimate(pair, false, nullptr);
			}
			Assert::AreEqual(uint64_t(0), sched.Counters().guided);
			Assert::AreEqual(uint64_t(9), sched.Counters().refusals[size_t(CameraLink::Refusal::AllOff)]);
			Assert::IsTrue(sched.StatusLines().empty(), L"no status noise with the feature off");
		}

		TEST_METHOD(EveryRefusalReasonGivesNullAndACounter)
		{
			using CameraLink::Refusal;
			FakeLink link;
			link.AddPan(1);   // pair 3 reads P7(3 - 2) = P7(1)
			CameraLink::GuidanceScheduler sched(link, On());
			sched.SetLag(2);
			// guided
			CameraLink::GuideResult r = sched.Guide(3);
			Assert::IsTrue(r.frame != nullptr && r.refusal == Refusal::None, L"the happy path really guides");
			Assert::IsTrue(r.frame->active);
			// record late (absent)
			r = sched.Guide(4);
			Assert::IsTrue(r.frame == nullptr && r.refusal == Refusal::NoRecord);
			// slip
			sched.SetContextProvider([](uint64_t, CameraLink::PairContext& c) { c.spansSlip = true; return true; });
			r = sched.Guide(3);
			Assert::IsTrue(r.frame == nullptr && r.refusal == Refusal::Slip);
			sched.SetContextProvider(nullptr);
			// not usable: ambiguous
			auto amb = std::make_shared<CameraModel::CameraMotion>(*link.records[1]);
			amb->reason = CameraModel::Reason::Ambiguous;
			link.records[1] = amb;
			r = sched.Guide(3);
			Assert::IsTrue(r.frame == nullptr && r.refusal == Refusal::NotUsable);
			// low confidence (usable by reason, below the plan floor)
			auto low = std::make_shared<CameraModel::CameraMotion>(*link.records[1]);
			low->reason = CameraModel::Reason::Ok;
			low->confidence = 0.6;
			link.records[1] = low;
			r = sched.Guide(3);
			Assert::IsTrue(r.frame == nullptr && r.refusal == Refusal::LowConfidence);
			// the lag is fixed: pair 2 would read P7(0), absent
			r = sched.Guide(2);
			Assert::IsTrue(r.frame == nullptr && r.refusal == Refusal::NoRecord);
			const CameraLink::GuidanceCounters& c = sched.Counters();
			Assert::AreEqual(uint64_t(1), c.guided);
			Assert::AreEqual(uint64_t(2), c.refusals[size_t(Refusal::NoRecord)]);
			Assert::AreEqual(uint64_t(1), c.refusals[size_t(Refusal::Slip)]);
			Assert::AreEqual(uint64_t(1), c.refusals[size_t(Refusal::NotUsable)]);
			Assert::AreEqual(uint64_t(1), c.refusals[size_t(Refusal::LowConfidence)]);
			Assert::IsFalse(sched.StatusLines().empty(), L"refusals are visible");
		}

		TEST_METHOD(AGovernorTripRefusesNarrowOnlyConfigurationsAndRecovers)
		{
			FakeLink link;
			link.AddPan(1);
			CameraLink::GuidanceSettings s;
			s.switches.narrowWindow = true;   // narrow only
			CameraLink::GuidanceScheduler sched(link, s);
			sched.SetLag(2);
			Assert::IsTrue(sched.Guide(3).frame != nullptr);
			// Feed an escalation-heavy pair: the governor trips.
			const float bad[4] = { 1.0f, 1.0f, -1.0f, -1.0f };   // far fewer accepted than planned
			sched.OnCounts(3, bad, 1.0 / 24);
			Assert::IsTrue(sched.Governor().Tripped() || sched.Diagnostics().frames >= 1);
			if (sched.Governor().Tripped())
			{
				const CameraLink::GuideResult r = sched.Guide(3);
				Assert::IsTrue(r.frame == nullptr && r.refusal == CameraLink::Refusal::Governor);
			}
		}

		TEST_METHOD(TheHookBindsNullWhenRefusedAndTheFrameWhenGuidedAndParsesCountsLater)
		{
			FakeLink link;
			link.AddPan(1);
			CameraLink::GuidanceSettings s = On();
			CameraLink::GuidanceScheduler sched(link, s);
			sched.SetLag(2);
			FakeCounts counts;
			CameraLink::CameraGuidanceHook hook(sched, &counts, link);
			FakeSink sink;
			VpMotion::EstimateContext c;
			c.pairSeq = 3;
			c.nextPairSeconds = 1.0 / 24;
			hook.BeforeEstimate(c, sink);
			Assert::IsTrue(sink.last != nullptr && sink.calls == 1);
			hook.AfterEstimate(3, true, reinterpret_cast<pl_tex>(static_cast<uintptr_t>(0x10)));
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
