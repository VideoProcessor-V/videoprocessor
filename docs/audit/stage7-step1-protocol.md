# Audit — Stage 7 step 1: projector protocol layer

Diff audited: 5dcfc088..b3c77214 (0c911e97, b38c8355, b3c77214)
Build: OK — full x64 Release /t:Rebuild of VideoProcessor.sln at HEAD 3542215b, 0 errors, no warnings in audited code (MagewellSdkRoot set to the installed MWCapture SDK 3.3.1.1596; it is not set by default on this PC).
Tests: native suite (vstest, VideoProcessor-Test.dll) 2042 passed, 1 failed (ShellStylePlacementWithoutSizingHintsIsNotReverted — known environmental); VideoProcessorConfigTests 87 passed, 2 failed (the two known Config-app RPC tests). One run at HEAD, covering all steps.

Spec used: guide §2 layer 3, §5 (as at b38c8355), answers-1 §1, §6, §11;
README §5 step 1. Later steps (step 5 serial pacing) replaced the pacing; the
items below are judged at HEAD where they still apply.

## Blockers
- None.

## Should fix
- led/BarcoAsciiDriver.cpp `Worker` vs `SetLightNow` (still at HEAD,
  ~line 365 and ~line 300) — the exit restore can be overtaken. The worker
  takes a (dimmer) target under `stateMutex`, releases it, then waits for
  `portMutex`. If the exit path's `SetLightNow(led_ceiling)` gets the port in
  that gap, the full-light command goes out first and the stale dim target
  right after it; `Close()` comes later. The projector is then left dim
  until VP next starts. Answers-1 §6 (restore on clean exit/disable). Rare
  (very short window) but cheap to close: stop the worker (or make it drop
  its target once stopping) before the restore.
- Same path — the restore is not strictly "≤ 1 s" (answers-1 §6). It waits
  for any command the worker is in the middle of (write timeout 500 ms, plus
  up to `ack_timeout_ms` with read-back on; `ack_timeout_ms` accepts up to
  5000) before its own bounded 1 s. With defaults the worst case is about
  1.1 s (1.6 s if a write stalls); with a large `ack_timeout_ms` it is
  several seconds. Either cap `ack_timeout_ms` or abort the in-flight wait
  on exit.

## Notes
- docs/cinema/VideoProcessor-test.cfg `terminator` comment: "auto tries "",
  then "cr"" — means a bare carriage return, then literal `cr` + CR. Reads
  oddly for the owner.
- With `read_back: off` the driver clamps to 0–255, not to
  `led_floor`..`led_ceiling` as guide §5 says. Harmless today because both
  controllers already keep their targets inside the floors and ceiling.
- `min_command_gap_ms` was 100 at this step (guide then); step 5 made it a
  hard 200 ms floor — see stage7-step5b-serial.md.

## Verified OK
- No transmit path for `protocol: none`, `enabled: false` or
  `mode: log_only`: `EffectiveProtocol` returns "none", which builds the
  dry-run driver that never opens a port (test
  NoneProtocolLogOnlyAndDisabledNeverOpenAPort). A port that fails to open
  sends nothing and the module continues as a dry run.
- `SerialPort` (Win32: `CreateFile \\.\COMn`, `SetCommState` 8N1 no flow
  control, `SetCommTimeouts`) is a separate class from `BarcoAsciiDriver`.
- Reply parsing `%001 MNEM 000nnn` (strict; mnemonic matched case-insensitive);
  `ack_timeout_ms` honoured; timeouts counted and logged.
- Coalescing: a newer target replaces one not yet sent (test
  NewerTargetsReplaceOnesNotYetSent).
- Every outgoing level clamped to the reported range or the light_min/max
  overrides.
- Startup (read-back on): ECHO on, terminator probe `\r` then `cr\r` (logged),
  `?N`/`?M`, read back `LPW1` and `CNTR`; light to ceiling; CNTR set to 50
  only if not already 50 and never written again (test
  ContrastIsOnlyWrittenAtStartupAndOnlyWhenNot50). A silent projector falls
  back to the script's exact bytes, unacknowledged.
- Driver runs its own thread; the controller never sees the port.
