// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// THE count of frames the configured queue holds (lead10/design-queue-keyed-products.md amendment 2, A1 / audit M1: one
// derivation, no second, conflicting count): render-ahead R + lane lead L + the look-ahead walk W (frames the walk covers BEYOND
// the queue, FramesInFlight.h). core/QueueWindow.h (the window's ahead = L + W), FramesInFlight() and QueueFramesFor (Products.h)
// all call it. Its own tiny header so FramesInFlight.h / Products.h need not include the window. Header-only, C++14.

#include <cstddef>
#include <cstdint>
#include <limits>

namespace VpSkeleton {

// The lane-lead feed's two times (hoisted from SkeletonModule.cpp so the window, its tests and the skeleton share one value, never a
// literal): the lane lead the graph is built for is the LOWEST queue size over the last kLeadWindowSeconds of real time; the window's
// lane-lead term is the BEST lead of the last kLeadPeakHoldSeconds (a decaying peak, WindowPeak in QueueWindow.h).
constexpr double kLeadWindowSeconds = 0.5;
constexpr double kLeadPeakHoldSeconds = 10.0;
inline uint64_t SecondsToUs(double seconds) { return uint64_t(seconds * 1e6); }

// The one sum, in any unsigned type, saturating at its maximum instead of wrapping (FramesInFlight counts in size_t).
template <class U>
inline U QueueFramesIn(U renderAhead, U laneLead, U lookAheadWalk)
{
    const U top = (std::numeric_limits<U>::max)();
    if (laneLead > top - renderAhead)
        return top;
    const U sum = U(renderAhead + laneLead);
    if (lookAheadWalk > top - sum)
        return top;
    return U(sum + lookAheadWalk);
}

inline uint32_t QueueFrames(uint32_t renderAhead, uint32_t laneLead, uint32_t lookAheadWalk)
{
    return QueueFramesIn<uint32_t>(renderAhead, laneLead, lookAheadWalk);
}

}  // namespace VpSkeleton
