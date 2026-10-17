#pragma once

// CPU reference models of the film-grain stages (owner 2026-10-17: film grain is
// never averaged or interpolated away; docs/audit/grain-audit-2026-10-17.md).
// Each mirrors its shader (FrameInterpolator.cpp, DejudderWarp.cpp,
// AdaptiveDownscale.cpp, VpDebander.cpp) in double precision on one plane, with
// both values of its switch, so the maths can be checked on the CPU; the gated
// GPU tests check the shaders themselves. Positions in pixels, texel centres at
// +0.5, edges clamped, as in the shaders.

#include <vprenderer/GrainPreserve.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <random>
#include <vector>

namespace GrainModel
{
	struct Image
	{
		int w = 0, h = 0;
		std::vector<double> v;
		Image() = default;
		Image(int width, int height, double fill = 0.0) : w(width), h(height), v(static_cast<size_t>(width) * height, fill) {}
		double& at(int x, int y) { return v[static_cast<size_t>(y) * w + x]; }
		double at(int x, int y) const { return v[static_cast<size_t>(y) * w + x]; }
		double get(int x, int y) const
		{
			return at((std::max)(0, (std::min)(w - 1, x)), (std::max)(0, (std::min)(h - 1, y)));
		}
	};

	inline Image operator-(const Image& a, const Image& b)
	{
		Image o(a.w, a.h);
		for (size_t i = 0; i < o.v.size(); ++i) o.v[i] = a.v[i] - b.v[i];
		return o;
	}

	inline Image operator+(const Image& a, const Image& b)
	{
		Image o(a.w, a.h);
		for (size_t i = 0; i < o.v.size(); ++i) o.v[i] = a.v[i] + b.v[i];
		return o;
	}

	// The shaders' DEFINE_BILERP (FrameInterpolator) / hardware bilinear.
	inline double Bilinear(const Image& img, double px, double py)
	{
		const double gx = px - 0.5, gy = py - 0.5;
		const int ix = static_cast<int>(std::floor(gx)), iy = static_cast<int>(std::floor(gy));
		const double fx = gx - ix, fy = gy - iy;
		const double top = img.get(ix, iy) * (1.0 - fx) + img.get(ix + 1, iy) * fx;
		const double bottom = img.get(ix, iy + 1) * (1.0 - fx) + img.get(ix + 1, iy + 1) * fx;
		return top * (1.0 - fy) + bottom * fy;
	}

	inline void CatmullWeights(double t, double w[4])
	{
		w[0] = t * (-0.5 + t * (1.0 - 0.5 * t));
		w[1] = 1.0 + t * t * (-2.5 + 1.5 * t);
		w[2] = t * (0.5 + t * (2.0 - 1.5 * t));
		w[3] = t * t * (-0.5 + 0.5 * t);
	}

	// GrainPreserve::CatmullGlsl's DEFINE_CATMULL.
	inline double Catmull(const Image& img, double px, double py)
	{
		const double gx = px - 0.5, gy = py - 0.5;
		const int ix = static_cast<int>(std::floor(gx)), iy = static_cast<int>(std::floor(gy));
		double wx[4], wy[4];
		CatmullWeights(gx - ix, wx);
		CatmullWeights(gy - iy, wy);
		double acc = 0.0;
		for (int j = 0; j < 4; ++j)
		{
			double row = 0.0;
			for (int i = 0; i < 4; ++i)
				row += wx[i] * img.get(ix + i - 1, iy + j - 1);
			acc += wy[j] * row;
		}
		return acc;
	}

	// The low band at half resolution (GrainPreserve; LOW_HALF_BODY, LOW_H_BODY,
	// LOW_V_BODY): a 2x2 box average, then the separable Gaussian.
	inline Image Low(const Image& img)
	{
		const int hw = (img.w + 1) / 2, hh = (img.h + 1) / 2;
		Image half(hw, hh);
		for (int y = 0; y < hh; ++y)
			for (int x = 0; x < hw; ++x)
				half.at(x, y) = 0.25 * (img.get(2 * x, 2 * y) + img.get(2 * x + 1, 2 * y) + img.get(2 * x, 2 * y + 1) + img.get(2 * x + 1, 2 * y + 1));
		const int r = GrainPreserve::kLowRadius;
		std::vector<double> w(2 * r + 1);
		for (int i = -r; i <= r; ++i) w[i + r] = GrainPreserve::LowWeight(i);
		Image across(hw, hh), out(hw, hh);
		for (int y = 0; y < hh; ++y)
			for (int x = 0; x < hw; ++x)
			{
				double acc = 0.0;
				for (int i = -r; i <= r; ++i) acc += w[i + r] * half.get(x + i, y);
				across.at(x, y) = acc;
			}
		for (int y = 0; y < hh; ++y)
			for (int x = 0; x < hw; ++x)
			{
				double acc = 0.0;
				for (int i = -r; i <= r; ++i) acc += w[i + r] * across.get(x, y + i);
				out.at(x, y) = acc;
			}
		return out;
	}

	// The low band at p (full-resolution pixels): the half-resolution band, bilinear at p / 2.
	inline double LowAt(const Image& lowHalf, double px, double py) { return Bilinear(lowHalf, px * 0.5, py * 0.5); }

	struct Motion
	{
		double vx = 0.0, vy = 0.0;   // A -> B, pixels per source frame (one trusted vector everywhere)
	};

	// interpolation_grain blend (INTERPOLATE_BODY, trusted field, no cut): the
	// bidirectional blend; occludedB(lbx, lby) true = wb 0 (the A sample alone).
	inline Image InterpolateBlend(const Image& a, const Image& b, double t, Motion v,
		const std::function<bool(double, double)>& occludedB = nullptr)
	{
		Image out(a.w, a.h);
		for (int y = 0; y < a.h; ++y)
			for (int x = 0; x < a.w; ++x)
			{
				const double px = x + 0.5, py = y + 0.5;
				const double lax = px - t * v.vx, lay = py - t * v.vy, lbx = px + (1.0 - t) * v.vx, lby = py + (1.0 - t) * v.vy;
				const double wb = occludedB && occludedB(lbx, lby) ? 0.0 : t;
				out.at(x, y) = Bilinear(a, lax, lay) * (1.0 - wb) + Bilinear(b, lbx, lby) * wb;
			}
		return out;
	}

	// interpolation_grain preserve (PRESERVE_BODY): the low bands mixed by phase,
	// the high band of the frame the mix leans on (Catmull-Rom minus its low band),
	// one gain per 16x16 output block matching the source's own high-band energy at
	// the texel nearest each sample plus the grain the low-band blend loses
	// (wb (1 - wb) (la - lb)^2, at most 4x the pixel's own), clamped to [1, 2].
	inline Image InterpolatePreserve(const Image& a, const Image& b, double t, Motion v,
		const std::function<bool(double, double)>& occludedB = nullptr, Image* gainOut = nullptr)
	{
		const Image lowA = Low(a), lowB = Low(b);
		Image lo(a.w, a.h), hf(a.w, a.h), tg(a.w, a.h);
		for (int y = 0; y < a.h; ++y)
			for (int x = 0; x < a.w; ++x)
			{
				const double px = x + 0.5, py = y + 0.5;
				const double lax = px - t * v.vx, lay = py - t * v.vy, lbx = px + (1.0 - t) * v.vx, lby = py + (1.0 - t) * v.vy;
				const double wb = occludedB && occludedB(lbx, lby) ? 0.0 : t;
				const double la = LowAt(lowA, lax, lay), lb = LowAt(lowB, lbx, lby);
				lo.at(x, y) = la * (1.0 - wb) + lb * wb;
				const bool useB = wb >= 0.5;
				const Image& s = useB ? b : a;
				const Image& l = useB ? lowB : lowA;
				const double sx = useB ? lbx : lax, sy = useB ? lby : lay;
				hf.at(x, y) = Catmull(s, sx, sy) - (useB ? lb : la);
				const int nx = static_cast<int>(std::floor(sx)), ny = static_cast<int>(std::floor(sy));
				const double d = s.get(nx, ny) - LowAt(l, nx + 0.5, ny + 0.5);
				const double dl = la - lb;
				tg.at(x, y) = d * d + wb * (1.0 - wb) * (std::min)(dl * dl, 4.0 * d * d);
			}
		Image out(a.w, a.h);
		if (gainOut) *gainOut = Image(a.w, a.h, 1.0);
		for (int by = 0; by < a.h; by += 16)
			for (int bx = 0; bx < a.w; bx += 16)
			{
				double sh = 0.0, st = 0.0;
				int n = 0;
				for (int y = by; y < (std::min)(a.h, by + 16); ++y)
					for (int x = bx; x < (std::min)(a.w, bx + 16); ++x)
					{
						sh += hf.at(x, y) * hf.at(x, y);
						st += tg.at(x, y);
						++n;
					}
				double g = 1.0;
				if (n >= GrainPreserve::kGainMinPixels && sh > 1e-12)
					g = (std::max)(GrainPreserve::kGainMin, (std::min)(GrainPreserve::kGainMax, std::sqrt(st / sh)));
				for (int y = by; y < (std::min)(a.h, by + 16); ++y)
					for (int x = bx; x < (std::min)(a.w, bx + 16); ++x)
					{
						out.at(x, y) = lo.at(x, y) + g * hf.at(x, y);
						if (gainOut) gainOut->at(x, y) = g;
					}
			}
		return out;
	}

	// The de-judder's source warp: A at p - (dx, dy); bilinear (the hardware's) or Catmull-Rom.
	inline Image Warp(const Image& a, double dx, double dy, bool catmull)
	{
		Image out(a.w, a.h);
		for (int y = 0; y < a.h; ++y)
			for (int x = 0; x < a.w; ++x)
				out.at(x, y) = catmull ? Catmull(a, x + 0.5 - dx, y + 0.5 - dy) : Bilinear(a, x + 0.5 - dx, y + 0.5 - dy);
		return out;
	}

	// The CNN / SSIM residual stabilisation (STABILISE_AND_STORE) on the residual
	// plane: r = this frame's residual, prev = the previous frame's held residual,
	// v = the motion since it (trusted), alpha 1/3. Legacy: the previous residual
	// bilinear at p - v. Guard: by texel; grainy (0..1, from the grain term g and its
	// floor) passes r through; the held change limited to noiseLimit (0 = none).
	inline Image StabiliseLegacy(const Image& r, const Image& prev, Motion v)
	{
		Image out(r.w, r.h);
		for (int y = 0; y < r.h; ++y)
			for (int x = 0; x < r.w; ++x)
			{
				const double rp = Bilinear(prev, x + 0.5 - v.vx, y + 0.5 - v.vy);
				out.at(x, y) = rp + (r.at(x, y) - rp) * (1.0 / 3.0);
			}
		return out;
	}

	inline double Smoothstep(double e0, double e1, double x) { return GrainPreserve::Smoothstep(e0, e1, x); }

	inline Image StabiliseGuard(const Image& r, const Image& prev, Motion v, double grainTerm, double grainFloor, double noiseLimit)
	{
		const double grainy = (std::max)(0.0, (std::min)(1.0, (1.0 - grainTerm) / (std::max)(1.0 - grainFloor, 1e-3)));
		const double pass = Smoothstep(0.25, 0.75, grainy);
		Image out(r.w, r.h);
		for (int y = 0; y < r.h; ++y)
			for (int x = 0; x < r.w; ++x)
			{
				const double qx = x + 0.5 - v.vx, qy = y + 0.5 - v.vy;
				const double rp = prev.get(static_cast<int>(std::floor(qx)), static_cast<int>(std::floor(qy)));
				const double cur = r.at(x, y);
				double rs = rp + (cur - rp) * (1.0 / 3.0);
				rs = rs + (cur - rs) * pass;
				if (noiseLimit > 0.0)
					rs = cur + (std::max)(-noiseLimit, (std::min)(noiseLimit, rs - cur));
				out.at(x, y) = rs;
			}
		return out;
	}

	// GUARD_BODY's busy test at (x, y): the Laplacian above lapMin, or a strict local
	// extremum along x, y or a diagonal by more than extMin on both sides.
	inline bool GuardBusy(const Image& img, int x, int y, double lapMin, double extMin)
	{
		const double c = img.at(x, y);
		const double dw = c - img.get(x - 1, y), de = c - img.get(x + 1, y);
		const double dn = c - img.get(x, y - 1), ds = c - img.get(x, y + 1);
		if (std::fabs(dw + de + dn + ds) > lapMin)
			return true;
		const double d[4][2] = { { dw, de }, { dn, ds },
			{ c - img.get(x - 1, y - 1), c - img.get(x + 1, y + 1) }, { c - img.get(x + 1, y - 1), c - img.get(x - 1, y + 1) } };
		for (const auto& line : d)
			if ((std::max)((std::min)(line[0], line[1]), -(std::max)(line[0], line[1])) > extMin)
				return true;
		return false;
	}

	// VpDebander per frame (no vectors: BODY without HOLD / GRAIN), with the spatial
	// grain guard (GUARD_BODY + SPATIAL_GUARD) when lapMin > 0 (levels from
	// GrainPreserve::GuardLaplacianCodes / GuardExtremumCodes). Values in codes.
	inline Image Deband(const Image& img, double thr, double radius, double lapMin, double extMin)
	{
		Image share;
		if (lapMin > 0.0)
		{
			const int bw = (img.w + 15) / 16, bh = (img.h + 15) / 16;
			share = Image(bw, bh);
			for (int by = 0; by < bh; ++by)
				for (int bx = 0; bx < bw; ++bx)
				{
					int hit = 0, n = 0;
					for (int y = by * 16; y < (std::min)(img.h, by * 16 + 16); ++y)
						for (int x = bx * 16; x < (std::min)(img.w, bx * 16 + 16); ++x)
						{
							hit += GuardBusy(img, x, y, lapMin, extMin) ? 1 : 0;
							++n;
						}
					share.at(bx, by) = static_cast<double>(hit) / (std::max)(n, 1);
				}
		}
		Image out(img.w, img.h);
		for (int y = 0; y < img.h; ++y)
			for (int x = 0; x < img.w; ++x)
			{
				const double px = x + 0.5, py = y + 0.5;
				uint32_t h = static_cast<uint32_t>(x) * 1973u + static_cast<uint32_t>(y) * 9277u + 26699u;
				h = (h ^ (h >> 13u)) * 1540483477u;
				h ^= h >> 15u;
				const double r1 = static_cast<double>(h & 65535u) / 65535.0;
				const double r2 = static_cast<double>(h >> 16u) / 65535.0;
				const double d = 1.0 + (radius - 1.0) * r1;
				const double ox = std::cos(r2 * 6.2831853) * d, oy = std::sin(r2 * 6.2831853) * d;
				const double c = img.at(x, y);
				const double avg = 0.25 * (Bilinear(img, px + ox, py + oy) + Bilinear(img, px - ox, py - oy) +
					Bilinear(img, px - oy, py + ox) + Bilinear(img, px + oy, py - ox));
				double w = 1.0 - Smoothstep(0.6 * thr, thr, std::fabs(c - avg));
				if (lapMin > 0.0)
					w *= 1.0 - Smoothstep(GrainPreserve::kVetoLo, GrainPreserve::kVetoHi, Bilinear(share, px / 16.0, py / 16.0));
				out.at(x, y) = c * (1.0 - w) + avg * w;
			}
		return out;
	}

	// Fresh grain: N(0, sigma) per pixel; soft = blurred (Gaussian 0.7 px) and
	// renormalised to sigma (film grain is softer than white).
	inline Image Grain(int w, int h, double sigma, bool soft, std::mt19937& rng)
	{
		std::normal_distribution<double> n(0.0, 1.0);
		Image g(w, h);
		for (double& x : g.v) x = n(rng);
		if (soft)
		{
			const double s = 0.7;
			const int r = 3;
			std::vector<double> k(2 * r + 1);
			double sum = 0.0;
			for (int i = -r; i <= r; ++i) sum += k[i + r] = std::exp(-0.5 * i * i / (s * s));
			for (double& x : k) x /= sum;
			Image t(w, h), o(w, h);
			for (int y = 0; y < h; ++y)
				for (int x = 0; x < w; ++x)
				{
					double acc = 0.0;
					for (int i = -r; i <= r; ++i) acc += k[i + r] * g.at((x + i + w) % w, y);
					t.at(x, y) = acc;
				}
			for (int y = 0; y < h; ++y)
				for (int x = 0; x < w; ++x)
				{
					double acc = 0.0;
					for (int i = -r; i <= r; ++i) acc += k[i + r] * t.at(x, (y + i + h) % h);
					o.at(x, y) = acc;
				}
			g = o;
		}
		double mean = 0.0, sq = 0.0;
		for (double x : g.v) mean += x;
		mean /= g.v.size();
		for (double x : g.v) sq += (x - mean) * (x - mean);
		const double scale = sigma / std::sqrt(sq / g.v.size());
		for (double& x : g.v) x = (x - mean) * scale;
		return g;
	}

	// RMS (about the mean) and correlation over the picture less a margin.
	inline double Rms(const Image& a, int margin)
	{
		double mean = 0.0, sq = 0.0;
		int n = 0;
		for (int y = margin; y < a.h - margin; ++y)
			for (int x = margin; x < a.w - margin; ++x) { mean += a.at(x, y); ++n; }
		mean /= n;
		for (int y = margin; y < a.h - margin; ++y)
			for (int x = margin; x < a.w - margin; ++x) sq += (a.at(x, y) - mean) * (a.at(x, y) - mean);
		return std::sqrt(sq / n);
	}

	inline double Corr(const Image& a, const Image& b, int margin)
	{
		double ma = 0.0, mb = 0.0;
		int n = 0;
		for (int y = margin; y < a.h - margin; ++y)
			for (int x = margin; x < a.w - margin; ++x) { ma += a.at(x, y); mb += b.at(x, y); ++n; }
		ma /= n;
		mb /= n;
		double sab = 0.0, saa = 0.0, sbb = 0.0;
		for (int y = margin; y < a.h - margin; ++y)
			for (int x = margin; x < a.w - margin; ++x)
			{
				const double da = a.at(x, y) - ma, db = b.at(x, y) - mb;
				sab += da * db;
				saa += da * da;
				sbb += db * db;
			}
		return sab / std::sqrt((std::max)(saa * sbb, 1e-300));
	}
}
