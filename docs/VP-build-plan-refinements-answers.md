# Build plan (refinements to the cut) — planner answers

2026-10-12. Answers to docs/VP-build-plan-refinements.md. (Recorded from
the planner's handoff.)

## 1. S2 — not unorm: use float32 for the output-pass FBOs

Unorm is ruled out:
- A global unorm switch clips out-of-range values when Stage 10 is off, and
  crushes linear-light SDR shadows.
- Even a post-colour-management unorm would clip at PRE_OUTPUT. There the
  picture is in the target transfer but still pre-encode: a WTW code 255
  sits at (255−16)/219 ≈ 1.09 and BTB below 0.
- Only after `encode_color` (OUTPUT) is the limited-range picture inside
  [0,1], and by then the float16 rounding at PRE_OUTPUT has already happened.
- The scoped flag would almost never apply in the owner's cinema profile,
  which scales in linear light.

Use **rgba32f** for the FBOs from colour management onward:
- These are the output pass: the PRE_OUTPUT image, the OUTPUT-stage hook
  FBOs and the dither input. rgba16f stays for everything before.
- Float32 is exact to 2⁻²⁴, clips nothing and keeps the extended range.
- The cost is bandwidth only, at output resolution: about 64 MB per FBO at
  1600p, under 2% of the card's bandwidth at 60 Hz.

Patch:
- `renderer.c` `find_fbo_format` chooses one format. Give the output pass its
  own via `pl_render_params.output_fbo_float32` (fork-only, default false):
  prefer rgba32f, and fall back to the existing choice if the GPU lacks it.
- VP sets it unconditionally for the VP Renderer.
- If the output pass's format is not separable in 377 without a larger
  change, say so and leave S2 out of the cut.

Tests:
- OUTPUT and PRE_OUTPUT FBO formats are float32 when the flag applies.
- Highlight precision: max error at codes 900–1019 below 1/16 of a 10-bit
  step.
- A BTB/WTW probe with Stage 10 off and on through the output pass keeps its
  extended codes.
- The full matrix.

## 2. D2 / D4 defaults — OK as first values
- `motion_scale_lo 0.25 / hi 1.5` source px per source frame, and
  `trust_scale 0.6`.
- The owner tunes `hi` by eye if slow pans still show the hitch.

## 3. D2/D4 rate limit ±0.25 per source frame — OK
- Four frames (≈170 ms at 24p) from zero to full.
- Keep the immediate hold on a cut.

## 4. D3 — promote, but verify the promoted vector
- Promote an untrusted block when ≥7 of its 8 neighbours agree.
- Before trusting it, score the block's own match at the median vector and
  require it within `match_tolerance`. Otherwise an occluded block inside a
  moving region would get a confidently wrong vector.

## Everything else — as planned
- The de-judder itself stays off in the shipped sample, so "off = Bill's VP"
  holds.
- The matrix gains an "all refinements on" axis.

## Open gate finding — agreed; do not loosen the limit
- The pattern points at the limited-range encode path rather than at any
  hook. The 7.360 repair's −0.42 is the obvious suspect, but it must be named
  by measurement.
- Run the exact-code check (flat fields at known codes, chain and reference
  each against the expected code) in the final gate.
- Fix what it names, and only then decide the limit.
