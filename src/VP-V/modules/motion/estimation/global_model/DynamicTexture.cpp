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
		inline double Len(double x, double y) { return std::sqrt(x * x + y * y); }

		// Derived noise scale constants for the model-free symmetric discrete vector curvature under i.i.d. component
		// noise sigma = prec:
		//  * 4-point symmetric stencil v(x,y) - 1/4 (v_L + v_R + v_U + v_D): Var = (1 + 4/16) sigma^2 = (5/4) sigma^2
		//  * 2-point symmetric opposing pair v(x,y) - 1/2 (v_A + v_B):       Var = (1 + 2/4)  sigma^2 = (3/2) sigma^2
		// Both stencils cancel ANY linear camera motion (Translation, Similarity, Affine) identically.
		const double kCurvatureSigma4 = std::sqrt(5.0) * 0.5;   // sqrt(5/4)
		const double kCurvatureSigma2 = std::sqrt(1.5);         // sqrt(3/2)

		// SHOULD-1: use the fitter's FINAL precision (quantum floor + raised robust scale) when passed;
		// when called directly (resolvedPrecPx <= 0), still honour the uniform quantization sigma q / sqrt(12).
		inline double ResolvePrecision(const PairInput& in, double resolvedPrecPx)
		{
			if (resolvedPrecPx > 0.0)
				return resolvedPrecPx;
			const double base = in.precisionPx > 0.0 ? in.precisionPx : 0.25 * in.blockSize / 16.0;
			return in.quantumPx > 0.0 ? std::max(base, in.quantumPx / std::sqrt(12.0)) : base;
		}

		// Evaluates the symmetric discrete vector curvature norm |nabla^2 v| and its derived noise sigma at (bx, by).
		// Returns false when neither opposing pair (L+R or U+D) is valid (so linear motion cannot be cancelled).
		inline bool EvalSymmetricCurvature(int bx, int by, int gw, int gh, const uint8_t* valid,
			const float* vxArr, const float* vyArr, double prec, double* curvMagOut, double* sigmaOut)
		{
			const size_t gws = static_cast<size_t>(gw);
			const size_t b = static_cast<size_t>(by) * gws + static_cast<size_t>(bx);
			const bool hasL = bx > 0 && valid[b - 1] != 0;
			const bool hasR = bx + 1 < gw && valid[b + 1] != 0;
			const bool hasU = by > 0 && valid[b - gws] != 0;
			const bool hasD = by + 1 < gh && valid[b + gws] != 0;
			const bool hasLR = hasL && hasR;
			const bool hasUD = hasU && hasD;
			if (!hasLR && !hasUD)
				return false;
			double avgX = 0.0, avgY = 0.0, sigma = 0.0;
			if (hasLR && hasUD)
			{
				avgX = (static_cast<double>(vxArr[b - 1]) + static_cast<double>(vxArr[b + 1])
					+ static_cast<double>(vxArr[b - gws]) + static_cast<double>(vxArr[b + gws])) / 4.0;
				avgY = (static_cast<double>(vyArr[b - 1]) + static_cast<double>(vyArr[b + 1])
					+ static_cast<double>(vyArr[b - gws]) + static_cast<double>(vyArr[b + gws])) / 4.0;
				sigma = kCurvatureSigma4 * prec;
			}
			else if (hasLR)
			{
				avgX = 0.5 * (static_cast<double>(vxArr[b - 1]) + static_cast<double>(vxArr[b + 1]));
				avgY = 0.5 * (static_cast<double>(vyArr[b - 1]) + static_cast<double>(vyArr[b + 1]));
				sigma = kCurvatureSigma2 * prec;
			}
			else
			{
				avgX = 0.5 * (static_cast<double>(vxArr[b - gws]) + static_cast<double>(vxArr[b + gws]));
				avgY = 0.5 * (static_cast<double>(vyArr[b - gws]) + static_cast<double>(vyArr[b + gws]));
				sigma = kCurvatureSigma2 * prec;
			}
			*curvMagOut = Len(static_cast<double>(vxArr[b]) - avgX, static_cast<double>(vyArr[b]) - avgY);
			*sigmaOut = sigma;
			return true;
		}
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
		const double prec = ResolvePrecision(in, resolvedPrecPx);
		const double absThr = cfg.absSigmas * prec;
		const double absThr2 = absThr * absThr;
		const double absThrSafeLo2 = absThr2 * (1.0 - 1e-9);
		const double rel2 = cfg.rel * cfg.rel;
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
		// Whole-field camera-advected lag-1 residual autocorrelation accumulators (rhoTemp).
		int rhoCompared = 0;
		double rhoNum = 0.0, rhoDenCur = 0.0, rhoDenPrev = 0.0;
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
				const double fx = static_cast<double>(in.fwdX[b]), fy = static_cast<double>(in.fwdY[b]);
				const double rxv = fx - mx, ryv = fy - my;
				const double m2 = mx * mx + my * my;
				// GEM-149 (P2-3 AMEND): skip sqrt when strictly below absThr; keep exact sqrt whenever near or above it.
				const double t = (cfg.rel <= 0.0 || rel2 * m2 < absThrSafeLo2) ? absThr : std::max(absThr, cfg.rel * std::sqrt(m2));
				const double t2 = t * t;
				const double tSafeLo2 = t2 * (1.0 - 1e-9);
				const double tSafeHi2 = t2 * (1.0 + 1e-9);
				rx[b] = static_cast<float>(rxv);
				ry[b] = static_cast<float>(ryv);
				vx[b] = in.fwdX[b];
				vy[b] = in.fwdY[b];
				thr[b] = static_cast<float>(t);
				valid[b] = 1;
				++trusted;
				const double r2 = rxv * rxv + ryv * ryv;
				const bool d = r2 > tSafeHi2 ? true : (r2 < tSafeLo2 ? false : (std::sqrt(r2) > t));
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
					// Camera-advected lag-1 residual autocorrelation (whole-field, unbiased by chaotic block self-advection).
					const double sxCam = (cx - mx / k) / in.blockSize - 0.5, syCam = (cy - my / k) / in.blockSize - 0.5;
					if (IsFiniteBits(sxCam) && IsFiniteBits(syCam) && sxCam > -0.5 && syCam > -0.5 && sxCam < gw - 0.5 && syCam < gh - 0.5)
					{
						const int cpx = static_cast<int>(std::floor(sxCam + 0.5)), cpy = static_cast<int>(std::floor(syCam + 0.5));
						if (cpx >= 0 && cpx < gw && cpy >= 0 && cpy < gh)
						{
							const size_t cpb = static_cast<size_t>(cpy) * gw + cpx;
							if (cpb < state.prev.valid.size() && state.prev.valid[cpb] != 0)
							{
								const double prx = static_cast<double>(state.prev.rx[cpb]) * k;
								const double pry = static_cast<double>(state.prev.ry[cpb]) * k;
								rhoNum += rxv * prx + ryv * pry;
								rhoDenCur += r2;
								rhoDenPrev += prx * prx + pry * pry;
								++rhoCompared;
							}
						}
					}
				}
				if (havePrev)
				{
					{
						const double qx = state.prev.vx[pb] * k, qy = state.prev.vy[pb] * k;
						const double dfx = fx - qx, dfy = fy - qy;
						const double df2 = dfx * dfx + dfy * dfy;
						++fCompared;
						// Keep exact sqrt for max(absThr, agreeRatio * Len) per CE AMEND; skip sqrt when strictly below absThr.
						if (df2 >= absThrSafeLo2)
						{
							const double v2 = fx * fx + fy * fy;
							const double q2 = qx * qx + qy * qy;
							if (std::sqrt(df2) > std::max(absThr, cfg.agreeRatio * std::sqrt(std::max(v2, q2))))
								++fIncoh;
						}
					}
					const double px = state.prev.rx[pb] * k, py = state.prev.ry[pb] * k;
					const double p2 = px * px + py * py;
					const bool pDis = d || (p2 > tSafeHi2 ? true : (p2 < tSafeLo2 ? false : (std::sqrt(p2) > t)));
					if (pDis)
					{
						++tCompared;
						const double dx = rxv - px, dy = ryv - py;
						const double td2 = dx * dx + dy * dy;
						if (td2 >= tSafeLo2)
						{
							if (std::sqrt(td2) > std::max(t, cfg.agreeRatio * std::sqrt(std::max(r2, p2))))
								++tIncoh;
						}
					}
				}
			}
		}
		// Spatial incoherence (right and lower neighbours that both disagree with the model) + whole-field
		// discrete vector curvature. No disagree >= minShare blind-spot gate: always computed whenever enough trusted blocks exist.
		int sCompared = 0, sIncoh = 0;
		int curvEvaluated = 0, curvIncoh = 0;
		if (trusted >= std::max(1, cfg.minBlocks))
		{
			for (int by = 0; by < gh; ++by)
				for (int bx = 0; bx < gw; ++bx)
				{
					const size_t a = static_cast<size_t>(by) * gw + bx;
					if (valid[a])
					{
						double curvMag = 0.0, curvSigma = 0.0;
						if (EvalSymmetricCurvature(bx, by, gw, gh, valid.data(), vx.data(), vy.data(), prec, &curvMag, &curvSigma))
						{
							++curvEvaluated;
							if (curvMag > cfg.absSigmas * curvSigma)
								++curvIncoh;
						}
					}
					if (!dis[a])
						continue;
					const double rxa = static_cast<double>(rx[a]), rya = static_cast<double>(ry[a]);
					const double ma2 = rxa * rxa + rya * rya;
					for (int n = 0; n < 2; ++n)
					{
						const int nx = bx + (n == 0 ? 1 : 0), ny = by + (n == 1 ? 1 : 0);
						if (nx >= gw || ny >= gh)
							continue;
						const size_t c = static_cast<size_t>(ny) * gw + nx;
						if (!dis[c])
							continue;
						++sCompared;
						const double rxc = static_cast<double>(rx[c]), ryc = static_cast<double>(ry[c]);
						const double ddx = rxa - rxc, ddy = rya - ryc;
						const double sd2 = ddx * ddx + ddy * ddy;
						const double sthr = static_cast<double>(std::max(thr[a], thr[c]));
						if (sd2 >= sthr * sthr * (1.0 - 1e-9))
						{
							const double mc2 = rxc * rxc + ryc * ryc;
							if (std::sqrt(sd2) > std::max(sthr, cfg.agreeRatio * std::sqrt(std::max(ma2, mc2))))
								++sIncoh;
						}
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
			out.spatialCurvature = curvEvaluated >= cfg.minCompared ? static_cast<double>(curvIncoh) / curvEvaluated : 0.0;
			out.haveRhoTemp = rhoCompared >= cfg.minCompared && rhoDenCur > 1e-12 && rhoDenPrev > 1e-12;
			out.rhoTemp = out.haveRhoTemp ? std::max(-1.0, std::min(1.0, rhoNum / std::sqrt(rhoDenCur * rhoDenPrev))) : 0.0;
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
		const double prec = ResolvePrecision(in, precPx);
		const bool prevOk = prevState.have && prevState.gridW == gw && prevState.gridH == gh &&
			prevState.picture == in.picture && prevState.seconds > 0 && in.pairSeconds > 0 &&
			prevState.prev.valid.size() == nb;
		const double k = prevOk ? in.pairSeconds / prevState.seconds : 1.0;
		// Derived temporal difference threshold: for i.i.d. noise sigma = prec on each pair,
		// std(v_t - k * v_{t-1}) = sqrt(1 + k^2) * prec (zero arbitrary rel * |v| scaling).
		const double temporalThr = cfg.curvatureSigmas * std::sqrt(1.0 + k * k) * prec;

		// Precompute per-block validity once (inside active picture, not text-guard, finite positive trust & vector).
		std::vector<uint8_t> valid(nb, 0);
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
				if (tr > 0.0f && IsFiniteBits(tr) && IsFiniteBits(in.fwdX[b]) && IsFiniteBits(in.fwdY[b]))
					valid[b] = 1;
			}
		}

		for (int by = 0; by < gh; ++by)
		{
			for (int bx = 0; bx < gw; ++bx)
			{
				const size_t b = static_cast<size_t>(by) * static_cast<size_t>(gw) + static_cast<size_t>(bx);
				if (!valid[b])
					continue;
				++stats.evaluated;

				// Model-free symmetric discrete vector curvature (100% invariant to pan, zoom, rotation, affine speed).
				double curvMag = 0.0, curvSigma = 0.0;
				if (!EvalSymmetricCurvature(bx, by, gw, gh, valid.data(), in.fwdX, in.fwdY, prec, &curvMag, &curvSigma))
					continue;
				const bool spatialIncoh = curvMag > cfg.curvatureSigmas * curvSigma;
				if (!spatialIncoh)
					continue;

				// Check temporal coherence against the advected previous-pair vector when history exists.
				bool haveTemporal = false;
				bool temporalIncoh = false;
				if (prevOk)
				{
					const double vx = static_cast<double>(in.fwdX[b]), vy = static_cast<double>(in.fwdY[b]);
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
								const double qx = static_cast<double>(prevState.prev.vx[pb]) * k;
								const double qy = static_cast<double>(prevState.prev.vy[pb]) * k;
								temporalIncoh = Len(vx - qx, vy - qy) > temporalThr;
							}
						}
					}
				}

				const bool drop = haveTemporal ? temporalIncoh : true;
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
