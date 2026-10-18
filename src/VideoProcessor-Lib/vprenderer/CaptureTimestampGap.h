#pragma once

#include <cstdint>

// Bug hunt 2026-10-17: the capture timestamp step between two delivered
// frames, classified the way UpdateFrameRateAndPPM treats it. Pure, so the
// rules are unit-tested without a capture card. Nothing here assumes a rate:
// the expected step is the measured/nominal frame period in clock ticks.
//
// - Normal: within half a period of one period.
// - Missed: 2 or more whole periods (within a quarter period) spanning at most
//   kMissedMaxSeconds: frames the capture missed; the cadence estimate is kept
//   and the frames are counted. Post-merge audit 2026-10-17: defined by time, so
//   the same loss behaves alike at every rate (before, 2-8 periods: 67 ms at 120p
//   but 333 ms at 24p). Without the clock's rate (ticksPerSecond 0): 2 to 8 periods.
// - Break: anything else (a backward or zero step, a jump of 9 or more
//   periods, a step between whole periods): the cadence estimate restarts.
//   With timing_continuity_fixes this is also a gap: no motion pair, history
//   or film clock carries across it (before, only Missed was a gap, so a long
//   or odd jump paired frames seconds apart and warped them ~10x).
namespace CaptureTimestampGap
{
	enum class Kind { Normal, Missed, Break };

	struct Step
	{
		Kind kind = Kind::Normal;
		long long periods = 1;   // whole periods (Missed: 2 or more)
		long long missed = 0;    // frames missed (Missed only)
	};

	// The longest loss counted as missed frames: 8 periods of 23.976 Hz film
	// (333.7 ms), the old count's span at the lowest common rate.
	constexpr double kMissedMaxSeconds = 0.34;
	constexpr long long kMissedMaxPeriodsUntimed = 8;

	inline Step Classify(long long observedTicks, long long expectedFrameTicks, long long ticksPerSecond = 0)
	{
		Step step;
		if (expectedFrameTicks <= 0)
			return step;   // no expectation yet: not judged
		const long long periods = observedTicks > 0 ?
			(observedTicks + expectedFrameTicks / 2) / expectedFrameTicks : 0;
		const long long remainder = observedTicks - periods * expectedFrameTicks;
		step.periods = periods;
		const bool withinSpan = ticksPerSecond > 0 ?
			static_cast<double>(periods * expectedFrameTicks) <= kMissedMaxSeconds * static_cast<double>(ticksPerSecond) :
			periods <= kMissedMaxPeriodsUntimed;
		if (periods >= 2 && withinSpan && remainder < expectedFrameTicks / 4 && remainder > -expectedFrameTicks / 4)
		{
			step.kind = Kind::Missed;
			step.missed = periods - 1;
		}
		else if (observedTicks <= 0 || observedTicks > expectedFrameTicks * 3 / 2 ||
			observedTicks < expectedFrameTicks / 2)
			step.kind = Kind::Break;
		return step;
	}

	// The dequeue's defence for an ordinary (not gap-fill) pair A, B whose
	// times are both known: B follows A by at most 1.5 periods of the pair's
	// rate. Unknown times or rate: not judged (true).
	inline bool AdjacentPairTimes(double timeA, double timeB, double pairRateHz)
	{
		if (!(timeA > 0.0) || !(timeB > 0.0) || !(pairRateHz > 0.0))
			return true;
		return timeB > timeA && timeB - timeA <= 1.5 / pairRateHz;
	}
	// Post-merge audit 2026-10-17: with each time's clock. A frame's time is the film
	// clock (QPC arrival) while pulldown is locked and the capture clock otherwise;
	// across a lock change the two are different clocks, so the pair is not judged.
	inline bool AdjacentPairTimes(double timeA, bool filmClockA, double timeB, bool filmClockB, double pairRateHz)
	{
		if (filmClockA != filmClockB)
			return true;
		return AdjacentPairTimes(timeA, timeB, pairRateHz);
	}
}

// Q1 (queue stress 2026-10-18, queue_timing_fixes): a delivered frame's arrival on the QPC
// clock. OnVideoFrame reads the capture clock and QPC back to back, so a frame's hardware
// age is known at the callback. A fresh frame (younger than kFreshAgeSeconds) arrives at its
// hardware time, as before. A stale frame (a capture that stalled, then delivered its held
// frames at once) used to arrive at the callback, late by up to the stall: the film clock
// anchored on it and stepped back ~2 s later inside the lock. With the fixes a stale frame
// keeps its hardware time when that is plausible: its age is known, its capture timestamp
// continues the previous frame's (no break), and its hardware time does not run before the
// previous one's in a chain that began at a fresh frame (so a timing clock on another base,
// old on every frame, is never trusted). An implausible stale frame keeps the callback time.
// The first stale frame of a run, and the first fresh frame after a run with callback times,
// are delivery discontinuities: the caller marks them no-fill gaps (never a gap-fill pair
// across the stall) after the pulldown detector, so the cadence lock is kept.
// Times in seconds on one steady clock. Nothing here assumes a rate.
namespace CaptureArrival
{
	constexpr double kFreshAgeSeconds = 0.5;

	enum class Kind { Callback, Fresh, StaleHardware, StaleCallback };

	struct Result
	{
		Kind kind = Kind::Callback;
		double arrival = 0.0;
		bool staleStart = false;   // the first stale frame of a run (fixes only)
		bool staleEnd = false;     // the first fresh frame after a run with callback times (fixes only)
		bool Hardware() const { return kind == Kind::Fresh || kind == Kind::StaleHardware; }
	};

	class Tracker
	{
	public:
		void Reset() { *this = Tracker(); }

		// callback: when the callback runs; hardware: callback - age (ageKnown: the capture
		// clock and its mapping were read; the age may be anything); continuous: the
		// capture timestamp step from the previous frame is not a break.
		Result Next(double callback, bool ageKnown, double hardware, bool continuous, bool fixes)
		{
			Result r;
			r.arrival = callback;
			const double age = callback - hardware;
			bool stale = false;
			if (!ageKnown || !(age >= 0.0))
				m_chain = false;
			else if (age < kFreshAgeSeconds)
			{
				r.kind = Kind::Fresh;
				r.arrival = hardware;
				m_chain = true;
				m_lastHardware = hardware;
			}
			else
			{
				stale = true;
				if (fixes && m_chain && continuous && hardware >= m_lastHardware)
				{
					r.kind = Kind::StaleHardware;
					r.arrival = hardware;
					m_lastHardware = hardware;
				}
				else
				{
					r.kind = Kind::StaleCallback;
					m_chain = false;
				}
			}
			if (fixes)
			{
				r.staleStart = stale && !m_inStale;
				r.staleEnd = !stale && m_runHadCallback;
			}
			m_runHadCallback = stale && (m_runHadCallback || r.kind == Kind::StaleCallback);
			m_inStale = stale;
			if (r.kind == Kind::StaleHardware) ++m_staleHardware;
			if (r.kind == Kind::StaleCallback) ++m_staleCallback;
			return r;
		}

		unsigned long long StaleHardware() const { return m_staleHardware; }
		unsigned long long StaleCallback() const { return m_staleCallback; }

	private:
		bool m_chain = false, m_inStale = false, m_runHadCallback = false;
		double m_lastHardware = 0.0;
		unsigned long long m_staleHardware = 0, m_staleCallback = 0;
	};
}
