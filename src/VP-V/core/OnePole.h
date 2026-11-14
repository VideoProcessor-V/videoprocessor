// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <GeneralRate.h>   // Bill's header, read only: PerFrameWeight

#include <cmath>

// ONE one-pole time-constant smoother (rule 20, ND05). e += alpha (target - e), alpha = 1 - exp(-dt / tau).
//
// Nine private copies of this law existed (dither need, ambient adaptation, light-level blocks, queue
// rules, LED settle, ...). They differ only in AXES, which are parameters here, not copies:
//   - unit of dt and tau: seconds or milliseconds (AlphaFor / AlphaForMs; dt and tau must share the unit);
//   - dt source: a MEASURED interval (AlphaFor) or one frame at a rate (AlphaPerFrame / PerFrameAt);
//   - a dt of 0 or less: "unknown, no smoothing" (alpha 1, the dither-need rule) or "as given" (the
//     caller already clamped dt to >= 0; dt 0 holds, alpha 0). DtRule selects it.
// A time constant of 0 or less (or NaN) is "no smoothing" (alpha 1) everywhere.
//
// PerFrameAt wraps GeneralRate::PerFrameWeight, the repo's rate rule (a weight tuned per frame at the 60 Hz
// reference keeps its TIME above it).
namespace OnePole
{
	enum class DtRule
	{
		UnknownIsNoSmoothing,   // dt <= 0 or NaN: alpha 1 (no smoothing)
		AsGiven                 // dt used as is (0 holds, alpha 0); the caller guarantees dt >= 0
	};

	// The smoothing coefficient of one interval dt for time constant tau (same unit).
	inline double AlphaFor(double dt, double tau, DtRule rule = DtRule::UnknownIsNoSmoothing)
	{
		if (!(tau > 0.0)) return 1.0;
		if (rule == DtRule::UnknownIsNoSmoothing && !(dt > 0.0)) return 1.0;
		return 1.0 - std::exp(-dt / tau);
	}

	// The same law with dt and tau in milliseconds (the ratio is unit free; the name records the unit).
	inline double AlphaForMs(double dtMs, double tauMs, DtRule rule = DtRule::UnknownIsNoSmoothing)
	{
		return AlphaFor(dtMs, tauMs, rule);
	}

	// The coefficient of ONE frame at rateHz for time constant tauSeconds: 1 - exp(-1 / (tau * rate)).
	inline double AlphaPerFrame(double tauSeconds, double rateHz)
	{
		if (!(tauSeconds > 0.0)) return 1.0;
		return 1.0 - std::exp(-1.0 / (tauSeconds * rateHz));
	}

	// The per-frame weight for time constant tauSeconds tuned at the 60 Hz reference and held in TIME above it
	// (GeneralRate::PerFrameWeight). rateHz unknown or <= the reference: the reference weight as it stands.
	inline double PerFrameAt(double tauSeconds, double rateHz, bool general = true)
	{
		return GeneralRate::PerFrameWeight(AlphaPerFrame(tauSeconds, GeneralRate::kFrameCountReferenceHz), rateHz, general);
	}
}
