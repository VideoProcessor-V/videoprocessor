// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// Temporal Detail v2 — Stage TD-S2 (2026-11-23_TD-consolidated-plan.md s2.1 D10/D11 & s3.2):
//   1. Picture-only P15 / P15c frame-role detector filling the exact landed 64-byte QualityPattern::Record
//      (ZERO bitstream/encoder side data; explicit abstention reasons & counters on cuts/fades/flat rhythm).
//   2. Visibly labelled P16 block-grid stand-in (TdV2CodingGridStandIn behind ICodingGridSource interface,
//      always engaging StandInKind::P16BlockGrid in StandInRegistry so stand-in usage is NEVER silent).
//
// Every constant is derived from signal/quantization geometry or existing VP-V contracts (Rule 1).
// Uses U24 RobustStats for quantiles/MAD and U25 RampMath for clamping.
// Header-only, C++14/17, no Windows headers.
#pragma once

#include <VP-V/core/Ramp.h>
#include <VP-V/core/RobustStats.h>
#include <VP-V/modules/input/source_analysis/identity/quality_pattern/QualityPatternRecord.h>
#include <VP-V/modules/repair/grain_split/shared/BandFromBase.h>
#include <VP-V/modules/repair/temporal_detail/v2/TdV2BlendMath.h>
#include <VP-V/modules/repair/temporal_detail/v2/TdV2Config.h>
#include <VP-V/modules/repair/temporal_detail/v2/TdV2StandIns.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <vector>

namespace TdV2
{
	// Strictly numerical floor for ratio/variance denominators (Rule 1).
	constexpr double kRoleNumFloor = 1e-9;

	// Exact-copy cell threshold: half an integer code step at the frame's source bit depth
	// (exact rounding limit of integer-quantized identical frames: |a - b| <= 0.5 * DepthScale).
	constexpr double kExactCopyHalfStep10Bit = 0.5;

	// Full-frame exact-copy cadence fraction: at least 15/16 (= 0.9375) of active cells identical to N-1 or N-2,
	// allowing up to 1/16 edge/overlay border cells without breaking 3:2 pulldown repeat detection.
	constexpr double kFullFrameCopyFrac = 15.0 / 16.0;

	// Standard 8x8 transform block stride inside the canonical 16x16 P16 cell (QualityPattern::kBlockGridCellEdge).
	constexpr int kTransformSubCellPx = 8;

	// Q8.8 fixed-point scale (2^8 = 256) matching QualityPattern::Record stepMean/stepP90/stepMax layout.
	constexpr double kQ88Scale = 256.0;

	// Exact squared coefficient of variation Var(|X|) / E[|X|]^2 = (pi / 2) - 1 of a half-normal random variable
	// (used to derive the 3-sigma sampling-noise margin for P16 8-px boundary-ratio period detection).
	constexpr double kHalfNormalCvSq = 0.5 * 3.14159265358979323846 - 1.0;

	// 3-sigma Gaussian tail multiplier (P(z > 3) = 0.135% one-sided; consistent with TdV2BlendMath kPixelBoundSigma).
	constexpr double kBlockingRatioSigma = 3.0;

	// ---- Visible P16 Block-Grid Stand-In Interface (1-line swap when GPU kCodingEvidence grid lands) ----
	struct CodingGridCell
	{
		double stepDelta = 0.0;      // Estimated local quantization step from 8/16-px boundary discontinuities
		double boundaryRatio = 1.0;  // Boundary step / interior gradient ratio (> 1 indicates block edges)
		bool copyPrev1 = false;      // Cell identical to frame N-1 within rounding floor
		bool copyPrev2 = false;      // Cell identical to frame N-2 within rounding floor
	};

	struct CodingGridSummary
	{
		int cellsW = 0;
		int cellsH = 0;
		std::vector<CodingGridCell> cells;

		double stepMean = 0.0;
		double stepP90 = 0.0;
		double stepMax = 0.0;
		double exactCopyFracPrev1 = 0.0;
		double exactCopyFracPrev2 = 0.0;
		double pictureScore = 0.0;   // 0..1 frame quality proxy (1 = zero block-edge step)
		uint16_t detectedPeriodX = 0;
		uint16_t detectedPeriodY = 0;
		bool valid = false;
	};

	// Abstract 1-line swap interface for P16 block-grid evidence (plan s2.1 D11).
	struct ICodingGridSource
	{
		virtual ~ICodingGridSource() = default;
		virtual CodingGridSummary Measure(
			const double* curY,
			const double* prev1Y,
			const double* prev2Y,
			int w,
			int h,
			int sourceBits,
			StandInRegistry* standIns) const = 0;
	};

	// Visible CPU P16 block-grid stand-in: computes per-16x16 cell 8-px boundary discontinuities and
	// exact-copy flags against N-1 / N-2, always engaging StandInKind::P16BlockGrid in StandInRegistry.
	struct TdV2CodingGridStandIn : public ICodingGridSource
	{
		CodingGridSummary Measure(
			const double* curY,
			const double* prev1Y,
			const double* prev2Y,
			int w,
			int h,
			int sourceBits,
			StandInRegistry* standIns) const override
		{
			if (standIns != nullptr)
				standIns->Engage(StandInKind::P16BlockGrid, "CPU 16x16 stand-in");

			CodingGridSummary out;
			if (curY == nullptr || w <= 0 || h <= 0)
				return out;
			if (!(PeakCodeForBits(sourceBits) > 0.0))
			{
				if (standIns != nullptr)
					standIns->Engage(StandInKind::NonFiniteClosure, "TdV2CodingGridStandIn invalid sourceBits");
				return out;
			}

			const int cellEdge = static_cast<int>(QualityPattern::kBlockGridCellEdge);
			out.cellsW = static_cast<int>(QualityPattern::BlockGridCellsX(static_cast<uint32_t>(w)));
			out.cellsH = static_cast<int>(QualityPattern::BlockGridCellsY(static_cast<uint32_t>(h)));
			const size_t numCells = static_cast<size_t>(out.cellsW) * static_cast<size_t>(out.cellsH);
			out.cells.resize(numCells);

			const double depthScale = DepthScale(sourceBits);
			const double copyTol = kExactCopyHalfStep10Bit * depthScale;

			std::vector<double> cellSteps;
			cellSteps.reserve(numCells);
			double sumStep = 0.0;
			double maxStep = 0.0;
			size_t copy1Count = 0;
			size_t copy2Count = 0;
			double totalBoundXSum = 0.0, totalInterXSum = 0.0;
			size_t totalBoundXCnt = 0, totalInterXCnt = 0;
			double totalBoundYSum = 0.0, totalInterYSum = 0.0;
			size_t totalBoundYCnt = 0, totalInterYCnt = 0;

			for (int cy = 0; cy < out.cellsH; ++cy)
			{
				const int y0 = cy * cellEdge;
				const int y1 = (std::min)(h, y0 + cellEdge);
				for (int cx = 0; cx < out.cellsW; ++cx)
				{
					const int x0 = cx * cellEdge;
					const int x1 = (std::min)(w, x0 + cellEdge);

					double boundXSum = 0.0, interXSum = 0.0;
					size_t boundXCnt = 0, interXCnt = 0;
					double boundYSum = 0.0, interYSum = 0.0;
					size_t boundYCnt = 0, interYCnt = 0;

					double copyDiffPrev1 = 0.0, copyDiffPrev2 = 0.0;
					size_t pxCnt = 0;
					bool cellNonFinite = false;

					for (int y = y0; y < y1; ++y)
					{
						for (int x = x0; x < x1; ++x)
						{
							const size_t idx = static_cast<size_t>(y) * w + x;
							const double v = curY[idx];
							if (!std::isfinite(v))
							{
								cellNonFinite = true;
								continue;
							}
							++pxCnt;
							if (prev1Y != nullptr)
							{
								const double p1 = prev1Y[idx];
								if (std::isfinite(p1))
									copyDiffPrev1 += std::fabs(v - p1);
								else
									cellNonFinite = true;
							}
							if (prev2Y != nullptr)
							{
								const double p2 = prev2Y[idx];
								if (std::isfinite(p2))
									copyDiffPrev2 += std::fabs(v - p2);
								else
									cellNonFinite = true;
							}

							if (x > 0)
							{
								const double vl = curY[idx - 1];
								if (std::isfinite(vl))
								{
									const double d = std::fabs(v - vl);
									if ((x % kTransformSubCellPx) == 0)
									{
										boundXSum += d;
										++boundXCnt;
									}
									else
									{
										interXSum += d;
										++interXCnt;
									}
								}
							}
							if (y > 0)
							{
								const double vu = curY[idx - static_cast<size_t>(w)];
								if (std::isfinite(vu))
								{
									const double d = std::fabs(v - vu);
									if ((y % kTransformSubCellPx) == 0)
									{
										boundYSum += d;
										++boundYCnt;
									}
									else
									{
										interYSum += d;
										++interYCnt;
									}
								}
							}
						}
					}

					if (cellNonFinite)
					{
						if (standIns != nullptr)
							standIns->Engage(StandInKind::NonFiniteClosure, "TdV2CodingGridStandIn non-finite pixel");
						out.valid = false;
						return out;
					}

					CodingGridCell& cell = out.cells[static_cast<size_t>(cy) * out.cellsW + cx];
					const double bx = boundXCnt > 0 ? (boundXSum / static_cast<double>(boundXCnt)) : 0.0;
					const double ix = interXCnt > 0 ? (interXSum / static_cast<double>(interXCnt)) : 0.0;
					const double by = boundYCnt > 0 ? (boundYSum / static_cast<double>(boundYCnt)) : 0.0;
					const double iy = interYCnt > 0 ? (interYSum / static_cast<double>(interYCnt)) : 0.0;

					// Excess discontinuity at 8-px transform boundaries over interior gradients estimates step size.
					const double excessX = (bx > ix) ? (bx - ix) : 0.0;
					const double excessY = (by > iy) ? (by - iy) : 0.0;
					cell.stepDelta = 0.5 * (excessX + excessY);

					const double rx = (boundXCnt > 0) ? (bx / (std::max)(ix, kRoleNumFloor)) : 1.0;
					const double ry = (boundYCnt > 0) ? (by / (std::max)(iy, kRoleNumFloor)) : 1.0;
					cell.boundaryRatio = 0.5 * (rx + ry);
					// Exact cell-wide pair-average activity gate: every pair has equal weight 1/(N_B + N_I),
					// so by exact exchangeability E[bx | cellMean > copyTol] == E[ix | cellMean > copyTol]
					// with zero left-tail truncation bias on i.i.d. white noise while flat cells stay excluded.
					if (boundXCnt > 0 && interXCnt > 0 &&
					    (boundXSum + interXSum) / static_cast<double>(boundXCnt + interXCnt) > copyTol)
					{
						totalBoundXSum += boundXSum;
						totalBoundXCnt += boundXCnt;
						totalInterXSum += interXSum;
						totalInterXCnt += interXCnt;
					}
					if (boundYCnt > 0 && interYCnt > 0 &&
					    (boundYSum + interYSum) / static_cast<double>(boundYCnt + interYCnt) > copyTol)
					{
						totalBoundYSum += boundYSum;
						totalBoundYCnt += boundYCnt;
						totalInterYSum += interYSum;
						totalInterYCnt += interYCnt;
					}

					if (pxCnt > 0)
					{
						if (prev1Y != nullptr && (copyDiffPrev1 / static_cast<double>(pxCnt)) <= copyTol)
						{
							cell.copyPrev1 = true;
							++copy1Count;
						}
						if (prev2Y != nullptr && (copyDiffPrev2 / static_cast<double>(pxCnt)) <= copyTol)
						{
							cell.copyPrev2 = true;
							++copy2Count;
						}
					}

					cellSteps.push_back(cell.stepDelta);
					sumStep += cell.stepDelta;
					if (cell.stepDelta > maxStep)
						maxStep = cell.stepDelta;
				}
			}

			if (numCells > 0)
			{
				const double invCells = 1.0 / static_cast<double>(numCells);
				out.stepMean = sumStep * invCells;
				out.stepP90 = RobustStats::Quantile(cellSteps, 0.90, RobustStats::RankRule::NearestRank);
				out.stepMax = maxStep;
				out.exactCopyFracPrev1 = static_cast<double>(copy1Count) * invCells;
				out.exactCopyFracPrev2 = static_cast<double>(copy2Count) * invCells;
				// Smooth monotonic quality score in (0, 1]: 1.0 when zero block-edge step, decreasing with step/depthScale.
				out.pictureScore = RampMath::Clamp01(1.0 / (1.0 + out.stepMean / (std::max)(depthScale, kRoleNumFloor)));
				// Period detection with derived 3-sigma half-normal sampling-noise margin:
				// Under stationary iid white noise (zero blocking), pooled mean ratio B_bar / I_bar has standard error
				// sigma_R = sqrt((pi/2 - 1) * (1/N_B + 1/N_I)), so requiring ratio > 1 + 3*sigma_R suppresses false
				// blocking triggers on pure flat noise while cleanly detecting genuine 8-px block discontinuities.
				if (totalBoundXCnt > 0)
				{
					const double meanBx = totalBoundXSum / static_cast<double>(totalBoundXCnt);
					const double meanIx = totalInterXCnt > 0 ? (totalInterXSum / static_cast<double>(totalInterXCnt)) : 0.0;
					const double invNb = 1.0 / static_cast<double>(totalBoundXCnt);
					const double invNi = totalInterXCnt > 0 ? (1.0 / static_cast<double>(totalInterXCnt)) : 0.0;
					const double marginX = kBlockingRatioSigma * std::sqrt(kHalfNormalCvSq * (invNb + invNi));
					if ((meanBx / (std::max)(meanIx, kRoleNumFloor)) > (1.0 + marginX))
						out.detectedPeriodX = static_cast<uint16_t>(kTransformSubCellPx);
				}
				if (totalBoundYCnt > 0)
				{
					const double meanBy = totalBoundYSum / static_cast<double>(totalBoundYCnt);
					const double meanIy = totalInterYCnt > 0 ? (totalInterYSum / static_cast<double>(totalInterYCnt)) : 0.0;
					const double invNb = 1.0 / static_cast<double>(totalBoundYCnt);
					const double invNi = totalInterYCnt > 0 ? (1.0 / static_cast<double>(totalInterYCnt)) : 0.0;
					const double marginY = kBlockingRatioSigma * std::sqrt(kHalfNormalCvSq * (invNb + invNi));
					if ((meanBy / (std::max)(meanIy, kRoleNumFloor)) > (1.0 + marginY))
						out.detectedPeriodY = static_cast<uint16_t>(kTransformSubCellPx);
				}
				out.valid = true;
			}
			return out;
		}
	};

	// ---- TD-U1 (Owner D1(b)): Grid-Free Non-Block Picture-Only Coding-Step Estimator ----
	// Does NOT rely on x % 8 == 0 or y % 8 == 0 block boundaries (works on deblocked HEVC, AV1,
	// and rescaled 1080p->4K streams). Combines two derived quantization-physics cues:
	//   1) Flat-plateau staircase step-comb: plateau-to-plateau jump heights |I(p) - I(p-1)| or |I(p+1) - I(p-1)|
	//      bracketed by flat neighbors within half-LSB rounding resolution (0.5 * DepthScale).
	//   2) Scale-invariant fine-band dead-zone deficit: compares fine-band variance E_H = Var(I - G_fine(I))
	//      against the analytical 1/f step-edge reference E_M * (E_H_ref / E_M_ref) derived strictly from
	//      U2 BandFromBase sigmas (kSigmaFine, kSigmaBase), mapping missing fine variance through V0 = theta^2 / 3
	//      with theta = (1 - phi) * Delta -> Delta = sqrt(3 * missingVar / gains.fine) / (1 - phi).
	struct TdV2NonBlockStepEstimator : public ICodingGridSource
	{
		CodingGridSummary Measure(
			const double* curY,
			const double* prev1Y,
			const double* prev2Y,
			int w,
			int h,
			int sourceBits,
			StandInRegistry* standIns) const override
		{
			return MeasureWithOverlay(curY, prev1Y, prev2Y, w, h, sourceBits, nullptr, kCanonicalBlockPx, standIns);
		}

		// Grid-free monotonic flat-plateau staircase estimator (UPG-HIGH-4):
		// - Excludes Overlay cells and high-contrast scene/subtitle edges (|jump| > PeakCode / 16).
		// - Excludes 1-LSB native bit-depth rounding (|jump| <= 1.5 * depthScale) and smooth gradients.
		// - Requires >= 2 same-sign consistent (max/min <= 2.0) plateau-to-plateau steps along the SAME row or column
		//   (eliminating single isolated graphic edges and 2-tone boxes).
		// - Removes the invalid E_H/E_M band-ratio assumption so clean upscaled 1080p->4K / soft content reports 0.
		CodingGridSummary MeasureWithOverlay(
			const double* curY,
			const double* prev1Y,
			const double* prev2Y,
			int w,
			int h,
			int sourceBits,
			const uint8_t* overlayCells,
			int cellPx,
			StandInRegistry* standIns) const
		{
			CodingGridSummary out;
			if (curY == nullptr || w <= 0 || h <= 0)
				return out;
			const double peakCode = PeakCodeForBits(sourceBits);
			if (!(peakCode > 0.0))
			{
				if (standIns != nullptr)
					standIns->Engage(StandInKind::NonFiniteClosure, "TdV2NonBlockStepEstimator invalid sourceBits");
				return out;
			}

			const size_t nPx = static_cast<size_t>(w) * static_cast<size_t>(h);
			for (size_t i = 0; i < nPx; ++i)
			{
				if (!std::isfinite(curY[i]))
				{
					if (standIns != nullptr)
						standIns->Engage(StandInKind::NonFiniteClosure, "TdV2NonBlockStepEstimator non-finite pixel");
					return out;
				}
			}

			const int cellEdge = cellPx > 0 ? cellPx : static_cast<int>(QualityPattern::kBlockGridCellEdge);
			out.cellsW = (w + cellEdge - 1) / cellEdge;
			out.cellsH = (h + cellEdge - 1) / cellEdge;
			const size_t numCells = static_cast<size_t>(out.cellsW) * static_cast<size_t>(out.cellsH);
			out.cells.resize(numCells);

			const double depthScale = DepthScale(sourceBits);
			const double halfLsb = kExactCopyHalfStep10Bit * depthScale;
			// Strictly above 1-LSB native rounding and below 4-bit coarse-quantization ceiling (Peak/16 = 64 codes at 10-bit).
			const double minBandJump = 1.5 * depthScale;
			const double maxBandJump = peakCode / 16.0;

			std::vector<double> cellSteps;
			cellSteps.reserve(numCells);
			double sumStep = 0.0;
			double maxStep = 0.0;
			size_t copy1Count = 0;
			size_t copy2Count = 0;

			for (int cy = 0; cy < out.cellsH; ++cy)
			{
				const int y0 = cy * cellEdge;
				const int y1 = (std::min)(h, y0 + cellEdge);
				for (int cx = 0; cx < out.cellsW; ++cx)
				{
					const int x0 = cx * cellEdge;
					const int x1 = (std::min)(w, x0 + cellEdge);
					const size_t cIdx = static_cast<size_t>(cy) * static_cast<size_t>(out.cellsW) + static_cast<size_t>(cx);
					const bool isOverlay = (overlayCells != nullptr && overlayCells[cIdx] != 0u);

					double copyDiff1 = 0.0;
					double copyDiff2 = 0.0;
					size_t pxCnt = 0;
					bool cellNonFinite = false;

					for (int y = y0; y < y1; ++y)
					{
						for (int x = x0; x < x1; ++x)
						{
							const size_t idx = static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x);
							const double v = curY[idx];
							++pxCnt;
							if (prev1Y != nullptr)
							{
								if (std::isfinite(prev1Y[idx])) copyDiff1 += std::fabs(v - prev1Y[idx]);
								else cellNonFinite = true;
							}
							if (prev2Y != nullptr)
							{
								if (std::isfinite(prev2Y[idx])) copyDiff2 += std::fabs(v - prev2Y[idx]);
								else cellNonFinite = true;
							}
						}
					}

					if (cellNonFinite)
					{
						if (standIns != nullptr)
							standIns->Engage(StandInKind::NonFiniteClosure, "TdV2NonBlockStepEstimator non-finite prev pixel");
						out.valid = false;
						return out;
					}

					double cellJumpSum = 0.0;
					size_t cellJumpCnt = 0;
					if (!isOverlay)
					{
						// Horizontal scanlines inside cell: require >= 2 same-sign consistent 3-px-plateau steps per row.
						for (int y = y0; y < y1; ++y)
						{
							int posCnt = 0, negCnt = 0;
							double posSum = 0.0, posMin = maxBandJump, posMax = 0.0;
							double negSum = 0.0, negMin = maxBandJump, negMax = 0.0;
							const size_t rowBase = static_cast<size_t>(y) * static_cast<size_t>(w);
							for (int x = x0; x < x1; ++x)
							{
								if (x < 3 || x + 2 >= w)
									continue;
								const double vm3 = curY[rowBase + static_cast<size_t>(x - 3)];
								const double vm2 = curY[rowBase + static_cast<size_t>(x - 2)];
								const double vm1 = curY[rowBase + static_cast<size_t>(x - 1)];
								const double v0  = curY[rowBase + static_cast<size_t>(x)];
								const double vp1 = curY[rowBase + static_cast<size_t>(x + 1)];
								const double vp2 = curY[rowBase + static_cast<size_t>(x + 2)];
								const bool leftFlat = (std::fabs(vm1 - vm2) <= halfLsb && std::fabs(vm2 - vm3) <= halfLsb);
								if (!leftFlat)
									continue;
								double signedJump = 0.0;
								bool matched = false;
								if (std::fabs(vp1 - v0) <= halfLsb && std::fabs(vp2 - vp1) <= halfLsb)
								{
									signedJump = v0 - vm1;
									matched = true;
								}
								else if (x + 3 < w)
								{
									const double vp3 = curY[rowBase + static_cast<size_t>(x + 3)];
									const bool monoMid = ((v0 - vm1) * (vp1 - v0) >= 0.0);
									if (monoMid && std::fabs(vp2 - vp1) <= halfLsb && std::fabs(vp3 - vp2) <= halfLsb)
									{
										signedJump = vp1 - vm1;
										matched = true;
									}
								}
								if (!matched)
									continue;
								const double aj = std::fabs(signedJump);
								if (aj > minBandJump && aj <= maxBandJump)
								{
									if (signedJump > 0.0)
									{
										++posCnt;
										posSum += aj;
										if (aj < posMin) posMin = aj;
										if (aj > posMax) posMax = aj;
									}
									else
									{
										++negCnt;
										negSum += aj;
										if (aj < negMin) negMin = aj;
										if (aj > negMax) negMax = aj;
									}
								}
							}
							if (posCnt >= 2 && posMax <= 2.0 * posMin)
							{
								cellJumpSum += posSum;
								cellJumpCnt += static_cast<size_t>(posCnt);
							}
							if (negCnt >= 2 && negMax <= 2.0 * negMin)
							{
								cellJumpSum += negSum;
								cellJumpCnt += static_cast<size_t>(negCnt);
							}
						}

						// Vertical scanlines inside cell: require >= 2 same-sign consistent 3-px-plateau steps per column.
						const size_t stride = static_cast<size_t>(w);
						for (int x = x0; x < x1; ++x)
						{
							int posCnt = 0, negCnt = 0;
							double posSum = 0.0, posMin = maxBandJump, posMax = 0.0;
							double negSum = 0.0, negMin = maxBandJump, negMax = 0.0;
							for (int y = y0; y < y1; ++y)
							{
								if (y < 3 || y + 2 >= h)
									continue;
								const size_t idx = static_cast<size_t>(y) * stride + static_cast<size_t>(x);
								const double vm3 = curY[idx - 3 * stride];
								const double vm2 = curY[idx - 2 * stride];
								const double vm1 = curY[idx - stride];
								const double v0  = curY[idx];
								const double vp1 = curY[idx + stride];
								const double vp2 = curY[idx + 2 * stride];
								const bool leftFlat = (std::fabs(vm1 - vm2) <= halfLsb && std::fabs(vm2 - vm3) <= halfLsb);
								if (!leftFlat)
									continue;
								double signedJump = 0.0;
								bool matched = false;
								if (std::fabs(vp1 - v0) <= halfLsb && std::fabs(vp2 - vp1) <= halfLsb)
								{
									signedJump = v0 - vm1;
									matched = true;
								}
								else if (y + 3 < h)
								{
									const double vp3 = curY[idx + 3 * stride];
									const bool monoMid = ((v0 - vm1) * (vp1 - v0) >= 0.0);
									if (monoMid && std::fabs(vp2 - vp1) <= halfLsb && std::fabs(vp3 - vp2) <= halfLsb)
									{
										signedJump = vp1 - vm1;
										matched = true;
									}
								}
								if (!matched)
									continue;
								const double aj = std::fabs(signedJump);
								if (aj > minBandJump && aj <= maxBandJump)
								{
									if (signedJump > 0.0)
									{
										++posCnt;
										posSum += aj;
										if (aj < posMin) posMin = aj;
										if (aj > posMax) posMax = aj;
									}
									else
									{
										++negCnt;
										negSum += aj;
										if (aj < negMin) negMin = aj;
										if (aj > negMax) negMax = aj;
									}
								}
							}
							if (posCnt >= 2 && posMax <= 2.0 * posMin)
							{
								cellJumpSum += posSum;
								cellJumpCnt += static_cast<size_t>(posCnt);
							}
							if (negCnt >= 2 && negMax <= 2.0 * negMin)
							{
								cellJumpSum += negSum;
								cellJumpCnt += static_cast<size_t>(negCnt);
							}
						}
					}

					CodingGridCell& cell = out.cells[cIdx];
					const double stepEst = (cellJumpCnt > 0) ? (cellJumpSum / static_cast<double>(cellJumpCnt)) : 0.0;
					if (pxCnt > 0)
					{
						const double invCnt = 1.0 / static_cast<double>(pxCnt);
						if (prev1Y != nullptr && (copyDiff1 * invCnt) <= halfLsb)
						{
							cell.copyPrev1 = true;
							++copy1Count;
						}
						if (prev2Y != nullptr && (copyDiff2 * invCnt) <= halfLsb)
						{
							cell.copyPrev2 = true;
							++copy2Count;
						}
					}
					cell.stepDelta = stepEst;
					cell.boundaryRatio = 1.0; // Strictly grid-free: no 8-px boundary ratio claim
					cellSteps.push_back(stepEst);
					sumStep += stepEst;
					if (stepEst > maxStep)
						maxStep = stepEst;
				}
			}

			if (numCells > 0)
			{
				const double invCells = 1.0 / static_cast<double>(numCells);
				out.stepMean = sumStep * invCells;
				out.stepP90 = RobustStats::Quantile(cellSteps, 0.90, RobustStats::RankRule::NearestRank);
				out.stepMax = maxStep;
				out.exactCopyFracPrev1 = static_cast<double>(copy1Count) * invCells;
				out.exactCopyFracPrev2 = static_cast<double>(copy2Count) * invCells;
				out.pictureScore = RampMath::Clamp01(1.0 / (1.0 + out.stepMean / (std::max)(depthScale, kRoleNumFloor)));
				out.detectedPeriodX = 0;
				out.detectedPeriodY = 0;
				out.valid = true;
			}
			return out;
		}
	};

	// ---- TD-U1 (Plan Phase 1): Empirical Own-Error Prior A2e (U_j(class)) + Source-Seconds History Guard ----
	// Activity classes for A2e: 0 = flat, 1 = textured (separated strictly by 0-dB SNR vs 1-LSB quantization floor).
	constexpr int kA2eActivityClasses = 2;
	// Rolling history window in SOURCE SECONDS (never hardcoded frame counts, matching plan 99 s2.7 Frames(1 s)).
	constexpr double kA2eHistoryHorizonSec = 1.0;
	// Flash / missed-cut over-estimate cap ratio from Plan Phase 1 line 66 ("never more than 2x above").
	constexpr double kA2eOverEstimateCapFactor = 2.0;

	struct EmpiricalOwnErrorPrior
	{
		// U[band][activityClass]: robust median of grain-corrected c_IV over active cells
		// band: 0=L, 1=M, 2=H; activityClass: 0=flat, 1=textured.
		double U[3][kA2eActivityClasses] = {};
		uint32_t cellCount[3][kA2eActivityClasses] = {};
		bool valid = false;
		bool cappedByHistory = false;
		bool frozenOnDiscontinuity = false;
		bool heldFromHistory = false;

		double ForBandClass(int band, int activityClass) const
		{
			if (band < 0 || band >= 3 || activityClass < 0 || activityClass >= kA2eActivityClasses)
				return 0.0;
			const double v = U[band][activityClass];
			return (std::isfinite(v) && v > 0.0) ? v : 0.0;
		}

		double TotalForClass(int activityClass) const
		{
			return ForBandClass(0, activityClass) + ForBandClass(1, activityClass) + ForBandClass(2, activityClass);
		}
	};

	// Rolling time-domain (source seconds) history of recent same-role A2e priors so a single-frame flash,
	// strobe, or missed cut cannot inflate U_j and widen the output limit.
	class A2ePriorHistory
	{
	public:
		struct Entry
		{
			double timeSec = 0.0;
			QualityPattern::FrameRole role = QualityPattern::FrameRole::Unknown;
			EmpiricalOwnErrorPrior prior;
		};

		void Reset()
		{
			history_.clear();
			elapsedSec_ = 0.0;
			hasFrozenPrior_ = false;
			frozenPrior_ = EmpiricalOwnErrorPrior{};
		}

		// Cap `candidate` by 2x rolling median of recent same-role frames within kA2eHistoryHorizonSec.
		// On cut/fade/resync (`freezeDiscontinuity == true`), freezes history (does not push `candidate` and caps
		// against the last frozen prior if present).
		EmpiricalOwnErrorPrior ApplyCapAndRecord(
			const EmpiricalOwnErrorPrior& candidate,
			QualityPattern::FrameRole role,
			double sourceHz,
			bool freezeDiscontinuity,
			bool resetOnCut)
		{
			EmpiricalOwnErrorPrior out = candidate;
			const double hz = (std::isfinite(sourceHz) && sourceHz > 0.0) ? sourceHz : kDefaultSourceHz;
			const double dtSec = 1.0 / hz;

			if (resetOnCut)
			{
				Reset();
				out.frozenOnDiscontinuity = true;
				return out;
			}

			elapsedSec_ += dtSec;

			// Prune entries older than kA2eHistoryHorizonSec (strictly in source seconds).
			while (!history_.empty() && (elapsedSec_ - history_.front().timeSec) > kA2eHistoryHorizonSec)
				history_.pop_front();

			if (freezeDiscontinuity)
			{
				out.frozenOnDiscontinuity = true;
				if (!out.valid && hasFrozenPrior_ && frozenPrior_.valid)
				{
					out = frozenPrior_;
					out.frozenOnDiscontinuity = true;
					out.heldFromHistory = true;
					return out;
				}
				if (hasFrozenPrior_ && frozenPrior_.valid && out.valid)
				{
					for (int b = 0; b < 3; ++b)
					{
						for (int c = 0; c < kA2eActivityClasses; ++c)
						{
							const double refU = frozenPrior_.ForBandClass(b, c);
							if (refU > kRoleNumFloor && out.U[b][c] > kA2eOverEstimateCapFactor * refU)
							{
								out.U[b][c] = kA2eOverEstimateCapFactor * refU;
								out.cappedByHistory = true;
							}
						}
					}
				}
				return out;
			}

			// UPG-HIGH-5: On one-sided frames (K = 1, carry off) where candidate has no A1 pairs (`!out.valid`),
			// hold the rolling median of recent same-role frames (or all recent frames / frozenPrior_ within horizon)
			// so one-sided frames never collapse to 1-LSB! Do NOT push the held copy back into history_.
			if (!out.valid)
			{
				EmpiricalOwnErrorPrior held;
				bool anyHeld = false;
				for (int b = 0; b < 3; ++b)
				{
					for (int c = 0; c < kA2eActivityClasses; ++c)
					{
						std::vector<double> sameRoleVals;
						std::vector<double> anyRoleVals;
						for (const Entry& e : history_)
						{
							if (!e.prior.valid || e.prior.cellCount[b][c] == 0)
								continue;
							const double v = e.prior.ForBandClass(b, c);
							anyRoleVals.push_back(v);
							if (role != QualityPattern::FrameRole::Unknown &&
							    e.role != QualityPattern::FrameRole::Unknown &&
							    e.role == role)
							{
								sameRoleVals.push_back(v);
							}
						}
						const std::vector<double>& src = !sameRoleVals.empty() ? sameRoleVals : anyRoleVals;
						if (!src.empty())
						{
							held.U[b][c] = (std::max)(0.0, RobustStats::Median(src, RobustStats::MedianRule::MeanOfMiddle));
							held.cellCount[b][c] = static_cast<uint32_t>(src.size());
							anyHeld = true;
						}
					}
				}
				if (anyHeld)
				{
					held.valid = true;
					held.heldFromHistory = true;
					return held;
				}
				if (hasFrozenPrior_ && frozenPrior_.valid && !history_.empty())
				{
					held = frozenPrior_;
					held.heldFromHistory = true;
					return held;
				}
				return out;
			}

			// Gather same-role recent entries (or all entries if role == Unknown) for robust median cap.
			for (int b = 0; b < 3; ++b)
			{
				for (int c = 0; c < kA2eActivityClasses; ++c)
				{
					std::vector<double> vals;
					vals.reserve(history_.size());
					for (const Entry& e : history_)
					{
						if (role != QualityPattern::FrameRole::Unknown &&
						    e.role != QualityPattern::FrameRole::Unknown &&
						    e.role != role)
						{
							continue;
						}
						if (e.prior.valid && e.prior.cellCount[b][c] > 0)
							vals.push_back(e.prior.ForBandClass(b, c));
					}
					if (vals.size() >= 2u)
					{
						const double med = RobustStats::Median(vals, RobustStats::MedianRule::MeanOfMiddle);
						const double cap = kA2eOverEstimateCapFactor * med;
						if (med > kRoleNumFloor && out.U[b][c] > cap)
						{
							out.U[b][c] = cap;
							out.cappedByHistory = true;
						}
					}
				}
			}

			history_.push_back(Entry{ elapsedSec_, role, out });
			frozenPrior_ = out;
			hasFrozenPrior_ = true;
			return out;
		}

		size_t Size() const { return history_.size(); }

	private:
		std::deque<Entry> history_;
		double elapsedSec_ = 0.0;
		bool hasFrozenPrior_ = false;
		EmpiricalOwnErrorPrior frozenPrior_;
	};

	// ---- TD-U1 (Plan Phase 1): Motion-Aligned Relative Sharpness Along Motion ----
	// For each adjacent pair (t, t-1 warped onto t), on cells with trusted motion, non-focus-flagged,
	// and texture above the 0-dB 1-LSB quantization noise floor:
	//   E_t        = max(0, E_M(t) + E_H(t) - retainedGrainVar * (gains.mid + gains.fine))
	//   E'_(t-1)   = max(0, E_M(warped t-1) + E_H(warped t-1) / max(beta_HP(vx,vy), eps) - prevGrainLeak)
	//   r_t        = median_trusted( ln(E_t / E'_(t-1)) )
	// Optional symmetric mode: if reverse pair (E'_t warped onto t-1 vs E_(t-1)) is also supplied,
	//   r_t = 0.5 * (median ln(E_t / E'_(t-1)) - median ln(E_(t-1) / E'_t)), cancelling warp attenuation identically.
	struct AlignedSharpnessSummary
	{
		double relativeSharpnessR = 0.0;    // r_t = median_trusted ln(E_t / E'_{t-1})
		double exactCopyFracPrev1 = 0.0;
		double exactCopyFracPrev2 = 0.0;
		size_t trustedCellCount = 0;
		size_t focusAbstainedCells = 0;
		bool valid = false;
	};

	inline AlignedSharpnessSummary MeasureAlignedSharpnessFromCellEnergies(
		const std::vector<double>& curMidEnergy,
		const std::vector<double>& curFineEnergy,
		const std::vector<double>& prevWarpedMidEnergy,
		const std::vector<double>& prevWarpedFineEnergy,
		const std::vector<double>& motionVx,
		const std::vector<double>& motionVy,
		const std::vector<uint8_t>& motionValid,
		const std::vector<uint8_t>* focusFlaggedCells,
		const std::vector<double>* retainedGrainVar,
		int sourceBits,
		double exactCopyFracPrev1 = 0.0,
		double exactCopyFracPrev2 = 0.0,
		const std::vector<double>* revPrevMidEnergy = nullptr,
		const std::vector<double>* revPrevFineEnergy = nullptr,
		const std::vector<double>* revCurWarpedMidEnergy = nullptr,
		const std::vector<double>* revCurWarpedFineEnergy = nullptr,
		StandInRegistry* standIns = nullptr)
	{
		AlignedSharpnessSummary out;
		out.exactCopyFracPrev1 = exactCopyFracPrev1;
		out.exactCopyFracPrev2 = exactCopyFracPrev2;
		const size_t n = curMidEnergy.size();
		if (n == 0 || curFineEnergy.size() != n ||
		    prevWarpedMidEnergy.size() != n || prevWarpedFineEnergy.size() != n ||
		    motionVx.size() != n || motionVy.size() != n || motionValid.size() != n ||
		    !(PeakCodeForBits(sourceBits) > 0.0))
		{
			return out;
		}

		const BandWhiteNoiseGains gains = ComputeBandWhiteNoiseGains();
		const double depthScale = DepthScale(sourceBits);
		const double baseQuantVar = (depthScale * depthScale) / 12.0;
		// Exact 0-dB SNR texture floor in the (M + H) band domain against 1-LSB quantization noise.
		const double textureFloor = 2.0 * (gains.mid + gains.fine) * baseQuantVar;

		const bool hasSymmetric = (revPrevMidEnergy != nullptr && revPrevFineEnergy != nullptr &&
		                           revCurWarpedMidEnergy != nullptr && revCurWarpedFineEnergy != nullptr &&
		                           revPrevMidEnergy->size() == n && revPrevFineEnergy->size() == n &&
		                           revCurWarpedMidEnergy->size() == n && revCurWarpedFineEnergy->size() == n);

		std::vector<double> logRatios;
		logRatios.reserve(n);
		for (size_t i = 0; i < n; ++i)
		{
			if (motionValid[i] == 0u)
				continue;
			if (focusFlaggedCells != nullptr && i < focusFlaggedCells->size() && (*focusFlaggedCells)[i] != 0u)
			{
				++out.focusAbstainedCells;
				continue;
			}
			const double cm = curMidEnergy[i];
			const double ch = curFineEnergy[i];
			const double pm = prevWarpedMidEnergy[i];
			const double ph = prevWarpedFineEnergy[i];
			const double vx = motionVx[i];
			const double vy = motionVy[i];
			if (!std::isfinite(cm) || !std::isfinite(ch) || !std::isfinite(pm) || !std::isfinite(ph) ||
			    !std::isfinite(vx) || !std::isfinite(vy))
			{
				if (standIns != nullptr)
					standIns->Engage(StandInKind::NonFiniteClosure, "MeasureAlignedSharpness non-finite cell energy");
				continue;
			}

			double grainLeakM = 0.0;
			double grainLeakH = 0.0;
			if (retainedGrainVar != nullptr && i < retainedGrainVar->size())
			{
				const double gv = (*retainedGrainVar)[i];
				if (std::isfinite(gv) && gv > 0.0)
				{
					grainLeakM = gv * gains.mid;
					grainLeakH = gv * gains.fine;
				}
			}

			const double eCurM = (std::max)(0.0, cm - grainLeakM);
			const double eCurH = (std::max)(0.0, ch - grainLeakH);
			const double eCur = eCurM + eCurH;

			const double betaHp = (std::max)(FineBandBicubicWarpVarianceGain(vx, vy), kRoleNumFloor);
			const double ePrevM = (std::max)(0.0, pm - grainLeakM);
			// Debias bicubic sub-pixel H-band warp attenuation so fractional pans do not bias r_t.
			const double ePrevH = (std::max)(0.0, (ph / betaHp) - grainLeakH);
			const double ePrev = ePrevM + ePrevH;

			if (eCur <= textureFloor || ePrev <= textureFloor)
				continue;

			double lr = std::log(eCur / ePrev);
			if (hasSymmetric)
			{
				const double rpm = (*revPrevMidEnergy)[i];
				const double rph = (*revPrevFineEnergy)[i];
				const double rcm = (*revCurWarpedMidEnergy)[i];
				const double rch = (*revCurWarpedFineEnergy)[i];
				if (std::isfinite(rpm) && std::isfinite(rph) && std::isfinite(rcm) && std::isfinite(rch))
				{
					const double eRevPrev = (std::max)(0.0, rpm - grainLeakM) + (std::max)(0.0, rph - grainLeakH);
					const double eRevCur = (std::max)(0.0, rcm - grainLeakM) + (std::max)(0.0, (rch / betaHp) - grainLeakH);
					if (eRevPrev > textureFloor && eRevCur > textureFloor)
					{
						// Symmetric average cancels residual kernel-shape attenuation identically.
						lr = 0.5 * (lr - std::log(eRevPrev / eRevCur));
					}
				}
			}

			if (std::isfinite(lr))
				logRatios.push_back(lr);
		}

		out.trustedCellCount = logRatios.size();
		if (!logRatios.empty())
		{
			out.relativeSharpnessR = RobustStats::Median(logRatios, RobustStats::MedianRule::MeanOfMiddle);
			out.valid = std::isfinite(out.relativeSharpnessR);
		}
		return out;
	}

	// =========================================================================
	// Pass 3B — Forensic Blind Keyframe / Peak Detector (TD-GOP-PEAK-V2)
	// Pure physics/quantization math, zero bitstream side data, zero neural net.
	// Every signal is bit-depth scale-invariant (10-bit == 12-bit via DepthScale)
	// and independently toggleable (strictly default OFF at enableForensicGopPeakV2).
	// =========================================================================

	struct ForensicPeakV2Config
	{
		bool enableForensicGopPeakV2 = false;
		bool enableSkipGrainFreeze = true;
		bool enableAcSparsity = true;
		bool enableInnovationBidir = true;
		bool enableCadenceLock = true;

		static ForensicPeakV2Config FromConfig(const Config& cfg)
		{
			ForensicPeakV2Config c;
			c.enableForensicGopPeakV2 = cfg.enableForensicGopPeakV2;
			c.enableSkipGrainFreeze = cfg.enablePeakV2SkipGrainFreeze;
			c.enableAcSparsity = cfg.enablePeakV2AcSparsity;
			c.enableInnovationBidir = cfg.enablePeakV2InnovationBidir;
			c.enableCadenceLock = cfg.enablePeakV2CadenceLock;
			return c;
		}
	};

	struct ForensicFrameSignals
	{
		// Signal 1: SKIP-block / temporal grain-freeze fraction in [0, 1].
		// High -> deep B-frame freezing film grain; Low -> fresh grain / Peak.
		double skipGrainFreezeFrac = 0.0;
		size_t grainActiveCells = 0;
		bool hasSkipGrainFreeze = false;

		// Signal 2: Grain-free 8x8 AC transform sparsity pi_0 in [0, 1].
		// Immune to HEVC/AV1 in-loop deblocking + SAO/CDEF; tracks relative QP directly.
		double acSparsityPi0 = 0.0;
		size_t texturedAcBlocks = 0;
		bool hasAcSparsity = false;

		// Signal 3a: Motion-compensated grain-free innovation reset (median cell RMS / quantSigma).
		// Signal 3b: Bidirectional clean-band prediction asymmetry ratio in [0, 1]:
		//   |X_clean(t) - 0.5*(warp(t-1) + warp(t+1))| / (0.5*(|X_clean(t) - warp(t-1)| + |X_clean(t) - warp(t+1)|)).
		double innovationResetRatio = 0.0;
		double bidirAsymmetryRatio = 0.0;
		size_t innovationCells = 0;
		bool hasInnovation = false;
		bool hasBidirAsymmetry = false;

		// Composite forensic peak score in [0, 1] (equal-weight average over active valid cues).
		double compositePeakScore = 0.5;
		int activeCueCount = 0;
		bool valid = false;
	};

	// Signal 1: SKIP-block / temporal grain-freeze fraction.
	// Physics derivation (Rule 1):
	//   For two frames with independent photochemical grain of variance sigma_g^2 (from P10g),
	//   Var(g_t - g_{t-1}) = 2 * sigma_g^2. On a SKIP/MERGE block where the encoder froze the
	//   residual to zero, temporal diff variance collapses below the exact -3 dB half-power
	//   point: Var_cell(Y_t - warp(Y_{t-1})) <= sigma_g^2 = 0.5 * (2 * sigma_g^2).
	//   Only evaluates active cells where sigma_g^2 > baseQuantVar (grain exceeds 1-LSB noise).
	inline bool MeasureSkipGrainFreezeFraction(
		const double* curRawY,
		const double* prevWarpedRawY,
		int w,
		int h,
		const double* cellGrainVarP10g,
		double scalarGrainVar,
		int cellPx,
		int sourceBits,
		double& outFreezeFrac,
		size_t& outGrainActiveCells,
		const ActiveRect* activeRect = nullptr,
		const uint8_t* trustedCellMask = nullptr)
	{
		outFreezeFrac = 0.0;
		outGrainActiveCells = 0;
		if (curRawY == nullptr || prevWarpedRawY == nullptr || w <= 0 || h <= 0)
			return false;
		if (!(PeakCodeForBits(sourceBits) > 0.0))
			return false;

		const int step = cellPx > 0 ? cellPx : kCanonicalBlockPx;
		const int cellsW = (w + step - 1) / step;
		const int cellsH = (h + step - 1) / step;
		const double dScale = DepthScale(sourceBits);
		const double baseQuantVar = (dScale * dScale) / 12.0;

		const int rx0 = (activeRect != nullptr && activeRect->IsValid(w, h)) ? activeRect->x0 : 0;
		const int ry0 = (activeRect != nullptr && activeRect->IsValid(w, h)) ? activeRect->y0 : 0;
		const int rx1 = (activeRect != nullptr && activeRect->IsValid(w, h)) ? activeRect->x1 : w;
		const int ry1 = (activeRect != nullptr && activeRect->IsValid(w, h)) ? activeRect->y1 : h;

		size_t activeCount = 0;
		size_t frozenCount = 0;

		for (int cy = 0; cy < cellsH; ++cy)
		{
			const int y0 = (std::max)(cy * step, ry0);
			const int y1 = (std::min)((cy + 1) * step, ry1);
			if (y0 >= y1)
				continue;
			for (int cx = 0; cx < cellsW; ++cx)
			{
				const int x0 = (std::max)(cx * step, rx0);
				const int x1 = (std::min)((cx + 1) * step, rx1);
				if (x0 >= x1)
					continue;
				const size_t cIdx = static_cast<size_t>(cy) * static_cast<size_t>(cellsW) + static_cast<size_t>(cx);
				if (trustedCellMask != nullptr && trustedCellMask[cIdx] == 0)
					continue;

				const double gVar = (cellGrainVarP10g != nullptr) ? cellGrainVarP10g[cIdx] : scalarGrainVar;
				if (!std::isfinite(gVar) || gVar <= baseQuantVar)
					continue;

				double sumD = 0.0;
				double sumD2 = 0.0;
				size_t cnt = 0;
				bool cellFinite = true;
				for (int y = y0; y < y1 && cellFinite; ++y)
				{
					const size_t row = static_cast<size_t>(y) * static_cast<size_t>(w);
					for (int x = x0; x < x1; ++x)
					{
						const double a = curRawY[row + static_cast<size_t>(x)];
						const double b = prevWarpedRawY[row + static_cast<size_t>(x)];
						if (!std::isfinite(a) || !std::isfinite(b))
						{
							cellFinite = false;
							break;
						}
						const double d = a - b;
						sumD += d;
						sumD2 += d * d;
						++cnt;
					}
				}
				if (!cellFinite || cnt < 4u)
					continue;

				const double invN = 1.0 / static_cast<double>(cnt);
				const double meanD = sumD * invN;
				const double varD = (std::max)(0.0, sumD2 * invN - meanD * meanD);
				++activeCount;
				// Exact -3 dB half-power threshold below independent-grain Var(g_t - g_{t-1}) = 2 * gVar:
				if (varD <= gVar)
					++frozenCount;
			}
		}

		outGrainActiveCells = activeCount;
		if (activeCount == 0)
			return false;
		outFreezeFrac = static_cast<double>(frozenCount) / static_cast<double>(activeCount);
		return std::isfinite(outFreezeFrac);
	}

	// Signal 2: Grain-free 8x8 orthonormal DCT-II AC transform sparsity (pi_0).
	// Physics derivation (Rule 1):
	//   Orthonormal 8x8 DCT-II preserves variance (Parseval): 1-LSB rounding noise contributes
	//   63 * baseQuantVar across the 63 AC coefficients. On textured 8x8 blocks (acEnergy > 63 * baseQuantVar),
	//   an AC coefficient quantized to zero sits inside the exact half-LSB rounding dead-zone
	//   |c_uv| <= 0.5 * DepthScale(sourceBits). Immune to HEVC/AV1 in-loop deblocking and SAO/CDEF.
	inline bool MeasureCleanAcSparsityPi0(
		const double* curCleanY,
		int w,
		int h,
		int sourceBits,
		double& outSparsityPi0,
		size_t& outTexturedBlocks,
		const ActiveRect* activeRect = nullptr)
	{
		outSparsityPi0 = 0.0;
		outTexturedBlocks = 0;
		if (curCleanY == nullptr || w < 8 || h < 8)
			return false;
		if (!(PeakCodeForBits(sourceBits) > 0.0))
			return false;

		static const std::array<std::array<double, 8>, 8> kDctBasis = []() {
			std::array<std::array<double, 8>, 8> m{};
			constexpr double kPi = 3.14159265358979323846;
			const double s0 = 1.0 / std::sqrt(8.0);
			const double sk = 0.5;
			for (int u = 0; u < 8; ++u)
			{
				const double a = (u == 0) ? s0 : sk;
				for (int x = 0; x < 8; ++x)
					m[u][x] = a * std::cos(kPi * (2.0 * static_cast<double>(x) + 1.0) * static_cast<double>(u) / 16.0);
			}
			return m;
		}();

		const double dScale = DepthScale(sourceBits);
		const double halfLsb = 0.5 * dScale;
		const double minTexturedAcEnergy = 63.0 * (dScale * dScale) / 12.0;

		const int rx0 = (activeRect != nullptr && activeRect->IsValid(w, h)) ? activeRect->x0 : 0;
		const int ry0 = (activeRect != nullptr && activeRect->IsValid(w, h)) ? activeRect->y0 : 0;
		const int rx1 = (activeRect != nullptr && activeRect->IsValid(w, h)) ? activeRect->x1 : w;
		const int ry1 = (activeRect != nullptr && activeRect->IsValid(w, h)) ? activeRect->y1 : h;

		size_t texturedBlocks = 0;
		uint64_t zeroedAcTotal = 0;

		double rowTmp[8][8];
		for (int by = ry0; by + 8 <= ry1; by += 8)
		{
			for (int bx = rx0; bx + 8 <= rx1; bx += 8)
			{
				bool blockFinite = true;
				for (int y = 0; y < 8 && blockFinite; ++y)
				{
					const double* srcRow = curCleanY + static_cast<size_t>(by + y) * static_cast<size_t>(w) + static_cast<size_t>(bx);
					for (int x = 0; x < 8; ++x)
					{
						if (!std::isfinite(srcRow[x]))
						{
							blockFinite = false;
							break;
						}
					}
					if (!blockFinite)
						break;
					for (int u = 0; u < 8; ++u)
					{
						double acc = 0.0;
						for (int x = 0; x < 8; ++x)
							acc += kDctBasis[u][x] * srcRow[x];
						rowTmp[y][u] = acc;
					}
				}
				if (!blockFinite)
					continue;

				double acEnergy = 0.0;
				uint64_t zeroAc = 0;
				for (int v = 0; v < 8; ++v)
				{
					for (int u = 0; u < 8; ++u)
					{
						if (u == 0 && v == 0)
							continue;
						double coeff = 0.0;
						for (int y = 0; y < 8; ++y)
							coeff += kDctBasis[v][y] * rowTmp[y][u];
						acEnergy += coeff * coeff;
						if (std::fabs(coeff) <= halfLsb)
							++zeroAc;
					}
				}

				if (acEnergy > minTexturedAcEnergy)
				{
					++texturedBlocks;
					zeroedAcTotal += zeroAc;
				}
			}
		}

		outTexturedBlocks = texturedBlocks;
		if (texturedBlocks == 0)
			return false;
		outSparsityPi0 = static_cast<double>(zeroedAcTotal) / (63.0 * static_cast<double>(texturedBlocks));
		return std::isfinite(outSparsityPi0);
	}

	// Signal 3: Motion-compensated grain-free innovation reset spike + bidirectional prediction asymmetry.
	// Physics derivation (Rule 1):
	//   - Innovation reset: per-cell RMS |X_clean(t) - warp(X_clean(t-1))| normalized by quantSigma = DepthScale/sqrt(12).
	//   - Bidirectional asymmetry: RMS |X_clean(t) - 0.5*(warp(t-1)+warp(t+1))| / (0.5*(RMS_prev + RMS_next)).
	//     By the triangle inequality, an interpolated B-frame sitting between neighbors has ratio near 0,
	//     whereas a fresh I/Peak frame carrying detail absent from both neighbors has ratio near 1.0.
	inline bool MeasureCleanInnovationAndBidirAsymmetry(
		const double* curCleanY,
		const double* prevWarpedCleanY,
		const double* nextWarpedCleanY,
		int w,
		int h,
		int cellPx,
		int sourceBits,
		double& outInnovationRatio,
		double& outBidirAsymmetryRatio,
		bool& outHasBidir,
		size_t& outCellCount,
		const ActiveRect* activeRect = nullptr,
		const uint8_t* trustedCellMask = nullptr)
	{
		outInnovationRatio = 0.0;
		outBidirAsymmetryRatio = 0.0;
		outHasBidir = false;
		outCellCount = 0;
		if (curCleanY == nullptr || prevWarpedCleanY == nullptr || w <= 0 || h <= 0)
			return false;
		if (!(PeakCodeForBits(sourceBits) > 0.0))
			return false;

		const int step = cellPx > 0 ? cellPx : kCanonicalBlockPx;
		const int cellsW = (w + step - 1) / step;
		const int cellsH = (h + step - 1) / step;
		const double dScale = DepthScale(sourceBits);
		const double quantSigma = dScale / std::sqrt(12.0);
		if (!(quantSigma > 0.0))
			return false;

		const int rx0 = (activeRect != nullptr && activeRect->IsValid(w, h)) ? activeRect->x0 : 0;
		const int ry0 = (activeRect != nullptr && activeRect->IsValid(w, h)) ? activeRect->y0 : 0;
		const int rx1 = (activeRect != nullptr && activeRect->IsValid(w, h)) ? activeRect->x1 : w;
		const int ry1 = (activeRect != nullptr && activeRect->IsValid(w, h)) ? activeRect->y1 : h;

		std::vector<double> innovRatios;
		std::vector<double> bidirRatios;
		innovRatios.reserve(static_cast<size_t>(cellsW) * static_cast<size_t>(cellsH));
		if (nextWarpedCleanY != nullptr)
			bidirRatios.reserve(static_cast<size_t>(cellsW) * static_cast<size_t>(cellsH));

		for (int cy = 0; cy < cellsH; ++cy)
		{
			const int y0 = (std::max)(cy * step, ry0);
			const int y1 = (std::min)((cy + 1) * step, ry1);
			if (y0 >= y1)
				continue;
			for (int cx = 0; cx < cellsW; ++cx)
			{
				const int x0 = (std::max)(cx * step, rx0);
				const int x1 = (std::min)((cx + 1) * step, rx1);
				if (x0 >= x1)
					continue;
				const size_t cIdx = static_cast<size_t>(cy) * static_cast<size_t>(cellsW) + static_cast<size_t>(cx);
				if (trustedCellMask != nullptr && trustedCellMask[cIdx] == 0)
					continue;

				double sumPrev2 = 0.0;
				double sumNext2 = 0.0;
				double sumMid2 = 0.0;
				size_t cnt = 0;
				bool cellFinite = true;
				for (int y = y0; y < y1 && cellFinite; ++y)
				{
					const size_t row = static_cast<size_t>(y) * static_cast<size_t>(w);
					for (int x = x0; x < x1; ++x)
					{
						const size_t p = row + static_cast<size_t>(x);
						const double c = curCleanY[p];
						const double pv = prevWarpedCleanY[p];
						if (!std::isfinite(c) || !std::isfinite(pv))
						{
							cellFinite = false;
							break;
						}
						const double dp = c - pv;
						sumPrev2 += dp * dp;
						if (nextWarpedCleanY != nullptr)
						{
							const double nv = nextWarpedCleanY[p];
							if (!std::isfinite(nv))
							{
								cellFinite = false;
								break;
							}
							const double dn = c - nv;
							const double dm = c - 0.5 * (pv + nv);
							sumNext2 += dn * dn;
							sumMid2 += dm * dm;
						}
						++cnt;
					}
				}
				if (!cellFinite || cnt < 4u)
					continue;

				const double invN = 1.0 / static_cast<double>(cnt);
				const double rmsPrev = std::sqrt(sumPrev2 * invN);
				innovRatios.push_back(rmsPrev / quantSigma);

				if (nextWarpedCleanY != nullptr)
				{
					const double rmsNext = std::sqrt(sumNext2 * invN);
					const double rmsMid = std::sqrt(sumMid2 * invN);
					const double avgSide = 0.5 * (rmsPrev + rmsNext);
					if (avgSide > quantSigma)
						bidirRatios.push_back(RampMath::Clamp01(rmsMid / avgSide));
				}
			}
		}

		outCellCount = innovRatios.size();
		if (innovRatios.empty())
			return false;
		outInnovationRatio = RobustStats::Median(innovRatios, RobustStats::MedianRule::MeanOfMiddle);
		if (!bidirRatios.empty())
		{
			outBidirAsymmetryRatio = RobustStats::Median(bidirRatios, RobustStats::MedianRule::MeanOfMiddle);
			outHasBidir = std::isfinite(outBidirAsymmetryRatio);
		}
		return std::isfinite(outInnovationRatio);
	}

	// Combines whichever Pass 3B forensic cues are enabled and valid via exact equal-weight (1/N) average into [0, 1].
	inline ForensicFrameSignals ComputeForensicFrameSignals(
		const ForensicPeakV2Config& cfg,
		const double* curRawY,
		const double* prevWarpedRawY,
		const double* curCleanY,
		const double* prevWarpedCleanY,
		const double* nextWarpedCleanY,
		int w,
		int h,
		const double* cellGrainVarP10g,
		double scalarGrainVar,
		int cellPx = kCanonicalBlockPx,
		int sourceBits = 10,
		const ActiveRect* activeRect = nullptr,
		const uint8_t* trustedCellMask = nullptr)
	{
		ForensicFrameSignals sig;
		double scoreSum = 0.0;
		int cueCount = 0;

		if (cfg.enableSkipGrainFreeze)
		{
			sig.hasSkipGrainFreeze = MeasureSkipGrainFreezeFraction(
				curRawY, prevWarpedRawY, w, h, cellGrainVarP10g, scalarGrainVar, cellPx, sourceBits,
				sig.skipGrainFreezeFrac, sig.grainActiveCells, activeRect, trustedCellMask);
			if (sig.hasSkipGrainFreeze)
			{
				scoreSum += RampMath::Clamp01(1.0 - sig.skipGrainFreezeFrac);
				++cueCount;
			}
		}

		if (cfg.enableAcSparsity)
		{
			sig.hasAcSparsity = MeasureCleanAcSparsityPi0(
				curCleanY, w, h, sourceBits, sig.acSparsityPi0, sig.texturedAcBlocks, activeRect);
			if (sig.hasAcSparsity)
			{
				scoreSum += RampMath::Clamp01(1.0 - sig.acSparsityPi0);
				++cueCount;
			}
		}

		if (cfg.enableInnovationBidir)
		{
			sig.hasInnovation = MeasureCleanInnovationAndBidirAsymmetry(
				curCleanY, prevWarpedCleanY, nextWarpedCleanY, w, h, cellPx, sourceBits,
				sig.innovationResetRatio, sig.bidirAsymmetryRatio, sig.hasBidirAsymmetry,
				sig.innovationCells, activeRect, trustedCellMask);
			if (sig.hasInnovation)
			{
				// Map innovation SNR into [0, 1] using derived 3-sigma scale (kBlockingRatioSigma = 3.0).
				scoreSum += RampMath::Clamp01(std::tanh((std::max)(0.0, sig.innovationResetRatio) / kBlockingRatioSigma));
				++cueCount;
			}
			if (sig.hasBidirAsymmetry)
			{
				scoreSum += RampMath::Clamp01(sig.bidirAsymmetryRatio);
				++cueCount;
			}
		}

		sig.activeCueCount = cueCount;
		if (cueCount > 0)
		{
			sig.compositePeakScore = RampMath::Clamp01(scoreSum / static_cast<double>(cueCount));
			sig.valid = std::isfinite(sig.compositePeakScore);
		}
		return sig;
	}

	// Nested Mini-GOP (4 / 8 / 16 unique frames) phase-folded rhythm lock + U7 post-cut I-frame seeding,
	// fully wired to the mandatory s3.5 cut / fade / flash / resync / pulldown-repeat contract.
	class ForensicGopCadenceLock
	{
	public:
		static constexpr int kNumCandidatePeriods = 3;
		static constexpr int kCandidatePeriods[kNumCandidatePeriods] = { 4, 8, 16 };
		static constexpr size_t kMaxHistorySamples = 32; // >= 2 full cycles of longest period (16)
		// 2-sigma statistical separation between peak phase mean and non-peak phase mean (95% confidence).
		static constexpr double kMinLockSeparationZ = 2.0;

		void ResetAll()
		{
			samples_.clear();
			uniqueFrameIndex_ = 0;
			lockedPeriod_ = 0;
			lockedPhase_ = -1;
			lockSeparationZ_ = 0.0;
			postCutIFramePending_ = false;
			fadeSuspended_ = false;
			cutResetCount_ = 0;
			fadeSuspendCount_ = 0;
			flashSkipCount_ = 0;
			resyncResetCount_ = 0;
			pulldownRepeatSkipCount_ = 0;
		}

		// s3.5 Hard Cut (U7): wipe previous shot's lock completely, reset unique index to 0,
		// and seed frame 0 post-cut as a high-prior new-shot I-frame anchor.
		void OnHardCut()
		{
			samples_.clear();
			uniqueFrameIndex_ = 0;
			lockedPeriod_ = 0;
			lockedPhase_ = -1;
			lockSeparationZ_ = 0.0;
			postCutIFramePending_ = true;
			fadeSuspended_ = false;
			++cutResetCount_;
		}

		// s3.5 Resync / Discontinuity / Seek: cold-start wipe WITHOUT blindly seeding frame 0 as an I-frame!
		void OnResync()
		{
			samples_.clear();
			uniqueFrameIndex_ = 0;
			lockedPeriod_ = 0;
			lockedPhase_ = -1;
			lockSeparationZ_ = 0.0;
			postCutIFramePending_ = false;
			fadeSuspended_ = false;
			++resyncResetCount_;
		}

		// s3.5 Fade / Dissolve: suspend cadence-lock updates & peak scoring until fade clears.
		void OnFade()
		{
			fadeSuspended_ = true;
			++fadeSuspendCount_;
		}

		// s3.5 Flash / Strobe (P4): skip flash frame in accumulator, advancing uniqueFrameIndex_ ONLY
		// so a locked Mini-GOP 4/8/16 phase stays 100% intact across a 1-frame flash!
		void OnFlash()
		{
			++uniqueFrameIndex_;
			++flashSkipCount_;
		}

		// s3.5 3:2 Pulldown Repeat (dt == 0 / exact-copy): NEVER advance uniqueFrameIndex_ or push into accumulator!
		void OnPulldownRepeat()
		{
			++pulldownRepeatSkipCount_;
		}

		// Push a unique non-fade, non-flash, non-repeat frame score and return its assigned unique source-frame index.
		uint64_t PushUniqueFrame(double score, int sourceBits, bool& outWasPostCutSeed)
		{
			fadeSuspended_ = false;
			const uint64_t assignedIdx = uniqueFrameIndex_;
			++uniqueFrameIndex_;
			outWasPostCutSeed = postCutIFramePending_ && (assignedIdx == 0);
			postCutIFramePending_ = false;

			if (std::isfinite(score))
			{
				samples_.push_back(CadenceSample{ assignedIdx, RampMath::Clamp01(score) });
				if (samples_.size() > kMaxHistorySamples)
					samples_.pop_front();
				RecomputeLock(sourceBits);
			}
			return assignedIdx;
		}

		bool IsLocked() const { return lockedPeriod_ > 0 && lockedPhase_ >= 0; }
		int LockedPeriod() const { return lockedPeriod_; }
		int LockedPhase() const { return lockedPhase_; }
		double LockSeparationZ() const { return lockSeparationZ_; }
		bool IsFadeSuspended() const { return fadeSuspended_; }
		uint64_t UniqueFrameIndex() const { return uniqueFrameIndex_; }

		bool IsPeakPhase(uint64_t uniqueIdx) const
		{
			if (!IsLocked())
				return false;
			return static_cast<int>(uniqueIdx % static_cast<uint64_t>(lockedPeriod_)) == lockedPhase_;
		}

		uint64_t CutResetCount() const { return cutResetCount_; }
		uint64_t FadeSuspendCount() const { return fadeSuspendCount_; }
		uint64_t FlashSkipCount() const { return flashSkipCount_; }
		uint64_t ResyncResetCount() const { return resyncResetCount_; }
		uint64_t PulldownRepeatSkipCount() const { return pulldownRepeatSkipCount_; }

	private:
		struct CadenceSample
		{
			uint64_t uniqueIdx = 0;
			double score = 0.0;
		};

		void RecomputeLock(int sourceBits)
		{
			const double peakCode = PeakCodeForBits(sourceBits);
			const double quantFloor = (peakCode > 0.0) ? (0.5 / peakCode) : kRoleNumFloor;

			int bestPeriod = 0;
			int bestPhase = -1;
			double bestZ = 0.0;

			for (int pi = 0; pi < kNumCandidatePeriods; ++pi)
			{
				const int P = kCandidatePeriods[pi];
				if (samples_.size() < static_cast<size_t>(2 * P))
					continue;

				double sumPhase[16] = {};
				int cntPhase[16] = {};
				for (const auto& s : samples_)
				{
					const int ph = static_cast<int>(s.uniqueIdx % static_cast<uint64_t>(P));
					sumPhase[ph] += s.score;
					++cntPhase[ph];
				}

				int populatedBins = 0;
				int peakPh = -1;
				double peakMean = -1.0;
				for (int ph = 0; ph < P; ++ph)
				{
					if (cntPhase[ph] > 0)
						++populatedBins;
					if (cntPhase[ph] >= 2)
					{
						const double m = sumPhase[ph] / static_cast<double>(cntPhase[ph]);
						if (m > peakMean)
						{
							peakMean = m;
							peakPh = ph;
						}
					}
				}
				if (peakPh < 0 || populatedBins < P - 1)
					continue;

				double otherSum = 0.0;
				int otherCnt = 0;
				for (int ph = 0; ph < P; ++ph)
				{
					if (ph != peakPh)
					{
						otherSum += sumPhase[ph];
						otherCnt += cntPhase[ph];
					}
				}
				if (otherCnt < 2)
					continue;
				const double otherMean = otherSum / static_cast<double>(otherCnt);
				if (peakMean <= otherMean + quantFloor)
					continue;

				double ssPeak = 0.0;
				double ssOther = 0.0;
				for (const auto& s : samples_)
				{
					const int ph = static_cast<int>(s.uniqueIdx % static_cast<uint64_t>(P));
					if (ph == peakPh)
					{
						const double d = s.score - peakMean;
						ssPeak += d * d;
					}
					else
					{
						const double d = s.score - otherMean;
						ssOther += d * d;
					}
				}
				const double varPeak = (cntPhase[peakPh] > 1) ? (ssPeak / static_cast<double>(cntPhase[peakPh] - 1)) : 0.0;
				const double varOther = (otherCnt > 1) ? (ssOther / static_cast<double>(otherCnt - 1)) : 0.0;
				const double se = std::sqrt(varPeak / static_cast<double>(cntPhase[peakPh]) + varOther / static_cast<double>(otherCnt));
				const double z = (peakMean - otherMean) / (std::max)(se, quantFloor);

				if (z >= kMinLockSeparationZ)
				{
					// Prefer smallest fundamental period (4 < 8 < 16) unless a longer period beats it by > 1 sigma.
					if (bestPeriod == 0 || z > bestZ + 1.0)
					{
						bestPeriod = P;
						bestPhase = peakPh;
						bestZ = z;
					}
				}
			}

			lockedPeriod_ = bestPeriod;
			lockedPhase_ = bestPhase;
			lockSeparationZ_ = bestZ;
		}

		std::deque<CadenceSample> samples_;
		uint64_t uniqueFrameIndex_ = 0;
		int lockedPeriod_ = 0;
		int lockedPhase_ = -1;
		double lockSeparationZ_ = 0.0;
		bool postCutIFramePending_ = false;
		bool fadeSuspended_ = false;
		uint64_t cutResetCount_ = 0;
		uint64_t fadeSuspendCount_ = 0;
		uint64_t flashSkipCount_ = 0;
		uint64_t resyncResetCount_ = 0;
		uint64_t pulldownRepeatSkipCount_ = 0;
	};

	// Picture-only evidence supplied per unique frame (ZERO bitstream/encoder side data).
	struct FrameRoleEvidence
	{
		uint64_t seq = 0;
		double qualityScore = 0.0;        // 0..1 (1 = sharpest/cleanest)
		double stepMean = 0.0;            // Q8.8-encoded into QualityPattern::Record
		double stepP90 = 0.0;
		double stepMax = 0.0;
		uint16_t periodX = 0;
		uint16_t periodY = 0;
		double exactCopyFracPrev1 = 0.0;  // Fraction of cells matching N-1
		double exactCopyFracPrev2 = 0.0;  // Fraction of cells matching N-2
		bool isCut = false;               // External P4 / U7 hard-cut flag
		bool isFade = false;              // External P4 / U7 fade flag
		bool isFlash = false;             // External P4 1-frame flash/strobe flag (s3.5)
		bool isResyncOrDiscontinuity = false; // External VpIndicator kFull|kContinuity / seek / span-invalid flag (s3.5)
		bool isPulldownRepeat = false;    // External 3:2 pulldown repeat / dt == 0 flag (s3.5)
		double forensicPeakScore = -1.0;  // Optional Pass 3B compositePeakScore in [0, 1]
		bool hasForensicScore = false;    // True when forensicPeakScore was populated by ComputeForensicFrameSignals
		int sourceBits = 10;

		static FrameRoleEvidence FromCodingGrid(
			uint64_t seqId,
			const CodingGridSummary& grid,
			bool cutFlag = false,
			bool fadeFlag = false,
			int bits = 10)
		{
			FrameRoleEvidence ev;
			ev.seq = seqId;
			ev.qualityScore = grid.pictureScore;
			ev.stepMean = grid.stepMean;
			ev.stepP90 = grid.stepP90;
			ev.stepMax = grid.stepMax;
			ev.periodX = grid.detectedPeriodX;
			ev.periodY = grid.detectedPeriodY;
			ev.exactCopyFracPrev1 = grid.exactCopyFracPrev1;
			ev.exactCopyFracPrev2 = grid.exactCopyFracPrev2;
			ev.isCut = cutFlag;
			ev.isFade = fadeFlag;
			ev.sourceBits = bits;
			return ev;
		}
	};

	// Maintains running log-sharpness sum S_t = sum r_k with sliding-window median removed (Plan Phase 1)
	// and converts into FrameRoleEvidence for TdV2FrameRoleDetector (preserving exact-copy & cut/fade handling).
	class AlignedSharpnessTracker
	{
	public:
		static constexpr size_t kMaxRunningWindow = 9;

		void Reset()
		{
			runningSumS_ = 0.0;
			recentS_.clear();
		}

		double CurrentRunningSum() const { return runningSumS_; }

		FrameRoleEvidence PushPairAndBuildEvidence(
			uint64_t seq,
			const AlignedSharpnessSummary& summary,
			const EmpiricalOwnErrorPrior* a2ePrior,
			bool isCut,
			bool isFade,
			int sourceBits,
			uint16_t bonusPeriodX = 0,
			uint16_t bonusPeriodY = 0)
		{
			FrameRoleEvidence ev;
			ev.seq = seq;
			ev.exactCopyFracPrev1 = summary.exactCopyFracPrev1;
			ev.exactCopyFracPrev2 = summary.exactCopyFracPrev2;
			ev.isCut = isCut;
			ev.isFade = isFade;
			ev.sourceBits = sourceBits;
			ev.periodX = bonusPeriodX;
			ev.periodY = bonusPeriodY;

			if (isCut)
			{
				Reset();
				ev.qualityScore = 0.5;
				return ev;
			}
			if (isFade)
			{
				ev.qualityScore = 0.5;
				return ev;
			}

			// Exact copies do not advance the running sharpness sum (zero drift on 3:2 pulldown repeats).
			const bool isCopy = (summary.exactCopyFracPrev1 >= kFullFrameCopyFrac) ||
			                    (summary.exactCopyFracPrev2 >= kFullFrameCopyFrac);
			if (!isCopy && summary.valid && std::isfinite(summary.relativeSharpnessR))
			{
				runningSumS_ += summary.relativeSharpnessR;
			}
			recentS_.push_back(runningSumS_);
			if (recentS_.size() > kMaxRunningWindow)
				recentS_.pop_front();

			// Remove sliding-window median from running sum S_t -> centered relative sharpness S_tilde.
			std::vector<double> winVec(recentS_.begin(), recentS_.end());
			const double medS = RobustStats::Median(winVec, RobustStats::MedianRule::MeanOfMiddle);
			double sTilde = runningSumS_ - medS;

			// Secondary role cue from A2e empirical own-error prior U_j (Plan Phase 1 item 2(a)):
			// Valleys have higher own error U; fold in stepMean/stepP90/stepMax from sqrt(U_total)
			// so QualityPattern::Record carries real own-error scale even on deblocked HEVC/AV1!
			if (a2ePrior != nullptr && a2ePrior->valid)
			{
				const double uFlat = a2ePrior->TotalForClass(0);
				const double uTex = a2ePrior->TotalForClass(1);
				const double rmsFlat = std::sqrt((std::max)(0.0, uFlat));
				const double rmsTex = std::sqrt((std::max)(0.0, uTex));
				ev.stepMean = 0.5 * (rmsFlat + rmsTex);
				ev.stepP90 = (std::max)(rmsFlat, rmsTex);
				ev.stepMax = ev.stepP90;
			}

			// Monotonic smooth bijection R -> (0, 1): 0.5 + 0.5 * tanh(sTilde) preserves exact ordering,
			// local maxima (Peaks), local minima (Valleys), and window rank!
			ev.qualityScore = RampMath::Clamp01(0.5 + 0.5 * std::tanh(sTilde));
			return ev;
		}

	private:
		double runningSumS_ = 0.0;
		std::deque<double> recentS_;
	};

	// ---- Picture-Only P15 / P15c Frame-Role Detector (plan s2.1 D10 & s3.2 TD-S2) ----

	enum class AbstainReason : uint8_t
	{
		None = 0,
		CutReset,
		FadeActive,
		InsufficientWindow,
		LowRhythmContrast,
		NonFiniteEvidence,
		FlashAbstained,
		ResyncReset,
		Count
	};

	inline const char* AbstainReasonToken(AbstainReason r)
	{
		switch (r)
		{
		case AbstainReason::None: return "none";
		case AbstainReason::CutReset: return "cut reset";
		case AbstainReason::FadeActive: return "fade active";
		case AbstainReason::InsufficientWindow: return "insufficient window";
		case AbstainReason::LowRhythmContrast: return "low rhythm contrast";
		case AbstainReason::NonFiniteEvidence: return "non-finite evidence";
		case AbstainReason::FlashAbstained: return "flash abstained";
		case AbstainReason::ResyncReset: return "resync reset";
		case AbstainReason::Count: break;
		}
		return "unknown";
	}

	struct FrameRoleDecision
	{
		QualityPattern::Record record{};  // Exact 64-byte skeleton POD
		AbstainReason abstain = AbstainReason::None;
		bool produced = false;
		// Pending frames flushed in sequence order before a cut or lead reduction with available reduced lead (marked provisional; zero silent drops).
		std::vector<FrameRoleDecision> flushedBeforeCut;
		// Pass 3B forensic cadence observability (always default-inert when enableForensicGopPeakV2 == false).
		bool cadenceLocked = false;
		int lockedGopPeriod = 0;
		int lockedGopPhase = -1;
		bool postCutIFrameSeeded = false;
		bool cadencePromotedPeak = false;
	};

	class TdV2FrameRoleDetector
	{
	public:
		// Standard sliding-window capacity: 2 past + center + 2 ahead = 5 core slots (history kept up to 9 for robust MAD).
		static constexpr size_t kMaxWindowFrames = 9;
		static constexpr uint8_t kMaxLeadFrames = 2;

		explicit TdV2FrameRoleDetector(
			uint8_t leadFrames = kMaxLeadFrames,
			const ForensicPeakV2Config& forensicCfg = ForensicPeakV2Config{})
			: leadFrames_(leadFrames > kMaxLeadFrames ? kMaxLeadFrames : leadFrames)
			, forensicCfg_(forensicCfg)
		{
		}

		void SetForensicConfig(const ForensicPeakV2Config& cfg) { forensicCfg_ = cfg; }
		const ForensicPeakV2Config& ForensicConfig() const { return forensicCfg_; }
		const ForensicGopCadenceLock& CadenceLock() const { return cadenceLock_; }

		void SetLeadFrames(uint8_t leadFrames, std::vector<FrameRoleDecision>* flushedOut = nullptr)
		{
			leadFrames_ = leadFrames > kMaxLeadFrames ? kMaxLeadFrames : leadFrames;
			if (flushedOut != nullptr && window_.size() > static_cast<size_t>(leadFrames_))
			{
				const size_t flushEnd = window_.size() - static_cast<size_t>(leadFrames_);
				for (size_t i = 0; i < flushEnd; ++i)
				{
					if (window_[i].emitted)
						continue;
					const size_t remAhead = window_.size() - 1 - i;
					const uint8_t availLead = (remAhead < static_cast<size_t>(kMaxLeadFrames))
						? static_cast<uint8_t>(remAhead)
						: kMaxLeadFrames;
					FrameRoleDecision fd = ClassifyTargetWithLead(i, window_[i].ev, availLead, /*forceProvisional=*/true);
					if (fd.produced)
						window_[i].emitted = true;
					flushedOut->push_back(std::move(fd));
				}
			}
		}

		uint8_t LeadFrames() const { return leadFrames_; }

		void Reset()
		{
			for (const auto& entry : window_)
			{
				if (!entry.emitted)
					++droppedCount_;
			}
			window_.clear();
			cadenceLock_.ResetAll();
			hasEmittedSeq_ = false;
			lastEmittedSeq_ = 0;
		}

		uint64_t AbstainCount(AbstainReason r) const
		{
			const size_t idx = static_cast<size_t>(r);
			return idx < abstainCounts_.size() ? abstainCounts_[idx] : 0;
		}

		// Visible counter of un-emitted buffered frames dropped via Reset() or window eviction (Rule 4).
		uint64_t DroppedRecordCount() const { return droppedCount_; }

		size_t WindowSize() const { return window_.size(); }

		// Push the latest arriving frame and classify the frame at delay `leadFrames_` (0 => causal P15c, 2 => P15).
		FrameRoleDecision PushAndClassify(const FrameRoleEvidence& ev, StandInRegistry* standIns = nullptr)
		{
			if (standIns != nullptr)
				standIns->Engage(StandInKind::P15FramePattern, leadFrames_ < 2 ? "causal provisional" : "lead-2 picture-only");

			FrameRoleDecision dec;
			dec.record.unit = QualityPattern::kUnitVersion;
			dec.record.seq = ev.seq;

			// Non-finite check (Rule 4: fail closed with visible abstain counter; also check invalid sourceBits).
			// Flush all earlier un-emitted buffered frames in strictly monotonic sequence order BEFORE emitting
			// the non-finite abstain record for `ev.seq` so buffered records are never emitted out of order.
			if (!std::isfinite(ev.qualityScore) || !std::isfinite(ev.stepMean) ||
			    !std::isfinite(ev.stepP90) || !std::isfinite(ev.stepMax) ||
			    !std::isfinite(ev.exactCopyFracPrev1) || !std::isfinite(ev.exactCopyFracPrev2) ||
			    (ev.hasForensicScore && !std::isfinite(ev.forensicPeakScore)) ||
			    PeakCodeForBits(ev.sourceBits) <= 0.0)
			{
				if (standIns != nullptr)
					standIns->Engage(StandInKind::NonFiniteClosure, "TdV2FrameRoleDetector non-finite or invalid sourceBits");
				std::vector<FrameRoleDecision> pendingFlushed;
				for (size_t i = 0; i < window_.size(); ++i)
				{
					if (window_[i].emitted)
						continue;
					const size_t remAhead = window_.size() - 1 - i;
					const uint8_t availLead = (remAhead < static_cast<size_t>(kMaxLeadFrames))
						? static_cast<uint8_t>(remAhead)
						: kMaxLeadFrames;
					FrameRoleDecision fd = ClassifyTargetWithLead(i, window_[i].ev, availLead, /*forceProvisional=*/true);
					window_[i].emitted = true;
					pendingFlushed.push_back(std::move(fd));
				}
				Abstain(dec, AbstainReason::NonFiniteEvidence, ev.seq, leadFrames_, /*emitRecord=*/true);
				dec.flushedBeforeCut = std::move(pendingFlushed);
				return dec;
			}

			// s3.5 Resync / Discontinuity / Seek: full cold-start wipe of window & cadence lock
			// WITHOUT blindly seeding frame 0 as a confirmed I-frame! Takes precedence over isCut.
			if (ev.isResyncOrDiscontinuity)
			{
				std::vector<FrameRoleDecision> flushed;
				for (size_t i = 0; i < window_.size(); ++i)
				{
					if (window_[i].emitted)
						continue;
					const size_t remAhead = window_.size() - 1 - i;
					const uint8_t availLead = (remAhead < static_cast<size_t>(kMaxLeadFrames))
						? static_cast<uint8_t>(remAhead)
						: kMaxLeadFrames;
					FrameRoleDecision fd = ClassifyTargetWithLead(i, window_[i].ev, availLead, /*forceProvisional=*/true);
					window_[i].emitted = true;
					flushed.push_back(std::move(fd));
				}
				window_.clear();
				cadenceLock_.OnResync();
				const bool emitNow = (leadFrames_ == 0);
				WindowEntry entry{};
				entry.ev = ev;
				entry.emitted = emitNow;
				if (forensicCfg_.enableForensicGopPeakV2 && forensicCfg_.enableCadenceLock && !ev.isFade && !ev.isFlash)
				{
					uint8_t cf = 0;
					if (ev.isPulldownRepeat || IsExactCopy(ev, cf))
					{
						cadenceLock_.OnPulldownRepeat();
					}
					else
					{
						bool wasSeed = false;
						entry.uniqueIdx = cadenceLock_.PushUniqueFrame(EffectiveScore(ev), ev.sourceBits, wasSeed);
						entry.hasUniqueIdx = true;
						entry.isPostCutIFrameSeed = false; // NEVER seed on resync/seek
					}
				}
				window_.push_back(entry);
				if (emitNow)
				{
					hasEmittedSeq_ = true;
					lastEmittedSeq_ = ev.seq;
				}
				Abstain(dec, AbstainReason::ResyncReset, ev.seq, leadFrames_, emitNow);
				dec.flushedBeforeCut = std::move(flushed);
				return dec;
			}

			// Cut immediately flushes ALL un-emitted pre-cut frames in sequence order (with available reduced lead,
			// marked provisional) into `dec.flushedBeforeCut` BEFORE clearing the temporal window so peaks are NEVER
			// bridged across cuts and pending records are NEVER silently dropped!
			// When leadFrames_ > 0, the cut frame stays buffered as the new post-cut anchor and is NOT
			// emitted prematurely (produced = false), preventing duplicate seq emission when lead frames arrive.
			if (ev.isCut)
			{
				std::vector<FrameRoleDecision> flushed;
				for (size_t i = 0; i < window_.size(); ++i)
				{
					if (window_[i].emitted)
						continue;
					const size_t remAhead = window_.size() - 1 - i;
					const uint8_t availLead = (remAhead < static_cast<size_t>(kMaxLeadFrames))
						? static_cast<uint8_t>(remAhead)
						: kMaxLeadFrames;
					FrameRoleDecision fd = ClassifyTargetWithLead(i, window_[i].ev, availLead, /*forceProvisional=*/true);
					window_[i].emitted = true;
					flushed.push_back(std::move(fd));
				}
				window_.clear();
				cadenceLock_.OnHardCut();

				const bool emitNow = (leadFrames_ == 0);
				WindowEntry entry{};
				entry.ev = ev;
				entry.emitted = emitNow;
				if (forensicCfg_.enableForensicGopPeakV2 && forensicCfg_.enableCadenceLock && !ev.isFade && !ev.isFlash)
				{
					uint8_t cf = 0;
					if (ev.isPulldownRepeat || IsExactCopy(ev, cf))
					{
						cadenceLock_.OnPulldownRepeat();
					}
					else
					{
						bool wasSeed = false;
						entry.uniqueIdx = cadenceLock_.PushUniqueFrame(EffectiveScore(ev), ev.sourceBits, wasSeed);
						entry.hasUniqueIdx = true;
						entry.isPostCutIFrameSeed = wasSeed;
					}
				}
				window_.push_back(entry);

				// Pass 3B U7 post-cut I-frame seeding when causal (leadFrames_ == 0): emit frame 0 immediately as Peak!
				if (emitNow && entry.isPostCutIFrameSeed)
				{
					FrameRoleDecision seedDec = ClassifyTargetWithLead(0, window_[0].ev, 0, /*forceProvisional=*/true);
					seedDec.flushedBeforeCut = std::move(flushed);
					return seedDec;
				}

				if (emitNow)
				{
					hasEmittedSeq_ = true;
					lastEmittedSeq_ = ev.seq;
				}
				Abstain(dec, AbstainReason::CutReset, ev.seq, leadFrames_, emitNow);
				dec.flushedBeforeCut = std::move(flushed);
				return dec;
			}

			// Feed Pass 3B cadence lock & s3.5 event counters for arriving frame.
			WindowEntry newEntry{};
			newEntry.ev = ev;
			newEntry.emitted = false;
			if (ev.isFlash)
			{
				if (standIns != nullptr)
					standIns->Engage(StandInKind::FlashFrameAbstained, "P4 flash abstained in TdV2FrameRoleDetector");
				cadenceLock_.OnFlash();
			}
			else if (ev.isFade)
			{
				if (forensicCfg_.enableForensicGopPeakV2 && standIns != nullptr)
					standIns->Engage(StandInKind::CadenceSuspendedFade, "U7 fade suspended forensic cadence lock");
				cadenceLock_.OnFade();
			}
			else
			{
				uint8_t copyFlags = 0;
				if (ev.isPulldownRepeat || IsExactCopy(ev, copyFlags))
				{
					cadenceLock_.OnPulldownRepeat();
				}
				else if (forensicCfg_.enableForensicGopPeakV2 && forensicCfg_.enableCadenceLock)
				{
					bool wasSeed = false;
					newEntry.uniqueIdx = cadenceLock_.PushUniqueFrame(EffectiveScore(ev), ev.sourceBits, wasSeed);
					newEntry.hasUniqueIdx = true;
					newEntry.isPostCutIFrameSeed = wasSeed;
				}
			}

			window_.push_back(newEntry);
			if (window_.size() > kMaxWindowFrames)
			{
				if (!window_.front().emitted)
					++droppedCount_;
				window_.pop_front();
			}

			// Need at least leadFrames_ + 1 frames in window to emit the target frame at index (size - 1 - lead).
			// While warming up, produced = false so ev.seq is never emitted twice.
			if (window_.size() <= static_cast<size_t>(leadFrames_))
			{
				return Abstain(dec, AbstainReason::InsufficientWindow, ev.seq, leadFrames_, /*emitRecord=*/false);
			}

			const size_t targetIdx = window_.size() - 1 - static_cast<size_t>(leadFrames_);

			// Flush any stranded earlier un-emitted frames (e.g., after SetLeadFrames(higher -> lower) without flushedOut)
			// in sequence order before classifying targetIdx so pending records are never stranded or dropped on eviction.
			std::vector<FrameRoleDecision> pendingFlushed;
			for (size_t i = 0; i < targetIdx; ++i)
			{
				if (window_[i].emitted)
					continue;
				const size_t remAhead = window_.size() - 1 - i;
				const uint8_t availLead = static_cast<uint8_t>((std::min)(static_cast<size_t>(kMaxLeadFrames), remAhead));
				FrameRoleDecision fd = ClassifyTargetWithLead(i, window_[i].ev, availLead, /*forceProvisional=*/true);
				if (fd.produced)
					window_[i].emitted = true;
				pendingFlushed.push_back(std::move(fd));
			}

			// Never duplicate an already-emitted entry (e.g., after ClassifyLatestCausal() or SetLeadFrames(0 -> 2)).
			if (window_[targetIdx].emitted)
			{
				Abstain(dec, AbstainReason::InsufficientWindow, window_[targetIdx].ev.seq, leadFrames_, /*emitRecord=*/false);
				dec.flushedBeforeCut = std::move(pendingFlushed);
				return dec;
			}

			FrameRoleDecision outDec = ClassifyTarget(targetIdx, window_[targetIdx].ev);
			if (outDec.produced)
				window_[targetIdx].emitted = true;
			outDec.flushedBeforeCut = std::move(pendingFlushed);
			return outDec;
		}

		// Flush ALL remaining un-emitted buffered frames at stream end in sequence order (each with its available
		// reduced lead, marked provisional, and marked emitted so repeated calls produce zero duplicates).
		std::vector<FrameRoleDecision> FlushEndOfStream(StandInRegistry* standIns = nullptr)
		{
			if (standIns != nullptr)
				standIns->Engage(StandInKind::P15FramePattern, "end-of-stream provisional flush");
			std::vector<FrameRoleDecision> out;
			for (size_t i = 0; i < window_.size(); ++i)
			{
				if (window_[i].emitted)
					continue;
				const size_t remAhead = window_.size() - 1 - i;
				const uint8_t availLead = static_cast<uint8_t>((std::min)(static_cast<size_t>(kMaxLeadFrames), remAhead));
				FrameRoleDecision fd = ClassifyTargetWithLead(i, window_[i].ev, availLead, /*forceProvisional=*/true);
				window_[i].emitted = true;
				out.push_back(std::move(fd));
			}
			return out;
		}

		// Flush the latest buffered frame causally (lead = 0, marked provisional, never duplicating an already-emitted frame).
		// Any earlier un-emitted buffered entries are flushed first in strictly monotonic sequence order into `flushedBeforeCut`.
		FrameRoleDecision ClassifyLatestCausal(StandInRegistry* standIns = nullptr)
		{
			if (standIns != nullptr)
				standIns->Engage(StandInKind::P15FramePattern, "causal provisional");
			FrameRoleDecision dec;
			dec.record.unit = QualityPattern::kUnitVersion;
			if (window_.empty())
				return Abstain(dec, AbstainReason::InsufficientWindow, 0, 0, /*emitRecord=*/false);
			const size_t idx = window_.size() - 1;
			std::vector<FrameRoleDecision> pendingFlushed;
			for (size_t i = 0; i < idx; ++i)
			{
				if (window_[i].emitted)
					continue;
				const size_t remAhead = window_.size() - 1 - i;
				const uint8_t availLead = static_cast<uint8_t>((std::min)(static_cast<size_t>(kMaxLeadFrames), remAhead));
				FrameRoleDecision fd = ClassifyTargetWithLead(i, window_[i].ev, availLead, /*forceProvisional=*/true);
				window_[i].emitted = true;
				pendingFlushed.push_back(std::move(fd));
			}
			if (window_[idx].emitted)
			{
				Abstain(dec, AbstainReason::InsufficientWindow, window_[idx].ev.seq, 0, /*emitRecord=*/false);
				dec.flushedBeforeCut = std::move(pendingFlushed);
				return dec;
			}
			FrameRoleDecision fd = ClassifyTargetWithLead(idx, window_[idx].ev, 0, /*forceProvisional=*/true);
			if (fd.produced)
				window_[idx].emitted = true;
			fd.flushedBeforeCut = std::move(pendingFlushed);
			return fd;
		}

	private:
		struct WindowEntry
		{
			FrameRoleEvidence ev;
			bool emitted = false;
			uint64_t uniqueIdx = 0;
			bool hasUniqueIdx = false;
			bool isPostCutIFrameSeed = false;
		};

		double EffectiveScore(const FrameRoleEvidence& e) const
		{
			if (forensicCfg_.enableForensicGopPeakV2 && e.hasForensicScore && std::isfinite(e.forensicPeakScore))
				return RampMath::Clamp01(e.forensicPeakScore);
			return RampMath::Clamp01(e.qualityScore);
		}

		void StampCadenceTelemetry(FrameRoleDecision& dec) const
		{
			if (forensicCfg_.enableForensicGopPeakV2 && forensicCfg_.enableCadenceLock)
			{
				dec.cadenceLocked = cadenceLock_.IsLocked();
				dec.lockedGopPeriod = cadenceLock_.LockedPeriod();
				dec.lockedGopPhase = cadenceLock_.LockedPhase();
			}
		}

		static uint16_t ToQ88(double v)
		{
			if (!std::isfinite(v) || v <= 0.0)
				return 0;
			const double scaled = std::round(v * kQ88Scale);
			return scaled >= 65535.0 ? 65535u : static_cast<uint16_t>(scaled);
		}

		static bool IsExactCopy(const FrameRoleEvidence& e, uint8_t& outFlags)
		{
			outFlags = 0;
			if (e.exactCopyFracPrev1 >= kFullFrameCopyFrac)
				outFlags |= QualityPattern::kCopyOfPrev1;
			if (e.exactCopyFracPrev2 >= kFullFrameCopyFrac)
				outFlags |= QualityPattern::kCopyOfPrev2;
			return outFlags != 0;
		}

		FrameRoleDecision Abstain(
			FrameRoleDecision& dec,
			AbstainReason reason,
			uint64_t seq,
			uint8_t leadUsed,
			bool emitRecord,
			bool forceProvisional = false)
		{
			const size_t idx = static_cast<size_t>(reason);
			if (idx < abstainCounts_.size())
				++abstainCounts_[idx];
			dec.abstain = reason;
			dec.produced = emitRecord;
			dec.record.unit = QualityPattern::kUnitVersion;
			dec.record.seq = seq;
			dec.record.role = static_cast<uint8_t>(QualityPattern::FrameRole::Unknown);
			dec.record.peakValley = static_cast<uint8_t>(QualityPattern::PeakValley::None);
			dec.record.leadUsed = leadUsed;
			dec.record.causal = (leadUsed == 0) ? 1u : 0u;
			if (forceProvisional || leadUsed < kMaxLeadFrames)
				dec.record.validityBits |= QualityPattern::kProvisional;
			StampCadenceTelemetry(dec);
			if (emitRecord)
			{
				hasEmittedSeq_ = true;
				lastEmittedSeq_ = seq;
			}
			return dec;
		}

		FrameRoleDecision ClassifyTarget(size_t targetIdx, const FrameRoleEvidence& target)
		{
			return ClassifyTargetWithLead(targetIdx, target, leadFrames_, false);
		}

		FrameRoleDecision ClassifyTargetWithLead(
			size_t targetIdx,
			const FrameRoleEvidence& target,
			uint8_t leadUsed,
			bool forceProvisional = false)
		{
			FrameRoleDecision dec;
			QualityPattern::Record& r = dec.record;
			r.unit = QualityPattern::kUnitVersion;
			r.seq = target.seq;
			r.leadUsed = leadUsed;
			r.causal = (leadUsed == 0) ? 1u : 0u;
			const double tScore = EffectiveScore(target);
			r.score = static_cast<float>(tScore);
			r.stepMean = ToQ88(target.stepMean);
			r.stepP90 = ToQ88(target.stepP90);
			r.stepMax = ToQ88(target.stepMax);
			r.gridPeriodX = target.periodX;
			r.gridPeriodY = target.periodY;
			r.validityBits = QualityPattern::kValidScore | QualityPattern::kValidSteps | QualityPattern::kValidCopy;
			if (target.periodX > 0 || target.periodY > 0)
				r.validityBits |= QualityPattern::kValidGrid;
			if (forceProvisional || leadUsed < kMaxLeadFrames)
				r.validityBits |= QualityPattern::kProvisional;
			StampCadenceTelemetry(dec);

			// s3.5 Flash active -> abstain cleanly without contaminating cadence lock or neighbors.
			if (target.isFlash)
			{
				return Abstain(dec, AbstainReason::FlashAbstained, target.seq, leadUsed, /*emitRecord=*/true, forceProvisional);
			}

			// Fade active -> abstain visibly rather than guessing peaks during a ramp.
			if (target.isFade)
			{
				return Abstain(dec, AbstainReason::FadeActive, target.seq, leadUsed, /*emitRecord=*/true, forceProvisional);
			}

			// Cadence repeat check (3:2 pulldown): ExactCopy MUST NEVER be promoted to Peak!
			uint8_t copyFlags = 0;
			if (target.isPulldownRepeat || IsExactCopy(target, copyFlags))
			{
				if (copyFlags == 0)
					copyFlags = QualityPattern::kCopyOfPrev1;
				r.exactCopyFlags = copyFlags;
				r.role = static_cast<uint8_t>(QualityPattern::FrameRole::ExactCopy);
				r.peakValley = static_cast<uint8_t>(QualityPattern::PeakValley::None);
				r.validityBits |= QualityPattern::kValidRole;
				dec.abstain = AbstainReason::None;
				dec.produced = true;
				hasEmittedSeq_ = true;
				lastEmittedSeq_ = target.seq;
				return dec;
			}

			// Pass 3B U7 post-cut I-frame seeding: frame 0 after a confirmed U7 hard cut is a high-prior I-frame anchor!
			const WindowEntry& targetEntry = window_[targetIdx];
			if (forensicCfg_.enableForensicGopPeakV2 && forensicCfg_.enableCadenceLock && targetEntry.isPostCutIFrameSeed)
			{
				r.role = static_cast<uint8_t>(QualityPattern::FrameRole::Peak);
				r.peakValley = static_cast<uint8_t>(QualityPattern::PeakValley::Peak);
				r.validityBits |= QualityPattern::kValidRole;
				dec.abstain = AbstainReason::None;
				dec.produced = true;
				dec.postCutIFrameSeeded = true;
				hasEmittedSeq_ = true;
				lastEmittedSeq_ = target.seq;
				return dec;
			}

			// Collect unique (non-ExactCopy, non-fade, non-flash) frames in window for ranking and neighbor comparison.
			std::vector<double> uniqueScores;
			uniqueScores.reserve(window_.size());
			double prevUniqueScore = 0.0;
			bool hasPrevUnique = false;
			double nextUniqueScore = 0.0;
			bool hasNextUnique = false;

			for (size_t i = 0; i < window_.size(); ++i)
			{
				const FrameRoleEvidence& we = window_[i].ev;
				if (we.isFade || we.isFlash)
					continue;
				uint8_t cf = 0;
				if (i != targetIdx && (we.isPulldownRepeat || IsExactCopy(we, cf)))
					continue;
				const double sc = EffectiveScore(we);
				uniqueScores.push_back(sc);
				if (i < targetIdx)
				{
					prevUniqueScore = sc;
					hasPrevUnique = true;
				}
				else if (i > targetIdx && !hasNextUnique)
				{
					nextUniqueScore = sc;
					hasNextUnique = true;
				}
			}

			// Need at least 2 unique frames in window (3 for full P15 peak/valley with both sides, 2 for causal P15c).
			const size_t minUnique = (leadUsed >= kMaxLeadFrames) ? 3u : 2u;
			if (uniqueScores.size() < minUnique)
			{
				return Abstain(dec, AbstainReason::InsufficientWindow, target.seq, leadUsed, /*emitRecord=*/true, forceProvisional);
			}

			// Compute rank (0 = highest score) and robust spread via U24 RobustStats.
			uint16_t betterCount = 0;
			double minSc = uniqueScores[0], maxSc = uniqueScores[0];
			for (double s : uniqueScores)
			{
				if (s > tScore + kRoleNumFloor)
					++betterCount;
				if (s < minSc) minSc = s;
				if (s > maxSc) maxSc = s;
			}
			r.rank = betterCount;
			r.windowFrames = static_cast<uint16_t>(uniqueScores.size());
			r.validityBits |= QualityPattern::kValidRank;

			// Normalized quantization floor on dimensionless [0, 1] qualityScore:
			// exact half-LSB rounding resolution of a [0, 1] signal quantized to `sourceBits` integer codes
			// (PeakCodeForBits = 2^bits - 1; 0.5 / peakCode ~= 4.89e-4 at 10-bit). Fail closed on invalid sourceBits.
			const double peakCode = PeakCodeForBits(target.sourceBits);
			if (!(peakCode > 0.0))
			{
				return Abstain(dec, AbstainReason::NonFiniteEvidence, target.seq, leadUsed, /*emitRecord=*/true, forceProvisional);
			}
			const double quantScoreFloor = 0.5 / peakCode;
			const double spread = maxSc - minSc;
			if (spread < 2.0 * quantScoreFloor)
			{
				return Abstain(dec, AbstainReason::LowRhythmContrast, target.seq, leadUsed, /*emitRecord=*/true, forceProvisional);
			}

			// Derive robust scale immune to bimodal/flat-bottom GOPs where MAD == 0 (e.g., I-B-B-P):
			//   - madSigma   = kMadToSigma * MAD (standard Gaussian MAD scale)
			//   - iqrSigma   = (Q75 - Q25) / (2 * Phi^-1(0.75)), 2 * Phi^-1(0.75) = 1.3489795003921634
			//   - rangeSigma = spread / 4.0 (standard 4-sigma 95% normal range rule)
			// Contrast margin = half standard error of the N-frame sliding window mean (0.5 * robustSigma / sqrt(N)),
			// floored by the half-LSB normalized score quantization floor `quantScoreFloor`.
			constexpr double kNormalIqrPerSigma = 1.3489795003921634;
			const double medScore = RobustStats::Median(uniqueScores, RobustStats::MedianRule::MeanOfMiddle);
			const double mad = RobustStats::MadAbout(uniqueScores, medScore, RobustStats::MedianRule::MeanOfMiddle);
			const double madSigma = RobustStats::kMadToSigma * mad;
			const double q25 = RobustStats::Quantile(uniqueScores, 0.25, RobustStats::RankRule::Interpolated);
			const double q75 = RobustStats::Quantile(uniqueScores, 0.75, RobustStats::RankRule::Interpolated);
			const double iqrSigma = (q75 - q25) / kNormalIqrPerSigma;
			const double rangeSigma = spread / 4.0;
			const double robustSigma = (std::max)(madSigma, (std::max)(iqrSigma, rangeSigma));
			const double windowStdErr = robustSigma / std::sqrt(static_cast<double>(uniqueScores.size()));
			const double margin = (std::max)(0.5 * windowStdErr, quantScoreFloor);

			bool isPeak = false;
			bool isValley = false;
			if (hasPrevUnique && hasNextUnique)
			{
				// Two-sided P15 peak/valley test:
				// - Peak strictly exceeds both unique neighbors by margin.
				// - Valley is strictly below both neighbors OR sits on a flat-bottom valley (e.g. I-B-B-P)
				//   where it is <= both neighbors (within margin) and strictly below at least one by margin.
				if (tScore > prevUniqueScore + margin && tScore > nextUniqueScore + margin)
				{
					isPeak = true;
				}
				else if ((tScore + margin < prevUniqueScore && tScore + margin < nextUniqueScore) ||
				         (tScore <= prevUniqueScore + margin && tScore <= nextUniqueScore + margin &&
				          (tScore + margin < prevUniqueScore || tScore + margin < nextUniqueScore)))
				{
					isValley = true;
				}
			}
			else if (hasPrevUnique && !hasNextUnique)
			{
				// Causal P15c (lead == 0): provisional peak if clearly above previous & median; valley if below both.
				if (tScore > prevUniqueScore + margin && tScore >= medScore)
					isPeak = true;
				else if (tScore + margin < prevUniqueScore && tScore <= medScore)
					isValley = true;
			}
			else if (!hasPrevUnique && hasNextUnique)
			{
				// Start of GOP right after reset: provisional peak if clearly above next & median.
				if (tScore > nextUniqueScore + margin && tScore >= medScore)
					isPeak = true;
				else if (tScore + margin < nextUniqueScore && tScore <= medScore)
					isValley = true;
			}

			// Pass 3B Mini-GOP (4/8/16) cadence lock promotion: when cadence is statistically locked (>= 2 sigma)
			// and this frame lands on the locked peak phase WITHOUT contradicting the window median (not a valley),
			// recover peaks whose single-frame contrast fell slightly below the sliding-window margin.
			if (!isPeak && !isValley &&
			    forensicCfg_.enableForensicGopPeakV2 && forensicCfg_.enableCadenceLock &&
			    targetEntry.hasUniqueIdx && cadenceLock_.IsPeakPhase(targetEntry.uniqueIdx) &&
			    tScore >= medScore - quantScoreFloor)
			{
				isPeak = true;
				dec.cadencePromotedPeak = true;
			}

			if (isPeak)
			{
				r.role = static_cast<uint8_t>(QualityPattern::FrameRole::Peak);
				r.peakValley = static_cast<uint8_t>(QualityPattern::PeakValley::Peak);
			}
			else if (isValley)
			{
				r.role = static_cast<uint8_t>(QualityPattern::FrameRole::NonReference);
				r.peakValley = static_cast<uint8_t>(QualityPattern::PeakValley::Valley);
			}
			else
			{
				r.role = static_cast<uint8_t>(QualityPattern::FrameRole::Reference);
				r.peakValley = static_cast<uint8_t>(QualityPattern::PeakValley::None);
			}

			r.validityBits |= QualityPattern::kValidRole;
			dec.abstain = AbstainReason::None;
			dec.produced = true;
			hasEmittedSeq_ = true;
			lastEmittedSeq_ = target.seq;
			return dec;
		}

		uint8_t leadFrames_ = kMaxLeadFrames;
		ForensicPeakV2Config forensicCfg_{};
		ForensicGopCadenceLock cadenceLock_{};
		std::deque<WindowEntry> window_;
		std::array<uint64_t, static_cast<size_t>(AbstainReason::Count)> abstainCounts_{};
		bool hasEmittedSeq_ = false;
		uint64_t lastEmittedSeq_ = 0;
		uint64_t droppedCount_ = 0;
	};
}
