// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// Light-level LUT (`input.light_level`, DRAFT): per 10-bit input code, the luminance the viewer sees (L, nits), its
// perceptual coordinate J = PQ_OETF(L), the local slope S = dJ/dcode and the visibility V of one code step in Weber
// units. Header-only CPU reference, C++14, deterministic (fixed loops, no threads, no Windows headers).
//
// Contexts (Context::kind):
//   Sdr            SDR source on an SDR display: optional SdrToneShape, then the PHYSICAL display EOTF (BT.1886 with
//                  black, numeric power, exact piecewise sRGB) with white / black = light_level_display_white_nits /
//                  light_level_display_black_nits (auto = sdr_target_nits / sdr_black_nits).
//   HdrPassthrough HDR source shown as HDR: PQ EOTF, then a BT.2390 EETF roll-off in PQ space from the source
//                  [sourceMinNits, sourcePeakNits] to the display [physical display black, displayPeakNits] (approximation of
//                  the display's own roll-off, exactness Approx).
//   HdrToSdr       HDR source tone mapped to SDR: exact port of tools/eval/tonemap_ref.py SplineTM (libplacebo 7.360
//                  pl_tone_map_spline + st2094_pick_knee) with the ToneMappingTuning constants, static knee, mapping
//                  to the ENCODING target [sdr_black_nits, sdr_target_nits]. libplacebo encodes the tone-mapped light
//                  L_out with the INVERSE of the target transfer including the target black, so the SDR signal is
//                  signal = TargetDisplay.Inverse(clamp(L_out)), sdrCode = 64 + 876 signal (with black 0 this is
//                  tonemap_ref.pq_out_to_sdr_code), stored in Entry::sdrCode. L is what the PHYSICAL display emits
//                  for that signal: PhysicalDisplay(signal); with display auto, L == L_out.
//
// BTB rule: this is a LOOKUP. Pixel data keep their below-black and above-white values untouched (the owner rule);
// the LUT never clamps picture data. Limited-range codes below 64 (v < 0) get J = S = V = 0 and L = display black
// (the physical display black); codes above white are looked up as written (SDR: curve continued, L capped at white; HDR: the
// PQ signal is clipped to [0, 1]).
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <iomanip>
#include <locale>
#include <sstream>
#include <string>
#include <vector>

#include "LightLevelSettings.h"

namespace LightLevel
{
	struct Context
	{
		enum Kind { Sdr, HdrPassthrough, HdrToSdr } kind = Sdr;
		bool fullRange = false;             // code domain: full (c / 1023) or limited ((c - 64) / 876)
		double sourcePeakNits = 0.0;        // mastering peak; 0 = unknown, the PQ ceiling is used
		double sourceMinNits = 0.0;
		double displayPeakNits = 0.0;       // HdrPassthrough: 0 = unknown, no roll-off
		double dynamicPeakNits = 0.0;       // measured scene peak (peak detection); 0 = none
	};

	enum class Exactness { Exact, Approx };

	struct Entry
	{
		double L = 0.0;         // luminance the viewer sees, nits
		double J = 0.0;         // PQ_OETF(L), 0..1
		double S = 0.0;         // dJ / dcode
		double V = 0.0;         // visibility of one code step, Weber units
		double sdrCode = 0.0;   // HdrToSdr: SDR signal code (limited-range scale); other kinds: the input code
	};

	constexpr int kLutSize = 1024;

	struct Lut
	{
		std::array<Entry, kLutSize> e;
		Exactness exactness = Exactness::Exact;
		std::string status;
		std::string fingerprint;
	};

	namespace pq
	{
		// SMPTE ST 2084 (identical to tonemap_ref.py)
		constexpr double M1 = 2610.0 / 16384.0;
		constexpr double M2 = 2523.0 / 4096.0 * 128.0;
		constexpr double C1 = 3424.0 / 4096.0;
		constexpr double C2 = 2413.0 / 4096.0 * 32.0;
		constexpr double C3 = 2392.0 / 4096.0 * 32.0;
		constexpr double Ceiling = 10000.0;     // PQ signal 1.0, defined by ST 2084

		inline double Eotf(double e)
		{
			e = std::min(1.0, std::max(0.0, e));
			const double p = std::pow(e, 1.0 / M2);
			return Ceiling * std::pow(std::max(p - C1, 0.0) / (C2 - C3 * p), 1.0 / M1);
		}
		inline double Oetf(double nits)
		{
			const double y = std::pow(std::min(1.0, std::max(0.0, nits / Ceiling)), M1);
			return std::pow((C1 + C2 * y) / (1.0 + C3 * y), M2);
		}
	}

	namespace detail
	{
		// libplacebo pl_tone_map_spline in PQ space (tonemap_ref.py SplineTM, static knee: no scene average).
		struct Spline
		{
			double inMin = 0, inMax = 0, outMin = 0, outMax = 0, sp = 0, dp = 0;
			double Pa = 0, Pb = 0, Qa = 0, Qb = 0, Qc = 0;
			bool identity = true;

			static double Mix(double a, double b, double t) { return a + (b - a) * t; }
			static double Smoothstep(double e0, double e1, double x)
			{
				if (e0 == e1) return x >= e0 ? 1.0 : 0.0;
				const double t = std::min(std::max((x - e0) / (e1 - e0), 0.0), 1.0);
				return t * t * (3.0 - 2.0 * t);
			}

			Spline(double srcPeak, double dstPeak, double srcMin, double dstMin,
				const std::array<double, ToneMappingTuning::Count>& c)
			{
				namespace T = ToneMappingTuning;
				inMin = pq::Oetf(srcMin); inMax = pq::Oetf(srcPeak);
				outMin = pq::Oetf(dstMin); outMax = pq::Oetf(dstPeak);
				const double mn = c[T::KneeMinimum], mx = c[T::KneeMaximum], df = c[T::KneeDefault];
				double srcKnee = Mix(inMin, inMax, df);
				srcKnee = std::min(std::max(srcKnee, Mix(inMin, inMax, mn)), Mix(inMin, inMax, mx));
				const double target = inMax > inMin ? (srcKnee - inMin) / (inMax - inMin) : 0.0;
				const double adapted = Mix(outMin, outMax, target);
				const double tuning = 1.0 - Smoothstep(mx, df, target) * Smoothstep(mn, df, target);
				const double adaptation = Mix(c[T::KneeAdaptation], 1.0, tuning);
				double dstKnee = Mix(srcKnee, adapted, adaptation);
				dstKnee = std::min(std::max(dstKnee, Mix(outMin, outMax, mn)), Mix(outMin, outMax, mx));
				sp = srcKnee; dp = dstKnee;
				if (inMax <= outMax || !(srcKnee > inMin)) { identity = true; return; }   // no compression needed
				identity = false;
				double slope = (dstKnee - outMin) / (srcKnee - inMin);
				double ratio = inMax / outMax - 1.0;
				ratio = std::min(std::max(c[T::SlopeTuning] * ratio, c[T::SlopeOffset]), 1.0 + c[T::SlopeOffset]);
				slope = std::pow(slope, (1.0 - c[T::SplineContrast]) * ratio);
				const double iMin = inMin - srcKnee, iMax = inMax - srcKnee;
				const double oMin = outMin - dstKnee, oMax = outMax - dstKnee;
				Pa = (oMin - slope * iMin) / (iMin * iMin);
				Pb = slope;
				const double t = 2.0 * iMax * iMax;
				Qa = (slope * iMax - oMax) / (iMax * t);
				Qb = -3.0 * (slope * iMax - oMax) / t;
				Qc = slope;
			}

			// PQ in -> PQ out, input clipped to [inMin, inMax] as the libplacebo LUT.
			double operator()(double x) const
			{
				x = std::min(std::max(x, inMin), inMax);
				if (identity) return std::min(x, outMax);
				const double u = x - sp;
				return (u > 0 ? ((Qa * u + Qb) * u + Qc) * u : (Pa * u + Pb) * u) + dp;
			}
		};

		// Display EOTF, signal v (0 = black, 1 = white, may exceed 1) -> nits, capped at white.
		struct Display
		{
			Transfer transfer = Transfer::Bt1886;
			double power = 2.4, white = 0, black = 0, a = 0, b = 0;

			// Encoding target display (sdr_target_nits / sdr_black_nits) or physical display (resolved display keys).
			Display(const Settings& s, double whiteNits, double blackNits) : transfer(s.transfer),
				power(s.transfer == Transfer::Power ? s.gammaPower : 2.4), white(whiteNits), black(blackNits)
			{
				if (transfer == Transfer::Bt1886)
				{
					power = 2.4;    // BT.1886 definition
					const double lw = std::pow(white, 1.0 / power), lb = std::pow(black, 1.0 / power);
					a = std::pow(lw - lb, power);
					b = lb / (lw - lb);
				}
			}
			double operator()(double v) const
			{
				double l;
				if (transfer == Transfer::Bt1886)
					l = a * std::pow(std::max(v + b, 0.0), power);
				else if (transfer == Transfer::Srgb)
				{
					const double x = std::max(v, 0.0);
					const double lin = x <= 0.04045 ? x / 12.92 : std::pow((x + 0.055) / 1.055, 2.4);
					l = black + (white - black) * lin;
				}
				else
					l = black + (white - black) * std::pow(std::max(v, 0.0), power);
				return std::min(l, white);
			}
			// Exact inverse of operator() for L in [black, white]: nits -> signal. L below black gives signal 0
			// (piecewise forms) or the BT.1886 signal that reproduces L.
			double Inverse(double nits) const
			{
				if (transfer == Transfer::Bt1886)
					return std::pow(std::max(nits, 0.0) / a, 1.0 / power) - b;
				const double lin = std::max(nits - black, 0.0) / (white - black);
				if (transfer == Transfer::Srgb)
					return lin <= 0.04045 / 12.92 ? lin * 12.92 : 1.055 * std::pow(lin, 1.0 / 2.4) - 0.055;
				return std::pow(lin, 1.0 / power);
			}
		};

		// BT.2390 EETF in PQ space (ITU-R BT.2390-10 section 5.4.1, knee offset 1 = specification form).
		struct Eetf
		{
			double minLum = 0, range = 1, maxT = 1, minT = 0, ks = 1;
			bool identity = true;
			Eetf(double srcMin, double srcPeak, double dstMin, double dstPeak)
			{
				const double lo = pq::Oetf(srcMin), hi = pq::Oetf(srcPeak);
				minLum = lo; range = hi - lo;
				if (!(dstPeak > 0.0) || !(range > 0.0) || dstPeak >= srcPeak) return;   // no roll-off needed
				maxT = (pq::Oetf(dstPeak) - lo) / range;
				minT = (pq::Oetf(dstMin) - lo) / range;
				ks = 1.5 * maxT - 0.5;
				identity = false;
			}
			double operator()(double nits, double dstPeak) const
			{
				if (identity) return nits;
				const double e1 = std::min(1.0, std::max(0.0, (pq::Oetf(nits) - minLum) / range));   // clipped, mirrors light_level_ref.py
				double e2 = e1;
				if (e1 >= ks)
				{
					const double t = std::min(1.0, std::max(0.0, (e1 - ks) / (1.0 - ks))), t2 = t * t, t3 = t2 * t;
					e2 = (2.0 * t3 - 3.0 * t2 + 1.0) * ks + (t3 - 2.0 * t2 + t) * (1.0 - ks) + (-2.0 * t3 + 3.0 * t2) * maxT;
				}
				const double e3 = e2 + minT * std::pow(1.0 - e2, 4.0);
				return std::min(pq::Eotf(e3 * range + minLum), dstPeak);
			}
		};

		inline bool PeakDetectionUsed(const Settings& s, const Context& c)
		{
			return c.dynamicPeakNits > 0.0 && PeakDetectionOn(s);
		}

		// One continuous evaluation of the analytic map at (possibly half-integer) code c.
		struct Evaluator
		{
			const Settings& s;
			const Context& ctx;
			Display target;     // encoding target: sdr_target_nits / sdr_black_nits
			Display display;    // physical display: light_level_display_white_nits / _black_nits
			std::array<double, ToneMappingTuning::Count> tone;
			double srcPeak;
			Spline spline;
			Eetf eetf;

			static std::array<double, ToneMappingTuning::Count> Resolve(const Settings& s)
			{
				std::array<double, ToneMappingTuning::Count> t{};
				for (int i = 0; i < ToneMappingTuning::Count; ++i)
					t[i] = s.tuning[i].value_or(ToneMappingTuning::Specs()[i].defaultValue);
				return t;
			}
			static double SourcePeak(const Settings& s, const Context& c)
			{
				const double p = PeakDetectionUsed(s, c) ? c.dynamicPeakNits : c.sourcePeakNits;
				return p > 0.0 ? p : pq::Ceiling;
			}

			Evaluator(const Settings& settings, const Context& context)
				: s(settings), ctx(context), target(settings, settings.sdrTargetNits, settings.sdrBlackNits),
				display(settings, settings.displayWhiteNits, settings.displayBlackNits), tone(Resolve(settings)),
				srcPeak(SourcePeak(settings, context)),
				spline(srcPeak, settings.sdrTargetNits, context.sourceMinNits, settings.sdrBlackNits, tone),
				eetf(context.sourceMinNits, srcPeak, settings.displayBlackNits, context.displayPeakNits) {}

			double Signal(double c) const { return ctx.fullRange ? c / 1023.0 : (c - 64.0) / 876.0; }
			bool Btb(double c) const { return Signal(c) < 0.0; }

			// Returns luminance (nits); sdrCode set for HdrToSdr, else the input code.
			double L(double c, double& sdrCode) const
			{
				sdrCode = c;
				const double v = Signal(c);
				if (v < 0.0) return s.displayBlackNits;     // BTB: physical display black, see the header
				switch (ctx.kind)
				{
				case Context::Sdr:
				{
					double vp = v;
					if (SdrToneShape::Active(s.shape))
						vp = SdrToneShape::Map(v, EffectivePower(s), s.shape.headroom, s.shape.kneeStart,
							SdrToneShape::EffectiveStrength(s.shape));
					return display(vp);
				}
				case Context::HdrPassthrough:
				{
					const double nits = pq::Eotf(v);
					const double peak = ctx.displayPeakNits > 0.0 ? ctx.displayPeakNits : srcPeak;
					return std::max(eetf(nits, peak), s.displayBlackNits);
				}
				default:    // HdrToSdr
				{
					const double out = pq::Eotf(spline(std::min(std::max(v, 0.0), 1.0)));
					const double sig = target.Inverse(std::min(std::max(out, s.sdrBlackNits), s.sdrTargetNits));
					sdrCode = 64.0 + 876.0 * sig;
					return display(sig);
				}
				}
			}
			double L(double c) const { double unused; return L(c, unused); }
		};

	}

	inline std::string Fingerprint(const Settings& s, const Context& c)
	{
		std::ostringstream o; o.imbue(std::locale::classic()); o << std::setprecision(17);
		o << "LL2|" << s.weber << '|' << s.ambientNits << '|' << s.sdrTargetNits << '|' << s.sdrBlackNits << '|'
			<< s.displayWhiteNits << '|' << s.displayBlackNits << '|' << s.quality << '|'
			<< static_cast<int>(s.transfer) << '|' << s.gammaPower << '|' << s.toneMapping << '|'
			<< static_cast<int>(s.peakDetection) << '|' << ToneMappingTuning::Fingerprint(s.tuning) << '|'
			<< (s.shape.knee ? 1 : 0) << '|' << s.shape.headroom << '|' << s.shape.kneeStart << '|' << s.shape.kneeStrength
			<< "|ctx|" << static_cast<int>(c.kind) << '|' << (c.fullRange ? 1 : 0) << '|' << c.sourcePeakNits << '|'
			<< c.sourceMinNits << '|' << c.displayPeakNits << '|' << c.dynamicPeakNits;
		return o.str();
	}

	inline Lut Build(const Settings& s, const Context& c)
	{
		Lut lut;
		lut.fingerprint = Fingerprint(s, c);
		const detail::Evaluator ev(s, c);

		// Exactness and status: every reason the map is an approximation is named, joined with "; ".
		lut.exactness = Exactness::Exact;
		if (c.kind == Context::Sdr)
			lut.status = "light level: exact (SDR display EOTF)";
		else if (c.kind == Context::HdrPassthrough)
		{
			lut.exactness = Exactness::Approx;
			lut.status = "light level: HDR passthrough (approx display roll-off)";
		}
		else
		{
			std::vector<std::string> reasons;
			if (s.toneMapping != "auto" && s.toneMapping != "spline")
				reasons.push_back("tone_mapping " + s.toneMapping + " not modelled; spline used");
			if (PeakDetectionOn(s))
				reasons.push_back(c.dynamicPeakNits > 0.0 ? "dynamic tone mapping"
					: "dynamic tone mapping; static curve, no VP peak estimate");
			if (reasons.empty())
				lut.status = "light level: exact (spline tone map)";
			else
			{
				lut.exactness = Exactness::Approx;
				lut.status = "light level: approx (";
				for (std::size_t r = 0; r < reasons.size(); ++r)
					lut.status += (r ? "; " : "") + reasons[r];
				lut.status += ")";
			}
		}

		// Analytic maps, central difference over +-0.5 code (one-sided at the ends).
		const double lowestStep = pq::Eotf(1.0 / (kLutSize - 1));   // PQ minimum step at 0: the only denominator guard
		const double floorNits = s.displayBlackNits + s.ambientNits;
		for (int i = 0; i < kLutSize; ++i)
		{
			Entry& en = lut.e[static_cast<std::size_t>(i)];
			const double code = static_cast<double>(i);
			double sdrCode = code;
			en.L = ev.L(code, sdrCode);
			en.sdrCode = sdrCode;
			if (ev.Btb(code)) { en.J = 0.0; en.S = 0.0; en.V = 0.0; continue; }
			en.J = pq::Oetf(en.L);
			const double lo = i > 0 ? code - 0.5 : code;
			const double hi = i < kLutSize - 1 ? code + 0.5 : code;
			const double span = hi - lo;
			const double lLo = ev.L(lo), lHi = ev.L(hi);
			en.S = (pq::Oetf(lHi) - pq::Oetf(lLo)) / span;
			const double dL = std::fabs(lHi - lLo) / span;
			const double denom = s.weber * std::max(en.L + floorNits, lowestStep);
			en.V = dL / denom;
		}
		return lut;
	}

	// code10 on the 10-bit code axis; the INDEX is clamped to [0, 1023] (the LUT is a lookup, pixel data keep their
	// BTB / WTW values), linear interpolation between integer codes.
	inline Entry Fetch(const Lut& lut, double code10)
	{
		double idx = code10;
		if (!(idx > 0.0)) idx = 0.0;
		if (idx > static_cast<double>(kLutSize - 1)) idx = static_cast<double>(kLutSize - 1);
		const int i0 = static_cast<int>(std::floor(idx));
		const int i1 = std::min(i0 + 1, kLutSize - 1);
		const double t = idx - static_cast<double>(i0);
		const Entry& a = lut.e[static_cast<std::size_t>(i0)];
		const Entry& b = lut.e[static_cast<std::size_t>(i1)];
		Entry r;
		r.L = a.L + (b.L - a.L) * t;
		r.J = a.J + (b.J - a.J) * t;
		r.S = a.S + (b.S - a.S) * t;
		r.V = a.V + (b.V - a.V) * t;
		r.sdrCode = a.sdrCode + (b.sdrCode - a.sdrCode) * t;
		return r;
	}

	// RGBA32F, 1024 x 1: (J, S, V, L / 10000), the layout light_level_lut_fetch reads.
	inline std::vector<float> PackTexels(const Lut& lut)
	{
		std::vector<float> t(static_cast<std::size_t>(kLutSize) * 4);
		for (int i = 0; i < kLutSize; ++i)
		{
			const Entry& en = lut.e[static_cast<std::size_t>(i)];
			float* p = &t[static_cast<std::size_t>(i) * 4];
			p[0] = static_cast<float>(en.J);
			p[1] = static_cast<float>(en.S);
			p[2] = static_cast<float>(en.V);
			p[3] = static_cast<float>(en.L / pq::Ceiling);
		}
		return t;
	}

	// Rebuilds only when the fingerprint changes.
	class LutCache
	{
	public:
		const Lut& Get(const Settings& s, const Context& c)
		{
			const std::string fp = Fingerprint(s, c);
			rebuilt_ = !valid_ || fp != lut_.fingerprint;
			if (rebuilt_) { lut_ = Build(s, c); valid_ = true; }
			return lut_;
		}
		bool LastRebuilt() const { return rebuilt_; }
		void Reset() { valid_ = false; }
	private:
		Lut lut_;
		bool valid_ = false;
		bool rebuilt_ = false;
	};
}
