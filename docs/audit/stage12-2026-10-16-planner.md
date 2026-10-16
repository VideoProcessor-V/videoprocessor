# Planner design review — 947f4c31..HEAD (merge-v1.4.02), 2026-10-16

Save as `docs/audit/stage12-2026-10-16-planner.md`. This is the **design**
review of the range; the Code auditor's file (`stage12-2026-10-16.md`)
covers build, tests and line-level conformance and should be written
separately by the auditor session against the same scope list. Where I
ask the auditor to check something, it is marked **[auditor]**.

Overall: a very large range (ten parts, Bill's v1.4.02 merged, several
owner-driven additions beyond the cut list). Nothing here is a design
blocker. The four "not yet" items at the end of the request — GPU cost
measurements, the whole-chain matrix with the Stage 12 axes, the GeForce
gate, Beta 377 for this range — **are** the gate, and the uncommitted
pixel-identity divergence is the one thing I would trace before the
deploy folder (see Q8).

---

## Parts 1–3 (motion follow-ups, stream robustness, efficiency)

- Landing check on M6 (a source pixel counts only if its own motion
  carries it back within 1.5 px): correct — it is the per-pixel form of
  the forward/backward consistency the block decision had. Accepted.
- D3 to storage with its document: right, per the 10-14 answer.
- Stream robustness (general pulldown, repeats/drops, drift, jitter,
  resync, no fixed rates or resolutions): this is the "config-adaptable"
  constraint applied to timing; good. **[auditor]** the vector consumers'
  fallbacks on a gap must be the same ones `Reason::SmoothMotion` uses
  (part 8), so there is one no-vector path, not two.
- Efficiency byte-identical by test, rejected variants documented, NVIDIA
  optical flow experimental and off with MIT headers kept: all as it
  should be. The optical-flow experiment belongs under Stage 11b's
  "candidate finder" heading in the record, not as a separate thread.

## Part 4 (native RGB ingest), Part 5 (early upload), Part 6 (12-bit output)

- GPU RGB unpack on by default with the R10b/R10l and ARGB/BGRA fixes:
  accepted; it closes the Q6 item from the 10-13 review.
  `ycbcr_12bit_capture` with no Config control: give it one (constraint
  12) even if it is a single checkbox.
- Early upload experimental and off: right to keep off; the probe
  finding (libplacebo's D3D11 has no host-mapped buffers) is a libplacebo
  fact for the brief's §3.
- 12-bit output: the rgba32f cache and warped picture are the right
  minimum; dropping the float display surface is the owner's call and the
  10-bit surface is what the DVI chain can use anyway. Record the reason
  ("parallel-path hassle") so it is not reopened.

## Part 7 (LED drive)

- The Python source retired after a 0-difference correlation over 482
  loops on real clips, script pacing as the default (150/30 ms), one LED
  core around the general RS-232 driver, legacy modes mapped and logged,
  retired keys accepted and ignored: exactly the end state the policy
  wanted. **[auditor]** confirm `picture_only` and `none` still never open
  the port after the core refactor.
- `source_range auto | full | limited` set once from the captured signal
  and followed by the picture and the LED look-ahead: correct; the LED's
  exact measurement stays in limited code units so the script's curves
  hold. **[auditor]** the raw-frame 1D transfer (delta 1 item 5) must use
  the same range decision.

## Part 8 (the v1.4.02 merge)

- Plugin API 29 = ours 27 + his 28; his DV tunnel first on the upload
  route with our GPU unpack kept off the tunnel; his analysis helper
  replacing our inline raw-route analysis; our motion invalidation on
  gaps kept beside his DV gap handling; his smooth-motion draw loop
  wrapping our render call with the extras callbacks: all sound as
  described. **[auditor]** confirm PR #168's migration fix (named Default
  motion profile, "motion" in the migration child list) is on this
  branch — the request asks and I cannot see the tree.
- `motion_engine auto | vp | smooth_motion`, one or the other by the same
  profile rules, with VP's de-judder stepping aside via
  `Reason::SmoothMotion` so every vector consumer takes its no-vector
  fallback: correct. See Q6 for the completeness list.
- One `chroma_upscaler` implementation (auto = Catmull-Rom; his
  ewa_lanczos; our hooks): fine; the CfL hook remains the owner's choice
  through the standard-shader entry.

## Part 9 (crash state) — see Q7.

## Part 10 (Stage 12 + delta 15) — the eight deviations

1. **Re-base in the signal domain, per plane:** accepted, and arguably
   more correct than the spec. The upstream upscale being undone was done
   in the signal domain; inverting it there inverts it, inverting it in
   linear light would not.
2. **Any height 144–8640:** accepted (constraint 10).
3. **Mirror coherence instead of the radial spectrum:** accepted, and a
   better detector — the spectral cliff overlapping on real clips (4K to
   0.92 decades, upscales from 0.29) is the kind of result that only
   real footage gives, and coherence separating 0.1–0.2 from 0.9–0.99
   is decisive. Grain-map exclusion not built: **confirmed** — coherence
   measures the mirror symmetry of the upstream kernel, and grain has
   none, so it is insensitive by construction. Keep the seven-clip
   numbers as the regression fixture.
4. **Auto selectable, default native; confidence = evidence × two-axis
   clarity:** accepted; see Q1/Q2 for the two refinements.
5. **Effective bit depth from code occupancy:** accepted; "multiples of
   4" was the SDR-only form, and SDR re-encoded to PQ reading 9 is
   exactly why occupancy is the right measure.
6. **Debander without grain, fixed ring, hold confined to the blur band;
   default off:** accepted (and Q3: off until the projector check).
7. **A4 within half-float — worst pixel 0.0023 (< one 8-bit code), the
   upstream kernel's own softening:** accepted; record the number.
8. **One floor key for SSIM and CNN; 2c's anti-ringing via
   `low_bitrate_handling` + `ssim_bitrate_aware`:** accepted; fewer keys
   is better where the behaviour is the same.

Results: 0/42 genuine frames reading lower is the number that matters;
coded 1440 at 11/28 (rest native) is the detector erring the right way.
The debander's 1.15 → 0.07 codes RMS with 98% texture kept, and the
residual flicker halved, are the two visible wins to look for on the
projector.

## Questions

**Q1 — bias to native.** Strong enough as described (several shots and
60 s of evidence before a lower reading; native at once on detail above
it). Two cases:
- A camera that bins both axes produces genuine 4K sampling of a
  lower-detail image — no mirror structure, so coherence stays low and it
  reads native. Correct.
- A 2K DI finished to 4K **is** a genuine upscale: the information is
  not there, and re-basing it to 2K and applying VP's upscale is the
  right treatment, not a false positive. Say so in the tooltip ("many
  films are 2K masters; `auto` will re-base them, and that is correct").
The one case to watch is a 4K master with deliberate softness
(diffusion, old anamorphic glass): no mirror symmetry, so it should read
native — add one such clip to the fixture set when the owner has one.

**Q2 — auto disengage.** Split by cause. Disengaging *because detail
above the reading appeared* must stay immediate: being re-based while
true 4K detail is present is the harmful direction. Disengaging *because
confidence merely dropped* should wait for a cut, bounded (≤ 5 s), so
the scaler path does not change mid-scene; if no cut comes within the
bound, disengage anyway. Engage stays at a cut.

**Q3 — debander default.** Off until the projector check. The spec said
`auto` before the HDR speck finding and before the owner had seen any
debanding on this display; `auto` is a one-key change afterwards.

**Q4 — settings fingerprint coverage.** **[auditor]** enumerate every
`RendererSettings` field against the fingerprint's inputs; then make it
structural: a test that builds the fingerprint from the parser's key
list (or a reflection of the struct) so a new key cannot be added
without entering the fingerprint. The three that were missed
(`source_range`, `capture_upload`, the Stage 12 keys) are the kind of
thing that only shows up as "the setting did not apply live," which is
hard to notice in the cinema.

**Q5 — the owner's slider scheme.** Five pages, each one slider of ≤ 5
positions whose presets fill the sub-settings in the box below (still
editable; "reset to defaults" restores the preset):

- **Motion** — Off / Gentle (s 0.15) / Standard (0.25) / Strong (0.35) /
  Smooth (even cadences at 1.0, `dejudder_strength_even`). Sub-settings:
  per-cadence strengths, `object_strength`, R-A roll-off, M6, landing
  check, hysteresis, feather, estimator options (E1–E4), `motion_engine`
  (auto / vp / smooth_motion) shown but not moved by the slider.
- **Picture (Stage 12)** — Off / Detect only / Conservative / Standard /
  Aggressive. Sub-settings: `source_resolution` (native/auto/height),
  `low_bitrate_handling`, `deband_vp` and its threshold, residual
  steadying (SSIM and CNN), `ssim_bitrate_floor`. "Detect only" = the
  readings on the stats line, no response.
- **Scaling quality** — Fast / Standard / Best. Sub-settings: chroma
  (Catmull-Rom / ewa / CfL), upscaler (kernel / FSRCNNX / ArtCNN),
  SSIM blend and grain map, `fbo_float32`.
- **Dither** — Off / Standard / Motion-carried (with external vectors).
  Sub-settings: tolerances, grain-aware carry, noise amplitude.
- **Light (LED)** — Off / Picture only / Script / Compensate (standard
  floors) / Compensate deep (floor 60, `max_cut_dip` lower). Sub-settings:
  the policy keys.
Keep the GPU cost markers (+/++/+++) on each slider position.

**Q6 — one-or-the-other completeness.** With `smooth_motion` active the
following must all take their no-vector path, and the stats line must
say "motion: smooth_motion": the occlusion mask (off), the dither's
external vectors (own matcher), the grain map (per-frame scalar), the
residual stabilisation (this frame's own), the debander's hold (off), the
R-A/R-B rules (no field, no change), the LED look-ahead (unaffected — it
reads the queue, not the field). **[auditor]** verify each. Nothing in
libplacebo's smooth motion needs matching before 50/60p; the only
requirement is that the cadence rules choose one engine per profile and
never both.

**Q7 — crash state.** Keep: build and patch list, source format and
range, display refresh, renderer and GPU/driver versions, active
profiles and the settings fingerprint, de-judder/motion engine state,
presentation and stall counters, LED state (mode, last sent, port
open?), source-resolution reading, the 600-line ring, the last
exception and the minidump reference. **Remove or redact:** absolute
paths containing the Windows user name (`C:\Users\…` → `%USERPROFILE%`),
any remote-Config peer address, the COM port's *transcript* (keep the
last command and reply only), and nothing from the picture (no
statistics that could reconstruct content — APL and highlight values are
fine). **[auditor]** grep the writer for those four.

**Q8 — before roadmap item 6 (50/60p).** In order: (1) the pixel-identity
divergence — two identical runs differing is a determinism fault and it
would poison every byte-identical claim the project relies on; trace it
first; (2) the four "not yet" gate items on this range; (3) the one
flake (3/3 alone is a timing flake; fix the test, as with the Config RPC
one); (4) Q2's bounded disengage and Q4's structural fingerprint test;
(5) the `ycbcr_12bit_capture` control. Then 50/60p.
