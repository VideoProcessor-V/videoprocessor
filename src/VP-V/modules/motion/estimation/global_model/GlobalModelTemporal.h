// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// Global camera model: temporal filter (prediction + consistency only, never used for warps) and
// the fixed-lag smoother (queued modes). Pure C++14, deterministic.
#pragma once

#include <cstdint>
#include <deque>

#include "CameraMotion.h"

namespace CameraModel
{
	// Constant-velocity filter on the similarity rates (logScale, angle, tx, ty), all per second.
	// State per component: value and rate of change per second. Process noise grows with the
	// measured motion (relative terms, no pixel constants). Used ONLY for H0 prediction and the
	// temporal consistency signal.
	class TemporalFilter
	{
	public:
		void Reset();
		bool Valid() const { return m_valid; }
		// Predicted rates for the next pair (dt = seconds since the last update). False if no state.
		bool Predict(double dtSeconds, SimilarityRates& out) const;
		// 0..1: how well `measured` agrees with the prediction (1 = identical; falls off relative to
		// the motion magnitude and the filter's own variance). Returns 0.5 (neutral) when no state.
		double Consistency(double dtSeconds, const SimilarityRates& measured) const;
		// Fold in a measured set of rates; dtSeconds = time since the previous update (<= 0 on the
		// first update is allowed). Updates the state deterministically.
		void Update(double dtSeconds, const SimilarityRates& measured);

	private:
		bool m_valid = false;
		double m_value[4] = { 0, 0, 0, 0 };
		double m_rate[4] = { 0, 0, 0, 0 };
		double m_var[4] = { 1, 1, 1, 1 };
	};

	// Symmetric weighted window of +-lag pairs over time-normalised rates (similarity) or
	// picture-corner displacements (affine/homography), producing smoothed Params for a record.
	// Never spans a cut, a generation change or a flash frame (those records break the window:
	// neighbours on the other side are ignored). lag 0 -> returns the record's own params.
	class FixedLagSmoother
	{
	public:
		explicit FixedLagSmoother(int lag = 0) : m_lag(lag < 0 ? 0 : lag) {}
		int Lag() const { return m_lag; }
		void Reset();
		// Records must be pushed in increasing seq. Keeps 2*lag+1 plus margin.
		void Push(const CameraMotion& record);
		// Smoothed params for `seq` (pair seq-1 -> seq model rescaled to that record's own
		// pairSeconds). True only when the record is known, valid, and ALL records seq-lag..seq+lag
		// that belong to the same segment are present (a window cut short by a cut / generation /
		// flash boundary uses the available same-segment neighbours; a window cut short by the
		// queue end (neighbours not pushed yet) returns false so the caller uses the current fit).
		bool Smoothed(uint64_t seq, Params& out) const;

	private:
		int m_lag;
		std::deque<CameraMotion> m_records;   // copies without block data
	};
}
