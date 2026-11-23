// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// ChainPatternVoter (SID-E, Stage S5):
// Pure C++14 P15 (QualityPattern::Record) consumer, blind block-grid period detector, and bounded
// block-grid log-likelihood-ratio (LLR) voter.
//
// Spec & Rule 1 derivations:
//   - Hard LLR cap (doc 78 D15 / OD-7): kMaxBlockGridVoteNats = 2.0 nats. Even if a caller passes a
//     larger maxVoteNats, every vote is hard-clamped internally to [-2.0, +2.0] nats and visibly
//     flagged via wasCapped and gridVotesCappedAt2Nats (Rule 4): Case 3 (positive scaled support)
//     reports reason = ClampedToMaxNats when capped, while Case 1 (negative fine-grid veto) preserves
//     reason = NativeFineGridContradictsUpscale alongside wasCapped = true and gridVotesCappedAt2Nats.
//   - Boundary contrast gate: lineGateFactor = 1.5 (ChainEvidence.h contract): boundary step mean must
//     exceed 1.5x interior step mean (and 1.5 * floorCodes) to count as a real block grid.
//   - Period match tolerance: Scaling::GeometrySet::kConventionTolerance (0.006 relative) with a 0.5-px
//     discrete pixel rounding floor max(0.5, tol * B * S).
//   - Native codec ambiguity guard: periods in {8, 16, 32, 64} are standard H.264/HEVC/AV1 native
//     transform/CTU sizes and NEVER earn positive upscale nats (AmbiguousNativeCodecGrid -> 0.0 nats).
//     An intact 8-px delivered grid when 8*S >= 12 contradicts pure player upscaling (negative nats).
//   - Log-ratio vote scaling: +/- 2.0 * ln(boundaryContrastRatio) (derived Gaussian log-variance ratio
//     2 * ln(sigma_boundary / sigma_interior) in nats), hard-clamped to [-cap, +cap].
//
// Pure C++14, header-only, deterministic doubles, fixed loop order, no Windows headers, no threads/clocks.
// OFF by default (enabled = false); zero silent fallbacks (Rule 4).

#include "ChainCommon.h"
#include "../quality_pattern/QualityPatternRecord.h"

#include <VP-V/modules/scaling/geometry/SourceGeometrySet.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

#pragma fp_contract(off)

namespace SourceIdentity
{
	namespace Chain
	{
		// Hard spec constant (doc 78 D15 / OD-7): block-grid evidence NEVER exceeds +/-2.0 nats.
		constexpr double kMaxBlockGridVoteNats = 2.0;

		enum class PatternVoterReason : int
		{
			Ok = 0,
			Disabled,
			InvalidInput,
			P15NotProduced,
			P15VersionMismatch,
			P15ProvisionalOnly,
			GridAbsentOrWeak,
			AmbiguousNativeCodecGrid,
			ScaledGridSupportsCandidate,
			NativeFineGridContradictsUpscale,
			ClampedToMaxNats
		};

		inline const char* PatternVoterReasonText(PatternVoterReason r)
		{
			switch (r)
			{
			case PatternVoterReason::Ok: return "ok";
			case PatternVoterReason::Disabled: return "pattern voter disabled";
			case PatternVoterReason::InvalidInput: return "invalid input";
			case PatternVoterReason::P15NotProduced: return "P15 quality pattern record not produced";
			case PatternVoterReason::P15VersionMismatch: return "P15 quality pattern unit version mismatch";
			case PatternVoterReason::P15ProvisionalOnly: return "P15 quality pattern record is provisional/causal only";
			case PatternVoterReason::GridAbsentOrWeak: return "block grid absent, weak, or unmatched";
			case PatternVoterReason::AmbiguousNativeCodecGrid: return "ambiguous native codec block grid (abstain)";
			case PatternVoterReason::ScaledGridSupportsCandidate: return "non-native scaled block grid supports candidate";
			case PatternVoterReason::NativeFineGridContradictsUpscale: return "native 8-px fine grid contradicts pure upscale";
			case PatternVoterReason::ClampedToMaxNats: return "block-grid vote clamped to 2.0-nat hard cap";
			default: return "unknown";
			}
		}

		struct PatternVoterSettings
		{
			bool enabled = false;                                                    // OFF by default (Stage S5 safe contract)
			double maxVoteNats = kMaxBlockGridVoteNats;                              // Hard-clamped internally to <= kMaxBlockGridVoteNats
			double lineGateFactor = 1.5;                                             // ChainEvidence.h contract: boundary contrast >= 1.5x
			double periodMatchTolerance = Scaling::GeometrySet::kConventionTolerance;// Existing 0.006 relative tolerance (+ 0.5 px rounding)
		};

		struct PatternConsumeResult
		{
			bool available = false;
			bool provisional = false;
			QualityPattern::FrameRole role = QualityPattern::FrameRole::Unknown;
			QualityPattern::PeakValley peakValley = QualityPattern::PeakValley::None;
			bool hasScore = false;
			double qualityScore = 0.0;
			bool hasSteps = false;
			double stepMean = 0.0;
			double stepP90 = 0.0;
			double stepMax = 0.0;
			bool hasGrid = false;
			int gridPeriodX = 0;
			int gridPeriodY = 0;
			int gridOffsetX = 0;
			int gridOffsetY = 0;
			bool isExactCopy = false;
			uint8_t exactCopyFlags = 0;
			PatternVoterReason reason = PatternVoterReason::Disabled;
		};

		struct BlindBlockGridResult
		{
			bool valid = false;
			bool gridDetected = false;
			int dominantPeriodPx = 0;
			int dominantPhasePx = 0;
			double boundaryContrastRatio = 1.0;
			PatternVoterReason reason = PatternVoterReason::Disabled;
		};

		struct BlockGridVoteResult
		{
			bool valid = false;
			double llrNats = 0.0;         // Guaranteed strictly within [-2.0, +2.0] nats
			double rawUncappedNats = 0.0; // Pre-cap LLR preserved visibly for diagnostics (Rule 4)
			bool wasCapped = false;       // True when |rawUncappedNats| > effectiveCap (<= 2.0 nats)
			bool provisional = false;     // True when fed from a provisional/causal P15 record (Rule 4)
			int observedPeriodPx = 0;
			int matchedCodecBlockPx = 0;  // 8 or 16 when matched, 0 otherwise
			PatternVoterReason reason = PatternVoterReason::Disabled;
		};

		struct PatternVoterCounters
		{
			uint64_t p15Consumed = 0;
			uint64_t p15Unavailable = 0;
			uint64_t gridVotesPositive = 0;
			uint64_t gridVotesNegative = 0;
			uint64_t gridVotesAbstainedAmbiguous = 0;
			uint64_t gridVotesCappedAt2Nats = 0;
		};

		// Standard H.264 / HEVC / AV1 native transform / CTU block periods in pixels.
		inline bool IsNativeCodecBlockPeriod(int periodPx)
		{
			return periodPx == 8 || periodPx == 16 || periodPx == 32 || periodPx == 64;
		}

		namespace detail_pattern_voter
		{
			constexpr uint32_t kAnyDataValidBits =
				QualityPattern::kValidRole |
				QualityPattern::kValidScore |
				QualityPattern::kValidRank |
				QualityPattern::kValidSteps |
				QualityPattern::kValidGrid |
				QualityPattern::kValidCopy;

			inline bool ValidLineSet(const LineSet& ls, int minLen)
			{
				if (ls.count <= 0 || ls.length < minLen)
					return false;
				if (ls.data.size() / static_cast<size_t>(ls.length) < static_cast<size_t>(ls.count))
					return false;
				return true;
			}
		} // namespace detail_pattern_voter

		// ============================================================================================================
		// 1. P15 QualityPattern::Record Consumer (Visible Absent / Version / Provisional Handling)
		// ============================================================================================================
		inline PatternConsumeResult ConsumeQualityPattern(
			const QualityPattern::Record* rec,
			const PatternVoterSettings& s = PatternVoterSettings(),
			PatternVoterCounters* counters = nullptr)
		{
			PatternConsumeResult res;
			if (!s.enabled)
			{
				res.reason = PatternVoterReason::Disabled;
				return res;
			}
			if (rec == nullptr || rec->unit == 0u)
			{
				if (counters != nullptr)
					++counters->p15Unavailable;
				res.available = false;
				res.reason = PatternVoterReason::P15NotProduced;
				return res;
			}
			if (rec->unit != QualityPattern::kUnitVersion)
			{
				if (counters != nullptr)
					++counters->p15Unavailable;
				res.available = false;
				res.reason = PatternVoterReason::P15VersionMismatch;
				return res;
			}
			// A record with zero populated data-validity bits has no produced P15 content (Rule 4).
			if ((rec->validityBits & detail_pattern_voter::kAnyDataValidBits) == 0u)
			{
				if (counters != nullptr)
					++counters->p15Unavailable;
				res.available = false;
				res.reason = PatternVoterReason::P15NotProduced;
				return res;
			}

			res.available = true;
			if (counters != nullptr)
				++counters->p15Consumed;

			const uint32_t vb = rec->validityBits;
			res.provisional = ((vb & QualityPattern::kProvisional) != 0u) || (rec->causal != 0u);

			if ((vb & QualityPattern::kValidRole) != 0u &&
				rec->role <= static_cast<uint8_t>(QualityPattern::FrameRole::ExactCopy))
			{
				res.role = static_cast<QualityPattern::FrameRole>(rec->role);
			}

			if ((vb & QualityPattern::kValidRank) != 0u &&
				rec->peakValley <= static_cast<uint8_t>(QualityPattern::PeakValley::Valley))
			{
				res.peakValley = static_cast<QualityPattern::PeakValley>(rec->peakValley);
			}

			if ((vb & QualityPattern::kValidScore) != 0u && std::isfinite(rec->score))
			{
				res.hasScore = true;
				res.qualityScore = static_cast<double>(rec->score);
			}

			// Q8.8 fixed-point step summary decoded by exact division by 256.0 (2^8).
			if ((vb & QualityPattern::kValidSteps) != 0u)
			{
				res.hasSteps = true;
				res.stepMean = static_cast<double>(rec->stepMean) / 256.0;
				res.stepP90 = static_cast<double>(rec->stepP90) / 256.0;
				res.stepMax = static_cast<double>(rec->stepMax) / 256.0;
			}

			if ((vb & QualityPattern::kValidGrid) != 0u && (rec->gridPeriodX > 0u || rec->gridPeriodY > 0u))
			{
				static_assert(sizeof(QualityPattern::Record) == QualityPattern::kRecordBytes, "P15 POD size pinned");
				res.hasGrid = true;
				res.gridPeriodX = static_cast<int>(rec->gridPeriodX);
				res.gridPeriodY = static_cast<int>(rec->gridPeriodY);
				// Read pinned P15 64-byte POD byte offsets 38 and 39 (QualityPatternRecord.h:69-70, 76-80).
				uint8_t gx = 0u;
				uint8_t gy = 0u;
				std::memcpy(&gx, reinterpret_cast<const uint8_t*>(rec) + 38, 1);
				std::memcpy(&gy, reinterpret_cast<const uint8_t*>(rec) + 39, 1);
				res.gridOffsetX = static_cast<int>(gx);
				res.gridOffsetY = static_cast<int>(gy);
			}

			if ((vb & QualityPattern::kValidCopy) != 0u)
			{
				res.exactCopyFlags = rec->exactCopyFlags;
				res.isExactCopy = (rec->exactCopyFlags != 0u);
			}
			if (res.role == QualityPattern::FrameRole::ExactCopy)
				res.isExactCopy = true;

			res.reason = res.provisional ? PatternVoterReason::P15ProvisionalOnly : PatternVoterReason::Ok;
			return res;
		}

		// ============================================================================================================
		// 2. Blind Block-Grid Period Detector (Scans {8, 12, 16, 24, 32, 48} px)
		// ============================================================================================================
		inline BlindBlockGridResult MeasureBlindBlockGridPeriod(
			const LineSet& ls,
			double floorCodes,
			const PatternVoterSettings& s = PatternVoterSettings())
		{
			BlindBlockGridResult res;
			if (!s.enabled)
			{
				res.reason = PatternVoterReason::Disabled;
				return res;
			}
			if (!detail_pattern_voter::ValidLineSet(ls, 17) ||
				!std::isfinite(floorCodes) || !(floorCodes > 0.0) ||
				!std::isfinite(s.lineGateFactor) || !(s.lineGateFactor > 1.0))
			{
				res.reason = PatternVoterReason::InvalidInput;
				return res;
			}

			// Verify finite samples and collect informative lines exceeding lineGateFactor * floorCodes.
			const double detailGate = s.lineGateFactor * floorCodes;
			std::vector<int> activeLines;
			activeLines.reserve(static_cast<size_t>(ls.count));
			for (int li = 0; li < ls.count; ++li)
			{
				const double* line = ls.Line(li);
				double sum = 0.0;
				for (int j = 0; j < ls.length; ++j)
				{
					if (!std::isfinite(line[j]))
					{
						res.reason = PatternVoterReason::InvalidInput;
						return res;
					}
					sum += line[j];
				}
				const double mean = sum / static_cast<double>(ls.length);
				double sumSq = 0.0;
				for (int j = 0; j < ls.length; ++j)
				{
					const double d = line[j] - mean;
					sumSq += d * d;
				}
				const double acRms = std::sqrt(sumSq / static_cast<double>(ls.length));
				if (acRms >= detailGate)
					activeLines.push_back(li);
			}

			if (activeLines.empty())
			{
				res.valid = true;
				res.gridDetected = false;
				res.boundaryContrastRatio = 1.0;
				res.reason = PatternVoterReason::GridAbsentOrWeak;
				return res;
			}

			// Single division-free LCM(8, 12, 16, 24, 32, 48) = 96 bin accumulation pass over all informative lines.
			// Preserves exact non-zero ls.origin modulo semantics: rem == ((ls.origin + j) % 96 + 96) % 96.
			constexpr int kGridLcm = 96;
			double binSum[kGridLcm] = {};
			uint64_t binCnt[kGridLcm] = {};
			double totalAbsDiff = 0.0;
			uint64_t totalCount = 0;
			const int startRem = (((ls.origin + 1) % kGridLcm) + kGridLcm) % kGridLcm;

			for (size_t ai = 0; ai < activeLines.size(); ++ai)
			{
				const double* line = ls.Line(activeLines[ai]);
				int rem = startRem;
				for (int j = 1; j < ls.length; ++j)
				{
					const double d = std::fabs(line[j] - line[j - 1]);
					binSum[rem] += d;
					++binCnt[rem];
					totalAbsDiff += d;
					++totalCount;
					if (++rem == kGridLcm)
						rem = 0;
				}
			}

			// Fold the 96-bin histogram into each candidate (P, ph) in ascending P order so fundamental periods
			// win any tie against multiples (e.g. 8 over 16/24/32/48).
			static const int kCandidatePeriods[6] = { 8, 12, 16, 24, 32, 48 };
			int bestPeriod = 0;
			int bestPhase = 0;
			double bestRatio = 1.0;
			double bestBoundaryMean = 0.0;

			for (int pi = 0; pi < 6; ++pi)
			{
				const int P = kCandidatePeriods[pi];
				// Require >= 2*P + 1 samples (2*P diffs) so every phase ph in [0, P-1] gets >= 2 boundaries.
				if (ls.length < 2 * P + 1)
					continue;
				for (int ph = 0; ph < P; ++ph)
				{
					double bSum = 0.0;
					uint64_t bCnt = 0;
					for (int k = ph; k < kGridLcm; k += P)
					{
						bSum += binSum[k];
						bCnt += binCnt[k];
					}
					const uint64_t iCnt = totalCount > bCnt ? (totalCount - bCnt) : 0u;
					if (bCnt < 2u || iCnt < 2u)
						continue;
					const double iSum = totalAbsDiff > bSum ? (totalAbsDiff - bSum) : 0.0;
					const double bMean = bSum / static_cast<double>(bCnt);
					const double iMean = iSum / static_cast<double>(iCnt);
					if (bMean < detailGate)
						continue;
					const double ratio = bMean / (std::max)(iMean, floorCodes);
					// Require a strict relative improvement (> 1e-6) for a larger period to beat a smaller fundamental period.
					if (ratio > bestRatio * (1.0 + 1e-6))
					{
						bestRatio = ratio;
						bestPeriod = P;
						bestPhase = ph;
						bestBoundaryMean = bMean;
					}
				}
			}

			res.valid = true;
			res.boundaryContrastRatio = bestRatio;
			if (bestPeriod > 0 && bestRatio >= s.lineGateFactor && bestBoundaryMean >= detailGate)
			{
				res.gridDetected = true;
				res.dominantPeriodPx = bestPeriod;
				res.dominantPhasePx = bestPhase;
				res.reason = PatternVoterReason::Ok;
			}
			else
			{
				res.gridDetected = false;
				res.dominantPeriodPx = 0;
				res.dominantPhasePx = 0;
				res.reason = PatternVoterReason::GridAbsentOrWeak;
			}
			return res;
		}

		// ============================================================================================================
		// 3. Hard-Capped Block-Grid LLR Voter (Strictly Bounded to [-2.0, +2.0] Nats)
		// ============================================================================================================
		inline BlockGridVoteResult VoteFromBlockGrid(
			int candidateN,
			int axisLen,
			int observedPeriodPx,
			double boundaryContrastRatio,
			const PatternVoterSettings& s = PatternVoterSettings(),
			PatternVoterCounters* counters = nullptr)
		{
			BlockGridVoteResult res;
			if (!s.enabled)
			{
				res.reason = PatternVoterReason::Disabled;
				return res;
			}
			if (candidateN <= 0 || axisLen <= 0 || candidateN > axisLen ||
				!std::isfinite(boundaryContrastRatio) || boundaryContrastRatio < 0.0 ||
				!std::isfinite(s.maxVoteNats) || s.maxVoteNats < 0.0 ||
				!std::isfinite(s.lineGateFactor) || !(s.lineGateFactor > 1.0) ||
				!std::isfinite(s.periodMatchTolerance) || s.periodMatchTolerance < 0.0)
			{
				res.reason = PatternVoterReason::InvalidInput;
				return res;
			}

			// Hard cap enforced unconditionally even if caller configures maxVoteNats > kMaxBlockGridVoteNats.
			const double effectiveCap = ClampD(s.maxVoteNats, 0.0, kMaxBlockGridVoteNats);
			res.observedPeriodPx = observedPeriodPx;

			if (observedPeriodPx <= 0 || boundaryContrastRatio < s.lineGateFactor)
			{
				res.valid = true;
				res.llrNats = 0.0;
				res.rawUncappedNats = 0.0;
				res.wasCapped = false;
				res.reason = PatternVoterReason::GridAbsentOrWeak;
				return res;
			}

			// Case 1: Intact native 8-px delivered grid contradicts pure upscale when 8 * (N / n) >= 12.
			const bool scaledEightExceeds12 =
				(static_cast<int64_t>(8) * static_cast<int64_t>(axisLen)) >=
				(static_cast<int64_t>(12) * static_cast<int64_t>(candidateN));
			if (observedPeriodPx == 8 && scaledEightExceeds12)
			{
				const double raw = -2.0 * std::log(boundaryContrastRatio);
				const double clamped = ClampD(raw, -effectiveCap, 0.0);
				const bool capped = (std::fabs(raw) > effectiveCap);
				res.valid = true;
				res.rawUncappedNats = raw;
				res.llrNats = clamped;
				res.wasCapped = capped;
				res.matchedCodecBlockPx = 8;
				res.reason = PatternVoterReason::NativeFineGridContradictsUpscale;
				if (counters != nullptr)
				{
					++counters->gridVotesNegative;
					if (capped)
						++counters->gridVotesCappedAt2Nats;
				}
				return res;
			}

			// Case 2: Native power-of-two codec block periods {8, 16, 32, 64} NEVER earn positive upscale nats.
			if (IsNativeCodecBlockPeriod(observedPeriodPx))
			{
				res.valid = true;
				res.llrNats = 0.0;
				res.rawUncappedNats = 0.0;
				res.wasCapped = false;
				res.reason = PatternVoterReason::AmbiguousNativeCodecGrid;
				if (counters != nullptr)
					++counters->gridVotesAbstainedAmbiguous;
				return res;
			}

			// Case 3: Non-native scaled period matching B * (N / n) for codec block size B in {8, 16}.
			const double scale = static_cast<double>(axisLen) / static_cast<double>(candidateN);
			static const int kCodecBlocks[2] = { 8, 16 };
			for (int bi = 0; bi < 2; ++bi)
			{
				const int blockPx = kCodecBlocks[bi];
				const double expected = static_cast<double>(blockPx) * scale;
				const double tol = (std::max)(0.5, s.periodMatchTolerance * expected);
				if (std::fabs(static_cast<double>(observedPeriodPx) - expected) <= tol)
				{
					const double raw = 2.0 * std::log(boundaryContrastRatio);
					const double clamped = ClampD(raw, 0.0, effectiveCap);
					const bool capped = (raw > effectiveCap);
					res.valid = true;
					res.rawUncappedNats = raw;
					res.llrNats = clamped;
					res.wasCapped = capped;
					res.matchedCodecBlockPx = blockPx;
					res.reason = capped ? PatternVoterReason::ClampedToMaxNats : PatternVoterReason::ScaledGridSupportsCandidate;
					if (counters != nullptr)
					{
						++counters->gridVotesPositive;
						if (capped)
							++counters->gridVotesCappedAt2Nats;
					}
					return res;
				}
			}

			// Case 4: Observed period does not match candidate scale.
			res.valid = true;
			res.llrNats = 0.0;
			res.rawUncappedNats = 0.0;
			res.wasCapped = false;
			res.reason = PatternVoterReason::GridAbsentOrWeak;
			return res;
		}

		// ============================================================================================================
		// 4. Stateful Convenience Wrapper (PatternVoterSettings + PatternVoterCounters)
		// ============================================================================================================
		class ChainPatternVoter
		{
		public:
			explicit ChainPatternVoter(const PatternVoterSettings& s = PatternVoterSettings())
				: settings_(s)
			{
			}

			const PatternVoterSettings& Settings() const { return settings_; }
			void SetSettings(const PatternVoterSettings& s) { settings_ = s; }

			PatternConsumeResult Consume(const QualityPattern::Record* rec)
			{
				return ConsumeQualityPattern(rec, settings_, &counters_);
			}

			BlockGridVoteResult VoteFromP15(
				const QualityPattern::Record* rec,
				int axis,
				int candidateN,
				int axisLen,
				double boundaryContrastRatio)
			{
				BlockGridVoteResult res;
				if (!settings_.enabled)
				{
					res.reason = PatternVoterReason::Disabled;
					return res;
				}
				if (axis != 0 && axis != 1)
				{
					res.reason = PatternVoterReason::InvalidInput;
					return res;
				}
				const PatternConsumeResult consumed = Consume(rec);
				if (!consumed.available)
				{
					res.valid = false;
					res.reason = consumed.reason;
					return res;
				}
				if (!consumed.hasGrid)
				{
					res.valid = true;
					res.llrNats = 0.0;
					res.provisional = consumed.provisional;
					res.reason = PatternVoterReason::GridAbsentOrWeak;
					return res;
				}
				const int observedPeriod = axis == 0 ? consumed.gridPeriodX : consumed.gridPeriodY;
				BlockGridVoteResult vote = VoteFromBlockGrid(candidateN, axisLen, observedPeriod, boundaryContrastRatio, settings_, &counters_);
				vote.provisional = consumed.provisional;
				return vote;
			}

			BlockGridVoteResult VoteBlind(const LineSet& ls, int candidateN, double floorCodes)
			{
				BlockGridVoteResult res;
				if (!settings_.enabled)
				{
					res.reason = PatternVoterReason::Disabled;
					return res;
				}
				if (candidateN <= 0 || ls.axisLen <= 0 || candidateN > ls.axisLen)
				{
					res.reason = PatternVoterReason::InvalidInput;
					return res;
				}
				const BlindBlockGridResult grid = MeasureBlindBlockGridPeriod(ls, floorCodes, settings_);
				if (!grid.valid)
				{
					res.valid = false;
					res.reason = grid.reason;
					return res;
				}
				if (!grid.gridDetected)
				{
					res.valid = true;
					res.llrNats = 0.0;
					res.reason = PatternVoterReason::GridAbsentOrWeak;
					return res;
				}
				return VoteFromBlockGrid(candidateN, ls.axisLen, grid.dominantPeriodPx, grid.boundaryContrastRatio, settings_, &counters_);
			}

			const PatternVoterCounters& Counters() const { return counters_; }
			void Reset() { counters_ = PatternVoterCounters{}; }

		private:
			PatternVoterSettings settings_;
			PatternVoterCounters counters_;
		};
	}
}

#pragma fp_contract(on)
