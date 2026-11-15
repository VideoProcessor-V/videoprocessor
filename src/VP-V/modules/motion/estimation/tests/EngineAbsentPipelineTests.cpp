// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// W2-C1 (C1-4, B9 "one motion-engine state"): the engine-absent pipeline. Pure CPU, 600 frames per scenario, every want ON. Each
// frame runs the renderer's per-frame order with the product code: VectorEngine::PlanProducer (null device) -> the VectorDemand
// input (VectorDemandInput: the synthesis wants follow the settings) -> VectorEngine::UpdateVectorEngine (VectorDemand::DecideForHost,
// the engine state, the fallback entries) -> RunDejudderDecision (DejudderDecisionRunnerHost.h) -> the pair check and estimate.
// The matcher's own counter sites live in renderer files (EngineKeeper.inl warm-up, VectorEngineEstimate.inl create / estimate,
// DejudderPolicyRunner.inl analysis estimate); they are modelled here at the decision level with the same conditions: a warm-up
// creates the matcher, a PairCheck outcome creates it if absent and estimates once, an analysis PairCheck counts once more. The
// producer's counters (producerStarts / producerPairs) are the engine's own (Work()).
//
// Scenarios: (1) optical_flow off; (2) NVIDIA optical flow unavailable (VectorEngine::FailProducerStartsForTesting, rule 25);
// (3) libplacebo smooth motion; (4) competitive low latency (no queue ahead); (5) nothing absent.
// Batch B (lead18 plan-motion-engine-switch.md, WAIVER-B1-ENGINE-01): the engine is ONE switch (vector_demand), there is no starter.
// - Engine OFF (the setting) x every scenario, with the de-judder, hiccup smoothing and the mask on in the user's settings: the Q3
//   resolver (SlotSwitchSettings::ApplyEngineOff, as PostLoad runs it) forces them off visibly; no motion work at all; every want
//   is named in the "motion engine off (setting)" line; no Fallbacks entry (Q2).
// - Engine ON: (5) serves every want (no matcher while pairs are expected); (1) and (2) run the named matcher fallback (Degraded);
//   (1)-(4) with the de-judder on as before. The CPU rig has no device, so the producer never starts: a served engine is Degraded
//   for want of a running producer (the producer's first frames: no entry), On on a GPU.
// Built without the test project's precompiled header (as VectorEngineTests): keep windows.h from defining min/max.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "CppUnitTest.h"

#include <VP-V/modules/motion/estimation/VectorEngine.h>
#include <VP-V/modules/motion/estimation/VectorEngineInterface.h>
#include <VP-V/modules/motion/consumers/tests/DejudderDecisionRunnerHost.h>
#include <VP-V/core/SlotSwitchSettings.h>

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
			DejudderDecisionLegacy::Settings settings;   // the settings after the Q3 resolver
		};

		// Batch B: the rig's always-on wants: every non-synthesis consumer the renderer's input can register (the camera fit is declared
		// by the camera link, SetCameraFitWanted, and gated on its own: VectorEngineTests). The synthesis wants follow the settings.
		bool AlwaysWanted(C c) { return VectorDemand::DomainOf(c) != VectorDemand::Domain::Synthesis && c != C::CameraFit; }
		unsigned AlwaysWantedBits()
		{
			unsigned bits = 0;
			for (int i = 0; i < VectorDemand::kConsumerCount; ++i)
				if (AlwaysWanted(VectorDemand::ConsumerAt(i)))
					bits |= 1u << i;
			return bits;
		}
		unsigned StatisticsBits() { return (1u << static_cast<int>(C::SourceMonitorPan)) | (1u << static_cast<int>(C::MotionIntent)); }

		// engineOn = the vector_demand setting; dejudderOn = the user's de-judder (strength 1), hiccup smoothing and occlusion mask.
		Result RunScenario(Scenario sc, bool engineOn, bool dejudderOn, int frames = 600)
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
			f.activeSettings.dejudderStrength = dejudderOn ? 1.0 : 0.0;
			f.activeSettings.interpolation = InterpolationPolicy::Mode::Off;
			f.activeSettings.interpolationFillGaps = dejudderOn;   // hiccup repair (the rig's default is on)
			f.activeSettings.occlusionMask = dejudderOn;           // a de-judder extra (synthesis want)
			f.activeSettings.vectorDemand = engineOn;
			// Q3: PostLoad's resolver (VpvRendererSettings.cpp), so OFF forces the sub-selections off with its line.
			f.activeSettings.engineOffForced = ::SlotSwitchSettings::DescribeEngineOff(::SlotSwitchSettings::ApplyEngineOff(f.activeSettings));
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

				// 2. The demand input (VectorDemandInput, LVR:5735): enabled = vector_demand; every non-synthesis want ON; the
				// synthesis wants follow the (resolved) settings. Batch B: no starter rule.
				VectorDemand::Input in;
				in.enabled = f.activeSettings.vectorDemand;
				in.slotsRun = true;
				in.lowLatency = lowLatency;
				in.queueAheadCapacity = lowLatency ? 0 : 4;
				in.smoothMotion = smoothMotion;
				for (int i = 0; i < VectorDemand::kConsumerCount; ++i)
					in.wants[i] = AlwaysWanted(VectorDemand::ConsumerAt(i));
				in.wants[static_cast<int>(C::DejudderExtras)] = f.activeSettings.dejudderStrength > 0.0 || f.activeSettings.dejudderStrengthEven > 0.0;
				in.wants[static_cast<int>(C::Interpolator)] = f.activeSettings.interpolation != InterpolationPolicy::Mode::Off;
				in.wants[static_cast<int>(C::HiccupRepair)] = f.activeSettings.interpolationFillGaps;
				in.wants[static_cast<int>(C::OcclusionMask)] = f.activeSettings.occlusionMask;

				// 3. UpdateVectorEngine (the decision, the state, the entries).
				VectorEngine::UpdateFacts facts;
				facts.vectorDemandSetting = f.activeSettings.vectorDemand;
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
			r.settings = f.activeSettings;
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

		void AssertNoMatcherWork(const Result& r)
		{
			Assert::AreEqual(0ull, static_cast<unsigned long long>(r.matcher.creates + r.matcher.estimates + r.matcher.warmUps +
				r.matcher.analysisEstimates), L"no matcher work");
			Assert::AreEqual(0ull, static_cast<unsigned long long>(r.work.matcherCreates + r.work.matcherEstimates + r.work.warmUps +
				r.work.analysisEstimates), L"no renderer counter moved");
		}

		// Batch B (was AssertNoMotionWork for "starters off"): engine OFF by the setting. Nothing runs (zero matcher and producer work),
		// the de-judder / hiccup smoothing / mask the user had on are forced off with the Q3 line, every want is named in the OFF line,
		// and nothing is a Fallbacks entry (Q2). The reasons: VectorDemandOff, plus LowLatency in scenario 4 (competitive without
		// vectors; it leads, enum order).
		void AssertEngineOffRunsNothing(const Result& r, Scenario sc)
		{
			Assert::AreEqual(0ull, static_cast<unsigned long long>(r.matcher.creates), L"matcherCreates");
			Assert::AreEqual(0ull, static_cast<unsigned long long>(r.matcher.estimates), L"matcherEstimates");
			Assert::AreEqual(0ull, static_cast<unsigned long long>(r.matcher.warmUps), L"warmUps");
			Assert::AreEqual(0ull, static_cast<unsigned long long>(r.matcher.analysisEstimates), L"analysisEstimates");
			Assert::AreEqual(0ull, static_cast<unsigned long long>(r.work.matcherCreates + r.work.matcherEstimates + r.work.warmUps +
				r.work.analysisEstimates), L"no renderer counter moved");
			Assert::AreEqual(0ull, static_cast<unsigned long long>(r.work.producerStarts), L"producerStarts");
			Assert::AreEqual(0ull, static_cast<unsigned long long>(r.work.producerPairs), L"producerPairs");
			Assert::IsFalse(r.decision.producer || r.decision.estimate, L"the decision starts nothing");
			Assert::AreEqual(0u, r.decision.served, L"nothing served");
			Assert::IsTrue(r.state.state == S::Off, L"state Off");
			Assert::IsTrue(r.state.Has(R::VectorDemandOff), L"VectorDemandOff");
			Assert::IsFalse(r.state.Has(R::NoStarter), L"NoStarter is never derived");
			const bool competitive = sc == Scenario::CompetitiveLowLatency;
			Assert::IsTrue(r.state.Has(R::LowLatency) == competitive, L"the low-latency bit only in scenario 4");
			Assert::IsTrue(r.state.reason == (competitive ? R::LowLatency : R::VectorDemandOff), L"leading reason");
			Assert::AreEqual(0u, r.state.affected, L"nothing affected");
			Assert::IsFalse(FallbackCatalog::MotionEngine(r.state).engaged, L"Q2: the motion engine entry is released");
			for (const auto& e : r.entries)
				Assert::IsFalse(e.second.engaged, W("Q2: no Fallbacks entry: " + e.first + ": " + e.second.reason).c_str());
			const std::string off = VectorDemand::OffLine(r.decision);
			Assert::IsTrue(off.compare(0, 29, "motion engine off (setting): ") == 0, W(off).c_str());
			Assert::IsTrue(Contains(off, " use their non-motion path"), W(off).c_str());
			for (int i = 0; i < VectorDemand::kConsumerCount; ++i)
			{
				const C c = VectorDemand::ConsumerAt(i);
				if (!AlwaysWanted(c))
					continue;
				const std::string name = VectorDemand::Name(c);
				Assert::IsTrue(Contains(off, name), W("the OFF line names " + name + ": " + off).c_str());
			}
			Assert::AreEqual(AlwaysWantedBits(), r.decision.wants, L"exactly the always-on wants: the forced synthesis wants are gone");
			// Q3: the resolver forced the de-judder (and its mask) and hiccup smoothing off, visibly.
			Assert::AreEqual(std::string("de-judder off: motion engine off; hiccup smoothing off: motion engine off"), r.settings.engineOffForced);
			Assert::IsTrue(r.settings.dejudderStrength == 0.0 && !r.settings.interpolationFillGaps && !r.settings.occlusionMask &&
				r.settings.interpolation == InterpolationPolicy::Mode::Off, L"the sub-selections are off");
			Assert::IsTrue(r.dejudderReason == DejudderPolicy::Reason::Off, L"the de-judder is off");
		}
	}

	TEST_CLASS(EngineAbsentPipelineTests)
	{
	public:
		// Batch B: the five StartersOff* become EngineOff x scenario (the user's de-judder, hiccup smoothing and mask ON in the file).
		TEST_METHOD(EngineOffOpticalFlowOffRunsNothing) { AssertEngineOffRunsNothing(RunScenario(Scenario::OpticalFlowOff, false, true), Scenario::OpticalFlowOff); }
		TEST_METHOD(EngineOffNvofUnavailableRunsNothing) { AssertEngineOffRunsNothing(RunScenario(Scenario::NvofUnavailable, false, true), Scenario::NvofUnavailable); }
		TEST_METHOD(EngineOffSmoothMotionRunsNothing) { AssertEngineOffRunsNothing(RunScenario(Scenario::SmoothMotion, false, true), Scenario::SmoothMotion); }
		TEST_METHOD(EngineOffCompetitiveLowLatencyRunsNothing)
		{
			AssertEngineOffRunsNothing(RunScenario(Scenario::CompetitiveLowLatency, false, true), Scenario::CompetitiveLowLatency);
		}
		TEST_METHOD(EngineOffNothingAbsentRunsNothing) { AssertEngineOffRunsNothing(RunScenario(Scenario::NothingAbsent, false, true), Scenario::NothingAbsent); }

		// Q3 (owner 2026-11-15): the same source and settings, engine ON: the de-judder acts; engine OFF: it is off, with its line.
		TEST_METHOD(EngineOffTurnsTheDejudderOffWithItsLine)
		{
			const Result on = RunScenario(Scenario::NothingAbsent, true, true);
			Assert::IsTrue(on.dejudderReason == DejudderPolicy::Reason::Active, L"engine on: the de-judder acts (23.976 on 60 Hz)");
			Assert::AreEqual(std::string(), on.settings.engineOffForced, L"engine on: nothing forced");
			const Result off = RunScenario(Scenario::NothingAbsent, false, true);
			Assert::IsTrue(off.dejudderReason == DejudderPolicy::Reason::Off, L"engine off: de-judder off");
			Assert::IsTrue(Contains(off.settings.engineOffForced, "de-judder off: motion engine off"), W(off.settings.engineOffForced).c_str());
			Assert::IsTrue(Contains(off.settings.engineOffForced, "hiccup smoothing off: motion engine off"), W(off.settings.engineOffForced).c_str());
			const Result plain = RunScenario(Scenario::NothingAbsent, false, false);
			Assert::AreEqual(std::string(), plain.settings.engineOffForced, L"nothing was on: nothing forced, no Q3 line");
			Assert::IsTrue(Contains(VectorDemand::OffLine(plain.decision), "motion engine off (setting): "), L"the OFF line still names the wants");
		}

		// Engine ON, nothing absent, the de-judder off: every want is served with no starter; while pairs are expected the matcher costs
		// nothing (no render-time estimate). The CPU rig has no device: the producer is wanted but cannot start (Degraded, no entry).
		TEST_METHOD(EngineOnNothingAbsentServesEveryWant)
		{
			const Result r = RunScenario(Scenario::NothingAbsent, true, false);
			Assert::AreEqual(AlwaysWantedBits(), r.decision.served, L"every want served");
			Assert::AreEqual(0u, r.decision.unserved, L"nothing unserved");
			Assert::IsTrue(r.decision.mode == VectorDemand::Mode::Forward && r.decision.producer, L"forward, the producer wanted");
			Assert::IsFalse(r.decision.estimate || r.decision.matcherFallback, L"pairs expected: no matcher estimate");
			AssertNoMatcherWork(r);
			Assert::IsTrue(r.state.state == S::Degraded && r.state.reasons == 0u, L"rig: the producer cannot start (no device): first frames");
			Assert::IsFalse(r.state.Has(R::NoStarter), L"NoStarter is never derived");
			Assert::IsFalse(Entry(r, "motion engine").engaged, L"no hard absence: released");
			Assert::IsFalse(Entry(r, "vector_engine").engaged, L"every want served: released");
			Assert::AreEqual(std::string("Vector engine: on (forward) for output dither, grain map, deband hold, detail pulse, entrance segment, "
				"source monitor, intent, texture restore, residual stabilise, temporal detail"), VectorDemand::StatusLine(r.decision));
		}

		TEST_METHOD(EngineOnNothingAbsentWithTheDejudderRunsItsEstimate)
		{
			const Result r = RunScenario(Scenario::NothingAbsent, true, true);
			Assert::IsTrue(r.dejudderReason == DejudderPolicy::Reason::Active, L"the de-judder acts");
			Assert::AreEqual(AlwaysWantedBits(), r.decision.served, L"every want served");
			Assert::IsTrue(r.decision.producer && !r.decision.matcherFallback, L"the producer wanted, no fallback");
			Assert::AreEqual(1ull, static_cast<unsigned long long>(r.matcher.creates), L"one matcher, kept (no churn)");
			Assert::IsTrue(r.matcher.estimates >= 590, L"the de-judder estimates (nearly) every frame");
			Assert::AreEqual(0ull, static_cast<unsigned long long>(r.matcher.analysisEstimates), L"the de-judder's own estimate, not an analysis one");
			Assert::IsFalse(Entry(r, "vector_engine").engaged, L"every want served: released");
		}

		// Engine ON, optical flow off, the de-judder off: the named matcher fallback serves the field consumers (Degraded, deliberate:
		// no motion engine entry); the statistics consumers have no matcher form: named in Ctrl+I, not an entry (deliberate, review M4).
		TEST_METHOD(EngineOnOpticalFlowOffUsesTheNamedMatcherFallback)
		{
			const Result r = RunScenario(Scenario::OpticalFlowOff, true, false);
			Assert::IsTrue(r.state.state == S::Degraded && r.state.reason == R::OpticalFlowOff, L"Degraded, optical flow off");
			Assert::IsTrue(r.decision.matcherFallback && r.decision.estimate, L"the matcher fallback");
			Assert::IsTrue(r.decision.pairsDeliberatelyOff, L"a setting");
			Assert::AreEqual(AlwaysWantedBits() & ~StatisticsBits(), r.decision.served, L"the field consumers served by the matcher");
			Assert::AreEqual(StatisticsBits(), r.decision.noPairs, L"the statistics consumers: no pairs");
			Assert::AreEqual(StatisticsBits(), r.decision.unserved);
			Assert::AreEqual(std::string("Vector engine: on (forward; matcher fallback, no optical-flow pairs) for output dither, grain map, "
				"deband hold, detail pulse, entrance segment, texture restore, residual stabilise, temporal detail; source monitor, intent "
				"without vectors (no optical-flow pairs (optical flow off or not running))"), VectorDemand::StatusLine(r.decision),
				L"the fallback and the unserved consumers are named (Ctrl+I)");
			Assert::AreEqual(0ull, static_cast<unsigned long long>(r.work.producerStarts), L"no producer with optical flow off");
			Assert::AreEqual(1ull, static_cast<unsigned long long>(r.matcher.creates), L"one matcher, kept");
			Assert::IsTrue(r.matcher.analysisEstimates >= 590, L"the fallback estimates (nearly) every frame");
			Assert::AreEqual(r.matcher.estimates, r.matcher.analysisEstimates, L"every estimate is the fallback's (the de-judder is off)");
			Assert::IsFalse(Entry(r, "motion engine").engaged, L"optical flow off is deliberate: released");
			Assert::IsFalse(Entry(r, "vector_engine").engaged, L"deliberate no-pairs: not an entry (M4)");
		}

		// Engine ON, NVIDIA optical flow unavailable, the de-judder off: the start fails (a fault): the matcher fallback runs, the motion
		// engine entry names the block searches, and the statistics consumers without pairs are a vector_engine entry.
		TEST_METHOD(EngineOnNvofUnavailableUsesTheNamedMatcherFallback)
		{
			const Result r = RunScenario(Scenario::NvofUnavailable, true, false);
			Assert::IsTrue(r.state.state == S::Degraded && r.state.reason == R::NvofUnavailable, L"Degraded, NVOF unavailable");
			Assert::IsTrue(r.decision.matcherFallback && r.decision.estimate, L"the matcher fallback");
			Assert::IsFalse(r.decision.pairsDeliberatelyOff, L"a fault, not a setting");
			Assert::AreEqual(StatisticsBits(), r.decision.noPairs);
			Assert::AreEqual(0ull, static_cast<unsigned long long>(r.work.producerStarts), L"the start failed");
			Assert::AreEqual(1ull, static_cast<unsigned long long>(r.matcher.creates), L"one matcher, kept");
			Assert::IsTrue(r.matcher.analysisEstimates >= 590, L"the fallback estimates from the frame after the failed start");
			const FallbackCatalog::Decision& e = Entry(r, "motion engine");
			Assert::IsTrue(e.engaged, L"the motion engine entry is engaged");
			Assert::IsTrue(Contains(e.reason, "motion engine degraded: NVIDIA optical flow is not available"), W(e.reason).c_str());
			Assert::IsTrue(Contains(e.reason, "use the block searches without optical-flow pairs"), W(e.reason).c_str());
			const FallbackCatalog::Decision& v = Entry(r, "vector_engine");
			Assert::IsTrue(v.engaged, L"the statistics consumers without pairs: visible");
			Assert::AreEqual(std::string("source monitor, intent without vectors (no optical-flow pairs (optical flow off or not running))"), v.reason);
		}

		// (1) with the de-judder on: the matcher runs alone (Degraded, deliberate: the entry stays released), the producer never starts.
		TEST_METHOD(DejudderOnOpticalFlowOffIsDegradedWithoutTheProducer)
		{
			const Result r = RunScenario(Scenario::OpticalFlowOff, true, true);
			Assert::IsTrue(r.dejudderReason == DejudderPolicy::Reason::Active, L"the de-judder acts (23.976 on 60 Hz)");
			Assert::IsTrue(r.state.state == S::Degraded, L"Degraded");
			Assert::IsTrue(r.state.reason == R::OpticalFlowOff, L"leading reason OpticalFlowOff");
			Assert::AreEqual(0ull, static_cast<unsigned long long>(r.work.producerStarts), L"producerStarts");
			Assert::AreEqual(0ull, static_cast<unsigned long long>(r.work.producerPairs), L"producerPairs");
			Assert::AreEqual(1ull, static_cast<unsigned long long>(r.matcher.creates), L"one matcher, kept (no churn)");
			Assert::IsTrue(r.matcher.estimates >= 590, L"the matcher estimates (nearly) every frame");
			Assert::IsFalse(Entry(r, "motion engine").engaged, L"optical flow off is deliberate: the entry is released");
			// Batch B: was `decision.refused == 0` (the refused field is gone): the field consumers are served by the named matcher
			// fallback, the statistics consumers named without pairs, and no vector_engine entry (deliberate, M4).
			Assert::IsTrue(r.decision.matcherFallback, L"the matcher fallback serves the field consumers");
			Assert::AreEqual(AlwaysWantedBits() & ~StatisticsBits(), r.decision.served, L"every field consumer served");
			Assert::AreEqual(StatisticsBits(), r.decision.noPairs, L"the statistics consumers named without pairs");
			Assert::IsFalse(Entry(r, "vector_engine").engaged, L"deliberate no-pairs: not an entry");
		}

		// (2) with the de-judder on: the producer's start fails (the seam), the matcher runs; the entry names the block searches.
		// One failed start: the retry back-off is 5 s of real time (GetTickCount64) and the 600 frames run in well under that.
		TEST_METHOD(DejudderOnNvofUnavailableIsDegradedAndVisible)
		{
			const Result r = RunScenario(Scenario::NvofUnavailable, true, true);
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
			// Batch B: the failed start is a fault, so the statistics consumers without pairs are a visible entry too.
			Assert::IsTrue(Entry(r, "vector_engine").engaged, L"statistics without pairs (a fault): an entry");
			Assert::IsTrue(r.decision.matcherFallback, L"the matcher fallback serves the field consumers");
		}

		// (3) and (4) with the de-judder on: it cannot act there (SmoothMotion / LowLatency reasons), so nothing starts the engine.
		TEST_METHOD(DejudderOnSmoothMotionStillRunsNothing)
		{
			const Result r = RunScenario(Scenario::SmoothMotion, true, true);
			Assert::IsTrue(r.dejudderReason == DejudderPolicy::Reason::SmoothMotion, L"the de-judder yields to smooth motion");
			Assert::AreEqual(0ull, static_cast<unsigned long long>(r.matcher.creates + r.matcher.estimates + r.matcher.warmUps +
				r.matcher.analysisEstimates), L"no matcher work");
			Assert::AreEqual(0ull, static_cast<unsigned long long>(r.work.producerStarts + r.work.producerPairs), L"no producer work");
			Assert::IsTrue(r.state.state == S::Off, L"Off");
		}
		TEST_METHOD(DejudderOnCompetitiveLowLatencyStillRunsNothing)
		{
			const Result r = RunScenario(Scenario::CompetitiveLowLatency, true, true);
			Assert::IsTrue(r.dejudderReason == DejudderPolicy::Reason::LowLatency, L"the de-judder is off in low latency");
			Assert::AreEqual(0ull, static_cast<unsigned long long>(r.matcher.creates + r.matcher.estimates + r.matcher.warmUps +
				r.matcher.analysisEstimates), L"no matcher work");
			Assert::AreEqual(0ull, static_cast<unsigned long long>(r.work.producerStarts + r.work.producerPairs), L"no producer work");
			Assert::IsTrue(r.state.state == S::Off, L"Off");
			Assert::IsTrue(r.state.Has(R::LowLatency), L"the low-latency reason is shown");
		}
	};
}
