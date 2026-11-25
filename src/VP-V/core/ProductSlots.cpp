// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
// Out-of-line non-template bodies of VpSkeleton::SlotSet (see ProductSlots.h).
#include <VP-V/core/ProductSlots.h>

namespace VpSkeleton {

ClaimResult SlotSet::Claim(uint64_t seq, Provenance prov, uint64_t completedRelease, LaneTicket* out)
{
    if (seq > kSlotSeqMask - ring_)
        return ClaimResult::OutOfRange;
    const uint16_t idx = SlotOf(seq);
    Slot& s = slots_[idx];
    const uint64_t mine = Biased(seq);
    const uint64_t prev = mine - ring_;
    uint64_t raw = s.word.load(std::memory_order_acquire);
    const SlotWord w = Unpack(raw);
    if (w.seq >= mine)
        return ClaimResult::Stale;
    if (w.seq < prev)
        return ClaimResult::TooEarly;
    if (w.state == SlotState::Released) {
        if (s.releaseValue.load(std::memory_order_acquire) > completedRelease)
            return ClaimResult::ReleasePending;
    } else if (w.state != SlotState::Empty) {
        return ClaimResult::TooEarly;
    }
    SlotWord next;
    next.seq = mine;
    next.gen = uint16_t(w.gen + 1u);
    next.state = SlotState::Claimed;
    next.prov = prov;
    // seq_cst: pairs with PinRef's fetch_add + load (Dekker) so a pinned reader is seen.
    if (!s.word.compare_exchange_strong(raw, Pack(next), std::memory_order_seq_cst, std::memory_order_acquire)) {
        const SlotWord now = Unpack(raw);
        return now.seq >= mine ? ClaimResult::Stale : ClaimResult::TooEarly;
    }
    counters_.claims.fetch_add(1, std::memory_order_relaxed);
    if (out) {
        out->seq = seq;
        out->slot = idx;
        out->gen = next.gen;
        out->prov = prov;
        out->valid = true;
    }
    return ClaimResult::Claimed;
}

bool SlotSet::StartWriting(const LaneTicket& t)
{
    if (!t.valid)
        return false;
    Slot& s = slots_[t.slot];
    SlotWord expect;
    expect.seq = Biased(t.seq);
    expect.gen = t.gen;
    expect.state = SlotState::Claimed;
    expect.prov = t.prov;
    SlotWord next = expect;
    next.state = SlotState::Writing;
    uint64_t raw = Pack(expect);
    return s.word.compare_exchange_strong(raw, Pack(next), std::memory_order_acq_rel, std::memory_order_acquire);
}

PublishResult SlotSet::Publish(LaneTicket& t, uint64_t producerSignal, uint64_t producerCompleted)
{
    if (!t.valid)
        return PublishResult::BadTicket;
    if (producerCompleted < producerSignal)
        return PublishResult::NotComplete;
    Slot& s = slots_[t.slot];
    uint64_t raw = s.word.load(std::memory_order_acquire);
    const SlotWord w = Unpack(raw);
    if (w.seq != Biased(t.seq) || w.gen != t.gen
        || (w.state != SlotState::Writing && w.state != SlotState::Claimed)) {
        // Rejected (or a stale/copied ticket): never touch the header of the slot's current
        // tenant. The render only reads laneHdr in Published/Sealed, which this ticket cannot be.
        counters_.lateDiscards.fetch_add(1, std::memory_order_relaxed);
        Ack(t);
        return PublishResult::Late;
    }
    const uint32_t cgen = NewCgen(s);
    s.laneHdr.seq = t.seq;
    s.laneHdr.prov = t.prov;
    s.laneHdr.cgen = cgen;
    s.laneCgen.store(cgen, std::memory_order_relaxed);
    s.producerValue.store(producerSignal, std::memory_order_relaxed);
    SlotWord expect = w;
    SlotWord next = expect;
    next.state = SlotState::Published;
    raw = Pack(expect);
    if (s.word.compare_exchange_strong(raw, Pack(next), std::memory_order_acq_rel, std::memory_order_acquire)) {
        counters_.published.fetch_add(1, std::memory_order_relaxed);
        t.valid = false;
        return PublishResult::Published;
    }
    counters_.lateDiscards.fetch_add(1, std::memory_order_relaxed);
    Ack(t);
    return PublishResult::Late;
}

bool SlotSet::Abandon(LaneTicket& t, uint64_t producerSignal, uint64_t producerCompleted)
{
    if (!t.valid)
        return true;
    if (producerCompleted < producerSignal)
        return false;
    counters_.abandoned.fetch_add(1, std::memory_order_relaxed);
    Ack(t);
    return true;
}

bool SlotSet::PinRef(uint64_t seq, AncestorRef* out, int* liveIndex, bool renderValidOnly)
{
    if (seq > kSlotSeqMask - ring_)
        return false;
    const uint16_t idx = SlotOf(seq);
    Slot& s = slots_[idx];
    for (int attempt = 0; attempt < 4; ++attempt) {
        // seq_cst pin + load: pairs with the seq_cst claim CAS (Writable) and the seq_cst release
        // CAS + pin load in BeginLive (Dekker): either the writer sees the pin or we see its word.
        s.pins.fetch_add(1, std::memory_order_seq_cst);
        const uint64_t raw = s.word.load(std::memory_order_seq_cst);
        const SlotWord w = Unpack(raw);
        if (w.seq != Biased(seq) || !HasContent(w.state) || (renderValidOnly && !ValidForRender(w.state))) {
            s.pins.fetch_sub(1, std::memory_order_release);
            return false;
        }
        const bool lane = UsesLaneHeader(w.state);
        const uint32_t cgen = lane ? s.laneCgen.load(std::memory_order_relaxed)
                                   : s.liveCgen.load(std::memory_order_relaxed);
        const int live = lane ? -1 : int(s.liveIndexPub.load(std::memory_order_relaxed));
        if (live >= 0)
            livePins_[size_t(live)].fetch_add(1, std::memory_order_seq_cst);
        if (s.word.load(std::memory_order_seq_cst) != raw) {
            // Changed under us (Published -> Sealed keeps the content; try again).
            if (live >= 0)
                livePins_[size_t(live)].fetch_sub(1, std::memory_order_release);
            s.pins.fetch_sub(1, std::memory_order_release);
            continue;
        }
        if (out) {
            out->productId = product_;
            out->slotIndex = idx;
            out->seq = seq;
            out->cgen = cgen;
            out->prov = w.prov;
        }
        if (liveIndex)
            *liveIndex = live;
        return true;
    }
    counters_.pinGiveUps.fetch_add(1, std::memory_order_relaxed);
    return false;
}

int SlotSet::BeginLive(uint64_t seq)
{
    Slot& s = slots_[SlotOf(seq)];
    const SlotWord w = Unpack(s.word.load(std::memory_order_acquire));
    if (w.seq != Biased(seq)
        || !(w.state == SlotState::Rejected || w.state == SlotState::RejectedLaneOwned
             || w.state == SlotState::Sealed)) {
        counters_.orderErrors.fetch_add(1, std::memory_order_relaxed);
        return -1;
    }
    if (s.liveIndex >= 0 && liveOwner_[size_t(s.liveIndex)] == Biased(seq))
        return s.liveIndex;
    bool pinnedSkip = false;
    for (size_t i = 0; i < liveOwner_.size(); ++i) {
        if (liveOwner_[i] != 0)
            continue;
        // A lane reader may still read this buffer's previous frame (PinRef): never reuse it
        // under the pin (seq_cst: pairs with PinRef, see Release).
        if (livePins_[i].load(std::memory_order_seq_cst) != 0) {
            pinnedSkip = true;
            continue;
        }
        liveOwner_[i] = Biased(seq);
        s.liveIndex = int16_t(i);
        return int(i);
    }
    if (pinnedSkip)
        counters_.pinnedLiveSkips.fetch_add(1, std::memory_order_relaxed);
    counters_.noLiveBuffer.fetch_add(1, std::memory_order_relaxed);
    return -1;
}

bool SlotSet::CommitLive(uint64_t seq, const Header& hdr)
{
    Slot& s = slots_[SlotOf(seq)];
    if (s.liveIndex < 0 || liveOwner_[size_t(s.liveIndex)] != Biased(seq)) {
        counters_.orderErrors.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    for (;;) {
        uint64_t raw = s.word.load(std::memory_order_acquire);
        const SlotWord w = Unpack(raw);
        if (w.seq != Biased(seq)) {
            counters_.orderErrors.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        SlotWord next = w;
        if (w.state == SlotState::Rejected || w.state == SlotState::Sealed)
            next.state = SlotState::Live;
        else if (w.state == SlotState::RejectedLaneOwned)
            next.state = SlotState::LiveLaneOwned;
        else {
            counters_.refusedRelive.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        next.prov = hdr.prov == Provenance::LiveFallback ? Provenance::LiveFallback : Provenance::LiveRender;
        // liveHdr is render-only; its cgen is published to lane readers through liveCgen,
        // stored before the releasing CAS below.
        s.liveHdr = hdr;
        s.liveHdr.seq = seq;
        s.liveHdr.prov = next.prov;
        const uint32_t cgen = NewCgen(s);
        s.liveHdr.cgen = cgen;
        s.liveCgen.store(cgen, std::memory_order_relaxed);
        s.liveIndexPub.store(s.liveIndex, std::memory_order_relaxed);
        if (!s.word.compare_exchange_strong(raw, Pack(next), std::memory_order_acq_rel,
                                            std::memory_order_acquire))
            continue;   // the lane acknowledged meanwhile
        counters_.live.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
}

bool SlotSet::Release(uint64_t seq, uint64_t releaseValue)
{
    Slot& s = slots_[SlotOf(seq)];
    for (;;) {
        uint64_t raw = s.word.load(std::memory_order_acquire);
        const SlotWord w = Unpack(raw);
        if (w.seq != Biased(seq)) {
            counters_.orderErrors.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        SlotWord next = w;
        switch (w.state) {
        case SlotState::Sealed:
        case SlotState::Live:
        case SlotState::Rejected:
            // Render fence values only grow: keeping the larger one also covers an older tenant's
            // pending reads (and a sealed result refilled live). Published by the CAS.
            if (releaseValue > s.releaseValue.load(std::memory_order_relaxed))
                s.releaseValue.store(releaseValue, std::memory_order_relaxed);
            next.state = SlotState::Released;
            break;
        case SlotState::LiveLaneOwned:
        case SlotState::RejectedLaneOwned:
            next.state = SlotState::ReleasedLaneOwned;
            break;
        default:
            counters_.orderErrors.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        next.prov = Provenance::None;
        // seq_cst: a lane PinRef either sees this release (and fails) or BeginLive sees its pin.
        if (!s.word.compare_exchange_strong(raw, Pack(next), std::memory_order_seq_cst,
                                            std::memory_order_acquire))
            continue;
        if (s.liveIndex >= 0 && liveOwner_[size_t(s.liveIndex)] == Biased(seq)) {
            liveOwner_[size_t(s.liveIndex)] = 0;
            s.liveIndex = -1;
        }
        return true;
    }
}

uint32_t SlotSet::ReleaseThrough(uint64_t upToSeq, uint64_t releaseValue)
{
    if (upToSeq > kSlotSeqMask - ring_) {
        counters_.releaseThroughExactOrderErrors.fetch_add(1, std::memory_order_relaxed);
        return 0u;
    }
    if (hasReleasedThrough_ && upToSeq <= releasedThrough_) {
        counters_.releaseThroughExactOrderErrors.fetch_add(1, std::memory_order_relaxed);
        return 0u;
    }
    Slot& exact = slots_[SlotOf(upToSeq)];
    const uint64_t exactBiased = Biased(upToSeq);
    const SlotWord ew = Unpack(exact.word.load(std::memory_order_acquire));
    const bool exactRenderHeld =
        ew.seq == exactBiased
        && (ew.state == SlotState::Sealed || ew.state == SlotState::Live || ew.state == SlotState::Rejected
            || ew.state == SlotState::LiveLaneOwned || ew.state == SlotState::RejectedLaneOwned);
    const bool exactDiscarded = (exact.discardedSeq == exactBiased);
    if (!exactRenderHeld && !exactDiscarded)
        counters_.releaseThroughExactOrderErrors.fetch_add(1, std::memory_order_relaxed);
    const uint64_t limit = Biased(upToSeq);
    uint32_t released = 0;
    const bool fullScan = !hasReleasedThrough_ || (upToSeq - releasedThrough_) >= uint64_t(ring_);
    if (fullScan) {
        for (size_t i = 0; i < ring_; ++i)
            if (ReleaseSlotIfThrough(slots_[i], limit, releaseValue))
                ++released;
    } else {
        const uint64_t lo = releasedThrough_ + 1u;
        for (uint64_t s = lo; s <= upToSeq; ++s)
            if (ReleaseSlotIfThrough(slots_[SlotOf(s)], limit, releaseValue))
                ++released;
    }
    releasedThrough_ = upToSeq;
    hasReleasedThrough_ = true;
    return released;
}

bool SlotSet::Discard(uint64_t seq)
{
    Slot& s = slots_[SlotOf(seq)];
    const uint64_t mine = Biased(seq);
    for (;;) {
        uint64_t raw = s.word.load(std::memory_order_acquire);
        const SlotWord w = Unpack(raw);
        SlotWord next = w;
        next.seq = mine;
        next.prov = Provenance::None;
        if (w.seq > mine) {
            counters_.orderErrors.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        switch (w.state) {
        case SlotState::Claimed:
        case SlotState::Writing:
        case SlotState::ReleasedLaneOwned:
            next.state = SlotState::ReleasedLaneOwned;
            break;
        case SlotState::Published:
        case SlotState::Abandoned:
            next.state = SlotState::Released;
            break;
        case SlotState::Empty:
        case SlotState::Released:
            if (w.seq == mine) {
                counters_.orderErrors.fetch_add(1, std::memory_order_relaxed);
                return false;
            }
            next.state = SlotState::Released;
            break;
        default:
            if (w.seq == mine)
                counters_.orderErrors.fetch_add(1, std::memory_order_relaxed);   // already sealed
            else
                counters_.overruns.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        if (s.word.compare_exchange_strong(raw, Pack(next), std::memory_order_acq_rel, std::memory_order_acquire)) {
            s.discardedSeq = mine;
            return true;
        }
    }
}

bool SlotSet::ReleaseSlotIfThrough(Slot& s, uint64_t limit, uint64_t releaseValue)
{
    for (;;) {
        uint64_t raw = s.word.load(std::memory_order_acquire);
        const SlotWord w = Unpack(raw);
        if (w.seq < ring_ || w.seq > limit)
            return false;
        SlotWord next = w;
        bool renderHeld = true;
        switch (w.state) {
        case SlotState::Sealed:
        case SlotState::Live:
        case SlotState::Rejected:
            if (releaseValue > s.releaseValue.load(std::memory_order_relaxed))
                s.releaseValue.store(releaseValue, std::memory_order_relaxed);
            next.state = SlotState::Released;
            break;
        case SlotState::LiveLaneOwned:
        case SlotState::RejectedLaneOwned:
            next.state = SlotState::ReleasedLaneOwned;
            break;
        default:
            renderHeld = false;
            break;
        }
        if (!renderHeld)
            return false;
        next.prov = Provenance::None;
        // seq_cst: a lane PinRef either sees this release (and fails) or BeginLive sees its pin.
        if (!s.word.compare_exchange_strong(raw, Pack(next), std::memory_order_seq_cst,
                                            std::memory_order_acquire))
            continue;
        if (s.liveIndex >= 0 && liveOwner_[size_t(s.liveIndex)] == w.seq) {
            liveOwner_[size_t(s.liveIndex)] = 0;
            s.liveIndex = -1;
        }
        return true;
    }
}

void SlotSet::Ack(LaneTicket& t)
{
    Slot& s = slots_[t.slot];
    for (;;) {
        uint64_t raw = s.word.load(std::memory_order_acquire);
        const SlotWord w = Unpack(raw);
        if (w.gen != t.gen)
            break;
        SlotWord next = w;
        if ((w.state == SlotState::Claimed || w.state == SlotState::Writing) && w.seq == Biased(t.seq))
            next.state = SlotState::Abandoned;
        else if (w.state == SlotState::RejectedLaneOwned)
            next.state = SlotState::Rejected;
        else if (w.state == SlotState::LiveLaneOwned)
            next.state = SlotState::Live;
        else if (w.state == SlotState::ReleasedLaneOwned)
            next.state = SlotState::Empty;
        else
            break;
        if (s.word.compare_exchange_strong(raw, Pack(next), std::memory_order_acq_rel, std::memory_order_acquire))
            break;
    }
    t.valid = false;
}

} // namespace VpSkeleton
