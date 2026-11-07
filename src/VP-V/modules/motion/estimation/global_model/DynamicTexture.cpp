// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#include "DynamicTexture.h"

#include <algorithm>
#include <cmath>

#include "GlobalModelFit.h"

namespace CameraModel
{
	DynTextureResult DetectDynamicTexture(const DynTextureConfig& cfg, const PairInput& in, const Params& model, DynTextureState& state)
	{
		DynTextureResult out;
		const int gw = in.gridW, gh = in.gridH;
		if (gw <= 0 || gh <= 0 || in.picture.Empty() || !(in.blockSize > 0) || !(in.pairSeconds > 0) || in.fwdX == nullptr
			|| in.fwdY == nullptr || in.fwdTrust == nullptr || model.type == ModelType::None)
			return out;
		const size_t nb = static_cast<size_t>(gw) * static_cast<size_t>(gh);
		const double prec = in.precisionPx > 0 ? in.precisionPx : 0.25 * in.blockSize / 16.0;
		const double absThr = cfg.absSigmas * prec;
		const bool prevOk = state.have && state.gridW == gw && state.gridH == gh && state.picture == in.picture && state.seconds > 0;
		const double k = prevOk ? in.pairSeconds / state.seconds : 1.0;

		std::vector<float> rx(nb, 0.0f), ry(nb, 0.0f), vx(nb, 0.0f), vy(nb, 0.0f);
		std::vector<uint8_t> valid(nb, 0), dis(nb, 0);
		std::vector<float> thr(nb, 0.0f);
		int trusted = 0, disagree = 0;
		int tCompared = 0, tIncoh = 0, fCompared = 0, fIncoh = 0;
		for (int by = 0; by < gh; ++by)
		{
			const double cy = (by + 0.5) * in.blockSize;
			if (cy < in.picture.top || cy >= in.picture.bottom)
				continue;
			for (int bx = 0; bx < gw; ++bx)
			{
				const double cx = (bx + 0.5) * in.blockSize;
				if (cx < in.picture.left || cx >= in.picture.right)
					continue;
				const size_t b = static_cast<size_t>(by) * gw + bx;
				if (in.textGuard != nullptr && in.textGuard[b] == 1)
					continue;
				const float tr = in.fwdTrust[b];
				if (!(tr > 0.0f) || !IsFiniteBits(in.fwdX[b]) || !IsFiniteBits(in.fwdY[b]))
					continue;
				double x, y, mx, my;
				BlockCentre(bx, by, in.blockSize, in.picture, &x, &y);
				Displacement(model, x, y, &mx, &my);
				const double rxv = in.fwdX[b] - mx, ryv = in.fwdY[b] - my;
				const double t = std::max(absThr, cfg.rel * std::hypot(mx, my));
				rx[b] = static_cast<float>(rxv);
				ry[b] = static_cast<float>(ryv);
				vx[b] = in.fwdX[b];
				vy[b] = in.fwdY[b];
				thr[b] = static_cast<float>(t);
				valid[b] = 1;
				++trusted;
				const double rm = std::hypot(rxv, ryv);
				const bool d = rm > t;
				dis[b] = d ? 1 : 0;
				disagree += d ? 1 : 0;
				if (prevOk && state.valid[b])
				{
					{
						const double qx = state.vx[b] * k, qy = state.vy[b] * k;
						const double vm = std::hypot(static_cast<double>(in.fwdX[b]), static_cast<double>(in.fwdY[b]));
						++fCompared;
						if (std::hypot(in.fwdX[b] - qx, in.fwdY[b] - qy) > std::max(absThr, cfg.agreeRatio * std::max(vm, std::hypot(qx, qy))))
							++fIncoh;
					}
					const double px = state.rx[b] * k, py = state.ry[b] * k;
					const double pm = std::hypot(px, py);
					if (d || pm > t)
					{
						++tCompared;
						const double diff = std::hypot(rxv - px, ryv - py);
						if (diff > std::max(t, cfg.agreeRatio * std::max(rm, pm)))
							++tIncoh;
					}
				}
			}
		}
		// Spatial: right and lower neighbours that both disagree with the model.
		int sCompared = 0, sIncoh = 0;
		if (trusted >= cfg.minBlocks)
		{
			for (int by = 0; by < gh; ++by)
				for (int bx = 0; bx < gw; ++bx)
				{
					const size_t a = static_cast<size_t>(by) * gw + bx;
					if (!dis[a])
						continue;
					for (int n = 0; n < 2; ++n)
					{
						const int nx = bx + (n == 0 ? 1 : 0), ny = by + (n == 1 ? 1 : 0);
						if (nx >= gw || ny >= gh)
							continue;
						const size_t c = static_cast<size_t>(ny) * gw + nx;
						if (!dis[c])
							continue;
						++sCompared;
						const double diff = std::hypot(static_cast<double>(rx[a]) - rx[c], static_cast<double>(ry[a]) - ry[c]);
						const double ma = std::hypot(static_cast<double>(rx[a]), static_cast<double>(ry[a]));
						const double mc = std::hypot(static_cast<double>(rx[c]), static_cast<double>(ry[c]));
						if (diff > std::max(static_cast<double>(std::max(thr[a], thr[c])), cfg.agreeRatio * std::max(ma, mc)))
							++sIncoh;
					}
				}
		}
		if (trusted >= cfg.minBlocks)
		{
			out.computed = true;
			out.share = static_cast<double>(disagree) / trusted;
			out.haveTemporal = tCompared >= cfg.minCompared;
			out.temporal = out.haveTemporal ? static_cast<double>(tIncoh) / tCompared : 0.0;
			out.field = fCompared >= cfg.minCompared ? static_cast<double>(fIncoh) / fCompared : 0.0;
			out.spatial = sCompared >= cfg.minCompared ? static_cast<double>(sIncoh) / sCompared : 0.0;
			const bool temporalOk = !out.haveTemporal || out.temporal >= cfg.minTemporal;
			out.detected = out.share >= cfg.minShare && temporalOk && out.spatial >= cfg.minSpatial;
			state.have = true;
			state.gridW = gw;
			state.gridH = gh;
			state.picture = in.picture;
			state.seconds = in.pairSeconds;
			state.vx.swap(vx);
			state.vy.swap(vy);
			state.rx.swap(rx);
			state.ry.swap(ry);
			state.valid.swap(valid);
		}
		return out;
	}
}
