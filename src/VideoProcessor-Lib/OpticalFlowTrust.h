#pragma once

// The optical-flow engine's extras as trust inputs (docs/VP-nvof-ahead.md, owner
// 2026-10-16: "take and vector differences etc that it allows that fed into our
// path"). CPU-only maths, run on the producer's worker (or for a few thousand
// blocks on the render thread), so it is testable without a GPU:
//  - the engine's per-cell cost, normalised per block (block size and grid
//    general) against the field's own distribution (median and spread), as a
//    weight in [0, 1]: 1 = the engine is as sure as usual, 0 = far less sure;
//  - the forward/backward consistency of the engine's own two directions (the
//    engine computes both in one run, so it costs no extra engine time), per
//    cell in the engine's input pixels, and per block as a weight in [0, 1];
//  - whether the engine's whole-frame motion (D1 format) is usable, so the
//    global search runs instead when too few cells agree.
// The weights multiply into the de-judder's search texture (w) and raise a
// block's match error in its consistency pass: a low weight lowers the trust,
// so the matcher's own vector, or no motion, is preferred there. Weight 1
// changes nothing.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <vector>

namespace OpticalFlowTrust
{
	// 1 below lo, 0 above hi, smooth between (GLSL's 1 - smoothstep).
	inline float FallOff(float lo, float hi, float x)
	{
		if (!(hi > lo)) return x <= lo ? 1.0f : 0.0f;
		float t = (x - lo) / (hi - lo);
		t = t < 0.0f ? 0.0f : t > 1.0f ? 1.0f : t;
		return 1.0f - t * t * (3.0f - 2.0f * t);
	}

	// The engine's cells per de-judder block along one axis (a block of blockPx input
	// pixels over a grid of `grid` pixels per vector): at least 1.
	inline int CellsPerBlock(int grid, int blockPx)
	{
		return grid > 0 && blockPx >= grid ? blockPx / grid : 1;
	}

	// How the cost is judged against the field's own distribution: a block is fully
	// trusted up to median + lo x spread and not at all from median + hi x spread; the
	// spread is the cells' median absolute deviation (x 1.4826, a standard deviation for
	// normal data) and at least floorCodes engine cost codes (a uniform field: no
	// down-weight). Relative, so the engine's cost scale (grid, preset, driver) does not
	// matter; conservative, so only clearly unsure blocks lose trust.
	struct CostScale
	{
		float lo = 3.0f;
		float hi = 8.0f;
		float floorCodes = 4.0f;
	};

	// The field's cost median and spread (8-bit costs, exact, by histogram). False when
	// there are no cells.
	inline bool CostStatistics(const std::vector<uint8_t>& costs, size_t cells, float& median, float& spread,
		const CostScale& scale = CostScale())
	{
		median = 0.0f;
		spread = scale.floorCodes;
		if (cells == 0 || costs.size() < cells) return false;
		size_t histogram[256] = {};
		for (size_t i = 0; i < cells; ++i) ++histogram[costs[i]];
		const size_t half = (cells - 1) / 2;   // the lower median
		size_t seen = 0;
		int m = 0;
		for (; m < 256; ++m)
		{
			seen += histogram[m];
			if (seen > half) break;
		}
		size_t deviations[256] = {};
		for (int v = 0; v < 256; ++v) deviations[std::abs(v - m)] += histogram[v];
		seen = 0;
		int d = 0;
		for (; d < 256; ++d)
		{
			seen += deviations[d];
			if (seen > half) break;
		}
		median = static_cast<float>(m);
		spread = (std::max)(scale.floorCodes, 1.4826f * static_cast<float>(d));
		return true;
	}

	// The per-block cost weight (blocksX x blocksY, row-major): each block's mean cell
	// cost (the cells it covers; edge blocks the cells that exist), against the field's
	// distribution. False (out empty) when the inputs do not fit: then no weight is
	// applied (exactly as without the engine's cost).
	inline bool CostWeights(const std::vector<uint8_t>& costs, int gridW, int gridH, int grid, int blockPx,
		int blocksX, int blocksY, std::vector<float>& out, const CostScale& scale = CostScale())
	{
		out.clear();
		if (gridW <= 0 || gridH <= 0 || blocksX <= 0 || blocksY <= 0) return false;
		const size_t cells = static_cast<size_t>(gridW) * gridH;
		float median = 0.0f, spread = 0.0f;
		if (!CostStatistics(costs, cells, median, spread, scale)) return false;
		const float lo = median + scale.lo * spread, hi = median + scale.hi * spread;
		const int per = CellsPerBlock(grid, blockPx);
		out.assign(static_cast<size_t>(blocksX) * blocksY, 1.0f);
		for (int by = 0; by < blocksY; ++by)
			for (int bx = 0; bx < blocksX; ++bx)
			{
				unsigned sum = 0, n = 0;
				for (int cy = by * per; cy < (by + 1) * per && cy < gridH; ++cy)
					for (int cx = bx * per; cx < (bx + 1) * per && cx < gridW; ++cx)
					{
						sum += costs[static_cast<size_t>(cy) * gridW + cx];
						++n;
					}
				if (n) out[static_cast<size_t>(by) * blocksX + bx] = FallOff(lo, hi, static_cast<float>(sum) / n);
			}
		return true;
	}

	// The engine's own forward/backward consistency per cell: |f + b| in the engine's
	// input pixels, where f is the cell's forward vector and b the backward vector of the
	// cell its centre lands in (clamped to the field, as the matcher's consistency test
	// does). S10.5 vectors, x then y. False when the fields do not fit.
	inline bool FbError(const std::vector<int16_t>& forward, const std::vector<int16_t>& backward, int gridW, int gridH,
		int grid, std::vector<float>& out)
	{
		out.clear();
		const size_t cells = static_cast<size_t>(gridW > 0 ? gridW : 0) * (gridH > 0 ? gridH : 0);
		if (cells == 0 || grid <= 0 || forward.size() < cells * 2 || backward.size() < cells * 2) return false;
		out.resize(cells);
		const float g = static_cast<float>(grid);
		for (int cy = 0; cy < gridH; ++cy)
			for (int cx = 0; cx < gridW; ++cx)
			{
				const size_t i = static_cast<size_t>(cy) * gridW + cx;
				const float fx = forward[i * 2] / 32.0f, fy = forward[i * 2 + 1] / 32.0f;
				const float lx = cx * g + g * 0.5f + fx, ly = cy * g + g * 0.5f + fy;
				int tx = static_cast<int>(std::floor(lx / g)), ty = static_cast<int>(std::floor(ly / g));
				tx = tx < 0 ? 0 : tx >= gridW ? gridW - 1 : tx;
				ty = ty < 0 ? 0 : ty >= gridH ? gridH - 1 : ty;
				const size_t j = static_cast<size_t>(ty) * gridW + tx;
				const float ex = fx + backward[j * 2] / 32.0f, ey = fy + backward[j * 2 + 1] / 32.0f;
				out[i] = std::sqrt(ex * ex + ey * ey);
			}
		return true;
	}

	// The per-block consistency weight: the mean over the block's cells of 1 within
	// `tolerance` (the matcher's consistency limit, the same pixels), falling to 0 at twice
	// it. A block straddling an occlusion edge (some cells inconsistent) is partly
	// trusted; the matcher's block test alone sees one vector per block.
	inline bool FbWeights(const std::vector<float>& cellError, int gridW, int gridH, int grid, int blockPx,
		int blocksX, int blocksY, float tolerance, std::vector<float>& out)
	{
		out.clear();
		const size_t cells = static_cast<size_t>(gridW > 0 ? gridW : 0) * (gridH > 0 ? gridH : 0);
		if (cells == 0 || cellError.size() < cells || blocksX <= 0 || blocksY <= 0 || !(tolerance > 0.0f)) return false;
		const int per = CellsPerBlock(grid, blockPx);
		out.assign(static_cast<size_t>(blocksX) * blocksY, 1.0f);
		for (int by = 0; by < blocksY; ++by)
			for (int bx = 0; bx < blocksX; ++bx)
			{
				float sum = 0.0f;
				unsigned n = 0;
				for (int cy = by * per; cy < (by + 1) * per && cy < gridH; ++cy)
					for (int cx = bx * per; cx < (bx + 1) * per && cx < gridW; ++cx)
					{
						sum += FallOff(tolerance, 2.0f * tolerance, cellError[static_cast<size_t>(cy) * gridW + cx]);
						++n;
					}
				if (n) out[static_cast<size_t>(by) * blocksX + bx] = sum / n;
			}
		return true;
	}

	// The weights the de-judder applies: the product of the enabled ones (each 1 where it is
	// missing). False (out empty) when none applies: then the de-judder runs exactly as
	// without them.
	inline bool Combine(const std::vector<float>* cost, const std::vector<float>* fb, size_t blocks, std::vector<float>& out)
	{
		out.clear();
		const bool useCost = cost && cost->size() == blocks && blocks > 0;
		const bool useFb = fb && fb->size() == blocks && blocks > 0;
		if (!useCost && !useFb) return false;
		out.assign(blocks, 1.0f);
		for (size_t i = 0; i < blocks; ++i)
			out[i] = (useCost ? (*cost)[i] : 1.0f) * (useFb ? (*fb)[i] : 1.0f);
		return true;
	}

	// The engine's whole-frame motion (D1 format: x, y, ambiguity, valid) is usable when
	// it is valid and clear (the ambiguity below D1's 0.9: about a third of the confident
	// cells agree). Otherwise the global search runs as without the engine.
	inline bool GlobalUsable(const float g[4])
	{
		return g[3] > 0.5f && g[2] < 0.9f;
	}
}
