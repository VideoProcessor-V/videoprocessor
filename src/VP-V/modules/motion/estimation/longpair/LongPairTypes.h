// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// I-to-I long-pair service (doc 72 section 1 "Service: I-to-I long-range flow"): value types, the S10.5 fixed-point
// helpers, the hint resampling to the engine's hint grid, and the visible-fallback wording. Header-only, pure C++14
// (no Windows headers): shared by the service, the CPU seeded search and the tests.
//
// Units: everything is in HALF-LUMA px (the NVOF input is 8-bit half luma, 4x4 output grid); a long pair is any two
// frames A, B of the same size (the distance is the measured GOP length, never a fixed lag).
#pragma once

#include <VP-V/core/FallbackCatalog.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace VpLongPair
{
	constexpr int kGrid = 4;   // NVOF output grid (half-luma px per cell)

	// The seed: TD's composed trail field (U14) on its own block grid. Vectors A -> B in half-luma px.
	struct HintField
	{
		int gridW = 0, gridH = 0;
		int cellPx = 8;                // half-luma px per hint cell
		std::vector<float> vx, vy;     // gridW * gridH each; non-finite = no hint for that cell (treated as 0)
		bool Valid() const
		{
			return gridW > 0 && gridH > 0 && cellPx > 0 && vx.size() == static_cast<size_t>(gridW) * gridH && vy.size() == vx.size();
		}
	};

	// The result on the NVOF 4x4 grid of half luma: S10.5 vectors (x, y interleaved) A -> B and B -> A, costs u8
	// (higher = less sure, the engine's convention; the CPU search scales its SAD to the same range).
	struct Field
	{
		int gridW = 0, gridH = 0;
		std::vector<int16_t> forward, backward;
		std::vector<uint8_t> forwardCost, backwardCost;
	};

	enum class Path
	{
		None,
		NvofHinted,     // second NVOF session with external hints
		NvofUnhinted,   // the same session, no hint supplied for this pair (hints are optional per pair)
		SeededSearch    // matcher seeded search (CPU reference or the bound matcher backend): the visible fallback
	};

	inline const char* PathName(Path p)
	{
		switch (p)
		{
		case Path::NvofHinted: return "nvof_hinted";
		case Path::NvofUnhinted: return "nvof_unhinted";
		case Path::SeededSearch: return "seeded_search";
		default: return "none";
		}
	}

	inline int16_t ToS105(float v)
	{
		if (!std::isfinite(v))
			return 0;
		const float s = std::floor(v * 32.0f + 0.5f);
		return static_cast<int16_t>(std::min(std::max(s, -32767.0f), 32767.0f));
	}
	inline float FromS105(int16_t v) { return static_cast<float>(v) / 32.0f; }

	inline int GridOf(int planeSize) { return (planeSize + kGrid - 1) / kGrid; }

	// The hint resampled onto the engine's hint grid (same grid as the output: 4 half-luma px per cell): bilinear between
	// hint-cell centres, edge clamp, S10.5. Cell centres: hint cell c is centred at (c + 0.5) * cellPx.
	inline bool ResampleHint(const HintField& hint, int halfW, int halfH, std::vector<int16_t>& out)
	{
		if (!hint.Valid() || halfW <= 0 || halfH <= 0)
			return false;
		const int gw = GridOf(halfW), gh = GridOf(halfH);
		out.assign(static_cast<size_t>(gw) * gh * 2, 0);
		auto at = [&](const std::vector<float>& v, int x, int y)
		{
			const float f = v[static_cast<size_t>(std::min(std::max(y, 0), hint.gridH - 1)) * hint.gridW + std::min(std::max(x, 0), hint.gridW - 1)];
			return std::isfinite(f) ? f : 0.0f;
		};
		for (int cy = 0; cy < gh; ++cy)
			for (int cx = 0; cx < gw; ++cx)
			{
				const float u = (static_cast<float>(cx * kGrid) + kGrid * 0.5f) / hint.cellPx - 0.5f;
				const float v = (static_cast<float>(cy * kGrid) + kGrid * 0.5f) / hint.cellPx - 0.5f;
				const float fu = std::floor(u), fv = std::floor(v), tx = u - fu, ty = v - fv;
				const int x0 = static_cast<int>(fu), y0 = static_cast<int>(fv);
				float r[2];
				for (int c = 0; c < 2; ++c)
				{
					const std::vector<float>& p = c == 0 ? hint.vx : hint.vy;
					const float top = at(p, x0, y0) * (1.0f - tx) + at(p, x0 + 1, y0) * tx;
					const float bot = at(p, x0, y0 + 1) * (1.0f - tx) + at(p, x0 + 1, y0 + 1) * tx;
					r[c] = top * (1.0f - ty) + bot * ty;
				}
				const size_t o = (static_cast<size_t>(cy) * gw + cx) * 2;
				out[o] = ToS105(r[0]);
				out[o + 1] = ToS105(r[1]);
			}
		return true;
	}

	// ---- visible fallback (ADR-0001: no silent fallbacks) -------------------------------------------------------------
	// Engaged whenever the pair was NOT done by the hinted NVOF session although a hint was supplied.
	struct Why
	{
		bool hintSupplied = false;
		std::string reason;   // why the hinted session did not run (driver / caps / execute error)
	};

	inline FallbackCatalog::Decision Decision(Path path, const Why& why)
	{
		if (path != Path::SeededSearch || !why.hintSupplied)
			return FallbackCatalog::Released("motion long pair");
		return FallbackCatalog::Engaged("motion long pair", "motion: long-pair seeded search (NVOF with hints unavailable: " +
			(why.reason.empty() ? std::string("not opened") : why.reason) + ")");
	}

	inline std::string StatusLine(Path path, const Why& why)
	{
		switch (path)
		{
		case Path::NvofHinted: return "motion: long pair NVOF + hints";
		case Path::NvofUnhinted: return "motion: long pair NVOF (no hint for this pair)";
		case Path::SeededSearch:
			return why.hintSupplied ? "motion: long-pair seeded search (NVOF with hints unavailable: " +
				(why.reason.empty() ? std::string("not opened") : why.reason) + ")" : "motion: long-pair seeded search";
		default: return "motion: long pair idle";
		}
	}
}
