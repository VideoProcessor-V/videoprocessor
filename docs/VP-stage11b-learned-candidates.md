# Stage 11b — learned motion candidates (post-cut; recorded now)

> **REJECTED by the owner 2026-10-17:** neural/learned methods are rejected for VP's processing
> (they invent detail and are poor value for the cost), including learned motion estimators
> (RIFE/RAFT class) as a primary path. A neural flow would only be reconsidered as a supplement if
> real footage shows an occlusion shortfall. Motion comes from the NVIDIA optical-flow engine with
> VP's matcher as fallback (docs/VP-nvof-ahead.md). The "owner-approved direction" below dates from
> 2026-10-13 and is superseded; the text is kept for the record.

2026-10-13. Owner-approved direction. Not for the cut: comes after the
projector has confirmed the current field's limits. Two parts: a motion
corpus with ground truth (the prerequisite and the training set), and a
small learned estimator that feeds *candidates* into the existing
matcher — an add-on, never a replacement.

## 1. Why an add-on

The matcher does two jobs: it finds vectors and it verifies them (SAD
against tolerance, forward/backward consistency, trust, hysteresis).
Learned flow is excellent at finding — smooth, dense, strong on
repetitive texture and thin structures where SAD search fails — and
poor at knowing when it is wrong: it produces a confident, plausible
vector where there is no true match. So the model feeds the candidate
list (as D1's global vector and E1's neighbours do) and the existing
verification decides. A wrong model output fails verification and the
block falls back to today's candidates or to hold: the field can only
get better, never worse than today. Everything downstream — warps,
occlusion mask, grain map, dither carry (R1) — is untouched.

Setting: `dejudder_estimator: block | block+learned`; `block` is the
regression reference.

## 2. The corpus (first; also the regression set for everything else)

- **Synthetic, exact ground truth:** procedurally generated cases —
  pans, fences and grilles, repetitive texture, thin moving objects,
  occlusion/reveal, rotation, zoom, lighting change, grain, subtitles
  over motion — each with the true field and the true intermediate
  frame. Hundreds of case types, thousands of instances; extends the
  GeForce motion probe.
- **Real, ground truth by frame dropping:** 60p/120p sources (the
  owner's own 60p captures, public high-frame-rate footage), alternate
  frames dropped, synthesised, scored against the originals. Supplies
  real grain, compression, motion blur, camera motion.
- **Scoring:** per-block vector error, per-pixel error of the synthesised
  frame against truth, held fraction, and a perceptual metric
  (LPIPS-class) beside the pixel metrics; the worst N cases exported as
  images for the eye.
- **Use before any training:** run the corpus on the current estimator
  (with 11a) to get the failure map: which classes are parameter
  problems (fit them automatically against the corpus) and which are
  design limits (the ones 11b exists for).
- **Outcome, 2026-10-13 (first failure map; 1ccfd7d1, d63186ba):**
  34 synthetic scenes x 18 settings (match tolerance x E1 lambda x matcher
  size), D2/D4 off. Wrongly trusted blocks <= 1.6% everywhere. Headroom
  (fence and cut excluded): best single fixed setting +8.2% (match
  tolerance 4.5), best per scene +8.7%. **Step 9 (a motion-adaptive or
  learned controller of the existing settings) is ruled out: about 0.5%
  over the best fixed setting.** The fixed-settings A/B (step 7) is the
  lever. Design limits found: search range (UVG Jockey, 300-800 px per
  film frame, whole-frame hold), whole-frame hold on hard motion,
  periodic texture, brightness fades. They go to VP's own matcher first
  (wider top-level search, more global candidates, regional hold, fence
  disambiguation, fade-gated mean-removed matching; planner answers
  docs/VP-review-2026-10-13-night-answers.md Q8-Q11). The learned
  candidate finder (step 10) stays open pending those fixes.
- **Not to reopen (planner D4, 2026-10-14):** fence period detection is
  dropped. The only remaining fence case after M1-M6 is a one-period alias
  (a fence moved by exactly one bar spacing), which renders identically to the
  true motion, so there is nothing visible to fix.
- **Fade test fix (2026-10-14, baeb5e13):** a fade must scale the picture
  about a plausible black level (B ~ black + g x (A - black), g = the spread
  ratio); a pan moves the mean as content enters and leaves but keeps g ~ 1,
  and fails. Found by the Beta 6d check (the defaults were 18.8 from the
  halfway picture against 13.3 without refinements; 2.88 after the fix).

## 3. The model

- Small flow network at 1/8 or 1/4 resolution, upsampled to block
  centres; outputs a vector and a confidence per block. Inference via
  the fork's bundled onnxruntime-DirectML (or a hand shader if the
  model is small enough); a few ms per source frame at 4K on the 3090.
- Loss weighted for this pipeline: heavy on pans and texture, light
  inside regions that would be held anyway, and a trust head trained
  to predict where the *warp* would be visibly wrong (from the corpus
  scores), not where the flow is uncertain in the abstract.
- Integration: the model's vector enters E1's candidate list with its
  confidence as a prior on the smoothness penalty; the block search
  evaluates it within a small range and verifies as today. The
  consistency check still runs on the verified field.
- Determinism: fixed model file in the repo (versioned, in
  BUILD-STATUS), fixed inference options; a corpus run before and after
  any model change.

## 3b. Model variants (owner's request)

The soap-opera look is a property of the **strength policy** (how far
frames are moved toward their true time positions), not of the
estimator, so "a model that prevents it" is a different, smaller model
from the candidate finder. Three variants:

1. **Candidates, 24p film** (the default above): loss weighted for
   quarter-displacement warps with hold; ground truth from the corpus.
2. **Candidates, even cadence at full strength** (delta 10's 30p case):
   same architecture, loss weighted for full-displacement error
   everywhere, since the extras are full warps there. Ground truth from
   the corpus.
3. **Strength model — withdrawn (see `VP-delta-14-soe-rules.md`):** the
   owner is not sold on a rated-clip taste model; two hand rules (a
   high-speed roll-off and a camera/object split) take its place. Kept
   here for the record only. It would have learned a per-**frame** `s` from what the frame
   contains (pan speed, contrast, motion magnitude — how visible the
   cadence hitch would be against how visible smoothing would be). The
   hand-made version is D2. Ground truth is the **owner's judgement**:
   short clips rated "hitch visible / looks like video / right", a few
   hundred ratings. It is a taste model, not an engineering one, and it
   is the only variant that bears on the soap-opera look. It stays per
   frame: the global-`s` rule holds, and no model is allowed to set
   regional strength (that is where the floaty look comes from).

Selection: variants 1 and 2 by the cadence rules (as the strengths are
today); variant 3 by `dejudder_strength_model: off | on`, replacing D2's
ramp when on, with D2 as the fallback and reference.

## 4. What it is not

- Not a replacement for the matcher, not a learned *synthesis* (no
  interpolation network — the warp, hold and mask stay as they are).
- Not SVP: it targets the quarter-displacement, hold-fallback design,
  which is why a small model suffices.

## Order (post-cut)

Corpus → failure map on the current estimator → auto-fit of 11a's
parameters → only then the model, if the failure map shows design
limits worth it. Each step its own package and audit.

## 2026-10-14 afternoon: D3 measured, the landing check (owner)

Real HFR clips, D2/D4 off, 32x32 tiles damaged when the warp is 6 codes worse than the hold:

| Variant | All removed | All damaged | Film-like removed | Film-like damaged | Extreme damaged |
|---|---|---|---|---|---|
| Defaults (M6) | 41.9% | 0.97% | 31.7% | 0.39% | 1.99% |
| **M6 + landing check (1.5 px; new default)** | **41.3%** | **0.30%** | **31.3%** | **0.13%** | **0.61%** |
| D3 blend | 50.2% | 2.29% | 40.8% | 0.78% | 4.92% |
| D3 + landing check A and B | 49.8% | 1.74% | 40.3% | 0.59% | 3.75% |

- The landing check: a source pixel M6 looks up counts only if its own motion carries it back to the output position
  (within 1.5 source px; 3 px measures the same). Backward warping otherwise takes valid pixels that are not the ones
  visible at an occlusion. Synthetic set unchanged (1472). Key `dejudder_landing_check` (default on), both warps.
- D3 stays off (test-only). Its damage persisted through four gates (sample agreement, motion agreement, fill only, B's
  own blocks only). RAFT comparison on the worst frames: B's trusted motion agrees with RAFT to about 1 px; A's trusted
  motion is the confidently wrong one (Jockey: 73% of A's trusted blocks in the damaged tiles off by more than 4 px).
- RAFT ceiling confirmed: full precision changes nothing (50.4 / 55.8%); quarter vs half size on the same frames
  53.3 / 57.4% vs 53.5 / 57.8% (4K does not fit: 62 GB correlation volume).
- Next candidate (A alone): a match-certainty test (a flat error curve around the best offset = not trusted).
