/*
 * Copyright(C) 2021 Dennis Fleurbaaij <mail@dennisfleurbaaij.com>
 *
 * This program is free software: you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 3.
 * This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.
 * You should have received a copy of the GNU General Public License along with this program. If not, see < https://www.gnu.org/licenses/>.
 */


#include <pch.h>

#include "TimingClock.h"


double TimingClockDiffMs(timingclocktime_t start, timingclocktime_t stop, timingclocktime_t ticksPerSecond)
{
	// Hunt 2 13-timing L2: the result keeps its sub-millisecond part (the integer
	// division used to truncate every latency readout toward zero, about 0.5 ms
	// low on average), and a clock without a rate (0 or negative ticks per second)
	// gives 0 instead of an integer division by zero. Whole seconds and the
	// remainder are converted separately, so no tick is lost and diff * 1000 can
	// never overflow.
	if (ticksPerSecond <= 0)
		return 0.0;
	const timingclocktime_t diff = stop - start;
	const timingclocktime_t wholeSeconds = diff / ticksPerSecond;
	const timingclocktime_t remainderTicks = diff % ticksPerSecond;
	return static_cast<double>(wholeSeconds) * 1000.0 +
		static_cast<double>(remainderTicks) * 1000.0 / static_cast<double>(ticksPerSecond);
}
