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
	// mode), the application's rate, the vblank cadence, the whole-hertz mode.
	// Hunt 2 05-renderahead M5 (queue_timing_fixes, wholeHzLast): the whole-hertz
	// mode (59 for 59.94, a 1.6% error: a repeat or a lateness about every second)
	// is taken only when nothing better exists; it used to rank above the measured
	// vblank cadence. wholeHzLast false: the old order.
	inline Choice Select(const Candidates& c, bool renderAhead, bool wholeHzLast = true)
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
			{ wholeHzLast ? c.cadenceHz : c.modeIntegerHz, wholeHzLast ? Source::VBlankCadence : Source::ModeInteger,
				DisplayRefreshBounds::Accepts(wholeHzLast ? c.cadenceHz : c.modeIntegerHz) },
			{ wholeHzLast ? c.modeIntegerHz : c.cadenceHz, wholeHzLast ? Source::ModeInteger : Source::VBlankCadence,
				DisplayRefreshBounds::Accepts(wholeHzLast ? c.modeIntegerHz : c.cadenceHz) } };
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

	// The display's real refresh period from DWM's vblank times (hunt 2 05-renderahead
	// M5, queue_timing_fixes). Every open-loop rate is a mode or a nominal period, ppm
	// (or, for a whole-hertz mode, 1.6%) from the display's real refresh: a chain planned
	// with it drifts until a lateness correction moves it a refresh (about every 170 s
	// at 100 ppm and 60 Hz) - a dropped extra each time. DWM stamps each vblank on QPC;
	// a least-squares line through (refresh index, vblank time), exponentially weighted
	// over kWindowSeconds, gives the real period. The refresh index between two samples
	// is counted with the nominal rate (samples at most kMaximumGapSeconds apart, so a
	// 2% nominal error cannot miscount). Refine returns the measured rate when it agrees
	// with the planned one (kDwmAgreesWithMode: the same display), else the planned.
	class VBlankGridRate
	{
	public:
		static constexpr double kWindowSeconds = 60.0;
		static constexpr double kMinimumSpanSeconds = 10.0;
		static constexpr double kMaximumGapSeconds = 0.25;

		void Reset() { *this = VBlankGridRate(); }

		void Add(double vblankSeconds, double nominalHz)
		{
			if (!DisplayRefreshBounds::Accepts(nominalHz) || !(vblankSeconds > 0.0) || !std::isfinite(vblankSeconds))
				return;
			if (started && vblankSeconds == last)
				return;   // the same vblank read again
			if (started && (vblankSeconds < last || vblankSeconds - last > kMaximumGapSeconds ||
				std::abs(nominalHz - nominal) > kDwmAgreesWithMode * nominal))
				Reset();  // a step back, a gap, or another display rate: start over
			if (!started)
			{
				started = true;
				t0 = vblankSeconds;
				index = 0.0;
				nominal = nominalHz;
			}
			else
			{
				const double periods = std::floor((vblankSeconds - last) * nominalHz + 0.5);
				if (periods < 1.0)
					return;   // a vblank read twice within one refresh
				index += periods;
			}
			const double keep = last > 0.0 ? std::exp(-(vblankSeconds - last) / kWindowSeconds) : 1.0;
			last = vblankSeconds;
			const double x = index;
			const double y = vblankSeconds - t0;
			w = w * keep + 1.0;
			sx = sx * keep + x;
			sy = sy * keep + y;
			sxx = sxx * keep + x * x;
			sxy = sxy * keep + x * y;
		}

		// 0 until kMinimumSpanSeconds of vblanks are in.
		double Hz() const
		{
			if (!started || last - t0 < kMinimumSpanSeconds || w < 3.0)
				return 0.0;
			const double mx = sx / w, my = sy / w;
			const double vxx = sxx / w - mx * mx;
			const double vxy = sxy / w - mx * my;
			if (!(vxx > 0.0) || !(vxy > 0.0))
				return 0.0;
			const double hz = vxx / vxy;
			return DisplayRefreshBounds::Accepts(hz) ? hz : 0.0;
		}

		double Refine(double plannedHz) const
		{
			const double hz = Hz();
			return hz > 0.0 && plannedHz > 0.0 && std::abs(hz - plannedHz) <= kDwmAgreesWithMode * plannedHz ?
				hz : plannedHz;
		}

	private:
		bool started = false;
		double t0 = 0.0, last = 0.0, index = 0.0, nominal = 0.0;
		double w = 0.0, sx = 0.0, sy = 0.0, sxx = 0.0, sxy = 0.0;
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
