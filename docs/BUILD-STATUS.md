# Build status

What this build contains, how each part is switched, what to check first
at the projector, and how to get back to Bill's VP. Written for the first
FL35 deployment (cut plan: docs/VP-cut-plan.md). Settings are rendering
profile keys (`[vprenderer.*]` sections) unless a section is named.

"Default" is what VP does when the key is absent. Every feature built in
this fork is **off by default**: with no new keys, VP renders as Bill's VP
(apart from the always-on parts marked "always", which change no picture),
with one exception agreed by the owner (2026-10-13): the chroma upscaler
defaults to Catmull-Rom (`chroma_upscaler: upscaler` gives Bill's).

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
| 11a E1 Candidate sweep | A second matcher sweep that also tries the neighbours' vectors, with a smoothness penalty, so a block follows its neighbours unless its own match is clearly better | `dejudder_est_candidates`, `dejudder_est_lambda` 0.5 | on* | on* | Fewer odd single blocks on pans | `EstimatorNeighbourCandidatesBeatATextureOutlier` |
| 11a E3 Quarter-pel step | Trusted blocks refine their vector to a quarter source pixel | `dejudder_est_qpel` | on* | on* | Slow pans: no stepping | `EstimatorQuarterPelRefinesASubPixelPan` |
| 11a E4 Static gate | Blocks that did not change skip the search (GPU-side) | `dejudder_est_gate` | on* | on* | Nothing visible; less matcher time on still scenes | `EstimatorGateKeepsTheFieldOnStillScenes` |
| 11a E2 Block split | A block that matches badly is split into 8x8-source-pixel sub-blocks with their own vectors (a sub-field read by both warps, the occlusion mask and the grain map) | `dejudder_est_split`, `dejudder_est_split_sad` 0 (= the match tolerance) | on* | on* | Object edges on pans: fewer held squares | `EstimatorSplitFollowsASmallObject`, `EstimatorSplitOffStateLeavesEveryConsumerAlone` |
| Eff Matcher size (4K) | Owner 2026-10-13 efficiency: above 1920 wide the matcher runs one level coarser (a half-size copy) and its field, sub-field and trust are scaled back; the dither's occlusion mask and the grain map stay on the 16-pixel grid. On the owner's 4K clip: 108% of the full-size de-judder benefit (the coarser copy averages grain away), matcher + warp about 19 ms instead of 37-44 ms on an RTX 3050 Ti, the dither's error unchanged (0.0435 vs 0.0431 of a code). `search`: only the searches coarse, refinements at full size (95%) | `dejudder_low_res` all / search / off | all* | all* | Pans as smooth as before; matcher time | `Beta de-judder quality`, `Beta dither compare` (reports), `DejudderSettingsAreInputProfileNumbersWithLimits` |
| Eff Estimate ahead | Owner 2026-10-13: when the frame after next is queued and converted (a queue target of 3 frames or more), the next pair's motion is estimated after the first de-judder extra, in that extra's spare refresh, so the next frame's own refresh holds only its render; the remaining extras warp with a copy of the current field. Without that frame, or with a different format, a replaced queue entry, the source warp, or the key off, the estimate runs before the frame as before | `dejudder_estimate_ahead` | on | on | Stats line: "ahead N%" and the last reason when not; the log every 5 s: frames per path and per reason | `DejudderEstimateAheadMatchesTheSequentialPath` (paths and byte-identical outputs, C withheld and replaced), `DejudderEstimateAheadFallsBackForEachReason`, `DejudderSnapshotKeepsTheWarpInputsWhileTheNextPairIsEstimated` |
| Eff Extras with moved subtitles | Owner 2026-10-13: frames whose subtitle is moved (cut/paste) keep their de-judder extras on the rendered warp; the subtitle's rectangles (where it was, its fill, where it is shown, its card; 2 px margin) hold the frame's own pixels and nothing samples from them; the dither's vectors are zero there. The source warp still holds such frames | (with `dejudder_warp_stage: rendered`) | always | always | Pan under a moved subtitle: smooth picture, steady text | `DejudderRenderedWarpHoldsAMovedSubtitleStill` |
| 14 R-A Fast motion keeps its strobing | Owner 2026-10-13 (docs/VP-delta-14-soe-rules.md): the strength rises with motion as D2 and falls above a fast pan, from 1 at `roll_lo` to `roll_floor` at `roll_hi` (the frame's camera speed from D1, else the trusted mean; D2's rate limit and cut hold) | `dejudder_motion_rolloff`, `dejudder_roll_lo` 8, `dejudder_roll_hi` 24, `dejudder_roll_floor` 0.25 | off | listed, off (the R-A/R-B check by eye) | Brisk pan smooth; a whip pan or fast action keeps film's strobing | `DejudderMotionRolloffKeepsFastMotionsStrobing` |
| 14 R-B Camera/object split | The camera's motion smoothed fully, motion within the frame by `object_strength` (both warps and the dither's vectors; the mask and the grain map read the true field). The camera is D1's global vector refined to the mean of the trusted blocks near it (D1 alone is in 4-px steps); without D1 the whole vector is scaled | `dejudder_object_strength` 1 (off) / 0.5 | 1 (off) | listed, off (try 0.5) | People walking across a pan: the pan smooth, the walk less like video | `DejudderObjectStrengthSplitsTheCameraFromObjects` (both warp paths), matrix axis (delta 14) |
| 9 H1 Feathered hold | A warped block blends into a held neighbour over this many source pixels instead of a hard edge | `dejudder_hold_feather_px` 4 (0 = off) | on* | on* | Edges of held patches soft, not blocky | `HoldFeatherSoftensTheHeldBoundary` (both warp paths) |
| 9 H2 Hold hysteresis | A block must be trusted for N source frames running before it warps, and untrusted for N before it holds (no flicker between the two). The first frame after a start or a cut holds | `dejudder_hold_hysteresis` 2 (1 = off) | on* | on* | No blocks blinking between moving and held | `HoldHysteresisNeedsTwoFrames` |
| 9 H3 Block taper | Experiment: marginal blocks move at reduced strength instead of holding | `dejudder_block_taper`, `dejudder_taper_sad` 0 | off | off | Only as an A/B | `BlockTaperWarpsMarginalBlocksAtARampedStrength` |
| 10 Even-cadence strength | De-judder on 2:2 cadences (30 / 25 fps film-like): one extra at the exact midpoint | `dejudder_strength_even` (0 = off), `dejudder_even_rates` 30 \| 30_and_29.97 | 0 (off) | off (judge 24p first) | 30p pan: smoother, no doubled edges | `DejudderEvenCadenceStrength` |
| R1 Dither on the de-judder's vectors | On de-judder extras the sigma-delta takes the motion since the previous present from the de-judder's field (exact there) and skips its own search (Beta libplacebo, fork patch 0004); every other refresh, and 7.360, search as before | `dither_vectors` auto / own | auto* | auto* | Nothing new to see; per-present time in the stats overlay | `Beta R1`, `DitherVectorsAutoFallsBackWithoutThePatch` |
| 13 SDR tone shape | SDR only: reference white placed below peak (headroom) and a soft knee into it, the shape tone-mapped HDR has, no inverse tone map; contrast recovery returns the detail the knee flattens | `sdr_tone_shape` off / knee, `sdr_white_headroom` 0, `sdr_knee_start` 0.80, `sdr_knee_strength` 0.5, `sdr_contrast_recovery` auto | off | off (the cinema check: headroom, then + knee, then + recovery, against the HDR version) | 200-255 ramp: smooth into white, no clip at 235 | `SdrToneShapeReferencePlacesWhiteAndRollsOffIntoTheHeadroom`, `SdrToneShapeShaderMatchesItsReference` |
| D5 Occlusion fill | Experiment: uncovered picture is taken from the next frame instead of held. **Source warp only** (`dejudder_warp_stage: source`); with `rendered` it is ignored and logged once (planner: B-early caching is a post-cut item) | `dejudder_occlusion_fill` | off | off | Only as an A/B with `source` | `DejudderOcclusionFillFromB` |
| 8 Adaptive downscale | 4K to 1600: SSIM where there is edge detail, faithful where there is grain | `adaptive_downscale`, `adaptive_w_base` 1, `adaptive_edge_lo/hi` 8/40, `adaptive_grain_lo/hi` 2/8, `adaptive_grain_floor` 0.4, `adaptive_chroma_ssim` off, `downscale_light` linear | off | on | Grainy sky over a sharp foreground: grain calmer, no halos on horizons or text | `AdaptiveDownscaleBlendsEdgesTextureAndGrain` |
| 8-3 Grain map | The grain measure per 16x16 block from the motion vectors (the matcher runs for it) | `adaptive_grain_map`, `adaptive_grain_map_smooth_ms` 500 | on (with Stage 8) | on | As Stage 8 | `AdaptiveDownscaleGrainMapFromTheVectors`, `DejudderGrainErrorSeparatesNoiseFromTexture` |
| Eff Chroma upscaler | The colour planes' upscale (4:2:0 / 4:2:2) before the main scaling. Owner 2026-10-13: Catmull-Rom looked identical to the polar filter on 4K footage (mean 0.33 of 1023 codes) at about a seventh of the cost (about 4.5 ms per refresh on the laptop) | scaling profile `chroma_upscaler` auto (Catmull-Rom) / upscaler (Bill's: the main upscaler) / ewa_lanczossharp / lanczos / catmull_rom / bicubic / bilinear | auto | auto | Coloured edges and text: no fringing | `ChromaUpscalerIsAScalingChoice` |
| Eff Faithful branch | The adaptive downscale's faithful branch: polar (today) or ortho (about 2.4 ms cheaper, keeps 82% of the blend's effect, so not the default) | `adaptive_faithful` polar / ortho | polar | polar | As Stage 8 | `Beta option quality` (report) |
| 8b Adaptive upscale | 1080p and below to 1600: where the grain measure sees grain, luma leans from the CNN upscaler (FSRCNNX / ArtCNN) toward a faithful ewa_lanczossharp upscale; no edge term; SSimSuperRes's refinement scaled by the same grain term. Faithful branch from the source picture, or a doubler's exact 2x averaged back to source resolution | `adaptive_upscale`, `adaptive_upscale_w_base` 1, `adaptive_upscale_superres_scale` on, `upscale_light` linear; grain keys shared with Stage 8 | off | listed, off (the cinema check turns it on) | 1080p grainy sky over a sharp foreground: CNN alone vs blend: grain calmer, edges unchanged, no shimmer; if not better, leave off | `AdaptiveUpscaleBlendsCnnAndFaithfulByGrain`, matrix refinements axis |
| S1 Grain-aware dither carry | Where the grain map sees grain, the sigma-delta's block match and per-pixel gate are this many times more tolerant, so its 10-bit average survives grain | `dither_grain_tolerance_scale` 3 (1 = off) | on* | on* | Grainy flat areas (night sky): smooth, not dithered-noisy | `SigmaDeltaGrainAwareCarry` |
| 10 Extended range | BTB / WTW kept through scaling and colour conversion (SDR) | `preserve_extended_range` | off | on | 235-255 ramp continues above white; PLUGE 4/12/16/20 (record either way) | `ExtendedRange*` |
| Eff Parallel searches | The de-judder block search, second sweep and quarter-pel, the global motion and the sigma-delta's motion search run one work group per block / candidate: byte-identical results (the global motion's match ratio up to float rounding; the same vector); the sigma-delta skips its search on cadence repeats | always | always | always | Nothing to see; matcher and dither time | `Beta de-judder: parallel block search byte-identical`, `Beta sigma-delta: parallel motion search byte-identical` |
| S2 Float32 output pass | libplacebo's output pass (PRE_OUTPUT to the dither) in rgba32f: no half-float rounding before the encode, nothing clipped (patch 0003, Beta libplacebo). A correctness fix: it removed a 0.25-0.6 code darkening | always (VP Renderer, Beta libplacebo) | always | always | Nothing to see | `Beta S2`, `Beta S2 A/B` (the bias returns with the flag off) |
| S2 Float32 everywhere | Every libplacebo intermediate in rgba32f: exact highlights (half-float rounds above white by up to 0.84 of a 10-bit step) | `fbo_float32` auto / on / off | auto (on; off with the Low_Latency queue) | auto | Render time in the stats overlay (Ctrl+I) on the 3090; turn off if it shows pressure | `Beta S2` |
| HDR deband off | libplacebo's debanding off for PQ/HLG (its HDR grain turns ~1 pixel in 10^4 black) | `[vprenderer.HDR]` `when: $eotf=="pq" \|\| $eotf=="hlg"`, `deband: off` | in the shipped sample | on | HDR film: no black specks in bright areas | `ShippedConfigurationsTurnDebandingOffForHdr` |
| LED drive | Projector LED / CNTR control from the picture over RS-232 | `[led_drive.Default]` `enabled`, `mode` off / log_only / script / compensate / picture_only / listen; `[projector.Default]` port, baud | disabled | `log_only` for the first minutes, then `compensate` | Log first; then fade to black, hard cut dark to bright, static mid-APL, 5-10% grey | `Led*`, `ReplyParserAcceptsOnlyBarcoReplies` and the RS-232 connection test |
| LED R2 Grain-suppressed highlights | A grainy block's highlight statistic leans toward its neighbours' mean, so grain does not read as highlights (APL and the script's block maximum unchanged) | `led_highlight_grain_suppress` | on (with the LED drive) | on | Grainy bright sky: LED not pumping on grain | `LedHighlightStatisticIgnoresGrain` |
| LED delta 11 Boost toe, ANSI lift | The gamma boost fades out below `gamma_boost_knee` (shadows keep their gradation); the ANSI branch lifts the darks next to large highlights by `shadow_lift_ansi` (scene-adaptive, slewed at `shadow_lift_ansi_rate`, reset on a cut) | `gamma_boost_knee` 0, `gamma_boost_knee_width` 0, `shadow_lift_ansi` 0, `shadow_lift_ansi_ramp_lo/hi` 0.01/0.10, `shadow_lift_ansi_rate` 1 per s | off | off (the dark-hair test) | Dark hair against a bright window: texture kept | `LedBoostToeAndShadowLift` |
| LED picture_only | VP's picture compensation only, gain exactly 1: statistics, veil and shadow lift as in compensate; **the RS-232 port is never opened** (the original Python script can use it) | `mode: picture_only` | - | - | Port free: the script connects | `PictureOnlyNeverOpensThePortAndKeepsGainOne` |
| LED listen | The original script (`docs/reference/main_vp.py`: main.py plus a UDP addendum) drives COM1 and tells VP each command; VP eases each heard contrast step as if it had sent it, compares its own copy of the script's rules, and never opens the port. Config LED page: the listen monitor (heard commands, VP's measurement, agreement) | `mode: listen`, `listen_port` 47811, `listen_log` off (on: `logs\led-listen.csv`, every message with VP's measurement); `listen_script` (VP starts the script itself: no window, restarted after 2, 4 ... 60 s, ended with the mode or VP, output in `logs\led-script.log`; empty = you start it), `python` (empty = python on the PATH) | - | - | Monitor shows "Receiving"; agreement figures; "VP runs the script (pid N)" | `ListenModeFollowsTheScriptWithoutThePort`, `ListenLogOffWritesNoCsv`, `ScriptHostRunsWatchesRestartsAndStopsTheScript` |
| 9 Room lights | Dark / lit room from a lux sensor on its own COM port; profiles select on `${ambient}` | `[ambient]` `mode` off / sensor, `port`, `baud` | off | off (sensor hardware not chosen) | - | `AmbientRoomLightsSwitchAndSelectProfiles`, `AmbientSensorReadsLuxLinesFromItsOwnPort` |
| Low-latency capture | Owner 2026-10-13 (docs/VP-gpu-capture-low-latency-plan.md): DeckLink V210 / UYVY / HDYC handed to the renderer raw and unpacked on the GPU into the same P210 planes the CPU formatters make (sample-exact); the analysis reads the raw frame; any other format, a conversion override or a failed unpack stays on / returns to the CPU formatters (logged once). 4K on the laptop RTX 3050 Ti: V210 8.1 -> 3.7 ms, UYVY 7.6 -> 2.8 ms per frame (CPU conversion + upload vs raw upload + a 0.3 ms pass). Meant for a low-latency input profile chosen by a rule (`when: $source_rate > 59`); cinema keeps standard. The de-judder works from GPU-unpacked frames (checked at 23.976p with fake frames); its extras / estimate-ahead with them need the projector to confirm. Not yet: r210 / R12L, pinned upload buffers | `capture_path` standard / gpu (input profile) | standard | standard (a `[vprenderer.input.low_latency]` with `when: $source_rate > 59` and `capture_path: gpu` for games/TV) | Log: "Capture path gpu: V210 unpacked on the GPU"; the `libplacebo initialized ... upload` line and `conversion_ms` / `source_upload_cpu_ms` in the frame pacing trace, standard vs gpu | `GpuCaptureUnpackMatchesTheCpuFormatters`, `CapturePathRunsThroughTheRendererWithFakeDeckLinkFrames` (hardware; fake DeckLink frames through the shipped renderer DLL; forced fallback) |
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
sdr_tone_shape: off
sdr_white_headroom: 0
dejudder_low_res: off
# scaling profile (e.g. [vprenderer.scaling.cinema])
chroma_upscaler: upscaler
# Standard shaders: Temporal sigma-delta disabled
# [led_drive.Default]
enabled: false
# [ambient]
mode: off
```

The refinements individually (for A/B with the de-judder on):
`dejudder_global_prior: off`, `dejudder_vector_smooth: off`,
`dejudder_motion_scale_hi: 0`, `dejudder_trust_scale: 0`,
`dejudder_occlusion_fill: off`, `dither_grain_tolerance_scale: 1`,
`dejudder_est_candidates: off`, `dejudder_est_qpel: off`,
`dejudder_est_gate: off`, `dejudder_est_split: off`,
`dejudder_hold_feather_px: 0`, `dejudder_hold_hysteresis: 1`,
`dither_vectors: own`.

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
- H2: with `dejudder_hold_hysteresis` 2 the first source frame after a
  start or a cut holds every block (none has been trusted twice yet); the
  extras start on the next frame. Intended (the spec's N consecutive
  frames).
- The refinement round improved the synthesis (Beta 6d check, (12, 6) pan,
  extra at s 0.5 against the true halfway picture, mean code difference):
  9.68 with every refinement on (D1-D4, E1-E4, H1-H2) vs 13.33 without; 12.52
  before delta 9.
- R1's cost is measured on the stats line (Ctrl+I, de-judder line): the
  sigma-delta's GPU time per extra, "dither X ms (de-judder vectors)" or
  "(own search)"; switch `dither_vectors` to compare.
- Sigma-delta state tags are per hook instance (`instance_tag`, set by the hook
  chain on creation): a new hook never trusts a freed hook's leftover state (the
  2026-10-13 GeForce finding). libplacebo offers no way to clear an mpv
  shader's //!STORAGE textures, so the tag is the whole fix.
- R1 gives the sigma-delta external vectors only on de-judder extras (the
  motion since the previous present is the shift step there, exactly); the
  first present of each source frame, cadence repeats and every frame
  without extras search as before. A held block is a trusted zero vector.
  The external vectors are whole pixels (the quantizer's), from each 16x16
  block's centre; the per-pixel gate checks every pixel as before.
- 13: the SDR tone shape runs first at PRE_OUTPUT, so the LED measurement
  sees the shaped picture (what is shown). Its curve is per channel, in the
  output gamma; the knee is in linear light, as a fraction of peak.
- picture_only: switching from compensate rebuilds the module (the port
  closes; full light is restored by the rebuild, not by a command).
- Listen: the agreement figures compare the script's own measurement
  (screenshot, mean2/max) with VP's (the picture before the encode); a
  steady offset is expected (different picture, different scaling).
  VP's monitor file `logs\led-listen-now.txt` is written whether or not
  `listen_log` is on.
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
with four VP patches (`source/fl35-patches`): 0001/0002 the limited-range
output clamp, 0003 the optional rgba32f FBOs, 0004
`pl_mpv_user_shader_set_texture` (R1: a user shader's own `//!TEXTURE`
pointed at a VP texture, restored before destroy; one new export). Any
libplacebo rebuild must carry all four. Rebuild with MSYS2 MINGW64:
`C:\dev\libplacebo-build\build.sh` (it runs libplacebo's own tests), then
copy the DLL, `include/libplacebo/renderer.h` and
`include/libplacebo/shaders/custom.h`, add any new export to
`lib/libplacebo-377.def` and regenerate the import library
(`lib.exe /def:libplacebo-377.def /machine:x64 /out:libplacebo-377.lib`).
