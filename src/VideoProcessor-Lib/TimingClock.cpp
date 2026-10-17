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
	const timingclocktime_t diff = stop - start;
	
	if (ticksPerSecond == 0)
		return 0.0;

	// Bug hunt 2 (L12): the earlier integer (diff * 1000) / ticksPerSecond
	// truncated to whole milliseconds (and overflowed for large diffs).
	// Split into whole seconds and the remainder so both parts are exact
	// before the single conversion to double: sub-millisecond latency kept.
	const timingclocktime_t seconds = diff / ticksPerSecond;
	const timingclocktime_t remainder = diff % ticksPerSecond;
	return static_cast<double>(seconds) * 1000.0 +
		static_cast<double>(remainder) * 1000.0 / static_cast<double>(ticksPerSecond);
}
