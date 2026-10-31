// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// Temporal history vs VP-V's entrance stages (Bill v1.4.02 merge, owner decision 1, 2026-10-31).
//
// Bill's TemporalRenderer (libplacebo smooth motion mixing, deinterlacing) keeps its own history of
// CAPTURED pictures and renders each one itself (ProcessSource); of the frame the host passes it
// uses only the crop and rotation. VP-V's entrance stages (re-base, VP deband, detail pulse,
// texture restore) process the current picture only. Mixing the two would apply a re-based crop
// to un-re-based history pictures (wrong geometry) and silently drop the stages' work.
//
// Either/or, one authority per frame:
//   - Bill's temporal renderer active  -> its history is authoritative: the entrance stages do not
//     run on that draw, the frame it is given carries the captured picture's own crop (exact
//     geometry), libplacebo's deband stands in for VP's (the existing deband_vp_exclusive yield);
//     the pause is listed under Fallbacks engaged while any entrance stage would have run.
//   - otherwise                        -> VP-V's entrance path, unchanged.
// The Original pathway has no entrance stages (its table switches them off): Bill's renderer
// untouched. History holding VP-processed pictures (cache keyed on the entrance result) is the
// alternative the owner allowed; it is not built (per-field re-base for the deinterlacer).
//
// Header-only, C++14.
namespace TemporalHistoryPolicy
{
	struct Decision
	{
		bool runEntrance = true;    // the entrance stages run on this draw
		bool showPause = false;     // list "entrance paused" under Fallbacks engaged
	};

	inline Decision Decide(bool temporalActive, bool entranceHasStages)
	{
		Decision d;
		d.runEntrance = !temporalActive;
		d.showPause = temporalActive && entranceHasStages;
		return d;
	}

	inline const char* FallbackId() { return "temporal entrance"; }
	inline const char* PauseText()
	{
		return "VP entrance stages (re-base, VP deband, detail) paused: libplacebo smooth motion / deinterlacing owns the temporal history";
	}
}
