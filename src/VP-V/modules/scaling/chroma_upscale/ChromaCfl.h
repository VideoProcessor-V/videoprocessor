// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// scaling.chroma_upscale: chroma-from-luma (CfL) upsampling of a subsampled YCbCr frame (CPU reference model).
//
// libplacebo upsamples 4:2:0 / 4:2:2 chroma with a generic kernel. This module predicts the missing chroma detail from the
// full-resolution luma instead: where luma and chroma are locally correlated, chroma follows luma's edges; elsewhere it is the
// plain Catmull-Rom of the chroma, so a failed prediction cannot do harm. All values are CODES of the signal (normalised
// value / code unit); the GPU module (ChromaCflGpu) mirrors this model pass by pass in float.
//
//   A  Grain guard   luma and chroma crops are split (repair.grain_split): clean = base + s1 + s2, grain = X - clean. The
//                    regression runs on the clean pictures; the chroma grain is added back exactly after the clamp.
//   B  Luma at the chroma sites   separable tent of the clean luma, widths fx / fy, centred on the chroma sample's position
//                    in luma pixels (px = (jj + 0.5) fx + shiftX - 0.5); taps clamp at the active picture's border.
//   C  Regression    on the chroma grid, box (2r+1)^2: a = cov / (varY + eps), b = mC - a mY, rho2 = cov^2 / ((varY + eps)
//                    (varC + eps)); a and b box-averaged again (guided filter); lo / hi = min / max of the clean chroma
//                    over (2 clampR + 1)^2.
//   D  Upsample      Catmull-Rom 4 x 4 of a, b, rho2, the clean chroma, lo, hi and the chroma grain at every luma pixel of the
//                    active picture: out = base + M (clamp(A Yc + B, min(LO, HI), max(LO, HI)) - CR(a Ylo + b)) + G (residual
//                    form: the base, the Original kernel of the clean chroma, keeps any grain the split left in it). No range clamp:
//                    blacker-than-black and whiter-than-white chroma codes pass.
//   E  Bars          outside the active picture: Catmull-Rom of the raw chroma with taps clamped to the bar's own side, so a
//                    flat bar stays exactly its code and nothing inside the picture reads a bar (and the reverse).
//
// Siting comes from libplacebo's pl_plane::shift_x / shift_y (luma-pixel units; -0.5 = left / co-sited for a 2x axis, 0 =
// centred). Inside the active picture EVERY tap (grain split, luma downsample, box windows, min / max, Catmull-Rom) clamps at the
// picture's border: the bar boundary is an edge, nothing reads across it.
//
// CPU reference model: planes in codes (doubles), raster loops, double sums, no threads: deterministic. Header-only,
// C++14-compatible, no Windows headers.
#pragma once

#include <VP-V/modules/repair/grain_split/GrainSplit.h>
#include <VP-V/modules/repair/temporal_detail/TemporalDetail.h>
#include <VP-V/modules/repair/temporal_detail/products/TdProductReads.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <map>
#include <string>
#include <vector>

namespace ChromaUpscale
{
	using Plane = GrainSplit::Plane;
	using ActiveRect = TemporalDetail::ActiveRect;

	enum class Mode { Off, Cfl };

	struct Settings
	{
		Mode mode = Mode::Off;
	};

	inline std::string ToLower(std::string s)
	{
		for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
		return s;
	}

	// Key chroma_upscale_vp (off | cfl, case-insensitive). An invalid value adds a message to errors and keeps the previous
	// value of inOut; unknown keys are ignored. Returns true when the key was absent or valid.
	inline bool ParseSettings(const std::map<std::string, std::string>& kv, Settings& inOut, std::vector<std::string>* errors)
	{
		const auto it = kv.find("chroma_upscale_vp");
		if (it == kv.end()) return true;
		const std::string v = ToLower(it->second);
		if (v == "off") inOut.mode = Mode::Off;
		else if (v == "cfl") inOut.mode = Mode::Cfl;
		else
		{
			if (errors) errors->push_back(it->first + ": expected off or cfl, got '" + it->second + "'");
			return false;
		}
		return true;
	}

	struct CflParams
	{
		int radius = 2;            // regression box (2r + 1)^2 on the chroma grid
		double epsFrac = 0.002;    // eps = (epsFrac (2^depth - 1))^2, in codes^2
		int clampR = 1;            // neighbour range window (2 clampR + 1)^2 on the chroma grid
		bool grainGuard = true;    // model / test knob: false = regression on the raw planes, no grain add-back
	};

	struct Result
	{
		Plane cb, cr;              // full luma size, codes
		std::string status;        // ok, 4:4:4: nothing to upsample, or the failure reason
		// R1-W1-RES: the read path of the temporal-detail engine's chroma products (base buffers, mirror, refined vectors).
		// Reset 1: not requested, nothing used, the Original-kernel + CfL form below runs unchanged.
		TdProducts::ReadResult td;
	};

	inline double Scale(double lines) { return TemporalDetail::Scale(lines); }
	// The grain split's block edge (px) at a picture height.
	inline int BlockPx(double lines) { return (std::max)(1, static_cast<int>(std::floor(16.0 * Scale(lines) + 0.5))); }

	struct Rect { int x0 = 0, y0 = 0, x1 = 0, y1 = 0; int W() const { return x1 - x0; } int H() const { return y1 - y0; } };

	// The active picture's rectangle on the chroma plane: outward rounded, clamped to the plane.
	inline Rect ChromaRect(const ActiveRect& r, double fx, double fy, int cw, int ch)
	{
		Rect c;
		c.x0 = (std::max)(0, (std::min)(cw, static_cast<int>(std::floor(r.left / fx))));
		c.x1 = (std::max)(0, (std::min)(cw, static_cast<int>(std::ceil(r.right / fx))));
		c.y0 = (std::max)(0, (std::min)(ch, static_cast<int>(std::floor(r.top / fy))));
		c.y1 = (std::max)(0, (std::min)(ch, static_cast<int>(std::ceil(r.bottom / fy))));
		if (c.x1 <= c.x0 || c.y1 <= c.y0) { c.x0 = 0; c.y0 = 0; c.x1 = cw; c.y1 = ch; }
		return c;
	}

	inline Plane CropPlane(const Plane& p, const Rect& r)
	{
		Plane o(r.W(), r.H());
		for (int y = 0; y < o.h; ++y)
			for (int x = 0; x < o.w; ++x) o.at(x, y) = p.at(r.x0 + x, r.y0 + y);
		return o;
	}

	// Catmull-Rom weights for the taps floor(u) - 1 .. floor(u) + 2 at fraction t.
	inline void CatmullRomWeights(double t, double w[4])
	{
		const double t2 = t * t, t3 = t2 * t;
		w[0] = -0.5 * t3 + t2 - 0.5 * t;
		w[1] = 1.5 * t3 - 2.5 * t2 + 1.0;
		w[2] = -1.5 * t3 + 2.0 * t2 + 0.5 * t;
		w[3] = 0.5 * t3 - 0.5 * t2;
	}

	// 4 x 4 Catmull-Rom at (u, v) of `p`, tap indices clamped to [xlo, xhi] x [ylo, yhi]; row-inner sum.
	inline double CatmullRomClamped(const Plane& p, double u, double v, int xlo, int xhi, int ylo, int yhi)
	{
		const int iu = static_cast<int>(std::floor(u)), iv = static_cast<int>(std::floor(v));
		double wu[4], wv[4];
		CatmullRomWeights(u - iu, wu);
		CatmullRomWeights(v - iv, wv);
		double s = 0.0;
		for (int m = 0; m < 4; ++m)
		{
			const int yy = (std::max)(ylo, (std::min)(yhi, iv - 1 + m));
			double rs = 0.0;
			for (int n = 0; n < 4; ++n) rs += wu[n] * p.at((std::max)(xlo, (std::min)(xhi, iu - 1 + n)), yy);
			s += wv[m] * rs;
		}
		return s;
	}

	inline double CatmullRomAt(const Plane& p, double u, double v) { return CatmullRomClamped(p, u, v, 0, p.w - 1, 0, p.h - 1); }

	// The plain (Original-kernel stand-in) upsample of a whole chroma plane to W x H with the siting shifts.
	inline Plane PlainUpsample(const Plane& c, int W, int H, double shiftX, double shiftY)
	{
		const double fx = static_cast<double>(W) / c.w, fy = static_cast<double>(H) / c.h;
		Plane o(W, H);
		for (int y = 0; y < H; ++y)
			for (int x = 0; x < W; ++x)
				o.at(x, y) = CatmullRomAt(c, (x + 0.5 - shiftX) / fx - 0.5, (y + 0.5 - shiftY) / fy - 0.5);
		return o;
	}

	// B: the luma picture `src` (a crop whose top-left sits at luma plane position (left, top)) at the sites of the chroma
	// crop grid (ow x oh, whose top-left is chroma plane index (cl, ct)). Separable tent, normalised, rows inner then columns;
	// luma indices clamp to the crop.
	inline Plane TentAtSites(const Plane& src, int cl, int ct, int ow, int oh, double fx, double fy, double shiftX, double shiftY, int left, int top)
	{
		Plane o(ow, oh);
		for (int j = 0; j < oh; ++j)
		{
			const double py = (ct + j + 0.5) * fy + shiftY - 0.5;
			const int y0 = static_cast<int>(std::ceil(py - fy)), y1 = static_cast<int>(std::floor(py + fy));
			double wsy = 0.0;
			for (int y = y0; y <= y1; ++y) wsy += (std::max)(0.0, 1.0 - std::fabs(y - py) / fy);
			for (int i = 0; i < ow; ++i)
			{
				const double px = (cl + i + 0.5) * fx + shiftX - 0.5;
				const int x0 = static_cast<int>(std::ceil(px - fx)), x1 = static_cast<int>(std::floor(px + fx));
				double wsx = 0.0;
				for (int x = x0; x <= x1; ++x) wsx += (std::max)(0.0, 1.0 - std::fabs(x - px) / fx);
				double acc = 0.0;
				for (int y = y0; y <= y1; ++y)
				{
					const double wy = (std::max)(0.0, 1.0 - std::fabs(y - py) / fy);
					double rs = 0.0;
					for (int x = x0; x <= x1; ++x)
					{
						const double wx = (std::max)(0.0, 1.0 - std::fabs(x - px) / fx);
						rs += wx * src.get(x - left, y - top);
					}
					acc += (wy / wsy) * (rs / wsx);
				}
				o.at(i, j) = acc;
			}
		}
		return o;
	}

	// C: the local linear regression of one chroma channel on the luma at the sites, plus the neighbour range.
	struct Regression
	{
		Plane a, b;        // box-averaged
		Plane rho2;        // not averaged
		Plane lo, hi;      // min / max of the chroma over (2 clampR + 1)^2
	};

	inline Regression Regress(const Plane& ylo, const Plane& cc, int radius, double eps, int clampR)
	{
		const int w = ylo.w, h = ylo.h;
		const double n = static_cast<double>((2 * radius + 1) * (2 * radius + 1));
		Plane a(w, h), b(w, h);
		Regression r;
		r.rho2 = Plane(w, h); r.lo = Plane(w, h); r.hi = Plane(w, h);
		for (int y = 0; y < h; ++y)
			for (int x = 0; x < w; ++x)
			{
				// Two-pass (centred) moments: equal to E[Y^2] - mY^2 etc. in exact arithmetic, and exact enough in float.
				double sy = 0.0, sc = 0.0;
				for (int j = -radius; j <= radius; ++j)
					for (int i = -radius; i <= radius; ++i) { sy += ylo.get(x + i, y + j); sc += cc.get(x + i, y + j); }
				const double mY = sy / n, mC = sc / n;
				double syy = 0.0, scc = 0.0, syc = 0.0;
				for (int j = -radius; j <= radius; ++j)
					for (int i = -radius; i <= radius; ++i)
					{
						const double dy = ylo.get(x + i, y + j) - mY, dc = cc.get(x + i, y + j) - mC;
						syy += dy * dy; scc += dc * dc; syc += dy * dc;
					}
				const double vY = (std::max)(syy / n, 0.0), vC = (std::max)(scc / n, 0.0), cov = syc / n;
				const double aa = cov / (vY + eps);
				a.at(x, y) = aa;
				b.at(x, y) = mC - aa * mY;
				r.rho2.at(x, y) = GrainSplit::Clamp01(cov * cov / ((vY + eps) * (vC + eps)));
				double lo = 1e300, hi = -1e300;
				for (int j = -clampR; j <= clampR; ++j)
					for (int i = -clampR; i <= clampR; ++i)
					{
						const double v = cc.get(x + i, y + j);
						lo = (std::min)(lo, v); hi = (std::max)(hi, v);
					}
				r.lo.at(x, y) = lo; r.hi.at(x, y) = hi;
			}
		r.a = Plane(w, h); r.b = Plane(w, h);
		for (int y = 0; y < h; ++y)
			for (int x = 0; x < w; ++x)
			{
				double sa = 0.0, sb = 0.0;
				for (int j = -radius; j <= radius; ++j)
					for (int i = -radius; i <= radius; ++i) { sa += a.get(x + i, y + j); sb += b.get(x + i, y + j); }
				r.a.at(x, y) = sa / n;
				r.b.at(x, y) = sb / n;
			}
		return r;
	}

	// The upsample of a subsampled frame. Y is W x H, Cb and Cr cw x ch (same size), all in codes. shiftX / shiftY are the
	// chroma plane's libplacebo shifts; rect the active picture in luma pixels (empty = whole plane); lumaLines sets the
	// grain split's Scale (<= 0: the luma height).
	inline Result Upsample(const Plane& Y, const Plane& Cb, const Plane& Cr, double shiftX, double shiftY, const ActiveRect& activeRect,
		int colorDepth, double lumaLines, const CflParams& p = CflParams())
	{
		Result res;
		res.td = TdProducts::ReadToday(TdProducts::Consumer::ChromaStage);
		res.cb = Cb; res.cr = Cr;
		const int W = Y.w, H = Y.h, cw = Cb.w, ch = Cb.h;
		if (W <= 0 || H <= 0 || cw <= 0 || ch <= 0 || Cr.w != cw || Cr.h != ch) { res.status = "chroma upscale: invalid plane sizes"; return res; }
		if (cw == W && ch == H) { res.status = "4:4:4: nothing to upsample"; return res; }
		if (cw > W || ch > H) { res.status = "chroma upscale: chroma plane larger than luma"; return res; }
		const double fx = static_cast<double>(W) / cw, fy = static_cast<double>(H) / ch;
		const ActiveRect rc = activeRect.Resolve(W, H);
		const Rect lr = { rc.left, rc.top, rc.right, rc.bottom };
		const Rect cr = ChromaRect(rc, fx, fy, cw, ch);
		const double lines = lumaLines > 0.0 ? lumaLines : static_cast<double>(H);

		// A: crops and the grain split.
		const Plane yCrop = CropPlane(Y, lr);
		const Plane cbCrop = CropPlane(Cb, cr), crCrop = CropPlane(Cr, cr);
		Plane yc = yCrop, ccB = cbCrop, ccR = crCrop, gB(cbCrop.w, cbCrop.h), gR(crCrop.w, crCrop.h);
		if (p.grainGuard)
		{
			yc = GrainSplit::Compute(yCrop, Scale(lines), BlockPx(lines)).clean;
			const double cl = lines / fy;
			const GrainSplit::Split sb = GrainSplit::Compute(cbCrop, Scale(cl), BlockPx(cl)), sr = GrainSplit::Compute(crCrop, Scale(cl), BlockPx(cl));
			ccB = sb.clean; gB = sb.grain;
			ccR = sr.clean; gR = sr.grain;
		}

		// B + C
		const Plane ylo = TentAtSites(yc, cr.x0, cr.y0, cr.W(), cr.H(), fx, fy, shiftX, shiftY, lr.x0, lr.y0);
		double peak = 1.0;
		for (int i = 0; i < colorDepth; ++i) peak *= 2.0;
		const double eps = (p.epsFrac * (peak - 1.0)) * (p.epsFrac * (peak - 1.0));
		const Regression rB = Regress(ylo, ccB, p.radius, eps, p.clampR), rR = Regress(ylo, ccR, p.radius, eps, p.clampR);
		// The prediction at the chroma sites (a Ylo + b): its Catmull-Rom is the part of the prediction the Original kernel
		// already carries, so only the luma-predicted detail beyond it is added to the base (residual form, grain-safe).
		Plane plB(ylo.w, ylo.h), plR(ylo.w, ylo.h);
		for (size_t i = 0; i < ylo.v.size(); ++i)
		{
			plB.v[i] = rB.a.v[i] * ylo.v[i] + rB.b.v[i];
			plR.v[i] = rR.a.v[i] * ylo.v[i] + rR.b.v[i];
		}

		// D + E
		res.cb = Plane(W, H);
		res.cr = Plane(W, H);
		for (int y = 0; y < H; ++y)
			for (int x = 0; x < W; ++x)
			{
				const double uPlane = (x + 0.5 - shiftX) / fx - 0.5, vPlane = (y + 0.5 - shiftY) / fy - 0.5;
				const bool inside = x >= lr.x0 && x < lr.x1 && y >= lr.y0 && y < lr.y1;
				if (!inside)
				{
					int xlo = 0, xhi = cw - 1, ylo2 = 0, yhi = ch - 1;
					if (x < lr.x0) { xlo = 0; xhi = (std::max)(cr.x0 - 1, 0); }
					else if (x >= lr.x1) { xlo = (std::min)(cr.x1, cw - 1); xhi = cw - 1; }
					if (y < lr.y0) { ylo2 = 0; yhi = (std::max)(cr.y0 - 1, 0); }
					else if (y >= lr.y1) { ylo2 = (std::min)(cr.y1, ch - 1); yhi = ch - 1; }
					res.cb.at(x, y) = CatmullRomClamped(Cb, uPlane, vPlane, xlo, xhi, ylo2, yhi);
					res.cr.at(x, y) = CatmullRomClamped(Cr, uPlane, vPlane, xlo, xhi, ylo2, yhi);
					continue;
				}
				const double u = uPlane - cr.x0, v = vPlane - cr.y0;
				const double yv = yc.at(x - lr.x0, y - lr.y0);
				for (int c = 0; c < 2; ++c)
				{
					const Regression& g = c == 0 ? rB : rR;
					const Plane& cc = c == 0 ? ccB : ccR;
					const Plane& gr = c == 0 ? gB : gR;
					const double A = CatmullRomAt(g.a, u, v), Bv = CatmullRomAt(g.b, u, v);
					const double M = GrainSplit::Clamp01(CatmullRomAt(g.rho2, u, v));
					const double base = CatmullRomAt(cc, u, v);
					const double LO = CatmullRomAt(g.lo, u, v), HI = CatmullRomAt(g.hi, u, v);
					const double G = CatmullRomAt(gr, u, v);
					const double PL = CatmullRomAt(c == 0 ? plB : plR, u, v);
					// Only the luma prediction is clamped to the local range: the base is the Original kernel, unclamped, so any
					// grain the split left in the clean chroma is not clipped (CR overshoot of grain is grain).
					const double lo = (std::min)(LO, HI), hi = (std::max)(LO, HI);
					const double pred = (std::max)(lo, (std::min)(hi, A * yv + Bv));
					(c == 0 ? res.cb : res.cr).at(x, y) = base + M * (pred - PL) + G;
				}
			}
		res.status = "ok";
		return res;
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
	constexpr ProductInfo kProduct = { "chroma.cfl", "source.luma, source.chroma, grain.split (luma, chroma)", "GpuCompute", 0,
		"renderer: the source frame before libplacebo (4:4:4 out, shifts 0)" };
}
