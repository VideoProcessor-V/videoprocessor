// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <cstddef>
#include <limits>

// Frames in flight for sizing the central Indicator's event ring
// (Indicator::RequiredCapacity(FramesInFlight(...))). Every one of these frames can still receive an
// event (an Advance with its fromSeq) before it is sealed, so the ring must hold
// kMaxEventsPerFrame events for each:
//   queueCapacity  the capture queue's hard capacity (frames waiting to be rendered)
//   lookAheadWalk  frames the motion look-ahead walks beyond the queue
//   history        frames kept behind the render position (motion history, pair partners)
//   renderAheadR   the render-ahead depth in frames (rendered but not yet sealed or presented)
//   laneLeadL      the lead of the producer lane over the render lane, in frames
//   + 1            the frame being sealed now.
// No magic numbers: every term is supplied by the caller from its own configuration. The sum
// saturates at SIZE_MAX instead of wrapping.
inline size_t FramesInFlight(size_t queueCapacity, size_t lookAheadWalk, size_t history, size_t renderAheadR,
                             size_t laneLeadL)
{
    const size_t terms[5] = { queueCapacity, lookAheadWalk, history, renderAheadR, laneLeadL };
    size_t sum = 1;
    for (size_t t : terms) {
        if (t > std::numeric_limits<size_t>::max() - sum)
            return std::numeric_limits<size_t>::max();
        sum += t;
    }
    return sum;
}
