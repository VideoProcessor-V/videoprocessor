// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// CF-2 B1 (refs/123 v2 N1.2): THE hybrid blend law, written once for both placements (B1: the end placement in
// scaling/output/AdaptiveDownscale's upscale form; B2: scaling/route_4k). Header-only, C++14, no libplacebo.
//
//   out = sharp + (1 - w) (alias_safe - sharp)            on luma; the sharp branch's chroma stays
//   w   = clamp(w_base x edge_term x grain_term, 0, 1) x motion_term
//
//   sharp       libplacebo's main scaler output (the POST_KERNEL picture; a LUMA prescaler there = the CNN evaluation arm)
//   alias_safe  ScalerPass from the PRE_KERNEL source picture (the faithful branch)
//   w_base      the setting x the bitrate term (AdaptiveDownscale::BitrateScale of the shared quality reading)
//   edge_term   1 - e, e = the edge ramp (edge_lo .. edge_hi) at the output pixel's source position
//   grain_term  g, the grain scale (map or scalar)
//   motion_term the motion map's factor; motion_floor when the vectors are absent or invalid (N1.4: the alias-safe lean)
//
// The motion term multiplies AFTER the clamp (today's order: motion on / off never moves an in-range weight). No clamp of the
// output: codes below black and above white (BTB / WTW) pass exactly as the branches give them. WeightGlsl() is the same law
// for the shaders; the CPU functions below are its mirror (tests, the M1 harness model).

#include <VP-V/core/Ramp.h>   // RampMath::Clamp01: the shared clamp (U25)

#include <cmath>

namespace HybridBlend
{
	struct Terms
	{
		float wBase = 1.0f;   // the setting x the bitrate term
		float edge = 0.0f;    // e, the edge ramp 0..1 (edge_term = 1 - e)
		float grain = 1.0f;   // g, the grain term
		float motion = 1.0f;  // the motion term (MotionTerm)
	};

	// Fail-safe unit-range clamp: non-finite values (NaN, +/-Inf) map to 0 (alias-safe / off).
	inline float SanitizeUnit(float x)
	{
		return std::isfinite(x) ? RampMath::Clamp01(x) : 0.0f;
	}

	inline float EdgeTerm(float e)
	{
		return std::isfinite(e) ? 1.0f - RampMath::Clamp01(e) : 0.0f;
	}

	// Valid vectors: the map's factor; absent / invalid (no estimate, resync, a drop): the floor (the alias-safe lean).
	// motionStrength (0..1): continuum blend from standard unadapted blend (0 -> 1) to full motion factor (1 -> raw).
	inline float MotionTerm(bool vectorsValid, float mapFactor, float motionFloor, float motionStrength = 1.0f)
	{
		const float raw = SanitizeUnit(vectorsValid ? mapFactor : motionFloor);
		const float s = SanitizeUnit(motionStrength);
		return 1.0f - s * (1.0f - raw);
	}

	inline float Weight(const Terms& t)
	{
		return SanitizeUnit(t.wBase * EdgeTerm(t.edge) * t.grain) * SanitizeUnit(t.motion);
	}

	// One channel's luma form: the sharp value plus (1 - w) times the luma difference.
	inline float BlendLuma(float sharpY, float safeY, float w)
	{
		return sharpY + (1.0f - w) * (safeY - sharpY);
	}

	// The RGB form the shader writes: every channel moves by the same luma difference (kw = the source's luma weights).
	inline void BlendRgb(const float sharp[3], const float safe[3], const float kw[3], float w, float out[3])
	{
		const float ys = sharp[0] * kw[0] + sharp[1] * kw[1] + sharp[2] * kw[2];
		const float yf = safe[0] * kw[0] + safe[1] * kw[1] + safe[2] * kw[2];
		const float d = (1.0f - w) * (yf - ys);
		for (int c = 0; c < 3; ++c)
			out[c] = sharp[c] + d;
	}

	// The law as GLSL (one function, the same order as Weight).
	inline const char* WeightGlsl()
	{
		return "float hb_weight(float w_base, float e, float g, float m)\n"
			"{\n"
			"    return clamp(w_base * (1.0 - clamp(e, 0.0, 1.0)) * g, 0.0, 1.0) * clamp(m, 0.0, 1.0);\n"
			"}\n";
	}
}
