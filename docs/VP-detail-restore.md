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

Neither runs while smooth motion / deinterlacing runs (as the debander). A cadence repeat (same
source sequence) reuses the frame's decision and never advances the history. A picture between
frames (a Stage B phase picture, a hiccup gap fill, a de-judder extra) is a repeat of its source
frame: the frame's decision on THAT picture's own bands (hunt 3 F6/F7: A's stored low band on a
moved picture was a ghost of A; texture restore's repeat keeps its low bands in a set of its own,
so the next frame still compares against A). The history advances on A as drawn: re-based and
debanded (hunt 3 F5), the same domain as every other frame; the look-ahead measures the next
frame debanded too (per picture, the debander's look-ahead slot; hunt 3 F4).

During a source-resolution crossfade (source_resolution_latch_fixes) the path being left carries
the same stages: VP deband per picture, the detail pulse's and texture restore's decision of the
frame on its own bands at its own geometry (ApplyForeign: tile weights / gains read at the same
picture position; never advancing). The two paths are mixed in the output signal domain by VP's
PRE_OUTPUT hooks (CrossfadeMix.h, hunt 3 F1: BTB/WTW carried exactly, the extended-range late
restore once per path), on A's present and on every source-warp extra (F3); libplacebo's frame
mixer only as the fallback.

One-plane RGB pictures (an RGB capture; the subtitle move's composed picture during a cue) are
stabilised by the detail pulse on their luma, in the source's codes, with the same delta added to
R, G and B (= the YCbCr pulse exactly): the history runs on through subtitle cues (hunt 3 F8/F9).
Texture restore stays luma-plane only; a picture a stage cannot read is counted and the status
says "inactive (reason)" instead of "on".

## detail_pulse_stabilise (research option e) - as built

Starved encodes code I-frames finely and B-frames coarsely, so fine detail (and grain) pulses
with the GOP ("breathing"). The stabiliser scales each frame's OWN detail up towards the level
of its best-coded recent neighbour. Nothing is copied between frames, so grain stays per-frame.
The exact maths and constants are in DetailPulse.h (shared with the CPU model).

1. Two luma bands: `D1 = Y - G(1.0) Y`, `D2 = G(1.0) Y - L`, L = Gaussian 2.5 px held at half
   resolution; sigmas x GrainScale above 2160 lines.
2. Per 32 px tile: band energies, low-band mean and spread. Frame energy per band = winsorised
   mean over tiles that are above the noise floor, not vetoed and not jumping (new content).
3. History: a GPU ring of per-frame energies. `R = max(E_j x 0.9^(age s))` over a 0.17 s window
   (frames from the measured content rate; 0.5 s lifted a CRF 32 I-frame by 1.04 because a
   P-frame was coded better). Look-ahead: the next frame's energies (MeasureAhead) when it is
   uploaded, no re-base is engaged and it is comparable. Causal alone cuts breathing 43-46 %
   on the real clip; with the look-ahead 58-60 %.
4. Gain per band `g = sqrt(R / E / 1.01)` outside a 1 % dead zone, clamped to [1, cap]. The
   best frame gets exactly 1.
5. Per-tile veto (bilinear between tiles): with valid vectors, the untrusted share of the tile;
   without, the change of the tile's low-band mean/spread beyond 4x the coding-noise RMS
   (motion-tolerant: a pan keeps the gain, new content or a flash does not).
6. Restarts (gain 1): cut (over half the tiles changed), fade (mean luma ramp on two frames),
   Forget, sequence gap, geometry change. A repeat reuses the decision (on its own bands).
7. `Y' = Y + w ((g1 - 1) D1 + (g2 - 1) D2)`; chroma untouched. Passes: measure (per tile,
   fused), reduce (one workgroup), apply (fused). Estimated 4K: 0.55-0.7 ms on the 3050 Ti,
   0.15-0.2 ms on the 3090.

## texture_restore: off | statistical (experimental) - as built

Constants in TextureRestore.h (shared with the CPU model).

- Bands: D1 = Y - G(1) (fine), D2 = G(1) - L2 (mid), D3 = L2 - L3 (low-mid); L2, L3 = Gaussians
  of 2.5 and 6 px at half resolution; x GrainScale.
- Grain level per 16 px tile: the mean of its 4 quietest of 16 sub-blocks, then the minimum over
  the 5 x 5 neighbouring tiles (grain is everywhere on the film; texture is local). Per pixel the
  fine band is split by its local 3x3 energy: the part above 2x the grain level is texture, the
  rest grain.
- Focus evidence F = T2 / (T2 + E3) (the mid band against the low-mid band). Compression removes
  D1 but keeps D2/D3 (x265 clips: D2 0.83-0.94, D3 0.95-0.99 of the original), so F barely moves
  with bitrate; optical defocus widens edges and lowers it (0.28 at 1 px blur, 0.12 at 3 px).
  Texture gain needs current and reference in focus, F and mid-band texture not fallen more than
  coding explains (a focus pull blocks it), and texture above grain + coding noise in both.
- References per tile (grain level, T1, T2, F): peak-hold, energy halving in 2 s at the measured
  rate, carried bilinearly along trusted vectors; untrusted blocks restart; without vectors only
  tiles whose low band did not change keep their reference. Cuts, fades, Forget, gaps restart.
- Gains in [1, cap]: grain `sqrt(Nref / (N x 1.35))` on the grain part (only grain the frame
  has: no synthesis), texture `sqrt(Tref / T)` x focus gate on the texture part.
  `Y' = Y + (gG - 1)(1 - f) D1 + (gT1 - 1) f D1 + (gT2 - 1) f D2`.
- Passes: half box, two half-res Gaussians, per-tile stats, tile-grid grain/frame/reference,
  fused apply. Estimated 4K: 1.2-1.4 ms on the 3050 Ti, 0.3-0.4 ms on the 3090.
- On the real clips it is safe and nearly inert (the camera pans: without vectors nothing is
  restored; with a stand-in field B-frame detail 0.776 -> 0.780 of the original). Its effect is
  on static or vector-tracked coded texture.

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
