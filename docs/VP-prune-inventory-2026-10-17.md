# Settings inventory and pruning, phase 1 (2026-10-17)

Owner rule (2026-10-17): "why keep inferior options" - remove options that a newer default fully
supersedes. Keep every low-latency path, every non-optical-flow fallback (VP's own matcher when the
NVIDIA engine is unavailable), anything another feature depends on, and anything Bill's upstream
users rely on in their cfg files. A removed key must still load: it is migrated or ignored with a log
line, never an error.

Branch `feat/prune-options` from `integ/2026-10-17` (93369c39). Phase 1 = this inventory plus the
safe removals. Out of scope for phase 1 (being merged by other groups): timing / queue scheduling,
intent, detail, calibration and the MPC filter; their candidates are listed in section 6 for phase 2.

## Classes

- **superseded**: a newer default fully replaces it (named replacement); removed in phase 1.
- **keep-fallback**: an inferior-looking path that is a needed fallback (low latency, matcher without
  NVOF, CPU path when a GPU path fails, diagnostics).
- **keep-unique**: does something nothing else does (a feature, a taste value, a slider key, an
  upstream key).
- **unsure**: possibly superseded; the owner decides (section 5).

Code references: `RPC` = src/VideoProcessor-Lib/RendererProfileConfig.h (validation, line numbers on
this branch), `LVR` = src/VideoProcessor-Lib/vprenderer/LibplaceboVideoRenderer.cpp (runtime read),
`CEW` = src/VideoProcessor-Config/ConfigEditorWindow.cpp (Config control), `MCS` =
src/VideoProcessor-Lib/MainConfigSchema.h.

## 1. Removed in phase 1 (superseded)

| Setting | Was | Replacement (what always runs now) | Old files | Code |
|---|---|---|---|---|
| `fix_scene_cooldown` (key) | Fixes under test since 2026-10-14, on; off = the scene detector's 2 s cooldown counted in display-mode units, which always hit the 300-frame cap (12.5 s at 24p, 5 s at 60p) - a unit bug | the 2 s cooldown at the content rate (TimeScale ticks -> 100 ns when the rate is unknown) | accepted with any value, ignored, one log line | RetiredSettings.h; LVR scene detector call (`sceneDetector.Analyze`) |
| `dejudder_picture_area` (key) | Fixes under test since 2026-10-14, on; off = letterbox/pillarbox bars counted as picture in the matcher's statistics (cuts in letterboxed film missed) | matcher statistics over the active picture only, bar blocks held (`Dejudder::Settings::pictureArea` stays as an internal field for tests) | accepted, ignored, logged | RetiredSettings.h; LVR `settings.pictureArea = true` |
| `dither_vectors_new_frame` (key) | Fixes under test since 2026-10-14, on; off = each new film frame's temporal dither ran its own motion search (about 5 ms more per new frame on the laptop) | a new frame's sigma-delta takes the de-judder's field when it is valid; its own search remains the automatic fallback (failed extra, no field, `dither_vectors: own`, no Beta libplacebo) | accepted, ignored, logged | RetiredSettings.h; LVR B's first present (`dejudderNextVectorField`) |
| `adaptive_faithful: polar` (value) | forced polar EWA for the grain-true downscale branch | `auto` (owner 2026-10-16: Auto or 1D; auto already uses polar wherever both axes shrink alike, and separable where polar would soften the axis that should stay sharp). Config already showed a saved polar as Auto | still valid; runs as auto, logged; Config keeps the value as written | RetiredSettings.h; RPC:774; LVR adaptive_faithful read; CEW hidden value holder |
| `dejudder_even_rates: 30`, `30_and_29.97` (values) | the earlier names (owner 2026-10-14, general rates) | `whole`, `whole_and_1001` (identical behaviour) | still valid; migrated, logged | RetiredSettings.h; RPC:645; LVR dejudder_even_rates read |

Migration mechanism: `src/VideoProcessor-Lib/RetiredSettings.h`. Validation
(`RendererProfileConfig::ValidateProfileSetting`, input group) accepts a retired key with any value,
so Config Save/Apply, startup validation and the profile runtime never fail on an old file; retired
values stay in the choice lists. The renderer logs one line per retired key/value per section
(`LogRetiredSettings`, "profile settings: [section] key is retired and ignored: ..."). Config keeps
the keys in the file as written (configuration is user data) and has no control for them.

## 2. Our renderer profile keys (FL35 fork; not in Bill's v1.4.02-beta)

### Capture and source
| Key | Default | Class | Reason | Code |
|---|---|---|---|---|
| `capture_path` gpu/standard | gpu | keep-fallback | standard = the CPU formatters; owner: "remains the fallback" (VP_TEST_CAPTURE_UNPACK_FAIL forces it) | RPC:684, CEW:7906 |
| `capture_upload` auto/render/early | auto | keep-fallback | low-latency path (early upload in low latency) | RPC:685, CEW:7916 |
| `source_range` | auto | keep-unique | sources that send the other range (DeckLink reports none) | RPC:686 |
| `native_rgb_ingest` on/off | on | unsure | off = CPU 10-bit 4:2:0 P010 (lossy) for R12B/R12L/r210; the CPU path stays the automatic fallback (R12 width not a multiple of 8). Owner 2026-10-16 kept it as a Fixes switch | RPC:665, CEW:8658 |
| `dv_tunnel_gpu_unpack` | off | keep-unique | under test, not validated yet (DV live verification pending) | RPC:666 |
| `dv_trims` | off | keep-unique | under test (L2 trims) | RPC:666 |
| `dv_tunnel_fixes` | on | unsure | restore switch from today's DV hunt; judge after the DV cinema session | RPC:667, CEW:8652 |

### Source resolution, re-base, debanding (Stage 12)
| Key | Default | Class | Reason | Code |
|---|---|---|---|---|
| `source_resolution` native/auto/height | auto | keep-unique | the feature | RPC:688 |
| `source_resolution_auto_min_confidence`, `_evidence_shots`, `_evidence_seconds` | 0.8, 3, 10 | keep-unique | Picture slider values | RPC:697-701 |
| `source_resolution_engage_max_wait_s`, `_crossfade_s`, `_release_fade_s` | 20, 1.5, 0.25 | keep-unique | latch timing (srcres work in progress) | RPC:739-741 |
| `source_resolution_latch_fixes` | on | unsure | restore switch, 2026-10-17 latch review; srcres latch still being worked | RPC:738 |
| `source_resolution_text_guard`, `source_resolution_mixed_guard` | on | keep-unique | guards against false evidence; off is a diagnostic for a missed upscale | RPC:734-735 |
| `source_coding_fixes` | on | unsure | hunt 2 restore switch (2026-10-17) | RPC:736 |
| `rebase_kernel_match` | on | keep-unique | off = the generic faithful re-base, which also runs whenever no kernel is identified; a diagnostic for a misidentified kernel | RPC:733 |
| `deband_vp` off/auto/on | off | keep-unique | VP's grain-guarded debander | RPC:702 |
| `deband_vp_grain_guard` on/veto/off | on | unsure | `veto` is the earlier 2026-10-17 guard superseded by the grain split (`on`) the same day; candidate to retire the value after the owner's look | RPC:724 |
| `deband_vp_exclusive` | on | unsure | grain restore switch (hunt 2 C14) | RPC:726 |
| `low_bitrate_handling` | auto | keep-unique | bitrate reading raises the debander | RPC:703 |
| `cnn_residual_stabilise`, `ssim_residual_stabilise` | on | keep-unique | vector refinements (fall back without vectors) | RPC:704 |
| `ssim_bitrate_aware`, `ssim_bitrate_floor` | on, 0.4 | keep-unique | delta 15 S8-2 | RPC:704-705 |
| `residual_stabilise_grain_guard` | on | unsure | grain restore switch (2026-10-17) | RPC:722 |

### Optical flow and motion vectors
| Key | Default | Class | Reason | Code |
|---|---|---|---|---|
| `optical_flow` | on | keep-fallback | off = the matcher only: the non-NVIDIA path (owner rule) | RPC:709 |
| `optical_flow_global` | on | keep-unique | engine camera motion for D1 | RPC:710 |
| `optical_flow_global_fallback` | on | keep-fallback | the global search when the engine's motion is unclear | RPC:712 |
| `optical_flow_cost_trust` | off | unsure | bit-identical on the corpus; owner "retune later" | RPC:712 |
| `optical_flow_fb_check` | off | keep-unique | owner taste option | RPC:712 |
| `motion_ahead_frames` 0-4 | 4 | keep-unique (queue-coupled; phase 2 note) | depth ahead; 0 = matcher; low latency disables | RPC:707 |

### De-judder (VP motion engine)
| Key | Default | Class | Reason | Code |
|---|---|---|---|---|
| `dejudder_strength`, `dejudder_strength_even`, `dejudder_even_rates`, `dejudder_object_strength`, `dejudder_motion_rolloff`, `dejudder_roll_lo/hi/floor` | per slider | keep-unique | the motion slider's eight keys (CEW kKeys); values `30`/`30_and_29.97` retired (section 1) | RPC:641-645, 663, 753-755 |
| `dejudder_match_tolerance`, `dejudder_frame_trust`, `dejudder_texture_tolerance`, `dejudder_cut_histogram`, `dejudder_search_range`, `dejudder_consistency_px`, `dejudder_fade_floor`, `dejudder_pixel_trust_lo/hi`, `dejudder_est_lambda`, `dejudder_motion_scale_lo/hi`, `dejudder_trust_scale`, `dejudder_hold_feather_px`, `dejudder_hold_hysteresis` | tuned | keep-unique | matcher/taste numbers; the matcher is the non-NVOF fallback and must stay tunable | RPC:646-680, 758-763 |
| `dejudder_global_prior`, `_vector_smooth`, `_est_candidates`, `_est_qpel`, `_est_gate`, `_global_two`, `_regional_hold`, `_fade_match`, `_pixel_trust`, `_landing_check` | on | keep-unique | "always on, they only reduce error" (VP-motion-slider.md §2) but they are the matcher's parts and its A/B handles; the fallback matcher must stay at full quality and testable | RPC:660-665 |
| `dejudder_estimate_ahead` | on | keep-unique (timing-adjacent) | spare-refresh estimate; interacts with render-ahead (phase 2 note) | RPC:662 |
| `dejudder_low_res` off/search/all | all | keep-unique | `off` = full-resolution matcher (cost vs accuracy) | RPC:682 |
| `dejudder_pulldown` | off | keep-unique | experimental feature, off | RPC:665 |
| `dejudder_warp_stage` rendered/source | rendered | unsure | owner named "dejudder source warp" as internal-only; but `dejudder_occlusion_fill` (D5) works only with `source`, and the source warp is the internal fallback (no NLS map, interpolation) | RPC:654, CEW:8680 |
| `dejudder_occlusion_fill` (D5) | off | unsure | experiment; only effective with the source warp - retire together with `dejudder_warp_stage` or keep both | RPC:660 |
| `dejudder_est_split` (E2), `dejudder_est_split_sad` | off, 0 | unsure | off since 2026-10-14 ("with the 4.5 tolerance and M6 it costs benefit"): possibly superseded by M6 | RPC:662, 750 |
| `dejudder_verify` (M5), `dejudder_verify_share` | off, 0.2 | unsure | pixel verification of trusted blocks; M6 per-pixel trust (on) may supersede it | RPC:664, 676 |
| `dejudder_block_taper` (H3), `dejudder_taper_sad` | off, 0 | unsure | experiment, off since delta 9 | RPC:662, 760 |
| `dejudder_source_warp_filter` catmull/bilinear | catmull | unsure | grain restore switch (2026-10-17); bilinear = the earlier output | RPC:720 |
| `nls_sampling` catmull/bilinear | catmull | unsure | hunt 2 M1 restore switch (2026-10-17) | RPC:721 |
| `pulldown_fixes` | on | keep (not judged) | waits for the Apple TV recordings replay (open item) | RPC:742 |
| `occlusion_mask` | off | keep-unique | 6c mask for the sigma-delta | RPC:652 |

### 50/60p interpolation and engine choice
| Key | Default | Class | Reason | Code |
|---|---|---|---|---|
| `motion_engine` auto/vp/smooth_motion | auto | keep-unique | one-or-the-other rule (owner 2026-10-15) | RPC:797 |
| `interpolation` off/auto/always | off | keep-unique | kept for other users (owner not a fan) | RPC:715 |
| `interpolation_fallback` hold/blend | hold | keep-unique | blend = the frame-mixer parity the owner asked for | RPC:716 |
| `interpolation_fill_gaps` | on (owner 2026-10-18) | keep-unique | gap fill (wanted, distinct from interpolation) | RPC:717 |
| `interpolation_grain` preserve/blend | preserve | unsure | grain restore switch; `blend` averages grain (against the grain rule) | RPC:719 |
| `source_class` legacy/measured | legacy | keep (phase 2, B2) | the measured classifier is batch B2 | RPC:727 |

### Dither, buffers, scaling and SDR
| Key | Default | Class | Reason | Code |
|---|---|---|---|---|
| `temporal_dither` auto/off/libplacebo/sigma_delta | auto | keep-unique | one dither choice per profile | RPC:732 |
| `dither_vectors` auto/own | auto | unsure (timing-adjacent) | `own` = the dither's own search even on extras; also feeds the render-ahead L1 path (phase 2) | RPC:765 |
| `dither_grain_tolerance_scale` | 3 | keep-unique | S1 | RPC:764 |
| `fbo_float32` auto/on/off | auto | keep-fallback | `off` = half-float buffers (low-latency path / laptop); `on` equals auto today | RPC:767 |
| `adaptive_downscale`, `adaptive_upscale`, `adaptive_chroma_ssim`, `adaptive_grain_map`, `adaptive_upscale_superres_scale`, `adaptive_upscale_w_base`, `adaptive_w_base`, `adaptive_edge_lo/hi`, `adaptive_grain_lo/hi/floor/shadow_bias`, `adaptive_grain_smooth_ms`, `adaptive_grain_map_smooth_ms` | various | keep-unique | Stage 8 blends and their tuning | RPC:769-779 |
| `adaptive_faithful` auto/ortho | auto | keep-unique | Auto / 1D buttons; `polar` retired (section 1) | RPC:774 |
| `downscale_light`, `upscale_light` linear/gamma | linear | keep-unique | light domain (taste) | RPC:828-829 |
| `deband_grain` | off | keep-unique | libplacebo's deband grain back on for those who want it | RPC:839 |
| `preserve_extended_range` | on | keep-fallback | off = libplacebo's clip; diagnostic and the cheaper path | RPC:782 |
| `extended_range_more_cases` | on | unsure | restore switch (GPU tests 2026-10-17 night); off clips BTB/WTW in cases the owner wants kept | RPC:783 |
| `sdr_tone_shape`, `sdr_white_headroom`, `sdr_knee_start`, `sdr_knee_strength`, `sdr_contrast_recovery` | off | keep-unique | delta 13 | RPC:785-789 |
| `saturation` | 0 | keep-unique | colour adjustment | RPC:635 |
| `blank_left/top/right/bottom` (viewport) | 0 | keep-unique | edge blanking | RPC:919-920 |

### Timing, queue, intent (out of phase 1 scope)
`render_ahead`, `render_ahead_queue_credit`, `render_ahead_utilisation`, `dejudder_gate`,
`timing_continuity_fixes`, `low_latency_path`, `target_latency_ms` (RPC:743-748, 978-984) and
`intent_content`, `intent_content_hold_seconds` (RPC:730-731): not classified for removal; see
section 6.

## 3. Bill's upstream keys (present in billslack2 v1.4.02-beta) - keep-unique

Kept as they are: his users' cfg files rely on them, and the owner's scope rule says our pruning and
reformatting cover only our own keys. Existing legacy handling stays (e.g. `downscaler` none /
ewa_lanczos accepted as Auto; `mode` and `priority` ignored with a warning; `live_profile_updates`
alias; `[display_rules]` legacy-only).

- input: `tone_mapping`, `gamut_mapping`, `peak_detection`, `dynamic_target`, `custom_tone_mapping`,
  `contrast_recovery`, the tone-mapping tuning keys (ToneMappingTuning.h) - RPC:630-634.
- scaling: `quality`, `upscaler`, `chroma_upscaler`, `downscaler`, `sigmoid`, `dithering` (all
  methods), `deband_strength`, `deband` - RPC:809-837, 1045.
- motion (v1.4.02): `smooth_motion`, `frame_mixer`, `deinterlace`, `deinterlace_algorithm`,
  `field_order`, `deinterlace_cadence`, `deinterlace_skip_spatial` - RPC:798-804.
- display/colour: `calibration_lut_*`, `display_bit_depth`, `sdr_target_nits`, `sdr_black_nits`,
  `sdr_lut_input_gamma`, `hdr_tone_map_target_gamma`, `output_gamma`, `target_primaries`,
  `sdr_target_primaries`, `report_bt2020_to_display`, `sdr_input_transfer`, `sdr_adjust_gamma` -
  RPC:865-887, 790-791.
- output: `output_path_profile`, `output_presentation`, `output_range`, `output_transport_gamma`,
  `output_diagnostics`, `diagnostic_*` (6 keys) - RPC:892-902.
- viewport/zoom: `mode`, `screen_aspect`, `vertical_alignment`, `screen_edge_padding`,
  `anamorphic_scale`, `automatic_crop`, `crop_*_content_*`, `fixed_crop_aspect`, `subtitle_fit`,
  `hdr_peak_analysis_*`, `subtitle_hold_seconds`, `subtitle_engage/release_drift_ms`,
  `subtitle_padding_pixels`, `subtitle_target_buffer_pixels` - RPC:909-960.
- subtitles: `type`, `subtitle_script_hint`, `rounded_corners`, `temporal_fill`, `cleanup_fill`,
  `background_edge`, `text_reduction_percent`, `minimum_headroom_pixels`, `offset_pixels`, the
  `subtitle_*` box / cut-paste / generated-gray keys - RPC:591-615, 844-863.
- policy ([vprenderer.input_processing] / base): `video_conversion`, `container_colorspace`,
  `hdr_colorspace`, `hdr_luminance`, `profile_update_mode`, `live_profile_updates`,
  `switch_refresh_rate` - RPC:1007-1038.
- queue: `queue_size`, `lead_frames`, `target_frames`, `startup_preroll_frames`,
  `steady_reserve_frames`, `active_picture_lookahead_frames`, `reset_after_render_restart_seconds`,
  `reset_queue_too_large_percent` - RPC:972-982 (also timing scope).
- lldv: `dolby_vision_tunnel`, `max_cll`, `max_fall`, `mastering_min_luminance`,
  `mastering_max_luminance` - RPC:989-994.

## 4. Main configuration (MainConfigSchema.h) and GUI

| Section / key | Owner | Class | Note |
|---|---|---|---|
| `[general]` fullscreen, renderer, capture, colour, scene, subtitle_reposition, refresh switching, overlay, queue flags (MCS:67-130) | Bill | keep-unique | upstream |
| `[general] newlldv`, `new_lldv`, `newlldv_sdr_veto` (MCS:103-105) | both | keep-unique | newlldv now also flagless HDR10 (feat/newlldv-hdr10) |
| `[general] magewell_recover_faults` (MCS:130) | ours | keep-unique | capture fault recovery |
| `[general] capture_display_fixes` (MCS:131) | ours | unsure | restore switch, post-merge audit 2026-10-17 |
| `[decklink] rgb_*_packing` (MCS:204-206) | Bill | keep-unique | |
| `[decklink] ycbcr_12bit_capture` rgb12/v210/off (MCS:207) | ours | keep-unique | v210 default keeps BTB/WTW; rgb12 the earlier default, off refuses |
| `[p010_conversion]`, `[ppm_correction]`, `[queue_recovery]`, `[display_refresh_rate_override]`, `[shaders]`, `[directshow]`, `[logging]`, `[command_line]`, `[renderer_alias]` | Bill | keep-unique | upstream |
| `[queue.*]` incl. `low_latency_path`, `target_latency_ms` (MCS:146-159) | both | phase 2 | timing scope |
| `[shortcuts] foreground_only`, `global_hotkeys` (MCS:197) | ours | keep-unique | |
| `[ambient]` mode, room, lux_lit, lux_dark, hold_s, sensor port/baud (AmbientConfig.h) | ours | keep-unique | owner: ambient adaptation is a real feature |
| `[led_drive.*]` / `[dynamic_drive.*]` (~100 keys, VP-V/modules/drive/led_compensation/LedDriveConfig.h) | ours | keep-unique | Python source keys already retired-and-ignored (2026-10-15); Dynamic Drive rename + projector profiles queued - LED tuning keys reviewed there (phase 2) |
| Config pages (CEW): Queue, Output/Colour, Rendering (sections Capture & source, Source resolution & re-base, Adaptive downscale/upscale, De-judder, Estimation, Temporal dither, Fixes under test, De-judder experiments, Content-adaptive, Processing, SDR highlights), Motion, Subtitles, Screen/Zoom, LLDV, Shaders, LED drive, Ambient, Updates | mixed | - | the three retired switches' controls are gone from Fixes under test; no other page changed |

## 5. Unsure - for the owner

Recommendation in brackets.
1. `native_rgb_ingest` [retire: off is lossy 4:2:0; CPU path stays the automatic fallback] - owner kept it as a Fixes switch on 2026-10-16.
2. `dejudder_warp_stage` + `dejudder_occlusion_fill` [retire both: the source warp becomes internal-only (it stays the automatic fallback); D5 goes with it, or keep D5 and its switch as an experiment].
3. `dejudder_est_split` + `_sad` (E2), `dejudder_verify` + `_share` (M5), `dejudder_block_taper` + `dejudder_taper_sad` (H3) [retire after one corpus run confirms M6 covers them].
4. Restore switches added 2026-10-16/17, retire once judged on screen: `source_coding_fixes`, `nls_sampling`, `dejudder_source_warp_filter`, `interpolation_grain` (blend), `residual_stabilise_grain_guard`, `deband_vp_exclusive`, `deband_vp_grain_guard` value `veto`, `extended_range_more_cases`, `dv_tunnel_fixes`, `source_resolution_latch_fixes`, `[general] capture_display_fixes`.
5. `optical_flow_cost_trust` [keep until the retune, then decide].
6. `dither_vectors: own` [decide with the render-ahead group; it feeds the L1 path].

## 6. Phase 2 candidates in the frozen areas (note only, nothing changed)

- Timing/queue: `render_ahead` (off until the on-screen runs pass; when it is the default, `dejudder_gate: refresh` and `render_ahead_queue_credit` may become internal), `timing_continuity_fixes` (restore switch), `dejudder_estimate_ahead` (with render-ahead), `steady_reserve_frames` (legacy alias of `target_frames`, already migrated at load), `[directshow] presentation_lead_frames` (legacy alias of `lead_frames`), `[queue_recovery]` (legacy home of the reset keys), `motion_ahead_frames` vs queue depth.
- Intent: `intent_content`, `intent_content_hold_seconds` - review after the intent remainder merges.
- Detail: `detail_pulse_stabilise`, `texture_restore` (incoming) - none to prune yet.
- Calibration: `calibration_lut_input_transfer` (accepted Rendering alias of the HDR target gamma), the calibration archive migrations - Bill's; leave.
- MPC filter: no settings of its own on this branch.
- B1/B2: `source_class: legacy` once the measured classifier is judged.
