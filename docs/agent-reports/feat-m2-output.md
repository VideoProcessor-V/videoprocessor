# feat/m2-output: M2 steps 4/5/8 paired with WP-13 (agent report)

Worktree `D:\codex\videoprocessor\m2-output`, branch `feat/m2-output` from perf/byte-identical
29c3843d + chore/early-migrations ac49a1de (merge d20bc45c). Not pushed.

## Commits

- 0aaaf2e5 M2 step 4: colour.extended_range moved (ExtendedRange.*).
- 2c393254 M2 step 5: output.dither moved (SigmaDeltaDomain.h, OutputEncodingRepair.*, CustomHookChain.*).
- 1f3d6fbc M2 step 8: scaling.output moved (AdaptiveDownscale.*, DownscaleLight.h).
  Forwarders only for LibplaceboVideoRenderer.cpp (Bill's host) and Bill's upstream test files.
- 9bf46a74 WP-13: ExtendedRangeStage, AdaptiveDownscaleStage, DejudderCacheStage, SigmaDeltaVectors.h;
  renderer call sites as docs/design/m2-output-callsites.patch (+ .md); CPU + GPU (not run) tests.
- e33ddf0a README-vs-code pass, key lists, fallback texts; findings
  docs/design/module-findings-2026-10-25-m2-output.md.
- ec7dfd9f test files on the test project's pch.
- ce56623a fix: scaling.output fallback ("output scaling") visible.

## State / next

- Builds: compile checks of VPRenderer, VPRendererBeta and Test with the patch applied in the
  working tree (logs in D:\codex\videoprocessor\m2-output-logs). Branch state itself has no renderer
  change.
- Verification (filtered single-threaded CPU suite) waits for an independent audit of this branch
  (owner rule 2026-10-25). GPU tests: written, not run.
- Rebase onto integ/batch1 when its hash arrives; fold WP-4's BTB/WTW line and WP-5's ER idle
  lines into the stages (m2-output-callsites.md, "Rebase notes").
