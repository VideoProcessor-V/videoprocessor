// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include "CppUnitTest.h"

// Camera AHEAD FIT (owner decision, reset 1): the fit reads the ahead NVOF pair (IVectorEngine::FlowOf) and publishes P7 on time when it
// finished before the frame's seal, else through the lag bridge. CPU only: the copy and the layout checks, the conversion onto the 16-px
// source grid (half- and quarter-size cells, deterministic), the ONE engine trust (OpticalFlowTrust::CellTrust = MV5's formula), F2
// provenance (Ahead accepted, Live still refused), the worker's park / on-time / bridge decisions, the resync re-check at the seal and at
// use, the deterministic path (the bridge) and "off is inert".
#include "CameraLinkTestSupport.h"
#include <VP-V/modules/motion/estimation/MotionAheadProducer.h>
#include <VP-V/modules/motion/estimation/OpticalFlowTrust.h>
#include <VP-V/modules/motion/estimation/VectorEngineInterface.h>
#include <VP-V/modules/motion/estimation/global_model/link/AheadFlow.h>
#include <VP-V/modules/motion/estimation/global_model/link/CameraLinkCore.h>
#include <VP-V/modules/motion/estimation/global_model/link/CameraSettings.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <string>
#include <type_traits>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace CameraAheadFitTestsNs
{
	// Provenance by type: the ahead input is made only from a FlowView (the producer's unguided pair), never from the guided field or
	// the seed, and it is not either of them.
	static_assert(!std::is_constructible<CameraLink::AheadCopy, const VpMotion::VerifiedField&>::value, "ahead from the guided field");
	static_assert(!std::is_constructible<CameraLink::AheadCopy, const VpMotion::SeedField&>::value, "ahead from the seed");
	static_assert(!std::is_base_of<VpMotion::VerifiedField, CameraLink::AheadCopy>::value, "ahead is not a verified field");
	static_assert(!std::is_assignable<CameraLink::AheadCopy&, const VpMotion::VerifiedField&>::value, "verified field assigns to ahead");
	// The trust block is the producer's (FlowExtras), not a camera choice.
	static_assert(CameraLink::kEngineTrustBlockPx == MotionAhead::Flow::kBlockPx, "the engine trust block is the de-judder's");

	// A synthetic engine pair: S10.5 vectors per 4 x 4 cell of an engineW x engineH luma.
	struct Pair
	{
		int engineW = 0, engineH = 0, gridW = 0, gridH = 0;
		double toSource = 2.0;   // source px per engine px
		std::vector<int16_t> fwd, bwd;
		std::vector<uint8_t> cost;
		Pair(int w, int h, double scale) : engineW(w), engineH(h), gridW((w + 3) / 4), gridH((h + 3) / 4), toSource(scale)
		{
			fwd.assign(static_cast<size_t>(gridW) * gridH * 2, 0);
			bwd.assign(fwd.size(), 0);
			cost.assign(static_cast<size_t>(gridW) * gridH, 10);
		}
		// A uniform motion of (sx, sy) SOURCE px (consistent backward).
		void Uniform(double sx, double sy)
		{
			for (size_t i = 0; i < cost.size(); ++i)
			{
				fwd[i * 2] = static_cast<int16_t>(std::lround(sx / toSource * 32.0));
				fwd[i * 2 + 1] = static_cast<int16_t>(std::lround(sy / toSource * 32.0));
				bwd[i * 2] = static_cast<int16_t>(-fwd[i * 2]);
				bwd[i * 2 + 1] = static_cast<int16_t>(-fwd[i * 2 + 1]);
			}
		}
		VpMotion::FlowView View(uint64_t seq) const
		{
			VpMotion::FlowView v;
			v.from = seq - 1;
			v.to = seq;
			v.gridW = gridW;
			v.gridH = gridH;
			v.blockSize = 4.0 * toSource;
			v.rawForward = fwd.data();
			v.rawBackward = bwd.data();
			v.forwardCost = cost.data();
			v.backwardCost = cost.data();
			v.rawUnitsPerEnginePx = 32.0;
			v.enginePxToSource = toSource;
			v.cellEnginePx = 4;
			v.engineW = engineW;
			v.engineH = engineH;
			return v;
		}
	};

	// MV5's per-cell trust exactly as SK-MV wrote it inline (SharedMotionSource::Take, forward): the reference CellTrust must equal.
	std::vector<float> Mv5Trust(const CameraLink::AheadCopy& a, float consistencyPx)
	{
		std::vector<float> cellError, blockCost;
		OpticalFlowTrust::FbError(a.forward, a.backward, a.gridW, a.gridH, NvOpticalFlow::Engine::Grid, cellError);
		const int blocksX = (a.engineW + MotionAhead::Flow::kBlockPx - 1) / MotionAhead::Flow::kBlockPx;
		const int blocksY = (a.engineH + MotionAhead::Flow::kBlockPx - 1) / MotionAhead::Flow::kBlockPx;
		OpticalFlowTrust::CostWeights(a.forwardCost, a.gridW, a.gridH, NvOpticalFlow::Engine::Grid, MotionAhead::Flow::kBlockPx, blocksX, blocksY, blockCost);
		const int perBlock = OpticalFlowTrust::CellsPerBlock(NvOpticalFlow::Engine::Grid, MotionAhead::Flow::kBlockPx);
		std::vector<float> t(a.Cells());
		for (size_t i = 0; i < t.size(); ++i)
		{
			const int cx = static_cast<int>(i % static_cast<size_t>(a.gridW)), cy = static_cast<int>(i / static_cast<size_t>(a.gridW));
			const size_t block = static_cast<size_t>((std::min)(cy / perBlock, blocksY - 1)) * blocksX + (std::min)(cx / perBlock, blocksX - 1);
			t[i] = blockCost[block] * OpticalFlowTrust::FallOff(consistencyPx, 2.0f * consistencyPx, cellError[i]);
		}
		return t;
	}

	// A second rig whose P7 is sealed AT its frame (the on-time target: what the skeleton lane would be).
	VpSkeleton::LaneLeaseSource SourceOf(VideoProcessorTest::CameraLinkTest::Rig& r)
	{
		VpSkeleton::SlotTable* t = r.table.get();
		VpSkeleton::CpuBuffers* b = r.bytes.get();
		VpIndicator::Indicator* i = r.indicator.get();
		return [t, b, i]() { return VpSkeleton::LaneLease::Borrow(t, b, i); };
	}

	// The skeleton's seal of frame seq in the on-time rig: Resolve with the frame's stamp; a product published outside is never live-filled.
	void SealAt(VideoProcessorTest::CameraLinkTest::Rig& r, uint64_t seq)
	{
		r.table->Resolve(seq, r.indicator->StampOf(seq), [](const VpSkeleton::Node&, uint64_t, int, VpAncestry::Header&, uint32_t) { return false; });
	}

	std::shared_ptr<CameraLink::FieldCopy> AheadField(uint64_t seq, double tx)
	{
		auto f = VideoProcessorTest::CameraLinkTest::PanJob(seq, tx).field;
		auto c = std::make_shared<CameraLink::FieldCopy>(*f);
		c->origin = CameraLink::FieldOrigin::Ahead;
		return c;
	}

	bool Has(const std::vector<std::string>& lines, const std::string& needle)
	{
		for (const std::string& l : lines)
			if (l.find(needle) != std::string::npos)
				return true;
		return false;
	}
}

namespace VideoProcessorTest
{
	using namespace CameraAheadFitTestsNs;

	TEST_CLASS(CameraAheadFitTests)
	{
	public:
		TEST_METHOD(TheCopyReadsTheLayoutFromTheViewAndOwnsItsArrays)
		{
			Pair p(960, 540, 2.0);
			p.Uniform(3.0, -1.0);
			CameraLink::AheadCopy a;
			Assert::IsTrue(CameraLink::CopyAhead(p.View(7), 7, a) == CameraLink::AheadRefusal::None);
			Assert::AreEqual(240, a.gridW);
			Assert::AreEqual(135, a.gridH);
			Assert::AreEqual(8.0, a.CellSourcePx(), 1e-12, L"4 engine px x 2 source px per engine px");
			const int16_t before = a.forward[0];
			p.fwd[0] = 999;   // the engine's lease is replaced by the next FlowOf: the copy must not see it
			Assert::AreEqual(before, a.forward[0], L"copied at once, never pointing into the view");
			// Never guessed: wrong frame, no backward, inconsistent grid.
			Assert::IsTrue(CameraLink::CopyAhead(p.View(7), 8, a) == CameraLink::AheadRefusal::NoPair);
			VpMotion::FlowView v = p.View(7);
			v.rawBackward = nullptr;
			Assert::IsTrue(CameraLink::CopyAhead(v, 7, a) == CameraLink::AheadRefusal::Incomplete);
			v = p.View(7);
			v.gridW += 1;
			Assert::IsTrue(CameraLink::CopyAhead(v, 7, a) == CameraLink::AheadRefusal::Layout);
			v = p.View(7);
			v.cellEnginePx = 8;   // the cell size is read, not assumed: an 8-px cell does not fit this grid
			Assert::IsTrue(CameraLink::CopyAhead(v, 7, a) == CameraLink::AheadRefusal::Layout);
		}

		TEST_METHOD(AHalfSizePairPoolsTwoByTwoCellsPerSourceBlockDeterministically)
		{
			// 1080p: half-size 960 x 540 engine luma, 8-px cells: each 16-px block holds 2 x 2 cell centres.
			Pair p(960, 540, 2.0);
			p.Uniform(3.25, -0.5);
			CameraLink::AheadCopy a;
			Assert::IsTrue(CameraLink::CopyAhead(p.View(5), 5, a) == CameraLink::AheadRefusal::None);
			CameraLink::FieldCopy f1, f2;
			CameraLink::AheadConvertInfo info;
			Assert::IsTrue(CameraLink::AheadToSourceGrid(a, 2.0f, 120, 68, f1, &info));
			Assert::IsTrue(CameraLink::AheadToSourceGrid(a, 2.0f, 120, 68, f2));
			Assert::IsTrue(f1.origin == CameraLink::FieldOrigin::Ahead);
			Assert::AreEqual(4, info.maxCellsPerBlock);
			Assert::AreEqual(16.0, f1.blockSize);
			Assert::IsTrue(f1.Valid() && f1.Blocks() == 120u * 68u);
			for (size_t i = 0; i < f1.Blocks(); ++i)
			{
				Assert::AreEqual(3.25f, f1.fwdX[i], 1e-6f);
				Assert::AreEqual(-0.5f, f1.fwdY[i], 1e-6f);
				Assert::AreEqual(1.0f, f1.fwdTrust[i], 1e-6f, L"a clean pan: fully trusted");
			}
			Assert::IsTrue(f1.fwdX == f2.fwdX && f1.fwdY == f2.fwdY && f1.fwdTrust == f2.fwdTrust, L"deterministic");
			// A grid the pair does not cover is refused (never stretched).
			Assert::IsFalse(CameraLink::AheadToSourceGrid(a, 2.0f, 240, 135, f1));
		}

		TEST_METHOD(AQuarterSizePairGivesOneCellPerSourceBlock)
		{
			// lowRes 4K: quarter-size 960 x 540 engine luma for a 3840 x 2160 source: 16-px cells, one per block.
			Pair p(960, 540, 4.0);
			for (int cy = 0; cy < p.gridH; ++cy)
				for (int cx = 0; cx < p.gridW; ++cx)
				{
					const size_t i = static_cast<size_t>(cy) * p.gridW + cx;
					p.fwd[i * 2] = static_cast<int16_t>(cx % 7);   // a distinct vector per cell (units of 1/32 engine px)
					p.bwd[i * 2] = static_cast<int16_t>(-(cx % 7));
				}
			CameraLink::AheadCopy a;
			Assert::IsTrue(CameraLink::CopyAhead(p.View(5), 5, a) == CameraLink::AheadRefusal::None);
			CameraLink::FieldCopy f;
			CameraLink::AheadConvertInfo info;
			Assert::IsTrue(CameraLink::AheadToSourceGrid(a, 2.0f, 240, 135, f, &info));
			Assert::AreEqual(1, info.maxCellsPerBlock);
			for (int tx = 0; tx < 240; ++tx)
				Assert::AreEqual(static_cast<float>((tx % 7) / 32.0 * 4.0), f.fwdX[static_cast<size_t>(tx)], 1e-6f, L"block t takes cell t");
		}

		TEST_METHOD(TheTrustIsTheEngineDefinitionAndIsMonotonic)
		{
			Pair p(320, 180, 2.0);
			uint32_t r = 12345;
			auto rnd = [&r]() { r = r * 1664525u + 1013904223u; return r >> 8; };
			for (size_t i = 0; i < p.cost.size(); ++i)
			{
				p.fwd[i * 2] = static_cast<int16_t>(static_cast<int>(rnd() % 200) - 100);
				p.fwd[i * 2 + 1] = static_cast<int16_t>(static_cast<int>(rnd() % 200) - 100);
				p.bwd[i * 2] = static_cast<int16_t>(-p.fwd[i * 2] + static_cast<int>(rnd() % 64) - 32);
				p.bwd[i * 2 + 1] = static_cast<int16_t>(-p.fwd[i * 2 + 1] + static_cast<int>(rnd() % 64) - 32);
				p.cost[i] = static_cast<uint8_t>(rnd() % 60);
			}
			CameraLink::AheadCopy a;
			Assert::IsTrue(CameraLink::CopyAhead(p.View(3), 3, a) == CameraLink::AheadRefusal::None);
			std::vector<float> t;
			Assert::IsTrue(CameraLink::AheadCellTrust(a, 1.5f, t));
			const std::vector<float> ref = Mv5Trust(a, 1.5f);
			Assert::AreEqual(ref.size(), t.size());
			for (size_t i = 0; i < t.size(); ++i)
				Assert::AreEqual(ref[i], t[i], 0.0f, L"bit-equal to MV5's costWeight x fbError");
			// Monotonic. (a) Inconsistency: from a consistent pair (backward = -forward), any change of the backward vectors only raises the
			// cells' forward/backward error, so no cell gains trust and the changed ones lose it.
			// A uniform motion: every cell's centre lands on a cell with the same exact inverse, so the error starts at 0 everywhere.
			CameraLink::AheadCopy clean = a;
			for (size_t i = 0; i < clean.Cells(); ++i)
			{
				clean.forward[i * 2] = 37;
				clean.forward[i * 2 + 1] = -21;
				clean.backward[i * 2] = -37;
				clean.backward[i * 2 + 1] = 21;
			}
			std::vector<float> t0, t1, t2;
			Assert::IsTrue(CameraLink::AheadCellTrust(clean, 1.5f, t0));
			CameraLink::AheadCopy inconsistent = clean;
			for (size_t i = 0; i < inconsistent.backward.size(); i += 5)
				inconsistent.backward[i] = static_cast<int16_t>(inconsistent.backward[i] + 200);
			Assert::IsTrue(CameraLink::AheadCellTrust(inconsistent, 1.5f, t1));
			size_t lower = 0;
			for (size_t i = 0; i < t0.size(); ++i)
			{
				Assert::IsTrue(t1[i] <= t0[i] + 1e-6f, L"a higher inconsistency never gives a higher trust");
				lower += t1[i] < t0[i] ? 1u : 0u;
			}
			Assert::IsTrue(lower > 0, L"the inconsistent cells are trusted less");
			// (b) Cost: one cell made far less sure: its own trust never rises.
			CameraLink::AheadCopy costly = clean;
			const size_t c = costly.Cells() / 2;
			costly.forwardCost[c] = 255;
			Assert::IsTrue(CameraLink::AheadCellTrust(costly, 1.5f, t2));
			Assert::IsTrue(t2[c] <= t0[c] + 1e-6f, L"a higher cost never gives a higher trust");
			// The block trust is the mean of its cells' trust (cells whose centres lie in the block).
			CameraLink::FieldCopy f;
			Assert::IsTrue(CameraLink::AheadToSourceGrid(a, 1.5f, 40, 23, f));
			const float mean00 = (t[0] + t[1] + t[static_cast<size_t>(a.gridW)] + t[static_cast<size_t>(a.gridW) + 1]) / 4.0f;
			Assert::AreEqual(mean00, f.fwdTrust[0], 1e-6f);
		}

		TEST_METHOD(AheadIsAcceptedTheGuidedFieldIsStillRefused)
		{
			CameraLinkTest::Rig rig;
			FrameAnalysis::Store store;
			CameraLink::CameraLinkCore core(store);
			CameraLink::CameraLinkSettings s;
			s.p7Wanted = true;
			s.aheadFit = true;
			s.deterministic = true;   // no thread: the queue is inspected
			core.OnSettings(s);
			core.AttachGraph(rig.table.get(), rig.bytes.get(), rig.indicator.get());
			auto live = AheadField(3, 2.0);
			live->origin = CameraLink::FieldOrigin::Live;
			CameraLink::FramePhaseFacts f;
			Assert::IsFalse(core.SubmitField(live, f), L"the guided live field is never fitted");
			Assert::AreEqual(uint64_t(1), core.Counters().guidedRefused.load());
			Assert::IsFalse(core.SubmitAheadField(live), L"the ahead entry takes only an Ahead field");
			auto seed = AheadField(3, 2.0);
			seed->origin = CameraLink::FieldOrigin::Seed;
			Assert::IsFalse(core.SubmitAheadField(seed));
			Assert::IsTrue(core.SubmitField(seed, f), L"the seed path is unchanged");
			Assert::IsTrue(core.SubmitAheadField(AheadField(4, 2.0)), L"the ahead field is accepted");
			Assert::AreEqual(size_t(2), core.Worker()->Pending());
		}

		TEST_METHOD(AnAheadFitPublishesOnTimeWhenItsFrameIsNotSealedYet)
		{
			CameraLinkTest::Rig bridge, onTime;
			CameraLink::FitCounters counters;
			CameraLink::CameraRecordPool pool(32);
			CameraLink::CameraFitProducer producer(CameraModel::Config(), &counters);
			CameraLink::CameraFitEnv env = bridge.Env(&pool, &counters);
			env.slotBytes = nullptr;   // each lease writes its own table's bytes
			env.stampOf = nullptr;
			env.aheadMode = true;
			env.onTimeSource = SourceOf(onTime);
			std::vector<CameraLink::PublishOutcome> ends;
			env.onResult = [&ends](const CameraLink::CameraFitJob&, const CameraLink::CameraFitResult&, CameraLink::PublishOutcome o) { ends.push_back(o); };
			CameraLink::CameraFitWorker worker(producer, env);
			worker.Tick(1);   // frame 1 is rendering
			// Frame 2: the on-time target's inputs exist (fed after frame 1's render): published before frame 2's seal.
			onTime.PublishInputs(1);
			onTime.Retire(1);
			onTime.PublishInputs(2);
			CameraLink::CameraFitJob j = CameraLinkTest::PanJob(2, 4.0);
			j.ahead = true;
			Assert::IsTrue(worker.Submit(j));
			worker.ProcessQueued();
			Assert::AreEqual(size_t(1), ends.size());
			Assert::IsTrue(ends[0] == CameraLink::PublishOutcome::PublishedOnTime);
			Assert::AreEqual(uint64_t(1), counters.aheadOnTime.load());
			SealAt(onTime, 2);
			Assert::IsTrue(onTime.table->Find(VpSkeleton::kP7CameraFit)->StateOf(2) == VpSkeleton::SlotState::Sealed, L"sealed with its frame");
			Assert::IsTrue(bridge.table->Find(VpSkeleton::kP7CameraFit)->StateOf(2) == VpSkeleton::SlotState::Empty, L"the bridge is retired for it");
		}

		TEST_METHOD(AnAheadFitWaitsForTheTargetsInputsThenBridgesWhenItsFrameWasSealed)
		{
			CameraLinkTest::Rig bridge, onTime;
			CameraLink::FitCounters counters;
			CameraLink::CameraRecordPool pool(32);
			CameraLink::CameraFitProducer producer(CameraModel::Config(), &counters);
			CameraLink::CameraFitEnv env = bridge.Env(&pool, &counters);
			env.slotBytes = nullptr;
			env.stampOf = nullptr;
			env.aheadMode = true;
			env.onTimeSource = SourceOf(onTime);
			std::vector<CameraLink::PublishOutcome> ends;
			env.onResult = [&ends](const CameraLink::CameraFitJob&, const CameraLink::CameraFitResult&, CameraLink::PublishOutcome o) { ends.push_back(o); };
			CameraLink::CameraFitWorker worker(producer, env);
			worker.Tick(1);
			// Frames 2 and 3 fitted ahead; the target has no inputs for them yet: both parked (nothing lost, nothing late yet).
			for (uint64_t seq = 2; seq <= 3; ++seq)
			{
				CameraLink::CameraFitJob j = CameraLinkTest::PanJob(seq, 4.0);
				j.ahead = true;
				worker.Submit(j);
			}
			worker.ProcessQueued();
			Assert::AreEqual(size_t(0), ends.size());
			Assert::AreEqual(size_t(2), worker.Parked());
			// Frame 2's inputs arrive (after frame 1's render): frame 2 goes on time; frame 3 still waits.
			onTime.PublishInputs(1);
			onTime.Retire(1);
			onTime.PublishInputs(2);
			worker.Tick(1);
			worker.RetryParked();
			Assert::AreEqual(size_t(1), ends.size());
			Assert::IsTrue(ends[0] == CameraLink::PublishOutcome::PublishedOnTime);
			// Frame 3's seal ran before its inputs reached the target (the fit missed the lead): it is bridged, once the bridge has
			// frame 3's markers (frame 3 began).
			bridge.PublishInputs(1);
			bridge.Retire(1);
			bridge.PublishInputs(2);
			bridge.Retire(2);
			bridge.PublishInputs(3);
			worker.Tick(3);
			worker.RetryParked();
			Assert::AreEqual(size_t(2), ends.size());
			Assert::IsTrue(ends[1] == CameraLink::PublishOutcome::Published, L"bridged: published into the lag bridge");
			Assert::AreEqual(uint64_t(1), counters.aheadOnTime.load());
			Assert::AreEqual(uint64_t(1), counters.aheadBridged.load());
			Assert::AreEqual(size_t(0), worker.Parked());
			Assert::AreEqual(uint64_t(0), counters.late.load(), L"no on-time attempt counts as late");
		}

		TEST_METHOD(WithoutAnOnTimeTargetEveryAheadFitIsBridgedAtItsFrame)
		{
			CameraLinkTest::Rig bridge;
			CameraLink::FitCounters counters;
			CameraLink::CameraRecordPool pool(32);
			CameraLink::CameraFitProducer producer(CameraModel::Config(), &counters);
			CameraLink::CameraFitEnv env = bridge.Env(&pool, &counters);
			env.aheadMode = true;
			std::vector<CameraLink::PublishOutcome> ends;
			env.onResult = [&ends](const CameraLink::CameraFitJob&, const CameraLink::CameraFitResult&, CameraLink::PublishOutcome o) { ends.push_back(o); };
			CameraLink::CameraFitWorker worker(producer, env);
			worker.Tick(1);
			CameraLink::CameraFitJob j = CameraLinkTest::PanJob(2, 4.0);
			j.ahead = true;
			worker.Submit(j);
			worker.ProcessQueued();
			Assert::AreEqual(size_t(1), worker.Parked(), L"its markers do not exist before frame 2 begins");
			bridge.PublishInputs(1);
			bridge.Retire(1);
			bridge.PublishInputs(2);
			worker.Tick(2);
			worker.RetryParked();
			Assert::AreEqual(size_t(1), ends.size());
			Assert::IsTrue(ends[0] == CameraLink::PublishOutcome::Published);
			Assert::AreEqual(uint64_t(1), counters.aheadBridged.load());
		}

		TEST_METHOD(SeedJobsPublishAsBeforeInAheadMode)
		{
			CameraLinkTest::Rig bridge;
			CameraLink::FitCounters counters;
			CameraLink::CameraRecordPool pool(32);
			CameraLink::CameraFitProducer producer(CameraModel::Config(), &counters);
			CameraLink::CameraFitEnv env = bridge.Env(&pool, &counters);
			env.aheadMode = true;
			CameraLink::CameraFitWorker worker(producer, env);
			bridge.PublishInputs(1);
			worker.Submit(CameraLinkTest::PanJob(1, 4.0));   // not ahead: the bridge path, at once
			worker.ProcessQueued();
			Assert::AreEqual(uint64_t(1), counters.published.load());
			Assert::AreEqual(size_t(0), worker.Parked());
			Assert::AreEqual(uint64_t(0), counters.aheadBridged.load() + counters.aheadOnTime.load());
		}

		TEST_METHOD(TheOnTimeSealAcceptsRetiresTheBridgeAndAResyncIsRejectedAtTheSealAndAtUse)
		{
			CameraLinkTest::Rig bridge, onTime;
			FrameAnalysis::Store store;
			for (uint64_t seq = 1; seq <= 3; ++seq)
			{
				FrameAnalysis::Identity id;
				id.seq = seq;
				id.width = 64;
				id.height = 36;
				store.Begin(id, FrameAnalysis::kCamera);   // the frames the seal hands to the frame-analysis store
			}
			CameraLink::CameraLinkCore core(store);
			CameraLink::CameraLinkSettings s;
			s.p7Wanted = true;
			s.aheadFit = true;
			core.OnSettings(s);
			core.SetAheadSource([](uint64_t, CameraLink::AheadCopy&) { return CameraLink::AheadRefusal::NoPair; }, CameraLink::PairFactsProvider());
			core.AttachOnTime(SourceOf(onTime), "test lane");
			core.AttachGraph(bridge.table.get(), bridge.bytes.get(), bridge.indicator.get());
			Assert::IsTrue(core.AheadActive() && core.HasThread());
			CameraLink::FramePhaseFacts f;
			f.seq = f.renderSeq = 1;
			core.OnFrameStart(f);   // frame 1 renders
			onTime.PublishInputs(1);
			onTime.Retire(1);
			onTime.PublishInputs(2);
			// Frame 2: fitted ahead, published on time, sealed at 2.
			Assert::IsTrue(core.SubmitAheadField(AheadField(2, 4.0)));
			core.Worker()->WaitIdle();
			Assert::AreEqual(uint64_t(1), core.Counters().aheadOnTime.load());
			SealAt(onTime, 2);
			f.seq = f.renderSeq = 2;
			core.OnFrameStart(f);
			core.Worker()->WaitIdle();
			Assert::AreEqual(uint64_t(1), core.Counters().onTimeSealed.load());
			Assert::IsTrue(core.SealedRecord(2) != nullptr, L"served from its own frame on");
			// Frame 3: published on time, then a resync reaches frame 3 before its seal: the seal rejects it, nothing is served.
			onTime.PublishInputs(3);
			Assert::IsTrue(core.SubmitAheadField(AheadField(3, 4.0)));
			core.Worker()->WaitIdle();
			Assert::AreEqual(uint64_t(2), core.Counters().aheadOnTime.load());
			onTime.indicator->Advance(VpIndicator::kContinuity, VpResync::Cause::MissedFrames, 3);
			SealAt(onTime, 3);
			Assert::IsFalse(onTime.table->Find(VpSkeleton::kP7CameraFit)->StateOf(3) == VpSkeleton::SlotState::Sealed);
			CameraLink::FramePhaseFacts f3;
			f3.seq = f3.renderSeq = 3;
			core.OnFrameStart(f3);
			Assert::AreEqual(uint64_t(1), core.Counters().onTimeRejected.load());
			Assert::IsTrue(core.SealedRecord(3) == nullptr && core.CausalRecord(3) == nullptr, L"a rejected record is never served");
			// The lag bridge later seals frames 2 and 3 without a P7 of its own: not a second verdict, not late.
			const uint64_t lateBefore = core.Counters().sealedLate.load();
			for (uint64_t seq = 2; seq <= 3; ++seq)
			{
				CameraLink::FramePhaseFacts lag;
				lag.seq = seq;
				lag.renderSeq = seq + 6;
				lag.p7SealedPublished = false;
				core.OnFrameStart(lag);
			}
			Assert::AreEqual(lateBefore, core.Counters().sealedLate.load(), L"the bridge is retired for the frames the on-time seal decided");
			Assert::IsTrue(core.SealedRecord(2) != nullptr, L"still served");
			// At use: an event that reaches frame 2 after its seal (in the on-time lane's Indicator) refuses the record.
			onTime.indicator->Advance(VpIndicator::kContinuity, VpResync::Cause::MissedFrames, 2);
			Assert::IsTrue(core.SealedRecord(2) == nullptr, L"re-checked at consumption");
			Assert::AreEqual(uint64_t(1), core.Counters().staleAtUse.load());
			// G-A2: with a usable target, a late frame is informational (a line), never an engaged entry.
			Assert::IsTrue(core.OnTimeUsable());
			for (const FallbackCatalog::Decision& d : core.CollectDecisions())
				Assert::IsFalse(d.id == CameraLink::kIdAheadNoTarget && d.engaged, L"no defect while the target exists");
			std::vector<std::string> lines;
			core.CollectStatus(lines);
			Assert::IsTrue(Has(lines, "frames late (bridged)"), L"the informational late line");
			Assert::IsTrue(Has(lines, "on-time target: test lane"));
		}

		// G-A3: P7's P2 provenance is the FlowOf pair + the vectors-valid epoch it was served under; another epoch at the claim, at the
		// on-time seal or at a consumer read refuses the record, counted and shown. P3 / P2 are no lane ancestors of P7.
		TEST_METHOD(TheP2ProvenanceIsCheckedAtClaimSealAndUse)
		{
			const VpSkeleton::Registry reg = VpSkeleton::SkeletonProducts();
			const VpSkeleton::ProductDecl& p7 = reg.Get(VpSkeleton::kP7CameraFit);
			Assert::AreEqual(1, int(p7.inputCount), L"P4 only");
			CameraLinkTest::Rig bridge, onTime;
			FrameAnalysis::Store store;
			for (uint64_t seq = 1; seq <= 4; ++seq)
			{
				FrameAnalysis::Identity id;
				id.seq = seq;
				id.width = 64;
				id.height = 36;
				store.Begin(id, FrameAnalysis::kCamera);
			}
			CameraLink::CameraLinkCore core(store);
			CameraLink::CameraLinkSettings s;
			s.p7Wanted = true;
			s.aheadFit = true;
			core.OnSettings(s);
			std::atomic<uint64_t> epoch{ 5 };
			core.SetAheadSource([](uint64_t, CameraLink::AheadCopy&) { return CameraLink::AheadRefusal::NoPair; }, CameraLink::PairFactsProvider());
			core.SetP2EpochSource([&epoch]() { return epoch.load(); });
			core.AttachOnTime(SourceOf(onTime), "test lane");
			core.AttachGraph(bridge.table.get(), bridge.bytes.get(), bridge.indicator.get());
			CameraLink::FramePhaseFacts f;
			f.seq = f.renderSeq = 1;
			core.OnFrameStart(f);
			onTime.PublishInputs(1);
			onTime.Retire(1);
			onTime.PublishInputs(2);
			onTime.PublishInputs(3);
			auto field = [](uint64_t seq, uint64_t ep) {
				auto c = AheadField(seq, 4.0);
				c->haveP2 = true;
				c->p2Epoch = ep;
				return c;
			};
			// At the claim: served under epoch 4, the engine is at 5: refused, never published anywhere.
			Assert::IsTrue(core.SubmitAheadField(field(2, 4)));
			core.Worker()->WaitIdle();
			Assert::AreEqual(uint64_t(1), core.Counters().p2RefusedClaim.load());
			Assert::AreEqual(uint64_t(0), core.Counters().aheadOnTime.load());
			// Frame 3 under the current epoch: on time; then the epoch moves before its seal: rejected at the seal.
			Assert::IsTrue(core.SubmitAheadField(field(3, 5)));
			core.Worker()->WaitIdle();
			Assert::AreEqual(uint64_t(1), core.Counters().aheadOnTime.load());
			SealAt(onTime, 2);
			SealAt(onTime, 3);
			epoch.store(6);
			f.seq = f.renderSeq = 3;
			core.OnFrameStart(f);
			Assert::AreEqual(uint64_t(1), core.Counters().p2RefusedSeal.load());
			Assert::IsTrue(core.SealedRecord(3) == nullptr);
			std::vector<std::string> lines;
			core.CollectStatus(lines);
			Assert::IsTrue(Has(lines, "P2 provenance refused at claim / seal / use 1 / 1 / 0"), L"shown");
		}

		TEST_METHOD(TheP2ProvenanceIsCheckedAtAConsumerRead)
		{
			CameraLinkTest::Rig bridge, onTime;
			FrameAnalysis::Store store;
			for (uint64_t seq = 1; seq <= 3; ++seq)
			{
				FrameAnalysis::Identity id;
				id.seq = seq;
				id.width = 64;
				id.height = 36;
				store.Begin(id, FrameAnalysis::kCamera);
			}
			CameraLink::CameraLinkCore core(store);
			CameraLink::CameraLinkSettings s;
			s.p7Wanted = true;
			s.aheadFit = true;
			core.OnSettings(s);
			std::atomic<uint64_t> epoch{ 5 };
			core.SetAheadSource([](uint64_t, CameraLink::AheadCopy&) { return CameraLink::AheadRefusal::NoPair; }, CameraLink::PairFactsProvider());
			core.SetP2EpochSource([&epoch]() { return epoch.load(); });
			core.AttachOnTime(SourceOf(onTime), "test lane");
			core.AttachGraph(bridge.table.get(), bridge.bytes.get(), bridge.indicator.get());
			CameraLink::FramePhaseFacts f;
			f.seq = f.renderSeq = 1;
			core.OnFrameStart(f);
			onTime.PublishInputs(1);
			onTime.Retire(1);
			onTime.PublishInputs(2);
			auto c = AheadField(2, 4.0);
			c->haveP2 = true;
			c->p2Epoch = 5;
			Assert::IsTrue(core.SubmitAheadField(c));
			core.Worker()->WaitIdle();
			SealAt(onTime, 2);
			f.seq = f.renderSeq = 2;
			core.OnFrameStart(f);
			Assert::IsTrue(core.SealedRecord(2) != nullptr, L"sealed on time under the current epoch");
			epoch.store(6);
			Assert::IsTrue(core.SealedRecord(2) == nullptr, L"refused at use once the epoch moved");
			Assert::AreEqual(uint64_t(1), core.Counters().p2RefusedUse.load());
		}

		TEST_METHOD(TheAheadPollTakesPairsInOrderAndStopsAtTheFirstMissing)
		{
			CameraLinkTest::Rig bridge;
			FrameAnalysis::Store store;
			CameraLink::CameraLinkCore core(store);
			CameraLink::CameraLinkSettings s;
			s.p7Wanted = true;
			s.aheadFit = true;
			core.OnSettings(s);
			Pair p(960, 540, 2.0);
			p.Uniform(2.0, 0.0);
			std::vector<uint64_t> asked;
			core.SetAheadSource([&p, &asked](uint64_t seq, CameraLink::AheadCopy& out) {
				asked.push_back(seq);
				return seq <= 12 ? CameraLink::CopyAhead(p.View(seq), seq, out) : CameraLink::AheadRefusal::NoPair;
			}, CameraLink::PairFactsProvider());
			core.SetAheadGrid(120, 68);
			core.AttachGraph(bridge.table.get(), bridge.bytes.get(), bridge.indicator.get());
			CameraLink::FramePhaseFacts f;
			f.seq = f.renderSeq = 9;
			core.OnFrameStart(f);
			core.Worker()->WaitIdle();
			Assert::AreEqual(uint64_t(3), core.Counters().aheadSubmitted.load(), L"10, 11, 12: computed ahead");
			Assert::AreEqual(uint64_t(13), core.AheadNext());
			Assert::AreEqual(uint64_t(10), asked.front());
			Assert::AreEqual(uint64_t(13), asked.back(), L"stops at the first frame without a pair");
			std::vector<std::string> lines;
			core.CollectStatus(lines);
			Assert::IsTrue(Has(lines, "camera ahead fit:"), L"the counters are shown");
			Assert::IsTrue(Has(lines, "on-time target: none"), L"no on-time target is said so");
			bool engaged = false;
			for (const FallbackCatalog::Decision& d : core.CollectDecisions())
				engaged = engaged || (d.id == CameraLink::kIdAheadNoTarget && d.engaged);
			Assert::IsTrue(engaged, L"bridging is a visible fallback");
		}

		TEST_METHOD(DeterministicModeTakesTheBridgePathOnly)
		{
			CameraLinkTest::Rig bridge;
			FrameAnalysis::Store store;
			CameraLink::CameraLinkCore core(store);
			CameraLink::CameraLinkSettings s;
			s.p7Wanted = true;
			s.aheadFit = true;
			s.deterministic = true;
			core.OnSettings(s);
			int calls = 0;
			core.SetAheadSource([&calls](uint64_t, CameraLink::AheadCopy&) { ++calls; return CameraLink::AheadRefusal::NoPair; },
				CameraLink::PairFactsProvider());
			core.AttachGraph(bridge.table.get(), bridge.bytes.get(), bridge.indicator.get());
			Assert::IsFalse(core.AheadActive());
			Assert::IsFalse(core.HasThread());
			CameraLink::FramePhaseFacts f;
			for (uint64_t seq = 1; seq <= 5; ++seq)
			{
				f.seq = f.renderSeq = seq;
				core.OnFrameStart(f);
				core.OnPostSubmitGap();
			}
			Assert::AreEqual(0, calls, L"the ahead pair is never asked for");
			std::vector<std::string> lines;
			core.CollectStatus(lines);
			Assert::IsTrue(Has(lines, CameraLink::kAheadDeterministicText));
			for (const FallbackCatalog::Decision& d : core.CollectDecisions())
				Assert::IsFalse(d.id == CameraLink::kIdAheadNoTarget);
		}

		TEST_METHOD(OffIsInert)
		{
			CameraLinkTest::Rig bridge;
			FrameAnalysis::Store store;
			CameraLink::CameraLinkCore core(store);
			CameraLink::CameraLinkSettings s;
			s.p7Wanted = true;   // camera on, ahead fit off
			core.OnSettings(s);
			int calls = 0;
			core.SetAheadSource([&calls](uint64_t, CameraLink::AheadCopy&) { ++calls; return CameraLink::AheadRefusal::NoPair; },
				CameraLink::PairFactsProvider());
			core.AttachGraph(bridge.table.get(), bridge.bytes.get(), bridge.indicator.get());
			Assert::IsFalse(core.AheadActive());
			CameraLink::FramePhaseFacts f;
			for (uint64_t seq = 1; seq <= 5; ++seq)
			{
				f.seq = seq;
				core.OnFrameStart(f);
				core.OnPostSubmitGap();
			}
			Assert::AreEqual(0, calls);
			Assert::AreEqual(size_t(7), core.CollectDecisions().size(), L"the decisions are exactly the camera's seven");
			std::vector<std::string> lines;
			core.CollectStatus(lines);
			Assert::IsFalse(Has(lines, "ahead"));
			Assert::IsTrue(core.Pool()->Ring() < 32, L"the pool is not enlarged (only the ahead fit needs the longer record life)");
		}

		TEST_METHOD(TheAheadKeyNeedsTheModelAndTheOriginalPathwayForcesItOff)
		{
			char dir[MAX_PATH] = {};
			GetTempPathA(MAX_PATH, dir);
			const std::string path = std::string(dir) + "vp-camera-ahead-" + std::to_string(GetCurrentProcessId()) + ".cfg";
			{
				std::ofstream out(path, std::ios::binary | std::ios::trunc);
				out << "[vpv.camera]\ncamera_ahead_fit: on\n";
			}
			Vpv::Camera::CameraSettings c = Vpv::Camera::Load([path](ConfigFile& f) { return f.Load(path, ConfigFile::ReadPolicy::Fresh); });
			std::remove(path.c_str());
			Assert::IsTrue(c.aheadFit && c.problems.empty() && c.unknownKeys.empty());
			Assert::IsFalse(c.AheadFit(), L"inert while camera_model is off");
			Assert::IsTrue(c.AnySet() && c.Wanted(), L"shown as set");
			c.model = true;
			Assert::IsTrue(c.AheadFit());
			c.originalPathway = true;
			Assert::IsFalse(c.Effective().AheadFit() || c.Effective().aheadFit);
			Assert::IsTrue(Vpv::Camera::Validate("camera_ahead_fit", "maybe") == Vpv::Camera::Verdict::Invalid);
			Assert::IsTrue(Vpv::Camera::Validate("camera_ahead_fit", "off") == Vpv::Camera::Verdict::Valid);
			Vpv::Camera::CameraSettings d;
			Assert::IsFalse(d.aheadFit, L"default off");
			Assert::IsFalse(c == d);
		}
	};
}
