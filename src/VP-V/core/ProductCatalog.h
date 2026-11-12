// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "FallbackCatalog.h"
#include "PassTimerInbox.h"
#include "ProductGraph.h"
#include "Products.h"
#include "VramGovernor.h"

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
constexpr uint32_t kExpectedTemporalDetailUnit = 2;   // doc 72: ids 16-18 redefined (td.blocks / td.memory / td.anchor_pass), TemporalDetail v2
constexpr uint32_t kExpectedLightLevelUnit = 3;
constexpr uint32_t kExpectedVectorUnit = 1;
constexpr uint32_t kExpectedStatsUnit = 1;
constexpr uint32_t kExpectedGrainSplitUnit = 2;
constexpr uint32_t kExpectedRebaseUnit = 1;
constexpr uint32_t kExpectedHalfLumaUnit = 1;
constexpr uint32_t kExpectedMonitorUnit = 1;
constexpr uint32_t kExpectedSourceIdentityUnit = 1;
constexpr uint32_t kExpectedSourceGeometryUnit = 1;   // SourceGeometry (P14g source_geometry, doc 78 section 6): input/source_analysis/identity/SourceGeometryProduct.h
// The temporal-detail engine products (doc 72; repair/temporal_detail/products states the same numbers in TdProducts and a test
// compares them): U13 CodingEvidence, QualityPattern, U15 FieldRefine and U19 TdMapsView at 1.
constexpr uint32_t kExpectedCodingEvidenceUnit = 1;
constexpr uint32_t kExpectedQualityPatternUnit = 1;
constexpr uint32_t kExpectedFieldRefineUnit = 1;
constexpr uint32_t kExpectedTdMapsViewUnit = 1;

// Doc 83 MV4: SERVICE products. The long-pair field is not a per-frame ring product (its pair distance is the measured GOP length and the
// service is asynchronous), so it has no id in the per-frame registry (adding one would move kSkeletonProductEnd and the live-fill owner
// rule, which every per-frame product must satisfy). It is CATALOGUED here instead, with its three cost routes, before the service is wired:
// the VRAM user it declares to the governor, the pass-timer row it posts, and the visible fallback entry for its absence.
struct ServiceProductRow {
    const char* token;           // the product's name in the status text and the ledger
    const char* unitFolder;      // the owning module folder
    uint32_t expectedUnit;       // the unit version
    uint16_t vramUser;           // the governor user its textures are declared under (VramUsers.h)
    const char* passTimerRow;    // the row its jobs post to PassTimers::Inbox::Motion()
    Tier tier;                   // T2: optional, shed first
    const char* fallbackId;      // the FallbackCatalog entry id raised while it is absent (VpMotion::kLongPairFallbackId)
    const char* note;
};
constexpr uint32_t kExpectedLongPairUnit = 1;
inline const std::vector<ServiceProductRow>& ServiceProductRows()
{
    static const std::vector<ServiceProductRow> rows = {
        { "long_pair_field", "motion/estimation/longpair", kExpectedLongPairUnit, kUserLongPair, PassTimers::Rows::kLongPairNvof, Tier::T2,
          "vector_engine_long_pair", "async I-to-I long-range flow (hinted NVOF session or the seeded search); unwired; optional, absent = visible" },
    };
    return rows;
}

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
        // P10c: RESERVED, NO PRODUCER (C17, doc 93 section 2 D4): declared, never registered as a lane job (modules/control/skeleton/SkeletonModule.cpp registers P10/P11/P12
        // only) and never live-filled; made real by C10. A reader of P10c finds it absent and takes its classical form (visible).
        { kP10cGrainSplitChroma, "repair/grain_split", kExpectedGrainSplitUnit, "reserved (no producer: built with C10)" },
        { kP11RegionBlockStats, "input/source_analysis/regions", kExpectedRegionUnit, "region block statistics pass" },
        { kP12LightLevel, "input/source_analysis/light_level", kExpectedLightLevelUnit, "light level pass" },
        { kP13SourceRing, "input/capture (queue + early upload)", 0, "" },
        { kTdBlocks, "repair/temporal_detail", kExpectedTemporalDetailUnit, "reserved (engine: reset 2)" },
        { kTdMemory, "repair/temporal_detail", kExpectedTemporalDetailUnit, "" },
        { kTdAnchorPass, "repair/temporal_detail", kExpectedTemporalDetailUnit, "reserved (engine: reset 2)" },
        // P14 is a HELD-STATE product: the monitor worker publishes it on each consensus evaluation, no lane producer, no live fill.
        { kSourceIdentity, "input/source_analysis/identity", kExpectedSourceIdentityUnit, "" },
        // Doc 72: p7g (registered, lane producer = the camera worker) and the engine products. No producer for the engine ones in reset 1
        // (a placeholder publishes "absent"), so every consumer's read path finds them absent and takes its classical form visibly.
        { kP7gCameraGuide, "motion/estimation/global_model", kExpectedCameraUnit, "camera guide (camera worker)" },
        { kCodingEvidence, "input/source_analysis/regions", kExpectedCodingEvidenceUnit, "reserved (fused with the region block statistics pass)" },
        { kQualityPattern, "input/source_analysis/identity/quality_pattern", kExpectedQualityPatternUnit, "reserved (lane CPU job)" },
        { kQualityPatternCausal, "input/source_analysis/identity/quality_pattern", kExpectedQualityPatternUnit, "reserved (lane CPU job)" },
        { kRefinedField, "motion/estimation/refine", kExpectedFieldRefineUnit, "reserved (field refine pass after P3)" },
        { kTdAnchorRefs, "repair/temporal_detail", kExpectedTemporalDetailUnit, "reserved (engine: reset 2)" },
        { kTdDelta, "repair/temporal_detail", kExpectedTemporalDetailUnit, "reserved (engine: reset 2)" },
        { kTdMaps, "repair/temporal_detail", kExpectedTdMapsViewUnit, "reserved (engine: reset 2)" },
        { kTdTrailSummary, "repair/temporal_detail", kExpectedTemporalDetailUnit, "reserved (engine: reset 2)" },
        // Wave S2: P14g source_geometry is HELD STATE like P14: the source-resolution side (single writer) publishes it on each evaluation, no lane
        // producer, no live fill; its readers take the latest sealed record (lag 1) and fall back visibly when it is absent or stale.
        { kSourceGeometry, "input/source_analysis/identity", kExpectedSourceGeometryUnit, "" },
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
    // More reads were named than kMaxConsumes holds: the extra reads were NOT recorded. Release builds have no assert, so this is a
    // visible flag: ValidateConsumerRows() rejects the catalog with the row's name and the graph build rejects the decl (same flag).
    bool overflow = false;
};

// Sorting key: pathway unit first, then render order.
inline uint32_t PathwayKey(const ConsumerRow& c) { return uint32_t(c.unit) * 10000u + c.order; }

constexpr uint16_t kCatalogConsumerIdBase = 0x0300;

inline ConsumerRow MakeRow(uint16_t index, const char* name, const char* module, PathwayUnit unit, uint16_t order,
                           std::initializer_list<ProductId> reads, FallbackKind kind, const char* fallbackId,
                           bool holdsSpan = false, std::initializer_list<ProductId> heldReads = {})
{
    ConsumerRow r;
    r.decl.id = uint16_t(kCatalogConsumerIdBase + index);
    r.decl.name = name;
    r.family = name;
    assert(reads.size() + heldReads.size() <= kMaxConsumes);   // Debug builds stop here; Release flags it (ConsumerRow::overflow)
    r.overflow = reads.size() + heldReads.size() > kMaxConsumes;
    for (ProductId p : reads)
        AddConsume(r.decl, p);
    // Held-state reads (P14 source identity): the latest sealed record, i.e. a lag >= 1 read, never a live fill.
    for (ProductId p : heldReads)
        AddConsume(r.decl, p, 1);
    r.overflow = r.overflow || r.decl.overflow;
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
    case C::CameraFit: return { PathwayUnit::Motion, 26, "camera measurement" };
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
        r.push_back(MakeRow(13, "matcher / motion warp", "VP-V/modules/motion/estimation/matcher/DejudderWarp", PathwayUnit::Motion,
                            20, { kP1HalfLuma, kP2VectorPair, kP4SourceStats }, own, "product.motion_warp"));
        r.push_back(MakeRow(9, "interpolator / hiccup repair", "VP-V/modules/motion/consumers", PathwayUnit::Motion, 40,
                            { kP3AnalysisField, kP10GrainSplit }, own, "product.interpolator"));
        // Repair
        r.push_back(MakeRow(10, "temporal detail", "VP-V/modules/repair/temporal_detail", PathwayUnit::Repair, 10,
                            { kP10GrainSplit, kP11RegionBlockStats, kP13SourceRing, kTdAnalysis, kTdRefstore, kTdAdmission },
                            lower, "product.temporal_detail"));
        r.push_back(MakeRow(0, "detail pulse", "VP-V/modules/repair/detail_pulse", PathwayUnit::Repair, 20,
                            { kP10GrainSplit, kP11RegionBlockStats, kP4SourceStats }, own, "product.detail_pulse", false,
                            { kSourceIdentity }));
        r.push_back(MakeRow(1, "texture restore", "VP-V/modules/repair/texture_restore", PathwayUnit::Repair, 30,
                            { kP10GrainSplit, kP11RegionBlockStats, kP4SourceStats }, own, "product.texture_restore", false,
                            { kSourceIdentity }));
        r.push_back(MakeRow(2, "debander (with hold)", "VP-V/modules/repair/deband", PathwayUnit::Repair, 40,
                            { kP10GrainSplit, kP11RegionBlockStats, kP12LightLevel, kP3AnalysisField }, own,
                            "product.debander", true, { kSourceIdentity }));
        // Contrast recovery gates itself off on a clean high-bitrate source (get out of the way): it reads only the held record.
        r.push_back(MakeRow(18, "contrast recovery need", "VP-V/modules/repair", PathwayUnit::Repair, 55,
                            {}, own, "product.contrast_recovery", false, { kSourceIdentity }));
        r.push_back(MakeRow(16, "light-level users", "VP-V/modules/repair", PathwayUnit::Repair, 50,
                            { kP12LightLevel, kP11RegionBlockStats }, own, "product.light_level"));
        // Scaling
        r.push_back(MakeRow(11, "chroma from luma", "VP-V/modules/scaling/chroma_upscale", PathwayUnit::Scaling, 10,
                            { kP10GrainSplit, kP10cGrainSplitChroma, kP13SourceRing }, lower, "product.chroma_cfl", false,
                            { kSourceGeometry }));
        r.push_back(MakeRow(3, "adaptive downscale (grain map)", "VP-V/modules/scaling/adaptive_downscale",
                            PathwayUnit::Scaling, 20, { kP11RegionBlockStats, kP10GrainSplit, kP4SourceStats }, own,
                            "product.adaptive_downscale", false, { kSourceIdentity, kSourceGeometry }));
        // Drive and output
        r.push_back(MakeRow(5, "LED look-ahead", "VP-V/modules/drive/led_measure", PathwayUnit::Drive, 10, { kP4SourceStats },
                            own, "product.led_lookahead"));
        r.push_back(MakeRow(17, "dither need (source quality)", "VP-V/modules/output/dither", PathwayUnit::Output, 15,
                            {}, own, "product.dither_need", false, { kSourceIdentity }));
        r.push_back(MakeRow(4, "sigma-delta (analysis field mapped)", "VP-V/modules/output/dither", PathwayUnit::Output, 10,
                            { kP3AnalysisField }, own, "product.sigma_delta", true));
        // Camera guidance (doc 56 / doc 72 item 1): reads p7g (registered now); the first reader of the guide in the motion unit.
        {
            ConsumerRow row;
            row.decl.id = uint16_t(CameraLink::kCameraConsumerIdBase + 0x40);   // beside the 14 camera consumers (CameraGuideProduct.h)
            row.decl.name = "camera guidance";
            AddConsume(row.decl, kP7gCameraGuide);
            row.decl.hasReducedMode = true;   // refusal: the matcher runs today's full search (visible)
            row.module = "VP-V/modules/motion/estimation/global_model/link";
            row.unit = PathwayUnit::Motion;
            row.order = 24;
            row.kind = own;
            row.fallbackId = "product.camera";
            row.family = "camera guidance";
            r.push_back(row);
        }
        // The consumers' READ PATHS of the temporal-detail engine's products (doc 72 section 3). Each reads what it will use when the
        // engine exists (td.maps fields through TdMapsView), finds it absent in reset 1 and runs its classical form (byte-identical;
        // visible once the engine is requested), under its OWN existing fallback id. The sets equal TdProducts::NeededBy (a test compares).
        r.push_back(MakeRow(19, "adaptive scaling (temporal-detail read path)", "VP-V/modules/scaling/output/AdaptiveDownscaleStage",
                            PathwayUnit::Scaling, 21, { kTdMaps, kRefinedField }, own, "product.adaptive_downscale"));
        r.push_back(MakeRow(20, "dither need (temporal-detail read path)", "VP-V/modules/output/dither/DitherNeed",
                            PathwayUnit::Output, 16, { kTdMaps, kQualityPattern }, own, "product.dither_need"));
        r.push_back(MakeRow(21, "chroma upscale (temporal-detail read path)", "VP-V/modules/scaling/chroma_upscale/basic/BasicChroma",
                            PathwayUnit::Scaling, 11, { kTdMaps, kTdDelta, kTdMemory, kRefinedField }, own, "product.chroma_cfl"));
        r.push_back(MakeRow(22, "sigma-delta (temporal-detail read path)", "VP-V/modules/output/dither/SigmaDeltaVectors",
                            PathwayUnit::Output, 13, { kTdMaps, kRefinedField }, own, "product.sigma_delta"));
        r.push_back(MakeRow(23, "debander (temporal-detail read path)", "VP-V/modules/repair/deband/VpDebander", PathwayUnit::Repair, 43,
                            { kTdMaps, kCodingEvidence }, own, "product.debander"));
        // The engine's side: two rows of ONE family ("temporal detail", the component of the module row at repair 10; one reader, at its
        // earliest position), so the engine is the live-fill owner of every engine product it makes. The module row (repair 10) reads
        // td.blocks / td.memory / td.anchor_pass.
        r.push_back(MakeRow(24, "temporal detail engine (reserved: precursors)", "VP-V/modules/repair/temporal_detail/products",
                            PathwayUnit::Repair, 13, { kCodingEvidence, kQualityPattern, kRefinedField, kTdAnchorRefs }, own,
                            "product.td_engine"));
        r.back().family = "temporal detail";
        r.push_back(MakeRow(25, "temporal detail engine (reserved: outputs)", "VP-V/modules/repair/temporal_detail/products",
                            PathwayUnit::Repair, 14, { kTdDelta, kTdMaps, kTdTrailSummary }, own, "product.td_engine"));
        r.back().family = "temporal detail";
        // P14g source_geometry (wave S2, doc 78 section 6): the provisional readers inside the temporal-detail family are quality_pattern and
        // coding_evidence (their grid placement and sharpening discount) and temporal detail itself (the latched coded raster and the gens as a
        // discontinuity); the adaptive scaler and the chroma stage read it through their own rows above (held reads, lag 1). One more row for
        // status. HELD STATE: every read is lag 1 (the latest sealed record), so none of them can fill it live; absent / stale / another unit
        // version = the delivered raster and each reader's classical form, visibly (SourceGeometry::Read, "product.source_geometry").
        r.push_back(MakeRow(26, "temporal detail (source geometry read path)", "VP-V/modules/repair/temporal_detail/products",
                            PathwayUnit::Repair, 15, {}, own, "product.source_geometry", false, { kSourceGeometry }));
        r.back().family = "temporal detail";
        r.push_back(MakeRow(27, "source geometry status", "VP-V/core/StatusText", PathwayUnit::Output, 95, {}, own,
                            "product.source_geometry", false, { kSourceGeometry }));
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

// ---- declared vs actually read (C17, doc 93 W0-4 / section 2) -------------------------------------------------
//
// A declaration (ProductDecl::AddInput, ConsumerRow reads) says what a product / consumer is WIRED to read; it is not a proof that the
// code reads it today. The flags below are ADDITIVE: they do not change a declaration, the graph order, the live-fill owner or a slot.
// They say, per declared input, where the declaration and the code differ today, so the conformance checker and the lane graph can be
// trusted. An input without a flag is read as declared. A test pins the exact flag set: a new unread declaration needs a flag here.
//   NotYetConsumed : declared, kept, and not yet read (the code takes a private copy); made true by the named work item
//   NotRead        : a consumer row declares the read, the code reads none of it today (private implementation); made true by the item
//   ViewIsActual   : the declared input is read through another VIEW of the data (named in the note), not the declared product itself
//   ExternalView   : a read the code does today through an external accessor and the declaration does NOT carry (shown, not declared)
//   Reserved       : the product is declared and has no producer (no lane job, no live fill); every reader finds it absent (visible)
enum class FlagKind : uint8_t { NotYetConsumed, NotRead, ViewIsActual, ExternalView, Reserved };

inline const char* Token(FlagKind k)
{
    switch (k) {
    case FlagKind::NotYetConsumed: return "declared, not yet consumed";
    case FlagKind::NotRead: return "declared, not read";
    case FlagKind::ViewIsActual: return "actual input is a view";
    case FlagKind::ExternalView: return "external view (not declared)";
    case FlagKind::Reserved: return "reserved, no producer";
    }
    return "?";
}

// A flag on a PRODUCT's declaration (Registry): `input` = the declared input it concerns (kNoProduct = the whole product).
struct ProductFlag {
    ProductId product;
    ProductId input;
    FlagKind kind;
    const char* item;   // the work item that makes the declaration true / the register row (doc 93)
    const char* note;
};

// A flag on a CONSUMER row's declared read (the module row's id = kCatalogConsumerIdBase + index).
struct ConsumerFlag {
    uint16_t consumerId;
    ProductId input;
    FlagKind kind;
    const char* item;
    const char* note;
};

inline const std::vector<ProductFlag>& ProductFlags()
{
    static const std::vector<ProductFlag> flags = {
        // D1: P2 / P2m / P3 keep their P1 declaration; they read their own half-luma copies until C9 makes it true (doc 93 section 2 D1).
        { kP2VectorPair, kP1HalfLuma, FlagKind::NotYetConsumed, "C9", "reads its own half-luma copy (producer HalfLuma8); core/Products.h P2 declaration kept" },
        { kP2mMatcherPair, kP1HalfLuma, FlagKind::NotYetConsumed, "C9", "reads the matcher's own Downsample copies; core/Products.h P2m declaration kept" },
        { kP3AnalysisField, kP1HalfLuma, FlagKind::NotYetConsumed, "C9", "reads the matcher's own inner pyramids; core/Products.h P3 declaration kept" },
        // D2: P10 declares P13 and reads the lane's extended TAP (the one GPU luma of the frame), not the ring planes.
        { kP10GrainSplit, kP13SourceRing, FlagKind::ViewIsActual, "C9b",
          "actual input = the lane tap view (modules/input/source_analysis/lane/LaneGpuProducts.cpp UploadTapRows: SourceFrameView luma uploaded to the tap texture)" },
        // D4: P10c has no producer.
        { kP10cGrainSplitChroma, kNoProduct, FlagKind::Reserved, "C10", "no lane job (modules/control/skeleton/SkeletonModule.cpp registers P10/P11/P12 only), no live fill" },
        // P14g: its producing evaluation takes its pixels from the source-frame tap sink, P5 is the monitor snapshot it declares.
        { kSourceGeometry, kP5MonitorTiles, FlagKind::ViewIsActual, "C9b",
          "pixel evidence = the SourceFrameTap view (modules/input/source_analysis/identity/chain/ChainEstimator.cpp UpdateTap); P5 stays the declared monitor snapshot" },
        // D3: P7 reads P2 through IVectorEngine::FlowOf (pair id recorded) and declares only P4 (by design).
        { kP7CameraFit, kP2VectorPair, FlagKind::ExternalView, "C3", "external view: P2 via FlowOf (modules/motion/estimation/global_model/link/CameraExtension.cpp ahead fit); pair id recorded, not a declared input" },
    };
    return flags;
}

inline const std::vector<ConsumerFlag>& ConsumerFlags()
{
    static const std::vector<ConsumerFlag> flags = {
        // D4: DetailPulse / TextureRestore are catalogued as P10 / P11 / P4 readers but read none (modules/repair/texture_restore/DetailPulse.cpp, TextureRestore.cpp:
        // private bands / tiles and private cut-fade logic, host scalar params); the declarations stay, the code is made to read them by C10 / D10
        // (doc 93 s2 D10 reader switch, rows 6/7; C7 landed without switching them, AUDIT-R2 F1).
        { uint16_t(kCatalogConsumerIdBase + 0), kP10GrainSplit, FlagKind::NotRead, "C10", "private bands (DetailPulse.h)" },
        { uint16_t(kCatalogConsumerIdBase + 0), kP11RegionBlockStats, FlagKind::NotRead, "C10", "private 32-px tiles" },
        { uint16_t(kCatalogConsumerIdBase + 0), kP4SourceStats, FlagKind::NotRead, "D10", "private cut / fade logic" },
        { uint16_t(kCatalogConsumerIdBase + 1), kP10GrainSplit, FlagKind::NotRead, "C10", "private bands (TextureRestore.h)" },
        { uint16_t(kCatalogConsumerIdBase + 1), kP11RegionBlockStats, FlagKind::NotRead, "C10", "private 16-px tiles" },
        { uint16_t(kCatalogConsumerIdBase + 1), kP4SourceStats, FlagKind::NotRead, "D10", "private cut / fade logic" },
        // AUDIT-R2 F1 (B3-Z Z2): the optical-flow producer, the matcher and source identification print their declared reads as actual
        // otherwise. The producer and the matcher keep private half-luma copies (the P2 / P2m -> P1 product flags above); the matcher's M3
        // histogram is the shared unit U6 computed privately in its trust pass, not the P4 product; identification's chain reads the
        // source-frame tap (the P14g view above), and no P4 read exists in the identity tree.
        { uint16_t(kCatalogConsumerIdBase + 14), kP1HalfLuma, FlagKind::NotRead, "C9", "own HalfLuma8 copy (MotionAheadProducer.h HalfLuma8)" },
        { uint16_t(kCatalogConsumerIdBase + 13), kP1HalfLuma, FlagKind::NotRead, "C9", "own Downsample copies" },
        { uint16_t(kCatalogConsumerIdBase + 13), kP4SourceStats, FlagKind::NotRead, "D8",
          "M3 histogram computed privately (matcher/DejudderWarp.cpp FrameTrustHeader / FrameTrustBody, SourceStats::CoarseHist*)" },
        { uint16_t(kCatalogConsumerIdBase + 8), kP5MonitorTiles, FlagKind::ViewIsActual, "C9b",
          "pixel evidence = the SourceFrameTap view (modules/input/source_analysis/identity/chain/ChainEstimator.cpp UpdateTap)" },
        { uint16_t(kCatalogConsumerIdBase + 8), kP4SourceStats, FlagKind::NotRead, "D6-D8", "no P4 read in the identity tree" },
        // B3-Z Z-DECL (lead 2026-11-11, no declaration change): the render-time readers of the matcher field. The synthesis consumers and the
        // detail pulse / texture restore / adaptive downscale rows read the LIVE matcher field (dejudder->Field()) at render and do not
        // declare P3; the lane P3 declarations land with the matcher-ahead ring (B3-Z Z1-Z3) and lane-time refinement (C9b).
        { uint16_t(VectorDemand::kConsumerIdBase + int(VectorDemand::Consumer::DejudderExtras)), kP3AnalysisField, FlagKind::ExternalView, "C9b",
          "read at render: live matcher field (LibplaceboVideoRenderer.cpp warpField, de-judder extras loop)" },
        { uint16_t(VectorDemand::kConsumerIdBase + int(VectorDemand::Consumer::Interpolator)), kP3AnalysisField, FlagKind::ExternalView, "C9b",
          "read at render: live matcher field (Interpolator::Motion.field = warpField)" },
        { uint16_t(VectorDemand::kConsumerIdBase + int(VectorDemand::Consumer::HiccupRepair)), kP3AnalysisField, FlagKind::ExternalView, "C9b",
          "read at render: live matcher field (gap pairs through the interpolator)" },
        { uint16_t(VectorDemand::kConsumerIdBase + int(VectorDemand::Consumer::OcclusionMask)), kP3AnalysisField, FlagKind::ExternalView, "C9b",
          "read at render: the matcher's occlusion slots (dejudder->Occlusion(slot)) of its live estimate" },
        { uint16_t(kCatalogConsumerIdBase + 0), kP3AnalysisField, FlagKind::ExternalView, "C9b",
          "read at render: live matcher field (DetailPulse::Params.field = dejudder->Field())" },
        { uint16_t(kCatalogConsumerIdBase + 1), kP3AnalysisField, FlagKind::ExternalView, "C9b",
          "read at render: live matcher field (TextureRestore::Params.field, one-period field only)" },
        { uint16_t(kCatalogConsumerIdBase + 3), kP3AnalysisField, FlagKind::ExternalView, "C9b",
          "read at render: live matcher field (AdaptiveDownscaleStage::MotionSource.field)" },
        // The camera fit declares P3 and reads the matcher field through IVectorEngine::SeedFieldOf, not the lane slot; the real
        // declaration lands with C2-cam.
        { uint16_t(VectorDemand::kConsumerIdBase + int(VectorDemand::Consumer::CameraFit)), kP3AnalysisField, FlagKind::ViewIsActual, "C2-cam",
          "actual input = IVectorEngine::SeedFieldOf (global_model/link/CameraExtension.cpp, CameraLinkCore.cpp), not the lane slot" },
    };
    return flags;
}

inline const ConsumerFlag* ConsumerFlagOf(uint16_t consumerId, ProductId input)
{
    for (const ConsumerFlag& f : ConsumerFlags())
        if (f.consumerId == consumerId && f.input == input)
            return &f;
    return nullptr;
}

// Consumer rows with no renderer call site today (AUDIT-R2 F1; doc 91 G correction): every declared read is "not read" until the
// row is wired. The temporal-detail family: the module row, its vector-demand and camera rows and the engine / read-path rows.
struct UnwiredConsumer {
    uint16_t consumerId;
    const char* item;
    const char* note;
};

inline const std::vector<UnwiredConsumer>& UnwiredConsumers()
{
    static const std::vector<UnwiredConsumer> rows = {
        { uint16_t(kCatalogConsumerIdBase + 10), "TD", "no renderer call site" },
        { uint16_t(VectorDemand::kConsumerIdBase + int(VectorDemand::Consumer::TemporalDetail)), "TD", "no renderer call site" },
        { uint16_t(CameraLink::kCameraConsumerIdBase + int(CameraLink::CameraConsumer::TemporalDetail)), "TD", "no renderer call site" },
        { uint16_t(kCatalogConsumerIdBase + 24), "TD", "no renderer call site (engine reserved)" },
        { uint16_t(kCatalogConsumerIdBase + 25), "TD", "no renderer call site (engine reserved)" },
        { uint16_t(kCatalogConsumerIdBase + 26), "TD", "no renderer call site" },
    };
    return rows;
}

inline const UnwiredConsumer* UnwiredConsumerOf(uint16_t consumerId)
{
    for (const UnwiredConsumer& u : UnwiredConsumers())
        if (u.consumerId == consumerId)
            return &u;
    return nullptr;
}

// A product with no producer today: a Reserved product flag, a lane producer marked "reserved", or one of the products whose
// producer is not built (P13: EU-P13; td.memory: the TD engine). A read of it is never an actual read (AUDIT-R2 F1).
inline const char* NoProducerReason(ProductId p)
{
    for (const ProductFlag& f : ProductFlags())
        if (f.product == p && f.input == kNoProduct && f.kind == FlagKind::Reserved)
            return "reserved, no producer";
    const ProductRow* row = RowOf(p);
    if (row && std::string(row->laneProducer).find("reserved") == 0)
        return "reserved, no producer";
    if (p == kP13SourceRing)
        return "no producer (EU-P13 not built)";
    if (p == kTdMemory)
        return "no producer (TD engine not built)";
    return nullptr;
}

// The reads a consumer row actually performs today: its declared reads minus the NotRead flags, the reads of products without a
// producer and every read of an unwired row, as the document prints them.
inline std::string ActualReads(const ConsumerRow& c, const Registry& reg)
{
    std::string reads, unread;
    const UnwiredConsumer* unwired = UnwiredConsumerOf(c.decl.id);
    for (size_t k = 0; k < c.decl.count; ++k) {
        const ProductId p = c.decl.consumes[k].product;
        const std::string name = std::string(reg.Has(p) ? reg.Get(p).token : "?");
        const ConsumerFlag* f = ConsumerFlagOf(c.decl.id, p);
        const char* noProducer = NoProducerReason(p);
        if (unwired)
            unread += (unread.empty() ? "" : "; ") + name + " (" + unwired->note + ", " + unwired->item + ")";
        else if (noProducer)
            unread += (unread.empty() ? "" : "; ") + name + " (" + noProducer + ")";
        else if (f && f->kind == FlagKind::NotRead)
            unread += (unread.empty() ? "" : "; ") + name + " (" + f->note + ", " + f->item + ")";
        else
            reads += (reads.empty() ? "" : ", ") + name + (c.decl.consumes[k].lag ? "@lag" + std::to_string(unsigned(c.decl.consumes[k].lag)) : "")
                     + (f && f->kind == FlagKind::ViewIsActual ? std::string(" (view, ") + f->item + ")" : std::string());
    }
    // Reads the code does through an external accessor that the row does NOT declare (shown, not declared).
    std::string external;
    for (const ConsumerFlag& f : ConsumerFlags())
        if (f.consumerId == c.decl.id && f.kind == FlagKind::ExternalView)
            external += (external.empty() ? "" : "; ") + std::string(reg.Has(f.input) ? reg.Get(f.input).token : "?") + " (" + f.item + ")";
    if (!unread.empty())
        reads += std::string(reads.empty() ? "" : "; ") + "not read: " + unread;
    if (!external.empty())
        reads += std::string(reads.empty() ? "" : "; ") + "external view: " + external;
    return reads;
}

// "" when no consumer row dropped a read, else the first row's name and why (Release-visible; a test pins it empty).
inline std::string ValidateConsumerRows()
{
    for (const ConsumerRow& c : ConsumerRows())
        if (c.overflow || c.decl.overflow || c.decl.count > kMaxConsumes)
            return std::string("consumer row \"") + c.decl.name + "\" names more reads than a consumer holds (" + std::to_string(kMaxConsumes) + ")";
    return std::string();
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

inline const char* Token(Residency r) { return r == Residency::Vram ? "vram" : r == Residency::Transient ? "transient (arena)" : "system RAM"; }

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
    s += "| id | consumer | module | pathway | reads (declared) | reads (actual today) | fallback kind | fallback id |\n|---|---|---|---|---|---|---|---|\n";
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
             + Token(c->unit) + " " + std::to_string(unsigned(c->order)) + " | " + reads + " | " + ActualReads(*c, reg) + " | "
             + Token(c->kind) + " | " + c->fallbackId + " |\n";
    }
    s += "\n### Declared vs actually read (additive flags)\n\n";
    s += "A declaration is what a product or consumer is wired to read, not proof the code reads it today. These flags do not change a "
         "declaration, the graph order or a slot; the exact set is pinned by `ProductCatalogTests`. An input without a flag is read as declared. "
         "The duplicate register is section 2 of the consolidation plan (doc 93); this table is its code side.\n\n";
    s += "| subject | input | flag | item | note |\n|---|---|---|---|---|\n";
    for (const ProductFlag& f : ProductFlags())
        s += "| product " + std::string(reg.Has(f.product) ? reg.Get(f.product).token : "?") + " | "
             + (f.input == kNoProduct ? std::string("(whole product)") : std::string(reg.Has(f.input) ? reg.Get(f.input).token : "?")) + " | "
             + Token(f.kind) + " | " + f.item + " | " + f.note + " |\n";
    for (const ConsumerFlag& f : ConsumerFlags()) {
        std::string who = "consumer " + std::to_string(unsigned(f.consumerId));
        for (const ConsumerRow& c : ConsumerRows())
            if (c.decl.id == f.consumerId)
                who = std::string("consumer ") + c.decl.name;
        s += "| " + who + " | " + std::string(reg.Has(f.input) ? reg.Get(f.input).token : "?") + " | " + Token(f.kind) + " | " + f.item + " | "
             + f.note + " |\n";
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
