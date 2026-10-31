// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// Global camera model: object models with persistent ids (sequential RANSAC). Deterministic.
#include "GlobalModelObjects.h"

#include <algorithm>
#include <cmath>

#include "GlobalModelGeometry.h"

namespace CameraModel
{
	namespace
	{
		const int kHypotheses = 100;

		struct Candidate
		{
			int index;          // block index
			int bx, by;
			double x, y;        // picture-centred centre
			double dx, dy;      // measured forward displacement
			double w;           // trust
		};

		struct Found
		{
			ObjectModel model;
			std::vector<int> members;   // block indices
		};

		inline double Threshold(const ObjectConfig& cfg, double precisionPx, double predDx, double predDy)
		{
			const double a = cfg.absThresholdSigmas * precisionPx;
			const double r = cfg.relThreshold * std::sqrt(predDx * predDx + predDy * predDy);
			return a > r ? a : r;
		}
	}

	void ObjectTracker::Reset()
	{
		m_previous.clear();
		m_nextId = 1;
	}

	std::vector<ObjectModel> ObjectTracker::Update(const ObjectInput& in, const ObjectConfig& cfg,
		std::vector<uint8_t>& objectId)
	{
		std::vector<ObjectModel> result;
		const size_t total = (in.gridW > 0 && in.gridH > 0)
			? static_cast<size_t>(in.gridW) * static_cast<size_t>(in.gridH) : 0;
		objectId.assign(total, 0);
		if (total == 0 || !(in.blockSize > 0) || !in.fwdX || !in.fwdY || !in.trust || !in.label
			|| cfg.maxObjects <= 0)
		{
			m_previous.clear();
			return result;
		}

		const double halfW = 0.5 * in.picture.Width(), halfH = 0.5 * in.picture.Height();
		int totalBlocks = 0;
		std::vector<Candidate> cands;
		for (int by = 0; by < in.gridH; ++by)
		{
			for (int bx = 0; bx < in.gridW; ++bx)
			{
				double x = 0, y = 0;
				BlockCentre(bx, by, in.blockSize, in.picture, &x, &y);
				if (std::fabs(x) > halfW || std::fabs(y) > halfH) continue;
				++totalBlocks;
				const int i = by * in.gridW + bx;
				if (in.label[i] != static_cast<uint8_t>(Label::Object)) continue;
				if (!(in.trust[i] > 0)) continue;
				if (!IsFiniteBits(in.fwdX[i]) || !IsFiniteBits(in.fwdY[i])) continue;
				Candidate c;
				c.index = i; c.bx = bx; c.by = by; c.x = x; c.y = y;
				c.dx = in.fwdX[i]; c.dy = in.fwdY[i]; c.w = in.trust[i];
				cands.push_back(c);
			}
		}
		int minSupport = static_cast<int>(std::ceil(cfg.objectMinShare * totalBlocks));
		if (minSupport < 3) minSupport = 3;

		const double scale = 0.5 * std::sqrt(static_cast<double>(in.picture.Width()) * in.picture.Width()
			+ static_cast<double>(in.picture.Height()) * in.picture.Height());
		const double precision = in.precisionPx > 0 ? in.precisionPx : 1e-6;

		std::vector<Found> found;
		std::vector<Candidate> remaining = cands;
		const int maxAttempts = 2 * cfg.maxObjects + 2;
		for (int attempt = 0; attempt < maxAttempts && static_cast<int>(found.size()) < cfg.maxObjects
			&& static_cast<int>(remaining.size()) >= minSupport; ++attempt)
		{
			const int n = static_cast<int>(remaining.size());
			SplitMix64 rng(in.seed + static_cast<uint64_t>(attempt));
			const int hyps = n <= kHypotheses ? n : kHypotheses;
			int bestCount = 0;
			double bestDx = 0, bestDy = 0;
			// Two-stage scoring on large candidate sets: every hypothesis is counted on a fixed stride subset,
			// the best few are then counted on all candidates (squared distances, no sqrt).
			const int kSubset = 256, kFull = 4;
			const bool staged = n > 2 * kSubset;
			std::vector<double> hxs(static_cast<size_t>(hyps)), hys(static_cast<size_t>(hyps));
			std::vector<int> stage1(static_cast<size_t>(hyps), 0);
			for (int h = 0; h < hyps; ++h)
			{
				const int s = (n <= kHypotheses) ? h : static_cast<int>(rng.Below(static_cast<uint32_t>(n)));
				hxs[static_cast<size_t>(h)] = remaining[s].dx;
				hys[static_cast<size_t>(h)] = remaining[s].dy;
			}
			auto countOn = [&](double hx, double hy, bool subset)
			{
				const double thr = Threshold(cfg, precision, hx, hy);
				const double thr2 = thr * thr;
				int count = 0;
				if (subset)
				{
					for (int k = 0; k < kSubset; ++k)
					{
						const Candidate& cd = remaining[static_cast<size_t>((static_cast<int64_t>(k) * n) / kSubset)];
						const double rx = cd.dx - hx, ry = cd.dy - hy;
						if (rx * rx + ry * ry <= thr2) ++count;
					}
				}
				else
				{
					for (int j = 0; j < n; ++j)
					{
						const double rx = remaining[j].dx - hx, ry = remaining[j].dy - hy;
						if (rx * rx + ry * ry <= thr2) ++count;
					}
				}
				return count;
			};
			if (!staged)
			{
				for (int h = 0; h < hyps; ++h)
				{
					const int count = countOn(hxs[static_cast<size_t>(h)], hys[static_cast<size_t>(h)], false);
					if (count > bestCount) { bestCount = count; bestDx = hxs[static_cast<size_t>(h)]; bestDy = hys[static_cast<size_t>(h)]; }
				}
			}
			else
			{
				for (int h = 0; h < hyps; ++h)
					stage1[static_cast<size_t>(h)] = countOn(hxs[static_cast<size_t>(h)], hys[static_cast<size_t>(h)], true);
				std::vector<char> taken(static_cast<size_t>(hyps), 0);
				for (int pick = 0; pick < kFull && pick < hyps; ++pick)
				{
					int bi = -1;
					for (int h = 0; h < hyps; ++h)
						if (!taken[static_cast<size_t>(h)] && (bi < 0 || stage1[static_cast<size_t>(h)] > stage1[static_cast<size_t>(bi)])) bi = h;
					taken[static_cast<size_t>(bi)] = 1;
					const int count = countOn(hxs[static_cast<size_t>(bi)], hys[static_cast<size_t>(bi)], false);
					if (count > bestCount) { bestCount = count; bestDx = hxs[static_cast<size_t>(bi)]; bestDy = hys[static_cast<size_t>(bi)]; }
				}
			}
			if (bestCount < minSupport) break;

			// Refine the translation on its inliers (two passes).
			std::vector<int> inl;
			double tx = bestDx, ty = bestDy;
			for (int pass = 0; pass < 2; ++pass)
			{
				const double thr = Threshold(cfg, precision, tx, ty);
				inl.clear();
				double sw = 0, sx = 0, sy = 0;
				const double thrSq = thr * thr;
				for (int j = 0; j < n; ++j)
				{
					const double rx = remaining[j].dx - tx, ry = remaining[j].dy - ty;
					if (rx * rx + ry * ry <= thrSq)
					{
						inl.push_back(j);
						sw += remaining[j].w;
						sx += remaining[j].w * remaining[j].dx;
						sy += remaining[j].w * remaining[j].dy;
					}
				}
				if (inl.empty() || !(sw > 0)) break;
				tx = sx / sw; ty = sy / sw;
			}
			if (static_cast<int>(inl.size()) < minSupport) break;

			ModelType type = ModelType::Translation;
			Params fit;
			fit.type = ModelType::Translation;
			fit.h[2] = tx; fit.h[5] = ty;

			// Try Affine when the support is large enough and the cost drop beats the penalty.
			if (inl.size() >= 6)
			{
				std::vector<Sample> samples(inl.size());
				double wsum = 0, costT = 0;
				for (size_t k = 0; k < inl.size(); ++k)
				{
					const Candidate& c = remaining[inl[k]];
					samples[k].x = c.x; samples[k].y = c.y;
					samples[k].dx = c.dx; samples[k].dy = c.dy; samples[k].w = c.w;
					wsum += c.w;
					const double rx = c.dx - tx, ry = c.dy - ty;
					costT += c.w * (rx * rx + ry * ry);
				}
				Params aff;
				if (FitWeighted(ModelType::Affine, samples.data(), static_cast<int>(samples.size()), scale, aff))
				{
					double costA = 0;
					for (size_t k = 0; k < samples.size(); ++k)
					{
						double ox = 0, oy = 0;
						Apply(aff, samples[k].x, samples[k].y, &ox, &oy);
						const double rx = samples[k].dx - (ox - samples[k].x);
						const double ry = samples[k].dy - (oy - samples[k].y);
						costA += samples[k].w * (rx * rx + ry * ry);
					}
					const double denom = std::max(costT, wsum * precision * precision);
					if ((costT - costA) / denom > cfg.complexityPenaltyPerParam * 4.0)
					{
						aff.type = ModelType::Affine;
						fit = aff;
						type = ModelType::Affine;
					}
				}
			}

			// Support under the chosen model.
			std::vector<int> support;
			if (type == ModelType::Translation)
			{
				support = inl;
			}
			else
			{
				for (int j = 0; j < n; ++j)
				{
					double ox = 0, oy = 0;
					Apply(fit, remaining[j].x, remaining[j].y, &ox, &oy);
					const double px = ox - remaining[j].x, py = oy - remaining[j].y;
					const double rx = remaining[j].dx - px, ry = remaining[j].dy - py;
					if (std::sqrt(rx * rx + ry * ry) <= Threshold(cfg, precision, px, py)) support.push_back(j);
				}
				if (static_cast<int>(support.size()) < minSupport) { support = inl; type = ModelType::Translation; fit = Params(); fit.type = type; fit.h[2] = tx; fit.h[5] = ty; }
			}

			// Compactness.
			int minBx = in.gridW, maxBx = -1, minBy = in.gridH, maxBy = -1;
			double cxs = 0, cys = 0;
			for (size_t k = 0; k < support.size(); ++k)
			{
				const Candidate& c = remaining[support[k]];
				minBx = std::min(minBx, c.bx); maxBx = std::max(maxBx, c.bx);
				minBy = std::min(minBy, c.by); maxBy = std::max(maxBy, c.by);
				cxs += c.x; cys += c.y;
			}
			const double area = static_cast<double>(maxBx - minBx + 1) * static_cast<double>(maxBy - minBy + 1);
			const bool compact = area <= 4.0 * static_cast<double>(support.size());

			std::vector<char> drop(static_cast<size_t>(n), 0);
			for (size_t k = 0; k < support.size(); ++k) drop[support[k]] = 1;

			if (compact)
			{
				Found f;
				f.model.type = type;
				for (int q = 0; q < 9; ++q) f.model.h[q] = fit.h[q];
				f.model.blocks = static_cast<uint32_t>(support.size());
				f.model.cx = cxs / static_cast<double>(support.size());
				f.model.cy = cys / static_cast<double>(support.size());
				for (size_t k = 0; k < support.size(); ++k) f.members.push_back(remaining[support[k]].index);
				found.push_back(f);
			}
			// Accepted or rejected, a set formed from >= minSupport inliers leaves the pool.
			std::vector<Candidate> next;
			for (int j = 0; j < n; ++j) if (!drop[j]) next.push_back(remaining[j]);
			remaining.swap(next);
		}

		// Persistent ids by predicted-centre matching.
		const size_t nf = found.size();
		std::vector<int> matchPrev(nf, -1);
		std::vector<char> prevUsed(m_previous.size(), 0);
		for (;;)
		{
			double bestD = 0;
			int bi = -1, bj = -1;
			for (size_t i = 0; i < nf; ++i)
			{
				if (matchPrev[i] >= 0) continue;
				for (size_t j = 0; j < m_previous.size(); ++j)
				{
					if (prevUsed[j]) continue;
					const ObjectModel& p = m_previous[j];
					double dx = 0, dy = 0;
					Params pp; pp.type = p.type;
					for (int q = 0; q < 9; ++q) pp.h[q] = p.h[q];
					Displacement(pp, p.cx, p.cy, &dx, &dy);
					const double ex = found[i].model.cx - (p.cx + dx), ey = found[i].model.cy - (p.cy + dy);
					const double d = std::sqrt(ex * ex + ey * ey);
					const double lim = in.blockSize * (1.0 + std::sqrt(static_cast<double>(
						std::max(found[i].model.blocks, p.blocks))));
					if (d > lim) continue;
					if (bi < 0 || d < bestD) { bestD = d; bi = static_cast<int>(i); bj = static_cast<int>(j); }
				}
			}
			if (bi < 0) break;
			matchPrev[bi] = bj;
			prevUsed[bj] = 1;
		}
		bool used[256] = {};
		for (size_t i = 0; i < nf; ++i)
		{
			if (matchPrev[i] >= 0)
			{
				found[i].model.id = m_previous[matchPrev[i]].id;
				used[found[i].model.id] = true;
			}
		}
		for (size_t i = 0; i < nf; ++i)
		{
			if (matchPrev[i] >= 0) continue;
			uint8_t id = m_nextId ? m_nextId : 1;
			for (int tries = 0; tries < 255 && used[id]; ++tries) id = (id == 255) ? 1 : static_cast<uint8_t>(id + 1);
			found[i].model.id = id;
			used[id] = true;
			m_nextId = (id == 255) ? 1 : static_cast<uint8_t>(id + 1);
		}

		for (size_t i = 0; i < nf; ++i)
		{
			for (size_t k = 0; k < found[i].members.size(); ++k) objectId[found[i].members[k]] = found[i].model.id;
			result.push_back(found[i].model);
		}
		m_previous = result;
		return result;
	}
}
