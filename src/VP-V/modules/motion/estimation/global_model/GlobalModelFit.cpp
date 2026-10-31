// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// Global camera model: the per-pair fitter (implementation plan section 1, steps 1-11).
// Pure C++14, deterministic: fixed loop orders, double sums, splitmix64, no clocks except the
// injected Config::clock, no unordered containers, no threads.
#include "GlobalModelFit.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

namespace CameraModel
{
	namespace
	{
		const double kSqrt2 = 1.4142135623730951;

		inline bool FiniteF(float v) { return std::isfinite(v); }
		inline double Clamp01(double v) { return v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v); }

		// The usable, stratified sample set of one pair (picture-centred positions).
		struct Field
		{
			int gridW = 0, gridH = 0;
			double blockSize = 0;
			Rect pic;
			double prec = 0;                 // vector precision, source px
			double halfDiag = 0;             // picture half diagonal
			double diag = 0;
			double absThr = 0;               // absThresholdSigmas * prec
			double rel = 0;                  // relThreshold
			std::vector<Sample> s;           // w = trust
			std::vector<int> blk;            // block index per sample
			std::vector<uint8_t> excluded;   // per block: bars or text guard
			int cellsX = 1, cellsY = 1, cellsWithSample = 0;
			double trustSum = 0;
		};

		struct Score
		{
			int inliers = 0;
			double cost = 0;
		};

		inline bool Better(const Score& a, const Score& b)
		{
			if (a.inliers != b.inliers)
				return a.inliers > b.inliers;
			return a.cost < b.cost;
		}

		inline double Threshold(const Field& f, double predMag)
		{
			return std::max(f.absThr, f.rel * predMag);
		}

		inline void Residual(const Params& p, const Sample& s, double* r, double* predMag)
		{
			double dx = 0, dy = 0;
			Displacement(p, s.x, s.y, &dx, &dy);
			const double ex = s.dx - dx, ey = s.dy - dy;
			*r = std::sqrt(ex * ex + ey * ey);
			*predMag = std::sqrt(dx * dx + dy * dy);
		}

		// MAGSAC++-style sigma-marginalised weight: 1 at 0, 0 beyond 3 sigma.
		inline double MagW(double r, double sigma)
		{
			if (!(r < 3.0 * sigma))
				return 0.0;
			return std::erfc(r / (kSqrt2 * sigma));
		}

		Score ScoreOn(const Field& f, const Params& p, const int* idx, int n)
		{
			Score sc;
			const int count = idx ? n : static_cast<int>(f.s.size());
			for (int k = 0; k < count; ++k)
			{
				const Sample& s = f.s[idx ? idx[k] : k];
				double r, pm;
				Residual(p, s, &r, &pm);
				const double t = Threshold(f, pm);
				const double r2 = r * r, t2 = t * t;
				if (r <= t)
					++sc.inliers;
				sc.cost += s.w * ((r2 < t2) ? r2 : t2);
			}
			return sc;
		}

		void BuildWeighted(const Field& f, const std::vector<Sample>& samples, const Params& p, std::vector<Sample>& out)
		{
			out.clear();
			for (size_t i = 0; i < samples.size(); ++i)
			{
				double r, pm;
				Residual(p, samples[i], &r, &pm);
				const double w = samples[i].w * MagW(r, Threshold(f, pm));
				if (w > 0.0)
				{
					Sample q = samples[i];
					q.w = w;
					out.push_back(q);
				}
			}
		}

		// IRLS refinement of p (its own type). True when at least one iteration was accepted.
		bool Refine(const Field& f, const std::vector<Sample>& samples, Params& p, int iterations)
		{
			if (p.type == ModelType::None)
				return false;
			bool any = false;
			std::vector<Sample> buf;
			buf.reserve(samples.size());
			for (int it = 0; it < iterations; ++it)
			{
				BuildWeighted(f, samples, p, buf);
				if (static_cast<int>(buf.size()) < MinimalCount(p.type))
					break;
				Params q;
				if (!FitWeighted(p.type, buf.data(), static_cast<int>(buf.size()), f.halfDiag, q))
					break;
				q.type = p.type;
				if (IsDegenerate(q, f.pic))
					break;
				p = q;
				any = true;
			}
			return any;
		}

		int ParamCount(ModelType t)
		{
			switch (t)
			{
			case ModelType::Translation: return 2;
			case ModelType::Similarity: return 4;
			case ModelType::Affine: return 6;
			case ModelType::Homography: return 8;
			default: return 0;
			}
		}

		inline int Quadrant(double x, double y) { return (x >= 0 ? 1 : 0) + (y >= 0 ? 2 : 0); }

		bool BuildField(const PairInput& in, const Config& c, Field& f)
		{
			if (in.gridW <= 0 || in.gridH <= 0 || in.picture.Empty() || !(in.blockSize > 0) || !(in.pairSeconds > 0)
				|| in.fwdX == nullptr || in.fwdY == nullptr || in.fwdTrust == nullptr)
				return false;
			f.gridW = in.gridW;
			f.gridH = in.gridH;
			f.blockSize = in.blockSize;
			f.pic = in.picture;
			f.prec = in.precisionPx > 0 ? in.precisionPx : 0.25 * in.blockSize / 16.0;
			const double W = in.picture.Width(), H = in.picture.Height();
			f.diag = std::sqrt(W * W + H * H);
			f.halfDiag = 0.5 * f.diag;
			f.absThr = c.absThresholdSigmas * f.prec;
			f.rel = c.relThreshold;

			const int nb = in.gridW * in.gridH;
			f.excluded.assign(static_cast<size_t>(nb), 0);
			for (int by = 0; by < in.gridH; ++by)
				for (int bx = 0; bx < in.gridW; ++bx)
				{
					const double cx = (bx + 0.5) * in.blockSize, cy = (by + 0.5) * in.blockSize;
					const bool bar = cx < in.picture.left || cx >= in.picture.right || cy < in.picture.top || cy >= in.picture.bottom;
					const bool guard = in.textGuard != nullptr && in.textGuard[by * in.gridW + bx] == 1;
					if (bar || guard)
						f.excluded[static_cast<size_t>(by * in.gridW + bx)] = 1;
				}

			// Cells: sized from targetCells, aspect-scaled to the picture, never smaller than a block.
			const double target = std::max(1, c.targetCells);
			int cellsX = static_cast<int>(std::floor(std::sqrt(target * W / H) + 0.5));
			const int maxX = std::max(1, static_cast<int>(std::floor(W / in.blockSize)));
			cellsX = std::min(std::max(1, cellsX), maxX);
			int cellsY = std::max(1, static_cast<int>(target / cellsX));
			const int maxY = std::max(1, static_cast<int>(std::floor(H / in.blockSize)));
			cellsY = std::min(cellsY, maxY);
			f.cellsX = cellsX;
			f.cellsY = cellsY;
			const double cw = W / cellsX, ch = H / cellsY;

			const int cells = cellsX * cellsY;
			std::vector<int> best(static_cast<size_t>(cells), -1);
			std::vector<float> bestTrust(static_cast<size_t>(cells), 0.0f);
			for (int by = 0; by < in.gridH; ++by)
				for (int bx = 0; bx < in.gridW; ++bx)
				{
					const int b = by * in.gridW + bx;
					if (f.excluded[static_cast<size_t>(b)])
						continue;
					const float tr = in.fwdTrust[b];
					if (!FiniteF(tr) || !(tr > 0.0f) || !FiniteF(in.fwdX[b]) || !FiniteF(in.fwdY[b]))
						continue;
					const double fx = (bx + 0.5) * in.blockSize - in.picture.left;
					const double fy = (by + 0.5) * in.blockSize - in.picture.top;
					int cx = static_cast<int>(std::floor(fx / cw)), cy = static_cast<int>(std::floor(fy / ch));
					cx = std::min(std::max(cx, 0), cellsX - 1);
					cy = std::min(std::max(cy, 0), cellsY - 1);
					const int ci = cy * cellsX + cx;
					if (best[static_cast<size_t>(ci)] < 0 || tr > bestTrust[static_cast<size_t>(ci)])
					{
						best[static_cast<size_t>(ci)] = b;
						bestTrust[static_cast<size_t>(ci)] = tr;
					}
				}
			for (int ci = 0; ci < cells; ++ci)
			{
				const int b = best[static_cast<size_t>(ci)];
				if (b < 0)
					continue;
				const int bx = b % in.gridW, by = b / in.gridW;
				Sample s;
				BlockCentre(bx, by, in.blockSize, in.picture, &s.x, &s.y);
				s.dx = in.fwdX[b];
				s.dy = in.fwdY[b];
				s.w = in.fwdTrust[b];
				f.s.push_back(s);
				f.blk.push_back(b);
				f.trustSum += s.w;
				++f.cellsWithSample;
			}
			return true;
		}

		void SortedByTrust(const Field& f, std::vector<int>& order)
		{
			order.resize(f.s.size());
			for (size_t i = 0; i < order.size(); ++i)
				order[i] = static_cast<int>(i);
			std::sort(order.begin(), order.end(), [&f](int a, int b)
			{
				if (f.s[static_cast<size_t>(a)].w != f.s[static_cast<size_t>(b)].w)
					return f.s[static_cast<size_t>(a)].w > f.s[static_cast<size_t>(b)].w;
				return a < b;
			});
		}

		// Draw k distinct indices from order[0..pool).
		void Draw(SplitMix64& rng, const std::vector<int>& order, int pool, int k, int* out)
		{
			for (int i = 0; i < k; ++i)
			{
				for (;;)
				{
					const int v = order[rng.Below(static_cast<uint32_t>(pool))];
					bool dup = false;
					for (int j = 0; j < i; ++j)
						if (out[j] == v)
							dup = true;
					if (!dup)
					{
						out[i] = v;
						break;
					}
				}
			}
		}

		struct Hyp
		{
			Params p;
			Score s1;
		};

		double InliersSpreadConditioning(const Field& f, const std::vector<uint8_t>& inl)
		{
			double sw = 0, mx = 0, my = 0;
			for (size_t i = 0; i < f.s.size(); ++i)
				if (inl[i])
				{
					sw += f.s[i].w;
					mx += f.s[i].w * f.s[i].x;
					my += f.s[i].w * f.s[i].y;
				}
			if (!(sw > 0))
				return 0.0;
			mx /= sw;
			my /= sw;
			double vx = 0, vy = 0;
			for (size_t i = 0; i < f.s.size(); ++i)
				if (inl[i])
				{
					vx += f.s[i].w * (f.s[i].x - mx) * (f.s[i].x - mx);
					vy += f.s[i].w * (f.s[i].y - my) * (f.s[i].y - my);
				}
			const double sx = std::sqrt(vx / sw) / (0.3 * f.pic.Width());
			const double sy = std::sqrt(vy / sw) / (0.3 * f.pic.Height());
			return Clamp01(std::min(sx, sy));
		}
	}

	struct Fitter::Impl
	{
		static void FillIdentity(CameraMotion& rec, const PairInput& in)
		{
			rec.seq = in.seq;
			rec.pairFrom = in.pairFrom;
			rec.pairTo = in.pairTo;
			rec.pairSeconds = in.pairSeconds;
			rec.resyncGeneration = in.resyncGeneration;
			rec.rebaseGeneration = in.rebaseGeneration;
			rec.picture = in.picture;
			rec.fadeGain = in.fadeGain;
			rec.fadeOffset = in.fadeOffset;
			rec.inFade = in.fade;
		}

		static bool DeadlinePassed(const Config& c, const PairInput& in)
		{
			return c.clock != nullptr && in.deadlineMicros > 0 && c.clock() > in.deadlineMicros;
		}

		// Fits one pair. Reads (never writes) the Fitter's history; objects are updated only when asked.
		static CameraMotion Run(Fitter& F, const PairInput& in, const Params* h0, bool temporalRead, bool runObjects, bool genReset)
		{
			const Config& c = F.m_config;
			CameraMotion rec;
			FillIdentity(rec, in);
			rec.valid = false;
			rec.reason = Reason::NoInput;
			rec.confidence = 0;

			Field f;
			if (!BuildField(in, c, f))
				return rec;
			const int N = static_cast<int>(f.s.size());
			rec.samples = static_cast<uint32_t>(N);
			const int cellsTotal = f.cellsX * f.cellsY;
			const int minSamples = std::max(8, static_cast<int>(std::ceil(0.01 * cellsTotal)));
			if (N < minSamples)
			{
				rec.reason = genReset ? Reason::GenerationReset : Reason::TooFewSamples;
				return rec;
			}

			const ModelType maxModel = (c.maxModel == ModelType::None) ? ModelType::Translation : c.maxModel;
			const uint64_t seed = in.seq ^ (in.pairFrom * 0x9E3779B97F4A7C15ull);
			SplitMix64 rng(seed);
			int cap = std::max(1, c.maxHypotheses);
			if (c.preemptiveBudget > 0)
				cap = std::min(cap, c.preemptiveBudget);
			const int irls = std::max(0, c.irlsIterations);
			bool deadlineHit = false;

			// ---- Hypotheses ----
			int count = 0;
			std::vector<Params> pool;           // every scored hypothesis (competitor candidates)
			bool haveWinner = false;
			Params winner;
			Score winnerScore;

			auto consider = [&](const Params& p, const Score& s)
			{
				if (!haveWinner || Better(s, winnerScore))
				{
					winner = p;
					winnerScore = s;
					haveWinner = true;
				}
			};

			bool earlyExit = false;
			Score h1Score;
			bool haveH1 = false;
			if (h0 != nullptr && h0->type != ModelType::None && h0->type <= maxModel && !IsDegenerate(*h0, f.pic))
			{
				++count;
				Params h0r = *h0;
				Refine(f, f.s, h0r, 3);
				const Score s0 = ScoreOn(f, h0r, nullptr, 0);
				pool.push_back(h0r);
				consider(h0r, s0);
				// H1 below is scored first for the early-exit comparison.
				if (in.haveEngineGlobal && std::isfinite(in.engineGx) && std::isfinite(in.engineGy))
				{
					Params h1 = IdentityParams(ModelType::Translation);
					h1.h[2] = in.engineGx;
					h1.h[5] = in.engineGy;
					h1Score = ScoreOn(f, h1, nullptr, 0);
					haveH1 = true;
					++count;
					pool.push_back(h1);
					consider(h1, h1Score);
				}
				const double floorCost = f.trustSum * (0.25 * f.prec) * (0.25 * f.prec);
				if (static_cast<double>(s0.inliers) >= 0.8 * N && (!haveH1 || s0.cost <= 1.1 * h1Score.cost + floorCost))
				{
					winner = h0r;
					winnerScore = s0;
					earlyExit = true;
				}
			}
			else if (in.haveEngineGlobal && std::isfinite(in.engineGx) && std::isfinite(in.engineGy))
			{
				Params h1 = IdentityParams(ModelType::Translation);
				h1.h[2] = in.engineGx;
				h1.h[5] = in.engineGy;
				h1Score = ScoreOn(f, h1, nullptr, 0);
				haveH1 = true;
				++count;
				pool.push_back(h1);
				consider(h1, h1Score);
			}

			if (!earlyExit)
			{
				std::vector<int> order;
				SortedByTrust(f, order);
				std::vector<int> sub;
				if (N > c.stage1Subset && c.stage1Subset > 0)
				{
					const int m = c.stage1Subset;
					const uint32_t off = rng.Below(static_cast<uint32_t>(N));
					sub.resize(static_cast<size_t>(m));
					for (int k = 0; k < m; ++k)
						sub[static_cast<size_t>(k)] = static_cast<int>((static_cast<int64_t>(k) * N + off) / m);
				}
				const int* subPtr = sub.empty() ? nullptr : sub.data();
				const int subN = static_cast<int>(sub.size());

				static const ModelType kTypes[4] = { ModelType::Translation, ModelType::Similarity, ModelType::Affine, ModelType::Homography };
				const int ntypes = static_cast<int>(maxModel) - static_cast<int>(ModelType::Translation) + 1;
				const int maxAttempts = 4 * cap + 8;
				const int halfCap = std::max(1, cap / 2);
				std::vector<Hyp> hyps;
				hyps.reserve(static_cast<size_t>(cap));
				for (int attempt = 0; count < cap && attempt < maxAttempts; ++attempt)
				{
					if ((attempt & 15) == 15 && DeadlinePassed(c, in))
					{
						deadlineHit = true;
						break;
					}
					const ModelType ty = kTypes[attempt % ntypes];
					const int k = MinimalCount(ty);
					int poolSize = k + static_cast<int>(static_cast<int64_t>(N - k) * std::min(attempt, halfCap) / halfCap);
					poolSize = std::min(N, std::max(k, poolSize));
					int idx[4];
					Draw(rng, order, poolSize, k, idx);
					Sample ms[4];
					for (int j = 0; j < k; ++j)
						ms[j] = f.s[static_cast<size_t>(idx[j])];
					Params p;
					if (!SolveMinimal(ty, ms, k, p))
						continue;
					p.type = ty;
					if (IsDegenerate(p, f.pic))
						continue;
					Hyp h;
					h.p = p;
					h.s1 = ScoreOn(f, p, subPtr, subN);
					hyps.push_back(h);
					++count;
				}

				// Stage 2: the best stage2Best by stage-1 score are scored on all samples.
				const int nb2 = std::min(std::max(1, c.stage2Best), static_cast<int>(hyps.size()));
				std::vector<uint8_t> taken(hyps.size(), 0);
				for (int k = 0; k < nb2; ++k)
				{
					int bi = -1;
					for (size_t i = 0; i < hyps.size(); ++i)
						if (!taken[i] && (bi < 0 || Better(hyps[i].s1, hyps[static_cast<size_t>(bi)].s1)))
							bi = static_cast<int>(i);
					if (bi < 0)
						break;
					taken[static_cast<size_t>(bi)] = 1;
					const Params& p = hyps[static_cast<size_t>(bi)].p;
					consider(p, ScoreOn(f, p, nullptr, 0));
				}
				for (size_t i = 0; i < hyps.size(); ++i)
					pool.push_back(hyps[i].p);
			}
			rec.hypotheses = static_cast<uint32_t>(count);

			if (!haveWinner)
			{
				rec.reason = deadlineHit ? Reason::DeadlineMissed : (genReset ? Reason::GenerationReset : Reason::Degenerate);
				return rec;
			}

			// ---- Local optimisation + model choice ----
			Params w = winner;
			Refine(f, f.s, w, irls);
			std::vector<Sample> wbuf;
			BuildWeighted(f, f.s, w, wbuf);
			if (static_cast<int>(wbuf.size()) < 1)
				wbuf = f.s;

			// Support set of the winner (for the cost comparison) and its quadrant mass.
			std::vector<uint8_t> support(f.s.size(), 0);
			double tiny = 0;
			for (size_t i = 0; i < f.s.size(); ++i)
			{
				double r, pm;
				Residual(w, f.s[i], &r, &pm);
				if (r <= 3.0 * Threshold(f, pm))
				{
					support[i] = 1;
					tiny += f.s[i].w * (0.5 * f.prec) * (0.5 * f.prec);
				}
			}
			double qMass[4] = { 0, 0, 0, 0 }, massTotal = 0;
			for (size_t i = 0; i < wbuf.size(); ++i)
			{
				qMass[Quadrant(wbuf[i].x, wbuf[i].y)] += wbuf[i].w;
				massTotal += wbuf[i].w;
			}
			int quadrants = 0;
			for (int q = 0; q < 4; ++q)
				if (massTotal > 0 && qMass[q] / massTotal >= c.quadrantMinShare)
					++quadrants;

			auto supportCost = [&](const Params& p)
			{
				double cost = 0;
				for (size_t i = 0; i < f.s.size(); ++i)
					if (support[i])
					{
						double r, pm;
						Residual(p, f.s[i], &r, &pm);
						const double t3 = 3.0 * Threshold(f, pm);
						const double r2 = r * r, t2 = t3 * t3;
						cost += f.s[i].w * ((r2 < t2) ? r2 : t2);
					}
				return cost;
			};

			static const ModelType kAll[4] = { ModelType::Translation, ModelType::Similarity, ModelType::Affine, ModelType::Homography };
			Params chosen;
			bool haveChosen = false;
			double chosenCost = 0;
			for (int ti = 0; ti < 4; ++ti)
			{
				const ModelType ty = kAll[ti];
				if (ty > maxModel)
					break;
				if (ty == ModelType::Affine && quadrants < 2 && haveChosen)
					continue;
				if (ty == ModelType::Homography && quadrants < 3 && haveChosen)
					continue;
				Params p;
				if (static_cast<int>(wbuf.size()) < MinimalCount(ty) || !FitWeighted(ty, wbuf.data(), static_cast<int>(wbuf.size()), f.halfDiag, p))
					continue;
				p.type = ty;
				if (IsDegenerate(p, f.pic))
					continue;
				Refine(f, f.s, p, irls);
				if (IsDegenerate(p, f.pic))
					continue;
				const double cost = supportCost(p);
				if (!haveChosen)
				{
					chosen = p;
					chosenCost = cost;
					haveChosen = true;
					continue;
				}
				const double extra = ParamCount(ty) - ParamCount(chosen.type);
				const double drop = (chosenCost - cost) / std::max(chosenCost, tiny);
				if (drop > c.complexityPenaltyPerParam * extra)
				{
					chosen = p;
					chosenCost = cost;
				}
			}
			if (!haveChosen)
			{
				rec.reason = Reason::Degenerate;
				return rec;
			}
			const Params model = chosen;

			// ---- Inliers, signals ----
			std::vector<uint8_t> inl(f.s.size(), 0);
			double inlierMass = 0, resSq = 0;
			int inlierCount = 0;
			for (size_t i = 0; i < f.s.size(); ++i)
			{
				double r, pm;
				Residual(model, f.s[i], &r, &pm);
				if (r <= Threshold(f, pm))
				{
					inl[i] = 1;
					++inlierCount;
					inlierMass += f.s[i].w;
					resSq += f.s[i].w * r * r;
				}
			}
			Signals sg;
			sg.inlierShare = f.trustSum > 0 ? Clamp01(inlierMass / f.trustSum) : 0.0;
			const double rms = inlierMass > 0 ? std::sqrt(resSq / inlierMass) : 0.0;
			{
				const double q = rms / (2.0 * f.prec);
				sg.precision = 1.0 / (1.0 + q * q);
			}
			sg.coverage = std::min(1.0, 1.5 * static_cast<double>(f.cellsWithSample) / cellsTotal);
			sg.conditioning = InliersSpreadConditioning(f, inl);

			// Engine agreement.
			if (in.haveEngineGlobal && std::isfinite(in.engineGx) && std::isfinite(in.engineGy))
			{
				double dx = 0, dy = 0;
				Displacement(model, 0.0, 0.0, &dx, &dy);
				const double d = std::hypot(dx - in.engineGx, dy - in.engineGy);
				const double mag = std::max(std::hypot(in.engineGx, in.engineGy), std::hypot(dx, dy));
				const double t = std::max(f.absThr, f.rel * mag);
				const double q = d / (2.0 * t);
				sg.engineAgreement = std::exp(-q * q);
				sg.haveEngine = true;
			}

			// Ambiguity: best disjoint-ish competitor.
			{
				std::vector<int> nonIn;
				for (size_t i = 0; i < f.s.size(); ++i)
					if (!inl[i])
						nonIn.push_back(static_cast<int>(i));
				std::vector<Params> cands = pool;
				if (nonIn.size() >= 2)
				{
					SplitMix64 r2(seed ^ 0xA5A5A5A5DEADBEEFull);
					for (int k = 0; k < 24; ++k)
					{
						const ModelType ty = (k & 1) ? ModelType::Similarity : ModelType::Translation;
						if (ty > maxModel)
							continue;
						const int kk = MinimalCount(ty);
						int idx[2];
						int tries = 0;
						bool ok = false;
						while (tries++ < 8 && !ok)
						{
							for (int j = 0; j < kk; ++j)
								idx[j] = nonIn[r2.Below(static_cast<uint32_t>(nonIn.size()))];
							ok = (kk == 1) || (idx[0] != idx[1]);
						}
						if (!ok)
							continue;
						Sample ms[2];
						for (int j = 0; j < kk; ++j)
							ms[j] = f.s[static_cast<size_t>(idx[j])];
						Params p;
						if (SolveMinimal(ty, ms, kk, p))
						{
							p.type = ty;
							if (!IsDegenerate(p, f.pic))
								cands.push_back(p);
						}
					}
				}
				auto evalCand = [&](const Params& p, int* support, int* overlap)
				{
					int sup = 0, ov = 0;
					for (size_t i = 0; i < f.s.size(); ++i)
					{
						double r, pm;
						Residual(p, f.s[i], &r, &pm);
						if (r <= Threshold(f, pm))
						{
							++sup;
							if (inl[i])
								++ov;
						}
					}
					*support = sup;
					*overlap = ov;
				};
				// Cheap pre-selection on a deterministic subset, then full evaluation of the best few.
				std::vector<int> csub;
				if (N > c.stage1Subset && c.stage1Subset > 0)
				{
					const int m = c.stage1Subset;
					csub.resize(static_cast<size_t>(m));
					for (int k = 0; k < m; ++k)
						csub[static_cast<size_t>(k)] = static_cast<int>((static_cast<int64_t>(k) * N + (N >> 1)) / m);
				}
				auto evalSub = [&](const Params& p, int* support, int* overlap)
				{
					int sup = 0, ov = 0;
					const int cnt = csub.empty() ? N : static_cast<int>(csub.size());
					for (int k = 0; k < cnt; ++k)
					{
						const size_t i = static_cast<size_t>(csub.empty() ? k : csub[static_cast<size_t>(k)]);
						double r, pm;
						Residual(p, f.s[i], &r, &pm);
						if (r <= Threshold(f, pm))
						{
							++sup;
							if (inl[i])
								++ov;
						}
					}
					*support = sup;
					*overlap = ov;
				};
				const int keep = 3;
				int topIdx[keep] = { -1, -1, -1 };
				int topSup[keep] = { 0, 0, 0 };
				for (size_t k = 0; k < cands.size(); ++k)
				{
					int sup, ov;
					evalSub(cands[k], &sup, &ov);
					if (!(sup > 0 && ov * 2 < sup))
						continue;
					for (int j = 0; j < keep; ++j)
						if (topIdx[j] < 0 || sup > topSup[j])
						{
							for (int m2 = keep - 1; m2 > j; --m2)
							{
								topIdx[m2] = topIdx[m2 - 1];
								topSup[m2] = topSup[m2 - 1];
							}
							topIdx[j] = static_cast<int>(k);
							topSup[j] = sup;
							break;
						}
				}
				int bestSup = 0;
				Params bestP;
				bool haveBest = false;
				for (int j = 0; j < keep; ++j)
				{
					if (topIdx[j] < 0)
						continue;
					const Params& cp = cands[static_cast<size_t>(topIdx[j])];
					int sup, ov;
					evalCand(cp, &sup, &ov);
					if (sup > 0 && ov * 2 < sup && sup > bestSup)
					{
						bestSup = sup;
						bestP = cp;
						haveBest = true;
					}
				}
				if (haveBest)
				{
					Params q = bestP;
					if (Refine(f, f.s, q, 2))
					{
						int sup, ov;
						evalCand(q, &sup, &ov);
						if (sup > 0 && ov * 2 < sup && sup > bestSup)
							bestSup = sup;
					}
				}
				const double ratio = inlierCount > 0 ? static_cast<double>(bestSup) / inlierCount : 1.0;
				sg.ambiguity = 1.0 - Clamp01(ratio / 0.5);
			}

			// Backward check.
			if (in.bwdX != nullptr && in.bwdY != nullptr && in.bwdTrust != nullptr)
			{
				std::vector<Sample> bs;
				double bTrust = 0;
				for (size_t i = 0; i < f.s.size(); ++i)
				{
					const int b = f.blk[i];
					if (!FiniteF(in.bwdTrust[b]) || !(in.bwdTrust[b] > 0.0f) || !FiniteF(in.bwdX[b]) || !FiniteF(in.bwdY[b]))
						continue;
					Sample s = f.s[i];
					s.dx = in.bwdX[b];
					s.dy = in.bwdY[b];
					s.w = in.bwdTrust[b];
					bs.push_back(s);
					bTrust += s.w;
				}
				if (static_cast<int>(bs.size()) >= std::max(3, MinimalCount(model.type) + 2))
				{
					bool invOk = true;
					Params inv = Invert(model, &invOk);
					inv.type = model.type;
					sg.haveBackward = true;
					if (!invOk)
						sg.inverse = 0.0;
					else
					{
						double inMass = 0;
						for (size_t i = 0; i < bs.size(); ++i)
						{
							double r, pm;
							Residual(inv, bs[i], &r, &pm);
							if (r <= Threshold(f, pm))
								inMass += bs[i].w;
						}
						const double bShare = bTrust > 0 ? inMass / bTrust : 0.0;
						double devTerm = 0.0;
						Params B = inv;
						if (Refine(f, bs, B, 1))
						{
							const double dev = MaxCornerDifference(Compose(B, model), IdentityParams(model.type), f.pic);
							const double tol = 4.0 * f.prec / f.diag;
							devTerm = 1.0 - Clamp01(dev / tol);
						}
						const double shareTerm = std::min(1.0, bShare / (0.7 * sg.inlierShare + 1e-9));
						sg.inverse = Clamp01(devTerm * shareTerm);
					}
				}
			}

			// Temporal.
			SimilarityRates rates;
			const bool haveRates = ToSimilarity(model, in.picture, in.pairSeconds, rates);
			if (haveRates)
			{
				rec.rates = rates;
				if (temporalRead && F.m_temporal.Valid())
					sg.temporal = Clamp01(F.m_temporal.Consistency(in.pairSeconds, rates));
			}

			double logit = c.b[0] + c.b[1] * sg.inlierShare + c.b[2] * sg.precision + c.b[3] * sg.coverage
				+ c.b[4] * sg.temporal + c.b[5] * sg.conditioning + c.b[6] * sg.engineAgreement
				+ c.b[7] * sg.inverse + c.b[8] * sg.ambiguity;
			sg.logit = logit;
			rec.signals = sg;
			rec.confidence = 1.0 / (1.0 + std::exp(-logit));
			rec.params = model;
			rec.valid = true;
			if (sg.ambiguity < 0.35)
				rec.reason = Reason::Ambiguous;
			else if (sg.haveBackward && sg.inverse < 0.3)
				rec.reason = Reason::InverseMismatch;
			else if (rec.confidence < 0.5)
				rec.reason = Reason::LowConfidence;
			else
				rec.reason = Reason::Ok;

			Labels(F, in, f, rec, seed, runObjects);
			return rec;
		}

		static void Labels(Fitter& F, const PairInput& in, const Field& f, CameraMotion& rec, uint64_t seed, bool runObjects)
		{
			const Config& c = F.m_config;
			const int gw = in.gridW, gh = in.gridH;
			const size_t nb = static_cast<size_t>(gw) * static_cast<size_t>(gh);
			auto bd = std::make_shared<BlockData>();
			bd->gridW = gw;
			bd->gridH = gh;
			bd->blockSize = in.blockSize;
			bd->label.assign(nb, static_cast<uint8_t>(Label::Unknown));
			bd->resX.assign(nb, 0.0f);
			bd->resY.assign(nb, 0.0f);
			bd->modelX.assign(nb, 0.0f);
			bd->modelY.assign(nb, 0.0f);
			bd->objectId.assign(nb, 0);
			bd->tinyMover.assign(nb, 0);

			std::vector<uint8_t> trusted(nb, 0), zeroCand(nb, 0);
			std::vector<double> thr(nb, 0.0);
			const bool confident = rec.confidence >= 0.5;
			for (int by = 0; by < gh; ++by)
				for (int bx = 0; bx < gw; ++bx)
				{
					const size_t b = static_cast<size_t>(by) * gw + bx;
					double x, y, dx = 0, dy = 0;
					BlockCentre(bx, by, in.blockSize, in.picture, &x, &y);
					Displacement(rec.params, x, y, &dx, &dy);
					if (!std::isfinite(dx) || !std::isfinite(dy))
						dx = dy = 0;
					bd->modelX[b] = static_cast<float>(dx);
					bd->modelY[b] = static_cast<float>(dy);
					const double mm = std::sqrt(dx * dx + dy * dy);
					thr[b] = std::max(f.absThr, f.rel * mm);
					const bool tr = !f.excluded[b] && FiniteF(in.fwdTrust[b]) && in.fwdTrust[b] > 0.0f
						&& FiniteF(in.fwdX[b]) && FiniteF(in.fwdY[b]);
					trusted[b] = tr ? 1 : 0;
					if (tr)
					{
						bd->resX[b] = static_cast<float>(in.fwdX[b] - dx);
						bd->resY[b] = static_cast<float>(in.fwdY[b] - dy);
						const double vm = std::sqrt(static_cast<double>(in.fwdX[b]) * in.fwdX[b] + static_cast<double>(in.fwdY[b]) * in.fwdY[b]);
						if (confident && vm <= f.absThr && mm > thr[b])
							zeroCand[b] = 1;
					}
				}

			for (int by = 0; by < gh; ++by)
				for (int bx = 0; bx < gw; ++bx)
				{
					const size_t b = static_cast<size_t>(by) * gw + bx;
					if (f.excluded[b])
					{
						bd->label[b] = static_cast<uint8_t>(Label::Overlay);
						continue;
					}
					if (zeroCand[b])
					{
						int nbrs = 0;
						if (bx > 0 && zeroCand[b - 1]) ++nbrs;
						if (bx + 1 < gw && zeroCand[b + 1]) ++nbrs;
						if (by > 0 && zeroCand[b - gw]) ++nbrs;
						if (by + 1 < gh && zeroCand[b + gw]) ++nbrs;
						if (nbrs >= c.overlayZeroClusterMin)
						{
							bd->label[b] = static_cast<uint8_t>(Label::Overlay);
							continue;
						}
					}
					if (in.pixelResidual != nullptr && FiniteF(in.pixelResidual[b]) && in.pixelResidual[b] >= c.pixelResidualMinShare)
					{
						bd->label[b] = static_cast<uint8_t>(Label::Object);
						bd->tinyMover[b] = 1;
						continue;
					}
					if (trusted[b])
					{
						const double rx = bd->resX[b], ry = bd->resY[b];
						bd->label[b] = static_cast<uint8_t>(std::sqrt(rx * rx + ry * ry) <= thr[b] ? Label::Camera : Label::Object);
						continue;
					}
					bool cam = in.texture != nullptr && FiniteF(in.texture[b]) && in.texture[b] <= c.flatTexture;
					if (!cam && FiniteF(in.fwdX[b]) && FiniteF(in.fwdY[b]))
					{
						const double rx = in.fwdX[b] - bd->modelX[b], ry = in.fwdY[b] - bd->modelY[b];
						cam = std::sqrt(rx * rx + ry * ry) <= thr[b];
					}
					bd->label[b] = static_cast<uint8_t>(cam ? Label::Camera : Label::Unknown);
				}

			if (c.objects && runObjects)
			{
				ObjectConfig oc;
				oc.maxObjects = c.maxObjects;
				oc.objectMinShare = c.objectMinShare;
				oc.complexityPenaltyPerParam = c.complexityPenaltyPerParam;
				oc.relThreshold = c.relThreshold;
				oc.absThresholdSigmas = c.absThresholdSigmas;
				ObjectInput oi;
				oi.gridW = gw;
				oi.gridH = gh;
				oi.blockSize = in.blockSize;
				oi.picture = in.picture;
				oi.fwdX = in.fwdX;
				oi.fwdY = in.fwdY;
				oi.trust = in.fwdTrust;
				oi.label = bd->label.data();
				oi.precisionPx = f.prec;
				oi.seed = seed;
				std::vector<uint8_t> ids;
				rec.objects = F.m_objects.Update(oi, oc, ids);
				if (ids.size() == nb)
					bd->objectId = ids;
			}
			rec.blocks = bd;
		}
	};

	Fitter::Fitter(const Config& config) : m_config(config) {}

	void Fitter::Reset()
	{
		m_previous = CameraMotion();
		m_havePrevious = false;
		m_temporal.Reset();
		m_objects.Reset();
		m_resyncGeneration = 0;
		m_rebaseGeneration = 0;
		m_haveGenerations = false;
		m_seedPending = false;
	}

	CameraMotion Fitter::Fit(const PairInput& in)
	{
		const int64_t t0 = m_config.clock ? m_config.clock() : 0;
		CameraMotion rec;
		Impl::FillIdentity(rec, in);
		auto finish = [&](CameraMotion& r)
		{
			r.costMicros = m_config.clock ? (m_config.clock() - t0) : 0;
		};
		try
		{
			if (!(in.pairSeconds > 0) || in.gridW <= 0 || in.gridH <= 0 || in.picture.Empty() || !(in.blockSize > 0)
				|| in.fwdX == nullptr || in.fwdY == nullptr || in.fwdTrust == nullptr)
			{
				rec.valid = false;
				rec.reason = Reason::NoInput;
				finish(rec);
				return rec;
			}

			bool genReset = false;
			if (m_haveGenerations && (in.resyncGeneration != m_resyncGeneration || in.rebaseGeneration != m_rebaseGeneration))
			{
				Reset();
				genReset = true;
			}
			m_resyncGeneration = in.resyncGeneration;
			m_rebaseGeneration = in.rebaseGeneration;
			m_haveGenerations = true;

			if (in.cut)
			{
				m_havePrevious = false;
				m_previous = CameraMotion();
				m_temporal.Reset();
				if (m_config.objects)
					m_objects.Reset();
				m_seedPending = false;
				rec.valid = false;
				rec.reason = Reason::Cut;
				rec.confidence = 0;
				if (in.next != nullptr)
				{
					CameraMotion seed = Impl::Run(*this, *in.next, nullptr, false, m_config.objects, false);
					if (seed.valid)
					{
						rec.valid = true;
						rec.seededFromNext = true;
						rec.reason = Reason::PostCutSeeded;
						rec.params = seed.params;
						if (in.next->pairSeconds != in.pairSeconds && in.next->pairSeconds > 0)
							rec.params = TimeScale(seed.params, in.pairSeconds / in.next->pairSeconds, in.picture);
						rec.rates = seed.rates;
						rec.confidence = seed.confidence;
						rec.signals = seed.signals;
						rec.samples = seed.samples;
						rec.hypotheses = seed.hypotheses;
						rec.objects = seed.objects;
						rec.blocks = seed.blocks;
						m_previous = rec;
						m_havePrevious = true;
						m_temporal.Update(in.next->pairSeconds, seed.rates);
						m_seedPending = true;
					}
				}
				finish(rec);
				return rec;
			}

			if (in.flash)
			{
				rec.valid = false;
				rec.reason = Reason::Flash;
				rec.confidence = 0;
				if (in.next != nullptr && m_havePrevious && m_previous.valid && m_previous.pairSeconds > 0 && in.next->pairSeconds > 0)
				{
					const Params pre = TimeScale(m_previous.params, in.pairSeconds / m_previous.pairSeconds, in.picture);
					const Params preForNext = TimeScale(m_previous.params, in.next->pairSeconds / m_previous.pairSeconds, in.next->picture);
					CameraMotion nx = Impl::Run(*this, *in.next, &preForNext, false, false, false);
					if (nx.valid)
					{
						const Params nextP = TimeScale(nx.params, in.pairSeconds / in.next->pairSeconds, in.picture);
						const double hw = 0.5 * in.picture.Width(), hh = 0.5 * in.picture.Height();
						const double cxs[4] = { -hw, hw, hw, -hw }, cys[4] = { -hh, -hh, hh, hh };
						Sample s[4];
						for (int i = 0; i < 4; ++i)
						{
							double ax, ay, bx, by;
							Displacement(pre, cxs[i], cys[i], &ax, &ay);
							Displacement(nextP, cxs[i], cys[i], &bx, &by);
							s[i].x = cxs[i];
							s[i].y = cys[i];
							s[i].dx = 0.5 * (ax + bx);
							s[i].dy = 0.5 * (ay + by);
							s[i].w = 1;
						}
						const ModelType ty = (pre.type > nextP.type) ? pre.type : nextP.type;
						Params br;
						bool ok = ty != ModelType::None && FitWeighted(ty, s, 4, 0.5 * std::sqrt(4 * hw * hw + 4 * hh * hh), br);
						br.type = ty;
						if (ok && !IsDegenerate(br, in.picture))
						{
							rec.valid = true;
							rec.bridged = true;
							rec.reason = Reason::FlashBridged;
							rec.params = br;
							SimilarityRates rr;
							if (ToSimilarity(br, in.picture, in.pairSeconds, rr))
								rec.rates = rr;
							rec.confidence = std::min(m_previous.confidence, nx.confidence);
							rec.signals = nx.signals;
							rec.samples = nx.samples;
							rec.hypotheses = nx.hypotheses;
						}
					}
				}
				finish(rec);
				return rec;
			}

			Params h0;
			const Params* h0p = nullptr;
			if (m_havePrevious && m_previous.valid && m_previous.pairSeconds > 0 && m_previous.picture == in.picture
				&& m_previous.params.type != ModelType::None)
			{
				h0 = TimeScale(m_previous.params, in.pairSeconds / m_previous.pairSeconds, in.picture);
				h0.type = m_previous.params.type;
				h0p = &h0;
			}
			CameraMotion fit = Impl::Run(*this, in, h0p, true, m_config.objects, genReset);
			if (fit.valid)
			{
				m_previous = fit;
				m_havePrevious = true;
				m_seedPending = false;
				m_temporal.Update(in.pairSeconds, fit.rates);
			}
			fit.inFade = in.fade;
			finish(fit);
			return fit;
		}
		catch (...)
		{
			CameraMotion bad;
			Impl::FillIdentity(bad, in);
			bad.valid = false;
			bad.reason = Reason::NoInput;
			return bad;
		}
	}
}
