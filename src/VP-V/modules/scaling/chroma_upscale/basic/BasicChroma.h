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
//
// CF-2 B5a-2 step 2: the tables, the phase location, the tap clamps and both integer passes (CPU and GLSL) are ScalerPass's Q14
// fixed-point mode (scaling/shared/ScalerPassFixed.h); this unit keeps its geometry, sitings, checks and status and calls it.
#pragma once

#include <VP-V/modules/repair/temporal_detail/products/TdProductReads.h>
#include <VP-V/modules/scaling/shared/KernelTable.h>
#include <VP-V/modules/scaling/shared/ScalerPassFixed.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

namespace BasicChroma
{
	// Bump on ANY change of the law, a table or the GLSL (the catalog expectation lives with the unit's rows: the unit is a shared
	// unit, tools/check_shared_units.py U20).
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

	// The pre-CF-2 private kernel, kept only as the reference the equality test compares the shared table against (BuildPhaseTable
	// takes its weights from ScalerKernels, the one kernel table, refs/123 N1).
	inline double Lanczos(double d)
	{
		const double ad = std::fabs(d);
		if (ad < 1e-12) return 1.0;
		if (ad >= kLobes) return 0.0;
		const double pi = 3.14159265358979323846;
		const double pd = pi * d;
		return kLobes * std::sin(pd) * std::sin(pd / kLobes) / (pd * pd);
	}

	inline ScalerKernels::Spec LanczosSpec()   // the table's Lanczos a = 3 entry
	{
		ScalerKernels::Spec sp;
		sp.kind = ScalerKernels::Kind::Lanczos;
		sp.radius = kLobes;
		return sp;
	}

	// The Q14 weights of every phase of a factor f (phases 0 .. 4f-1; phase p is the chroma fraction p / 4f), 6 taps each for
	// the taps i0-2 .. i0+3. Exactly kWeightOne per phase. The shared fixed-point mode builds them from the table's Lanczos a = 3.
	using PhaseTable = ScalerPass::FixedQ14::PhaseTable;

	inline PhaseTable BuildPhaseTable(int factor) { return ScalerPass::FixedQ14::BuildPhaseTable(LanczosSpec(), factor); }

	inline bool SupportedFactor(int f) { return f == 1 || f == 2 || f == 4; }

	inline int FloorDiv(int a, int b) { return ScalerPass::FixedQ14::FloorDiv(a, b); }   // b > 0

	// i0 and phase of luma pixel x for factor f and the quadrupled shift s4.
	inline void Locate(int x, int f, int s4, int& i0, int& phase) { ScalerPass::FixedQ14::Locate(x, f, s4, i0, phase); }

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
		ScalerPass::FixedQ14::ClampRange(x, L, R, cL, cR, cn, lo, hi);
	}

	// The two axes of the shared fixed-point mode for a geometry (luma = output, chroma = source; the active picture and its
	// outward-rounded chroma rectangle as the clamp intervals).
	inline void AxesOf(const Geometry& g, ScalerPass::FixedQ14::Axis& ax, ScalerPass::FixedQ14::Axis& ay)
	{
		const int cw = g.chromaW(), ch = g.chromaH();
		const Rect lr = g.active.Empty() ? Rect{ 0, 0, g.lumaW, g.lumaH } : g.active;
		const Rect cr = ChromaRect(lr, g.fx, g.fy, cw, ch);
		ax.out = g.lumaW; ax.in = cw; ax.factor = g.fx; ax.s4 = g.s4x; ax.L = lr.x0; ax.R = lr.x1; ax.cL = cr.x0; ax.cR = cr.x1;
		ay.out = g.lumaH; ay.in = ch; ay.factor = g.fy; ay.s4 = g.s4y; ay.L = lr.y0; ay.R = lr.y1; ay.cL = cr.y0; ay.cR = cr.y1;
	}

	// ---- the CPU reference -----------------------------------------------------------------------------------------------

	// One chroma plane (Cb, Cr or P, T) to luma size. `c` is cw x ch with cw = ceil(lumaW / fx), ch = ceil(lumaH / fy).
	inline Plane16 UpsamplePlane(const Plane16& c, const Geometry& g)
	{
		if (!Check(g).empty()) return Plane16();   // callers go through Upsample / Check; an unsupported geometry makes no picture
		// The plane must be the geometry's chroma plane (cw = ceil(lumaW / fx) ...) and hold every sample: the loops below index it by the
		// geometry, so a smaller or mislabelled plane would read outside it. No picture (an empty plane) rather than undefined reads.
		if (c.w != g.chromaW() || c.h != g.chromaH() || c.w <= 0 || c.h <= 0 ||
			c.v.size() < static_cast<size_t>(c.w) * static_cast<size_t>(c.h)) return Plane16();
		Plane16 out(g.lumaW, g.lumaH);
		if (g.fx == 1 && g.fy == 1) { out.v = c.v; return out; }
		ScalerPass::FixedQ14::Axis ax, ay;
		AxesOf(g, ax, ay);
		if (!ScalerPass::FixedQ14::Upsample(c.v.data(), c.v.size(), ax, ay, LanczosSpec(), out.v)) return Plane16();
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
