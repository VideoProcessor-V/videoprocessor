// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <cstddef>
#include <cstdint>

#include <VP-V/core/AheadCeiling.h>

// B3-Z Z2 scaffold (refs 93-prep-B3Z s2, s3, s6; TD design 99 s3.3, s3.4, s12 item 2): the matcher top-up in the
// post-present spare slot (core/PostPresentSpare.h, class Core, order 1) as a pure decision, plus the pair-keyed
// entry layout Z1 (ring + slots) and Z3 (engine ring + feed) build on. No host types, no GPU, no clock. C++14.
//
// - Key motion_estimate_ahead_spare = auto | on | off (VP-V master page). auto = off until the Z4 ledger row shows
//   render ms saved with no added late presents (owner Q2; C5b precedent OnDemandEstimate auto).
// - Low latency (1-frame) = single-frame motion: no field ahead, reason LowLatency. Competitive: nothing.
// - Depth target 4 frames ahead, ring minimum 2 (TD 99 s3.4; doc 93 s8e); bounded by the frames really queued
//   after the render frame's next and by the lane lead (lead - 1) when the lane is on - never added queueing.
// - E3: a lane pair only when its P2 is in the slot or the producer is not running (P2NotReady, retried next slot).
// - No fixed matcher cost: the spare slot's SpareMeasuredCost decides fit (NoSpare is the slot's verdict, counted here).
namespace VpMotion {
namespace LaneAhead {

enum class SpareKey : uint8_t { Auto = 0, On, Off };

inline const char* KeyName(SpareKey k)
{
    switch (k) {
    case SpareKey::Auto: return "auto";
    case SpareKey::On: return "on";
    case SpareKey::Off: return "off";
    default: return "?";
    }
}

// Accepts exactly auto / on / off (the config layer lower-cases); false leaves `out` unchanged.
inline bool ParseKey(const char* text, SpareKey& out)
{
    if (!text) return false;
    const char* names[] = { "auto", "on", "off" };
    for (int i = 0; i < 3; ++i) {
        const char* a = names[i];
        const char* b = text;
        while (*a && *a == *b) { ++a; ++b; }
        if (*a == 0 && *b == 0) { out = static_cast<SpareKey>(i); return true; }
    }
    return false;
}

// auto resolves to off today (owner Q2).
inline bool KeyOn(SpareKey k) { return k == SpareKey::On; }

// S-3a S1 (coordinator 2026-11-14 08:22 via lead 10, Q5): auto = ON once the S1 acceptance row (render ms saved, no added late presents) is
// recorded, shown in status. The host passes `autoAccepted` from that row; without it auto stays off exactly as KeyOn says.
inline bool KeyOn(SpareKey k, bool autoAccepted) { return k == SpareKey::On || (k == SpareKey::Auto && autoAccepted); }

inline const char* KeyStateText(SpareKey k, bool autoAccepted)
{
    if (k == SpareKey::Auto) return autoAccepted ? "auto (on: S1 acceptance recorded)" : "auto (off until the S1 acceptance row)";
    return KeyName(k);
}

constexpr int kDepthTarget = 4;   // TD design point
constexpr int kDepthMin = 2;      // the ring holds at least two pairs

// The maximum depth is derived, never a fixed 4 (doc 101 C6; TD 99 s3.4: design lead = 2 + anchor slots; the
// extended tier may want more). Structural cap = the P2 ring: 16 pairs minus the 3 kept behind = 13. These mirror
// MotionAhead.h kP2RingPairs / kPairsKeptBehind / kRingMaxLead (this header is included by core/PostPresentSpare.h, so it does
// not include the motion header; LaneAheadPolicyTests static_asserts the three against it, audit S8).
// BEHAVIOUR-CHANGE OD-11-14-AHEAD-CAP (lead10/design-queue-keyed-products.md s10): the depth is ALSO held to the ahead-compute
// ceiling core/AheadCeiling.h kAheadCeiling 6 (the same cap as MotionAhead::kMaxLead). BEFORE: the extended tier and a large design
// lead ran up to the ring's 13; AFTER: at most 6. A ceiling, never a target.
constexpr int kP2RingPairs = 16;
constexpr int kPairsKeptBehind = 3;
constexpr int kStructuralMaxLead = kP2RingPairs - kPairsKeptBehind;
constexpr int kAheadCeilingLead = int(VpSkeleton::kAheadCeiling) < kStructuralMaxLead ? int(VpSkeleton::kAheadCeiling) : kStructuralMaxLead;
constexpr int kAnchorSlotsDefault = 2;

inline int DesignLead(int anchorSlots = kAnchorSlotsDefault)
{
    return 2 + (anchorSlots > 0 ? anchorSlots : 0);
}

// Standard tier: the design lead, capped by the ahead-compute ceiling (within the P2 ring). Extended tier: the cap itself.
inline int DepthMax(int anchorSlots = kAnchorSlotsDefault, bool extendedTier = false)
{
    const int lead = extendedTier ? kAheadCeilingLead : DesignLead(anchorSlots);
    const int capped = lead > kAheadCeilingLead ? kAheadCeilingLead : lead;
    return capped < kDepthMin ? kDepthMin : capped;
}

// Ring capacity from a requested depth: within [kDepthMin, maxDepth] (default: the standard-tier design lead, 4).
inline int RingCapacity(int requested = kDepthTarget, int maxDepth = DepthMax())
{
    return requested < kDepthMin ? kDepthMin : (requested > maxDepth ? maxDepth : requested);
}

enum class Reason : uint8_t {
    Ahead = 0,      // pairs wanted this slot
    Off,            // key off (or auto)
    LowLatency,     // single-frame motion: no field ahead
    Competitive,    // no field consumers estimate at all
    EngineOff,      // the matcher is not estimating for the next frame
    ShortQueue,     // no frame after the render frame's next is queued
    RingFull,       // depth already held
    P2NotReady,     // E3: the producer runs and the pair's P2 is not in its slot yet
    NoSpare,        // the slot's budget did not fit the matcher (set by the host from the slot's report)
    SourceNotServed,   // S-3a S1: Acquire(P13 source ring) of a pair frame was Unavailable in the slot (its owner serves no view there)
    Count
};

inline const char* ReasonName(Reason r)
{
    switch (r) {
    case Reason::Ahead: return "ahead";
    case Reason::Off: return "off";
    case Reason::LowLatency: return "low latency: single-frame motion, no field ahead";
    case Reason::Competitive: return "competitive latency";
    case Reason::EngineOff: return "matcher not estimating";
    case Reason::ShortQueue: return "short queue";
    case Reason::RingFull: return "depth held";
    case Reason::P2NotReady: return "P2 not ready";
    case Reason::NoSpare: return "no spare";
    case Reason::SourceNotServed: return "source ring not served in the slot";
    default: return "?";
    }
}

struct Input {
    SpareKey key = SpareKey::Auto;
    bool lowLatency = false;
    bool competitive = false;
    bool matcherActive = false;   // Active OR the analysis estimate (93-prep-B3Z s3)
    int requestedDepth = kDepthTarget;   // the TD design point; the host passes kDepthMin (2) while Q3 says "depth 2 now"
    int anchorSlots = kAnchorSlotsDefault;   // TD anchor slots (design lead = 2 + anchorSlots)
    bool extendedTier = false;               // extended tier: up to the P2 ring cap
    int queuedAfterNext = 0;      // frames queued after the render frame's next (each one pair deeper)
    int laneLead = 0;             // the analysis lane's lead when it is on (usable = lead - 1); 0 = lane off
    int held = 0;                 // pairs already in the ring (chained from the render pair)
    bool producerRunning = false;
    bool nextPairP2Ready = true;  // the first pair to estimate has its P2 in the slot
    bool autoAccepted = false;    // S-3a S1 / Q5: auto resolves to on (KeyOn(key, autoAccepted)); false = today's rule
};

struct Decision {
    Reason reason = Reason::Off;
    int depth = 0;         // usable depth this slot
    int pairsWanted = 0;   // > 0 only with Reason::Ahead
};

inline Decision Decide(const Input& in)
{
    Decision d;
    if (!KeyOn(in.key, in.autoAccepted)) { d.reason = Reason::Off; return d; }
    if (in.competitive) { d.reason = Reason::Competitive; return d; }
    if (in.lowLatency) { d.reason = Reason::LowLatency; return d; }
    if (!in.matcherActive) { d.reason = Reason::EngineOff; return d; }
    int depth = RingCapacity(in.requestedDepth, DepthMax(in.anchorSlots, in.extendedTier));
    if (in.queuedAfterNext < depth) depth = in.queuedAfterNext > 0 ? in.queuedAfterNext : 0;
    if (in.laneLead > 0 && in.laneLead - 1 < depth) depth = in.laneLead - 1 > 0 ? in.laneLead - 1 : 0;
    d.depth = depth;
    if (depth <= 0) { d.reason = Reason::ShortQueue; return d; }
    if (in.held >= depth) { d.reason = Reason::RingFull; return d; }
    if (in.producerRunning && !in.nextPairP2Ready) { d.reason = Reason::P2NotReady; return d; }
    d.reason = Reason::Ahead;
    d.pairsWanted = depth - (in.held > 0 ? in.held : 0);
    return d;
}

// Counted reasons for the status line (no silent fallbacks).
class ReasonCounts {
public:
    void Note(Reason r) { m_last = r; ++m_counts[static_cast<size_t>(r)]; }
    unsigned long long Count(Reason r) const { return m_counts[static_cast<size_t>(r)]; }
    Reason Last() const { return m_last; }

private:
    Reason m_last = Reason::Off;
    unsigned long long m_counts[static_cast<size_t>(Reason::Count)] = {};
};

// ---- Entry layout for Z1 / Z3 (pair-keyed) ----
// An entry is keyed by its pair and the epoch it was made in (E12: both sides of a consume use this epoch).
struct EntryKey {
    uint64_t from = 0;
    uint64_t to = 0;
    uint64_t epoch = 0;
    bool Valid() const { return from != 0 && to > from && epoch != 0; }
    bool SamePair(const EntryKey& o) const { return from == o.from && to == o.to; }
    bool operator==(const EntryKey& o) const { return SamePair(o) && epoch == o.epoch; }
    bool operator!=(const EntryKey& o) const { return !(*this == o); }
};

// Room for ONE more block-grid texture per entry (TD 99 s12 item 2): P3b, the backward verified field on the LATER
// frame's 16 px grid, with its new-content flag. Opaque handle: the host owns the texture.
constexpr int kExtraGridTextures = 1;
constexpr uint32_t kExtraGridCellPx = 16;

struct EntryExtraGrid {
    const void* texture = nullptr;   // host texture (null = not made)
    uint32_t cellPx = kExtraGridCellPx;
    uint64_t onFrame = 0;            // the later frame of the pair (== EntryKey::to)
    bool newContent = false;
};

// Occlusion: Z1 decides whether it is a texture per entry or made inside the P3b pass (93-prep-B3Z Q7; TD 99 s12
// item 2; doc 101 row 36: "decide and test with Z1" - Z1 is not built, so the decision and its test land there).
// Nothing is reserved for it here.
// Z3 notes (doc 101; not built here): the P3b / P3r rings are levels of kUserMotionMatcher, declared with
// VramGovernorTests rows in Z3; P5c snapshot consistency is a Z3 acceptance row.
struct Entry {
    EntryKey key;
    EntryExtraGrid extra[kExtraGridTextures];
};

// The hook point after each estimate (spare slot AND render time, also in low latency): both frames' luma planes
// are still bound, so P3b / P3r run without a second upload. Opaque plane handles.
struct AfterEstimateView {
    EntryKey key;
    const void* fromLuma = nullptr;
    const void* toLuma = nullptr;
    bool atRender = false;   // the render-time estimate (a consume miss, low latency) vs the spare slot
};

class IAfterEstimate {
public:
    virtual ~IAfterEstimate() {}
    virtual void AfterEstimate(const AfterEstimateView& view, Entry& entry) = 0;
};

// The one dispatch both estimate paths use (doc 101 row 9: IAfterEstimate is called exactly once per estimate with
// both luma planes valid, on the spare path AND the render-time path). An estimate is identified by its serial
// (the matcher's estimate counter, > 0). A repeat of the last serial, a missing plane or an invalid key is not
// dispatched and is counted.
enum class AfterEstimateResult : uint8_t { Called = 0, NoHook, PlanesMissing, BadKey, Repeat, Count };

class AfterEstimateDispatch {
public:
    AfterEstimateResult Notify(IAfterEstimate* hook, const AfterEstimateView& view, Entry& entry, uint64_t serial)
    {
        AfterEstimateResult r = AfterEstimateResult::Called;
        if (serial == 0 || serial == m_lastSerial) r = AfterEstimateResult::Repeat;
        else if (!view.key.Valid() || view.key != entry.key) r = AfterEstimateResult::BadKey;
        else if (!view.fromLuma || !view.toLuma) r = AfterEstimateResult::PlanesMissing;
        else if (!hook) r = AfterEstimateResult::NoHook;
        if (r != AfterEstimateResult::Repeat) m_lastSerial = serial;
        if (r == AfterEstimateResult::Called) hook->AfterEstimate(view, entry);
        ++m_counts[static_cast<size_t>(r)];
        if (r == AfterEstimateResult::Called) ++m_byPath[view.atRender ? 1 : 0];
        return r;
    }
    unsigned long long Count(AfterEstimateResult r) const { return m_counts[static_cast<size_t>(r)]; }
    unsigned long long CalledAtRender() const { return m_byPath[1]; }
    unsigned long long CalledInSpare() const { return m_byPath[0]; }

private:
    uint64_t m_lastSerial = 0;
    unsigned long long m_counts[static_cast<size_t>(AfterEstimateResult::Count)] = {};
    unsigned long long m_byPath[2] = {};
};

// ---- Z1: the chain ring (refs 93-prep-B3Z s3, rules E1 / E12; S-3a S1) ----
// The ONE arbiter of ahead pairs: entries strictly chained (entry k.to == entry k+1.from, E1), each pair held at most once (compute once),
// each keyed by the epoch it was made in (E12). At depth 1 it behaves as Dejudder::AheadSchedule's one-pair entry (hit = the front pair with
// its epoch; anything else = a miss that discards every entry, the render estimates the pair as today). Pure bookkeeping: the textures live in
// the host's slots (Entry::extra, the matcher's SnapshotForWarp(slot)); the slot of an entry is its ring position.
enum class DiscardCause : uint8_t {
    NextDiffers = 0,   // the frame after the render frame is not the entry's `to` (replaced frame / chain broken)
    Drop,              // a frame the ring held a pair for was passed without being consumed (dropped / skipped frame)
    Resync,            // queue reset / seek / resync (host)
    EpochMoved,        // the right pair, made in another validity epoch
    PictureArea,       // the active picture changed after the estimate (host)
    LeaseLost,         // the host lost the entry's slot texture
    Count
};

inline const char* DiscardCauseName(DiscardCause c)
{
    switch (c) {
    case DiscardCause::NextDiffers: return "next differs";
    case DiscardCause::Drop: return "drop";
    case DiscardCause::Resync: return "resync";
    case DiscardCause::EpochMoved: return "epoch moved";
    case DiscardCause::PictureArea: return "picture";
    case DiscardCause::LeaseLost: return "lease lost";
    default: return "?";
    }
}

enum class PushResult : uint8_t { Held = 0, BadKey, NotChained, AlreadyHeld, Full, Count };

class AheadRing {
public:
    static constexpr int kCapacityMax = kStructuralMaxLead;

    explicit AheadRing(int capacity = RingCapacity()) { SetCapacity(capacity); }

    // The ring's depth target (bounded to [1, kCapacityMax]); a smaller capacity drops the deepest entries (counted as Drop: never silent).
    void SetCapacity(int capacity)
    {
        m_capacity = capacity < 1 ? 1 : (capacity > kStructuralMaxLead ? kStructuralMaxLead : capacity);
        while (m_count > m_capacity) { --m_count; ++m_discarded[static_cast<size_t>(DiscardCause::Drop)]; }
    }
    int Capacity() const { return m_capacity; }
    int Held() const { return m_count; }
    bool Empty() const { return m_count == 0; }
    const EntryKey& At(int i) const { return m_entries[static_cast<size_t>(i)]; }
    // The frame the next pair starts from (the back's `to`; 0 = empty: the host starts at the render frame's next).
    uint64_t BackTo() const { return m_count > 0 ? m_entries[static_cast<size_t>(m_count - 1)].to : 0; }
    bool Holds(uint64_t from, uint64_t to) const
    {
        for (int i = 0; i < m_count; ++i)
            if (m_entries[static_cast<size_t>(i)].from == from && m_entries[static_cast<size_t>(i)].to == to) return true;
        return false;
    }

    PushResult Push(const EntryKey& k)
    {
        PushResult r = PushResult::Held;
        if (!k.Valid()) r = PushResult::BadKey;
        else if (Holds(k.from, k.to)) r = PushResult::AlreadyHeld;
        else if (m_count > 0 && k.from != BackTo()) r = PushResult::NotChained;
        else if (m_count >= m_capacity) r = PushResult::Full;
        ++m_push[static_cast<size_t>(r)];
        if (r == PushResult::Held) m_entries[static_cast<size_t>(m_count++)] = k;
        return r;
    }

    // At the render frame's estimate of (from, to) in `epoch`: true = the front entry is that pair from that epoch (taken; the render reads
    // its slot, no estimate). False = the render estimates now; entries the render passed are discarded as Drop, a front entry of another
    // pair or epoch discards the whole chain (E1: never a chain with a hole).
    bool Consume(uint64_t from, uint64_t to, uint64_t epoch)
    {
        while (m_count > 0 && m_entries[0].from < from) { PopFront(); ++m_discarded[static_cast<size_t>(DiscardCause::Drop)]; }
        if (m_count == 0) { ++m_sequential; m_consumedEpoch = 0; return false; }
        const EntryKey& f = m_entries[0];
        if (f.from == from && f.to == to && epoch != 0 && f.epoch == epoch) {
            m_consumedEpoch = f.epoch;
            PopFront();
            ++m_hits;
            return true;
        }
        Discard(f.from == from && f.to == to ? DiscardCause::EpochMoved : DiscardCause::NextDiffers);
        ++m_sequential;
        m_consumedEpoch = 0;
        return false;
    }

    // Host-detected causes (Resync, PictureArea, LeaseLost) and the internal ones: every entry goes, counted per entry.
    void Discard(DiscardCause c)
    {
        m_discarded[static_cast<size_t>(c)] += static_cast<unsigned long long>(m_count);
        m_count = 0;
    }
    void Reset() { Discard(DiscardCause::Resync); }

    unsigned long long Hits() const { return m_hits; }
    unsigned long long Sequential() const { return m_sequential; }   // render-time estimates (no entry to take)
    unsigned long long Discarded(DiscardCause c) const { return m_discarded[static_cast<size_t>(c)]; }
    unsigned long long Pushed(PushResult r) const { return m_push[static_cast<size_t>(r)]; }
    uint64_t ConsumedEpoch() const { return m_consumedEpoch; }

private:
    void PopFront()
    {
        for (int i = 1; i < m_count; ++i) m_entries[static_cast<size_t>(i - 1)] = m_entries[static_cast<size_t>(i)];
        --m_count;
    }

    EntryKey m_entries[kStructuralMaxLead];
    int m_capacity = 1;
    int m_count = 0;
    uint64_t m_consumedEpoch = 0;
    unsigned long long m_hits = 0, m_sequential = 0;
    unsigned long long m_discarded[static_cast<size_t>(DiscardCause::Count)] = {};
    unsigned long long m_push[static_cast<size_t>(PushResult::Count)] = {};
};

// ---- Z3: the engine-side feed of held entries (refs 93-prep-B3Z s3 "AheadFieldSeqs", s5 "Feed") ----
// FeedEngine publishes EVERY held entry not yet published, ascending; the published frame number is monotonic, so an entry is fed once.
// Pure: the host publishes the slot for each `to` returned.
class AheadFeed {
public:
    // The `to` frames of held entries not yet published, ascending (written to out[0..n), n returned; at most `max`).
    int Pending(const AheadRing& ring, uint64_t* out, int max) const
    {
        int n = 0;
        for (int i = 0; i < ring.Held() && n < max; ++i)
            if (ring.At(i).to > m_lastPublished) out[n++] = ring.At(i).to;
        return n;
    }
    void MarkPublished(uint64_t to) { if (to > m_lastPublished) { m_lastPublished = to; ++m_published; } }
    void Reset() { m_lastPublished = 0; }
    uint64_t LastPublished() const { return m_lastPublished; }
    unsigned long long Published() const { return m_published; }

private:
    uint64_t m_lastPublished = 0;
    unsigned long long m_published = 0;
};

// ---- VRAM per tier of the ring (risk row of brief S-3a S1; geometry upper bound of 93-prep-B3Z s7) ----
// Per held entry: the matcher's OWN outputs only - the 16 px block field (rgba32f, 16 B per block) + the 4 sub-vectors per block (64 B) + the
// half-resolution pixel mask (R16, 2 B per half-res pixel). The pair's source planes are NOT held here: they are Acquired from the P13 source
// ring (one copy, rule 1; coordinator ruling via lead 10, 2026-11-14), so their bytes are on P13's owner's ledger line, never on this one.
// Derived from the dimensions, never a table.
inline uint64_t EntryFieldBytes(uint32_t width, uint32_t height)
{
    const uint64_t bw = (uint64_t(width) + 15u) / 16u, bh = (uint64_t(height) + 15u) / 16u;
    const uint64_t hw = (uint64_t(width) + 1u) / 2u, hh = (uint64_t(height) + 1u) / 2u;
    return bw * bh * (16u + 64u) + hw * hh * 2u;
}
inline uint64_t RingVramBytes(uint32_t width, uint32_t height, int depth)
{
    return depth <= 0 ? 0u : uint64_t(depth) * EntryFieldBytes(width, height);
}
// What P13's owner would add per queued frame held to the slot when it extends the ring (the named owner change, S3A-UNPACK-SLOT /
// P13-AHEAD-VIEWS): 16-bit luma + 4:2:0 16-bit chroma = 3 B per source pixel. Reported for the ledger; not allocated by this unit.
inline uint64_t SourceRingFrameBytes(uint32_t width, uint32_t height) { return uint64_t(width) * uint64_t(height) * 3u; }

}   // namespace LaneAhead
}   // namespace VpMotion
