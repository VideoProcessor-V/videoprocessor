// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "Indicator.h"
#include "Products.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

// Skeleton S5: the analysis-lane regulator (design B8, audit 25a 3.8/3.9). Render thread only.
//
// It keeps one number, the LEVEL: how many steps of a fixed LADDER are granted. The ladder (from the
// minimum upward) is every sheddable T0 product, then every T1, then every T2 (given order inside a
// tier), then, on non-deterministic runs with an optical-flow product, the OFA variant steps
// (quarter -> preset -> extras). A product below its step is shed: moved into the render when its
// fallback is identical (only with accounting), else dropped when it declares a reduced mode and the
// run is not deterministic, else forced live (it stays laned; the seal live-fills it when late, and
// the cost is charged). Products that may not be shed sit permanently at their full state.
//
// Once per window of frames (derived from the measured period, never a fixed rate) the 5th
// percentile of slack is compared with a margin built from the render-only time spread: below it the
// level halves (once per window), well above it for two windows the level grows by one. Header-only,
// pure C++14, deterministic: same inputs give the same decisions and traces.
namespace VpSkeleton {

enum class ProductState : uint8_t { Laned, MovedToRender, Dropped, ForcedLive };

enum class RegAction : uint8_t {
    None,
    BackOff,
    Grow,
    MoveToRender,
    MoveToLane,
    Drop,
    Restore,
    ForcedLive,
    OfaStepDown,
    OfaStepUp,
    ConservativeFloor,
    StepDownCut,
    StepDownMinor,
    SkipToNext,
    PinnedMin
};

struct RegProduct {
    ProductId id = kNoProduct;
    Tier tier = Tier::T0;
    bool identical = true;      // Variants::Identical: the in-render fallback gives the same content
    bool reducedMode = false;   // every consumer declares a reduced mode (the product may be dropped)
    bool ofa = false;
    double inRenderMs = 0.0;    // measured in-render cost
};

struct RegConfig {
    double windowSeconds = 2.0;
    double k = 2.0;
    double marginMinFrac = 0.02;   // of the frame period
    double hysteresisFrac = 0.05;
    int dwellWindows = 2;
    double backoff = 0.5;
    double emaLongAlpha = 0.1;
    uint8_t ofaSteps = 3;          // extras, preset, quarter
};

struct RegTrace {
    uint64_t seq = 0;
    RegAction action = RegAction::None;
    ProductId id = kNoProduct;
    uint16_t level = 0;
    float slackLowMs = 0.f;
    float marginMs = 0.f;
    float predictedDeltaMs = 0.f;
};

class Regulator {
public:
    static constexpr size_t kTraceCapacity = 256;

    void Configure(const std::vector<RegProduct>& products, double framePeriodMs, bool deterministic,
                   const RegConfig& cfg = RegConfig())
    {
        cfg_ = cfg;
        det_ = deterministic;
        periodMs_ = framePeriodMs > 0.0 ? framePeriodMs : 1.0;
        items_.clear();
        for (const RegProduct& p : products) {
            Item it;
            it.p = p;
            items_.push_back(it);
        }

        // Ladder: tier order, given order inside a tier; only products that may be shed are steps.
        steps_.clear();
        t0Steps_ = 0;
        const Tier tiers[3] = { Tier::T0, Tier::T1, Tier::T2 };
        for (size_t t = 0; t < 3; ++t) {
            for (size_t i = 0; i < items_.size(); ++i) {
                if (items_[i].p.tier != tiers[t] || !Sheddable(items_[i].p))
                    continue;
                items_[i].step = int(steps_.size());
                steps_.push_back(i);
                if (t == 0)
                    ++t0Steps_;
            }
        }
        laneSteps_ = steps_.size();
        bool anyOfa = false;
        for (const Item& it : items_)
            anyOfa = anyOfa || it.p.ofa;
        ofaSteps_ = (!det_ && anyOfa) ? size_t(cfg_.ofaSteps) : 0;
        maxLevel_ = uint16_t(laneSteps_ + ofaSteps_);
        level_ = maxLevel_;

        windowFrames_ = size_t(std::max(8.0, std::floor(cfg_.windowSeconds * 1000.0 / periodMs_ + 0.5)));
        slack_.clear();
        render_.clear();
        slack_.reserve(windowFrames_);
        render_.reserve(windowFrames_);

        dwell_ = 0;
        ema_ = 0.0;
        emaInit_ = false;
        freeze_ = 0;
        marginMs_ = std::max(cfg_.marginMinFrac * periodMs_, 0.0);
        decisionMargin_ = marginMs_;
        lastSlackLow_ = 0.0;
        haveWindow_ = false;
        pinned_ = false;
        lastSeq_ = 0;
        traceCount_ = 0;
    }

    // ---- per frame ------------------------------------------------------------------------------
    void OnFrame(uint64_t seq, double slackMs, double renderOnlyMs, bool deadlineMissed)
    {
        lastSeq_ = seq;
        if (windowFrames_ == 0)
            return;
        slack_.push_back(deadlineMissed && slackMs > 0.0 ? 0.0 : slackMs);
        render_.push_back(renderOnlyMs);
        if (slack_.size() >= windowFrames_)
            EndWindow(seq);
    }

    void OnIndicatorChange(VpIndicator::CounterMask changed)
    {
        const VpIndicator::CounterMask hard = VpIndicator::Bit(VpIndicator::kFull) | VpIndicator::Bit(VpIndicator::kGraph);
        if ((changed & hard) != 0) {
            if (level_ > t0Steps_) {
                SetLevel(uint16_t(t0Steps_), RegAction::ConservativeFloor, false);
            }
            else
                Trace(RegAction::ConservativeFloor, kNoProduct, 0.f);
        }
        else if ((changed & VpIndicator::Bit(VpIndicator::kContinuity)) != 0)
            StepDown(RegAction::StepDownMinor);
    }

    void OnCut() { StepDown(RegAction::StepDownCut); }

    // Per-window lateness delta of a laned product (from the runner/slot counters).
    void OnLaneLate(ProductId id, uint32_t lateFrames)
    {
        for (Item& it : items_)
            if (it.p.id == id) {
                it.lateFrames += lateFrames;
                return;
            }
    }

    // ---- queries --------------------------------------------------------------------------------
    ProductState StateOf(ProductId id) const
    {
        for (const Item& it : items_)
            if (it.p.id == id)
                return it.state;
        return ProductState::Laned;
    }
    // 0 = full settings; ofaSteps = the lowest (quarter).
    uint8_t OfaStep() const
    {
        if (ofaSteps_ == 0)
            return 0;
        const size_t granted = level_ > laneSteps_ ? size_t(level_) - laneSteps_ : 0;
        return uint8_t(ofaSteps_ - std::min(granted, ofaSteps_));
    }
    uint16_t Level() const { return level_; }
    uint16_t MaxLevel() const { return maxLevel_; }
    double MarginMs() const { return marginMs_; }
    bool PinnedAtMinimum() const { return pinned_; }
    size_t WindowFrames() const { return windowFrames_; }
    size_t T0StepCount() const { return t0Steps_; }

    std::vector<ProductId> Dropped() const { return IdsIn(ProductState::Dropped); }
    std::vector<ProductId> ForcedLiveIds() const { return IdsIn(ProductState::ForcedLive); }
    double ForcedChargedMs() const
    {
        double s = 0.0;
        for (const Item& it : items_)
            if (it.state == ProductState::ForcedLive)
                s += it.p.inRenderMs;
        return s;
    }
    // True while a T0 product runs in the render.
    bool T0InRender() const
    {
        for (const Item& it : items_)
            if (it.p.tier == Tier::T0 && it.state == ProductState::MovedToRender)
                return true;
        return false;
    }

    // Plain language; "" when nothing is reduced.
    std::string Notice() const
    {
        std::string out;
        auto list = [](const std::vector<ProductId>& v) {
            std::string s;
            for (size_t i = 0; i < v.size(); ++i)
                s += (i ? ", " : "") + std::to_string(unsigned(v[i]));
            return s;
        };
        std::vector<ProductId> moved = IdsIn(ProductState::MovedToRender);
        const std::vector<ProductId> dropped = Dropped();
        const std::vector<ProductId> forced = ForcedLiveIds();
        if (!dropped.empty())
            out += "reduced: analysis products " + list(dropped) + " dropped";
        if (!moved.empty())
            out += std::string(out.empty() ? "" : "; ") + "moved into the render: products " + list(moved);
        if (!forced.empty()) {
            char b[48];
            std::snprintf(b, sizeof(b), "%.2f", ForcedChargedMs());
            out += std::string(out.empty() ? "" : "; ") + "forced live: products " + list(forced) +
                   ", charged " + b + " ms";
        }
        if (OfaStep() != 0)
            out += std::string(out.empty() ? "" : "; ") + "optical-flow analysis reduced (step " +
                   std::to_string(unsigned(OfaStep())) + ")";
        if (pinned_)
            out += std::string(out.empty() ? "" : "; ") + "analysis at minimum";
        return out;
    }

    // Oldest first; returns the number written.
    size_t Traces(RegTrace* out, size_t max) const
    {
        if (out == nullptr || max == 0)
            return 0;
        const size_t retained = traceCount_ < kTraceCapacity ? size_t(traceCount_) : kTraceCapacity;
        const size_t n = retained < max ? retained : max;
        const uint64_t first = traceCount_ - n;
        for (size_t k = 0; k < n; ++k)
            out[k] = trace_[size_t((first + k) % kTraceCapacity)];
        return n;
    }

private:
    struct Item {
        RegProduct p;
        int step = -1;                       // ladder index, -1: not a step (permanently full)
        ProductState state = ProductState::Laned;
        uint32_t lateFrames = 0;             // this window
        uint32_t lateWindows = 0;            // consecutive late windows
        uint32_t calmWindows = 0;            // consecutive calm windows (while lateShed)
        bool lateShed = false;
    };

    bool Sheddable(const RegProduct& p) const
    {
        return p.identical || (p.reducedMode && !det_);
    }

    bool IsShed(const Item& it) const
    {
        if (it.step < 0)
            return false;
        const size_t laneLevel = std::min<size_t>(level_, laneSteps_);
        return size_t(it.step) >= laneLevel || it.lateShed;
    }

    std::vector<ProductId> IdsIn(ProductState s) const
    {
        std::vector<ProductId> v;
        for (const Item& it : items_)
            if (it.state == s)
                v.push_back(it.p.id);
        std::sort(v.begin(), v.end());
        return v;
    }

    void Trace(RegAction a, ProductId id, float delta)
    {
        RegTrace& t = trace_[size_t(traceCount_ % kTraceCapacity)];
        t.seq = lastSeq_;
        t.action = a;
        t.id = id;
        t.level = level_;
        t.slackLowMs = float(lastSlackLow_);
        t.marginMs = float(decisionMargin_);
        t.predictedDeltaMs = delta;
        ++traceCount_;
    }

    void SetLevel(uint16_t newLevel, RegAction why, bool fresh)
    {
        if (newLevel > maxLevel_)
            newLevel = maxLevel_;
        const uint16_t old = level_;
        const uint8_t ofaOld = OfaStep();
        level_ = newLevel;
        dwell_ = newLevel < old ? 0 : dwell_;
        Trace(why, kNoProduct, 0.f);
        const uint8_t ofaNew = OfaStep();
        if (ofaNew > ofaOld)
            Trace(RegAction::OfaStepDown, kNoProduct, 0.f);
        else if (ofaNew < ofaOld)
            Trace(RegAction::OfaStepUp, kNoProduct, 0.f);
        Recompute(fresh);
    }

    void StepDown(RegAction why)
    {
        if (level_ > 0)
            SetLevel(uint16_t(level_ - 1), why, false);
        else
            Trace(why, kNoProduct, 0.f);
    }

    // Derives every product's state from the level (and the late step-downs). The move accounting:
    // a product moved into the render must fit, together with every product already moved, in
    // slackLow - margin of the last window; otherwise it is dropped (reduced mode, non-deterministic)
    // or forced live. `fresh` re-decides every shed product; otherwise only newly shed products are
    // decided and the others keep their state (no churn between windows).
    void Recompute(bool fresh)
    {
        const double budget = haveWindow_ ? lastSlackLow_ - decisionMargin_ : 0.0;
        std::vector<ProductState> next(items_.size(), ProductState::Laned);
        std::vector<bool> undecided(items_.size(), false);
        double cum = 0.0;
        for (size_t i = 0; i < items_.size(); ++i) {
            const Item& it = items_[i];
            if (!IsShed(it))
                continue;
            if (!fresh && it.state != ProductState::Laned) {
                next[i] = it.state;
                if (it.state == ProductState::MovedToRender)
                    cum += it.p.inRenderMs;
            }
            else
                undecided[i] = true;
        }
        // Highest tier (highest step) is decided first: it is moved first, the lowest tier is dropped first.
        for (size_t s = steps_.size(); s-- > 0;) {
            const size_t i = steps_[s];
            if (!undecided[i])
                continue;
            const RegProduct& p = items_[i].p;
            if (p.identical) {
                if (cum + p.inRenderMs <= budget + 1e-9) {
                    next[i] = ProductState::MovedToRender;
                    cum += p.inRenderMs;
                }
                else
                    next[i] = (p.reducedMode && !det_) ? ProductState::Dropped : ProductState::ForcedLive;
            }
            else
                next[i] = ProductState::Dropped;   // sheddable + non-identical implies reducedMode && !det
        }
        for (size_t i = 0; i < items_.size(); ++i) {
            Item& it = items_[i];
            if (it.state == next[i])
                continue;
            const ProductState from = it.state;
            it.state = next[i];
            const float cost = float(it.p.inRenderMs);
            switch (next[i]) {
            case ProductState::MovedToRender: Trace(RegAction::MoveToRender, it.p.id, cost); break;
            case ProductState::Dropped: Trace(RegAction::Drop, it.p.id, 0.f); break;
            case ProductState::ForcedLive: Trace(RegAction::ForcedLive, it.p.id, cost); break;
            case ProductState::Laned:
                if (from == ProductState::MovedToRender)
                    Trace(RegAction::MoveToLane, it.p.id, -cost);
                else
                    Trace(RegAction::Restore, it.p.id, from == ProductState::ForcedLive ? -cost : 0.f);
                break;
            }
        }
    }

    static double Percentile(std::vector<double>& sorted, double q)
    {
        const size_t n = sorted.size();
        return sorted[size_t(std::floor(q * double(n - 1)))];
    }

    void EndWindow(uint64_t seq)
    {
        lastSeq_ = seq;
        std::vector<double> s(slack_);
        std::vector<double> r(render_);
        slack_.clear();
        render_.clear();
        std::sort(s.begin(), s.end());
        std::sort(r.begin(), r.end());
        const double slackLow = Percentile(s, 0.05);
        const double p50 = Percentile(r, 0.50);
        const double p99 = Percentile(r, 0.99);
        const double spread = std::max(0.0, p99 - p50);

        // Margin from the render-only spread; the long EMA is frozen after a back-off.
        const bool frozen = freeze_ > 0;
        const double cand = emaInit_ ? ema_ + cfg_.emaLongAlpha * (spread - ema_) : spread;
        const double useEma = frozen && emaInit_ ? ema_ : cand;
        const double floorMargin = cfg_.marginMinFrac * periodMs_;
        const double m = std::max(floorMargin, cfg_.k * useEma);
        const bool low = slackLow < m;

        haveWindow_ = true;
        lastSlackLow_ = slackLow;
        decisionMargin_ = m;

        if (frozen)
            --freeze_;
        if (low) {
            freeze_ = 1;   // this window and the next do not feed the EMA
            marginMs_ = emaInit_ ? std::max(floorMargin, cfg_.k * ema_) : m;
        }
        else if (!frozen) {
            ema_ = cand;
            emaInit_ = true;
            marginMs_ = m;
        }
        else
            marginMs_ = m;

        // Lateness of laned products: skip to N+1 now; a second late window steps that product down.
        bool lateChanged = false;
        for (Item& it : items_) {
            if (it.lateFrames > 0) {
                Trace(RegAction::SkipToNext, it.p.id, 0.f);
                ++it.lateWindows;
                it.calmWindows = 0;
                if (it.lateWindows >= 2 && it.step >= 0 && !it.lateShed) {
                    it.lateShed = true;
                    lateChanged = true;
                }
            }
            else {
                it.lateWindows = 0;
                if (it.lateShed && ++it.calmWindows >= uint32_t(cfg_.dwellWindows < 1 ? 1 : cfg_.dwellWindows)) {
                    it.lateShed = false;
                    it.calmWindows = 0;
                    lateChanged = true;
                }
            }
            it.lateFrames = 0;
        }

        if (low) {
            dwell_ = 0;
            if (level_ > 0) {
                pinned_ = false;
                SetLevel(uint16_t(std::floor(double(level_) * cfg_.backoff)), RegAction::BackOff, true);
                return;
            }
            if (!pinned_) {
                pinned_ = true;
                Trace(RegAction::PinnedMin, kNoProduct, 0.f);
            }
        }
        else {
            pinned_ = false;
            if (slackLow >= m + cfg_.hysteresisFrac * periodMs_) {
                if (++dwell_ >= cfg_.dwellWindows && level_ < maxLevel_) {
                    dwell_ = 0;
                    SetLevel(uint16_t(level_ + 1), RegAction::Grow, false);
                    return;
                }
            }
            else
                dwell_ = 0;
        }
        if (lateChanged)
            Recompute(false);
    }

    RegConfig cfg_;
    bool det_ = false;
    double periodMs_ = 1.0;
    std::vector<Item> items_;
    std::vector<size_t> steps_;      // ladder: item index per step, lowest first
    size_t laneSteps_ = 0;
    size_t ofaSteps_ = 0;
    size_t t0Steps_ = 0;
    uint16_t level_ = 0;
    uint16_t maxLevel_ = 0;
    size_t windowFrames_ = 0;
    std::vector<double> slack_;
    std::vector<double> render_;
    int dwell_ = 0;
    double ema_ = 0.0;
    bool emaInit_ = false;
    int freeze_ = 0;
    double marginMs_ = 0.0;          // the committed margin (frozen across a back-off)
    double decisionMargin_ = 0.0;    // the margin the last decision used
    double lastSlackLow_ = 0.0;
    bool haveWindow_ = false;
    bool pinned_ = false;
    uint64_t lastSeq_ = 0;
    std::array<RegTrace, kTraceCapacity> trace_{};
    uint64_t traceCount_ = 0;
};

} // namespace VpSkeleton
