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

		TEST_METHOD(DemandOnSyncsTheThreeEntriesEveryFrameAndPublishesOnlyWhenTheLineChanges)
		{
			VectorEngine engine;
			FakeHost host;
			VectorEngine::UpdateFacts facts;
			facts.vectorDemandSetting = true;
			const VectorDemand::Input in = OneConsumer(VectorDemand::Consumer::GrainMap);
			engine.UpdateVectorEngine(in, facts, host);
			Assert::AreEqual(1, host.publishes, L"first line is new");
			Assert::AreEqual<size_t>(3, host.fallbackIds.size());
			Assert::AreEqual(std::string("vector_engine"), host.fallbackIds[0]);
			Assert::AreEqual(std::string("vector_engine_producer"), host.fallbackIds[1]);
			Assert::AreEqual(std::string("vector_engine_global"), host.fallbackIds[2]);
			engine.UpdateVectorEngine(in, facts, host);
			Assert::AreEqual(1, host.publishes, L"an unchanged line is not republished");
			Assert::AreEqual<size_t>(6, host.fallbackIds.size());
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
			Assert::AreEqual<size_t>(3, host.fallbackIds.size());
			for (bool engaged : host.fallbackEngaged)
				Assert::IsFalse(engaged, L"released");
			host.fallbackIds.clear();
			engine.UpdateVectorEngine(OneConsumer(VectorDemand::Consumer::GrainMap), facts, host);
			Assert::AreEqual<size_t>(0, host.fallbackIds.size(), L"released once");
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
