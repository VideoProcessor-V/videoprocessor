#pragma once

#include "PipelineSlots.h"

#include <algorithm>
#include <vector>

// The corrected output-hook order in ONE place (design freeze 2026-10-24; ADR-0013; barrier R1/R2).
//
// The hybrid scaler runs inside libplacebo BEFORE its PRE_OUTPUT hooks, so every VP output hook
// runs at the output size. Within a libplacebo stage, hooks run in the order they are handed
// over, so the renderer tags each hook it collected and hands libplacebo Order(tagged):
//
//   libplacebo output-encoding repair (workaround; first whenever a PRE_OUTPUT hook runs)
//   -> 200 SDR tone shape + DV trims -> 210 Stage 13 appearance -> 220 LED measurement
//   -> 230 LED compensation -> 240 black level
//   -> user PRE_OUTPUT shaders -> the de-judder rendered-picture cache
//   -> 250 projector calibration LUT (VP's LAST PRE_OUTPUT hook, R1)
//   -> OUTPUT-stage hooks -> 260 the output quantiser (sigma-delta) last.
//
// Hooks at other stages (scaler, NLS geometry, adaptive downscale measure/blend) are "Other": they
// keep their relative order and are placed first; libplacebo runs them at their own stages.
// Only hooks of slots that RUN are collected at all (zero-cost rule, PipelineSlots.h).
namespace OutputHookOrder
{
	enum class Kind
	{
		Other,              // not an output-stage hook (scaler, geometry, ...)
		EncodingRepair,     // OutputEncodingRepair (libplacebo 7.360 PRE_OUTPUT workaround)
		Slot,               // a pipeline slot's hook: ranked by the slot order
		UserPreOutput,      // configured custom PRE_OUTPUT shaders
		DejudderCache,      // keeps the rendered picture for the de-judder extras
		OutputStage,        // other OUTPUT-stage hooks
		Quantiser           // the output quantiser (sigma-delta): always last
	};

	template <class Hook>
	struct Tagged
	{
		Hook hook;
		Kind kind = Kind::Other;
		PipelineSlots::Id slot = PipelineSlots::Id::Count;   // for Kind::Slot
	};

	// Rank of a tagged hook. Gaps leave the slot orders (200..250) meaningful.
	template <class Hook>
	int Rank(const Tagged<Hook>& t)
	{
		using PipelineSlots::Id;
		switch (t.kind)
		{
		case Kind::Other: return 0;
		case Kind::EncodingRepair: return 100;
		case Kind::Slot:
		{
			const int order = PipelineSlots::Get(t.slot).order;
			// The calibration LUT is the last colour op: after the user shaders and the cache.
			return t.slot == Id::OutputCalibrationLut ? 248 : order == 260 ? 400 : order;
		}
		case Kind::UserPreOutput: return 245;
		case Kind::DejudderCache: return 246;
		case Kind::OutputStage: return 300;
		case Kind::Quantiser: return 400;
		}
		return 0;
	}

	// The hooks in the corrected order (stable: equal ranks keep the collection order).
	template <class Hook>
	std::vector<Hook> Order(std::vector<Tagged<Hook>> tagged)
	{
		std::stable_sort(tagged.begin(), tagged.end(),
			[](const Tagged<Hook>& a, const Tagged<Hook>& b) { return Rank(a) < Rank(b); });
		std::vector<Hook> hooks;
		hooks.reserve(tagged.size());
		for (const Tagged<Hook>& t : tagged)
			hooks.push_back(t.hook);
		return hooks;
	}

	template <class Hook>
	bool InOrder(const std::vector<Tagged<Hook>>& tagged)
	{
		return std::is_sorted(tagged.begin(), tagged.end(),
			[](const Tagged<Hook>& a, const Tagged<Hook>& b) { return Rank(a) < Rank(b); });
	}
}
