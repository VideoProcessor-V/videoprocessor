# Geometry (`scaling.geometry`)

Unit: **scaling** | side: processing | order: 150 | status: **built** | hook point: libplacebo

## Purpose / why

Crop/active picture, A-lens (anamorphic), NLS, zoom/viewport.

## Contract

- Input: processing-size picture + active area
- Output: source and target rectangles for libplacebo; NLS hook
- Geometry: defines output active size (feeds the processing-size rule)
- Delay share: 0 frames
- Look-ahead: active-picture decisions use queued frames

## How the code works

### NLS geometry policy (migrated: `NlsGeometryPolicy.h/.cpp`)

Pure C++ decisions (global functions, no namespace, no GPU, no Windows). They decide whether and how the non-linear stretch maps the picture onto the screen aspect. The shader itself (`shaders/NLS.glsl`, `shaders/NLSPlus.glsl`) and its hook binding stay in the renderer.

- Types:
  - `NlsMappingMode`: `OFF`, `WAITING`, `LINEAR_PASSTHROUGH`, `ACTIVE`, `SAFE_FIT`.
  - `NlsAspectDirection`: `NARROWER_ONLY`, `WIDER_ONLY`, `ANY`.
  - `NlsPresentationCropPreference`: `PRESERVE_IMAGE`, `MINIMIZE_DISTORTION`.
  - `NlsMappingDecision`: mode, aspects, requested / maximum / applied stretch ratio, warp axis, safe-fit fraction and axis, reason.
  - `NlsSourceGeometry`: rectangle + aspect + valid.
  - `NlsPresentationCropDecision`.
- Constants: `NLS_MINIMUM_STRETCH_RATIO` 1.0, `NLS_DEFAULT_MAXIMUM_STRETCH_RATIO` 1.4, `NLS_SHADER_MAXIMUM_STRETCH_RATIO` 1.5 (the shaders' own clamp).
- `EvaluateNlsMapping(...)`: the mode for an active aspect against a target aspect.
  - `WAITING` when the ratio limit is invalid, the geometry is not stable, the target is unknown or the picture is below `active_aspect_min`.
  - `LINEAR_PASSTHROUGH` within `tolerance_percent`, or when `aspect_direction` excludes the direction.
  - `SAFE_FIT` (letterbox/pillarbox, no warp) when the needed ratio exceeds `max_stretch_ratio`.
  - Otherwise `ACTIVE`, with a vertical warp when the picture is wider than the target.
  - A Boolean `narrowerOnly` overload is kept for old callers.
- `ResolveNlsSourceGeometry` / `ResolveNlsPresentationSourceGeometry`: the rectangle NLS maps. An accepted viewport crop wins; else the trusted active picture (so NLS can drop encoded bars without automatic crop); else the full raster.
- `ResolveNlsPresentationCrop(...)`: an optional bounded crop (`vprenderer_max_crop_percent`, 0..10 % per edge) along the long axis.
  - `preserve_image` crops only to turn a `SAFE_FIT` into a mapping within the ratio limit.
  - `minimize_distortion` crops up to the limit to reduce the stretch.
  - The cropped result must evaluate to `ACTIVE` or `LINEAR_PASSTHROUGH`.
- `NlsOwnsPresentationGeometry(...)`: whether NLS owns the source rectangle this frame. Never with a fixed viewport crop or while the presentation fails open.
- `ResolveNlsTargetAspect`: the configured aspect, else the panel's. Called only by tests today.
- Name helpers: `NlsMappingModeName`, `NlsMappingAxisName`, `NlsAspectDirectionName`, `NlsPresentationCropPreferenceName`.

Data flow:

1. `MadVRShaderLoader` (src/VideoProcessor-Lib/microsoft_directshow) loads the selected NLS profile into a `ShaderRule` (ratio limit, tolerance, direction, crop limit/preference, shader parameters). MadVRShaderRuntimeState.h includes this header and builds the presentation plan from its types.
2. VP Renderer, render thread (LibplaceboVideoRenderer.cpp), each frame:
   - the physical target aspect comes from `AnamorphicPresentation::PhysicalTarget` (screen profile aspect, lens scale, panel);
   - fail-open conditions are checked: crop recovery, moving-picture transition, near-black full raster, admission block;
   - then `ResolveNlsPresentationSourceGeometry`, `ResolveNlsPresentationCrop`, `EvaluateNlsMapping` and `NlsOwnsPresentationGeometry` run. The decision drives the source crop, the hook's stretch ratio and axis, and the de-judder / occlusion-mask geometry.
3. The DirectShow renderer calls `MadVRShaderLoader::EvaluateNlsMapping`, which wraps the same function.

Threading: stateless functions; each caller owns its inputs. The renderer calls them on its render thread.

### Config keys actually read

NLS is chosen by the `nls` selection group. A profile is a `[shader.nls.<name>]` section with `shader_type: nls`. Keys read there by MadVRShaderLoader.cpp:

- Policy: `max_stretch_ratio` (1.0..1.5, default 1.4), `tolerance_percent` (0..50; the old alias `aspect_tolerance_percent` cannot be set at the same time), `active_aspect_min` (1..4), `aspect_direction` (`narrower_only` / `wider_only` / `any`), `vprenderer_max_crop_percent` (0..10), `vprenderer_crop_preference` (`preserve_image` / `minimize_distortion`), `nls_target_aspect_ratio`, `inactive_rule`.
- Shader parameters: `strength`, `geometry`, `center_protection`, `curve`, `quality`, `axis_balance`, `max_center_zoom`, `horizontal_center_protection`, `vertical_center_protection` (also as `param_<name>`).
- Shader files and stage: `glsl_file`, `hlsl_file`, `stage`, `order`.

An invalid value logs a line and marks the rule invalid.

Outside the profile:

- `nls_sampling` (`bilinear` / `catmull`) per renderer profile (`[vprenderer.<name>]`).
- The target aspect from the screen profile's `screen_aspect` (`[vprenderer.viewport.<name>]`).

The keys `nls_active`, `nls_curve`, `nls_geometry`, `nls_protect` and `nls_scale` listed in slots.json do not exist in the code.

### User-visible fallbacks / notices

- NLS hook failure: libplacebo disables the hook; `NlsHookRecovery::RenderOwned` withdraws it and redraws the frame with the lens-aware linear fit. The mode becomes `SAFE_FIT`; the shader status line shows "NLS: Safe fit (hook failure)"; this frame's de-judder extras hold.
- The planned "Fallbacks engaged" entry `Geometry: NLS safe fit (hook failure)` is not implemented: no `EngageFallback` call exists for NLS.
- A ratio above `max_stretch_ratio` gives `SAFE_FIT` by design (the decision carries the reason); it is not a fallback.

### Dependencies to lift it

`NlsGeometryPolicy.*`: C++ standard library and `pch.h` only. It is compiled into VideoProcessor-Lib. The rest of the slot needs the crop pipeline (`AlphaSourceCropPolicy.*`), `AnamorphicPresentation.h`, `NlsHookRecovery.h`, `NlsMaskMapping.h`, the shader loader and the GLSL shaders.

### Test it standalone

- CPU (most direct coverage of the policy functions): `MadVRShaderRuntimeStateTests`, `AlphaSourceCropPolicyTests` (NLS cases, e.g. `AnamorphicPhysicalNlsRatioDoesNotIncludeLens`, `SceneHoldExpiryWithdrawsCropAndNlsTogether`), `PictureTransitionHandoffTests`.
- GPU (D3D11; WARP unless `VP_TEST_GPU=hardware`; run only when GPU tests are allowed): `LibplaceboLutParserTests` NLS methods (`NlsHookFailureRetryLeavesNoStaleDejudderCache`, `BundledNlsGlSlHooksMovePixelsOnTheRealGpuPath`, ...).

Run single-threaded:

`vstest.console.exe x64\Release\VideoProcessor-Test.dll /Platform:x64 /TestCaseFilter:"FullyQualifiedName~MadVRShaderRuntimeStateTests|FullyQualifiedName~AlphaSourceCropPolicyTests|FullyQualifiedName~PictureTransitionHandoffTests" /Logger:"trx;LogFileName=geometry.trx"`

## Settings

- Existing keys: the `nls` selection group (`[shader.nls.<name>]` profiles; keys above) and `nls_sampling`. The listed `nls_active`, `nls_curve`, `nls_geometry`, `nls_protect`, `nls_scale` do not exist.
- Planned keys: (none)
- Tiers: not tiered

## Fallbacks

- "Fallbacks engaged" entry: Geometry: NLS safe fit (hook failure) (planned; not implemented, shown only as the shader status "NLS: Safe fit (hook failure)")
- Behaviour: NLS hook failure redraws with the lens-aware linear safe fit (in code).

## Net fingerprint / upstream

- Fingerprint: (none)
- Upstream modules: `input.source_analysis`

## How to build / test

Build the solution via the shared build-slot runner (see AGENTS.md); tests single-threaded; GPU tests only on mains with `VP_TEST_GPU=hardware` (`VP_TEST_ADAPTER=NVIDIA` picks the GeForce).

Placeholder tests:
- (none yet)

Isolation tests:
- NLS policy: `MadVRShaderRuntimeStateTests`, NLS cases in `AlphaSourceCropPolicyTests` (no class is named NlsGeometryPolicyTests)
- AnamorphicPresentation: cases in `AlphaSourceCropPolicyTests` and `LibplaceboLutParserTests` (no dedicated class)

Contract tests:
- active area propagated to every net slot

Chain tests:
- A-lens/NLS + re-base + vectors

## Known issues / open items

- NLS hook failure has no "Fallbacks engaged" entry (log/status only)

## Where the code lives (live file list)

Code location: **legacy**. The authoritative list is `live_files` in [MANIFEST.json](MANIFEST.json).

- `src/VideoProcessor-Lib/NlsGeometryPolicy.cpp`
- `src/VideoProcessor-Lib/vprenderer/NlsMaskMapping.h`
- `src/VideoProcessor-Lib/vprenderer/NlsHookRecovery.h`
- `src/VideoProcessor-Lib/vprenderer/AnamorphicPresentation.h`
- `src/VideoProcessor-Lib/vprenderer/AlphaSourceCropPolicy.cpp`
- `shaders/NLS.glsl`
- `shaders/NLSPlus.glsl`

## Migration directive

Code moves into this folder only when this slot is touched (opportunistic until M1, a dedicated phase after M1, finished by M2). A move = `git mv` + project/include updates + this module's `live_files` + `docs/design/components-master.json` (regenerate with `python tools/check_module_manifests.py --write`) in the SAME commit, with the build, this module's tests and the chain-gate subset passing. Never leave a copy behind; one module per change; never during a release/deploy window. Update this README in the same commit as any slot change.

## Links

- Design: [VP-pipeline-final.md](../../../../../docs/design/VP-pipeline-final.md)
- Slot data: [slots.json](../../../../../docs/design/slots.json)
- Decisions: (none) in [docs/decisions](../../../../../docs/decisions/README.md)

<!-- vpv-settings-tests:begin (generated by tools/vpv_tests/seed_module_readmes.py) -->

## Settings

- Keys: `nls`, `nls_active`, `nls_curve`, `nls_geometry`, `nls_protect`, `nls_scale`.
- Tiers: not tiered.
- VP-V page, Scaling tab: tiers Classic / Low / Mid; tier keys `adaptive_downscale`, `adaptive_w_base`, `source_resolution`.
- Plain-language guide: [docs/settings/VP-settings-reference.md#5-scaling](../../../../docs/settings/VP-settings-reference.md#5-scaling).
- Fallback entry ("Fallbacks engaged"): `Geometry: NLS safe fit (hook failure)`.

## Tests

- Placeholder (byte-identity on/off, zero cost): n/a.
- Isolation (fixed inputs, expected outputs, cost): NlsGeometryPolicy tests; AnamorphicPresentation tests.
- Contract (formats, geometry, fallback entry): active area propagated to every net slot.
- Chain (total delay at the 4/3/2-frame budgets): A-lens/NLS + re-base + vectors.
- Combinations: pairwise / high-risk / random lines in `tests/vpv/` that set this slot's unit (`tools/run_combinations.ps1`); matrix: `docs/design/slot-test-matrix.md`.

<!-- vpv-settings-tests:end -->
