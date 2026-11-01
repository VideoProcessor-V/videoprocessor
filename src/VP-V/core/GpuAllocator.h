// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "VramGovernor.h"

#include <VP-V/core/FallbackCatalog.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <map>
#include <string>
#include <tuple>
#include <vector>

// Skeleton SA1: the GPU allocator core (doc 51 section 3, P10 / arena). Two things live here, both pure
// C++14 (no Windows, no GPU, no libplacebo; a Backend template creates the real textures, see
// modules/control/gpu_memory/PlTexBackend.h):
//
//   1. The SCRATCH ARENA plan: per-render temporaries (row passes, bands, quiet maps, half-resolution
//      work textures) are requested per SCOPE (one render stage = one scope). Scopes run one after the
//      other inside a render, so their temporaries can share memory: the arena keeps, per texture
//      shape, as many textures as the BUSIEST scope needs (capacity[shape] = max over scopes of that
//      scope's count), not the sum. The plan is independent of request order. The arena is a governor
//      user (kUserScratchArena) with levels [plan(T2), plan(T1), plan(T0), 0]: under memory pressure the
//      T2 temporaries go first, then T1, then T0 (ScratchArena.h acquires nothing for a shed request
//      and says so visibly).
//   2. PRODUCT TEXTURES: the ring + live-pool textures of one GPU skeleton product (a graph Node), from
//      a declared layout whose bytes must equal the product's SizeRule (the governor counts exactly what
//      is allocated).
//
// Sizes are always derived from the source dimensions (shift terms): no hard-coded resolutions.
// The ledger lines (VramLedgerLines) are what Ctrl+Alt+D prints: one line per VRAM user with its bytes
// and level, the arena, the total against the budget.
namespace VpSkeleton {

// ---- texture formats and shapes ----

enum class TexFormat : uint8_t { R8, R16, RG16, R16F, RG16F, RGBA16F, R32F, RG32F, RGBA32F };

inline uint32_t BytesPerTexel(TexFormat f)
{
    switch (f) {
    case TexFormat::R8: return 1;
    case TexFormat::R16: return 2;
    case TexFormat::RG16: return 4;
    case TexFormat::R16F: return 2;
    case TexFormat::RG16F: return 4;
    case TexFormat::RGBA16F: return 8;
    case TexFormat::R32F: return 4;
    case TexFormat::RG32F: return 8;
    case TexFormat::RGBA32F: return 16;
    }
    return 0;
}

inline const char* Token(TexFormat f)
{
    switch (f) {
    case TexFormat::R8: return "r8";
    case TexFormat::R16: return "r16";
    case TexFormat::RG16: return "rg16";
    case TexFormat::R16F: return "r16f";
    case TexFormat::RG16F: return "rg16f";
    case TexFormat::RGBA16F: return "rgba16f";
    case TexFormat::R32F: return "r32f";
    case TexFormat::RG32F: return "rg32f";
    case TexFormat::RGBA32F: return "rgba32f";
    }
    return "?";
}

struct TexShape {
    uint32_t w = 0;
    uint32_t h = 0;
    TexFormat fmt = TexFormat::R8;
};

inline bool operator==(const TexShape& a, const TexShape& b) { return a.w == b.w && a.h == b.h && a.fmt == b.fmt; }
inline bool operator!=(const TexShape& a, const TexShape& b) { return !(a == b); }

// The sort key of a shape: format, then width, then height (a total order, independent of insertion).
inline bool ShapeLess(const TexShape& a, const TexShape& b)
{
    return std::make_tuple(uint8_t(a.fmt), a.w, a.h) < std::make_tuple(uint8_t(b.fmt), b.w, b.h);
}

inline uint64_t ShapeBytes(const TexShape& s) { return uint64_t(s.w) * uint64_t(s.h) * BytesPerTexel(s.fmt); }

// ---- the scratch arena plan ----

// One scope's request: `count` textures of `shape` for the duration of the scope. `owner` is a stable
// literal naming the consumer (visible in the fallback text and the ledger). `tier` says how essential:
// a shed tier gets nothing from the arena.
struct ArenaRequest {
    uint16_t scope = 0;
    const char* owner = "";
    Tier tier = Tier::T0;
    TexShape shape;
    uint8_t count = 1;
};

struct ArenaPool {
    TexShape shape;
    uint32_t capacity = 0;   // textures kept for this shape (max over scopes)
    uint64_t bytes = 0;      // capacity * ShapeBytes
};

struct ArenaPlan {
    Tier maxTier = Tier::T2;                // requests above this tier are shed
    std::vector<ArenaPool> pools;           // sorted by (format, width, height)
    std::vector<ArenaRequest> admitted;     // sorted by (scope, owner, shape); requests inside maxTier
    uint32_t shedRequests = 0;              // requests dropped for their tier
    uint64_t bytes = 0;                     // what the arena allocates (the sum over pools)
    uint64_t maxScopeBytes = 0;             // the busiest scope's own need (a lower bound of bytes)
    uint64_t sumAllBytes = 0;               // the sum of every scope's need (what private textures would cost)

    bool Empty() const { return pools.empty(); }
    const ArenaPool* FindPool(const TexShape& s) const
    {
        for (const ArenaPool& p : pools)
            if (p.shape == s)
                return &p;
        return nullptr;
    }
    // How many textures of `shape` the admitted requests of `scope` may hold at once.
    uint32_t ScopeCount(uint16_t scope, const TexShape& s) const
    {
        uint32_t n = 0;
        for (const ArenaRequest& r : admitted)
            if (r.scope == scope && r.shape == s)
                n += r.count;
        return n;
    }
    // What ONE owner may hold at once of `shape` in `scope`, and where its slice of the pool starts (the sum of
    // the owners sorted before it): usage is counted per (owner, shape) so an owner never takes another's textures.
    uint32_t OwnerCount(uint16_t scope, const char* owner, const TexShape& s) const
    {
        uint32_t n = 0;
        const std::string o = owner ? owner : "";
        for (const ArenaRequest& r : admitted)
            if (r.scope == scope && r.shape == s && std::string(r.owner ? r.owner : "") == o)
                n += r.count;
        return n;
    }
    uint32_t OwnerBase(uint16_t scope, const char* owner, const TexShape& s) const
    {
        uint32_t n = 0;
        const std::string o = owner ? owner : "";
        for (const ArenaRequest& r : admitted)
            if (r.scope == scope && r.shape == s && std::string(r.owner ? r.owner : "") < o)
                n += r.count;
        return n;
    }
    // Whether `owner` asked for `shape` in `scope` and was admitted (not shed by tier).
    bool Admits(uint16_t scope, const char* owner, const TexShape& s) const
    {
        for (const ArenaRequest& r : admitted)
            if (r.scope == scope && r.shape == s && std::string(r.owner) == std::string(owner ? owner : ""))
                return true;
        return false;
    }
};

inline uint8_t TierRank(Tier t) { return uint8_t(t); }

// The plan for `requests` with every tier up to and including `maxTier`. Deterministic: the result does
// not depend on the order of `requests` (sorted keys, ordered maps, no hashing of pointers).
inline ArenaPlan PlanArena(const std::vector<ArenaRequest>& requests, Tier maxTier)
{
    ArenaPlan plan;
    plan.maxTier = maxTier;
    for (const ArenaRequest& r : requests) {
        if (TierRank(r.tier) > TierRank(maxTier)) {
            ++plan.shedRequests;
            continue;
        }
        if (r.count == 0 || r.shape.w == 0 || r.shape.h == 0)
            continue;
        plan.admitted.push_back(r);
    }
    std::sort(plan.admitted.begin(), plan.admitted.end(), [](const ArenaRequest& a, const ArenaRequest& b) {
        if (a.scope != b.scope)
            return a.scope < b.scope;
        const int c = std::string(a.owner ? a.owner : "").compare(std::string(b.owner ? b.owner : ""));
        if (c != 0)
            return c < 0;
        if (a.shape != b.shape)
            return ShapeLess(a.shape, b.shape);
        if (a.count != b.count)
            return a.count < b.count;
        return TierRank(a.tier) < TierRank(b.tier);
    });
    // Per scope, per shape: the textures it holds at once. Capacity = the max over scopes.
    std::map<uint16_t, std::map<std::tuple<uint8_t, uint32_t, uint32_t>, uint32_t>> perScope;
    for (const ArenaRequest& r : plan.admitted)
        perScope[r.scope][std::make_tuple(uint8_t(r.shape.fmt), r.shape.w, r.shape.h)] += r.count;
    std::map<std::tuple<uint8_t, uint32_t, uint32_t>, uint32_t> capacity;
    for (const auto& sc : perScope) {
        uint64_t scopeBytes = 0;
        for (const auto& kv : sc.second) {
            uint32_t& c = capacity[kv.first];
            if (kv.second > c)
                c = kv.second;
            TexShape s;
            s.fmt = TexFormat(std::get<0>(kv.first));
            s.w = std::get<1>(kv.first);
            s.h = std::get<2>(kv.first);
            scopeBytes += ShapeBytes(s) * kv.second;
        }
        plan.sumAllBytes += scopeBytes;
        plan.maxScopeBytes = std::max(plan.maxScopeBytes, scopeBytes);
    }
    for (const auto& kv : capacity) {
        ArenaPool p;
        p.shape.fmt = TexFormat(std::get<0>(kv.first));
        p.shape.w = std::get<1>(kv.first);
        p.shape.h = std::get<2>(kv.first);
        p.capacity = kv.second;
        p.bytes = ShapeBytes(p.shape) * kv.second;
        plan.bytes += p.bytes;
        plan.pools.push_back(p);
    }
    std::sort(plan.pools.begin(), plan.pools.end(),
              [](const ArenaPool& a, const ArenaPool& b) { return ShapeLess(a.shape, b.shape); });
    return plan;
}

// The arena as TWO governor users, so pressure on the optional (T1/T2) temporaries never takes the T0 ones:
//   kUserScratchArena      tier T0: the T0 plan, levels [plan(T0), 0]. It is shed only after every T1/T2 user.
//   kUserScratchArenaExtra tier T1: the increment of the T1/T2 temporaries over the T0 plan, levels
//                          [plan(T2) - plan(T0), plan(T1) - plan(T0), 0] (equal levels dropped).
// Not lossless (a shed temporary switches a repair off or reduces it).
inline VramUser ScratchArenaT0User(const std::vector<ArenaRequest>& requests)
{
    VramUser u;
    u.id = kUserScratchArena;
    u.name = "scratch arena";
    u.tier = Tier::T0;
    u.shedOrder = 200;
    u.lossless = false;
    const uint64_t b0 = PlanArena(requests, Tier::T0).bytes;
    u.levels.push_back(b0);
    u.levelText.push_back(nullptr);
    if (b0 > 0) {
        u.levels.push_back(0);
        u.levelText.push_back("T0 temporaries off");
    }
    return u;
}

inline VramUser ScratchArenaExtraUser(const std::vector<ArenaRequest>& requests)
{
    VramUser u;
    u.id = kUserScratchArenaExtra;
    u.name = "scratch arena (optional temporaries)";
    u.tier = Tier::T1;
    u.shedOrder = 0;
    u.lossless = false;
    const uint64_t b0 = PlanArena(requests, Tier::T0).bytes;
    const uint64_t b1 = PlanArena(requests, Tier::T1).bytes;
    const uint64_t b2 = PlanArena(requests, Tier::T2).bytes;
    const uint64_t i2 = b2 > b0 ? b2 - b0 : 0;
    const uint64_t i1 = b1 > b0 ? b1 - b0 : 0;
    u.levels.push_back(i2);
    u.levelText.push_back(nullptr);
    if (i1 < u.levels.back()) {
        u.levels.push_back(i1);
        u.levelText.push_back("T2 temporaries off");
    }
    if (u.levels.back() > 0) {
        u.levels.push_back(0);
        u.levelText.push_back("T1 and T2 temporaries off");
    }
    return u;
}

inline void AddScratchArenaUsers(std::vector<VramUser>& users, const std::vector<ArenaRequest>& requests)
{
    users.push_back(ScratchArenaT0User(requests));
    users.push_back(ScratchArenaExtraUser(requests));
}

// What the arena may keep when the governor granted `t0Bytes` to kUserScratchArena and `extraBytes` to
// kUserScratchArenaExtra: `any` is false when the T0 plan does not fit (the arena is off); else the largest
// tier whose increment over the T0 plan fits the extra grant.
struct ArenaGrant {
    bool any = false;
    Tier maxTier = Tier::T0;
};

inline ArenaGrant ArenaGrantFor(const std::vector<ArenaRequest>& requests, uint64_t t0Bytes, uint64_t extraBytes)
{
    ArenaGrant g;
    const uint64_t b0 = PlanArena(requests, Tier::T0).bytes;
    if (b0 > t0Bytes)
        return g;   // off
    g.any = true;
    for (Tier t : { Tier::T2, Tier::T1 }) {
        const uint64_t bt = PlanArena(requests, t).bytes;
        if ((bt > b0 ? bt - b0 : 0) <= extraBytes) {
            g.maxTier = t;
            return g;
        }
    }
    g.maxTier = Tier::T0;
    return g;
}

// ---- product textures: ring + live pool of one GPU product ----

// One buffer of a product = these textures; each term is a shift of the source dims and a format. Sizes
// are always derived from the source dims.
struct TexTerm {
    uint8_t shift = 0;
    TexFormat fmt = TexFormat::R8;
    // Optional absolute size (both > 0): the ACTUAL plane dims of a product whose geometry is not a plain shift of
    // the source (a chroma plane): the runtime path uses what the plane really is, never an assumed subsampling.
    uint32_t absW = 0;
    uint32_t absH = 0;
};

struct ProductTexLayout {
    std::vector<TexTerm> terms;

    TexShape ShapeOf(size_t i, Dims d) const
    {
        const uint64_t div = uint64_t(1) << terms[i].shift;
        TexShape s;
        s.w = terms[i].absW > 0 && terms[i].absH > 0 ? terms[i].absW : uint32_t((uint64_t(d.width) + div - 1) / div);
        s.h = terms[i].absW > 0 && terms[i].absH > 0 ? terms[i].absH : uint32_t((uint64_t(d.height) + div - 1) / div);
        s.fmt = terms[i].fmt;
        return s;
    }
    uint64_t BytesPerBuffer(Dims d) const
    {
        uint64_t b = 0;
        for (size_t i = 0; i < terms.size(); ++i)
            b += ShapeBytes(ShapeOf(i, d));
        return b;
    }
    // The static check of the governor: the layout is exactly what the declaration counts. A layout without
    // absolute terms must equal the SizeRule in total. A layout with absolute terms (a grid whose cell edge is a
    // runtime choice, e.g. the grain split's block edge B) is checked term by term against the declaration's terms
    // in the same order: a relative term exactly, an absolute term at the same bytes per cell and AT MOST the
    // declared cells (the declaration is the upper bound the governor plans with).
    bool MatchesDecl(const ProductDecl& decl, Dims d) const
    {
        bool hasAbs = false;
        for (const TexTerm& t : terms)
            if (t.absW > 0 && t.absH > 0)
                hasAbs = true;
        if (!hasAbs)
            return BytesPerBuffer(d) == BytesOf(decl.size, d);
        const size_t n = decl.size.termCount < decl.size.terms.size() ? size_t(decl.size.termCount) : decl.size.terms.size();
        if (decl.size.fixedBytes != 0 || terms.size() != n)
            return false;
        for (size_t i = 0; i < n; ++i) {
            const SizeTerm& st = decl.size.terms[i];
            const uint64_t declBytes = CellsAt(d, st.shift) * st.bytesPerCell;
            const uint64_t bytes = ShapeBytes(ShapeOf(i, d));
            if (terms[i].absW > 0 && terms[i].absH > 0) {
                if (BytesPerTexel(terms[i].fmt) != st.bytesPerCell || bytes > declBytes)
                    return false;
            } else if (bytes != declBytes) {
                return false;
            }
        }
        return true;
    }
};

// The textures of one product: `ring` buffers when laned, always `live` buffers. Allocation happens only
// in Allocate (deterministic order: ring slots, then live slots, textures in term order); nothing is
// allocated per frame. TexBackend: using Handle; Handle Create(const TexShape&); void Destroy(Handle&).
template <class TexBackend>
class ProductTextures {
public:
    using Handle = typename TexBackend::Handle;

    explicit ProductTextures(TexBackend& backend) : backend_(&backend) {}
    ~ProductTextures() { Release(); }
    ProductTextures(const ProductTextures&) = delete;
    ProductTextures& operator=(const ProductTextures&) = delete;

    // Ring + live when `plan.Laned(node.id)` (and the node has a lane), the live pool alone otherwise.
    // False (and nothing held) when a texture could not be created: the caller treats the product as
    // unavailable (a visible, graceful path), never partially allocated.
    // Refuses (visibly: LastRefusal() / RefusalDecision()) when the layout's bytes do not equal the declaration's
    // SizeRule, unless the declaration is a planning-only size (decl.planningSize: its runtime bytes are the
    // layout's actual ones, reported to the governor through TierInput::actual).
    bool Allocate(const Node& node, const ProductDecl& decl, const ProductTexLayout& layout, Dims dims, const TierPlan& plan)
    {
        Release();
        refusal_.clear();
        if (!decl.planningSize && !layout.MatchesDecl(decl, dims)) {
            refusal_ = std::string("layout does not match the declaration of ") + decl.name;
            ++refusals_;
            return false;
        }
        const bool lane = plan.Laned(node.id) && node.backend != Backend::RenderOnly;
        const size_t ringCount = lane ? node.ring : 0;
        const size_t liveCount = node.livePool;
        layout_ = layout;
        dims_ = dims;
        laned_ = lane;
        const size_t perBuffer = layout.terms.size();
        ring_.assign(ringCount * perBuffer, Handle());
        live_.assign(liveCount * perBuffer, Handle());
        for (std::vector<Handle>* set : { &ring_, &live_ })
            for (size_t i = 0; i < set->size(); ++i) {
                Handle h = backend_->Create(layout.ShapeOf(i % perBuffer, dims));
                if (!h) {
                    ++createFailures_;
                    Release();
                    return false;
                }
                (*set)[i] = h;
            }
        bytes_ = (uint64_t(ringCount) + liveCount) * layout.BytesPerBuffer(dims);
        return true;
    }

    void Release()
    {
        for (std::vector<Handle>* set : { &ring_, &live_ }) {
            for (Handle& h : *set)
                if (h)
                    backend_->Destroy(h);
            set->clear();
        }
        bytes_ = 0;
        laned_ = false;
    }

    const std::string& LastRefusal() const { return refusal_; }
    unsigned Refusals() const { return refusals_; }
    FallbackCatalog::Decision RefusalDecision() const
    {
        if (refusal_.empty())
            return FallbackCatalog::Released("product textures");
        return FallbackCatalog::Engaged("product textures", refusal_ + ": the product is unavailable (readers fall back)");
    }
    bool Laned() const { return laned_; }
    size_t RingBuffers() const { return layout_.terms.empty() ? 0 : ring_.size() / layout_.terms.size(); }
    size_t LiveBuffers() const { return layout_.terms.empty() ? 0 : live_.size() / layout_.terms.size(); }
    uint64_t Bytes() const { return bytes_; }
    unsigned CreateFailures() const { return createFailures_; }
    // Texture `term` of ring slot / live buffer `slot` (null when out of range or not laned).
    Handle Ring(size_t slot, size_t term) const { return At(ring_, slot, term); }
    Handle Live(size_t slot, size_t term) const { return At(live_, slot, term); }

private:
    Handle At(const std::vector<Handle>& set, size_t slot, size_t term) const
    {
        const size_t per = layout_.terms.size();
        if (per == 0 || term >= per || (slot + 1) * per > set.size())
            return Handle();
        return set[slot * per + term];
    }

    TexBackend* backend_;
    ProductTexLayout layout_;
    Dims dims_;
    bool laned_ = false;
    std::vector<Handle> ring_;
    std::vector<Handle> live_;
    uint64_t bytes_ = 0;
    unsigned createFailures_ = 0;
    std::string refusal_;
    unsigned refusals_ = 0;
};

// ---- the ledger (Ctrl+Alt+D) ----

inline std::string FormatMB(uint64_t bytes)
{
    char s[48];
    std::snprintf(s, sizeof(s), "%.1f MB", double(bytes) / (1024.0 * 1024.0));
    return s;
}

// One line per VRAM user (products and the arena included) with its bytes and level, the arena's plan
// (what it allocates, the busiest scope, what private textures would cost), and the total against the
// governor's budget. Transient products are listed as arena-counted (they have no ring).
inline std::vector<std::string> VramLedgerLines(const TierPlan& plan, const ArenaPlan& arena, const Registry& reg)
{
    std::vector<std::string> lines;
    for (const UserGrant& g : plan.governor.grants) {
        std::string line = std::string("VRAM ") + Token(g.tier) + " " + g.name + ": " + FormatMB(g.bytes) + " level "
                           + std::to_string(g.level);
        if (g.shed)
            line += " (off)";
        else if (g.reduced)
            line += " (reduced)";
        lines.push_back(line);
    }
    for (const TierEntry& e : plan.entries)
        if (e.residency == Residency::Transient && reg.Has(e.id))
            lines.push_back(std::string("VRAM ") + reg.Get(e.id).name + ": transient (counted in the scratch arena)");
    {
        std::string line = std::string("VRAM scratch arena plan (up to ") + Token(arena.maxTier) + "): "
                           + std::to_string(arena.pools.size()) + " pools, " + FormatMB(arena.bytes) + ", busiest scope "
                           + FormatMB(arena.maxScopeBytes) + ", private temporaries would be " + FormatMB(arena.sumAllBytes);
        if (arena.shedRequests > 0)
            line += ", " + std::to_string(arena.shedRequests) + " shed by tier";
        lines.push_back(line);
    }
    lines.push_back(std::string("VRAM total ") + FormatMB(plan.vramBytes) + " of budget " + FormatMB(plan.budgetBytes)
                    + (plan.liveOverBudget ? " (OVER budget)" : " (fits)"));
    return lines;
}

} // namespace VpSkeleton
