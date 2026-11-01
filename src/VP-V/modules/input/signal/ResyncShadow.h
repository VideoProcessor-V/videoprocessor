// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <VP-V/core/FallbackCatalog.h>
#include <VP-V/core/Indicator.h>
#include <VP-V/core/ResyncCause.h>
#include <VP-V/modules/control/diagnostics/VpEventLine.h>
#include <VP-V/modules/input/signal/ResyncEvent.h>

#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

// Resync shadow mode (R1). The renderer's enqueue path feeds every frame's facts to the R0 detector;
// in Shadow mode the verdicts are written to the central Indicator and summarised as one status line
// and one event list. Nothing here changes the picture, the queue or any decision: the renderer
// never reads a verdict back. In Off mode OnEnqueue costs one relaxed atomic load.
//
// Threads: Configure runs on the render thread (settings apply). ArmQueueGeneration /
// ArmInPlaceSourceChange / NoteRefusal are called by the writers named in ResyncEvent.h. OnEnqueue
// runs under the renderer's queue mutex (the detector is only ever touched there). StatusLine and
// EventLines may run on any thread (GUI): they read atomics, a seqlock snapshot and one small mutex.
//
// Header-only, pure C++14, no Windows, no logging dependency: the caller supplies a log callback.
namespace VpResync {

class Shadow {
public:
    // On (R3 / L1 / R4a, expert until the R5 gate): Shadow, plus the render loop seals each frame and
    // the Indicator drives the readers' resets (ResyncDrive.h). Off and Shadow never change a decision.
    enum class Mode : uint8_t { Off, Shadow, On };

    Shadow()
    {
        for (size_t i = 0; i < kMinorRing; ++i)
            m_minorTimes[i].store(-1.0e30, std::memory_order_relaxed);
    }

    // Render thread. Turning Shadow on, or asking for more frames in flight than the current
    // Indicator holds, builds a fresh Indicator (new era) and restarts the detector (the next frame
    // is a FULL Start). The restart is consumed in OnEnqueue under the queue mutex.
    void Configure(Mode mode, size_t framesInFlight)
    {
        const Mode previous = static_cast<Mode>(m_mode.load(std::memory_order_relaxed));
        if (mode != Mode::Off) {
            // Shadow <-> On keeps the Indicator (the same detector and era); Off -> either builds one.
            const size_t want = VpIndicator::Indicator::RequiredCapacity(framesInFlight);
            const std::shared_ptr<VpIndicator::Indicator> current = std::atomic_load(&m_indicator);
            if (previous == Mode::Off || !current || want > current->Capacity()) {
                const std::shared_ptr<VpIndicator::Indicator> next =
                    std::make_shared<VpIndicator::Indicator>(VpIndicator::NextEra(), want);
                // Shadow/On -> a larger ring: the old era's events for queued, unsealed frames move to
                // the new era, and any writer still holding the old one (OnEnqueue under the queue
                // mutex, a late slip, MotionValidity's settings advance) is forwarded there: one
                // critical section on the old Indicator, so nothing lands in the dead era. Off -> on
                // starts clean (nothing was written while Off).
                if (current && previous != Mode::Off)
                    current->HandOverTo(next);
                std::atomic_store(&m_indicator, next);
                ResetCursor();   // the old era's enqueue cursor must not outlive its Indicator
                m_restart.store(true, std::memory_order_release);
            }
        }
        m_mode.store(static_cast<uint8_t>(mode), std::memory_order_release);
    }

    // Bug hunt 35 #3 / 25a R5: the detector's windowed measured rate (capture frames with timestamps;
    // 0 until the window has filled since the last break, and in Off). Any thread. The interface for
    // the pulldown's signal rate in `on` (the renderer passes the cumulative rate today: listed).
    double WindowedRateHz() const { return IsShadow() ? m_windowedRateHz.load(std::memory_order_relaxed) : 0.0; }

    // Relaxed load: true in Shadow and On. The renderer gates its whole facts block on this.
    bool Active() const { return IsShadow(); }

    // Relaxed load: true in On only (the readers act).
    bool Driving() const { return m_mode.load(std::memory_order_relaxed) == static_cast<uint8_t>(Mode::On); }

    Mode GetMode() const { return static_cast<Mode>(m_mode.load(std::memory_order_relaxed)); }

    ArmBox& Arms() { return m_arms; }

    void ArmQueueGeneration(uint64_t generation, const char* reason)
    {
        if (!IsShadow())
            return;
        m_arms.ArmQueue(generation, CauseFromQueueReason(reason));
    }

    void ArmInPlaceSourceChange(uint64_t stateKey)
    {
        if (!IsShadow())
            return;
        m_arms.ArmState(stateKey, Cause::InPlaceSourceChange);
    }

    void NoteRefusal(RefusalReason reason)
    {
        if (!IsShadow())
            return;
        RefusalNote::Set(reason);
    }

    // A late MINOR (a drift-correction drop, a kept-run hole from a backlog flush) found on the render
    // loop at slipSeq: every frame from slipSeq on gets the new continuity stamp (clamped above the
    // last sealed frame in On). Any thread; never throws.
    void OnLateSlip(Cause cause, uint64_t slipSeq, uint32_t gapFrames, double nowSeconds, double sourceSeconds = -1.0)
    {
        if (!IsShadow() || slipSeq == 0)
            return;
        try {
            const std::shared_ptr<VpIndicator::Indicator> indicator = std::atomic_load(&m_indicator);
            if (!indicator)
                return;
            Record(*indicator, m_detector.Late(cause, slipSeq, gapFrames), nowSeconds, StormTime(nowSeconds, sourceSeconds));
        } catch (...) {
            m_errors.fetch_add(1, std::memory_order_relaxed);
        }
    }

    // R4a (On only): the re-base decider changed the width (H) and/or height (V) factor or kernel.
    // Written for the first unsealed frame (the Indicator clamps fromSeq; never retroactive).
    void OnRebaseDecision(bool horizontal, bool vertical, uint64_t fromSeq)
    {
        if (!Driving() || (!horizontal && !vertical))
            return;
        try {
            const std::shared_ptr<VpIndicator::Indicator> indicator = std::atomic_load(&m_indicator);
            if (!indicator)
                return;
            uint64_t effective = fromSeq;
            if (horizontal)
                effective = indicator->Advance(VpIndicator::kRebaseH, Cause::RebaseDecision, fromSeq);
            if (vertical)
                effective = indicator->Advance(VpIndicator::kRebaseV, Cause::RebaseDecision, fromSeq);
            // Re-base changes are rare (a decision, not a frame event): every one is logged.
            Log(std::string("Resync (on): re-base ") + (horizontal && vertical ? "H+V" : horizontal ? "H" : "V") +
                " stamp from seq " + std::to_string(effective));
        } catch (...) {
            m_errors.fetch_add(1, std::memory_order_relaxed);
        }
    }

    // The caller holds the queue mutex. nowSeconds is a steady clock used for display, the minor
    // window and the VPEV1 time stamp. The detector's verdicts depend only on the facts. The STORM
    // HOLD (a decision in `on`) runs on nowSeconds by default; in reproducible (frame-bound /
    // deterministic) runs call SetStormSourceTime(true) and pass `sourceSeconds` (the frame's source
    // time, never a constant): the storm window and release then use source time only. A negative
    // sourceSeconds in that mode is counted (StormClockMissing) and the last source time is kept.
    void OnEnqueue(const EnqueueFacts& facts, double nowSeconds, double sourceSeconds = -1.0)
    {
        if (!IsShadow())
            return;
        try {
            if (m_restart.exchange(false, std::memory_order_acq_rel)) {
                m_detector.Restart();
                m_windowedRateHz.store(0.0, std::memory_order_relaxed);
                ResetCursor();
                ResetSummary();
            }
            const std::shared_ptr<VpIndicator::Indicator> indicator = std::atomic_load(&m_indicator);
            if (!indicator)
                return;
            const Decision decision = m_detector.OnEnqueue(facts, m_arms);
            m_windowedRateHz.store(m_detector.WindowedRateHz(), std::memory_order_relaxed);
            for (uint8_t i = 0; i < decision.count; ++i)
                Record(*indicator, decision.events[i], nowSeconds, StormTime(nowSeconds, sourceSeconds));
        } catch (...) {
            m_errors.fetch_add(1, std::memory_order_relaxed);   // never propagates into the renderer; shown in the status line
        }
    }

    // Display text. False in Off (the line is unknown: no row anywhere).
    bool StatusLine(std::string& out, double nowSeconds) const
    {
        if (!IsShadow())
            return false;
        try {
            return BuildStatus(out, nowSeconds);
        } catch (...) {
            const uint32_t n = m_errors.fetch_add(1, std::memory_order_relaxed) + 1u;
            try { out = "shadow: status unavailable; shadow error x" + std::to_string(n); } catch (...) {}
            return true;
        }
    }

    // The Indicator's last 64 events, one item each, joined with " | ". False in Off.
    bool EventLines(std::string& out) const
    {
        if (!IsShadow())
            return false;
        try {
            return BuildEvents(out);
        } catch (...) {
            const uint32_t n = m_errors.fetch_add(1, std::memory_order_relaxed) + 1u;
            try { out = "shadow: events unavailable; shadow error x" + std::to_string(n); } catch (...) {}
            return true;
        }
    }

    // ---- B2 cursor: the highest sequence enqueued so far (the pulldown reads it at the seal) -------
    // A compare-exchange loop: the value only ever grows until ResetCursor (restart / era change, the
    // render thread), and a NoteEnqueued racing a ResetCursor never writes back a value read before
    // the reset over it (it retries against the reset value).
    void NoteEnqueued(uint64_t seq)
    {
        uint64_t cur = m_cursorSeq.load(std::memory_order_relaxed);
        while (seq > cur && !m_cursorSeq.compare_exchange_weak(cur, seq, std::memory_order_release, std::memory_order_relaxed)) {
        }
    }
    uint64_t PulldownCursorSeq() const { return m_cursorSeq.load(std::memory_order_acquire); }
    void ResetCursor() { m_cursorSeq.store(0, std::memory_order_release); }

    // ---- G8 / D4 storm hold ---------------------------------------------------------------------
    // Engages on more than StormWindow's threshold of FULL events (3) in 10 s (calibration / start
    // causes excluded, as StormWindow does) and stays held until 10 s pass with no FULL since the
    // last FULL. True ONLY in On: in Shadow nothing is held and the status text keeps "would hold".
    struct SafeSettings {
        bool laneT0 = false;
        bool nativeRebase = false;
        bool vectorsPaused = false;
    };
    // The storm clock: steady seconds by default; in source-time mode (SetStormSourceTime) every event
    // is timed by its frame's source seconds. The HOLD is decided for the RENDERED frame: StormHold,
    // StormSafeSettings and StormNotice take that frame's time (source seconds in source-time mode)
    // and see only the FULL events timed at or before it, so in source-time mode the enqueue thread
    // running ahead (events of queued frames already recorded) never changes the answer. On the
    // steady clock (the default) event times are enqueue times, so the hold stays timing-dependent
    // by nature (not reproducible). AdvanceStormClock only feeds the status line's display time.
    void SetStormSourceTime(bool on) { m_stormSourceTime.store(on, std::memory_order_release); }
    bool StormSourceTime() const { return m_stormSourceTime.load(std::memory_order_acquire); }
    // Render thread, once per frame in source-time mode: lets the 10 s quiet release run without an
    // event (source seconds only move forward here).
    void AdvanceStormClock(double sourceSeconds)
    {
        double cur = m_stormClock.load(std::memory_order_relaxed);
        if (sourceSeconds > cur)
            m_stormClock.store(sourceSeconds, std::memory_order_relaxed);
    }
    uint64_t StormClockMissing() const { return m_stormClockMissing.load(std::memory_order_relaxed); }

    // renderSeconds = the rendered (sealed) frame's time: its source seconds in source-time mode, the
    // steady clock otherwise. Deterministic in source-time mode: the result depends only on the FULL
    // events whose source time is <= renderSeconds, never on how far the enqueue thread has run ahead.
    bool StormHold(double renderSeconds) const
    {
        if (!Driving())
            return false;
        std::lock_guard<std::mutex> lock(m_stormMutex);
        return StormHeldAtLocked(renderSeconds);
    }
    // All true iff StormHold, else all false.
    SafeSettings StormSafeSettings(double nowSeconds) const
    {
        SafeSettings s;
        s.laneT0 = s.nativeRebase = s.vectorsPaused = StormHold(nowSeconds);
        return s;
    }
    // The visible notice for the "resync_storm" fallback entry; false unless held (On only).
    bool StormNotice(std::string& out, double nowSeconds) const
    {
        if (!StormHold(nowSeconds))
            return false;
        try {
            int count = 0;
            {
                std::lock_guard<std::mutex> lock(m_stormMutex);
                count = m_storm.FullCount(nowSeconds);   // events at or before the rendered frame only
            }
            out = FallbackCatalog::ResyncStorm(true, count).reason;
            return true;
        } catch (...) {
            m_errors.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
    }

    // ---- D: unthrottled VPEV1 resync lines ----------------------------------------------------------
    // One line per recorded event, never rate-limited (the human log stays throttled):
    //   VPEV1 t=<ms> kind=resync level=full|minor|timing cause=<token> seq=<n> gap=<n> mode=shadow|on readers=pending
    // t is nowSeconds * 1000 (the caller's steady clock), mode the Shadow mode; the order follows the
    // vplog grammar (level cause seq gap mode, then the extra key). readers=pending always: readers
    // act at the seal, after the event is recorded (EventLines shows who acted later). No sink set
    // = no string is built. The sink runs on the recording thread (the queue mutex may be held).
    void SetEventSink(std::function<void(const std::string&)> sink)
    {
        const bool has = static_cast<bool>(sink);
        std::atomic_store(&m_eventSink, std::make_shared<const std::function<void(const std::string&)>>(std::move(sink)));
        m_hasEventSink.store(has, std::memory_order_release);
    }

    // Before frames flow (or from the render thread); safe against a concurrent OnEnqueue.
    void SetLogger(std::function<void(const std::string&)> logger)
    {
        std::atomic_store(&m_logger, std::make_shared<const std::function<void(const std::string&)>>(std::move(logger)));
    }

    // The current Indicator (shadow only), for tests and later consumers.
    std::shared_ptr<VpIndicator::Indicator> GetIndicator() const { return std::atomic_load(&m_indicator); }

    // Exceptions swallowed so far (OnEnqueue, StatusLine, EventLines, logger).
    uint32_t Errors() const { return m_errors.load(std::memory_order_relaxed); }

private:
    bool BuildStatus(std::string& out, double nowSeconds) const
    {
        std::string text = Driving() ? "on: " : "shadow: ";
        if (m_lastFullValid.load(std::memory_order_acquire)) {
            const double age = nowSeconds - m_lastFullTime.load(std::memory_order_relaxed);
            const long long seconds = age > 0.0 ? static_cast<long long>(age) : 0;
            text += "FULL " + std::to_string(seconds) + " s ago (" +
                Detail(static_cast<Cause>(m_lastFullCause.load(std::memory_order_relaxed)),
                       m_lastFullDetail.load(std::memory_order_relaxed)) + ")";
        } else {
            text += "no FULL yet";
        }
        const uint32_t minors = MinorInWindow(nowSeconds);
        text += "; minor x" + std::to_string(minors) + (minors >= kMinorRing ? "+" : "") + " / 10 min";
        text += "; timing x" + std::to_string(m_timingCount.load(std::memory_order_relaxed));
        int storm = 0;
        bool engaged = false;
        {
            std::lock_guard<std::mutex> lock(m_stormMutex);
            engaged = m_storm.Engaged(StormNow(nowSeconds));
            storm = m_storm.FullCount(StormNow(nowSeconds));
        }
        if (engaged)
            text += "; storm: " + std::to_string(storm) + " FULL in 10 s (would hold safe settings)";
        if (const std::shared_ptr<VpIndicator::Indicator> indicator = std::atomic_load(&m_indicator)) {
            const uint32_t early = indicator->PrematureFolds();
            // Folds before the first Seal also count as premature; shadow mode never seals (no
            // render-thread call site until R2), so the clause is shown only once frames are sealed.
            // The raw count is always in EventLines (premature_folds=).
            if (early > 0 && indicator->HasSealed())
                text += "; indicator ring too small (" + std::to_string(early) + " early folds)";
        }
        if (m_stormSourceTime.load(std::memory_order_acquire)) {
            const uint64_t missing = m_stormClockMissing.load(std::memory_order_relaxed);
            text += "; storm clock: source time";
            if (missing > 0)
                text += " (" + std::to_string(missing) + " events without source time)";
        }
        const uint32_t errors = m_errors.load(std::memory_order_relaxed);
        if (errors > 0)
            text += "; shadow error x" + std::to_string(errors);
        out = text;
        return true;
    }

    bool BuildEvents(std::string& out) const
    {
        const std::shared_ptr<VpIndicator::Indicator> indicator = std::atomic_load(&m_indicator);
        if (!indicator)
            return false;
        VpIndicator::Event events[VpIndicator::Indicator::kMinCapacity];
        const size_t n = indicator->Snapshot(events, VpIndicator::Indicator::kMinCapacity);
        std::string text = "era=" + std::to_string(indicator->Era()) +
            " capacity=" + std::to_string(indicator->Capacity()) +
            " ancient_below=" + std::to_string(indicator->AncientBelow()) +
            " premature_folds=" + std::to_string(indicator->PrematureFolds()) +
            " clamped=" + std::to_string(indicator->ClampedAdvances()) + "; ";
        for (size_t i = 0; i < n; ++i) {
            const VpIndicator::Event& e = events[i];
            if (i != 0)
                text += " | ";
            text += "#" + std::to_string(e.serial) + " seq=" + std::to_string(e.fromSeq) + " " +
                CounterToken(e.counter) + " " + Token(e.cause);
            if (e.detail != 0)
                text += " detail=" + DetailText(e.cause, e.detail);
            if (e.gapFrames != 0)
                text += " gap=" + std::to_string(e.gapFrames);
            text += " readers=";
            bool any = false;
            for (int r = 0; r < 64; ++r) {
                if (((e.actedReaders >> unsigned(r)) & 1u) == 0)
                    continue;
                if (any)
                    text += ",";
                text += indicator->ReaderName(r);
                any = true;
            }
            if (!any)
                text += "none";
        }
        out = text;
        return true;
    }


    static constexpr size_t kMinorRing = 256;
    static constexpr double kMinorWindowSeconds = 600.0;

    // Shadow or On: the detector runs and the Indicator is written.
    bool IsShadow() const { return m_mode.load(std::memory_order_relaxed) != static_cast<uint8_t>(Mode::Off); }

    static bool IsPowerOfTwo(uint32_t n) { return n != 0 && (n & (n - 1)) == 0; }

    static const char* CounterToken(VpIndicator::Counter c)
    {
        switch (c) {
        case VpIndicator::kFull: return "full";
        case VpIndicator::kContinuity: return "continuity";
        case VpIndicator::kTiming: return "timing";
        case VpIndicator::kRebaseH: return "rebase_h";
        case VpIndicator::kRebaseV: return "rebase_v";
        default: return "other";
        }
    }

    static std::string DetailText(Cause cause, uint8_t detail)
    {
        if (cause == Cause::StartAfterSourceChange)
            return Describe(static_cast<RefusalReason>(detail));
        return std::to_string(static_cast<unsigned>(detail));
    }

    // "cause" or "cause; detail".
    static std::string Detail(Cause cause, uint8_t detail)
    {
        std::string text = Describe(cause);
        if (detail != 0)
            text += "; " + DetailText(cause, detail);
        return text;
    }

    void ResetSummary()
    {
        m_lastFullValid.store(false, std::memory_order_relaxed);
        m_lastFullCause.store(0, std::memory_order_relaxed);
        m_lastFullDetail.store(0, std::memory_order_relaxed);
        m_lastFullTime.store(0.0, std::memory_order_relaxed);
        m_minorCount.store(0, std::memory_order_relaxed);
        m_timingCount.store(0, std::memory_order_relaxed);
        m_minorNext.store(0, std::memory_order_relaxed);
        for (size_t i = 0; i < kMinorRing; ++i)
            m_minorTimes[i].store(-1.0e30, std::memory_order_relaxed);
        std::lock_guard<std::mutex> lock(m_stormMutex);
        m_storm = StormWindow();
        m_stormHeld = false;
        m_stormLastFull = 0.0;
        m_stormHistoryCount = 0;
        m_stormHistoryNext = 0;
    }

    uint32_t MinorInWindow(double nowSeconds) const
    {
        uint32_t n = 0;
        for (size_t i = 0; i < kMinorRing; ++i) {
            const double age = nowSeconds - m_minorTimes[i].load(std::memory_order_relaxed);
            if (age >= 0.0 && age <= kMinorWindowSeconds)
                ++n;
        }
        return n;
    }

    void Log(const std::string& text) const
    {
        const std::shared_ptr<const std::function<void(const std::string&)>> logger = std::atomic_load(&m_logger);
        if (!logger || !*logger)
            return;
        try {
            (*logger)(text);
        } catch (...) {
            m_errors.fetch_add(1, std::memory_order_relaxed);
        }
    }

    // vp-event/1: one VPEV1 line per recorded event. The logger copy is rate-limited (human log);
    // the event sink (SetEventSink, the soak report) gets EVERY event, unthrottled, in the same
    // vp-event/1 format. No sink and no logger limit hit = nothing built beyond the limiter check.
    void EmitEventLine(const Event& e, double nowSeconds)
    {
        try {
            const char* level = e.level == Level::Full ? "full" : (e.level == Level::Minor ? "minor" : "timing");
            std::vector<VpEvent::Field> fields;
            fields.emplace_back("level", level);
            fields.emplace_back("cause", Token(e.cause));
            fields.emplace_back("seq", static_cast<uint64_t>(e.fromSeq));
            if (e.gapFrames != 0)
                fields.emplace_back("gap", static_cast<uint64_t>(e.gapFrames));
            if (e.detail != 0)
                fields.emplace_back("detail", static_cast<uint64_t>(e.detail));
            fields.emplace_back("mode", Driving() ? "on" : "shadow");
            const double tMs = nowSeconds * 1000.0;
            // The guard keeps the common no-sink case off the shared_ptr atomic (MSVC's global spinlock).
            const std::shared_ptr<const std::function<void(const std::string&)>> sink =
                m_hasEventSink.load(std::memory_order_acquire) ? std::atomic_load(&m_eventSink)
                                                                : std::shared_ptr<const std::function<void(const std::string&)>>();
            if (sink && *sink) {
                // The vp-event/1 resync grammar (VpEventLine.h) has no `readers` key: readers act at the
                // seal of the carrying frame and are shown in Ctrl+Alt+D / the event list instead.
                (*sink)(VpEvent::FormatRange(VpEvent::Kind::resync, tMs, fields.data(), fields.size()));
            }
            uint32_t suppressed = 0;
            if (!m_eventLimiter.Allow(VpEvent::Kind::resync, tMs, suppressed))
                return;
            if (suppressed != 0)
                fields.emplace_back("suppressed", static_cast<uint64_t>(suppressed));
            Log(VpEvent::FormatRange(VpEvent::Kind::resync, tMs, fields.data(), fields.size()));
        } catch (...) {
            m_errors.fetch_add(1, std::memory_order_relaxed);
        }
    }

    // m_stormMutex held. The recorded FULL with the LATEST time at or before `seconds` decides (ties:
    // the newest recorded): held then, and fewer than 10 s since the last counted FULL at that point.
    // Taking the latest time, not the newest record, keeps an enqueue-ahead entry of a new (lower)
    // source timebase from deciding a frame of the old one.
    bool StormHeldAtLocked(double seconds) const
    {
        const StormEntry* best = nullptr;
        for (size_t k = 0; k < m_stormHistoryCount; ++k) {
            const StormEntry& h = m_stormHistory[(m_stormHistoryNext + kStormHistory - 1 - k) % kStormHistory];
            if (h.seconds <= seconds && (best == nullptr || h.seconds > best->seconds))
                best = &h;
        }
        return best != nullptr && best->held && (seconds - best->lastFull) < kStormReleaseSeconds;
    }

    double StormNow(double nowSeconds) const
    {
        return m_stormSourceTime.load(std::memory_order_acquire) ? m_stormClock.load(std::memory_order_relaxed) : nowSeconds;
    }

    // The time the storm window runs on for this event: nowSeconds, or in source-time mode the
    // event's source seconds (kept as the storm clock).
    double StormTime(double nowSeconds, double sourceSeconds)
    {
        if (!m_stormSourceTime.load(std::memory_order_acquire))
            return nowSeconds;
        if (sourceSeconds < 0.0 || !std::isfinite(sourceSeconds)) {
            m_stormClockMissing.fetch_add(1, std::memory_order_relaxed);
            return m_stormClock.load(std::memory_order_relaxed);
        }
        AdvanceStormClock(sourceSeconds);
        return sourceSeconds;
    }

    void Record(VpIndicator::Indicator& indicator, const Event& e, double nowSeconds, double stormNow)
    {
        VpIndicator::Counter counter = VpIndicator::kContinuity;
        const char* label = "MINOR";
        bool log = false;
        switch (e.level) {
        case Level::Full:
            counter = VpIndicator::kFull;
            label = "FULL";
            {
                std::lock_guard<std::mutex> lock(m_stormMutex);
                if (m_stormHeld && (stormNow - m_stormLastFull) >= kStormReleaseSeconds)
                    m_stormHeld = false;   // 10 s with no FULL since the last: released before this one counts
                m_storm.Note(e.cause, stormNow);
                if (LevelOf(e.cause) == Level::Full && !ExcludedFromStorm(e.cause)) {
                    m_stormLastFull = stormNow;
                    if (m_storm.Engaged(stormNow))
                        m_stormHeld = true;
                }
                // The state after this FULL, keyed by its time: StormHold reads the newest entry at or
                // before the rendered frame's time (never an event of a frame still queued).
                StormEntry& h = m_stormHistory[m_stormHistoryNext];
                h.seconds = stormNow;
                h.held = m_stormHeld;
                h.lastFull = m_stormLastFull;
                m_stormHistoryNext = (m_stormHistoryNext + 1) % kStormHistory;
                if (m_stormHistoryCount < kStormHistory)
                    ++m_stormHistoryCount;
            }
            m_lastFullCause.store(static_cast<uint16_t>(e.cause), std::memory_order_relaxed);
            m_lastFullDetail.store(e.detail, std::memory_order_relaxed);
            m_lastFullTime.store(nowSeconds, std::memory_order_relaxed);
            m_lastFullValid.store(true, std::memory_order_release);
            log = true;
            break;
        case Level::Minor:
        {
            counter = VpIndicator::kContinuity;
            const uint32_t count = m_minorCount.fetch_add(1, std::memory_order_relaxed) + 1u;
            const uint32_t slot = m_minorNext.fetch_add(1, std::memory_order_relaxed);
            m_minorTimes[slot % kMinorRing].store(nowSeconds, std::memory_order_relaxed);
            log = count == 1 || IsPowerOfTwo(count);
            break;
        }
        case Level::Timing:
        {
            counter = VpIndicator::kTiming;
            label = "TIMING";
            const uint32_t count = m_timingCount.fetch_add(1, std::memory_order_relaxed) + 1u;
            log = count == 1 || IsPowerOfTwo(count);
            break;
        }
        default:
            return;
        }
        indicator.Advance(counter, e.cause, e.fromSeq, e.gapFrames, e.detail);
        EmitEventLine(e, nowSeconds);
        if (log) {
            std::string text = std::string(Driving() ? "Resync (on): " : "Resync (shadow): ") + label + " at seq " +
                std::to_string(e.fromSeq) + " (" + Detail(e.cause, e.detail);
            if (e.gapFrames != 0)
                text += "; gap " + std::to_string(e.gapFrames);
            text += ")";
            Log(text);
        }
    }

    std::atomic<uint8_t> m_mode{static_cast<uint8_t>(Mode::Off)};
    std::atomic<bool> m_restart{false};
    std::shared_ptr<VpIndicator::Indicator> m_indicator;                                 // atomic_load / atomic_store only
    std::shared_ptr<const std::function<void(const std::string&)>> m_logger;             // atomic_load / atomic_store only

    VpEvent::Limiter m_eventLimiter;
    ArmBox m_arms;
    Detector m_detector;   // queue mutex
    std::atomic<double> m_windowedRateHz{0.0};

    mutable std::mutex m_stormMutex;
    StormWindow m_storm;   // m_stormMutex
    std::atomic<bool> m_hasEventSink{false};
    std::atomic<bool> m_stormSourceTime{false};
    std::atomic<double> m_stormClock{0.0};            // source-time mode: the latest source second seen
    std::atomic<uint64_t> m_stormClockMissing{0};
    bool m_stormHeld = false;        // m_stormMutex
    double m_stormLastFull = 0.0;    // m_stormMutex; time of the last counted FULL
    static constexpr double kStormReleaseSeconds = 10.0;
    struct StormEntry {
        double seconds = 0.0;
        bool held = false;
        double lastFull = 0.0;
    };
    static constexpr size_t kStormHistory = 64;   // FULLs; far more than any render-behind-enqueue lag
    StormEntry m_stormHistory[kStormHistory];      // m_stormMutex
    size_t m_stormHistoryCount = 0;                // m_stormMutex
    size_t m_stormHistoryNext = 0;                 // m_stormMutex
    std::atomic<uint64_t> m_cursorSeq{0};
    std::shared_ptr<const std::function<void(const std::string&)>> m_eventSink;   // atomic_load / atomic_store only

    std::atomic<bool> m_lastFullValid{false};
    std::atomic<uint16_t> m_lastFullCause{0};
    std::atomic<uint8_t> m_lastFullDetail{0};
    std::atomic<double> m_lastFullTime{0.0};
    std::atomic<uint32_t> m_minorCount{0};
    std::atomic<uint32_t> m_timingCount{0};
    std::atomic<uint32_t> m_minorNext{0};
    mutable std::atomic<uint32_t> m_errors{0};
    std::atomic<double> m_minorTimes[kMinorRing];
};

} // namespace VpResync
