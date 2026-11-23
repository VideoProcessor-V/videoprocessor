// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// scaling.shared: the ONE kernel table of the hybrid upscaler rebuild (refs/123 N1 / N4 B0).
// Header-only, no libplacebo, C++14. The 1-D weight expressions are the SAME expressions as
// KernelInverse::Candidates() (scaling/rebase/KernelInverse.cpp), so a table entry's phase taps equal that
// candidate's taps bit for bit (ScalerKernelTableMatchesKernelInverseTaps). B5a: KernelInverse now DRAWS its
// candidates from this table (one expression, one place), and the chroma CfL model's Catmull-Rom tap weights
// (ChromaCfl.h) are CatmullRomTapWeights below.
#pragma once

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace ScalerKernels
{
	enum class Kind { Lanczos, CatmullRom, Bicubic, Hermite, Bilinear, EwaLanczosSharp };

	// radius is used by Lanczos only (the others are fixed); B and C by Bicubic only.
	struct Spec
	{
		Kind kind = Kind::Lanczos;
		int radius = 3;
		double B = 0.0;
		double C = 0.5;
	};

	inline bool IsPolar(Kind k) { return k == Kind::EwaLanczosSharp; }

	// The radius in source texels at ratio 1 (EWA lanczossharp: libplacebo's 3.2383154841662362).
	inline double Radius(const Spec& s)
	{
		switch (s.kind)
		{
		case Kind::Lanczos: return s.radius;
		case Kind::Bilinear: return 1.0;
		case Kind::EwaLanczosSharp: return 3.2383154841662362;
		default: return 2.0;
		}
	}

	inline double Sinc(double x)
	{
		const double pi = 3.14159265358979323846;
		return std::fabs(x) < 1e-12 ? 1.0 : std::sin(pi * x) / (pi * x);
	}

	// Mitchell-Netravali family; the same expression as KernelInverse's Bicubic().
	inline double Cubic(double B, double C, double x)
	{
		x = std::fabs(x);
		if (x < 1) return ((12 - 9 * B - 6 * C) * x * x * x + (-18 + 12 * B + 6 * C) * x * x + (6 - 2 * B)) / 6;
		if (x < 2) return ((-B - 6 * C) * x * x * x + (6 * B + 30 * C) * x * x + (-12 * B - 48 * C) * x + (8 * B + 24 * C)) / 6;
		return 0.0;
	}

	// The 1-D weight at distance x (source texels / support scale). EwaLanczosSharp is radial, not a 1-D
	// kernel: it has no 1-D weight here (returns 0) and is sampled by the polar pass only.
	inline double Weight(const Spec& s, double x)
	{
		switch (s.kind)
		{
		case Kind::Lanczos: return std::fabs(x) < s.radius ? Sinc(x) * Sinc(x / s.radius) : 0.0;
		case Kind::CatmullRom: return Cubic(0.0, 0.5, x);
		case Kind::Bicubic: return Cubic(s.B, s.C, x);
		case Kind::Hermite: return Cubic(0.0, 0.0, x);   // 2x^3 - 3x^2 + 1
		case Kind::Bilinear: return (std::max)(0.0, 1.0 - std::fabs(x));
		default: return 0.0;
		}
	}

	// The four Catmull-Rom tap weights for the taps floor(u) - 1 .. floor(u) + 2 at fraction t
	// (the Cubic(0, 0.5) kernel in polynomial form). The expression is the one the chroma CfL CPU model has always
	// used (ChromaCfl.h, now calling this), kept in this exact polynomial form so its sums are bit-identical.
	inline void CatmullRomTapWeights(double t, double w[4])
	{
		const double t2 = t * t, t3 = t2 * t;
		w[0] = -0.5 * t3 + t2 - 0.5 * t;
		w[1] = 1.5 * t3 - 2.5 * t2 + 1.0;
		w[2] = -1.5 * t3 + 2.0 * t2 + 0.5 * t;
		w[3] = 0.5 * t3 - 0.5 * t2;
	}

	struct Entry
	{
		std::string name;   // a KernelInverse candidate name where one exists
		Spec spec;
		bool inKernelInverse = false;
	};

	// The table: the seven KernelInverse candidates (same names, same order), then hermite and
	// ewa_lanczossharp. Entries are drawn from KernelInverse (owner brief).
	inline const std::vector<Entry>& Table()
	{
		static const std::vector<Entry> list = [] {
			std::vector<Entry> v;
			auto add = [&v](const char* n, Kind k, int r, double B, double C, bool inv) {
				Entry e;
				e.name = n; e.spec.kind = k; e.spec.radius = r; e.spec.B = B; e.spec.C = C; e.inKernelInverse = inv;
				v.push_back(e);
			};
			add("bicubic C0.5 (Catmull-Rom)", Kind::Bicubic, 2, 0.0, 0.5, true);
			add("bicubic C0.6", Kind::Bicubic, 2, 0.0, 0.6, true);
			add("bicubic C0.75", Kind::Bicubic, 2, 0.0, 0.75, true);
			add("lanczos2", Kind::Lanczos, 2, 0.0, 0.0, true);
			add("lanczos3", Kind::Lanczos, 3, 0.0, 0.0, true);
			add("lanczos4", Kind::Lanczos, 4, 0.0, 0.0, true);
			add("bilinear", Kind::Bilinear, 1, 0.0, 0.0, true);
			add("hermite", Kind::Hermite, 2, 0.0, 0.0, false);
			add("ewa_lanczossharp", Kind::EwaLanczosSharp, 0, 0.0, 0.0, false);
			return v;
		}();
		return list;
	}

	inline int Find(const std::string& name)
	{
		const auto& t = Table();
		for (size_t i = 0; i < t.size(); ++i) if (t[i].name == name) return static_cast<int>(i);
		return -1;
	}

	// The 2x upscale phase filter at source offset `s` (-0.25 / +0.25: KernelInverse's two phases): taps
	// 2 * radius + 3 starting at first = -radius - 1, normalised, as KernelInverse::Make().
	struct PhaseTaps
	{
		int first = 0;
		std::vector<float> taps;
	};
	inline PhaseTaps Phase(const Spec& spec, double s)
	{
		const int radius = spec.kind == Kind::Bilinear ? 1 : spec.kind == Kind::Lanczos ? spec.radius : 2;
		PhaseTaps p;
		p.first = -radius - 1;
		const int n = 2 * radius + 3;
		std::vector<double> f(static_cast<size_t>(n));
		double sum = 0.0;
		for (int t = 0; t < n; ++t) { f[static_cast<size_t>(t)] = Weight(spec, s - (p.first + t)); sum += f[static_cast<size_t>(t)]; }
		p.taps.resize(f.size());
		for (int t = 0; t < n; ++t) p.taps[static_cast<size_t>(t)] = static_cast<float>(f[static_cast<size_t>(t)] / sum);
		return p;
	}
}
