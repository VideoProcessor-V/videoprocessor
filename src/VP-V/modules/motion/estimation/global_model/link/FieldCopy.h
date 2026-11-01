// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// Camera link (K1): the CPU copy of one verified P3 field, in SOURCE pixels, as the fit and the guidance plan read it.
// Pure, header-only, C++14, no Windows, no GPU.
#pragma once

#include <VP-V/core/Indicator.h>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace CameraLink
{
	struct FieldCopy
	{
		uint64_t seq = 0, pairFrom = 0, pairTo = 0;
		int gridW = 0, gridH = 0;
		double blockSize = 0;                       // source px per block
		std::vector<float> fwdX, fwdY, fwdTrust;    // source px, gridW*gridH
		std::vector<float> bwdX, bwdY, bwdTrust;    // empty when no backward field
		VpIndicator::Stamp stamp;                   // the stamp the field was computed under
		bool HaveBackward() const { return !bwdX.empty(); }
		size_t Blocks() const { return static_cast<size_t>(gridW > 0 ? gridW : 0) * static_cast<size_t>(gridH > 0 ? gridH : 0); }
		bool Valid() const { return gridW > 0 && gridH > 0 && blockSize > 0 && fwdX.size() == Blocks(); }
	};

	// Unpack an rgba32f texel array (vx, vy, trust, unused per block; gridW*gridH*4 floats) into the planes, scaling the
	// vectors by `scaleToSourcePx`. Trust is copied as is. Deterministic, order independent.
	inline void UnpackRgba(const float* rgba, size_t blocks, float scaleToSourcePx,
		std::vector<float>& x, std::vector<float>& y, std::vector<float>& trust)
	{
		x.resize(blocks);
		y.resize(blocks);
		trust.resize(blocks);
		for (size_t i = 0; i < blocks; ++i)
		{
			x[i] = rgba[i * 4 + 0] * scaleToSourcePx;
			y[i] = rgba[i * 4 + 1] * scaleToSourcePx;
			trust[i] = rgba[i * 4 + 2];
		}
	}
}
