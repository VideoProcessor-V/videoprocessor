// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// Global camera model: CPU reference of the per-block tiny-mover measure. Independent of
// GlobalModelGeometry (uses a private 3x3 inverse).
#include "PixelResidualReference.h"

#include <cmath>

namespace CameraModel
{
	namespace
	{
		bool Inverse3(const double* m, double* r)
		{
			const double c00 = m[4] * m[8] - m[5] * m[7];
			const double c01 = m[5] * m[6] - m[3] * m[8];
			const double c02 = m[3] * m[7] - m[4] * m[6];
			const double det = m[0] * c00 + m[1] * c01 + m[2] * c02;
			// Relative singularity test against the matrix's own magnitude.
			double mag = 0;
			for (int i = 0; i < 9; ++i) mag = std::fmax(mag, std::fabs(m[i]));
			if (!std::isfinite(det) || !(mag > 0) || std::fabs(det) <= 1e-12 * mag * mag * mag) return false;
			const double id = 1.0 / det;
			r[0] = c00 * id;
			r[1] = (m[2] * m[7] - m[1] * m[8]) * id;
			r[2] = (m[1] * m[5] - m[2] * m[4]) * id;
			r[3] = c01 * id;
			r[4] = (m[0] * m[8] - m[2] * m[6]) * id;
			r[5] = (m[2] * m[3] - m[0] * m[5]) * id;
			r[6] = c02 * id;
			r[7] = (m[1] * m[6] - m[0] * m[7]) * id;
			r[8] = (m[0] * m[4] - m[1] * m[3]) * id;
			return true;
		}

		inline double Bilinear(const float* img, int w, int h, double sx, double sy)
		{
			if (!std::isfinite(sx)) sx = 0;
			if (!std::isfinite(sy)) sy = 0;
			if (sx < 0) sx = 0;
			if (sy < 0) sy = 0;
			if (sx > w - 1) sx = w - 1;
			if (sy > h - 1) sy = h - 1;
			const int x0 = static_cast<int>(std::floor(sx));
			const int y0 = static_cast<int>(std::floor(sy));
			const int x1 = x0 + 1 < w ? x0 + 1 : w - 1;
			const int y1 = y0 + 1 < h ? y0 + 1 : h - 1;
			const double fx = sx - x0, fy = sy - y0;
			const double a = img[static_cast<size_t>(y0) * w + x0], b = img[static_cast<size_t>(y0) * w + x1];
			const double c = img[static_cast<size_t>(y1) * w + x0], d = img[static_cast<size_t>(y1) * w + x1];
			return (a * (1 - fx) + b * fx) * (1 - fy) + (c * (1 - fx) + d * fx) * fy;
		}
	}

	bool PixelResidualReference(const float* prev, const float* cur, int width, int height,
		const Params& camera, const Rect& picture, int gridW, int gridH, double blockSize,
		double k, double noiseRms, std::vector<float>& share)
	{
		const size_t cells = (gridW > 0 && gridH > 0) ? static_cast<size_t>(gridW) * static_cast<size_t>(gridH) : 0;
		share.assign(cells, 0.0f);
		if (!prev || !cur || width <= 0 || height <= 0 || cells == 0 || !(blockSize > 0)) return false;
		double inv[9];
		if (!Inverse3(camera.h, inv)) return false;
		Params ip;
		ip.type = camera.type;
		for (int i = 0; i < 9; ++i) ip.h[i] = inv[i];

		const double threshold = k * noiseRms;
		const double cx = picture.CentreX(), cy = picture.CentreY();
		std::vector<uint32_t> count(cells, 0), pixels(cells, 0);
		for (int py = 0; py < height; ++py)
		{
			const int by = static_cast<int>(std::floor(py / blockSize));
			if (by < 0 || by >= gridH) continue;
			for (int px = 0; px < width; ++px)
			{
				const int bx = static_cast<int>(std::floor(px / blockSize));
				if (bx < 0 || bx >= gridW) continue;
				const size_t cell = static_cast<size_t>(by) * gridW + bx;
				double ox = 0, oy = 0;
				Apply(ip, px + 0.5 - cx, py + 0.5 - cy, &ox, &oy);
				const double s = Bilinear(prev, width, height, ox + cx - 0.5, oy + cy - 0.5);
				const double diff = std::fabs(static_cast<double>(cur[static_cast<size_t>(py) * width + px]) - s);
				++pixels[cell];
				if (diff > threshold) ++count[cell];
			}
		}
		for (size_t i = 0; i < cells; ++i)
			share[i] = pixels[i] ? static_cast<float>(static_cast<double>(count[i]) / pixels[i]) : 0.0f;
		return true;
	}
}
