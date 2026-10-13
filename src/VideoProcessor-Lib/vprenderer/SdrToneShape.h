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
//                          Nothing is created above the master's white.
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
	// scaled by 1 - headroom). slope: dy'/dy.
	inline double Knee(double y, double start, double strength, double* slope = nullptr)
	{
		if (slope)
			*slope = 1.0;
		if (strength <= 0.0 || y <= start)
			return y;
		const double k = 1.0 - start;
		const double e = std::exp(-(y - start) / k);
		const double rolled = start + k * (1.0 - e);
		if (slope)
			*slope = (1.0 - strength) + strength * e;
		return y + (rolled - y) * strength;
	}

	// One channel of the output signal (sign-preserving: below-black kept).
	inline double Map(double v, double gamma, double headroom, double start, double strength)
	{
		const double sign = v < 0.0 ? -1.0 : 1.0;
		const double linear = std::pow(std::fabs(v), gamma) * (1.0 - headroom);
		const double shaped = v < 0.0 ? linear : Knee(linear, start, strength);
		return sign * std::pow(shaped, 1.0 / gamma);
	}
}
