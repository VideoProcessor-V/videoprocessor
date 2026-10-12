# Delta 5 — config-adaptable audit: findings (step 1 of docs/VP-delta-5-config-adaptable.md)

2026-10-12. Read-only pass over everything outside the LED module that the
FL35 work added: hook chain, edge blanking, sigma-delta, upscaler `//!WHEN`,
de-judder, occlusion mask, NLS mask mapping, queue, output encoding repair.
Stage 10 (carry/restore) is not built yet and will be built to the rule.

## To change

| # | Where | Assumption | Change |
|---|---|---|---|
| D5-1 | `Temporal sigma-delta.glsl` parameters `levels_full`, `swapchain_bits` | Typed by hand in the shader rule (the cinema config has `levels_full: 0`, `swapchain_bits: 10`). If the profile's `output_range` or the swapchain changes, the quantizer works in the wrong code domain. | The renderer sets both every frame from the frame's target representation (levels, sample depth), as it already does for `occlusion_mask`. A rule value is then ignored (log once if it disagrees). |
| D5-2 | `Temporal sigma-delta.glsl` quantizer (`k = clamp(..., 0, 255)`, `out_step` 4/1023 or 1/255) | Always quantizes to **8-bit** codes. Right for an 8-bit-precision output (the FL35: `display_bit_depth: 8` on a 10-bit swapchain), but with `display_bit_depth: 10` (colour depth 10) it throws away the extra precision. | Quantize to the target's colour depth (new parameter `color_bits`, set by the renderer with D5-1): code grid 2^depth − 1, limited/full scale from the level. The tolerances (`noise_amplitude`, `motion_threshold`, `match_tolerance`) stay in 8-bit-code units as perceptual units and are scaled internally. |
| D5-3 | `DejudderWarp.cpp` downsample (`sum * 0.25 * 255.0`) | Luma "in 8-bit codes" straight from the source's normalised values, which assumes limited-range sources; on a full-range source the match tolerance and cut threshold mean about 14% more. | Scale the luma to code units by the source's level range (219 or 255 code span), so the tolerances mean the same for both. |
| D5-4 | GPU tests | Several stage tests run only one range / bit depth. | Second runs (delta 5 step 2): sigma-delta exact codes on full range and on true 10-bit; occlusion mask and de-judder render tests with `output_range: full`; LED tests are excluded (LED module). The same checks on the Beta libplacebo (377) where cheap. |

## Cleared (no change)

- **Hook chain templates:** sizes come from `{{output_width}}`, `{{output_height}}`, refresh from `{{refresh_hz}}` / cadence; nothing assumes 2560×1600 or 60 Hz.
- **Upscalers' `//!WHEN`:** ratios of output to source height (`OUTPUT.h > LUMA.h / 1.3`), no fixed resolution.
- **Edge blanking:** shrinks libplacebo's crop; the blanked strips use libplacebo's own clear, encoded for the target's range.
- **Occlusion mask:** alpha 0/1 and block geometry only; range-independent. The NLS mapping reads the profile's NLS settings.
- **De-judder cadence:** reads the real refresh rate and source frame rate.
- **OutputEncodingRepair:** reads the target representation each frame (and is off on libplacebo 377).
- **Queue:** time-based (`target_latency_ms`), no fixed rate.
- **LED module:** excluded by the rule (the owner's projector numbers live there by design). The LED's output-gamma helper (`LedDisplayGamma`) does read the frame's transfer.
- **Not ours:** rc4's subtitle measurement falls back to 60 fps only when the display mode is unknown.

## Proposed order

D5-1 and D5-2 together (one shader and renderer change, with the GPU tests),
then D5-3, then the D5-4 test runs, then Stage 10 to the same rule.
