# Texture restore + breathing add-on (`repair.texture_restore`)

Unit: **repair** | side: input | order: 90 | status: **built** | hook point: prepass

## Purpose / why

Long-memory add-on on A's output: texture restore (~2 s peak-hold fine-band reference along vectors) and breathing reduction (detail_pulse_stabilise, the GOP pulse). Premium, needs an efficiency rebuild (9.8 ms at 4K on the 3050 Ti).

## Contract

- Input: planes after the re-base and deband (luma; detail pulse also takes a one-plane RGB picture); vectors (validity-gated)
- Output: same size and format; only the luma (or the RGB plane) is changed, chroma planes pass through
- Geometry: same as input
- Delay share: 0 frames
- Look-ahead: detail pulse may measure the next queued frame (optional)

## How the code works

Two independent modules run in one slot, in this order: detail pulse stabilise (`DetailPulse.*`, the breathing reduction), then texture restore (`TextureRestore.*`). The maths are documented at the top of each header and in docs/VP-detail-restore.md. The CPU reference models (src/VideoProcessor-Test/DetailPulseModel.h and TextureRestoreModel.h) share the constants.

### Public API

`DetailPulse` (DetailPulse.h):

- Constants and helpers shared with the model: `kDefaultCap` 1.35, `kCapMin`/`kCapMax` 1.0..1.5, `kDefaultFast`, `Scale`, `WindowFrames` (0.17 s of history), `Gain`, `VetoWeight`, `TrustWeight`, `LumaWeights`.
- `Params`: `cap`, `colorDepth`, `planeLines` (re-based height), `sourceHz`, `bitrateQuality`, the optional `field` with `fieldScale`/`fieldScaleX`, `sequence`, `lumaSystem`/`codeLevels` (a one-plane RGB picture is stabilised on its luma), `fast` (detail_pulse_fast).
- `Module::Apply`: a new consecutive `sequence` measures two luma bands per 32 px tile, advances the GPU history ring and decides gains. A repeat of the same sequence applies the stored decision to the picture given, with that picture's own bands. A non-consecutive sequence, a geometry change, a cut or a fade restarts the history (gain 1).
- `ApplyForeign`: the current decision on a picture of another geometry; never advances the history.
- `MeasureAhead(..., next frame, debander, deband params)`: optional look-ahead. It measures the next source frame's bands, debanded first through `VpDebander` slot `kSlotLookAhead` when the debander runs. A second overload takes a bare luma texture.
- `Forget()`, `Release(gpu)`, `LastError()`, `Failures()`, `Frames()`, `HistorySequence()`, `HistoryFrames()`, `AheadFrames()`. `SetPassTimers` / `PassTimes`: GPU pass timing for tests.

`TextureRestore` (TextureRestore.h):

- Constants shared with the model (bands 1 / 2.5 / 6 px, 16 px tiles x `Scale`, 2 s hold half-life, focus and pull gates), `kDefaultCap` 1.5, `kCapMin`/`kCapMax` 1.0..2.0.
- `Params`: `cap`, `colorDepth`, `planeLines`, `sourceHz`, `bitrateQuality`, `field`/`fieldScale`/`fieldScaleX`, `sequence`.
- `Module::Apply`: eight passes. Half-resolution low bands; tile statistics; grain level; cuts and fades; references and gains; apply. A repeat reuses its frame's gains with its own low bands (`lowRepeat`) and never advances the references. A `cap` of 1.0 returns the input itself and restarts the references when the cap rises.
- `ApplyForeign`, `Forget()`, `Release(gpu)`, `LastError()`, `Failures()`, `Frames()`, `HistorySequence()`.

### Data flow

In: a `pl_frame`; only plane 0 is read. Out: a copy of the frame with plane 0 replaced by the module's own texture: `out` from `Apply`, `outForeign` / `fOut` from `ApplyForeign`. The module owns the textures; each call overwrites them, and `Release` frees them. Detail pulse's history is a GPU ring (`kRingSlots` = 128 entries); texture restore keeps two reference sets and two low-band sets.

Call sites (`src/VP-V/segments/EntranceSegment.cpp`, `Segment::Detail` and `Segment::DetailLookAhead`):

- Main, drawn as itself: `DetailLookAhead`, then `DetailPulse::Apply`, then `TextureRestore::Apply` on its output. A failure sets the status reason (see below).
- Main, Stage B phase picture: the history first advances once on A itself (A's debanded picture, the result discarded), then the picture takes the same Apply calls as a repeat.
- De-judder extra: `Apply` as a repeat, only when the history already holds this source frame. It never advances the history.
- Crossfade path being left: `ApplyForeign` with `planeLines` scaled by the factor ratio, only when a decision for this frame is held.
- Look-ahead: once per new source frame, only when the de-judder uploaded the next frame (two planes, not RGB) and no re-base is engaged. It is also checked when a cached or ring entrance result is reused.

The host (`LibplaceboVideoRenderer.cpp`) owns both modules and fills `detailPulseParams` / `textureRestoreParams` and the `detailPulseRuns` / `textureRestoreRuns` flags once per frame. Both are off while smooth motion or deinterlacing runs. The field is the de-judder's only when its estimate is for this source frame and motion validity is current; texture restore also needs a one-period field. The content rate is the pulldown film rate, else the measured capture rate, else the nominal rate. Both histories restart (`Forget`) on motion invalidations, a re-base factor change, and when a stage stops running.

### Threading

Render thread only. There are no locks in either module. Detail pulse's pass timers (`SetPassTimers` / `PassTimes`) are a file-static global for tests, one GPU at a time.

### Config keys actually read

From the renderer profile sections that apply (`[vprenderer.Default]`, `[vprenderer.<name>]`, `[vprenderer.input.<name>]`; `ApplyDisplayRuleOverrides`), all validated in RendererProfileConfig.h. An invalid value is logged and the previous value kept.

- `detail_pulse_stabilise`: `on` (default) / `off`.
- `detail_pulse_cap`: 1.0 .. 1.5, default 1.35.
- `detail_pulse_fast`: `on` (default) / `off` (the original passes).
- `texture_restore`: `off` (default) / `statistical`.
- `texture_restore_cap`: 1.0 .. 2.0, default 1.5.

The Original pathway sets `detail_pulse_stabilise` and `texture_restore` to `off`. Smooth motion or deinterlacing (motion keys) stops both.

### User-visible fallbacks / notices

Neither module produces a "Fallbacks engaged" entry. The listed `Texture restore: inactive (<reason>)` is a status line, not a FallbackStatus entry:

- Main-path failure: the frame renders without the stage. The renderer's processing list (status) then shows "Detail pulse stabilise: inactive (<reason>)" or "Texture restore: inactive (<reason>)" instead of "on (cap N)" / "statistical (cap N)". The log line is repeated only when the reason changes. There is no one-time notice. The status text is built when the next frame's parameters are set, so it lags by one frame.
- Texture restore needs a YCbCr luma plane: an RGB picture (RGB capture, the subtitle move's composed picture) always reads "inactive (plane 0 is not luma)". Detail pulse handles RGB.
- Failures on the extra and crossfade paths, and on A's history advance for a phase picture, are silent. A failed look-ahead is silent (optional by design).
- While smooth motion or deinterlacing runs, the status says "not applied while smooth motion or deinterlacing runs".

### Dependencies to lift it

libplacebo (compute shaders through `pl_dispatch`) and `pch.h`. `DetailPulse` also needs `src/VideoProcessor-Lib/vprenderer/GrainPreserve.h` and `VpDebander.h` (repair.deband, for the look-ahead). `TextureRestore` needs neither: it has its own copies of `Smoothstep` and the coding-noise curve. Both are compiled into VideoProcessor-VPRenderer(.Beta) and VideoProcessor-Test. The forwarding headers src/VideoProcessor-Lib/vprenderer/DetailPulse.h and TextureRestore.h keep old includes building until M2.

### Test it standalone

- `DetailPulseTests` (DetailPulseTests.cpp): CPU model tests, plus GPU tests whose names start with `DetailPulseGpu` (D3D11; WARP unless `VP_TEST_GPU=hardware`; `DetailPulseGpuTiming` needs hardware). `DetailPulseRealClipBreathing` needs `VP_TEST_REAL_FOOTAGE`, else it is skipped.
- `TextureRestoreTests` (CPU model; `TextureRestoreRealClipBreathing` needs `VP_TEST_REAL_FOOTAGE`) and `TextureRestoreGpuTests` (GPU, WARP unless `VP_TEST_GPU=hardware`; `TextureRestoreGpuTiming` needs hardware), both in TextureRestoreTests.cpp.
- `DetailRestoreConfigTests` (CPU): key validation, cap ranges, a profile file, and the source-resolution monitor's input left unchanged.
- Through the renderer (`GpuHardware`, skipped without hardware): `RendererCaptureHarnessTests` methods `DetailRestoreKeysReachTheSettings` and `DetailRestoreGpuSourceMonitorUnchanged`.

Run single-threaded (CPU only):

`vstest.console.exe x64\Release\VideoProcessor-Test.dll /Platform:x64 /TestCaseFilter:"(FullyQualifiedName~DetailPulseTests&FullyQualifiedName!~DetailPulseGpu)|FullyQualifiedName~TextureRestoreTests::|FullyQualifiedName~DetailRestoreConfigTests" /Logger:"trx;LogFileName=texture_restore.trx"`

## Settings

- Existing keys: `texture_restore`, `texture_restore_cap`, `detail_pulse_stabilise`, `detail_pulse_cap`, `detail_pulse_fast`
- Planned keys: (none)
- Tiers: Upper only (after the efficiency pass)

## Fallbacks

- "Fallbacks engaged" entry: none produced. `Texture restore: inactive (<reason>)` (and `Detail pulse stabilise: inactive (<reason>)`) appear in the status line only, not under "Fallbacks engaged".
- Behaviour: Failure renders without it (counted in the status line on the main path; silent on the extra and crossfade paths).

## Net fingerprint / upstream

- Fingerprint: on/off is an unlabelled robustness input to the refine net
- Upstream modules: `repair.encoding`, `repair.deband`, `motion.estimation`

## How to build / test

Build the solution via the shared build-slot runner (see AGENTS.md); tests single-threaded; GPU tests only on mains with `VP_TEST_ADAPTER=NVIDIA`.

Placeholder tests:
- (none yet)

Isolation tests:
- TextureRestoreGpuTiming
- DetailPulse tests

Contract tests:
- never runs with libplacebo smooth motion / deinterlacing

Chain tests:
- stacked A -> texture restore (double-boost check)

## Known issues / open items

- efficiency rebuild (fused passes, shared-memory tile stats)
- redesign to measure in the A-cleaned domain

## Where the code lives (live file list)

Code location: **migrated**. The authoritative list is `live_files` in [MANIFEST.json](MANIFEST.json).

- `src/VP-V/modules/repair/texture_restore/DetailPulse.cpp`
- `src/VP-V/modules/repair/texture_restore/DetailPulse.h`
- `src/VP-V/modules/repair/texture_restore/TextureRestore.cpp`
- `src/VP-V/modules/repair/texture_restore/TextureRestore.h`
- `docs/VP-detail-restore.md`

Forwarding headers at the legacy path (removed in M2; they only keep files on active branches building, new code includes the `<VP-V/...>` path):

- `src/VideoProcessor-Lib/vprenderer/TextureRestore.h`
- `src/VideoProcessor-Lib/vprenderer/DetailPulse.h`

The call-site glue (`Segment::Detail`, `Segment::DetailLookAhead`) is in the entrance segment, `src/VP-V/segments/EntranceSegment.cpp`. The per-frame parameters are set in `src/VideoProcessor-Lib/vprenderer/LibplaceboVideoRenderer.cpp`, which owns the modules.

## Migration directive

Code moves into this folder only when this slot is touched (opportunistic until M1, a dedicated phase after M1, finished by M2). A move = `git mv` + project/include updates + this module's `live_files` + `docs/design/components-master.json` (regenerate with `python tools/check_module_manifests.py --write`) in the SAME commit, with the build, this module's tests and the chain-gate subset passing. Never leave a copy behind; one module per change; never during a release/deploy window. Update this README in the same commit as any slot change.

## Links

- Design: [VP-pipeline-final.md](../../../../../docs/design/VP-pipeline-final.md)
- Slot data: [slots.json](../../../../../docs/design/slots.json)
- Decisions: ADR-0009 in [docs/decisions](../../../../../docs/decisions/README.md)

<!-- vpv-settings-tests:begin (generated by tools/vpv_tests/seed_module_readmes.py) -->

## Settings

- Keys: `texture_restore`, `texture_restore_cap`, `detail_pulse_stabilise`, `detail_pulse_cap`, `detail_pulse_fast`.
- Tiers: Upper only (after the efficiency pass).
- VP-V page, Repair tab: tiers Classic / Low / Mid / Upper; tier keys `deband_vp`, `detail_pulse_stabilise`, `texture_restore`.
- Listed under the tier: `detail_pulse_fast`.
- Plain-language guide: [docs/settings/VP-settings-reference.md#4-repair](../../../../../docs/settings/VP-settings-reference.md#4-repair).
- Fallback entry ("Fallbacks engaged"): `Texture restore: inactive (<reason>)`.

## Tests

- Placeholder (byte-identity on/off, zero cost): n/a.
- Isolation (fixed inputs, expected outputs, cost): TextureRestoreGpuTiming; DetailPulse tests.
- Contract (formats, geometry, fallback entry): never runs with libplacebo smooth motion / deinterlacing.
- Chain (total delay at the 4/3/2-frame budgets): stacked A -> texture restore (double-boost check).
- Combinations: pairwise / high-risk / random lines in `tests/vpv/` that set this slot's unit (`tools/run_combinations.ps1`); matrix: `docs/design/slot-test-matrix.md`.

<!-- vpv-settings-tests:end -->
