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

		// L-B6 migration identity (WAIVER-B6-CELLTRUST condition): the camera's retired private recompute (AheadCellTrust: CostWeights on the
		// forward costs, FbError of the two directions, CellTrust at the matcher's tolerance, units rescaled to S10.5) is reproduced HERE as
		// the reference (test code, not product code) from the raw arrays the view serves, and the ENGINE memo FlowView::CellTrust must equal
		// it bit for bit, for pairs with and without the producer's extras and for several tolerances. A difference would be a behaviour
		// change of the camera trust (a registered fix), not a refactor.
		TEST_METHOD(TheEngineMemoTrustEqualsTheRetiredPrivateRecompute)
		{
			const int w = 320, h = 180, gw = (w + 3) / 4, gh = (h + 3) / 4;
			uint32_t r = 2468u;
			auto rnd = [&r]() { r = r * 1664525u + 1013904223u; return r >> 8; };
			for (int withExtras = 0; withExtras < 2; ++withExtras)
			{
				Rig rig;
				auto flow = std::make_shared<MotionAhead::Flow>();
				flow->halfW = w;
				flow->halfH = h;
				flow->gridW = gw;
				flow->gridH = gh;
				const size_t cells = static_cast<size_t>(gw) * gh;
				for (size_t i = 0; i < cells; ++i)
				{
					const int fx = static_cast<int>(rnd() % 200) - 100 + 150, fy = static_cast<int>(rnd() % 200) - 100 - 60;
					flow->forward.push_back(static_cast<int16_t>(fx));
					flow->forward.push_back(static_cast<int16_t>(fy));
					flow->backward.push_back(static_cast<int16_t>(-fx + static_cast<int>(rnd() % 64) - 32));
					flow->backward.push_back(static_cast<int16_t>(-fy + static_cast<int>(rnd() % 64) - 32));
					flow->forwardCost.push_back(static_cast<uint8_t>(rnd() % 60));
					flow->backwardCost.push_back(static_cast<uint8_t>(rnd() % 60));
				}
				if (withExtras)
				{
					MotionAhead::FlowGlobals(*flow, true);
					MotionAhead::FlowExtras(*flow);
				}
				rig.engine.p2Slot->Publish(11, 12, rig.engine.p2Slot->Generation(), flow);
				rig.engine.motionValidity.NoteKept(12);
				VpMotion::FlowView view;
				Assert::IsTrue(rig.engine.FlowOf(12, view));
				const int grid = NvOpticalFlow::Engine::Grid, block = MotionAhead::Flow::kBlockPx;
				const int blocksX = (w + block - 1) / block, blocksY = (h + block - 1) / block;
				const float tolerances[] = { 0.5f, 1.5f, 2.0f, 3.0f };
				for (float tol : tolerances)
				{
					// The retired recompute, from the view's own raw arrays (S10.5, so no unit rescale is needed here).
					std::vector<float> blockCost, cellError, reference;
					Assert::IsTrue(OpticalFlowTrust::CostWeights(std::vector<uint8_t>(view.forwardCost, view.forwardCost + cells), gw, gh, grid, block,
						blocksX, blocksY, blockCost));
					Assert::IsTrue(OpticalFlowTrust::FbError(std::vector<int16_t>(view.rawForward, view.rawForward + cells * 2),
						std::vector<int16_t>(view.rawBackward, view.rawBackward + cells * 2), gw, gh, grid, cellError));
					Assert::IsTrue(OpticalFlowTrust::CellTrust(blockCost, blocksX, blocksY, cellError, gw, gh, grid, block, tol, reference));
					const float* memo = view.CellTrust(tol);
					Assert::IsNotNull(memo);
					bool someBelowOne = false;
					for (size_t i = 0; i < cells; ++i)
					{
						Assert::AreEqual(reference[i], memo[i], 0.0f, L"the engine memo equals the retired private recompute, bit for bit");
						someBelowOne = someBelowOne || memo[i] < 1.0f;
					}
					Assert::IsTrue(someBelowOne, L"the inputs exercise the weighting");
				}
			}
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

		TEST_METHOD(OnePruneKeepsThreePairsBehindTheRenderFrame)   // GEM VE-P2-LIFE
		{
			Rig rig;
			VpMotion::FlowView view;
			rig.engine.PruneBehindRender(11);   // rendering 11: its own pair (10, 11) stays
			Assert::IsTrue(rig.engine.FlowOf(11, view));
			rig.engine.PruneBehindRender(13);   // 13 renders: pairs ending at 11, 12, 13 are kept (3 behind)
			Assert::IsTrue(rig.engine.FlowOf(11, view));
			rig.engine.PruneBehindRender(14);   // the pair ending at 11 is now 4 behind
			VpMotion::FlowView after;
			Assert::IsFalse(rig.engine.FlowOf(11, after));
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

		TEST_METHOD(PerAxisScaleHandlesNonUniformCropAspectAndRebuildsLeaseOnHeightChange)
		{
			auto absDiff = [](double a, double b) { return a > b ? a - b : b - a; };
			auto makeFlow = [](int halfW, int halfH, int16_t rawX, int16_t rawY) {
				auto flow = std::make_shared<MotionAhead::Flow>();
				flow->halfW = halfW;
				flow->halfH = halfH;
				flow->gridW = (halfW + 3) / 4;
				flow->gridH = (halfH + 3) / 4;
				const size_t cells = static_cast<size_t>(flow->gridW) * static_cast<size_t>(flow->gridH);
				flow->forward.reserve(cells * 2);
				for (size_t i = 0; i < cells; ++i)
				{
					flow->forward.push_back(rawX);
					flow->forward.push_back(rawY);
				}
				return flow;
			};

			// 1920x802 crop halved twice: 1920 -> 960 -> 480, 802 -> 401 -> 201 (genuine X != Y anisotropy).
			const int sourceW = 1920, sourceH = 802;
			const int engineW = 480, engineH = 201;
			const int16_t rawX = 320;    // 10 engine px -> 40.0 source px
			const int16_t rawY = 1600;   // 50 engine px -> 199.5025 source px (vs 200.0 width-only)
			const auto cropFlow = makeFlow(engineW, engineH, rawX, rawY);
			const size_t lastCell = static_cast<size_t>(cropFlow->gridW) * static_cast<size_t>(cropFlow->gridH) - 1;

			VpMotion::VectorEngine engine;
			engine.PublishProducer(std::make_shared<MotionAhead::Producer>());
			engine.NoteFlowSourceSize(sourceW, sourceH, engineW, engineH);
			engine.motionValidity.NoteKept(10);
			engine.motionValidity.NoteKept(11);
			Assert::IsTrue(engine.p2Slot->Publish(10, 11, engine.p2Slot->Generation(), cropFlow));

			const double scaleX = static_cast<double>(sourceW) / engineW;
			const double scaleY = static_cast<double>(sourceH) / engineH;
			const double expectedX = static_cast<double>(rawX) / 32.0 * scaleX;
			const double expectedY = static_cast<double>(rawY) / 32.0 * scaleY;

			VpMotion::FlowView view;
			Assert::IsTrue(engine.FlowOf(11, view));
			Assert::AreEqual(scaleX, view.enginePxToSource, 1e-12);
			Assert::AreEqual(scaleY, view.enginePxToSourceY, 1e-12);
			Assert::AreEqual(4.0 * scaleX, view.blockSize, 1e-12);
			Assert::AreEqual(4.0 * scaleY, view.blockSizeY, 1e-12);
			Assert::AreEqual(static_cast<float>(expectedX), view.x[0], 0.0f);
			Assert::AreEqual(static_cast<float>(expectedY), view.y[0], 0.0f);
			Assert::AreEqual(static_cast<float>(expectedX), view.x[lastCell], 0.0f);
			Assert::AreEqual(static_cast<float>(expectedY), view.y[lastCell], 0.0f);
			Assert::AreEqual(1ull, engine.FlowReads().conversions);
			Assert::AreEqual(1ull, engine.Leases().live);

			// Same-input negative control: naive width-only scaling on the exact same vertical vector deviates by ~0.5 px (> 0.25 px).
			const double naiveWidthOnlyY = static_cast<double>(rawY) / 32.0 * scaleX;
			Assert::IsTrue(absDiff(naiveWidthOnlyY, static_cast<double>(view.y[0])) > 0.25,
				L"anisotropy hazard must be real: width-only scaling deviates > 0.25 px from per-axis view.y");
			Assert::IsTrue(absDiff(static_cast<double>(view.y[0]), expectedY) < 1e-4,
				L"per-axis FlowOf matches true vertical displacement");

			// Same-input unprotected engine path (width-only NoteFlowSourceWidth) confirms the legacy path exhibits that exact error.
			VpMotion::VectorEngine widthOnlyEngine;
			widthOnlyEngine.PublishProducer(std::make_shared<MotionAhead::Producer>());
			widthOnlyEngine.NoteFlowSourceWidth(sourceW);
			widthOnlyEngine.motionValidity.NoteKept(10);
			widthOnlyEngine.motionValidity.NoteKept(11);
			Assert::IsTrue(widthOnlyEngine.p2Slot->Publish(10, 11, widthOnlyEngine.p2Slot->Generation(), cropFlow));
			VpMotion::FlowView widthOnlyView;
			Assert::IsTrue(widthOnlyEngine.FlowOf(11, widthOnlyView));
			Assert::AreEqual(scaleX, widthOnlyView.enginePxToSourceY, 1e-12);
			Assert::IsTrue(absDiff(static_cast<double>(widthOnlyView.y[0]), expectedY) > 0.25,
				L"unprotected width-only path violates vertical displacement accuracy on the exact same payload");

			// Height-only change with same engine size (802 -> 804, engineH still 201): LeaseKey must include scaleY and rebuild the lease.
			engine.NoteFlowSourceSize(1920, 804, 480, 201);
			const double scaleY804 = 804.0 / 201.0;
			const double expectedY804 = static_cast<double>(rawY) / 32.0 * scaleY804;
			VpMotion::FlowView heightChanged;
			Assert::IsTrue(engine.FlowOf(11, heightChanged));
			Assert::AreEqual(scaleX, heightChanged.enginePxToSource, 1e-12);
			Assert::AreEqual(scaleY804, heightChanged.enginePxToSourceY, 1e-12);
			Assert::AreEqual(4.0 * scaleY804, heightChanged.blockSizeY, 1e-12);
			Assert::AreEqual(static_cast<float>(expectedY804), heightChanged.y[0], 0.0f);
			Assert::IsTrue(absDiff(static_cast<double>(heightChanged.y[0]), expectedY) > 0.25,
				L"height-only change must update view.y instead of returning stale 802px lease data");
			Assert::AreEqual(2ull, engine.FlowReads().conversions, L"scaleY change must trigger a new lease build");
			Assert::IsTrue(engine.Leases().live <= 1ull, L"single-lease invariant holds without a queue board");
			Assert::AreEqual(1ull, engine.Leases().live);
			Assert::AreEqual(1ull, engine.Leases().droppedSingle);

			// Changing to 1920x1080 (target engine 480x270) refuses the stale 480x201 pair as unscaled, then serves a matching 480x270 pair.
			const auto beforeMismatch = engine.FlowReads();
			engine.NoteFlowSourceSize(1920, 1080, 480, 270);
			VpMotion::FlowView mismatched;
			Assert::IsFalse(engine.FlowOf(11, mismatched), L"stale 480x201 pair must be refused once target engineH is 270");
			Assert::IsNull(mismatched.x);
			Assert::AreEqual(beforeMismatch.unscaled + 1ull, engine.FlowReads().unscaled);
			Assert::IsTrue(engine.Leases().live <= 1ull);
			Assert::AreEqual(0ull, engine.Leases().live);

			const auto fullHeightFlow = makeFlow(480, 270, rawX, rawY);
			engine.motionValidity.NoteKept(12);
			Assert::IsTrue(engine.p2Slot->Publish(11, 12, engine.p2Slot->Generation(), fullHeightFlow));
			VpMotion::FlowView view1080;
			Assert::IsTrue(engine.FlowOf(12, view1080));
			Assert::AreEqual(4.0, view1080.enginePxToSource, 1e-12);
			Assert::AreEqual(4.0, view1080.enginePxToSourceY, 1e-12);
			Assert::AreEqual(16.0, view1080.blockSize, 1e-12);
			Assert::AreEqual(16.0, view1080.blockSizeY, 1e-12);
			Assert::AreEqual(static_cast<float>(rawY / 32.0 * 4.0), view1080.y[0], 0.0f);
			Assert::IsTrue(engine.Leases().live <= 1ull);
			Assert::AreEqual(1ull, engine.Leases().live);
		}

		TEST_METHOD(ResolutionSwitch1080pTo4KNeverServesTwoXStaleVectors)
		{
			auto makeFlow = [](int halfW, int halfH, int16_t rawX, int16_t rawY) {
				auto flow = std::make_shared<MotionAhead::Flow>();
				flow->halfW = halfW;
				flow->halfH = halfH;
				flow->gridW = (halfW + 3) / 4;
				flow->gridH = (halfH + 3) / 4;
				const size_t cells = static_cast<size_t>(flow->gridW) * static_cast<size_t>(flow->gridH);
				flow->forward.reserve(cells * 2);
				for (size_t i = 0; i < cells; ++i)
				{
					flow->forward.push_back(rawX);
					flow->forward.push_back(rawY);
				}
				return flow;
			};

			const int16_t rawX = 320;    // +10 engine px
			const int16_t rawY = -160;   // -5 engine px
			const auto flow1080p = makeFlow(960, 540, rawX, rawY);

			// 1) Start on 1080p (960x540): FlowOf serves cleanly at scaleX = 2.0, scaleY = 2.0.
			VpMotion::VectorEngine engine;
			engine.PublishProducer(std::make_shared<MotionAhead::Producer>());
			engine.NoteFlowSourceSize(1920, 1080, 960, 540);
			engine.motionValidity.NoteKept(10);
			engine.motionValidity.NoteKept(11);
			Assert::IsTrue(engine.p2Slot->Publish(10, 11, engine.p2Slot->Generation(), flow1080p));

			VpMotion::FlowView view1080;
			Assert::IsTrue(engine.FlowOf(11, view1080));
			Assert::AreEqual(2.0, view1080.enginePxToSource, 1e-12);
			Assert::AreEqual(2.0, view1080.enginePxToSourceY, 1e-12);
			Assert::AreEqual(8.0, view1080.blockSize, 1e-12);
			Assert::AreEqual(8.0, view1080.blockSizeY, 1e-12);
			Assert::AreEqual(20.0f, view1080.x[0], 1e-5f);
			Assert::AreEqual(-10.0f, view1080.y[0], 1e-5f);

			// Same-input negative control (hazard proof): an unprotected width-only engine switching to 3840 while the stale 960x540 pair
			// sits in p2Slot silently serves 4x-scaled (2x-corrupted) vectors instead of refusing the mismatch.
			VpMotion::VectorEngine unprotected;
			unprotected.PublishProducer(std::make_shared<MotionAhead::Producer>());
			unprotected.NoteFlowSourceWidth(3840);
			unprotected.motionValidity.NoteKept(10);
			unprotected.motionValidity.NoteKept(11);
			Assert::IsTrue(unprotected.p2Slot->Publish(10, 11, unprotected.p2Slot->Generation(), flow1080p));
			VpMotion::FlowView bogus4k;
			Assert::IsTrue(unprotected.FlowOf(11, bogus4k), L"unprotected width-only path accepts mismatched 960x540 pair");
			Assert::AreEqual(4.0, bogus4k.enginePxToSource, 1e-12, L"stale pair gets silently inflated by 2x (4.0 vs 2.0)");
			Assert::AreEqual(40.0f, bogus4k.x[0], 1e-5f);

			// 2) Protected switch to 4K full-res (1920x1080 engine) while the stale 960x540 pair is still in p2Slot: must refuse as unscaled.
			const auto beforeSwitch = engine.FlowReads();
			engine.NoteFlowSourceSize(3840, 2160, 1920, 1080);
			VpMotion::FlowView staleAfterSwitch;
			Assert::IsFalse(engine.FlowOf(11, staleAfterSwitch), L"stale 960x540 pair must never be served under 4K 1920x1080 target");
			Assert::IsNull(staleAfterSwitch.x);
			const auto afterSwitch = engine.FlowReads();
			Assert::AreEqual(beforeSwitch.unscaled + 1ull, afterSwitch.unscaled);
			Assert::AreEqual(beforeSwitch.noPair, afterSwitch.noPair);
			Assert::AreEqual(beforeSwitch.invalidated, afterSwitch.invalidated);

			// 3) Matching 4K full-res pair (1920x1080) succeeds cleanly at scaleX == 2.0, scaleY == 2.0.
			const auto flow4kFull = makeFlow(1920, 1080, rawX, rawY);
			engine.motionValidity.NoteKept(12);
			Assert::IsTrue(engine.p2Slot->Publish(11, 12, engine.p2Slot->Generation(), flow4kFull));
			VpMotion::FlowView view4kFull;
			Assert::IsTrue(engine.FlowOf(12, view4kFull));
			Assert::AreEqual(2.0, view4kFull.enginePxToSource, 1e-12);
			Assert::AreEqual(2.0, view4kFull.enginePxToSourceY, 1e-12);
			Assert::AreEqual(8.0, view4kFull.blockSize, 1e-12);
			Assert::AreEqual(8.0, view4kFull.blockSizeY, 1e-12);
			Assert::AreEqual(20.0f, view4kFull.x[0], 1e-5f);
			Assert::AreEqual(-10.0f, view4kFull.y[0], 1e-5f);

			// 4) lowRes=true same-engine-size switch: 1080p and 4K lowRes BOTH run at 960x540 engine size.
			int ew1080 = 0, eh1080 = 0, h1080 = 0;
			int ew4k = 0, eh4k = 0, h4k = 0;
			VpMotion::VectorEngine::SizeFor(true, 1920, 1080, ew1080, eh1080, h1080);
			VpMotion::VectorEngine::SizeFor(true, 3840, 2160, ew4k, eh4k, h4k);
			Assert::AreEqual(960, ew1080);
			Assert::AreEqual(540, eh1080);
			Assert::AreEqual(ew1080, ew4k, L"1080p and 4K lowRes share identical engine width");
			Assert::AreEqual(eh1080, eh4k, L"1080p and 4K lowRes share identical engine height");

			VpMotion::VectorEngine lowResEngine;
			lowResEngine.PublishProducer(std::make_shared<MotionAhead::Producer>());
			lowResEngine.NoteFlowSourceSize(1920, 1080, ew1080, eh1080);
			lowResEngine.motionValidity.NoteKept(10);
			lowResEngine.motionValidity.NoteKept(11);
			Assert::IsTrue(lowResEngine.p2Slot->Publish(10, 11, lowResEngine.p2Slot->Generation(), flow1080p));
			VpMotion::FlowView lowResBefore;
			Assert::IsTrue(lowResEngine.FlowOf(11, lowResBefore));
			Assert::AreEqual(2.0, lowResBefore.enginePxToSource, 1e-12);
			Assert::AreEqual(2.0, lowResBefore.enginePxToSourceY, 1e-12);

			// Trigger SourceFormatChange through TakeInvalidation: remembered 1080p size must be cleared.
			lowResEngine.motionValidity.Invalidate(::MotionValidity::Cause::SourceFormatChange);
			::MotionValidity::Pending pending;
			Assert::IsTrue(lowResEngine.TakeInvalidation(pending));
			Assert::IsTrue(pending.cause == ::MotionValidity::Cause::SourceFormatChange);

			// Publish a new-generation 4K lowRes pair (same 960x540 engine dims) BEFORE NoteFlowSourceSize is called for 4K.
			const auto flow4kLowRes = makeFlow(ew4k, eh4k, rawX, rawY);
			const uint64_t newGen = lowResEngine.motionValidity.StreamGeneration();
			lowResEngine.p2Slot->SetGeneration(newGen);
			Assert::IsTrue(lowResEngine.p2Slot->Publish(20, 21, newGen, flow4kLowRes));
			lowResEngine.motionValidity.NoteKept(20);
			lowResEngine.motionValidity.NoteKept(21);

			// Same-input negative control: without SourceFormatChange clearing flowSize_, the exact same 960x540 4K pair would be
			// served at the stale 1080p 2.0 scale (20 px instead of 40 px - 2x error).
			VpMotion::VectorEngine unresetEngine;
			unresetEngine.PublishProducer(std::make_shared<MotionAhead::Producer>());
			unresetEngine.NoteFlowSourceSize(1920, 1080, ew1080, eh1080);
			unresetEngine.motionValidity.NoteKept(20);
			unresetEngine.motionValidity.NoteKept(21);
			Assert::IsTrue(unresetEngine.p2Slot->Publish(20, 21, unresetEngine.p2Slot->Generation(), flow4kLowRes));
			VpMotion::FlowView staleLowResScale;
			Assert::IsTrue(unresetEngine.FlowOf(21, staleLowResScale));
			Assert::AreEqual(2.0, staleLowResScale.enginePxToSource, 1e-12);
			Assert::AreEqual(20.0f, staleLowResScale.x[0], 1e-5f, L"stale 1080p size halves true 4K lowRes displacement");

			// Protected engine refuses FlowOf as unscaled until NoteFlowSourceSize(3840, 2160, 960, 540) is called.
			const auto beforeFormatRead = lowResEngine.FlowReads();
			VpMotion::FlowView unscaledAfterFormat;
			Assert::IsFalse(lowResEngine.FlowOf(21, unscaledAfterFormat),
				L"SourceFormatChange must clear flowSize_ so same-engine-size 4K lowRes is refused until re-sized");
			Assert::IsNull(unscaledAfterFormat.x);
			const auto afterFormatRead = lowResEngine.FlowReads();
			Assert::AreEqual(beforeFormatRead.unscaled + 1ull, afterFormatRead.unscaled);
			Assert::AreEqual(beforeFormatRead.noPair, afterFormatRead.noPair);
			Assert::AreEqual(beforeFormatRead.invalidated, afterFormatRead.invalidated);

			// Once NoteFlowSourceSize is called for 4K lowRes, FlowOf recovers with true 4.0 scale (2x stale 1080p scale).
			lowResEngine.NoteFlowSourceSize(3840, 2160, ew4k, eh4k);
			VpMotion::FlowView recovered4kLowRes;
			Assert::IsTrue(lowResEngine.FlowOf(21, recovered4kLowRes));
			Assert::AreEqual(4.0, recovered4kLowRes.enginePxToSource, 1e-12);
			Assert::AreEqual(4.0, recovered4kLowRes.enginePxToSourceY, 1e-12);
			Assert::AreEqual(16.0, recovered4kLowRes.blockSize, 1e-12);
			Assert::AreEqual(16.0, recovered4kLowRes.blockSizeY, 1e-12);
			Assert::AreEqual(40.0f, recovered4kLowRes.x[0], 1e-5f);
			Assert::AreEqual(-20.0f, recovered4kLowRes.y[0], 1e-5f);
			Assert::AreEqual(2.0f * staleLowResScale.x[0], recovered4kLowRes.x[0], 1e-5f);
		}
	};
}
