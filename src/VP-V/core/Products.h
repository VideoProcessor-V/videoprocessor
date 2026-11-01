// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "Indicator.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

// Skeleton S0: the product registry (design doc 25 section B1). A product is a piece of analysis
// DATA made once per source frame (a re-base decision, half luma, a vector pair, ...). Each
// product declares its inputs (other products with a lag, the source frame, settings domains), its
// backend, history, size, the Indicator counters it honours, its tier, residency and variant kind.
// Components declare which products they CONSUME (ConsumerDecl); the graph (ProductGraph.h) keeps
// only the closure of what active consumers need, each product once.
//
// Header-only, pure C++14, no Windows, no GPU. The cost figures are planning estimates on the
// reference card (computeScale 1.0 = the RTX 3050 Ti laptop) at 1920x1080; measured per-product
// costs (PassTimers) replace them in the tier plan (CardProfile.h).
namespace VpSkeleton {

using ProductId = uint16_t;
constexpr ProductId kNoProduct = 0xFFFFu;

// The first products (design B9). P2m is the matcher pair: the motion product on cards without an
// optical-flow engine (B6). It is a separate id because its result is not identical to P2's.
enum : ProductId {
    kP0RebaseDecision = 0,
    kP1HalfLuma,
    kP2VectorPair,
    kP2mMatcherPair,
    kP3AnalysisField,
    kP4SourceStats,
    kP5MonitorTiles,
    kP6EntranceAhead,
    kFirstProductCount
};

// The camera-linking products (plan 45 section 1, K0). Appended AFTER the first products so the
// first ids and kFirstProductCount stay exactly as they were. SkeletonProducts() below is the one
// assembly point (later phases append region evidence and the light-level map there, in fixed order).
//   P7 camera fit       the per-frame global camera-motion fit (CPU), seeded by its own previous frame
//   P8 camera labels    the per-16x16-block camera/object label field (compute)
//   P9 camera smoothed  P7 smoothed over a look-ahead of +-kMaxCameraLead frames (needs the queue)
enum : ProductId {
    kP7CameraFit = kFirstProductCount,
    kP8CameraLabels,
    kP9CameraSmoothed,
    kCameraProductEnd
};

// Frames of look-ahead P9 reads from P7 at film depth (the declared lead; the effective lead is
// capped by the lane lead of the graph).
constexpr uint8_t kMaxCameraLead = 2;

enum class Backend : uint8_t { Ofa, GpuCompute, Cpu, RenderOnly };
enum class Tier : uint8_t { T0, T1, T2 };
enum class Residency : uint8_t { Vram, SystemRam };
enum class Variants : uint8_t { Identical, NonIdentical };

inline const char* Token(Backend b)
{
    switch (b) {
    case Backend::Ofa: return "ofa";
    case Backend::GpuCompute: return "compute";
    case Backend::Cpu: return "cpu";
    case Backend::RenderOnly: return "render";
    }
    return "?";
}

inline const char* Token(Tier t)
{
    switch (t) {
    case Tier::T0: return "T0";
    case Tier::T1: return "T1";
    case Tier::T2: return "T2";
    }
    return "?";
}

struct Dims {
    uint32_t width = 0;
    uint32_t height = 0;
};

inline bool operator==(const Dims& a, const Dims& b) { return a.width == b.width && a.height == b.height; }
inline bool operator!=(const Dims& a, const Dims& b) { return !(a == b); }

// bytes = fixed + sum over terms of ceil(w / 2^shift) * ceil(h / 2^shift) * bytesPerCell.
// shift 0 is full resolution.
struct SizeTerm {
    uint8_t shift = 0;
    uint16_t bytesPerCell = 0;
};

struct SizeRule {
    uint32_t fixedBytes = 0;
    uint8_t termCount = 0;
    std::array<SizeTerm, 2> terms{};
};

inline uint64_t CellsAt(Dims d, uint8_t shift)
{
    const uint64_t div = uint64_t(1) << shift;
    return ((uint64_t(d.width) + div - 1) / div) * ((uint64_t(d.height) + div - 1) / div);
}

inline uint64_t BytesOf(const SizeRule& r, Dims d)
{
    uint64_t total = r.fixedBytes;
    const size_t n = r.termCount < r.terms.size() ? size_t(r.termCount) : r.terms.size();
    for (size_t i = 0; i < n; ++i)
        total += CellsAt(d, r.terms[i].shift) * r.terms[i].bytesPerCell;
    return total;
}

// A full-resolution plane made per frame (never system RAM, doc 21 item 3).
inline bool IsFullResPerFrame(const SizeRule& r)
{
    const size_t n = r.termCount < r.terms.size() ? size_t(r.termCount) : r.terms.size();
    for (size_t i = 0; i < n; ++i)
        if (r.terms[i].shift == 0 && r.terms[i].bytesPerCell != 0)
            return true;
    return false;
}

struct InputRef {
    ProductId product = kNoProduct;
    uint8_t lag = 0;   // frames back: 0 = the same source frame, 1 = the previous one
    // Frames AHEAD (N+lead): a look-ahead read of a product the queue has already analysed. An input
    // has a lag OR a lead, never both. A lead edge is not a lag-0 edge (no cycle check, no topological
    // constraint). The product that reads a lead must declare shortQueueSubstitute.
    //
    // Seal note (S3/K1, NOT implemented in K0): the seal validates a LEAD ancestor as Published with
    // stamps == StampOf(N+k) (it need not be Sealed: frame N+k has not reached its own seal yet).
    // ProductSlots.h today computes every ancestor as seq - lag and ignores `lead`.
    uint8_t lead = 0;
};

constexpr size_t kMaxInputs = 4;

struct ProductDecl {
    ProductId id = kNoProduct;
    const char* token = "";
    const char* name = "";
    uint8_t inputCount = 0;
    std::array<InputRef, kMaxInputs> inputs{};
    bool sourceFrame = false;                 // reads the source frame itself
    VpIndicator::CounterMask settings = 0;    // settings domains (kSettings* bits)
    VpIndicator::CounterMask reads = 0;       // every counter its validity honours (includes settings)
    // Counters that must be equal across its temporal (lag > 0) inputs: a pair spanning a change of
    // one of these is invalid ("reset" policy). 0 = no span rule.
    VpIndicator::CounterMask spanMask = 0;
    Backend backend = Backend::Cpu;
    uint8_t history = 0;                      // previous frames kept by the product itself
    SizeRule size;
    Tier tier = Tier::T0;
    Residency residency = Residency::Vram;    // preferred; CardProfile.h applies the system-RAM rules
    Variants variants = Variants::Identical;  // of its in-render fallback (B4 fallback rules)
    bool rebasedPlane = false;                // sized on the re-based plane: an axis change rebuilds
    bool rarelyTouched = false;               // read rarely: may live in system RAM (doc 21 item 3)
    ProductId noOfaSubstitute = kNoProduct;   // used instead on cards without an optical-flow engine
    // Used instead when the queue cannot supply the declared lead (lane lead 0: Competitive mode or
    // queue depth 0). It is a NON-identical, visibly reduced mode ("camera: causal (queue short)"),
    // never a silent one: the graph lists it in Graph::ReducedNodes().
    ProductId shortQueueSubstitute = kNoProduct;
    // A declared form for a live fill with an input missing (e.g. no previous frame after a reset).
    // Without it such a fill is refused: the product is unavailable for that frame (visible).
    bool missingInputForm = false;
    float refRenderMs = 0.0f;                 // estimated in-render cost on the reference card, 1080p
};

// Products declared outside FirstProducts by their owning module (dense ids appended at plan time) keep a registered
// token here so every reader names the same node.
//   grain.split  the shared grain product (grain-free picture + grain layer of the decoded source, computed once, early).
//                Owner and declaration: input.source_analysis/regions (RegionEvidence::AppendProducts, GrainProduct; the
//                one read API is RegionEvidence::GrainView). Implementation: repair.grain_split (GrainSplit::Compute on the
//                CPU, the GrainSplit::Gpu::Splitter passes on the GPU). Readers: region block stats, repair.temporal_detail,
//                scaling.chroma_upscale (CfL grain guard), camera-model grain map, grain-safe contrast recovery.
constexpr const char* kTokenGrainSplit = "grain.split";

namespace detail {

inline ProductDecl Make(ProductId id, const char* token, const char* name, Backend backend, uint8_t history,
                        Tier tier, VpIndicator::CounterMask reads,
                        VpIndicator::CounterMask settings)
{
    ProductDecl d;
    d.id = id;
    d.token = token;
    d.name = name;
    d.backend = backend;
    d.history = history;
    d.tier = tier;
    d.reads = reads | settings;
    d.settings = settings;
    return d;
}

inline void AddInput(ProductDecl& d, ProductId p, uint8_t lag)
{
    if (d.inputCount < kMaxInputs) {
        d.inputs[d.inputCount].product = p;
        d.inputs[d.inputCount].lag = lag;
        d.inputs[d.inputCount].lead = 0;
        ++d.inputCount;
    }
}

// A look-ahead input: `lead` frames ahead (lag stays 0).
inline void AddLead(ProductDecl& d, ProductId p, uint8_t lead)
{
    if (d.inputCount < kMaxInputs) {
        d.inputs[d.inputCount].product = p;
        d.inputs[d.inputCount].lag = 0;
        d.inputs[d.inputCount].lead = lead;
        ++d.inputCount;
    }
}

inline void AddTerm(ProductDecl& d, uint8_t shift, uint16_t bytesPerCell)
{
    if (d.size.termCount < d.size.terms.size()) {
        d.size.terms[d.size.termCount].shift = shift;
        d.size.terms[d.size.termCount].bytesPerCell = bytesPerCell;
        ++d.size.termCount;
    }
}

} // namespace detail

// The registry: products indexed by their id (dense 0..n-1).
class Registry {
public:
    Registry() = default;
    explicit Registry(std::vector<ProductDecl> decls) : decls_(std::move(decls)) {}

    size_t Size() const { return decls_.size(); }
    bool Has(ProductId id) const { return id < decls_.size() && decls_[id].id == id; }
    const ProductDecl& Get(ProductId id) const { return decls_[id]; }
    const std::vector<ProductDecl>& All() const { return decls_; }

    // Ids dense and in place, inputs and substitutes known, at most one lag-0 path per pair (no
    // lag-0 cycle: the seal runs in topological order over lag-0 edges). Returns false with a
    // plain reason on the first problem.
    bool Validate(const char** reason = nullptr) const
    {
        auto fail = [reason](const char* why) {
            if (reason)
                *reason = why;
            return false;
        };
        for (size_t i = 0; i < decls_.size(); ++i) {
            const ProductDecl& d = decls_[i];
            if (d.id != i)
                return fail("product ids are not dense");
            if (d.inputCount > kMaxInputs)
                return fail("too many inputs");
            bool hasLead = false;
            for (size_t k = 0; k < d.inputCount; ++k) {
                const InputRef& in = d.inputs[k];
                if (!Has(in.product))
                    return fail("unknown input product");
                if (in.lead > 0) {
                    hasLead = true;
                    if (in.product == d.id)
                        return fail("product reads itself at a lead");
                    if (in.lag != 0)
                        return fail("an input has a lag or a lead, not both");
                } else if (in.lag == 0 && in.product == d.id) {
                    return fail("product reads itself at lag 0");
                }
            }
            if (hasLead && d.shortQueueSubstitute == kNoProduct)
                return fail("a look-ahead product needs a short-queue substitute");
            if (d.shortQueueSubstitute != kNoProduct) {
                if (!Has(d.shortQueueSubstitute) || d.shortQueueSubstitute == d.id)
                    return fail("bad short-queue substitute");
                const ProductDecl& sub = decls_[d.shortQueueSubstitute];
                for (size_t k = 0; k < sub.inputCount; ++k) {
                    if (sub.inputs[k].lead > 0)
                        return fail("short-queue substitute has a look-ahead input");
                    if (sub.inputs[k].product == d.id)
                        return fail("short-queue substitute reads the product it replaces");
                }
            }
            if (d.noOfaSubstitute != kNoProduct) {
                if (!Has(d.noOfaSubstitute) || d.backend != Backend::Ofa)
                    return fail("bad optical-flow substitute");
                if (decls_[d.noOfaSubstitute].backend == Backend::Ofa)
                    return fail("optical-flow substitute needs the engine");
            }
        }
        // Look-ahead edges: no chains (the target has no lead inputs itself) and no loops (the target
        // cannot reach the reader through ANY edge, lag, lag 0 or lead): a frame-ahead read must end at
        // a product made from the source alone.
        for (const ProductDecl& d : decls_)
            for (size_t k = 0; k < d.inputCount; ++k) {
                if (d.inputs[k].lead == 0)
                    continue;
                const ProductDecl& target = decls_[d.inputs[k].product];
                for (size_t j = 0; j < target.inputCount; ++j)
                    if (target.inputs[j].lead > 0)
                        return fail("a look-ahead input names a product that itself looks ahead");
                std::vector<char> seen(decls_.size(), 0);
                std::vector<ProductId> stack(1, target.id);
                seen[target.id] = 1;
                while (!stack.empty()) {
                    const ProductId p = stack.back();
                    stack.pop_back();
                    if (p == d.id)
                        return fail("a look-ahead input closes a loop");
                    for (size_t j = 0; j < decls_[p].inputCount; ++j) {
                        const ProductId q = decls_[p].inputs[j].product;
                        if (!seen[q]) {
                            seen[q] = 1;
                            stack.push_back(q);
                        }
                    }
                }
            }
        // Lag-0 cycle check (Kahn).
        std::vector<int> indeg(decls_.size(), 0);
        for (const ProductDecl& d : decls_)
            for (size_t k = 0; k < d.inputCount; ++k)
                if (d.inputs[k].lag == 0 && d.inputs[k].lead == 0)
                    ++indeg[d.id];
        std::vector<ProductId> ready;
        for (size_t i = 0; i < decls_.size(); ++i)
            if (indeg[i] == 0)
                ready.push_back(ProductId(i));
        size_t done = 0;
        while (!ready.empty()) {
            const ProductId p = ready.back();
            ready.pop_back();
            ++done;
            for (const ProductDecl& d : decls_)
                for (size_t k = 0; k < d.inputCount; ++k)
                    if (d.inputs[k].lag == 0 && d.inputs[k].lead == 0 && d.inputs[k].product == p
                        && --indeg[d.id] == 0)
                        ready.push_back(d.id);
        }
        if (done != decls_.size())
            return fail("lag-0 cycle between products");
        if (reason)
            *reason = "";
        return true;
    }

private:
    std::vector<ProductDecl> decls_;
};

// The first products (B9) with their declarations.
inline Registry FirstProducts()
{
    using detail::AddInput;
    using detail::AddTerm;
    using detail::Make;
    using namespace VpIndicator;
    const CounterMask fullOnly = Bit(kFull);
    const CounterMask content = Bit(kFull) | Bit(kContinuity);
    std::vector<ProductDecl> d;

    // P0: per-axis factor, kernel and fade (R4a: made on the render thread).
    ProductDecl p0 = Make(kP0RebaseDecision, "p0_rebase_decision", "re-base decision", Backend::RenderOnly, 1,
                          Tier::T0, fullOnly | Bit(kRebaseH) | Bit(kRebaseV), Bit(kSettingsRebase));
    p0.sourceFrame = true;
    p0.size.fixedBytes = 256;
    p0.refRenderMs = 0.01f;
    d.push_back(p0);

    // P1: half luma plus quarter luma, 16-bit.
    ProductDecl p1 = Make(kP1HalfLuma, "p1_half_luma", "half luma", Backend::Cpu, 1, Tier::T0, fullOnly, 0);
    p1.sourceFrame = true;
    AddTerm(p1, 1, 2);
    AddTerm(p1, 2, 2);
    p1.refRenderMs = 0.6f;
    d.push_back(p1);

    // P2: vector-engine pair (forward + backward + cost + global) on the 4x4 grid of half luma.
    ProductDecl p2 = Make(kP2VectorPair, "p2_vector_pair", "vector-engine pair", Backend::Ofa, 1, Tier::T0, content,
                          Bit(kSettingsMotion));
    AddInput(p2, kP1HalfLuma, 0);
    AddInput(p2, kP1HalfLuma, 1);
    AddTerm(p2, 3, 10);   // per 8x8 source cell: two directions x (2 x int16 flow + cost byte)
    p2.size.fixedBytes = 64;
    p2.spanMask = content;
    p2.variants = Variants::NonIdentical;   // render-time fallback is the medium preset
    p2.noOfaSubstitute = kP2mMatcherPair;
    p2.refRenderMs = 11.0f;
    d.push_back(p2);

    // P2m: the matcher pair, the motion product without an optical-flow engine.
    ProductDecl p2m = Make(kP2mMatcherPair, "p2m_matcher_pair", "matcher pair", Backend::GpuCompute, 1, Tier::T0,
                           content, Bit(kSettingsMotion));
    AddInput(p2m, kP1HalfLuma, 0);
    AddInput(p2m, kP1HalfLuma, 1);
    AddTerm(p2m, 3, 10);
    p2m.size.fixedBytes = 64;
    p2m.spanMask = content;
    p2m.refRenderMs = 6.0f;
    d.push_back(p2m);

    // P3: analysis motion field (+ grain error, occlusion) per 8x8 source block.
    ProductDecl p3 = Make(kP3AnalysisField, "p3_analysis_field", "analysis motion field", Backend::GpuCompute, 1,
                          Tier::T0, content, Bit(kSettingsMotion) | Bit(kSettingsAnalysis));
    AddInput(p3, kP1HalfLuma, 0);
    AddInput(p3, kP1HalfLuma, 1);
    AddInput(p3, kP2VectorPair, 0);
    AddTerm(p3, 3, 16);
    p3.spanMask = content;
    p3.refRenderMs = 2.5f;
    d.push_back(p3);

    // P4: source stats (cut/fade/flash, rect, histogram, grain); history = the stats window.
    ProductDecl p4 = Make(kP4SourceStats, "p4_source_stats", "source stats", Backend::Cpu, 1, Tier::T0, content,
                          Bit(kSettingsAnalysis));
    AddInput(p4, kP1HalfLuma, 0);
    p4.size.fixedBytes = 8192;
    p4.residency = Residency::SystemRam;
    p4.missingInputForm = true;   // stats of a frame need no history to start
    p4.refRenderMs = 0.4f;
    d.push_back(p4);

    // P5: monitor tiles (64x64 tiles, 32 bytes each), async readback.
    ProductDecl p5 = Make(kP5MonitorTiles, "p5_monitor_tiles", "monitor tiles", Backend::GpuCompute, 1, Tier::T1,
                          fullOnly, Bit(kSettingsAnalysis));
    p5.sourceFrame = true;
    AddTerm(p5, 6, 32);
    p5.refRenderMs = 0.5f;
    d.push_back(p5);

    // P6: entrance output of N+1 (RGBA16F on the re-based plane), dropped first.
    ProductDecl p6 = Make(kP6EntranceAhead, "p6_entrance_ahead", "entrance output ahead", Backend::GpuCompute, 1,
                          Tier::T1, content | Bit(kRebaseH) | Bit(kRebaseV),
                          Bit(kSettingsRebase) | Bit(kSettingsAnalysis));
    p6.sourceFrame = true;
    AddInput(p6, kP0RebaseDecision, 0);
    AddInput(p6, kP3AnalysisField, 0);
    AddTerm(p6, 0, 8);
    p6.rebasedPlane = true;
    p6.refRenderMs = 1.5f;
    d.push_back(p6);

    return Registry(std::move(d));
}

// The camera products (P7..P9), appended after the first products. Their ids are the enum values
// above, so they must be appended onto exactly the first products (Registry::Validate rejects a
// registry whose ids are not dense).
inline void AppendCameraProducts(std::vector<ProductDecl>& decls)
{
    using detail::AddInput;
    using detail::AddLead;
    using detail::AddTerm;
    using detail::Make;
    using namespace VpIndicator;
    const CounterMask content = Bit(kFull) | Bit(kContinuity);

    // P7: the per-frame global camera fit; its own previous frame seeds the temporal fit (history 1).
    ProductDecl p7 = Make(kP7CameraFit, "p7_camera_fit", "camera fit", Backend::Cpu, 1, Tier::T0, content,
                          Bit(kSettingsMotion));
    AddInput(p7, kP3AnalysisField, 0);
    AddInput(p7, kP4SourceStats, 0);
    AddInput(p7, kP2VectorPair, 0);   // resolves to P2m on a card without an engine
    p7.residency = Residency::SystemRam;
    p7.spanMask = content;
    p7.size.fixedBytes = 1024;        // per-block object arrays only with `objects` (stays off)
    p7.refRenderMs = 0.5f;
    decls.push_back(p7);

    // P8: the camera/object label field, RGBA16F per 16x16 block plus the LabelConstants header.
    ProductDecl p8 = Make(kP8CameraLabels, "p8_camera_labels", "camera labels", Backend::GpuCompute, 0, Tier::T0,
                          content, 0);
    AddInput(p8, kP7CameraFit, 0);
    AddInput(p8, kP3AnalysisField, 0);
    AddTerm(p8, 4, 8);
    p8.size.fixedBytes = 256;
    p8.refRenderMs = 0.1f;
    decls.push_back(p8);

    // P9: the smoothed camera, read over +-kMaxCameraLead frames of P7 (non-identical reduced mode:
    // causal P7 when the queue is short).
    ProductDecl p9 = Make(kP9CameraSmoothed, "p9_camera_smoothed", "camera smoothed", Backend::Cpu, 0, Tier::T1,
                          content, Bit(kSettingsMotion));
    AddInput(p9, kP7CameraFit, 0);
    AddLead(p9, kP7CameraFit, kMaxCameraLead);
    p9.residency = Residency::SystemRam;
    p9.variants = Variants::NonIdentical;
    p9.spanMask = content;
    p9.size.fixedBytes = 256;
    p9.shortQueueSubstitute = kP7CameraFit;
    p9.refRenderMs = 0.05f;
    decls.push_back(p9);
}

// The registry the skeleton runs with: the first products plus the camera products, in fixed id order.
inline Registry SkeletonProducts()
{
    std::vector<ProductDecl> d = FirstProducts().All();
    AppendCameraProducts(d);
    return Registry(std::move(d));
}

// A component's consumption: which products it reads, and at which lag.
struct ConsumeRef {
    ProductId product = kNoProduct;
    uint8_t lag = 0;
};

constexpr size_t kMaxConsumes = 4;

struct ConsumerDecl {
    uint16_t id = 0;
    const char* name = "";
    uint8_t count = 0;
    std::array<ConsumeRef, kMaxConsumes> consumes{};
    bool holdsSpan = false;       // holds the next frame's product through the presentation span
    bool hasReducedMode = false;  // can run visibly reduced when a product is dropped (regulator)
};

inline void AddConsume(ConsumerDecl& c, ProductId p, uint8_t lag = 0)
{
    if (c.count < kMaxConsumes) {
        c.consumes[c.count].product = p;
        c.consumes[c.count].lag = lag;
        ++c.count;
    }
}

} // namespace VpSkeleton
