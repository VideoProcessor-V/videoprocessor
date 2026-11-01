// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// Test helper (header-only): synthetic luma frames in 10-bit studio codes for the region evidence tests (CPU and WARP).
// Deterministic: a fixed xorshift generator and Box-Muller, no std distributions.
#pragma once

#include <VP-V/modules/input/source_analysis/regions/RegionEvidence.h>

#include <cmath>
#include <cstdint>

namespace VideoProcessorTest
{
	namespace RegionSynth
	{
		using GrainSplit::Plane;

		struct Rng
		{
			uint64_t s;
			explicit Rng(uint64_t seed) : s(seed * 2654435761ull + 0x9E3779B97F4A7C15ull) {}
			double Uniform()
			{
				s ^= s << 13; s ^= s >> 7; s ^= s << 17;
				return (static_cast<double>(s >> 11) + 0.5) / 9007199254740992.0;
			}
			double Gauss()
			{
				const double u = Uniform(), v = Uniform();
				return std::sqrt(-2.0 * std::log(u)) * std::cos(6.283185307179586 * v);
			}
		};

		inline Plane Flat(int w, int h, double code) { return Plane(w, h, code); }

		inline void AddGrain(Plane& p, double sigma, uint64_t seed)
		{
			Rng r(seed);
			for (double& v : p.v) v += sigma * r.Gauss();
		}

		// Fills rows/columns outside `picture` with a bar code (black 64 by default).
		inline void Bars(Plane& p, const RegionEvidence::Rect& picture, double code = 64.0)
		{
			for (int y = 0; y < p.h; ++y)
				for (int x = 0; x < p.w; ++x)
					if (x < picture.left || x >= picture.right || y < picture.top || y >= picture.bottom) p.at(x, y) = code;
		}

		// A vertical step at column `edgeX` (lo left, hi right) and optional ringing (damped oscillation) on both sides.
		inline Plane Step(int w, int h, int edgeX, double lo, double hi, double ringAmp)
		{
			Plane p(w, h);
			for (int y = 0; y < h; ++y)
				for (int x = 0; x < w; ++x)
				{
					const double d = x < edgeX ? static_cast<double>(edgeX - 1 - x) : static_cast<double>(x - edgeX);
					const double base = x < edgeX ? lo : hi;
					const double sign = x < edgeX ? -1.0 : 1.0;
					const double ring = ringAmp * std::cos(3.141592653589793 * d) * std::exp(-d / 5.0);   // alternating, decaying
					p.at(x, y) = base + sign * ring;
				}
			return p;
		}

		inline Plane Ramp(int w, int h, double start, double slopePerPx)
		{
			Plane p(w, h);
			for (int y = 0; y < h; ++y)
				for (int x = 0; x < w; ++x) p.at(x, y) = start + slopePerPx * x;
			return p;
		}

		inline void AddTexture(Plane& p, int x0, int y0, int x1, int y1, double amp, double period)
		{
			for (int y = y0; y < y1; ++y)
				for (int x = x0; x < x1; ++x)
					p.at(x, y) += amp * std::sin(6.283185307179586 * x / period) * std::cos(6.283185307179586 * y / (period * 1.3));
		}
	}
}
