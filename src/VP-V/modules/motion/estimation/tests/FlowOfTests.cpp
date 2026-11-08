// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// SK-MV IVectorEngine::FlowOf over the P2 slot. CPU: FlowOf(N) equals the P2 slot contents for N (in source px, block 4 engine px); a resync,
// a pruned pair, a missing producer / size / pair each give false and are counted by reason; a view taken before a Prune stays readable
// (the engine's lease holds the shared pair); an unbound engine answers false (what a host without the engine sees).
#include "pch.h"
#include <VP-V/modules/motion/estimation/MotionAheadProducer.h>
#include <VP-V/modules/motion/estimation/VectorEngine.h>

#include <cstring>
#include <memory>

namespace VideoProcessorTest
{
	namespace
	{
		// A 16 x 8 engine-size luma: grid 4 x 2; forward cell i = (raw 64 + 32 * i, raw -32) = (2 + i, -1) engine px.
		MotionAhead::FlowPtr MakeFlow()
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
			return flow;
		}

		// An engine with a (never started) producer, the source width noted, the run 10, 11 kept, and the pair (10, 11) in the slot.
		struct Rig
		{
			VpMotion::VectorEngine engine;
			MotionAhead::FlowPtr flow = MakeFlow();
			Rig()
			{
				engine.PublishProducer(std::make_shared<MotionAhead::Producer>());
				engine.NoteFlowSourceWidth(64);   // 4 source px per engine px
				engine.motionValidity.NoteKept(10);
				engine.motionValidity.NoteKept(11);
				engine.p2Slot->Publish(10, 11, engine.p2Slot->Generation(), flow);
			}
		};
	}

	TEST_CLASS(FlowOfTests)
	{
	public:
		TEST_METHOD(FlowOfEqualsTheP2SlotContentsForThePair)
		{
			Rig rig;
			VpMotion::FlowView view;
			Assert::IsTrue(rig.engine.FlowOf(11, view));
			Assert::AreEqual(4, view.gridW);
			Assert::AreEqual(2, view.gridH);
			Assert::AreEqual(16.0, view.blockSize, 1e-9, L"a cell is 4 engine px = 16 source px");
			Assert::AreEqual(uint64_t(10), view.from);
			Assert::AreEqual(uint64_t(11), view.to);
			for (int i = 0; i < 8; ++i)
			{
				// raw / 32 = engine px; x 4 = source px.
				Assert::AreEqual(static_cast<double>(rig.flow->forward[i * 2]) / 32.0 * 4.0, static_cast<double>(view.x[i]), 1e-5);
				Assert::AreEqual(static_cast<double>(rig.flow->forward[i * 2 + 1]) / 32.0 * 4.0, static_cast<double>(view.y[i]), 1e-5);
			}
			Assert::AreEqual(8.0, static_cast<double>(view.x[0]), 1e-6, L"2 engine px = 8 source px");
			Assert::AreEqual(-4.0, static_cast<double>(view.y[0]), 1e-6);
			const auto reads = rig.engine.FlowReads();
			Assert::AreEqual(1ull, reads.requests);
			Assert::AreEqual(1ull, reads.served);
		}

		TEST_METHOD(TheViewCarriesTheNativeGridAndTheOriginalPayloadBitForBit)
		{
			Rig rig;
			// Costs and a backward field on the stored pair, so the view's optional planes are exercised.
			auto full = std::make_shared<MotionAhead::Flow>(*rig.flow);
			full->backward = full->forward;
			for (int16_t& v : full->backward) v = static_cast<int16_t>(-v);
			full->forwardCost.assign(8, 7);
			full->backwardCost.assign(8, 9);
			rig.engine.p2Slot->Publish(11, 12, rig.engine.p2Slot->Generation(), full);
			rig.engine.motionValidity.NoteKept(12);
			VpMotion::FlowView view;
			Assert::IsTrue(rig.engine.FlowOf(12, view));
			Assert::AreEqual(full->gridW, view.gridW, L"native NVOF grid, not resampled");
			Assert::AreEqual(full->gridH, view.gridH);
			Assert::AreEqual(full->halfW, view.engineW);
			Assert::AreEqual(full->halfH, view.engineH);
			Assert::AreEqual(4, view.cellEnginePx);
			Assert::AreEqual(32.0, view.rawUnitsPerEnginePx, 0.0);
			Assert::AreEqual(4.0, view.enginePxToSource, 1e-12);
			Assert::AreEqual(16.0, view.blockSize, 1e-12, L"cellEnginePx x enginePxToSource");
			Assert::IsTrue(view.rawForward == full->forward.data(), L"the original payload, not a copy");
			Assert::IsTrue(view.rawBackward == full->backward.data());
			Assert::IsTrue(view.forwardCost == full->forwardCost.data());
			Assert::IsTrue(view.backwardCost == full->backwardCost.data());
			const size_t cells = static_cast<size_t>(full->gridW) * full->gridH;
			Assert::AreEqual(0, std::memcmp(view.rawForward, full->forward.data(), cells * 2 * sizeof(int16_t)), L"bit for bit");
			Assert::AreEqual(0, std::memcmp(view.rawBackward, full->backward.data(), cells * 2 * sizeof(int16_t)));
			Assert::AreEqual(0, std::memcmp(view.forwardCost, full->forwardCost.data(), cells));
			// The float planes are exactly the raw values in source px.
			for (size_t i = 0; i < cells; ++i)
			{
				Assert::AreEqual(static_cast<float>(view.rawForward[i * 2] / view.rawUnitsPerEnginePx * view.enginePxToSource), view.x[i]);
				Assert::AreEqual(static_cast<float>(view.rawForward[i * 2 + 1] / view.rawUnitsPerEnginePx * view.enginePxToSource), view.y[i]);
			}
			// A pair without a backward field or costs: those planes are null, the forward payload is still the original.
			VpMotion::FlowView plain;
			Assert::IsTrue(rig.engine.FlowOf(11, plain));
			Assert::IsTrue(plain.rawForward == rig.flow->forward.data());
			Assert::IsNull(plain.rawBackward);
			Assert::IsNull(plain.forwardCost);
		}

		TEST_METHOD(AfterAResyncTheAnswerIsFalseAndCounted)
		{
			Rig rig;
			VpMotion::FlowView view;
			Assert::IsTrue(rig.engine.FlowOf(11, view));
			rig.engine.motionValidity.Invalidate(VpMotion::MotionValidity::Cause::Resync);
			VpMotion::FlowView after;
			after.gridW = 99;
			Assert::IsFalse(rig.engine.FlowOf(11, after));
			Assert::IsNull(after.x, L"the view is cleared, never left with the old pair");
			Assert::AreEqual(0, after.gridW);
			Assert::AreEqual(1ull, rig.engine.FlowReads().invalidated);
		}

		TEST_METHOD(APrunedPairGivesFalseAndAViewTakenBeforeThePruneStaysReadable)
		{
			Rig rig;
			VpMotion::FlowView view;
			Assert::IsTrue(rig.engine.FlowOf(11, view));
			rig.engine.P2().Prune(12);   // the pair (10, 11) ends before 12: gone from the slot
			rig.flow.reset();            // and the test no longer holds it either: only the engine's lease does
			Assert::AreEqual(8.0, static_cast<double>(view.x[0]), 1e-6, L"the lease keeps the pair alive under the reader");
			VpMotion::FlowView after;
			Assert::IsFalse(rig.engine.FlowOf(11, after));
			Assert::IsNull(after.x);
			Assert::AreEqual(1ull, rig.engine.FlowReads().noPair);
		}

		TEST_METHOD(NoProducerNoSizeNoPairAndLowLatencyEachAnswerFalseByReason)
		{
			VpMotion::FlowView view;
			VpMotion::VectorEngine unbound;   // a host without the engine's producer / slot content
			Assert::IsFalse(unbound.FlowOf(11, view));
			Assert::IsNull(view.x);
			Assert::AreEqual(1ull, unbound.FlowReads().noProducer);

			Rig rig;
			rig.engine.motionLowLatencyNoVectors = true;
			Assert::IsFalse(rig.engine.FlowOf(11, view));
			Assert::AreEqual(1ull, rig.engine.FlowReads().noProducer);
			rig.engine.motionLowLatencyNoVectors = false;

			Assert::IsFalse(rig.engine.FlowOf(1, view), L"no earlier frame");
			Assert::IsFalse(rig.engine.FlowOf(12, view), L"the pair (11, 12) is not valid / not computed");
			Assert::AreEqual(2ull, rig.engine.FlowReads().noPair + rig.engine.FlowReads().invalidated);

			VpMotion::VectorEngine noSize;
			noSize.PublishProducer(std::make_shared<MotionAhead::Producer>());
			noSize.motionValidity.NoteKept(10);
			noSize.motionValidity.NoteKept(11);
			noSize.p2Slot->Publish(10, 11, noSize.p2Slot->Generation(), MakeFlow());
			Assert::IsFalse(noSize.FlowOf(11, view));
			Assert::AreEqual(1ull, noSize.FlowReads().unscaled);
			noSize.NoteFlowSourceWidth(64);
			Assert::IsTrue(noSize.FlowOf(11, view));
		}
	};
}
