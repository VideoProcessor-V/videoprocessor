// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "Ancestry.h"
#include "Indicator.h"
#include "ProductGraph.h"
#include "Products.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

// Skeleton S1: product slots and the ownership protocol (design doc 25 B3/B4, audit 25a §3 and
// N2-N4, owner decisions doc 21 item 6). Header-only, pure C++14, no Windows, no GPU calls: the
// fence values are plain numbers the caller signals and polls (render Wait / lane poll).
//
// One SlotSet per product, sized at graph build (ring + live pool, nothing allocated per frame).
// The slot of frame `seq` is seq mod ring. Each slot has one 64-bit state word
//     { seq:40 (biased by +ring), claimGen:16, state:4, provenance:4 }
// and two headers: laneHdr (written only by the lane while it owns the slot) and liveHdr (written
// only by the render). The state says which header (and which buffer: the ring buffer or a live-
// pool buffer) is current.
//
//   Claim    lane    CAS Empty|Released(seq-ring, g) -> Writing(seq, g+1). A stale or late claim
//                    cannot match the slot's monotonic seq: ABA-proof (audit 25a §3.3).
//   Write    lane    its ring buffer + laneHdr only; poll Writable() first (lane input pins).
//   Publish  lane    after write + fence (stamp LAST): CAS Writing(seq) -> Published(seq). A
//                    failure is LATE: late results never overwrite; the buffer is acknowledged.
//   Seal N   render  per product in topological order (SlotTable::Resolve): Writing (any seq <= N)
//                    -> Rejected lane-owned (N3); Published(N) validated (stamps, span, ancestry
//                    cgens) -> Sealed or Rejected; anything older -> Rejected(N). Seq > N and an
//                    older frame still held (overrun) are counted errors.
//   Live     render  a live-pool buffer + liveHdr, cgen++ (every backend, CPU included: the
//                    render never writes the lane's ring buffer, audit 25a §3.1). Descendants
//                    recorded with the old cgen fail at their seal: the cascade.
//   Release  render  after N's last use: Released(N) with the render's release-fence value (the
//                    lane polls it before reuse; CPU products pass 0: the state is the flag, N4).
//   Ack      lane    a lane-owned Rejected/Live/Released slot returns at Publish failure or Abandon.
//
// Threading: one lane producer thread per product (the lane side), one render thread (the render
// side). Lane jobs reading another product pin that slot (PinRef): its own lane cannot write the
// reclaimed slot (Writable) and the render does not reuse its live buffer under the reader.
namespace VpSkeleton {

static_assert(kMaxInputs <= VpAncestry::kMaxAncestors, "a header must record every declared input");
static_assert(kMaxInputs <= 32, "ProductFrameResult::missingInputs is a 32-bit mask");

using VpAncestry::AncestorRef;
using VpAncestry::Header;
using VpAncestry::Provenance;

enum class SlotState : uint8_t {
    Empty = 0,          // free; seq = the last frame that used it
    Writing,            // the lane owns the ring buffer and laneHdr
    Published,          // lane result complete (stamp last), not sealed yet
    Sealed,             // the render accepted the lane result (ring buffer, laneHdr)
    Abandoned,          // the lane stopped without a result; ring buffer free
    Rejected,           // the render owns the frame; no result yet; ring buffer free
    RejectedLaneOwned,  // as Rejected, the lane still holds the ring buffer
    Live,               // the render filled a live buffer (liveHdr); ring buffer free
    LiveLaneOwned,      // as Live, the lane still holds the ring buffer
    Released,           // render done; the lane reuses it after the release fence passes
    ReleasedLaneOwned,  // render done; the lane still holds the ring buffer (its ack frees it)
};

inline const char* Token(SlotState s)
{
    switch (s) {
    case SlotState::Empty: return "empty";
    case SlotState::Writing: return "writing";
    case SlotState::Published: return "published";
    case SlotState::Sealed: return "sealed";
    case SlotState::Abandoned: return "abandoned";
    case SlotState::Rejected: return "rejected";
    case SlotState::RejectedLaneOwned: return "rejected_lane_owned";
    case SlotState::Live: return "live";
    case SlotState::LiveLaneOwned: return "live_lane_owned";
    case SlotState::Released: return "released";
    case SlotState::ReleasedLaneOwned: return "released_lane_owned";
    }
    return "?";
}

constexpr uint64_t kSlotSeqBits = 40;
constexpr uint64_t kSlotSeqMask = (uint64_t(1) << kSlotSeqBits) - 1u;

struct SlotWord {
    uint64_t seq = 0;   // biased: frame seq + ring
    uint16_t gen = 0;
    SlotState state = SlotState::Empty;
    Provenance prov = Provenance::None;
};

inline uint64_t Pack(const SlotWord& w)
{
    return (w.seq & kSlotSeqMask) | (uint64_t(w.gen) << 40) | (uint64_t(uint8_t(w.state) & 0xFu) << 56)
           | (uint64_t(uint8_t(w.prov) & 0xFu) << 60);
}

inline SlotWord Unpack(uint64_t v)
{
    SlotWord w;
    w.seq = v & kSlotSeqMask;
    w.gen = uint16_t((v >> 40) & 0xFFFFu);
    w.state = static_cast<SlotState>((v >> 56) & 0xFu);
    w.prov = static_cast<Provenance>((v >> 60) & 0xFu);
    return w;
}

inline bool HasContent(SlotState s)
{
    return s == SlotState::Published || s == SlotState::Sealed || s == SlotState::Live
           || s == SlotState::LiveLaneOwned;
}

inline bool UsesLaneHeader(SlotState s) { return s == SlotState::Published || s == SlotState::Sealed; }
inline bool UsesLiveHeader(SlotState s) { return s == SlotState::Live || s == SlotState::LiveLaneOwned; }
inline bool ValidForRender(SlotState s)
{
    return s == SlotState::Sealed || s == SlotState::Live || s == SlotState::LiveLaneOwned;
}

enum class ClaimResult : uint8_t {
    Claimed,
    TooEarly,         // the slot still holds an earlier frame (in use, or the lead exceeds the ring)
    ReleasePending,   // released, but the render's release fence has not passed yet
    Stale,            // the slot is already at or past this frame: the lane is late, skip it
    OutOfRange,       // seq beyond the 40-bit range
};

enum class PublishResult : uint8_t {
    Published,
    Late,          // the seal rejected the frame first: the result is discarded, buffer returned
    NotComplete,   // the producer fence has not passed: nothing published (stamp LAST)
    BadTicket,
};

enum class SealOutcome : uint8_t {
    Sealed,       // the ahead result is accepted
    NeedsLive,    // rejected or missing: the render fills it live
    Unavailable,  // overrun / order error / no live buffer / the fill declined
};

enum class SealReason : uint8_t {
    Accepted,
    LaneLate,            // the lane was still writing at the seal (flagged LATE to the regulator)
    LaneMissing,         // nothing published for this frame
    InRenderOnly,        // the tier plan gave the product no lane (not a miss)
    StampMismatch,
    AncientStamp,
    SpanBroken,          // a temporal input spans a change of the product's span counters
    AncestryIncomplete,  // the recorded ancestors do not match the declared inputs
    AncestorGone,        // an ancestor slot was released or reused first (N2: must stay 0)
    AncestorReplaced,    // an ancestor was refilled after this product recorded it: the cascade
    AncestorInvalid,     // an ancestor has no valid content
    Overrun,             // an older frame still holds the slot (ring undersized)
    OrderError,          // seq beyond the slot, or the frame already released/discarded
    AlreadySealed,       // a second seal of the same frame (idempotent)
    NoLiveBuffer,
    FillDeclined,
    InputMissing,        // a live fill lacks an input and the product declares no form for that
};

inline const char* Token(SealReason r)
{
    switch (r) {
    case SealReason::Accepted: return "accepted";
    case SealReason::LaneLate: return "lane_late";
    case SealReason::LaneMissing: return "lane_missing";
    case SealReason::InRenderOnly: return "in_render_only";
    case SealReason::StampMismatch: return "stamp_mismatch";
    case SealReason::AncientStamp: return "ancient_stamp";
    case SealReason::SpanBroken: return "span_broken";
    case SealReason::AncestryIncomplete: return "ancestry_incomplete";
    case SealReason::AncestorGone: return "ancestor_gone";
    case SealReason::AncestorReplaced: return "ancestor_replaced";
    case SealReason::AncestorInvalid: return "ancestor_invalid";
    case SealReason::Overrun: return "overrun";
    case SealReason::OrderError: return "order_error";
    case SealReason::AlreadySealed: return "already_sealed";
    case SealReason::NoLiveBuffer: return "no_live_buffer";
    case SealReason::FillDeclined: return "fill_declined";
    case SealReason::InputMissing: return "input_missing";
    }
    return "?";
}

struct SealResult {
    SealOutcome outcome = SealOutcome::Unavailable;
    SealReason reason = SealReason::OrderError;
};

struct LaneTicket {
    uint64_t seq = 0;
    uint16_t slot = 0;
    uint16_t gen = 0;
    Provenance prov = Provenance::None;
    bool valid = false;
};

// Where the render reads frame `seq` of a product.
struct ReadInfo {
    enum Kind : uint8_t { None, Ring, Live } kind = None;
    int index = -1;               // ring slot or live-pool buffer
    uint64_t waitValue = 0;       // producer fence value to Wait on before the first read (Ring)
    const Header* header = nullptr;
};

struct SlotCounters {
    std::atomic<uint64_t> claims{ 0 };
    std::atomic<uint64_t> published{ 0 };
    std::atomic<uint64_t> lateDiscards{ 0 };     // publish after the seal: discarded, never written
    std::atomic<uint64_t> abandoned{ 0 };
    std::atomic<uint64_t> sealed{ 0 };
    std::atomic<uint64_t> live{ 0 };
    std::atomic<uint64_t> late{ 0 };             // lane still writing at the seal
    std::atomic<uint64_t> missing{ 0 };
    std::atomic<uint64_t> invalid{ 0 };          // published but rejected by validation
    std::atomic<uint64_t> ancestorGone{ 0 };
    std::atomic<uint64_t> overruns{ 0 };
    std::atomic<uint64_t> orderErrors{ 0 };
    std::atomic<uint64_t> doubleSeals{ 0 };
    std::atomic<uint64_t> noLiveBuffer{ 0 };
    std::atomic<uint64_t> refusedRelive{ 0 };
    std::atomic<uint64_t> pinnedLiveSkips{ 0 };   // a free live buffer skipped under a lane pin
    std::atomic<uint64_t> pinGiveUps{ 0 };        // PinRef gave up on a word changing under it
    std::atomic<uint64_t> liveMissingInputs{ 0 }; // live fills made in the declared missing-input form
    std::atomic<uint64_t> inputMissing{ 0 };      // live fills refused: an input missing, no form
    std::atomic<uint64_t> spanUnknown{ 0 };       // the span check had no sealed stamp (not N2)
    std::atomic<uint64_t> chainCarryForward{ 0 }; // ordered chain: sealed in the declared reduced form (self ancestor absent)
    std::atomic<uint64_t> leadAccepted{ 0 };      // lead ancestors accepted at a SEALED reader (Published, stamp == StampOf(N+k))
    std::atomic<uint64_t> leadLaterRejected{ 0 }; // a frame accepted as a lead ancestor was rejected at its own seal later (residual K1 risk)
    std::atomic<uint64_t> chainWaitRejected{ 0 }; // ordered chain: frames whose job waited on a REJECTED predecessor (counted once per frame)
    std::atomic<uint64_t> chainWaitUnsealed{ 0 }; // ordered chain: frames whose job waited because the predecessor was only Published (its seal not done yet; counted once per frame)
    std::atomic<uint64_t> storeWaitRejected{ 0 }; // single store: frames whose lag-1 (or lag-0) READER job waited on a REJECTED store frame (no live fill exists for a store, so it waits until the frame is released; counted once per reader frame, on the store's set)
    std::atomic<uint64_t> writeWaitPinned{ 0 };   // lane jobs that found their claimed slot still pinned by a late reader of the previous tenant (counted once per job, by the LaneRunner)
};

class SlotSet {
public:
    SlotSet(ProductId product, uint16_t ring, uint16_t livePool)
        : product_(product)
        , ring_(ring == 0 ? 1 : ring)
        , livePool_(livePool)   // 0 = lane-only: no live fill (a single-store product, ProductDecl::singletonStore)
        , slots_(ring_)
        , liveOwner_(livePool_, 0)
        , livePins_(livePool_)
    {
        for (std::atomic<uint32_t>& pin : livePins_)
            pin.store(0, std::memory_order_relaxed);
        for (uint16_t i = 0; i < ring_; ++i) {
            SlotWord w;
            w.seq = i;   // = biased(i - ring): frame i is the slot's first claimable frame
            slots_[i].word.store(Pack(w), std::memory_order_relaxed);
        }
    }

    SlotSet(const SlotSet&) = delete;
    SlotSet& operator=(const SlotSet&) = delete;

    ProductId Product() const { return product_; }
    uint16_t Ring() const { return ring_; }
    uint16_t LivePool() const { return livePool_; }
    uint16_t SlotOf(uint64_t seq) const { return uint16_t(seq % ring_); }
    const SlotCounters& Counters() const { return counters_; }
    SlotCounters& MutableCounters() { return counters_; }
    // The tier plan's grant: without a lane, a missing result is expected, not counted.
    void SetLaneGranted(bool granted) { laneGranted_.store(granted, std::memory_order_relaxed); }
    bool LaneGranted() const { return laneGranted_.load(std::memory_order_relaxed); }

    // Any thread: the decoded word of the slot that frame `seq` maps to (diagnostics, tests).
    SlotWord WordFor(uint64_t seq) const { return Unpack(slots_[SlotOf(seq)].word.load(std::memory_order_acquire)); }
    SlotState StateOf(uint64_t seq) const
    {
        const SlotWord w = WordFor(seq);
        return w.seq == Biased(seq) ? w.state : SlotState::Empty;
    }
    // The frame seq a slot word holds (UINT64_MAX before its first use).
    uint64_t FrameOf(const SlotWord& w) const { return w.seq < ring_ ? ~uint64_t(0) : w.seq - ring_; }

    // ---- lane side (one producer thread per product) ----

    // completedRelease = the render release fence's completed value as polled by the lane
    // (CPU products: UINT64_MAX).
    ClaimResult Claim(uint64_t seq, Provenance prov, uint64_t completedRelease, LaneTicket* out)
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
        next.state = SlotState::Writing;
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

    // Lane: a reader of the previous tenant may still hold a pin; write only once this is true.
    bool Writable(const LaneTicket& t) const
    {
        return t.valid && slots_[t.slot].pins.load(std::memory_order_seq_cst) == 0;
    }

    // Lane: the header to fill between Claim and Publish (lane-private until Publish).
    Header& LaneHeader(const LaneTicket& t) { return slots_[t.slot].laneHdr; }

    // Lane, between chunks: false once the seal took the frame (cancel; then Abandon).
    bool LaneContinue(const LaneTicket& t) const
    {
        if (!t.valid)
            return false;
        const SlotWord w = Unpack(slots_[t.slot].word.load(std::memory_order_acquire));
        return w.seq == Biased(t.seq) && w.gen == t.gen && w.state == SlotState::Writing;
    }

    // Lane: publish after the write AND the producer fence (stamp last). producerSignal = the
    // value the producer device signalled after its last write, producerCompleted = the polled
    // completed value (CPU: 0 and UINT64_MAX). On Late the buffer is acknowledged here.
    PublishResult Publish(LaneTicket& t, uint64_t producerSignal = 0, uint64_t producerCompleted = ~uint64_t(0))
    {
        if (!t.valid)
            return PublishResult::BadTicket;
        if (producerCompleted < producerSignal)
            return PublishResult::NotComplete;
        Slot& s = slots_[t.slot];
        if (!LaneContinue(t)) {
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
        SlotWord expect;
        expect.seq = Biased(t.seq);
        expect.gen = t.gen;
        expect.state = SlotState::Writing;
        expect.prov = t.prov;
        SlotWord next = expect;
        next.state = SlotState::Published;
        uint64_t raw = Pack(expect);
        if (s.word.compare_exchange_strong(raw, Pack(next), std::memory_order_acq_rel, std::memory_order_acquire)) {
            counters_.published.fetch_add(1, std::memory_order_relaxed);
            t.valid = false;
            return PublishResult::Published;
        }
        counters_.lateDiscards.fetch_add(1, std::memory_order_relaxed);
        Ack(t);
        return PublishResult::Late;
    }

    // Lane: stop without publishing (cancelled at a chunk boundary). Submitted GPU/OFA work must
    // land first: with producerCompleted < producerSignal nothing is returned (false; poll again),
    // so the ring buffer never has two writers. CPU: the defaults.
    bool Abandon(LaneTicket& t, uint64_t producerSignal = 0, uint64_t producerCompleted = ~uint64_t(0))
    {
        if (!t.valid)
            return true;
        if (producerCompleted < producerSignal)
            return false;
        counters_.abandoned.fetch_add(1, std::memory_order_relaxed);
        Ack(t);
        return true;
    }

    // Any thread (the lane recording ancestors): the reference to frame `seq`'s current content,
    // pinned until Unpin: its own lane cannot write the reclaimed ring slot (Writable) and the render
    // does not reuse its live buffer (BeginLive). *liveIndex = the live buffer to read, or -1 for the
    // ring slot. False (nothing pinned) when there is no content. renderValidOnly: pin only content the render accepted
    // (Sealed / Live / Live lane-owned); a Published (not yet sealed) frame then counts as no content (an ordered-chain
    // predecessor: it could still be rejected and live-filled, which would replace the ancestor a successor recorded).
    bool PinRef(uint64_t seq, AncestorRef* out, int* liveIndex = nullptr, bool renderValidOnly = false)
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

    // liveIndex = what PinRef returned for this pin.
    void Unpin(uint64_t seq, int liveIndex = -1)
    {
        if (liveIndex >= 0 && size_t(liveIndex) < livePins_.size())
            livePins_[size_t(liveIndex)].fetch_sub(1, std::memory_order_release);
        slots_[SlotOf(seq)].pins.fetch_sub(1, std::memory_order_release);
    }

    // ---- render side (the render thread only) ----

    // Seal frame N. `check(const Header&)` validates a published lane header (SlotTable).
    template <class Check>
    SealResult Seal(uint64_t seq, Check check)
    {
        SealResult res;
        if (seq > kSlotSeqMask - ring_) {
            counters_.orderErrors.fetch_add(1, std::memory_order_relaxed);
            return res;
        }
        Slot& s = slots_[SlotOf(seq)];
        const uint64_t mine = Biased(seq);
        for (;;) {
            uint64_t raw = s.word.load(std::memory_order_acquire);
            const SlotWord w = Unpack(raw);
            SlotWord next = w;
            next.seq = mine;
            SealReason reason = SealReason::LaneMissing;
            if (w.seq > mine) {
                counters_.orderErrors.fetch_add(1, std::memory_order_relaxed);
                return res;   // Unavailable, OrderError
            }
            if (w.seq == mine) {
                switch (w.state) {
                case SlotState::Writing:
                    next.state = SlotState::RejectedLaneOwned;
                    reason = SealReason::LaneLate;
                    break;
                case SlotState::Published: {
                    reason = check(static_cast<const Header&>(s.laneHdr));
                    next.state = reason == SealReason::Accepted ? SlotState::Sealed : SlotState::Rejected;
                    break;
                }
                case SlotState::Abandoned:
                    next.state = SlotState::Rejected;
                    reason = SealReason::LaneMissing;
                    break;
                case SlotState::Sealed:
                case SlotState::Live:
                case SlotState::LiveLaneOwned:
                    counters_.doubleSeals.fetch_add(1, std::memory_order_relaxed);
                    res.outcome = SealOutcome::Sealed;
                    res.reason = SealReason::AlreadySealed;
                    return res;
                case SlotState::Rejected:
                case SlotState::RejectedLaneOwned:
                    counters_.doubleSeals.fetch_add(1, std::memory_order_relaxed);
                    res.outcome = SealOutcome::NeedsLive;
                    res.reason = SealReason::AlreadySealed;
                    return res;
                default:   // Empty / Released / ReleasedLaneOwned at N: already released or discarded
                    counters_.orderErrors.fetch_add(1, std::memory_order_relaxed);
                    return res;
                }
            } else {
                switch (w.state) {
                case SlotState::Writing:   // stale lane work on an older frame (N3: lane-owned)
                    next.state = SlotState::RejectedLaneOwned;
                    reason = SealReason::LaneLate;
                    break;
                case SlotState::ReleasedLaneOwned:
                    next.state = SlotState::RejectedLaneOwned;
                    break;
                case SlotState::Empty:
                case SlotState::Released:
                case SlotState::Published:
                case SlotState::Abandoned:
                    next.state = SlotState::Rejected;
                    break;
                default:   // an older frame still holds the slot
                    counters_.overruns.fetch_add(1, std::memory_order_relaxed);
                    res.reason = SealReason::Overrun;
                    return res;
                }
            }
            if (next.state != SlotState::Sealed)
                next.prov = Provenance::None;
            if (!s.word.compare_exchange_strong(raw, Pack(next), std::memory_order_acq_rel,
                                                std::memory_order_acquire))
                continue;   // the lane published / acknowledged meanwhile: decide again
            if (next.state == SlotState::Sealed) {
                counters_.sealed.fetch_add(1, std::memory_order_relaxed);
                res.outcome = SealOutcome::Sealed;
                res.reason = SealReason::Accepted;
                return res;
            }
            if (reason == SealReason::LaneLate)
                counters_.late.fetch_add(1, std::memory_order_relaxed);
            else if (reason == SealReason::LaneMissing && !LaneGranted())
                reason = SealReason::InRenderOnly;
            else if (reason == SealReason::LaneMissing)
                counters_.missing.fetch_add(1, std::memory_order_relaxed);
            else if (reason == SealReason::AncestorGone) {
                counters_.ancestorGone.fetch_add(1, std::memory_order_relaxed);
                counters_.invalid.fetch_add(1, std::memory_order_relaxed);
            } else
                counters_.invalid.fetch_add(1, std::memory_order_relaxed);
            res.outcome = SealOutcome::NeedsLive;
            res.reason = reason;
            return res;
        }
    }

    // Render: a live-pool buffer for frame `seq` (rejected, or a sealed result being replaced).
    // Returns its index, or -1 when no buffer is free (counted) or the frame is not the render's.
    int BeginLive(uint64_t seq)
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

    // Render: give a live buffer back without committing (the fill declined).
    void CancelLive(uint64_t seq)
    {
        Slot& s = slots_[SlotOf(seq)];
        if (s.liveIndex >= 0 && liveOwner_[size_t(s.liveIndex)] == Biased(seq)) {
            liveOwner_[size_t(s.liveIndex)] = 0;
            s.liveIndex = -1;
        }
    }

    // Render: commit the live fill (after BeginLive and the render's write). `hdr` supplies the
    // reads, honoured stamp, ancestors, cost and provenance (LiveRender or LiveFallback); the seq
    // and a new cgen are set here. A frame already live is never refilled (counted).
    bool CommitLive(uint64_t seq, const Header& hdr)
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

    // Render: after frame `seq`'s last use (including every descendant's seal: N2). The release
    // value is the render fence signalled after the last read of the ring buffer; the lane polls
    // it before reuse. CPU products pass 0.
    bool Release(uint64_t seq, uint64_t releaseValue = 0)
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

    // Render: frame `seq` will never be rendered (dropped): free its slot for the lane.
    bool Discard(uint64_t seq)
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
            if (s.word.compare_exchange_strong(raw, Pack(next), std::memory_order_acq_rel, std::memory_order_acquire))
                return true;
        }
    }

    // Lane: frame `seq` is Rejected (the render owns it, no content yet): the live fill has not made it, or declined,
    // or found no live buffer. An ordered-chain successor then waits (Transient), visibly (chainWaitRejected).
    bool RejectedAwaitingLive(uint64_t seq) const
    {
        const SlotWord w = Unpack(slots_[SlotOf(seq)].word.load(std::memory_order_acquire));
        return w.seq == Biased(seq) && (w.state == SlotState::Rejected || w.state == SlotState::RejectedLaneOwned);
    }

    // Lane: frame `seq` is Published and its seal has not run yet. An ordered-chain successor then waits (Transient),
    // visibly (chainWaitUnsealed): only a render-valid predecessor is pinned.
    bool PublishedAwaitingSeal(uint64_t seq) const
    {
        const SlotWord w = Unpack(slots_[SlotOf(seq)].word.load(std::memory_order_acquire));
        return w.seq == Biased(seq) && w.state == SlotState::Published;
    }

    // Lane: why PinRef(seq) failed. Absent = the frame will never have content from this lane (never claimed, abandoned,
    // released, or the slot moved on); Transient = it may still get content (being written, rejected awaiting the live
    // fill, or the word changed under the pin). An ordered chain carries forward only on Absent.
    enum class Absence : uint8_t { Absent, Transient };
    Absence AbsenceOf(uint64_t seq) const
    {
        const Slot& s = slots_[SlotOf(seq)];
        const SlotWord w = Unpack(s.word.load(std::memory_order_acquire));
        if (w.seq != Biased(seq))
            return Absence::Absent;
        switch (w.state) {
        case SlotState::Empty:
        case SlotState::Abandoned:
        case SlotState::Released:
        case SlotState::ReleasedLaneOwned:
            return Absence::Absent;
        default:
            return Absence::Transient;
        }
    }

    // Render: what the ancestry check sees of slot `slotIndex`.
    VpAncestry::SlotView ViewAt(uint16_t slotIndex) const
    {
        VpAncestry::SlotView v;
        if (slotIndex >= ring_)
            return v;
        const Slot& s = slots_[slotIndex];
        const SlotWord w = Unpack(s.word.load(std::memory_order_acquire));
        if (w.seq < ring_ || w.state == SlotState::Empty || w.state == SlotState::Released
            || w.state == SlotState::ReleasedLaneOwned)
            return v;   // never used, or released: gone
        v.present = true;
        v.seq = w.seq - ring_;
        v.cgen = UsesLaneHeader(w.state) ? s.laneHdr.cgen : UsesLiveHeader(w.state) ? s.liveHdr.cgen : 0u;
        v.valid = ValidForRender(w.state);
        v.lanePublished = w.state == SlotState::Published;
        return v;
    }

    // Render: the stamp the content of frame `seq` was computed under (lane or live header); false without content.
    bool HonouredOf(uint64_t seq, VpIndicator::Stamp* out) const
    {
        const Slot& s = slots_[SlotOf(seq)];
        const SlotWord w = Unpack(s.word.load(std::memory_order_acquire));
        if (w.seq != Biased(seq) || !HasContent(w.state))
            return false;
        if (out)
            *out = UsesLaneHeader(w.state) ? s.laneHdr.honoured : s.liveHdr.honoured;
        return true;
    }

    // Render: where to read frame `seq` (None when there is no valid content).
    // allowPublished: a LEAD input is read before its own seal, from the lane's published (not yet sealed) result.
    ReadInfo Read(uint64_t seq, bool allowPublished = false) const
    {
        ReadInfo r;
        const uint16_t idx = SlotOf(seq);
        const Slot& s = slots_[idx];
        const SlotWord w = Unpack(s.word.load(std::memory_order_acquire));
        if (w.seq != Biased(seq))
            return r;
        if (w.state == SlotState::Sealed || (allowPublished && w.state == SlotState::Published)) {
            r.kind = ReadInfo::Ring;
            r.index = idx;
            r.waitValue = s.producerValue.load(std::memory_order_relaxed);
            r.header = &s.laneHdr;
        } else if (UsesLiveHeader(w.state)) {
            r.kind = ReadInfo::Live;
            r.index = s.liveIndex;
            r.header = &s.liveHdr;
        }
        return r;
    }

private:
    struct Slot {
        std::atomic<uint64_t> word{ 0 };
        std::atomic<uint32_t> cgenCounter{ 0 };
        std::atomic<uint32_t> laneCgen{ 0 };
        std::atomic<uint32_t> liveCgen{ 0 };
        std::atomic<uint32_t> pins{ 0 };
        std::atomic<uint64_t> releaseValue{ 0 };
        std::atomic<uint64_t> producerValue{ 0 };
        Header laneHdr;          // lane-written while it owns the slot
        Header liveHdr;          // render-only
        int16_t liveIndex = -1;  // render-only
        std::atomic<int16_t> liveIndexPub{ -1 };   // liveIndex for lane readers (set before the Live CAS)
    };

    uint64_t Biased(uint64_t seq) const { return seq + ring_; }

    static uint32_t NewCgen(Slot& s)
    {
        uint32_t g = s.cgenCounter.fetch_add(1, std::memory_order_relaxed) + 1u;
        while (g == 0)
            g = s.cgenCounter.fetch_add(1, std::memory_order_relaxed) + 1u;
        return g;
    }

    // Lane: return a lane-owned buffer (matched by the claim generation: one ticket per slot).
    void Ack(LaneTicket& t)
    {
        Slot& s = slots_[t.slot];
        for (;;) {
            uint64_t raw = s.word.load(std::memory_order_acquire);
            const SlotWord w = Unpack(raw);
            if (w.gen != t.gen)
                break;
            SlotWord next = w;
            if (w.state == SlotState::Writing && w.seq == Biased(t.seq))
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

    const ProductId product_;
    const uint16_t ring_;
    const uint16_t livePool_;
    std::vector<Slot> slots_;
    std::vector<uint64_t> liveOwner_;   // render-only: biased seq holding each live buffer, 0 = free
    std::vector<std::atomic<uint32_t>> livePins_;   // lane readers pinning each live buffer (PinRef)
    SlotCounters counters_;
    std::atomic<bool> laneGranted_{ true };
};

// A lane job's pinned inputs (one per declared input), released by Unpin.
struct LaneInputs {
    size_t count = 0;
    std::array<SlotSet*, kMaxInputs> sets{};
    std::array<uint64_t, kMaxInputs> seqs{};
    std::array<int, kMaxInputs> liveIndex{};   // -1: read the ring slot; else this live buffer

    void Unpin()
    {
        for (size_t i = 0; i < count; ++i)
            if (sets[i])
                sets[i]->Unpin(seqs[i], liveIndex[i]);
        count = 0;
    }
};

// Per-frame result of SlotTable::Resolve, one per graph node (topological order).
struct ProductFrameResult {
    ProductId id = kNoProduct;
    SealResult seal;                          // what the seal decided
    SealOutcome final = SealOutcome::Unavailable;  // after the live fill: Sealed (ahead or live) or Unavailable
    bool liveFilled = false;
    SealReason finalReason = SealReason::OrderError;
    uint32_t missingInputs = 0;   // bit k: input k had no valid content for the live fill
};

// All products of one graph: preallocated SlotSets in topological order, the frozen sealed stamps
// of recent frames (span checks), and the per-frame resolve that seals and live-fills in order.
class SlotTable {
public:
    // Look-ahead (K1, W1-SKG): an input with a lead reads frame N+k. The lane pins the ancestor result (a job
    // waits until the ancestor lane has made N+k); the seal accepts it when it is Published (or already Sealed /
    // Live) with the same cgen the reader recorded and its honoured stamp equals StampOf(N+k) under the
    // ancestor own reads. The stamp of a frame that has not been resolved yet comes from the injected source
    // (SetStampSource: the Indicator StampOf); without one a lead ancestor is refused visibly (AncestryIncomplete,
    // counted), never accepted unchecked. The table is always valid.
    explicit SlotTable(const Graph& g) : nodes_(g.nodes)
    {
        sets_.reserve(nodes_.size());
        size_t maxRing = 1;
        for (const Node& n : nodes_) {
            sets_.emplace_back(new SlotSet(n.id, n.ring, n.livePool));
            if (n.ring > maxRing)
                maxRing = n.ring;
        }
        sealed_.resize(maxRing + 1);
        results_.resize(nodes_.size());
        leadMark_.resize(nodes_.size());
        chainWaitSeen_.assign(nodes_.size(), 0u);
        chainWaitUnsealedSeen_.assign(nodes_.size(), 0u);
        storeWaitSeen_.assign(nodes_.size(), 0u);
        pendingLead_.reserve(kMaxInputs);   // the Check in flight pushes at most one entry per input: no allocation on the render thread
        for (size_t i = 0; i < nodes_.size(); ++i)
            leadMark_[i].assign(size_t(nodes_[i].ring == 0 ? 1 : nodes_[i].ring), 0u);
    }
    ~SlotTable()
    {
        for (SlotSet* s : sets_)
            delete s;
    }
    SlotTable(const SlotTable&) = delete;
    SlotTable& operator=(const SlotTable&) = delete;

    using StampSource = std::function<VpIndicator::Stamp(uint64_t)>;
    // The Indicator StampOf, for the seal of lead ancestors (render thread; set before the first Resolve).
    void SetStampSource(StampSource f) { stampOf_ = std::move(f); }
    bool Valid() const { return valid_; }
    const char* Error() const { return error_; }
    size_t Count() const { return sets_.size(); }
    const Node& NodeAt(size_t i) const { return nodes_[i]; }
    SlotSet& At(size_t i) { return *sets_[i]; }
    SlotSet* Find(ProductId id)
    {
        for (size_t i = 0; i < nodes_.size(); ++i)
            if (nodes_[i].id == id)
                return sets_[i];
        return nullptr;
    }
    const Node* NodeOf(ProductId id) const
    {
        for (const Node& n : nodes_)
            if (n.id == id)
                return &n;
        return nullptr;
    }

    // Lane: pin every declared input of product `id` at frame `seq` and record them as the
    // header's ancestors. False (nothing pinned) when an input has no content yet.
    bool LaneAncestors(ProductId id, uint64_t seq, Header& hdr, LaneInputs& pins)
    {
        pins.count = 0;
        const Node* n = NodeOf(id);
        if (!n)
            return false;
        hdr.ancestorCount = 0;
        hdr.chainReduced = false;
        for (size_t k = 0; k < n->inputCount; ++k) {
            const InputRef& in = n->inputs[k];
            SlotSet* set = Find(in.product);
            AncestorRef ref;
            int live = -1;
            uint64_t aseq = 0;
            const bool haveSeq = InputSeq(in, seq, &aseq);
            // Ordered chain: the self input at lag >= 1 is optional. Absent (chain start, a predecessor that never
            // published, a skipped range) it is NOT waited for (our own lane finished frame seq-1 before this job
            // began: it will never appear): the job carries forward in its declared reduced form.
            // The chain pins only a RENDER-VALID predecessor (Sealed / Live): a Published-unsealed one could still be
            // rejected and live-filled at its seal, which would replace the ancestor this job recorded and make every
            // later frame fail with AncestorReplaced (a permanent per-frame live fill). It counts as Transient: the job waits.
            const bool chainSelf = n->orderedChain && in.product == id && in.lag > 0;
            if (!set || !haveSeq || !set->PinRef(aseq, &ref, &live, chainSelf)) {
                if (chainSelf && set && (!haveSeq || set->AbsenceOf(aseq) == SlotSet::Absence::Absent)) {
                    AncestorRef empty;
                    empty.productId = in.product;
                    empty.seq = haveSeq ? aseq : 0;
                    pins.sets[pins.count] = nullptr;   // positional: no pin, nothing to unpin
                    pins.seqs[pins.count] = empty.seq;
                    pins.liveIndex[pins.count] = -1;
                    ++pins.count;
                    hdr.ancestors[k] = empty;
                    hdr.ancestorCount = uint8_t(k + 1);
                    hdr.chainReduced = true;
                    continue;
                }
                if (chainSelf && set && haveSeq) {
                    // Waiting on a Rejected predecessor: it waits for the live fill (which may decline or find no live
                    // buffer: then it never comes and this chain stalls until the frame is released). Or on a Published
                    // one whose seal has not run. Counted once per frame, not per poll: the entries are private to this
                    // product's lane thread.
                    const int ni = IndexOfNode(id);
                    if (ni >= 0 && set->RejectedAwaitingLive(aseq)) {
                        if (chainWaitSeen_[size_t(ni)] != seq + 1u) {
                            chainWaitSeen_[size_t(ni)] = seq + 1u;
                            set->MutableCounters().chainWaitRejected.fetch_add(1, std::memory_order_relaxed);
                        }
                    } else if (ni >= 0 && set->PublishedAwaitingSeal(aseq)) {
                        if (chainWaitUnsealedSeen_[size_t(ni)] != seq + 1u) {
                            chainWaitUnsealedSeen_[size_t(ni)] = seq + 1u;
                            set->MutableCounters().chainWaitUnsealed.fetch_add(1, std::memory_order_relaxed);
                        }
                    }
                }
                if (!chainSelf && set && haveSeq) {
                    // A reader of a SINGLE-STORE product whose store frame is Rejected (the seal refused it): a store has no live pool, so the
                    // frame never gets content and the reader job waits until the frame is released. Counted once per reader frame (not per
                    // poll) on the store's set, so the wait is visible (status line "single-store reader frames waited ...").
                    const Node* src = NodeOf(in.product);
                    const int ni = IndexOfNode(id);
                    if (src && src->singletonStore && ni >= 0 && set->RejectedAwaitingLive(aseq) && storeWaitSeen_[size_t(ni)] != seq + 1u) {
                        storeWaitSeen_[size_t(ni)] = seq + 1u;
                        set->MutableCounters().storeWaitRejected.fetch_add(1, std::memory_order_relaxed);
                    }
                }
                pins.Unpin();
                hdr.ancestorCount = 0;
                hdr.chainReduced = false;
                return false;
            }
            pins.sets[pins.count] = set;
            pins.seqs[pins.count] = aseq;
            pins.liveIndex[pins.count] = live;
            ++pins.count;
            hdr.ancestors[k] = ref;
            hdr.ancestorCount = uint8_t(k + 1);
        }
        hdr.reads = n->reads;
        return true;
    }

    // The sealed (frozen) stamp of a recent frame; false when not recorded or overwritten.
    bool SealedStamp(uint64_t seq, VpIndicator::Stamp* out) const
    {
        const SealedRecord& r = sealed_[size_t(seq % sealed_.size())];
        if (!r.set || r.seq != seq)
            return false;
        if (out)
            *out = r.stamp;
        return true;
    }

    // Render, after frame `seq` is done (presented, or its render-ahead slot retired): releases, for
    // every product, the frame that just left its history (seq - history). This is the N2 rule:
    // an ancestor stays until every descendant that may record it has been sealed.
    void ReleaseAfter(uint64_t seq, uint64_t releaseValue = 0)
    {
        for (size_t i = 0; i < nodes_.size(); ++i) {
            const uint64_t h = nodes_[i].history;
            if (seq >= h)
                sets_[i]->Release(seq - h, releaseValue);
        }
    }

    uint64_t LiveFills() const { return liveFills_; }
    uint64_t FillDeclines() const { return fillDeclines_; }

    // Render, at render start of frame `seq`, with the Indicator's sealed stamp of that frame.
    // Seals every product in topological order; a product the seal rejects is live-filled at once
    // (so its descendants, sealed after it, see the new cgen: the cascade). fill(const Node&,
    // uint64_t seq, int liveIndex, Header& hdr, uint32_t missingInputs) writes the live buffer, sets
    // hdr.prov / hdr.costUs and returns false to decline; hdr.reads/honoured/ancestors are prepared
    // here. With an input missing, fill runs only for a product that declares a missing-input form,
    // and the result is marked LiveFallback (counted); otherwise it is unavailable (InputMissing).
    // Returns one result per node.
    template <class Fill>
    const std::vector<ProductFrameResult>& Resolve(uint64_t seq, const VpIndicator::Stamp& stampN, Fill fill)
    {
        SealedRecord& rec = sealed_[size_t(seq % sealed_.size())];
        rec.set = true;
        rec.seq = seq;
        rec.stamp = stampN;
        for (size_t i = 0; i < nodes_.size(); ++i) {
            const Node& n = nodes_[i];
            SlotSet& set = *sets_[i];
            ProductFrameResult& out = results_[i];
            out = ProductFrameResult();
            out.id = n.id;
            pendingLead_.clear();
            pendingChain_ = false;
            out.seal = set.Seal(seq, [&](const Header& h) { return Check(n, seq, stampN, h); });
            if (out.seal.outcome == SealOutcome::Sealed && out.seal.reason == SealReason::Accepted) {
                // Counted once, after the seal took effect (not per input, not on a CAS retry).
                for (ProductId p : pendingLead_)
                    if (SlotSet* src = Find(p))
                        src->MutableCounters().leadAccepted.fetch_add(1, std::memory_order_relaxed);
                if (pendingChain_)
                    set.MutableCounters().chainCarryForward.fetch_add(1, std::memory_order_relaxed);
                // Remember which frames were accepted as lead ancestors: their own seal comes later.
                for (size_t k = 0; k < n.inputCount; ++k)
                    if (n.inputs[k].lead > 0)
                        MarkLeadAccepted(n.inputs[k], seq);
            }
            if (out.seal.outcome == SealOutcome::NeedsLive) {
                uint64_t& m = leadMark_[i][size_t(set.SlotOf(seq))];
                if (m == seq + 1u)
                    set.MutableCounters().leadLaterRejected.fetch_add(1, std::memory_order_relaxed);
            }
            out.final = out.seal.outcome;
            out.finalReason = out.seal.reason;
            if (out.seal.outcome != SealOutcome::NeedsLive)
                continue;
            if (set.LivePool() == 0) {
                // Lane-only product (a single-store product): no live fill exists. The frame is absent and the reader runs its
                // own visible fallback (counted by the caller as an unavailable product frame).
                out.final = SealOutcome::Unavailable;
                out.finalReason = SealReason::NoLiveBuffer;
                continue;
            }
            const int live = set.BeginLive(seq);
            if (live < 0) {
                out.final = SealOutcome::Unavailable;
                out.finalReason = SealReason::NoLiveBuffer;
                continue;
            }
            Header h;
            h.reads = n.reads;
            h.honoured = stampN;
            h.prov = Provenance::LiveRender;
            // Ancestors stay positional (index k = input k); a missing input keeps an empty ref
            // (cgen 0, provenance None).
            uint32_t missing = 0;
            uint32_t leadUsed = 0;   // bit k: lead input k was read from a Published / valid lead ancestor
            for (size_t k = 0; k < n.inputCount; ++k) {
                const InputRef& in = n.inputs[k];
                SlotSet* src = Find(in.product);
                AncestorRef& r = h.ancestors[k];
                r = AncestorRef();
                r.productId = in.product;
                uint64_t aseq = 0;
                const bool haveSeq = InputSeq(in, seq, &aseq);
                r.seq = haveSeq ? aseq : 0;
                bool ok = src != nullptr && haveSeq;
                VpAncestry::SlotView v;
                if (ok) {
                    v = src->ViewAt(src->SlotOf(aseq));
                    // A lead input is read from the lane published result (its own seal comes later).
                    ok = v.present && v.seq == aseq && (v.valid || (in.lead > 0 && v.lanePublished));
                    // K1 also binds the live fill: a lead ancestor whose stamp moved (or is unknown) is a MISSING input
                    // (declared missing-input form, LiveFallback, visible), never a full result from stale data.
                    if (ok && in.lead > 0 && LeadStampReason(in.product, aseq) != SealReason::Accepted)
                        ok = false;
                }
                if (!ok) {
                    missing |= 1u << k;
                    continue;
                }
                if (in.lead > 0)
                    leadUsed |= 1u << k;
                r.slotIndex = src->SlotOf(aseq);
                r.cgen = v.cgen;
                r.prov = src->WordFor(aseq).prov;
            }
            h.ancestorCount = uint8_t(n.inputCount);
            out.missingInputs = missing;
            if (missing != 0 && !n.missingInputForm) {
                set.CancelLive(seq);
                set.MutableCounters().inputMissing.fetch_add(1, std::memory_order_relaxed);
                out.final = SealOutcome::Unavailable;
                out.finalReason = SealReason::InputMissing;
                continue;
            }
            if (!fill(n, seq, live, h, missing)) {
                set.CancelLive(seq);
                ++fillDeclines_;
                out.final = SealOutcome::Unavailable;
                out.finalReason = SealReason::FillDeclined;
                continue;
            }
            if (missing != 0) {
                // The declared reduced form: visible as a fallback, never as a full live result.
                h.prov = Provenance::LiveFallback;
                set.MutableCounters().liveMissingInputs.fetch_add(1, std::memory_order_relaxed);
            }
            if (set.CommitLive(seq, h)) {
                ++liveFills_;
                out.liveFilled = true;
                out.final = SealOutcome::Sealed;
                // A live fill that used a Published lead ancestor is the same residual K1 risk as a sealed reader: that ancestor
                // gets its own seal later and may be rejected then (leadLaterRejected).
                for (size_t k = 0; k < n.inputCount; ++k)
                    if ((leadUsed & (1u << k)) != 0u)
                        MarkLeadAccepted(n.inputs[k], seq);
            } else {
                set.CancelLive(seq);
                out.final = SealOutcome::Unavailable;
                out.finalReason = SealReason::OrderError;
            }
        }
        return results_;
    }

private:
    struct SealedRecord {
        bool set = false;
        uint64_t seq = 0;
        VpIndicator::Stamp stamp;
    };

    int IndexOfNode(ProductId id) const
    {
        for (size_t i = 0; i < nodes_.size(); ++i)
            if (nodes_[i].id == id)
                return int(i);
        return -1;
    }

    // Remember that frame seq + lead of the input's product was used as a lead ancestor: its own seal comes later (render thread).
    void MarkLeadAccepted(const InputRef& in, uint64_t seq)
    {
        const int j = IndexOfNode(in.product);
        if (j < 0)
            return;
        const uint64_t a = seq + in.lead;
        leadMark_[size_t(j)][size_t(sets_[size_t(j)]->SlotOf(a))] = a + 1u;
    }

    // K1: the content of frame `aseq` of a lead ancestor product must have been computed under the stamp of that frame.
    // Accepted, or why not (AncestryIncomplete without a stamp source or content, AncientStamp, StampMismatch).
    SealReason LeadStampReason(ProductId product, uint64_t aseq)
    {
        SlotSet* src = Find(product);
        const Node* an = NodeOf(product);
        VpIndicator::Stamp honoured;
        if (!stampOf_ || !src || !an || !src->HonouredOf(aseq, &honoured))
            return SealReason::AncestryIncomplete;
        const VpIndicator::Stamp want = stampOf_(aseq);
        if (want.ancient || honoured.ancient)
            return SealReason::AncientStamp;
        if (!VpIndicator::SameFor(honoured, want, an->reads))
            return SealReason::StampMismatch;
        return SealReason::Accepted;
    }

    // The seal's validation of a published lane header for frame `seq` (design B4 (a)/(b)).
    // RENDER THREAD ONLY (the single caller of Resolve -> Seal -> Check): leadMark_, pendingLead_ and pendingChain_ are
    // plain members with no synchronisation. Check may run more than once per product and frame (SlotSet::Seal retries
    // the CAS), so it starts from empty pending state: counts are taken once, by Resolve, after the seal took effect.
    SealReason Check(const Node& n, uint64_t seq, const VpIndicator::Stamp& stampN, const Header& h)
    {
        pendingLead_.clear();
        pendingChain_ = false;
        if (stampN.ancient || h.honoured.ancient)
            return SealReason::AncientStamp;
        if (h.seq != seq || h.cgen == 0)
            return SealReason::OrderError;
        if (!VpIndicator::SameFor(h.honoured, stampN, n.reads))
            return SealReason::StampMismatch;
        if (h.ancestorCount != n.inputCount)
            return SealReason::AncestryIncomplete;
        for (size_t k = 0; k < n.inputCount; ++k) {
            const InputRef& in = n.inputs[k];
            const AncestorRef& ref = h.ancestors[k];
            // Ordered chain carry-forward: the job declared its self lag input absent (Header::chainReduced) and
            // recorded an empty ancestor: accepted (visible in the counters), nothing to validate.
            if (h.chainReduced && n.orderedChain && in.product == n.id && in.lag > 0 && ref.productId == in.product
                && ref.cgen == 0) {
                // Carry-forward is only for a truly absent predecessor: if frame N-1 has valid content by now (sealed or
                // live-filled), the reduced result is rejected and the frame is made in full by the live fill.
                uint64_t pseq = 0;
                if (InputSeq(in, seq, &pseq)) {
                    SlotSet* self = Find(n.id);
                    const VpAncestry::SlotView pv = self->ViewAt(self->SlotOf(pseq));
                    if (pv.present && pv.seq == pseq && (pv.valid || pv.lanePublished))
                        return SealReason::AncestryIncomplete;
                }
                pendingChain_ = true;
                continue;
            }
            uint64_t aseq = 0;
            if (!InputSeq(in, seq, &aseq) || ref.productId != in.product || ref.seq != aseq)
                return SealReason::AncestryIncomplete;
            SlotSet* src = Find(ref.productId);
            if (!src || ref.slotIndex != src->SlotOf(ref.seq))
                return SealReason::AncestryIncomplete;
            const VpAncestry::SlotView v = src->ViewAt(ref.slotIndex);
            if (!v.present || v.seq != ref.seq)
                return SealReason::AncestorGone;
            if (v.cgen != ref.cgen)
                return v.cgen == 0 ? SealReason::AncestorInvalid : SealReason::AncestorReplaced;
            if (in.lead > 0) {
                // K1: the lead ancestor needs a result (Published is enough) computed under the stamp of ITS frame.
                if (!v.valid && !v.lanePublished)
                    return SealReason::AncestorInvalid;
                const SealReason lr = LeadStampReason(ref.productId, ref.seq);
                if (lr != SealReason::Accepted)
                    return lr;
                pendingLead_.push_back(ref.productId);
                continue;
            }
            if (!v.valid)
                return SealReason::AncestorInvalid;
            if (in.lag > 0 && n.spanMask != 0) {
                VpIndicator::Stamp past;
                if (!SealedStamp(ref.seq, &past)) {
                    src->MutableCounters().spanUnknown.fetch_add(1, std::memory_order_relaxed);
                    return SealReason::AncestorGone;
                }
                if (!VpIndicator::SameFor(past, stampN, n.spanMask))
                    return SealReason::SpanBroken;
            }
        }
        return SealReason::Accepted;
    }

    bool valid_ = true;
    const char* error_ = "";
    StampSource stampOf_;
    // Render thread only (see Check): the single caller contract for the three members below.
    std::vector<ProductId> pendingLead_;   // lead ancestors seen by the Check in flight (counted on Accepted)
    bool pendingChain_ = false;            // the Check in flight accepted a chain carry-forward
    std::vector<std::vector<uint64_t>> leadMark_;   // per node, per slot: frame + 1 accepted as a lead ancestor
    std::vector<uint64_t> chainWaitSeen_;   // per node: frame + 1 last counted in chainWaitRejected (that product's lane thread only)
    std::vector<uint64_t> storeWaitSeen_;   // per READER node: frame + 1 last counted in storeWaitRejected (that product's lane thread only)
    std::vector<uint64_t> chainWaitUnsealedSeen_;   // per node: frame + 1 last counted in chainWaitUnsealed (that product's lane thread only)
    std::vector<Node> nodes_;
    std::vector<SlotSet*> sets_;
    std::vector<SealedRecord> sealed_;
    std::vector<ProductFrameResult> results_;
    uint64_t liveFills_ = 0;
    uint64_t fillDeclines_ = 0;
};

// Preallocated CPU storage for a CPU product: one buffer per ring slot and per live buffer.
class CpuBuffers {
public:
    explicit CpuBuffers(const Node& n)
        : bytes_(size_t(n.bufferBytes))
        , ring_(size_t(n.ring) * size_t(n.bufferBytes))
        , live_(size_t(n.livePool) * size_t(n.bufferBytes))
    {
    }
    size_t Bytes() const { return bytes_; }
    uint8_t* Ring(size_t slot) { return ring_.data() + slot * bytes_; }
    uint8_t* Live(size_t index) { return live_.data() + index * bytes_; }

private:
    size_t bytes_;
    std::vector<uint8_t> ring_;
    std::vector<uint8_t> live_;
};

} // namespace VpSkeleton
