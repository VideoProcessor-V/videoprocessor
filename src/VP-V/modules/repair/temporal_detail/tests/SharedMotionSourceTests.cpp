// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// SK-MV MV5: TemporalDetail::SharedMotionSource (the IMotionSource over the vector engine's P2 view). CPU only. The adapter answers with
// exactly what the shared view holds for the pair, answers ABSENT (and counts and shows it) for a missing, stale or unfit pair, and leaves
// TemporalDetail's own sources untouched.
#include "pch.h"
#include "CppUnitTest.h"

#include <VP-V/modules/repair/temporal_detail/SharedMotionSource.h>

#include <chrono>
#include <cstdio>
#include <memory>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace VideoProcessorTest
{
	TEST_CLASS(SharedMotionSourceTests)
	{
	public:
		static std::shared_ptr<MotionAhead::Flow> MakeFlow(int halfW, int halfH)
		{
			auto flow = std::make_shared<MotionAhead::Flow>();
			flow->halfW = halfW;
			flow->halfH = halfH;
			flow->gridW = (halfW + 3) / 4;
			flow->gridH = (halfH + 3) / 4;
			const size_t cells = static_cast<size_t>(flow->gridW) * flow->gridH;
			flow->forward.resize(cells * 2);
			flow->backward.resize(cells * 2);
			flow->forwardCost.resize(cells);
			flow->backwardCost.resize(cells);
			for (size_t i = 0; i < cells; ++i)
			{
				flow->forward[i * 2] = static_cast<int16_t>(64 + static_cast<int>(i % 7));       // 2.0 + k/32 producer pixels
				flow->forward[i * 2 + 1] = static_cast<int16_t>(-32);                              // -1.0
				flow->backward[i * 2] = static_cast<int16_t>(-64 - static_cast<int>(i % 5));
				flow->backward[i * 2 + 1] = static_cast<int16_t>(32);
				flow->forwardCost[i] = static_cast<uint8_t>(10 + i % 20);
				flow->backwardCost[i] = static_cast<uint8_t>(200);
			}
			return flow;
		}

		TEST_METHOD(ItReturnsTheSamePairAndVectorsTheSharedViewHoldsForThatPair)
		{
			auto slot = std::make_shared<VpMotion::P2Slot>(16);
			slot->SetGeneration(1);
			const auto flow = MakeFlow(96, 48);
			MotionAhead::FlowExtras(*flow);   // the producer's worker does this: the de-judder's cost and consistency weights
			Assert::IsTrue(slot->Publish(4, 5, 1, flow));
			MotionValidity::State validity;
			validity.NoteKept(4);
			validity.NoteKept(5);
			// The producer worked at half the source: the source is 192 wide, so a producer pixel is 2 source pixels.
			TemporalDetail::SharedMotionSource source(VpMotion::P2View(slot), &validity);
			source.SetSourceSize(192, 96);
			TemporalDetail::MotionPair fwd, bwd;
			Assert::IsTrue(source.Forward(5, fwd), L"Forward(seq) is the pair (seq - 1 -> seq)");
			Assert::IsTrue(source.Backward(5, bwd), L"Backward(seq) is the pair (seq -> seq - 1)");
			const MotionAhead::FlowPtr held = VpMotion::P2View(slot).Find(4, 5);   // what the shared view holds for the same pair
			Assert::IsTrue(held == flow);
			Assert::AreEqual(held->gridW, fwd.gridW);
			Assert::AreEqual(held->gridH, fwd.gridH);
			Assert::AreEqual(8.0, fwd.blockPx, 1e-9, L"an engine cell is 4 producer pixels = 8 source pixels");
			const size_t cells = static_cast<size_t>(held->gridW) * held->gridH;
			Assert::AreEqual(cells, fwd.vx.size());
			// Trust is the de-judder's engine-trust weighting (MV5 follow-up): the block cost weight Flow::costWeight times the engine's
			// forward/backward consistency weight from Flow::fbError (FallOff from the matcher's limit to twice it), per cell. Forward uses what the
			// producer worked out; backward the same maths on the backward fields.
			std::vector<float> backCost, backErr;
			const int perBlock = OpticalFlowTrust::CellsPerBlock(NvOpticalFlow::Engine::Grid, MotionAhead::Flow::kBlockPx);
			Assert::IsTrue(OpticalFlowTrust::CostWeights(held->backwardCost, held->gridW, held->gridH, NvOpticalFlow::Engine::Grid,
				MotionAhead::Flow::kBlockPx, held->blocksX, held->blocksY, backCost));
			Assert::IsTrue(OpticalFlowTrust::FbError(held->backward, held->forward, held->gridW, held->gridH, NvOpticalFlow::Engine::Grid, backErr));
			for (size_t i = 0; i < cells; ++i)
			{
				const int cx = static_cast<int>(i % static_cast<size_t>(held->gridW)), cy = static_cast<int>(i / static_cast<size_t>(held->gridW));
				const size_t block = static_cast<size_t>(cy / perBlock) * static_cast<size_t>(held->blocksX) + static_cast<size_t>(cx / perBlock);
				const double expectedFwd = held->costWeight[block] * OpticalFlowTrust::FallOff(source.ConsistencyPixels(), 2.0f * source.ConsistencyPixels(), held->fbError[i]);
				const double expectedBwd = backCost[block] * OpticalFlowTrust::FallOff(source.ConsistencyPixels(), 2.0f * source.ConsistencyPixels(), backErr[i]);
				Assert::AreEqual(expectedFwd, static_cast<double>(fwd.trust[i]), 1e-6, L"forward trust = cost weight x consistency weight");
				Assert::AreEqual(expectedBwd, static_cast<double>(bwd.trust[i]), 1e-6, L"backward trust = the same maths on the backward fields");
				Assert::IsTrue(fwd.trust[i] >= 0.0f && fwd.trust[i] <= 1.0f && bwd.trust[i] >= 0.0f && bwd.trust[i] <= 1.0f);
			}
			for (size_t i = 0; i < cells; ++i)
			{
				Assert::AreEqual(held->forward[i * 2] / 32.0 * 2.0, static_cast<double>(fwd.vx[i]), 1e-4);
				Assert::AreEqual(held->forward[i * 2 + 1] / 32.0 * 2.0, static_cast<double>(fwd.vy[i]), 1e-4);
				Assert::AreEqual(held->backward[i * 2] / 32.0 * 2.0, static_cast<double>(bwd.vx[i]), 1e-4);
			}
			Assert::AreEqual<uint64_t>(2, source.GetStats().hits);
			Assert::IsFalse(source.CurrentDecision().engaged, L"served: nothing to show");
			// TemporalDetail's own field source is untouched by this adapter's existence.
			TemporalDetail::FieldMotionSource own;
			TemporalDetail::MotionPair none;
			Assert::IsFalse(own.Forward(5, none));
		}

		// Plausibility of the weighting (not a copy of the formula): in a consistent pair an engine-unsure patch loses all its trust and the
		// rest keeps it; an inconsistent pair (forward and backward disagree by more than twice the limit) is not trusted anywhere.
		TEST_METHOD(AnUnsurePatchLosesTrustAndAnInconsistentPairIsNotTrusted)
		{
			auto slot = std::make_shared<VpMotion::P2Slot>(16);
			slot->SetGeneration(1);
			auto flow = std::make_shared<MotionAhead::Flow>();
			flow->halfW = 64;
			flow->halfH = 64;
			flow->gridW = flow->gridH = 16;
			const size_t cells = 256;
			flow->forward.assign(cells * 2, 0);
			flow->backward.assign(cells * 2, 0);
			flow->forwardCost.assign(cells, 10);
			flow->backwardCost.assign(cells, 10);
			for (int cy = 0; cy < 4; ++cy)
				for (int cx = 0; cx < 4; ++cx)
					flow->forwardCost[static_cast<size_t>(cy) * 16 + cx] = 250;   // the top-left 4 x 4 cells (2 x 2 blocks): the engine is far less sure
			MotionAhead::FlowExtras(*flow);
			Assert::IsTrue(slot->Publish(1, 2, 1, flow));
			MotionValidity::State validity;
			validity.NoteKept(1);
			validity.NoteKept(2);
			TemporalDetail::SharedMotionSource source(VpMotion::P2View(slot), &validity);
			source.SetSourceSize(128, 128);
			TemporalDetail::MotionPair fwd;
			Assert::IsTrue(source.Forward(2, fwd));
			Assert::AreEqual(0.0, static_cast<double>(fwd.trust[0]), 1e-6, L"an engine-unsure block is not trusted");
			Assert::AreEqual(0.0, static_cast<double>(fwd.trust[3 * 16 + 3]), 1e-6);
			Assert::AreEqual(1.0, static_cast<double>(fwd.trust[8 * 16 + 8]), 1e-6, L"a sure, consistent cell keeps full trust");
			// Forward says +8 px, backward says +8 px too (not the inverse): |f + b| = 16 px >> twice the limit everywhere.
			for (size_t i = 0; i < cells; ++i)
			{
				flow->forward[i * 2] = static_cast<int16_t>(8 * 32);
				flow->backward[i * 2] = static_cast<int16_t>(8 * 32);
			}
			flow->forwardCost.assign(cells, 10);
			MotionAhead::FlowExtras(*flow);
			auto slot2 = std::make_shared<VpMotion::P2Slot>(16);
			slot2->SetGeneration(1);
			Assert::IsTrue(slot2->Publish(1, 2, 1, flow));
			TemporalDetail::SharedMotionSource inconsistent(VpMotion::P2View(slot2), &validity);
			inconsistent.SetSourceSize(128, 128);
			TemporalDetail::MotionPair bad;
			Assert::IsTrue(inconsistent.Forward(2, bad));
			for (size_t i = 0; i < cells; ++i)
				Assert::AreEqual(0.0, static_cast<double>(bad.trust[i]), 1e-6, L"forward and backward disagree: not trusted");
			// The matcher's own limit can be handed over: a larger limit accepts the same disagreement.
			inconsistent.SetConsistencyPixels(40.0f);
			TemporalDetail::MotionPair lenient;
			Assert::IsTrue(inconsistent.Forward(2, lenient));
			Assert::AreEqual(1.0, static_cast<double>(lenient.trust[0]), 1e-6, L"within the handed-over limit");
		}

		// Monotonicity, both directions: a higher engine cost, or a higher forward/backward inconsistency, never gives a higher trust.
		TEST_METHOD(HigherCostOrHigherInconsistencyNeverGivesHigherTrust)
		{
			auto trustAt = [](bool forward, uint8_t patchCost, float inconsistentPx, size_t cell) {
				auto slot = std::make_shared<VpMotion::P2Slot>(16);
				slot->SetGeneration(1);
				auto flow = std::make_shared<MotionAhead::Flow>();
				flow->halfW = 64;
				flow->halfH = 64;
				flow->gridW = flow->gridH = 16;
				const size_t cells = 256;
				flow->forward.assign(cells * 2, 0);
				flow->backward.assign(cells * 2, 0);
				flow->forwardCost.assign(cells, 10);
				flow->backwardCost.assign(cells, 10);
				std::vector<uint8_t>& costs = forward ? flow->forwardCost : flow->backwardCost;
				std::vector<int16_t>& vectors = forward ? flow->forward : flow->backward;   // the other direction is zero: |f + b| = inconsistentPx
				for (int cy = 0; cy < 2; ++cy)
					for (int cx = 0; cx < 2; ++cx)
						costs[static_cast<size_t>(cy) * 16 + cx] = patchCost;   // one de-judder block (2 x 2 cells)
				vectors[(8 * 16 + 8) * 2] = static_cast<int16_t>(inconsistentPx * 32.0f);
				MotionAhead::FlowExtras(*flow);
				Assert::IsTrue(slot->Publish(1, 2, 1, flow));
				MotionValidity::State validity;
				validity.NoteKept(1);
				validity.NoteKept(2);
				TemporalDetail::SharedMotionSource source(VpMotion::P2View(slot), &validity);
				source.SetSourceSize(128, 128);
				TemporalDetail::MotionPair out;
				Assert::IsTrue(forward ? source.Forward(2, out) : source.Backward(2, out));
				return static_cast<double>(out.trust[cell]);
			};
			const float limit = TemporalDetail::SharedMotionSource::DefaultConsistencyPixels();
			for (const bool forward : { true, false })
			{
				double previous = 2.0;
				for (const int cost : { 10, 20, 30, 40, 60, 120, 250 })
				{
					const double t = trustAt(forward, static_cast<uint8_t>(cost), 0.0f, 0);
					Assert::IsTrue(t >= 0.0 && t <= 1.0 && t <= previous + 1e-9, L"a higher cost never gives a higher trust");
					previous = t;
				}
				Assert::IsTrue(previous < 0.01 && trustAt(forward, 10, 0.0f, 0) > 0.99, L"(and the range is used: sure = 1, far less sure = 0)");
				previous = 2.0;
				for (const float factor : { 0.0f, 0.5f, 1.0f, 1.25f, 1.5f, 1.75f, 2.0f, 3.0f })
				{
					const double t = trustAt(forward, 10, factor * limit, 8 * 16 + 8);
					Assert::IsTrue(t >= 0.0 && t <= 1.0 && t <= previous + 1e-9, L"a higher inconsistency never gives a higher trust");
					previous = t;
				}
				Assert::IsTrue(previous < 0.01, L"inconsistent by more than twice the limit: not trusted");
			}
		}

		// The sustained-absent threshold is derived from the look-ahead depth the way the "motion" entry's NoPairTracker does, not a constant.
		TEST_METHOD(TheSustainedAbsentThresholdFollowsTheLookAheadDepth)
		{
			Assert::AreEqual(8u, TemporalDetail::SharedMotionSource::AbsentRun(0), L"the floor");
			Assert::AreEqual(8u, TemporalDetail::SharedMotionSource::AbsentRun(4), L"60p depth 4 + margin 4");
			Assert::AreEqual(20u, TemporalDetail::SharedMotionSource::AbsentRun(16), L"240p depth 16 + margin 4");
			TemporalDetail::SharedMotionSource shallow((VpMotion::P2View()), nullptr), deep((VpMotion::P2View()), nullptr);
			shallow.SetSourceSize(192, 96);
			deep.SetSourceSize(192, 96);
			deep.SetDepth(16);
			TemporalDetail::MotionPair out;
			for (unsigned i = 0; i < 19; ++i)
			{
				shallow.Forward(2, out);
				deep.Forward(2, out);
			}
			Assert::IsTrue(shallow.CurrentDecision().engaged, L"19 absent in a row is long past the shallow queue's threshold");
			Assert::IsFalse(deep.CurrentDecision().engaged, L"a refill of a deep queue is not yet a sustained loss");
			deep.Forward(2, out);
			Assert::IsTrue(deep.CurrentDecision().engaged, L"20 in a row shows");
		}

		// W1-C3: the forward direction reads the producer's Flow::fbError (worked out once on its worker) instead of recomputing it; the
		// trust is bit-equal to the recompute path (same FbError on the same fields). The backward direction has no producer extra and
		// still computes (counted).
		TEST_METHOD(TheForwardTrustReadsTheProducersErrorAndIsBitEqualToTheRecompute)
		{
			auto slot = std::make_shared<VpMotion::P2Slot>(16);
			slot->SetGeneration(1);
			auto withExtras = MakeFlow(96, 48);
			MotionAhead::FlowExtras(*withExtras);
			Assert::IsFalse(withExtras->fbError.empty());
			auto recompute = std::make_shared<MotionAhead::Flow>(*withExtras);
			recompute->fbError.clear();   // the same pair without the producer's error: Take works it out
			Assert::IsTrue(slot->Publish(4, 5, 1, withExtras));
			Assert::IsTrue(slot->Publish(5, 6, 1, recompute));
			MotionValidity::State validity;
			validity.NoteKept(4);
			validity.NoteKept(5);
			validity.NoteKept(6);
			TemporalDetail::SharedMotionSource source(VpMotion::P2View(slot), &validity);
			source.SetSourceSize(192, 96);
			TemporalDetail::MotionPair a, b, back;
			Assert::IsTrue(source.Forward(5, a));
			Assert::AreEqual<uint64_t>(0, source.GetStats().fbErrorComputed, L"forward: the producer's error, not recomputed");
			Assert::IsTrue(source.Forward(6, b));
			Assert::AreEqual<uint64_t>(1, source.GetStats().fbErrorComputed, L"no producer error: computed here");
			Assert::AreEqual(a.trust.size(), b.trust.size());
			for (size_t i = 0; i < a.trust.size(); ++i)
				Assert::AreEqual(b.trust[i], a.trust[i], 0.0f, L"bit-equal to the recompute path");
			Assert::IsTrue(a.vx == b.vx && a.vy == b.vy);
			Assert::IsTrue(source.Backward(5, back));
			Assert::AreEqual<uint64_t>(2, source.GetStats().fbErrorComputed, L"backward: no producer extra, computed");
		}

		// W1-C3 cost line: TD's per-pair forward Take with the error recomputed (before) and read from the producer (after), at the engine
		// grids of a 1080p and a 2160p source. Logged, never gated (timing).
		TEST_METHOD(CostLineForwardTakeBeforeAndAfter)
		{
			const int sizes[2][2] = { { 960, 540 }, { 1920, 1080 } };
			for (int s = 0; s < 2; ++s)
			{
				auto slot = std::make_shared<VpMotion::P2Slot>(16);
				slot->SetGeneration(1);
				auto withExtras = MakeFlow(sizes[s][0], sizes[s][1]);
				MotionAhead::FlowExtras(*withExtras);
				auto recompute = std::make_shared<MotionAhead::Flow>(*withExtras);
				recompute->fbError.clear();
				Assert::IsTrue(slot->Publish(4, 5, 1, recompute));
				Assert::IsTrue(slot->Publish(5, 6, 1, withExtras));
				TemporalDetail::SharedMotionSource source(VpMotion::P2View(slot), nullptr);
				source.SetSourceSize(sizes[s][0] * 2, sizes[s][1] * 2);
				const int reps = 20;
				TemporalDetail::MotionPair out;
				double sink = 0;
				const auto t0 = std::chrono::steady_clock::now();
				for (int r = 0; r < reps; ++r) { source.Forward(5, out); sink += out.trust.empty() ? 0.0 : out.trust[0]; }
				const auto t1 = std::chrono::steady_clock::now();
				for (int r = 0; r < reps; ++r) { source.Forward(6, out); sink += out.trust.empty() ? 0.0 : out.trust[0]; }
				const auto t2 = std::chrono::steady_clock::now();
				const double before = std::chrono::duration<double, std::milli>(t1 - t0).count() / reps;
				const double after = std::chrono::duration<double, std::milli>(t2 - t1).count() / reps;
				char line[256];
				std::snprintf(line, sizeof(line), "W1C3_COST td forward Take grid=%dx%d before_ms=%.3f after_ms=%.3f (sink %.1f)",
					withExtras->gridW, withExtras->gridH, before, after, sink);
				Logger::WriteMessage(line);
				Assert::AreEqual<uint64_t>(static_cast<uint64_t>(reps), source.GetStats().fbErrorComputed, L"only the recompute pair computed");
			}
		}

		// C2a-F 2: once a frame is kept in the new epoch the epoch form of the gate no longer refuses the old pair; the pair's stream
		// generation does (resync_event off: the validity is unbound).
		TEST_METHOD(APairOfAnOlderStreamGenerationIsStaleAfterTheNextKeptFrame)
		{
			auto slot = std::make_shared<VpMotion::P2Slot>(16);
			MotionValidity::State validity;
			slot->SetGeneration(validity.StreamGeneration());
			validity.NoteKept(1);
			validity.NoteKept(2);
			Assert::IsTrue(slot->Publish(1, 2, slot->Generation(), MakeFlow(96, 48)));
			TemporalDetail::SharedMotionSource source(VpMotion::P2View(slot), &validity);
			source.SetSourceSize(192, 96);
			TemporalDetail::MotionPair out;
			Assert::IsTrue(source.Forward(2, out), L"control: served in its own stream generation");
			validity.Invalidate(MotionValidity::Cause::Seek);
			validity.NoteKept(3);
			Assert::IsTrue(validity.PairServes(1, 2, validity.EpochOfLastKept(), validity.Epoch()),
				L"premise: the epoch form alone no longer refuses the pair");
			const uint64_t staleBefore = source.GetStats().stale;
			Assert::IsFalse(source.Forward(2, out), L"the pair of the older stream generation is refused");
			Assert::IsFalse(source.Backward(2, out), L"in both directions");
			Assert::AreEqual<uint64_t>(staleBefore + 2, source.GetStats().stale, L"counted stale");
			// Once the producer re-keys the slot and publishes again, the new pair is served.
			slot->SetGeneration(validity.StreamGeneration());
			Assert::IsTrue(slot->Publish(1, 2, slot->Generation(), MakeFlow(96, 48)));
			Assert::IsTrue(source.Forward(2, out), L"a pair of the current stream generation is served");
		}

		TEST_METHOD(AMissingStaleOrUnfitPairIsAbsentCountedAndVisible)
		{
			auto slot = std::make_shared<VpMotion::P2Slot>(16);
			slot->SetGeneration(1);
			MotionValidity::State validity;
			validity.NoteKept(1);
			validity.NoteKept(2);
			TemporalDetail::SharedMotionSource source(VpMotion::P2View(slot), &validity);
			TemporalDetail::MotionPair out;
			// Missing: no pair in the slot.
			source.SetSourceSize(192, 96);
			Assert::IsFalse(source.Forward(2, out), L"no pair computed ahead: absent");
			Assert::AreEqual<uint64_t>(1, source.GetStats().absent);
			Assert::IsFalse(out.valid);
			// Unfit: a pair exists but the source size is not known.
			Assert::IsTrue(slot->Publish(1, 2, 1, MakeFlow(96, 48)));
			TemporalDetail::SharedMotionSource unknownSize(VpMotion::P2View(slot), &validity);
			Assert::IsFalse(unknownSize.Forward(2, out), L"the source size is not known: absent, not guessed");
			Assert::AreEqual<uint64_t>(1, unknownSize.GetStats().unfit);
			// Served, then the vectors-valid state moves (a resync): the same pair is STALE.
			Assert::IsTrue(source.Forward(2, out));
			validity.Invalidate(MotionValidity::Cause::Resync);
			Assert::IsFalse(source.Forward(2, out), L"stale after a resync");
			Assert::AreEqual<uint64_t>(1, source.GetStats().stale);
			// A sustained run is a visible entry with the reason; a served pair releases it.
			for (unsigned i = 0; i < TemporalDetail::SharedMotionSource::AbsentRun(0); ++i)
				Assert::IsFalse(source.Forward(2, out));
			const FallbackCatalog::Decision shown = source.CurrentDecision();
			Assert::IsTrue(shown.engaged && shown.id == "td motion");
			Assert::IsTrue(shown.reason.find("vectors-valid state moved") != std::string::npos, L"with the plain reason");
			// No vector engine at all (an unbound view): absent, with its own words.
			TemporalDetail::SharedMotionSource noEngine((VpMotion::P2View()), nullptr);
			noEngine.SetSourceSize(192, 96);
			Assert::IsFalse(noEngine.Backward(3, out));
			Assert::IsTrue(noEngine.CurrentDecision().reason.empty() || !noEngine.CurrentDecision().engaged);
			Assert::IsFalse(noEngine.Forward(0, out), L"sequence 0 has no previous frame");
		}
	};
}
