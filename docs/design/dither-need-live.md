# Dither need live wiring (`dither_need = auto`, member 4p / G-T1)

Status: design note (first commit of member `4p` on `gemini/w23-union`, task `G-T1`, verified against `HEAD = 2ae13d48c`). Built from `PENDING-MASTER.md` s15 (`GEM-DN-GPU-UNWIRED`), doc 115 row 12 (`DONE-UNWIRED`), `refs/102b-deferred-register.md:86` (dither-need part superseded by s15) and `:283` (owner 2026-11-12 05:06 EST: *"DITHER-NEED -> after the Saturday reset, FIRST item, with the owner's FL35 A/B; default off"*), `lead22/w23-member-list.md` (owner `Q-W23-3: YES`, member `4p`), `src/VP-V/modules/output/dither/DitherNeedGpu.h:75,288` (`BlockPass` + `HookGuard`, no production caller), and `src/VP-V/segments/CoreSegments.inl:552-577` (`:460` on older trees: `EvaluateDitherNeed`, empty `Feed`).

Supersedes the "NOT live yet" / "empty Feed" gap described in `docs/design/dither-need-callsites.md`. The owner's FL35 visual A/B runs after landing, before any default change.

---

## 1. Problem statement & current state on `gemini/w23-union`

The per-block dither-need law, GPU block pass, and sigma-delta shader consumer are already implemented and unit-tested, but the live GPU path has **no production caller**:

1. `DitherNeed::Gpu::BlockPass` (`src/VP-V/modules/output/dither/DitherNeedGpu.h:75-262`) and `DitherNeed::Gpu::HookGuard` (`DitherNeedGpu.h:288-333`) are included only by tests (`src/VP-V/modules/output/dither/tests/SigmaDeltaHookTests.cpp` and `src/VP-V/tests/beta_gpu/BetaGpuTests.cpp:13`). No production translation unit instantiates or invokes either class. Also, `BlockPass::VramBytes()` (`DitherNeedGpu.h:228`) returned `0` after `Invalidate()` even while both `rgba32f` ping-pong textures remain allocated in VRAM.
2. `CoreSegments.inl:552-577` (formerly `:460`) calls `outputFinish.EvaluateDitherNeed(activeSettings.ditherNeed == "auto" && any, pipelineSlots.OriginalPathway())` with the default empty `DitherNeedSeam::Feed{}` (`OutputSegments.h:148-152`). Every frame with `dither_need = auto` therefore reports `Unavailable (no grain product; no region precursors; no light-level table)` and binds nothing to `VPDN`.
3. In the skeleton analysis lane (`src/VP-V/modules/control/skeleton/SkeletonModule.cpp`), the three GPU product kernels already exist (`GrainSplitKernel` P10 at `:832`, `RegionStatsKernel` P11 at `:835`, `LightLevelKernel` P12 at `:836`) and are registered with `SkeletonGpuDispatcher`, **but**:
   - `SkeletonModule.cpp:254` calls `lane.SetGpuLiveDeferred(true)`, deferring live GPU fills to `AnalysisLane::RunDeferredLive(seq)` (`src/VP-V/core/AnalysisLane.h:1254-1292`);
   - `SkeletonModule::OnFrame` for `FramePhase::Entrance` (`SkeletonModule.cpp:2693`) only calls `budgetFeed.NoteCaptureRate(c.captureRateHz)` and never invokes `RunDeferredLive(seq)` (the deferred G6 live-fill gap);
   - `LightPrevStateOf` (`src/VP-V/modules/input/source_analysis/lane/LaneGpuLightKernel.h:49-59`) maps `ProductRead::State::Pending` to `LightPrevState::None` until G6 drives `RunDeferredLive`;
   - `SkeletonModule::UpdateDispatcher` (`SkeletonModule.cpp:815-840`) only creates a `SkeletonGpuDispatcher` when `analysis_lane = on` and `st.laneRaise.empty()`, so a raising consumer that needs GPU products cannot raise the dispatcher when `analysis_lane` is configured `off` (the default).

---

## 2. How the G6 P11/P12 live-fill dependency is satisfied

All live-fill plumbing stays strictly inside `src/VP-V/` with **zero new lines in Bill-owned files** (`LibplaceboVideoRenderer.cpp` is untouched; H2 and TU ceilings do not grow).

### 2.1 Consumer-driven GPU demand raise (`dither_need = auto` only)

Following the owner's demand-driven principle (`15c-c8`, `PENDING-MASTER.md` line 72):

- When `dither_need == "auto"`, `!pipelineSlots.OriginalPathway()`, the sigma-delta hook is active (`any` in `CoreSegments.inl:530`), and NLS is inactive (`submittedNls == nullptr`), `OutputFinishSegment` declares raising external demand via `Vpv::Skeleton::Demand()->SetRaisingExternalDemand(host, owner, consumers, {}, "dither need")` with one `ConsumerDecl` consuming `{ kP10GrainSplit, kP11RegionBlockStats, kP12LightLevel }` at lag `0` (and `kP4SourceStats` transitively as an ancestor of P11/P12). It tracks `ExternalErasures(host)` so a GPU-only reset of the same renderer re-declares cleanly.
- When `dither_need == "off"`, or on the Original pathway, or when the sigma-delta is inactive (`!any`), or when NLS declines `dither_need`, or on GPU teardown (`ReleaseGpu`), `ClearExternalDemand(host, owner)` is called immediately (change-only: no per-frame mutex churn when demand state is unchanged).
- In `SkeletonModule::UpdateDispatcher` (`SkeletonModule.cpp:815-840`) and `SkeletonModule::BuildLane` consumer list (`SkeletonModule.cpp:2964-2975`):
  - Track `bool raisedWantsGpu` — true if and only if at least one active raising external consumer consumes a GPU product (`kP10GrainSplit`, `kP11RegionBlockStats`, or `kP12LightLevel`).
  - `UpdateDispatcher` constructs `SkeletonGpuDispatcher` when `st.gpu != nullptr && st.settings.lane.mode == LaneSettings::Mode::On && (st.laneRaise.empty() || st.raisedWantsGpu)`.
  - An LED-only raise (`"LED drive"`, consuming CPU P4 only) keeps `raisedWantsGpu == false`, so **no GPU dispatcher is created for LED-only demand** (`15c-c8` zero-waste invariant preserved).
  - With `dither_need = off` (default) and `analysis_lane = off` (default), **nothing is raised and no GPU dispatcher or kernel is constructed**.

### 2.2 G6 Entrance deferred live fill (`RunDeferredLive`)

`FramePhase::Entrance` is already fanned out on the render thread inside the GPU-legal phase before `CollectFrameHooks` (`LibplaceboVideoRenderer.cpp:20806` -> `RendererExtension::NotifyEntrance` -> `SkeletonModule::OnFrame(FramePhase::Entrance)`).

1. **Drive `RunDeferredLive(seq)` at Entrance (`SkeletonModule.cpp:2693`)**:
   - In `SkeletonModule::OnFrame` when `c.phase == RE::FramePhase::Entrance`:
     - Keep `e->budgetFeed.NoteCaptureRate(c.captureRateHz);`.
     - When `e->active.load(std::memory_order_acquire)` and `c.seq > 0` and bundle `b` has a live dispatcher (`b->wiring.dispatcher != nullptr`):
       - Ensure `e->dispatcher->FlushReleases()` and `e->dispatcher->Configure(...)` have allocated product textures for the active graph/dims before dispatching (no-op when unchanged; avoids Frame 1 failure before the first `AfterRender`).
       - Call `b->lane.RunDeferredLive(static_cast<uint64_t>(c.seq))` on the render thread.
       - Record that Entrance live dispatch is active (`e->entranceLiveDriven = true`) and refresh visible diagnostics/status when live dispatch transitions or runs.
   - Because `AnalysisLane::RunDeferredLive(seq)` (`AnalysisLane.h:1254-1292`) iterates `livePending_` in topological order, P10 (`kP10GrainSplit`), P11 (`kP11RegionBlockStats`), and P12 (`kP12LightLevel`) are dispatched through `SkeletonGpuDispatcher::DispatchLive` in exact dependency order before `CollectFrameHooks` runs.
   - If a product was already computed ahead in the post-render gap (`skeleton_gpu_gap = after_render`), it is already in the ring (`ProductRead::State::Ring`) and `RunDeferredLive` is a no-op for it (compute-once preserved).
2. **P12 begin gate (`LaneGpuLightKernel.h:49-68`, `SkeletonModule.cpp:244-247`)**:
   - Update `LightPrevStateOf` with `bool g6LiveDriven = false`: when G6 live fills are driven by Entrance, `ProductRead::State::Pending` maps to `LightPrevState::Pending` (preventing P12(N) from prematurely resetting its one-pole while P12(N-1)'s Entrance fill is genuinely pending), while preserving the legacy `Pending -> None` behaviour when a lane is driven without G6 live dispatch.
3. **Display facts sync to P11/P12 kernels**:
   - Feed the resolved `LightLevel::Settings` and `LightLevel::Context` (derived from `activeSettings`, `renderImage.color`, `renderImage.repr`, and `target.color` in `CoreSegments.inl`) through a header-light `IExternalDemand::SyncDitherNeedDisplayFacts` helper to `RegionStatsKernel::SetDisplayFacts` and `LightLevelKernel::SetSettings` (change-only), so P11 and P12 use the real display facts rather than static SDR defaults whenever live facts are known; if facts are unavailable, the existing `region_stats_display` / `light_level_display` fallbacks stay visibly engaged.
4. **Release `SkeletonGpuLive` fallback once G6 is active (`SkeletonModule.cpp:3304`)**:
   - `FallbackCatalog::SkeletonGpuLive` is released once Entrance has driven live fills (and `DeferredTotals().failed > 0 || stale > 0` remains visible via `SkeletonGpuFailed` / status diagnostics).

### 2.3 Product acquisition, NLS guard (`GEM-224`), & GPU block-pass execution (`CoreSegments.inl` + `OutputSegments`)

All GPU product reads happen on the render thread inside `CoreSegments.inl` (VP-V owned) after `OrderFrameSlotHooks()` and `frameParams.hooks` setup (`CoreSegments.inl:799-801`), right beside `nextVectorGuard.Apply` (`:806-820`) and before `TraceFrameHookList` (`:871`):

1. **Read shared GPU products P10 + P11 from the skeleton (no private recomputation, rule 25)**:
   - Query `IExternalDemand::ReadDitherNeedGpuInputs(host, sourceSequence)` (render-thread-only method on `IExternalDemand` with safe default returning `{}` when no skeleton/dispatcher exists, using opaque `const void*` texture handles so `SkeletonFacts.h` stays free of libplacebo headers):
     - Reads `P10` (`kP10GrainSplit`) via `lane.ReadOf(kP10GrainSplit, seq)` -> `GpuProductTextures::Ring`/`Live`:
       - Extracts the `blocks` texture (`term 2` in Compact layout `[splitA, splitB, blocks]`, `term 1` in Full layout `[split, blocks]`, resolved from `set->TermCount()`, where `.x` is the per-block grain level `N`).
     - Reads `P11` (`kP11RegionBlockStats`) via `lane.ReadOf(kP11RegionBlockStats, seq)` -> `GpuProductTextures::Ring`/`Live`:
       - Extracts all 6 `rgba32f` precursor textures `precursors[0..5]` (`re_s0..re_s4`, `re_aux`).
     - Reports `srcW`, `srcH`, `gridW`, `gridH`, `blockPx = RegionEvidence::BlockSizeFor(dims.height)`, and `grainSeq` / `evidenceSeq` (for age/staleness verification).
2. **Build / cache P12 light-level LUT textures (`LightLevel::LutCache`)**:
   - Build `LightLevel::Settings` and `LightLevel::Context` from `activeSettings` + `renderImage.color` / `renderImage.repr` + `target.color` (matching `LightLevelSettings.h` and `LightLevelLut.h` exact rules: `Sdr`, `HdrPassthrough`, or `HdrToSdr`).
   - Call `const LightLevel::Lut& lut = lutCache_.Get(llSettings, llContext)`.
   - Only when `lutCache_.LastRebuilt()` or textures are not yet allocated on `gpu` (or `gpu` changed), pack and upload:
     - `lutTex_` (`1024 x 1 rgba32f`, `LightLevel::PackTexels(lut)` — `(J, S, V, L/10000)`),
     - `lutOutTex_` (`1024 x 1 rgba32f`, `LightLevel::PackOutputTexels(lut)` — `(sdrCode, Sout, 0, 0)`).
   - Both textures live inside `OutputFinishSegment` and are destroyed by `ReleaseGpu(oldGpu)` before `pl_gpu` teardown, with total allocated VRAM (`BlockPass::VramBytes() + TexBytesOf(lutTex_) + TexBytesOf(lutOutTex_)`) declared to `VpSkeleton::VramUsers` under `VpSkeleton::kUserDitherNeed`.
3. **Read P14 source quality (`SourceIdentity::QualityView`)**:
   - Read from `vpv.sourceAnalysis.sourceMonitor.QualityView()` (same shared view already read at `CoreSegments.inl:318` and `:485-486`).
   - If `QualityView` is absent or in fallback, `qualityNeed = 1.0` (full designed amplitude), status appends `[dither need: classical quality]`, and `framesClassicalQuality` increments (never silent).
4. **NLS guard (`GEM-224`)**:
   - Because `Temporal sigma-delta.glsl` samples `VPDN` via an affine mapping `(posOut + 0.5) * dn_scale + dn_off`, an active non-linear stretch (`submittedNls != nullptr`) would silently misalign the block map.
   - When `submittedNls != nullptr`, `DitherNeedSeam::Feed::nlsActive = true` sets `DitherNeed::kNonLinearStretch` (`"non-linear stretch active"`), returning `Result::Unavailable`, incrementing `framesUnavailable` and `framesNlsDeclined`, engaging `FallbackCatalog::DitherNeed`, invalidating `BlockPass`, and leaving `HookGuard` unbound (`dither_need = 0`). Recovery is automatic (`decision.reset = true`) once NLS turns off.
5. **Evaluate host seam + run `DitherNeed::Gpu::BlockPass::Step` + bind `DitherNeed::Gpu::HookGuard::Apply`**:
   - Construct `DitherNeed::Params` with `aBase` read from the loaded sigma-delta quantiser's configured `noise_amplitude` (`CustomHookChain::Chain::QuantizerConfigured("noise_amplitude", val)`, default `0.15`), and `aNeed` scaled by `qualityNeed`:
     `params.aNeed = base.aBase + (base.aNeed - base.aBase) * qualityNeed`.
   - Evaluate `DitherNeedSeam::Stage::Evaluate` with real P10/P11/P12/P14 `Feed`, `dtSeconds` (from measured `captureRateHz` / `nominalSourceRateHz`, `<= 0` when unknown), `discontinuity` (scene cut / resync / frame generation jump), product age (`kStaleProduct` when `age > DitherNeedSeam::kMaxProductAgeSeconds = 0.5 s`), and affine placement validation (`dstW > 0 && dstH > 0`).
   - When `need.extraAllowed`, run `blockPass_.Step(d3d11->gpu, dispatch, args, frameArgs, /*requireBind=*/true)`.
   - If `step.ran` (`blockPass_.Current() != nullptr`), apply `hookGuard_.Apply(frameParams.hooks, frameParams.num_hooks, blockPass_.Current(), placement)`:
     - If `Apply` returns `false` for a non-null product, convert the frame outcome to `Unavailable` (`Stage::RecordGpuFailure`), report `DitherNeed::Gpu::HookGuard::MissingAfterApply()` (`kNoSetTexture` on libplacebo 7.360 without patch 0004, or `kPassFailed` if binding failed) through `FallbackCatalog::DitherNeed`, and increment `framesUnavailable`.
     - On `Apply == true`, attach `[this]() { ReleaseDitherNeedBind(); }` to `nextVectorGuard` (`SigmaDeltaVectors.h`, 100% VP-V owned!), which `LibplaceboVideoRenderer.cpp:21351` ALREADY calls immediately after `NlsHookRecovery::RenderOwned(...)` and before de-judder extras (and `OutputFinishSegment::OnDraw` calls on an NLS retry draw, and `~NextVectorGuard()` calls on scope exit). That restores `dither_need = 0.0` and unbinds `VPDN` with **zero new Bill lines**.
   - **GPU teardown safety (Rule 2 — backing lifecycle invariant)**:
     - `OutputSegments::Merger::Release()` (`OutputSegmentsMerge.h:80`, called at `LibplaceboVideoRenderer.cpp:6566` BEFORE `d3d11->gpu` is destroyed) calls `s3.ReleaseGpu(gpu)`, which releases `hookGuard_`, calls `blockPass_.Release(gpu)`, destroys `lutTex_` / `lutOutTex_` and `pl_dispatch`, withdraws `VramUsers` (`kUserDitherNeed`), and withdraws external demand.
     - If `gpu` pointer changes between frames without a prior `Release()`, old GPU handles are invalidated safely before allocating on the new `pl_gpu` (and `BlockPass::Release` is called only on a still-alive `pl_gpu`).

---

## 3. Derived mathematics & contract constants (zero invented constants)

Every formula and constant comes directly from signal physics, bit-depth quantization, or existing frozen contract constants in `DitherNeed.h`, `DitherNeedGlsl.h`, `DitherNeedSeam.h`, and `LightLevelLut.h`:

| Quantity | Formula / Value | Derivation / Contract Source |
|---|---|---|
| Link code step in 10-bit units | `q_out = 2^(10 - colorBits)` | Exact bit-depth ratio (`DitherNeed.h:78-82`: 8-bit -> `4`, 10-bit -> `1`) |
| Grain RMS in link codes | `sigmaOut = sqrt(max(N, 0)) * |Sout| / q_out` | Variance-to-RMS conversion scaled by output transfer slope `Sout = d(sdrCode)/d(code)` (`DitherNeed.h:102`, `DitherNeedGlsl.h:69`) |
| Effective wanted amplitude | `aNeed_eff = aBase + (aNeed - aBase) * (1 - quality)` | Existing contract (`DitherNeedSeam.h:119,126`; `SourceIdentity::DitherNeed(q) = 1 - q`) |
| Target total amplitude | `aTarget = clamp(aNeed_eff - sigmaOut, aBase, max(aMax, aBase))` | Subtracts existing grain dithering; bounded by `aMax = 0.5` (exact half-LSB rounding boundary so exact codes never move, `DitherNeed.h:70,105`) |
| Visibility gate `gVis` | `|Sout| > 1e-6 ? Smoothstep(visLo, visHi, V * q_out / |Sout|) : 0` | One link step in Weber JNDs (`V * q_out / |Sout|`); `1e-6` is a strict numerical singularity guard (`DitherNeed.h:91-95`, `core/Ramp.h`) |
| Extra amplitude target | `target = (inPicture && !overlay) ? banding * gVis * (aTarget - aBase) : 0` | Zero outside active picture, on overlays, in non-banding blocks, or where link step is invisible (`DitherNeed.h:107-108`) |
| Temporal smoothing | `alpha = 1 - exp(-dt / tau)` (`dt <= 0` -> `1.0`) | Rate-independent continuous one-pole (`core/OnePole.h`, `DitherNeed.h:85-88`); hysteresis deadband `params.deadband` |
| Product staleness limit | `kMaxProductAgeSeconds = 0.5` | Existing contract constant (`DitherNeedSeam.h:52`) |
| Output-to-source affine placement | `scaleX = cropW / dstW`, `scaleY = cropH / dstH`, `offX = crop.x0 - dst.x0 * scaleX`, `offY = crop.y0 - dst.y0 * scaleY`, `blockPx = RegionEvidence::BlockSizeFor(height)` | Exact affine map `src = (pos + 0.5) * scale + off` (`DitherNeed.h:416-423`, `DitherNeedGpu.h:266-270`). Guarded: `dstW > 0 && dstH > 0`, else fallback `kPassFailed`. Under NLS (`submittedNls != nullptr`), visibly declined with `kNonLinearStretch` (`GEM-224`). |
| Link-noise acceptance gate (offline / FL35 A/B) | Shadow (`sigmaOut < 0.35`): `<= 1.40x`; elsewhere: `<= 1.05x` vs plain+4 | Existing owner-approved gate (`DitherNeedSeam.h:38-49`, doc 115 items 12-13) |

**VRAM footprint**: `2 * gridW * gridH * 16 B` (two `rgba32f` block-grid ping-pong textures, accurately reported by `BlockPass::VramBytes()` even while invalidated) + `2 * 1024 * 16 B` (32 KB for the two 1024x1 LUT textures), declared to `VpSkeleton::VramUsers` under `VpSkeleton::kUserDitherNeed`. At 1080p (`120 x 68` at `B = 16`) or 4K (`120 x 68` at `B = 32`), total private VRAM is ~293 KB.

---

## 4. Files touched by member `4p`

**Zero Bill-owned files are edited.** All production changes live under `docs/design/` and `src/VP-V/`:

| Commit | Kind | Files | Purpose |
|---|---|---|---|
| Commit 1 | Docs (first commit, G-T1) | `docs/design/dither-need-live.md` | This design note |
| Commit 2 | Prod (G6 live fill + DN live GPU wiring + NLS guard) | `src/VP-V/core/VramGovernor.h`, `src/VP-V/core/VramUsers.h`, `src/VP-V/modules/control/skeleton/SkeletonFacts.h`, `SkeletonModule.h`, `SkeletonModule.cpp`, `src/VP-V/modules/input/source_analysis/lane/LaneGpuLightKernel.h`, `src/VP-V/modules/output/dither/DitherNeed.h`, `DitherNeedGpu.h`, `DitherNeedSeam.h`, `SigmaDeltaVectors.h`, `src/VP-V/segments/OutputSegments.h`, `OutputSegments.cpp`, `OutputSegmentsMerge.h`, `OutputSegmentsMerge.cpp`, `CoreSegments.inl`, `src/VP-V/modules/output/dither/README.md` | Drive `RunDeferredLive(seq)` at `FramePhase::Entrance`; allow raising external GPU consumers to create `SkeletonGpuDispatcher`; expose render-thread P10/P11 GPU texture reader and display-facts sync on `IExternalDemand`; fix `BlockPass::VramBytes()` after `Invalidate()`; declare `kUserDitherNeed` to `VramUsers`; wire `BlockPass` + `HookGuard` + `LutCache` + NLS guard (`kNonLinearStretch`) into `OutputFinishSegment` and `CoreSegments.inl`; RAII post-render unbind via `NextVectorGuard`; pre-teardown GPU release via `OutputSegments::Merger::Release` |
| Commit 3 | Tests (separate Red Team `WAIVER-*` commit) | `src/VP-V/modules/output/dither/tests/DitherNeedSeamTests.cpp`, `SigmaDeltaHookTests.cpp`, `src/VP-V/modules/control/skeleton/tests/SkeletonModuleTests.cpp`, `src/VP-V/tests/AnalysisLaneGpuHookTests.cpp` | Independent adversarial proving tests with negative controls (Section 8 below) |

---

## 5. Default OFF & Original pathway identity

- **Default OFF**: `VpvRendererFields.h:312` (`std::string ditherNeed = "off";`), `DitherNeed.h:439` (`bool on = false;`), `VpvConfigTests.cpp:428`, and `src/VP-V/tests/golden/cpu-defaults.golden.txt` all keep `dither_need = off` by default.
- **Zero work when OFF**: When `dither_need == "off"`, no external demand is registered (so no GPU dispatcher is raised), `BlockPass::Step` is not dispatched (tracker is reset/invalidated only), `HookGuard::Apply` is not called, `dither_need` shader PARAM stays `0.0`, `VPDN` stays unbound (1x1 zero placeholder), no fallback entry is engaged, and no counters increment. Output is byte-identical to today.
- **Original pathway**: `OriginalPathway.h:96` and `VpvRendererSettings.cpp:1152` force `dither_need = "off"`. Even if called directly with `originalPathway == true`, `DitherNeedSeam::Stage::Evaluate` and `OutputFinishSegment` return immediately, withdraw any external demand, release any bound guard, and report nothing. Original == Bill byte-for-byte.

---

## 6. Config row on the VP-V master page

The canonical Config row already resides on the **VP-V master page** (`src/VP-V/modules/control/config/config_pages/VpvSection.cpp:1604-1618`):

- **Location**: `VP-V > Pipeline (expert) > Dither where needed` (part of `VpvSection.cpp`, the VP-V master page; never on any Bill Config page).
- **Control**: `QComboBox`, object name `vpv.expert.dither_need`.
- **Items**:
  - Index 0: `Off (default)` -> stored value `""` (omitted / `"off"` in `[vpv]`).
  - Index 1: `Auto (extra dither only where banding would show)` -> stored value `"auto"` in `[vpv] dither_need`.
- **Original pathway greying**: `VpvSection.cpp:1741-1742` disables the combo and sets grey reason `"Greyed: the Original pathway is on (VP's additions are off)."` when `original` is active.
- **Registry & compliance**: Registered in `VpvSchemaKeys.h:313,372`, `docs/settings/MASTER-SETTINGS.md`, `modules/control/config/README.md:65`, and `modules/output/dither/README.md:22`. No duplicate control is added, keeping `check_config_placement.py` and `check_master_settings.py --strict` green.

---

## 7. Visible fallbacks & diagnostics (no silent fallbacks)

Every non-`On` state while `dither_need == "auto"` (and not Original) is explicitly visible in Ctrl+I / Config status (`FallbackCatalog::DitherNeed` / `FallbackCatalog::DitherNeedInactive`), counted in `DitherNeedSeam::Counters`, and logged on every state transition:

| Condition | Missing bit(s) / Reason | Fallback entry (`id = "dither need"`) | Counter(s) | Output behaviour |
|---|---|---|---|---|
| `dither_need == "off"` or Original | none | Released | none | `dither_need = 0`, `VPDN` unbound (byte-identical) |
| `dither_need == "auto"` but sigma-delta off (`temporal_dither != sigma_delta` or `output.dither` slot off) | inactive reason | `FallbackCatalog::DitherNeedInactive(true, why)` | status logged on change | No pass, no bind |
| Non-linear stretch active (`submittedNls != nullptr`, `GEM-224`) | `kNonLinearStretch` | `non-linear stretch active` | `framesUnavailable`, `framesNlsDeclined` | `BlockPass::Invalidate()`, base amplitude |
| Missing P10 grain split (`gsBlocks == nullptr`) | `kNoGrain` | `no grain product` | `framesUnavailable` | `BlockPass::Invalidate()`, base amplitude |
| Grain grid != block grid | `kGrainGridMismatch` | `grain product on another grid` | `framesUnavailable` | `BlockPass::Invalidate()`, base amplitude |
| Missing P11 region precursors (any of `re_s0..s4`, `re_aux` null) | `kNoPrecursors` | `no region precursors` | `framesUnavailable` | `BlockPass::Invalidate()`, base amplitude |
| Precursor grid != block grid | `kEvidenceGridMismatch` | `region precursors on another grid` | `framesUnavailable` | `BlockPass::Invalidate()`, base amplitude |
| Missing P12 light-level LUT / upload failed | `kNoLut` | `no light-level table` | `framesUnavailable` | `BlockPass::Invalidate()`, base amplitude |
| LUT texture width != 1024 | `kLutSizeMismatch` | `light-level table of the wrong size` | `framesUnavailable` | `BlockPass::Invalidate()`, base amplitude |
| Output domain unsupported (float / 16-bit target) | `kUnsupportedDomain` | `output domain not supported` | `framesUnavailable` | `BlockPass::Invalidate()`, base amplitude |
| Product age > `0.5 s` (`kMaxProductAgeSeconds`) | `kStaleProduct` | `a shared product is stale` | `framesUnavailable`, `framesStale` | `BlockPass::Invalidate()`, base amplitude |
| Standard libplacebo 7.360 without patch 0004 (`!PL_VP_MPV_USER_SHADER_SET_TEXTURE`) | `kNoSetTexture` | `libplacebo without set_texture` | `framesUnavailable` | `BlockPass::Invalidate()`, base amplitude |
| `BlockPass::Run` fails or `HookGuard::Apply` fails or invalid crop (`dstW <= 0` / `dstH <= 0`) | `kPassFailed` (+ `BlockPass::LastError()` logged) | `GPU pass failed` | `framesUnavailable` | `BlockPass::Invalidate()`, base amplitude |
| Link step invisible at all codes (`!NeededAnywhere`) or clean source (`qualityNeed < 1e-3`) | none | Released; status `"dither need: not needed (link step invisible)"` | `framesNotNeeded` | `BlockPass::Invalidate()`, base amplitude |
| P14 `QualityView` absent / fallback while `On` | none (additive status tag) | Status appends `[dither need: classical quality]` | `framesOn`, `framesClassicalQuality` | Runs with full `aNeed` (`qualityNeed = 1.0`) |
| All products present, fresh, valid, and bound | none | Released; status `"dither need: on"` | `framesOn` | `BlockPass` runs; `VPDN` bound; `HookGuard::Release()` restores after render |

Additionally, skeleton-level G6 diagnostics remain visible:
- `SkeletonGpuDispatcher::Fallbacks()` surfaces `region_stats_bands`, `region_stats_display`, `light_level_frame`, `light_level_display`, `light_level_smoothing`, `skeleton_gpu` (refusals), and `skeleton_gpu_failed` (`gd->Stats().failures`, `gd->LastError()`).
- `AnalysisLane::DeferredTotals()` surfaces `run`, `failed`, and `stale` counts for Entrance deferred live fills.

---

## 8. Proving tests & expected executed counts

All new GPU hardware tests use `HardwarePrerequisites::RequireOrSkip(Kind::Gpu)` (emitting `SKIP-NO-HARDWARE` when no hardware GPU exists; never bespoke skip strings). No existing test assertion is weakened or deleted.

### 8.1 Required new / strengthened proving tests (G-T1)

1. **Non-test production-caller wiring pin** (`DitherNeedSeamTests`):
   - Reads production sources under `src/VP-V/` (excluding `tests/` and `beta_gpu/`): `OutputSegments.h`, `OutputSegments.cpp`, `CoreSegments.inl`, and `SigmaDeltaVectors.h`.
   - Asserts that non-comment production code references `DitherNeed::Gpu::BlockPass` (`.Step(` and `.Release(`) AND `DitherNeed::Gpu::HookGuard` (`.Apply(` and `.Release(`), AND that `CoreSegments.inl` passes live products rather than an empty default `Feed`.
2. **On / Off / Original identity & NLS guard (`GEM-224`)** (`DitherNeedSeamTests` + `DitherNeedWarpTests`):
   - CPU & WARP: `dither_need = off` and `Original` pathway register zero external demand, never invoke `BlockPass`, leave `HookGuard` unbound, emit empty status, and release fallback.
   - `submittedNls != nullptr` visibly declines `dither_need` (`kNonLinearStretch`, `framesNlsDeclined`, `FallbackCatalog::DitherNeed` engaged, `HookGuard` unbound) and recovers automatically when NLS turns off.
   - `BlockPass::VramBytes()` stays non-zero after `Invalidate()` while ping-pong textures remain allocated, and drops to `0` after `Release(gpu)`.
   - `OutputFinishSegment::RunDitherNeedGpu` declares VRAM to `VramUsers` (`kUserDitherNeed`) and withdraws it on `ReleaseGpu`.
3. **Stale / absent product & automatic recovery** (`DitherNeedSeamTests` + `DitherNeedWarpTests`):
   - Missing P10, missing P11, missing P12 LUT, stale product (`age > kMaxProductAgeSeconds`), mismatched grid, degenerate crop, and unsupported domain each produce `Unavailable`, engage `FallbackCatalog::DitherNeed` with the exact missing-bit text, increment `framesUnavailable` (and `framesStale` when stale), leave `BlockPass::Current() == nullptr` and `HookGuard` unbound, and recover automatically with `decision.reset == true` on the next valid frame.
4. **G6 Entrance live-fill & GPU demand raise** (`SkeletonModuleTests` + `AnalysisLaneGpuHookTests`):
   - `OnFrame(FramePhase::Entrance)` invokes `RunDeferredLive(seq)` on the dispatcher, increments `DeferredTotals().run`, and releases `SkeletonGpuLive`.
   - Raising external demand consuming `{ kP10GrainSplit, kP11RegionBlockStats, kP12LightLevel }` creates `SkeletonGpuDispatcher` even when `analysis_lane = off`, whereas LED-only P4 raising demand creates no dispatcher, and `ClearExternalDemand` tears it down cleanly.
   - `LightPrevStateOf(Pending, /*g6LiveDriven=*/true) == LightPrevState::Pending` while legacy non-G6 mode stays `None`.
5. **Hardware GPU / Beta fork verification** (`DitherNeedWarpTests` with `VP_TEST_GPU=hardware` + `BetaGpuTests` case `(g)`):
   - Runs on WARP in the CPU lane and on hardware NVIDIA in `-GpuWarp`; any hardware-only GPU test uses `HardwarePrerequisites::RequireOrSkip(Kind::Gpu)`.
   - `BetaGpuTests` case `(g)` (`BetaGpuTests.cpp:7690`) continues to prove real `HookGuard` binding against libplacebo fork 377 (`PL_VP_MPV_USER_SHADER_SET_TEXTURE`).

### 8.2 Expected executed test counts

| Test Class / Suite | File | Baseline Count (pre-4p) | Expected Executed Count (post-4p) | Delta | Lane |
|---|---|---|---|---|---|
| `DitherNeedSeamTests` | `modules/output/dither/tests/DitherNeedSeamTests.cpp` | 8 | >= 11 | >= +3 | CPU |
| `DitherNeedTests` | `modules/output/dither/tests/SigmaDeltaHookTests.cpp` | 15 | 15 | 0 | CPU |
| `DitherNeedWarpTests` | `modules/output/dither/tests/SigmaDeltaHookTests.cpp` | 11 | >= 13 | >= +2 | CPU (WARP) & `-GpuWarp` |
| `SigmaDeltaHookTests` | `modules/output/dither/tests/SigmaDeltaHookTests.cpp` | 15 | 15 | 0 | CPU (WARP) & `-GpuWarp` |
| `SkeletonModuleTests` | `modules/control/skeleton/tests/SkeletonModuleTests.cpp` | existing | existing + >= 2 | >= +2 | CPU |
| `AnalysisLaneGpuHookTests` | `tests/AnalysisLaneGpuHookTests.cpp` | existing | existing + >= 1 | >= +1 | CPU |
| `VpvConfigTests` | `modules/control/config/tests/VpvConfigTests.cpp` | existing | existing | 0 | CPU (ConfigTests) |
| `BetaGpuTests` | `tests/beta_gpu/BetaGpuTests.cpp` | 1 suite (`RunBetaSuite`) | 1 suite (incl. case `(g)`) | 0 | Beta host (377) |

---

## 9. Follow-ups explicitly outside member `4p`

- **Owner FL35 A/B (`DN-OWNER-AB`)**: runs after `gemini/w23-union` lands; `dither_need` stays default `off` until the owner rules on the FL35 visual A/B and link-noise ratios.
- **Step 5 package AC (`GEM-SD-NEED`, `GEM-224` non-affine shader placement)**: non-affine `VPDN` sampling under NLS and `GEM-021`/`022`/`030` sigma-delta need refinements rebase on top of `4p` (which visibly declines `dither_need` under NLS via `kNonLinearStretch` rather than silently misaligning).
