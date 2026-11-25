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
//  Vectors are in SOURCE pixels of the frame the producer saw (the engine's grid cell is 4 pixels of the producer's input, scaled per-axis by
//  sourceWidth / flow.halfW horizontally and sourceHeight / flow.halfH vertically); the host scales them to the re-based plane as for any source (the module never rescales).
//  Trust per cell is the ENGINE's memo (FlowView::CellTrust / CellTrustBackward, computed once per direction and tolerance while the pair is the engine's current FlowOf lease;
//  this adapter holds no trust arithmetic, L-B6 WAIVER-B6-TD-TRUST). Both are in [0, 1]; a pair without a trust plane is not guessed at (absent:
//  "the pair has no vectors or costs"). Camera() stays false (the camera model reaches TD through its own adapter).
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
#include <VP-V/modules/motion/estimation/VectorEngineInterface.h>
#include <VP-V/modules/motion/estimation/VectorProducts.h>

#include <algorithm>
#include <cstdint>
#include <string>
#include <functional>
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
		};

		// view: the engine's P2 view. validity: its vectors-valid state (null = trust the slot). The source size is needed to put the
		// engine's cells into source pixels (the producer may have worked at half again).
		// flowOf: the engine's FlowOf (render thread), the source of the per-cell trust memo (null = no trust, every pair absent).
		using FlowOfFn = std::function<bool(uint64_t seq, VpMotion::FlowView& out)>;
		SharedMotionSource(VpMotion::P2View view, const MotionValidity::State* validity, FlowOfFn flowOf = nullptr)
			: m_view(std::move(view)), m_validity(validity), m_flowOf(std::move(flowOf)) {}
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
			if (m_validity && !m_validity->PairServes(from, to, m_validity->EpochOfLastKept(), m_validity->Epoch()))   // W1-C2a A1 / S2
				return Absent(m_stats.stale, "the vectors-valid state moved (a resync, a gap or a settings change)");
			uint64_t pairGeneration = 0;
			const MotionAhead::FlowPtr flow = m_view.Find(from, to, pairGeneration);
			// C2a-F 2: a pair the slot still holds from another stream generation (before a resync / seek / queue reset / format
			// change, not yet re-keyed by the producer) is never served.
			if (flow && m_validity && !m_validity->PairGenerationCurrent(pairGeneration))
				return Absent(m_stats.stale, "the vectors-valid state moved (a resync, a gap or a settings change)");
			if (!flow)
				return Absent(m_stats.absent, m_view.Bound() ? "no pair computed ahead for these frames" : "no vector engine");
			if (m_srcW <= 0 || m_srcH <= 0 || flow->halfW <= 0 || flow->halfH <= 0 || flow->gridW <= 0 || flow->gridH <= 0)
				return Absent(m_stats.unfit, "the source size is not known");
			const std::vector<int16_t>& vec = forward ? flow->forward : flow->backward;
			const std::vector<uint8_t>& cost = forward ? flow->forwardCost : flow->backwardCost;
			const size_t cells = static_cast<size_t>(flow->gridW) * flow->gridH;
			const std::vector<int16_t>& other = forward ? flow->backward : flow->forward;
			if (vec.size() < cells * 2 || cost.size() < cells || other.size() < cells * 2)
				return Absent(m_stats.unfit, "the pair has no vectors or costs");
			// L-B6 (WAIVER-B6-TD-TRUST): the engine's ONE per-cell trust for this direction, READ from the engine memo (FlowView::CellTrust /
			// CellTrustBackward: computed once per direction and tolerance for as long as the pair is the engine's current lease; another pair's FlowOf recomputes), never worked out here. No engine handle, or
			// a memo without the plane: absent (never guessed). Copied here, so the pair outlives the engine's next FlowOf.
			VpMotion::FlowView view;
			if (!m_flowOf || !m_flowOf(seq, view))
				return Absent(m_stats.unfit, "the engine has no trust for this pair");
			const float* trustPlane = forward ? view.CellTrust(m_consistencyPx) : view.CellTrustBackward(m_consistencyPx);
			if (!trustPlane || view.gridW != flow->gridW || view.gridH != flow->gridH)
				return Absent(m_stats.unfit, "the pair has no vectors or costs");
			const double scaleX = static_cast<double>(m_srcW) / flow->halfW;   // horizontal producer pixels -> source pixels
			const double scaleY = static_cast<double>(m_srcH) / flow->halfH;   // vertical producer pixels -> source pixels (GEM-110, matches FlowView::enginePxToSourceY)
			MotionPair pair;
			pair.valid = true;
			pair.gridW = flow->gridW;
			pair.gridH = flow->gridH;
			pair.blockPx = 4.0 * scaleX;   // the engine's cell is 4 producer pixels
			pair.vx.resize(cells);
			pair.vy.resize(cells);
			pair.trust.resize(cells);
			for (size_t i = 0; i < cells; ++i)
			{
				pair.vx[i] = static_cast<float>(vec[i * 2] / 32.0 * scaleX);       // S10.5 producer pixels -> source pixels
				pair.vy[i] = static_cast<float>(vec[i * 2 + 1] / 32.0 * scaleY);
				pair.trust[i] = trustPlane[i];
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
		FlowOfFn m_flowOf;
		const MotionValidity::State* m_validity = nullptr;
		int m_srcW = 0, m_srcH = 0;
		float m_consistencyPx = DefaultConsistencyPixels();   // the live dejudder_consistency_px default; the wiring sets its matcher's value
		mutable Stats m_stats;
		mutable VpMotion::NoPairTracker m_tracker;
		int m_depth = 0;
		mutable std::string m_lastWhy;
	};
}
