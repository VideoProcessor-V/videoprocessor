// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// SK-MV MV5: TemporalDetail's motion source on the SHARED vector products. A VP-side adapter implementing TemporalDetail::IMotionSource
// (TemporalDetailMotion.h) over the vector engine's P2 view (VectorProducts.h: the NVOF pairs the one producer published), so TD reads
// the same pair the de-judder and the camera fit read instead of a motion pass of its own. Nothing in TemporalDetail changes: it still sees
// IMotionSource; the host (when TD is wired) hands it this object in place of a FieldMotionSource it would have to feed.
//
//  * Forward(seq)  = pair (seq - 1 -> seq) on frame seq - 1's grid: the pair's forward vectors.
//  * Backward(seq) = pair (seq -> seq - 1) on frame seq's grid: the pair's backward vectors.
//  Vectors are in SOURCE pixels of the frame the producer saw (the engine's grid cell is 4 pixels of the producer's input, which is the
//  source scaled by sourceWidth / flow.halfW); the host scales them to the re-based plane as for any source (the module never rescales).
//  Trust per cell is the de-judder's own engine-trust weighting (OpticalFlowTrust), not a raw cost scale: the cell's block cost weight
//  (Flow::costWeight for the forward direction, the same CostWeights maths on the backward costs; cost judged against the pair's own median and
//  spread) times the engine's forward/backward consistency weight (Flow::fbError, FallOff from the matcher's consistency limit to twice it;
//  the backward direction swaps the two fields). Both are in [0, 1], 1 = as sure as the engine usually is; a pair without them is not guessed
//  at (absent: "the pair has no vectors or costs"). Camera() stays false (the camera model reaches TD through its own adapter).
//
// ABSENT is visible: a pair that is not in the slot, is stale (the vectors-valid state moved: MotionValidity::PairValid), or does not fit
// the source size answers false (TD takes its causal substitute) and is counted by kind; a sustained run raises the "td motion" entry
// (FallbackCatalog::TdMotionSource). The P3 GPU field has no CPU form (IVectorEngine::FlowIsCpuResident is false), so this adapter reads P2
// only; where there is no P2 (no producer) TD is absent, never silently fed from a different source.
//
// Header only, C++14. Render thread (the same thread that publishes and prunes).
#pragma once

#include "TemporalDetailMotion.h"

#include <VP-V/core/FallbackCatalog.h>
#include <VP-V/modules/motion/estimation/MotionAheadProducer.h>
#include <VP-V/modules/motion/estimation/MotionValidity.h>
#include <VP-V/modules/motion/estimation/OpticalFlowTrust.h>
#include <VP-V/modules/motion/estimation/VectorProducts.h>

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace TemporalDetail
{
	class SharedMotionSource : public IMotionSource
	{
	public:
		// Consecutive absent answers that make the visible entry: derived as the "motion" entry's NoPairTracker does - the look-ahead depth at the
		// measured rate plus a margin, never fewer than its floor (a refill takes about the depth) - not a constant of its own.
		static unsigned AbsentRun(int depthFrames) { return VpMotion::NoPairTracker::Threshold(depthFrames); }

		struct Stats
		{
			uint64_t hits = 0, absent = 0, stale = 0, unfit = 0;   // absent = not in the slot; stale = epoch moved; unfit = size unknown / mismatched
			uint64_t fbErrorComputed = 0;   // W1-C3: FbError worked out here (the backward direction, or a pair without the producer's extras)
		};

		// view: the engine's P2 view. validity: its vectors-valid state (null = trust the slot). The source size is needed to put the
		// engine's cells into source pixels (the producer may have worked at half again).
		SharedMotionSource(VpMotion::P2View view, const MotionValidity::State* validity) : m_view(std::move(view)), m_validity(validity) {}
		void SetSourceSize(int width, int height) { m_srcW = width; m_srcH = height; }
		// The measured look-ahead depth in frames (VectorEngineState::motionAheadConfigured, the number the "motion" entry uses): the visible
		// entry's threshold follows it. 0 (the default) = the tracker's floor.
		void SetDepth(int depthFrames) { m_depth = depthFrames; }
		// The matcher's forward/backward consistency limit in producer (half-size) pixels (Dejudder::MotionWarp::ConsistencyPixels): the host
		// hands over the value its matcher uses, so TD judges the engine's consistency exactly as the de-judder does. The default is the live
		// dejudder_consistency_px default (2.0, VpvRendererFields.h) - NOT Dejudder::Settings::consistencyPixels' object default of 1.0 - and
		// the wiring MUST set it from warp.ConsistencyPixels() (which also scales by height above the reference).
		static float DefaultConsistencyPixels() { return 2.0f; }
		void SetConsistencyPixels(float pixels) { if (pixels > 0.0f) m_consistencyPx = pixels; }
		float ConsistencyPixels() const { return m_consistencyPx; }

		bool Forward(uint64_t seq, MotionPair& out) const override { return Take(seq, true, out); }
		bool Backward(uint64_t seq, MotionPair& out) const override { return Take(seq, false, out); }

		const Stats& GetStats() const { return m_stats; }
		// Visible state: engaged while the absent answers are SUSTAINED (a run of AbsentRun(depth), or at least half of the last window of
		// four such runs - the NoPairTracker rule, so a partial miss ratio cannot hide under a run counter that keeps resetting).
		FallbackCatalog::Decision CurrentDecision() const
		{
			return FallbackCatalog::TdMotionSource(m_tracker.Sustained(m_depth), m_lastWhy);
		}

	private:
		bool Take(uint64_t seq, bool forward, MotionPair& out) const
		{
			if (seq == 0)
				return Absent(m_stats.absent, "no previous frame");
			const uint64_t from = seq - 1, to = seq;
			if (m_validity && !m_validity->PairValid(from, to, m_validity->EpochOfLastKept(), m_validity->Epoch()))
				return Absent(m_stats.stale, "the vectors-valid state moved (a resync, a gap or a settings change)");
			const MotionAhead::FlowPtr flow = m_view.Find(from, to);
			if (!flow)
				return Absent(m_stats.absent, m_view.Bound() ? "no pair computed ahead for these frames" : "no vector engine");
			if (m_srcW <= 0 || m_srcH <= 0 || flow->halfW <= 0 || flow->halfH <= 0 || flow->gridW <= 0 || flow->gridH <= 0)
				return Absent(m_stats.unfit, "the source size is not known");
			const std::vector<int16_t>& vec = forward ? flow->forward : flow->backward;
			const std::vector<uint8_t>& cost = forward ? flow->forwardCost : flow->backwardCost;
			const size_t cells = static_cast<size_t>(flow->gridW) * flow->gridH;
			if (vec.size() < cells * 2 || cost.size() < cells)
				return Absent(m_stats.unfit, "the pair has no vectors or costs");
			// The de-judder's trust weights for this direction (OpticalFlowTrust), per cell.
			std::vector<float> cellErrorLocal, blockCost;
			const std::vector<int16_t>& other = forward ? flow->backward : flow->forward;
			if (other.size() < cells * 2)
				return Absent(m_stats.unfit, "the pair has no vectors or costs");
			// W1-C3: the forward direction's error is the producer's Flow::fbError (FbError(forward, backward), worked out once on its
			// worker); the backward direction (fields swapped) has no producer extra and is computed here, as before.
			const std::vector<float>* cellError = &cellErrorLocal;
			if (forward && flow->fbError.size() == cells)
				cellError = &flow->fbError;
			else
			{
				++m_stats.fbErrorComputed;
				if (!OpticalFlowTrust::FbError(vec, other, flow->gridW, flow->gridH, NvOpticalFlow::Engine::Grid, cellErrorLocal))
					return Absent(m_stats.unfit, "the pair has no vectors or costs");
			}
			const int blocksX = (flow->halfW + MotionAhead::Flow::kBlockPx - 1) / MotionAhead::Flow::kBlockPx;
			const int blocksY = (flow->halfH + MotionAhead::Flow::kBlockPx - 1) / MotionAhead::Flow::kBlockPx;
			const size_t blocks = static_cast<size_t>(blocksX) * blocksY;
			const std::vector<float>* costBlocks = nullptr;
			if (forward && flow->blocksX == blocksX && flow->blocksY == blocksY && flow->costWeight.size() == blocks)
				costBlocks = &flow->costWeight;   // the producer already worked it out
			else if (OpticalFlowTrust::CostWeights(cost, flow->gridW, flow->gridH, NvOpticalFlow::Engine::Grid, MotionAhead::Flow::kBlockPx,
					blocksX, blocksY, blockCost))
				costBlocks = &blockCost;
			if (!costBlocks)
				return Absent(m_stats.unfit, "the pair has no vectors or costs");
			// The engine's ONE per-cell trust (OpticalFlowTrust::CellTrust: the camera ahead fit calls the same function).
			std::vector<float> cellTrust;
			if (!OpticalFlowTrust::CellTrust(*costBlocks, blocksX, blocksY, *cellError, flow->gridW, flow->gridH, NvOpticalFlow::Engine::Grid,
					MotionAhead::Flow::kBlockPx, m_consistencyPx, cellTrust))
				return Absent(m_stats.unfit, "the pair has no vectors or costs");
			const double scale = static_cast<double>(m_srcW) / flow->halfW;   // producer pixels -> source pixels
			MotionPair pair;
			pair.valid = true;
			pair.gridW = flow->gridW;
			pair.gridH = flow->gridH;
			pair.blockPx = 4.0 * scale;   // the engine's cell is 4 producer pixels
			pair.vx.resize(cells);
			pair.vy.resize(cells);
			pair.trust.resize(cells);
			for (size_t i = 0; i < cells; ++i)
			{
				pair.vx[i] = static_cast<float>(vec[i * 2] / 32.0 * scale);       // S10.5 producer pixels -> source pixels
				pair.vy[i] = static_cast<float>(vec[i * 2 + 1] / 32.0 * scale);
				pair.trust[i] = cellTrust[i];
			}
			out = std::move(pair);
			++m_stats.hits;
			m_tracker.Note(false, m_depth);
			return true;
		}

		bool Absent(uint64_t& counter, const char* why) const
		{
			++counter;
			m_tracker.Note(true, m_depth);
			m_lastWhy = why;
			return false;
		}

		VpMotion::P2View m_view;
		const MotionValidity::State* m_validity = nullptr;
		int m_srcW = 0, m_srcH = 0;
		float m_consistencyPx = DefaultConsistencyPixels();   // the live dejudder_consistency_px default; the wiring sets its matcher's value
		mutable Stats m_stats;
		mutable VpMotion::NoPairTracker m_tracker;
		int m_depth = 0;
		mutable std::string m_lastWhy;
	};
}
