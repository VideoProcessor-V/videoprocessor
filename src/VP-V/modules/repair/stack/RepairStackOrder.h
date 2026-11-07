// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// repair.stack (DRAFT module, wave S2, WP9): the ORDER of the repair stack as one explicit, tested declaration.
//
//   temporal detail  ->  deband  ->  contrast recovery          (RepairStack::Order)
//
// Until now the order was implied by where the code happened to run and by prose ("today's text": deband -> temporal detail -> contrast
// recovery, doc 62 section 3.4 Stack A). Temporal detail (TD) reconstructs structure from the anchors, so it goes FIRST: the debander
// then works on the picture TD has already repaired, and contrast recovery last, on the debanded picture. The debander becomes the
// FALLBACK LAYER under TD: where TD covered a block (td.maps validity / stability) the debander does not deband it a second time; where TD is
// absent it debands everywhere, exactly as it does today.
//
// Today's behaviour is unchanged: TD is not built (the engine is requested by no key: TdProducts::kEngineRequestedToday), so the layers that
// RUN are {deband, contrast recovery}, whose relative order is the same in both the old and the new declaration (the test pins it). When TD
// is requested but its products are not usable the plan says so: the deband is the fallback layer and the status line names the reason
// (no silent fallback); it recovers by itself when TD becomes usable.
//
// Pure, header-only, C++14, no Windows, no GPU. The slot table (core/PipelineSlots.h: repair.deband 80, repair.texture_restore 90) and the
// catalog's pathway order (core/ProductCatalog.h: temporal detail 10, debander 40, contrast recovery need 55) agree with this order; a
// test compares the catalog rows. The wave of WP4b / WP5b / WP8 (deband / contrast-recovery reference rewrites) is NOT part of this unit.
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace RepairStack
{
	enum class Layer : uint8_t { TemporalDetail, Deband, ContrastRecovery };
	constexpr size_t kLayerCount = 3;

	inline const char* Name(Layer l)
	{
		switch (l)
		{
		case Layer::TemporalDetail: return "temporal detail";
		case Layer::Deband: return "deband";
		case Layer::ContrastRecovery: return "contrast recovery";
		}
		return "?";
	}

	// The declared order, first runs first.
	inline const std::array<Layer, kLayerCount>& Order()
	{
		static const std::array<Layer, kLayerCount> order = { { Layer::TemporalDetail, Layer::Deband, Layer::ContrastRecovery } };
		return order;
	}

	// The order of the earlier text (doc 62 section 3.4 Stack A: "deband -> [TD structure if present] -> CR"), kept for the record: the test
	// pins that it differed from Order() only in the position of temporal detail.
	inline const std::array<Layer, kLayerCount>& LegacyOrder()
	{
		static const std::array<Layer, kLayerCount> order = { { Layer::Deband, Layer::TemporalDetail, Layer::ContrastRecovery } };
		return order;
	}

	inline size_t Rank(Layer l)
	{
		const std::array<Layer, kLayerCount>& o = Order();
		for (size_t i = 0; i < kLayerCount; ++i)
			if (o[i] == l)
				return i;
		return kLayerCount;
	}

	inline bool Precedes(Layer a, Layer b) { return Rank(a) < Rank(b); }

	// "temporal detail -> deband -> contrast recovery" (the layers of `order`, in sequence).
	inline std::string OrderText(const std::array<Layer, kLayerCount>& order = Order())
	{
		std::string s;
		for (size_t i = 0; i < kLayerCount; ++i)
			s += (i ? " -> " : "") + std::string(Name(order[i]));
		return s;
	}

	// The layers that run this frame, in order, and what the stack says about it.
	struct Plan
	{
		std::array<Layer, kLayerCount> layers{};
		size_t count = 0;
		bool temporalDetailRuns = false;
		bool debandIsFallbackLayer = false;   // TD was asked for but is not usable: the debander covers the picture on its own, everywhere
		std::string status;                   // "" unless debandIsFallbackLayer: the visible line (Ctrl+I) with the reason

		bool Has(Layer l) const
		{
			for (size_t i = 0; i < count; ++i)
				if (layers[i] == l)
					return true;
			return false;
		}
	};

	// `tdRequested`: the temporal-detail engine is asked for (today: never). `tdUsable`: its products the stack reads are present, fresh and
	// of this unit version. `tdReason`: why not (TdProducts::ReadResult::reason).
	//   not requested        -> {deband, contrast recovery}: today's behaviour, byte-identical, nothing to say (not a fallback)
	//   requested and usable -> {TD, deband, contrast recovery}: the debander skips what TD covered
	//   requested, unusable  -> {deband, contrast recovery}, deband = the fallback layer, visibly
	inline Plan Decide(bool tdRequested, bool tdUsable, const std::string& tdReason)
	{
		Plan p;
		p.temporalDetailRuns = tdRequested && tdUsable;
		for (Layer l : Order())
		{
			if (l == Layer::TemporalDetail && !p.temporalDetailRuns)
				continue;
			p.layers[p.count++] = l;
		}
		if (tdRequested && !tdUsable)
		{
			p.debandIsFallbackLayer = true;
			p.status = "Repair stack: " + OrderText() + "; temporal detail unavailable" + (tdReason.empty() ? std::string() : " (" + tdReason + ")")
			           + ", deband runs everywhere as the fallback layer";
		}
		return p;
	}
}
