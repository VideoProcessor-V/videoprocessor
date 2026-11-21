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

// GF-P1 (lead ruling; owner OD 2026-11-14: ahead cap 6, derived from active consumers): the ahead depth of the fill of `id` =
// the larger of the fill's own structural need and the largest ConsumerDecl::aheadDepth of the consumers that read `id`, capped at
// kAheadCeiling. `asked` / `by` = the uncapped request and its requester (null = the structural need or nobody), kept for the
// window's existing ceiling notice ("ahead compute 6 (by X; ceiling 6)"); `capped` = asked > kAheadCeiling. 0 with no request.
struct LaneAheadDepth {
    uint16_t depth = 0;
    uint16_t asked = 0;
    const char* by = nullptr;
    bool capped = false;
};
inline LaneAheadDepth LaneAheadDepthOf(ProductId id, uint16_t structural, const std::vector<ConsumerDecl>& consumers)
{
    LaneAheadDepth r;
    r.asked = structural;
    for (const ConsumerDecl& c : consumers) {
        if (c.aheadDepth <= r.asked)
            continue;
        bool reads = false;
        for (size_t k = 0; k < c.count && k < kMaxConsumes; ++k)
            reads = reads || c.consumes[k].product == id;
        if (reads) {
            r.asked = c.aheadDepth;
            r.by = c.name;
        }
    }
    r.capped = r.asked > kAheadCeiling;
    r.depth = r.capped ? kAheadCeiling : r.asked;
    return r;
}

// `consumers` (GF-P1): the lane's configured consumers, external demand included (SkeletonModule's Configure list); their
// declared ahead depths set each lane fill's aheadDepth (LaneAheadDepthOf). Rebind after a Configure whose consumers changed.
inline void BindLaneToMemo(AnalysisLane& lane, SkeletonMemo& memo, LaneGpuHandleOf gpuHandleOf = LaneGpuHandleOf(),
                           LaneLeaseOf leaseOf = LaneLeaseOf(), const std::vector<ConsumerDecl>& consumers = std::vector<ConsumerDecl>())
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
    // QKP slice C (R14-73): the memo's Acquire asks the lane to make a frame ahead of the render frame NOW (the lane's own claim: one
    // copy; the worker skips it). AnalysisLane::AheadRun maps 1:1.
    src.aheadNow = [l](ProductId id, uint64_t seq) {
        const AnalysisLane::AheadRunResult r = l->RunAheadNow(id, seq);
        LaneSlotSource::AheadNowResult out;
        out.why = r.why;
        out.ownerMayFill = r.ownerMayFill;   // C5-SKREDS: a decline the owner fill may still answer (the memo's own spot)
        switch (r.kind) {
        case AnalysisLane::AheadRun::Ran: out.kind = LaneSlotSource::AheadNow::Ran; break;
        case AnalysisLane::AheadRun::Claimed: out.kind = LaneSlotSource::AheadNow::Claimed; break;
        case AnalysisLane::AheadRun::NotNode: out.kind = LaneSlotSource::AheadNow::NotNode; break;
        case AnalysisLane::AheadRun::Declined: out.kind = LaneSlotSource::AheadNow::Declined; break;
        }
        return out;
    };
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
        // The lane's live compute reads nothing ahead itself (structural 0): its depth is its consumers' declared request.
        const LaneAheadDepth ad = LaneAheadDepthOf(id, 0, consumers);
        f.aheadDepth = ad.depth;
        f.aheadAsked = ad.capped ? ad.asked : uint16_t(0);
        f.aheadBy = ad.by;
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
    // GF-P1 MUST-3 (GV AMEND, binder side): binding never clears the owner-device presence. A shadow or raised lane makes no lane
    // GPU dispatcher (SkeletonModule UpdateDispatcher), yet the memo's attached OwnerGpuDevice still serves the non-view GPU owner
    // fills (P1, edge map, grain split variant); PlanNoLaneGpu skips a lane-bound memo, so nothing would restore it. The gate in
    // Acquire is unchanged; SkeletonModule's AttachMemoGpu / DetachMemoGpu re-assert presence while a lane is bound (P2b).
    memo.SetGpuDispatcherPresent(lane.HasGpuDispatcher() || memo.HasGpuDevice());

    // L-CORE M5 (coordinator ruling on B1-SEAL-VIA-ACQUIRE): the seal (1) REUSES a frame an Acquire reader published (copy, counted
    // sealReused) and (2) when it computes, its result IS the publication: the lane slot is the memo's first source for that frame
    // (counted sealComputed), so Acquire after the seal reuses it and the frame is computed once (AcquireCounters::Computed()).
    // The lookup is asked for a seal with every input present; a missing-input seal computes another result (its own form).
    SkeletonMemo* m = &memo;
    lane.SetAheadSinks([m](ProductId id) { m->NoteSealComputedReduced(id); }, [m](ProductId id) { m->NoteWorkerReused(id); });
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
    lane.SetAheadSinks(nullptr, nullptr);
    const bool unbound = memo.UnbindLane(&lane);
    if (unbound)
        memo.SetGpuDispatcherPresent(false);
    return unbound;
}

}  // namespace VpSkeleton
