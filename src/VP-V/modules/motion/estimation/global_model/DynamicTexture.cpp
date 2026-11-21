// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#include "DynamicTexture.h"

#include <algorithm>
#include <cmath>

#include "GlobalModelFit.h"

namespace CameraModel
{
	namespace
	{
		// sqrt of the sum of squares: std::hypot is several times slower and the range here is far from overflow.
		inline double Len(double x, double y) { return std::sqrt(x * x + y * y); }
	}

	DynTextureResult DetectDynamicTexture(const DynTextureConfig& cfg, const PairInput& in, const Params& model, DynTextureState& state,
		double resolvedPrecPx)
	{
		DynTextureResult out;
		const int gw = in.gridW, gh = in.gridH;
		if (gw <= 0 || gh <= 0 || in.picture.Empty() || !(in.blockSize > 0) || !(in.pairSeconds > 0) || in.fwdX == nullptr
			|| in.fwdY == nullptr || in.fwdTrust == nullptr || model.type == ModelType::None)
			return out;
		const size_t nb = static_cast<size_t>(gw) * static_cast<size_t>(gh);
		// SHOULD-1: the fitter's FINAL precision (quantum floor, raised noise scale) when it passes one; 0 = the pair's nominal precision.
		const double prec = resolvedPrecPx > 0 ? resolvedPrecPx : (in.precisionPx > 0 ? in.precisionPx : 0.25 * in.blockSize / 16.0);
		const double absThr = cfg.absSigmas * prec;
		const bool prevOk = state.have && state.gridW == gw && state.gridH == gh && state.picture == in.picture && state.seconds > 0;
		const double k = prevOk ? in.pairSeconds / state.seconds : 1.0;

		DynTextureBuffers& cb = state.cur;
		if (cb.valid.size() != nb)
		{
			cb.rx.assign(nb, 0.0f); cb.ry.assign(nb, 0.0f); cb.vx.assign(nb, 0.0f); cb.vy.assign(nb, 0.0f); cb.thr.assign(nb, 0.0f);
			cb.valid.assign(nb, 0); cb.dis.assign(nb, 0);
		}
		else
		{
			std::fill(cb.valid.begin(), cb.valid.end(), static_cast<uint8_t>(0));
			std::fill(cb.dis.begin(), cb.dis.end(), static_cast<uint8_t>(0));
		}
		std::vector<float>& rx = cb.rx; std::vector<float>& ry = cb.ry; std::vector<float>& vx = cb.vx; std::vector<float>& vy = cb.vy;
		std::vector<float>& thr = cb.thr; std::vector<uint8_t>& valid = cb.valid; std::vector<uint8_t>& dis = cb.dis;
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
				if (!(tr > 0.0f) || !IsFiniteBits(tr) || !IsFiniteBits(in.fwdX[b]) || !IsFiniteBits(in.fwdY[b]))
					continue;
				double x, y, mx, my;
				BlockCentre(bx, by, in.blockSize, in.picture, &x, &y);
				Displacement(model, x, y, &mx, &my);
				if (!IsFiniteBits(mx) || !IsFiniteBits(my))
					continue;
				const double rxv = in.fwdX[b] - mx, ryv = in.fwdY[b] - my;
				const double t = std::max(absThr, cfg.rel * Len(mx, my));
				rx[b] = static_cast<float>(rxv);
				ry[b] = static_cast<float>(ryv);
				vx[b] = in.fwdX[b];
				vy[b] = in.fwdY[b];
				thr[b] = static_cast<float>(t);
				valid[b] = 1;
				++trusted;
				const double rm = Len(rxv, ryv);
				const bool d = rm > t;
				dis[b] = d ? 1 : 0;
				disagree += d ? 1 : 0;
				// SHOULD-4: the previous pair's residual is read where this block's content WAS (advected by this block's own vector over the
				// previous pair's duration), not at the same block index: a moving scene would otherwise compare different content. The index is
				// bounds-checked; a block that came from outside the grid has no previous value.
				size_t pb = b;
				bool havePrev = false;
				if (prevOk)
				{
					const double sx = (cx - in.fwdX[b] / k) / in.blockSize - 0.5, sy = (cy - in.fwdY[b] / k) / in.blockSize - 0.5;
					if (IsFiniteBits(sx) && IsFiniteBits(sy) && sx > -0.5 && sy > -0.5 && sx < gw - 0.5 && sy < gh - 0.5)
					{
						const int px0 = static_cast<int>(std::floor(sx + 0.5)), py0 = static_cast<int>(std::floor(sy + 0.5));
						if (px0 >= 0 && px0 < gw && py0 >= 0 && py0 < gh)
						{
							pb = static_cast<size_t>(py0) * gw + px0;
							havePrev = pb < state.prev.valid.size() && state.prev.valid[pb] != 0;
						}
					}
				}
				if (havePrev)
				{
					{
						const double qx = state.prev.vx[pb] * k, qy = state.prev.vy[pb] * k;
						const double vm = Len(static_cast<double>(in.fwdX[b]), static_cast<double>(in.fwdY[b]));
						++fCompared;
						if (Len(in.fwdX[b] - qx, in.fwdY[b] - qy) > std::max(absThr, cfg.agreeRatio * std::max(vm, Len(qx, qy))))
							++fIncoh;
					}
					const double px = state.prev.rx[pb] * k, py = state.prev.ry[pb] * k;
					const double pm = Len(px, py);
					if (d || pm > t)
					{
						++tCompared;
						const double diff = Len(rxv - px, ryv - py);
						if (diff > std::max(t, cfg.agreeRatio * std::max(rm, pm)))
							++tIncoh;
					}
				}
			}
		}
		// Spatial: right and lower neighbours that both disagree with the model.
		int sCompared = 0, sIncoh = 0;
		if (trusted >= std::max(1, cfg.minBlocks) && static_cast<double>(disagree) >= cfg.minShare * trusted)
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
						const double diff = Len(static_cast<double>(rx[a]) - rx[c], static_cast<double>(ry[a]) - ry[c]);
						const double ma = Len(static_cast<double>(rx[a]), static_cast<double>(ry[a]));
						const double mc = Len(static_cast<double>(rx[c]), static_cast<double>(ry[c]));
						if (diff > std::max(static_cast<double>(std::max(thr[a], thr[c])), cfg.agreeRatio * std::max(ma, mc)))
							++sIncoh;
					}
				}
		}
		if (trusted >= std::max(1, cfg.minBlocks))
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
			state.prev.rx.swap(cb.rx); state.prev.ry.swap(cb.ry); state.prev.vx.swap(cb.vx); state.prev.vy.swap(cb.vy);
			state.prev.valid.swap(cb.valid);
		}
		else
			state.Reset();   // too few trusted blocks: the history is stale for the next pair
		return out;
	}

	WaterExcludeStats ComputeWaterExclusionMask(const DynTextureExcludeConfig& cfg, const PairInput& in,
		const DynTextureState& prevState, double precPx, std::vector<uint8_t>& maskOut)
	{
		WaterExcludeStats stats;
		const int gw = in.gridW, gh = in.gridH;
		if (gw <= 0 || gh <= 0 || in.picture.Empty() || !(in.blockSize > 0) ||
			in.fwdX == nullptr || in.fwdY == nullptr || in.fwdTrust == nullptr)
		{
			maskOut.clear();
			return stats;
		}
		const size_t nb = static_cast<size_t>(gw) * static_cast<size_t>(gh);
		maskOut.assign(nb, 0);
		const double prec = precPx > 0 ? precPx : (in.precisionPx > 0 ? in.precisionPx : 0.25 * in.blockSize / 16.0);
		const double absThr = cfg.spatialSigmas * prec;
		const bool prevOk = prevState.have && prevState.gridW == gw && prevState.gridH == gh &&
			prevState.picture == in.picture && prevState.seconds > 0 && in.pairSeconds > 0 &&
			prevState.prev.valid.size() == nb;
		const double k = prevOk ? in.pairSeconds / prevState.seconds : 1.0;

		auto blockValid = [&](int bx, int by) -> bool {
			if (bx < 0 || bx >= gw || by < 0 || by >= gh)
				return false;
			const double cx = (bx + 0.5) * in.blockSize, cy = (by + 0.5) * in.blockSize;
			if (cx < in.picture.left || cx >= in.picture.right || cy < in.picture.top || cy >= in.picture.bottom)
				return false;
			const size_t b = static_cast<size_t>(by) * static_cast<size_t>(gw) + static_cast<size_t>(bx);
			if (in.textGuard != nullptr && in.textGuard[b] == 1)
				return false;
			const float tr = in.fwdTrust[b];
			return tr > 0.0f && IsFiniteBits(tr) && IsFiniteBits(in.fwdX[b]) && IsFiniteBits(in.fwdY[b]);
		};

		const int dx4[4] = { -1, 1, 0, 0 };
		const int dy4[4] = { 0, 0, -1, 1 };
		for (int by = 0; by < gh; ++by)
		{
			for (int bx = 0; bx < gw; ++bx)
			{
				if (!blockValid(bx, by))
					continue;
				++stats.evaluated;
				const size_t b = static_cast<size_t>(by) * static_cast<size_t>(gw) + static_cast<size_t>(bx);
				const double vx = in.fwdX[b], vy = in.fwdY[b];
				const double vm = Len(vx, vy);

				int validN = 0, incohN = 0;
				for (int n = 0; n < 4; ++n)
				{
					const int nx = bx + dx4[n], ny = by + dy4[n];
					if (!blockValid(nx, ny))
						continue;
					++validN;
					const size_t nbIdx = static_cast<size_t>(ny) * static_cast<size_t>(gw) + static_cast<size_t>(nx);
					const double nvx = in.fwdX[nbIdx], nvy = in.fwdY[nbIdx];
					const double thr = std::max(absThr, cfg.rel * std::max(vm, Len(nvx, nvy)));
					if (Len(vx - nvx, vy - nvy) > thr)
						++incohN;
				}
				if (validN < cfg.minValidNeighbours)
					continue;
				const double spatialFrac = static_cast<double>(incohN) / static_cast<double>(validN);

				// Check temporal coherence against the advected previous-pair vector when history exists.
				bool haveTemporal = false;
				bool temporalIncoh = false;
				if (prevOk)
				{
					const double cx = (bx + 0.5) * in.blockSize, cy = (by + 0.5) * in.blockSize;
					const double sx = (cx - vx / k) / in.blockSize - 0.5;
					const double sy = (cy - vy / k) / in.blockSize - 0.5;
					if (IsFiniteBits(sx) && IsFiniteBits(sy) && sx > -0.5 && sy > -0.5 && sx < gw - 0.5 && sy < gh - 0.5)
					{
						const int px0 = static_cast<int>(std::floor(sx + 0.5));
						const int py0 = static_cast<int>(std::floor(sy + 0.5));
						if (px0 >= 0 && px0 < gw && py0 >= 0 && py0 < gh)
						{
							const size_t pb = static_cast<size_t>(py0) * static_cast<size_t>(gw) + static_cast<size_t>(px0);
							if (prevState.prev.valid[pb] != 0)
							{
								haveTemporal = true;
								const double qx = prevState.prev.vx[pb] * k, qy = prevState.prev.vy[pb] * k;
								const double thr = std::max(absThr, cfg.rel * std::max(vm, Len(qx, qy)));
								temporalIncoh = Len(vx - qx, vy - qy) > thr;
							}
						}
					}
				}

				const bool drop = haveTemporal
					? (spatialFrac >= cfg.minSpatialIncoh && temporalIncoh)
					: (validN >= std::max(cfg.minValidNeighbours, 3) && spatialFrac >= cfg.coldSpatialIncoh);
				if (drop)
				{
					maskOut[b] = 1;
					++stats.excluded;
				}
			}
		}
		return stats;
	}

	void RecordRawFieldForWaterExclusion(const PairInput& in, DynTextureState& state)
	{
		const int gw = in.gridW, gh = in.gridH;
		if (gw <= 0 || gh <= 0 || in.picture.Empty() || !(in.blockSize > 0) || !(in.pairSeconds > 0) ||
			in.fwdX == nullptr || in.fwdY == nullptr || in.fwdTrust == nullptr)
		{
			state.Reset();
			return;
		}
		const size_t nb = static_cast<size_t>(gw) * static_cast<size_t>(gh);
		if (state.prev.valid.size() != nb)
		{
			state.prev.vx.assign(nb, 0.0f);
			state.prev.vy.assign(nb, 0.0f);
			state.prev.valid.assign(nb, 0);
		}
		else
			std::fill(state.prev.valid.begin(), state.prev.valid.end(), static_cast<uint8_t>(0));
		int validCount = 0;
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
				const size_t b = static_cast<size_t>(by) * static_cast<size_t>(gw) + static_cast<size_t>(bx);
				if (in.textGuard != nullptr && in.textGuard[b] == 1)
					continue;
				const float tr = in.fwdTrust[b];
				if (!(tr > 0.0f) || !IsFiniteBits(tr) || !IsFiniteBits(in.fwdX[b]) || !IsFiniteBits(in.fwdY[b]))
					continue;
				state.prev.vx[b] = in.fwdX[b];
				state.prev.vy[b] = in.fwdY[b];
				state.prev.valid[b] = 1;
				++validCount;
			}
		}
		if (validCount > 0)
		{
			state.have = true;
			state.gridW = gw;
			state.gridH = gh;
			state.picture = in.picture;
			state.seconds = in.pairSeconds;
		}
		else
			state.Reset();
	}
}
