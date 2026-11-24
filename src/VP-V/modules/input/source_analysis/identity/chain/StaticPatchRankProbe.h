// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// StaticPatchRankProbe (SID Stage S2, Multi-Frame Static-Patch Cross-Covariance Rank-Cliff Detector):
// Pure standard-library C++14, header-only, deterministic multi-frame rank-cliff detector operating on
// sampled 1-D luma lines (LineSet). Exposes low-rank linear resampling cliffs on heavily grainy or
// compressed 8-bit 4:2:0 streams where single-frame analysis is buried under temporal noise, while
// abstaining on native broadband streams and moving-camera shots. Zero pixel modification.
//
// Signal-processing physics & Rule 1 derivations (zero arbitrary constants):
//   1. First-difference pre-whitening (dx[i] = x[i+1] - x[i], m = 8 taps over m+1 = 9 pixels):
//        - Linear LTI operator: preserves exact Landau-Pollak bandlimited rank r = round(alpha * m)
//          for any linear upscaler of ratio alpha (e.g., r = 2 for 1/4, 4 for 1/2, 6 for 3/4).
//        - Annihilates per-patch DC without reducing the 8-D subspace dimension (zero structural rank loss).
//        - Pre-whitens natural 1/f^2 native spectra via |2 sin(pi f)|^2, flattening native broadband
//          eigenvalues so native 2160p stays full-rank (r = 8).
//        - Exact m = 8 matches the 8-px codec transform block width and resolves canonical k/8 ratios.
//   2. Lag-1 cross-covariance temporal noise cancellation:
//        For static patches x_t = s + n_t with i.i.d. temporal grain n_t (E[n_t n_{t-1}^T] = 0):
//          E[dx_t dx_{t-1}^T] = E[dx_s dx_s^T]  (exact symmetric PSD signal covariance, zero grain bias!).
//        By contrast, single-frame covariance E[dx_t dx_t^T] = E[dx_s dx_s^T] + C_dn adds high-frequency
//        grain eigenvalues 4 * sigma_n^2 * sin^2(k pi / 18) directly into the trailing modes.
//   3. Spatial noise estimate (motion-invariant, matching ChainEdgeStats.h):
//        Second difference d_i = y_{i-1} - 2*y_i + y_{i+1} has Var(d_i) = 6 * sigma^2 for white noise,
//        so sigma_seg = (RobustStats::kMadToSigma * MAD(d)) / sqrt(6). Lower quartile (q = 1/4) over
//        kSegPx = 256-px segments isolates grain/floor from sharp edges; clamped below by floorCodes.
//   4. Two-stage motion rejection (zero covariance pollution on moving cameras):
//        - Stage 1 (per-patch chi-square gate): for static patches, r[i] = x_t[i] - x_{t-1}[i] has
//          Var(r[i]) = 2 * sigma_spatial^2; mean-square over m = 8 samples has upper 3-sigma chi-square
//          bound 2 * sigma_spatial^2 * (1 + motionChiSigma * sqrt(2 / m)). Fewer than minStaticPatchFraction
//          (elementary quarter 1/4) static patches rejects the frame transition.
//        - Stage 2 (directional shift & sub-pixel asymmetry gate): on any static scene, AM-GM gives
//          0.5*(dx[i]^2 + dx[i+d]^2) >= dx[i]*dx[i+d] pointwise and E[dx_t[i]*dx_{t-1}[i+1] -
//          dx_t[i+1]*dx_{t-1}[i]] = 0 identically. Significant off-diagonal peak excess or lag-1
//          asymmetry (> 3-sigma sampling SE) catches slow 0.5-3 px pans even on uniformly textured frames.
//   5. Rank-cliff & high-frequency nullspace criteria:
//        - Sampling noise floor of dx cross-covariance: 2 * sigma_spatial^2 / sqrt(acceptedStaticPairs),
//          combined with empirical Wigner symmetry floor max(0, -lambda_min) of the PSD cross-covariance.
//        - Cliff ratio >= minCliffEigenRatio = 4.0 (2^2, 6 dB eigenvalue drop) AND trailing energy share
//          <= maxTrailingEnergyShare = 1 / (2 * m) = 1/16 = 0.0625.
//        - High-frequency nullspace guard via 1-D discrete Neumann Laplacian Rayleigh quotient:
//          mean trailing roughness >= 2.0 (= 4 * sin^2(pi/4), exact half-Nyquist midpoint) and > leading,
//          plus cliff mode roughness >= 0.5 * 4 * sin^2(r*pi/(2m)) when well above noise, blocking false
//          triggers from first-difference f = 0 suppression on native white-noise textures.
//        - Top signal RMS >= minSignalToFloorRatio * floorCodes (1.5, ChainEvidence.h::lineGateFactor).
//
// Rule 2: Zero static or thread_local mutable state; all history and scratch live in caller-owned
//         strictly bounded StaticPatchWorkspace (<= kStaticPatchMaxSlots = 512, <= kStaticPatchMaxFrames = 8).
// Rule 4: Zero silent fallbacks; every disabled, warming-up, rejected, low-SNR, or native outcome returns
//         an explicit StaticRankReason and populates diagnostic counters.
// Default-OFF contract: StaticPatchRankSettings::enabled = false by default (100% W1 bit-exact when OFF).

#include "ChainCommon.h"
#include "../../../../../core/RobustStats.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

#pragma fp_contract(off)

namespace SourceIdentity
{
	namespace Chain
	{
		// Exact m = 8 1-D first-difference taps (covering m + 1 = 9 adjacent pixels):
		// resolves canonical k/8 ratios (1/4, 3/8, 1/2, 5/8, 3/4, 7/8) and matches 8-px codec block width.
		constexpr int kStaticPatchTaps = 8;
		constexpr int kStaticPatchPixels = kStaticPatchTaps + 1;

		// Bounded caller-owned temporal ring (K = 4..8 static frames):
		// K >= 4 reduces temporal noise variance >= 4x and yields >= 3 lag-1 transitions; K <= 8 caps memory.
		constexpr int kStaticPatchMinFrames = 4;
		constexpr int kStaticPatchMaxFrames = 8;

		// Bounded per-axis spatial patch slot cap (2^9 = 512):
		// 512 * 8 * 8 bytes = 32 KB per frame slot => 8 * 32 KB = 256 KB max history per axis.
		constexpr int kStaticPatchMaxSlots = 512;

		enum class StaticRankReason : int
		{
			Ok = 0,
			Disabled,
			InvalidInput,
			WarmingUp,
			InsufficientStaticPatches,
			MotionRejected,
			LowSignalToNoise,
			NativeFullRank,
			RankCliffDetected
		};

		inline const char* StaticRankReasonText(StaticRankReason r)
		{
			switch (r)
			{
			case StaticRankReason::Ok: return "ok";
			case StaticRankReason::Disabled: return "static patch rank probe disabled";
			case StaticRankReason::InvalidInput: return "invalid input";
			case StaticRankReason::WarmingUp: return "warming up (collecting static frames)";
			case StaticRankReason::InsufficientStaticPatches: return "abstain (insufficient static patch pairs)";
			case StaticRankReason::MotionRejected: return "motion rejected (moving camera or scene cut)";
			case StaticRankReason::LowSignalToNoise: return "abstain (signal below floor or noise dominated)";
			case StaticRankReason::NativeFullRank: return "native full rank (no high-frequency rank cliff)";
			case StaticRankReason::RankCliffDetected: return "low-rank linear resampling cliff detected";
			default: return "unknown";
			}
		}

		struct StaticPatchRankSettings
		{
			bool enabled = false;                       // Strictly OFF by default (W1 bit-exact contract)
			int minFrames = kStaticPatchMinFrames;      // 4 static frames (>= 3 lag-1 transitions, >= 4x variance reduction)
			int maxFrames = kStaticPatchMaxFrames;      // 8 static frames (bounded sliding window cap)
			double motionChiSigma = 3.0;                // 3-sigma chi-square upper bound: 2*sigma^2*(1 + 3*sqrt(2/m))
			double minStaticPatchFraction = 0.25;       // Elementary quarter (1/4): < 25% static => reject moving frame
			int minAcceptedStaticPairs = 32;            // 4 * m = 32 minimum static pairs for stable 8x8 covariance
			double minCliffEigenRatio = 4.0;            // 2^2 = 4.0 (6 dB) eigenvalue cliff lambda_{r-1} / max(lambda_r, floor)
			double maxTrailingEnergyShare = 0.0625;     // 1 / (2*m) = 1/16: trailing (m-r) modes carry < 1/(2m) of energy
			double minSignalToFloorRatio = 1.5;         // ChainEvidence.h::lineGateFactor = 1.5: top RMS >= 1.5 * floorCodes
		};

		struct StaticRankResult
		{
			bool valid = false;
			bool rankCliffDetected = false;
			StaticRankReason reason = StaticRankReason::Disabled;
			int effectiveRank = kStaticPatchTaps;       // 1..8 (< 8 when rankCliffDetected)
			double estimatedRatio = 1.0;                // effectiveRank / 8.0
			double cliffRatio = 1.0;                    // evals[r - 1] / max(posEvals[r], effectiveNoiseFloor)
			double trailingEnergyShare = 1.0;           // sum_{i=r}^{7} posEvals[i] / sum_{i=0}^{7} posEvals[i]
			double spatialFloorCodes = 0.0;             // lower-quartile 2nd-diff MAD sigma (>= floorCodes)
			double eigenvalues[kStaticPatchTaps] = {};  // descending eigenvalues of 8x8 covariance
			int staticFramesUsed = 0;
			int acceptedStaticPairs = 0;
			int rejectedMotionPatches = 0;
			int motionRejectedFrames = 0;
		};

		// ============================================================================================================
		// 1. Exact Deterministic Symmetric Jacobi Eigensolver (m = 8, zero allocations, descending eigenvalues)
		// ============================================================================================================

		inline void SymmetricJacobiEigen8(
			const double A[kStaticPatchTaps][kStaticPatchTaps],
			double evals[kStaticPatchTaps],
			double evecs[kStaticPatchTaps][kStaticPatchTaps])
		{
			double mat[kStaticPatchTaps][kStaticPatchTaps] = {};
			bool allFinite = true;
			double maxDiag = 0.0;
			double maxOff = 0.0;

			for (int i = 0; i < kStaticPatchTaps; ++i)
			{
				for (int j = 0; j < kStaticPatchTaps; ++j)
				{
					const double sym = 0.5 * (A[i][j] + A[j][i]);
					if (!std::isfinite(sym))
					{
						allFinite = false;
					}
					mat[i][j] = sym;
					evecs[i][j] = (i == j) ? 1.0 : 0.0;
					const double mag = std::fabs(sym);
					if (i == j)
					{
						if (mag > maxDiag)
							maxDiag = mag;
					}
					else if (j > i)
					{
						if (mag > maxOff)
							maxOff = mag;
					}
				}
			}

			const double scale = (std::max)(maxDiag, maxOff);
			if (!allFinite || !(scale > 0.0))
			{
				for (int i = 0; i < kStaticPatchTaps; ++i)
				{
					evals[i] = 0.0;
					for (int j = 0; j < kStaticPatchTaps; ++j)
						evecs[i][j] = (i == j) ? 1.0 : 0.0;
				}
				return;
			}

			// Numerical convergence tolerance (1e-12 * scale) and strict rotation skip guard (1e-15 * scale).
			const double sweepTol = 1e-12 * scale;
			const double rotSkipTol = 1e-15 * scale;
			constexpr int kMaxJacobiSweeps = 50;

			for (int sweep = 0; sweep < kMaxJacobiSweeps; ++sweep)
			{
				double sweepMaxOff = 0.0;
				for (int p = 0; p < kStaticPatchTaps; ++p)
				{
					for (int q = p + 1; q < kStaticPatchTaps; ++q)
					{
						const double mag = std::fabs(mat[p][q]);
						if (mag > sweepMaxOff)
							sweepMaxOff = mag;
					}
				}
				if (sweepMaxOff <= sweepTol)
					break;

				for (int p = 0; p < kStaticPatchTaps; ++p)
				{
					for (int q = p + 1; q < kStaticPatchTaps; ++q)
					{
						const double apq = mat[p][q];
						if (std::fabs(apq) <= rotSkipTol)
							continue;

						const double app = mat[p][p];
						const double aqq = mat[q][q];
						const double tau = (aqq - app) / (2.0 * apq);
						double t = 0.0;
						if (std::fabs(tau) > 1e15)
						{
							// Avoid tau*tau overflow when |tau| >> 1; exact to IEEE-754 double precision.
							t = 0.5 / tau;
						}
						else if (tau >= 0.0)
						{
							t = 1.0 / (tau + std::sqrt(1.0 + tau * tau));
						}
						else
						{
							t = -1.0 / (-tau + std::sqrt(1.0 + tau * tau));
						}

						const double c = 1.0 / std::sqrt(1.0 + t * t);
						const double s = t * c;
						const double h = t * apq;

						mat[p][p] = app - h;
						mat[q][q] = aqq + h;
						mat[p][q] = 0.0;
						mat[q][p] = 0.0;

						for (int r = 0; r < kStaticPatchTaps; ++r)
						{
							if (r == p || r == q)
								continue;
							const double mrp = mat[r][p];
							const double mrq = mat[r][q];
							const double newRp = c * mrp - s * mrq;
							const double newRq = s * mrp + c * mrq;
							mat[r][p] = newRp;
							mat[p][r] = newRp;
							mat[r][q] = newRq;
							mat[q][r] = newRq;
						}

						for (int r = 0; r < kStaticPatchTaps; ++r)
						{
							const double vrp = evecs[r][p];
							const double vrq = evecs[r][q];
							evecs[r][p] = c * vrp - s * vrq;
							evecs[r][q] = s * vrp + c * vrq;
						}
					}
				}
			}

			for (int i = 0; i < kStaticPatchTaps; ++i)
				evals[i] = mat[i][i];

			// Deterministic selection sort descending (evals[0] >= evals[1] >= ... >= evals[7]),
			// keeping corresponding eigenvector columns paired.
			for (int i = 0; i < kStaticPatchTaps - 1; ++i)
			{
				int best = i;
				for (int j = i + 1; j < kStaticPatchTaps; ++j)
				{
					if (evals[j] > evals[best])
						best = j;
				}
				if (best != i)
				{
					const double tmpVal = evals[i];
					evals[i] = evals[best];
					evals[best] = tmpVal;
					for (int r = 0; r < kStaticPatchTaps; ++r)
					{
						const double tmpVec = evecs[r][i];
						evecs[r][i] = evecs[r][best];
						evecs[r][best] = tmpVec;
					}
				}
			}
		}

		// ============================================================================================================
		// 2. Caller-Owned Strictly Bounded Workspace (Rule 2: zero static/thread_local mutable state)
		// ============================================================================================================

		struct StaticPatchSlot
		{
			int lineIdx = 0;
			int offset = 0;
		};

		struct StaticPatchTransition
		{
			double rawCrossCov[kStaticPatchTaps][kStaticPatchTaps] = {}; // sum of dx_t * dx_{t-1}^T
			double singleCov[kStaticPatchTaps][kStaticPatchTaps] = {};   // sum of 0.5*(dx_t*dx_t^T + dx_{t-1}*dx_{t-1}^T)
			double spatialFloorCodes = 0.0;
			int acceptedPairs = 0;
			int rejectedPatches = 0;
		};

		struct StaticPatchWorkspace
		{
			// Geometry fingerprint: any change triggers an automatic clean reset.
			int axis = -1;
			int axisLen = 0;
			int length = 0;
			int count = 0;
			int origin = 0;

			// Fixed spatial patch coordinates (<= kStaticPatchMaxSlots) for current geometry.
			std::vector<StaticPatchSlot> slots;

			// Previous anchor frame patches (row-major: slots.size() * kStaticPatchPixels / kStaticPatchTaps).
			bool hasPrevFrame = false;
			std::vector<double> prevRawPatches;
			std::vector<double> prevDiffPatches;

			// Reusable caller-owned scratch buffers (zero steady-state per-frame heap allocations).
			std::vector<double> currRawPatches;
			std::vector<double> currDiffPatches;
			std::vector<double> segSigmaScratch;

			// Strictly bounded sliding window of up to (kStaticPatchMaxFrames - 1) lag-1 transitions.
			StaticPatchTransition transitions[kStaticPatchMaxFrames - 1];
			int transitionHead = 0;
			int transitionCount = 0;
			int activeRingCap = kStaticPatchMaxFrames - 1;

			// Summed (un-normalized) symmetrized lag-1 cross-covariance & single-frame covariance
			// over the active sliding window (re-summed from ring entries for zero cancellation drift).
			double crossCov[kStaticPatchTaps][kStaticPatchTaps] = {};
			double singleCov[kStaticPatchTaps][kStaticPatchTaps] = {};

			// Observable counters (Rule 4).
			int framesObserved = 0;
			int staticFramesAccepted = 0;
			int motionRejectedFrames = 0;
			int acceptedStaticPairs = 0;
			int rejectedMotionPatches = 0;
			int geometryResets = 0;

			void ClearTransitions()
			{
				transitionHead = 0;
				transitionCount = 0;
				acceptedStaticPairs = 0;
				rejectedMotionPatches = 0;
				staticFramesAccepted = hasPrevFrame ? 1 : 0;
				for (int k = 0; k < kStaticPatchMaxFrames - 1; ++k)
					transitions[k] = StaticPatchTransition();
				for (int i = 0; i < kStaticPatchTaps; ++i)
				{
					for (int j = 0; j < kStaticPatchTaps; ++j)
					{
						crossCov[i][j] = 0.0;
						singleCov[i][j] = 0.0;
					}
				}
			}

			void Reset()
			{
				axis = -1;
				axisLen = 0;
				length = 0;
				count = 0;
				origin = 0;
				slots.clear();
				hasPrevFrame = false;
				prevRawPatches.clear();
				prevDiffPatches.clear();
				currRawPatches.clear();
				currDiffPatches.clear();
				segSigmaScratch.clear();
				activeRingCap = kStaticPatchMaxFrames - 1;
				framesObserved = 0;
				motionRejectedFrames = 0;
				geometryResets = 0;
				ClearTransitions();
			}
		};

		// ============================================================================================================
		// 3. Internal Deterministic Helpers
		// ============================================================================================================

		namespace Detail
		{
			// Border margin = 2 * kStaticPatchTaps = 16 px (two 8-px codec blocks at each line end).
			constexpr int kBorderMarginPx = 2 * kStaticPatchTaps;

			inline bool ValidateRankSettings(const StaticPatchRankSettings& cfg)
			{
				if (cfg.minFrames < 2 || cfg.maxFrames < cfg.minFrames || cfg.maxFrames > kStaticPatchMaxFrames)
					return false;
				if (!std::isfinite(cfg.motionChiSigma) || cfg.motionChiSigma < 0.0)
					return false;
				if (!std::isfinite(cfg.minStaticPatchFraction) ||
				    cfg.minStaticPatchFraction <= 0.0 || cfg.minStaticPatchFraction > 1.0)
					return false;
				if (cfg.minAcceptedStaticPairs < kStaticPatchTaps)
					return false;
				if (!std::isfinite(cfg.minCliffEigenRatio) || cfg.minCliffEigenRatio <= 1.0)
					return false;
				if (!std::isfinite(cfg.maxTrailingEnergyShare) ||
				    cfg.maxTrailingEnergyShare <= 0.0 || cfg.maxTrailingEnergyShare >= 1.0)
					return false;
				if (!std::isfinite(cfg.minSignalToFloorRatio) || cfg.minSignalToFloorRatio <= 0.0)
					return false;
				return true;
			}

			inline bool ValidateLineSetAndFloor(const LineSet& ls, double floorCodes)
			{
				if (!std::isfinite(floorCodes) || !(floorCodes > 0.0))
					return false;
				if (ls.count <= 0 || ls.length < 2 * kBorderMarginPx + kStaticPatchPixels)
					return false;
				// Verify backing vector holds all count * length samples before indexing ls.Line(li).
				if (ls.data.size() / static_cast<size_t>(ls.length) < static_cast<size_t>(ls.count))
					return false;
				return true;
			}

			// Builds deterministic, evenly distributed, non-overlapping patch coordinates across all lines
			// of ls, strictly capped at kStaticPatchMaxSlots (512).
			inline void BuildPatchSlots(const LineSet& ls, std::vector<StaticPatchSlot>& outSlots)
			{
				outSlots.clear();
				const int minOffset = kBorderMarginPx;
				const int maxOffset = ls.length - kBorderMarginPx - kStaticPatchPixels;
				if (ls.count <= 0 || maxOffset < minOffset)
					return;

				const int span = maxOffset - minOffset;
				const int maxNonOverlapPerLine = 1 + span / kStaticPatchPixels;

				if (ls.count >= kStaticPatchMaxSlots)
				{
					// Select kStaticPatchMaxSlots evenly spaced lines with 1 centered slot each.
					outSlots.reserve(static_cast<size_t>(kStaticPatchMaxSlots));
					const int centerOffset = minOffset + span / 2;
					for (int k = 0; k < kStaticPatchMaxSlots; ++k)
					{
						const int lineIdx = static_cast<int>(
							(static_cast<int64_t>(k) * static_cast<int64_t>(ls.count - 1)) /
							static_cast<int64_t>(kStaticPatchMaxSlots - 1));
						StaticPatchSlot slot;
						slot.lineIdx = lineIdx;
						slot.offset = centerOffset;
						outSlots.push_back(slot);
					}
					return;
				}

				const int perLineBudget = (std::max)(1, kStaticPatchMaxSlots / ls.count);
				const int nPerLine = (std::min)(perLineBudget, maxNonOverlapPerLine);
				outSlots.reserve(static_cast<size_t>(ls.count) * static_cast<size_t>(nPerLine));

				for (int li = 0; li < ls.count; ++li)
				{
					for (int k = 0; k < nPerLine; ++k)
					{
						int off = minOffset;
						if (nPerLine == 1)
						{
							off = minOffset + span / 2;
						}
						else
						{
							off = minOffset + static_cast<int>(
								(static_cast<int64_t>(k) * static_cast<int64_t>(span)) /
								static_cast<int64_t>(nPerLine - 1));
						}
						StaticPatchSlot slot;
						slot.lineIdx = li;
						slot.offset = off;
						outSlots.push_back(slot);
					}
				}
			}

			// Motion-invariant single-frame spatial noise estimate from lower-quartile (q = 1/4)
			// kSegPx-segment 2nd-difference MAD: sigma = max(floorCodes, kMadToSigma * MAD(d) / sqrt(6)).
			// Returns false if any sampled pixel is non-finite.
			inline bool MeasureSpatialFloorCodes(
				const LineSet& ls,
				double floorCodes,
				std::vector<double>& segSigmaScratch,
				double& outSigma)
			{
				segSigmaScratch.clear();
				const double sqrt6 = std::sqrt(6.0);
				double diffStack[kSegPx];

				for (int li = 0; li < ls.count; ++li)
				{
					const double* y = ls.Line(li);
					for (int start = 0; start < ls.length; start += kSegPx)
					{
						int end = start + kSegPx;
						if (end > ls.length)
							end = ls.length;
						if (end - start < 5)
						{
							for (int i = start; i < end; ++i)
							{
								if (!std::isfinite(y[i]))
									return false;
							}
							continue;
						}

						if (!std::isfinite(y[start]) || !std::isfinite(y[end - 1]))
							return false;

						int cnt = 0;
						for (int i = start + 1; i + 1 < end; ++i)
						{
							const double yi = y[i];
							if (!std::isfinite(yi))
								return false;
							diffStack[cnt++] = y[i - 1] - 2.0 * yi + y[i + 1];
						}

						const double mad = RobustStats::MadInPlace(
							diffStack, cnt, RobustStats::MedianRule::MeanOfMiddle, 0.0);
						if (mad > 0.0)
						{
							segSigmaScratch.push_back((RobustStats::kMadToSigma * mad) / sqrt6);
						}
					}
				}

				if (segSigmaScratch.empty())
				{
					outSigma = floorCodes;
					return true;
				}

				// Lower quartile (1/4) isolates quiet/grain-dominated segments from sharp textured edges.
				const double quietSigma = RobustStats::QuantileInPlace(
					segSigmaScratch.data(),
					static_cast<int>(segSigmaScratch.size()),
					0.25,
					RobustStats::RankRule::Interpolated,
					floorCodes);
				outSigma = (std::max)(floorCodes, quietSigma);
				return true;
			}

			// Extracts raw 9-pixel patches and pre-whitened 8-tap first-difference patches dx[i] = x[i+1] - x[i].
			// Returns false if any patch sample is non-finite.
			inline bool ExtractPatches(
				const LineSet& ls,
				const std::vector<StaticPatchSlot>& slots,
				std::vector<double>& outRaw,
				std::vector<double>& outDiff)
			{
				const size_t numSlots = slots.size();
				outRaw.resize(numSlots * static_cast<size_t>(kStaticPatchPixels));
				outDiff.resize(numSlots * static_cast<size_t>(kStaticPatchTaps));

				for (size_t s = 0; s < numSlots; ++s)
				{
					const double* y = ls.Line(slots[s].lineIdx) + slots[s].offset;
					double* rawDst = outRaw.data() + s * static_cast<size_t>(kStaticPatchPixels);
					double* diffDst = outDiff.data() + s * static_cast<size_t>(kStaticPatchTaps);

					for (int i = 0; i < kStaticPatchPixels; ++i)
					{
						const double v = y[i];
						if (!std::isfinite(v))
							return false;
						rawDst[i] = v;
					}
					for (int i = 0; i < kStaticPatchTaps; ++i)
					{
						diffDst[i] = rawDst[i + 1] - rawDst[i];
					}
				}
				return true;
			}

			// Evaluates the 8x8 symmetric covariance spectrum for a high-frequency bandlimit rank cliff.
			inline void EvaluateRankSpectrum(
				const double normCov[kStaticPatchTaps][kStaticPatchTaps],
				double floorCodes,
				double noiseEigFloor,
				const StaticPatchRankSettings& cfg,
				StaticRankResult& res)
			{
				double evals[kStaticPatchTaps] = {};
				double evecs[kStaticPatchTaps][kStaticPatchTaps] = {};
				SymmetricJacobiEigen8(normCov, evals, evecs);

				double posEvals[kStaticPatchTaps] = {};
				double totalPosEnergy = 0.0;
				double totalNegEnergy = 0.0;
				for (int i = 0; i < kStaticPatchTaps; ++i)
				{
					res.eigenvalues[i] = evals[i];
					if (evals[i] > 0.0)
					{
						posEvals[i] = evals[i];
						totalPosEnergy += evals[i];
					}
					else
					{
						posEvals[i] = 0.0;
						totalNegEnergy += -evals[i];
					}
				}

				// Wigner spectral symmetry of zero-mean estimation noise gives an empirical noise floor
				// from the magnitude of the most negative eigenvalue of the PSD cross-covariance.
				const double empiricalNegFloor =
					(evals[kStaticPatchTaps - 1] < 0.0) ? -evals[kStaticPatchTaps - 1] : 0.0;
				const double effectiveNoiseFloor = (std::max)(noiseEigFloor, empiricalNegFloor);

				// Top signal eigenvalue RMS must clear minSignalToFloorRatio * floorCodes, and positive
				// PSD energy must dominate negative noise energy.
				const double minTopSignalEig =
					(cfg.minSignalToFloorRatio * floorCodes) * (cfg.minSignalToFloorRatio * floorCodes);
				if (!(evals[0] >= minTopSignalEig) ||
				    !(totalPosEnergy > 0.0) ||
				    (empiricalNegFloor > 0.0 &&
				     (evals[0] <= cfg.minSignalToFloorRatio * empiricalNegFloor ||
				      totalPosEnergy <= cfg.minSignalToFloorRatio * totalNegEnergy)))
				{
					res.valid = false;
					res.rankCliffDetected = false;
					res.effectiveRank = kStaticPatchTaps;
					res.estimatedRatio = 1.0;
					res.cliffRatio = 1.0;
					res.trailingEnergyShare = 1.0;
					res.reason = StaticRankReason::LowSignalToNoise;
					return;
				}

				// 1-D discrete Neumann Laplacian Rayleigh quotient rho(v_k) = sum_{i=0}^{m-2} (v_k[i+1] - v_k[i])^2
				// in [0, 4]. Half-Nyquist midpoint f = 1/4 corresponds to exact rho = 4 * sin^2(pi/4) = 2.0.
				// Average trailing roughness is rotationally invariant across degenerate nullspaces.
				double eigRoughness[kStaticPatchTaps] = {};
				for (int k = 0; k < kStaticPatchTaps; ++k)
				{
					double rSum = 0.0;
					for (int i = 0; i + 1 < kStaticPatchTaps; ++i)
					{
						const double d = evecs[i + 1][k] - evecs[i][k];
						rSum += d * d;
					}
					eigRoughness[k] = rSum;
				}

				bool foundCliff = false;
				int bestRank = kStaticPatchTaps;
				double bestCliffRatio = 1.0;
				double bestTrailingShare = 1.0;

				double maxDiagRatio = 1.0;
				double minDiagTrailing = 1.0;

				// Search smallest qualifying rank r in [1, m - 1] so soft transition-band leakage
				// modes (e.g. bicubic 1% tail) do not shift rank upward past the primary cliff.
				for (int r = 1; r < kStaticPatchTaps; ++r)
				{
					double leadRoughSum = 0.0;
					for (int k = 0; k < r; ++k)
						leadRoughSum += eigRoughness[k];
					const double meanRhoLeading = leadRoughSum / static_cast<double>(r);

					double trailRoughSum = 0.0;
					double trailingEnergy = 0.0;
					for (int k = r; k < kStaticPatchTaps; ++k)
					{
						trailRoughSum += eigRoughness[k];
						trailingEnergy += posEvals[k];
					}
					const double meanRhoTrailing =
						trailRoughSum / static_cast<double>(kStaticPatchTaps - r);

					// Half of the r-th m-point DCT-II Neumann Laplacian eigenvalue: 2 * sin^2(r*pi/(2m)).
					const double sinTerm = std::sin(
						(static_cast<double>(r) * kPi) / (2.0 * static_cast<double>(kStaticPatchTaps)));
					const double minResolvedModeRoughness = 2.0 * sinTerm * sinTerm;
					const bool cliffModeOk =
						!(posEvals[r] > 2.0 * effectiveNoiseFloor) ||
						(eigRoughness[r] >= minResolvedModeRoughness);

					// Upscaler nullspaces are strictly high-frequency (meanRhoTrailing >= 2.0 and > leading),
					// rejecting first-difference f = 0 suppression on native white-noise textures.
					const bool trailingIsHighFreq =
						(meanRhoTrailing >= 2.0) && (meanRhoTrailing > meanRhoLeading) && cliffModeOk;

					const double denom = (std::max)(posEvals[r], effectiveNoiseFloor);
					const double ratio = (denom > 0.0) ? (posEvals[r - 1] / denom) : 0.0;
					const double trailingShare = trailingEnergy / totalPosEnergy;

					if (trailingIsHighFreq)
					{
						if (ratio > maxDiagRatio)
							maxDiagRatio = ratio;
						if (trailingShare < minDiagTrailing)
							minDiagTrailing = trailingShare;
					}

					if (trailingIsHighFreq &&
					    ratio >= cfg.minCliffEigenRatio &&
					    trailingShare <= cfg.maxTrailingEnergyShare)
					{
						foundCliff = true;
						bestRank = r;
						bestCliffRatio = ratio;
						bestTrailingShare = trailingShare;
						break;
					}
				}

				res.valid = true;
				if (foundCliff)
				{
					res.rankCliffDetected = true;
					res.effectiveRank = bestRank;
					res.estimatedRatio =
						static_cast<double>(bestRank) / static_cast<double>(kStaticPatchTaps);
					res.cliffRatio = bestCliffRatio;
					res.trailingEnergyShare = bestTrailingShare;
					res.reason = StaticRankReason::RankCliffDetected;
				}
				else
				{
					res.rankCliffDetected = false;
					res.effectiveRank = kStaticPatchTaps;
					res.estimatedRatio = 1.0;
					res.cliffRatio = maxDiagRatio;
					res.trailingEnergyShare = minDiagTrailing;
					res.reason = StaticRankReason::NativeFullRank;
				}
			}
		}

		// ============================================================================================================
		// 4. Multi-Frame Static-Patch Rank Probe (Primary Entry Point)
		// ============================================================================================================

		inline StaticRankResult SubmitFrame(
			const LineSet& ls,
			double floorCodes,
			const StaticPatchRankSettings& cfg,
			StaticPatchWorkspace& ws)
		{
			StaticRankResult res;
			if (!cfg.enabled)
			{
				res.reason = StaticRankReason::Disabled;
				return res;
			}
			if (!Detail::ValidateRankSettings(cfg) || !Detail::ValidateLineSetAndFloor(ls, floorCodes))
			{
				res.reason = StaticRankReason::InvalidInput;
				return res;
			}

			// Estimate spatial noise sigma and verify line finiteness before mutating workspace state.
			double sigmaSpatial = floorCodes;
			if (!Detail::MeasureSpatialFloorCodes(ls, floorCodes, ws.segSigmaScratch, sigmaSpatial))
			{
				ws.segSigmaScratch.clear();
				res.reason = StaticRankReason::InvalidInput;
				return res;
			}

			// Detect geometry change against workspace fingerprint.
			const bool geometryMatches =
				(ws.framesObserved > 0) &&
				(ws.axis == ls.axis) &&
				(ws.axisLen == ls.axisLen) &&
				(ws.length == ls.length) &&
				(ws.count == ls.count) &&
				(ws.origin == ls.origin);

			if (ws.framesObserved > 0 && !geometryMatches)
			{
				const int prevResets = ws.geometryResets;
				ws.Reset();
				ws.geometryResets = prevResets + 1;
			}

			const int desiredRingCap = cfg.maxFrames - 1;
			if (ws.slots.empty())
			{
				ws.axis = ls.axis;
				ws.axisLen = ls.axisLen;
				ws.length = ls.length;
				ws.count = ls.count;
				ws.origin = ls.origin;
				ws.activeRingCap = desiredRingCap;
				Detail::BuildPatchSlots(ls, ws.slots);
				if (ws.slots.empty())
				{
					res.reason = StaticRankReason::InvalidInput;
					return res;
				}
			}
			else if (ws.activeRingCap != desiredRingCap)
			{
				ws.ClearTransitions();
				ws.activeRingCap = desiredRingCap;
			}

			// Extract current frame's patches into caller-owned scratch buffers.
			if (!Detail::ExtractPatches(ls, ws.slots, ws.currRawPatches, ws.currDiffPatches))
			{
				res.reason = StaticRankReason::InvalidInput;
				return res;
			}

			++ws.framesObserved;
			const int numSlots = static_cast<int>(ws.slots.size());

			// First anchor frame: store patches and warm up.
			if (!ws.hasPrevFrame)
			{
				ws.prevRawPatches.swap(ws.currRawPatches);
				ws.prevDiffPatches.swap(ws.currDiffPatches);
				ws.hasPrevFrame = true;
				ws.staticFramesAccepted = 1;

				res.valid = false;
				res.rankCliffDetected = false;
				res.reason = StaticRankReason::WarmingUp;
				res.spatialFloorCodes = sigmaSpatial;
				res.staticFramesUsed = 1;
				res.acceptedStaticPairs = 0;
				res.rejectedMotionPatches = 0;
				res.motionRejectedFrames = ws.motionRejectedFrames;
				return res;
			}

			// Stage 1: Per-patch chi-square static motion gate on raw 8-tap temporal difference x_t - x_{t-1}.
			// Var(x_t - x_{t-1}) = 2 * sigma_spatial^2; upper bound = 2 * sigma^2 * (1 + chiSigma * sqrt(2/m)).
			const double expectedDiffVar = 2.0 * sigmaSpatial * sigmaSpatial;
			const double chiUpper = expectedDiffVar *
				(1.0 + cfg.motionChiSigma * std::sqrt(2.0 / static_cast<double>(kStaticPatchTaps)));

			StaticPatchTransition trans;
			double sumAsym = 0.0;
			double sumAsymSq = 0.0;

			for (int s = 0; s < numSlots; ++s)
			{
				const double* curRaw = ws.currRawPatches.data() + static_cast<size_t>(s) * static_cast<size_t>(kStaticPatchPixels);
				const double* prvRaw = ws.prevRawPatches.data() + static_cast<size_t>(s) * static_cast<size_t>(kStaticPatchPixels);

				double ms = 0.0;
				for (int i = 0; i < kStaticPatchTaps; ++i)
				{
					const double d = curRaw[i] - prvRaw[i];
					ms += d * d;
				}
				ms /= static_cast<double>(kStaticPatchTaps);

				if (ms > chiUpper)
				{
					++trans.rejectedPatches;
					continue;
				}

				++trans.acceptedPairs;
				const double* curDx = ws.currDiffPatches.data() + static_cast<size_t>(s) * static_cast<size_t>(kStaticPatchTaps);
				const double* prvDx = ws.prevDiffPatches.data() + static_cast<size_t>(s) * static_cast<size_t>(kStaticPatchTaps);

				for (int i = 0; i < kStaticPatchTaps; ++i)
				{
					const double ci = curDx[i];
					const double pi = prvDx[i];
					for (int j = 0; j < kStaticPatchTaps; ++j)
					{
						trans.rawCrossCov[i][j] += ci * prvDx[j];
						trans.singleCov[i][j] += 0.5 * (ci * curDx[j] + pi * prvDx[j]);
					}
				}

				// Lag-1 antisymmetric projection (exact zero on static patches; peaks on sub-pixel pans).
				double patchAsym = 0.0;
				for (int i = 0; i + 1 < kStaticPatchTaps; ++i)
					patchAsym += curDx[i] * prvDx[i + 1] - curDx[i + 1] * prvDx[i];
				patchAsym /= static_cast<double>(kStaticPatchTaps - 1);
				sumAsym += patchAsym;
				sumAsymSq += patchAsym * patchAsym;
			}

			const double staticFraction =
				static_cast<double>(trans.acceptedPairs) / static_cast<double>(numSlots);
			bool motionRejected =
				(trans.acceptedPairs <= 0) || (staticFraction < cfg.minStaticPatchFraction);

			// Stage 2: Directional shift & sub-pixel asymmetry gate on accepted transition cross-covariance.
			// Catches slow 0.5..3 px camera pans even when dense spatial texture inflates sigmaSpatial.
			if (!motionRejected && trans.acceptedPairs >= kStaticPatchTaps)
			{
				const double invPairs = 1.0 / static_cast<double>(trans.acceptedPairs);
				const double shiftNoiseSe =
					(2.0 * sigmaSpatial * sigmaSpatial) / std::sqrt(static_cast<double>(trans.acceptedPairs));

				constexpr int kMaxCheckLag = 3;
				for (int lag = 1; lag <= kMaxCheckLag; ++lag)
				{
					const int entries = kStaticPatchTaps - lag;
					double r0Sum = 0.0;
					double rpSum = 0.0;
					double rmSum = 0.0;
					for (int i = 0; i < entries; ++i)
					{
						r0Sum += 0.5 * (trans.rawCrossCov[i][i] + trans.rawCrossCov[i + lag][i + lag]);
						rpSum += trans.rawCrossCov[i][i + lag];
						rmSum += trans.rawCrossCov[i + lag][i];
					}
					const double invScale = invPairs / static_cast<double>(entries);
					const double r0 = r0Sum * invScale;
					const double rp = rpSum * invScale;
					const double rm = rmSum * invScale;
					const double maxShift = (std::max)(rp, rm);
					if (maxShift > r0 + cfg.motionChiSigma * shiftNoiseSe)
					{
						motionRejected = true;
						break;
					}
				}

				if (!motionRejected)
				{
					double traceCross = 0.0;
					for (int i = 0; i < kStaticPatchTaps; ++i)
						traceCross += trans.rawCrossCov[i][i];
					const double r0All = traceCross * (invPairs / static_cast<double>(kStaticPatchTaps));
					const double asymMean = sumAsym * invPairs;
					const double asymVar = (std::max)(0.0, (sumAsymSq * invPairs) - asymMean * asymMean);
					const double asymSe = (std::max)(shiftNoiseSe, std::sqrt(asymVar * invPairs));

					// Elementary quarter (1/4) asymmetry share + 3-sigma SE threshold catches sub-pixel pans.
					if (r0All > 0.0 &&
					    std::fabs(asymMean) > 0.25 * r0All &&
					    std::fabs(asymMean) > cfg.motionChiSigma * asymSe)
					{
						motionRejected = true;
					}
				}
			}

			// Always advance anchor to the newest frame so recovery begins immediately when motion stops.
			ws.prevRawPatches.swap(ws.currRawPatches);
			ws.prevDiffPatches.swap(ws.currDiffPatches);
			ws.hasPrevFrame = true;

			if (motionRejected)
			{
				ws.ClearTransitions();
				++ws.motionRejectedFrames;

				res.valid = false;
				res.rankCliffDetected = false;
				res.reason = StaticRankReason::MotionRejected;
				res.spatialFloorCodes = sigmaSpatial;
				res.staticFramesUsed = 1;
				res.acceptedStaticPairs = 0;
				res.rejectedMotionPatches = trans.rejectedPatches;
				res.motionRejectedFrames = ws.motionRejectedFrames;
				return res;
			}

			// Push accepted static transition into the bounded sliding window ring.
			trans.spatialFloorCodes = sigmaSpatial;
			ws.transitions[ws.transitionHead] = trans;
			ws.transitionHead = (ws.transitionHead + 1) % ws.activeRingCap;
			if (ws.transitionCount < ws.activeRingCap)
				++ws.transitionCount;

			// Re-sum active ring entries in exact chronological (oldest -> newest) order
			// (<= 7 matrices of 8x8 -> zero cancellation drift and bit-exact sliding-window shift invariance).
			double sumRawCross[kStaticPatchTaps][kStaticPatchTaps] = {};
			double sumSingle[kStaticPatchTaps][kStaticPatchTaps] = {};
			double sumNoiseVar = 0.0;
			int totalPairs = 0;
			int totalRejected = 0;
			const int oldestIdx = (ws.transitionCount == ws.activeRingCap) ? ws.transitionHead : 0;
			for (int k = 0; k < ws.transitionCount; ++k)
			{
				const int slotIdx = (oldestIdx + k) % ws.activeRingCap;
				const StaticPatchTransition& entry = ws.transitions[slotIdx];
				totalPairs += entry.acceptedPairs;
				totalRejected += entry.rejectedPatches;
				sumNoiseVar += entry.spatialFloorCodes * entry.spatialFloorCodes;
				for (int i = 0; i < kStaticPatchTaps; ++i)
				{
					for (int j = 0; j < kStaticPatchTaps; ++j)
					{
						sumRawCross[i][j] += entry.rawCrossCov[i][j];
						sumSingle[i][j] += entry.singleCov[i][j];
					}
				}
			}

			for (int i = 0; i < kStaticPatchTaps; ++i)
			{
				for (int j = 0; j < kStaticPatchTaps; ++j)
				{
					ws.crossCov[i][j] = 0.5 * (sumRawCross[i][j] + sumRawCross[j][i]);
					ws.singleCov[i][j] = 0.5 * (sumSingle[i][j] + sumSingle[j][i]);
				}
			}
			ws.acceptedStaticPairs = totalPairs;
			ws.rejectedMotionPatches = totalRejected;
			ws.staticFramesAccepted = ws.transitionCount + 1;

			res.spatialFloorCodes = sigmaSpatial;
			res.staticFramesUsed = ws.staticFramesAccepted;
			res.acceptedStaticPairs = ws.acceptedStaticPairs;
			res.rejectedMotionPatches = ws.rejectedMotionPatches;
			res.motionRejectedFrames = ws.motionRejectedFrames;

			if (ws.staticFramesAccepted < cfg.minFrames)
			{
				res.valid = false;
				res.rankCliffDetected = false;
				res.reason = StaticRankReason::WarmingUp;
				return res;
			}

			if (ws.acceptedStaticPairs < cfg.minAcceptedStaticPairs)
			{
				res.valid = false;
				res.rankCliffDetected = false;
				res.reason = StaticRankReason::InsufficientStaticPatches;
				return res;
			}

			double normCov[kStaticPatchTaps][kStaticPatchTaps] = {};
			const double invTotalPairs = 1.0 / static_cast<double>(ws.acceptedStaticPairs);
			for (int i = 0; i < kStaticPatchTaps; ++i)
			{
				for (int j = 0; j < kStaticPatchTaps; ++j)
					normCov[i][j] = ws.crossCov[i][j] * invTotalPairs;
			}

			// Exact standard error of sample cross-covariance of dx (noise variance = 2 * mean_sigma_spatial^2).
			const double meanNoiseVar = sumNoiseVar / static_cast<double>(ws.transitionCount);
			const double noiseEigFloor =
				(2.0 * meanNoiseVar) / std::sqrt(static_cast<double>(ws.acceptedStaticPairs));
			Detail::EvaluateRankSpectrum(normCov, floorCodes, noiseEigFloor, cfg, res);
			return res;
		}

		// ============================================================================================================
		// 5. Single-Frame Covariance Rank Probe (Diagnostic / Negative-Control Companion)
		// ============================================================================================================

		// Computes the exact same 8x8 covariance rank probe on a single frame C_0 = E[dx_t dx_t^T]
		// without cross-frame noise cancellation, allowing direct single-frame vs multi-frame comparison.
		inline StaticRankResult SingleFrameRankProbe(
			const LineSet& ls,
			double floorCodes,
			const StaticPatchRankSettings& cfg = StaticPatchRankSettings())
		{
			StaticRankResult res;
			if (!cfg.enabled)
			{
				res.reason = StaticRankReason::Disabled;
				return res;
			}
			if (!Detail::ValidateRankSettings(cfg) || !Detail::ValidateLineSetAndFloor(ls, floorCodes))
			{
				res.reason = StaticRankReason::InvalidInput;
				return res;
			}

			std::vector<double> segScratch;
			double sigmaSpatial = floorCodes;
			if (!Detail::MeasureSpatialFloorCodes(ls, floorCodes, segScratch, sigmaSpatial))
			{
				res.reason = StaticRankReason::InvalidInput;
				return res;
			}

			std::vector<StaticPatchSlot> slots;
			Detail::BuildPatchSlots(ls, slots);
			if (slots.empty())
			{
				res.reason = StaticRankReason::InvalidInput;
				return res;
			}

			std::vector<double> rawPatches;
			std::vector<double> diffPatches;
			if (!Detail::ExtractPatches(ls, slots, rawPatches, diffPatches))
			{
				res.reason = StaticRankReason::InvalidInput;
				return res;
			}

			const int numSlots = static_cast<int>(slots.size());
			res.spatialFloorCodes = sigmaSpatial;
			res.staticFramesUsed = 1;
			res.acceptedStaticPairs = numSlots;
			res.rejectedMotionPatches = 0;
			res.motionRejectedFrames = 0;

			if (numSlots < kStaticPatchTaps)
			{
				res.valid = false;
				res.rankCliffDetected = false;
				res.reason = StaticRankReason::InsufficientStaticPatches;
				return res;
			}

			double cov[kStaticPatchTaps][kStaticPatchTaps] = {};
			const double invSlots = 1.0 / static_cast<double>(numSlots);
			for (int s = 0; s < numSlots; ++s)
			{
				const double* dx = diffPatches.data() + static_cast<size_t>(s) * static_cast<size_t>(kStaticPatchTaps);
				for (int i = 0; i < kStaticPatchTaps; ++i)
				{
					const double dxi = dx[i];
					for (int j = 0; j < kStaticPatchTaps; ++j)
						cov[i][j] += dxi * dx[j];
				}
			}
			for (int i = 0; i < kStaticPatchTaps; ++i)
			{
				for (int j = 0; j < kStaticPatchTaps; ++j)
					cov[i][j] *= invSlots;
			}

			const double noiseEigFloor =
				(2.0 * floorCodes * floorCodes) / std::sqrt(static_cast<double>(numSlots));
			Detail::EvaluateRankSpectrum(cov, floorCodes, noiseEigFloor, cfg, res);
			return res;
		}
	}
}

#pragma fp_contract(on)
