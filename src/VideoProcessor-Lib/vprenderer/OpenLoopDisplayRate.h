#pragma once

#include <DisplayRefreshRatePolicy.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

// Render-ahead without DXGI frame statistics (follow-up 2026-10-17). Some displays
// (the laptop's internal panel on the Intel iGPU, a DWM-composed window on many
// drivers) never report GetFrameStatistics, so AlphaPresentationTelemetry never
// measures the display rate and the de-judder extras held with "timing not known:
// display rate". docs/VP-render-ahead-design.md says render-ahead runs open loop
// there; this picks the display rate it plans with.
//
// Pure (no Windows calls): the renderer reads the candidates and this decides.
// Nothing is fixed: every candidate is read from the system or measured, and the
// tolerances are relative (owner [[vp-general-not-fixed]]).
namespace OpenLoopDisplayRate
{
	enum class Source
	{
		None,
		Statistics,     // AlphaPresentationTelemetry's measured rate (closed loop)
		DisplayConfig,  // the window's monitor mode, exact rational (QueryDisplayConfig)
		Dwm,            // DwmGetCompositionTimingInfo's refresh period (QPC)
		Host,           // the rate the application supplied (SetRenderLoadDisplayRefreshRate)
		ModeInteger,    // EnumDisplaySettings: whole hertz only (59 for 59.94)
		VBlankCadence   // QPC intervals between IDXGIOutput::WaitForVBlank returns
	};

	inline const char* Name(Source source)
	{
		switch (source)
		{
		case Source::Statistics: return "DXGI frame statistics";
		case Source::DisplayConfig: return "monitor mode (display config)";
		case Source::Dwm: return "DWM composition rate";
		case Source::Host: return "application display rate";
		case Source::ModeInteger: return "monitor mode (whole Hz)";
		case Source::VBlankCadence: return "vblank cadence (QPC)";
		default: return "none";
		}
	}

	// 0 (or anything outside DisplayRefreshBounds) = not available.
	struct Candidates
	{
		double statisticsHz = 0.0;
		double displayConfigHz = 0.0;
		double dwmHz = 0.0;
		double hostHz = 0.0;
		double modeIntegerHz = 0.0;
		double cadenceHz = 0.0;
	};

	struct Choice
	{
		double hz = 0.0;
		Source source = Source::None;
		bool openLoop = false;   // not from the statistics: plan without closed-loop correction
	};

	// DWM composes at one rate, which need not be the rate of the monitor the window
	// is on (mixed-rate desktops). With the monitor's own mode known, a DWM rate
	// further than this (relative) from it is another monitor's and is skipped.
	constexpr double kDwmAgreesWithMode = 0.02;

	// The statistics' rate always wins. Without it, only render-ahead (open loop)
	// takes a fallback; render_ahead off keeps the old hold (Choice{} - no rate).
	// Order: the monitor's exact mode, DWM's measured period (when it agrees with the
	// mode), the application's rate, the whole-hertz mode, the vblank cadence.
	inline Choice Select(const Candidates& c, bool renderAhead)
	{
		Choice choice;
		if (DisplayRefreshBounds::Accepts(c.statisticsHz))
		{
			choice.hz = c.statisticsHz;
			choice.source = Source::Statistics;
			return choice;
		}
		if (!renderAhead)
			return choice;
		const double mode = DisplayRefreshBounds::Accepts(c.displayConfigHz) ? c.displayConfigHz :
			DisplayRefreshBounds::Accepts(c.modeIntegerHz) ? c.modeIntegerHz : 0.0;
		const bool dwmUsable = DisplayRefreshBounds::Accepts(c.dwmHz) &&
			(!(mode > 0.0) || std::abs(c.dwmHz - mode) <= kDwmAgreesWithMode * mode);
		const struct { double hz; Source source; bool usable; } order[] = {
			{ c.displayConfigHz, Source::DisplayConfig, DisplayRefreshBounds::Accepts(c.displayConfigHz) },
			{ c.dwmHz, Source::Dwm, dwmUsable },
			{ c.hostHz, Source::Host, DisplayRefreshBounds::Accepts(c.hostHz) },
			{ c.modeIntegerHz, Source::ModeInteger, DisplayRefreshBounds::Accepts(c.modeIntegerHz) },
			{ c.cadenceHz, Source::VBlankCadence, DisplayRefreshBounds::Accepts(c.cadenceHz) } };
		for (const auto& o : order)
		{
			if (o.usable)
			{
				choice.hz = o.hz;
				choice.source = o.source;
				choice.openLoop = true;
				return choice;
			}
		}
		return choice;
	}

	// The rate change that is worth a log line (relative).
	constexpr double kLogChange = 1e-3;

	inline bool Changed(const Choice& before, const Choice& now)
	{
		return before.source != now.source ||
			std::abs(before.hz - now.hz) > kLogChange * std::max(before.hz, now.hz);
	}

	// The vblank cadence: the median interval between consecutive vblank times (s).
	// Robust to a missed or doubled wait. 0 until kMinIntervals intervals are in.
	class VBlankCadence
	{
	public:
		static constexpr size_t kMinIntervals = 8;
		static constexpr size_t kCapacity = 64;

		void Add(double vblankSeconds)
		{
			if (last > 0.0 && vblankSeconds > last)
			{
				intervals.push_back(vblankSeconds - last);
				if (intervals.size() > kCapacity)
					intervals.erase(intervals.begin());
			}
			last = vblankSeconds;
		}

		void Reset()
		{
			intervals.clear();
			last = 0.0;
		}

		size_t Count() const { return intervals.size(); }

		double Hz() const
		{
			if (intervals.size() < kMinIntervals)
				return 0.0;
			std::vector<double> sorted(intervals);
			std::nth_element(sorted.begin(), sorted.begin() + static_cast<std::ptrdiff_t>(sorted.size() / 2), sorted.end());
			const double median = sorted[sorted.size() / 2];
			const double hz = median > 0.0 ? 1.0 / median : 0.0;
			return DisplayRefreshBounds::Accepts(hz) ? hz : 0.0;
		}

	private:
		std::vector<double> intervals;
		double last = 0.0;
	};

	// Open-loop lateness by QPC (no statistics to say when a present was shown): a
	// render-ahead present whose Present call returned more than one refresh after
	// the vsync it was planned for cannot have been shown on time. One refresh, not
	// half: the open-loop grid may be up to a refresh away from the real vsyncs.
	// Returns the whole refreshes the chain is behind (0 = on time).
	inline int LateRefreshes(double presentReturned, double plannedVsync, double displayPeriod)
	{
		if (!(displayPeriod > 0.0) || !(plannedVsync > 0.0))
			return 0;
		const double error = presentReturned - plannedVsync;
		if (!(error > displayPeriod * (1.0 + 1e-9)))   // exactly one refresh (rounding) is on time
			return 0;
		return static_cast<int>(std::ceil(error / displayPeriod - 1e-9));
	}
}
