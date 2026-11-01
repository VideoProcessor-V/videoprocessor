// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// Shared unit U2 "band split (extra sigma)" (doc 62 section 1.2): the one separable-Gaussian law every band split
// uses. A consumer that needs a band of the picture at a sigma it chooses takes the radius, the normalised weights
// and the half-resolution sigma from here instead of keeping a private copy; the CPU reference blur and the GLSL
// emitter (`vpu_band_from_base_*`) are built from the same Radius()/Weight(), so they cannot drift apart.
//
// Law (unchanged from the private copies it replaces, bit for bit):
//   radius  r = ceil(factor * sigma - eps), at least 1 when minOne
//   weight  w(k) = exp(-0.5 k k / (sigma sigma)) / sum_{i=-r..r} exp(-0.5 i i / (sigma sigma))   (sum in index order -r..r)
//   half-resolution sigma of a full-resolution sigma (the 2x2 box adds 0.25 px^2 per axis):
//           sqrt(sigma^2 - 0.25) / 2
// Determinism: fixed summation order (taps -r..r, rows then columns), no threads, no atomics.
// Sizes are fractions of the active-picture height (Params::sigmaRel); callers that already hold a pixel sigma
// (the legacy consumers: sigma x scale) pass it straight to Radius()/Weight().
//
// Header-only, C++14, no Windows or GPU headers.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace VpUnit
{
namespace BandFromBase
{
	// Bump on ANY change of the law, the radius rule or the emitted GLSL entry names.
	constexpr uint32_t kUnitVersion = 1;

	struct Params
	{
		double sigmaRel = 0.0;       // sigma as a fraction of the active-picture height
		double activeHeight = 0.0;   // active-picture height in px of the plane being filtered
		double factor = 3.0;         // radius = ceil(factor * sigma - eps)
		bool minOne = true;          // at least one tap each side
		double eps = 0.0;
		double SigmaPx() const { return sigmaRel * activeHeight; }
	};

	inline int Radius(double sigma, double factor, bool minOne, double eps)
	{
		const int r = static_cast<int>(std::ceil(factor * sigma - eps));
		return minOne ? (std::max)(1, r) : r;
	}
	inline int Radius(const Params& p) { return Radius(p.SigmaPx(), p.factor, p.minOne, p.eps); }

	// Normalised weight of tap k for radius r. Same expression and summation order as every private copy.
	inline double Weight(int k, double sigma, int r)
	{
		const double s = sigma;
		double sum = 0.0;
		for (int i = -r; i <= r; ++i) sum += std::exp(-0.5 * i * i / (s * s));
		return std::exp(-0.5 * k * k / (s * s)) / sum;
	}

	// The half-resolution sigma of a full-resolution split.
	inline double HalfSigma(double sigma) { return std::sqrt(sigma * sigma - 0.25) / 2.0; }

	// CPU reference: separable Gaussian of a w x h plane, edges replicated, fixed summation order (rows across,
	// then columns down). sigma <= 0 returns the input.
	inline std::vector<double> Blur(const std::vector<double>& in, int w, int h, double sigma)
	{
		if (!(sigma > 0.0)) return in;
		const int r = Radius(sigma, 3.0, true, 0.0);
		std::vector<double> k(static_cast<size_t>(2 * r + 1));
		double sum = 0.0;
		for (int i = -r; i <= r; ++i) sum += k[static_cast<size_t>(i + r)] = std::exp(-0.5 * i * i / (sigma * sigma));
		for (double& x : k) x /= sum;
		auto get = [&](int x, int y) {
			return in[static_cast<size_t>((std::max)(0, (std::min)(h - 1, y))) * w + static_cast<size_t>((std::max)(0, (std::min)(w - 1, x)))];
		};
		std::vector<double> t(in.size()), o(in.size(), 0.0);
		std::vector<double> pad(static_cast<size_t>(w + 2 * r));
		for (int y = 0; y < h; ++y)
		{
			for (int x = -r; x < w + r; ++x) pad[static_cast<size_t>(x + r)] = get(x, y);
			for (int x = 0; x < w; ++x)
			{
				double a = 0.0;
				for (int i = -r; i <= r; ++i) a += k[static_cast<size_t>(i + r)] * pad[static_cast<size_t>(x + r + i)];
				t[static_cast<size_t>(y) * w + x] = a;
			}
		}
		for (int y = 0; y < h; ++y)
		{
			double* dst = &o[static_cast<size_t>(y) * w];
			for (int i = -r; i <= r; ++i)
			{
				const double* src = &t[static_cast<size_t>((std::max)(0, (std::min)(h - 1, y + i))) * w];
				const double kk = k[static_cast<size_t>(i + r)];
				for (int x = 0; x < w; ++x) dst[x] += kk * src[x];
			}
		}
		return o;
	}

	// GLSL number text (shortest round-trip-safe form used by the VP shader emitters: %.9g, ".0" when integral).
	inline std::string GlslNum(double v)
	{
		char s[48];
		std::snprintf(s, sizeof(s), "%.9g", v);
		std::string r = s;
		if (r.find_first_of(".eEn") == std::string::npos) r += ".0";
		return r;
	}

	// GLSL for one band: weights as literals from Weight(), plus the two edge-replicating taps loops.
	// Entry names: vpu_band_from_base_r (radius), vpu_band_from_base_w(k), vpu_band_from_base_across / _down.
	// Not wired into a pass yet (consumers keep their own pass structure); the text is the contract.
	inline std::string Glsl(const Params& p)
	{
		const double sigma = p.SigmaPx();
		const int r = Radius(p);
		std::string s = "#define VPU_BAND_FROM_BASE_VERSION " + std::to_string(kUnitVersion) + "\n";
		s += "#define VPU_BAND_FROM_BASE_R " + std::to_string(r) + "\n";
		s += "const float vpu_band_from_base_tab[" + std::to_string(2 * r + 1) + "] = float[" + std::to_string(2 * r + 1) + "](";
		for (int i = -r; i <= r; ++i)
		{
			s += GlslNum(Weight(i, sigma, r));
			if (i < r) s += ", ";
		}
		s += ");\n";
		s += "float vpu_band_from_base_w(int k) { return vpu_band_from_base_tab[k + VPU_BAND_FROM_BASE_R]; }\n";
		s += "vec4 vpu_band_from_base_across(sampler2D src, ivec2 q)\n{\n    ivec2 hi = textureSize(src, 0) - ivec2(1);\n    vec4 acc = vec4(0.0);\n"
			"    for (int i = -VPU_BAND_FROM_BASE_R; i <= VPU_BAND_FROM_BASE_R; i++)\n"
			"        acc += vpu_band_from_base_w(i) * texelFetch(src, clamp(q + ivec2(i, 0), ivec2(0), hi), 0);\n    return acc;\n}\n";
		s += "vec4 vpu_band_from_base_down(sampler2D src, ivec2 q)\n{\n    ivec2 hi = textureSize(src, 0) - ivec2(1);\n    vec4 acc = vec4(0.0);\n"
			"    for (int i = -VPU_BAND_FROM_BASE_R; i <= VPU_BAND_FROM_BASE_R; i++)\n"
			"        acc += vpu_band_from_base_w(i) * texelFetch(src, clamp(q + ivec2(0, i), ivec2(0), hi), 0);\n    return acc;\n}\n";
		return s;
	}
}
}
