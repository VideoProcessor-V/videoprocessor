# Build status

What this build contains, how each part is switched, what to check first
at the projector, and how to get back to Bill's VP. Written for the first
FL35 deployment (cut plan: docs/VP-cut-plan.md). Settings are rendering
profile keys (`[vprenderer.*]` sections) unless a section is named.

"Default" is what VP does when the key is absent. Every feature built in
this fork is **off by default**: with no new keys, VP renders as Bill's VP
(apart from the always-on parts marked "always", which change no picture).

## The stages

| Stage | What it does | Key(s) | Default | FL35 test config, first session | First thing to check | Tests |
|---|---|---|---|---|---|---|
| 2-4 Temporal sigma-delta | 10-bit accurate dither on the 8-bit link; carries the sub-code residual along the motion | Standard shader `Temporal sigma-delta.glsl` (`param_enabled`, `param_motion_threshold` 1, `param_noise_amplitude` 0.15, `param_match_tolerance` 1.5) | shader not enabled | on, motion carry on | Ramp: banded / grainy / smooth; flat greys: flicker? | `SigmaDelta*`, `OcclusionMask*` |
| 6 De-judder | Fills 24p's repeated refreshes with pictures moved by `s` x the motion | `dejudder_strength` (0 = off; Gentle 0.25, Medium 0.35), `dejudder_match_tolerance` 3, `dejudder_frame_trust` 0.25 | 0 (off) | 0.25 | Slow pan over detail: judder gone? shimmer? held patches? | `Dejudder*` |
| 6c Occlusion mask | Tells the sigma-delta where content is new, so no residual is carried into it | `occlusion_mask` | off | on | Nothing visible by itself; with the dither on, no smear at edges of moving objects | `OcclusionMask*` |
| 6d Rendered warp | De-judder extras warp the finished picture (fast) instead of re-rendering | `dejudder_warp_stage` rendered / source | rendered | rendered | Per-present render time in the stats overlay (Ctrl+I) | `DejudderRenderedWarpMatchesTheSourceWarp` |
| D1 Global-motion prior | Whole-picture motion first; every block starts its search there (fast pans) | `dejudder_global_prior` | on* | on* | Fast pans followed (no holds on a whip pan) | `DejudderGlobalPriorFindsAPanBeyondTheCoarseRange` |
| D2 Motion-scaled strength | Strength ramps from 0 below `lo` to full at `hi` (source px per film frame) | `dejudder_motion_scale_lo` 0.25, `dejudder_motion_scale_hi` 1.5 (0 = off) | on* | on* | Still scenes untouched; slow pans: lower `hi` if they still hitch | `DejudderMotionScaledStrength` |
| D3 Vector smoothing | Isolated wrong vectors take their neighbours' median; held blocks move with agreeing neighbours when their own picture matches | `dejudder_vector_smooth` | on* | on* | Fewer held patches on pans; a lone small object moving against a pan may be moved with the pan | `DejudderVectorSmoothing` |
| D4 Trust-adaptive strength | Fewer trusted blocks than `trust_scale` eases strength down instead of holding more | `dejudder_trust_scale` 0.6 (0 = off) | on* | on* | Busy scenes: smooth retreat, not patchy judder | `DejudderTrustAdaptiveStrengthAndRateLimit` |
| D2/D4 rate limit | The strength scale changes by at most 0.25 per film frame; a cut holds at once | (fixed) | - | - | No pumping at a pan's start or end | same |
| D5 Occlusion fill | Experiment: uncovered picture is taken from the next frame instead of held. **Source warp only** (`dejudder_warp_stage: source`); with `rendered` it is ignored and logged once (planner: B-early caching is a post-cut item) | `dejudder_occlusion_fill` | off | off | Only as an A/B with `source` | `DejudderOcclusionFillFromB` |
| 8 Adaptive downscale | 4K to 1600: SSIM where there is edge detail, faithful where there is grain | `adaptive_downscale`, `adaptive_w_base` 1, `adaptive_edge_lo/hi` 8/40, `adaptive_grain_lo/hi` 2/8, `adaptive_grain_floor` 0.4, `adaptive_chroma_ssim` off, `downscale_light` linear | off | on | Grainy sky over a sharp foreground: grain calmer, no halos on horizons or text | `AdaptiveDownscaleBlendsEdgesTextureAndGrain` |
| 8-3 Grain map | The grain measure per 16x16 block from the motion vectors (the matcher runs for it) | `adaptive_grain_map`, `adaptive_grain_map_smooth_ms` 500 | on (with Stage 8) | on | As Stage 8 | `AdaptiveDownscaleGrainMapFromTheVectors`, `DejudderGrainErrorSeparatesNoiseFromTexture` |
| 8b Adaptive upscale | 1080p and below to 1600: where the grain measure sees grain, luma leans from the CNN upscaler (FSRCNNX / ArtCNN) toward a faithful ewa_lanczossharp upscale; no edge term; SSimSuperRes's refinement scaled by the same grain term. Faithful branch from the source picture, or a doubler's exact 2x averaged back to source resolution | `adaptive_upscale`, `adaptive_upscale_w_base` 1, `adaptive_upscale_superres_scale` on, `upscale_light` linear; grain keys shared with Stage 8 | off | listed, off (the cinema check turns it on) | 1080p grainy sky over a sharp foreground: CNN alone vs blend: grain calmer, edges unchanged, no shimmer; if not better, leave off | `AdaptiveUpscaleBlendsCnnAndFaithfulByGrain`, matrix refinements axis |
| S1 Grain-aware dither carry | Where the grain map sees grain, the sigma-delta's block match and per-pixel gate are this many times more tolerant, so its 10-bit average survives grain | `dither_grain_tolerance_scale` 3 (1 = off) | on* | on* | Grainy flat areas (night sky): smooth, not dithered-noisy | `SigmaDeltaGrainAwareCarry` |
| 10 Extended range | BTB / WTW kept through scaling and colour conversion (SDR) | `preserve_extended_range` | off | on | 235-255 ramp continues above white; PLUGE 4/12/16/20 (record either way) | `ExtendedRange*` |
| S2 Float32 output pass | libplacebo's output pass (PRE_OUTPUT to the dither) in rgba32f: no half-float rounding before the encode, nothing clipped (patch 0003, Beta libplacebo). A correctness fix: it removed a 0.25-0.6 code darkening | always (VP Renderer, Beta libplacebo) | always | always | Nothing to see | `Beta S2`, `Beta S2 A/B` (the bias returns with the flag off) |
| S2 Float32 everywhere | Every libplacebo intermediate in rgba32f: exact highlights (half-float rounds above white by up to 0.84 of a 10-bit step) | `fbo_float32` auto / on / off | auto (on; off with the Low_Latency queue) | auto | Render time in the stats overlay (Ctrl+I) on the 3090; turn off if it shows pressure | `Beta S2` |
| HDR deband off | libplacebo's debanding off for PQ/HLG (its HDR grain turns ~1 pixel in 10^4 black) | `[vprenderer.HDR]` `when: $eotf=="pq" \|\| $eotf=="hlg"`, `deband: off` | in the shipped sample | on | HDR film: no black specks in bright areas | `ShippedConfigurationsTurnDebandingOffForHdr` |
| LED drive | Projector LED / CNTR control from the picture over RS-232 | `[led_drive.Default]` `enabled`, `mode` off / log_only / script / compensate; `[projector.Default]` port, baud | disabled | `log_only` for the first minutes, then `compensate` | Log first; then fade to black, hard cut dark to bright, static mid-APL, 5-10% grey | `Led*`, `ReplyParserAcceptsOnlyBarcoReplies` and the RS-232 connection test |
| 9 Room lights | Dark / lit room from a lux sensor on its own COM port; profiles select on `${ambient}` | `[ambient]` `mode` off / sensor, `port`, `baud` | off | off (sensor hardware not chosen) | - | `AmbientRoomLightsSwitchAndSelectProfiles`, `AmbientSensorReadsLuxLinesFromItsOwnPort` |
| Edge blanking | Light-engine edges blanked; picture scaled into the rest | `[vprenderer.viewport.cinema]` `blank_left` 10, `blank_right` 15 | none | 10 / 15 | Edges black; picture present | chain tests (blanking phase) |

\* "on" defaults of the refinements act only when the de-judder (`dejudder_strength` > 0) or the grain map is on; with those off they do nothing.

## Getting back to Bill's VP

Per feature, set the key shown; all of these together give Bill's VP's
picture (the rest of the fork changes no picture when its keys are absent):

```
# rendering profile (e.g. [vprenderer.input.Cinema])
dejudder_strength: 0
occlusion_mask: off
adaptive_downscale: off
preserve_extended_range: off
dither_grain_tolerance_scale: 1
adaptive_upscale: off
fbo_float32: off
# Standard shaders: Temporal sigma-delta disabled
# [led_drive.Default]
enabled: false
# [ambient]
mode: off
```

The refinements individually (for A/B with the de-judder on):
`dejudder_global_prior: off`, `dejudder_vector_smooth: off`,
`dejudder_motion_scale_hi: 0`, `dejudder_trust_scale: 0`,
`dejudder_occlusion_fill: off`, `dither_grain_tolerance_scale: 1`.

The output-pass float32 has no key (a correctness fix): the only way back
is the previous `libplacebo-377.dll`.

## Known, measured biases (not defects of this build)

- **7.360 (stable libplacebo):** its output-encoding repair re-encodes
  about 0.42 code low on average (at most 1 code), the same on WARP and the
  GeForce. Its chain-test limit stays at mean 0.8.
- **377 (Beta, deployed):** clamps limited-range output to codes 4..1019 by
  design (patch 0002: keeps footroom and headroom).
- **Before S2**, every Stage-10-off chain on both libplacebos was 0.25-0.6
  code low: the half-float round trip at the PRE_OUTPUT texture hooks (the
  LED hook). S2 removes it on the Beta; 7.360 keeps it.
- **Highlights:** the main pass's half-float FBOs still round values above
  white to about 0.84 of a 10-bit step (S2's test). Making every FBO
  float32 (`fbo_float32`, built, not set) removes that.
- **Cost of float32 FBOs** (GeForce RTX 3050 Ti Laptop, 4K to 2560x1600,
  Mitchell in linear light plus one PRE_OUTPUT texture hook; Beta suite
  with `VP_TEST_TIMING=1 VP_TEST_GPU=hardware`, 2026-10-12): normal FBOs
  3.89 ms per frame, output pass rgba32f (S2 as built) 4.61 ms (+0.73 ms),
  every FBO rgba32f 6.69 ms (+2.8 ms). The full VP chain costs more in
  absolute terms; the differences are the bandwidth. The laptop GPU has
  roughly a third to a quarter of the cinema RTX 3090's throughput: read
  the 3090's figures from the stats line in the first cinema session.
- Highlight precision, codes 900-1019 (Beta S2 test): normal FBOs 1.02 of
  a 10-bit step, output pass rgba32f 0.84, every FBO rgba32f 0.
- **HDR debanding** (libplacebo bug): black specks; hence deband off for HDR.

## Known behaviours

- D3: a small object (about one block) moving against a pan is an isolated
  outlier by definition; when its own match at the neighbours' median
  passes the tolerance it is moved with the pan. At `s` 0.25 its motion is
  smoothed by a quarter; judge it on the first pan.
- S1 scales both of the sigma-delta's gates (the block match and the
  per-pixel motion gate) by the grain term: with the per-pixel gate alone at
  one code, grain would reset the carry anyway.
- D5 is source-warp only (ignored with the rendered warp, logged once).
- 8b's faithful branch averages a doubler's 2x output back to the source
  resolution (prescale ratios other than 1 and 2: 8b skips the frame and
  logs it).
- 8b `upscale_light`: linear = libplacebo's linear light where it scales
  down (2x to 1.48x after a doubler), sigmoid off; without a CNN the plain
  upscale is in gamma light. gamma = gamma light everywhere (madVR's habit).
  Both branches always share the domain. First A/B: the two settings.
- Stage 8 and 8b and the two de-judder warp paths: both stages run in the
  main render, so the source warp's extras re-run them and the rendered warp
  warps their result; nothing is path-specific (no separate tests).

## Build notes

Things that look like build problems but are not.

### Windows Application Control blocks a freshly linked test binary

On the development laptop, Windows Application Control (Smart App Control)
intermittently refuses to start a just-linked test binary:

- `VideoProcessorBetaTests.exe`: bash reports "Permission denied" (exit 126);
  cmd says the command "is not recognized";
- `VideoProcessor-Test.dll`: vstest reports "Failed to load the test assembly
  or its dependencies" for every test;
- `VideoProcessorConfigTests.exe`: sometimes the same.

The build is fine. Relink the project (delete the output and build it, or
`/t:<project>:Rebuild`); if it is still blocked, wait and retry later - on
2026-10-12 the Beta exe ran again after a while with nothing changed. Never
change the Smart App Control / Application Control setting (standing rule).

### Stale incremental-link files (LNK1103 / LNK1318)

`ProcessCpuUsageWindowTests.obj : fatal error LNK1103: debugging information
corrupt` or LNK1318 on an incremental build: delete
`src/VideoProcessor-Test/x64/Release/ProcessCpuUsageWindowTests.obj` and the
`*.iobj` / `*.ipdb` there, then build again.

### Shader-compiler timing notes

libplacebo logs "Spent N ms translating HLSL to DXBC (slow!)" (or "GLSL to SPIR-V") when a shader
compiles slowly (WARP, a loaded machine). Not a failure; the GPU tests'
clean-log checks ignore it. VP keeps libplacebo's compiled-shader cache.

### The Beta libplacebo (377) is VP's own build

`3rdparty/libplacebo-beta/bin/libplacebo-377.dll` is Bill's Beta source
with three VP patches (`source/fl35-patches`): 0001/0002 the limited-range
output clamp, 0003 the optional rgba32f FBOs. Rebuild with MSYS2 MINGW64:
`C:\dev\libplacebo-build\build.sh` (it runs libplacebo's own tests), then
copy the DLL and `include/libplacebo/renderer.h`.
