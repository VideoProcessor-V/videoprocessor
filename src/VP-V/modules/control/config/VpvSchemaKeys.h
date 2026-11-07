// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// The VP-V renderer profile keys and the [vpv] section check, moved out of RendererProfileConfig.h (M2 P3-7).
// ProfileSchemaHooks.h includes this header INSIDE namespace RendererProfileConfig when the build defines
// PROFILE_SCHEMA_EXTENSION_HEADER (Directory.Build.targets), after the Is*/Parse* helpers it uses; the host
// asks it only about a key its own validators rejected (ValidateProfileSetting) or does not know
// (ValidateBaseSetting). Each group answers exactly the keys the old if-chains of that group answered, so
// the per-group result of every key is unchanged (src/VP-V/tests/golden/schema-validation.golden.txt).
// The host also keeps RetiredSettings.h included (the input group answers a retired key with "valid").
//
// Group of a key (R2 GroupOf): input = tone/colour/de-judder/optical-flow/intent/render-ahead/fixes/adaptive
// keys, motion = motion_engine, scaling = chroma_upscale_vp, downscale_light, upscale_light, deband_grain,
// viewport = blank_*, queue = target_latency_ms, low_latency_path, base (outside any group) = the pipeline
// keys and the retirement decisions. Known global keys of [vpv] are listed in IsKnownGlobalKey.

inline bool ValidateBaseSetting(const std::string& key, const std::string& value);

namespace ProfileSchemaHooks
{
	enum class Verdict { NotMine, Valid, Invalid };

	namespace Detail
	{
		inline Verdict V(bool valid) { return valid ? Verdict::Valid : Verdict::Invalid; }

		inline Verdict Input(const std::string& key, const std::string& value)
		{
			// Pruning 2026-10-17: a retired key loads with any value and is ignored (logged by the renderer).
			if (RetiredSettings::FindKey(key)) return Verdict::Valid;
			if (key == "saturation")
			{
				int parsed = 0; return V(ParseInteger(value, -100, 100, parsed));
			}
			// Stage 6: motion-compensated de-judder (0 = off, today's path) and
			// its own match tolerance (mean luma difference, 8-bit codes).
			if (key == "dejudder_strength") return V(IsNumberInRange(value, 0.0, 1.0));
			// Delta 10: even (2:2) cadences, and which rates take it.
			if (key == "dejudder_strength_even") return V(IsNumberInRange(value, 0.0, 1.0));
			// "30" / "30_and_29.97": retired spellings, still loaded (RetiredSettings migrates them).
			if (key == "dejudder_even_rates") return V(IsChoice(value, { "whole", "whole_and_1001", "30", "30_and_29.97" }));
			if (key == "dejudder_match_tolerance") return V(IsNumberInRange(value, 0.5, 16.0));
			// 6b-2: below this share of trusted blocks a frame is held whole
			// (a cut between A and B).
			if (key == "dejudder_frame_trust") return V(IsNumberInRange(value, 0.0, 1.0));
			// 6c: the de-judder's occlusion mask for the sigma-delta (works
			// with dejudder_strength 0: mask-only mode).
			if (key == "occlusion_mask") return V(IsChoice(value, { "on", "off" }));
			// WP-24: captured repeats drawn from the rendered-picture cache (OUTPUT stage only).
			if (key == "repeat_render_reuse") return V(IsChoice(value, { "on", "off" }));
			// 6d: the extras warp A's rendered picture, or a warped source.
			if (key == "dejudder_warp_stage") return V(IsChoice(value, { "rendered", "source" }));
			// Refinements (docs/VP-build-plan-refinements.md): D1 global prior,
			// D3 vector smoothing, D5 occlusion fill; D2 motion-scaled strength
			// (source px per source frame, hi 0 = off); D4 trust-adaptive
			// strength (share at full strength, 0 = off); S1 the sigma-delta's
			// tolerance on grain (1 = off).
			if (key == "dejudder_global_prior" || key == "dejudder_vector_smooth" || key == "dejudder_occlusion_fill" ||
				key == "dejudder_est_candidates" || key == "dejudder_est_qpel" || key == "dejudder_est_still_zero" || key == "dejudder_est_gate" ||
				key == "dejudder_est_split" || key == "dejudder_block_taper" || key == "dejudder_estimate_ahead" ||
				key == "dejudder_motion_rolloff" || key == "dejudder_global_two" || key == "dejudder_regional_hold" ||
				key == "dejudder_fade_match" || key == "dejudder_verify" || key == "dejudder_pixel_trust" ||
				key == "dejudder_landing_check" || key == "dejudder_pulldown" || key == "native_rgb_ingest" ||
				key == "dv_tunnel_gpu_unpack" || key == "dv_trims" ||
				key == "dv_tunnel_fixes")   // restore switch 2026-10-17
				return V(IsChoice(value, { "on", "off" }));
			// Motion fixes (owner 2026-10-13 night): M1 texture tolerance, M3's
			// cut test, the coarse search range, the forward/backward agreement.
			if (key == "dejudder_texture_tolerance") return V(IsNumberInRange(value, 0.0, 4.0));
			if (key == "dejudder_cut_histogram") return V(IsNumberInRange(value, 0.0, 2.0));
			if (key == "dejudder_search_range") { int n = 0; return V(ParseInteger(value, 1, 16, n)); }
			if (key == "dejudder_consistency_px") return V(IsNumberInRange(value, 0.5, 8.0));
			if (key == "dejudder_fade_floor") return V(IsNumberInRange(value, 0.0, 1.0));
			if (key == "dejudder_verify_share") return V(IsNumberInRange(value, 0.02, 1.0));
			if (key == "dejudder_pixel_trust_lo") return V(IsNumberInRange(value, 0.25, 16.0));
			if (key == "dejudder_pixel_trust_hi") return V(IsNumberInRange(value, 0.5, 32.0));
			// Stage 11a E1: the smoothness penalty (codes per source pixel).
			if (key == "dejudder_est_lambda") return V(IsNumberInRange(value, 0.0, 8.0));
			// Owner 2026-10-13: the de-judder matcher one level coarser (4K).
			if (key == "dejudder_low_res") return V(IsChoice(value, { "off", "search", "all" }));
			// Owner 2026-10-13: the capture path (gpu = the low-latency GPU unpack).
			if (key == "capture_path") return V(IsChoice(value, { "standard", "gpu" }));
			if (key == "capture_upload") return V(IsChoice(value, { "auto", "render", "early" }));
			if (key == "dv_tone_mapping") return V(IsChoice(value, { "libplacebo", "dolby_style" }));   // feat/dv-l8
			if (key == "dv_compare_capture_folder") return V(value.find('"') == std::string::npos);   // feat/dv-l8 WIP: a folder, "" = off
			if (key == "source_range") return V(IsChoice(value, { "auto", "full", "limited" }));
			// native | auto | any height in lines (general: not a fixed list).
			if (key == "source_resolution")
			{
				if (IsChoice(value, { "native", "auto" })) return Verdict::Valid;
				auto lines = [](const std::string& v, double hi) {
					return !v.empty() && v.find_first_not_of("0123456789") == std::string::npos && IsNumberInRange(v, 144, hi); };
				// A height, or WIDTHxHEIGHT (per axis: a source upscaled in one direction).
				const size_t x = value.find('x');
				return V(x == std::string::npos ? lines(value, 8640) : lines(value.substr(0, x), 17280) && lines(value.substr(x + 1), 8640));
			}
			if (key == "source_resolution_auto_min_confidence") return V(IsNumberInRange(value, 0.0, 1.0));
			// The Picture slider's evidence: whole shots 1..10, seconds 2..120.
			if (key == "source_resolution_evidence_shots")
				return V(!value.empty() && value.find_first_not_of("0123456789") == std::string::npos && IsNumberInRange(value, 1, 10));
			if (key == "source_resolution_evidence_seconds") return V(IsNumberInRange(value, 2.0, 120.0));
			if (key == "deband_vp") return V(IsChoice(value, { "off", "auto", "on" }));
			if (key == "low_bitrate_handling") return V(IsChoice(value, { "auto", "off", "on" }));
			if (key == "cnn_residual_stabilise" || key == "ssim_residual_stabilise" || key == "ssim_bitrate_aware") return V(IsChoice(value, { "on", "off" }));
			if (key == "ssim_bitrate_floor") return V(IsNumberInRange(value, 0.0, 1.0));
			// docs/VP-nvof-ahead.md (owner 2026-10-16): optical-flow frames ahead, 0..4 (whole frames).
			if (key == "motion_ahead_frames") return V(IsChoice(value, { "0", "1", "2", "3", "4" }));
			// The optical-flow engine: the default; off = the matcher only.
			if (key == "optical_flow") return V(IsChoice(value, { "on", "off" }));
			if (key == "optical_flow_global") return V(IsChoice(value, { "on", "off" }));
			// The engine's extras (2026-10-17): D1's fallback to the search, cost and forward/backward trust.
			if (key == "optical_flow_global_fallback" || key == "optical_flow_cost_trust" || key == "optical_flow_fb_check")
				return V(IsChoice(value, { "on", "off" }));
			// VP's 50/60p frame interpolation (docs/VP-interpolator-50-60-plan.md): off by default.
			if (key == "interpolation") return V(IsChoice(value, { "off", "auto", "always" }));
			if (key == "interpolation_fallback") return V(IsChoice(value, { "hold", "blend" }));
			if (key == "interpolation_fill_gaps") return V(IsChoice(value, { "off", "on" }));
			// Film grain (owner 2026-10-17; docs/audit/grain-audit-2026-10-17.md).
			if (key == "interpolation_grain") return V(IsChoice(value, { "preserve", "blend" }));
			if (key == "dejudder_source_warp_filter") return V(IsChoice(value, { "catmull", "bilinear" }));
			if (key == "nls_sampling") return V(IsChoice(value, { "catmull", "bilinear" }));   // hunt 2 M1
			if (key == "residual_stabilise_grain_guard") return V(IsChoice(value, { "on", "off" }));
			// on = the grain split (hunt 2 07-scaling H1), veto = the 2026-10-17 spatial-share veto.
			if (key == "deband_vp_grain_guard") return V(IsChoice(value, { "on", "veto", "off" }));
			// 15-intent C14: libplacebo's automatic deband off while VP's grain-guarded debander runs.
			if (key == "deband_vp_exclusive") return V(IsChoice(value, { "on", "off" }));
			// Detail pulse stabilisation and texture restore (docs/VP-detail-restore.md, 2026-10-17):
			// the caps are the module headers' ranges.
			if (key == "detail_pulse_stabilise") return V(IsChoice(value, { "on", "off" }));
			if (key == "detail_pulse_cap") return V(IsNumberInRange(value, 1.0, 1.5));
			if (key == "detail_pulse_fast") return V(IsChoice(value, { "on", "off" }));
			if (key == "texture_restore") return V(IsChoice(value, { "off", "statistical" }));
			if (key == "texture_restore_cap") return V(IsNumberInRange(value, 1.0, 2.0));
			if (key == "source_class") return V(IsChoice(value, { "legacy", "measured" }));
			// Content recognition (intent interpreter item 1, 2026-10-17): default off;
			// a new class is taken after this many seconds, at a cut.
			if (key == "intent_content") return V(IsChoice(value, { "off", "on" }));
			if (key == "intent_content_hold_seconds") return V(IsNumberInRange(value, 1.0, 60.0));
			// Intent remainder (2026-10-17): motion intent, ambient adaptation, black protection and
			// display mapping readout; all experimental, default off.
			if (key == "intent_motion") return V(IsChoice(value, { "off", "on" }));
			if (key == "intent_motion_policy") return V(IsChoice(value, { "display", "ease_fast", "ease_slow", "visibility", "metadata" }));
			if (key == "intent_motion_min") return V(IsNumberInRange(value, 0.0, 1.0));
			if (key == "intent_motion_min_shot_s") return V(IsNumberInRange(value, 0.5, 30.0));
			if (key == "intent_ease_s") return V(IsNumberInRange(value, 0.0, 5.0));
			if (key == "ambient_adapt") return V(IsChoice(value, { "off", "on" }));
			if (key == "ambient_adapt_strength") return V(IsNumberInRange(value, 0.0, 1.0));
			if (key == "ambient_screen_reflect") return V(IsNumberInRange(value, 0.0, 1.0));
			if (key == "ambient_max_black_nits") return V(IsNumberInRange(value, 0.0, 10.0));
			if (key == "ambient_tau_up_s") return V(IsNumberInRange(value, 1.0, 600.0));
			if (key == "ambient_tau_down_s") return V(IsNumberInRange(value, 1.0, 1800.0));
			if (key == "intent_black") return V(IsChoice(value, { "off", "report", "led_protect" }));
			if (key == "intent_display") return V(IsChoice(value, { "off", "report" }));
			if (key == "temporal_dither") return V(IsChoice(value, { "auto", "off", "libplacebo", "sigma_delta" }));
			if (key == "rebase_kernel_match") return V(IsChoice(value, { "on", "off" }));
			if (key == "source_resolution_text_guard") return V(IsChoice(value, { "on", "off" }));
			if (key == "source_resolution_mixed_guard") return V(IsChoice(value, { "on", "off" }));
			if (key == "source_coding_fixes") return V(IsChoice(value, { "on", "off" }));   // hunt 2 M2/M3
			// Latch review 2026-10-17: the latch fixes and the switch timing (seconds).
			if (key == "source_resolution_latch_fixes") return V(IsChoice(value, { "on", "off" }));
			if (key == "source_resolution_cut_fixes") return V(IsChoice(value, { "on", "off" }));
			if (key == "active_picture_startup_hold") return V(IsChoice(value, { "on", "off" }));
			if (key == "source_resolution_engage_max_wait_s") return V(IsNumberInRange(value, 0.0, 600.0));
			if (key == "source_resolution_crossfade_s") return V(IsNumberInRange(value, 0.0, 10.0));
			if (key == "source_resolution_release_fade_s") return V(IsNumberInRange(value, 0.0, 0.25));
			if (key == "pulldown_fixes") return V(IsChoice(value, { "on", "off" }));
			if (key == "timing_continuity_fixes") return V(IsChoice(value, { "on", "off" }));   // Bug hunt 2026-10-17
			if (key == "cadence_class_fixes") return V(IsChoice(value, { "on", "off" }));   // Hunt 2 2026-10-17
			if (key == "motion_history_fixes") return V(IsChoice(value, { "on", "off" }));   // Hunt 2 fix group 5
			if (key == "vector_demand") return V(IsChoice(value, { "on", "off" }));   // vector engine on demand
			if (key == "queue_timing_fixes") return V(IsChoice(value, { "on", "off" }));   // Hunt 2 group 5 (queue/cadence timing)
			if (key == "general_rate_fixes") return V(IsChoice(value, { "on", "off" }));   // fixed-constant batch 2026-10-18
			if (key == "efficiency_fixes") return V(IsChoice(value, { "on", "off" }));   // efficiency round 2026-10-18
			// Render-ahead (docs/VP-render-ahead-design.md, 2026-10-17): queued extras, the
			// de-judder's gate per source period, its GPU budget and the queue credit.
			if (key == "render_ahead" || key == "render_ahead_queue_credit") return V(IsChoice(value, { "on", "off" }));
			if (key == "dejudder_gate") return V(IsChoice(value, { "auto", "period", "refresh" }));
			if (key == "render_ahead_utilisation") return V(IsNumberInRange(value, 0.5, 1.0));
			// Stage 11a E2: the split threshold (0 = the match tolerance).
			if (key == "dejudder_est_split_sad") return V(IsNumberInRange(value, 0.0, 16.0));
			// Delta 14: R-A's roll-off (source px per source frame; floor as a
			// share of the strength) and R-B's object strength (1 = off).
			if (key == "dejudder_roll_lo") return V(IsNumberInRange(value, 0.0, 200.0));
			if (key == "dejudder_roll_hi") return V(IsNumberInRange(value, 0.0, 400.0));
			if (key == "dejudder_roll_floor" || key == "dejudder_object_strength") return V(IsNumberInRange(value, 0.0, 1.0));
			// Delta 9: H1 feather (output px, 0 = off), H2 hysteresis (source
			// frames, 1 = off), H3 taper end (codes, 0 = 2 x match tolerance).
			if (key == "dejudder_hold_feather_px") return V(IsNumberInRange(value, 0.0, 16.0));
			if (key == "dejudder_hold_hysteresis") { int n = 0; return V(ParseInteger(value, 1, 8, n)); }
			if (key == "dejudder_taper_sad") return V(IsNumberInRange(value, 0.0, 32.0));
			if (key == "dejudder_motion_scale_lo") return V(IsNumberInRange(value, 0.0, 16.0));
			if (key == "dejudder_motion_scale_hi") return V(IsNumberInRange(value, 0.0, 32.0));
			if (key == "dejudder_trust_scale") return V(IsNumberInRange(value, 0.0, 1.0));
			if (key == "dither_grain_tolerance_scale") return V(IsNumberInRange(value, 1.0, 8.0));
			if (key == "dither_vectors") return V(IsChoice(value, { "auto", "own" }));   // delta 8 R1
			// S2: every FBO in 32-bit float (auto: on, off with Low_Latency).
			if (key == "fbo_float32") return V(IsChoice(value, { "auto", "on", "off" }));
			// Stage 8-2: the adaptive downscale blend.
			if (key == "adaptive_downscale" || key == "adaptive_chroma_ssim" || key == "adaptive_grain_map" ||
				key == "adaptive_upscale" || key == "adaptive_upscale_superres_scale")
				return V(IsChoice(value, { "on", "off" }));
			if (key == "adaptive_upscale_w_base") return V(IsNumberInRange(value, 0.0, 1.0));
			// "polar": retired 2026-10-17 (runs as auto), still loaded.
			if (key == "adaptive_faithful") return V(IsChoice(value, { "polar", "ortho", "auto" }));
			if (key == "adaptive_w_base" || key == "adaptive_grain_floor" || key == "adaptive_grain_shadow_bias")
				return V(IsNumberInRange(value, 0.0, 1.0));
			if (key == "adaptive_edge_lo" || key == "adaptive_edge_hi") return V(IsNumberInRange(value, 0.0, 255.0));
			if (key == "adaptive_grain_lo" || key == "adaptive_grain_hi") return V(IsNumberInRange(value, 0.0, 64.0));
			if (key == "adaptive_grain_smooth_ms" || key == "adaptive_grain_map_smooth_ms")
				return V(IsNumberInRange(value, 0.0, 10000.0));
			// The motion-adapted blend (VP-V scaling.output, owner 2026-10-31; candidate, off).
			if (key == "adaptive_motion") return V(IsChoice(value, { "on", "off" }));
			if (key == "adaptive_motion_speed_lo" || key == "adaptive_motion_speed_hi") return V(IsNumberInRange(value, 0.0, 10.0));
			if (key == "adaptive_motion_floor") return V(IsNumberInRange(value, 0.0, 1.0));
			if (key == "adaptive_motion_smooth_ms") return V(IsNumberInRange(value, 0.0, 10000.0));
			// Stage 10: BTB/WTW kept through libplacebo's scaling (SDR).
			if (key == "preserve_extended_range") return V(IsChoice(value, { "on", "off" }));
			if (key == "extended_range_more_cases") return V(IsChoice(value, { "on", "off" }));
			// Delta 13: the SDR tone shape and headroom.
			if (key == "sdr_tone_shape") return V(IsChoice(value, { "off", "knee" }));
			if (key == "sdr_white_headroom") return V(IsNumberInRange(value, 0.0, 0.5));
			if (key == "sdr_knee_start") return V(IsNumberInRange(value, 0.5, 0.99));
			if (key == "sdr_knee_strength") return V(IsNumberInRange(value, 0.0, 1.0));
			if (key == "sdr_contrast_recovery") return V(IsChoice(value, { "auto", "off" }) || IsNumberInRange(value, 0.0, 2.0));
			return Verdict::NotMine;
		}

		inline Verdict Motion(const std::string& key, const std::string& value)
		{
            // VP's motion handling or libplacebo's smooth motion, one or the
            // other (owner 2026-10-15); auto = whichever is configured, VP's when both.
            if(key=="motion_engine")return V(IsChoice(value,{"auto","vp","smooth_motion"}));
			return Verdict::NotMine;
		}

		inline Verdict Scaling(const std::string& key, const std::string& value)
		{
			// VP-V scaling.chroma_upscale (2026-10-31; candidate, off until GPU-validated)
			if (key == "chroma_upscale_vp") return V(IsChoice(value, { "off", "cfl" }));
			// Stage 8-1: the light domain of downscaling frames.
			if (key == "downscale_light") return V(IsChoice(value, { "linear", "gamma" }));
			if (key == "upscale_light") return V(IsChoice(value, { "linear", "gamma" }));
			// Owner 2026-10-17 (grain audit): libplacebo's deband adds grain (off: none; VP's dither).
			if (key == "deband_grain") return V(IsChoice(value, { "on", "off" }));
			return Verdict::NotMine;
		}

		inline Verdict Viewport(const std::string& key, const std::string& value)
		{
			if ((key == "blank_left" || key == "blank_top" ||
				key == "blank_right" || key == "blank_bottom"))
			{
				int parsed = 0; return V(ParseInteger(value, 0, 4000, parsed));
			}
			return Verdict::NotMine;
		}

		inline Verdict Queue(const std::string& key, const std::string& value)
		{
			int parsed = 0;
			if (key == "target_latency_ms")
				return V(ParseInteger(value, 0, 1000, parsed));
			if (key == "low_latency_path") return V(IsChoice(value, { "on", "off" }));
			return Verdict::NotMine;
		}

		// Keys that belong to no group: the pipeline skeleton and the retirement decisions.
		inline Verdict Base(const std::string& key, const std::string& value)
		{
			// Pipeline skeleton (design freeze 2026-10-24): the Original pathway baseline (ADR-0019),
			// the module order (ADR-0021; slot names, validated by the renderer's registry, an invalid
			// order never runs), slot-level switches and where the calibration LUT runs (R1).
			if (key == "pipeline_original_pathway") return V(IsChoice(value, { "on", "off" }));
			if (key == "pipeline_entrance_once") return V(IsChoice(value, { "on", "off" }));   // B3 (ADR-0024)
			if (key == "frame_analysis_store") return V(IsChoice(value, { "on", "off" }));     // WP-23
			if (key == "resync_event") return V(IsChoice(value, { "off", "shadow", "on" }));   // R1 shadow; R3/L1/R4a on
			if (key == "entrance_gpu_drain") return V(IsChoice(value, { "off", "flush", "finish" }));   // expert workaround
			// Config usability (2026-10-25): the owner's Approve / Keep decision per retirement group (A-D; keys retirement_a .. retirement_d, names are case-insensitive), recorded by the VP-V page's
			// Proposed retirement tab. A UI-only record: nothing in the renderer reads it and nothing is hidden or removed by it.
			if (key == "retirement_a" || key == "retirement_b" || key == "retirement_c" || key == "retirement_d")
				return V(IsChoice(value, { "approve", "keep" }));
			if (key == "pipeline_order" || key == "pipeline_slots_off")
				return V(value.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789_.,;> ") == std::string::npos);
			if (key == "calibration_lut_stage") return V(IsChoice(value, { "libplacebo", "vp" }));
			if (key == "output_segments_merged") return V(IsChoice(value, { "on", "off" }));   // output segments S2/S3 (draft)
			if (key == "hdr_inference") return V(IsChoice(value, { "vp", "original" }));   // F-8 (HdrInferenceChoice.h)
			return Verdict::NotMine;
		}
	}

	// `group` is the host's profile group name; "base" asks about a key outside any group.
	inline Verdict Validate(const std::string& group, const std::string& key, const std::string& value)
	{
		if (group == "input") return Detail::Input(key, value);
		if (group == "motion") return Detail::Motion(key, value);
		if (group == "scaling") return Detail::Scaling(key, value);
		if (group == "viewport") return Detail::Viewport(key, value);
		if (group == "queue") return Detail::Queue(key, value);
		if (group == "base") return Detail::Base(key, value);
		return Verdict::NotMine;
	}

	// The global keys of the [vpv] section (ADR-0023): not per renderer profile, so Config's profile
	// migrations never move them.
	inline bool IsKnownGlobalKey(const std::string& section, const std::string& key)
	{
		if (section != "vpv") return false;
		static const char* const vpvKeys[] = { "pipeline_original_pathway", "pipeline_order",
			"pipeline_slots_off", "calibration_lut_stage", "pipeline_entrance_once", "output_segments_merged", "frame_analysis_store", "resync_event", "entrance_gpu_drain", "hdr_inference" };
		bool known = false;
		for (const char* candidate : vpvKeys) known = known || key == candidate;
		for (const char* candidate : { "retirement_a", "retirement_b", "retirement_c", "retirement_d" }) known = known || key == candidate;   // UI-only decisions
		return known;
	}

	// The [vpv] section: each key is validated like the same key in [vprenderer]; anything else there is an
	// error, as in every owned section.
	inline bool ValidateSections(const ConfigFile& config, std::string& error)
	{
		if (const auto* vpvValues = config.GetSectionValues("vpv"))
			for (const auto& value : *vpvValues)
			{
				if (!IsKnownGlobalKey("vpv", value.first))
				{
					error = "[vpv] unknown key '" + value.first + "'";
					return false;
				}
				if (!ValidateBaseSetting(value.first, value.second))
				{
					error = "[vpv] invalid " + value.first + " '" + value.second + "'";
					return false;
				}
			}
		return true;
	}
}
