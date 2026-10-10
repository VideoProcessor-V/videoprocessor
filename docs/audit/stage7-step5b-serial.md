# Audit — Stage 7 step 5: read-back toggle and command pacing

Diff audited: f88e15d4..f7814c07
Build: OK — full x64 Release /t:Rebuild of VideoProcessor.sln at HEAD 3542215b, 0 errors, no warnings in audited code (MagewellSdkRoot set to the installed MWCapture SDK 3.3.1.1596; it is not set by default on this PC).
Tests: native suite (vstest, VideoProcessor-Test.dll) 2042 passed, 1 failed (ShellStylePlacementWithoutSizingHintsIsNotReverted — known environmental); VideoProcessorConfigTests 87 passed, 2 failed (the two known Config-app RPC tests). One run at HEAD, covering all steps.

Spec used: owner's serial decisions in guide §5 (and brief Open questions,
"Real sustainable serial command rate"). The `max_levels_per_command` part
of those decisions arrived in step 10 and is checked there.

## Blockers
- None.

## Should fix
- Exit restore can be overtaken by an in-flight dim command, and is not
  strictly within 1 s — see stage7-step1-protocol.md (the worker/exit
  interaction in BarcoAsciiDriver.cpp is unchanged by this step).

## Notes
- led/BarcoAsciiDriver.cpp `SelectFraming` — with `read_back: off` and
  `terminator: auto` the driver sends the script's exact bytes (`:lpw1255cr`
  + CR LF, lower case). That is what guide §5 asks for ("the script's exact
  bytes"); `cr`/`literal_cr` remain selectable.
- With `read_back: off` the "range" is not taken from `led_floor`/`led_ceiling`
  in the driver (it clamps 0–255); the controllers already stay inside them.

## Verified OK
- `min_command_gap_ms` (default 200) is a hard floor for every command:
  startup handshake, startup light/contrast, worker commands. Only the bounded
  exit restore may cut a wait short, and it logs by how much
  (CommandsKeepTheGapAndTheOptionalPause, RestoreOnExitNeverBlocksForLong).
- The worker waits out the gap/pause on a condition variable that `Close()`
  interrupts (ClosingDoesNotWaitOutAPause) and keeps taking newer targets
  meanwhile (coalescing kept).
- `burst_commands` (default 0 = off) / `burst_pause_ms` (5000) implement the
  optional Barco pause.
- `read_back: off` (default): no ECHO, no queries, no waiting; contrast reset
  to 50 blind and light set to the ceiling at startup, in the script's order
  (ReadBackOffOnlySendsTheScriptsBytes). `read_back: on`: ECHO, terminator
  probe, range and state queries, wait up to `ack_timeout_ms`, and at least
  2 s before the next command after a missing reply; timeouts logged and
  counted (MissingReplyIsCountedAndTheDriverCarriesOn).
- The modelled LED level follows the levels the driver actually wrote,
  counted in the frame of the write (TheGainFollowsOnlyTheLevelsActuallySent),
  so the gain steps with each real LED step.
- Test config documents all keys with the owner's defaults (200 ms, 0 burst,
  `read_back: off`).
