# WP-15: LED / Dynamic Drive call sites (renderer patch)

Integration batch 1, WP-15 (docs/design/integration-batch-plan.md on `docs/integration-batch-plan`).
The LED / Dynamic Drive glue leaves `LibplaceboVideoRenderer.cpp`/`.h` (Bill's files) for the VP-V
host adapter in `src/VP-V/modules/drive/led_compensation/`:

- `DynamicDriveHost` (outer renderer, VideoProcessor-Lib): module lifetime, suspend/resume,
  hold-over, shutdown stop, status line.
- `DynamicDriveShared`: what the host publishes to the render side (module, walk observer, hook
  parameters; atomics, no GPU types).
- `DynamicDriveFrame` (render thread, libplacebo; derives from `DynamicDriveShared`): the per-frame
  set-up of the S2 LED measurement and compensation hooks, the hook errors, the stats collection.

The renderer keeps one call per hook point. This patch is the renderer side; the branch never
commits renderer edits (host = call sites only, integrator applies).

## Apply

```
git apply --check --cached docs/design/wp15-led-callsites.patch   # clean on feat/wp15-led-glue
git apply docs/design/wp15-led-callsites.patch
```

- Generated against `feat/wp15-led-glue` (base `chore/early-migrations` ac49a1de; the renderer files
  are unchanged on this branch). `git apply --check --cached` passes there.
- It needs this branch's adapter (commits "Dynamic Drive host adapter" and the `fix:` after it).
- Verified by building: with the patch applied, VideoProcessor-VPRenderer and
  VideoProcessor-VPRendererBeta compiled and linked (x64 Release, no new warnings; build log
  `build7-patched.log`). The renderer files were then restored.
- On `integ/batch1`, regenerate after the rebase (the agent does this when the lead gives the hash).
  By hand: 23 self-contained hunks, listed below.

## Size

Renderer host: **567 lines out, 85 in** (net -482) across the two files.

| File | Out | In |
|---|---:|---:|
| LibplaceboVideoRenderer.cpp | 540 | 78 |
| LibplaceboVideoRenderer.h | 27 | 7 |

## Hook points

| Renderer site | Before | After |
|---|---|---|
| includes | LedDriveModule / Arbiter / Holdover / LedMeasurementHook / LedRawTransfer (legacy paths), FileHostSettings | `DynamicDriveFrame.h`, `LedRawTransfer.h` (new path; calibration tone curve, WP-12) |
| `Impl` members | `ledDrive`, measurement hook, compensation chain, raw-transfer signature, log latches, 8 parameter atomics | `LedDrive::DynamicDriveFrame dynamicDrive{ queuedFrameObserver, fallbacks, GlslHookLoader() }` after `fallbacks` |
| GPU teardown | `ledMeasurement.Release(); ledCompensation.Clear(); ...Configured = false` | `dynamicDrive.Release();` |
| Intent limits (render thread) | atomic_load + `SetIntentLimits` + log | `LedLimitsDiffer(...) && dynamicDrive.SendIntentLimits(...)` |
| Per-frame S2 LED block (~190 lines) | BeginFrame/BeginFrameAhead, raw transfer, measurement geometry, picture-only masks, NLS mapping, grain map, hook push and tagging, boost warning, 11 hook parameters | fill `DynamicDriveFrameInputs` from the frame's locals (25 assignments) + `dynamicDrive.PrepareFrame(led, frameHooks, tag)` |
| Hook errors | `ledCompensation.MarkRuntimeFailures` + measurement-disabled loop | `dynamicDrive.OnHookErrors(...)` |
| Stats out | Collect + SubmitMeasurement loop | `dynamicDrive.CollectMeasurements();` |
| De-judder extras | `ledMeasurement.Hook()` | `dynamicDrive.MeasurementHookPointer()` |
| Outer header members | 15 `m_led*` members, `UpdateLedDrive`, `m_calibrationLedSuspended` | `LedDrive::DynamicDriveHost m_dynamicDrive{ m_stateMutex }; DynamicDriveContext()` |
| `~LibplaceboVideoRenderer` | hold-over park block | `m_dynamicDrive.ParkForHoldover(DynamicDriveContext());` |
| `Build` | publish module, observer, 8 atomics | `m_dynamicDrive.Attach(DynamicDriveContext());` |
| `UpdateLedDrive` (~120 lines) | the whole function | removed; `DynamicDriveContext()` (frame pointer, projector slot, last render tick) in its place |
| Snapshot apply | hold-over ms, configuration, `UpdateLedDrive` | `LookupVariable("source")` + `m_dynamicDrive.ApplySnapshot(...)` |
| Calibration patterns start / end | suspend block / resume block | `SuspendForCalibration` / `ResumeAfterCalibration` |
| `SuspendLedDriveForTest`, `StopLedDriveForShutdown`, `ResumeLedDriveAfterTest`, `GetLedDriveStatus` | bodies | one forwarding call each (shutdown builds a frame-only context: it never reads `pipelineSlots`) |
| Render loop walk depth | atomic loads + `WantedLookaheadFrames` | `m_impl->dynamicDrive.WantedLookaheadFrames()` |

Kept in the host on purpose: `LedDisplayGamma` (also used by SDR tone shape and DV trims; passed in
as `displayGamma`), `queuedFrameObserver` (the walk's observer slot; the adapter writes it by
reference), `ledIntentLimitsSent` (intent glue, WP-16), the calibration tone-curve block (WP-12).

## Behaviour

- **Byte-identical with the LED off** (no module, no boost, no lift): `PrepareFrame` returns before
  any hook work, as the inline block did. Only the fallback registry is consulted (no-ops).
- **Byte-identical with compensate/script running normally**: same calls in the same order; the
  config is read once per frame instead of four `Config()` copies (same values).
- **Changes, by design (module findings 2026-10-24 items 1, 2, 7; owner rule no silent fallbacks):**
  - compensation unavailable (shader not loaded, disabled by libplacebo, or slot
    `drive.led_compensation` off) -> `SetCompensationAvailable(false, reason)`: compensate holds full
    drive, "Fallbacks engaged: LED drive (held at full: picture compensation unavailable (...))";
    script mode not held;
  - the module's dry-run entry "projector (not connected: ...; LED drive off (dry run))" reaches the
    heading;
  - "gamma boost (not applied: ...)" and "LED measurement (... the LED follows the last statistics)"
    (pass disabled, or the readback ring's late / unmeasured note) are visible, no longer log-only.
- Lock scopes, thread affinity and the order of module stop / publish are as before
  (DynamicDriveHost takes the renderer's `m_stateMutex` by reference).

## Interaction with other patches

- **WP-4 silent-fallbacks patch** (`docs/design/silent-fallbacks-callsites.patch`):
  - its hunk after `ledMeasurement.Collect` (LED measurement `LateNote`) is **superseded**:
    `DynamicDriveFrame::CollectMeasurements` carries it (same id "LED measurement", with hold-off
    so the heading does not flap). Drop that hunk when both are applied.
  - its `Build` hunk anchors on the `m_impl->led*Passthrough.store(...)` lines this patch removes:
    keep its `displayRefreshRate.SetFallbackSink(...)` line right after `m_dynamicDrive.Attach(...)`.
- **WP-6 segments (S2)**: the measure/compensation pixel work stays in their S2 stages;
  `PrepareFrame` is the S2 LED entry point the segment runner can call instead of the host.
- **WP-12** (feat/wp12-calib-glue, wp12-calib-callsites.patch): applied after this patch with
  `--3way`, 3 trivial conflicts (checked 2026-10-25): the tone-curve hunk (keep
  `calibrationHost.NoteToneCurve(...)` before the Dynamic Drive block) and the calibration-pattern
  start/end (keep WP-12's `SetPattern` / `ResetPatternLogs` / `EndPattern` /
  `ReleaseFallback("calibration patch")` around `SuspendForCalibration` / `ResumeAfterCalibration`).
  WP-12 removes `calibrationTone` and the host's LedRawTransfer use; then this patch's
  `LedRawTransfer.h` include line can go too.

## Tests

- CPU: `DynamicDriveAdapterTests` (13), plus the LED classes (`LedIntentLimitsTests`,
  `LedScriptLookaheadTests`, `LedDrive*Tests`, `LedDriveArbiterTests`, `LedDriveHoldoverTests`).
- GPU, owed (coordinator; not run by the agent):
  - `LibplaceboLutParserTests`: `LedMeasurementHook*`, `LedCompensation*`, `LedBoostToeAndShadowLift`,
    `LedHighlightStatisticIgnoresGrain` (WARP; moved files, unchanged code);
  - with the patch applied: renderer harness byte identity LED off (r6 frozen hashes) and with
    compensate + script dry run; forced shader-missing run (LED held, entry shown); renderer restart / rebuild with compensate on: no "held at full" engagement on the first frames (audit I3: the shader must be ready when the first frame's hooks are collected, or the LED jumps to full); BTB/WTW probes
    through the compensation hook (codes 4 and 1019);
  - FL35 dry run / real projector: hold-over between files in the filter, RS-232 test
    suspend/resume, calibration patterns suspend/resume, forced-shutdown restore.

## Upstreamable

No. VP-only (the Dynamic Drive is VP's feature). The patch shrinks VP's footprint in Bill's files.
