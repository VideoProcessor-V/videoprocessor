// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// repair.temporal_detail / products: the RESERVED products of the temporal-detail engine (R1-W1-RES, owner 2026-11-06;
// design doc 67v2 section 13). The engine is built in reset 2; reset 1 reserves its products so that
//   * the ids, layouts, unit version and fill rules are fixed before any consumer is wired (core/Products.h declares the
//     registry entries, core/ProductCatalog.h the catalog rows and the three consumer read paths),
//   * every consumer already READS the product, finds it absent and takes its classical form VISIBLY (TdProductReads.h),
//     so reset 2 only fills producers.
//
// This header is the CONTRACT table: one row per product (token, fill rule, layout, what it means, which readers) and the VRAM
// budget per tier at 1080p / 4K / 8K computed from the registry declarations (never a second copy of the sizes). Header-only,
// pure C++14, no Windows, no GPU. A change of any layout, range or fill rule bumps kUnitVersion here AND
// Catalog::kExpectedTdProductsUnit in the same commit (a test compares them; a reader built against another number engages
// its visible fallback through Catalog::VerifyUnit).
#pragma once

#include <VP-V/core/Products.h>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace TdProducts
{
	constexpr uint32_t kUnitVersion = 1;

	using VpSkeleton::ProductId;

	constexpr ProductId kFirstId = VpSkeleton::kTdQualityPattern;
	constexpr int kCount = int(VpSkeleton::kTdRefinePixel) - int(VpSkeleton::kTdQualityPattern) + 1;   // 15

	inline bool IsReserved(ProductId id) { return id >= kFirstId && int(id) < int(kFirstId) + kCount; }
	inline int IndexOf(ProductId id) { return IsReserved(id) ? int(id) - int(kFirstId) : -1; }

	// How a product gets its content (reset 2 implements the producers; the rule is what the consumers may rely on now).
	enum class FillRule : uint8_t
	{
		HeldRecord,   // a small CPU record the engine's worker publishes; readers take the latest sealed record (lag >= 1), never fill it
		EngineMap,    // a per-frame map the engine fills in the render (live-fill owner: temporal detail); never made by a reader
		AnchorSlot,   // a stored picture: filled by a GPU-side copy when an anchor (I / reference) arrives; slots are history
		OptionalMap   // like EngineMap, but whether it is stored or recomputed at the point of use is decided by measurement
	};

	inline const char* Token(FillRule r)
	{
		switch (r)
		{
		case FillRule::HeldRecord: return "held record";
		case FillRule::EngineMap: return "engine map";
		case FillRule::AnchorSlot: return "anchor slot";
		case FillRule::OptionalMap: return "optional map (store vs recompute TBD)";
		}
		return "?";
	}

	enum class Consumer : uint8_t { AdaptiveScaler, DitherNeed, ChromaStage, Engine };

	struct Contract
	{
		ProductId id;
		const char* token;         // equals the registry token
		FillRule fill;
		const char* layout;        // storage form, planning size in core/Products.h
		const char* meaning;       // the contract the readers may rely on
		bool storeVsRecomputeTbd;  // the size declared is the STORE worst case; a measurement may replace it with a recompute
	};

	inline const std::vector<Contract>& Contracts()
	{
		using namespace VpSkeleton;
		static const std::vector<Contract> rows = {
			{ kTdQualityPattern, "td.quality_pattern", FillRule::HeldRecord, "256 B record (system RAM)",
			  "frame quality pattern from the source identity record (68v2): per-frame class and quality tier, GOP phase when known; "
			  "stale record = absent", false },
			{ kTdTrailConfidence, "td.trail_confidence", FillRule::EngineMap, "R16F per 8 px block",
			  "confidence 0..1 of the carried trail (how much of the block's detail travelled from the anchor); 0 = no trail", false },
			{ kTdResidual, "td.residual", FillRule::EngineMap, "R16F at half resolution",
			  "signed residual of the re-prediction against the received picture, in signal codes of the plane", false },
			{ kTdValidity, "td.validity", FillRule::EngineMap, "R8 per 8 px block",
			  "state of the carried data per block: 0 invalid, 1 refresh, 2 fade, 3 valid (readers treat anything but 3 as no carry)", false },
			{ kTdRealDetail, "td.real_detail", FillRule::EngineMap, "R8 at half resolution",
			  "0..1: validated carried structure versus rounding noise and blocking (1 = real detail the scaler may trust)", false },
			{ kTdGrainLevel, "td.grain_level", FillRule::EngineMap, "RG16F per 16 px block",
			  "(own grain rms, coding-noise rms) in signal codes: film grain that must survive versus noise the encoder added", false },
			{ kTdStability, "td.stability", FillRule::EngineMap, "R8 per 8 px block",
			  "0..1 anchor-held regions (1 = the block still equals its anchor-carried reconstruction)", false },
			{ kTdLighting, "td.lighting", FillRule::EngineMap, "RGBA16F per 16 px block",
			  "lighting layer state (slow illumination change separated from structure): gain, offset, confidence, age", false },
			{ kTdDrift, "td.drift", FillRule::EngineMap, "RG16F per 16 px block",
			  "I-to-I drift (re-verification of the previous I carried along the tracked motion): magnitude in codes, direction class", false },
			{ kTdMotionModel, "td.motion_model", FillRule::HeldRecord, "4 KB record (system RAM)",
			  "camera and object motion model calibrated near the I-frame: model class, parameters, residual, valid-from sequence", false },
			{ kTdMirror, "td.mirror", FillRule::HeldRecord, "2 KB record (system RAM)",
			  "the stream's reference set as the decoder holds it: per slot handle into td.base_short, source sequence, quality, age", false },
			{ kTdBaseLong, "td.base_long", FillRule::AnchorSlot, "R16 luma 1/4 per axis + 2 x R16 chroma 4:2:0 at 1/8 per axis, 2 slots",
			  "the gently repaired I-frame and the previous I as grain-free low-res base, with Cb and Cr at native 4:2:0 geometry", false },
			{ kTdBaseShort, "td.base_short", FillRule::AnchorSlot, "same layout, 6 slots",
			  "the reference mirror's pictures: the improved low-res base of each recent reference (about one per mini-GOP)", false },
			{ kTdRefinedVectors, "td.refined_vectors", FillRule::EngineMap, "8 B per 8 px block",
			  "refined per-block vector (2 x int16, quarter-sample units) + confidence u8 + flags u8 (DMVR-style refinement of P3)", false },
			{ kTdRefinePixel, "td.refine_pixel", FillRule::OptionalMap, "RG16F at half resolution (store worst case)",
			  "per-pixel refinement offsets on top of the refined block vectors (BDOF/PROF-style); the recompute form stores nothing", true },
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

	// The products each consumer reads (the catalog rows in core/ProductCatalog.h state the same sets; a test compares them).
	inline std::vector<ProductId> NeededBy(Consumer c)
	{
		using namespace VpSkeleton;
		switch (c)
		{
		case Consumer::AdaptiveScaler:
			return { kTdTrailConfidence, kTdRealDetail, kTdGrainLevel, kTdStability, kTdMotionModel };
		case Consumer::DitherNeed:
			return { kTdTrailConfidence, kTdGrainLevel, kTdStability, kTdMotionModel };
		case Consumer::ChromaStage:
			return { kTdBaseLong, kTdBaseShort, kTdMirror, kTdRefinedVectors, kTdTrailConfidence, kTdRealDetail };
		case Consumer::Engine:
			return { kTdQualityPattern, kTdResidual, kTdValidity, kTdLighting, kTdDrift, kTdRefinePixel };
		}
		return {};
	}

	inline const char* FallbackIdOf(Consumer c)
	{
		switch (c)
		{
		case Consumer::AdaptiveScaler: return "product.td_scaler";
		case Consumer::DitherNeed: return "product.td_dither";
		case Consumer::ChromaStage: return "product.td_chroma";
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
		case Consumer::ChromaStage: return "chroma upscale";
		case Consumer::Engine: return "temporal detail engine";
		}
		return "?";
	}

	// ---- VRAM budget (declared sizes: BytesOf(registry size rule, dims) x stored slots) ---------------------------------

	// Stored slots of a product: the base buffers are pictures kept in history (their history field is the slot count); every
	// other product is one slot per frame (the graph's ring multiplies per frame products by the lane lead).
	inline unsigned StoredSlots(const VpSkeleton::ProductDecl& d)
	{
		return d.id == VpSkeleton::kTdBaseLong || d.id == VpSkeleton::kTdBaseShort ? unsigned(d.history) : 1u;
	}

	struct TierBudget
	{
		uint64_t vram[3] = { 0, 0, 0 };        // T0 / T1 / T2, GPU-resident bytes
		uint64_t systemRam[3] = { 0, 0, 0 };   // held records and CPU products
		uint64_t Vram() const { return vram[0] + vram[1] + vram[2]; }
		uint64_t SystemRam() const { return systemRam[0] + systemRam[1] + systemRam[2]; }
	};

	inline TierBudget BudgetAt(const VpSkeleton::Registry& reg, VpSkeleton::Dims dims)
	{
		TierBudget b;
		for (int i = 0; i < kCount; ++i)
		{
			const VpSkeleton::ProductDecl& d = reg.Get(ProductId(int(kFirstId) + i));
			const uint64_t bytes = VpSkeleton::BytesOf(d.size, dims) * StoredSlots(d);
			const int t = int(d.tier);
			(d.residency == VpSkeleton::Residency::SystemRam ? b.systemRam : b.vram)[t] += bytes;
		}
		return b;
	}
	inline TierBudget BudgetAt(VpSkeleton::Dims dims) { return BudgetAt(VpSkeleton::SkeletonProducts(), dims); }
}
