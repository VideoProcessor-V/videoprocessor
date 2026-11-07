// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <VP-V/core/LaneInterfaces.h>

#include <cmath>
#include <cstdint>
#include <functional>
#include <utility>
#include <vector>

// Skeleton S6: the generic lane job of the GPU-compute products (P3, P5, P6). The job runs on the
// render device in the idle gap: the host's IGpuDispatcher records each chunk; the seal and the
// dispatch are ordered on one thread, so there is no fence (ProducerSignal 0). Pure C++14.
namespace VpLaneProducts {

using VpSkeleton::ChunkResult;
using VpSkeleton::Dims;
using VpSkeleton::IGpuDispatcher;
using VpSkeleton::ILaneProducer;
using VpSkeleton::LaneJobContext;
using VpSkeleton::ProductId;

struct ChunkPlan {
    uint32_t chunkCount = 1;
    uint32_t rowsPerChunk = 0;
    uint32_t rows = 0;
};

// Enough chunks that each is predicted <= the cap: at least 1, at most `rows`, and no empty chunk.
inline ChunkPlan PlanChunks(uint32_t rows, double predictedMs, uint32_t chunkCapUs)
{
    ChunkPlan p;
    p.rows = rows;
    if (rows == 0) {
        p.chunkCount = 1;
        p.rowsPerChunk = 0;
        return p;
    }
    const double predictedUs = predictedMs > 0.0 ? predictedMs * 1000.0 : 0.0;
    double want = 1.0;
    if (predictedUs > 0.0)
        want = chunkCapUs == 0 ? double(rows) : predictedUs / double(chunkCapUs);   // cap 0: nothing fits, finest split
    uint64_t count = want <= 1.0 ? 1u : uint64_t(want);
    if (double(count) < want)
        ++count;
    if (count < 1u)
        count = 1u;
    if (count > rows)
        count = rows;
    const uint32_t rpc = uint32_t((uint64_t(rows) + count - 1u) / count);
    p.rowsPerChunk = rpc;
    p.chunkCount = uint32_t((uint64_t(rows) + rpc - 1u) / rpc);
    return p;
}

// Rows of the product's block grid: P3 per 8x8 block row, P5 per 64x64 tile row, P6 per source row.
inline uint32_t RowsFor(ProductId id, Dims d)
{
    switch (id) {
    case VpSkeleton::kP3AnalysisField: return (d.height + 7u) / 8u;
    case VpSkeleton::kP5MonitorTiles: return (d.height + 63u) / 64u;
    case VpSkeleton::kP6EntranceAhead: return d.height;
    default: return 0;
    }
}

// 70b G0/G1: the lane form of the VP-owned GPU products (P10, P11, P12) is a FIXED sequence of passes, one chunk
// per pass (never split by the budget): the same dispatches in the same order as the live form, so lane == live by
// construction. The common input is the tap plane (the lane SourceFrameView luma as R16 unorm, code << 6), uploaded
// in row bands of at most kTapBandBytes each.
constexpr uint32_t kTapBandBytes = 4u << 20;
constexpr uint32_t kP10SplitterSteps = 5;   // GH, GV, QUIET, NMIN, F (GrainSplitGpu.h)

inline uint32_t TapRowsPerBand(Dims d)
{
    const uint64_t rowBytes = uint64_t(d.width) * 2u;
    if (rowBytes == 0)
        return 0;
    const uint64_t rows = uint64_t(kTapBandBytes) / rowBytes;
    return rows == 0 ? 1u : uint32_t(rows > d.height ? d.height : rows);
}

inline uint32_t TapBands(Dims d)
{
    const uint32_t rpb = TapRowsPerBand(d);
    return rpb == 0 || d.height == 0 ? 0u : (d.height + rpb - 1u) / rpb;
}

// Steps (chunks) of a fixed-step product: P10 = tap bands + extend + the splitter passes; P11 and P12 one pass each
// (the tap and extend are the P10 job's, or made inline by their one chunk when absent). 0 = not fixed-step.
inline uint32_t StepsFor(ProductId id, Dims d)
{
    if (d.width == 0 || d.height == 0)
        return 0;
    switch (id) {
    case VpSkeleton::kP10GrainSplit: return TapBands(d) + 1u + kP10SplitterSteps;
    case VpSkeleton::kP11RegionBlockStats: return 1;
    case VpSkeleton::kP12LightLevel: return 1;
    default: return 0;
    }
}

// 70b §3 / doc 70 §6.2: until the source ring (P13, M2 P10) exists, P10 and P11 are computed from the TAP plane (the
// lane's analysis luma), not from the queued source planes: the registry the lane is built on drops their P13 input.
// The tap form has its own unit version and provenance tag (kTapFormUnitVersion, kTapProvenance); at the migration to
// P13 both change and consumers CheckFit and fall back visibly (WrongSize / Stale). Every other declaration is kept.
constexpr uint32_t kTapFormUnitVersion = 1;
constexpr const char* kTapProvenance = "tap: analysis luma (10-bit studio codes << 6, R16 unorm)";

inline VpSkeleton::Registry TapFormProducts(const VpSkeleton::Registry& reg)
{
    std::vector<VpSkeleton::ProductDecl> d = reg.All();
    for (VpSkeleton::ProductDecl& p : d) {
        if (p.id != VpSkeleton::kP10GrainSplit && p.id != VpSkeleton::kP11RegionBlockStats)
            continue;
        uint8_t w = 0;
        for (uint8_t k = 0; k < p.inputCount; ++k)
            if (p.inputs[k].product != VpSkeleton::kP13SourceRing)
                p.inputs[w++] = p.inputs[k];
        for (uint8_t k = w; k < p.inputCount; ++k)
            p.inputs[k] = VpSkeleton::InputRef();
        p.inputCount = w;
    }
    return VpSkeleton::Registry(std::move(d));
}

class GpuComputeLaneJob : public ILaneProducer {
public:
    GpuComputeLaneJob(ProductId id, IGpuDispatcher* dispatcher, std::function<double()> predictedMs,
                      std::function<uint32_t()> chunkCapUs, uint32_t rows)
        : id_(id)
        , dispatcher_(dispatcher)
        , predictedMs_(std::move(predictedMs))
        , chunkCapUs_(std::move(chunkCapUs))
        , rows_(rows)
    {
    }

    // 70b: a fixed-step job (StepsFor): exactly `steps` chunks, one per pass, whatever the budget.
    struct FixedStepsTag {};
    GpuComputeLaneJob(FixedStepsTag, ProductId id, IGpuDispatcher* dispatcher, std::function<double()> predictedMs,
                      uint32_t steps)
        : id_(id)
        , dispatcher_(dispatcher)
        , predictedMs_(std::move(predictedMs))
        , rows_(0)
        , fixedSteps_(steps == 0 ? 1u : steps)
    {
    }
    uint32_t FixedStepCount() const { return fixedSteps_; }

    // 70b: an optional gate consulted by Begin (render thread): false = cannot run this frame (e.g. P12 waits for
    // P12(N-1) to be published). The runner abandons and counts the job.
    void SetBeginGate(std::function<bool(const LaneJobContext&)> gate) { gate_ = std::move(gate); }

    ProductId Product() const override { return id_; }

    bool Begin(const LaneJobContext& job) override
    {
        active_ = false;
        next_ = 0;
        if (dispatcher_ == nullptr || (gate_ && !gate_(job)))
            return false;
        plan_ = PlanFor();
        predictedUs_ = PredictedUs();
        next_ = 0;
        active_ = true;
        return true;
    }

    // The device cost of the next chunk (the CPU only records it): the whole-job prediction divided by
    // the chunk count of the current plan; before Begin / between jobs, of the plan the next job would
    // get from the current inputs. Rounded up so a chunk planned to fit the cap is predicted <= cap.
    uint32_t PredictChunkUs() const override
    {
        if (active_)
            return ChunkUs(predictedUs_, plan_.chunkCount);
        const ChunkPlan p = PlanFor();
        return ChunkUs(PredictedUs(), p.chunkCount);
    }

    ChunkResult RunChunk(const LaneJobContext& job, uint32_t) override
    {
        if (dispatcher_ == nullptr || next_ >= plan_.chunkCount) {
            active_ = false;
            return ChunkResult::Failed;
        }
        if (!dispatcher_->Dispatch(job, next_, plan_.chunkCount)) {
            active_ = false;
            return ChunkResult::Failed;
        }
        ++next_;
        if (next_ >= plan_.chunkCount) {
            active_ = false;
            return ChunkResult::Done;
        }
        return ChunkResult::More;
    }

    // The seal (or a plan change) took the job: stop recording; no job is in progress any more.
    void Cancel(const LaneJobContext&) override { active_ = false; }

    const ChunkPlan& Plan() const { return plan_; }
    uint32_t NextChunk() const { return next_; }

private:
    ChunkPlan PlanFor() const
    {
        if (fixedSteps_ != 0) {
            ChunkPlan p;
            p.chunkCount = fixedSteps_;
            return p;
        }
        return PlanChunks(rows_, predictedMs_ ? predictedMs_() : 0.0, chunkCapUs_ ? chunkCapUs_() : 0u);
    }
    double PredictedUs() const
    {
        const double ms = predictedMs_ ? predictedMs_() : 0.0;
        return ms > 0.0 ? ms * 1000.0 : 0.0;
    }
    static uint32_t ChunkUs(double wholeUs, uint32_t chunks)
    {
        if (!(wholeUs > 0.0) || chunks == 0)
            return 0;
        const double per = std::ceil(wholeUs / double(chunks));
        return per >= 4294967295.0 ? 0xFFFFFFFFu : uint32_t(per);
    }

    ProductId id_;
    IGpuDispatcher* dispatcher_;
    std::function<double()> predictedMs_;
    std::function<uint32_t()> chunkCapUs_;
    uint32_t rows_;
    uint32_t fixedSteps_ = 0;   // 70b: one chunk per pass (StepsFor); 0 = rows split by the budget
    std::function<bool(const LaneJobContext&)> gate_;
    ChunkPlan plan_;
    uint32_t next_ = 0;
    bool active_ = false;       // a job is in progress: plan_ and predictedUs_ are its own
    double predictedUs_ = 0.0;  // the whole-job prediction at Begin
};

} // namespace VpLaneProducts
