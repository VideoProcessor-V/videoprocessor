<!-- Copyright(C) 2026 VP-V contributors -->
<!-- SPDX-License-Identifier: GPL-3.0-only -->
# motion.estimation / global_model — global camera model (Plan 19, phase 1)

A sub-component of the `motion.estimation` slot (no new slot: `docs/design/slots.json` is frozen).
Phase 1 is the CPU data side only: the fitter, the per-pair record, the accessor policy and the
frame-analysis-store record. Nothing is wired into the renderer yet (no GPU texture, no shader, no
settings keys, no host call sites); the renderer projects do not compile these files, only the test
project does. Authority: `plan19-implementation-camera-model.md` sections 1-2, the audit, and
`docs/design/plan19-phase1-delegation.md`.

## Purpose
From the verified block-vector field of one source-frame pair (frame N-1 to N, after cadence
detection) fit ONE global camera model (translation, similarity, affine or homography), label every
block (Camera / Object / Overlay / Unknown) and give a calibrated confidence. Later phases use the
model as a prior and as a filler for blocks the measurement cannot trust (flat sky, periodic
texture), and to narrow the matcher's search.

## Files
| File | Role |
|---|---|
| `CameraMotion.h` | record + conventions (header-only) |
| `GlobalModelGeometry.h/.cpp` | minimal solvers, weighted LS, invert/compose, decomposition, interpolation, degeneracy tests |
| `GlobalModelFit.h/.cpp` | `PairInput`, `Config`, `Fitter` (plan section 1 steps 1-11) |
| `GlobalModelTemporal.h/.cpp` | `TemporalFilter` (prediction + consistency only), `FixedLagSmoother` |
| `DynamicTexture.h/.cpp` | `DetectDynamicTexture`: water-like whole-frame dynamic-texture detector (`Config::dynTexture`; Ambiguous + `verdictFlags`) |
| `StaticHypothesis.h/.cpp` | `EvaluateStaticCamera`: the competing static (identity) camera hypothesis on the border ring (`Config::staticCamera`; Ambiguous + `verdictFlags`, demotes only) |
| `GlobalModelObjects.h/.cpp` | 1-3 object models with persistent ids (only when `Config::objects`) |
| `PixelResidualReference.h/.cpp` | CPU reference / shader-mirror spec of the tiny-mover measure (not on the runtime path yet) |
| `PhotometricRefine.h/.cpp` | R3 direct photometric refinement of the chosen model on the luma pair (CPU reference; inverse-compositional Gauss-Newton + fade terms, inlier blocks, verified fall-back, counters). Not wired yet |
| `CameraAccessor.h` | per-consumer policy, blend state, `FieldVector` (pure, header-only) |
| `GlobalModelSearchHints.h/.cpp` | phase-2 matcher hints (data API only, nothing wired) |
| `GlobalModelMeasurement.h/.cpp` | phase-2 measurement plan: switches, `BuildMeasurementPlan`, verification / escalation, diagnostics, GPU guide texels |
| `ReferenceMatcher.h/.cpp` | CPU reference of the matcher semantics (tests and cost measurement only, not on the runtime path) |
| `CameraModelGpu.h/.cpp` | GPU label pass: constants, GLSL, CPU float mirror, `LabelPass` (libplacebo) |
| `CameraAccessorGlsl.h` | GLSL `camera_field_vector` + `AccessorConstants` |
| `CameraGuidanceGpu.h` | `GuidanceFrame` (plan -> matcher) + the guided pass GLSL (narrow window / E1 skip), wired in `matcher/DejudderWarp.cpp` |
| `CameraProducts.h` | camera link layer (K0): `CameraConsumer`, `CameraDemandInput`, `DeclareCameraConsumers` (registry consumers of P7 camera fit / P8 camera labels / P9 camera smoothed, ids 0x0200 + index) |
| `CameraReaders.h` | camera link layer (K0): one Indicator reader policy table (full/continuity/settings/graph Reset, timing Relatch, rebase Ignore), `RegisterCameraReader`, binders for `Fitter`, `TemporalFilter`, `BlendState`, `PairSpansSlip` |
| `CameraStoreAdapter.h` | camera link layer (K0): `CameraSealToStore`, the ONLY non-test caller of `PublishCamera` (a record the seal rejected is skipped and counted) |
| `../../core/FrameAnalysisStore.h` | `kCamera` group, `Record::camera`, `PublishCamera` |

## Inputs and outputs
Input (`PairInput`): the forward block field (vectors in source px + trust 0..1), optional backward
field, bar rectangle (active picture), text-guard tiles, optional texture / pixel-residual maps,
engine global translation, cut / fade / flash flags, fade gain/offset, source pair
(`seq`, `pairFrom`, `pairTo`, `pairSeconds`), resync / re-base generations, expected vector
precision, an optional deadline, and optionally the queued next pair.
Output (`CameraMotion`): validity and `Reason`, model type and parameters (time-normalised via
`rates` per second), confidence + its signals, per-block label / residual / model vector /
object id / tiny-mover flag, object models, cost.
Coordinates: source pixels, origin at the active-picture centre; `Params` maps frame N-1 to N.

## Determinism
Same input gives bitwise-equal records. Sampling seed = `seq ^ (pairFrom * 0x9E3779B97F4A7C15)`
(splitmix64; no `std` distributions), fixed loop and reduction orders, no unordered containers, no
threads, no clocks other than the injected `Config::clock` (cost only). All thresholds are relative to
the block size, picture size, expected precision or a hypothesis count: no hard-coded resolutions,
rates or cadences (tested from 1000x1000 up to 8K grids and with 1/24, 2.5/24 and 1/60 s pairs).

## Prior / filler never override
The model supplies a vector only where the measurement is untrusted (or, with
`preferModelWhenAgrees`, where the measurement already agrees with it within noise). A trusted
measured vector that disagrees with the model is kept, and its block is labelled Object. Any block
whose `pixelResidual` flags a tiny fast mover (ball, puck) is Object and `tinyMover = 1`, never
Camera. Object / Overlay / Unknown blocks never receive the model vector. Policy disabled, record
absent, invalid or without block data: the accessor returns exactly the caller's input
(byte-identical to not having the model). A store that never asks for `kCamera` behaves exactly as before.

## Scene handling
`cut`: Reason Cut, no fit across it; with `next`, the first post-cut pair is fitted with a full search
and published as this frame's seed (`seededFromNext`, PostCutSeeded). `flash`: Reason Flash,
confidence 0, temporal state not updated; with `next` a bridged model (FlashBridged). `fade`: fitted
normally (`inFade`), never forced to 0. A generation change resets the Fitter. A missed deadline
yields `valid = false`, Reason DeadlineMissed (a visible fallback; no state is updated).

## SHADER MIRROR SPEC (for phase 2; the CPU `FieldVector` in `CameraAccessor.h` is the reference)
GPU data (written once per source frame, block resolution):
* Texture `cameraModel`: RG16F = model vector (modelX, modelY) in source px at the block centre; a
  second texture (or the channels of an RGBA16F) holds label (uint 0..3 packed) and residual
  magnitude; same grid as the existing field texture.
* Constant buffer: `confidence`, `weight` (the consumer's `BlendState` output, computed on the CPU per
  frame), `preferModelWhenAgrees` (bool), `blockSize`, `enabled`, and the fit's own agreement threshold
  `absThresholdPx` and `relThreshold` (floats, copied from `BlockData`).
Branch order (identical to the CPU, float arithmetic only, no recursion or containers):
0. `!enabled`, no valid record, block outside the grid, `weight == 0` or label data missing ->
   return the caller's value unchanged (measured if trusted, else the caller's hold value).
1. measured vector trusted: with `preferModelWhenAgrees`, label Camera and
   `|res| <= max(absThresholdPx, relThreshold * |model|)` (the fit's stored threshold; no constants) ->
   `mix(measured, model, weight)`; otherwise measured.
2. not trusted and label Camera -> `mix(hold, model, weight)` (weight == 1 gives exactly the model).
3. else (Object / Overlay / Unknown) -> the caller's hold value.
Weight (CPU, once per frame per consumer): zero for Cut / Flash / DeadlineMissed / NoInput /
GenerationReset / Disabled / invalid; zero at once below `floor`; otherwise
`smoothstep((confidence - thr)/blendWidth)` with `thr = threshold - hysteresis` while the weight is
already above 0, moved by at most `rateLimit` per frame (no pop). Output-space mapping (crop, re-base,
NLS) happens after the choice, as today.
`PixelResidualReference` is the spec of the per-block share of pixels with
`|cur(x) - prev(H^-1 x)| > k * noiseRms` (bilinear previous-frame sample, clamp at borders).

## Record lifetime (hold-the-shared_ptr rule)
A consumer must keep its own copy of the `CameraMotion` record (or at least `record.blocks`, a
`shared_ptr<const BlockData>`) alive for as long as it calls `FieldVector` with it. The Fitter recycles a
pooled `BlockData` only when `use_count() == 1`, so a record that is held is never overwritten. Never keep a
raw `BlockData` pointer past the record. A `Fitter` is not thread-safe (one per motion worker); the records it
returns are immutable and may be shared across threads.

## GPU label pass (phase 2 prep)
`CameraModelGpu.h/.cpp`: `LabelConstants` (the constant buffer), the two GLSL passes (`LabelPassGlsl`,
`ZeroClusterGlsl`), a float CPU mirror (`LabelBlockMirror`, `ZeroClusterMirror`) that performs the shader's
arithmetic in the same order, and `LabelPass` (libplacebo dispatch). Output texture per block:
(modelX, modelY, |residual|, code) with code = label + 4 * tinyMover + 8 * zeroCandidate (small exact
integers). `Config::cpuLabels = false` (objects off) makes the fitter leave the per-block arrays empty and set
`CameraMotion::labelsOnGpu`; the accessor and `BuildHints` already treat short arrays as "no model" (caller's
value / full search). `CameraAccessorGlsl.h` is the GLSL port of `FieldVector` (`camera_field_vector`). The CPU
accessor and the shader compare the same float |residual| (`sqrtf(rx*rx + ry*ry)`).
Policies: `Policy::curve` (Smoothstep / Linear / Step) and `PolicyFor(Consumer)` presets (all `enabled = false`).

## Search hints (phase 2 data API)
`BuildHints(..., precisionPx, nextPairSeconds)`: seeds (model and previous forward vectors) are scaled by
`nextPairSeconds / prev->pairSeconds` for uneven cadences (3:2); `<= 0` means no scaling. Only bar and
text-guard blocks (`BlockData::excluded`) are `Excluded`; a static-logo Overlay stays searchable
(`FullSearch`).

## Measurement plan (phase 2)
`BuildMeasurementPlan(switches, config, input)` turns the PREVIOUS pair's causal record (never the queued smoother),
the scene flags and the switches into a per-frame, per-block plan for the matcher. All switches default off; no
switch (or an unusable record) gives an inactive / empty plan and today's search, byte for byte. Records after a cut,
a flash, a resync, a missed deadline, with low confidence or with labels only on the GPU give no model-driven saving and
name the reason in `fallbackReason` (status line). Vectors are source px over the pair about to be searched, scaled by
`nextPairSeconds / prev->pairSeconds`. `ReferenceMatcher` is the CPU mirror used by `CameraMeasurementTests`
(`CAMERA_MEAS_COST ...` lines).

| Switch | Saves | Escalation / fallback |
|---|---|---|
| `modelGlobal` | D1's global search: the model's centre displacement (fwd and bwd) is the global start | no usable record: today's D1 |
| `guidedCandidate` | the model prediction is the first candidate of the full search and of the second sweep | none needed (an extra candidate only) |
| `narrowWindow` | camera blocks searched +-r (>= `narrowMinRadiusPx`) around the prediction | block fails the SAD tolerance: full search in the same frame (`ApplyVerification`) |
| `e1Skip` | second sweep skipped where the first-sweep vector agrees with the prediction | disagreement: the sweep runs as today (counted escalated) |
| `staticGate` | E4 from the model: confident, `Ok`, all four picture corners within `staticSigmas` precision | otherwise the field is searched |
| `cutSkip` | no search across a cut; `seedFromNext` when a queued pair exists | switch off: normal search, no guidance from the old scene |
| `fadeNormalised` | zero-mean cost for the narrow pass in a fade | failing blocks fall back to today's plain-SAD search |
| `flashBridge` | the flash pair is not searched; a bridged record stays usable but never seeds temporally | none |
| `temporalSeed` | previous forward vector as the seed when it agrees with NVOF | disagreement: full search; bridged record: model seed |
| `tinyMoverRoi` | tiny movers get `FinerGrid` (never narrowed); ROIs limited to `roiBudgetShare` of the grid | over budget: raster-order `FullSearch`, `roiOverBudget` counted |
| `objects` | object blocks with a tracked model narrowed around the OBJECT model (seed and agreement reference) | switch off: those blocks are `FullSearch` |

## Matcher guidance on the GPU (phase 2)
`MakeGuidanceFrame(plan)` -> `Dejudder::MotionWarp::SetCameraGuidance()` before the pair's `Estimate` (cleared by
each Estimate; a low-resolution inner receives it with its own pixel scale). Null or inactive: nothing is bound or
dispatched, the matcher's shaders preprocess to today's text (`CAMERA_KEEP` / `CAMERA_STATIC` undefined).
* `useModelGlobal`: D1's texel = the model's centre translation (1/8 texels, z 0.5, w 1); the D1 search does not run;
  M2's second candidate is cleared.
* `staticScene` (only where E4's gate runs): `CAMERA_STATIC` lets `static_scene()` fire when the previous mean |v| is
  below `motion_lo` (the model replaces D1's static test, not E4's own history test).
* `skipMatcher` (cut / flash): the searches are written untrusted (error 1e4) and D1 / M2 cleared: the consistency
  pass and the frame trust hold the pair as a failed match does; the temporal history restarts.
* narrow: the guided pass (before the block search) searches +-R whole px (R = ceil(radius x scale)) around the seed,
  then half-pel; accepted (sad <= tolerance) blocks are kept by the block search (`CAMERA_KEEP`, as E4 keeps the
  previous vectors), every other block runs today's search in the same dispatch (the escalation). Backward: the
  forward seeds negated (verification decides). Not used when the engine seeds or replaces the searches.
* E1 skip: the guided pass checks the first sweep (or the engine's seed) against the prediction (within `e1AgreePx`)
  and its sad; accepted blocks skip the second sweep, the rest run it.
* Fade: `CG_FADE` = zero-mean cost in the guided pass; full searches keep M4.
Diagnostics: `MotionWarp::CameraGuidanceUsed()` (per Estimate) + `MeasurementDiagnostics::StatusLine()` (plan counters).
GPU follow-ups (not in this phase): the guided candidate inside the full search / second sweep candidate lists, tiny
movers forcing the E2 split / an NVOF ROI, the guidance texture built on the GPU from the label pass (then
`cpuLabels = false` is possible with guidance on).

## Cost
Per-step cost is profiled by a test-only harness: building the tests with `CAMERA_MODEL_PROFILE` defined
makes `GlobalModelFit.cpp` accumulate per-step milliseconds in `CameraModel::g_profileMs[8]` (field,
hypotheses, local optimisation + ladder, signals, ambiguity, backward, temporal, labels). The macro is not
defined in the product build. The early-exit path keeps H0's type, tries one step up, and (when the type is still H0's and above Translation) one step down from the same weighted set: the lower type wins when the higher one does not earn its extra parameters (same complexity-penalty test), so a model that was a homography relaxes by one type per pair when the motion becomes a pure pan.

The fit runs on a CPU worker, off the render path (render-path cost: zero). Two full-grid passes are
inherent on the CPU: the stratified field scan (before the fit) and the per-block model/residual/label
pass (after it). On a 4K grid (240x135 blocks) each costs about as much as a plain memory pass over the
vector field, so the label pass is the largest step of a typical (early-exit) pair. Phase 2 can move the
per-block label pass to the GPU (it is a natural pass over the vector texture), leaving the CPU with the
field scan and the fit on ~1300 samples. Buffers: the per-block record (`BlockData`) is recycled from a
small pool when no record still holds it, and the per-pair scratch is reused, so steady state allocates
nothing large (no first-touch page faults).

## Robustness (2026-11-01)
On real sequences (crowd_run under a pan, ~half the vectors on movers) the RANSAC fit was right but the confidence
could never reach 0.5. Three `Config` fixes (the fitter still has no runtime caller, so no released behaviour changes):
1. `inlierShareLo / Hi` = 0.25 / 0.55 (phase 1: 0.4 / 0.9). The inlier-share signal asked that the camera explain
   most vectors; with many movers that is "no dominant motion" whatever the margin. Dominance against a competing
   motion is the ambiguity signal's job.
2. `temporalNoiseFloor` = true. `TemporalFilter::Consistency / Update` take an optional `floorSigma[4]` (one vector
   precision per component: translation `precision / dt`, scale and angle at a quarter of the picture diagonal). Null
   = phase-1 relative-only noise, under which near-zero components (scale, angle, a slow pan) made the signal ~0.
3. `verdictGatesConfidence` = true. An Ambiguous / InverseMismatch record's confidence is multiplied by
   `0.5 * clamp(signal / gate)`, so a failed verdict is never usable (< 0.5).
Phase-1 behaviour = `{inlierShareLo 0.4, inlierShareHi 0.9, temporalNoiseFloor false, verdictGatesConfidence false}`
(`camera_eval --phase1`). Tests: `CrowdUnderPanIsDominant` (also asserts the phase-1 failure), `NoDominantMotionRandomField`,
`TwoLayerHalfAndHalfIsAmbiguous`, `ObjectOverBackgroundSixtyForty`, `NewDefaultsAreDeterministic`, and the temporal
noise-floor tests.

Measured with `tools/camera_eval` (`run_eval.py --match`; usable = valid and confidence >= 0.5; real truth = master
DIS/NVOF fits that agree, region metric described in `tools/camera_eval/xcheck_real.py`):

| | phase 1 | new |
|---|---|---|
| synthetic usable: crowd_pan / crowd_shake / crowd_static | 0 / 0 / 0 | 1.00 / 0.31 / 1.00 |
| synthetic WRONG-confident (9 scenarios, 39 pairs each) | 0 | 0 |
| real usable, mean over tiers and sources: crowd / tos / sol / boat | 0.00 / 0.26 / 0.20 / 0.35 | 0.91 / 0.98 / 0.81 / 0.59 |
| real WRONG-confident, region metric (confident pairs judged; p90 block error over the true-camera blocks > max(1 px, 10 % of the p90 truth magnitude); all tiers incl. master) | 5 of 459 | 16 of 1857 (crowd 2/515, boat 14/323, sol 0, tos 0); over the fit's own camera labels 4 and 13; the crowd tiers plus boat/sol/tos `_low` alone: 5 of 741 vs 1 of 103 |
| real WRONG-confident, extrapolated corner error (includes non-camera regions; mostly measures affine / homography extrapolation into runners and other movers) | 19 of 409 | 114 of 1660 |
| matcher evaluations guided / full (coded `*_1080p_low` NVOF, 40 pairs): crowd / tos / sol / boat | 1.00 / 0.98 / 1.00 / 0.88 | 0.68 / 0.39 / 0.85 / 0.84 |
| matcher EPE vs reference (all blocks), guided vs full | unchanged | unchanged or slightly better |

Tools and metric definitions: `tools/camera_eval/README.md`.

Known limitation: a coherent object covering about 50 % of the picture, over a background of incoherent or garbage vectors,
is indistinguishable from a pan behind a crowd. It is fitted as the dominant motion, by definition. The ambiguity signal only
catches a coherent competitor with at least about a third of the camera's support (test
`CoherentObjectOverRandomBackgroundIsFittedAsDominant`).

## Tests
`../tests/Camera*Tests.cpp`, CPU tier, all synthetic: geometry, temporal / objects / residual,
accessor + hints, store record, fitter behaviour (pan, zoom, rotation, perspective, noise, objects,
tiny movers, sky, cut, fade, flash, parallax, crowds, logos, credits, letterbox, shake, pulldown,
inverse, determinism, generations, deadline, 8K) and a cost report (`CAMERA_COST ...` lines).

## Two-layer (K10a)
`Config::twoLayer` (default **off**; off = records byte-identical to the single-layer fitter). A large foreground that
fills the middle of the picture (a boat, a vehicle, a tracked subject) can out-vote the background and be reported as a
confident camera. With `twoLayer` the best competing motion found by the ambiguity search is kept as a second layer and
the camera layer is the one with the best `support x continuity x border evidence x area history`:
- **Border evidence**: share of a layer's inlier weight inside a ring of `borderRingFraction` (12.5 %) of the active
  picture, divided by the ring's share of all samples (~1 = background-like, ~0 = centred foreground); factor 0.25..1.
- **Continuity**: closeness (corner displacement) of the layer to the last usable camera layer, time-scaled to the pair;
  factor `continuityFloor`..1. **Area history**: factor 0.5..1 from the change of the layer's inlier share.
- With two layers the ambiguity signal is the **score ratio** (same 0.35 gate): a near tie stays Ambiguous (visible,
  never used). With one layer that touches no border (ratio < `borderVetoRatio`), explains < 75 % of the vectors and has
  no agreeing continuity reference, the record is Ambiguous with `ForegroundSuspect`.
- **Hysteresis**: when the new winner is not continuous with the reference but the reference layer is still present, the
  reference layer is kept for `M-1` pairs (`M = max(1, round(layerSwapSeconds / pairSeconds))`, derived from the measured
  pair duration), then the change is accepted (`Swapped`, `layerRisk` 1 decaying linearly to 0 over `M` pairs, confidence
  scaled by `1 - 0.5*layerRisk`). If the reference layer has vanished, the new layer is accepted at once.
- Record fields: `layerFlags` (1 ran, 2 competitor, 4 camera is the RANSAC runner-up, 8 foreground suspect, 16 swap
  pending, 32 swapped, 64 continuity used), `objectLayer` (the other layer; persistent id 1..255), `layerRisk`, and the
  `Signals::border` / `continuity` diagnostics. State (reference, object id, hold count) resets with the fitter, on a cut
  and on a generation reset; a flash leaves it untouched. Evaluation: `tools/camera_eval` (`--two-layer`).

Default stays **off**: `twoLayer` can create new usable verdicts (a foreground-suspect layer becomes the camera), so the
acceptance bar (no new confident-wrong) is not met; the opus gate rejected the default flip. It is a tuning/evaluation option
until the keys exist (K3).

## Dynamic-texture detector (R1-GATES)
`Config::dynTexture` (default **off**; off = state never touched, record byte-identical, whatever `dynTextureCfg` holds).
Water, foliage and similar textures give block vectors that are valid-looking but different in every pair, so a clean
anchor set can still make the fit "usable". `DynamicTexture.h/.cpp` takes the share of trusted blocks that disagree with the
fitted model, the **temporal** incoherence of that disagreeing field against the previous pair's residuals, and the
**spatial** incoherence between neighbouring disagreeing blocks. When share >= `minShare` (0.5), temporal >= `minTemporal`
(0.6, only when a previous field exists) and spatial >= `minSpatial` (0.5) the record becomes `Reason::Ambiguous` with
`verdictFlags |= kVerdictDynamicTexture`, confidence x 0.495 (below 0.5), the status text names a dynamic texture, and the
pair does not feed the camera prediction. Never usable by a consumer (visible fallback). History resets with the fitter, on a
cut, on stale input, on a flash and on any **invalid fit**. Defaults are set so that no real pair of the camera_eval clips the
fitter calls usable is turned Ambiguous. Signals: `dynShare`, `dynTemporal`, `dynSpatial`, `dynField`. Evaluation:
`camera_eval fit --dyn-texture`.

## Static-camera hypothesis (R1-STATIC)
`Config::staticCamera` (default **off**; off = nothing evaluated, record byte-identical, whatever `staticCfg` holds). A coherent
foreground object (a boat over water) can be fitted as the camera. After the fit, `StaticHypothesis.h/.cpp` scores two models on the
**border ring** of the active picture (width `ringFraction` = 1/8 of each dimension, min 1 block; the part of the picture where a
foreground object rarely sits and a real camera motion moves every block): the identity (static) model and the fitted model. A
trusted ring block is explained by the static model when `|v| <= absSigmas * precision` and by the fit when `|v - model| <=
max(absSigmas * precision, rel * |model|)` (the fit's own rule). The fit is demoted when the fitted model really moves the ring (more
than half of the ring blocks have a model displacement above the static threshold, `requireMotion`) AND the static model explains at
least `minStaticShare` (0.4) of the trusted ring blocks AND at least (the fit's ring share - `margin` (0.2)). Demoted = `Reason::Ambiguous`, `verdictFlags |=
kVerdictStaticCamera`, confidence x 0.495, status text "ambiguous (static background explains the motion better: foreground
object)", and the pair does not feed the camera prediction. **It only demotes**: only records whose reason is Ok or LowConfidence are
judged, no record becomes usable, and an already Ambiguous record is never touched. Cost: one pass over the ring blocks (about 0.1 ms
at the 120 x 68 grid, 0.4 ms at 240 x 135, one core). Signals: `staticRing`, `staticFit`. Evaluation: `camera_eval fit --static-camera
[--sc-share F --sc-margin F --sc-ring F]`, `run_eval.py --vs static` (second column = hypothesis on).

Result on the real clips (R1-STATIC, `tools/camera_eval`, truth pinned to the default fits; WRONG = region metric at max(1 px, 10 %), in
brackets the pairs beyond the 1.5 px noise tier). The base fitter's own ambiguity signal already refuses every synthetic case where the
static background is a coherent second motion, so the hypothesis only acts in the band where the fit is confident but the ring is
mostly (not coherently) still, which is what the boat clip does:

| setting (minStaticShare / margin) | boat usable | boat WRONG (>1.5 px) | crowd usable | sol | tos | synthetic WRONG |
|---|---|---|---|---|---|---|
| off (base) | 0.59 | 14 of 323 (2) | 0.91 | 0.81 | 0.98 | 0 |
| 0.5 / 0.0 | 0.59 | 14 (2) | 0.90 | 0.81 | 0.98 | 0 |
| 0.4 / 0.1 | 0.59 | 14 (2) | 0.90 | 0.81 | 0.98 | 0 |
| 0.4 / 0.2 (shipped values) | 0.57 | 9 of 317 (0) | 0.90 | 0.81 | 0.98 | 0 |
| 0.4 / 0.3 | 0.51 | 7 of 288 (0) | 0.90 | 0.81 | 0.98 | 0 |

A demoted fit still seeds the next pair's prediction state (`m_previous`), exactly like a dynamic-texture demotion (accepted; it never feeds the temporal filter). The boat gate (confident-wrong <= 1 %, that is <= 3 of 323) is **not met** (9 of 317, 2.8 %): the remaining wrong pairs are near-threshold misses
(1.0 to 1.3 px error against a 1 px floor, true camera motion not zero) whose ring statistics are the same as the correct pairs
(static share 0.44 vs 0.45, fit share 0.72 vs 0.82), so no ring statistic separates them. The two pairs beyond the 1.5 px tier are removed. Hence the
default stays **off**: the hypothesis is an option with visible status, not a shipped default.

## Noise-tier metric (camera_eval)
The real-clip region metric is reported at two error floors: `WRONG@true-cam` (max(1 px, 10 %)) and `>1.5 px@true-cam`
(max(1.5 px, 10 %), `xcheck_real.NOISE_TIER_PX`). A pair wrong only between 1 and 1.5 px is a near-threshold miss (vector noise
against a 1 px metric), not a foreground lock; a confident pair beyond 1.5 px is a real error. Acceptance uses both columns.
See `tools/camera_eval/README.md`.

## Link layer (K1 / K2 / K4, module side)
`link/` holds the fit runner (P7 producer, worker thread, readback ring, pool), the P8 producer / fill / per-consumer binding and
the guided-matching scheduler, governor and `p7g_camera_guide` product. All of it is inert until the H1 adapter registers it; see
`link/README.md` (wiring table, fill rule, private-history justification). The verdict gate (D-K4) is `CameraModel::ReasonUsable`:
one set shared by `BlendState::ForcesZero` and the guidance gate (Ok, PostCutSeeded, FlashBridged); Ambiguous, InverseMismatch,
Degenerate, TooFewSamples and LowConfidence records are published for diagnostics and never used.

## Photometric refinement (R3)
Doc 103 s3 R3: the only model source independent of the NVOF field. `PhotometricRefiner::Refine(model, LumaPairView, picture,
InlierMask)` polishes the vector fit's chosen model on the PIXELS of the pair and returns `RefineResult` (model, gain/offset,
mean Huber cost before / after on the same pixels and sigma, correction in source px, iterations, pixels, status).

- Inputs: the producer's engine luma of frames N-1 (template) and N, 8-bit (`HalfLuma8` / quarter buffer) or R16 (P1 half
  luma, `codeScale` 255/65535); `sourcePxPerPixel` 2 (half) or 4 (quarter); the record's `BlockData` labels via `MaskOf`
  (Camera and not excluded only; an empty label array = no mask, `maskUsed` false).
- Resolution per case (this CPU reference): 1080 sources refine on the 8-bit half luma (960x540, s = 2); 4K sources on
  the 8-bit QUARTER luma (960x540, s = 4) or, in the R16 path, the P1 half luma (1920x1080, s = 2); always on a subset of
  at most `targetPixels` (50k) strongest-gradient pixels.
- Resolution decision (OWNER, 2026-11-11 about 15:30, supersedes coordinator call R3-RES 14:58): the refinement resolution
  is a MEASURED choice, not fixed at native. The GPU form is measured at quarter, half and full resolution (bias and EPE
  on the acceptance rows, ms per resolution) and uses the CHEAPEST resolution that meets the accuracy target (<= 0.02
  source px at 1080, <= 0.04 at 4K); full only if measurably better (doc 91 D tail). It becomes a named setting whose
  default is that cheapest passing resolution. Owner's reason: the quarter-size reference already reaches about 0.02 px
  at 4K; the vector-fit bias comes from NVOF's quantised vectors, not from the resolution. This CPU reference's half /
  quarter input is therefore within the decision, recorded here for traceability (an audit had flagged the earlier text
  "never shrinks the input" as inaccurate).
- Method: inverse-compositional Gauss-Newton (template gradients and Jacobian once), Translation models 2 parameters,
  every other type a 4-parameter similarity correction composed onto the model (`W(p) <- W(p) o W(d)^-1`, type kept),
  plus photometric gain/offset (`RefineOptions::fadeTerms`). Pixels: the `targetPixels` (50k) strongest-gradient inlier pixels,
  chosen by a gradient-bin threshold with ties in raster order (deterministic). Huber weights (1.345 robust sigma,
  sigma = 1.4826 median |r|, floor 0.5 code), Keys bicubic sampling of frame N without clamping (a tap outside the plane
  drops the pixel), at most `maxIterations` (4) steps, stop when no corner moves more than 0.001 engine px.
  `RefineResult::lastStepPx` reports the last step (source px); the tests require it below the bias target (0.02 source
  px at 1080, 0.04 at 4K), so a run that ends on the iteration cap has stopped moving at the accuracy that matters.
  Most test runs end on the cap (4): the last step is 0.0003-0.004 source px, just above the 0.001 engine-px stop rule,
  which sits at the step noise of Huber re-weighting on 8-bit codes; that motion is 10-100x below the bias target.
- Real content (`RealTextureKnownShift`, opt-in by `VP_R3_REAL_GRAY` / `_W` / `_H`): one real luma frame cropped at
  integer source-px offsets and box-downscaled, so the truth is exact and the phase sub-pixel. 2026-11-11: Meridian 4K
  s 2 bias -0.003 / -0.002 (max 0.004), s 4 bias -0.001 / +0.013 (max 0.045); Tears of Steel 1920x800 s 2 bias 0.0007 /
  0.0002 (max 0.001) source px. Real clips have no camera truth finer than about 0.5 px (tools/camera_eval).
- Verified fall-back: the input model (gain 1, offset 0) is returned unless the final mean Huber cost is lower than the
  input model's on the same pixels by `minImprovement` (1e-6 relative); also on a correction above `maxCorrectionLumaPx`
  (1 engine px, the sub-pixel basin), gain outside [0.5, 2], or a degenerate system. The status says which
  (`RefineStatusText`).
- Counters (`PhotometricRefiner::Counters()`): refined / fell back / skipped no luma / skipped other. They are the
  visible R3 status lines once wired (not wired yet: the fit worker holds no luma; the camfix integration plumbs the
  producer's luma pair to the worker and adds the status line).
- Determinism: single thread, fixed order, double accumulation, no clocks; the scratch buffers are cost only. Byte-equal
  repeat runs are a test.
- Cost (CPU, laptop, `PhotometricRefineTests::CostLine`, median of 5): see the TRX `cost ...` lines; recorded in the
  commit and the lane hand-back.
- GPU form (doc 103 s5 R3-GPU stage 1; doc 91 section D and its line 99 apply as stated above, nothing new decided here):
  `PhotometricRefinerGpu` (`PhotometricRefineGpu.h/.cpp`, kernels in `PhotometricRefineGpuShaders.inl`) runs the same
  algorithm as D3D11 cs_5_0 compute on the producer worker's own device (`MotionAheadProducer.cpp` CreateDevice): candidate
  pass + 4096-bin gradient histogram, threshold bin with raster-order ties via a row prefix, then per Gauss-Newton pass the
  residuals, an exact radix-select median of |r|, the Huber normal equations as a fixed-order tree reduction and a
  one-thread solve / inverse-compositional update, the verification sums last. All passes are dispatched up front (passes
  after convergence are no-ops): one submission, ONE readback, collectable later (`Submit` / `Collect(wait = false)`).
  Doubles use SM 5.0 base ops only (no 11.1 double extensions: division / reciprocal / square root by Newton steps from
  float); the final status decisions run on the CPU with the reference's own geometry functions. The CPU reference stays
  the definition: `PhotometricRefineGpuTests` runs every CPU case on both and requires the same status, candidates and
  pixels, the model within 1e-4 engine px at the corners / centre, gain / offset within 1e-4, and byte-equal GPU repeats
  (device: `VP_TEST_ADAPTER` substring, else WARP; a named adapter that is absent fails). Input is one interface
  (`RefineLumaInput`); stage 1 has the CPU-upload path only (`RefineUploadInput`: the `HalfLuma8` / quarter 8-bit buffer,
  or full-resolution 10-bit codes from `AnalysisLumaSource` as R16, codeScale 255/1023); the twin / fence inputs are a
  later swap behind it. Limits: luma at most 8191 x 2304 (else `SkippedNoLuma` with `LastError`). Not wired: no setting,
  no status line. Cost at Q / H / F for 1080 and 4K: `GpuCostLine` (opt-in `VP_R3_GPU_COST=1`, run under GPU-QUIET).

INPUT SCAN (rules 24/25). Read before writing: `PixelResidualReference.h/.cpp` (per-block residual shares from float luma
under a frozen shader-mirror spec; measures, never refines: extended would disturb that spec, so a new unit), `GlobalModelFit.h`
(`PairInput`, `Config`, `Fitter`, `BlockData` use; warm start and stop rule), `GlobalModelGeometry.h/.cpp` (reused: `Compose`,
`Invert`, `MaxCornerDifference`, `IsDegenerate`, `SplitMix64`), `CameraMotion.h` (conventions, `Apply`, `Rect`, labels),
`MotionAheadProducer.cpp` (`HalfLuma8` 2x2 box and the quarter 2x2 box of it: the pixel-centre convention used here),
`link/AheadFlow.h` (`AheadCopy` carries no luma: the plumbing is open), `core/Products.h` P1 (R16 half luma), `CameraProducts.h`
(P7/P8/P9 consumers). No existing photometric / direct alignment code in the estimation module; the matcher's sub-pixel
refine works per block on vectors, not on the global model.

## Settings

The fitter's own options are `Config` fields set by the tests and `tools/camera_eval`. Camera K3 added the `[vpv.camera]` keys (switches only, all off; table in `../README.md` Settings, parser `link/CameraSettings.h`). Config placement for the keys that exist (`camera_link`, `analysis_lane`): see `control/skeleton/README.md`. "Original (Bill)" = the Original pathway builds no camera link (`camera_link` off).

| Key | Section | Type | Range / values | Default | Config placement | Original (Bill) | Fallbacks / status | Tiers |
|---|---|---|---|---|---|---|---|---|
| `camera_link` (see `link/README.md`) | `[vpv.skeleton]` | choice | off, report | off | none yet (master VP-V page, Pipeline) | off | `camera_link` entry (cannot report); status line | none |
| `Config::twoLayer` (internal; no key) | internal | bool | false, true | false | none (internal) | not built | foreground-suspect records are `Ambiguous` (`ForegroundSuspect`) in the camera status text; never usable | same on every tier |
| `Config::dynTexture` (internal; no key) | internal | bool | false, true | false | none (internal) | not built | `Ambiguous` with the dynamic-texture reason text; never usable (visible fallback) | same on every tier |
| `Config::staticCamera` (internal; no key) | internal | bool | false, true | false | none (internal) | not built | `Ambiguous` with the static-background reason text (foreground object); never usable (visible fallback) | same on every tier |
| `Config::staticCfg.minStaticShare` | internal | float | 0..1 | 0.4 | none (internal) | not built | none | same on every tier |
| `Config::staticCfg.margin` | internal | float | 0..1 | 0.2 | none (internal) | not built | none | same on every tier |
| `Config::staticCfg.ringFraction` | internal | float | 0..0.5 | 0.125 | none (internal) | not built | none | same on every tier |
| `Config::staticCfg.minRingBlocks` | internal | int | >= 1 | 48 | none (internal) | not built | fewer trusted ring blocks: no verdict, record unchanged | same on every tier |
| `Config::staticCfg.absSigmas` | internal | float | > 0 | 3.0 | none (internal) | not built | none | same on every tier |
| `Config::staticCfg.rel` | internal | float | 0..1 | 0.05 | none (internal) | not built | none | same on every tier |
| `Config::staticCfg.requireMotion` | internal | bool | false, true | true | none (internal) | not built | none | same on every tier |
| `Config::dynTextureCfg` (minShare, minTemporal, minSpatial, agreeRatio, absSigmas, rel, minBlocks, minCompared) | internal | numbers | 0.5 / 0.6 / 0.5 / 0.5 / 3 / 0.05 / 64 / 48 | as listed | none (internal) | not built | none | none |

| `RefineOptions` (R3; internal; no key) | internal | struct | see `PhotometricRefine.h` | as listed there | none (internal) | not built | fall-back returns the input model, status + counters | same on every tier |

Tiers: the fit is cheap CPU work and is not tiered (cost lines `CAMERA_COST`).

**Downstream consumers (TEMPORAL DETAIL, docs 71 section Q5 and 72):** per-pair homography (P7/P9), the object layer (per-object motion), P8 per-block membership + confidence, and K4 guided residuals.
