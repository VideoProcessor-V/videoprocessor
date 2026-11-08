// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// Lane lease: how an OUTSIDE worker (the camera fit thread) holds the analysis lane's per-graph objects safely.
//
// AnalysisLane::Configure swaps its LaneState (graph, SlotTable, CpuBuffers, Indicator hold) on every rebuild (external-demand
// change, vramReplan, queueMoved, the period). A raw SlotTable* / CpuBuffers* / Indicator* kept by a thread across that swap is
// a use-after-free. A LaneLease is the safe form:
//  * `hold` is a shared_ptr to the owning LaneState (type-erased: no dependency on AnalysisLane.h), so the table, the buffers and
//    the Indicator stay alive for as long as the lease is held, however many rebuilds happen meanwhile;
//  * `generation` is the LaneState generation (AnalysisLane::Generation(), 1,2,3... one per built state): a holder compares it
//    with the lane's current generation and REBINDS (takes a new lease) when it changed. Work stamped with an older generation is
//    REFUSED and counted, never posted into the new graph's slots (its seq belongs to a graph that no longer exists).
// A lease from AnalysisLane::Lease() is Owned() and generation >= 1. LaneLease::Borrow() builds an unowned generation-0 lease from
// raw pointers for tests and standalone drivers whose objects outlive the holder by construction; it can never go stale.
// Pure, header-only, C++14.
#pragma once

#include "Indicator.h"
#include "ProductSlots.h"

#include <cstdint>
#include <functional>
#include <memory>

namespace VpSkeleton {

struct LaneLease {
    std::shared_ptr<const void> hold;       // the owning LaneState; null = a borrowed lease (tests / standalone)
    SlotTable* table = nullptr;
    CpuBuffers* bytes = nullptr;            // the bytes ring of one CPU product (the caller names it); may be null
    VpIndicator::Indicator* indicator = nullptr;   // may be null (the holder then uses the stamp it was given)
    uint64_t generation = 0;                // 0 = borrowed

    bool Valid() const { return table != nullptr; }
    bool Owned() const { return bool(hold); }

    static LaneLease Borrow(SlotTable* t, CpuBuffers* b, VpIndicator::Indicator* ind) {
        LaneLease l;
        l.table = t;
        l.bytes = b;
        l.indicator = ind;
        return l;
    }
};

// Re-acquires the lane's CURRENT lease (empty / invalid when the lane is inactive or the graph is invalid).
// Called off the render thread too: it must be cheap and thread-safe (AnalysisLane::Lease is an atomic_load).
using LaneLeaseSource = std::function<LaneLease()>;

}  // namespace VpSkeleton
