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
// Cost (v3 GPU form, GrainSplitGpu.h; refs/116): the 2.5 / 6 (x Scale) Gaussians at half resolution, G1 (1 x Scale)
// at full resolution in shared memory, one 3 x 3 energy, one per-block reduction of the sub-block energies and a 5 x 5
// block minimum; no full-size intermediates; Compact product 8 B/px. Estimate 1.4-1.9 ms at 4K on the RTX 3050 Ti
// laptop (target <= 2 ms; GrainSplitV3CostBench measures).
//
// CPU reference model: one luma plane in codes (doubles), edges clamped, raster loops, double sums, no
// threads: deterministic. Header-only, C++14-compatible, no Windows headers.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

#include <VP-V/modules/repair/grain_split/shared/BandFromBase.h>
#include <VP-V/modules/input/source_analysis/lane/shared/HalfLuma.h>

namespace GrainSplit
{
	// Storage layout of the GPU form of the split product (SA2 either/or switch `grain_split_storage`; default
	// compact since v3, decision D2). Full: rgba32f split (s1, s2, f, base) + rgba32f blocks
	// (N, e3, e1, e2). Compact: texture A rg16f (s1, s2 in codes), texture B rg16 unorm (f, base x codeUnit), blocks
	// rgba32f (N, e3, e1, e2): 8 B/px + 16 B per block instead of 16 + 16; e1/e2 are the block means of d1^2 / d2^2
	// (extra modifier fields, version 2). Readers never see the layout: GLSL gs_fetch / gs_blocks / gs_energies / gs_tilt
	// and the C++ view (GrainSplitGpu.h) are the only read path.
	enum class Storage : uint8_t { Full, Compact };
	inline const char* Token(Storage s) { return s == Storage::Compact ? "compact" : "full"; }

	// THE GRAIN SPLIT IS ONE DEPENDENCY UNIT: this CPU reference (Compute), its GLSL (GrainSplitGpu.cpp) and the storage
	// layouts. Bump on ANY change of the law, the passes or a layout: a published frame carries it (Gpu::Frame::version),
	// the GLSL embeds it as a define GS_UNIT_VERSION, and a reader refuses a frame of another version (stale, visible
	// fallback, automatic recovery once a frame of the current version arrives).
	constexpr uint32_t kUnitVersion = 4;   // 4: N capped by the frame noise level function (refs/116b); 2: Compact blocks carry the band energies (e1, e2); 3: the v3 law (the two low
	                                       // bands at half resolution, refs/116; WAIVER-P10V3-01 covers the v2 pins)

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
		double energyFloor = 1e-3;     // codes^2 (ten-bit-equivalent units, see sourceBits)
		// Noise level function cap (refs/116b, kUnitVersion 4): N_block = min(N_local, NLF(block luma)).
		int levelBins = 8;             // luma bins over the code range
		int levelPercentile = 10;      // percentile of the blocks' quiet values per bin
		int levelMinBlocks = 8;        // a bin with fewer blocks has no level (interpolated)
		bool levelCap = true;          // test-only off switch (ParamsV2 equivalence); NOT a user setting
		// Source depth of the plane's codes (10 = legacy tap, 12 = the 12-bit tap). Every code-domain limit of the
		// law (energyFloor, the level key floor, the luma range) is stated in ten-bit-equivalent units and scaled by
		// the depth: codes x 2^(bits-10), energies x 2^(2 (bits-10)). Never a hard-coded depth.
		int sourceBits = 10;
	};
	// The code scale of a depth relative to ten bits (2^(bits-10)); the energy scale is its square.
	inline double DepthScale(int bits) { return std::ldexp(1.0, bits - 10); }

	// The product of one frame. Planes are full size; N is on the block grid (bw x bh, block B px).
	struct Split
	{
		int w = 0, h = 0, bw = 0, bh = 0, B = 16;
		Plane base, d1, d2, d3, f;     // bands and the Wiener share
		Plane s1, s2;                  // grain-free structure bands: f D1, f D2
		Plane clean, grain;            // clean = base + s1 + s2, grain = Y - clean
		std::vector<double> N;         // block grain level (codes^2 of D)
		std::vector<double> e1, e2, e3; // block means of d1^2, d2^2, d3^2 (codes^2; raster block order)
	};

	inline double Clamp01(double v) { return v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v); }

	// Block means of d1^2, d2^2, d3^2 over each B x B block (edge blocks partial), raster order, double sums.
	inline void BlockMeans(Split& a, const Plane& d1, const Plane& d2, const Plane& d3)
	{
		const size_t nb = static_cast<size_t>(a.bw) * a.bh;
		a.e1.assign(nb, 0.0); a.e2.assign(nb, 0.0); a.e3.assign(nb, 0.0);
		for (int by = 0; by < a.bh; ++by)
			for (int bx = 0; bx < a.bw; ++bx)
			{
				const int x1 = (std::min)(a.w, (bx + 1) * a.B), y1 = (std::min)(a.h, (by + 1) * a.B);
				double s1 = 0.0, s2 = 0.0, s3 = 0.0;
				for (int yy = by * a.B; yy < y1; ++yy)
					for (int xx = bx * a.B; xx < x1; ++xx)
					{
						s1 += d1.at(xx, yy) * d1.at(xx, yy);
						s2 += d2.at(xx, yy) * d2.at(xx, yy);
						s3 += d3.at(xx, yy) * d3.at(xx, yy);
					}
				const double cnt = static_cast<double>((x1 - bx * a.B) * (y1 - by * a.B));
				const size_t i = static_cast<size_t>(by) * a.bw + bx;
				a.e1[i] = s1 / cnt; a.e2[i] = s2 / cnt; a.e3[i] = s3 / cnt;
			}
	}

	// ---- v3 law helpers (refs/116 s6): the low-band structure of texture_restore (TextureRestore.cpp passes 1-3) ----
	// Half-resolution sigma and radius of a full-resolution sigma: the shared U2 law (BandFromBase::HalfSigma: the U3 half
	// step adds a quarter px^2 per axis) with texture_restore's half radius rule (3.5 sigma; TextureRestore.h HalfRadius).
	inline double V3HalfSigma(double sigma) { return VpUnit::BandFromBase::HalfSigma(sigma); }
	inline int V3HalfRadius(double sigma) { return VpUnit::BandFromBase::Radius(V3HalfSigma(sigma), 3.5, true, 1e-9); }
	inline double V3HalfWeight(int k, double sigma) { return VpUnit::BandFromBase::Weight(k, V3HalfSigma(sigma), V3HalfRadius(sigma)); }
	// The smallest full-resolution low-band sigma the v3 law accepts (HalfSigma needs sigma^2 > 0.25).
	constexpr double kV3MinLowSigma = 0.55;

	// The half plane by the shared U3 law (VpUnit::HalfLuma: grid HalfDim, taps Tap0 / Tap1, the mean MeanF in the
	// accessor's tap order), in codes: the CPU twin of the GPU pass's vpu_half_luma.
	inline Plane V3Half(const Plane& y)
	{
		Plane o(static_cast<int>(VpUnit::HalfLuma::HalfDim(static_cast<uint32_t>(y.w))), static_cast<int>(VpUnit::HalfLuma::HalfDim(static_cast<uint32_t>(y.h))));
		for (int j = 0; j < o.h; ++j)
			for (int i = 0; i < o.w; ++i)
			{
				const int x0 = VpUnit::HalfLuma::Tap0(i, y.w), x1 = VpUnit::HalfLuma::Tap1(i, y.w);
				const int y0 = VpUnit::HalfLuma::Tap0(j, y.h), y1 = VpUnit::HalfLuma::Tap1(j, y.h);
				o.at(i, j) = VpUnit::HalfLuma::MeanF(y.at(x0, y0), y.at(x1, y0), y.at(x0, y1), y.at(x1, y1));
			}
		return o;
	}

	// Separable blur of a half-resolution plane at the half law of a full-resolution sigma: across, then down; edges
	// clamped; taps -r..r in order.
	inline Plane V3HalfBlur(const Plane& h, double fullSigma)
	{
		const int r = V3HalfRadius(fullSigma);
		std::vector<double> k(static_cast<size_t>(2 * r + 1));
		for (int i = -r; i <= r; ++i) k[static_cast<size_t>(i + r)] = V3HalfWeight(i, fullSigma);
		Plane t(h.w, h.h), o(h.w, h.h);
		for (int y = 0; y < h.h; ++y)
			for (int x = 0; x < h.w; ++x)
			{
				double a = 0.0;
				for (int i = -r; i <= r; ++i) a += k[static_cast<size_t>(i + r)] * h.get(x + i, y);
				t.at(x, y) = a;
			}
		for (int y = 0; y < h.h; ++y)
			for (int x = 0; x < h.w; ++x)
			{
				double a = 0.0;
				for (int i = -r; i <= r; ++i) a += k[static_cast<size_t>(i + r)] * t.get(x, y + i);
				o.at(x, y) = a;
			}
		return o;
	}

	// A half-resolution plane at a full-resolution pixel: bilinear at (p + 0.5) / 2 - 0.5 with clamped indices
	// (texture_restore low_at); the weights are exactly 0.25 / 0.75 per axis.
	inline double V3LowAt(const Plane& h, int x, int y)
	{
		const double hx = (x + 0.5) * 0.5 - 0.5, hy = (y + 0.5) * 0.5 - 0.5;
		const int i0 = static_cast<int>(std::floor(hx)), j0 = static_cast<int>(std::floor(hy));
		const double fx = hx - i0, fy = hy - j0;
		const double a = h.get(i0, j0), b = h.get(i0 + 1, j0), c = h.get(i0, j0 + 1), d = h.get(i0 + 1, j0 + 1);
		const double top = a + (b - a) * fx, bot = c + (d - c) * fx;
		return top + (bot - top) * fy;
	}

	// Separable Gaussian, edges replicated; fixed summation order (the shared U2 law: VpUnit::BandFromBase).
	inline Plane Gauss(const Plane& in, double sigma)
	{
		if (!(sigma > 0.0)) return in;
		Plane o(in.w, in.h);
		o.v = VpUnit::BandFromBase::Blur(in.v, in.w, in.h, sigma);
		return o;
	}

	// ---- NLF cap (refs/116b s4) ----
	constexpr int kLevelKeyBase = 968;   // (127 - 6) x 8: key 0 = 2^-6 codes^2
	constexpr int kLevelKeys = 160;      // 1/8-octave classes, 2^-6 .. 2^14 codes^2
	inline uint32_t LevelFloatBits(float v) { uint32_t u; std::memcpy(&u, &v, sizeof(u)); return u; }
	inline float LevelBitsFloat(uint32_t u) { float v; std::memcpy(&v, &u, sizeof(v)); return v; }
	// Integer class key of a quiet value in ten-bit-equivalent codes^2: identical on CPU and GPU.
	inline int LevelKey(double quiet10)
	{
		const float q = static_cast<float>((std::max)(quiet10, 1.0 / 64.0));
		const int k = static_cast<int>(LevelFloatBits(q) >> 20) - kLevelKeyBase;
		return k < 0 ? 0 : (k >= kLevelKeys ? kLevelKeys - 1 : k);
	}
	// The class midpoint of a key, codes^2 (ten-bit-equivalent).
	inline double LevelOfKey(int key) { return static_cast<double>(LevelBitsFloat((static_cast<uint32_t>(key + kLevelKeyBase) << 20) | (1u << 19))); }
	inline int LevelBin(double lumaTen, const Params& p)
	{
		const int j = static_cast<int>(std::floor(lumaTen * p.levelBins / 1024.0));
		return j < 0 ? 0 : (j >= p.levelBins ? p.levelBins - 1 : j);
	}
	// The per-bin table: quiet and blockLuma in the plane's own codes; level[] in ten-bit-equivalent codes^2; valid[j]
	// false for a bin with fewer than levelMinBlocks blocks. level / valid hold at least p.levelBins entries.
	inline void GrainLevelTable(const std::vector<double>& quiet, const std::vector<double>& blockLuma, const Params& p, double* level, bool* valid)
	{
		const double cs = DepthScale(p.sourceBits), es = cs * cs;
		std::vector<std::vector<int>> keys(static_cast<size_t>(p.levelBins));
		for (size_t i = 0; i < quiet.size(); ++i)
			keys[static_cast<size_t>(LevelBin(blockLuma[i] / cs, p))].push_back(LevelKey(quiet[i] / es));
		for (int j = 0; j < p.levelBins; ++j)
		{
			std::vector<int>& k = keys[static_cast<size_t>(j)];
			const int n = static_cast<int>(k.size());
			valid[j] = n >= p.levelMinBlocks && n > 0;
			level[j] = 0.0;
			if (!valid[j]) continue;
			std::sort(k.begin(), k.end());
			const int r = (p.levelPercentile * n + 99) / 100;   // ceil(percentile n / 100), r >= 1
			level[j] = LevelOfKey(k[static_cast<size_t>((std::max)(1, r) - 1)]);
		}
	}
	// cap(L): piecewise linear over the valid bin centres, constant beyond the first / last valid bin. Returns false (no
	// cap) when no bin has a level. lumaTen in ten-bit-equivalent codes; the result is ten-bit-equivalent codes^2.
	inline bool GrainLevelCap(const double* level, const bool* valid, const Params& p, double lumaTen, double& cap)
	{
		int lo = -1, hi = -1;
		const double pos = lumaTen * p.levelBins / 1024.0 - 0.5;   // bin-centre coordinates
		for (int j = 0; j < p.levelBins; ++j)
		{
			if (!valid[j]) continue;
			if (j <= pos) lo = j;
			if (j >= pos && hi < 0) hi = j;
		}
		if (lo < 0 && hi < 0) return false;
		if (lo < 0) { cap = level[hi]; return true; }
		if (hi < 0 || hi == lo) { cap = level[lo]; return true; }
		const double t = (pos - lo) / static_cast<double>(hi - lo);
		cap = level[lo] + (level[hi] - level[lo]) * t;
		return true;
	}
	// Apply the cap to N_local (block grid), in place. blockLuma = the half-res base at each block centre.
	inline void ApplyLevelCap(std::vector<double>& N, const std::vector<double>& quiet, const std::vector<double>& blockLuma, const Params& p)
	{
		if (!p.levelCap || p.levelBins <= 0) return;
		std::vector<double> level(static_cast<size_t>(p.levelBins));
		std::unique_ptr<bool[]> valid(new bool[static_cast<size_t>(p.levelBins)]);
		GrainLevelTable(quiet, blockLuma, p, level.data(), valid.get());
		const double cs = DepthScale(p.sourceBits), es = cs * cs;
		for (size_t i = 0; i < N.size(); ++i)
		{
			double cap = 0.0;
			if (GrainLevelCap(level.data(), valid.get(), p, blockLuma[i] / cs, cap)) N[i] = (std::min)(N[i], cap * es);
		}
	}
	// V2 parameters for the equivalence tests: the v3 law with the cap off.
	inline Params ParamsV2() { Params p; p.levelCap = false; return p; }

	// THE V2 LAW (kUnitVersion 2: every Gaussian at full resolution). Kept ONLY as the A/B baseline of the v3 equivalence
	// tests (refs/116 s5); no production reader. scale multiplies the sigmas; B is the block edge (px).
	inline Split ComputeV2(const Plane& y, double scale, int B, const Params& p = Params())
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
		BlockMeans(a, a.d1, a.d2, a.d3);
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

	// THE SPLIT (v3 law, kUnitVersion 3; refs/116 s6). The estimator of v2 unchanged (sub-block grain level, neighbourhood
	// minimum, Wiener share on D = d1 + d2), with the two LOW bands computed at HALF resolution as texture_restore does:
	//   Yh = the U3 half plane of Y; Gbh, G6h = half-resolution blurs at HalfSigma(sigmaBase s), HalfSigma(sigmaLow s), read at full
	//   resolution bilinearly (V3LowAt). G1 (sigmaFine s) stays at full resolution.
	//   base = Gb(p), d1 = Y - G1, d2 = G1 - base, d3 = base - G6(p), D = d1 + d2 = Y - base.
	// sigmaBase s and sigmaLow s must be >= kV3MinLowSigma (the GPU form refuses the law otherwise).
	inline Split Compute(const Plane& y, double scale, int B, const Params& p = Params())
	{
		Split a;
		a.w = y.w; a.h = y.h; a.B = B;
		a.bw = (y.w + B - 1) / B; a.bh = (y.h + B - 1) / B;
		const Plane g1 = Gauss(y, p.sigmaFine * scale);
		const Plane yh = V3Half(y);
		const Plane gbh = V3HalfBlur(yh, p.sigmaBase * scale), g6h = V3HalfBlur(yh, p.sigmaLow * scale);
		a.base = Plane(y.w, y.h);
		a.d1 = Plane(y.w, y.h); a.d2 = Plane(y.w, y.h); a.d3 = Plane(y.w, y.h);
		Plane d(y.w, y.h);
		for (int yy = 0; yy < y.h; ++yy)
			for (int xx = 0; xx < y.w; ++xx)
			{
				const double gb = V3LowAt(gbh, xx, yy), g6 = V3LowAt(g6h, xx, yy);
				a.base.at(xx, yy) = gb;
				a.d1.at(xx, yy) = y.at(xx, yy) - g1.at(xx, yy);
				a.d2.at(xx, yy) = g1.at(xx, yy) - gb;
				a.d3.at(xx, yy) = gb - g6;
				d.at(xx, yy) = a.d1.at(xx, yy) + a.d2.at(xx, yy);
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
		{
			// NLF cap (refs/116b): the block luma is the half-res base at the block centre (V3LowAt, edge blocks clamp).
			std::vector<double> blockLuma(nb, 0.0);
			for (int by = 0; by < a.bh; ++by)
				for (int bx = 0; bx < a.bw; ++bx)
					blockLuma[static_cast<size_t>(by) * a.bw + bx] = V3LowAt(gbh, (std::min)(bx * B + B / 2, y.w - 1), (std::min)(by * B + B / 2, y.h - 1));
			ApplyLevelCap(a.N, quiet, blockLuma, p);
		}
		const double floorE = p.energyFloor * DepthScale(p.sourceBits) * DepthScale(p.sourceBits);
		BlockMeans(a, a.d1, a.d2, a.d3);
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
				const double f = Clamp01(1.0 - p.kappa * N / (std::max)(e, floorE));
				a.f.at(xx, yy) = f;
				a.s1.at(xx, yy) = f * a.d1.at(xx, yy);
				a.s2.at(xx, yy) = f * a.d2.at(xx, yy);
				a.clean.at(xx, yy) = a.base.at(xx, yy) + a.s1.at(xx, yy) + a.s2.at(xx, yy);
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
