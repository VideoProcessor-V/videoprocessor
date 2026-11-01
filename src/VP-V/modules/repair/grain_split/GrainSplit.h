// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// repair.grain_split (DRAFT): the shared grain-free picture. One split per presented source frame,
// computed once and read by every consumer that must not touch grain (temporal_detail today; chroma,
// mosquito repair and scaling are the intended next readers):
//
//     clean = base + f D,   G = Y - clean   (so Y = clean + G exactly, by construction)
//
// The estimator is texture_restore's (repair.texture_restore, TextureRestore.cpp): a per-block grain level
// N from the quietest sub-blocks (minimum over a neighbourhood) and the per-pixel Wiener share
// f = clamp(1 - kappa N / e3x3, 0, 1) on the detail band D = Y - G(sigma_base) Y. It is promoted here,
// unchanged in law, so that every reader shares one grain map. A consumer works on `clean` and adds its
// own frame's G back at the end, exactly: Y_out = F(clean) + G = Y + (F(clean) - clean).
// Misclassification is safe in that direction: structure read as grain is added back unchanged
// (no gain there, no harm); grain read as structure (dense texture) stays in `clean`, which is why
// readers that MOVE structure between frames still need their own grain budget (temporal_detail does).
//
// Cost (GPU plan, per frame): three separable Gaussians (sigma 1, 2.5, 6 x Scale; the 2.5/6 pair at half
// resolution), one 3 x 3 energy, one per-block reduction of 16 sub-block energies and a 5 x 5 block
// minimum; outputs base (half res R16F), s1/s2 (RG16F), f (R8) and N (block grid). About 7 B/px of
// textures at 4K (~58 MB) and well under 1 ms on the 3090 (estimate; not dispatched yet).
//
// CPU reference model: one luma plane in codes (doubles), edges clamped, raster loops, double sums, no
// threads: deterministic. Header-only, C++14-compatible, no Windows headers.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace GrainSplit
{
	// Storage layout of the GPU form of the split product (SA2 either/or switch `grain_split_storage`; default
	// full until the coordinator's grain gate passes). Full: rgba32f split (s1, s2, f, base) + rgba32f blocks
	// (N, e3, 0, 0). Compact: texture A rg16f (s1, s2 in codes), texture B rg16 unorm (f, base x codeUnit), blocks
	// rg32f (N, e3): 8 B/px + 8 B per block instead of 16 + 16. Readers never see the difference: GLSL gs_fetch /
	// gs_blocks and the C++ view (GrainSplitGpu.h) are the only read path.
	enum class Storage : uint8_t { Full, Compact };
	inline const char* Token(Storage s) { return s == Storage::Compact ? "compact" : "full"; }

	// THE GRAIN SPLIT IS ONE DEPENDENCY UNIT: this CPU reference (Compute), its GLSL (GrainSplitGpu.cpp) and the storage
	// layouts. Bump on ANY change of the law, the passes or a layout: a published frame carries it (Gpu::Frame::version),
	// the GLSL embeds it as a define GS_UNIT_VERSION, and a reader refuses a frame of another version (stale, visible
	// fallback, automatic recovery once a frame of the current version arrives).
	constexpr uint32_t kUnitVersion = 1;

	struct Plane
	{
		int w = 0, h = 0;
		std::vector<double> v;
		Plane() = default;
		Plane(int width, int height, double fill = 0.0) : w(width), h(height), v(static_cast<size_t>(width) * static_cast<size_t>(height), fill) {}
		double& at(int x, int y) { return v[static_cast<size_t>(y) * w + x]; }
		double at(int x, int y) const { return v[static_cast<size_t>(y) * w + x]; }
		double get(int x, int y) const { return at((std::max)(0, (std::min)(w - 1, x)), (std::max)(0, (std::min)(h - 1, y))); }
	};

	// The split's law (texture_restore's values). Sigmas are multiplied by the caller's scale.
	struct Params
	{
		double sigmaFine = 1.0;        // D1 = Y - G(sigmaFine) Y (fine band), D2 = G(sigmaFine) Y - base (mid band)
		double sigmaBase = 2.5;        // base = G(sigmaBase) Y
		double sigmaLow = 6.0;         // D3 = base - G(sigmaLow) Y (low-mid band, for focus measures)
		int subBlocks = 4;             // grain level: sub-blocks per block axis
		int quietSubBlocks = 4;        // the quietest sub-blocks averaged
		int neighbourhood = 2;         // minimum over (2 n + 1)^2 blocks
		double grainCal = 2.2;         // pure grain reads about its own energy (quiet sub-blocks read low)
		double kappa = 2.0;            // Wiener share f = clamp(1 - kappa N / e3x3, 0, 1)
		double energyFloor = 1e-3;     // codes^2
	};

	// The product of one frame. Planes are full size; N is on the block grid (bw x bh, block B px).
	struct Split
	{
		int w = 0, h = 0, bw = 0, bh = 0, B = 16;
		Plane base, d1, d2, d3, f;     // bands and the Wiener share
		Plane s1, s2;                  // grain-free structure bands: f D1, f D2
		Plane clean, grain;            // clean = base + s1 + s2, grain = Y - clean
		std::vector<double> N;         // block grain level (codes^2 of D)
	};

	inline double Clamp01(double v) { return v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v); }

	// Separable Gaussian, edges replicated; fixed summation order.
	inline Plane Gauss(const Plane& in, double sigma)
	{
		if (!(sigma > 0.0)) return in;
		const int r = (std::max)(1, static_cast<int>(std::ceil(3.0 * sigma)));
		std::vector<double> k(static_cast<size_t>(2 * r + 1));
		double sum = 0.0;
		for (int i = -r; i <= r; ++i) sum += k[static_cast<size_t>(i + r)] = std::exp(-0.5 * i * i / (sigma * sigma));
		for (double& x : k) x /= sum;
		Plane t(in.w, in.h), o(in.w, in.h);
		std::vector<double> pad(static_cast<size_t>(in.w + 2 * r));
		for (int y = 0; y < in.h; ++y)
		{
			for (int x = -r; x < in.w + r; ++x) pad[static_cast<size_t>(x + r)] = in.get(x, y);
			for (int x = 0; x < in.w; ++x)
			{
				double a = 0.0;
				for (int i = -r; i <= r; ++i) a += k[static_cast<size_t>(i + r)] * pad[static_cast<size_t>(x + r + i)];
				t.at(x, y) = a;
			}
		}
		for (int y = 0; y < in.h; ++y)
		{
			double* dst = &o.v[static_cast<size_t>(y) * in.w];
			for (int i = -r; i <= r; ++i)
			{
				const double* src = &t.v[static_cast<size_t>((std::max)(0, (std::min)(in.h - 1, y + i))) * in.w];
				const double kk = k[static_cast<size_t>(i + r)];
				for (int x = 0; x < in.w; ++x) dst[x] += kk * src[x];
			}
		}
		return o;
	}

	// The split of one luma plane. scale multiplies the sigmas; B is the block edge (px).
	inline Split Compute(const Plane& y, double scale, int B, const Params& p = Params())
	{
		Split a;
		a.w = y.w; a.h = y.h; a.B = B;
		a.bw = (y.w + B - 1) / B; a.bh = (y.h + B - 1) / B;
		const Plane g1 = Gauss(y, p.sigmaFine * scale), gb = Gauss(y, p.sigmaBase * scale), g6 = Gauss(y, p.sigmaLow * scale);
		a.base = gb;
		a.d1 = Plane(y.w, y.h); a.d2 = Plane(y.w, y.h); a.d3 = Plane(y.w, y.h);
		Plane d(y.w, y.h);
		for (size_t i = 0; i < y.v.size(); ++i)
		{
			a.d1.v[i] = y.v[i] - g1.v[i];
			a.d2.v[i] = g1.v[i] - gb.v[i];
			a.d3.v[i] = gb.v[i] - g6.v[i];
			d.v[i] = a.d1.v[i] + a.d2.v[i];
		}
		const size_t nb = static_cast<size_t>(a.bw) * a.bh;
		std::vector<double> quiet(nb, 0.0);
		for (int by = 0; by < a.bh; ++by)
			for (int bx = 0; bx < a.bw; ++bx)
			{
				std::vector<double> e;
				for (int sj = 0; sj < p.subBlocks; ++sj)
					for (int si = 0; si < p.subBlocks; ++si)
					{
						const int x0 = bx * B + si * B / p.subBlocks, x1 = (std::min)(y.w, bx * B + (si + 1) * B / p.subBlocks);
						const int y0 = by * B + sj * B / p.subBlocks, y1 = (std::min)(y.h, by * B + (sj + 1) * B / p.subBlocks);
						if (x1 <= x0 || y1 <= y0) continue;
						double sum = 0.0;
						for (int yy = y0; yy < y1; ++yy)
							for (int xx = x0; xx < x1; ++xx) sum += d.at(xx, yy) * d.at(xx, yy);
						e.push_back(sum / ((x1 - x0) * (y1 - y0)));
					}
				std::sort(e.begin(), e.end());
				const size_t n = (std::min)(e.size(), static_cast<size_t>((std::max)(1, p.quietSubBlocks)));
				double m = 0.0;
				for (size_t i = 0; i < n; ++i) m += e[i];
				quiet[static_cast<size_t>(by) * a.bw + bx] = n ? p.grainCal * m / static_cast<double>(n) : 0.0;
			}
		a.N.assign(nb, 0.0);
		for (int by = 0; by < a.bh; ++by)
			for (int bx = 0; bx < a.bw; ++bx)
			{
				double m = 1e300;
				for (int j = -p.neighbourhood; j <= p.neighbourhood; ++j)
					for (int i = -p.neighbourhood; i <= p.neighbourhood; ++i)
						m = (std::min)(m, quiet[static_cast<size_t>((std::max)(0, (std::min)(a.bh - 1, by + j))) * a.bw + (std::max)(0, (std::min)(a.bw - 1, bx + i))]);
				a.N[static_cast<size_t>(by) * a.bw + bx] = m;
			}
		a.f = Plane(y.w, y.h); a.s1 = Plane(y.w, y.h); a.s2 = Plane(y.w, y.h);
		a.clean = Plane(y.w, y.h); a.grain = Plane(y.w, y.h);
		for (int yy = 0; yy < y.h; ++yy)
			for (int xx = 0; xx < y.w; ++xx)
			{
				double e = 0.0;
				for (int j = -1; j <= 1; ++j)
					for (int i = -1; i <= 1; ++i) { const double dv = d.get(xx + i, yy + j); e += dv * dv; }
				e /= 9.0;
				const double N = a.N[static_cast<size_t>(yy / B) * a.bw + (xx / B)];
				const double f = Clamp01(1.0 - p.kappa * N / (std::max)(e, p.energyFloor));
				a.f.at(xx, yy) = f;
				a.s1.at(xx, yy) = f * a.d1.at(xx, yy);
				a.s2.at(xx, yy) = f * a.d2.at(xx, yy);
				a.clean.at(xx, yy) = gb.at(xx, yy) + a.s1.at(xx, yy) + a.s2.at(xx, yy);
				a.grain.at(xx, yy) = y.at(xx, yy) - a.clean.at(xx, yy);
			}
		return a;
	}

	// Declared product (design doc 25 section B1 shape; plain data until core/Products.h lands).
	struct ProductInfo
	{
		const char* id;
		const char* inputs;
		const char* backend;
		int history;
		const char* readers;
	};
	// ONE product node (integration 2026-11-01): token VpSkeleton::kTokenGrainSplit (core/Products.h), declared and owned by
	// input.source_analysis/regions (RegionEvidence::AppendProducts, GrainProduct; GrainView is the one read API), computed
	// once, early, on the decoded source with the picture extended by mirroring. This module is its implementation: the CPU
	// law below and the GPU passes (GrainSplitGpu.h, one Frame per plane carried in GrainProduct::gpu). The exact original
	// grain is added back once at the end of the source-domain block.
	constexpr ProductInfo kProduct = { "grain.split", "source luma, picture-extended (lag 0); p4 picture rect", "GpuCompute", 1,
		"camera model grain map, repair.temporal_detail, scaling.chroma_upscale (CfL chroma-grain guard), grain-safe contrast "
		"recovery, region block stats (declared); deband, mosquito repair, scaling (intended)" };
}
