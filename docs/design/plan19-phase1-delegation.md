# Plan 19 phase 1 — camera model core + store record: delegation plan (Opus planner, 2026-10-31)

Scope: implementation plan §6 steps 1–2 (data side only). Sources, in authority order:
`plan19-implementation-camera-model.md` (wins), `audit-plan19-global-camera-model.md`,
`docs/plans/2026-10-26/plan-global-camera-model.md` ("Refinements — IN THE FIRST BUILD"),
owner extras (tiny fast movers never camera; determinism; 8K-general; prior/filler never override).
Worktree `D:\codex\videoprocessor\feat-camera-model`, branch `feat/camera-model`. Team rules:
`D:\codex\vp-handover\TEAM-RULES.md` (build slots, /m:4, CL_MPCount=2, no GPU tests, no push).

OUT of scope (phase 2+): GPU texture/constant buffer, shader code, renderer wiring, §3 matcher
changes, §4 consumers, settings keys, fingerprints, status lines, host call sites.

## Placement decisions (planner)

- `docs/design/slots.json` is frozen (2026-10-24): NO new slot. The module is a sub-component of
  `motion.estimation`: folder `src/VP-V/modules/motion/estimation/global_model/` (precedent:
  `output/calibration_lut/calibration/`). Add a `README.md` there; list every new source file in
  `src/VP-V/modules/motion/estimation/MANIFEST.json` `live_files`; run
  `python tools/check_module_manifests.py --write` then without `--write` (must print 0 problems).
  No config keys are added in this phase (keys = step 5), so slots.json is untouched.
- Core stays independent of modules: `FrameAnalysisStore.h` forward-declares
  `namespace CameraModel { struct CameraMotion; }` and holds `std::shared_ptr<const CameraModel::CameraMotion>`.
- Every new file: `// Copyright(C) 2026 VP-V contributors` + `// SPDX-License-Identifier: GPL-3.0-only`.
  Pure C++14/17, no Windows headers in the module, no clocks except through the injected `Clock`.
- Tests: `src/VP-V/modules/motion/estimation/tests/CameraModel*Tests.cpp`, registered in
  `src/VideoProcessor-Test/VideoProcessor-Test.vcxproj.vcxproj` (+ `.filters`) with
  `<PrecompiledHeader>NotUsing</PrecompiledHeader>` and the Win32 exclusion like
  `EstimatorHookTests.cpp`; module `.cpp` files registered in the test project the same way
  (renderer projects do NOT compile them yet). Test names contain no "Gpu"/"Shader"; no
  category attribute (CPU by default); `python tools/check_test_categories.py` must pass.

## Files and public API (fixed by the planner — workers implement, do not redesign)

1. `CameraMotion.h` (header-only data): namespace `CameraModel`.
   - `enum class ModelType : uint8_t { None, Translation, Similarity, Affine, Homography };`
   - `enum class Label : uint8_t { Unknown = 0, Camera = 1, Object = 2, Overlay = 3 };`
   - `enum class Reason : uint8_t { Ok, NoInput, TooFewSamples, Cut, Flash, PostCutSeeded,
     FlashBridged, LowConfidence, Degenerate, Ambiguous, InverseMismatch, DeadlineMissed,
     GenerationReset, Disabled };` + `const char* ReasonText(Reason)`.
   - `struct Params { ModelType type; double h[9]; }` — 3×3 row-major mapping of a point in
     frame N−1 to frame N, in SOURCE ACTIVE-PICTURE coordinates with the origin at the picture
     centre, in source pixels, for the actual pair. Identity = no motion.
   - `struct SimilarityRates { double logScale, angle, tx, ty; }` per SECOND (time-normalised).
   - `struct Signals { double inlierShare, precision, coverage, temporal, conditioning,
     engineAgreement, inverse, ambiguity, logit; bool haveBackward, haveEngine; }` (each 0..1).
   - `struct ObjectModel { uint8_t id; ModelType type; double h[9]; uint32_t blocks; double cx, cy; }`.
   - `struct BlockData { int gridW, gridH; std::vector<uint8_t> label; std::vector<float> resX, resY,
     modelX, modelY; std::vector<uint8_t> objectId; }` (row-major, gridW*gridH; model vector =
     the model's prediction at the block centre over the pair, source px).
   - `struct CameraMotion { uint64_t seq, pairFrom, pairTo; double pairSeconds; uint64_t
     resyncGeneration, rebaseGeneration; bool valid; Params params; SimilarityRates rates;
     double confidence; Reason reason; Signals signals; bool seededFromNext, bridged, inFade,
     smoothed; Params smoothedParams; uint32_t samples, hypotheses; int64_t costMicros;
     std::vector<ObjectModel> objects; std::shared_ptr<const BlockData> blocks; Rect picture
     (own small struct); }`.
   - Inline helpers: `Apply(const Params&, x, y, &ox, &oy)`, `Displacement(...)`.
2. `GlobalModelGeometry.h/.cpp`: minimal solvers (1/2/3/4 points; DLT with Hartley normalisation,
   8×8 partial-pivot solve), weighted least squares per model type (fixed-order sums, double),
   `Invert`, `Compose`, `ToSimilarity` (decomposition → rates), `Interpolate(const Params&,
   double fraction)` = similarity: log-scale/angle/translation linear; affine/homography: the four
   picture-corner displacements scaled by `fraction` and refit exactly (audit must-change 4),
   `TimeScale(params, ratio)` (same rule, used for H0), degeneracy tests relative to the picture
   size (no pixel constants).
3. `GlobalModelFit.h/.cpp`: `struct PairInput` and `class Fitter`.
   - `PairInput`: `int gridW, gridH; double blockSize` (source px per block, from the caller);
     `Rect picture` (active picture; blocks whose centre lies outside = bars, excluded);
     `const float *fwdX, *fwdY, *fwdTrust` (trust 0..1, 0 = untrusted); optional
     `const float *bwdX, *bwdY, *bwdTrust`; optional `const uint8_t* textGuard` (per block, 1 =
     excluded); optional `const float* texture` (0 = flat … 1 = textured); optional
     `const float* pixelResidual` (per block: share of pixels whose post-camera-warp residual is
     beyond noise — produced by the GPU in phase 2, by `PixelResidualReference` on CPU now);
     `bool haveEngineGlobal; double engineGx, engineGy;` `bool cut, fade, flash;`
     `uint64_t seq, pairFrom, pairTo; double pairSeconds` (source-frame pair after cadence
     detection; ≤ 0 → NoInput); `uint64_t resyncGeneration, rebaseGeneration;`
     `double precisionPx` (expected vector precision; ≤ 0 → 0.25·blockSize/16 derived default);
     `int64_t deadlineMicros` (0 = none); `const PairInput* next` (queued next pair or null).
   - `struct Config` (all defaults conservative, every number relative or a count of
     hypotheses/IRLS steps): `ModelType maxModel = Homography; int targetCells = 1296` (48×27
     at 16:9, aspect-scaled); `int stage1Subset = 256; int stage2Best = 4; int maxHypotheses =
     300; int preemptiveBudget = 0` (0 = off; depth-0 bound); `int irlsIterations = 3; double
     relThreshold = 0.05` (share of predicted displacement); `double absThresholdSigmas = 3;
     double complexityPenaltyPerParam = 0.02; double quadrantMinShare = 0.05; double
     confidence coefficients b0..b8` (conservative: needs most signals high to exceed 0.5);
     `bool objects = false; int maxObjects = 3; double objectMinShare = 0.01; double
     overlayZeroClusterMin = 2` (4-neighbours); `double pixelResidualMinShare = 0.01; double
     flatTexture = 0.15; int smootherLag = 0` (0 = depth 0, causal); `Clock clock = nullptr`
     (`int64_t(*)()` microseconds; null = no deadline enforcement except tests' fake clock).
   - `class Fitter { explicit Fitter(Config); CameraMotion Fit(const PairInput&); void Reset();
     const CameraMotion* Previous() const; }` — keeps H0, the temporal filter and object ids.
   - Algorithm = implementation plan §1 steps 1–11 exactly, with these planner rulings:
     - RNG: splitmix64 seeded from `seq ^ (pairFrom * 0x9E3779B97F4A7C15)`; never std
       distributions; fixed loop orders; no unordered containers; no threads.
     - Sampling: cells aspect-scaled from `targetCells`; per cell the most trusted block (ties →
       lowest index); cell coverage map kept for coverage/quadrant tests.
     - Hypotheses: H0 = previous fit time-scaled by `pairSeconds/prev.pairSeconds` (skipped after
       cut/generation change), H1 = engine global translation, then PROSAC (trust-sorted pool
       grown progressively) minimal sets for every type ≤ maxModel.
     - Scoring: truncated quadratic, per-sample threshold `max(absThresholdSigmas·precisionPx,
       relThreshold·|predicted displacement|)`. Stage 1 on a fixed 256 subset, best 4 on all.
       Early exit when refined H0 beats the best alternative by a margin.
     - LO: MAGSAC++-style σ-marginalised weight `w(r) = erfc(r/(√2·σmax))` (w(0)=1, 0 beyond
       3σmax, σmax = the per-sample threshold), 3 IRLS steps, fixed reduction order.
     - Model choice: escalate only when relative weighted-cost drop > penalty·Δparams; homography
       only with inlier weight ≥ quadrantMinShare in ≥ 3 quadrants (affine ≥ 2).
     - Backward: fitted from inverse(H0 or forward fit); `inverse` signal from the forward∘backward
       deviation at the 4 corners + centre relative to the picture diagonal; absent → neutral 0.5
       and `haveBackward=false`.
     - Ambiguity signal: best disjoint competitor's support ÷ the winner's (object filling frame,
       crowds, parallax → confidence low).
     - Confidence = logistic(b·signals); cut/flash → 0; DeadlineMissed → `valid=false`.
     - Labels (prior/filler, never override): Overlay = bars, text guard, or a zero-motion cluster
       (≥ overlayZeroClusterMin zero neighbours) during a confident non-zero model; trusted
       blocks: Camera if residual ≤ threshold, else Object; untrusted blocks: Camera only if flat
       (texture ≤ flatTexture) or their measured vector agrees within the threshold, else Unknown;
       ANY block with `pixelResidual ≥ pixelResidualMinShare` → Object (tiny fast movers, ball /
       puck, never Camera); no morphological clean-up of isolated Object blocks. Residual and
       model vector stored for every block.
     - Scene: `cut` → record Reason Cut, no fit across it; if `next` is given, fit it with a full
       search (no H0) and publish it as this frame's seed (`seededFromNext`, Reason
       PostCutSeeded, temporal signal neutral); H0 for the following call = that seed. `flash` →
       Reason Flash, confidence 0, the temporal state is NOT updated; with `next`, a bridged model
       = interpolation of the pre-flash model and next's fit (`bridged`, Reason FlashBridged,
       confidence = min of both sides). `fade` → fit normally, `inFade = true`, never forced to 0.
     - Generations: a change of resync or re-base generation → Reset() (Reason GenerationReset on
       the first record only if no fit is possible).
4. `GlobalModelTemporal.h/.cpp`: `TemporalFilter` (constant-velocity filter on the similarity
   rates; process noise grows with the measured motion; used ONLY for H0 prediction and the
   temporal signal) and `FixedLagSmoother` (`Push(const CameraMotion&)`,
   `bool Smoothed(uint64_t seq, Params& out)`; symmetric weighted window ±lag over time-normalised
   similarity rates/corner displacements; never spans a cut, generation change or flash frame;
   lag 0 → current fit). Consumers use the smoother when available, else the current fit.
5. `GlobalModelObjects.h/.cpp`: sequential RANSAC for 1–3 object models (translation, then
   affine if it beats the penalty) on Object-labelled trusted blocks, min support =
   objectMinShare of picture blocks, compactness check, persistent ids by predicted-centre
   matching to the previous frame's objects. Only when `Config::objects`.
6. `PixelResidualReference.h/.cpp`: CPU reference (and shader-mirror spec) of the per-block
   tiny-mover measure: for each block, share of pixels with `|cur(x) − prev(H⁻¹x)|` (bilinear)
   > k·noiseRms; k and noiseRms are inputs (noise from FrameAnalysis grain RMS). Not called by the
   runtime path in this phase.
7. `CameraAccessor.h` (header-only, pure): `struct Policy { double threshold, floor; double
   blendWidth; double hysteresis; double rateLimit; bool enabled /*internal test switch*/;
   bool preferModelWhenAgrees; }`; `class BlendState` (per consumer: smooth confidence weight with
   hysteresis + rate limit; hard fallback below `floor`); `enum class Choice { Measured, Model,
   Blend, Hold }`; `Choice FieldVector(const Policy&, BlendState&, const CameraMotion*, int bx,
   int by, bool measuredTrusted, float mx, float my, float& ox, float& oy)` implementing plan §2
   steps 1–3. Policy disabled or record absent/invalid → returns exactly the measured/hold value
   (byte-identical). Plus `docs` section in README: the SHADER MIRROR SPEC (texture layout RG16F
   model vector + packed label/residual, constant buffer fields, the same branch order).
8. `src/VP-V/core/FrameAnalysisStore.h`: `kCamera = 1u << 9` group; `Record::camera`
   (`std::shared_ptr<const CameraModel::CameraMotion>`); `PublishCamera(seq, ptr)` refuses a
   record from an older epoch (like PublishMotion); `Invalidate` clears camera with motion.
   Nothing else changes (store off / group not expected → byte-identical behaviour).

## Subtasks (TEAM-RULES: one narrow job per fresh worker; self-contained briefs naming exact
files, commands and output format; results only, no file dumps)

| # | Model | Job | File scope | Acceptance |
|---|---|---|---|---|
| W1 | sonnet | items 1+2 (data header, geometry) + `CameraModelGeometryTests.cpp` | those files only | solvers exact on noiseless sets; Interpolate(0/1) exact; Invert∘Compose ≈ I |
| W2 | sonnet | item 3 (fitter) after W1 lands | `GlobalModelFit.*` | compiles; uses W1 API unchanged |
| W3 | sonnet | items 4, 5, 6 | `GlobalModelTemporal.*`, `GlobalModelObjects.*`, `PixelResidualReference.*` | unit tests in `CameraModelTemporalTests.cpp` |
| W4 | sonnet | items 7, 8 + `CameraAccessorTests.cpp` + store tests appended to a NEW `CameraModelStoreTests.cpp` (do not edit existing test files) | `CameraAccessor.h`, `FrameAnalysisStore.h` | off/absent byte-identical test; blend continuity; hysteresis |
| W5 | sonnet | synthetic field generator `SyntheticCameraFields.h` + `CameraModelFitTests.cpp` + `CameraModelCostTests.cpp` (after W2/W3) | tests only | every case in the list below |
| W6 | haiku | build + CPU test run + TRX summary (counts, failing names) | none (read-only) | report only |

The integrator owns: README, MANIFEST, vcxproj/.filters registration, the drift check, the
test-category check, fixing compile errors across workers, commits (one per logical step), and the
final check against this plan.

### Required synthetic tests (W5)
Known pan, zoom, rotation, perspective (parameter tolerances relative to picture size) + noise +
moving objects incl. a TINY FAST object (one or two blocks; and a sub-block ball through
`pixelResidual`) → those blocks Object, never Camera; flat sky (low texture, untrusted) → Camera
fill, confidence per coverage; cut (Reason Cut; post-cut seed with `next`); fade (fit kept valid);
flash (outlier, bridged with `next`, temporal state untouched); parallax (two depth planes) and
crowd → confidence < 0.5; object filling ~70% of frame → ambiguity → confidence < 0.5; static logo
over pan (with text guard: excluded/Overlay; without: zero cluster → Overlay); credits over static
picture (guarded tiles Overlay, model ≈ identity, confidence high); letterbox + pan (bars Overlay,
fit unaffected); handheld shake (each frame's own fit tracked, confidence stays ≥ 0.5); 3:2
source (same motion over different `pairSeconds` → equal rates; `pairSeconds ≤ 0` → NoInput);
forward/backward inverse (consistent → high; corrupted backward → lower); run-twice determinism
(bitwise equal records, incl. block data); generation change resets; deadline miss with a fake
clock → `valid=false`, Reason DeadlineMissed; 8K grid (480×270 blocks) works without constants.
Cost test: 240×135 (4K, 16 px) and 480×270 (8K) fields, typical (temporal pan, H0 exit) and worst
(cut → full search; homography + objects), median of N fits via `std::chrono::steady_clock`
(test-side only), printed with `Logger::WriteMessage`; asserts only a generous ceiling (worst
≤ 20 ms) so the CPU tier is not flaky; the numbers go in the report.

## Gates
1. Integrator self-check against this plan, then the planner audit (PASS/FAIL list).
2. Release x64 build green (build slot taken/released), CPU tier green
   (`python -I tools/gen_test_filters.py --cpu`, vstest `/Logger:trx`, read the TRX).
3. `python tools/check_module_manifests.py` and `python tools/check_test_categories.py` = 0 problems.

## Revision 1 (Opus planner, continuation, 2026-10-31) — binding additions

R1. PCH: test `.cpp` files `#include "pch.h"` first (like `EstimatorHookTests.cpp`) and are
    registered exactly like it (Win32 `ExcludedFromBuild`). MODULE `.cpp` files (no pch.h) are
    registered in the test project with per-file `<PrecompiledHeader>NotUsing</PrecompiledHeader>`
    and the same Win32 exclusion.
R2. Phase-2 optimisation API (data/CPU only, nothing wired) — new item 9, owned by W4:
    `GlobalModelSearchHints.h/.cpp`, namespace `CameraModel`:
    - `enum class SceneAction : uint8_t { Normal, SkipAcrossCut, SeedFromNextPair, BridgeFlash,
      FadeNormalised };` — `SkipAcrossCut` = matcher must not search across this pair;
      `SeedFromNextPair` = seed the new scene from the first post-cut pair in the queue;
      `BridgeFlash` = outlier frame, use the bridged model; `FadeNormalised` = use zero-mean
      (gain/offset-normalised) matching cost for this pair.
    - `enum class BlockHint : uint8_t { FullSearch = 0, NarrowAroundPrediction = 1,
      FinerGrid = 2, Excluded = 3 };`
    - `struct HintConfig { double minConfidence = 0.7; double agreeSigmas = 2.0;
      double narrowRadiusSigmas = 4.0; bool temporalSeed = true; bool tinyMoverRoi = true;
      bool fadeNormalised = true; bool cutSkip = true; bool flashBridge = true; }`
      (every switch defaults ON in this data API; phase 2 adds the user/internal keys).
    - `struct SearchHints { SceneAction scene; double fadeGain, fadeOffset; int gridW, gridH;
      std::vector<uint8_t> hint; std::vector<float> seedX, seedY; double narrowRadiusPx;
      struct Roi { int bx0, by0, bx1, by1; }; std::vector<Roi> rois; uint32_t narrowBlocks,
      fullBlocks, finerBlocks; }`.
    - `SearchHints BuildHints(const HintConfig&, const CameraMotion* prev, const float* prevFwdX,
      const float* prevFwdY /*previous measured vectors = per-block temporal seed, may be null*/,
      const float* nvofX, const float* nvofY /*may be null*/, bool cut, bool fade, bool flash,
      double fadeGain, double fadeOffset, double precisionPx);`
      Rules: block → `NarrowAroundPrediction` only if prev valid, confidence ≥ minConfidence,
      label Camera or Object-with-tracked-object-model, and the temporal seed (previous vector,
      else model vector) agrees with the NVOF seed within `agreeSigmas·precisionPx` (if NVOF is
      null, agreement with the model vector is required instead); otherwise `FullSearch`
      (ESCALATION: phase 2 re-runs a narrow block that fails verification as FullSearch).
      Blocks labelled Object whose `pixelResidual` flagged a tiny mover → `FinerGrid`, and
      4-connected groups of them become `rois` (bounding boxes, block units, deterministic
      raster order) for the NVOF ROI / finer grid. Overlay blocks from bars/text guard →
      `Excluded`. Scene action from the flags (cut beats flash beats fade). Radius =
      narrowRadiusSigmas·precisionPx (relative, no constants). Absent/invalid prev → all
      FullSearch (byte-identical to today's search).
    - Tests in `CameraAccessorTests.cpp` (or a new `CameraModelHintsTests.cpp`): pan → narrow
      majority; disagreeing NVOF block → FullSearch; tiny mover → FinerGrid + one ROI; cut →
      SkipAcrossCut; null prev → all FullSearch.
R3. `PairInput` also carries `double fadeGain = 1, fadeOffset = 0` (from the store's luma
    means; informational, copied into the record as `fadeGain/fadeOffset`) so the fit stays valid
    through fades and phase 2 can normalise matching.
R4. `BlockData` also carries `std::vector<uint8_t> tinyMover` (1 = pixelResidual flagged) so
    the hints and phase-2 ROI do not re-threshold.
R5. The planner (not the integrator) writes `docs/design/plan19-phase2-optimisations.md`.
