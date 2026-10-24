#pragma once

// VP-V Config organisation (first organisation pass, 2026-10-24; docs/settings/organisation.md).
// Pure data, no Qt: where every VP setting lives on the VP-V page.
//
// Each unit tab is laid out top to bottom as: the unit's summary and tier slider (VpvSection), then
// the cards below in order, then the planned settings and the unit's stages. A card holds, in its
// listed order:
//   Toggle  - a VP-V toggle of that unit (vpv-tiers.json "toggles"), by key;
//   Section - a whole upstream Rendering-page section, by its id ("rendererSection.<id>"); the
//             section keeps its form, so its own show/hide logic keeps working;
//   Row     - one upstream Rendering-page row, by renderer key (every row whose control is named
//             "config.vprenderer.<key>" or "config.vprenderer.<key>.<part>");
//   Note    - a line of help text.
// Upstream controls MOVE (one live control per key, never a copy); they stay bound to the
// Rendering page's selected profile, which the VP-V profile picker follows. Rows of keys VP-V
// itself manages (tier keys, toggles) are hidden on the VP-V page while the VP-V pathway is on,
// because the VP-V control above them is the live one.
//
// A toggle not named in any card lands in a card "Other settings" (nothing is lost); a toggle that
// is a retirement candidate is shown on the Proposed retirement tab instead (owner: candidates
// only there). Anything the table names that the editor does not have is skipped (the tests list
// it), so the table can run ahead of, or behind, upstream.
//
// The Rendering page keeps Bill's own sections (kUpstreamSections) in place; every other
// Rendering-page section that is left goes into one collapsed "All raw settings (expert)" group at
// the bottom of the profile (the safety net: nothing is removed).

#include <vector>

namespace Vpv
{
	enum class ItemKind { Toggle, Section, Row, Note };
	struct CardItem { ItemKind kind; const char* id; };
	struct Card
	{
		const char* unit;          // unit id (vpv-tiers.json), or "retirement"
		const char* id;            // object name part: vpv.<unit>.card.<id>
		const char* title;
		const char* help;          // may be nullptr
		bool collapsed;            // an "Advanced" card: closed until opened
		std::vector<CardItem> items;
	};

	inline const std::vector<Card>& Organisation()
	{
		using K = ItemKind;
		static const std::vector<Card> cards = {
			// ---- Input ----
			{ "input", "capture", "Capture", "How the picture comes in from the capture card.", false, {
				{ K::Toggle, "capture_path" }, { K::Toggle, "capture_upload" }, { K::Toggle, "native_rgb_ingest" },
				{ K::Toggle, "dv_tunnel_gpu_unpack" }, { K::Toggle, "dv_trims" } } },
			{ "input", "signal", "Signal and content", "What kind of signal it is, and labels the profile rules can use.", false, {
				{ K::Toggle, "source_range" }, { K::Toggle, "intent_content" }, { K::Row, "intent_content_hold_seconds" } } },

			// ---- Motion ----
			{ "motion", "engine", "Motion engine and hiccup repair",
				"Where the motion vectors come from, and how dropped or repeated frames are repaired.", false, {
				{ K::Toggle, "interpolation_fill_gaps" }, { K::Toggle, "interpolation" }, { K::Toggle, "interpolation_fallback" },
				{ K::Toggle, "dejudder_estimate_ahead" } } },
			{ "motion", "dejudder", "De-judder (your motion slider)",
				"Your taste: how much film judder is smoothed. The slider sets the fields below it.", false, {
				{ K::Section, "dejudder" } } },
			{ "motion", "render_ahead", "Render ahead (under test)",
				"Draws de-judder frames early to absorb timing hiccups.", false, {
				{ K::Toggle, "render_ahead" }, { K::Row, "render_ahead_utilisation" } } },
			{ "motion", "easing", "Per-shot easing",
				"Lowers the de-judder on shots that need less (never raises it).", false, {
				{ K::Toggle, "intent_motion" }, { K::Row, "intent_motion_policy" }, { K::Row, "intent_motion_min" },
				{ K::Row, "intent_motion_min_shot_s" }, { K::Row, "intent_ease_s" } } },
			{ "motion", "advanced", "Advanced: motion estimator (measured, leave alone)",
				"The estimator's measured internals. Normal viewing never needs these.", true, {
				{ K::Section, "dejudderEstimation" }, { K::Row, "dejudder_gate" } } },

			// ---- Repair ----
			{ "repair", "deband", "Deband",
				"VP's debander: removes banding from compressed sources and leaves film grain alone.", false, {
				{ K::Toggle, "deband_vp_grain_guard" }, { K::Toggle, "deband_vp_exclusive" }, { K::Toggle, "low_bitrate_handling" },
				{ K::Note, "libplacebo's own debander (Deband strength) is Bill's: VP Renderer > Rendering > Processing. "
					"With VP's debander on it is replaced (the switch above)." } } },
			{ "repair", "texture", "Texture restore and detail pulse",
				"Steadies detail that pulses with compression, and restores texture lost to it. The tier sets them on or off; "
				"these are their limits.", false, {
				{ K::Row, "detail_pulse_cap" }, { K::Row, "texture_restore_cap" } } },

			// ---- Scaling ----
			{ "scaling", "rebase", "Re-base (your picture slider)",
				"Finds a picture that was upscaled before VP (a 1080p film sent at 4K) and works from its real size. "
				"The slider sets how much evidence it needs.", false, {
				{ K::Toggle, "rebase_kernel_match" }, { K::Toggle, "source_resolution_text_guard" },
				{ K::Toggle, "source_resolution_mixed_guard" }, { K::Section, "sourceRebase" } } },
			{ "scaling", "grain", "Grain-aware scaling",
				"VP's downscale and upscale blends that keep film grain true.", false, {
				{ K::Toggle, "adaptive_faithful" }, { K::Toggle, "adaptive_grain_map" }, { K::Toggle, "ssim_bitrate_aware" },
				{ K::Toggle, "ssim_residual_stabilise" }, { K::Toggle, "cnn_residual_stabilise" }, { K::Toggle, "adaptive_upscale" },
				{ K::Note, "The scaler filters themselves (upscaler, downscaler, chroma) are Bill's: VP Renderer > Scaling." } } },
			{ "scaling", "advanced", "Advanced: scaler thresholds and re-base timing",
				"Measured thresholds and weights. Normal viewing never needs these.", true, {
				{ K::Section, "adaptiveDownscale" }, { K::Section, "adaptiveUpscale" },
				{ K::Row, "source_resolution_engage_max_wait_s" }, { K::Row, "source_resolution_crossfade_s" },
				{ K::Row, "source_resolution_release_fade_s" } } },

			// ---- Colour ----
			{ "colour", "range", "Range and precision", "Keeps every value the source carries.", false, {
				{ K::Toggle, "preserve_extended_range" }, { K::Toggle, "fbo_float32" } } },
			{ "colour", "sdr", "SDR tone shape", "A gentle highlight knee for SDR sources.", false, {
				{ K::Toggle, "sdr_tone_shape" }, { K::Section, "sdrHighlights" } } },
			{ "colour", "room", "Room-light adaptation (experimental)",
				"Adjusts black and contrast to the room light the sensor measures.", false, {
				{ K::Toggle, "ambient_adapt" }, { K::Row, "ambient_adapt_strength" }, { K::Row, "ambient_screen_reflect" },
				{ K::Row, "ambient_max_black_nits" }, { K::Row, "ambient_tau_up_s" }, { K::Row, "ambient_tau_down_s" } } },
			{ "colour", "upstream", "Tone mapping and display (Bill's pages)", nullptr, false, {
				{ K::Note, "Tone mapping, HDR target, gamut and output gamma: VP Renderer > Rendering (Tone mapping cards) and "
					"VP Renderer > Color / Output. The calibration LUT file: Color / Output; where it runs: the Pipeline (expert) tab." },
				{ K::Note, "Colour space, HDR luminance and video conversion of the INPUT are set once on General > Input "
					"processing. The VP Renderer and DirectShow pages can override them for one renderer; they say "
					"\"Inherited\" while they follow General." } } },

			// ---- Dynamic Drive ----
			{ "drive", "pages", "Dynamic Drive (its own page)", nullptr, false, {
				{ K::Note, "Everything Dynamic Drive does is on the Dynamic Drive page, in cards: Projector connection, "
					"Dynamic Drive (command source, script or compensate), LED profile, LED speeds, Contrast and cuts, "
					"Projector response, and the collapsed Advanced LED / Advanced projector / Experimental cards." } } },

			// ---- Output ----
			{ "output", "dither", "Temporal dither", "How the final picture is fitted to the projector link's bit depth.", false, {
				{ K::Toggle, "occlusion_mask" }, { K::Section, "temporalDither" } } },
			{ "output", "presentation", "Presentation and delay", nullptr, false, {
				{ K::Note, "Display bit depth and libplacebo's dithering (used when VP's is off) are Bill's: VP Renderer > "
					"Rendering > Processing. The total delay: Input tab (Total delay) and the Queue page." } } },

			// ---- Proposed retirement: the candidates' controls (they still work until approved) ----
			{ "retirement", "controls", "Controls of the candidates (still active until you decide)",
				"Each still works as before. Retiring keeps today's default behaviour (the fixes stay on).", true, {
				{ K::Row, "pulldown_fixes" }, { K::Row, "timing_continuity_fixes" }, { K::Row, "cadence_class_fixes" },
				{ K::Row, "motion_history_fixes" }, { K::Row, "queue_timing_fixes" }, { K::Row, "general_rate_fixes" },
				{ K::Row, "efficiency_fixes" }, { K::Row, "source_coding_fixes" }, { K::Row, "source_resolution_latch_fixes" },
				{ K::Row, "source_resolution_cut_fixes" }, { K::Row, "active_picture_startup_hold" },
				{ K::Row, "extended_range_more_cases" }, { K::Row, "dv_tunnel_fixes" }, { K::Row, "residual_stabilise_grain_guard" },
				{ K::Row, "optical_flow_cost_trust" }, { K::Row, "interpolation_grain" }, { K::Row, "dejudder_source_warp_filter" },
				{ K::Row, "nls_sampling" }, { K::Row, "deband_grain" }, { K::Row, "render_ahead_queue_credit" },
				{ K::Section, "dejudderExperiments" } } },
		};
		return cards;
	}

	// Bill's sections of the Rendering profile: they stay where they are, as his themed cards.
	inline const std::vector<const char*>& UpstreamRenderingSections()
	{
		static const std::vector<const char*> ids = {
			"toneMapping", "toneSpline", "toneOther", "tonePeak", "toneHighlightBeta", "processing" };
		return ids;
	}
}
