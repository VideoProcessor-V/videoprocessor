// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// SK-MV IVectorEngine::FlowOf over the P2 slot. CPU: FlowOf(N) equals the P2 slot contents for N (in source px, block 4 engine px); a resync,
// a pruned pair, a missing producer / size / pair each give false and are counted by reason; a view taken before a Prune stays readable
// (the engine's lease holds the shared pair); an unbound engine answers false (what a host without the engine sees).
#include "pch.h"
#include "CppUnitTest.h"
#include <VP-V/modules/motion/estimation/MotionAheadProducer.h>
#include <VP-V/modules/motion/estimation/OpticalFlowTrust.h>
#include <VP-V/modules/motion/estimation/VectorEngine.h>

#include <cstring>
#include <memory>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

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

		// W1-C3: the producer's extras ride on the view as the producer worked them out (not recomputed), and the pair's cell trust is
		// computed ONCE per pair id however often FlowOf serves it (the lease memo), equal to OpticalFlowTrust::CellTrust on the same inputs.
		TEST_METHOD(TheProducerExtrasRideOnTheViewAndTheCellTrustIsComputedOncePerPair)
		{
			Rig rig;
			auto full = std::make_shared<MotionAhead::Flow>(*rig.flow);
			full->backward = full->forward;
			for (size_t i = 0; i < full->backward.size(); ++i)
				full->backward[i] = static_cast<int16_t>(-full->backward[i] + (i % 3 == 0 ? 40 : 0));   // some cells inconsistent
			full->forwardCost.clear();
			for (int i = 0; i < 8; ++i) full->forwardCost.push_back(static_cast<uint8_t>(5 + 9 * i));
			full->backwardCost.assign(8, 9);
			MotionAhead::FlowGlobals(*full, true);
			MotionAhead::FlowExtras(*full);
			Assert::IsTrue(full->globalSet);
			Assert::AreEqual(full->gridW * full->gridH, static_cast<int>(full->fbError.size()));
			Assert::AreEqual(full->blocksX * full->blocksY, static_cast<int>(full->costWeight.size()));
			rig.engine.p2Slot->Publish(11, 12, rig.engine.p2Slot->Generation(), full);
			rig.engine.motionValidity.NoteKept(12);
			const int calls = 6;
			const float* first = nullptr;
			for (int n = 0; n < calls; ++n)
			{
				VpMotion::FlowView view;
				Assert::IsTrue(rig.engine.FlowOf(12, view));
				Assert::IsTrue(view.haveGlobal && view.globalGeneral);
				for (int d = 0; d < 2; ++d)
					for (int k = 0; k < 4; ++k)
					{
						Assert::AreEqual(full->global[d][k], view.global[d][k], 0.0f, L"the producer's vote, bit for bit");
						Assert::AreEqual(full->second[d][k], view.second[d][k], 0.0f);
					}
				Assert::IsTrue(view.costWeight == full->costWeight.data(), L"the producer's plane, not a copy");
				Assert::IsTrue(view.fbError == full->fbError.data());
				Assert::AreEqual(full->blocksX, view.blocksX);
				Assert::AreEqual(full->blocksY, view.blocksY);
				Assert::AreEqual(MotionAhead::Flow::kBlockPx, view.extrasBlockPx);
				Assert::AreEqual(NvOpticalFlow::Engine::Grid, view.extrasCellPx);
				Assert::AreEqual(32.0, view.extrasUnitsPerEnginePx, 0.0);
				const float* t = view.CellTrust(1.5f);
				Assert::IsNotNull(t);
				if (!first) first = t;
				Assert::IsTrue(t == first, L"the memo, not a new computation");
			}
			auto reads = rig.engine.FlowReads();
			Assert::AreEqual(static_cast<unsigned long long>(calls), reads.served);
			Assert::AreEqual(1ull, reads.conversions, L"one conversion for N FlowOf calls of the pair");
			Assert::AreEqual(1ull, reads.trustComputes, L"one cell-trust computation for N FlowOf calls of the pair");
			// Equal to the ONE engine trust on the same inputs: the producer's planes, and the same maths from the raw fields.
			std::vector<float> ref, cost, err, fromRaw;
			Assert::IsTrue(OpticalFlowTrust::CellTrust(full->costWeight, full->blocksX, full->blocksY, full->fbError, full->gridW, full->gridH,
				NvOpticalFlow::Engine::Grid, MotionAhead::Flow::kBlockPx, 1.5f, ref));
			Assert::IsTrue(OpticalFlowTrust::CostWeights(full->forwardCost, full->gridW, full->gridH, NvOpticalFlow::Engine::Grid,
				MotionAhead::Flow::kBlockPx, full->blocksX, full->blocksY, cost));
			Assert::IsTrue(OpticalFlowTrust::FbError(full->forward, full->backward, full->gridW, full->gridH, NvOpticalFlow::Engine::Grid, err));
			Assert::IsTrue(OpticalFlowTrust::CellTrust(cost, full->blocksX, full->blocksY, err, full->gridW, full->gridH,
				NvOpticalFlow::Engine::Grid, MotionAhead::Flow::kBlockPx, 1.5f, fromRaw));
			Assert::AreEqual(ref.size(), fromRaw.size());
			bool someBelowOne = false;
			for (size_t i = 0; i < ref.size(); ++i)
			{
				Assert::AreEqual(ref[i], first[i], 0.0f, L"bit-equal to OpticalFlowTrust::CellTrust");
				Assert::AreEqual(fromRaw[i], first[i], 0.0f);
				someBelowOne = someBelowOne || first[i] < 1.0f;
			}
			Assert::IsTrue(someBelowOne, L"the inputs exercise the weighting (not all trust 1)");
			// Another tolerance is a new computation (counted); the pair is not converted again. The FIRST pointer stays valid and
			// unchanged (one stable plane per tolerance), and asking the first tolerance again is still the memo.
			const std::vector<float> firstCopy(first, first + ref.size());
			VpMotion::FlowView again;
			Assert::IsTrue(rig.engine.FlowOf(12, again));
			const float* third = again.CellTrust(3.0f);
			Assert::IsNotNull(third);
			Assert::IsTrue(third != first);
			for (size_t i = 0; i < ref.size(); ++i)
				Assert::AreEqual(firstCopy[i], first[i], 0.0f, L"the first tolerance's plane is untouched by the second");
			std::vector<float> ref3;
			Assert::IsTrue(OpticalFlowTrust::CellTrust(full->costWeight, full->blocksX, full->blocksY, full->fbError, full->gridW, full->gridH,
				NvOpticalFlow::Engine::Grid, MotionAhead::Flow::kBlockPx, 3.0f, ref3));
			for (size_t i = 0; i < ref3.size(); ++i)
				Assert::AreEqual(ref3[i], third[i], 0.0f);
			Assert::IsTrue(again.CellTrust(1.5f) == first, L"back to the first tolerance: the memo");
			Assert::IsTrue(again.CellTrust(3.0f) == third);
			reads = rig.engine.FlowReads();
			Assert::AreEqual(2ull, reads.trustComputes);
			Assert::AreEqual(1ull, reads.conversions);
			// A pair without extras (no costs, no backward, no vote): nothing on the view, the trust cannot be formed (null), counted once.
			VpMotion::FlowView plain;
			Assert::IsTrue(rig.engine.FlowOf(11, plain));
			Assert::IsFalse(plain.haveGlobal);
			Assert::IsNull(plain.costWeight);
			Assert::IsNull(plain.fbError);
			Assert::IsNull(plain.CellTrust(1.5f));
			Assert::IsNull(plain.CellTrust(1.5f));
			reads = rig.engine.FlowReads();
			Assert::AreEqual(2ull, reads.conversions);
			Assert::AreEqual(3ull, reads.trustComputes, L"a failed formation is memoised too");
			// A default view (no FlowOf) has no memo.
			VpMotion::FlowView none;
			Assert::IsNull(none.CellTrust(1.5f));
		}

		TEST_METHOD(AfterAResyncTheAnswerIsFalseAndCounted)
		{
			Rig rig;
			VpMotion::FlowView view;
			Assert::IsTrue(rig.engine.FlowOf(11, view));
			rig.engine.motionValidity.Invalidate(::MotionValidity::Cause::Resync);
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
