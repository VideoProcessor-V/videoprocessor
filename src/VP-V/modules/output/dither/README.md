# Dither / sigma-delta (`output.dither`)

Unit: **output** | side: output | order: 260 | status: **built** | hook point: OUTPUT

## Purpose / why

Quantise last for output links/displays of limited bit depth (8- or 10-bit): temporal sigma-delta (with vectors) or libplacebo dither; 12-bit / DV output paths later.

## Contract

- Input: projector codes in float
- Output: link bit depth codes
- Geometry: output size
- Delay share: 0 frames
- Look-ahead: none

## Settings

| Key | Section | Type | Range / values | Default | Config placement | Original (Bill) | Fallbacks / status | Tiers |
|---|---|---|---|---|---|---|---|---|
| `shaders.cinema_only` | shader rule section [shader.<name>] (per rule) | bool | true, false | false | not exposed in Config (cfg only) | Bill's custom hook entries; cinema_only is VP's addition | libplacebo backend only (custom GLSL hook rules): true skips the hook in the Low_Latency queue; a value that is not true/false marks the rule invalid. | not tiered |
| `dither_need` | profile (draft, header-only) | enum | off, auto (case-insensitive; other values give an error and off) | off | none yet (cfg only) | n/a (VP-only) | declared (draft, DitherNeed::Parse only, not read by the host): auto adds extra sigma-delta amplitude only where the grain does not dither the link and one link step is visible; off = today's amplitude. | not tiered |
| `dither_grain_tolerance_scale` | profile ([vprenderer.Default] and matching profiles) | float | 1.0 - 8.0 | 3 | VP-V > Output > Advanced > Temporal dither: tuning | n/a (VP-only): unchanged by the Original pathway | The sigma-delta's motion-match tolerance is multiplied by this on grain (1 = off), keeping the averaging alive on grain | same on all (not tiered) |
| `dither_vectors` | profile ([vprenderer.Default] and matching profiles) | enum | auto, own | auto | VP Renderer > Rendering > Temporal dither | n/a (VP-only): unchanged by the Original pathway | auto = the motion module's chain (motion/estimation DitherVectorSource.h): a verified shared pair, else the matcher's field of this frame, else an on-demand pair, else the moved output-domain search, static when the rule's motion_compensation 0 (a rule that sets it; the default is 1 since owner 2026-11-11 22:39, doc 91:108; the shader //!PARAM stays 0.0); own = always the output-domain search; proposed for retirement (group C); status: Ctrl+I segment "dither vectors: <step>" + counters, fallback entries dither vectors matcher / on demand / own search / unvalidated + dither motion compensation (Ctrl+I "dither motion compensation: on or off (<reason>)") | same on all (not tiered) |
| `occlusion_mask` | profile ([vprenderer.Default] and matching profiles) | bool | on, off | off | VP-V > Output > Main > Temporal dither; also VP Renderer > Rendering > Temporal dither (upstream copy greyed while VP-V is on) | Original pathway sets `off` (ADR-0019) | Tells the sigma-delta where new picture appears so it carries no residual into it (no smears at moving edges); works with dejudder_strength 0 (mask-only mode). status line: dither: libplacebo dither | same on all (not tiered) |
| `output_segments_merged` | [vpv] (global; also read from the profile) | bool | on, off | off | VP-V > Pipeline (expert) (stored in [vpv]) | Original pathway sets `off` (ADR-0019) | Draft: the S2/S3 output chain as one PRE_OUTPUT and one OUTPUT hook; off until its GPU tolerance gate passes | same on all (not tiered) |
| `temporal_dither` | profile ([vprenderer.Default] and matching profiles) | enum | auto, off, libplacebo, sigma_delta | auto | VP-V > Output > Advanced > Set by the Output tier | Original pathway sets `libplacebo` (ADR-0019) | The dither as one choice per profile: off, libplacebo's, or VP's temporal sigma-delta (loaded by the renderer itself); auto = the shader list and the dithering setting decide. Spreads steps finer than the output depth over time along the motion (keeps 10-bit smoothness through an 8-bit link) | Output Classic=libplacebo; Output Low=auto; Output Mid=sigma_delta |


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
- `dither_vectors`: `own` / `auto` (auto = the motion module's chain (motion/estimation DitherVectorSource.h): a verified shared pair, else the matcher's field of this frame, else an on-demand pair, else the moved output-domain search, static when the rule's motion_compensation 0 (a rule that sets it; the default is 1 since owner 2026-11-11 22:39, doc 91:108; the shader //!PARAM stays 0.0); own = always the output-domain search).
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
- Planned keys (not yet implemented): `dither_need`.
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

### Dither need (`dither_need`, WP4a/WP5a; header-only, default off)

Key `dither_need`: `off` (default) | `auto`. Off is byte-identical to the sigma-delta above (the shader's `dither_need` parameter stays 0 and nothing is bound).

Files: `DitherNeed.h` (the law, temporal state, sampling, `Parse`, status, the `dither.need` product declaration; CPU reference templated on `T`, double = reference, float = shader mirror), `DitherNeedGlsl.h` (block pass, one thread per region block, same arithmetic order), `DitherNeedGpu.h` (libplacebo wrapper `BlockPass`, `HookGuard`). Python mirror: `tools/eval/dither_need_ref.py`; golden: `tests/DitherNeedGolden.generated.h` (`tools/eval/gen_dither_need_golden.py`). Tests: `DitherNeedTests` (CPU) and `DitherNeedWarpTests` (WARP) at the end of `tests/SigmaDeltaHookTests.cpp`. Move them to their own `.cpp` when the PROJ zone lock is free.

What it does: the extra amplitude is raised only in banding-prone picture blocks (not overlay) where one LINK code step is visible after the context's display mapping (`gVis`, from the light-level `V` and the output slope `Sout`) and the grain is below the link step (`sigmaOut = sqrt(N) * |Sout| / q_out`). `aTarget = clamp(aNeed - sigmaOut, aBase, max(aMax, aBase))`, `extra = banding * gVis * (aTarget - aBase)`. The total amplitude never exceeds `aMax` = 0.5, so exact codes (black, BTB, WTW) are never moved. The per-block extra is smoothed (time constant 0.25 s from the MEASURED frame interval, deadband 0.02) and reset at a cut, resync, re-base, context change, grid, LUT, colour-depth or base-amplitude change.

Light-level dependence: extra dither exists only where a link step is visible (`gVis > 0`), i.e. mainly in dark smooth gradients after the context's mapping (SDR, HDR passthrough or HDR to SDR); bright regions, where the step is invisible, get none. `NeededAnywhere` (once per LUT fingerprint) skips the whole pass when no code has a visible link step: status "dither need: not needed (link step invisible)".

Missing input (no grain split, mismatched grid, no precursors, no LUT, unsupported domain): extra = 0, i.e. today's amplitude, with a named status (`FallbackCatalog::DitherNeed`, id "dither need") and automatic recovery.

Cost: the product is the block-grid state, rgba32f, two textures (ping-pong), 2 x blocks x 16 B. The block grid is about 120 x 68 at every size (16 px at 1080, 32 at 2160, 64 at 4320): ~0.26 MB VRAM at 1080p, 4K and 8K alike. Per output pixel: 4 texel fetches + ~10 ALU in the quantiser; the block pass is one thread per block. Not needed / off: no pass, no fetch.

Link noise versus the grain rule: the added dither is judged by its OWN limit, not the +-5 % grain rule: noise vs plain+4 at most 1.40 in shadow blocks where `sigmaOut < 0.35` (the grain is already below the link step, so the "grain" there is quantiser noise) and 1.05 elsewhere (`tools/eval/link_noise.py`); to be confirmed by eye on a limited-bit-depth display.

GPU decision and skipped frames: `DitherNeed::StateTracker::Decide(FrameArgs)` takes the reset / off / missing decision (grid, LUT fingerprint, colour depth, aBase, the caller's discontinuity flag, inputs present, needed) from CPU data only; `MapT` (CPU) and `Gpu::BlockPass::Step` (GPU: Decide + input validation of the grain, precursor and LUT textures against the block grid + the pass) use the same tracker. A skipped, missing or failed frame calls `BlockPass::Invalidate()`: `Current()` is null, `HookGuard::Apply` binds nothing and the sigma-delta runs at its base amplitude. Extra `Missing` bits: `kLutSizeMismatch`, `kNoSetTexture` (libplacebo without `pl_mpv_user_shader_set_texture`), `kPassFailed`.

Acceptance note (A2): on HDR passthrough the 2-frame-mean <5 nit geo-mean is 1.025 against the 1.02 gate (prototype 1.011), a marginal miss accepted by the planner pending the owner's display A/B; the per-frame ratio 0.609 passes (gate 0.8).

## Output-domain map (K7-1, pure unit, not wired)

`OutputDomainMap.h` maps source-plane points and vectors (P8 model vectors) to output pixels: active picture / crop and the
output scale, the NLS stretch (the hook's law, inverted by bisection), and the P0 re-base plane (reported, it cancels in the output
position). Out-of-picture points are flagged. Consumers: the sigma-delta carry reset on camera motion (K7-2) and the debander
hold (K7-3). Tests: `tests/OutputDomainMapTests.cpp`.

## All settings (complete list)

Config placement: master VP-V page, Output sub-page (`temporal_dither` is the Output tier's key; the others sit under Advanced). "Original (Bill)" = the Original pathway (`core/OriginalPathway.h`).

| Key | Section | Type | Range | Default | Config placement | What Original (Bill) does | Visible fallbacks / status lines | Tier differences |
|---|---|---|---|---|---|---|---|---|
| `temporal_dither` | profile (`[vprenderer.<profile>]`) | choice | auto, off, libplacebo, sigma_delta | auto (the shader list decides) | VP-V > Output (Advanced; tier key) | forced `libplacebo` | `Dither: libplacebo (sigma-delta dropped: <reason>)` under Fallbacks engaged | Classic = libplacebo, Low = auto, Mid = sigma_delta |
| `dithering` | profile | choice | auto, on, off, blue_noise, ordered_lut, ordered_fixed, white_noise, error_diffusion_* | auto | VP-V > Output > Dithering (VP-V list) (`config.vprenderer.scaling.dithering.vpv`, the full list; edits the Scaling profile selected on VP Renderer > Scaling) | Bill's own key (libplacebo dither); Original = Bill's Dithering on VP Renderer > Scaling, his four choices (a VP-V-only value shows there as his "Saved legacy method") | none | none |
| `dither_vectors` | profile | choice | auto, own | auto (the motion module's vector chain, W2-C8) | Rendering > Profile details (proposed retirement) | not forced | Ctrl+I "dither vectors: <step>" + "dither motion compensation: on or off (<reason>)"; dither vectors matcher / on demand / own search / unvalidated / dither motion compensation | none |
| `dither_grain_tolerance_scale` | profile | number | 1..8 (1 = off) | 3 | VP-V > Advanced | not read (no sigma-delta) | none | none |
| `output_segments_merged` | global | choice | on, off | off | VP-V > Pipeline (expert) | forced off | none | none |
| `dither_need` (header-only) | profile | choice | off, auto | off | none yet | not built | none | none |
| `OutputDomainMap` (internal; no key yet, K3 adds `camera_*` keys) | internal | - | - | - | none (internal) | not built | a map that is not valid is the identity on the raster; the consumer keeps today's behaviour | none |

Tiers: Classic libplacebo dither; Low .. Upper sigma-delta.

**Downstream consumers (TEMPORAL DETAIL, docs 71 section Q5 and 72):** per-pair homography (P7/P9), the object layer (per-object motion), P8 per-block membership + confidence, and K4 guided residuals.

## VP_INCLUDE and the vector chain (W2-C8)

### Motion compensation default (owner 2026-11-11 22:39, doc 91:108)
The sigma-delta rule parameter `param_motion_compensation` (a shader-rule parameter, not a VP-V setting key) now defaults to 1: CustomHookChain adds `motion_compensation = 1` to every quantiser rule whose shader declares the parameter and whose rule does not set it (the shader //!PARAM itself stays 0.0, so the expanded file stays byte-identical to b5ae6eda5). The Original pathway removes the sigma-delta (temporal_dither libplacebo), so it is unaffected. Per source frame (RunDejudderPolicy, DitherVectorSource.h DecideCompensation) the carry runs WITHOUT compensation (the plain motion gate, the behaviour before the default), visibly, when the pair ending at the frame is not valid (discontinuity: resync / queue reset / seek / format or settings change; a dropped frame; refused resync stamps) or the motion engine is off: Ctrl+I "dither motion compensation: on or off (<reason>)" + counters, fallback entry "dither motion compensation" (held 3 s). A cadence repeat keeps it on (zero motion, static_refresh). The stream's first frame and a rule set to 0 are status-only. With compensation configured off the sigma-delta no longer asks the vector engine for vectors (C8-FINDING: the want condition).


- `VpShaderInclude.h`: a whole line `// VP_INCLUDE <id>` in a custom GLSL hook (a plain comment, never a `//!` directive) is replaced, before templates and parsing, by the GLSL the owning module publishes under `<id>`. Known id: `motion.output_domain_search` (`VpMotion::OutputDomainSearch::Glsl`, in motion.estimation). The inserted text takes the including line's line ends, so the expanded file equals the file the passes were cut from. The custom hook chain, the GPU tests' loaders and the Beta host share this one expansion; an unknown id is an error with a reason.
- `CustomHookChain::Chain::QuantizerParameter(name, value)`: the current value of a `//!PARAM` of the loaded quantiser (the rule's value or the shader default); the renderer reads `motion_compensation` through it for the chain's step 5.
- `shaders/Temporal sigma-delta.glsl` keeps the sigma-delta plus the VPVEC / VPPIX consumption only; the search passes are the include line. Where the block vectors come from (shared pair, matcher field, on-demand pair, own search, static) is decided in motion.estimation (`DitherVectorSource.h`).
- Settings rows are unchanged: `dither_vectors` is still `auto | own`; no new key. `own` = the module's search directly.
