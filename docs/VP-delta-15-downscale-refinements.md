# Delta 15 — nearly-free downscale refinements (added to the deploy)

2026-10-14. Companion to Stage 8 and Stage 12. Each a setting; off =
today.

## S8-1 — Temporal stabilisation of the SSIM residual (`ssim_residual_stabilise: on`)

SSIM preserves local variance by re-estimating it every frame, so on
grain and fine texture its added contrast (SSIM output minus the
faithful branch) shimmers frame to frame. Same remedy as the CNN
residual in Stage 12 §2c: where the de-judder's vectors are trusted,
average the SSIM residual with the previous frame's motion-compensated
residual (one-pole, ≈3 frames); the faithful part is untouched;
untrusted blocks use the current residual. The grain term still sets
*how much* SSIM; this sets how *steady* it is. Cost: one block-mapped
texture read in the blend pass.

## S8-2 — Bitrate-aware SSIM weight (`ssim_bitrate_aware: on`)

At low `bitrate_quality` (Stage 12 §1b), SSIM sharpens blocking and
mosquito noise as if they were texture. Multiply `w_base` by a soft
ramp on `bitrate_quality` (1 at clean, `ssim_bitrate_floor` default 0.4
at poor), beyond what the grain term does, and raise the faithful
kernel's anti-ringing at the same time (as for the CNN in Stage 12 §2c).

## S8-3 — Re-base takes precedence over the downscale

When Stage 12 classifies a 4K source as effectively 1080p or lower, the
picture is re-based to that size and goes through the *upscale* path
(8b); Stage 8's downscale blend does not run on it. A 4K source
classified as 1440 is re-based to 1440 and then downscaled to 1600
vertically only where the anamorphic fit requires — Stage 8 applies
there as to any source above the output. State the precedence in both
Config cards so the owner knows which stage acted; the stats line names
it.

## S8-4 — Edge term after re-base, not before

The edge term reads the source. On an upstream-upscaled source the
upscaler's ringing reads as edges and would wrongly push the blend to
the faithful branch. Computing the edge term on the re-based picture
(S8-3) removes the ringing first; no change for true 4K.

## Tests

- S8-1: a flickering synthetic SSIM residual on a trusted pan is
  averaged; static texture unchanged; untrusted blocks pass the current
  residual; both warp paths.
- S8-2: a blocked synthetic frame lowers `w` by the ramp; a clean one
  does not.
- S8-3/4: a bicubically upscaled 1080p chart at 4K goes through re-base
  + 8b and Stage 8 does not run; a true 4K chart goes through Stage 8;
  the edge map on the re-based chart has no ringing edges.

## Order

With Stage 12 (same step), before the final gate.
