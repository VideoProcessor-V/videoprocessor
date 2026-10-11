# Audit — Stage 6a: de-judder motion matcher and warp (standalone)

Diff audited: a1805261..HEAD on stage7-rc2 (9aad1fd7 code; c4504ee9 docs, for
information; 5e685c6f tests). The other Claude session was idle throughout.
Build: OK — full x64 Release /t:Rebuild at 5e685c6f, 0 errors, no new warnings.
Tests: native 2468 passed, 2 failed (ShellStylePlacementWithoutSizingHintsIsNotReverted,
SameSizeEditWithRestoredWriteTimeInvalidatesCache — both known); the three Dejudder GPU tests
pass. VideoProcessorConfigTests 97 passed, 2 failed (the two known RPC tests).

Spec used: brief Stage 6 ("Design settled", 6a status),
VP-stage6-dejudder-design.md, VP-stage6-dejudder-answers.md (Q3, points 3–4),
design-review-4.md; libplacebo 7.360.1 source (dispatch.c, gpu.c, shaders.c).

## Blockers
- None.

## Should fix
- Test DejudderRefinesToHalfAPixel cannot detect a missing half-pixel
  refinement. The true motion (7, 3) is (3.5, 1.5) at half resolution. A
  matcher that stops at whole half-resolution pixels returns 6 or 8 by 2 or
  4, and the test's tolerance (±1 source pixel) accepts those. The (12, 6)
  test uses whole half-resolution pixels. So the refinement Q3 asks for is
  implemented (code checked) but not proven. Tighten to ±0.5 source px (or
  assert the vector is exactly 7.0/3.0 within float error) on (7, 3).
- (i)1 is only partly done. The de-judder tests and the sigma-delta runs fail
  on any libplacebo warning or error, but the other GPU tests do not:
  - AllBundledStandardShadersLoadAndUpscalersEngageByHeight,
    BundledCustomHooksParseAndSigmaDeltaRunsOnTheRealGpuPath and
    LedMeasurementHookMeasuresWithoutTouchingThePicture capture the log only
    around parsing, and only print it in failure messages;
  - LedRawTransferMatchesTheRenderedOutput,
    LedCompensationShaderAppliesGainWithASoftKnee and the NLS/LUT GPU tests
    do not capture it at all.

  Design review 4 N-6 asked for this "for the hook chain" too. Wrap each
  GPU test's whole render in CaptureLog and finish with
  AssertCleanLibplaceboLog.

## Notes
- (d) Silent-failure risk, confirmed in libplacebo's source.
  - gpu.c `pl_pass_run` checks its bindings with `require(...)`: a texture
    bound twice, a linear sample of a format without linear filtering, or a
    storage binding of a non-storable texture. On failure it only logs an
    error and skips the pass. It returns nothing, and dispatch.c
    `pl_dispatch_compute` still returns true.
  - (A pass that fails to *compile* does return false.)
  - The component avoids the known cases: one binding per texture, and it
    checks storability. But 6b will hand it the renderer's own upload planes,
    which must be sampleable with a linear-capable format; an unsuitable
    plane would make Warp "succeed" with stale output.
  - 6b should check the plane caps up front (`PL_FMT_CAP_LINEAR`,
    sampleable) or watch the libplacebo log at run time.
- Value scale: the matcher multiplies normalised luma by 255, so
  `matchTolerance` is in 8-bit codes only when the plane fills its container.
  The renderer's P010/P210 uploads keep 10-bit samples in the high bits, so
  this holds. 6b should still derive it from the frame's `repr.bits` rather
  than assume it.
- The component handles planar YCbCr only. RGB captures (also supported by
  the renderer) must be a hold, not passed in. That is 6b's job; note it in
  6b.
- Warp re-uses the chroma texture made at Estimate time. If A's chroma
  format changed between Estimate and Warp without the luma size changing
  (e.g. 4:2:2 → 4:2:0), Warp would write into the old layout. A format
  change always means a new Estimate in practice; a cheap guard would close
  it.
- The warp samples A once per output pixel with bilinear filtering (a
  sub-pixel position in A), never mixing A and B, so the no-blend rule holds.
  For the hard hold, "A's own pixel" is an exact copy (tested equal).
- Edges: libplacebo has no "border colour" address mode (only clamp, repeat,
  mirror), so with CLAMP set a black edge is impossible by construction. The
  test's left-edge check (> 4000) would also pass if those blocks were held,
  so it does not prove the clamp — the API does.
- (g) Cost, rough estimate for a 4K source (3840×2160 → 240×135 = 32.4k
  blocks):
  - 1/32 level: ~0.9k reads per block;
  - 1/8 level: two 13×13 half-step searches × 16 bilinear samples ≈ 10.8k;
  - half resolution: up to 123 whole-pixel vectors × 144 × 2 ≈ 35k;
  - half-pixel step: ≈ 2.3k.

  That is ≈ 49k reads per block per direction (design review 4 assumed
  ~25k), ≈ 3.2 G reads per source frame. Expect ~2–5 ms per source frame on
  the 3090, against the ~3 ms budget. Each warp is ~0.1–0.2 ms per extra
  present. Levers, best first:
  1. Load each block's search area once into shared memory and compute the
     sums from there — most reads are repeats; likely several times faster.
  2. Skip the zero-centred searches when the coarse result is zero.
  3. Whole-texel steps at 1/8, or one centre instead of two.
  4. A smaller 1/32 range (±2).
- (e) Untested: 4:2:0 (chroma divided by 2 vertically — code right, never
  run); real footage, grain and noise; rotation or zoom; motion near the
  ±96 px limit; flat areas; the revealed (trailing) edge of the occlusion
  case (only the covered edge is asserted); A/B size mismatch and Warp before
  Estimate (simple guards, read only).

## Verified OK
- (a) Q3:
  - 8×8 blocks at half resolution on source luma (= 16×16 source px);
  - refined to half a pixel with bilinear samples (`sad_sub`);
  - forward/backward consistency `|f + g| ≤ 1` half-resolution pixel, with
    `g` taken from the B-block where A's block lands;
  - a separate `matchTolerance` (3 codes, mean over the 12×12 window), not
    shared with the dither's;
  - untrusted blocks are a hard hold.
  The 1/32 → 1/8 → ½ pyramid carrying two candidates (zero-centred and
  coarse-centred) goes beyond the spec; the level conversions (×4 each) and
  ranges are consistent. Sound: the full-resolution match always decides, so
  a bad coarse guess cannot win. Design review 4 N-1 accepted it.
- (b) Acceptance criteria:
  - no blend (above);
  - `shift` (s·δ) is one uniform per Warp call, so it is global per frame;
  - `trusted = false`, no estimate, or shift 0 return A's own planes (same
    textures, no dispatch);
  - edges clamp.
- (c) Chroma: each CbCr pixel finds its block from its luma-space centre
  (`c × chroma_scale`) and moves by `v / chroma_scale`, with `chroma_scale` =
  (2, 1) for 4:2:2 and (2, 2) for 4:2:0. The tested 4:2:2 chroma lands at
  (3, 3) chroma pixels.
- (d) libplacebo use:
  - `pl_shader_custom` as compute;
  - variable data is copied by libplacebo (`sh_var` → `sh_memdup`), so stack
    values are safe;
  - descriptor names are unique per pass;
  - CLAMP address mode everywhere; LINEAR only where sub-pixel reads happen
    (`to_half`, `to_l1`, the warp source), on formats requested with
    `PL_FMT_CAP_LINEAR`;
  - search and field textures are r16f/rgba16f, sampleable and storable;
  - warped planes use A's exact format and size, and storability is checked;
  - `pl_tex_recreate` reuses textures when unchanged;
  - one `pl_dispatch` per object, freed in the destructor.
  The double-binding finding is confirmed (above).
- (e) Hazards: every pass reads only textures written by earlier passes and
  writes only its own `dst`. Separate dispatches are ordered. Every texture
  is fully written before it is read (pyramid, then both searches, then the
  consistency check, then the warp), and Warp refuses without a completed
  Estimate. A and B of different sizes are refused. Searches clamp their
  reads at image edges, so edge blocks match poorly and fall back to a hold.
- (f) The tests run the real component on WARP:
  - (12, 6) motion: ≥95% of interior blocks trusted with the right vector;
    luma and 4:2:2 chroma land at s·δ = 0.5; shift 0 returns A;
  - a cut: ≤2% trusted, warp output ≥97% identical to A, caller flag = A;
  - occlusion: the covered edge is untrusted and exactly held, the square
    and the far background trusted.
  These would fail on wrong vectors, a wrong chroma division, a blend at a
  cut, or a soft hold. (The half-pixel gap is under Should fix.)
- (h) No behaviour change: only the component, the tests and the two project
  files reference `Dejudder`; nothing in the renderer calls it.
- (i)2: the warped planes have A's format and size (stride is the GPU's own;
  range/levels are metadata in the caller's `pl_frame`), so 6b can pass them
  to `pl_render_image` in A's frame unchanged.
- (i)3: the constructor only creates the `pl_dispatch` (CPU-side, no GPU
  memory). Textures are allocated, and passes dispatched, only inside
  Estimate and a real Warp. So "off" costs nothing if 6b constructs the
  component only when the de-judder or the mask is on.
