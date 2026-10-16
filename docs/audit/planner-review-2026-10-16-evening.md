# Planner review — 2026-10-16 evening (3cd4be16..3ae9aaf9)

(Saved verbatim as received from the owner.)

## Design and evidence, with the risks flagged

**Optical flow as the default motion source (seed mode).** Sound as
designed: the engine supplies the block vectors, the matcher keeps its
refinement, half-pel step and every trust test, and the engine alone is
not offered because it warps cuts. The numbers justify the default
(99.8–100% of the matcher on the corpus with the cut held; on real clips
41.9% vs 41.3% judder removed with *less* damage, 0.35 vs 0.40). Risks:
- A hardware/driver dependency the project did not have: NVIDIA-only,
  a driver minimum, a second D3D11 device and a worker thread. The
  fallback (miss → engine at render time → searches) keeps VP working
  without it, so this is a portability note, not a design problem; put
  the driver minimum and the "no NVOF → matcher" behaviour in
  BUILD-STATUS and on the stats line.
- The teardown fail-fast and the precedence bug (backward search run
  over the engine's flow) are exactly the class the chain tests exist
  for; both fixed, both need a regression.
- Queue depth 5 by default (4 pairs ahead): ≈208 ms at 24p, which is
  the owner's 200 ms target anyway; audio delay set once. Low_Latency
  exempt — correct.

**Source resolution.** The real-codec result (126 encodes, 0 native read
lower, player-upscaled 1080 224/224) is the one that matters; the
1080-master-coded-at-4K gap is a genuine limit (Q3) and errs the safe
way. Per-axis re-base, kernel-matched 2× inversion (+8 to +15 dB, never
worse), the F12/G-items closed: all accepted. One risk: a separable luma
re-base ringing 2.2 codes below black into the bars was caught — keep
polar luma for the default and make "no ringing into blanked bars" a
standing test for every re-base variant.

**Subtitles and layout.** The text guard is the right fix for a real
failure (a subtitled 1080p stream never declared, or thrown back by one
frame); see Q6 for its edges. Softened 4K text through the re-base is a
known cost the owner accepted; record it on the Picture page tooltip
("player subtitles are softened when a stream is re-based"). Layout
within 1 px / 0 px shift across scope, NLS and letterbox with blanking
is the invariant to keep.

**Evidence time 3 shots / 10 s at clarity 0.8**, with Cautious / Standard
/ Fast on the slider: acceptable given every false-alarm test passes at
10 s; Q7 lists the cases I would add before relying on Fast.

**Detail accumulation**: the prototype answered the question (the
B-frames' loss is correlated with their references'); park it (Q4).

**Not measured on the 3090**: everything in this range. That is the gate.

## Q1 — engine global flow for D1; engine cost into trust

- **Global:** use the engine's field (its robust median, or a 1/32-level
  average) as the *primary* global candidate and keep D1's 0.4 ms search
  as the fallback when the engine is absent or the pair missed. Do not
  remove D1; it is the no-NVOF path. Saving: the 0.4 ms whenever the
  engine ran.
- **Trust:** no. The engine's cost map is coarse and not comparable to
  the matcher's SAD, consistency and M6 tests, which are the project's
  defined trust. Use the engine cost only as a *prior for candidate
  ordering* (try the engine's vector first where its cost is low), never
  as an input to the trusted/held decision. That keeps "trust" meaning
  one measured thing.

## Q2 — kernel-matched re-base beyond 2×: wait for G19

Wait. The 2× case is identified by reprojection against interpolating
candidates with integer phases; 3:2 and per-axis have fractional phase
patterns that differ per kernel and the player's actual kernel is
unknown until the Apple TV captures exist. The generic polar re-base
already covers those ratios at today's quality. Build the 3:2 and
per-axis inversion once G19 shows which kernel the player uses; if it
is one kernel, the extension is small and certain rather than
speculative.

## Q3 — the 1080-master-coded-at-4K gap

Accept it as the known limit, with one experiment before closing it:
measure coherence on the **highest-quality frames only** — the I-frames,
identifiable from the bitrate-quality pulse — rather than time-averaged
over the GOP. The encoder removes the mirror signature most on the
heavily quantised P/B frames; the I-frames may retain enough of it.
If I-frame-only coherence does not separate either, the limit stands
and the tooltip says so ("4K encodes of upscaled masters read native").
Nothing else beyond spectral statistics is worth trying here: the
picture genuinely has 4K coding structure.

## Q4 — detail accumulation: park it

Agree. The correlated-loss finding is decisive for the v1 idea, and
"anchor to the nearest I-frame only" does not escape it (the I-frame's
loss is what the B-frames inherit). Targeting grain/noise is a different
thing — temporal noise averaging — which changes the look and is out of
scope. Retest only when real streaming captures (G19) exist, and only if
the I/B pulse is visible on the projector.

## Q5 — G1 periodic content: add the outright exclusion

Add a fourth guard: a tile whose autocorrelation has a peak above 0.9
at a lag shorter than a quarter of the tile is **exactly periodic** and
is excluded from the reading. Periodic content carries no reliable
coherence meaning either way, and the three soft guards can only argue
with it; exclusion costs nothing (a zone plate or a fence then simply
does not vote). Keep the three guards for the near-periodic cases.

## Q6 — text guard edges

The band rule is good for the common case; extend it in three ways and
protect against two:
- **Any band, either axis:** a quarter-height horizontal band anywhere
  (top-positioned subtitles, lower-third graphics) *or* a quarter-width
  vertical band (CJK vertical text, side captions).
- **Translucent boxes** are caught already — the box edges are sharp
  steps in the band; **drop-shadow-only** text is caught by the glyph
  edges themselves. Nothing to add.
- **Burned-in signs and small graphics**: below the 0.2% threshold they
  do not trigger, which is right — they are picture content.
- **Protect:** starfields and city lights are sharp points spread over
  the whole tile, not concentrated in a band, so the rule leaves them
  (confirm with a test). **Rolling credits** fill the tile with text and
  escape the band rule, so they read as native detail and throw the
  re-base off at once — acceptable at a film's end, and the tracker
  re-engages after the rule when the feature resumes. If a mid-film
  text-full frame (an on-screen letter, a title card) ever thrashes, add
  a whole-tile "text-like" test (many isolated sharp steps on a flat
  background) — but only on evidence; do not build it pre-emptively.

## Q7 — false-positive scenarios to test before relying on 10 s / 3 shots

Soft-focus native 4K with quick cutting is the right worry; coherence
needs mirror structure, which blur does not create, so these should
pass — but they must be *shown* to:
- a diffusion-filtered or soft-lens film (1970s stock, Pro-Mist) with
  three sub-3 s shots in a row;
- fog, smoke, underwater and heavy-rain scenes;
- night scenes dominated by bokeh and out-of-focus lights;
- a dream/flashback sequence with deliberate softness and quick cuts;
- heavy film grain on a soft image (grain is broadband; confirm the
  axis-agreement rule does not find spurious coherence);
- a 2.8K-sensor "4K" title: genuinely lower, so reading it lower is
  *correct* — include it to make sure the fixture distinguishes
  "soft" (native) from "upsampled" (lower).
Run all six at Standard and at Fast; Fast is only usable if they pass
there too.

## Before the deploy

The full suite re-run, the 3090 measurements (everything in this range
is laptop-only), regressions for the two optical-flow fixes, the
"no ringing into bars" re-base test, and the Q6/Q7 fixtures. Then the
gate, the audit, the folder.
