# Audit — Stage 7 step 3: measurement pass and renderer integration

Diff audited: 3769fa00..ea11aeb7
Build: OK — full x64 Release /t:Rebuild of VideoProcessor.sln at HEAD 3542215b, 0 errors, no warnings in audited code (MagewellSdkRoot set to the installed MWCapture SDK 3.3.1.1596; it is not set by default on this PC).
Tests: native suite (vstest, VideoProcessor-Test.dll) 2042 passed, 1 failed (ShellStylePlacementWithoutSizingHintsIsNotReverted — known environmental); VideoProcessorConfigTests 87 passed, 2 failed (the two known Config-app RPC tests). One run at HEAD, covering all steps.

Spec used: guide §2 layer 1, answers-1 §4, §5, §7, §8; README §5 step 3.
Later steps changed the module's per-frame calls and added the third call
site; those are judged in the step 5 files.

## Blockers
- None in this step's own scope. The "any LED setting edit restarts the
  module" problem was introduced here (`UpdateLedDrive`) and is reported as a
  blocker in stage7-step4-compensate.md, where the mode-change rule is checked.

## Should fix
- None.

## Notes
- led/LedMeasurement.cpp `StatsBuilder::Build` (exact path) — the GPU pass runs
  on every output refresh, so a 24p film in the 60 Hz cinema queue feeds each
  source frame 2–3 times. Repeated frames show no APL change, so the exact
  path's fade test ("APL fell on each of the last 6 frames") can never fire
  for 24p. `mode: compensate` takes its fade flag from the lookahead (one
  sample per source frame) when predictions exist, so in practice this only
  bites when there are no predictions. Worth deduplicating by sequence.
- led/LedMeasurementHook.cpp — if libplacebo disables the native measurement
  hook at runtime (`failed = true`), nothing logs it and the controller keeps
  using the last statistics it had. Consider logging it like the custom hooks.
- The 3×3 blocks are sampled on a grid of at most 320×180 (not a full tiling);
  fine for a 99.5th percentile, just not literally "every 3×3 block".

## Verified OK
- Measurement is a native `pl_hook` at `PL_HOOK_PRE_OUTPUT` and is placed in
  the per-frame hook list *before* the compensation hook, so statistics see
  the picture before the gain (answers-1 §4). It never writes the image
  (`PL_HOOK_SIG_NONE`). The sigma-delta stays at OUTPUT, after both.
- Active-picture rectangle comes from the fork's trusted active picture
  (`hdrTrustedPicture`), mapped into the displayed crop; whole picture when
  untrusted (test MeasurementFollowsTheTrustedActivePicture). Not a fixed crop.
- Code scale `code = 16 + 219 × value`; `apl_weights` default 0.114, 0.587,
  0.299, configurable, with the "looks backwards" comment in the test config.
- Highlight = configured percentile (99.5) of block means; cut = mean absolute
  block-luma difference > `cut_threshold` (24); fade = APL down by ≥
  `fade_min_step` on each of the last `fade_frames` (6) frames, no cut.
- Async readback: 4-slot ring of storage buffers, `pl_buf_poll` only; a frame
  is skipped (counted) rather than waiting when the ring is full. The render
  thread never blocks.
- Renderer touch points at this step: stats out (Collect/SubmitMeasurement),
  gain in, plus the compensation hook-chain entry — within the guide's limit.
- `log_only` never opens a port and never changes the picture (test
  LogOnlyNeverOpensAPortOrTouchesThePicture); an absent port degrades to a dry
  run.
