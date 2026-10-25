#pragma once

#include <VP-V/core/FallbackCatalog.h>
#include <VP-V/modules/colour/extended_range/ExtendedRange.h>

#include <libplacebo/renderer.h>

#include <cstdint>
#include <string>
#include <vector>

// WP-13 (integration batch 1): the colour.extended_range glue that lived in the renderer host
// (LibplaceboVideoRenderer.cpp RenderLocked, the hook-failure scan and the crossfade's late
// restore) as one VP-V stage object. The host keeps one call per hook point:
//
//   extendedRange.Prepare(frame, renderImage.color, target.color, frameHooks)   per frame
//   extendedRange.NoteDisabledHooks(errors.disabled_hooks, errors.num_disabled_hooks)
//   extendedRange.LateRestore()                                                 crossfade mix
//   extendedRange.Release()                                                     GPU teardown
//
// The decision, the colour relabel, the hook placement and the log lines are exactly today's
// (moved, not changed): the move commit is byte-identical. The maths stays in ExtendedRange.cpp.
// Threading: render thread only (as the host code it replaces). No locks.
namespace ExtendedRangeStage
{
	// What the host knows about this frame (all plain values; the stage reads no host state).
	struct Frame
	{
		bool preserve = false;              // activeSettings.preserveExtendedRange
		bool moreCases = false;             // activeSettings.extendedRangeMoreCases
		bool hdrDynamicToneMapping = false; // renderParams.peak_detect_params != nullptr
		bool temporalActive = false;        // temporalRenderer.Active()
		bool temporalMixing = false;        // temporalRenderer.Mixing()
		bool singlePlaneRgb = false;        // renderImage.num_planes == 1 (the carry's texture form)
		const pl_custom_lut* targetLut = nullptr;          // target.lut
		enum pl_lut_type targetLutType = PL_LUT_UNKNOWN;   // target.lut ? target.lut_type : UNKNOWN
	};

	// The ExtendedRange::Settings today's host code builds from the two keys and the peak detection.
	ExtendedRange::Settings SettingsFor(const Frame& frame);

	class Stage
	{
	public:
		// Decides on this frame's source and target, relabels sourceColor when active and places the
		// carry/restore hooks in `hooks` (ExtendedRange::Prepare). Returns true when sourceColor was
		// relabelled (the temporal renderer's retained frames then get the same label).
		bool Prepare(const Frame& frame, pl_color_space& sourceColor, const pl_color_space& targetColor,
			std::vector<const pl_hook*>& hooks);

		// libplacebo's disabled-hook list after a render: a disabled carry/restore turns the
		// feature off until restart (logged once; visible through Fallback()).
		void NoteDisabledHooks(const uint64_t* disabled, int count);

		const pl_hook* LateRestore() const { return hooks.LateRestore(); }
		void Release() { hooks.Release(); }

		// The last frame's decision and whether libplacebo's frame mix clipped BTB on it.
		const ExtendedRange::Decision& LastDecision() const { return decision; }
		bool MixClipsBtb() const { return mixClipsBtb; }
		bool HookFailed() const { return hookFailed; }
		// ADR-0001/0002: the "Fallbacks engaged" decision for the last frame (the wording lives in
		// FallbackCatalog::BtbWtw). Design exclusions (HDR source, output primaries, ...) are not
		// downgrades and stay status-only.
		FallbackCatalog::Decision Fallback(bool preserveRequested) const;

		// The carry/restore hook objects (status readouts, the efficiency audit's idle release).
		ExtendedRange::Hooks& CarryHooks() { return hooks; }
		const ExtendedRange::Hooks& CarryHooks() const { return hooks; }

	private:
		ExtendedRange::Hooks hooks;
		ExtendedRange::Decision decision{};
		int logged = -1;            // the last logged decision
		std::string described;      // the last logged description (audit 2026-10-16)
		bool hookFailed = false;    // libplacebo disabled a hook: off until restart
		bool mixLogged = false;     // hunt 2 (08 #2): BTB lost in libplacebo's frame mix, logged
		bool mixClipsBtb = false;
	};
}
