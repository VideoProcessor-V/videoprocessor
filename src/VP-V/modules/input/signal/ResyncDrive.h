// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <VP-V/core/Indicator.h>
#include <VP-V/core/ResyncCause.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>

// resync_event = on (R3 / L1 / R4a): the central Indicator DRIVES the resets of the parts listed
// below. Each part is a READER: it declares which Indicator counters it reads and its policy per
// counter (design 25 v2, table A5), and keeps no epoch of its own.
//
// Two reader forms:
// - Render-thread readers are registered with the Indicator. The render loop seals each dequeued
//   frame and observes it (SealAndObserve); the Indicator tells every reader which of its counters
//   changed for that frame, and the reader's pending mask collects them. The part takes its mask
//   (Take) at its own call site, so every action happens where the part already runs, on the frame
//   that carries the change, before that frame is rendered.
// - Readers that run before the seal (the pulldown detector on the capture thread) keep a Cursor:
//   the stamp of the last frame they applied, compared with the stamp of the frame they use.
//
// Pair validity (the sigma-delta carry) compares StampOf(from) and StampOf(to) directly.
//
// Header-only, pure C++14. Render-thread only, except Cursor (one owner thread) and the static
// helpers (any thread: StampOf is a lock-free seqlock read).
namespace VpResync {

enum class Reader : uint8_t {
    RenderAhead,     // output.presentation: the render-ahead gate, its schedule and the de-judder phase
    Timeline,        // the VP interpolation timeline (one picture per refresh)
    SigmaDelta,      // the sigma-delta's next-frame vectors carried from A's field into B
    SceneDetector,   // the renderer's scene detector (Bill's component; on = an either/or option)
    SourceMonitor,   // the source-resolution monitor's evidence and worker jobs
    LedDrive,        // Dynamic Drive: look-ahead, gain, CNTR, frame-period smoothing
    Libplacebo,      // libplacebo's frame-mix cache and detected peak (L1, VP pathway only)
    RebaseDecision,  // the re-base decision product P0 (per-axis fade state)
    Count
};

using VpIndicator::Bit;
using VpIndicator::CounterMask;
using VpIndicator::Policy;

struct ReaderSpec {
    const char* name;
    CounterMask reads;
    std::array<Policy, VpIndicator::kCounterCount> policies;
};

// The policy table (design 25 v2, A5). Policies are declarative: they name what the part does on
// each counter; the part's call site implements it.
inline const ReaderSpec& SpecOf(Reader r)
{
    using namespace VpIndicator;
    auto P = [](Policy full, Policy minor, Policy timing, Policy axis) {
        std::array<Policy, kCounterCount> p{};
        p[kFull] = full;
        p[kContinuity] = minor;
        p[kTiming] = timing;
        p[kRebaseH] = axis;
        p[kRebaseV] = axis;
        return p;
    };
    static const ReaderSpec specs[static_cast<size_t>(Reader::Count)] = {
        { "render-ahead", Bit(kFull) | Bit(kContinuity) | Bit(kTiming),
          P(Policy::Reset, Policy::Relatch, Policy::Relatch, Policy::Ignore) },
        { "timeline", Bit(kFull) | Bit(kContinuity) | Bit(kTiming),
          P(Policy::Reset, Policy::Relatch, Policy::Relatch, Policy::Ignore) },
        { "sigma-delta", Bit(kFull) | Bit(kContinuity) | Bit(kRebaseH) | Bit(kRebaseV),
          P(Policy::Reset, Policy::Reset, Policy::Ignore, Policy::Reset) },
        { "scene-detector", Bit(kFull),
          P(Policy::Reset, Policy::Ignore, Policy::Ignore, Policy::Ignore) },
        { "source-monitor", Bit(kFull) | Bit(kContinuity),
          P(Policy::Reset, Policy::Hold, Policy::Ignore, Policy::Ignore) },
        { "led-drive", Bit(kFull) | Bit(kContinuity) | Bit(kTiming),
          P(Policy::Reset, Policy::Hold, Policy::Relatch, Policy::Ignore) },
        { "libplacebo", Bit(kFull),
          P(Policy::Reset, Policy::Ignore, Policy::Ignore, Policy::Ignore) },
        { "rebase-p0", Bit(kFull) | Bit(kContinuity) | Bit(kRebaseH) | Bit(kRebaseV),
          P(Policy::Reset, Policy::Hold, Policy::Ignore, Policy::Crossfade) },
    };
    return specs[static_cast<size_t>(r) < static_cast<size_t>(Reader::Count) ? static_cast<size_t>(r) : 0];
}

class Drive {
public:
    static constexpr size_t kReaders = static_cast<size_t>(Reader::Count);

    // Render thread: the frame is about to be rendered. Registers the readers on a new Indicator
    // (a new era: Configure built one), seals the frame and observes it. Returns the changed mask
    // (0 without an Indicator). The callbacks only collect pending masks.
    // Standalone path (today's renderer). With a FrameSeal use RegisterOn + OnObserved instead: the
    // FrameSeal owns the one seal per frame.
    CounterMask SealAndObserve(const std::shared_ptr<VpIndicator::Indicator>& indicator, uint64_t seq)
    {
        if (!indicator || seq == 0)
            return 0;
        if (indicator != m_registered)
            RegisterOn(indicator);
        m_lastSealed = seq;
        const CounterMask changed = indicator->SealAndObserve(seq);
        ApplyLatch(*indicator, seq);
        return changed;
    }

    // FrameSeal provider entry (render thread, before the Indicator's first Observe): registers the
    // readers on `indicator` and arms the entering-`on` latch. A no-op when already registered on it.
    // Readers that cannot be registered (the Indicator refused: already observed) keep id -1 and never
    // act; Indicator::RejectedReaders() counts them.
    void RegisterOn(const std::shared_ptr<VpIndicator::Indicator>& indicator)
    {
        if (!indicator || indicator == m_registered)
            return;
        m_registered = indicator;
        m_pending.fill(0);
        for (size_t i = 0; i < kReaders; ++i) {
            const ReaderSpec& spec = SpecOf(static_cast<Reader>(i));
            CounterMask* slot = &m_pending[i];
            m_ids[i] = indicator->Register(spec.name, spec.reads, spec.policies,
                [slot](const VpIndicator::Indicator::Change& c) { *slot |= c.relevant; });
        }
        m_latch = true;
        m_latchBaseline = indicator->LatchBaseline();   // a successor era: latch from the last sealed frame
    }

    // FrameSeal subscriber entry (render thread): the frame `seq` was sealed and observed by the
    // FrameSeal (no second seal). Applies the entering-`on` latch exactly like SealAndObserve and
    // returns `changed`. Ignored (returns `changed`) when `indicator` is not the one registered.
    CounterMask OnObserved(const std::shared_ptr<VpIndicator::Indicator>& indicator, uint64_t seq, CounterMask changed)
    {
        if (!indicator || seq == 0 || indicator != m_registered)
            return changed;
        m_lastSealed = seq;
        ApplyLatch(*indicator, seq);
        return changed;
    }

    // Render thread: the counters this reader saw change since its last Take (then cleared).
    CounterMask Take(Reader r)
    {
        const size_t i = static_cast<size_t>(r);
        if (i >= kReaders)
            return 0;
        const CounterMask m = m_pending[i];
        m_pending[i] = 0;
        return m;
    }

    // Without clearing (tests, status).
    CounterMask Peek(Reader r) const
    {
        const size_t i = static_cast<size_t>(r);
        return i < kReaders ? m_pending[i] : 0;
    }

    // Render thread: resync_event left `on`. Pending masks are dropped; the registration is kept
    // (an Indicator accepts readers only before its first Observe, and the callbacks stay valid for
    // this Drive), so a return to `on` on the same Indicator observes from where it stopped (and latches: only the changes AT that frame act).
    void Detach()
    {
        m_pending.fill(0);
        m_latch = true;   // the next `on` observe latches (see SealAndObserve)
        m_latchBaseline = 0;
    }

    const std::shared_ptr<VpIndicator::Indicator>& Registered() const { return m_registered; }
    uint64_t LastSealed() const { return m_lastSealed; }
    int ReaderIndex(Reader r) const
    {
        const size_t i = static_cast<size_t>(r);
        return i < kReaders ? m_ids[i] : -1;
    }

    // A generation key for parts that compare a generation number: the era and the `full` counter
    // of seq (any FULL, any era change gives a new key). Never 0.
    static uint64_t FullKey(const VpIndicator::Indicator& indicator, uint64_t seq)
    {
        const VpIndicator::Stamp s = indicator.StampOf(seq);
        return (uint64_t(s.era) << 32) | (uint64_t(s.v[VpIndicator::kFull]) + 1u);
    }

    // True when the frames a and b carry the same masked counters (same era, neither ancient).
    static bool PairValid(const VpIndicator::Indicator& indicator, uint64_t a, uint64_t b, CounterMask mask)
    {
        return VpIndicator::TemporalValid(indicator, a, b, mask);
    }

    // A reader that runs before the seal (one owner thread): the counters that changed between the
    // stamp it applied last and the stamp of seq. The first call, a new era or an ancient seq report
    // every counter.
    class Cursor {
    public:
        CounterMask Advance(const VpIndicator::Indicator& indicator, uint64_t seq)
        {
            const VpIndicator::Stamp s = indicator.StampOf(seq);
            const CounterMask changed = m_have ? VpIndicator::Changed(m_stamp, s) : VpIndicator::kAllCounters;
            m_stamp = s;
            m_have = true;
            return changed;
        }
        void Reset() { m_have = false; }
        bool Has() const { return m_have; }

    private:
        VpIndicator::Stamp m_stamp;
        bool m_have = false;
    };

private:
    void ApplyLatch(const VpIndicator::Indicator& indicator, uint64_t seq)
    {
        if (!m_latch)
            return;
        // Entering `on` (25a R5, matching the pulldown's Cursor): the first observe after a
        // registration (shadow -> on keeps the Indicator; off -> on builds a new era) or a return
        // to `on` (Detach) only LATCHES. The Indicator reports every counter on its first observe
        // (and everything since the last observe on a return); the readers act only on what
        // changed AT this frame - its stamp against the previous frame's - never on the history
        // of the era (a settings change is not a source event). An ancient previous frame
        // compares as "everything" (the conservative reset). The returned mask is unfiltered.
        // On a successor era (Indicator::HandOverTo) the baseline is the last frame sealed in the old era,
        // so an event carried onto a frame that was never sealed (dropped) still acts here.
        m_latch = false;
        const CounterMask atFrame = VpIndicator::ChangedSinceLatch(indicator, m_latchBaseline, seq);
        m_latchBaseline = 0;
        for (CounterMask& pending : m_pending)
            pending &= atFrame;
    }

    std::shared_ptr<VpIndicator::Indicator> m_registered;
    std::array<CounterMask, kReaders> m_pending{};
    std::array<int, kReaders> m_ids{};
    uint64_t m_lastSealed = 0;
    bool m_latch = true;
    uint64_t m_latchBaseline = 0;
};

} // namespace VpResync
