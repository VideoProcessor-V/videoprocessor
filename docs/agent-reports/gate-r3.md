# Gate r3 (2026-10-18): round 3 chain-gate failures, efficiency GPU checks

Branch `fix/gate-r3` from `integ/2026-10-17` 17e534c3 (code e824dd4d). Laptop RTX 3050 Ti, GPU exclusive.
The CPU was shared with other agents' builds and tests throughout (about 70 % load).

## 1. StreamFaultsKeepTheDejudderSafeAndRecovering: "the de-judder runs on the film frames"

**The failing case.** It is the 24p-in-29.97 cadence (NNNNR), which is film in this test, not real 30p.
- The lock is found: "Pulldown: 23.976p film in 29.970 Hz (4 new of 5)".
- After the lock the de-judder never estimates. Its status stays "next frame not ready" until teardown.
- "VP motion: ... source_hz=29.97" is libplacebo's temporal mixer, which is off by design here. It is not the cause.

**Cause: the timing merge (74340728), via `fix/hunt2-timing` 52666dd7 (cadence_class_fixes, 06-interp M3).**
`LockedFilmQueueTarget` scales the queue target by new/cycle while locked. Its floor for "A shown and B queued" was 2.
- The harness target of 3 became 2 at 4:5.
- The steady dequeue takes A as soon as the queue reaches its target. B is then always the frame that has
  just arrived, and it is still being converted.
- The motion-ahead floor (look-ahead depth + 1) cannot lift the target, because it needs an estimate that is
  already running. It is a deadlock.

**Evidence.**
- A temporary dequeue log (removed) after the lock printed, at every dequeue:
  `q=1 target=2 ... conv=0` (B queued but not converted), with every other pair condition true.
- With `cadence_class_fixes: off`, the same build passes all three cadences.
- Bisect over the prebuilt binaries:
  - integ-base 1592d39d: passes (2 of 2 runs).
  - fix/hunt2-timing tip 07a1afe2: fails the same assertion.
  - integ e824dd4d: fails (3 of 3 runs).
- The round 3 merges and the prune did not cause it. Neither did hiccup repair: its standalone pairs also need
  a converted B.

**The new behaviour is wrong by the owner's rules.** The de-judder must run on locked film.

**Fix.**
- `PulldownDetector.h:817`: new constant `kLockedQueuePairFrames = 3` (A, a converted B, and the arrival).
  It counts the queue's structure, not a rate.
- `LibplaceboVideoRenderer.cpp:29850`: the locked target's floor is now this constant (it was 2). The target
  is still never raised above the configured target, and deeper targets still scale (12 at 3:2 gives 5).
- `Hunt2CadenceClassTests.cpp:193`: pins the constant and the 24-in-30 and 3:2 cases. 37/37 cadence-class
  and pulldown tests pass.
- The harness report now also prints the "De-judder" and "Interpolation" lines.

## 2. DetailRestoreGpuSourceMonitorUnchanged: "frames presented"

The test setup was at fault, not the shaders.
- `DetailPulseGpuMatchesModel` passes (worst difference 0.0088 codes).
- `TextureRestoreGpuMatchesModel` passes (worst 0.009 codes, 0 px over 0.5).
- The detail test itself passes once the shader cache is warm.
- From a copy of the integ binaries with an empty `vp-harness-shader-cache`, neither run presented anything in
  its 4 s window. A second run passed.
- In the gate, this was the first renderer with the 4K detail shaders, so the "on" run compiled them inside its
  window.

**Fix.**
- `RendererCaptureHarnessTests.cpp:1584`: one warm-up run with the stages on before the two runs that are
  compared. Each "presented" assertion now prints its run's log tail.
- `SourceResolutionMonitor.cpp:406`: tests-only `VP_TEST_SOURCE_MONITOR_DIGEST=1`. It logs an FNV-1a digest of
  the tiles the monitor read back.
- The test now requires one digest across both runs.

**Measured on the GPU:** `tiles 8, tile 256, fnv f151b5fff2130383` with the stages off and on. The monitor's
input is bit-identical, and the status lines are identical too.

## Gate

The full command on `fix/gate-r3`: **205 run, 205 passed** (TRX `%TEMP%\vp-gate-r3-fix2\chain-gate.trx`).

The first full run gave 202/205. The failures were LowLatencyQueueFromTheFirstFrame,
InterpolationRunsAtAMeasuredRateMismatch and DolbyVisionGpuUnpackMatchesTheCpuUnpackThroughTheRenderer.
- None of them involves a pulldown lock.
- All three pass alone, and all pass in the second full run.
- They are load flakes.

The early blocks of the stream-fault test also flaked under load: missed frames not counted, or no estimate in
the drift block. This happened on the base binaries as well.

## Efficiency GPU checks (feat/efficiency b7d09db3; worktree eff-gpu, branch test/eff-gpu de54c404)

All runs used `VP_TEST_GPU=hardware`.

- **UploadReuseRoutesAreBitIdentical:** it failed at "UYVY GPU unpack" because of the test's readback, not the
  unpack.
  - A plane written by the unpack and blitted to a host-readable copy read back stale texels. Both unpacks
    were wrong, and differently.
  - Downloaded directly, both equal the CPU expectation. GpuCaptureUnpackMatchesTheCpuFormatters also passes
    on hardware.
  - Fixed in the test. It now passes.
- **EfficiencyFixesArePixelIdentical:** this found a real bug. `UploadDejudderPlanes` never set
  `Record::sequence`, so `CanReuse` refused every reuse ("layout differs"). efficiency_fixes saved nothing.
  - Fixed at `LibplaceboVideoRenderer.cpp:14405` (eff-gpu): the B and C uploads record their frame.
  - Then the strength-1.0 capture landed on a main present or an extra depending on timing: two codes for the
    same picture in either mode. The test now uses the occlusion mask alone, which estimates without extras.
  - **Pixel-identical: yes.** In every steady run where the reuse engaged, off and on gave the same codes on
    both routes.
  - Under this CPU load some runs wrote no capture, or the reuse did not engage. It stays load-sensitive.
- **EfficiencyStageCostsOnThisGpu** (RTX 3050 Ti laptop; ms per source frame):

| Stage | 3840x2160 | 1920x1080 |
|---|---|---|
| P010 upload (CPU route, saved) | 16.03 | 2.20 |
| P210 upload (CPU route, saved) | 24.81 | 3.42 |
| V210 raw upload + GPU unpack (default route, saved) | 4.41 | 1.11 |
| Render, high quality, no hooks | 6.35 | 1.65 |
| Render with the de-judder cache armed | 7.00 (cache 0.65) | 2.27 (cache 0.62) |
| Re-base 2:1 | 5.94 | 1.70 |
| Source-resolution monitor tile copy (2 Hz) | 2.45 | 1.00 |
