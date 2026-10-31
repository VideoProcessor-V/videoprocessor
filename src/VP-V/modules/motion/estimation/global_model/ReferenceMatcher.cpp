// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// CPU reference matcher (see ReferenceMatcher.h). Mirrors the GPU matcher's search order so that tests and
// the cost measurement can compare today's search with the plan-guided one. Not on the runtime path.
#include "ReferenceMatcher.h"

#include <algorithm>
#include <chrono>
#include <cmath>

namespace CameraModel
{
	namespace Reference
	{
		namespace
		{
			// The matcher's own geometry: an 8x8 block, a 2-px SAD margin, 4x4 sub-blocks for tiny movers.
			const int kBlock = 8;
			const int kMargin = 2;
			const int kSub = 4;
			const int kMaxWindow = (kBlock + 2 * kMargin) * (kBlock + 2 * kMargin);
			const double kUntrusted = 1e4;

			struct Ctx
			{
				const Plane& f;
				const Plane& t;
				uint64_t evals = 0;
				Ctx(const Plane& from, const Plane& to) : f(from), t(to) {}
			};

			inline int Clamp(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
			inline int RoundInt(double v) { return static_cast<int>(std::floor(v + 0.5)); }
			inline float Tap(const Plane& p, int x, int y) { return p.px[static_cast<size_t>(Clamp(y, 0, p.h - 1)) * static_cast<size_t>(p.w) + static_cast<size_t>(Clamp(x, 0, p.w - 1))]; }

			// Mean absolute difference per sample between the `from` window at (ox, oy) and the `to` window at
			// (ox, oy) + (vx, vy). norm: zero-mean (each window's mean removed before the difference).
			double WindowSad(Ctx& c, int ox, int oy, int ww, int wh, double vx, double vy, bool norm)
			{
				++c.evals;
				const int n = ww * wh;
				const double fx = std::floor(vx), fy = std::floor(vy);
				const int ix = static_cast<int>(fx), iy = static_cast<int>(fy);
				const float ax = static_cast<float>(vx - fx), ay = static_cast<float>(vy - fy);
				const bool whole = ax == 0.0f && ay == 0.0f;
				if (whole && !norm)
				{
					double sum = 0;
					const bool inside = ox >= 0 && oy >= 0 && ox + ww <= c.f.w && oy + wh <= c.f.h &&
						ox + ix >= 0 && oy + iy >= 0 && ox + ix + ww <= c.t.w && oy + iy + wh <= c.t.h;
					if (inside)
					{
						for (int y = 0; y < wh; ++y)
						{
							const float* a = c.f.px + static_cast<size_t>(oy + y) * static_cast<size_t>(c.f.w) + ox;
							const float* b = c.t.px + static_cast<size_t>(oy + y + iy) * static_cast<size_t>(c.t.w) + ox + ix;
							for (int x = 0; x < ww; ++x) sum += std::fabs(a[x] - b[x]);
						}
					}
					else
					{
						for (int y = 0; y < wh; ++y)
							for (int x = 0; x < ww; ++x)
								sum += std::fabs(Tap(c.f, ox + x, oy + y) - Tap(c.t, ox + x + ix, oy + y + iy));
					}
					return sum / n;
				}
				float fs[kMaxWindow], ts[kMaxWindow];
				int k = 0;
				for (int y = 0; y < wh; ++y)
					for (int x = 0; x < ww; ++x, ++k)
					{
						fs[k] = Tap(c.f, ox + x, oy + y);
						const int sx = ox + x + ix, sy = oy + y + iy;
						if (whole) ts[k] = Tap(c.t, sx, sy);
						else
						{
							const float a = Tap(c.t, sx, sy), b = Tap(c.t, sx + 1, sy), cc = Tap(c.t, sx, sy + 1), d = Tap(c.t, sx + 1, sy + 1);
							ts[k] = (1 - ay) * ((1 - ax) * a + ax * b) + ay * ((1 - ax) * cc + ax * d);
						}
					}
				double mf = 0, mt = 0;
				if (norm)
				{
					for (int i = 0; i < n; ++i) { mf += fs[i]; mt += ts[i]; }
					mf /= n;
					mt /= n;
				}
				double sum = 0;
				for (int i = 0; i < n; ++i) sum += std::fabs((fs[i] - mf) - (ts[i] - mt));
				return sum / n;
			}

			struct Best
			{
				double sad = 1e30;
				double vx = 0, vy = 0;
			};

			// Evaluates candidates; the first strictly lower SAD wins (ties keep the earlier candidate).
			struct Searcher
			{
				Ctx& c;
				int ox, oy, ww, wh;
				bool norm;
				Best best;
				Searcher(Ctx& ctx, int x, int y, int w, int h, bool n) : c(ctx), ox(x), oy(y), ww(w), wh(h), norm(n) {}
				void Eval(double vx, double vy)
				{
					const double s = WindowSad(c, ox, oy, ww, wh, vx, vy, norm);
					if (s < best.sad) { best.sad = s; best.vx = vx; best.vy = vy; }
				}
				void Box(int cx, int cy, int rx, int ry)
				{
					for (int dy = -ry; dy <= ry; ++dy)
						for (int dx = -rx; dx <= rx; ++dx) Eval(cx + dx, cy + dy);
				}
				void HalfPel()
				{
					const Best base = best;
					for (int dy = -1; dy <= 1; ++dy)
						for (int dx = -1; dx <= 1; ++dx)
						{
							if (dx == 0 && dy == 0) continue;
							Eval(base.vx + 0.5 * dx, base.vy + 0.5 * dy);
						}
				}
			};

			struct FullArgs
			{
				int radius = 12;
				bool haveGlobal = false;
				double gx = 0, gy = 0;
				bool prevValid = false;
				double pvx = 0, pvy = 0;
				bool guided = false;
				double px = 0, py = 0;
			};

			// Today's search: zero, exhaustive around zero, around the global start, around the previous vector, half-pel.
			// With a guided candidate the prediction is evaluated first (ties keep it).
			Best FullSearch(Ctx& c, int ox, int oy, int ww, int wh, const FullArgs& a)
			{
				Searcher s(c, ox, oy, ww, wh, false);
				if (a.guided) s.Box(RoundInt(a.px), RoundInt(a.py), 1, 1);
				s.Eval(0, 0);
				s.Box(0, 0, a.radius, a.radius);
				if (a.haveGlobal) s.Box(RoundInt(a.gx), RoundInt(a.gy), 3, 3);
				if (a.prevValid) s.Box(RoundInt(a.pvx), RoundInt(a.pvy), 3, 3);
				s.HalfPel();
				return s.best;
			}

			double Median(std::vector<float>& v)
			{
				std::sort(v.begin(), v.end());
				const size_t n = v.size();
				return (n & 1u) ? v[n / 2] : 0.5 * (static_cast<double>(v[n / 2 - 1]) + v[n / 2]);
			}

			size_t PlanIndex(const MeasurementPlan& p, int bx, int by, double s2m)
			{
				if (p.blockSize <= 0) return static_cast<size_t>(Clamp(by, 0, p.gridH - 1)) * static_cast<size_t>(p.gridW) + static_cast<size_t>(Clamp(bx, 0, p.gridW - 1));
				const double sx = (bx + 0.5) * kBlock / s2m, sy = (by + 0.5) * kBlock / s2m;
				const int px = Clamp(static_cast<int>(std::floor(sx / p.blockSize)), 0, p.gridW - 1);
				const int py = Clamp(static_cast<int>(std::floor(sy / p.blockSize)), 0, p.gridH - 1);
				return static_cast<size_t>(py) * static_cast<size_t>(p.gridW) + static_cast<size_t>(px);
			}
		}

		MatchState StateFrom(const MatchResult& r)
		{
			MatchState s;
			s.vx = r.vx;
			s.vy = r.vy;
			s.sad = r.sad;
			s.valid = !r.vx.empty() && !r.skipped;
			return s;
		}

		MatchResult Match(const MatchConfig& cfg, const Plane& from, const Plane& to, const MeasurementPlan* plan,
			const MatchState* previous, double globalX, double globalY)
		{
			const std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
			MatchResult res;
			auto finish = [&]() { res.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count(); };
			if (from.px == nullptr || to.px == nullptr) { finish(); return res; }
			res.bw = std::min(from.w, to.w) / kBlock;
			res.bh = std::min(from.h, to.h) / kBlock;
			if (res.bw <= 0 || res.bh <= 0) { res.bw = res.bh = 0; finish(); return res; }
			const int bw = res.bw, bh = res.bh;
			const size_t n = static_cast<size_t>(bw) * static_cast<size_t>(bh);
			res.vx.assign(n, 0.0f);
			res.vy.assign(n, 0.0f);
			res.sad.assign(n, 0.0f);
			res.path.assign(n, kPathFull);
			const double s2m = cfg.sourceToMatcher;
			const double tol = cfg.tolerance;

			const bool planOn = plan != nullptr && plan->active;
			if (planOn && plan->skipMatcher)
			{
				std::fill(res.sad.begin(), res.sad.end(), static_cast<float>(kUntrusted));
				std::fill(res.path.begin(), res.path.end(), kPathSkipped);
				res.skipped = true;
				finish();
				return res;
			}
			const bool prevValid = previous != nullptr && previous->valid && previous->vx.size() == n && previous->vy.size() == n &&
				previous->sad.size() == n;

			// Global start: the model's (plan) or today's D1 result (arguments), source px.
			double gx = globalX, gy = globalY;
			if (planOn && plan->useModelGlobal) { gx = plan->globalX; gy = plan->globalY; }
			const bool haveGlobal = !std::isnan(gx) && !std::isnan(gy);

			// E4: keep the previous field when the scene is static.
			if (cfg.gate && prevValid)
			{
				double mean = 0;
				for (size_t i = 0; i < n; ++i) mean += std::hypot(static_cast<double>(previous->vx[i]), static_cast<double>(previous->vy[i]));
				mean = mean / static_cast<double>(n) / s2m;   // source px
				bool still = false;
				if (haveGlobal && std::hypot(gx, gy) < cfg.motionLo && mean < cfg.motionLo) still = true;
				if (planOn && plan->staticScene && mean < cfg.motionLo) still = true;
				if (still)
				{
					res.vx = previous->vx;
					res.vy = previous->vy;
					res.sad = previous->sad;
					std::fill(res.path.begin(), res.path.end(), kPathStatic);
					res.staticKept = static_cast<uint32_t>(n);
					finish();
					return res;
				}
			}

			const bool haveBlocks = planOn && !plan->hint.empty() && plan->gridW > 0 && plan->gridH > 0 &&
				plan->hint.size() == static_cast<size_t>(plan->gridW) * static_cast<size_t>(plan->gridH) &&
				plan->eligible.size() == plan->hint.size() && plan->seedX.size() == plan->hint.size() &&
				plan->seedY.size() == plan->hint.size() && plan->predX.size() == plan->hint.size() && plan->predY.size() == plan->hint.size();
			const bool guided = haveBlocks && plan->guidedCandidate;
			Ctx c(from, to);
			auto windowOrigin = [](int origin, int extent, int window) { return Clamp(origin - kMargin, 0, std::max(0, extent - window)); };
			const int fullW = kBlock + 2 * kMargin;
			const int subW = kSub + 2 * kMargin;

			// ---- first sweep ----
			for (int by = 0; by < bh; ++by)
				for (int bx = 0; bx < bw; ++bx)
				{
					const size_t i = static_cast<size_t>(by) * static_cast<size_t>(bw) + static_cast<size_t>(bx);
					const int ox = windowOrigin(bx * kBlock, from.w, fullW), oy = windowOrigin(by * kBlock, from.h, fullW);
					size_t pi = 0;
					BlockHint hint = BlockHint::FullSearch;
					bool eligible = false;
					if (haveBlocks)
					{
						pi = PlanIndex(*plan, bx, by, s2m);
						hint = static_cast<BlockHint>(plan->hint[pi]);
						eligible = plan->eligible[pi] != 0;
					}
					FullArgs fa;
					fa.radius = cfg.fullRadius;
					fa.haveGlobal = haveGlobal;
					fa.gx = gx * s2m;
					fa.gy = gy * s2m;
					fa.prevValid = prevValid;
					if (prevValid) { fa.pvx = previous->vx[i]; fa.pvy = previous->vy[i]; }
					if (guided && eligible) { fa.guided = true; fa.px = plan->predX[pi] * s2m; fa.py = plan->predY[pi] * s2m; }

					Best best;
					uint8_t path = kPathFull;
					if (hint == BlockHint::NarrowAroundPrediction)
					{
						Searcher s(c, ox, oy, fullW, fullW, plan->fadeNormalised);
						const int radius = std::max(1, static_cast<int>(std::ceil(plan->narrowRadiusPx * s2m)));
						s.Box(RoundInt(plan->seedX[pi] * s2m), RoundInt(plan->seedY[pi] * s2m), radius, radius);
						s.HalfPel();
						if (s.best.sad <= tol) { best = s.best; path = kPathNarrow; ++res.narrowAccepted; }
						else
						{
							FullArgs plain = fa;
							best = FullSearch(c, ox, oy, fullW, fullW, plain);
							path = kPathEscalated;
							++res.escalated;
						}
					}
					else if (hint == BlockHint::FinerGrid)
					{
						best = FullSearch(c, ox, oy, fullW, fullW, fa);
						FullArgs sa = fa;
						sa.guided = false;
						bool found = false;
						Best pick;
						double pickLen = -1;
						for (int sy = 0; sy < 2; ++sy)
							for (int sx = 0; sx < 2; ++sx)
							{
								const int sox = windowOrigin(bx * kBlock + sx * kSub, from.w, subW);
								const int soy = windowOrigin(by * kBlock + sy * kSub, from.h, subW);
								const Best sb = FullSearch(c, sox, soy, subW, subW, sa);
								if (sb.sad > tol) continue;
								const double len = std::hypot(sb.vx, sb.vy);
								if (len > pickLen) { pickLen = len; pick = sb; found = true; }
							}
						if (found) best = pick;
						path = kPathFiner;
						++res.finer;
					}
					else
						best = FullSearch(c, ox, oy, fullW, fullW, fa);
					res.vx[i] = static_cast<float>(best.vx);
					res.vy[i] = static_cast<float>(best.vy);
					res.sad[i] = static_cast<float>(best.sad);
					res.path[i] = path;
				}

			// ---- second sweep ----
			if (cfg.secondSweep)
			{
				const std::vector<float> fvx = res.vx, fvy = res.vy, fsad = res.sad;
				std::vector<float> nbx, nby;
				for (int by = 0; by < bh; ++by)
					for (int bx = 0; bx < bw; ++bx)
					{
						const size_t i = static_cast<size_t>(by) * static_cast<size_t>(bw) + static_cast<size_t>(bx);
						const uint8_t path = res.path[i];
						if (path == kPathFiner) continue;
						size_t pi = 0;
						bool eligible = false;
						if (haveBlocks) { pi = PlanIndex(*plan, bx, by, s2m); eligible = plan->eligible[pi] != 0; }
						if (haveBlocks && plan->e1Skip && eligible)
						{
							const double dx = fvx[i] - plan->predX[pi] * s2m, dy = fvy[i] - plan->predY[pi] * s2m;
							if (std::hypot(dx, dy) <= plan->e1AgreePx * s2m && fsad[i] <= tol)
							{
								res.path[i] = kPathE1Skip;
								++res.e1Skipped;
								continue;
							}
							if (path != kPathEscalated) ++res.escalated;
						}
						nbx.clear();
						nby.clear();
						for (int dy = -1; dy <= 1; ++dy)
							for (int dx = -1; dx <= 1; ++dx)
							{
								if (dx == 0 && dy == 0) continue;
								const int nx = bx + dx, ny = by + dy;
								if (nx < 0 || ny < 0 || nx >= bw || ny >= bh) continue;
								const size_t ni = static_cast<size_t>(ny) * static_cast<size_t>(bw) + static_cast<size_t>(nx);
								nbx.push_back(fvx[ni]);
								nby.push_back(fvy[ni]);
							}
						// Candidates in a fixed order: own, the 8 neighbours, previous, global, prediction.
						std::vector<float> cand;   // pairs
						cand.push_back(fvx[i]); cand.push_back(fvy[i]);
						for (int dy = -1; dy <= 1; ++dy)
							for (int dx = -1; dx <= 1; ++dx)
							{
								if (dx == 0 && dy == 0) continue;
								const int nx = bx + dx, ny = by + dy;
								if (nx < 0 || ny < 0 || nx >= bw || ny >= bh) continue;
								const size_t ni = static_cast<size_t>(ny) * static_cast<size_t>(bw) + static_cast<size_t>(nx);
								cand.push_back(fvx[ni]); cand.push_back(fvy[ni]);
							}
						if (prevValid) { cand.push_back(previous->vx[i]); cand.push_back(previous->vy[i]); }
						if (haveGlobal) { cand.push_back(static_cast<float>(gx * s2m)); cand.push_back(static_cast<float>(gy * s2m)); }
						if (guided && eligible) { cand.push_back(static_cast<float>(plan->predX[pi] * s2m)); cand.push_back(static_cast<float>(plan->predY[pi] * s2m)); }
						const double medx = nbx.empty() ? fvx[i] : Median(nbx);
						const double medy = nby.empty() ? fvy[i] : Median(nby);
						const int ox = windowOrigin(bx * kBlock, from.w, fullW), oy = windowOrigin(by * kBlock, from.h, fullW);
						double bestScore = 1e30, bestSad = 0, bvx = fvx[i], bvy = fvy[i];
						auto consider = [&](double vx, double vy)
						{
							const double s = WindowSad(c, ox, oy, fullW, fullW, vx, vy, false);
							const double score = s + cfg.lambda * 2.0 * std::hypot(vx - medx, vy - medy);
							if (score < bestScore) { bestScore = score; bestSad = s; bvx = vx; bvy = vy; }
						};
						for (size_t k = 0; k + 1 < cand.size(); k += 2) consider(cand[k], cand[k + 1]);
						const double cvx = bvx, cvy = bvy;
						for (int dy = -1; dy <= 1; ++dy)
							for (int dx = -1; dx <= 1; ++dx)
							{
								if (dx == 0 && dy == 0) continue;
								consider(cvx + 0.5 * dx, cvy + 0.5 * dy);
							}
						res.vx[i] = static_cast<float>(bvx);
						res.vy[i] = static_cast<float>(bvy);
						res.sad[i] = static_cast<float>(bestSad);
					}
			}
			res.evaluations = c.evals;
			finish();
			return res;
		}
	}
}
