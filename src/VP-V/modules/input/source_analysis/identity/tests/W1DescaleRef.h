// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// Test-only genuine verbatim W1 reference snapshot (commit bfd055d1f) for Opus MED-2 byte-identity verification:
//   - W1DescaleRef::DescaleVerifierW1: verbatim bfd055d1f SourceIdentity::Chain::DescaleVerifier
//     (uses RobustStats::MedianOf / TrimmedMeanOf, per-call local buffers; NO FastMedianSmall,
//      NO FastTrimmedMeanSmall, NO SID-OPT scratch hoisting, NO p3TwoPass).
//   - W1DescaleRef::PowerProbeW1: verbatim bfd055d1f SourceIdentity::Chain::PowerProbe
//     (NO PowerProbe::Workspace caching).
//
// Pure C++14, header-only, /W4 clean, zero name collisions with SourceIdentity::Chain::DescaleVerifier or PowerProbe.
#include <VP-V/core/RobustStats.h>
#include <VP-V/modules/input/source_analysis/identity/chain/DescaleVerifier.h>
#include <VP-V/modules/input/source_analysis/identity/chain/PowerProbe.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <list>
#include <map>
#include <memory>
#include <vector>

// No contraction (fused multiply-add) in the descale arithmetic: matches bfd055d1f DescaleVerifier.h exactly.
#pragma fp_contract(off)

namespace W1DescaleRef
{
	using SourceIdentity::Chain::CandidateResult;
	using SourceIdentity::Chain::DescaleOperator;
	using SourceIdentity::Chain::Kernel;
	using SourceIdentity::Chain::KernelInfo;
	using SourceIdentity::Chain::LineSet;
	using SourceIdentity::Chain::ProbeResult;
	using SourceIdentity::Chain::ProbeSettings;
	using SourceIdentity::Chain::RepresentativeKernels;
	using SourceIdentity::Chain::RoundToInt;
	using SourceIdentity::Chain::TapMemo;
	using SourceIdentity::Chain::TapWeights;
	using SourceIdentity::Chain::VerifierSettings;
	using SourceIdentity::Chain::VerifyOptions;
	using SourceIdentity::Chain::VerifyResult;
	using SourceIdentity::Chain::kFamilyCount;
	using SourceIdentity::Chain::kKernelCount;
	using SourceIdentity::Chain::kMaxLines;
	using SourceIdentity::Chain::kMaxSegs;
	using SourceIdentity::Chain::kReasonBadInput;
	using SourceIdentity::Chain::kReasonCoarseOnly;
	using SourceIdentity::Chain::kReasonDark;
	using SourceIdentity::Chain::kReasonFlat;
	using SourceIdentity::Chain::kReasonNoCandidates;
	using SourceIdentity::Chain::kReasonNoPower;
	using SourceIdentity::Chain::kReasonNoReference;
	using SourceIdentity::Chain::kReasonShortLines;
	using SourceIdentity::Chain::kReasonSoft;
	using SourceIdentity::Chain::kResidualBatch;
	using SourceIdentity::Chain::kSegPx;

	// Byte-for-byte scalar comparison helper (strict std::memcmp == 0; catches even a 1-ULP or -0.0/+0.0 difference).
	template <typename T>
	inline bool MemEq(const T& a, const T& b)
	{
		return std::memcmp(&a, &b, sizeof(T)) == 0;
	}

	// Verbatim bfd055d1f DescaleVerifier (git show bfd055d1f:src/VP-V/modules/input/source_analysis/identity/chain/DescaleVerifier.h).
	class DescaleVerifierW1
	{
	public:
		explicit DescaleVerifierW1(const VerifierSettings& s = VerifierSettings()) : s_(s), taps_(TapEntriesFor(s.cacheBytes)) {}

		const VerifierSettings& Settings() const { return s_; }
		void ClearCache() { cache_.clear(); lru_.clear(); cacheBytes_ = 0; memo_.clear(); memoBytes_ = 0; }
		size_t CachedOperators() const { return cache_.size(); }
		size_t CachedBytes() const { return cacheBytes_ + taps_.Bytes() + memoBytes_; }

		bool Evaluate(const LineSet& ls, const int* cand, int count, const VerifyOptions& opt, VerifyResult& out)
		{
			out.reason = 0;
			out.costMs = 0.0;
			out.cold = false;
			out.operatorBuilds = 0;
			out.solves = 0;
			out.solvesReused = 0;
			out.candidates.clear();
			const double clock0 = s_.nowMs != nullptr ? s_.nowMs(s_.clockCtx) : 0.0;
			const int builds0 = builds_;
			const int solves0 = solves_;
			const int reused0 = reused_;
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
			out.operatorBuilds = builds_ - builds0;
			out.solves = solves_ - solves0;
			out.solvesReused = reused_ - reused0;
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
			while (cache_.size() > 1 && (cache_.size() > limit || (s_.cacheBytes > 0 && cacheBytes_ + taps_.Bytes() + memoBytes_ > s_.cacheBytes)))
			{
				auto victim = cache_.find(lru_.front());
				cacheBytes_ -= victim->second.bytes;
				cache_.erase(victim);
				lru_.pop_front();
			}
		}

		static int PhaseQ(double phase) { return static_cast<int>(std::floor(phase * 1024.0 + 0.5)); }

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
				+ sizeof(SolveKey) + sizeof(SolveBatch) + 48;
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

		bool Baseline(const LineSet& ls, int stage, const int* lineIdx, int nl, int winOff, int winLen, int n0, const int* reps, int nrep,
			double* base, std::vector<double>* refSeg, int& nref)
		{
			int refs[4];
			nref = RefSizes(n0, ls.axisLen, refs);
			if (nref < 2)
				return false;
			std::vector<double> perRef(static_cast<size_t>(nref) * static_cast<size_t>(nl), 0.0);
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
					perRef[static_cast<size_t>(r) * static_cast<size_t>(nl) + static_cast<size_t>(i)] = best;
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
			std::vector<double> col(static_cast<size_t>(nref));
			for (int i = 0; i < nl; ++i)
			{
				for (int r = 0; r < nref; ++r)
					col[static_cast<size_t>(r)] = perRef[static_cast<size_t>(r) * static_cast<size_t>(nl) + static_cast<size_t>(i)];
				base[i] = RobustStats::MedianOf(col.data(), nref);
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
			std::vector<int> idx(static_cast<size_t>(nl));
			for (int i = 0; i < nl; ++i)
				idx[static_cast<size_t>(i)] = (i * ls.count) / nl;
			int nrep = 0;
			const int* reps = RepresentativeKernels(nrep);
			std::vector<double> base(static_cast<size_t>(nWin) * static_cast<size_t>(nl), 0.0);
			for (int w = 0; w < nWin; ++w)
			{
				int nref = 0;
				if (!Baseline(ls, 0, idx.data(), nl, winOff[w], Lc, nC, reps, nrep, &base[static_cast<size_t>(w) * static_cast<size_t>(nl)], nullptr, nref))
					return nref < 2 ? kReasonNoReference : kReasonShortLines;
			}
			const int maxN = static_cast<int>(std::floor(s_.maxLatentRatio * static_cast<double>(N)));
			const int step = s_.coarseStep < 1 ? 1 : s_.coarseStep;
			std::vector<double> vals(static_cast<size_t>(nWin) * static_cast<size_t>(nl), 0.0);
			bool have = false;
			const SolveBatch* bw[2][kFamilyCount];
			auto visit = [&](int d) {
				const int n = nC + d;
				if (n < s_.minLatent || n > maxN)
					return;
				bool ok = true;
				for (int w = 0; w < nWin && ok; ++w)
					for (int q = 0; q < nrep; ++q)
					{
						bw[w][q] = SolveWindow(ls, 0, idx.data(), nl, winOff[w], Lc, n, reps[q], 0.0);
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
						vals[static_cast<size_t>(w) * static_cast<size_t>(nl) + static_cast<size_t>(i)]
							= std::log(best / base[static_cast<size_t>(w) * static_cast<size_t>(nl) + static_cast<size_t>(i)]);
					}
				const double T = RobustStats::TrimmedMeanOf(vals.data(), nWin * nl, s_.trimFraction);
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
				const int bestD = nBest - nC;
				for (int d = bestD - (step - 1); d <= bestD + (step - 1); ++d)
					if (d >= -s_.coarseHalfWidth && d <= s_.coarseHalfWidth && ((d + s_.coarseHalfWidth) % step) != 0)
						visit(d);
			}
			return have ? 0u : kReasonShortLines;
		}

		uint32_t ExactStage(const LineSet& ls, int n0, const VerifyOptions& opt, CandidateResult& cr)
		{
			const int N = ls.axisLen;
			const int L = ls.length;
			const int avail = (std::min)(ls.count, kMaxLines);
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
			if (!Baseline(ls, 1, idx.data(), nl, 0, L, n0, reps, nrep, base.data(), &refSeg, nref))
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
			int bestNi = -1;
			double bestT = 0.0;
			for (size_t ni = 0; ni < ns.size(); ++ni)
			{
				for (int q = 0; q < nrep; ++q)
				{
					const SolveBatch* sb = SolveWindow(ls, 1, idx.data(), nl, 0, L, ns[ni], reps[q], 0.0);
					if (sb == nullptr)
						return kReasonShortLines;
					for (int i = 0; i < nl; ++i)
						scanS[ni * stride + static_cast<size_t>(q) * static_cast<size_t>(nl) + static_cast<size_t>(i)] = sb->s[static_cast<size_t>(i)];
				}
				for (int i = 0; i < nl; ++i)
				{
					double best = 0.0;
					for (int q = 0; q < nrep; ++q)
					{
						const double s = scanS[ni * stride + static_cast<size_t>(q) * static_cast<size_t>(nl) + static_cast<size_t>(i)];
						if (q == 0 || s < best)
							best = s;
					}
					vals[static_cast<size_t>(i)] = std::log(best / base[static_cast<size_t>(i)]);
				}
				const double T = RobustStats::TrimmedMeanOf(vals.data(), nl, s_.trimFraction);
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
					famT[static_cast<int>(Kernel(reps[q]).family)] = RobustStats::TrimmedMeanOf(vals.data(), nl, s_.trimFraction);
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
					int reusedQ = -1;
					if (pi == 0)
						for (int q = 0; q < nrep; ++q)
							if (reps[q] == kernels[ki])
							{
								reusedQ = q;
								break;
							}
					const SolveBatch* sb = nullptr;
					if (reusedQ < 0)
					{
						sb = SolveWindow(ls, 1, idx.data(), nl, 0, L, nStar, kernels[ki], phases[pi]);
						if (sb == nullptr)
							return kReasonShortLines;
					}
					for (int i = 0; i < nl; ++i)
					{
						const double s = reusedQ >= 0
							? scanS[static_cast<size_t>(bestNi) * stride + static_cast<size_t>(reusedQ) * static_cast<size_t>(nl) + static_cast<size_t>(i)]
							: sb->s[static_cast<size_t>(i)];
						vals[static_cast<size_t>(i)] = std::log(s / base[static_cast<size_t>(i)]);
					}
					const double T = RobustStats::TrimmedMeanOf(vals.data(), nl, s_.trimFraction);
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
			cr.kernelMask = 0;
			for (size_t k = 0; k < scores.size(); ++k)
			{
				cr.kernelMask |= 1u << scores[k].kernel;
				cr.kernelT[scores[k].kernel] = scores[k].T;
			}
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
			std::vector<double> refCol(static_cast<size_t>(nref));
			const SolveBatch* fin = SolveWindow(ls, 1, idx.data(), nl, 0, L, nStar, cr.kernel, cr.phase);
			if (fin == nullptr)
				return kReasonShortLines;
			const double dofv = fin->dof;
			for (int i = 0; i < nl; ++i)
			{
				const double s = fin->s[static_cast<size_t>(i)];
				const double ms = fin->ms[static_cast<size_t>(i)];
				const double* segSum = &fin->segSum[static_cast<size_t>(i) * kMaxSegs];
				const int* segRows = fin->segRows.data();
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
					const double baseMs = RobustStats::MedianOf(refCol.data(), nv);
					const double levelMs = (segSum[g] / static_cast<double>(segRows[g]) + e2) / dofv;
					cr.segStat[cell] = 0.5 * std::log(levelMs / baseMs);
					cr.segBase[cell] = std::sqrt(baseMs);
				}
			}
			cr.floorCodes = RobustStats::TrimmedMeanOf(cr.lineRms.data(), nl, s_.trimFraction);
			cr.baselineMedian = RobustStats::MedianOf(cr.lineBase.data(), nl);
			cr.exactDone = true;
			return 0u;
		}

		VerifierSettings s_;
		std::map<OpKey, CacheEntry> cache_;
		std::list<OpKey> lru_;
		size_t cacheBytes_ = 0;
		TapMemo taps_;
		size_t memoBytes_ = 0;
		std::map<SolveKey, SolveBatch> memo_;
		std::vector<double> scratch_;
		int builds_ = 0;
		int solves_ = 0;
		int reused_ = 0;
	};
}

#pragma fp_contract(on)

namespace W1DescaleRef
{
	// Verbatim bfd055d1f PowerProbe (git show bfd055d1f:src/VP-V/modules/input/source_analysis/identity/chain/PowerProbe.h).
	class PowerProbeW1
	{
	public:
		static bool Run(DescaleVerifierW1& verifier, const LineSet& ls, double floorCodes, double roundStepCodes,
			const ProbeSettings& ps, ProbeResult& out)
		{
			const VerifierSettings& vs = verifier.Settings();
			const double t0 = vs.nowMs != nullptr ? vs.nowMs(vs.clockCtx) : 0.0;
			const bool ok = RunImpl(verifier, ls, floorCodes, roundStepCodes, ps, out);
			out.costMs = vs.nowMs != nullptr ? vs.nowMs(vs.clockCtx) - t0 : 0.0;
			return ok;
		}

	private:
		static bool RunImpl(DescaleVerifierW1& verifier, const LineSet& ls, double floorCodes, double roundStepCodes,
			const ProbeSettings& ps, ProbeResult& out)
		{
			out = ProbeResult();
			if (ls.count < 1 || ls.length < 128 || ls.axisLen < 64 || !(ps.ratio > 0.2) || !(ps.ratio < 0.95))
			{
				out.reason = kReasonBadInput | kReasonNoPower;
				return false;
			}
			// The lines used: maxLines of them, evenly spread.
			const int nl = (std::min)(ls.count, (std::max)(1, ps.maxLines));
			std::vector<int> pick(static_cast<size_t>(nl));
			for (int i = 0; i < nl; ++i)
				pick[static_cast<size_t>(i)] = (i * ls.count) / nl;
			// What the real lines look like (for the reason when the probe is unseen).
			double sumMean = 0.0;
			std::vector<double> stds(static_cast<size_t>(nl), 0.0);
			for (int j = 0; j < nl; ++j)
			{
				const double* y = ls.Line(pick[static_cast<size_t>(j)]);
				double m = 0.0;
				for (int i = 0; i < ls.length; ++i)
					m += y[i];
				m /= static_cast<double>(ls.length);
				double v = 0.0;
				for (int i = 0; i < ls.length; ++i)
					v += (y[i] - m) * (y[i] - m);
				sumMean += m;
				stds[static_cast<size_t>(j)] = std::sqrt(v / static_cast<double>(ls.length));
			}
			out.meanCode = sumMean / static_cast<double>(nl);
			out.medianLineStd = RobustStats::MedianOf(stds.data(), nl);

			// Synthetic upscale of every line: N' output pixels from the N real ones.
			const KernelInfo& ki = Kernel(ps.kernel);
			const int r = ki.radius;
			const int taps = 2 * r;
			const int Nn = RoundToInt(static_cast<double>(ls.axisLen) / ps.ratio);
			const double step = static_cast<double>(ls.axisLen) / static_cast<double>(Nn);
			int jlo = -1;
			int jhi = -1;
			for (int j = 0; j < Nn; ++j)
			{
				const double p = (static_cast<double>(j) + 0.5) * step - 0.5;
				const int c = static_cast<int>(std::floor(p)) - r + 1;
				if (c < ls.origin)
					continue;
				if (c + taps - 1 > ls.origin + ls.length - 1)
					break;
				if (jlo < 0)
					jlo = j;
				jhi = j;
			}
			if (jlo < 0 || jhi - jlo + 1 < 128)
			{
				out.reason = kReasonShortLines | kReasonNoPower;
				return false;
			}
			const int L2 = jhi - jlo + 1;
			std::vector<double> w(static_cast<size_t>(L2) * static_cast<size_t>(taps), 0.0);
			std::vector<int> c0(static_cast<size_t>(L2), 0);
			for (int j = 0; j < L2; ++j)
			{
				const double p = (static_cast<double>(jlo + j) + 0.5) * step - 0.5;
				const double fl = std::floor(p);
				TapWeights(ps.kernel, p - fl, &w[static_cast<size_t>(j) * static_cast<size_t>(taps)]);
				c0[static_cast<size_t>(j)] = static_cast<int>(fl) - r + 1 - ls.origin;
			}
			LineSet syn;
			syn.axis = ls.axis;
			syn.axisLen = Nn;
			syn.origin = jlo;
			syn.Resize(nl, L2);
			for (int l = 0; l < nl; ++l)
				syn.cross[static_cast<size_t>(l)] = pick[static_cast<size_t>(l)] < static_cast<int>(ls.cross.size()) ? ls.cross[static_cast<size_t>(pick[static_cast<size_t>(l)])] : 0;
			for (int l = 0; l < nl; ++l)
			{
				const double* y = ls.Line(pick[static_cast<size_t>(l)]);
				double* o = syn.Line(l);
				for (int j = 0; j < L2; ++j)
				{
					const double* ww = &w[static_cast<size_t>(j) * static_cast<size_t>(taps)];
					const double* yy = y + c0[static_cast<size_t>(j)];
					double acc = 0.0;
					for (int s = 0; s < taps; ++s)
						acc += ww[s] * yy[s];
					if (roundStepCodes > 0.0)
						acc = std::floor(acc / roundStepCodes + 0.5) * roundStepCodes;
					o[j] = acc;
				}
			}
			out.syntheticAxisLen = Nn;
			out.syntheticLength = L2;

			VerifyOptions opt;
			opt.exactOnly = true;
			opt.onlyKernel = ps.kernel;
			opt.exactHalfWidth = 0;
			VerifyResult vr;
			const int cand[1] = { ls.axisLen };
			if (!verifier.Evaluate(syn, cand, 1, opt, vr) || vr.candidates.empty() || !vr.candidates[0].valid || !vr.candidates[0].exactDone)
			{
				out.reason = (vr.reason | kReasonNoPower);
				return false;
			}
			out.ran = true;
			out.cold = vr.cold;
			const CandidateResult& cr = vr.candidates[0];
			std::vector<double> gated;
			const double floorC = floorCodes > 1e-3 ? floorCodes : 1e-3;
			for (int j = 0; j < cr.lineCount; ++j)
				if (cr.lineBase[static_cast<size_t>(j)] >= ps.lineGateFactor * floorC)
					gated.push_back(cr.lineStat[static_cast<size_t>(j)]);
			out.gatedLines = static_cast<int>(gated.size());
			const int minGate = (std::max)(ps.minGateLinesFloor, static_cast<int>(std::ceil(ps.minGateFraction * static_cast<double>(cr.lineCount))));
			if (out.gatedLines >= minGate)
			{
				out.stat = RobustStats::TrimmedMeanOf(gated.data(), out.gatedLines, ps.trimFraction);
				out.seen = out.stat <= ps.seenThreshold;
			}
			if (!out.seen)
			{
				out.reason |= kReasonNoPower;
				if (out.meanCode < ps.darkMeanCodes)
					out.reason |= kReasonDark;
				else if (out.medianLineStd < ps.flatStdCodes)
					out.reason |= kReasonFlat;
				else
					out.reason |= kReasonSoft;
			}
			return true;
		}
	};
}
