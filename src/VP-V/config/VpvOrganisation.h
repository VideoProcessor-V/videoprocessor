#pragma once

// VP-V Config organisation (organisation passes 1 and 2, 2026-10-24; docs/settings/organisation.md).
// Pure data, no Qt: where every VP setting lives, and which control is the MASTER of which settings.
//
// Each unit tab has two sub-pages:
//   Main     - the unit's summary, its tier slider, then the cards of masters and switches a viewer
//              might use;
//   Advanced - the settings derived from a master on Main (greyed "Set by <master>" until Custom is
//              ticked, or "Used only when <master> is on"), the measured internals, the planned
//              settings and the unit's stages.
// The same card order on every unit (Main: tier, taste master, switch cards; Advanced: set by the
// tier, set by a slider, feature tuning, measured internals, planned, stages).
//
// A card holds, in order:
//   Toggle  - a VP-V toggle of that unit (vpv-tiers.json "toggles"), by key;
//   Section - a whole upstream Rendering-page section, by its id ("rendererSection.<id>"); it keeps
//             its own form, so its own show/hide logic keeps working;
//   Row     - one upstream Rendering-page row, by renderer key (every row whose control is named
//             "config.vprenderer.<key>" or "config.vprenderer.<key>.<part>");
//   Hide    - upstream rows hidden while the VP-V pathway is on (headings of hidden rows);
//   Note    - a line of help text.
// Upstream controls MOVE (one live control per key, never a copy); they stay bound to the
// Rendering page's selected profile, which the VP-V profile picker follows. The rows of keys a
// VP-V toggle owns are hidden wherever they are while the VP-V pathway is on (the toggle is the
// one live control); the rows of TIER keys move into the unit's "Set by the tier" card and are the
// one control for those keys (locked to the tier until Custom).
//
// A card's lock (the master -> derived rule):
//   Tier     - locked to the unit's tier; Custom unlocks; unticking Custom puts the tier back;
//   Slider   - locked to an upstream slider (masterKey = the slider's name label, resetKey = its
//              reset button); Custom unlocks (ticked by itself when the slider reads Custom);
//   Feature  - usable only while masterKey's effective value is not one of offValues
//              (comma separated; the key's built-in default when unset is fallback);
//   Defaults - locked at the built-in defaults; Custom unlocks; unticking Custom clears the keys.
// lockKeys (optional): lock only these keys' controls in place (a moved section whose other rows
// stay editable); otherwise the whole card body is locked.

#include <vector>

namespace Vpv
{
	// Elsewhere: a toggle of this unit shown in another place (named in the note), not on this card.
	enum class ItemKind { Toggle, Section, Row, Hide, Note, Elsewhere };
	struct CardItem { ItemKind kind; const char* id; };
	enum class LockKind { None, Tier, Slider, Feature, Defaults };
	struct Lock
	{
		LockKind kind;                      // None when the card has no lock (value-initialised)
		const char* master;       // plain name of the master, for "Set by ..." / "Used only when ... is on"
		const char* masterKey;    // Slider: the slider's name label part; Feature: the renderer key
		const char* resetKey;     // Slider: the slider's reset button part
		const char* offValues;    // Feature: values that mean off
		const char* fallback;     // Feature: the built-in value when unset
		std::vector<const char*> lockKeys;  // lock only these keys' controls (empty: the whole card)
	};
	struct Card
	{
		const char* unit;          // unit id (vpv-tiers.json), or "retirement"
		const char* id;            // object name part: vpv.<unit>.card.<id>
		const char* title;
		const char* help;          // may be nullptr
		bool advanced;             // on the unit's Advanced sub-page
		bool collapsed;            // closed until opened (measured internals)
		std::vector<CardItem> items;
		Lock lock;
	};

	inline const std::vector<Card>& Organisation()
	{
		using K = ItemKind;
		using L = LockKind;
		static const std::vector<Card> cards = {
			// ---- Input ----
			{ "input", "capture", "Capture", "How the picture comes in from the capture card.", false, false, {
				{ K::Toggle, "capture_path" }, { K::Toggle, "native_rgb_ingest" }, { K::Toggle, "dv_tunnel_gpu_unpack" } } },
			{ "input", "signal", "Signal and content", "What kind of signal it is, and labels the profile rules can use.", false, false, {
				{ K::Toggle, "source_range" }, { K::Toggle, "intent_content" } } },
			{ "input", "plumbing", "Capture plumbing (expert)", "When each captured frame is copied to the GPU. Normal viewing never needs it.", true, false, {
				{ K::Toggle, "capture_upload" } } },
			{ "input", "content_tuning", "Content labels: tuning", "How long a label (film, video, sports) is held before it may change.", true, false, {
				{ K::Row, "intent_content_hold_seconds" } },
				{ L::Feature, "Content labels", "intent_content", nullptr, "off", "off" } },
			{ "input", "diagnostics", "Readouts (diagnostics)", "Shown in the stats overlay; they change nothing in the picture.", true, true, {
				{ K::Row, "intent_black" }, { K::Row, "intent_display" }, { K::Row, "dv_compare_capture_folder" } } },

			// ---- Motion ----
			{ "motion", "taste", "De-judder amount (your taste)",
				"How much film judder is smoothed: your taste. The tier above chooses how motion is measured; it never "
				"changes the amount (As shot = no motion processing).", false, false, {
				{ K::Row, "motion_preset_box" } } },
			{ "motion", "engine", "Motion engine and hiccup repair",
				"How dropped or repeated frames are repaired, and how motion is estimated.", false, false, {
				{ K::Toggle, "interpolation_fill_gaps" }, { K::Toggle, "interpolation" }, { K::Toggle, "interpolation_fallback" },
				{ K::Toggle, "dejudder_estimate_ahead" } } },
			{ "motion", "render_ahead", "Render ahead (under test)", "Draws de-judder frames early to absorb timing hiccups.", false, false, {
				{ K::Toggle, "render_ahead" } } },
			{ "motion", "easing", "Per-shot easing", "Lowers the de-judder on shots that need less (never raises it).", false, false, {
				{ K::Toggle, "intent_motion" } } },
			{ "motion", "tier_keys", "Set by the Motion tier", "The motion features the Motion tier chooses: where the motion vectors come from, how far ahead they look, finer block splitting.", true, false, {
				{ K::Row, "optical_flow" }, { K::Row, "motion_ahead_frames" }, { K::Row, "dejudder_est_split" },
				{ K::Row, "dejudder_est_qpel" },
				{ K::Hide, "motion_source_heading" }, { K::Hide, "hiccup_repair_heading" }, { K::Hide, "interpolation_heading" },
				{ K::Hide, "interpolation_note" } },
				{ L::Tier, "the Motion tier" } },
			{ "motion", "taste_keys", "Set by the de-judder amount", "The eight fields the de-judder amount slider sets.", true, false, {
				{ K::Row, "dejudder_strength" }, { K::Row, "dejudder_strength_even" }, { K::Row, "dejudder_even_rates" },
				{ K::Row, "dejudder_object_strength" }, { K::Row, "dejudder_motion_rolloff" }, { K::Row, "dejudder_roll_floor" },
				{ K::Row, "dejudder_roll_lo" }, { K::Row, "dejudder_roll_hi" } },
				{ L::Slider, "the de-judder amount", "motion_preset_name", "motion_preset_reset" } },
			{ "motion", "shaping", "De-judder shaping (yours; the slider does not set these)",
				"Fade matching, pulldown handling and the motion-vector options: independent of the amount.", true, false, {
				{ K::Section, "dejudder" } } },
			{ "motion", "render_ahead_tuning", "Render ahead: tuning", "How render ahead budgets its early frames. Used only while render ahead is on.", true, false, {
				{ K::Row, "dejudder_gate" }, { K::Row, "render_ahead_utilisation" }, { K::Row, "render_ahead_queue_credit" } },
				{ L::Feature, "Render ahead", "render_ahead", nullptr, "off", "off" } },
			{ "motion", "easing_tuning", "Per-shot easing: tuning", "How strongly and how quickly the de-judder eases per shot. Used only while easing is on.", true, false, {
				{ K::Row, "intent_motion_policy" }, { K::Row, "intent_motion_min" }, { K::Row, "intent_motion_min_shot_s" },
				{ K::Row, "intent_ease_s" } },
				{ L::Feature, "Per-shot easing", "intent_motion", nullptr, "off", "off" } },
			{ "motion", "internals", "Motion estimator (measured, leave alone)",
				"The estimator's measured internals. Normal viewing never needs these.", true, true, {
				{ K::Section, "dejudderEstimation" } } },

			// ---- Repair ----
			{ "repair", "deband", "Deband",
				"VP's debander: removes banding from compressed sources and leaves film grain alone.", false, false, {
				{ K::Toggle, "deband_vp_grain_guard" }, { K::Toggle, "deband_vp_exclusive" }, { K::Toggle, "low_bitrate_handling" },
				{ K::Note, "libplacebo's own debander (Deband strength) is Bill's: VP Renderer > Rendering > Processing. "
					"With VP's debander on it is replaced (the switch above)." } },
				{ L::Feature, "VP's debander (the Repair tier, Mid or above)", "deband_vp", nullptr, "off", "off" } },
			{ "repair", "tier_keys", "Set by the Repair tier", "The repair stages the Repair tier switches on: VP's debander, the detail-pulse steadier and texture restore.", true, false, {
				{ K::Row, "deband_vp" }, { K::Row, "detail_pulse_stabilise" }, { K::Row, "texture_restore" } },
				{ L::Tier, "the Repair tier" } },
			{ "repair", "limits", "Texture restore and detail pulse: limits",
				"Each limit is used only while its stage is on (greyed otherwise).", true, false, {
				{ K::Row, "detail_pulse_cap" }, { K::Row, "texture_restore_cap" } } },

			// ---- Scaling ----
			{ "scaling", "rebase", "Re-base (your picture slider)",
				"Finds a picture that was upscaled before VP (a 1080p film sent at 4K) and works from its real size. The tier "
				"turns re-base on (Low and above); the slider sets how much evidence it needs.", false, false, {
				{ K::Row, "picture_preset_box" }, { K::Toggle, "rebase_kernel_match" }, { K::Toggle, "source_resolution_text_guard" },
				{ K::Toggle, "source_resolution_mixed_guard" } },
				{ L::Feature, "re-base (the Scaling tier, Low or above)", "source_resolution", nullptr, "!auto", "auto" } },
			{ "scaling", "grain", "Grain-aware downscale", "VP's downscale blend that keeps film grain true.", false, false, {
				{ K::Toggle, "adaptive_faithful" }, { K::Toggle, "adaptive_grain_map" }, { K::Toggle, "ssim_bitrate_aware" } },
				{ L::Feature, "the grain-aware downscale (the Scaling tier, Mid)", "adaptive_downscale", nullptr, "off", "off" } },
			{ "scaling", "upscale", "Grain-aware upscale", "For sources smaller than the projector.", false, false, {
				{ K::Toggle, "adaptive_upscale" },
				{ K::Note, "The scaler filters themselves (upscaler, downscaler, chroma) are Bill's: VP Renderer > Scaling." } } },
			{ "scaling", "tier_keys", "Set by the Scaling tier", "What the Scaling tier switches on: re-base (working from the real source size) and the grain-aware downscale.", true, false, {
				{ K::Row, "source_resolution" }, { K::Row, "adaptive_downscale" }, { K::Row, "adaptive_w_base" } },
				{ L::Tier, "the Scaling tier" } },
			{ "scaling", "slider_keys", "Re-base evidence", "The values the picture slider sets.", true, false, {
				{ K::Row, "source_resolution_auto_min_confidence" }, { K::Row, "source_resolution_evidence_seconds" },
				{ K::Row, "source_resolution_evidence_shots" } },
				{ L::Slider, "the picture slider", "picture_preset_name", "picture_preset_reset" } },
			{ "scaling", "rebase_timing", "Re-base timing", "How quickly re-base engages and fades.", true, false, {
				{ K::Row, "source_resolution_engage_max_wait_s" }, { K::Row, "source_resolution_crossfade_s" },
				{ K::Row, "source_resolution_release_fade_s" } },
				{ L::Feature, "re-base (the Scaling tier, Low or above)", "source_resolution", nullptr, "!auto", "auto" } },
			{ "scaling", "steadiness", "Scaler steadiness (expert)",
				"Keeps the SSIM and CNN scalers' added detail steady along motion. Normal viewing never needs these.", true, false, {
				{ K::Toggle, "ssim_residual_stabilise" }, { K::Toggle, "cnn_residual_stabilise" } } },
			{ "scaling", "internals", "Grain-aware scaler thresholds (measured, leave alone)",
				"Measured thresholds and weights. Normal viewing never needs these.", true, true, {
				{ K::Section, "adaptiveDownscale" }, { K::Section, "adaptiveUpscale" } } },

			// ---- Colour ----
			{ "colour", "dv", "Dolby Vision", "How Dolby Vision pictures are mapped to your display.", false, false, {
				{ K::Toggle, "dv_trims" }, { K::Row, "dv_tone_mapping" } } },
			{ "colour", "range", "Range and precision", "Keeps every value the source carries.", false, false, {
				{ K::Toggle, "preserve_extended_range" }, { K::Elsewhere, "fbo_float32" },
				{ K::Note, "Full precision (32-bit float intermediates) is chosen once for every tier, under the global tier at the top of this page." } } },
			{ "colour", "sdr", "SDR tone shape", "A gentle highlight knee for SDR sources.", false, false, {
				{ K::Toggle, "sdr_tone_shape" } } },
			{ "colour", "room", "Room-light adaptation (experimental)",
				"Adjusts black and contrast to the room light the sensor measures.", false, false, {
				{ K::Toggle, "ambient_adapt" } } },
			{ "colour", "upstream", "Tone mapping and display (Bill's pages)", "Where the colour settings that are not VP-V's live.", false, false, {
				{ K::Note, "Tone mapping, HDR target, gamut and output gamma: VP Renderer > Rendering (Tone mapping cards) and "
					"VP Renderer > Color / Output. The calibration LUT file: Color / Output; where it runs: the Pipeline (expert) tab." },
				{ K::Note, "Colour space, HDR luminance and video conversion of the INPUT are set once on General > Input "
					"processing. The VP Renderer and DirectShow pages can override them for one renderer; they say "
					"\"Inherited\" while they follow General." } } },
			{ "colour", "sdr_tuning", "SDR tone shape: values", "Where the highlight knee starts and how strong it is. Used only while SDR tone shape is on.", true, false, {
				{ K::Section, "sdrHighlights" } },
				{ L::Feature, "SDR tone shape", "sdr_tone_shape", nullptr, "off", "off" } },
			{ "colour", "room_tuning", "Room-light adaptation: values", "How strongly and how fast the picture follows the room light. Used only while room-light adaptation is on.", true, false, {
				{ K::Row, "ambient_adapt_strength" }, { K::Row, "ambient_screen_reflect" }, { K::Row, "ambient_max_black_nits" },
				{ K::Row, "ambient_tau_up_s" }, { K::Row, "ambient_tau_down_s" } },
				{ L::Feature, "Room-light adaptation", "ambient_adapt", nullptr, "off", "off" } },

			// ---- Dynamic Drive ----
			{ "drive", "pages", "Dynamic Drive (its own page)", "The projector LED dimming has its own page in the sidebar.", false, false, {
				{ K::Note, "Everything Dynamic Drive does is on the Dynamic Drive page. Its master is the command source "
					"(your script, VP's compensate, or none): each card appears only when that source uses it. The "
					"Advanced LED card has two more masters: \"Advanced LED values\" (your script's values, or Custom) "
					"and \"LED fixes under test\" (all on, or Custom); their settings are greyed until Custom." } } },

			// ---- Output ----
			{ "output", "dither", "Temporal dither", "The Output tier above is the dither choice: Classic = libplacebo's, Low = as shipped, Mid = always VP's motion-aware dither. This switch refines VP's.", false, false, {
				{ K::Toggle, "occlusion_mask" } },
				{ L::Feature, "VP's sigma-delta dither (the Output tier)", "temporal_dither", nullptr, "off,libplacebo", "auto" } },
			{ "output", "presentation", "Presentation and delay", "Where the final output depth and the total delay are set.", false, false, {
				{ K::Note, "Display bit depth and libplacebo's dithering (used when VP's is off) are Bill's: VP Renderer > "
					"Rendering > Processing. The total delay: Input tab (Total delay) and the Queue page." } } },
			{ "output", "tier_keys", "Set by the Output tier", "The dither the Output tier chooses: libplacebo's, or VP's motion-aware dither.", true, false, {
				{ K::Row, "temporal_dither" } },
				{ L::Tier, "the Output tier" } },
			{ "output", "dither_tuning", "Temporal dither: tuning", "How much grain the dither tolerates before it backs off.", true, false, {
				{ K::Row, "dither_grain_tolerance_scale" },
				{ K::Note, "It also follows the grain map (Scaling > Grain-aware downscale)." } },
				{ L::Feature, "VP's sigma-delta dither (the Output tier)", "temporal_dither", nullptr, "off,libplacebo", "auto" } },

			// ---- Proposed retirement: the candidates' controls (they still work until approved) ----
			{ "retirement", "fixes", "Proven fixes (all on by default)",
				"Each fix replaced older behaviour and has been on for weeks. Retiring them keeps them on.", false, true, {
				{ K::Row, "pulldown_fixes" }, { K::Row, "timing_continuity_fixes" }, { K::Row, "cadence_class_fixes" },
				{ K::Row, "motion_history_fixes" }, { K::Row, "queue_timing_fixes" }, { K::Row, "general_rate_fixes" },
				{ K::Row, "efficiency_fixes" }, { K::Row, "source_coding_fixes" }, { K::Row, "source_resolution_latch_fixes" },
				{ K::Row, "source_resolution_cut_fixes" }, { K::Row, "active_picture_startup_hold" },
				{ K::Row, "extended_range_more_cases" }, { K::Row, "dv_tunnel_fixes" }, { K::Row, "residual_stabilise_grain_guard" } },
				{ L::Defaults, "Proven fixes: all on", nullptr, nullptr, nullptr, "on" } },
			{ "retirement", "experiments", "Experiments that lost, and superseded switches",
				"Each still works as before until you decide.", false, true, {
				{ K::Row, "optical_flow_cost_trust" }, { K::Row, "interpolation_grain" }, { K::Row, "dejudder_source_warp_filter" },
				{ K::Row, "nls_sampling" }, { K::Row, "deband_grain" },
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

	// The Dynamic Drive page's Advanced LED card, regrouped (organisation pass 2). Every group is set
	// by the master "Advanced LED values" (your script's values, or Custom); a group a command source
	// does not use is greyed too (mode: "internal_script" / "compensate" / nullptr = both).
	// "Set by" for the Advanced LED groups: the LED profile (your script's built-in values, or a
	// profile file) and, once Custom is ticked, you.
	struct LedGroup { const char* id; const char* title; const char* help; const char* mode; std::vector<const char*> keys; };
	inline const std::vector<LedGroup>& LedAdvancedGroups()
	{
		static const std::vector<LedGroup> groups = {
			{ "tables", "Light curve, steps and ramps (your script's tables)", "Which LED level each picture brightness gets, and how big each LED step is.", nullptr,
				{ "curve", "steps_up_dark", "steps_up_bright", "steps_up_split_apl", "steps_down", "anti_pump_band", "apl_weights",
					"led_ceiling" } },
			{ "highlights", "Highlights", "What counts as a highlight that the LED must keep bright, and for how long.", nullptr,
				{ "highlight_percentile", "highlight_persist_ms", "highlight_area_small", "highlight_area_large", "highlight_release_ms" } },
			{ "cuts", "Cuts and fades", "How a scene cut or a fade is recognised and how the LED follows it.", nullptr, { "cut_jump", "cut_threshold", "fade_ms", "fade_min_slope" } },
			{ "script", "Script mode only (VP's copy of your script)", "Timing of VP's copy of your dimming script.", "internal_script", { "tick_ms", "script_highlight_code" } },
			{ "compensate", "Compensate mode only", "How VP's own controller holds the LED steady and treats bright cuts.", "compensate",
				{ "deadband", "knee", "apl_persist_ms", "early_ramp", "bright_cut_dump", "ftb_black_ms" } },
		};
		return groups;
	}
	// The LED switches proposed for retirement (group A): one master "LED fixes under test".
	inline const std::vector<const char*>& LedFixKeys()
	{
		static const std::vector<const char*> keys = { "led_peak_full_coverage", "led_picture_only", "script_resend_failed_writes",
			"correction_weight_by_time", "preposition_black_only", "led_landing_ms", "led_wtw_passthrough", "led_btb_passthrough",
			"cntr_pair_predict" };
		return keys;
	}
}
