// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "FallbackCatalog.h"
#include "ProductGraph.h"
#include "Products.h"

#include <VP-V/modules/motion/estimation/VectorDemandProducts.h>
#include <VP-V/modules/motion/estimation/global_model/CameraProducts.h>

#include <array>
#include <cassert>
#include <initializer_list>
#include <utility>
#include <cstdint>
#include <string>
#include <vector>

// The ONE catalog of the product graph (SA0, doc 51 sections 1 and 3). Products.h declares what a product IS
// (registry); this adds, per product, WHO makes it and WHO fills it live, and per consumer WHERE it sits in the
// pathway, what it reads and what it does when a product is not there:
//
//   product : producing UNIT path + unit version (a uint32 constant in that unit's header; the number the core
//             expects is HERE and a test compares), the lane producer (or none), the LIVE-FILL OWNER (the first
//             consumer in pathway order: when the lane missed a frame it fills the product live, so two producers
//             never write one slot for one frame), the consumers, the stamps it honours, tier, residency, VRAM.
//   consumer: id, name, module path, pathway order (input < motion < repair < scaling < colour < drive < output;
//             within a unit the render order), products read, and a FALLBACK {kind, FallbackCatalog id}.
//
// The module builders (VectorDemandProducts.h, CameraProducts.h) stay; this catalog reuses their declarations and a
// test checks the two agree. Declarations only: nothing here changes a slot or a pipeline. Header-only, pure, C++14.
namespace VpSkeleton {
namespace Catalog {

enum class PathwayUnit : uint8_t { Input, Motion, Repair, Scaling, Colour, Drive, Output };
enum class FallbackKind : uint8_t { OwnReduced, LowerTier, Off };

inline const char* Token(PathwayUnit u)
{
    switch (u) {
    case PathwayUnit::Input: return "input";
    case PathwayUnit::Motion: return "motion";
    case PathwayUnit::Repair: return "repair";
    case PathwayUnit::Scaling: return "scaling";
    case PathwayUnit::Colour: return "colour";
    case PathwayUnit::Drive: return "drive";
    case PathwayUnit::Output: return "output";
    }
    return "?";
}

inline const char* Token(FallbackKind k)
{
    switch (k) {
    case FallbackKind::OwnReduced: return "own reduced form";
    case FallbackKind::LowerTier: return "lower tier";
    case FallbackKind::Off: return "off";
    }
    return "?";
}

// ---- products ------------------------------------------------------------------------------------------------

struct ProductRow {
    ProductId id;
    const char* unit;           // producing unit path (module tree)
    uint32_t unitVersion;       // the version the core EXPECTS of that unit (0 = the unit has no version constant yet)
    const char* laneProducer;   // "" = none (render only / produced by its owner outside the lane)
};

// Unit versions the core expects. A unit bumps its constant on ANY change of the law, a pass or a layout and the
// expected number here in the same commit; a reader built against another number engages a visible fallback
// (VerifyUnit). P10/P10c: the grain split unit defines GrainSplit::kUnitVersion with SA2 (2 = compact blocks carry
// the band energies); the expectation is declared here so the catalog is complete.
constexpr uint32_t kExpectedCameraUnit = 1;
constexpr uint32_t kExpectedRegionUnit = 2;
constexpr uint32_t kExpectedTemporalDetailUnit = 1;
constexpr uint32_t kExpectedLightLevelUnit = 2;
constexpr uint32_t kExpectedVectorUnit = 1;
constexpr uint32_t kExpectedStatsUnit = 1;
constexpr uint32_t kExpectedGrainSplitUnit = 2;
constexpr uint32_t kExpectedRebaseUnit = 1;
constexpr uint32_t kExpectedHalfLumaUnit = 1;
constexpr uint32_t kExpectedMonitorUnit = 1;

inline const std::vector<ProductRow>& ProductRows()
{
    static const std::vector<ProductRow> rows = {
        { kP0RebaseDecision, "scaling/rebase", kExpectedRebaseUnit, "" },
        { kP1HalfLuma, "input/source_analysis/lane", kExpectedHalfLumaUnit, "HalfLumaProducer" },
        { kP2VectorPair, "motion/estimation", kExpectedVectorUnit, "NVOF producer (MotionAheadProducer)" },
        { kP2mMatcherPair, "motion/estimation", kExpectedVectorUnit, "matcher (DejudderWarp estimate)" },
        { kP3AnalysisField, "motion/estimation", kExpectedVectorUnit, "analysis field pass" },
        { kP4SourceStats, "input/source_analysis/lane", kExpectedStatsUnit, "SourceStatsProducer" },
        { kP5MonitorTiles, "input/source_analysis", kExpectedMonitorUnit, "tile pass + async readback" },
        { kP6EntranceAhead, "scaling/rebase", kExpectedRebaseUnit, "entrance B-ahead" },
        { kP7CameraFit, "motion/estimation/global_model", kExpectedCameraUnit, "camera fit (CPU worker)" },
        { kP8CameraLabels, "motion/estimation/global_model", kExpectedCameraUnit, "camera label pass" },
        { kP9CameraSmoothed, "motion/estimation/global_model", kExpectedCameraUnit, "camera smoothing (CPU)" },
        { kP10GrainSplit, "repair/grain_split", kExpectedGrainSplitUnit, "grain split pass" },
        { kP10cGrainSplitChroma, "repair/grain_split", kExpectedGrainSplitUnit, "grain split chroma pass" },
        { kP11RegionBlockStats, "input/source_analysis/regions", kExpectedRegionUnit, "region block statistics pass" },
        { kP12LightLevel, "input/source_analysis/light_level", kExpectedLightLevelUnit, "light level pass" },
        { kP13SourceRing, "input/capture (queue + early upload)", 0, "" },
        { kTdAnalysis, "repair/temporal_detail", kExpectedTemporalDetailUnit, "td analysis pass" },
        { kTdRefstore, "repair/temporal_detail", kExpectedTemporalDetailUnit, "" },
        { kTdAdmission, "repair/temporal_detail", kExpectedTemporalDetailUnit, "td admission cleaner" },
    };
    return rows;
}

inline const ProductRow* RowOf(ProductId id)
{
    for (const ProductRow& r : ProductRows())
        if (r.id == id)
            return &r;
    return nullptr;
}

// ---- consumers -----------------------------------------------------------------------------------------------

struct ConsumerRow {
    ConsumerDecl decl;
    const char* module;       // module path (source tree)
    PathwayUnit unit;
    uint16_t order;           // render order within the unit; unique per (unit, order)
    FallbackKind kind;
    const char* fallbackId;   // an entry of FallbackCatalog::ProductEntries
    const char* family;       // the component this row belongs to: several rows (module row, vector demand, camera) of
                              // one component are ONE reader for ownership and the document
};

// Sorting key: pathway unit first, then render order.
inline uint32_t PathwayKey(const ConsumerRow& c) { return uint32_t(c.unit) * 10000u + c.order; }

constexpr uint16_t kCatalogConsumerIdBase = 0x0300;

inline ConsumerRow MakeRow(uint16_t index, const char* name, const char* module, PathwayUnit unit, uint16_t order,
                           std::initializer_list<ProductId> reads, FallbackKind kind, const char* fallbackId,
                           bool holdsSpan = false)
{
    ConsumerRow r;
    r.decl.id = uint16_t(kCatalogConsumerIdBase + index);
    r.decl.name = name;
    r.family = name;
    assert(reads.size() <= kMaxConsumes);   // a row never silently drops a read
    for (ProductId p : reads)
        AddConsume(r.decl, p);
    r.decl.holdsSpan = holdsSpan;
    r.decl.hasReducedMode = true;
    r.module = module;
    r.unit = unit;
    r.order = order;
    r.kind = kind;
    r.fallbackId = fallbackId;
    return r;
}

// Real render-order positions of the vector-demand consumers (within their pathway unit; the module rows use 10, 20, ...
// and these sit next to the component they belong to). The statistics consumers read the vector pair's global vector
// right after the producer, so they are motion-unit readers of P2; the entrance segment belongs to the input side.
struct Placement {
    PathwayUnit unit;
    uint16_t order;
    const char* family;
};

inline Placement PlacementOf(VectorDemand::Consumer c)
{
    using C = VectorDemand::Consumer;
    switch (c) {
    case C::DejudderExtras: return { PathwayUnit::Motion, 50, "de-judder extras" };
    case C::Interpolator: return { PathwayUnit::Motion, 41, "interpolator / hiccup repair" };
    case C::HiccupRepair: return { PathwayUnit::Motion, 66, "interpolator / hiccup repair" };
    case C::OcclusionMask: return { PathwayUnit::Motion, 81, "occlusion mask" };
    case C::SigmaDelta: return { PathwayUnit::Output, 11, "sigma-delta (analysis field mapped)" };
    case C::GrainMap: return { PathwayUnit::Repair, 5, "grain map" };
    case C::DebandHold: return { PathwayUnit::Repair, 41, "debander (with hold)" };
    case C::DetailPulse: return { PathwayUnit::Repair, 21, "detail pulse" };
    case C::EntranceSegment: return { PathwayUnit::Input, 45, "entrance stages" };
    case C::SourceMonitorPan: return { PathwayUnit::Motion, 30, "source monitor pan" };
    case C::MotionIntent: return { PathwayUnit::Motion, 35, "motion intent" };
    case C::TextureRestore: return { PathwayUnit::Repair, 31, "texture restore" };
    case C::ResidualStabilise: return { PathwayUnit::Repair, 61, "residual stabilise" };
    case C::TemporalDetail: return { PathwayUnit::Repair, 11, "temporal detail" };
    }
    return { PathwayUnit::Motion, 99, "?" };
}

inline Placement PlacementOf(CameraLink::CameraConsumer c)
{
    using CameraLink::CameraConsumer;
    switch (c) {
    case CameraConsumer::Diagnostics: return { PathwayUnit::Output, 90, "camera diagnostics" };
    case CameraConsumer::GrainMap: return { PathwayUnit::Repair, 6, "grain map" };
    case CameraConsumer::ResidualStabilise: return { PathwayUnit::Repair, 62, "residual stabilise" };
    case CameraConsumer::TemporalDetail: return { PathwayUnit::Repair, 12, "temporal detail" };
    case CameraConsumer::DetailPulse: return { PathwayUnit::Repair, 22, "detail pulse" };
    case CameraConsumer::TextureRestore: return { PathwayUnit::Repair, 32, "texture restore" };
    case CameraConsumer::Debander: return { PathwayUnit::Repair, 42, "debander (with hold)" };
    case CameraConsumer::RollOff: return { PathwayUnit::Repair, 70, "camera roll-off" };
    case CameraConsumer::DitherVectors: return { PathwayUnit::Output, 12, "sigma-delta (analysis field mapped)" };
    case CameraConsumer::OcclusionFiller: return { PathwayUnit::Motion, 45, "occlusion mask" };
    case CameraConsumer::Measurement: return { PathwayUnit::Motion, 25, "camera measurement" };
    case CameraConsumer::DejudderWarp: return { PathwayUnit::Motion, 55, "de-judder extras" };
    case CameraConsumer::HiccupRepair: return { PathwayUnit::Motion, 67, "interpolator / hiccup repair" };
    case CameraConsumer::ObjectSplit: return { PathwayUnit::Motion, 27, "camera object split" };
    }
    return { PathwayUnit::Motion, 99, "?" };
}

// Every consumer, in a fixed order: the module consumers of doc 51 section 1, then the VectorDemand consumers and the
// camera consumers (their declarations are reused, so their ids and reads agree with the module builders).
inline const std::vector<ConsumerRow>& ConsumerRows()
{
    static const std::vector<ConsumerRow> rows = [] {
        std::vector<ConsumerRow> r;
        const FallbackKind own = FallbackKind::OwnReduced;
        const FallbackKind lower = FallbackKind::LowerTier;
        // Input
        r.push_back(MakeRow(7, "source resolution monitor", "VP-V/modules/input/source_analysis/SourceResolutionMonitor",
                            PathwayUnit::Input, 10, { kP5MonitorTiles, kP4SourceStats }, own, "product.source_resolution"));
        r.push_back(MakeRow(8, "source identification", "VP-V/modules/input/source_analysis/identity", PathwayUnit::Input,
                            20, { kP5MonitorTiles, kP4SourceStats }, own, "product.source_id"));
        r.push_back(MakeRow(6, "HDR inference", "VP-V/modules/input/signal", PathwayUnit::Input, 30, { kP4SourceStats }, own,
                            "product.hdr_inference"));
        r.push_back(MakeRow(12, "entrance stages", "VP-V/core/EntranceStages", PathwayUnit::Input, 40,
                            { kP0RebaseDecision, kP6EntranceAhead }, FallbackKind::Off, "product.entrance"));
        r.push_back(MakeRow(15, "region evidence (derived maps)", "VP-V/modules/input/source_analysis/regions",
                            PathwayUnit::Input, 50, { kP10GrainSplit, kP11RegionBlockStats, kP12LightLevel }, own,
                            "product.region_evidence"));
        // The optical-flow producer's module is motion/estimation, but it runs AHEAD in the lane (before any render-side reader),
        // so it ranks first in the pathway (input, order 5): it is the live-fill owner of P1 and P2.
        // Motion
        r.push_back(MakeRow(14, "optical-flow producer", "VP-V/modules/motion/estimation/MotionAheadProducer",
                            PathwayUnit::Input, 5, { kP1HalfLuma, kP2VectorPair }, own, "product.nvof_producer"));
        r.push_back(MakeRow(13, "matcher / motion warp", "VP-V/modules/motion/consumers/DejudderWarp", PathwayUnit::Motion,
                            20, { kP1HalfLuma, kP2VectorPair, kP4SourceStats }, own, "product.motion_warp"));
        r.push_back(MakeRow(9, "interpolator / hiccup repair", "VP-V/modules/motion/consumers", PathwayUnit::Motion, 40,
                            { kP3AnalysisField, kP10GrainSplit }, own, "product.interpolator"));
        // Repair
        r.push_back(MakeRow(10, "temporal detail", "VP-V/modules/repair/temporal_detail", PathwayUnit::Repair, 10,
                            { kP10GrainSplit, kP11RegionBlockStats, kP13SourceRing, kTdAnalysis, kTdRefstore, kTdAdmission },
                            lower, "product.temporal_detail"));
        r.push_back(MakeRow(0, "detail pulse", "VP-V/modules/repair/detail_pulse", PathwayUnit::Repair, 20,
                            { kP10GrainSplit, kP11RegionBlockStats, kP4SourceStats }, own, "product.detail_pulse"));
        r.push_back(MakeRow(1, "texture restore", "VP-V/modules/repair/texture_restore", PathwayUnit::Repair, 30,
                            { kP10GrainSplit, kP11RegionBlockStats, kP4SourceStats }, own, "product.texture_restore"));
        r.push_back(MakeRow(2, "debander (with hold)", "VP-V/modules/repair/deband", PathwayUnit::Repair, 40,
                            { kP10GrainSplit, kP11RegionBlockStats, kP12LightLevel, kP3AnalysisField }, own,
                            "product.debander", true));
        r.push_back(MakeRow(16, "light-level users", "VP-V/modules/repair", PathwayUnit::Repair, 50,
                            { kP12LightLevel, kP11RegionBlockStats }, own, "product.light_level"));
        // Scaling
        r.push_back(MakeRow(11, "chroma from luma", "VP-V/modules/scaling/chroma_upscale", PathwayUnit::Scaling, 10,
                            { kP10GrainSplit, kP10cGrainSplitChroma, kP13SourceRing }, lower, "product.chroma_cfl"));
        r.push_back(MakeRow(3, "adaptive downscale (grain map)", "VP-V/modules/scaling/adaptive_downscale",
                            PathwayUnit::Scaling, 20, { kP11RegionBlockStats, kP10GrainSplit, kP4SourceStats }, own,
                            "product.adaptive_downscale"));
        // Drive and output
        r.push_back(MakeRow(5, "LED look-ahead", "VP-V/modules/drive/led_measure", PathwayUnit::Drive, 10, { kP4SourceStats },
                            own, "product.led_lookahead"));
        r.push_back(MakeRow(4, "sigma-delta (analysis field mapped)", "VP-V/modules/output/dither", PathwayUnit::Output, 10,
                            { kP3AnalysisField }, own, "product.sigma_delta", true));
        // VectorDemand consumers: ids and reads from VectorDemandProducts.h
        for (int i = 0; i < VectorDemand::kConsumerCount; ++i) {
            const VectorDemand::Consumer c = VectorDemand::ConsumerAt(i);
            ConsumerRow row;
            row.decl = VectorDemand::DeclarationOf(c);
            const Placement pl = PlacementOf(c);
            row.module = "VP-V/modules/motion/estimation/VectorDemand";
            row.unit = pl.unit;
            row.order = pl.order;
            row.family = pl.family;
            row.kind = own;
            row.fallbackId = "product.vector_demand";
            r.push_back(row);
        }
        // Camera consumers: ids and reads from CameraProducts.h
        for (int i = 0; i < CameraLink::kCameraConsumerCount; ++i) {
            const CameraLink::CameraConsumer c = CameraLink::ConsumerAt(i);
            ConsumerRow row;
            row.decl = CameraLink::DeclarationOf(c);
            const Placement pl = PlacementOf(c);
            row.module = "VP-V/modules/motion/estimation/global_model";
            row.unit = pl.unit;
            row.order = pl.order;
            row.family = pl.family;
            row.kind = own;
            row.fallbackId = "product.camera";
            r.push_back(row);
        }
        return r;
    }();
    return rows;
}

inline std::vector<ConsumerDecl> AllConsumerDecls()
{
    std::vector<ConsumerDecl> out;
    for (const ConsumerRow& c : ConsumerRows())
        out.push_back(c.decl);
    return out;
}

// The readers of `product` (declared id), in pathway order, one per family (a component with a module row AND a
// vector-demand / camera row is one reader, at its earliest position). `lag0Only` keeps the reads of the SAME frame:
// only those can fill the frame's slot live. `active` (consumer ids) restricts to the consumers that are on; null = all.
inline std::vector<const ConsumerRow*> ConsumersOf(ProductId product, bool lag0Only = false,
                                                   const std::vector<uint16_t>* active = nullptr)
{
    std::vector<const ConsumerRow*> out;
    for (const ConsumerRow& c : ConsumerRows()) {
        if (active) {
            bool on = false;
            for (uint16_t id : *active)
                on = on || id == c.decl.id;
            if (!on)
                continue;
        }
        bool reads = false;
        for (size_t k = 0; k < c.decl.count; ++k)
            reads = reads || (c.decl.consumes[k].product == product && (!lag0Only || c.decl.consumes[k].lag == 0));
        if (!reads)
            continue;
        bool merged = false;
        for (const ConsumerRow*& o : out)
            if (std::string(o->family) == c.family) {
                if (PathwayKey(c) < PathwayKey(*o))
                    o = &c;
                merged = true;
                break;
            }
        if (!merged)
            out.push_back(&c);
    }
    for (size_t i = 1; i < out.size(); ++i)
        for (size_t j = i; j > 0 && PathwayKey(*out[j]) < PathwayKey(*out[j - 1]); --j)
            std::swap(out[j], out[j - 1]);
    return out;
}

// The consumer that fills `product` LIVE when the lane did not make it: the first lag-0 reader in pathway order among
// the ACTIVE consumers (a lag >= 1 reader needs the product already made, so it can never be the filler). nullptr when
// no active consumer reads it.
inline const ConsumerRow* LiveFillOwner(ProductId product, const std::vector<uint16_t>* active = nullptr)
{
    const std::vector<const ConsumerRow*> c = ConsumersOf(product, true, active);
    return c.empty() ? nullptr : c.front();
}

// Reader version check: a reader built for unit version `readerExpected` of `product` against a catalog that expects
// another one engages a visible fallback (never a silent mix of two laws). True when they agree (or the unit has no
// version yet: catalog 0). A reader that declares 0 against a versioned unit is a mismatch (it was not built against
// any version).
inline bool VerifyUnit(ProductId product, uint32_t readerExpected, FallbackCatalog::Decision* out)
{
    const ProductRow* row = RowOf(product);
    if (!row) {
        if (out)
            *out = FallbackCatalog::Engaged("product.unit_version", "unknown product " + std::to_string(unsigned(product)));
        return false;
    }
    if (row->unitVersion == 0 || row->unitVersion == readerExpected) {
        if (out)
            *out = FallbackCatalog::Released("product.unit_version");
        return true;
    }
    if (out)
        *out = FallbackCatalog::ProductFallback("product.unit_version", std::string(row->unit) + " makes unit version "
                                                    + std::to_string(row->unitVersion) + ", the reader expects "
                                                    + std::to_string(readerExpected));
    return false;
}

// ---- VRAM ----------------------------------------------------------------------------------------------------

struct TierBytes {
    uint64_t ring[3] = { 0, 0, 0 };   // lane ring buffers per tier T0/T1/T2
    uint64_t live[3] = { 0, 0, 0 };   // live pools per tier
    uint64_t Total(int t) const { return ring[t] + live[t]; }
    uint64_t Total() const { return Total(0) + Total(1) + Total(2); }
};

inline TierBytes VramByTier(const Graph& g)
{
    TierBytes t;
    for (const Node& n : g.nodes) {
        const int i = int(n.tier);
        t.ring[i] += n.ringBytes;
        t.live[i] += n.liveBytes;
    }
    return t;
}

// The graph with EVERY consumer active (the worst case the catalog describes).
inline Graph FullGraph(const GraphSettings& gs, const Registry& reg = SkeletonProducts())
{
    return BuildGraph(reg, AllConsumerDecls(), gs);
}

// ---- generated document --------------------------------------------------------------------------------------

inline std::string MegaBytes(uint64_t bytes)
{
    const uint64_t tenths = (bytes * 10u + 500000u) / 1000000u;   // decimal MB, one decimal
    return std::to_string(tenths / 10u) + "." + std::to_string(tenths % 10u);
}

inline const char* Token(Residency r) { return r == Residency::Vram ? "vram" : "system RAM"; }

constexpr const char* kDocBegin = "<!-- BEGIN GENERATED product-graph -->";
constexpr const char* kDocEnd = "<!-- END GENERATED product-graph -->";

// The text between the markers of docs/design/product-graph.md (markers included). Deterministic.
inline std::string RenderDoc()
{
    const Registry reg = SkeletonProducts();
    const Dims dims[3] = { { 1920, 1080 }, { 3840, 2160 }, { 7680, 4320 } };
    std::string s;
    s += std::string(kDocBegin) + "\n\n";
    s += "Generated by `ProductCatalogTests` from `core/ProductCatalog.h` and `core/Products.h`. Do not edit by hand: "
         "run the test with `VP_WRITE_PRODUCT_GRAPH_DOC=1` to rewrite it. Sizes are decimal MB for ONE slot "
         "(declared sizes, not measured).\n\n";
    s += "### Products\n\n";
    s += "| id | token | unit (version) | backend | lane producer | live-fill owner | consumers | stamps | tier | residency | 1080p MB | 4K MB | 8K MB |\n";
    s += "|---|---|---|---|---|---|---|---|---|---|---|---|---|\n";
    for (const ProductDecl& d : reg.All()) {
        const ProductRow* row = RowOf(d.id);
        const std::vector<const ConsumerRow*> cs = ConsumersOf(d.id);
        const ConsumerRow* owner = LiveFillOwner(d.id);
        std::string consumers;
        for (const ConsumerRow* c : cs)
            consumers += (consumers.empty() ? "" : ", ") + std::string(c->family);
        std::string stamps;
        for (int b = 0; b < int(VpIndicator::kCounterCount); ++b)
            if ((d.reads & (VpIndicator::CounterMask(1u) << unsigned(b))) != 0) {
                static const char* const kNames[] = { "full", "continuity", "timing", "rebaseH", "rebaseV", "graph",
                                                      "settings.motion", "settings.rebase", "settings.analysis", "settings.output" };
                stamps += (stamps.empty() ? "" : ",") + std::string(kNames[b]);
            }
        s += "| " + std::to_string(unsigned(d.id)) + " | " + d.token + " | " + (row ? row->unit : "?") + " (v"
             + std::to_string(row ? row->unitVersion : 0u) + ") | " + Token(d.backend) + " | "
             + ((row && row->laneProducer[0]) ? row->laneProducer : "none") + " | "
             + (owner ? owner->family : "none") + " | " + (consumers.empty() ? "none" : consumers) + " | "
             + stamps + " | " + Token(d.tier) + " | " + Token(d.residency);
        for (const Dims& dm : dims)
            s += " | " + MegaBytes(BytesOf(d.size, dm));
        s += " |\n";
    }
    s += "\n### Consumers\n\n";
    s += "| id | consumer | module | pathway | reads | fallback kind | fallback id |\n|---|---|---|---|---|---|---|\n";
    std::vector<const ConsumerRow*> sorted;
    for (const ConsumerRow& c : ConsumerRows())
        sorted.push_back(&c);
    for (size_t i = 1; i < sorted.size(); ++i)
        for (size_t j = i; j > 0 && PathwayKey(*sorted[j]) < PathwayKey(*sorted[j - 1]); --j)
            std::swap(sorted[j], sorted[j - 1]);
    for (const ConsumerRow* c : sorted) {
        std::string reads;
        for (size_t k = 0; k < c->decl.count; ++k) {
            const ProductId p = c->decl.consumes[k].product;
            reads += (reads.empty() ? "" : ", ") + std::string(reg.Has(p) ? reg.Get(p).token : "?")
                     + (c->decl.consumes[k].lag ? "@lag" + std::to_string(unsigned(c->decl.consumes[k].lag)) : "");
        }
        s += "| " + std::to_string(unsigned(c->decl.id)) + " | " + c->decl.name + " | " + c->module + " | "
             + Token(c->unit) + " " + std::to_string(unsigned(c->order)) + " | " + reads + " | " + Token(c->kind) + " | "
             + c->fallbackId + " |\n";
    }
    s += "\n### VRAM per tier (all consumers active, laneLead 3, render-ahead 0, optical-flow engine present)\n\n";
    s += "Ring buffers + live pools of every node, by tier (decimal MB).\n\n";
    s += "| frame | T0 ring | T0 live | T1 ring | T1 live | T2 ring | T2 live | total |\n|---|---|---|---|---|---|---|---|\n";
    for (const Dims& dm : dims) {
        GraphSettings gs;
        gs.dims = dm;
        gs.laneLead = 3;
        gs.queueDepth = 6;
        gs.lookAheadWalk = 2;
        const Graph g = FullGraph(gs, reg);
        const TierBytes t = VramByTier(g);
        s += "| " + std::to_string(dm.width) + "x" + std::to_string(dm.height);
        for (int i = 0; i < 3; ++i)
            s += " | " + MegaBytes(t.ring[i]) + " | " + MegaBytes(t.live[i]);
        s += " | " + MegaBytes(t.Total()) + " |\n";
    }
    s += "\n" + std::string(kDocEnd);
    return s;
}

} // namespace Catalog
} // namespace VpSkeleton
