// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// S-3a S2 (lead9/brief-S-3a-scheduler.md s4 S2; refs/122 S2, N4; IC-2 R3, R4, R10): ONE adapter for a lane GPU job that is BOTH an ISpareWork
// (the core-first slot) and an IGapWork (the gap scheduler), so the same JobDecl registration serves both modes (R10: the hidden test key
// precompute_schedule = shared_gap runs the same registrations as gap work). It replaces LaneComputeGapWork's role for lane products once the
// registration in core/AnalysisLane.h moves to JobDecls (that move waits for L-B2, L-B5, L-DET, S-FXDV and L-CORE on integ: brief s4 S2 risks).
//
// Owner intent: OD:612 (1) (2026-11-12 15:13) "core shared jobs -> derived products -> TD reserved share -> extras; gap scheduler only mops
// up small leftovers"; owner correction 2026-11-12T20:01:26Z [cf1d27fa] "Anything shared is shared ..." (every job is a product fill).
//
//  - Slot face: Run(0) steps the runner until the job publishes (or stops progressing); the cost is the larger of the measured CPU time and the
//    producer's own device-cost prediction (a GPU-compute Step only records: its CPU time says nothing about the GPU time), an EWMA per job,
//    never cost-decayed in the slot (R4). One job per source frame: the runner publishes each frame once (compute once).
//  - Gap face: today's LaneComputeGapWork law, unchanged (one chunk per call; the measured part halves on capNoFit, the GAP's starvation rule).
//  - Gap eligibility (S2-T1): in core_first the gap carries only backfill (Fill); Core / Derived / Slice / Unpack never go to RunGap.
//  - StaticClassOf: the class of a lane product until S3's admission table decides it at Configure from measured costs (doc 91 C :79 fixed
//    core set; P10 optional fill until its 4K cost <= 2 ms is proven, coordinator 2026-11-14 08:22 via lead 10 Q4 / doc 91 C :119; P12 is
//    Derived-live: an Acquire live fill, not a job).
// Pure C++14 apart from the runner type (a template parameter, so tests drive a fake).

#include <VP-V/core/LaneInterfaces.h>
#include <VP-V/core/LaneRunner.h>
#include <VP-V/core/SchedulerContract.h>

#include <algorithm>
#include <cstdint>
#include <functional>
#include <utility>

namespace VpSkeleton {

// The class of a product before S3 (static; doc 91 C :79 / :119, Q4). `jobbed` false = no job at all (Derived-live through Acquire).
struct StaticClass {
    JobClass cls;
    bool jobbed;
};

inline StaticClass StaticClassOf(ProductId id)
{
    switch (id) {
    // fixed core set, never shed for time (doc 91 C :79): P1 + P2 (P2m), P7 / P8, P3, P15 / P16, P4 + P11
    case kP1HalfLuma: case kP2VectorPair: case kP2mMatcherPair: case kP3AnalysisField: case kP4SourceStats:
    case kP7CameraFit: case kP8CameraLabels: case kP11RegionBlockStats: case kQualityPattern: case kCodingEvidence:
        return StaticClass{ JobClass::Core, true };
    // derived-ahead (run right after their inputs)
    case kRefinedField:
        return StaticClass{ JobClass::Derived, true };
    // derived-live: filled at first read through Acquire, never a job
    case kP12LightLevel:
        return StaticClass{ JobClass::Derived, false };
    // optional fill, admitted once at Configure (doc 91 C :79): P5, P6, P9, P10 (until proven, :119), P10c
    case kP5MonitorTiles: case kP6EntranceAhead: case kP9CameraSmoothed: case kP10GrainSplit: case kP10cGrainSplitChroma:
        return StaticClass{ JobClass::Fill, true };
    default:
        return StaticClass{ JobClass::Fill, true };
    }
}

// S2-T1: what the gap may carry. core_first = backfill only; shared_gap (R10) = every registration.
inline bool GapEligible(const JobDecl& d, ScheduleMode mode)
{
    return mode == ScheduleMode::SharedGap || d.cls == JobClass::Fill;
}

inline bool LaneStepMadeProgress(StepOutcome o)
{
    return o == StepOutcome::Ran || o == StepOutcome::Published || o == StepOutcome::Late || o == StepOutcome::Abandoned;
}

// ISpareWork::Class() and IGapWork::Class() share a signature with different return types, so one class cannot override both directly: each
// face renames its own (Id() and HasWork() are identical in both and one override serves both bases).
class SpareFace : public ISpareWork {
public:
    SpareClass Class() const final { return SpareClassOfJob(); }
    virtual SpareClass SpareClassOfJob() const = 0;
};
class GapFace : public IGapWork {
public:
    GapClass Class() const final { return GapClassOfJob(); }
    virtual GapClass GapClassOfJob() const = 0;
};

template <class Runner>
class BasicLaneSpareWork : public SpareFace, public GapFace {
public:
    BasicLaneSpareWork(Runner* runner, const JobDecl& decl, std::function<uint64_t()> nowUs, Tier tier = Tier::T0, double heavyMs = 0.0)
        : runner_(runner), decl_(decl), nowUs_(std::move(nowUs)), tier_(tier), heavyMs_(heavyMs)
    {
    }

    const JobDecl& Decl() const { return decl_; }

    // ---- shared ----
    uint16_t Id() const override { return decl_.product; }
    bool HasWork() const override { return !idle_ && runner_ != nullptr && runner_->Laned(decl_.product); }

    // ---- ISpareWork (core-first slot) ----
    SpareClass SpareClassOfJob() const override { return SpareClassOf(decl_.cls); }
    uint32_t Order() const override { return decl_.graphOrder; }
    double PredictMs() const override
    {
        const double device = runner_ != nullptr ? double(runner_->PredictChunkUs(decl_.product)) / 1000.0 : 0.0;
        return (std::max)(slotCost_.PredictMs(), device);
    }
    double Run(uint32_t rows) override
    {
        (void)rows;   // a lane job is one product fill: the producer chunks it itself (deterministic: the bytes never depend on chunking)
        if (runner_ == nullptr) return -1.0;
        const uint64_t t0 = nowUs_ ? nowUs_() : 0u;
        double deviceUs = 0.0;
        bool progressed = false;
        for (int i = 0; i < kMaxStepsPerRun; ++i) {
            const uint32_t pred = runner_->PredictChunkUs(decl_.product);
            const StepOutcome o = runner_->Step(decl_.product, kWholeJobBudgetUs);
            if (!LaneStepMadeProgress(o)) break;
            progressed = true;
            deviceUs += double(pred);
            ++slotSteps_;
            if (o == StepOutcome::Published) { ++slotPublished_; break; }
            if (o != StepOutcome::Ran) break;
        }
        const uint64_t t1 = nowUs_ ? nowUs_() : 0u;
        if (!progressed) { idle_ = true; ++slotIdle_; return 0.0; }   // nothing to do this frame (Wake on the next)
        const double cpuMs = double(t1 > t0 ? t1 - t0 : 0u) / 1000.0;
        const double ms = (std::max)(cpuMs, deviceUs / 1000.0);
        slotCost_.Note(ms);
        ++slotRuns_;
        return ms;
    }

    // ---- IGapWork (gap scheduler; today's LaneComputeGapWork law) ----
    GapClass GapClassOfJob() const override { return GapClass::LaneCompute; }
    Tier WorkTier() const override { return tier_; }
    double HeavyMs() const override { return heavyMs_; }
    uint32_t PredictUs() const override
    {
        const uint32_t p = runner_ != nullptr ? runner_->PredictChunkUs(decl_.product) : 0u;
        return (std::max)(gapLastUs_, p);
    }
    void OnCapNoFit() override { gapLastUs_ /= 2u; }
    void RunChunk(uint32_t budgetUs) override
    {
        const uint64_t t0 = nowUs_ ? nowUs_() : 0u;
        const StepOutcome o = runner_->Step(decl_.product, budgetUs);
        const uint64_t t1 = nowUs_ ? nowUs_() : 0u;
        ++gapChunks_;
        if (LaneStepMadeProgress(o)) {
            const uint64_t dt = t1 > t0 ? t1 - t0 : 0u;
            gapLastUs_ = dt > 0xFFFFFFFFull ? 0xFFFFFFFFu : uint32_t(dt);
        } else {
            idle_ = true;
        }
    }

    void Wake() { idle_ = false; }
    uint64_t GapChunks() const { return gapChunks_; }
    uint64_t SlotRuns() const { return slotRuns_; }
    uint64_t SlotSteps() const { return slotSteps_; }
    uint64_t SlotPublished() const { return slotPublished_; }
    const SpareMeasuredCost& SlotCost() const { return slotCost_; }

    static constexpr int kMaxStepsPerRun = 64;               // bound per slot call (a job publishes long before: the producer chunks by budget)
    static constexpr uint32_t kWholeJobBudgetUs = 0xFFFFFFFFu;   // the slot asks for the whole job; the slot itself decided it fits

    ISpareWork* AsSpare() { return this; }
    IGapWork* AsGap() { return this; }

private:
    Runner* runner_;
    JobDecl decl_;
    std::function<uint64_t()> nowUs_;
    Tier tier_;
    double heavyMs_;
    bool idle_ = false;
    uint32_t gapLastUs_ = 0;
    uint64_t gapChunks_ = 0;
    SpareMeasuredCost slotCost_;
    uint64_t slotRuns_ = 0, slotSteps_ = 0, slotPublished_ = 0, slotIdle_ = 0;
};

// The production form over the lane runner (brief interface: LaneSpareWork(LaneRunner*, const JobDecl&, nowUs)).
using LaneSpareWork = BasicLaneSpareWork<LaneRunner>;

}   // namespace VpSkeleton
