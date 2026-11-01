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

- **Sdr**: optional `SdrToneShape::Map` (when `sdr_tone_shape`/`sdr_white_headroom` are active), then the display EOTF
  with white `sdr_target_nits` and black `sdr_black_nits`. BT.1886: g = 2.4, a = (Lw^(1/g) - Lb^(1/g))^g,
  b = Lb^(1/g) / (Lw^(1/g) - Lb^(1/g)), L = a max(v + b, 0)^g. Numeric power: L = Lb + (Lw - Lb) v^g. sRGB: piecewise,
  likewise. Above white the curve continues and L is capped at Lw. Exact.
- **HdrPassthrough**: L = PQ EOTF(v), then a BT.2390 EETF roll-off in PQ space from [sourceMinNits, sourcePeakNits]
  (10000 when unknown) to [sdr_black_nits, displayPeakNits]. Approximation of the display's own roll-off: Approx.
- **HdrToSdr**: exact port of `tools/eval/tonemap_ref.py` `SplineTM` (libplacebo 7.360 `pl_tone_map_spline` with the
  ST 2094 knee pick), constants from the ToneMappingTuning keys (auto = Specs() default), src peak = mastering peak
  (static knee), src min = `sourceMinNits`, dst = [`sdr_black_nits`, `sdr_target_nits`], input clipped to
  [PQ(src min), PQ(src peak)]. SDR signal = 64 + 876 (L_out / Lw)^(1/g), g from `hdr_tone_map_target_gamma`
  (bt1886 2.4, srgb approximated by 2.2, numeric power); stored in `Entry::sdrCode`; L = display EOTF of that signal.
  Peak detection on (a measured `dynamicPeakNits`) uses it as the source peak: Approx ("dynamic tone mapping"; the scene
  average knee is not modelled). `tone_mapping` other than auto/spline uses the spline: Approx, status names the curve.

## Units and formulas

Per code c in 0..1023: v = (c - 64) / 876 limited, c / 1023 full. J = PQ_OETF(L) in 0..1. S = dJ/dcode, central
difference over +-0.5 code of the same analytic map (one-sided at the ends). V = |dL/dcode| /
(weber (L + L_floor)), L_floor = sdr_black_nits + light_level_ambient_nits; one V unit = one just-visible Weber step per
code. Where L + L_floor is 0 the denominator uses the PQ minimum step (PQ EOTF of 1/1023, a format constant).

BTB: limited-range codes below 64 get J = S = V = 0 and L = display black. The LUT is a lookup: pixel data keep their
below-black and above-white values; `Fetch` clamps only the index. WTW: SDR continues the curve (L capped at white), HDR
clips the PQ signal to 1.

## Settings

VP-owned (`Specs()`): `light_level_weber` (default 0.02, 0.001 to 0.2, provisional) and `light_level_ambient_nits`
(default 0, 0 to 1000, provisional). Read (existing keys, renderer parse rules, cited in LightLevelSettings.h):
`sdr_target_nits`, `sdr_black_nits` (auto = 0), `hdr_tone_map_target_gamma`, `tone_mapping`, `peak_detection`, the
ToneMappingTuning keys, `sdr_tone_shape`, `sdr_white_headroom`, `sdr_knee_start`, `sdr_knee_strength`. An invalid value
falls back to the default and adds a line to `Settings::notes`. No nit, rate or resolution is hard-coded: everything is
a setting or comes in through `Context`.

## Files

- `LightLevelSettings.h`: `Specs()`, `Settings`, `Read(get)`.
- `LightLevelLut.h`: `Context`, `Entry`, `Lut`, `Build`, `Fingerprint`, `Fetch`, `PackTexels`, `LutCache`
  (rebuilds only when the fingerprint changes).
- `LightLevelGlsl.h`: `LightLevelGlsl()`: `light_level_lut_fetch`, `light_level_j` (sampler `light_level_lut`, 1024 x 1
  rgba32f: J, S, V, L / 10000).

Deterministic (fixed loops, double math, no threads), C++14, no Windows headers.

## Status line

`Light level: approx (<why>) | neutral (<why>)` when wired; the LUT's own `status` carries the exact / approx text.
