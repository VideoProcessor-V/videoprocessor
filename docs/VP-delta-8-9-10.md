# Deltas 8, 9, 10 — reuse, synthesis-lite, per-cadence strength

2026-10-13. Owner-approved. Recorded from the planner's handoffs.

**Common rules.**

- Each item sits behind its own setting; off = today's path exactly.
- Each is tested under both warp paths where it applies.
- All come before the final gate.

## Delta 8 — two reuse refinements

**R1 — the sigma-delta carries its residual on the de-judder's vectors.**

- **What changes.** When the matcher runs, the sigma-delta takes the
  de-judder's trusted field and skips its own search.
  - In the rendered path the field is already mapped into output space for
    the warp.
  - That field arrives as a block-resolution RG16F texture in output space,
    with trust in B.
- **Delivery.** Fork patch 0004 (planner, 2026-10-13):
  `pl_mpv_user_shader_set_texture`.
  - The shader declares `//!TEXTURE VPVEC` as a 1×1 placeholder and
    `//!BIND VPVEC` in its carry pass.
  - The original texture is restored before destroy.
- **Settings.**
  - `//!PARAM vector_source` (0 = own matcher, 1 = external field).
  - `dither_vectors: auto | own`, default `auto`. `auto` = external when the
    matcher ran this frame and the patch is present, else own.
- **Fallback.** Low_Latency, video cadences, no matcher this frame, or 7.360
  without the patch: the sigma-delta's own matcher.
- **Tests.**
  - Along a synthetic pan, the external carry matches the own-matcher carry
    within the gate tolerance, and the own search is not dispatched.
  - `own` is byte-identical to today.
  - Both warp paths; the matrix axis.
  - The `auto` fallback with the patch present and absent.
  - Destroying the hook with VPVEC bound leaves no dangling reference.
- **Expected.** About 1 ms less per present where the de-judder runs.
- **Records.** Patch 0004 goes in the brief's libplacebo facts and in
  BUILD-STATUS. It is the third fork-only hook facility, and any libplacebo
  rebuild must carry it.

**R2 — grain-suppressed highlight statistics for the LED drive** (built,
77f74227).

- Setting: `led_highlight_grain_suppress: on`.
- A grainy block's histogram entry leans by g toward the mean of the 3×3
  blocks around it.
- The APL and the script-mode block maximum (SF-4) are unchanged.

## Delta 9 — synthesis-lite: three cheap changes to the hold

These are per-pixel or per-block arithmetic inside passes that already run.
None of them blends between frames. They apply to both warp paths.

**H1 — feathered hold boundaries** (`dejudder_hold_feather_px: 4`; 0 = off).

- Where a held block meets a warped one, blend over a band of
  `hold_feather_px` output pixels between the warped A and the unwarped A,
  with a smoothstep.
- This is computed in the warp bodies from the trust of the neighbouring
  blocks. The occlusion mask is unchanged.
- Test: the boundary has no step greater than the feather's slope; off gives
  a hard edge.

**H2 — hold hysteresis** (`dejudder_hold_hysteresis: 2`; 1 = off).

- A block must be trusted for N consecutive source frames before it warps,
  and untrusted for N frames before it holds.
- A cut resets every block to untrusted at once.
- There is one small counter per block, carried in a ping-pong; it also
  applies to sub-blocks when E2 split them.
- Test: an alternating block stays put; a real change takes effect after N
  frames; a cut holds immediately.

**H3 — per-block strength taper** (`dejudder_block_taper: off`).

- This departs from "s global per frame". It is off by default, for the
  owner to judge.
- A block whose SAD lies between `match_tolerance` and `taper_sad` (default
  2× tolerance) warps at `s × ramp(SAD)` instead of being held.
- Test:
  - a marginal block warps at the ramped strength when on and is held when
    off;
  - fully trusted and fully untrusted blocks are unchanged.

**Not included (recorded).**

- OBMC.
- Bidirectional synthesis (the frame blend the owner rejects).
- D5 on the rendered path.

**Tests.** Each runs under both warp paths. The matrix's "all refinements
on" axis gains H1–H2 on, with H3 off.

## Delta 10 — per-cadence de-judder strength (even cadences allowed)

The owner's observation: 30p (nature documentaries) is shot to look real,
and 30 Hz judder on slow pans is what viewers notice.

**Settings.**

- `dejudder_strength`: uneven cadences (24p on 60 Hz), default 0.25.
- `dejudder_strength_even`: even cadences (2:2 — 30p on 60 Hz, 25p on
  50 Hz), default 0 (= off, today).
  - When above 0, the extras run at the midpoint (δ = 0.5).
  - At s 1.0 this is true frame-rate doubling from A, with the hold fallback.
- `dejudder_even_rates: 30 | 30_and_29.97`, default `30`. With `30`, only
  exact 30.00 (±0.01) takes the even strength; 29.97 (broadcast) is opt-in.
- All three are rendering-profile keys, so `${cadence}` rules select them per
  source.

**Rules that do not change.**

- 25p on 60 Hz is uneven (2.4 refreshes per frame) and takes the normal
  strength.
- The "≥ 2.0 refreshes per source frame" rule stays (50p on 60 Hz is
  excluded), and Low_Latency stays off.

**Config card and BUILD-STATUS.** Both show both strengths with their
cadences, and these caveats:

- At s 1.0 quality rides on the estimator. Held regions show as 30 Hz judder
  inside a smooth frame.
- The s = 1 experiment becomes the 30p nature-documentary test in the first
  cinema session.

**Tests.**

- 30p with the even strength at 0: no extras.
- 29.97 with `30`: no extras; with `30_and_29.97`: extras. 30.00 gets them
  in both cases.
- At 1.0:
  - one extra per source frame, at δ = 0.5;
  - the extra equals the true halfway frame on a synthetic pan;
  - an untrusted block holds;
  - a cut gives A.
- 24p is unaffected; 50p on 60 Hz is still excluded.
- Both warp paths; the matrix axis.
