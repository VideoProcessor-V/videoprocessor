#pragma once
/*
 * Copyright(C) 2026 VP-V contributors
 * SPDX-License-Identifier: GPL-3.0-only
 */
// F-8 (m2-dv findings, owner rule vp-either-or-overlaps): the either/or between VP's HDR inference
// (HdrInferenceTracker: BT.2020 at 10/12-bit or 8-bit with an LLDV indication, DV-tunnel and
// explicit-settings gates, signal epochs, the picture-statistics SDR veto) and Bill's candidate
// decision (BT.2020 + SDR EOTF + no HDR InfoFrame + both HDR modes Follow input (LLDV), confirmed
// after 1.5 s). The dialog flow around the decision stays VP's.
//
// Key: [vpv] hdr_inference = vp | original (default vp: the behaviour since 2026-10-17).
// The selection is gated through the slot registry on input.signal: the Original pathway puts
// input.signal in its upstream form (PipelineSlots::Registry::UpstreamForm), which selects Bill's
// heuristic whatever the key says; so does input.signal not running. (input.signal is a host slot
// that pipeline_slots_off cannot switch off today, so in practice the gate is the Original
// pathway; the check stays general.)
//
// Both sides keep [general] newlldv as the master switch (it is Bill's key) and newlldv_sdr_veto
// (VP's veto; inert in the original heuristic, which has none).
//
// Used by the GUI start-up (VideoProcessorApp: /hdr_inference <effective>) and by the renderer's
// settings dump / status. Header-only, C++14.
#include <ConfigFile.h>
#include <VP-V/core/SlotRegistry.h>

#include <string>

namespace HdrInferenceChoice
{
	enum class Mode { Vp, Original };

	constexpr const char* kKey = "hdr_inference";
	constexpr const char* kSection = "vpv";

	inline const char* Name(Mode mode) { return mode == Mode::Original ? "original" : "vp"; }

	// "vp" / "original" (case and separators as ConfigFile normalises them); anything else: false.
	inline bool Parse(const std::string& text, Mode& mode)
	{
		const std::string value = ConfigFile::NormalizeName(text);
		if (value == "vp") { mode = Mode::Vp; return true; }
		if (value == "original") { mode = Mode::Original; return true; }
		return false;
	}

	// The mode in effect: the setting, unless the registry has input.signal in its upstream form
	// (Original pathway) or not running - then Bill's heuristic.
	inline Mode Effective(Mode setting, const PipelineSlots::Registry& registry)
	{
		if (registry.UpstreamForm(PipelineSlots::Id::InputSignal) || !registry.Runs(PipelineSlots::Id::InputSignal))
			return Mode::Original;
		return setting;
	}

	struct Resolved
	{
		Mode setting = Mode::Vp;      // what the key says (default vp)
		Mode effective = Mode::Vp;    // after the slot gate
		bool originalPathway = false; // [vpv] pipeline_original_pathway = on
		std::string problem;          // an unreadable value (kept at the default, reported)
	};

	// From the configuration file the way the renderer reads the VP-V globals ([vpv], then
	// [vprenderer] for older files).
	inline Resolved FromConfig(const ConfigFile& config)
	{
		Resolved r;
		std::string raw;
		if (config.TryGetString(kSection, kKey, raw) || config.TryGetString("vprenderer", kKey, raw))
			if (!Parse(raw, r.setting))
				r.problem = std::string(kKey) + " '" + raw + "' (vp|original)";
		std::string pathway;
		if (config.TryGetString(kSection, "pipeline_original_pathway", pathway) ||
			config.TryGetString("vprenderer", "pipeline_original_pathway", pathway))
			r.originalPathway = ConfigFile::NormalizeName(pathway) == "on";
		PipelineSlots::Registry registry;
		if (r.originalPathway)
			registry.ApplyOriginalPathway();
		r.effective = Effective(r.setting, registry);
		return r;
	}

	// One line for logs and the state report.
	inline std::string Describe(const Resolved& r)
	{
		std::string text = std::string("HDR inference: ") + (r.effective == Mode::Original ?
			"Bill's candidate decision (original newlldv heuristic)" : "VP inference");
		if (r.effective != r.setting)
			text += " (hdr_inference " + std::string(Name(r.setting)) + "; the Original pathway selects Bill's)";
		if (!r.problem.empty())
			text += "; invalid " + r.problem + ", kept vp";
		return text;
	}
}
