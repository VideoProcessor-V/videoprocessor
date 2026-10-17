#pragma once

#include <algorithm>
#include <cmath>

// Delta 13 (owner 2026-10-13, docs/VP-delta-11-shadow-boost.md): SDR tone
// shape and headroom. Tone-mapped HDR looks better than native SDR because
// of the curve's shape; SDR sources get the shape directly, with no inverse
// tone map. shaders/SDR tone shape.glsl does it at PRE_OUTPUT (after the
// Stage 10 restore, before the LED compensation and the encode); Map() is
// its reference, per channel, in the output's signal (0-1, values above 1
// are the above-white the Stage 10 carry keeps).
//
//   sdr_white_headroom h   reference white goes to (1 - h) of peak: a scale
//                          in linear light (the output gamma), a placement,
//                          not a stretch. 0 = today.
//   sdr_tone_shape knee    above sdr_knee_start (a fraction of peak, linear)
//                          the light rolls off with slope 1 at the start
//                          toward peak (the compensation knee's form), mixed
//                          with the straight line by sdr_knee_strength.
//                          Nothing is created above the master's white;
//                          the master's above-white stays above the
//                          shaped white (audit 2026-10-16, see Knee).
//   sdr_contrast_recovery  libplacebo's contrast recovery for SDR: the local
//                          detail the knee compresses is added back, in
//                          proportion to how much the knee flattened it.
//                          auto = the profile's contrast_recovery (libplacebo's
//                          own when that is auto), off = 0.
//
// Off (shape off, headroom 0) installs no hook: today's exact path. HDR
// sources and HDR outputs never get it.
namespace SdrToneShape
{
	struct Settings
	{
		bool knee = false;          // sdr_tone_shape: knee
		double headroom = 0.0;      // sdr_white_headroom, 0-0.5
		double kneeStart = 0.80;    // sdr_knee_start, 0.5-0.99
		double kneeStrength = 0.5;  // sdr_knee_strength, 0-1
		double recovery = -1.0;     // sdr_contrast_recovery: -1 auto, 0 off, else 0-2
	};

	inline bool Active(const Settings& settings)
	{
		return settings.knee || settings.headroom > 0.0;
	}

	// The strength the shader gets: 0 without the knee.
	inline double EffectiveStrength(const Settings& settings)
	{
		return settings.knee ? std::min(1.0, std::max(0.0, settings.kneeStrength)) : 0.0;
	}

	// The contrast recovery the shader gets: auto follows the profile's
	// (libplacebo's) value.
	inline double EffectiveRecovery(const Settings& settings, double profileRecovery)
	{
		return settings.recovery < 0.0 ? std::max(0.0, profileRecovery) : settings.recovery;
	}

	// Linear light after the placement and the knee (y: linear, already
	// scaled by 1 - headroom). slope: dy'/dy. white: the placed reference
	// white (1 - headroom; Map passes it).
	//
	// Audit 2026-10-16 (owner rule: below-black and above-white preserved in
	// all cases): up to the placed white the knee is exactly as before,
	//   K(y) = y + (start + k (1 - exp(-(y - start) / k)) - y) * strength,
	//   k = 1 - start,
	// so every value inside 0..1 is unchanged. That roll-off approaches peak
	// asymptotically, so at strength 1 the source's above-white was squeezed
	// toward 1 and flattened. Above the placed white the curve now continues
	// as its tangent there:
	//   y > white:  K(white) + K'(white) (y - white),
	//   K'(white) = (1 - strength) + strength * exp(-(white - start) / k)
	// - C1 at the white (and still C1, slope 1, at the knee start), strictly
	// increasing (K' >= exp(-1) > 0 for every start/headroom/strength), never
	// above the unshaped value (K' <= 1, K(white) <= white), identity at
	// strength 0. A knee start at or above the placed white leaves the
	// above-white untouched (K'(white) = 1).
	inline double Knee(double y, double start, double strength, double* slope = nullptr, double white = 1.0)
	{
		if (slope)
			*slope = 1.0;
		const double yc = y < white ? y : white;
		if (strength <= 0.0 || yc <= start)
			return y;
		const double k = 1.0 - start;
		const double e = std::exp(-(yc - start) / k);
		const double rolled = start + k * (1.0 - e);
		const double m = (1.0 - strength) + strength * e;
		if (slope)
			*slope = m;
		const double kneed = yc + (rolled - yc) * strength;
		return y > white ? kneed + m * (y - white) : kneed;
	}

	// One channel of the output signal (sign-preserving: below-black kept).
	inline double Map(double v, double gamma, double headroom, double start, double strength)
	{
		const double sign = v < 0.0 ? -1.0 : 1.0;
		const double linear = std::pow(std::fabs(v), gamma) * (1.0 - headroom);
		const double shaped = v < 0.0 ? linear : Knee(linear, start, strength, nullptr, 1.0 - headroom);
		return sign * std::pow(shaped, 1.0 / gamma);
	}
}
