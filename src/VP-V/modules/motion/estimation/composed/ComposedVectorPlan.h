// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// ENGINE-COMPOSED-VECTORS depth law (OD 11-14, lead 13 via the coordinator; lead9\brief-S-3c-P10g-T2-T5.md owner block "weak texture";
// register row ENGINE-COMPOSED-VECTORS). Pure decision, no host types, no clock, no GPU. Header-only, C++14.
//
// The product is read by P10g (multi-frame grain level, borrowed-grain neighbours) and TemporalDetail. Each active reader DECLARES how many
// frames it needs behind and ahead of the current frame, per mode; the product computes
//     back  = min(max over active readers of their back need, kBackCeiling, frames kept behind the current frame)
//     ahead = min(max over active readers of their ahead need, VpSkeleton::kAheadCeiling, frames queued ahead)      (0 in low latency)
// (the ahead rule is the one shared ceiling, core/AheadCeiling.h, OD 11-14 09:10: an upper bound, never a target). No active reader
// = 0 / 0: nothing is computed for nobody (get-or-compute-and-publish, rule 2). A cap that bites is NEVER silent: Plan::note says what
// was asked and what the queue or history allowed, for the reader's Ctrl+I line (rule 5).
//
// Frame counts here are the owner's design numbers (they are counts of NEIGHBOURS the readers use, not rates, rule 6):
//   standard      8 back + 4 ahead  (the borrowed-grain neighbour set; TD may ask up to the ceiling, 6, ahead)
//   low latency   10 back + 0 ahead (no frame ahead exists: the ahead share is replaced by older frames; owner 11-14: "back-6 for low latency"
//                 is the number of NEIGHBOURS the grain level USES, the product keeps the longer chain so the readers can choose by trust)

#include <VP-V/core/AheadCeiling.h>

#include <algorithm>
#include <cstdint>
#include <string>

namespace VpComposed {

constexpr uint16_t kBackStandard = 8;       // OD 11-14: borrowed grain 8 back ...
constexpr uint16_t kAheadStandard = 4;      // ... + 4 ahead
constexpr uint16_t kBackLowLatency = 10;    // low latency 10 back ...
constexpr uint16_t kAheadLowLatency = 0;    // ... + 0 ahead
constexpr uint16_t kBackCeiling = 16;       // memory bound of the chain (planes are kept per offset); a request above it is capped, visibly

enum class Reader : uint8_t { P10gGrainLevel = 0, TemporalDetail, kCount };

inline const char* ReaderName(Reader r)
{
    switch (r) {
    case Reader::P10gGrainLevel: return "grain level";
    case Reader::TemporalDetail: return "temporal detail";
    default: return "?";
    }
}

struct Need
{
    uint16_t back = 0, ahead = 0;
};

// What each reader asks for in each mode (the readers' lanes may change their own row through Product::SetNeed; these are the defaults).
// TemporalDetail: the ahead need is the ceiling (TD designs for up to 6 ahead, brief owner block); its back need follows the standard set.
inline Need DefaultNeed(Reader r, bool lowLatency)
{
    Need n;
    if (lowLatency) {
        n.back = kBackLowLatency;
        n.ahead = kAheadLowLatency;
        return n;
    }
    n.back = kBackStandard;
    n.ahead = (r == Reader::TemporalDetail) ? VpSkeleton::kAheadCeiling : kAheadStandard;
    return n;
}

enum class Cap : uint8_t { None = 0, NoReader, LowLatency, Queue, Ceiling, History };

inline const char* CapName(Cap c)
{
    switch (c) {
    case Cap::None: return "none";
    case Cap::NoReader: return "no active reader";
    case Cap::LowLatency: return "low latency (no frame ahead)";
    case Cap::Queue: return "the queue holds fewer frames ahead";
    case Cap::Ceiling: return "the ahead ceiling";
    case Cap::History: return "fewer frames kept behind";
    }
    return "?";
}

struct Plan
{
    uint16_t back = 0, ahead = 0;
    uint16_t wantBack = 0, wantAhead = 0;   // the readers' maximum asks before the caps
    Cap backCap = Cap::None, aheadCap = Cap::None;
    // Plain words for the readers' status line; empty when nothing bit (a low-latency plan is the designed shape, not a downgrade).
    std::string note;
};

struct PlanInput
{
    bool active[static_cast<size_t>(Reader::kCount)] = { false, false };
    Need need[static_cast<size_t>(Reader::kCount)] = {};   // read only where active
    bool lowLatency = false;
    unsigned queuedAhead = 0;     // source frames queued AFTER the current frame
    unsigned keptBehind = 0;      // source frames kept BEHIND the current frame (the keep window; planes need their pair history)
};

inline Plan PlanDepth(const PlanInput& in)
{
    Plan p;
    bool any = false;
    for (size_t i = 0; i < static_cast<size_t>(Reader::kCount); ++i) {
        if (!in.active[i])
            continue;
        any = true;
        p.wantBack = (std::max)(p.wantBack, in.need[i].back);
        p.wantAhead = (std::max)(p.wantAhead, in.need[i].ahead);
    }
    if (!any) {
        p.backCap = p.aheadCap = Cap::NoReader;
        return p;
    }
    // Back.
    uint16_t back = (std::min)(p.wantBack, kBackCeiling);
    if (back < p.wantBack)
        p.backCap = Cap::Ceiling;
    if (in.keptBehind < back) {
        back = static_cast<uint16_t>(in.keptBehind);
        p.backCap = Cap::History;
    }
    p.back = back;
    // Ahead.
    if (in.lowLatency) {
        p.ahead = 0;
        p.aheadCap = Cap::LowLatency;
    } else {
        uint16_t ahead = (std::min)(p.wantAhead, VpSkeleton::kAheadCeiling);
        if (ahead < p.wantAhead)
            p.aheadCap = Cap::Ceiling;
        if (in.queuedAhead < ahead) {
            ahead = static_cast<uint16_t>(in.queuedAhead);
            p.aheadCap = Cap::Queue;
        }
        p.ahead = ahead;
    }
    // The note: only a cap that cut below what was asked in this mode (low latency has asked for 0 ahead by design).
    std::string note;
    if (p.back < p.wantBack)
        note += "composed vectors: " + std::to_string(p.back) + " of " + std::to_string(p.wantBack) + " frames back (" + CapName(p.backCap) + ")";
    if (!in.lowLatency && p.ahead < p.wantAhead)
        note += std::string(note.empty() ? "" : "; ") + "composed vectors: " + std::to_string(p.ahead) + " of " + std::to_string(p.wantAhead) +
                " frames ahead (" + CapName(p.aheadCap) + ")";
    p.note = note;
    return p;
}

}   // namespace VpComposed
