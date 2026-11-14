// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// S-3a S4 (lead9/brief-S-3a-scheduler.md s4 S4; refs/122 S4, N6; IC-2 R7, R8): the ONE overload arbiter. Pure; wiring (the render-ahead gate
// ladder reading ExtrasAllowed instead of its own controller on the same signal; removal of the dead gap classes) waits for S3 on integ and
// S-FXDV (LaneInterfaces.h).
//
// Owner intent: OD:612 (3) (2026-11-12 15:13) "overload: TD steps down BEFORE de-judder extras, visible on Ctrl+I"; doc 91 C :79 (07:45)
// "Regulator = one alarm, optional extras only"; IC-2 R7 fixed order FillOff -> TdStepDown -> ExtrasStepDown -> CoreDeferred -> FeatureOff,
// hysteresis as the gate ladder (down after 2 s over, up after 5 s under u - 0.10), never two controllers on one signal, every step visible.
//
//  - The signal: u = (render ms + slot used ms) / (period - period/10), the load against the same budget law the slot is fed with (RenderBudget).
//    Over: u > 1. Under: u < 1 - 0.10. Between: hold (timers restart).
//  - Down: after 2 s continuously over, ONE stage deeper: FillOff; then TD one tier per stage until its lowest (tier 0); then the extras stage
//    (only when extras are active, else skipped); then CoreDeferred (core rows deferred one slot, counted; never dropped, R4); then FeatureOff
//    (the deepest: the feature that overloads is switched off visibly). A stage that does not apply (no TD running, no extras) is skipped.
//  - Up: after 5 s continuously under, ONE stage back, in reverse order (TD tiers come back one per stage).
//  - Never a presentation action: the arbiter only says what the jobs may do; no present is skipped, repeated or delayed by it (R8).
//  - Every state has its line (Ctrl+I + Config + Ctrl+Alt+D); None says "no overload".
//  - FillOff is VISIBLE by name (lead 13 ruling, 2026-11-14; no silent fallbacks): the caller names the optional fills now admitted
//    (SetOptionalFills, from the admission: every Fill-class job, the grain split P10 included while it is optional fill under the 08:30
//    ruling / Q4). While FillOff is engaged the state line names each fill it turns off ("overload: optional fill off: grain split, ..."),
//    FillLine() keeps that list for the deeper stages, and TakeFillNotice() hands the one-time notice once per engagement (re-armed when the
//    ladder returns to None). The grain split needs no stage of its own: FillOff is its step.
// Pure C++14.

#include <VP-V/core/SchedulerContract.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace VpSkeleton {

constexpr double kOverloadDownAfterMs = 2000.0;   // IC-2 R7
constexpr double kOverloadUpAfterMs = 5000.0;     // IC-2 R7
constexpr double kOverloadUnderMargin = 0.10;     // IC-2 R7: under = u < 1 - 0.10

inline const char* OverloadStepLine(OverloadStep s)
{
    switch (s) {
    case OverloadStep::None: return "overload: none";
    case OverloadStep::FillOff: return "overload: optional fill off";
    case OverloadStep::TdStepDown: return "overload: temporal detail stepped down";
    case OverloadStep::ExtrasStepDown: return "overload: temporal detail at its lowest tier, de-judder extras stepped down";
    case OverloadStep::CoreDeferred: return "overload: core work deferred to the next slot (never dropped)";
    case OverloadStep::FeatureOff: return "overload: feature off (load still over budget)";
    }
    return "overload: ?";
}

// The gate ladder's question: may render-ahead / de-judder extras run at their own level? (S4: it reads this instead of a second controller.)
inline bool ExtrasAllowed(const OverloadState& s) { return uint8_t(s.step) < uint8_t(OverloadStep::ExtrasStepDown); }
inline bool FillAllowed(const OverloadState& s) { return s.step == OverloadStep::None; }

// The load signal against the slot budget law (period - period/10). NaN / unmeasured -> 0 (no overload claimed without a measurement).
inline double OverloadLoad(const SlotBudget& b)
{
    if (!b.measured || !(b.periodMs > 0.0)) return 0.0;
    const double usable = b.periodMs - b.periodMs * 0.1;
    const double load = (b.renderMs > 0.0 ? b.renderMs : 0.0) + (b.usedMs > 0.0 ? b.usedMs : 0.0);
    return std::isfinite(load) ? load / usable : 0.0;
}

class OverloadArbiter {
public:
    OverloadArbiter() { Reset(); }
    // State().line may point into this arbiter (the FillOff line names the fills), so it is not copied.
    OverloadArbiter(const OverloadArbiter&) = delete;
    OverloadArbiter& operator=(const OverloadArbiter&) = delete;

    // The display names of the optional fills now admitted (Configure / admission refresh). FillOff turns these off and names them.
    void SetOptionalFills(const char* const* names, size_t count)
    {
        m_fills.clear();
        for (size_t i = 0; i < count; ++i)
            if (names != nullptr && names[i] != nullptr && names[i][0] != '\0') m_fills.emplace_back(names[i]);
        BuildFillLine();
        UpdateLine();
    }

    // "optional fill off: a, b" while FillOff or deeper is engaged; empty otherwise.
    const std::string& FillLine() const { return m_engagedFills; }

    // The one-time notice for this FillOff engagement (true once; re-armed after the ladder returns to None).
    bool TakeFillNotice(std::string* text)
    {
        if (!m_noticePending) return false;
        m_noticePending = false;
        if (text != nullptr) *text = m_engagedFills;
        return true;
    }

    // Once per source frame (render thread). tdTier = TD's current full tier (its own ladder's top; 0 = TD not running or already lowest).
    OverloadState Step(double nowMs, const SlotBudget& budget, bool extrasActive, uint8_t tdTier)
    {
        m_tdFull = tdTier;
        if (m_tdSteps > m_tdFull) m_tdSteps = m_tdFull;
        const double u = OverloadLoad(budget);
        const bool over = u > 1.0;
        const bool under = u < 1.0 - kOverloadUnderMargin;
        if (!std::isfinite(nowMs)) nowMs = m_lastMs;
        m_lastMs = nowMs;
        if (over) {
            m_underSince = -1.0;
            if (m_overSince < 0.0) m_overSince = nowMs;
            if (nowMs - m_overSince >= kOverloadDownAfterMs) { Deeper(extrasActive); m_overSince = nowMs; }
        } else if (under) {
            m_overSince = -1.0;
            if (m_underSince < 0.0) m_underSince = nowMs;
            if (nowMs - m_underSince >= kOverloadUpAfterMs) { Shallower(); m_underSince = nowMs; }
        } else {
            m_overSince = -1.0;
            m_underSince = -1.0;
        }
        if (m_state.step == OverloadStep::ExtrasStepDown && !extrasActive) { Shallower(); }   // the extras stage holds only while extras exist
        m_state.tdTier = uint8_t(m_tdFull - m_tdSteps);
        UpdateLine();
        return m_state;
    }

    void Reset()
    {
        m_state.step = OverloadStep::None;
        m_state.tdTier = 0;
        m_state.extrasLevel = 0;
        m_state.deferredCore = 0;
        m_state.line = OverloadStepLine(OverloadStep::None);
        m_overSince = m_underSince = -1.0;
        m_tdSteps = 0;
        m_tdFull = 0;
        m_engagedFills.clear();
        m_noticePending = false;
    }

    // The slot reports a deferred core job (counted on the state while CoreDeferred or deeper is engaged, else only in the total).
    void NoteCoreDeferred(uint32_t n) { m_deferredTotal += n; if (uint8_t(m_state.step) >= uint8_t(OverloadStep::CoreDeferred)) m_state.deferredCore += n; }

    const OverloadState& State() const { return m_state; }
    unsigned long long StepsDown() const { return m_down; }
    unsigned long long StepsUp() const { return m_up; }
    unsigned long long DeferredTotal() const { return m_deferredTotal; }

private:
    void BuildFillLine()
    {
        m_fillLine = "optional fill off: ";
        if (m_fills.empty()) m_fillLine += "no optional fill running";
        for (size_t i = 0; i < m_fills.size(); ++i) { if (i > 0) m_fillLine += ", "; m_fillLine += m_fills[i]; }
        m_fillStateLine = "overload: " + m_fillLine;
    }

    void UpdateLine()
    {
        const bool engaged = m_state.step != OverloadStep::None;
        if (engaged && m_engagedFills != m_fillLine) m_engagedFills = m_fillLine;   // the fills off follow the admitted set
        if (!engaged) m_engagedFills.clear();
        m_state.line = m_state.step == OverloadStep::FillOff ? m_fillStateLine.c_str() : OverloadStepLine(m_state.step);
    }

    void Deeper(bool extrasActive)
    {
        switch (m_state.step) {
        case OverloadStep::None:
            m_state.step = OverloadStep::FillOff;
            m_noticePending = true;   // one-time notice for this engagement, naming the fills
            break;
        case OverloadStep::FillOff:
            if (m_tdSteps < m_tdFull) { ++m_tdSteps; m_state.step = OverloadStep::TdStepDown; }
            else if (extrasActive) { m_state.step = OverloadStep::ExtrasStepDown; m_state.extrasLevel = -1; }
            else m_state.step = OverloadStep::CoreDeferred;
            break;
        case OverloadStep::TdStepDown:
            if (m_tdSteps < m_tdFull) ++m_tdSteps;
            else if (extrasActive) { m_state.step = OverloadStep::ExtrasStepDown; m_state.extrasLevel = -1; }
            else m_state.step = OverloadStep::CoreDeferred;
            break;
        case OverloadStep::ExtrasStepDown: m_state.step = OverloadStep::CoreDeferred; break;
        case OverloadStep::CoreDeferred: m_state.step = OverloadStep::FeatureOff; break;
        case OverloadStep::FeatureOff: return;   // deepest: nothing further (stays visible)
        }
        ++m_down;
    }

    void Shallower()
    {
        switch (m_state.step) {
        case OverloadStep::None: return;
        case OverloadStep::FeatureOff: m_state.step = OverloadStep::CoreDeferred; break;
        case OverloadStep::CoreDeferred:
            m_state.deferredCore = 0;
            m_state.step = m_state.extrasLevel < 0 ? OverloadStep::ExtrasStepDown : (m_tdSteps > 0 ? OverloadStep::TdStepDown : OverloadStep::FillOff);
            break;
        case OverloadStep::ExtrasStepDown:
            m_state.extrasLevel = 0;
            m_state.step = m_tdSteps > 0 ? OverloadStep::TdStepDown : OverloadStep::FillOff;
            break;
        case OverloadStep::TdStepDown:
            if (m_tdSteps > 0) --m_tdSteps;
            if (m_tdSteps == 0) m_state.step = OverloadStep::FillOff;
            break;
        case OverloadStep::FillOff: m_state.step = OverloadStep::None; m_noticePending = false; break;
        }
        ++m_up;
    }

    OverloadState m_state;
    std::vector<std::string> m_fills;
    std::string m_fillLine = "optional fill off: no optional fill running", m_fillStateLine = "overload: optional fill off: no optional fill running";
    std::string m_engagedFills;
    bool m_noticePending = false;
    double m_overSince = -1.0, m_underSince = -1.0, m_lastMs = 0.0;
    uint8_t m_tdSteps = 0, m_tdFull = 0;
    unsigned long long m_down = 0, m_up = 0, m_deferredTotal = 0;
};

}   // namespace VpSkeleton
