// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// Long-pair fallback: the seeded block search on the 4x4 cell grid of 8-bit half luma (CPU reference of the matcher's
// seeded search; the bound matcher backend replaces it at P5a through ISeededBackend). Header-only, pure C++14.
//
// Law (mirrored exactly by tools/eval/motion_bench/longpair_bench.py `seeded_search`): per cell a window of 8x8 half
// px centred on the 4x4 cell; seed rounded with floor(x + 0.5); +-radius (default 6) integer offsets around it; cost =
// SAD of A(window) against B shifted by seed + offset (integer indices, edge clamp); argmin with the first lowest in scan
// order (dy outer, dx inner); parabolic sub-pel per axis (only strictly inside the range and with curvature > 1e-6,
// clamped to +-0.5); vector = rounded seed + offset + sub-pel; cost u8 = min(255, round(SAD * 255 / 2048)).
// Backward = the same search B -> A with the seed -hint at the same cell. The grid is ceil(size / 4) (cells past the
// edge clamp), like the engine's.
#pragma once

#include "LongPairTypes.h"

namespace VpLongPair
{
	constexpr int kSearchRadius = 6;

	inline float SubPel(int cm, int c0, int cp)
	{
		const double den = static_cast<double>(cm) - 2.0 * c0 + cp;
		if (!(den > 1e-6))
			return 0.0f;
		const double off = (static_cast<double>(cm) - cp) / (2.0 * den);
		return static_cast<float>(std::min(std::max(off, -0.5), 0.5));
	}

	// One direction. seedS105: gw*gh*2 S10.5 (x, y), already negated for the backward direction. Writes S10.5 vectors + cost.
	inline void SearchDirection(const uint8_t* a, const uint8_t* b, int w, int h, const std::vector<int16_t>& seedS105,
		int radius, std::vector<int16_t>& vec, std::vector<uint8_t>& cost)
	{
		const int gw = GridOf(w), gh = GridOf(h), win = 8, margin = 2, n = 2 * radius + 1;
		vec.assign(static_cast<size_t>(gw) * gh * 2, 0);
		cost.assign(static_cast<size_t>(gw) * gh, 0);
		std::vector<int> costs(static_cast<size_t>(n) * n);
		auto cl = [](int v, int hi) { return v < 0 ? 0 : v > hi ? hi : v; };
		for (int cy = 0; cy < gh; ++cy)
			for (int cx = 0; cx < gw; ++cx)
			{
				const size_t ci = static_cast<size_t>(cy) * gw + cx;
				const int sx = static_cast<int>(std::floor(FromS105(seedS105[ci * 2]) + 0.5f));
				const int sy = static_cast<int>(std::floor(FromS105(seedS105[ci * 2 + 1]) + 0.5f));
				const int ox = cx * kGrid - margin, oy = cy * kGrid - margin;
				int best = 0, bestIdx = -1;
				for (int dy = -radius; dy <= radius; ++dy)
					for (int dx = -radius; dx <= radius; ++dx)
					{
						int sad = 0;
						for (int j = 0; j < win; ++j)
						{
							const int ay = cl(oy + j, h - 1), by = cl(oy + j + sy + dy, h - 1);
							for (int i = 0; i < win; ++i)
							{
								const int av = a[static_cast<size_t>(ay) * w + cl(ox + i, w - 1)];
								const int bv = b[static_cast<size_t>(by) * w + cl(ox + i + sx + dx, w - 1)];
								sad += av > bv ? av - bv : bv - av;
							}
						}
						const int idx = (dy + radius) * n + (dx + radius);
						costs[idx] = sad;
						if (bestIdx < 0 || sad < best)
						{
							best = sad;
							bestIdx = idx;
						}
					}
				const int bx = bestIdx % n, by = bestIdx / n;
				float subX = 0.0f, subY = 0.0f;
				if (bx > 0 && bx < n - 1)
					subX = SubPel(costs[by * n + bx - 1], best, costs[by * n + bx + 1]);
				if (by > 0 && by < n - 1)
					subY = SubPel(costs[(by - 1) * n + bx], best, costs[(by + 1) * n + bx]);
				vec[ci * 2] = ToS105(static_cast<float>(sx + (bx - radius)) + subX);
				vec[ci * 2 + 1] = ToS105(static_cast<float>(sy + (by - radius)) + subY);
				cost[ci] = static_cast<uint8_t>(std::min(255, static_cast<int>(std::floor(best * 255.0 / 2048.0 + 0.5))));
			}
	}

	// Both directions. `hint` may be null or invalid (zero seed).
	inline bool SeededSearchPair(const uint8_t* a, const uint8_t* b, int w, int h, const HintField* hint, Field& out, int radius = kSearchRadius)
	{
		if (!a || !b || w <= 0 || h <= 0)
			return false;
		out.gridW = GridOf(w);
		out.gridH = GridOf(h);
		std::vector<int16_t> seed(static_cast<size_t>(out.gridW) * out.gridH * 2, 0);
		if (hint && hint->Valid())
			ResampleHint(*hint, w, h, seed);
		SearchDirection(a, b, w, h, seed, radius, out.forward, out.forwardCost);
		for (auto& v : seed)
			v = static_cast<int16_t>(-v);
		SearchDirection(b, a, w, h, seed, radius, out.backward, out.backwardCost);
		return true;
	}
}
