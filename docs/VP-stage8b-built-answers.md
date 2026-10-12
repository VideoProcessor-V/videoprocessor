# Stage 8b built — planner answers (2026-10-12, night)

On d1837c6d. Recorded from the planner's handoff. The applied refinement
answers are as intended; the `auto` form of `fbo_float32` is the right way
to carry a queue-profile condition in a rendering key. Nothing blocks the
gate.

## A. Faithful branch source behind a CNN doubler — accepted
- With FSRCNNX/ArtCNN hooking LUMA, PRE_KERNEL is already the doubled
  picture, so "from PREKERNEL" is not the source.
- Box-average the 2x picture back to source resolution in the same light
  domain, then `ewa_lanczossharp` to the output. This relies on the
  doubler's own consistency, as SSimSuperRes already does: "the kernel from
  a consistent estimate of the source".
- Keep the skip-and-log for other prescale ratios. Record it in the 8b note.

## B. `upscale_light` — accept "linear" as built; name it plainly
- "linear" means libplacebo's linear light wherever it scales down (2x to
  1.48x behind a doubler), with sigmoid off. Plain upscales without a CNN
  are in gamma light.
- Do not keep sigmoid on: it clips WTW on upscales. The blend only needs
  both branches in one domain.
- Config card: "linear (downscale after a doubler) / gamma (all)". The
  owner's madVR habit is gamma; the first A/B compares the two.

## C. SuperRes after a doubler not unit-measurable — accepted
The matrix (real FSRCNNX + SSimSuperRes) is sufficient cover. Add one line
to the test's comment saying why the matrix covers it rather than the unit
test, so an auditor does not read it as a gap.

## D. No separate 8b test under the two warp paths — accepted
8b runs in the main render: the source warp's extras re-run it and the
rendered warp warps its result. Note it in BUILD-STATUS alongside Stage 8.

## Also
- The clean-log filter must ignore only libplacebo's timing messages, by
  text, not anything with "slow" in it.
- Beta full matrix 488/488 before 8b, with the re-run in progress: that is
  the state to deploy from.

## Next
- Gate → audit → deploy folder, on the owner's request.
- The first cinema session checklist gains, under item 4: "1080p source
  with grain: CNN alone → adaptive upscale on."
