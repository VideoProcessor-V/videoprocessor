# VP-V unit: output

The projector calibration LUT, dither (sigma-delta) and presentation.

One folder per slot below. Each slot's README.md is its module doc, and its MANIFEST.json holds
its components and its live file list. The order and zones come from docs/design/slots.json;
see src/VP-V/core/README.md for how the skeleton runs them.

| Order | Slot | Status | Zone | Purpose |
|---|---|---|---|---|
| 250 | [calibration_lut](calibration_lut/README.md) | partial | output_hooks | The LAST colour op: SDR target -> projector codes, applied BY VP at the end of its PRE_OUTPUT hooks, after LED compensation and black level. |
| 260 | [dither](dither/README.md) | built | quantise | Quantise last for the 8-bit FL35 link: temporal sigma-delta (with vectors) or libplacebo dither; 12-bit / DV output paths later. |
| 270 | [presentation](presentation/README.md) | built | present | Swapchain, refresh, render-ahead (inside the delay budget), subtitle placement, transient confirmations (no always-on OSD). |
