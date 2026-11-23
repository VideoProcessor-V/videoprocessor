// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
// See QueueLookahead.h.
#include <pch.h>
#include <VP-V/modules/input/queue_timing/QueueLookahead.h>

#include <VP-V/core/RunningPathway.h>

namespace QueueLookahead
{
    size_t ResolveActivePictureLookahead(bool configured, size_t value, bool lowLatencyPath,
        size_t targetFrames, size_t queueSize, size_t maxFrames)
    {
        if (configured)
            return (std::min)(value, maxFrames);
        if (Vpv::RunningPathway::Original()) return 0;   // CC W4 B1: unset on Original = Bill's 0 (no automatic look-ahead)
        if (lowLatencyPath)
            return 0;
        const size_t fromTarget = targetFrames > 0 ? targetFrames - 1 : 0;
        const size_t fromQueue = queueSize > 0 ? queueSize - 1 : 0;
        return (std::min)((std::min)(fromTarget, fromQueue), maxFrames);
    }
}
