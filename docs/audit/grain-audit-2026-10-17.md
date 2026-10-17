# Film-grain audit (2026-10-17, read-only, CPU simulations)

Owner rule: grain is never averaged away. The goal is a faithful image with detail as if it were
max-bitrate RGB.

Simulations use fresh N(0,1) grain on every frame, so they isolate the grain path. Real grain is
softer than this white grain, so real losses are somewhat smaller.

## Defaults

Nothing VP adds by default averages grain across frames:

- **Off by default:** interpolation, de-judder (strength 0), adaptive down/upscale (and with them
  the residual stabilisation), and VP's debander.
- **Sigma-delta:** carries at most 0.5 code.
- **Source-resolution detection:** grain raises the spectral floor, so the re-base refuses grainy
  sources and they stay native.

**One default exception: libplacebo deband on SDR (high preset).** At 0.5 code of grain it keeps
0.86x; at 1 code or more, about 1.0x. It also adds about 0.29 code RMS of unlabelled random noise on
every frame. HDR sources have it off.

## At risk when features are enabled

| Stage | Grain effect | Evidence |
|---|---|---|
| FrameInterpolator: Stage A, gap fill, per-vsync | Blends warped A and B by phase (0.71x at t=0.5) and samples bilinearly. Synthesised frames carry 0.45-0.49x of the source's grain, so at 50p on 60 Hz grain pulses at 10 Hz. Lag-1 correlation up to 0.55. | FrameInterpolator.cpp:39-51, 95-108 |
| Interpolator fallback `blend` | Unwarped A/B mix, 0.71x | FrameInterpolator.cpp:53-54 |
| Occlusion forcing wb=0 | Grain strength becomes patchy across the picture | FrameInterpolator.cpp:44-50 |
| De-judder, rendered warp | One frame only (A), Catmull-Rom: 0.96-0.98x (safe) | RenderedWarp.cpp:67-89 |
| De-judder, source warp | Hardware bilinear: 0.62x at a 0.25 px shift, 0.50x at 0.5 px | DejudderWarp.cpp:2192-2203 |
| CNN/SSIM residual stabilisation | One-pole filter, alpha 1/3: total grain 0.83-0.90x | AdaptiveDownscale.cpp:216-234 |
| VpDebander | At grain of 0.5 / 1 code: 0.59x / 0.89x at the 1.5-code threshold; 0.50x / 0.59x at the low-bitrate maximum. The grain veto works only with vectors. | VpDebander.cpp:17-66, 108-121 |
| Re-base | 0.89x on a correctly detected upscale (only the invented band is lost); 0.41x if a true 4K grainy source is misclassified or forced | SourceRebase / detector |

## Fixes, ranked

Each fix sits behind a switch whose default is the faithful behaviour. Costs are estimates for a
3050 Ti at 4K.

1. **`interpolation_grain: preserve | blend`**
   - Blend only the low band of warped A/B by phase.
   - Take the high band from the nearer frame, sampled with Catmull-Rom.
   - Match its energy per 16x16 block to that source frame's high band (gain clamped 1-2).
   - Result: grain 0.94-0.96x, temporal correlation like a hold. Cost about 1-1.5 ms.
2. **Variance-preserving blend:** cheaper (about 0.4 ms) but reaches only 0.53-0.64x. Second choice.
3. **`deband_grain: off`:** libplacebo deband grain set to 0; VP's dither does that job. Cost 0 ms.
4. **Residual stabilisation limited to the coding-artefact band:** grain blocks pass straight
   through, and the held change is clamped to the coding-noise RMS from the bitrate reading. Use
   texel fetches. Cost about 0.1 ms.
5. **VpDebander grain guard without vectors:** a spatial grain estimate vetoes blurring. Cost about
   0.2 ms.
6. **De-judder source warp:** Catmull-Rom instead of bilinear. Cost about 0.3 ms.
7. **Re-base guard:** refuse a class when the band above its Nyquist holds temporally independent
   energy.
8. **Detail accumulation ~~(when built)~~ (rejected by the owner 2026-10-17; applies to any successor):** exclude the temporally independent band from history.

## Tests

**CPU reference models:**
- Input: a panning texture at fractional motion with fresh grain on every frame, sigma 0.5, 1, 2
  and 4 codes, both white and soft.
- Pass if grain RMS is within ±5% of the source at every output frame, and lag-1 correlation is no
  higher than the source's own hold/repeat at the same cadence.

**GPU tests (gated):**
- InterpolatorKeepsGrainRmsAtEveryPhase
- InterpolatorGrainNotSmearedAcrossFrames
- DejudderRenderedWarpKeepsGrainRms
- SsimResidualStabiliseLeavesGrainBlocks / CnnResidualStabiliseLeavesGrainBlocks
- VpDebanderKeepsGrainWithoutVectors
- LibplaceboDebandAddsNoGrainWhenOff
- RebaseAutoRefusesGrainAboveClass

Each also checks that the legacy switch reproduces today's output byte for byte.
