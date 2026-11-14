// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "AnalysisLane.h"
#include "SkeletonAcquire.h"

#include <functional>
#include <map>
#include <memory>
#include <utility>
#include <vector>

// B1 (refs/126 s1.1-s1.2): the existing seal-time live fill generalised onto Acquire (one law, not a second one).
//   - the lane's slots are G2's first source: a sealed / live-filled frame of a lane product is REUSED (the same bytes the
//     seal made, bit-identical by construction: the pointer is the slot's; the result pins the lane state, i.e. the BUFFERS:
//     a ring slot's CONTENT is valid while the frame is in the ring, which on the render thread is the frame's lifetime);
//   - a GPU lane product is reused through its dispatcher texture (`gpuHandleOf`: the product's texture set, ring / live
//     slot and the producer fence to wait on); no reachable texture = Unavailable(NoGpuHandle), visible;
//   - the lane's registered CPU live computes (P1, P4 ...) are bound as the OWNER FILLS of their products under the
//     catalog's owning unit (ProductRows: P1 / P4 = input/source_analysis/lane): an Acquire of a frame the lane holds
//     nowhere runs AnalysisLane::RunCpuLive (the seal's own CPU law) and publishes it in the memo, its inputs
//     got-or-computed through the same Acquire chain (the input pins are HELD until the fill returns: G0 audit S3);
//   - the seal reuses such a publication when no input is missing (AnalysisLane::SetPublishedLookup), never computing twice;
//   - GPU lane products: Pending / Failed deferred live fills are visible causes; with no GPU dispatcher (the SkeletonModule.cpp
//     no-dispatcher wiring that DECLINES every GPU live fill) a GPU form is Unavailable(NoGpuDispatcher), counted and on the
//     Fallbacks list, instead of a consumer's private path.
// L-CORE (lead8/iface-L-CORE.md s3, s5): the binding is SkeletonMemo::BindLane with the lane as token, so the memo outlives
// the lane (it is the renderer's, held by the skeleton module) and UnbindLaneFromMemo at the bundle's end drops exactly this
// binding. The lane fills are render-thread-bound (they read the lane's render state); off the render thread a lane CPU
// product is served from its PUBLISHED bytes through the lane lease (`leaseOf`, any thread: AnalysisLane::Lease is an
// atomic load; the caller keeps the lane alive across the call), never recomputed.
namespace VpSkeleton {

// (product, ring slot?, index, producer fence) -> the dispatcher's texture of that lane slot. False = not reachable.
using LaneGpuHandleOf = std::function<bool(ProductId, bool, int, uint64_t, GpuMemoHandle*)>;
// The lane's CURRENT lease of a product's bytes ring, from any thread (empty = no lane / not in the graph).
using LaneLeaseOf = std::function<LaneLease(ProductId)>;

inline void BindLaneToMemo(AnalysisLane& lane, SkeletonMemo& memo, LaneGpuHandleOf gpuHandleOf = LaneGpuHandleOf(),
                           LaneLeaseOf leaseOf = LaneLeaseOf())
{
    AnalysisLane* l = &lane;
    LaneSlotSource src;
    src.token = l;
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
    const LaneLeaseOf leases = leaseOf ? leaseOf : LaneLeaseOf([l](ProductId id) { return l->Lease(id); });
    src.copyPublished = [leases](ProductId id, uint64_t seq, std::vector<uint8_t>* copy) {
        const LaneLease lease = leases(id);
        if (!lease.Valid() || lease.table->Find(id) == nullptr)
            return LaneSlotSource::State::NotLaneProduct;
        if (lease.bytes == nullptr || copy == nullptr)
            return LaneSlotSource::State::Absent;   // not a CPU product of the lane
        const PublishedRead pr = ReadPublishedBytes(lease, id, seq, lease.bytes->Bytes(), [copy](const uint8_t* bytes, size_t n) {
            copy->assign(bytes, bytes + n);
        });
        if (pr != PublishedRead::Copied) {
            copy->clear();
            return LaneSlotSource::State::Absent;
        }
        return LaneSlotSource::State::Present;
    };

    std::map<std::pair<ProductId, AcquireForm>, OwnerFill> fills;
    for (ProductId id : lane.CpuLiveProducts()) {
        OwnerFill f;
        f.renderThreadOnly = true;   // RunCpuLive reads the lane's render state
        f.fill = [l](const OwnerFillRequest& req, SkeletonMemo& m, OwnerFillOutput& out) {
            const size_t n = l->CpuBytesOf(req.id);
            if (n == 0)
                return false;
            out.bytes.assign(n, 0);
            std::vector<AcquireResult> held;   // the inputs' pins live until this fill returns (G0 audit S3)
            return l->ComputeCpuOwnerForm(req.id, req.seq, out.bytes.data(), n,
                                          [&m, &held](ProductId in, uint64_t s, size_t bytes) -> const uint8_t* {
                                              AcquireResult r = m.Acquire(in, s, AcquireForm::Cpu);
                                              if (!r.Ok() || r.cpuBytes != bytes)
                                                  return nullptr;
                                              const uint8_t* p = r.cpu;
                                              held.push_back(std::move(r));
                                              return p;
                                          });
        };
        fills[std::make_pair(id, AcquireForm::Cpu)] = std::move(f);
    }
    memo.BindLane(std::move(src), std::move(fills));
    memo.SetGpuDispatcherPresent(lane.HasGpuDispatcher());

    // L-CORE M5 (coordinator ruling on B1-SEAL-VIA-ACQUIRE): the seal (1) REUSES a frame an Acquire reader published (copy, counted
    // sealReused) and (2) when it computes, its result IS the publication: the lane slot is the memo's first source for that frame
    // (counted sealComputed), so Acquire after the seal reuses it and the frame is computed once (AcquireCounters::Computed()).
    // The lookup is asked for a seal with every input present; a missing-input seal computes another result (its own form).
    SkeletonMemo* m = &memo;
    lane.SetPublishedLookup([m](ProductId id, uint64_t seq, uint8_t* dst, size_t n) {
        if (m->CopyPublished(id, seq, dst, n)) {
            m->NoteSealReused(id);
            return true;
        }
        m->NoteSealComputed(id);
        return false;
    });
}

// The lane's end (the bundle that owns it goes): drops exactly this lane's binding (a newer bound lane is kept).
inline bool UnbindLaneFromMemo(AnalysisLane& lane, SkeletonMemo& memo)
{
    lane.SetPublishedLookup(AnalysisLane::PublishedLookup());
    const bool unbound = memo.UnbindLane(&lane);
    if (unbound)
        memo.SetGpuDispatcherPresent(false);
    return unbound;
}

}  // namespace VpSkeleton
