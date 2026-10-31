// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <VP-V/core/Indicator.h>
#include <VP-V/core/ResyncCause.h>
#include <VP-V/modules/input/signal/ResyncEvent.h>

#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

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
    enum class Mode : uint8_t { Off, Shadow };

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
        if (mode == Mode::Shadow) {
            const size_t want = VpIndicator::Indicator::RequiredCapacity(framesInFlight);
            const std::shared_ptr<VpIndicator::Indicator> current = std::atomic_load(&m_indicator);
            if (previous != Mode::Shadow || !current || want > current->Capacity()) {
                std::atomic_store(&m_indicator,
                                  std::make_shared<VpIndicator::Indicator>(VpIndicator::NextEra(), want));
                m_restart.store(true, std::memory_order_release);
            }
        }
        m_mode.store(static_cast<uint8_t>(mode), std::memory_order_release);
    }

    // Relaxed load: true in Shadow mode. The renderer gates its whole facts block on this.
    bool Active() const { return IsShadow(); }

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

    // The caller holds the queue mutex. nowSeconds is a steady clock used for display and the storm
    // window only; no decision depends on it.
    void OnEnqueue(const EnqueueFacts& facts, double nowSeconds)
    {
        if (!IsShadow())
            return;
        try {
            if (m_restart.exchange(false, std::memory_order_acq_rel)) {
                m_detector.Restart();
                ResetSummary();
            }
            const std::shared_ptr<VpIndicator::Indicator> indicator = std::atomic_load(&m_indicator);
            if (!indicator)
                return;
            const Decision decision = m_detector.OnEnqueue(facts, m_arms);
            for (uint8_t i = 0; i < decision.count; ++i)
                Record(*indicator, decision.events[i], nowSeconds);
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
        std::string text = "shadow: ";
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
            engaged = m_storm.Engaged(nowSeconds);
            storm = m_storm.FullCount(nowSeconds);
        }
        if (engaged)
            text += "; storm: " + std::to_string(storm) + " FULL in 10 s (would hold safe settings)";
        if (const std::shared_ptr<VpIndicator::Indicator> indicator = std::atomic_load(&m_indicator)) {
            const uint32_t early = indicator->PrematureFolds();
            if (early > 0)
                text += "; indicator ring too small (" + std::to_string(early) + " early folds)";
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

    bool IsShadow() const { return m_mode.load(std::memory_order_relaxed) == static_cast<uint8_t>(Mode::Shadow); }

    static bool IsPowerOfTwo(uint32_t n) { return n != 0 && (n & (n - 1)) == 0; }

    static const char* CounterToken(VpIndicator::Counter c)
    {
        switch (c) {
        case VpIndicator::kFull: return "full";
        case VpIndicator::kContinuity: return "continuity";
        case VpIndicator::kTiming: return "timing";
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

    void Record(VpIndicator::Indicator& indicator, const Event& e, double nowSeconds)
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
                m_storm.Note(e.cause, nowSeconds);
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
        if (log) {
            std::string text = std::string("Resync (shadow): ") + label + " at seq " +
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

    ArmBox m_arms;
    Detector m_detector;   // queue mutex

    mutable std::mutex m_stormMutex;
    StormWindow m_storm;   // m_stormMutex

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
