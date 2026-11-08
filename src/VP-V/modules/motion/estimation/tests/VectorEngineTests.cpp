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

		VectorDemand::Input OneConsumer(VectorDemand::Consumer c)
		{
			VectorDemand::Input in;
			in.enabled = true;
			in.slotsRun = true;
			in.queueAheadCapacity = 4;
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
			Assert::AreEqual<size_t>(4, host.fallbackIds.size());
			Assert::AreEqual(std::string("vector_engine"), host.fallbackIds[0]);
			Assert::AreEqual(std::string("vector_engine_producer"), host.fallbackIds[1]);
			Assert::AreEqual(std::string("vector_engine_global"), host.fallbackIds[2]);
			Assert::AreEqual(std::string("vector producer"), host.fallbackIds[3]);
			engine.UpdateVectorEngine(in, facts, host);
			Assert::AreEqual(1, host.publishes, L"an unchanged line is not republished");
			Assert::AreEqual<size_t>(8, host.fallbackIds.size());
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
			Assert::AreEqual<size_t>(4, host.fallbackIds.size());
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

		TEST_METHOD(TheAtomicEngagedFlagKeepsTheOldValues)
		{
			VectorEngine engine;
			Assert::IsFalse(engine.motionAheadEngaged);
			engine.motionAheadEngaged = true;
			Assert::IsTrue(engine.motionAheadEngaged);
			engine.motionAheadEngaged = false;
			Assert::IsFalse(engine.motionAheadEngaged);
		}
	};
}
