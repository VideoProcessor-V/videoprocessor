// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#include "StaticHypothesis.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include "GlobalModelFit.h"

namespace CameraModel
{
	StaticCameraResult EvaluateStaticCamera(const StaticCameraConfig& cfg, const PairInput& in, const Params& model)
	{
		StaticCameraResult out;
		const int gw = in.gridW, gh = in.gridH;
		if (gw <= 0 || gh <= 0 || in.picture.Empty() || !(in.blockSize > 0) || in.fwdX == nullptr || in.fwdY == nullptr
			|| in.fwdTrust == nullptr || model.type == ModelType::None)
			return out;
		// Picture extent in blocks (centres inside the picture) -> ring width.
		int bx0 = gw, bx1 = -1, by0 = gh, by1 = -1;
		for (int bx = 0; bx < gw; ++bx)
		{
			const double cx = (bx + 0.5) * in.blockSize;
			if (cx >= in.picture.left && cx < in.picture.right) { bx0 = std::min(bx0, bx); bx1 = std::max(bx1, bx); }
		}
		for (int by = 0; by < gh; ++by)
		{
			const double cy = (by + 0.5) * in.blockSize;
			if (cy >= in.picture.top && cy < in.picture.bottom) { by0 = std::min(by0, by); by1 = std::max(by1, by); }
		}
		if (bx1 < bx0 || by1 < by0)
			return out;
		const int pw = bx1 - bx0 + 1, ph = by1 - by0 + 1;
		const int ringX = std::max(1, static_cast<int>(std::lround(cfg.ringFraction * pw)));
		const int ringY = std::max(1, static_cast<int>(std::lround(cfg.ringFraction * ph)));
		const double prec = in.precisionPx > 0 ? in.precisionPx : 0.25 * in.blockSize / 16.0;
		const double absThr = cfg.absSigmas * prec;
		int ring = 0, staticOk = 0, fitOk = 0, moving = 0;
		for (int by = by0; by <= by1; ++by)
		{
			const bool rowRing = by - by0 < ringY || by1 - by < ringY;
			for (int bx = bx0; bx <= bx1; ++bx)
			{
				const bool isRing = rowRing || bx - bx0 < ringX || bx1 - bx < ringX;
				if (!isRing)
				{
					bx = std::max(bx, bx1 - ringX);   // skip the interior of the row; the loop's ++ lands on the right band
					continue;
				}
				const size_t b = static_cast<size_t>(by) * gw + bx;
				if (in.textGuard != nullptr && in.textGuard[b] == 1)
					continue;
				const float tr = in.fwdTrust[b];
				if (!(tr > 0.0f) || !IsFiniteBits(tr) || !IsFiniteBits(in.fwdX[b]) || !IsFiniteBits(in.fwdY[b]))
					continue;
				double x, y, mx, my;
				BlockCentre(bx, by, in.blockSize, in.picture, &x, &y);
				Displacement(model, x, y, &mx, &my);
				if (!IsFiniteBits(mx) || !IsFiniteBits(my))
					continue;
				const double vx = in.fwdX[b], vy = in.fwdY[b];
				const double mlen = std::sqrt(mx * mx + my * my);
				const bool sOk = std::sqrt(vx * vx + vy * vy) <= absThr;
				const bool fOk = std::sqrt((vx - mx) * (vx - mx) + (vy - my) * (vy - my)) <= std::max(absThr, cfg.rel * mlen);
				++ring;
				if (sOk)
					++staticOk;
				if (fOk)
					++fitOk;
				if (mlen > absThr)
					++moving;
			}
		}
		if (ring < std::max(1, cfg.minRingBlocks))
			return out;
		out.computed = true;
		out.ringBlocks = ring;
		out.staticShare = static_cast<double>(staticOk) / ring;
		out.fitShare = static_cast<double>(fitOk) / ring;
		const bool moves = !cfg.requireMotion || 2 * moving > ring;   // the model's median ring displacement exceeds the threshold
		out.detected = moves && out.staticShare >= cfg.minStaticShare && out.staticShare >= out.fitShare - cfg.margin;
		return out;
	}
}
