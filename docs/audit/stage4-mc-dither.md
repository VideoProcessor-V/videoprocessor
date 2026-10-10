# Audit — Stage 4: motion-compensated residual carry (sigma-delta)

Diff audited: 1dd05504..ba19ee26 on stage7-rc2 (d55443e2, 950872b7, 91ebdaee,
01620bd9, ba19ee26). The other Claude session was idle throughout.
Build: OK — full x64 Release /t:Rebuild at ba19ee26, 0 errors, no new warnings.
Tests: native 2466 passed, 1 failed (ShellStylePlacementWithoutSizingHintsIsNotReverted — known);
the three sigma-delta GPU tests pass. VideoProcessorConfigTests 97 passed, 2 failed (the two
known RPC tests).

Spec used: brief §5, Stage 4 (with its 2026-10-10 status note), §7; the
design reviewer's three confirmations; libplacebo 7.360.1
src/shaders/custom_mpv.c (bundled source).

## Blockers
- None.

## Should fix
- shaders/Temporal sigma-delta.glsl, quantizer `ign(vec2(pos), frame)` —
  **(b)3 fails: with `motion_compensation = 0` the output is NOT byte-for-byte
  the earlier version's.** libplacebo adds 1 to `frame` for every *pass* it
  runs (custom_mpv.c ~1363, `p->frame_count++` per pass). The old shader had
  one pass, so the quantizer saw frame = 1, 2, 3…; it now runs fifth of five
  passes and sees 5, 10, 15…. The decorrelation noise therefore takes a
  different pattern each refresh whenever `noise_amplitude` > 0 (default
  0.15). Same amplitude, and it still cycles through all 64 offsets, so it
  looks statistically the same — but the shader header's "behaves exactly as
  the stage 2 version" and the reviewer's byte-for-byte premise are wrong.
  Identical only with `noise_amplitude: 0`. Either use `frame / 5` in the
  quantizer (all five passes always run) or correct the claim. Stage 3 has
  not been photographed yet, so nothing measured is invalidated.
- Same shader, MV/quantizer — **vectors are whole quarter-resolution texels,
  i.e. 4-pixel steps** (`v = round(mv) * 4`). For motion that is not a
  multiple of 4 px per refresh, the carry comes from up to 2 px beside the
  true source. The brief accepted that ("only misplaces ≤0.5 code"), but this
  design also gates the carry against the *displaced* previous value. On
  detailed texture a 1–2 px offset usually fails that gate, so the pixel
  falls back to the plain gate and loses its residual — the Stage 4 gain is
  then largely lost for real pans (arbitrary speeds). On smooth areas the
  offset passes the gate and carries the residual a little off target, as the
  brief allowed. All three motion tests use 4, 20 and 48 px (multiples of 4),
  so this is not seen. Designer to decide: add a full-pixel refinement (e.g.
  ±2 px around the vector, per block) or accept it; either way add a test at
  e.g. 5 and 10 px/refresh.

## Notes
- The MV search runs every refresh even with `motion_compensation = 0`
  (no early return on that parameter), so switching it off saves no GPU time.
- Picture edges: the carry source is bounds-checked against the state
  texture (whole output), not the picture. Content entering from a blanked or
  letterbox edge can read a texel outside the picture: never written there
  (no tag → refused), or left from a wider picture before a zoom change
  (valid tag, so it still has to pass the gate). At worst ≤0.5 code once.
- LQPREV's tag is checked only at texel (0,0). After the picture rectangle
  grows without a renderer reset, the new area reads stale or uninitialised
  luma. NaN/garbage gives a failing SAD or confidence 0, and every carry is
  still gated, so it is harmless.
- (f) Cost, rough estimate (not measured — tally item 17): at 2560×1600,
  - quarter luma: 256k threads × 16 reads;
  - coarse luma: trivial;
  - motion search: 16k blocks × ~11.6k reads ≈ 190 M reads, with only 16k
    threads, so latency-bound — about 0.1–0.5 ms;
  - state copy: a full-size read+write of two rgba16f textures plus a dummy
    output, ≈ 0.1 ms;
  - quantizer: ≈ 0.1 ms.
  Likely 0.3–0.8 ms in total, so plausibly within the ~1 ms budget, but only
  the 3090 overlay can tell. Memory: six 2560×1600 rgba16f storage textures
  = 6 × 32.8 MB ≈ 197 MB, plus about 33 MB per refresh for the state pass's
  dummy output (libplacebo allocates it at full size whatever COMPONENTS
  says). Fine on 24 GB.
- (e) Untested: diagonal or vertical motion; non-multiple-of-4 speeds (above);
  occlusion edges (one object over another); limited range (`levels_full 0`,
  the cinema setting); 10-bit swapchain (`swapchain_bits 10`, the cinema
  setting); `noise_amplitude` > 0; 3:2 film cadence (0/N px alternating), which
  the vector-keeping logic exists for. No test compares `motion_compensation
  0` against the previous shader.
- (g) Changing Projector gamma also changes the gamma boost's strength (the
  boost exponent is `compensation_eotf / gamma_boost`). At 2.41 with boost
  2.4 there is practically no lift left. That is consistent with design
  review 2 SF-1, but the owner should know the two move together.

## Verified OK
- (a) Matches the brief:
  - Luma is taken from the gamma-encoded output codes (OUTPUT stage,
    BT.709 weights).
  - Quarter level and 1/16 coarse level.
  - Coarse full search ±4 coarse texels (±64 px), then fine full searches
    ±2 around zero and the coarse result, ±1 around the block's last real
    vector.
  - Forward only, against the persistent previous-luma texture LQPREV.
  - Vectors, confidence and error go into the MV texture per 16×16 block;
    the last real vector is kept in MVPREV through 3:2 repeats.
  - The quantizer takes the residual from `pos − v`; if that fails, the
    plain same-pixel gate; then zero.
- (b)1: `carried(from, …)` compares the current value with PREV at the
  *displaced* position, not the same screen position.
- (b)2: the passes are separate dispatches in a fixed order (quarter luma,
  coarse, search, state, quantizer). D3D11 makes one dispatch's writes
  visible to the next, so the state pass's copy (staging → read set) always
  finishes before any quantizer pixel of the same refresh reads or writes.
- (b)3 logic: apart from the noise counter above, the carry and gate
  decisions with `motion_compensation 0` are the old ones. The old code read
  RESID/PREV in place; the new code reads the copy of last refresh's staging
  values, which hold the same data.
- (c) No pass reads a texel another invocation of the same pass writes:
  - search: reads LQPREV, reads and writes only its own MVPREV texel;
  - state: copies RESID_NEXT→RESID and PREV_NEXT→PREV at its own pixel, and
    writes LQPREV (read only by the earlier search pass);
  - quantizer: reads RESID/PREV anywhere, writes only RESID_NEXT/PREV_NEXT.
  First refresh: no LQPREV tag, so no vectors and MVPREV is initialised. A
  resolution change rebuilds the hook (new, untagged textures). Picture edges
  are clamped in the searches and bounds-checked in the quantizer.
- (d) custom_mpv.c:
  - Sizes: WIDTH/HEIGHT `(w+1.5)/4` and `(w+7.5)/16` are rounded with
    `roundf`, which equals rounding up (ceil w/4, ceil w/16) for every width.
    The MV grid has exactly one texel per 16-px block and covers the edge.
  - COMPUTE 16 16 / 8 8 are dispatched as "divide and round up", with an
    in-shader size check.
  - SAVE to a new name leaves the picture (HOOKED) untouched; BIND gets the
    whole saved texture.
  - The COMPONENTS 1 dummy output is never bound, so it is harmless.
  - The `frame` uniform does count passes, not refreshes — confirmed; hence
    the staging set (and the should-fix above).
- (e) The GPU tests run the real shader file through libplacebo on WARP:
  - 4, 20 and 48 px/refresh textured motion: the time-average along the
    motion is within 0.05 code with compensation, over 0.2 with the plain
    gate;
  - a still picture is unchanged;
  - a scene cut carries on at most 2% of pixels.
- (g) Projector gamma: a 2-decimal number box, 1.80–3.00, showing 2.20 when
  unset. It writes `compensation_eotf` to `[led_drive.Default]` (wins over the
  profile), the key VP's LED drive and compensation hook read (the single
  value checked in round 2 (b)); the test saves 2.41. Tick boxes appear only
  for range-0–1 settings whose description starts "0 =/1 =/0 for/1 when"
  (enabled, levels_full, motion_compensation, test_ramp; NLS warp_axis) and
  save `param_<name>: 1/0`. A plain 0–1 strength stays a number (tested).
