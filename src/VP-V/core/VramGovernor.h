// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "CardProfile.h"
#include "ProductGraph.h"
#include "Products.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// Skeleton S2: the ONE VRAM governor (coordinator input 2026-10-31, bug hunt doc 38 H2/M3). Every
// VRAM user registers its need as levels (level 0 = full, each later level smaller, the last the
// minimum; 0 bytes = shed) with a tier and a shed order. The governor keeps the total (plus any
// declared transient, e.g. the 2x early-upload pool during a size change) under the budget:
//
//   budget = (WDDM budget - usage by others) x adaptive headroom
//   usage by others = CurrentUsage - VP's own current allocation (so VP's own memory is re-planned)
//
// Over budget, users step down ONE level at a time: T2 before T1 before T0, within a tier by shed
// order then id (doc 38 order: render-ahead depth, early-upload slots, TemporalDetail future slots).
// Every user below level 0 gives a plain status line (no silent fallbacks); a plan that cannot fit
// even at every minimum says so. Fixed users (libplacebo intermediates, capture planes) have one
// level and only count. Skeleton lane products register through AddProductUsers (lane ring + live
// pool, or the live pool alone = in-render). Pure C++14: the renderer polls QueryVideoMemoryInfo
// and applies the levels; nothing here allocates on the GPU.
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
    kUserProductBase = 0x100,  // + ProductId: skeleton lane products
};

struct VramUser {
    uint16_t id = 0;
    const char* name = "";
    Tier tier = Tier::T0;
    uint8_t shedOrder = 0;            // within a tier: lower sheds first
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

inline GovernorPlan PlanVram(const GovernorInput& in)
{
    GovernorPlan plan;
    const double headroom = std::min(kHeadroomMax, std::max(kHeadroomMin, in.headroom));
    const uint64_t others = OthersUsage(in.card, in.ownUsageBytes);
    const uint64_t avail = in.card.vramBudgetBytes > others ? in.card.vramBudgetBytes - others : 0;
    plan.budgetBytes = uint64_t(double(avail) * headroom);

    std::vector<size_t> level(in.users.size(), 0);
    auto bytesOf = [&](size_t i) -> uint64_t {
        const VramUser& u = in.users[i];
        return u.levels.empty() ? 0 : u.levels[level[i] < u.levels.size() ? level[i] : u.levels.size() - 1];
    };
    auto total = [&]() {
        uint64_t t = in.transientBytes;
        for (size_t i = 0; i < in.users.size(); ++i)
            t += bytesOf(i);
        return t;
    };
    // Deterministic shed rank: tier (T2 first), shed order, id.
    auto before = [&](size_t a, size_t b) {
        const VramUser& x = in.users[a];
        const VramUser& y = in.users[b];
        if (x.tier != y.tier)
            return uint8_t(x.tier) > uint8_t(y.tier);
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
    plan.totalBytes = t;
    plan.fits = t <= plan.budgetBytes;

    for (size_t i = 0; i < in.users.size(); ++i) {
        const VramUser& u = in.users[i];
        UserGrant g;
        g.id = u.id;
        g.name = u.name;
        g.tier = u.tier;
        g.level = level[i];
        g.bytes = bytesOf(i);
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
    return plan;
}

// The skeleton's nodes as governor users: a lane product needs its ring + live pool (level 0) and
// can fall back to the live pool alone (level 1 = in-render, identical); a render-only product has
// one level. System-RAM products are not VRAM users.
inline void AddProductUsers(const Registry& reg, const Graph& g, std::vector<VramUser>& out)
{
    for (const Node& n : g.nodes) {
        const ProductDecl& d = reg.Get(n.id);
        if (ResidencyFor(d, n.bufferBytes) != Residency::Vram)
            continue;
        VramUser u;
        u.id = uint16_t(kUserProductBase + n.id);
        u.name = d.name;
        u.tier = n.tier;
        u.shedOrder = 0;
        u.levels.push_back(n.ringBytes + n.liveBytes);
        u.levelText.push_back(nullptr);
        if (n.ringBytes > 0) {
            u.levels.push_back(n.liveBytes);
            u.levelText.push_back("computed in the render (no look-ahead)");
        }
        out.push_back(u);
    }
}

// The governor over time: re-plans on every poll; memory pressure (usage above the WDDM budget)
// shrinks the headroom at once, calm polls grow it after a dwell, so levels come back slowly.
class VramGovernor {
public:
    const GovernorPlan& Update(GovernorInput in)
    {
        if (in.card.vramUsageBytes > in.card.vramBudgetBytes)
            headroom_.OnPressure();
        else
            headroom_.OnCalmWindow();
        in.headroom = headroom_.Value();
        plan_ = PlanVram(in);
        return plan_;
    }
    double Headroom() const { return headroom_.Value(); }
    const GovernorPlan& Plan() const { return plan_; }

private:
    VpSkeleton::Headroom headroom_;
    GovernorPlan plan_;
};

} // namespace VpSkeleton
