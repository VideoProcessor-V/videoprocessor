#pragma once
/*
 * Copyright(C) 2026 Victor McPherson
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
// "Dolby-style" tone mapping for Dolby Vision content (feat/dv-l8 task 5, 2026-10-24), behind
// the renderer key dv_tone_mapping (libplacebo | dolby_style; default libplacebo until the LLDV
// pair comparison decides). Header-only and pure (C++14) apart from the libplacebo function
// table at the end; a separable unit of the future display-adaptation module (CAM16 will sit
// after it for the ambient handling only; ReferenceConditions below is its input).
//
// APPROXIMATION, NOT CERTIFIED, NOT Dolby display management. It follows the PUBLISHED
// STRUCTURE of Dolby's display management (CM v2.9 / v4.0), from public sources only (each is
// listed in src/VP-V/modules/colour/sdr_tone_shape/README.md):
//   1. the picture in an opponent PQ space (ICtCp, ITU-R BT.2100; IPT-PQ for CM v2.9);
//   2. the INTENSITY tone-mapped with a per-scene curve built from L1 (scene min / avg / max,
//      PQ) and the target display (min / max, from the user's tone-mapping target): the
//      three-anchor rational curve y = (c1 + c2 x) / (1 + c3 x) of SMPTE ST 2094-10 Annex B
//      (the public form of Dolby's CM v2.9 curve), evaluated in PQ; the scene mid (L1 avg) is
//      kept where the target allows it (kMidAdaptation = 1: the source mid itself), never above
//      kMidCeiling of the target range;
//   3. chroma handled separately from the intensity (libplacebo applies the curve in its IPT /
//      cone stage with chromaticity-preserving cone ratios; its gamut mapping compresses
//      what leaves the target gamut);
//   4. the L2 / L8 trims IN ICtCp (L8 preferred, DvTrimSelection): slope / offset / power and
//      the L8 mid-contrast / clip bends on PQ intensity relative to the target peak; chroma
//      weight, saturation gain and the L8 per-hue vectors on (Ct, Cp).
// No expansion: a scene that already fits the target (source range inside the target range)
// is passed unchanged (identity at matching source / target).
//
// ROUTE: the curve is a custom libplacebo tone-mapping function (Function(), PL_HDR_PQ), set
// for Dolby Vision pictures only; libplacebo's L1-derived scene values (input min / avg / max)
// and the target (output min / max) arrive in its parameters, so its LUT cache follows scene
// changes. The ICtCp trims run in shaders/DV trims.glsl (dv_space = 1). When the mode cannot
// run (a calibration LUT, an output transfer the trims stage does not support, the slot off,
// the trims shader missing), the renderer lists "DV tone mapping" under "Fallbacks engaged" and
// uses bt2390 (+ the luminance trims when dv_trims is on). libplacebo's input_avg / input_max
// come from the picture's hdr.avg_pq_y / max_pq_y (L1); when they are absent the renderer says
// "L1 missing -> mid-range fallback" in the status (the curve then uses the range's middle). BTB / WTW: below-black residuals are carried
// around the trims and the curve continues above the scene peak with its end slope.
#include <VP-V/modules/colour/sdr_tone_shape/DvTrimSelection.h>

#include <cmath>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

namespace DvDolbyStyle
{
	// How far the target mid moves from the proportional (chord) position toward the source mid.
	// 1 = the scene mid kept where the target allows it (below kMidCeiling), as Dolby's display
	// management keeps the scene's mid-tones. GPU check 2026-10-24: 0.7 rendered the synthetic
	// policy scenes about one stop darker than the L1 intent model (luminance ratio ~0.5 even on
	// the dim scene, where bt2390 gave 0.998). To be tuned against the LLDV pairs.
	constexpr double kMidAdaptation = 1.0;
	constexpr double kMidCeiling = 0.85;        // the target mid stays below this share of the target range
	constexpr double kMidMinShare = 0.1;        // the source mid is kept inside [10 %, 90 %] of the scene range
	constexpr double kMidMaxShare = 0.9;
	constexpr double kDefaultMidShare = 0.5;    // no L1 average: the scene range's middle (PQ)
	constexpr double kBelowTolerance = 1e-4;    // ICtCp trims: round-trip rounding allowed below black (share of the light)

	// ------------------------------------------------------------------ the curve (PQ 0..1)

	struct Curve
	{
		bool identity = true;
		double sMin = 0.0, sMid = 0.5, sMax = 1.0;   // source anchors (PQ)
		double tMin = 0.0, tMid = 0.5, tMax = 1.0;   // target anchors (PQ)
		double c1 = 0.0, c2 = 1.0, c3 = 0.0;         // y = (c1 + c2 x) / (1 + c3 x) on [sMin, sMax]

		double Core(double x) const { return (c1 + c2 * x) / (1.0 + c3 * x); }
		double Slope(double x) const { const double d = 1.0 + c3 * x; return (c2 - c1 * c3) / (d * d); }

		double Map(double x) const
		{
			if (identity)
				return x;
			if (x < sMin)
				return sMin > 0.0 ? tMin * (x / sMin) : tMin + (x - sMin) * Slope(sMin);   // to black through 0
			if (x > sMax)
				return tMax + (x - sMax) * Slope(sMax);   // above the scene peak: the end slope (WTW ordered)
			return Core(x);
		}
	};

	// The rational through three points (ST 2094-10 Annex B form). False when degenerate.
	inline bool Solve(double x1, double y1, double x2, double y2, double x3, double y3, double& c1, double& c2, double& c3)
	{
		// (c1 + c2 x) = y (1 + c3 x)  ->  c1 + c2 x - c3 x y = y, three equations (Cramer).
		const double a[3][3] = { { 1.0, x1, -x1 * y1 }, { 1.0, x2, -x2 * y2 }, { 1.0, x3, -x3 * y3 } };
		const double b[3] = { y1, y2, y3 };
		auto det3 = [](const double m[3][3]) {
			return m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) - m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) +
				m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
		};
		const double d = det3(a);
		if (!(std::fabs(d) > 1e-15))
			return false;
		double out[3];
		for (int k = 0; k < 3; ++k)
		{
			double m[3][3];
			for (int r = 0; r < 3; ++r)
				for (int c = 0; c < 3; ++c)
					m[r][c] = c == k ? b[r] : a[r][c];
			out[k] = det3(m) / d;
		}
		c1 = out[0]; c2 = out[1]; c3 = out[2];
		return true;
	}

	// The scene curve. sAvg <= 0: unknown (the middle of the scene range). All values PQ 0..1.
	inline Curve Build(double sMin, double sAvg, double sMax, double tMin, double tMax)
	{
		Curve c;
		if (!(sMax > sMin) || !(tMax > tMin))
			return c;   // nothing usable: identity
		c.sMin = sMin; c.sMax = sMax; c.tMin = tMin; c.tMax = tMax;
		if (sMax <= tMax + 1e-6 && sMin >= tMin - 1e-5)   // PQ(0 nits) is 7e-7, not 0: a tolerance
		{
			c.sMid = c.tMid = sAvg > 0.0 ? sAvg : DolbyVisionTrims::Clamp(sMin + (sMax - sMin) * kDefaultMidShare, sMin, sMax);
			return c;   // the scene fits the target: no compression, no expansion
		}
		const double mid = sAvg > 0.0 ? sAvg : sMin + (sMax - sMin) * kDefaultMidShare;
		c.sMid = DolbyVisionTrims::Clamp(mid, sMin + (sMax - sMin) * kMidMinShare, sMin + (sMax - sMin) * kMidMaxShare);
		const double proportional = tMin + (c.sMid - sMin) * (tMax - tMin) / (sMax - sMin);
		const double ceiling = tMin + (tMax - tMin) * kMidCeiling;
		const double goal = c.sMid < ceiling ? c.sMid : ceiling;
		// The target mid moves from the chord toward the source mid (never below the chord:
		// the curve stays concave or straight, so it has no pole in range and is monotonic).
		c.tMid = goal > proportional ? proportional + kMidAdaptation * (goal - proportional) : proportional;
		if (!Solve(sMin, tMin, c.sMid, c.tMid, sMax, tMax, c.c1, c.c2, c.c3) || !(c.c2 - c.c1 * c.c3 > 0.0) ||
			!(1.0 + c.c3 * sMin > 0.0) || !(1.0 + c.c3 * sMax > 0.0))
		{
			// Degenerate: the straight chord (still hits min / max).
			c.tMid = proportional;
			c.c3 = 0.0;
			c.c2 = (tMax - tMin) / (sMax - sMin);
			c.c1 = tMin - c.c2 * sMin;
		}
		c.identity = false;
		return c;
	}

	// The curve for a DM and a target (nits). Source anchors: L1 min / avg / max, else the
	// source (mastering) range from the DM.
	inline Curve ForDm(const DolbyVisionTunnelFrameMetadata& dm, double targetMinNits, double targetMaxNits)
	{
		double sMin = dm.sourceMinPq / 4095.0, sMax = dm.sourceMaxPq / 4095.0, sAvg = 0.0;
		if (dm.hasSceneMetadata && dm.sceneMaxPq > dm.sceneMinPq)
		{
			sMin = dm.sceneMinPq / 4095.0;
			sMax = dm.sceneMaxPq / 4095.0;
			sAvg = dm.sceneAvgPq / 4095.0;
		}
		return Build(sMin, sAvg, sMax, DolbyVisionTrims::PqFromNits(targetMinNits), DolbyVisionTrims::PqFromNits(targetMaxNits));
	}

	// ------------------------------------------------------------------ ICtCp (BT.2100)

	// Linear BT.2020 RGB (1 = 10000 nits) <-> LMS, and PQ LMS <-> ICtCp (ITU-R BT.2100 Table 7).
	constexpr double kRgb2020ToLms[3][3] = { { 1688.0 / 4096, 2146.0 / 4096, 262.0 / 4096 },
		{ 683.0 / 4096, 2951.0 / 4096, 462.0 / 4096 }, { 99.0 / 4096, 309.0 / 4096, 3688.0 / 4096 } };
	constexpr double kLmsToIctcp[3][3] = { { 0.5, 0.5, 0.0 }, { 6610.0 / 4096, -13613.0 / 4096, 7003.0 / 4096 },
		{ 17933.0 / 4096, -17390.0 / 4096, -543.0 / 4096 } };

	inline void Mul(const double m[3][3], const double v[3], double out[3])
	{
		for (int r = 0; r < 3; ++r)
			out[r] = m[r][0] * v[0] + m[r][1] * v[1] + m[r][2] * v[2];
	}

	inline bool Invert(const double m[3][3], double inv[3][3])
	{
		const double d = m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) - m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) +
			m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
		if (!(std::fabs(d) > 1e-15))
			return false;
		inv[0][0] = (m[1][1] * m[2][2] - m[1][2] * m[2][1]) / d;
		inv[0][1] = (m[0][2] * m[2][1] - m[0][1] * m[2][2]) / d;
		inv[0][2] = (m[0][1] * m[1][2] - m[0][2] * m[1][1]) / d;
		inv[1][0] = (m[1][2] * m[2][0] - m[1][0] * m[2][2]) / d;
		inv[1][1] = (m[0][0] * m[2][2] - m[0][2] * m[2][0]) / d;
		inv[1][2] = (m[0][2] * m[1][0] - m[0][0] * m[1][2]) / d;
		inv[2][0] = (m[1][0] * m[2][1] - m[1][1] * m[2][0]) / d;
		inv[2][1] = (m[0][1] * m[2][0] - m[0][0] * m[2][1]) / d;
		inv[2][2] = (m[0][0] * m[1][1] - m[0][1] * m[1][0]) / d;
		return true;
	}

	// RGB -> XYZ of primaries (CIE xy of R, G, B, W).
	inline void RgbToXyz(const double red[2], const double green[2], const double blue[2], const double white[2], double m[3][3])
	{
		auto xyz = [](const double c[2], double v[3]) { v[0] = c[0] / c[1]; v[1] = 1.0; v[2] = (1.0 - c[0] - c[1]) / c[1]; };
		double R[3], G[3], B[3], W[3];
		xyz(red, R); xyz(green, G); xyz(blue, B); xyz(white, W);
		const double P[3][3] = { { R[0], G[0], B[0] }, { R[1], G[1], B[1] }, { R[2], G[2], B[2] } };
		double Pi[3][3];
		Invert(P, Pi);
		double S[3];
		Mul(Pi, W, S);
		for (int r = 0; r < 3; ++r)
			for (int c = 0; c < 3; ++c)
				m[r][c] = P[r][c] * S[c];
	}

	// The output RGB -> LMS matrix (linear, output primaries in, BT.2100 LMS out).
	inline void OutputToLms(const double red[2], const double green[2], const double blue[2], const double white[2], double m[3][3])
	{
		static const double r2020[2] = { 0.708, 0.292 }, g2020[2] = { 0.170, 0.797 }, b2020[2] = { 0.131, 0.046 }, d65[2] = { 0.3127, 0.3290 };
		double outXyz[3][3], bt2020Xyz[3][3], xyzTo2020[3][3];
		RgbToXyz(red, green, blue, white, outXyz);
		RgbToXyz(r2020, g2020, b2020, d65, bt2020Xyz);
		Invert(bt2020Xyz, xyzTo2020);
		double outTo2020[3][3];
		for (int r = 0; r < 3; ++r)
			for (int c = 0; c < 3; ++c)
				outTo2020[r][c] = xyzTo2020[r][0] * outXyz[0][c] + xyzTo2020[r][1] * outXyz[1][c] + xyzTo2020[r][2] * outXyz[2][c];
		for (int r = 0; r < 3; ++r)
			for (int c = 0; c < 3; ++c)
				m[r][c] = kRgb2020ToLms[r][0] * outTo2020[0][c] + kRgb2020ToLms[r][1] * outTo2020[1][c] + kRgb2020ToLms[r][2] * outTo2020[2][c];
	}

	// Linear output RGB in nits -> ICtCp (and back).
	inline void ToIctcp(const double nits[3], const double toLms[3][3], double ictcp[3])
	{
		double lin[3] = { nits[0] / 10000.0, nits[1] / 10000.0, nits[2] / 10000.0 }, lms[3], pq[3];
		Mul(toLms, lin, lms);
		for (int k = 0; k < 3; ++k)
			pq[k] = DolbyVisionTrims::PqFromNits(lms[k] * 10000.0);   // negative cones clamp to 0 (in-range input only)
		Mul(kLmsToIctcp, pq, ictcp);
	}

	inline void FromIctcp(const double ictcp[3], const double fromLms[3][3], double nits[3])
	{
		static double inv[3][3];
		static bool ready = Invert(kLmsToIctcp, inv);
		(void)ready;
		double pq[3], lms[3], lin[3];
		Mul(inv, ictcp, pq);
		for (int k = 0; k < 3; ++k)
			lms[k] = DolbyVisionTrims::NitsFromPq(pq[k]) / 10000.0;
		Mul(fromLms, lms, lin);
		for (int k = 0; k < 3; ++k)
			nits[k] = lin[k] * 10000.0;
	}

	// ------------------------------------------------------------------ trims in ICtCp

	struct IctcpSignal
	{
		DolbyVisionTrims::Signal signal;   // the output signal (PQ or gamma), its peak, kr / kg / kb
		double toLms[3][3] = {};           // output RGB -> LMS
		double fromLms[3][3] = {};         // LMS -> output RGB
	};

	inline IctcpSignal MakeSignal(const DolbyVisionTrims::Signal& s, const double red[2], const double green[2],
		const double blue[2], const double white[2])
	{
		IctcpSignal out;
		out.signal = s;
		OutputToLms(red, green, blue, white, out.toLms);
		Invert(out.toLms, out.fromLms);
		return out;
	}

	// One pixel of the output signal (in place): the trim on ICtCp intensity relative to the
	// target peak, chroma (Ct, Cp) by saturation gain x per-hue gain x (I'/I)^chroma_weight,
	// rotated by the per-hue vector. Below-black parts are carried around unchanged; a result
	// that would push an in-range channel below black has its chroma reduced (bisection).
	inline void ApplyPixelIctcp(double rgb[3], const DvTrimSelection::ExtParams& p, const IctcpSignal& is)
	{
		const DolbyVisionTrims::Signal& s = is.signal;
		double L[3], pos[3], carry[3];
		for (int c = 0; c < 3; ++c)
		{
			L[c] = DolbyVisionTrims::ToNits(rgb[c], s);
			pos[c] = L[c] > 0.0 ? L[c] : 0.0;
			carry[c] = L[c] - pos[c];
		}
		if (!(pos[0] + pos[1] + pos[2] > 0.0))
			return;
		double ictcp[3];
		ToIctcp(pos, is.toLms, ictcp);
		if (!(ictcp[0] > 0.0))
			return;
		const double peakPq = DolbyVisionTrims::PqFromNits(s.peakNits);
		const double t = ictcp[0] / peakPq;
		const double t2 = DvTrimSelection::Curve(t, p, DolbyVisionTrims::kOffsetFadePq / peakPq);
		const double I2 = t2 * peakPq;
		// Hue (for the per-hue vectors): the RGB hexagon of the linear chroma, as the L2 path.
		const double Y = s.kr * pos[0] + s.kg * pos[1] + s.kb * pos[2];
		double C[3] = { pos[0] - Y, pos[1] - Y, pos[2] - Y }, hx = 0.0, hy = 0.0, degrees = 0.0;
		double hueSat = 1.0, turn = 0.0;
		const bool sat = !DvTrimSelection::SatVectorNeutral(p), rotate = !DvTrimSelection::HueVectorNeutral(p);
		if ((sat || rotate) && DvTrimSelection::HueOf(C, hx, hy, degrees))
		{
			if (sat) hueSat = DvTrimSelection::HueInterp(p.saturation, degrees);
			if (rotate) turn = DvTrimSelection::HueInterp(p.hueDegrees, degrees);
		}
		const double ratio = I2 / ictcp[0];
		const double gain = DolbyVisionTrims::Clamp(p.base.saturationGain * hueSat *
			(p.base.chromaWeight == 0.0 ? 1.0 : std::pow(ratio > 1e-6 ? ratio : 1e-6, p.base.chromaWeight)), 0.0, 2.0);
		const double a = turn * DvTrimSelection::kPi / 180.0, ca = std::cos(a), sa = std::sin(a);
		const double ct = ictcp[1] * ca - ictcp[2] * sa, cp = ictcp[1] * sa + ictcp[2] * ca;
		double k = gain, out[3];
		{
			const double v[3] = { I2, ct * k, cp * k };
			FromIctcp(v, is.fromLms, out);
		}
		for (int iter = 0; iter < 12; ++iter)
		{
			// Rounding of the round trip is not a gamut excursion: a relative tolerance.
			const double tolerance = -kBelowTolerance * (pos[0] + pos[1] + pos[2]);
			bool below = false;
			for (int c = 0; c < 3; ++c)
				below = below || out[c] < tolerance;
			if (!below)
				break;
			k *= 0.5;   // halve the chroma toward the grey of the same intensity; the grey last
			const double v[3] = { I2, iter == 11 ? 0.0 : ct * k, iter == 11 ? 0.0 : cp * k };
			FromIctcp(v, is.fromLms, out);
		}
		for (int c = 0; c < 3; ++c)
			rgb[c] = DolbyVisionTrims::FromNits((out[c] > 0.0 ? out[c] : 0.0) + carry[c], s);   // in-range part >= black
	}

	// ------------------------------------------------------------------ mode, status, conditions

	enum class Mode { Libplacebo, DolbyStyle };

	inline Mode ParseMode(const std::string& value) { return value == "dolby_style" ? Mode::DolbyStyle : Mode::Libplacebo; }

	// The fallback's own name: bt2390, with the luminance trims only when dv_trims is on.
	inline std::string FallbackName(bool trimsOn) { return trimsOn ? "bt2390 + trims" : "bt2390"; }

	// "DV tone mapping: Dolby-style (approx.)" [+ "; L1 missing -> mid-range fallback"] /
	// "... libplacebo (<curve>)" / "... bt2390 [+ trims] (Dolby-style could not run: why)".
	inline std::string StatusText(Mode requested, bool running, const std::string& libplaceboCurve, const std::string& fallbackReason,
		bool trimsOn = true, bool l1Missing = false)
	{
		if (requested == Mode::DolbyStyle && running)
			return std::string("DV tone mapping: Dolby-style (approx.)") + (l1Missing ? "; L1 missing -> mid-range fallback" : "");
		if (requested == Mode::DolbyStyle)
			return "DV tone mapping: " + FallbackName(trimsOn) + " (Dolby-style could not run: " + fallbackReason + ")";
		return "DV tone mapping: libplacebo (" + libplaceboCurve + ")";
	}

	// The conditions the grade / the trims assumed, per DV picture: what a later CAM16 stage must
	// NOT compensate again (it compensates only the difference to the real screen and room).
	struct ReferenceConditions
	{
		bool valid = false;                 // a DV picture with DM
		DvTrimSelection::Level trimLevel = DvTrimSelection::Level::None;
		bool trimApplied = false;
		double trimTargetMinNits = 0.0;     // the selected / interpolated trim target
		double trimTargetMaxNits = 0.0;
		double masteringPeakNits = 0.0;     // L6 (else the DM's source range = L0)
		double masteringMinNits = 0.0;
		bool masteringFromL6 = false;
		int sourcePrimaryIndex = -1;        // L9 (-1 = absent)
		uint8_t contentType = 0;            // L11 (0 = absent / default)
		bool referenceMode = false;         // L11 reference-mode flag
		bool dolbyStyleCurve = false;       // true: the Dolby-style curve ran; false: libplacebo's
		std::string curve;                  // "dolby_style" or libplacebo's curve name (e.g. "bt2390")
	};

	inline ReferenceConditions Conditions(const DolbyVisionTunnelFrameMetadata& dm, const DvTrimSelection::Selection& trims,
		bool dolbyStyle, const std::string& curveName)
	{
		ReferenceConditions r;
		r.valid = dm.valid;
		if (!dm.valid)
			return r;
		r.trimLevel = trims.level;
		r.trimApplied = trims.apply;
		auto targetOf = [&](int index, double& minNits, double& maxNits)
		{
			if (index < 0 || index >= dm.trimCount)
				return false;
			const auto& t = dm.trims[index];
			if (t.level == 2)
			{
				maxNits = DolbyVisionTrims::NitsFromPq(t.targetMaxPq / 4095.0);
				minNits = 0.0;
				return true;
			}
			if (const auto* td = DvSignalLevels::FindTarget(dm, t.targetDisplayIndex))
			{
				maxNits = DolbyVisionTrims::NitsFromPq(td->maxPq / 4095.0);
				minNits = DolbyVisionTrims::NitsFromPq(td->minPq / 4095.0);
				return true;
			}
			const int pq = DvTrimSelection::PredefinedTargetMaxPq(t.targetDisplayIndex);
			if (pq < 0)
				return false;
			maxNits = DolbyVisionTrims::NitsFromPq(pq / 4095.0);
			minNits = 0.0;
			return true;
		};
		double loMin = 0, loMax = 0, hiMin = 0, hiMax = 0;
		const bool lo = targetOf(trims.lowerIndex, loMin, loMax), hi = targetOf(trims.upperIndex, hiMin, hiMax);
		if (lo && hi)
		{
			r.trimTargetMaxNits = DolbyVisionTrims::NitsFromPq(DolbyVisionTrims::PqFromNits(loMax) +
				(DolbyVisionTrims::PqFromNits(hiMax) - DolbyVisionTrims::PqFromNits(loMax)) * trims.weight);
			r.trimTargetMinNits = loMin + (hiMin - loMin) * trims.weight;
		}
		else if (lo || hi)
		{
			r.trimTargetMaxNits = lo ? loMax : hiMax;
			r.trimTargetMinNits = lo ? loMin : hiMin;
		}
		if (dm.hasL6 && dm.l6MaxDisplayMasteringLuminance > 0)
		{
			r.masteringFromL6 = true;
			r.masteringPeakNits = dm.l6MaxDisplayMasteringLuminance;
			r.masteringMinNits = dm.l6MinDisplayMasteringLuminance / 10000.0;
		}
		else
		{
			r.masteringPeakNits = DolbyVisionTrims::NitsFromPq(dm.sourceMaxPq / 4095.0);
			r.masteringMinNits = DolbyVisionTrims::NitsFromPq(dm.sourceMinPq / 4095.0);
		}
		r.sourcePrimaryIndex = dm.hasL9 ? dm.l9SourcePrimaryIndex : -1;
		r.contentType = dm.hasL11 ? dm.l11ContentType : 0;
		r.referenceMode = dm.hasL11 && dm.l11ReferenceMode;
		r.dolbyStyleCurve = dolbyStyle;
		r.curve = dolbyStyle ? "dolby_style" : curveName;
		return r;
	}
}

// ------------------------------------------------------------------ the libplacebo function
// Only where libplacebo's headers are included (the renderer); the maths above stays pure.
#if defined(LIBPLACEBO_TONE_MAPPING_H_) && defined(LIBPLACEBO_SHADERS_COLORSPACE_H_)
namespace DvDolbyStyle
{
	inline void MapLut(float* lut, const struct pl_tone_map_params* params)
	{
		// PL_HDR_PQ scaling: params->input_* / output_* and the LUT values are PQ (0..1).
		const Curve c = Build(params->input_min, params->input_avg, params->input_max, params->output_min, params->output_max);
		for (size_t i = 0; i < params->lut_size; ++i)
			lut[i] = static_cast<float>(c.Map(lut[i]));
	}

	// The output transfers the trims stage supports (as the renderer's dv_trims gate).
	inline bool OutputSupported(enum pl_color_transfer trc)
	{
		return trc == PL_COLOR_TRC_PQ || trc == PL_COLOR_TRC_GAMMA18 || trc == PL_COLOR_TRC_GAMMA20 ||
			trc == PL_COLOR_TRC_GAMMA22 || trc == PL_COLOR_TRC_GAMMA24 || trc == PL_COLOR_TRC_GAMMA26 ||
			trc == PL_COLOR_TRC_GAMMA28 || trc == PL_COLOR_TRC_BT_1886 || trc == PL_COLOR_TRC_SRGB;
	}

	// Why the Dolby-style mode cannot run for this picture ("" = it can, or it was not asked for).
	inline std::string CannotRun(bool wanted, bool calibrationLut, enum pl_color_transfer trc, bool colourStageRuns,
		bool trimsShaderMissing = false)
	{
		if (!wanted)
			return std::string();
		if (calibrationLut)
			return "a calibration LUT is active";
		if (!OutputSupported(trc))
			return "unsupported output transfer";
		// DvHostStage pre-gates the slot (F-12: off is off, not a fallback); kept for other callers.
		if (!colourStageRuns)
			return "the colour tone-shape slot is off";
		if (trimsShaderMissing)
			return "shaders/DV trims.glsl did not load";
		return std::string();
	}

	// dv_space and the output RGB -> LMS matrix for shaders/DV trims.glsl.
	inline std::vector<std::pair<const char*, double>> ShaderParameters(bool ictcp, const pl_raw_primaries* prim)
	{
		static const char* const names[9] = { "dv_lms00", "dv_lms01", "dv_lms02", "dv_lms10", "dv_lms11", "dv_lms12",
			"dv_lms20", "dv_lms21", "dv_lms22" };
		static const char* const inverse[9] = { "dv_ilms00", "dv_ilms01", "dv_ilms02", "dv_ilms10", "dv_ilms11", "dv_ilms12",
			"dv_ilms20", "dv_ilms21", "dv_ilms22" };
		std::vector<std::pair<const char*, double>> out = { { "dv_space", ictcp ? 1.0 : 0.0 } };
		if (!ictcp)
			return out;
		double m[3][3];
		const double r[2] = { prim ? prim->red.x : 0.708, prim ? prim->red.y : 0.292 };
		const double g[2] = { prim ? prim->green.x : 0.170, prim ? prim->green.y : 0.797 };
		const double b[2] = { prim ? prim->blue.x : 0.131, prim ? prim->blue.y : 0.046 };
		const double w[2] = { prim ? prim->white.x : 0.3127, prim ? prim->white.y : 0.3290 };
		OutputToLms(r, g, b, w, m);
		double mi[3][3];
		Invert(m, mi);   // passed in, not inverted per pixel (audit 2026-10-24)
		for (int k = 0; k < 9; ++k)
			out.emplace_back(names[k], m[k / 3][k % 3]);
		for (int k = 0; k < 9; ++k)
			out.emplace_back(inverse[k], mi[k / 3][k % 3]);
		return out;
	}

	// Swaps the tone-mapping function for DV pictures and restores the configured one after
	// (a settings re-apply in between is adopted as the new configured function).
	class ToneSwitch
	{
	public:
		void Apply(pl_color_map_params& params, const pl_tone_map_function* want)
		{
			if (!m_overridden || params.tone_mapping_function != m_applied)
				m_configured = params.tone_mapping_function;
			if (want)
			{
				params.tone_mapping_function = want;
				m_applied = want;
				m_overridden = true;
			}
			else if (m_overridden)
			{
				params.tone_mapping_function = m_configured;
				m_overridden = false;
			}
		}

	private:
		const pl_tone_map_function* m_configured = nullptr;
		const pl_tone_map_function* m_applied = nullptr;
		bool m_overridden = false;
	};

	// The custom function the renderer sets for DV pictures in Dolby-style mode.
	inline const pl_tone_map_function& Function()
	{
		static pl_tone_map_function f = []() {
			pl_tone_map_function g = {};
			g.name = "vp-dolby-style";
			g.description = "VP Dolby-style DV curve (ST 2094-10 Annex B form in PQ, L1-driven; approximation)";
			g.scaling = PL_HDR_PQ;
			g.map = MapLut;
			return g;
		}();
		return f;
	}
}
#endif
