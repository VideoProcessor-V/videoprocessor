# Config + tiers (`control.config`)

Unit: **control** | side: control | order: 930 | status: **partial** | hook point: host

## Purpose / why

Per-unit Classic/Low/Mid/Upper tier sliders + global, toggles listed beneath, unbuilt slots shown as 'not yet implemented'; settings preview exe.

## Renderer settings (M2 P3)

The VP-V members of the renderer settings live here, not in `LibplaceboVideoRenderer.cpp`:

- `VpvRendererFields.h`: `VpvRenderer::Fields`, the VP-V members (verbatim); the host struct inherits them through
  `RendererSettingsExtension` (host header `vprenderer/RendererSettingsExtension.h`, H7 props define
  `RENDERER_SETTINGS_EXTENSION_HEADER`).
- `VpvRendererSettings.{h,cpp}`: `ReadBase`, `ReadProfile`, `ReadGlobal`, `PostLoad` (Original overlay, slot switches,
  presentation owner; returns the host-field `Overrides`), `Fingerprint`, `AppendDelta`, `CopyLive`, `Describe`.
- `VpvSettingsModule.cpp`: the hub module serving `RendererExtension::SettingsLoad`; registered first in `HubBootstrap.cpp`.
- `VpvSettingsDump.h`, `VpvSettingsExports.cpp`: the identity-kit dump (tests only).
- `VpvSchemaKeys.h`: the VP-V renderer profile keys per group (input, motion, scaling, viewport, queue, base), the
  `[vpv]` known keys and its section check. The host header `ProfileSchemaHooks.h` includes it inside
  `namespace RendererProfileConfig` when the build defines `PROFILE_SCHEMA_EXTENSION_HEADER`; the host validators
  (`ValidateProfileSetting`, `ValidateBaseSetting`) ask it only about a key they rejected or do not know.
- `VpvProfileQueueAdvice.h` (P14, header-only C++14): the VP-V part of `UnifiedProfileRuntime`'s snapshot builder: `Compute(setting, sourceHz)` returns the look-ahead queue depth (optical_flow, motion_ahead_frames, dejudder_strength(_even), interpolation, via `MotionAhead::DepthAt/QueueFramesFor`) and `general_rate_fixes`. No new keys, no classifier, no motion work (one motion source, rule 21); the host keeps the profile resolution.
- `VpvStartupSchema.h`: the VP-V startup sections and keys (`[ambient]`, `[led_drive*]`, `[calibration*]`, VP-V keys of
  `[general]`, `[queue]`, `[decklink]`, `[logging]`, `[shortcuts]`, the file-filter constants, the shortcut
  `global_actions` check). `MainSchemaHooks.h` includes it when `MAIN_SCHEMA_EXTENSION_HEADER` is defined. The host's
  list functions (`GeneralRules` ... `LoggingRules`) hold Bill's keys only; the VP-V keys are added to a copy that
  `Validate` uses (`MainConfigSchema::WithVpKeys`, `SchemaHooks::AppendRules`).
- Why headers and not a runtime registry (R2): the schema headers are compiled into the GUI, the Config editor, both
  renderer DLLs and the tests. A start-up registry would be a separate copy per binary and need a registration and
  link entry in each project; the compile-time seam (both macros come from `Directory.Build.targets`) gives every binary
  the same rules. Without the macros the hooks are inert stubs and the host answers alone.
- Validation order is Bill's (owner decision 2026-11-07, `BillIdenticalSchemaTests`): `MainConfigSchema::Validate` runs
  Bill's checks, then the VP-V group (`SchemaHooks::Validate`); on any fault it re-runs Bill's checks with the VP-V keys
  accepted unchecked (`SchemaHooks::HostOnlyScope`) and reports Bill's first fault if there is one. `RendererProfileConfig::Read`
  does the same (`ProfileSchemaHooks::HostOnlyScope`: VP-V profile keys, the `[vpv]` check).
  Across the two functions: startup runs `Read` then `Validate` (a VP-V profile fault also yields to a host startup fault); Config Save/Apply, the
  legacy editor and the dialog reload run `Validate` then `Read` (`SchemaHooks::ProfileFault`, filled by `RendererProfileConfig.h`, lets a
  VP-V startup-group fault yield to a host profile fault). A file with only VP-V faults
  reports the VP-V text (schema golden); a file with several faults reports the fault Bill's build reports for Bill's keys.

## Settings table (moved settings)

| Key | Section | Type | Range / values | Default | Config placement | Original (Bill) | Fallbacks / status | Tiers |
|---|---|---|---|---|---|---|---|---|
| `global_actions` | [shortcuts] | string | comma-separated [shortcuts] action names (an unknown name is refused) | empty (none) | Config > VP-V page > Keyboard > Global actions | n/a (VP-only) | Actions that act while another application has focus even with global_hotkeys off, e.g. config_editor, toggle_noui. | not tiered |
| `global_hotkeys` | shortcuts (startup) | bool | true, false | false | Config > VP-V page > Keyboard > Every shortcut works while another application has focus | n/a (VP-only) | off: shortcuts act only while VP has focus, except the actions in global_actions; on: every shortcut also acts while another application has focus. | not tiered |
| `renderer_failure_keep_fullscreen` | general (startup) | bool | true, false | true | Config > General page > General behavior > Stay fullscreen and retry after a renderer failure | off = the earlier behaviour | on: after a renderer failure VP stays fullscreen and retries (after 2 s, 5 s, 10 s, then every 30 s, at most 10 times, then Restart Renderer); off: fullscreen is left and the renderer waits for a manual restart. | not tiered |
| `file_av_offset_ms` | general (startup) | int | -2000 to 2000 | 0 | Config > General page > File playback (VP Renderer filter) > Picture offset (A/V) | n/a (VP-only) | Moves the picture against the sound for files played through the VP Renderer filter: positive shows the picture later; capture is not affected. | not tiered |
| `file_led_holdover_ms` | general (startup) | int | 0 to 30000 | 3000 | Config > General page > File playback (VP Renderer filter) > Dynamic Drive hold between files | n/a (VP-only) | The filter keeps Dynamic Drive running this long between files so the next file continues from the current LED level; 0 = restore full light at once. (Owning module may be drive.led_compensation.) | not tiered |
| `retirement_a` | [vpv] (global; also read from the profile) | enum | approve, keep | unset (not decided) | VP-V > Proposed retirement tab, group A decision | n/a (VP-only): unchanged by the Original pathway | The owner's Approve / Keep decision for retirement group A, recorded by the VP-V page's Proposed retirement tab. UI-only record: nothing in the renderer reads it and nothing is hidden or removed by it | same on all (not tiered) |
| `retirement_b` | [vpv] (global; also read from the profile) | enum | approve, keep | unset (not decided) | VP-V > Proposed retirement tab, group B decision | n/a (VP-only): unchanged by the Original pathway | The owner's Approve / Keep decision for retirement group B, recorded by the VP-V page's Proposed retirement tab. UI-only record: nothing in the renderer reads it and nothing is hidden or removed by it | same on all (not tiered) |
| `retirement_c` | [vpv] (global; also read from the profile) | enum | approve, keep | unset (not decided) | VP-V > Proposed retirement tab, group C decision | n/a (VP-only): unchanged by the Original pathway | The owner's Approve / Keep decision for retirement group C, recorded by the VP-V page's Proposed retirement tab. UI-only record: nothing in the renderer reads it and nothing is hidden or removed by it | same on all (not tiered) |
| `retirement_d` | [vpv] (global; also read from the profile) | enum | approve, keep | unset (not decided) | VP-V > Proposed retirement tab, group D decision | n/a (VP-only): unchanged by the Original pathway | The owner's Approve / Keep decision for retirement group D, recorded by the VP-V page's Proposed retirement tab. UI-only record: nothing in the renderer reads it and nothing is hidden or removed by it | same on all (not tiered) |


<!-- BEGIN GENERATED SETTINGS (tools/gen_control_config_readme.py) -->

Generated by `tools/gen_control_config_readme.py` from the schema headers, the settings coverage audit, `docs/design/vpv-tiers.json`, `ApplyOriginal` and the module manifests; do not edit by hand. 197 settings. Tiers are budget-driven (the tier presets list the values they set); no setting is tied to one display or GPU model.

| Key | Section | Type / range | Default | Config placement | Original (Bill) | Fallback / status | Tier differences |
|---|---|---|---|---|---|---|---|
| `calibration_lut_stage` | [vpv] / [vprenderer] | choice: libplacebo, vp | - | Pipeline (expert) | Original sets libplacebo | VP-V section (ADR-0023): stored in [vpv] by the VP-V page (vpv.expert.calibration_lut_stage); also read from [vprenderer] | - |
| `entrance_gpu_drain` | [vpv] / [vprenderer] | choice: off, flush, finish | - | Pipeline (expert) | Original sets off | VP-V section (ADR-0023): stored in [vpv] by the VP-V page (vpv.expert.entrance_gpu_drain); also read from [vprenderer] | - |
| `frame_analysis_store` | [vpv] / [vprenderer] | choice: on, off | - | Pipeline (expert) | Original sets off | VP-V section (ADR-0023): stored in [vpv] by the VP-V page (vpv.expert.frame_analysis_store); also read from [vprenderer] | - |
| `hdr_inference` | [vpv] / [vprenderer] | choice: vp, original | - | Pipeline (expert) | Original sets original | VP-V section (ADR-0023): stored in [vpv] by the VP-V page (vpv.expert.hdr_inference); also read from [vprenderer] | - |
| `output_segments_merged` | [vpv] / [vprenderer] | choice: on, off | - | Pipeline (expert) | Original sets off | VP-V section (ADR-0023): stored in [vpv] by the VP-V page (vpv.expert.output_segments_merged); also read from [vprenderer] | - |
| `pipeline_entrance_once` | [vpv] / [vprenderer] | choice: on, off | - | Pipeline (expert) | unchanged by Original | VP-V section (ADR-0023): stored in [vpv] by the VP-V page (vpv.expert.pipeline_entrance_once, a test key: B3 is the default); also read from [vprenderer] | - |
| `pipeline_order` | [vpv] / [vprenderer] | slot list text | - | Pipeline (expert) | unchanged by Original | VP-V section (ADR-0023): stored in [vpv] by the VP-V page (vpv.expert.pipeline_order); also read from [vprenderer] | - |
| `pipeline_original_pathway` | [vpv] / [vprenderer] | choice: on, off | - | Pathway | unchanged by Original | VP-V section (ADR-0023): stored in [vpv] by the VP-V page (vpv.pathway); also read from [vprenderer] | - |
| `pipeline_slots_off` | [vpv] / [vprenderer] | slot list text | - | Pipeline (expert) | unchanged by Original | VP-V section (ADR-0023): stored in [vpv] by the VP-V page (vpv.expert.pipeline_slots_off); also read from [vprenderer] | - |
| `resync_event` | [vpv] / [vprenderer] | choice: off, shadow, on | - | Pipeline (expert) | Original sets off | VP-V section (ADR-0023): stored in [vpv] by the VP-V page (vpv.expert.resync_event); also read from [vprenderer] | - |
| `retirement_a` | [vpv] / [vprenderer] | choice: approve, keep | - | Config page of its section (audit unit: No control of its own (allow-listed)) | unchanged by Original | allow-listed, NO EFFECT: VP-V section (ADR-0023): the owner's Approve / Keep decision for retirement group A, stored in [vpv] by the VP-V page (vpv.retirement.decision_A); a UI-only record the renderer never reads | - |
| `retirement_b` | [vpv] / [vprenderer] | choice: approve, keep | - | Config page of its section (audit unit: No control of its own (allow-listed)) | unchanged by Original | allow-listed, NO EFFECT: VP-V section (ADR-0023): the owner's Approve / Keep decision for retirement group B, stored in [vpv] by the VP-V page (vpv.retirement.decision_B); a UI-only record the renderer never reads | - |
| `retirement_c` | [vpv] / [vprenderer] | choice: approve, keep | - | Config page of its section (audit unit: No control of its own (allow-listed)) | unchanged by Original | allow-listed, NO EFFECT: VP-V section (ADR-0023): the owner's Approve / Keep decision for retirement group C, stored in [vpv] by the VP-V page (vpv.retirement.decision_C); a UI-only record the renderer never reads | - |
| `retirement_d` | [vpv] / [vprenderer] | choice: approve, keep | - | Config page of its section (audit unit: No control of its own (allow-listed)) | unchanged by Original | allow-listed, NO EFFECT: VP-V section (ADR-0023): the owner's Approve / Keep decision for retirement group D, stored in [vpv] by the VP-V page (vpv.retirement.decision_D); a UI-only record the renderer never reads | - |
| `active_picture_startup_hold` | [vprenderer.*] input group | choice: on, off | On | VP-V > Proposed retirement | Original sets off | retire A | - |
| `adaptive_chroma_ssim` | [vprenderer.*] input group | choice: on, off | Off | VP-V > Advanced | unchanged by Original | - | - |
| `adaptive_downscale` | [vprenderer.*] input group | choice: on, off | Off | VP-V > Advanced | Original sets off | DUAL (managed) | Scaling Classic=off; Scaling Low=off; Scaling Mid=on |
| `adaptive_edge_hi` | [vprenderer.*] input group | number 0.0..255.0 | 40 | VP-V > Advanced | unchanged by Original | - | - |
| `adaptive_edge_lo` | [vprenderer.*] input group | number 0.0..255.0 | 8 | VP-V > Advanced | unchanged by Original | - | - |
| `adaptive_faithful` | [vprenderer.*] input group | choice: polar, ortho, auto | Auto (polar; separable when only one axi | VP-V > Advanced; VP-V page > Scaling (toggle) | unchanged by Original | output scaling: libplacebo downscaler (SSimDownscaler not in the shader list) / plain SSIM (adaptive downscale failed on the GPU); DUAL (managed) | - |
| `adaptive_grain_floor` | [vprenderer.*] input group | number 0.0..1.0 | 0.4 | VP-V > Advanced | unchanged by Original | - | - |
| `adaptive_grain_hi` | [vprenderer.*] input group | number 0.0..64.0 | 8 | VP-V > Advanced | unchanged by Original | - | - |
| `adaptive_grain_lo` | [vprenderer.*] input group | number 0.0..64.0 | 2 | VP-V > Advanced | unchanged by Original | - | - |
| `adaptive_grain_map` | [vprenderer.*] input group | choice: on, off | On | VP-V > Advanced; VP-V page > Scaling (toggle) | unchanged by Original | output scaling: libplacebo downscaler (SSimDownscaler not in the shader list) / plain SSIM (adaptive downscale failed on the GPU); DUAL (managed) | - |
| `adaptive_grain_map_smooth_ms` | [vprenderer.*] input group | number 0.0..10000.0 | 500 | VP-V > Advanced | unchanged by Original | - | - |
| `adaptive_grain_shadow_bias` | [vprenderer.*] input group | number 0.0..1.0 | 0.5 | VP-V > Advanced | unchanged by Original | - | - |
| `adaptive_grain_smooth_ms` | [vprenderer.*] input group | number 0.0..10000.0 | 1000 | VP-V > Advanced | unchanged by Original | - | - |
| `adaptive_motion` | [vprenderer.*] input group | choice: on, off | Use default | VP-V > Advanced | Original sets off | - | - |
| `adaptive_motion_floor` | [vprenderer.*] input group | number 0.0..1.0 | 0 | VP-V > Advanced | unchanged by Original | - | - |
| `adaptive_motion_smooth_ms` | [vprenderer.*] input group | number 0.0..10000.0 | 250 | VP-V > Advanced | unchanged by Original | - | - |
| `adaptive_motion_speed_hi` | [vprenderer.*] input group | number 0.0..10.0 | 0.4 | VP-V > Advanced | unchanged by Original | - | - |
| `adaptive_motion_speed_lo` | [vprenderer.*] input group | number 0.0..10.0 | 0.1 | VP-V > Advanced | unchanged by Original | - | - |
| `adaptive_upscale` | [vprenderer.*] input group | choice: on, off | Off | VP-V > Advanced; VP-V page > Scaling (toggle) | Original sets off | output scaling: libplacebo downscaler (SSimDownscaler not in the shader list) / plain SSIM (adaptive downscale failed on the GPU); DUAL (managed) | - |
| `adaptive_upscale_superres_scale` | [vprenderer.*] input group | choice: on, off | On | VP-V > Advanced | unchanged by Original | - | - |
| `adaptive_upscale_w_base` | [vprenderer.*] input group | number 0.0..1.0 | 1 | VP-V > Advanced | unchanged by Original | - | - |
| `adaptive_w_base` | [vprenderer.*] input group | number 0.0..1.0 | 1 | VP-V > Advanced | unchanged by Original | DUAL (managed) | Scaling Mid=0.2 |
| `ambient_adapt` | [vprenderer.*] input group | choice: off, on | Off | Tone Mapping > Profile details; VP-V page > Colour (toggle) | Original sets off | Appearance: off (shader failed); DUAL (managed) | - |
| `ambient_adapt_strength` | [vprenderer.*] input group | number 0.0..1.0 | 1 | VP-V > Advanced | unchanged by Original | - | - |
| `ambient_max_black_nits` | [vprenderer.*] input group | number 0.0..10.0 | 1 | VP-V > Advanced | unchanged by Original | - | - |
| `ambient_screen_reflect` | [vprenderer.*] input group | number 0.0..1.0 | 0.01 | VP-V > Advanced | unchanged by Original | - | - |
| `ambient_tau_down_s` | [vprenderer.*] input group | number 1.0..1800.0 | 90 | VP-V > Advanced | unchanged by Original | - | - |
| `ambient_tau_up_s` | [vprenderer.*] input group | number 1.0..600.0 | 15 | VP-V > Advanced | unchanged by Original | - | - |
| `cadence_class_fixes` | [vprenderer.*] input group | choice: on, off | On | VP-V > Proposed retirement | Original sets off | retire A | - |
| `capture_path` | [vprenderer.*] input group | choice: standard, gpu | gpu | Tone Mapping > Profile details; VP-V page > Input (toggle) | Original sets standard | capture GPU unpack: CPU formatters for the session: <error> / native RGB GPU unpack: CPU conversion to 10-bit 4:2:0 for the session: <error> / early upload: the render upload (<error>) / capture extension: capture routes off for this renderer (no extension slot): the CPU formatters and the render upload; DUAL (managed) | - |
| `capture_upload` | [vprenderer.*] input group | choice: auto, render, early | Auto (early in every mode) | Tone Mapping > Profile details; VP-V page > Input (toggle) | Original sets render | capture GPU unpack: CPU formatters for the session: <error> / native RGB GPU unpack: CPU conversion to 10-bit 4:2:0 for the session: <error> / early upload: the render upload (<error>) / capture extension: capture routes off for this renderer (no extension slot): the CPU formatters and the render upload; DUAL (managed) | - |
| `cnn_residual_stabilise` | [vprenderer.*] input group | choice: on, off | On | VP-V > Advanced; VP-V page > Scaling (toggle) | Original sets off | output scaling: libplacebo downscaler (SSimDownscaler not in the shader list) / plain SSIM (adaptive downscale failed on the GPU); DUAL (managed) | - |
| `deband_vp` | [vprenderer.*] input group | choice: off, auto, on | Off | VP-V > Advanced | Original sets off | DUAL (managed) | Repair Classic=off; Repair Low=off; Repair Mid=on; Repair Upper=on |
| `deband_vp_exclusive` | [vprenderer.*] input group | choice: on, off | On | Tone Mapping > Profile details; VP-V page > Repair (toggle) | Original sets off | deband: libplacebo deband (VP debander failed: <error>) / VP deband: off on the DV tunnel planes (no deband after decode yet); DUAL (managed) | - |
| `deband_vp_grain_guard` | [vprenderer.*] input group | choice: on, veto, off | On | Tone Mapping > Profile details; VP-V page > Repair (toggle) | unchanged by Original | deband: libplacebo deband (VP debander failed: <error>) / VP deband: off on the DV tunnel planes (no deband after decode yet); DUAL (managed) | - |
| `dejudder_block_taper` | [vprenderer.*] input group | choice: on, off | Off | VP-V > Proposed retirement | unchanged by Original | retire B | - |
| `dejudder_consistency_px` | [vprenderer.*] input group | number 0.5..8.0 | 2 | VP-V > Advanced | unchanged by Original | - | - |
| `dejudder_cut_histogram` | [vprenderer.*] input group | number 0.0..2.0 | 0.3 | VP-V > Advanced | unchanged by Original | - | - |
| `dejudder_est_candidates` | [vprenderer.*] input group | choice: on, off | On | VP-V > Advanced | unchanged by Original | - | - |
| `dejudder_est_gate` | [vprenderer.*] input group | choice: on, off | On | VP-V > Advanced | unchanged by Original | - | - |
| `dejudder_est_lambda` | [vprenderer.*] input group | number 0.0..8.0 | 0.5 | VP-V > Advanced | unchanged by Original | - | - |
| `dejudder_est_qpel` | [vprenderer.*] input group | choice: on, off | On | VP-V > Advanced | unchanged by Original | DUAL (managed) | Motion Classic=off; Motion Low=on; Motion Mid=on; Motion Upper=on |
| `dejudder_est_split` | [vprenderer.*] input group | choice: on, off | Off | VP-V > Advanced | unchanged by Original | DUAL (managed) | Motion Low=off; Motion Mid=off; Motion Upper=on |
| `dejudder_est_split_sad` | [vprenderer.*] input group | number 0.0..16.0 | 0 | VP-V > Advanced | unchanged by Original | - | - |
| `dejudder_est_still_zero` | [vprenderer.*] input group | choice: on, off | On | VP-V > Advanced | Original sets off | - | - |
| `dejudder_estimate_ahead` | [vprenderer.*] input group | choice: on, off | On | VP-V > Advanced; VP-V page > Motion (toggle) | Original sets off | motion: matcher only (optical flow failed: <error>) / no optical-flow look-ahead (producer stopped: <error>); DUAL (managed) | - |
| `dejudder_even_rates` | [vprenderer.*] input group | choice: whole, whole_and_1001, 30, 30_and_29.97 | whole | VP-V > Advanced | unchanged by Original | - | - |
| `dejudder_fade_floor` | [vprenderer.*] input group | number 0.0..1.0 | 0.25 | VP-V > Advanced | unchanged by Original | - | - |
| `dejudder_fade_match` | [vprenderer.*] input group | choice: on, off | On | VP-V > Advanced | unchanged by Original | - | - |
| `dejudder_frame_trust` | [vprenderer.*] input group | number 0.0..1.0 | 0.25 | VP-V > Advanced | unchanged by Original | - | - |
| `dejudder_gate` | [vprenderer.*] input group | choice: auto, period, refresh | Auto (per frame with render ahead) | VP-V > Advanced | unchanged by Original | - | - |
| `dejudder_global_prior` | [vprenderer.*] input group | choice: on, off | On | VP-V > Advanced | unchanged by Original | - | - |
| `dejudder_global_two` | [vprenderer.*] input group | choice: on, off | On | VP-V > Advanced | unchanged by Original | - | - |
| `dejudder_hold_feather_px` | [vprenderer.*] input group | number 0.0..16.0 | 4 | VP-V > Advanced | unchanged by Original | - | - |
| `dejudder_hold_hysteresis` | [vprenderer.*] input group | integer 1..8 | 2 | VP-V > Advanced | unchanged by Original | - | - |
| `dejudder_landing_check` | [vprenderer.*] input group | choice: on, off | On | VP-V > Advanced | unchanged by Original | - | - |
| `dejudder_low_res` | [vprenderer.*] input group | choice: off, search, all | all | VP-V > Advanced | unchanged by Original | - | - |
| `dejudder_match_tolerance` | [vprenderer.*] input group | number 0.5..16.0 | 4.5 | VP-V > Advanced | unchanged by Original | - | - |
| `dejudder_motion_rolloff` | [vprenderer.*] input group | choice: on, off | Off | VP-V > Advanced | unchanged by Original | - | - |
| `dejudder_motion_scale_hi` | [vprenderer.*] input group | number 0.0..32.0 | 1.5 | VP-V > Advanced | unchanged by Original | - | - |
| `dejudder_motion_scale_lo` | [vprenderer.*] input group | number 0.0..16.0 | 0.25 | VP-V > Advanced | unchanged by Original | - | - |
| `dejudder_object_strength` | [vprenderer.*] input group | number 0.0..1.0 | 1 (off) | VP-V > Advanced | unchanged by Original | - | - |
| `dejudder_occlusion_fill` | [vprenderer.*] input group | choice: on, off | Off | VP-V > Proposed retirement | unchanged by Original | retire B | - |
| `dejudder_pixel_trust` | [vprenderer.*] input group | choice: on, off | On | VP-V > Advanced | unchanged by Original | - | - |
| `dejudder_pixel_trust_hi` | [vprenderer.*] input group | number 0.5..32.0 | 8 | VP-V > Advanced | unchanged by Original | - | - |
| `dejudder_pixel_trust_lo` | [vprenderer.*] input group | number 0.25..16.0 | 3 | VP-V > Advanced | unchanged by Original | - | - |
| `dejudder_pulldown` | [vprenderer.*] input group | choice: on, off | Off | VP-V > Advanced | unchanged by Original | - | - |
| `dejudder_regional_hold` | [vprenderer.*] input group | choice: on, off | On | VP-V > Advanced | unchanged by Original | - | - |
| `dejudder_roll_floor` | [vprenderer.*] input group | number 0.0..1.0 | 0.25 | VP-V > Advanced | unchanged by Original | - | - |
| `dejudder_roll_hi` | [vprenderer.*] input group | number 0.0..400.0 | 24 | VP-V > Advanced | unchanged by Original | - | - |
| `dejudder_roll_lo` | [vprenderer.*] input group | number 0.0..200.0 | 8 | VP-V > Advanced | unchanged by Original | - | - |
| `dejudder_search_range` | [vprenderer.*] input group | integer 1..16 | 8 | VP-V > Advanced | unchanged by Original | - | - |
| `dejudder_source_warp_filter` | [vprenderer.*] input group | choice: catmull, bilinear | catmull | VP-V > Proposed retirement | Original sets off | retire B | - |
| `dejudder_strength` | [vprenderer.*] input group | number 0.0..1.0 | 0 = off; the slider sets it | VP-V > Advanced; VP-V page > Motion (taste) | Original sets 0.0 | - | - |
| `dejudder_strength_even` | [vprenderer.*] input group | number 0.0..1.0 | 0 = off | VP-V > Advanced; VP-V page > Motion (taste) | Original sets 0.0 | - | - |
| `dejudder_taper_sad` | [vprenderer.*] input group | number 0.0..32.0 | 0 | VP-V > Proposed retirement | unchanged by Original | retire B | - |
| `dejudder_texture_tolerance` | [vprenderer.*] input group | number 0.0..4.0 | 0.25 | VP-V > Advanced | unchanged by Original | - | - |
| `dejudder_trust_scale` | [vprenderer.*] input group | number 0.0..1.0 | 0.6 | VP-V > Advanced | unchanged by Original | - | - |
| `dejudder_vector_smooth` | [vprenderer.*] input group | choice: on, off | On | VP-V > Advanced | unchanged by Original | - | - |
| `dejudder_verify` | [vprenderer.*] input group | choice: on, off | Off | VP-V > Advanced | unchanged by Original | retire D | - |
| `dejudder_verify_share` | [vprenderer.*] input group | number 0.02..1.0 | 0.2 | VP-V > Advanced | unchanged by Original | retire D | - |
| `dejudder_warp_stage` | [vprenderer.*] input group | choice: rendered, source | rendered | VP-V > Proposed retirement | unchanged by Original | retire B | - |
| `detail_pulse_cap` | [vprenderer.*] input group | number 1.0..1.5 | 1.35 | VP-V > Advanced | unchanged by Original | - | - |
| `detail_pulse_fast` | [vprenderer.*] input group | choice: on, off | On | Tone Mapping > Profile details; VP-V page > Repair (toggle) | unchanged by Original | Texture restore: inactive (<reason>); DUAL (managed), retire C | - |
| `detail_pulse_stabilise` | [vprenderer.*] input group | choice: on, off | On | VP-V > Advanced | Original sets off | DUAL (managed) | Repair Classic=off; Repair Low=on; Repair Mid=on; Repair Upper=on |
| `dither_grain_tolerance_scale` | [vprenderer.*] input group | number 1.0..8.0 | 3 | VP-V > Advanced | unchanged by Original | - | - |
| `dither_vectors` | [vprenderer.*] input group | choice: auto, own | De-judder's (auto) | Tone Mapping > Profile details; VP-V page > Motion (toggle) | unchanged by Original | motion consumers: <warp failed / rendered warp failed / interpolation failed>: the source frame is shown instead; DUAL (managed), retire C | - |
| `dv_compare_capture_folder` | [vprenderer.*] input group | a folder path without a double quote (empty = off) | Auto | VP-V > Advanced | unchanged by Original | - | - |
| `dv_tone_mapping` | [vprenderer.*] input group | choice: libplacebo, dolby_style | libplacebo | VP-V > Main | Original sets libplacebo | - | - |
| `dv_trims` | [vprenderer.*] input group | choice: on, off | On | Tone Mapping > Profile details; VP-V page > Colour (toggle) | Original sets dv_compare_capture_folder 		settings.dvTrims = false | DV tone mapping: <why>: bt2390[ + trims] / Dolby Vision trims: <state> / SDR tone shape: not applied (<why>); DUAL (managed) | - |
| `dv_tunnel_fixes` | [vprenderer.*] input group | choice: on, off | On | VP-V > Proposed retirement | Original sets off | retire A | - |
| `dv_tunnel_gpu_unpack` | [vprenderer.*] input group | choice: on, off | On | Tone Mapping > Profile details; VP-V page > Input (toggle) | Original sets off | Dolby Vision GPU unpack: CPU unpack for the session: <error> / DV metadata: <held / static>; DUAL (managed) | - |
| `efficiency_fixes` | [vprenderer.*] input group | choice: on, off | On | VP-V > Proposed retirement | Original sets off | retire A | - |
| `extended_range_more_cases` | [vprenderer.*] input group | choice: on, off | On | VP-V > Proposed retirement | Original sets off | retire A | - |
| `fbo_float32` | [vprenderer.*] input group | choice: auto, on, off | From input | Tone Mapping > Profile details; VP-V page > Colour (toggle) | Original sets 0 | Tone map: libplacebo (combined LUT missing); DUAL (managed) | - |
| `general_rate_fixes` | [vprenderer.*] input group | choice: on, off | On | VP-V > Proposed retirement | Original sets off | retire A | - |
| `intent_black` | [vprenderer.*] input group | choice: off, report, led_protect | Off | VP-V > Advanced | Original sets BlackIntentMode::Off | - | - |
| `intent_content` | [vprenderer.*] input group | choice: off, on | Off | Tone Mapping > Profile details; VP-V page > Input (toggle) | Original sets off | Source analysis: labels unknown (<reason>); DUAL (managed) | - |
| `intent_content_hold_seconds` | [vprenderer.*] input group | number 1.0..60.0 | 5 | VP-V > Advanced | unchanged by Original | - | - |
| `intent_display` | [vprenderer.*] input group | choice: off, report | Off | VP-V > Advanced | Original sets off | - | - |
| `intent_ease_s` | [vprenderer.*] input group | number 0.0..5.0 | 1.5 | VP-V > Advanced | unchanged by Original | - | - |
| `intent_motion` | [vprenderer.*] input group | choice: off, on | Off | Tone Mapping > Profile details; VP-V page > Motion (toggle) | Original sets off | motion consumers: <warp failed / rendered warp failed / interpolation failed>: the source frame is shown instead; DUAL (managed) | - |
| `intent_motion_min` | [vprenderer.*] input group | number 0.0..1.0 | 0.5 | VP-V > Advanced | unchanged by Original | - | - |
| `intent_motion_min_shot_s` | [vprenderer.*] input group | number 0.5..30.0 | 2 | VP-V > Advanced | unchanged by Original | - | - |
| `intent_motion_policy` | [vprenderer.*] input group | choice: display, ease_fast, ease_slow, visibility, metadata | display | VP-V > Advanced | unchanged by Original | - | - |
| `interpolation` | [vprenderer.*] input group | choice: off, auto, always | Off | VP-V > Advanced; VP-V page > Motion (toggle) | Original sets InterpolationPolicy::Mode::Off | motion consumers: <warp failed / rendered warp failed / interpolation failed>: the source frame is shown instead; DUAL (managed) | - |
| `interpolation_fallback` | [vprenderer.*] input group | choice: hold, blend | hold | VP-V > Advanced; VP-V page > Motion (toggle) | Original sets InterpolationPolicy::Fallback::Hold | motion consumers: <warp failed / rendered warp failed / interpolation failed>: the source frame is shown instead; DUAL (managed) | - |
| `interpolation_fill_gaps` | [vprenderer.*] input group | choice: off, on | On | VP-V > Advanced; VP-V page > Motion (toggle) | Original sets off | motion consumers: <warp failed / rendered warp failed / interpolation failed>: the source frame is shown instead; DUAL (managed) | - |
| `interpolation_grain` | [vprenderer.*] input group | choice: preserve, blend | preserve | VP-V > Proposed retirement | unchanged by Original | retire B | - |
| `low_bitrate_handling` | [vprenderer.*] input group | choice: auto, off, on | Auto (measured) | Tone Mapping > Profile details; VP-V page > Repair (toggle) | Original sets off | deband: libplacebo deband (VP debander failed: <error>) / VP deband: off on the DV tunnel planes (no deband after decode yet); DUAL (managed) | - |
| `motion_ahead_frames` | [vprenderer.*] input group | choice: 0, 1, 2, 3, 4 | 4 | VP-V > Advanced | Original sets 0 | DUAL (managed) | Motion Classic=0; Motion Low=0; Motion Mid=4; Motion Upper=4 |
| `motion_history_fixes` | [vprenderer.*] input group | choice: on, off | On | VP-V > Proposed retirement | Original sets off | retire A | - |
| `native_rgb_ingest` | [vprenderer.*] input group | choice: on, off | On | Tone Mapping > Profile details; VP-V page > Input (toggle) | Original sets off | capture GPU unpack: CPU formatters for the session: <error> / native RGB GPU unpack: CPU conversion to 10-bit 4:2:0 for the session: <error> / early upload: the render upload (<error>) / capture extension: capture routes off for this renderer (no extension slot): the CPU formatters and the render upload; DUAL (managed) | - |
| `nls_sampling` | [vprenderer.*] input group | choice: catmull, bilinear | catmull | VP-V > Proposed retirement | Original sets off | retire B | - |
| `occlusion_mask` | [vprenderer.*] input group | choice: on, off | Off | Tone Mapping > Profile details; VP-V page > Output (toggle) | Original sets off | dither: libplacebo dither; sigma-delta dropped: <reason>; DUAL (managed) | - |
| `optical_flow` | [vprenderer.*] input group | choice: on, off | On | VP-V > Advanced | Original sets off | DUAL (managed) | Motion Classic=off; Motion Low=off; Motion Mid=on; Motion Upper=on |
| `optical_flow_cost_trust` | [vprenderer.*] input group | choice: on, off | Off | VP-V > Proposed retirement | unchanged by Original | retire B | - |
| `optical_flow_fb_check` | [vprenderer.*] input group | choice: on, off | Off | VP-V > Advanced | unchanged by Original | - | - |
| `optical_flow_global` | [vprenderer.*] input group | choice: on, off | On | VP-V > Advanced | unchanged by Original | - | - |
| `optical_flow_global_fallback` | [vprenderer.*] input group | choice: on, off | On | VP-V > Advanced | unchanged by Original | - | - |
| `preserve_extended_range` | [vprenderer.*] input group | choice: on, off | On | Tone Mapping > Profile details; VP-V page > Colour (toggle) | Original sets off | BTB/WTW: clipped: <reason>; DUAL (managed) | - |
| `pulldown_fixes` | [vprenderer.*] input group | choice: on, off | On | VP-V > Proposed retirement | Original sets off | retire A | - |
| `queue_timing_fixes` | [vprenderer.*] input group | choice: on, off | On | VP-V > Proposed retirement | Original sets off | retire A | - |
| `rebase_kernel_match` | [vprenderer.*] input group | choice: on, off | Off (SID r2; was On) | Tone Mapping > Profile details; VP-V page > Scaling (toggle) | Original sets off | re-base precision: <the re-base's float-intermediate note>; re-base warm-up: <the hold status while shaders compile, or the visible bypass>; DUAL (managed) | - |
| `render_ahead` | [vprenderer.*] input group | choice: on, off | Off | Tone Mapping > Profile details; VP-V page > Motion (toggle) | Original sets off | presentation: open loop: no display statistics, estimated rate / de-judder held: display rate not known; DUAL (managed) | - |
| `render_ahead_queue_credit` | [vprenderer.*] input group | choice: on, off | Off | VP-V > Advanced | unchanged by Original | - | - |
| `render_ahead_utilisation` | [vprenderer.*] input group | number 0.5..1.0 | 0.85 | VP-V > Advanced | unchanged by Original | - | - |
| `repeat_render_reuse` | [vprenderer.*] input group | choice: on, off | From input | Tone Mapping > Profile details | Original sets off | - | - |
| `residual_stabilise_grain_guard` | [vprenderer.*] input group | choice: on, off | On | VP-V > Proposed retirement | Original sets off | retire A | - |
| `saturation` | [vprenderer.*] input group | integer -100..100 | 0 | Tone Mapping > Profile details | unchanged by Original | - | - |
| `sdr_contrast_recovery` | [vprenderer.*] input group | choice: auto, off or number 0.0..2.0 | Auto | VP-V > Advanced | unchanged by Original | - | - |
| `sdr_knee_start` | [vprenderer.*] input group | number 0.5..0.99 | 0.80 | VP-V > Advanced | unchanged by Original | - | - |
| `sdr_knee_strength` | [vprenderer.*] input group | number 0.0..1.0 | 0.5 | VP-V > Advanced | unchanged by Original | - | - |
| `sdr_tone_shape` | [vprenderer.*] input group | choice: off, knee | Off | VP-V > Advanced; VP-V page > Colour (toggle) | Original sets off | DV tone mapping: <why>: bt2390[ + trims] / Dolby Vision trims: <state> / SDR tone shape: not applied (<why>); DUAL (managed) | - |
| `sdr_white_headroom` | [vprenderer.*] input group | number 0.0..0.5 | 0 | VP-V > Advanced | unchanged by Original | - | - |
| `source_class` | [vprenderer.*] input group | choice: legacy, measured | legacy | VP-V > Advanced | unchanged by Original | - | - |
| `source_coding_fixes` | [vprenderer.*] input group | choice: on, off | On | VP-V > Proposed retirement | Original sets off | retire A | - |
| `source_geometry_detect` | [vprenderer.*] input group | all, conventional or a list of source-size ids (unknown ids: a status line) | all | VP-V > Scaling > Re-base | unchanged by Original | - | - |
| `source_range` | [vprenderer.*] input group | choice: auto, full, limited | Auto (from the capture format) | Tone Mapping > Profile details; VP-V page > Input (toggle) | Original sets auto | Dolby Vision GPU unpack: CPU unpack for the session: <error> / DV metadata: <held / static>; DUAL (managed) | - |
| `source_resolution` | [vprenderer.*] input group | native, auto, a height 144..8640 or WIDTHxHEIGHT (width up to 17280) | Auto (detected) | VP-V > Advanced | Original sets native | DUAL (managed) | Scaling Classic=native; Scaling Low=auto; Scaling Mid=auto |
| `source_resolution_auto_min_confidence` | [vprenderer.*] input group | number 0.0..1.0 | 0.8 | VP-V > Advanced | unchanged by Original | - | - |
| `source_resolution_crossfade_s` | [vprenderer.*] input group | number 0.0..10.0 | 1.5 | VP-V > Advanced | unchanged by Original | - | - |
| `source_resolution_cut_fixes` | [vprenderer.*] input group | choice: on, off | On | VP-V > Proposed retirement | Original sets off | retire A | - |
| `source_resolution_engage_max_wait_s` | [vprenderer.*] input group | number 0.0..600.0 | 20 | VP-V > Advanced | unchanged by Original | - | - |
| `source_resolution_evidence_seconds` | [vprenderer.*] input group | number 2.0..120.0 | 10 | VP-V > Advanced | unchanged by Original | - | - |
| `source_resolution_evidence_shots` | [vprenderer.*] input group | number 1..10 | 3 | VP-V > Advanced | unchanged by Original | - | - |
| `source_resolution_latch_fixes` | [vprenderer.*] input group | choice: on, off | On | VP-V > Proposed retirement | Original sets off | retire A | - |
| `source_resolution_mixed_guard` | [vprenderer.*] input group | choice: on, off | On | Tone Mapping > Profile details; VP-V page > Scaling (toggle) | unchanged by Original | re-base precision: <the re-base's float-intermediate note>; re-base warm-up: <the hold status while shaders compile, or the visible bypass>; DUAL (managed) | - |
| `source_resolution_release_fade_s` | [vprenderer.*] input group | number 0.0..0.25 | 0.25 | VP-V > Advanced | unchanged by Original | - | - |
| `source_resolution_text_guard` | [vprenderer.*] input group | choice: on, off | On | Tone Mapping > Profile details; VP-V page > Scaling (toggle) | unchanged by Original | re-base precision: <the re-base's float-intermediate note>; re-base warm-up: <the hold status while shaders compile, or the visible bypass>; DUAL (managed) | - |
| `ssim_bitrate_aware` | [vprenderer.*] input group | choice: on, off | On | VP-V > Advanced; VP-V page > Scaling (toggle) | Original sets off | output scaling: libplacebo downscaler (SSimDownscaler not in the shader list) / plain SSIM (adaptive downscale failed on the GPU); DUAL (managed) | - |
| `ssim_bitrate_floor` | [vprenderer.*] input group | number 0.0..1.0 | 0.4 | VP-V > Advanced | unchanged by Original | - | - |
| `ssim_residual_stabilise` | [vprenderer.*] input group | choice: on, off | On | VP-V > Advanced; VP-V page > Scaling (toggle) | Original sets off | output scaling: libplacebo downscaler (SSimDownscaler not in the shader list) / plain SSIM (adaptive downscale failed on the GPU); DUAL (managed) | - |
| `temporal_dither` | [vprenderer.*] input group | choice: auto, off, libplacebo, sigma_delta | Auto (the shader list decides) | VP-V > Advanced | Original sets libplacebo | DUAL (managed) | Output Classic=libplacebo; Output Low=auto; Output Mid=sigma_delta |
| `texture_restore` | [vprenderer.*] input group | choice: off, statistical | Off | VP-V > Advanced | Original sets off | DUAL (managed) | Repair Classic=off; Repair Low=off; Repair Mid=off; Repair Upper=statistical |
| `texture_restore_cap` | [vprenderer.*] input group | number 1.0..2.0 | 1.50 | VP-V > Advanced | unchanged by Original | - | - |
| `timing_continuity_fixes` | [vprenderer.*] input group | choice: on, off | On | VP-V > Proposed retirement | Original sets off | retire A | - |
| `vector_demand` | [vprenderer.*] input group | choice: on, off | On | Tone Mapping > Profile details; VP-V page > Motion (toggle) | Original sets off | motion: matcher only (optical flow failed: <error>) / no optical-flow look-ahead (producer stopped: <error>); DUAL (managed) | - |
| `motion_engine` | [vprenderer.*] motion group | choice: auto, vp, smooth_motion | From input | Motion > Profile details | unchanged by Original | - | - |
| `low_latency_path` | [queue] / [queue.*] (and the queue profile group) and [queue] / [queue.*] | choice: on, off | - | Queue > Profile details | unchanged by Original | - | - |
| `target_latency_ms` | [queue] / [queue.*] (and the queue profile group) and [queue] / [queue.*] | integer 0..1000 | Off | Queue > Profile details | unchanged by Original | - | - |
| `chroma_upscale_vp` | [vprenderer.*] scaling group | choice: off, cfl | - | VP-V > Main | unchanged by Original | - | - |
| `deband_grain` | [vprenderer.*] scaling group | choice: on, off | Off | VP-V > Proposed retirement | Original sets on | retire B | - |
| `downscale_light` | [vprenderer.*] scaling group | choice: linear, gamma | Use default | VP-V > Main | Original sets linear | - | - |
| `upscale_light` | [vprenderer.*] scaling group | choice: linear, gamma | Linear | VP-V > Advanced | Original sets linear | - | - |
| `auto_report` | [logging] | boolean | unchecked | VP-V > Problem reports | unchanged by Original | ORPHAN? | - |
| `capture_display_fixes` | [general] / [command_line] | boolean | checked | General > General behavior | unchanged by Original | retire A | - |
| `dv_tunnel_handoff` | [general] / [command_line] | boolean | checked | VP-V > Input (both pathways) | unchanged by Original | - | - |
| `file_av_offset_ms` | [general] / [command_line] | integer -2000..2000 | 0 ms | General > File playback (VP Renderer filter) | unchanged by Original | - | - |
| `file_led_holdover_ms` | [general] / [command_line] | integer 0..30000 | 3000 ms | General > File playback (VP Renderer filter) | unchanged by Original | - | - |
| `global_hotkeys` | [shortcuts] | boolean | unchecked | VP-V > Keyboard | unchanged by Original | - | - |
| `magewell_recover_faults` | [general] / [command_line] | boolean | checked | General > General behavior | unchanged by Original | retire A | - |
| `newlldv_sdr_veto` | [general] / [command_line] | boolean | checked | VP-V > Input (both pathways) | unchanged by Original | - | - |
| `queue_auto_reset` | [general] / [command_line] | boolean | checked | General > General behavior | unchanged by Original | - | - |
| `renderer_failure_keep_fullscreen` | [general] / [command_line] | boolean | checked | General > General behavior | unchanged by Original | - | - |
| `report_thumbnail` | [logging] | boolean | unchecked | VP-V > Problem reports | unchanged by Original | ORPHAN? | - |
| `stats_overlay` | [general] / [command_line] | boolean | unchecked | General > General behavior | unchanged by Original | - | - |
| `use_frame_queue` | [general] / [command_line] | boolean | checked | General > General behavior | unchanged by Original | - | - |
| `ycbcr_12bit_capture` | [decklink] | choice: rgb12, v210, off | 10-bit 4:2:2, keeps BTB/WTW | VP-V > Input (both pathways) | unchanged by Original | - | - |
| `blank_bottom` | [vprenderer.*] viewport group | integer 0..4000 | - | Screen > Profile details | unchanged by Original | - | - |
| `blank_left` | [vprenderer.*] viewport group | integer 0..4000 | - | Screen > Profile details | unchanged by Original | - | - |
| `blank_right` | [vprenderer.*] viewport group | integer 0..4000 | - | Screen > Profile details | unchanged by Original | - | - |
| `blank_top` | [vprenderer.*] viewport group | integer 0..4000 | - | Screen > Profile details | unchanged by Original | - | - |

<!-- END GENERATED SETTINGS -->

## Layout of the module (W0-3 / M-d)

One settings home. `control.config` holds, in this folder, the renderer-side settings (schema headers, renderer fields, identity dump; the sections above) and, in `config_pages/`, the Config-editor side (the VP-V page, its rows, cards, tiers and organisation; the section below). `config_pages/` was `src/VP-V/config/` (moved by `git mv`; content identical except include lines and one README-pointer comment, `VpvSection.h:2`). Seven one-line forwarding headers stay at `src/VP-V/config/` (listed in `MANIFEST.json` `forwarding_headers`) so Bill's Config files (`ConfigEditorWindow.h/.cpp`, `AppMain.cpp`, `ConfigEditorWindowTests.cpp`) keep their include lines unchanged; they are retired at the next Bill-merge window. Next Bill-merge window list: Bill's `ConfigEditorWindow.cpp` comments at :133, :5449, :10436 and :12454 still name the old `src/VP-V/config/README.md` path; update them (and retire the 10 include sites that go through the forwarders; `LVR` includes `control\config` directly) in that window. There is no `control\config_ui` and no `control\intent` module (rule 25: docs 60/77 names redirect here). The Settings table above is the module's one doc-70 table; the Config-side keys it lists are placed by `config_pages/`. `VpvOrganisation.h` is the one placement table; `tools/check_config_placement.py` (test `testPlacementTable`) holds the module README placements to it.

## Config pages (`config_pages/`: the VP-V Config section)

The VP-V section is the single Config page that holds everything VP-V adds, by unit. It
contains:
- the pathway switch (VP-V or Original);
- the global tier;
- the "Fallbacks engaged" line;
- a "Proposed retirement" tab (the settings guide's section-12 candidates from `vpv-tiers.json` `proposed_retirement`; the same note is appended to each upstream control's tooltip by `MarkRetirements`; nothing is removed);
- one tab per unit (Input, Motion, Repair, Scaling, Colour, Dynamic Drive, Output). Each tab has
  the unit's tier slider, the settings listed under it, the planned settings ("not yet
  implemented"), a "More settings of this unit" pointer (`more_settings`) and the unit's stages with
  their fallback text.

It is **self-contained** (owner 2026-10-24): it can be plugged into a fresh copy of upstream's
`ConfigEditorWindow` at the M2 rebuild. ADR: `docs/decisions/ADR-0023-vpv-config-section.md`.

### Files

| File | What |
|---|---|
| `VpvTierModel.h` | Pure C++ (no Qt): the data types, tier matching, tier writes, global mapping, managed keys. |
| `VpvTierTable.generated.h` | GENERATED by `tools/gen_vpv_tiers.py` from `docs/design/vpv-tiers.json` + `docs/design/slots.json`. Do not edit. |
| `VpvSection.h/.cpp` | The Qt page, built on `Host` (below). It never includes `ConfigEditorWindow.h`. |
| `VpvOrganisation.h` | Pure data: the cards of each unit tab and what each holds (VP-V toggles, upstream Rendering rows and sections moved there, notes); Bill's Rendering sections that stay. Map for people: `docs/settings/organisation.md`. |
| `../tests/VpvConfigTests.cpp` | Its tests, compiled into VideoProcessorConfigTests and run off screen with the rest of the Config suite. |
| `../../../core/DelayBudgetLadder.h` | The delay card's and the delay gate's helpers around the skeleton's `DelayBudget.h` (ADR-0020). |

### The integration hook (re-apply to an upstream ConfigEditorWindow)

The hook has four parts.

1. In `ConfigEditorWindow.cpp`, include the header:
   `#include <VP-V/modules/control/config/config_pages/VpvSection.h>` (the src root is on every project's include path, Directory.Build.targets).
2. In `createShell()`, after the last `pages_->addWidget(...)`, add the page:
   ```cpp
   const int vpvPage = pages_->count();
   {
       Vpv::Host vpvHost;
       vpvHost.value = [this](const QString& section, const QString& key) { return value(section, key); };
       vpvHost.setValue = [this](const QString& section, const QString& key, const QString& text)
       {
           if (!document_) return;
           if (text.isEmpty()) document_->RemoveKnown(section.toStdString(), key.toStdString().c_str());
           else document_->SetKnown(section.toStdString(), key.toStdString().c_str(), text.toLocal8Bit().constData());
           markDirty();
       };
       vpvHost.profileSections = [this](const QString& root) { return profileSections(root); };
       vpvHost.upstreamControlName = [](const QString& key) { return controlName(QStringLiteral("vprenderer"), key); };
       vpvHost.upstreamRoot = pages_;
       pages_->addWidget(createPage(QStringLiteral("VP-V"), QStringLiteral("..."), Vpv::CreateSection(vpvHost)));
   }
   ```
3. With the other navigation leaves, add one:
   `addLeaf(QStringLiteral("VP-V"), vpvPage)->setObjectName(QStringLiteral("config.openVpv"));`
   The VP-V leaf closes Bill's list (after Updates), and its sub-pages (Dynamic Drive, Calibration)
   get child leaves right below it (`VpvConfigPages::addNavigationLeaves`).
4. Add the source files to the projects:
   - `..\VP-V\modules\control\config\config_pages\VpvSection.cpp` in `VideoProcessor-Config.vcxproj`;
   - `..\VP-V\modules\control\config\config_pages\VpvSection.cpp` and `..\VP-V\modules\control\config\tests\VpvConfigTests.cpp` in
     `VideoProcessor-ConfigTests.vcxproj`.
   The src root must be on the include path (`Directory.Build.targets` adds it for every project).

   Then add one line to the suite's `main`, before its no-match exit:
   `failures += RunVpvConfigTests(run, require, save);`.

Outside Config, `RendererProfileConfig.h` accepts the namespaced `[vpv]` / `[vpv.*]` sections
(`OwnsSection` and the model reader).

### Organisation (passes 1 and 2, 2026-10-24)

Each unit tab has **Main** (summary, tier slider, taste master, switch cards) and **Advanced** (cards
whose settings a master on Main sets, feature tuning, measured internals, planned, stages), from
`VpvOrganisation.h`. VP's own Rendering-page rows and sections MOVE into the cards (one live control
per key); they stay bound to the Rendering page's selected profile, and the VP-V profile picker
follows that list both ways. After a VP-V write the page re-emits the list's `currentItemChanged`,
so Config reloads the moved rows.

- **Locks (master -> derived):** Tier / Slider / Defaults cards are greyed "Set by <master>" until
  their Custom box is ticked (auto-ticked when the values differ; unticking puts the master's values
  back); Feature cards are greyed "Used only when <feature> is on". Per-key locks set Config's own
  `derivedSetting` property so a profile reload keeps them greyed.
- **One live control per key:** rows of keys a VP-V toggle owns are hidden wherever they are, in
  both pathways; tier keys' rows live in "Set by the tier" (`vpvKeyHome`) and are their one control.
  The Original pathway disables the whole VP-V page (every VP addition is off) and shows a note;
  none of Bill's keys is on it. The Rendering page's raw group shows only while it holds a control.
- **One slider per unit:** the Classic Motion tier greys the de-judder amount; below Mid the picture
  slider is greyed and never offers its own Off on the VP-V pathway.
- **Dynamic Drive:** the page becomes Main | Advanced (its two-column grid is replaced): masters
  (drive, command source, LED profile, speeds preset, connection) on Main; the speeds' values (set by
  the preset until Custom), Contrast and cuts, the Advanced LED groups (`LedAdvancedGroups`, one card
  each, set by "Advanced LED values" until Custom), the projector's limits and Experimental on
  Advanced. "LED fixes under test" masters the nine LED fix switches.
- **De-judder:** the eight fields the de-judder amount sets are their own card (locked to the slider);
  the de-judder's independent shaping is another. Config's `showIn` follows a row to the form that
  holds it now (one commented change in ConfigEditorWindow.cpp).
- **Fit:** `FitWidth` stacks wide radio rows, puts every form field under its label and lets tab rows
  scroll, so the page does not scroll sideways at 1040x700 (tested with the Windows fonts by a second
  pass of `tools/run_config_tests.ps1`).

Upstream footprint of the moves: in `ConfigEditorWindow.cpp`'s profile refresh, the lookups of the
renderer's refresh actions and content-gate widgets also search the whole editor when they miss
`profileFields` (`findProfileWidget` / `findProfileAction`, commented). Re-apply with the hook.

### Config usability package (2026-10-25)

Owner decisions of 2026-10-25, built on the pages above:

- **Dynamic Drive Main** holds only on/off, the command source, the LED profile, the LED speeds preset and
  the projector link (`DriveMainKeys()`); the rest of the first card moves whole to an Advanced "behaviour
  and tuning" card (so the page's own show/hide logic keeps working), the two response times and the reply
  reading go to "Projector link: timing and replies". Every greyed control says why (`SetGreyReason`, the
  banner and the behaviour card's reasons).
- **Four tier steps everywhere**: a unit's `plannedTiers` give a fourth slider position, "Upper (planned -
  uses Mid for now)"; moving onto it snaps back and writes nothing.
- **"Set by" per setting** (`VpvSetBy.h`, pure): a small line under each setting's row on the unit tabs
  (Original pathway / a master / a profile rule / you / the default profile / VP's built-in default; and
  "Not used now: ..." when a feature switch leaves it unused). A disabled Main card collapses to
  "Turns on at <Unit> <Tier>".
- **Approve / Keep per retirement group**: recorded as `[vpv] retirement_a..d = approve | keep` (validator
  in `RendererProfileConfig.h`; no render effect, so no Original-pathway entry). Nothing is hidden.

### Rules

- **Tiers are presets of existing keys.** No key stores a tier; it is read back from the values,
  which gives "Custom" when a value differs. Only keys whose value changes are written.
- **Global VP-V keys go in `[vpv]`.** These are the skeleton's renderer keys, validated in
  `RendererProfileConfig.h`. The renderer reads `[vpv]` first, then `[vprenderer]`.
  - `pipeline_original_pathway` on|off: the pathway switch; on = every VP addition off
    (ADR-0019).
  - `pipeline_order`: the expert page. The order is checked with `PipelineSlots::Registry`, and
    a rejected order shows its plain-language reason and is not saved (ADR-0021).
  - `pipeline_slots_off`: the expert page; unknown slot names are not saved.
  - `calibration_lut_stage` libplacebo|vp: the expert page (R1).

  A default is written as an absent key, so untouched configs stay byte-identical.
- **Managed controls.** While the VP-V pathway is on, upstream controls for the keys VP-V
  manages are disabled with a "Managed by VP-V" tooltip. This covers the tier keys and the
  toggles, never the motion taste. In Original pathway mode they are restored exactly as they
  were (their previous enabled state and tooltip).
- **"Fallbacks engaged" is mirrored, not duplicated.** The section shows the running renderer's
  heading from the footer label `config.live.fallbacks`, which is fed by the r6
  `FallbackStatus` registry through ActiveProfileStatus v11.
- **Changing a tier.** Edit `docs/design/vpv-tiers.json`, run `python -I tools/gen_vpv_tiers.py`,
  then run the Config tests with `-Match "VP-V"`. `--check` is part of
  `tools/run_chain_gate.ps1`.

### Helpers called from Bill's `ConfigEditorWindow.cpp` (v1.4.02 merge)

Bill's editor file only gets call sites; the logic is here (header-only):

- `VpvBothPathwayCards.h` (P8 step 2): the VP-V rows that used to sit on Bill's pages and act on both
  pathways (12-bit YCbCr capture, the DV hand-off and SDR veto, problem reports, global shortcuts, and
  Bill's `bounded_invalid_capture_recovery`, which his editor has no control for). Built through the
  EditorKit (same object names, keys, defaults); `Host::bothPathwayCards` places them below the unit tabs,
  outside the part the Original pathway greys. Bill's General, DV, Logs and Shortcuts pages are his again.
- `VpvScalingRows.h` (P8 step 2): VP-V's rows of the Scaling profile page (VP-V's chroma list
  `chroma_upscaler.vpv`, `chroma_upscale_vp`, `downscale_light`; VP-V's Dithering list `dithering.vpv` with
  its Auto status), built by Bill's `addChoice` (same keys, inheritance, Reset, save) into forms of their own
  that the VP-V page shows as unit cards (`Host::unitCards`). Bill's Chroma upscaler and Dithering controls
  stay his (his lists, his text); each pair follows the other's user edits. One `VPV-HOOK` call in his
  Scaling section; the loader skips his retired-dither mapping only for `vpvOwnChoices` controls.
- `VpvProfileUseRows.h` (P8 step 2): the frame-rate rule and the queue's "Use this queue" choice, on a
  VP-V "Profile use" card (On both pathways) whose picker follows Bill's Queue / Rendering profile list.
  They write Bill's own rule editor and "Use rule" check; his placeholder is his again.
- `VpvQueueBillGrey.h` (CONFIG lane, 2026-11-11): OnBillField provider for the Queue page. VP-V pathway: greys Bill's Active-picture lookahead and (while Target delay > 0) Target frames with a pointer to the VP-V page; Original: greys VP-V's own queue controls, Bill's page untouched. Bill's Buffering-policy combo is not a bound field (not reachable here) and stays active; it follows the pathway through the preset table (DIV-CFG-09). A greyed Bill control is a `derivedSetting`, so the save-time freeze of inherited values skips it on purpose: VP-V's own control writes the key. Uses `derivedSetting`; no Bill line.
- `VpvQueueRows.h` (row c, RESTORED-CFG-04): VP-V's rows of the Queue profile page (VP's low-latency
  path, Target delay and its note, Queue target By frames / By time, the look-ahead note) and VP-V's
  look-ahead with Auto (`config.queue.active_picture_lookahead_frames.vpv`, explicit-0 notice) on the VP-V
  card "Queue target and low latency" (On both pathways), built by Bill's addChoice / addInteger and bound to
  the queue profile selected on his list. Bill's Queue form is his a591ad3b form (one `VPV-HOOK` call); his
  0..8 look-ahead and VP-V's follow each other (DIV-D-10). Also the loader / preset hooks of these rows.
- `VpvDitherChoices.h`: the Dithering choice sets: Bill's four (blue_noise, ordered_fixed,
  white_noise, off) on his Scaling-page control, always; VP-V's full list (Auto, ordered_lut, the
  error_diffusion_* modes) on VP-V's own control (`VpvScalingRows.h`, P8 step 2). Neither rewrites a
  stored value.
- `VpvRenderingDefaults.h`: what an unset VP-V key shows on a Rendering profile (moved out of
  Bill's file) and `IsVpvChoiceKey`, the VP-V choice keys that keep their "Inherited / not set"
  item and whose explicit `auto` is a live value (Bill's concrete-only rules are for his keys).
- Page lookups are by name (`ConfigEditorWindow::pageIndex`, `selectPage(QString)`): Bill's pages
  keep his order and numbers (0-22); Dynamic Drive, Calibration and VP-V follow them (23-25).
  In the navigation list the VP-V leaf closes Bill's list and has child leaves (Dynamic Drive,
  Calibration; properties `vpvChild`/`vpvParent`).

## Contract

- Input: slots.json + config schema
- Output: Config UI + validated settings
- Geometry: n/a
- Delay share: n/a
- Look-ahead: n/a

## Settings

- Existing keys: the moved settings table above (generated; `python -I tools/gen_control_config_readme.py`).
- Planned keys: (none)
- Tiers: n/a

## Fallbacks

- "Fallbacks engaged" entry: (n/a)
- Behaviour: n/a

## Net fingerprint / upstream

- Fingerprint: (none)
- Upstream modules: (none)

## How to build / test

Build the solution via the shared build-slot runner (see AGENTS.md); tests single-threaded; GPU tests only on mains with `VP_TEST_ADAPTER=NVIDIA`.

Placeholder tests:
- (none yet)

Isolation tests:
- ConfigFileTests
- ConfigEditorWindowTests

Contract tests:
- every planned key exists once the settings agent lands

Chain tests:
- (none yet)

## Known issues / open items

- settings reference guide (item 54)

### Leftovers and aliases (M2 P3 closing list)

Still in host files after P3, each reported as a Bill-line edit or a data/model edit rather than a movable VP block:

- `chroma_upscaler`: the host's choice list is extended (`auto`, `upscaler`, `ewa_lanczossharp`, `lanczos`, `catmull_rom`
  added) and its default is `auto` (the host's is `bicubic`): `LibplaceboVideoRenderer.cpp` (`readChoice`, struct default)
  and `RendererProfileConfig.h` (`ValidateProfileSettingBuiltin`, scaling group; the legacy `[display]` known-key list also
  names `chroma_upscaler`, `deband_grain`, `downscale_light`). A Bill behaviour change; divergence register. `PostLoad`
  returns the Original value (`bicubic`) through `Overrides`.
- `ApplyProfileSettingsLiveLocked` side effects (capture atomics, DV formatter): move with P10 (capture) and P11.
- `debandGrain` delta term moved to `AppendDelta`; the host's "profile delta" text stays Bill's (`DescribeProfileSettingsDelta`
  is Bill's function) and the VP names follow as `; vp_fields=a,b` appended at its one call site
  (`RendererSettingsHooks::AppendDeltaText`); `BillIdenticalSchemaTests` compares Bill's function with the baseline text.
  With only VP settings changed the text reads `none; vp_fields=x` (and `Renderer rebuild: none; vp_fields=x`): Bill's `none` is kept.
- Data/model edits in `RendererProfileConfig.h` that are not validators: queue profile fields (`targetLatencyMs`,
  `lowLatencyPath`), viewport `blank_*`, `IsContentClassifierInput/IsContentGated`, `DefaultSectionNotAppliedWarning`,
  the `persist_profile_selection` default (opt-in), extra `when:` variables (`hdr_inferred`,
  `ambient`, `content`, `source`), the `Read` / `ReadUnchecked` split. Divergence register entries; each is a bug fix or
  a VP data field and stays until the owner decides.
- Other host-file lines P3 leaves: two VP includes in `LibplaceboVideoRenderer.cpp` (`VpvSettingsDump.h` mid-file,
  `RendererSettingsExtension.h`), the 19-line test-only dump export there, and the 7-9-line `switch` hooks in
  `RendererProfileConfig.h` (wrapper and base block).
- `#include "RetiredSettings.h"` and `#include "SettingsExtension.h"` in `RendererProfileConfig.h`; `RefreshKey` in
  `ConfigFile.cpp`.
- `QueueRules()`, `GeneralRules()`, ... in `MainConfigSchema.h` are the VP refactor of Bill's inline rule lists (exposed
  for the Config coverage test); they hold Bill's keys in Bill's order (`bill-hunks.golden.txt`).
- Registry form of R2 (`GroupOf`, `KnownGlobalKeys`, `GeneralRules`) is the compile-time seam above; `GroupOf` is
  `ProfileSchemaHooks::Validate(group, ...)`, `KnownGlobalKeys("vpv")` is `IsKnownGlobalKey`.

## Where the code lives (live file list)

Code location: **legacy**. The authoritative list is `live_files` in [MANIFEST.json](MANIFEST.json).

- `src/VP-V/core/PipelineSlots.h`
- `src/VP-V/core/SlotContracts.h`
- `src/VP-V/core/OrderValidator.h`
- `src/VP-V/core/SlotRegistry.h`
- `src/VP-V/core/EntranceStages.h`
- `src/VP-V/core/OutputHookOrder.h`
- `src/VP-V/tests/PipelineSkeletonTests.cpp`
- `src/VideoProcessor-Config/ConfigEditorWindow.cpp`
- `src/VideoProcessor-Lib/ConfigFile.cpp`
- `src/VideoProcessor-Lib/ConfigSchema.h`
- `src/VideoProcessor-Lib/MainConfigSchema.h`
- `src/VideoProcessor-Lib/RendererProfileConfig.h`

## Migration directive

Code moves into this folder only when this slot is touched (opportunistic until M1, a dedicated phase after M1, finished by M2). A move = `git mv` + project/include updates + this module's `live_files` + `docs/design/components-master.json` (regenerate with `python tools/check_module_manifests.py --write`) in the SAME commit, with the build, this module's tests and the chain-gate subset passing. Never leave a copy behind; one module per change; never during a release/deploy window. Update this README in the same commit as any slot change.

## Links

- Design: [VP-pipeline-final.md](../../../../../docs/design/VP-pipeline-final.md)
- Slot data: [slots.json](../../../../../docs/design/slots.json)
- Decisions: ADR-0015 in [docs/decisions](../../../../../docs/decisions/README.md)

<!-- vpv-settings-tests:begin (generated by tools/vpv_tests/seed_module_readmes.py) -->

## Settings

- Keys: none yet.
- Tiers: n/a.
- Fallback entry ("Fallbacks engaged"): none (control slot).

## Tests

- Placeholder (byte-identity on/off, zero cost): n/a.
- Isolation (fixed inputs, expected outputs, cost): ConfigFileTests; ConfigEditorWindowTests.
- Contract (formats, geometry, fallback entry): every planned key exists once the settings agent lands.
- Chain (total delay at the 4/3/2-frame budgets): n/a.
- Combinations: pairwise / high-risk / random lines in `tests/vpv/` that set this slot's unit (`tools/run_combinations.ps1`); matrix: `docs/design/slot-test-matrix.md`.

<!-- vpv-settings-tests:end -->
