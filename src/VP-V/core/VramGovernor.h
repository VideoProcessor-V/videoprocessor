// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "CardProfile.h"
#include "ProductGraph.h"
#include "ProductSlots.h"
#include "Products.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// Skeleton S2: the ONE VRAM governor (coordinator input 2026-10-31, bug hunt doc 38 H2/M3) and the
// tier plan built on it. There is one budget and one shed order for all of VP's video memory:
//
//   budget = (WDDM budget - usage by others) x adaptive headroom
//   usage by others = CurrentUsage - VP's own current allocation (VP's own memory is re-planned,
//   never counted twice, so grants do not flap between rebuilds)
//
// Every VRAM user registers levels (level 0 = full need, each later level smaller, the last the
// minimum; 0 bytes = off) with a tier, a shed order and whether its steps lose picture quality.
// Over budget, users step down ONE level at a time: T2 before T1 before T0; within a tier, lossless
// steps (fewer render-ahead buffers or upload slots, a product recomputed identically in the
// render) before lossy ones, then by shed order, then id. Doc 38's order is render-ahead depth,
// early-upload slots, TemporalDetail future slots. Every user below level 0 gives one plain status
// line (no silent fallbacks); a plan that cannot fit even at every minimum says so. Fixed users
// (libplacebo intermediates, capture planes) have one level and only count.
//
// Skeleton products are users too (PlanTiers): a lane product needs ring + live pool (level 0) and
// can fall back to the live pool alone (level 1 = in-render, identical). T0 products shed readers
// first (reverse topological order); T1/T2 products shed the lowest measured ms saved per MB first,
// so the grant is "T0 first, then T1/T2 by ms saved per MB" expressed as one shed order. A product
// whose same-frame input is in the render goes in the render too. System-RAM products are admitted
// against free system RAM instead. Pure C++14: the renderer polls QueryVideoMemoryInfo and applies
// the levels (ApplyLaneGrants for the slots); nothing here allocates on the GPU.
namespace VpSkeleton {

enum VramUserId : uint16_t {
    kUserLibplacebo = 1,       // intermediates (fixed)
    kUserCapturePlanes,        // capture unpack and decoded planes (fixed)
    kUserRenderAhead,          // render-ahead swapchain depth
    kUserEarlyUpload,          // early-upload pool slots
    kUserNvofProducer,         // optical-flow engine + producer ring
    kUserTemporalDetail,       // refstore (+ future slots)
    kUserCameraModel,          // camera-model textures
    kUserInterpolator,         // interpolator engine + grain bands
    kUserEnhancers,            // texture restore, detail pulse, debander extras
    kUserScratchArena,         // SA1: the shared scratch arena for per-render temporaries (GpuAllocator.h)
    kUserProductBase = 0x100,  // + ProductId: skeleton lane products
};

struct VramUser {
    uint16_t id = 0;
    const char* name = "";
    Tier tier = Tier::T0;
    uint8_t shedOrder = 0;            // within a tier and lossless class: lower sheds first
    bool lossless = false;            // its steps keep the picture (latency, robustness or render time)
    std::vector<uint64_t> levels;     // bytes, non-increasing; levels[0] = full need
    std::vector<const char*> levelText;   // optional: the reduced mode at each level (status wording)
};

struct GovernorInput {
    CardProfile card;                 // vramBudgetBytes / vramUsageBytes (live)
    uint64_t ownUsageBytes = 0;       // VP's current allocation (inside vramUsageBytes)
    uint64_t transientBytes = 0;      // declared transient on top (e.g. a pool reopening at a new size)
    double headroom = kHeadroomStart;
    std::vector<VramUser> users;
};

struct UserGrant {
    uint16_t id = 0;
    const char* name = "";
    Tier tier = Tier::T0;
    size_t level = 0;
    uint64_t bytes = 0;
    bool reduced = false;   // below its full need
    bool shed = false;      // at 0 bytes
};

struct GovernorPlan {
    std::vector<UserGrant> grants;    // registration order
    uint64_t budgetBytes = 0;
    uint64_t totalBytes = 0;          // granted, plus the transient
    bool fits = true;
    uint32_t steps = 0;               // level steps taken (trace)
    std::vector<std::string> lines;   // one status line per reduced user, plus an over-budget line

    const UserGrant* Find(uint16_t id) const
    {
        for (const UserGrant& g : grants)
            if (g.id == id)
                return &g;
        return nullptr;
    }
};

inline uint64_t OthersUsage(const CardProfile& card, uint64_t own)
{
    return card.vramUsageBytes > own ? card.vramUsageBytes - own : 0;
}

inline uint64_t GovernorBudget(const CardProfile& card, uint64_t own, double headroomIn)
{
    const double headroom = std::min(kHeadroomMax, std::max(kHeadroomMin, headroomIn));
    const uint64_t others = OthersUsage(card, own);
    const uint64_t avail = card.vramBudgetBytes > others ? card.vramBudgetBytes - others : 0;
    return uint64_t(double(avail) * headroom);
}

inline uint64_t LevelBytes(const VramUser& u, size_t level)
{
    if (u.levels.empty())
        return 0;
    return u.levels[level < u.levels.size() ? level : u.levels.size() - 1];
}

inline void FinishGovernorPlan(const GovernorInput& in, const std::vector<size_t>& level, GovernorPlan& plan)
{
    plan.grants.clear();
    plan.lines.clear();
    uint64_t t = in.transientBytes;
    for (size_t i = 0; i < in.users.size(); ++i)
        t += LevelBytes(in.users[i], level[i]);
    plan.totalBytes = t;
    plan.fits = t <= plan.budgetBytes;
    for (size_t i = 0; i < in.users.size(); ++i) {
        const VramUser& u = in.users[i];
        UserGrant g;
        g.id = u.id;
        g.name = u.name;
        g.tier = u.tier;
        g.level = level[i];
        g.bytes = LevelBytes(u, level[i]);
        g.reduced = level[i] > 0;
        g.shed = g.reduced && g.bytes == 0;
        plan.grants.push_back(g);
        if (g.reduced) {
            std::string line = std::string("Video memory: ") + u.name;
            if (level[i] < u.levelText.size() && u.levelText[level[i]] != nullptr)
                line += std::string(" ") + u.levelText[level[i]];
            else
                line += g.shed ? " off" : " reduced";
            line += " (not enough free video memory)";
            plan.lines.push_back(line);
        }
    }
    if (!plan.fits)
        plan.lines.push_back("Video memory: over budget even at minimum settings");
}

// The plan for `in`; `levels` (optional) receives each user's level.
inline GovernorPlan PlanVram(const GovernorInput& in, std::vector<size_t>* levels = nullptr)
{
    GovernorPlan plan;
    plan.budgetBytes = GovernorBudget(in.card, in.ownUsageBytes, in.headroom);
    std::vector<size_t> level(in.users.size(), 0);
    auto total = [&]() {
        uint64_t t = in.transientBytes;
        for (size_t i = 0; i < in.users.size(); ++i)
            t += LevelBytes(in.users[i], level[i]);
        return t;
    };
    // Deterministic shed rank: tier (T2 first), lossless before lossy, shed order, id.
    auto before = [&](size_t a, size_t b) {
        const VramUser& x = in.users[a];
        const VramUser& y = in.users[b];
        if (x.tier != y.tier)
            return uint8_t(x.tier) > uint8_t(y.tier);
        if (x.lossless != y.lossless)
            return x.lossless;
        if (x.shedOrder != y.shedOrder)
            return x.shedOrder < y.shedOrder;
        return x.id < y.id;
    };
    uint64_t t = total();
    while (t > plan.budgetBytes) {
        size_t pick = in.users.size();
        for (size_t i = 0; i < in.users.size(); ++i) {
            if (level[i] + 1 >= in.users[i].levels.size())
                continue;   // at its minimum (or fixed)
            if (pick == in.users.size() || before(i, pick))
                pick = i;
        }
        if (pick == in.users.size())
            break;
        ++level[pick];
        ++plan.steps;
        t = total();
    }
    FinishGovernorPlan(in, level, plan);
    if (levels)
        *levels = level;
    return plan;
}

// ---- the tier plan (skeleton products as governor users) ----

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
    uint64_t ownUsageBytes = 0;              // VP's current VRAM allocation (inside CurrentUsage)
    uint64_t transientBytes = 0;
    std::vector<VramUser> otherUsers;        // every non-skeleton VRAM user (early upload, ...)
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
    std::vector<TierEntry> entries;   // grant order: T0 topological, then T1, T2 by ms per MB
    GovernorPlan governor;            // the one plan for every VRAM user (its lines are the notices)
    uint64_t budgetBytes = 0;         // the governor's budget
    uint64_t vramBytes = 0;           // the governor's total (all users)
    uint64_t systemRamBytes = 0;
    bool t0OverBudget = false;        // an essential product runs in the render: visible notice
    bool liveOverBudget = false;      // over budget even at every minimum: visible notice
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
inline double RenderMsOf(const ProductDecl& d, const Graph& g, const CardProfile& card,
                         const std::vector<MeasuredCost>& measured)
{
    for (const MeasuredCost& m : measured)
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
    const double uploadBytesPerMs = card.uploadGBps > 0.0 ? card.uploadGBps * 1.0e6 : 0.0;

    // One entry per node (graph order = topological).
    std::vector<TierEntry> all;
    std::vector<char> canLane;
    all.reserve(g.nodes.size());
    for (const Node& n : g.nodes) {
        const ProductDecl& d = reg.Get(n.id);
        TierEntry e;
        e.id = n.id;
        e.tier = n.tier;
        e.backend = n.backend;
        e.residency = ResidencyFor(d, n.bufferBytes);
        e.extraBytes = n.ringBytes;
        double saved = RenderMsOf(d, g, card, in.measured);
        if (e.residency == Residency::SystemRam)
            saved -= uploadBytesPerMs > 0.0 ? double(n.bufferBytes) / uploadBytesPerMs : saved;
        e.msSaved = saved > 0.0 ? saved : 0.0;
        const double mb = double(e.extraBytes) / (1024.0 * 1024.0);
        e.msPerMB = mb > 0.0 ? e.msSaved / mb : e.msSaved * 1.0e9;   // free items rank first
        if (n.backend == Backend::Ofa) {
            plan.ofaUsed = true;
            if (!card.ofaPresent)
                plan.ofaFits = false;   // the graph was built for an engine this card lacks
            if (in.framePeriodMs > 0.0) {
                plan.ofaLoad = RenderMsOf(d, g, card, std::vector<MeasuredCost>()) / in.framePeriodMs;
                plan.ofaFits = plan.ofaFits && plan.ofaLoad <= 1.0;
            }
        }
        if (n.backend == Backend::RenderOnly || e.residency == Residency::Transient)
            e.why = GrantWhy::RenderOnly;   // a Transient product has no ring: the arena user counts it
        else if (e.tier != Tier::T0 && e.msSaved <= 0.0)
            e.why = GrantWhy::NoGain;
        else
            e.why = GrantWhy::Granted;
        canLane.push_back(e.why == GrantWhy::Granted ? 1 : 0);
        all.push_back(e);
    }

    // Shed order among the products of one tier: T0 readers first (reverse topological); T1/T2 the
    // lowest ms saved per MB first, ties by id.
    std::vector<uint8_t> shed(all.size(), 0);
    for (Tier t : { Tier::T0, Tier::T1, Tier::T2 }) {
        std::vector<size_t> idx;
        for (size_t i = 0; i < all.size(); ++i)
            if (all[i].tier == t)
                idx.push_back(i);
        if (t == Tier::T0)
            std::reverse(idx.begin(), idx.end());
        else
            std::stable_sort(idx.begin(), idx.end(), [&](size_t a, size_t b) {
                if (all[a].msPerMB != all[b].msPerMB)
                    return all[a].msPerMB < all[b].msPerMB;
                return all[a].id < all[b].id;
            });
        for (size_t r = 0; r < idx.size(); ++r)
            shed[idx[r]] = uint8_t(r > 255 ? 255 : r);
    }

    // The governor input: every other user, then the VRAM products.
    GovernorInput gi;
    gi.card = card;
    gi.ownUsageBytes = in.ownUsageBytes;
    gi.transientBytes = in.transientBytes;
    gi.headroom = in.headroom;
    gi.users = in.otherUsers;
    std::vector<int> userOf(all.size(), -1);
    for (size_t i = 0; i < all.size(); ++i) {
        const Node& n = g.nodes[i];
        if (all[i].residency != Residency::Vram)
            continue;
        VramUser u;
        u.id = uint16_t(kUserProductBase + n.id);
        u.name = reg.Get(n.id).name;
        u.tier = n.tier;
        u.shedOrder = shed[i];
        u.lossless = true;   // in-render recompute is identical
        u.levels.push_back((canLane[i] ? n.ringBytes : 0) + n.liveBytes);
        u.levelText.push_back(nullptr);
        if (canLane[i] && n.ringBytes > 0) {
            u.levels.push_back(n.liveBytes);
            u.levelText.push_back("computed in the render (no look-ahead)");
        }
        userOf[i] = int(gi.users.size());
        gi.users.push_back(u);
    }
    std::vector<size_t> level;
    plan.governor = PlanVram(gi, &level);

    // System RAM products: against free system RAM.
    uint64_t systemBytes = 0;
    for (size_t i = 0; i < all.size(); ++i)
        if (all[i].residency == Residency::SystemRam)
            systemBytes += g.nodes[i].liveBytes;
    for (size_t i = 0; i < all.size(); ++i) {
        TierEntry& e = all[i];
        if (e.why != GrantWhy::Granted)
            continue;
        if (e.residency == Residency::SystemRam) {
            if (systemBytes + e.extraBytes <= card.systemRamFreeBytes) {
                systemBytes += e.extraBytes;
                e.grant = Grant::Lane;
            } else {
                e.why = GrantWhy::OverBudget;
            }
        } else if (level[size_t(userOf[i])] == 0) {
            e.grant = Grant::Lane;
        } else {
            e.why = GrantWhy::OverBudget;
        }
    }
    // A lane product whose same-frame input is in the render goes in the render too (topological
    // order, so this settles in one pass); its lane memory is freed.
    bool changed = false;
    for (size_t i = 0; i < all.size(); ++i) {
        TierEntry& e = all[i];
        if (e.grant != Grant::Lane)
            continue;
        const Node& n = g.nodes[i];
        for (size_t k = 0; k < n.inputCount; ++k) {
            if (n.inputs[k].lag != 0)
                continue;
            const int j = g.IndexOf(n.inputs[k].product);
            if (j >= 0 && all[size_t(j)].grant != Grant::Lane) {
                e.grant = Grant::InRender;
                e.why = GrantWhy::InputInRender;
                if (userOf[i] >= 0 && gi.users[size_t(userOf[i])].levels.size() > 1) {
                    level[size_t(userOf[i])] = 1;
                    changed = true;
                } else if (e.residency == Residency::SystemRam) {
                    systemBytes -= e.extraBytes;
                }
                break;
            }
        }
    }
    if (changed) {
        FinishGovernorPlan(gi, level, plan.governor);
        // An input-in-render product is not "short of memory": keep its status off the memory lines.
        std::vector<std::string> lines;
        for (const std::string& l : plan.governor.lines) {
            bool keep = true;
            for (size_t i = 0; i < all.size(); ++i)
                if (all[i].why == GrantWhy::InputInRender && userOf[i] >= 0
                    && l.find(std::string("Video memory: ") + gi.users[size_t(userOf[i])].name + " ") == 0)
                    keep = false;
            if (keep)
                lines.push_back(l);
        }
        plan.governor.lines = lines;
    }

    plan.budgetBytes = plan.governor.budgetBytes;
    plan.vramBytes = plan.governor.totalBytes;
    plan.systemRamBytes = systemBytes;
    plan.liveOverBudget = !plan.governor.fits;
    for (const TierEntry& e : all)
        if (e.tier == Tier::T0 && e.why == GrantWhy::OverBudget)
            plan.t0OverBudget = true;

    // Entries in grant order: T0 topological, then T1 and T2 by ms per MB, best first.
    for (const TierEntry& e : all)
        if (e.tier == Tier::T0)
            plan.entries.push_back(e);
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
        for (const TierEntry& e : tier)
            plan.entries.push_back(e);
    }

    // Plain-language notice (no silent fallbacks): the governor's lines plus the engine load.
    std::string notice;
    auto add = [&notice](const std::string& s) {
        if (!notice.empty())
            notice += "; ";
        notice += s;
    };
    if (plan.t0OverBudget)
        add("Analysis memory: essential analysis runs in the render (not enough free video memory)");
    for (const std::string& l : plan.governor.lines)
        add(l);
    if (plan.ofaUsed && !plan.ofaFits)
        add("Vector engine slower than the frame rate: reduced engine settings needed");
    plan.notice = notice;
    return plan;
}

// The tier plan's lane grants applied to the slots (a product without a lane: misses not counted).
inline void ApplyLaneGrants(const TierPlan& plan, SlotTable& table)
{
    for (size_t i = 0; i < table.Count(); ++i)
        table.At(i).SetLaneGranted(plan.Laned(table.NodeAt(i).id));
}

// The governor over time: re-plans on every poll; memory pressure (usage above the WDDM budget)
// shrinks the headroom at once, calm polls grow it after a dwell, so levels come back slowly.
class VramGovernor {
public:
    const TierPlan& Update(const Registry& reg, const Graph& g, const CardProfile& card, TierInput in)
    {
        if (card.vramUsageBytes > card.vramBudgetBytes)
            headroom_.OnPressure();
        else
            headroom_.OnCalmWindow();
        in.headroom = headroom_.Value();
        plan_ = PlanTiers(reg, g, card, in);
        return plan_;
    }
    double Headroom() const { return headroom_.Value(); }
    const TierPlan& Plan() const { return plan_; }

private:
    VpSkeleton::Headroom headroom_;
    TierPlan plan_;
};

} // namespace VpSkeleton
