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
//  - TD reserved slice: derived at Configure from the MEASURED remainder (budget - core), never a fixed number (coordinator 2026-11-14 08:22
//    via lead 10, Q3); below TD's minimum TD shows its reduced tier (the line says so). The 3090 "4K ~8 ms + P10" is a design figure, never
//    a constant here; 3090 numbers are estimates until a VP run there (feedback-no-waiting-on-3090) and are shown as "estimated".
//  - Latency modes: standard (250 ms) and AVR-safe (200 ms) admit the same way; games (1 queued) = backward products only, so no optional
//    fill and no TD look-ahead slice; competitive (0) = no slot work at all (IC-2 R9).
//  - "GPU tier" = the measured costs; the card name is a label only (rule 6). No literal rate, cadence or resolution (S3-T2).
// Pure C++14.

#include <VP-V/core/LaneSpareWork.h>
#include <VP-V/core/Products.h>
#include <VP-V/core/SchedulerContract.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>

namespace VpSkeleton {
namespace Admission {

constexpr size_t kProductCount = size_t(kSkeletonProductEnd);

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
    int aheadWantFrames = 0;               // frames ahead the consumers want (L of the window; derived from the queue, never fixed)
    int aheadMinFrames = 1;                // the core floor of frames ahead (below it ahead core fills at render, visibly)
    int queueFrames = 0;                   // frames the queue holds now
    uint64_t queueBytesPerFrame = 0;       // bytes each queued frame holds regardless of ahead demand (early-upload slot etc.)
    double queueFloorMs = 200.0;           // the queue is never shortened below this latency (AVR-safe floor, owner 91 C :17)
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
    int aheadFrames = 0;                   // frames ahead demanded
    int queueFrames = 0;                   // the queue length the plan asks for (== facts.queueFrames unless step 3 shortened it)
    uint64_t aheadNeedBytes = 0;           // what the plan holds (ahead spots + queued frames)
    uint32_t aheadProductsDropped = 0;     // step 1: optional products moved from early to render time
    int aheadFramesDropped = 0;            // step 2
    int queueFramesDropped = 0;            // step 3 (latency change, shown)
    bool aheadOverGrant = false;           // still over the grant at every floor: ahead core fills at render (visible)
    std::string aheadLine;                 // the ahead plan line (Ctrl+I / Config status / Ctrl+Alt+D)
    std::string line;                      // Ctrl+I / Config status / Ctrl+Alt+D
};

// The ahead plan's bytes: queued frames + ahead spots of the products computed early.
inline uint64_t AheadNeedBytes(const AdmissionFacts& f, const Result& r, int aheadFrames, int queueFrames)
{
    uint64_t perFrame = 0;
    for (size_t i = 0; i < kProductCount; ++i)
        if (r.aheadAdmitted[i]) perFrame += f.aheadBytes[i];
    return uint64_t(aheadFrames > 0 ? aheadFrames : 0) * perFrame + uint64_t(queueFrames > 0 ? queueFrames : 0) * f.queueBytesPerFrame;
}

// Sizes the ahead work to the VRAM grant (owner answer via lead 11, 2026-11-14): first fewer products computed early (optional fill first, the
// highest product id first; core last never), then fewer frames ahead down to the core floor, and only as the LAST step a shorter queue (a
// latency change, never below the AVR-safe floor). Never evicts: what is not planned early is filled at its own render through the same Acquire.
inline void PlanAhead(const AdmissionFacts& f, Result& r)
{
    r.queueFrames = f.queueFrames;
    r.aheadFrames = f.aheadWantFrames > 0 ? f.aheadWantFrames : 0;
    if (f.mode == LatencyMode::Competitive || f.mode == LatencyMode::Games) r.aheadFrames = 0;   // R9: no look-ahead work
    for (size_t i = 0; i < kProductCount; ++i) r.aheadAdmitted[i] = r.admitted[i] && f.aheadBytes[i] > 0 && r.aheadFrames > 0;
    char buf[240] = {};
    if (f.vramGrantBytes == 0 || r.aheadFrames == 0) {
        r.aheadNeedBytes = AheadNeedBytes(f, r, r.aheadFrames, r.queueFrames);
        std::snprintf(buf, sizeof(buf), "ahead plan: %d frame(s)%s", r.aheadFrames,
            r.aheadFrames == 0 ? "" : " (no VRAM grant known: not planned; a spot over the grant is refused at fill and made at render)");
        r.aheadLine = buf;
        return;
    }
    auto fits = [&] { return AheadNeedBytes(f, r, r.aheadFrames, r.queueFrames) <= f.vramGrantBytes; };
    // 1. fewer products early: optional (non-core) first, highest id first
    for (size_t k = kProductCount; k-- > 0 && !fits();)
        if (r.aheadAdmitted[k] && r.cls[k] != JobClass::Core && r.cls[k] != JobClass::Unpack) { r.aheadAdmitted[k] = false; ++r.aheadProductsDropped; }
    // 2. fewer frames ahead, down to the core floor
    const int floorAhead = f.aheadMinFrames > 0 ? f.aheadMinFrames : 1;
    while (!fits() && r.aheadFrames > floorAhead) { --r.aheadFrames; ++r.aheadFramesDropped; }
    // 3. LAST: a shorter queue, never below the floor latency and never below what the ahead frames need (ahead + the render frame)
    const int floorQueue = f.periodMs > 0.0 ? int(std::ceil(f.queueFloorMs / f.periodMs - 1e-9)) : r.queueFrames;
    const int minQueue = (std::max)(floorQueue, r.aheadFrames + 1);
    while (!fits() && r.queueFrames > minQueue) { --r.queueFrames; ++r.queueFramesDropped; }
    r.aheadOverGrant = !fits();
    if (r.aheadOverGrant)
        for (size_t i = 0; i < kProductCount; ++i) r.aheadAdmitted[i] = false;   // nothing early: every frame fills at its own render
    r.aheadNeedBytes = AheadNeedBytes(f, r, r.aheadOverGrant ? 0 : r.aheadFrames, r.queueFrames);
    std::snprintf(buf, sizeof(buf), "ahead plan: %d frame(s), %.1f of %.1f MB%s%s%s%s", r.aheadOverGrant ? 0 : r.aheadFrames,
        double(r.aheadNeedBytes) / 1048576.0, double(f.vramGrantBytes) / 1048576.0,
        r.aheadProductsDropped > 0 ? "; fewer products early" : "", r.aheadFramesDropped > 0 ? "; fewer frames ahead" : "",
        r.queueFramesDropped > 0 ? "; queue shortened (latency lower)" : "",
        r.aheadOverGrant ? "; over the grant at every floor: ahead work at render" : "");
    r.aheadLine = buf;
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
    if (!rb.Measured() || !(hz > 0.0)) r.line += "; no budget (render or source rate not measured)";
    PlanAhead(f, r);
    return r;
}

}   // namespace Admission
}   // namespace VpSkeleton
