# Design review 4 — Stage 6a package (Dejudder::MotionWarp, standalone)

2026-10-10. Scope: design intent. One unit, not yet connected to rendering.
No blockers, no should-fixes. Notes, verified-OK, and one scope addition for
6c carried in from the planner (see the end).

---

## Notes

- N-1 The three-level pyramid with two carried candidates is a justified
  deviation from "8×8 at half res, refined to half a pixel": the block size
  and the refinement are as asked, and carrying two candidates fixed a
  failure the tests exposed (one wrong coarse guess followed all the way
  down). Keep it.
- N-2 Bilinear vector interpolation between *trusted* block centres, with
  the pixel's own block deciding whether to warp at all, is the right split:
  no block seams inside warped regions, and a hard edge only where a hold
  begins — which is what "hard hold, never a blend" requires. Expect that
  edge to be where any artefact shows on the projector; that is the
  intended failure mode.
- N-3 Chroma vector divided by the actual subsampling (both axes for
  4:2:0) and edge clamping to A's edge pixel: both as asked.
- N-4 Cut handling: the caller's flag *and* the natural collapse of trusted
  blocks (≤2%) on an unrelated B. Belt and braces; good.
- N-5 GPU time: the read count (~25k reads per block per direction) on a
  4K source works out to a few milliseconds per source frame on the 3090,
  not per refresh — inside the budget, but it is the one number the WARP
  tests cannot give. First look on the stats line in 6b; the levers are the
  1/32 search range and the number of fine candidates.
- N-6 The "libplacebo logs a double-binding error but returns success"
  finding is important beyond this step: the Code auditor should check
  that every GPU test asserts on the libplacebo log being clean, not only
  on return values — for this component and for the hook chain.
- N-7 `matchTolerance: 3` codes here vs 1.5 in the dither: sensible, since
  this matcher must tolerate grain to keep tracking a pan, and a wrong
  vector here is hard-held rather than carried. Both become config keys in
  6b; keep them separate.

## Verified OK (design intent)

- Standalone component; not wired into rendering; no behaviour change.
- Bidirectional estimation A→B and B→A; trust = good match AND consistency
  within 1 half-res pixel; untrusted = occlusion or bad match.
- Warp by `s·δ·v` producing planes in A's own formats for the normal
  render; hard hold on untrusted blocks; never a blend; `s` applied by the
  caller globally.
- Half-pixel refinement at the fine level; one binding per texture with
  linear filtering where sub-pixel reads are needed.
- WARP tests: moving texture lands where expected; occlusion holds; cut
  holds; synthetic detail at 4/16/64 px.

## For the Code auditor

- Libplacebo log clean in every GPU test (N-6).
- Warp output planes have A's exact format, stride and range, so 6b can
  hand them to `pl_render_image` unchanged.
- No path in this component allocates or dispatches unless `Estimate` or
  `Warp` is called (so "off" in 6b costs nothing).

## Scope addition for 6c (from the planner; not part of this package)

**Mask-only mode is required in 6c.** The dither's occlusion refinement must
not depend on the de-judder being on. Add `occlusion_mask: on|off` beside
`dejudder_strength`: with strength 0 and the mask on, the component runs
`Estimate` once per source frame and publishes the occlusion mask (via the
M1 alpha path if the harness proves it) but makes no extra presents and no
warp — the presentation loop is untouched. Cost: the matching plus B's
upload. Cinema profile only. If M3 is the outcome, the key still exists and
is a documented no-op. Nothing changes for 6a or 6b. (Also recorded in
`docs/VP-stage6-design-answers.md` under Q2.)
