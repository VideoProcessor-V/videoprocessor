// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// Temporal Detail v2 — Stage TD-S4 / Owner Q5(a) & Q5(b) Refinements
// (2026-11-23_TD-consolidated-plan.md s1.6 #2, s3.2 TD-S4, s4.3 Q5):
//   - Q5(a): Half-pixel pan innovation energy I_r comparison between cross form (I_cross = max(0, E - C))
//            and projection form (I_proj = max(0, E - max(0, C)^2 / E_prev)), using VpComposed::SampleBicubic
//            strictly inside ActiveRect. Adopts projection form when relative half-pixel pan bias exceeds
//            Owner Q5's 5% acceptance threshold (kQ5PanBiasThreshold = 0.05).
//   - Q5(b): Crushed-black zero-energy flat block exclusion from dark NLF accumulation strictly by
//            measured block energy (NEVER by code value, preserving BTB/WTW), using the exact 1-pixel
//            half-LSB quantization energy floor (lsbStep^2 / (4 * regionPixels)).
//
// ZERO edits to modules/repair/grain_split/*. Every constant is derived from quantization physics or
// the Owner Q5 specification (Rule 1). Every exclusion and non-finite closure is recorded visibly in
// StandInRegistry and result counters (Rule 4).
// Header-only, C++14/17, no Windows headers.
#pragma once

#include <VP-V/core/RobustStats.h>
#include <VP-V/modules/motion/estimation/composed/WarpSampler.h>
#include <VP-V/modules/repair/temporal_detail/v2/TdV2ReferenceEngine.h>
#include <VP-V/modules/repair/temporal_detail/v2/TdV2StandIns.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace TdV2
{
	// Strictly numerical singularity guard for energy denominators (Rule 1).
	constexpr double kQ5NumericalFloor = 1e-9;

	// Owner Q5(a) half-pixel pan relative bias adoption threshold (2026-11-23_TD-consolidated-plan.md s1.6 #2 & s4.3 Q5):
	// adopt projection form I_proj = max(0, E - max(0, C)^2 / E_prev) when half-pixel pan relative bias > 5%.
	constexpr double kQ5PanBiasThreshold = 0.05;

	// ---- Q5(a): Innovation Energy I_r Cross Form vs Projection Form ----

	// Cross form (GrainLevel v5 L1 baseline): I_cross = max(0, E - C).
	// Non-finite inputs or negative E close visibly to 0.
	inline double ComputeCrossIr(double E, double C, StandInRegistry* standIns = nullptr)
	{
		if (!std::isfinite(E) || !std::isfinite(C) || E < 0.0)
		{
			if (standIns != nullptr)
				standIns->Engage(StandInKind::NonFiniteClosure, "ComputeCrossIr non-finite or negative E");
			return 0.0;
		}
		const double ir = E - C;
		return ir > 0.0 ? ir : 0.0;
	}

	// Projection form (Q5(a) half-pixel bicubic attenuation immune):
	//   I_proj = max(0, E - max(0, C)^2 / E_prev)
	// When warped previous band suffers multiplicative bicubic attenuation alpha (W T = alpha * T),
	// C = alpha * ||T||^2 and E_prev = alpha^2 * ||T||^2 + beta_w * sigma_g^2, so C^2 / E_prev cancels
	// alpha^2 and recovers sigma_g^2 without the (1 - alpha) * ||T||^2 texture bias of E - C.
	// Strictly numerical floor kQ5NumericalFloor = 1e-9 guards E_prev -> 0; non-finite fails closed visibly.
	inline double ComputeProjectionIr(double E, double C, double Eprev, StandInRegistry* standIns = nullptr)
	{
		if (!std::isfinite(E) || !std::isfinite(C) || !std::isfinite(Eprev) || E < 0.0 || Eprev < 0.0)
		{
			if (standIns != nullptr)
				standIns->Engage(StandInKind::NonFiniteClosure, "ComputeProjectionIr non-finite or negative energy");
			return 0.0;
		}
		if (Eprev <= kQ5NumericalFloor)
			return E;
		const double cPos = C > 0.0 ? C : 0.0;
		const double ir = E - (cPos * cPos) / Eprev;
		return ir > 0.0 ? ir : 0.0;
	}

	// Per-region innovation energy selector choosing projection form when Q5(a) adoption is active.
	inline double SelectGrainIr(double E, double C, double Eprev, bool useProjection, StandInRegistry* standIns = nullptr)
	{
		return useProjection
			? ComputeProjectionIr(E, C, Eprev, standIns)
			: ComputeCrossIr(E, C, standIns);
	}

	struct HalfPixelPanIrEval
	{
		size_t regionCount = 0;
		size_t nonFiniteRegions = 0;
		double meanCrossIr = 0.0;
		double meanProjIr = 0.0;
		double relativeBias = 0.0;
		bool adoptProjection = false;
		bool valid = false;

		std::vector<double> regionE;
		std::vector<double> regionC;
		std::vector<double> regionEprev;
		std::vector<double> regionCrossIr;
		std::vector<double> regionProjIr;
	};

	// Evaluate half-pixel pan bias between cross form and projection form on high-band planes (curD, prevD).
	// Warps `prevD` with VpComposed::SampleBicubic strictly inside `activeRect` along current->prev motion (vx, vy)
	// (e.g. vx = 0.5, vy = 0.0), skipping border regions whose 4x4 bicubic footprint touches outside ActiveRect.
	// Computes relativeBias = |meanCross - meanProj| / max(meanProj, grainVarRef, kQ5NumericalFloor),
	// setting adoptProjection = (relativeBias > kQ5PanBiasThreshold).
	inline HalfPixelPanIrEval EvaluateHalfPixelPanIr(
		const TdV2Plane& curD,
		const TdV2Plane& prevD,
		const ActiveRect& activeRect,
		double vx,
		double vy,
		int regionPx = 8,
		double grainVarRef = 0.0,
		StandInRegistry* standIns = nullptr)
	{
		HalfPixelPanIrEval out;
		if (curD.Empty() || prevD.Empty() || curD.w != prevD.w || curD.h != prevD.h || regionPx <= 0)
			return out;
		if (!std::isfinite(vx) || !std::isfinite(vy) || !std::isfinite(grainVarRef) || grainVarRef < 0.0)
		{
			if (standIns != nullptr)
				standIns->Engage(StandInKind::NonFiniteClosure, "EvaluateHalfPixelPanIr invalid args");
			return out;
		}

		const ActiveRect active = activeRect.Empty()
			? ActiveRect::Full(curD.w, curD.h)
			: activeRect.ClampedTo(curD.w, curD.h);
		if (active.Empty())
			return out;

		const int aw = active.Width();
		const int ah = active.Height();
		ActiveSubPlaneView prevView{ &prevD, active.x0, active.y0, aw, ah };

		const int rw = (aw + regionPx - 1) / regionPx;
		const int rh = (ah + regionPx - 1) / regionPx;
		double sumCross = 0.0;
		double sumProj = 0.0;

		for (int ry = 0; ry < rh; ++ry)
		{
			const int ly0 = ry * regionPx;
			const int ly1 = (std::min)(ah, ly0 + regionPx);
			for (int rx = 0; rx < rw; ++rx)
			{
				const int lx0 = rx * regionPx;
				const int lx1 = (std::min)(aw, lx0 + regionPx);

				double sumE = 0.0;
				double sumC = 0.0;
				double sumEprev = 0.0;
				size_t cnt = 0;
				bool allInFootprint = true;
				bool hasNonFinite = false;

				for (int ly = ly0; ly < ly1 && allInFootprint && !hasNonFinite; ++ly)
				{
					for (int lx = lx0; lx < lx1; ++lx)
					{
						const double dc = curD.at(active.x0 + lx, active.y0 + ly);
						if (!std::isfinite(dc))
						{
							hasNonFinite = true;
							break;
						}
						double dp = 0.0;
						if (!VpComposed::SampleBicubic(prevView, static_cast<double>(lx) + vx, static_cast<double>(ly) + vy, dp))
						{
							allInFootprint = false;
							break;
						}
						if (!std::isfinite(dp))
						{
							hasNonFinite = true;
							break;
						}
						sumE += dc * dc;
						sumC += dc * dp;
						sumEprev += dp * dp;
						++cnt;
					}
				}

				if (hasNonFinite)
				{
					++out.nonFiniteRegions;
					continue;
				}
				if (!allInFootprint || cnt == 0)
					continue;

				const double invCnt = 1.0 / static_cast<double>(cnt);
				const double E = sumE * invCnt;
				const double C = sumC * invCnt;
				const double Eprev = sumEprev * invCnt;
				const double ic = ComputeCrossIr(E, C, standIns);
				const double ip = ComputeProjectionIr(E, C, Eprev, standIns);

				out.regionE.push_back(E);
				out.regionC.push_back(C);
				out.regionEprev.push_back(Eprev);
				out.regionCrossIr.push_back(ic);
				out.regionProjIr.push_back(ip);
				sumCross += ic;
				sumProj += ip;
				++out.regionCount;
			}
		}

		if (out.nonFiniteRegions > 0 && standIns != nullptr)
			standIns->Engage(StandInKind::NonFiniteClosure, "EvaluateHalfPixelPanIr non-finite region");

		if (out.regionCount > 0)
		{
			const double invR = 1.0 / static_cast<double>(out.regionCount);
			out.meanCrossIr = sumCross * invR;
			out.meanProjIr = sumProj * invR;
			const double denom = (std::max)(out.meanProjIr, (std::max)(grainVarRef, kQ5NumericalFloor));
			out.relativeBias = std::fabs(out.meanCrossIr - out.meanProjIr) / denom;
			out.adoptProjection = (out.relativeBias > kQ5PanBiasThreshold);
			out.valid = true;
		}
		return out;
	}

	// ---- Q5(b): Crushed Black Zero-Energy Block Exclusion by Energy (NEVER Code Value, Preserving BTB/WTW) ----

	// Exact derived quantization floor for a flat block (Rule 1: zero arbitrary constants):
	// Energy of a single pixel deviating by 0.5 LSB averaged across `regionPixels` pixels:
	//   E_floor = (0.5 * lsbStep)^2 / regionPixels = (lsbStep * lsbStep) / (4.0 * regionPixels).
	inline double ZeroEnergyBlockThreshold(size_t regionPixels, double lsbStep = 1.0)
	{
		if (regionPixels == 0 || !std::isfinite(lsbStep) || lsbStep <= 0.0)
			return 0.0;
		return (lsbStep * lsbStep) / (4.0 * static_cast<double>(regionPixels));
	}

	// Returns true iff `blockEnergy` is a valid non-negative finite energy at or below the 1-pixel half-LSB
	// quantization floor. Strictly energy-based (never inspects code values, preserving BTB/WTW).
	inline bool IsZeroEnergyFlatBlock(double blockEnergy, size_t regionPixels, double lsbStep = 1.0)
	{
		if (!std::isfinite(blockEnergy) || blockEnergy < 0.0 ||
		    regionPixels == 0 || !std::isfinite(lsbStep) || lsbStep <= 0.0)
		{
			return false;
		}
		return blockEnergy <= ZeroEnergyBlockThreshold(regionPixels, lsbStep);
	}

	struct DarkNlfResult
	{
		double nlfEstimate = 0.0;
		size_t retainedBlocks = 0;
		size_t excludedZeroEnergyBlocks = 0;
		size_t nonFiniteBlocks = 0;
		bool valid = false;
	};

	// Accumulate dark-bin NLF quantile from per-block innovation energies `blockIr`, excluding crushed
	// zero-energy flat blocks strictly by `blockEnergies` (never by code value, preserving BTB/WTW).
	// Uses U24 RobustStats::Quantile and visibly engages StandInKind::ZeroEnergyDarkBlockExcluded whenever
	// any zero-energy flat block is excluded.
	inline DarkNlfResult AccumulateDarkNlfExcludingZeroEnergy(
		const std::vector<double>& blockEnergies,
		const std::vector<double>& blockIr,
		size_t regionPixels,
		double lsbStep,
		double quantileP,
		StandInRegistry* standIns = nullptr)
	{
		DarkNlfResult res;
		if (regionPixels == 0 || !std::isfinite(lsbStep) || lsbStep <= 0.0 ||
		    !std::isfinite(quantileP) || quantileP < 0.0 || quantileP > 1.0 ||
		    blockEnergies.size() != blockIr.size())
		{
			if (standIns != nullptr)
				standIns->Engage(StandInKind::NonFiniteClosure, "AccumulateDarkNlfExcludingZeroEnergy invalid args");
			return res;
		}

		std::vector<double> retained;
		retained.reserve(blockIr.size());

		for (size_t i = 0; i < blockEnergies.size(); ++i)
		{
			const double e = blockEnergies[i];
			const double ir = blockIr[i];
			if (!std::isfinite(e) || !std::isfinite(ir) || e < 0.0)
			{
				++res.nonFiniteBlocks;
				continue;
			}
			if (IsZeroEnergyFlatBlock(e, regionPixels, lsbStep))
			{
				++res.excludedZeroEnergyBlocks;
				continue;
			}
			retained.push_back(ir > 0.0 ? ir : 0.0);
		}

		if (res.nonFiniteBlocks > 0 && standIns != nullptr)
			standIns->Engage(StandInKind::NonFiniteClosure, "AccumulateDarkNlfExcludingZeroEnergy non-finite block");
		if (res.excludedZeroEnergyBlocks > 0 && standIns != nullptr)
			standIns->Engage(StandInKind::ZeroEnergyDarkBlockExcluded);

		res.retainedBlocks = retained.size();
		if (!retained.empty())
		{
			res.nlfEstimate = RobustStats::Quantile(retained, quantileP, RobustStats::RankRule::Interpolated);
			res.valid = std::isfinite(res.nlfEstimate);
		}
		return res;
	}
}
