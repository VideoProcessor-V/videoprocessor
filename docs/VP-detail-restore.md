# Detail pulse stabilisation and texture restore (design, 2026-10-17)

Successors of the rejected frame detail accumulation (docs/VP-detail-accumulation.md). Research
and gates: docs/audit/detail-retrieval-research-2026-10-17.md.

## Owner rules

- Faithful to the underlying image: detail "as though it was max bitrate RGB". No neural or
  learned methods. No invented structure.
- Film grain is never averaged or interpolated away. Grain and scene texture are split and
  treated separately. Each frame keeps its own grain (no grain is carried between frames).
- Texture restore repairs towards the original and never creates detail where the picture is
  out of focus: it is gated by local focus evidence.
- Source-resolution detection keeps reading the ORIGINAL frame; neither stage can change its
  decision.
- Motion-dependent parts use the de-judder's vectors when they are valid for the frame
  (MotionValidity) and fall back without them. Neither stage ever starts the matcher.
- No hard-coded rates or resolutions: distances scale with picture height
  (GrainPreserve::GrainScale, reference 2160 lines), time constants are in seconds and use the
  measured content rate (reference 23.976p when it is unknown).

## Placement

```
capture -> source-resolution monitor (reads renderImage: the original frame, untouched)
        -> [render hook] re-base (if engaged) -> VP debander -> detail_pulse_stabilise
        -> texture_restore -> libplacebo (scaling, sharpening, output)
```

Both stages run in the same render hook as the debander, after it, on the plane libplacebo
scales from. Deviation from the roadmap's "before re-base": the kernel-matched re-base is an
exact inverse of the player's upscale and needs the signal the player produced, so nothing may
change the picture before it; on a re-based source the stages then work at the content's own
resolution (cheaper, and the bands are the content's frequencies). Both are still before every
scaling step. A re-base factor change restarts both histories.

The monitor's input is taken before the hook runs (LibplaceboVideoRenderer: sourceMonitor.Frame
on renderImage), so the stages cannot reach it. A CPU test pins this (DetailRestoreConfigTests).

Neither runs on a de-judder extra (a picture between frames) or while smooth motion /
deinterlacing runs (as the debander). A cadence repeat (same source sequence) reuses the frame's
decision and never advances the history.

## detail_pulse_stabilise (research option e)

Starved encodes code I-frames finely and B-frames coarsely, so fine detail (and grain) pulses
with the GOP ("breathing"). The stabiliser scales each frame's OWN detail up towards the level
of its best-coded recent neighbour. Nothing is copied between frames, so grain stays per-frame.

1. Two luma detail bands at the plane's resolution: `D1 = Y - G(s1) Y`, `D2 = G(s1) Y - G(s2) Y`,
   s1 = 1.0 px, s2 = 2.5 px at 2160 lines and below (x GrainScale above). Separable Gaussians.
2. Band energies per tile, then a robust frame energy per band (tiles in black bars or below the
   coding-noise floor excluded; trimmed mean).
3. History: a small ring of per-frame band energies (and the frame's mean luma for the fade
   check), one entry per source frame, kept on the GPU. Reference per band
   `R = max_k E_k * decay^(age_k seconds)` over the frames since the last cut, within a window of
   ~0.5 s (the content rate converts it to frames). Optional look-ahead: the next source frame's
   energies when it is already uploaded and no re-base is engaged (same domain).
4. Frame gain per band `g = clamp(sqrt(R / E), 1, cap)` with a dead zone for estimator noise
   (a picture with steady detail gets exactly 1). An I-frame (the best) gets 1.
5. Per-tile veto: with valid vectors, tiles whose blocks are untrusted keep gain 1; without
   vectors, tiles whose low band changed beyond the coding-noise allowance since the previous
   frame keep gain 1 (new content, flashes). Gains are smoothed between tile centres.
6. Cuts and fades: a large frame-wide low-band change (cut) or a steady mean-luma ramp (fade)
   restarts the history (gain 1 on that frame). MotionValidity invalidations restart it too.
7. Output `Y' = Y + (g1 - 1) D1 + (g2 - 1) D2`; chroma passes untouched.

Everything stays on the GPU (no readback); the gain is deterministic for a given input
sequence. Cost target: 4K <= 1.0 ms on the 3050 Ti, about 0.3 ms on the 3090.

## texture_restore: off | statistical (experimental)

Per tile (16 plane px x GrainScale) the detail is split into:

- **grain**: the spatially uncorrelated, unstructured part of the fine band (the residual where
  the local structure tensor is incoherent; level from a robust noise estimate of the tile);
- **scene texture**: the structured part of both bands (coherent orientation, energy above the
  grain level).

A per-tile reference (grain level, texture energy, focus evidence, low-band mean) is kept from
the best-coded recent frame: a peak-hold with a decay in seconds, carried along the vectors when
they are valid; without vectors only tiles whose low band did not change keep their reference
(others restart from the current frame). Cuts, fades and invalidations restart it.

- Grain: the current frame's own grain component is scaled towards the reference grain level
  (`clamp(sqrt(Gref / G), 1, cap)`). Grain sits on the film, not the scene, so this applies in
  defocused areas too, but it only scales grain this frame already has: nothing is synthesised,
  and a frame with no grain left gets none.
- Scene texture: scaled towards the reference texture energy (`clamp(sqrt(Tref / T), 1, cap)`)
  only where the focus gate passes: the current tile shows in-focus evidence (sharp edge
  acutance / fine-to-mid band ratio above the defocus level) and the reference tile had real
  texture. A defocused tile (or one whose focus was pulled since the reference) gets texture
  gain 1: no added energy.

Output `Y' = Y + (gG - 1) grain + (gT - 1) texture`; chroma untouched. Cost target: 4K
<= 2.5 ms on the 3050 Ti, about 0.6 ms on the 3090.

## Settings (Config: "Experimental content-adaptive settings" group)

| Key | Values | Default |
|---|---|---|
| `detail_pulse_stabilise` | off, on | on (3090 cost small; see the agent report) |
| `detail_pulse_cap` | 1.00-1.50 | 1.35 |
| `texture_restore` | off, statistical | off (experimental) |
| `texture_restore_cap` | 1.00-2.00 | 1.50 |

Off is byte-identical to the frame without the stage.

## Tests

CPU (models in src/VideoProcessor-Test/DetailPulseModel.h, TextureRestoreModel.h, mirroring the
shaders): grain RMS within +-5 %; no added energy in defocused regions; flicker reduced on a
static compressed sequence without lowering detail; I-frame gain 1.00 +- 0.01; no-vector
fallback; cuts/fades restart; source-resolution input unchanged. Real clips (D:\hfr\breathing,
env VP_TEST_REAL_FOOTAGE). GPU (WARP or hardware, for Claude): shader against model, timings.
