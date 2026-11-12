// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "ProductCatalog.h"
#include "ProductGraph.h"
#include "Products.h"
#include "RingContract.h"

#include <cstdint>
#include <string>
#include <vector>

// P13-DECL (TD reserve items 2 and 9, refs/99c-td-method-search.md A3 :77-80 and :94-96; decision: coordinator
// 2026-11-11, P13 Q2 = the reserve shows on the skeleton VRAM governor STATUS LINE as "reserved, not allocated", per tier).
//
// DECLARATIONS ONLY. These rows are the VRAM governor's RESERVATION figures: bytes are SHOWN, none is TAKEN, no allocator,
// governor plan, graph or product reads them (0 bytes allocated, 0 behaviour change). The temporal-detail round turns
// them into real governor users. Nothing in Bill's files; pure C++14, no GPU, no Windows.
//
// Tiers are BUDGET tiers (owner 2026-11-07: tiers are budget-driven, never one named card). The full-budget tier is the
// one the A3 figures were written for; the reduced-budget tier is the visible smaller reserve for a card whose VRAM does
// not fit it. Which one a run gets is the governor's decision at configure (TD round); until then both are listed.
//
// The frame counts below are SIZING CEILINGS of a VRAM reservation (how many frames the reserve is dimensioned for),
// not depth constants: the window the ring actually keeps is RingWindowFrames(BACK, LEAD) with BACK and LEAD derived
// (core/RingContract.h), and ClampWindowToReserve() reports when the derived window exceeds the reserve (visible).
namespace VpSkeleton {

enum class ReserveTier : uint8_t { FullBudget, ReducedBudget };

inline const char* ReserveTierName(ReserveTier t)
{
    return t == ReserveTier::FullBudget ? "full-budget tier" : "reduced-budget tier";
}

// Planning geometry the byte figures are written for (A3 / plan 99 s4: "decimal MB at 4K"). Other sizes scale by pixels.
constexpr uint64_t kReservePlanningWidth = 3840;
constexpr uint64_t kReservePlanningHeight = 2160;

// Plan 99 s4 :282 per held frame at 4K: luma 19.7 MB; native 4:2:0 received chroma 8.3 MB (two planes, 1920x1080, 16 bit).
constexpr uint64_t kReserveLumaBytesPerFrame4K = 19700000;
constexpr uint64_t kReserveChromaBytesPerFrame4K = 8300000;

// The temporal-detail store reserve ceiling at 4K, full-budget tier (A3 item 9: "about 450 MB"; plan 99 s4: 297 luma + 142 chroma = 439).
constexpr uint64_t kReserveTdCeilingBytes4K = 450000000;

// One reservation row. `bytes4K` is the reserved figure at the planning geometry; ReserveRowBytes() scales it to the source size.
struct ReserveRow {
    ReserveTier tier;
    ProductId product;       // the catalog product the reserve belongs to (P13 or a kTd* id)
    const char* role;        // P13: "keyed source ring"; TD: the store ROLE (recent ring, peak pins, anchor delta, prediction plane, records, maps)
    uint8_t ringFrames;      // P13 rows: frames the reserve is dimensioned for (a sizing ceiling, not a depth); 0 for TD roles
    uint8_t pins;            // P13 rows: frames a consumer may pin beyond the window (kMaxRingPins); 0 for TD roles
    uint64_t bytes4K;        // reserved bytes at the planning geometry
    Catalog::FlagKind kind;           // always Catalog::FlagKind::DeclaredNotAllocated
    const char* note;        // where the figure comes from
};

// Bytes of a reserve row at the source size (pixel-proportional; integer arithmetic; 0 size = the planning geometry).
inline uint64_t ReserveRowBytes(const ReserveRow& r, const Dims& d)
{
    if (d.width == 0 || d.height == 0)
        return r.bytes4K;
    return r.bytes4K * (uint64_t(d.width) * uint64_t(d.height)) / (kReservePlanningWidth * kReservePlanningHeight);
}

inline const std::vector<ReserveRow>& ReserveRows()
{
    // P13, full-budget: 8 frames + 3 pins, luma + chroma. 11 x (19.7 + 8.3) = 308 MB (A3 item 2: "about 320 MB with chroma",
    // "about 220 MB luma": 11 x 19.7 = 216.7).
    // P13, reduced-budget: 4 frames + 3 pins, luma only (the received chroma stays on the coarse grid: plan 99 s4 laptop line).
    // The reduced figure is a SIZING CHOICE of this declaration (A3 gives none): flagged for the lead in the hand-back.
    // TD roles: the plan 99 s4 :282 inventory mapped to the six A3 roles; the sum is 439.1 MB, inside the 450 MB reserve.
    static const std::vector<ReserveRow> rows = {
        { ReserveTier::FullBudget, kP13SourceRing, "keyed source ring", 8, kMaxRingPins,
          (8u + kMaxRingPins) * (kReserveLumaBytesPerFrame4K + kReserveChromaBytesPerFrame4K), Catalog::FlagKind::DeclaredNotAllocated,
          "A3 item 2: 8 frames + 3 pins, luma 19.7 MB + chroma 8.3 MB per frame at 4K" },
        { ReserveTier::ReducedBudget, kP13SourceRing, "keyed source ring", 4, kMaxRingPins,
          (4u + kMaxRingPins) * kReserveLumaBytesPerFrame4K, Catalog::FlagKind::DeclaredNotAllocated,
          "reduced tier: 4 frames + 3 pins, luma only (sizing choice, lead to confirm)" },

        { ReserveTier::FullBudget, kTdMemory, "recent ring", 0, 0, 5u * (kReserveLumaBytesPerFrame4K + kReserveChromaBytesPerFrame4K),
          Catalog::FlagKind::DeclaredNotAllocated, "plan 99 s4: RECENT ring 5 x (19.7 + 8.3) MB" },
        { ReserveTier::FullBudget, kTdAnchorRefs, "peak pins", 0, 0, 145000000, Catalog::FlagKind::DeclaredNotAllocated,
          "plan 99 s4: CAND 2 x 28.0 + LT-A 56.0 + 4:4:4 accumulation canvas 33.0 MB" },
        { ReserveTier::FullBudget, kTdDelta, "anchor delta", 0, 0, 94200000, Catalog::FlagKind::DeclaredNotAllocated,
          "plan 99 s4: td.delta ring 6 x 11.5 + chroma atlas 6 x 4.2 MB" },
        { ReserveTier::FullBudget, kTdAnchorPass, "prediction plane", 0, 0, 34100000, Catalog::FlagKind::DeclaredNotAllocated,
          "plan 99 s4: LT-P 6.2 + 0.5 + ST 2 x 11.2 + tiles 2 x 2.5 MB" },
        { ReserveTier::FullBudget, kTdBlocks, "records", 0, 0, 6000000, Catalog::FlagKind::DeclaredNotAllocated,
          "plan 99 s4: td.blocks 2.4 + NA 3.6 MB" },
        { ReserveTier::FullBudget, kTdMaps, "maps", 0, 0, 19800000, Catalog::FlagKind::DeclaredNotAllocated,
          "plan 99 s4: td.maps 4.7 + P3b 1.6 + P3r 6.2 + P16 3.1 + CfL (a, b) 4.2 MB" },

        { ReserveTier::ReducedBudget, kTdMemory, "all roles (coarse / one-reference)", 0, 0, 85000000, Catalog::FlagKind::DeclaredNotAllocated,
          "plan 99 s4 laptop line: about 85 MB at native 4K, chroma on the coarse grid" },
    };
    return rows;
}

// Sum of the TD role rows of a tier (P13 rows excluded), at the planning geometry.
inline uint64_t ReserveTdBytes4K(ReserveTier tier)
{
    uint64_t sum = 0;
    for (const ReserveRow& r : ReserveRows())
        if (r.tier == tier && r.product != kP13SourceRing)
            sum += r.bytes4K;
    return sum;
}

// The P13 row of a tier (never null: both tiers are declared).
inline const ReserveRow* ReserveRingRow(ReserveTier tier)
{
    for (const ReserveRow& r : ReserveRows())
        if (r.tier == tier && r.product == kP13SourceRing)
            return &r;
    return nullptr;
}

// The P13 window the graph derives: BACK = the P13 node's largest reader lag (Node::historyFull), LEAD = the sealed laneLead.
// Returns false (and leaves *frames alone) when the graph has no P13 node (no active reader): no window, nothing reserved.
inline bool DerivedRingWindow(const Graph& g, const GraphSettings& gs, uint32_t* frames)
{
    const Node* n = g.Find(kP13SourceRing);
    if (n == nullptr)
        return false;
    *frames = RingWindowFrames(n->historyFull, gs.laneLead);
    return true;
}

// The window the reserve of a tier can hold: min(derived window, reserve frames). `*capped` is set when the derived window is larger
// (the TD round must then shed visibly; this header only reports it).
inline uint32_t ClampWindowToReserve(uint32_t derivedWindow, ReserveTier tier, bool* capped)
{
    const ReserveRow* r = ReserveRingRow(tier);
    const uint32_t cap = r != nullptr ? r->ringFrames : 0u;
    const bool over = derivedWindow > cap;
    if (capped != nullptr)
        *capped = over;
    return over ? cap : derivedWindow;
}

// The status lines (Ctrl+I block of the skeleton, with the other "Skeleton ..." lines): one per tier, "reserved, not allocated".
// `d` = the source size the bytes are scaled to (0 = the 4K planning size, said in the line).
inline std::vector<std::string> ReserveStatusLines(const Dims& d)
{
    std::vector<std::string> out;
    const bool planning = d.width == 0 || d.height == 0;
    const std::string at = planning ? std::string("at the 4K planning size")
                                    : std::string("at ") + std::to_string(d.width) + "x" + std::to_string(d.height);
    for (ReserveTier tier : { ReserveTier::FullBudget, ReserveTier::ReducedBudget }) {
        const ReserveRow* ring = ReserveRingRow(tier);
        uint64_t td = 0;
        size_t roles = 0;
        for (const ReserveRow& r : ReserveRows())
            if (r.tier == tier && r.product != kP13SourceRing) {
                td += ReserveRowBytes(r, d);
                ++roles;
            }
        std::string line = std::string("Skeleton reserve, ") + ReserveTierName(tier) + " (" + Catalog::Token(Catalog::FlagKind::DeclaredNotAllocated) + ", " + at + "): ";
        if (ring != nullptr)
            line += "source ring up to " + std::to_string(unsigned(ring->ringFrames)) + " frames + " + std::to_string(unsigned(ring->pins)) +
                    " pins " + Catalog::MegaBytes(ReserveRowBytes(*ring, d)) + " MB; ";
        line += "temporal-detail store " + Catalog::MegaBytes(td) + " MB in " + std::to_string(roles) + (roles == 1 ? " role" : " roles");
        out.push_back(line);
    }
    return out;
}

}   // namespace VpSkeleton
