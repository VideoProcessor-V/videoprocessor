// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "LaneInterfaces.h"
#include "OrderedChain.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

// Skeleton S3: the lane runner. One state machine per product that drives an ILaneProducer through
// the S1 slot protocol: Claim, Writable, input pins (LaneAncestors), stamp (StampOf, after the pins),
// Begin, chunks, Publish (stamp last) or Abandon. It owns no thread: the host calls Step(product,
// budget) from that product's lane thread (OFA worker, CPU worker, or the render idle gap). Header-
// only, pure C++14, no Windows, no GPU. Clocks are injected so tests are deterministic.
//
// Timing decides WHEN a job runs, never WHAT it contains: the finished bytes depend only on the
// frame, its inputs and the producer's deterministic chunking. A job the seal took, or a tier-plan
// change took away, is cancelled at the next Step and its buffer returned (never a silent drop: every
// path has a counter the host shows).
namespace VpSkeleton {

enum class StepOutcome : uint8_t {
    NoProducer,     // nothing registered for the product
    NotLaned,       // the plan gave the product no lane
    WaitingSource,  // the next frame has not arrived
    WaitingSlot,    // claim too early / release pending / lane buffer still pinned
    WaitingInputs,  // an input has no content yet (retry next Step)
    WaitingDevice,  // producer fence not passed yet (publish or abandon retried)
    Ran,            // one chunk ran (a job in progress)
    Published,      // the job's result was published
    Late,           // the frame was skipped or its result was discarded by the seal
    Abandoned,      // the job was cancelled and its buffer returned
    Failed,         // the claim is out of range
};

struct LaneProductCounters {
    std::atomic<uint64_t> published{ 0 };
    std::atomic<uint64_t> staleSkips{ 0 };
    std::atomic<uint64_t> latePublishes{ 0 };
    std::atomic<uint64_t> skippedDropped{ 0 };   // source frame unknown or dropped
    std::atomic<uint64_t> cancelled{ 0 };        // the seal (or the plan) took a running job
    std::atomic<uint64_t> abandoned{ 0 };        // buffers returned without a result
    std::atomic<uint64_t> beginFailures{ 0 };
    std::atomic<uint64_t> chunkFailures{ 0 };
    std::atomic<uint64_t> laneUs{ 0 };           // measured producer time
    std::atomic<uint64_t> chainCarryForward{ 0 };// ordered chain: jobs run in the declared reduced form (no predecessor)
    std::atomic<uint64_t> chainRefused{ 0 };     // ordered chain: out-of-order work refused (never run, never published)
};

class LaneRunner {
public:
    LaneRunner(SlotTable& table, const VpIndicator::Indicator* indicator, std::function<uint64_t()> nowUs)
        : table_(table)
        , indicator_(indicator)
        , nowUs_(std::move(nowUs))
    {
        ProductId maxId = 0;
        for (size_t i = 0; i < table_.Count(); ++i)
            if (table_.NodeAt(i).id > maxId)
                maxId = table_.NodeAt(i).id;
        states_ = std::vector<State>(table_.Count() == 0 ? size_t(0) : size_t(maxId) + 1u);
        for (size_t i = 0; i < table_.Count(); ++i) {
            const Node& n = table_.NodeAt(i);
            State& st = states_[n.id];
            st.node = &n;
            st.set = &table_.At(i);
        }
    }

    LaneRunner(const LaneRunner&) = delete;
    LaneRunner& operator=(const LaneRunner&) = delete;

    // ---- setup (before the lane threads start) ----

    // One producer per product present in the table; others are ignored and counted.
    void Register(ILaneProducer* p)
    {
        const ProductId id = p ? p->Product() : kNoProduct;
        State* st = StateOf(id);
        if (!st || !st->node || st->producer) {
            ++ignoredProducers_;
            return;
        }
        st->producer = p;
    }

    void SetBuffers(std::function<uint8_t*(ProductId, int)> ring,
                    std::function<const uint8_t*(ProductId, uint64_t, int)> input,
                    std::function<size_t(ProductId)> bytes)
    {
        ring_ = std::move(ring);
        input_ = std::move(input);
        bytes_ = std::move(bytes);
    }

    // Copies the held view of `seq` (planes kept alive by its keepAlive) into *out; false: not held.
    // `out` may be null (presence test only).
    void SetSources(std::function<bool(uint64_t, SourceFrameView*)> find) { find_ = std::move(find); }
    void SetReleaseCompleted(std::function<uint64_t(ProductId)> f) { releaseCompleted_ = std::move(f); }
    void SetReadsSource(ProductId id, bool reads)
    {
        if (State* st = StateOf(id))
            st->readsSource = reads;
    }

    // ---- any thread ----

    void OnSourceFrame(uint64_t seq)
    {
        const uint64_t want = seq + 1u;   // newest arrived seq + 1; 0 = none yet
        uint64_t cur = arrived_.load(std::memory_order_relaxed);
        while (cur < want
               && !arrived_.compare_exchange_weak(cur, want, std::memory_order_release, std::memory_order_relaxed)) {
        }
    }

    void SetLaned(ProductId id, bool laned)
    {
        if (State* st = StateOf(id))
            st->laned.store(laned, std::memory_order_release);
    }
    bool Laned(ProductId id) const
    {
        const State* st = StateOf(id);
        return st && st->laned.load(std::memory_order_acquire);
    }
    void SetSkipBefore(uint64_t seq) { skipBefore_.store(seq, std::memory_order_release); }

    const LaneProductCounters& Counters(ProductId id) const
    {
        const State* st = StateOf(id);
        return st ? st->counters : none_;
    }
    uint64_t IgnoredProducers() const { return ignoredProducers_; }
    bool HasProducer(ProductId id) const
    {
        const State* st = StateOf(id);
        return st && st->producer != nullptr;
    }
    // The producer's prediction of its next chunk (0 = unknown or no producer). Lane thread of the product.
    uint32_t PredictChunkUs(ProductId id) const
    {
        const State* st = StateOf(id);
        return st && st->producer ? st->producer->PredictChunkUs() : 0u;
    }

    // ---- the product's lane thread only ----

    StepOutcome Step(ProductId id, uint32_t budgetUs)
    {
        State* sp = StateOf(id);
        if (!sp || !sp->producer || !sp->node)
            return StepOutcome::NoProducer;
        State& st = *sp;
        if (!st.laned.load(std::memory_order_acquire)) {
            if (st.phase == Phase::Idle)
                return StepOutcome::NotLaned;
            // A job in flight: a claimed or running one is abandoned here; one already complete is
            // published normally (its work is done).
            if (st.phase == Phase::Running) {
                st.producer->Cancel(st.job);
                st.counters.cancelled.fetch_add(1, std::memory_order_relaxed);
                st.phase = Phase::Abandoning;
            } else if (st.phase == Phase::Claimed) {
                st.phase = Phase::Abandoning;
            }
        }
        for (;;) {
            StepOutcome o = StepOutcome::Failed;
            switch (st.phase) {
            case Phase::Idle:
                if (!StartJob(st, &o))
                    return o;
                break;   // Claimed: carry on in this Step
            case Phase::Claimed:
                if (!PrepareJob(st, &o))
                    return o;
                break;   // Running (or Abandoning): carry on in this Step
            case Phase::Running:
                return RunJob(st, budgetUs);
            case Phase::Completing:
                return CompleteJob(st);
            case Phase::Abandoning:
                return AbandonJob(st);
            }
        }
    }

private:
    enum class Phase : uint8_t { Idle, Claimed, Running, Completing, Abandoning };

    struct State {
        const Node* node = nullptr;
        SlotSet* set = nullptr;
        ILaneProducer* producer = nullptr;
        bool readsSource = false;
        std::atomic<bool> laned{ true };
        LaneProductCounters counters;
        // lane-thread private
        Phase phase = Phase::Idle;
        uint64_t nextSeq = 0;
        uint64_t seq = 0;
        LaneTicket ticket;
        LaneInputs pins;
        LaneJobContext job;
        SourceFrameView src;      // the job's own copy of the source view: its keepAlive holds the planes
        bool began = false;       // the producer's fence values are meaningful
        uint64_t costUs = 0;
        ChainOrder chain;         // ordered-chain products: the last published frame
    };

    State* StateOf(ProductId id) { return id < states_.size() ? &states_[id] : nullptr; }
    const State* StateOf(ProductId id) const { return id < states_.size() ? &states_[id] : nullptr; }

    // The job is over: drop the view copy (and its keepAlive) so the host's planes can be recycled.
    static void ReleaseSource(State& st)
    {
        st.src = SourceFrameView();
        st.job.source = nullptr;
    }

    uint64_t Now() const { return nowUs_ ? nowUs_() : 0u; }

    // Idle: pick the frame and claim its slot. False: *out says why the Step ends.
    bool StartJob(State& st, StepOutcome* out)
    {
        uint64_t seq = st.nextSeq;
        const uint64_t skip = skipBefore_.load(std::memory_order_acquire);
        if (skip > seq)
            seq = skip;
        if (indicator_ && indicator_->HasSealed() && indicator_->LastSealed() + 1u > seq)
            seq = indicator_->LastSealed() + 1u;
        if (seq + 1u > arrived_.load(std::memory_order_acquire)) {
            *out = StepOutcome::WaitingSource;
            return false;
        }
        if (st.readsSource && (!find_ || !find_(seq, nullptr))) {   // presence only: no keepAlive copy
            st.nextSeq = seq + 1u;
            st.counters.skippedDropped.fetch_add(1, std::memory_order_relaxed);
            *out = StepOutcome::Late;
            return false;
        }
        // Only Refused gates here; the CarryForward verdict is informational (the slot table decides the reduced form).
        if (st.node->orderedChain && st.chain.Admit(seq) == ChainOrder::Verdict::Refused) {
            // Out of order for the chain (not after the last published frame): refused visibly, the lane moves on.
            st.nextSeq = seq + 1u;
            st.counters.chainRefused.fetch_add(1, std::memory_order_relaxed);
            *out = StepOutcome::Late;
            return false;
        }
        const Provenance prov = st.node->backend == Backend::Ofa ? Provenance::AheadOfa : Provenance::AheadLane;
        const uint64_t completed = releaseCompleted_ ? releaseCompleted_(st.node->id) : ~uint64_t(0);
        LaneTicket t;
        switch (st.set->Claim(seq, prov, completed, &t)) {
        case ClaimResult::Claimed:
            break;
        case ClaimResult::Stale:
            st.nextSeq = seq + 1u;
            st.counters.staleSkips.fetch_add(1, std::memory_order_relaxed);
            *out = StepOutcome::Late;
            return false;
        case ClaimResult::TooEarly:
        case ClaimResult::ReleasePending:
            *out = StepOutcome::WaitingSlot;
            return false;
        default:
            *out = StepOutcome::Failed;
            return false;
        }
        st.seq = seq;
        st.ticket = t;
        st.began = false;
        st.costUs = 0;
        st.pins.count = 0;
        st.phase = Phase::Claimed;
        return true;
    }

    // Claimed: Writable, pins, stamp, context, Begin. True: the phase moved on (Running or
    // Abandoning). False: the Step ends with *out.
    bool PrepareJob(State& st, StepOutcome* out)
    {
        if (!st.set->LaneContinue(st.ticket)) {
            st.phase = Phase::Abandoning;
            return true;
        }
        if (!st.set->Writable(st.ticket)) {
            *out = StepOutcome::WaitingSlot;
            return false;
        }
        const SourceFrameView* src = nullptr;
        if (st.readsSource) {
            SourceFrameView v;
            if (!find_ || !find_(st.seq, &v)) {
                st.counters.skippedDropped.fetch_add(1, std::memory_order_relaxed);
                st.phase = Phase::Abandoning;
                return true;
            }
            st.src = std::move(v);   // kept (with its planes) until the job returns to Idle
            src = &st.src;
        }
        Header& hdr = st.set->LaneHeader(st.ticket);
        if (!table_.LaneAncestors(st.node->id, st.seq, hdr, st.pins)) {
            *out = StepOutcome::WaitingInputs;
            return false;
        }
        // The stamp is read AFTER the pins: a late event then either precedes the Begin (honoured) or
        // is seen by the seal (mismatch -> live).
        hdr.honoured = indicator_ ? indicator_->StampOf(st.seq) : VpIndicator::Stamp();
        hdr.costUs = 0;

        LaneJobContext& job = st.job;
        job = LaneJobContext();
        job.node = st.node;
        job.seq = st.seq;
        job.ticket = st.ticket;
        job.ringSlot = int(st.ticket.slot);
        job.inputs = &st.pins;
        job.source = src;
        job.out = ring_ ? ring_(st.node->id, job.ringSlot) : nullptr;
        job.outBytes = bytes_ ? bytes_(st.node->id) : size_t(0);
        job.header = &hdr;
        job.chainReduced = hdr.chainReduced;
        if (input_)
            for (size_t k = 0; k < st.pins.count && k < job.inputData.size(); ++k)
                if (st.pins.sets[k] != nullptr)   // null: an absent chain-self input (carry-forward)
                    job.inputData[k] = input_(st.pins.sets[k]->Product(), st.pins.seqs[k], st.pins.liveIndex[k]);

        if (!st.producer->Begin(job)) {
            st.counters.beginFailures.fetch_add(1, std::memory_order_relaxed);
            st.phase = Phase::Abandoning;   // nothing submitted: the default fence values apply
            return true;
        }
        st.began = true;
        if (hdr.chainReduced)   // counted once the job really started (not on a failed Begin)
            st.counters.chainCarryForward.fetch_add(1, std::memory_order_relaxed);
        st.phase = Phase::Running;
        return true;
    }

    StepOutcome RunJob(State& st, uint32_t budgetUs)
    {
        if (!st.set->LaneContinue(st.ticket)) {
            st.producer->Cancel(st.job);
            st.counters.cancelled.fetch_add(1, std::memory_order_relaxed);
            st.phase = Phase::Abandoning;
            return AbandonJob(st);
        }
        const uint64_t t0 = Now();
        const ChunkResult r = st.producer->RunChunk(st.job, budgetUs);
        const uint64_t t1 = Now();
        const uint64_t dt = t1 > t0 ? t1 - t0 : uint64_t(0);
        st.costUs += dt;
        st.counters.laneUs.fetch_add(dt, std::memory_order_relaxed);
        if (r == ChunkResult::More)
            return StepOutcome::Ran;
        if (r == ChunkResult::Done) {
            st.phase = Phase::Completing;
            return StepOutcome::Ran;
        }
        st.counters.chunkFailures.fetch_add(1, std::memory_order_relaxed);
        st.phase = Phase::Abandoning;
        return AbandonJob(st);
    }

    StepOutcome CompleteJob(State& st)
    {
        // The header is ours only while the slot is: after the seal it belongs to the slot's tenant.
        if (st.set->LaneContinue(st.ticket))
            st.set->LaneHeader(st.ticket).costUs = st.costUs > 0xFFFFFFFFull ? 0xFFFFFFFFu : uint32_t(st.costUs);
        const PublishResult r = st.set->Publish(st.ticket, st.producer->ProducerSignal(),
                                                st.producer->ProducerCompleted());
        if (r == PublishResult::NotComplete)
            return StepOutcome::WaitingDevice;
        st.pins.Unpin();
        ReleaseSource(st);
        st.nextSeq = st.seq + 1u;
        st.phase = Phase::Idle;
        if (r == PublishResult::Published) {
            if (st.node->orderedChain)
                st.chain.Published(st.seq);
            st.counters.published.fetch_add(1, std::memory_order_relaxed);
            return StepOutcome::Published;
        }
        st.counters.latePublishes.fetch_add(1, std::memory_order_relaxed);
        return StepOutcome::Late;
    }

    StepOutcome AbandonJob(State& st)
    {
        const uint64_t signal = st.began ? st.producer->ProducerSignal() : uint64_t(0);
        const uint64_t completed = st.began ? st.producer->ProducerCompleted() : ~uint64_t(0);
        if (!st.set->Abandon(st.ticket, signal, completed))
            return StepOutcome::WaitingDevice;
        st.pins.Unpin();
        ReleaseSource(st);
        st.nextSeq = st.seq + 1u;
        st.phase = Phase::Idle;
        st.counters.abandoned.fetch_add(1, std::memory_order_relaxed);
        return StepOutcome::Abandoned;
    }

    SlotTable& table_;
    const VpIndicator::Indicator* indicator_;
    std::function<uint64_t()> nowUs_;
    std::vector<State> states_;
    std::function<uint8_t*(ProductId, int)> ring_;
    std::function<const uint8_t*(ProductId, uint64_t, int)> input_;
    std::function<size_t(ProductId)> bytes_;
    std::function<bool(uint64_t, SourceFrameView*)> find_;
    std::function<uint64_t(ProductId)> releaseCompleted_;
    std::atomic<uint64_t> arrived_{ 0 };
    std::atomic<uint64_t> skipBefore_{ 0 };
    uint64_t ignoredProducers_ = 0;
    LaneProductCounters none_;
};

} // namespace VpSkeleton
