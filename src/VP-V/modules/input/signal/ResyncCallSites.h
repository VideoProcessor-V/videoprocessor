// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <VP-V/core/ResyncCause.h>
#include <VP-V/modules/input/signal/ResyncDrive.h>
#include <VP-V/modules/input/signal/ResyncShadow.h>

#include <memory>
#include <string>

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

// R3: the counters a reader saw change since its last call, or 0 when resync_event is not `on`
// (then the reader's pending mask is dropped: nothing carries over into a later `on`).
inline VpIndicator::CounterMask TakeIfDriving(const Shadow& shadow, Drive& drive, Reader reader)
{
    const VpIndicator::CounterMask m = drive.Take(reader);
    return shadow.Driving() ? m : 0;
}

// R3: the generation a part compares (render-ahead gate, de-judder phase, interpolation timeline):
// the queue generation when resync_event is not `on` (today's key, unchanged), else the Indicator's
// FULL key of the frame (a new key on every FULL: seek, in-place source change, calibration...).
inline uint64_t ReaderGeneration(const Shadow& shadow, uint64_t queueGeneration, uint64_t seq)
{
    if (!shadow.Driving() || seq == 0)
        return queueGeneration;
    const std::shared_ptr<VpIndicator::Indicator> indicator = shadow.GetIndicator();
    return indicator ? Drive::FullKey(*indicator, seq) : queueGeneration;
}

// R3: the sigma-delta carry from A's field into B's first present is valid only when A and B carry
// the same full, continuity and re-base H/V stamps. Always true when resync_event is not `on`.
inline bool CarryValid(const Shadow& shadow, uint64_t fromSeq, uint64_t toSeq)
{
    if (!shadow.Driving())
        return true;
    const std::shared_ptr<VpIndicator::Indicator> indicator = shadow.GetIndicator();
    if (!indicator || fromSeq == 0 || toSeq == 0)
        return false;
    return Drive::PairValid(*indicator, fromSeq, toSeq,
        VpIndicator::Bit(VpIndicator::kFull) | VpIndicator::Bit(VpIndicator::kContinuity) |
        VpIndicator::Bit(VpIndicator::kRebaseH) | VpIndicator::Bit(VpIndicator::kRebaseV));
}

// R3 / L1 / R4a: the mode the renderer configures from resync_event (Original pathway: Off).
inline Shadow::Mode ModeFromSetting(const std::string& value, bool originalPathway)
{
    if (originalPathway)
        return Shadow::Mode::Off;
    if (value == "on")
        return Shadow::Mode::On;
    if (value == "shadow")
        return Shadow::Mode::Shadow;
    return Shadow::Mode::Off;
}

} // namespace VpResync
