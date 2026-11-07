// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <VP-V/core/LaneInterfaces.h>
#include <VP-V/modules/input/source_analysis/events/shared/CutFadeJudge.h>
#include <VP-V/modules/input/source_analysis/lane/shared/HalfLuma.h>
#include <VP-V/modules/input/source_analysis/lane/shared/SourceStats.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <utility>
#include <vector>

// Skeleton S6: the CPU lane products P1 (half + quarter luma) and P4 (source stats), their one-shot
// live-fill forms, the either/or switch of the P4 walk, and the per-consumer event classifier.
// Pure C++14, no Windows, no GPU. Every function is deterministic: a finished product never depends
// on how the work was chunked (chunking only decides WHEN rows are computed, never WHAT they contain).
namespace VpLaneProducts {

using VpSkeleton::ChunkResult;
using VpSkeleton::Dims;
using VpSkeleton::ILaneProducer;
using VpSkeleton::LaneJobContext;
using VpSkeleton::ProductId;
using VpSkeleton::SourceFrameView;

// ---------------------------------------------------------------------------------------------
// P1 half luma. Product buffer: half plane (hw x hh, tightly packed uint16) then the quarter plane
// (qw x qh). hw = ceil(w/2), hh = ceil(h/2), qw = ceil(w/4), qh = ceil(h/4).
// ---------------------------------------------------------------------------------------------

// The grid, taps and rounding are the shared unit U3 (VpUnit::HalfLuma, lane/shared/HalfLuma.h).
inline uint32_t HalfDim(uint32_t v) { return VpUnit::HalfLuma::HalfDim(v); }
inline uint32_t QuarterDim(uint32_t v) { return VpUnit::HalfLuma::QuarterDim(v); }
inline size_t HalfLumaHalfBytes(Dims d) { return VpUnit::HalfLuma::HalfBytes(d.width, d.height); }
inline size_t HalfLumaQuarterBytes(Dims d) { return VpUnit::HalfLuma::QuarterBytes(d.width, d.height); }
inline size_t HalfLumaBytes(Dims d) { return VpUnit::HalfLuma::TotalBytes(d.width, d.height); }

// Half rows [rowBegin, rowEnd) of the 2x2 box mean of the 16-bit-scaled samples; odd edges replicate.
inline void HalfLumaRows(const SourceFrameView& v, uint16_t* half, uint32_t rowBegin, uint32_t rowEnd)
{
    const uint32_t hw = HalfDim(v.width);
    const uint32_t hh = HalfDim(v.height);
    if (rowEnd > hh)
        rowEnd = hh;
    for (uint32_t y = rowBegin; y < rowEnd; ++y) {
        const uint32_t y0 = VpUnit::HalfLuma::Tap0(y, v.height);
        const uint32_t y1 = VpUnit::HalfLuma::Tap1(y, v.height);
        const uint16_t* r0 = v.luma + size_t(y0) * v.strideSamples;
        const uint16_t* r1 = v.luma + size_t(y1) * v.strideSamples;
        uint16_t* dst = half + size_t(y) * hw;
        for (uint32_t x = 0; x < hw; ++x) {
            const uint32_t x0 = VpUnit::HalfLuma::Tap0(x, v.width);
            const uint32_t x1 = VpUnit::HalfLuma::Tap1(x, v.width);
            const uint32_t a = uint32_t(uint16_t(r0[x0] << v.msbShift));
            const uint32_t b = uint32_t(uint16_t(r0[x1] << v.msbShift));
            const uint32_t c = uint32_t(uint16_t(r1[x0] << v.msbShift));
            const uint32_t d = uint32_t(uint16_t(r1[x1] << v.msbShift));
            dst[x] = VpUnit::HalfLuma::FinishP1(VpUnit::HalfLuma::Box4(a, b, c, d));
        }
    }
}

// Quarter rows [rowBegin, rowEnd) from the finished half plane (hw x hh); odd edges replicate.
inline void QuarterFromHalf(const uint16_t* half, uint32_t hw, uint32_t hh, uint16_t* quarter, uint32_t rowBegin,
                            uint32_t rowEnd)
{
    const uint32_t qw = (hw + 1u) / 2u;
    const uint32_t qh = (hh + 1u) / 2u;
    if (rowEnd > qh)
        rowEnd = qh;
    for (uint32_t y = rowBegin; y < rowEnd; ++y) {
        const uint32_t y0 = VpUnit::HalfLuma::Tap0(y, hh);
        const uint32_t y1 = VpUnit::HalfLuma::Tap1(y, hh);
        const uint16_t* r0 = half + size_t(y0) * hw;
        const uint16_t* r1 = half + size_t(y1) * hw;
        uint16_t* dst = quarter + size_t(y) * qw;
        for (uint32_t x = 0; x < qw; ++x) {
            const uint32_t x0 = VpUnit::HalfLuma::Tap0(x, hw);
            const uint32_t x1 = VpUnit::HalfLuma::Tap1(x, hw);
            dst[x] = VpUnit::HalfLuma::FinishP1(VpUnit::HalfLuma::Box4(r0[x0], r0[x1], r1[x0], r1[x1]));
        }
    }
}

// The live-fill form: the whole product at once. False when the frame is invalid or the buffer short.
inline bool ComputeHalfLuma(const SourceFrameView& v, uint8_t* out, size_t outBytes)
{
    if (!v.Valid() || out == nullptr)
        return false;
    const Dims d{ v.width, v.height };
    if (outBytes < HalfLumaBytes(d))
        return false;
    const uint32_t hw = HalfDim(v.width);
    const uint32_t hh = HalfDim(v.height);
    uint16_t* half = reinterpret_cast<uint16_t*>(out);
    uint16_t* quarter = reinterpret_cast<uint16_t*>(out + HalfLumaHalfBytes(d));
    HalfLumaRows(v, half, 0, hh);
    QuarterFromHalf(half, hw, hh, quarter, 0, (hh + 1u) / 2u);
    return true;
}

// Rows per chunk from a budget and a measured cost per row (scheduling only). An unknown cost
// (0) is treated as 1 us per row.
inline uint32_t RowsForBudget(uint32_t budgetUs, double usPerRow, uint32_t remaining)
{
    if (remaining == 0)
        return 0;
    const double per = usPerRow > 0.0 ? usPerRow : 1.0;
    const double rows = double(budgetUs) / per;
    if (rows < 1.0)
        return 1u;
    if (rows >= double(remaining))
        return remaining;
    return uint32_t(rows);
}

// An optional injected microsecond clock; the cost per row is an exponential mean of the measured
// chunks.
class RowCostTracker {
public:
    explicit RowCostTracker(std::function<uint64_t()> clockUs = nullptr) : clock_(std::move(clockUs)) {}
    uint64_t Now() const { return clock_ ? clock_() : 0u; }
    bool HasClock() const { return bool(clock_); }
    double UsPerRow() const { return usPerRow_; }
    void Observe(uint64_t t0, uint32_t rows)
    {
        if (!clock_ || rows == 0)
            return;
        const uint64_t t1 = clock_();
        const uint64_t el = t1 > t0 ? t1 - t0 : 0u;
        const double us = double(el) / double(rows);
        const double v = us > 0.001 ? us : 0.001;
        usPerRow_ = usPerRow_ > 0.0 ? 0.5 * usPerRow_ + 0.5 * v : v;
        totalUs_ += uint32_t(el);
    }
    uint32_t TotalUs() const { return totalUs_; }
    void Reset() { totalUs_ = 0; }

private:
    std::function<uint64_t()> clock_;
    double usPerRow_ = 0.0;
    uint32_t totalUs_ = 0;
};

class HalfLumaProducer : public ILaneProducer {
public:
    explicit HalfLumaProducer(std::function<uint64_t()> clockUs = nullptr) : cost_(std::move(clockUs)) {}

    ProductId Product() const override { return VpSkeleton::kP1HalfLuma; }

    bool Begin(const LaneJobContext& job) override
    {
        if (job.source == nullptr || !job.source->Valid() || job.out == nullptr)
            return false;
        dims_ = Dims{ job.source->width, job.source->height };
        if (job.outBytes < HalfLumaBytes(dims_))
            return false;
        halfRows_ = HalfDim(dims_.height);
        quarterRows_ = QuarterDim(dims_.height);
        phase_ = 0;
        row_ = 0;
        cost_.Reset();
        return true;
    }

    ChunkResult RunChunk(const LaneJobContext& job, uint32_t budgetUs) override
    {
        if (job.source == nullptr || job.out == nullptr || halfRows_ == 0)
            return ChunkResult::Failed;
        const uint32_t hw = HalfDim(dims_.width);
        uint16_t* half = reinterpret_cast<uint16_t*>(job.out);
        uint16_t* quarter = reinterpret_cast<uint16_t*>(job.out + HalfLumaHalfBytes(dims_));
        const uint64_t t0 = cost_.Now();
        uint32_t done = 0;
        if (phase_ == 0) {
            done = RowsForBudget(budgetUs, cost_.UsPerRow(), halfRows_ - row_);
            HalfLumaRows(*job.source, half, row_, row_ + done);
            row_ += done;
            if (row_ >= halfRows_) {
                phase_ = 1;
                row_ = 0;
            }
        } else {
            done = RowsForBudget(budgetUs, cost_.UsPerRow(), quarterRows_ - row_);
            QuarterFromHalf(half, hw, halfRows_, quarter, row_, row_ + done);
            row_ += done;
        }
        cost_.Observe(t0, done);
        if (phase_ == 1 && row_ >= quarterRows_) {
            if (job.header != nullptr && cost_.HasClock())
                job.header->costUs = cost_.TotalUs();
            return ChunkResult::Done;
        }
        return ChunkResult::More;
    }

private:
    Dims dims_;
    uint32_t halfRows_ = 0;
    uint32_t quarterRows_ = 0;
    uint32_t phase_ = 0;
    uint32_t row_ = 0;
    RowCostTracker cost_;
};

// ---------------------------------------------------------------------------------------------
// P4 source stats.
// ---------------------------------------------------------------------------------------------

// P4 record, accumulator and percentile/rect/grain law: the shared unit U6 (VpUnit::SourceStats, lane/shared/SourceStats.h).
// The lane names below are aliases of it (identity: the code was moved unchanged).
constexpr uint32_t kStatsHistBins = VpUnit::SourceStats::kHistBins;      // top 10 bits of the 16-bit half-plane sample
constexpr uint32_t kStatsVersion = VpUnit::SourceStats::kRecordVersion;
constexpr uint32_t kHalfLumaVersion = VpUnit::HalfLuma::kUnitVersion;       // P1 half + quarter luma layout and law (core/ProductCatalog.h)
constexpr uint32_t kStatsBytes = VpUnit::SourceStats::kRecordBytes;     // the declared fixed size of P4
constexpr uint32_t kGrainBlock = VpUnit::SourceStats::kGrainBlock;      // grain: 8x8 blocks of the half plane
constexpr uint32_t kRectFractionShift = VpUnit::SourceStats::kRectFractionShift;  // rect threshold = p1 + (p99 - p1) / 16

using SourceStatsRecord = VpUnit::SourceStats::Record;
using StatsAccumulator = VpUnit::SourceStats::Accumulator;
static_assert(sizeof(SourceStatsRecord) <= kStatsBytes, "the stats record must fit the P4 buffer");

inline bool ReadSourceStats(const uint8_t* buf, size_t bytes, SourceStatsRecord* out)
{
    return VpUnit::SourceStats::ReadRecord(buf, bytes, out);
}

inline uint32_t Isqrt64(uint64_t v) { return VpUnit::SourceStats::Isqrt64(v); }

inline void WriteStatsRecord(const SourceStatsRecord& rec, uint8_t* out, size_t outBytes)
{
    std::memset(out, 0, outBytes);
    std::memcpy(out, &rec, sizeof(rec));
}

// The one-shot (live-fill) form: P1's product bytes -> the stored record (rest of the buffer zero).
inline bool ComputeSourceStats(const uint8_t* halfLumaProduct, Dims dims, uint8_t* out, size_t outBytes)
{
    if (halfLumaProduct == nullptr || dims.width == 0 || dims.height == 0)
        return false;
    return VpUnit::SourceStats::ComputeRecord(reinterpret_cast<const uint16_t*>(halfLumaProduct), HalfDim(dims.width),
                                              HalfDim(dims.height), out, outBytes);
}

class SourceStatsProducer : public ILaneProducer {
public:
    explicit SourceStatsProducer(Dims dims, std::function<uint64_t()> clockUs = nullptr)
        : dims_(dims), cost_(std::move(clockUs))
    {
    }

    ProductId Product() const override { return VpSkeleton::kP4SourceStats; }

    bool Begin(const LaneJobContext& job) override
    {
        if (job.out == nullptr || job.outBytes < sizeof(SourceStatsRecord) || dims_.width == 0 || dims_.height == 0
            || job.inputData[0] == nullptr)
            return false;
        hw_ = HalfDim(dims_.width);
        hh_ = HalfDim(dims_.height);
        acc_.Reset(hw_, hh_);
        row_ = 0;
        cost_.Reset();
        return true;
    }

    ChunkResult RunChunk(const LaneJobContext& job, uint32_t budgetUs) override
    {
        if (job.out == nullptr || job.inputData[0] == nullptr || hh_ == 0)
            return ChunkResult::Failed;
        const uint64_t t0 = cost_.Now();
        const uint32_t rows = RowsForBudget(budgetUs, cost_.UsPerRow(), hh_ - row_);
        acc_.AddRows(reinterpret_cast<const uint16_t*>(job.inputData[0]), row_, row_ + rows);
        row_ += rows;
        cost_.Observe(t0, rows);
        if (row_ < hh_)
            return ChunkResult::More;
        SourceStatsRecord rec;
        acc_.Finish(&rec);
        WriteStatsRecord(rec, job.out, job.outBytes);
        if (job.header != nullptr && cost_.HasClock())
            job.header->costUs = cost_.TotalUs();
        return ChunkResult::Done;
    }

private:
    Dims dims_;
    uint32_t hw_ = 0, hh_ = 0, row_ = 0;
    StatsAccumulator acc_;
    RowCostTracker cost_;
};

// ---------------------------------------------------------------------------------------------
// ---------------------------------------------------------------------------------------------
// Event classification: the shared unit U7 (VpUnit::CutFadeJudge, events/shared/CutFadeJudge.h). Derived per
// consumer from two stored records (cut/fade/flash are never stored); the lane names are aliases of it.
// ---------------------------------------------------------------------------------------------

constexpr double kCutHistFrac = VpUnit::CutFadeJudge::kCutHistFrac;
constexpr double kCutPossibleHistFrac = VpUnit::CutFadeJudge::kCutPossibleHistFrac;
constexpr int64_t kFadeMinDiv = VpUnit::CutFadeJudge::kFadeMinDiv;
constexpr int64_t kFadeMaxDiv = VpUnit::CutFadeJudge::kFadeMaxDiv;
constexpr int64_t kFlashMeanDiv = VpUnit::CutFadeJudge::kFlashMeanDiv;

using EventSignals = VpUnit::CutFadeJudge::EventSignals;

inline uint32_t HistDistance(const SourceStatsRecord& a, const SourceStatsRecord& b)
{
    return VpUnit::CutFadeJudge::HistDistance(a, b);
}

inline EventSignals ClassifyEvents(const SourceStatsRecord& prev, const SourceStatsRecord& cur, bool fullBetween,
                                   bool continuityBetween)
{
    return VpUnit::CutFadeJudge::ClassifyEvents(prev, cur, fullBetween, continuityBetween);
}

// ---------------------------------------------------------------------------------------------
// P4 walk either/or: Original = Bill's walk (default). Lane only when requested AND P4 exists; the
// host shows FallbackCatalog::SourceStatsWalk when the request cannot be honoured.
// ---------------------------------------------------------------------------------------------

enum class WalkSource { Original, Lane };

inline WalkSource ParseWalkSource(const char* s)
{
    if (s == nullptr)
        return WalkSource::Original;
    const char* lane = "lane";
    size_t i = 0;
    for (; lane[i] != '\0'; ++i) {
        char c = s[i];
        if (c >= 'A' && c <= 'Z')
            c = char(c - 'A' + 'a');
        if (c != lane[i])
            return WalkSource::Original;
    }
    return s[i] == '\0' ? WalkSource::Lane : WalkSource::Original;
}

inline WalkSource EffectiveWalk(WalkSource requested, bool p4Available)
{
    return requested == WalkSource::Lane && p4Available ? WalkSource::Lane : WalkSource::Original;
}

} // namespace VpLaneProducts
