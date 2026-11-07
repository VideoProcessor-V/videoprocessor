// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// K7-1 (C3): the output-domain map. P8 (and the model vector it carries) lives in SOURCE-plane pixels of the captured raster; the
// sigma-delta, the debander hold and the overlay work in OUTPUT pixels. This unit maps a source point or vector there:
//
//     source raster px -> active picture (crop: the P11 active picture and any presentation crop) -> normalised 0..1 within it
//       -> non-linear stretch (NLS, on its axis) -> output px (the output scale: where the active picture lands, letterbox included)
//
// and, as information, through the P0 re-base decision (the working plane is the raster scaled per axis by the applied factor):
// the factor cancels in the normalised step, so it changes no output position, but PlanePoint() / PlaneSize() / OutputScale()
// report the intermediate plane, and Result.rebaseFading tells a consumer that the plane is mid-crossfade (its carry should reset).
//
// NLS law: the shipped hook (shaders/NLS.glsl, nls_map) maps an OUTPUT coordinate to the SOURCE sample position; this unit needs the
// inverse (source -> output), found by bisection on the monotone map (fixed iteration count, deterministic). Law parameters are
// the hook's: effective ratio mix(1, ratio, strength) clamped to [1, 1.5], curve [1, 2.9], geometry 0 (power curve) or 1 (centre
// protection, quintic Hermite edge), centre protection [0, 0.45]. The decision logic (when NLS is active, SAFE_FIT, the ratio) is
// the host's; it hands the resulting numbers in Nls. A SAFE_FIT crop is expressed as a smaller `active` box, not here.
//
// Points outside the active picture are flagged (inside = false) and mapped by linear extension with the edge slope, so a vector
// that starts or ends just off the picture stays continuous. A map that is not valid() (empty boxes, non-finite numbers) is the
// identity on the raster and says so; the consumer treats it as "no map" and keeps today's behaviour.
//
// Pure, deterministic, header-only, C++14, no Windows or libplacebo headers. Source coordinates here are raster px with the
// origin at the top-left; the P8 / CameraMotion record is picture-centred, see MapPictureCentred / MapBlockVectors.
#pragma once

#include <VP-V/modules/motion/estimation/global_model/CameraMotion.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace OutputDomainMap
{
	struct Box
	{
		double left = 0, top = 0, right = 0, bottom = 0;
		double Width() const { return right - left; }
		double Height() const { return bottom - top; }
		bool Empty() const { return !(right > left) || !(bottom > top); }
	};

	// The applied P0 re-base decision (scaling/rebase RebaseDecision: per-axis factor, fade weight).
	struct Rebase
	{
		double factorX = 1.0, factorY = 1.0;
		double fadeWeight = 1.0;          // < 1: a factor change is crossfading
		bool Fading() const { return fadeWeight < 1.0; }
	};

	// The NLS hook's numbers for this frame.
	struct Nls
	{
		bool active = false;              // false (OFF / WAITING / LINEAR_PASSTHROUGH): linear
		bool vertical = false;            // the warped axis
		double ratio = 1.0;               // stretch ratio (hook: stretch_ratio, 1..1.5)
		double strength = 1.0;            // hook: strength 0..1
		double curve = 2.0;               // hook: curve 1..2.9
		int geometry = 0;                 // hook: geometry 0 / 1
		double centerProtection = 0.0;    // hook: center_protection 0..0.45
	};

	struct Config
	{
		double rasterW = 0, rasterH = 0;  // captured source raster, px
		Box active;                       // active picture within the raster (crop); empty = the whole raster
		Rebase rebase;
		Nls nls;
		Box output;                       // where the active picture lands, output px (output scale + letterbox); empty = same size as active at the origin

		static Config Identity(double rasterW, double rasterH)
		{
			Config c;
			c.rasterW = rasterW;
			c.rasterH = rasterH;
			return c;
		}
	};

	struct Result
	{
		double x = 0, y = 0;              // output px (a point) / output px displacement (a vector)
		bool inside = true;               // the point (vector: its start and end) lies in the active picture
		bool valid = true;                // the map itself was usable; false = the identity was returned
		bool rebaseFading = false;
	};

	namespace detail
	{
		inline bool Fin(double v) { return CameraModel::IsFiniteBits(v); }
		inline double Clamp(double v, double lo, double hi) { return v < lo ? lo : (v > hi ? hi : v); }

		inline Box ActiveOf(const Config& c)
		{
			if (!c.active.Empty())
				return c.active;
			Box b;
			b.right = c.rasterW;
			b.bottom = c.rasterH;
			return b;
		}

		inline Box OutputOf(const Config& c)
		{
			if (!c.output.Empty())
				return c.output;
			const Box a = ActiveOf(c);
			Box b;
			b.right = a.Width();
			b.bottom = a.Height();
			return b;
		}

		inline bool BoxFinite(const Box& b) { return Fin(b.left) && Fin(b.top) && Fin(b.right) && Fin(b.bottom); }

		inline bool Valid(const Config& c)
		{
			if (!(c.rasterW > 0.0) || !(c.rasterH > 0.0) || !Fin(c.rasterW) || !Fin(c.rasterH))
				return false;
			if (!BoxFinite(c.active) || !BoxFinite(c.output))
				return false;
			const Box a = ActiveOf(c), o = OutputOf(c);
			if (a.Empty() || o.Empty())
				return false;
			if (a.left < -1e-6 || a.top < -1e-6 || a.right > c.rasterW + 1e-6 || a.bottom > c.rasterH + 1e-6)
				return false;
			if (!(c.rebase.factorX > 0.0) || !(c.rebase.factorY > 0.0) || !Fin(c.rebase.factorX) || !Fin(c.rebase.factorY) || !Fin(c.rebase.fadeWeight))
				return false;
			if (c.nls.active && (!Fin(c.nls.ratio) || !Fin(c.nls.strength) || !Fin(c.nls.curve) || !Fin(c.nls.centerProtection)))
				return false;
			return true;
		}

		// The hook's nls_map: OUTPUT coordinate (0..1) -> SOURCE coordinate (0..1).
		inline double NlsForward(double coordinate, double ratio, double curve, int geometry, double centerProtection)
		{
			const double centered = coordinate * 2.0 - 1.0;
			const double radius = std::fabs(centered);
			double mapped;
			if (geometry == 0 || centerProtection <= 0.0)
				mapped = ratio * radius - (ratio - 1.0) * std::pow(radius, curve);
			else if (radius <= centerProtection)
				mapped = radius * ratio;
			else
			{
				const double span = 1.0 - centerProtection;
				const double t = Clamp((radius - centerProtection) / span, 0.0, 1.0);
				const double t2 = t * t, t3 = t2 * t, t4 = t3 * t, t5 = t4 * t;
				const double h00 = 1.0 - 10.0 * t3 + 15.0 * t4 - 6.0 * t5;
				const double h10 = t - 6.0 * t3 + 8.0 * t4 - 3.0 * t5;
				const double h01 = 10.0 * t3 - 15.0 * t4 + 6.0 * t5;
				const double h11 = -4.0 * t3 + 7.0 * t4 - 3.0 * t5;
				const double startPosition = centerProtection * ratio;
				const double startSlope = ratio;
				const double edgeSlope = (std::max)(0.20, 1.0 - 0.5 * (ratio - 1.0) * curve);
				mapped = h00 * startPosition + h10 * span * startSlope + h01 + h11 * span * edgeSlope;
			}
			return (centered < 0.0 ? -mapped : mapped) * 0.5 + 0.5;
		}

		// The NLS law's effective parameters for the axis, or false when it is linear.
		struct NlsLaw
		{
			bool on = false;
			double ratio = 1.0, curve = 2.0, protection = 0.0;
			int geometry = 0;
		};

		inline NlsLaw LawOf(const Nls& n)
		{
			NlsLaw l;
			if (!n.active)
				return l;
			const double active = Clamp(n.ratio, 1.0, 1.5);
			const double strength = Clamp(n.strength, 0.0, 1.0);
			l.ratio = 1.0 + (active - 1.0) * strength;
			l.curve = Clamp(n.curve, 1.0, 2.9);
			l.geometry = n.geometry == 0 ? 0 : 1;
			l.protection = Clamp(n.centerProtection, 0.0, 0.45);
			l.on = l.ratio > 1.0;
			return l;
		}

		// Source -> output, normalised, for u within [0, 1]: the inverse of NlsForward by bisection (48 halvings of [0, 1]).
		inline double NlsInverseInside(double u, const NlsLaw& l)
		{
			double lo = 0.0, hi = 1.0;
			for (int i = 0; i < 48; ++i)
			{
				const double mid = 0.5 * (lo + hi);
				if (NlsForward(mid, l.ratio, l.curve, l.geometry, l.protection) < u)
					lo = mid;
				else
					hi = mid;
			}
			return 0.5 * (lo + hi);
		}

		// Normalised axis map with linear extension (edge slope) outside [0, 1].
		inline double AxisMap(double u, const NlsLaw& l)
		{
			if (!l.on)
				return u;
			constexpr double h = 1.0e-4;
			if (u < 0.0)
				return u * (NlsInverseInside(h, l) / h);
			if (u > 1.0)
				return 1.0 + (u - 1.0) * ((1.0 - NlsInverseInside(1.0 - h, l)) / h);
			return NlsInverseInside(u, l);
		}
	}

	inline bool IsValid(const Config& c) { return detail::Valid(c); }

	// Nothing to do: the map would return its input.
	inline bool IsIdentity(const Config& c)
	{
		if (!detail::Valid(c))
			return true;
		const Box a = detail::ActiveOf(c), o = detail::OutputOf(c);
		return !detail::LawOf(c.nls).on && a.left == o.left && a.top == o.top && a.right == o.right && a.bottom == o.bottom;
	}

	// A source raster point (px, top-left origin) to output px.
	inline Result MapPoint(const Config& c, double x, double y)
	{
		Result r;
		r.x = x;
		r.y = y;
		if (!detail::Valid(c) || !detail::Fin(x) || !detail::Fin(y))
		{
			r.valid = false;   // the input is returned unchanged
			return r;
		}
		const Box a = detail::ActiveOf(c), o = detail::OutputOf(c);
		const detail::NlsLaw law = detail::LawOf(c.nls);
		const double u = (x - a.left) / a.Width(), v = (y - a.top) / a.Height();
		r.inside = x >= a.left && x <= a.right && y >= a.top && y <= a.bottom;
		r.rebaseFading = c.rebase.Fading();
		const double mu = law.on && !c.nls.vertical ? detail::AxisMap(u, law) : u;
		const double mv = law.on && c.nls.vertical ? detail::AxisMap(v, law) : v;
		r.x = o.left + mu * o.Width();
		r.y = o.top + mv * o.Height();
		return r;
	}

	// A source displacement at (x, y) (start point in raster px) as an output displacement: the difference of the mapped end and
	// start points, so the non-linear stretch scales it by the local slope exactly. inside = start and end both inside.
	inline Result MapVector(const Config& c, double x, double y, double vx, double vy)
	{
		const Result s = MapPoint(c, x, y), e = MapPoint(c, x + vx, y + vy);
		Result r;
		r.valid = s.valid;
		r.rebaseFading = s.rebaseFading;
		if (!s.valid)
		{
			r.x = vx;
			r.y = vy;
			return r;
		}
		r.x = e.x - s.x;
		r.y = e.y - s.y;
		r.inside = s.inside && e.inside;
		return r;
	}

	// The record's picture-centred coordinates (CameraMotion.h) to output px; `picture` is the record's active picture.
	inline Result MapPictureCentred(const Config& c, const CameraModel::Rect& picture, double cx, double cy)
	{
		return MapPoint(c, cx + picture.CentreX(), cy + picture.CentreY());
	}

	// The record's model vectors (one per block, P8 modelX / modelY, source px at the block centre) to output-px vectors.
	// outVx / outVy are resized to the grid; a block whose start or end leaves the active picture keeps its (linearly extended) vector and has outInside = 0.
	// Returns false (outputs zero) when the map is not valid.
	inline bool MapBlockVectors(const Config& c, const CameraModel::Rect& picture, int gridW, int gridH, double blockSize,
		const std::vector<float>& modelX, const std::vector<float>& modelY,
		std::vector<float>& outVx, std::vector<float>& outVy, std::vector<uint8_t>& outInside)
	{
		const size_t n = static_cast<size_t>(gridW > 0 ? gridW : 0) * static_cast<size_t>(gridH > 0 ? gridH : 0);
		outVx.assign(n, 0.0f);
		outVy.assign(n, 0.0f);
		outInside.assign(n, 0);
		if (!detail::Valid(c) || modelX.size() < n || modelY.size() < n)
			return false;
		for (int by = 0; by < gridH; ++by)
			for (int bx = 0; bx < gridW; ++bx)
			{
				const size_t i = static_cast<size_t>(by) * static_cast<size_t>(gridW) + static_cast<size_t>(bx);
				double cx = 0, cy = 0;
				CameraModel::BlockCentre(bx, by, blockSize, picture, &cx, &cy);
				const Result r = MapVector(c, cx + picture.CentreX(), cy + picture.CentreY(), modelX[i], modelY[i]);
				outInside[i] = r.inside ? 1 : 0;   // the vector is kept (linear extension) even when 0: a consumer decides
				outVx[i] = static_cast<float>(r.x);
				outVy[i] = static_cast<float>(r.y);
			}
		return true;
	}

	// ---- the re-based working plane (information) -----------------------------------------------------------------------------

	// A raster point in working-plane px (the raster scaled by the applied per-axis re-base factor).
	inline Result PlanePoint(const Config& c, double x, double y)
	{
		Result r;
		r.x = x * c.rebase.factorX;
		r.y = y * c.rebase.factorY;
		r.valid = detail::Valid(c);
		r.rebaseFading = c.rebase.Fading();
		return r;
	}

	// The active picture's size in the working plane.
	inline void PlaneSize(const Config& c, double& w, double& h)
	{
		const Box a = detail::ActiveOf(c);
		w = a.Width() * c.rebase.factorX;
		h = a.Height() * c.rebase.factorY;
	}

	// The scale the output scaler applies to the working plane (output size / plane size, per axis).
	inline void OutputScale(const Config& c, double& sx, double& sy)
	{
		double pw = 0, ph = 0;
		PlaneSize(c, pw, ph);
		const Box o = detail::OutputOf(c);
		sx = pw > 0.0 ? o.Width() / pw : 0.0;
		sy = ph > 0.0 ? o.Height() / ph : 0.0;
	}
}
