// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <VP-V/core/LaneInterfaces.h>

#include <cmath>
#include <cstdint>
#include <functional>
#include <utility>

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

    ProductId Product() const override { return id_; }

    bool Begin(const LaneJobContext&) override
    {
        if (dispatcher_ == nullptr)
            return false;
        plan_ = PlanChunks(rows_, predictedMs_ ? predictedMs_() : 0.0, chunkCapUs_ ? chunkCapUs_() : 0u);
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
        const ChunkPlan p = PlanChunks(rows_, predictedMs_ ? predictedMs_() : 0.0, chunkCapUs_ ? chunkCapUs_() : 0u);
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

    const ChunkPlan& Plan() const { return plan_; }
    uint32_t NextChunk() const { return next_; }

private:
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
    ChunkPlan plan_;
    uint32_t next_ = 0;
    bool active_ = false;       // a job is in progress: plan_ and predictedUs_ are its own
    double predictedUs_ = 0.0;  // the whole-job prediction at Begin
};

} // namespace VpLaneProducts
