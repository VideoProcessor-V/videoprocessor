# VideoProcessor: temporal dithering, judder reduction, LED drive — project brief

Status: design agreed, nothing built yet. Revision 7, 2026-10-08, against
`billslack2/videoprocessor` tag `1.4-beta-rc1`.

This is the handoff to a Claude Code session and the project's record. Read it
fully before touching source. When a decision changes, change it here.

---

## 0. People, hardware, workflow

- **Owner:** Victor McPherson. Not a developer; has never compiled anything.
  Builds via Claude Code on a *separate* PC, copies a deploy folder to the
  cinema PC by hand, looks at the screen, reports. Explain things in plain
  terms. Approve-each-step mode is fine.
- **Cinema PC:** RTX 3090 → DisplayPort → Barco DP-to-dual-link-DVI box →
  Barco FL35 (single-chip LED DLP, WQXGA) at 2560×1600, 59.97 Hz, 8-bit RGB,
  through an ISCO III anamorphic lens (1.33× horizontal). Blackmagic DeckLink
  capture, sources up to 4K 10-bit incl. LLDV. PS5 runs on a separate HDFury
  Diva path straight to the projector and is **out of scope**.
- **Projector cannot change refresh:** 48 Hz crops ~100 lines (firmware), 72 Hz
  not accepted; firmware is unsupported and will not be modified. 60 Hz is the
  fixed constraint for all design.
- **Current VP:** an old release-binary version (VP2.exe) with madVR, launched
  at login from a batch file in the Windows Startup folder under a per-launch
  `runAsDate` wrapper for madVR's build expiry. Its switches:
  `/fullscreen /hdr_colorspace FOLLOW_INPUT_LLDV /hdr_luminance
  FOLLOW_INPUT_LLDV /renderer_start_stop_time_method CLOCK_SMART
  /video_conversion V210_TO_P010 /frame_offset auto /renderer_nominal_range
  LIMITED /renderer "directshow - madVR"`. Everything about the picture
  (tone map, gamma, scaling, dither, anamorphic, calibration) lives in madVR's
  own settings — captured in §4b. Leave the Startup entry alone. The old
  install stays untouched as the fallback; the new
  build goes in its own folder (portable ZIP) with its own shortcut and
  `/config VideoProcessor-test.cfg`. The DeckLink is exclusive, so only one runs
  at a time. Nothing the new version installs affects the old one (no filter
  registration, same DeckLink SDK 12 generation, app-local VC runtime).
- **Fork owner (Bill, billslack2)** wrote the `Low_Latency` queue profile at
  Victor's request. He is busy: do not ask him for work. Build changes
  mergeable (tests, his style); offer a PR once it works; expect nothing.

## 1. Non-negotiable constraints

1. **Portability.** Renderer changes minimal and isolated: new files under
   `src/VideoProcessor-Lib/vprenderer/`, a few call sites in
   `LibplaceboVideoRenderer.cpp`. Shader files do the work. Each stage must be
   re-applicable to a newer fork release by hand in minutes.
2. **Existing mechanisms only.** Shaders appear in the Config app's Standard
   shaders list (`[shader.standard.<name>]`, group `type: multi`) with
   Enable/Disable in VP, shortcut keys, and `param_<name>` keys shown in the
   Config "Standard shader parameters" table. The LED module gets its own
   config section, editable the same way. No new menus or windows.
3. **`Low_Latency` profile untouched** (one-frame queue, Shift+L; used for
   games). Dithering stages run in every profile (sub-frame cost). De-judder
   and the time-based queue are cinema-profile only. The LED module runs in
   both, with lead time 0 in `Low_Latency`.
4. **Fail safe.** A shader that fails to load logs one line and libplacebo's
   configured dither resumes. Never a black screen. The LED module never
   stalls rendering (serial on its own thread) and clamps every command to the
   projector's reported range.
5. **Build discipline.** Build the untouched tag first (x64 Release), run the
   native test suite, confirm clean, then feature work. Follow `AGENTS.md` for
   build/packaging; produce a deploy folder (exe + `vprenderer\…DLL` from the
   same build, verified app-local runtime DLLs, shader files, `DEPLOY.txt`
   listing file destinations and config lines). Victor copies it over and
   backs up first.
6. **Range chain stays as is:** VP outputs limited (video) levels; GPU set to
   full so the driver does not remap; projector input level "video". BTB/WTW
   preserved. The quantizer is written for this.
7. **Edge blanking is the gating requirement for the first deploy.** The
   light-engine mod leaves the extreme left and right columns blue-tinted and
   unusable; madVR currently masks left 10 / right 15. The fork has NO
   horizontal blanking today (`screen_edge_padding` is only a vertical shift
   of a fitted picture, see `vprenderer/AnamorphicPresentation.h`). Add
   `blank_left`, `blank_right`, `blank_top`, `blank_bottom` (output pixels,
   default 0) to the viewport profile; they shrink the destination rectangle
   so the picture is scaled into the lit area and the blanked columns are
   black. Do this FIRST in stage 2, verify on the projector, then the hook
   chain. No uniformity mask — a colour-cast edge cannot be gained back.
8. **No P010 conversion.** The old path needed `V210_TO_P010` (4:2:2→4:2:0,
   halving vertical chroma) for the anamorphic stretch. The VP Renderer takes
   v210 4:2:2 natively; `video_conversion` is off in the test config and the
   stretch is done by the viewport/NLS profile.
9. **Colour chain stays as is:** projector RealColor targets P3 (0.680/0.320,
   0.265/0.690, 0.150/0.060), VP `target_primaries: p3_d65`. Use the P3 pair
   for BT.2020 sources too (VP gamut-maps 2020→P3 in float); do not switch to
   a 2020 container on an 8-bit link.

## 2. Verified facts — the fork (read from source)

Paths under `src/VideoProcessor-Lib/`.

- VP Renderer = libplacebo 7.360.1 on D3D11 (`pl_d3d11_swapchain`), no
  DirectShow graph. `vprenderer/LibplaceboVideoRenderer.cpp` (~843 KB).
  Bundled libplacebo source at
  `3rdparty/libplacebo/source/libplacebo-7.360.1-vp-source.zip`; prebuilt
  lib/include/bin alongside. Config app is Qt — **not needed for this work**.
- **Single image per refresh:** `pl_render_image` via `NlsHookRecovery::Render`
  (`vprenderer/NlsHookRecovery.h`). Frame mixing off
  ("frame_mixing_active=0"). Cadence repeats re-render the same source frame.
- **Queue:** `vprenderer/AlphaQueuePolicy.h`; profile keys are frame counts
  (`queue_size`, `lead_frames`, `target_frames`, `startup_preroll_frames`).
  Latency therefore scales with source frame period (≈125 ms at 24p for
  target_frames 3, ≈50 ms at 60p) — the audio offset is currently
  source-rate dependent. Stage 5 fixes this.
- **Hooks:** parsed by `pl_mpv_user_shader_parse`. The renderer installs only
  the NLS hook (`renderParams.hooks = &nlsHook; num_hooks = 1;` ~line 13154)
  and tests NLS activity by `num_hooks == 1` (~line 13721). Nine other sites
  reset `num_hooks = 0`. Custom GLSL rules (`shader_type: custom`,
  `glsl_file`) ARE resolved into the libplacebo-backend selection
  (`microsoft_directshow/MadVRShaderLoader.cpp` ~2960–3060) but
  `SetConfiguredShaderSelection` (~line 7625) ignores all but NLS.
- `{{name}}` templates: `ApplyUserShaderParameters` (~line 377). NLS also sets
  `//!PARAM` values at runtime through `pl_hook_par` data (`stretch_ratio`).
- `param_<name>` keys → `rule.parameters` (`LoadShaderParameters`,
  MadVRShaderLoader.cpp ~750); shown in Config as the parameter table.
- Shader groups are keyboard-selectable (reset key + per-member `when`
  conditions; see the NLS entries in `VideoProcessor.cfg` for exact syntax).
  Config app right-click: Enable in VP / Disable in VP, applied live.
- **Dithering:** `[vprenderer.*] dithering: off | ordered | blue_noise |
  white_noise | error_diffusion` (`RendererProfileConfig.h` ~540); `off`
  clears both `dither_params` and `error_diffusion`.
- `display_bit_depth: 8` on the default 10-bit swapchain → target repr
  `color_depth 8, bit_shift 2` (`ApplyDisplayBitDepth`).
  `diagnostic_force_8bit_sdr_swapchain: true` selects an 8-bit swapchain.
- HDR→SDR: `tone_mapping` (spline/bt2390/st2094-40/reinhard),
  `peak_detection`, `gamut_mapping`, `sdr_target_nits`, `sdr_black_nits`,
  `hdr_tone_map_target_gamma`, `hdr_colorspace/hdr_luminance:
  FOLLOW_INPUT_LLDV`. For LLDV sources: static curve, peak detection off.
- Anamorphic: VP does the vertical stretch (fork has anamorphic-aware NLS
  compensation); projector stays 1:1.
- `/config <path>` command-line option exists (`VideoProcessorApp.cpp` ~298).
- No serial code anywhere in the fork today.

## 3. Verified facts — libplacebo 7.360.1 (bundled source)

- Intermediate FBOs are **rgba16f**. Above mid-grey the source is held to
  ±¼ of a 10-bit step; finer below. Accepted precision floor.
- The **OUTPUT hook runs after `pl_shader_encode_color` and before dither**
  (renderer.c ~2790–2960). At OUTPUT the colour is already in the target code
  domain: limited = code/256, full = code/255. The renderer then multiplies by
  `1/scale` and stores to the swapchain. Exact 8-bit code k in the top bits of
  the 10-bit surface is produced by emitting limited: 4k/1023, full: k/255
  (8-bit swapchain: k/255 either way).
- Hook parser supports persistent `//!TEXTURE … //!STORAGE` (data optional),
  `//!COMPUTE bw bh [tw th]` writing `imageStore(out_image, …)`, `//!PARAM`
  with `//!TYPE float|int|DEFINE|CONSTANT|DYNAMIC`, uniforms `frame` (int,
  per render) and `random`. Bound textures expose `NAME_raw/_pos/_size/_pt/
  _tex()/_mul`. `pl_hook` exposes `stages` and `parameters`.
- Possible pre-existing issue: limited range on the 10-bit swapchain encodes
  code/256 and stores ×1023 → code k lands at round(3.996·k) = 4k−1 for the
  upper half. If the Barco box truncates, today's output may be one code low
  above ~128. Settle with the ramp test of the unmodified build (§7).

## 4. Verified facts — projector and chain (from menu photos and manuals)

Current FL35 settings, all correct for this project; change nothing:
- Frame lock: source, 60 Hz, locked (external). Input DVI 1 digital,
  2560×1600 progressive, total 2720×1646, 268.5 MHz, vertical 59.97 Hz.
- Picture: brightness 50, contrast 50, aspect one-to-one, gamma film 2.2.
  BrilliantColor: VizSim (no colour boost — the quiet mode). Input level:
  video. Colour space: auto. Source correction offsets all 0. No sharpness
  or dynamic-contrast control exposed.
- RealColor on, P3 primaries, white x 0.310 / y 0.340 labelled 6500 K.
  (D65 is 0.3127/0.3290 — y 0.340 is visibly green of D65 unless it came from
  a meter correction. Check when the spectro is set up; irrelevant to
  dithering.)
- Projector has an internal colour-balancing module that keeps white point
  stable as LED drive changes (unlike the FL32). Per-colour dim mode is
  therefore unnecessary.
- Serial: 19200 8N1, no flow control. ASCII protocol: `:MNEM value CR`,
  ACK `%001 MNEM 000nnn CR`. Relevant mnemonics: `LPW1` LED power
  (`?N`/`?M` read min/max; owner expects min ≈60), `CNTR` contrast, `BRIG`,
  `PMUT` picture mute, `SHUT` shutter, `ECHO` response on/off, `FLSO` frame
  lock source, `S1T1` scaling 1:1, `OSDC`. Document's timing rules: ≥500 ms
  between commands, wait for response, 5 s pause after 20 commands. The
  script has run at 200 ms gaps for years, so the projector is more tolerant
  than documented; the module must pace on ACKs and find the real limit
  empirically.

## 4b. Verified facts — current madVR settings (v0.92.17, from screenshots)

Device "Projectiondesign WQXGA DLP". These define the like-for-like test
config; later stages deliberately change some of them.
- Properties: display expects TV levels 16–235; native bit depth auto.
- Screen config: visible area cropped left 10, top 0, right 15, bottom 0;
  anamorphic lens on, stretch 4/3.
- Calibration: "this display is already calibrated", DCI-P3, **pure power
  2.40**, no LUT, GPU gamma ramps enabled. The projector runs film 2.2, so the
  2.4/2.2 mismatch is the deliberate brightening ("gamma boost").
- Color & gamma: saturation +5 only.
- HDR (profiles 1/2/3; rule: fps 59–61 → Profile 2, else Profile 1):
  tone map with pixel shaders, display peak 100 nits, dynamic target nits
  off, dynamic clipping off, colour tweaks balanced, highlight recovery high
  (100), **contrast recovery Log Medium–High** (used to counter the washed
  look of the gamma boost — treat gamma lift and contrast recovery as one
  adjustment with two knobs), shadow recovery low, HSTM no brightening/no
  darkening, avgHighlights ceiling 2.0×, limit HSTM, flicker protection off.
  Differences between HDR profiles 1/2/3 not yet captured.
- Dithering: Error Diffusion option 2, colored noise off, change-every-frame
  off. (This is the baseline the sigma-delta is compared against.)
- Image downscaling (all fps profiles): SSIM 2D 100%, scale in linear light
  OFF, anti-ringing off, anti-bloating off.
- Chroma upscaling: NGU Sharp high quality, anti-ringing off, SuperRes off.
- Smooth motion: enabled, "only if there would be motion judder" — i.e. 24p
  is currently frame-blended. Fork equivalent: oversample frame mixer
  (`pl_render_image_mix`), but frame mixing is OFF in the fork and enabling
  it is part of the stage 5/6 presentation-path change — NOT in the first
  deploy. Until then the first comparison is plain 3:2 vs madVR's blend;
  judge dithering and colour, not judder, in that comparison.
- Zoom profiles (rule: AR ≤ 1.9 → zoom169, else zoom1): both detect hard-coded
  black bars and skip rescaling for ≤2-line changes; zoom169 additionally
  "if big black bars: reduce bar size by 50%" and "crop black bars". The 50%
  partial zoom may need approximation via the fork's crop aspect limits.
- Display modes: switching enabled but the mode list is empty → never
  switches; always 60 Hz. Nothing to carry over.

## 5. Design summary

Dithering is replaced by a per-pixel first-order temporal sigma-delta at the
OUTPUT stage: quantise `x + r` to 8-bit, store residual `r' = x + r − k`
(±0.5 code) in a persistent GPU texture, reuse next refresh. The eye and the
DMD's PWM do the averaging; for unchanged pixels the time-average converges to
the 10-bit source in 4 refreshes. A per-pixel motion gate discards the residual
where the pixel changed; stage 4 adds motion-compensated carry. A small
decorrelation dither (IGN, ±0.15 code) prevents limit cycles. It never
averages frames and needs no lookahead. Projector-side LED dimming scales the
output and does not disturb the time-average. All stages run on the GPU as
libplacebo passes; CPU work is only queue/serial plumbing.

Later stages: time-based queue (constant audio delay independent of source
rate), low-strength motion-compensated de-judder with a continuous strength
knob and hard-hold fallback, the LED controller ported into VP with exact
gain compensation and highlight protection, and an adaptive downscale blend.

## 6. Stages, in build order

Stage numbers are labels, not sequence. **Build in this order:**

1. Stage 1 — environment, clean baseline build.
2. Stage 2 — **first: `blank_left/right/top/bottom` viewport keys** (set
   10 / 15 / 0 / 0) + the test config, deployed and checked on the projector;
   **then** hook chain + `Temporal sigma-delta.glsl` + igv's
   `SSimDownscaler.glsl` + a chroma hook (Artoriuz CfL as the NGU stand-in)
   as `[shader.standard.*]` entries + a `saturation` rendering key. Two
   deploy folders if that is more convenient.
3. Stage 3 — ramp photo. Go/no-go for everything after.
4. Stage 7 — LED module, **reactive form first** (lead time 0): port,
   gain compensation, highlight policy, configurable floors, side-by-side
   logging against the script. Needs only the hook chain and serial code.
5. Stage 5 — time-based queue. LED module then gains lookahead by config;
   audio delay set once.
6. Stage 4 — motion-compensated residual carry.
7. Stage 6 — de-judder (open-ended, skippable).
8. Stage 8 — adaptive downscale blend on top of the SSIM shader.
9. Stage 9 — ambient-light switching.

The fork's viewport profiles already cover the madVR screen-config feature
set: `anamorphic_scale` (set 4:3 for the ISCO III, matching madVR's 4/3), `mode: normal|scope`,
`screen_aspect`, `vertical_alignment`, `screen_edge_padding` (a vertical shift only, not blanking), automatic crop of narrower/wider
content with aspect limits, trusted active-picture detection with near-black
recovery, subtitle fit. Bill runs a scope screen himself, so this path is
exercised.

### Stage 1 — Environment (build PC)
Visual Studio 2022 Community with "Desktop development with C++" and the MFC
component; Git for Windows; Claude Desktop (Code tab). Clone at tag
`1.4-beta-rc1`. Build x64 Release untouched; run tests. Deliverable: a clean
baseline and a first deploy folder.

### Stage 2 — Blanking keys, custom GLSL hook chain, saturation key (the only renderer changes for stages 2–4, 8)
0. Before anything else: the four `blank_*` viewport keys (constraint 7),
   deployed and verified on the projector. Then a `saturation` key in the
   rendering profile (libplacebo colour adjustment; madVR runs +5).
New `vprenderer/CustomHookChain.h/.cpp`, owned by the renderer:
1. In `SetConfiguredShaderSelection`, collect rules with
   `!nls && !none && !filename.empty()` ordered by `stageOrder` then
   `profileSelectionOrder`; read file; apply `{{…}}` templates (built-ins
   below + `rule.parameters`); `pl_mpv_user_shader_parse`; then set
   `//!PARAM` values from `rule.parameters` by name. Destroy previous hooks
   first. One log line per load/failure.
2. Immediately before `NlsHookRecovery::Render`: `hookChain = customHooks +
   (nlsActive ? nlsHook : none)`; set `hooks/num_hooks`. Replace the
   `num_hooks == 1 ? nlsHook` test with an explicit `nlsActive` flag.
3. **Dither suppression:** if any active custom hook has `PL_HOOK_OUTPUT` in
   `stages`, clear `dither_params` and `error_diffusion` for that frame;
   otherwise the configured `dithering:` applies. The Config toggle is then
   a true A/B against libplacebo's own dither. Users keep `dithering:` at
   their normal value.
4. Built-in templates for every GLSL rule: `{{output_width}}
   {{output_height}} {{source_width}} {{source_height}} {{refresh_hz}}
   {{source_fps}}`. Selection re-applies on renderer reset, so storage
   textures follow resolution changes. (Replace the shader's hardcoded
   `//!SIZE 2560 1600` with the templates once this exists.)
5. A rule may declare `cinema_only: true` (skipped in `Low_Latency`).
6. Stats overlay: one line per active hook.
7. Tests in the fork's style: chain order, dither suppression, param
   application, load-failure fallback, no-hook path unchanged.
8. **Check early:** whether the shader-loading preflight or Config app rejects
   `.glsl` in `[shader.standard.*]` (today's entries are HLSL for madVR). If
   so, a small loader change is part of this stage.
Then install `Temporal sigma-delta.glsl` in `shaders/`, add the config
section (§8), deploy, run §7.

### Stage 3 — Verify on the projector
Ramp protocol (§7). Settle converter rounding vs truncation. Tune
`noise_amplitude`, `motion_threshold`. If the time-average does not converge
on this display, stop and reassess before any further stage.

### Stage 4 — Motion-compensated residual carry
Compute pass before the quantizer: hierarchical block matching on
gamma-encoded luma at quarter resolution (vector error only misplaces ≤0.5
code), forward-only from a persistent previous-luma texture; vectors +
confidence in a storage texture. Quantizer warps the residual along vectors;
low confidence ⇒ existing gate. Budget ≈1 ms. Reference: FFmpeg
`vf_minterpolate` (algorithm, not code).

### Stage 5 — Time-based queue target (presentation path, cinema profile)
New queue-profile key `target_latency_ms` (e.g. 120). Frame depth derived
per source rate; release the frame whose timestamp is that old. `Low_Latency`
unchanged. OSD shows effective delay so the audio offset is set once. Raise
`queue_size` accordingly (8 frames at 60p is trivial). Provides lookahead for
stages 6 and 7 and the first-frame-early cut flag.

### Stage 6 — Low-strength motion-compensated de-judder (cinema profile)
Needs the render path to see frame A and next frame B (second texture to a
hook, or `pl_render_image_mix`). Per refresh, true position t between A and B
(3:2 at 60 Hz cycles 0, .4, .8, .2, .6); render at
`t_render = t_nearest + s·(t_refresh − t_nearest)`; `s` is the strength
(0 = plain 3:2, 1 = full interpolation; expected zone 0.2–0.35). Bidirectional
half-res vectors; forward/backward consistency for occlusions; **fallback is a
hard hold of the nearest frame, never a blend**; no synthesis across cuts;
`s` applied globally per frame. Budget ≈3 ms. Highest-risk stage, taste
outcome, built last of the picture stages, skippable. Owner dislikes both
frame-blend blur and soap-opera effect — do not offer either.

### Stage 7 — LED drive module (native; replaces `beta_22.py`)
Built right after stage 3 in reactive form (lead time 0); lookahead arrives
with stage 5 as a config change.
**Measurement (GPU, per frame, over the active picture, not a fixed crop):**
APL = mean of Rec.601 luma (0.299/0.587/0.114) of the *output* codes
(gamma-encoded, after tone map/LUT, limited range, so black = code 16);
highlight statistic = high percentile (≈99.5th) of 3×3-block means with a
rolloff, replacing the script's "max of 854×480 ≥ 215"; plus cut flag (step
change) and fade flag (monotonic APL decline toward black over consecutive
frames, structure otherwise static).

**Controller (ported from the script, parameter for parameter):**
LED target vs APL: 255 above 100; 230 at 70; 200 at 40; 100 at 20;
floor below. Step tables by distance to target (up 7/5/4/2/1, down 5/3/2/1
per tick). Highlight rules: anti-crush (fast protective response when a
highlight appears) and anti-pump (ignore small differences in one direction).
`sudden_flag` path is dead code — omit. Tick period: the script's loop was
unfixed (~300–500 ms); start at 400 ms and match by eye, or from a timestamp
log if provided.

**New: gain compensation in VP instead of projector contrast.** Apply gain =
full/LED_actual in linear light before the LUT and quantizer, so content
brightness is constant and only the black floor moves. Gain must follow the
LED's *actual* trajectory (one-off camera measurement of LED step response →
model), never the commanded target, or there will be flashes. The projector
contrast command is retired; keep `mode: contrast` as a fallback that
reproduces the script's contrast curve. Contrast steps were the most visible
artifact of the old scheme and the reason the LED floor was held at 100; with
float gain that limit disappears.

**Highlight policy (replaces the script's contrast dump).** The trade in any
scene is floor depth vs highlight headroom, with the LED level as the only
dial; compensation holds everything else fixed.
- *Persistence gates protection.* Brief flashes (lightning, muzzle flash) are
  soft-clipped and otherwise ignored — never dim the scene for them, and the
  LED target is filtered over a longer window so it never chases transients.
  Sustained highlights (headlights, lit windows, lamps) are always protected.
- *Area decides how.* Small sustained highlights are handled by a **knee**:
  the top of the tone curve rolls off above what the current LED supports, so
  a headlight keeps its gradation (core, bloom, falloff) while the dark
  surround stays fully compensated. Large sustained highlights lower the
  **global gain** while the LED comes up. Blend continuously by area.
- *Continuous, never stepped.* No duration/area thresholds that a borderline
  scene could toggle across; response strength is a smooth function of
  persistence and area, eased in over a few frames (hysteresis comes free).
  Asymmetry preserved: protection engages faster than it relaxes.
- *One taste parameter:* `highlight_tolerance` (stops of knee compression
  accepted in exchange for a lower floor; start ≈1/3 stop). The controller
  picks the lowest LED level that keeps compression within it. Tiny
  highlights spend little of it, so dark scenes go deep. Nudgeable from a key.

**Modes:** cut ⇒ jump (masked by the cut); scene ⇒ drift at the script's
rates; fade ⇒ track to black, contrast/gain unchanged. Lead time =
`lead_ms` in the cinema profile (acts before the frame is shown), 0 in
`Low_Latency` (reactive; the only residual artifact is a brief soft dip after
dark→bright cuts).

**Config:** `[led_drive.Default]` with `com_port`, `baud`, `lead_ms`,
`tick_ms`, `led_floor` (default 100, the owner's anti-pump floor),
`led_floor_black` (default 60, for fades/cuts to black), `led_ceiling`,
`highlight_percentile`, `highlight_tolerance`, `highlight_persist_ms`,
`knee`, `mode`, and the target curve / step tables.
Module clamps to `LPW1 ?N/?M`, reads back state at startup (`CNTR?`,
`LPW1?`), paces on ACKs (`ECHO` on), serial on a background thread, logs what
it commands. Side-by-side logging mode against the Python script before
retiring it. Test once: `SHUT 1` / `PMUT 1` speed and audibility for true
black on hard cuts.

**Why the old approach had limits:** screenshot-based APL sees the frame
after display (lag → the floor was raised to 100 to hide pumping), crops a
fixed scope band (wrong on 16:9), can't tell fade from dark scene because
black is code 16, and can only act through integer contrast steps. All four
go away here. With compensation, lag no longer moves content brightness, so
the floor can likely drop to 60 later with the same drift rates — test, keep
it configurable.

### Stage 8 — Adaptive downscale blend (4K → 1600p, anisotropic)
Plain SSIM downscaling (igv's shader, unmodified) is installed in stage 2;
this stage is the blend on top of it.
Faithful branch: libplacebo ewa_lanczossharp, linear light, anti-ring.
Perceptual branch: SSIM (igv's `SSimDownscaler.glsl`, POSTKERNEL pattern) or
DPID. Per-pixel soft blend: edge strength → faithful; per-frame global grain
estimate (taken on the source) → reduce SSIM weight; mask temporally
stabilised with stage-4 vectors. **Scale factors differ per axis** because of
the anamorphic stretch — separate horizontal/vertical radii; vertical may be
≈1:1. Run both branches in linear light. Smallest payoff; last.

### Stage 9 — Ambient-light profile switching (after stage 7)
Room light raises the visible black; the correct response is to tone-map to
that black, not to change gamma ad hoc. Two rendering profiles on keys:
lights-off with `sdr_black_nits` at native black, lights-on with it raised to
the measured ambient-lit black (spectro, or by eye to start); BT.1886 as the
gamma target behaves the same way. Composes cleanly with the LED module.
Automatic version: room lights are not smart, so use a lux sensor —
ESP32/Arduino + VEML7700 (or TSL2591) over USB serial, or a Yocto-Light-V4 —
mounted facing the ceiling behind the seating so it never sees the screen;
calibrate lights-off/lights-on readings once; switch with hysteresis.
`[ambient]` config section; read on the LED module's serial thread.

## 7. Test protocol (stage 3)

1. Unmodified build, `dithering: off`, 10-bit grey ramp: photograph. Then
   `dithering: error_diffusion`: photograph. References.
2. Sigma-delta on, `param_test_ramp: 1`: tripod, exposure ≥0.5 s (≥30
   refreshes). Expect a smooth ramp. `param_enabled: 0` with the ramp must
   show 8-bit steps.
3. 1/60 s exposure of the same ramp: fine unstructured noise, no periodic
   pattern (pattern ⇒ raise `noise_amplitude`).
4. Flat mid-grey at a half-code level: look for flicker at normal distance
   (flicker ⇒ raise `noise_amplitude` slightly).
5. Slow pan over detail: no trails or ghosting; grain should look like plain
   dither.
6. Compare ramp top end between `error_diffusion` and sigma-delta to settle
   converter rounding vs truncation.
7. Record results here. (Spectro not yet set up; camera only for now.)

## 8. Config contracts

```
[shader.standard.sigma_delta]
shader_type: custom
label: Temporal sigma-delta
glsl_file: Temporal sigma-delta.glsl
param_enabled: 1
param_levels_full: 0
param_swapchain_bits: 10
param_noise_amplitude: 0.15
param_motion_threshold: 1.0
param_test_ramp: 0
```
Every tunable is a `//!PARAM` with DESC/MIN/MAX/default, settable via
`param_<name>`; compile-time sizes use `{{…}}` templates.

Test config (`VideoProcessor-test.cfg`, launched with `/config`) carries over
from the old shortcut: `hdr_colorspace: FOLLOW_INPUT_LLDV`,
`hdr_luminance: FOLLOW_INPUT_LLDV`, `frame_offset: AUTO`,
`output_range: limited`, `renderer: VP Renderer`, fullscreen on,
`video_conversion` off (v210 native), `Low_Latency` profile untouched, and
the picture side from §4b: `target_primaries: p3_d65`, `output_gamma: 2.4`
(projector stays at film 2.2), `calibration_lut_enabled: false`,
`sdr_target_nits: 100`, `peak_detection: off`, static `tone_mapping`,
`contrast_recovery` matched to madVR's Log Medium–High, `saturation: +5`
(new key), `dithering: error_diffusion` as baseline, viewport with
`anamorphic_scale: 4:3`, `blank_left: 10`, `blank_right: 15`, automatic crop
approximating the zoom169/zoom1 rules, standard shaders: SSimDownscaler
(gamma light to match today; A/B linear later), CfL chroma (A/B vs
ewa_lanczossharp), sigma-delta. No frame mixing in the first deploy (see
§4b); 24p shows plain 3:2 until stage 5/6.

## 9. References (reach for these rather than inventing)

- igv `SSimDownscaler.glsl` / `SSimSuperRes.glsl` — use as-is (check licence
  header before any PR).
- Artoriuz CfL (chroma-from-luma) and ArtCNN; igv KrigBilateral — chroma
  upscaling hooks standing in for madVR NGU (no open NGU exists).
- FFmpeg `vf_minterpolate` — block matching, bidirectional vectors, occlusion
  handling, OBMC (port the algorithm to compute shaders).
- FFmpeg `scdet` / `select` scene score — cut detection.
- DPID (Weber et al.) reference implementation — downscale alternative.
- Wolfe et al. spatiotemporal blue-noise masks — if IGN leaves visible
  structure (embed as `//!TEXTURE`, index by `frame`).
- Barco "F(L)35, FS35 IR, F85 ASCII Commands Protocol Reference Manual"
  601-0381-00 — serial protocol.
- No prior art to port for the sigma-delta, the strength-scaled de-judder
  with hard-hold fallback, or the hook-chain change.

## 10. Open questions

- Preflight / Config app acceptance of `.glsl` in the standard list (stage 2).
- rgba16f storage support on this D3D11 build (else rgba32f).
- Converter rounds or truncates (stage 3).
- LED response time and `?N` minimum; `SHUT`/`PMUT` speed (stage 7).
- Real sustainable serial command rate (stage 7).
- Script loop period for tick matching (stage 7).
- RealColor white y 0.340 vs D65 (spectro session, unrelated to this work).
- Differences between madVR HDR profiles 1/2/3 and the rules for colour /
  calibration profile groups 2 and 3 (assumed P3 vs 2020 pair).
- Exact fork keys for saturation (to add) and whether `contrast_recovery`
  maps closely enough to madVR's.
- Upstream PR vs private patch (after stage 3 works).
