#pragma once

#include <cstdint>

// Bug hunt 2026-10-17: the capture timestamp step between two delivered
// frames, classified the way UpdateFrameRateAndPPM treats it. Pure, so the
// rules are unit-tested without a capture card. Nothing here assumes a rate:
// the expected step is the measured/nominal frame period in clock ticks.
//
// - Normal: within half a period of one period.
// - Missed: 2 to 8 whole periods (within a quarter period): frames the
//   capture missed; the cadence estimate is kept and the frames are counted.
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
		long long periods = 1;   // whole periods (Missed: 2..8)
		long long missed = 0;    // frames missed (Missed only)
	};

	inline Step Classify(long long observedTicks, long long expectedFrameTicks)
	{
		Step step;
		if (expectedFrameTicks <= 0)
			return step;   // no expectation yet: not judged
		const long long periods = observedTicks > 0 ?
			(observedTicks + expectedFrameTicks / 2) / expectedFrameTicks : 0;
		const long long remainder = observedTicks - periods * expectedFrameTicks;
		step.periods = periods;
		if (periods >= 2 && periods <= 8 && remainder < expectedFrameTicks / 4 && remainder > -expectedFrameTicks / 4)
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
}
