// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// The QUEUE WINDOW: which source frames are still "in the queue" for the skeleton (slice A of lead10/design-queue-keyed-products.md
// s2.2, amendment 2 s9.0 A1-A5). Owner 2026-11-14 08:33 EST: "Skeleton products have to be attached to the specific frame in the
// queue and be available when demanded earlier in the queue. Not all will be computed ahead, but that is what the scheduler is for."
// ONE derived object for the memo (lifetime, admission), the lane claim and the vector-engine lease map; no consumer keeps its own
// constant (coordinator 08:35).
//
// Every term is MEASURED or DERIVED, never configured (00-UNIVERSAL rule 6):
//   R renderAhead    Lifecycle::QueueDepth: frames the render-ahead plan keeps ahead of the present, rounded up
//   P presentLag     render seq - the last presented source seq, the highest sample of the SAME window the lane lead uses
//   H history        the largest input LAG edge onto a memo / lane product + the graph's maxSealLag + the owner fills' declared
//                    historyFrames (HistoryTermOf). NEVER Node::historyFull: record histories reach 223 frames (amendment M2).
//   L laneLead       the ONE published lane lead (the skeleton's min-window, RingSlotBudget-capped value; no second smoothing)
//   W lookAheadWalk  motion_ahead_frames (SettingsApplied): frames the walk covers BEYOND the queue (FramesInFlight.h)
// QueueFrames(R, L, W) = R + L + W is THE queue-frames derivation; QueueFramesFor (Products.h) and FramesInFlight's R + L + W part
// call it (amendment M1: no second, conflicting count).
// back  = H + max(R, P): frames behind the render frame still in the render-ahead / presenter queue, plus the history readers. R is
//         the planned lead of the queue the presenter drains, P what was actually presented (it contains the presents in flight):
//         the same frames, so the max, not the sum.
// ahead = L + W: the frames queued ahead plus the walk beyond them (amendment A1: the sum, max withdrawn).
// Frames() = back + 1 + ahead (the +1 is the frame being sealed; no other margin). An undelivered term is 0 and named "not fed".
// aheadCompute (amendment 3, OWNER question window 2026-11-14 ~09:10 / ~09:15 via the coordinator; supersedes the 11-08 "the lane
// runs the full queue depth"): how far ahead work is COMPUTED, separate from how many frames are QUEUED:
//   aheadCompute = min(max over ACTIVE consumers of their declared ahead need, kAheadCeiling, queue ahead); 0 with no active consumer.
// The queue terms (QueueFrames) and the keep window (the full queue back) do not change; a frame further down the queue is computed
// once when it comes within aheadCompute (get-or-compute-and-publish at that point).
// Header-only, C++14, no Windows, no GPU API.

#include "AheadCeiling.h"
#include "DelayBudget.h"
#include "ProductGraph.h"
#include "QueueFrames.h"
#include "SchedulerContract.h"
#include "VariantKey.h"

#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <initializer_list>
#include <string>
#include <utility>
#include <vector>

namespace VpSkeleton {

// The owner's ceiling on ahead COMPUTE (amendment 3), kAheadCeiling: AheadCeiling.h (one value for the window, S-3a's PlanAhead and the
// motion look-ahead lead). An upper bound, not a target (S-B may plan lower). The only named depth.

// What binds aheadCompute (shown: "ahead compute N (by <consumer>; ceiling 6 | budget | queue)").
enum class AheadBinding : uint8_t { NoConsumer, Consumer, Ceiling, Queue, Budget };

inline const char* Token(AheadBinding b)
{
    switch (b) {
    case AheadBinding::NoConsumer: return "no active ahead consumer";
    case AheadBinding::Consumer: return "consumer";
    case AheadBinding::Ceiling: return "ceiling";
    case AheadBinding::Queue: return "queue";
    case AheadBinding::Budget: return "budget";
    }
    return "?";
}

struct QueueWindowTerms {
    enum : uint8_t { kRenderAhead = 1, kPresentLag = 2, kHistory = 4, kLaneLead = 8, kLookAhead = 16, kAheadNeed = 32,
                     kQueue = 31, kAll = 63 };
    uint16_t renderAhead = 0;    // R
    uint16_t presentLag = 0;     // P
    uint16_t history = 0;        // H
    uint16_t laneLead = 0;       // L
    uint16_t lookAheadWalk = 0;  // W
    // The largest ahead need any ACTIVE consumer declared (amendment 3) and who declared it; not wired = no active ahead consumer.
    uint16_t aheadNeed = 0;
    const char* aheadNeedBy = "";
    uint8_t wired = 0;           // the terms delivered
};

struct QueueWindow {
    uint64_t current = 0;        // the render frame the window was derived at
    uint16_t back = 0;           // H + max(R, P)
    uint16_t ahead = 0;          // L + W (frames QUEUED ahead: admission, keep)
    uint16_t aheadCompute = 0;   // min(declared need, kAheadCeiling, ahead): how far ahead work is COMPUTED (amendment 3)
    AheadBinding aheadBinding = AheadBinding::NoConsumer;
    const char* aheadBy = "";    // the binding consumer (when it is the consumer's need)
    uint16_t history = 0;        // H
    uint16_t renderAhead = 0;    // R (the no-present release edge is current - (H + R))
    bool presentKnown = false;   // P was delivered (the presented edge can be used)
    uint64_t presented = 0;      // the last presented source seq (valid when presentKnown)
    uint64_t epoch = 0;          // the invalidation epoch (a discontinuity bumps it)
    uint8_t wired = 0;
    uint32_t Frames() const { return uint32_t(back) + 1u + uint32_t(ahead); }
};

inline uint16_t Max16(uint16_t a, uint16_t b) { return a > b ? a : b; }
inline uint16_t Sat16(uint32_t v) { return v > 0xFFFFu ? uint16_t(0xFFFFu) : uint16_t(v); }

// Undelivered terms read 0 (their floor) and are named by NotFed.
inline QueueWindow DeriveWindow(const QueueWindowTerms& t, uint64_t current, uint64_t epoch)
{
    const uint16_t R = (t.wired & QueueWindowTerms::kRenderAhead) ? t.renderAhead : uint16_t(0);
    const uint16_t P = (t.wired & QueueWindowTerms::kPresentLag) ? t.presentLag : uint16_t(0);
    const uint16_t H = (t.wired & QueueWindowTerms::kHistory) ? t.history : uint16_t(0);
    const uint16_t L = (t.wired & QueueWindowTerms::kLaneLead) ? t.laneLead : uint16_t(0);
    const uint16_t W = (t.wired & QueueWindowTerms::kLookAhead) ? t.lookAheadWalk : uint16_t(0);
    QueueWindow w;
    w.current = current;
    w.history = H;
    w.renderAhead = R;
    w.back = Sat16(uint32_t(H) + uint32_t(Max16(R, P)));
    w.ahead = Sat16(QueueFrames(0, L, W));
    // amendment 3: the computed depth, its binding term named.
    if (!(t.wired & QueueWindowTerms::kAheadNeed) || t.aheadNeed == 0) {
        w.aheadCompute = 0;
        w.aheadBinding = AheadBinding::NoConsumer;
    } else {
        uint16_t c = t.aheadNeed;
        AheadBinding b = AheadBinding::Consumer;
        if (kAheadCeiling < c) {
            c = kAheadCeiling;
            b = AheadBinding::Ceiling;
        }
        if (w.ahead < c) {
            c = w.ahead;
            b = AheadBinding::Queue;
        }
        w.aheadCompute = c;
        w.aheadBinding = b;
        w.aheadBy = t.aheadNeedBy != nullptr ? t.aheadNeedBy : "";
    }
    w.presentKnown = (t.wired & QueueWindowTerms::kPresentLag) != 0;
    w.presented = w.presentKnown && current >= P ? current - P : 0;
    w.epoch = epoch;
    w.wired = t.wired;
    return w;
}

// H from a graph (amendment A2): the largest input LAG edge of any node, plus the graph's maxSealLag, plus the owner fills' declared
// history. A product's OWN record history (Node::history / historyFull, up to 223 frames) is not a reader lag and never counts.
// Null graph = no lag edges (the fills' history and nothing else).
inline uint16_t HistoryTermOf(const Graph* g, uint16_t fillHistory)
{
    uint32_t lag = 0;
    uint32_t sealLag = 0;
    if (g != nullptr) {
        for (const Node& n : g->nodes)
            for (uint8_t i = 0; i < n.inputCount && i < n.inputs.size(); ++i)
                if (n.inputs[i].lag > lag)
                    lag = n.inputs[i].lag;
        sealLag = g->maxSealLag;
    }
    const uint32_t edge = lag > fillHistory ? lag : fillHistory;
    return Sat16(edge + sealLag);
}

// The terms not delivered, for the status lines ("" = all fed).
inline std::string NotFed(uint8_t wired)
{
    std::string out;
    auto add = [&out](const char* n) { out += (out.empty() ? "" : ", ") + std::string(n); };
    if (!(wired & QueueWindowTerms::kRenderAhead)) add("render-ahead depth");
    if (!(wired & QueueWindowTerms::kPresentLag)) add("present facts");
    if (!(wired & QueueWindowTerms::kHistory)) add("history");
    if (!(wired & QueueWindowTerms::kLaneLead)) add("lane lead");
    if (!(wired & QueueWindowTerms::kLookAhead)) add("look-ahead walk");
    if (!(wired & QueueWindowTerms::kAheadNeed)) add("ahead consumers");
    return out;
}

// "ahead compute N (by <consumer>; ceiling 6 | budget | queue)" (amendment 3; Ctrl+Alt+D always, Ctrl+I / Config when a cap binds).
inline std::string AheadComputeText(const QueueWindow& w)
{
    std::string by = w.aheadBinding == AheadBinding::NoConsumer ? std::string(Token(AheadBinding::NoConsumer))
                                                                 : "by " + std::string(w.aheadBy != nullptr && w.aheadBy[0] ? w.aheadBy : "consumer");
    std::string bind;
    if (w.aheadBinding == AheadBinding::Ceiling)
        bind = "; ceiling " + std::to_string(kAheadCeiling);
    else if (w.aheadBinding == AheadBinding::Queue)
        bind = "; queue";
    else if (w.aheadBinding == AheadBinding::Budget)
        bind = "; budget";
    return "ahead compute " + std::to_string(w.aheadCompute) + " (" + by + bind + ")";
}
// The largest declared ahead need of the ACTIVE consumers (amendment 3) and who declared it, for QueueWindowTerms::aheadNeed /
// aheadNeedBy. Empty = no active ahead consumer (need 0).
struct AheadDemand { uint16_t need = 0; const char* consumer = ""; };
inline AheadDemand MaxAheadNeed(const std::vector<AheadDemand>& active)
{
    AheadDemand best;
    for (const AheadDemand& d : active)
        if (d.need > best.need)
            best = d;
    return best;
}
// Feeds the ACTIVE consumers' declared needs into the terms (the largest one and who declared it; none = no active ahead consumer).
inline void SetAheadConsumers(QueueWindowTerms& t, const std::vector<AheadDemand>& active)
{
    const AheadDemand d = MaxAheadNeed(active);
    t.aheadNeed = d.need;
    t.aheadNeedBy = d.consumer != nullptr ? d.consumer : "";
    t.wired = uint8_t(t.wired | QueueWindowTerms::kAheadNeed);
}

// Slice C wiring (lead 11; OD 11-14, design s10): what the window hands S-3a's PlanAhead (core/AdmissionTable.h AdmissionFacts::
// aheadFeed, this struct itself: no parallel consumer list there), so the scheduler computes the SAME ahead-compute limit as the window:
// the same active consumers with their declared needs, the same queue frames ahead (the window's ahead, the bound DeriveWindow applies)
// and the same ceiling (AheadCeiling.h). The window's own aheadCompute rides along and S-3a's AheadCompute checks both agree before any
// mode or budget step (a difference is drift, shown on its Ctrl+Alt+D line); S-3a's games / competitive zero and its S-B / S-C then only
// lower it, each shown.
// R3 (lead 12): the feed carries BOTH depths, each named for what it is:
//   queueAheadFrames   = L + W, the frames queued ahead (the window's ahead: the bound DeriveWindow applies; not L alone);
//   aheadComputeLimit  = min(max active consumer request, kAheadCeiling 6, queueAheadFrames): how far ahead work is COMPUTED.
struct PlanAheadFeed {
    std::vector<AheadDemand> activeConsumers;   // each active ahead consumer and its declared request (need 0 = not listed)
    uint16_t queueAheadFrames = 0;              // L + W: the frames queued ahead (the window's ahead)
    uint16_t aheadCeiling = kAheadCeiling;      // the one ceiling (never a second copy at the call site)
    uint16_t aheadComputeLimit = 0;             // the window's aheadCompute (before any plan): min(request, ceiling, queue)
};
inline PlanAheadFeed PlanAheadFeedOf(const QueueWindow& w, const std::vector<AheadDemand>& active)
{
    PlanAheadFeed f;
    for (const AheadDemand& d : active)
        if (d.need > 0)
            f.activeConsumers.push_back(d);
    f.queueAheadFrames = w.ahead;
    f.aheadComputeLimit = w.aheadCompute;
    return f;
}
// The ahead-compute limit applied to a feed (the law S-3a's AheadCompute implements; the wiring's drift check compares the two).
inline uint16_t AheadComputeLimitOf(const PlanAheadFeed& f)
{
    uint16_t need = MaxAheadNeed(f.activeConsumers).need;
    if (need > f.aheadCeiling)
        need = f.aheadCeiling;
    return need < f.queueAheadFrames ? need : f.queueAheadFrames;
}

// True when a cap (ceiling, queue or budget) binds: the Ctrl+I / Config status line is shown (no silent cap).
inline bool AheadComputeCapped(const QueueWindow& w)
{
    return w.aheadBinding == AheadBinding::Ceiling || w.aheadBinding == AheadBinding::Queue || w.aheadBinding == AheadBinding::Budget;
}

// "memo window: back B (render-ahead R, presented lag P, history H), ahead A (lane lead L, look-ahead W), frames F" (not fed named).
inline std::string WindowText(const QueueWindowTerms& t, const QueueWindow& w)
{
    auto term = [&t](uint8_t bit, uint16_t v) { return (t.wired & bit) ? std::to_string(v) : std::string("not fed"); };
    return "memo window: back " + std::to_string(w.back) + " (render-ahead " + term(QueueWindowTerms::kRenderAhead, t.renderAhead)
           + ", presented lag " + term(QueueWindowTerms::kPresentLag, t.presentLag) + ", history "
           + term(QueueWindowTerms::kHistory, t.history) + "), ahead " + std::to_string(w.ahead) + " (lane lead "
           + term(QueueWindowTerms::kLaneLead, t.laneLead) + ", look-ahead " + term(QueueWindowTerms::kLookAhead, t.lookAheadWalk)
           + "), frames " + std::to_string(w.Frames()) + "; " + AheadComputeText(w);
}

// The highest sample over the last `windowUs` (the P term: the same window the lane lead's min-window uses, its length passed by the
// caller). A sustained fall is taken once the window has passed; a rise at once. One writer (the render thread).
// Single-threaded (the render thread's Begin). The LAST sample is kept however old: a feed that stops (a stall) keeps its value
// until a new sample arrives, on purpose (the window must not shrink under a stalled present).
class WindowPeak {
public:
    explicit WindowPeak(uint64_t windowUs) : windowUs_(windowUs) {}
    void Sample(uint64_t nowUs, uint16_t v)
    {
        while (!samples_.empty() && samples_.back().v <= v)
            samples_.pop_back();
        samples_.push_back(S{ nowUs, v });
        Expire(nowUs);
    }
    uint16_t Value(uint64_t nowUs)
    {
        Expire(nowUs);
        return samples_.empty() ? uint16_t(0) : samples_.front().v;
    }
    bool Any() const { return !samples_.empty(); }

private:
    struct S { uint64_t us; uint16_t v; };
    void Expire(uint64_t nowUs)
    {
        while (samples_.size() > 1 && nowUs > samples_.front().us && nowUs - samples_.front().us > windowUs_)
            samples_.pop_front();
    }
    uint64_t windowUs_;
    std::deque<S> samples_;   // decreasing values, increasing times
};

// The per-host snapshot the skeleton writes once per Begin and any thread reads (the vector-engine lease map, the scheduler).
// A seqlock over plain fields: a reader never sees a torn window. ONE writer (the render thread's Begin); any number of readers.
class QueueWindowBoard {
public:
    void Publish(const QueueWindow& w)
    {
        const uint64_t v = version_.load(std::memory_order_relaxed);
        version_.store(v + 1u, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_release);
        current_.store(w.current, std::memory_order_relaxed);
        back_.store(w.back, std::memory_order_relaxed);
        ahead_.store(w.ahead, std::memory_order_relaxed);
        history_.store(w.history, std::memory_order_relaxed);
        renderAhead_.store(w.renderAhead, std::memory_order_relaxed);
        aheadCompute_.store(w.aheadCompute, std::memory_order_relaxed);
        aheadBinding_.store(uint8_t(w.aheadBinding), std::memory_order_relaxed);
        presented_.store(w.presentKnown ? w.presented + 1u : 0u, std::memory_order_relaxed);
        epoch_.store(w.epoch, std::memory_order_relaxed);
        wired_.store(w.wired, std::memory_order_relaxed);
        version_.store(v + 2u, std::memory_order_release);
        published_.store(true, std::memory_order_release);
    }
    // False before the first Publish (or a writer kept it busy): no window rather than a torn one.
    bool Read(QueueWindow* out) const
    {
        if (!published_.load(std::memory_order_acquire) || out == nullptr)
            return false;
        for (int attempt = 0; attempt < 64; ++attempt) {
            const uint64_t v0 = version_.load(std::memory_order_acquire);
            if (v0 & 1u)
                continue;
            QueueWindow w;
            w.current = current_.load(std::memory_order_relaxed);
            w.back = back_.load(std::memory_order_relaxed);
            w.ahead = ahead_.load(std::memory_order_relaxed);
            w.history = history_.load(std::memory_order_relaxed);
            w.renderAhead = renderAhead_.load(std::memory_order_relaxed);
            w.aheadCompute = aheadCompute_.load(std::memory_order_relaxed);
            w.aheadBinding = AheadBinding(aheadBinding_.load(std::memory_order_relaxed));
            const uint64_t p = presented_.load(std::memory_order_relaxed);
            w.presentKnown = p != 0;
            w.presented = p != 0 ? p - 1u : 0u;
            w.epoch = epoch_.load(std::memory_order_relaxed);
            w.wired = wired_.load(std::memory_order_relaxed);
            std::atomic_thread_fence(std::memory_order_acquire);
            if (version_.load(std::memory_order_relaxed) == v0) {
                *out = w;
                return true;
            }
        }
        return false;
    }

private:
    std::atomic<uint64_t> version_{ 0 };
    std::atomic<bool> published_{ false };
    std::atomic<uint64_t> current_{ 0 }, presented_{ 0 }, epoch_{ 0 };
    std::atomic<uint16_t> back_{ 0 }, ahead_{ 0 }, history_{ 0 }, renderAhead_{ 0 }, aheadCompute_{ 0 };
    std::atomic<uint8_t> wired_{ 0 }, aheadBinding_{ 0 };
};

// ---- The S3 admission link (design s2.7, amendment A5; OWNER Q2 2026-11-14, verbatim: "Less compute ahead +/- shorten queue length
// or reduce what's calculated early") ----------------------------------------------------------------------------------------------
// A memory shortage is PLANNED away before frames are demanded, never met by eviction. The memo describes what its ahead work costs
// (SkeletonMemo::MakeAheadNeed); the scheduler's Configure-time admission (S-3a S3) plans the depth per product with THE ladder below
// (S3's PlanAhead calls PlanAheadLadder: one copy of the law); the memo enforces the plan (SkeletonMemo::SetAheadPlan: beyond its
// planned depth = AcquireCause::NotPlannedAhead, made at its render) and asks the VRAM ledger for ONE LEVEL PER LADDER STEP
// (SkeletonMemo::GpuNeedLevels), so a short grant gives the largest plan that fits, never 0 (amendment M6).

// The class of an ahead job is the scheduler contract's VpSkeleton::JobClass (core/SchedulerContract.h, IC-2: Unpack, Core, Derived,
// Slice, Fill); there is one enum. The ladder's shed order is written out below, never taken from the enum values: S-A sheds Fill
// first, then Derived, then the ahead part of the TD Slice; Core and Unpack are never shed (R4), they keep at least the depth their
// consumers declared (S-B).
inline const char* Token(JobClass c)
{
    switch (c) {
    case JobClass::Unpack: return "unpack";
    case JobClass::Fill: return "fill";
    case JobClass::Derived: return "derived";
    case JobClass::Slice: return "slice";
    case JobClass::Core: return "core";
    }
    return "?";
}

// Core and Unpack are never shed for memory (SchedulerContract R4): S-B shrinks them only down to their declared depth.
inline bool KeepsDeclaredDepth(JobClass c) { return c == JobClass::Core || c == JobClass::Unpack; }

struct AheadCost {
    uint16_t id = 0;                // ProductId
    uint8_t form = 0;               // AcquireForm (0 CPU, 1 GPU)
    VariantKey variant;             // the variant costed (owner Q3: luma now, chroma when asked are separate costs; audit S11a)
    uint64_t bytesPerFrame = 0;     // GPU bytes one frame's spot holds (declared reserve, or measured when `measured`)
    double msPerFrame = 0.0;        // compute cost from the cost ledger (0 = unknown)
    bool measured = false;
    JobClass cls = JobClass::Core;
    uint16_t declaredDepth = 0;     // the ahead depth the consumers declared (Core keeps at least this)
};

// S-C asks the DELAY BUDGET for its next LOWER ladder step (core/DelayBudget.h is the ONE owner of the queue length, ADR-0020; design
// s2.7 S10): the same budget, so the TOTAL delay is unchanged (the padding makes it up) and look-ahead is lost; never below
// DelayBudget::kMinimumDepth. The ladder never does its own queue arithmetic. False = no lower step. The default is
// DelayBudget::NextLowerQueueFrames; a caller may wrap it (to count or log the steps), never replace the law.
using LowerQueueStep = std::function<bool(const DelayBudget::Inputs& budget, int currentFrames, DelayBudget::Plan* lower)>;

struct AheadNeed {
    QueueWindow window;
    uint64_t grantBytes = 0;          // the memo's VRAM ledger grant
    uint64_t heldBytes = 0;           // GPU bytes held now
    std::vector<AheadCost> costs;
    QueueMode mode = QueueMode::Normal;   // Games / Competitive: S-C is a no-op (their queues are already short), never a raise
    // The delay budget the scheduler plans the queue with (the user's one total delay and the MEASURED rates). Not fed = no S-C
    // step (the floor line says so). The 200 ms AVR-safe tier is its budgetMs: a ceiling the plan never exceeds, never a floor.
    DelayBudget::Inputs budget;
    bool budgetFed = false;
    LowerQueueStep lowerQueueStep;        // null = DelayBudget::NextLowerQueueFrames
};

// The frames ahead left when the queue gives up `framesGivenUp` look-ahead frames (S-C): the window's queued-ahead term loses them,
// so the computed depth is capped by what is still queued. Window arithmetic only; the queue length itself is DelayBudget's.
inline uint16_t AheadComputeAfterShorterQueue(const QueueWindow& w, int framesGivenUp)
{
    const int left = int(w.ahead) - (framesGivenUp > 0 ? framesGivenUp : 0);
    const uint16_t queued = left > 0 ? uint16_t(left) : uint16_t(0);
    return w.aheadCompute < queued ? w.aheadCompute : queued;
}

enum class AheadStep : uint8_t { None, ReduceEarlyProducts, FewerFramesAhead, ShorterQueue, AtLatencyFloor };

inline const char* Token(AheadStep s)
{
    switch (s) {
    case AheadStep::None: return "full ahead depth";
    case AheadStep::ReduceEarlyProducts: return "fewer products computed early";
    case AheadStep::FewerFramesAhead: return "fewer frames ahead";
    case AheadStep::ShorterQueue: return "shorter queue (total delay unchanged)";
    case AheadStep::AtLatencyFloor: return "ahead work limited by VRAM at the queue floor";
    }
    return "?";
}

struct AheadPlan {
    struct Entry { uint16_t id; uint64_t variantHash; uint16_t depth; };   // variantHash 0 = every variant of the product
    std::vector<Entry> depth;
    // The delay-budget plan this ahead plan assumes: DelayBudget::Compute(budget), each S-C step DelayBudget's next lower one (its
    // queueFrames, paddingMs: the total stays budgetMs). Not feasible (queueFrames 0) when the budget was not fed.
    DelayBudget::Plan queue;
    uint16_t aheadCap = 0;            // the computed-depth cap: the window's aheadCompute; S-C lowers it with the queue
    AheadStep step = AheadStep::None;
    uint64_t bytes = 0;               // what the plan needs (one ledger level)
    std::string line;                 // the visible line (Ctrl+I, Config status, Ctrl+Alt+D)
    // The planned depth of (id, variant): an exact variant entry, else the product's any-variant entry. False = not planned.
    bool Has(uint16_t id, uint64_t variantHash, uint16_t* d) const
    {
        const Entry* any = nullptr;
        for (const Entry& e : depth)
            if (e.id == id) {
                if (e.variantHash == variantHash) {
                    if (d != nullptr)
                        *d = e.depth;
                    return true;
                }
                if (e.variantHash == 0)
                    any = &e;
            }
        if (any != nullptr && d != nullptr)
            *d = any->depth;
        return any != nullptr;
    }
    bool Has(uint16_t id, uint16_t* d) const { return Has(id, 0, d); }
};

// The bytes a set of depths needs: every frame behind (back), the render frame, and the planned frames ahead.
inline uint64_t AheadBytes(const AheadNeed& n, const std::vector<uint16_t>& depth)
{
    uint64_t b = 0;
    for (size_t i = 0; i < n.costs.size(); ++i)
        b += n.costs[i].bytesPerFrame * (1u + uint64_t(n.window.back) + uint64_t(depth[i]));
    return b;
}

// Every state of THE step-down ladder, in order (the first is the full window; each later one takes one more step):
//   ReduceEarlyProducts  S-A: Fill products lose their ahead depth one at a time (catalog order), then Derived, then Slice; Core and
//                        Unpack keep their declared depth (memory changes how FAR ahead core is made, never WHETHER);
//   FewerFramesAhead     S-B: the Core / Unpack depth shrinks from the furthest frame inward, down to its declared depth;
//   ShorterQueue         S-C, last, Normal mode only, budget fed: DelayBudget's next lower ladder step (total delay unchanged), the
//                        ahead cap following the shorter queue; never below DelayBudget::kMinimumDepth, never a raise;
//   AtLatencyFloor       the final state when even the floor does not fit (shown; the refusal safety net then applies).
inline std::vector<AheadPlan> AheadLadderStates(const AheadNeed& n)
{
    std::vector<AheadPlan> states;
    const uint16_t A = n.window.aheadCompute;   // amendment 3: the plan starts at the computed depth, never the whole queue
    std::vector<uint16_t> depth(n.costs.size(), A);
    uint16_t cap = A;
    DelayBudget::Plan queue = n.budgetFed ? DelayBudget::Compute(n.budget) : DelayBudget::Plan();
    const int startFrames = queue.queueFrames;
    std::string why;   // on the floor line: why S-C was not (or no longer) available
    auto push = [&](AheadStep s) {
        AheadPlan p;
        p.step = s;
        p.queue = queue;
        p.aheadCap = cap;
        for (size_t i = 0; i < n.costs.size(); ++i)
            p.depth.push_back(AheadPlan::Entry{ n.costs[i].id, n.costs[i].variant.Hash(), depth[i] });
        p.bytes = AheadBytes(n, depth);
        p.line = std::string("memo ahead plan: ") + Token(s) + " (queue "
                 + (queue.feasible ? std::to_string(queue.queueFrames) + " frame(s), padding " + std::to_string(int(queue.paddingMs + 0.5)) + " ms"
                                   : std::string("delay budget not fed"))
                 + ", ahead compute " + std::to_string(cap) + " of " + std::to_string(A) + ", need " + std::to_string(p.bytes >> 20)
                 + " MiB of " + std::to_string(n.grantBytes >> 20) + " MiB" + (s == AheadStep::AtLatencyFloor && !why.empty() ? "; " + why : "")
                 + ")";
        states.push_back(p);
    };
    push(AheadStep::None);
    for (JobClass cls : { JobClass::Fill, JobClass::Derived, JobClass::Slice })   // the shed order, written out (never enum values)
        for (size_t i = 0; i < n.costs.size(); ++i)
            if (n.costs[i].cls == cls && depth[i] != 0) {
                depth[i] = 0;
                push(AheadStep::ReduceEarlyProducts);
            }
    for (uint16_t d = A; d > 0; --d) {
        bool changed = false;
        for (size_t i = 0; i < n.costs.size(); ++i)
            if (KeepsDeclaredDepth(n.costs[i].cls) && depth[i] >= d && uint32_t(d) - 1u >= n.costs[i].declaredDepth) {
                depth[i] = uint16_t(d - 1u);
                changed = true;
            }
        if (changed)
            push(AheadStep::FewerFramesAhead);
    }
    if (n.mode != QueueMode::Normal)
        why = "games / competitive queue: no shorter queue";
    else if (!n.budgetFed || !queue.feasible)
        why = "no shorter queue: delay budget not fed";
    else {
        const LowerQueueStep lower = n.lowerQueueStep ? n.lowerQueueStep : LowerQueueStep(DelayBudget::NextLowerQueueFrames);
        DelayBudget::Plan next;
        while (lower(n.budget, queue.queueFrames, &next) && next.feasible && next.queueFrames < queue.queueFrames) {
            queue = next;
            cap = AheadComputeAfterShorterQueue(n.window, startFrames - queue.queueFrames);
            for (uint16_t& d : depth)
                if (d > cap)
                    d = cap;
            push(AheadStep::ShorterQueue);
        }
        why = "queue at the delay budget's minimum";
    }
    push(AheadStep::AtLatencyFloor);
    return states;
}

// The plan: the first ladder state that fits the grant; else the last (AtLatencyFloor).
inline AheadPlan PlanAheadLadder(const AheadNeed& n)
{
    const std::vector<AheadPlan> states = AheadLadderStates(n);
    for (const AheadPlan& p : states)
        if (p.step != AheadStep::AtLatencyFloor && p.bytes <= n.grantBytes)
            return p;
    return states.back();
}

// The window as the PLAN leaves it (audit SHOULD 7, no silent cap): when the plan computes fewer frames ahead than the window's
// aheadCompute, aheadCompute is the plan's deepest depth and the binding is Budget ("ahead compute N (...; budget)", shown).
inline QueueWindow WithAheadPlan(QueueWindow w, const AheadPlan& p)
{
    uint16_t deepest = 0;
    for (const AheadPlan::Entry& e : p.depth)
        if (e.depth > deepest)
            deepest = e.depth;
    if (p.aheadCap < deepest)
        deepest = p.aheadCap;
    if (!p.depth.empty() && deepest < w.aheadCompute) {
        w.aheadCompute = deepest;
        w.aheadBinding = AheadBinding::Budget;
    }
    return w;
}

// One VRAM ledger level per ladder step (distinct byte needs, non-increasing; the governor grants the largest that fits).
inline std::vector<uint64_t> AheadLevels(const AheadNeed& n)
{
    std::vector<uint64_t> levels;
    for (const AheadPlan& p : AheadLadderStates(n))
        if (levels.empty() || p.bytes < levels.back())
            levels.push_back(p.bytes);
    return levels;
}

}  // namespace VpSkeleton
