// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// THE count of frames the configured queue holds (lead10/design-queue-keyed-products.md amendment 2, A1 / audit M1: one
// derivation, no second, conflicting count): render-ahead R + lane lead L + the look-ahead walk W (frames the walk covers BEYOND
// the queue, FramesInFlight.h). core/QueueWindow.h (the window's ahead = L + W), FramesInFlight() and QueueFramesFor (Products.h)
// all call it. Its own tiny header so FramesInFlight.h / Products.h need not include the window. Header-only, C++14.

#include <cstdint>

namespace VpSkeleton {

inline uint32_t QueueFrames(uint32_t renderAhead, uint32_t laneLead, uint32_t lookAheadWalk)
{
    return renderAhead + laneLead + lookAheadWalk;
}

}  // namespace VpSkeleton
