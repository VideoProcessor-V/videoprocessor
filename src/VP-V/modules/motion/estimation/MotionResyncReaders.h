// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <VP-V/core/Indicator.h>
#include <VP-V/core/ResyncCause.h>
#include <VP-V/modules/input/signal/ResyncDrive.h>
#include <VP-V/modules/motion/estimation/MotionValidity.h>

#include <array>
#include <cstddef>
#include <cstdint>

// The motion family as readers of the central Indicator (resync_event = on). Two readers, both
// called on the render thread at the seal of the frame that carries the change:
//
//   motion-history   moves MotionValidity's epoch (State::OnSealedChange) and collects what the
//                    consumer must forget at its apply point (Take: same style as Drive::Take).
//   vector-producer  re-keys the look-ahead producer's engage generation to Drive::FullKey at the
//                    carrying frame and records the in-flight job generation to drop, WITHOUT a
//                    restart or a back-off; a continuity change only sets the "skip pairs
//                    spanning" note (pair validity itself is State::PairValidAt).
//
// They are not entries of VpResync::Reader / Drive: they are plain registrations a FrameSeal
// provider calls later (RegisterOn). Like Drive's readers they LATCH on their first observe: the
// Indicator reports every counter then, and a reader acts only on what changed AT that frame (the
// stamp of seq against the stamp of seq - 1), never on the history of the era.
//
// Header-only, C++14. Render thread only (callbacks, Take, counters).
namespace VpMotionResync {

using VpIndicator::Bit;
using VpIndicator::CounterMask;
using VpIndicator::Policy;

enum class Reader : uint8_t { MotionHistory, VectorProducer, Count };

struct ReaderSpec {
    const char* name;
    CounterMask reads;
    std::array<Policy, VpIndicator::kCounterCount> policies;
};

inline const ReaderSpec& SpecOf(Reader r)
{
    using namespace VpIndicator;
    static const ReaderSpec specs[static_cast<size_t>(Reader::Count)] = {
        // F Reset (release resources when the FULL cause releases), M Reset ("forget"),
        // rebase axes Reset ("forget re-based-plane holders only"), settings[motion] Reset.
        { "motion-history",
          Bit(kFull) | Bit(kContinuity) | Bit(kRebaseH) | Bit(kRebaseV) | Bit(kSettingsMotion),
          [] {
              std::array<Policy, kCounterCount> p{};
              p[kFull] = Policy::Reset;
              p[kContinuity] = Policy::Reset;
              p[kRebaseH] = Policy::Reset;
              p[kRebaseV] = Policy::Reset;
              p[kSettingsMotion] = Policy::Reset;
              return p;
          }() },
        // F and settings[motion]: re-key, drop the in-flight job, no restart. M: note only.
        { "vector-producer",
          Bit(kFull) | Bit(kContinuity) | Bit(kSettingsMotion),
          [] {
              std::array<Policy, kCounterCount> p{};
              p[kFull] = Policy::Reset;
              p[kContinuity] = Policy::Hold;
              p[kSettingsMotion] = Policy::Reset;
              return p;
          }() },
    };
    return specs[static_cast<size_t>(r) < static_cast<size_t>(Reader::Count) ? static_cast<size_t>(r) : 0];
}

// What the motion-history consumer must do at its apply point (cleared by Take).
struct HistoryPending {
    bool changed = false;                 // the epoch moved at a seal: forget vector-derived history
    bool forgetRebaseHolders = false;     // a rebase axis (or a FULL) moved: forget re-based-plane holders only
    bool release = false;                 // a FULL from a source/format change: release resources
    MotionValidity::Cause cause = MotionValidity::Cause::Gap;   // the last cause applied
    unsigned moves = 0;                   // epoch moves since the last Take
    CounterMask counters = 0;             // the counters that changed since the last Take
};

// What the vector producer must do (cleared by TakeProducerNote).
struct ProducerNote {
    bool drop = false;                    // drop the job in flight under droppedInFlightGeneration
    uint64_t droppedInFlightGeneration = 0;   // the old engage generation (0 = none)
    uint64_t engageGeneration = 0;        // the new one: Drive::FullKey at the carrying seq
    bool skipSpanning = false;            // a continuity change: skip pairs spanning it
};

// The readers' state. Must outlive the Indicator's callbacks (the callbacks hold its address).
class Readers {
public:
    Readers() = default;
    Readers(const Readers&) = delete;
    Readers& operator=(const Readers&) = delete;

    // motion-history
    HistoryPending TakeHistory()
    {
        const HistoryPending p = m_history;
        m_history = HistoryPending();
        return p;
    }
    const HistoryPending& PeekHistory() const { return m_history; }

    // vector-producer
    ProducerNote TakeProducerNote()
    {
        const ProducerNote n = m_note;
        m_note.drop = false;
        m_note.droppedInFlightGeneration = 0;
        m_note.skipSpanning = false;
        return n;   // engageGeneration stays current
    }
    uint64_t EngageGeneration() const { return m_note.engageGeneration; }
    uint64_t RestartsAvoided() const { return m_restartsAvoided; }
    uint64_t InFlightDrops() const { return m_inFlightDrops; }

    // Test hook: callbacks that ACTED (a relevant change AT the carrying frame), one per frame,
    // and all callbacks (including the latching first one).
    unsigned HistoryActs() const { return m_historyActs; }
    unsigned ProducerActs() const { return m_producerActs; }
    unsigned HistoryCalls() const { return m_historyCalls; }
    unsigned ProducerCalls() const { return m_producerCalls; }

    // Re-arm for a NEW Indicator (a new era; RegisterOn calls it): both readers latch again on their
    // first observe of the new Indicator (only what changed AT that frame acts), the cause lookup
    // forgets the old era's last seq, and the producer baseline is re-taken from the new Indicator.
    // Pending flags the consumer has not taken yet are kept (they are still owed).
    // baseline = Indicator::LatchBaseline() of the new era (0 = latch against the frame before).
    void Rearm(uint64_t baseline = 0)
    {
        m_historyLatched = false;
        m_historyHave = baseline != 0;   // cause lookups then search (baseline, seq]
        m_historyLast = baseline;
        m_producerLatched = false;
        m_latchBaseline = baseline;
        ++m_rearms;
    }
    unsigned Rearms() const { return m_rearms; }

    // Called by RegisterOn's callbacks (render thread).
    void OnHistory(VpIndicator::Indicator& ind, MotionValidity::State& state,
                   const VpIndicator::Indicator::Change& c)
    {
        ++m_historyCalls;
        CounterMask rel = c.relevant;
        if (!m_historyLatched) {
            m_historyLatched = true;
            rel &= VpIndicator::ChangedSinceLatch(ind, m_latchBaseline, c.seq);
        }
        const VpIndicator::Indicator::Change cc{ c.seq, c.changed, rel, c.policies };
        if (rel == 0) {
            m_historyLast = c.seq;
            m_historyHave = true;
            return;
        }
        VpResync::Cause full = VpResync::Cause::None, cont = VpResync::Cause::None;
        if ((rel & Bit(VpIndicator::kFull)) != 0)
            full = LatestCause(ind, VpIndicator::kFull, m_historyHave, m_historyLast, c.seq);
        else if ((rel & Bit(VpIndicator::kContinuity)) != 0)
            cont = LatestCause(ind, VpIndicator::kContinuity, m_historyHave, m_historyLast, c.seq);
        const MotionValidity::SealedResult r = state.OnSealedChange(cc, full, cont);
        ++m_historyActs;
        m_historyLast = c.seq;
        m_historyHave = true;
        m_history.counters |= rel;
        if (r.moved) {
            m_history.changed = true;
            m_history.cause = r.cause;
            m_history.moves += r.moves;
            if (r.release)
                m_history.release = true;
        }
        if ((rel & (Bit(VpIndicator::kRebaseH) | Bit(VpIndicator::kRebaseV))) != 0)
            m_history.forgetRebaseHolders = true;
    }

    void OnProducer(VpIndicator::Indicator& ind, const VpIndicator::Indicator::Change& c)
    {
        ++m_producerCalls;
        CounterMask rel = c.relevant;
        if (!m_producerLatched) {
            m_producerLatched = true;
            rel &= VpIndicator::ChangedSinceLatch(ind, m_latchBaseline, c.seq);
            // Baseline before the carrying frame: a change AT this frame then re-keys and drops.
            const uint64_t base = (m_latchBaseline != 0 && m_latchBaseline < c.seq) ? m_latchBaseline
                                                                                  : (c.seq == 0 ? 0 : c.seq - 1);
            m_note.engageGeneration = VpResync::Drive::FullKey(ind, base);
        }
        if (rel == 0)
            return;
        ++m_producerActs;
        if ((rel & (Bit(VpIndicator::kFull) | Bit(VpIndicator::kSettingsMotion))) != 0) {
            m_note.drop = true;
            m_note.droppedInFlightGeneration = m_note.engageGeneration;
            m_note.engageGeneration = VpResync::Drive::FullKey(ind, c.seq);
            ++m_inFlightDrops;
            ++m_restartsAvoided;   // the producer keeps running: no restart, no back-off count
        }
        if ((rel & Bit(VpIndicator::kContinuity)) != 0)
            m_note.skipSpanning = true;
    }

private:
    // The cause of the newest event of `counter` that applies to seq and is newer than `last`
    // (Indicator::LastCauseIn: lock-free, no allocation). Not found - none retained, or the interval
    // is older than the ring - is UNKNOWN = Cause::None, which maps conservatively (FULL -> Resync,
    // releasing resources; continuity -> Gap).
    static VpResync::Cause LatestCause(const VpIndicator::Indicator& ind, VpIndicator::Counter counter,
                                       bool haveLast, uint64_t last, uint64_t seq)
    {
        const uint64_t from = haveLast ? last : (seq > 0 ? seq - 1 : 0);
        VpResync::Cause cause = VpResync::Cause::None;
        if (!ind.LastCauseIn(counter, from, seq, cause))
            return VpResync::Cause::None;
        return cause;
    }

    HistoryPending m_history;
    ProducerNote m_note;
    uint64_t m_restartsAvoided = 0;
    uint64_t m_inFlightDrops = 0;
    unsigned m_historyActs = 0, m_producerActs = 0, m_historyCalls = 0, m_producerCalls = 0;
    unsigned m_rearms = 0;
    uint64_t m_historyLast = 0;
    bool m_historyHave = false;
    bool m_historyLatched = false;
    bool m_producerLatched = false;
    uint64_t m_latchBaseline = 0;
};

struct Ids {
    int motionHistory = -1;
    int vectorProducer = -1;
};

// Setup (before the Indicator's first Observe). `readers` and `state` must outlive the Indicator's
// observes. Returns the reader ids; -1 for a reader the Indicator refused (counted in its
// RejectedReaders; a refused reader never fires). This form does NOT bind `state` (the caller must
// have bound it to this same Indicator); FrameSeal providers use the shared_ptr overload below.
inline Ids RegisterOn(VpIndicator::Indicator& indicator, MotionValidity::State& state, Readers& readers)
{
    Ids ids;
    readers.Rearm(indicator.LatchBaseline());   // a new Indicator (era): latch again (from the inherited seal)
    VpIndicator::Indicator* ind = &indicator;
    MotionValidity::State* st = &state;
    Readers* rd = &readers;
    const ReaderSpec& history = SpecOf(Reader::MotionHistory);
    ids.motionHistory = indicator.Register(history.name, history.reads, history.policies,
        [ind, st, rd](const VpIndicator::Indicator::Change& c) { rd->OnHistory(*ind, *st, c); });
    const ReaderSpec& producer = SpecOf(Reader::VectorProducer);
    ids.vectorProducer = indicator.Register(producer.name, producer.reads, producer.policies,
        [ind, rd](const VpIndicator::Indicator::Change& c) { rd->OnProducer(*ind, c); });
    return ids;
}

// The FrameSeal provider entry (the form every host must use): binds `state` to `indicator` FIRST, so
// State::PairValidAt / StreamGenerationAt / Invalidate(SettingsChange ...) read and write the same
// Indicator the readers are registered on, then registers the readers. Without the bind an era swap
// left the State on the previous (dead) Indicator: pairs spanning a new-era FULL were accepted and
// settings[motion] advances were lost. A null Indicator unbinds the State and registers nothing.
inline Ids RegisterOn(const std::shared_ptr<VpIndicator::Indicator>& indicator, MotionValidity::State& state,
                      Readers& readers)
{
    if (!indicator) {
        state.Unbind();
        return Ids();
    }
    state.Bind(indicator);
    return RegisterOn(*indicator, state, readers);
}

} // namespace VpMotionResync
