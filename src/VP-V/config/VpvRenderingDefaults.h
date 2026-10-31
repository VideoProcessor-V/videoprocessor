// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// Config editor: what an unset VP-V key in a Rendering profile shows. Moved out of Bill's
// ConfigEditorWindow.cpp in the v1.4.02 merge (rehearsal c-list C4); the editor keeps one call
// site after Bill's RenderingBuiltInDefault. Qt only.

#include <QByteArray>
#include <QString>

#include <algorithm>
#include <cstring>
#include <iterator>
#include <utility>

namespace Vpv
{
// The Choice keys VP-V's editor rows add to a profile page (Bill's v1.4.02 pages have none of them).
// Bill's profile-page rules for his own choice keys - the "Inherited / not set" item is removed,
// a saved `auto` is a retired value - must not reach these: for VP-V an explicit `auto` is a live
// value and its rows keep their own inherit / Not set behaviour. Sorted (binary search).
inline bool IsVpvChoiceKey(const QString& key)
{
    static const char* const keys[] = {
        "active_picture_startup_hold",
        "adaptive_chroma_ssim",
        "adaptive_downscale",
        "adaptive_faithful",
        "adaptive_grain_map",
        "adaptive_motion",
        "adaptive_upscale",
        "adaptive_upscale_superres_scale",
        "cadence_class_fixes",
        "capture_path",
        "capture_upload",
        "cnn_residual_stabilise",
        "deband_grain",
        "deband_vp",
        "deband_vp_exclusive",
        "deband_vp_grain_guard",
        "dejudder_block_taper",
        "dejudder_est_candidates",
        "dejudder_est_gate",
        "dejudder_est_qpel",
        "dejudder_est_split",
        "dejudder_est_still_zero",
        "dejudder_estimate_ahead",
        "dejudder_even_rates",
        "dejudder_fade_match",
        "dejudder_gate",
        "dejudder_global_prior",
        "dejudder_global_two",
        "dejudder_landing_check",
        "dejudder_low_res",
        "dejudder_motion_rolloff",
        "dejudder_occlusion_fill",
        "dejudder_pixel_trust",
        "dejudder_pulldown",
        "dejudder_regional_hold",
        "dejudder_source_warp_filter",
        "dejudder_vector_smooth",
        "dejudder_verify",
        "dejudder_warp_stage",
        "detail_pulse_fast",
        "detail_pulse_stabilise",
        "dither_vectors",
        "downscale_light",
        "dv_tone_mapping",
        "dv_trims",
        "dv_tunnel_fixes",
        "dv_tunnel_gpu_unpack",
        "efficiency_fixes",
        "extended_range_more_cases",
        "fbo_float32",
        "general_rate_fixes",
        "intent_content",
        "intent_motion_policy",
        "interpolation",
        "interpolation_fallback",
        "interpolation_fill_gaps",
        "interpolation_grain",
        "low_bitrate_handling",
        "low_latency_path",
        "motion_ahead_frames",
        "motion_engine",
        "motion_history_fixes",
        "native_rgb_ingest",
        "nls_sampling",
        "occlusion_mask",
        "optical_flow",
        "optical_flow_cost_trust",
        "optical_flow_fb_check",
        "optical_flow_global",
        "optical_flow_global_fallback",
        "preserve_extended_range",
        "pulldown_fixes",
        "queue_timing_fixes",
        "rebase_kernel_match",
        "render_ahead",
        "render_ahead_queue_credit",
        "repeat_render_reuse",
        "residual_stabilise_grain_guard",
        "sdr_tone_shape",
        "source_class",
        "source_coding_fixes",
        "source_range",
        "source_resolution",
        "source_resolution_cut_fixes",
        "source_resolution_latch_fixes",
        "source_resolution_mixed_guard",
        "source_resolution_text_guard",
        "ssim_bitrate_aware",
        "ssim_residual_stabilise",
        "temporal_dither",
        "texture_restore",
        "timing_continuity_fixes",
        "upscale_light",
        "vector_demand",
    };
    const QByteArray latin = key.toLatin1();
    return std::binary_search(std::begin(keys), std::end(keys), latin.constData(),
        [](const char* a, const char* b) { return std::strcmp(a, b) < 0; });
}

// VP-V hook for the Rendering profile's unset-key defaults: what an unset VP-V
// key shows (the renderer's own defaults, so it never says "Auto" wrongly).
// Returns true when `key` is a VP-V key; Bill's RenderingBuiltInDefault covers
// the rest. Merge v1.4.02: this was the tail of the [vprenderer] default block.
inline bool VpvRenderingDefault(const QString& key, QString& out)
{
    if (key == QStringLiteral("saturation")) { out = QStringLiteral("0"); return true; }
        // The renderer's own defaults (LibplaceboVideoRenderer.cpp's
        // settings, AdaptiveDownscale.h), so an unset choice says what
        // it really is (audit 2026-10-14c: these all showed "Auto").
        static const std::pair<const char*, const char*> choiceDefaults[] = {
            { "capture_path", "gpu" }, { "capture_upload", "auto" }, { "source_range", "auto" }, { "source_resolution", "auto" }, { "deband_vp", "off" }, { "low_bitrate_handling", "auto" }, { "cnn_residual_stabilise", "on" }, { "ssim_residual_stabilise", "on" }, { "ssim_bitrate_aware", "on" }, { "optical_flow", "on" }, { "optical_flow_global", "on" }, { "rebase_kernel_match", "on" }, { "source_resolution_text_guard", "on" }, { "dejudder_even_rates", "whole" }, { "occlusion_mask", "off" },
            { "dejudder_warp_stage", "rendered" }, { "dejudder_global_prior", "on" },
            { "dejudder_vector_smooth", "on" }, { "dejudder_occlusion_fill", "off" },
            { "dejudder_est_candidates", "on" }, { "dejudder_est_qpel", "on" }, { "dejudder_est_still_zero", "on" }, { "dejudder_est_gate", "on" },
            { "dejudder_est_split", "off" }, { "dejudder_motion_rolloff", "off" }, { "dejudder_global_two", "on" },
            { "dejudder_regional_hold", "on" }, { "dejudder_pixel_trust", "on" }, { "dejudder_verify", "off" },
            { "dejudder_landing_check", "on" }, { "native_rgb_ingest", "on" }, { "dv_tunnel_gpu_unpack", "on" }, { "dv_trims", "on" }, { "dv_tone_mapping", "libplacebo" }, { "dejudder_pulldown", "off" },
            { "dejudder_fade_match", "on" }, { "dejudder_low_res", "all" }, { "dejudder_estimate_ahead", "on" },
            { "dejudder_block_taper", "off" }, { "dither_vectors", "auto" }, { "adaptive_downscale", "off" },
            { "adaptive_chroma_ssim", "off" }, { "adaptive_faithful", "auto" }, { "adaptive_grain_map", "on" },
            { "adaptive_upscale", "off" }, { "adaptive_upscale_superres_scale", "on" }, { "upscale_light", "linear" },
            { "downscale_light", "linear" }, { "preserve_extended_range", "on" }, { "extended_range_more_cases", "on" }, { "sdr_tone_shape", "off" },
            { "motion_ahead_frames", "4" }, { "source_resolution_mixed_guard", "on" }, { "source_resolution_latch_fixes", "on" }, { "source_resolution_cut_fixes", "on" }, { "active_picture_startup_hold", "on" }, { "pulldown_fixes", "on" }, { "timing_continuity_fixes", "on" }, { "cadence_class_fixes", "on" }, { "motion_history_fixes", "on" }, { "vector_demand", "on" }, { "queue_timing_fixes", "on" }, { "general_rate_fixes", "on" }, { "efficiency_fixes", "on" },
            // Render-ahead (docs/VP-render-ahead-design.md).
            { "render_ahead", "off" }, { "dejudder_gate", "auto" }, { "render_ahead_queue_credit", "off" },
            // 2026-10-17: the engine's extras, the grain switches, the DV tunnel restore switch.
            { "optical_flow_global_fallback", "on" }, { "optical_flow_cost_trust", "off" }, { "optical_flow_fb_check", "off" },
            { "interpolation_grain", "preserve" }, { "dejudder_source_warp_filter", "catmull" },
            { "residual_stabilise_grain_guard", "on" }, { "deband_vp_grain_guard", "on" }, { "deband_grain", "off" }, { "deband_vp_exclusive", "on" },
            { "dv_tunnel_fixes", "on" },
            // Hunt 2 M1: the NLS warp's sampling.
            { "nls_sampling", "catmull" }, { "source_coding_fixes", "on" },
            // VP's 50/60p interpolation (owner defaults 2026-10-16).
            { "interpolation", "off" }, { "interpolation_fallback", "hold" }, { "interpolation_fill_gaps", "on" },
            { "source_class", "legacy" },   // audit 2026-10-16: showed "Default: Auto" (the runtime's 4)
            // Content recognition (intent interpreter item 1, 2026-10-17).
            { "intent_content", "off" },
            // Detail pulse stabilisation and texture restore (docs/VP-detail-restore.md).
            { "detail_pulse_stabilise", "on" }, { "detail_pulse_fast", "on" }, { "texture_restore", "off" },
            // Intent remainder (2026-10-17): all experimental, default off.
            { "intent_motion", "off" }, { "intent_motion_policy", "display" }, { "ambient_adapt", "off" },
            { "intent_black", "off" }, { "intent_display", "off" } };
        for (const auto& entry : choiceDefaults)
            if (key == QLatin1String(entry.first)) { out = QString::fromLatin1(entry.second); return true; }
        // Numbers with a placeholder showing their default: empty, so
        // the placeholder (not "Auto") is what an unset field shows.
        if ((key.startsWith(QStringLiteral("dejudder_")) || key.startsWith(QStringLiteral("dither_")) ||
                key.startsWith(QStringLiteral("adaptive_")) || key == QStringLiteral("sdr_white_headroom") ||
                key == QStringLiteral("sdr_knee_start") || key == QStringLiteral("sdr_knee_strength") ||
                key == QStringLiteral("ssim_bitrate_floor") || key == QStringLiteral("render_ahead_utilisation") ||
                key.startsWith(QStringLiteral("source_resolution_auto_")) ||
                key.startsWith(QStringLiteral("source_resolution_evidence_")) ||
                // Hunt 3 (CFG2): the latch timing fields show 20 / 1.5 / 0.25.
                key == QStringLiteral("source_resolution_engage_max_wait_s") ||
                key == QStringLiteral("source_resolution_crossfade_s") ||
                key == QStringLiteral("source_resolution_release_fade_s") ||
                key == QStringLiteral("intent_content_hold_seconds") ||
                key == QStringLiteral("detail_pulse_cap") || key == QStringLiteral("texture_restore_cap") ||
                key.startsWith(QStringLiteral("intent_motion_")) || key == QStringLiteral("intent_ease_s") ||
                key.startsWith(QStringLiteral("ambient_"))) &&
            key != QStringLiteral("dejudder_preset"))
        { out = QString(); return true; }
    return false;
}
}
