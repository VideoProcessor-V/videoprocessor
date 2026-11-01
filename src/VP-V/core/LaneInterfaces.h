// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "ProductSlots.h"

#include <cstddef>
#include <cstdint>

// Skeleton S3-S7 contracts between the analysis lane (LaneRunner.h), the render thread's idle-gap
// scheduler (GapScheduler.h), the product producers (modules/input/source_analysis/lane/) and the host
// hooks the renderer implements later through the M2 hook layer (AnalysisLane.h lists them). Pure
// C++14: the host owns every device call; these interfaces only describe them.
namespace VpSkeleton {

// A source frame as the host hands it to the lane (capture/conversion thread). The host keeps the
// planes valid until OnFrameRetired(seq) or OnFrameDropped(seq). Luma samples are 16-bit; a sample
// scaled to the full 16-bit range is (raw << msbShift) (P010/P016: msbShift 0).
struct SourceFrameView {
    uint64_t seq = 0;
    const uint16_t* luma = nullptr;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t strideSamples = 0;   // samples per row (>= width)
    uint8_t msbShift = 0;
    bool Valid() const { return luma != nullptr && width > 0 && height > 0 && strideSamples >= width; }
};

enum class ChunkResult : uint8_t { More, Done, Failed };

// One lane job: product `node->id` for frame `seq`, writing ring slot `ringSlot`.
struct LaneJobContext {
    const Node* node = nullptr;
    uint64_t seq = 0;
    LaneTicket ticket;
    int ringSlot = -1;
    const LaneInputs* inputs = nullptr;                 // pinned, positional (= node->inputs)
    std::array<const uint8_t*, kMaxInputs> inputData{}; // CPU products: input k's bytes (ring or live buffer); else null
    const SourceFrameView* source = nullptr;            // when the product reads the source frame
    uint8_t* out = nullptr;                             // CPU products: the ring slot's bytes; else null
    size_t outBytes = 0;
    Header* header = nullptr;                           // the lane header: a producer may set costUs only
};

// A lane producer for one product. All calls for one product come from ONE thread (the lane side of
// its SlotSet): the OFA worker, the CPU worker, or the render thread for GPU-compute products.
class ILaneProducer {
public:
    virtual ~ILaneProducer() {}
    virtual ProductId Product() const = 0;
    // Once per job, after the claim, Writable and the input pins. False: cannot run this frame (the
    // runner abandons the job and counts it).
    virtual bool Begin(const LaneJobContext& job) = 0;
    // One bounded chunk of about budgetUs. Deterministic: the finished result never depends on how
    // the work was chunked.
    virtual ChunkResult RunChunk(const LaneJobContext& job, uint32_t budgetUs) = 0;
    // Producer-device fence: the value signalled after this job's last write, and the polled completed
    // value. CPU and render-device products: 0 and UINT64_MAX.
    virtual uint64_t ProducerSignal() const { return 0; }
    virtual uint64_t ProducerCompleted() const { return ~uint64_t(0); }
    // The seal took the frame between chunks: stop submitting (device work already submitted lands;
    // the runner polls the fence before returning the buffer).
    virtual void Cancel(const LaneJobContext& job) { (void)job; }
};

// Work that shares the render thread's idle gap, in fixed class priority (design B6).
enum class GapClass : uint8_t { RenderAheadExtra = 0, EarlyUploadWrap = 1, LaneCompute = 2 };

class IGapWork {
public:
    virtual ~IGapWork() {}
    virtual GapClass Class() const = 0;
    virtual uint16_t Id() const = 0;                   // stable: deterministic order and traces
    virtual Tier WorkTier() const { return Tier::T0; }
    virtual double HeavyMs() const { return 0.0; }     // whole-job estimate: heavier first within a tier
    virtual bool HasWork() const = 0;
    virtual uint32_t PredictUs() const = 0;            // the next chunk's predicted cost
    virtual void RunChunk(uint32_t budgetUs) = 0;      // render thread, one chunk
};

// Render-device work of GPU-compute products (host hook, render thread only). Lane GPU-compute jobs
// run on the render device in the idle gap, so the seal and the dispatch are ordered on one thread:
// no fences (design B4).
class IGpuDispatcher {
public:
    virtual ~IGpuDispatcher() {}
    // Record chunk `chunk` of `chunkCount` of job.node->id for frame job.seq into ring slot
    // job.ringSlot. False: failed (the job is abandoned, counted and shown).
    virtual bool Dispatch(const LaneJobContext& job, uint32_t chunk, uint32_t chunkCount) = 0;
    // The in-render (live) form: the same shader, parameters and inputs, whole product at once.
    virtual bool DispatchLive(const Node& node, uint64_t seq, int liveIndex, const Header& hdr,
                              uint32_t missingInputs) = 0;
};

// Asynchronous readback of a GPU product into system memory (host hook, render thread only): copy
// into a staging buffer, then poll an event query. Never a blocking Map on the render context.
class IReadbackDevice {
public:
    virtual ~IReadbackDevice() {}
    virtual bool CopyToStaging(ProductId product, uint64_t seq, int stagingIndex) = 0;
    virtual bool QueryDone(int stagingIndex) = 0;                // non-blocking
    virtual const void* MapNoWait(int stagingIndex, size_t* bytes) = 0;  // null when busy (DO_NOT_WAIT)
    virtual void Unmap(int stagingIndex) = 0;
};

} // namespace VpSkeleton
