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
//  Trust is 1 - cost / 255 per cell (the engine's own confidence). Camera() stays false (the camera model reaches TD through its own
//  adapter).
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
#include <VP-V/modules/motion/estimation/VectorProducts.h>

#include <algorithm>
#include <cstdint>
#include <string>

namespace TemporalDetail
{
	class SharedMotionSource : public IMotionSource
	{
	public:
		static constexpr unsigned kAbsentRun = 8;   // consecutive absent answers that make the visible entry (a refill is fewer)

		struct Stats
		{
			uint64_t hits = 0, absent = 0, stale = 0, unfit = 0;   // absent = not in the slot; stale = epoch moved; unfit = size unknown / mismatched
		};

		// view: the engine's P2 view. validity: its vectors-valid state (null = trust the slot). The source size is needed to put the
		// engine's cells into source pixels (the producer may have worked at half again).
		SharedMotionSource(VpMotion::P2View view, const MotionValidity::State* validity) : m_view(std::move(view)), m_validity(validity) {}
		void SetSourceSize(int width, int height) { m_srcW = width; m_srcH = height; }

		bool Forward(uint64_t seq, MotionPair& out) const override { return Take(seq, true, out); }
		bool Backward(uint64_t seq, MotionPair& out) const override { return Take(seq, false, out); }

		const Stats& GetStats() const { return m_stats; }
		// Visible state: engaged while the last kAbsentRun answers were all absent.
		FallbackCatalog::Decision CurrentDecision() const
		{
			return FallbackCatalog::TdMotionSource(m_run >= kAbsentRun, m_lastWhy);
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
				pair.trust[i] = static_cast<float>(1.0 - cost[i] / 255.0);
			}
			out = std::move(pair);
			++m_stats.hits;
			m_run = 0;
			return true;
		}

		bool Absent(uint64_t& counter, const char* why) const
		{
			++counter;
			++m_run;
			m_lastWhy = why;
			return false;
		}

		VpMotion::P2View m_view;
		const MotionValidity::State* m_validity = nullptr;
		int m_srcW = 0, m_srcH = 0;
		mutable Stats m_stats;
		mutable unsigned m_run = 0;
		mutable std::string m_lastWhy;
	};
}
