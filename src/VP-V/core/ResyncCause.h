// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <cstdint>

// Shared resync vocabulary: what happened (Cause), how serious it is (Level), and why a source
// change was refused (RefusalReason). Header-only, pure C++14. Used by the resync detector
// (modules/input/signal/ResyncEvent.h) and the central Indicator (Indicator.h).
namespace VpResync {

enum class Level : uint8_t { None, Timing, Minor, Full };

enum class Cause : uint16_t {
    None = 0,
    // FULL
    Start,
    StartAfterSourceChange,
    InPlaceSourceChange,
    RendererReset,
    LiveQueueReset,
    CalibrationStart,
    CalibrationEnd,
    QueueGenerationOther,
    FileDiscontinuity,
    CounterBackwards,
    TimestampBreakLarge,
    SourceFormatChanged,
    // MINOR at enqueue
    MissedFrames,
    TimestampBreak,
    StaleDelivery,
    StaleDeliveryEnded,
    OverflowDrop,
    RepeatPreservingDrop,
    PulldownLockChange,
    PulldownRephase,
    PulldownClockRestart,
    PulldownOff,
    GapOther,
    // MINOR late
    DriftDrop,
    KeptRunHole,
    // timing
    RateStep,
    // non-resync writers
    SettingsApply,
    RebaseDecision,
    GraphRebuild,
    Count
};

inline Level LevelOf(Cause c)
{
    switch (c) {
    case Cause::Start:
    case Cause::StartAfterSourceChange:
    case Cause::InPlaceSourceChange:
    case Cause::RendererReset:
    case Cause::LiveQueueReset:
    case Cause::CalibrationStart:
    case Cause::CalibrationEnd:
    case Cause::QueueGenerationOther:
    case Cause::FileDiscontinuity:
    case Cause::CounterBackwards:
    case Cause::TimestampBreakLarge:
    case Cause::SourceFormatChanged:
        return Level::Full;
    case Cause::MissedFrames:
    case Cause::TimestampBreak:
    case Cause::StaleDelivery:
    case Cause::StaleDeliveryEnded:
    case Cause::OverflowDrop:
    case Cause::RepeatPreservingDrop:
    case Cause::PulldownLockChange:
    case Cause::PulldownRephase:
    case Cause::PulldownClockRestart:
    case Cause::PulldownOff:
    case Cause::GapOther:
    case Cause::DriftDrop:
    case Cause::KeptRunHole:
        return Level::Minor;
    case Cause::RateStep:
        return Level::Timing;
    default:
        return Level::None;
    }
}

inline const char* Token(Cause c)
{
    switch (c) {
    case Cause::None: return "none";
    case Cause::Start: return "start";
    case Cause::StartAfterSourceChange: return "start_after_source_change";
    case Cause::InPlaceSourceChange: return "in_place_source_change";
    case Cause::RendererReset: return "renderer_reset";
    case Cause::LiveQueueReset: return "live_queue_reset";
    case Cause::CalibrationStart: return "calibration_start";
    case Cause::CalibrationEnd: return "calibration_end";
    case Cause::QueueGenerationOther: return "queue_generation_other";
    case Cause::FileDiscontinuity: return "file_discontinuity";
    case Cause::CounterBackwards: return "counter_backwards";
    case Cause::TimestampBreakLarge: return "timestamp_break_large";
    case Cause::SourceFormatChanged: return "source_format_changed";
    case Cause::MissedFrames: return "missed_frames";
    case Cause::TimestampBreak: return "timestamp_break";
    case Cause::StaleDelivery: return "stale_delivery";
    case Cause::StaleDeliveryEnded: return "stale_delivery_ended";
    case Cause::OverflowDrop: return "overflow_drop";
    case Cause::RepeatPreservingDrop: return "repeat_preserving_drop";
    case Cause::PulldownLockChange: return "pulldown_lock_change";
    case Cause::PulldownRephase: return "pulldown_rephase";
    case Cause::PulldownClockRestart: return "pulldown_clock_restart";
    case Cause::PulldownOff: return "pulldown_off";
    case Cause::GapOther: return "gap_other";
    case Cause::DriftDrop: return "drift_drop";
    case Cause::KeptRunHole: return "kept_run_hole";
    case Cause::RateStep: return "rate_step";
    case Cause::SettingsApply: return "settings_apply";
    case Cause::RebaseDecision: return "rebase_decision";
    case Cause::GraphRebuild: return "graph_rebuild";
    default: return "unknown";
    }
}

inline const char* Describe(Cause c)
{
    switch (c) {
    case Cause::None: return "none";
    case Cause::Start: return "start";
    case Cause::StartAfterSourceChange: return "start after a source change";
    case Cause::InPlaceSourceChange: return "source change";
    case Cause::RendererReset: return "renderer reset";
    case Cause::LiveQueueReset: return "seek, skip or capture re-lock";
    case Cause::CalibrationStart: return "calibration patterns started";
    case Cause::CalibrationEnd: return "calibration patterns ended";
    case Cause::QueueGenerationOther: return "queue reset";
    case Cause::FileDiscontinuity: return "file discontinuity";
    case Cause::CounterBackwards: return "frame counter went backwards";
    case Cause::TimestampBreakLarge: return "large timestamp break";
    case Cause::SourceFormatChanged: return "source format changed";
    case Cause::MissedFrames: return "missed frames";
    case Cause::TimestampBreak: return "timestamp break";
    case Cause::StaleDelivery: return "stale capture delivery";
    case Cause::StaleDeliveryEnded: return "stale capture delivery ended";
    case Cause::OverflowDrop: return "queue overflow drop";
    case Cause::RepeatPreservingDrop: return "repeat-preserving drop";
    case Cause::PulldownLockChange: return "pulldown lock change";
    case Cause::PulldownRephase: return "pulldown re-phase";
    case Cause::PulldownClockRestart: return "pulldown clock restart";
    case Cause::PulldownOff: return "pulldown off";
    case Cause::GapOther: return "gap (other)";
    case Cause::DriftDrop: return "drift drop";
    case Cause::KeptRunHole: return "kept-run hole";
    case Cause::RateStep: return "frame rate step";
    case Cause::SettingsApply: return "settings applied";
    case Cause::RebaseDecision: return "re-base decision";
    case Cause::GraphRebuild: return "graph rebuilt";
    default: return "unknown";
    }
}

// Calibration patterns are a deliberate, operator-driven reset; they never count towards a storm.
inline bool ExcludedFromStorm(Cause c)
{
    return c == Cause::CalibrationStart || c == Cause::CalibrationEnd;
}

enum class RefusalReason : uint8_t {
    None,
    DisplayMode,
    Encoding,
    DvTunnel,
    InvalidState,
    SettingsRebuild,
    InitRace
};

inline const char* Describe(RefusalReason r)
{
    switch (r) {
    case RefusalReason::DisplayMode: return "display mode (incl. 59.94/60 re-latch)";
    case RefusalReason::Encoding: return "encoding change";
    case RefusalReason::DvTunnel: return "Dolby Vision tunnel change";
    case RefusalReason::InvalidState: return "invalid video state";
    case RefusalReason::SettingsRebuild: return "settings change needs a rebuild";
    case RefusalReason::InitRace: return "renderer initialization race";
    default: return "none";
    }
}

} // namespace VpResync
