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
//               by lane readers x lane jobs in flight (a late lane reader may pin a released live
//               buffer until its job ends):
//               every frame the render holds may be
//               a live fill (design B3 says R + 1; that leaves no live buffer for a history frame
//               that was itself filled live, so an in-render-only history product could not run)
//
// history = max(the product's own history, the largest lag any active node or consumer reads it at).
//
// Look-ahead (K0): an input may carry a LEAD (frames ahead). The effective lead of a node is
// min(its largest declared lead, laneLead - 1): the lane makes N+L when frame N starts rendering, so a
// reader that needs N+L would have zero slack; one frame less is what is safely there. A product read at lead k gets k more ring slots, and
// framesInFlight grows by the largest effective lead. When the queue cannot supply any lead (effective
// lead 0) the node resolves to its shortQueueSubstitute (a visible, non-identical reduced mode, recorded in
// Graph::reductions and listed by Graph::ReducedNodes()). An effective lead above 0 but below the declared lead
// is a PARTIAL lead (Graph::PartialLeadNodes(): both numbers are on the Node). Lead edges are not lag-0 edges: they play no part in the topological order.
// Everything is fixed at build (no per-frame allocation). A rebuild advances the Indicator's
// `graph` counter (GraphOwner); every node honours `graph`, so lane work from the old graph is
// rejected at the seal. Header-only, pure C++14.
namespace VpSkeleton {

enum class QueueMode : uint8_t { Normal, Games, Competitive };

// A history depth the VRAM governor granted a governed-history product (VramGovernor.h PlanTiers) below its full
// depth. Carried in GraphSettings so the graph is built with it; a change is a rebuild (kRebuildHistory).
struct HistoryCap {
    ProductId id = kNoProduct;
    uint8_t history = 0;
    bool operator==(const HistoryCap& o) const { return id == o.id && history == o.history; }
    bool operator!=(const HistoryCap& o) const { return !(*this == o); }
};

struct GraphSettings {
    Dims dims;                    // source active picture
    uint8_t renderAhead = 0;      // R
    uint8_t laneLead = 1;         // L, measured to render start (0: backward pairs on arrival)
    uint16_t queueDepth = 0;      // queue capacity in frames (derived from the fixed-ms queue)
    uint16_t lookAheadWalk = 0;   // frames the look-ahead walks copy out of the queue
    QueueMode queueMode = QueueMode::Normal;
    bool ofaPresent = true;       // the card has an optical-flow engine
    uint64_t cardProfileKey = 0;  // CardProfile identity (adapter LUID + driver), a rebuild trigger
    uint8_t laneJobsInFlight = 1; // jobs one lane producer may hold pins for at once (OFA: up to its fence)
    std::vector<HistoryCap> historyCaps;   // governed-history products running below their full depth (sorted by id)
};

struct Node {
    ProductId id = kNoProduct;
    ProductId declared = kNoProduct;   // the id a consumer asked for (differs when substituted)
    Backend backend = Backend::Cpu;
    Tier tier = Tier::T0;
    uint8_t history = 0;               // the granted depth (what the ring and live pool are sized for)
    uint8_t historyFull = 0;           // the depth without a governor cap: max(declared, largest lag read)
    uint8_t historyMin = 0;            // the lowest depth the governor may grant (== historyFull: not governed)
    bool orderedChain = false;         // ProductDecl::orderedChain
    bool singletonStore = false;       // ProductDecl::singletonStore (one persistent store, read at lag 1; never governed)
    bool holdsSpan = false;
    uint8_t ring = 0;
    uint8_t livePool = 0;
    uint8_t inputCount = 0;
    std::array<InputRef, kMaxInputs> inputs{};   // substituted ids
    VpIndicator::CounterMask reads = 0;          // the product's reads plus `graph`
    VpIndicator::CounterMask spanMask = 0;
    bool missingInputForm = false;
    uint8_t lead = 0;                 // effective look-ahead of this node (0 = none)
    uint8_t declaredLead = 0;         // what the product declared (lead < declaredLead and lead > 0: a partial lead)
    uint8_t leadRead = 0;             // the largest effective lead any node reads THIS product at
    bool shortQueue = false;          // stands in for a look-ahead product the queue cannot feed
    ProductId reducedFrom = kNoProduct;   // that look-ahead product (declared id) when shortQueue
    bool laneVisible = false;         // ProductDecl::laneVisible (a view the lane can read: never demotes a lane reader)
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
    kRebuildHistory = 1u << 9,    // the governor granted a governed-history product another depth
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
        mix((c.holdsSpan ? 1u : 0u) | (c.hasReducedMode ? 2u : 0u) | (c.overflow ? 4u : 0u));
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
        || a.lookAheadWalk != b.lookAheadWalk || a.queueMode != b.queueMode
        || a.laneJobsInFlight != b.laneJobsInFlight)
        r |= kRebuildQueue;
    if (a.cardProfileKey != b.cardProfileKey || a.ofaPresent != b.ofaPresent)
        r |= kRebuildCard;
    if (a.dims != b.dims)
        r |= kRebuildDims;
    if (before.consumers != after.consumers)
        r |= kRebuildConsumers;
    if (a.historyCaps != b.historyCaps)
        r |= kRebuildHistory;
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
    // One record per (asked, made) substitution because the queue is short. A node that was ALSO asked
    // for directly is still made as itself: the record, not the node, says what was reduced.
    struct Reduction {
        ProductId asked = kNoProduct;
        ProductId made = kNoProduct;
    };
    std::vector<Reduction> reductions;

    int IndexOf(ProductId id) const
    {
        for (size_t i = 0; i < nodes.size(); ++i)
            if (nodes[i].id == id)
                return int(i);
        return -1;
    }
    bool Has(ProductId id) const { return IndexOf(id) >= 0; }
    // Nodes that stand in for a look-ahead product the queue cannot feed (plan 45: "camera: causal").
    // Lists a node even when it is also asked for directly; `reductions` names what was reduced.
    std::vector<const Node*> ReducedNodes() const
    {
        std::vector<const Node*> out;
        for (const Node& n : nodes)
            if (n.shortQueue)
                out.push_back(&n);
        return out;
    }
    // Nodes whose look-ahead is shorter than declared but not zero (lead < declaredLead).
    std::vector<const Node*> PartialLeadNodes() const
    {
        std::vector<const Node*> out;
        for (const Node& n : nodes)
            if (n.lead > 0 && n.lead < n.declaredLead)
                out.push_back(&n);
        return out;
    }
    // Nodes running below their full history because the budget could not fit it (visible reduced mode).
    std::vector<const Node*> HistoryReducedNodes() const
    {
        std::vector<const Node*> out;
        for (const Node& n : nodes)
            if (n.history < n.historyFull)
                out.push_back(&n);
        return out;
    }
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

// The largest lead any input of `d` declares.
inline uint8_t MaxDeclaredLead(const ProductDecl& d)
{
    uint8_t m = 0;
    for (size_t k = 0; k < d.inputCount; ++k)
        if (d.inputs[k].lead > m)
            m = d.inputs[k].lead;
    return m;
}

// The look-ahead the queue can supply to `d`: its declared lead capped by the lane lead (a render-only
// product has no lane, so none).
inline uint8_t EffectiveLead(const ProductDecl& d, const GraphSettings& gs)
{
    if (d.backend == Backend::RenderOnly)
        return 0;
    const uint8_t m = MaxDeclaredLead(d);
    const uint8_t avail = gs.laneLead > 0 ? uint8_t(gs.laneLead - 1) : uint8_t(0);
    return m < avail ? m : avail;
}

// Resolve with the queue: the no-engine substitute first, then the short-queue substitute for a
// look-ahead product that cannot get any lead. *shortQueue is set when the latter applied.
inline ProductId Resolve(const Registry& reg, ProductId id, const GraphSettings& gs, bool* shortQueue = nullptr)
{
    if (shortQueue)
        *shortQueue = false;
    ProductId p = Resolve(reg, id, gs.ofaPresent);
    if (p == kNoProduct)
        return p;
    const ProductDecl& d = reg.Get(p);
    if (MaxDeclaredLead(d) > 0 && EffectiveLead(d, gs) == 0 && d.shortQueueSubstitute != kNoProduct) {
        if (shortQueue)
            *shortQueue = true;
        return Resolve(reg, d.shortQueueSubstitute, gs.ofaPresent);
    }
    return p;
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
    std::vector<uint8_t> maxLeadRead(n, 0);
    std::vector<char> span(n, 0);
    std::vector<ProductId> declaredAs(n, kNoProduct);
    std::vector<ProductId> reducedFrom(n, kNoProduct);
    std::vector<ProductId> work;

    auto want = [&](ProductId asked, uint8_t lag, bool holdsSpan, uint8_t lead) -> bool {
        bool reduced = false;
        const ProductId p = Resolve(reg, asked, gs, &reduced);
        if (p == kNoProduct)
            return false;
        if (lag > maxLag[p])
            maxLag[p] = lag;
        if (lead > maxLeadRead[p])
            maxLeadRead[p] = lead;
        if (holdsSpan)
            span[p] = 1;
        if (reduced) {
            if (reducedFrom[p] == kNoProduct)
                reducedFrom[p] = asked;
            bool known = false;
            for (const Graph::Reduction& r : g.reductions)
                known = known || (r.asked == asked && r.made == p);
            if (!known) {
                Graph::Reduction r;
                r.asked = asked;
                r.made = p;
                g.reductions.push_back(r);
            }
        }
        if (!needed[p]) {
            needed[p] = 1;
            declaredAs[p] = asked;
            work.push_back(p);
        }
        return true;
    };

    for (const ConsumerDecl& c : consumers)
        if (c.overflow || c.count > kMaxConsumes) {
            g.error = "a consumer declares more reads than the seam allows (an extra read was dropped)";
            return g;
        }
    for (const ConsumerDecl& c : consumers)
        for (size_t k = 0; k < c.count && k < kMaxConsumes; ++k)
            if (!want(c.consumes[k].product, c.consumes[k].lag, c.holdsSpan, 0)) {
                g.error = "a consumer asks for an unknown product";
                return g;
            }
    // Closure: each product visited once. A lead input is read at the reader's effective lead.
    for (size_t w = 0; w < work.size(); ++w) {
        const ProductDecl& d = reg.Get(work[w]);
        const uint8_t eff = EffectiveLead(d, gs);
        for (size_t k = 0; k < d.inputCount; ++k) {
            const uint8_t lead = d.inputs[k].lead < eff ? d.inputs[k].lead : eff;
            want(d.inputs[k].product, d.inputs[k].lag, false, lead);
        }
    }

    // Topological order over lag-0 edges of the needed set; the smallest ready id first.
    std::vector<int> indeg(n, 0);
    for (size_t p = 0; p < n; ++p) {
        if (!needed[p])
            continue;
        const ProductDecl& d = reg.Get(ProductId(p));
        for (size_t k = 0; k < d.inputCount; ++k)
            if (d.inputs[k].lag == 0 && d.inputs[k].lead == 0)
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
                if (d.inputs[k].lag == 0 && d.inputs[k].lead == 0
                    && Resolve(reg, d.inputs[k].product, gs) == next)
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
            ++laneReadRefs[Resolve(reg, d.inputs[k].product, gs)];
    }

    const unsigned R = gs.renderAhead;
    size_t maxHistory = 0;
    unsigned maxLead = 0;
    for (ProductId p : order) {
        const ProductDecl& d = reg.Get(p);
        Node node;
        node.id = p;
        node.declared = declaredAs[p];
        node.backend = d.backend;
        node.tier = d.tier;
        node.laneVisible = d.laneVisible;
        node.historyFull = d.history > maxLag[p] ? d.history : maxLag[p];
        node.historyMin = node.historyFull;
        node.history = node.historyFull;
        if (d.historyFloor != kHistoryFixed) {
            const uint8_t floorEff = d.historyFloor > maxLag[p] ? d.historyFloor : maxLag[p];
            node.historyMin = floorEff < node.historyFull ? floorEff : node.historyFull;
            for (const HistoryCap& c : gs.historyCaps)
                if (c.id == p) {
                    uint8_t h = c.history < node.historyMin ? node.historyMin : c.history;
                    node.history = h < node.historyFull ? h : node.historyFull;
                }
        }
        node.orderedChain = d.orderedChain;
        node.singletonStore = d.singletonStore;
        node.holdsSpan = span[p] != 0;
        const bool lane = d.backend != Backend::RenderOnly;
        const unsigned L = lane ? gs.laneLead : 0u;
        const unsigned q = lane ? 1u : 0u;
        node.lead = EffectiveLead(d, gs);
        node.declaredLead = MaxDeclaredLead(d);
        node.leadRead = maxLeadRead[p];
        node.reducedFrom = reducedFrom[p];
        node.shortQueue = reducedFrom[p] != kNoProduct;
        const unsigned ring = unsigned(node.history) + 1u + R + L + q + (node.holdsSpan ? 1u : 0u)
                              + unsigned(node.leadRead);
        node.ring = uint8_t(ring > 255u ? 255u : ring);
        const unsigned live = unsigned(node.history) + 1u + R + (node.holdsSpan ? 1u : 0u)
                              + laneReadRefs[p] * unsigned(gs.laneJobsInFlight == 0 ? 1u : gs.laneJobsInFlight);
        node.livePool = uint8_t(live > 255u ? 255u : live);
        node.inputCount = d.inputCount;
        for (size_t k = 0; k < d.inputCount; ++k) {
            node.inputs[k].product = Resolve(reg, d.inputs[k].product, gs);
            node.inputs[k].lag = d.inputs[k].lag;
            node.inputs[k].lead = d.inputs[k].lead < node.lead ? d.inputs[k].lead : node.lead;
        }
        node.reads = d.reads | VpIndicator::Bit(VpIndicator::kGraph);
        node.spanMask = d.spanMask;
        node.missingInputForm = d.missingInputForm;
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
        if (node.lead > maxLead)
            maxLead = node.lead;
        g.nodes.push_back(node);
    }
    g.framesInFlight = size_t(gs.queueDepth) + gs.lookAheadWalk + maxHistory + R + gs.laneLead + 1u
                       + maxLead;
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
