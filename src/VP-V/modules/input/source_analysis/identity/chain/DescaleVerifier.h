// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// DescaleVerifier (SID-C1 unit 3b): "analysis by synthesis" for candidate original sizes.
//
// For a candidate latent size n of an N-pixel axis and a kernel k, every sampled line is shrunk back to n
// samples by least squares and blown up again with the same kernel (DescaleOperator, KernelBank.h). If the
// picture really was upscaled from n by k, almost nothing is lost (only rounding noise); a native picture
// always loses its finest detail.
//
// Statistic. For line j: s_j(n, k) = rms residual over the inner rows / sqrt(1 - n/N) (the residual per
// remaining degree of freedom; it removes the "a finer latent grid absorbs noise" trend, so natives sit at 0).
// baseline_j = median over reference sizes n_ref = n (1 +- 4%), n (1 +- 8%) of the smallest s_j over the four
// representative kernels (bilinear, bicubic 0/0.5, lanczos3, spline36). Per-line value
//   v_j = ln(s_j(n, k) / baseline_j)   (negative = a dip at n)
// and the candidate statistic T = trimmed mean of v_j over lines (20% off each end). A kernel is judged by its T
// against the SAME baseline, so the best kernel is the one with the smallest residual.
//
// Search (per candidate, both stages deterministic):
//   1. coarse scan, short windows (about N/8 rows at each end of the lines, the notch of a short window is a
//      few integers wide and alias free at the line ends), every integer within +-4 of the candidate, the four
//      representative kernels, 16 lines. It refines the candidate to n^ and ranks candidates.
//   2. exact stage on the best `maxExactCandidates` candidates (plus the caller's latched size, `forceExactN`): the
//      full lines, integers n^ +-1 (the coarse stage already centres it) with the representative kernels, then at the best integer every kernel of the best two families (optionally at
//      several sub-sample phases). Outputs the best kernel, its margin to the runner-up kernel and to the best
//      kernel of another family, the residual floor in 10-bit codes, per-line values, and per-256-px segment sums.
//
// Operators are cached by (N, n, kernel, phase step, window); the cache only saves time, never changes a result. It is bounded by
// a count (cacheLimit) and by bytes (cacheBytes), least recently used first. Within one Evaluate call the solves of every
// (window, n, kernel, phase) are kept as well, so candidates that share an integer (overlapping coarse scans, equal reference
// sizes) read the numbers instead of solving again; the lines of a stage are solved together (DescaleOperator::ResidualBatch),
// with the same arithmetic in the same order as a line on its own, so none of this changes a single bit of a result (SID-OPT
// step 1; ChainVerifierTests pins the equality with the single-line reference path, VerifierSettings::batchedSolve = false).
// Degrees of freedom: s uses 1 - nodes/rows of the operator (the unknowns of the fit), not 1 - n/N, so short
// windows are not biased. Give the PowerProbe its own verifier instance (its 1.64x longer operators would
// otherwise push the candidates' operators out of the cache).
// Not thread safe (one instance per worker). No clock inside: the caller times it.
#include <cstdlib>
#include <iterator>
#include <list>
#include <map>
#include <memory>

#include "EwaJincProbe.h"
#include "HdrNoiseFloor.h"
#include "KernelBank.h"

// No contraction (fused multiply-add) in the descale arithmetic: the batched and the single-line solve must round identically under any /arch.
#pragma fp_contract(off)

namespace SourceIdentity
{
	namespace Chain
	{
		// The search-narrowing constants (SID-OPT step 2; VP-owned, ONE set, documented in README.md "Cost"). They bound how much of the
		// exact (full line) stage runs per axis and evaluation. kExactHalfWidth and kMaxExactCandidates are the values the estimator has
		// always run with (measured before SID-OPT: one candidate = 1888 solves per axis = 832 coarse + 1056 exact, which is exactly
		// 3 integers x 4 kernels in the scan and 2 promoted candidates); they are named here so the narrowing is one reviewed set rather
		// than scattered defaults. The two entries marked "lever" are new: a non-default value changes results.
		namespace Narrowing
		{
			constexpr int kExactHalfWidth = 1;               // exact stage scans the coarse best n +- this (the coarse stage already centres it)
			constexpr int kMaxExactCandidates = 2;           // coarse-ranked candidates promoted to full lines (plus the caller's confident n)
			constexpr int kConfidentMaxExactCandidates = 0;  // lever: when the caller forces its confident n, promote at most this many coarse
			                                                 // ranked candidates besides it (0 = kMaxExactCandidates, the shipped behaviour)
			constexpr int kConfidentAxisExactLines = 0;      // lever (step 3, OFF until measured): exact-stage lines of one confident axis while the
			                                                 // other axis still acquires (0 = off, all sampled lines; 16 = on); applied by the
			                                                 // estimator (EstimatorSettings::confidentAxisExactLines, ExactLinesFor), not by the verifier
			constexpr int kCoarseWindows = 2;                // lever: coarse windows per line (2 = both ends, shipped; 1 = the left end only)
			constexpr bool kCoarseRefine = false;            // lever: with coarseStep > 1, scan the integers the step skipped around the best (false = shipped)
		}

		struct VerifierSettings
		{
			int coarseHalfWidth = 4;            // integers scanned each side of a candidate by the coarse stage
			int coarseStep = 1;                 // step of that scan (the notch of a short window is a few integers wide)
			bool coarseRefine = Narrowing::kCoarseRefine;   // coarseStep > 1: also scan the skipped integers within step - 1 of the best (lever; see Narrowing)
			int coarseWindows = Narrowing::kCoarseWindows;  // 2 = a short window at each end of the lines; 1 = the left one only (lever; see Narrowing)
			int coarseLines = 8;                // lines used by the coarse stage (F0: the size is still right down to 8 lines)
			double coarseWindowFraction = 0.125;   // coarse window = this fraction of N ...
			int coarseMinWindow = 160;          // ... but at least this many rows
			int exactHalfWidth = Narrowing::kExactHalfWidth;             // integers scanned each side by the exact stage
			int maxExactCandidates = Narrowing::kMaxExactCandidates;     // candidates promoted to the exact stage
			int confidentMaxExactCandidates = Narrowing::kConfidentMaxExactCandidates;   // see Narrowing; 0 = maxExactCandidates
			int exactFamilies = 2;              // kernel families evaluated in full at the best integer
			double refFraction1 = 0.04;         // baseline sizes n (1 +- f1), n (1 +- f2)
			double refFraction2 = 0.08;
			double trimFraction = 0.2;
			double epsCodes = 0.02;             // numerical floor added (in quadrature) to a line's rms
			int minLatent = 16;
			double maxLatentRatio = 0.985;      // n / N above this is not an upscale
			int phaseCount = 1;                 // odd; sub-sample phases tried at the best integer
			double phaseStep = 0.25;            // latent samples between phases
			// Optional wall-clock hook for the cost the result reports (the units themselves never read a clock): returns
			// milliseconds from any monotonic source; the caller (SID-C2) passes steady_clock. nullptr = costMs stays 0.
			double (*nowMs)(void*) = nullptr;
			void* clockCtx = nullptr;
			size_t cacheLimit = 4096;           // operators kept at most (time only; the byte budget below binds first at 4K)
			size_t cacheBytes = static_cast<size_t>(160) * 1024 * 1024;   // heap bytes of ALL cached state at most (operators + tap memo + the call's kept solves;
			                                                              // 0 = no byte limit); 30 KB (coarse) to 400 KB (full line) per operator at 3840x2160,
			                                                              // tap memo <= 1/16 of it, kept solves about 2 MB. The probe verifier gets 1/8 of it.
			bool batchedSolve = true;           // false: solve the lines one at a time with the reference routine (same bits; for tests and bisecting)
			HdrNoiseSettings hdrNoise;          // SOTA #1 (default OFF): heteroscedastic PQ/HLG linearized WLS verification
			bool gaussNewtonPhaseRefine = false;// SOTA #5 (default OFF): 1-step analytical Golub-Pereyra Gauss-Newton sub-pixel phase refinement
		};

		struct VerifyOptions
		{
			bool exactOnly = false;     // skip the coarse stage (n is known exactly, e.g. the power probe)
			int onlyKernel = -1;        // >= 0: evaluate only this kernel
			int exactHalfWidth = -1;    // >= 0 overrides the settings value
			int forceExactN = 0;        // > 0: the candidate asked for with exactly this size always goes to the exact stage
			                            // (the caller's latched size), on top of the best maxExactCandidates
			int exactLines = 0;         // > 0: the exact stage uses only this many (evenly spread) lines, e.g. 16 when latched
			// SID-OPT (default OFF): two-pass P3 exact screening (Pass 1 screens all candidates at halfWidth = 0 with rep kernels;
			// Pass 2 expands full halfWidth + full kernel families on top p3ScreenKeepTop, p3ProtectedN, forceExactN, and any dip < p3ScreenDipThreshold).
			bool p3TwoPass = false;
			int p3ProtectedN[2] = { 0, 0 };
			int p3ScreenKeepTop = Narrowing::kMaxExactCandidates;
			// Derived: (2.0 / 3.0) * EvidenceSettings::majorityDip (-0.03) = -0.02, strictly milder than majorityDip so any dipping candidate is expanded in Pass 2.
			double p3ScreenDipThreshold = -0.02;
		};

		struct CandidateResult
		{
			int nProposed = 0;              // the size asked for
			int n = 0;                      // refined size (exact stage) or coarse-refined size
			bool valid = false;             // false: could not be evaluated (see reason)
			bool exactDone = false;         // verified on full lines
			int kernel = -1;                // best kernel index (KernelBank.h)
			int family = -1;
			double phase = 0.0;
			double stat = 0.0;              // T (trimmed mean of the per-line values); exact stage, else the coarse value
			double coarseStat = 0.0;
			double kernelMargin = 0.0;      // T(runner-up kernel) - T(best), >= 0
			double familyMargin = 0.0;      // T(best kernel of another family) - T(best), >= 0
			// Per-kernel T at the refined size (exact stage): bit k of kernelMask says kernel k was evaluated, kernelT[k] is its T
			// (smaller = better; the best of the evaluated ones equals `stat`). Only the kernels of the best exactFamilies families
			// are evaluated. Deterministic. The latch builds its kernel equivalence class from these.
			uint32_t kernelMask = 0;
			double kernelT[kKernelCount] = {};
			double floorCodes = 0.0;        // trimmed mean over lines of the RAW rms residual at the best (n, kernel), 10-bit codes.
			                            // NOT dof-normalised: the rounding floor the evidence gate wants (AxisObservation.floorCodes,
			                            // 1.155 for an 8-bit output, 0.289 for 10-bit) is a different, normalised quantity
			double baselineMedian = 0.0;    // median line baseline (codes): the line's detail level above the floor
			uint32_t reason = 0;
			int lineCount = 0;
			int segCount = 0;
			std::vector<double> lineStat;   // v_j
			std::vector<double> lineBase;   // baseline_j (codes)
			std::vector<double> lineRms;    // raw rms residual (codes)
			std::vector<double> segSum;     // lines x segCount: residual sum of squares of each 256-px segment
			std::vector<int> segRows;       // lines x segCount: inner rows of each segment
			std::vector<double> segStat;    // lines x segCount: 0.5 ln(segment level / its baseline); 0 where segRows == 0
			std::vector<double> segBase;    // lines x segCount: baseline rms of the segment (codes)
			// SOTA #1 (default OFF): heteroscedastic HDR linearized WLS diagnostics (all zero/false while hdrNoise.enabled == false).
			bool hdrLinearizedUsed = false;
			double hdrWhitenedStat = 0.0;
			double hdrWhitenedRms = 0.0;
			int hdrDarkToeClampedSamples = 0;
			// SOTA #5 (default OFF): 1-step analytical Gauss-Newton sub-pixel phase refinement diagnostics.
			bool phaseRefined = false;
			bool phaseRefineFallback = false;
			double phaseRefineStep = 0.0;
		};

		struct VerifyResult
		{
			uint32_t reason = 0;
			double costMs = 0.0;      // wall time of this Evaluate (needs VerifierSettings::nowMs), for a CPU-share governor
			bool cold = false;        // operators were built during this call (a cache miss), so the cost is the cold one
			int operatorBuilds = 0;   // deterministic work counters of this call
			int solves = 0;           // line solves executed
			int solvesReused = 0;     // line solves read back from an earlier identical solve of the same call (not executed)
			int phaseRefineFallbacks = 0; // SOTA #5 (Rule 4): ill-conditioned or non-improving Gauss-Newton phase refinement fallbacks
			std::vector<CandidateResult> candidates;
		};

		class DescaleVerifier
		{
		public:
			explicit DescaleVerifier(const VerifierSettings& s = VerifierSettings()) : s_(s), taps_(TapEntriesFor(s.cacheBytes)) {}

			const VerifierSettings& Settings() const { return s_; }
			void ClearCache() { cache_.clear(); lru_.clear(); cacheBytes_ = 0; memo_.clear(); memoBytes_ = 0; hdrWs_.Reset(); gnZ_.clear(); gnV_.clear(); gnDz_.clear(); gnPred_.clear(); gnProj_.clear(); gnDw_.clear(); }
			size_t CachedOperators() const { return cache_.size(); }
			// Heap bytes of everything this verifier keeps for speed: operators + tap memo + (during a call) the kept solves. After every
			// Evaluate this is <= cacheBytes (unless ONE operator alone is larger than the budget); the tap memo survives ClearCache.
			size_t CachedBytes() const { return cacheBytes_ + taps_.Bytes() + memoBytes_; }

			bool Evaluate(const LineSet& ls, const int* cand, int count, const VerifyOptions& opt, VerifyResult& out)
			{
				out.reason = 0;
				out.costMs = 0.0;
				out.cold = false;
				out.operatorBuilds = 0;
				out.solves = 0;
				out.solvesReused = 0;
				out.phaseRefineFallbacks = 0;
				out.candidates.clear();
				const double clock0 = s_.nowMs != nullptr ? s_.nowMs(s_.clockCtx) : 0.0;
				const int builds0 = builds_;
				const int solves0 = solves_;
				const int reused0 = reused_;
				const int phaseFallbacks0 = phaseRefineFallbacks_;
				memo_.clear();   // the solves are those of THIS call's lines
				memoBytes_ = 0;
				if (ls.count < 1 || ls.axisLen < 64 || ls.length < 96 || cand == nullptr || count <= 0)
				{
					out.reason = (count <= 0) ? kReasonNoCandidates : kReasonBadInput;
					return false;
				}
				const int N = ls.axisLen;
				const int maxN = static_cast<int>(std::floor(s_.maxLatentRatio * static_cast<double>(N)));
				out.candidates.resize(static_cast<size_t>(count));
				std::vector<double> coarseT(static_cast<size_t>(count), 0.0);
				for (int c = 0; c < count; ++c)
				{
					CandidateResult& cr = out.candidates[static_cast<size_t>(c)];
					cr.nProposed = cand[c];
					cr.n = cand[c];
					if (cand[c] < s_.minLatent || cand[c] > maxN)
					{
						cr.reason |= kReasonBadInput;
						continue;
					}
					if (opt.exactOnly)
					{
						cr.valid = true;
						continue;
					}
					int nBest = 0;
					double tBest = 0.0;
					const uint32_t why = CoarseScan(ls, cand[c], nBest, tBest);
					if (why != 0u)
					{
						cr.reason |= why;
						continue;
					}
					cr.valid = true;
					cr.n = nBest;
					cr.coarseStat = tBest;
					cr.stat = tBest;
					cr.lineCount = 0;
					coarseT[static_cast<size_t>(c)] = tBest;
				}
				// Duplicates after refinement: keep the first.
				for (int c = 0; c < count; ++c)
				{
					CandidateResult& cr = out.candidates[static_cast<size_t>(c)];
					if (!cr.valid)
						continue;
					for (int d = 0; d < c; ++d)
					{
						const CandidateResult& o = out.candidates[static_cast<size_t>(d)];
						if (o.valid && o.n == cr.n)
						{
							cr.valid = false;
							cr.reason |= kReasonNoCandidates;
							break;
						}
					}
				}
				// Promote the best candidates (by coarse statistic, then by size) to the exact stage.
				std::vector<int> order;
				for (int c = 0; c < count; ++c)
					if (out.candidates[static_cast<size_t>(c)].valid)
						order.push_back(c);
				std::sort(order.begin(), order.end(), [&](int a, int b) {
					const double ta = coarseT[static_cast<size_t>(a)];
					const double tb = coarseT[static_cast<size_t>(b)];
					if (ta != tb)
						return ta < tb;
					const int na = out.candidates[static_cast<size_t>(a)].n;
					const int nb = out.candidates[static_cast<size_t>(b)].n;
					if (na != nb)
						return na < nb;
					return a < b;
				});
				if (opt.exactOnly && opt.p3TwoPass)
				{
					VerifyOptions screenOpt = opt;
					screenOpt.exactHalfWidth = 0;
					for (size_t i = 0; i < order.size(); ++i)
					{
						CandidateResult& cr = out.candidates[static_cast<size_t>(order[i])];
						const uint32_t why = ExactStage(ls, cr.n, screenOpt, cr, true);
						if (why != 0u)
						{
							cr.valid = false;
							cr.reason |= why;
						}
					}
					std::vector<int> screenOrder;
					screenOrder.reserve(order.size());
					for (size_t i = 0; i < order.size(); ++i)
						if (out.candidates[static_cast<size_t>(order[i])].valid)
							screenOrder.push_back(order[i]);
					std::sort(screenOrder.begin(), screenOrder.end(), [&](int a, int b) {
						const CandidateResult& ca = out.candidates[static_cast<size_t>(a)];
						const CandidateResult& cb = out.candidates[static_cast<size_t>(b)];
						if (ca.stat != cb.stat)
							return ca.stat < cb.stat;
						if (ca.n != cb.n)
							return ca.n < cb.n;
						return a < b;
					});
					const int keepTop = opt.p3ScreenKeepTop > 0 ? opt.p3ScreenKeepTop : 0;
					auto nearProtected = [&](int nProp) -> bool {
						if (opt.forceExactN > 0 && std::abs(nProp - opt.forceExactN) <= 1)
							return true;
						for (int p = 0; p < 2; ++p)
							if (opt.p3ProtectedN[p] > 0 && std::abs(nProp - opt.p3ProtectedN[p]) <= 1)
								return true;
						return false;
					};
					for (size_t i = 0; i < screenOrder.size(); ++i)
					{
						CandidateResult& cr = out.candidates[static_cast<size_t>(screenOrder[i])];
						const bool oddCandidate = (cr.nProposed & 1) != 0;   // cropped scope rasters (e.g. 1600 * 2/3 = 1067) may sit 1 px off an even coded size
						const bool promoteFull = static_cast<int>(i) < keepTop
							|| nearProtected(cr.nProposed)
							|| oddCandidate
							|| cr.stat < opt.p3ScreenDipThreshold;
						if (!promoteFull)
						{
							// M3 (Rule 4): Pass-1 screen-only candidate (hw=0, representative kernels, phase 0) was not
							// fully verified; clear exactDone and tag kReasonScreenOnly so ChainEvidence and Dump v4
							// exclude it from LLR and never confuse it with a full exact evaluation.
							cr.exactDone = false;
							cr.reason |= kReasonScreenOnly;
							continue;
						}
						const int nProp = cr.nProposed;
						cr = CandidateResult();
						cr.nProposed = nProp;
						cr.n = nProp;
						cr.valid = true;
						const uint32_t why = ExactStage(ls, nProp, opt, cr, false);
						if (why != 0u)
						{
							cr.valid = false;
							cr.reason |= why;
						}
					}
				}
				else
				{
					int maxExact = s_.maxExactCandidates;
					if (opt.forceExactN > 0 && s_.confidentMaxExactCandidates > 0 && s_.confidentMaxExactCandidates < maxExact)
						maxExact = s_.confidentMaxExactCandidates;   // lever (Narrowing): a confident axis needs its own n plus fewer challengers
					const int promote = opt.exactOnly ? static_cast<int>(order.size())
						: (std::min<int>)(static_cast<int>(order.size()), maxExact);
					for (int i = 0; i < static_cast<int>(order.size()); ++i)
					{
						CandidateResult& cr = out.candidates[static_cast<size_t>(order[static_cast<size_t>(i)])];
						const bool forced = opt.forceExactN > 0 && cr.nProposed == opt.forceExactN;
						if (i < promote || forced)
						{
							const uint32_t why = ExactStage(ls, cr.n, opt, cr, false);
							if (why != 0u)
							{
								cr.valid = false;
								cr.reason |= why;
							}
						}
						else
							cr.reason |= kReasonCoarseOnly;
					}
				}
				for (int c = 0; c < count; ++c)
					out.reason |= out.candidates[static_cast<size_t>(c)].reason;
				out.operatorBuilds = builds_ - builds0;
				out.solves = solves_ - solves0;
				out.solvesReused = reused_ - reused0;
				out.phaseRefineFallbacks = phaseRefineFallbacks_ - phaseFallbacks0;
				Evict();   // with the kept solves still counted: the call's peak, not only its end state, is inside the budget
				memo_.clear();
				memoBytes_ = 0;
				out.cold = out.operatorBuilds > 0;
				out.costMs = s_.nowMs != nullptr ? s_.nowMs(s_.clockCtx) - clock0 : 0.0;
				return true;
			}

		private:
			struct OpKey
			{
				int N, n, kernel, phaseQ, origin, length;
				bool operator<(const OpKey& o) const
				{
					if (N != o.N) return N < o.N;
					if (n != o.n) return n < o.n;
					if (kernel != o.kernel) return kernel < o.kernel;
					if (phaseQ != o.phaseQ) return phaseQ < o.phaseQ;
					if (origin != o.origin) return origin < o.origin;
					return length < o.length;
				}
			};

			// The solves of one set of lines (a stage's line plan) against one operator, kept for the rest of the Evaluate call: a second
			// candidate (or reference size) that needs the same (window, n, kernel, phase) reads these instead of solving again. The numbers
			// are the ones a fresh solve would give (same operator, same lines), so reuse never changes a result.
			struct SolveKey
			{
				OpKey op;
				int stage;   // 0 = coarse line plan (no segment sums kept), 1 = exact line plan (segment sums kept)
				bool operator<(const SolveKey& o) const
				{
					if (stage != o.stage) return stage < o.stage;
					return op < o.op;
				}
			};

			struct SolveBatch
			{
				bool ok = false;
				double dof = 1.0;               // 1 - nodes / rows of the operator (floored), the same for every line
				std::vector<double> s;          // per line: sqrt((ms + eps^2) / dof)
				std::vector<double> ms;         // per line: mean square residual over the inner rows
				std::vector<double> segSum;     // nl x kMaxSegs (stage 1 only)
				std::vector<int> segRows;       // kMaxSegs (stage 1 only), the same for every line
			};

			struct CacheEntry
			{
				std::unique_ptr<DescaleOperator> op;
				std::list<OpKey>::iterator pos;   // place in lru_
				size_t bytes = 0;
			};

			// The operator for key `k`, built on a miss (with the verifier's shared tap memo). Least recently used operators leave first
			// when the count or the byte budget is exceeded; the operator just built always stays. Eviction only costs time.
			const DescaleOperator* Op(const OpKey& k, double phase)
			{
				auto it = cache_.find(k);
				if (it != cache_.end())
				{
					lru_.splice(lru_.end(), lru_, it->second.pos);
					return it->second.op->Ok() ? it->second.op.get() : nullptr;
				}
				CacheEntry e;
				e.op.reset(new DescaleOperator());
				e.op->Build(k.N, k.n, k.kernel, phase, k.origin, k.length, &taps_);
				++builds_;
				e.bytes = e.op->Bytes();
				lru_.push_back(k);
				e.pos = std::prev(lru_.end());
				const DescaleOperator* raw = e.op.get();
				const bool ok = raw->Ok();
				cacheBytes_ += e.bytes;
				cache_.emplace(k, std::move(e));
				Evict();
				return ok ? raw : nullptr;
			}

			// Tap-memo entries a budget allows: the memo (at most about 120 bytes per entry including table and growth slack) stays under
			// 1/16 of cacheBytes, never more than 65536 entries, never fewer than 256. 0 = no byte limit = the full 65536.
			static size_t TapEntriesFor(size_t cacheBytes)
			{
				if (cacheBytes == 0)
					return 65536;
				const size_t e = cacheBytes / 16 / 120;
				return e < 256 ? 256 : (e > 65536 ? 65536 : e);
			}

			void Evict()
			{
				const size_t limit = s_.cacheLimit < 1 ? 1 : s_.cacheLimit;
				// The byte budget covers ALL of it: operators + tap memo + the solves kept for the current call.
				while (cache_.size() > 1 && (cache_.size() > limit || (s_.cacheBytes > 0 && cacheBytes_ + taps_.Bytes() + memoBytes_ > s_.cacheBytes)))
				{
					auto victim = cache_.find(lru_.front());
					cacheBytes_ -= victim->second.bytes;
					cache_.erase(victim);
					lru_.pop_front();
				}
			}

			static int PhaseQ(double phase) { return static_cast<int>(std::floor(phase * 1024.0 + 0.5)); }

			// The lines lineIdx[0 .. nl) of one window at (n, kernel, phase), solved together (or, with batchedSolve off, one by one by the
			// reference routine; both give the same bits). nullptr when the operator is unusable. The result lives in memo_ (stable until the
			// next Evaluate). stage 1 keeps the per-256-px segment sums of every line.
			const SolveBatch* SolveWindow(const LineSet& ls, int stage, const int* lineIdx, int nl, int winOff, int winLen, int n, int kernel, double phase)
			{
				SolveKey sk;
				sk.op.N = ls.axisLen;
				sk.op.n = n;
				sk.op.kernel = kernel;
				sk.op.phaseQ = PhaseQ(phase);
				sk.op.origin = ls.origin + winOff;
				sk.op.length = winLen;
				sk.stage = stage;
				auto found = memo_.find(sk);
				if (found != memo_.end())
				{
					reused_ += nl;
					return found->second.ok ? &found->second : nullptr;
				}
				SolveBatch& b = memo_[sk];
				solves_ += nl;
				const DescaleOperator* op = Op(sk.op, static_cast<double>(sk.op.phaseQ) / 1024.0);
				if (op == nullptr)
					return nullptr;
				const bool keepSeg = stage == 1;
				b.s.assign(static_cast<size_t>(nl), 0.0);
				b.ms.assign(static_cast<size_t>(nl), 0.0);
				b.segRows.assign(static_cast<size_t>(kMaxSegs), 0);
				if (keepSeg)
					b.segSum.assign(static_cast<size_t>(nl) * static_cast<size_t>(kMaxSegs), 0.0);
				b.dof = 1.0 - static_cast<double>(op->Nodes()) / static_cast<double>(op->Length());   // rows minus the fit unknowns
				if (b.dof < 0.02)
					b.dof = 0.02;
				double rss[kResidualBatch];
				double segTmp[kResidualBatch * kMaxSegs];
				int segRowsTmp[kMaxSegs];
				const double* y[kResidualBatch];
				for (int i0 = 0; i0 < nl; i0 += kResidualBatch)
				{
					const int lanes = (std::min)(kResidualBatch, nl - i0);
					int rows = 0;
					if (s_.batchedSolve)
					{
						for (int g = 0; g < lanes; ++g)
							y[g] = ls.Line(lineIdx[i0 + g]) + winOff;
						op->ResidualBatch(y, lanes, scratch_, rss, rows, segTmp, segRowsTmp);
					}
					else
					{
						for (int g = 0; g < lanes; ++g)
						{
							double r1 = 0.0;
							int rows1 = 0;
							op->Residual(ls.Line(lineIdx[i0 + g]) + winOff, scratch_, r1, rows1, segTmp + static_cast<size_t>(g) * kMaxSegs, segRowsTmp);
							rss[g] = r1;
							rows = rows1;
						}
					}
					if (rows <= 0)
						return nullptr;
					for (int g = 0; g < lanes; ++g)
					{
						const double ms = rss[g] / static_cast<double>(rows);
						b.ms[static_cast<size_t>(i0 + g)] = ms;
						b.s[static_cast<size_t>(i0 + g)] = std::sqrt((ms + s_.epsCodes * s_.epsCodes) / b.dof);
						if (keepSeg)
							for (int k = 0; k < kMaxSegs; ++k)
								b.segSum[static_cast<size_t>(i0 + g) * kMaxSegs + static_cast<size_t>(k)] = segTmp[static_cast<size_t>(g) * kMaxSegs + static_cast<size_t>(k)];
					}
					for (int k = 0; k < kMaxSegs; ++k)
						b.segRows[static_cast<size_t>(k)] = segRowsTmp[k];
				}
				b.ok = true;
				memoBytes_ += (b.s.capacity() + b.ms.capacity() + b.segSum.capacity()) * sizeof(double) + b.segRows.capacity() * sizeof(int)
					+ sizeof(SolveKey) + sizeof(SolveBatch) + 48;   // 48 = the map node's own bookkeeping, an estimate
				return &b;
			}

			int RefSizes(int n0, int N, int* out) const
			{
				const int maxN = static_cast<int>(std::floor(s_.maxLatentRatio * static_cast<double>(N)));
				const double fr[2] = { s_.refFraction1, s_.refFraction2 };
				int k = 0;
				for (int a = 0; a < 2; ++a)
					for (int sg = -1; sg <= 1; sg += 2)
					{
						const int nr = RoundToInt(static_cast<double>(n0) * (1.0 + static_cast<double>(sg) * fr[a]));
						if (nr >= s_.minLatent && nr <= maxN && nr != n0)
							out[k++] = nr;
					}
				return k;
			}

			// SID-OPT: stack-allocated small-array median / trimmed mean (100% bit-identical to RobustStats::MedianOf / TrimmedMeanOf, 0 heap allocs).
			static double FastMedianSmall(const double* v, int n)
			{
				if (n <= 0)
					return 0.0;
				if (n == 1)
					return v[0];
				if (n == 2)
					return 0.5 * (v[0] + v[1]);
				if (n <= kMaxLines * 2)
				{
					double buf[kMaxLines * 2];
					for (int i = 0; i < n; ++i)
						buf[i] = v[i];
					std::sort(buf, buf + n);
					return (n & 1) != 0 ? buf[n / 2] : 0.5 * (buf[n / 2 - 1] + buf[n / 2]);
				}
				return RobustStats::MedianOf(v, n);
			}

			static double FastTrimmedMeanSmall(const double* v, int n, double trim)
			{
				if (n <= 0)
					return 0.0;
				if (n <= kMaxLines * 2)
				{
					double buf[kMaxLines * 2];
					for (int i = 0; i < n; ++i)
						buf[i] = v[i];
					std::sort(buf, buf + n);
					int k = static_cast<int>(std::floor(trim * static_cast<double>(n)));
					if (k < 0)
						k = 0;
					if (n - 2 * k <= 0)
						k = 0;
					double s = 0.0;
					for (int i = k; i < n - k; ++i)
						s += buf[i];
					return s / static_cast<double>(n - 2 * k);
				}
				return RobustStats::TrimmedMeanOf(v, n, trim);
			}

			// Per-line baseline of one window geometry: the median over the reference sizes of the smallest s over `reps`.
			// refSeg (optional, nref x nl x kMaxSegs) receives the segment level of the
			// winning kernel at every reference ((mean square per row + eps^2) / dof, as the line level), -1 where the segment has no rows
			// (needs stage 1: only that stage keeps segment sums).
			bool Baseline(const LineSet& ls, int stage, const int* lineIdx, int nl, int winOff, int winLen, int n0, const int* reps, int nrep,
				double* base, std::vector<double>* refSeg, int& nref)
			{
				int refs[4];
				nref = RefSizes(n0, ls.axisLen, refs);
				if (nref < 2)
					return false;
				perRefScratch_.assign(static_cast<size_t>(nref) * static_cast<size_t>(nl), 0.0);
				if (refSeg != nullptr)
					refSeg->assign(static_cast<size_t>(nref) * static_cast<size_t>(nl) * static_cast<size_t>(kMaxSegs), -1.0);
				const SolveBatch* bq[kFamilyCount];
				for (int r = 0; r < nref; ++r)
				{
					for (int q = 0; q < nrep; ++q)
					{
						bq[q] = SolveWindow(ls, stage, lineIdx, nl, winOff, winLen, refs[r], reps[q], 0.0);
						if (bq[q] == nullptr)
							return false;
					}
					for (int i = 0; i < nl; ++i)
					{
						double best = 0.0;
						int bestQ = 0;
						bool have = false;
						for (int q = 0; q < nrep; ++q)
						{
							const double s = bq[q]->s[static_cast<size_t>(i)];
							if (!have || s < best)
							{
								best = s;
								bestQ = q;
								have = true;
							}
						}
						perRefScratch_[static_cast<size_t>(r) * static_cast<size_t>(nl) + static_cast<size_t>(i)] = best;
						if (refSeg != nullptr)
						{
							const SolveBatch& wb = *bq[bestQ];
							for (int g = 0; g < kMaxSegs; ++g)
								if (wb.segRows[static_cast<size_t>(g)] > 0)
									(*refSeg)[(static_cast<size_t>(r) * static_cast<size_t>(nl) + static_cast<size_t>(i)) * kMaxSegs + static_cast<size_t>(g)]
										= (wb.segSum[static_cast<size_t>(i) * kMaxSegs + static_cast<size_t>(g)] / static_cast<double>(wb.segRows[static_cast<size_t>(g)])
											+ s_.epsCodes * s_.epsCodes) / wb.dof;
						}
					}
				}
				double col[4];
				for (int i = 0; i < nl; ++i)
				{
					for (int r = 0; r < nref; ++r)
						col[r] = perRefScratch_[static_cast<size_t>(r) * static_cast<size_t>(nl) + static_cast<size_t>(i)];
					base[i] = FastMedianSmall(col, nref);
				}
				return true;
			}

			uint32_t CoarseScan(const LineSet& ls, int nC, int& nBest, double& tBest)
			{
				const int N = ls.axisLen;
				const int L = ls.length;
				int Lc = (std::max)(s_.coarseMinWindow, static_cast<int>(std::floor(s_.coarseWindowFraction * static_cast<double>(N))));
				if (Lc > L)
					Lc = L;
				const int nWin = (Lc >= L || s_.coarseWindows <= 1) ? 1 : 2;
				const int winOff[2] = { 0, L - Lc };
				const int nl = (std::min)(ls.count, (std::max)(1, s_.coarseLines));
				idxScratch_.resize(static_cast<size_t>(nl));
				for (int i = 0; i < nl; ++i)
					idxScratch_[static_cast<size_t>(i)] = (i * ls.count) / nl;
				int nrep = 0;
				const int* reps = RepresentativeKernels(nrep);
				baseScratch_.assign(static_cast<size_t>(nWin) * static_cast<size_t>(nl), 0.0);
				for (int w = 0; w < nWin; ++w)
				{
					int nref = 0;
					if (!Baseline(ls, 0, idxScratch_.data(), nl, winOff[w], Lc, nC, reps, nrep, &baseScratch_[static_cast<size_t>(w) * static_cast<size_t>(nl)], nullptr, nref))
						return nref < 2 ? kReasonNoReference : kReasonShortLines;
				}
				const int maxN = static_cast<int>(std::floor(s_.maxLatentRatio * static_cast<double>(N)));
				const int step = s_.coarseStep < 1 ? 1 : s_.coarseStep;
				std::vector<double> cPhases;
				cPhases.push_back(0.0);
				for (int q = 1; q <= (s_.phaseCount - 1) / 2; ++q)
				{
					cPhases.push_back(static_cast<double>(q) * s_.phaseStep);
					cPhases.push_back(-static_cast<double>(q) * s_.phaseStep);
				}
				const size_t cPhaseCount = cPhases.size();
				valsScratch_.resize(static_cast<size_t>(nWin) * static_cast<size_t>(nl));
				bool have = false;
				const SolveBatch* bw[2][kFamilyCount];
				auto visit = [&](int d) {
					const int n = nC + d;
					if (n < s_.minLatent || n > maxN)
						return;
					if (cPhaseCount == 1u)
					{
						bool ok = true;
						for (int w = 0; w < nWin && ok; ++w)
							for (int q = 0; q < nrep; ++q)
							{
								bw[w][q] = SolveWindow(ls, 0, idxScratch_.data(), nl, winOff[w], Lc, n, reps[q], 0.0);
								if (bw[w][q] == nullptr)
								{
									ok = false;
									break;
								}
							}
						if (!ok)
							return;
						for (int w = 0; w < nWin; ++w)
							for (int i = 0; i < nl; ++i)
							{
								double best = 0.0;
								bool h2 = false;
								for (int q = 0; q < nrep; ++q)
								{
									const double s = bw[w][q]->s[static_cast<size_t>(i)];
									if (!h2 || s < best)
									{
										best = s;
										h2 = true;
									}
								}
								valsScratch_[static_cast<size_t>(w) * static_cast<size_t>(nl) + static_cast<size_t>(i)]
									= std::log(best / baseScratch_[static_cast<size_t>(w) * static_cast<size_t>(nl) + static_cast<size_t>(i)]);
							}
					}
					else
					{
						// When phaseCount > 1 (default 1 untouched for W1 bit-identity), evaluate configured phases
						// so off-phase upscales (e.g. +-0.25 bob fields or sub-pixel shifts) never drift off true n.
						double bestLine[2 * kMaxLines];
						bool haveLine[2 * kMaxLines];
						const int total = nWin * nl;
						for (int idx = 0; idx < total; ++idx)
						{
							bestLine[idx] = 0.0;
							haveLine[idx] = false;
						}
						for (size_t pi = 0; pi < cPhaseCount; ++pi)
						{
							for (int w = 0; w < nWin; ++w)
							{
								for (int q = 0; q < nrep; ++q)
								{
									const SolveBatch* sb = SolveWindow(ls, 0, idxScratch_.data(), nl, winOff[w], Lc, n, reps[q], cPhases[pi]);
									if (sb == nullptr)
										return;
									for (int i = 0; i < nl; ++i)
									{
										const int idx = w * nl + i;
										const double s = sb->s[static_cast<size_t>(i)];
										if (!haveLine[idx] || s < bestLine[idx])
										{
											bestLine[idx] = s;
											haveLine[idx] = true;
										}
									}
								}
							}
						}
						for (int idx = 0; idx < total; ++idx)
							valsScratch_[static_cast<size_t>(idx)] = std::log(bestLine[idx] / baseScratch_[static_cast<size_t>(idx)]);
					}
					const double T = FastTrimmedMeanSmall(valsScratch_.data(), nWin * nl, s_.trimFraction);
					const bool better = !have || T < tBest || (T == tBest && std::abs(d) < std::abs(nBest - nC));
					if (better)
					{
						have = true;
						tBest = T;
						nBest = n;
					}
				};
				for (int d = -s_.coarseHalfWidth; d <= s_.coarseHalfWidth; d += step)
					visit(d);
				if (s_.coarseRefine && step > 1 && have)
				{
					// The grid skipped step - 1 integers between its points: look at those within step - 1 of the grid's best.
					const int bestD = nBest - nC;
					for (int d = bestD - (step - 1); d <= bestD + (step - 1); ++d)
						if (d >= -s_.coarseHalfWidth && d <= s_.coarseHalfWidth && ((d + s_.coarseHalfWidth) % step) != 0)
							visit(d);
				}
				return have ? 0u : kReasonShortLines;
			}

			uint32_t ExactStage(const LineSet& ls, int n0, const VerifyOptions& opt, CandidateResult& cr, bool screenOnly = false)
			{
				const int N = ls.axisLen;
				const int L = ls.length;
				const int avail = (std::min)(ls.count, kMaxLines);
				const int nl = (opt.exactLines > 0 && opt.exactLines < avail) ? opt.exactLines : avail;
				idxScratch_.resize(static_cast<size_t>(nl));
				for (int i = 0; i < nl; ++i)
					idxScratch_[static_cast<size_t>(i)] = (i * avail) / nl;
				int repAll = 0;
				const int* repsAll = RepresentativeKernels(repAll);
				int repOne[1] = { opt.onlyKernel };
				const int* reps = opt.onlyKernel >= 0 ? repOne : repsAll;
				const int nrep = opt.onlyKernel >= 0 ? 1 : repAll;
				baseScratch_.assign(static_cast<size_t>(nl), 0.0);
				int nref = 0;
				if (!Baseline(ls, 1, idxScratch_.data(), nl, 0, L, n0, reps, nrep, baseScratch_.data(), &refSegScratch_, nref))
					return nref < 2 ? kReasonNoReference : kReasonShortLines;

				// Integer scan with the representative kernels.
				std::vector<double> phases;
				phases.push_back(0.0);
				if (!screenOnly)
				{
					for (int q = 1; q <= (s_.phaseCount - 1) / 2; ++q)
					{
						phases.push_back(static_cast<double>(q) * s_.phaseStep);
						phases.push_back(-static_cast<double>(q) * s_.phaseStep);
					}
				}
				const bool multiPhase = phases.size() > 1u;

				const int maxN = static_cast<int>(std::floor(s_.maxLatentRatio * static_cast<double>(N)));
				const int hw = screenOnly ? 0 : (opt.exactHalfWidth >= 0 ? opt.exactHalfWidth : s_.exactHalfWidth);
				std::vector<int> ns;
				ns.reserve(static_cast<size_t>(2 * hw + 2));
				for (int n = n0 - hw; n <= n0 + hw; ++n)
					if (n >= s_.minLatent && n <= maxN)
						ns.push_back(n);
				if ((multiPhase || s_.gaussNewtonPhaseRefine) && !screenOnly &&
					cr.nProposed >= s_.minLatent && cr.nProposed <= maxN &&
					std::find(ns.begin(), ns.end(), cr.nProposed) == ns.end())
				{
					ns.push_back(cr.nProposed);
				}
				if (ns.empty())
					return kReasonBadInput;
				const size_t stride = static_cast<size_t>(nrep) * static_cast<size_t>(nl);
				scanSScratch_.resize(ns.size() * stride);
				valsScratch_.resize(static_cast<size_t>(nl));
				int bestNi = -1;
				double bestT = 0.0;
				for (size_t ni = 0; ni < ns.size(); ++ni)
				{
					if (!multiPhase)
					{
						for (int q = 0; q < nrep; ++q)
						{
							const SolveBatch* sb = SolveWindow(ls, 1, idxScratch_.data(), nl, 0, L, ns[ni], reps[q], 0.0);
							if (sb == nullptr)
								return kReasonShortLines;
							for (int i = 0; i < nl; ++i)
								scanSScratch_[ni * stride + static_cast<size_t>(q) * static_cast<size_t>(nl) + static_cast<size_t>(i)] = sb->s[static_cast<size_t>(i)];
						}
					}
					else
					{
						// When phaseCount > 1, evaluate each (ns[ni], reps[q]) over all configured phases so nStar
						// and exactFamilies selection never lock onto the wrong integer or family on off-phase inputs.
						for (int q = 0; q < nrep; ++q)
						{
							double bestPhaseT = 0.0;
							const SolveBatch* bestSb = nullptr;
							for (size_t pi = 0; pi < phases.size(); ++pi)
							{
								const SolveBatch* sb = SolveWindow(ls, 1, idxScratch_.data(), nl, 0, L, ns[ni], reps[q], phases[pi]);
								if (sb == nullptr)
									return kReasonShortLines;
								for (int i = 0; i < nl; ++i)
									valsScratch_[static_cast<size_t>(i)] = std::log(sb->s[static_cast<size_t>(i)] / baseScratch_[static_cast<size_t>(i)]);
								const double Tp = FastTrimmedMeanSmall(valsScratch_.data(), nl, s_.trimFraction);
								if (bestSb == nullptr || Tp < bestPhaseT)
								{
									bestPhaseT = Tp;
									bestSb = sb;
								}
							}
							for (int i = 0; i < nl; ++i)
								scanSScratch_[ni * stride + static_cast<size_t>(q) * static_cast<size_t>(nl) + static_cast<size_t>(i)] = bestSb->s[static_cast<size_t>(i)];
						}
					}
					for (int i = 0; i < nl; ++i)
					{
						double best = 0.0;
						for (int q = 0; q < nrep; ++q)
						{
							const double s = scanSScratch_[ni * stride + static_cast<size_t>(q) * static_cast<size_t>(nl) + static_cast<size_t>(i)];
							if (q == 0 || s < best)
								best = s;
						}
						valsScratch_[static_cast<size_t>(i)] = std::log(best / baseScratch_[static_cast<size_t>(i)]);
					}
					const double T = FastTrimmedMeanSmall(valsScratch_.data(), nl, s_.trimFraction);
					bool better = bestNi < 0 || T < bestT;
					if (!better && T == bestT)
						better = std::abs(ns[ni] - n0) < std::abs(ns[static_cast<size_t>(bestNi)] - n0);
					if (better)
					{
						bestNi = static_cast<int>(ni);
						bestT = T;
					}
				}
				const int nStar = ns[static_cast<size_t>(bestNi)];

				// Kernels to evaluate in full at nStar: every kernel of the best families (or only rep kernels during Pass-1 screen).
				int kernels[kKernelCount];
				int kernelCount = 0;
				if (opt.onlyKernel >= 0)
				{
					kernels[kernelCount++] = opt.onlyKernel;
				}
				else if (screenOnly)
				{
					for (int q = 0; q < nrep && kernelCount < kKernelCount; ++q)
						kernels[kernelCount++] = reps[q];
				}
				else
				{
					double famT[kFamilyCount];
					int famOrder[kFamilyCount];
					for (int q = 0; q < nrep; ++q)
					{
						for (int i = 0; i < nl; ++i)
							valsScratch_[static_cast<size_t>(i)] = std::log(scanSScratch_[static_cast<size_t>(bestNi) * stride + static_cast<size_t>(q) * static_cast<size_t>(nl) + static_cast<size_t>(i)] / baseScratch_[static_cast<size_t>(i)]);
						famT[static_cast<int>(Kernel(reps[q]).family)] = FastTrimmedMeanSmall(valsScratch_.data(), nl, s_.trimFraction);
					}
					for (int f = 0; f < kFamilyCount; ++f)
						famOrder[f] = f;
					std::sort(famOrder, famOrder + kFamilyCount, [&](int a, int b) {
						if (famT[a] != famT[b])
							return famT[a] < famT[b];
						return a < b;
					});
					const int nf = (std::max)(1, (std::min)(s_.exactFamilies, kFamilyCount));
					for (int k = 0; k < kKernelCount; ++k)
						for (int f = 0; f < nf; ++f)
							if (static_cast<int>(Kernel(k).family) == famOrder[f])
							{
								kernels[kernelCount++] = k;
								break;
							}
				}

				struct KernelScore { int kernel; double phase; double T; };
				KernelScore scores[kKernelCount];
				int scoreCount = 0;
				for (int ki = 0; ki < kernelCount; ++ki)
				{
					KernelScore best;
					best.kernel = kernels[ki];
					best.phase = 0.0;
					best.T = 0.0;
					bool have = false;
					for (size_t pi = 0; pi < phases.size(); ++pi)
					{
						// Reuse the scan value when single-phase (phase 0, representative kernel; W1 bit-exact).
						// When multiPhase is active, SolveWindow reads directly from memo_ with zero extra work.
						int reusedQ = -1;
						if (!multiPhase && pi == 0)
							for (int q = 0; q < nrep; ++q)
								if (reps[q] == kernels[ki])
								{
									reusedQ = q;
									break;
								}
						const SolveBatch* sb = nullptr;
						if (reusedQ < 0)
						{
							sb = SolveWindow(ls, 1, idxScratch_.data(), nl, 0, L, nStar, kernels[ki], phases[pi]);
							if (sb == nullptr)
								return kReasonShortLines;
						}
						for (int i = 0; i < nl; ++i)
						{
							const double s = reusedQ >= 0
								? scanSScratch_[static_cast<size_t>(bestNi) * stride + static_cast<size_t>(reusedQ) * static_cast<size_t>(nl) + static_cast<size_t>(i)]
								: sb->s[static_cast<size_t>(i)];
							valsScratch_[static_cast<size_t>(i)] = std::log(s / baseScratch_[static_cast<size_t>(i)]);
						}
						const double T = FastTrimmedMeanSmall(valsScratch_.data(), nl, s_.trimFraction);
						if (!have || T < best.T)
						{
							have = true;
							best.T = T;
							best.phase = phases[pi];
						}
					}
					scores[scoreCount++] = best;
				}
				int bi = 0;
				for (int k = 1; k < scoreCount; ++k)
					if (scores[k].T < scores[bi].T)
						bi = k;
				cr.kernelMask = 0;
				for (int k = 0; k < scoreCount; ++k)
				{
					cr.kernelMask |= 1u << scores[k].kernel;
					cr.kernelT[scores[k].kernel] = scores[k].T;
				}
				double second = 0.0;
				bool haveSecond = false;
				double otherFam = 0.0;
				bool haveOther = false;
				for (int k = 0; k < scoreCount; ++k)
				{
					if (k == bi)
						continue;
					if (!haveSecond || scores[k].T < second)
					{
						second = scores[k].T;
						haveSecond = true;
					}
					if (Kernel(scores[k].kernel).family != Kernel(scores[bi].kernel).family && (!haveOther || scores[k].T < otherFam))
					{
						otherFam = scores[k].T;
						haveOther = true;
					}
				}

				// Final pass at the winner with segment sums (the solves were kept by the scan above, so this reads them back).
				cr.n = nStar;
				cr.kernel = scores[bi].kernel;
				cr.family = static_cast<int>(Kernel(cr.kernel).family);
				cr.phase = scores[bi].phase;
				cr.stat = scores[bi].T;
				cr.kernelMargin = haveSecond ? second - scores[bi].T : 0.0;
				cr.familyMargin = haveOther ? otherFam - scores[bi].T : 0.0;
				// SOTA #5 (opt-in, default OFF): analytical Golub-Pereyra variable-projection Gauss-Newton phase refinement
				// (up to 2 safeguarded iterations, total step clamped strictly to [-0.25, +0.25] from discrete grid winner).
				if (s_.gaussNewtonPhaseRefine && !screenOnly)
				{
					const double phiInit = static_cast<double>(PhaseQ(cr.phase)) / 1024.0;
					for (int gnIter = 0; gnIter < 2; ++gnIter)
					{
						OpKey gnKey;
						gnKey.N = N;
						gnKey.n = nStar;
						gnKey.kernel = cr.kernel;
						gnKey.phaseQ = PhaseQ(cr.phase);
						gnKey.origin = ls.origin;
						gnKey.length = L;
						const double phi0 = static_cast<double>(gnKey.phaseQ) / 1024.0;
						const DescaleOperator* op0 = Op(gnKey, phi0);
						if (op0 == nullptr || !op0->Ok())
						{
							if (gnIter == 0)
							{
								cr.phaseRefineFallback = true;
								++phaseRefineFallbacks_;
							}
							break;
						}
						const int taps = op0->Taps();
						const int margin = op0->Margin();
						const double ratio = static_cast<double>(nStar) / static_cast<double>(N);
						const int* cl = op0->FirstNodes();
						gnV_.assign(static_cast<size_t>(L), 0.0);
						gnPred_.assign(static_cast<size_t>(L), 0.0);
						gnProj_.assign(static_cast<size_t>(L), 0.0);
						gnDw_.assign(static_cast<size_t>(L) * static_cast<size_t>(taps), 0.0);
						double wTmp[8], dwTmp[8];
						for (int j = 0; j < L; ++j)
						{
							const double p = (static_cast<double>(ls.origin + j) + 0.5) * ratio - 0.5 + phi0;
							const double fl = std::floor(p);
							const double t = p - fl;
							TapWeightDerivatives(cr.kernel, t, wTmp, dwTmp);
							double* drow = &gnDw_[static_cast<size_t>(j) * static_cast<size_t>(taps)];
							for (int s = 0; s < taps; ++s)
								drow[s] = dwTmp[s];
						}
						double gradAcc = 0.0;
						double hessAcc = 0.0;
						double rssAcc = 0.0;
						for (int i = 0; i < nl; ++i)
						{
							const double* yi = ls.Line(idxScratch_[static_cast<size_t>(i)]);
							op0->SolveLatent(yi, gnZ_);
							op0->ApplyForward(gnZ_.data(), gnPred_.data());
							for (int j = 0; j < L; ++j)
							{
								const double* drow = &gnDw_[static_cast<size_t>(j) * static_cast<size_t>(taps)];
								const int c = cl[j];
								double vj = 0.0;
								for (int s = 0; s < taps; ++s)
									vj += drow[s] * gnZ_[static_cast<size_t>(c + s)];
								gnV_[static_cast<size_t>(j)] = vj;
							}
							op0->SolveLatent(gnV_.data(), gnDz_);
							op0->ApplyForward(gnDz_.data(), gnProj_.data());
							for (int j = margin; j < L - margin; ++j)
							{
								const double rj = yi[j] - gnPred_[static_cast<size_t>(j)];
								const double drj = -(gnV_[static_cast<size_t>(j)] - gnProj_[static_cast<size_t>(j)]);
								gradAcc += rj * drj;
								hessAcc += drj * drj;
								rssAcc += rj * rj;
							}
						}
						// Strictly numerical ill-conditioning guard (Rule 1 & Rule 4).
						const double curvMin = 1e-9 * (std::max)(rssAcc, 1.0);
						if (!(hessAcc > curvMin) || !std::isfinite(gradAcc) || !std::isfinite(hessAcc))
						{
							if (gnIter == 0)
							{
								cr.phaseRefineFallback = true;
								++phaseRefineFallbacks_;
							}
							break;
						}
						const double dphi = ClampD(-gradAcc / (hessAcc + 1e-12), -0.25, 0.25);
						const double phi1 = ClampD(phi0 + dphi, phiInit - 0.25, phiInit + 0.25);
						if (gnIter > 0 && std::fabs(phi1 - phi0) < 1e-4)
							break;
						const SolveBatch* sb1 = SolveWindow(ls, 1, idxScratch_.data(), nl, 0, L, nStar, cr.kernel, phi1);
						if (sb1 == nullptr)
						{
							if (gnIter == 0)
							{
								cr.phaseRefineFallback = true;
								++phaseRefineFallbacks_;
							}
							break;
						}
						for (int i = 0; i < nl; ++i)
							valsScratch_[static_cast<size_t>(i)] = std::log(sb1->s[static_cast<size_t>(i)] / baseScratch_[static_cast<size_t>(i)]);
						const double t1 = FastTrimmedMeanSmall(valsScratch_.data(), nl, s_.trimFraction);
						if (std::isfinite(t1) && t1 < cr.stat)
						{
							cr.phase = static_cast<double>(PhaseQ(phi1)) / 1024.0;
							cr.stat = t1;
							cr.kernelT[cr.kernel] = t1;
							cr.kernelMargin = haveSecond ? second - t1 : 0.0;
							cr.familyMargin = haveOther ? otherFam - t1 : 0.0;
							cr.phaseRefined = true;
							cr.phaseRefineStep = cr.phase - phiInit;
							if (std::fabs(cr.phaseRefineStep) >= 0.25 - 1e-6)
								break;
						}
						else
						{
							if (gnIter == 0)
							{
								cr.phaseRefineFallback = true;
								++phaseRefineFallbacks_;
							}
							break;
						}
					}
				}
				cr.lineCount = nl;
				const int segCount = (std::min)(kMaxSegs, (L + kSegPx - 1) / kSegPx);
				cr.segCount = segCount;
				cr.lineStat.assign(static_cast<size_t>(nl), 0.0);
				cr.lineBase.assign(static_cast<size_t>(nl), 0.0);
				cr.lineRms.assign(static_cast<size_t>(nl), 0.0);
				const size_t cells = static_cast<size_t>(nl) * static_cast<size_t>(segCount);
				cr.segSum.assign(cells, 0.0);
				cr.segRows.assign(cells, 0);
				cr.segStat.assign(cells, 0.0);
				cr.segBase.assign(cells, 0.0);
				const double e2 = s_.epsCodes * s_.epsCodes;
				double refCol[4];
				const SolveBatch* fin = SolveWindow(ls, 1, idxScratch_.data(), nl, 0, L, nStar, cr.kernel, cr.phase);
				if (fin == nullptr)
					return kReasonShortLines;
				const double dofv = fin->dof;
				for (int i = 0; i < nl; ++i)
				{
					const double s = fin->s[static_cast<size_t>(i)];
					const double ms = fin->ms[static_cast<size_t>(i)];
					const double* segSum = &fin->segSum[static_cast<size_t>(i) * kMaxSegs];
					const int* segRows = fin->segRows.data();
					cr.lineStat[static_cast<size_t>(i)] = std::log(s / baseScratch_[static_cast<size_t>(i)]);
					cr.lineBase[static_cast<size_t>(i)] = baseScratch_[static_cast<size_t>(i)];
					cr.lineRms[static_cast<size_t>(i)] = std::sqrt(ms);
					for (int g = 0; g < segCount; ++g)
					{
						const size_t cell = static_cast<size_t>(i) * static_cast<size_t>(segCount) + static_cast<size_t>(g);
						cr.segSum[cell] = segSum[g];
						cr.segRows[cell] = segRows[g];
						if (segRows[g] <= 0)
							continue;
						int nv = 0;
						for (int r = 0; r < nref; ++r)
						{
							const double v = refSegScratch_[(static_cast<size_t>(r) * static_cast<size_t>(nl) + static_cast<size_t>(i)) * kMaxSegs + static_cast<size_t>(g)];
							if (v >= 0.0)
								refCol[nv++] = v;
						}
						if (nv < 1)
							continue;
						const double baseMs = FastMedianSmall(refCol, nv);
						const double levelMs = (segSum[g] / static_cast<double>(segRows[g]) + e2) / dofv;
						cr.segStat[cell] = 0.5 * std::log(levelMs / baseMs);
						cr.segBase[cell] = std::sqrt(baseMs);
					}
				}
				cr.floorCodes = FastTrimmedMeanSmall(cr.lineRms.data(), nl, s_.trimFraction);
				cr.baselineMedian = FastMedianSmall(cr.lineBase.data(), nl);
				if (s_.hdrNoise.enabled && s_.hdrNoise.transfer != HdrTransfer::Sdr && s_.hdrNoise.linearizeForDescale && !screenOnly)
				{
					// HIGH-4: evaluate the integer neighborhood (plus cr.nProposed in case code-space CoarseScan
					// drifted on high-contrast PQ/HLG edges) in linearized Gauss-Markov WLS space, and populate
					// per-line cr.lineStat / cr.lineBase / cr.lineRms + per-segment cr.segStat / cr.segBase so
					// ChainEvidence::Judge consumes the true linearized WLS residuals.
					std::vector<int> hdrNs = ns;
					if (cr.nProposed >= s_.minLatent && cr.nProposed <= maxN &&
						std::find(hdrNs.begin(), hdrNs.end(), cr.nProposed) == hdrNs.end())
					{
						hdrNs.push_back(cr.nProposed);
					}
					bool haveHdr = false;
					int bestHdrN = cr.n;
					HdrVerifyResult bestHr;
					for (size_t hi = 0; hi < hdrNs.size(); ++hi)
					{
						const int candN = hdrNs[hi];
						const HdrVerifyResult hr = VerifyLinearizedHeteroscedastic(
							ls, candN, s_.hdrNoise, hdrWs_, idxScratch_.data(), nl);
						if (!hr.valid)
							continue;
						const bool better = !haveHdr || hr.stat < bestHr.stat ||
							(hr.stat == bestHr.stat && std::abs(candN - cr.nProposed) < std::abs(bestHdrN - cr.nProposed));
						if (better)
						{
							haveHdr = true;
							bestHdrN = candN;
							bestHr = hr;
						}
					}
					cr.hdrDarkToeClampedSamples = haveHdr ? bestHr.darkToeClampedSamples : 0;
					if (haveHdr)
					{
						cr.n = bestHdrN;
						cr.hdrLinearizedUsed = true;
						cr.hdrWhitenedStat = bestHr.stat;
						cr.hdrWhitenedRms = bestHr.whitenedRms;
						cr.stat = bestHr.stat;
						if (bestHr.bestKernel >= 0 && bestHr.bestKernel < kKernelCount)
						{
							cr.kernel = bestHr.bestKernel;
							cr.family = static_cast<int>(Kernel(cr.kernel).family);
						}
						// Map dimensionless whitened SNR units (1.0 = quantization floor) back to 10-bit code floor
						// scale (1 / sqrt(12)) so ChainEvidence::Judge's lineGateFactor * floorC gate stays calibrated.
						const double unitFloor10 = 1.0 / std::sqrt(12.0);
						if (static_cast<int>(bestHr.lineStat.size()) == nl &&
							static_cast<int>(bestHr.lineBaseWhitened.size()) == nl &&
							static_cast<int>(bestHr.lineRmsWhitened.size()) == nl)
						{
							for (int i = 0; i < nl; ++i)
							{
								cr.lineStat[static_cast<size_t>(i)] = bestHr.lineStat[static_cast<size_t>(i)];
								cr.lineBase[static_cast<size_t>(i)] = bestHr.lineBaseWhitened[static_cast<size_t>(i)] * unitFloor10;
								cr.lineRms[static_cast<size_t>(i)] = bestHr.lineRmsWhitened[static_cast<size_t>(i)] * unitFloor10;
							}
							cr.floorCodes = FastTrimmedMeanSmall(cr.lineRms.data(), nl, s_.trimFraction);
							cr.baselineMedian = FastMedianSmall(cr.lineBase.data(), nl);
						}
						if (bestHr.segCount == segCount &&
							bestHr.segStat.size() == cells &&
							bestHr.segBaseWhitened.size() == cells &&
							bestHr.segRows.size() == cells)
						{
							for (size_t cell = 0; cell < cells; ++cell)
							{
								cr.segStat[cell] = bestHr.segStat[cell];
								cr.segBase[cell] = bestHr.segBaseWhitened[cell] * unitFloor10;
								cr.segRows[cell] = bestHr.segRows[cell];
							}
						}
					}
				}
				cr.exactDone = true;
				return 0u;
			}

			VerifierSettings s_;
			std::map<OpKey, CacheEntry> cache_;
			std::list<OpKey> lru_;               // least recently used first
			size_t cacheBytes_ = 0;              // sum of the cached operators' Bytes()
			TapMemo taps_;                       // tap weights shared by every operator this verifier builds (a pure memo; <= cacheBytes / 16, see TapEntriesFor)
			size_t memoBytes_ = 0;               // estimated heap bytes of memo_
			std::map<SolveKey, SolveBatch> memo_;   // the solves of the current Evaluate call
			std::vector<double> scratch_;
			// SID-OPT: reusable working scratch buffers (never affect CachedBytes / Evict budget).
			std::vector<double> perRefScratch_;
			std::vector<double> baseScratch_;
			std::vector<double> refSegScratch_;
			std::vector<double> scanSScratch_;
			std::vector<double> valsScratch_;
			std::vector<int> idxScratch_;
			HdrNoiseWorkspace hdrWs_;
			std::vector<double> gnZ_;
			std::vector<double> gnV_;
			std::vector<double> gnDz_;
			std::vector<double> gnPred_;
			std::vector<double> gnProj_;
			std::vector<double> gnDw_;
			int builds_ = 0;
			int solves_ = 0;
			int reused_ = 0;
			int phaseRefineFallbacks_ = 0;
		};
	}
}
#pragma fp_contract(on)
