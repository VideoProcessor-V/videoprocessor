// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "AnalysisLane.h"
#include "SkeletonAcquire.h"

#include <functional>
#include <memory>
#include <vector>

// B1 (refs/126 s1.1-s1.2): the existing seal-time live fill generalised onto Acquire (one law, not a second one).
//   - the lane's slots are G2's first source: a sealed / live-filled frame of a lane product is REUSED (the same bytes the
//     seal made, bit-identical by construction: the pointer is the slot's; the result pins the lane state);
//   - a GPU lane product is reused through its dispatcher texture (`gpuHandleOf`: the product's texture set, ring / live
//     slot and the producer fence to wait on); no reachable texture = Unavailable(NoGpuHandle), visible;
//   - the lane's registered CPU live computes (P1, P4 ...) are registered as the OWNER FILLS of their products under the
//     catalog's owning unit (ProductRows: P1 / P4 = input/source_analysis/lane): an Acquire of a frame the lane holds
//     nowhere runs AnalysisLane::RunCpuLive (the seal's own CPU law) and publishes it in the memo, its inputs
//     got-or-computed through the same Acquire chain;
//   - the seal reuses such a publication when no input is missing (AnalysisLane::SetPublishedLookup), never computing twice;
//   - GPU lane products: Pending / Failed deferred live fills are visible causes; with no GPU dispatcher (the SkeletonModule.cpp
//     no-dispatcher wiring that DECLINES every GPU live fill) a GPU form is Unavailable(NoGpuDispatcher), counted and on the
//     Fallbacks list, instead of a consumer's private path.
// Render thread (the lane's slots are read on it). Bind after the lane's live computes are registered; a re-bind replaces
// the lane's own fills explicitly (ReplaceOwnerFill).
namespace VpSkeleton {

// (product, ring slot?, index, producer fence) -> the dispatcher's texture of that lane slot. False = not reachable.
using LaneGpuHandleOf = std::function<bool(ProductId, bool, int, uint64_t, GpuMemoHandle*)>;

inline void BindLaneToMemo(AnalysisLane& lane, SkeletonMemo& memo, LaneGpuHandleOf gpuHandleOf = LaneGpuHandleOf())
{
    AnalysisLane* l = &lane;
    LaneSlotSource src;
    src.read = [l, gpuHandleOf](ProductId id, uint64_t seq, AcquireForm form, LaneSlotSource::Read* out) {
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
                    return LaneSlotSource::State::Absent;   // a GPU product asked in CPU form: the owner publishes that form
                out->cpu = r.cpu;
                out->cpuBytes = r.cpuBytes;
            } else {
                if (b == Backend::Cpu)
                    return LaneSlotSource::State::Absent;   // a CPU product asked in GPU form: the owner publishes that form
                if (!gpuHandleOf || !gpuHandleOf(id, r.state == AnalysisLane::ProductRead::Ring, r.info.index, r.info.waitValue, &out->gpu)
                    || !out->gpu.Valid())
                    return LaneSlotSource::State::NoHandle;
            }
            out->pin = std::shared_ptr<const void>(l->State());   // the lane state (its buffers) outlives the read
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
        f.fill = [l](const OwnerFillRequest& req, SkeletonMemo& m, OwnerFillOutput& out) {
            const size_t n = l->CpuBytesOf(req.id);
            if (n == 0)
                return false;
            out.bytes.assign(n, 0);
            return l->ComputeCpuOwnerForm(req.id, req.seq, out.bytes.data(), n,
                                          [&m](ProductId in, uint64_t s, size_t bytes) -> const uint8_t* {
                                              // The input is pinned by the memo / lane for the frame: the owner fill reads it now.
                                              const AcquireResult r = m.Acquire(in, s, AcquireForm::Cpu);
                                              return r.Ok() && r.cpuBytes == bytes ? r.cpu : nullptr;
                                          });
        };
        memo.ReplaceOwnerFill(id, AcquireForm::Cpu, SkeletonMemo::OwnerUnitOf(id), std::move(f));
    }

    SkeletonMemo* m = &memo;
    lane.SetPublishedLookup([m](ProductId id, uint64_t seq, uint8_t* dst, size_t n) { return m->CopyPublished(id, seq, dst, n); });
}

}  // namespace VpSkeleton
