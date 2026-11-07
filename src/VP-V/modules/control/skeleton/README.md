<!-- Copyright(C) 2026 VP-V contributors -->
<!-- SPDX-License-Identifier: GPL-3.0-only -->
# Skeleton activation (`control.skeleton`)

Switches the analysis lane (`core/AnalysisLane.h`) on through the VP-V hub, one lane per renderer. **Off by default**: with
`[vpv.skeleton] analysis_lane = off` (and in the Original pathway) there is no state, no thread, no source tap, no seal provider
and no status line; the renderer behaves byte for byte as before.

## Settings (`[vpv.skeleton]` of `VideoProcessor.cfg`)

| Key | Values | Default |
|---|---|---|
| `analysis_lane` | `off` / `shadow` / `on` | `off` |
| `analysis_lane_chunk_us` | 0..4000000 (0 = from the frame period) | `0` |
| `analysis_lane_deterministic` | `auto` / `on` / `off` | `auto` |
| `analysis_lane_test_wait_ms` | 0..4000000 | `0` |
| `camera_link` | `off` / `report` (see `motion/estimation/global_model/link/CameraExtension.h`) | `off` |

The lane keys are `VpSkeleton::LaneSettings::Parse`. An invalid value keeps the default and is shown under Fallbacks
(`skeleton_settings`). The file is read at `GpuCreated` and at every `SettingsApplied` (`Lifecycle` with `code != 0` = Original
pathway = off), on the render thread.

## Data sources

* **Source frames:** `VpFrameAnalysis::SourceFrameTap` (owner key = the renderer identity `host`). The frame-analysis worker
  decodes the shared luma plane for a registered sink and delivers it as a `SourceFrameView` (10-bit sampler codes, `msbShift` 6,
  `keepAlive` = the plane). This needs `frame_analysis_store = on`; without it the module shows
  `skeleton_lane: no source frames: needs frame_analysis_store=on`.
* **The one seal:** `ResyncExtension::SealFor(host)` (a `FrameSeal` provider rebinding the lane to each new-era Indicator and a
  subscriber calling `OnSealed`). Needs `resync_event = on`; otherwise `skeleton_lane: waiting: resync_event=on required`.
  The hooks are added once per renderer; switching the lane on after the first frame counts as a late provider (the lane
  binds at the next era; shown).
* **Render phases:** hub `OnFrame(Begin)` -> `OnRenderStart` (lazy `Configure` once the source size is known / changes);
  `OnFrame(AfterRender)` -> post-submit gap (half the measured frame period; skipped and counted while it is unknown) and
  `OnFrameRetired`; `OnQueue(Drop)` -> `OnFrameDropped` for the dropped sequences (applied at the next Begin).
* **Video memory:** `IDXGIAdapter3::QueryVideoMemoryInfo` of the renderer's device at each `Configure`; a failure leaves zeros and
  is shown. The lane plans its tiers itself (`VramGovernor` inside `AnalysisLane`).

## What is live and what is declined

* **Live:** P1 half luma and P4 source stats, CPU producers on the lane's CPU worker; a late or missing lane result is filled
  live in the render, bit-identical (`ComputeHalfLuma` / `ComputeSourceStats`).
* **Declined (visible):** P10 grain split, P11 region block stats, P12 light level. They have no producer and no live fill
  here yet: `skeleton_products: ... declined: render-thread GPU dispatch hook pending (M2 Upload/Entrance phase)`. Their
  consumers keep their own computation. Nothing reads the products yet (the `skeleton.report` consumer only declares them).
* **Capped record history (visible):** the quality-pattern and td trail-summary histories are seconds of the measured source rate, bounded so
  every ring fits 255 slots. When the rate asks for more, `skeleton_history: quality pattern history capped at 223 frames (source rate above
  111.5 fps)` appears in Fallbacks; a graph whose slot counts still overflow is rejected (analysis lane unavailable, with the reason), never clamped.

## Why "skeleton first"

Every analysis product has one owner and one sealed form; this module gives the lane a place to run under the real hub, with
the real seal and real source frames, before any consumer depends on it. Consumers move onto the products one package at a
time, each behind its own switch.

## Threading

Lifecycle and frame phases: render thread. The tap sink: the frame-analysis worker. Queue drops: the queue thread (a mutex-guarded
list the render thread drains). Status: a cache rebuilt on the render thread. Per-host state is found through
`core/hub/HostSlots.h` (lock-free lookup); the seal hooks hold weak references, so a released renderer's state can never be touched.

## Stale state and drops (audit notes)

* A renderer whose device creation throws never gets `GpuReleased`; a new renderer may then reuse its address. The tap keeps a
  generation per owner (bumped when the Host's store registration changes); module state records the generation it was built
  for and is rebuilt (lifecycle or the next frame) when it differs. The tap sink is registered again whenever it is missing.
* The seal hooks of a rebuilt state are appended to the seal again (a `FrameSeal` cannot drop them). The old hooks hold weak
  references to the discarded state and do nothing; the cost is a few idle closures per address reuse.
* Backlog-flush drops (`OnQueue(Drop)`): the call site passes the surviving sequence and the number of frames dropped before
  it, so the dropped sequences are taken as `[seq - n, seq)` (consecutive). That assumption is counted and shown
  (`dropped frame(s) were taken as consecutive sequences`); at most 16 per event are applied, the rest are counted as lost.
