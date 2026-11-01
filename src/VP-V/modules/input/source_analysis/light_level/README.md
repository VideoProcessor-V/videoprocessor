# Light level (`input.light_level`, DRAFT)

Unit **input** | draft, no slot wiring, no call sites | order 45 (after source_analysis) | CPU reference + GLSL mirror

## Light-level rule

Every repair weighs the local light level AS THE VIEWER SEES IT, not the coded value: the same code step is a large,
visible step in the shadows and a negligible one in the highlights, and the display (SDR EOTF with its black, or the HDR
tone map) decides which. The LUT turns an input code into four numbers: the luminance L the viewer sees, its
perceptual coordinate J, the local slope S and the visibility V of one code step. WP2 will add the readers: per-feature
strength weights from V (deband, grain, sharpening), J-space local statistics, and the host call sites and sampler
binding. Nothing here is wired yet.

## Contexts (`Context::kind`)

- **Sdr**: optional `SdrToneShape::Map` (when `sdr_tone_shape`/`sdr_white_headroom` are active), then the PHYSICAL
  display EOTF with white `light_level_display_white_nits` and black `light_level_display_black_nits` (auto =
  `sdr_target_nits` / `sdr_black_nits`). BT.1886: g = 2.4, a = (Lw^(1/g) - Lb^(1/g))^g,
  b = Lb^(1/g) / (Lw^(1/g) - Lb^(1/g)), L = a max(v + b, 0)^g. Numeric power: L = Lb + (Lw - Lb) v^g. sRGB: piecewise,
  likewise. Above white the curve continues and L is capped at Lw. Exact at the modelled defaults; Approx (reasons named,
  joined with "; ") when the display black is above 0, when `sdr_input_transfer` or `output_gamma` (read for labelling
  only; auto / 2.4 / bt1886 are the modelled spellings, any value matching `hdr_tone_map_target_gamma` is too) differs from
  the modelled display transfer, or when the SDR tone shape is active with sRGB (the shape uses power 2.2).
- **HdrPassthrough**: L = PQ EOTF(v), then a BT.2390 EETF roll-off in PQ space from [sourceMinNits, sourcePeakNits]
  (10000 when unknown) to [physical display black, displayPeakNits]. The EETF uses the libplacebo knee_offset 0.5
  (ks = 1.5 maxT - 0.5). A real roll-off (0 < display peak < source peak) is an approximation of the display's own:
  Approx, "light level: HDR passthrough (approx display roll-off)". Display peak >= source peak: identity, Exact, "light
  level: exact (HDR passthrough, no display roll-off needed)". Display peak unknown (<= 0): identity, Approx, "light level:
  approx (display peak unknown; no roll-off modelled)".
- **HdrToSdr**: exact port of `tools/eval/tonemap_ref.py` `SplineTM` (libplacebo 7.360 `pl_tone_map_spline` with the
  ST 2094 knee pick), constants from the ToneMappingTuning keys (auto = Specs() default), src peak = mastering peak
  (static knee), src min = `sourceMinNits`, dst = the ENCODING target [`sdr_black_nits`, `sdr_target_nits`], input
  clipped to [PQ(src min), PQ(src peak)]. libplacebo encodes the tone-mapped light L_out with the INVERSE of the target
  transfer including the target black, so the display reproduces L_out:
  signal = TargetDisplay.Inverse(clamp(L_out, Lb_t, Lw_t)); sdrCode = 64 + 876 signal (stored in `Entry::sdrCode`);
  L = PhysicalDisplay(signal). TargetDisplay uses `sdr_target_nits` / `sdr_black_nits` / `hdr_tone_map_target_gamma`;
  PhysicalDisplay uses the display keys. With the display on auto, L == L_out; with target black 0 and BT.1886 this is
  `tonemap_ref.pq_out_to_sdr_code` ((L_out / Lw)^(1/2.4)). `Display::Inverse` is the exact inverse of the display
  EOTF (BT.1886 with black, power with black, piecewise sRGB). sRGB is approximated by gamma 2.2 only in the SDR
  tone-shape power (matching the renderer, `EffectivePower`); Display / Inverse use the exact piecewise sRGB.
  Any peak detection that is on is Approx (the scene-average knee is not modelled): with a VP peak estimate
  (`dynamicPeakNits` > 0, used as the source peak) "light level: approx (dynamic tone mapping)", without one "light
  level: approx (dynamic tone mapping; static curve, no VP peak estimate)". `peak_detection: auto` keeps the quality
  preset's value: on for quality high / balanced, off for fast (LibplaceboRenderParameters.cpp:317-326, presets
  :45-50). `tone_mapping` other than auto/spline uses the spline and adds "tone_mapping X not modelled; spline used".
  An explicit `contrast_recovery` above 0 adds "contrast_recovery not modelled" (`auto` keeps the preset value, 0.30 for
  quality high, and is not labelled; no key for grey handling, intensity vs Y', was found in the renderer config).
  The status names every reason, joined with "; ".

## Units and formulas

Per code c in 0..1023: v = (c - 64) / 876 limited, c / 1023 full. J = PQ_OETF(L) in 0..1. S = dJ/dcode, central
difference over +-0.5 code of the same analytic map (one-sided at the ends). V = |dL/dcode| /
(weber (L + ambient)), ambient = light_level_ambient_nits only (L already contains the physical display black, so the black
is not counted twice); one V unit = one just-visible Weber step per code. Where L + ambient is 0 the denominator uses the
PQ minimum step (PQ EOTF of 1/1023, a format constant).

BTB: limited-range codes below 64 get L = display black, J = PQ_OETF(display black) (equal to J at code 64, so there is
no fake edge at the BTB boundary), S = V = 0. The LUT is a lookup: pixel data keep their below-black and above-white
values (never clamped); `Fetch` clamps only the index. WTW: SDR continues the curve (L capped at white), HDR
clips the PQ signal to 1.

## Settings

VP-owned (`Specs()`): `light_level_weber` (default 0.02, 0.001 to 0.2, provisional), `light_level_ambient_nits`
(default 0, 0 to 1000, provisional), `light_level_display_white_nits` (`auto` = `sdr_target_nits`, or 1 to 10000,
provisional) and `light_level_display_black_nits` (`auto` = `sdr_black_nits`, or 0 to 100, provisional); white must
exceed black, else a note and both fall back to auto. They describe the PHYSICAL display, separate from the ENCODING
target (`sdr_target_nits` / `sdr_black_nits`). Read (existing keys, renderer parse rules, cited in LightLevelSettings.h):
`sdr_target_nits`, `sdr_black_nits` (auto = 0), `hdr_tone_map_target_gamma`, `tone_mapping`, `peak_detection`, `quality` (selects the preset for `peak_detection: auto`), the
ToneMappingTuning keys, `sdr_tone_shape`, `sdr_white_headroom`, `sdr_knee_start`, `sdr_knee_strength`, and for exactness labelling only `sdr_input_transfer`, `output_gamma`, `contrast_recovery`. An invalid value
falls back to the default and adds a line to `Settings::notes`. No nit, rate or resolution is hard-coded: everything is
a setting or comes in through `Context`.

## Files

- `LightLevelSettings.h`: `Specs()`, `Settings`, `Read(get)`.
- `LightLevelLut.h`: `Context`, `Entry`, `Lut`, `Build`, `Fingerprint`, `Fetch`, `PackTexels`, `LutCache`
  (rebuilds only when the fingerprint changes).
- `LightLevelGlsl.h`: `LightLevelGlsl()`: `light_level_lut_fetch`, `light_level_j` (sampler `light_level_lut`, 1024 x 1
  rgba32f: J, S, V, L / 10000).

Deterministic (fixed loops, double math, no threads), C++14, no Windows headers.

## Block product P12 (WP2, unit version 2)

One rgba texel per region block (the P11 grid; 8160 blocks = 65 KB per frame at 1080p, 4K and 8K, plus the 16 KB LUT):
`(J_surround, J_local, V, conf)`. Declared in `core/Products.h` (P12, history 1: the previous slot is the one-pole state, no
private copy), catalogued in `core/ProductCatalog.h` (expected unit version 2), counted by the VRAM governor.

- **J_local**: mean J over the block's picture pixels, taken by P11's stats pass from this LUT (`StatsConstants::lutTexels`,
  `FetchPacked`; RegionEvidence unit version 2). It replaces the generic `Nits` / `PqOf` / `Visibility` chain; without a LUT the
  generic law is kept as the visible reduced path (`RegionEvidence::StatusOf`: "no light-level LUT").
- **J_surround**: Gaussian over J_local weighted by picture coverage, so bars (coverage 0) never enter it, whatever their level;
  sigma = 1 degree of visual angle from the picture height and `Params::viewingDistancePictureHeights` (display settings), else
  picture height / 40, in blocks (the same share of the picture at every resolution); window radius 3 sigma, capped and reported.
  Then a one-pole (tau 0.3 s at the 60 Hz reference, `GeneralRate::PerFrameWeight`) that resets on cut, resync, re-base,
  geometry (frame, block, picture rect) and context (LUT fingerprint) changes.
- **V**: the LUT visibility (Weber units) of one code step at the block mean code, capped at 64 (unclamped, unlike P11's 0..1
  `kVisibilityStat`). **conf**: 1 in the picture with a grain split, scaled by the frame gate (0 when P11 was made without this
  LUT or for another context).
- **Modes and status** (`LightLevelBlocks.h`, `Out::status`): Full; FrameLevel (no P11: one level from P4's APL, conf 0.5);
  CodeOnly (no P11, no APL: conf 0, consumers use the LUT per pixel); Neutral (no LUT/context: light term 1). Every reduction is
  named ("Light level: reduced (...)"), reset reasons are appended, the product recovers when its inputs return.
- **Consumer read**: `vpu_light_block(blk)`, `vpu_light_jeff(px, code10)` (GLSL, `LightLevelBlocksGlsl.h`) and
  `EffectiveAt<T>` (CPU mirror): `J_eff = J_p + (J_s - J_p) * (surroundMix * conf)`, J_s bilinear over the block centres; conf 0
  gives J_p. Consumers: dither-need map (V, Sout), deband (`k_light`), contrast recovery.
- **Fill**: lane after P11 (one pass), or the first consumer fills it live into the same slot with the same pass
  (`LightLevelBlocksPassGlsl`). Mirror: `BlockOut<float>`; reference: `BlockOut<double>`; python: `tools/eval/light_level_ref.py`
  `block_product`, pinned by `tests/LightLevelBlocksGolden.generated.h`.
- Open: the surround sigma is a guess until the WP9 eval; the viewing distance has no setting key yet (the host passes it).

## Status line

`Light level: approx (<why>) | neutral (<why>)` when wired; the LUT's own `status` carries the exact / approx text.
