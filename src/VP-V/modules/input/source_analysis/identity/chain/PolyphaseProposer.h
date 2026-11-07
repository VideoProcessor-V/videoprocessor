// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// PolyphaseProposer (SID-C1 unit 2): cheap, kernel-agnostic candidate rates for one axis.
//
// An upscaled line is periodic in its sampling phase: with latent/output ratio n/N = m/q the output pixel i has
// the sub-sample phase (i m mod q) / q, so a linear predictor that fits ONE filter for all pixels is a
// compromise, while a predictor fitted per phase class (i mod q) predicts markedly better. The proposer fits,
// for every period q = 2 .. qMax, a 5-tap least-squares predictor (neighbours at -3, -2, -1, +1, +2, line mean
// removed) per phase class, and compares the summed residual with the single pooled predictor:
//   gain(q)  = N ln(RSS_pooled / RSS_q)                 (nats; N = predicted samples)
//   ratio(q) = gain(q) / (5 (q - 1))                    (per extra degree of freedom; ~1 on a native picture)
//   dBIC(q)  = gain(q) - 5 (q - 1) ln N
// Only the sufficient statistics (normal equations per phase class) are accumulated, over lines and over
// evaluations (halved when more than `windowEvaluations` have been added), so the cost is paid once per sample
// and the statistic sharpens with time. Periods with ratio >= ratioMin are taken in order of dBIC; multiples of an
// already accepted period are skipped (they are explained by it).
//
// A period q says the ratio is m/q for some m coprime to q; the candidates are n = round(N m / q) for every such
// m with n/N in [minNRatio, maxNRatio], those nearest 1/2 first (the verifier decides; the order inside one
// period carries no evidence). The slots are filled by strength: periods in dBIC order, ONE size per period per
// round (round robin), so a weaker period is not starved by a stronger one with many sizes. `lowVarPhase` reports the phase class with the smallest residual variance.
// Caller-supplied sizes (the previous latched n, rationals of the active size) are listed first. At most
// kMaxCandidates are returned.
//
// Limits: a ratio that is not (near) m/q with q <= qMax is only found through the caller's candidates; a size
// whose N m / q is not an integer drifts out of phase over a long line and is proposed with reduced strength.
#include "ChainCommon.h"

namespace SourceIdentity
{
	namespace Chain
	{
		struct ProposerSettings
		{
			int qMax = 16;                 // longest phase period tested
			int maxLines = 16;             // lines of a LineSet used per evaluation
			double ratioMin = 5.0;         // per-degree-of-freedom gain a period must reach
			int maxPeriods = 3;
			double minNRatio = 0.2;        // candidates n / N range
			double maxNRatio = 0.985;
			int windowEvaluations = 16;    // statistics are halved beyond this many evaluations
			int minSamples = 2000;         // predicted samples needed before any period is reported
			int callerSlots = 4;           // caller-supplied sizes kept (first come, first kept)
			int minLatent = 16;
		};

		constexpr uint32_t kSourceCaller = 1u;
		constexpr uint32_t kSourcePolyphase = 2u;

		struct ProposedCandidate
		{
			int n = 0;
			int q = 0;                // period of the polyphase evidence (0 for caller sizes)
			int m = 0;
			uint32_t source = 0;
			double score = 0.0;       // dBIC of the period (0 for caller sizes)
		};

		struct PeriodScore
		{
			int q = 0;
			double gain = 0.0;        // nats
			double ratio = 0.0;       // per extra degree of freedom
			double dBic = 0.0;
			int lowVarPhase = 0;
		};

		class PolyphaseProposer
		{
		public:
			explicit PolyphaseProposer(const ProposerSettings& s = ProposerSettings()) : s_(s)
			{
				if (s_.qMax < 2)
					s_.qMax = 2;
				if (s_.qMax > 32)
					s_.qMax = 32;
				int total = 0;
				for (int q = 2; q <= s_.qMax; ++q)
				{
					off_[q] = total;
					total += q;
				}
				slots_ = total;
				Reset();
			}

			void Reset()
			{
				stats_.assign(static_cast<size_t>(slots_) * kTerms, 0.0);
				evals_ = 0;
				axisLen_ = 0;
				origin_ = 0;
			}

			int Evaluations() const { return evals_; }

			// Adds one axis worth of lines. A change of axis length or origin resets the statistics first.
			bool AddEvaluation(const LineSet& ls)
			{
				if (ls.count < 1 || ls.length < 64 || ls.axisLen < 64)
					return false;
				if (axisLen_ != 0 && (axisLen_ != ls.axisLen || origin_ != ls.origin))
					Reset();
				axisLen_ = ls.axisLen;
				origin_ = ls.origin;
				const int nl = std::min(ls.count, std::max(1, s_.maxLines));
				std::vector<double> v(static_cast<size_t>(ls.length), 0.0);
				double tm[kTerms];
				for (int li = 0; li < nl; ++li)
				{
					const double* y = ls.Line((li * ls.count) / nl);
					double mean = 0.0;
					for (int k = 0; k < ls.length; ++k)
						mean += y[k];
					mean /= static_cast<double>(ls.length);
					for (int k = 0; k < ls.length; ++k)
						v[static_cast<size_t>(k)] = y[k] - mean;
					int ph[33];
					for (int q = 2; q <= s_.qMax; ++q)
						ph[q] = (origin_ + 3) % q;   // phase class of sample k = 3; advanced by 1 per sample (no division in the loop)
					for (int k = 3; k <= ls.length - 3; ++k)
					{
						const double f[kTaps] = { v[static_cast<size_t>(k - 3)], v[static_cast<size_t>(k - 2)], v[static_cast<size_t>(k - 1)],
							v[static_cast<size_t>(k + 1)], v[static_cast<size_t>(k + 2)] };
						const double t = v[static_cast<size_t>(k)];
						int idx = 0;
						for (int i = 0; i < kTaps; ++i)
							for (int j = 0; j <= i; ++j)
								tm[idx++] = f[i] * f[j];
						for (int i = 0; i < kTaps; ++i)
							tm[15 + i] = f[i] * t;
						tm[20] = t * t;
						tm[21] = 1.0;
						for (int q = 2; q <= s_.qMax; ++q)
						{
							double* dst = &stats_[(static_cast<size_t>(off_[q]) + static_cast<size_t>(ph[q])) * kTerms];
							for (int e = 0; e < kTerms; ++e)
								dst[e] += tm[e];
							if (++ph[q] == q)
								ph[q] = 0;
						}
					}
				}
				++evals_;
				if (evals_ > s_.windowEvaluations)
				{
					for (size_t e = 0; e < stats_.size(); ++e)
						stats_[e] *= 0.5;
					evals_ = s_.windowEvaluations / 2;
				}
				return true;
			}

			// Every tested period (q = 2 .. qMax) with its gain, ratio and dBIC, in q order: diagnostics and tests.
			int AllScores(PeriodScore* out, int cap) const
			{
				std::vector<PeriodScore> all;
				Scores(all);
				int n = 0;
				for (size_t i = 0; i < all.size() && n < cap; ++i)
					out[n++] = all[i];
				return n;
			}

			// All periods that reach ratioMin, accepted order (dBIC descending, multiples of accepted periods skipped).
			int Periods(PeriodScore* out, int cap) const
			{
				std::vector<PeriodScore> all;
				Scores(all);
				std::sort(all.begin(), all.end(), [](const PeriodScore& a, const PeriodScore& b) {
					if (a.dBic != b.dBic)
						return a.dBic > b.dBic;
					return a.q < b.q;
				});
				int n = 0;
				for (size_t i = 0; i < all.size() && n < cap && n < s_.maxPeriods; ++i)
				{
					if (all[i].ratio < s_.ratioMin)
						continue;
					bool multiple = false;
					for (int a = 0; a < n; ++a)
						if (all[i].q % out[a].q == 0)
							multiple = true;
					if (multiple)
						continue;
					out[n++] = all[i];
				}
				return n;
			}

			// Candidate latent sizes for an axis of N pixels: caller sizes first, then the polyphase proposals.
			int Propose(int N, const int* callerN, int callerCount, ProposedCandidate* out, int cap) const
			{
				if (cap > kMaxCandidates)
					cap = kMaxCandidates;
				int n = 0;
				const int maxN = static_cast<int>(std::floor(s_.maxNRatio * static_cast<double>(N)));
				int taken = 0;
				for (int i = 0; i < callerCount && n < cap && taken < s_.callerSlots; ++i)
				{
					if (callerN[i] < s_.minLatent || callerN[i] > maxN || Has(out, n, callerN[i], 0))
						continue;
					ProposedCandidate c;
					c.n = callerN[i];
					c.source = kSourceCaller;
					out[n++] = c;
					++taken;
				}
				PeriodScore per[kMaxCandidates];
				const int np = Periods(per, kMaxCandidates);
				std::vector<std::vector<int>> sizes(static_cast<size_t>(np));
				size_t longest = 0;
				for (int p = 0; p < np; ++p)
				{
					const int q = per[p].q;
					std::vector<int>& ms = sizes[static_cast<size_t>(p)];
					for (int m = 1; m < q; ++m)
					{
						const double r = static_cast<double>(m) / static_cast<double>(q);
						if (GcdInt(m, q) == 1 && r >= s_.minNRatio && r <= s_.maxNRatio)
							ms.push_back(m);
					}
					std::sort(ms.begin(), ms.end(), [q](int a, int b) {
						const double da = std::abs(static_cast<double>(a) / q - 0.5);
						const double db = std::abs(static_cast<double>(b) / q - 0.5);
						if (da != db)
							return da < db;
						return a < b;
					});
					if (ms.size() > longest)
						longest = ms.size();
				}
				for (size_t rank = 0; rank < longest && n < cap; ++rank)
					for (int p = 0; p < np && n < cap; ++p)
					{
						const std::vector<int>& ms = sizes[static_cast<size_t>(p)];
						if (rank >= ms.size())
							continue;
						const int q = per[p].q;
						const int cn = RoundToInt(static_cast<double>(N) * static_cast<double>(ms[rank]) / static_cast<double>(q));
						if (cn < s_.minLatent || cn > maxN || Has(out, n, cn, 1))
							continue;
						ProposedCandidate c;
						c.n = cn;
						c.q = q;
						c.m = ms[rank];
						c.source = kSourcePolyphase;
						c.score = per[p].dBic;
						out[n++] = c;
					}
				return n;
			}

		private:
			enum { kTaps = 5, kTerms = 22 };   // kTerms = 15 lower-triangle products, 5 cross terms, y^2, count

			static bool Has(const ProposedCandidate* c, int n, int v, int tol)
			{
				for (int i = 0; i < n; ++i)
				{
					const int d = c[i].n > v ? c[i].n - v : v - c[i].n;
					if (d <= tol)
						return true;
				}
				return false;
			}

			// Least-squares fit of one set of statistics: residual sum of squares and sample count.
			static bool Fit(const double* st, double& rss, double& cnt)
			{
				cnt = st[21];
				if (cnt < static_cast<double>(4 * kTaps))
					return false;
				double A[kTaps][kTaps];
				int idx = 0;
				for (int i = 0; i < kTaps; ++i)
					for (int j = 0; j <= i; ++j)
					{
						A[i][j] = st[idx];
						A[j][i] = st[idx];
						++idx;
					}
				double tr = 0.0;
				for (int i = 0; i < kTaps; ++i)
					tr += A[i][i];
				const double ridge = 1e-9 * tr / static_cast<double>(kTaps) + 1e-12;
				for (int i = 0; i < kTaps; ++i)
					A[i][i] += ridge;
				// Cholesky A = L L^T in place (lower).
				for (int j = 0; j < kTaps; ++j)
				{
					double s = A[j][j];
					for (int k = 0; k < j; ++k)
						s -= A[j][k] * A[j][k];
					if (!(s > 1e-300))
						return false;
					const double d = std::sqrt(s);
					A[j][j] = d;
					for (int i = j + 1; i < kTaps; ++i)
					{
						double v = A[i][j];
						for (int k = 0; k < j; ++k)
							v -= A[i][k] * A[j][k];
						A[i][j] = v / d;
					}
				}
				double b[kTaps];
				for (int i = 0; i < kTaps; ++i)
					b[i] = st[15 + i];
				double z[kTaps];
				for (int i = 0; i < kTaps; ++i)
				{
					double v = b[i];
					for (int k = 0; k < i; ++k)
						v -= A[i][k] * z[k];
					z[i] = v / A[i][i];
				}
				double w[kTaps];
				for (int i = kTaps - 1; i >= 0; --i)
				{
					double v = z[i];
					for (int k = i + 1; k < kTaps; ++k)
						v -= A[k][i] * w[k];
					w[i] = v / A[i][i];
				}
				double fit = 0.0;
				for (int i = 0; i < kTaps; ++i)
					fit += b[i] * w[i];
				rss = st[20] - fit;
				if (rss < 0.0)
					rss = 0.0;
				return true;
			}

			void Scores(std::vector<PeriodScore>& out) const
			{
				out.clear();
				if (evals_ <= 0)
					return;
				double pooled[kTerms];
				for (int e = 0; e < kTerms; ++e)
					pooled[e] = stats_[static_cast<size_t>(off_[2]) * kTerms + e] + stats_[(static_cast<size_t>(off_[2]) + 1) * kTerms + e];
				double rssPooled = 0.0;
				double total = 0.0;
				if (!Fit(pooled, rssPooled, total) || total < static_cast<double>(s_.minSamples) || !(rssPooled > 0.0))
					return;
				const double lnN = std::log(total);
				for (int q = 2; q <= s_.qMax; ++q)
				{
					double rssQ = 0.0;
					bool ok = true;
					int low = 0;
					double lowVar = 0.0;
					for (int c = 0; c < q; ++c)
					{
						double r = 0.0;
						double cn = 0.0;
						if (!Fit(&stats_[(static_cast<size_t>(off_[q]) + static_cast<size_t>(c)) * kTerms], r, cn))
						{
							ok = false;
							break;
						}
						rssQ += r;
						const double var = r / cn;
						if (c == 0 || var < lowVar)
						{
							lowVar = var;
							low = c;
						}
					}
					if (!ok || !(rssQ > 0.0))
						continue;
					PeriodScore p;
					p.q = q;
					p.gain = total * std::log(rssPooled / rssQ);
					if (p.gain < 0.0)
						p.gain = 0.0;
					const double dof = static_cast<double>(kTaps * (q - 1));
					p.ratio = p.gain / dof;
					p.dBic = p.gain - dof * lnN;
					p.lowVarPhase = low;
					out.push_back(p);
				}
			}

			ProposerSettings s_;
			int off_[33] = {};
			int slots_ = 0;
			std::vector<double> stats_;
			int evals_ = 0;
			int axisLen_ = 0;
			int origin_ = 0;
		};
	}
}
