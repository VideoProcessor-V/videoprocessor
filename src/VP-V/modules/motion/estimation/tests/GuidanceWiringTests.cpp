// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "CppUnitTest.h"

// Camera link K4 wiring (prepared before P5a): the one refusal gate, the measured guide lag, the guide lane job (lane == live),
// the PairContext provider and the flow resampler, the skeleton-visible status (FallbackCatalog decisions) and the PassTimers
// rows. CPU only; every piece is driven through fakes of the engine and the link. No test name carries the word for the device.
#include "CameraLinkTestSupport.h"
#include <VP-V/modules/motion/estimation/global_model/link/CameraLinkCore.h>
#include <VP-V/modules/motion/estimation/global_model/link/GuideLaneJob.h>
#include <VP-V/modules/motion/estimation/global_model/link/GuidanceBracket.h>
#include <VP-V/modules/motion/estimation/global_model/link/GuidanceContext.h>
#include <VP-V/modules/motion/estimation/global_model/link/GuidanceGate.h>
#include <VP-V/modules/motion/estimation/global_model/link/GuidanceLag.h>
#include <VP-V/modules/motion/estimation/global_model/link/GuidanceScheduler.h>
#include <VP-V/modules/motion/estimation/global_model/link/GuidanceStatus.h>
#include <VP-V/modules/motion/estimation/global_model/link/GuidanceTimers.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
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
		};

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
			void SetPreEstimateHook(VpMotion::IPreEstimateHook*) override {}
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

		// The matcher as the bracket sees it (MotionWarp's SetCameraGuidance / CameraGuidanceUsed / CameraGuidanceCounts).
		struct WireWarp
		{
			struct Use
			{
				bool modelGlobal = false, staticFromModel = false, matcherSkipped = false, narrowPass = false, e1Pass = false,
					fadeNormalised = false, narrowDroppedEngine = false;
				const char* rejected = "";
				const char* gpuNotApplied = "";
			};
			std::shared_ptr<const CameraModel::GuidanceFrame> bound;
			int sets = 0;
			Use use;
			void SetCameraGuidance(std::shared_ptr<const CameraModel::GuidanceFrame> f) { bound = std::move(f); ++sets; }
			Use CameraGuidanceUsed() const { return use; }
			pl_tex CameraGuidanceCounts() const { return FakeTex(); }
		};

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
			Assert::IsTrue(GuidanceRefusal(on, cfg, nullptr, false, false) == Refusal::NoRecord, L"record late");
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

		// ---- K4-1: the measured guide lag -------------------------------------------------------------------------------

		TEST_METHOD(TheSampleWindowIgnoresNonPositiveSamplesAndUsesNearestRank)
		{
			CameraLink::SampleWindow w(128);
			w.Add(0);
			w.Add(-1);
			Assert::AreEqual(size_t(0), w.Count());
			Assert::AreEqual(0.0, w.P95(), 0.0, L"empty: nothing measured");
			for (int i = 1; i <= 20; ++i)
				w.Add(i);
			Assert::AreEqual(19.0, w.P95(), 0.0, L"nearest rank: ceil(0.95 * 20) = 19");
			Assert::AreEqual(20.0, w.Quantile(1.0), 0.0);
			Assert::AreEqual(1.0, w.Quantile(0.0), 0.0);
			CameraLink::SampleWindow small(4);
			for (int i = 1; i <= 6; ++i)
				small.Add(i);
			Assert::AreEqual(size_t(4), small.Count(), L"a sliding window: the oldest samples left");
			Assert::AreEqual(3.0, small.Quantile(0.0), 0.0);
			Assert::AreEqual(6.0, small.Quantile(1.0), 0.0);
		}

		TEST_METHOD(TheLagFollowsMeasuredFitAndReadbackCostsOverTheMeasuredSourcePeriod)
		{
			CameraLink::GuideLagMeter m;
			Assert::AreEqual(2, int(m.Lag(1.0 / 24, false)), L"nothing measured: the structural floor");
			for (int i = 1; i <= 100; ++i)
				m.AddFitSeconds(i * 0.001);   // p95 = 95 ms
			for (int i = 0; i < 10; ++i)
				m.AddReadbackSeconds(0.010);
			Assert::AreEqual(0.095, m.P95FitSeconds(), 1.5e-3);
			Assert::AreEqual(0.010, m.P95ReadbackSeconds(), 1e-12);
			// 105 ms of work: 2.5 source periods at 24 fps -> ceil 3 -> d = 4; 6.3 at 60 fps -> 7 -> d = 8; 5.25 at 50 fps -> 6 -> d = 7.
			Assert::AreEqual(4, int(m.Lag(1.0 / 24, false)));
			Assert::AreEqual(8, int(m.Lag(1.0 / 60, false)));
			Assert::AreEqual(7, int(m.Lag(1.0 / 50, false)));
			Assert::AreEqual(2, int(m.Lag(0.0, false)), L"unknown source period: the floor");
			Assert::AreEqual(2, int(m.Lag(1.0 / 24, true)), L"a deterministic run keeps the floor: d must not depend on timing");
			m.Clear();
			Assert::AreEqual(2, int(m.Lag(1.0 / 24, false)));
		}

		TEST_METHOD(TheLagIsNeverAboveTheMaximumWhateverTheCosts)
		{
			CameraLink::GuideLagMeter m;
			m.AddFitSeconds(60.0);
			Assert::AreEqual(int(CameraLink::kMaxGuideLag), int(m.Lag(1.0 / 60, false)));
		}

		// ---- K4-2: the guide lane job ---------------------------------------------------------------------------------

		TEST_METHOD(AFitWantsThePairDFramesLaterAndAPairWithoutP4StaysWantedUntilItArrives)
		{
			WireLink link;
			link.AddPan(1);
			CameraLink::GuidanceScheduler sched(link, GuideOn());
			sched.SetLag(2);
			bool p4Published = false;
			sched.SetContextProvider([&p4Published](uint64_t, CameraLink::PairContext&) { return p4Published; });
			CameraLink::GuideLaneJob job(sched);
			job.OnFitDone(1);
			Assert::AreEqual(size_t(1), job.Pending(), L"pair 1 + d = 3 is wanted");
			Assert::AreEqual(size_t(0), job.Pump(), L"P4(3) is not published yet");
			Assert::AreEqual(size_t(1), job.Pending(), L"still wanted");
			Assert::AreEqual(uint64_t(1), job.GetStats().retried);
			p4Published = true;
			Assert::AreEqual(size_t(1), job.Pump());
			Assert::AreEqual(size_t(0), job.Pending());
			Assert::AreEqual(uint64_t(1), sched.Counters().lanePublished);
			const CameraLink::GuideResult r = sched.Guide(3);
			Assert::IsTrue(r.fromLane, L"the render used the lane's result");
		}

		TEST_METHOD(TheLaneResultEqualsTheLiveResultByteForByte)
		{
			WireLink link;
			link.AddPan(1);
			CameraLink::GuidanceScheduler lane(link, GuideOn()), live(link, GuideOn());
			lane.SetLag(2);
			live.SetLag(2);
			CameraLink::GuideLaneJob job(lane);
			job.OnFitDone(1);
			Assert::AreEqual(size_t(1), job.Pump());
			const CameraLink::GuideResult a = lane.Guide(3);
			const CameraLink::GuideResult b = live.Guide(3);
			Assert::IsTrue(a.fromLane && !b.fromLane);
			Assert::IsTrue(a.refusal == b.refusal);
			Assert::IsTrue(a.frame != nullptr && b.frame != nullptr, L"the pan is guided");
			Assert::IsTrue(CameraLink::SameGuide(*a.frame, *b.frame));
		}

		TEST_METHOD(ALaneResultAfterTheRenderAskedIsDiscardedAndCounted)
		{
			WireLink link;
			link.AddPan(1);
			CameraLink::GuidanceScheduler sched(link, GuideOn());
			sched.SetLag(2);
			CameraLink::GuideLaneJob job(sched);
			const CameraLink::GuideResult live = sched.Guide(3);
			Assert::IsTrue(live.frame != nullptr && !live.fromLane);
			job.OnFitDone(1);
			Assert::AreEqual(size_t(0), job.Pump());
			Assert::AreEqual(size_t(0), job.Pending(), L"judged: it does not stay wanted");
			Assert::AreEqual(uint64_t(1), job.GetStats().discarded);
			Assert::AreEqual(uint64_t(1), sched.Counters().laneDiscarded);
		}

		TEST_METHOD(WithAllSwitchesOffTheLaneJobWantsNothingAndStoresNothing)
		{
			WireLink link;
			link.AddPan(1);
			CameraLink::GuidanceScheduler sched(link, CameraLink::GuidanceSettings());
			sched.SetLag(2);
			CameraLink::GuideLaneJob job(sched);
			Assert::IsFalse(sched.LaneWanted());
			job.OnFitDone(1);
			Assert::AreEqual(size_t(0), job.Pending());
			Assert::AreEqual(size_t(0), job.Pump());
			Assert::AreEqual(uint64_t(0), sched.Counters().lanePublished);
			Assert::AreEqual(uint64_t(0), job.GetStats().wanted);
		}

		TEST_METHOD(TheWantedSetIsBoundedDuplicatesAreIgnoredAndAResetForgets)
		{
			WireLink link;
			CameraLink::GuidanceScheduler sched(link, GuideOn());
			sched.SetLag(2);
			sched.SetContextProvider([](uint64_t, CameraLink::PairContext&) { return false; });
			CameraLink::GuideLaneJob job(sched, 2);
			job.OnFitDone(1);
			job.OnFitDone(1);
			Assert::AreEqual(size_t(1), job.Pending(), L"the same pair is wanted once");
			job.OnFitDone(2);
			job.OnFitDone(3);
			Assert::AreEqual(size_t(2), job.Pending(), L"bounded");
			Assert::AreEqual(uint64_t(1), job.GetStats().dropped, L"the oldest was dropped and counted");
			job.Reset();
			Assert::AreEqual(size_t(0), job.Pending());
		}

		TEST_METHOD(TheLaneUsesTheSnapshotLagOfTheScheduler)
		{
			WireLink link;
			CameraLink::GuidanceScheduler sched(link, GuideOn());
			sched.SetLag(4);
			sched.SetContextProvider([](uint64_t, CameraLink::PairContext&) { return false; });
			Assert::AreEqual(4, int(sched.SnapshotLag()));
			CameraLink::GuideLaneJob job(sched);
			job.OnFitDone(10);
			// The pair it wants is 14: asking the scheduler about 14 with a record at 10 publishes it.
			link.AddPan(10);
			sched.SetContextProvider(nullptr);
			Assert::AreEqual(size_t(1), job.Pump());
			Assert::IsTrue(sched.Guide(14).fromLane);
		}

		TEST_METHOD(TheCoreCallsTheFitObserverAfterThePublishAndTheLaneJobStoresTheGuide)
		{
			CameraLinkTest::Rig rig;
			FrameAnalysis::Store store;
			CameraLink::CameraLinkCore core(store);
			CameraLink::CameraLinkSettings on;
			on.p7Wanted = true;
			on.deterministic = true;
			core.OnSettings(on);
			core.AttachGraph(rig.table.get(), rig.bytes.get(), rig.indicator.get());
			CameraLink::TimingFacts tf;
			tf.measuredSourceHz = 24;
			core.SetTimingFacts(tf);
			core.Producer()->NoteTiming();
			CameraLink::GuidanceScheduler sched(*core.Link(), GuideOn());
			sched.SetLag(2);
			CameraLink::GuideLaneJob job(sched);
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
			Assert::IsTrue(core.SubmitField(CameraLinkTest::PanJob(1, 8).field, f));
			Assert::AreEqual(size_t(1), core.ProcessPending());
			Assert::AreEqual(size_t(1), seen.size());
			Assert::AreEqual(uint64_t(1), seen[0]);
			Assert::IsTrue(recordWasThere, L"the observer runs after the publish");
			Assert::AreEqual(uint64_t(1), sched.Counters().lanePublished, L"the lane stored the guide of pair 1 + 2");
			Assert::IsTrue(sched.Guide(3).fromLane);
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
				[]() { return uint8_t(3); });
			CameraLink::PairContext ctx;
			Assert::IsFalse(provider(10, ctx), L"P4(N) not published: the lane asks again later");
			published = true;
			Assert::IsTrue(provider(10, ctx));
			Assert::IsTrue(ctx.cut && ctx.fade && ctx.flash && ctx.ancientStamp);
			Assert::AreEqual(0.5, ctx.fadeGain, 0.0);
			Assert::AreEqual(3.0, ctx.fadeOffset, 0.0);
			Assert::AreEqual(0.04, ctx.nextPairSeconds, 0.0);
			Assert::AreEqual(uint64_t(7), engine.askedFrom, L"PairSpansSlip(N - d, N)");
			Assert::AreEqual(uint64_t(10), engine.askedTo);
			Assert::IsFalse(ctx.spansSlip);
			engine.slip = true;
			Assert::IsTrue(provider(10, ctx));
			Assert::IsTrue(ctx.spansSlip);
		}

		TEST_METHOD(AProviderWithoutAnEngineStillDescribesThePair)
		{
			WireLink link;
			auto provider = CameraLink::MakeContextProvider(nullptr, link,
				[](uint64_t, CameraLink::PairContext& out) { out.nextPairSeconds = 0.02; return true; }, nullptr);
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
				[](uint64_t, CameraLink::PairContext&) { return true; }, []() { return uint8_t(2); });
			CameraLink::PairContext ctx;
			engine.cpuFlow = false;
			Assert::IsTrue(provider(3, ctx));
			Assert::IsFalse(ctx.flowCpuResident);
			Assert::IsTrue(ctx.nvofX.empty() && ctx.nvofY.empty(), L"a GPU-only flow gives no temporal-seed input");
			engine.cpuFlow = true;
			Assert::IsTrue(provider(3, ctx));
			Assert::IsTrue(ctx.flowCpuResident);
			Assert::AreEqual(size_t(bd->gridW) * size_t(bd->gridH), ctx.nvofX.size());
			Assert::AreEqual(ctx.nvofX.size(), ctx.nvofY.size());
			engine.flowAvailable = false;
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
			link.AddPan(1);
			WireEngine engine;
			CameraLink::GuidanceSettings s = GuideOn();
			s.switches.cutSkip = true;
			CameraLink::GuidanceScheduler lane(link, s), live(link, s);
			lane.SetLag(2);
			live.SetLag(2);
			for (CameraLink::GuidanceScheduler* g : { &lane, &live })
			{
				CameraLink::GuidanceScheduler* sp = g;
				g->SetContextProvider(CameraLink::MakeContextProvider(&engine, link,
					[](uint64_t, CameraLink::PairContext& out) { out.cut = true; out.nextPairSeconds = 1.0 / 24; return true; },
					[sp]() { return sp->SnapshotLag(); }));
			}
			CameraLink::GuideLaneJob job(lane);
			job.OnFitDone(1);
			Assert::AreEqual(size_t(1), job.Pump());
			const CameraLink::GuideResult a = lane.Guide(3, 1.0 / 24), b = live.Guide(3, 1.0 / 24);
			Assert::IsTrue(a.fromLane && !b.fromLane);
			Assert::IsTrue(a.frame != nullptr && b.frame != nullptr);
			Assert::IsTrue(a.frame->skipMatcher && b.frame->skipMatcher, L"the cut flag of P4(N) reached both");
			Assert::IsTrue(CameraLink::SameGuide(*a.frame, *b.frame));
		}

		// ---- K4-4: the status through the skeleton's FallbackCatalog -------------------------------------------------------

		TEST_METHOD(AllSwitchesOffShowsNoGuidanceEntryEvenAfterManyPairs)
		{
			WireLink link;
			CameraLink::GuidanceScheduler sched(link, CameraLink::GuidanceSettings());
			sched.SetLag(2);
			for (uint64_t p = 3; p < 20; ++p)
				sched.Guide(p, 1.0 / 24);
			const std::vector<FallbackCatalog::Decision> d = sched.CollectDecisions();
			Assert::AreEqual(size_t(3), d.size(), L"every id is always listed so a Sync releases it");
			Assert::IsFalse(IsEngaged(d, CameraLink::kIdGuidanceRefused));
			Assert::IsFalse(IsEngaged(d, CameraLink::kIdGuidanceEscalations));
			Assert::IsFalse(IsEngaged(d, CameraLink::kIdGuidanceNarrowIdle));
		}

		static void RefusalShowsForHalfAMeasuredSecond(const std::vector<double>& dts)
		{
			WireLink link;
			CameraLink::GuidanceScheduler sched(link, GuideOn());
			sched.SetLag(2);
			sched.Guide(3, dts[0]);   // no record: refused
			std::vector<FallbackCatalog::Decision> d = sched.CollectDecisions();
			Assert::IsTrue(IsEngaged(d, CameraLink::kIdGuidanceRefused));
			for (const FallbackCatalog::Decision& x : d)
				if (x.id == CameraLink::kIdGuidanceRefused)
					Assert::IsTrue(x.reason.find("camera record late") != std::string::npos, L"the reason is named");
			link.AddPan(1);
			double elapsed = 0, last = 0;
			size_t n = 0;
			while (IsEngaged(sched.CollectDecisions(), CameraLink::kIdGuidanceRefused) && n < 400)
			{
				last = dts[n % dts.size()];
				Assert::IsTrue(sched.Guide(3, last).frame != nullptr);
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
		}

		TEST_METHOD(ATrippedGovernorShowsTheEscalationsEntryAndItReleasesOnRecovery)
		{
			WireLink link;
			for (uint64_t q = 1; q <= 80; ++q)
				link.AddPan(q);
			CameraLink::GuidanceSettings s;
			s.switches.narrowWindow = true;
			CameraLink::GuidanceScheduler sched(link, s);
			sched.SetLag(2);
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
			link.AddPan(1);
			CameraLink::GuidanceScheduler sched(link, GuideOn());
			sched.SetLag(2);
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
			link.AddPan(1);
			CameraLink::GuidanceScheduler sched(link, GuideOn());
			sched.SetLag(2);
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
				link.AddPan(1);
				CameraLink::MotionBenchWriter bench;
				bench.Open(p.string().c_str());
				CameraLink::GuidanceScheduler sched(link, GuideOn());
				sched.SetLag(2);
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

		// ---- K4-3: the bracket around Estimate ----------------------------------------------------------------------------------

		TEST_METHOD(ABracketWithNoHookBindsNothingAndReadsNothing)
		{
			WireWarp w;
			w.use.narrowPass = true;
			CameraLink::BeforeGuidedEstimate(nullptr, w, 5, false, 0.04);
			CameraLink::AfterGuidedEstimate(nullptr, w, 5);
			CameraLink::AfterGuidedEstimateEx(nullptr, w, 5, 1.0);
			Assert::AreEqual(0, w.sets, L"nothing registered: the matcher is not touched (byte-identical)");
		}

		TEST_METHOD(TheBracketBindsTheGuideThenReportsWhatTheMatcherDid)
		{
			WireLink link;
			link.AddPan(1);
			CameraLink::GuidanceScheduler sched(link, GuideOn());
			sched.SetLag(2);
			WireCounts counts;
			CameraLink::CameraGuidanceHook hook(sched, &counts, link);
			WireWarp w;
			CameraLink::BeforeGuidedEstimate(&hook, w, 3, false, 1.0 / 24);
			Assert::AreEqual(1, w.sets);
			Assert::IsTrue(w.bound != nullptr, L"the guide reached the matcher");
			w.use.narrowPass = true;
			w.use.modelGlobal = true;
			CameraLink::AfterGuidedEstimate(&hook, w, 3);
			Assert::AreEqual(1, counts.requested, L"a counting pass ran: the 16 B are read a frame later");
			Assert::AreEqual(uint64_t(1), hook.UsedPairs());
			// A pair with no record is refused: the matcher is handed an explicit null (today's search), and nothing is counted.
			CameraLink::BeforeGuidedEstimate(&hook, w, 4, true, 1.0 / 24);
			Assert::AreEqual(2, w.sets);
			Assert::IsTrue(w.bound == nullptr);
			w.use = WireWarp::Use();
			CameraLink::AfterGuidedEstimate(&hook, w, 4);
			Assert::AreEqual(1, counts.requested);
			Assert::AreEqual(uint64_t(1), hook.UsedPairs());
		}

		TEST_METHOD(OutcomeOfMapsTheMatcherFieldsAndHidesCountsThatNoPassWrote)
		{
			WireWarp w;
			w.use.modelGlobal = true;
			CameraLink::EstimateOutcome o = CameraLink::OutcomeOf(w, 1.5);
			Assert::IsTrue(o.guidanceUsed);
			Assert::IsTrue(o.counts == nullptr, L"the model global alone writes no counts: the texture is stale");
			Assert::AreEqual(1.5, o.gpuMs, 0.0);
			w.use.e1Pass = true;
			w.use.narrowDroppedEngine = true;
			w.use.rejected = "rejected";
			w.use.gpuNotApplied = "not applied";
			o = CameraLink::OutcomeOf(w, -1.0);
			Assert::IsTrue(o.e1Pass && !o.narrowPass && o.narrowDroppedEngine);
			Assert::IsTrue(o.counts == FakeTex());
			Assert::AreEqual(std::string("rejected"), std::string(o.rejected));
			Assert::AreEqual(std::string("not applied"), std::string(o.gpuNotApplied));
			w.use = WireWarp::Use();
			Assert::IsFalse(CameraLink::OutcomeOf(w, -1.0).guidanceUsed);
			w.use.matcherSkipped = true;
			Assert::IsTrue(CameraLink::OutcomeOf(w, -1.0).guidanceUsed, L"a skipped cut pair is a guided effect");
		}

		TEST_METHOD(TheExtendedBracketFeedsTheDiagnosticsAndTheBenchMilliseconds)
		{
			WireLink link;
			link.AddPan(1);
			CameraLink::GuidanceScheduler sched(link, GuideOn());
			sched.SetLag(2);
			CameraLink::CameraGuidanceHook hook(sched, nullptr, link);
			WireWarp w;
			CameraLink::BeforeGuidedEstimate(&hook, w, 3, false, 1.0 / 24);
			w.use.e1Pass = true;
			w.use.narrowDroppedEngine = true;
			CameraLink::AfterGuidedEstimateEx(&hook, w, 3, 2.5);
			Assert::AreEqual(uint64_t(1), sched.Counters().narrowIdleEngine);
			Assert::AreEqual(uint64_t(1), hook.UsedPairs());
		}

		TEST_METHOD(ASinkWithoutAMatcherIsHarmless)
		{
			CameraLink::WarpGuidanceSink<WireWarp> sink(nullptr);
			sink.SetCameraGuidance(nullptr);
			WireWarp w;
			CameraLink::WarpGuidanceSink<WireWarp> live(&w);
			live.SetCameraGuidance(std::shared_ptr<const CameraModel::GuidanceFrame>());
			Assert::AreEqual(1, w.sets);
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
