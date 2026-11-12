// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// The ONE read API of the P15 / P15c frame-pattern record for its consumers (temporal-detail engine status, dither need): pure,
// header-only, like SourceQualityView.h. DECLARATION ONLY (P15-DECL): no producer exists, so today every call finds the record
// absent and returns the VISIBLE "frame pattern: not produced" (engaged FallbackCatalog "product.quality_pattern"; never silent).
//
//   record null / unit 0 / other unit version  -> available = false, reason, engaged decision, status "frame pattern: not produced"
//   record from P15c (causal flag, or lead 0)  -> available, provisional = true, status "frame pattern: provisional"
//   record from P15                            -> available, provisional = the record's own kProvisional bit
//
// Both consumers are STATUS-ONLY here (owner: DITHER-NEED wiring stays after the reset; the engine is not built): the view never feeds
// behaviour, StatusOnly(consumer) says so, and a test pins it.
#include <VP-V/core/FallbackCatalog.h>
#include <VP-V/modules/input/source_analysis/identity/quality_pattern/QualityPatternRecord.h>

#include <string>

namespace QualityPattern
{
	enum class Consumer { TemporalDetailEngine, DitherNeed };

	struct View
	{
		bool available = false;
		FrameRole role = FrameRole::Unknown;
		bool provisional = false;
		std::string reason;                   // why it is not available ("" when available)
		FallbackCatalog::Decision fallback;   // engaged exactly when !available (id "product.quality_pattern")
	};

	// `rec` = the sealed record at the reader's lag (null = none). `isCausal` = the reader got it from quality_pattern.causal (P15c).
	inline View Read(const Record* rec, bool isCausal)
	{
		View v;
		if (rec == nullptr)
			v.reason = "no producer or no record";
		else if (rec->unit != kUnitVersion)
			v.reason = rec->unit == 0 ? "record never written" : "record of another unit version";
		if (!v.reason.empty())
		{
			v.fallback = FallbackCatalog::ProductFallback("product.quality_pattern", v.reason);
			return v;
		}
		v.available = true;
		v.fallback = FallbackCatalog::Released("product.quality_pattern");
		v.role = rec->role <= uint8_t(FrameRole::ExactCopy) ? FrameRole(rec->role) : FrameRole::Unknown;
		v.provisional = isCausal || rec->causal != 0 || (rec->validityBits & kProvisional) != 0;
		return v;
	}

	// The visible status text (both states are shown, never ""). `consumer` only names who is reading (the wording is the same).
	inline std::string StatusText(const View& v)
	{
		return v.available ? (v.provisional ? "frame pattern: provisional" : "frame pattern: available") : "frame pattern: not produced";
	}

	// Both consumers only SHOW the status today; no behaviour reads the role (DITHER-NEED wiring stays after the reset).
	inline bool StatusOnly(Consumer) { return true; }
}
