# Gate fix 2026-10-17 (fix/gate-1017)

Worktree `D:\codex\videoprocessor\gate-fix`, branch `fix/gate-1017` from `integ/2026-10-17` (93369c39).
Local only. GPU: the laptop's RTX 3050 Ti, used by this agent alone. Base for comparison: `integ-base`
(1592d39d binaries), plus a bisect worktree `gate-bisect` built at 3aaa561b (platform merge) and f0c2df4f
(platform + picture merge).

None of the four failures is a product regression. Three are test assumptions that the merged
behaviour exposed; one is test timing that the base build fails too. All four are fixed in the tests;
no product code changed.

## 1. InterpolationGapFillPairsAcrossMissedFrames: environment (startup shader compile) + test timing

- Every harness `Run` creates a renderer whose libplacebo cache starts empty. The test host has no
  persistent cache: "persistent shader cache: no existing file at ...\TestPlatform\vprenderer\..." and
  "not saved: write failed". So the first interpolated pictures compile their shaders (HLSL to DXBC,
  0.2-0.6 s each here). With a CPU build beside it, that became a 1.3-2.9 s render stall and a backlog
  flush, which invalidates the vectors.
- The gaps sat at frames 90 and 150-151 (1.8 s and 3.1 s), inside that window.
- Alternating runs under the same load: **base 0/3 passed, integration 1/3**. Every failure had the same
  message, "on: gap pairs made", with 31-89 pictures instead of ~119. Not a regression.
- Fix (RendererCaptureHarnessTests.cpp, `InterpolationGapFillPairsAcrossMissedFrames`): the gaps are now
  at 5.0 s and 6.5 s of source time (derived from the measured rate) in an 8.5-s run. Assertions are
  unchanged. Gap fill on gave 2 gap pairs and off gave 0.

## 2. RebaseKeepsTheLayoutWithCropZoomNlsAndBlanking: test timing (capture before the auto crop engaged)

- Only the auto-crop case failed: the re-based picture was at x 508-3363 y 493-1690, against the native
  x 48-3823 y 301-1882. That is the letterboxed 16:9 frame pillarboxed in the scope screen, before the
  crop.
- Full renderer logs from a pass and a fail on the same binaries show identical crop decisions
  (rect 0,276-3840,1884, authority current). Only the order differed: the failing run's 3-s capture took
  source frame 93, while the crop first applied at frame 103.
- Intermittent: the integration build failed 2/3 runs and base passed 5/5. The bisect flagged f0c2df4f
  only because its startup was slower.
- The crop's first frame is the same before and after the picture merge:
  - 3aaa561b (platform only): native ~30, re-based 1080 at frames 98/104/106;
  - integration: native ~30, re-based 1080 at frames 96/96/105/105.
  The picture merge did not change crop timing. The capture raced a 1.9-2.1 s startup stall.
- Not the 540-in-1080 tile quantisation (latch-542): the source_resolution here is a forced 1080.
- Fix: capture at 5 s (run 6 s). For the auto-crop case the test now also asserts that the captured
  source frame follows the crop's first application, so a too-early capture is reported as such, not
  as a moved layout. 4/4 passes, with captures at frames 144-224 after crops at 96-105.
- For the owner: under re-base the auto crop engages ~70 frames (~2-3 s) later than native. This is
  pre-existing (same on 3aaa561b) and worth a separate look.

## 3. WholeHookChainCombinationMatrix (8-in-10, scale 1.50, full range): test assumption, no sigma-delta bias

Experiments on the failing combination (VP_TEST_MATRIX_ONLY, a new test-only filter). The probe now
reports each ramp step's patch mean, min and max, and the reference mean. Results:

| Sigma-delta / NLS shader | code 900, frames 0/1/2 patch mean | pixels | signed mean |
|---|---|---|---|
| integration (bound 0.5 + noise, k clamp) | 975.00 / 974.59 / 973.43 | 971-979 | -0.489 / -0.491 / -0.475 |
| bound reverted to 0.5 | 975.00 / 974.59 / 973.42 | 971-975 | -0.489 / -0.491 / -0.470 |
| whole base sigma-delta shader | same as the row above | 971-975 | same |
| base NLS.glsl (or nls sampling bilinear) | 975.00 / 975.00 / 975.00 | 975 | -0.491 / -0.491 / -0.478 |

- **The sigma-delta change did not introduce a bias.** Reverting L1/L2 changes the frame-2 mean by
  0.01 code. The base build's signed means for this combination are the same: -0.491 / -0.490 / -0.472.
- The ~-0.5 signed error appears across base combinations, 10-bit included (-0.68 to 0.0). It is the
  documented Stable 7.360 repair / half-float store bias in the chain-vs-reference comparison, not the
  sigma-delta. Its fix is still pending.
- What moved: the shaping reference for code 900 is **974.39**, not the ideal 976, which is 242.88 in
  8-bit. The sigma-delta snaps x to a whole code when it is within 0.13 (the rgba16f snap).
  - Base NLS (bilinear): x landed inside the snap window, giving 243 and every pixel 975.
  - Picture's Catmull NLS sampling: x moves by under 0.01 code (measured: nearest + 100 x (Catmull -
    nearest) renders like Catmull alone, so the identity is kept). That is enough to land just outside
    the window. The sigma-delta then dithers 242/243: pixels 971 and 975, three-frame mean 974.34
    against the reference 974.39.
  - This is faithful. The base build's constant 975 was the snap's +0.6-code error. The test read one
    pixel against the ideal 976 with a tolerance of 4.
- Fix (ChainGpuProbe.h `RunMatrix`): for 8-in-10, the ramp level is the step's patch mean, with the
  same tolerance of 4 (with Stage 10 off: the reference's patch mean). A biased level would still fail.
  10-bit keeps the single pixel. The per-pixel limit (pictureMax 6) and the mean limits are unchanged.

## 4. MixedScreenNativeUiOverAnUpscaleIsNotRebased: test assumption (merge 49dcb50e, srcres-latch)

- With latch fixes on, the latch engages only at a cut, black, fast pan or low-detail moment, or by
  crossfade after declared + 20 s. Cuts are judged by tile-pattern decorrelation (`Tracker::CutJudge`).
- The harness "cut" was a brightness jump of the same framing, which is an in-shot flash by design
  ("fades and flashes are not cuts"). The log showed "confirmed: waiting for a cut ..." for the whole
  14 s. Not the tile quantisation.
- Fix (RendererCaptureHarnessTests.cpp `FillV210`): a harness cut also moves the picture a third of the
  width on (an even offset, so it stays an exact 2x upscale). Log: "engaged at the cut / black
  (look-ahead verified)". The native-UI run is still never re-based.

## Harness shader cache (the flake behind #1 and #2, and others)

- With the four fixes in place, full gate runs still failed 1-3 different timed harness tests per run:
  - StreamFaults "locks on the film" and "missed: history forgotten";
  - NativeRgbIngest and VpDebander "frames presented";
  - MotionAhead re-engage;
  - InterpolationTimeline and InterpolationStreamFaults.
  Each passed when rerun alone.
- Same cause as #1: a cold shader compile in every `Run` (render_ms 1158 on the first frame), on top of
  other agents' builds loading the CPU to 100 %.
- The integration build fails StreamFaults just the same under that load: 2/2 alternating runs failed,
  while this branch passed 2/2.
- Fix (src/VideoProcessor-Lib/vprenderer/LibplaceboVideoRenderer.cpp `ShaderCacheTestFolder`, tests
  only): `VP_TEST_SHADER_CACHE_DIR` stands in for the exe folder in `ShaderCachePath` and
  `ShaderCacheClearRequestPath`. The harness `Run` points it at `x64\Release\vp-harness-shader-cache`.
  Production behaviour is unchanged when the variable is unset.
- Measured: after the first renderer of a build, no HLSL compiles and no submit-gap stalls. The gap-fill
  run went from 303 to 442 pictures.

## fix/latch-540-exact merged; monitor GPU test unpinned

- 0ccfc561 is merged (cdf3a090). Neither #2 nor #4 involved the declared ratio: #2 forces 1080, and #4
  waited for a trigger.
- `SourceResolutionMonitorDeclaresAutoEngagesAtACutAndDisengagesAtOnce` now runs with latch fixes on.
  It declares ~540 (0.50) and re-bases to 0.5 exactly.
- The test accepts both wait texts. Its fade sub-test sets a 600-s engage wait, because the default
  20-s crossfade otherwise fired inside its 40-s fade. It accepts the fixes' real-black and low-detail
  engages; measured: "engaged during a low-detail moment". Passes on the RTX 3050 Ti.

## Results

- **Chain gate** (the full command, binaries built at 4523ea25, the branch's final code, `-OutDir
  D:\codex\videoprocessor\gate-fix-trx\full4`): **188 run, 188 passed**. The only SKIP markers are the four
  opt-in on-screen tests.
- Earlier full runs on this branch, while other agents' builds and tests held the CPU at 100 %:
  - 185/188 before the cache fix;
  - 187/188 after the latch merge;
  - 185/188 after the cache fix, under heavy load.
  Each failure passed when rerun alone. The final run had no GPU contention from gate-verify.
- **CPU, single-threaded** (vstest, TRX): LibplaceboLutParserTests, SourceResolutionDetectorTests and
  SourceResolutionLatchTests: **260/260 passed**.
- Builds: full x64 Release with the shared slot runner. LNK1103/LNK1318 failures were fixed by
  deleting the stale .iobj/.ipdb files.

## Commits (fix/gate-1017)

- dc0b296b: the four test fixes and the probe diagnostics.
- cdf3a090: merge fix/latch-540-exact (0ccfc561).
- 8c1dd023: the monitor GPU test unpinned.
- 4523ea25: the harness persistent shader cache (tests-only env override in the renderer).
- This report.

## For the owner

- The sigma-delta is unbiased relative to base. The ~-0.5-code chain-vs-reference signed error on the
  stable DLL predates this work: it is the 7.360 repair / half-float store bias, fix pending.
- Under re-base the auto crop engages ~2-3 s later than at native (frame ~100 vs ~30). This is
  pre-existing.
- The worktree `D:\codex\videoprocessor\gate-bisect` (detached, 3aaa561b build) can be removed.
