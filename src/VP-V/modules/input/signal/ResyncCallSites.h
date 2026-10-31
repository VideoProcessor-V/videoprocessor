// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <VP-V/core/ResyncCause.h>

// Helpers for the renderer's resync call sites, kept out of Bill's file. Pure.
namespace VpResync {

// Which of OnVideoState's first refusal comparisons fired (same comparisons, same fields).
template<typename State>
inline RefusalReason ClassifyStateRefusal(const State& next, const State& current)
{
    if (next.valid == false)
        return RefusalReason::InvalidState;
    if (!next.displayMode || !current.displayMode || *next.displayMode != *current.displayMode)
        return RefusalReason::DisplayMode;
    if (next.videoFrameEncoding != current.videoFrameEncoding)
        return RefusalReason::Encoding;
    if (next.dolbyVisionTunnel != current.dolbyVisionTunnel)
        return RefusalReason::DvTunnel;
    return RefusalReason::None;
}

} // namespace VpResync
