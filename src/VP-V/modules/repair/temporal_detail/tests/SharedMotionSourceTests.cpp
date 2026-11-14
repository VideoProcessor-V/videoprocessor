// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// SK-MV MV5: TemporalDetail::SharedMotionSource (the IMotionSource over the vector engine's P2 view). CPU only. The adapter answers with
// exactly what the shared view holds for the pair, answers ABSENT (and counts and shows it) for a missing, stale or unfit pair, and leaves
// TemporalDetail's own sources untouched.
#include "pch.h"
#include "CppUnitTest.h"

#include <VP-V/modules/motion/estimation/OpticalFlowTrust.h>
#include <VP-V/modules/motion/estimation/VectorEngine.h>
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
		// L-B6 (WAIVER-B6-TD-TRUST): the adapter reads its trust from the ENGINE memo, so the tests run over a real (producer-less) engine:
		// the P2 slot, the vectors-valid state and FlowOf are the engine's. `sourceW` = the source width the engine size is noted for.
		struct EngineRig
		{
			VpMotion::VectorEngine engine;
			explicit EngineRig(int sourceW)
			{
				engine.PublishProducer(std::make_shared<MotionAhead::Producer>());
				engine.NoteFlowSourceWidth(sourceW);
			}
			std::shared_ptr<VpMotion::P2Slot> Slot() { return engine.p2Slot; }
			bool Publish(uint64_t from, uint64_t to, const MotionAhead::FlowPtr& flow) { return engine.p2Slot->Publish(from, to, engine.p2Slot->Generation(), flow); }
			VpMotion::P2View View() { return VpMotion::P2View(engine.p2Slot); }
			TemporalDetail::SharedMotionSource::FlowOfFn FlowOf()
			{
				VpMotion::VectorEngine* e = &engine;
				return [e](uint64_t seq, VpMotion::FlowView& out) { return e->FlowOf(seq, out); };
			}
		};

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
			EngineRig rig(192);   // the producer worked at half the source: the source is 192 wide, so a producer pixel is 2 source pixels
			const auto flow = MakeFlow(96, 48);
			MotionAhead::FlowExtras(*flow);   // the producer's worker does this: the de-judder's cost and consistency weights
			Assert::IsTrue(rig.Publish(4, 5, flow));
			rig.engine.motionValidity.NoteKept(4);
			rig.engine.motionValidity.NoteKept(5);
			TemporalDetail::SharedMotionSource source(rig.View(), &rig.engine.motionValidity, rig.FlowOf());
			source.SetSourceSize(192, 96);
			TemporalDetail::MotionPair fwd, bwd;
			Assert::IsTrue(source.Forward(5, fwd), L"Forward(seq) is the pair (seq - 1 -> seq)");
			Assert::IsTrue(source.Backward(5, bwd), L"Backward(seq) is the pair (seq -> seq - 1)");
			const MotionAhead::FlowPtr held = rig.View().Find(4, 5);   // what the shared view holds for the same pair
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
			EngineRig rig(128);
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
			Assert::IsTrue(rig.Publish(1, 2, std::make_shared<MotionAhead::Flow>(*flow)));
			rig.engine.motionValidity.NoteKept(1);
			rig.engine.motionValidity.NoteKept(2);
			TemporalDetail::SharedMotionSource source(rig.View(), &rig.engine.motionValidity, rig.FlowOf());
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
			EngineRig rig2(128);   // a fresh engine: the lease and its trust memo belong to the pair as published
			Assert::IsTrue(rig2.Publish(1, 2, std::make_shared<MotionAhead::Flow>(*flow)));
			rig2.engine.motionValidity.NoteKept(1);
			rig2.engine.motionValidity.NoteKept(2);
			TemporalDetail::SharedMotionSource inconsistent(rig2.View(), &rig2.engine.motionValidity, rig2.FlowOf());
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
				EngineRig rig(128);
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
				Assert::IsTrue(rig.Publish(1, 2, flow));
				rig.engine.motionValidity.NoteKept(1);
				rig.engine.motionValidity.NoteKept(2);
				TemporalDetail::SharedMotionSource source(rig.View(), &rig.engine.motionValidity, rig.FlowOf());
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

		// L-B6 MIGRATION IDENTITY (WAIVER-B6-TD-TRUST, rule 28 condition): TD held its own copy of the engine trust (CostWeights + FbError +
		// CellTrust per direction, the forward direction taking the producer's extras when present). That arithmetic is gone from the
		// adapter; HERE it is the TEST-SIDE REFERENCE (the old recompute, verbatim) and the engine memo the adapter now reads must equal it
		// bit for bit, in BOTH directions, with and without the producer's extras, at the default and a handed-over tolerance. Any
		// difference is a behaviour change (a registered fix with before/after), never a tolerance widening.
		static std::vector<float> OldTdRecompute(const MotionAhead::Flow& f, bool forward, float tolerance)
		{
			const std::vector<int16_t>& vec = forward ? f.forward : f.backward;
			const std::vector<uint8_t>& cost = forward ? f.forwardCost : f.backwardCost;
			const std::vector<int16_t>& other = forward ? f.backward : f.forward;
			const size_t cells = static_cast<size_t>(f.gridW) * f.gridH;
			std::vector<float> cellErrorLocal, blockCost, trust;
			const std::vector<float>* cellError = &cellErrorLocal;
			if (forward && f.fbError.size() == cells)
				cellError = &f.fbError;
			else
				Assert::IsTrue(OpticalFlowTrust::FbError(vec, other, f.gridW, f.gridH, NvOpticalFlow::Engine::Grid, cellErrorLocal));
			const int blocksX = (f.halfW + MotionAhead::Flow::kBlockPx - 1) / MotionAhead::Flow::kBlockPx;
			const int blocksY = (f.halfH + MotionAhead::Flow::kBlockPx - 1) / MotionAhead::Flow::kBlockPx;
			const std::vector<float>* costBlocks = nullptr;
			if (forward && f.blocksX == blocksX && f.blocksY == blocksY && f.costWeight.size() == static_cast<size_t>(blocksX) * blocksY)
				costBlocks = &f.costWeight;
			else
			{
				Assert::IsTrue(OpticalFlowTrust::CostWeights(cost, f.gridW, f.gridH, NvOpticalFlow::Engine::Grid, MotionAhead::Flow::kBlockPx, blocksX, blocksY, blockCost));
				costBlocks = &blockCost;
			}
			Assert::IsTrue(OpticalFlowTrust::CellTrust(*costBlocks, blocksX, blocksY, *cellError, f.gridW, f.gridH, NvOpticalFlow::Engine::Grid,
				MotionAhead::Flow::kBlockPx, tolerance, trust));
			return trust;
		}

		TEST_METHOD(TheEngineMemoTrustEqualsTheRetiredPrivateRecomputeBothDirections)
		{
			EngineRig rig(192);
			auto withExtras = MakeFlow(96, 48);
			MotionAhead::FlowExtras(*withExtras);
			Assert::IsFalse(withExtras->fbError.empty());
			auto without = std::make_shared<MotionAhead::Flow>(*withExtras);
			without->fbError.clear();
			without->costWeight.clear();
			without->blocksX = without->blocksY = 0;
			Assert::IsTrue(rig.Publish(4, 5, withExtras));
			Assert::IsTrue(rig.Publish(5, 6, without));
			rig.engine.motionValidity.NoteKept(4);
			rig.engine.motionValidity.NoteKept(5);
			rig.engine.motionValidity.NoteKept(6);
			TemporalDetail::SharedMotionSource source(rig.View(), &rig.engine.motionValidity, rig.FlowOf());
			source.SetSourceSize(192, 96);
			for (const float tolerance : { 2.0f, 1.5f })
			{
				source.SetConsistencyPixels(tolerance);
				for (const uint64_t seq : { uint64_t(5), uint64_t(6) })
				{
					const MotionAhead::FlowPtr held = rig.View().Find(seq - 1, seq);
					Assert::IsTrue(held != nullptr);
					for (const bool forward : { true, false })
					{
						TemporalDetail::MotionPair got;
						Assert::IsTrue(forward ? source.Forward(seq, got) : source.Backward(seq, got));
						const std::vector<float> reference = OldTdRecompute(*held, forward, tolerance);
						Assert::AreEqual(reference.size(), got.trust.size());
						for (size_t i = 0; i < reference.size(); ++i)
							Assert::AreEqual(reference[i], got.trust[i], 0.0f, L"the engine memo is bit-equal to the retired recompute");
					}
				}
			}
			// The producer's extras do not change the trust (forward), and the pair's two directions differ (the test is not vacuous).
			TemporalDetail::MotionPair a, b, back;
			source.SetConsistencyPixels(2.0f);
			// Restored against the engine memo (was fbErrorComputed 0/1/2): each (pair, direction, tolerance) is computed ONCE, by the engine
			// (its trustComputes counter), none by the adapter; the lease is per pair, so each of these three first asks is one compute.
			const unsigned long long computes0 = rig.engine.FlowReads().trustComputes;
			Assert::IsTrue(source.Forward(5, a));
			Assert::AreEqual<unsigned long long>(computes0 + 1, rig.engine.FlowReads().trustComputes, L"forward, producer extras: one engine compute");
			Assert::IsTrue(source.Forward(6, b));
			Assert::AreEqual<unsigned long long>(computes0 + 2, rig.engine.FlowReads().trustComputes, L"forward, no producer extras: one engine compute");
			Assert::IsTrue(a.trust == b.trust);
			Assert::IsTrue(a.vx == b.vx && a.vy == b.vy, L"the vectors are the same pair's, whichever pair carried the extras");
			Assert::IsTrue(source.Backward(5, back));
			Assert::AreEqual<unsigned long long>(computes0 + 3, rig.engine.FlowReads().trustComputes, L"backward: one engine compute");
			Assert::IsTrue(back.trust != a.trust);
			// Computed once by the engine per pair, direction and tolerance, however often TD asked; none worked out by the adapter.
			{   // WIP-DIAG (revert): why the second Forward(6) fails
				const bool okDiag = source.Forward(6, b);
				const auto sd = source.GetStats();
				char diag[300];
				snprintf(diag, sizeof(diag), "DIAG Forward(6) ok=%d hits=%llu absent=%llu stale=%llu unfit=%llu why=%s", okDiag ? 1 : 0, (unsigned long long)sd.hits, (unsigned long long)sd.absent, (unsigned long long)sd.stale, (unsigned long long)sd.unfit, source.CurrentDecision().reason.c_str());
				Logger::WriteMessage(diag);
			}
			Assert::IsTrue(source.Forward(6, b));
			Assert::IsTrue(source.Backward(6, back));
			const unsigned long long before = rig.engine.FlowReads().trustComputes;
			for (int r = 0; r < 5; ++r)
			{
				Assert::IsTrue(source.Forward(6, b));
				Assert::IsTrue(source.Backward(6, back));
			}
			Assert::AreEqual(before, rig.engine.FlowReads().trustComputes, L"repeat asks are memo hits (the engine counts computes)");
			// No engine handle: no trust, absent (never guessed).
			TemporalDetail::SharedMotionSource noTrust(rig.View(), &rig.engine.motionValidity);
			noTrust.SetSourceSize(192, 96);
			TemporalDetail::MotionPair none;
			Assert::IsFalse(noTrust.Forward(5, none));
			Assert::AreEqual<uint64_t>(1, noTrust.GetStats().unfit);
		}

		// Cost line: TD's per-pair Take (cold: the engine computes the memo; warm: the memo is read) at the engine grids of a 1080p and a
		// 2160p source. Logged, never gated (timing); the compute count is exact.
		TEST_METHOD(CostLineTakeColdAndWarmFromTheEngineMemo)
		{
			const int sizes[2][2] = { { 960, 540 }, { 1920, 1080 } };
			for (int s = 0; s < 2; ++s)
			{
				EngineRig rig(sizes[s][0] * 2);
				auto flow = MakeFlow(sizes[s][0], sizes[s][1]);
				MotionAhead::FlowExtras(*flow);
				Assert::IsTrue(rig.Publish(4, 5, flow));
				rig.engine.motionValidity.NoteKept(4);
				rig.engine.motionValidity.NoteKept(5);
				TemporalDetail::SharedMotionSource source(rig.View(), &rig.engine.motionValidity, rig.FlowOf());
				source.SetSourceSize(sizes[s][0] * 2, sizes[s][1] * 2);
				const int reps = 20;
				TemporalDetail::MotionPair out;
				double sink = 0;
				const auto t0 = std::chrono::steady_clock::now();
				Assert::IsTrue(source.Forward(5, out));
				sink += out.trust[0];
				const auto t1 = std::chrono::steady_clock::now();
				for (int r = 0; r < reps; ++r) { source.Forward(5, out); sink += out.trust.empty() ? 0.0 : out.trust[0]; }
				const auto t2 = std::chrono::steady_clock::now();
				const double cold = std::chrono::duration<double, std::milli>(t1 - t0).count();
				const double warm = std::chrono::duration<double, std::milli>(t2 - t1).count() / reps;
				char line[256];
				std::snprintf(line, sizeof(line), "W1C3_COST td forward Take grid=%dx%d cold_ms=%.3f warm_ms=%.3f (sink %.1f)",
					flow->gridW, flow->gridH, cold, warm, sink);
				Logger::WriteMessage(line);
				Assert::AreEqual(1ull, rig.engine.FlowReads().trustComputes, L"one engine compute, however often TD asked");
			}
		}

		// C2a-F 2: once a frame is kept in the new epoch the epoch form of the gate no longer refuses the old pair; the pair's stream
		// generation does (resync_event off: the validity is unbound).
		TEST_METHOD(APairOfAnOlderStreamGenerationIsStaleAfterTheNextKeptFrame)
		{
			EngineRig rig(192);
			MotionValidity::State& validity = rig.engine.motionValidity;
			auto slot = rig.Slot();
			slot->SetGeneration(validity.StreamGeneration());
			validity.NoteKept(1);
			validity.NoteKept(2);
			Assert::IsTrue(slot->Publish(1, 2, slot->Generation(), MakeFlow(96, 48)));
			TemporalDetail::SharedMotionSource source(rig.View(), &validity, rig.FlowOf());
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
			EngineRig rig(192);
			auto slot = rig.Slot();
			MotionValidity::State& validity = rig.engine.motionValidity;
			validity.NoteKept(1);
			validity.NoteKept(2);
			TemporalDetail::SharedMotionSource source(rig.View(), &validity, rig.FlowOf());
			TemporalDetail::MotionPair out;
			// Missing: no pair in the slot.
			source.SetSourceSize(192, 96);
			Assert::IsFalse(source.Forward(2, out), L"no pair computed ahead: absent");
			Assert::AreEqual<uint64_t>(1, source.GetStats().absent);
			Assert::IsFalse(out.valid);
			// Unfit: a pair exists but the source size is not known.
			Assert::IsTrue(slot->Publish(1, 2, slot->Generation(), MakeFlow(96, 48)));
			TemporalDetail::SharedMotionSource unknownSize(rig.View(), &validity, rig.FlowOf());
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
