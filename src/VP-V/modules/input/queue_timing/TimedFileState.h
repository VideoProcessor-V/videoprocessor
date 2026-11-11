// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// M2 P10 R20 (r2/p10): the file thread's own state of the timed file frames (TimedFileFrames.inl), moved out of the
// renderer's header (it held three loose members). Touched only by OnTimedVideoFrame, i.e. the file source's streaming
// thread (the reject counter is atomic as it was); nothing else reads it. The slot gate (FileFrameSlotGate) stays a
// renderer member: the render loop and the retire paths call it. C++14.

#include <atomic>
#include <cstdint>

namespace Vpv
{
	namespace QueueTiming
	{
		struct TimedFileState
		{
			std::atomic<uint64_t> rejected{0};   // frames refused for an unusable timing block (logged on powers of two)
			double loggedFilmHz = -1.0;          // the file film clock as last logged
			bool loggedMotionOff = false;
		};
	}
}
