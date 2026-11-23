// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <algorithm>

// ONE set of clamp / ramp / smoothstep / mix helpers (rule 20, ND07). Private copies existed in the light-level
// LUT, dither need, region evidence, source-identity vote, step detector, global-model fit, grain split, temporal
// detail and texture restore (DetailPulse called Bill's GrainPreserve::Smoothstep); they differ only in small rules,
// kept as explicit axes so every site stays bit-identical:
//   * Clamp01 passes NaN through (as the template copies did); Clamp01NanToZero maps NaN to 0 (the vote / step copies).
//   * Smoothstep has THREE edge rules (SmoothRule):
//     - StepWhenEqual: e0 == e1 is a step at e0; a reversed e1 < e0 ramps downwards (the light-level LUT rule);
//     - StepWhenNotAscending: any e1 <= e0 is a step at e0 (the dither-need rule, the default);
//     - Unguarded: no edge guard, the ONE no-guard smoothstep of TemporalDetail, TextureRestore and DetailPulse;
//       exactly Bill's GrainPreserve::Smoothstep law, t = max(0, min(1, q)), so a NaN ratio q gives t = 1, never NaN.
// The two guarded Smoothstep rules are a registered rule-20 VARIANT (unifying changes a reversed edge in the light-level LUT: a FIX, deferred).
// Unguarded is bit-identical to Bill's formula for every input and to the old ternary-clamp copies for every non-NaN q
// (C6-1 U25 ruling 5b: NaN-safe, DetailPulse output unchanged; NaN never propagates toward pixels).
// The GLSL built-in smoothstep is untouched.

#include <cmath>

namespace RampMath
{
	template <class T> inline T Clamp01(T v) { return v < T(0) ? T(0) : (v > T(1) ? T(1) : v); }

	// NaN -> 0 (comparisons false), otherwise the same clamp.
	template <class T> inline T Clamp01NanToZero(T x) { return x > T(0) ? (x < T(1) ? x : T(1)) : T(0); }

	// Linear ramp lo..hi; a degenerate ramp (hi <= lo) is a step at hi (never NaN).
	template <class T> inline T Ramp(T v, T lo, T hi) { return hi > lo ? Clamp01((v - lo) / (hi - lo)) : (v >= hi ? T(1) : T(0)); }

	enum class SmoothRule { StepWhenEqual, StepWhenNotAscending, Unguarded };

	template <class T> inline T Smoothstep(T e0, T e1, T x, SmoothRule rule = SmoothRule::StepWhenNotAscending)
	{
		if (rule == SmoothRule::Unguarded)
		{
			// Argument order matters: (std::min)(T(1), NaN) returns T(1), so a NaN ratio clamps to 1 (Bill's law).
			const T t = (std::max)(T(0), (std::min)(T(1), (x - e0) / (e1 - e0)));
			return t * t * (T(3) - T(2) * t);
		}
		if (rule == SmoothRule::StepWhenEqual ? (e0 == e1) : !(e1 > e0))
			return x >= e0 ? T(1) : T(0);
		const T t = Clamp01((x - e0) / (e1 - e0));
		return t * t * (T(3) - T(2) * t);
	}

	template <class T> inline T Mix(T a, T b, T t) { return a + (b - a) * t; }

	// Gate forms that CLOSE on non-finite evidence (weight 0 in both the S and the 1 - S form). The comparison-based
	// Smoothstep would pass NaN through the S form and open the 1 - S form on it. Moved here from temporal detail
	// (TD-NONFINITE; gate cb-u1 r1 SHOULD 4: one owner, U25). For ascending edges (every caller) this is the same
	// arithmetic as the unguarded temporal-detail Smoothstep, so the finite path is unchanged.
	template <class T> inline T GateS(T e0, T e1, T x) { return std::isfinite(x) ? Smoothstep(e0, e1, x) : T(0); }
	template <class T> inline T GateInv(T e0, T e1, T x) { return std::isfinite(x) ? T(1) - Smoothstep(e0, e1, x) : T(0); }
}
