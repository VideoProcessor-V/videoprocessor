# Per-frame efficiency audit (2026-10-24)

Read-only audit of `feat/skeleton` **d0212534**. All line numbers refer to that commit. Nothing was
built, run or profiled: timing gates were running on this machine. **Every cost below is an estimate
from code reading and bandwidth arithmetic, not a measurement.** The timer plan (section 4) says how
to measure each one.

Scope: the renderer (`LibplaceboVideoRenderer.cpp`), the hook chain, entrance/B3, motion
(DejudderWarp, NVOF), deband/texture restore, AdaptiveDownscale, capture upload/unpack and the output
chain. Per the coordinator, the 9 modules under migration (diagnostics, projector RS-232, LED measure,
look-ahead and compensation, KernelInverse/re-base helpers, subtitle detection, NLS geometry policy,
LibplaceboOutputPolicy, calibration LUT) are covered only for how the renderer calls them.

Constraints honoured: no change may average grain (vp-grain-faithful), clip BTB/WTW
(vp-btb-wtw-preserve) or hard-code rates or sizes (vp-general-not-fixed). Targets: RTX 3090 real
time at 2560x1600@60 with features on by default; RTX 3050 Ti laptop usable at 4K30.

## How the estimates were made

- **Bandwidth.** One output-size FBO at 2560x1600 is 65.5 MB in rgba32f and 32.8 MB in rgba16f. At
  3840x2160 it is 133 MB and 66 MB. Effective bandwidth was taken as ~650 GB/s on the 3090 (936
  peak) and ~140 GB/s on the 3050 Ti laptop (192 peak). A "hook boundary" is one FBO write plus one
  read: ~0.2 ms on the 3090 at 1600p fp32, and ~1.9 ms on the laptop at 4K fp32.
- **Upload.** The figures come from the code's own measured comment (EarlyUpload.h:5-9): ~2.4 ms CPU
  copy plus ~3 ms transfer for a 4K 12-bit frame.
- **Uncertain** means the pass count or the shader cost is not known without timers.

## 1. Findings ranked by likely saving

| # | Where | What is wasted | Est. cost (how) | Fix | Output | Risk |
|---|---|---|---|---|---|---|
| 1 | Output hook assembly, `RenderLocked` 24560-24900; `PresentDejudderExtras` 17548-17602 | **10-13 separate libplacebo hooks at PRE_OUTPUT/OUTPUT**: tone shape, DV trims, extended-range late restore, LED measure and compensation, calibration LUT, de-judder cache, encoding repair, occlusion mask, NLS and sigma-delta. Each `PL_HOOK_SIG_TEX` hook is an FBO round trip at output size. Most of these stages are per-pixel and could share one pass. | 0.2 ms per boundary on the 3090 at 1600p fp32. With 5-8 removable boundaries: **~1-1.5 ms per refresh on the 3090**, and **~4-8 ms per frame on the laptop at 4K** (uncertain: the boundary count depends on the active features) | The S2/S3 segment plan (central-migration-needs.md §6): one PRE_OUTPUT and one OUTPUT hook that run the per-pixel stages fused in registers via `pl_dispatch`, with the sigma-delta last inside S3. Extras reuse the same segment objects. | Identical up to fp32 rounding of intermediates that were already fp32. Gate with the capture harness at 0 codes. | Medium: order rules (dither last, repair before OUTPUT shaders) move into the segment. |
| 2 | `RenderLocked` 24897 (`frameParams.fbo_float32`); default `fboFloat32 = -1` (auto = on) 1347, 7616 | **Every libplacebo intermediate is rgba32f**, on all tiers and in low latency. That is twice the bandwidth and VRAM of rgba16f for each scaler, colour and hook FBO. | Halves FBO traffic. Assuming ~12-16 FBO round trips per refresh: **~1-2 ms on the 3090 at 1600p, ~5-12 ms on the laptop at 4K** (uncertain: FBO count), plus ~0.5-1 GB VRAM on the laptop at 4K | Owner decision. Options: (a) per-tier `fbo_float32` (off on the laptop tier; the cfg comment says fp16 rounds only above white, ≤0.84 of a 10-bit step); (b) a fork patch that keeps fp32 only for the linear-light scaler FBOs and uses fp16 elsewhere | **Not byte-identical.** Highlights above white differ by ≤0.84 of a 10-bit step. BTB/WTW are kept in both (float). Grain is unaffected. | Low code risk. Picture sign-off needed. |
| 3 | `EarlyActive()` 5453-5457; raw route `UnpackRawPayload` 6711, `UploadDejudderPlanes` 16657 | `capture_upload auto` uploads early **only in low latency**. In the normal (film / de-judder) mode, each raw 4K frame is copied and transferred **on the render thread**. With native RGB plus de-judder, B's luma is unpacked from one raw upload and A from another: UploadReuse covers only the YCbCr routes (`TakeNextPlanes` 16754). | **~5.4 ms of render-thread time per 4K 12-bit source frame** (EarlyUpload.h measurement). The RGB double upload doubles that when it applies. | Make `auto` mean early in every mode once the slot pool is sized by need (frames in flight, not the 1 GB cap). With early on, both RGB unpacks read the same GPU slot. | Byte-identical (same bytes, fence-ordered) | Medium: VRAM on the 4 GB laptop (4K r210 ~33 MB per slot). Re-run the anti-tearing tests. |
| 4 | Main render on `cadenceRepeat` (`RenderLocked`; `static_refresh` 24334) | A captured repeat (24-in-30, 30-in-60) skips analysis and re-upload (19535) but **re-runs the whole `pl_render_image`**: scaler, tone/gamut, adaptive downscale, all PRE_OUTPUT hooks. Only the temporal sigma-delta needs to run again. | One full render per repeat refresh: ~20% of renders for 24-in-30 and ~50% for 30-in-60. Probably ~2-6 ms per repeat refresh on the laptop (uncertain) | Arm the 6d rendered-picture cache (`dejudderCache`, 24775) on every first present, and draw repeats with `DejudderRendered::OutputOnly` (the extras' path) | Should be identical: the pre-OUTPUT picture is deterministic and the cache is rgba32f. Verify with the capture harness, as 6d was. | Medium: encoding repair, LUT and mask rules as in 17575-17602 |
| 5 | `OutputEncodingRepair` inserted at 24790 | **An extra full-frame OUTPUT pass on almost every frame**: any texture PRE_OUTPUT hook (LED, tone shape, de-judder cache, ER late) triggers the libplacebo 7.360 encoding bug workaround | 1 boundary: **~0.2 ms on the 3090, ~1-2 ms on the laptop at 4K** | Fix the encoding drop in the vp-fl35 libplacebo fork, or fold the repair into the S3 segment (#1) | Must match the repair's encode. Verify with the tolerance gate. | Low-medium |
| 6 | `AdaptiveDownscale::Measuring` (AdaptiveDownscale.cpp 553-720) | When on (higher tiers), it runs a **second full scaler**: polar `ewa_lanczossharp` from the source crop into an rgba32f "faithful" picture, in addition to libplacebo's own kernel, plus the edge/grain pass | Polar EWA 4K→1600p is ~60 taps per output pixel: **~1-3 ms on the laptop, ~0.3-0.6 ms on the 3090** (uncertain) | `adaptive_faithful` ortho (two 1-D Lanczos passes: about 5-8x fewer taps, and the owner prefers separable filters, vp-prefer-separable-simple). Today `auto` picks polar whenever the axis ratios agree within 10%, which is the usual case. | **Changes output** (polar vs separable). Owner A/B. Grain is untouched (the blend weights are unchanged). | Low code risk |
| 7 | `ExtendedRange` carry, ExtendedRange.cpp 615-648 | Two full passes read the **same** RGB-stage texture at source size, writing the clipped picture and the remainder to two rgba32f textures | One extra source-size read plus a dispatch: **~0.1 ms on the 3090, ~0.5-1 ms on the laptop at 4K** | One compute pass with two `imageStore` outputs | Byte-identical (same math) | Low. BTB/WTW behaviour unchanged. |
| 8 | VRAM: `Temporal sigma-delta.glsl` 299-311; release sites only at device teardown (8835-8850) | (a) `LQPREV`/`MVPREV` are allocated at full output size but use only 1/16 and 1/256 of it (shader comment 130). (b) ExtendedRange (2x rgba32f at source size), AdaptiveDownscale, CrossfadeMix scratch, rebase, debander and texture-restore textures are kept until device teardown after their feature goes idle (for example ER after SDR→HDR) | (a) **~125 MB at 4K, ~62 MB at 1600p**. (b) ER alone ~266 MB at 4K. On the 4 GB laptop this is what pushes `MotionEngineIdle` into pressure releases. | (a) Add `{{output_width_q}}`/`{{output_width_b}}` templates to the hook-chain substitution. (b) Release a module's textures after N seconds inactive (as MotionEngineIdle does) | Byte-identical | Low |
| 9 | B3 ring `CopyEntrance` 5910-5953, `StoreEntranceOwned` 5956 | Each source frame's processed planes are **copied** into the ring (a full-plane render pass per plane) | 4K 16-bit 4:2:0 ≈ 50 MB read+write: **~0.35 ms on the laptop, ~0.08 ms on the 3090 per source frame** (B3 is off by default today) | Let the stages render into a ring-provided target, or swap ownership of the module's output texture into the ring entry instead of copying | Byte-identical | Medium: module texture lifetime |
| 10 | CPU per refresh in `RenderLocked`: 19829, 20141, 22399, 22740, 22940, 25114 | **Seven `std::ostringstream` change signatures per refresh**, each formatted (locale-aware floats, heap) and `str()` called twice, only to detect a change for a log line. There are also per-frame `std::string` copies (`nlsRule.filename`/`substr` 24750, `ExtendedRange::Describe` 24709) and per-frame `std::vector` hook lists (also rebuilt per extra, 17543). `DebugLog::Log` always `snprintf`s into a 4 KB buffer and takes the queue mutex (DebugLog.h:55-70); most sites are rate-limited before the call. | **~20-60 µs per refresh** (uncertain; no CPU timers). Small, but on the render thread at 60-120 Hz with extras. | Compare POD tuples or hashes; build the string only on change. Reserve the hook vectors once in `Impl`. | Identical | Low |

Lower-ranked observations (each under ~0.1 ms, or already handled):

- **Sigma-delta own search** (`Temporal sigma-delta.glsl` 382-716): a hierarchical block search at
  output size on every present except the extras and the new frame's first present after extras
  (24866). When the de-judder estimate exists for the frame but next-vectors were not carried, this
  duplicates motion already estimated. Fix: hand the de-judder field (`vector_source = 1`) whenever
  `motionValidity` holds. The 60p / no-de-judder paths still need it. ~0.2-0.5 ms on the laptop
  (uncertain).
- **DejudderWarp per-frame shader text**: the `#define` headers are rebuilt as `std::string` every
  estimate (3760, 3905, 4163). They depend only on settings, so the libplacebo pass cache still hits,
  but there are ~10 small allocations per estimate. `LAND_A` embeds a settings float via
  `std::to_string` (4163): it is a cache miss only when the setting changes. `pl_tex_recreate` x40 per
  estimate is a parameter compare, with no reallocation.
- **`SnapshotForWarp`** (2892) copies block-size fields with compute dispatches: tiny textures, fine.
- **NVOF**: the render-time busy-wait `GetData` loops (NvOpticalFlow.cpp 159/176) run only when timed
  (tests). The readback at DejudderWarp 3440-3470 is test-only. The producer only gets frames while a
  motion consumer runs (23161-23170). Correctly gated.
- **Locks**: `renderMutex` is held for the whole render plus extras plus Present (31590). In the
  default path (render_ahead off), that includes up to 3 `WaitForVBlank` per extra (17661). Every
  other reader uses `try_to_lock` (28494-29216), so there is no contention cost. The calibration info
  getter spins `Sleep(2)` up to 100 times (28912), but off the render thread.
- **Render-ahead off (default)**: each extra waits for the previous picture's vblank before rendering
  (17650-17680). GPU and CPU therefore never overlap across refreshes, so each extra's GPU time must
  fit inside one refresh after the wait. This is a headroom loss, not wasted work. Render-ahead
  (under repair) is the fix.

## 2. Features running when OFF (systematic check)

| Feature | Gate | Result |
|---|---|---|
| De-judder estimate | 16414-16450: returns for Off/LowLatency/Interlaced/MaskOnly/IneligibleCadence/NativeRgb/SmoothMotion; skips repeats and estimated pairs | OK. The engine is kept loaded but idle for hiccup repair (VRAM only, by design) |
| NVOF producer | 23113-23170: wanted only with de-judder plus optical flow outside low latency; frames only with a consumer | OK |
| Entrance stages | `ActiveEntrance()` lists active slots only; deband idle at threshold 0 (5794-5806) | OK, zero passes |
| AdaptiveDownscale | `Measuring` returns `unchanged` unless downscaling, or on the upscale gate (568-580) | OK. Textures are kept after it turns off (#8) |
| ExtendedRange | `Decide` per frame; hooks only when active | Passes OK. Textures kept (#8) |
| Occlusion mask / grain | 24604-24660: installed only when the mask or grain map is in use | OK |
| Sigma-delta search | skipped on repeats (`static_refresh`) and with `vector_source` | OK (see the lower-ranked note) |
| LED measurement (call site only) | excluded from extras (17548-17552); runs on main presents **including captured repeats** | Repeats re-measure an identical picture. Feed the cached result on `cadenceRepeat` (owner of the module: migration agent) |

## 3. Duplicated computation across modules (call level)

- **CPU luma of the same raw frame is decoded independently** by SceneDetector, ActivePictureEvidence,
  SubtitleBoxDetector, HdrInference, ContentIntent/IntentInterpreter (`BuildGrid` via per-pixel
  `Sample` lambdas), LedLookahead (full-coverage rows) and the NVOF producer's half-size luma. That
  makes 5-7 passes over the same 4K buffer on CPU threads. A shared per-frame reduced luma product
  (one decode to a 16-bit luma plane, or a grid pyramid, cached with `AnalysisLumaSource.generation`)
  would serve all of them; full-coverage readers keep the plane. This matters most on the laptop's
  CPU (vp-cpu-thread-budget). Owners: mostly Bill's analysis plus migrated modules, so propose it as
  an interface, not a rewrite.
- **Motion**: the matcher's pyramid is built from A's reused planes (DejudderWarp 3612-3630, already
  exact reuse). The NVOF producer separately downsamples on the CPU (MotionAheadProducer.cpp 40-125).
  This is acceptable: it is off the render thread and on its own device.
- **Upload**: the YCbCr and DV routes reuse B's upload for A (`TakeNextPlanes` 16754, efficiency_fixes). The
  RGB route does not (#3).

## 4. Where to add GPU timers first (top 5)

Existing infrastructure: `AlphaD3D11FrameGpuTimer` (asynchronous D3D11 timestamp ranges, polled two
frames later, no flush) and libplacebo `info_callback` per-pass `pass->last` (already used for the
sigma-delta on extras, 5321, and for the PR75 accumulator, 23922). Neither waits on the GPU.

1. **Every libplacebo pass, by description, sampled 1 in N frames.** Generalise
   `DitherExtraPassCallback` into a per-description accumulator on the main render and the extras.
   This immediately prices #1, #2, #5, #6 and #7: scaler, tone/gamut, each hook, the sigma-delta's 5
   passes, ER carry/restore, AD faithful/blend, encoding repair. Zero sync cost.
2. **Entrance S1 per path** (`RunMainEntrance`/`RunEntrance`/`CopyEntrance`, 5690-5765). This runs on
   `rebaseDispatch`, outside `pl_render_image`, so (1) cannot see it. Use pl_dispatch timers (polled,
   never drained) per stage: re-base, deband, texture restore/detail pulse, ring copy.
3. **Motion estimate**: enable DejudderWarp's existing per-pass `pl_timer` collection (2585-2640)
   **without** `passDrain`, sampled, in production. Split it into matcher vs NVOF-ahead consume vs
   warp. NVOF itself runs off the graphics queue, so time it with QPC around the producer's fence.
4. **Upload/unpack**: split the existing `source_upload` stage into CPU copy (QPC), GPU unpack
   (pl_timer) and early-slot fence wait, with the thread named. This prices #3.
5. **Per extra and per repeat**: keep `renderAheadExtraTimer`/`renderAheadEstimateTimer` (17750,
   18134) always on, sampled. Add a CPU QPC split of `RenderLocked` (analysis/subtitle/crop, hook
   assembly, submit, present wait) so vblank waits are not counted as GPU cost. This prices #4 and
   #10.

## 5. Suggested order

#3 (early upload in all modes; byte-identical, largest render-thread saving), #7 and #8 (byte-identical,
low risk), then timers (section 4). Then #1 and #5 together with the S2/S3 segment work, #4 after the
segments exist, and #2 and #6 as owner picture decisions with A/B captures.
