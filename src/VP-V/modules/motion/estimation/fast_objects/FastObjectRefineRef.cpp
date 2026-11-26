// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// Stage B.5 CPU reference implementation: ROI-only coarse-to-fine masked ZNCC refinement
// (quarter -> half +/-2 -> full +/-2 -> parabolic sub-pel fit), background subtraction for
// disjoint fast movers, differential 1D motion-blur kernel synthesis, and transparent
// kCentroidVector fallback.
#include "FastObjectRefineRef.h"

#include <algorithm>
#include <cmath>

namespace FastObjects
{
	namespace
	{
		constexpr int kRefineRange = 2;             // +/-2 samples at half and full pyramid levels (FieldRefineRef law)
		constexpr float kMinRadiusForZnccPx = 3.0f; // Spec B.5: r_est < 3 px falls back to centroid vector
		constexpr float kMinZnccAcceptance = 0.25f; // Elementary 1/4 correlation floor
		constexpr float kMaxSearchRadiusPx = 64.0f; // Bounds ROI patch memory while covering 3*sigma + r_est

		inline float SampleBilinearClamp(const float* img, int w, int h, float x, float y)
		{
			if (!CameraModel::IsFiniteBits(x)) x = 0.0f;
			if (!CameraModel::IsFiniteBits(y)) y = 0.0f;
			if (x < 0.0f) x = 0.0f;
			else if (x > static_cast<float>(w - 1)) x = static_cast<float>(w - 1);
			if (y < 0.0f) y = 0.0f;
			else if (y > static_cast<float>(h - 1)) y = static_cast<float>(h - 1);
			const int x0 = static_cast<int>(std::floor(x));
			const int y0 = static_cast<int>(std::floor(y));
			const int x1 = (x0 + 1 < w) ? (x0 + 1) : (w - 1);
			const int y1 = (y0 + 1 < h) ? (y0 + 1) : (h - 1);
			const float fx = x - static_cast<float>(x0);
			const float fy = y - static_cast<float>(y0);
			const size_t r0 = static_cast<size_t>(y0) * static_cast<size_t>(w);
			const size_t r1 = static_cast<size_t>(y1) * static_cast<size_t>(w);
			const float a = img[r0 + static_cast<size_t>(x0)];
			const float b = img[r0 + static_cast<size_t>(x1)];
			const float c = img[r1 + static_cast<size_t>(x0)];
			const float d = img[r1 + static_cast<size_t>(x1)];
			return (a * (1.0f - fx) + b * fx) * (1.0f - fy) + (c * (1.0f - fx) + d * fx) * fy;
		}

		// Exact parabolic sub-pel law matching FieldRefineRef.h:107-114.
		inline float ParabolicSubPel(float cm, float c0, float cp, float& outCurvature)
		{
			const float den = cm - 2.0f * c0 + cp;
			outCurvature = den;
			if (!(den > 1e-6f))
				return 0.0f;
			const float off = (cm - cp) / (2.0f * den);
			return std::min(std::max(off, -0.5f), 0.5f);
		}

		// Convolve a 2D patch with a unit-area 1D line PSF of length `lenSamples` along unit vector (ax, ay).
		void Apply1DLineBlur(
			const float* src, int w, int h,
			float lenSamples, float ax, float ay,
			std::vector<float>& dst)
		{
			const size_t n = static_cast<size_t>(w) * static_cast<size_t>(h);
			dst.resize(n);
			const int halfTaps = std::max(1, static_cast<int>(std::ceil(0.5f * lenSamples)));
			const int numTaps = 2 * halfTaps + 1;
			const float step = lenSamples / static_cast<float>(numTaps - 1);
			const float invTaps = 1.0f / static_cast<float>(numTaps);

			for (int y = 0; y < h; ++y)
			{
				for (int x = 0; x < w; ++x)
				{
					float sum = 0.0f;
					for (int t = -halfTaps; t <= halfTaps; ++t)
					{
						const float s = static_cast<float>(t) * step;
						sum += SampleBilinearClamp(src, w, h, static_cast<float>(x) + s * ax, static_cast<float>(y) + s * ay);
					}
					dst[static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x)] = sum * invTaps;
				}
			}
		}

		// 2x2 box downsample for ROI patch (float values) and OR downsample for binary mask.
		void Downsample2x(
			const float* src, int sw, int sh,
			std::vector<float>& dst, int& dw, int& dh)
		{
			dw = std::max(1, sw / 2);
			dh = std::max(1, sh / 2);
			dst.resize(static_cast<size_t>(dw) * static_cast<size_t>(dh));
			for (int y = 0; y < dh; ++y)
			{
				const int sy0 = std::min(y * 2, sh - 1);
				const int sy1 = std::min(sy0 + 1, sh - 1);
				for (int x = 0; x < dw; ++x)
				{
					const int sx0 = std::min(x * 2, sw - 1);
					const int sx1 = std::min(sx0 + 1, sw - 1);
					const float v = 0.25f * (
						src[static_cast<size_t>(sy0) * static_cast<size_t>(sw) + static_cast<size_t>(sx0)] +
						src[static_cast<size_t>(sy0) * static_cast<size_t>(sw) + static_cast<size_t>(sx1)] +
						src[static_cast<size_t>(sy1) * static_cast<size_t>(sw) + static_cast<size_t>(sx0)] +
						src[static_cast<size_t>(sy1) * static_cast<size_t>(sw) + static_cast<size_t>(sx1)]);
					dst[static_cast<size_t>(y) * static_cast<size_t>(dw) + static_cast<size_t>(x)] = v;
				}
			}
		}

		void DownsampleMask2x(
			const uint8_t* src, int sw, int sh,
			std::vector<uint8_t>& dst, int& dw, int& dh)
		{
			dw = std::max(1, sw / 2);
			dh = std::max(1, sh / 2);
			dst.resize(static_cast<size_t>(dw) * static_cast<size_t>(dh));
			for (int y = 0; y < dh; ++y)
			{
				const int sy0 = std::min(y * 2, sh - 1);
				const int sy1 = std::min(sy0 + 1, sh - 1);
				for (int x = 0; x < dw; ++x)
				{
					const int sx0 = std::min(x * 2, sw - 1);
					const int sx1 = std::min(sx0 + 1, sw - 1);
					const uint8_t anyOn = static_cast<uint8_t>(
						(src[static_cast<size_t>(sy0) * static_cast<size_t>(sw) + static_cast<size_t>(sx0)] |
						 src[static_cast<size_t>(sy0) * static_cast<size_t>(sw) + static_cast<size_t>(sx1)] |
						 src[static_cast<size_t>(sy1) * static_cast<size_t>(sw) + static_cast<size_t>(sx0)] |
						 src[static_cast<size_t>(sy1) * static_cast<size_t>(sw) + static_cast<size_t>(sx1)]) ? 1 : 0);
					dst[static_cast<size_t>(y) * static_cast<size_t>(dw) + static_cast<size_t>(x)] = anyOn;
				}
			}
		}

		// Masked Zero-Mean Normalized Cross-Correlation between template A (wA x hA, binary mask)
		// and target patch B (wB x hB) at integer shift (ox, oy) where A(x,y) aligns with B(x+ox, y+oy).
		// Returns true and sets outZncc in [-1, 1] if both patches have non-degenerate variance on mask.
		bool EvalMaskedZncc(
			const float* patchA, const uint8_t* maskA, int wA, int hA,
			const float* patchB, int wB, int hB,
			int ox, int oy,
			float& outZncc)
		{
			double sumA = 0.0, sumB = 0.0;
			int cnt = 0;
			for (int y = 0; y < hA; ++y)
			{
				const int by = y + oy;
				if (by < 0 || by >= hB)
					continue;
				const size_t ra = static_cast<size_t>(y) * static_cast<size_t>(wA);
				const size_t rb = static_cast<size_t>(by) * static_cast<size_t>(wB);
				for (int x = 0; x < wA; ++x)
				{
					if (maskA[ra + static_cast<size_t>(x)] == 0)
						continue;
					const int bx = x + ox;
					if (bx < 0 || bx >= wB)
						continue;
					sumA += static_cast<double>(patchA[ra + static_cast<size_t>(x)]);
					sumB += static_cast<double>(patchB[rb + static_cast<size_t>(bx)]);
					++cnt;
				}
			}
			if (cnt < 4)
				return false;

			const double meanA = sumA / static_cast<double>(cnt);
			const double meanB = sumB / static_cast<double>(cnt);
			double varA = 0.0, varB = 0.0, cov = 0.0;
			for (int y = 0; y < hA; ++y)
			{
				const int by = y + oy;
				if (by < 0 || by >= hB)
					continue;
				const size_t ra = static_cast<size_t>(y) * static_cast<size_t>(wA);
				const size_t rb = static_cast<size_t>(by) * static_cast<size_t>(wB);
				for (int x = 0; x < wA; ++x)
				{
					if (maskA[ra + static_cast<size_t>(x)] == 0)
						continue;
					const int bx = x + ox;
					if (bx < 0 || bx >= wB)
						continue;
					const double da = static_cast<double>(patchA[ra + static_cast<size_t>(x)]) - meanA;
					const double db = static_cast<double>(patchB[rb + static_cast<size_t>(bx)]) - meanB;
					varA += da * da;
					varB += db * db;
					cov += da * db;
				}
			}

			const double denom = std::sqrt(varA * varB);
			if (!CameraModel::IsFiniteBits(denom) || !(denom > 1e-12))
				return false;
			const double rho = std::fmin(1.0, std::fmax(-1.0, cov / denom));
			outZncc = static_cast<float>(rho);
			return true;
		}
	}

	void RefineWorkspace::Reserve(int maxPatchSide)
	{
		if (maxPatchSide <= 0)
			return;
		const size_t n = static_cast<size_t>(maxPatchSide) * static_cast<size_t>(maxPatchSide);
		patchA.reserve(n);
		patchABlurred.reserve(n);
		patchB.reserve(n * 4);
		patchBBlurred.reserve(n * 4);
		maskA.reserve(n);
		halfA.reserve(n / 4 + 1);
		halfB.reserve(n + 1);
		halfMask.reserve(n / 4 + 1);
		quarterA.reserve(n / 16 + 1);
		quarterB.reserve(n / 4 + 1);
		quarterMask.reserve(n / 16 + 1);
	}

	void RefineFastObjectVector(
		const RefineInput& in,
		RefineWorkspace& ws,
		RefineOutput& out)
	{
		out = RefineOutput{};
		out.vx = in.predVx;
		out.vy = in.predVy;

		// Transparent kCentroidVector fallback for missing planes or tiny discs (r_est < 3 px).
		if (!in.lumaA || !in.lumaB || in.width <= 0 || in.height <= 0 || !(in.planeScale > 0.0f) ||
			in.radiusPx < kMinRadiusForZnccPx)
		{
			out.usedCentroidFallback = true;
			return;
		}

		const float scale = in.planeScale;
		const float invScale = 1.0f / scale;
		const float clampedSearchSrc = std::min(std::max(in.searchRadiusPx, 2.0f), kMaxSearchRadiusPx);
		const int searchSamples = std::max(2, static_cast<int>(std::ceil(clampedSearchSrc * invScale)));

		// Build template bounding box in plane samples (1-sample collar around maskBox for crisp silhouette contrast).
		CameraModel::Rect box = in.maskBox;
		if (box.Empty())
		{
			const int r = std::max(3, static_cast<int>(std::ceil(in.radiusPx)));
			box.left = static_cast<int>(std::floor(in.cx)) - r;
			box.top = static_cast<int>(std::floor(in.cy)) - r;
			box.right = static_cast<int>(std::ceil(in.cx)) + r + 1;
			box.bottom = static_cast<int>(std::ceil(in.cy)) + r + 1;
		}

		const int ax0 = static_cast<int>(std::floor(static_cast<float>(box.left) * invScale)) - 1;
		const int ay0 = static_cast<int>(std::floor(static_cast<float>(box.top) * invScale)) - 1;
		const int ax1 = static_cast<int>(std::ceil(static_cast<float>(box.right) * invScale)) + 1;
		const int ay1 = static_cast<int>(std::ceil(static_cast<float>(box.bottom) * invScale)) + 1;
		const int wA = ax1 - ax0;
		const int hA = ay1 - ay0;
		if (wA < 4 || hA < 4 || wA > 160 || hA > 160)
		{
			out.usedCentroidFallback = true;
			return;
		}

		// Target ROI patch B centered at predicted displacement (predVx, predVy) with +/- searchSamples margin.
		const float predDxS = in.predVx * invScale;
		const float predDyS = in.predVy * invScale;
		const int baseOffX = static_cast<int>(std::lround(predDxS));
		const int baseOffY = static_cast<int>(std::lround(predDyS));
		const int bx0 = ax0 + baseOffX - searchSamples;
		const int by0 = ay0 + baseOffY - searchSamples;
		const int wB = wA + 2 * searchSamples;
		const int hB = hA + 2 * searchSamples;

		// Decide whether background subtraction is physically valid (ball disjoint from its prior footprint).
		bool useBgSub = false;
		CameraModel::Params invCam;
		CameraModel::Rect pic = in.picture;
		if (pic.Empty())
		{
			pic.left = 0;
			pic.top = 0;
			pic.right = static_cast<int>(std::lround(static_cast<float>(in.width) * scale));
			pic.bottom = static_cast<int>(std::lround(static_cast<float>(in.height) * scale));
		}
		const double picCx = pic.CentreX();
		const double picCy = pic.CentreY();

		if (in.camUsable)
		{
			bool invOk = false;
			invCam = InvertHomography(in.camAToB, &invOk);
			if (invOk)
			{
				double camDx = 0.0, camDy = 0.0;
				CameraModel::Displacement(in.camAToB, static_cast<double>(in.cx) - picCx, static_cast<double>(in.cy) - picCy, &camDx, &camDy);
				const double relVx = static_cast<double>(in.predVx) - camDx;
				const double relVy = static_cast<double>(in.predVy) - camDy;
				const double relDisp = std::sqrt(relVx * relVx + relVy * relVy);
				if (relDisp > 2.0 * static_cast<double>(in.radiusPx) + 2.0)
					useBgSub = true;
			}
		}
		out.usedBackgroundSub = useBgSub;

		// Extract ROI template patch A and binary support mask (dilated by 1 sample for clean silhouette gradient).
		ws.patchA.resize(static_cast<size_t>(wA) * static_cast<size_t>(hA));
		ws.maskA.assign(static_cast<size_t>(wA) * static_cast<size_t>(hA), 0);
		int maskCount = 0;
		for (int y = 0; y < hA; ++y)
		{
			const int py = ay0 + y;
			const float sy = (static_cast<float>(py) + 0.5f) * scale;
			for (int x = 0; x < wA; ++x)
			{
				const int px = ax0 + x;
				const float sx = (static_cast<float>(px) + 0.5f) * scale;
				const size_t idx = static_cast<size_t>(y) * static_cast<size_t>(wA) + static_cast<size_t>(x);

				float valA = SampleBilinearClamp(in.lumaA, in.width, in.height, static_cast<float>(px), static_cast<float>(py));
				if (useBgSub)
				{
					double wx = 0.0, wy = 0.0;
					CameraModel::Apply(in.camAToB, static_cast<double>(sx) - picCx, static_cast<double>(sy) - picCy, &wx, &wy);
					const float bx = static_cast<float>((wx + picCx) * invScale - 0.5);
					const float by = static_cast<float>((wy + picCy) * invScale - 0.5);
					valA -= SampleBilinearClamp(in.lumaB, in.width, in.height, bx, by);
				}
				ws.patchA[idx] = valA;

				// Include pixel if inside support mask (or within 1-sample collar around maskBox for silhouette).
				bool onMask = MaskContainsPoint(box, in.maskBits, in.hasMaskBits, sx, sy);
				if (!onMask && in.hasMaskBits)
				{
					// Check 4-connected neighbors at +/- scale to add a 1-sample silhouette collar.
					onMask = MaskContainsPoint(box, in.maskBits, true, sx - scale, sy) ||
						MaskContainsPoint(box, in.maskBits, true, sx + scale, sy) ||
						MaskContainsPoint(box, in.maskBits, true, sx, sy - scale) ||
						MaskContainsPoint(box, in.maskBits, true, sx, sy + scale);
				}
				if (onMask)
				{
					ws.maskA[idx] = 1;
					++maskCount;
				}
			}
		}

		if (maskCount < 4)
		{
			out.usedCentroidFallback = true;
			return;
		}

		// Extract target ROI patch B.
		ws.patchB.resize(static_cast<size_t>(wB) * static_cast<size_t>(hB));
		for (int y = 0; y < hB; ++y)
		{
			const int py = by0 + y;
			const float sy = (static_cast<float>(py) + 0.5f) * scale;
			for (int x = 0; x < wB; ++x)
			{
				const int px = bx0 + x;
				const float sx = (static_cast<float>(px) + 0.5f) * scale;
				const size_t idx = static_cast<size_t>(y) * static_cast<size_t>(wB) + static_cast<size_t>(x);
				float valB = SampleBilinearClamp(in.lumaB, in.width, in.height, static_cast<float>(px), static_cast<float>(py));
				if (useBgSub)
				{
					double wx = 0.0, wy = 0.0;
					CameraModel::Apply(invCam, static_cast<double>(sx) - picCx, static_cast<double>(sy) - picCy, &wx, &wy);
					const float ax = static_cast<float>((wx + picCx) * invScale - 0.5);
					const float ay = static_cast<float>((wy + picCy) * invScale - 0.5);
					valB -= SampleBilinearClamp(in.lumaA, in.width, in.height, ax, ay);
				}
				ws.patchB[idx] = valB;
			}
		}

		// Differential 1D motion-blur PSF synthesis for accelerating/decelerating blur-stretched objects.
		// Always blur the sharper patch by |deltaBlurLen| so both templates share identical physical blur.
		const float* fullA = ws.patchA.data();
		const float* fullB = ws.patchB.data();
		const float deltaBlurSamples = in.deltaBlurLenPx * invScale;
		if (in.streakLenPx > in.radiusPx && std::fabs(deltaBlurSamples) >= 0.5f)
		{
			const float axisNorm = std::sqrt(in.blurAxisX * in.blurAxisX + in.blurAxisY * in.blurAxisY);
			const float ux = (axisNorm > 1e-6f) ? (in.blurAxisX / axisNorm) : std::cos(in.streakAngleRad);
			const float uy = (axisNorm > 1e-6f) ? (in.blurAxisY / axisNorm) : std::sin(in.streakAngleRad);
			if (deltaBlurSamples > 0.0f)
			{
				Apply1DLineBlur(ws.patchA.data(), wA, hA, deltaBlurSamples, ux, uy, ws.patchABlurred);
				fullA = ws.patchABlurred.data();
			}
			else
			{
				Apply1DLineBlur(ws.patchB.data(), wB, hB, -deltaBlurSamples, ux, uy, ws.patchBBlurred);
				fullB = ws.patchBBlurred.data();
			}
			out.usedDifferentialBlur = true;
		}

		// Coarse-to-fine pyramid: Quarter-res -> Half-res +/-2 -> Full-res +/-2.
		// In full-res patch coordinates, offset (dx, dy) in [-searchSamples, +searchSamples] maps to B index (dx + searchSamples, dy + searchSamples).
		int bestDx = 0;
		int bestDy = 0;

		if (searchSamples >= 4 && wA >= 8 && hA >= 8)
		{
			int hwA = 0, hhA = 0, hwB = 0, hhB = 0;
			Downsample2x(fullA, wA, hA, ws.halfA, hwA, hhA);
			Downsample2x(fullB, wB, hB, ws.halfB, hwB, hhB);
			DownsampleMask2x(ws.maskA.data(), wA, hA, ws.halfMask, hwA, hhA);

			int bestHalfX = 0;
			int bestHalfY = 0;
			const int halfSearch = std::max(1, searchSamples / 2);

			if (searchSamples >= 8 && hwA >= 4 && hhA >= 4)
			{
				int qwA = 0, qhA = 0, qwB = 0, qhB = 0;
				Downsample2x(ws.halfA.data(), hwA, hhA, ws.quarterA, qwA, qhA);
				Downsample2x(ws.halfB.data(), hwB, hhB, ws.quarterB, qwB, qhB);
				DownsampleMask2x(ws.halfMask.data(), hwA, hhA, ws.quarterMask, qwA, qhA);

				const int qSearch = std::max(1, searchSamples / 4);
				const int qCenter = searchSamples / 4;
				int bestQX = 0, bestQY = 0;
				float bestQScore = -2.0f;
				for (int dy = -qSearch; dy <= qSearch; ++dy)
				{
					for (int dx = -qSearch; dx <= qSearch; ++dx)
					{
						float rho = 0.0f;
						if (EvalMaskedZncc(ws.quarterA.data(), ws.quarterMask.data(), qwA, qhA,
							ws.quarterB.data(), qwB, qhB, qCenter + dx, qCenter + dy, rho) && rho > bestQScore)
						{
							bestQScore = rho;
							bestQX = dx;
							bestQY = dy;
						}
					}
				}
				bestHalfX = bestQX * 2;
				bestHalfY = bestQY * 2;

				// Half-res +/-2 refinement around quarter-res best.
				const int hCenter = searchSamples / 2;
				float bestHScore = -2.0f;
				int seedHX = bestHalfX, seedHY = bestHalfY;
				for (int dy = -kRefineRange; dy <= kRefineRange; ++dy)
				{
					const int cy = seedHY + dy;
					if (cy < -halfSearch || cy > halfSearch) continue;
					for (int dx = -kRefineRange; dx <= kRefineRange; ++dx)
					{
						const int cx = seedHX + dx;
						if (cx < -halfSearch || cx > halfSearch) continue;
						float rho = 0.0f;
						if (EvalMaskedZncc(ws.halfA.data(), ws.halfMask.data(), hwA, hhA,
							ws.halfB.data(), hwB, hhB, hCenter + cx, hCenter + cy, rho) && rho > bestHScore)
						{
							bestHScore = rho;
							bestHalfX = cx;
							bestHalfY = cy;
						}
					}
				}
			}
			else
			{
				// Directly search half-res over +/-halfSearch when search window is compact.
				const int hCenter = searchSamples / 2;
				float bestHScore = -2.0f;
				for (int dy = -halfSearch; dy <= halfSearch; ++dy)
				{
					for (int dx = -halfSearch; dx <= halfSearch; ++dx)
					{
						float rho = 0.0f;
						if (EvalMaskedZncc(ws.halfA.data(), ws.halfMask.data(), hwA, hhA,
							ws.halfB.data(), hwB, hhB, hCenter + dx, hCenter + dy, rho) && rho > bestHScore)
						{
							bestHScore = rho;
							bestHalfX = dx;
							bestHalfY = dy;
						}
					}
				}
			}
			bestDx = bestHalfX * 2;
			bestDy = bestHalfY * 2;
		}
		else
		{
			// Small search radius: direct full-res scan over +/-searchSamples.
			float bestScore = -2.0f;
			for (int dy = -searchSamples; dy <= searchSamples; ++dy)
			{
				for (int dx = -searchSamples; dx <= searchSamples; ++dx)
				{
					float rho = 0.0f;
					if (EvalMaskedZncc(fullA, ws.maskA.data(), wA, hA,
						fullB, wB, hB, searchSamples + dx, searchSamples + dy, rho) && rho > bestScore)
					{
						bestScore = rho;
						bestDx = dx;
						bestDy = dy;
					}
				}
			}
		}

		// Final Full-res +/-2 surface evaluation (5x5 = 25 candidates, fixed scan order dy outer / dx inner).
		constexpr int kSide = 2 * kRefineRange + 1;
		float costGrid[kSide * kSide];
		float znccGrid[kSide * kSide];
		bool validGrid[kSide * kSide] = {};
		int bestLocalX = 0, bestLocalY = 0;
		float bestCost = 1e30f;
		float bestZncc = -2.0f;
		float sumCost = 0.0f;
		int validCount = 0;

		const int seedFullX = std::min(std::max(bestDx, -searchSamples), searchSamples);
		const int seedFullY = std::min(std::max(bestDy, -searchSamples), searchSamples);

		for (int ly = -kRefineRange; ly <= kRefineRange; ++ly)
		{
			const int dy = seedFullY + ly;
			for (int lx = -kRefineRange; lx <= kRefineRange; ++lx)
			{
				const int dx = seedFullX + lx;
				const int gi = (ly + kRefineRange) * kSide + (lx + kRefineRange);
				costGrid[gi] = 2.0f;
				znccGrid[gi] = -1.0f;
				if (dx < -searchSamples || dx > searchSamples || dy < -searchSamples || dy > searchSamples)
					continue;
				float rho = 0.0f;
				if (!EvalMaskedZncc(fullA, ws.maskA.data(), wA, hA,
					fullB, wB, hB, searchSamples + dx, searchSamples + dy, rho))
					continue;
				const float c = 1.0f - rho;
				costGrid[gi] = c;
				znccGrid[gi] = rho;
				validGrid[gi] = true;
				sumCost += c;
				++validCount;
				if (c < bestCost)
				{
					bestCost = c;
					bestZncc = rho;
					bestLocalX = lx;
					bestLocalY = ly;
				}
			}
		}

		if (validCount == 0 || bestZncc < kMinZnccAcceptance)
		{
			out.usedCentroidFallback = true;
			out.bestZncc = (validCount > 0) ? bestZncc : 0.0f;
			out.bestCost = (validCount > 0) ? bestCost : 1.0f;
			out.meanCost = (validCount > 0) ? (sumCost / static_cast<float>(validCount)) : 1.0f;
			return;
		}

		// Parabolic sub-pel fit on interior optimum (exact FieldRefineRef law).
		float subX = 0.0f, subY = 0.0f;
		float curvX = 0.0f, curvY = 0.0f;
		const int bxIdx = bestLocalX + kRefineRange;
		const int byIdx = bestLocalY + kRefineRange;
		const int centerGi = byIdx * kSide + bxIdx;

		if (bestLocalX > -kRefineRange && bestLocalX < kRefineRange &&
			validGrid[byIdx * kSide + (bxIdx - 1)] && validGrid[byIdx * kSide + (bxIdx + 1)])
		{
			subX = ParabolicSubPel(
				costGrid[byIdx * kSide + (bxIdx - 1)],
				costGrid[centerGi],
				costGrid[byIdx * kSide + (bxIdx + 1)],
				curvX);
		}
		if (bestLocalY > -kRefineRange && bestLocalY < kRefineRange &&
			validGrid[(byIdx - 1) * kSide + bxIdx] && validGrid[(byIdx + 1) * kSide + bxIdx])
		{
			subY = ParabolicSubPel(
				costGrid[(byIdx - 1) * kSide + bxIdx],
				costGrid[centerGi],
				costGrid[(byIdx + 1) * kSide + bxIdx],
				curvY);
		}

		// Textureless/flat curvature floor check (spec B.5).
		const bool interior = (bestLocalX > -kRefineRange && bestLocalX < kRefineRange &&
			bestLocalY > -kRefineRange && bestLocalY < kRefineRange);
		if (interior && !(curvX > 1e-6f) && !(curvY > 1e-6f))
		{
			out.usedCentroidFallback = true;
			out.bestZncc = bestZncc;
			out.bestCost = bestCost;
			out.meanCost = sumCost / static_cast<float>(validCount);
			return;
		}

		const float finalDxSamples = static_cast<float>(baseOffX + seedFullX + bestLocalX) + subX;
		const float finalDySamples = static_cast<float>(baseOffY + seedFullY + bestLocalY) + subY;
		out.vx = finalDxSamples * scale;
		out.vy = finalDySamples * scale;
		out.bestZncc = bestZncc;
		out.bestCost = bestCost;
		out.meanCost = sumCost / static_cast<float>(validCount);
		out.matchMargin = (out.meanCost > 1e-6f)
			? std::min(std::max(1.0f - out.bestCost / out.meanCost, 0.0f), 1.0f)
			: 0.0f;
		out.usedCentroidFallback = false;
	}
}
