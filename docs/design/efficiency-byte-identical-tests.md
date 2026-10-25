# Byte-identical efficiency fixes: GPU test plan (2026-10-24)

Branch `perf/byte-identical` (from `feat/skeleton` d0212534). Source: the efficiency audit
`docs/design/efficiency-audit-2026-10-24.md` (branch docs/efficiency-audit). **None of these GPU
tests has been run** (no GPU runs on 2026-10-24); the CPU tests listed are in the branch.

Every fix below must leave the output **bit-identical**. The comparison in every GPU test is exact:
`memcmp` of float textures read back, or the capture harness at **0 codes** difference on every
pixel of every frame (not a tolerance gate). BTB/WTW (vp-btb-wtw-preserve) and grain
(vp-grain-faithful) need no separate check: identical bytes keep both. Every test uses several
output and source sizes, including odd ones (1919x1079, 2561x1601, 3840x2160, 1280x720), so no
result depends on one resolution (vp-general-not-fixed).

| Fix | Upstreamable? | CPU test (in branch) | GPU test (written, not run) |
|---|---|---|---|
| #7 ER one-pass carry | VP-only (ExtendedRange is VP) | `ExtendedRangeCarryTests::OnePassCarryKeepsBothPassesExpressions` | G7 |
| Source-resolution fit unlocked | VP-only (SourceResolutionMonitor is VP) | `SourceResolutionLatchTests::KernelFitDoesNotHoldTheReadLock` (+ the existing kernel/latch decision tests) | GS |
| #8a sigma-delta LQPREV/MVPREV sizes | VP-only (shader + CustomHookChain are VP) | `CustomHookChainTests::SigmaDeltaGridTemplatesCoverThePassSizes` | G8a |
| #3 early upload in all modes + VRAM guard | VP-only (EarlyUpload is VP; host call sites in the patch) | `EfficiencyPolicyTests::EarlyUpload*` | G3 |
| #8b idle texture release | VP-only | `EfficiencyPolicyTests::IdleTextures*`, `ThrashingDoublesTheIdleTime` | G8b |
| #10 change keys | **Upstreamable** (Bill's code, no VP-V dependency) | `ChangeKeyTests` | G10 |
| Timers | VP-only (readout in VP-V/core) | `PassTimersTests` | GT |
| #9 B3 ring handover | VP-only | design only (not built, see efficiency-callsites.md) | G9 |

## G7: ExtendedRange carry, one pass vs two passes

Harness: `ExtendedRangeGpuProbe.h` (VideoProcessor-Test) plus a new `TEST_METHOD` in the GPU
probe suite.

1. For each case: transfers (power 1.8/2.2/2.4/2.8, BT.1886 with b = 0 and measured, sRGB,
   identity), primaries (BT.709 -> BT.709 / P3 / BT.2020; BT.2020 SDR on BT.709 with
   `libplaceboMapsPrimaries`, i.e. the linear remainder), gamma conversion on/off, a synthetic
   RGB-stage texture with values in [-0.1, 1.1] (BTB, WTW, every channel independently out of
   range, NaN-free), at 1919x1079 and 3840x2160.
2. Run the carry hook with `ForceTwoPassCarryForTest(true)`, read back `clipped` (the hook's
   output texture) and `remainder` (the restore's binding); repeat with `false`.
3. Assert `memcmp == 0` for both textures, and `CarryPasses() == None` in the one-pass run (the
   storage-image path really ran: on a GPU without it the test reports the fallback reason and
   is inconclusive, not passed).
4. Whole chain: ChainGpuProbe SDR->HDR and SDR->SDR with `preserve_extended_range` on, one-pass
   vs forced two-pass, final output 0 codes.

## GS: source-resolution monitor, fit outside the lock

CPU status 2026-10-24: on the branch build, SourceResolutionLatchTests + SourceResolutionMonitor +
MixedScreenSourceResolutionTests + EarlyUpload filter: 51/51 passed (real corpus);
KernelFitDoesNotHoldTheReadLock: 24 fits probed, 0 blocked. The RESULT-line diff against a
d0212534-equivalent build was started and stopped unfinished (it used another worktree's build);
redo it from this worktree with d0212534 built separately.

1. Real clips (`D:\testclips` manifest, the 2:1 upscales used by
   `UpscalesEngageAtACutWithTheirClassAndKernel` and `RealClipSourceResolutionCutsAndLatch`):
   run the latch suite on d0212534 and on the branch with the monitor history CSV on; the CSVs
   must be identical row for row (every decision: declared ratio, kernel, votes, confidence).
2. Live: 2:1 source on the laptop, `render stall` and frame-time logs: the ~50 ms stalls twice a
   second at 2:1 are gone (Read() never waits for a fit; the probe test proves it on CPU).

## G8a: sigma-delta state textures sized to their grids

1. BetaGpuTests' sigma-delta sequences (`Load(..., "Temporal sigma-delta.glsl", ...)`) with the
   d0212534 shader saved as a fixture vs the branch shader: 120 refreshes per case with motion
   (search on), cadence repeats (`static_refresh`), `vector_source` 1, motion compensation off,
   output sizes 1280x720, 1919x1079, 2560x1600, 2561x1601, 3840x2160. Output textures bit-equal
   on every refresh.
2. Renderer: the sigma-delta on a de-judder clip (extras and mains), capture harness 0 codes.
   Include edge blanking + NLS + a non-filling aspect (the OUTPUT image smaller than the target):
   the new sizes assume HOOKED <= {{output_width}} x {{output_height}} (audit 2026-10-25 #4).
3. VRAM: `LQPREV` + `MVPREV` at 3840x2160 drop from 2 x 66 MB to ~4.2 MB + ~0.3 MB.

## G3: early upload in every mode

1. Capture harness with recorded raw input for each raw route: r210 / R12B / R12L native RGB (with
   and without de-judder: both unpacks now read one slot), P210 (v210) GPU unpack, DV tunnel.
   Modes: film / de-judder, normal, low latency. `capture_upload=render` vs `auto`: output CRC per
   frame identical, same frame count, no extra repeats.
2. Anti-tearing: the existing early-upload tests (fences, slot reuse, payload release) in the new
   modes; a 10-minute de-judder run with `noFreeSlot` / `failures` counters logged at close.
3. VRAM guard: a forced small budget (test hook on `QueryMotionEngineBudget`) shows "Fallbacks
   engaged: early upload (render upload: video memory short, needs N MB, M MB free)" in Ctrl+I,
   the Config status and Ctrl+Alt+D, output identical to `render`.
4. Laptop 4 GB at 4K30: slots x 33 MB (r210) fit with the 25% reserve or the fallback is listed;
   MotionEngineIdle pressure releases do not increase.
5. Timing: the render thread's "upload: CPU copy + unpack submit" line (T4) drops by ~5 ms per 4K
   12-bit frame in film mode.

## G8b: idle feature textures

1. SDR clip with ER active, then an HDR clip for 15 s (ER idle -> released after 10 s), then SDR
   again: output after re-activation identical (0 codes) to a run with the releaser disabled; the
   log shows the release and VRAM drops by ~266 MB at 4K.
2. Toggle SDR/HDR every 5 s for 5 minutes: at most one release per thrash window, idle time
   backs off (`ThrashBackoffs`), output identical to the no-release run.
3. Follow-ups (not in the patch yet, each needs this test before it is added): AdaptiveDownscale,
   CrossfadeMix, the re-base, debander and texture restore (the entrance ones must also invalidate
   `entranceCache` and bump `entranceGeneration` when released).

## G10: change keys (upstreamable)

`ChangeKeyTests` proves the key text equals the old ostringstream text for every type used, so
the same refreshes log. GPU-free confirmation: the same 5-minute clip on d0212534 and on the
branch; the counts and contents of `SDR_GAMMA`, `LUMINANCE_CONTRACT`, the crop policy, final
layout, `Alpha NLS` and `VP anamorphic layout` lines are identical (timestamps aside). No pixel
path is touched.

## GT: timers

1. Output identical with timers sampling every refresh (`sampler.SetEveryN(1)`) vs N = 8: capture
   harness 0 codes (the info callback only reads `pass->last`).
2. No GPU waits: frame-time distribution with N = 1 vs timers off within run-to-run noise; no
   `pl_timer` drains, no `pl_gpu_finish` (code review: `PassTimerHook` calls none).
3. The readout appears in vp.log every 30 s while frames are timed (and so in the Ctrl+Alt+D
   bundle's `log-excerpt.txt`): GPU passes by description, CPU work split, CPU waits apart
   (render present, de-judder extra vblank waits), upload CPU line.
4. Motion (T3): "motion: <pass>" GPU groups appear on de-judder content; DejudderWarp output
   identical with sampling on/off (timers do not change dispatches).
5. Entrance (T2): "entrance: deband / re-base / texture restore" groups appear; output identical
   with sampling on/off; `GpuSpanTimers::Dropped()` stays near 0.
6. Still to wire (design in efficiency-callsites.md): the inner half-size MotionWarp, the conversion thread's copy time into the
   readout, GPU unpack pl_timer.

## G9: B3 ring handover (when built)

B3 on (`entrance_once`), the harness's `VP_TEST_ENTRANCE_ONCE_FALLBACK_EVERY=N` run: ring
pictures handed over vs copied, 0 codes, stateful stages never restart.
