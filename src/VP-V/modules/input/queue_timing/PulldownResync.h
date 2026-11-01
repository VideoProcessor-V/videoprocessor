// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <VP-V/core/Indicator.h>
#include <VP-V/core/ResyncCause.h>
#include <VP-V/modules/input/signal/ResyncShadow.h>

#include <cmath>
#include <cstdint>
#include <functional>

// Pulldown reader policy for resync_event = on (Q0-A). Pure helpers; INERT until the renderer's
// pulldown call site uses them. The detector itself (PulldownDetector.h, incl. its 0.5% signal-rate
// backstop) is not touched. No rate, cadence or resolution is fixed here: the cadence class is a
// function the caller supplies, the rates are measured.
namespace VpResync {

// The signal rate the pulldown detector is fed. In `on` the Resync detector's windowed measured rate
// (capture frames with timestamps) when it has one; otherwise `billValue` (today's cumulative rate).
// 0, negative or non-finite windowed rates (window not filled since the last break) fall back.
inline double PulldownSignalHz(const Shadow& shadow, double billValue)
{
    if (shadow.Driving()) {
        const double w = shadow.WindowedRateHz();
        if (w > 0.0 && std::isfinite(w))
            return w;
    }
    return billValue;
}

// What the pulldown does for one frame. Flags compose:
//  - reset: drop the lock (Detector::Reset) and relock from fresh frames.
//  - absorb: Detector::MarkDiscontinuity(); keep the lock; re-phase in place (a seek or capture
//    re-lock leaves the pattern; the verification cycle after it still guards a wrong lock).
//  - rederiveHz: take the new measured signal rate (RelatchSignalHz-style) without a reset.
//  - resetOnClassChange: informational; the cadence class of the rate changed, `reset` is set too.
struct PulldownResyncAction {
    bool reset = false;
    bool absorb = false;
    bool rederiveHz = false;
    bool resetOnClassChange = false;
    bool None() const { return !reset && !absorb && !rederiveHz && !resetOnClassChange; }
};

class PulldownResyncPolicy {
public:
    // Cadence class of a measured signal rate (caller defined; equal values = same class).
    using ClassFn = std::function<int(double hz)>;

    struct Input {
        VpIndicator::CounterMask changed = 0;   // counters changed since the pulldown's cursor stamp
        Cause fullCause = Cause::None;          // cause of the FULL between the stamps (None = unknown)
        bool modeWasOnNowOff = false;           // resync_event left `on` at this frame (see ModeEdgeLatch)
        double oldHz = 0.0;                     // measured signal rate at the last applied stamp
        double newHz = 0.0;                     // measured signal rate now
    };

    // Rows, in priority order:
    //  1. on -> off edge: reset (once; the edge is consumed by ModeEdgeLatch).
    //  2. FULL (any cause, with or without timing: FULL wins): LiveQueueReset (seek, skip, capture
    //     re-lock) -> absorb; every other cause, and an unknown cause -> reset.
    //  3. timing only: rederive the Hz; reset only when classOf(oldHz) != classOf(newHz). With an
    //     unknown rate (<= 0 or non-finite on either side) the class cannot be compared: rederive
    //     only (the detector's own 0.5% backstop still guards the lock).
    //  4. otherwise: none.
    static PulldownResyncAction Decide(const Input& in, const ClassFn& classOf)
    {
        PulldownResyncAction a;
        if (in.modeWasOnNowOff) {
            a.reset = true;
            return a;
        }
        if ((in.changed & VpIndicator::Bit(VpIndicator::kFull)) != 0) {
            if (in.fullCause == Cause::LiveQueueReset)
                a.absorb = true;
            else
                a.reset = true;
            return a;
        }
        if ((in.changed & VpIndicator::Bit(VpIndicator::kTiming)) != 0) {
            a.rederiveHz = true;
            const bool known = in.oldHz > 0.0 && in.newHz > 0.0 && std::isfinite(in.oldHz) &&
                               std::isfinite(in.newHz) && static_cast<bool>(classOf);
            if (known && classOf(in.oldHz) != classOf(in.newHz)) {
                a.resetOnClassChange = true;
                a.reset = true;
            }
        }
        return a;
    }

    // The cause of the latest FULL whose fromSeq lies in (fromSeqExclusive, toSeqInclusive]
    // (Indicator::LastCauseIn). Not found (none, or out of the ring) = unknown = Cause::None, which
    // Decide treats as a reset. A template so no Indicator method is needed to include this header.
    template <class IndicatorT>
    static Cause FullCauseBetween(const IndicatorT& indicator, uint64_t fromSeqExclusive, uint64_t toSeqInclusive)
    {
        Cause cause = Cause::None;
        if (!indicator.LastCauseIn(VpIndicator::kFull, fromSeqExclusive, toSeqInclusive, cause))
            return Cause::None;
        return cause;
    }

    template <class IndicatorT>
    static PulldownResyncAction DecideFromIndicator(const IndicatorT& indicator, uint64_t fromSeqExclusive,
                                                    uint64_t toSeqInclusive, VpIndicator::CounterMask changed,
                                                    bool modeWasOnNowOff, double oldHz, double newHz,
                                                    const ClassFn& classOf)
    {
        Input in;
        in.changed = changed;
        in.modeWasOnNowOff = modeWasOnNowOff;
        in.oldHz = oldHz;
        in.newHz = newHz;
        if ((changed & VpIndicator::Bit(VpIndicator::kFull)) != 0)
            in.fullCause = FullCauseBetween(indicator, fromSeqExclusive, toSeqInclusive);
        return Decide(in, classOf);
    }
};

// Latches the on -> off edge: Consume(drivingNow) is true exactly once when the previous call saw
// `on` and this one does not. The first call only records (starts as "was off").
class ModeEdgeLatch {
public:
    bool Consume(bool drivingNow)
    {
        const bool edge = m_was && !drivingNow;
        m_was = drivingNow;
        return edge;
    }
    void Reset() { m_was = false; }

private:
    bool m_was = false;
};

} // namespace VpResync
