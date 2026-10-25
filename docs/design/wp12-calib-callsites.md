# WP-12: calibration call sites in the renderer host (hand-off for the integrator)

WP-12 of the integration batch (docs/design/integration-batch-plan.md, branch
docs/integration-batch-plan) moves VP's calibration glue out of `LibplaceboVideoRenderer.cpp`
into one adapter in the module, `src/VP-V/modules/output/calibration_lut/CalibrationLutHost.{h,cpp}`
(decisions in `CalibrationLutHostPolicy.h`, CPU tests in `tests/CalibrationLutHostTests.cpp`).
Rule: host = call sites only. The renderer keeps one call per hook point; this patch is that
renderer side. **VP-only** (not upstreamable).

## Apply

```
git show feat/wp12-calib-glue:docs/design/wp12-calib-callsites.patch | git apply -
```

Apply the committed blob (as above), not the working-tree file: with `core.autocrlf=true` a
Windows checkout gives `*.patch` files CRLF line ends and `git apply` then rejects every hunk
(WP-4's `silent-fallbacks-callsites.patch` has the same problem; a repo-wide `*.patch -text` in
`.gitattributes` would fix it for all patch hand-offs).

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
| after the render | `RejectDisplayLutAfterRenderFailure()` only logged; `calibrationLutAttached = rendered && targetLutApplied` | adds `calibrationHost.LutDropped(path)` -> `EngageFallback` (fix); `calibrationHost.LutAttached(rendered, targetLutApplied)` |
| `GetCalibrationOutput` | format derivation, cache, generations, readback, tone copy | try-lock loop publishes `OutputFacts`; `calibrationHost.FillOutputInfo(...)` |
| `SetCalibrationPattern` | request under the mutex; log-flag resets; end | `SetPattern`, `ResetPatternLogs`, `EndPattern`; at the end `ReleaseFallback("calibration patch")` (fix) |
| `VideoProcessorDlg.cpp` `HandleCalibrationRpc` (GUI host) | `hooks.configPath` never set | set to the configuration VP itself resolved, absolute (fix) |

Byte identity: the picture path is unchanged (same hook, same position and tag, same stage choice,
same LUT, same patch codes). The patch is otherwise behaviour-free except for these fixes (module
findings 2026-10-24; the decisions live in the module, CPU-tested):

- the VP-stage LUT status keeps a rejected reload (`Active (VP stage; reload rejected: ...)`) and is
  written only when it changes;
- "calibration LUT attached" counts the VP stage. Its readers, all intended by the module finding:
  the Ctrl+I limited-range text (`DescribeLimitedTransfer`), the "(inactive: calibration LUT)"
  display-gamma marker, `ActiveProfileStatus` / Config's usable-LUT check;
- `calibration LUT (dropped after a render error: ...)` and `calibration patch (full field ...)`
  engage under "Fallbacks engaged" (they were log-only);
- `GetCalibrationOutput` makes one try-lock without sleeping while patterns show and the cache is
  filled (README cost item 1: no 0.2-1.6 s wait per patch; a free renderMutex still refreshes, so a
  swapchain rebuilt mid-run is seen; audit item 3); the presented-generation counters stay live;
- the calibration controller gets the absolute configuration path.

`EngageFallback`/`ReleaseFallback` are this renderer's own. Where WP-4's `SyncFallback(Decision)`
has landed, `if (d.engaged) EngageFallback(d.id.c_str(), d.reason);` may become `SyncFallback(d)`
for `LutDropped` and the patch window, but NOT for `AfterRender` (its "released" answer must not
release the shared `calibration LUT` id).

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

## Interaction with WP-15 (feat/wp15-led-glue, `wp15-led-callsites.patch`)

Both patches edit the `pl_renderer_get_errors` block, `SetCalibrationPattern` (WP-15: the LED
suspend/resume blocks; WP-12: the pattern request, log-flag resets, end flag and the
`calibration patch` release) and the area after the HDR-verify tone curve (WP-15 replaces the inline
LED block that is this patch's trailing context: keep `calibrationHost.NoteToneCurve(...)` just
before `dynamicDrive.PrepareFrame`). The WP-15 auditor measured 3 trivial `--3way` conflicts. Apply WP-15 first, then this patch with `git apply -3` or by hand;
the hunks are independent in meaning. After both, the host's `LedRawTransfer.h` include (kept by
WP-15 only for the tone-curve block) can go: `CalibrationLutHost` owns that block now.
