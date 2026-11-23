// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// scaling.shared: ScalerPass's Q14 FIXED-POINT mode (CF-2 B5a-2 step 2, refs/123 N1 "one scaler unit"). The exact-integer form
// of a separable integer-factor upsample: the same law on the CPU (this header) and in GLSL (generated here), so the GPU pass is
// byte-identical to the CPU model on any adapter. BasicChroma (scaling.chroma_upscale / basic) is its first user and keeps its
// own geometry, status and module plumbing; the arithmetic lives here once.
//
// The law (per axis; factor f in {1, 2, 4}, s4 = 4 x the libplacebo shift in luma pixels):
//   position   output x reads the source coordinate n / (4f), n = 4x + 2 - s4 - 2f; i0 = floor(n / 4f), phase = n - 4f i0 in [0, 4f).
//   weights    a separable table kernel (KernelTable.h) of integer radius r (2r taps i0-(r-1) .. i0+r, at most kMaxTaps), each of
//              the 4f phases computed in double, normalised, rounded to Q14; the largest tap takes the rounding remainder so every
//              phase sums to EXACTLY 16384 (flat stays flat).
//   passes     horizontal then vertical, (sum + 8192) >> 14 in int32; the horizontal result stays an UNCLAMPED integer (overshoot is
//              signal: BTB / WTW codes pass); only the final value is clamped to the 16-bit CONTAINER [0, 65535].
//   edges      per output column / row the taps clamp to an interval: the active picture's own source interval inside it, the bar's
//              side outside it (nothing reads across the picture boundary).
// Header-only, C++14, no Windows headers, no libplacebo.
#pragma once

#include <VP-V/modules/scaling/shared/KernelTable.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

namespace ScalerPass
{
	namespace FixedQ14
	{
		constexpr int kWeightBits = 14;
		constexpr int kWeightOne = 1 << kWeightBits;      // 16384
		constexpr int kMaxFactor = 4;
		constexpr int kMaxPhases = 4 * kMaxFactor;        // 16 (4f phases: shifts in quarter output pixels)
		constexpr int kMaxTaps = 6;
		constexpr int kContainerMax = 65535;

		inline bool SupportedFactor(int f) { return f == 1 || f == 2 || f == 4; }

		// The tap count of a spec in this mode: 2r for a separable kernel of integer radius r with 2r <= kMaxTaps, else 0 (refused).
		inline int TapsOf(const ScalerKernels::Spec& spec)
		{
			if (ScalerKernels::IsPolar(spec.kind)) return 0;
			const double r = ScalerKernels::Radius(spec);
			const int ri = static_cast<int>(std::floor(r + 0.5));
			if (ri < 1 || std::fabs(r - ri) > 1e-9 || 2 * ri > kMaxTaps) return 0;
			return 2 * ri;
		}

		// The Q14 weights of every phase of a factor f (phase p is the source fraction p / 4f), taps i0-(r-1) .. i0+r.
		struct PhaseTable
		{
			int factor = 1;
			int phases = 4;
			int taps = kMaxTaps;
			int w[kMaxPhases][kMaxTaps] = {};
			const int* Row(int phase) const { return w[phase]; }
		};

		inline PhaseTable BuildPhaseTable(const ScalerKernels::Spec& spec, int factor)
		{
			PhaseTable t;
			t.factor = factor;
			t.phases = 4 * factor;
			t.taps = TapsOf(spec);
			const int off = t.taps / 2 - 1;
			for (int p = 0; p < t.phases && p < kMaxPhases; ++p)
			{
				const double frac = static_cast<double>(p) / t.phases;   // j - i0
				double raw[kMaxTaps] = {}, dsum = 0.0;
				for (int k = 0; k < t.taps; ++k)
				{
					const double d = (k - off) - frac;                   // tap index i0 + (k - off), distance to j
					raw[k] = ScalerKernels::Weight(spec, d);
					dsum += raw[k];
				}
				// Normalise the truncated kernel first (its taps do not sum to 1), then round; only the true rounding remainder
				// (a few units of 1/16384) goes to the largest tap.
				int sum = 0, best = 0;
				for (int k = 0; k < t.taps; ++k)
				{
					t.w[p][k] = static_cast<int>(std::floor(raw[k] / dsum * kWeightOne + 0.5));
					sum += t.w[p][k];
					if (std::abs(t.w[p][k]) > std::abs(t.w[p][best])) best = k;
				}
				if (t.taps > 0) t.w[p][best] += kWeightOne - sum;
			}
			return t;
		}

		inline int FloorDiv(int a, int b)   // b > 0
		{
			int q = a / b;
			if ((a % b) != 0 && a < 0) --q;
			return q;
		}

		// i0 and phase of output pixel x for factor f and the quadrupled shift s4.
		inline void Locate(int x, int f, int s4, int& i0, int& phase)
		{
			const int n = 4 * x + 2 - s4 - 2 * f;
			const int den = 4 * f;
			i0 = FloorDiv(n, den);
			phase = n - i0 * den;
		}

		// The tap clamp interval [lo, hi] (source indices) of output coordinate x on one axis: the picture's own interval inside,
		// the bar's side outside. L, R: the active interval in output pixels, cL, cR in source samples, cn: source size.
		inline void ClampRange(int x, int L, int R, int cL, int cR, int cn, int& lo, int& hi)
		{
			if (x >= L && x < R) { lo = cL; hi = cR - 1; }
			else if (x < L) { lo = 0; hi = (std::max)(cL - 1, 0); }
			else { lo = (std::min)(cR, cn - 1); hi = cn - 1; }
		}

		// One axis of an upsample: the output size, the source size, the factor, the quadrupled shift and the active interval
		// (L, R in output pixels; cL, cR in source samples).
		struct Axis
		{
			int out = 0, in = 0;
			int factor = 1;
			int s4 = 0;
			int L = 0, R = 0;
			int cL = 0, cR = 0;
		};

		inline bool AxisOk(const Axis& a)
		{
			return a.out > 0 && a.in > 0 && SupportedFactor(a.factor) && a.in == (a.out + a.factor - 1) / a.factor;
		}

		// The CPU model of one plane: `src` is ax.in x ay.in 16-bit container codes (row-major), `dst` becomes ax.out x ay.out.
		// False (dst cleared) on an unsupported axis, kernel or a short source: no picture rather than undefined reads.
		inline bool Upsample(const uint16_t* src, size_t srcCount, const Axis& ax, const Axis& ay, const ScalerKernels::Spec& spec,
			std::vector<uint16_t>& dst)
		{
			dst.clear();
			const int taps = TapsOf(spec);
			if (taps == 0 || !AxisOk(ax) || !AxisOk(ay) || src == nullptr ||
				srcCount < static_cast<size_t>(ax.in) * static_cast<size_t>(ay.in)) return false;
			const int W = ax.out, H = ay.out, cw = ax.in, ch = ay.in, off = taps / 2 - 1;
			const PhaseTable tx = BuildPhaseTable(spec, ax.factor), ty = BuildPhaseTable(spec, ay.factor);
			dst.assign(static_cast<size_t>(W) * static_cast<size_t>(H), 0);
			// Horizontal pass over every source row, per output column (the clamp interval depends on the column only).
			std::vector<int> hrow(static_cast<size_t>(W) * ch);
			for (int x = 0; x < W; ++x)
			{
				int i0 = 0, ph = 0, lo = 0, hi = 0;
				Locate(x, ax.factor, ax.s4, i0, ph);
				ClampRange(x, ax.L, ax.R, ax.cL, ax.cR, cw, lo, hi);
				const int* wt = tx.Row(ph);
				for (int y = 0; y < ch; ++y)
				{
					int acc = 0;
					for (int k = 0; k < taps; ++k)
						acc += static_cast<int>(src[static_cast<size_t>(y) * cw + (std::min)((std::max)(i0 - off + k, lo), hi)]) * wt[k];
					hrow[static_cast<size_t>(y) * W + x] = (acc + kWeightOne / 2) >> kWeightBits;
				}
			}
			for (int y = 0; y < H; ++y)
			{
				int j0 = 0, ph = 0, lo = 0, hi = 0;
				Locate(y, ay.factor, ay.s4, j0, ph);
				ClampRange(y, ay.L, ay.R, ay.cL, ay.cR, ch, lo, hi);
				const int* wt = ty.Row(ph);
				for (int x = 0; x < W; ++x)
				{
					int acc = 0;
					for (int k = 0; k < taps; ++k)
						acc += hrow[static_cast<size_t>((std::min)((std::max)(j0 - off + k, lo), hi)) * W + x] * wt[k];
					const int v = (acc + kWeightOne / 2) >> kWeightBits;
					dst[static_cast<size_t>(y) * W + x] = static_cast<uint16_t>((std::min)((std::max)(v, 0), kContainerMax));
				}
			}
			return true;
		}

		// ---- the GLSL form ---------------------------------------------------------------------------------------------------
		// One compute pass, one thread per output pixel, sampling `<p>_src` (two-component 16-bit unorm, source size) and storing
		// `<p>_dst` (two-component 16-bit unorm, output size). `P` is the define / table prefix (upper case), `p` the identifier
		// prefix (lower case): BasicChroma uses "BC" / "bc".

		namespace Detail
		{
			inline std::string ReplaceAll(std::string s, const std::string& from, const std::string& to)
			{
				for (size_t at = s.find(from); at != std::string::npos; at = s.find(from, at + to.size()))
					s.replace(at, from.size(), to);
				return s;
			}

			inline int Log2(int v) { int l = 0; while ((1 << l) < v) ++l; return l; }

			inline std::string Define(const std::string& name, long long v)
			{
				return "#define " + name + " " + std::to_string(v) + "\n";
			}

			inline std::string IntArray(const std::string& name, const PhaseTable& t)
			{
				const int n = kMaxPhases * t.taps;
				std::string s = "const int " + name + "[" + std::to_string(n) + "] = int[" + std::to_string(n) + "](";
				for (int p = 0; p < kMaxPhases; ++p)
					for (int k = 0; k < t.taps; ++k)
					{
						s += std::to_string(p < t.phases ? t.w[p][k] : 0);
						if (p + 1 < kMaxPhases || k + 1 < t.taps) s += ", ";
					}
				return s + ");\n";
			}

			// The range function sits in the header (a function cannot sit inside the pass body); floor division by 4f is a shift
			// (4f is 4, 8 or 16) and the phase a mask: the same results as FloorDiv for negative n (two's complement).
			const char* const kRangeTemplate = R"GLSL(
void @p_range(int x, int L, int R, int cL, int cR, int cn, out int lo, out int hi)
{
    if (x >= L && x < R) { lo = cL; hi = cR - 1; }
    else if (x < L) { lo = 0; hi = max(cL - 1, 0); }
    else { lo = min(cR, cn - 1); hi = cn - 1; }
}
)GLSL";
			const char* const kBodyTemplate = R"GLSL(
ivec2 p = ivec2(gl_GlobalInvocationID.xy);
if (p.x < @P_W && p.y < @P_H) {
    int nx = 4 * p.x + 2 - @P_S4X - 2 * @P_FX;
    int ix0 = nx >> @P_LX;
    int phx = nx & (4 * @P_FX - 1);
    int ny = 4 * p.y + 2 - @P_S4Y - 2 * @P_FY;
    int iy0 = ny >> @P_LY;
    int phy = ny & (4 * @P_FY - 1);
    int xlo, xhi, ylo, yhi;
    @p_range(p.x, @P_AX0, @P_AX1, @P_CX0, @P_CX1, @P_CW, xlo, xhi);
    @p_range(p.y, @P_AY0, @P_AY1, @P_CY0, @P_CY1, @P_CH, ylo, yhi);
    ivec2 accV = ivec2(0);
    for (int ty = 0; ty < @T; ty++) {
        int ky = clamp(iy0 - @O + ty, ylo, yhi);
        ivec2 accH = ivec2(0);
        for (int tx = 0; tx < @T; tx++) {
            int kx = clamp(ix0 - @O + tx, xlo, xhi);
            vec2 t = texelFetch(@p_src, ivec2(kx, ky), 0).xy;
            ivec2 c = ivec2(floor(t * 65535.0 + 0.5));
            accH += c * @PX_W[phx * @T + tx];
        }
        ivec2 hv = (accH + ivec2(8192)) >> 14;
        accV += hv * @PY_W[phy * @T + ty];
    }
    ivec2 o = clamp((accV + ivec2(8192)) >> 14, ivec2(0), ivec2(65535));
    imageStore(@p_dst, p, vec4(vec2(o) / 65535.0, 0.0, 1.0));
}
)GLSL";

			inline std::string Fill(const char* tmpl, const std::string& P, const std::string& p, int taps)
			{
				std::string s = ReplaceAll(tmpl, "@P", P);
				s = ReplaceAll(s, "@p", p);
				s = ReplaceAll(s, "@T", std::to_string(taps));
				return ReplaceAll(s, "@O", std::to_string(taps / 2 - 1));
			}
		}

		// The pass header: the integer defines of both axes, the two Q14 tables and the range function. Integer constants only (no
		// float constant enters the arithmetic). Empty when the axes or the kernel are refused.
		inline std::string GlslHeader(const std::string& P, const std::string& p, const Axis& ax, const Axis& ay,
			const ScalerKernels::Spec& spec)
		{
			if (TapsOf(spec) == 0 || !AxisOk(ax) || !AxisOk(ay)) return std::string();
			std::string h;
			h += Detail::Define(P + "_W", ax.out) + Detail::Define(P + "_H", ay.out);
			h += Detail::Define(P + "_CW", ax.in) + Detail::Define(P + "_CH", ay.in);
			h += Detail::Define(P + "_FX", ax.factor) + Detail::Define(P + "_FY", ay.factor);
			h += Detail::Define(P + "_LX", Detail::Log2(4 * ax.factor)) + Detail::Define(P + "_LY", Detail::Log2(4 * ay.factor));
			h += Detail::Define(P + "_S4X", ax.s4) + Detail::Define(P + "_S4Y", ay.s4);
			h += Detail::Define(P + "_AX0", ax.L) + Detail::Define(P + "_AX1", ax.R);
			h += Detail::Define(P + "_AY0", ay.L) + Detail::Define(P + "_AY1", ay.R);
			h += Detail::Define(P + "_CX0", ax.cL) + Detail::Define(P + "_CX1", ax.cR);
			h += Detail::Define(P + "_CY0", ay.cL) + Detail::Define(P + "_CY1", ay.cR);
			h += Detail::IntArray(P + "X_W", BuildPhaseTable(spec, ax.factor));
			h += Detail::IntArray(P + "Y_W", BuildPhaseTable(spec, ay.factor));
			h += Detail::Fill(Detail::kRangeTemplate, P, p, TapsOf(spec));
			return h;
		}

		// The pass body for a prefix and a kernel's tap count.
		inline std::string GlslBody(const std::string& P, const std::string& p, const ScalerKernels::Spec& spec)
		{
			return Detail::Fill(Detail::kBodyTemplate, P, p, TapsOf(spec));
		}
	}
}
