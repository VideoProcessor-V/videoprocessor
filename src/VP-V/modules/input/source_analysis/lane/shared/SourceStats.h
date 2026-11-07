// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// Shared unit U6 "histograms / APL / percentiles" (doc 62 section 1.2): the one law of the P4 source-stats record.
// Moved unchanged from the lane (LaneCpuProducts.h StatsAccumulator and record) so the lane producer, the live fill and
// the VP-only consumers (HdrInference veto grid, VpFrameAnalysis Reducer percentiles) call ONE implementation.
//
// Law:
//   histogram  1024 bins, bin = top 10 bits of the 16-bit half-plane sample (Histogram10 for 10-bit codes: bin = code)
//   percentile the smallest bin whose cumulative count reaches the target (FirstBinReaching; lane: the bin centre)
//   rect       evidence rect: rows/cols whose mean exceeds p1 + (p99 - p1) / 16 (NOT Bill's active picture; see U8)
//   grain      median over 8x8 blocks of the RMS second difference
//   distance   half the L1 distance of two normalised histograms (HalfL1Distance), the input of U7
// Determinism: integer sums in a fixed row order; a finished record never depends on how the rows were chunked.
//
// Header-only, C++14, no Windows or GPU headers.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace VpUnit
{
namespace SourceStats
{
// Bump on ANY change of the record layout, the bin law, the percentile law, the rect rule or the grain law
// (P4's ProductDecl expected number, core/ProductCatalog.h kExpectedStatsUnit, and every stored record carry it).
constexpr uint32_t kUnitVersion = 1;
constexpr uint32_t kRecordVersion = kUnitVersion;
constexpr uint32_t kHistBins = 1024;      // top 10 bits of the 16-bit half-plane sample
constexpr uint32_t kRecordBytes = 8192;   // the declared fixed size of P4
constexpr uint32_t kGrainBlock = 8;       // grain: 8x8 blocks of the half plane
constexpr uint32_t kRectFractionShift = 4; // rect threshold = p1 + (p99 - p1) / 16

// ---- the small laws every histogram consumer shares -------------------------------------------------------------------

// Bin of a 10-bit code in an N-bin histogram: code / (1024 / N), clamped to the last bin (N a power of two <= 1024).
inline uint32_t HistBin10(uint32_t code10, uint32_t bins)
{
    const uint32_t c = (std::min)(code10, 1023u);
    return (std::min)(c / (1024u / bins), bins - 1u);
}

// The index of the first bin whose cumulative count reaches `target`, or -1. `Acc` is the accumulation type (uint64_t for
// integer targets, double where the consumer compares in floating point); counts and target are converted to it.
template <class Acc, class H, class T>
inline int FirstBinReaching(const H* hist, int bins, T target)
{
    Acc cumulative = Acc(0);
    const Acc goal = static_cast<Acc>(target);
    for (int i = 0; i < bins; ++i) {
        cumulative += static_cast<Acc>(hist[i]);
        if (cumulative >= goal)
            return i;
    }
    return -1;
}

// Half the L1 distance of two histograms normalised by their totals na and nb (pass 1 for histograms that already are
// fractions): 0 = identical, 1 = disjoint. Fixed bin order.
template <class Acc, class A, class B>
inline Acc HalfL1Distance(const A* a, const B* b, size_t n, Acc na, Acc nb)
{
    Acc l1 = Acc(0);
    for (size_t i = 0; i < n; ++i)
        l1 += std::fabs(static_cast<Acc>(a[i]) / na - static_cast<Acc>(b[i]) / nb);
    return l1 * Acc(0.5);
}

// The stored record (POD). Codes are on the 16-bit scale. The picture rect is on the half plane
// (end exclusive) and is an EVIDENCE rect (rows/cols brighter than the frame's own floor), NOT
// Bill's active-picture result.
struct Record {
    uint32_t version = kRecordVersion;
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
    uint32_t hist[kHistBins] = {};
};
static_assert(sizeof(Record) <= kRecordBytes, "the stats record must fit the P4 buffer");

inline bool ReadRecord(const uint8_t* buf, size_t bytes, Record* out)
{
    if (buf == nullptr || out == nullptr || bytes < sizeof(Record))
        return false;
    std::memcpy(out, buf, sizeof(Record));
    return out->version == kRecordVersion;
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
class Accumulator {
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

    void Finish(Record* rec) const
    {
        *rec = Record();
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
        const int i = FirstBinReaching<uint64_t>(hist_, static_cast<int>(kHistBins), target);
        return ((i >= 0 ? static_cast<uint32_t>(i) : kHistBins - 1u) << 6) | 32u;
    }

    uint32_t hw_ = 0, hh_ = 0, bw_ = 0, bh_ = 0;
    uint32_t hist_[kHistBins] = {};
    uint64_t sum_ = 0;
    uint32_t min_ = 0xFFFFu;
    uint32_t max_ = 0;
    std::vector<uint64_t> rowSum_, colSum_, blockSq_;
    std::vector<uint32_t> blockN_;
};

// The one-shot form over a half plane (hw x hh uint16, tightly packed): the record, rest of `out` zero. False on bad input.
inline bool ComputeRecord(const uint16_t* half, uint32_t hw, uint32_t hh, uint8_t* out, size_t outBytes)
{
    if (half == nullptr || out == nullptr || outBytes < sizeof(Record) || hw == 0 || hh == 0)
        return false;
    Accumulator acc;
    acc.Reset(hw, hh);
    acc.AddRows(half, 0, hh);
    Record rec;
    acc.Finish(&rec);
    std::memset(out, 0, outBytes);
    std::memcpy(out, &rec, sizeof(rec));
    return true;
}

// counts[i] / total as `Acc` (the veto signature's normalised histogram); the division is in Acc, as the veto always did.
template <class Acc, class C, class T>
inline void Fractions(const C* counts, size_t n, T total, Acc* out)
{
    for (size_t i = 0; i < n; ++i)
        out[i] = static_cast<Acc>(counts[i]) / total;
}

// A 1024-bin histogram of 10-bit codes (the veto grid's active-rows histogram, per-frame percentile reads).
struct Histogram10 {
    uint32_t bins[kHistBins] = {};
    void Clear() { std::memset(bins, 0, sizeof(bins)); }
    void Add(uint32_t code10) { ++bins[(std::min)(code10, kHistBins - 1u)]; }
    uint64_t CountAtOrAbove(int code) const
    {
        uint64_t n = 0;
        for (int c = (std::max)(code, 0); c < static_cast<int>(kHistBins); ++c)
            n += bins[c];
        return n;
    }
    // First code whose cumulative count reaches `target` (double compare, as the veto's p99.9), or -1.
    int CodeReaching(double target) const { return FirstBinReaching<double>(bins, static_cast<int>(kHistBins), target); }
};

} // namespace SourceStats
} // namespace VpUnit
