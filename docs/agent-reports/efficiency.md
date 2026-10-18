# Efficiency round (2026-10-18)

Branch `feat/efficiency`, worktree `D:\codex\videoprocessor\efficiency`, base `integ/2026-10-17` at
74340728 (timing merged). Local only (no GitHub). Goal: a cheaper render pipeline with no loss of
precision or fidelity: no lower bit depth, no FP16 where FP32 is used, no grain loss and no
automatic degrading. The timing and queue scheduling decisions were not touched.

## Method

- **Measurements I could use.** The laptop's per-stage GPU costs (docs/audit/laptop-4k30-budget-2026-10-17.md,
  RTX 3050 Ti), the Beta timing gate and matcher pass timings, and the renderer's own CPU timers
  (`lookaheadMs`, `source_upload_cpu_ms`, the motion hand-off).
- **What I added.** A CPU timing of the work I removed from the render thread
  (`EfficiencyTests::ActivePictureMeasurementCpuCost`), and a gated GPU benchmark for Claude.
- **Code inspection** of the capture → upload → render-loop path for work done twice: uploads,
  colour conversions, look-ahead walks, readbacks, per-frame allocations and shader recompiles.
- **Already clean.** Many of the obvious candidates had already been handled:
  - the matcher reuses the last pair's B pyramid;
  - the sigma-delta ping-pongs its state;
  - every readback is polled and never waited on;
  - per-frame textures use `pl_tex_recreate`;
  - generated GLSL headers are constant per setting (no per-frame recompiles);
  - the source-resolution look-ahead and the LED measure each frame once;
  - the extras' OutputOnly render drops peak detection, deband and the LUT.

## Ranked cost per source frame

These are the costs on the critical path (render thread or GPU).

- **Laptop** = measured on the RTX 3050 Ti (budget doc and Beta timing), except where marked "est.".
- **3090** = estimated from the laptop at about 4.9x bandwidth and compute, and about 2x PCIe.
  These are not measured; the benchmark below measures both.
- Source-size stages scale with the source. Output-size stages scale with the output (4K on the
  projector).
- **1080p24** is a 1080p source rendered to a 4K output.

| # | Stage | Laptop 4K24 / 4K30 | Laptop 1080p24 | 3090 4K (est.) | Notes |
|---|---|---|---|---|---|
| 1 | De-judder extras (rendered warp + OutputOnly render, ~1.5 per frame at 24p on 60 Hz) | ~7 ms each, ~10 ms | same (output size) | ~2 ms | 4K30 on 60 Hz true 30p: none |
| 2 | Matcher (optical-flow seed mode) | 5.5 ms | ~1.5 ms est. | ~1.2 ms | full search 7.85 ms |
| 3 | **Source upload on the render thread, twice while the de-judder estimates (A and B)** | **2 x 2.4 ms** blocking (+2 x 0.36 ms v210 unpack) | 2 x ~0.6 ms | 2 x ~1.2 ms | **halved by `efficiency_fixes`** |
| 4 | Sigma-delta own search (new frame) | 4.2 ms | same (output size) | ~0.9 ms | replaced by de-judder vectors when the extras run |
| 5 | Re-base 2:1 (only when Stage 12 finds an upscaled source) | 3.5 ms | n/a | ~0.7 ms | |
| 6 | fbo_float32 on every FBO (precision; kept) | +2.8 ms | same | ~0.6 ms | not a lever (owner) |
| 7 | Adaptive downscale, polar vs separable (laptop panel only) | +2.4 ms | | | profile choice |
| 8 | De-judder PRE_OUTPUT cache (4K rgba32f copy + intermediate) | ~1.4 ms est. | same | ~0.3 ms est. | measured by the benchmark; candidate below |
| 9 | Debander (off by default) / chroma upscale | 0.8 / 0.65 ms | | | |
| 10 | Source-resolution monitor tile copy (2 Hz) | 0.9 ms spike, ~0.04 ms mean | | | |
| CPU | Active-picture measurement per walk frame (LED look-ahead / content intent walk) | see below, x(queue depth - 1) | | | **removed by `efficiency_fixes`** |
| off path | Optical-flow producer | 11.3 ms per pair | | | own worker and engine; 34% busy at 30p |

## Changes (one switch, `efficiency_fixes`, default on; off = the earlier behaviour)

The switch is wired like the other fixes switches:
- settings struct, fingerprint, profile parse, change list and settings dump;
- `RendererProfileConfig` validation;
- a Config control under "Fixes under test" with a tooltip, and the defaults table;
- the Config test edit list;
- the harness key tests (default and flip).

Both changes are intended bit-identical.

### 1. The frame shown takes the planes uploaded for it as the next frame

While the de-judder runs (or the occlusion mask, the grain map, interpolation or gap repair),
`UpdateDejudderEstimate` uploads or GPU-unpacks B for the motion estimate. One frame later B is the
frame shown, and RenderLocked uploaded the same payload again into its own planes.

Now:
- `UploadDejudderPlanes` records how each plane pair was filled: the payload (weak_ptr), the frame,
  the layout, the route, and pl_upload_plane's plane descriptions.
- The records are swapped with the textures when the pair estimated ahead is consumed.
- `TakeNextPlanes` swaps B's planes in as the frame's own when `UploadReuse::CanReuse` holds: the
  same live payload object, the same frame, width, height, chroma height, row bytes and route
  (YCbCr only; the RGB route uploads the matcher's luma only).
- Every other case uploads as before.
- The log shows when the reuse engages (`Efficiency: the frame shown took the planes ...`) and
  when it misses, with the reason (first five).

Files: `vprenderer/UploadReuse.h` (pure decision); in LibplaceboVideoRenderer.cpp:
`DejudderPlaneUpload`, `UploadDejudderPlanes`, `TakeNextPlanes`, the main upload site and
`ReleaseDejudder`.

**Proof**
- **CPU, `EfficiencyTests::UploadReuseOnlyForExactlyTheSameUpload`.** Reuse happens only for
  exactly the same upload. Each of the 15 differences refuses, including a freed payload, another
  route and RGB.
- **GPU, `UploadReuseRoutesAreBitIdentical`** (WARP; hardware with VP_TEST_GPU=hardware):
  - covers both routes: P010 and P210 through pl_upload_plane, and V210 and UYVY raw upload +
    GpuCaptureUnpack;
  - the same payload gives byte-identical planes, with the same format and size, in a second
    texture pair, even one that held another size and format before;
  - the plane descriptions handed on are the upload's own.
- **Renderer, `EfficiencyFixesArePixelIdentical`** (GpuHardware harness, for Claude):
  - 24p film with the de-judder on, on the gpu and standard routes;
  - the presented codes (`raw_r10_sha256`) are equal with the fix on and off;
  - the reuse is logged only when on.

**Saving.** One source upload (and its GPU unpack) per new frame while the de-judder estimates:

| | Saving per new frame |
|---|---|
| Laptop 4K | ~2.4 ms of render-thread blocking + 0.36 ms GPU unpack (12-bit RGB route excluded) |
| Laptop 1080p | ~0.6 ms |
| 3090 4K | ~1.2 ms est. |

On 60 Hz with de-judder this is directly inside the new-frame work the gate budgets. With
`capture_upload: early` the second upload was a GPU copy from the early texture, so the saving
there is the copy and the unpack (~0.4-0.6 ms laptop est.).

### 2. The look-ahead walk measures a frame only for an observer that still wants it

With the LED's look-ahead (compensate, or script with look-ahead: the whole queue) or content
intent without the active-picture look-ahead, the render loop's statistics-only walk ran
`ExtractActivePictureEvidence` on every walked frame on every dequeue. Each frame was therefore
measured once per walk that reached it: about 4 times with the Normal queue of 5, and up to 8.
The observers then ignored every repeat (each measures a frame once, in order).

Now:
- `QueuedFrameStatistics::IObserver::WantsFrame` (default: every frame) mirrors the observer's
  own early returns: `LookaheadObserver` (predictions wanted, sequence not seen) and
  `ContentIntent::Observer` (on, sequence not seen).
- The walk skips frames that no observer wants, after `PrepareFrameAnalysis`, so the Dolby
  Vision gap break is unchanged.

**Proof**
- **CPU, `EfficiencyTests::*WantsFrameMirrorsObserveFrame`.**
- **CPU, `EfficiencyTests::WalkSkipsOnlyFramesNoObserverWants`.** The real observers walk a
  sliding Normal queue through the real `SelectLookaheadWalk`, with and without the fix:
  - content intent receives exactly the same calls and ends in the same classification;
  - the LED gets the same first observation of every frame and the same depth;
  - each frame is measured once (40 measurements instead of ~190).

**Saving (render thread).** (queue depth - 1) x the measurement cost per source frame. The
measured cost is in the verification section.

## Candidates measured or estimated but not implemented

| Candidate | Saving | Why not now |
|---|---|---|
| Fuse the de-judder cache into libplacebo's PRE_OUTPUT shader (a storage-image write from a SIG_COLOR hook instead of a texture hook + copy) | the cache's cost (benchmark), ~1.4 ms laptop est. per new frame with extras | needs GPU validation of storage writes in libplacebo's fragment output pass. Not bit-identical in the rgba16f configuration (it would store unrounded float32: more precise, but different) |
| LUMAQ (sigma-delta quarter-res luma) computed inside the quantizer on refreshes whose search is skipped (extras with de-judder vectors, cadence repeats) | ~0.7 ms laptop / 0.15 ms 3090 per such present at a 4K rgba32f output | needs a `//!WHEN` on a runtime parameter and a shared-memory reduction that keeps the summation order. GPU-only verification |
| Merge the matcher's small passes (budget doc #7) | -0.5 to -1 ms | needs per-pass GPU timing to pick merges. The Beta timing gate gives it |
| Motion-ahead producer: a persistent row pool instead of 3 threads created per frame | ~0.1 ms CPU per frame on its worker | not on the critical path |

## Verification (this laptop, CPU and WARP only)

- **Build.** The full solution, x64 Release, built (rc 0).
- **Main suite** (VideoProcessor-Test, one vstest process, TRX): 3352 tests, 3350 passed.
  - `DeckLinkConvertersSustained4K60PerformanceComparison`: a CPU formatter p95 over 16.7 ms
    while other agents' builds were loading the laptop. The formatter code is untouched.
  - `RendererDllExportsTheHostBuildId`: an artefact of relinking the test DLL alone. It passed
    after the full rebuild, together with EfficiencyTests and the key tests (7/7).
- **Efficiency tests.** EfficiencyTests 5/5 and `UploadReuseRoutesAreBitIdentical` (WARP) passed.
  The walk measured 190 times before the fix and 40 after (40 frames, queue 5).
- **Config tests** (`run_config_tests.ps1 -Shards 1`, off screen): GATE PASSED.
- **Measurement cost.** `ActivePictureMeasurementCpuCost` on this CPU: 0.17 ms per measurement
  at 4K and 0.29 ms at 1080p. With the Normal queue that saves about 0.7-1.2 ms of render-loop
  CPU per source frame whenever the LED's look-ahead or content intent walks the queue. Real
  pictures that need the deeper scans cost more.
- **Not run (Claude, on the GPU):**
  - `EfficiencyStageCostsOnThisGpu`;
  - `EfficiencyFixesArePixelIdentical`;
  - `UploadReuseRoutesAreBitIdentical` on hardware;
  - the whole-chain gate;
  - an on-screen check that `Efficiency: the frame shown took the planes` appears with the
    de-judder on and `source_upload_cpu_ms` drops.

## Estimated savings per new frame (with the de-judder estimating)

| | Laptop 3050 Ti | 3090 (est.) |
|---|---|---|
| 4K24 / 4K30, capture_path gpu, render upload | ~2.4 ms render-thread blocking + ~0.36 ms GPU (about 10% of the 60 Hz new-frame slot) | ~1.2 ms + ~0.1 ms |
| 1080p24 | ~0.6 ms + ~0.1 ms | ~0.3 ms |
| capture_upload early | ~0.4-0.6 ms (copy + unpack) | ~0.1 ms |
| LED look-ahead / content walk (CPU) | ~0.7-1.2 ms per source frame | similar CPU class |

The benchmark replaces these estimates with measurements.

## GPU benchmark for Claude

Target `VideoProcessor-Test.dll`, test `LibplaceboLutParserTests::EfficiencyStageCostsOnThisGpu`.
- Environment: `VP_TEST_GPU=hardware` and `VP_TEST_ADAPTER=NVIDIA`. With
  `VP_TEST_REQUIRE_HARDWARE=1` a missing GPU fails the test instead of skipping it.
- It reports, at 4K and 1080p:
  - the saved upload on each route;
  - the de-judder cache's cost;
  - the re-base and monitor costs.
- Also run `RendererCaptureHarnessTests::EfficiencyFixesArePixelIdentical` (GpuHardware) and
  `LibplaceboLutParserTests::UploadReuseRoutesAreBitIdentical` with `VP_TEST_GPU=hardware`.
