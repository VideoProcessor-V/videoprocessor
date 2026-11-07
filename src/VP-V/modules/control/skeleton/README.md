<!-- Copyright(C) 2026 VP-V contributors -->
<!-- SPDX-License-Identifier: GPL-3.0-only -->
# Skeleton activation (`control.skeleton`)

Switches the analysis lane (`core/AnalysisLane.h`) on through the VP-V hub, one lane per renderer, and (70b G5) hosts the render-thread
GPU dispatcher for the GPU products P10 / P11 / P12. **Off by default**: with `[vpv.skeleton] analysis_lane = off` (and in the Original
pathway) there is no state, no thread, no source tap, no seal provider, no dispatcher and no status line; the renderer behaves byte for
byte as before.

Unit: control/skeleton

## Settings

Keys of `[vpv.skeleton]` in `VideoProcessor.cfg`. The file is read at `GpuCreated` and at every `SettingsApplied` (`code != 0` = the Original
pathway = off), on the render thread. An invalid value keeps the default and is shown under Fallbacks (`skeleton_settings`), never log-only.
"Original (Bill)" is the Original pathway: the lane is off, no state, thread, tap, dispatcher or status line.

| Key | Section | Type | Range / values | Default | Config placement | Original (Bill) | Fallbacks / status | Tiers |
|---|---|---|---|---|---|---|---|---|
| `analysis_lane` | [vpv.skeleton] | enum | off, shadow, on | off | not in Config (cfg-only until the M2 Config page) | off (nothing built) | `analysis_lane` (reduced / forced live / at minimum), `skeleton_lane` (waiting for `resync_event=on` or `frame_analysis_store=on`), `skeleton_gpu_shadow` ("shadow: GPU products skipped"), `skeleton_gpu_device` ("GPU products: no render device"), Ctrl+I line, Ctrl+Alt+D | the lane plans its tiers by video memory (VramGovernor); shadow = CPU lane only, no GPU dispatcher; on + a render device = CPU and GPU products (see the shed order under Status and fallbacks) |
| `analysis_lane_chunk_us` | [vpv.skeleton] | int | 0..4000000 (0 = derived from the frame period) | 0 | not in Config (cfg-only until the M2 Config page) | n/a (VP-only) | invalid value: `skeleton_settings`, default kept | same on all |
| `analysis_lane_deterministic` | [vpv.skeleton] | enum | auto, on, off | auto | not in Config (cfg-only until the M2 Config page) | n/a (VP-only) | `analysis_lane_determinism` when frames miss the bounded wait | same on all |
| `analysis_lane_test_wait_ms` | [vpv.skeleton] | int | 0..4000000 | 0 | not in Config (cfg-only until the M2 Config page; test only) | n/a (VP-only) | invalid value: `skeleton_settings` | same on all |
| `camera_link` | [vpv.skeleton] | enum | off, report | off | not in Config (cfg-only until the M2 Config page) | off (no state, no line) | `camera_link` when asked to report but unable (no store, no seal); status line when on | same on all |
| `skeleton_gpu_gap` | [vpv.skeleton] | enum | off, after_render | off | not in Config (cfg-only until the M2 Config page) | n/a (VP-only; off in the Original pathway) | after_render: `skeleton_gpu_gap` ("GPU lane work runs ahead of the present"); off with a dispatcher: `skeleton_gpu_gap_off`; invalid value: `skeleton_settings` | off: the GPU products get no lane producer (in-render-only, live form deferred); after_render: P10/P11/P12 lane jobs run in the AfterRender gap, chunked by the gap budget (one chunk per pass); the CPU products' gap is unchanged either way |
| `grain_split_storage` | [vpv.skeleton] | enum | full, compact | full | not in Config (cfg-only until the M2 Config page) | n/a (VP-only) | `grain_split_storage` entry of the kernel (Compact refused for the plane -> Full, with the reason; Compact granted -> Released); invalid value: `skeleton_settings` | P10 storage either/or; Compact is also the governor's visible budget step (Compact -> Full / Full -> Compact is shown by the kernel's decision, never silent) |
| `source_stats_walk` | [vpv.skeleton] | enum | original, lane | original | not in Config (cfg-only until the M2 Config page) | Bill's active-picture walk | `source_stats_walk` when the lane's source stats are unavailable | same on all |

`source_stats_walk` is a lane key (`LaneSettings::Parse`) that the module's key loop does not pass from the file yet (tests and tooling set it; the cfg key has no effect today). `skeleton_gpu_gap` and `grain_split_storage` are parsed in `SkeletonSettings.h` with the same problem-reporting style as the lane keys.
`camera_link = report` only creates the camera link core and its status line; there is no vector engine yet, so it never feeds a consumer.

## Environment and diagnostics

| Name | Purpose | Default | Scope |
|---|---|---|---|
| Ctrl+I (status overlay) | one `skeleton=analysis lane <mode> <w>x<h>, ...` line while the lane runs | off with the lane | runtime |
| Ctrl+Alt+D (diagnostics) | the full status text and every engaged fallback of this module (`OnCommand("skeleton","status")` returns the same text) | n/a | runtime |
| `VP_TEST_GPU` | `hardware` runs the GPU tests of the dispatcher and the three kernels on the real adapter instead of WARP | WARP | test |

No `VP_*` variable changes the module's behaviour.

## Data sources

* **Source frames:** `VpFrameAnalysis::SourceFrameTap` (owner key = the renderer identity `host`). The frame-analysis worker decodes the shared
  luma plane for a registered sink and delivers it as a `SourceFrameView` (10-bit sampler codes, `msbShift` 6, `keepAlive` = the plane). Needs
  `frame_analysis_store = on`; without it the module shows `skeleton_lane: no source frames: needs frame_analysis_store=on`.
* **The one seal:** `ResyncExtension::SealFor(host)` (a `FrameSeal` provider rebinding the lane to each new-era Indicator and a subscriber calling
  `OnSealed`). Needs `resync_event = on`; otherwise `skeleton_lane: waiting: resync_event=on required`. The hooks are added once per renderer;
  switching the lane on after the first frame counts as a late provider (the lane binds at the next era; shown).
* **Render phases:** hub `OnFrame(Begin)` -> `OnRenderStart` (lazy `Configure` once the source size is known / changes; never GPU work here:
  Begin runs before the render lock); `OnFrame(AfterRender)` -> deferred releases + dispatcher `Configure` (GPU-legal) -> post-submit gap (half the
  measured frame period; skipped and counted while it is unknown) -> `OnFrameRetired`; `OnQueue(Drop)` -> `OnFrameDropped` (applied at the next Begin).
* **Render device:** `LifecycleFacts::gpu` (the renderer's `pl_gpu`) is stored at `GpuCreated` (per-host aux slot) and read when the dispatcher is
  decided; `LifecycleFacts::device` serves `IDXGIAdapter3::QueryVideoMemoryInfo` at each lane `Configure` (a failure leaves zeros and is shown).

## GPU products and the dispatcher (70b G5)

* A `SkeletonGpuDispatcher` (own `pl_dispatch` on the renderer's `pl_gpu`) with `GrainSplitKernel` (storage per `grain_split_storage`),
  `RegionStatsKernel` and `LightLevelKernel` exists per renderer **only** while the lane is wanted AND `analysis_lane = on` AND a `pl_gpu` was given.
  Shadow, Off, the Original pathway and a missing/unusable render device construct none: no GPU call is ever made (tested), and the status says why.
* The Bundle (built when the source size is known) registers the three `GpuComputeLaneJob`s (`FixedStepsTag`, steps = `StepsFor(id, dims)`) as lane
  producers before the worker threads start, only with `skeleton_gpu_gap = after_render`; P12 carries a `LightLevelBeginGate` on `ReadOf(P12, seq-1)`
  ("waits for P12(N-1)"). Their predicted cost is the larger of the dispatcher's measured time and a conservative figure derived from the
  declaration's reference ms and the pixel count (never a fixed resolution or rate). The lane is configured on `TapFormProducts(SkeletonProducts(compact))`
  when a dispatcher is wired (the plain registry otherwise): P13 (RenderOnly, never filled) is dropped from P10/P11 or P10 would be InputMissing every frame.
* The declining live fill is installed only when there is no dispatcher (a live fill would win over it). With a dispatcher the lane defers GPU live
  fills (`SetGpuLiveDeferred`); `RunDeferredLive` is not called before the Entrance phase (M2 P4), so the status shows `live: waits for the Entrance phase (M2 P4)`.
* Textures are allocated at `Configure` (never per frame) in AfterRender, before the gap; the previous set is destroyed at the next AfterRender
  (`FlushReleases`) or at `GpuReleased` / switch-off (`Release`).
* With `skeleton_gpu_gap = off` the GPU products have no lane producer (in-render-only); until the Entrance phase exists nothing makes them, which the
  status states (`skeleton_gpu_gap_off`, `skeleton_gpu_live`).

## Products

| Id | Unit version | Fill rule | Consumer fallback when absent |
|---|---|---|---|
| P1 half luma, P4 source stats | catalog | CPU producers on the lane's CPU worker; a late or missing result is filled live in the render, bit-identical (`ComputeHalfLuma` / `ComputeSourceStats`) | consumers compute their own |
| P10 grain split (GPU) | 2 (`kExpectedGrainSplitUnit`); tap form `kTapFormUnitVersion` = 1 | lane: tap bands, extend, five splitter passes (one chunk per pass); live: the same steps at once (Entrance, deferred); storage Full or Compact (`grain_split_storage`) | readers run their own split; `CheckFit` returns WrongSize / Stale and the reader falls back visibly |
| P11 region block stats (GPU) | 2 (`kExpectedRegionUnit`); tap form 1 | one pass on the extended tap plane; band energies from P10 Compact; without P10 / Compact: "P11 without bands" (re_have_split 0); display facts = shown defaults | regional repairs use their own local statistics (`product.region_evidence`) |
| P12 light level (GPU) | 3 (`kExpectedLightLevelUnit`) | one pass; reads P11 S0/S3/S4 and its own previous frame; no P11: frame level from P4's APL ("P12 frame-level"); CPU LUT built at Prepare / settings generation | light-level users use the global level (`product.light_level`) |
| P10c grain split chroma | n/a | **declined**: "P10c declined: chroma needs the source ring (M2 P10)" | chroma upscale runs a lower tier without the grain guard (`product.chroma_cfl`) |

**Tap form (70b section 3):** until the source ring (P13, M2 P10) exists, P10 and P11 read the *tap plane*: the lane's analysis luma (10-bit studio codes << 6,
R16 unorm, uploaded in row bands). Unit version `VpLaneProducts::kTapFormUnitVersion` = 1, provenance `VpLaneProducts::kTapProvenance`. At the migration to
P13 both change, and every consumer's `CheckFit` falls back visibly (WrongSize / Stale) instead of reading a plane of another provenance.
Nothing reads P10-P12 yet (the `skeleton.report` consumer only declares them).

## Status and fallbacks

Every downgrade is visible (Ctrl+I line, Ctrl+Alt+D, Config status); none is log-only. Strings (FallbackCatalog ids):

| Id | Text | When |
|---|---|---|
| `skeleton_lane` | `waiting: ...` / `no source frames: needs frame_analysis_store=on` | the lane cannot run yet |
| `skeleton_settings` | invalid key list | a `[vpv.skeleton]` value was not valid (default kept) |
| `skeleton_gpu_shadow` | `shadow: GPU products skipped (no GPU dispatcher, no GPU work)` | `analysis_lane = shadow` |
| `skeleton_gpu_device` | `GPU products: no render device (...)` | `analysis_lane = on` without a usable `pl_gpu` |
| `skeleton_gpu_live` | `live: waits for the Entrance phase (M2 P4)` | a dispatcher exists (live fills deferred) |
| `skeleton_gpu_gap` | `GPU lane work runs ahead of the present` | `skeleton_gpu_gap = after_render` |
| `skeleton_gpu_gap_off` | `GPU lane work off (skeleton_gpu_gap = off): the GPU products are not made ahead` | dispatcher exists, gap off |
| `skeleton_gpu_shed` | `VRAM shed: <products> lost the lane ring (not enough free video memory); made in the render only` | the tier plan refused a GPU product its ring (`GrantWhy::OverBudget`) |
| `skeleton_gpu_memory` | `GPU products not made: private working memory N MiB exceeds the free video memory M MiB (WxH)` | the dispatcher's private working memory alone (below) is larger than the free video memory at a graph build |
| `skeleton_gpu_failed` | `dispatch failed N time(s): <LastError>` | the dispatcher's failure counter is non-zero |
| `skeleton_gpu_chroma` | `P10c declined: chroma needs the source ring (M2 P10)` | a dispatcher exists |
| `skeleton_gpu` (dispatcher) | `<refusal>: the product is unavailable (readers fall back)` | a product's textures could not be allocated |
| kernel entries | grain split storage (Compact -> Full / Full -> Compact), `region_stats_bands` ("P11 without bands"), `region_stats_display` / `light_level_display` ("display facts: defaults"), `light_level_frame` ("P12 frame-level"), `light_level_smoothing` | reported by `SkeletonGpuDispatcher::Fallbacks()` (includes every kernel's list) |

**VRAM shed order (VramGovernor, one budget for all of VP's video memory):** over budget, users step down one level at a time. Rank: tier T2 first, then
T1, then T0; within a tier lossless steps before lossy ones; then the shed order; then the id. Skeleton products are lossless users (level 0 = ring + live
pool, level 1 = live pool only, "computed in the render"): within T0 the readers shed first (reverse topological), within T1/T2 the lowest ms saved per MB
first. So the GPU products shed P12 (T1) first, then P11, then P10 (T0, readers before the producer): each loses its lane ring and is made in the render only.
A tier plan that cannot fit even at every minimum says so (`Video memory: over budget even at minimum settings`).

**Private working memory (not in the declarations, so the governor does not plan it).** The dispatcher holds, outside the product textures: the tap ring
(3 x [tap + extended] R16 planes = 12 B/px) and the grain split's working textures, one set per Splitter (the lane form and the live form each own
one, because intermediates survive across gaps and a live fill must not clobber a lane job): Compact = rows + bands (32 B/px), Full = bands (16 B/px), plus the
block grid. `SkeletonGpuDispatcher::PrivateBytes` computes it analytically; each graph build adds it to the used video memory the governor sees, so the shed
order above acts on what is left. Figures at 3840x2160 (8.29 Mpx, B = 32), lane + live, tap ring included, product ring 2 + live 1:
Compact = 199 MB products + 532 MB working + 100 MB tap = about 0.83 GB; Full = 398 + 266 + 100 = about 0.76 GB (so, with two Splitters, Compact no
longer saves memory; its advantage returns only if the lane and live forms could share one working set). After the governor shed P10's lane ring
(live-only) the lane Splitter is never run, so only the live set stays: Compact about 0.37 GB, Full about 0.30 GB. If the private memory alone exceeds the
free video memory the GPU products are not wired at all (`skeleton_gpu_memory`; the lane's CPU products continue).

## Tests

CPU only (no GPU): classes `SkeletonSettingsTests` (incl. `skeleton_gpu_gap` / `grain_split_storage` parsing and invalid-value problems),
`SkeletonModuleTests` (Off and Original construct no dispatcher and make no GPU call; shadow and "no render device" statuses), `SkeletonGpuStatusTexts`,
`SkeletonLaneTests`, `AnalysisLaneGpuHookTests` (lane deferral, `ReadOf`, `StepsFor`).
GPU (libplacebo WARP unless `VP_TEST_GPU=hardware`): `SkeletonGpuDispatcherWarpTests` (1024 codes exact, extend == CPU, alloc == declaration, lane == live),
`LaneGpuGrainKernelWarpTests`, `LaneGpuRegionKernelWarpTests`, `LaneGpuLightKernelWarpTests`.

```
vstest.console.exe x64\Release\VideoProcessor-Test.dll /Logger:trx /TestCaseFilter:"ClassName~SkeletonModuleTestsNs"
vstest.console.exe x64\Release\VideoProcessor-Test.dll /Logger:trx /TestCaseFilter:"Name~Skeleton|Name~LaneGpu|Name~AnalysisLaneGpuHook"
```

Read the TRX summary line (vstest prints none for this DLL). A WARP test that needs a `pl_gpu` through `SkeletonModule` itself (dispatcher built from `GpuCreated`
with a real device, status lines of the engaged states) is not written yet: the fixture does not give a module a `pl_gpu`.

## Why "skeleton first"

Every analysis product has one owner and one sealed form; this module gives the lane a place to run under the real hub, with the real seal and real source
frames, before any consumer depends on it. Consumers move onto the products one package at a time, each behind its own switch.

## Threading

Lifecycle and frame phases: render thread. The tap sink: the frame-analysis worker. Queue drops: the queue thread (a mutex-guarded list the render thread
drains). Status: a cache rebuilt on the render thread. Per-host state is found through `core/hub/HostSlots.h` (lock-free lookup); the seal hooks hold weak
references, so a released renderer's state can never be touched. All dispatcher calls are render thread.

## Stale state and drops (audit notes)

* A renderer whose device creation throws never gets `GpuReleased`; a new renderer may then reuse its address. The tap keeps a generation per owner (bumped when
  the Host's store registration changes); module state records the generation it was built for and is rebuilt (lifecycle or the next frame) when it differs.
  The tap sink is registered again whenever it is missing.
* The seal hooks of a rebuilt state are appended to the seal again (a `FrameSeal` cannot drop them). The old hooks hold weak references to the discarded state
  and do nothing; the cost is a few idle closures per address reuse.
* Backlog-flush drops (`OnQueue(Drop)`): the call site passes the surviving sequence and the number of frames dropped before it, so the dropped sequences are
  taken as `[seq - n, seq)` (consecutive). That assumption is counted and shown (`dropped frame(s) were taken as consecutive sequences`); at most 16 per event
  are applied, the rest are counted as lost.
* A dispatcher is destroyed together with the lane bundle that points at it (the bundle is stopped first); a changed `pl_gpu`, `grain_split_storage` or
  `skeleton_gpu_gap` rebuilds the bundle at the next Begin.

**Downstream consumers (TEMPORAL DETAIL, docs 71 section Q5 and 72):** per-pair homography (P7/P9), the object layer (per-object motion), P8 per-block membership + confidence, and K4 guided residuals.
