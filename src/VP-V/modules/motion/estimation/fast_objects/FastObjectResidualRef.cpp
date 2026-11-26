// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// Stage A CPU reference implementation: 3-frame same-polarity symmetric residual, physics-derived
// local noise/gradient floor, 3-DOF chroma chi^2 combination, 4x4 cell matched filter, bounded
// active-cell overflow guard, and exact weighted second-moment streak deconvolution.
#include "FastObjectResidualRef.h"

#include <algorithm>
#include <cmath>

namespace FastObjects
{
	namespace
	{
		constexpr uint8_t kSampleValid = 1u << 0;
		constexpr uint8_t kSampleSeed = 1u << 1;
		constexpr uint8_t kSampleGrow = 1u << 2;
		constexpr uint8_t kSampleStreakEligible = 1u << 3;
		constexpr uint8_t kSampleNearLargeObj = 1u << 4;

		constexpr uint8_t kCellActive = 1u << 0;
		constexpr uint8_t kCellStreakTrigger = 1u << 1;
		constexpr uint8_t kCellGuardOk = 1u << 2;

		inline double BilinearSafe(const float* img, int w, int h, double sx, double sy)
		{
			if (sx < 0.0) sx = 0.0;
			else if (sx > static_cast<double>(w - 1)) sx = static_cast<double>(w - 1);
			if (sy < 0.0) sy = 0.0;
			else if (sy > static_cast<double>(h - 1)) sy = static_cast<double>(h - 1);
			const int x0 = static_cast<int>(std::floor(sx));
			const int y0 = static_cast<int>(std::floor(sy));
			const int x1 = (x0 + 1 < w) ? (x0 + 1) : (w - 1);
			const int y1 = (y0 + 1 < h) ? (y0 + 1) : (h - 1);
			const double fx = sx - static_cast<double>(x0);
			const double fy = sy - static_cast<double>(y0);
			const size_t row0 = static_cast<size_t>(y0) * static_cast<size_t>(w);
			const size_t row1 = static_cast<size_t>(y1) * static_cast<size_t>(w);
			const double a = static_cast<double>(img[row0 + static_cast<size_t>(x0)]);
			const double b = static_cast<double>(img[row0 + static_cast<size_t>(x1)]);
			const double c = static_cast<double>(img[row1 + static_cast<size_t>(x0)]);
			const double d = static_cast<double>(img[row1 + static_cast<size_t>(x1)]);
			return (a * (1.0 - fx) + b * fx) * (1.0 - fy) + (c * (1.0 - fx) + d * fx) * fy;
		}

		inline double ApplyFadeNorm(double v, bool fade, double gain, double offset)
		{
			if (!fade || !(std::fabs(gain) > 1e-6))
				return v;
			return (v - offset) / gain;
		}

		// Exact physics-derived bilinear 2nd-order Taylor truncation error bound on the warped source image:
		//   |I_bilin(x0+fx, y0+fy) - I(x, y)| <= 0.5 * min(fx, 1-fx) * |I_xx| + 0.5 * min(fy, 1-fy) * |I_yy|.
		// Evaluated on the warped source image (prev/next), so static panning pitch/rink lines & textured peaks
		// (present in prev & next) are cleanly bounded while real moving balls (absent at cur's pos in prev/next)
		// incur zero SNR penalty. Returns 0 identically for integer shifts (fx == 0 && fy == 0).
		inline double BilinearCurvatureBound(
			const float* img, int w, int h, double sx, double sy,
			bool fade, double fadeGain)
		{
			if (sx < 0.0) sx = 0.0;
			else if (sx > static_cast<double>(w - 1)) sx = static_cast<double>(w - 1);
			if (sy < 0.0) sy = 0.0;
			else if (sy > static_cast<double>(h - 1)) sy = static_cast<double>(h - 1);
			const int x0 = static_cast<int>(std::floor(sx));
			const int y0 = static_cast<int>(std::floor(sy));
			const double fx = sx - static_cast<double>(x0);
			const double fy = sy - static_cast<double>(y0);
			const double wx = 0.5 * std::fmin(fx, 1.0 - fx);
			const double wy = 0.5 * std::fmin(fy, 1.0 - fy);
			if (!(wx > 1e-9) && !(wy > 1e-9))
				return 0.0;

			const int x1 = (x0 + 1 < w) ? (x0 + 1) : (w - 1);
			const int y1 = (y0 + 1 < h) ? (y0 + 1) : (h - 1);

			auto sampleAt = [&](int ix, int iy) -> double
			{
				const int cx = std::min(std::max(ix, 0), w - 1);
				const int cy = std::min(std::max(iy, 0), h - 1);
				return static_cast<double>(img[static_cast<size_t>(cy) * static_cast<size_t>(w) + static_cast<size_t>(cx)]);
			};

			double maxD2x = 0.0;
			double maxD2y = 0.0;
			const int xs[2] = { x0, x1 };
			const int ys[2] = { y0, y1 };
			for (int yi = 0; yi < 2; ++yi)
			{
				const int ky = ys[yi];
				for (int xi = 0; xi < 2; ++xi)
				{
					const int kx = xs[xi];
					const double c = sampleAt(kx, ky);
					if (wx > 1e-9)
					{
						const double d2x = std::fabs(sampleAt(kx - 1, ky) - 2.0 * c + sampleAt(kx + 1, ky));
						if (d2x > maxD2x) maxD2x = d2x;
					}
					if (wy > 1e-9)
					{
						const double d2y = std::fabs(sampleAt(kx, ky - 1) - 2.0 * c + sampleAt(kx, ky + 1));
						if (d2y > maxD2y) maxD2y = d2y;
					}
				}
			}

			double bound = wx * maxD2x + wy * maxD2y;
			if (fade && std::fabs(fadeGain) > 1e-6)
				bound /= std::fabs(fadeGain);
			return bound;
		}

		// Exact same-polarity symmetric 3-frame residual (eliminates single-pair ghosts and opposite-sign flicker).
		inline double SamePolarityResidual(double dm, double dp)
		{
			if (dm * dp <= 0.0)
				return 0.0;
			const double mag = std::fmin(std::fabs(dm), std::fabs(dp));
			return (dm > 0.0) ? mag : -mag;
		}

		// Inline 3x3 Sobel gradient magnitude normalized by 1/8 (exact first derivative scale for linear ramps).
		inline double SobelGradMag(const float* img, int w, int h, int x, int y)
		{
			const int xm = (x > 0) ? (x - 1) : 0;
			const int xp = (x + 1 < w) ? (x + 1) : (w - 1);
			const int ym = (y > 0) ? (y - 1) : 0;
			const int yp = (y + 1 < h) ? (y + 1) : (h - 1);
			const size_t rm = static_cast<size_t>(ym) * static_cast<size_t>(w);
			const size_t r0 = static_cast<size_t>(y) * static_cast<size_t>(w);
			const size_t rp = static_cast<size_t>(yp) * static_cast<size_t>(w);
			const double a00 = img[rm + static_cast<size_t>(xm)];
			const double a01 = img[rm + static_cast<size_t>(x)];
			const double a02 = img[rm + static_cast<size_t>(xp)];
			const double a10 = img[r0 + static_cast<size_t>(xm)];
			const double a12 = img[r0 + static_cast<size_t>(xp)];
			const double a20 = img[rp + static_cast<size_t>(xm)];
			const double a21 = img[rp + static_cast<size_t>(x)];
			const double a22 = img[rp + static_cast<size_t>(xp)];
			const double gx = ((a02 - a00) + 2.0 * (a12 - a10) + (a22 - a20)) * 0.125;
			const double gy = ((a20 - a00) + 2.0 * (a21 - a01) + (a22 - a02)) * 0.125;
			return std::sqrt(gx * gx + gy * gy);
		}

		// Robust MAD sigma estimator in-place on scratch vector (returns 0 if empty).
		double EstimateDiffSigmaFromMad(std::vector<float>& scratch)
		{
			if (scratch.empty())
				return 0.0;
			const size_t n = scratch.size();
			std::nth_element(scratch.begin(), scratch.begin() + static_cast<std::ptrdiff_t>(n / 2), scratch.end());
			const float med = scratch[n / 2];
			for (size_t i = 0; i < n; ++i)
				scratch[i] = std::fabs(scratch[i] - med);
			std::nth_element(scratch.begin(), scratch.begin() + static_cast<std::ptrdiff_t>(n / 2), scratch.end());
			return kMadToSigma * static_cast<double>(scratch[n / 2]);
		}

		// Marks connected P8 Label::Object components whose area exceeds (maxMinorAxisFrac * picH)^2.
		void BuildLargeObjectMasks(
			const ResidualInput& in,
			double maxLargeObjAreaSrcPx2,
			ResidualWorkspace& ws)
		{
			const int gw = in.guardGridW;
			const int gh = in.guardGridH;
			const size_t gcount = (gw > 0 && gh > 0) ? static_cast<size_t>(gw) * static_cast<size_t>(gh) : 0;
			ws.largeObjectBlocks.assign(gcount, 0);
			ws.nearLargeObjectBlocks.assign(gcount, 0);
			if (!in.blockLabels || gcount == 0 || !(in.guardBlockSrcPx > 0.0))
				return;

			ws.visited.assign(gcount, 0);
			const double blockArea = in.guardBlockSrcPx * in.guardBlockSrcPx;
			const int dx4[4] = { -1, 1, 0, 0 };
			const int dy4[4] = { 0, 0, -1, 1 };

			for (int by = 0; by < gh; ++by)
			{
				for (int bx = 0; bx < gw; ++bx)
				{
					const size_t start = static_cast<size_t>(by) * static_cast<size_t>(gw) + static_cast<size_t>(bx);
					if (ws.visited[start] != 0)
						continue;
					if (in.blockLabels[start] != static_cast<uint8_t>(CameraModel::Label::Object))
						continue;

					ws.bfsQueue.clear();
					ws.componentIndices.clear();
					ws.visited[start] = 1;
					ws.bfsQueue.push_back(static_cast<int>(start));
					size_t head = 0;
					while (head < ws.bfsQueue.size())
					{
						const int cur = ws.bfsQueue[head++];
						ws.componentIndices.push_back(cur);
						const int cx = cur % gw;
						const int cy = cur / gw;
						for (int k = 0; k < 4; ++k)
						{
							const int nx = cx + dx4[k];
							const int ny = cy + dy4[k];
							if (nx < 0 || nx >= gw || ny < 0 || ny >= gh)
								continue;
							const size_t nidx = static_cast<size_t>(ny) * static_cast<size_t>(gw) + static_cast<size_t>(nx);
							if (ws.visited[nidx] != 0)
								continue;
							if (in.blockLabels[nidx] != static_cast<uint8_t>(CameraModel::Label::Object))
								continue;
							ws.visited[nidx] = 1;
							ws.bfsQueue.push_back(static_cast<int>(nidx));
						}
					}

					const double compArea = static_cast<double>(ws.componentIndices.size()) * blockArea;
					if (compArea > maxLargeObjAreaSrcPx2)
					{
						for (int idx : ws.componentIndices)
							ws.largeObjectBlocks[static_cast<size_t>(idx)] = 1;
					}
				}
			}

			// Dilate by 1 block to mark proximity for Stage B active-strike contact validation.
			for (int by = 0; by < gh; ++by)
			{
				for (int bx = 0; bx < gw; ++bx)
				{
					const size_t idx = static_cast<size_t>(by) * static_cast<size_t>(gw) + static_cast<size_t>(bx);
					if (ws.largeObjectBlocks[idx] == 0)
						continue;
					for (int oy = -1; oy <= 1; ++oy)
					{
						const int ny = by + oy;
						if (ny < 0 || ny >= gh) continue;
						for (int ox = -1; ox <= 1; ++ox)
						{
							const int nx = bx + ox;
							if (nx < 0 || nx >= gw) continue;
							ws.nearLargeObjectBlocks[static_cast<size_t>(ny) * static_cast<size_t>(gw) + static_cast<size_t>(nx)] = 1;
						}
					}
				}
			}
		}

		bool CompareBlobPriority(const BlobCandidate& a, const BlobCandidate& b)
		{
			if (a.intSnr != b.intSnr)
				return a.intSnr > b.intSnr;
			if (a.cy != b.cy)
				return a.cy < b.cy;
			return a.cx < b.cx;
		}
	}

	void ResidualWorkspace::Reserve(int width, int height, int guardGridW, int guardGridH)
	{
		if (width <= 0 || height <= 0)
			return;
		const size_t n = static_cast<size_t>(width) * static_cast<size_t>(height);
		diffMinus.reserve(n);
		signedResY.reserve(n);
		zMapY.reserve(n);
		sigmaDiffY.reserve(n);
		bilinErrY.reserve(n);
		signedResCb.reserve(n);
		signedResCr.reserve(n);
		bilinErrCb.reserve(n);
		bilinErrCr.reserve(n);
		chi2Map.reserve(n);
		bgLuma.reserve(n);
		sampleMask.reserve(n);
		visited.reserve(n);
		const size_t cw = static_cast<size_t>((width + kCellSizeHalfPx - 1) / kCellSizeHalfPx);
		const size_t ch = static_cast<size_t>((height + kCellSizeHalfPx - 1) / kCellSizeHalfPx);
		cells.reserve(cw * ch);
		activeCellIndices.reserve(kMaxActiveCells + 1);
		bfsQueue.reserve(256);
		componentIndices.reserve(256);
		madScratch.reserve(n / 4 + 1);
		madScratchCb.reserve(n / 4 + 1);
		madScratchCr.reserve(n / 4 + 1);
		candidateScratch.reserve(kMaxBlobs * 2);
		if (guardGridW > 0 && guardGridH > 0)
		{
			const size_t gn = static_cast<size_t>(guardGridW) * static_cast<size_t>(guardGridH);
			largeObjectBlocks.reserve(gn);
			nearLargeObjectBlocks.reserve(gn);
		}
	}

	bool DetectFastObjectBlobs(
		const Config& cfg,
		const ResidualInput& in,
		ResidualWorkspace& ws,
		ResidualOutput& out)
	{
		out.Clear();
		out.causalMode = in.causalTwoBack;

		if (in.sceneDiscontinuity)
		{
			out.abstainReason |= AbstainReason::SceneDiscontinuity;
			return false;
		}
		if (!in.lumaPrev || !in.lumaCur || !in.lumaNext || in.width <= 0 || in.height <= 0 || !(in.sourceScale > 0.0f))
		{
			out.abstainReason |= AbstainReason::NoUsableCamera;
			return false;
		}
		if (!in.camPrevUsable || !in.camNextUsable)
		{
			out.abstainReason |= AbstainReason::NoUsableCamera;
			return false;
		}

		// Prepare warps into frame t's plane:
		//  * mapToPrev = H_{t-1 -> t}^{-1}
		//  * mapToNext = H_{t -> t+1} (bidirectional) OR (H_{t-1 -> t} o H_{t-2 -> t-1})^{-1} (causal two-back)
		bool invPrevOk = false;
		const CameraModel::Params mapToPrev = InvertHomography(in.camPrevToCur, &invPrevOk);
		if (!invPrevOk)
		{
			out.abstainReason |= AbstainReason::NoUsableCamera;
			return false;
		}

		CameraModel::Params mapToNext = in.camCurToNext;
		if (in.causalTwoBack)
		{
			bool compOk = false;
			const CameraModel::Params hPrev2ToCur = ComposeHomography(in.camPrevToCur, in.camCurToNext, &compOk);
			bool inv2Ok = false;
			if (compOk)
				mapToNext = InvertHomography(hPrev2ToCur, &inv2Ok);
			if (!compOk || !inv2Ok)
			{
				out.abstainReason |= AbstainReason::NoUsableCamera;
				return false;
			}
			out.abstainReason |= AbstainReason::CausalNoLookAhead;
		}

		const int w = in.width;
		const int h = in.height;
		const size_t n = static_cast<size_t>(w) * static_cast<size_t>(h);
		const double scale = static_cast<double>(in.sourceScale);

		CameraModel::Rect pic = in.picture;
		if (pic.Empty())
		{
			pic.left = 0;
			pic.top = 0;
			pic.right = static_cast<int>(std::lround(static_cast<double>(w) * scale));
			pic.bottom = static_cast<int>(std::lround(static_cast<double>(h) * scale));
		}
		const double picCx = pic.CentreX();
		const double picCy = pic.CentreY();
		const double picH = static_cast<double>(pic.Height());
		const double maxMinorDiamSrcPx = cfg.maxMinorAxisFrac * picH;
		const double maxLargeObjAreaSrcPx2 = maxMinorDiamSrcPx * maxMinorDiamSrcPx;

		const bool hasChroma = cfg.enableChromaResidual &&
			in.cbPrev != nullptr && in.cbCur != nullptr && in.cbNext != nullptr &&
			in.crPrev != nullptr && in.crCur != nullptr && in.crNext != nullptr;
		out.chromaUsed = hasChroma;

		BuildLargeObjectMasks(in, maxLargeObjAreaSrcPx2, ws);

		ws.diffMinus.assign(n, 0.0f);
		ws.signedResY.assign(n, 0.0f);
		ws.zMapY.assign(n, 0.0f);
		ws.sigmaDiffY.assign(n, 0.0f);
		ws.bilinErrY.assign(n, 0.0f);
		ws.bgLuma.assign(n, 0.0f);
		ws.sampleMask.assign(n, 0);
		if (hasChroma)
		{
			ws.signedResCb.assign(n, 0.0f);
			ws.signedResCr.assign(n, 0.0f);
			ws.bilinErrCb.assign(n, 0.0f);
			ws.bilinErrCr.assign(n, 0.0f);
			ws.chi2Map.assign(n, 0.0f);
		}

		const bool needMadLuma = (in.grainVarMap == nullptr && in.uniformGrainSigma < 0.0);
		const bool needMadChroma = hasChroma && (in.uniformChromaGrainSigma < 0.0 && in.grainVarMap == nullptr && in.uniformGrainSigma < 0.0);
		ws.madScratch.clear();
		ws.madScratchCb.clear();
		ws.madScratchCr.clear();

		const bool hasGuardGrid = (in.guardGridW > 0 && in.guardGridH > 0 && in.guardBlockSrcPx > 0.0);
		const double invGuardBlock = hasGuardGrid ? (1.0 / in.guardBlockSrcPx) : 0.0;
		const double maxValidX = static_cast<double>(w - 1);
		const double maxValidY = static_cast<double>(h - 1);

		// Pass 1a: compute camera-compensated 3-frame same-polarity residuals and collect MAD samples if needed.
		for (int hy = 0; hy < h; ++hy)
		{
			const double sy = (static_cast<double>(hy) + 0.5) * scale;
			const bool yInPic = (sy >= static_cast<double>(pic.top) && sy < static_cast<double>(pic.bottom));
			const int gby = hasGuardGrid ? std::min(std::max(static_cast<int>(std::floor(sy * invGuardBlock)), 0), in.guardGridH - 1) : 0;

			for (int hx = 0; hx < w; ++hx)
			{
				const size_t idx = static_cast<size_t>(hy) * static_cast<size_t>(w) + static_cast<size_t>(hx);
				if (!yInPic)
					continue;
				const double sx = (static_cast<double>(hx) + 0.5) * scale;
				if (sx < static_cast<double>(pic.left) || sx >= static_cast<double>(pic.right))
					continue;

				bool isCameraBlock = true;
				uint8_t sflags = 0;
				if (hasGuardGrid)
				{
					const int gbx = std::min(std::max(static_cast<int>(std::floor(sx * invGuardBlock)), 0), in.guardGridW - 1);
					const size_t gidx = static_cast<size_t>(gby) * static_cast<size_t>(in.guardGridW) + static_cast<size_t>(gbx);
					if (in.blockExcluded && in.blockExcluded[gidx] != 0)
						continue;
					if (in.waterProb && static_cast<double>(in.waterProb[gidx]) >= kWaterProbThreshold)
						continue;
					if (in.blockLabels)
					{
						const uint8_t lbl = in.blockLabels[gidx];
						if (lbl == static_cast<uint8_t>(CameraModel::Label::Overlay))
							continue;
						isCameraBlock = (lbl == static_cast<uint8_t>(CameraModel::Label::Camera));
					}
					if (!ws.largeObjectBlocks.empty() && ws.largeObjectBlocks[gidx] != 0)
						continue;
					if (!ws.nearLargeObjectBlocks.empty() && ws.nearLargeObjectBlocks[gidx] != 0)
						sflags |= kSampleNearLargeObj;
				}

				const double xc = sx - picCx;
				const double yc = sy - picCy;
				double pxC = 0.0, pyC = 0.0;
				double nxC = 0.0, nyC = 0.0;
				CameraModel::Apply(mapToPrev, xc, yc, &pxC, &pyC);
				CameraModel::Apply(mapToNext, xc, yc, &nxC, &nyC);

				const double hPrevX = (pxC + picCx) / scale - 0.5;
				const double hPrevY = (pyC + picCy) / scale - 0.5;
				const double hNextX = (nxC + picCx) / scale - 0.5;
				const double hNextY = (nyC + picCy) / scale - 0.5;

				// Exclude frame borders newly uncovered by camera motion (prevents border pan artifacts).
				if (!CameraModel::IsFiniteBits(hPrevX) || !CameraModel::IsFiniteBits(hPrevY) ||
					!CameraModel::IsFiniteBits(hNextX) || !CameraModel::IsFiniteBits(hNextY) ||
					hPrevX < 0.0 || hPrevX > maxValidX || hPrevY < 0.0 || hPrevY > maxValidY ||
					hNextX < 0.0 || hNextX > maxValidX || hNextY < 0.0 || hNextY > maxValidY)
					continue;

				const double curY = static_cast<double>(in.lumaCur[idx]);
				const double prevY = ApplyFadeNorm(BilinearSafe(in.lumaPrev, w, h, hPrevX, hPrevY), in.fadePrev, in.fadeGainPrev, in.fadeOffsetPrev);
				const double nextY = ApplyFadeNorm(BilinearSafe(in.lumaNext, w, h, hNextX, hNextY), in.fadeNext, in.fadeGainNext, in.fadeOffsetNext);
				const double dm = curY - prevY;
				const double dp = curY - nextY;
				const double rY = SamePolarityResidual(dm, dp);
				const double bPrevY = BilinearCurvatureBound(in.lumaPrev, w, h, hPrevX, hPrevY, in.fadePrev, in.fadeGainPrev);
				const double bNextY = BilinearCurvatureBound(in.lumaNext, w, h, hNextX, hNextY, in.fadeNext, in.fadeGainNext);

				ws.diffMinus[idx] = static_cast<float>(dm);
				ws.signedResY[idx] = static_cast<float>(rY);
				ws.bilinErrY[idx] = static_cast<float>(std::fmin(bPrevY, bNextY));
				ws.bgLuma[idx] = static_cast<float>(0.5 * (prevY + nextY));
				ws.sampleMask[idx] = static_cast<uint8_t>(sflags | kSampleValid);

				if (needMadLuma && isCameraBlock && ((hx & 1) == 0) && ((hy & 1) == 0))
					ws.madScratch.push_back(static_cast<float>(dm));

				if (hasChroma)
				{
					const double curCb = static_cast<double>(in.cbCur[idx]);
					const double prevCb = ApplyFadeNorm(BilinearSafe(in.cbPrev, w, h, hPrevX, hPrevY), in.fadePrev, in.fadeGainPrev, in.fadeOffsetPrev);
					const double nextCb = ApplyFadeNorm(BilinearSafe(in.cbNext, w, h, hNextX, hNextY), in.fadeNext, in.fadeGainNext, in.fadeOffsetNext);
					const double dmCb = curCb - prevCb;
					const double dpCb = curCb - nextCb;
					ws.signedResCb[idx] = static_cast<float>(SamePolarityResidual(dmCb, dpCb));
					const double bPrevCb = BilinearCurvatureBound(in.cbPrev, w, h, hPrevX, hPrevY, in.fadePrev, in.fadeGainPrev);
					const double bNextCb = BilinearCurvatureBound(in.cbNext, w, h, hNextX, hNextY, in.fadeNext, in.fadeGainNext);
					ws.bilinErrCb[idx] = static_cast<float>(std::fmin(bPrevCb, bNextCb));

					const double curCr = static_cast<double>(in.crCur[idx]);
					const double prevCr = ApplyFadeNorm(BilinearSafe(in.crPrev, w, h, hPrevX, hPrevY), in.fadePrev, in.fadeGainPrev, in.fadeOffsetPrev);
					const double nextCr = ApplyFadeNorm(BilinearSafe(in.crNext, w, h, hNextX, hNextY), in.fadeNext, in.fadeGainNext, in.fadeOffsetNext);
					const double dmCr = curCr - prevCr;
					const double dpCr = curCr - nextCr;
					ws.signedResCr[idx] = static_cast<float>(SamePolarityResidual(dmCr, dpCr));
					const double bPrevCr = BilinearCurvatureBound(in.crPrev, w, h, hPrevX, hPrevY, in.fadePrev, in.fadeGainPrev);
					const double bNextCr = BilinearCurvatureBound(in.crNext, w, h, hNextX, hNextY, in.fadeNext, in.fadeGainNext);
					ws.bilinErrCr[idx] = static_cast<float>(std::fmin(bPrevCr, bNextCr));

					if (needMadChroma && isCameraBlock && ((hx & 1) == 0) && ((hy & 1) == 0))
					{
						ws.madScratchCb.push_back(static_cast<float>(dmCb));
						ws.madScratchCr.push_back(static_cast<float>(dmCr));
					}
				}
			}
		}

		// Derived baseline noise floor: 2*sigma_g^2 + 2*sigma_q^2.
		const double sigmaQ = cfg.QuantizationSigma();
		const double twoSigmaQ2 = 2.0 * sigmaQ * sigmaQ;

		double baseDiffVarLuma = twoSigmaQ2;
		if (in.uniformGrainSigma >= 0.0)
			baseDiffVarLuma = 2.0 * in.uniformGrainSigma * in.uniformGrainSigma + twoSigmaQ2;
		else if (needMadLuma)
		{
			const double madDiffSigma = EstimateDiffSigmaFromMad(ws.madScratch);
			baseDiffVarLuma = std::fmax(madDiffSigma * madDiffSigma, twoSigmaQ2);
		}
		out.noiseSigmaEst = static_cast<float>(std::sqrt(baseDiffVarLuma));

		double baseDiffVarCb = baseDiffVarLuma;
		double baseDiffVarCr = baseDiffVarLuma;
		if (hasChroma)
		{
			if (in.uniformChromaGrainSigma >= 0.0)
			{
				baseDiffVarCb = 2.0 * in.uniformChromaGrainSigma * in.uniformChromaGrainSigma + twoSigmaQ2;
				baseDiffVarCr = baseDiffVarCb;
			}
			else if (needMadChroma)
			{
				const double madCb = EstimateDiffSigmaFromMad(ws.madScratchCb);
				const double madCr = EstimateDiffSigmaFromMad(ws.madScratchCr);
				baseDiffVarCb = std::fmax(madCb * madCb, twoSigmaQ2);
				baseDiffVarCr = std::fmax(madCr * madCr, twoSigmaQ2);
			}
		}

		const double seedSigma = cfg.seedSigma;
		const double growSigma = cfg.growSigma;
		// Exact uniform per-sample floor for a streak triggering the 4x4 matched filter: k_seed / 4 = 1.0 sigma.
		const double streakSampleSigma = seedSigma / static_cast<double>(kCellSizeHalfPx);
		const double seedChi2Dof3 = hasChroma ? Chi2Dof3ThresholdForSigma(seedSigma) : 0.0;
		const double growChi2Dof3 = hasChroma ? Chi2Dof3ThresholdForSigma(growSigma) : 0.0;
		const double streakChi2Dof3 = hasChroma ? Chi2Dof3ThresholdForSigma(streakSampleSigma) : 0.0;

		// Camera registration uncertainty converted to input-sample gradient units: sigma_H_src / sourceScale.
		const double sigmaHInSamples = std::fmax(0.0, in.camSigmaPx) / scale;
		const bool hasGrainGrid = (in.grainVarMap != nullptr && in.grainGridW > 0 && in.grainGridH > 0 && in.grainBlockSrcPx > 0.0);
		const double invGrainBlock = hasGrainGrid ? (1.0 / in.grainBlockSrcPx) : 0.0;

		const int cellW = (w + kCellSizeHalfPx - 1) / kCellSizeHalfPx;
		const int cellH = (h + kCellSizeHalfPx - 1) / kCellSizeHalfPx;
		ws.cells.assign(static_cast<size_t>(cellW) * static_cast<size_t>(cellH), CellDescriptor{});
		ws.activeCellIndices.clear();
		uint32_t totalActiveCells = 0;

		// Pass 1b: local noise/gradient normalization + 4x4 cell Neyman-Pearson matched-filter reduction.
		for (int cy = 0; cy < cellH; ++cy)
		{
			const int y0 = cy * kCellSizeHalfPx;
			const int y1 = std::min(y0 + kCellSizeHalfPx, h);
			for (int cx = 0; cx < cellW; ++cx)
			{
				const int x0 = cx * kCellSizeHalfPx;
				const int x1 = std::min(x0 + kCellSizeHalfPx, w);

				double maxAbsZ = 0.0;
				double sumSignedZ = 0.0;
				double sumSignedZCb = 0.0;
				double sumSignedZCr = 0.0;
				double maxChi2 = 0.0;
				uint16_t activeCnt = 0;
				uint16_t growCnt = 0;

				for (int hy = y0; hy < y1; ++hy)
				{
					const double sy = (static_cast<double>(hy) + 0.5) * scale;
					const int gby = hasGrainGrid ? std::min(std::max(static_cast<int>(std::floor(sy * invGrainBlock)), 0), in.grainGridH - 1) : 0;

					for (int hx = x0; hx < x1; ++hx)
					{
						const size_t idx = static_cast<size_t>(hy) * static_cast<size_t>(w) + static_cast<size_t>(hx);
						if ((ws.sampleMask[idx] & kSampleValid) == 0)
							continue;

						double diffVar = baseDiffVarLuma;
						if (hasGrainGrid)
						{
							const double sx = (static_cast<double>(hx) + 0.5) * scale;
							const int gbx = std::min(std::max(static_cast<int>(std::floor(sx * invGrainBlock)), 0), in.grainGridW - 1);
							const double gv = std::fmax(0.0, static_cast<double>(in.grainVarMap[static_cast<size_t>(gby) * static_cast<size_t>(in.grainGridW) + static_cast<size_t>(gbx)]));
							diffVar = 2.0 * gv + twoSigmaQ2;
						}

						const double gradY = in.edgeMagMap ? static_cast<double>(in.edgeMagMap[idx]) : SobelGradMag(in.lumaCur, w, h, hx, hy);
						const double regTermY = sigmaHInSamples * gradY;
						const double bilinY = static_cast<double>(ws.bilinErrY[idx]);
						const double sigmaY = std::sqrt(std::fmax(diffVar + regTermY * regTermY + bilinY * bilinY, 1e-24));
						const double zY = static_cast<double>(ws.signedResY[idx]) / sigmaY;
						const double absZY = std::fabs(zY);

						ws.sigmaDiffY[idx] = static_cast<float>(sigmaY);
						ws.zMapY[idx] = static_cast<float>(zY);

						++activeCnt;
						sumSignedZ += zY;
						if (absZY > maxAbsZ)
							maxAbsZ = absZY;

						bool isSeed = (absZY >= seedSigma);
						bool isGrow = (absZY >= growSigma);
						bool isStreakPx = (absZY >= streakSampleSigma);

						if (hasChroma)
						{
							const double gradCb = SobelGradMag(in.cbCur, w, h, hx, hy);
							const double gradCr = SobelGradMag(in.crCur, w, h, hx, hy);
							const double cVarCb = hasGrainGrid ? diffVar : baseDiffVarCb;
							const double cVarCr = hasGrainGrid ? diffVar : baseDiffVarCr;
							const double regCb = sigmaHInSamples * gradCb;
							const double regCr = sigmaHInSamples * gradCr;
							const double bilinCb = static_cast<double>(ws.bilinErrCb[idx]);
							const double bilinCr = static_cast<double>(ws.bilinErrCr[idx]);
							const double sigmaCb = std::sqrt(std::fmax(cVarCb + regCb * regCb + bilinCb * bilinCb, 1e-24));
							const double sigmaCr = std::sqrt(std::fmax(cVarCr + regCr * regCr + bilinCr * bilinCr, 1e-24));
							const double zCb = static_cast<double>(ws.signedResCb[idx]) / sigmaCb;
							const double zCr = static_cast<double>(ws.signedResCr[idx]) / sigmaCr;
							const double chi2 = zY * zY + zCb * zCb + zCr * zCr;
							ws.chi2Map[idx] = static_cast<float>(chi2);
							sumSignedZCb += zCb;
							sumSignedZCr += zCr;
							if (chi2 > maxChi2)
								maxChi2 = chi2;
							if (chi2 >= seedChi2Dof3) isSeed = true;
							if (chi2 >= growChi2Dof3) isGrow = true;
							if (chi2 >= streakChi2Dof3) isStreakPx = true;
						}

						if (isSeed) ws.sampleMask[idx] |= kSampleSeed;
						if (isGrow)
						{
							ws.sampleMask[idx] |= kSampleGrow;
							++growCnt;
						}
						if (isStreakPx) ws.sampleMask[idx] |= kSampleStreakEligible;
					}
				}

				const size_t cidx = static_cast<size_t>(cy) * static_cast<size_t>(cellW) + static_cast<size_t>(cx);
				CellDescriptor& desc = ws.cells[cidx];
				desc.activeCount = activeCnt;
				desc.growCount = growCnt;
				desc.maxAbsZ = static_cast<float>(maxAbsZ);
				desc.maxChi2 = static_cast<float>(maxChi2);

				if (activeCnt > 0)
				{
					const double invSqrtN = 1.0 / std::sqrt(static_cast<double>(activeCnt));
					const double zCell = sumSignedZ * invSqrtN;
					desc.zCell = static_cast<float>(zCell);
					double cellChi2 = zCell * zCell;
					if (hasChroma)
					{
						const double zCellCb = sumSignedZCb * invSqrtN;
						const double zCellCr = sumSignedZCr * invSqrtN;
						cellChi2 += zCellCb * zCellCb + zCellCr * zCellCr;
						desc.cellChi2 = static_cast<float>(cellChi2);
					}

					const bool streakTrig = (std::fabs(zCell) >= seedSigma) || (hasChroma && cellChi2 >= seedChi2Dof3);
					const bool peakTrig = (maxAbsZ >= seedSigma) || (hasChroma && maxChi2 >= seedChi2Dof3);
					desc.flags = kCellGuardOk;
					if (peakTrig || streakTrig)
					{
						desc.flags |= kCellActive;
						if (streakTrig)
							desc.flags |= kCellStreakTrigger;
						++totalActiveCells;
						if (ws.activeCellIndices.size() <= cfg.maxActiveCells)
							ws.activeCellIndices.push_back(static_cast<uint32_t>(cidx));
					}
				}
			}
		}

		out.activeCells = totalActiveCells;

		// Rule 4: explicit clutter overflow abstention (never silently truncate).
		if (totalActiveCells > cfg.maxActiveCells)
		{
			out.clutterOverflow = true;
			out.abstainReason |= AbstainReason::ClutterOverflow;
			return false;
		}
		if (totalActiveCells == 0)
			return true;

		// Pass 2: 8-connected hysteresis components over active cells + exact moment deconvolution.
		ws.visited.assign(n, 0);
		ws.candidateScratch.clear();

		const int dx8[8] = { -1, 0, 1, -1, 1, -1, 0, 1 };
		const int dy8[8] = { -1, -1, -1, 0, 0, 1, 1, 1 };
		const double sheppardVar = (scale * scale) / 12.0; // Exact uniform pixel-cell variance

		for (uint32_t cidx : ws.activeCellIndices)
		{
			const CellDescriptor& cell = ws.cells[cidx];
			const int cy = static_cast<int>(cidx) / cellW;
			const int cx = static_cast<int>(cidx) % cellW;
			const int y0 = cy * kCellSizeHalfPx;
			const int y1 = std::min(y0 + kCellSizeHalfPx, h);
			const int x0 = cx * kCellSizeHalfPx;
			const int x1 = std::min(x0 + kCellSizeHalfPx, w);
			const bool isStreakCell = (cell.flags & kCellStreakTrigger) != 0;

			for (int sy = y0; sy < y1; ++sy)
			{
				for (int sx = x0; sx < x1; ++sx)
				{
					const size_t seedIdx = static_cast<size_t>(sy) * static_cast<size_t>(w) + static_cast<size_t>(sx);
					if (ws.visited[seedIdx] != 0)
						continue;
					const uint8_t sm = ws.sampleMask[seedIdx];
					const bool canSeed = ((sm & kSampleSeed) != 0) || (isStreakCell && ((sm & kSampleStreakEligible) != 0));
					if (!canSeed)
						continue;

					ws.bfsQueue.clear();
					ws.componentIndices.clear();
					ws.visited[seedIdx] = 1;
					ws.bfsQueue.push_back(static_cast<int>(seedIdx));

					size_t head = 0;
					while (head < ws.bfsQueue.size())
					{
						const int cur = ws.bfsQueue[head++];
						ws.componentIndices.push_back(cur);
						const int px = cur % w;
						const int py = cur / w;

						for (int k = 0; k < 8; ++k)
						{
							const int nx = px + dx8[k];
							const int ny = py + dy8[k];
							if (nx < 0 || nx >= w || ny < 0 || ny >= h)
								continue;
							const size_t nidx = static_cast<size_t>(ny) * static_cast<size_t>(w) + static_cast<size_t>(nx);
							if (ws.visited[nidx] != 0)
								continue;
							const uint8_t nm = ws.sampleMask[nidx];
							if ((nm & kSampleValid) == 0)
								continue;

							const size_t ncidx = static_cast<size_t>(ny / kCellSizeHalfPx) * static_cast<size_t>(cellW) +
								static_cast<size_t>(nx / kCellSizeHalfPx);
							const bool nInStreakCell = (ws.cells[ncidx].flags & kCellStreakTrigger) != 0;

							bool accept = ((nm & kSampleGrow) != 0);
							if (!accept && nInStreakCell && ((nm & kSampleStreakEligible) != 0))
							{
								// On pure matched-filter diluted streak cells (no >= seedSigma peak, triggered
								// solely by coherent luma |zCell| >= seedSigma), require weak 1..3 sigma pixels
								// to share the streak's coherent luma polarity so opposite-polarity noise cannot
								// attach, while leaving isoluminant chroma streaks and dual-polarity textured
								// balls (maxAbsZ >= seedSigma) intact.
								const CellDescriptor& nc = ws.cells[ncidx];
								const bool pureLumaStreak =
									(nc.maxAbsZ < static_cast<float>(seedSigma)) &&
									(std::fabs(nc.zCell) >= static_cast<float>(seedSigma));
								if (!pureLumaStreak || (ws.zMapY[nidx] * nc.zCell > 0.0f))
									accept = true;
							}
							if (!accept)
								continue;

							ws.visited[nidx] = 1;
							ws.bfsQueue.push_back(static_cast<int>(nidx));
						}
					}

					const size_t area = ws.componentIndices.size();
					if (static_cast<double>(area) < cfg.minBlobAreaHalfPx)
						continue;

					// Weighted spatial moments & conserved photometric invariants.
					double sumW = 0.0;
					double sumWX = 0.0;
					double sumWY = 0.0;
					double sumFluxY = 0.0;
					double sumSigma2Y = 0.0;
					double sumCb = 0.0;
					double sumCr = 0.0;
					double sumChi2 = 0.0;
					double sumBg = 0.0;
					double peakSnr = 0.0;
					bool nearLarge = false;
					int minHx = w, maxHx = -1, minHy = h, maxHy = -1;

					for (int idxInt : ws.componentIndices)
					{
						const size_t idx = static_cast<size_t>(idxInt);
						const int hx = idxInt % w;
						const int hy = idxInt / w;
						if (hx < minHx) minHx = hx;
						if (hx > maxHx) maxHx = hx;
						if (hy < minHy) minHy = hy;
						if (hy > maxHy) maxHy = hy;

						const double absZY = std::fabs(static_cast<double>(ws.zMapY[idx]));
						double sampleSnr = absZY;
						if (hasChroma)
						{
							const double c2 = static_cast<double>(ws.chi2Map[idx]);
							sumChi2 += c2;
							sampleSnr = std::sqrt(std::fmax(c2, 0.0));
							sumCb += static_cast<double>(ws.signedResCb[idx]);
							sumCr += static_cast<double>(ws.signedResCr[idx]);
						}
						if (sampleSnr > peakSnr)
							peakSnr = sampleSnr;

						const double weight = std::fmax(sampleSnr, 1e-6);
						const double spx = (static_cast<double>(hx) + 0.5) * scale;
						const double spy = (static_cast<double>(hy) + 0.5) * scale;
						sumW += weight;
						sumWX += weight * spx;
						sumWY += weight * spy;
						sumFluxY += static_cast<double>(ws.signedResY[idx]);
						const double syNoise = static_cast<double>(ws.sigmaDiffY[idx]);
						sumSigma2Y += syNoise * syNoise;
						sumBg += static_cast<double>(ws.bgLuma[idx]);
						if ((ws.sampleMask[idx] & kSampleNearLargeObj) != 0)
							nearLarge = true;
					}

					if (!(sumW > 0.0))
						continue;

					const double centX = sumWX / sumW;
					const double centY = sumWY / sumW;

					double mxx = 0.0;
					double myy = 0.0;
					double mxy = 0.0;
					for (int idxInt : ws.componentIndices)
					{
						const size_t idx = static_cast<size_t>(idxInt);
						const int hx = idxInt % w;
						const int hy = idxInt / w;
						const double absZY = std::fabs(static_cast<double>(ws.zMapY[idx]));
						const double weight = std::fmax(hasChroma ? std::sqrt(std::fmax(static_cast<double>(ws.chi2Map[idx]), 0.0)) : absZY, 1e-6);
						const double dx = (static_cast<double>(hx) + 0.5) * scale - centX;
						const double dy = (static_cast<double>(hy) + 0.5) * scale - centY;
						mxx += weight * dx * dx;
						myy += weight * dy * dy;
						mxy += weight * dx * dy;
					}
					mxx = mxx / sumW + sheppardVar;
					myy = myy / sumW + sheppardVar;
					mxy = mxy / sumW;

					const double tr = mxx + myy;
					const double diff = mxx - myy;
					const double disc = std::sqrt(diff * diff + 4.0 * mxy * mxy);
					const double lambda1 = 0.5 * (tr + disc);
					const double lambda2 = std::fmax(0.0, 0.5 * (tr - disc));
					const double rEst = 2.0 * std::sqrt(lambda2);
					const double lEst = std::sqrt(12.0 * std::fmax(0.0, lambda1 - lambda2));
					const double angle = 0.5 * std::atan2(2.0 * mxy, diff);

					// Minor-axis size gate (bounds 2*r_est only; never rejects long motion-blurred streaks).
					if (2.0 * rEst > maxMinorDiamSrcPx)
						continue;

					const double rmsSigmaY = std::sqrt(std::fmax(sumSigma2Y / static_cast<double>(area), 1e-24));
					const double lumaIntSnr = std::fabs(sumFluxY) / (rmsSigmaY * std::sqrt(static_cast<double>(area)));
					double intSnr = lumaIntSnr;
					if (hasChroma)
					{
						const double meanCb = sumCb / static_cast<double>(area);
						const double meanCr = sumCr / static_cast<double>(area);
						const double cbSnr = std::fabs(meanCb) * std::sqrt(static_cast<double>(area)) / std::sqrt(std::fmax(baseDiffVarCb, 1e-24));
						const double crSnr = std::fabs(meanCr) * std::sqrt(static_cast<double>(area)) / std::sqrt(std::fmax(baseDiffVarCr, 1e-24));
						intSnr = std::sqrt(lumaIntSnr * lumaIntSnr + cbSnr * cbSnr + crSnr * crSnr);
					}

					if (peakSnr < seedSigma && intSnr < seedSigma)
						continue;

					BlobCandidate blob;
					blob.cx = static_cast<float>(centX);
					blob.cy = static_cast<float>(centY);
					blob.areaHalfPx = static_cast<float>(area);
					blob.radiusPx = static_cast<float>(rEst);
					blob.streakLenPx = static_cast<float>(lEst);
					blob.streakAngleRad = static_cast<float>(angle);
					blob.flux = static_cast<float>(sumFluxY);
					if (lumaIntSnr >= growSigma)
						blob.polarity = (sumFluxY >= 0.0) ? static_cast<int8_t>(1) : static_cast<int8_t>(-1);
					else
						blob.polarity = 0; // Isoluminant / chroma-driven blob
					blob.peakSnr = static_cast<float>(peakSnr);
					blob.intSnr = static_cast<float>(intSnr);
					blob.lumaIntSnr = static_cast<float>(lumaIntSnr);
					blob.hasChroma = hasChroma;
					if (hasChroma)
					{
						blob.cbContrast = static_cast<float>(sumCb / static_cast<double>(area));
						blob.crContrast = static_cast<float>(sumCr / static_cast<double>(area));
						blob.chromaFlux = static_cast<float>(std::hypot(sumCb, sumCr));
						blob.chromaSigma = static_cast<float>(std::sqrt(0.5 * (baseDiffVarCb + baseDiffVarCr)));
					}
					blob.meanBgLuma = static_cast<float>(sumBg / static_cast<double>(area));
					blob.nearLargeObject = nearLarge;
					if (hasGuardGrid)
					{
						const int cgbx = std::min(std::max(static_cast<int>(std::floor(centX * invGuardBlock)), 0), in.guardGridW - 1);
						const int cgby = std::min(std::max(static_cast<int>(std::floor(centY * invGuardBlock)), 0), in.guardGridH - 1);
						const size_t cgidx = static_cast<size_t>(cgby) * static_cast<size_t>(in.guardGridW) + static_cast<size_t>(cgbx);
						if (in.blockLabels && in.blockLabels[cgidx] == static_cast<uint8_t>(CameraModel::Label::Overlay))
							blob.overlapsOverlay = true;
						if (in.waterProb && static_cast<double>(in.waterProb[cgidx]) >= kWaterProbThreshold)
							blob.overlapsWater = true;
					}

					blob.maskBox.left = static_cast<int>(std::floor(static_cast<double>(minHx) * scale));
					blob.maskBox.top = static_cast<int>(std::floor(static_cast<double>(minHy) * scale));
					blob.maskBox.right = static_cast<int>(std::ceil(static_cast<double>(maxHx + 1) * scale));
					blob.maskBox.bottom = static_cast<int>(std::ceil(static_cast<double>(maxHy + 1) * scale));
					MaskClear(blob.maskBits);
					const int spanX = std::max(1, maxHx - minHx + 1);
					const int spanY = std::max(1, maxHy - minHy + 1);
					for (int idxInt : ws.componentIndices)
					{
						const int hx = idxInt % w;
						const int hy = idxInt / w;
						const int mx0 = ((hx - minHx) * kMaskDim) / spanX;
						const int mx1 = std::max(mx0 + 1, ((hx - minHx + 1) * kMaskDim) / spanX);
						const int my0 = ((hy - minHy) * kMaskDim) / spanY;
						const int my1 = std::max(my0 + 1, ((hy - minHy + 1) * kMaskDim) / spanY);
						for (int my = my0; my < my1 && my < kMaskDim; ++my)
							for (int mx = mx0; mx < mx1 && mx < kMaskDim; ++mx)
								MaskSetBit(blob.maskBits, mx, my);
					}
					blob.hasMaskBits = true;

					ws.candidateScratch.push_back(blob);
				}
			}
		}

		// Deterministic top-K ordering by descending integrated SNR.
		std::sort(ws.candidateScratch.begin(), ws.candidateScratch.end(), CompareBlobPriority);
		const uint32_t keep = static_cast<uint32_t>(std::min<size_t>(ws.candidateScratch.size(), kMaxBlobs));
		out.blobCount = keep;
		out.droppedBlobs = static_cast<uint32_t>(ws.candidateScratch.size()) - keep;
		for (uint32_t i = 0; i < keep; ++i)
			out.blobs[i] = ws.candidateScratch[i];

		return true;
	}
}
