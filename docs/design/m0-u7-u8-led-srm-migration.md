# M0 Design Note: Shared U7 / U8 / P4 & P2/P3 Reader Contracts (LED, SRM, DetailPulse, SRD)

**Track:** Wave B2 — Package O (Shared-Unit Reader Migration)
**Status:** Active design contract for `O#2..O#6` and forward contract for `O#7..O#8`
**Governing Rules:** `C:\dev\SUBAGENT-RULES.md` (Rules 1–8), zero invented constants, zero silent fallbacks, strict C++14, Bill-file TU neutrality.

---

## 1. Shared Producer & Reader Contracts

Every consumer migrated under Package O reads shared products through the skeleton/shared-unit read surfaces rather than maintaining private duplicate judges or statistics.

### 1.1 U7 — `CutFadeJudge` (v5) & `SceneRecordReader`
- **Owner:** `src/VP-V/modules/input/source_analysis/events/shared/CutFadeJudge.h` (`VpUnit::CutFadeJudge`, `kUnitVersion = 5`), published into `FrameAnalysis::Store` and served get-or-compute by `SkeletonModule::SceneOf` (`Vpv::Skeleton::IExternalDemand::SceneOf`).
- **Reader facade (`src/VP-V/modules/control/skeleton/SceneRecordReader.h`):**
  - `SceneVerdict { bool cut; bool fade; float fadeKeep; }` (`fadeKeep < 0` when not provided).
  - `CutOf(seq, cut)` — peek-only store read of delivered cut (existing content-intent path).
  - `SceneOf(seq, SceneVerdict& out)` — peek-only store read (`SingleStore()->PeekScene(seq, scene)`) returning `cut`, `fade`, and `fadeKeep`.
  - `AcquireScene(seq, SceneVerdict& out, const char** why)` — get-or-compute reader under the single-owner contract (`owners.size() == 1`, never guessed; implemented out-of-line in `SceneRecordReader.cpp`), obeying `MUST-9`: first performs a non-mutating store peek (`SourceFrameTap::StoreFor(owners[0])->PeekScene(seq, scene)`, safe on any thread); on a store miss, checks `MemoFor(owners[0], seq)` and refuses off-render-thread live fills (`"scene not published (off render thread)"`), released frames (`Released(seq) || Released(seq - 1)` -> `"frame released"`), and ahead-of-seal frames outside the queue window / ahead compute / ahead plan (`NotQueued` / `NotPlannedAhead` / `PresentsStalled`) before delegating to `IExternalDemand::SceneOf(owners[0], seq)`, so `SkeletonMemo::Fail()` is never polluted. When no memo exists (e.g. lightweight mock `IExternalDemand`), delegates directly to `SceneOf`. On any unavailability, returns `false` with exact static `*why` and neutral `out`.
  - `AcquireCut(seq, cut, why)` & `CutOf(seq, cut)` — keep their original inline bodies in `SceneRecordReader.h` for exact Bill-TU line parity (`0` header delta), while `SceneOf`, `AcquireScene`, and `AcquireSourceStats` live out-of-line in `SceneRecordReader.cpp`.
- **Shared blank/black predicate (`CutFadeJudge::BlankOf`):**
  - `BlankVerdict { bool blank; bool black; bool flat; }` derived strictly from existing U7/P4 contract constants (Rule 1, zero invented numbers):
    - `rec.pixelCount == 0` -> `{ false, false, false }`.
    - `spread = rec.p99 > rec.p01 ? rec.p99 - rec.p01 : 0`.
    - `flat = (spread <= kBlackSpreadP4)` (`256` P4 codes = 1 8-bit code); if `!flat && rec.gridFound != 0`, `flat = Flatness(GridOf(rec)) < kFlatF` (`0.02`).
    - `black = flat && rec.mean <= kBlackMeanMaxP4` (`12288` P4 codes = 48 8-bit codes).
    - `blank = flat` (no detail: black, fade dark end, or flat frame).

### 1.2 U8 — `PictureRect` (v1)
- **Owner:** `src/VP-V/modules/input/source_analysis/picture/shared/PictureRect.h` (`VpUnit::PictureRect`, `kUnitVersion = 1`).
- **Contract:** Bill's active-picture rectangle when provided (`In::given`), else near-black bar bounds on the 1/8 luma (`blackCode + barCodes`), else whole frame `(0, 0, frameW, frameH)`. Readers query `Effective`, `InPicture`, `Coverage`, `Halved`, or GLSL `vpu_in_picture` / `vpu_picture_coverage`.

### 1.3 P4 — `SourceStats` (v2)
- **Owner:** `src/VP-V/modules/input/source_analysis/lane/shared/SourceStats.h` (`VpUnit::SourceStats`, `kUnitVersion = 2`, product `VpSkeleton::kP4SourceStats`).
- **Scale:** 16-bit Q6 analysis axis (`Vpv::AnalysisDomain::kAxisQ6Max = 65472`, 10-bit whole axis code `<< 6`).
- **Shared helpers:**
  - `FirstBinReaching<Acc>(hist, bins, target)` — shared percentile bin-search law (reused by both source-domain P4 and display-domain `SourceStatsDisplay`).
  - `NormalisedMean(const Record& rec)` — `rec.pixelCount > 0 ? min(1.0, double(rec.mean) / double(Vpv::AnalysisDomain::kAxisQ6Max)) : 0.0`.
- **Reader facade (`SceneRecordReader::AcquireSourceStats`, out-of-line in `SceneRecordReader.cpp`):**
  - Single-owner check (`StoreOwners().size() == 1`), then obeys `MUST-9` without mutating `SkeletonMemo::lastCause_`:
    1. Non-mutating lane-lease peek via `IExternalDemand::LeaseFor(owners[0], kP4SourceStats)` + `ReadPublishedBytes` (any thread).
    2. Non-mutating memo peek via `IExternalDemand::MemoFor(owners[0], seq)` + `CopyPublished` (accepting both `kRecordBytes` and `sizeof(Record)`, any thread).
    3. Guarded render-thread `Acquire`: checks `HasOwnerFill` (`"no analysis lane"`), `OnRenderThread` (`"p4 not published (off render thread)"`), `Released(seq)` (`"frame released"`), and non-mutating ahead-of-seal queue-window / ahead-plan admission (`NotQueued` / `NotPlannedAhead` / `PresentsStalled`) before calling `memo->Acquire(kP4SourceStats, seq, AcquireForm::Cpu)` -> `SourceStats::ReadRecord`.
  - Every failure returns `false` with exact static `*why` (`"no store owner"`, `"renderer ambiguous"`, `"no skeleton module"`, `"no frame memo"`, `"no analysis lane"`, `"p4 not published (off render thread)"`, `VpSkeleton::Token(ar.cause)`, or `"p4 record version mismatch"`).

### 1.4 P2 / P3 — `EngineFieldFacts` & `MotionValidity`
- **Owner:** `src/VP-V/modules/control/skeleton/SkeletonFacts.h` (`Vpv::Skeleton::EngineFieldFacts`, published alongside `kP2VectorPair` / `kP2mMatcherPair` / `kP3AnalysisField`) and `MotionValidity` (`StampAt`, discontinuity epoch / `PairSpansSlip`).
- **Contract:** Motion consumers read verified pair facts (`pairFrom`, `pairTo`, `globalValid`, `globalX`, `globalY`, `stamp`) through the skeleton / validity facade and reject pairs whose stamp was invalidated by a resync event, with visible fallback when no field is published (`p2Published` / `p3Published` false).

---

## 2. Active Migration Contracts (`O#2..O#6`)

### 2.1 `O#2` (`L1`) — LED Fade Migration to Shared U7 `SceneOf`
- **Files:** `SceneRecordReader.h`, `FrameStats.h`, `LedMeasurement.{h,cpp}`, `LedController.cpp`.
- **`FrameStats`:** `fade` carries the shared U7 fade verdict; `fadeKeep` (`float`, default `-1.0f` = not provided) carries U7 v3 partial-reset keep factor.
- **`StatsBuilder`:**
  - Deletes private `aplHistory` and the `fadeMs` / `fadeMinSlope` APL-decline loop completely.
  - Adds `LedDrive::SceneVerdict { bool cut; bool fade; float fadeKeep; }` and `SetSceneVerdictReader(SceneVerdictFn)` while keeping `SetSceneReader(SceneCutFn)` 100% source- and ABI-compatible for `LedDriveModule` and `LedLookahead`.
  - Resolution priority in `StatsBuilder::Build`: (1) `sceneVerdictReader` if non-empty, (2) `sceneReader` if non-empty (`fade = false`, `fadeKeep = -1.0f`), (3) `VpSkeleton::SceneRecordReader::AcquireScene(sequence, verdict, &why)`.
  - On `sequence == 0` or reader returning `false`: `cut = false`, `fade = false`, `fadeKeep = -1.0f`, `sceneKnown = false`, `sceneWhy = why`, `++sceneMissing` (honest visible fallback; zero private fade fallback).
- **Fade-in direction safety (`CompensateController::Tick` in `LedController.cpp`):**
  - U7 `stats.fade` fires for both fade-out (darkening) and fade-in (brightening).
  - Derive `const bool fadeToBlack = stats.fade && !(previousApl >= 0.0 && stats.apl > previousApl);`.
  - Only `fadeToBlack` targets `ledFloorBlack`, guards black prepositioning, engages fade-rate dimming (`level > target`), sets `out.holdGain`, and logs the dimming fade state. A brightening fade-in (`stats.apl > previousApl`) follows the normal curve target and never clamps to `ledFloorBlack` or holds gain.

### 2.2 `O#3` (`L2a`) — Display-Domain P4 CPU Variant (`SourceStatsDisplay.h`)
- **Files:** New `src/VP-V/modules/drive/led_measure/SourceStatsDisplay.h` (pure header-only C++14, zero Windows/GPU headers), `LedMeasurement.cpp`, `drive/led_measure/MANIFEST.json`, `VideoProcessor-Lib.items.props`.
- **Canonical identity (`LedDrive::SourceStatsDisplay`):**
  - `kUnitVersion = 1`, `kDisplayCodeUnit = 8` (ITU-R BT.709 limited-range 8-bit code units: `16 + 219 * v`), `kLimitedBlack = 16.0`, `kLimitedSpan = 219.0`, `kBlockSize = 3`, `kDefaultMaxBlocksX = 320`, `kDefaultMaxBlocksY = 180`.
  - `PreOutputStage()` = `VpSkeleton::VariantKey::StageAfter(uint8_t(PipelineSlots::Id::ColourAppearance))` (the `PRE_OUTPUT` stage immediately preceding `drive.led_measure`).
  - `VariantKeyFor(stage, precision)` returns canonical `VpSkeleton::VariantKey` with `codeUnit = kDisplayCodeUnit`, `stage`, `precision` (default `kPrecisionFloat32`).
- **Laws:**
  - `DisplayPercentile(histogram, total, percentile)` reuses `VpUnit::SourceStats::FirstBinReaching<double>` (U6 shared law), preserving exact boundary parity (`total == 0 -> 16.0`, `above >= total -> 0.0`, strict `counted > above` upper-tail threshold mapped to cumulative `total - above`).
  - `SummaryFromRaw(raw, highlightPercentile, scriptHighlightCode)` is the single CPU law turning `RawMeasurement` into `DisplayRecord` (`apl`, `highlight`, `highlightArea`, `maximum`, `p01`, `p50`, `p99`, `blocks`, `raw`).
  - `MeasureDisplayFrame(width, height, sampleRgb, params)` is the exact CPU twin of `LedMeasurementHook`'s `PRE_OUTPUT` 3x3 block pass (+ `AreaAveragedMaximum` when `peakFullCoverage`), applying clamp `[0, 1]`, optional `pow(rgb, boost)`, `dot(rgb, weights)`, `code = 16 + 219 * clamp(mean, 0, 1)`, `scaled = uint32(code * 16 + 0.5)`, `PixelRect::Contains(originX + 1.5, originY + 1.5)` masking, then `SummaryFromRaw`.
- **Delegation & TU hygiene:** `LedMeasurement.cpp` (only `.cpp`, not `.h`) includes `SourceStatsDisplay.h` and delegates `HistogramPercentile` and `StatsBuilder::Build` summary calculation to it.

### 2.3 `O#4` (`L2b`) — GPU Measured Form as Stage Variant (`LedMeasurementHook`)
- **Files:** `LedMeasurementHook.h`, `LedMeasurementHook.cpp`.
- **Contract:**
  - `enum class MeasurementStageVariant : uint8_t { PreOutput = 0 };` (maps to `PL_HOOK_PRE_OUTPUT`).
  - `explicit MeasurementHook(MeasurementStageVariant stageVariant)` (default ctor delegates to `PreOutput`, 100% backward-compatible).
  - Out-of-line accessors: `StageVariant()`, `HookStagesMask()`, `MatchesStage(uint64_t stage)`, `VariantKey()` (returns `SourceStatsDisplay::VariantKeyFor(PreOutputStage(), kPrecisionFloat32)`), `StageMismatches()`, and `CollectDisplay(std::vector<SourceStatsDisplay::DisplayRecord>&, ...)`.
  - Stage guard in `Measure(params)`: if `params->stage != 0 && (uint64_t(params->stage) & hook.stages) == 0`, increment `stageMismatches`, log once visibly via `DebugLog::Log`, and return `PL_HOOK_SIG_NONE` (never silently measure a wrong pipeline stage).
  - Compute shaders and `RawMeasurement` layout remain bit-exact (preserving golden digest `0x6fe339a231a79bc2ull`).

### 2.4 `O#5` (`M1–M4`) — SRM Blank / Cut / Fade / Mean Migration
- **Files:** `CutFadeJudge.h`, `SourceStats.h`, `SceneRecordReader.h`, `SourceResolutionMonitor.{h,cpp}`.
- **Shared inputs & injectable seams:**
  - `SharedSceneInput { bool cut; bool fade; float fadeKeep; }`, `SharedP4Input { bool blank; bool black; bool flat; double mean; }`.
  - `SetSharedSceneReader(SharedSceneFn)` / `SetSharedP4Reader(SharedP4Fn)` (empty = default `SceneRecordReader::AcquireScene` / `SceneRecordReader::AcquireSourceStats` + `CutFadeJudge::BlankOf` + `SourceStats::NormalisedMean`).
- **Look-ahead (`LookAheadFrameCoded`, `M2`):**
  - Queries shared U7 scene for `cut` and shared P4 for `blank`/`black`/`mean`.
  - When shared U7 is available, uses shared `cut` directly (skipping private `frameJudge`/`aheadCutJudge`/`|mean - aheadLastMean|`) and increments `u7SceneShared`.
  - When shared P4 is available, uses `fixes ? p4.black : p4.blank` directly and increments `p4StatsShared`.
  - When unavailable, falls back visibly (`++u7SceneFallback` / `++p4StatsFallback`, exact `why`) to the local check.
- **Worker analysis (`Analyse`, `M3`):**
  - When `j.sequence != 0`: queries shared U7 (`cut`, `fade`) and shared P4 (`BlankOf`, `NormalisedMean`).
  - Shared U7 available: feeds shared `cut` into `cutFlag`, populates `snapshot.frameFade` and CSV fade column (replacing the `0` placeholder), increments `u7SceneShared`. Unavailable: keeps tracker cut delta, `frameFade = false`, increments `u7SceneFallback` with exact `u7SceneFallbackWhy`.
  - Shared P4 available: replaces private tile mean `frame.mean` for `SourceIdentity::FrameInput::apl` and CSV/diag mean; combines `!frame.informative() || p4.blank` for `frameBlank`/`in.blank`/`Blank` event and `p4.black` for `frameBlack`; increments `p4StatsShared`. Unavailable: keeps tile mean/blank/black, increments `p4StatsFallback` with exact `p4StatsFallbackWhy`.
  - `Describe()` appends visible fallback counts & causes whenever `u7SceneFallback > 0` or `p4StatsFallback > 0`.

### 2.5 `O#6` (`SRM-FRAMEBOUND-WAIT`) — Wait-Miss Diagnostics & Deterministic-Only Ready Wait
- **Files:** `SourceResolutionMonitor.h` (forward-declaring `namespace VpSkeleton { class AnalysisLane; }` to keep Bill TU lines unchanged), `SourceResolutionMonitor.cpp` (including `AnalysisLane.h` and `SealPoint.h`).
- **Exact per-miss cause classification:**
  - `enum class WaitMissCause : uint8_t { None = 0, LatePoll, DropOrBacklog, BoundedWaitTimeout };` + `WaitMissCauseText(cause)`.
  - `Snapshot` exposes `lastWaitMissCause`, `lastWaitMissFramesLate`, `lastWaitMissAnalyseMs`, and per-cause counters `waitMissLatePoll`, `waitMissDropOrBacklog`, `waitMissTimeout`.
  - `Worker()` stamps `Job::hadBacklog = !jobQueue.empty()` at pop time.
  - In `Analyse(Job& j)`, when frame-bound `j.sequence != 0 && j.sequence <= releasedThrough`: classifies `DropOrBacklog` if `j.hadBacklog || jobsDropped > 0 || heldDropped > 0 || submitSkipped > 0`, else `LatePoll`, computing `framesLate = rel - j.sequence + 1` and recording `snapshot.analyseMs`.
  - `NoteWaitMissed()` remains backward-compatible and delegates to `NoteWaitMissed(WaitMissCause::BoundedWaitTimeout, 0)`.
  - `Describe()` (feeding Ctrl+I and Ctrl+Alt+D) appends exact per-miss breakdown whenever `s.frameBound && s.waitsMissed != 0`.
- **Deterministic-only bounded ready wait:**
  - `BindLaneReadyProbe(AnalysisLane&)` wires `lane.SetReadyProbe([this](uint64_t s){ return ReadyFor(s); }, [this](){ NoteWaitMissed(WaitMissCause::BoundedWaitTimeout, 0); })`; `UnbindLaneReadyProbe(AnalysisLane&)` clears both callbacks.
  - `SetDeterministicWait(bool deterministic, uint32_t boundUs = 0)` defaults to `false` (live mode NEVER waits, 0 sleeps, 0 `waitsMissed`).
  - `WaitForReadyDeterministic(renderSequence, presentSlackUs, guardUs)`: uses `VpSkeleton::DecideWait(Variants::NonIdentical, true, SlotState::Writing, policy, waitedUs, &waitMoreUs)` looping on `outCv.wait_for(...)` (woken immediately by `FinishSequence`, `SetFrameBound`, and `~Monitor()`). Records `NoteWaitMissed(BoundedWaitTimeout, renderSequence)` once on `BoundMissed`. Called before `ReleaseHeldFor` in `BeginBoundFrame` (fast-returning `true` with 0 yields when already ready while incrementing `DeterministicWaitCalls()` to distinguish deterministic checks from live mode) and in `AutoFactor` (only when `bound && deterministicWait && !ReadyFor(...)`). Exposes `DeterministicWaitCalls()` and `DeterministicWaitYields()` for direct proof.

---

## 3. Forward Migration Contracts (`O#7` & `O#8`)

### 3.1 `O#7` — `DetailPulse.cpp` (Awaiting `M2`)
- **Current private copies to retire:** `X2` (`kCutShare`, `kFadeStepShare` tile-change cut/fade judge) and `X3` (private tile energies / block stats), plus private frame-geometry assumptions.
- **Target shared inputs:**
  - **U7 (`SceneRecordReader::AcquireScene` / `SkeletonModule::SceneFactsOf`):** Replace `kCutShare` and `kFadeStepShare` with shared `cut` (full ring reset) and `fade` + `fadeKeep` (partial temporal history scaling `fadeKeep` in amplitude / `fadeKeep^2` in energy).
  - **U8 (`VpUnit::PictureRect` / `vpu_in_picture`, `vpu_picture_coverage`):** Mask black bars out of tile energy reduction so letterbox borders never dilute frame energy or trigger false resets.
  - **P4 (`SourceStats` v2 via `AcquireSourceStats`):** Use shared P4 mean/spread/grid when CPU-side scene gating or blank-frame abstention (`CutFadeJudge::BlankOf`) is needed.
  - **P2 / P3 (`EngineFieldFacts` / `MotionValidity`):** Gate temporal band-energy stabilisation on `MotionValidity::StampAt` continuity and `EngineFieldFacts::globalValid` / `p3Published`, abstaining or resetting across resync stamps.
- **Fallback visibility:** If any shared input (`SceneOf`, P4, U8, or P2/P3) is unavailable for a frame, `DetailPulse` holds neutral gain (`1.0`, safe bypass) rather than running a private duplicate judge, and surfaces the exact `why` via `FallbackCatalog` and counters.

### 3.2 `O#8` — `SourceResolutionDetector.cpp` (Awaiting `A2` `U#4`)
- **Current private copies to retire:** `X6` (private flat-block test `hi - lo <= flat` inside `SourceResolutionDetector.cpp`) and ad-hoc per-tile bar/mean/blank detection.
- **Target shared inputs:**
  - **P11 (`VpSkeleton::kP11RegionBlockStats`, U4):** Replace the private flat-block test (`X6`) with shared P11 block variance / range / structure-tensor facts once `A2` `U#4` lands P11 CPU/GPU readback parity.
  - **U8 (`VpUnit::PictureRect`):** Replace ad-hoc edge bar scanning (`barLeft`/`barRight`/`barTop`/`barBottom`) with shared `PictureRect` bounds, keeping tile placement strictly inside the active picture.
  - **P4 (`SourceStats` v2) & U7 (`CutFadeJudge::BlankOf` / `SceneOf`):** Consume shared frame-level luma mean (`NormalisedMean`), blank/black verdicts (`BlankOf`), and cut/fade verdicts (`SceneOf`) passed from `SourceResolutionMonitor` rather than recomputing whole-frame mean/cut/blank proxies inside tile spectra.
- **Fallback visibility:** When P11 or P4/U7/U8 is absent (standalone synthetic tile tests or degraded memo), `SourceResolutionDetector` marks the reading visibly via explicit `Thresholds` / `FrameReading` availability flags (`codingUnavailable`-style contract) and never silently fabricates private duplicate metrics in production.
