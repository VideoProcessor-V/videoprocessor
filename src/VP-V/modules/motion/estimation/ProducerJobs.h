// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// Producer throughput plan (refs\105 s3.3, phase 1a): the job classes the ONE NVOF session serves and the pure chooser that
// picks the next one. CorePair = the look-ahead walk's pairs, OnDemand = C5b requests, LongPair = the TD long pair (the queue
// exists, nothing fills it yet). Pure CPU, no state, header only, C++14.
//
// Order (design-intent register 115 #57; owner 2026-11-11 07:10, owner-decisions-2026-11-07.md:388; doc 99 s12 item 11):
// core pair > on-demand request > long pair. A waiting core pair always goes first (the core precompute set is never shed
// for time, 115 #56); a request next; a long pair only when neither waits, so it NEVER starts while a request waits.
// refs\105 O1's recommendation (a request before a core pair unless the pair's slack is short) has no owner answer and is
// not built (coordinator ruling 2026-11-15, lead 17 C4). A core pair chosen while a request waited is flagged
// (`requestWaited`): the producer counts it, so a request's wait behind the walk is visible.
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

		struct Choice
		{
			bool any = false;
			JobClass cls = JobClass::CorePair;
			bool requestWaited = false;   // a core pair ran although a request waited (counted as coreAheadOfRequests)
		};

		inline Choice NextJob(const Queues& queues)
		{
			Choice choice;
			if (!queues.Any())
				return choice;
			choice.any = true;
			if (queues.corePairs != 0)
			{
				choice.cls = JobClass::CorePair;
				choice.requestWaited = queues.onDemand != 0;
				return choice;
			}
			choice.cls = queues.onDemand != 0 ? JobClass::OnDemand : JobClass::LongPair;
			return choice;
		}
	}
}
