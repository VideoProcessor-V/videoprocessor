# Integration: MPC renderer filter + report-a-problem hotkey (integ/mpc-state)

Date: 2026-10-17/18. Local only (GitHub account suspended; no remote calls).
Worktree `D:\codex\videoprocessor\integ-mpc`, branch `integ/mpc-state`, from local `integ/2026-10-17` at `93369c39`.

## Merges

1. `feat/state-report` @ `4cfe0a0c` -> `5ccdc74b`. Clean (its base was integ's `0642f68c`). Plugin API 36.
2. `feat/vp-renderer-filter` @ `ef0f0a90` -> `0982ab82`. Merge base `bc0ed443` (2f2474f3 + ci/github-actions).
   Nine files conflicted, one hunk each:
   - `IRenderer.h`: kept `WriteDiagnosticHistory` (36) and put the four file-frame methods after it (37) in the vtable.
   - `LibplaceboRendererPluginApi.h`: version 37, history comment for 35/36/37 and the intent-remainder note.
   - `LibplaceboPluginExports.cpp`: both includes (build identity + LED hold-over).
   - `LibplaceboVideoRenderer.cpp` `SetState`: state-report decision row and the file slot-gate notify.
   - `RendererProfileConfig.h`, `UnifiedProfileRuntime.cpp`: rule variables keep integ's `hdr_inferred` and add `source`.
   - `VideoProcessorDlg.cpp`: both include sets.
   - `ConfigFileTests.cpp`, `LibplaceboPluginProxyTests.cpp`: both sides' tests, API pins to 37.
   - Auto-merged and checked: the LED module. `UpdateLedDrive` uses integ's `ResolveLedDriveSection` (Dynamic Drive
     rename, `[dynamic_drive*]` alias). The LED profile file is part of the hold-over signature, so a held drive is
     adopted only with identical projector/LED/profile settings. New drives use `ArbitratedSerialPortFactory`
     (COM-port mutex). The destructor parks the drive only when `source=file`. `VideoProcessorLibplaceboEndHeldLedDrive`
     (player-close guard) is still exported. The CI workflow is integ's: the filter branch did not change it after its base.
   - Hold-over Config control and the cfg comment use "Dynamic Drive" wording.

## API numbering (final)

| API | Change |
|---|---|
| 35 | integ: required build-ID export (hunt 2) |
| 36 | state report: `IVideoRenderer::WriteDiagnosticHistory` |
| 37 | MPC filter: NV12/P010/P016, `FileFrameTiming`, 4 file-frame methods (claimed 35 on its branch) |

- **feat/intent-remainder also claims 36. It must become 38 when it merges.**
- The exe proxy uses the constant. The filter's own check (`VPRendererFilterPluginCheck`) now also applies the
  executable's build-ID pair rule (`BuildIdMismatch`, checked after the API version and before the flavor; new test
  `RefusesAnotherBuildBeforeTheFlavor`).
- The API pin tests (ConfigFileTests, proxy, `FileFrameTests::PluginApiVersionIs37`, filter check tests) expect 37.
- Comments now say "ABI 37".
- `tools/test_state_report_onscreen.ps1` expects plugin_api 37.
- The design doc carries the renumbering note.

## Fixes on this branch

- `f27bb0ff`: the filter test's probe constant was not applied (C2065). Added the build-ID refusal test.
- `ac140382`: `FileFrameTests::PluginApiVersionIs35` still pinned 35. It was missed in the renumbering; it now pins 37.

## Verification (x64 Release, MagewellSdkRoot set, /m:4 CL_MPCount=2)

- **Full solution build:** succeeded. Final build ID `68bca2df401d706278d09fae309e884a`. No LNK1103/1318.
- **CPU suite** (single vstest process, RendererCaptureHarnessTests excluded, TRX `D:\codex\videoprocessor\integ-mpc-trx\cpu.trx`):
  3436 run, 3430 passed, 6 failed.
  - `PluginApiVersionIs35`: a real failure, fixed in `ac140382`.
  - Five failures were load-sensitive under heavy machine load (other agents): `DeckLinkConvertersSustained4K60PerformanceComparison`,
    `CDeckLinkRGBToP010VideoFrameFormatter4KSmokeTest`, `ReadBackOffOnlySendsTheScriptsBytes`,
    `WithoutLookaheadTheGainLandsLedLatencyAfterTheCommand` and `WithLookaheadTheCommandPrecedesItsFrameByLedLatency`.
    None of them touches merged code.
  - Rerun on the final build (`rerun.trx`): FileFrame*, proxy, VPRendererFilter*, LED delta-1/protocol/hold-over/arbiter,
    the API pin and both perf tests. **182/182 passed.**
- **Config tests**, off screen, 1 shard: 143 passed, 1 failed, 1 skipped (needs a real window).
  - The failure, `remote-only client opens its sole discovered VP` (RPC timing under load), passed on rerun (2/2, GATE PASSED).
- **`tools/test_build_identity.ps1`:** 28/28. The EXE and both renderer DLLs carry one ID.

## GPU / on-screen (not run; for Claude)

- `VPRendererFilterGraphTests.FileSourceLavVpRendererPlaysSeeksStepsAndStops`. Set `VP_TEST_GPU=hardware` and
  `VP_TEST_FILTER_CLIP=<24p HDR10 clip>`, with LAV installed. It logged SKIP-NO-GPU here.
- `tools\test_state_report_onscreen.ps1 -Run -VpRoot <deploy>`. It now expects plugin_api 37.
