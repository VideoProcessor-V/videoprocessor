// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// SK-MV hiccup gap seed (owner decision 2026-11-08): when a capture hiccup drops frames, the de-judder's fill-pair estimate (A -> B with
// B more than one source period after A, DejudderPolicy fill pairs) normally searches from nothing. If a SHARED pair ended exactly at A
// before the gap (the producer's P2 pair, with its whole-frame motion) and the vectors-valid state moved only because of that gap (the one
// state, P5a MotionValidity: cause Gap, one step), the fill estimate starts from that pair's whole-frame motion scaled by the periods the
// gap spans, as D1's start (MotionWarp::SetSearchSeed): a search PRIOR only, every block's own match still decides. Any other situation
// (a resync, a seek, a format change, a settings change, more than the gap, no usable global motion, no pair) gives NO seed and the
// estimate runs exactly as it does today. No standing cost: nothing is computed unless a gap pair is about to be estimated.
//
// Pure CPU, header only, C++14.
#pragma once

#include <VP-V/modules/motion/estimation/OpticalFlowTrust.h>

#include <cstdint>

namespace VpMotion
{
	struct GapSeed
	{
		static constexpr uint64_t kMaxPeriods = 16;   // a longer gap is not a hiccup: no seed

		bool valid = false;
		float global[2][4] = {};   // forward then backward, D1 format (1/8-size texels: x, y, ambiguity, 1), ready for MotionWarp::SetSearchSeed
		const char* why = "no seed";

		// pairGlobal: the shared pair's whole-frame motion (Flow::global: [0] forward, [1] backward, per-frame, in the same D1 format).
		// pairTo: the pair's later frame, which must be the gap's A. gapFrom / gapTo: the fill pair's frames. pairEpoch: the vectors-valid epoch
		// the pair was estimated in; currentEpoch / lastCauseIsGap: the state now. The gap's own invalidation is one step of the epoch.
		static GapSeed Make(const float pairGlobal[2][4], uint64_t pairTo, uint64_t gapFrom, uint64_t gapTo, uint64_t pairEpoch,
			uint64_t currentEpoch, bool lastCauseIsGap)
		{
			GapSeed s;
			if (pairGlobal == nullptr) { s.why = "no shared pair before the gap"; return s; }
			if (gapTo <= gapFrom + 1) { s.why = "not a gap pair"; return s; }
			const uint64_t periods = gapTo - gapFrom;
			if (periods > kMaxPeriods) { s.why = "the gap is too long to be a hiccup"; return s; }
			if (pairTo != gapFrom) { s.why = "the shared pair does not end at the gap's first frame"; return s; }
			if (!lastCauseIsGap || currentEpoch != pairEpoch + 1) { s.why = "the vectors-valid state moved for more than the gap"; return s; }
			if (!OpticalFlowTrust::GlobalUsable(pairGlobal[0]) || !OpticalFlowTrust::GlobalUsable(pairGlobal[1])) { s.why = "the pair's whole-frame motion is ambiguous"; return s; }
			for (int d = 0; d < 2; ++d)
			{
				s.global[d][0] = pairGlobal[d][0] * static_cast<float>(periods);
				s.global[d][1] = pairGlobal[d][1] * static_cast<float>(periods);
				s.global[d][2] = 0.5f;   // confident enough to start from, as the camera model's D1
				s.global[d][3] = 1.0f;
			}
			s.valid = true;
			s.why = "seeded from the shared pair before the gap";
			return s;
		}
	};
}
