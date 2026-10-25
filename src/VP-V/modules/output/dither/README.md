# Dither / sigma-delta (`output.dither`)

Unit: **output** | side: output | order: 260 | status: **built** | hook point: OUTPUT

## Purpose / why

Quantise last for the 8-bit FL35 link: temporal sigma-delta (with vectors) or libplacebo dither; 12-bit / DV output paths later.

## Contract

- Input: projector codes in float
- Output: link bit depth codes
- Geometry: output size
- Delay share: 0 frames
- Look-ahead: none

## Settings

- Existing keys: `temporal_dither`, `dithering`, `dither_vectors`, `dither_grain_tolerance_scale`
- Planned keys: (none)
- Tiers: Classic libplacebo dither; Low..Upper sigma-delta

## Fallbacks

- "Fallbacks engaged" entry: dither: libplacebo dither; sigma-delta dropped: <reason>
- Behaviour: Unsupported code domain, shader not loaded or disabled by libplacebo drops the sigma-delta (FallbackCatalog::Dither; call site WP-4).

## Net fingerprint / upstream

- Fingerprint: (none)
- Upstream modules: `output.calibration_lut`

## How to build / test

Build the solution via the shared build-slot runner (see AGENTS.md); tests single-threaded; GPU tests only on mains with `VP_TEST_ADAPTER=NVIDIA`.

Placeholder tests:
- (none yet)

Isolation tests:
- SigmaDeltaDomain tests

Contract tests:
- the quantiser is the last OUTPUT hook

Chain tests:
- (none yet)

## Known issues / open items

- (none yet)

## How the code works

Checked against the code 2026-10-25 (M2 step 5 + WP-13).

### Public API

- `SigmaDeltaDomain.h`: `FromTarget(repr)` gives the code domain (`full`, swapchain `sample` depth, `color` depth; `supported` only for 8- to 10-bit codes). `Limits`, `ResidualBound`, `QuantiseStep`, `Ign`: a CPU model of the shader's quantiser for tests.
- `CustomHookChain.h`: the `[shader.standard.*]` GLSL hooks other than NLS (the sigma-delta among them). `Chain::Configure`, `Ensure`, `Collect(lowLatency, hooks)`, `SubmitsFile`, `IsQuantizerHook` (a hook that quantises the output clears libplacebo's dither), `MarkRuntimeFailures`. `SetHookParameter(hook, name, value)` sets a `//!PARAM` per frame.
- `OutputEncodingRepair.h`: `Needed(hooks, errors, lutType)` and `Hook` (first at OUTPUT): re-applies the output encoding libplacebo 7.360.1 drops after a texture-input PRE_OUTPUT hook (never needed with Beta 377).
- `SigmaDeltaVectors.h` (WP-13): `Supported()`, `Set` (VPVEC), `SetPixel` (VPPIX) through the libplacebo fork's `pl_mpv_user_shader_set_texture` (patch 0004; absent in 7.360, where the shader searches itself), and `NextVectorGuard` (B's first present takes A's carried vectors; restored after the render).

### Data flow

The renderer (render thread) collects the chain's hooks, then per frame: `static_refresh` on cadence repeats; the code domain from the target (`levels_full`, `swapchain_bits`, `color_bits`, `output_float32`), dropping the sigma-delta on an unsupported domain; the sigma-delta moved last if another OUTPUT hook follows it; the occlusion mask and grain tolerance just before it; the encoding repair first at OUTPUT; libplacebo's dither cleared when a quantiser is in the frame or `temporal_dither` is `off`. These steps become S3 in `feat/segment-runner` (OutputSegments). Vectors: the shader's own search, the de-judder's on the extras, or the next-frame guard.

### Threading

Render thread only; the chain's lazy loads run there too. No locks.

### Config keys actually read

- `temporal_dither`: `auto` / `off` / `libplacebo` / `sigma_delta` (the sigma-delta is added to the shader list if missing, or left out). `sigma_delta` is a value, not a key.
- `dithering`: libplacebo's dither method (Bill's key; used when the sigma-delta does not run).
- `dither_vectors`: `own` / `auto` (auto = the de-judder's vectors on the extras).
- `dither_grain_tolerance_scale`: 1.0 to 8.0 (1 = off): the tolerance on grain from the adaptive blend's grain map.
- The sigma-delta rule's `[shader.standard.*]` parameters are the shader's own.

### Fallbacks a user sees

- `dither` under "Fallbacks engaged" (`FallbackCatalog::Dither`): the sigma-delta asked for but dropped (unsupported code domain, the shader did not load, disabled by libplacebo); libplacebo's dither runs instead. The call site is WP-4's (`silent-fallbacks-callsites.patch`, at the domain step); on a base without WP-4 this reaches only the log.
- The encoding repair is a workaround, not a downgrade (logged once).

### What it needs to be lifted

libplacebo (the fork's set-texture call for vectors), `pch.h`, `DebugLog.h`, `vprenderer/HookInstanceTag.h`, `microsoft_directshow/MadVRShaderLoader.h` (rule types). The shader stays in `shaders/`.

### Test it standalone

- CPU: `CustomHookChainTests`; the sigma-delta model tests in `LibplaceboLutParserTests`; `OutputGlueStagesTests.NextVectorGuardTouchesOnlySigmaDeltaHooks`.
- GPU (only when allowed): `LibplaceboLutParserTests` sigma-delta methods, the `ChainGpuProbe` chain tests, Beta `BetaGpuTests` (the repair).

`vstest.console.exe x64\Release\VideoProcessor-Test.dll /Platform:x64 /TestCaseFilter:"FullyQualifiedName~CustomHookChainTests|FullyQualifiedName~OutputGlueStagesTests" /Logger:"trx;LogFileName=dither.trx"`

## Where the code lives (live file list)

Code location: **migrated**. The authoritative list is `live_files` in [MANIFEST.json](MANIFEST.json).

- `src/VP-V/modules/output/dither/SigmaDeltaDomain.h`
- `src/VP-V/modules/output/dither/OutputEncodingRepair.h`
- `src/VP-V/modules/output/dither/OutputEncodingRepair.cpp`
- `src/VP-V/modules/output/dither/CustomHookChain.h`
- `src/VP-V/modules/output/dither/CustomHookChain.cpp`
- `shaders/Temporal sigma-delta.glsl`
- `src/VP-V/modules/output/dither/SigmaDeltaVectors.h`
- `src/VP-V/modules/output/dither/DitherSlot.h`

Forwarding headers (legacy path; removed in M2 step 0): `src/VideoProcessor-Lib/vprenderer/CustomHookChain.h`, `src/VideoProcessor-Lib/vprenderer/OutputEncodingRepair.h`, `src/VideoProcessor-Lib/vprenderer/SigmaDeltaDomain.h`.

The forwarders stay for LibplaceboVideoRenderer.cpp (Bill's host) and the upstream LibplaceboLutParserTests.cpp, which this move does not edit; feat/segment-runner's OutputSegments.cpp also includes the old paths until its rebase. The sigma-delta shader stays in shaders/ (runtime layout). SigmaDeltaVectors.h and DitherSlot.h are the host's vector glue and slot switch (WP-13): call sites in docs/design/m2-output-callsites.patch.

## Migration directive

Code moves into this folder only when this slot is touched (opportunistic until M1, a dedicated phase after M1, finished by M2). A move = `git mv` + project/include updates + this module's `live_files` + `docs/design/components-master.json` (regenerate with `python tools/check_module_manifests.py --write`) in the SAME commit, with the build, this module's tests and the chain-gate subset passing. Never leave a copy behind; one module per change; never during a release/deploy window. Update this README in the same commit as any slot change.

## Links

- Design: [VP-pipeline-final.md](../../../../../docs/design/VP-pipeline-final.md)
- Slot data: [slots.json](../../../../../docs/design/slots.json)
- Decisions: (none) in [docs/decisions](../../../../../docs/decisions/README.md)

<!-- vpv-settings-tests:begin (generated by tools/vpv_tests/seed_module_readmes.py) -->

## Settings

- Keys: `temporal_dither`, `dithering`, `dither_vectors`, `dither_grain_tolerance_scale`, `output_segments_merged`.
- Tiers: Classic libplacebo dither; Low..Upper sigma-delta.
- VP-V page, Output tab: tiers Classic / Low / Mid; tier keys `temporal_dither`.
- Listed under the tier: `occlusion_mask`.
- Plain-language guide: [docs/settings/VP-settings-reference.md#8-output](../../../../../docs/settings/VP-settings-reference.md#8-output).
- Fallback entry ("Fallbacks engaged"): `Dither: libplacebo (sigma-delta dropped: <reason>)`.

## Tests

- Placeholder (byte-identity on/off, zero cost): n/a.
- Isolation (fixed inputs, expected outputs, cost): SigmaDeltaDomain tests; SegmentRunnerTests.DitherSlotOffRemovesTheSigmaDelta (switch honoured: output.dither off submits no sigma-delta); SegmentRunnerTests.DitherFallbackOnlyForDomainOrShaderNotForSlotOff.
- Contract (formats, geometry, fallback entry): the quantiser is the last OUTPUT hook.
- Chain (total delay at the 4/3/2-frame budgets): MISSING.
- Combinations: pairwise / high-risk / random lines in `tests/vpv/` that set this slot's unit (`tools/run_combinations.ps1`); matrix: `docs/design/slot-test-matrix.md`.

<!-- vpv-settings-tests:end -->
