// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// repair.grain_split GrainGuard: the shared output-vs-input grain guard unit (refs/129 L-B2 (d), D1).
//
// A reader that changes a picture which carries grain measures, on the shared split's grain map G_in = Y - clean, how
// much of the frame's grain its output still holds: G_out = G_in + (1 - f) delta (delta = the reader's change). Per
// block it keeps seven sums over the grain-dominated pixels (f < kMaxShare):
//
//     s[0] = sum G_in^2, s[1] = sum G_out^2, s[2] = sum ((1 - f) delta) G_in,
//     s[3] = sum G_in P_in, s[4] = sum G_out P_out, s[5] = sum P_in^2, s[6] = sum P_out^2
//
// (P_in / P_out: the previous frame's maps warped one hop along the backward vectors; measurement only). Violation of a
// pooled (3 x 3 blocks) set of sums: v = max(drop / kDropTrip, (L - 1) / kRiseTrip, freeze / kFreezeTrip) with
// L = sqrt(s1 / s0), drop = max(1 - L, -s2 / s0), freeze = the lag-1 correlation increase. Trip above 1, release below
// kRelease, fast fall / slow rise of the weight. temporal_detail is the first reader (its model and GLSL use these
// values and this law; the GLSL receives them as defines). Header-only, C++14, no Windows headers.
#pragma once

#include <cmath>
#include <cstdint>

namespace GrainSplit
{
	namespace Guard
	{
		constexpr uint32_t kUnitVersion = 1;
		constexpr double kStatsSeconds = 0.25;      // EMA time constant of the tile sums
		constexpr double kDropTrip = 0.04;          // current-grain loss (projection or level) that trips
		constexpr double kRiseTrip = 0.05;          // output grain level above the input's that trips
		constexpr double kFreezeTrip = 0.03;        // lag-1 grain correlation increase that trips
		constexpr double kRelease = 0.5;            // hysteresis: released below this share of the trip level
		constexpr double kFallSeconds = 0.1, kRiseSeconds = 2.0;   // weight rate limits (slow recovery: no trip/release cycling)
		constexpr double kAddBackMin = 0.01;        // measured current-grain loss below this is not added back
		constexpr double kMaxShare = 0.2;           // statistics use grain-dominated pixels only (Wiener share f below this)
		constexpr double kTemplateShare = 0.25;     // a template transfer is excluded where T^2 > this x the block level N
		constexpr int kSums = 7;

		// Violation of one pooled set of sums (law above). 0 when the block holds no grain.
		inline double Violation(const double* o)
		{
			double v = 0.0;
			if (o[0] > 1e-9)
			{
				const double L = std::sqrt(o[1] / o[0]);
				const double a = 1.0 - L, b = -o[2] / o[0];
				const double drop = a > b ? a : b;
				const double r1 = drop / kDropTrip, r2 = (L - 1.0) / kRiseTrip;
				v = r1 > r2 ? r1 : r2;
				if (o[5] > 1e-9 && o[6] > 1e-9 && o[1] > 1e-9)
				{
					const double freeze = o[4] / std::sqrt(o[1] * o[6]) - o[3] / std::sqrt(o[0] * o[5]);
					const double r3 = freeze / kFreezeTrip;
					v = v > r3 ? v : r3;
				}
			}
			return v;
		}

		// Immediate add-back share of one pooled set of sums: the measured missing current-frame grain, or 0.
		inline double AddBack(const double* o)
		{
			const double k = o[0] > 1e-9 ? -o[2] / o[0] : 0.0;
			return k > kAddBackMin ? (k < 1.0 ? k : 1.0) : 0.0;
		}

		// One weight step: trip / release with hysteresis, then the rate-limited move toward the target. Returns true on a trip.
		inline bool Step(double v, double fall, double rise, bool& on, double& weight)
		{
			bool trip = false;
			if (!on && v > 1.0) { on = true; trip = true; }
			else if (on && v < kRelease) on = false;
			const double target = on ? (v > 1.0 ? weight / v : weight) : 1.0;
			double dlt = target - weight;
			dlt = dlt < -fall ? -fall : (dlt > rise ? rise : dlt);
			weight += dlt;
			return trip;
		}
	}
}
