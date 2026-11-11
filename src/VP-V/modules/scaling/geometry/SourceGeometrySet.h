// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// scaling.geometry: the PUBLISHED list of supported (conventional, repairable) source geometries. Owner decision 2026-11-07 (SID E0 rule R2/R3): a
// source-identity candidate is accepted only if its ratio to the measured raster is one of these; anything else is UNSURE (visible), never a
// re-base. The list lives here, in the scaling area, because the scaling module owns what it can repair; identity (input) only reads it. Later the
// scaling module may extend or replace the table; callers use only the functions below.
//
// Representation: ratios (n / N) of the measured active-area axis against a 3840x2160 reference raster, so a letterboxed or pillarboxed picture
// (n and N both measured in the active area) keeps its ratio. Squares: 3840x2160 (1), 2560x1440, 1920x1080, 1280x720, 960x540, DCI 2K 2048 and
// DCI flat 1998 widths (of 3840). DCI 4096 is wider than the 3840 raster (ratio above 1: not an upscaled source, so not a candidate here).
// Non-square pairs: 720x576 and 720x480 (SD, pixel aspect not square) and 1440x1080 (anamorphic HD; single-axis re-base of it is PARKED:
// it needs the width axis alone, see the two-axis-only rule). Common scope / flat active heights (letterbox) are covered because the ratio
// uses the active area. Tolerance: 0.6 % of the ratio (about 6 px at 1920 of 3840).
//
// Pure, header-only, no state, C++14, no Windows macros.
#include <cmath>
#include <cstddef>

namespace Scaling
{
	namespace GeometrySet
	{
		constexpr double kConventionTolerance = 0.006;   // relative, on the ratio n / N

		struct Pair
		{
			const char* name;
			double rx;
			double ry;
			bool singleAxisParked;   // the pair needs a single-axis re-base (anamorphic 1440x1080): listed, not applied alone
		};

		// Ratios of the named geometry's size to a 3840x2160 raster.
		inline const Pair* Pairs(std::size_t& count)
		{
			static const Pair kPairs[] = {
				{ "2560x1440", 2560.0 / 3840.0, 2560.0 / 3840.0, false },
				{ "1920x1080", 0.5, 0.5, false },
				{ "1280x720", 1280.0 / 3840.0, 1280.0 / 3840.0, false },
				{ "960x540", 0.25, 0.25, false },
				{ "DCI 2K 2048", 2048.0 / 3840.0, 2048.0 / 3840.0, false },
				{ "DCI flat 1998", 1998.0 / 3840.0, 1998.0 / 3840.0, false },
				{ "1440x1080 anamorphic", 1440.0 / 3840.0, 0.5, true },
				{ "720x576", 720.0 / 3840.0, 576.0 / 2160.0, false },
				{ "720x480", 720.0 / 3840.0, 480.0 / 2160.0, false },
			};
			count = sizeof(kPairs) / sizeof(kPairs[0]);
			return kPairs;
		}

		inline bool Near(double r, double ref, double tol = kConventionTolerance)
		{
			return std::fabs(r - ref) <= tol * ref + 1e-12;
		}

		// One axis alone (axis 0 = horizontal, 1 = vertical): is n / N a ratio of some listed geometry on that axis?
		inline bool ConventionalAxis(int n, int N, int axis)
		{
			if (n <= 0 || N <= 0)
				return false;
			const double r = static_cast<double>(n) / static_cast<double>(N);
			std::size_t count = 0;
			const Pair* p = Pairs(count);
			for (std::size_t i = 0; i < count; ++i)
				if (Near(r, axis == 0 ? p[i].rx : p[i].ry))
					return true;
			return false;
		}

		// The two axes together: the pair of ratios must be ONE listed geometry (returns its name, nullptr when none). `singleAxisParked` of the
		// match is reported through `parked` (optional).
		inline const char* ConventionalPair(int nx, int Nx, int ny, int Ny, bool* parked = nullptr)
		{
			if (parked != nullptr)
				*parked = false;
			if (nx <= 0 || Nx <= 0 || ny <= 0 || Ny <= 0)
				return nullptr;
			const double rx = static_cast<double>(nx) / static_cast<double>(Nx);
			const double ry = static_cast<double>(ny) / static_cast<double>(Ny);
			std::size_t count = 0;
			const Pair* p = Pairs(count);
			for (std::size_t i = 0; i < count; ++i)
				if (Near(rx, p[i].rx) && Near(ry, p[i].ry))
				{
					if (parked != nullptr)
						*parked = p[i].singleAxisParked;
					return p[i].name;
				}
			return nullptr;
		}
	}
}
