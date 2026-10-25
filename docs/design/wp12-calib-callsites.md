# WP-12: calibration call sites in the renderer host (hand-off for the integrator)

WP-12 of the integration batch (docs/design/integration-batch-plan.md, branch
docs/integration-batch-plan) moves VP's calibration glue out of `LibplaceboVideoRenderer.cpp`
into one adapter in the module, `src/VP-V/modules/output/calibration_lut/CalibrationLutHost.{h,cpp}`
(decisions in `CalibrationLutHostPolicy.h`, CPU tests in `tests/CalibrationLutHostTests.cpp`).
Rule: host = call sites only. The renderer keeps one call per hook point; this patch is that
renderer side. **VP-only** (not upstreamable).

## Apply

```
git apply docs/design/wp12-calib-callsites.patch
```

Generated against `feat/wp12-calib-glue`'s base (`chore/early-migrations` ac49a1de, renderer =
`feat/skeleton` fd60b650); `git apply --check --cached` passes on this branch. On a later renderer
use `git apply -3` or apply the hunks by hand (each is self-contained, listed below). It needs this
branch's module files and the two renderer project entries (`CalibrationLutHost.cpp` in
`VideoProcessor-VPRenderer.vcxproj` and `VideoProcessor-VPRendererBeta.vcxproj`, committed here).

## What moves (host hunks)

| Hook point (function / anchor) | Before | After |
|---|---|---|
| includes | `CalibrationLutStage.h` | `CalibrationLutHost.h` |
| `Impl` members | `calibrationLutStage`, `calibrationLutStageLogged`, `calibrationLutStageFailed`; the pattern state (mutex, request, generations, surface size, target transfer, patch texture, log flags, output cache, readback words, tone samples, sent HDR) | one `CalibrationLutHost::Adapter calibrationHost` |
| `displayLutObserved*`, `displayLutNextVersionCheckTick` (VP's automatic LUT reload) | members + 4 lines in `LoadDisplayLut` | `calibrationHost.NoteLutFileVersion(...)` |
| `MaybeReloadDisplayLut` | probe schedule + version compare | `if (calibrationHost.LutReloadDue(...)) LoadDisplayLut(...)` |
| GPU release | `pl_tex_destroy(calibrationPatchTexture)`, `calibrationLutStage.Release()` | `calibrationHost.ReleaseGpu(gpu)` |
| `ReadBackCalibrationPattern`, `FillCalibrationPattern`, `PresentCalibrationPattern` | ~190 lines | `PresentCalibrationPattern` = output check + `calibrationHost.Present({...})` |
| `ConfigureDisplayLutForTarget` (Bill's function) | VP-stage block inside it | one `calibrationHost.TakeLut(...)` before his libplacebo branch |
| target set-up | `calibrationTargetTransfer.store(...)` | `calibrationHost.NoteVideoTarget(...)` |
| HDR verify tone curve | 16 lines | `calibrationHost.NoteToneCurve(...)` |
| PRE_OUTPUT hook push | `calibrationLutStage.Lut()/Get()` | `calibrationHost.HookForFrame()` (same position, same slot tag) |
| after `pl_renderer_get_errors` | stage failure scan | `calibrationHost.AfterRender(hookErrors)` -> `EngageFallback` |
| after the render | `calibrationLutAttached = rendered && targetLutApplied` | `calibrationHost.LutAttached(rendered, targetLutApplied)` |
| `GetCalibrationOutput` | format derivation, cache, generations, readback, tone copy | try-lock loop publishes `OutputFacts`; `calibrationHost.FillOutputInfo(...)` |
| `SetCalibrationPattern` | request under the mutex; log-flag resets; end | `SetPattern`, `ResetPatternLogs`, `EndPattern` |

Byte identity: the picture path is unchanged (same hook, same position and tag, same stage choice,
same LUT). Status and log text are unchanged in this version.

## Bill's code (untouched)

`LibplaceboDisplayLut.*`, `LibplaceboCalibrationLutPolicy.h` and the renderer's `LoadDisplayLut`,
`DetachDisplayLut`, `RejectDisplayLutAfterRenderFailure` and the libplacebo branch of
`ConfigureDisplayLutForTarget` stay as they are (cluster 14, Bill's). The patch only removes VP
lines from inside them and adds one call each.

## The segment runner (feat/segment-runner, WP-6)

S2 ends with the VP calibration LUT and holds the stage object by reference. With this patch the
stage lives in the adapter: in `segments-callsites.patch` replace `calibrationLutStage` with
`calibrationHost.Stage()` (the `OutputColourSegment` constructor) and `calibrationLutStage.Lut()`
with `calibrationHost.Stage().Lut()`; S2's hook push replaces hunk "PRE_OUTPUT hook push" above.
`AfterRender` and `LutAttached` read the same stage, so they keep working when S2 pushes the hook.

## Bill's explicit-Apply LUT reload (arrives with the Bill merge, WP-21)

Bill's beta reads LUT files once per Config Apply (`LibplaceboDisplayLut::AppliedCache`,
`ApplyPendingDisplayLutReloadLocked`); VP probes the file once a second. Either/or (owner rule):
`CalibrationLutHost::ReloadPolicy` has both modes (`Auto` = VP, today's default; `ExplicitApply` =
Bill's) and `RequestCalibrationReload()` (owner decision 4: VP's calibration loads the LUT it has
just written without a Config Apply, with a notice). At the Bill merge:

- keep his `LoadDisplayLut` / cache; keep the single `NoteLutFileVersion` call after his load;
- `MaybeReloadDisplayLut`'s `LutReloadDue(...)` true -> `displayLutCache.Clear(); LoadDisplayLut(activeSettings, true);`
  (his cache would otherwise return the old bytes);
- read the planned key `calibration_lut_reload` (`auto` | `apply`; Original pathway = `apply`) in
  the VP settings readers (WP-18) and call `calibrationHost.Reload().SetMode(...)`;
- on `WM_MESSAGE_CALIBRATION_APPLIED` in apply mode, call `Reload().RequestCalibrationReload()`
  through the renderer and show `ReloadPolicy::CalibrationReloadNotice()` once;
- his `SetDisplayLutStatus` publishes the OSD telemetry: after `TakeLut`, call it when
  `TakeResult::statusChanged` is set.

## Not covered here (GPU, for later)

No GPU test was run (WP-12 rule). Listed for the coordinator: the calibration harness case (patterns
present/readback through the adapter), the `calibration_lut_stage = vp` GPU case, byte identity
frozen-b3base with the patch applied.
