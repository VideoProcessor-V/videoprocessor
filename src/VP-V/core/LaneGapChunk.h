// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// THE gap-chunk law of a lane GPU-compute product (one copy, rule 1 / 00-UNIVERSAL; PREC2-S3A audit MUST 2). Both gap adapters call it:
// core/AnalysisLane.h LaneComputeGapWork (today's production gap work) and core/LaneSpareWork.h BasicLaneSpareWork's gap face (S-3a S2).
//   - progress: a Step that Ran, Published, was Late or Abandoned made progress; anything else = nothing to do until the next gap (Wake).
//   - predict:  the larger of the last measured CPU time of a Step and the producer's own device-cost prediction (a GPU-compute Step only
//               records: its CPU time says nothing about the chunk's GPU time).
//   - cap no fit: the measured part halves per gap, so one jittery over-cap Step cannot starve the product.
//   - chunk:    one runner Step per call with the gap's budget; a progressing Step's CPU time becomes the measured part.
// Header-only, pure C++14. The runner is a template parameter (LaneRunner in production, a fake in tests).

#include "LaneRunner.h"

#include <algorithm>
#include <cstdint>
#include <functional>

namespace VpSkeleton {

inline bool LaneStepProgressed(StepOutcome o)
{
    return o == StepOutcome::Ran || o == StepOutcome::Published || o == StepOutcome::Late
           || o == StepOutcome::Abandoned;
}

namespace detail {

// Prefer the 3-arg Step(id, budgetUs, /*completeOnDone*/ true) when the Runner supports it so a product publishes
// immediately on its last chunk; fall back to 2-arg Step(id, budgetUs) for minimal test fake runners.
template <class Runner>
auto StepGapRunner(Runner* runner, ProductId id, uint32_t budgetUs, int)
    -> decltype(runner->Step(id, budgetUs, true))
{
    return runner->Step(id, budgetUs, true);
}

template <class Runner>
StepOutcome StepGapRunner(Runner* runner, ProductId id, uint32_t budgetUs, long)
{
    return runner->Step(id, budgetUs);
}

}   // namespace detail

// The per-product gap state and its law. The owner keeps one and forwards its IGapWork calls here.
struct LaneGapChunk {
    uint32_t lastUs = 0;     // the measured part of the prediction
    bool idle = false;       // nothing to do until Wake
    uint64_t chunks = 0;     // RunChunk calls

    template <class Runner>
    uint32_t PredictUs(Runner* runner, ProductId id) const
    {
        const uint32_t p = runner != nullptr ? runner->PredictChunkUs(id) : 0u;
        return (std::max)(lastUs, p);
    }
    void OnCapNoFit() { lastUs /= 2u; }
    template <class Runner>
    void RunChunk(Runner* runner, ProductId id, uint32_t budgetUs, const std::function<uint64_t()>& nowUs)
    {
        const uint64_t t0 = nowUs ? nowUs() : 0u;
        const StepOutcome o = detail::StepGapRunner(runner, id, budgetUs, 0);
        const uint64_t t1 = nowUs ? nowUs() : 0u;
        ++chunks;
        if (LaneStepProgressed(o)) {
            const uint64_t dt = t1 > t0 ? t1 - t0 : 0u;
            lastUs = dt > 0xFFFFFFFFull ? 0xFFFFFFFFu : uint32_t(dt);
        } else {
            idle = true;
        }
    }
    void Wake() { idle = false; }
};

}   // namespace VpSkeleton
