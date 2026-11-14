// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// IC-2: the FROZEN scheduler contract (lead8/brief-S0-scheduler.md s2; owner decision 2026-11-12 15:13, refs/owner-decisions-2026-11-07.md:612;
// refs/122). One core-first slot per SOURCE frame: render first, then core shared jobs, derived products, the TD reserved share, extras; the gap
// scheduler only mops up small leftovers. Every scheduled job IS a skeleton product fill (owner correction, refs/122 header). The types and
// rules here change only through lead 8; S1-S4 code against them.
//
// Rules frozen with the types (R1-R10 of the brief; the numbers refer to it):
//  R1  Render first: slot work runs only after the present (R12 AfterPresent); budget = RenderBudget::BudgetMs(measured source rate) =
//      period - render EWMA - period/10. The source rate is the measured captureRateHz, the nominal one only when unmeasured.
//  R2  The slot writes SlotBudget::usedMs (EWMA over source frames, plus the declared TD slice) to Presenter::spareSlotMs, so render-ahead and the
//      slot never promise the same ms twice.
//  R3  Order inside the slot = class, then graphOrder (index in the run-time Graph::nodes), then product id. A job whose inputs are not sealed for
//      the frame is not run (it is Derived-live: filled at first read through Acquire).
//  R4  Core / Unpack are never cap-skipped, never cost-decayed, never shed for time: row-split to fit, else DEFERRED one slot (counted, first in the next).
//  R5  Slice (TD) gets a Configure-time share; only TD's own tier ladder controls it.
//  R6  Fill only in what is left; "fill off" = not admitted this slot, counted + one visible line.
//  R7  Overload order FillOff -> TdStepDown -> ExtrasStepDown -> CoreDeferred -> FeatureOff; ONE arbiter, fixed order, hysteresis (down after
//      2 s over, up after 5 s under u - 0.10), never two controllers on one signal; every step visible (Ctrl+I + Config), never log-only.
//  R8  Never a dropped frame: no slot or overload action may skip, repeat or delay a presentation; the slot never blocks, starts no thread.
//  R9  Low latency: games (1 queued) = backward products only, competitive (0) = no slot work. Original pathway: nothing registered, byte-identical.
//  R10 SharedGap (hidden test switch, removed after the loop grid): the same JobDecl registrations run as IGapWork; TD runs reduced / off, visibly.
//
// Pure C++14, no host types.

#include <VP-V/core/PostPresentSpare.h>

#include <cstdint>

namespace VpSkeleton {

// The ONE render budget (moved here verbatim from motion/estimation/OnDemandEstimate.h, S0 / refs/122 N2; OnDemand::RenderBudget is now an alias of
// this class). One render-time sample per source frame in, the budget a post-present job (or an on-demand request) may use out:
// the source period minus the MEASURED render time (an EWMA, 1/8 per sample) minus a tenth of the period as a margin; never below 0. Never a fixed
// number of milliseconds: until the first sample it is half the period; an unknown rate gives 0 (no budget).
class RenderBudget {
public:
    void Note(double renderMs)
    {
        if (!(renderMs >= 0.0))
            return;   // NaN or negative: ignored
        ewmaMs_ = measured_ ? ewmaMs_ + (renderMs - ewmaMs_) / 8.0 : renderMs;
        measured_ = true;
    }
    bool Measured() const { return measured_; }
    double RenderMs() const { return ewmaMs_; }
    double BudgetMs(double sourceHz) const
    {
        if (!(sourceHz > 0.0))
            return 0.0;
        const double period = 1000.0 / sourceHz;
        if (!measured_)
            return period * 0.5;
        const double b = period - ewmaMs_ - period * 0.1;
        return b > 0.0 ? b : 0.0;
    }
    void Reset() { ewmaMs_ = 0.0; measured_ = false; }

private:
    double ewmaMs_ = 0.0;
    bool measured_ = false;
};

// Fixed class order; the render itself is outside (always first).
enum class JobClass : uint8_t { Unpack = 0, Core = 1, Derived = 2, Slice = 3, Fill = 4 };

// The spare slot's own class of a job class: Derived runs with Core (after its inputs, by graphOrder). PostPresentSpare's enum and tests stay unchanged.
inline SpareClass SpareClassOf(JobClass c)
{
    switch (c) {
    case JobClass::Unpack: return SpareClass::Unpack;
    case JobClass::Core: return SpareClass::Core;
    case JobClass::Derived: return SpareClass::Core;
    case JobClass::Slice: return SpareClass::Slice;
    case JobClass::Fill: return SpareClass::Fill;
    }
    return SpareClass::Fill;
}

struct JobDecl {
    uint16_t product;        // a ProductId: every job IS a skeleton product fill (owner correction, refs/122 header)
    JobClass cls;
    uint32_t graphOrder;     // = index in Graph::nodes (run-time topological order), never hand-set
    bool splittable;
    bool cpu;                // runs in the CPU lane (LaneRunner), not on the render GPU
};

struct JobCost {             // from SpareMeasuredCost; GPU = SUM of all passes of a frame
    double predictMs;
    double lastMs;
    uint32_t samples;
};

struct SlotBudget {          // per source frame
    double periodMs, renderMs, budgetMs, sliceMs, usedMs;
    bool measured;
};

enum class OverloadStep : uint8_t { None = 0, FillOff = 1, TdStepDown = 2, ExtrasStepDown = 3, CoreDeferred = 4, FeatureOff = 5 };

struct OverloadState {
    OverloadStep step;
    uint8_t tdTier;
    int extrasLevel;
    uint32_t deferredCore;
    const char* line;
};

enum class ScheduleMode : uint8_t { CoreFirst = 0, SharedGap = 1 };   // hidden key precompute_schedule; default core_first

}   // namespace VpSkeleton
