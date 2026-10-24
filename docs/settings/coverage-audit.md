# Settings coverage audit (2026-10-24)

For the owner's settings review. Checks the rule "every setting has a visible Config control"
(vp-shortcuts-while-watching) against every key VP actually accepts, flags what breaks it or
confuses it, and proposes a clean organisation. The full key-by-key inventory is section A at the
end; it is generated, not hand-kept.

## 1. The numbers

| | Count |
|---|---|
| Key entries VP accepts (section-qualified; 513 distinct names) | **563** |
| ... with a Config control (upstream page, VP-V toggle or VP-V tier) | **502** |
| ... without a control, allow-listed with a reason | **61** |
| ... without a control and not allow-listed (**violates the rule**) | **0** |
| ... accepted but changing nothing (**orphans**: retired, ignored, never read) | **17** |
| ... on the VP-V page | **52** (36 before this audit) |
| ... on the VP-V page and an upstream page (**managed duplicates**, upstream greyed under VP-V) | **44** |
| ... unmanaged duplicates (two live controls for one key) | **0** within a section; see 3.4 for layered overrides |
| ... in the settings guide | 212 |
| ... retirement candidates (guide section 12) | **41** marked: 39 to retire, 2 to hide |
| Controls that write a key VP does not accept (no effect) | **0** upstream; 8 VP-V placeholders, greyed by design |
| Shortcut actions / with a control on the Shortcuts page | 22 / 22 |

Where the controls are: Rendering page 177 keys, Dynamic Drive 103, General 49, Calibration 34,
Subtitles 26, Queue 23, Color / Output 22, DV 14, Input processing 12, Screen 8, Motion 8, Zoom 6,
Logs 5, Scaling 5, Shortcuts 2, VP-V only 8.

## 2. How it was measured

- **Keys**: the Config test exe's `--inventory` mode walks the same schema sources as the coverage
  test `every schema key has a Config control` (renderer validators; `[general]`, `[queue]`,
  `[queue_recovery]`, `[directshow*]`, `[decklink]`, `[lldv]`, `[logging]`, `[shortcuts]`,
  `[ambient]`, `[calibration]`; `[led_drive]`/`[dynamic_drive]` and `[projector]`; `[vpv]`), plus
  the tone-mapping tuning table and the retired keys (see 3.6). VP rejects any other key in its
  sections, so this is the complete set of keys VP can read.
- **Controls**: every `config.*` / `vpv.*` widget of a real (off-screen) Config window, with its
  page, card or tab, tooltip, and what it shows for an unset key (the default column).
- **Reads**: each key as a literal in the player/renderer sources outside the validators.
- **Tier, toggle, guide, retirement**: `docs/design/vpv-tiers.json` and
  `docs/settings/VP-settings-reference.md`.
- Re-run: `VideoProcessorConfigTests.exe --inventory inv.tsv` (QT_QPA_PLATFORM=offscreen), then
  `python -I tools/settings_coverage_audit.py inv.tsv`.

Shaders (`[shaders]`, `[shader.*]`) and actions (`[actions.*]`) are lists of entries edited on the
Shaders and Actions pages (each entry's fields have controls there); they are outside the key
count. Keyboard shortcuts are listed in A (all 22 have a control).

## 3. Findings

### 3.1 No control (the rule)
None. Every accepted key has a control or an allow-list reason. The 61 allow-listed keys are:
- 17 that change nothing (3.2);
- about 15 aliases / older spellings of a key that has the control (`windowedfullscreenmode`,
  `steady_reserve_frames`, `live_profile_updates`, `rate_up`, ...);
- about 21 legacy keys and legacy locations that Config migrates on load (`[general]
  frame_offset`, `queue_size`, the old subtitle padding keys, ...);
- 4 edited by another control (Dolby Vision mode buttons, `[general]` policy keys);
- 2 diagnostics (`subtitle_bbox_test`, `subtitle_cut_paste_test`) and 2 command-line/session
  keys (`dolby_vision_tunnel`, `interface`).

**Gap found in the coverage test itself (fixed):** it only saw keys validated as `key == "x"`.
The 16 tone-mapping tuning keys (`knee_*`, `slope_*`, `percentile`, `black_cutoff`, ...) and the
3 retired keys were invisible to it. The tone keys do have controls (Rendering page sliders); the
test now includes both groups.

### 3.2 Orphans (accepted, no effect)
| Key | Section | Why |
|---|---|---|
| `fix_scene_cooldown`, `dejudder_picture_area`, `dither_vectors_new_frame` | renderer | Retired 2026-10-17 (RetiredSettings.h): the fixed behaviour always applies. |
| `listen_port`, `listen_log`, `listen_script`, `python`, `python_drives_port`, `python_feed` | led_drive | The retired Python relay (removed 2026-10-15). |
| `calibration_lut_input_gamma`, `subtitle_fit`, `subtitle_detection_optimization`, `mode` | renderer | Retired or overridden (the Subtitles profile resets the subtitle key; `mode` never changes the geometry). |
| `subtitle_reposition`, `disable_detection_features`, `scene_correction_mode`, `scene_correction_basic` | general | Validated, not read at runtime (command line / compatibility editing only). |

**Proposal:** keep accepting them (old files must load) but log one line when present, and
drop them from the schema list shown to people. They need no control.

### 3.3 Controls with no effect
- Upstream: none. Five controls are not named after a key, all checked as UI helpers (DV mode
  buttons, the anamorphic enable box, the SDR response preset, the experimental fade unlock).
- VP-V: the 8 greyed "not yet implemented" boxes are placeholders by design (encoding repair,
  label detector, motion look-ahead in ms, 4K route, refine net, VP chroma upscale, D refine,
  combined HDR LUT, appearance, black level correction).
- **Fixed:** the Output tab listed "Calibration LUT applied by VP" as *not yet implemented*,
  but it is built: `[vpv] calibration_lut_stage` on the Pipeline (expert) tab sets it and the
  renderer reads it. The misleading greyed box is gone.

### 3.4 Duplicates and conflicts
- **Managed duplicates (44, by design):** each VP-V toggle or tier key also has its old control on
  the Rendering page. While the VP-V pathway is on, that control is greyed "Managed by VP-V", so
  only one is live. The exception is intended: the motion taste (`dejudder_strength`,
  `dejudder_strength_even`) stays live on the Motion/Rendering page and the Motion tier's Classic
  step also writes it.
- **Layered overrides (upstream, Bill's):** `video_conversion`, `container_colorspace`,
  `hdr_colorspace`, `hdr_luminance` each have three controls ([general] on General, and the
  [directshow] / [vprenderer.input_processing] overrides on Input processing). It is deliberate
  (the overrides win) but reads as three answers to one question. Proposal: show the effective
  value beside each override. Not changed here (Bill's pages).
- **One key, two profile kinds:** `sdr_input_transfer` is accepted in Rendering profiles and in
  Color/Output profiles, but Config shows it only on Color/Output. The cinema configuration sets
  it in both. Proposal: decide which wins and migrate the other (upstream; flagged only).
- **Same name, different settings:** `enabled` ([led_drive], [logging], actions), `mode` (legacy
  renderer and legacy LED), `when`/`shortcut`/`cycle_shortcut` (every profile kind). The coverage
  test is per section, so they cannot hide each other; the names are fine where the page makes the
  context clear.
- Minor: two widgets share the object name `config.vprenderer.intent_black.unset`.

### 3.5 Confusing names (proposed clearer labels; keys stay)
| Keys | Problem | Proposal |
|---|---|---|
| `render_ahead`, `motion_ahead_frames`, `dejudder_estimate_ahead` | Three "ahead"s doing different jobs (guide 10). | Labels already say presentation / vectors / load balancing; retire `motion_ahead_frames` for `motion_ahead_ms`. |
| `deband`, `deband_strength`, `deband_vp` (+ `deband_grain`) | Two debanders, a legacy switch and a grain add-on. | VP-V Repair tier owns the choice; retire `deband_grain`; `deband` stays a migrated alias. |
| `temporal_dither`, `dithering`, `dither_vectors`, `dither_grain_tolerance_scale` | VP's dither vs libplacebo's dither vs the motion source. | Output tier owns `temporal_dither`; retire `dither_vectors`; label `dithering` "libplacebo dither (when VP's is off)". |
| `source_resolution`, `source_range`, `source_class` | One word, three meanings (size / levels / film-or-video). | Labels: "Encoded size (re-base)", "Signal range", "Cadence classifier". |
| `adaptive_downscale`, `adaptive_upscale`, `adaptive_faithful`, `adaptive_w_base`, `adaptive_upscale_w_base` | "adaptive" says nothing. | Group under VP-V Scaling as "Grain-aware scaler"; weights into the expert group. |
| `lookahead`, `led_lookahead_ms`, `script_lookahead`, `active_picture_lookahead_frames` | Four look-aheads in three units. | Prefix labels with the unit (Dynamic Drive / crop detector). |
| `*_fixes` (14 switches) | Opaque names. | Retire (group A) after a clean chain gate. |
| `intent_content`, `intent_motion`, `intent_black`, `intent_display` | One prefix across four units. | Now placed by unit on VP-V (content = Input, motion = Motion); `intent_black`/`intent_display` stay diagnostics. |

### 3.6 Retirement candidates
The guide's section 12 list is now in `vpv-tiers.json` (`proposed_retirement`) and shown, never
removed:
- **A. Proven fixes under test** (25): the 14 renderer `*_fixes` and fix switches,
  `magewell_recover_faults`, `capture_display_fixes`, and the 9 LED "(fix under test)" switches
  (`led_peak_full_coverage`, `led_picture_only`, `script_resend_failed_writes`,
  `correction_weight_by_time`, `preposition_black_only`, `led_landing_ms`, `led_wtw_passthrough`,
  `led_btb_passthrough`, `cntr_pair_predict`). Retiring keeps the ON behaviour (BTB/WTW stay
  preserved).
- **B. Experiments that lost** (9): `optical_flow_cost_trust`, `dejudder_warp_stage`,
  `dejudder_occlusion_fill`, `dejudder_block_taper`, `dejudder_taper_sad`, `interpolation_grain`,
  `dejudder_source_warp_filter`, `nls_sampling`, `deband_grain`.
- **C. Superseded** (5): `render_ahead_queue_credit`, `motion_ahead_frames`, `detail_pulse_fast`,
  `dither_vectors`, `target_frames` (normal profiles).
- **D. Hide as diagnostics** (2): `dejudder_verify`, `dejudder_verify_share` (plus, as groups, the
  motion estimator's "measured, leave alone" internals and the adaptive-scaler thresholds).

If A-C are approved, VP drops 39 switches from the visible set.

## 4. Proposed organisation

One rule: **each unit has one home, the VP-V tab of that unit**, laid out top to bottom as
1. the unit's **tier slider** (where the unit has real steps);
2. its **toggles** (the few choices a viewer might make);
3. an **Expert** group, collapsed: the measured internals ("leave alone"), thresholds and weights;
4. **Diagnostics** only on a separate expert tab;
5. a pointer to anything that still lives on another page.

| Unit | Tier keys | Toggles (VP-V now) | Expert (to move in) | Still elsewhere |
|---|---|---|---|---|
| Input | - | capture path / upload, native RGB, DV GPU unpack, source range, DV trims, content labels | queue mechanics (`lead_frames`, `startup_preroll_frames`, ...) | Queue page (total delay), DV page, General (capture card) |
| Motion | optical flow, motion ahead, split blocks | hiccup repair, interpolation, interpolation fallback, estimate ahead, dither vectors, render ahead, per-shot easing | the ~30 `dejudder_*` estimator internals, `optical_flow_global*`, `render_ahead_utilisation` | Motion page (taste slider, profiles) |
| Repair | VP deband, texture restore, detail pulse | grain guard, deband exclusive, low-bitrate handling, detail pulse fast | `detail_pulse_cap`, `texture_restore_cap`, `deband_vp_*` tuning | `deband_strength` (libplacebo) on Rendering |
| Scaling | source resolution, adaptive downscale, weight | re-base kernel match, grain-true filter, text / mixed-screen guards, grain map, bitrate-aware SSIM, residual stabilisers, grain-aware upscale | adaptive thresholds and weights, `source_resolution_*` timings | Scaling page (libplacebo filters) |
| Colour | - | extended range, SDR tone shape, float processing, room-light adaptation | `sdr_knee_*`, `sdr_white_headroom`, ambient details | Color / Output and Rendering (tone mapping, Bill's) |
| Dynamic Drive | - | (its own page) | Advanced LED / projector cards | Dynamic Drive page |
| Output | temporal dither | occlusion mask | `dither_grain_tolerance_scale` | Rendering (bit depth, libplacebo dither), expert tab (LUT stage) |

The biggest single problem is the Rendering page: **177 keys in one "Profile details" card**, 112
of them VP's own. The VP-V tabs should take them over unit by unit (each move greys the old
control as managed, so nothing is lost and Bill's layout is untouched), and the expert groups keep
the measured internals out of sight.

## 5. Fixes made in this branch (feat/settings-preview)

1. **16 new VP-V toggles** for VP's own per-profile keys that had a control only on the Rendering
   page: `source_range`, `dv_trims`, `intent_content` (Input); `interpolation_fallback`,
   `intent_motion` (Motion); `low_bitrate_handling` (Repair); `source_resolution_text_guard`,
   `source_resolution_mixed_guard`, `adaptive_grain_map`, `ssim_bitrate_aware`,
   `ssim_residual_stabilise`, `cnn_residual_stabilise`, `adaptive_upscale` (Scaling);
   `fbo_float32`, `ambient_adapt` (Colour); `occlusion_mask` (Output). Each shows "Not set
   (default: ...)"; its Rendering-page control greys as managed.
2. **"More settings of this unit"** line on every unit tab, naming the pages that hold the rest.
3. **"Proposed retirement" tab** on the VP-V page: the 41 candidates by group, with where each
   control is and the value the profile uses; the same note is appended to each control's tooltip,
   and VP-V toggles that are candidates say "(proposed retirement)". Nothing is removed.
4. The wrong "not yet implemented" LUT-stage box removed from the Output tab.
5. The VP-V unit tabs are drawn on Config's dark card surface (they were light text on a white
   pane, hard to read).
6. Coverage test now also checks the tone-mapping tuning keys and the retired keys.
7. Tools: `--inventory` (this audit), `--resave` (the preview), `tools/settings_coverage_audit.py`.
8. Config `--sandbox` mode for the settings preview: edits only its `--config` file, never
   searches the LAN, reads live status, sends commands or signals a running VP, keeps its own
   preferences and has no tray icon.

Config tests (off screen): 162 run, all pass (two failures in the first full run were the
missing Beta renderer DLL, built since, and a 250 ms timing test that passes when run alone).

## 6. Not done here (for the owner / later)
- The retirements themselves (needs your yes per group).
- Moving the expert internals into collapsed VP-V groups (an organisation pass after your review
  of the preview).
- The upstream items in 3.4 (layered overrides, `sdr_input_transfer` in two profile kinds).

<!-- BEGIN GENERATED (tools/settings_coverage_audit.py) -->

## A. Generated inventory

Generated from the Config test exe's `--inventory` dump and the source; do not edit by hand. Re-run: `VideoProcessorConfigTests.exe --inventory inv.tsv` (offscreen) then `python -I tools/settings_coverage_audit.py inv.tsv`.

| Measure | Count |
|---|---|
| Keys VP accepts (schemas + tone tuning + retired) | 563 |
| ... with a Config control (upstream page, VP-V toggle or VP-V tier) | 502 |
| ... without a control, allow-listed with a reason (alias / legacy location / retired / diagnostic) | 61 |
| ... without a control and NOT allow-listed (violates the rule) | 0 |
| ... accepted but changing nothing (allow-list says retired / ignored / never read) | 17 |
| ... accepted but never read as a literal by the player/renderer (orphan candidates) | 0 |
| ... on the VP-V page (toggle or tier) | 52 |
| ... on the VP-V page AND an upstream page (managed duplicates: the upstream control greys under VP-V) | 44 |
| ... with controls on two or more upstream pages | 0 |
| ... in the settings guide | 212 |
| ... retirement candidates (guide section 12) | 41 |
| Upstream controls that write a key no schema accepts (no effect; UI helpers excluded) | 0 |
| Shortcut actions (compiled defaults) / with a Shortcuts-page control | 22 / 22 |

Flags: **NO CONTROL** = no Config control and no allow-list reason; **allow-listed** = no control by design (reason in the Notes column); **NO EFFECT** = accepted but ignored at runtime; **ORPHAN?** = accepted but no literal read in the player/renderer (see section 3 for the hand check); **DUAL (managed)** = on the VP-V page and an upstream page (the upstream one is greyed 'Managed by VP-V' while the VP-V pathway is on); **DUP pages** = controls on two upstream pages; **retire X** = guide section 12 group X.

### Input (34)

| Key | Section | Upstream control | VP-V | Tier | Default | Guide | Flags / notes |
|---|---|---|---|---|---|---|---|
| `active_picture_lookahead_frames` | queue | Queue > Profile details | - | - | 0 | s2 |  |
| `active_picture_lookahead_frames` | vprenderer | Queue > Profile details | - | - | 0 | s2 |  |
| `capture_path` | vprenderer | Rendering > Profile details | Input | - | gpu | s2 | DUAL (managed) |
| `capture_upload` | vprenderer | Rendering > Profile details | Input | - | Auto (early in the low-latency path) | s2 | DUAL (managed) |
| `dv_trims` | vprenderer | Rendering > Profile details | Input | - | Off | s2 | DUAL (managed) |
| `dv_tunnel_fixes` | vprenderer | Rendering > Profile details | - | - | On | s2 | retire A |
| `dv_tunnel_gpu_unpack` | vprenderer | Rendering > Profile details | Input | - | Off | s2 | DUAL (managed) |
| `dv_tunnel_handoff` | general | DV > Profile details | - | - | checked | s2 |  |
| `intent_content` | vprenderer | Rendering > Profile details | Input | - | Off | s3 | DUAL (managed) |
| `lead_frames` | queue | Queue > Profile details | - | - | frames | s2 |  |
| `lead_frames` | vprenderer | Queue > Profile details | - | - | frames | s2 |  |
| `low_latency_path` | queue | Queue > Profile details | - | - | - | s2 |  |
| `low_latency_path` | vprenderer | Queue > Profile details | - | - | - | s2 |  |
| `native_rgb_ingest` | vprenderer | Rendering > Profile details | Input | - | On | s2 | DUAL (managed) |
| `newlldv` | general | DV > Profile details | - | - | unchecked | s2 |  |
| `newlldv_sdr_veto` | general | DV > Profile details | - | - | checked | s2 |  |
| `queue_size` | general | only in Queue > Profile details | - | - | - | s2 | allow-listed: legacy location: VP reads [command_line] then [general] queue_size after the default queue profile (hunt 3, CFG9); Config moves both into [queue.*], which has the control |
| `queue_size` | queue | Queue > Profile details | - | - | 1 | s2 |  |
| `queue_size` | vprenderer | Queue > Profile details | - | - | 1 | s2 |  |
| `queue_timing_fixes` | vprenderer | Rendering > Profile details | - | - | On | s2 | retire A |
| `reset_after_render_restart_seconds` | queue | Queue > Profile details | - | - | 1 | s2 |  |
| `reset_after_render_restart_seconds` | queue_recovery | Queue > Profile details | - | - | 1 | s2 | legacy location: Config moves it into [queue.*], which has the control |
| `reset_after_render_restart_seconds` | vprenderer | Queue > Profile details | - | - | 1 | s2 |  |
| `reset_queue_too_large_percent` | queue | Queue > Profile details | - | - | 1 | s2 |  |
| `reset_queue_too_large_percent` | queue_recovery | Queue > Profile details | - | - | 1 | s2 | legacy location: Config moves it into [queue.*], which has the control |
| `reset_queue_too_large_percent` | vprenderer | Queue > Profile details | - | - | 1 | s2 |  |
| `source_coding_fixes` | vprenderer | Rendering > Profile details | - | - | On | s2 | retire A |
| `source_range` | vprenderer | Rendering > Profile details | Input | - | Auto (from the capture format) | s2 | DUAL (managed) |
| `startup_preroll_frames` | queue | Queue > Profile details | - | - | frames | s2 |  |
| `startup_preroll_frames` | vprenderer | Queue > Profile details | - | - | frames | s2 |  |
| `target_frames` | queue | Queue > Profile details | - | - | frames | s2 | retire C |
| `target_frames` | vprenderer | Queue > Profile details | - | - | frames | s2 |  |
| `target_latency_ms` | queue | Queue > Profile details | - | - | Off | s2 |  |
| `target_latency_ms` | vprenderer | Queue > Profile details | - | - | Off | s2 |  |

### Motion (50)

| Key | Section | Upstream control | VP-V | Tier | Default | Guide | Flags / notes |
|---|---|---|---|---|---|---|---|
| `dejudder_consistency_px` | vprenderer | Rendering > Profile details | - | - | 2 | s3 |  |
| `dejudder_cut_histogram` | vprenderer | Rendering > Profile details | - | - | 0.3 | s3 |  |
| `dejudder_est_candidates` | vprenderer | Rendering > Profile details | - | - | On | s3 |  |
| `dejudder_est_gate` | vprenderer | Rendering > Profile details | - | - | On | s3 |  |
| `dejudder_est_lambda` | vprenderer | Rendering > Profile details | - | - | 0.5 | s3 |  |
| `dejudder_est_qpel` | vprenderer | Rendering > Profile details | - | - | On | s3 |  |
| `dejudder_est_split` | vprenderer | Rendering > Profile details | - | Low=off, Mid=off, Upper=on | Off | s3 | DUAL (managed) |
| `dejudder_est_split_sad` | vprenderer | Rendering > Profile details | - | - | 0 | s3 |  |
| `dejudder_estimate_ahead` | vprenderer | Rendering > Profile details | Motion | - | On | s3 | DUAL (managed) |
| `dejudder_even_rates` | vprenderer | Rendering > Profile details | - | - | whole | s3 |  |
| `dejudder_fade_floor` | vprenderer | Rendering > Profile details | - | - | 0.25 | s3 |  |
| `dejudder_fade_match` | vprenderer | Rendering > Profile details | - | - | On | s3 |  |
| `dejudder_frame_trust` | vprenderer | Rendering > Profile details | - | - | 0.25 | s3 |  |
| `dejudder_gate` | vprenderer | Rendering > Profile details | - | - | Auto (per frame with render ahead) | s3 |  |
| `dejudder_global_prior` | vprenderer | Rendering > Profile details | - | - | On | s3 |  |
| `dejudder_global_two` | vprenderer | Rendering > Profile details | - | - | On | s3 |  |
| `dejudder_hold_feather_px` | vprenderer | Rendering > Profile details | - | - | 4 | s3 |  |
| `dejudder_hold_hysteresis` | vprenderer | Rendering > Profile details | - | - | 2 | s3 |  |
| `dejudder_landing_check` | vprenderer | Rendering > Profile details | - | - | On | s3 |  |
| `dejudder_low_res` | vprenderer | Rendering > Profile details | - | - | all | s3 |  |
| `dejudder_match_tolerance` | vprenderer | Rendering > Profile details | - | - | 4.5 | s3 |  |
| `dejudder_motion_rolloff` | vprenderer | Rendering > Profile details | - | - | Off | s3 |  |
| `dejudder_object_strength` | vprenderer | Rendering > Profile details | - | - | 1 (off) | s3 |  |
| `dejudder_pulldown` | vprenderer | Rendering > Profile details | - | - | Off | s3 |  |
| `dejudder_regional_hold` | vprenderer | Rendering > Profile details | - | - | On | s3 |  |
| `dejudder_search_range` | vprenderer | Rendering > Profile details | - | - | 8 | s3 |  |
| `dejudder_strength` | vprenderer | Rendering > Profile details | - | Classic=0 | 0 = off; the slider sets it | s3 | DUAL (managed) |
| `dejudder_strength_even` | vprenderer | Rendering > Profile details | - | Classic=0 | 0 = off | s3 | DUAL (managed) |
| `dejudder_texture_tolerance` | vprenderer | Rendering > Profile details | - | - | 0.25 | s3 |  |
| `dejudder_trust_scale` | vprenderer | Rendering > Profile details | - | - | 0.6 | s3 |  |
| `dejudder_vector_smooth` | vprenderer | Rendering > Profile details | - | - | On | s3 |  |
| `dither_vectors` | vprenderer | Rendering > Profile details | Motion | - | De-judder's (auto) | s8 | DUAL (managed), retire C |
| `intent_content_hold_seconds` | vprenderer | Rendering > Profile details | - | - | 5 | s3 |  |
| `intent_ease_s` | vprenderer | Rendering > Profile details | - | - | 1.5 | s3 |  |
| `intent_motion` | vprenderer | Rendering > Profile details | Motion | - | Off | s3 | DUAL (managed) |
| `interpolation` | vprenderer | Rendering > Profile details | Motion | - | Off | s3 | DUAL (managed) |
| `interpolation_fallback` | vprenderer | Rendering > Profile details | Motion | - | hold | s3 | DUAL (managed) |
| `interpolation_fill_gaps` | vprenderer | Rendering > Profile details | Motion | - | On | s3 | DUAL (managed) |
| `interpolation_grain` | vprenderer | Rendering > Profile details | - | - | preserve | s3 | retire B |
| `motion_ahead_frames` | vprenderer | Rendering > Profile details | - | Classic=0, Low=0, Mid=4, Upper=4 | 4 | s3 | DUAL (managed), retire C |
| `optical_flow` | vprenderer | Rendering > Profile details | - | Classic=off, Low=off, Mid=on, Upper=on | On | s3 | DUAL (managed) |
| `optical_flow_cost_trust` | vprenderer | Rendering > Profile details | - | - | Off | s3 | retire B |
| `optical_flow_fb_check` | vprenderer | Rendering > Profile details | - | - | Off | s3 |  |
| `optical_flow_global` | vprenderer | Rendering > Profile details | - | - | On | s3 |  |
| `optical_flow_global_fallback` | vprenderer | Rendering > Profile details | - | - | On | s3 |  |
| `pulldown_fixes` | vprenderer | Rendering > Profile details | - | - | On | s3 | retire A |
| `render_ahead` | vprenderer | Rendering > Profile details | Motion | - | Off | s3 | DUAL (managed) |
| `render_ahead_queue_credit` | vprenderer | Rendering > Profile details | - | - | Off | s3 | retire C |
| `render_ahead_utilisation` | vprenderer | Rendering > Profile details | - | - | 0.85 | s3 |  |
| `source_class` | vprenderer | Rendering > Profile details | - | - | legacy | s3 |  |

### Repair (11)

| Key | Section | Upstream control | VP-V | Tier | Default | Guide | Flags / notes |
|---|---|---|---|---|---|---|---|
| `deband_grain` | vprenderer | Rendering > Profile details | - | - | Off | s4 | retire B |
| `deband_strength` | vprenderer | Rendering > Profile details | - | - | Auto | s4 |  |
| `deband_vp` | vprenderer | Rendering > Profile details | - | Classic=off, Low=off, Mid=on, Upper=on | Off | s4 | DUAL (managed) |
| `deband_vp_exclusive` | vprenderer | Rendering > Profile details | Repair | - | On | s4 | DUAL (managed) |
| `deband_vp_grain_guard` | vprenderer | Rendering > Profile details | Repair | - | On | s4 | DUAL (managed) |
| `detail_pulse_cap` | vprenderer | Rendering > Profile details | - | - | 1.35 | s4 |  |
| `detail_pulse_fast` | vprenderer | Rendering > Profile details | Repair | - | On | s4 | DUAL (managed), retire C |
| `detail_pulse_stabilise` | vprenderer | Rendering > Profile details | - | Classic=off, Low=on, Mid=on, Upper=on | On | s4 | DUAL (managed) |
| `low_bitrate_handling` | vprenderer | Rendering > Profile details | Repair | - | Auto (measured) | s4 | DUAL (managed) |
| `texture_restore` | vprenderer | Rendering > Profile details | - | Classic=off, Low=off, Mid=off, Upper=statistical | Off | s4 | DUAL (managed) |
| `texture_restore_cap` | vprenderer | Rendering > Profile details | - | - | 1.50 | s4 |  |

### Scaling (24)

| Key | Section | Upstream control | VP-V | Tier | Default | Guide | Flags / notes |
|---|---|---|---|---|---|---|---|
| `active_picture_startup_hold` | vprenderer | Rendering > Profile details | - | - | On | s5 | retire A |
| `adaptive_chroma_ssim` | vprenderer | Rendering > Profile details | - | - | Off | s5 |  |
| `adaptive_downscale` | vprenderer | Rendering > Profile details | - | Classic=off, Low=on, Mid=on | Off | s5 | DUAL (managed) |
| `adaptive_faithful` | vprenderer | Rendering > Profile details | Scaling | - | Auto (polar; separable when only one axi | s5 | DUAL (managed) |
| `adaptive_grain_map` | vprenderer | Rendering > Profile details | Scaling | - | On | s5 | DUAL (managed) |
| `adaptive_upscale` | vprenderer | Rendering > Profile details | Scaling | - | Off | s5 | DUAL (managed) |
| `adaptive_upscale_superres_scale` | vprenderer | Rendering > Profile details | - | - | On | s5 |  |
| `adaptive_upscale_w_base` | vprenderer | Rendering > Profile details | - | - | 1 | s5 |  |
| `adaptive_w_base` | vprenderer | Rendering > Profile details | - | Low=0.2, Mid=0.2 | 1 | s5 | DUAL (managed) |
| `cnn_residual_stabilise` | vprenderer | Rendering > Profile details | Scaling | - | On | s5 | DUAL (managed) |
| `downscale_light` | vprenderer | Scaling > Profile details | - | - | - | s5 |  |
| `nls_sampling` | vprenderer | Rendering > Profile details | - | - | catmull | s5 | retire B |
| `rebase_kernel_match` | vprenderer | Rendering > Profile details | Scaling | - | On | s5 | DUAL (managed) |
| `residual_stabilise_grain_guard` | vprenderer | Rendering > Profile details | - | - | On | s5 | retire A |
| `source_resolution` | vprenderer | Rendering > Profile details | - | Classic=native, Low=native, Mid=auto | Auto (detected) | s2 | DUAL (managed) |
| `source_resolution_auto_min_confidence` | vprenderer | Rendering > Profile details | - | - | 0.8 | s5 |  |
| `source_resolution_engage_max_wait_s` | vprenderer | Rendering > Profile details | - | - | 20 | s5 |  |
| `source_resolution_latch_fixes` | vprenderer | Rendering > Profile details | - | - | On | s5 | retire A |
| `source_resolution_mixed_guard` | vprenderer | Rendering > Profile details | Scaling | - | On | s2 | DUAL (managed) |
| `source_resolution_text_guard` | vprenderer | Rendering > Profile details | Scaling | - | On | s2 | DUAL (managed) |
| `ssim_bitrate_aware` | vprenderer | Rendering > Profile details | Scaling | - | On | s5 | DUAL (managed) |
| `ssim_bitrate_floor` | vprenderer | Rendering > Profile details | - | - | 0.4 | s5 |  |
| `ssim_residual_stabilise` | vprenderer | Rendering > Profile details | Scaling | - | On | s5 | DUAL (managed) |
| `upscale_light` | vprenderer | Rendering > Profile details | - | - | linear | s5 |  |

### Colour (13)

| Key | Section | Upstream control | VP-V | Tier | Default | Guide | Flags / notes |
|---|---|---|---|---|---|---|---|
| `ambient_adapt` | vprenderer | Rendering > Profile details | Colour | - | Off | s6 | DUAL (managed) |
| `custom_tone_mapping` | vprenderer | Rendering > Profile details | - | - | unchecked | s6 |  |
| `dynamic_target` | vprenderer | Rendering > Profile details | - | - | checked | s6 |  |
| `extended_range_more_cases` | vprenderer | Rendering > Profile details | - | - | On | s6 | retire A |
| `fbo_float32` | vprenderer | Rendering > Profile details | Colour | - | Auto | s6 | DUAL (managed) |
| `intent_black` | vprenderer | Rendering > Profile details | - | - | Off | s6 |  |
| `intent_display` | vprenderer | Rendering > Profile details | - | - | Off | s6 |  |
| `preserve_extended_range` | vprenderer | Rendering > Profile details | Colour | - | On | s6 | DUAL (managed) |
| `sdr_contrast_recovery` | vprenderer | Rendering > Profile details | - | - | Auto | s6 |  |
| `sdr_knee_start` | vprenderer | Rendering > Profile details | - | - | 0.80 | s6 |  |
| `sdr_knee_strength` | vprenderer | Rendering > Profile details | - | - | 0.5 | s6 |  |
| `sdr_tone_shape` | vprenderer | Rendering > Profile details | Colour | - | Off | s6 | DUAL (managed) |
| `sdr_white_headroom` | vprenderer | Rendering > Profile details | - | - | 0 | s6 |  |

### Dynamic Drive (63)

| Key | Section | Upstream control | VP-V | Tier | Default | Guide | Flags / notes |
|---|---|---|---|---|---|---|---|
| `ack_timeout_ms` | projector | Dynamic Drive > Advanced projector | - | - | 300 | s7 |  |
| `adaptive_gamma` | led_drive | Dynamic Drive | - | - | unchecked | s7 |  |
| `anti_pump_band` | led_drive | Dynamic Drive > Advanced LED | - | - | 3 levels | s7 |  |
| `apl_persist_ms` | led_drive | Dynamic Drive > Advanced LED | - | - | 100 ms | s7 |  |
| `apl_weights` | led_drive | Dynamic Drive > Advanced LED | - | - | 0.114, 0.587, 0.299 | s7 |  |
| `black_prelift_max` | led_drive | Dynamic Drive > Contrast and cuts | - | - | 120 | s7 |  |
| `bright_cut_dump` | led_drive | Dynamic Drive > Advanced LED | - | - | checked | s7 |  |
| `cntr_latency_ms` | projector | Dynamic Drive > Projector connection | - | - | 80 ms | s7 |  |
| `cntr_max_usable` | projector | Dynamic Drive > Projector response | - | - | 74 | s7 |  |
| `cntr_pair_predict` | led_drive | Dynamic Drive > Contrast and cuts | - | - | checked | s7 | retire A |
| `command_source` | led_drive | Dynamic Drive | - | - | Compensate (VP's own controller) | s7 |  |
| `compensation_eotf` | led_drive | Dynamic Drive | - | - | 2.41 | s7 |  |
| `correction_weight_by_time` | led_drive | Dynamic Drive > Experimental | - | - | checked | s7 | retire A |
| `curve` | led_drive | Dynamic Drive > Advanced LED | - | - | 0:100, 20:100, 40:200, 70:230, 100:255,  | s7 |  |
| `cut_cntr_dump` | led_drive | Dynamic Drive > Contrast and cuts | - | - | checked | s7 |  |
| `cut_cntr_level` | led_drive | Dynamic Drive > Contrast and cuts | - | - | auto (white clip) | s7 |  |
| `cut_jump` | led_drive | Dynamic Drive > Advanced LED | - | - | checked | s7 |  |
| `cut_lead` | led_drive | Dynamic Drive > Contrast and cuts | - | - | Off | s7 |  |
| `cut_threshold` | led_drive | Dynamic Drive > Advanced LED | - | - | 24.0 | s7 |  |
| `deadband` | led_drive | Dynamic Drive > Advanced LED | - | - | 3 levels | s7 |  |
| `dmd_clip_cntr` | projector | Dynamic Drive > Projector response | - | - | 59 | s7 |  |
| `early_ramp` | led_drive | Dynamic Drive > Advanced LED | - | - | checked | s7 |  |
| `echo` | projector | Dynamic Drive > Advanced projector | - | - | checked | s7 |  |
| `enabled` | led_drive | Dynamic Drive | - | - | checked | s7 |  |
| `fade_min_slope` | led_drive | Dynamic Drive > Advanced LED | - | - | 12.0 /s | s7 |  |
| `fade_ms` | led_drive | Dynamic Drive > Advanced LED | - | - | 250 ms | s7 |  |
| `flux_table` | projector | Dynamic Drive > Advanced projector | - | - | empty = light proportional to the LED le | s7 |  |
| `highlight_percentile` | led_drive | Dynamic Drive > Advanced LED | - | - | 99.5 % | s7 |  |
| `highlight_persist_ms` | led_drive | Dynamic Drive > Advanced LED | - | - | 250 ms | s7 |  |
| `highlight_release_ms` | led_drive | Dynamic Drive > Advanced LED | - | - | 2000 ms | s7 |  |
| `highlight_tolerance_stops` | led_drive | Dynamic Drive | - | - | Medium | s7 |  |
| `knee` | led_drive | Dynamic Drive > Advanced LED | - | - | 0.08 | s7 |  |
| `led_btb_passthrough` | led_drive | Dynamic Drive > Contrast and cuts | - | - | checked | s7 | retire A |
| `led_ceiling` | led_drive | Dynamic Drive > Advanced LED | - | - | 255 | s7 |  |
| `led_floor` | led_drive | Dynamic Drive | - | - | 100 | s7 |  |
| `led_floor_black` | led_drive | Dynamic Drive | - | - | 60 | s7 |  |
| `led_floor_reactive` | led_drive | Dynamic Drive | - | - | 100 | s7 |  |
| `led_highlight_grain_suppress` | led_drive | Dynamic Drive > Contrast and cuts | - | - | checked | s7 |  |
| `led_idle_restore_s` | led_drive | Dynamic Drive | - | - | 10 s | s7 |  |
| `led_landing_ms` | led_drive | Dynamic Drive > Contrast and cuts | - | - | checked | s7 | retire A |
| `led_latency_ms` | projector | Dynamic Drive > Projector connection | - | - | 80 ms | s7 |  |
| `led_lookahead_ms` | led_drive | Dynamic Drive > Contrast and cuts | - | - | queue depth | s7 |  |
| `led_peak_full_coverage` | led_drive | Dynamic Drive | - | - | checked | s7 | retire A |
| `led_picture_only` | led_drive | Dynamic Drive | - | - | checked | s7 | retire A |
| `led_wtw_passthrough` | led_drive | Dynamic Drive > Contrast and cuts | - | - | checked | s7 | retire A |
| `lookahead` | led_drive | Dynamic Drive | - | - | checked | s7 |  |
| `max_cut_dip` | led_drive | Dynamic Drive > Contrast and cuts | - | - | 0.50 | s7 |  |
| `max_levels_per_command` | projector | Dynamic Drive > Advanced projector | - | - | 12 | s7 |  |
| `min_command_gap_ms` | projector | Dynamic Drive > Advanced projector | - | - | 200 | s7 |  |
| `preposition_black_only` | led_drive | Dynamic Drive > Contrast and cuts | - | - | checked | s7 | retire A |
| `protocol` | projector | Dynamic Drive > Projector connection | - | - | Barco serial (RS232) | s7 |  |
| `read_back` | projector | Dynamic Drive > Projector connection | - | - | unchecked | s7 |  |
| `recovery_bias_pct` | led_drive | Dynamic Drive > Contrast and cuts | - | - | 3.0 | s7 |  |
| `script_echo_ms` | led_drive | Dynamic Drive | - | - | 30 ms | s7 |  |
| `script_highlight_code` | led_drive | Dynamic Drive > Advanced LED | - | - | 215 | s7 |  |
| `script_lookahead` | led_drive | Dynamic Drive | - | - | checked | s7 |  |
| `script_measure_ms` | led_drive | Dynamic Drive | - | - | 150 ms | s7 |  |
| `script_pacing` | led_drive | Dynamic Drive | - | - | The script's own rhythm | s7 |  |
| `script_resend_failed_writes` | led_drive | Dynamic Drive | - | - | checked | s7 | retire A |
| `shaped_recovery` | led_drive | Dynamic Drive > Contrast and cuts | - | - | checked | s7 |  |
| `software_first` | led_drive | Dynamic Drive > Contrast and cuts | - | - | checked | s7 |  |
| `terminator` | projector | Dynamic Drive > Advanced projector | - | - | Automatic (try the projector) | s7 |  |
| `tick_ms` | led_drive | Dynamic Drive > Advanced LED | - | - | 400 ms | s7 |  |

### Output (5)

| Key | Section | Upstream control | VP-V | Tier | Default | Guide | Flags / notes |
|---|---|---|---|---|---|---|---|
| `display_bit_depth` | vprenderer | Rendering > Profile details | - | - | Auto | s8 |  |
| `dither_grain_tolerance_scale` | vprenderer | Rendering > Profile details | - | - | 3 | s8 |  |
| `dithering` | vprenderer | Rendering > Profile details | - | - | Auto | s8 |  |
| `occlusion_mask` | vprenderer | Rendering > Profile details | Output | - | Off | s8 | DUAL (managed) |
| `temporal_dither` | vprenderer | Rendering > Profile details | - | Classic=libplacebo, Low=sigma_delta | Auto (the shader list decides) | s8 | DUAL (managed) |

### No control of its own (allow-listed) (60)

| Key | Section | Upstream control | VP-V | Tier | Default | Guide | Flags / notes |
|---|---|---|---|---|---|---|---|
| `calibration_lut_input_gamma` | vprenderer | - | - | - | - | - | allow-listed, NO EFFECT: retired: accepted but no longer controls LUT / HDR encoding (hdr_tone_map_target_gamma has the control) |
| `calibration_lut_input_transfer` | vprenderer | - | - | - | - | - | allow-listed: alias accepted for HDR target (Rendering) |
| `deband` | vprenderer | - | - | - | - | s10 | allow-listed: legacy toggle; deband_strength has the control |
| `dejudder_picture_area` | vprenderer | - | - | - | - | - | allow-listed, NO EFFECT: retired (RetiredSettings.h): accepted with any value, ignored |
| `disable_detection_features` | general | - | - | - | - | - | allow-listed, NO EFFECT: compatibility editing only |
| `dither_vectors_new_frame` | vprenderer | - | - | - | - | - | allow-listed, NO EFFECT: retired (RetiredSettings.h): accepted with any value, ignored |
| `dolby_vision_tunnel` | general | - | - | - | - | - | allow-listed: command-line / per-session capture state |
| `dolby_vision_tunnel` | lldv | - | - | - | - | - | allow-listed: edited by the DV mode buttons (config.dv.mode.*) |
| `dolby_vision_tunnel` | vprenderer | - | - | - | - | - | allow-listed: edited by the DV mode buttons (config.dv.mode.*) |
| `fade_frames` | led_drive | - | - | - | - | - | allow-listed: alias: older fade length in frames at 24p (fade_ms has the control) |
| `fade_min_step` | led_drive | - | - | - | - | - | allow-listed: alias: older per-frame fade slope, read as fade_min_slope x 24 (fade_min_slope has the control) |
| `fix_scene_cooldown` | vprenderer | - | - | - | - | - | allow-listed, NO EFFECT: retired (RetiredSettings.h): accepted with any value, ignored |
| `frame_offset` | general | only in General > Timing | - | - | - | - | allow-listed: legacy location: Config moves [general] frame_offset to [directshow] frame_offset, which has the control |
| `interface` | general | - | - | - | - | - | allow-listed: command-line interface choice |
| `listen_log` | led_drive | - | - | - | - | - | allow-listed, NO EFFECT: retired (ignored) |
| `listen_port` | led_drive | - | - | - | - | - | allow-listed, NO EFFECT: retired (ignored) |
| `listen_script` | led_drive | - | - | - | - | - | allow-listed, NO EFFECT: retired (ignored) |
| `live_profile_updates` | vprenderer | - | - | - | - | - | allow-listed: alias: legacy switch for profile_update_mode live (which has the control) |
| `mode` | led_drive | only in General > Room lights | - | - | - | - | allow-listed: legacy: older files' mode; Config shows it as command_source (which has the control) and keeps the older switches only for such files |
| `mode` | vprenderer | only in General > Room lights | - | - | - | - | allow-listed, NO EFFECT: legacy Screen label (normal / scope): validated for compatibility, never changes the geometry; screen_aspect has the control |
| `new_lldv` | general | - | - | - | - | - | allow-listed: alias of newlldv |
| `no_ui` | general | - | - | - | - | - | allow-listed: alias of noui |
| `presentation_lead_frames` | directshow | - | - | - | - | - | allow-listed: legacy location: Config moves it to [queue.*] lead_frames, which has the control |
| `profile_update_mode` | vprenderer | only in General > Display | - | - | - | - | allow-listed: renderer policy read from [general]; the [general] control covers it |
| `python` | led_drive | - | - | - | - | - | allow-listed, NO EFFECT: retired (ignored) |
| `python_drives_port` | led_drive | - | - | - | - | - | allow-listed, NO EFFECT: retired (ignored) |
| `python_feed` | led_drive | - | - | - | - | - | allow-listed, NO EFFECT: retired (ignored) |
| `rate_down` | led_drive | - | - | - | - | - | allow-listed: alias: older single rate, superseded by rate_gain/min/max_down |
| `rate_up` | led_drive | - | - | - | - | - | allow-listed: alias: older single rate, superseded by rate_gain/min/max_up |
| `renderer_nominal_range` | general | only in General > Renderer overrides | - | - | - | - | allow-listed: legacy location: the [directshow] control replaces the [general] value on edit |
| `renderer_primaries` | general | only in General > Renderer overrides | - | - | - | - | allow-listed: legacy location: the [directshow] control replaces the [general] value on edit |
| `renderer_start_stop_time_method` | general | only in General > Timing | - | - | - | - | allow-listed: legacy location: the [directshow] control replaces the [general] value on edit |
| `renderer_transfer_function` | general | only in General > Renderer overrides | - | - | - | - | allow-listed: legacy location: the [directshow] control replaces the [general] value on edit |
| `renderer_transfer_matrix` | general | only in General > Renderer overrides | - | - | - | - | allow-listed: legacy location: the [directshow] control replaces the [general] value on edit |
| `scene` | general | - | - | - | - | - | allow-listed: alias of scene_detect |
| `scene_correction_basic` | general | - | - | - | - | - | allow-listed, NO EFFECT: compatibility editing only |
| `scene_correction_mode` | general | - | - | - | - | - | allow-listed, NO EFFECT: compatibility editing only |
| `script_smoothing` | led_drive | - | - | - | - | - | allow-listed: legacy: set from adaptive_gamma (which has the control) whenever command_source is named; read only from older mode-only files |
| `sdr_target_primaries` | vprenderer | - | - | - | - | - | allow-listed: legacy spelling of target_primaries |
| `start_minimized` | general | - | - | - | - | - | allow-listed: alias of startminimized |
| `steady_reserve_frames` | queue | - | - | - | - | - | allow-listed: alias: legacy spelling of target_frames; Config migrates it on load |
| `steady_reserve_frames` | vprenderer | - | - | - | - | - | allow-listed: alias: legacy spelling of target_frames; Config migrates it on load |
| `subtitle_bbox_test` | vprenderer | - | - | - | - | - | allow-listed: test / diagnostic overlay |
| `subtitle_box_padding_bottom` | vprenderer | - | - | - | - | - | allow-listed: legacy subtitle key |
| `subtitle_box_padding_bottom_percent` | vprenderer | - | - | - | - | - | allow-listed: legacy subtitle key |
| `subtitle_box_padding_mode` | vprenderer | - | - | - | - | - | allow-listed: legacy subtitle key (Subtitles profiles replace it) |
| `subtitle_box_padding_sides` | vprenderer | - | - | - | - | - | allow-listed: legacy subtitle key |
| `subtitle_box_padding_sides_percent` | vprenderer | - | - | - | - | - | allow-listed: legacy subtitle key |
| `subtitle_box_padding_top` | vprenderer | - | - | - | - | - | allow-listed: legacy subtitle key |
| `subtitle_box_padding_top_percent` | vprenderer | - | - | - | - | - | allow-listed: legacy subtitle key |
| `subtitle_cut_paste_background` | vprenderer | - | - | - | - | - | allow-listed: legacy subtitle key (rule-section background, migrated to the Subtitles profiles) |
| `subtitle_cut_paste_test` | vprenderer | - | - | - | - | - | allow-listed: test / diagnostic overlay |
| `subtitle_detection_optimization` | vprenderer | - | - | - | - | - | allow-listed, NO EFFECT: legacy subtitle key: accepted in renderer / Screen sections only, and the renderer resets it before applying the Subtitles profile (LVR, "Discard legacy subtitle settings"), so it never takes effect |
| `subtitle_fit` | vprenderer | - | - | - | - | - | allow-listed, NO EFFECT: retired Screen setting (ignored) |
| `subtitle_move_inset` | vprenderer | - | - | - | - | - | allow-listed: legacy subtitle key |
| `subtitle_padding_pixels` | vprenderer | - | - | - | - | - | allow-listed: legacy subtitle key |
| `subtitle_reposition` | general | - | - | - | - | - | allow-listed, NO EFFECT: command-line option (/subtitle_reposition); the [general] key is validated but never read at runtime |
| `switch_refresh_rate` | vprenderer | only in General > Display | - | - | - | - | allow-listed: renderer policy read from [general]; Config migrates it there and the [general] control covers it |
| `temporal_fill` | vprenderer | - | - | - | - | - | allow-listed: alias: legacy Subtitles switch for cleanup_fill temporal (cleanup_fill has the control) |
| `windowedfullscreenmode` | general | - | - | - | - | - | allow-listed: alias of windowed_fullscreen_mode |

### Upstream: Calibration (34)

| Key | Section | Upstream control | VP-V | Tier | Default | Guide | Flags / notes |
|---|---|---|---|---|---|---|---|
| `argyll_path` | calibration | Calibration > Meter | - | - | Search PATH and the usual install folder | - |  |
| `background_percent` | calibration | Calibration > Display and patches | - | - | 0.0 % grey | - |  |
| `display_kind` | calibration | Calibration > Display and patches | - | - | Projector | - |  |
| `display_name` | calibration | Calibration > Display and patches | - | - | The output monitor's name | - |  |
| `display_type` | calibration | Calibration > Meter | - | - | The meter's default | - |  |
| `drift_interval` | calibration | Calibration > Display and patches | - | - | 50 patches | - |  |
| `hdr_verify_source_peak` | calibration | Calibration > Dynamic Drive, room light and HDR runs | - | - | 1000 cd/m² | - |  |
| `hdr_verify_tone_curve` | calibration | Calibration > Dynamic Drive, room light and HDR runs | - | - | VP's own tone mapping | - |  |
| `led_cntr_dwell_ms` | calibration | Calibration > Dynamic Drive, room light and HDR runs | - | - | 1500 ms | - |  |
| `led_cntr_sweep_max` | calibration | Calibration > Dynamic Drive, room light and HDR runs | - | - | 74 | - |  |
| `led_dwell_ms` | calibration | Calibration > Dynamic Drive, room light and HDR runs | - | - | 3000 ms | - |  |
| `led_flux_step` | calibration | Calibration > Dynamic Drive, room light and HDR runs | - | - | 5 LPW1 steps | - |  |
| `led_reads_per_level` | calibration | Calibration > Dynamic Drive, room light and HDR runs | - | - | 2 readings | - |  |
| `led_sensor_baud` | calibration | Calibration > Dynamic Drive, room light and HDR runs | - | - | 115200 baud | - |  |
| `led_sensor_port` | calibration | Calibration > Dynamic Drive, room light and HDR runs | - | - | None (latency not measured) | - |  |
| `led_step_repeats` | calibration | Calibration > Dynamic Drive, room light and HDR runs | - | - | 3 | - |  |
| `lut_size` | calibration | Calibration > Target and LUT | - | - | 17³ | - |  |
| `meter_model` | calibration | Calibration > Meter | - | - | Automatic (from the connected meter) | - |  |
| `meter_port` | calibration | Calibration > Meter | - | - | First found | - |  |
| `near_black_max_reads` | calibration | Calibration > Display and patches | - | - | 4 readings | - |  |
| `patch_window_percent` | calibration | Calibration > Display and patches | - | - | Automatic | - |  |
| `plan` | calibration | Calibration > Target and LUT | - | - | Quick (greyscale + colours) | - |  |
| `read_timeout_s` | calibration | Calibration > Meter | - | - | 60 s | - |  |
| `record_transcripts` | calibration | Calibration > Dynamic Drive, room light and HDR runs | - | - | unchecked | - |  |
| `refresh_mode` | calibration | Calibration > Meter | - | - | Meter decides | - |  |
| `repeat_tolerance_percent` | calibration | Calibration > Display and patches | - | - | 1.0 % | - |  |
| `settle_ms` | calibration | Calibration > Display and patches | - | - | Automatic | - |  |
| `spectral_correction` | calibration | Calibration > Meter | - | - | None | - |  |
| `target_gamma` | calibration | Calibration > Target and LUT | - | - | 2.40 | - |  |
| `target_transfer` | calibration | Calibration > Target and LUT | - | - | BT.1886 (with the measured black) | - |  |
| `target_white_percent` | calibration | Calibration > Target and LUT | - | - | 100 % of the brightest D65 white | - |  |
| `transcript_folder` | calibration | Calibration > Dynamic Drive, room light and HDR runs | - | - | luts\calibration\transcripts | - |  |
| `verify_after` | calibration | Calibration > Target and LUT | - | - | checked | - |  |
| `warmup_minutes` | calibration | Calibration > Display and patches | - | - | 30 min | - |  |

### Upstream: Color / Output (22)

| Key | Section | Upstream control | VP-V | Tier | Default | Guide | Flags / notes |
|---|---|---|---|---|---|---|---|
| `calibration_lut_bt2020` | vprenderer | Color / Output > Profile details | - | - | None | - |  |
| `calibration_lut_bt709` | vprenderer | Color / Output > Profile details | - | - | None | - |  |
| `calibration_lut_enabled` | vprenderer | Color / Output > Profile details | - | - | unchecked | - |  |
| `calibration_lut_p3_d65` | vprenderer | Color / Output > Profile details | - | - | None | - |  |
| `diagnostic_allow_full_g22` | vprenderer | Color / Output > Profile details | - | - | unchecked | - |  |
| `diagnostic_allow_limited_g22` | vprenderer | Color / Output > Profile details | - | - | unchecked | - |  |
| `diagnostic_disable_compute` | vprenderer | Color / Output > Profile details | - | - | unchecked | - |  |
| `diagnostic_disable_shader_cache` | vprenderer | Color / Output > Profile details | - | - | unchecked | - |  |
| `diagnostic_force_8bit_sdr_swapchain` | vprenderer | Color / Output > Profile details | - | - | unchecked | - |  |
| `diagnostic_vp_owned_dxgi_presenter` | vprenderer | Color / Output > Profile details | - | - | unchecked | - |  |
| `hdr_tone_map_target_gamma` | vprenderer | Color / Output > Profile details | - | - | - | - |  |
| `output_diagnostics` | vprenderer | Color / Output > Profile details | - | - | unchecked | - |  |
| `output_gamma` | vprenderer | Color / Output > Profile details | - | - | - | - |  |
| `output_path_profile` | vprenderer | Color / Output > Profile details | - | - | - | - |  |
| `output_presentation` | vprenderer | Color / Output > Profile details | - | - | - | - |  |
| `output_range` | vprenderer | Color / Output > Profile details | - | - | - | - |  |
| `output_transport_gamma` | vprenderer | Color / Output > Profile details | - | - | - | - |  |
| `report_bt2020_to_display` | vprenderer | Color / Output > Profile details | - | - | unchecked | - |  |
| `sdr_adjust_gamma` | vprenderer | Color / Output > Profile details | - | - | unchecked | - |  |
| `sdr_input_transfer` | vprenderer | Color / Output > Profile details | - | - | - | - |  |
| `sdr_lut_input_gamma` | vprenderer | Color / Output > Profile details | - | - | - | - |  |
| `target_primaries` | vprenderer | Color / Output > Profile details | - | - | Rec. 709 | - |  |

### Upstream: DV (11)

| Key | Section | Upstream control | VP-V | Tier | Default | Guide | Flags / notes |
|---|---|---|---|---|---|---|---|
| `cycle_shortcut` | lldv | DV > Profile details | - | - | - | - |  |
| `mastering_max_luminance` | lldv | DV > Profile details | - | - | nits | - |  |
| `mastering_max_luminance` | vprenderer | DV > Profile details | - | - | nits | - |  |
| `mastering_min_luminance` | lldv | DV > Profile details | - | - | nits | - |  |
| `mastering_min_luminance` | vprenderer | DV > Profile details | - | - | nits | - |  |
| `max_cll` | lldv | DV > Profile details | - | - | nits | - |  |
| `max_cll` | vprenderer | DV > Profile details | - | - | nits | - |  |
| `max_fall` | lldv | DV > Profile details | - | - | nits | - |  |
| `max_fall` | vprenderer | DV > Profile details | - | - | nits | - |  |
| `shortcut` | lldv | DV > Profile details | - | - | - | - |  |
| `when` | lldv | DV > Profile details | - | - | - | - |  |

### Upstream: Dynamic Drive (40)

| Key | Section | Upstream control | VP-V | Tier | Default | Guide | Flags / notes |
|---|---|---|---|---|---|---|---|
| `baud` | projector | Dynamic Drive > Advanced projector | - | - | 19200 | - |  |
| `burst_commands` | projector | Dynamic Drive > Advanced projector | - | - | 0 | - |  |
| `burst_pause_ms` | projector | Dynamic Drive > Advanced projector | - | - | 5000 | - |  |
| `cntr_assist` | led_drive | Dynamic Drive > Contrast and cuts | - | - | checked | s10 |  |
| `cntr_assist_max` | led_drive | Dynamic Drive > Contrast and cuts | - | - | 100 | - |  |
| `cntr_settle_ms` | projector | Dynamic Drive > Projector response | - | - | 0 | - |  |
| `ftb_black_ms` | led_drive | Dynamic Drive > Advanced LED | - | - | 500 ms | - |  |
| `ftb_custom_dim` | led_drive | Dynamic Drive > Experimental | - | - | All three colours together | - |  |
| `ftb_custom_floor` | led_drive | Dynamic Drive > Experimental | - | - | 0 | - |  |
| `ftb_custom_start` | led_drive | Dynamic Drive > Experimental | - | - | 40 | - |  |
| `ftb_step_ms` | led_drive | Dynamic Drive > Experimental | - | - | 100 ms | - |  |
| `ftb_turn_step` | led_drive | Dynamic Drive > Experimental | - | - | 2 levels | - |  |
| `gamma_boost` | led_drive | Dynamic Drive | - | - | Off (plain output gamma) | - |  |
| `gamma_boost_knee` | led_drive | Dynamic Drive | - | - | 0.00 | - |  |
| `gamma_boost_knee_width` | led_drive | Dynamic Drive | - | - | 0.05 | - |  |
| `highlight_area_large` | led_drive | Dynamic Drive > Advanced LED | - | - | 0.050 | - |  |
| `highlight_area_small` | led_drive | Dynamic Drive > Advanced LED | - | - | 0.005 | - |  |
| `led_settle_ms` | projector | Dynamic Drive > Projector response | - | - | 300 | - |  |
| `light_max` | projector | Dynamic Drive > Advanced projector | - | - | Ask the projector | - |  |
| `light_min` | projector | Dynamic Drive > Advanced projector | - | - | Ask the projector | - |  |
| `port` | projector | Dynamic Drive > Projector connection | - | - | COM1 | - |  |
| `profile_file` | led_drive | Dynamic Drive > LED profile | - | - | Built-in (your dimming script's values) | - |  |
| `rate_gain_down` | led_drive | Dynamic Drive > LED speeds | - | - | preset | - |  |
| `rate_gain_up` | led_drive | Dynamic Drive > LED speeds | - | - | preset | - |  |
| `rate_max_down` | led_drive | Dynamic Drive > LED speeds | - | - | preset | - |  |
| `rate_max_up` | led_drive | Dynamic Drive > LED speeds | - | - | preset | - |  |
| `rate_min_down` | led_drive | Dynamic Drive > LED speeds | - | - | preset | - |  |
| `rate_min_up` | led_drive | Dynamic Drive > LED speeds | - | - | preset | - |  |
| `rate_preset` | led_drive | Dynamic Drive > LED speeds | - | - | Script (your beta 22 speeds) | - |  |
| `recovery_rate_gain` | led_drive | Dynamic Drive > LED speeds | - | - | preset | - |  |
| `recovery_rate_max` | led_drive | Dynamic Drive > LED speeds | - | - | preset | - |  |
| `recovery_rate_min` | led_drive | Dynamic Drive > LED speeds | - | - | preset | - |  |
| `shadow_lift_ansi` | led_drive | Dynamic Drive | - | - | 0.00 | - |  |
| `shadow_lift_ansi_ramp_hi` | led_drive | Dynamic Drive | - | - | 0.100 | - |  |
| `shadow_lift_ansi_ramp_lo` | led_drive | Dynamic Drive | - | - | 0.010 | - |  |
| `shadow_lift_ansi_rate` | led_drive | Dynamic Drive | - | - | 1.00 | - |  |
| `steps_down` | led_drive | Dynamic Drive > Advanced LED | - | - | 100:5, 50:3, 25:2, 10:1, 0:1 | - |  |
| `steps_up_bright` | led_drive | Dynamic Drive > Advanced LED | - | - | 100:7, 50:4, 25:2, 10:1, 0:1 | - |  |
| `steps_up_dark` | led_drive | Dynamic Drive > Advanced LED | - | - | 100:7, 50:5, 25:4, 10:1, 0:1 | - |  |
| `steps_up_split_apl` | led_drive | Dynamic Drive > Advanced LED | - | - | 40 | - |  |

### Upstream: General (49)

| Key | Section | Upstream control | VP-V | Tier | Default | Guide | Flags / notes |
|---|---|---|---|---|---|---|---|
| `baud` | ambient | General > Room lights | - | - | 115200 | - |  |
| `bounded_invalid_capture_recovery` | general | General > General behavior | - | - | checked | - |  |
| `capture_device` | general | General > Hardware | - | - | No capture devices discovered | - |  |
| `capture_display_fixes` | general | General > General behavior | - | - | checked | - | retire A |
| `capture_input` | general | General > Hardware | - | - | No inputs discovered | - |  |
| `chroma_downsampling` | directshow.conversion | General > Hardware | - | - | Average | - |  |
| `container_colorspace` | general | General > Input processing | - | - | Follow input | - |  |
| `conversion_method` | directshow.conversion | General > Hardware | - | - | Auto | - |  |
| `file_av_offset_ms` | general | General > File playback (VP Renderer filter) | - | - | 0 ms | - |  |
| `file_led_holdover_ms` | general | General > File playback (VP Renderer filter) | - | - | 3000 ms | - |  |
| `frame_offset` | directshow | General > Timing | - | - | 90 | - |  |
| `fullscreen` | general | General > General behavior | - | - | unchecked | - |  |
| `fullscreen_monitor_name` | general | General > Display | - | - | Default monitor | - |  |
| `fullscreen_monitor_session_mode` | general | General > Display | - | - | Keep the existing displays | - |  |
| `hdr_colorspace` | general | General > Input processing | - | - | Follow input | - |  |
| `hdr_luminance` | general | General > Input processing | - | - | Follow input | - |  |
| `hide_legacy_renderers` | general | General > Display | - | - | checked | - |  |
| `hold_s` | ambient | General > Room lights | - | - | 5 | - |  |
| `lux_dark` | ambient | General > Room lights | - | - | 8 | - |  |
| `lux_lit` | ambient | General > Room lights | - | - | 20 | - |  |
| `magewell_recover_faults` | general | General > General behavior | - | - | checked | - | retire A |
| `max_core_count` | directshow.conversion | General > Hardware | - | - | 1 | - |  |
| `min_core_count` | directshow.conversion | General > Hardware | - | - | 1 | - |  |
| `mode` | ambient | General > Room lights | - | - | Off | - |  |
| `noui` | general | General > General behavior | - | - | unchecked | - |  |
| `persist_profile_selection` | general | General > General behavior | - | - | unchecked | - |  |
| `port` | ambient | General > Room lights | - | - | COM5 | - |  |
| `ppm` | directshow.ppm | General > Hardware | - | - | no correction | - |  |
| `profile_change_display_seconds` | general | General > Display | - | - | 5 seconds | - |  |
| `queue_auto_reset` | general | General > General behavior | - | - | checked | - |  |
| `renderer` | general | General > Display | - | - | No renderers discovered | - |  |
| `renderer_failure_keep_fullscreen` | general | General > General behavior | - | - | checked | - |  |
| `renderer_nominal_range` | directshow | General > Renderer overrides | - | - | Auto | - |  |
| `renderer_primaries` | directshow | General > Renderer overrides | - | - | Auto | - |  |
| `renderer_start_stop_time_method` | directshow | General > Timing | - | - | Clock Smart | - |  |
| `renderer_transfer_function` | directshow | General > Renderer overrides | - | - | Auto | - |  |
| `renderer_transfer_matrix` | directshow | General > Renderer overrides | - | - | Auto | - |  |
| `rgb_10bit_packing` | decklink | General > Hardware | - | - | AUTO: r210 | - |  |
| `rgb_12bit_packing` | decklink | General > Hardware | - | - | AUTO: R12B | - |  |
| `rgb_8bit_packing` | decklink | General > Hardware | - | - | AUTO: ARGB | - |  |
| `room` | ambient | General > Room lights | - | - | Dark | - |  |
| `scene_detect` | general | General > General behavior | - | - | unchecked | - |  |
| `startminimized` | general | General > General behavior | - | - | unchecked | - |  |
| `stats_overlay` | general | General > General behavior | - | - | unchecked | - |  |
| `switch_refresh_rate` | general | General > Display | - | - | Full Screen Only | - |  |
| `use_frame_queue` | general | General > General behavior | - | - | checked | - |  |
| `video_conversion` | general | General > Input processing | - | - | Disabled | - |  |
| `windowed_fullscreen_mode` | general | General > General behavior | - | - | unchecked | - |  |
| `ycbcr_12bit_capture` | decklink | General > Hardware | - | - | 10-bit 4:2:2, keeps BTB/WTW | - |  |

### Upstream: Input processing (12)

| Key | Section | Upstream control | VP-V | Tier | Default | Guide | Flags / notes |
|---|---|---|---|---|---|---|---|
| `container_colorspace` | directshow | Input processing | - | - | Follow input | - |  |
| `container_colorspace` | vprenderer.input_processing | Input processing | - | - | Follow input | - |  |
| `container_colorspace` | vprenderer | Input processing | - | - | Follow input | - |  |
| `hdr_colorspace` | directshow | Input processing | - | - | Follow input | - |  |
| `hdr_colorspace` | vprenderer.input_processing | Input processing | - | - | Follow input | - |  |
| `hdr_colorspace` | vprenderer | Input processing | - | - | Follow input | - |  |
| `hdr_luminance` | directshow | Input processing | - | - | Follow input | - |  |
| `hdr_luminance` | vprenderer.input_processing | Input processing | - | - | Follow input | - |  |
| `hdr_luminance` | vprenderer | Input processing | - | - | Follow input | - |  |
| `video_conversion` | directshow | Input processing | - | - | Disabled | - |  |
| `video_conversion` | vprenderer.input_processing | Input processing | - | - | Disabled | - |  |
| `video_conversion` | vprenderer | Input processing | - | - | Disabled | - |  |

### Upstream: Logs (5)

| Key | Section | Upstream control | VP-V | Tier | Default | Guide | Flags / notes |
|---|---|---|---|---|---|---|---|
| `auto_report` | logging | Logs | - | - | unchecked | - |  |
| `debug` | logging | Logs | - | - | unchecked | - |  |
| `debug_log_retention` | logging | Logs | - | - | 10 | - |  |
| `enabled` | logging | Logs | - | - | checked | s7 |  |
| `report_thumbnail` | logging | Logs | - | - | unchecked | - |  |

### Upstream: Motion (8)

| Key | Section | Upstream control | VP-V | Tier | Default | Guide | Flags / notes |
|---|---|---|---|---|---|---|---|
| `deinterlace` | vprenderer | Motion > Profile details | - | - | Auto | - |  |
| `deinterlace_algorithm` | vprenderer | Motion > Profile details | - | - | bwdif | - |  |
| `deinterlace_cadence` | vprenderer | Motion > Profile details | - | - | field | - |  |
| `deinterlace_skip_spatial` | vprenderer | Motion > Profile details | - | - | false | - |  |
| `field_order` | vprenderer | Motion > Profile details | - | - | Auto | - |  |
| `frame_mixer` | vprenderer | Motion > Profile details | - | - | Oversample | - |  |
| `motion_engine` | vprenderer | Motion > Profile details | - | - | Auto | - |  |
| `smooth_motion` | vprenderer | Motion > Profile details | - | - | Off | - |  |

### Upstream: Queue (3)

| Key | Section | Upstream control | VP-V | Tier | Default | Guide | Flags / notes |
|---|---|---|---|---|---|---|---|
| `cycle_shortcut` | queue | Queue > Profile details | - | - | - | - |  |
| `shortcut` | queue | Queue > Profile details | - | - | - | - |  |
| `when` | queue | Queue > Profile details | - | - | - | - |  |

### Upstream: Rendering (65)

| Key | Section | Upstream control | VP-V | Tier | Default | Guide | Flags / notes |
|---|---|---|---|---|---|---|---|
| `adaptive_edge_hi` | vprenderer | Rendering > Profile details | - | - | 40 | - |  |
| `adaptive_edge_lo` | vprenderer | Rendering > Profile details | - | - | 8 | - |  |
| `adaptive_grain_floor` | vprenderer | Rendering > Profile details | - | - | 0.4 | - |  |
| `adaptive_grain_hi` | vprenderer | Rendering > Profile details | - | - | 8 | - |  |
| `adaptive_grain_lo` | vprenderer | Rendering > Profile details | - | - | 2 | - |  |
| `adaptive_grain_map_smooth_ms` | vprenderer | Rendering > Profile details | - | - | 500 | - |  |
| `adaptive_grain_shadow_bias` | vprenderer | Rendering > Profile details | - | - | 0.5 | - |  |
| `adaptive_grain_smooth_ms` | vprenderer | Rendering > Profile details | - | - | 1000 | - |  |
| `ambient_adapt_strength` | vprenderer | Rendering > Profile details | - | - | 1 | - |  |
| `ambient_max_black_nits` | vprenderer | Rendering > Profile details | - | - | 1 | - |  |
| `ambient_screen_reflect` | vprenderer | Rendering > Profile details | - | - | 0.01 | - |  |
| `ambient_tau_down_s` | vprenderer | Rendering > Profile details | - | - | 90 | - |  |
| `ambient_tau_up_s` | vprenderer | Rendering > Profile details | - | - | 15 | - |  |
| `black_cutoff` | vprenderer | Rendering > Profile details | - | - | 1 | - |  |
| `cadence_class_fixes` | vprenderer | Rendering > Profile details | - | - | On | - | retire A |
| `contrast_recovery` | vprenderer | Rendering > Profile details | - | - | Auto | - |  |
| `dejudder_block_taper` | vprenderer | Rendering > Profile details | - | - | Off | - | retire B |
| `dejudder_motion_scale_hi` | vprenderer | Rendering > Profile details | - | - | 1.5 | - |  |
| `dejudder_motion_scale_lo` | vprenderer | Rendering > Profile details | - | - | 0.25 | - |  |
| `dejudder_occlusion_fill` | vprenderer | Rendering > Profile details | - | - | Off | - | retire B |
| `dejudder_pixel_trust` | vprenderer | Rendering > Profile details | - | - | On | - |  |
| `dejudder_pixel_trust_hi` | vprenderer | Rendering > Profile details | - | - | 8 | - |  |
| `dejudder_pixel_trust_lo` | vprenderer | Rendering > Profile details | - | - | 3 | - |  |
| `dejudder_roll_floor` | vprenderer | Rendering > Profile details | - | - | 0.25 | - |  |
| `dejudder_roll_hi` | vprenderer | Rendering > Profile details | - | - | 24 | - |  |
| `dejudder_roll_lo` | vprenderer | Rendering > Profile details | - | - | 8 | - |  |
| `dejudder_source_warp_filter` | vprenderer | Rendering > Profile details | - | - | catmull | - | retire B |
| `dejudder_taper_sad` | vprenderer | Rendering > Profile details | - | - | 0 | - | retire B |
| `dejudder_verify` | vprenderer | Rendering > Profile details | - | - | Off | - | retire D |
| `dejudder_verify_share` | vprenderer | Rendering > Profile details | - | - | 0.2 | - | retire D |
| `dejudder_warp_stage` | vprenderer | Rendering > Profile details | - | - | rendered | - | retire B |
| `efficiency_fixes` | vprenderer | Rendering > Profile details | - | - | On | - | retire A |
| `film_strength` | vprenderer | Rendering > Profile details | - | - | 0.3 | - |  |
| `gamut_mapping` | vprenderer | Rendering > Profile details | - | - | Auto | - |  |
| `general_rate_fixes` | vprenderer | Rendering > Profile details | - | - | On | - | retire A |
| `intent_motion_min` | vprenderer | Rendering > Profile details | - | - | 0.5 | - |  |
| `intent_motion_min_shot_s` | vprenderer | Rendering > Profile details | - | - | 2 | - |  |
| `intent_motion_policy` | vprenderer | Rendering > Profile details | - | - | display | - |  |
| `knee_adaptation` | vprenderer | Rendering > Profile details | - | - | 0.4 | - |  |
| `knee_default` | vprenderer | Rendering > Profile details | - | - | 0.4 | - |  |
| `knee_maximum` | vprenderer | Rendering > Profile details | - | - | 0.8 | - |  |
| `knee_minimum` | vprenderer | Rendering > Profile details | - | - | 0.1 | - |  |
| `knee_offset` | vprenderer | Rendering > Profile details | - | - | 1 | - |  |
| `mastering_clip` | vprenderer | Rendering > Profile details | - | - | 1 | - |  |
| `motion_history_fixes` | vprenderer | Rendering > Profile details | - | - | On | - | retire A |
| `peak_detection` | vprenderer | Rendering > Profile details | - | - | Auto | - |  |
| `percentile` | vprenderer | Rendering > Profile details | - | - | 99.995 | - |  |
| `quality` | vprenderer | Rendering > Profile details | - | - | High | - |  |
| `reinhard_contrast` | vprenderer | Rendering > Profile details | - | - | 0.5 | - |  |
| `saturation` | vprenderer | Rendering > Profile details | - | - | 0 | - |  |
| `scene_threshold_high` | vprenderer | Rendering > Profile details | - | - | 3 | - |  |
| `scene_threshold_low` | vprenderer | Rendering > Profile details | - | - | 1 | - |  |
| `sdr_black_nits` | vprenderer | Rendering > Profile details | - | - | 0 | - |  |
| `sdr_target_nits` | vprenderer | Rendering > Profile details | - | - | 203 | - |  |
| `slope_offset` | vprenderer | Rendering > Profile details | - | - | 0.2 | - |  |
| `slope_tuning` | vprenderer | Rendering > Profile details | - | - | 1.5 | - |  |
| `smoothing_period` | vprenderer | Rendering > Profile details | - | - | 20 | - |  |
| `source_resolution_crossfade_s` | vprenderer | Rendering > Profile details | - | - | 1.5 | - |  |
| `source_resolution_cut_fixes` | vprenderer | Rendering > Profile details | - | - | On | - | retire A |
| `source_resolution_evidence_seconds` | vprenderer | Rendering > Profile details | - | - | 10 | - |  |
| `source_resolution_evidence_shots` | vprenderer | Rendering > Profile details | - | - | 3 | - |  |
| `source_resolution_release_fade_s` | vprenderer | Rendering > Profile details | - | - | 0.25 | - |  |
| `spline_contrast` | vprenderer | Rendering > Profile details | - | - | 0.5 | - |  |
| `timing_continuity_fixes` | vprenderer | Rendering > Profile details | - | - | On | - | retire A |
| `tone_mapping` | vprenderer | Rendering > Profile details | - | - | Auto | - |  |

### Upstream: Scaling (4)

| Key | Section | Upstream control | VP-V | Tier | Default | Guide | Flags / notes |
|---|---|---|---|---|---|---|---|
| `chroma_upscaler` | vprenderer | Scaling > Profile details | - | - | - | s10 |  |
| `downscaler` | vprenderer | Scaling > Profile details | - | - | - | - |  |
| `sigmoid` | vprenderer | Scaling > Profile details | - | - | - | - |  |
| `upscaler` | vprenderer | Scaling > Profile details | - | - | - | - |  |

### Upstream: Screen (8)

| Key | Section | Upstream control | VP-V | Tier | Default | Guide | Flags / notes |
|---|---|---|---|---|---|---|---|
| `anamorphic_scale` | vprenderer | Screen > Profile details | - | - | - | - |  |
| `blank_bottom` | vprenderer | Screen > Profile details | - | - | px | - |  |
| `blank_left` | vprenderer | Screen > Profile details | - | - | px | - |  |
| `blank_right` | vprenderer | Screen > Profile details | - | - | px | - |  |
| `blank_top` | vprenderer | Screen > Profile details | - | - | px | - |  |
| `screen_aspect` | vprenderer | Screen > Profile details | - | - | Use output display aspect | - |  |
| `screen_edge_padding` | vprenderer | Screen > Profile details | - | - | px | - |  |
| `vertical_alignment` | vprenderer | Screen > Profile details | - | - | - | - |  |

### Upstream: Shortcuts (2)

| Key | Section | Upstream control | VP-V | Tier | Default | Guide | Flags / notes |
|---|---|---|---|---|---|---|---|
| `foreground_only` | shortcuts | Shortcuts > Keyboard handling | - | - | unchecked | - |  |
| `global_hotkeys` | shortcuts | Shortcuts > Keyboard handling | - | - | unchecked | - |  |

### Upstream: Subtitles (26)

| Key | Section | Upstream control | VP-V | Tier | Default | Guide | Flags / notes |
|---|---|---|---|---|---|---|---|
| `background_edge` | vprenderer | Subtitles > Profile details | - | - | Extend to black bar | - |  |
| `cleanup_fill` | vprenderer | Subtitles > Profile details | - | - | Mirror nearby picture | - |  |
| `hdr_peak_analysis_height_percent` | vprenderer | Subtitles > Profile details | - | - | % | - |  |
| `hdr_peak_analysis_motion_compensation` | vprenderer | Subtitles > Profile details | - | - | unchecked | - |  |
| `hdr_peak_analysis_picture_only` | vprenderer | Subtitles > Profile details | - | - | unchecked | - |  |
| `hdr_peak_analysis_position` | vprenderer | Subtitles > Profile details | - | - | top | - |  |
| `minimum_headroom_pixels` | vprenderer | Subtitles > Profile details | - | - | source pixels | - |  |
| `offset_pixels` | vprenderer | Subtitles > Profile details | - | - | source pixels | - |  |
| `rounded_corners` | vprenderer | Subtitles > Profile details | - | - | unchecked | - |  |
| `subtitle_engage_drift_ms` | vprenderer | Subtitles > Profile details | - | - | ms | - |  |
| `subtitle_generated_gray_blur_px` | vprenderer | Subtitles > Profile details | - | - | source pixels | - |  |
| `subtitle_generated_gray_border_color` | vprenderer | Subtitles > Profile details | - | - | RRGGBB | - |  |
| `subtitle_generated_gray_border_opacity` | vprenderer | Subtitles > Profile details | - | - | 0–1 | - |  |
| `subtitle_generated_gray_border_width` | vprenderer | Subtitles > Profile details | - | - | source pixels | - |  |
| `subtitle_generated_gray_color` | vprenderer | Subtitles > Profile details | - | - | RRGGBB | - |  |
| `subtitle_generated_gray_max_luminance` | vprenderer | Subtitles > Profile details | - | - | linear 0.01–1 | - |  |
| `subtitle_generated_gray_opacity` | vprenderer | Subtitles > Profile details | - | - | 0–1 | - |  |
| `subtitle_hold_ms` | vprenderer | Subtitles > Profile details | - | - | ms | - |  |
| `subtitle_hold_seconds` | vprenderer | Subtitles > Profile details | - | - | seconds | - |  |
| `subtitle_near_bar_px` | vprenderer | Subtitles > Profile details | - | - | source pixels | - |  |
| `subtitle_release_drift_ms` | vprenderer | Subtitles > Profile details | - | - | ms | - |  |
| `subtitle_script_hint` | vprenderer | Subtitles > Profile details | - | - | - | - |  |
| `subtitle_target_buffer_pixels` | vprenderer | Subtitles > Profile details | - | - | source pixels | - |  |
| `subtitle_text_color` | vprenderer | Subtitles > Profile details | - | - | RRGGBB | - |  |
| `text_reduction_percent` | vprenderer | Subtitles > Profile details | - | - | % (0 = unchanged) | - |  |
| `type` | vprenderer | Subtitles > Profile details | - | - | Off | - |  |

### Upstream: Zoom (6)

| Key | Section | Upstream control | VP-V | Tier | Default | Guide | Flags / notes |
|---|---|---|---|---|---|---|---|
| `automatic_crop` | vprenderer | Zoom > Profile details | - | - | unchecked | - |  |
| `crop_narrower_content_aspect_limit` | vprenderer | Zoom > Profile details | - | - | Optional, e.g. 2.20:1 | - |  |
| `crop_narrower_content_to_fill_screen` | vprenderer | Zoom > Profile details | - | - | unchecked | - |  |
| `crop_wider_content_aspect_limit` | vprenderer | Zoom > Profile details | - | - | Optional, e.g. 2.76:1 | - |  |
| `crop_wider_content_to_fill_screen` | vprenderer | Zoom > Profile details | - | - | unchecked | - |  |
| `fixed_crop_aspect` | vprenderer | Zoom > Profile details | - | - | Off; e.g. 2.00:1 | - |  |

### VP-V: global keys ([vpv]) (8)

| Key | Section | Upstream control | VP-V | Tier | Default | Guide | Flags / notes |
|---|---|---|---|---|---|---|---|
| `calibration_lut_stage` | vprenderer | - | Pipeline (expert) | - | - | s1 | VP-V section (ADR-0023): stored in [vpv] by the VP-V page (vpv.expert.calibration_lut_stage); also read from [vprenderer] |
| `calibration_lut_stage` | vpv | - | Pipeline (expert) | - | - | s1 |  |
| `pipeline_order` | vprenderer | - | Pipeline (expert) | - | - | s1 | VP-V section (ADR-0023): stored in [vpv] by the VP-V page (vpv.expert.pipeline_order); also read from [vprenderer] |
| `pipeline_order` | vpv | - | Pipeline (expert) | - | - | s1 |  |
| `pipeline_original_pathway` | vprenderer | - | Pathway | - | - | s1 | VP-V section (ADR-0023): stored in [vpv] by the VP-V page (vpv.pathway); also read from [vprenderer] |
| `pipeline_original_pathway` | vpv | - | Pathway | - | - | s1 |  |
| `pipeline_slots_off` | vprenderer | - | Pipeline (expert) | - | - | s1 | VP-V section (ADR-0023): stored in [vpv] by the VP-V page (vpv.expert.pipeline_slots_off); also read from [vprenderer] |
| `pipeline_slots_off` | vpv | - | Pipeline (expert) | - | - | s1 |  |

### Shortcut actions

| Action | Default chord | Control |
|---|---|---|
| `auto_set` | Ctrl+Shift+A | Shortcuts > Application |
| `fullscreen_exit` | Esc | Shortcuts > Application |
| `fullscreen_toggle` | Alt+Enter | Shortcuts > Application |
| `toggle_stats_overlay` | Ctrl+I | Shortcuts > Application |
| `capture_rendered_output` | Ctrl+Alt+S | Shortcuts > Application |
| `state_report` | Ctrl+Alt+D | Shortcuts > Application |
| `raw_capture` | Ctrl+Alt+W | Shortcuts > Capture & renderer |
| `reapply_rules` | (none) | Shortcuts > Application |
| `room_lights` | (none) | Shortcuts > Application |
| `show_profiles` | Ctrl+Alt+I | Shortcuts > Application |
| `pq_set` | Ctrl+Shift+P | Shortcuts > Application |
| `renderer_restart` | Ctrl+Alt+Shift+R | Shortcuts > Capture & renderer |
| `renderer_reset` | Ctrl+Shift+R | Shortcuts > Capture & renderer |
| `capture_1` | Ctrl+1 | Shortcuts > Capture & renderer |
| `capture_2` | Ctrl+2 | Shortcuts > Capture & renderer |
| `capture_3` | Ctrl+3 | Shortcuts > Capture & renderer |
| `capture_4` | Ctrl+4 | Shortcuts > Capture & renderer |
| `video_conversion_off` | Ctrl+Alt+V | Shortcuts > Capture & renderer |
| `video_conversion_p010` | Ctrl+Shift+V | Shortcuts > Capture & renderer |
| `config_editor` | Ctrl+Shift+S | Shortcuts > Application |
| `toggle_noui` | Ctrl+Shift+U | Shortcuts > Application |
| `display_rules_auto` | Ctrl+Shift+F4 | Shortcuts > Capture & renderer |

### Controls not named after an accepted key

| Control | Page | Checked |
|---|---|---|
| `config.led_drive.default.ftb_experimental` | Dynamic Drive > Experimental | unlocks the experimental fade-to-black fields |
| `config.vprenderer.color.sdr_response` | Color / Output > Profile details | preset drop-down that fills the SDR response keys |
| `config.dv.mode.lldv` | DV > Profile details | DV mode button: writes dolby_vision_tunnel (allow-list) |
| `config.dv.mode.tunneling` | DV > Profile details | DV mode button: writes dolby_vision_tunnel (allow-list) |
| `config.vprenderer.viewport.anamorphic_enabled` | Screen > Profile details | check box that enables / clears anamorphic_scale |

<!-- END GENERATED -->
