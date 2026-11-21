// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// colour.appearance: CPU reference CAM16 (CIE 2016 colour appearance model, Li et al. 2017).
// This is the reference the appearance shader (CAM-3) is checked against; nothing here runs in the
// picture path and the switch that would use it defaults OFF (CAM-4).
//
// Scale: XYZ is relative to the white of the conditions it is measured under, Y of that white
// = 100 (the CIE convention). The absolute level enters only through the adapting luminance La
// (cd/m2), which is how the reference (large Lw_ref) and screen (own Lw_screen) conditions differ
// (the Stevens and Hunt effects come out of FL and D). Linear RGB -> XYZ is done by the caller's primaries.
//
// General, not fixed: every number is a parameter (white point, La, Yb, surround F/c/Nc); the CIE
// surround constants below are the published ones and the surround is interpolated by the caller.

#include <algorithm>
#include <array>
#include <cmath>

namespace Cam16
{
	using Vec3 = std::array<double, 3>;

	constexpr double kPi = 3.14159265358979323846;

	// CIE 2016 surround constants (dark / dim / average).
	struct Surround
	{
		double F;
		double c;
		double Nc;
	};
	constexpr Surround kSurroundDark = { 0.8, 0.525, 0.8 };
	constexpr Surround kSurroundDim = { 0.9, 0.59, 0.9 };
	constexpr Surround kSurroundAverage = { 1.0, 0.69, 1.0 };

	// The viewing conditions of one side (reference or screen).
	struct Conditions
	{
		Vec3 whiteXyz = { { 95.047, 100.0, 108.883 } }; // D65, Y = 100
		double adaptingNits = 20.0;                     // La (0.2 x the white's luminance in the usual case)
		double backgroundY = 20.0;                      // Yb, relative to Yw = 100
		Surround surround = kSurroundAverage;
		double discount = -1.0;                         // < 0: computed from F and La; else the explicit D (0..1)
	};

	// Everything derived from the conditions once (per settings / room step, never per pixel).
	struct Prepared
	{
		Vec3 dc = { { 1, 1, 1 } }; // D Yw / Rw + 1 - D, per CAT16 channel
		double fl = 1.0;
		double n = 0.2;
		double z = 1.9;
		double nbb = 1.0;          // = Ncb
		double aw = 1.0;
		double c = 0.69;
		double nc = 1.0;
	};

	struct Appearance
	{
		double J = 0, C = 0, M = 0, h = 0; // h in degrees [0, 360)
	};

	inline Vec3 Mul3(const double m[3][3], const Vec3& v)
	{
		return { { m[0][0] * v[0] + m[0][1] * v[1] + m[0][2] * v[2],
			m[1][0] * v[0] + m[1][1] * v[1] + m[1][2] * v[2],
			m[2][0] * v[0] + m[2][1] * v[1] + m[2][2] * v[2] } };
	}

	// CAT16 matrix and its inverse (published CIE 2016 values).
	inline Vec3 XyzToM16(const Vec3& xyz)
	{
		static const double m[3][3] = { { 0.401288, 0.650173, -0.051461 }, { -0.250268, 1.204414, 0.045854 }, { -0.002079, 0.048952, 0.953127 } };
		return Mul3(m, xyz);
	}
	inline Vec3 M16ToXyz(const Vec3& rgb)
	{
		static const double m[3][3] = { { 1.862067855087233, -1.011254630531685, 0.149186775444452 },
			{ 0.387526542878237, 0.621447441346826, -0.008973984225064 }, { -0.015841498849333, -0.034122938028909, 1.049964436878243 } };
		return Mul3(m, rgb);
	}

	// Post-adaptation compression and its inverse (Ra' = 400 sgn(x) p / (27.13 + p) + 0.1, p = (FL |x| / 100)^0.42).
	inline double Compress(double fl, double x)
	{
		const double p = std::pow(fl * std::fabs(x) / 100.0, 0.42);
		const double v = 400.0 * p / (27.13 + p);
		return (x < 0.0 ? -v : v) + 0.1;
	}
	inline double Uncompress(double fl, double ra)
	{
		const double d = ra - 0.1;
		const double ad = std::fabs(d);
		const double den = 400.0 - ad;
		const double p = std::pow(27.13 * ad / (den > 1e-12 ? den : 1e-12), 1.0 / 0.42);
		const double v = 100.0 / fl * p;
		return d < 0.0 ? -v : v;
	}

	inline Prepared Prepare(const Conditions& cond)
	{
		Prepared p;
		const double la = std::max(cond.adaptingNits, 1e-6);
		double d = cond.discount;
		if (d < 0.0)
			d = cond.surround.F * (1.0 - (1.0 / 3.6) * std::exp((-la - 42.0) / 92.0));
		d = std::min(1.0, std::max(0.0, d));
		const Vec3 rw = XyzToM16(cond.whiteXyz);
		for (int i = 0; i < 3; ++i)
			p.dc[i] = d * cond.whiteXyz[1] / rw[i] + 1.0 - d;
		const double k = 1.0 / (5.0 * la + 1.0);
		const double k4 = k * k * k * k;
		p.fl = 0.2 * k4 * (5.0 * la) + 0.1 * (1.0 - k4) * (1.0 - k4) * std::cbrt(5.0 * la);
		p.n = cond.backgroundY / cond.whiteXyz[1];
		p.z = 1.48 + std::sqrt(p.n);
		p.nbb = 0.725 * std::pow(p.n, -0.2);
		p.c = cond.surround.c;
		p.nc = cond.surround.Nc;
		p.aw = (2.0 * Compress(p.fl, p.dc[0] * rw[0]) + Compress(p.fl, p.dc[1] * rw[1]) + 0.05 * Compress(p.fl, p.dc[2] * rw[2]) - 0.305) * p.nbb;
		return p;
	}

	inline double Eccentricity(double hRad) { return 0.25 * (std::cos(hRad + 2.0) + 3.8); }

	inline Appearance Forward(const Prepared& p, const Vec3& xyz)
	{
		const Vec3 r = XyzToM16(xyz);
		const double ra = Compress(p.fl, p.dc[0] * r[0]);
		const double ga = Compress(p.fl, p.dc[1] * r[1]);
		const double ba = Compress(p.fl, p.dc[2] * r[2]);
		const double a = ra - 12.0 * ga / 11.0 + ba / 11.0;
		const double b = (ra + ga - 2.0 * ba) / 9.0;
		const double aa = (2.0 * ra + ga + 0.05 * ba - 0.305) * p.nbb;
		Appearance o;
		double hDeg = std::atan2(b, a) * 180.0 / kPi;
		if (hDeg < 0.0)
			hDeg += 360.0;
		o.h = hDeg;
		const double ratio = std::max(aa / p.aw, 0.0);
		o.J = 100.0 * std::pow(ratio, p.c * p.z);
		const double denom = ra + ga + 1.05 * ba;
		const double et = Eccentricity(hDeg * kPi / 180.0);
		const double t = std::fabs(denom) > 1e-12 ? (50000.0 / 13.0 * p.nc * p.nbb * et * std::sqrt(a * a + b * b)) / denom : 0.0;
		o.C = std::pow(std::max(t, 0.0), 0.9) * std::sqrt(o.J / 100.0) * std::pow(1.64 - std::pow(0.29, p.n), 0.73);
		o.M = o.C * std::pow(p.fl, 0.25);
		return o;
	}

	// Inverse from (J, M, h); M (not C) so a soft M compression can sit between the two halves.
	inline Vec3 Inverse(const Prepared& p, double J, double M, double hDeg)
	{
		const double C = M / std::pow(p.fl, 0.25);
		const double jr = std::max(J, 0.0) / 100.0;
		const double k = std::sqrt(jr) * std::pow(1.64 - std::pow(0.29, p.n), 0.73);
		const double t = (k > 1e-12 && C > 0.0) ? std::pow(C / k, 1.0 / 0.9) : 0.0;
		const double hr = hDeg * kPi / 180.0;
		const double et = Eccentricity(hr);
		const double aa = p.aw * std::pow(jr, 1.0 / (p.c * p.z));
		const double p1 = (t > 1e-12) ? (50000.0 / 13.0 * p.nc * p.nbb * et) / t : 0.0;
		const double p2 = aa / p.nbb + 0.305;
		const double p3 = 21.0 / 20.0;
		const double sn = std::sin(hr), cs = std::cos(hr);
		double a = 0.0, b = 0.0;
		if (t > 1e-12)
		{
			if (std::fabs(sn) >= std::fabs(cs))
			{
				const double p4 = p1 / sn;
				b = p2 * (2.0 + p3) * (460.0 / 1403.0) /
					(p4 + (2.0 + p3) * (220.0 / 1403.0) * (cs / sn) - 27.0 / 1403.0 + p3 * (6300.0 / 1403.0));
				a = b * (cs / sn);
			}
			else
			{
				const double p5 = p1 / cs;
				a = p2 * (2.0 + p3) * (460.0 / 1403.0) /
					(p5 + (2.0 + p3) * (220.0 / 1403.0) - (27.0 / 1403.0 - p3 * (6300.0 / 1403.0)) * (sn / cs));
				b = a * (sn / cs);
			}
		}
		const double ra = (460.0 * p2 + 451.0 * a + 288.0 * b) / 1403.0;
		const double ga = (460.0 * p2 - 891.0 * a - 261.0 * b) / 1403.0;
		const double ba = (460.0 * p2 - 220.0 * a - 6300.0 * b) / 1403.0;
		const Vec3 rgb = { { Uncompress(p.fl, ra) / p.dc[0], Uncompress(p.fl, ga) / p.dc[1], Uncompress(p.fl, ba) / p.dc[2] } };
		return M16ToXyz(rgb);
	}

	// Trim/Hunt cap (doc 115 B13): a DV trim already applied a saturation gain T; the model's own M gain H
	// is reduced so that T x H never exceeds the larger of the two alone. A gain <= 1 is never capped
	// (a product with a gain <= 1 cannot pass the larger factor).
	inline double CappedHuntGain(double trimSatGain, double huntGain)
	{
		if (!(trimSatGain > 0.0))
			return huntGain;
		return std::min(huntGain, std::max(trimSatGain, huntGain) / trimSatGain);
	}
}
