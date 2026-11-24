// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// Temporal Detail v2 — Stage TD-S4 / Owner Q5(a) & Q5(b) Refinements
// (2026-11-23_TD-consolidated-plan.md s1.6 #2, s3.2 TD-S4, s4.3 Q5):
//   - Q5(a): Half-pixel pan innovation energy I_r comparison between cross form (I_cross = max(0, E - C))
//            and exact bias-corrected projection form solving (E - I_proj)(E_prev - beta * I_proj) = max(0, C)^2
//            (where beta = ||W||_2^2 is the exact Catmull-Rom bicubic variance transfer gain from
//            VpComposed::CubicWeights), using VpComposed::SampleBicubic strictly inside ActiveRect.
//            Adopts projection form when relative half-pixel pan bias exceeds Owner Q5's 5% threshold
//            (kQ5PanBiasThreshold = 0.05).
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
	// adopt bias-corrected projection form when half-pixel pan relative bias > 5%.
	constexpr double kQ5PanBiasThreshold = 0.05;

	// ---- Q5(a): Innovation Energy I_r Cross Form vs Exact Bias-Corrected Projection Form ----

	// Exact 2D separable Catmull-Rom bicubic white-variance transfer gain beta(vx, vy) = ||W_x||_2^2 * ||W_y||_2^2
	// using shared VpComposed::CubicWeights (Rule 1 & D5: zero private cubic weight copies).
	// Examples: integer shift (0, 0) -> beta = 1.0; half-pixel horizontal pan (0.5, 0) -> beta = 41/64 = 0.640625.
	inline double BicubicWarpVarianceGain(double vx, double vy)
	{
		if (!std::isfinite(vx) || !std::isfinite(vy))
			return 1.0;
		const double fx = vx - std::floor(vx);
		const double fy = vy - std::floor(vy);
		double wx[4] = {};
		double wy[4] = {};
		VpComposed::CubicWeights(fx, wx);
		VpComposed::CubicWeights(fy, wy);
		double sx = 0.0;
		double sy = 0.0;
		for (int i = 0; i < 4; ++i)
		{
			sx += wx[i] * wx[i];
			sy += wy[i] * wy[i];
		}
		return sx * sy;
	}

	// Exact 2D separable high-pass band-weighted Catmull-Rom bicubic variance transfer gain
	// beta_HP(vx, vy) = ||H * W||_2^2 / ||H||_2^2 for H = I - G_x G_y and bicubic W = w_x w_y (Rule 1 Parseval identity).
	// Reduces identically to 1.0 at integer shifts (num == den), falls back to BicubicWarpVarianceGain(vx, vy)
	// when gx or gy is empty (H = I), and is strictly < BicubicWarpVarianceGain(vx, vy) at sub-pixel shifts
	// because high-pass H emphasizes high frequencies where bicubic interpolation attenuates more strongly.
	inline double HighPassBicubicWarpVarianceGain(
		double vx,
		double vy,
		const std::vector<double>& gx,
		const std::vector<double>& gy)
	{
		if (!std::isfinite(vx) || !std::isfinite(vy))
			return 1.0;
		if (gx.empty() || gy.empty())
			return BicubicWarpVarianceGain(vx, vy);

		auto computeAxis = [](double v, const std::vector<double>& g,
		                      double& outWW, double& outUU, double& outWU, double& outGG, double& outG0)
		{
			const double f = v - std::floor(v);
			double w[4] = {};
			VpComposed::CubicWeights(f, w);

			const int gLen = static_cast<int>(g.size());
			const int r = gLen / 2;
			const int mMin = -r - 1;
			const int mMax = (gLen - 1 - r) + 2;

			auto gAt = [&](int offset) -> double
			{
				const int idx = offset + r;
				return (idx >= 0 && idx < gLen) ? g[static_cast<size_t>(idx)] : 0.0;
			};

			outWW = 0.0;
			for (int i = 0; i < 4; ++i)
				outWW += w[i] * w[i];

			outGG = 0.0;
			for (int j = 0; j < gLen; ++j)
				outGG += g[static_cast<size_t>(j)] * g[static_cast<size_t>(j)];
			outG0 = g[static_cast<size_t>(r)];

			outUU = 0.0;
			outWU = 0.0;
			for (int m = mMin; m <= mMax; ++m)
			{
				double u = 0.0;
				for (int i = 0; i < 4; ++i)
					u += w[i] * gAt(m - (i - 1));
				outUU += u * u;
				if (m >= -1 && m <= 2)
					outWU += w[m + 1] * u;
			}
		};

		double wwX = 0.0, uuX = 0.0, wuX = 0.0, ggX = 0.0, g0X = 0.0;
		double wwY = 0.0, uuY = 0.0, wuY = 0.0, ggY = 0.0, g0Y = 0.0;
		computeAxis(vx, gx, wwX, uuX, wuX, ggX, g0X);
		computeAxis(vy, gy, wwY, uuY, wuY, ggY, g0Y);

		const double den = 1.0 - 2.0 * g0X * g0Y + ggX * ggY;
		if (!(den > kQ5NumericalFloor))
			return BicubicWarpVarianceGain(vx, vy);

		const double num = wwX * wwY - 2.0 * wuX * wuY + uuX * uuY;
		const double ratio = num / den;
		if (!std::isfinite(ratio))
			return BicubicWarpVarianceGain(vx, vy);
		return ratio < 0.0 ? 0.0 : (ratio > 1.0 ? 1.0 : ratio);
	}

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

	// Exact bias-corrected projection form (Q5(a) half-pixel bicubic attenuation immune, Rule 1):
	// Model under sub-pixel bicubic warp W with multiplicative texture attenuation alpha in (0, 1] and
	// exact band-weighted warp variance transfer gain warpGainBeta = ||H * W||_2^2 / ||H||_2^2 in (0, 1]
	// (reduces to ||W||_2^2 for white/unfiltered noise H = I, and is strictly < ||W||_2^2 on high-pass bands H = I - G):
	//   X = T + G_0,   W Y_prev = alpha * T + W G_1   (G_0, G_1 independent zero-mean variance sigma^2)
	// Observable second moments (S = E[T^2] >= 0):
	//   E = S + sigma^2,   C = alpha * S,   E_prev = alpha^2 * S + beta * sigma^2.
	// The naive form E - C^2 / E_prev neglects the warped grain variance beta * sigma^2 in E_prev and therefore
	// converges to (1 + beta / alpha^2) * sigma^2 (~2.36 * sigma^2 at 0.5-px pan) when S >> sigma^2.
	// Subtracting beta * sigma^2 from E_prev yields the exact fixed-point identity:
	//   I_proj = E - C_+^2 / (E_prev - beta * I_proj)   <=>   (E - I_proj) * (E_prev - beta * I_proj) = C_+^2
	// whose unique physical root (0 <= I_proj <= min(E, E_prev / beta)) in cancellation-free rationalized form is:
	//   I_proj = 2 * max(0, E * E_prev - C_+^2) / ((E_prev + beta * E) + sqrt((E_prev - beta * E)^2 + 4 * beta * C_+^2)).
	// Exact limits:
	//   - Texture + grain (any S >= 0, alpha > 0, beta > 0): I_proj == sigma^2 identically (0% bias).
	//   - Pure grain (C <= 0): returns E == sigma^2 directly.
	//   - Pure texture (sigma^2 == 0): E * E_prev - C_+^2 == 0 => I_proj == 0.
	//   - Integer shift (alpha == 1, beta == 1, E == E_prev): reduces algebraically to E - C_+.
	//   - beta -> 0+: reduces smoothly to max(0, E - C_+^2 / E_prev) without division by zero.
	inline double ComputeProjectionIr(
		double E,
		double C,
		double Eprev,
		StandInRegistry* standIns = nullptr,
		double warpGainBeta = 1.0)
	{
		if (!std::isfinite(E) || !std::isfinite(C) || !std::isfinite(Eprev) || !std::isfinite(warpGainBeta) ||
		    E < 0.0 || Eprev < 0.0 || warpGainBeta < 0.0)
		{
			if (standIns != nullptr)
				standIns->Engage(StandInKind::NonFiniteClosure, "ComputeProjectionIr non-finite or negative energy");
			return 0.0;
		}
		if (Eprev <= kQ5NumericalFloor)
			return E;
		if (C <= 0.0)
			return E;
		const double det = E * Eprev - C * C;
		if (det <= 0.0)
			return 0.0;
		const double bE = warpGainBeta * E;
		const double diff = Eprev - bE;
		const double disc = diff * diff + 4.0 * warpGainBeta * C * C;
		const double denom = (Eprev + bE) + std::sqrt(disc);
		if (!(denom > kQ5NumericalFloor * kQ5NumericalFloor))
			return 0.0;
		const double ir = (2.0 * det) / denom;
		return ir > 0.0 ? ir : 0.0;
	}

	// Per-region innovation energy selector choosing bias-corrected projection form when Q5(a) adoption is active.
	inline double SelectGrainIr(
		double E,
		double C,
		double Eprev,
		bool useProjection,
		StandInRegistry* standIns = nullptr,
		double warpGainBeta = 1.0)
	{
		return useProjection
			? ComputeProjectionIr(E, C, Eprev, standIns, warpGainBeta)
			: ComputeCrossIr(E, C, standIns);
	}

	struct HalfPixelPanIrEval
	{
		size_t regionCount = 0;
		size_t skippedBorderRegions = 0;
		size_t nonFiniteRegions = 0;
		double warpGainBeta = 1.0;
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

		const double warpBeta = BicubicWarpVarianceGain(vx, vy);
		out.warpGainBeta = warpBeta;

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
				{
					++out.skippedBorderRegions;
					continue;
				}

				const double invCnt = 1.0 / static_cast<double>(cnt);
				const double E = sumE * invCnt;
				const double C = sumC * invCnt;
				const double Eprev = sumEprev * invCnt;
				const double ic = ComputeCrossIr(E, C, standIns);
				const double ip = ComputeProjectionIr(E, C, Eprev, standIns, warpBeta);

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

	// Compose Q5(a) pan evaluation with Q5(b) zero-energy dark block exclusion using the selected Ir form.
	inline DarkNlfResult AccumulatePanEvalDarkNlf(
		const HalfPixelPanIrEval& eval,
		int regionPx,
		double lsbStep = 1.0,
		double quantileP = 0.5,
		StandInRegistry* standIns = nullptr)
	{
		if (!eval.valid || regionPx <= 0)
			return DarkNlfResult{};
		const size_t regionPixels = static_cast<size_t>(regionPx) * static_cast<size_t>(regionPx);
		const std::vector<double>& chosenIr = eval.adoptProjection ? eval.regionProjIr : eval.regionCrossIr;
		return AccumulateDarkNlfExcludingZeroEnergy(eval.regionE, chosenIr, regionPixels, lsbStep, quantileP, standIns);
	}
}
