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
