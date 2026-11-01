// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <VP-V/core/Indicator.h>
#include <VP-V/core/ResyncCause.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iterator>
#include <string>
#include <vector>

// Settings stamps (Q0-A): which settings domains changed between two applies. Each domain keeps a
// stable 64-bit FNV-1a hash of its keys' current values (sorted by key, so the order of the key list
// never matters; a MISSING key and an EMPTY value hash differently). Configure compares the hashes
// with the previous apply: the first Configure only RECORDS; later ones Advance
// kSettings<domain> (Cause::SettingsApply) for exactly the domains whose hash changed.
// INERT until a settings-apply call site uses it. Header-only, pure C++14, deterministic.
//
// Default key lists (copied from the Keys: lines of the modules whose products read each stamp,
// core/Products.h: P2/P2m motion; P3 motion + analysis; P4/P5 analysis; P0/P6 rebase + analysis):
//
//   domain    counter            keys (module README)
//   motion    kSettingsMotion    modules/motion/estimation + vector_delivery:
//                                dejudder_consistency_px dejudder_cut_histogram dejudder_est_candidates
//                                dejudder_est_gate dejudder_est_lambda dejudder_est_qpel dejudder_est_split
//                                dejudder_est_split_sad dejudder_estimate_ahead dejudder_fade_floor
//                                dejudder_fade_match dejudder_global_prior dejudder_global_two dejudder_low_res
//                                dejudder_match_tolerance dejudder_regional_hold dejudder_search_range
//                                dejudder_texture_tolerance dejudder_vector_smooth dejudder_verify
//                                dejudder_verify_share general_rate_fixes motion_ahead_frames motion_engine
//                                motion_history_fixes optical_flow optical_flow_cost_trust
//                                optical_flow_fb_check optical_flow_global optical_flow_global_fallback
//                                vector_demand dejudder_even_rates dejudder_strength dejudder_strength_even
//                                occlusion_mask timing_continuity_fixes
//   rebase    kSettingsRebase    modules/scaling/rebase: source_resolution rebase_kernel_match
//                                entrance_gpu_drain
//   analysis  kSettingsAnalysis  modules/input/source_analysis: source_resolution
//                                source_resolution_auto_min_confidence source_resolution_evidence_shots
//                                source_resolution_evidence_seconds source_resolution_text_guard
//                                source_resolution_mixed_guard source_resolution_latch_fixes
//                                source_resolution_cut_fixes source_resolution_engage_max_wait_s
//                                source_resolution_crossfade_s source_resolution_release_fade_s subtitle_fit
//                                subtitle_reposition subtitle_type source_coding_fixes frame_analysis_store
//                                (source_resolution is read by both rebase and analysis: it moves both.)
//   output    kSettingsOutput    modules/output/presentation + dither + calibration_lut and
//                                modules/scaling/output: dejudder_gate render_ahead
//                                render_ahead_queue_credit render_ahead_utilisation repeat_render_reuse
//                                temporal_dither dithering dither_vectors dither_grain_tolerance_scale
//                                output_segments_merged calibration_lut_bt2020 calibration_lut_bt709
//                                calibration_lut_enabled calibration_lut_input_transfer calibration_lut_p3_d65
//                                calibration_lut_stage adaptive_downscale adaptive_w_base adaptive_edge_lo
//                                adaptive_edge_hi adaptive_grain_lo adaptive_grain_hi adaptive_grain_floor
//                                adaptive_grain_smooth_ms adaptive_grain_shadow_bias adaptive_chroma_ssim
//                                adaptive_grain_map adaptive_grain_map_smooth_ms adaptive_faithful
//                                adaptive_upscale adaptive_upscale_w_base adaptive_upscale_superres_scale
//                                downscale_light upscale_light ssim_bitrate_aware ssim_bitrate_floor
//                                cnn_residual_stabilise ssim_residual_stabilise residual_stabilise_grain_guard
//                                fbo_float32 adaptive_motion adaptive_motion_speed_lo adaptive_motion_speed_hi
//                                adaptive_motion_floor adaptive_motion_smooth_ms
namespace VpResync {

class SettingsStamp {
public:
    enum class Domain : uint8_t { Motion, Rebase, Analysis, Output, Count };
    static constexpr size_t kDomains = static_cast<size_t>(Domain::Count);

    struct Result {
        bool first = false;                     // at least one domain was only recorded (no Advance)
        VpIndicator::CounterMask advanced = 0;  // the settings counters advanced by this call
    };

    using Getter = std::function<bool(const std::string& key, std::string& value)>;

    SettingsStamp()
    {
        static const char* const motion[] = {
            "dejudder_consistency_px", "dejudder_cut_histogram", "dejudder_est_candidates", "dejudder_est_gate",
            "dejudder_est_lambda", "dejudder_est_qpel", "dejudder_est_split", "dejudder_est_split_sad",
            "dejudder_estimate_ahead", "dejudder_fade_floor", "dejudder_fade_match", "dejudder_global_prior",
            "dejudder_global_two", "dejudder_low_res", "dejudder_match_tolerance", "dejudder_regional_hold",
            "dejudder_search_range", "dejudder_texture_tolerance", "dejudder_vector_smooth", "dejudder_verify",
            "dejudder_verify_share", "general_rate_fixes", "motion_ahead_frames", "motion_engine",
            "motion_history_fixes", "optical_flow", "optical_flow_cost_trust", "optical_flow_fb_check",
            "optical_flow_global", "optical_flow_global_fallback", "vector_demand", "dejudder_even_rates",
            "dejudder_strength", "dejudder_strength_even", "occlusion_mask", "timing_continuity_fixes" };
        static const char* const rebase[] = { "source_resolution", "rebase_kernel_match", "entrance_gpu_drain" };
        static const char* const analysis[] = {
            "source_resolution", "source_resolution_auto_min_confidence", "source_resolution_evidence_shots",
            "source_resolution_evidence_seconds", "source_resolution_text_guard", "source_resolution_mixed_guard",
            "source_resolution_latch_fixes", "source_resolution_cut_fixes", "source_resolution_engage_max_wait_s",
            "source_resolution_crossfade_s", "source_resolution_release_fade_s", "subtitle_fit",
            "subtitle_reposition", "subtitle_type", "source_coding_fixes", "frame_analysis_store" };
        static const char* const output[] = {
            "dejudder_gate", "render_ahead", "render_ahead_queue_credit", "render_ahead_utilisation",
            "repeat_render_reuse", "temporal_dither", "dithering", "dither_vectors",
            "dither_grain_tolerance_scale", "output_segments_merged", "calibration_lut_bt2020",
            "calibration_lut_bt709", "calibration_lut_enabled", "calibration_lut_input_transfer",
            "calibration_lut_p3_d65", "calibration_lut_stage", "adaptive_downscale", "adaptive_w_base",
            "adaptive_edge_lo", "adaptive_edge_hi", "adaptive_grain_lo", "adaptive_grain_hi",
            "adaptive_grain_floor", "adaptive_grain_smooth_ms", "adaptive_grain_shadow_bias",
            "adaptive_chroma_ssim", "adaptive_grain_map", "adaptive_grain_map_smooth_ms", "adaptive_faithful",
            "adaptive_upscale", "adaptive_upscale_w_base", "adaptive_upscale_superres_scale", "downscale_light",
            "upscale_light", "ssim_bitrate_aware", "ssim_bitrate_floor", "cnn_residual_stabilise",
            "ssim_residual_stabilise", "residual_stabilise_grain_guard", "fbo_float32", "adaptive_motion",
            "adaptive_motion_speed_lo", "adaptive_motion_speed_hi", "adaptive_motion_floor",
            "adaptive_motion_smooth_ms" };
        SetDomainKeys(Domain::Motion, std::vector<std::string>(std::begin(motion), std::end(motion)));
        SetDomainKeys(Domain::Rebase, std::vector<std::string>(std::begin(rebase), std::end(rebase)));
        SetDomainKeys(Domain::Analysis, std::vector<std::string>(std::begin(analysis), std::end(analysis)));
        SetDomainKeys(Domain::Output, std::vector<std::string>(std::begin(output), std::end(output)));
    }

    // Replaces a domain's key list (sorted and de-duplicated here, so the order given is irrelevant).
    // The domain's recorded hash is dropped: the next Configure re-records it without an Advance
    // (a different key list is not a settings change).
    void SetDomainKeys(Domain domain, std::vector<std::string> keys)
    {
        const size_t d = static_cast<size_t>(domain);
        if (d >= kDomains)
            return;
        std::sort(keys.begin(), keys.end());
        keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
        m_keys[d] = std::move(keys);
        m_recorded[d] = false;
    }

    const std::vector<std::string>& DomainKeys(Domain domain) const
    {
        static const std::vector<std::string> none;
        const size_t d = static_cast<size_t>(domain);
        return d < kDomains ? m_keys[d] : none;
    }

    static VpIndicator::Counter CounterOf(Domain domain)
    {
        switch (domain) {
        case Domain::Motion: return VpIndicator::kSettingsMotion;
        case Domain::Rebase: return VpIndicator::kSettingsRebase;
        case Domain::Analysis: return VpIndicator::kSettingsAnalysis;
        default: return VpIndicator::kSettingsOutput;
        }
    }

    // The stable hash of one domain under `getter` (keys sorted; each entry: key, present flag, value).
    // A missing key (getter false) and an empty value (getter true, "") differ.
    uint64_t HashDomain(Domain domain, const Getter& getter) const
    {
        uint64_t h = 14695981039346656037ull;   // FNV-1a 64 offset basis
        const size_t d = static_cast<size_t>(domain);
        if (d >= kDomains)
            return h;
        for (const std::string& key : m_keys[d]) {
            std::string value;
            const bool present = getter && getter(key, value);
            Mix(h, key);
            Byte(h, 0x1f);
            Byte(h, present ? 0x31 : 0x30);
            if (present)
                Mix(h, value);
            Byte(h, 0x1e);
        }
        return h;
    }

    // Records (first call, or a domain re-keyed) or compares and Advances changed domains from fromSeq
    // (the Indicator clamps it above the last sealed frame). `ind` may be null: hashes are updated and
    // nothing is written. Render thread (settings apply).
    Result Configure(const Getter& getter, VpIndicator::Indicator* ind, uint64_t fromSeq)
    {
        Result r;
        for (size_t d = 0; d < kDomains; ++d) {
            const Domain domain = static_cast<Domain>(d);
            const uint64_t h = HashDomain(domain, getter);
            if (!m_recorded[d]) {
                m_recorded[d] = true;
                m_hash[d] = h;
                r.first = true;
                continue;
            }
            if (h == m_hash[d])
                continue;
            m_hash[d] = h;
            if (ind != nullptr) {
                const VpIndicator::Counter counter = CounterOf(domain);
                ind->Advance(counter, Cause::SettingsApply, fromSeq);
                r.advanced |= VpIndicator::Bit(counter);
            }
        }
        return r;
    }

    uint64_t Hash(Domain domain) const
    {
        const size_t d = static_cast<size_t>(domain);
        return d < kDomains ? m_hash[d] : 0;
    }
    bool Recorded(Domain domain) const
    {
        const size_t d = static_cast<size_t>(domain);
        return d < kDomains && m_recorded[d];
    }

    // A graph rebuild (slot switch, S0 rebuild): kGraph with Cause::GraphRebuild (the existing
    // graph cause in ResyncCause.h). Returns the effective fromSeq.
    static uint64_t AdvanceGraph(VpIndicator::Indicator& ind, uint64_t fromSeq)
    {
        return ind.Advance(VpIndicator::kGraph, Cause::GraphRebuild, fromSeq);
    }

private:
    static void Byte(uint64_t& h, unsigned char b)
    {
        h ^= b;
        h *= 1099511628211ull;   // FNV-1a 64 prime
    }
    static void Mix(uint64_t& h, const std::string& s)
    {
        for (unsigned char c : s)
            Byte(h, c);
    }

    std::array<std::vector<std::string>, kDomains> m_keys;
    std::array<uint64_t, kDomains> m_hash{};
    std::array<bool, kDomains> m_recorded{};
};

} // namespace VpResync
