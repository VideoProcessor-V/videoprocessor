# Plan 19 phase 2: vector engine optimisations built on the camera model core

Opus planner, 2026-10-31. Phase 1 (this branch) delivered only the data and CPU side:
`CameraModel::Fitter`, `CameraMotion` in the frame analysis store (`kCamera`), the accessor
(`CameraAccessor.h`) and `BuildHints` (`GlobalModelSearchHints.h`). Nothing is wired yet, and
the vector engine behaves exactly as before. Phase 2 wires the items below into the VECTOR
ENGINE (the motion engine; code identifiers are unchanged).

Rules that apply to every item:
- Each item has its own internal test switch, listed in the diagnostics section.
- When the switch is off, or the record is absent or invalid, the result is byte-identical to
  today's search.
- Every automatic downgrade is visible (a status line or a diagnostic counter), never only in
  the log.
- Measurement-side changes use the PREVIOUS pair's causal fit (plan §3), never the queued
  smoother, so that no feedback loop runs through the search.

| # | Optimisation | Core API already in place | Phase-2 switch (internal) | Escalation / fallback |
|---|---|---|---|---|
| 1 | **Cuts**: no matcher search across a cut; the new scene is seeded from the first post-cut pair in the queue | `PairInput.cut` + `next` → record `PostCutSeeded` / `seededFromNext`; hints `SceneAction::SkipAcrossCut`, caller sets `SeedFromNextPair` | `vector_cut_skip` | Without a queued next pair: Reason `Cut`, the consumers hold (today's behaviour). The first post-cut pair is fitted with a full search, with no H0. |
| 2 | **Fades**: brightness-normalised matching (zero-mean SAD / NCC, gain-offset compensated), with the camera fit kept valid through the fade | `PairInput.fade`, `fadeGain/fadeOffset` (from the store's luma means) copied to the record; the fit is never forced to 0 in a fade (`inFade`); hints `SceneAction::FadeNormalised` | `vector_fade_normalised` | If the normalised cost fails verification, the block falls back to the plain SAD search. Confidence still follows the signals. |
| 3 | **Flashes**: the outlier frame is bridged | `PairInput.flash` + `next` → `FlashBridged`, bridged = an interpolation of the pre-flash model and next's fit, confidence = min of both sides, and the temporal state is untouched; hints `SceneAction::BridgeFlash` | `vector_flash_bridge` | Without `next` or a pre-flash model: Reason `Flash`, confidence 0, the consumers hold. |
| 4 | **Many objects**: a per-block temporal seed (the previous vector, time-scaled to the next pair) gives a narrow window when it agrees with the NVOF seed | `BuildHints(... prevFwd, nvof, nextPairSeconds ...)` → `NarrowAroundPrediction` with `seedX/Y`, `narrowRadiusPx = narrowRadiusSigmas·precision` (eligible: Camera blocks, or Object blocks with a tracked object model, and confidence ≥ `minConfidence`) | `vector_temporal_seed` | **Any narrow block that fails verification is re-searched with today's full search** in the same frame, so an object entering a camera region is never missed. Counters: narrow / escalated / full. |
| 5 | **Many objects**: sequential RANSAC for 1-3 object models with persistent ids | `Config::objects`, `maxObjects`, `objectMinShare`; `CameraMotion::objects`, `BlockData::objectId` | `camera_model_objects` (default decided after validation) | Off → camera model only. An object below `objectMinShare` of picture blocks is not modelled, and its blocks keep full search. |
| 6 | **Small fast objects** (ball, puck): the per-pixel residual after the camera warp flags them as `Object`, never `Camera`; a finer grid or NVOF ROI is used there | `PairInput.pixelResidual` (GPU in phase 2; CPU reference and shader spec in `PixelResidualReference`), `BlockData::tinyMover`, hints `FinerGrid` + 4-connected `rois` | `vector_tiny_mover_roi` | No GPU residual available → no tiny-mover flag, and those blocks keep today's search (they are never relabelled Camera by the model). If the ROI budget is exceeded, use full search in raster order and show a counter. |
| 7 | **Model as D1's global candidate**, guided first candidate, E1 second sweep skip, E4 static gate (plan §3) | `CameraMotion.params/confidence`, `BlockData::modelX/Y` | one switch per §3 row | Confidence below the threshold → today's D1 / E1 / E4. |

## Phase-1 cost (CPU, laptop) and phase-2 targets
The planner hand-back records the fitter cost per step. Remaining CPU budget: typical ≤ 0.5 ms
and worst ≤ 5 ms at 4K (plan §0). If per-block label writing dominates at 8K, phase 2 moves the
label and model-vector pass to the GPU (the same branch order as the README shader-mirror
spec), and the CPU keeps only sampling and fitting.

## Not in phase 2
Confidence coefficients fitted from the stored RAFT results (validation, §7), user keys (step
5), and consumers (step 4).
