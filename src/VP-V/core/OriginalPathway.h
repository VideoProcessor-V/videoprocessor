#pragma once

#include <array>

// The ORIGINAL PATHWAY baseline as data (ADR-0019; skeleton audit item 7, 2026-10-24).
//
// pipeline_original_pathway = on must reproduce Bill's pathway: every VP addition off and every
// slot that exists upstream in its upstream form. This table is the one list of what that means
// in renderer settings (verified against bill-v1.4.02: docs/design/original-pathway-verified.md).
// - The renderer's ApplyOriginalPathwaySettings sets exactly these keys; each of its lines carries
//   a "key=<name>" comment, and tools/check_module_manifests.py fails when the two disagree or
//   when docs/design/slots.json names a key that is not here.
// - The CPU test checks every key/value with the configuration validator (a key that does not
//   exist, or a value it refuses, fails), so the table cannot name non-keys.
// - The projector (RS-232 LED/CNTR drive) is not a renderer setting: in the Original pathway the
//   renderer does not start the LED drive at all (slot drive.projector_control is off).
//
// Header-only, C++14.
namespace OriginalPathway
{
	struct Override
	{
		const char* key;
		const char* value;
		const char* slot;   // the slot whose VP additions it removes (slots.json id)
	};

	inline const std::array<Override, 61>& Overrides()
	{
		static const std::array<Override, 61> table = { {
			{ "capture_path", "standard", "input.capture" },
			{ "capture_upload", "render", "input.capture" },
			{ "native_rgb_ingest", "off", "input.capture" },
			{ "dv_tunnel_gpu_unpack", "off", "input.signal" },
			{ "dv_tunnel_fixes", "off", "input.signal" },
			{ "dv_compare_capture_folder", "", "input.signal" },   // feat/dv-l8: the diagnostic off
			{ "dv_trims", "off", "colour.sdr_tone_shape" },
			{ "dv_tone_mapping", "libplacebo", "colour.sdr_tone_shape" },   // feat/dv-l8
			{ "queue_timing_fixes", "off", "input.queue_timing" },
			{ "pulldown_fixes", "off", "input.queue_timing" },
			{ "timing_continuity_fixes", "off", "input.queue_timing" },
			{ "cadence_class_fixes", "off", "input.queue_timing" },
			{ "general_rate_fixes", "off", "input.queue_timing" },
			{ "efficiency_fixes", "off", "input.capture" },
			{ "source_resolution", "native", "input.source_analysis" },
			{ "rebase_kernel_match", "off", "scaling.rebase" },
			{ "entrance_gpu_drain", "off", "scaling.rebase" },
			{ "source_coding_fixes", "off", "input.source_analysis" },
			{ "frame_analysis_store", "off", "input.source_analysis" },   // WP-23: Bill's own decodes only
			{ "source_resolution_latch_fixes", "off", "input.source_analysis" },
			{ "source_resolution_cut_fixes", "off", "input.source_analysis" },
			{ "deband_vp", "off", "repair.deband" },
			{ "deband_vp_exclusive", "off", "repair.deband" },
			{ "detail_pulse_stabilise", "off", "repair.texture_restore" },
			{ "texture_restore", "off", "repair.texture_restore" },
			{ "dejudder_strength", "0", "motion.consumers" },
			{ "dejudder_strength_even", "0", "motion.consumers" },
			{ "interpolation", "off", "motion.consumers" },
			{ "interpolation_fill_gaps", "off", "motion.consumers" },
			{ "occlusion_mask", "off", "motion.consumers" },
			{ "repeat_render_reuse", "off", "output.presentation" },
			{ "optical_flow", "off", "motion.estimation" },
			{ "motion_ahead_frames", "0", "motion.estimation" },
			{ "motion_history_fixes", "off", "motion.estimation" },
			{ "dejudder_source_warp_filter", "bilinear", "motion.consumers" },
			{ "nls_sampling", "bilinear", "scaling.geometry" },
			{ "adaptive_downscale", "off", "scaling.output" },
			{ "adaptive_upscale", "off", "scaling.output" },
			{ "ssim_bitrate_aware", "off", "scaling.output" },
			{ "downscale_light", "linear", "scaling.output" },
			{ "fbo_float32", "off", "scaling.output" },
			{ "preserve_extended_range", "off", "colour.extended_range" },
			{ "extended_range_more_cases", "off", "colour.extended_range" },
			{ "sdr_tone_shape", "off", "colour.sdr_tone_shape" },
			{ "intent_content", "off", "control.config" },
			{ "intent_motion", "off", "control.config" },
			{ "intent_display", "off", "control.config" },
			{ "intent_black", "off", "control.config" },
			{ "ambient_adapt", "off", "control.config" },
			{ "temporal_dither", "libplacebo", "output.dither" },
			{ "output_segments_merged", "off", "output.dither" },   // VP's merged output hooks: never in Bill's pathway
			{ "render_ahead", "off", "output.presentation" },
			{ "calibration_lut_stage", "libplacebo", "output.calibration_lut" },
			// Settings audit 2026-10-24: VP additions the first table missed (absent from Bill's v1.4.02-beta).
			{ "active_picture_startup_hold", "off", "input.source_analysis" },
			{ "low_bitrate_handling", "off", "repair.deband" },
			{ "ssim_residual_stabilise", "off", "scaling.output" },
			{ "cnn_residual_stabilise", "off", "scaling.output" },
			{ "residual_stabilise_grain_guard", "off", "scaling.output" },
			{ "dejudder_estimate_ahead", "off", "motion.estimation" },
			{ "dejudder_est_still_zero", "off", "motion.estimation" },
			{ "interpolation_fallback", "hold", "motion.consumers" },
		} };
		return table;
	}
}
