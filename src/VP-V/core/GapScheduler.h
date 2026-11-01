// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "LaneInterfaces.h"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <map>
#include <vector>

// Skeleton S4: the render thread's idle-gap scheduler (design B6). Work shares the gap in fixed class
// priority (render-ahead extras, early-upload wraps, lane compute); within lane compute by tier, then
// heavier first, then id. A chunk is only started when its prediction fits the chunk cap and the time
// left; a smaller lower-priority chunk may backfill. Learned costs only decide WHEN work runs, never
// what a product contains. Render thread only; clocks are injected (microseconds).
namespace VpSkeleton {

struct GapReport {
    uint32_t chunks = 0;
    uint64_t usedUs[3] = { 0, 0, 0 };   // per GapClass: measured time of the chunks run
    uint32_t overruns = 0;              // chunks that took longer than the time left at their start
    uint64_t overrunUs = 0;             // sum of (actual - remaining) over those chunks
    bool laneStarved = false;           // lane compute had work but got no chunk in this gap
    uint32_t laneStarvedGaps = 0;       // cumulative form: gaps in which the lane was starved
    uint32_t renderAheadNoFit = 0;      // render-ahead extras whose next chunk did not fit (once per id per gap)
};

class GapScheduler {
public:
    void Add(IGapWork* w) { if (w) m_works.push_back(w); }
    void SetChunkCapUs(uint32_t capUs) { m_capUs = capUs; }
    uint32_t ChunkCapUs() const { return m_capUs; }

    // settingUs > 0 wins; otherwise about period/8 clamped to [100, 1000] us (no fixed rate assumed).
    static uint32_t DerivedChunkCapUs(double framePeriodMs, uint32_t settingUs)
    {
        if (settingUs > 0) return settingUs;
        double us = framePeriodMs * 1000.0 / 8.0;
        if (!(us >= 100.0)) us = 100.0;     // also catches NaN / non-positive periods
        if (us > 1000.0) us = 1000.0;
        return static_cast<uint32_t>(us);
    }

    GapReport RunGap(uint64_t gapEndUs, const std::function<uint64_t()>& nowUs)
    {
        GapReport r;
        if (!nowUs) return r;
        std::vector<IGapWork*> order;
        std::vector<uint16_t> noFitSeen;
        bool laneHadWork = false;
        bool laneRan = false;
        // Bounded: a clock that never advances must not spin forever.
        for (uint32_t iter = 0; iter < kMaxChunksPerGap; ++iter) {
            const uint64_t before = nowUs();
            if (before >= gapEndUs) break;
            const uint64_t remaining = gapEndUs - before;
            const uint64_t limit = std::min<uint64_t>(m_capUs, remaining);

            order.clear();
            for (IGapWork* w : m_works) if (w->HasWork()) order.push_back(w);
            std::sort(order.begin(), order.end(), &GapScheduler::Before);
            for (IGapWork* w : order)
                if (w->Class() == GapClass::LaneCompute) { laneHadWork = true; break; }

            IGapWork* pick = nullptr;
            for (IGapWork* w : order) {
                if (Predict(w) <= limit) { pick = w; break; }
                if (w->Class() == GapClass::RenderAheadExtra &&
                    std::find(noFitSeen.begin(), noFitSeen.end(), w->Id()) == noFitSeen.end()) {
                    noFitSeen.push_back(w->Id());
                    ++r.renderAheadNoFit;
                }
            }
            if (!pick) break;

            pick->RunChunk(static_cast<uint32_t>(limit));
            const uint64_t after = nowUs();
            const uint64_t actual = after > before ? after - before : 0;

            Learned& l = LearnedFor(pick);
            l.emaUs = static_cast<uint64_t>(static_cast<int64_t>(l.emaUs) +
                (static_cast<int64_t>(actual) - static_cast<int64_t>(l.emaUs)) / 4);

            ++r.chunks;
            r.usedUs[static_cast<size_t>(pick->Class())] += actual;
            if (pick->Class() == GapClass::LaneCompute) laneRan = true;
            if (actual > remaining) {
                ++r.overruns;
                r.overrunUs += actual - remaining;
            }
        }
        r.laneStarved = laneHadWork && !laneRan;
        if (r.laneStarved) r.laneStarvedGaps = 1;
        Accumulate(r);
        return r;
    }

    const GapReport& Totals() const { return m_totals; }

    // Scheduling-only learned cost for a work id (0 when never run).
    uint64_t LearnedUs(uint16_t id) const
    {
        const auto it = m_learned.find(id);
        return it == m_learned.end() ? 0 : it->second.emaUs;
    }

private:
    static const uint32_t kMaxChunksPerGap = 1u << 16;

    struct Learned { uint64_t emaUs = 0; };

    static bool Before(const IGapWork* a, const IGapWork* b)
    {
        if (a->Class() != b->Class()) return a->Class() < b->Class();
        if (a->Class() == GapClass::LaneCompute) {
            if (a->WorkTier() != b->WorkTier()) return a->WorkTier() < b->WorkTier();
            if (a->HeavyMs() != b->HeavyMs()) return a->HeavyMs() > b->HeavyMs();
        }
        return a->Id() < b->Id();
    }

    Learned& LearnedFor(IGapWork* w)
    {
        auto it = m_learned.find(w->Id());
        if (it == m_learned.end()) {
            Learned l;
            l.emaUs = w->PredictUs();   // start = the first prediction
            it = m_learned.insert(std::make_pair(w->Id(), l)).first;
        }
        return it->second;
    }

    uint64_t Predict(IGapWork* w)
    {
        const uint64_t p = w->PredictUs();
        const auto it = m_learned.find(w->Id());
        return it == m_learned.end() ? p : std::max<uint64_t>(p, it->second.emaUs);
    }

    void Accumulate(const GapReport& r)
    {
        m_totals.chunks += r.chunks;
        for (int i = 0; i < 3; ++i) m_totals.usedUs[i] += r.usedUs[i];
        m_totals.overruns += r.overruns;
        m_totals.overrunUs += r.overrunUs;
        m_totals.laneStarved = m_totals.laneStarved || r.laneStarved;
        m_totals.laneStarvedGaps += r.laneStarvedGaps;
        m_totals.renderAheadNoFit += r.renderAheadNoFit;
    }

    std::vector<IGapWork*> m_works;
    std::map<uint16_t, Learned> m_learned;   // ordered by id: never an unordered container in decisions
    uint32_t m_capUs = 1000;
    GapReport m_totals;
};

} // namespace VpSkeleton
