// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// W2-C1 (C1-4, B9 "one motion-engine state"): the engine-absent pipeline. Pure CPU, 600 frames per scenario, every want ON. Each
// frame runs the renderer's per-frame order with the product code: VectorEngine::PlanProducer (null device) -> the VectorDemand
// input (RunDejudderPolicy's starter rule, DejudderPolicyRunner.inl) -> VectorEngine::UpdateVectorEngine (VectorDemand::Decide,
// the engine state, the fallback entries) -> RunDejudderDecision (DejudderDecisionRunnerHost.h) -> the pair check and estimate.
// The matcher's own counter sites live in renderer files (EngineKeeper.inl warm-up, VectorEngineEstimate.inl create / estimate,
// DejudderPolicyRunner.inl analysis estimate); they are modelled here at the decision level with the same conditions: a warm-up
// creates the matcher, a PairCheck outcome creates it if absent and estimates once, an analysis PairCheck counts once more. The
// producer's counters (producerStarts / producerPairs) and refusedStarts are the engine's own (Work()).
//
// Scenarios: (1) optical_flow off; (2) NVIDIA optical flow unavailable (VectorEngine::FailProducerStartsForTesting, rule 25);
// (3) libplacebo smooth motion; (4) competitive low latency (no queue ahead); (5) nothing absent. Every scenario runs with all
// starters off (no motion work at all, every reuse-only want refused and named); (1) and (2) also with the de-judder on (Degraded:
// the matcher runs, the producer never starts), (3) and (4) with it on too (the de-judder cannot act there: still nothing runs).
// Built without the test project's precompiled header (as VectorEngineTests): keep windows.h from defining min/max.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "CppUnitTest.h"

#include <VP-V/modules/motion/estimation/VectorEngine.h>
#include <VP-V/modules/motion/estimation/VectorEngineInterface.h>
#include <VP-V/modules/motion/consumers/tests/DejudderDecisionRunnerHost.h>

#include <atomic>
#include <map>
#include <string>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using namespace VpMotion;

namespace EngineAbsentPipelineTest
{
	namespace
	{
		using C = VectorDemand::Consumer;
		using R = VectorDemand::MotionEngineReason;
		using S = VectorDemand::MotionEngineState;

		enum class Scenario { OpticalFlowOff, NvofUnavailable, SmoothMotion, CompetitiveLowLatency, NothingAbsent };

		struct FakeHost : IVectorEngineHost
		{
			std::map<std::string, FallbackCatalog::Decision> last;   // the last decision synced per entry id
			void SyncFallback(const FallbackCatalog::Decision& d) override { last[d.id] = d; }
			void PublishStatus(bool) override {}
			bool DitherVectorsSupported() const override { return true; }
			void NoteSourceMonitorMotion(double, uint64_t) override {}
			bool IntentActive() const override { return true; }
			void NoteIntentVectors(const IntentInterpreter::VectorSample&) override {}
		};

		// The renderer's matcher counters (MotionEngineWork fields the CPU engine cannot reach), at the decision level.
		struct MatcherModel
		{
			bool held = false;
			uint64_t creates = 0, estimates = 0, warmUps = 0, analysisEstimates = 0;
		};

		struct Result
		{
			VectorDemand::MotionEngineStatus state;
			VectorDemand::MotionEngineWork work;
			VectorDemand::Decision decision;
			MatcherModel matcher;
			DejudderPolicy::Reason dejudderReason = DejudderPolicy::Reason::Off;
			std::map<std::string, FallbackCatalog::Decision> entries;
		};

		bool IsReuseOnly(C c) { return !VectorDemand::IsStarter(c); }

		Result RunScenario(Scenario sc, bool startersOn, int frames = 600)
		{
			VectorEngine engine;
			FakeHost host;
			if (sc == Scenario::NvofUnavailable)
				engine.FailProducerStartsForTesting("NVIDIA optical flow is not available on this adapter (test)");
			const bool lowLatency = sc == Scenario::CompetitiveLowLatency;
			const bool smoothMotion = sc == Scenario::SmoothMotion;
			const bool opticalFlow = sc != Scenario::OpticalFlowOff;
			std::atomic<bool> lowLatencyFlag(lowLatency);
			std::atomic<size_t> queuedAhead(lowLatency ? 0u : 4u);

			DejudderDecisionRunner::Rig rig;
			DejudderDecisionLegacy::Facts f;
			f.activeSettings.dejudderStrength = startersOn ? 1.0 : 0.0;
			f.activeSettings.interpolation = InterpolationPolicy::Mode::Off;
			f.activeSettings.interpolationFillGaps = startersOn;   // hiccup repair (the rig's default is on)
			f.activeSettings.occlusionMask = true;                 // a reuse-only want
			f.activeSettings.vectorDemand = true;
			f.adaptiveGrainMapWanted = true;
			f.sourceHz = 23.976;
			f.measuredDisplayHz = 60.0;
			f.haveNext = true;
			f.nextMatches = true;
			f.smoothMotionEngine = smoothMotion;
			f.lowLatency = lowLatency;
			f.motionEngineWarmPending = true;   // a new source: the keeper may warm the idle engine once
			f.sourceSequence = 10;
			rig.Begin(f);

			MatcherModel m;
			for (int frame = 0; frame < frames; ++frame)
			{
				// 1. PlanProducer (LVR ~23160): previous-frame de-judder state, a null device.
				const DejudderPolicy::Reason reason = rig.st.dejudderReason;
				VectorEngine::ProducerInputs pin;
				pin.lowLatency = &lowLatencyFlag;
				pin.queuedFutureFrames = &queuedAhead;
				pin.opticalFlow = opticalFlow;
				pin.motionAheadFrames = 4;
				pin.dejudderPresent = m.held;
				pin.dejudderEstimating = reason == DejudderPolicy::Reason::Active || reason == DejudderPolicy::Reason::NoNextFrame ||
					reason == DejudderPolicy::Reason::NextFrameDiffers;
				pin.dejudderCanEngage = m.held && DejudderPolicy::CanEngageOrAcquiring(reason, false);
				pin.sourceHz = f.sourceHz;
				pin.gpu = nullptr;
				engine.PlanProducer(pin);

				// 2. The demand input (VectorDemandInput, LVR:5847): every want ON; the starter wants follow the settings.
				VectorDemand::Input in;
				in.enabled = true;
				in.slotsRun = true;
				in.lowLatency = lowLatency;
				in.queueAheadCapacity = lowLatency ? 0 : 4;
				in.smoothMotion = smoothMotion;
				for (int i = 0; i < VectorDemand::kConsumerCount; ++i)
					in.wants[i] = IsReuseOnly(VectorDemand::ConsumerAt(i));
				in.wants[static_cast<int>(C::DejudderExtras)] = f.activeSettings.dejudderStrength > 0.0;
				in.wants[static_cast<int>(C::HiccupRepair)] = f.activeSettings.interpolationFillGaps;
				// RunDejudderPolicy's starter rule (DejudderPolicyRunner.inl): the standing de-judder, the interpolator, a gap fill.
				const bool dejudderStanding = m.held && f.activeSettings.dejudderStrength > 0.0 &&
					DejudderPolicy::CanEngageOrAcquiring(reason, false);
				in.starterEngaged = dejudderStanding || rig.st.interpolating || f.vpGapFillStandalone;

				// 3. UpdateVectorEngine (the decision, the state, the entries).
				VectorEngine::UpdateFacts facts;
				facts.vectorDemandSetting = true;
				facts.sigmaDeltaRanLast = true;
				facts.opticalFlow = opticalFlow;
				engine.NoteVramRelease(rig.motionEngineIdle.Pressured() && !m.held);
				engine.UpdateVectorEngine(in, facts, host);

				// 4. The de-judder decision with this frame's demand.
				f.vectorDemand = engine.vectorDemand;
				f.dejudder = m.held;
				rig.ApplyInputs(f);
				rig.warmed = rig.released = false;
				rig.analysisEstimate = false;
				rig.Run();
				if (rig.warmed)
				{
					++m.warmUps;
					if (!m.held)
						++m.creates;
					m.held = true;
				}
				if (rig.released)
					m.held = false;
				// 5. The pair check and the estimate (VectorEngineEstimate.inl).
				if (rig.outcome == DejudderDecisionLegacy::Outcome::PairCheck)
				{
					if (!m.held)
					{
						++m.creates;
						m.held = true;
					}
					++m.estimates;
					if (rig.analysisEstimate)
						++m.analysisEstimates;
					f.dejudderEstimatedSequence = f.sourceSequence;
				}
				++f.sourceSequence;
				rig.nowTick += 42;
			}
			Result r;
			r.state = engine.State();
			r.work = engine.Work();
			r.decision = engine.vectorDemand;
			r.matcher = m;
			r.dejudderReason = rig.st.dejudderReason;
			r.entries = host.last;
			return r;
		}

		std::wstring W(const std::string& s) { return std::wstring(s.begin(), s.end()); }

		const FallbackCatalog::Decision& Entry(const Result& r, const char* id)
		{
			const auto it = r.entries.find(id);
			Assert::IsTrue(it != r.entries.end(), W(std::string("entry synced: ") + id).c_str());
			return it->second;
		}

		bool Contains(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }

		// Starters off: nothing runs, every reuse-only want is refused (or unserved for its own reason) and named.
		void AssertNoMotionWork(const Result& r, Scenario sc)
		{
			Assert::AreEqual(0ull, static_cast<unsigned long long>(r.matcher.creates), L"matcherCreates");
			Assert::AreEqual(0ull, static_cast<unsigned long long>(r.matcher.estimates), L"matcherEstimates");
			Assert::AreEqual(0ull, static_cast<unsigned long long>(r.matcher.warmUps), L"warmUps");
			Assert::AreEqual(0ull, static_cast<unsigned long long>(r.matcher.analysisEstimates), L"analysisEstimates");
			Assert::AreEqual(0ull, static_cast<unsigned long long>(r.work.matcherCreates + r.work.matcherEstimates + r.work.warmUps +
				r.work.analysisEstimates), L"no renderer counter moved");
			Assert::AreEqual(0ull, static_cast<unsigned long long>(r.work.producerStarts), L"producerStarts");
			Assert::AreEqual(0ull, static_cast<unsigned long long>(r.work.producerPairs), L"producerPairs");
			Assert::IsTrue(r.state.state == S::Off, L"state Off");
			Assert::IsTrue(r.state.reason == R::NoStarter, L"leading reason NoStarter");
			Assert::IsTrue(r.state.Has(R::LowLatency) == (sc == Scenario::CompetitiveLowLatency), L"the low-latency bit only in scenario 4");
			const std::string engineText = Entry(r, "motion engine").reason;
			Assert::IsTrue(Entry(r, "motion engine").engaged, L"the motion engine entry is engaged");
			Assert::IsTrue(Contains(engineText, "motion engine off: "), W(engineText).c_str());
			Assert::IsTrue(Contains(engineText, "take their non-motion path"), W(engineText).c_str());
			const FallbackCatalog::Decision& demand = Entry(r, "vector_engine");
			Assert::IsTrue(demand.engaged, L"the vector_engine entry is engaged");
			for (int i = 0; i < VectorDemand::kConsumerCount; ++i)
			{
				const C c = VectorDemand::ConsumerAt(i);
				if (!IsReuseOnly(c))
					continue;
				const std::string name = VectorDemand::Name(c);
				Assert::IsTrue(Contains(engineText, name), W("motion engine entry names " + name + ": " + engineText).c_str());
				Assert::IsTrue(Contains(demand.reason, name), W("vector_engine entry names " + name + ": " + demand.reason).c_str());
				if (sc == Scenario::SmoothMotion && c == C::SigmaDelta)
				{
					// Smooth motion owns the output: unserved for that reason, not a refused start.
					Assert::AreEqual(0ull, static_cast<unsigned long long>(r.work.refusedStarts[i]), L"output dither: not refused");
					Assert::IsTrue(Contains(demand.reason, "output dither without vectors (libplacebo smooth motion owns output)"),
						W(demand.reason).c_str());
					continue;
				}
				Assert::AreEqual(600ull, static_cast<unsigned long long>(r.work.refusedStarts[i]), W("refusedStarts " + name).c_str());
			}
			Assert::IsTrue(Contains(demand.reason, "without vectors (no starter runs the motion engine)"), W(demand.reason).c_str());
		}
	}

	TEST_CLASS(EngineAbsentPipelineTests)
	{
	public:
		TEST_METHOD(StartersOffOpticalFlowOffRunsNothing) { AssertNoMotionWork(RunScenario(Scenario::OpticalFlowOff, false), Scenario::OpticalFlowOff); }
		TEST_METHOD(StartersOffNvofUnavailableRunsNothing) { AssertNoMotionWork(RunScenario(Scenario::NvofUnavailable, false), Scenario::NvofUnavailable); }
		TEST_METHOD(StartersOffSmoothMotionRunsNothing) { AssertNoMotionWork(RunScenario(Scenario::SmoothMotion, false), Scenario::SmoothMotion); }
		TEST_METHOD(StartersOffCompetitiveLowLatencyRunsNothing)
		{
			AssertNoMotionWork(RunScenario(Scenario::CompetitiveLowLatency, false), Scenario::CompetitiveLowLatency);
		}
		TEST_METHOD(StartersOffNothingAbsentRunsNothing) { AssertNoMotionWork(RunScenario(Scenario::NothingAbsent, false), Scenario::NothingAbsent); }

		// (1) with the de-judder on: the matcher runs alone (Degraded, deliberate: the entry stays released), the producer never starts.
		TEST_METHOD(DejudderOnOpticalFlowOffIsDegradedWithoutTheProducer)
		{
			const Result r = RunScenario(Scenario::OpticalFlowOff, true);
			Assert::IsTrue(r.dejudderReason == DejudderPolicy::Reason::Active, L"the de-judder acts (23.976 on 60 Hz)");
			Assert::IsTrue(r.state.state == S::Degraded, L"Degraded");
			Assert::IsTrue(r.state.reason == R::OpticalFlowOff, L"leading reason OpticalFlowOff");
			Assert::AreEqual(0ull, static_cast<unsigned long long>(r.work.producerStarts), L"producerStarts");
			Assert::AreEqual(0ull, static_cast<unsigned long long>(r.work.producerPairs), L"producerPairs");
			Assert::AreEqual(1ull, static_cast<unsigned long long>(r.matcher.creates), L"one matcher, kept (no churn)");
			Assert::IsTrue(r.matcher.estimates >= 590, L"the matcher estimates (nearly) every frame");
			Assert::IsFalse(Entry(r, "motion engine").engaged, L"optical flow off is deliberate: the entry is released");
			Assert::IsTrue(r.decision.refused == 0, L"a starter runs: no refused start at the end");
		}

		// (2) with the de-judder on: the producer's start fails (the seam), the matcher runs; the entry names the block searches.
		TEST_METHOD(DejudderOnNvofUnavailableIsDegradedAndVisible)
		{
			const Result r = RunScenario(Scenario::NvofUnavailable, true);
			Assert::IsTrue(r.dejudderReason == DejudderPolicy::Reason::Active, L"the de-judder acts (23.976 on 60 Hz)");
			Assert::IsTrue(r.state.state == S::Degraded, L"Degraded");
			Assert::IsTrue(r.state.reason == R::NvofUnavailable, L"leading reason NvofUnavailable");
			Assert::AreEqual(0ull, static_cast<unsigned long long>(r.work.producerStarts), L"producerStarts");
			Assert::AreEqual(0ull, static_cast<unsigned long long>(r.work.producerPairs), L"producerPairs");
			Assert::AreEqual(1ull, static_cast<unsigned long long>(r.matcher.creates), L"one matcher, kept (no churn)");
			const FallbackCatalog::Decision& e = Entry(r, "motion engine");
			Assert::IsTrue(e.engaged, L"the motion engine entry is engaged");
			Assert::IsTrue(Contains(e.reason, "motion engine degraded: NVIDIA optical flow is not available"), W(e.reason).c_str());
			Assert::IsTrue(Contains(e.reason, "use the block searches without optical-flow pairs"), W(e.reason).c_str());
		}

		// (3) and (4) with the de-judder on: it cannot act there (SmoothMotion / LowLatency reasons), so nothing starts the engine.
		TEST_METHOD(DejudderOnSmoothMotionStillRunsNothing)
		{
			const Result r = RunScenario(Scenario::SmoothMotion, true);
			Assert::IsTrue(r.dejudderReason == DejudderPolicy::Reason::SmoothMotion, L"the de-judder yields to smooth motion");
			Assert::AreEqual(0ull, static_cast<unsigned long long>(r.matcher.creates + r.matcher.estimates + r.matcher.warmUps +
				r.matcher.analysisEstimates), L"no matcher work");
			Assert::AreEqual(0ull, static_cast<unsigned long long>(r.work.producerStarts + r.work.producerPairs), L"no producer work");
			Assert::IsTrue(r.state.state == S::Off, L"Off");
		}
		TEST_METHOD(DejudderOnCompetitiveLowLatencyStillRunsNothing)
		{
			const Result r = RunScenario(Scenario::CompetitiveLowLatency, true);
			Assert::IsTrue(r.dejudderReason == DejudderPolicy::Reason::LowLatency, L"the de-judder is off in low latency");
			Assert::AreEqual(0ull, static_cast<unsigned long long>(r.matcher.creates + r.matcher.estimates + r.matcher.warmUps +
				r.matcher.analysisEstimates), L"no matcher work");
			Assert::AreEqual(0ull, static_cast<unsigned long long>(r.work.producerStarts + r.work.producerPairs), L"no producer work");
			Assert::IsTrue(r.state.state == S::Off, L"Off");
			Assert::IsTrue(r.state.Has(R::LowLatency), L"the low-latency reason is shown");
		}
	};
}
