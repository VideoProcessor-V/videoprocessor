// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// repair.temporal_detail / products: the CONTRACT table of the temporal-detail engine's products (doc 72, R1-W1-RES round 2; the
// engine itself is built in reset 2). core/Products.h declares the registry entries (ids appended after P14 in doc 72's order,
// ids 16-18 redefined as td.blocks / td.memory / td.anchor_pass, unit v2); core/ProductCatalog.h the catalog rows and the consumer
// read paths. This header is, per product, the part a registry entry cannot say:
//   * the fill rule (lane / live / reduced) and the unit that makes it, with its unit version,
//   * the BUDGET-GOVERNED STAGES of each large buffer: the registry declares the FLOOR (stage 0: the repair of the still / dark
//     regions plus the full-resolution mid band at the true source resolution, the least the budget grants); the VRAM / ms governor
//     adds the larger stages while headroom lasts, each visible, shed in reverse order. There are NO fixed per-GPU tiers,
//   * the INTENDED inputs. The framework holds 16 inputs per product (kMaxInputs) and 10 consumed products per consumer
//     (kMaxConsumes); the registry still declares only the first four inputs (reset-2 open item: short-queue substitutes for the
//     lead edges) and this table records the full list, which a test compares.
// Header-only, pure C++14, no Windows, no GPU. A change of any layout, range or fill rule bumps the unit version of the unit that
// makes the product AND the expectation in Catalog (the same commit); a reader built against another number takes its classical
// form visibly (Catalog::VerifyUnit, TdProductReads.h).
#pragma once

#include <VP-V/core/Products.h>
#include <VP-V/modules/repair/temporal_detail/shared/TdMapsView.h>

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <vector>

namespace TdProducts
{
	// Unit versions the core expects (the catalog states the same numbers; a test compares them).
	constexpr uint32_t kUnitVersion = 2;            // TemporalDetail v2: td.blocks, td.memory, td.anchor_pass, td.anchor_refs, td.delta, td.trail_summary
	constexpr uint32_t kCodingEvidenceUnit = 1;     // U13 CodingEvidence
	constexpr uint32_t kQualityPatternUnit = 1;     // QualityPattern (identity/quality_pattern)
	constexpr uint32_t kFieldRefineUnit = 1;        // U15 FieldRefine
	constexpr uint32_t kMapsViewUnit = TdMapsView::kUnitVersion;   // U19 TdMapsView (td.maps)

	using VpSkeleton::ProductId;

	// Anchors td.anchor_refs holds at its full stage: structural (the reference candidates of a hybrid codec: LT-A, LT-P, one ST), not
	// a frame count over time. R16 = 2 B/px per anchor.
	constexpr uint32_t kAnchorRefCount = 3;

	// The products of this table: ids 16-18 (redefined) and the engine products appended after p7g (doc 72 section 1.1).
	constexpr int kAppended = int(VpSkeleton::kTdTrailSummary) - int(VpSkeleton::kCodingEvidence) + 1;   // 8
	constexpr int kCount = kAppended + 3;                                                                  // + td.blocks, td.memory, td.anchor_pass

	// Dense index 0..kCount-1 (the three redefined ids first, then the appended ones); -1 for any other product.
	inline int IndexOfId(ProductId id)
	{
		if (id == VpSkeleton::kTdBlocks) return 0;
		if (id == VpSkeleton::kTdMemory) return 1;
		if (id == VpSkeleton::kTdAnchorPass) return 2;
		if (id >= VpSkeleton::kCodingEvidence && id <= VpSkeleton::kTdTrailSummary) return 3 + int(id) - int(VpSkeleton::kCodingEvidence);
		return -1;
	}
	inline int IndexOf(ProductId id) { return IndexOfId(id); }
	inline bool IsEngineProduct(ProductId id) { return IndexOfId(id) >= 0; }

	struct Input
	{
		ProductId product;
		uint8_t lag;
		uint8_t lead;
	};

	// One governed stage of a buffer: a stage is the WHOLE buffer at that level (not an increment).
	struct Stage
	{
		const char* name;
		VpSkeleton::SizeRule size;
	};

	struct Contract
	{
		ProductId id;
		const char* token;           // equals the registry token
		const char* unit;            // the unit that makes it (module path)
		uint32_t unitVersion;        // that unit's version the core expects
		const char* fillLane;        // "lane" rule
		const char* fillLive;        // "live" rule (who runs the same unit once when the lane missed)
		const char* fillReduced;     // the declared visible reduced form
		const char* layout;
		std::vector<Input> intended; // the intended inputs (the registry declares the first four of them today)
		std::vector<Stage> stages;   // governed stages, floor first; empty = not budget-governed (one size, the registry's)
	};

	inline VpSkeleton::SizeRule Rule(std::initializer_list<VpSkeleton::SizeTerm> terms, uint32_t fixedBytes = 0)
	{
		VpSkeleton::SizeRule r;
		r.fixedBytes = fixedBytes;
		for (const VpSkeleton::SizeTerm& t : terms)
			if (r.termCount < r.terms.size()) r.terms[r.termCount++] = t;
		return r;
	}

	inline const std::vector<Contract>& Contracts()
	{
		using namespace VpSkeleton;
		static const std::vector<Contract> rows = {
			{ kTdBlocks, "td.blocks", "repair/temporal_detail", kUnitVersion, "TD lane job, sequential (reads itself at lag 1)",
			  "TD apply runs the same job", "carry-forward: memory not advanced, previous delta warped by P3, weights decayed (visible \"temporal detail: lane late (carried)\")",
			  "per 16 px block ~28 B (state, origin slot, trail, per-band confidence, lighting layer, motion class, counters) + 8 px quadtree pool <= 25%",
			  { { kP10GrainSplit, 0, 0 }, { kP11RegionBlockStats, 0, 0 }, { kP13SourceRing, 0, 0 }, { kTdBlocks, 1, 0 },
			    { kP12LightLevel, 0, 0 }, { kP13SourceRing, 0, 2 }, { kP13SourceRing, 1, 0 }, { kQualityPattern, 0, 0 },
			    { kCodingEvidence, 0, 0 }, { kCodingEvidence, 1, 0 }, { kRefinedField, 0, 0 }, { kP9CameraSmoothed, 0, 0 },
			    { kP8CameraLabels, 0, 0 }, { kTdMemory, 1, 0 }, { kTdAnchorRefs, 1, 0 }, { kSourceIdentity, 1, 0 }, { kP7CameraFit, 0, 0 } },
			  {} },
			{ kTdMemory, "td.memory", "repair/temporal_detail", kUnitVersion, "TD job feedback at anchors + the anchor pass (no other writer)",
			  "none (no live fill)", "absent or shed: temporal detail runs level 3 / 4 or off, visibly",
			  "the whole store: slots LT-A, LT-P, ST, NA, FP; bands B0 at 1/4 per axis, B1 at FULL resolution; chroma at B0",
			  { { kTdBlocks, 0, 0 }, { kP10GrainSplit, 0, 0 }, { kP3AnalysisField, 0, 0 }, { kP13SourceRing, 0, 0 } },
			  { { "floor: still/dark repair + full-resolution mid band", Rule({ { 2, 55 }, { 1, 1 }, { 0, 2 } }) },
			    { "full: more slots, full-resolution band 1 everywhere", Rule({ { 2, 111 }, { 1, 5 }, { 0, 13 } }) } } },
			{ kTdAnchorPass, "td.anchor_pass", "repair/temporal_detail", kUnitVersion, "T2 gap job chunked <= 1 ms, triggered by quality_pattern anchors",
			  "none", "shed first: \"anchor repair: light only\"", "Transient arena, peak ~6 band-1 planes (a shared maximum, not a sum)",
			  { { kP13SourceRing, 0, 0 }, { kP10GrainSplit, 0, 0 }, { kP11RegionBlockStats, 0, 0 }, { kTdMemory, 1, 0 }, { kP13SourceRing, 0, 3 } }, {} },
			{ kCodingEvidence, "coding_evidence", "input/source_analysis/regions", kCodingEvidenceUnit, "regions: the block statistics pass (one fused dispatch, two outputs)",
			  "temporal detail (first lag-0 reader) runs the same unit", "absent: quality_pattern abstains, TD drops to level 3 / 4 visibly, deband / source ID use today's paths",
			  "per 8x8 block 8 B (hash32, blockiness u16, copy u8, phase u8) + 256 B per-frame summary; history 2",
			  { { kP10GrainSplit, 0, 0 }, { kP11RegionBlockStats, 0, 0 }, { kP13SourceRing, 0, 0 }, { kCodingEvidence, 1, 0 }, { kCodingEvidence, 2, 0 } }, {} },
			{ kQualityPattern, "quality_pattern", "input/source_analysis/identity/quality_pattern", kQualityPatternUnit, "lane CPU job when coding_evidence(N+2) is published",
			  "temporal detail runs the same pure function", "quality_pattern.causal when the effective lead < 2 (visible \"frame pattern: provisional\")",
			  "CPU POD ~64 B per unique source frame, history = kQualityPatternHistorySeconds of the measured source rate (system RAM); NOT part of P14's held state",
			  { { kCodingEvidence, 0, 0 }, { kCodingEvidence, 0, 1 }, { kCodingEvidence, 0, 2 }, { kQualityPattern, 1, 0 } }, {} },
			{ kQualityPatternCausal, "quality_pattern.causal", "input/source_analysis/identity/quality_pattern", kQualityPatternUnit, "same unit, causal (no lead)",
			  "same", "n/a (it is the reduced form)", "same POD as quality_pattern", { { kCodingEvidence, 0, 0 }, { kQualityPatternCausal, 1, 0 } }, {} },
			{ kRefinedField, "refined_field", "motion/estimation/refine", kFieldRefineUnit, "field refine pass right after P3; never starts the matcher",
			  "temporal detail runs the same unit", "the unrefined P3 (visible \"motion: refinement late\")",
			  "per 8x8 block 8 B (vector RG16F, confidence u8, flags u8); history 1. NO per-pixel offsets are stored (derived on the fly)",
			  { { kP3AnalysisField, 0, 0 }, { kP1HalfLuma, 0, 0 }, { kP1HalfLuma, 1, 0 }, { kP8CameraLabels, 0, 0 } }, {} },
			{ kTdAnchorRefs, "td.anchor_refs", "repair/temporal_detail", kUnitVersion, "TD job copy at anchor detection",
			  "none", "absent: static / camera mode uses coding_evidence hashes and the P13 lag-1 view",
			  // The "3" is STRUCTURAL, not a rate: the anchors a hybrid codec can reference at once (the long-term I, the previous I and one
			  // short-term P, the LT-A / LT-P / ST slots of td.memory), whatever the source rate. How long ago they were is measured.
			  "received full-resolution luma R16 of the last <= 3 anchors (kAnchorRefCount: the codec's reference candidates)",
			  { { kP13SourceRing, 0, 0 }, { kQualityPattern, 0, 0 } },
			  { { "floor: none", Rule({}) }, { "full: 3 anchors", Rule({ { 0, 2 * kAnchorRefCount } }) } } },
			{ kTdDelta, "td.delta", "repair/temporal_detail", kUnitVersion, "TD job", "TD apply runs the same job",
			  "carry-forward (as td.blocks)", "Delta0 at 1/4 resolution (luma + 2 chroma R16F), per-block band weights RGBA8; ring = lane lead + 1",
			  { { kTdBlocks, 0, 0 }, { kTdMemory, 1, 0 }, { kP13SourceRing, 0, 0 } },
			  { { "floor: Delta0 + weights", Rule({ { 2, 6 }, { 4, 4 } }) },
			    { "full: + full-resolution Delta1 atlas", Rule({ { 2, 6 }, { 0, 1 }, { 4, 4 } }) } } },
			{ kTdMaps, "td.maps", "repair/temporal_detail (U19 TdMapsView)", kMapsViewUnit, "TD job", "temporal detail (first lag-0 reader) runs the apply",
			  "absent: every reader runs its classical form (byte-identical today)",
			  "per 16 px block 3 x RGBA16F = 12 named fields (TdMapsView::Field); ring = lane lead + 1",
			  { { kTdBlocks, 0, 0 }, { kP10GrainSplit, 0, 0 }, { kCodingEvidence, 0, 0 }, { kRefinedField, 0, 0 } }, {} },
			{ kTdTrailSummary, "td.trail_summary", "repair/temporal_detail", kUnitVersion, "TD job (reduction + async readback)", "none",
			  "absent: source identity keeps its record unchanged", "CPU record per frame, 256 B, history = kTdTrailSummaryHistorySeconds of the measured source rate (system RAM)",
			  { { kTdBlocks, 0, 0 } }, {} },
		};
		return rows;
	}

	inline const Contract* ContractOf(ProductId id)
	{
		for (const Contract& c : Contracts())
			if (c.id == id)
				return &c;
		return nullptr;
	}

	// ---- the readers --------------------------------------------------------------------------------------------------------

	enum class Consumer : uint8_t { AdaptiveScaler, DitherNeed, SigmaDelta, ChromaStage, Deband, Engine };

	// The products each consumer reads (the catalog rows in core/ProductCatalog.h state the same sets; a test compares them).
	// Reads of td.maps go through TdMapsView (the fields: TdMapsView::FieldsOf).
	// Built once and returned by reference: the per-frame read of a consumer must not allocate.
	inline const std::vector<ProductId>& NeededBy(Consumer c)
	{
		using namespace VpSkeleton;
		static const std::vector<ProductId> sets[] = {
			/* AdaptiveScaler */ { kTdMaps, kRefinedField },
			/* DitherNeed     */ { kTdMaps, kQualityPattern },
			/* SigmaDelta     */ { kTdMaps, kRefinedField },
			/* ChromaStage    */ { kTdMaps, kTdDelta, kTdMemory, kRefinedField },
			/* Deband         */ { kTdMaps, kCodingEvidence },
			/* Engine         */ { kCodingEvidence, kQualityPattern, kRefinedField, kTdAnchorRefs, kTdDelta, kTdMaps, kTdTrailSummary },
		};
		static const std::vector<ProductId> none;
		const size_t i = size_t(c);
		return i < sizeof(sets) / sizeof(sets[0]) ? sets[i] : none;
	}

	inline TdMapsView::Reader MapsReaderOf(Consumer c)
	{
		switch (c)
		{
		case Consumer::AdaptiveScaler: return TdMapsView::Reader::AdaptiveScaler;
		case Consumer::DitherNeed: return TdMapsView::Reader::DitherNeed;
		case Consumer::SigmaDelta: return TdMapsView::Reader::SigmaDelta;
		case Consumer::ChromaStage: return TdMapsView::Reader::ChromaStage;
		case Consumer::Deband: return TdMapsView::Reader::Deband;
		case Consumer::Engine: break;
		}
		return TdMapsView::Reader::AdaptiveScaler;
	}

	// The consumer's BASE fallback id: its own existing entry (doc 72 section 3), the one its classical cause engages and the one
	// the catalog rows name; the engine itself has its own. The temporal-detail read never engages this bare id: it keys per id +
	// cause (TdProducts::FallbackKey in TdProductReads.h), so its entry cannot collide with the consumer's own cause.
	inline const char* FallbackIdOf(Consumer c)
	{
		switch (c)
		{
		case Consumer::AdaptiveScaler: return "product.adaptive_downscale";
		case Consumer::DitherNeed: return "product.dither_need";
		case Consumer::SigmaDelta: return "product.sigma_delta";
		case Consumer::ChromaStage: return "product.chroma_cfl";
		case Consumer::Deband: return "product.debander";
		case Consumer::Engine: return "product.td_engine";
		}
		return "product.td_engine";
	}

	inline const char* Name(Consumer c)
	{
		switch (c)
		{
		case Consumer::AdaptiveScaler: return "adaptive scaling";
		case Consumer::DitherNeed: return "dither need";
		case Consumer::SigmaDelta: return "sigma-delta";
		case Consumer::ChromaStage: return "chroma upscale";
		case Consumer::Deband: return "debander";
		case Consumer::Engine: return "temporal detail engine";
		}
		return "?";
	}

	// ---- VRAM budget (declared sizes; the governor decides the stage) ---------------------------------------------------------

	struct Budget
	{
		uint64_t vram = 0;          // GPU-resident bytes (Vram residency)
		uint64_t systemRam = 0;     // records and CPU products
		uint64_t transient = 0;     // the shared arena's peak figure (a maximum, not a sum)
	};

	// Bytes of one product at `dims` with the governed stage `stage` (clamped to its last stage); a product that is not governed
	// has one size, the registry's.
	inline uint64_t BytesAt(const VpSkeleton::Registry& reg, ProductId id, VpSkeleton::Dims dims, size_t stage)
	{
		const Contract* c = ContractOf(id);
		if (c && !c->stages.empty())
			return VpSkeleton::BytesOf(c->stages[stage < c->stages.size() ? stage : c->stages.size() - 1].size, dims);
		return VpSkeleton::BytesOf(reg.Get(id).size, dims);
	}

	// All engine products at the floor (stage 0, what the registry declares) or at their last (full) stage; ring multipliers and the
	// stored history of the CPU records are the graph's / the product's own (records count their history).
	inline Budget BudgetAt(const VpSkeleton::Registry& reg, VpSkeleton::Dims dims, bool full)
	{
		Budget b;
		for (const Contract& c : Contracts())
		{
			const VpSkeleton::ProductDecl& d = reg.Get(c.id);
			const uint64_t bytes = BytesAt(reg, c.id, dims, full ? c.stages.size() : 0);
			if (d.residency == VpSkeleton::Residency::SystemRam) b.systemRam += bytes * (d.history > 0 ? d.history : 1u);
			else if (d.residency == VpSkeleton::Residency::Transient) b.transient += bytes;
			else b.vram += bytes;
		}
		return b;
	}
	inline Budget BudgetAt(VpSkeleton::Dims dims, bool full) { return BudgetAt(VpSkeleton::SkeletonProducts(), dims, full); }
}
