// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// Producer throughput plan (refs\105 s3.3, phase 1a): the job classes the ONE NVOF session serves and the pure chooser that
// picks the next one. CorePair = the look-ahead walk's pairs, OnDemand = C5b requests, LongPair = the TD long pair (the queue
// exists, nothing fills it yet). Pure CPU, no state of its own beyond the caller's ChooserState, header only, C++14.
//
// Order (105 O1): a core pair whose deadline slack is under two service times goes first; otherwise on-demand requests go first,
// but at most `maxRequestStreak` in a row WHILE a core job waits (the walk is never starved); a long pair only when no
// request and no core pair waits - it NEVER overtakes a waiting request (O1: "long pair last, never while a request waits"), so
// only a core pair may break a request streak. The streak counts only choices made while a core job waited: requests served on an idle walk do
// not use it up (the C5b dequeue-stall fix - the old counter ran up on an idle walk, so the first job to arrive overtook a
// waiting request at once and a request burst after it lost its priority).
#pragma once

#include <cstddef>

namespace MotionAhead
{
	namespace ProducerJobs
	{
		enum class JobClass { CorePair, OnDemand, LongPair };

		struct Queues
		{
			size_t corePairs = 0, onDemand = 0, longPairs = 0;
			bool Any() const { return corePairs + onDemand + longPairs != 0; }
		};

		// The caller's state, kept under its own lock: requests chosen in a row while a core job waited.
		struct ChooserState { int requestStreak = 0; };

		struct Choice
		{
			bool any = false;
			JobClass cls = JobClass::CorePair;
			bool fairness = false;   // a core job ran although a request waited (the streak bound; counted as fairnessJobs)
			bool urgent = false;     // a core job ran first because its slack was under two service times
		};

		// coreSlackMs < 0 or serviceMs <= 0 = unknown (no urgency); else urgent when slack < 2 x service.
		inline bool CoreIsUrgent(double coreSlackMs, double serviceMs)
		{
			return coreSlackMs >= 0.0 && serviceMs > 0.0 && coreSlackMs < 2.0 * serviceMs;
		}

		inline Choice NextJob(const Queues& queues, ChooserState& state, int maxRequestStreak, double coreSlackMs = -1.0, double serviceMs = 0.0)
		{
			Choice choice;
			if (!queues.Any())
				return choice;
			choice.any = true;
			// Only a core pair may overtake a waiting request (O1); a long pair never does, so it does not count as "waiting" here.
			const bool otherWaits = queues.corePairs != 0;
			if (queues.onDemand != 0)
			{
				const bool urgentCore = queues.corePairs != 0 && CoreIsUrgent(coreSlackMs, serviceMs);
				if (!urgentCore && (!otherWaits || state.requestStreak < maxRequestStreak))
				{
					choice.cls = JobClass::OnDemand;
					if (otherWaits)
						++state.requestStreak;
					return choice;
				}
				// A core job overtakes a waiting request: by urgency, or because the streak bound is reached (otherWaits = a core pair
				// is queued, so this branch always has one).
				choice.cls = JobClass::CorePair;
				choice.urgent = urgentCore;
				choice.fairness = !urgentCore;
				state.requestStreak = 0;
				return choice;
			}
			state.requestStreak = 0;
			choice.cls = queues.corePairs != 0 ? JobClass::CorePair : JobClass::LongPair;
			return choice;
		}
	}
}
