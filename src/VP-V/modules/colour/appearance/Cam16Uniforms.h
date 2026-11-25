// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// colour.appearance: the uniform block of the CAM16 appearance pass, packed from the viewing
// conditions once per settings or room step (never per pixel). CPU only; the shader (CAM-3) reads
// this layout. All floats, 16-byte rows, so the same bytes can feed a std140 uniform block.

#include "Cam16.h"

#include <algorithm>
#include <cmath>

namespace Cam16
{
	// One side of the model (reference or screen), the per-condition constants the shader needs.
	struct SideUniforms
	{
		float dc[3];      // adaptation gain per CAT16 channel
		float fl;         // luminance-level adaptation factor
		float nbb;        // background induction (= Ncb)
		float aw;         // achromatic response of the white
		float cz;         // c * z (J exponent)
		float nc;         // chromatic induction of the surround
		float n;          // Yb / Yw
		float tFactor;    // (1.64 - 0.29^n)^0.73
		float flQuarter;  // FL^0.25
		float pad;
	};

	struct Cam16Uniforms
	{
		SideUniforms reference;
		SideUniforms screen;
		float strength;   // 0 = identity, 1 = full model
		float veil;       // screen-side flare as a fraction of the screen white (0 in reference mode)
		float screenWhiteNits;
		float referenceWhiteNits;
	};
	static_assert(sizeof(SideUniforms) == 48, "SideUniforms rows are 16 bytes");
	static_assert(sizeof(Cam16Uniforms) == 112, "Cam16Uniforms rows are 16 bytes");

	struct UniformInputs
	{
		double referenceWhiteNits = 100.0;  // Lw_ref
		double screenWhiteNits = 100.0;     // Lw_screen (measured or the placeholder)
		Surround referenceSurround = kSurroundDim;   // reference mode is DIM, fixed
		Surround screenSurround = kSurroundDark;     // from the room (interpolated by the caller)
		double veilNits = 0.0;              // room light reflected by the screen, cd/m2
		double strength = 1.0;
		double adaptingFraction = 0.2;      // La = adaptingFraction x Lw (CIE usual: 20 % grey background)
		double backgroundY = 20.0;
		double whiteXyz[3] = { 95.047, 100.0, 108.883 };   // FIX1 SHOULD 3: the output white (Y = 100), from the target primaries
	};

	inline SideUniforms PackSide(const Prepared& p)
	{
		SideUniforms s = {};
		s.dc[0] = static_cast<float>(p.dc[0]);
		s.dc[1] = static_cast<float>(p.dc[1]);
		s.dc[2] = static_cast<float>(p.dc[2]);
		s.fl = static_cast<float>(p.fl);
		s.nbb = static_cast<float>(p.nbb);
		s.aw = static_cast<float>(p.aw);
		s.cz = static_cast<float>(p.c * p.z);
		s.nc = static_cast<float>(p.nc);
		s.n = static_cast<float>(p.n);
		s.tFactor = static_cast<float>(std::pow(1.64 - std::pow(0.29, p.n), 0.73));
		s.flQuarter = static_cast<float>(std::pow(p.fl, 0.25));
		return s;
	}

	inline Conditions MakeConditions(double whiteNits, const Surround& surround, double adaptingFraction, double backgroundY)
	{
		Conditions c;
		c.adaptingNits = adaptingFraction * std::max(whiteNits, 1e-6);
		c.backgroundY = backgroundY;
		c.surround = surround;
		return c;
	}

	// FIX1 SHOULD 3: the same, adapted to the output's own white (not a fixed D65).
	inline Conditions MakeConditions(double whiteNits, const Surround& surround, double adaptingFraction, double backgroundY,
		const double whiteXyz[3])
	{
		Conditions c = MakeConditions(whiteNits, surround, adaptingFraction, backgroundY);
		if (whiteXyz[1] > 0.0)
			for (int i = 0; i < 3; ++i)
				c.whiteXyz[i] = whiteXyz[i] * (100.0 / whiteXyz[1]);
		return c;
	}

	// The two viewing conditions of one frame: the reference side, and the screen side with the flare in its adapting field
	// (FIX1 MUST 5). PackUniforms and the trim x Hunt cap (FIX1 SHOULD 2) use the same two.
	inline Conditions ReferenceConditions(const UniformInputs& in)
	{
		return MakeConditions(in.referenceWhiteNits, in.referenceSurround, in.adaptingFraction, in.backgroundY, in.whiteXyz);
	}

	inline Conditions ScreenConditions(const UniformInputs& in)
	{
		Conditions screen = MakeConditions(in.screenWhiteNits, in.screenSurround, in.adaptingFraction, in.backgroundY, in.whiteXyz);
		screen.adaptingNits += std::max(0.0, in.veilNits);
		return screen;
	}

	// FIX1 MUST 5 (refs/125 s4: the veil enters the SCREEN side as flare on the inverse). The flare is part of the screen
	// viewing conditions (the eye adapts to the screen light plus the reflected room light), and the displayed light is
	// brought down by a flare TOE, never a subtract-and-clip: g(o) = o (o + f) / (o + 2 f), normalised so g(u) = u
	// (u = upper: 1.0 for SDR, 10000 / screenWhiteNits for ST.2084 PQ).
	// For o >> f, g(o) + f ~ o (the seen light keeps the intended contrast); near black g has slope 1/2 and reaches 0 only
	// at 0, so every displayed level stays distinct (no shadow crush, low-level grain kept). Monotonic on [0, u] with
	// 0 < g(o) < o < g(o) + f on (0, u); f = 0 is the identity. The shader's ap_flare (shaders/vp_cam16_appearance.glsl) is
	// this function, per channel.
	inline double FlareToDisplayed(double o, double f, double upper = 1.0)
	{
		if (!(f > 0.0))
			return o;
		o = std::max(0.0, o);
		const double u = upper > 0.0 ? upper : 1.0;
		const double g = o * (o + f) / (o + 2.0 * f);
		const double gu = (u + f) / (u + 2.0 * f);
		return g / gu;
	}

	inline Cam16Uniforms PackUniforms(const UniformInputs& in)
	{
		Cam16Uniforms u = {};
		u.reference = PackSide(Prepare(ReferenceConditions(in)));
		u.screen = PackSide(Prepare(ScreenConditions(in)));   // FIX1 MUST 5: the flare is in the screen's adapting field
		u.strength = static_cast<float>(std::min(1.0, std::max(0.0, in.strength)));
		u.screenWhiteNits = static_cast<float>(in.screenWhiteNits);
		u.referenceWhiteNits = static_cast<float>(in.referenceWhiteNits);
		u.veil = static_cast<float>(in.screenWhiteNits > 0.0 ? std::max(0.0, in.veilNits) / in.screenWhiteNits : 0.0);
		return u;
	}
}
