// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// ONE set of clamp / ramp / smoothstep / mix helpers (rule 20, ND07). Private copies existed in the light-level
// LUT, dither need, region evidence, source-identity vote, step detector and global-model fit; they differ only in
// two small rules, kept as explicit axes so every site stays bit-identical:
//   * Clamp01 passes NaN through (as the template copies did); Clamp01NanToZero maps NaN to 0 (the vote / step copies).
//   * Smoothstep's degenerate edge: StepWhenEqual (e0 == e1 is a step at e0; a reversed e1 < e0 ramps downwards)
//     or StepWhenNotAscending (any e1 <= e0 is a step at e0, the dither-need rule).
// The two Smoothstep rules are a registered rule-20 VARIANT (unifying changes a reversed edge in the light-level LUT: a FIX, deferred).
// The temporal-detail Smoothstep (no guard) is owned by L-B2 and is not routed here.
// The GLSL built-in smoothstep is untouched.

namespace RampMath
{
	template <class T> inline T Clamp01(T v) { return v < T(0) ? T(0) : (v > T(1) ? T(1) : v); }

	// NaN -> 0 (comparisons false), otherwise the same clamp.
	template <class T> inline T Clamp01NanToZero(T x) { return x > T(0) ? (x < T(1) ? x : T(1)) : T(0); }

	// Linear ramp lo..hi; a degenerate ramp (hi <= lo) is a step at hi (never NaN).
	template <class T> inline T Ramp(T v, T lo, T hi) { return hi > lo ? Clamp01((v - lo) / (hi - lo)) : (v >= hi ? T(1) : T(0)); }

	enum class SmoothRule { StepWhenEqual, StepWhenNotAscending };

	template <class T> inline T Smoothstep(T e0, T e1, T x, SmoothRule rule = SmoothRule::StepWhenNotAscending)
	{
		if (rule == SmoothRule::StepWhenEqual ? (e0 == e1) : !(e1 > e0))
			return x >= e0 ? T(1) : T(0);
		const T t = Clamp01((x - e0) / (e1 - e0));
		return t * t * (T(3) - T(2) * t);
	}

	template <class T> inline T Mix(T a, T b, T t) { return a + (b - a) * t; }
}
