// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// S-3a S3 (lead9/brief-S-3a-scheduler.md s4 S3; refs/122 S3; doc 91 C :79): the Configure-time admission table. Once per Configure (and per
// latency-mode change; never per window, doc 96 s3.1) it decides, from MEASURED ledger costs and the measured source rate: the class of each
// product, which products are admitted, and the TD slice ms the slot declares (SpareHost::SetBudget(budget, sliceMs) / SlotBudgetFeed::
// SetDeclaredSliceMs). Pure: S3 wiring (Config status / Ctrl+I lines through the existing VP-V status rows) waits for S2 on integ.
//
// Decisions (provenance, rule 17):
//  - FIXED CORE SET never shed for time: P1 + P2 (P2m), P7 / P8, P3, resync/validity, P15 / P16, P4 + P11 (doc 91 C :79, owner 2026-11-11 07:45).
//    Core is admitted even when its measured cost exceeds the budget (the overload arbiter, core/OverloadOrder.h, then steps visibly).
//  - Optional fill (P5, P6, P9, P10c, P12-as-job, long pair) admitted ONCE at Configure from the ledger (doc 91 C :79); an UNMEASURED optional
//    product is not admitted and says "not measured" (S3-T4; rule 5).
//  - P10 (grain split) is optional fill until <= 2 ms at 4K is proven, then core (doc 91 C :119 D7; coordinator 2026-11-14 08:22 via lead 10, Q4).
//  - TD's products (td.*, Slice class) are the TD slice's own jobs, admitted with the slice and costed inside it; never optional fill, never in
//    the FillOff list (owner OD:391 2026-11-11 07:45 "never core, never TD"; 115 #47 / #59). Every id has an explicit class (StaticClassOf).
//  - TD reserved slice: derived at Configure from the MEASURED remainder (budget - core), never a fixed number (coordinator 2026-11-14 08:22
//    via lead 10, Q3); below TD's minimum TD shows its reduced tier (the line says so). The 3090 "4K ~8 ms + P10" is a design figure, never
//    a constant here; 3090 numbers are estimates until a VP run there (feedback-no-waiting-on-3090) and are shown as "estimated".
//  - Latency modes: standard (250 ms) and AVR-safe (200 ms, a CEILING on total latency, never a floor) admit the same way; games (1 queued) =
//    backward products only, so no optional fill and no TD look-ahead slice (TD's backward-only tier); competitive (0) = no slot work at all
//    (IC-2 R9). Neither game mode is ever given queueing. kDesignDepth 4 is TD's full-strength need, not a queue floor or cap: below it TD
//    steps down visibly. Coordinator 2026-11-14 ruling via lead 11 (design s9 :168-176).
//  - Ahead compute (OD 11-14, design s10, supersedes "the lane runs the full queue depth" and kMaxLead 13). The feed carries BOTH, named
//    (lead 12 ruling R3): queueAheadFrames = L + W, the frames queued ahead of the render (DelayBudget / QueueFrames own it), and the
//    ahead-compute limit aheadComputeLimit = min(max active consumer request, kAheadCeiling 6 (core/AheadCeiling.h, the one value),
//    queueAheadFrames); 0 with no active consumer. 6 is a ceiling, not a target: PlanAhead may go lower under budget (S-B). The keep window
//    and the queue length are not this header's business and are unchanged.
//    Ctrl+Alt+D shows "ahead compute N (by <consumer>; ceiling 6 | budget | queue)". Held at the owner's 6 cap is the DESIGNED state, not a
//    downgrade (R3): a Ctrl+I info line only, no notice or fallback entry. A visible notice (Ctrl+I, Config status, one-time notice) only
//    when the consumer gets LESS than it requested within the cap (the queue or the budget bound it).
//  - "GPU tier" = the measured costs; the card name is a label only (rule 6). No literal rate, cadence or resolution (S3-T2).
// Pure C++14.

#include <VP-V/core/AheadCeiling.h>
#include <VP-V/core/DelayBudget.h>
#include <VP-V/core/LaneSpareWork.h>
#include <VP-V/core/Products.h>
#include <VP-V/core/QueueWindow.h>
#include <VP-V/core/SchedulerContract.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace VpSkeleton {
namespace Admission {

constexpr size_t kProductCount = size_t(kSkeletonProductEnd);

// The ahead ceiling is VpSkeleton::kAheadCeiling (core/AheadCeiling.h; lead 12 ruling R3): one value, no local copy.
constexpr size_t kMaxAheadConsumers = 8;

// One ahead consumer (TD, motion vectors, ...): its declared ahead need (usually 4; TD 4 at full strength, up to 6 if required).
struct AheadConsumer {
    const char* name = nullptr;
    bool active = false;
    int need = 0;
};

// What bound the ahead compute (shown; no silent cap).
enum class AheadBound : uint8_t { NoConsumer = 0, Consumer, Ceiling, Queue, Budget };

enum class LatencyMode : uint8_t { Standard = 0, AvrSafe, Games, Competitive };

inline const char* LatencyModeName(LatencyMode m)
{
    switch (m) {
    case LatencyMode::Standard: return "standard";
    case LatencyMode::AvrSafe: return "AVR-safe";
    case LatencyMode::Games: return "games";
    case LatencyMode::Competitive: return "competitive";
    }
    return "?";
}

struct AdmissionFacts {
    double periodMs = 0.0;                 // the MEASURED source period (1000 / captureRateHz; nominal only while unmeasured)
    double renderMs = 0.0;                 // the measured render EWMA (RenderBudget)
    double costMs[kProductCount] = {};     // the ledger's measured per-frame cost of each product at the current geometry
    bool measured[kProductCount] = {};     // costMs[i] is a measurement
    bool present[kProductCount] = {};      // the product is in the run-time graph (a node): nothing else is admitted
    LatencyMode mode = LatencyMode::Standard;
    double tdMinMs = 0.0;                  // TD's own minimum tier (its ladder; 0 = TD not running)
    double tdWantMs = 0.0;                 // TD's wanted share (its full tier)
    bool p10Proven = false;                // the ledger row "P10 <= 2 ms at 4K" is recorded (Q4)
    bool estimated = false;                // the costs are estimates (no VP run on this card yet), shown
    // Ahead memory (owner answer via lead 11, 2026-11-14; lead10/design-queue-keyed-products.md s2.7): the VRAM ledger grant for ahead spots and
    // what one demanded ahead frame of each product holds (the s2.7 per-frame memo set at the current geometry, e.g. P13 luma 3 B/px ...).
    uint64_t vramGrantBytes = 0;           // 0 = no grant known: no ahead planning (the memo's own OverBudget refusal stays the backstop)
    uint64_t aheadBytes[kProductCount] = {};   // bytes per demanded ahead frame (0 = the product holds nothing ahead)
    AheadConsumer aheadConsumers[kMaxAheadConsumers] = {};   // the ahead consumers and their declared needs (inactive ones are ignored)
    int queueAheadFrames = 0;              // L + W: the frames queued ahead of the render (DelayBudget / QueueFrames own it, never fixed here)
    int queueBackFrames = 0;               // the window's back term (H + max(R, P)): frames behind the render each ahead product also holds
    int aheadMinFrames = 1;                // the core declared depth (S-B never shrinks Core / Unpack below it; the ladder's declaredDepth)
    int queueFrames = 0;                   // frames the queue holds now (the result while the ladder takes no S-C step)
    // S-C (the LAST step) is THE ladder's (QueueWindow.h PlanAheadLadder): DelayBudget's next lower step for the SAME delay budget (total delay
    // unchanged), never below DelayBudget::kMinimumDepth, standard / AVR-safe only, never a raise; games 1 and competitive 0 are never touched
    // (coordinator 2026-11-14 ruling, owner correction; lead10/design-queue-keyed-products.md s9 :168-176). Not fed = no S-C step (shown).
    // The 200 ms AVR-safe tier is its budgetMs: a CEILING owned by DelayBudget, never a floor.
    DelayBudget::Inputs delayBudget;
    bool delayBudgetFed = false;
};

struct Result {
    JobClass cls[kProductCount] = {};
    bool admitted[kProductCount] = {};
    bool jobbed[kProductCount] = {};       // a job (false = Derived-live through Acquire)
    bool notMeasured[kProductCount] = {};  // optional product left out because unmeasured (visible)
    double budgetMs = 0.0;                 // the slot budget (RenderBudget law)
    double coreMs = 0.0;                   // measured core cost (unmeasured core counted 0, named in the line)
    double sliceMs = 0.0;                  // the TD slice declared to the slot
    double fillMs = 0.0;                   // admitted fill
    bool tdReduced = false;                // the slice is below TD's minimum: TD runs its reduced tier
    bool coreOver = false;                 // core alone exceeds the budget (the overload arbiter acts; core is never dropped)
    uint32_t unmeasuredCore = 0;
    // Ahead plan (owner answer via lead 11: plan to the grant, never evict; steps in order, each visible)
    bool aheadAdmitted[kProductCount] = {};   // computed early for frames ahead (else at its own render, through the same Acquire)
    int aheadFrames = 0;                   // frames ahead demanded (the plan's deepest depth)
    int queueFrames = 0;                   // the queue length the plan asks for (the ladder's DelayBudget plan when S-C ran, else facts.queueFrames)
    uint64_t aheadNeedBytes = 0;           // what the plan holds (AheadBytes of the ladder state: back + render + planned ahead, per product)
    uint32_t aheadProductsDropped = 0;     // S-A: optional products moved from early to render time
    int aheadFramesDropped = 0;            // S-B (and the ahead cap S-C lowered)
    int queueFramesDropped = 0;            // S-C (look-ahead shorter; DelayBudget padding keeps the total delay, shown)
    AheadPlan aheadPlan;                   // THE ladder's plan (QueueWindow.h PlanAheadLadder), for the memo (SkeletonMemo::SetAheadPlan)
    int aheadRequest = 0;                  // the largest ACTIVE consumer request (before the ceiling)
    int aheadComputeLimit = 0;             // min(aheadRequest, kAheadCeiling, queueAheadFrames): the ahead-compute limit, before the budget
    AheadBound aheadBound = AheadBound::NoConsumer;   // what bound the final ahead frames (Budget when S-B or the grant cut it)
    const char* aheadBy = nullptr;         // the binding (largest-need) active consumer
    bool aheadCeilingInfo = false;         // held at the owner's cap: the DESIGNED state, a Ctrl+I info line only (no notice, no fallback)
    bool aheadShortNotice = false;         // the consumer gets LESS than it asked within the cap (queue or budget): visible notice
    std::string aheadComputeLine;          // Ctrl+Alt+D: "ahead compute N (by <consumer>; ceiling 6 | budget | queue)"
    bool aheadOverGrant = false;           // still over the grant at every floor: ahead core fills at render (visible)
    std::string aheadLine;                 // the ahead plan line (Ctrl+I / Config status / Ctrl+Alt+D)
    std::string line;                      // Ctrl+I / Config status / Ctrl+Alt+D
};

// The window's feed for these facts (QueueWindow.h PlanAheadFeed): the ACTIVE consumers with their declared needs and the frames queued ahead.
inline PlanAheadFeed AheadFeedOf(const AdmissionFacts& f)
{
    PlanAheadFeed feed;
    for (size_t i = 0; i < kMaxAheadConsumers; ++i) {
        const AheadConsumer& c = f.aheadConsumers[i];
        if (c.active && c.need > 0)
            feed.activeConsumers.push_back(AheadDemand{ Sat16(uint32_t(c.need)), c.name != nullptr ? c.name : "?" });
    }
    feed.queueAheadFrames = Sat16(uint32_t(f.queueAheadFrames > 0 ? f.queueAheadFrames : 0));
    return feed;
}

// aheadCompute (OD 11-14, design s10): THE law QueueWindow.h AheadComputeLimitOf applies to the feed (min(max active need, kAheadCeiling,
// queue frames ahead); 0 with no active consumer), never a second copy. Game modes compute nothing ahead (R9). Sets aheadRequest,
// aheadComputeLimit, aheadBound and aheadBy.
inline int AheadCompute(const AdmissionFacts& f, Result& r)
{
    const PlanAheadFeed feed = AheadFeedOf(f);
    const AheadDemand top = MaxAheadNeed(feed.activeConsumers);
    r.aheadRequest = int(top.need);
    r.aheadBy = top.need > 0 ? top.consumer : nullptr;
    if (top.need == 0) {
        r.aheadBound = AheadBound::NoConsumer;
        r.aheadComputeLimit = 0;
        return 0;
    }
    if (f.mode == LatencyMode::Competitive || f.mode == LatencyMode::Games) {   // R9: no look-ahead work (backward-only tier)
        r.aheadBound = AheadBound::Consumer;
        r.aheadComputeLimit = 0;
        return 0;
    }
    const uint16_t limit = AheadComputeLimitOf(feed);
    const uint16_t held = top.need < feed.aheadCeiling ? top.need : feed.aheadCeiling;
    r.aheadBound = limit < held ? AheadBound::Queue : (top.need > feed.aheadCeiling ? AheadBound::Ceiling : AheadBound::Consumer);
    r.aheadComputeLimit = int(limit);
    return int(limit);
}

inline QueueMode QueueModeOf(LatencyMode m)
{
    return m == LatencyMode::Games ? QueueMode::Games : (m == LatencyMode::Competitive ? QueueMode::Competitive : QueueMode::Normal);
}

// What the admission hands THE ladder (QueueWindow.h AheadNeed): the window (back, queued ahead, the computed depth), the grant, the mode, the
// delay budget, and one AheadCost per product computed early (its bytes per frame, its class, Core / Unpack keeping the declared depth).
inline AheadNeed AdmissionAheadNeed(const AdmissionFacts& f, const Result& r)
{
    AheadNeed n;
    n.window.back = Sat16(uint32_t(f.queueBackFrames > 0 ? f.queueBackFrames : 0));
    n.window.ahead = Sat16(uint32_t(f.queueAheadFrames > 0 ? f.queueAheadFrames : 0));
    n.window.aheadCompute = Sat16(uint32_t(r.aheadFrames > 0 ? r.aheadFrames : 0));
    n.window.aheadBy = r.aheadBy != nullptr ? r.aheadBy : "";
    n.grantBytes = f.vramGrantBytes;
    n.mode = QueueModeOf(f.mode);
    n.budget = f.delayBudget;
    n.budgetFed = f.delayBudgetFed;
    const int floorAhead = f.aheadMinFrames > 0 ? (std::min)(f.aheadMinFrames, r.aheadFrames) : 0;
    for (size_t i = 0; i < kProductCount; ++i) {
        if (!r.aheadAdmitted[i]) continue;
        AheadCost c;
        c.id = uint16_t(i);
        c.form = 1;   // GPU spot
        c.bytesPerFrame = f.aheadBytes[i];
        c.msPerFrame = f.measured[i] && f.costMs[i] > 0.0 ? f.costMs[i] : 0.0;
        c.measured = f.measured[i];
        c.cls = r.cls[i];
        c.declaredDepth = KeepsDeclaredDepth(c.cls) ? Sat16(uint32_t(floorAhead > 0 ? floorAhead : 0)) : uint16_t(0);
        n.costs.push_back(c);
    }
    return n;
}

// The Ctrl+Alt+D ahead compute line. Ceiling-bound: Ctrl+I info only (designed state). Less than asked within the cap: visible notice.
inline void AheadComputeLine(const AdmissionFacts& f, Result& r)
{
    const int ceiling = int(kAheadCeiling);   // the one ahead ceiling (shown on Ctrl+I when it binds, G7)
    const int frames = r.aheadOverGrant ? 0 : r.aheadFrames;
    const char* by = r.aheadBy != nullptr ? r.aheadBy : "?";
    char buf[160] = {};
    if (r.aheadBound == AheadBound::NoConsumer)
        std::snprintf(buf, sizeof(buf), "ahead compute %d (no active consumer)", frames);
    else if (f.mode == LatencyMode::Competitive || f.mode == LatencyMode::Games)
        std::snprintf(buf, sizeof(buf), "ahead compute 0 (by %s; %s: backward-only)", by, LatencyModeName(f.mode));
    else {
        char bound[32] = {};
        if (r.aheadBound == AheadBound::Ceiling) std::snprintf(bound, sizeof(bound), "; ceiling %d", ceiling);
        else if (r.aheadBound == AheadBound::Budget) std::snprintf(bound, sizeof(bound), "; budget");
        else if (r.aheadBound == AheadBound::Queue) std::snprintf(bound, sizeof(bound), "; queue");
        std::snprintf(buf, sizeof(buf), "ahead compute %d (by %s%s)", frames, by, bound);
    }
    r.aheadComputeLine = buf;
    const bool gameMode = f.mode == LatencyMode::Competitive || f.mode == LatencyMode::Games;   // backward-only tier by design
    r.aheadCeilingInfo = r.aheadRequest > ceiling;
    r.aheadShortNotice = !gameMode && frames < (std::min)(r.aheadRequest, ceiling);
}

// Sizes the ahead work to the VRAM grant (owner answer via lead 11, 2026-11-14) with THE ladder, QueueWindow.h PlanAheadLadder (one copy of
// the law; audit MUST 1): S-A fewer products computed early (Fill, then Derived, then Slice; Core / Unpack never), S-B fewer frames ahead
// down to the declared core depth, and only as the LAST step S-C DelayBudget's next lower queue step (total delay unchanged, never below
// kMinimumDepth, never in games / competitive, never a raise). Never evicts: what is not planned early is filled at its own render through
// the same Acquire. This function only maps the facts onto the ladder and its result onto the lines.
inline void PlanAhead(const AdmissionFacts& f, Result& r)
{
    r.queueFrames = f.queueFrames;
    r.aheadFrames = AheadCompute(f, r);   // a ceiling, not a target: S-B below may plan fewer
    for (size_t i = 0; i < kProductCount; ++i) r.aheadAdmitted[i] = r.admitted[i] && f.aheadBytes[i] > 0 && r.aheadFrames > 0;
    const AheadNeed n = AdmissionAheadNeed(f, r);
    char buf[240] = {};
    if (f.vramGrantBytes == 0 || r.aheadFrames == 0) {
        r.aheadNeedBytes = AheadBytes(n, std::vector<uint16_t>(n.costs.size(), n.window.aheadCompute));
        std::snprintf(buf, sizeof(buf), "ahead plan: %d frame(s)%s", r.aheadFrames,
            r.aheadFrames == 0 ? "" : " (no VRAM grant known: not planned; a spot over the grant is refused at fill and made at render)");
        r.aheadLine = buf;
        AheadComputeLine(f, r);
        return;
    }
    const int requested = r.aheadFrames;
    const int startQueue = n.budgetFed ? DelayBudget::Compute(n.budget).queueFrames : 0;
    r.aheadPlan = PlanAheadLadder(n);
    const AheadPlan& p = r.aheadPlan;
    r.aheadOverGrant = p.bytes > n.grantBytes;   // only the AtLatencyFloor state can be over: shown, ahead work at render
    int deepest = 0;
    for (size_t k = 0; k < p.depth.size(); ++k) {
        const int d = (std::min)(int(p.depth[k].depth), int(p.aheadCap));
        const size_t id = p.depth[k].id;
        if (d == 0) {
            if (id < kProductCount && r.aheadAdmitted[id] && !KeepsDeclaredDepth(r.cls[id])) ++r.aheadProductsDropped;
            if (id < kProductCount) r.aheadAdmitted[id] = false;
        }
        if (d > deepest) deepest = d;
    }
    r.aheadFrames = deepest;
    r.aheadFramesDropped = requested > deepest ? requested - deepest : 0;
    if (n.mode == QueueMode::Normal && p.queue.feasible) {
        r.queueFrames = p.queue.queueFrames;
        r.queueFramesDropped = startQueue > p.queue.queueFrames ? startQueue - p.queue.queueFrames : 0;
    }
    if (r.aheadFramesDropped > 0 || r.aheadOverGrant) r.aheadBound = AheadBound::Budget;
    if (r.aheadOverGrant)
        for (size_t i = 0; i < kProductCount; ++i) r.aheadAdmitted[i] = false;   // nothing early: every frame fills at its own render
    r.aheadNeedBytes = r.aheadOverGrant ? AheadBytes(n, std::vector<uint16_t>(n.costs.size(), uint16_t(0))) : p.bytes;
    std::snprintf(buf, sizeof(buf), "ahead plan: %d frame(s), %.1f of %.1f MB%s%s%s%s", r.aheadOverGrant ? 0 : r.aheadFrames,
        double(r.aheadNeedBytes) / 1048576.0, double(f.vramGrantBytes) / 1048576.0,
        r.aheadProductsDropped > 0 ? "; fewer products early" : "", r.aheadFramesDropped > 0 ? "; fewer frames ahead" : "",
        r.queueFramesDropped > 0 ? "; queue shortened (less look-ahead)" : "",
        r.aheadOverGrant ? "; limited by VRAM at the queue floor: ahead work at render" : "");
    r.aheadLine = std::string(buf) + "; " + p.line;   // the ladder's own line (its step, the queue and padding, why S-C stopped)
    AheadComputeLine(f, r);
}

// The class of a product at Configure: the static class, with P10 promoted to core once proven (Q4).
inline StaticClass ClassOf(ProductId id, const AdmissionFacts& f)
{
    if (id == kP10GrainSplit && f.p10Proven) return StaticClass{ JobClass::Core, true };
    return StaticClassOf(id);
}

// The settings cost mark from measured ms at the 4K30 laptop reference (control-config rule 4: < 0.5 "+", 0.5-2 "++", > 2 "+++"; none untimed).
inline const char* CostMark(double ms, bool measured)
{
    if (!measured || !(ms >= 0.0)) return "";
    return ms < 0.5 ? "+" : (ms <= 2.0 ? "++" : "+++");
}

inline Result Admit(const AdmissionFacts& f)
{
    Result r;
    // The ONE render budget law (core/SchedulerContract.h): period - render - period/10.
    RenderBudget rb;
    if (f.renderMs > 0.0) rb.Note(f.renderMs);
    const double hz = f.periodMs > 0.0 ? 1000.0 / f.periodMs : 0.0;
    r.budgetMs = rb.Measured() ? rb.BudgetMs(hz) : 0.0;

    for (size_t i = 0; i < kProductCount; ++i) {
        const StaticClass c = ClassOf(ProductId(i), f);
        r.cls[i] = c.cls;
        r.jobbed[i] = c.jobbed;
    }

    char buf[320] = {};
    if (f.mode == LatencyMode::Competitive) {
        std::snprintf(buf, sizeof(buf), "admission (competitive): no slot work (latency 0)");
        r.line = buf;
        PlanAhead(f, r);
        return r;
    }

    // 1. Core: always admitted.
    for (size_t i = 0; i < kProductCount; ++i) {
        if (!f.present[i] || r.cls[i] != JobClass::Core || !r.jobbed[i]) continue;
        r.admitted[i] = true;
        if (f.measured[i] && f.costMs[i] > 0.0) r.coreMs += f.costMs[i];
        else if (!f.measured[i]) ++r.unmeasuredCore;
    }
    // Derived: a job right after its inputs (admitted with core: it is part of the core chain); Derived-live is never a job.
    for (size_t i = 0; i < kProductCount; ++i) {
        if (!f.present[i] || r.cls[i] != JobClass::Derived) continue;
        r.admitted[i] = r.jobbed[i];
        if (r.admitted[i] && f.measured[i] && f.costMs[i] > 0.0) r.coreMs += f.costMs[i];
    }
    double left = r.budgetMs - r.coreMs;
    r.coreOver = left < 0.0;
    if (left < 0.0) left = 0.0;

    // 2. TD slice from the measured remainder (Q3). Games: no look-ahead slice.
    if (f.mode != LatencyMode::Games && f.tdWantMs > 0.0) {
        r.sliceMs = f.tdWantMs < left ? f.tdWantMs : left;
        r.tdReduced = r.sliceMs < f.tdMinMs;
        left -= r.sliceMs;
    }
    // The TD products (Slice class, LaneSpareWork.h StaticClassOf) are the slice's OWN jobs (OD:391 (2)/(3); 115 #47 / #59): admitted with
    // TD, costed inside sliceMs (never on top of it, never from fill leftovers), never "not measured" fill and never in the FillOff list
    // (OptionalFillIds). A short slice is TD's own reduced tier (tdReduced, shown), never a TD product switched off.
    uint32_t sliceJobs = 0;
    for (size_t i = 0; i < kProductCount; ++i) {
        if (!f.present[i] || r.cls[i] != JobClass::Slice || !r.jobbed[i]) continue;
        r.admitted[i] = true;
        ++sliceJobs;
    }

    // 3. Fill in what is left, in product order, measured only. Games: none (backward products only).
    uint32_t fillOn = 0, fillOff = 0, fillUnmeasured = 0;
    for (size_t i = 0; i < kProductCount; ++i) {
        if (!f.present[i] || r.cls[i] != JobClass::Fill || !r.jobbed[i]) continue;
        if (f.mode == LatencyMode::Games) { ++fillOff; continue; }
        if (!f.measured[i]) { r.notMeasured[i] = true; ++fillUnmeasured; continue; }
        const double c = f.costMs[i] > 0.0 ? f.costMs[i] : 0.0;
        if (c <= left) { r.admitted[i] = true; left -= c; r.fillMs += c; ++fillOn; }
        else ++fillOff;
    }

    std::snprintf(buf, sizeof(buf),
        "admission (%s%s): budget %.2f ms, core %.2f ms%s%s, TD slice %.2f ms%s, fill %u on / %u off%s%s",
        LatencyModeName(f.mode), f.estimated ? ", estimated" : "", r.budgetMs, r.coreMs,
        r.unmeasuredCore > 0 ? " (some core not measured)" : "", r.coreOver ? " OVER BUDGET (core kept; overload steps)" : "",
        r.sliceMs, f.mode == LatencyMode::Games ? " (games: none)" : (r.tdReduced ? " (below TD minimum: reduced tier)" : ""),
        unsigned(fillOn), unsigned(fillOff), fillUnmeasured > 0 ? ", not measured " : "",
        fillUnmeasured > 0 ? std::to_string(fillUnmeasured).c_str() : "");
    r.line = buf;
    if (sliceJobs > 0) r.line += "; TD jobs " + std::to_string(sliceJobs) + " in the TD slice";
    if (!rb.Measured() || !(hz > 0.0)) r.line += "; no budget (render or source rate not measured)";
    PlanAhead(f, r);
    return r;
}

// The optional fills the admission turned on: what the wiring hands OverloadArbiter::SetOptionalFills (core/OverloadOrder.h), so FillOff
// switches off optional extras only (OD:391 (2): never core, never TD). Fill class only: a Core, Derived or Slice (TD) product is never listed.
inline std::vector<ProductId> OptionalFillIds(const Result& r)
{
    std::vector<ProductId> ids;
    for (size_t i = 0; i < kProductCount; ++i)
        if (r.admitted[i] && r.jobbed[i] && r.cls[i] == JobClass::Fill) ids.push_back(ProductId(i));
    return ids;
}

}   // namespace Admission
}   // namespace VpSkeleton
