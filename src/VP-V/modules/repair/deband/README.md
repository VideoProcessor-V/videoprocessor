# Deband (`repair.deband`)

Unit: **repair** | side: input | order: 80 | status: **built** | hook point: prepass

## Purpose / why

Remove banding at source resolution, after repair (cheaper and correct for source-resolution bands).

## Contract

- Input: every source plane after the re-base (source or re-based size), in the signal domain (PQ codes on a PQ source)
- Output: same size and plane layout in the module's own textures (at least 16 bits per component); grain guard keeps grain
- Geometry: same as input
- Delay share: 0 frames
- Look-ahead: none of its own. The hold uses the current and the previous source frame only. A separate look-ahead slot debands the next frame for detail_pulse_stabilise's look-ahead (repair.texture_restore)

## How the code works

### Public API (`VpDebander.h`, namespace `VpDebander`)

- `Strength(effectiveBits, colorDepth, bitrateQuality, useQuality, snapLevels)`: the threshold in signal codes. The content's level step is 1.5 x 2^(depth - bits). With `snapLevels` (source_coding_fixes) the bits are floored, and readings within half a bit of the signal depth count as the full depth. Low bitrate adds up to three 8-bit steps below quality 0.8. 0 = nothing to deband.
- `Params`: `thresholdCodes` (0 = the module refuses), `colorDepth`, `radiusScale` (sample radius 16 px at 1080 lines, clamped 2..64 px), the optional de-judder `field` / `grainError` with `fieldScale` / `fieldScaleX`, `sequence`, `holdAlpha` (0.5), `grainCodes` (2.0), `grainGuard` and `grainSplit` (both default true).
- `Module::Apply(gpu, dispatch, source, params, out, slot)`: debands every plane. It returns false and leaves `out` untouched on any failure; `LastError()` and `Failures()` say why. Slots: 0 = the main frame (the only slot that keeps hold state), 1 = per-picture (a de-judder extra, a Stage B phase picture), `kSlotLookAhead` = 2 (the detail-pulse look-ahead), `kSlotCrossfade` = 3 (the crossfade's path being left). Each slot has its own output textures, so a lower slot's frame stays valid.
- `ForgetHold()` (motion discontinuity, re-base factor change), `CarryHold(sequence)` (a source frame no refresh showed), `HeldFrames()`, `HoldSequence()` / `SetHoldSequenceForTest()`, `Release(gpu)`.

Per plane the passes are, in this order:

1. Grain split (`grainGuard` and `grainSplit`, the default): a half-resolution low band (2x2 box, then a separable Gaussian, `GrainPreserve::kSplitSigma`) and a structure tensor per block of it. Only the low band is debanded. The residual (grain, noise) passes unchanged, and the band's sharp step part is put back where it is coherent along the contour.
2. Spatial veto (`grainGuard` on, `grainSplit` off, `deband_vp_grain_guard: veto`): per 16x16 block, the share of busy pixels (Laplacian or local extremum, `GrainPreserve::Guard*Codes`) vetoes the blur.
3. Deband: four samples on a ring whose angle and distance come from a hash of the pixel position, so a still picture never shimmers. The ring mean replaces the pixel with weight 1 below 0.6 x threshold and 0 at the threshold. No grain is added.
   - Hold (slot 0 only): with a field, on trusted blocks, inside this frame's blur band, the weight is mixed with the previous frame's weight read back along the vector.
   - Vector grain veto (only without the split): blocks with `grainError` above `grainCodes` are never blurred.

### Data flow

In: the `pl_frame` planes (any count, up to 3 components each). Out: a copy of the frame whose plane textures are the module's `out[slot][plane]`. The module owns them; they are overwritten by the next call on the same slot and freed by `Release`. The weight textures (`weight[2][plane]`, r16f) belong to slot 0. A repeat of the same `sequence` is recomputed against the same previous weights and never advances them.

Call sites (`src/VP-V/segments/EntranceSegment.cpp`, `Segment::Deband`, run in the slot registry's entrance order after the re-base):

- Main, drawn as itself: slot 0, with the field when it is live for this frame.
- Main, Stage B phase picture: A itself through slot 0 once per source frame (the hold advances on A), then the picture through slot 1 without a field.
- De-judder extra: slot 1, no field.
- Crossfade path being left: `kSlotCrossfade`, no field, radius scaled by the factor ratio.
- Detail-pulse look-ahead (`DetailPulse::Module::MeasureAhead`): `kSlotLookAhead`, no field, the next frame's sequence.

The host (`LibplaceboVideoRenderer.cpp`) owns the `Module` and sets `debandThreshold` and `vpDebandParams` once per frame. The threshold is `Strength(...)` from the source-resolution monitor's reading; `deband_vp: on` raises it to at least one 8-bit step x 1.5, and smooth motion or deinterlacing sets it to 0. `holdAlpha` comes from `GeneralRate::PerFrameWeight`. The field and grain error come from the de-judder only when its live field is for this source frame. `ForgetHold` is called on motion invalidations and on a re-base factor change; `CarryHold` on a skipped source frame.

### Threading

Render thread only: the host's frame setup and the entrance segment. There are no locks or atomics in the module.

### Config keys actually read

From the renderer profile sections that apply (`[vprenderer.Default]`, `[vprenderer.<name>]`, `[vprenderer.input.<name>]`; `ApplyDisplayRuleOverrides`). All of them are validated in RendererProfileConfig.h.

- `deband_vp`: `off` (default) / `auto` / `on`. Off: the module is never called.
- `deband_vp_grain_guard`: `on` (default, the grain split) / `veto` (spatial-share veto) / `off`. Maps to `grainGuard` / `grainSplit`.
- `low_bitrate_handling`: `auto` (default) / `off` (no bitrate term) / `on` (quality capped at 0.4). It is shared with the upscale blend weighting.
- `source_coding_fixes` (input.source_analysis): `snapLevels` in `Strength`. `general_rate_fixes`: the hold weight above 60 Hz.

libplacebo's deband, which shares the slot (the host, not the module):

- `deband_vp_exclusive`: `on` (default) / `off`. On, with `deband_vp` not `off`, a grain guard, `deband_strength: auto` and `deband: auto`, libplacebo's deband is switched off.
- `deband`: `auto` / `on` / `off`. `deband_strength`: `auto` / `off` / `light` / `default`. `deband_grain`: `on` / `off` (default off). Read from the base `[vprenderer]` section and from the profiles.

The Original pathway sets `deband_vp`, `deband_vp_exclusive` and `low_bitrate_handling` to `off`.

### User-visible fallbacks / notices

- "Fallbacks engaged: VP deband (off on the DV tunnel planes (no deband after decode yet))": `deband_vp` not `off` on a Dolby Vision tunnel picture. This is the only entry this slot produces.
- The planned entry `Deband: libplacebo deband (VP debander idle: <reason>)` is never produced. `FallbackCatalog::PlanDeband` (a libplacebo stand-in for a failed VP debander, id `deband`) exists and is tested but not wired into the host.
- A failed `Apply` on the main frame renders the frame without VP deband. libplacebo's deband stays yielded, so the frame has no deband at all. This is log-only, and logged only when the debander's total failure count is 1 ("VP deband failed (...); the frame is rendered without it"). Failures on the extra, crossfade and look-ahead slots are silent.
- Status line (the renderer's processing list, not "Fallbacks engaged"): "VP deband: threshold N codes", "idle (...)" or "not applied while smooth motion or deinterlacing runs", with "; held along the vectors (N frames)". With smooth motion or deinterlacing on, libplacebo's yielded deband is turned back on for those frames. That is log-only ("libplacebo deband: on for temporal frames").

### Dependencies to lift it

libplacebo (`renderer.h`, `dispatch.h`, compute shaders), `src/VideoProcessor-Lib/vprenderer/GrainPreserve.h` (the split and guard constants) and `pch.h`. The module is compiled into VideoProcessor-VPRenderer(.Beta) and VideoProcessor-Test. The forwarding header src/VideoProcessor-Lib/vprenderer/VpDebander.h keeps old includes building until M2. `DetailPulse.h` (repair.texture_restore) includes this header.

### Test it standalone

- CPU: `GrainPreservationTests` methods `VpDebander*` (the reference model of the guard and the split, GrainReferenceModel.h), and `LibplaceboLutParserTests::VpDebanderStrengthSnapsOccupancyReadingToContentLevels` (`Strength` only).
- GPU (D3D11, WARP unless `VP_TEST_GPU=hardware`; run only when GPU tests are allowed): `LibplaceboLutParserTests` methods `VpDebanderSmoothsBandsKeepsTextureAndHoldsAlongTheVectors`, `VpDebanderKeepsGrainWithoutVectors` and `VpDebanderGrainSplitDebandsNoisyBands`. Through the renderer (`GpuHardware`, skipped without hardware): `RendererCaptureHarnessTests::VpDebanderRunsThroughTheRenderer`.
- `FallbackCatalogTests::DebandSubstitutesOnlyForAFailedVpDebander` (CPU) tests the unwired plan.

Run single-threaded:

`vstest.console.exe x64\Release\VideoProcessor-Test.dll /Platform:x64 /TestCaseFilter:"FullyQualifiedName~GrainPreservationTests::VpDebander|FullyQualifiedName~VpDebanderStrengthSnaps" /Logger:"trx;LogFileName=deband.trx"`

## Settings

- Existing keys: `deband_vp`, `deband_vp_exclusive`, `deband_vp_grain_guard`, `deband`, `deband_strength`, `deband_grain`; the code also reads `low_bitrate_handling` (see above)
- Planned keys: (none)
- Tiers: Classic libplacebo deband; Low..Upper VP deband

## Fallbacks

- "Fallbacks engaged" entry: `VP deband (off on the DV tunnel planes ...)` is the only one produced. The listed `Deband: libplacebo deband (VP debander idle: <reason>)` is not implemented.
- Behaviour: VP debander failure renders without it and without libplacebo's deband (log-only); temporal frames keep libplacebo's deband.

## Net fingerprint / upstream

- Fingerprint: deband on/off is an unlabelled robustness input to the refine net
- Upstream modules: `repair.encoding`

## How to build / test

Build the solution via the shared build-slot runner (see AGENTS.md); tests single-threaded; GPU tests only on mains with `VP_TEST_ADAPTER=NVIDIA`.

Placeholder tests:
- (none yet)

Isolation tests:
- VpDebander GPU tests
- real-pan shimmer x52 (queue item 1)

Contract tests:
- source-resolution run

Chain tests:
- deband + re-base + 4K route

## Known issues / open items

- NEED with A on (does A remove most banding?) - decide from the results
- x52 pan shimmer on 1080->1600 (item 1)

## Where the code lives (live file list)

Code location: **migrated**. The authoritative list is `live_files` in [MANIFEST.json](MANIFEST.json).

- `src/VP-V/modules/repair/deband/VpDebander.cpp`
- `src/VP-V/modules/repair/deband/VpDebander.h`
- `src/VideoProcessor-Lib/vprenderer/LibplaceboVideoRenderer.cpp`

Forwarding headers at the legacy path (removed in M2; they only keep files on active branches building, new code includes the `<VP-V/...>` path):

- `src/VideoProcessor-Lib/vprenderer/VpDebander.h`

The call-site glue (`Segment::Deband`) is in the entrance segment, `src/VP-V/segments/EntranceSegment.cpp`. The host keeps the module and its per-frame parameters.

## Migration directive

Code moves into this folder only when this slot is touched (opportunistic until M1, a dedicated phase after M1, finished by M2). A move = `git mv` + project/include updates + this module's `live_files` + `docs/design/components-master.json` (regenerate with `python tools/check_module_manifests.py --write`) in the SAME commit, with the build, this module's tests and the chain-gate subset passing. Never leave a copy behind; one module per change; never during a release/deploy window. Update this README in the same commit as any slot change.

## Links

- Design: [VP-pipeline-final.md](../../../../../docs/design/VP-pipeline-final.md)
- Slot data: [slots.json](../../../../../docs/design/slots.json)
- Decisions: ADR-0009 in [docs/decisions](../../../../../docs/decisions/README.md)

<!-- vpv-settings-tests:begin (generated by tools/vpv_tests/seed_module_readmes.py) -->

## Settings

- Keys: `deband_vp`, `deband_vp_exclusive`, `deband_vp_grain_guard`, `deband`, `deband_strength`, `deband_grain`, `low_bitrate_handling`.
- Tiers: Classic libplacebo deband; Low..Upper VP deband.
- VP-V page, Repair tab: tiers Classic / Low / Mid / Upper; tier keys `deband_vp`, `detail_pulse_stabilise`, `texture_restore`.
- Listed under the tier: `deband_vp_grain_guard`, `deband_vp_exclusive`, `low_bitrate_handling`.
- Plain-language guide: [docs/settings/VP-settings-reference.md#4-repair](../../../../../docs/settings/VP-settings-reference.md#4-repair).
- Fallback entry ("Fallbacks engaged"): `VP deband: off on the DV tunnel planes (no deband after decode yet)`.

## Tests

- Placeholder (byte-identity on/off, zero cost): n/a.
- Isolation (fixed inputs, expected outputs, cost): VpDebander GPU tests; real-pan shimmer x52 (queue item 1).
- Contract (formats, geometry, fallback entry): source-resolution run.
- Chain (total delay at the 4/3/2-frame budgets): deband + re-base + 4K route.
- Combinations: pairwise / high-risk / random lines in `tests/vpv/` that set this slot's unit (`tools/run_combinations.ps1`); matrix: `docs/design/slot-test-matrix.md`.

<!-- vpv-settings-tests:end -->
