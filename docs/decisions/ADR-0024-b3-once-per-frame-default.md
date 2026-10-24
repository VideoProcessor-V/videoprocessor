# ADR-0024: The input side runs once per source frame (B3); the default once accepted, a hard gate

- Status: Accepted (implementation in progress; the default is OFF until the acceptance below passes)
- Date: 2026-10-24 (owner decisions)
- Slots: the input-side slots (re-base, repair, deband, texture restore, 4K route, refine,
  chroma), motion.consumers, motion.vector_delivery

## Context

Today the renderer runs the motion consumers BEFORE the entrance:

- De-judder extras and phase pictures warp the NATIVE planes.
- Each warped picture is then sent through the whole entrance again.
- The debander hold and the detail history re-run A once more.

At 24p in 60, a net in the entrance would run 2-3 times per film frame (barrier B3). The design
(VP-pipeline-final.md 3.8) puts the consumers AFTER the input side, warping the processed frame.

## Decision

1. **B3 is the target behaviour and becomes the DEFAULT once it passes its acceptance.**
   - The input side runs once per real source frame, into an entrance cache that owns its
     textures (A and B).
   - De-judder, hiccup repair and interpolation pictures warp the cached PROCESSED frames.
   - Vectors are scaled per axis to the processed geometry and mapped through crop/A-lens/NLS.
   - Vector validity and fallbacks are unchanged: invalid vectors show the plain processed frame.
2. **It is a HARD GATE.** There is no deploy, no motion training and no dependent net integration
   until B3 passes all nine real-clip acceptance criteria below and is the default.
3. **The switch stays only as a diagnostic/fallback.**
   - OFF must remain byte-identical to today's code.
   - The Original pathway is unchanged (ADR-0019).
   - While OFF or unavailable, a visible status says so ("Fallbacks engaged").
4. **During source-resolution crossfades,** version 1 keeps the old order as a visible fallback.

## Acceptance (real clips, every item before default-ON)

1. **Byte-identity:** still or locked frames, and any frame with invalid vectors, are
   byte-identical to the plain processed frame.
2. **No pops:** no temporal pop at source-frame boundaries (A to B continuity of the processed
   frames, no hold-state jump).
3. **Pans:** de-judder smoothness on real 24p-in-60 pans is equal to or better than B3 off
   (owner A/B).
4. **Repair paths:** hiccup repair and interpolation are correct with a processed B (the gap and
   standalone pairs included).
5. **Grain kept:** at least 90-95% per the grain gates. Warping the processed frame must never
   smooth grain.
6. **Cost:** the input-side GPU cost per source frame drops about 2-3x at 24p-in-60 (SlotTiming,
   before/after, laptop and projected 3090).
7. **Delay:** the delay gate passes (constant total, no stalls), with the 4/3/2 ladder and
   recovery.
8. **Signal formats:** HDR/LLDV, BTB/WTW probes, the DV tunnel and RGB capture are unchanged
   (no clamps; probe codes identical).
9. **Geometry:** subtitles, crop, A-lens and NLS stay in the right place, and vectors map
   through them (no misplaced warps at edges or bars).

## Consequences

- The net runner and net A integration wait for B3.
- `UpstreamKey` excludes `motion.consumers` until B3 lands, then includes them at their real
  position.
- The work plan is the B3 needs assessment:
  - the cache ring;
  - shaders split into estimate size and target size;
  - B processed ahead;
  - the renderer wiring behind the switch;
  - crossfade;
  - visibility;
  - the stateful stages;
  - SlotTiming;
  - the tests.

## Implementation (2026-10-24, feat/skeleton)

- Switch: `[vpv] pipeline_entrance_once` (on|off, default off; also read from `[vprenderer]`;
  Config expert page). The Original pathway never uses it.
- Ring: `EntranceStages::Ring` (owned copies of A and B by sequence, structure key and role).
- B ahead as its own A: with stateful stages, B's own field is estimated ahead (the de-judder's
  estimate ahead, A's pictures read the (A, B) snapshot), so B ahead is B drawn as A. Otherwise
  (no C queued, an estimate failure) B is processed per picture for that frame, visibly.
- Shaders: `MotionWarp::WarpProcessed`, `FrameInterpolator::Run(..., estimate size)`.
- Fallbacks (visible, "Fallbacks engaged: entrance once"): crossfade, composed picture, copy
  failure, B's vectors not ready ahead.
- Tests: `EntranceOnceMotionPicturesFromTheProcessedFrames`, `EntranceOnceRealClipAcceptance`
  (criteria 1, 2, 5, 6, 8 on a real film clip), every harness test with
  `VP_TEST_ENTRANCE_ONCE=1` (criteria 4, 9), CPU ring tests.
