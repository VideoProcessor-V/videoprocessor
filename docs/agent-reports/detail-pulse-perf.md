# detail_pulse_stabilise: faster shader (agent report, 2026-10-18)

Branch `perf/detail-pulse` (worktree D:\codex\videoprocessor\pulse-fast), from `fix/h3-picture`
at 11cf28e0. Local only. Texture restore was not touched.

## Result

GPU time per frame on the RTX 3050 Ti, from libplacebo pass timers (`DetailPulse::SetPassTimers`).
The fast-path figures add up its four passes (low band, measure, reduce, apply).

| Size | Path | Original | Fast | Fast, gain exactly 1 |
|---|---|---|---|---|
| 3840x2160 | new frame | 2.29 ms | 1.14 ms | 0.85 ms |
| 3840x2160 | repeat (F6) | 1.12 ms | 0.70 ms | - |
| 2560x1600 | new frame | 1.54 ms | 0.70 ms | 0.53 ms |
| 2560x1600 | repeat (F6) | 0.64 ms | 0.41 ms | - |
| 1920x1080 | new frame | 0.81 ms | 0.39 ms | 0.31 ms |
| 1920x1080 | repeat (F6) | 0.34 ms | 0.23 ms | - |

- The "gain exactly 1" column covers frames in the dead zone, restarts, and tiles whose vectors
  are untrusted. On those, the apply pass only copies.
- Wall clock per call, median, synced around `pl_gpu_finish` (the original test's metric, run on
  its own):
  - 4K: new frame 2.66 to 1.34 ms; repeat 1.46 to 0.98 ms.
  - 1080p: new frame 1.05 to 0.68 ms; repeat 0.55 to 0.43 ms.
  - The owner's baseline was 2.75 ms (4K) and 0.88 ms (1080p).
- Batched (32 frames between finishes), per frame:

  | Size | New frame | Steady | Repeat |
  |---|---|---|---|
  | 4K | 2.66 to 1.37 ms | 3.39 to 1.07 ms | 1.47 to 0.98 ms |
  | 2560x1600 | 1.73 to 0.83 ms | 1.72 to 0.75 ms | 0.80 to 0.59 ms |
  | 1080p | 1.00 to 0.70 ms | 1.06 to 0.54 ms | 0.61 to 0.42 ms |

- **Caution on synced 4K timings.** When the whole DetailPulseGpu suite runs first, synced 4K calls
  on the laptop can read 5-7 ms for about 30 frames, then drop to about 1.2 ms.
  - During those frames the CPU time per call stays at 0.2 ms and the GPU pass times at about
    1 ms.
  - The original path shows the same kind of swing (2.7 vs 3.4 ms for the same work).
  - This points to the GPU's power state, not the shader. VP_TEST_PULSE_FRAMES=1 prints the
    per-frame and CPU times.
- 2560x1600 is the owner's output size, but the pulse runs on the source plane before scaling.
  It costs the same as a source of that size.

## Accuracy

- **Fast vs the CPU model:** worst 0.0088 codes. The original GPU path gives the same 0.0088.
  This is inside the 0.05 tolerance in every DetailPulseGpu*MatchesModel test (new frames,
  field, look-ahead, F6 repeat, foreign geometry, RGB).
- **Fast vs the original path:** DetailPulseGpuFastMatchesOriginal, on 1.27 M pixels, odd sizes
  included.
  - 27 pixels (0.002 %) differ, by one r16 storage step each (1/64 of a 10-bit code). The rest
    are identical.
  - RGB float output: at most 5e-5 codes.
  - Error budget: at most one storage step, where a different fp32 summation order of the same
    sums lands on the other side of a rounding boundary.
- **Precision:** every value that reaches the picture is computed as before in fp32.
  - The low band is stored in an r32f texture.
  - The output keeps the source plane format (at least 16 bits).
  - The delta is still added to the source sample itself. Grain is untouched.
  - A block with gain exactly 1 is the source bit for bit.
- **Source-resolution detector:** its input is not touched. DetailRestoreSourceResolutionInput*
  tests pass.

## What was done (in order of measured payoff)

All of it is in `src/VideoProcessor-Lib/vprenderer/DetailPulse.cpp`, in the "detail_pulse_fast"
block.

1. **Low band computed once per picture, in its own half-resolution pass.**
   - 32x32 texels per workgroup; the Gaussian's border is read about 1.7 times instead of 3.
   - Output goes to an r32f texture. Measure and apply read back 18x18 texels each.
   - Values are computed exactly as before: the box at clamped positions, the same order of
     sums.
   - The original computed the low band inline in both measure and apply. The fast path's
     apply on new frames went from 0.95 to 0.48 ms at 4K before the later steps.
   - Item 2 of the brief was already half resolution. Going coarser would change the output,
     so it was not done.
2. **Register-blocked filters with literal weights.**
   - Horizontal: a thread computes 8 adjacent outputs. Vertical: 4 adjacent rows.
   - Shared memory uses odd row pitches, so there are no bank conflicts.
   - Measure: 0.40 to 0.26 ms at 2560x1600.
3. **Measure-pass reduction.** Three strided steps replace two 9-step trees, and pixel counts are
   computed analytically. Measure: 0.51 to 0.40 ms at 2560x1600. The summation order changed,
   which moves the energies by about 1e-7 relative.
4. **Skip when the gain is exactly 1.**
   - An apply block whose gains are 1, or whose nine tile weights are all 0, is a copy.
   - Copy: 0.16 ms against 0.33 ms for full work at 2560x1600.
5. **textureGather quads** for the codes: one read for four codes. This did not change the
   timing by itself; texture reads were not the bottleneck.
6. **F6 repeats:** a low-band pass (0.09 ms at 2560x1600), then the same apply pass.
   - There is no content identity for a repeated picture, so reusing the frame's low band would
     risk the F6 ghost.
   - If the renderer could flag a cadence repeat of the identical picture, it could reuse
     `out` directly. That is not done here.

## Switch

- `detail_pulse_fast: on|off`, default on. Off runs the original passes, which are unchanged.
- Plumbing:
  - `Params::fast`;
  - LVR settings, fingerprint, change log and describe dump;
  - profile validation in `RendererProfileConfig.h`;
  - Config control "Detail pulse: fast path" under the cap, greyed while the pulse is off;
  - docs/VP-detail-restore.md settings table.

## Tests

- GPU, VP_TEST_GPU=hardware, `tools/run_chain_gate.ps1 -NoHarness -Names "DetailPulseGpu"`:
  **8/8 passed.**
  - New: DetailPulseGpuFastMatchesOriginal.
  - DetailPulseGpuTiming now covers 3 sizes x both paths x (new, steady, repeat), with batched
    figures and GPU pass times.
- CPU, single process, TRX: **27/27 passed.**
  - DetailPulseTests, with the GPU ones on WARP: fast vs original 8 pixels at 1 storage step.
  - DetailRestore*.
  - EveryProfileKeyChangesTheLiveSettingsFingerprint and DetailRestoreKeysReachTheSettings.
- Config editor: "detail restore controls" passed, off screen.
- x64 Release solution build: rc=0.

## Not done / open

- The CPU side of each call is about 0.2 ms, and is the floor of batched 1080p. Caching the
  generated headers per geometry would trim it; this was not measured as worthwhile.
- The reduce pass (one workgroup) is unchanged: 0.03 ms.
