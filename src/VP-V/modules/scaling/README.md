# VP-V unit: scaling

Re-base, the 4K route and refine, the chroma upscale slot, geometry and the output scaler.

One folder per slot below. Each slot's README.md is its module doc, and its MANIFEST.json holds
its components and its live file list. The order and zones come from docs/design/slots.json;
see src/VP-V/core/README.md for how the skeleton runs them.

| Order | Slot | Status | Zone | Purpose |
|---|---|---|---|---|
| 60 | [rebase](rebase/README.md) | built | front_end | Undo the player's upscale per axis: a 1-D kernel-matched inverse (or Lanczos) H then V back to the ENCODED grid, so later stages work on the real picture, not on scaler artifacts. |
| 100 | [route_4k](route_4k/README.md) | placeholder | front_end | Bring the picture to the PROCESSING resolution = min(source, max(4K, effective output active size)) with fixed 1-D x2 shaders per needed axis (H then V); 8K placeholder rules. |
| 110 | [refine](refine/README.md) | placeholder | front_end | ONE refine net at the end of the 4K route, trained to the truth: inputs = picture + continuous per-axis scale (sx, sy) + A-ran flag. |
| 120 | [chroma_upscale](chroma_upscale/README.md) | placeholder | front_end | Reserve a VP-owned chroma upscale right after the input stages, in case libplacebo's chroma upscaling (chroma_upscaler auto / CfL) proves a compromise. |
| 150 | [geometry](geometry/README.md) | built | libplacebo | Crop/active picture, A-lens (anamorphic), NLS, zoom/viewport. |
| 160 | [output](output/README.md) | partial | libplacebo | Scale to the output active size inside libplacebo: hybrid SSIM downscale (adaptive_w_base 0. |
