# Audit — LED additions: custom-dim fade to black, reactive floor, cut jumps, bright-cut dump

Diff audited: f38ba56e..706f86f0 (f00fe36a, 64f990bd, 73262733, a9bc8fbc,
091cadd6, 647ad0d4, 06970f83, 706f86f0)
Build: OK — full x64 Release /t:Rebuild of VideoProcessor.sln at HEAD 2d3bf2d3, 0 errors, no new warnings (MagewellSdkRoot set to the installed MWCapture SDK 3.3.1.1596).
Tests: native suite (vstest) 2056 passed, 1 failed (ShellStylePlacementWithoutSizingHintsIsNotReverted — known); VideoProcessorConfigTests 91 passed, 2 failed (the two known RPC tests). One run at HEAD, covering all three units.

Spec used: brief Open questions entries "Fade to black below LPW1 60",
"Reactive vs look-ahead", "Light-meter data" (owner 2026-10-10),
LED-PROFILE-GUIDE.md "Fade to black below 60", README §4, owner check (c).

## Blockers
- led/LedDriveModule.cpp `Reconfigure` (~line 165) with the custom dim
  engaged — switching `mode` from compensate to script in the Config app
  while the custom dim is on (a black stretch, LEDs at custom level 0) leaves
  the projector in custom dim. The fade-to-black state machine only runs in
  compensate mode, so nothing ever sends `LDMM 0`; the screen stays dark
  while the picture plays, until VP exits. Meanwhile script mode sends LPW1
  commands (~line 616, no custom-dim check) — breaking "no LPW1 while custom
  dim is on" (owner check c). Fix: on any mode change away from compensate
  (and whenever the experiment is switched off), queue `LDMM 0` and return to
  the normal state before anything else is sent.

## Should fix
- led/ProjectorDriver.cpp `BringToKnownState` and LedDriveModule.cpp `Run`
  (exit) — `LDMM 0` is sent whenever `ftb_custom_dim` is not `off`, in any
  mode, so `mode: script` with the experiment ticked sends LDMM at start and
  exit. Owner check (c) allows LDMM only with the experiment on, enabled and
  compensate. Harmless in effect (it selects the standard mode), but either
  limit it to compensate or have the owner accept "reset is always sent when
  configured". (log_only uses the dry-run driver, which sends nothing.)
- Tests do not cover: a mode switch while the custom dim is engaged; script
  mode with the experiment configured; exit with read-back on during the
  dim-down.

## Notes
- Exit path: the exit `LDMM 0` waits at most 200 ms for the port. With
  `read_back: on` a custom-dim command in flight could hold it for up to
  `ack_timeout_ms`, but those commands are sent "abortable": the reply wait
  breaks within ~50 ms once shutdown starts, so the exit gets the port in
  time. Only a stalled serial write (500 ms write timeout) could still make
  it skip.
- Cut jump timing: the jump is sent the first frame a cut enters the window
  `led_latency_ms` ahead, so it is timed to land on the cut frame — unless the
  driver is still inside its 200 ms command gap, in which case it lands up to
  ~200 ms late (the gain still follows the real send, so the picture stays
  right; only the floor moves late).
- Cut jumps (both directions) and the bright-cut dump are sent whole, past
  `max_levels_per_command`. That follows the owner's 2026-10-10 decision
  (supersedes delta-1 answer 3 for brightening); dimming jumps also bypass
  the 12-level cap, which the brief entry implies ("one jump").
- `led_floor_reactive` only raises the floor (the higher of the two applies);
  setting it below `led_floor` does nothing.
- `[queue.Low_Latency] active_picture_lookahead_frames: 0` is a new line in
  the Low_Latency section (owner-recorded in the brief); frame counts unchanged.

## Verified OK
- (c) Entry into the custom dim needs `ftb_custom_dim` ≠ off, compensate mode
  (the step only runs while compensating), a projector really being driven
  (`applyGain`: enabled, not log_only, port open), look-ahead covering the
  LED's latency, no light in the queue, black for `ftb_black_ms`, the LED at
  `led_floor_black`, and an idle driver. Disabled / log_only use the dry-run
  driver, whose `SendNow`/`QueueSequence` send nothing.
- (c) While Deep, controller LPW1 commands are suppressed; leaving queues
  `LDMM 0` ahead of any LPW1 (the sequence has priority in the driver
  thread), then the LED jumps to the coming scene's level. Exit/disable/
  switch to log_only: `BeginShutdown` drops queued commands, then `LDMM 0`,
  then LPW1 ceiling (see Notes, exit path). Start-up sends `LDMM 0`
  before anything else when the experiment is configured.
- Sequence: colours preset to `ftb_custom_start`, `LDMM 1`, then down to
  `ftb_custom_floor` together or in turns of `ftb_turn_step`, one command per
  `ftb_step_ms`; validation keeps floor < start, ranges 0–39 / 1–40.
- Reactive floor: `led_floor_reactive` applies whenever the look-ahead does
  not reach the frame the LED lands on (ReactiveModeUsesItsOwnFloor).
- Seen cuts jump; unseen dark-to-bright cuts go straight up
  (SeenCutsJumpAndUnseenBrightCutsDump, CutJumpsAreNotSplitByTheDimmingCap).
- Config app: experimental box off by default and greyed until ticked; keys
  `ftb_custom_dim`, `ftb_step_ms`, `ftb_turn_step`, `ftb_custom_start`,
  `ftb_custom_floor`, `led_floor_reactive`, and `led_latency_ms` (written to
  `[projector.Default]`) — all keys VP reads; floor box limited below start.
- (b) still holds: the new controller code uses the same configured
  `compensation_eotf`; the light-meter doc does not change the value (owner
  keeps 2.2; SF-1 open pending measurement).
- No renderer change in this unit.

## Re-audit (548b4a5a, diff 2d3bf2d3..548b4a5a)

Build: OK — full x64 Release /t:Rebuild at 548b4a5a, 0 errors.
Tests: native 2057 passed, 2 failed (ShellStylePlacementWithoutSizingHintsIsNotReverted,
SameSizeEditWithRestoredWriteTimeInvalidatesCache — both known); VideoProcessorConfigTests
91 passed, 2 failed (the two known RPC tests).

- Blocker, mode change during the custom dim leaves the screen dark —
  **resolved.** `Reconfigure` now leaves the custom dim when the module stops
  compensating, stops driving the projector, or the experiment is switched
  off; script mode also refuses to send LPW1 while it is engaged.
- Should fix, LDMM 0 at start and exit in script mode — **disputed-and-I-agree.**
  Kept on purpose as a safety reset (standard dim mode), so a crash in a
  compensate session can never leave a later session dark; custom-dim levels
  (LDC*, LDMM 1) are still only sent in compensate mode (new test
  ScriptModeWithTheExperimentConfiguredSendsNoCustomDim).
- Should fix, missing tests — **resolved** for the mode switch and script
  mode (two new tests); exit with read-back on during the dim-down has no
  test, which I accept because the earlier "abortable wait" note shows it is
  safe.
- Notes — unchanged; no action was needed.

Owner check (3) **OK.** On a mode change while the custom dim is on,
`LDMM 0` is queued with `QueueSequenceNow`; the driver always sends queued
sequence commands before any LPW1, so it goes first. If the dim was already
leaving, its `LDMM 0` is already in that same priority queue. Test
SwitchingModeDuringTheCustomDimLeavesItAtOnce checks that `LDMM 0` is the
first command after the switch and that no custom-dim levels follow.
Owner check (4) **OK.** No change to frame pacing or the queue in this unit.
