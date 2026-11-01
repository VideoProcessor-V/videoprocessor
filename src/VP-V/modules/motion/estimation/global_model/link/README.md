<!-- Copyright(C) 2026 VP-V contributors -->
<!-- SPDX-License-Identifier: GPL-3.0-only -->
# Camera link: K1 fit runner, K2 P8 + accessor constants, K4 guided matching (module side)

Design: `D:\codex\vp-handover\56-design-camera-k1-k2-k4.md`. Everything here is **inert**: nothing is registered in a renderer,
no Bill file is touched, and with no consumer of P7 there is no thread, product, ring, pool or shader text.

## What is here

| Part | Files | Role |
|---|---|---|
| Interface | `CameraLink.h` (`ICameraLink`, query `vpv.camera.v1`), `../../VectorEngineInterface.h` (`IVectorEngine`, `IPreEstimateHook`, query `vpv.vector_engine.v1`) | the typed surfaces; P5a implements the second |
| K1-1/2 | `PairTiming.h`, `PairAssembly.h`, `FieldCopy.h` | pair seconds (film lock / measured / nominal / unknown = 0), `PairInput` assembly, stamp -> generation (full, continuity, settings[motion] only; re-base is always 0) |
| K1-3 | `CameraRecordPool.h`, `CameraUsable.h` | pool parallel to the P7 SlotSet, 330 B `CameraRecordPod` (declared 1 KB), the one usable set (D-K4) |
| K1-4 | `FieldReadbackRing.*`, `NativeCopyReadback.*`, `GuidanceCountsRing.*` | P3 field / 16 B counts to CPU without waiting: on D3D11 (no buffer transfers there) the shared `LedDrive::NativeTexReadback` with a copy pass; elsewhere `pl_buf` + `pl_buf_poll(...,0)`. Not an `IReadbackDevice` until K1-9 |
| K1-5 | `CameraFitProducer.*`, `CameraFitWorker.*` | `ILaneProducer` for P7; the one worker thread; never `BindFitter` (F2): the reader only bumps atomics |
| K1-6 | `CameraLinkCore.*`, `CameraFitStatus.h` | phases as plain methods, status, `camera.no_model.*` ids, `VP_CAMERA_FIT_TRACE` |
| K2 | `CameraLabelPack.h`, `CameraLabelProducer.*`, `CameraLabelFill.*`, `CameraConsumerBinding.*` | P8 upload / compute (one unit), lane / live fill, per-consumer binding |
| K4 | `CameraGuideProduct.h`, `GuidanceScheduler.*`, `GuidanceGovernor.h`, `GuidanceCountsRing.*`, `MotionBenchWriter.h` | `p7g_camera_guide`, refusal gate, hook, governor, counts readback, `VP_MOTION_BENCH` |

## Skeleton products declared

* P7 camera fit, P8 camera labels, P9 camera smoothed: K0 (`core/Products.h`). P8 is RGBA32F, 16 B per 16x16 block.
* `p7g_camera_guide` (D-K3): `CameraGuideProduct.h`. Cpu, T0, inputs P7 at lag d and P4 at lag 0, 20 B per block. It is **not** in
  `SkeletonProducts()` / `ProductRows()` yet (those tables are pinned by the catalog tests): `SkeletonProductsWithGuide(d)` and
  `GuideRow()` are the integration surface; the wiring step adds it to the assembly and the catalog in one commit.
* Unit version: `kCameraUnitVersion` covers the fit, the labels (both modes) and the guide.

## Private history: justification (TEAM-RULES "skeleton first")

* **Readback ring** (`FieldReadbackRing`): `lane lead + 2` (at least 3) host-mapped buffers of one field each (4K: 0.5 MB x2 planes).
  It is a transfer staging area, not a frame history: an entry lives from the request until the next poll, then is freed. The
  shared products stay in the store; this is the only way a GPU product reaches the CPU fit without a blocking map.
* **Record pool** (`CameraRecordPool`): holds the `shared_ptr`s the P7 slot cannot (1 KB cap): the per-block arrays and the
  field copy. It is indexed like the P7 SlotSet ring, written only under the claim, read only for sealed / pinned seqs, and erased
  on a late publish. It stores nothing P7 does not already own; it is the heavy half of the same product.
* **Fitter history** (previous record, temporal filter, object tracker): inside `Fitter`, history 1 as P7 declares; it is the
  fit's own state, not a picture buffer.
* No private full-resolution texture exists anywhere here: P8 slots are per-block (0.5 MB at 4K), the counts texture is 1x1.

## Decisions recorded by the audit round (2026-11-01)

* **No deadline in the Fitter** (overrides doc 56 section 1.4 deadline row): `PairInput::deadlineMicros` is always 0. Every pair is
  fully fitted, so the fit history is deterministic; lateness is judged only at the publish / seal (late = fitted, not published).
  The seal-time estimate (`FramePhaseFacts::sealUs`) and the p95 guard were removed with it.
* **Reset / relatch generations are stamped into the job at submit** (`CameraFitWorker::Submit`); the fitting thread applies a reset
  before THAT job's Fit, whatever the queue backlog.
* **A pool entry is served only after the slot publish succeeded** (`committed`); only the seal observer may read an uncommitted one.
* **Guidance cool-down runs on measured pair time for every pair asked** (`GuidanceScheduler::Guide`), so narrow / E1-only configurations
  recover; lane and live both use the governed switches (snapshot under the mutex) and a lane result built under other switches or
  another lag is dropped. A NoRecord verdict is never stored by the lane.
* **Status holds**: each `camera.no_model.*` event engages its id for 0.5 s of measured time (frame period), then releases;
  `camera.no_model.queue` reports queue-full drops.
* **Binding**: the per-consumer define and the sampler follow the policy, never P8 presence; a 1x1 dummy is bound with `camera_enabled = 0`.

## Rules this code follows

* **Fill rule.** P7: the lane (camera worker) publishes before the seal; a result after the seal is discarded (SlotSet `Late`,
  pool entry erased); there is **no live fill** (D-K1): late = visible "camera: no model (late)". P8: lane in an idle gap, else
  the seal's live fill (only when P7 was sealed), later consumers reuse, P7 not sealed -> P8 absent -> `enabled = 0`.
* **Every fallback is visible.** `camera.no_model.{late,ambiguous,timing,readback,slip,depth0}` (FallbackCatalog `Decision`s from
  `CameraLinkCore::CollectDecisions`), `CollectStatus` lines, guidance refusal counters and governor line. Ambiguous engages after
  0.5 s of measured pair time and releases on the first other record.
* **Determinism.** Every submitted pair is fitted, late or not, so the fit history (hash `HistoryHash()`) does not depend on
  timing; deterministic mode (`CameraLinkSettings::deterministic`) starts no thread and drains with `ProcessPending()`.
* **No hard-coded rates.** Pair seconds, deadlines, cool-down and `d` all come from measured values.

## Activation (report mode, 2026-11-01)

`CameraExtension.h/.cpp` is the H1 adapter in REPORT mode (`[vpv.skeleton] camera_link = report`, off by default): per renderer a
`CameraLinkCore` over that renderer's `FrameAnalysis::Store` (found through `SourceFrameTap::StoreFor`), `OnGpuCreated`, the
Indicator reader through a FrameSeal provider, `OnFrameStart` at Begin (resync driving), `OnPostSubmitGap` after render, status and
decisions into the hub's status. No vector engine exists yet (P5a), so the core is configured with `p7Wanted = false`: no thread,
pool, ring or product. It never feeds a consumer and answers no `vpv.camera.v1` query. `VP_CAMERA_DUMP=1` logs the status once
a second. The rows below remain the plan for the full wiring.

## Wiring TODO (the H1 adapter, ~40 lines, a later step; needs `feat/m2-hooks` c716298f)

`CameraExtension : IRendererExtension`, registered after `ResyncExtension`:

| H1 call | CameraLinkCore call |
|---|---|
| `OnRenderPhase(FrameStart)` after FrameSeal | `OnFrameStart({seq, sealUs, displayPeriod, p7SealedPublished})` (also `table.Resolve(...)` with `fill` calling `CameraLabelFill::FillLive` for P8) |
| `OnRenderPhase(PostSubmitGap)` | `OnPostSubmitGap()` |
| GPU created / released | `OnGpuCreated(pl_gpu)` / `OnGpuReleased()` |
| settings change | `OnSettings(...)`, `AttachGraph(...)` on a graph rebuild, `CameraLabelProducer` mode from `ChooseLabelMode` |
| `CollectStatus` | `CollectStatus(lines)` + `CollectDecisions()` into the fallback registry |
| `QueryInterface("vpv.camera.v1")` | `Link()` |
| Indicator setup | `RegisterReader(indicator)` |
| P5a engine | `SetEngine(IVectorEngine*)`, `engine->SetPreEstimateHook(&CameraGuidanceHook)` |

Also pending: `FieldReadbackRing` becomes an `IReadbackDevice` again at K1-9 (the half-wired methods were removed); `items.props` (H7) listing `GlobalModel*.cpp` and `link/*.cpp`; K1-9 (LaneRunner registration of
`CameraFitProducer` at SK-W); a text-guard / pixel-residual producer ("text guard: none" is shown until then).

## Tests

CPU: `PairTimingTests`, `PairAssemblyTests`, `VerdictGateTests`, `CameraFitRunnerTests`, `CameraFitThreadingTests`, `CameraOffTests`,
`CameraLabelUploadTests`, `CameraConsumerBindingTests`, `P8FillRuleTests`, `GuidanceSchedulingTests`, `GuideProductTests`,
`GuidanceGovernorTests`, `MotionBenchWriterTests`. WARP (category `GpuWarp`): `FieldReadbackRingWarpTests`, `CameraLabelWarpTests`,
`GuidanceNullIdentityWarpTests`.
