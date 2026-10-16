# Scaling add-ons per shader (owner 2026-10-16 night) - plan

Status: agreed direction, not built. The committed Scaling slider (cc5917d5: Off / Fast / Best,
separable throughout) is an interim; this plan replaces it.

## Owner's direction

- The add-ons belong to shaders: a shader is selected in the shader list first, then **per shader a
  speed slider** sets which of that shader's add-ons are allowed. A shader that is not selected shows
  no slider (a note: "enable <shader> in the shader profiles").
- Speed profiles: Off lowest, each step costs more, levels genuinely distinct in cost.
- **Polar where able** (the restored `adaptive_faithful: auto`: polar when both axes shrink alike,
  separable when only one axis shrinks or the ratios differ > 10 %; polar cannot scale one axis
  alone). Measured earlier (BUILD-STATUS "Eff Faithful branch"): separable is ~2.4 ms cheaper but
  keeps ~82 % of the blend's effect.
- Mark every add-on that needs motion: "uses the de-judder's motion; with the de-judder off it runs
  the matcher (~X ms)".
- The 32-bit intermediates (`fbo_float32`) stay out of the sliders: precision, on by default; the VP
  libplacebo fork falls back by itself (32f -> 16f -> 16 unorm -> 8 unorm, renderer.c ~line 400) on
  cards without 32-bit float FBOs. Measure its real cost anyway (tooltip says "a few ms at high
  resolutions").

## Add-ons per shader (first cut; the measurements decide the levels)

| Shader | Add-on | Key | Needs motion |
|---|---|---|---|
| SSimDownscaler | SSIM / faithful blend | `adaptive_downscale` | no |
| | grain-true filter auto / separable / polar | `adaptive_faithful` (+ `auto`, to restore) | no |
| | grain map from motion | `adaptive_grain_map` | yes |
| | blend chroma too (off: fringes) | `adaptive_chroma_ssim` | no |
| | steady SSIM detail | `ssim_residual_stabilise` | yes |
| | gentler on poor streams | `ssim_bitrate_aware` (+ floor) | no |
| CNN (FSRCNNX / ArtCNN) | CNN / faithful blend | `adaptive_upscale` | no |
| | SuperRes strength follows grain | `adaptive_upscale_superres_scale` | no |
| | steady CNN detail | `cnn_residual_stabilise` | yes |
| | gentler on poor streams | `ssim_bitrate_aware` (shared) | no |
| Temporal sigma-delta | motion-carried vectors | dither keys (Dither slider) | yes |

Draft levels - SSimDownscaler: Off (plain) / Fast (+blend, separable, per-frame grain) / Standard
(+auto polar) / Best (+grain map, +steady detail). CNN: Off / Fast (+blend) / Best (+SuperRes
follows grain, +steady detail).

## Before building

1. How Config knows a shader is selected (exploration started 2026-10-16: shader profiles
   `[shader.standard.*]`, multi-select, rules / shortcuts / persisted selection - per profile or
   global?). Decide what "selected" means on the Config page.
2. Timings on the laptop RTX 3050 Ti (3090 later), each add-on where it actually runs: 4K -> 1080
   (downscale blend; polar vs separable vs auto), 1080 -> 4K (upscale blend with a CNN), 4K -> 4K (none
   run), the grain map with the de-judder on and off, `fbo_float32` on/off. Existing harnesses:
   Beta "option quality" (BetaGpuTests.cpp ~2283, `AdaptiveDownscale::Module`), Stage12PassCostsAt4K
   pattern (LibplaceboLutParserTests.cpp).
3. Restore `adaptive_faithful: auto` (AdaptiveDownscale `faithfulAuto`, ratio test in the faithful
   branch; reverted uncommitted 2026-10-16, easy to redo).
4. Then the per-shader sliders (choices only, same Custom / Reset / Default rules, measured ms in each
   level's description), Config tests; replace the interim Scaling slider.
