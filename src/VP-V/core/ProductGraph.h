// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "Indicator.h"
#include "Products.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// Skeleton S0: the product graph (design doc 25 section B2/B3). Built from the active consumer
// declarations, the registry, the queue/render-ahead settings, the source dims and whether the
// card has an optical-flow engine. It holds the closure over inputs (each product once, in a
// deterministic topological order over lag-0 edges, ties by product id) and the fixed slot counts:
//
//   ring = history + 1 (the presented frame) + R (renders started, not presented)
//          + L (lane lead to render start; 0 for render-only) + q (1 lane-owned rejected buffer for
//          every lane backend; 0 for render-only) + 1 only when a consumer holds the next frame's
//          product through the presentation span (a pair consumer, audit 25a N8)
//   live pool = history + 1 + R (+1 for a held span product) + one spare per input reference held
//               by lane readers (a late lane reader may pin a released live buffer for one job):
//               every frame the render holds may be
//               a live fill (design B3 says R + 1; that leaves no live buffer for a history frame
//               that was itself filled live, so an in-render-only history product could not run)
//
// history = max(the product's own history, the largest lag any active node or consumer reads it at).
// Everything is fixed at build (no per-frame allocation). A rebuild advances the Indicator's
// `graph` counter (GraphOwner); every node honours `graph`, so lane work from the old graph is
// rejected at the seal. Header-only, pure C++14.
namespace VpSkeleton {

enum class QueueMode : uint8_t { Normal, Games, Competitive };

struct GraphSettings {
    Dims dims;                    // source active picture
    uint8_t renderAhead = 0;      // R
    uint8_t laneLead = 1;         // L, measured to render start (0: backward pairs on arrival)
    uint16_t queueDepth = 0;      // queue capacity in frames (derived from the fixed-ms queue)
    uint16_t lookAheadWalk = 0;   // frames the look-ahead walks copy out of the queue
    QueueMode queueMode = QueueMode::Normal;
    bool ofaPresent = true;       // the card has an optical-flow engine
    uint64_t cardProfileKey = 0;  // CardProfile identity (adapter LUID + driver), a rebuild trigger
};

struct Node {
    ProductId id = kNoProduct;
    ProductId declared = kNoProduct;   // the id a consumer asked for (differs when substituted)
    Backend backend = Backend::Cpu;
    Tier tier = Tier::T0;
    uint8_t history = 0;
    bool holdsSpan = false;
    uint8_t ring = 0;
    uint8_t livePool = 0;
    uint8_t inputCount = 0;
    std::array<InputRef, kMaxInputs> inputs{};   // substituted ids
    VpIndicator::CounterMask reads = 0;          // the product's reads plus `graph`
    VpIndicator::CounterMask spanMask = 0;
    uint64_t bufferBytes = 0;
    uint64_t ringBytes = 0;   // lane ring buffers (0 for render-only)
    uint64_t liveBytes = 0;   // live pool, separate memory
};

enum RebuildReason : uint32_t {
    kRebuildNone = 0,
    kRebuildFirst = 1u << 0,
    kRebuildEra = 1u << 1,
    kRebuildSettings = 1u << 2,
    kRebuildFull = 1u << 3,
    kRebuildAxis = 1u << 4,       // only when the graph has re-based-plane products
    kRebuildQueue = 1u << 5,      // queue depth / mode, render-ahead depth, lane lead, walk
    kRebuildCard = 1u << 6,
    kRebuildDims = 1u << 7,
    kRebuildConsumers = 1u << 8,
};

// What a rebuild depends on. The `graph` counter itself is deliberately not part of it.
struct GraphKey {
    bool set = false;
    uint32_t era = 0;
    std::array<uint32_t, 4> settings{};   // kSettingsMotion..kSettingsOutput
    uint32_t full = 0;
    uint32_t rebaseH = 0;
    uint32_t rebaseV = 0;
    GraphSettings gs;
    uint64_t consumers = 0;               // fingerprint of the consumer declarations
};

// FNV-1a over the declarations (ids, consumed products, lags, flags), order-sensitive.
inline uint64_t ConsumerFingerprint(const std::vector<ConsumerDecl>& consumers)
{
    uint64_t h = 1469598103934665603ull;
    auto mix = [&h](uint64_t v) {
        for (int i = 0; i < 8; ++i) {
            h ^= (v >> (8 * i)) & 0xFFu;
            h *= 1099511628211ull;
        }
    };
    mix(consumers.size());
    for (const ConsumerDecl& c : consumers) {
        mix(c.id);
        mix(c.count);
        for (size_t k = 0; k < c.count && k < kMaxConsumes; ++k)
            mix((uint64_t(c.consumes[k].product) << 8) | c.consumes[k].lag);
        mix((c.holdsSpan ? 1u : 0u) | (c.hasReducedMode ? 2u : 0u));
    }
    return h;
}

inline GraphKey KeyOf(const VpIndicator::Stamp& stamp, const GraphSettings& gs,
                      const std::vector<ConsumerDecl>& consumers)
{
    GraphKey k;
    k.set = true;
    k.era = stamp.era;
    k.settings[0] = stamp.v[VpIndicator::kSettingsMotion];
    k.settings[1] = stamp.v[VpIndicator::kSettingsRebase];
    k.settings[2] = stamp.v[VpIndicator::kSettingsAnalysis];
    k.settings[3] = stamp.v[VpIndicator::kSettingsOutput];
    k.full = stamp.v[VpIndicator::kFull];
    k.rebaseH = stamp.v[VpIndicator::kRebaseH];
    k.rebaseV = stamp.v[VpIndicator::kRebaseV];
    k.gs = gs;
    k.consumers = ConsumerFingerprint(consumers);
    return k;
}

// The reasons `after` needs a new graph over `before`. A FULL also moves both rebase counters
// (Indicator::MovedBy), which reports kRebuildFull only, not kRebuildAxis.
inline uint32_t RebuildReasons(const GraphKey& before, const GraphKey& after, bool hasRebasedPlane)
{
    if (!before.set)
        return kRebuildFirst;
    uint32_t r = 0;
    if (before.era != after.era)
        r |= kRebuildEra;
    if (before.settings != after.settings)
        r |= kRebuildSettings;
    const bool full = before.full != after.full;
    if (full)
        r |= kRebuildFull;
    if (!full && hasRebasedPlane && (before.rebaseH != after.rebaseH || before.rebaseV != after.rebaseV))
        r |= kRebuildAxis;
    const GraphSettings& a = before.gs;
    const GraphSettings& b = after.gs;
    if (a.renderAhead != b.renderAhead || a.laneLead != b.laneLead || a.queueDepth != b.queueDepth
        || a.lookAheadWalk != b.lookAheadWalk || a.queueMode != b.queueMode)
        r |= kRebuildQueue;
    if (a.cardProfileKey != b.cardProfileKey || a.ofaPresent != b.ofaPresent)
        r |= kRebuildCard;
    if (a.dims != b.dims)
        r |= kRebuildDims;
    if (before.consumers != after.consumers)
        r |= kRebuildConsumers;
    return r;
}

struct Graph {
    bool valid = false;
    const char* error = "";        // plain reason when invalid (shown in status by the caller)
    std::vector<Node> nodes;       // topological over lag-0 edges, ties by product id
    GraphSettings settings;
    size_t framesInFlight = 0;     // audit 25a N6: sizes the Indicator's event ring
    uint64_t totalRingBytes = 0;
    uint64_t totalLiveBytes = 0;
    bool hasRebasedPlane = false;

    int IndexOf(ProductId id) const
    {
        for (size_t i = 0; i < nodes.size(); ++i)
            if (nodes[i].id == id)
                return int(i);
        return -1;
    }
    bool Has(ProductId id) const { return IndexOf(id) >= 0; }
    const Node* Find(ProductId id) const
    {
        const int i = IndexOf(id);
        return i < 0 ? nullptr : &nodes[size_t(i)];
    }
    size_t IndicatorCapacity() const { return VpIndicator::Indicator::RequiredCapacity(framesInFlight); }
};

// The product actually made for `id` on this card.
inline ProductId Resolve(const Registry& reg, ProductId id, bool ofaPresent)
{
    if (!reg.Has(id))
        return kNoProduct;
    const ProductDecl& d = reg.Get(id);
    if (!ofaPresent && d.backend == Backend::Ofa && d.noOfaSubstitute != kNoProduct)
        return d.noOfaSubstitute;
    return id;
}

inline Graph BuildGraph(const Registry& reg, const std::vector<ConsumerDecl>& consumers, const GraphSettings& gs)
{
    Graph g;
    g.settings = gs;
    const char* why = "";
    if (!reg.Validate(&why)) {
        g.error = why;
        return g;
    }
    const size_t n = reg.Size();
    std::vector<char> needed(n, 0);
    std::vector<uint8_t> maxLag(n, 0);
    std::vector<char> span(n, 0);
    std::vector<ProductId> declaredAs(n, kNoProduct);
    std::vector<ProductId> work;

    auto want = [&](ProductId asked, uint8_t lag, bool holdsSpan) -> bool {
        const ProductId p = Resolve(reg, asked, gs.ofaPresent);
        if (p == kNoProduct)
            return false;
        if (lag > maxLag[p])
            maxLag[p] = lag;
        if (holdsSpan)
            span[p] = 1;
        if (!needed[p]) {
            needed[p] = 1;
            declaredAs[p] = asked;
            work.push_back(p);
        }
        return true;
    };

    for (const ConsumerDecl& c : consumers)
        for (size_t k = 0; k < c.count && k < kMaxConsumes; ++k)
            if (!want(c.consumes[k].product, c.consumes[k].lag, c.holdsSpan)) {
                g.error = "a consumer asks for an unknown product";
                return g;
            }
    // Closure: each product visited once.
    for (size_t w = 0; w < work.size(); ++w) {
        const ProductDecl& d = reg.Get(work[w]);
        for (size_t k = 0; k < d.inputCount; ++k)
            want(d.inputs[k].product, d.inputs[k].lag, false);
    }

    // Topological order over lag-0 edges of the needed set; the smallest ready id first.
    std::vector<int> indeg(n, 0);
    for (size_t p = 0; p < n; ++p) {
        if (!needed[p])
            continue;
        const ProductDecl& d = reg.Get(ProductId(p));
        for (size_t k = 0; k < d.inputCount; ++k)
            if (d.inputs[k].lag == 0)
                ++indeg[p];
    }
    std::vector<char> placed(n, 0);
    std::vector<ProductId> order;
    for (;;) {
        ProductId next = kNoProduct;
        for (size_t p = 0; p < n; ++p)
            if (needed[p] && !placed[p] && indeg[p] == 0) {
                next = ProductId(p);
                break;
            }
        if (next == kNoProduct)
            break;
        placed[next] = 1;
        order.push_back(next);
        for (size_t p = 0; p < n; ++p) {
            if (!needed[p] || placed[p])
                continue;
            const ProductDecl& d = reg.Get(ProductId(p));
            for (size_t k = 0; k < d.inputCount; ++k)
                if (d.inputs[k].lag == 0 && Resolve(reg, d.inputs[k].product, gs.ofaPresent) == next)
                    --indeg[p];
        }
    }
    size_t neededCount = 0;
    for (size_t p = 0; p < n; ++p)
        neededCount += needed[p] ? 1u : 0u;
    if (order.size() != neededCount) {
        g.error = "lag-0 cycle between products";
        return g;
    }

    // Input references held by lane jobs: a (late) lane reader pins its inputs for one job at a time
    // (one producer per product), so at most this many released live buffers can be pinned at once.
    // That many spares mean the render never runs short of a live buffer and never waits.
    std::vector<unsigned> laneReadRefs(n, 0);
    for (ProductId p : order) {
        const ProductDecl& d = reg.Get(p);
        if (d.backend == Backend::RenderOnly)
            continue;
        for (size_t k = 0; k < d.inputCount; ++k)
            ++laneReadRefs[Resolve(reg, d.inputs[k].product, gs.ofaPresent)];
    }

    const unsigned R = gs.renderAhead;
    size_t maxHistory = 0;
    for (ProductId p : order) {
        const ProductDecl& d = reg.Get(p);
        Node node;
        node.id = p;
        node.declared = declaredAs[p];
        node.backend = d.backend;
        node.tier = d.tier;
        node.history = d.history > maxLag[p] ? d.history : maxLag[p];
        node.holdsSpan = span[p] != 0;
        const bool lane = d.backend != Backend::RenderOnly;
        const unsigned L = lane ? gs.laneLead : 0u;
        const unsigned q = lane ? 1u : 0u;
        const unsigned ring = unsigned(node.history) + 1u + R + L + q + (node.holdsSpan ? 1u : 0u);
        node.ring = uint8_t(ring > 255u ? 255u : ring);
        const unsigned live = unsigned(node.history) + 1u + R + (node.holdsSpan ? 1u : 0u) + laneReadRefs[p];
        node.livePool = uint8_t(live > 255u ? 255u : live);
        node.inputCount = d.inputCount;
        for (size_t k = 0; k < d.inputCount; ++k) {
            node.inputs[k].product = Resolve(reg, d.inputs[k].product, gs.ofaPresent);
            node.inputs[k].lag = d.inputs[k].lag;
        }
        node.reads = d.reads | VpIndicator::Bit(VpIndicator::kGraph);
        node.spanMask = d.spanMask;
        node.bufferBytes = BytesOf(d.size, gs.dims);
        // Ring buffers only for a lane (a render-only product's slots hold headers; its data is in
        // the live pool). The live pool is separate memory.
        node.ringBytes = lane ? node.bufferBytes * node.ring : 0;
        node.liveBytes = node.bufferBytes * node.livePool;
        g.totalRingBytes += node.ringBytes;
        g.totalLiveBytes += node.liveBytes;
        if (d.rebasedPlane)
            g.hasRebasedPlane = true;
        if (node.history > maxHistory)
            maxHistory = node.history;
        g.nodes.push_back(node);
    }
    g.framesInFlight = size_t(gs.queueDepth) + gs.lookAheadWalk + maxHistory + R + gs.laneLead + 1u;
    g.valid = true;
    return g;
}

// Owns the current graph; rebuilds on a trigger and advances the Indicator's `graph` counter.
class GraphOwner {
public:
    const Graph& Current() const { return graph_; }
    const GraphKey& Key() const { return key_; }
    uint32_t Rebuilds() const { return rebuilds_; }
    uint32_t LastReasons() const { return lastReasons_; }

    // `stamp` = the Indicator stamp of the first unsealed frame; `fromSeq` = that frame. Returns the
    // rebuild reasons (0 = graph kept). `indicator` may be null (tests, Original pathway).
    uint32_t Update(const Registry& reg, const std::vector<ConsumerDecl>& consumers, const GraphSettings& gs,
                    const VpIndicator::Stamp& stamp, VpIndicator::Indicator* indicator, uint64_t fromSeq)
    {
        const GraphKey key = KeyOf(stamp, gs, consumers);
        const uint32_t reasons = RebuildReasons(key_, key, graph_.hasRebasedPlane);
        if (reasons == 0)
            return 0;
        graph_ = BuildGraph(reg, consumers, gs);
        key_ = key;
        ++rebuilds_;
        lastReasons_ = reasons;
        if (indicator)
            indicator->Advance(VpIndicator::kGraph, VpResync::Cause::GraphRebuild, fromSeq);
        return reasons;
    }

private:
    Graph graph_;
    GraphKey key_;
    uint32_t rebuilds_ = 0;
    uint32_t lastReasons_ = 0;
};

} // namespace VpSkeleton
