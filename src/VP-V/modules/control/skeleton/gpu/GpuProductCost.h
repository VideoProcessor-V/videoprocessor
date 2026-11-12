// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// Measured cost of one GPU lane product (P10 grain split, P11, P12) as the dispatcher times it: every pass of the
// product's kernel records one pl_timer result (device ns); the dispatcher adds each result here and counts the frames
// it completed. mean ms per frame = summed device ns / frames, so it covers ALL passes of a frame (the earlier
// `lastNs` kept only the last single pass). Read-only statistics: nothing branches on them except the status line.
// Render thread only (the dispatcher's thread).

#include <cstdint>
#include <cstdio>
#include <string>

namespace Vpv
{
	namespace Skeleton
	{
		struct GpuProductCost
		{
			uint64_t frames = 0;         // frames completed (lane: last chunk; live: all steps)
			uint64_t timedFrames = 0;    // frames completed at the last timer poll (the ratio's denominator)
			uint64_t results = 0;        // timer results received (one per timed pass)
			uint64_t totalNs = 0;        // sum of every result
			uint64_t maxPassNs = 0;      // worst single pass
			uint64_t frameNsAccum = 0;   // results since the previous frame boundary
			uint64_t maxFrameNs = 0;     // worst accumulated frame (a frame's results can straddle a poll: indicative)

			void NoteResult(uint64_t ns)
			{
				++results;
				totalNs += ns;
				frameNsAccum += ns;
				if (ns > maxPassNs)
					maxPassNs = ns;
			}
			// Called after each poll: the frames done so far are the divisor (results lag the passes by up to a frame or two).
			void NotePoll()
			{
				timedFrames = frames;
			}
			void NoteFrameDone()
			{
				++frames;
				if (frameNsAccum > maxFrameNs)
					maxFrameNs = frameNsAccum;
				frameNsAccum = 0;
			}
			double MeanFrameMs() const { return timedFrames == 0 ? 0.0 : double(totalNs) / 1.0e6 / double(timedFrames); }
			double MaxPassMs() const { return double(maxPassNs) / 1.0e6; }
			double MaxFrameMs() const { return double(maxFrameNs) / 1.0e6; }
		};

		// One status-line text: "GPU cost P10 grain split: mean 3.21 ms/frame over 240 frames, worst pass 1.10 ms, worst frame 5.40 ms".
		// Empty until the first timed frame (the timer reports a frame or two late).
		inline std::string FormatCostLine(const char* name, const GpuProductCost& c)
		{
			if (c.timedFrames == 0 || c.results == 0)
				return std::string();
			char buf[200] = {};
			std::snprintf(buf, sizeof(buf), "GPU cost %s: mean %.2f ms/frame over %llu frames, worst pass %.2f ms, worst frame %.2f ms",
				name, c.MeanFrameMs(), static_cast<unsigned long long>(c.timedFrames), c.MaxPassMs(), c.MaxFrameMs());
			return std::string(buf);
		}
	}
}
