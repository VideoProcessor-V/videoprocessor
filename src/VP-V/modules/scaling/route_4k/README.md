# 4K route (entrance stage 2b) (`scaling.route_4k`)

Unit: **scaling** | side: input | order: 100 | status: **built** | hook point: prepass

## Purpose / why

Upscale planes to the derived processing geometry (`target = max(delivered, output)`, `dst = max(input, target)`) before render-time stages using the single `ScalerPass` unit (`scaling/shared/ScalerPass.h`) and the single `HybridBlend` law (`scaling/shared/HybridBlend.h`). Geometry ratios are derived from active delivered/output dimensions and `Scaling::GeometrySet` (`scaling/geometry/SourceGeometrySet.h`) — never hard-coded fixed x2 shaders or fixed resolution literals, and never downscaling an axis.

When `upscale_route` is `off` (default), the slot runs zero GPU passes and zero allocations, leaving the picture byte-identical straight to output scaling.

## Contract

- Input: planes at the encoded, rebased, or repaired size (`pl_frame`)
- Output: planes at the derived processing size in 32-bit float by default (16-bit float only when requested or when 32-bit float is unavailable, visibly recorded in `FormatNote()`). Unclamped float output preserves BTB (`< 0.0`) and WTW (`> 1.0`). Chroma subsampling is preserved, `shift_x`/`shift_y` are updated via `ChromaSiting::ShiftAfterResample`, and `crop` scales by `(dstW / srcW, dstH / srcH)`.
- Geometry: derived per-axis scale from `SourceGeometrySet` + delivered/output geometry; never downscales an axis; once upscaled to `dst >= output`, downstream `AdaptiveDownscale::UpscaleAxesOf` does not upscale again (no double upscale).
- Delay share: 0 frames
- Look-ahead: none

## Settings

| Key | Section | Type | Range / values | Default | Config placement | Original (Bill) | Fallbacks / status | Tiers |
|---|---|---|---|---|---|---|---|---|
| `upscale_route` | profile ([vprenderer.Default] and matching profiles) | enum | off, rebased_only, always_4k | off | VP-V > Scaling > Main > Grain-aware upscale | Original pathway sets `off` (ADR-0019) | Controls pre-render 4K route upscaling: `off` bypasses with zero GPU passes (byte-identical); `rebased_only` upscales only frames that were rebased; `always_4k` upscales whenever derived processing target exceeds input. On GPU failure engages `4K route: off (<reason>)` | same on all (not tiered; conservative off default) |

- Existing keys: `upscale_route`
- Planned keys: (none)
- Tiers: off by default across tiers (`rebased_only` / `always_4k` opt-in)

## Fallbacks

- "Fallbacks engaged" entry: `4K route: off (<reason>)` (`Route4k::Module::Fallback()` -> `FallbackCatalog::Engaged("4K route", ...)`)
- Behaviour: Off or failed -> `out` untouched on failure, `Failures()` incremented, `LastError()` populated; libplacebo scales straight to output.

## Net fingerprint / upstream

- Fingerprint: `ScalerPass` kernel spec + `HybridBlend` law (single source of truth)
- Upstream modules: `scaling.rebase`, `repair.texture_restore`

## How to build / test

Build the solution via the shared build-slot runner (see AGENTS.md); tests single-threaded; GPU tests only on mains with `VP_TEST_ADAPTER=NVIDIA`.

- Bypass: byte-identical with `upscale_route = off` / slot off / Original pathway; zero GPU passes and zero allocations.
- Geometry: derived ratios via `SourceGeometrySet` + delivered/output dimensions; never downscales an axis; no double upscale downstream (`WillDownstreamUpscale == false`).
- Blend & precision: luma sharp/safe blend uses `HybridBlend::WeightGlsl()` (`HybridBlendLumaReference`), preserves BTB `< 0.0` and WTW `> 1.0`, one-axis runs safe branch alone, chroma preserves subsampling and siting shifts.

## Where the code lives (live file list)

Code location: **migrated**. The authoritative list is `live_files` in [MANIFEST.json](MANIFEST.json).

- `src/VP-V/modules/scaling/route_4k/Route4k.h`
- `src/VP-V/modules/scaling/route_4k/Route4k.cpp`

## Migration directive

Code moves into this folder only when this slot is touched (opportunistic until M1, a dedicated phase after M1, finished by M2). A move = `git mv` + project/include updates + this module's `live_files` + `docs/design/components-master.json` (regenerate with `python tools/check_module_manifests.py --write`) in the SAME commit, with the build, this module's tests and the chain-gate subset passing. Never leave a copy behind; one module per change; never during a release/deploy window. Update this README in the same commit as any slot change.

## Links

- Design: [VP-pipeline-final.md](../../../../../docs/design/VP-pipeline-final.md)
- Slot data: [slots.json](../../../../../docs/design/slots.json)
- Decisions: ADR-0007 in [docs/decisions](../../../../../docs/decisions/README.md)
