#pragma once

// active_picture_lookahead_frames follows the queue (owner decision 2026-10-17,
// the same "queue = look-ahead" rule as Dynamic Drive). Pure, CPU-tested.
//
// - An explicit value (the active queue profile, the default queue section or the
//   command line) always wins, an explicit 0 included.
// - Absent everywhere: the normal queue looks ahead over what it holds,
//   min(target_frames - 1, queue_size - 1, max); the low-latency path (nothing is
//   held ahead) looks ahead 0.

#include <algorithm>
#include <cstddef>

namespace QueueLookahead
{
    // The renderer's own bound on the active-picture look-ahead.
    constexpr size_t kMaxActivePictureLookahead = 8;

    inline size_t ResolveActivePictureLookahead(bool configured, size_t value, bool lowLatencyPath,
        size_t targetFrames, size_t queueSize, size_t maxFrames = kMaxActivePictureLookahead)
    {
        if (configured)
            return (std::min)(value, maxFrames);
        if (lowLatencyPath)
            return 0;
        const size_t fromTarget = targetFrames > 0 ? targetFrames - 1 : 0;
        const size_t fromQueue = queueSize > 0 ? queueSize - 1 : 0;
        return (std::min)((std::min)(fromTarget, fromQueue), maxFrames);
    }
}
