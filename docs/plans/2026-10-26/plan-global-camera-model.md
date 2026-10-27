# Plan: robust global camera model with real-time confidence (2026-10-26)

Owner direction: a RANSAC-type camera model on top of VP's own motion engine (near-RAFT accuracy in real
time) is high value if cheap; it must assess its own correctness in real time and fall back gracefully.
Build early in the next round. RAFT stays the accuracy reference (it was the original reference in the
early motion modelling).

## 1. What it produces (per source frame)
A `CameraMotion` record stored with the frame (frame analysis store), stamped with the frame's resync and
re-base generations:
- model type and parameters: translation (2), similarity (4: pan, zoom, rotation), affine (6), homography
  (8) — the simplest model that explains the field;
- confidence 0–1 and its components (section 3);
- per-block label: camera / object / unknown, and the residual (object) vector per block;
- optional second model for one large independent object (crowd/vehicle), only if it clearly helps.

## 2. How it is computed (CPU, on the existing block vectors)
- Input: the engine's block vectors with their trust weights (NVOF seed + matcher verification), both
  directions where available; low latency uses backward vectors only. A 4K frame at the engine's block
  grid is a few thousand vectors — cheap.
- Robust fit: trust-weighted RANSAC (PROSAC-style ordering by trust), minimal sets of 1/2/3/4 vectors for
  the four models, ~100–300 iterations with early exit, inlier threshold scaled to the vectors' subpixel
  precision and frame size (no fixed pixel constants — 8K-general); refinement by iteratively reweighted
  least squares on the inliers.
- Model choice: escalate translation → similarity → affine → homography only when the inlier gain beats a
  complexity penalty (BIC-like); homography only with enough spatial coverage.
- Temporal model: a small filter over the parameters (predict from the previous frames, update with the
  new fit); reset on a RESYNC FULL stamp and on a re-base change of the matching axis; held across MINOR
  slips with the time gap corrected.
- Determinism: fixed random seed per frame (sequence-derived) so runs are repeatable.

## 3. Real-time correctness assessment (the confidence)
Combined from measurable signals, each normalised to 0–1:
1. Weighted inlier share (camera explains how much of the trusted field).
2. Residual size of the inliers vs the engine's expected precision.
3. Spatial coverage of the inliers (spread over the frame, not one corner/strip).
4. Temporal consistency: this frame's fit vs the filter's prediction (camera motion is smooth).
5. Conditioning: how well-determined the parameters are (degenerate geometry, low texture → low).
6. Agreement with the engine's own global estimate (sanity check, especially at start-up).
Scene cuts, fades and flashes (from the frame analysis store) force confidence to 0 until a fresh stable
fit. Thresholds are tuned on the corpus, not fixed by guesswork; every threshold crossing is visible
(diagnostics line: model, confidence, reason when below the use threshold).

## 4. Consumers and graceful fallback
Each consumer uses the model only above its own confidence threshold and otherwise behaves exactly as today
(block vectors + the current global estimate), visibly:
- de-judder and interpolation/hiccup repair (camera moves incl. zooms, the drift slip repair);
- vector trust and occlusion masks (camera vs object; parallax handled by falling back);
- motion estimate-ahead and the queue-continuum look-ahead features;
- sigma-delta / dither_vectors (dither pattern moving with the camera);
- Stage 8/8b grain map and the downscale residual stabilisation (correct motion compensation on zooms);
- detail pulse / texture restore stabilisation;
- fade-vs-pan check; DTM/LED look-ahead (camera-motion aware where useful).

## 5. Placement and modularity
- New sub-module `motion.global_model` (or inside `motion.vector_delivery`), slot-gated; off = byte-identical
  to today; Original pathway = off. Keys: `camera_model` (off/on, default on once it wins),
  `camera_model_max` (translation/similarity/affine/homography, default auto), per-consumer use switches
  only if testing shows they are needed (fewer pathways).
- VP logic in the module; host gets call sites only. Header 'VP-V contributors', GPL-3.0-only.

## 6. Tests
- Synthetic: frames warped by known pan/zoom/rotation/perspective (+ noise, + moving objects of known
  size) → parameters within tolerance, labels correct, confidence high; degenerate cases (flat sky, fades,
  cuts, parallax scenes, crowds, one object filling the frame) → confidence low and consumers fall back.
- Corpus scored against the RAFT reference: global-motion error, camera/object separation (precision/recall
  vs RAFT residuals), per-consumer gains (dither temporal stability, grain_keep, ringing on zooms, de-judder
  damaged tiles).
- Determinism run-twice; resync/re-base stamps reset it; queue depths 0–5; cost per frame on laptop and
  3090 (target well under 1 ms CPU at 4K).

## 7. Phases and size (agent wall-clock; sonnet drafts, Opus designs and audits)
1. Prototype + scoring harness on recorded vector fields (Python or C++ CPU), RAFT comparison — ~4–6 h.
2. Decision gate: does it win on the corpus at its cost? (owner sees the numbers)
3. Module implementation + confidence + diagnostics + tests — ~6–8 h, Opus.
4. Consumers wired one at a time, each measured and audited — ~1–2 h each.
5. Chain gate + nightly; defaults decided per consumer from the measurements.

## Planner audit 2026-10-26 — adopted (must-change before phase 3)
1. The model is a PRIOR and FILLER, never an override: trusted block vectors stay the truth; the model's
   predicted vector is used for untrusted/low-texture blocks and for camera/object labels. The PREVIOUS
   frame's model feeds E1 as the global candidate (replacing D1's search when confident), SAD-verified —
   no same-frame model↔vectors loop.
2. Exclude static-by-construction areas before fitting: the black-bar rectangle, blanked edges and the
   text-guard tiles (burned-in text, logos, credits). A dominant zero-motion cluster during a confident pan
   = overlay label, not camera; rolling credits over a static picture are not camera motion.
3. Parameters in SOURCE active-picture coordinates, per SOURCE-FRAME pair (after cadence detection), per
   unit time (pulldown and MINOR slips don't change them). Output-space consumers map per pixel through
   crop/re-base/NLS; never map the homography through NLS.
4. Intermediate times: interpolate similarity parameters (log-scale, angle, translation linearly); for
   affine/homography interpolate the four corner displacements and refit.
5. Consumers BLEND smoothly by confidence (hysteresis, rate limit), hard fallback only below a floor.
6. Calibrated confidence: a small logistic model fitted on the corpus predicting "block's model vector
   within tolerance of the reference" from the six signals + FORWARD/BACKWARD agreement (fits should be
   inverses); consumer thresholds are probabilities.
7. Temporal filter only for prediction/consistency, process noise grows with measured motion (handheld,
   whip pans); warps use the current frame's fit (no lag).
Should: inlier threshold relative to predicted magnitude (zoom edges); lens distortion/parallax = known
limits via lower confidence; second-object model deferred past phase 5; RAFT masked by its own fwd/bwd
consistency on real clips, true geometry on synthetic; decision gate in numbers (global error reduced by a
stated fraction vs today's D1/NVOF global, ≥1 consumer metric beyond noise, < 1 ms CPU at 4K on the laptop).
Wiring order: diagnostics only → Stage 8/8b grain map + residual stabilisation → detail pulse/texture
restore → sigma-delta dither_vectors → trust/occlusion filler → E1 global candidate → de-judder/hiccup
repair LAST (strictest threshold, hold policy stays). LED/DTM look-ahead dropped unless a concrete use.
Tests added: static logo over a pan; credits over a static picture; letterbox + pan; zoom edge vectors;
handheld shake; 3:2 source (no refresh pairs); NLS mapping for output consumers; blend continuity at the
threshold; forward/backward inverse; run-twice determinism with a fixed IRLS reduction order.

## Interaction with the nets (owner 2026-10-26)
Feeding the camera model into Stage 8/8b changes the hybrid downscale's output that net D (and B-refine
on 8b) were trained on. Rules: the nets' fingerprints include the grain-map/motion configuration (camera
model on/off, motion engine version) → a mismatch bypasses the net visibly; the study measures D with and
without the model; wire the model into Stage 8 BEFORE the round-3 D (and B-refine) training so the nets
train on their final input.

## Refinements to measure after the first build (owner questions 2026-10-27)
- Cuts with look-ahead: skip the matcher on the pair across a cut (its vectors are discarded by the reset);
  seed the new scene's model from the first post-cut pair already in the queue (no zero-confidence ramp).
- Fades: geometry is unchanged — when a fade is flagged ahead, match with brightness-normalised costs
  (zero-mean SAD / NCC, gain-offset compensation) and keep the camera fit valid through the fade.
- Flashes: treat the flash frame as an outlier; bridge the model from the frames either side.
- Independent motion: a per-block temporal seed (the block's previous vector) → narrow window when it agrees
  with the NVOF seed, escalating on failure, so moving objects get the saving too; sequential RANSAC for
  1-3 coherent object models and persistent object labels only if the corpus shows a gain.
