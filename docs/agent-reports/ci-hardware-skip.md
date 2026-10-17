# CI: hardware skips, sharding, tiers, caching (2026-10-17)

Branch `ci/hardware-skip` on vmcpherson2012-dev/videoprocessor (from `ci/github-actions`).

## Status: incomplete, GitHub account suspended

GitHub answered "Your account was suspended" (HTTP 403) to every API call from 21:25 UTC
on, while run 39070299029 (the first run with tiers and the build cache) was in its test
shards. All GitHub work stopped there: nothing more was polled, pushed or cleaned up. This
report was committed locally and **not pushed**. The account owner has to resolve the
suspension before anyone continues.

## Changes

- `src/VideoProcessor-Test/HardwarePrerequisites.h` (new): `RequireConditionOrSkip`
  logs `SKIP-NO-HARDWARE <test>: <reason>` and returns, or fails the test when
  `VP_TEST_REQUIRE_HARDWARE=1`. The checks measure the real conditions:
  `DesktopFitsWindow` (SM_CX/CYMAXTRACK), `PerformanceCpu` (at least 8 logical
  processors), `HardwareD3D11Renderer` (a hardware D3D11 device at 10_0 or above, not
  WARP or the Basic Render Driver).
- The four tests call it as their first statement: `WindowResizeLayoutTests.cpp`
  (ResizePreservesRequestedWindowBounds, LayoutCompletesBeforeRendererNotification),
  `VideoFrameFormatterTests.cpp` (DeckLinkConvertersSustained4K60PerformanceComparison),
  `LibplaceboPluginProxyTests.cpp` (RendererMetadataCyclesReuseAcceptedSnapshotWithoutDiskReads).
- Fail closed locally: `tools/run_chain_gate.ps1` and
  `.agents/skills/vp-release/scripts/build-vp.ps1` set `VP_TEST_REQUIRE_HARDWARE=1`, and the
  chain gate fails on a `SKIP-NO-HARDWARE` marker the same way it fails on `SKIP-NO-GPU`.
- `.github/workflows/ci-windows.yml`: the workflow no longer excludes the four tests. It has
  a build job, a dynamic matrix of up to 6 CPU shards, config tests running in parallel, and
  a test-summary job. It caches v142 ATL/MFC, adds the affected/full tiers, and caches
  build outputs.
- `tools/ci/plan_test_shards.ps1`, `summarize_test_shards.ps1`, `select_ci_scope.ps1`,
  `prepare_incremental_build.ps1`, `test-weights.json`; `docs/CI.md` (tiers, caching,
  push policy for agents).

Local compile check (slot rule): the four changed test files compiled cleanly with MSBuild
`/t:ClCompile`, Release x64, `/p:VpCiNoMagewell=true`. The workstation has no MWCapture SDK
at the documented path.

## Measured runs

| Run | What | Wall-clock | Result |
|---|---|---|---|
| 39059386364 (before) | one job: toolchain 4m50 (ATL install), build 10m05, CPU tests 34m48, config tests 10m44 | **61.4 min** | 4 environment failures |
| 39067065219 | sharded, ATL install (cold), default weights | **32.6 min** | all 3092 tests passed, the 4 tests show `SKIP-NO-HARDWARE`. The summary job failed on a summarizer bug: tests planned one by one were counted as unplanned. Fixed in 6f50adbd |
| 39070299029 | tiers, ATL from cache, cold build cache, measured weights | unknown: interrupted by the suspension | build job finished; shard 4 passed in 3m12; others still running |
| 39071091906 | probe branch `test/ci-affected-probe` (affected tier, cold) | unknown | was in Toolchain |

Run 39067065219 jobs: build 16.7 min (about 5 min of it the ATL install). Shards' vstest
times were 7.3, 2.8, 2.8, 9.4, 4.2 and 2.5 min, unbalanced because that run planned with
default weights. Config tests took 3.9 min, test-summary 0.5 min. Measured weights put the
heaviest test (WholeHookChainCombinationMatrix, 378 s) in its own shard, with the other
five at about 348 s each. That points to roughly 20 min for a full run with a warm ATL
cache. This has not been measured.

## Recommendation: low-concurrency CI after reinstatement

Before the suspension, about 10 agent branches were pushing at the same time (fix/hunt2-*,
including -a/-b/-c sub-branches, plus feat/vp-renderer-filter-a..f). Every push started a
Windows run, and this branch's runs fanned out into 8 jobs each. That load was probably a
factor in the suspension. Proposed for `.github/workflows/ci-windows.yml`:

1. **Triggers:** run automatically on push to `ci/*` only. Feature branches use
   `workflow_dispatch`, started by the coordinator when a branch is ready, never by
   sub-agents. Agent sub-branches (`-a`, `-b`, ...) and `fix/*`/`feat/*` pushes start
   nothing.
2. **Workflow-level concurrency:** `group: ci-windows` (one group for the whole
   repository), `cancel-in-progress: false`. At most one run executes and at most one
   waits; GitHub replaces an older pending run with a newer one.
3. **Concurrent job cap:** `strategy.max-parallel: 3` on the shard matrix, and at most
   3 shards (`TEST_SHARDS: 3`): about 12 min of tests instead of 6. This caps any moment
   at about 4 Windows jobs.
4. **Full runs by manual dispatch only** (`full: true`). Drop the nightly `schedule`
   added here until CI has been stable for a while. Keep the affected tier as the default
   for dispatched feature runs.
5. **API polling:** one coordinator polls, with `gh run watch -i 300`. Agents do not poll
   at all; they read the result once the run has finished.

## Open items

- Confirm a fully green full-tier run, then time the affected tier: push the probe branch
  twice, once cold and once warm. Delete `test/ci-affected-probe` on the fork afterwards.
- Caches are scoped by branch. Feature branches only get warm caches once the workflow is
  on the fork's default branch (`v1.4.02-beta`), where the nightly schedule keeps them warm.
- The build-output cache, the scope selection on the runner and the dynamic matrix have not
  been confirmed on GitHub yet.
