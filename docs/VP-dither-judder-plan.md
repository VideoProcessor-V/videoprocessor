# VideoProcessor: temporal dithering, judder reduction, LED drive — project brief

Status: design agreed; Stage 2 blanking keys and hook chain coded (untested on
the projector); Stage 7 in progress. Revision 15, 2026-10-11, against
`billslack2/videoprocessor` tag `1.4-beta-rc1`. **Rebase onto `1.4-beta-rc2`
(subtitle moving, remote Config) after the current Stage 7 step and before
Stage 5**, since RC2 touches the viewport/active-picture area Stage 5 builds
on; re-check the lookahead path and the sample config after rebasing.

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
   games). Queue profiles auto-select by the fork's `when:` rules on
   `source_rate` (> 59 → `Low_Latency`, else cinema), mirroring the madVR
   fps rule; Shift+L stays as manual override. Verify the numeric `when:`
   syntax from the rule parser. Dithering stages run in every profile (sub-frame cost). De-judder
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
   full so the driver does not remap; projector input level "video". The
   quantizer is written for this.

   BTB/WTW survive VP's own code but **not every libplacebo conversion**.
   Measured on the GPU harness on 2026-10-11 with libplacebo 7.360.1:
   10-bit limited YCbCr in, R10G10B10A2 limited out (colour depth 8, bit
   shift 2), neutral greys at codes 16, 40 (BTB), 64 … 940, 980 and 1019
   (WTW). Default and fast parameters gave the same results.

   | Conversion | BTB (16, 40) | WTW (980, 1019) |
   |---|---|---|
   | none (same transfer and primaries; `sdr_adjust_gamma: passthrough` with rec709 target) | kept | kept |
   | transfer only (BT.1886→2.2, 2.2→2.4) | clipped to 64 | kept (slightly reshaped) |
   | primaries BT.709→P3 (owner's `target_primaries: p3_d65`) | clipped to 64 | kept |
   | primaries BT.2020→P3 | clipped to 64 | **clipped to 940** |
   | full-range output | clipped to 0 | clipped to 1023 (no room, by definition) |

   - BTB is lost whenever libplacebo linearises, which it does for any
     transfer or primaries conversion. WTW survives linearisation, but
     BT.2020→P3 gamut mapping clips it.
   - In VP's own code:
     - the sigma-delta quantiser keeps codes 0–255 (BTB/WTW intact);
     - `OutputEncodingRepair` has no clamp;
     - the de-judder works on source codes and does not clip.
   - The LED compensation shader runs whenever `gamma_boost` ≠ off, even
     with the LED disabled, and does `max(color, 0)`. That clips BTB; it is
     redundant after any conversion above, but not in the passthrough case.
     With gain > 1 (compensate mode) the knee also compresses WTW toward
     white, by design.
   - HDR sources are tone-mapped, so BTB/WTW do not apply in the same way.
   - Measured further (`docs/VP-btb-wtw-investigation.md`): SDR upscaling
     (sigmoid) also clips WTW, and downscaling (linear light) clips BTB.
     **Stage 10** preserves both for SDR (`preserve_extended_range`);
     decisions in `docs/VP-btb-wtw-answers.md`.
7. **Survey before designing.** Before starting any stage, read the fork's
   source for mechanisms that already do part of the job, and build on them
   rather than beside them. This has already collapsed the lookahead work
   (the fork's black-bar lookahead walk carries our statistics) and the
   time-based queue (a derived frame count, not a new policy). For stage 6
   in particular, inspect the fork's frame-mixing scaffolding
   (`pl_render_image_mix`, the "frame_mixing_active" path, `pl_queue` use if
   any) even though it is switched off, and report what it provides before
   proposing a two-frame render path. Write the survey findings into this
   document under the stage before writing code.
8. **Edge blanking is the gating requirement for the first deploy.** The
   light-engine mod leaves the extreme left and right columns blue-tinted and
   unusable; madVR currently masks left 10 / right 15. The fork has NO
   horizontal blanking today (`screen_edge_padding` is only a vertical shift
   of a fitted picture, see `vprenderer/AnamorphicPresentation.h`). Add
   `blank_left`, `blank_right`, `blank_top`, `blank_bottom` (output pixels,
   default 0) to the viewport profile; they shrink the destination rectangle
   so the picture is scaled into the lit area and the blanked columns are
   black. Code this FIRST in stage 2 so it is in the first deploy; it is the
   first thing checked in the first cinema session. (The owner is banking
   builds and testing in batches, so the hook chain may be coded before that
   session; the blanking commit stays separate.) No uniformity mask — a
   colour-cast edge cannot be gained back.
9. **No P010 conversion.** The old path needed `V210_TO_P010` (4:2:2→4:2:0,
   halving vertical chroma) for the anamorphic stretch. The VP Renderer takes
   v210 4:2:2 natively; `video_conversion` is off in the test config and the
   stretch is done by the viewport/NLS profile.
10. **Config-adaptable, not setup-specific.** Nothing outside the LED module
   may assume limited range, 8-bit output, a particular gamma (2.2/2.4),
   P3, 2560×1600 or a 60 Hz display. Range, bit depth, transfers,
   primaries and raster come from the active profile or the hook-chain
   templates, and each stage's tests run in both `output_range` settings.
   The LED module is the one place the owner's projector numbers live
   (light-meter fit, CNTR reserve, latencies), by design.
   (`docs/VP-delta-5-config-adaptable.md`)
11. **Colour chain stays as is:** projector RealColor targets P3 (0.680/0.320,
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
  domain: limited on the 10-bit swapchain = 4k/1023 = code/255.75 (corrected
  2026-10-10; it was written as code/256), limited on an 8-bit swapchain and
  full range = code/255. The renderer then multiplies by
  `1/scale` and stores to the swapchain. Exact 8-bit code k in the top bits of
  the 10-bit surface is produced by emitting limited: 4k/1023, full: k/255
  (8-bit swapchain: k/255 either way).
- Hook parser supports persistent `//!TEXTURE … //!STORAGE` (data optional),
  `//!COMPUTE bw bh [tw th]` writing `imageStore(out_image, …)`, `//!PARAM`
  with `//!TYPE float|int|DEFINE|CONSTANT|DYNAMIC`, uniforms `frame` (int,
  per render) and `random`. Bound textures expose `NAME_raw/_pos/_size/_pt/
  _tex()/_mul`. `pl_hook` exposes `stages` and `parameters`.
- Settled (stage 6a re-audit, measured on a real R10G10B10A2 target): in
  7.360.1, limited range on the 10-bit swapchain lands code k exactly at 4k
  (white 940, black 64). The earlier "4k−1" worry does not apply. Only the
  Barco box's own rounding remains to check (stage 3).
- At OUTPUT the domain for **limited range depends on the swapchain**:
  - 4k/1023 = code/255.75 on the 10-bit swapchain (the default);
  - code/255 on an 8-bit one (`diagnostic_force_8bit_sdr_swapchain`).
  The sigma-delta read both as code/256 from Stage 2 until 2026-10-10. On
  the 10-bit cinema path, peak white read 235.23 and flickered to 236 about
  one refresh in four; on the 8-bit swapchain white read as 236. It is fixed
  in the quantizer and both luma passes.
- The rgba16f OUTPUT texture holds values above mid grey only to 1/8 of an
  8-bit code. It may also truncate (WARP does): white 235 arrived as
  234.875 and the carried residual dipped it to 234 every fifth refresh. The
  sigma-delta now treats a value within 0.13 code of a whole code as that
  code; 10-bit detail (0.25-code steps) is unaffected. Tested on a real
  10-bit target: white 940 and black 64 on every refresh, and mid grey
  averages libplacebo's own 10-bit result.
- **PRE_OUTPUT texture-input hooks make 7.360.1 skip
  `pl_shader_encode_color`** ("Attempted to modify an immutable shader!").
  `pass_output_target()` holds its shader across the hooks, and a
  texture-input hook finishes it first.
  - VP repairs this at OUTPUT (`OutputEncodingRepair`, first in the chain,
    before the sigma-delta).
  - **Every build from Stage 7 step 4 to e7b3cceb output full range.**
  - A GPU test asserts that the bug still exists. When a libplacebo update
    fixes it, that test fails: remove OutputEncodingRepair.
- **A `//!` anywhere, including inside a comment, starts a pass.** The parser
  skips leading text only up to the first `//!` in the file. Shader-writing
  convention for VP: comments never spell the directive prefix.
- **Some pass failures are only logged.** A texture bound twice in one pass,
  linear sampling of a non-linear format, or a storage binding of a
  non-storable texture: `pl_pass_run` logs an error and skips the pass, and
  the dispatch still returns success. GPU tests therefore require a clean
  libplacebo log.

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
  it is part of the stage 6 presentation-path change — NOT in the first
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
   10 / 15 / 0 / 0, constraint 8) + the test config, as its own commit;
   **then** hook chain + `Temporal sigma-delta.glsl` + igv's
   `SSimDownscaler.glsl` + a chroma hook (Artoriuz CfL as the NGU stand-in)
   as `[shader.standard.*]` entries + a `saturation` rendering key. One
   combined deploy folder with all shaders disabled by default. Stage 2
   addendum (upscaling shaders) as a small follow-on step.
3. Stage 3 — ramp photo. Go/no-go for everything after.
4. Stage 7 — LED module, **reactive form first** (lead time 0): port,
   gain compensation, highlight policy, configurable floors, side-by-side
   logging against the script. Needs only the hook chain and serial code.
5. **Checkpoint:** Stage 2 addendum (upscaling shaders); then review
   packages for every completed step → design review → fixes → Code-session
   audits per `docs/audit/README.md` (one audit file per step, re-audit if
   blockers); then **rebase onto
   `1.4-beta-rc2`** and audit the rebase (conflicts, lookahead path, config
   key changes). Nothing further is built on RC1.
6. Stage 5 — time-based queue. LED module then gains lookahead by config;
   audio delay set once.
7. Stage 4 — motion-compensated residual carry.
8. Stage 6 — de-judder (open-ended, skippable).
9. Stage 8 — adaptive downscale blend on top of the SSIM shader.
10. Stage 9 — ambient-light switching.
11. Stage 10 — extended range (BTB/WTW preserved for SDR), after Stage 6
    and delta 5.

Every step ends with: a commit → a review package per
`docs/REVIEW-PACKAGE.md` for the design reviewer → the implementer addresses
design findings → then the Code-session audit per `docs/audit/README.md`.
Design review comes first so an implementation change does not invalidate a
code audit already done.

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
0. Before anything else: the four `blank_*` viewport keys (constraint 8),
   as their own commit, first item on the first cinema checklist. Then a
   `saturation` key in the rendering profile (libplacebo colour adjustment;
   madVR runs +5).
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

**Stage 2 addendum (after stage 2 was completed):** add an image *upscaling*
hook for sub-native sources (1080p → 1600p is a 1.5× upscale). madVR uses
NGU Sharp for this; the open stand-in is igv's `FSRCNNX_x2_16-0-4-1.glsl`,
with Artoriuz ArtCNN as the A/B alternative and igv `SSimSuperRes.glsl` as
an optional refinement pass. Install as a `[shader.standard.*]` entry with
a rule that engages only when the source height is below the current
*output* height — use the output-height variable, never a literal, so it
survives a raster change — and make the SSIM downscaler's rule the mirror
(source height above output height). Verify the rule syntax from the fork's
parser. Disabled by default. Cost: a few ms at 1080p on the 3090.

### Stage 3 — Verify on the projector
Ramp protocol (§7). Settle converter rounding vs truncation. Tune
`noise_amplitude`, `motion_threshold`. If the time-average does not converge
on this display, stop and reassess before any further stage.

### Stage 4 — Motion-compensated residual carry

**Status (2026-10-10): built, not yet on the projector.** It is on
`stage7-rc2` and lives in `shaders/Temporal sigma-delta.glsl`, which is
still one file. The new parameter `motion_compensation` defaults to 0
(off), so the sigma-delta can be judged on its own first (stage 3); 1
switches the motion-compensated carry on. Both are in the Config app's
Shader settings box.

Passes:
1. Quarter-resolution luma, from gamma-encoded code values, plus a coarse
   1/16-resolution level computed from it.
2. Hierarchical block matching per 16x16 block:
   - coarse: a full search within ±4 coarse texels (±64 full pixels);
   - fine: a full search within ±2 quarter pixels of zero and of the coarse
     result, and ±1 of the block's last non-zero vector. That vector is kept
     through 3:2 repeats, so a film pan keeps its predictor.
   Vectors and confidence (mean luma error ≤ `match_tolerance`, default
   1.5 codes) go into a texture. A first version without the coarse level
   was not enough: its three-step search missed 20 px/refresh motion on
   fine texture.
3. A state pass. The quantizer reads one set of residual/value textures and
   writes a staging set, so no pixel overwrites a value another pixel still
   reads.
4. The quantizer. It takes the residual from where the content was on the
   previous refresh. Each carried value must pass the existing per-pixel
   motion gate; otherwise it falls back to the plain gate, then to zero.

libplacebo's `frame` uniform counts shader passes, not refreshes, so it
cannot be used to alternate the buffers; that is why there is a staging
set.

Verified on the real libplacebo path (WARP) in VideoProcessor-Test:
- A textured picture moving at 4 or 20 px per refresh: the time-average
  along the motion comes back to the true value (the plain gate leaves a
  quarter-code bias).
- Still pictures are unchanged.
- A scene cut carries nothing.

**Backward-only by design; occlusion handling is deferred to the Stage 6
plumbing (see Stage 6).** This is the sigma-delta delta 2 decision,
2026-10-10. At an occlusion a wrong backward vector can deposit at most
±0.5 code for one refresh, and only where both `match_tolerance` and the
per-pixel gate passed, so the effect is bounded and rare. Until then,
`match_tolerance` is the owner's lever: if reveal edges ever show anything
in a dark-scene A/B, tighten it.

The GPU time budget (≈1 ms) has not been measured on the 3090. Storage is
six full-size rgba16f textures, about 200 MB at 2560x1600. They are full size only because a hook file
cannot size a texture as a fraction of the output (`{{output_width}}` is the
only size it gets); if the fork ever offers divided sizes to hooks, the
previous-luma and previous-vector textures should shrink to 1/4 and 1/16
(design review 3, N-4). If the GPU time is over budget, the first lever is
the coarse search range (±64 px), the second the fine ±2 sets (N-5).

Code audit (docs/audit/stage4-mc-dither.md), responses:
- Should fix, `motion_compensation 0` was not byte-for-byte the stage 2
  output: fixed. The quantizer seeds its noise with `frame / 5`, which is
  the refresh count, because libplacebo counts 5 passes per refresh. The
  noise sequence is now the stage 2 one.
- Should fix, 4-pixel vector steps: fixed. A full-resolution refinement
  compares every whole-pixel vector within ±2 px of the quarter result on
  the 16x16 block, using last refresh's stored values. Trust now comes from
  that full-resolution match. MV now holds full-pixel vectors; MVPREV keeps
  quarter ones. New GPU tests cover 5 and 10 px/refresh, diagonal (6, 3)
  motion, and noise 0.15 (carried vs plain gate).
- Note acted on: with `motion_compensation 0` the search is skipped, saving
  its GPU time.
- Notes left as they are, since every carry is still gated and so harmless:
  - picture edges (the carry source is checked against the whole output,
    not the picture);
  - LQPREV's single validity tag at (0,0).
- Still untested:
  - limited range and a 10-bit swapchain: the GPU harness reads back 8-bit
    full range, and the quantizer's range handling is unchanged from
    stage 2;
  - occlusion edges;
  - 3:2 cadence.
- Projector gamma: the gamma boost exponent is `compensation_eotf /
  gamma_boost`, so at 2.41 with boost 2.4 there is practically no lift
  left. The two move together, as design review 2 SF-1 expected.
Compute pass before the quantizer: hierarchical block matching on
gamma-encoded luma at quarter resolution (vector error only misplaces ≤0.5
code), forward-only from a persistent previous-luma texture; vectors +
confidence in a storage texture. Quantizer warps the residual along vectors;
low confidence ⇒ existing gate. Budget ≈1 ms. Reference: FFmpeg
`vf_minterpolate` (algorithm, not code).

### Stage 5 — Time-based queue target (cinema profile; low-risk form)

**Status (2026-10-10): built, not yet on the projector.** Built on
`stage7-step5-delta1`, before the planned rebase onto rc2, at the owner's
request. Where it lives:
- `src/VideoProcessor-Lib/QueueLatency.h`: the derived count and its order of
  precedence.
- `CVideoProcessorDlg::ApplyUnifiedProfileSnapshot`: re-evaluated with every profile
  snapshot, which changes when `${cadence}` does.
- The Config app's Queue page: "Target delay", with a note showing the real
  delay at 24, 25, 30, 50 and 60p.
- The stats OSD: "Queue delay: N fr = X ms".

Rules as built:
- A profile's own `target_frames` switches off a time target inherited from
  the default queue (so `Low_Latency` keeps 1 frame).
- `target_latency_ms: 0` also switches it off.
- Range is 0-1000 ms, and the result is clamped to 1-16 frames (same as
  `target_frames`).

The test cfg sets 200 ms on Normal (owner, after design review 2 N-5;
first built with 120 ms). That gives 5 frames at 24p (209 ms, 4 future
frames, about 167 ms of LED look-ahead), so the audio delay goes up about
84 ms, once. A plain `[queue]` section with
no profiles ignores the key, because profiles are what carry the source
rate.
New queue-profile key `target_latency_ms` (e.g. 120). **Implement as a
derived frame count, not a new presentation policy:**
`target_frames = ceil(target_latency_ms × source_rate)`, re-evaluated on a
source-format change. The fork's pacing code (cadence repeats, clock-drift
handling, startup preroll) works in frame counts and must be left untouched;
it only receives a number that now comes from time. Profiles without the key
behave exactly as today; `Low_Latency` unchanged. Raise `queue_size`
accordingly (8 frames at 60p is trivial). OSD shows the effective delay
(quantised to whole source frames: ≈125 ms at 24p, ≈133 ms at 60p for a
120 ms target) so the audio offset is set once.

Lookahead: the fork already walks queued future frames for its black-bar
detector (`AnalyzeActivePictureLookahead`, `ActivePictureDecisionTimeline`,
per-profile `active_picture_lookahead_frames`, clamped to what the queue
holds). Stage 7's statistics reuse that path — a second analysis in the
same walk with its own frame-depth key — never a parallel mechanism. In
`Low_Latency` the queue holds one frame, so both analyses act on the current
frame, as the crop detector does today. Upstream candidate: the derived
count and a generic statistics hook, with no reference to the LED module
by name or purpose.

### Stage 6 — Low-strength motion-compensated de-judder (cinema profile)

**Design settled (2026-10-10):** see `docs/VP-stage6-dejudder-design.md` and
the planner's answers in `docs/VP-stage6-dejudder-answers.md`. Those answers
override the sketch below where they differ.

- **Shape:** warp the *source* frame A by `s·δ·v` (Y and CbCr planes; the
  chroma horizontal vector is halved for 4:2:2) and hand it to the normal
  render. Everything downstream applies unchanged.
- **Presents:** extra presents for the refreshes A already owns, inside the
  existing render loop (2a). B is never delayed. Drift correction is
  untouched.
- **Switch:** one flag, `dejudder_strength` (rendering profile; 0 or absent
  = off, which is today's path byte-for-byte).
- **Automatically off:** in `Low_Latency`, for non-film cadence, and at
  cuts.
- **Motion:** a half-resolution 8×8 matcher on source luma, refined to half
  a pixel. Forward/backward consistency within 1 px; a separate
  match-tolerance key. Failure means a hard hold of A's pixels. Edges clamp.
- **Occlusion mask:** M1 (mask in the warped frame's alpha, read by the
  sigma-delta at OUTPUT), proven on the GPU harness first; M3 (accept the
  bounded error) is the fallback.
  **6c tries M1′ first** (planner, 2026-10-10): a native OUTPUT-stage hook
  after `OutputEncodingRepair` writes the mask into the OUTPUT FBO's alpha
  for the sigma-delta. M3 is the fallback; see the answers file.
  **Mask-only mode is required in 6c:** `occlusion_mask: on|off` beside
  `dejudder_strength`. With strength 0 and the mask on, the matcher runs
  once per source frame and publishes the mask, with no extra presents and
  no warp (design review 4). If M3 is the outcome, it is a documented no-op.
- **LED:** the correction ratio pairs readings by source-frame identity,
  not present count.
- **Acceptance criteria:** `s` global per frame, a hard hold on failure, no
  blend.
- **Build order:** 6a standalone component with WARP tests → 6b ring,
  presents, phase, flag, stats, Config → 6c mask.

**6a status (2026-10-10): built, not connected.** `vprenderer/DejudderWarp.h/.cpp`
holds `Dejudder::MotionWarp`, a native compute component with its own
`pl_dispatch`.

`Estimate(A, B)` runs the motion search:
- Half-resolution luma, with a pyramid at 1/8 and 1/32 source resolution.
- Per 8×8 half-resolution block (16×16 source pixels):
  - a full search at 1/32 (±3 texels = ±96 px);
  - at 1/8, half-texel steps within ±3 of zero and of the 1/32 result;
  - at half resolution, whole pixels within ±3 of both 1/8 results and ±2
    of zero, then half a pixel.
  Keeping both candidates made the search robust: two pure coarse-to-fine
  versions failed on the tests.
- The search runs both ways. A block is trusted when its match error is
  ≤ `matchTolerance` (3 codes) and the two directions agree within 1
  half-resolution pixel.

`Warp(A, s·δ)` warps the Y and CbCr planes:
- the vector is interpolated bilinearly between trusted block centres;
- the CbCr vector is divided by the subsampling (horizontal halved for
  4:2:2);
- edges clamp;
- an untrusted block, or a `trusted = false` frame, is a hard hold of A's
  pixels. There is never a blend.

WARP GPU tests in `LibplaceboLutParserTests.cpp` cover:
- (12, 6) px motion: ≥95% of blocks trusted with the right vector; luma and
  chroma land at s·δ = 0.5; edges clamp, never black;
- (7, 3) px: within 1 px;
- a cut: ≤2% of blocks trusted, output = A;
- occlusion: a square moving over a still background. The covered edge is
  untrusted and held; the square and the far background keep their motion.

A libplacebo quirk found on the way: a pass that binds the same texture
twice is rejected only in the log, not as a failed dispatch.

**6b-1 status (2026-10-11): connected, estimate only, picture unchanged.**
- Input-profile keys `dejudder_strength` (0..1, default 0 = off) and
  `dejudder_match_tolerance` (0.5..16, default 3). Both are validated,
  in the fingerprint, and live-applied.
- RenderLoop hands RenderLocked the next queued source frame's converted
  planes (B). That happens only when B is in the same generation, is not a
  cadence repeat, has a different source sequence and is already converted.
  Nothing new is queued or waited for. B must also have A's video state
  (encoding, EOTF, colour space, orientation and display mode).
- `DejudderPolicy::Decide` (pure, unit-tested) gives the reason. The checks
  run in order: off, Low_Latency queue, interlaced input, rates unknown, not film cadence
  (source ≥ display/1.5), RGB capture, plane caps (sampleable, LINEAR,
  STORABLE), no next frame, next frame differs. Each change of reason is
  logged, at most one line per 5 s; failures are logged on the 1st and
  every 2400th.
- When active, B is uploaded with A's plane layout, and `Estimate(A, B)`
  runs once per source frame, never on cadence repeats. It is logged on the
  1st estimate and every 2400th.
- Off, low-latency, non-film and RGB free the matcher and B's textures.
  With strength 0, nothing new is uploaded or dispatched.
- Moved to 6b-2: extra presents, phase, the stats line, the Config box,
  LED pairing and the GPU time measurement. The Config box and stats line
  are not needed while nothing visible changes.

**6b-2a status (2026-10-11): extra presents built; not yet tried on the
projector.** Design: `docs/VP-stage6b2-design.md`; the planner's answers
were applied.
- **Where:** one block at the end of `RenderLocked`
  (`PresentDejudderExtras`), after A's present and all of A's telemetry.
  It runs only when the 6b-1 reason is Active, A rendered and presented,
  and A's motion was estimated against the current B. It reuses A's
  `renderImage`, `target` and `frameParams`. Per extra, only the
  backbuffer, the two source planes (the warp output) and the hook list
  change: the hooks lose the LED measurement, so there is one exact
  reading per source frame (LED guide §2).
- **One at a time (a change from the note's "back to back").** Before each
  extra, the block:
  1. waits for vblank (`IDXGIOutput::WaitForVBlank`) until the frame
     statistics show the previous picture (A or the last extra) on screen,
     at vsync V_prev;
  2. asks `DejudderPolicy::PlanExtra` whether V_prev + P_d is still A's;
  3. if so, warps, renders and presents.

  Nothing is queued ahead, so a wrong guess can never build a queue; a
  missed extra is just A shown again, as today.
- **Never delay B:** the extras stop when:
  - B (or anything newer, a reset or a stop) has arrived in the queue;
  - B could still make that refresh. B's present call is predicted from the
    capture clock: A's callback + P_s + A's own (smoothed)
    callback-to-present time, plus A's render time and 4 ms;
  - it is too late to render for the refresh;
  - an extra finishes after its refresh (counted as "late");
  - the safety cap of 3 is reached.
- **Phase (a refinement of the note):** A's first vsync is *observed* from
  the frame statistics, not predicted.
  - L = the minimum of (first vsync − callback) over the last 120 source
    frames.
  - δ = (V − callback_A − L)/P_s. At 24p on 60 Hz, A's first refresh
    reads 0 or 0.2.
  - The constant only decides which refresh counts as phase 0.
- **Holds** (no extras, so A is shown again exactly as today; counted and
  logged at most once per 5 s with the reason):
  - no estimate for this A;
  - a cadence repeat;
  - a composed source (subtitle moving): a Stage 6 follow-on, "extras with
    composed subtitles";
  - not flip presentation;
  - timing unknown;
  - render time > 0.6 of a refresh;
  - no DXGI swapchain or output.
- **Cadence rule (planner 6b-2 item 4, 6b-2a):** extras run only for an
  uneven film cadence: refreshes per source frame within neither ±0.05 of
  a whole number, and at least 1.95.
  - Included: 24p and 23.976p on 60 Hz (2.5), and 25p on 60 Hz (2.4).
  - Excluded: 25p/50, 30p/60, 24p/48, 24p/120, 1:1, and 50p on 60 Hz
    (1.2). 50p is video, often live TV, and an extra there would be motion
    smoothing.
- **Frame trust:** a GPU pass counts the trusted blocks. Below
  `dejudder_frame_trust` (default 0.25) the warp holds the whole frame,
  with no read-back. The cut test requires the output to equal A exactly.
- **A's records unchanged:**
  - The extras never touch A's presentation record, drift correction or
    the pacing traces.
  - RenderLoop takes A's render cycle, queue ages and capture-to-submit
    latency at the moment A's own present finished (a callback before the
    extras). When the extras ran, these read as they would without extras.
  - Drift correction reads the presentation debt (A records not yet on
    screen) and the last present id. Extras show before B, so the debt
    seen at B is as today.
  - The render thread holds `renderMutex` while it waits, for up to about
    two refreshes per source frame. B's arrival is not blocked by this:
    the capture callback (`OnVideoFrame`, the enqueue) and
    `ConversionLoop` never take `renderMutex`.
  - Other blocking users of `renderMutex` are on the render thread itself
    (the cadence bookkeeping, the look-ahead analysis, config application
    between frames), plus `Reset`, `Retire` and shutdown, which wait at
    most that long. The status getters for the UI use try-lock and never
    block.
- **Logging:** a line whenever L moves by more than one refresh, so a
  pipeline hiccup is visible (planner 6b-2a). The first 200 extras, then
  1 in 240, with φ0, δ, shift, ms
  and the Present block time. Holds at most one line per 5 s. One "phase"
  line per queue generation or rate change: the cadence, φ0, L, and the
  first frame's extras and stop reason.
- **Counters** for the 6b-2b stats line: extras, source frames with extras,
  holds and the last hold reason, swapchain full (a Present blocking more
  than half a refresh), late, render failures, last ms, cadence ratio.
- **libplacebo peak detection:** with `peak_detection` on, extras make the
  adaptation 2–2.5× faster in wall time at 24p. This is not corrected. The
  owner's cinema profile uses a static tone map, so it does not arise
  there.
- **Audit fixes (2026-10-11, `docs/audit/stage6b2a-dejudder-extras.md`):**
  - the allowance and the hold use A's whole processing time
    (pre-render work + RenderLocked to Present), not the render call;
  - "the frame after extras showed a refresh late" is counted;
  - `OutputEncodingRepair` is re-evaluated for the extras' hook list;
  - telemetry and the pacing trace see the main present (not an extra) as
    the latest on screen;
  - the phase line waits for 24 frames.
**6b-2b + 6c status (2026-10-11, one step as the owner asked): built, not
yet on the projector.**
- **Config box** (Rendering page, "De-judder (film motion)"):
  - a preset (Not set / Off / Gentle 0.25 / Medium 0.35 / Custom) bound to
    `dejudder_strength`, which takes any custom value;
  - `occlusion_mask` (on/off);
  - `dejudder_match_tolerance` and `dejudder_frame_trust` (advanced).
  - The preset follows a loaded value; the default profile's "Auto" counts
    as not set. Config app test: presets, Custom, save, mask.
- **Stats line** (Ctrl+I, "De-judder:"). Shown only when the de-judder or
  the mask is on. It reads:
  - with extras: s, cadence, extras of the last frame and their ms, late
    (an extra or the next frame), swapchain full, held;
  - otherwise: the reason, or "mask only (cadence x)";
  - plus "mask on" when the mask is set.
  The renderer publishes it at most twice a second. A new
  `IVideoRenderer::GetDejudderStatus` raises the plugin API to 25, so the
  exe and the DLL must come from the same build, as always.
- **6c, occlusion mask (M1′, proven on the harness, so M3 is not needed):**
  - `MotionWarp::Estimate(A, B, slot)` also writes **B's** occlusion mask:
    the backward/forward consistency test from B's side, 1 = seen in A,
    0 = new or uncovered. It uses two slots, so the mask for the frame now
    showing survives the next estimate.
  - `OcclusionMask::Hook` (OUTPUT, after `OutputEncodingRepair`, just before
    the sigma-delta) copies the picture unchanged and writes the mask into
    alpha. Each output pixel maps to its source block through the source
    crop and the destination rectangle; outside the picture it is 1.
  - The sigma-delta has a new parameter `occlusion_mask` (default 0 = alpha
    ignored, output unchanged), set by the renderer each refresh. With 1 and
    alpha 0 it carries no residual into that pixel, and it writes alpha 1.
  - The mask is used only on the **first refresh** of each source frame:
    - not on cadence repeats, and never on extras (the extras drop the hook
      and set the parameter to 0);
    - not with NLS active (non-linear mapping; logged once);
    - not when no sigma-delta in the chain takes it (logged once).
  - **Mask-only mode:** `occlusion_mask: on` with strength 0 runs the
    estimate on any cadence, with no extras (`DejudderPolicy::ExtrasAllowed`).
  - GPU tests (harness, 10-bit limited, colour depth 8):
    - writing alpha leaves the picture unchanged;
    - mask 1 keeps the carry, mask 0 stops it, alpha out is 1;
    - parameter 0 ignores alpha;
    - the mask lands on output x 40..63 for source block x 16..31 at 1.5×
      with 16 px blanking each side;
    - the matcher's mask marks the background a square uncovers as new.
  - Alpha ownership: the mask owns alpha from its OUTPUT hook to the
    sigma-delta; the Stage 10 carry owns it only from RGB to SCALED. They
    never overlap. This is written in both shader headers.
- **6c-2 (2026-10-11, planner 6b-2b/6c item 2): the mask with NLS.**
  - The mask hook follows the NLS shader's own per-axis map
    (`NlsMaskMapping`, shared with the renderer's NLS binding log so they
    cannot drift).
  - Classic NLS is the one-axis case. NLS+ is chosen by its file name and
    uses both axes with the balanced centre scales.
  - The map's 0..1 coordinates span the **source crop**. Measured: the
    MAIN-stage texture libplacebo hooks is the cropped picture. An
    off-centre crop tells the crop from the whole texture by 5 px.
  - GPU test: the owner's three profiles (standard, protected, NLS+) with
    a centred and an off-centre crop. A square at source block (13, 1) and
    the mask's stopped carry agree within 2 px in both axes.
  - Any other NLS shader, or a rule missing a parameter, keeps the mask off
    (logged once).
- **Audit fixes (`stage6b2b-6c-config-stats-mask.md`):** "mask on" only on
  recent use (otherwise the reason); cadence shown in mask-only mode; the
  next-frame-late prediction is tied to its frame; mask-only only for
  sources ≤ 30 fps (`VideoRateMaskOnly`, planner item 3).
- **Delta 5 note, for that step:** the sigma-delta's `levels_full` and
  `swapchain_bits` are typed into the shader profile, not read from the
  output. Delta 5 should take them from the active output.
- **Tally:** items 32–38 (pan judder, edges, frame timing, log lines,
  Medium, mask, 4K source).
- **Order of work after 6b-2a (planner, 2026-10-11):**
  1. 6b-2b (Config box, stats line);
  2. 6c (occlusion mask for the dither);
  3. Stage 7 step 7 (LED shader sign fix + deltas 3 and 4,
     `docs/VP-led-drive-delta-3-answers.md`);
  4. delta 5 (config-adaptable audit);
  5. Stage 10 (extended range).
- **A's own present stays unwarped.** Warping it by s·φ0 (≤ 0.1 frame) is
  a possible later refinement, not a plan.
- **Tests:**
  - policy: uneven cadence, `PlanExtra` (24p/60 phases 0.4 and 0.8; 0.6
    from phase 0.2; B near, B arrived, past the next frame, the cap,
    clamping), `LatencyFloor`;
  - GPU: frame trust and the cut hold equal to A exactly;
  - GPU: the extras' render path (warped planes through `pl_render_image`
    move the picture by shift × motion, and a zero shift renders A
    exactly);
  - settings: `dejudder_frame_trust`.
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

**Required deliverable of this stage (sigma-delta delta 2, 2026-10-10):**
- The two-frame render path includes a **generic app-provided texture input
  to the hook chain**. It must be PR-able, with no LED or dither naming.
- The de-judder's bidirectional matcher publishes its forward/backward
  consistency as an **occlusion mask** on that input.
- The sigma-delta gains an `occlusion_mask` input and skips the carry where
  a block is marked untrusted, falling back to the gate, then to plain
  rounding.
- Cinema profile only; `Low_Latency` stays backward-only.
- **Stage 6 is not complete until the sigma-delta is consuming the mask.**
  Occlusion handling is not optional once Stage 6 exists, and it is built
  here, not as a Stage 4 addition.

Not to do:
- render frames twice to get forward vectors;
- add a second motion-estimation pass to the dither shader;
- touch the presentation path for this.

The lookahead walk already has the next frame's raw luma on the CPU, but
delivering anything to the hook needs the app-provided texture input
above, which is also how the de-judder receives frame B. Build it once,
here.

### Stage 7 — LED drive module (native; replaces `beta_22.py`)
Built right after stage 3 in reactive form (lead 0); lookahead arrives with
stage 5 automatically, because the statistics ride on the fork's existing
queued-frame walk (see stage 5) and the available lead is whatever depth
that walk has. Detailed spec: `docs/VP-led-drive-module.md` and
`docs/VP-led-drive-answers-1.md` and `docs/VP-led-drive-answers-2.md`
(precedence answers-2 > answers-1 > guide > this brief: modes are
`compensate | script | log_only`, CNTR is never driven, hook at PRE_OUTPUT,
flux table for the non-stock light engine, `led_latency_ms`, slew-limited
per-frame ramps and early ramps before known cuts in `compensate` mode).
**Measurement (GPU, per frame, over the active picture, not a fixed crop):**
APL = weighted mean of the *output* codes (gamma-encoded, after tone
map/LUT, limited range, so black = code 16), weights configurable and
defaulting to the script's effective `0.114 R / 0.587 G / 0.299 B` (see
answers file §5);
highlight statistic = high percentile (≈99.5th) of 3×3-block means with a
rolloff, replacing the script's "max of 854×480 ≥ 215"; plus cut flag (step
change) and fade flag (monotonic APL decline toward black over consecutive
frames, structure otherwise static).

**Controller (ported from the script, parameter for parameter):**
LED target vs APL: 255 above 100; 230 at 70; 200 at 40; 100 at 20;
floor below. Step tables by distance to target, transcribed from the
script (up-steps are APL-dependent: `steps_up_dark` / `steps_up_bright`
split at APL 40; see answers file §2). Highlight rules: anti-crush (fast protective response when a
highlight appears) and anti-pump (ignore small differences in one direction).
`sudden_flag` path is dead code — omit. Tick period: the script's loop was
unfixed (~300–500 ms); start at 400 ms and match by eye, or from a timestamp
log if provided.

**New: gain compensation in VP instead of projector contrast.** Apply gain =
full/LED_actual in linear light at PRE_OUTPUT (after tone map, gamut map and
LUT, before encode and the sigma-delta; the hook linearises with
`compensation_eotf` = the projector's real 2.2), so content brightness is
constant and only the black floor moves. Gain must follow the
LED's *actual* trajectory (one-off camera measurement of LED step response →
model), never the commanded target, or there will be flashes. The projector
contrast command is never driven; `mode: script` reproduces the script's
contrast curve as an encoded-space gain in VP instead (answers file §1). Contrast steps were the most visible
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
- *One taste parameter:* `highlight_tolerance_stops` (stops of knee compression
  accepted in exchange for a lower floor; start ≈1/3 stop). The controller
  picks the lowest LED level that keeps compression within it. Tiny
  highlights spend little of it, so dark scenes go deep. Nudgeable from a key.

**Modes:** cut ⇒ jump (masked by the cut); scene ⇒ drift at the script's
rates (`script` mode) or slew-limited per-frame ramps (`compensate` mode);
fade ⇒ track to black, contrast/gain unchanged. Lead is not a setting: it
is derived from the lookahead walk depth and the source rate. The LED's
measured `led_latency_ms` is used both ways: with lookahead, commands go
that far ahead of the frame; without it, the gain is delayed by that much.
In `script` mode the LED never moves earlier than that; in `compensate`
mode it may ramp ahead of a known cut, because the gain holds the content
constant, bounded by the current scene's highlight headroom. The OSD
reports the effective lead. In `Low_Latency` the walk holds one frame, so
the module is reactive (the only residual artifact is a brief soft dip
after dark→bright cuts).

**Config:** `[projector.Default]` with `led_latency_ms` and `flux_table`;
`[led_drive.Default]` with `rate_up`, `rate_down`, `deadband`, `early_ramp`,
`tick_ms` (script mode), `led_floor` (default 100, the owner's anti-pump floor),
`led_floor_black` (default 60, for fades/cuts to black), `led_ceiling`,
`highlight_percentile`, `highlight_tolerance_stops`, `highlight_persist_ms`,
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

A fifth limit, and the origin of the gamma boost: the script starts dimming
the LED below APL 100 but only raises contrast below APL 60, so scenes
between 60 and 100 lost light with nothing compensating. The 2.4-vs-2.2
`output_gamma` mismatch was a global patch for that average loss.
`compensate` mode ties gain to the LED level at every APL, so that loss no
longer exists. **Do not change `output_gamma` or `contrast_recovery` in
code**; they stay at 2.4 / Log Medium–High for the like-for-like comparison.
Afterwards the owner revisits them by config, stepping gamma toward 2.2 with
contrast recovery following (they are one adjustment with two knobs) and
stopping where it looks right — possibly at 2.4 anyway.

**Owner decision, 2026-10-09 (supersedes the paragraph above for
`output_gamma`):** the gamma boost lives in the LED module as a switchable
option, `[led_drive] gamma_boost: 2.4` (or `off`), applied at the
PRE_OUTPUT compensation hook whether or not the module drives the LED, and
the measurement sees the boosted picture. `output_gamma` is 2.2 (the
projector's real gamma) in the test configuration. With no calibration LUT
the picture is identical to `output_gamma: 2.4`. Stepping the boost toward
2.2 or `off` is the config knob described above; `contrast_recovery` is
unchanged.

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
`[ambient]` config section; a second `SerialPort` instance with its own
small driver (the transport is already separated from the Barco driver).

### Stage 10 — Extended range (after Stage 6 and delta 5)

Approved in `docs/VP-btb-wtw-answers.md` on `docs/VP-btb-wtw-investigation.md`.
- 10a: a carry hook at `PL_HOOK_RGB` (the out-of-range offset in alpha) and a
  restore + VP-owned SDR conversion at `PL_HOOK_SCALED` (sign-preserving
  pure-power transfers, BT.709 → P3 matrix only), behind
  `preserve_extended_range: on|off` (off = today's exact path; default off
  until the cinema check passes). Auto-off with a log line for SDR BT.2020
  sources and non-power transfers. HDR/LLDV out of scope. No libplacebo fork.
- 10b: the LED compensation shader sign-preserving fix (planned earlier, with
  Stage 7 step 7).
- Alpha ownership: the carry owns alpha from RGB to SCALED; the occlusion mask
  (M1′) owns it from the native OUTPUT hook to the sigma-delta. They never
  overlap.
- Tests: the probe tables in both switch states; in-range bars max 1 code,
  mean ≤ 0.03 against off at up 1.5 and down 0.5; the auto-off cases.
- Projector check: a 235→255 ramp (it must continue above white with the
  switch on) and a PLUGE at 8-bit codes 4/12/16/20 (record whether the FL35
  shows sub-black at input level video).

**Built (10a, 2026-10-12).** `vprenderer/ExtendedRange.{h,cpp}`; profile key
`preserve_extended_range: on|off` (input group, default off; Config page
"Processing" card).
- **Survey:** libplacebo always converts from the frame's *declared* colour
  space to the target (`pass_convert_colors` uses `image->color`, not the
  hooked image's), so a hook cannot relabel the picture. Declaring the
  source with the target's colour space is the only no-conversion path that
  leaves the render target (and so overlays and subtitles) untouched.
- **Gamma change (the cinema chain is 2.4 in, 2.2 out):** the carry first
  re-encodes source → output gamma, sign-preserving, at `PL_HOOK_RGB`.
  libplacebo then linearises with the output gamma and gets exactly the
  linear light it scales in today. Declaring only the transfer as the
  output's, without this, changed the scaling look (edges mean 5.7 codes,
  max 54). The BT.709 → P3 matrix is applied at the restore: linear, so after
  scaling is the same picture.
- **The carried offset is stored in alpha as it is** (o in −1..1), so
  in-range pixels carry exactly 0. An offset encoding such as
  0.25 + 0.25·o failed the edge limit (mean 0.05–0.12): libplacebo's
  scaler weights do not sum to exactly 1, so a constant non-zero alpha came
  back close to a code off. libplacebo never sigmoidises, linearises or
  clamps alpha (`pl_shader_sigmoidize` is `color.rgb` only).
- **Order:** both hooks go first in the frame's list (the carry before NLS
  at RGB; the restore before any SCALED hook, the LED measurement, the LED
  compensation and the PRE_OUTPUT texture hooks). The decision is made after
  the LED lookahead's raw transfer (which keeps the true source colour space)
  and before `OutputEncodingRepair::Needed`. The de-judder extras render
  from the same image and hook list.
- **Auto-off (logged once per change):** HDR source or output, a
  calibration LUT, a non-power source or output transfer, SDR source
  primaries other than BT.709 (unless equal to the output's), output
  primaries that do not contain BT.709 (DCI-P3's white point). If
  libplacebo disables either hook, the switch is off until VP restarts.
- **10b** (LED compensation sign-preserving) landed with step 7a
  (`spow`, GPU test LedCompensationKeepsBelowBlackAndLightExact).
- **Tests** (libplacebo 7.360 native and 377 Beta, the same probe
  `src/VideoProcessor-Test/ExtendedRangeGpuProbe.h`):
  - greys 4 16 40 64 100 502 900 940 980 1019, BT.709 → P3, kept exactly at
    1:1, up 1.5 (sigmoid, and ewa_lanczossharp) and down 0.5 (linear light);
    off shows today's clipping (up 1.5: 64 … 940);
  - 2.2 → 2.4 within 1 code of the ideal sign-preserving curve (−11 lands on
    the output floor: 0 on 7.360, 4 on the patched 377);
  - in-range hard edges against off: max 1 code, mean ≤ 0.01 (default
    scaler, with and without a gamma change, both primaries); with
    ewa_lanczossharp mean 0.034 (the polar sampler's four-channel path rounds
    differently), bounded at 0.05;
  - in-gamut BT.709 colours into P3, 2.2 and 2.4 out: identical to off;
  - decision cases and hook order (`ExtendedRangeDecision`).
- **Not covered yet:** NLS with the carry on (NLS warps the picture at RGB
  after the carry, interpolating in the output gamma's encoding instead of
  the source's: a sub-code difference at fine detail, not measured), the
  sigma-delta/LED hooks after the restore (their own tests are unchanged;
  the restore hands them the same values), the projector check.

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
§4b); 24p shows plain 3:2 until stage 6.

## 9. References (reach for these rather than inventing)

- igv `SSimDownscaler.glsl` / `SSimSuperRes.glsl` — use as-is (check licence
  header before any PR).
- Artoriuz CfL (chroma-from-luma) and ArtCNN; igv KrigBilateral — chroma
  upscaling hooks standing in for madVR NGU (no open NGU exists).
- igv FSRCNNX (x2 16-0-4-1), Artoriuz ArtCNN, igv SSimSuperRes — image
  upscaling for 1080p sources, standing in for NGU Sharp.
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
- Real sustainable serial command rate (stage 7). Owner decisions
  2026-10-09 (revised after design review 1): a 200 ms minimum gap (the
  script's proven pace; Barco documents 500 ms), at most 12 levels per
  dimming command, the documented 5 s pause after 20 commands as an option
  (off), read-backs as a toggle (off; switch on once the terminator and
  replies are confirmed on the port, and record the result here) - see
  `VP-led-drive-module.md` §5 and §9.
- Script loop period for tick matching (stage 7).
- RealColor white y 0.340 vs D65 (spectro session, unrelated to this work).
- Differences between madVR HDR profiles 1/2/3 and the rules for colour /
  calibration profile groups 2 and 3 (assumed P3 vs 2020 pair).
- Exact fork keys for saturation (to add) and whether `contrast_recovery`
  maps closely enough to madVR's.
- Upstream PR vs private patch (after stage 3 works).
- Stage 7 (LED drive), from implementation against the real code
  (2026-10-09; detail in `VP-led-drive-module.md` and
  `VP-led-drive-answers-1.md`):
  - The guide's and this brief's up-step table (7/5/4/2/1) did not match
    `beta_22.py`: the script brightens by 1 for distance 10–24 and uses
    APL-dependent steps (+5/+4, +4/+2 split at APL 40). Fixed: the script is
    authoritative; `steps_up_dark` / `steps_up_bright` / `steps_up_split_apl`.
  - Projector contrast (CNTR) is never driven; `mode: contrast` became
    `mode: script`, which emulates the script's contrast bands in VP as an
    encoded-domain gain. Assumption to verify by eye: CNTR acts as a gain on
    the encoded signal with 50 = unity (else try the linear domain).
  - The script's APL weights were effectively 0.114 R / 0.587 G / 0.299 B
    (BGR2GRAY on an RGB screenshot); kept as the default `apl_weights`.
  - Measurement must see the picture before the compensation gain, or the
    loop feeds back. A plain GLSL user shader cannot hand statistics back to
    VP (libplacebo exposes no host access to a user shader's buffers), so the
    measurement pass is a native C++ `pl_hook` at PRE_OUTPUT, ahead of the
    compensation hook. (Verify in renderer.c that the target LUT is applied
    before PRE_OUTPUT.)
  - `mode: script` needs the script's highlight test (max ≥ 215 on an
    854×480 downscale); its mapping to the new 99.5th-percentile statistic is
    unspecified. Implemented as `script_highlight_code: 215` against the
    highlight statistic; tune if the port's highlight behaviour differs.
  - Barco framing: the script sent `:lpw1255cr` plus echo's CR/LF; which
    terminator and spacing the FL35 accepts, the reply format, the `?N`/`?M`
    range queries and the sustainable command rate are all to be confirmed on
    first connection (the driver probes and logs what it finds).
  - `mode: script` reproduces `beta_22.py` tick for tick (tested against a
    literal transcription of the script). Two small, deliberate or inherited
    differences: the configurable LED curve uses the exact 25/30 slope between
    APL 70 and 100, so the target is one level higher than the script's
    rounded 0.8333 at APL 76, 82, 88 and 94; and the guide's "contrast 62 at
    APL 30" is really 61 in the script (0.7333 truncates), jumping to 63 at
    29 - the port follows the script.
  - The script's contrast fast dumps happen inside one loop pass (100 ms
    steps, then 200 ms). The controller returns them as a timed schedule
    within the tick, and the next tick waits until the schedule has run.
  - Verified in libplacebo 7.360.1 renderer.c: tone mapping, gamut mapping
    and a PL_LUT_NORMALIZED/CONVERSION target LUT (VP's calibration LUT is
    normalized) all run in pass_convert_colors, before pass_output_target's
    PL_HOOK_PRE_OUTPUT; values there are the target transfer, 0-1, before
    the limited-range encode. A PL_LUT_NATIVE target LUT would come after.
  - mpv-style shader files spell the stage `PREOUTPUT`, not `PRE_OUTPUT`;
    libplacebo silently accepts an unknown name and the hook never runs. The
    custom hook chain now reports such shaders as not loaded.
  - Step order: the compensation shader (encoded gain only) moved into step
    3, because `mode: script` cannot work without applying its contrast
    gain. Step 4 adds the linear domain and `mode: compensate`.
  - Owner decision: the gamma boost moved into the LED module
    (`gamma_boost: 2.4`, applied always, measured as boosted) and
    `output_gamma` is now 2.2. Check by eye that the picture with the module
    off matches the old output_gamma 2.4 look.
  - `mode: compensate` interpretations of guide 4.2-4.4 (sustained APL for
    the LED target, 100 ms engage / `highlight_release_ms` relax,
    area-blended floor and gain cap, fade gain hold, cut jumps) are listed
    in `VP-led-drive-module.md` section 9 for the planning side to confirm;
    step 5 replaced the cut jumps with ramps and early ramps.
  - `led_latency_ms` (in `[projector.Default]`) defaults to 80 ms until the
    LED step response has been filmed; if it is wrong, a hard cut shows a
    brief flash or dip (acceptance test 2).
  - Step 5 implementation notes (deadband hysteresis, script-mode gain
    delay, early-ramp trigger, raw-path luma weights, serial-rate
    coalescing) are in `VP-led-drive-module.md` section 9.
- Stage 2 addendum (2026-10-09): FSRCNNX_x2_16-0-4-1 (igv, LGPL-3.0),
  ArtCNN_C4F16 (Artoriuz, MIT) and SSimSuperRes (igv, LGPL-3.0) installed as
  disabled `[shader.standard.*]` entries; once enabled each engages itself
  through its own `//!WHEN` rule on live texture sizes (no literals):
  SSimDownscaler when the source is taller than the output, SSimSuperRes
  when shorter. FSRCNNX and ArtCNN upstream require a 1.3x enlargement on
  both axes, which never happens for 1080p 16:9 with the 1.33x lens (~1.48x
  vertical, ~1.11x horizontal); their shipped copies test the height ratio
  only (modification documented in each file header). They hook LUMA, so
  they act on YUV capture only, not RGB input. Enable at most one of the two.
  The profile-group `when` mechanism was not used for this: a `when` on a
  standard-shader member would auto-enable it, not merely gate it.
- Black-bar zoom (brief §8 "automatic crop approximating zoom169/zoom1";
  owner 2026-10-09: similar, but switchable): `[vprenderer.zoom]` profiles
  in the test configuration, cycled with Shift+Z - `black_bars`
  (`automatic_crop`, both madVR profiles' bar removal), `fill` (also
  `crop_narrower_content_to_fill_screen` from 1.7:1, standing in for
  zoom169's "reduce big bars by 50%", which the fork cannot do by halves)
  and `off`; VP starts with the zoom off. Judge on the projector.
- Config app (owner 2026-10-10: switches, not keyboard shortcuts; keep it
  simple): live "Switch on/off in VP" / "Use in VP" buttons on the shader,
  zoom, screen, queue and NLS pages; a Shader settings box built from each
  GLSL shader's own //!PARAM blocks (saved as param_<name>); an LED drive
  page with on/off and five core settings (lowest LED level 60-255, default
  100; lowest level in fades to black, default and minimum 60; mode;
  highlight protection; gamma boost), the projector connection, and an
  optional LED profile file for everything else
  (docs/cinema/LED-PROFILE-GUIDE.md). The built-in profile is the beta_22
  script's curve and tables. The zoom's Shift+Z key was dropped.
- Fade to black below LPW1 60 (owner 2026-10-10): `SHUT` is not usable (no
  mechanical shutter in this FL35; too slow and noisy anyway) and `PMUT`
  only blanks pixels at the current LED level. The custom LED dim mode
  (`LDMM` 1, `LDCR`/`LDCG`/`LDCB`) may go lower but bypasses the automatic
  colour balance; measure on the projector whether it can refine fades to
  black. Owner 2026-10-10: built as an **experimental** option on the LED
  drive page (off by default, options greyed until on): `ftb_custom_dim`
  together | turns, `ftb_step_ms` 100 (the contrast-dump pace),
  `ftb_custom_start` (custom value matching LPW1 60, to be measured),
  `ftb_custom_floor`. Separate per-colour commands can tint the black
  briefly; "turns" keeps the colours within one step. Owner test
  2026-10-10: the custom values run 0-40 per colour, 40 is about 40/255
  (below LPW1's 60) and 0 turns that LED off; defaults are now start 40,
  floor 0 (true black), ranges 1-40 and 0-39. Low_Latency now sets `active_picture_lookahead_frames: 0`
  explicitly (it inherited Normal's 8); the LED drive follows the queue it
  is given and is reactive there.
- Reactive vs look-ahead (owner 2026-10-10): the LED drive follows the
  queue it is given. `led_floor_reactive` (default 100, the script's floor;
  on the LED drive page) applies whenever the look-ahead does not reach the
  frame the LED will land on; `led_floor` applies when it does. A cut the
  look-ahead has seen is one jump timed to land on the cut frame
  (`cut_jump`, both directions); an unseen dark-to-bright cut goes straight
  up in one command like the script's contrast dump (`bright_cut_dump`) -
  this supersedes delta-1 answer 3's "unseen cuts are rate-limited" for the
  brightening case. Both default on, in the LED profile. The experimental
  custom-dim fade to black engages only with look-ahead and leaves with a
  single `LDMM 0` as soon as light is seen in the queue (assumes LDMM 0
  returns to LPW1 at once - to be checked on the projector).
- Light-meter data (owner 2026-10-10, original light engine; table and fit
  in `docs/reference/LED-power-vs-contrast.md`): equal white at LED L and
  contrast C fits `L = 255 x (50/C)^2.41` (rms 1 level). Confirms CNTR as an
  encoded-domain gain with 50 = unity (script mode) and flux proportional
  to LPW1 on the original engine. Owner: the projector fits its LED drive
  at every startup, so the non-stock engine tracks LPW1 too - linear flux
  stays the default and a flux_table is optional (supersedes answers-2 B1's
  "do not assume linear"). The table implies a gamma of ~2.4 at the time,
  but the projector has its own gamma settings and reports 2.2 at the one
  in use; owner keeps `compensation_eotf` 2.2 (if it were really 2.4,
  compensation would be ~9% too bright at LED 100). Compensate mode applies
  the exact equal-brightness compensation at every LED level down to the
  floor - no fall-off like the script's contrast limit; the highlight
  protection is what limits it near white.
- Queue auto-select: `[queue.Low_Latency] when: ${source_rate} > 59` in the
  test configuration (syntax per CONFIGURATION.html; tested: 59.94 fps
  selects Low_Latency, 23.976 fps returns to Normal); Shift+L still
  switches by hand.

- Lookahead depth (delta-1 answers 4): `active_picture_lookahead_frames: 8`
  in `[queue.Normal]` only. Until Stage 5 the queue itself holds only
  `target_frames` (3) frames ahead, so the effective lookahead is ~3 frames
  regardless of the 8-frame cap and early ramps are partial.
- Gamma (delta-1 answers 7): `gamma_boost` is not only a read of the
  profile's `output_gamma`. Per the owner's decision above, the module
  applies the boost itself at the PRE_OUTPUT hook and `output_gamma` is 2.2;
  the raw-frame transfer includes both the profile's `output_gamma` and the
  module's `gamma_boost`, so it matches what the exact measurement sees.
