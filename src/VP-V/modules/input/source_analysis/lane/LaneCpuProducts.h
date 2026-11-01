// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <VP-V/core/LaneInterfaces.h>

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

inline uint32_t HalfDim(uint32_t v) { return (v + 1u) / 2u; }
inline uint32_t QuarterDim(uint32_t v) { return (v + 3u) / 4u; }
inline size_t HalfLumaHalfBytes(Dims d) { return size_t(HalfDim(d.width)) * HalfDim(d.height) * 2u; }
inline size_t HalfLumaQuarterBytes(Dims d) { return size_t(QuarterDim(d.width)) * QuarterDim(d.height) * 2u; }
inline size_t HalfLumaBytes(Dims d) { return HalfLumaHalfBytes(d) + HalfLumaQuarterBytes(d); }

// Half rows [rowBegin, rowEnd) of the 2x2 box mean of the 16-bit-scaled samples; odd edges replicate.
inline void HalfLumaRows(const SourceFrameView& v, uint16_t* half, uint32_t rowBegin, uint32_t rowEnd)
{
    const uint32_t hw = HalfDim(v.width);
    const uint32_t hh = HalfDim(v.height);
    if (rowEnd > hh)
        rowEnd = hh;
    for (uint32_t y = rowBegin; y < rowEnd; ++y) {
        const uint32_t y0 = 2u * y;
        const uint32_t y1 = std::min(y0 + 1u, v.height - 1u);
        const uint16_t* r0 = v.luma + size_t(y0) * v.strideSamples;
        const uint16_t* r1 = v.luma + size_t(y1) * v.strideSamples;
        uint16_t* dst = half + size_t(y) * hw;
        for (uint32_t x = 0; x < hw; ++x) {
            const uint32_t x0 = 2u * x;
            const uint32_t x1 = std::min(x0 + 1u, v.width - 1u);
            const uint32_t a = uint32_t(uint16_t(r0[x0] << v.msbShift));
            const uint32_t b = uint32_t(uint16_t(r0[x1] << v.msbShift));
            const uint32_t c = uint32_t(uint16_t(r1[x0] << v.msbShift));
            const uint32_t d = uint32_t(uint16_t(r1[x1] << v.msbShift));
            dst[x] = uint16_t((a + b + c + d + 2u) >> 2);
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
        const uint32_t y0 = 2u * y;
        const uint32_t y1 = std::min(y0 + 1u, hh - 1u);
        const uint16_t* r0 = half + size_t(y0) * hw;
        const uint16_t* r1 = half + size_t(y1) * hw;
        uint16_t* dst = quarter + size_t(y) * qw;
        for (uint32_t x = 0; x < qw; ++x) {
            const uint32_t x0 = 2u * x;
            const uint32_t x1 = std::min(x0 + 1u, hw - 1u);
            dst[x] = uint16_t((uint32_t(r0[x0]) + r0[x1] + r1[x0] + r1[x1] + 2u) >> 2);
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

constexpr uint32_t kStatsHistBins = 1024;      // top 10 bits of the 16-bit half-plane sample
constexpr uint32_t kStatsVersion = 1;
constexpr uint32_t kStatsBytes = 8192;         // the declared fixed size of P4
constexpr uint32_t kGrainBlock = 8;            // grain: 8x8 blocks of the half plane
constexpr uint32_t kRectFractionShift = 4;     // rect threshold = p1 + (p99 - p1) / 16

// The stored record (POD). Codes are on the 16-bit scale. The picture rect is on the half plane
// (end exclusive) and is an EVIDENCE rect (rows/cols brighter than the frame's own floor), NOT
// Bill's active-picture result.
struct SourceStatsRecord {
    uint32_t version = kStatsVersion;
    uint32_t halfWidth = 0;
    uint32_t halfHeight = 0;
    uint32_t pixelCount = 0;
    uint32_t mean = 0;
    uint32_t minCode = 0;
    uint32_t maxCode = 0;
    uint32_t p01 = 0;
    uint32_t p50 = 0;
    uint32_t p99 = 0;
    uint32_t rectFound = 0;   // 0: no row/col exceeded the threshold; the rect is the whole plane
    uint32_t rectX0 = 0;
    uint32_t rectY0 = 0;
    uint32_t rectX1 = 0;
    uint32_t rectY1 = 0;
    uint32_t grain = 0;       // median over 8x8 blocks of the RMS second difference (16-bit codes)
    uint32_t reserved[2] = { 0, 0 };
    uint32_t hist[kStatsHistBins] = {};
};
static_assert(sizeof(SourceStatsRecord) <= kStatsBytes, "the stats record must fit the P4 buffer");

inline bool ReadSourceStats(const uint8_t* buf, size_t bytes, SourceStatsRecord* out)
{
    if (buf == nullptr || out == nullptr || bytes < sizeof(SourceStatsRecord))
        return false;
    std::memcpy(out, buf, sizeof(SourceStatsRecord));
    return out->version == kStatsVersion;
}

inline uint32_t Isqrt64(uint64_t v)
{
    uint64_t r = 0;
    uint64_t bit = uint64_t(1) << 62;
    while (bit > v)
        bit >>= 2;
    while (bit != 0) {
        if (v >= r + bit) {
            v -= r + bit;
            r = (r >> 1) + bit;
        } else {
            r >>= 1;
        }
        bit >>= 2;
    }
    return uint32_t(r);
}

// Partial sums over half-plane rows. Everything is integer sums, so the reduction does not depend
// on the chunking; rows are still fed in ascending order.
class StatsAccumulator {
public:
    void Reset(uint32_t hw, uint32_t hh)
    {
        hw_ = hw;
        hh_ = hh;
        std::memset(hist_, 0, sizeof(hist_));
        sum_ = 0;
        min_ = 0xFFFFu;
        max_ = 0;
        rowSum_.assign(hh, 0);
        colSum_.assign(hw, 0);
        bw_ = (hw + kGrainBlock - 1u) / kGrainBlock;
        bh_ = (hh + kGrainBlock - 1u) / kGrainBlock;
        blockSq_.assign(size_t(bw_) * bh_, 0);
        blockN_.assign(size_t(bw_) * bh_, 0);
    }

    void AddRows(const uint16_t* half, uint32_t rowBegin, uint32_t rowEnd)
    {
        if (rowEnd > hh_)
            rowEnd = hh_;
        for (uint32_t y = rowBegin; y < rowEnd; ++y) {
            const uint16_t* r = half + size_t(y) * hw_;
            const bool vInterior = y >= 1u && y + 1u < hh_;
            uint64_t rs = 0;
            for (uint32_t x = 0; x < hw_; ++x) {
                const uint32_t s = r[x];
                ++hist_[s >> 6];
                rs += s;
                colSum_[x] += s;
                if (s < min_)
                    min_ = s;
                if (s > max_)
                    max_ = s;
                int64_t sq = 0;
                uint32_t n = 0;
                if (x >= 1u && x + 1u < hw_) {
                    const int64_t d = int64_t(r[x - 1]) - 2 * int64_t(s) + int64_t(r[x + 1]);
                    sq += d * d;
                    ++n;
                }
                if (vInterior) {
                    const int64_t d = int64_t(half[size_t(y - 1) * hw_ + x]) - 2 * int64_t(s)
                                      + int64_t(half[size_t(y + 1) * hw_ + x]);
                    sq += d * d;
                    ++n;
                }
                if (n != 0) {
                    const size_t b = size_t(y / kGrainBlock) * bw_ + (x / kGrainBlock);
                    blockSq_[b] += uint64_t(sq);
                    blockN_[b] += n;
                }
            }
            rowSum_[y] = rs;
            sum_ += rs;
        }
    }

    void Finish(SourceStatsRecord* rec) const
    {
        *rec = SourceStatsRecord();
        rec->halfWidth = hw_;
        rec->halfHeight = hh_;
        const uint64_t n = uint64_t(hw_) * hh_;
        rec->pixelCount = uint32_t(n);
        std::memcpy(rec->hist, hist_, sizeof(hist_));
        if (n == 0)
            return;
        rec->mean = uint32_t(sum_ / n);
        rec->minCode = min_;
        rec->maxCode = max_;
        rec->p01 = Percentile((n + 99u) / 100u);
        rec->p50 = Percentile((n + 1u) / 2u);
        rec->p99 = Percentile((n * 99u + 99u) / 100u);
        // Evidence rect: rows/cols whose mean exceeds p1 + (p99 - p1) / 16.
        const uint32_t span = rec->p99 > rec->p01 ? rec->p99 - rec->p01 : 0u;
        const uint64_t thr = uint64_t(rec->p01) + (span >> kRectFractionShift);
        bool found = false;
        uint32_t y0 = 0, y1 = 0, x0 = 0, x1 = 0;
        for (uint32_t y = 0; y < hh_; ++y)
            if (rowSum_[y] / hw_ > thr) {
                if (!found)
                    y0 = y;
                y1 = y + 1u;
                found = true;
            }
        bool foundX = false;
        for (uint32_t x = 0; x < hw_; ++x)
            if (colSum_[x] / hh_ > thr) {
                if (!foundX)
                    x0 = x;
                x1 = x + 1u;
                foundX = true;
            }
        if (found && foundX) {
            rec->rectFound = 1;
            rec->rectX0 = x0;
            rec->rectX1 = x1;
            rec->rectY0 = y0;
            rec->rectY1 = y1;
        } else {
            rec->rectX1 = hw_;
            rec->rectY1 = hh_;
        }
        // Grain: median over blocks of sqrt(mean of the squared second differences).
        std::vector<uint32_t> rms;
        rms.reserve(blockSq_.size());
        for (size_t b = 0; b < blockSq_.size(); ++b)
            if (blockN_[b] != 0)
                rms.push_back(Isqrt64(blockSq_[b] / blockN_[b]));
        if (!rms.empty()) {
            std::sort(rms.begin(), rms.end());
            rec->grain = rms[rms.size() / 2];
        }
    }

private:
    // Centre code of the smallest bin whose cumulative count reaches `target`.
    uint32_t Percentile(uint64_t target) const
    {
        uint64_t c = 0;
        for (uint32_t i = 0; i < kStatsHistBins; ++i) {
            c += hist_[i];
            if (c >= target)
                return (i << 6) | 32u;
        }
        return ((kStatsHistBins - 1u) << 6) | 32u;
    }

    uint32_t hw_ = 0, hh_ = 0, bw_ = 0, bh_ = 0;
    uint32_t hist_[kStatsHistBins] = {};
    uint64_t sum_ = 0;
    uint32_t min_ = 0xFFFFu;
    uint32_t max_ = 0;
    std::vector<uint64_t> rowSum_, colSum_, blockSq_;
    std::vector<uint32_t> blockN_;
};

inline void WriteStatsRecord(const SourceStatsRecord& rec, uint8_t* out, size_t outBytes)
{
    std::memset(out, 0, outBytes);
    std::memcpy(out, &rec, sizeof(rec));
}

// The one-shot (live-fill) form: P1's product bytes -> the stored record (rest of the buffer zero).
inline bool ComputeSourceStats(const uint8_t* halfLumaProduct, Dims dims, uint8_t* out, size_t outBytes)
{
    if (halfLumaProduct == nullptr || out == nullptr || outBytes < sizeof(SourceStatsRecord) || dims.width == 0
        || dims.height == 0)
        return false;
    const uint32_t hw = HalfDim(dims.width);
    const uint32_t hh = HalfDim(dims.height);
    StatsAccumulator acc;
    acc.Reset(hw, hh);
    acc.AddRows(reinterpret_cast<const uint16_t*>(halfLumaProduct), 0, hh);
    SourceStatsRecord rec;
    acc.Finish(&rec);
    WriteStatsRecord(rec, out, outBytes);
    return true;
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
// Event classification, derived per consumer from two stored records (cut/fade/flash are never
// stored). All thresholds are relative to the frames' own spread (p99 - p1) or normalised
// histogram mass: no absolute rate or resolution constants.
// ---------------------------------------------------------------------------------------------

constexpr double kCutHistFrac = 0.50;          // normalised L1 distance / 2 above this: a different picture
constexpr double kCutPossibleHistFrac = 0.25;  // above this (or a continuity slip): a cut is possible
constexpr int64_t kFadeMinDiv = 64;            // fade: |mean step| >= spread / 64 ...
constexpr int64_t kFadeMaxDiv = 4;             // ... and < spread / 4 (more is a jump, not a ramp)
constexpr int64_t kFlashMeanDiv = 4;           // flash: |mean step| >= spread / 4 with the spread kept within spread / 4

struct EventSignals {
    bool cut = false;
    bool cutPossible = false;
    bool fade = false;
    bool flash = false;
    uint32_t histDistance = 0;   // normalised L1 distance, 0..65535 (65535 = disjoint histograms)
};

inline uint32_t HistDistance(const SourceStatsRecord& a, const SourceStatsRecord& b)
{
    const double na = a.pixelCount > 0 ? double(a.pixelCount) : 1.0;
    const double nb = b.pixelCount > 0 ? double(b.pixelCount) : 1.0;
    double l1 = 0.0;
    for (uint32_t i = 0; i < kStatsHistBins; ++i)
        l1 += std::fabs(double(a.hist[i]) / na - double(b.hist[i]) / nb);
    const double frac = l1 * 0.5;   // 0..1
    const double v = frac * 65535.0 + 0.5;
    return v >= 65535.0 ? 65535u : uint32_t(v);
}

inline EventSignals ClassifyEvents(const SourceStatsRecord& prev, const SourceStatsRecord& cur, bool fullBetween,
                                   bool continuityBetween)
{
    EventSignals e;
    e.histDistance = HistDistance(prev, cur);
    const double frac = double(e.histDistance) / 65535.0;
    if (fullBetween) {
        e.cut = true;
        e.cutPossible = true;
        return e;
    }
    if (continuityBetween)
        e.cutPossible = true;
    if (frac > kCutPossibleHistFrac)
        e.cutPossible = true;

    const uint32_t spreadPrev = prev.p99 > prev.p01 ? prev.p99 - prev.p01 : 0u;
    const uint32_t spreadCur = cur.p99 > cur.p01 ? cur.p99 - cur.p01 : 0u;
    const int64_t spread = int64_t(std::max<uint32_t>(1u, std::max(spreadPrev, spreadCur)));
    const int64_t dMean = int64_t(cur.mean) - int64_t(prev.mean);
    const int64_t aMean = dMean < 0 ? -dMean : dMean;
    const int64_t dSpread = int64_t(spreadCur) - int64_t(spreadPrev);
    const int64_t aSpread = dSpread < 0 ? -dSpread : dSpread;

    if (frac > kCutHistFrac) {
        // A shifted histogram of unchanged spread is a flash (or a cut between look-alike
        // scenes: reported as a flash with cutPossible); any other jump is a cut.
        if (aMean * kFlashMeanDiv >= spread && aSpread * 4 <= spread) {
            e.flash = true;
            e.cutPossible = true;
        } else {
            e.cut = true;
            e.cutPossible = true;
        }
        return e;
    }
    // Gradual ramp: mean and contrast move together by a small step.
    const bool sameSign = (dMean > 0 && dSpread > 0) || (dMean < 0 && dSpread < 0);
    if (aMean * kFadeMinDiv >= spread && aMean * kFadeMaxDiv < spread && sameSign)
        e.fade = true;
    return e;
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
