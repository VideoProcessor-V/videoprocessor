// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// M2 P5a-4: the vector engine's decision/statistics logic moved out of the renderer (VectorEngine). Pure CPU: a fake
// host records what the engine asks of the renderer; no GPU, no producer. The renderer-side identity of these
// functions is covered by the on-screen chain gate (motion subset); here the rules are pinned: what is
// synchronised, when the status is republished, the global-pair hysteresis, the engine size and the shadow check.
// Built without the test project's precompiled header: keep windows.h from defining min/max.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "CppUnitTest.h"

#include <VP-V/modules/motion/estimation/VectorEngine.h>
#include <VP-V/modules/motion/estimation/VectorEngineInterface.h>
#include <VP-V/modules/motion/estimation/MotionRecordPublish.h>

#include <cstdio>
#include <string>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using namespace VpMotion;

namespace VectorEngineTest
{
	namespace
	{
		struct FakeHost : IVectorEngineHost
		{
			std::vector<std::string> fallbackIds;
			std::vector<bool> fallbackEngaged;
			int publishes = 0;
			bool ditherSupported = false;
			int monitorMotions = 0;
			bool intentActive = false;
			int intentVectors = 0;
			void SyncFallback(const FallbackCatalog::Decision& d) override
			{
				fallbackIds.push_back(d.id);
				fallbackEngaged.push_back(d.engaged);
			}
			void PublishStatus(bool) override { ++publishes; }
			bool DitherVectorsSupported() const override { return ditherSupported; }
			void NoteSourceMonitorMotion(double, uint64_t) override { ++monitorMotions; }
			bool IntentActive() const override { return intentActive; }
			void NoteIntentVectors(const IntentInterpreter::VectorSample&) override { ++intentVectors; }
		};

		// W1-C4: a host that records every source-statistics call, in order, with its exact arguments.
		struct RecordingHost : IVectorEngineHost
		{
			std::vector<std::string> calls;
			bool intentActive = false;
			void SyncFallback(const FallbackCatalog::Decision&) override {}
			void PublishStatus(bool) override {}
			bool DitherVectorsSupported() const override { return false; }
			void NoteSourceMonitorMotion(double sourcePixelsPerFrame, uint64_t sequence) override
			{
				char text[96];
				std::snprintf(text, sizeof(text), "monitor %.17g %llu", sourcePixelsPerFrame, static_cast<unsigned long long>(sequence));
				calls.push_back(text);
			}
			bool IntentActive() const override
			{
				const_cast<RecordingHost*>(this)->calls.push_back(intentActive ? "intent? yes" : "intent? no");
				return intentActive;
			}
			void NoteIntentVectors(const IntentInterpreter::VectorSample& s) override
			{
				char text[128];
				std::snprintf(text, sizeof(text), "intent %llu %llu %d %.17g", static_cast<unsigned long long>(s.from),
					static_cast<unsigned long long>(s.to), s.valid ? 1 : 0, s.heightsPerFrame);
				calls.push_back(text);
			}
		};

		// The two feeds BEFORE W1-C4, copied from VectorEngine.cpp ServeStatistics (estimate off) and
		// VectorEngineEstimate.inl RunDejudderEstimate (estimate on), written against the host.
		void BeforeServeStatistics(uint64_t from, uint64_t to, const float* global, int halvings, int pictureHeight,
			bool servesMonitor, bool servesIntent, IVectorEngineHost& host)
		{
			const VectorDemand::GlobalSample g = VectorDemand::SampleGlobal(DejudderPolicy::EngineGlobalSourcePixels(
				static_cast<double>(global[0]), static_cast<double>(global[1]), halvings), global[2], global[3], pictureHeight);
			if (g.usable && servesMonitor)
				host.NoteSourceMonitorMotion(g.sourcePixels, to);
			if (servesIntent && host.IntentActive())
			{
				IntentInterpreter::VectorSample vectors;
				vectors.from = from;
				vectors.to = to;
				vectors.valid = g.intentValid;
				vectors.heightsPerFrame = g.heightsPerFrame;
				host.NoteIntentVectors(vectors);
			}
		}

		void BeforeEstimate(uint64_t aSequence, uint64_t bSequence, const float* global, int searchHalvings, int pictureHeight,
			IVectorEngineHost& host)
		{
			if (global)
			{
				const VectorDemand::GlobalSample sample = VectorDemand::SampleGlobal(DejudderPolicy::EngineGlobalSourcePixels(
					static_cast<double>(global[0]), static_cast<double>(global[1]), searchHalvings), global[2], global[3], 0);
				if (sample.usable)
					host.NoteSourceMonitorMotion(sample.sourcePixels, bSequence);
			}
			if (global && host.IntentActive())
			{
				IntentInterpreter::VectorSample vectors;
				vectors.from = aSequence;
				vectors.to = bSequence;
				const VectorDemand::GlobalSample sample = VectorDemand::SampleGlobal(DejudderPolicy::EngineGlobalSourcePixels(
					static_cast<double>(global[0]), static_cast<double>(global[1]), searchHalvings), global[2], global[3], pictureHeight);
				vectors.valid = sample.intentValid;
				vectors.heightsPerFrame = sample.heightsPerFrame;
				host.NoteIntentVectors(vectors);
			}
		}

		// The record's motion group BEFORE W1-C4 (the MotionRecordPublish.h SummaryOf body).
		FrameAnalysis::MotionSummary BeforeSummary(bool pairValid, uint64_t from, uint64_t to, const float* global, int halvings)
		{
			if (!pairValid || !global || from == 0 || to <= from)
				return FrameAnalysis::MotionSummary();
			const VectorDemand::GlobalSample sample = VectorDemand::SampleGlobal(DejudderPolicy::EngineGlobalSourcePixels(
				static_cast<double>(global[0]), static_cast<double>(global[1]), halvings), global[2], global[3], 0);
			if (!sample.usable)
				return FrameAnalysis::MotionSummary();
			const double cell = DejudderPolicy::EngineCellSourcePixels(halvings);
			const double periods = static_cast<double>(to - from);
			FrameAnalysis::MotionSummary m;
			m.vectorsValid = true;
			m.pairFrom = from;
			m.globalX = cell * static_cast<double>(global[0]) / periods;
			m.globalY = cell * static_cast<double>(global[1]) / periods;
			m.source = FrameAnalysis::VectorSource::EngineAhead;
			return m;
		}

		// The input grid both modes run over: ambiguity / validity on both sides of the thresholds, signed globals,
		// halvings, picture heights (0 = none), adjacent and spanning pairs.
		template<class Fn>
		void ForEachPairInput(Fn fn)
		{
			const float zs[] = { 0.0f, 0.2f, 0.89f, 0.9f, 1.5f };
			const float ws[] = { 0.0f, 0.5f, 0.51f, 1.0f };
			const float gs[][2] = { { 0.0f, 0.0f }, { 3.5f, -2.0f }, { -40.0f, 12.25f } };
			const int halvingsSet[] = { 0, 1, 2 };
			const int heights[] = { 0, 800, 1080 };
			const uint64_t pairs[][2] = { { 10, 11 }, { 10, 12 } };
			for (float z : zs)
				for (float w : ws)
					for (const auto& g : gs)
						for (int h : halvingsSet)
							for (int ph : heights)
								for (const auto& p : pairs)
								{
									const float global[4] = { g[0], g[1], z, w };
									fn(p[0], p[1], global, h, ph);
								}
		}

		VectorDemand::Input OneConsumer(VectorDemand::Consumer c)
		{
			VectorDemand::Input in;
			in.enabled = true;
			in.slotsRun = true;
			in.queueAheadCapacity = 4;
			in.starterEngaged = true;   // W2-C1: a starter runs the engine (reuse-only consumers are served then)
			in.wants[static_cast<int>(c)] = true;
			return in;
		}
	}

	TEST_CLASS(VectorEngineTests)
	{
	public:
		TEST_METHOD(DemandOffTouchesNothingAndNeverPublishes)
		{
			VectorEngine engine;
			FakeHost host;
			VectorEngine::UpdateFacts facts;   // vector_demand setting off
			engine.UpdateVectorEngine(OneConsumer(VectorDemand::Consumer::GrainMap), facts, host);
			Assert::AreEqual(0, host.publishes);
			Assert::AreEqual<size_t>(0, host.fallbackIds.size());
			Assert::IsTrue(engine.vectorDemand.mode != VectorDemand::Mode::Off, L"the decision is still made");
		}

		TEST_METHOD(TheCameraFitWantStartsTheEngineLikeAnyFieldConsumer)
		{
			// SK-W stage D: nobody in the renderer wants vectors, but the camera link declared the fit: the engine counts CameraFit as the consumer.
			VectorEngine engine;
			FakeHost host;
			VectorEngine::UpdateFacts facts;
			facts.vectorDemandSetting = true;
			VectorDemand::Input none;
			none.enabled = true;
			none.slotsRun = true;
			none.queueAheadCapacity = 4;
			engine.UpdateVectorEngine(none, facts, host);
			Assert::IsTrue(engine.vectorDemand.mode == VectorDemand::Mode::Off, L"no consumer: idle");
			engine.SetCameraFitWanted(true);
			Assert::IsTrue(engine.CameraFitWanted());
			engine.UpdateVectorEngine(none, facts, host);
			Assert::IsTrue(engine.vectorDemand.mode == VectorDemand::Mode::Forward, L"the camera fit starts the engine");
			Assert::IsTrue(engine.vectorDemand.Serves(VectorDemand::Consumer::CameraFit));
			Assert::IsTrue(engine.vectorDemand.estimate && engine.vectorDemand.producer, L"a source-field consumer: the producer and the render-time estimate");
			engine.SetCameraFitWanted(false);
			engine.UpdateVectorEngine(none, facts, host);
			Assert::IsTrue(engine.vectorDemand.mode == VectorDemand::Mode::Off, L"withdrawn: idle again");
		}


		TEST_METHOD(DemandOnSyncsTheThreeEntriesEveryFrameAndPublishesOnlyWhenTheLineChanges)
		{
			VectorEngine engine;
			FakeHost host;
			VectorEngine::UpdateFacts facts;
			facts.vectorDemandSetting = true;
			const VectorDemand::Input in = OneConsumer(VectorDemand::Consumer::GrainMap);
			engine.UpdateVectorEngine(in, facts, host);
			Assert::AreEqual(1, host.publishes, L"first line is new");
			// T1 (plan 58 B3): the fourth entry, "vector producer" (the look-ahead's restarts), joins the three.
			// W2-C1: the fifth, "motion engine" (the one engine state), joins them.
			Assert::AreEqual<size_t>(5, host.fallbackIds.size());
			Assert::AreEqual(std::string("vector_engine"), host.fallbackIds[0]);
			Assert::AreEqual(std::string("vector_engine_producer"), host.fallbackIds[1]);
			Assert::AreEqual(std::string("vector_engine_global"), host.fallbackIds[2]);
			Assert::AreEqual(std::string("vector producer"), host.fallbackIds[3]);
			Assert::AreEqual(std::string("motion engine"), host.fallbackIds[4]);
			engine.UpdateVectorEngine(in, facts, host);
			Assert::AreEqual(1, host.publishes, L"an unchanged line is not republished");
			Assert::AreEqual<size_t>(10, host.fallbackIds.size());
		}

		// W2-C1: the engine state is derived in the same UpdateVectorEngine call that decides (no lag for the decision part).
		TEST_METHOD(UpdateDerivesTheEngineStateAndCountsRefusedStarts)
		{
			VectorEngine engine;
			FakeHost host;
			VectorEngine::UpdateFacts facts;
			facts.vectorDemandSetting = true;
			VectorDemand::Input in = OneConsumer(VectorDemand::Consumer::DetailPulse);
			in.starterEngaged = false;
			engine.UpdateVectorEngine(in, facts, host);
			VectorDemand::MotionEngineStatus st = engine.State();
			Assert::IsTrue(st.state == VectorDemand::MotionEngineState::Off && st.reason == VectorDemand::MotionEngineReason::NoStarter, L"no starter: off");
			Assert::AreEqual(1u << static_cast<int>(VectorDemand::Consumer::DetailPulse), st.affected, L"the refused consumer is affected");
			Assert::AreEqual(1ull, static_cast<unsigned long long>(st.frame));
			Assert::AreEqual(1ull, static_cast<unsigned long long>(engine.Work().refusedStarts[static_cast<int>(VectorDemand::Consumer::DetailPulse)]));
			Assert::IsTrue(host.fallbackEngaged[4], L"the motion engine entry is engaged");
			in.starterEngaged = true;
			engine.UpdateVectorEngine(in, facts, host);
			st = engine.State();
			Assert::IsTrue(st.reason != VectorDemand::MotionEngineReason::NoStarter, L"a starter: not NoStarter");
			Assert::AreEqual(2ull, static_cast<unsigned long long>(st.frame));
			Assert::AreEqual(1ull, static_cast<unsigned long long>(engine.Work().refusedStarts[static_cast<int>(VectorDemand::Consumer::DetailPulse)]), L"not refused again");
			Assert::AreEqual(0ull, static_cast<unsigned long long>(engine.Work().producerPairs), L"no producer: no pairs");
		}

		TEST_METHOD(SwitchingDemandOffReleasesTheEntriesOnce)
		{
			VectorEngine engine;
			FakeHost host;
			VectorEngine::UpdateFacts facts;
			facts.vectorDemandSetting = true;
			engine.UpdateVectorEngine(OneConsumer(VectorDemand::Consumer::GrainMap), facts, host);
			host.fallbackIds.clear();
			facts.vectorDemandSetting = false;
			engine.UpdateVectorEngine(OneConsumer(VectorDemand::Consumer::GrainMap), facts, host);
			Assert::AreEqual<size_t>(5, host.fallbackIds.size());
			for (bool engaged : host.fallbackEngaged)
				Assert::IsFalse(engaged, L"released");
			host.fallbackIds.clear();
			engine.UpdateVectorEngine(OneConsumer(VectorDemand::Consumer::GrainMap), facts, host);
			Assert::AreEqual<size_t>(0, host.fallbackIds.size(), L"released once");
		}

		// WAIVER-T1-01: the fourth Release is the reason the counts above went 3 -> 4. An engaged
		// "vector producer" entry must not outlive vector_demand: the real UpdateVectorEngine path releases it.
		TEST_METHOD(DemandOffReleasesAnEngagedVectorProducerEntry)
		{
			VectorEngine engine;
			FakeHost host;
			VectorEngine::UpdateFacts facts;
			facts.vectorDemandSetting = true;
			engine.UpdateVectorEngine(OneConsumer(VectorDemand::Consumer::GrainMap), facts, host);   // a line is logged
			host.SyncFallback(FallbackCatalog::VectorProducer(true, true, 2, "no device"));   // the entry engages
			Assert::IsTrue(host.fallbackEngaged.back(), L"engaged");
			facts.vectorDemandSetting = false;
			engine.UpdateVectorEngine(OneConsumer(VectorDemand::Consumer::GrainMap), facts, host);
			int last = -1;
			for (size_t i = 0; i < host.fallbackIds.size(); ++i)
				if (host.fallbackIds[i] == "vector producer")
					last = static_cast<int>(i);
			Assert::IsTrue(last >= 0);
			Assert::IsFalse(host.fallbackEngaged[static_cast<size_t>(last)], L"released with vector_demand off");
		}

		TEST_METHOD(GlobalPairMissIsSustainedOnlyAfter24InARowAndNeedsAServedConsumer)
		{
			VectorEngine engine;
			engine.vectorDemand = VectorDemand::Decide(OneConsumer(VectorDemand::Consumer::SourceMonitorPan));
			for (int i = 0; i < 23; ++i)
				engine.NoteGlobalPair(false);
			Assert::IsFalse(engine.vectorGlobalMissSustained);
			engine.NoteGlobalPair(false);
			Assert::IsTrue(engine.vectorGlobalMissSustained);
			for (int i = 0; i < 23; ++i)
				engine.NoteGlobalPair(true);
			Assert::IsTrue(engine.vectorGlobalMissSustained, L"23 hits do not release it");
			engine.NoteGlobalPair(true);
			Assert::IsFalse(engine.vectorGlobalMissSustained);
			engine.vectorDemand = VectorDemand::Decide(OneConsumer(VectorDemand::Consumer::GrainMap));
			for (int i = 0; i < 30; ++i)
				engine.NoteGlobalPair(false);
			Assert::IsFalse(engine.vectorGlobalMissSustained, L"no statistics consumer served: counters stay clear");
			Assert::AreEqual(0u, engine.vectorGlobalMisses);
		}

		TEST_METHOD(EngineSizeHalvesTheSearchInput)
		{
			int w = 0, h = 0, halvings = 0;
			VectorEngine::SizeFor(true, 3840, 2160, w, h, halvings);
			Assert::IsTrue(halvings >= 0 && halvings <= 4);
			// (width+1)/2, then (x+1)/2 per halving: derived, not hard-coded
			int ew = (3840 + 1) / 2, eh = (2160 + 1) / 2;
			for (int i = 0; i < halvings; ++i) { ew = (ew + 1) / 2; eh = (eh + 1) / 2; }
			Assert::AreEqual(ew, w);
			Assert::AreEqual(eh, h);
			Assert::AreEqual(1, halvings, L"2160 lines with low-res on: one halving");
			Assert::AreEqual(960, w);
			Assert::AreEqual(540, h);
			int w2 = 0, h2 = 0, halvings2 = 0;
			VectorEngine::SizeFor(false, 3840, 2160, w2, h2, halvings2);
			Assert::IsTrue(halvings2 <= halvings, L"low-res off never halves more");
		}

		TEST_METHOD(ServeStatisticsWithoutARunningProducerDoesNothing)
		{
			VectorEngine engine;
			FakeHost host;
			engine.vectorDemand = VectorDemand::Decide(OneConsumer(VectorDemand::Consumer::SourceMonitorPan));
			VectorEngine::ServeFacts serve;
			engine.ServeStatistics(10, 11, 1920, 1080, serve, host);
			Assert::AreEqual(0, host.monitorMotions);
			Assert::AreEqual(0, host.intentVectors);
			Assert::AreEqual<unsigned long long>(0, engine.motionAheadMisses, L"no producer: not even a miss is counted");
			Assert::AreEqual<uint64_t>(0, engine.vectorBackwardSequence, L"the sequence is not consumed");
		}

		TEST_METHOD(ShadowCheckCountsOnlyWhileTheDeliverySlotRuns)
		{
			VectorEngine engine;
			engine.MotionShadowCheck(false, 5, 6, true);
			Assert::AreEqual<unsigned long long>(0, engine.motionShadowChecks);
			engine.MotionShadowCheck(true, 5, 6, false);   // nothing published: the segment says no, the host says no
			Assert::AreEqual<unsigned long long>(1, engine.motionShadowChecks);
			Assert::AreEqual<unsigned long long>(0, engine.motionShadowMismatches);
			engine.MotionShadowCheck(true, 5, 6, true);    // the host says live, the segment has nothing: a mismatch
			Assert::AreEqual<unsigned long long>(1, engine.motionShadowMismatches);
		}

		// The reservations (doc 72 item 7): both are absent placeholders until the camera + motion lead implements them.
		TEST_METHOD(TheReservedRefinementAndLongPairEntryPointsAnswerAbsent)
		{
			struct Absent : IVectorEngine
			{
				uint64_t LatestVerifiedFieldSeq() const override { return 0; }
				uint64_t FieldEra() const override { return 0; }
				bool FieldOf(uint64_t, VerifiedField&) const override { return false; }
				float FieldScale() const override { return 1.0f; }
				bool GlobalFromEngine() const override { return false; }
				bool EngineGlobal(uint64_t, GlobalVec&) const override { return false; }
				bool PairSpansSlip(uint64_t, uint64_t) const override { return false; }
				bool FlowIsCpuResident() const override { return false; }
				bool FlowOf(uint64_t, FlowView&) const override { return false; }
				void SetPreEstimateHook(IPreEstimateHook*) override {}
			} engine;
			VerifiedField field;
			Assert::IsFalse(engine.RefinedFieldAvailable());
			Assert::IsFalse(engine.RefinedFieldOf(7, field));
			uint64_t ticket = 99;
			HintField hint;
			Assert::IsTrue(engine.SubmitLongPair(nullptr, nullptr, &hint, 3, 9, ticket) == LongPairResult::Unavailable);
			Assert::AreEqual<uint64_t>(0, ticket, L"no ticket is issued for an absent service");
			Assert::IsFalse(engine.PollLongPair(0, field));
			engine.CancelLongPair(0);   // a no-op, never a fault
			Assert::IsTrue(std::string(kLongPairFallbackText).find("matcher") != std::string::npos,
				L"the fallback wording names the matcher");
			Assert::AreEqual(std::string("refined_field"), std::string(kRefinedFieldProduct));
		}

		// W2-C1 MUST 1: the first refresh seeds the epoch; it opens no resync window.
		TEST_METHOD(TheFirstRefreshIsNoResync)
		{
			VectorEngine engine;
			FakeHost host;
			VectorEngine::UpdateFacts facts;
			facts.vectorDemandSetting = true;
			engine.UpdateVectorEngine(OneConsumer(VectorDemand::Consumer::GrainMap), facts, host);
			Assert::IsFalse(engine.State().Has(VectorDemand::MotionEngineReason::Resync), L"first refresh: no resync");
			engine.UpdateVectorEngine(OneConsumer(VectorDemand::Consumer::GrainMap), facts, host);
			Assert::IsFalse(engine.State().Has(VectorDemand::MotionEngineReason::Resync), L"same epoch: no resync");
		}

		// W2-C1 MUST 5: an invalidation (a new epoch) opens the resync window (1 s); the engine is degraded meanwhile.
		TEST_METHOD(AnInvalidationOpensTheResyncWindow)
		{
			VectorEngine engine;
			FakeHost host;
			VectorEngine::UpdateFacts facts;
			facts.vectorDemandSetting = true;
			const VectorDemand::Input in = OneConsumer(VectorDemand::Consumer::GrainMap);
			engine.UpdateVectorEngine(in, facts, host);
			engine.motionValidity.Invalidate(MotionValidity::Cause::Resync);
			engine.UpdateVectorEngine(in, facts, host);
			const VectorDemand::MotionEngineStatus st = engine.State();
			Assert::IsTrue(st.Has(VectorDemand::MotionEngineReason::Resync), L"the new epoch: resync");
			Assert::IsTrue(st.state == VectorDemand::MotionEngineState::Degraded, L"degraded, not off");
			Assert::IsTrue(engine.motionStatusEpochAtMs != 0, L"the window start is recorded");
			engine.motionStatusEpochAtMs = GetTickCount64() - 1001;   // the window's clock: 1 s later (no sleep)
			engine.UpdateVectorEngine(in, facts, host);
			Assert::IsFalse(engine.State().Has(VectorDemand::MotionEngineReason::Resync), L"the window closed after 1 s");
		}

		// W2-C1 MUST 5: the VRAM release is a hard off, fed each decision (NoteVramRelease).
		TEST_METHOD(TheVramReleaseTurnsTheEngineOff)
		{
			VectorEngine engine;
			FakeHost host;
			VectorEngine::UpdateFacts facts;
			facts.vectorDemandSetting = true;
			const VectorDemand::Input in = OneConsumer(VectorDemand::Consumer::GrainMap);
			engine.NoteVramRelease(true);
			engine.UpdateVectorEngine(in, facts, host);
			VectorDemand::MotionEngineStatus st = engine.State();
			Assert::IsTrue(st.state == VectorDemand::MotionEngineState::Off && st.Has(VectorDemand::MotionEngineReason::VramRelease));
			Assert::IsTrue(host.fallbackEngaged.back(), L"the motion engine entry names the release");
			engine.NoteVramRelease(false);
			engine.UpdateVectorEngine(in, facts, host);
			st = engine.State();
			Assert::IsFalse(st.Has(VectorDemand::MotionEngineReason::VramRelease), L"released latch cleared the next decision");
		}

		// W2-C1 MUST 5: the camera fit reads only the optical-flow seed: with optical flow off it does not start the engine.
		TEST_METHOD(TheCameraFitDoesNotStartTheEngineWithOpticalFlowOff)
		{
			VectorEngine engine;
			FakeHost host;
			VectorEngine::UpdateFacts facts;
			facts.vectorDemandSetting = true;
			facts.opticalFlow = false;
			VectorDemand::Input none;
			none.enabled = true;
			none.slotsRun = true;
			none.queueAheadCapacity = 4;
			engine.SetCameraFitWanted(true);
			engine.UpdateVectorEngine(none, facts, host);
			Assert::IsTrue(engine.vectorDemand.mode == VectorDemand::Mode::Off, L"gated: idle");
			Assert::IsFalse(engine.vectorDemand.Serves(VectorDemand::Consumer::CameraFit));
			Assert::IsTrue((engine.State().affected & (1u << static_cast<int>(VectorDemand::Consumer::CameraFit))) != 0, L"the camera fit is named affected");
			facts.opticalFlow = true;
			engine.UpdateVectorEngine(none, facts, host);
			Assert::IsTrue(engine.vectorDemand.Serves(VectorDemand::Consumer::CameraFit), L"optical flow on: the camera fit starts it");
			Assert::IsFalse(engine.motionCameraGated, L"no longer gated (still affected while the producer has not started: degraded)");
		}

		TEST_METHOD(TheAtomicEngagedFlagKeepsTheOldValues)
		{
			VectorEngine engine;
			Assert::IsFalse(engine.motionAheadEngaged);
			engine.motionAheadEngaged = true;
			Assert::IsTrue(engine.motionAheadEngaged);
			engine.motionAheadEngaged = false;
			Assert::IsFalse(engine.motionAheadEngaged);
		}

		// W1-C4: estimate OFF (the ServeStatistics feed): the one sample + Feed with the demand's Serves gates makes the
		// byte-identical SetMotion / intent call sequence of the old feed, for every gate and intent combination.
		TEST_METHOD(OneFeedMatchesTheStatisticsFeedCallSequence)
		{
			size_t cases = 0, calls = 0;
			ForEachPairInput([&](uint64_t from, uint64_t to, const float* global, int halvings, int pictureHeight)
			{
				for (int gates = 0; gates < 4; ++gates)
					for (int active = 0; active < 2; ++active)
					{
						const bool servesMonitor = (gates & 1) != 0, servesIntent = (gates & 2) != 0;
						RecordingHost before, after;
						before.intentActive = after.intentActive = active != 0;
						BeforeServeStatistics(from, to, global, halvings, pictureHeight, servesMonitor, servesIntent, before);
						MotionRecord::Feed(MotionRecord::SampleOf(from, to, global, halvings, pictureHeight), servesMonitor,
							servesIntent, after);
						Assert::IsTrue(before.calls == after.calls, L"estimate off: the call sequence is unchanged");
						++cases;
						calls += after.calls.size();
					}
			});
			Assert::IsTrue(cases > 0 && calls > cases, L"the grid made calls (monitor and intent both seen)");
		}

		// W1-C4: estimate ON (the render-time estimate feed): the one sample + Feed with gates true / true makes the
		// byte-identical call sequence of the old two-sample ungated feed, including no pair (no call at all).
		TEST_METHOD(OneFeedMatchesTheEstimateFeedCallSequence)
		{
			size_t monitorCalls = 0, intentCalls = 0;
			ForEachPairInput([&](uint64_t from, uint64_t to, const float* global, int halvings, int pictureHeight)
			{
				for (int active = 0; active < 2; ++active)
				{
					RecordingHost before, after;
					before.intentActive = after.intentActive = active != 0;
					BeforeEstimate(from, to, global, halvings, pictureHeight, before);
					MotionRecord::Feed(MotionRecord::SampleOf(from, to, global, halvings, pictureHeight), true, true, after);
					Assert::IsTrue(before.calls == after.calls, L"estimate on: the call sequence is unchanged");
					for (const std::string& c : after.calls)
					{
						if (c.compare(0, 8, "monitor ") == 0) ++monitorCalls;
						if (c.compare(0, 7, "intent ") == 0) ++intentCalls;
					}
				}
			});
			Assert::IsTrue(monitorCalls > 0 && intentCalls > 0, L"both consumers were fed on the grid");
			for (int active = 0; active < 2; ++active)
			{
				RecordingHost before, after;
				before.intentActive = after.intentActive = active != 0;
				BeforeEstimate(10, 11, nullptr, 1, 1080, before);
				MotionRecord::Feed(MotionRecord::SampleOf(10, 11, nullptr, 1, 1080), true, true, after);
				Assert::IsTrue(before.calls == after.calls && after.calls.empty(), L"no engine pair: no call, as before");
			}
		}

		// W1-C4: the record's motion group from the pair's one sample equals the old SummaryOf, field for field.
		TEST_METHOD(RecordFromTheOneSampleMatchesTheOldSummary)
		{
			ForEachPairInput([&](uint64_t from, uint64_t to, const float* global, int halvings, int pictureHeight)
			{
				for (int valid = 0; valid < 2; ++valid)
				{
					const FrameAnalysis::MotionSummary a = BeforeSummary(valid != 0, from, to, global, halvings);
					const FrameAnalysis::MotionSummary b = MotionRecord::SummaryOf(valid != 0,
						MotionRecord::SampleOf(from, to, global, halvings, pictureHeight));
					const FrameAnalysis::MotionSummary c = MotionRecord::SummaryOf(valid != 0, from, to, global, halvings);
					Assert::IsTrue(a.vectorsValid == b.vectorsValid && a.pairFrom == b.pairFrom && a.globalX == b.globalX &&
						a.globalY == b.globalY && a.source == b.source && a.magnitudeP50 == b.magnitudeP50 &&
						a.magnitudeP95 == b.magnitudeP95, L"the sample's record equals the old record");
					Assert::IsTrue(c.vectorsValid == a.vectorsValid && c.globalX == a.globalX && c.globalY == a.globalY &&
						c.pairFrom == a.pairFrom && c.source == a.source, L"the old entry point is unchanged");
				}
			});
			const FrameAnalysis::MotionSummary none = MotionRecord::SummaryOf(true, MotionRecord::SampleOf(10, 11, nullptr, 1, 0));
			Assert::IsFalse(none.vectorsValid, L"no engine pair: absent");
		}

		// W1-C4 audit MUST 1: the render-time estimate samples the engine global ONCE per pair (the record and the feed read that
		// one sample); the engine's CPU record equals the old two-sample record, and with the demand not deciding (vector_demand
		// off) the feed's call sequence equals the old ungated estimate feed.
		TEST_METHOD(TheEstimatePathSamplesEachPairOnce)
		{
			VectorDemand::Decision off;   // vector_demand off: Reason::Disabled, served 0
			Assert::IsTrue(off.reason == VectorDemand::Reason::Disabled, L"premise: the default decision is the demand off");
			size_t pairs = 0;
			ForEachPairInput([&](uint64_t from, uint64_t to, const float* global, int halvings, int pictureHeight)
			{
				for (int active = 0; active < 2; ++active)
				{
					VectorEngine oldEngine, newEngine;
					RecordingHost before, after;
					before.intentActive = after.intentActive = active != 0;
					oldEngine.NoteEngineGlobal(from, to, global, halvings);
					BeforeEstimate(from, to, global, halvings, pictureHeight, before);
					const uint64_t samplesBefore = MotionRecord::SamplesTaken().load();
					MotionRecord::EstimatePair(newEngine, off, from, to, global, halvings, pictureHeight, after);
					Assert::AreEqual<uint64_t>(1, MotionRecord::SamplesTaken().load() - samplesBefore, L"one sample per pair");
					Assert::IsTrue(before.calls == after.calls, L"demand off: the old ungated estimate feed, call for call");
					GlobalVec a, b;
					const bool oldServed = oldEngine.EngineGlobal(to, a), newServed = newEngine.EngineGlobal(to, b);
					Assert::IsTrue(oldServed == newServed && a.valid == b.valid && a.x == b.x && a.y == b.y,
						L"the engine's CPU record equals the old record");
					++pairs;
				}
			});
			Assert::IsTrue(pairs > 0, L"the grid ran");
			VectorEngine engine;
			RecordingHost host;
			host.intentActive = true;
			const uint64_t samplesBefore = MotionRecord::SamplesTaken().load();
			MotionRecord::EstimatePair(engine, off, 10, 11, nullptr, 1, 1080, host);
			Assert::AreEqual<uint64_t>(0, MotionRecord::SamplesTaken().load() - samplesBefore, L"no engine pair: nothing sampled");
			Assert::IsTrue(host.calls.empty(), L"no engine pair: no call, as before");
			GlobalVec none;
			Assert::IsFalse(engine.EngineGlobal(11, none), L"no engine pair: the record says so");
		}

		// W1-C4 audit MUST 2 (WAIVER-C4-01): while the demand decides, the render-time estimate's feed is gated by its Serves,
		// exactly as the statistics feed: for each served combination the sequence equals the old statistics feed with those gates;
		// serving both (the default: source_resolution auto, intent active) equals the old ungated estimate feed call for call.
		TEST_METHOD(TheEstimateFeedFollowsTheDemandWhileItDecides)
		{
			using C = VectorDemand::Consumer;
			size_t differing = 0;
			ForEachPairInput([&](uint64_t from, uint64_t to, const float* global, int halvings, int pictureHeight)
			{
				for (int gates = 0; gates < 4; ++gates)
					for (int active = 0; active < 2; ++active)
					{
						const bool servesMonitor = (gates & 1) != 0, servesIntent = (gates & 2) != 0;
						VectorDemand::Decision on;
						on.reason = VectorDemand::Reason::Forward;
						on.served = (servesMonitor ? 1u << static_cast<int>(C::SourceMonitorPan) : 0u) |
							(servesIntent ? 1u << static_cast<int>(C::MotionIntent) : 0u);
						VectorEngine engine;
						RecordingHost gated, statistics, ungated;
						gated.intentActive = statistics.intentActive = ungated.intentActive = active != 0;
						MotionRecord::EstimatePair(engine, on, from, to, global, halvings, pictureHeight, gated);
						BeforeServeStatistics(from, to, global, halvings, pictureHeight, servesMonitor, servesIntent, statistics);
						BeforeEstimate(from, to, global, halvings, pictureHeight, ungated);
						Assert::IsTrue(gated.calls == statistics.calls, L"demand on: the statistics feed's gates and sequence");
						if (servesMonitor && servesIntent)
							Assert::IsTrue(gated.calls == ungated.calls, L"both served: the old estimate feed, call for call");
						else if (gated.calls != ungated.calls)
							++differing;
					}
			});
			Assert::IsTrue(differing > 0, L"a consumer the demand does not serve is no longer fed on the estimate path");
			// The real decision with both consumers wanted (the default settings: vector_demand on, source_resolution auto) serves both.
			VectorDemand::Input in = OneConsumer(C::SourceMonitorPan);
			in.wants[static_cast<int>(C::MotionIntent)] = true;
			in.wants[static_cast<int>(C::DejudderExtras)] = true;
			const VectorDemand::Decision decided = VectorDemand::Decide(in);
			Assert::IsTrue(MotionRecord::EstimateGate(decided, C::SourceMonitorPan) && MotionRecord::EstimateGate(decided, C::MotionIntent),
				L"defaults: both gates open, the estimate feed is unchanged");
			VectorDemand::Input interlaced = in;
			interlaced.interlaced = true;
			const VectorDemand::Decision unserved = VectorDemand::Decide(interlaced);
			Assert::IsFalse(MotionRecord::EstimateGate(unserved, C::SourceMonitorPan) || MotionRecord::EstimateGate(unserved, C::MotionIntent),
				L"interlaced: the demand serves neither, the estimate path feeds neither");
			VectorDemand::Input disabled = in;
			disabled.enabled = false;
			const VectorDemand::Decision legacy = VectorDemand::Decide(disabled);
			Assert::IsTrue(MotionRecord::EstimateGate(legacy, C::SourceMonitorPan) && MotionRecord::EstimateGate(legacy, C::MotionIntent),
				L"vector_demand off: the legacy ungated feed");
		}
	};
}
