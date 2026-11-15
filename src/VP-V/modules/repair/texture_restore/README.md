# Texture restore + breathing add-on (`repair.texture_restore`)

Unit: **repair** | side: input | order: 90 | status: **built** | hook point: prepass

## Design goal, philosophy and decisions

- Goal: even out the detail the encoder spent unevenly across a GOP, from the picture's own bands, without ever copying content between frames or averaging grain away.
- Philosophy: one judgement of "the picture changed" for the whole player. These two modules do not keep a cut or fade judge of their own: a cut or fade is the shared judge's verdict (unit U7, CutFadeJudge over two P4 records), read from the skeleton's scene group. No scene group for a frame means restart (gain 1), counted and visible, never a private rule.
- Decision D8 (registered fix, owner confirmed; before/after in the lane's FIX-B6-D8 record): the earlier private tile-change cut and two-step mean-ramp fade laws are retired. The old laws remain only as test references in `tests/support` (`DetailPulseModel.h`, `TextureRestoreModel.h`), so the A/B stays reproducible.
- Consequence to watch: the shared judge uses histogram distance on whole frames, not tile shares, and has no vector-aware veto; a vector-explained pan no longer needs one because it moves histograms little.
- Decision (C5 batch B, `cut_soft_reset`, default on; owner 2026-11-15 08:35 EST, question window U7 Q4, refs\owner-decisions-2026-11-07.md: "need the bar fairly high here. Mid scene firing is a major problem"): a near-cut the shared judge is very sure of (its confirmed verdict a candidate that is neither cut nor flash, not a fade frame, strength >= `CutFadeJudge::kSoftResetBar`) reduces the history through the SAME keep multiply as a fade (keep = `SoftResetKeep`, squared for energies); below the bar nothing happens, a confirmed cut still restarts, and the fade partial reset is not behind the switch. The bar (1.0) and depth (0.5) are PROVISIONAL until the D8 truth run sets them (plan lead19\c5-softreset.md items 1-4).

## Purpose / why

Long-memory add-on on A's output: texture restore (~2 s peak-hold fine-band reference along vectors) and breathing reduction (detail_pulse_stabilise, the GOP pulse). Premium, needs an efficiency rebuild (9.8 ms at 4K on the 3050 Ti).

## Contract

- Input: planes after the re-base and deband (luma; detail pulse also takes a one-plane RGB picture); vectors (validity-gated)
- Output: same size and format; only the luma (or the RGB plane) is changed, chroma planes pass through
- Geometry: same as input
- Delay share: 0 frames
- Look-ahead: detail pulse may measure the next queued frame (optional)

## How the code works

Two independent modules run in one slot, in this order: detail pulse stabilise (`DetailPulse.*`, the breathing reduction), then texture restore (`TextureRestore.*`). The maths are documented at the top of each header and in docs/VP-detail-restore.md. The CPU reference models (src/VideoProcessor-Test/DetailPulseModel.h and TextureRestoreModel.h) share the constants.

### Public API

`DetailPulse` (DetailPulse.h):

- Constants and helpers shared with the model: `kDefaultCap` 1.35, `kCapMin`/`kCapMax` 1.0..1.5, `kDefaultFast`, `Scale`, `WindowFrames` (0.17 s of history), `Gain`, `VetoWeight`, `TrustWeight`, `LumaWeights`.
- `Params`: `cap`, `colorDepth`, `planeLines` (re-based height), `sourceHz`, `bitrateQuality`, the optional `field` with `fieldScale`/`fieldScaleX`, `sequence`, `lumaSystem`/`codeLevels` (a one-plane RGB picture is stabilised on its luma), `fast` (detail_pulse_fast), `softReset` (cut_soft_reset; both modules). `SceneSoftResets()` / `SceneSoftStrengthSum()` count the soft resets and their strengths (both modules).
- `Module::Apply`: a new consecutive `sequence` measures two luma bands per 32 px tile, advances the GPU history ring and decides gains. A repeat of the same sequence applies the stored decision to the picture given, with that picture's own bands. A non-consecutive sequence, a geometry change or a cut restarts the history (gain 1). A fade does not (U7 v3, owner 2026-11-15: partial reset on fades): every reference is scaled by the frame's fade keep squared per frame of its age (`SceneRestartAnswer::historyKeep`, `FrameAnalysis::Scene::fadeKeep`), so the history shrinks with the fading picture and no pre-fade detail is carried; texture restore takes the same factor as one more decay step of its references. A fade group without a keep restarts, visibly (the "<module> scene judge" fallback, `SceneFadeWithoutKeepWhy`). The cut / fade verdict is the shared judge's (U7 over P4, read from the skeleton scene group by `SceneRestart.h`; registered fix D8); a frame whose scene group is not readable restarts and is counted (`SceneMissing()`), never judged privately.
- `ApplyForeign`: the current decision on a picture of another geometry; never advances the history.
- `MeasureAhead(..., next frame, debander, deband params)`: optional look-ahead. It measures the next source frame's bands, debanded first through `VpDebander` slot `kSlotLookAhead` when the debander runs. A second overload takes a bare luma texture.
- `Forget()`, `Release(gpu)`, `LastError()`, `Failures()`, `Frames()`, `HistorySequence()`, `HistoryFrames()`, `AheadFrames()`. `SetPassTimers` / `PassTimes`: GPU pass timing for tests.

`TextureRestore` (TextureRestore.h):

- Constants shared with the model (bands 1 / 2.5 / 6 px, 16 px tiles x `Scale`, 2 s hold half-life, focus and pull gates), `kDefaultCap` 1.5, `kCapMin`/`kCapMax` 1.0..2.0.
- `Params`: `cap`, `colorDepth`, `planeLines`, `sourceHz`, `bitrateQuality`, `field`/`fieldScale`/`fieldScaleX`, `sequence`.
- `Module::Apply`: eight passes. Half-resolution low bands; tile statistics; grain level; restart flag (the shared judge's verdict); references and gains; apply. A repeat reuses its frame's gains with its own low bands (`lowRepeat`) and never advances the references. A `cap` of 1.0 returns the input itself and restarts the references when the cap rises.
- `ApplyForeign`, `Forget()`, `Release(gpu)`, `LastError()`, `Failures()`, `Frames()`, `HistorySequence()`.

### Data flow

In: a `pl_frame`; only plane 0 is read. Out: a copy of the frame with plane 0 replaced by the module's own texture: `out` from `Apply`, `outForeign` / `fOut` from `ApplyForeign`. The module owns the textures; each call overwrites them, and `Release` frees them. Detail pulse's history is a GPU ring (`kRingSlots` = 128 entries); texture restore keeps two reference sets and two low-band sets.

Call sites (`src/VP-V/segments/EntranceSegment.cpp`, `Segment::Detail` and `Segment::DetailLookAhead`):

- Main, drawn as itself: `DetailLookAhead`, then `DetailPulse::Apply`, then `TextureRestore::Apply` on its output. A failure sets the status reason (see below).
- Main, Stage B phase picture: the history first advances once on A itself (A's debanded picture, the result discarded), then the picture takes the same Apply calls as a repeat.
- De-judder extra: `Apply` as a repeat, only when the history already holds this source frame. It never advances the history.
- Crossfade path being left: `ApplyForeign` with `planeLines` scaled by the factor ratio, only when a decision for this frame is held.
- Look-ahead: once per new source frame, only when the de-judder uploaded the next frame (two planes, not RGB) and no re-base is engaged. It is also checked when a cached or ring entrance result is reused.

The host (`LibplaceboVideoRenderer.cpp`) owns both modules and fills `detailPulseParams` / `textureRestoreParams` and the `detailPulseRuns` / `textureRestoreRuns` flags once per frame. Both are off while smooth motion or deinterlacing runs. The field is the de-judder's only when its estimate is for this source frame and motion validity is current; texture restore also needs a one-period field. The content rate is the pulldown film rate, else the measured capture rate, else the nominal rate. Both histories restart (`Forget`) on motion invalidations, a re-base factor change, and when a stage stops running.

### Threading

Render thread only. There are no locks in either module. Detail pulse's pass timers (`SetPassTimers` / `PassTimes`) are a file-static global for tests, one GPU at a time.

### Config keys actually read

From the renderer profile sections that apply (`[vprenderer.Default]`, `[vprenderer.<name>]`, `[vprenderer.input.<name>]`; `ApplyDisplayRuleOverrides`), all validated in RendererProfileConfig.h. An invalid value is logged and the previous value kept.

- `detail_pulse_stabilise`: `on` / `off` (default). Off by default in every profile and Repair tier (owner 2026-11-11 19:39: a temporal-detail precursor, on only by explicit choice; docs: D:\codex\vp-handover\91-design-decisions-by-area.md line 106; log refs\owner-decisions-2026-11-07.md, 11-11 evening).
- `detail_pulse_cap`: 1.0 .. 1.5, default 1.35.
- `detail_pulse_fast`: `on` (default) / `off` (the original passes).
- `texture_restore`: `off` (default) / `statistical`. Off in every profile and Repair tier, including Upper (same decision; on only by explicit choice).
- `texture_restore_cap`: 1.0 .. 2.0, default 1.5.
- `cut_soft_reset`: `on` (default) / `off`. Both modules (`Params::softReset`); off = no soft reset, shown as ", soft reset: off" on the "Detail pulse stabilise: on (cap N)" / "Texture restore: statistical (cap N)" status lines (Ctrl+I processing list). On but inert (review softreset r1 M3, no silent fallbacks): when the last `kSoftResetInactiveGroups` (12) scene groups a module read were all provisional (low latency, a thin lane: a soft reset fires only on a confirmed verdict and is never delivered late), the same lines read ", soft reset: on (inactive: provisional verdicts only)" until the next confirmed group (`SoftResetWatch`, `SoftResetStatusText`; `SceneSoftResetInactive()`, `SceneSoftWaiting()` counts the frames the rule would have fired on had their verdict been confirmed).

The Original pathway sets `detail_pulse_stabilise` and `texture_restore` to `off`. Smooth motion or deinterlacing (motion keys) stops both.

### User-visible fallbacks / notices

Neither module produces a "Fallbacks engaged" entry. The listed `Texture restore: inactive (<reason>)` is a status line, not a FallbackStatus entry:

- Main-path failure: the frame renders without the stage. The renderer's processing list (status) then shows "Detail pulse stabilise: inactive (<reason>)" or "Texture restore: inactive (<reason>)" instead of "on (cap N)" / "statistical (cap N)". The log line is repeated only when the reason changes. There is no one-time notice. The status text is built when the next frame's parameters are set, so it lags by one frame.
- Texture restore needs a YCbCr luma plane: an RGB picture (RGB capture, the subtitle move's composed picture) always reads "inactive (plane 0 is not luma)". Detail pulse handles RGB.
- Failures on the extra and crossfade paths, and on A's history advance for a phase picture, are silent. A failed look-ahead is silent (optional by design).
- While smooth motion or deinterlacing runs, the status says "not applied while smooth motion or deinterlacing runs".

### Dependencies to lift it

libplacebo (compute shaders through `pl_dispatch`) and `pch.h`. `DetailPulse` also needs `src/VideoProcessor-Lib/vprenderer/GrainPreserve.h` and `VpDebander.h` (repair.deband, for the look-ahead), plus the shared unit U3 `src/VP-V/modules/input/source_analysis/lane/shared/HalfLuma.h`: the low band's 2x2 mean (`load_low` sbox and `LOW_BODY_FAST`) is `vpu_half_luma_mean4` (`VpUnit::HalfLuma::GlslMean4()`, injected into the band and low-pass headers); the four taps stay DetailPulse's picture-clamped reads (U3 one copy, bit-identical: x * 0.25 and 0.25 * x are the same product). `TextureRestore` needs neither: it has its own copies of `Smoothstep` and the coding-noise curve. Both are compiled into VideoProcessor-VPRenderer(.Beta) and VideoProcessor-Test. The forwarding headers src/VideoProcessor-Lib/vprenderer/DetailPulse.h and TextureRestore.h keep old includes building until M2.

### Test it standalone

- `DetailPulseTests` (DetailPulseTests.cpp): CPU model tests, plus GPU tests whose names start with `DetailPulseGpu` (D3D11; WARP unless `VP_TEST_GPU=hardware`; `DetailPulseGpuTiming` needs hardware). `DetailPulseRealClipBreathing` needs `VP_TEST_REAL_FOOTAGE`, else it is skipped.
- `TextureRestoreTests` (CPU model; `TextureRestoreRealClipBreathing` needs `VP_TEST_REAL_FOOTAGE`) and `TextureRestoreGpuTests` (GPU, WARP unless `VP_TEST_GPU=hardware`; `TextureRestoreGpuTiming` needs hardware), both in TextureRestoreTests.cpp.
- `DetailRestoreConfigTests` (CPU): key validation, cap ranges, a profile file, and the source-resolution monitor's input left unchanged.
- Through the renderer (`GpuHardware`, skipped without hardware): `RendererCaptureHarnessTests` methods `DetailRestoreKeysReachTheSettings` and `DetailRestoreGpuSourceMonitorUnchanged`.

Run single-threaded (CPU only):

`vstest.console.exe x64\Release\VideoProcessor-Test.dll /Platform:x64 /TestCaseFilter:"(FullyQualifiedName~DetailPulseTests&FullyQualifiedName!~DetailPulseGpu)|FullyQualifiedName~TextureRestoreTests::|FullyQualifiedName~DetailRestoreConfigTests" /Logger:"trx;LogFileName=texture_restore.trx"`

## Settings

| Key | Section | Type | Range / values | Default | Config placement | Original (Bill) | Fallbacks / status | Tiers |
|---|---|---|---|---|---|---|---|---|
| `cut_soft_reset` | profile ([vprenderer.Default] and matching profiles) | bool | on, off | on | VP-V > Motion > Advanced > Scene changes: soft reset | n/a (VP-only): unchanged by the Original pathway (detail pulse and texture restore never run there) | Detail pulse and texture restore shrink their history on a near-cut the scene judge is very sure of (high bar, provisional until the D8 truth run); below the bar nothing happens; a cut still restarts, a fade still shrinks it. Off: status lines read "soft reset: off" | same on all (not tiered) |
| `detail_pulse_cap` | profile ([vprenderer.Default] and matching profiles) | float | 1.0 - 1.5 | 1.35 | VP-V > Repair > Advanced > Texture restore and detail pulse: limits | n/a (VP-only): unchanged by the Original pathway | The most a frame's detail is raised (safety limit) | same on all (not tiered) |
| `detail_pulse_fast` | profile ([vprenderer.Default] and matching profiles) | bool | on, off | on | VP Renderer > Rendering > Experimental content-adaptive settings; also VP-V > Proposed retirement > VP-V switches proposed for retirement (retirement-C toggle) | n/a (VP-only): unchanged by the Original pathway | The same pulse in about half the GPU time (fewer passes); off = the original passes, for A/B. proposed for retirement (group C); status line: Texture restore: inactive (<reason>) | same on all (not tiered) |
| `detail_pulse_stabilise` | profile ([vprenderer.Default] and matching profiles) | bool | on, off | off | VP-V > Repair > Advanced > Set by the Repair tier | Original pathway sets `off` (ADR-0019) | Fine detail and grain "breathe" with the encoder's group of pictures: scales each frame's own detail towards its best-coded recent neighbour (never beyond it, capped by detail_pulse_cap); runs after VP's debander before scaling | Repair Classic=off; Repair Low=off; Repair Mid=off; Repair Upper=off |
| `texture_restore` | profile ([vprenderer.Default] and matching profiles) | enum | off, statistical | off | VP-V > Repair > Advanced > Set by the Repair tier | Original pathway sets `off` (ADR-0019) | Experimental: separates grain from scene texture per area and raises both towards the best-coded reference along the motion (about 2 s memory); high GPU cost | Repair Classic=off; Repair Low=off; Repair Mid=off; Repair Upper=statistical |
| `texture_restore_cap` | profile ([vprenderer.Default] and matching profiles) | float | 1.0 - 2.0 | 1.5 | VP-V > Repair > Advanced > Texture restore and detail pulse: limits | n/a (VP-only): unchanged by the Original pathway | The most texture is raised (safety limit) | same on all (not tiered) |


Dependency: `detail_pulse_stabilise` and `texture_restore` keep their history only while the skeleton can answer the frame's scene (its get-or-compute reader `SceneOf`: the store's group, or a live classification with `frame_analysis_store` off; C5). Without an answer (no skeleton module, analysis lane off, no sealed P4 pair) every frame restarts (gain 1), visibly through the "<module> scene judge" fallback (see "Scene judge" below).

- Existing keys: `texture_restore`, `texture_restore_cap`, `detail_pulse_stabilise`, `detail_pulse_cap`, `detail_pulse_fast`, `cut_soft_reset`
- Planned keys: (none)
- Tiers: Upper only (after the efficiency pass)

## Fallbacks

- "Fallbacks engaged" entry: none produced. `Texture restore: inactive (<reason>)` (and `Detail pulse stabilise: inactive (<reason>)`) appear in the status line only, not under "Fallbacks engaged".
- Behaviour: Failure renders without it (counted in the status line on the main path; silent on the extra and crossfade paths).
- Exception, the scene judge: a frame the skeleton's scene product cannot answer (e.g. the analysis lane off; `frame_analysis_store` off alone is answered live) engages the FallbackCatalog entry "<module> scene judge" (SceneJudgeUnavailable; Ctrl+I / Config status) and is counted by `SceneMissing()`.

## Net fingerprint / upstream

- Fingerprint: on/off is an unlabelled robustness input to the refine net
- Upstream modules: `repair.encoding`, `repair.deband`, `motion.estimation`

## How to build / test

Build the solution via the shared build-slot runner (see AGENTS.md); tests single-threaded; GPU tests only on mains with `VP_TEST_ADAPTER=NVIDIA`.

Placeholder tests:
- (none yet)

Isolation tests:
- TextureRestoreGpuTiming
- DetailPulse tests
- SoftResetTests (`tests/SoftResetTests.cpp`, CPU; WAIVER-C5-SOFTRESET-ON, 2026-11-15): `cut_soft_reset` default on, off unchanged, on only at the bar on a confirmed candidate, never on cut-free clips, keep 0.5, cut restarts, the off / inactive status text

Contract tests:
- never runs with libplacebo smooth motion / deinterlacing

Chain tests:
- stacked A -> texture restore (double-boost check)

## Known issues / open items

- efficiency rebuild (fused passes, shared-memory tile stats)
- redesign to measure in the A-cleaned domain

## Where the code lives (live file list)

Code location: **migrated**. The authoritative list is `live_files` in [MANIFEST.json](MANIFEST.json).

- `src/VP-V/modules/repair/texture_restore/DetailPulse.cpp`
- `src/VP-V/modules/repair/texture_restore/DetailPulse.h`
- `src/VP-V/modules/repair/texture_restore/TextureRestore.cpp`
- `src/VP-V/modules/repair/texture_restore/TextureRestore.h`
- `docs/VP-detail-restore.md`

Forwarding headers at the legacy path (removed in M2; they only keep files on active branches building, new code includes the `<VP-V/...>` path):

- `src/VideoProcessor-Lib/vprenderer/TextureRestore.h`
- `src/VideoProcessor-Lib/vprenderer/DetailPulse.h`

The call-site glue (`Segment::Detail`, `Segment::DetailLookAhead`) is in the entrance segment, `src/VP-V/segments/EntranceSegment.cpp`. The per-frame parameters are set in `src/VideoProcessor-Lib/vprenderer/LibplaceboVideoRenderer.cpp`, which owns the modules.

## Migration directive

Code moves into this folder only when this slot is touched (opportunistic until M1, a dedicated phase after M1, finished by M2). A move = `git mv` + project/include updates + this module's `live_files` + `docs/design/components-master.json` (regenerate with `python tools/check_module_manifests.py --write`) in the SAME commit, with the build, this module's tests and the chain-gate subset passing. Never leave a copy behind; one module per change; never during a release/deploy window. Update this README in the same commit as any slot change.

## Links

- Design: [VP-pipeline-final.md](../../../../../docs/design/VP-pipeline-final.md)
- Slot data: [slots.json](../../../../../docs/design/slots.json)
- Decisions: ADR-0009 in [docs/decisions](../../../../../docs/decisions/README.md)

<!-- vpv-settings-tests:begin (generated by tools/vpv_tests/seed_module_readmes.py) -->

## Settings

- Keys: `texture_restore`, `texture_restore_cap`, `detail_pulse_stabilise`, `detail_pulse_cap`, `detail_pulse_fast`, `cut_soft_reset`.
- Tiers: none. Both stages are off in every Repair tier (Classic, Low, Mid, Upper) from 2026-11-11 (owner decision, doc 91 line 106); on only by explicit choice of the key.
- VP-V page, Repair tab: tiers Classic / Low / Mid / Upper; tier keys `deband_vp`, `detail_pulse_stabilise`, `texture_restore`.
- Listed under the tier: `detail_pulse_fast`.
- Plain-language guide: [docs/settings/VP-settings-reference.md#4-repair](../../../../../docs/settings/VP-settings-reference.md#4-repair).
- Fallback entry ("Fallbacks engaged"): `Texture restore: inactive (<reason>)`.

## Tests

- Placeholder (byte-identity on/off, zero cost): n/a.
- Isolation (fixed inputs, expected outputs, cost): TextureRestoreGpuTiming; DetailPulse tests.
- Contract (formats, geometry, fallback entry): never runs with libplacebo smooth motion / deinterlacing.
- Chain (total delay at the 4/3/2-frame budgets): stacked A -> texture restore (double-boost check).
- Combinations: pairwise / high-risk / random lines in `tests/vpv/` that set this slot's unit (`tools/run_combinations.ps1`); matrix: `docs/design/slot-test-matrix.md`.

<!-- vpv-settings-tests:end -->

## Scratch arena (SA1)

- Temporaries from the arena (when Params::arena is set): Apply's `half` (one float-1 texture, half-resolution) and `tmp` (one float-2); ApplyForeign's trio `fHalf`, `fTmp`, `fLow` (one float-1, two float-2, half of the foreign dims). Formats are the ones the module already uses (FloatFormat), mapped with PlTexBackend::FormatOf; `TextureRestore::ArenaRequests(gpu, planeW, planeH, foreignW, foreignH)` lists them.
- Scope ids: `kArenaScopeApply` 0x0101 (owner `texture_restore`), `kArenaScopeForeign` 0x0102 (owner `texture_restore_foreign`). Tier T1: shed by the governor before T0.
- Fallback: no scratch texture (arena short, plan empty, tier shed) = "scratch memory short": that frame is rendered without texture restore (Apply returns false, ScratchShort() true, the failure counts). It is visible through the FallbackCatalog entry `scratch_arena` and the existing texture restore entry (TextureRestoreFailed). The failure restarts the references, so the module recovers by itself on the first frame after the arena returns.
- Off = byte-identical: with Params::arena null the code path and the private textures are exactly as before; with an arena the output is byte-identical (test TextureRestoreGpuArenaIsByteIdentical).
- Persistent state (output, low bands, repeat bands, tile statistics, grain, references, gains, state, foreign output) stays private: it is history or the returned output and migrates in SA6/SA7/SA8/SA12.
- Host call site to wire (hook layer, NOT done here): the renderer owns one `ScratchArena<PlTexBackend>`, reserves `PlanArena(requests, tier)` with the tier from the governor grant of `kUserScratchArena` (via ArenaGrantFor), calls `BeginFrame()` per frame, and passes the arena in `Params::arena`.

## Private GPU textures (skeleton rule)

This module still creates its own GPU textures (see the static test src/VP-V/tests/PrivateTextureScanTests.cpp). Its per-frame half-resolution temporaries (half, tmp, and the foreign-geometry trio) come from the shared scratch arena when Params::arena is set (SA1); its persistent state (low bands, tile statistics, references, gains, output) stays private and migrates in SA6/SA7/SA8/SA12.

## Scene judge (detail pulse and texture restore)

- Decision (L-B6, WAIVER-B6-D8-MODELS; source: lead 9 ruling on D8 and audit-L-B6 SHOULD 8; 2026-11-14T13:25Z): both modules read the shared cut/fade verdict (U7, the frame analysis scene group via `SceneReader`); their private tile-change laws are deleted and kept only as the test reference `TileJudgeReference`. The scene group is read with Peek, so with `frame_analysis_store` off, or a frame whose group is not sealed, there is no verdict: the references restart every frame (visibly: the "<module> scene judge" fallback, Ctrl+I). Detail pulse and texture restore therefore need `frame_analysis_store` on to keep their history. D8 is live: the shared-judge read is unconditional (DetailPulse passes `scene_restart` to its shader, TextureRestore calls `SceneRestartOf`) and there is no parking switch. The retired tile law's vector veto (the field's untrusted share) has no equivalent yet: a reader-selectable vector veto inside U7, fed from P7 / the engine memo trust, is a later addition (refs/102b row B6-D8-VECTOR-VETO; trigger: L-B4's P7 integration lands). Until then DP/TR may restart on a pan with a large histogram change where the tile law did not; the FIX-B6-D8 pan rows measure it.
- Decision (C5, owner 2026-11-14 17:38 "Ask the skeleton for it same as everything else"; gate C5 prequal x5dup MUST 1, extended by lead 18 to DP/TR): the Peek above is replaced. The production reader (`VpDetailRestore::ReadSceneFromSkeleton`, the default of `DefaultSceneReader()`) asks the skeleton's get-or-compute-and-publish reader `SkeletonModule::SceneOf` (through `Vpv::Skeleton::SceneGroupOf`, SkeletonAccess.h) for the renderer this thread renders: the store's scene group when it is there, else the skeleton classifies the frame now and publishes it. With `frame_analysis_store` off a cut therefore still restarts and a fade still gets its partial reset, as long as the analysis lane runs. A frame the skeleton cannot answer (no skeleton module / Original pathway, analysis lane off, no sealed pair of P4 records) still restarts, visibly, with the cause in the "<module> scene judge" fallback. `ReadSceneFromStore` (a store Peek) remains only for the test harnesses that drive a bare store without a skeleton module. Tests: `U7FadePartialResetTests::StoreOffACutStillRestartsThroughTheSkeleton` (WAIVER-C5-SCENEOF-DPTR).
- Decision (C5 batch B, `cut_soft_reset`; owner 2026-11-15 08:35 EST U7 Q4 "the bar fairly high", "below the bar no reset"; plan lead19\c5-softreset.md items 1-4): `SceneRestartOf(sequence, reader, softReset)` with `softReset` on (the modules pass `Params::softReset`; the argument's default stays off for the CPU models and harnesses) fires a soft reset on a frame that is no restart and no fade when `VpUnit::CutFadeJudge::SoftResetStrength` of its group is > 0 (confirmed, candidate, not cut / cutHere / flash / fade, `cutStrength >= kSoftResetBar`): `softPartial`, `strength` = that strength, `historyKeep` = `SoftResetKeep(strength)` = 1 - 0.5 s. Both modules apply it through the existing fade-keep multiply (`fade_keep2` = keep squared; no new pathway) and count it apart (`SceneSoftResets()`). A provisional group never fires. The bar 1.0 is the highest a strength takes (only a candidate vetoed right after a flash whose residual fully passed the spike bar can fire): PROVISIONAL until the D8 truth run (D8PARTIAL "soft" fields). Temporal detail does not read `SceneRestart.h` (its own keep per block from the scene product): not wired here.
- Decision (C5 batch B fix, review D:\codex\vp-handover\batchB\review-softreset-r1.md M3, 2026-11-15; core rule "no silent fallbacks", doc 91 section D): because a provisional group never fires and a soft reset is not delivered late (unlike the cut), a lane whose groups stay provisional at render leaves the switch on but inert. That state is shown, not hidden: `SceneRestartAnswer::provisional` / `softWaiting`, each module's `SoftResetWatch` (12 consecutive readable provisional groups enter "inactive", the first confirmed group leaves it, so a lone provisional group does not flip the line) and `SoftResetStatusText`, which writes ", soft reset: on (inactive: provisional verdicts only)" in the same place and style as ", soft reset: off". Delivering a soft reset one frame late (the alternative the review names) stays an owner question, not taken here.
