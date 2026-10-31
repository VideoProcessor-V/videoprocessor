#pragma once

#include "PipelineSlots.h"

#include <algorithm>
#include <vector>

// The corrected output-hook order in ONE place (design freeze 2026-10-24; ADR-0013; barrier R1/R2).
//
// The hybrid scaler runs inside libplacebo BEFORE its PRE_OUTPUT hooks, so every VP output hook
// runs at the output size. Within a libplacebo stage, hooks run in the order they are handed
// over, so the renderer tags each hook it collected, calls OrderSlotsInPlace(tagged, rank) and hands
// libplacebo the result:
//
//   libplacebo output-encoding repair (workaround; first whenever a PRE_OUTPUT hook runs)
//   -> 200 SDR tone shape + DV trims -> 210 Stage 13 appearance -> 220 LED measurement
//   -> 230 LED compensation -> 240 black level
//   -> user PRE_OUTPUT shaders -> 250 projector calibration LUT (VP's last colour hook, R1)
//   -> the de-judder rendered-picture cache (it keeps the LUT-mapped picture for the extras)
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
	// The ranks of the hooks that are not slots, around the slots' own orders (the PRE_OUTPUT
	// slots use their table order, 200..240). Named so the order reads as the design's list.
	namespace Ranks
	{
		constexpr int kOtherStages = 0;          // scaler / geometry hooks: their own libplacebo stages
		constexpr int kEncodingRepair = 100;     // first, before any PRE_OUTPUT slot
		constexpr int kUserPreOutput = 245;      // after the black level (240)
		constexpr int kCalibrationLut = 247;     // the last colour op
		constexpr int kDejudderCache = 248;      // keeps the LUT-mapped picture for the extras
		constexpr int kOutputStage = 300;        // other OUTPUT-stage hooks
		constexpr int kQuantiser = 400;          // the output quantiser, always last
	}

	template <class Hook>
	int Rank(const Tagged<Hook>& t)
	{
		using PipelineSlots::Id;
		switch (t.kind)
		{
		case Kind::Other: return Ranks::kOtherStages;
		case Kind::EncodingRepair: return Ranks::kEncodingRepair;
		case Kind::Slot:
			if (t.slot == Id::OutputCalibrationLut) return Ranks::kCalibrationLut;
			if (t.slot == Id::OutputDither) return Ranks::kQuantiser;
			return PipelineSlots::Get(t.slot).order;
		case Kind::UserPreOutput: return Ranks::kUserPreOutput;
		case Kind::DejudderCache: return Ranks::kDejudderCache;
		case Kind::OutputStage: return Ranks::kOutputStage;
		case Kind::Quantiser: return Ranks::kQuantiser;
		}
		return Ranks::kOtherStages;
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

	// The renderer's form (byte-identical by construction): only the hooks of pipeline SLOTS are
	// reordered, among the positions they already occupy, by `rank` (the registry's effective
	// order, so a validated custom order moves them too); every other hook (libplacebo stage
	// hooks, plumbing, user shaders) keeps its position. Today's chain is already in slot order,
	// so the result equals the input.
	template <class Hook, class RankFn>
	std::vector<Hook> OrderSlotsInPlace(const std::vector<Tagged<Hook>>& tagged, RankFn rank)
	{
		std::vector<size_t> positions;
		std::vector<Tagged<Hook>> slots;
		for (size_t i = 0; i < tagged.size(); ++i)
			if (tagged[i].kind == Kind::Slot)
			{
				positions.push_back(i);
				slots.push_back(tagged[i]);
			}
		std::stable_sort(slots.begin(), slots.end(),
			[&rank](const Tagged<Hook>& a, const Tagged<Hook>& b) { return rank(a.slot) < rank(b.slot); });
		std::vector<Hook> hooks;
		hooks.reserve(tagged.size());
		for (const Tagged<Hook>& t : tagged)
			hooks.push_back(t.hook);
		for (size_t k = 0; k < positions.size(); ++k)
			hooks[positions[k]] = slots[k].hook;
		return hooks;
	}
}
