// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#include "GlobalModelSearchHints.h"

#include <cmath>

namespace CameraModel
{
	SearchHints BuildHints(const HintConfig& cfg, const CameraMotion* prev, const float* prevFwdX, const float* prevFwdY,
		const float* nvofX, const float* nvofY, bool cut, bool fade, bool flash, double fadeGain, double fadeOffset,
		double precisionPx)
	{
		SearchHints out;
		out.scene = cut ? (cfg.cutSkip ? SceneAction::SkipAcrossCut : SceneAction::Normal)
			: flash ? (cfg.flashBridge ? SceneAction::BridgeFlash : SceneAction::Normal)
			: fade ? (cfg.fadeNormalised ? SceneAction::FadeNormalised : SceneAction::Normal)
			: SceneAction::Normal;
		out.fadeGain = fadeGain;
		out.fadeOffset = fadeOffset;

		if (prev == nullptr || !prev->blocks) return out;
		const BlockData& b = *prev->blocks;
		if (b.gridW <= 0 || b.gridH <= 0) return out;
		const size_t n = static_cast<size_t>(b.gridW) * static_cast<size_t>(b.gridH);
		out.gridW = b.gridW;
		out.gridH = b.gridH;
		out.hint.assign(n, static_cast<uint8_t>(BlockHint::FullSearch));
		out.seedX.assign(n, 0.0f);
		out.seedY.assign(n, 0.0f);

		const double precision = precisionPx > 0.0 ? precisionPx : 0.25 * b.blockSize / 16.0;
		out.narrowRadiusPx = cfg.narrowRadiusSigmas * precision;
		const bool haveModel = b.modelX.size() >= n && b.modelY.size() >= n;
		const bool haveLabel = b.label.size() >= n;
		if (haveModel)
			for (size_t i = 0; i < n; ++i) { out.seedX[i] = b.modelX[i]; out.seedY[i] = b.modelY[i]; }

		if (!prev->valid || !haveLabel)
		{
			out.fullBlocks = static_cast<uint32_t>(n);
			return out;
		}

		const bool confident = prev->confidence >= cfg.minConfidence;
		const double agree = cfg.agreeSigmas * precision;
		const bool useFwd = cfg.temporalSeed && prevFwdX != nullptr && prevFwdY != nullptr;
		const bool haveNvof = nvofX != nullptr && nvofY != nullptr;
		for (size_t i = 0; i < n; ++i)
		{
			const uint8_t label = b.label[i];
			uint8_t h = static_cast<uint8_t>(BlockHint::FullSearch);
			if (label == static_cast<uint8_t>(Label::Overlay))
				h = static_cast<uint8_t>(BlockHint::Excluded);
			else if (cfg.tinyMoverRoi && label == static_cast<uint8_t>(Label::Object) &&
				b.tinyMover.size() > i && b.tinyMover[i] == 1)
				h = static_cast<uint8_t>(BlockHint::FinerGrid);
			else if (confident)
			{
				bool eligible = label == static_cast<uint8_t>(Label::Camera);
				if (!eligible && label == static_cast<uint8_t>(Label::Object) && b.objectId.size() > i && b.objectId[i] != 0)
					for (const ObjectModel& obj : prev->objects)
						if (obj.id == b.objectId[i]) { eligible = true; break; }
				// The seed comes from the previous forward vectors, else the model; the reference it
				// must agree with is the engine vector, else the model vector.
				if (eligible && (useFwd || haveModel) && (haveNvof || haveModel))
				{
					const double sx = useFwd ? prevFwdX[i] : b.modelX[i];
					const double sy = useFwd ? prevFwdY[i] : b.modelY[i];
					const double rx = haveNvof ? nvofX[i] : b.modelX[i];
					const double ry = haveNvof ? nvofY[i] : b.modelY[i];
					if (std::hypot(sx - rx, sy - ry) <= agree)
					{
						h = static_cast<uint8_t>(BlockHint::NarrowAroundPrediction);
						out.seedX[i] = static_cast<float>(sx);
						out.seedY[i] = static_cast<float>(sy);
					}
				}
			}
			out.hint[i] = h;
		}

		for (size_t i = 0; i < n; ++i)
		{
			switch (static_cast<BlockHint>(out.hint[i]))
			{
			case BlockHint::FullSearch: ++out.fullBlocks; break;
			case BlockHint::NarrowAroundPrediction: ++out.narrowBlocks; break;
			case BlockHint::FinerGrid: ++out.finerBlocks; break;
			case BlockHint::Excluded: break;
			}
		}

		// ROIs: 4-connected FinerGrid components, raster-order flood fill with an explicit stack.
		const size_t gw = static_cast<size_t>(b.gridW);
		std::vector<uint8_t> seen(n, 0);
		std::vector<size_t> stack;
		for (size_t start = 0; start < n; ++start)
		{
			if (out.hint[start] != static_cast<uint8_t>(BlockHint::FinerGrid) || seen[start]) continue;
			SearchHints::Roi roi;
			roi.bx0 = static_cast<int>(start % gw);
			roi.by0 = static_cast<int>(start / gw);
			roi.bx1 = roi.bx0 + 1;
			roi.by1 = roi.by0 + 1;
			seen[start] = 1;
			stack.push_back(start);
			while (!stack.empty())
			{
				const size_t cur = stack.back();
				stack.pop_back();
				const int x = static_cast<int>(cur % gw), y = static_cast<int>(cur / gw);
				if (x < roi.bx0) roi.bx0 = x;
				if (y < roi.by0) roi.by0 = y;
				if (x + 1 > roi.bx1) roi.bx1 = x + 1;
				if (y + 1 > roi.by1) roi.by1 = y + 1;
				const int nx[4] = { x - 1, x + 1, x, x };
				const int ny[4] = { y, y, y - 1, y + 1 };
				for (int k = 0; k < 4; ++k)
				{
					if (nx[k] < 0 || ny[k] < 0 || nx[k] >= b.gridW || ny[k] >= b.gridH) continue;
					const size_t ni = static_cast<size_t>(ny[k]) * gw + static_cast<size_t>(nx[k]);
					if (seen[ni] || out.hint[ni] != static_cast<uint8_t>(BlockHint::FinerGrid)) continue;
					seen[ni] = 1;
					stack.push_back(ni);
				}
			}
			out.rois.push_back(roi);
		}
		return out;
	}
}
