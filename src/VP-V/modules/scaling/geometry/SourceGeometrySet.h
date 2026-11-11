// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// scaling.geometry: the PUBLISHED list of supported (conventional, repairable) source geometries. Owner decision 2026-11-07 (SID E0 rule R2/R3): a
// source-identity candidate is accepted only if its ratio to the measured axis is one of these; anything else is UNSURE (visible), never a
// re-base. The list lives here, in the scaling area, because the scaling module owns what it can repair; identity (input) only reads it. Later the
// scaling module may extend or replace the table; callers use only the functions below.
//
// Representation (r2 fix L3, general-not-fixed rule): ABSOLUTE source sizes with their display aspect. The expected ratios are derived from the
// DELIVERED raster (Rx x Ry) every call: the source picture is fitted into the raster at its display aspect (dispW = min(Rx, Ry * dar),
// dispH = dispW / dar) and the expected per-axis ratios are w / dispW and h / dispH. Identity compares them with n / N measured in the active
// area, so a letterboxed or pillarboxed picture (n and N both measured in the active area) keeps its ratio. On a 3840x2160 raster this gives
// exactly the E0 table (1920x1080 -> 0.5; DCI 2048 / 1998 -> equal ratios; 720x576 -> 720/3840 x 576/2160; 1440x1080 -> 0.375 x 0.5). On a
// 1920x1080 raster 1280x720 is 2/3 and 960x540 is 1/2; sizes at or above the raster on any axis (2560x1440 there) are not upscaled sources and
// are skipped. A raster of 0 (unknown) makes NOTHING conventional (fail safe: unsure, never a re-base).
// Entries: 2560x1440, 1920x1080, 1280x720, 960x540, DCI 2K 2048x1080, DCI flat 1998x1080 (square pixels), 720x576 and 720x480 (SD shown at
// 16:9, pixel aspect not square) and 1440x1080 anamorphic HD shown at 16:9 (single-axis re-base of it is PARKED: listed, never applied alone, never
// admitted by the per-axis rule, and the pair prior treats it as not agreeing). Known gap: 4:3 SD (pillarboxed 720x576 / 720x480 at 4:3) is not
// listed. Note (L2): horizontally 3/16 (720 of 3840) stays admitted per axis because the SD entries are conventional; 3/8 (1440 of 3840) is
// admitted per axis ONLY through a conventional (non-parked) entry, i.e. not at all today. Tolerance: 0.6 % of the ratio.
//
// Pure, header-only, no state, C++14, no Windows macros.
#include <cmath>
#include <cstddef>

namespace Scaling
{
	namespace GeometrySet
	{
		constexpr double kConventionTolerance = 0.006;   // relative, on the ratio n / N

		struct Geometry
		{
			const char* name;
			int w;
			int h;
			double dar;               // display aspect the picture is shown at (w / h for square pixels)
			bool singleAxisParked;    // the pair needs a single-axis re-base (anamorphic 1440x1080): listed, not applied alone
		};

		inline const Geometry* Geometries(std::size_t& count)
		{
			static const Geometry kGeometries[] = {
				{ "2560x1440", 2560, 1440, 2560.0 / 1440.0, false },
				{ "1920x1080", 1920, 1080, 1920.0 / 1080.0, false },
				{ "1280x720", 1280, 720, 1280.0 / 720.0, false },
				{ "960x540", 960, 540, 960.0 / 540.0, false },
				{ "DCI 2K 2048x1080", 2048, 1080, 2048.0 / 1080.0, false },
				{ "DCI flat 1998x1080", 1998, 1080, 1998.0 / 1080.0, false },
				{ "1440x1080 anamorphic", 1440, 1080, 16.0 / 9.0, true },
				{ "720x576", 720, 576, 16.0 / 9.0, false },
				{ "720x480", 720, 480, 16.0 / 9.0, false },
			};
			count = sizeof(kGeometries) / sizeof(kGeometries[0]);
			return kGeometries;
		}

		inline bool Near(double r, double ref, double tol = kConventionTolerance)
		{
			return std::fabs(r - ref) <= tol * ref + 1e-12;
		}

		// Expected per-axis ratios of `g` on a delivered Rx x Ry raster. False when the raster is unknown or the size is not an upscaled source
		// on that raster (a ratio above 1 on either axis, or no axis below 1).
		inline bool RatiosFor(const Geometry& g, int rasterW, int rasterH, double& rx, double& ry)
		{
			rx = 0.0;
			ry = 0.0;
			if (rasterW <= 0 || rasterH <= 0 || g.w <= 0 || g.h <= 0 || g.dar <= 0.0)
				return false;
			double dispW = static_cast<double>(rasterH) * g.dar;
			if (dispW > static_cast<double>(rasterW))
				dispW = static_cast<double>(rasterW);
			const double dispH = dispW / g.dar;
			rx = static_cast<double>(g.w) / dispW;
			ry = static_cast<double>(g.h) / dispH;
			const double hi = 1.0 + kConventionTolerance;
			const double lo = 1.0 - kConventionTolerance;
			if (rx > hi || ry > hi)
				return false;
			if (rx >= lo && ry >= lo)
				return false;
			return true;
		}

		// One axis alone (axis 0 = horizontal, 1 = vertical): is n / N the ratio of some listed geometry on that axis, on this raster? Parked
		// entries are not admitted unless `allowParked` (L2: single-axis re-base is parked, so its codec-grid ratios are not evidence).
		inline bool ConventionalAxis(int n, int N, int axis, int rasterW, int rasterH, bool allowParked = false)
		{
			if (n <= 0 || N <= 0)
				return false;
			const double r = static_cast<double>(n) / static_cast<double>(N);
			std::size_t count = 0;
			const Geometry* g = Geometries(count);
			for (std::size_t i = 0; i < count; ++i)
			{
				if (g[i].singleAxisParked && !allowParked)
					continue;
				double rx = 0.0, ry = 0.0;
				if (!RatiosFor(g[i], rasterW, rasterH, rx, ry))
					continue;
				if (Near(r, axis == 0 ? rx : ry))
					return true;
			}
			return false;
		}

		// The two axes together: the pair of ratios must be ONE listed geometry on this raster (returns its name, nullptr when none).
		// `singleAxisParked` of the match is reported through `parked` (optional). Non-parked entries are tried first.
		inline const char* ConventionalPair(int nx, int Nx, int ny, int Ny, int rasterW, int rasterH, bool* parked = nullptr)
		{
			if (parked != nullptr)
				*parked = false;
			if (nx <= 0 || Nx <= 0 || ny <= 0 || Ny <= 0)
				return nullptr;
			const double qx = static_cast<double>(nx) / static_cast<double>(Nx);
			const double qy = static_cast<double>(ny) / static_cast<double>(Ny);
			std::size_t count = 0;
			const Geometry* g = Geometries(count);
			for (int pass = 0; pass < 2; ++pass)
				for (std::size_t i = 0; i < count; ++i)
				{
					if (g[i].singleAxisParked != (pass == 1))
						continue;
					double rx = 0.0, ry = 0.0;
					if (!RatiosFor(g[i], rasterW, rasterH, rx, ry))
						continue;
					if (Near(qx, rx) && Near(qy, ry))
					{
						if (parked != nullptr)
							*parked = g[i].singleAxisParked;
						return g[i].name;
					}
				}
			return nullptr;
		}

		// A pair that agrees for the R3 prior and the latch: one listed, NON-parked geometry (L1).
		inline bool AgreeingPair(int nx, int Nx, int ny, int Ny, int rasterW, int rasterH)
		{
			bool parked = false;
			return ConventionalPair(nx, Nx, ny, Ny, rasterW, rasterH, &parked) != nullptr && !parked;
		}
	}
}
