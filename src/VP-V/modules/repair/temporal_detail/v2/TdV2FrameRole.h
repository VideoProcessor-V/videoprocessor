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
			double sumHorizBoundaryRatio = 0.0;
			double sumVertBoundaryRatio = 0.0;
			size_t ratioCellsX = 0;
			size_t ratioCellsY = 0;

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

					double sad1 = 0.0, sad2 = 0.0;
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
									sad1 += std::fabs(v - p1);
								else
									cellNonFinite = true;
							}
							if (prev2Y != nullptr)
							{
								const double p2 = prev2Y[idx];
								if (std::isfinite(p2))
									sad2 += std::fabs(v - p2);
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
					if (boundXCnt > 0 && bx > copyTol)
					{
						sumHorizBoundaryRatio += rx;
						++ratioCellsX;
					}
					if (boundYCnt > 0 && by > copyTol)
					{
						sumVertBoundaryRatio += ry;
						++ratioCellsY;
					}

					if (pxCnt > 0)
					{
						if (prev1Y != nullptr && (sad1 / static_cast<double>(pxCnt)) <= copyTol)
						{
							cell.copyPrev1 = true;
							++copy1Count;
						}
						if (prev2Y != nullptr && (sad2 / static_cast<double>(pxCnt)) <= copyTol)
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
				if (ratioCellsX > 0 && (sumHorizBoundaryRatio / static_cast<double>(ratioCellsX)) > 1.0)
					out.detectedPeriodX = static_cast<uint16_t>(kTransformSubCellPx);
				if (ratioCellsY > 0 && (sumVertBoundaryRatio / static_cast<double>(ratioCellsY)) > 1.0)
					out.detectedPeriodY = static_cast<uint16_t>(kTransformSubCellPx);
				out.valid = true;
			}
			return out;
		}
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
		case AbstainReason::Count: break;
		}
		return "unknown";
	}

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
		bool isCut = false;               // External P4 / MotionValidity transition flag
		bool isFade = false;              // External P4 fade flag
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

	struct FrameRoleDecision
	{
		QualityPattern::Record record{};  // Exact 64-byte skeleton POD
		AbstainReason abstain = AbstainReason::None;
		bool produced = false;
	};

	class TdV2FrameRoleDetector
	{
	public:
		// Standard sliding-window capacity: 2 past + center + 2 ahead = 5 core slots (history kept up to 9 for robust MAD).
		static constexpr size_t kMaxWindowFrames = 9;
		static constexpr uint8_t kMaxLeadFrames = 2;

		explicit TdV2FrameRoleDetector(uint8_t leadFrames = kMaxLeadFrames)
			: leadFrames_(leadFrames > kMaxLeadFrames ? kMaxLeadFrames : leadFrames)
		{
		}

		void SetLeadFrames(uint8_t leadFrames)
		{
			leadFrames_ = leadFrames > kMaxLeadFrames ? kMaxLeadFrames : leadFrames;
		}

		uint8_t LeadFrames() const { return leadFrames_; }

		void Reset()
		{
			window_.clear();
			hasEmittedSeq_ = false;
			lastEmittedSeq_ = 0;
		}

		uint64_t AbstainCount(AbstainReason r) const
		{
			const size_t idx = static_cast<size_t>(r);
			return idx < abstainCounts_.size() ? abstainCounts_[idx] : 0;
		}

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
			if (!std::isfinite(ev.qualityScore) || !std::isfinite(ev.stepMean) ||
			    !std::isfinite(ev.stepP90) || !std::isfinite(ev.stepMax) ||
			    !std::isfinite(ev.exactCopyFracPrev1) || !std::isfinite(ev.exactCopyFracPrev2) ||
			    PeakCodeForBits(ev.sourceBits) <= 0.0)
			{
				if (standIns != nullptr)
					standIns->Engage(StandInKind::NonFiniteClosure, "TdV2FrameRoleDetector non-finite or invalid sourceBits");
				return Abstain(dec, AbstainReason::NonFiniteEvidence, ev.seq, leadFrames_, /*emitRecord=*/true);
			}

			// Cut immediately flushes temporal window so peaks are NEVER bridged across cuts.
			// When leadFrames_ > 0, the cut frame stays buffered as the new post-cut anchor and is NOT
			// emitted prematurely (produced = false), preventing duplicate seq emission when lead frames arrive.
			if (ev.isCut)
			{
				window_.clear();
				window_.push_back(ev);
				const bool emitNow = (leadFrames_ == 0);
				if (emitNow)
				{
					hasEmittedSeq_ = true;
					lastEmittedSeq_ = ev.seq;
				}
				return Abstain(dec, AbstainReason::CutReset, ev.seq, leadFrames_, emitNow);
			}

			window_.push_back(ev);
			if (window_.size() > kMaxWindowFrames)
				window_.pop_front();

			// Need at least leadFrames_ + 1 frames in window to emit the target frame at index (size - 1 - lead).
			// While warming up, produced = false so ev.seq is never emitted twice.
			if (window_.size() <= static_cast<size_t>(leadFrames_))
			{
				return Abstain(dec, AbstainReason::InsufficientWindow, ev.seq, leadFrames_, /*emitRecord=*/false);
			}

			const size_t targetIdx = window_.size() - 1 - static_cast<size_t>(leadFrames_);
			const FrameRoleEvidence& target = window_[targetIdx];
			return ClassifyTarget(targetIdx, target);
		}

		// Flush remaining buffered frames at stream end (using available reduced lead, marked provisional).
		FrameRoleDecision ClassifyLatestCausal(StandInRegistry* standIns = nullptr)
		{
			if (standIns != nullptr)
				standIns->Engage(StandInKind::P15FramePattern, "causal provisional");
			FrameRoleDecision dec;
			dec.record.unit = QualityPattern::kUnitVersion;
			if (window_.empty())
				return Abstain(dec, AbstainReason::InsufficientWindow, 0, 0, /*emitRecord=*/false);
			const size_t idx = window_.size() - 1;
			if (hasEmittedSeq_ && window_[idx].seq == lastEmittedSeq_)
				return Abstain(dec, AbstainReason::InsufficientWindow, window_[idx].seq, 0, /*emitRecord=*/false);
			return ClassifyTargetWithLead(idx, window_[idx], 0);
		}

	private:
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

		FrameRoleDecision Abstain(FrameRoleDecision& dec, AbstainReason reason, uint64_t seq, uint8_t leadUsed, bool emitRecord)
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
			if (leadUsed < kMaxLeadFrames)
				dec.record.validityBits |= QualityPattern::kProvisional;
			if (emitRecord)
			{
				hasEmittedSeq_ = true;
				lastEmittedSeq_ = seq;
			}
			return dec;
		}

		FrameRoleDecision ClassifyTarget(size_t targetIdx, const FrameRoleEvidence& target)
		{
			return ClassifyTargetWithLead(targetIdx, target, leadFrames_);
		}

		FrameRoleDecision ClassifyTargetWithLead(size_t targetIdx, const FrameRoleEvidence& target, uint8_t leadUsed)
		{
			FrameRoleDecision dec;
			QualityPattern::Record& r = dec.record;
			r.unit = QualityPattern::kUnitVersion;
			r.seq = target.seq;
			r.leadUsed = leadUsed;
			r.causal = (leadUsed == 0) ? 1u : 0u;
			r.score = static_cast<float>(RampMath::Clamp01(target.qualityScore));
			r.stepMean = ToQ88(target.stepMean);
			r.stepP90 = ToQ88(target.stepP90);
			r.stepMax = ToQ88(target.stepMax);
			r.gridPeriodX = target.periodX;
			r.gridPeriodY = target.periodY;
			r.validityBits = QualityPattern::kValidScore | QualityPattern::kValidSteps | QualityPattern::kValidCopy;
			if (target.periodX > 0 || target.periodY > 0)
				r.validityBits |= QualityPattern::kValidGrid;
			if (leadUsed < kMaxLeadFrames)
				r.validityBits |= QualityPattern::kProvisional;

			// Fade active -> abstain visibly rather than guessing peaks during a ramp.
			if (target.isFade)
			{
				return Abstain(dec, AbstainReason::FadeActive, target.seq, leadUsed, /*emitRecord=*/true);
			}

			// Cadence repeat check (3:2 pulldown): ExactCopy MUST NEVER be promoted to Peak!
			uint8_t copyFlags = 0;
			if (IsExactCopy(target, copyFlags))
			{
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

			// Collect unique (non-ExactCopy, non-fade) frames in window for ranking and neighbor comparison.
			std::vector<double> uniqueScores;
			uniqueScores.reserve(window_.size());
			double prevUniqueScore = 0.0;
			bool hasPrevUnique = false;
			double nextUniqueScore = 0.0;
			bool hasNextUnique = false;

			for (size_t i = 0; i < window_.size(); ++i)
			{
				if (window_[i].isFade)
					continue;
				uint8_t cf = 0;
				if (i != targetIdx && IsExactCopy(window_[i], cf))
					continue;
				const double sc = RampMath::Clamp01(window_[i].qualityScore);
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
				return Abstain(dec, AbstainReason::InsufficientWindow, target.seq, leadUsed, /*emitRecord=*/true);
			}

			// Compute rank (0 = highest score) and robust spread via U24 RobustStats.
			const double tScore = RampMath::Clamp01(target.qualityScore);
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

			// Quantization floor on quality score contrast: half a code step normalized by peak code.
			// Fail closed on invalid sourceBits (zero silent fallback).
			const double peakCode = PeakCodeForBits(target.sourceBits);
			if (!(peakCode > 0.0))
			{
				return Abstain(dec, AbstainReason::NonFiniteEvidence, target.seq, leadUsed, /*emitRecord=*/true);
			}
			const double quantScoreFloor = 0.5 / peakCode;
			const double spread = maxSc - minSc;
			if (spread < 2.0 * quantScoreFloor)
			{
				return Abstain(dec, AbstainReason::LowRhythmContrast, target.seq, leadUsed, /*emitRecord=*/true);
			}

			// Derive robust scale immune to bimodal/flat-bottom GOPs where MAD == 0 (e.g., I-B-B-P):
			//   - madSigma = kMadToSigma * MAD (standard Gaussian MAD scale)
			//   - iqrSigma = (Q75 - Q25) / (2 * Phi^-1(0.75)), 2 * Phi^-1(0.75) = 1.3489795003921634
			//   - rangeSigma = spread / 4.0 (standard 4-sigma 95% range rule)
			// Minimum margin = 0.25 * max(madSigma, iqrSigma, rangeSigma), floored by quantScoreFloor.
			constexpr double kNormalIqrPerSigma = 1.3489795003921634;
			const double medScore = RobustStats::Median(uniqueScores, RobustStats::MedianRule::MeanOfMiddle);
			const double mad = RobustStats::MadAbout(uniqueScores, medScore, RobustStats::MedianRule::MeanOfMiddle);
			const double madSigma = RobustStats::kMadToSigma * mad;
			const double q25 = RobustStats::Quantile(uniqueScores, 0.25, RobustStats::RankRule::Interpolated);
			const double q75 = RobustStats::Quantile(uniqueScores, 0.75, RobustStats::RankRule::Interpolated);
			const double iqrSigma = (q75 - q25) / kNormalIqrPerSigma;
			const double rangeSigma = spread / 4.0;
			const double robustSigma = (std::max)(madSigma, (std::max)(iqrSigma, rangeSigma));
			const double margin = (std::max)(robustSigma / 4.0, quantScoreFloor);

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
		std::deque<FrameRoleEvidence> window_;
		std::array<uint64_t, static_cast<size_t>(AbstainReason::Count)> abstainCounts_{};
		bool hasEmittedSeq_ = false;
		uint64_t lastEmittedSeq_ = 0;
	};
}
