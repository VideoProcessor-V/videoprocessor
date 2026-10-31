// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <VP-V/core/ResyncCause.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>

// Resync detector (R0). Pure, deterministic, header-only C++14. It classifies every enqueued frame
// into at most one content event (FULL or MINOR), an optional overflow-successor MINOR and an
// optional Timing event. Nothing here knows a rate, a resolution or a cadence: every threshold is
// derived from the facts (measured rate, last normal timestamp delta) or from DetectorSettings.
namespace VpResync {

struct EnqueueFacts {
    uint64_t sequence = 0;        // the frame's sourceSequence (>0, strictly increasing per renderer)
    uint64_t queueGeneration = 0; // enqueueGeneration
    uint64_t stateKey = 0;        // identity of the frame's VideoState object (pointer value)
    uint64_t formatKey = 0;       // AlphaSourceFormatKey(state) (raster/encoding/colour contract)
    uint64_t frameCounter = 0;    // the capture/file frame counter
    int64_t  timestampTicks = 0;  // frame timing timestamp; 0 = unknown
    int64_t  ticksPerSecond = 0;  // 0 = unknown (then no timestamp grading)
    bool     gapBefore = false;
    uint32_t gapFrames = 0;       // whole missed frames if known
    const char* gapCause = nullptr;
    bool     sourceDiscontinuity = false;  // VideoFrame::IsSourceDiscontinuity() or file discontinuity
    bool     fileFrame = false;
    double   measuredRateHz = 0.0;         // 0 = not measured (file frames pass 0)
    uint64_t overflowSuccessorSeq = 0;     // != 0: an already-queued frame got an overflow-drop gap
};

struct Event {
    Level level = Level::None;
    Cause cause = Cause::None;
    uint64_t fromSeq = 0;
    uint32_t gapFrames = 0;
    uint8_t detail = 0;   // RefusalReason for StartAfterSourceChange, else 0
};

// content (FULL or MINOR) + overflow successor MINOR + timing
struct Decision {
    uint8_t count = 0;
    Event events[3];
};

struct DetectorSettings {
    double breakFullSeconds = 2.0;
    double rateStepPpm = 500.0;
};

// ---------------------------------------------------------------------------------------------
// ArmBox: lock-free, never blocks, never allocates. Two slots, each a seqlock built from atomic
// words (version odd while writing; the reader retries a bounded number of times and otherwise
// reports "nothing readable now", it never spins forever).
// Precondition: ONE writer at a time per slot (Queue-slot writers hold the renderer's
// m_queueMutex, State-slot writers hold m_stateMutex). The reader is the Detector.
// A new arm replaces a pending one in the same slot.
// ---------------------------------------------------------------------------------------------
class ArmBox {
public:
    enum Slot { Queue = 0, State = 1 };

    struct Snapshot {
        bool ok = false;          // consistent read obtained
        uint64_t version = 0;     // even; 0 = never armed
        uint64_t key = 0;
        Cause cause = Cause::None;
    };

    void ArmQueue(uint64_t generation, Cause cause) { Write(m_slots[Queue], generation, cause); }
    void ArmState(uint64_t stateKey, Cause cause) { Write(m_slots[State], stateKey, cause); }

    Snapshot Read(Slot slot) const
    {
        const Seq& s = m_slots[slot];
        Snapshot out;
        for (int attempt = 0; attempt < 16; ++attempt) {
            const uint64_t v1 = s.version.load(std::memory_order_acquire);
            if (v1 & 1u)
                continue;
            const uint64_t key = s.key.load(std::memory_order_relaxed);
            const uint32_t cause = s.cause.load(std::memory_order_relaxed);
            std::atomic_thread_fence(std::memory_order_acquire);
            const uint64_t v2 = s.version.load(std::memory_order_relaxed);
            if (v1 != v2)
                continue;
            out.ok = true;
            out.version = v1;
            out.key = key;
            out.cause = static_cast<Cause>(cause);
            return out;
        }
        return out;
    }

private:
    struct Seq {
        std::atomic<uint64_t> version{0};
        std::atomic<uint64_t> key{0};
        std::atomic<uint32_t> cause{0};
    };

    static void Write(Seq& s, uint64_t key, Cause cause)
    {
        const uint64_t v = s.version.load(std::memory_order_relaxed);
        s.version.store(v + 1, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_release);
        s.key.store(key, std::memory_order_relaxed);
        s.cause.store(static_cast<uint32_t>(cause), std::memory_order_relaxed);
        s.version.store(v + 2, std::memory_order_release);
    }

    Seq m_slots[2];
};

// ---------------------------------------------------------------------------------------------
// RefusalNote: process-global. A refused OnVideoState change is noted here so that the rebuilt
// renderer's first FULL (StartAfterSourceChange) can say why.
// ---------------------------------------------------------------------------------------------
class RefusalNote {
public:
    static void Set(RefusalReason r) { Cell().store(static_cast<uint8_t>(r), std::memory_order_relaxed); }
    static RefusalReason Take()
    {
        return static_cast<RefusalReason>(Cell().exchange(static_cast<uint8_t>(RefusalReason::None),
                                                          std::memory_order_relaxed));
    }

private:
    static std::atomic<uint8_t>& Cell()
    {
        static std::atomic<uint8_t> cell{0};
        return cell;
    }
};

// ---------------------------------------------------------------------------------------------
// Literal tables
// ---------------------------------------------------------------------------------------------
inline Cause CauseFromGapText(const char* text)
{
    if (!text)
        return Cause::GapOther;
    struct Row { const char* text; Cause cause; };
    static const Row rows[] = {
        { "missed frames", Cause::MissedFrames },
        { "capture timestamp discontinuity", Cause::TimestampBreak },
        { "stale capture delivery", Cause::StaleDelivery },
        { "stale capture delivery ended", Cause::StaleDeliveryEnded },
        { "queue overflow drop", Cause::OverflowDrop },
        { "repeat-preserving queue drop", Cause::RepeatPreservingDrop },
        { "pulldown lock change", Cause::PulldownLockChange },
        { "pulldown lock change (on a skipped repeat)", Cause::PulldownLockChange },
        { "pulldown re-phase", Cause::PulldownRephase },
        { "pulldown film clock restarted (switch toggled)", Cause::PulldownClockRestart },
        { "pulldown off", Cause::PulldownOff },
    };
    for (const Row& r : rows)
        if (std::strcmp(r.text, text) == 0)
            return r.cause;
    return Cause::GapOther;
}

inline Cause CauseFromQueueReason(const char* reason)
{
    if (!reason)
        return Cause::QueueGenerationOther;
    struct Row { const char* text; Cause cause; };
    static const Row rows[] = {
        { "start", Cause::Start },
        { "renderer reset", Cause::RendererReset },
        { "live queue reset", Cause::LiveQueueReset },
        { "calibration patterns", Cause::CalibrationStart },
        { "calibration patterns ended", Cause::CalibrationEnd },
    };
    for (const Row& r : rows)
        if (std::strcmp(r.text, reason) == 0)
            return r.cause;
    return Cause::QueueGenerationOther;
}

// ---------------------------------------------------------------------------------------------
// Detector: state guarded by the caller's queue mutex; not thread-safe itself.
// ---------------------------------------------------------------------------------------------
class Detector {
public:
    void SetSettings(const DetectorSettings& s) { m_settings = s; }

    // The next frame is a FULL Start.
    void Restart()
    {
        m_first = true;
        m_haveLastTs = false;
        m_lastNormalDeltaTicks = 0;
        m_prevRateHz = 0.0;
    }

    Decision OnEnqueue(const EnqueueFacts& f, ArmBox& arms)
    {
        Decision d;
        Event content;
        bool full = false;

        const ArmBox::Snapshot qa = arms.Read(ArmBox::Queue);
        const ArmBox::Snapshot sa = arms.Read(ArmBox::State);
        const bool qPending = qa.ok && qa.version != m_consumedQueue && qa.key <= f.queueGeneration;
        const bool sMatch = sa.ok && sa.version != m_consumedState && sa.key == f.stateKey;

        if (m_first) {
            const RefusalReason why = RefusalNote::Take();
            if (why != RefusalReason::None)
                content = MakeFull(Cause::StartAfterSourceChange, f.sequence, static_cast<uint8_t>(why));
            else
                content = MakeFull(Cause::Start, f.sequence, 0);
            full = true;
            if (qPending) m_consumedQueue = qa.version;
            if (sMatch) m_consumedState = sa.version;
        } else if (f.queueGeneration != m_lastGeneration) {
            content = MakeFull(qPending ? qa.cause : Cause::QueueGenerationOther, f.sequence, 0);
            full = true;
            if (qPending) m_consumedQueue = qa.version;
            if (sMatch) m_consumedState = sa.version;   // consumed silently: no second action
        } else if (sMatch) {
            content = MakeFull(sa.cause, f.sequence, 0);
            full = true;
            m_consumedState = sa.version;
        } else if (f.sourceDiscontinuity) {
            content = MakeFull(Cause::FileDiscontinuity, f.sequence, 0);
            full = true;
        } else if (f.frameCounter < m_lastCounter) {
            content = MakeFull(Cause::CounterBackwards, f.sequence, 0);
            full = true;
        } else if (f.formatKey != m_lastFormat) {
            content = MakeFull(Cause::SourceFormatChanged, f.sequence, 0);
            full = true;
        } else if (f.gapBefore) {
            Cause c = CauseFromGapText(f.gapCause);
            if (c == Cause::TimestampBreak && f.ticksPerSecond > 0 && f.timestampTicks != 0 && m_haveLastTs
                && GradeTimestampBreak(f))
                c = Cause::TimestampBreakLarge;
            if (LevelOf(c) == Level::Full)
                content = MakeFull(c, f.sequence, 0);
            else
                content = MakeMinor(c, f.sequence, f.gapFrames);
            full = LevelOf(c) == Level::Full;
        }

        if (content.level != Level::None)
            d.events[d.count++] = content;

        if (f.overflowSuccessorSeq != 0)
            d.events[d.count++] = MakeMinor(Cause::OverflowDrop, f.overflowSuccessorSeq, 0);

        if (!full && f.measuredRateHz > 0.0 && m_prevRateHz > 0.0
            && std::fabs(f.measuredRateHz - m_prevRateHz) / m_prevRateHz > m_settings.rateStepPpm * 1e-6) {
            Event e;
            e.level = Level::Timing;
            e.cause = Cause::RateStep;
            e.fromSeq = f.sequence;
            d.events[d.count++] = e;
        }

        // Memory update (every frame).
        m_prevRateHz = f.measuredRateHz > 0.0 ? f.measuredRateHz : 0.0;
        if (f.timestampTicks != 0 && f.ticksPerSecond > 0) {
            if (m_haveLastTs && !(f.gapBefore && content.cause == Cause::TimestampBreak)
                && !(content.level == Level::Full)) {
                const int64_t delta = f.timestampTicks - m_lastTs;
                if (delta > 0)
                    m_lastNormalDeltaTicks = delta;
            }
            m_lastTs = f.timestampTicks;
            m_haveLastTs = true;
        }
        m_lastGeneration = f.queueGeneration;
        m_lastCounter = f.frameCounter;
        m_lastFormat = f.formatKey;
        m_first = false;
        return d;
    }

    // A late MINOR (drift drop / kept-run hole) at the slip point.
    Event Late(Cause cause, uint64_t slipSeq, uint32_t gapFrames) const
    {
        return MakeMinor(cause, slipSeq, gapFrames);
    }

private:
    static Event MakeFull(Cause c, uint64_t seq, uint8_t detail)
    {
        Event e;
        e.level = Level::Full;
        e.cause = c;
        e.fromSeq = seq;
        e.detail = detail;
        return e;
    }
    static Event MakeMinor(Cause c, uint64_t seq, uint32_t frames)
    {
        Event e;
        e.level = Level::Minor;
        e.cause = c;
        e.fromSeq = seq;
        e.gapFrames = frames;
        return e;
    }

    // True when the break is FULL: backwards beyond one period, or a jump beyond breakFullSeconds.
    bool GradeTimestampBreak(const EnqueueFacts& f) const
    {
        const double tps = static_cast<double>(f.ticksPerSecond);
        const double delta = static_cast<double>(f.timestampTicks - m_lastTs);
        double periodTicks = 0.0;
        if (f.measuredRateHz > 0.0)
            periodTicks = tps / f.measuredRateHz;
        else if (m_lastNormalDeltaTicks > 0)
            periodTicks = static_cast<double>(m_lastNormalDeltaTicks);
        if (delta < -periodTicks)
            return true;
        return std::fabs(delta) / tps > m_settings.breakFullSeconds;
    }

    DetectorSettings m_settings;
    bool m_first = true;
    uint64_t m_lastGeneration = 0;
    uint64_t m_lastCounter = 0;
    uint64_t m_lastFormat = 0;
    bool m_haveLastTs = false;
    int64_t m_lastTs = 0;
    int64_t m_lastNormalDeltaTicks = 0;
    double m_prevRateHz = 0.0;
    uint64_t m_consumedQueue = 0;
    uint64_t m_consumedState = 0;
};

// ---------------------------------------------------------------------------------------------
// StormWindow: more than `threshold` FULL events within `windowSeconds`, calibration excluded.
// Fixed capacity (oldest overwritten); no growth.
// ---------------------------------------------------------------------------------------------
class StormWindow {
public:
    explicit StormWindow(int threshold = 3, double windowSeconds = 10.0)
        : m_threshold(threshold), m_window(windowSeconds) {}

    void Note(Cause cause, double seconds)
    {
        if (LevelOf(cause) != Level::Full || ExcludedFromStorm(cause))
            return;
        m_times[m_next] = seconds;
        m_next = (m_next + 1) % kCapacity;
        if (m_size < kCapacity)
            ++m_size;
    }

    int FullCount(double seconds) const
    {
        int n = 0;
        for (int i = 0; i < m_size; ++i) {
            const double age = seconds - m_times[i];
            if (age >= 0.0 && age <= m_window)
                ++n;
        }
        return n;
    }

    bool Engaged(double seconds) const { return FullCount(seconds) > m_threshold; }

private:
    static const int kCapacity = 64;
    int m_threshold;
    double m_window;
    double m_times[kCapacity] = {};
    int m_next = 0;
    int m_size = 0;
};

} // namespace VpResync
