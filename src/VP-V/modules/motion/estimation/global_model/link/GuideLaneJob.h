// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// Camera link (K4-2): the guide LANE job. The guide of pair N (p7g_camera_guide) is a pure function of the d-th PREVIOUS record P7, its field copy
// and P4(N), so the camera worker can make it ahead of the render: right after that fit, once P4(N) is published. The live fill
// is the render thread's own GuidanceScheduler::Guide(N); both run the SAME unit (BuildGuide), so their results are identical
// (GuideProductTests, GuidanceWiringTests pin it byte for byte).
//   OnFitDone(seq)   the camera worker, after the record of `seq` was published or found late: the NEXT pair (the first pair after the
//                    newest requested record, from the host's NextPair hint: the first P4-published seq after it) becomes WANTED.
//                    With gaps in the numbering the hint can name a seq that is not a pair (a repeat): that lane result is simply
//                    never asked for (the render computes the real pair live); it can never be WRONG, because the lane stores only
//                    what the render would compute for that very seq from the same record, thresholds and switches.
//   Pump()           tries every wanted pair through GuidanceScheduler::TryPublishLane:
//                      Published  stored for the render (the lane fill)
//                      NotReady   the record or P4(N) is not there yet: stays wanted, asked again at the next Pump
//                      Discarded  the render asked first (its live fill stands; the late lane result is counted and dropped)
//                      Off        no switch is on: forgotten
// The worker calls Pump() after every OnFitDone; the render thread may call it once per frame (FrameStart) as the "P4(N) arrived"
// trigger. Only ONE thread pumps at a time (a second concurrent call returns 0), so a pair is never published twice.
// The wanted set is bounded; the oldest entry is dropped and counted (visible), never waited for. d is read from the scheduler's
// worker-side snapshot (never from render-thread state). Nothing here allocates a thread or a product: with all switches off
// OnFitDone() adds nothing (LaneWanted() is false) and the job costs one locked branch.
// C++14, deterministic apart from WHEN a pump runs (never WHAT it computes).
#pragma once

#include "GuidanceScheduler.h"

#include <cstdint>
#include <functional>
#include <mutex>
#include <vector>

namespace CameraLink
{
	class GuideLaneJob
	{
	public:
		struct Stats
		{
			uint64_t wanted = 0, published = 0, retried = 0, discarded = 0, dropped = 0;
		};

		GuideLaneJob(GuidanceScheduler& scheduler, ICameraLink& link, size_t capacity = 16)
			: m_sched(scheduler), m_link(link), m_cap(capacity < 2 ? 2 : capacity) {}

		// The first pair published after `afterSeq` (P4 published), false when there is none yet. Default: afterSeq + 1.
		void SetNextPairHint(std::function<bool(uint64_t afterSeq, uint64_t& next)> f);

		void OnFitDone(uint64_t fitSeq);
		// Returns the number of pairs stored this call.
		size_t Pump();
		// A discontinuity: wanted pairs belong to the old numbering.
		void Reset();
		size_t Pending() const;
		Stats GetStats() const;

	private:
		void WantNext();
		GuidanceScheduler& m_sched;
		ICameraLink& m_link;
		size_t m_cap;
		std::function<bool(uint64_t, uint64_t&)> m_next;   // under m_mutex
		mutable std::mutex m_mutex;   // m_wanted, m_stats
		std::mutex m_pumpMutex;       // one pumper at a time
		std::vector<uint64_t> m_wanted;
		uint64_t m_lastWanted = ~uint64_t(0);
		Stats m_stats;
	};
}
