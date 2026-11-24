// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// Temporal Detail v2 — Stage TD-S0 (2026-11-23_TD-consolidated-plan.md s1.3, s1.4, s1.6, s3.2, s3.3):
// Acceptance metrics harness:
//   1. W1-W4 worst-area mask evaluator + worst-5% trackable cell delta-dB (never hiding regressions in global mean)
//   2. Camera-relative speed-bin sharpness evaluator (camera pan row strictly separate from object speed bins)
//   3. Authoritative synthetic grain-transfer evaluator (refs/111 s7.3: beta_own >= 0.98, beta_foreign <= 0.02,
//      RMS ratio within +/-2%, lag-1 temporal correlation rise <= +0.02)
//   4. Non-finite inputs fail closed with explicit visible counts (Rule 4).
//
// Uses U24 RobustStats for quantiles (no private quantile copies). Header-only, C++14/17, no Windows headers.
#pragma once

#include <VP-V/core/RobustStats.h>
#include <VP-V/modules/repair/grain_split/shared/BandFromBase.h>
#include <VP-V/modules/repair/temporal_detail/v2/TdV2Config.h>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace TdV2
{
	// Strictly numerical floor for PSNR / variance denominators (Rule 1).
	constexpr double kMetricNumFloor = 1e-12;

	// Authoritative grain preservation bars (refs/111 s7.3, 99 R2, MISS-grain-6).
	constexpr double kGrainBarMinBetaOwn = 0.98;
	constexpr double kGrainBarMaxBetaForeign = 0.02;
	constexpr double kGrainBarMaxRmsError = 0.02;
	constexpr double kGrainBarMaxLag1Rise = 0.02;

	// Worst-area mask bit flags (plan s1.4 & s3.3).
	enum WorstAreaBit : uint8_t
	{
		kMaskNone = 0,
		kMaskW1BandedSky = 1u << 0,
		kMaskW2DarkStill = 1u << 1,
		kMaskW3BetweenKeyframes = 1u << 2,
		kMaskW4ChromaEdges = 1u << 3,
		kMaskTrackable = 1u << 4
	};

	struct RegionPsnrStats
	{
		size_t pixelCount = 0;
		double baselineMse = 0.0;
		double outputMse = 0.0;
		double baselinePsnrDb = 0.0;
		double outputPsnrDb = 0.0;
		double deltaDb = 0.0;
		bool valid = false;
	};

	// Safe PSNR in dB from MSE and peak code value. Returns false when non-finite or peak <= 0.
	inline bool ComputePsnrDb(double mse, double peakCode, double& outDb)
	{
		if (!std::isfinite(mse) || !std::isfinite(peakCode) || mse < 0.0 || peakCode <= 0.0)
		{
			outDb = 0.0;
			return false;
		}
		// Cap zero-error MSE at numerical floor so exact match stays finite and strictly monotonic.
		const double clampedMse = mse > kMetricNumFloor ? mse : kMetricNumFloor;
		outDb = 10.0 * std::log10((peakCode * peakCode) / clampedMse);
		return std::isfinite(outDb);
	}

	struct WorstAreaReport
	{
		RegionPsnrStats global;
		RegionPsnrStats w1BandedSky;
		RegionPsnrStats w2DarkStill;
		RegionPsnrStats w3BetweenKeyframes;
		RegionPsnrStats w4ChromaEdges;

		// Worst 5% of trackable cells (plan s3.3 P2: mean delta-dB on worst 5% and fraction worsened > 0.1 dB).
		size_t trackableCellCount = 0;
		size_t worst5PctCellCount = 0;
		double worst5PctMeanDeltaDb = 0.0;
		double worst5PctWorsenedOver01DbFrac = 0.0;

		// W1 banding proxy: ratio of total variation in W1 mask relative to master gradient (lower is smoother/less banded).
		double w1BandingStepReduction = 0.0;

		size_t nonFiniteCount = 0;
		bool valid = false;
	};

	inline void FinalizeRegionPsnr(RegionPsnrStats& s, double sumSqBase, double sumSqOut, double peakCode)
	{
		if (s.pixelCount == 0)
		{
			s.valid = false;
			return;
		}
		s.baselineMse = sumSqBase / static_cast<double>(s.pixelCount);
		s.outputMse = sumSqOut / static_cast<double>(s.pixelCount);
		const bool okBase = ComputePsnrDb(s.baselineMse, peakCode, s.baselinePsnrDb);
		const bool okOut = ComputePsnrDb(s.outputMse, peakCode, s.outputPsnrDb);
		s.valid = okBase && okOut;
		s.deltaDb = s.valid ? (s.outputPsnrDb - s.baselinePsnrDb) : 0.0;
	}

	// Evaluate W1-W4 worst-area masks and worst-5% trackable cell delta-dB over a w x h plane.
	// `maskBits` has size w * h with bitwise OR of WorstAreaBit flags.
	inline WorstAreaReport EvaluateWorstAreaMasks(
		const double* master,
		const double* received,
		const double* output,
		const uint8_t* maskBits,
		int w,
		int h,
		int cellPx = kCanonicalBlockPx,
		double peakCode = 1023.0)
	{
		WorstAreaReport rep;
		if (master == nullptr || received == nullptr || output == nullptr || maskBits == nullptr ||
		    w <= 0 || h <= 0 || cellPx <= 0 || !std::isfinite(peakCode) || peakCode <= 0.0)
		{
			return rep;
		}

		double sumBase[5] = {};
		double sumOut[5] = {};
		size_t cnt[5] = {};

		// W1 banding proxy: sum of excess step magnitudes |diff_x(img) - diff_x(master)| inside W1.
		double w1ErrStepBase = 0.0;
		double w1ErrStepOut = 0.0;

		const size_t totalPx = static_cast<size_t>(w) * static_cast<size_t>(h);
		for (size_t i = 0; i < totalPx; ++i)
		{
			const double m = master[i];
			const double r = received[i];
			const double o = output[i];
			if (!std::isfinite(m) || !std::isfinite(r) || !std::isfinite(o))
			{
				++rep.nonFiniteCount;
				continue;
			}
			const double eb = (r - m) * (r - m);
			const double eo = (o - m) * (o - m);

			sumBase[0] += eb; sumOut[0] += eo; ++cnt[0];
			const uint8_t bits = maskBits[i];
			if (bits & kMaskW1BandedSky)         { sumBase[1] += eb; sumOut[1] += eo; ++cnt[1]; }
			if (bits & kMaskW2DarkStill)         { sumBase[2] += eb; sumOut[2] += eo; ++cnt[2]; }
			if (bits & kMaskW3BetweenKeyframes)  { sumBase[3] += eb; sumOut[3] += eo; ++cnt[3]; }
			if (bits & kMaskW4ChromaEdges)       { sumBase[4] += eb; sumOut[4] += eo; ++cnt[4]; }
		}

		if (rep.nonFiniteCount > 0)
		{
			rep.valid = false;
			return rep;
		}

		// W1 horizontal step-error reduction (banding step proxy).
		for (int y = 0; y < h; ++y)
		{
			for (int x = 1; x < w; ++x)
			{
				const size_t idx = static_cast<size_t>(y) * w + x;
				if ((maskBits[idx] & kMaskW1BandedSky) && (maskBits[idx - 1] & kMaskW1BandedSky))
				{
					const double dm = master[idx] - master[idx - 1];
					const double dr = received[idx] - received[idx - 1];
					const double dout = output[idx] - output[idx - 1];
					w1ErrStepBase += std::fabs(dr - dm);
					w1ErrStepOut += std::fabs(dout - dm);
				}
			}
		}
		if (w1ErrStepBase > kMetricNumFloor)
			rep.w1BandingStepReduction = (w1ErrStepBase - w1ErrStepOut) / w1ErrStepBase;

		rep.global.pixelCount = cnt[0];
		rep.w1BandedSky.pixelCount = cnt[1];
		rep.w2DarkStill.pixelCount = cnt[2];
		rep.w3BetweenKeyframes.pixelCount = cnt[3];
		rep.w4ChromaEdges.pixelCount = cnt[4];

		FinalizeRegionPsnr(rep.global, sumBase[0], sumOut[0], peakCode);
		FinalizeRegionPsnr(rep.w1BandedSky, sumBase[1], sumOut[1], peakCode);
		FinalizeRegionPsnr(rep.w2DarkStill, sumBase[2], sumOut[2], peakCode);
		FinalizeRegionPsnr(rep.w3BetweenKeyframes, sumBase[3], sumOut[3], peakCode);
		FinalizeRegionPsnr(rep.w4ChromaEdges, sumBase[4], sumOut[4], peakCode);

		// Per-cell trackable evaluation for worst-5% metric (plan s3.3 P2).
		struct CellEval { double baseMse; double deltaDb; };
		std::vector<CellEval> cells;
		std::vector<double> baseMses;
		for (int cy = 0; cy < h; cy += cellPx)
		{
			const int yEnd = (cy + cellPx < h) ? (cy + cellPx) : h;
			for (int cx = 0; cx < w; cx += cellPx)
			{
				const int xEnd = (cx + cellPx < w) ? (cx + cellPx) : w;
				double sb = 0.0, so = 0.0;
				size_t nTrack = 0;
				for (int y = cy; y < yEnd; ++y)
				{
					for (int x = cx; x < xEnd; ++x)
					{
						const size_t idx = static_cast<size_t>(y) * w + x;
						if (maskBits[idx] & kMaskTrackable)
						{
							const double eb = received[idx] - master[idx];
							const double eo = output[idx] - master[idx];
							sb += eb * eb;
							so += eo * eo;
							++nTrack;
						}
					}
				}
				if (nTrack > 0)
				{
					const double mb = sb / static_cast<double>(nTrack);
					const double mo = so / static_cast<double>(nTrack);
					double dbB = 0.0, dbO = 0.0;
					if (ComputePsnrDb(mb, peakCode, dbB) && ComputePsnrDb(mo, peakCode, dbO))
					{
						cells.push_back({ mb, dbO - dbB });
						baseMses.push_back(mb);
					}
				}
			}
		}

		rep.trackableCellCount = cells.size();
		if (!cells.empty())
		{
			// Worst 5% by received error = cells with baselineMse >= 95th percentile (via U24 RobustStats::Quantile).
			const double p95Mse = RobustStats::Quantile(baseMses, 0.95, RobustStats::RankRule::NearestRank);
			double sumDelta = 0.0;
			size_t nWorst = 0;
			size_t nWorsened = 0;
			for (const auto& c : cells)
			{
				if (c.baseMse >= p95Mse)
				{
					sumDelta += c.deltaDb;
					++nWorst;
					if (c.deltaDb < -0.1)
						++nWorsened;
				}
			}
			rep.worst5PctCellCount = nWorst;
			if (nWorst > 0)
			{
				rep.worst5PctMeanDeltaDb = sumDelta / static_cast<double>(nWorst);
				rep.worst5PctWorsenedOver01DbFrac = static_cast<double>(nWorsened) / static_cast<double>(nWorst);
			}
		}

		rep.valid = rep.global.valid;
		return rep;
	}

	// ---- Camera-Relative Speed-Bin Sharpness Evaluator (plan s1.3 & s3.3) ----
	// Camera pan row is ALWAYS kept separate from camera-relative moving-object speed bins.
	// Bin edges derived from physical motion scales:
	//   - 0.25 px: quarter-pel sub-pixel accuracy ceiling (P3r / CellTrust)
	//   - 1.0  px: 1-pixel Nyquist / kSigmaFine scale
	//   - 3.0  px: full Gaussian filter radius ceil(3 * kSigmaFine)
	//   - 8.0  px: half canonical 16-px cell width
	enum class ObjectSpeedBinId : uint8_t
	{
		Still = 0,       // [0.0, 0.25) px/frame
		Slow = 1,        // [0.25, 1.0) px/frame
		Medium = 2,      // [1.0, 3.0) px/frame
		Fast = 3,        // [3.0, 8.0) px/frame
		VeryFast = 4,    // [8.0, inf) px/frame
		Count = 5
	};

	inline ObjectSpeedBinId ClassifyObjectSpeed(double relSpeedPx)
	{
		// Derived physical bin edges (Rule 1):
		//   - 0.25 px: quarter-pel P3r accuracy ceiling
		//   - kSigmaFine (1.0 px): fine-band Gaussian sigma scale
		//   - BandRadius(kSigmaFine) (3.0 px): full fine-band filter radius
		//   - 0.5 * kCanonicalBlockPx (8.0 px): half canonical 16-px cell edge
		const double slowEdge = VpUnit::BandFromBase::kSigmaFine;
		const double medEdge = static_cast<double>(VpUnit::BandFromBase::BandRadius(VpUnit::BandFromBase::kSigmaFine));
		const double fastEdge = 0.5 * static_cast<double>(kCanonicalBlockPx);
		if (relSpeedPx < 0.25)     return ObjectSpeedBinId::Still;
		if (relSpeedPx < slowEdge) return ObjectSpeedBinId::Slow;
		if (relSpeedPx < medEdge)  return ObjectSpeedBinId::Medium;
		if (relSpeedPx < fastEdge) return ObjectSpeedBinId::Fast;
		return ObjectSpeedBinId::VeryFast;
	}

	struct SpeedBinRow
	{
		size_t pixelCount = 0;
		double masterGradEnergy = 0.0;
		double outputGradEnergy = 0.0;
		double sharpnessRetention = 1.0; // sqrt(outputGradEnergy / masterGradEnergy)
		double deltaDb = 0.0;
		bool valid = false;
	};

	struct SpeedBinReport
	{
		SpeedBinRow cameraPan;   // Camera-moving background (isCameraBg == 1, |v_cam| >= 0.25, |v_obj| < 0.25)
		std::array<SpeedBinRow, static_cast<size_t>(ObjectSpeedBinId::Count)> objectBins{};
		double worstObjectRetention = 1.0;
		ObjectSpeedBinId worstObjectBin = ObjectSpeedBinId::Still;
		size_t nonFiniteCount = 0;
		bool valid = false;
	};

	// Evaluate sharpness retention (gradient energy ratio vs master) and delta-dB per speed bin.
	// `isCameraBg` marks pixels belonging to the camera-moving background (1) vs moving/static object class (0).
	inline SpeedBinReport EvaluateSpeedBins(
		const double* master,
		const double* received,
		const double* output,
		const double* objSpeedPx,
		const double* camSpeedPx,
		const uint8_t* isCameraBg,
		int w,
		int h,
		double peakCode = 1023.0)
	{
		SpeedBinReport rep;
		if (master == nullptr || received == nullptr || output == nullptr ||
		    objSpeedPx == nullptr || camSpeedPx == nullptr || isCameraBg == nullptr ||
		    w < 2 || h < 2 || !std::isfinite(peakCode) || peakCode <= 0.0)
		{
			return rep;
		}

		struct Acc
		{
			size_t n = 0;
			double gMaster = 0.0;
			double gOut = 0.0;
			double errBase = 0.0;
			double errOut = 0.0;
		};
		Acc panAcc;
		std::array<Acc, static_cast<size_t>(ObjectSpeedBinId::Count)> objAcc{};

		for (int y = 0; y < h - 1; ++y)
		{
			for (int x = 0; x < w - 1; ++x)
			{
				const size_t i = static_cast<size_t>(y) * w + x;
				const size_t ix = i + 1;
				const size_t iy = i + static_cast<size_t>(w);
				const double m = master[i], mx = master[ix], my = master[iy];
				const double r = received[i];
				const double o = output[i], ox = output[ix], oy = output[iy];
				const double vo = objSpeedPx[i], vc = camSpeedPx[i];
				if (!std::isfinite(m) || !std::isfinite(mx) || !std::isfinite(my) ||
				    !std::isfinite(r) || !std::isfinite(o) || !std::isfinite(ox) || !std::isfinite(oy) ||
				    !std::isfinite(vo) || !std::isfinite(vc) || vo < 0.0 || vc < 0.0)
				{
					++rep.nonFiniteCount;
					continue;
				}
				const double gm = (mx - m) * (mx - m) + (my - m) * (my - m);
				const double go = (ox - o) * (ox - o) + (oy - o) * (oy - o);
				const double eb = (r - m) * (r - m);
				const double eo = (o - m) * (o - m);

				Acc* target = nullptr;
				if (isCameraBg[i] != 0 && vc >= 0.25 && vo < 0.25)
				{
					target = &panAcc;
				}
				else
				{
					const size_t binIdx = static_cast<size_t>(ClassifyObjectSpeed(vo));
					target = &objAcc[binIdx];
				}
				++target->n;
				target->gMaster += gm;
				target->gOut += go;
				target->errBase += eb;
				target->errOut += eo;
			}
		}

		if (rep.nonFiniteCount > 0)
		{
			rep.valid = false;
			return rep;
		}

		auto finalize = [peakCode](const Acc& a, SpeedBinRow& row)
		{
			row.pixelCount = a.n;
			if (a.n == 0)
			{
				row.valid = false;
				return;
			}
			const double invN = 1.0 / static_cast<double>(a.n);
			row.masterGradEnergy = a.gMaster * invN;
			row.outputGradEnergy = a.gOut * invN;
			row.sharpnessRetention = (row.masterGradEnergy > kMetricNumFloor)
				? std::sqrt(row.outputGradEnergy / row.masterGradEnergy)
				: 1.0;
			double dbB = 0.0, dbO = 0.0;
			if (ComputePsnrDb(a.errBase * invN, peakCode, dbB) &&
			    ComputePsnrDb(a.errOut * invN, peakCode, dbO))
			{
				row.deltaDb = dbO - dbB;
				row.valid = true;
			}
		};

		finalize(panAcc, rep.cameraPan);
		bool hasAnyObj = false;
		for (size_t b = 0; b < objAcc.size(); ++b)
		{
			finalize(objAcc[b], rep.objectBins[b]);
			if (rep.objectBins[b].valid)
			{
				if (!hasAnyObj || rep.objectBins[b].sharpnessRetention < rep.worstObjectRetention)
				{
					rep.worstObjectRetention = rep.objectBins[b].sharpnessRetention;
					rep.worstObjectBin = static_cast<ObjectSpeedBinId>(b);
					hasAnyObj = true;
				}
			}
		}
		rep.valid = rep.cameraPan.valid || hasAnyObj;
		return rep;
	}

	// ---- Authoritative Synthetic Grain-Transfer Evaluator (refs/111 s7.3) ----
	// Regresses output residual (output - masterClean) onto (ownGrain, foreignGrain):
	//   output - masterClean ~= betaOwn * ownGrain + betaForeign * foreignGrain
	// Also checks flat-tile std ratio and lag-1 temporal correlation rise when prevResidual is provided.
	struct GrainTransferReport
	{
		double betaOwn = 0.0;
		double betaForeign = 0.0;
		double stdRatio = 0.0;       // std(output - masterClean) / std(ownGrain)
		double lag1Corr = 0.0;       // temporal lag-1 correlation vs previous frame's residual
		double lag1Baseline = 0.0;   // baseline lag-1 correlation of pure independent grain
		double lag1Rise = 0.0;
		size_t nonFiniteCount = 0;
		bool lag1Evaluated = false;  // True ONLY when prevOutputResidual & prevOwnGrain were supplied and finite lag-1 evaluated
		bool valid = false;
		bool passesAuthoritativeBar = false;
	};

	inline GrainTransferReport EvaluateGrainTransfer(
		const double* masterClean,
		const double* ownGrain,
		const double* foreignGrain,
		const double* output,
		size_t n,
		const double* prevOutputResidual = nullptr,
		const double* prevOwnGrain = nullptr)
	{
		GrainTransferReport rep;
		if (masterClean == nullptr || ownGrain == nullptr || foreignGrain == nullptr || output == nullptr || n == 0)
			return rep;

		// Centered means in FP64.
		double meanR = 0.0, meanG0 = 0.0, meanGf = 0.0;
		for (size_t i = 0; i < n; ++i)
		{
			const double m = masterClean[i], g0 = ownGrain[i], gf = foreignGrain[i], o = output[i];
			if (!std::isfinite(m) || !std::isfinite(g0) || !std::isfinite(gf) || !std::isfinite(o))
			{
				++rep.nonFiniteCount;
				continue;
			}
			meanR += (o - m);
			meanG0 += g0;
			meanGf += gf;
		}
		if (rep.nonFiniteCount > 0)
		{
			rep.valid = false;
			return rep;
		}

		const double invN = 1.0 / static_cast<double>(n);
		meanR *= invN;
		meanG0 *= invN;
		meanGf *= invN;

		// 2x2 normal equations for [ownGrain, foreignGrain] -> residual.
		double s00 = 0.0, s01 = 0.0, s11 = 0.0;
		double b0 = 0.0, b1 = 0.0;
		double varR = 0.0;
		for (size_t i = 0; i < n; ++i)
		{
			const double r = (output[i] - masterClean[i]) - meanR;
			const double g0 = ownGrain[i] - meanG0;
			const double gf = foreignGrain[i] - meanGf;
			s00 += g0 * g0;
			s01 += g0 * gf;
			s11 += gf * gf;
			b0 += g0 * r;
			b1 += gf * r;
			varR += r * r;
		}

		const double det = s00 * s11 - s01 * s01;
		if (s00 <= kMetricNumFloor || s11 <= kMetricNumFloor || std::fabs(det) <= kMetricNumFloor * s00 * s11)
		{
			rep.valid = false;
			return rep;
		}

		rep.betaOwn = (s11 * b0 - s01 * b1) / det;
		rep.betaForeign = (-s01 * b0 + s00 * b1) / det;
		rep.stdRatio = std::sqrt(varR / s00);

		if (prevOutputResidual != nullptr && prevOwnGrain != nullptr)
		{
			double mPrevR = 0.0, mPrevG = 0.0;
			for (size_t i = 0; i < n; ++i)
			{
				if (!std::isfinite(prevOutputResidual[i]) || !std::isfinite(prevOwnGrain[i]))
				{
					++rep.nonFiniteCount;
					rep.valid = false;
					return rep;
				}
				mPrevR += prevOutputResidual[i];
				mPrevG += prevOwnGrain[i];
			}
			mPrevR *= invN;
			mPrevG *= invN;
			double covOut = 0.0, varPrevR = 0.0;
			double covBase = 0.0, varPrevG = 0.0;
			for (size_t i = 0; i < n; ++i)
			{
				const double r = (output[i] - masterClean[i]) - meanR;
				const double pr = prevOutputResidual[i] - mPrevR;
				const double g0 = ownGrain[i] - meanG0;
				const double pg = prevOwnGrain[i] - mPrevG;
				covOut += r * pr;
				varPrevR += pr * pr;
				covBase += g0 * pg;
				varPrevG += pg * pg;
			}
			if (varR > kMetricNumFloor && varPrevR > kMetricNumFloor &&
			    s00 > kMetricNumFloor && varPrevG > kMetricNumFloor)
			{
				rep.lag1Corr = covOut / std::sqrt(varR * varPrevR);
				rep.lag1Baseline = covBase / std::sqrt(s00 * varPrevG);
				rep.lag1Rise = rep.lag1Corr - rep.lag1Baseline;
				rep.lag1Evaluated = std::isfinite(rep.lag1Corr)
					&& std::isfinite(rep.lag1Baseline)
					&& std::isfinite(rep.lag1Rise);
			}
		}

		rep.valid = std::isfinite(rep.betaOwn) && std::isfinite(rep.betaForeign) && std::isfinite(rep.stdRatio);
		rep.passesAuthoritativeBar = rep.valid
			&& rep.lag1Evaluated
			&& (rep.betaOwn >= kGrainBarMinBetaOwn)
			&& (std::fabs(rep.betaForeign) <= kGrainBarMaxBetaForeign)
			&& (std::fabs(rep.stdRatio - 1.0) <= kGrainBarMaxRmsError)
			&& (rep.lag1Rise <= kGrainBarMaxLag1Rise);
		return rep;
	}
}
