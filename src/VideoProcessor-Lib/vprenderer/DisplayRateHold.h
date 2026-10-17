#pragma once

#include <cmath>

// Bug hunt 2026-10-17 (timing_continuity_fixes): the display rate the
// interpolation timeline schedules at. AlphaPresentationTelemetry's cadence
// reset (a disjoint or unavailable statistics sample) zeroes the measured
// rate for a few seconds; falling back to the monitor's nominal rate then
// moves the rate by more than the timeline's 0.1% restart threshold on a
// fractional display (119.88 measured vs 120 nominal) - one restart on the
// fallback and another when the measurement returns. The hold keeps the last
// good measurement while the measurement is 0, and falls back to the
// monitor's rate only when nothing was measured since start or since a mode
// change (the monitor's rate moving, or the held rate no longer near it).
// Pure; nothing fixed (the tolerances are relative).
class DisplayRateHold
{
public:
	// A mode change: the monitor's rate moved by more than this (relative).
	static constexpr double kModeChange = 1e-3;
	// A held rate further than this from the monitor's is not this mode's.
	static constexpr double kPlausible = 0.01;

	// measured: the telemetry's rate (0 = not measured now); monitor: the
	// monitor's nominal rate (0 = not known); hold: the fix is on.
	double Update(double measured, double monitor, bool hold)
	{
		if (monitor > 0.0 && m_monitor > 0.0 && std::abs(monitor - m_monitor) > kModeChange * m_monitor)
			m_held = 0.0;   // a mode change: the old measurement is not this mode's
		if (monitor > 0.0)
			m_monitor = monitor;
		if (measured > 0.0)
		{
			m_held = measured;
			return measured;
		}
		if (hold && m_held > 0.0 && (!(monitor > 0.0) || std::abs(m_held - monitor) <= kPlausible * monitor))
			return m_held;
		return monitor;
	}

	void Reset()
	{
		m_held = 0.0;
		m_monitor = 0.0;
	}

	double Held() const { return m_held; }

private:
	double m_held = 0.0;
	double m_monitor = 0.0;
};
