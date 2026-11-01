// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// Camera link (K2-1): the UPLOAD form of P8. The texels are exactly what the CPU accessor reads for every block, so the CPU
// and GPU consumers agree bit for bit: (modelX, modelY, sqrtf(resX^2 + resY^2), code) with code = label + 4 * tinyMover.
// This is the layout of the LabelPass output (CameraModelGpu.h) minus the zero-candidate bit, which the accessor never reads
// (it tests only the low two bits). Pure, header-only, C++14, deterministic.
#pragma once

#include <VP-V/modules/motion/estimation/global_model/CameraMotion.h>

#include <cmath>
#include <cstddef>
#include <vector>

namespace CameraLink
{
	// False when the record has no per-block arrays on the CPU (labelsOnGpu, objects off + cpuLabels false) or they are short.
	inline bool PackLabelTexels(const CameraModel::BlockData& b, std::vector<float>& out4)
	{
		const size_t n = static_cast<size_t>(b.gridW > 0 ? b.gridW : 0) * static_cast<size_t>(b.gridH > 0 ? b.gridH : 0);
		if (n == 0 || b.label.size() < n || b.modelX.size() < n || b.modelY.size() < n || b.resX.size() < n || b.resY.size() < n)
			return false;
		out4.assign(n * 4, 0.0f);
		for (size_t i = 0; i < n; ++i)
		{
			const float rx = b.resX[i], ry = b.resY[i];
			const float res = std::sqrt(rx * rx + ry * ry);   // the same float expression as CameraAccessor.h FieldVector
			const int tiny = (b.tinyMover.size() > i && b.tinyMover[i] != 0) ? 1 : 0;
			out4[i * 4 + 0] = b.modelX[i];
			out4[i * 4 + 1] = b.modelY[i];
			out4[i * 4 + 2] = res;
			out4[i * 4 + 3] = static_cast<float>(static_cast<int>(b.label[i]) + 4 * tiny);
		}
		return true;
	}
}
