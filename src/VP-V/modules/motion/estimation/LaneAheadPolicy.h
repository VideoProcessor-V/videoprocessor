// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

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
// - No fixed matcher cost: the spare slot's MeasuredCost decides fit (NoSpare is the slot's verdict, counted here).
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

constexpr int kDepthTarget = 4;   // TD design point
constexpr int kDepthMin = 2;      // the ring holds at least two pairs
constexpr int kDepthMax = 4;

// Ring capacity from a requested depth: within [kDepthMin, kDepthMax].
inline int RingCapacity(int requested = kDepthTarget)
{
    return requested < kDepthMin ? kDepthMin : (requested > kDepthMax ? kDepthMax : requested);
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
    default: return "?";
    }
}

struct Input {
    SpareKey key = SpareKey::Auto;
    bool lowLatency = false;
    bool competitive = false;
    bool matcherActive = false;   // Active OR the analysis estimate (93-prep-B3Z s3)
    int requestedDepth = kDepthTarget;
    int queuedAfterNext = 0;      // frames queued after the render frame's next (each one pair deeper)
    int laneLead = 0;             // the analysis lane's lead when it is on (usable = lead - 1); 0 = lane off
    int held = 0;                 // pairs already in the ring (chained from the render pair)
    bool producerRunning = false;
    bool nextPairP2Ready = true;  // the first pair to estimate has its P2 in the slot
};

struct Decision {
    Reason reason = Reason::Off;
    int depth = 0;         // usable depth this slot
    int pairsWanted = 0;   // > 0 only with Reason::Ahead
};

inline Decision Decide(const Input& in)
{
    Decision d;
    if (!KeyOn(in.key)) { d.reason = Reason::Off; return d; }
    if (in.competitive) { d.reason = Reason::Competitive; return d; }
    if (in.lowLatency) { d.reason = Reason::LowLatency; return d; }
    if (!in.matcherActive) { d.reason = Reason::EngineOff; return d; }
    int depth = RingCapacity(in.requestedDepth);
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
// item 2). Nothing is reserved for it here.
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

}   // namespace LaneAhead
}   // namespace VpMotion
