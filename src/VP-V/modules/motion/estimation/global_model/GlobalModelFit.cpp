// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// Global camera model: the per-pair fitter (implementation plan section 1, steps 1-11).
// Pure C++14, deterministic: fixed loop orders, double sums, splitmix64, no clocks except the
// injected Config::clock, no unordered containers, no threads.
#include "GlobalModelFit.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <limits>
#include <vector>

#ifdef CAMERA_MODEL_PROFILE
// Test-only per-step profile (defined only by a profiling build of the tests; never in the product).
#include <chrono>
namespace CameraModel
{
	double g_profileMs[8] = {};
	static std::chrono::steady_clock::time_point g_profileLast;
}
#define CM_PROF_RESET() (CameraModel::g_profileLast = std::chrono::steady_clock::now())
#define CM_PROF_MARK(slot) do { const auto cmNow = std::chrono::steady_clock::now(); \
	CameraModel::g_profileMs[slot] += std::chrono::duration<double, std::milli>(cmNow - CameraModel::g_profileLast).count(); \
	CameraModel::g_profileLast = cmNow; } while (0)
#else
#define CM_PROF_RESET() ((void)0)
#define CM_PROF_MARK(slot) ((void)0)
#endif

namespace CameraModel
{
	namespace
	{
		const double kSqrt2 = 1.4142135623730951;

		inline bool FiniteF(float v) { return IsFiniteBits(v); }
		inline double Clamp01(double v) { return v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v); }

		// Expected vector precision of a pair (source px): the caller's, else a quarter px per 16 px of block size.
		inline double PairPrecision(const PairInput& in) { return in.precisionPx > 0 ? in.precisionPx : 0.25 * in.blockSize / 16.0; }

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
			int bx0 = 0, bx1 = 0, by0 = 0, by1 = 0;   // grid rectangle whose block centres lie inside the picture
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

		// Squared agreement threshold from the squared predicted magnitude: max(abs, rel * |pred|)^2.
		inline double Threshold2(const Field& f, double pm2)
		{
			return std::max(f.absThr * f.absThr, f.rel * f.rel * pm2);
		}

		// True when p has no perspective row (h6 = h7 = 0, h8 = 1): the division by w is skipped.
		inline bool AffineRow(const Params& p) { return p.h[6] == 0.0 && p.h[7] == 0.0 && p.h[8] == 1.0; }

		// Model displacement at a point (Displacement(); the affine-row case skips the 1/w).
		inline void Disp(const Params& p, bool affine, double x, double y, double* dx, double* dy)
		{
			if (affine)
			{
				*dx = p.h[0] * x + p.h[1] * y + p.h[2] - x;
				*dy = p.h[3] * x + p.h[4] * y + p.h[5] - y;
			}
			else
				Displacement(p, x, y, dx, dy);
		}

		// Squared residual and squared predicted magnitude (no square roots).
		inline void ResidualSq(const Params& p, bool affine, const Sample& s, double* r2, double* pm2)
		{
			double dx, dy;
			Disp(p, affine, s.x, s.y, &dx, &dy);
			const double ex = s.dx - dx, ey = s.dy - dy;
			*r2 = ex * ex + ey * ey;
			*pm2 = dx * dx + dy * dy;
		}

		// erfc on [0, 3/sqrt(2)] from a fixed table with linear interpolation (abs error < 1e-6; built once,
		// deterministic). The MAGSAC++-style weight only needs it below 3 sigma.
		const int kErfcTable = 4096;
		const double kErfcMax = 3.0 / kSqrt2;
		struct ErfcTable
		{
			double v[kErfcTable + 2];
			ErfcTable() { for (int i = 0; i <= kErfcTable + 1; ++i) v[i] = std::erfc(kErfcMax * i / kErfcTable); }
		};
		inline double ErfcFast(double x)
		{
			static const ErfcTable t;
			const double u = x * (kErfcTable / kErfcMax);
			if (!(u < kErfcTable))
				return t.v[kErfcTable];
			const int i = static_cast<int>(u);
			const double a = u - i;
			return t.v[i] + a * (t.v[i + 1] - t.v[i]);
		}

		// MAGSAC++-style sigma-marginalised weight: 1 at 0, 0 beyond 3 sigma (squared inputs).
		inline double MagW2(double r2, double sigma2)
		{
			if (!(r2 < 9.0 * sigma2))
				return 0.0;
			return ErfcFast(std::sqrt(r2 / (2.0 * sigma2)));
		}

		Score ScoreOn(const Field& f, const Params& p, const int* idx, int n)
		{
			Score sc;
			const bool aff = AffineRow(p);
			const int count = idx ? n : static_cast<int>(f.s.size());
			for (int k = 0; k < count; ++k)
			{
				const Sample& s = f.s[idx ? idx[k] : k];
				double r2, pm2;
				ResidualSq(p, aff, s, &r2, &pm2);
				const double t2 = Threshold2(f, pm2);
				if (r2 <= t2)
					++sc.inliers;
				sc.cost += s.w * ((r2 < t2) ? r2 : t2);
			}
			return sc;
		}

		void BuildWeighted(const Field& f, const std::vector<Sample>& samples, const Params& p, std::vector<Sample>& out)
		{
			out.clear();
			const bool aff = AffineRow(p);
			for (size_t i = 0; i < samples.size(); ++i)
			{
				double r2, pm2;
				ResidualSq(p, aff, samples[i], &r2, &pm2);
				const double w = samples[i].w * MagW2(r2, Threshold2(f, pm2));
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
				// Converged: the step moved no picture corner by more than 1% of the vector precision; further
				// iterations would change the model by less than that, so they are skipped (a stopping rule,
				// not a fixed point).
				const bool converged = MaxCornerDifference(q, p, f.pic) * f.diag <= 0.01 * f.prec;
				p = q;
				any = true;
				if (converged)
					break;
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
			f.prec = PairPrecision(in);
			const double W = in.picture.Width(), H = in.picture.Height();
			f.diag = std::sqrt(W * W + H * H);
			f.halfDiag = 0.5 * f.diag;
			f.absThr = c.absThresholdSigmas * f.prec;
			f.rel = c.relThreshold;
			f.s.clear();
			f.blk.clear();
			f.trustSum = 0;
			f.cellsWithSample = 0;

			const int nb = in.gridW * in.gridH;
			const double bs = in.blockSize;
			// Blocks whose centre lies inside the picture form one rectangle of the grid (bars lie outside it).
			// The ranges are estimated and then fixed up with the exact per-centre predicate.
			auto inside = [&](int bx, int by)
			{
				const double cx = (bx + 0.5) * bs, cy = (by + 0.5) * bs;
				return !(cx < in.picture.left || cx >= in.picture.right || cy < in.picture.top || cy >= in.picture.bottom);
			};
			auto colOk = [&](int bx) { const double cx = (bx + 0.5) * bs; return !(cx < in.picture.left || cx >= in.picture.right); };
			auto rowOk = [&](int by) { const double cy = (by + 0.5) * bs; return !(cy < in.picture.top || cy >= in.picture.bottom); };
			(void)inside;
			int bx0 = std::min(std::max(static_cast<int>(std::floor(in.picture.left / bs)), 0), in.gridW);
			int bx1 = std::min(std::max(static_cast<int>(std::ceil(in.picture.right / bs)), 0), in.gridW);
			int by0 = std::min(std::max(static_cast<int>(std::floor(in.picture.top / bs)), 0), in.gridH);
			int by1 = std::min(std::max(static_cast<int>(std::ceil(in.picture.bottom / bs)), 0), in.gridH);
			while (bx0 < bx1 && !colOk(bx0)) ++bx0;
			while (bx1 > bx0 && !colOk(bx1 - 1)) --bx1;
			while (by0 < by1 && !rowOk(by0)) ++by0;
			while (by1 > by0 && !rowOk(by1 - 1)) --by1;
			f.excluded.assign(static_cast<size_t>(nb), 1);
			f.bx0 = bx0; f.bx1 = bx1; f.by0 = by0; f.by1 = by1;

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
			std::vector<int> colCell(static_cast<size_t>(std::max(0, bx1 - bx0)));
			for (int bx = bx0; bx < bx1; ++bx)
			{
				const double fx = (bx + 0.5) * bs - in.picture.left;
				colCell[static_cast<size_t>(bx - bx0)] = std::min(std::max(static_cast<int>(std::floor(fx / cw)), 0), cellsX - 1);
			}
			for (int by = by0; by < by1; ++by)
			{
				const double fy = (by + 0.5) * bs - in.picture.top;
				const int cy = std::min(std::max(static_cast<int>(std::floor(fy / ch)), 0), cellsY - 1);
				const int rowCell = cy * cellsX;
				const int rowBase = by * in.gridW;
				uint8_t* const exRow = f.excluded.data() + rowBase;
				const uint8_t* const guardRow = in.textGuard != nullptr ? in.textGuard + rowBase : nullptr;
				const float* const trustRow = in.fwdTrust + rowBase;
				const int* const cc = colCell.data();
				int* const bestP = best.data();
				float* const bestT = bestTrust.data();
				for (int bx = bx0; bx < bx1; ++bx)
				{
					if (guardRow != nullptr && guardRow[bx] == 1)
						continue;                              // excluded stays 1
					exRow[bx] = 0;
					const float tr = trustRow[bx];
					if (!(tr > 0.0f))
						continue;                              // also rejects NaN
					const int ci = rowCell + cc[bx - bx0];
					// Validity (finite trust and vector) is tested only for a block that would win its cell;
					// an invalid block never wins, so the chosen samples are the same as testing every block.
					if ((bestP[ci] < 0 || tr > bestT[ci]) && FiniteF(tr))
					{
						const int b = rowBase + bx;
						if (FiniteF(in.fwdX[b]) && FiniteF(in.fwdY[b]))
						{
							bestP[ci] = b;
							bestT[ci] = tr;
						}
					}
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

		// Temporal noise floor per similarity-rate component (per second): one vector precision as a
		// translation, and as scale / angle at a quarter of the picture diagonal.
		void TemporalFloor(double prec, const Rect& pic, double pairSeconds, double* out)
		{
			const double W = pic.Width(), H = pic.Height();
			const double r = 0.25 * std::sqrt(W * W + H * H);
			const double t = pairSeconds > 0 ? pairSeconds : 1.0;
			out[0] = out[1] = (r > 0 ? prec / r : 0.0) / t;
			out[2] = out[3] = prec / t;
		}

		struct ChosenModel
		{
			Params model;
			bool ok = false;
		};

		// Local optimisation + model choice from a start model (the RANSAC winner, or a competing layer's model): refine,
		// build the support set, then the complexity-penalised ladder. Every layer goes through this one function.
		ChosenModel ChooseModel(const Field& f, const Config& c, const Params& start, bool earlyExit, ModelType maxModel, int irls)
		{
			Params w = start;
			if (!earlyExit)
				Refine(f, f.s, w, irls);          // the early-exit winner (refined H0) is already refined
			std::vector<Sample> wbuf;
			BuildWeighted(f, f.s, w, wbuf);
			if (static_cast<int>(wbuf.size()) < 1)
				wbuf = f.s;

			// Support set of the winner (for the cost comparison) and its quadrant mass.
			std::vector<uint8_t> support(f.s.size(), 0);
			double tiny = 0;
			const bool wAff = AffineRow(w);
			for (size_t i = 0; i < f.s.size(); ++i)
			{
				double r2, pm2;
				ResidualSq(w, wAff, f.s[i], &r2, &pm2);
				if (r2 <= 9.0 * Threshold2(f, pm2))
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
				const bool aff = AffineRow(p);
				for (size_t i = 0; i < f.s.size(); ++i)
					if (support[i])
					{
						double r2, pm2;
						ResidualSq(p, aff, f.s[i], &r2, &pm2);
						const double t2 = 9.0 * Threshold2(f, pm2);
						cost += f.s[i].w * ((r2 < t2) ? r2 : t2);
					}
				return cost;
			};

			static const ModelType kAll[4] = { ModelType::Translation, ModelType::Similarity, ModelType::Affine, ModelType::Homography };
			Params chosen;
			bool haveChosen = false;
			double chosenCost = 0;
			// Early exit: keep H0's (refined) type and try ONE step up from the same weighted set under the same
			// complexity-penalty test; otherwise run the whole ladder from translation.
			int tiFirst = 0, tiLast = 3;
			if (earlyExit)
			{
				tiFirst = static_cast<int>(w.type) - static_cast<int>(ModelType::Translation);
				tiLast = std::min(3, tiFirst + 1);
				chosen = w;
				chosenCost = supportCost(w);
				haveChosen = true;
			}
			for (int ti = tiFirst + (earlyExit ? 1 : 0); ti <= tiLast; ++ti)
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
			// Early exit, step down: the loop above only tries one step UP from H0's type, so a model that was a
			// homography would never relax when the motion becomes a pure pan. When the type is still H0's and
			// above Translation, fit ONE type down from the same weighted set; the lower type wins when the
			// higher one does not earn its extra parameters under the same complexity-penalty test.
			if (c.stepDownOnEarlyExit && earlyExit && haveChosen && chosen.type == w.type && chosen.type > ModelType::Translation)
			{
				const ModelType lowerTy = kAll[static_cast<int>(chosen.type) - static_cast<int>(ModelType::Translation) - 1];
				Params lp;
				if (static_cast<int>(wbuf.size()) >= MinimalCount(lowerTy) &&
					FitWeighted(lowerTy, wbuf.data(), static_cast<int>(wbuf.size()), f.halfDiag, lp))
				{
					lp.type = lowerTy;
					if (!IsDegenerate(lp, f.pic))
					{
						Refine(f, f.s, lp, irls);
						if (!IsDegenerate(lp, f.pic))
						{
							const double costLower = supportCost(lp);
							const double extra = ParamCount(chosen.type) - ParamCount(lowerTy);
							if ((costLower - chosenCost) / std::max(chosenCost, tiny) <= c.complexityPenaltyPerParam * extra)
							{
								chosen = lp;
								chosenCost = costLower;
							}
						}
					}
				}
			}
			ChosenModel r;
			r.ok = haveChosen;
			r.model = chosen;
			return r;
		}

		// One layer's model, inlier set and the signals that depend only on them.
		struct Cand
		{
			Params model;
			std::vector<uint8_t> inl;
			double inlierMass = 0, rawShare = 0;
			int inlierCount = 0;
			Signals sg;
		};

		void BuildCand(const Field& f, const Config& c, const PairInput& in, int cellsTotal, const Params& model, Cand& cd)
		{
			std::vector<uint8_t> inl(f.s.size(), 0);
			double inlierMass = 0, resSq = 0;
			int inlierCount = 0;
			const bool mAff = AffineRow(model);
			for (size_t i = 0; i < f.s.size(); ++i)
			{
				double r2, pm2;
				ResidualSq(model, mAff, f.s[i], &r2, &pm2);
				if (r2 <= Threshold2(f, pm2))
				{
					inl[i] = 1;
					++inlierCount;
					inlierMass += f.s[i].w;
					resSq += f.s[i].w * r2;
				}
			}
			Signals sg;
			// Signal: the trusted share of vectors the camera explains, 0 at inlierShareLo, full at inlierShareHi.
			// A camera behind many movers (a crowd under a pan) explains about half the vectors and is still the
			// dominant motion when no other motion competes; a competing motion is the ambiguity signal's job.
			const double rawShare = f.trustSum > 0 ? Clamp01(inlierMass / f.trustSum) : 0.0;
			{
				const double lo = c.inlierShareLo, span = std::max(1e-6, c.inlierShareHi - c.inlierShareLo);
				sg.inlierShare = f.trustSum > 0 ? Clamp01((inlierMass / f.trustSum - lo) / span) : 0.0;
			}
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
			cd.model = model;
			cd.inl.swap(inl);
			cd.inlierMass = inlierMass;
			cd.inlierCount = inlierCount;
			cd.rawShare = rawShare;
			cd.sg = sg;
		}

		// Two-layer state changes of one pair, applied by Fitter::Fit only when the pair's record is kept.
		struct TlNext
		{
			bool have = false;
			bool swapped = false;                    // the camera layer changed: the reference follows it at once
			int holdCount = 0;
			int swapAge = -1;
			double camShare = 0;
			bool haveObj = false;
			Params obj;
			uint8_t objId = 0;
			uint8_t nextId = 1;
		};
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

		// Backward check, temporal consistency, confidence and verdict of one layer's candidate fit.
		static void Finish(Fitter& F, const PairInput& in, const Config& c, const Field& f, bool temporalRead, Cand& cd, double ambiguity, CameraMotion& rec)
		{
			Signals sg = cd.sg;
			sg.ambiguity = ambiguity;
			const Params& model = cd.model;
			const double rawShare = cd.rawShare;
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
						const bool iAff = AffineRow(inv);
						for (size_t i = 0; i < bs.size(); ++i)
						{
							double r2, pm2;
							ResidualSq(inv, iAff, bs[i], &r2, &pm2);
							if (r2 <= Threshold2(f, pm2))
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
						const double shareTerm = std::min(1.0, bShare / (0.7 * rawShare + 1e-9));
						sg.inverse = Clamp01(devTerm * shareTerm);
					}
				}
			}

			SimilarityRates rates;
			const bool haveRates = ToSimilarity(model, in.picture, in.pairSeconds, rates);
			if (haveRates)
			{
				rec.rates = rates;
				if (temporalRead && F.m_temporal.Valid())
				{
					double fl[4];
					TemporalFloor(f.prec, in.picture, in.pairSeconds, fl);
					sg.temporal = Clamp01(F.m_temporal.Consistency(in.pairSeconds, rates, c.temporalNoiseFloor ? fl : nullptr));
				}
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
			// A failed verdict is never a usable fit: an Ambiguous record (a competing motion with about a third
			// or more of the camera's support) or an InverseMismatch record keeps its model for diagnostics, but
			// its confidence is halved and scaled by how far the signal fell below its gate, so it stays < 0.5.
			if (c.verdictGatesConfidence && (rec.reason == Reason::Ambiguous || rec.reason == Reason::InverseMismatch))
			{
				const double g = rec.reason == Reason::Ambiguous ? Clamp01(sg.ambiguity / 0.35) : Clamp01(sg.inverse / 0.3);
				rec.confidence *= 0.5 * g;
			}
		}

		// Two-layer state after one pair (see Config::twoLayer). The reference follows usable records (and any accepted
		// layer change); it ages out after 2M pairs without a refresh.
		static void ApplyTl(Fitter& F, const PairInput& in, const CameraMotion& fit, const TlNext& n)
		{
			TwoLayerState& s = F.m_tl;
			const int M = std::max(1, static_cast<int>(std::lround(F.m_config.layerSwapSeconds / in.pairSeconds)));
			const bool ran = n.have && fit.valid;
			if (ran)
			{
				s.holdCount = n.holdCount;
				s.swapAge = n.swapAge;
				s.haveObj = n.haveObj;
				if (n.haveObj)
				{
					s.obj = n.obj;
					s.objSeconds = in.pairSeconds;
					s.objId = n.objId;
				}
				s.nextId = n.nextId;
			}
			const bool usable = fit.valid && fit.reason == Reason::Ok && fit.confidence >= 0.5;
			if (ran && (usable || n.swapped))
			{
				s.haveRef = true;
				s.ref = fit.params;
				s.refSeconds = in.pairSeconds;
				s.refShare = n.camShare;
				s.age = 0;
			}
			else if (s.haveRef && ++s.age > 2 * M)
				s.haveRef = false;
		}

		// Fits one pair. Reads (never writes) the Fitter's history; objects are updated only when asked.
		static CameraMotion Run(Fitter& F, const PairInput& in, const Params* h0, bool temporalRead, bool runObjects, bool genReset, TlNext* tlOut)
		{
			const Config& c = F.m_config;
			CameraMotion rec;
			FillIdentity(rec, in);
			rec.valid = false;
			rec.reason = Reason::NoInput;
			rec.confidence = 0;

			CM_PROF_RESET();
			Field f;
			struct ScratchGuard
			{
				Fitter& F;
				Field& f;
				ScratchGuard(Fitter& fitter, Field& field) : F(fitter), f(field)
				{
					f.s.swap(F.m_scratchSamples);
					f.blk.swap(F.m_scratchBlk);
					f.excluded.swap(F.m_scratchExcluded);
				}
				~ScratchGuard()
				{
					f.s.swap(F.m_scratchSamples);
					f.blk.swap(F.m_scratchBlk);
					f.excluded.swap(F.m_scratchExcluded);
				}
			} scratchGuard(F, f);
			if (!BuildField(in, c, f))
				return rec;
			CM_PROF_MARK(0);
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
			}
			rec.hypotheses = static_cast<uint32_t>(count);
			CM_PROF_MARK(1);

			if (!haveWinner)
			{
				rec.reason = deadlineHit ? Reason::DeadlineMissed : (genReset ? Reason::GenerationReset : Reason::Degenerate);
				return rec;
			}

			// ---- Local optimisation + model choice (layer A: the RANSAC winner) ----
			const ChosenModel chA = ChooseModel(f, c, winner, earlyExit, maxModel, irls);
			if (!chA.ok)
			{
				rec.reason = Reason::Degenerate;
				return rec;
			}
			CM_PROF_MARK(2);

			// ---- Inliers, signals ----
			Cand cdA;
			BuildCand(f, c, in, cellsTotal, chA.model, cdA);
			const std::vector<uint8_t>& inl = cdA.inl;
			const int inlierCount = cdA.inlierCount;
			const double inlierMass = cdA.inlierMass;
			CM_PROF_MARK(3);
			double ambA = 1.0;
			int bestSup = 0;
			Params bestP;
			bool haveBest = false;
			// Ambiguity: best disjoint-ish competitor.
			{
				std::vector<int> nonIn;
				for (size_t i = 0; i < f.s.size(); ++i)
					if (!inl[i])
						nonIn.push_back(static_cast<int>(i));
				std::vector<Params> cands;
				const double nonInlierShare = f.trustSum > 0 ? 1.0 - inlierMass / f.trustSum : 0.0;
				const bool searchCompetitor = nonInlierShare >= 2.0 * c.objectMinShare;
				if (searchCompetitor && nonIn.size() >= 2)
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
					const bool aff = AffineRow(p);
					for (size_t i = 0; i < f.s.size(); ++i)
					{
						double r2, pm2;
						ResidualSq(p, aff, f.s[i], &r2, &pm2);
						if (r2 <= Threshold2(f, pm2))
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
					const bool aff = AffineRow(p);
					for (int k = 0; k < cnt; ++k)
					{
						const size_t i = static_cast<size_t>(csub.empty() ? k : csub[static_cast<size_t>(k)]);
						double r2, pm2;
						ResidualSq(p, aff, f.s[i], &r2, &pm2);
						if (r2 <= Threshold2(f, pm2))
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
						{
							bestSup = sup;
							bestP = q;                          // the refined competitor (second layer, Config::twoLayer)
						}
					}
				}
				const double ratio = inlierCount > 0 ? static_cast<double>(bestSup) / inlierCount : 1.0;
				ambA = searchCompetitor ? 1.0 - Clamp01(ratio / 0.5) : 1.0;
			}

			CM_PROF_MARK(4);

			Cand* cam = &cdA;
			Cand cdB;
			double ambiguity = ambA;
			TlNext tl;
			uint8_t flags = 0;
			double layerRisk = 0;
			bool haveObj = false;
			ObjectModel objLayer;
			if (c.twoLayer)
			{
				flags = 1;
				const TwoLayerState& st = F.m_tl;
				const int M = std::max(1, static_cast<int>(std::lround(c.layerSwapSeconds / in.pairSeconds)));
				const int ringX = std::max(1, static_cast<int>(std::lround(c.borderRingFraction * (f.bx1 - f.bx0))));
				const int ringY = std::max(1, static_cast<int>(std::lround(c.borderRingFraction * (f.by1 - f.by0))));
				std::vector<uint8_t> ringMask(f.s.size(), 0);
				double allMass = 0, allRing = 0;
				for (size_t i = 0; i < f.s.size(); ++i)
				{
					const int b = f.blk[i];
					const int bx = b % f.gridW, by = b / f.gridW;
					const bool r = bx < f.bx0 + ringX || bx >= f.bx1 - ringX || by < f.by0 + ringY || by >= f.by1 - ringY;
					ringMask[i] = r ? 1 : 0;
					allMass += f.s[i].w;
					if (r)
						allRing += f.s[i].w;
				}
				const double allRingShare = allMass > 0 ? allRing / allMass : 0.0;

				// Continuity reference: the last usable camera layer, time-scaled to this pair.
				const bool haveRef = st.haveRef && st.refSeconds > 0;
				Params refScaled;
				double refMax = 0, floorPx = 0;
				if (haveRef)
				{
					refScaled = TimeScale(st.ref, in.pairSeconds / st.refSeconds, in.picture);
					const double hw = 0.5 * in.picture.Width(), hh = 0.5 * in.picture.Height();
					const double cxs[4] = { -hw, hw, hw, -hw }, cys[4] = { -hh, -hh, hh, hh };
					for (int k = 0; k < 4; ++k)
					{
						double dx, dy;
						Displacement(refScaled, cxs[k], cys[k], &dx, &dy);
						refMax = std::max(refMax, std::hypot(dx, dy));
					}
					double fl[4];
					TemporalFloor(f.prec, in.picture, in.pairSeconds, fl);
					floorPx = fl[2] * in.pairSeconds;   // reduces to f.prec, so tol = 5 * prec + 0.25 * refMax
				}

				struct Layer
				{
					bool present = false;
					Params p;
					int count = 0;
					double mass = 0, border = 1, share = 0, f = 1, a = 1, score = 0;
					const std::vector<uint8_t>* mask = nullptr;
				};
				Layer L[2];
				auto fillLayer = [&](Layer& Ly, const Params& p, const std::vector<uint8_t>& mask, int count)
				{
					Ly.present = true;
					Ly.p = p;
					Ly.count = count;
					Ly.mask = &mask;
					double mass = 0, ring = 0;
					for (size_t i = 0; i < f.s.size(); ++i)
						if (mask[i])
						{
							mass += f.s[i].w;
							if (ringMask[i])
								ring += f.s[i].w;
						}
					Ly.mass = mass;
					Ly.border = allRingShare > 0 ? (mass > 0 ? (ring / mass) / allRingShare : 0.0) : 1.0;
					Ly.share = f.trustSum > 0 ? Clamp01(mass / f.trustSum) : 0.0;
					if (haveRef)
					{
						const double d = MaxCornerDifference(p, refScaled, f.pic) * f.diag;
						const double tol = 4.0 * f.prec + 0.25 * refMax + floorPx;
						const double q = d / tol;
						Ly.f = std::max(c.continuityFloor, std::exp(-q * q));
						Ly.a = 1.0 - 0.5 * Clamp01(std::fabs(Ly.share - st.refShare) / std::max(st.refShare, 0.1));
					}
					Ly.score = Ly.count * Ly.f * (0.25 + 0.75 * Clamp01(Ly.border)) * Ly.a;
				};
				fillLayer(L[0], cdA.model, cdA.inl, cdA.inlierCount);
				std::vector<uint8_t> maskB;
				if (haveBest)
				{
					maskB.assign(f.s.size(), 0);
					int cntB = 0;
					const bool bAff = AffineRow(bestP);
					for (size_t i = 0; i < f.s.size(); ++i)
					{
						double r2, pm2;
						ResidualSq(bestP, bAff, f.s[i], &r2, &pm2);
						if (r2 <= Threshold2(f, pm2))
						{
							maskB[i] = 1;
							++cntB;
						}
					}
					if (cdA.inlierCount > 0 && cntB >= c.competitorMinRatio * cdA.inlierCount)
						fillLayer(L[1], bestP, maskB, cntB);
				}

				int ci = 0;
				bool held = false, swapped = false, riskSwap = false;
				int newHold = 0;
				if (L[1].present)
				{
					flags |= 2;
					const int win = L[1].score > L[0].score ? 1 : 0;
					const double ws = L[win].score, ls = L[1 - win].score;
					(void)ws; (void)ls;
					ci = win;
					if (haveRef && L[win].f < 0.5)
					{
						if (L[1 - win].f >= 0.5)
						{
							newHold = st.holdCount + 1;
							if (newHold >= M)
							{
								swapped = riskSwap = true;
								newHold = 0;
							}
							else
							{
								ci = 1 - win;
								held = true;
							}
						}
						else
							swapped = true;      // the reference layer is gone: a real change of camera motion, accepted at once
					}
				}
				if (ci == 1)
				{
					const ChosenModel chB = ChooseModel(f, c, bestP, false, maxModel, irls);
					if (chB.ok)
					{
						BuildCand(f, c, in, cellsTotal, chB.model, cdB);
						cam = &cdB;
						flags |= 4;
					}
					else
					{
						ci = 0;
						held = swapped = riskSwap = false;
						newHold = 0;
					}
				}
				// Ambiguity. Continuity may CHOOSE the layer but never makes the record confident: the score ratio is taken
				// without the continuity factor, and a record on layer A is never more lenient than the support-based value.
				if (L[1].present)
				{
					const double sa = L[0].count * (0.25 + 0.75 * Clamp01(L[0].border)) * L[0].a;
					const double sb = L[1].count * (0.25 + 0.75 * Clamp01(L[1].border)) * L[1].a;
					const double hi = std::max(sa, sb), lo = std::min(sa, sb);
					const double scoreAmb = hi > 0 ? 1.0 - Clamp01((lo / hi) / 0.5) : 0.0;
					ambiguity = ci == 1 ? scoreAmb : std::min(ambiguity, scoreAmb);
				}
				// ForegroundSuspect: the FINAL camera layer touches no border, explains < 3/4 of the vectors and has no agreeing reference.
				if (L[ci].border < c.borderVetoRatio && L[ci].share < 0.75 && !(haveRef && L[ci].f >= 0.8))
				{
					flags |= 8;
					ambiguity = std::min(ambiguity, 0.34 * Clamp01(L[ci].border / c.borderVetoRatio));
				}
				if (held)
					flags |= 16;
				if (swapped)
					flags |= 32;
				if (haveRef)
					flags |= 64;
				cam->sg.border = Clamp01(L[ci].border);
				cam->sg.continuity = haveRef ? L[ci].f : 0.5;

				int newSwapAge = -1;
				if (riskSwap)
					newSwapAge = 0;
				else if (st.swapAge >= 0 && st.swapAge + 1 < M)
					newSwapAge = st.swapAge + 1;
				if (newSwapAge >= 0)
					layerRisk = 1.0 - static_cast<double>(newSwapAge) / M;

				// The other layer (or the suspect single layer) is the object layer.
				const Layer* ob = L[1 - ci].present ? &L[1 - ci] : ((flags & 8) ? &L[0] : nullptr);
				tl.have = true;
				tl.swapped = swapped;
				tl.holdCount = newHold;
				tl.swapAge = newSwapAge;
				tl.camShare = cam->rawShare;
				tl.nextId = st.nextId;
				if (ob != nullptr)
				{
					haveObj = true;
					double sw = 0, mx = 0, my = 0;
					for (size_t i = 0; i < f.s.size(); ++i)
						if ((*ob->mask)[i])
						{
							sw += f.s[i].w;
							mx += f.s[i].w * f.s[i].x;
							my += f.s[i].w * f.s[i].y;
						}
					objLayer.type = ob->p.type;
					for (int k = 0; k < 9; ++k)
						objLayer.h[k] = ob->p.h[k];
					objLayer.blocks = static_cast<uint32_t>(ob->count);
					objLayer.cx = sw > 0 ? mx / sw : 0.0;
					objLayer.cy = sw > 0 ? my / sw : 0.0;
					bool cont = false;
					if (st.haveObj && st.objSeconds > 0)
					{
						const Params pr = TimeScale(st.obj, in.pairSeconds / st.objSeconds, in.picture);
						const double hw = 0.5 * in.picture.Width(), hh = 0.5 * in.picture.Height();
						const double cxs[4] = { -hw, hw, hw, -hw }, cys[4] = { -hh, -hh, hh, hh };
						double pmax = 0;
						for (int k = 0; k < 4; ++k)
						{
							double dx, dy;
							Displacement(pr, cxs[k], cys[k], &dx, &dy);
							pmax = std::max(pmax, std::hypot(dx, dy));
						}
						const double d = MaxCornerDifference(ob->p, pr, f.pic) * f.diag;
						cont = d <= 5.0 * f.prec + 0.25 * pmax;
					}
					if (cont)
						objLayer.id = st.objId;
					else
					{
						objLayer.id = st.nextId;
						tl.nextId = st.nextId >= 255 ? 1 : static_cast<uint8_t>(st.nextId + 1);
					}
					tl.haveObj = true;
					tl.obj = ob->p;
					tl.objId = objLayer.id;
				}
			}
			Finish(F, in, c, f, temporalRead, *cam, ambiguity, rec);
			if (c.twoLayer)
			{
				if (layerRisk > 0)
					rec.confidence *= 1.0 - 0.5 * layerRisk;
				if (rec.reason == Reason::Ok && rec.confidence < 0.5)
					rec.reason = Reason::LowConfidence;
				rec.layerFlags = flags;
				rec.layerRisk = layerRisk;
				rec.haveObjectLayer = haveObj;
				if (haveObj)
					rec.objectLayer = objLayer;
				if (tlOut != nullptr)
					*tlOut = tl;
			}

			CM_PROF_MARK(6);
			Labels(F, in, f, rec, seed, runObjects);
			CM_PROF_MARK(7);
			return rec;
		}

		static void Labels(Fitter& F, const PairInput& in, const Field& f, CameraMotion& rec, uint64_t seed, bool runObjects)
		{
			const Config& c = F.m_config;
			const int gw = in.gridW, gh = in.gridH;
			const size_t nb = static_cast<size_t>(gw) * static_cast<size_t>(gh);
			// A pooled buffer is reused only when the pool holds the sole reference (no record, consumer or
			// copy still points at it; nothing takes weak references). The acquire fence orders our writes
			// after the last holder's reads. Every element below is overwritten.
			std::shared_ptr<BlockData> bd;
			for (size_t i = 0; i < F.m_blockPool.size(); ++i)
				if (F.m_blockPool[i].use_count() == 1)
				{
					std::atomic_thread_fence(std::memory_order_acquire);
					bd = F.m_blockPool[i];
					break;
				}
			if (!bd)
			{
				bd = std::make_shared<BlockData>();
				const size_t kPool = 4;
				if (F.m_blockPool.size() < kPool)
					F.m_blockPool.push_back(bd);
			}
			bd->gridW = gw;
			bd->gridH = gh;
			bd->blockSize = in.blockSize;
			if (!c.cpuLabels && !c.objects)
			{
				// The GPU label pass produces label / residual / model / tiny; the arrays stay empty (size 0), which
				// the accessor and the search hints treat as "no model for this block" (today's behaviour).
				bd->label.clear();
				bd->resX.clear();
				bd->resY.clear();
				bd->modelX.clear();
				bd->modelY.clear();
				bd->objectId.clear();
				bd->tinyMover.clear();
				bd->excluded = f.excluded;
				bd->absThresholdPx = static_cast<float>(f.absThr);
				bd->relThreshold = static_cast<float>(f.rel);
				rec.labelsOnGpu = true;
				rec.blocks = bd;
				return;
			}
			bd->label.resize(nb);                   // all written by labelRow
			bd->resX.resize(nb);                    // resX/resY/modelX/modelY/tinyMover: all written below
			bd->resY.resize(nb);
			bd->modelX.resize(nb);
			bd->modelY.resize(nb);
			bd->objectId.assign(nb, 0);
			bd->tinyMover.resize(nb);
			bd->excluded = f.excluded;
			bd->absThresholdPx = static_cast<float>(f.absThr);
			bd->relThreshold = static_cast<float>(f.rel);

			// One pass over the blocks computes the model, residual and label of every block. The zero-motion
			// cluster test (Overlay, which takes precedence over every other label) needs the neighbours'
			// candidacy, so candidates are listed and resolved after the pass; a candidate that is not an
			// overlay keeps the label the pass gave it. All distance tests use squares.
			std::vector<uint8_t>& zeroCand = F.m_scratchZero;
			zeroCand.assign(nb, 0);
			std::vector<int> zeroList;
			const bool confident = rec.confidence >= 0.5;
			const double absThr2 = f.absThr * f.absThr, rel2 = f.rel * f.rel;
			const Params& P = rec.params;
			const bool persp = !AffineRow(P);
			const double bs = in.blockSize;
			const double cx0 = 0.5 * bs - in.picture.CentreX(), cy0 = 0.5 * bs - in.picture.CentreY();

			// Raw pointers hoisted out of the loop: byte stores would otherwise force the compiler to reload
			// every vector's data pointer after each label write (uint8_t aliases everything).
			uint8_t* const lab = bd->label.data();
			uint8_t* const tiny = bd->tinyMover.data();
			float* const resX = bd->resX.data();
			float* const resY = bd->resY.data();
			float* const modX = bd->modelX.data();
			float* const modY = bd->modelY.data();
			uint8_t* const zc = zeroCand.data();
			const uint8_t* const excl = f.excluded.data();
			const float* const fx = in.fwdX;
			const float* const fy = in.fwdY;
			const float* const ft = in.fwdTrust;
			const float* const pres = in.pixelResidual;
			const float* const tex = in.texture;
			const double pixMin = c.pixelResidualMinShare, flat = c.flatTexture;
			const size_t gws = static_cast<size_t>(gw);

			for (int by = 0; by < gh; ++by)
			{
				const size_t row = static_cast<size_t>(by) * gws;
				// Row start: numerators u, v and denominator w of the model at the first block centre; the
				// per-block step along the row is constant (no perspective row) or applied to the numerators
				// and the denominator (perspective: one division per block).
				const double y = cy0 + by * bs;
				double x = cx0;
				double u = P.h[0] * x + P.h[1] * y + P.h[2];
				double v = P.h[3] * x + P.h[4] * y + P.h[5];
				double w = P.h[6] * x + P.h[7] * y + P.h[8];
				const double du = P.h[0] * bs, dv = P.h[3] * bs, dw = P.h[6] * bs;
				for (int bx = 0; bx < gw; ++bx, x += bs, u += du, v += dv, w += dw)
				{
					const size_t b = row + static_cast<size_t>(bx);
					double dx, dy;
					if (persp)
					{
						const double iw = (w != 0.0) ? 1.0 / w : 0.0;
						dx = u * iw - x;
						dy = v * iw - y;
					}
					else
					{
						dx = u - x;
						dy = v - y;
					}
					if (!IsFiniteBits(dx) || !IsFiniteBits(dy))
						dx = dy = 0;
					const float mxf = static_cast<float>(dx), myf = static_cast<float>(dy);
					modX[b] = mxf;
					modY[b] = myf;
					const double mm2 = dx * dx + dy * dy;
					const double thr2 = std::max(absThr2, rel2 * mm2);
					const float vx = fx[b], vy = fy[b], vt = ft[b];
					const bool ex = excl[b] != 0;
					const bool tr = !ex && vt > 0.0f && FiniteF(vt) && FiniteF(vx) && FiniteF(vy);
					float rxf = 0.0f, ryf = 0.0f;
					if (tr)
					{
						rxf = static_cast<float>(vx - dx);
						ryf = static_cast<float>(vy - dy);
						const double vm2 = static_cast<double>(vx) * vx + static_cast<double>(vy) * vy;
						if (confident && vm2 <= absThr2 && mm2 > thr2)
						{
							zc[b] = 1;
							zeroList.push_back(static_cast<int>(b));
						}
					}
					resX[b] = rxf;
					resY[b] = ryf;

					uint8_t l, t = 0;
					if (ex)
						l = static_cast<uint8_t>(Label::Overlay);
					else if (pres != nullptr && FiniteF(pres[b]) && pres[b] >= pixMin)
					{
						l = static_cast<uint8_t>(Label::Object);
						t = 1;
					}
					else if (tr)
					{
						const double rx = rxf, ry = ryf;
						l = static_cast<uint8_t>(rx * rx + ry * ry <= thr2 ? Label::Camera : Label::Object);
					}
					else
					{
						bool cam = tex != nullptr && FiniteF(tex[b]) && tex[b] <= flat;
						if (!cam && FiniteF(vx) && FiniteF(vy))
						{
							const double rx = vx - mxf, ry = vy - myf;
							cam = rx * rx + ry * ry <= thr2;
						}
						l = static_cast<uint8_t>(cam ? Label::Camera : Label::Unknown);
					}
					lab[b] = l;
					tiny[b] = t;
				}
			}
			// Zero-motion clusters (static logos): 4-neighbour candidates; raster order, decisions read only zc.
			for (size_t k = 0; k < zeroList.size(); ++k)
			{
				const size_t b = static_cast<size_t>(zeroList[k]);
				const int bx = static_cast<int>(b % gws), by = static_cast<int>(b / gws);
				int nbrs = 0;
				if (bx > 0 && zc[b - 1]) ++nbrs;
				if (bx + 1 < gw && zc[b + 1]) ++nbrs;
				if (by > 0 && zc[b - gws]) ++nbrs;
				if (by + 1 < gh && zc[b + gws]) ++nbrs;
				if (nbrs >= c.overlayZeroClusterMin)
				{
					lab[b] = static_cast<uint8_t>(Label::Overlay);
					tiny[b] = 0;
				}
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
		m_tl = TwoLayerState();
		m_dyn.Reset();
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

			double floorBuf[4];
			auto updateFloor = [&](const PairInput& p) -> const double*
			{
				if (!m_config.temporalNoiseFloor)
					return nullptr;
				const double prec = PairPrecision(p);
				TemporalFloor(prec, p.picture, p.pairSeconds, floorBuf);
				return floorBuf;
			};

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
				m_tl = TwoLayerState();     // before the post-cut seed Run, which reads it
				m_dyn.Reset();
				rec.valid = false;
				rec.reason = Reason::Cut;
				rec.confidence = 0;
				if (in.next != nullptr)
				{
					CameraMotion seed = Impl::Run(*this, *in.next, nullptr, false, m_config.objects, false, nullptr);
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
						m_temporal.Update(in.next->pairSeconds, seed.rates, updateFloor(*in.next));
						m_seedPending = true;
					}
				}
				finish(rec);
				return rec;
			}

			if (in.flash)
			{
				m_dyn.Reset();   // the pair after a flash has a different intensity field
				rec.valid = false;
				rec.reason = Reason::Flash;
				rec.confidence = 0;
				if (in.next != nullptr && m_havePrevious && m_previous.valid && m_previous.pairSeconds > 0 && in.next->pairSeconds > 0)
				{
					const Params pre = TimeScale(m_previous.params, in.pairSeconds / m_previous.pairSeconds, in.picture);
					const Params preForNext = TimeScale(m_previous.params, in.next->pairSeconds / m_previous.pairSeconds, in.next->picture);
					CameraMotion nx = Impl::Run(*this, *in.next, &preForNext, false, false, false, nullptr);
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
			TlNext tlNext;
			CameraMotion fit = Impl::Run(*this, in, h0p, true, m_config.objects, genReset, &tlNext);
			if (fit.valid && Impl::DeadlinePassed(m_config, in))
			{
				// The consumers' slot has passed: no model for this frame (visible fallback); state is not updated.
				fit.valid = false;
				fit.confidence = 0;
				fit.reason = Reason::DeadlineMissed;
			}
			if (m_config.dynTexture && !fit.valid)
				m_dyn.Reset();   // an invalid pair breaks the sequence: the next pair has no previous residual field to compare against
			if (m_config.dynTexture && fit.valid)
			{
				const DynTextureResult dt = DetectDynamicTexture(m_config.dynTextureCfg, in, fit.params, m_dyn);
				fit.signals.dynShare = dt.share;
				fit.signals.dynTemporal = dt.temporal;
				fit.signals.dynSpatial = dt.spatial;
				fit.signals.dynField = dt.field;
				if (dt.detected && (fit.reason == Reason::Ok || fit.reason == Reason::LowConfidence))
				{
					fit.reason = Reason::Ambiguous;
					fit.verdictFlags |= kVerdictDynamicTexture;
					fit.confidence *= 0.495;   // below 0.5 for any logistic confidence <= 1
				}
			}
			if (m_config.staticCamera && fit.valid)
			{
				const StaticCameraResult sc = EvaluateStaticCamera(m_config.staticCfg, in, fit.params);
				fit.signals.staticRing = sc.staticShare;
				fit.signals.staticFit = sc.fitShare;
				// Only a record a consumer could use is judged: demote, never promote.
				if (sc.detected && (fit.reason == Reason::Ok || fit.reason == Reason::LowConfidence))
				{
					fit.reason = Reason::Ambiguous;
					fit.verdictFlags |= kVerdictStaticCamera;
					fit.confidence *= 0.495;
				}
			}
			if (fit.valid)
			{
				m_previous = fit;
				m_havePrevious = true;
				m_seedPending = false;
				if ((fit.verdictFlags & (kVerdictDynamicTexture | kVerdictStaticCamera)) == 0)
					m_temporal.Update(in.pairSeconds, fit.rates, updateFloor(in));   // a water-locked fit never feeds the camera prediction
			}
			if (m_config.twoLayer)
				Impl::ApplyTl(*this, in, fit, tlNext);
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
