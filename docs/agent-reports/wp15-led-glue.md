# feat/wp15-led-glue (WP-15: LED / Dynamic Drive glue + paired M2 moves)

Base: chore/early-migrations ac49a1de. Worktree D:\codex\videoprocessor\wp15-led. VP-ONLY throughout.

## Commits

| Commit | What | Kind |
|---|---|---|
| b1fb1c75 | drive.led_measure: FrameStats split, IPredictionSink, LedMeasurementHook moved in | move (behaviour-free) |
| c543f69a | drive.led_compensation: LedController, LedRawTransfer, LedDriveConfig moved in | move (behaviour-free) |
| a5005696 | drive.projector_control: LedDriveArbiter moved in (transport only); path map follows module-to-module moves | move (behaviour-free) + tool |
| daaf1643 | Dynamic Drive host adapter (DynamicDriveHost / Shared / Frame) + 9 CPU tests | extraction target (behaviour-free) |
| 8a00bc9e | fix: compensation-unavailable hold, visible LED fallbacks | change (inert unless a fallback occurs) |
| 899e13cf | README-vs-code pass, key lists, fallback entries, dry-run test | docs/data |
| a80ba3bb, d0b26edb | renderer call-site patch + notes (WP-4 / WP-12 overlap) | docs only |

## State

- Builds: full x64 Release solution green after every commit (logs D:\codex\videoprocessor\wp15-led-logs\build*.log);
  patched renderer (both VPRenderer DLLs) compiled and linked (build7-patched.log), then restored.
- Tests: DynamicDriveAdapterTests 13/13, LedIntentLimitsTests green; filtered CPU suite runs: see the report.
- Forwarding headers left (removed at M2 step 0): led/LedMeasurementHook.h, LedRawTransfer.h, LedDriveConfig.h,
  LedDriveArbiter.h (+ the earlier LedDriveModule.h, LedDriveHoldover.h, SerialPort.h, ConnectionTest.h).
- Next: rebase onto integ/batch1 when the lead gives the hash; regenerate the call-site patch there.

## Open

- GPU checks owed (listed in docs/design/wp15-led-callsites.md).
- VideoProcessorDlg.cpp LED glue (RS-232 test worker, shutdown restore orchestration, Ctrl+I line) left in
  the dialog: GUI lifecycle code, WP-11's area.
- Owner question: should compensate also hold full drive when slot drive.led_measure is switched off?
