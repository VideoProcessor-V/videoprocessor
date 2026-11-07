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
// Operators are cached by (N, n, kernel, phase step, window); the cache only saves time, never changes a result.
// Degrees of freedom: s uses 1 - nodes/rows of the operator (the unknowns of the fit), not 1 - n/N, so short
// windows are not biased. Give the PowerProbe its own verifier instance (its 1.64x longer operators would
// otherwise push the candidates' operators out of the cache).
// Not thread safe (one instance per worker). No clock inside: the caller times it.
#include <cstdlib>
#include <map>
#include <memory>

#include "KernelBank.h"

namespace SourceIdentity
{
	namespace Chain
	{
		struct VerifierSettings
		{
			int coarseHalfWidth = 4;            // integers scanned each side of a candidate by the coarse stage
			int coarseStep = 1;                 // step of that scan (the notch of a short window is a few integers wide)
			int coarseLines = 16;               // lines used by the coarse stage
			double coarseWindowFraction = 0.125;   // coarse window = this fraction of N ...
			int coarseMinWindow = 160;          // ... but at least this many rows
			int exactHalfWidth = 1;             // integers scanned each side by the exact stage
			int maxExactCandidates = 2;         // candidates promoted to the exact stage
			int exactFamilies = 2;              // kernel families evaluated in full at the best integer
			double refFraction1 = 0.04;         // baseline sizes n (1 +- f1), n (1 +- f2)
			double refFraction2 = 0.08;
			double trimFraction = 0.2;
			double epsCodes = 0.02;             // numerical floor added (in quadrature) to a line's rms
			int minLatent = 16;
			double maxLatentRatio = 0.985;      // n / N above this is not an upscale
			int phaseCount = 1;                 // odd; sub-sample phases tried at the best integer
			double phaseStep = 0.25;            // latent samples between phases
			size_t cacheLimit = 512;            // operators kept (about 70 MB at 3840x2160 with 3 candidates per axis; time only)
		};

		struct VerifyOptions
		{
			bool exactOnly = false;     // skip the coarse stage (n is known exactly, e.g. the power probe)
			int onlyKernel = -1;        // >= 0: evaluate only this kernel
			int exactHalfWidth = -1;    // >= 0 overrides the settings value
			int forceExactN = 0;        // > 0: the candidate asked for with exactly this size always goes to the exact stage
			                            // (the caller's latched size), on top of the best maxExactCandidates
			int exactLines = 0;         // > 0: the exact stage uses only this many (evenly spread) lines, e.g. 16 when latched
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
		};

		struct VerifyResult
		{
			uint32_t reason = 0;
			std::vector<CandidateResult> candidates;
		};

		class DescaleVerifier
		{
		public:
			explicit DescaleVerifier(const VerifierSettings& s = VerifierSettings()) : s_(s) {}

			const VerifierSettings& Settings() const { return s_; }
			void ClearCache() { cache_.clear(); order_.clear(); }
			size_t CachedOperators() const { return cache_.size(); }

			bool Evaluate(const LineSet& ls, const int* cand, int count, const VerifyOptions& opt, VerifyResult& out)
			{
				out.reason = 0;
				out.candidates.clear();
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
				const int promote = opt.exactOnly ? static_cast<int>(order.size())
					: std::min<int>(static_cast<int>(order.size()), s_.maxExactCandidates);
				for (int i = 0; i < static_cast<int>(order.size()); ++i)
				{
					CandidateResult& cr = out.candidates[static_cast<size_t>(order[static_cast<size_t>(i)])];
					const bool forced = opt.forceExactN > 0 && cr.nProposed == opt.forceExactN;
					if (i < promote || forced)
					{
						const uint32_t why = ExactStage(ls, cr.n, opt, cr);
						if (why != 0u)
						{
							cr.valid = false;
							cr.reason |= why;
						}
					}
					else
						cr.reason |= kReasonCoarseOnly;
				}
				for (int c = 0; c < count; ++c)
					out.reason |= out.candidates[static_cast<size_t>(c)].reason;
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

			const DescaleOperator* Op(const OpKey& k, double phase)
			{
				auto it = cache_.find(k);
				if (it != cache_.end())
					return it->second->Ok() ? it->second.get() : nullptr;
				std::unique_ptr<DescaleOperator> op(new DescaleOperator());
				op->Build(k.N, k.n, k.kernel, phase, k.origin, k.length);
				const DescaleOperator* raw = op.get();
				cache_[k] = std::move(op);
				order_.push_back(k);
				const size_t limit = s_.cacheLimit < 1 ? 1 : s_.cacheLimit;
				while (order_.size() > limit)
				{
					cache_.erase(order_.front());
					order_.erase(order_.begin());
				}
				return raw->Ok() ? raw : nullptr;
			}

			static int PhaseQ(double phase) { return static_cast<int>(std::floor(phase * 1024.0 + 0.5)); }

			// s of one window of one line at (n, kernel, phase). false when the operator is unusable.
			bool SolveLine(const LineSet& ls, int line, int winOff, int winLen, int n, int kernel, double phase,
				double& s, double& ms, double& dof, double* segSum, int* segRows)
			{
				OpKey key;
				key.N = ls.axisLen;
				key.n = n;
				key.kernel = kernel;
				key.phaseQ = PhaseQ(phase);
				key.origin = ls.origin + winOff;
				key.length = winLen;
				const DescaleOperator* op = Op(key, static_cast<double>(key.phaseQ) / 1024.0);
				if (op == nullptr)
					return false;
				double rss = 0.0;
				int rows = 0;
				op->Residual(ls.Line(line) + winOff, scratch_, rss, rows, segSum, segRows);
				if (rows <= 0)
					return false;
				ms = rss / static_cast<double>(rows);
				dof = 1.0 - static_cast<double>(op->Nodes()) / static_cast<double>(op->Length());   // rows minus the fit unknowns
				if (dof < 0.02)
					dof = 0.02;
				s = std::sqrt((ms + s_.epsCodes * s_.epsCodes) / dof);
				return true;
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

			// Per-line baseline of one window geometry: the median over the reference sizes of the smallest s over `reps`.
			// refSeg (optional, nref x nl x kMaxSegs) receives the segment level of the
			// winning kernel at every reference ((mean square per row + eps^2) / dof, as the line level), -1 where the segment has no rows.
			bool Baseline(const LineSet& ls, const int* lineIdx, int nl, int winOff, int winLen, int n0, const int* reps, int nrep,
				double* base, std::vector<double>* refSeg, int& nref)
			{
				int refs[4];
				nref = RefSizes(n0, ls.axisLen, refs);
				if (nref < 2)
					return false;
				std::vector<double> perRef(static_cast<size_t>(nref) * static_cast<size_t>(nl), 0.0);
				if (refSeg != nullptr)
					refSeg->assign(static_cast<size_t>(nref) * static_cast<size_t>(nl) * static_cast<size_t>(kMaxSegs), -1.0);
				double segSum[kMaxSegs];
				int segRows[kMaxSegs];
				double bestSeg[kMaxSegs];
				int bestRows[kMaxSegs];
				for (int r = 0; r < nref; ++r)
				{
					for (int i = 0; i < nl; ++i)
					{
						double best = 0.0;
						double bestDof = 1.0;
						bool have = false;
						for (int q = 0; q < nrep; ++q)
						{
							double s = 0.0;
							double ms = 0.0;
							double dofv = 0.0;
							if (!SolveLine(ls, lineIdx[i], winOff, winLen, refs[r], reps[q], 0.0, s, ms, dofv, segSum, segRows))
								return false;
							if (!have || s < best)
							{
								best = s;
								bestDof = dofv;
								have = true;
								for (int g = 0; g < kMaxSegs; ++g)
								{
									bestSeg[g] = segSum[g];
									bestRows[g] = segRows[g];
								}
							}
						}
						perRef[static_cast<size_t>(r) * static_cast<size_t>(nl) + static_cast<size_t>(i)] = best;
						if (refSeg != nullptr)
							for (int g = 0; g < kMaxSegs; ++g)
								if (bestRows[g] > 0)
									(*refSeg)[(static_cast<size_t>(r) * static_cast<size_t>(nl) + static_cast<size_t>(i)) * kMaxSegs + static_cast<size_t>(g)]
										= (bestSeg[g] / static_cast<double>(bestRows[g]) + s_.epsCodes * s_.epsCodes) / bestDof;
					}
				}
				std::vector<double> col(static_cast<size_t>(nref));
				for (int i = 0; i < nl; ++i)
				{
					for (int r = 0; r < nref; ++r)
						col[static_cast<size_t>(r)] = perRef[static_cast<size_t>(r) * static_cast<size_t>(nl) + static_cast<size_t>(i)];
					base[i] = MedianOf(col.data(), nref);
				}
				return true;
			}

			uint32_t CoarseScan(const LineSet& ls, int nC, int& nBest, double& tBest)
			{
				const int N = ls.axisLen;
				const int L = ls.length;
				int Lc = std::max(s_.coarseMinWindow, static_cast<int>(std::floor(s_.coarseWindowFraction * static_cast<double>(N))));
				if (Lc > L)
					Lc = L;
				const int nWin = (Lc >= L) ? 1 : 2;
				const int winOff[2] = { 0, L - Lc };
				const int nl = std::min(ls.count, std::max(1, s_.coarseLines));
				std::vector<int> idx(static_cast<size_t>(nl));
				for (int i = 0; i < nl; ++i)
					idx[static_cast<size_t>(i)] = (i * ls.count) / nl;
				int nrep = 0;
				const int* reps = RepresentativeKernels(nrep);
				std::vector<double> base(static_cast<size_t>(nWin) * static_cast<size_t>(nl), 0.0);
				for (int w = 0; w < nWin; ++w)
				{
					int nref = 0;
					if (!Baseline(ls, idx.data(), nl, winOff[w], Lc, nC, reps, nrep, &base[static_cast<size_t>(w) * static_cast<size_t>(nl)], nullptr, nref))
						return nref < 2 ? kReasonNoReference : kReasonShortLines;
				}
				const int maxN = static_cast<int>(std::floor(s_.maxLatentRatio * static_cast<double>(N)));
				const int step = s_.coarseStep < 1 ? 1 : s_.coarseStep;
				std::vector<double> vals(static_cast<size_t>(nWin) * static_cast<size_t>(nl), 0.0);
				bool have = false;
				double segSum[kMaxSegs];
				int segRows[kMaxSegs];
				for (int d = -s_.coarseHalfWidth; d <= s_.coarseHalfWidth; d += step)
				{
					const int n = nC + d;
					if (n < s_.minLatent || n > maxN)
						continue;
					bool ok = true;
					for (int w = 0; w < nWin && ok; ++w)
						for (int i = 0; i < nl; ++i)
						{
							double best = 0.0;
							bool h2 = false;
							for (int q = 0; q < nrep; ++q)
							{
								double s = 0.0;
								double ms = 0.0;
								double dofv = 0.0;
								if (!SolveLine(ls, idx[static_cast<size_t>(i)], winOff[w], Lc, n, reps[q], 0.0, s, ms, dofv, segSum, segRows))
								{
									ok = false;
									break;
								}
								if (!h2 || s < best)
								{
									best = s;
									h2 = true;
								}
							}
							if (!ok)
								break;
							vals[static_cast<size_t>(w) * static_cast<size_t>(nl) + static_cast<size_t>(i)]
								= std::log(best / base[static_cast<size_t>(w) * static_cast<size_t>(nl) + static_cast<size_t>(i)]);
						}
					if (!ok)
						continue;
					const double T = TrimmedMeanOf(vals.data(), nWin * nl, s_.trimFraction);
					const bool better = !have || T < tBest || (T == tBest && std::abs(d) < std::abs(nBest - nC));
					if (better)
					{
						have = true;
						tBest = T;
						nBest = n;
					}
				}
				return have ? 0u : kReasonShortLines;
			}

			uint32_t ExactStage(const LineSet& ls, int n0, const VerifyOptions& opt, CandidateResult& cr)
			{
				const int N = ls.axisLen;
				const int L = ls.length;
				const int avail = std::min(ls.count, kMaxLines);
				const int nl = (opt.exactLines > 0 && opt.exactLines < avail) ? opt.exactLines : avail;
				std::vector<int> idx(static_cast<size_t>(nl));
				for (int i = 0; i < nl; ++i)
					idx[static_cast<size_t>(i)] = (i * avail) / nl;
				int repAll = 0;
				const int* repsAll = RepresentativeKernels(repAll);
				int repOne[1] = { opt.onlyKernel };
				const int* reps = opt.onlyKernel >= 0 ? repOne : repsAll;
				const int nrep = opt.onlyKernel >= 0 ? 1 : repAll;
				std::vector<double> base(static_cast<size_t>(nl), 0.0);
				std::vector<double> refSeg;
				int nref = 0;
				if (!Baseline(ls, idx.data(), nl, 0, L, n0, reps, nrep, base.data(), &refSeg, nref))
					return nref < 2 ? kReasonNoReference : kReasonShortLines;

				// Integer scan with the representative kernels.
				const int maxN = static_cast<int>(std::floor(s_.maxLatentRatio * static_cast<double>(N)));
				const int hw = opt.exactHalfWidth >= 0 ? opt.exactHalfWidth : s_.exactHalfWidth;
				std::vector<int> ns;
				for (int n = n0 - hw; n <= n0 + hw; ++n)
					if (n >= s_.minLatent && n <= maxN)
						ns.push_back(n);
				if (ns.empty())
					return kReasonBadInput;
				const size_t stride = static_cast<size_t>(nrep) * static_cast<size_t>(nl);
				std::vector<double> scanS(ns.size() * stride, 0.0);
				std::vector<double> vals(static_cast<size_t>(nl), 0.0);
				double segSum[kMaxSegs];
				int segRows[kMaxSegs];
				int bestNi = -1;
				double bestT = 0.0;
				for (size_t ni = 0; ni < ns.size(); ++ni)
				{
					for (int i = 0; i < nl; ++i)
					{
						double best = 0.0;
						for (int q = 0; q < nrep; ++q)
						{
							double s = 0.0;
							double ms = 0.0;
							double dofv = 0.0;
							if (!SolveLine(ls, idx[static_cast<size_t>(i)], 0, L, ns[ni], reps[q], 0.0, s, ms, dofv, segSum, segRows))
								return kReasonShortLines;
							scanS[ni * stride + static_cast<size_t>(q) * static_cast<size_t>(nl) + static_cast<size_t>(i)] = s;
							if (q == 0 || s < best)
								best = s;
						}
						vals[static_cast<size_t>(i)] = std::log(best / base[static_cast<size_t>(i)]);
					}
					const double T = TrimmedMeanOf(vals.data(), nl, s_.trimFraction);
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

				// Kernels to evaluate in full at nStar: every kernel of the best families (or the one asked for).
				std::vector<int> kernels;
				if (opt.onlyKernel >= 0)
					kernels.push_back(opt.onlyKernel);
				else
				{
					double famT[kFamilyCount];
					int famOrder[kFamilyCount];
					for (int q = 0; q < nrep; ++q)
					{
						for (int i = 0; i < nl; ++i)
							vals[static_cast<size_t>(i)] = std::log(scanS[static_cast<size_t>(bestNi) * stride + static_cast<size_t>(q) * static_cast<size_t>(nl) + static_cast<size_t>(i)] / base[static_cast<size_t>(i)]);
						famT[static_cast<int>(Kernel(reps[q]).family)] = TrimmedMeanOf(vals.data(), nl, s_.trimFraction);
					}
					for (int f = 0; f < kFamilyCount; ++f)
						famOrder[f] = f;
					std::sort(famOrder, famOrder + kFamilyCount, [&](int a, int b) {
						if (famT[a] != famT[b])
							return famT[a] < famT[b];
						return a < b;
					});
					const int nf = std::max(1, std::min(s_.exactFamilies, kFamilyCount));
					for (int k = 0; k < kKernelCount; ++k)
						for (int f = 0; f < nf; ++f)
							if (static_cast<int>(Kernel(k).family) == famOrder[f])
							{
								kernels.push_back(k);
								break;
							}
				}
				std::vector<double> phases;
				phases.push_back(0.0);
				for (int q = 1; q <= (s_.phaseCount - 1) / 2; ++q)
				{
					phases.push_back(static_cast<double>(q) * s_.phaseStep);
					phases.push_back(-static_cast<double>(q) * s_.phaseStep);
				}

				struct KernelScore { int kernel; double phase; double T; };
				std::vector<KernelScore> scores;
				for (size_t ki = 0; ki < kernels.size(); ++ki)
				{
					KernelScore best;
					best.kernel = kernels[ki];
					best.phase = 0.0;
					best.T = 0.0;
					bool have = false;
					for (size_t pi = 0; pi < phases.size(); ++pi)
					{
						for (int i = 0; i < nl; ++i)
						{
							double s = 0.0;
							double ms = 0.0;
							double dofv = 0.0;
							// Reuse the scan value when it is the same operator (phase 0, a representative kernel).
							bool reused = false;
							if (pi == 0)
								for (int q = 0; q < nrep; ++q)
									if (reps[q] == kernels[ki])
									{
										s = scanS[static_cast<size_t>(bestNi) * stride + static_cast<size_t>(q) * static_cast<size_t>(nl) + static_cast<size_t>(i)];
										reused = true;
										break;
									}
							if (!reused && !SolveLine(ls, idx[static_cast<size_t>(i)], 0, L, nStar, kernels[ki], phases[pi], s, ms, dofv, segSum, segRows))
								return kReasonShortLines;
							vals[static_cast<size_t>(i)] = std::log(s / base[static_cast<size_t>(i)]);
						}
						const double T = TrimmedMeanOf(vals.data(), nl, s_.trimFraction);
						if (!have || T < best.T)
						{
							have = true;
							best.T = T;
							best.phase = phases[pi];
						}
					}
					scores.push_back(best);
				}
				size_t bi = 0;
				for (size_t k = 1; k < scores.size(); ++k)
					if (scores[k].T < scores[bi].T)
						bi = k;
				double second = 0.0;
				bool haveSecond = false;
				double otherFam = 0.0;
				bool haveOther = false;
				for (size_t k = 0; k < scores.size(); ++k)
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

				// Final pass at the winner with segment sums.
				cr.n = nStar;
				cr.kernel = scores[bi].kernel;
				cr.family = static_cast<int>(Kernel(cr.kernel).family);
				cr.phase = scores[bi].phase;
				cr.stat = scores[bi].T;
				cr.kernelMargin = haveSecond ? second - scores[bi].T : 0.0;
				cr.familyMargin = haveOther ? otherFam - scores[bi].T : 0.0;
				cr.lineCount = nl;
				const int segCount = std::min(kMaxSegs, (L + kSegPx - 1) / kSegPx);
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
				std::vector<double> refCol(static_cast<size_t>(nref));
				for (int i = 0; i < nl; ++i)
				{
					double s = 0.0;
					double ms = 0.0;
					double dofv = 0.0;
					if (!SolveLine(ls, idx[static_cast<size_t>(i)], 0, L, nStar, cr.kernel, cr.phase, s, ms, dofv, segSum, segRows))
						return kReasonShortLines;
					cr.lineStat[static_cast<size_t>(i)] = std::log(s / base[static_cast<size_t>(i)]);
					cr.lineBase[static_cast<size_t>(i)] = base[static_cast<size_t>(i)];
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
							const double v = refSeg[(static_cast<size_t>(r) * static_cast<size_t>(nl) + static_cast<size_t>(i)) * kMaxSegs + static_cast<size_t>(g)];
							if (v >= 0.0)
								refCol[static_cast<size_t>(nv++)] = v;
						}
						if (nv < 1)
							continue;
						const double baseMs = MedianOf(refCol.data(), nv);
						const double levelMs = (segSum[g] / static_cast<double>(segRows[g]) + e2) / dofv;
						cr.segStat[cell] = 0.5 * std::log(levelMs / baseMs);
						cr.segBase[cell] = std::sqrt(baseMs);
					}
				}
				cr.floorCodes = TrimmedMeanOf(cr.lineRms.data(), nl, s_.trimFraction);
				cr.baselineMedian = MedianOf(cr.lineBase.data(), nl);
				cr.exactDone = true;
				return 0u;
			}

			VerifierSettings s_;
			std::map<OpKey, std::unique_ptr<DescaleOperator>> cache_;
			std::vector<OpKey> order_;
			std::vector<double> scratch_;
		};
	}
}
