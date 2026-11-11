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
// 16:9, pixel aspect not square) and the anamorphic HD sizes HDV 1440x1080, DVCPRO HD 1280x1080 and 960x720 shown at 16:9 (PARKED, apply off:
// listed and recognised by name, never applied alone, never admitted by the per-axis rule, and the pair prior treats them as not agreeing). Known gap: 4:3 SD (pillarboxed 720x576 / 720x480 at 4:3) is not
// listed. Note (L2): horizontally 3/16 (720 of 3840) stays admitted per axis because the SD entries are conventional; 3/8 (1440 of 3840) is
// admitted per axis ONLY through a conventional (non-parked) entry, i.e. not at all today. Tolerance: 0.6 % of the ratio.
//
// Pure, header-only, no state, C++14, no Windows macros.
#include <cmath>
#include <cstddef>
#include <string>

namespace Scaling
{
	namespace GeometrySet
	{
		constexpr double kConventionTolerance = 0.006;   // relative, on the ratio n / N

		// ONE table, the single place to edit (owner 2026-11-11 ~10:25): each entry has two independent flags.
		//   detect - a candidate the detectors test and RECOGNISE (name in a hold note, log line and evidence counter). The default
		//            detection set (source_geometry_detect = all) is every entry with detect on: WIDE, all standard sizes.
		//   apply  - a re-base to it may be applied (still subject to the two-axis rule). apply off = PARKED: listed, recognised and
		//            named, never applied, never a latch (AgreeingPair) and never single-axis evidence (ConventionalAxis).
		// A detect-only entry (apply off) can only produce a named HOLD; narrowing detection never changes what is applied.
		//
		// Named anamorphic HD entries (owner 2026-11-11 ~10:20; parked, log only), coded sizes and pixel aspect from the standards:
		//   HDV 1080i/1080p (HDV specification, Canon/JVC/Sharp/Sony 2003; IEC 61834-11) and Sony XDCAM HD / MPEG HD 4:2:0
		//     (SMPTE RDD 9): 1440x1080 coded, pixel aspect 4:3, shown at 16:9 (H 0.75 on a 1920-wide picture). DVCPRO HD 1080/50
		//     (SMPTE 370M) uses the same 1440x1080 / 4:3 raster: one entry.
		//   DVCPRO HD 1080/59.94 and 1080/60 (SMPTE 370M): 1280x1080 coded, pixel aspect 3:2, shown at 16:9 (H 2/3 on 1920).
		//   DVCPRO HD 720p (SMPTE 370M): 960x720 coded, pixel aspect 4:3, shown at 16:9 (H 0.75 on a 1280-wide picture).
		// Each is placed on the DELIVERED raster like every entry (1440x1080 on 2160p: 0.375 x 0.5; 960x720 on 1080p: 0.5 x 0.667).
		struct Geometry
		{
			const char* name;
			int w;
			int h;
			double dar;               // display aspect the picture is shown at (w / h for square pixels)
			bool detect;              // in the default (wide) detection set: recognised and named
			bool apply;               // a re-base to it may be applied; off = parked (anamorphic / single-axis): named holds only
			const char* id;           // the token of source_geometry_detect (lower case, no spaces)
		};

		inline const Geometry* Geometries(std::size_t& count)
		{
			static const Geometry kGeometries[] = {
				{ "2560x1440", 2560, 1440, 2560.0 / 1440.0, true, true, "2560x1440" },
				{ "1920x1080", 1920, 1080, 1920.0 / 1080.0, true, true, "1920x1080" },
				{ "1280x720", 1280, 720, 1280.0 / 720.0, true, true, "1280x720" },
				{ "960x540", 960, 540, 960.0 / 540.0, true, true, "960x540" },
				{ "DCI 2K 2048x1080", 2048, 1080, 2048.0 / 1080.0, true, true, "dci_2048x1080" },
				{ "DCI flat 1998x1080", 1998, 1080, 1998.0 / 1080.0, true, true, "dci_1998x1080" },
				{ "anamorphic HDV 1440x1080", 1440, 1080, 16.0 / 9.0, true, false, "hdv_1440x1080" },
				{ "720x576", 720, 576, 16.0 / 9.0, true, true, "sd_720x576" },
				{ "720x480", 720, 480, 16.0 / 9.0, true, true, "sd_720x480" },
				{ "anamorphic DVCPRO HD 1280x1080", 1280, 1080, 16.0 / 9.0, true, false, "dvcprohd_1280x1080" },
				{ "anamorphic DVCPRO HD 960x720", 960, 720, 16.0 / 9.0, true, false, "dvcprohd_960x720" },
			};
			count = sizeof(kGeometries) / sizeof(kGeometries[0]);
			return kGeometries;
		}

		// The detection selection: one bit per table index (the table stays under 32 entries).
		typedef unsigned DetectMask;
		inline DetectMask DefaultDetectMask()
		{
			std::size_t count = 0;
			const Geometry* g = Geometries(count);
			DetectMask m = 0u;
			for (std::size_t i = 0; i < count; ++i)
				if (g[i].detect)
					m |= 1u << i;
			return m;
		}

		// source_geometry_detect: "all" (default, every detect entry), "conventional" (the apply entries only), or a list of entry ids
		// separated by commas, spaces or semicolons. Unknown tokens are returned in `unknown` (comma separated; the caller shows them);
		// false when any token was unknown (the known ones still apply). An empty value is "all".
		inline bool ParseDetect(const std::string& value, DetectMask& mask, std::string& unknown)
		{
			unknown.clear();
			std::size_t count = 0;
			const Geometry* g = Geometries(count);
			std::string v;
			for (char c : value)
				v += (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
			const std::size_t a = v.find_first_not_of(" \t");
			const std::string trimmed = a == std::string::npos ? std::string() : v.substr(a, v.find_last_not_of(" \t") - a + 1);
			if (trimmed.empty() || trimmed == "all")
			{
				mask = DefaultDetectMask();
				return true;
			}
			mask = 0u;
			if (trimmed == "conventional")
			{
				for (std::size_t i = 0; i < count; ++i)
					if (g[i].detect && g[i].apply)
						mask |= 1u << i;
				return true;
			}
			std::size_t pos = 0;
			while (pos < trimmed.size())
			{
				const std::size_t end = trimmed.find_first_of(", ;\t", pos);
				const std::string token = trimmed.substr(pos, (end == std::string::npos ? trimmed.size() : end) - pos);
				pos = end == std::string::npos ? trimmed.size() : end + 1;
				if (token.empty())
					continue;
				bool found = false;
				for (std::size_t i = 0; i < count && !found; ++i)
					if (token == g[i].id)
					{
						mask |= 1u << i;
						found = true;
					}
				if (!found)
					unknown += (unknown.empty() ? "" : ",") + token;
			}
			return unknown.empty();
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
				if (!g[i].apply && !allowParked)
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
		// A parked (apply off) match is reported through `parked` (optional). Non-parked entries are tried first.
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
					if (g[i].apply == (pass == 1))
						continue;
					double rx = 0.0, ry = 0.0;
					if (!RatiosFor(g[i], rasterW, rasterH, rx, ry))
						continue;
					if (Near(qx, rx) && Near(qy, ry))
					{
						if (parked != nullptr)
							*parked = !g[i].apply;
						return g[i].name;
					}
				}
			return nullptr;
		}

		// Recognition of a HELD reading (owner 2026-11-11): the per-axis ratios rH x rV (1.0 = native on that axis) of a parked
		// (detect-only) entry within `mask`, on the delivered raster; nullptr when none (a generic hold). Never a reason to apply.
		inline const Geometry* RecogniseParked(double rH, double rV, int rasterW, int rasterH, DetectMask mask)
		{
			if (rasterW <= 0 || rasterH <= 0 || !(rH > 0.0) || !(rV > 0.0))
				return nullptr;
			std::size_t count = 0;
			const Geometry* g = Geometries(count);
			for (std::size_t i = 0; i < count; ++i)
			{
				if (g[i].apply || (mask & (1u << i)) == 0u)
					continue;
				double rx = 0.0, ry = 0.0;
				if (!RatiosFor(g[i], rasterW, rasterH, rx, ry))
					continue;
				if (Near(rH, rx) && Near(rV, ry))
					return &g[i];
			}
			return nullptr;
		}

		// The table index of an entry (for per-entry counters); -1 when not in the table.
		inline int IndexOf(const Geometry* e)
		{
			std::size_t count = 0;
			const Geometry* g = Geometries(count);
			return e != nullptr && e >= g && e < g + count ? static_cast<int>(e - g) : -1;
		}

		// A pair that agrees for the R3 prior and the latch: one listed, NON-parked geometry (L1).
		inline bool AgreeingPair(int nx, int Nx, int ny, int Ny, int rasterW, int rasterH)
		{
			bool parked = false;
			return ConventionalPair(nx, Nx, ny, Ny, rasterW, rasterH, &parked) != nullptr && !parked;
		}
	}
}
