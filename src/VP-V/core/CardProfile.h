// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "ProductGraph.h"
#include "Products.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <vector>

// Skeleton S2: the card profile and the tier grant (design doc 25 B5, owner decisions doc 21
// item 3). Pure C++14: the renderer measures (QueryVideoMemoryInfo, a copy and a compute probe,
// the NvOF caps query and its ms per pair) and passes the numbers in; nothing here touches a GPU.
//
// Grant: every node keeps its live pool (an in-render product needs it anyway). The lane ring is
// granted T0 first (topological order), then T1 by measured ms saved per MB, then T2 the same way;
// a product is admitted only while the VRAM total stays under free VRAM x an adaptive headroom
// (no WDDM paging), and only when its same-frame inputs are laned too. A product without a lane
// is recomputed in the render (identical); a T0 that does not fit is a visible notice.
// System RAM holds only items < 1 MB per frame, CPU-made items or rarely touched items, never a
// full-resolution per-frame plane; they are uploaded ahead (the upload cost is charged).
namespace VpSkeleton {

struct AdapterKey {
    uint64_t luid = 0;
    uint64_t driverVersion = 0;
};

inline bool operator==(const AdapterKey& a, const AdapterKey& b)
{
    return a.luid == b.luid && a.driverVersion == b.driverVersion;
}

struct CardProfile {
    AdapterKey key;
    // Live values (re-queried at each graph build): QueryVideoMemoryInfo, local segment.
    uint64_t vramBudgetBytes = 0;
    uint64_t vramUsageBytes = 0;
    uint64_t systemRamFreeBytes = 0;
    // Measured once per adapter + driver (cached).
    double copyGBps = 0.0;        // device-local copy bandwidth
    double uploadGBps = 0.0;      // system RAM -> VRAM
    double computeScale = 1.0;    // compute relative to the reference card (RTX 3050 Ti laptop = 1.0)
    bool ofaPresent = false;      // NvOF caps query
    double ofaMsPerPairRef = 0.0; // measured ms per pair at the reference size (1920x1080)
    bool measured = false;

    uint64_t FreeVram() const { return vramBudgetBytes > vramUsageBytes ? vramBudgetBytes - vramUsageBytes : 0; }
    // A stable identity for the graph's rebuild key.
    uint64_t Identity() const { return key.luid * 1099511628211ull ^ key.driverVersion; }
};

// Cached measurements per adapter LUID + driver version. A driver update is a new entry.
class CardProfileCache {
public:
    const CardProfile* Find(const AdapterKey& key) const
    {
        for (const CardProfile& p : entries_)
            if (p.key == key)
                return &p;
        return nullptr;
    }

    void Store(const CardProfile& p)
    {
        for (CardProfile& e : entries_)
            if (e.key == p.key) {
                e = p;
                return;
            }
        entries_.push_back(p);
    }

    // The cached measurements with fresh live values.
    bool WithLiveValues(const AdapterKey& key, uint64_t budget, uint64_t usage, uint64_t systemFree,
                        CardProfile* out) const
    {
        const CardProfile* p = Find(key);
        if (!p || !out)
            return false;
        *out = *p;
        out->vramBudgetBytes = budget;
        out->vramUsageBytes = usage;
        out->systemRamFreeBytes = systemFree;
        return true;
    }

    size_t Size() const { return entries_.size(); }

private:
    std::vector<CardProfile> entries_;
};

// Adaptive headroom on free VRAM: shrinks at once on memory pressure (budget exceeded, a paging
// sign), grows a small step only after a dwell of calm windows. Deterministic.
constexpr double kHeadroomMin = 0.50;
constexpr double kHeadroomMax = 0.90;
constexpr double kHeadroomStart = 0.80;
constexpr double kHeadroomShrink = 0.85;
constexpr double kHeadroomGrowStep = 0.02;
constexpr int kHeadroomDwellWindows = 4;

class Headroom {
public:
    double Value() const { return value_; }

    void OnPressure()
    {
        value_ = std::max(kHeadroomMin, value_ * kHeadroomShrink);
        calm_ = 0;
    }

    void OnCalmWindow()
    {
        if (++calm_ >= kHeadroomDwellWindows) {
            value_ = std::min(kHeadroomMax, value_ + kHeadroomGrowStep);
            calm_ = 0;
        }
    }

private:
    double value_ = kHeadroomStart;
    int calm_ = 0;
};

constexpr uint64_t kSystemRamItemLimit = 1024u * 1024u;   // 1 MB per frame (doc 21 item 3)

// System RAM is allowed for items < 1 MB per frame, CPU-made items or rarely touched items, and
// never for a full-resolution per-frame plane.
inline bool SystemRamAllowed(const ProductDecl& d, uint64_t bytesPerFrame)
{
    if (IsFullResPerFrame(d.size))
        return false;
    return bytesPerFrame < kSystemRamItemLimit || d.backend == Backend::Cpu || d.rarelyTouched;
}

inline Residency ResidencyFor(const ProductDecl& d, uint64_t bytesPerFrame)
{
    return d.residency == Residency::SystemRam && SystemRamAllowed(d, bytesPerFrame) ? Residency::SystemRam
                                                                                      : Residency::Vram;
}

enum class Grant : uint8_t { Lane, InRender };

enum class GrantWhy : uint8_t { Granted, RenderOnly, OverBudget, InputInRender, NoGain };

inline const char* Token(GrantWhy w)
{
    switch (w) {
    case GrantWhy::Granted: return "granted";
    case GrantWhy::RenderOnly: return "render_only";
    case GrantWhy::OverBudget: return "over_budget";
    case GrantWhy::InputInRender: return "input_in_render";
    case GrantWhy::NoGain: return "no_gain";
    }
    return "?";
}

struct MeasuredCost {
    ProductId id = kNoProduct;
    double renderMs = 0.0;   // measured in-render cost (PassTimers), replaces the estimate
};

struct TierInput {
    double headroom = kHeadroomStart;
    double framePeriodMs = 0.0;              // measured source frame period (0 = unknown)
    std::vector<MeasuredCost> measured;
};

struct TierEntry {
    ProductId id = kNoProduct;
    Tier tier = Tier::T0;
    Backend backend = Backend::Cpu;
    Grant grant = Grant::InRender;
    GrantWhy why = GrantWhy::RenderOnly;
    Residency residency = Residency::Vram;
    uint64_t extraBytes = 0;     // the lane ring: what the lane costs on top of the live pool
    double msSaved = 0.0;        // render critical-path ms the lane removes (upload charged)
    double msPerMB = 0.0;
};

struct TierPlan {
    std::vector<TierEntry> entries;   // grant order
    uint64_t budgetBytes = 0;         // free VRAM x headroom
    uint64_t vramBytes = 0;           // live pools + granted rings
    uint64_t systemRamBytes = 0;
    bool t0OverBudget = false;        // an essential product runs in the render: visible notice
    bool liveOverBudget = false;      // even the live pools exceed the budget: visible notice
    bool ofaUsed = false;
    bool ofaFits = true;              // the optical-flow engine keeps up with the frame period
    double ofaLoad = 0.0;             // engine ms per pair / frame period
    std::string notice;               // plain-language status ("" = nothing to show)

    const TierEntry* Find(ProductId id) const
    {
        for (const TierEntry& e : entries)
            if (e.id == id)
                return &e;
        return nullptr;
    }
    bool Laned(ProductId id) const
    {
        const TierEntry* e = Find(id);
        return e && e->grant == Grant::Lane;
    }
};

constexpr double kReferencePixels = 1920.0 * 1080.0;

// Estimated in-render ms of a product on this card (a measured cost wins).
inline double RenderMsOf(const ProductDecl& d, const Graph& g, const CardProfile& card, const TierInput& in)
{
    for (const MeasuredCost& m : in.measured)
        if (m.id == d.id)
            return m.renderMs;
    const double pixels = double(g.settings.dims.width) * double(g.settings.dims.height);
    const double scale = pixels > 0.0 ? pixels / kReferencePixels : 0.0;
    if (d.backend == Backend::Ofa && card.ofaMsPerPairRef > 0.0)
        return card.ofaMsPerPairRef * scale;
    const double compute = card.computeScale > 0.0 ? card.computeScale : 1.0;
    return double(d.refRenderMs) * scale / (d.backend == Backend::Cpu ? 1.0 : compute);
}

inline TierPlan PlanTiers(const Registry& reg, const Graph& g, const CardProfile& card, const TierInput& in)
{
    TierPlan plan;
    const double headroom = std::min(kHeadroomMax, std::max(kHeadroomMin, in.headroom));
    plan.budgetBytes = uint64_t(double(card.FreeVram()) * headroom);
    const double uploadBytesPerMs = card.uploadGBps > 0.0 ? card.uploadGBps * 1.0e6 : 0.0;

    // Prepare one entry per node; live pools always count.
    std::vector<TierEntry> all;
    all.reserve(g.nodes.size());
    for (const Node& n : g.nodes) {
        const ProductDecl& d = reg.Get(n.id);
        TierEntry e;
        e.id = n.id;
        e.tier = n.tier;
        e.backend = n.backend;
        e.residency = ResidencyFor(d, n.bufferBytes);
        e.extraBytes = n.ringBytes;   // the lane's ring, on top of the live pool every node keeps
        double saved = RenderMsOf(d, g, card, in);
        if (e.residency == Residency::SystemRam) {
            saved -= uploadBytesPerMs > 0.0 ? double(n.bufferBytes) / uploadBytesPerMs : saved;
            plan.systemRamBytes += n.liveBytes;
        } else {
            plan.vramBytes += n.liveBytes;
        }
        e.msSaved = saved > 0.0 ? saved : 0.0;
        const double mb = double(e.extraBytes) / (1024.0 * 1024.0);
        e.msPerMB = mb > 0.0 ? e.msSaved / mb : e.msSaved * 1.0e9;   // free items rank first
        if (n.backend == Backend::Ofa) {
            plan.ofaUsed = true;
            if (!card.ofaPresent)
                plan.ofaFits = false;   // the graph was built for an engine this card lacks
            if (in.framePeriodMs > 0.0) {
                plan.ofaLoad = RenderMsOf(d, g, card, TierInput()) / in.framePeriodMs;
                plan.ofaFits = plan.ofaFits && plan.ofaLoad <= 1.0;
            }
        }
        all.push_back(e);
    }
    if (plan.vramBytes > plan.budgetBytes)
        plan.liveOverBudget = true;

    auto inputsLaned = [&](ProductId id) {
        const Node* n = g.Find(id);
        if (!n)
            return false;
        for (size_t k = 0; k < n->inputCount; ++k)
            if (n->inputs[k].lag == 0 && !plan.Laned(n->inputs[k].product))
                return false;
        return true;
    };

    auto admit = [&](TierEntry e) {
        if (e.backend == Backend::RenderOnly) {
            e.grant = Grant::InRender;
            e.why = GrantWhy::RenderOnly;
        } else if (!inputsLaned(e.id)) {
            e.grant = Grant::InRender;
            e.why = GrantWhy::InputInRender;
        } else if (e.tier != Tier::T0 && e.msSaved <= 0.0) {
            e.grant = Grant::InRender;
            e.why = GrantWhy::NoGain;
        } else if (e.residency == Residency::SystemRam) {
            if (plan.systemRamBytes + e.extraBytes <= card.systemRamFreeBytes) {
                plan.systemRamBytes += e.extraBytes;
                e.grant = Grant::Lane;
                e.why = GrantWhy::Granted;
            } else {
                e.grant = Grant::InRender;
                e.why = GrantWhy::OverBudget;
            }
        } else if (plan.vramBytes + e.extraBytes <= plan.budgetBytes) {
            plan.vramBytes += e.extraBytes;
            e.grant = Grant::Lane;
            e.why = GrantWhy::Granted;
        } else {
            e.grant = Grant::InRender;
            e.why = GrantWhy::OverBudget;
        }
        if (e.tier == Tier::T0 && e.why == GrantWhy::OverBudget)
            plan.t0OverBudget = true;
        plan.entries.push_back(e);
    };

    // T0 in topological order (graph order: inputs before readers).
    for (const TierEntry& e : all)
        if (e.tier == Tier::T0)
            admit(e);
    // T1, then T2: ms saved per MB, best first; ties by product id. A reader ranked above its own
    // same-frame input waits for it (a second pass picks it up).
    for (Tier t : { Tier::T1, Tier::T2 }) {
        std::vector<TierEntry> tier;
        for (const TierEntry& e : all)
            if (e.tier == t)
                tier.push_back(e);
        std::stable_sort(tier.begin(), tier.end(), [](const TierEntry& a, const TierEntry& b) {
            if (a.msPerMB != b.msPerMB)
                return a.msPerMB > b.msPerMB;
            return a.id < b.id;
        });
        std::vector<TierEntry> waiting;
        for (const TierEntry& e : tier) {
            if (e.backend != Backend::RenderOnly && !inputsLaned(e.id)) {
                bool inputInTier = false;
                const Node* n = g.Find(e.id);
                for (size_t k = 0; n && k < n->inputCount; ++k)
                    for (const TierEntry& o : tier)
                        if (n->inputs[k].lag == 0 && o.id == n->inputs[k].product && !plan.Find(o.id))
                            inputInTier = true;
                if (inputInTier) {
                    waiting.push_back(e);
                    continue;
                }
            }
            admit(e);
        }
        for (const TierEntry& e : waiting)
            admit(e);
    }

    // Plain-language notice (no silent fallbacks).
    std::string notice;
    auto add = [&notice](const char* s) {
        if (!notice.empty())
            notice += "; ";
        notice += s;
    };
    if (plan.liveOverBudget)
        add("Analysis memory: not enough free video memory even for in-render products");
    else if (plan.t0OverBudget)
        add("Analysis memory: essential analysis runs in the render (not enough free video memory)");
    if (plan.ofaUsed && !plan.ofaFits)
        add("Vector engine slower than the frame rate: reduced engine settings needed");
    plan.notice = notice;
    return plan;
}

} // namespace VpSkeleton
