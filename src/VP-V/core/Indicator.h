// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "ResyncCause.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

// The central Indicator: a small ring of "something changed from frame N on" events, plus a
// derived per-sequence Stamp (a vector of change counters). Frames and copies store NO stamp;
// StampOf(seq) is the only source, so a late event (a slip found after enqueue) re-stamps every
// queued frame at or after its fromSeq, including copies, without touching any frame.
//
// Header-only, pure C++14, deterministic. Writers (Advance, Seal, Observe bookkeeping) are
// serialised by one mutex that readers never take. StampOf and Snapshot are lock-free seqlock
// reads over relaxed std::atomic words (Boehm pattern), so there is no data race and no UB.
namespace VpIndicator {

enum Counter : uint8_t {
    kFull,
    kContinuity,
    kTiming,
    kRebaseH,
    kRebaseV,
    kGraph,
    kSettingsMotion,
    kSettingsRebase,
    kSettingsAnalysis,
    kSettingsOutput,
    kCounterCount
};

using CounterMask = uint32_t;

// 0 for an out-of-range counter (no undefined shift).
constexpr CounterMask Bit(Counter c)
{
    return static_cast<unsigned>(c) < static_cast<unsigned>(kCounterCount)
               ? (CounterMask(1u) << static_cast<unsigned>(c))
               : CounterMask(0);
}

constexpr CounterMask kAllCounters = (CounterMask(1u) << static_cast<unsigned>(kCounterCount)) - 1u;

// Implication: an event on kFull also moves continuity, rebaseH and rebaseV. Nothing else implies
// anything; kTiming never moves continuity.
constexpr CounterMask MovedBy(Counter c)
{
    return c == kFull ? (Bit(kFull) | Bit(kContinuity) | Bit(kRebaseH) | Bit(kRebaseV)) : Bit(c);
}

struct Stamp {
    uint32_t era = 0;
    bool ancient = false;
    std::array<uint32_t, kCounterCount> v{};
};

// True when both stamps are from the same Indicator era, neither is ancient, and every masked
// counter is equal.
inline bool SameFor(const Stamp& a, const Stamp& b, CounterMask m)
{
    if (a.era != b.era || a.ancient || b.ancient)
        return false;
    for (size_t i = 0; i < size_t(kCounterCount); ++i)
        if ((m & (CounterMask(1u) << i)) != 0 && a.v[i] != b.v[i])
            return false;
    return true;
}

// The counters that differ; all bits when the eras differ or either stamp is ancient.
inline CounterMask Changed(const Stamp& a, const Stamp& b)
{
    if (a.era != b.era || a.ancient || b.ancient)
        return kAllCounters;
    CounterMask out = 0;
    for (size_t i = 0; i < size_t(kCounterCount); ++i)
        if (a.v[i] != b.v[i])
            out |= CounterMask(1u) << i;
    return out;
}

struct Event {
    uint64_t serial = 0;
    uint32_t era = 0;
    uint64_t fromSeq = 0;
    Counter counter = kFull;
    VpResync::Cause cause = VpResync::Cause::None;
    uint8_t detail = 0;
    uint32_t gapFrames = 0;
    uint64_t actedReaders = 0;   // bit i = reader i was called for a change this event contributed to
};

enum class Policy : uint8_t { Ignore, Reset, Hold, Absorb, Relatch, Crossfade };

// Process-wide era source; never 0.
inline uint32_t NextEra()
{
    static std::atomic<uint32_t> counter(0);
    uint32_t e = counter.fetch_add(1, std::memory_order_relaxed) + 1u;
    while (e == 0)
        e = counter.fetch_add(1, std::memory_order_relaxed) + 1u;
    return e;
}

class Indicator {
public:
    static constexpr size_t kMinCapacity = 64;       // Ctrl+Alt+D shows the last 64
    static constexpr size_t kMaxEventsPerFrame = 4;  // content + overflow successor + timing + one render-thread writer
    static constexpr size_t kMaxReaders = 64;

    struct Change {
        uint64_t seq;
        CounterMask changed;
        CounterMask relevant;   // changed & the reader's mask
        const std::array<Policy, kCounterCount>* policies;
    };

    static size_t NextPow2(size_t n)
    {
        size_t p = 1;
        while (p < n)
            p <<= 1;
        return p;
    }

    // framesInFlight = queue capacity + look-ahead walk + history + render-ahead depth + lane lead
    // + 1. The graph supplies it from S0 on; R1 passes the queue's hard capacity + motion
    // look-ahead + render-ahead max.
    static size_t RequiredCapacity(size_t framesInFlight, size_t maxEventsPerFrame = kMaxEventsPerFrame)
    {
        const size_t want = NextPow2(framesInFlight * maxEventsPerFrame);
        return want < kMinCapacity ? kMinCapacity : want;
    }

    Indicator(uint32_t era, size_t capacity)
        : era_(era)
        , capacity_(NextPow2(capacity < kMinCapacity ? kMinCapacity : capacity))
        , mask_(capacity_ - 1)
        , ring_(capacity_)
    {
        for (Entry& e : ring_) {
            e.serial.store(0, std::memory_order_relaxed);
            e.fromSeq.store(0, std::memory_order_relaxed);
            e.meta.store(0, std::memory_order_relaxed);
            e.acted.store(0, std::memory_order_relaxed);
        }
        for (size_t i = 0; i < size_t(kCounterCount); ++i) {
            base_[i].store(0, std::memory_order_relaxed);
            total_[i].store(0, std::memory_order_relaxed);
        }
        version_.store(0, std::memory_order_relaxed);
        written_.store(0, std::memory_order_relaxed);
        ancientBelow_.store(0, std::memory_order_relaxed);
        sealedNext_.store(0, std::memory_order_relaxed);
        outOfOrder_.store(0, std::memory_order_relaxed);
        rejectedReaders_.store(0, std::memory_order_relaxed);
        prematureFolds_.store(0, std::memory_order_relaxed);
        unsealedObserves_.store(0, std::memory_order_relaxed);
        rejectedAdvances_.store(0, std::memory_order_relaxed);
        clampedAdvances_.store(0, std::memory_order_relaxed);
    }

    Indicator(const Indicator&) = delete;
    Indicator& operator=(const Indicator&) = delete;

    uint32_t Era() const { return era_; }
    size_t Capacity() const { return capacity_; }
    uint64_t AncientBelow() const { return ancientBelow_.load(std::memory_order_relaxed); }
    uint32_t Total(Counter c) const { return total_[size_t(c)].load(std::memory_order_relaxed); }
    uint32_t OutOfOrderObserves() const { return outOfOrder_.load(std::memory_order_relaxed); }
    uint32_t RejectedReaders() const { return rejectedReaders_.load(std::memory_order_relaxed); }
    int ReaderCount() const { return readerCount_.load(std::memory_order_acquire); }
    // Events folded out of the ring that still applied to unsealed frames (ring undersized).
    uint32_t PrematureFolds() const { return prematureFolds_.load(std::memory_order_relaxed); }
    // Observe(seq) calls for a seq that is not sealed yet (ignored).
    uint32_t UnsealedObserves() const { return unsealedObserves_.load(std::memory_order_relaxed); }
    // Advance calls with an out-of-range counter (ignored).
    uint32_t RejectedAdvances() const { return rejectedAdvances_.load(std::memory_order_relaxed); }
    // Advance calls whose fromSeq was raised above the last sealed frame.
    uint32_t ClampedAdvances() const { return clampedAdvances_.load(std::memory_order_relaxed); }

    // Last sealed sequence (0 when nothing is sealed yet; see HasSealed).
    uint64_t LastSealed() const
    {
        const uint64_t n = sealedNext_.load(std::memory_order_acquire);
        return n == 0 ? 0 : n - 1;
    }
    bool HasSealed() const { return sealedNext_.load(std::memory_order_acquire) != 0; }

    // Any thread. Returns the effective fromSeq (clamped above the last sealed frame), or 0 with
    // nothing written when the counter is out of range (counted in RejectedAdvances).
    uint64_t Advance(Counter counter, VpResync::Cause cause, uint64_t fromSeq,
                     uint32_t gapFrames = 0, uint8_t detail = 0)
    {
        if (static_cast<unsigned>(counter) >= static_cast<unsigned>(kCounterCount)) {
            rejectedAdvances_.fetch_add(1, std::memory_order_relaxed);
            return 0;
        }
        std::unique_lock<std::mutex> lock(writeMutex_);
        const uint64_t floorSeq = sealedNext_.load(std::memory_order_relaxed);   // LastSealed() + 1, or 0
        if (successor_) {
            // Handed over (HandOverTo): the event belongs to the successor era. Clamped above THIS era's
            // seal (those frames were sealed here), then written there; never lost in a dead era.
            const std::shared_ptr<Indicator> next = successor_;
            lock.unlock();
            forwardedAdvances_.fetch_add(1, std::memory_order_relaxed);
            if (fromSeq < floorSeq)
                clampedAdvances_.fetch_add(1, std::memory_order_relaxed);
            return next->Advance(counter, cause, fromSeq < floorSeq ? floorSeq : fromSeq, gapFrames, detail);
        }
        const uint64_t effective = fromSeq < floorSeq ? floorSeq : fromSeq;
        if (effective != fromSeq)
            clampedAdvances_.fetch_add(1, std::memory_order_relaxed);

        const uint64_t v = version_.load(std::memory_order_relaxed);
        version_.store(v + 1, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_release);

        const uint64_t written = written_.load(std::memory_order_relaxed);
        Entry& slot = ring_[size_t(written) & mask_];
        if (written >= capacity_) {
            // Fold the oldest entry (by write order) into the base counters.
            const uint64_t oldFrom = slot.fromSeq.load(std::memory_order_relaxed);
            if (oldFrom >= floorSeq)
                prematureFolds_.fetch_add(1, std::memory_order_relaxed);   // still applies to unsealed frames
            const Counter oldCounter = CounterOf(slot.meta.load(std::memory_order_relaxed));
            const CounterMask moved = MovedBy(oldCounter);
            for (size_t i = 0; i < size_t(kCounterCount); ++i)
                if ((moved & (CounterMask(1u) << i)) != 0)
                    base_[i].store(base_[i].load(std::memory_order_relaxed) + 1u, std::memory_order_relaxed);
            if (oldFrom > ancientBelow_.load(std::memory_order_relaxed))
                ancientBelow_.store(oldFrom, std::memory_order_relaxed);
        }
        slot.serial.store(written + 1, std::memory_order_relaxed);
        slot.fromSeq.store(effective, std::memory_order_relaxed);
        slot.meta.store(PackMeta(counter, cause, detail, gapFrames), std::memory_order_relaxed);
        slot.acted.store(0, std::memory_order_relaxed);
        written_.store(written + 1, std::memory_order_relaxed);
        total_[size_t(counter)].store(total_[size_t(counter)].load(std::memory_order_relaxed) + 1u,
                                      std::memory_order_relaxed);

        version_.store(v + 2, std::memory_order_release);
        return effective;
    }

    // Lock-free, any thread. Frames and copies store only their seq; this is the only stamp source.
    Stamp StampOf(uint64_t seq) const
    {
        for (unsigned attempt = 0;; ++attempt) {
            const uint64_t v1 = version_.load(std::memory_order_acquire);
            if ((v1 & 1u) != 0) {
                Backoff(attempt);
                continue;
            }
            Stamp s;
            s.era = era_;
            for (size_t i = 0; i < size_t(kCounterCount); ++i)
                s.v[i] = base_[i].load(std::memory_order_relaxed);
            const uint64_t ancientBelow = ancientBelow_.load(std::memory_order_relaxed);
            const uint64_t written = written_.load(std::memory_order_relaxed);
            const size_t n = written < capacity_ ? size_t(written) : capacity_;
            for (size_t k = 0; k < n; ++k) {
                const Entry& e = ring_[k];   // order-independent sum: slot order is irrelevant
                const uint64_t from = e.fromSeq.load(std::memory_order_relaxed);
                if (from > seq)
                    continue;
                const CounterMask moved = MovedBy(CounterOf(e.meta.load(std::memory_order_relaxed)));
                for (size_t i = 0; i < size_t(kCounterCount); ++i)
                    if ((moved & (CounterMask(1u) << i)) != 0)
                        ++s.v[i];
            }
            std::atomic_thread_fence(std::memory_order_acquire);
            if (version_.load(std::memory_order_relaxed) != v1) {
                Backoff(attempt);
                continue;
            }
            s.ancient = seq < ancientBelow;
            return s;
        }
    }

    // Render thread: the frame is sealed; later events can no longer reach it.
    // seq == UINT64_MAX is not a valid sequence (seq + 1 would wrap); it is ignored.
    Stamp Seal(uint64_t seq)
    {
        {
            std::lock_guard<std::mutex> lock(writeMutex_);
            if (seq != ~uint64_t(0) && sealedNext_.load(std::memory_order_relaxed) < seq + 1)
                sealedNext_.store(seq + 1, std::memory_order_release);
        }
        return StampOf(seq);
    }

    // Era handover (Shadow::Configure, before the new Indicator is published). Atomic with respect to
    // every writer of THIS Indicator (one critical section on its writer mutex):
    //   1. every retained event whose fromSeq is above this era's last sealed frame (it still applies to
    //      queued, unsealed frames) is re-written into `next`, oldest first, with its counter, cause,
    //      detail and gap; events for sealed frames stay here (they can no longer reach a frame);
    //   2. `next` inherits this era's seal floor (LastSealed() + 1), so HasSealed / LastSealed agree;
    //   3. from then on every Advance on this Indicator (an enqueue thread or a settings writer still
    //      holding it) is forwarded to `next`, clamped above this era's seal.
    // Returns the number of events carried. A second handover, a null or self `next` does nothing (0).
    // Lock order is always older era -> newer era. Events already folded out of the ring before the
    // handover cannot be carried; they are counted in PrematureFolds() as before.
    size_t HandOverTo(const std::shared_ptr<Indicator>& next)
    {
        // `next` must be a fresh era (never handed over itself): a handover cycle would make Latest and
        // the forwarded Advance loop forever.
        if (!next || next.get() == this || next->HandedOver())
            return 0;
        std::lock_guard<std::mutex> lock(writeMutex_);
        if (successor_)
            return 0;
        const uint64_t floorSeq = sealedNext_.load(std::memory_order_relaxed);
        {
            // The new era inherits the seal floor: frames sealed here stay sealed (a writer that asks
            // for "the first unsealed frame", LastSealed() + 1, gets the same answer in both eras, and
            // an advance can never land on - or uniformly below - a frame already rendered).
            // Frames before the last sealed one are ANCIENT in the new era (single-frame use only): the
            // new era's stamps no longer carry the events between them, so a pair reaching back across
            // the swap is refused instead of read as valid. The last sealed frame itself stays a normal
            // stamp: it is the baseline the readers latch from (LatchBaseline).
            std::lock_guard<std::mutex> nextLock(next->writeMutex_);
            if (next->sealedNext_.load(std::memory_order_relaxed) < floorSeq)
                next->sealedNext_.store(floorSeq, std::memory_order_release);
            const uint64_t ancient = floorSeq > 0 ? floorSeq - 1 : 0;
            if (next->ancientBelow_.load(std::memory_order_relaxed) < ancient) {
                const uint64_t v = next->version_.load(std::memory_order_relaxed);
                next->version_.store(v + 1, std::memory_order_relaxed);
                std::atomic_thread_fence(std::memory_order_release);
                next->ancientBelow_.store(ancient, std::memory_order_relaxed);
                next->version_.store(v + 2, std::memory_order_release);
            }
        }
        const uint64_t written = written_.load(std::memory_order_relaxed);
        const uint64_t retained = written < capacity_ ? written : uint64_t(capacity_);
        size_t carried = 0;
        for (uint64_t w = written - retained; w < written; ++w) {   // write order: oldest first
            const Entry& e = ring_[size_t(w) & mask_];
            const uint64_t from = e.fromSeq.load(std::memory_order_relaxed);
            if (from < floorSeq)
                continue;
            const uint64_t meta = e.meta.load(std::memory_order_relaxed);
            next->Advance(CounterOf(meta), static_cast<VpResync::Cause>((meta >> 8) & 0xFFFFu), from,
                          uint32_t(meta >> 32), uint8_t((meta >> 24) & 0xFFu));
            ++carried;
        }
        successor_ = next;
        std::atomic_store(&successorRead_, next);
        hasSuccessor_.store(true, std::memory_order_release);
        carriedEvents_.store(uint32_t(carried), std::memory_order_relaxed);
        return carried;
    }
    // True once HandOverTo ran: this era is dead, its writers are forwarded.
    bool HandedOver() const { return hasSuccessor_.load(std::memory_order_acquire); }
    // Lock-free, any thread: the era this one handed over to (null when it has not).
    std::shared_ptr<Indicator> Successor() const
    {
        return hasSuccessor_.load(std::memory_order_acquire) ? std::atomic_load(&successorRead_)
                                                             : std::shared_ptr<Indicator>();
    }
    // The newest era reachable from `ind` through handovers (ind itself when it was never handed over).
    static std::shared_ptr<Indicator> Latest(std::shared_ptr<Indicator> ind)
    {
        while (ind) {
            std::shared_ptr<Indicator> next = ind->Successor();
            if (!next)
                break;
            ind = std::move(next);
        }
        return ind;
    }
    // The frame a reader registered on this Indicator latches against at its first observe: the last
    // frame sealed before the registration when the seal floor was inherited from a predecessor era
    // (HandOverTo), else 0 (= the frame before the observed one). Call at registration (before the
    // first Observe). With it, a carried event on a frame that was never sealed (dropped from the
    // queue) still differs from the baseline and acts, instead of being swallowed by the latch.
    uint64_t LatchBaseline() const { return HasSealed() ? LastSealed() : 0; }
    uint32_t CarriedEvents() const { return carriedEvents_.load(std::memory_order_relaxed); }
    uint32_t ForwardedAdvances() const { return forwardedAdvances_.load(std::memory_order_relaxed); }

    // Setup only (before the first Observe). Returns the reader index, or -1 beyond the limit
    // (counted in RejectedReaders).
    int Register(const char* name, CounterMask reads, const std::array<Policy, kCounterCount>& policies,
                 std::function<void(const Change&)> onChange)
    {
        const int n = readerCount_.load(std::memory_order_relaxed);
        if (n >= int(kMaxReaders) || observed_) {
            rejectedReaders_.fetch_add(1, std::memory_order_relaxed);
            return -1;
        }
        Reader& r = readers_[size_t(n)];
        r.name = name;
        r.reads = reads;
        r.policies = policies;
        r.onChange = std::move(onChange);
        readerCount_.store(n + 1, std::memory_order_release);   // the reader is complete before it is counted
        return n;
    }

    const char* ReaderName(int i) const
    {
        return (i >= 0 && i < readerCount_.load(std::memory_order_acquire)) ? readers_[size_t(i)].name : "";
    }

    // Render thread: seal seq, then observe it.
    CounterMask SealAndObserve(uint64_t seq)
    {
        Seal(seq);
        return Observe(seq);
    }

    // Render thread, at the seal of seq. Returns the changed mask (0 when out of order, or when
    // seq is not sealed yet: counted in UnsealedObserves, no callbacks).
    // A reader callback may call Advance (it is clamped above the seal) but must NOT call Observe
    // or SealAndObserve: re-entrant Observe is not guarded.
    // Note: PrematureFolds also counts folds made before the first Seal (nothing sealed yet means
    // every folded event still applies to unsealed frames).
    CounterMask Observe(uint64_t seq)
    {
        if (!HasSealed() || seq > LastSealed()) {
            unsealedObserves_.fetch_add(1, std::memory_order_relaxed);
            return 0;
        }
        if (observed_ && seq < lastObservedSeq_) {
            outOfOrder_.fetch_add(1, std::memory_order_relaxed);
            return 0;
        }
        const Stamp stamp = StampOf(seq);
        const CounterMask changed = observed_ ? Changed(lastObservedStamp_, stamp) : kAllCounters;
        const int readerCount = readerCount_.load(std::memory_order_acquire);

        uint64_t acting = 0;
        for (int i = 0; i < readerCount; ++i)
            if ((readers_[size_t(i)].reads & changed) != 0)
                acting |= uint64_t(1) << unsigned(i);

        if (acting != 0) {
            std::lock_guard<std::mutex> lock(writeMutex_);
            const uint64_t written = written_.load(std::memory_order_relaxed);
            const size_t n = written < capacity_ ? size_t(written) : capacity_;
            for (size_t k = 0; k < n; ++k) {
                Entry& e = ring_[k];
                const uint64_t from = e.fromSeq.load(std::memory_order_relaxed);
                if (from > seq || (observed_ && from <= lastObservedSeq_))
                    continue;
                const CounterMask moved = MovedBy(CounterOf(e.meta.load(std::memory_order_relaxed)));
                uint64_t bits = 0;
                for (int i = 0; i < readerCount; ++i)
                    if (((acting >> unsigned(i)) & 1u) != 0 && (readers_[size_t(i)].reads & moved) != 0)
                        bits |= uint64_t(1) << unsigned(i);
                if (bits != 0)
                    e.acted.fetch_or(bits, std::memory_order_relaxed);
            }
        }

        observed_ = true;
        lastObservedSeq_ = seq;
        lastObservedStamp_ = stamp;

        // Callbacks run outside the writer mutex so a reader may call Advance.
        for (int i = 0; i < readerCount; ++i) {
            Reader& r = readers_[size_t(i)];
            const CounterMask relevant = r.reads & changed;
            if (relevant == 0 || !r.onChange)
                continue;
            const Change c{ seq, changed, relevant, &r.policies };
            r.onChange(c);
        }
        return changed;
    }

    // Retained events, oldest first, newest last (the newest `max` when more are retained).
    // Seqlock-consistent. Returns the number written.
    size_t Snapshot(Event* out, size_t max) const
    {
        if (out == nullptr || max == 0)
            return 0;
        for (unsigned attempt = 0;; ++attempt) {
            const uint64_t v1 = version_.load(std::memory_order_acquire);
            if ((v1 & 1u) != 0) {
                Backoff(attempt);
                continue;
            }
            const uint64_t written = written_.load(std::memory_order_relaxed);
            const size_t retained = written < capacity_ ? size_t(written) : capacity_;
            const size_t count = retained < max ? retained : max;
            const uint64_t first = written - count;   // write index of the first event returned
            for (size_t k = 0; k < count; ++k) {
                const Entry& e = ring_[size_t(first + k) & mask_];
                Event& ev = out[k];
                ev.serial = e.serial.load(std::memory_order_relaxed);
                ev.era = era_;
                ev.fromSeq = e.fromSeq.load(std::memory_order_relaxed);
                const uint64_t meta = e.meta.load(std::memory_order_relaxed);
                ev.counter = CounterOf(meta);
                ev.cause = static_cast<VpResync::Cause>((meta >> 8) & 0xFFFFu);
                ev.detail = uint8_t((meta >> 24) & 0xFFu);
                ev.gapFrames = uint32_t(meta >> 32);
                ev.actedReaders = e.acted.load(std::memory_order_relaxed);
            }
            std::atomic_thread_fence(std::memory_order_acquire);
            if (version_.load(std::memory_order_relaxed) != v1) {
                Backoff(attempt);
                continue;
            }
            return count;
        }
    }

    // Lock-free, any thread (seqlock read like Snapshot). Among retained events whose counter is
    // exactly `counter` (a kFull event is NOT returned for kContinuity) and whose fromSeq lies in
    // (fromSeqExclusive, toSeqInclusive], returns the cause (and detail, when non-null) of the one
    // with the highest serial. False when none is retained. Also false when the interval is older
    // than the ring (AncientBelow() > fromSeqExclusive: events there may have been folded away, so
    // "none" would be unreliable); callers can compare AncientBelow() to tell the two apart.
    bool LastCauseIn(Counter counter, uint64_t fromSeqExclusive, uint64_t toSeqInclusive,
                     VpResync::Cause& cause, uint8_t* detail = nullptr) const
    {
        if (static_cast<unsigned>(counter) >= static_cast<unsigned>(kCounterCount)
            || toSeqInclusive <= fromSeqExclusive)
            return false;
        for (unsigned attempt = 0;; ++attempt) {
            const uint64_t v1 = version_.load(std::memory_order_acquire);
            if ((v1 & 1u) != 0) {
                Backoff(attempt);
                continue;
            }
            const uint64_t ancientBelow = ancientBelow_.load(std::memory_order_relaxed);
            const uint64_t written = written_.load(std::memory_order_relaxed);
            const size_t n = written < capacity_ ? size_t(written) : capacity_;
            bool found = false;
            uint64_t bestSerial = 0;
            VpResync::Cause bestCause = VpResync::Cause::None;
            uint8_t bestDetail = 0;
            for (size_t k = 0; k < n; ++k) {
                const Entry& e = ring_[k];
                const uint64_t from = e.fromSeq.load(std::memory_order_relaxed);
                if (from <= fromSeqExclusive || from > toSeqInclusive)
                    continue;
                const uint64_t meta = e.meta.load(std::memory_order_relaxed);
                if (CounterOf(meta) != counter)
                    continue;
                const uint64_t serial = e.serial.load(std::memory_order_relaxed);
                if (!found || serial > bestSerial) {
                    found = true;
                    bestSerial = serial;
                    bestCause = static_cast<VpResync::Cause>((meta >> 8) & 0xFFFFu);
                    bestDetail = uint8_t((meta >> 24) & 0xFFu);
                }
            }
            std::atomic_thread_fence(std::memory_order_acquire);
            if (version_.load(std::memory_order_relaxed) != v1) {
                Backoff(attempt);
                continue;
            }
            if (ancientBelow > fromSeqExclusive)
                return false;
            if (!found)
                return false;
            cause = bestCause;
            if (detail != nullptr)
                *detail = bestDetail;
            return true;
        }
    }

    // Lock-free, any thread. True when ANY retained event of exactly `counter` with fromSeq in
    // (fromSeqExclusive, toSeqInclusive] has a cause other than `except`, or when the interval is
    // older than the ring (AncientBelow() > fromSeqExclusive: an event there may have been folded
    // away, so "only `except`" cannot be proven; the conservative answer). False for an empty
    // interval, an out-of-range counter, or when every event in the interval has cause `except`
    // (or there are none and the interval is within the ring). Used by the pulldown reader: absorb
    // only when EVERY FULL between two stamps was a LiveQueueReset.
    bool AnyCauseOtherThanIn(Counter counter, VpResync::Cause except, uint64_t fromSeqExclusive,
                             uint64_t toSeqInclusive) const
    {
        if (static_cast<unsigned>(counter) >= static_cast<unsigned>(kCounterCount)
            || toSeqInclusive <= fromSeqExclusive)
            return false;
        for (unsigned attempt = 0;; ++attempt) {
            const uint64_t v1 = version_.load(std::memory_order_acquire);
            if ((v1 & 1u) != 0) {
                Backoff(attempt);
                continue;
            }
            const uint64_t ancientBelow = ancientBelow_.load(std::memory_order_relaxed);
            const uint64_t written = written_.load(std::memory_order_relaxed);
            const size_t n = written < capacity_ ? size_t(written) : capacity_;
            bool other = false;
            for (size_t k = 0; k < n && !other; ++k) {
                const Entry& e = ring_[k];
                const uint64_t from = e.fromSeq.load(std::memory_order_relaxed);
                if (from <= fromSeqExclusive || from > toSeqInclusive)
                    continue;
                const uint64_t meta = e.meta.load(std::memory_order_relaxed);
                if (CounterOf(meta) != counter)
                    continue;
                if (static_cast<VpResync::Cause>((meta >> 8) & 0xFFFFu) != except)
                    other = true;
            }
            std::atomic_thread_fence(std::memory_order_acquire);
            if (version_.load(std::memory_order_relaxed) != v1) {
                Backoff(attempt);
                continue;
            }
            return other || ancientBelow > fromSeqExclusive;
        }
    }

private:
    struct Entry {
        std::atomic<uint64_t> serial;
        std::atomic<uint64_t> fromSeq;
        std::atomic<uint64_t> meta;    // counter | cause << 8 | detail << 24 | gapFrames << 32
        std::atomic<uint64_t> acted;
    };

    struct Reader {
        const char* name = "";
        CounterMask reads = 0;
        std::array<Policy, kCounterCount> policies{};
        std::function<void(const Change&)> onChange;
    };

    static uint64_t PackMeta(Counter c, VpResync::Cause cause, uint8_t detail, uint32_t gap)
    {
        return uint64_t(c) | (uint64_t(static_cast<uint16_t>(cause)) << 8) | (uint64_t(detail) << 24)
               | (uint64_t(gap) << 32);
    }
    static Counter CounterOf(uint64_t meta) { return static_cast<Counter>(meta & 0xFFu); }

    static void Backoff(unsigned attempt)
    {
        // Spin briefly, yield every 16th attempt, and past 1024 attempts yield on every spin.
        if (attempt >= 1024 || (attempt >= 16 && (attempt & 15u) == 0))
            std::this_thread::yield();
    }

    const uint32_t era_;
    const size_t capacity_;
    const size_t mask_;
    std::vector<Entry> ring_;

    mutable std::mutex writeMutex_;
    std::atomic<uint64_t> version_;
    std::atomic<uint64_t> written_;
    std::atomic<uint64_t> ancientBelow_;
    std::array<std::atomic<uint32_t>, kCounterCount> base_;
    std::array<std::atomic<uint32_t>, kCounterCount> total_;
    std::atomic<uint64_t> sealedNext_;   // LastSealed() + 1; 0 = nothing sealed
    std::atomic<uint32_t> outOfOrder_;
    std::atomic<uint32_t> rejectedReaders_;
    std::atomic<uint32_t> prematureFolds_;
    std::atomic<uint32_t> unsealedObserves_;
    std::atomic<uint32_t> rejectedAdvances_;
    std::atomic<uint32_t> clampedAdvances_;
    std::atomic<uint32_t> carriedEvents_{0};
    std::atomic<uint32_t> forwardedAdvances_{0};
    std::shared_ptr<Indicator> successor_;       // writeMutex_
    std::shared_ptr<Indicator> successorRead_;   // atomic_load / atomic_store only (lock-free readers)
    std::atomic<bool> hasSuccessor_{false};

    // Readers: registered at setup, called from the render thread only.
    std::array<Reader, kMaxReaders> readers_;
    std::atomic<int> readerCount_{0};   // written by Register (setup), read by status threads
    bool observed_ = false;
    uint64_t lastObservedSeq_ = 0;
    Stamp lastObservedStamp_;
};

// The counters a latching reader acts on at its first observe of `seq`: the stamp of seq against the
// stamp of `baseline` (Indicator::LatchBaseline at registration) when it is an earlier frame, else
// against seq - 1 (every counter for seq 0).
inline CounterMask ChangedSinceLatch(const Indicator& ind, uint64_t baseline, uint64_t seq)
{
    if (seq == 0)
        return kAllCounters;
    const uint64_t from = (baseline != 0 && baseline < seq) ? baseline : seq - 1;
    return Changed(ind.StampOf(from), ind.StampOf(seq));
}

// Ancient seqs are valid for single-frame use only: never temporal.
inline bool TemporalValid(const Indicator& ind, uint64_t seqA, uint64_t seqB, CounterMask m)
{
    return SameFor(ind.StampOf(seqA), ind.StampOf(seqB), m);
}

} // namespace VpIndicator
