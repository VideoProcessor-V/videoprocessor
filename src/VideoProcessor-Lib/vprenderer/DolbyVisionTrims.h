#pragma once

#include <video_frame_formatter/IVideoFrameFormatter.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>

// DV extension item 3 (owner 2026-10-17, dv_trims: off by default): Dolby
// Vision Level 2 display trims applied by VP after libplacebo's tone mapping.
//
// NOT Dolby display management and NOT certified: an independent
// reimplementation from the published L2 field semantics (12-bit codes,
// 2048 = neutral) of what a trim means, applied to VP's own tone-mapped
// output. Dolby's target display management (its tone curve, L8 and the
// exact trim maths) is proprietary; this only steers libplacebo's result
// toward the grader's trim for the nearest target display.
//
// Selection: the L2 trims' target_max_PQ (12-bit PQ codes) are compared with
// the display's peak in PQ. A trim at the display's peak is used as sent;
// two bracketing trims are interpolated linearly in PQ; below every target
// the lowest-target trim is used; above every target the highest-target trim
// fades to neutral at the mastering peak (sourceMaxPq), and at or above the
// mastering peak nothing is applied (no trim exists for it: identity).
//
// The trim, per pixel, in the output's signal after tone mapping
// (PRE_OUTPUT; shaders/DV trims.glsl, kept identical to ApplyPixel):
//   1. linear light (nits) from the output signal: PQ, or a gamma signal
//      times the display peak (SDR output)
//   2. Y = luminance with the output's coefficients; t = PQ(Y) / PQ(peak)
//   3. CDL on t (0..1): t' = clamp(t * slope + offset, 0, 1) ^ power; above
//      the peak (t > 1, the kept above-white) the curve continues with slope
//      1 from f(1), so nothing above white is clipped; Y <= 0 is untouched
//      (below-black kept)
//   4. RGB scaled by Y'/Y (hue and ratios kept)
//   5. saturation around Y': saturation_gain x (Y'/Y)^chroma_weight,
//      clamped to 0..2 (chroma_weight: how far chroma follows the intensity
//      change)
//   6. back to the output signal
// Field decoding (12-bit, 2048 = neutral): slope = c/4096 + 0.5, offset =
// c/4096 - 0.5, power = c/4096 + 0.5, chroma_weight = c/4096 - 0.5,
// saturation_gain = c/4096 + 0.5. ms_weight is not used (it applies to
// Dolby's own detail management).
namespace DolbyVisionTrims
{
	struct Params
	{
		double slope = 1.0;
		double offset = 0.0;
		double power = 1.0;
		double chromaWeight = 0.0;
		double saturationGain = 1.0;
	};

	inline bool Neutral(const Params& p)
	{
		return p.slope == 1.0 && p.offset == 0.0 && p.power == 1.0 &&
			p.chromaWeight == 0.0 && p.saturationGain == 1.0;
	}

	inline Params Decode(const DolbyVisionTunnelFrameMetadata::Trim& trim)
	{
		Params p;
		p.slope = trim.slope / 4096.0 + 0.5;
		p.offset = trim.offset / 4096.0 - 0.5;
		p.power = trim.power / 4096.0 + 0.5;
		p.chromaWeight = trim.chromaWeight / 4096.0 - 0.5;
		p.saturationGain = trim.saturationGain / 4096.0 + 0.5;
		return p;
	}

	inline Params Lerp(const Params& a, const Params& b, double w)
	{
		Params p;
		p.slope = a.slope + (b.slope - a.slope) * w;
		p.offset = a.offset + (b.offset - a.offset) * w;
		p.power = a.power + (b.power - a.power) * w;
		p.chromaWeight = a.chromaWeight + (b.chromaWeight - a.chromaWeight) * w;
		p.saturationGain = a.saturationGain + (b.saturationGain - a.saturationGain) * w;
		return p;
	}

	// SMPTE ST 2084: nits <-> PQ signal (0..1).
	inline double PqFromNits(double nits)
	{
		const double m1 = 2610.0 / 16384.0, m2 = 2523.0 / 4096.0 * 128.0;
		const double c1 = 3424.0 / 4096.0, c2 = 2413.0 / 4096.0 * 32.0, c3 = 2392.0 / 4096.0 * 32.0;
		const double y = (nits < 0.0 ? 0.0 : nits) / 10000.0;
		const double p = std::pow(y, m1);
		return std::pow((c1 + c2 * p) / (1.0 + c3 * p), m2);
	}

	inline double NitsFromPq(double pq)
	{
		const double m1 = 2610.0 / 16384.0, m2 = 2523.0 / 4096.0 * 128.0;
		const double c1 = 3424.0 / 4096.0, c2 = 2413.0 / 4096.0 * 32.0, c3 = 2392.0 / 4096.0 * 32.0;
		const double e = std::pow(pq < 0.0 ? 0.0 : pq, 1.0 / m2);
		const double num = e - c1 > 0.0 ? e - c1 : 0.0;
		return 10000.0 * std::pow(num / (c2 - c3 * e), 1.0 / m1);
	}

	struct Selection
	{
		bool apply = false;          // a non-neutral trim was chosen
		Params params;
		int lowerIndex = -1;         // trims[] index at or below the display (-1 none)
		int upperIndex = -1;         // trims[] index at or above the display
		double weight = 0.0;         // toward upper (0 = lower only)
		double displayPq = 0.0;      // 12-bit PQ code of the display peak
		const char* reason = "";
	};

	// The trim for a display peak in nits (> 0).
	inline Selection Select(const DolbyVisionTunnelFrameMetadata& dm, double displayPeakNits)
	{
		Selection s;
		if (!dm.valid) { s.reason = "no DM"; return s; }
		if (!(displayPeakNits > 0.0)) { s.reason = "no display peak"; return s; }
		s.displayPq = PqFromNits(displayPeakNits) * 4095.0;
		for (int i = 0; i < dm.trimCount && i < DolbyVisionTunnelFrameMetadata::kMaxTrims; ++i)
		{
			const auto& t = dm.trims[i];
			if (t.level != 2 || t.targetMaxPq > 4095)
				continue;
			if (t.targetMaxPq <= s.displayPq &&
				(s.lowerIndex < 0 || t.targetMaxPq > dm.trims[s.lowerIndex].targetMaxPq))
				s.lowerIndex = i;
			if (t.targetMaxPq >= s.displayPq &&
				(s.upperIndex < 0 || t.targetMaxPq < dm.trims[s.upperIndex].targetMaxPq))
				s.upperIndex = i;
		}
		if (s.lowerIndex < 0 && s.upperIndex < 0) { s.reason = "no L2 trim"; return s; }
		const bool sourcePeak = dm.sourceMaxPq > dm.sourceMinPq && dm.sourceMaxPq <= 4095;
		if (sourcePeak && s.displayPq >= dm.sourceMaxPq)
		{
			s.lowerIndex = s.upperIndex = -1;
			s.reason = "display at or above the mastering peak (identity)";
			return s;
		}
		if (s.lowerIndex >= 0 && s.upperIndex >= 0)
		{
			const double lo = dm.trims[s.lowerIndex].targetMaxPq, hi = dm.trims[s.upperIndex].targetMaxPq;
			s.weight = hi > lo ? (s.displayPq - lo) / (hi - lo) : 0.0;
			s.params = Lerp(Decode(dm.trims[s.lowerIndex]), Decode(dm.trims[s.upperIndex]), s.weight);
			s.reason = s.lowerIndex == s.upperIndex || hi <= lo ? "trim at the display peak" : "interpolated between two trims";
		}
		else if (s.upperIndex >= 0)
		{
			s.params = Decode(dm.trims[s.upperIndex]);
			s.reason = "display below every trim target: the lowest-target trim";
		}
		else
		{
			const double lo = dm.trims[s.lowerIndex].targetMaxPq;
			if (sourcePeak && dm.sourceMaxPq > lo)
			{
				s.weight = (s.displayPq - lo) / (dm.sourceMaxPq - lo);
				s.params = Lerp(Decode(dm.trims[s.lowerIndex]), Params(), s.weight);
				s.reason = "above every trim target: fading to neutral at the mastering peak";
			}
			else
			{
				s.params = Decode(dm.trims[s.lowerIndex]);
				s.reason = "above every trim target: the highest-target trim";
			}
		}
		s.apply = !Neutral(s.params);
		return s;
	}

	// The output signal the trim works in.
	struct Signal
	{
		bool pq = true;              // PQ output (HDR); else a gamma signal
		double gamma = 2.2;          // gamma output: linear = v^gamma x peak nits
		double peakNits = 1000.0;    // the display peak (the trims' reference)
		double kr = 0.2627, kg = 0.6780, kb = 0.0593;   // luminance coefficients
	};

	inline double ToNits(double v, const Signal& s)
	{
		if (s.pq)
			return v < 0.0 ? -NitsFromPq(-v) : NitsFromPq(v);   // below-black kept
		const double a = std::pow(std::fabs(v), s.gamma) * s.peakNits;
		return v < 0.0 ? -a : a;
	}

	inline double FromNits(double nits, const Signal& s)
	{
		if (s.pq)
			return nits < 0.0 ? -PqFromNits(-nits) : PqFromNits(nits);
		const double a = std::pow(std::fabs(nits) / s.peakNits, 1.0 / s.gamma);
		return nits < 0.0 ? -a : a;
	}

	// The CDL on t (PQ of the luminance over PQ of the peak), continued with
	// slope 1 above the peak.
	inline double Curve(double t, const Params& p)
	{
		const double inside = t < 1.0 ? t : 1.0;
		double v = inside * p.slope + p.offset;
		v = v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v);
		v = std::pow(v, p.power);
		return t > 1.0 ? v + (t - 1.0) : v;
	}

	// One pixel of the output signal (r, g, b in place).
	inline void ApplyPixel(double rgb[3], const Params& p, const Signal& s)
	{
		double L[3];
		for (int c = 0; c < 3; ++c)
			L[c] = ToNits(rgb[c], s);
		const double Y = s.kr * L[0] + s.kg * L[1] + s.kb * L[2];
		if (!(Y > 0.0))
			return;
		const double peakPq = PqFromNits(s.peakNits);
		const double t = PqFromNits(Y) / peakPq;
		const double t2 = Curve(t, p);
		const double Y2 = NitsFromPq(t2 * peakPq);
		const double r = Y2 / Y;
		double sat = p.saturationGain * (p.chromaWeight == 0.0 ? 1.0 : std::pow(r > 1e-6 ? r : 1e-6, p.chromaWeight));
		sat = sat < 0.0 ? 0.0 : (sat > 2.0 ? 2.0 : sat);
		for (int c = 0; c < 3; ++c)
		{
			const double scaled = L[c] * r;
			rgb[c] = FromNits(Y2 + sat * (scaled - Y2), s);
		}
	}

	inline std::string Describe(const Selection& s)
	{
		char text[256];
		std::snprintf(text, sizeof(text), "%s (display %.0f PQ code, slope %.3f offset %.3f power %.3f chroma_weight %.3f saturation %.3f)",
			s.reason, s.displayPq, s.params.slope, s.params.offset, s.params.power, s.params.chromaWeight,
			s.params.saturationGain);
		return text;
	}
}
