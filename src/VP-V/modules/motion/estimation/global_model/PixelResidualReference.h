// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// Global camera model: CPU reference (and shader-mirror spec) of the per-block tiny-mover
// measure. NOT called by the runtime path in this phase (the GPU produces it in phase 2).
#pragma once

#include <vector>

#include "CameraMotion.h"

namespace CameraModel
{
	// For every block of the gridW x gridH grid (blockSize source px, anchored at the frame's
	// top-left), the share (0..1) of the block's pixels whose absolute difference
	//   | cur(x) - prev(Inverse(camera)(x)) |   (bilinear sample of prev, clamped at the borders)
	// exceeds k * noiseRms. `prev` and `cur` are width*height single-channel float luma images
	// (the same units as noiseRms). `camera` maps a point of the previous frame to the current
	// frame in picture-centred coordinates (see CameraMotion.h); `picture` locates the centre.
	// Pixels are visited at their centres (x + 0.5, y + 0.5). Blocks partly outside the image use
	// the pixels inside. Output vector is resized to gridW*gridH. Fixed loop order. If the camera
	// is singular, share = 0 everywhere and false is returned.
	bool PixelResidualReference(const float* prev, const float* cur, int width, int height,
		const Params& camera, const Rect& picture, int gridW, int gridH, double blockSize,
		double k, double noiseRms, std::vector<float>& share);
}
