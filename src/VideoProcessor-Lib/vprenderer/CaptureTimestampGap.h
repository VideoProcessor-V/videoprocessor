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
