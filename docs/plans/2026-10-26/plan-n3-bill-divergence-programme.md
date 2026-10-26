# N3: programme to bring Bill's files to the end state (needs assessment + plan)

Read-only plan, 2026-10-26 (sonnet draft, Opus review follows). Tree: integ/batch1 d85519e2. Bill: origin/v1.4.02-beta, **now 81ac960f** (the tip moved since the first pass: 156 commits ahead of integ's merge-base view, was 153).
Inputs: divergence-sweep-firstpass.md, upstream-divergence-register-DRAFT.md, plan-review-consolidated.md (section 0), plan-f11-triage-and-small-items.md. Numbers below were re-measured on d85519e2 with three-dot diffs; cluster splits inside the big files are +-25% (first-pass method, not a hunk table; package P0 produces the exact table).

End-state rule (owner): in Bill's files only (a) host call sites, (b) registered bug fixes worth a PR, (c) include/registration changes, (d) either/or hooks where Original = Bill. Everything else moves to src/VP-V/modules behind a hook, or reverts.

## 1. Needs assessment

### 1.1 Totals (re-measured)

| Measure | Value |
|---|---|
| Bill files modified by integ (three-dot, M only) | 178 files, +44,206 / -7,937 (was 181 / +57,079: the 136 tests moved out of LibplaceboLutParserTests since) |
| Of these, also changed by Bill since merge-base (against the new tip) | 73 files (first pass said 74) |
| Textual merge conflicts of integ with Bill's tip (`git merge-tree`, old 3-arg form, no writes) | **124 conflict hunks in 39 files**: LibplaceboVideoRenderer.cpp 40, ConfigEditorWindowTests 18, ConfigEditorWindow.cpp 15, SubtitleCutPasteShader.h 4, rest 1-3 each |
| Host renderer size | LibplaceboVideoRenderer.cpp 31,476 lines (Bill base 20,771, Bill tip 25,669 after his own +6,890-line changes) |

Class mix by lines (from the first pass, adjusted for the test move): e1 about 80% of added lines, (b) about 4k, (a) about 1.5k, (c) about 1k, (d) about 0.2k. End-state estimate: Bill's host .cpp diff falls from +11,073 to roughly +1,500-2,000 (650 call sites, 150 Original-pathway gates, 250 (b) fixes, ~300 hook seats); VideoProcessorDlg.cpp from +3,001 to ~1,300 (mostly (b) shortcut/retry fixes); ConfigEditorWindow.cpp from +7,089 to ~900.

### 1.2 e1 per Bill file: how much, which feature, hook state, and what is already planned

"Planned" = already owned by WP-9/10/16/18, F-11, S1, step 0A/0B, M2 tail in plan-review-consolidated.md section 4. Those lines are NOT counted in this programme's hours.

| Bill file (diff) | e1 cluster | ~Lines | Feature / module | Hook or adapter today | Planned item (no double count) | Residual for N3 |
|---|---|---|---|---|---|---|
| LibplaceboVideoRenderer.cpp (+11,073/-368) | de-judder and B-frame planes (5509 +1635, 29663, 30116) | 1,900 | motion.consumers, entrance stages | EntranceSegment (S1, merged), OutputSegments | WP-9.0/9a/9b/9c, WP-9.6 | 0 |
| | intent interpreter (15395, 15575 +2132) | 2,300 | control.config/intent | none (IntentHost planned) | WP-16 | 0 |
| | settings parse, fingerprint, transport (1049, 2661, 3494, 11732, 11854) | 1,300 | control.config | VpvSection/tier tables exist; no parse seam | WP-18 owns tables + golden fingerprints; the code move is NOT stated in WP-18 | ~650 (move of parse/fingerprint code behind one `VpSettings` call) |
| | NVOF depth + vector delivery (22459, 13023) | 540 | motion.estimation | vector_delivery module exists | WP-10 A_motion (part), WP-9 | ~270 |
| | custom GLSL hook chain (7325 +394, 23296 +515) | 900 | output/colour segments | OutputSegments, OutputSegmentsMerge (merged) | none | ~600 |
| | render-ahead and queue/anchor (4716, 10307, 10420, 24122 + WaitForFileFrameSlot, HoldTicks*, UpdateQueueDelayHold, PrefillTarget ...) | 800 | output.presentation | planner/gate in module (RA commits); host holds queue code | none | ~800 (and it collides with Bill's DisplayVsyncClock, see 3) |
| | ABI-37 file frames (25570 +301, 29563-29792) | 600 | input.capture (VP Renderer filter) | none | none | 600 |
| | DV GPU unpack/tunnel/repack, hdr_inferred pushes (18761-19115, 13348-13923 share) | 450 | input.signal | DvHostStage, DolbyVisionMetadataExtension (m2-dv/group D) | F-11 covers the formatter, not the host stage | ~300 |
| | subtitles / active picture / look-ahead walk (27938 +190, 29130 +134) | 450 | input.source_analysis | WP-23 FrameAnalysisStore, adapters | none | ~350 |
| | report-a-problem rows, status getters, tests-only seams (24451, 679, 5295, 31225 +167, GetCalibration*, *ForTest) | 600 | control.diagnostics | WP-17 adapters | none | ~500 (many are test seams that should move behind a test-friend header) |
| | chroma siting, misc (19219 ...) | 100 | colour | core/ChromaSiting.h exists | step 0 | ~50 |
| | includes of VP modules | 100 | (c) | n/a | step 0A/0B | 0 |
| | **Host .cpp e1 total** | **~9,200** | | | covered ~5,500 | **~4,100** |
| LibplaceboVideoRenderer.h (+220) | members for the above | 220 | | | follows the .cpp | follows |
| VideoProcessorDlg.cpp (+3,001/-476) | report-a-problem/state dump (8808, 9384, 14295) | 300 | control.diagnostics | VpStatusAdapter (gui) | none | 300 |
| | LLDV/DV hand-off, HDR inference pushes (5678, 13348-13923) | 600 | input.signal | hdr-inference callsite patch (F-8) | none | ~450 |
| | Config RPC, profile dispatch, queue-profile switching, renderer combo (3244, 15141, 13053-13086) | 700 | control.config | none | none | ~600 |
| | overlay/status poll, fullscreen helpers | 100 | (a) | | | 0 |
| VideoProcessorDlg.h (+287), VideoProcessorApp.cpp/.h (+270) | members, command-line VP keys, crash state | ~350 e1 | control | none | none | ~300 |
| ConfigEditorWindow.cpp (+7,089/-350) | VP pages and controls (profile pages +2196, Actions/LED +2065, Startup, motion/detail/intent groups, shaders, DeckLink, grain) | 5,600 | config UI | VP-V/config (VpvSection, tier tables; 3,879 lines) already hosts the pipeline page | "Config org" is a queue item, not in the 20-item list | ~5,000 |
| | shortcut canonical spelling, controlName/key table (573, 1118) | 300 | gui | VpShortcuts.h | none | 300 |
| | misc (RS-232 test, placement clamp, context menu) | 250 | | | | 250 |
| | load-time user-file migrations (e2, see 2) | 270 | | | | 270 |
| ConfigEditorWindow.h (+65), AppMain.cpp (+49), ConfigurationRpcClient/Protocol/Server (+110) | members, sandbox, Calibration RPC | ~200 | | | | ~200 |
| DV formatter family (.cpp +1,085, .h +104, AVX2 +59, IVideoFrameFormatter.h +97, Noop +7) | class 2 ~535 + h~55 + 97 | ~700 | input.signal | DolbyVisionMetadataExtension (group D) | **F-11 (item 3)** | 0 |
| AnalysisLumaSource.cpp/.h (+456), AnalysisFrame.cpp/.h (+56), SubtitleCutPasteShader.h (+50), DolbyVisionLuminancePolicy/Status (+42) | packed-DV sampling 256, DV raw-word route, rgba32f composite, sceneMin | ~420 | input.source_analysis, colour | SampleLuma plane (WP-23, offered upstream) | WP-23 done; the DV additions not | ~400 |
| RendererProfileConfig.h (+395/-20), MainConfigSchema.h (+266/-96), UnifiedProfileRuntime.cpp/.h (+166), CalibrationProfileMigration.h, ProfileChangeOverlay.h, MotionConfig.h | VP keys and profile runtime | ~800 | control.config | tier tables, slots.json | **WP-18** (key tables) covers ~500; session-override groups, NVOF look-ahead rule, [vpv] section, -96 refactors do not | ~300 plus verification that the -96 lines are neutral |
| DisplayRuleExpression.h (+92/-19), StateVariables.cpp, MadVRShaderLoader.h/.cpp (+112) | new variables (ambient/content/source/hdr_inferred), cinemaOnly | ~120 | control | none | none | ~120 (variable-provider hook, PR-able) |
| LibplaceboRenderParameters.cpp/.h (+117) | SDR ambient lift, deband grain, 16-bit dither rule | 117 | colour.appearance, repair | SdrAmbientLift extraction | M2 tail step 7 (done in part: extracted) | ~60 |
| Capture backends: VideoState.h/.cpp, VideoFrameEncoding.h/.cpp, Noop formatter, DeckLink device/translate (12-bit route) | NV12/P010/P016 (ABI 37), hdrInferred/dolbyVisionVsif, 12-bit route | ~250 | input.capture | none | none | ~250 |
| IRenderer.h (+193), LibplaceboRendererPluginApi.h, PluginVideoRenderer .cpp/.h, RendererId, LibplaceboPluginExports | new virtuals/forwarders | ~550 | (a) | these ARE the call-site surface | n/a | 0 (optional trim, see P7) |
| StatsOverlayWindow, ConfigurationLiveApply (VP part), ActiveProfileStatus | status rows | ~150 | (a) | | | 0 |
| Tests (VP content inside Bill's files) | ConfigEditorWindowTests +9,111/-5,577, ConfigFileTests +2,074/-8, DolbyVisionTunnelFormatterTests +1,356/-206, BetaParametersTests +153, MotionRendererTests +69/-1, LibplaceboPluginProxyTests +257 (a), MagewellCaptureTests +137 (b) | ~13,000 | | | G6/B1 (item 18) did LutParserTests only; formatter tests follow F-11 | ~12,000 |

Residual e1 (not owned by any planned item): host .cpp ~4,100, Dlg+App ~1,350, Config UI ~6,000, analysis/DV ~400, rules ~120, capture ~250, profile/schema ~300, tests ~12,000. About **12,500 non-test lines** and **12,000 test lines** to extract. Planned items already cover about 9,000 non-test lines (intent 2,300, de-judder 1,900, settings tables ~650, formatter ~700, NVOF ~270, DV host ~150, ...).

### 1.3 Biggest files, in order of the work they need

1. LibplaceboVideoRenderer.cpp: ~4,100 residual e1 plus 40 merge conflicts (P8).
2. ConfigEditorWindow.cpp (+ ConfigEditorWindowTests +9.1k/-5.6k): ~6,000 residual and the largest rebase hazard (P10, P11).
3. VideoProcessorDlg.cpp/.h: ~1,350 (P9).
4. DV formatter family: F-11, nothing added here.
5. AnalysisLumaSource/AnalysisFrame (P4), RendererProfileConfig/MainConfigSchema (WP-18 + P6).

## 2. e2 items: behaviour/default divergences, proposal, owner list

Finding that shrinks this list: `OriginalPathway.h` (62 entries) already forces nls_sampling=bilinear, queue/rate/dv fixes off, fbo_float32 off, hdr_inference original, interpolation off, calibration_lut_stage libplacebo. It does NOT contain these Bill-key-or-default divergences: switch_refresh_rate, target_frames, global_hotkeys, motion_engine, ycbcr_12bit_capture, capture_display_fixes, magewell_recover_faults, plane_upscaler default. Closing that gap is the cheapest large step (P1).

| ID | Divergence | Where | Proposal | Owner list # |
|---|---|---|---|---|
| E2-01 | `switch_refresh_rate: fullscreen_only` (Bill: true) | VideoProcessor.cfg, VideoProcessorDlg.cpp (new value) | either/or: new value stays, Original sets `true`; shipped sample keeps VP default | 1 |
| E2-02 | `target_frames: 5` (Bill: 3) | VideoProcessor.cfg | Original sets 3; VP default 5 (look-ahead needs it) | 2 |
| E2-03 | new `[vprenderer.HDR] deband: off` rule | VideoProcessor.cfg | data only; keep as VP default, Original: sample loaded without it (profile data, document) | 3 |
| E2-04 | `global_hotkeys` default off | ConfigurationLiveApply.h | add `global_hotkeys on` to Original | 4 |
| E2-05 | `motion_engine auto` (smooth_motion vs VP) | MotionConfig.h | add `motion_engine smooth_motion` to Original; verify `auto` | 5 |
| E2-06 | NLS `sampling` default 1.0 Catmull-Rom | shaders/NLS*.glsl | Already either/or (Original sets bilinear): close, only confirm byte-identity | 6 |
| E2-07 | DeckLink `ycbcr_12bit_capture` default v210 (Bill refuses) | DeckLinkTranslate.h | Original sets `off` | 7 |
| E2-08 | `ConfigFile::Load` calls RefreshKey::Apply; PPM keys parsed as doubles; FindBestMatch rewritten (-48); Dlg lookups | ConfigFile.cpp, PPMCorrectionLoader.*, Dlg | revert in Bill's loader; VP wrapper (VP-V config load adapter) applies RefreshKey and double keys for VP callers; Bill's readers stay int | 9 |
| E2-09 | refresh bounds 10-240 -> 10-500 Hz, delivery hold 1.15x interval, log cadence in seconds (10 files, DIV-RATE) | Lib/*Rate*, CBufferedLiveSource..., DirectShowVideoRenderer | gate under existing `general_rate_fixes` (Original off); no new switch. Or offer as a "rate-independent" PR if owner wants | 9 |
| E2-10 | Config editor migrates the user's file on open (empty header removal, queue ownership, refresh-key convention; ~270 lines) | ConfigEditorWindow.cpp | move into VP-V config module, run only for VP keys/pages, never silently rewrite Bill's keys; Original: off | 10 |
| E2-11 | AnalysisLumaSource::Sample maps full-range to limited codes; 5 Bill tests edited | AnalysisLumaSource.cpp, 5 tests | revert Bill's Sample; VP wrapper does the mapping for VP detectors; restore Bill's tests as the oracle. Keep as (b) only if a repro exists | 11 |
| E2-12 | Shipped sample cfg: zoom profiles 16x9/Scope (F2/F3), `[vprenderer.motion]` renamed `.Default`, removed/moved hdr_peak_analysis keys | VideoProcessor.cfg | needs per-key check: renames Bill's profile names (a user's file keyed on them would break). Revert renames unless VP code requires; the sample file itself is class (f) fork distribution file | 12 |
| E2-13 | Config pages reorganised; 5,577 lines of Bill's page tests deleted/rewritten | ConfigEditorWindow.cpp, ConfigEditorWindowTests | restore Bill's pages/test file as oracle, add VP pages through a page/group contributor hook; if Bill's tests expose real layout changes, each is either VP page addition or reverted | 12b |
| E2-14 | MotionRendererTests deletes `plane_upscaler == bicubic`: a default under motion projection changed | MotionRendererTests, projection params | find the changing commit; make it selectable (Original = bicubic) or revert | (technical) |
| E2-15 | restore switches `magewell_recover_faults`, `capture_display_fixes` missing from OriginalPathway.h (dv_tunnel_fixes, queue_timing_fixes are there) | MagewellBackend, DisplayTopologySession | add both to Original; keep switches (consistent with the F-11 ruling: keep, never in a PR) | (technical) |
| E2-16 | FallbackNotice log-only edits (AlphaSourceCropPolicy, ActivePictureTransitionModel, BufferedPictureExpansion) | 3 files, 15 lines | keep as (a) hook calls (silent-fallbacks rule); log-only violates "never log-only": confirm each also reaches the Ctrl+I/Config status | (technical) |
| E2-17 | WindowResizeLayoutTests 1350 placement, AnalysisDiagnosticReplayTests +1 | tests | test-environment hardening: keep as (b) test fix or revert; low value | (technical) |
| E2-18 | e3: patched libplacebo-377.dll/.lib, IMediaSideData.h re-vendored, AGENTS.md, packaging scripts, build identity | 3rdparty, tools, docs | new class (f) fork infrastructure: listed in register, not reverted; BTB/WTW rule decides the DLL (keep). `tools/test_libplacebo_variants.py` asserts API 26 while plugin API is 44: fix | rule, not owner |

### Owner list (plain language; yes = keep it as the VP default and Bill's way stays selectable under "Original"; no = go back to Bill's behaviour)

1. Switch the screen's refresh rate only when playing full screen (Bill: always). Keep?
2. Hold 5 frames ahead of the picture (Bill: 3); it costs about two frames of extra delay and feeds the motion look-ahead. Keep?
3. Turn off libplacebo's debanding on HDR pictures (the bright-speckle bug you measured). Keep?
4. Shortcut keys work while VP is in the background only for Ctrl/Alt/F-keys (Bill: every configured key works everywhere). Keep?
5. "Auto" motion lets VP's own motion code replace libplacebo's smooth motion (Bill: libplacebo's). Keep?
6. NLS scaling reads the picture with the smoother Catmull-Rom curve (Bill: plain bilinear). Keep?
7. DeckLink 12-bit YCbCr signals are accepted and shown (Bill refuses them). Keep?
8. Per-refresh-rate tables treat 59.94 and 60 as different, and rates up to 500 Hz are allowed (Bill: 10-240 Hz, 59.94 = 60). Keep?
9. The Config window tidies your settings file when you open it (removes empty headers, converts old queue and refresh-rate keys); Bill's leaves the file alone until you change something. Keep?
10. Edge/subtitle detection looks at full-range pictures after converting them to video levels (Bill: raw values). Keep, or go back unless I can show a real error?
11. The sample settings file's shape: new Zoom profiles (16x9, Scope on F2/F3) and the motion profile renamed "Default". Keep?
12. The Config window's pages are laid out differently from Bill's. Keep VP's layout, or restore Bill's pages and add VP's as extra pages/sections?

Count: **12 owner questions**. Items 1-7 only need Original-pathway entries (cheap, no code risk); 8-12 change code or layout. Without answers, P1 proceeds on the recommended path: items 1-7 yes (either/or), 8 either/or under `general_rate_fixes`, 9 and 10 revert to Bill + VP wrapper, 11 revert renames only, 12 restore Bill's pages (the safest oracle) and extract.

## 3. Bill's newer commits

Bill's tip (81ac960f): 156 commits since merge-base, 224 files, +51,336/-2,871; 25 first-parent PRs (#167-#204). Facts from checking the tip against our register:

- **Already fixed upstream (drop ours / take his text):** DIV-TIM-01 for VideoTimingController.cpp and DirectShowTimingClock.cpp (PR #189 "madvr-clock-overflow"). Still ours alone on the tip: WallClock.cpp QPC*1e7 overflow, IntegerMath/ALiveSource pin, ACaptureDevice.h and VideoState m_refCount (uninitialised on his tip), DisplayMode.h `NanosecondsPerFrame() const {}` (empty body on his tip), source_rate floor in DisplayRuleExpression.h (still floor on his tip), TimingClockDiffMs, TryGetHardCapacityString, QueueConfiguration. Bill already has `CaptureCallbackBoundary.h` in the merge-base and uses it; DIV-CAP-01 must be diffed against that.
- **Overlap with our (b) rows, re-evaluate after merge:** DebugLogAsync.h plus DebugLogRetention include on his tip (DIV-LOG-01 rollover may be redundant); DisplayMonitorInventory in DisplayTopologySession (DIV-DISP-01); DisplayRefreshRatePolicy.cpp +374 (DIV-RATE gate); LutParser/LUT explicit Apply #191 (touches LibplaceboDisplayLut, LVR -86/+, IRenderer +4, PluginApi: our DIV-D-05 CalibrationLutHostPolicy default "Bill's 1 s probe" is no longer Bill's behaviour: Original must now select explicit Apply).
- **Capture backends, DV formatter, Magewell, DeckLink translate, ColorSpace: untouched by Bill** (formatter only +1 line each in CR210.cpp and IVideoFrameFormatter.h for sceneMinPq). These clusters merge clean and their (b) rows remain valid.
- **Hot spots where he changes the same code we extract:** `LibplaceboVideoRenderer.cpp` (+6,890 changed lines on his side: #198 display-vsync +499, DisplayVsyncClock/DisplayFrameCache, #204 nls-vsync, #202 Bt2020 worker, #191 LUT, #199 promote-vp-renderer touching RendererId.cpp, ConfigEditorWindow, VideoProcessor.cfg renderer default); ConfigEditorWindow.cpp (+924) and its tests (+696); AlphaSourceCropPolicy.cpp (he rewrote ~2,100 lines), TemporalRenderer.h (+759: our HistoryFrame hook), StatsOverlayWindow (+758), ActivePictureEvidence (+528), SubtitleCutPasteShader.h (+298).
- **Semantic (non-textual) conflicts to expect:** our render-ahead/queue/anchor pacing vs his DisplayVsyncClock and presentation queue; ABI counter (we 28->44, he bumped too: IRenderer/PluginApi conflict is mechanical but the number is a policy); OriginalPathway.h must be re-validated against his new defaults (#199 changed the default renderer text; subtitle default-off #171 etc.); MotionRendererTests (his +74 and +82 vs our +69/-1).

### Recommendation: do the extraction first, the Bill merge after the big test, with a read-only rehearsal now

Why extraction first:
- 124 conflict hunks today, 40 of them in the host renderer inside VP code that we will move anyway. After extraction the host diff is ~1,500-2,000 lines of hooks/(b) fixes and the conflict count should fall by roughly two thirds. Merging first means resolving the big files twice (once now, once as code moves).
- Moving code out of Bill's files first, with Original = Bill byte-identical, gives a precise "what we still differ by" so the merge is a merge of small hook edits.
- The big test (WP-20) is on the VP tree; merging 156 commits first would put his changes into the same test, cloud the evidence, and delay it.

Why not just wait: his tip moves ~10 PRs a week in the same files. Mitigation: P0 does a throwaway rehearsal merge (separate worktree, results not kept) to (1) fix the (b) list, and (2) mark "do not touch before merge" regions: render-ahead/queue/pacing (Hc), PreOutput/present path near DisplayVsyncClock. Decide Hc after the merge.

Order: P0 rehearsal and re-sweep, extraction P1-P9 (the hot Hc deferred), WP-20 big test, **WP-21 Bill merge** (10-12 h, mostly LVR/Config), a reduced second gate (chain gate + pacing + Original-pathway byte-identity vs his tip build), then P10/P11b (Config/test moves, no render risk). If extraction slips past about two weeks, switch to: merge first on a scratch branch, extract on top (accept the double resolution).

## 4. Programme: ordered packages

Hours = agent wall-clock; tokens: small <300k, medium 300-600k, large >600k. S = sonnet drafts from a written spec, O = opus design/judgement/review. Every package ends with one independent read-only audit (rule), listed as A (~0.5 h, 120k, O) and counted in section 5, not repeated per row. Verification keys: **B** = Original pathway byte-identical to Bill (build of his tip as oracle, capture-harness 0-code compare), **S0** = slot/hook uninstalled (null extension) identical to today, **CPU/Cfg/GPU** = test legs.

| # | Package | Scope and hook points to add (generic, PR-able where noted) | Deps in existing plan | Hours | Tokens | Model | Verification | Risks |
|---|---|---|---|---|---|---|---|---|
| P0 | Re-sweep and tooling | Throwaway rehearsal merge with 81ac960f in a separate worktree (read-only result); script (git diff -U0 + provenance regex) producing the exact per-hunk table for all 178 files, with class labels and coverage by planned item; refresh the register against his tip (drop fixed rows, rows to re-check); freeze list for hot regions | none; first | 3 | 300k (medium) | S script, O labels and review | table totals equal numstat; register diffed | the +-25% numbers move |
| P1a | Original pathway closes the default gaps (E2-01..07, 14, 15) | Extend OriginalPathway.h (62 -> ~72 entries), slots.json, ApplyOriginalPathwaySettings; `switch_refresh_rate` value stays an extension; golden-fixture test (owner's madVR-derived settings in Bill's context) | after the owner list (or recommended path); before WP-18 key freeze | 5 | 400k (medium) | S entries/tests, O semantics | B on 8 new keys; CPU + Config gate | Original gets no effect for keys read outside the pathway applier (check each reader) |
| P1b | Reverts and Bill's tests as oracle | E2-08 (ConfigFile/PPM revert + VP wrapper in VP-V config adapter), E2-09 gate under general_rate_fixes, E2-11 AnalysisLuma revert + VP wrapper, E2-14/16/17; restore Bill's edited assertions in AnalysisRecoveryParity/Dv, NativeRgbAnalysis, SubtitleBoxDetector, SubtitleOpaqueCard, MotionRenderer, WindowResize, ConfigFileTests -8 | P1a | 6 | 500k (medium) | O wrapper design, S test restores | Bill's tests pass unchanged against Original; CPU | wrapper must reach every VP caller (Dlg rate lookups, DirectShow) |
| P2 | (b) register and PR packets | Final register rows (docs/design/upstream-divergence-register.md), mark Bill-fixed rows closed, switch policy (keep restore switches as (d), never in PRs), per-row pinning tests named; packet drafts not opened | P0 | 3 | 250k (small) | S rows, O review | each row has a failing-on-Bill test or a repro | PRs only with owner approval per PR |
| P3 | Capture and Lib small clusters | VideoState hdrInferred/dolbyVisionVsif -> side struct via extension pointer (DIV-COM-01 refcount stays (b)); VideoFrameEncoding NV12/P010/P016 and Noop formatter via encoding-registry hook; DeckLink 12-bit route into input.capture module behind `IYCbCr12Route` seat; Magewell backend stub reviewed | none | 3 | 250k (small) | S moves, O hook shape | CPU capture tests (Magewell +137), B | ABI 37 struct layout shared with renderer DLL |
| P4 | Signal analysis e1 | AnalysisLumaSource packed-DV sampling and DV raw-word route -> `IAnalysisSampleSource` hook (generic, shared with WP-23 plane; PR-able); AnalysisFrame contract v2 behind it; SubtitleCutPasteShader rgba32f as parameter (default Bill's precision); DolbyVisionLuminancePolicy sceneMin as extras | WP-23 (done), F-11 extras | 5 | 450k (medium) | O hook, S moves | B, S0, analysis parity tests (Bill's), DV synthetic CPU corpus | SubtitleCutPasteShader.h has 4 merge conflicts: move before merge helps |
| P5 | DV formatter | **= F-11 (item 3), no extra scope** | F-11 | in F-11 | in F-11 | | | list only |
| P6 | Rules/profile/schema residual | DisplayRuleExpression new variables (ambient/content/source/hdr_inferred) via a variable-provider registry (generic, PR-able); MadVRShaderLoader cinemaOnly as rule attribute hook; UnifiedProfileRuntime session-override/NVOF look-ahead rule behind profile-runtime extension; check the -96 refactors in MainConfigSchema/RendererProfileConfig are neutral (diff with whitespace/semantic script); MotionConfig engine stays (d) | WP-18 for tables (sequence after WP-18 key freeze) | 6 | 550k (medium) | O | B, rule tests (DIV-RULE-01/02 stay), Cfg gate | WP-18 and this touch the same validator files: serialise |
| P7 | Plugin ABI trim (optional) | Collapse new IRenderer virtuals (status/intent/content, ~190 lines) into one `QueryExtension(id)` accessor with typed adapters; counter stays 44+ | none | 4 | 400k (medium) | O | proxy tests, DLL pair hash | ABI change touches both renderer DLLs and Beta host; low value: recommend DEFER unless the owner wants it, accept (a) as is |
| P8a | Host settings parse/fingerprint/transport | Move parse + fingerprint + transport (about 650 not covered by WP-18) behind `VpSettings::Parse(cfg, out)` call site in LVR | WP-18 (tables/golden), WP-16 | 5 | 500k (medium) | O design, S moves | fingerprint golden unchanged, B, S0 | fingerprint order must stay byte-equal to the WP-18 golden |
| P8b | Host output hook chain residual | custom GLSL chain (7325, 23296) into OutputSegments/Merge seats; builder statics to module | S2/S3 (merged), step 0 | 4 | 400k (medium) | O | B, S0, GLSL tests | PRE_OUTPUT order/hook tagging rules (OrderSlotsInPlace) |
| P8c | Render-ahead / queue / anchor host code | WaitForFileFrameSlot, HoldTicks*, prefill, anchor logic -> output.presentation module; host keeps queue-policy call sites | none; **after the Bill merge** (DisplayVsyncClock overlap) | 4 | 450k (medium) | O only | render-ahead GPU gate, B | hottest area; Bill also rewrote pacing |
| P8d | ABI-37 file frames | file-source frame path and VPRendererFilter hand-off -> input.capture adapter | M2 input | 4 | 350k (medium) | O hook, S moves | filter graph tests, GPU file-source leg | filter DLL boundary |
| P8e | DV host stage | GPU unpack/tunnel/repack and hdr_inferred pushes via DvHostStage seats (no new logic) | F-11 first, m2-dv | 3 | 300k (medium) | O | DV synthetic corpus byte-identical, GPU unpack tests | after F-11 |
| P8f | Subtitles / active picture / look-ahead | host walk (27938, 29130) -> WP-23/analysis adapters | WP-23 | 3 | 300k (medium) | O | subtitle regression clips, B | Bill's many subtitle PRs change this area: freeze list |
| P8g | Diagnostics rows and test seams | report-a-problem rows, status getters, `*ForTest` seams -> WP-17 adapters and a test-friend header | WP-17 | 3 | 250k (small) | S moves, O review | CPU + report-a-problem test | seams used by 100+ tests |
| P9 | VideoProcessorDlg/App extraction | VpDialogExtension object (state dump, LLDV hand-off, HDR inference, profile/RPC dispatch, command-line VP keys, crash state) with ~25 call sites; shortcut and retry (b) fixes stay | none | 8 | 800k (large) | O design, S moves | B, S0, GUI/Config gate, RPC tests | runtime GUI; no GPU needed |
| P10 | ConfigEditorWindow extraction (after big test) | `IConfigPageContributor` (add pages and group controls at fixed anchor points) into VP-V/config; VP pages, shortcut table, migrations (E2-10) move; Bill's page code restored; RPC Calibration ops via extension | "Config org" queue item; Config usability (merged) | 12 | 1.3M (large) | O hook and layout, S moves | Config gate, Bill's test file restored passes; off-screen runs | 15 conflict hunks with Bill; reorg already done once |
| P11a | Restore Bill test assertions | folded into P1b (analysis, motion, window) and P10 (config pages) | | in P1b/P10 | | | | |
| P11b | VP tests out of Bill's test files (after big test) | ConfigEditorWindowTests (+9.1k), ConfigFileTests (+2.1k), BetaParametersTests, DV tests (with F-11); category attrs via existing generator | B1/G6 (item 18 rest) | 4 | 350k (medium) | S | test counts equal before/after; CPU + Config gate | pure move; do after P10 |
| P12 | e3 / fork infrastructure and rulings | register class (f), libplacebo DLL rule, IMediaSideData re-vendor (check the HDR10+ GUID rename effect), AGENTS.md, packaging scripts, fix stale test_libplacebo_variants.py | none | 2 | 150k (small) | S | docs/test only | needs the rule class from the owner |
| P13 | Bill merge (= WP-21) | merge 81ac960f (or newer) into the extracted tree; resolve ~40-60 remaining hunks; re-check OriginalPathway.h vs his defaults (#199, #171, #191); drop closed (b) rows; ABI counter; Hc (P8c) done here | all above, after WP-20 | 10-12 | 900k (large) | O | B vs his tip build, chain gate, pacing/render-ahead gate, Config gate | the 156-commit tip keeps moving |
| P14 | Final gate and register close | per-hunk table rerun: residual e1 = 0; every Bill-file hunk has a register row or call-site doc; audits | all | 2 | 150k (small) | O | table totals | |

## 5. Totals and ordering relative to the big test (WP-20)

Not counted: items already in the 20-item list (WP-9/10/16/18, F-11, S1, M2 tail, B1/G6, step 0B, WP-19, WP-20). Add-on audits: about 12 x 0.5 h, 120k.

| Phase | Packages | Agent hours | Tokens |
|---|---|---|---|
| Before the big test | P0, P1a, P1b, P2, P3, P4, P6, P8a, P8b, P8d, P8e, P8f, P8g, P9, P12, 11 audits | about 62 h plus 6 h audits = **68 h** | about **5.6M** (6.9M with audits) |
| After the big test, before/with the Bill merge | P8c, P13, P14 | 16-18 h | ~1.5M |
| After the big test, no render risk | P10, P11b, (P7 optional 4 h/400k) | 16 h (+4) | 1.65M (+0.4M) |
| **Grand total** | | **about 100-105 h agent wall-clock (+4 optional)** | **about 10M tokens** |

Critical path (two lanes, builds max two at once): P0 -> P1a -> P1b (lane A: semantic, defaults) and P0 -> P3/P4/P8g (lane B, mechanical) -> P8a/P8b -> P9; roughly 38-45 h of wall-clock before the test if the lanes run in parallel, bounded by the existing WP-9 chain (45-55 h), so these packages mostly run beside it, not on top of it.

Must precede the big test (they change what the test measures): P1a, P1b (defaults and Original = Bill), P3, P4, P6, P8a/b/d/e/f/g, P9 (runtime GUI paths), F-11 and the planned WP items. Order inside: P1 before WP-18's key freeze; P8e after F-11; P6 after WP-18.
Can follow the big test: P8c (and the pacing parts) with the Bill merge, P10 and P11b (Config UI and test moves are covered by the Config gate and test counts, not by the GPU chain), P7, P12 documents, P14.
Parts suited to sonnet drafting: P0 script, P1a entries and tests, P2 rows, P3/P8a/P8g/P11b mechanical moves, P12; everything introducing a hook shape, the Original semantics, anything near rendering order, pacing, DV, fingerprints: opus.

## 6. Verification contract (all packages)

1. Original = Bill's: with `pipeline_original_pathway on`, a capture-harness run on the clip set equals a build of Bill's tip (tip build is the oracle, 0 codes) plus Bill's own test files restored and passing unmodified.
2. Slot/hook off: with the extension not installed (null), output identical to today's integ build (S0); one test per hook (`NoExtensionEqualsBill`).
3. Fingerprint golden (WP-18) unchanged after P8a/P10.
4. Per-package independent audit before the verification build (rule); builds /m:4, max two at once; GPU legs on mains and the 3090 for the confirmation.
5. Register: every hunk remaining in Bill's files is a call site, a registered (b) row with a failing-on-Bill pinning test, an include/registration line, or an either/or hook; P0's table is re-run at P14 to prove it.

## 7. Risks and open points

- The +-25% cluster sizes: P0 turns them into exact numbers and may move P8/P10 by a quarter either way.
- Moving the Config pages (P10) is the biggest single unknown: 7,089 lines, Bill changes the same file (+924), and reverting to Bill's page structure is a visible UI change; owner item 12 decides.
- Bill's pacing work (#198/#204) may make our render-ahead host code redundant or conflicting; P8c deferred until the merge for that reason.
- AnalysisLumaSource revert (E2-11) may re-expose detector errors that the mapping fixed; the evidence run (black bars, subtitles on full-range RGB) must precede the revert.
- Restoring Bill's tests could reveal real behaviour changes nobody listed; budgeted at 2 h per finding (not in totals).
- PRs to billslack2: nothing opened; packets prepared only, each needs explicit owner approval.
