// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "ProductGraph.h"
#include "ProductSlots.h"

#include <cstdint>

// Skeleton S3: the seal point. Where the seal sits relative to the presented frame, whether the lane
// can be on time for it, and the bounded deterministic wait (coordinator decision N1). Pure functions
// of their inputs; no clocks here (the caller passes how long it has waited).
namespace VpSkeleton {

// The frame whose render starts while `presentSeq` is presented; the seal is at that render start.
inline uint64_t RenderStartSeq(uint64_t presentSeq, uint8_t renderAhead)
{
    return presentSeq + renderAhead;
}

struct LeadInput {
    uint64_t presentSeq = 0;
    uint64_t newestQueuedSeq = 0;
    uint8_t renderAhead = 0;   // R
    uint8_t laneLead = 1;      // L
    QueueMode mode = QueueMode::Normal;
};

enum class LeadState : uint8_t { Ok, Shallow, BackwardOnArrival };

inline LeadState CheckLead(const LeadInput& in)
{
    if (in.mode == QueueMode::Competitive || in.laneLead == 0)
        return LeadState::BackwardOnArrival;
    const uint64_t ahead = in.newestQueuedSeq > in.presentSeq ? in.newestQueuedSeq - in.presentSeq : 0u;
    return ahead >= uint64_t(in.laneLead) + uint64_t(in.renderAhead) ? LeadState::Ok : LeadState::Shallow;
}

inline const char* LeadText(LeadState s)
{
    switch (s) {
    case LeadState::Ok: return "lane on time";
    case LeadState::Shallow: return "queue too shallow for the lane: products made in the render";
    case LeadState::BackwardOnArrival: return "backward pairs on arrival";
    }
    return "?";
}

// N1: a deterministic run (or an explicit test wait) may hold the seal a bounded time for a lane that
// is still working on frame N whose in-render fallback would differ, so the content never depends on
// timing. The bound never causes a late present.
struct WaitPolicy {
    bool deterministicRun = false;
    bool explicitTestWait = false;
    uint32_t presentSlackUs = 0;
    uint32_t guardUs = 0;
    uint32_t testWaitUs = 0;
};

enum class WaitDecision : uint8_t { SealNow, Wait, BoundMissed };

inline WaitDecision DecideWait(Variants variants, bool laneGranted, SlotState stateAtN, const WaitPolicy& policy,
                               uint32_t waitedUs, uint32_t* waitMoreUs)
{
    if (waitMoreUs)
        *waitMoreUs = 0;
    const bool working = stateAtN == SlotState::Writing || stateAtN == SlotState::Empty
                         || stateAtN == SlotState::Released;
    if (!(policy.deterministicRun && variants == Variants::NonIdentical && laneGranted && working))
        return WaitDecision::SealNow;
    const uint32_t bound = policy.explicitTestWait
                               ? policy.testWaitUs
                               : (policy.presentSlackUs > policy.guardUs ? policy.presentSlackUs - policy.guardUs : 0u);
    if (waitedUs >= bound)
        return WaitDecision::BoundMissed;
    if (waitMoreUs) {
        const uint32_t remaining = bound - waitedUs;
        *waitMoreUs = remaining < 500u ? remaining : 500u;
    }
    return WaitDecision::Wait;
}

} // namespace VpSkeleton
