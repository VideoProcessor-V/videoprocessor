// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// scaling.chroma_upscale / basic: the BASIC CHROMA stage, a stand-in for testing (reset 1 ADD, owner 2026-11-03; doc 65 "Chroma
// ORDER v2"). A simple SEPARABLE polyphase lanczos-class upsample of the subsampled chroma planes (4:2:0, 4:2:2, 4:1:1-style
// factors 2 and 4) to 4:4:4 with the chroma siting the source declares, for YCbCr and for the DV-tunnel IPT (composed 12-bit IPT
// 4:2:2: I at full size, P and T subsampled). Its purpose is to prove the order, the 4:4:4 hand-off to libplacebo (its chroma
// upscaler skipped) and the plumbing early; the hybrid CfL + MV-chroma upscale (reset 2) replaces it behind the same either/or:
// Original = libplacebo's chroma upscaler (the key chroma_upscale_basic = off | lanczos is DEFERRED: the SCHEMA lock is held, so
// no key exists yet and nothing calls this unit in a render).
//
// Position in the pathway (designed, not wired now): after the re-base and the grain split and the luma base repairs, before the
// grain re-add and the scaling.
//
// The law (everything exact INTEGER arithmetic, so the GLSL passes are BYTE-IDENTICAL to this model on any GPU):
//   position   chroma sample j sits at luma coordinate Lx(j) = (j + 0.5) f + shift - 0.5 (libplacebo's pl_plane::shift_x / shift_y,
//              luma-pixel units, the same convention as ChromaCfl.h: -(f-1)/2 = co-sited, 0 = centred). The chroma coordinate of
//              luma pixel x is j = (x + 0.5 - shift) / f - 0.5 = n / (4 f) with the INTEGER n = 4x + 2 - s4 - 2 f, s4 = 4 shift
//              (a shift is a multiple of a quarter luma pixel: the re-base scales a shift by its factor). i0 = floor(n / 4f),
//              phase = n - 4 f i0 in [0, 4f).
//   kernel     Lanczos a = 3 (not stretched: it is an upsample), 6 taps i0-2 .. i0+3 at distances d = k - j; the weights of each
//              of the 4f phases are computed in double, normalised, rounded to Q14 and the largest tap takes the (few-unit) rounding remainder so the
//              sum is EXACTLY 16384 (flat stays flat, DC exact).
//   passes     horizontal then vertical, each (sum + 8192) >> 14 in int32, the horizontal result kept as an integer (it may leave
//              the signal range: overshoot is signal, BTB / WTW codes pass); only the final value is clamped to the 16-bit
//              CONTAINER [0, 65535], never to the video range.
//   edges      taps clamp to the plane; inside the active picture they clamp to the picture's own rectangle and outside (bars)
//              to the bar's side, so nothing reads across the picture boundary (as ChromaCfl.h).
//   values     16-bit container codes (10-bit code << 6, 12-bit IPT << 4, ...): the filter is code-agnostic (weights sum to 1).
//
// CPU reference = this header. The GLSL form (BasicChromaGlsl in BasicChromaGpu.h) is generated from the same tables and runs the
// same integer arithmetic. Header-only, C++14, no Windows headers.
#pragma once

#include <VP-V/modules/repair/temporal_detail/products/TdProductReads.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

namespace BasicChroma
{
	// Bump on ANY change of the law, a table or the GLSL (the catalog expectation lives with the unit's rows: the unit is a shared
	// unit, tools/check_shared_units.py U14).
	constexpr uint32_t kUnitVersion = 1;

	constexpr int kLobes = 3;
	constexpr int kTaps = 2 * kLobes;                 // 6
	constexpr int kWeightBits = 14;
	constexpr int kWeightOne = 1 << kWeightBits;      // 16384
	constexpr int kMaxFactor = 4;
	constexpr int kMaxPhases = 4 * kMaxFactor;        // 16 (4f phases: shifts in quarter luma pixels)
	constexpr int kContainerMax = 65535;

	// The source's chroma siting (what a container / decoder states; libplacebo's pl_chroma_location).
	enum class Siting : uint8_t { Left, Center, TopLeft, Top, BottomLeft, Bottom };

	inline const char* Token(Siting s)
	{
		switch (s)
		{
		case Siting::Left: return "left";
		case Siting::Center: return "center";
		case Siting::TopLeft: return "top-left";
		case Siting::Top: return "top";
		case Siting::BottomLeft: return "bottom-left";
		case Siting::Bottom: return "bottom";
		}
		return "?";
	}

	// The libplacebo shift (luma-pixel units) of a siting for subsampling factors fx, fy: co-sited horizontally on Left /
	// TopLeft / BottomLeft (-(fx-1)/2), centred on Center / Top / Bottom; co-sited vertically at the top (-(fy-1)/2) or the bottom
	// (+(fy-1)/2), centred otherwise. A factor of 1 has no shift on that axis. 4:2:2 DV-tunnel IPT is Left (co-sited horizontally).
	inline void ShiftOf(Siting s, int fx, int fy, double& shiftX, double& shiftY)
	{
		const bool leftish = s == Siting::Left || s == Siting::TopLeft || s == Siting::BottomLeft;
		const bool top = s == Siting::TopLeft || s == Siting::Top;
		const bool bottom = s == Siting::BottomLeft || s == Siting::Bottom;
		shiftX = leftish ? -(fx - 1) * 0.5 : 0.0;
		shiftY = top ? -(fy - 1) * 0.5 : bottom ? (fy - 1) * 0.5 : 0.0;
	}

	// ---- the polyphase tables ------------------------------------------------------------------------------------------

	inline double Lanczos(double d)
	{
		const double ad = std::fabs(d);
		if (ad < 1e-12) return 1.0;
		if (ad >= kLobes) return 0.0;
		const double pi = 3.14159265358979323846;
		const double pd = pi * d;
		return kLobes * std::sin(pd) * std::sin(pd / kLobes) / (pd * pd);
	}

	// The Q14 weights of every phase of a factor f (phases 0 .. 4f-1; phase p is the chroma fraction p / 4f), 6 taps each for
	// the taps i0-2 .. i0+3. Exactly kWeightOne per phase.
	struct PhaseTable
	{
		int factor = 1;
		int phases = 4;
		int w[kMaxPhases][kTaps] = {};
		const int* Row(int phase) const { return w[phase]; }
	};

	inline PhaseTable BuildPhaseTable(int factor)
	{
		PhaseTable t;
		t.factor = factor;
		t.phases = 4 * factor;
		for (int p = 0; p < t.phases && p < kMaxPhases; ++p)
		{
			const double frac = static_cast<double>(p) / t.phases;   // j - i0
			double raw[kTaps], dsum = 0.0;
			for (int k = 0; k < kTaps; ++k)
			{
				const double d = (k - (kLobes - 1)) - frac;          // tap index i0 + (k - 2), distance to j
				raw[k] = Lanczos(d);
				dsum += raw[k];
			}
			// Normalise the truncated kernel first (its taps do not sum to 1), then round; only the true rounding remainder
			// (a few units of 1/16384) goes to the largest tap.
			int sum = 0, best = 0;
			for (int k = 0; k < kTaps; ++k)
			{
				t.w[p][k] = static_cast<int>(std::floor(raw[k] / dsum * kWeightOne + 0.5));
				sum += t.w[p][k];
				if (std::abs(t.w[p][k]) > std::abs(t.w[p][best])) best = k;
			}
			t.w[p][best] += kWeightOne - sum;
		}
		return t;
	}

	inline bool SupportedFactor(int f) { return f == 1 || f == 2 || f == 4; }

	inline int FloorDiv(int a, int b)   // b > 0
	{
		int q = a / b;
		if ((a % b) != 0 && a < 0) --q;
		return q;
	}

	// i0 and phase of luma pixel x for factor f and the quadrupled shift s4.
	inline void Locate(int x, int f, int s4, int& i0, int& phase)
	{
		const int n = 4 * x + 2 - s4 - 2 * f;
		const int den = 4 * f;
		i0 = FloorDiv(n, den);
		phase = n - i0 * den;
	}

	// ---- planes and geometry --------------------------------------------------------------------------------------------

	struct Plane16
	{
		int w = 0, h = 0;
		std::vector<uint16_t> v;
		Plane16() = default;
		Plane16(int w_, int h_, uint16_t fill = 0) : w(w_), h(h_), v(static_cast<size_t>(w_ > 0 ? w_ : 0) * (h_ > 0 ? h_ : 0), fill) {}
		uint16_t& at(int x, int y) { return v[static_cast<size_t>(y) * w + x]; }
		uint16_t at(int x, int y) const { return v[static_cast<size_t>(y) * w + x]; }
	};

	struct Rect { int x0 = 0, y0 = 0, x1 = 0, y1 = 0; bool Empty() const { return x1 <= x0 || y1 <= y0; } };

	// The geometry of one upsample: luma size, the factors, the quadrupled shifts and the active picture (luma pixels, empty = whole).
	struct Geometry
	{
		int lumaW = 0, lumaH = 0;
		int fx = 2, fy = 2;
		int s4x = 0, s4y = 0;     // 4 x the libplacebo shifts
		Rect active;              // empty = the whole plane
		bool shiftOk = true;      // false: a siting shift that is not a multiple of a quarter luma pixel (refused by Check)
		int chromaW() const { return (lumaW + fx - 1) / fx; }
		int chromaH() const { return (lumaH + fy - 1) / fy; }
	};

	// The status of a geometry: "" when it can run, else the reason (the caller shows "Chroma upscale: libplacebo (<reason>)").
	inline std::string Check(const Geometry& g)
	{
		if (g.lumaW <= 0 || g.lumaH <= 0) return "invalid plane sizes";
		if (!g.shiftOk) return "siting shift is not a multiple of a quarter luma pixel";
		if (!SupportedFactor(g.fx) || !SupportedFactor(g.fy)) return "subsampling factor not 1, 2 or 4";
		return std::string();
	}

	// A libplacebo shift to the quadrupled integer shift; false when it is not a multiple of a quarter luma pixel.
	inline bool Shift4Of(double shift, int& s4)
	{
		const double d = shift * 4.0;
		s4 = static_cast<int>(std::floor(d + 0.5));
		return std::fabs(d - s4) < 1e-6;
	}

	inline Geometry MakeGeometry(int lumaW, int lumaH, int fx, int fy, double shiftX, double shiftY, const Rect& active, std::string* why)
	{
		Geometry g;
		g.lumaW = lumaW; g.lumaH = lumaH; g.fx = fx; g.fy = fy; g.active = active;
		if (!Shift4Of(shiftX, g.s4x) || !Shift4Of(shiftY, g.s4y))
		{
			if (why) *why = "siting shift is not a multiple of a quarter luma pixel";
			g.shiftOk = false;
		}
		return g;
	}

	// The active picture's rectangle on the chroma plane, outward rounded and clamped (as ChromaCfl.h).
	inline Rect ChromaRect(const Rect& r, int fx, int fy, int cw, int ch)
	{
		Rect c;
		c.x0 = (std::max)(0, (std::min)(cw, r.x0 / fx));
		c.x1 = (std::max)(0, (std::min)(cw, (r.x1 + fx - 1) / fx));
		c.y0 = (std::max)(0, (std::min)(ch, r.y0 / fy));
		c.y1 = (std::max)(0, (std::min)(ch, (r.y1 + fy - 1) / fy));
		if (c.Empty()) { c.x0 = 0; c.y0 = 0; c.x1 = cw; c.y1 = ch; }
		return c;
	}

	// The tap clamp interval [lo, hi] (chroma indices) of luma coordinate x on one axis: the picture's own rectangle inside, the bar's
	// side outside. L, R: the active interval in luma pixels, cL, cR in chroma samples, cn: chroma plane size.
	inline void ClampRange(int x, int L, int R, int cL, int cR, int cn, int& lo, int& hi)
	{
		if (x >= L && x < R) { lo = cL; hi = cR - 1; }
		else if (x < L) { lo = 0; hi = (std::max)(cL - 1, 0); }
		else { lo = (std::min)(cR, cn - 1); hi = cn - 1; }
	}

	// ---- the CPU reference -----------------------------------------------------------------------------------------------

	// One chroma plane (Cb, Cr or P, T) to luma size. `c` is cw x ch with cw = ceil(lumaW / fx), ch = ceil(lumaH / fy).
	inline Plane16 UpsamplePlane(const Plane16& c, const Geometry& g)
	{
		if (!Check(g).empty()) return Plane16();   // callers go through Upsample / Check; an unsupported geometry makes no picture
		const int W = g.lumaW, H = g.lumaH, cw = c.w, ch = c.h;
		Plane16 out(W, H);
		if (g.fx == 1 && g.fy == 1) { out.v = c.v; return out; }
		const PhaseTable tx = BuildPhaseTable(g.fx), ty = BuildPhaseTable(g.fy);
		const Rect lr = g.active.Empty() ? Rect{ 0, 0, W, H } : g.active;
		const Rect cr = ChromaRect(lr, g.fx, g.fy, cw, ch);
		// Horizontal pass over every chroma row, per output column (the clamp interval depends on the column only).
		std::vector<int> hrow(static_cast<size_t>(W) * ch);
		for (int x = 0; x < W; ++x)
		{
			int i0 = 0, ph = 0, lo = 0, hi = 0;
			Locate(x, g.fx, g.s4x, i0, ph);
			ClampRange(x, lr.x0, lr.x1, cr.x0, cr.x1, cw, lo, hi);
			const int* wt = tx.Row(ph);
			for (int y = 0; y < ch; ++y)
			{
				int acc = 0;
				for (int k = 0; k < kTaps; ++k)
					acc += static_cast<int>(c.at((std::min)((std::max)(i0 - (kLobes - 1) + k, lo), hi), y)) * wt[k];
				hrow[static_cast<size_t>(y) * W + x] = (acc + kWeightOne / 2) >> kWeightBits;
			}
		}
		for (int y = 0; y < H; ++y)
		{
			int j0 = 0, ph = 0, lo = 0, hi = 0;
			Locate(y, g.fy, g.s4y, j0, ph);
			ClampRange(y, lr.y0, lr.y1, cr.y0, cr.y1, ch, lo, hi);
			const int* wt = ty.Row(ph);
			for (int x = 0; x < W; ++x)
			{
				int acc = 0;
				for (int k = 0; k < kTaps; ++k)
					acc += hrow[static_cast<size_t>((std::min)((std::max)(j0 - (kLobes - 1) + k, lo), hi)) * W + x] * wt[k];
				const int v = (acc + kWeightOne / 2) >> kWeightBits;
				out.at(x, y) = static_cast<uint16_t>((std::min)((std::max)(v, 0), kContainerMax));
			}
		}
		return out;
	}

	struct Result
	{
		Plane16 cb, cr;            // full luma size (for IPT: P and T)
		std::string status;        // ok, "4:4:4: nothing to upsample", or the failure reason
		// R1-W1-RES: the read path of the temporal-detail engine's chroma products (see TdProductReads.h). Reset 1: not requested,
		// nothing used; the plain kernel below runs. A requested engine with products missing shows through td.fallback.
		TdProducts::ReadResult td;
	};

	// The basic chroma upsample of a pair of planes (Cb + Cr, or P + T of a composed IPT 4:2:2 frame).
	inline Result Upsample(const Plane16& cb, const Plane16& cr, const Geometry& g,
		const TdProducts::Snapshot* td = TdProducts::PlaceholderSnapshot(), bool tdRequested = TdProducts::kEngineRequestedToday,
		int64_t nowUs = 0)
	{
		Result r;
		r.td = TdProducts::Read(TdProducts::Consumer::ChromaStage, td, tdRequested, nowUs);
		const std::string why = Check(g);
		if (!why.empty()) { r.status = "chroma upscale: " + why; return r; }
		if (cb.w != g.chromaW() || cb.h != g.chromaH() || cr.w != cb.w || cr.h != cb.h)
		{
			r.status = "chroma upscale: chroma plane size does not match the luma size and factors";
			return r;
		}
		if (g.fx == 1 && g.fy == 1) { r.cb = cb; r.cr = cr; r.status = "4:4:4: nothing to upsample"; return r; }
		r.cb = UpsamplePlane(cb, g);
		r.cr = UpsamplePlane(cr, g);
		r.status = "ok";
		return r;
	}
}
