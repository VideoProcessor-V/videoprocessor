// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// B3-Z Z2 scaffold (TD design 99 s3.3 + s12 item 1; refs 93-prep-B3Z s2): the ONE post-present spare slot of a
// SOURCE frame on the render device (keyed by the source sequence, never the rendered frame: audit C5). Generic: the host passes the slot's budget in (C5b's RenderBudget when it
// lands; until then whatever the host measures, never a constant here) and gets the used ms back. Works run in a
// fixed order: class (Unpack P13, Core P3 / P3b / P3r / P16, Slice TD, Fill), then the work's slot position, then
// its id. First work = the matcher top-up (motion/estimation/LaneAheadPolicy.h); the gap scheduler's second gap
// call goes behind the same hook later (no new LVR line).
//
// Rules (differ from GapScheduler on purpose):
//  - core (and unpack) work is NEVER capNoFit-skipped or cost-decayed: a core work whose prediction does not fit
//    the remaining budget runs a row split that fits (VpLaneProducts::PlanChunks rule, LaneGpuJobs.h), and an
//    unsplittable one is DEFERRED (counted, stays first next slot; its consumer computes at render as today);
//  - a slice work runs inside its Configure-time slice and the remaining budget; fill only in what is left;
//  - no fixed cost: each work predicts from its own measurement (SpareMeasuredCost); an unmeasured work runs only as
//    the first work of a slot (so it is measured once without stacking on other work); an unmeasured unpack /
//    core work deferred behind other work goes FIRST in the next slot that runs (once; then it is measured), so
//    it is never starved by an earlier work that always has work (audit S4);
//  - the key is consumed by whichever outcome the first trigger of a frame gets (Ran, NoBudget, NothingToDo):
//    later triggers of the same source frame count AlreadyRan and never retry it (audit S6: one decision per
//    source frame; the budget a later trigger sees belongs to the same frame);
//  - one slot per SOURCE frame (doc 101 row 11): the frame key is the source sequence of the last distinct source
//    frame (a cadence repeat keeps its predecessor's key, SpareSourceKey), so 24-in-60 runs once per film frame,
//    not per rendered frame; the first trigger point that fires for a key runs it, later ones count AlreadyRan
//    (the triggers: post-present without extras, after the last extra is queued, the plain path before the
//    dequeue wait - matched rates included);
//  - the hook never blocks and starts no thread; the host times each Run (GPU timer or CPU clock).
// Pure C++14, no host types.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

namespace VpSkeleton {

enum class SpareClass : uint8_t { Unpack = 0, Core = 1, Slice = 2, Fill = 3 };

enum class SpareTrigger : uint8_t {
    PostPresentNoExtras = 0,   // LVR post-present else branch (no extras this frame)
    AfterLastExtra = 1,        // after the frame's last extra is queued
    BeforeDequeueWait = 2,     // plain path: before the dequeue wait (matched rates)
    Count
};

enum class SpareOutcome : uint8_t { Ran = 0, Off, AlreadyRan, NoBudget, NothingToDo, Count };

inline const char* SpareTriggerName(SpareTrigger t)
{
    switch (t) {
    case SpareTrigger::PostPresentNoExtras: return "post-present";
    case SpareTrigger::AfterLastExtra: return "after extras";
    case SpareTrigger::BeforeDequeueWait: return "before dequeue";
    default: return "?";
    }
}

inline const char* SpareOutcomeName(SpareOutcome o)
{
    switch (o) {
    case SpareOutcome::Ran: return "ran";
    case SpareOutcome::Off: return "off";
    case SpareOutcome::AlreadyRan: return "already ran this frame";
    case SpareOutcome::NoBudget: return "no spare budget";
    case SpareOutcome::NothingToDo: return "nothing to do";
    default: return "?";
    }
}

// A work's own cost model: an EWMA of its measured ms (0 = not measured). Per-row cost when the work is split.
class SpareMeasuredCost {
public:
    void Note(double ms, uint32_t rows = 0, uint32_t ofRows = 0)
    {
        if (!(ms >= 0.0) || !std::isfinite(ms)) return;
        // A split run is scaled to the whole work so the prediction stays "whole work" in every case.
        const double whole = (rows > 0 && ofRows > rows) ? ms * double(ofRows) / double(rows) : ms;
        m_ms = m_samples == 0 ? whole : m_ms + (whole - m_ms) * kAlpha;
        ++m_samples;
    }
    double PredictMs() const { return m_samples == 0 ? 0.0 : m_ms; }
    uint32_t Samples() const { return m_samples; }
    void Reset() { m_ms = 0.0; m_samples = 0; }
    static constexpr double kAlpha = 0.125;

private:
    double m_ms = 0.0;
    uint32_t m_samples = 0;
};

class ISpareWork {
public:
    virtual ~ISpareWork() {}
    virtual uint16_t Id() const = 0;
    virtual SpareClass Class() const = 0;
    virtual uint32_t Order() const = 0;     // position in the slot's fixed order (TD 99 s3.3 #)
    virtual bool HasWork() const = 0;
    virtual double PredictMs() const = 0;   // what remains, from the work's own measurement; <= 0 = unmeasured
    virtual uint32_t RowsLeft() const { return 0; }   // > 0: splittable in rows (PlanChunks units); 0: one call
    // rows == 0: the whole remaining work; else that many rows from where it stopped. Returns the measured ms it
    // used (>= 0) or < 0 when it failed (the work keeps its own failure reason).
    virtual double Run(uint32_t rows) = 0;
};

struct SpareSlotInput {
    bool enabled = false;
    SpareTrigger trigger = SpareTrigger::PostPresentNoExtras;
    uint64_t frameKey = 0;   // the SOURCE frame (SpareSourceKey; 0 = no once-per-frame check)
    double budgetMs = 0.0;   // the slot's budget, measured by the host
    double sliceMs = 0.0;    // the Slice class's Configure-time share (<= budget)
};

struct SpareReport {
    SpareOutcome outcome = SpareOutcome::Off;
    SpareTrigger trigger = SpareTrigger::PostPresentNoExtras;
    double budgetMs = 0.0;
    double usedMs = 0.0;
    uint32_t ran = 0;            // works run (whole or split)
    uint32_t split = 0;          // of which a row split
    uint32_t unmeasured = 0;     // of which run without a measured cost
    uint32_t coreDeferred = 0;   // unpack / core work that did not fit and could not split (stays first)
    uint32_t sliceSkipped = 0;
    uint32_t fillSkipped = 0;
    uint32_t failed = 0;
};

// Rows of one split chunk that fits `limitMs` (VpLaneProducts::PlanChunks rule: enough chunks that each is
// predicted <= the cap; at least one row, never more than `rows`).
inline uint32_t SpareRowsThatFit(uint32_t rows, double predictedMs, double limitMs)
{
    if (rows == 0) return 0;
    if (!(predictedMs > 0.0) || !(limitMs > 0.0) || predictedMs <= limitMs) return rows;
    double want = predictedMs / limitMs;
    uint64_t count = uint64_t(want);
    if (double(count) < want) ++count;
    if (count < 1u) count = 1u;
    if (count > rows) count = rows;
    return uint32_t((uint64_t(rows) + count - 1u) / count);
}

class PostPresentSpare {
public:
    void Register(ISpareWork* w)
    {
        if (!w) return;
        if (std::find(m_works.begin(), m_works.end(), w) == m_works.end()) m_works.push_back(w);
    }
    void Unregister(ISpareWork* w)
    {
        m_works.erase(std::remove(m_works.begin(), m_works.end(), w), m_works.end());
        if (m_promote == w) m_promote = nullptr;
    }
    size_t WorkCount() const { return m_works.size(); }

    // A new source / resync: the next frame key runs again.
    void Reset() { m_lastKey = 0; }

    SpareReport Run(const SpareSlotInput& in)
    {
        SpareReport r;
        r.trigger = in.trigger;
        r.budgetMs = in.budgetMs;
        if (!in.enabled) return Finish(r, SpareOutcome::Off);
        if (in.frameKey != 0 && in.frameKey == m_lastKey) return Finish(r, SpareOutcome::AlreadyRan);
        m_lastKey = in.frameKey;
        if (!(in.budgetMs > 0.0) || !std::isfinite(in.budgetMs)) return Finish(r, SpareOutcome::NoBudget);

        m_order.clear();
        for (ISpareWork* w : m_works) if (w->HasWork()) m_order.push_back(w);
        if (m_order.empty()) return Finish(r, SpareOutcome::NothingToDo);
        std::sort(m_order.begin(), m_order.end(), &PostPresentSpare::Before);
        if (m_promote) {   // S4: a starved unmeasured unpack / core work goes first this slot
            auto it = std::find(m_order.begin(), m_order.end(), m_promote);
            if (it != m_order.end() && !(m_promote->PredictMs() > 0.0)) {
                std::rotate(m_order.begin(), it, it + 1);
                ++m_promoted;
            }
            m_promote = nullptr;
        }

        double sliceLeft = in.sliceMs > 0.0 && std::isfinite(in.sliceMs) ? (std::min)(in.sliceMs, in.budgetMs) : 0.0;
        for (ISpareWork* w : m_order) {
            const SpareClass cls = w->Class();
            const bool core = cls == SpareClass::Unpack || cls == SpareClass::Core;
            const double remaining = in.budgetMs - r.usedMs;
            const double limit = cls == SpareClass::Slice ? (std::min)(remaining, sliceLeft) : remaining;
            const double pred = w->PredictMs();
            uint32_t rows = 0;   // 0 = whole
            bool run = false;
            if (limit > 0.0) {
                if (!(pred > 0.0)) {
                    run = r.ran == 0 && r.usedMs <= 0.0;   // unmeasured: only as the slot's first work
                    if (run) ++r.unmeasured;
                } else if (pred <= limit) {
                    run = true;
                } else if (w->RowsLeft() > 0) {
                    rows = SpareRowsThatFit(w->RowsLeft(), pred, limit);
                    run = rows > 0;
                    if (run) ++r.split;
                }
            }
            if (!run) {
                if (core && !(pred > 0.0) && !m_promote) m_promote = w;   // S4: first next slot
                if (core) ++r.coreDeferred;
                else if (cls == SpareClass::Slice) ++r.sliceSkipped;
                else ++r.fillSkipped;
                continue;
            }
            const double ms = w->Run(rows);
            ++r.ran;
            if (!(ms >= 0.0)) { ++r.failed; continue; }
            r.usedMs += ms;
            if (cls == SpareClass::Slice) sliceLeft = (std::max)(0.0, sliceLeft - ms);
        }
        return Finish(r, SpareOutcome::Ran);
    }

    unsigned long long Count(SpareOutcome o) const { return m_outcomes[size_t(o)]; }
    unsigned long long Fired(SpareTrigger t) const { return m_triggers[size_t(t)]; }
    unsigned long long CoreDeferred() const { return m_coreDeferred; }
    unsigned long long Promoted() const { return m_promoted; }   // S4: unmeasured works moved first
    double UsedMsTotal() const { return m_usedTotal; }

private:
    static bool Before(const ISpareWork* a, const ISpareWork* b)
    {
        if (a->Class() != b->Class()) return uint8_t(a->Class()) < uint8_t(b->Class());
        if (a->Order() != b->Order()) return a->Order() < b->Order();
        return a->Id() < b->Id();
    }

    SpareReport& Finish(SpareReport& r, SpareOutcome o)
    {
        r.outcome = o;
        ++m_outcomes[size_t(o)];
        if (o == SpareOutcome::Ran) ++m_triggers[size_t(r.trigger)];
        m_coreDeferred += r.coreDeferred;
        m_usedTotal += r.usedMs;
        return r;
    }

    std::vector<ISpareWork*> m_works;
    std::vector<ISpareWork*> m_order;
    uint64_t m_lastKey = 0;
    ISpareWork* m_promote = nullptr;
    unsigned long long m_promoted = 0;
    std::array<unsigned long long, size_t(SpareOutcome::Count)> m_outcomes{};
    std::array<unsigned long long, size_t(SpareTrigger::Count)> m_triggers{};
    unsigned long long m_coreDeferred = 0;
    double m_usedTotal = 0.0;
};

// The spare slot's frame key: the source sequence of the last distinct source frame. Every captured frame has its
// own source sequence; a cadence repeat (24-in-60 pulldown) keeps the key of the frame it repeats. 0 = unknown.
class SpareSourceKey {
public:
    uint64_t Key(uint64_t sourceSequence, bool cadenceRepeat)
    {
        if (sourceSequence == 0) return m_last;
        if (!cadenceRepeat || m_last == 0) m_last = sourceSequence;
        return m_last;
    }
    uint64_t Last() const { return m_last; }
    void Reset() { m_last = 0; }

private:
    uint64_t m_last = 0;
};

// The renderer's side (render thread only; LVR VPV-HOOK call sites): one slot, the key of the frame last
// presented, and the slot budget. The budget is 0 until C5b's RenderBudget lands, so every trigger reports NoBudget
// (counted, visible) and nothing runs; with no work registered every trigger reports Off. Never blocks.
class SpareHost {
public:
    PostPresentSpare& Slot() { return m_slot; }
    const PostPresentSpare& Slot() const { return m_slot; }
    void NotePresented(uint64_t sourceSequence, bool cadenceRepeat) { m_key = m_source.Key(sourceSequence, cadenceRepeat); }
    void SetBudget(double budgetMs, double sliceMs = 0.0) { m_budgetMs = budgetMs; m_sliceMs = sliceMs; }
    // A new source / resync: keys start over.
    void Reset() { m_slot.Reset(); m_source.Reset(); m_key = 0; }

    SpareReport Fire(SpareTrigger trigger)
    {
        SpareSlotInput in;
        in.enabled = m_slot.WorkCount() > 0;
        in.trigger = trigger;
        in.frameKey = m_key;
        in.budgetMs = m_budgetMs;
        in.sliceMs = m_sliceMs;
        if (in.enabled && m_key == 0) {   // no presented source frame yet: never run without a key
            ++m_noKey;
            SpareReport r;
            r.trigger = trigger;
            r.outcome = SpareOutcome::Off;
            m_last = r;
            return r;
        }
        m_last = m_slot.Run(in);
        return m_last;
    }
    const SpareReport& LastReport() const { return m_last; }
    uint64_t Key() const { return m_key; }
    unsigned long long NoKey() const { return m_noKey; }

private:
    PostPresentSpare m_slot;
    SpareSourceKey m_source;
    uint64_t m_key = 0;
    double m_budgetMs = 0.0;
    double m_sliceMs = 0.0;
    SpareReport m_last;
    unsigned long long m_noKey = 0;
};

// A work built from functions (the host's adapters and the tests): it keeps its own SpareMeasuredCost from what Run
// reports, so no slot size is fixed before costs are measured.
class FunctionSpareWork : public ISpareWork {
public:
    FunctionSpareWork(uint16_t id, SpareClass cls, uint32_t order) : m_id(id), m_class(cls), m_order(order) {}
    std::function<bool()> hasWork;
    std::function<uint32_t()> rowsLeft;          // optional: splittable work
    std::function<double(uint32_t rows)> run;   // returns measured ms (< 0 failed)

    uint16_t Id() const override { return m_id; }
    SpareClass Class() const override { return m_class; }
    uint32_t Order() const override { return m_order; }
    bool HasWork() const override { return hasWork && hasWork(); }
    double PredictMs() const override
    {
        const double whole = m_cost.PredictMs();
        const uint32_t left = RowsLeft();
        return (left > 0 && m_rowsWhole > left && whole > 0.0) ? whole * double(left) / double(m_rowsWhole) : whole;
    }
    uint32_t RowsLeft() const override { return rowsLeft ? rowsLeft() : 0; }
    double Run(uint32_t rows) override
    {
        if (!run) return -1.0;
        const uint32_t left = RowsLeft();
        if (left > m_rowsWhole) m_rowsWhole = left;
        const uint32_t ran = left == 0 ? 0u : (rows == 0 || rows > left ? left : rows);
        const double ms = run(rows);
        if (ms >= 0.0) m_cost.Note(ms, ran, m_rowsWhole);
        if (RowsLeft() == 0) m_rowsWhole = 0;   // the work finished: the next one starts its own whole
        return ms;
    }
    const SpareMeasuredCost& Cost() const { return m_cost; }

private:
    uint16_t m_id;
    SpareClass m_class;
    uint32_t m_order;
    SpareMeasuredCost m_cost;
    uint32_t m_rowsWhole = 0;
};

}   // namespace VpSkeleton
