// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "AnalysisLane.h"
#include "SkeletonAcquire.h"

#include <vector>

// B1 (refs/126 s1.1-s1.2): the existing seal-time live fill generalised onto Acquire (one law, not a second one).
//   - the lane's slots are G2's first source: a sealed / live-filled frame of a lane product is REUSED (the same bytes the
//     seal made, bit-identical by construction: the pointer is the slot's);
//   - the lane's registered CPU live computes (P1, P4 ...) are registered as the OWNER FILLS of their products: an Acquire
//     of a frame the lane holds nowhere runs AnalysisLane::RunCpuLive (the seal's own CPU law) and publishes it in the
//     memo, its inputs got-or-computed through the same Acquire chain;
//   - the seal reuses such a publication (AnalysisLane::SetPublishedLookup): copied into the slot, never computed twice;
//   - GPU lane products: Pending / Failed deferred live fills are visible causes; with no GPU dispatcher (the
//     SkeletonModule.cpp no-dispatcher wiring that DECLINES every GPU live fill) an Acquire of a GPU form is
//     Unavailable(NoGpuDispatcher), counted and on the Fallbacks heading, instead of a consumer's private path.
// Render thread (the lane's slots are read on it). Bind after the lane's live computes are registered.
namespace VpSkeleton {

constexpr const char* kLaneOwnerUnit = "control/skeleton";   // the lane's own CPU products are owned by the skeleton unit

inline void BindLaneToMemo(AnalysisLane& lane, SkeletonMemo& memo)
{
    AnalysisLane* l = &lane;
    LaneSlotSource src;
    src.read = [l](ProductId id, uint64_t seq, AcquireForm form, const uint8_t** cpu, size_t* n) {
        Backend b = Backend::Cpu;
        if (!l->IsNode(id, &b))
            return LaneSlotSource::State::NotLaneProduct;
        const AnalysisLane::ProductRead r = l->ReadOf(id, seq);
        switch (r.state) {
        case AnalysisLane::ProductRead::Pending: return LaneSlotSource::State::Pending;
        case AnalysisLane::ProductRead::Failed: return LaneSlotSource::State::Failed;
        case AnalysisLane::ProductRead::Ring:
        case AnalysisLane::ProductRead::Live:
            if (form == AcquireForm::Cpu) {
                if (r.cpu == nullptr)
                    return LaneSlotSource::State::Absent;   // a GPU product asked in CPU form: not the lane's form
                *cpu = r.cpu;
                *n = r.cpuBytes;
            } else if (b == Backend::Cpu) {
                return LaneSlotSource::State::Absent;       // a CPU product asked in GPU form: the owner publishes that form
            }
            return LaneSlotSource::State::Present;
        case AnalysisLane::ProductRead::None: break;
        }
        return LaneSlotSource::State::Absent;
    };
    src.dropped = [l](ProductId id) { return l->IsNode(id) && l->RegulatorDropped(id); };
    memo.SetLaneSource(std::move(src));
    memo.SetGpuDispatcherPresent(lane.HasGpuDispatcher());

    for (ProductId id : lane.CpuLiveProducts()) {
        OwnerFill f;
        f.owner = kLaneOwnerUnit;
        f.cpu = [l](const OwnerFillRequest& req, SkeletonMemo& m, std::vector<uint8_t>& out) {
            const size_t n = l->CpuBytesOf(req.id);
            if (n == 0)
                return false;
            out.assign(n, 0);
            return l->ComputeCpuOwnerForm(req.id, req.seq, out.data(), n,
                                          [&m](ProductId in, uint64_t s, size_t bytes) -> const uint8_t* {
                                              const AcquireResult r = m.Acquire(in, s, AcquireForm::Cpu);
                                              return r.Ok() && r.cpuBytes == bytes ? r.cpu : nullptr;
                                          });
        };
        memo.RegisterOwnerFill(id, std::move(f));
    }

    SkeletonMemo* m = &memo;
    lane.SetPublishedLookup([m](ProductId id, uint64_t seq, uint8_t* dst, size_t n) { return m->CopyPublished(id, seq, dst, n); });
}

}  // namespace VpSkeleton
