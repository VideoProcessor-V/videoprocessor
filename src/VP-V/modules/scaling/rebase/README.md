# Re-base (entrance stage 2a) (`scaling.rebase`)

Unit: **scaling** | side: input | order: 60 | status: **built** | hook point: prepass

## Purpose / why

Undo the player's upscale per axis, back to the ENCODED grid, so later stages work on the real picture, not on scaler artifacts. When the player's kernel is identified and the axis is exactly 2:1, luma uses that kernel's 1-D least-squares inverse (across, then down). Otherwise luma uses polar EWA Lanczos sharp with anti-ringing, chroma two 1-D Lanczos passes (one-axis re-bases use 1-D Lanczos for every plane). Proposed FIRST, on the untouched delivered frame (the kernel inverse needs an exact kernel upscale as its input).

## Contract

- Input: delivered planes (P210/P010/RGB) at delivered size, untouched
- Output: planes at the encoded size per axis (x and y factors may differ); 16-bit UNORM intermediate per plane (32-bit float, else half float, when 16-bit is not renderable); kernel-inverse pass order H then V
- Geometry: per-axis factor sx, sy <= 1; the crop is scaled by the same factors; geometry decisions (active picture, NLS, subtitles, LED measurement) stay in native pixels
- Delay share: 0 frames; GPU time only
- Look-ahead: none

## How the code works

### Kernel inverse (migrated: `KernelInverse.h/.cpp`)

Pure CPU maths, no GPU and no Windows. Namespace `KernelInverse`:

- `Candidates()`: seven interpolating kernels, built once (thread-safe static): bicubic B=0 with C = 0.5 (Catmull-Rom) / 0.6 / 0.75, Lanczos 2 / 3 / 4, bilinear. Each `Candidate` holds the two upscale phase filters (positions -0.25 / +0.25 on the low grid) and the least-squares inverse filter `(U'U)^-1 U'`, computed through a 512-point DFT and truncated to 50 taps (80 for Lanczos 4), normalised so DC stays exact.
- `Find(name)`: index or -1.
- `InverseLine(c, in, n4, out)`: one axis, exactly 2:1 down (edges clamped). `UpscaleLine(c, in, n1, out)`: one axis, 2:1 up.
- `FitTile(tile, n, stride, codeMaximum)`: for an n x n tile (values 0..1) runs each candidate's inverse in both axes, upscales again, and returns the RMS residual in codes per candidate (a 32-pixel border is ignored), the best and the runner-up.
- `Accept(fit, floorCodes, margin = 1.1)`: true when the best residual is at the quantisation floor and the next is at least `margin` times larger.
- `FloorCodes(effectiveBits, signalBits = 10)`: the floor, `1.2 * step / sqrt(12) + 0.05` codes.

Data flow (callers in src/VideoProcessor-Lib/vprenderer):

1. `SourceResolutionMonitor` (its own worker thread): at a 2:1 reading (declared, or the candidate being gathered), two informative tiles per analysed frame go through `FitTile`. Accepted fits vote. A kernel stands with at least 6 votes over 2 shots and 80% agreement, within a 120 s window. It is published in the snapshot (`kernel`, `kernelVotes`, `kernelAgree`).
2. `LibplaceboVideoRenderer.cpp` (render thread): with `rebase_kernel_match` on, it reads the snapshot's kernel and calls `SourceRebase::Module::SetKernel` on change ("Re-base kernel: ..." in the log).
3. `SourceRebase.cpp` (`InversePlane`): copies the candidate's weights (up to 80) into a compute shader and runs it across, then down, into the luma plane's texture. Used only for plane 0, only when each re-based axis is exactly 2:1 and the texture is storable; anything else takes the polar / separable Lanczos path.

### Re-base itself (legacy: `SourceRebase.*`, `LibplaceboVideoRenderer.cpp`)

- The factor comes from `source_resolution`: `native` (1.0), `auto` (`SourceResolutionMonitor::AutoFactors`, per axis), a height (`1080`) or `WIDTHxHEIGHT` (`3840x1080`, 144..8640 lines).
- No re-base while the temporal renderer (smooth motion / deinterlacing) runs, or while a failure pause is active.
- In `auto` with latch fixes on, a factor change crossfades from the path being left (`AutoTransition`).
- `SourceRebase::Module::Apply` keeps three texture slots (main, a de-judder extra, the crossfade's old path).

### Config keys actually read

Read per renderer profile (`[vprenderer.Default]` and the `[vprenderer.<name>]` profiles whose `when:` matches) in `ApplyDisplayRuleOverrides`:

- `source_resolution`: `native` / `auto` / a height / `WIDTHxHEIGHT`; default `auto`. An unrecognised value is ignored without a log line.
- `rebase_kernel_match`: `on` / `off`; default `on`. An invalid value is logged and the previous value kept.
- Related monitor keys: `source_resolution_auto_min_confidence` and the `source_resolution_*` timing/evidence keys (input.source_analysis).
- `entrance_rebase` is not read anywhere (planned).
- The Original pathway sets `source_resolution: native` and `rebase_kernel_match: off`.

### User-visible fallbacks / notices

- `re-base precision (<note>)` under "Fallbacks engaged" when a plane's 16-bit format is not renderable and a float intermediate is used.
- A failed re-base renders the native frame, then pauses the re-base for 5 s, doubling to 60 s while it keeps failing. This is log-only ("Re-base failed (...); the native frame is rendered").
- No identified kernel: the polar filter, logged as "Re-base kernel: none identified (the polar filter)". This is the designed path, not a fallback.
- The planned entry `Re-base: off (<reason>)` is not implemented.

### Dependencies to lift it

`KernelInverse.*` needs only the C++ standard library (and `pch.h`). It is compiled into VideoProcessor-Lib. The forwarding header src/VideoProcessor-Lib/KernelInverse.h keeps old includes building until M2. The GPU half needs libplacebo (`SourceRebase.*`) and the source-resolution monitor.

### Test it standalone

- `KernelInverseTests` (KernelInverseTests.cpp): CPU only. `RealUpscalesAreIdentifiedAndRecovered` needs `VP_TEST_KERNELID_DIR`, else it is skipped.
- `SourceResolutionLatchTests` (SourceResolutionLatchTests.cpp): CPU; real-footage cases need `VP_TEST_REAL_FOOTAGE` / `VP_TEST_CUT_FILMS`, else skipped. `UpscalesEngageAtACutWithTheirClassAndKernel` covers the vote.
- GPU (D3D11; WARP unless `VP_TEST_GPU=hardware`; run only when GPU tests are allowed): `LibplaceboLutParserTests` methods `SourceRebase*` and `RebasedChartMatchesTheNativeChartThroughTheUpscalePath`.

Run single-threaded:

`vstest.console.exe x64\Release\VideoProcessor-Test.dll /Platform:x64 /TestCaseFilter:"FullyQualifiedName~KernelInverseTests|FullyQualifiedName~SourceResolutionLatchTests" /Logger:"trx;LogFileName=rebase.trx"`

## Cost

Efficiency review 2026-10-24 (code reading only; estimates from loop bounds and pixel counts x taps, not measured). Ranked by saving.

FitTile cost, used below: for one n x n tile (n = 256 at a 2160-line source, the 2:1 case; 128 at 1080) and m = n/2, each candidate costs n·m·W + m²·W multiply-adds for the inverse (W = 50 taps, 80 for Lanczos 4) and m·n·T + n²·T for the upscale (T = 2r+3 taps). Over the seven candidates (ΣW = 380, ΣT = 53) that is ~24 M multiply-adds per tile. The inner loop is scalar, accumulates in double through one dependent add chain and clamps every tap, so it runs at about 1 ns per tap: ~24 ms per tile, ~50 ms per analysed frame (two tiles), two analysed frames a second.

1. **Render-thread stall on the monitor lock** (largest). `SourceResolutionMonitor::Analyse` (worker thread) takes `mutex` before the tracker updates and holds it through both `FitTile` calls and the snapshot. The render thread takes the same mutex 3-5 times per frame: `SetSelection`, `AutoFactors` (two `Read()`), the kernel `Read()`, and one `Read()` each for VP deband and detail restore when on. While a 2:1 reading is declared or being gathered, a render-thread call that arrives during the fits waits up to ~50 ms (three frames at 60 Hz), twice a second. Fix: read `tracker.Ratio()` / `Candidate()` and advance `kernelFitCounter` under the lock, run the fits unlocked (tiles, spectra and the floor are local), then push the votes under the lock. Byte-identical: yes (same tiles, same vote order; the worker is the only writer). Risk: low.
2. **Kernel fit CPU**: ~100 ms of one core per second (2 tiles x 2/s x ~24 ms) for as long as a 2:1 source plays, also after the kernel stands.
   - Fix a: the fit runs whenever the reading is 2:1, whatever `source_resolution` and `rebase_kernel_match` say (the monitor does not know either). Skip it when the kernel cannot be used (`rebase_kernel_match: off`, or a selection that cannot re-base 2:1, e.g. `native`, which the Original pathway sets). Saves all of item 2 and item 1's stall there. Byte-identical output: yes (the kernel is not used in those cases; only the snapshot's `kernel` / votes fields change). Risk: low.
   - Fix b: compute 4-8 outputs per inner loop with separate accumulators and the edge clamp hoisted out of the interior, each output keeping its own summation order: ~3-4x faster. Byte-identical: yes. Risk: low.
   - Fix c: run the final upscale only over the interior the residual measures (192 x 192 of 256 x 256): ~8 % less. Byte-identical: yes. Risk: low.
   - Fix d: once the kernel stands with a clear margin, fit one tile every Nth analysed frame: up to ~90 % less. Byte-identical: no (vote counts, time to notice a kernel change). Risk: medium.
3. **First `Candidates()` build**: seven 512-point DFTs written as a double loop with two `std::polar` per term (~3.7 M complex exponentials), ~0.1-0.2 s once. It happens on the monitor worker at its first analysed frame, inside the locked section (`KernelInverse::Candidates().size()` in `Analyse`), so the render thread can wait that long once after start. Fix: call `Candidates()` once in the worker before its loop. Byte-identical: yes. Risk: low. (A twiddle table would be ~100x faster, but the weights could differ in the last bits.)
4. **GPU kernel-inverse passes** (luma, per drawn re-based frame, 2160 -> 1080): across pass 1920 x 2160 outputs x 50 taps plus down pass 1920 x 1080 x 50 = ~311 M `texelFetch` per frame (~500 M with Lanczos 4). From nominal texel rates, ~0.5 ms on the 3090 and ~3 ms on the 3050 Ti. Fix: each 16 x 16 group loads its row segment (2·16 + 49 texels) into groupshared memory once: ~10x fewer fetches, saving ~0.3-0.5 ms (3090) / ~2-3 ms (laptop) per frame. Byte-identical: yes in principle (same R16 values, weights and tap order); confirm with the chart test, as the compiler may contract differently. Risk: medium. The 16-bit UNORM intermediate (`inverseTemp`, re-based width x full height) adds ~17 MB of traffic per frame at 4K (~0.02 ms on the 3090): not worth fusing on its own. The float intermediates (32-bit, else half) only appear when R16 is not renderable; they double (or keep) the bytes per texel.
5. **Polar luma path** (no kernel identified, both axes): `pl_shader_sample_polar` EWA Lanczos sharp at a 2x downscale reaches ~6.5 px, about 130 taps per output, ~270 M taps per 1080p frame, against ~74 M for two 1-D Lanczos passes. ~3.5x fewer taps, but not byte-identical, and the code keeps luma polar on purpose (1-D Lanczos rings into black bars). Not recommended.
6. **Monitor tile copy**: `codeTiles[t] = luma` copies every tile for the fit even when no fit can run (not 2:1): 8-16 tiles x 256² floats = 2-4 MB allocated and copied per analysis, 4-8 MB/s, ~1-2 ms CPU per second. Fix: swap instead of copy, or keep the tiles only when the fit runs. Byte-identical: yes. Risk: low.
7. **Look-ahead on the render thread** (`source_resolution: auto` only). `LookAheadFrame` samples 8 tiles x 32 x 32 points through a `std::function` per new queued frame (~8,200 calls, ~0.1-0.25 ms per source frame). At a cut while a change is pending it copies 8 full tiles the same way (8 x 256² = 524,288 calls at 2160, ~5-15 ms) on the render thread under `aheadMutex`. Fix: copy rows in bulk from the prepared analysis frame, or hand the ref-counted frame to `AheadWorker` and copy there. Byte-identical: yes. Risk: low-medium (buffer lifetime). The prepared analysis frame is shared with the active-picture analysis (`PrepareFrameAnalysis` cache): no second conversion.
8. **Small per-frame items**: up to five `Snapshot` copies per frame (three `std::string` members each), ~1-3 µs. Fix: one `Read()` per frame, reused. Output: the same, apart from the race that a single read removes. Risk: low.

Verified as fine: the GPU tile readback never stalls (two-slot ring, `pl_buf_poll(..., 0)`, read only when complete, a busy slot skips the copy; 2-4 MB per copy at 2160, 2 a second). The history CSV (1,200 rows, ~100 KB) is rebuilt and rewritten at most every 5 s on the worker, with no flush. `SourceRebase::Apply` re-creates textures only when their parameters change (`pl_tex_recreate`), and drawn-as-itself pictures reuse the entrance cache. Phase pictures and the extras re-run the re-base: one more set of passes per such draw.

Cost when off: with the factor at 1 (`native`, `auto` reading native, temporal renderer running, failure pause) no re-base pass runs. The monitor still copies and analyses tiles twice a second (it feeds deband and detail restore), and still runs the kernel fit on a 2:1 reading (item 2, fix a). The look-ahead runs only in `auto`.

## Settings

- Existing keys: `source_resolution`, `rebase_kernel_match`
- Planned keys: `entrance_rebase`
- Tiers: Classic off; Low off; Mid/Upper on

## Fallbacks

- "Fallbacks engaged" entry: Re-base: off (<reason>) (planned; not implemented). Implemented: `re-base precision (<note>)`.
- Behaviour: A failed pass renders the native frame and pauses the re-base 5-60 s (log only); no identified kernel -> polar EWA Lanczos sharp (chroma: separable Lanczos); off -> straight to output scaling.

## Net fingerprint / upstream

- Fingerprint: (planned) the re-base shader identity (kernel-matched inverse or Lanczos, H->V, intermediate format) is part of every downstream net fingerprint
- Upstream modules: `input.source_analysis`

## How to build / test

Build the solution via the shared build-slot runner (see AGENTS.md); tests single-threaded; GPU tests only on mains with `VP_TEST_GPU=hardware` (`VP_TEST_ADAPTER=NVIDIA` picks the GeForce).

Placeholder tests:
- (none yet)

Isolation tests:
- `KernelInverseTests`
- SourceRebase port vs kit (within 1 LSB) (planned; no test yet)

Contract tests:
- per-axis factors (`SourceRebaseOneAxisScalesThatAxisOnly`); crossfade on latch changes (`SourceResolutionLatchTests`)

Chain tests:
- re-base x A-lens/NLS x vectors (high-risk combo)

## Known issues / open items

- ORDER vs repair.encoding PENDING the round-2 A diagnostic (both orders measured)

## Where the code lives (live file list)

Code location: **partly migrated**. The authoritative list is `live_files` in [MANIFEST.json](MANIFEST.json).

- `src/VP-V/modules/scaling/rebase/KernelInverse.cpp`
- `src/VP-V/modules/scaling/rebase/KernelInverse.h`
- `src/VP-V/modules/scaling/rebase/SourceRebase.cpp`
- `src/VideoProcessor-Lib/vprenderer/SourceRebase.h`
- `src/VideoProcessor-Lib/vprenderer/LibplaceboVideoRenderer.cpp`

Forwarding headers at the legacy path (early migration, removed in M2; they only keep files on active branches building, new code includes the `<VP-V/...>` path):

- `src/VideoProcessor-Lib/KernelInverse.h`

`SourceRebase.*` stays at the legacy path: it is an entrance stage the B3 work is changing.

## Migration directive

Code moves into this folder only when this slot is touched (opportunistic until M1, a dedicated phase after M1, finished by M2). A move = `git mv` + project/include updates + this module's `live_files` + `docs/design/components-master.json` (regenerate with `python tools/check_module_manifests.py --write`) in the SAME commit, with the build, this module's tests and the chain-gate subset passing. Never leave a copy behind; one module per change; never during a release/deploy window. Update this README in the same commit as any slot change.

## Links

- Design: [VP-pipeline-final.md](../../../../../docs/design/VP-pipeline-final.md)
- Slot data: [slots.json](../../../../../docs/design/slots.json)
- Decisions: ADR-0005, ADR-0007 in [docs/decisions](../../../../../docs/decisions/README.md)

<!-- vpv-settings-tests:begin (generated by tools/vpv_tests/seed_module_readmes.py) -->

## Settings

- Keys: `source_resolution`, `rebase_kernel_match`.
- Planned keys (not yet implemented): `entrance_rebase`.
- Tiers: Classic off; Low off; Mid/Upper on.
- VP-V page, Scaling tab: tiers Classic / Low / Mid; tier keys `adaptive_downscale`, `adaptive_w_base`, `source_resolution`.
- Listed under the tier: `rebase_kernel_match`, `source_resolution_text_guard`, `source_resolution_mixed_guard`.
- Plain-language guide: [docs/settings/VP-settings-reference.md#5-scaling](../../../../../docs/settings/VP-settings-reference.md#5-scaling).
- Fallback entry ("Fallbacks engaged"): `Re-base: off (<reason>)`.

## Tests

- Placeholder (byte-identity on/off, zero cost): n/a.
- Isolation (fixed inputs, expected outputs, cost): KernelInverse tests; SourceRebase port vs kit (within 1 LSB).
- Contract (formats, geometry, fallback entry): per-axis factors; crossfade on latch changes.
- Chain (total delay at the 4/3/2-frame budgets): re-base x A-lens/NLS x vectors (high-risk combo).
- Combinations: pairwise / high-risk / random lines in `tests/vpv/` that set this slot's unit (`tools/run_combinations.ps1`); matrix: `docs/design/slot-test-matrix.md`.

<!-- vpv-settings-tests:end -->
