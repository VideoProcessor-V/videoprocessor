# Stage 6 de-judder — planner answers to the design note

2026-10-10. Answers to `docs/VP-stage6-dejudder-design.md`. The note is right
about what the renderer does today, and its proposed shape is better than
the brief's sketch, because it warps the *source* frame and lets everything
downstream apply unchanged. Approved with the decisions below, which are
also applied to the brief's Stage 6.

## Q1 — 2a (extra presents inside the existing loop): yes

Approved, with the note's own guards made explicit requirements:

- One flag (`dejudder_strength > 0`), one code block. Off means today's
  path byte-for-byte; the Code auditor checks this by diffing the present
  sequence with the flag off.
- Extra presents only fill refreshes A already owns; B is never delayed. If
  B's due time is within one refresh, skip the last extra image.
- Drift correction is untouched and keeps acting per source frame.
- Phase comes from A's capture QPC against the predicted vsync, as
  proposed; the constant pipeline delay cancels. While the feature is new,
  log φ0 and each δ_k per present, and show the stats line as described.
- It switches off automatically in `Low_Latency`, for non-film cadence
  (source rate ≥ display rate / 1.5), and for any frame where a cut is
  detected between A and B.
- 2b (a vsync presenter thread) is not wanted.

## Q2 — occlusion mask: M1 on the harness first, M3 as the fallback, M2 no

- **M1:** the mask goes in the warped source frame's alpha; the sigma-delta
  reads `HOOKED.a` at OUTPUT and writes alpha 1. Try it on the GPU harness
  first. The test must prove, at our settings:
  - libplacebo leaves RGB untouched with a source alpha plane: no
    premultiply and no background blend;
  - alpha survives scaling as a usable mask.

  Also check libplacebo's alpha mode enum and the renderer's blend and
  background parameters, and pin them explicitly rather than relying on
  defaults.
- **M3, if M1 fails:** accept the bounded error and record it. The
  reasoning in delta 2 holds, and there is a second reason the error is
  mild. Occluded blocks are hard-held to A's pixels, so between extra
  presents those pixels are *static* and the sigma-delta's backward carry
  there is correct. The only exposure is at the A→B boundary, where the
  gate fires anyway.
- **M2** (hosting the sigma-delta natively) is not wanted now. The generic
  API is not worth its size while M1 may give the same result as a
  convention.

Whichever lands, record it in the brief under Stage 6 and in the shader's
header.

**Mask-only mode (required in 6c; added after design review 4):** the
dither's occlusion refinement must not depend on the de-judder being on.
- Add `occlusion_mask: on|off` beside `dejudder_strength`.
- With strength 0 and the mask on, the component runs the bidirectional
  matcher once per source frame and publishes the mask (M1 path). It makes
  no extra presents and no warp; the presentation loop is untouched. The
  cost is the matching plus B's upload, nothing else.
- Cinema profile only, as with the rest.
- If M3 is the outcome, the key still exists and is a documented no-op.

## Q3 — half-resolution 8×8 matcher on source luma: yes

It suits the budget and the use. Two requirements go beyond Stage 4's
matcher:

- Vector quality matters here, unlike for the dither, so refine to half a
  pixel at the fine level. Integer vectors scaled by `s·δ` produce
  fractional warps anyway, and the refinement is cheap.
- The consistency check is within 1 px at half resolution, as proposed; a
  block that fails is occluded and gets a hard hold. A block whose best
  match error exceeds a configurable tolerance is also untrusted (the same
  rule as Stage 4's `match_tolerance`, under a separate key).

Chroma: 4:2:2 payloads need the horizontal vector component halved for the
CbCr plane. Say so in the warp code.

## Q4 — B's double upload: later

One extra 4K upload per source frame at 24p is well inside the PCIe budget.
Optimise only if the stats line shows upload time competing with the warp.
Note the reuse idea in the code as a comment.

## Additional points (not asked, but needed)

1. **LED measurement and extra presents.** The exact PRE_OUTPUT measurement
   will now run on every present, so a 24p source frame gets two or three
   exact readings. The LED module's correction ratio pairs a raw prediction
   with the exact reading for the *same source frame*. Pair them by
   source-frame identity (capture timestamp or queue id), not by present
   count, or the ratio will drift. Use either the first present's reading
   or the mean of A's presents; pick one and state it.
2. **Sigma-delta per present is correct and wanted.** Each extra present is
   a distinct picture, so the quantizer's per-refresh carry behaves exactly
   as it does on repeats today; nothing to change. The LED compensation
   gain is already per frame.
3. **Acceptance criteria for the Code auditor,** because they are the
   owner's non-negotiables:
   - `s` is global per frame;
   - a hard hold on failure;
   - no blend.
4. **Edge handling in the warp:** a pixel whose source lies outside the
   frame after displacement takes A's pixel at the edge (clamp), not black,
   or the borders will flicker on pans.

## Settings and build order: approved as proposed

- **Setting:** `dejudder_strength` in the rendering profile. 0 or absent
  means off. Config default 0.25 ("Gentle"), 0.35 ("Medium"), or custom.
  Cinema profile only; off in the test config.
- **Tally:** items for pan judder, edge artefacts and render time.
- **Build order:**
  1. 6a: the standalone component, with WARP tests.
  2. 6b: the ring, presents, phase, flag, stats line and Config box.
  3. 6c: the mask (M1 or M3).

  Each step is committed, packaged for design review first, then audited.
