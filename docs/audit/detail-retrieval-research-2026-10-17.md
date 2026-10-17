# Low-bitrate detail retrieval: research (2026-10-17, CPU prototypes, read-only)

Owner goal: a faithful image, detail recovered as if from max-bitrate RGB, grain kept. Neural or
learned restoration is rejected by the owner. Prototype scripts and logs are in the session
scratchpad (detail/), not in the repo.

## Findings

- **What a starved encode discards is in no frame.**
  - An oracle (the best linear mix of 9 aligned frames, fitted per tile with the original in hand)
    gains only +0.36 to +0.58 dB on B-frames.
  - Practical fusion gains +0.06 to +0.11 dB.
  - No faithful post-process meets the +1 dB gate on this material.
- **Why accumulation failed.** x265 B-frames copy their references' already-reduced texture, so the
  missing detail is the same in every frame. The coding error is correlated along the motion (0.22
  at CRF 24, 0.35 at CRF 32), so averaging cannot cancel it.
- **On grainy film the breathing is mostly grain.**
  - CRF 24 keeps 48% of the grain on I-frames and 13% on B-frames.
  - CRF 32 keeps 15% and 0%.
  - Grain is a new random pattern in every frame, so it cannot be carried from I-frames into
    B-frames.
- **The "faithful" estimate is softer, not sharper.** Both oracles gain PSNR by lowering
  high-frequency energy, because the decoded fine detail is part texture and part coding noise.
- **(a) Deblock/dering:** nothing to remove. x265's in-loop filter leaves no block grid (blockiness
  0.98-1.02). Its DCT shrink also cuts grain.
- **(b) Quantisation-constraint (POCS) projection:** not viable without the bitstream. Lattice
  scores are 0.10-0.21 on the decodes, against 1.0 for a JPEG-like control.
- **(c)/(d) Fusion and look-ahead toward better-coded frames:** fails the grain rule. It cuts grain
  and copies it between frames (tcorr 0.25 to 0.27).
- **(e) Detail-energy equalisation:** works for steadiness. Each frame's own detail is scaled up to
  its best-coded neighbour's level.
  - Breathing halves: 0.112 to 0.046 on grainy CRF 24, 0.031 to 0.013 on clean CRF 24.
  - Grain is kept and stays per-frame.
  - Cost: 0.03-0.12 dB of PSNR. The picture is steadier, not closer to the original.
- **Grain the encoder deleted** could only return by synthesis (AV1-style). That invents pixel
  values, so it is an owner decision, not a recommendation.

## What Stage 12 does today

- **Re-base:** returns a player-upscaled stream to its real resolution. The kernel-matched inverse
  recovers +9 to +12 dB, and this is the real detail retrieval.
- **Readings:** bitrate_quality and effective_bit_depth.
- **VP debander:** removes banding and keeps 98% of texture.
- **Residual stabilisation:** stops the sharpener's own flicker.

## Recommendation

**Build (e) as `detail_pulse_stabilise`, default off.**

- **Method:**
  - Two luma detail bands.
  - Gain = sqrt(best / own) from trusted neighbours in the look-ahead, clamped to [1, slider cap
    1.35].
  - Frame-level gain, vetoed per tile where the vectors disagree.
  - Off at cuts, fades and invalid vectors.
  - Runs before the re-base and never starts the matcher.
- **Gates before default:**
  - breathing at least halved;
  - mean PSNR loss ≤ 0.15 dB, worst frame ≤ 0.35 dB;
  - grain and tcorr no worse than the input;
  - I-frame gain 1.00 ± 0.01;
  - owner viewing on the 3090;
  - re-tested on G19 real captures.
- **Cost (estimated):**

  | Variant | 3050 Ti, 4K | 3090 |
  |---|---|---|
  | Frame-level | 0.6-1.0 ms | 0.2-0.3 ms |
  | Per-tile | 1.5-2.5 ms | about 0.5 ms |

**Do not build (a), (b), (c)/(d) or neural methods.** Keep accumulation shelved. Better effort:
extend the kernel-matched re-base (1440p, chroma) and run the G19 measurements.

**Check:** confirm on a grain clip that the residual stabilisation and the debander's vector hold
leave grain un-averaged.
