// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// input.source_analysis/regions: the CPU reference of the owner-filled S5 busy-share variant (lead8/iface-L-B5.md section B):
//   BusyShare: the share of "busy" pixels per consumer block (VpDebander GUARD_BODY arithmetic, moved).
// In the plane's own code units (callers pass 10-bit tap codes); border = clamp to edge. Header-only, C++14, no Windows or
// libplacebo headers. The GLSL mirrors are the owner's lane kernel; these are the tolerance oracle and the tests' reference.
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

namespace RegionEvidence
{
	namespace BusyEdge
	{
		// One plane group: n component planes of w x h samples (row-major, tightly packed), n = 1..3.
		struct PlaneGroup
		{
			int w = 0, h = 0, n = 1;
			const float* comp[3] = { nullptr, nullptr, nullptr };
			float At(int c, int x, int y) const
			{
				x = (std::max)(0, (std::min)(w - 1, x));
				y = (std::max)(0, (std::min)(h - 1, y));
				return comp[c][static_cast<size_t>(y) * w + x];
			}
		};

		// Busy at (x, y): Laplacian |sum of the 4 neighbour differences| above lapMin, or a strict local extremum along a line
		// (h, v or a diagonal) by more than extMin on both sides; the largest over the group's components.
		inline bool IsBusy(const PlaneGroup& g, int x, int y, float lapMin, float extMin)
		{
			float m = 0.0f, e = 0.0f;
			for (int k = 0; k < g.n; ++k)
			{
				const float c = g.At(k, x, y);
				const float dw = c - g.At(k, x - 1, y), de = c - g.At(k, x + 1, y);
				const float dn = c - g.At(k, x, y - 1), ds = c - g.At(k, x, y + 1);
				const float d1 = c - g.At(k, x - 1, y - 1), d2 = c - g.At(k, x + 1, y + 1);
				const float d3 = c - g.At(k, x + 1, y - 1), d4 = c - g.At(k, x - 1, y + 1);
				const float l = std::fabs(dw + de + dn + ds);
				auto ext = [](float a, float b) { return (std::max)((std::min)(a, b), -(std::max)(a, b)); };
				const float xe = (std::max)((std::max)(ext(dw, de), ext(dn, ds)), (std::max)(ext(d1, d2), ext(d3, d4)));
				m = (std::max)(m, l);
				e = (std::max)(e, xe);
			}
			return m > lapMin || e > extMin;
		}

		// The share per block x block cell (ceil(w / block) x ceil(h / block), row-major), over in-plane pixels only.
		inline std::vector<float> BusyShare(const PlaneGroup& g, int block, float lapMin, float extMin)
		{
			const int gw = (g.w + block - 1) / block, gh = (g.h + block - 1) / block;
			std::vector<float> out(static_cast<size_t>(gw) * gh, 0.0f);
			for (int by = 0; by < gh; ++by)
				for (int bx = 0; bx < gw; ++bx)
				{
					double busy = 0.0, count = 0.0;
					for (int y = by * block; y < (std::min)(g.h, (by + 1) * block); ++y)
						for (int x = bx * block; x < (std::min)(g.w, (bx + 1) * block); ++x)
						{
							busy += IsBusy(g, x, y, lapMin, extMin) ? 1.0 : 0.0;
							count += 1.0;
						}
					out[static_cast<size_t>(by) * gw + bx] = static_cast<float>(busy / (std::max)(count, 1.0));
				}
			return out;
		}
	}
}
