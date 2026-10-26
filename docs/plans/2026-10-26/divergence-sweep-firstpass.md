# Upstream divergence sweep, first pass (2026-10-25)

Read-only first pass; an Opus review follows. No builds, no edits, no git writes.
Integ: `D:\codex\videoprocessor\integ-batch1` @ `0925f594` (integ/batch1). Bill: `origin/v1.4.02-beta` @ `8c453746`.

## 0. Method and three things to know first

1. **The two-dot diff is not "our changes".** `git diff origin/v1.4.02-beta HEAD` mixes our changes with 153 newer commits on Bill's tip
   (merge-base `83b7ffea`; integ is 1113 commits ahead of it, Bill's tip 153 ahead). Two-dot shows 330 non-added paths: 242 M plus 88 D.
   The 88 D are files Bill *added* after the merge-base (AnalysisEvaluationBudget.h, DebugLogAsync.h, DisplayRasterInfo.h, StatsOverlayLayout.h,
   the Prepared*/Queue*/Subtitle* proof headers, VP-0204/0205/0211 docs, ...); they are not deletions by us. I used the three-dot diff
   (`origin/v1.4.02-beta...HEAD`): **181 Bill files modified by integ** (1134 further paths are VP additions, ignored), +57,079 / -8,000 lines.
2. **74 of the 181 files were also changed by Bill since the merge-base** (column "Bill also changed"). Integ has not merged Bill's tip, so this
   sweep is against the old base; every (b) row must be re-checked against the tip. Already confirmed: Bill's tip fixes the tick overflow in
   `VideoProcessor-Lib/VideoTimingController.cpp` and `microsoft_directshow/DirectShowTimingClock.cpp` itself (128-bit mul/div), so DIV-TIM-01 is partly upstream already
   (take Bill's text on rebase). Bill's tip also has its own `DebugLogAsync.h` and `DisplayMonitorInventory.h` (DisplayTopologySession now uses it), which
   overlap DIV-LOG-01 and DIV-DISP-01.
3. Classification is by cluster, not hunk-by-hunk, for the large files. Evidence is the in-code provenance comments (audit 2026-10-16, bug hunt 2/3,
   fixed-constant batch Bx, WP-n, ABI n) plus the existing notes (plan-f11-triage..., modularity-integ-batch1, docs/design/*-callsites.md).
   Cluster line counts are +-15%. Small files were read in full diff; large ones by hunk list plus samples.

Classes: **(a)** host call site into VP code; **(b)** genuine bug fix in Bill's behaviour (register row, PR candidate); **(c)** include-path / build registration;
**(d)** either/or hook where Bill's behaviour stays selectable; **(e1)** VP feature body resident in Bill's file (extraction pending, covered by the skeleton migration);
**(e2)** unclear or unjustified change to Bill's behaviour, shipped default, or Bill's own tests; **(e3)** fork infrastructure (packaging, binaries, docs, build identity) that needs an owner ruling because
the rule has no class for it. Combined labels mean the file has hunks of each; "b/e2" means a fix-like change whose justification is only the owner's "general, not fixed" rule (no bug in Bill's tree).

## 1. Counts

Files touching each class (181 files; a file can touch several): **b 78, e1 60, e2 41, e3 24, a 22, c 14, d 7**.
Files with a single class only: b 33, e1 30, e3 23, e2 15, a 10, c 12, d 3; the rest are combinations.
By lines, class e1 dominates: roughly 88% of the +57k lines (LibplaceboLutParserTests +12.9k, ConfigEditorWindowTests +9.1k,
LibplaceboVideoRenderer.cpp +11.1k, ConfigEditorWindow.cpp +7.1k, VideoProcessorDlg.cpp +3.0k, ConfigFileTests +2.1k, DV formatter +1.1k and its tests +1.4k).
(a) is about 1.5k lines, (b) about 4k lines (capture backends ~1.4k, DV formatter class 1 ~0.6k, GUI/Config hunks ~0.8k, rest small), (c) about 1k lines.
(d) is small: Original-pathway gates in the host renderer (~150 lines), the libplacebo-beta fork API (additive, default off), the NLS `sampling` parameter.

## 2. Per-file tables

### Capture backends and timing (Lib)

| File | +/- | Bill also changed | Class | Hunks / note |
|---|---|---|---|---|
| `Lib/IntegerMath.h` | +39/-0 |  | b | 128-bit overflow-safe round(a*b/div); tick conversions overflowed int64 after ~25.6 h. Registered with WallClock/VideoTimingController/DirectShowTimingClock/ALiveSource pin (DIV-TIM-01). |
| `Lib/WallClock.cpp` | +4/-1 |  | b | QPC*1e7 overflow after ~25.6 h; uses I64_MulDivRoundSaturating (DIV-TIM-01). |
| `Lib/VideoTimingController.cpp` | +7/-8 | YES | b | exact tick->REFERENCE_TIME; old fallback truncated to whole seconds past ~10.7 days (DIV-TIM-01). |
| `Lib/microsoft_directshow/DirectShowTimingClock.cpp` | +4/-1 | YES | b | now*1e7 overflow froze the graph clock (DIV-TIM-01). |
| `Lib/microsoft_directshow/live_source_filter/ALiveSourceVideoOutputPin.h` | +5/-20 | YES | b | same overflow fix replacing the truncating fallback (DIV-TIM-01). |
| `Lib/TimingClock.cpp` | +12/-16 |  | b | TimingClockDiffMs keeps sub-millisecond part (DIV-TIM-02). |
| `Lib/ACaptureDevice.h` | +1/-1 |  | b | m_refCount uninitialised -> {0} (DIV-COM-01). |
| `Lib/magewell/MagewellCaptureDeviceDiscoverer.h` | +1/-1 |  | b | m_refCount{0} (DIV-COM-01). |
| `Lib/blackmagic_decklink/BlackMagicDeckLinkCaptureDeviceDiscoverer.h` | +3/-1 |  | b | m_refCount{0} (DIV-COM-01). |
| `Lib/VideoState.h` | +25/-1 |  | b+e1 | m_refCount{0} (b); hdrInferred/dolbyVisionVsif members (e1). |
| `Lib/VideoState.cpp` | +17/-1 |  | b+e1 | copy ctor starts refcount at 0 (b, DIV-COM-01); hdrInferred/dolbyVisionVsif copy and NV12/P010/P016 BytesPerRow (e1, ABI 37). |
| `Lib/blackmagic_decklink/BlackMagicDeckLinkCaptureDeviceDiscoverer.cpp` | +45/-15 |  | b | COM-boundary exception guard (CaptureCallbackBoundary), tolerant duplicate arrival/removal, ref ownership (DIV-CAP-01). |
| `Lib/blackmagic_decklink/BlackMagicDeckLinkCaptureDevice.cpp` | +332/-60 |  | b+e1 | bulk b (idempotent Stop, forced-stop state, frame period from card, throw->status, mode-table fallback; DIV-CAP-01); 12-bit YCbCr route cluster ~60 lines (e1, DIV-CAP-03). |
| `Lib/blackmagic_decklink/BlackMagicDeckLinkCaptureDevice.h` | +34/-2 |  | b | members for the above. |
| `Lib/blackmagic_decklink/BlackMagicDeckLinkTranslate.cpp` | +188/-21 |  | b+e1+e2 | Rec.601 on any raster and EOTF reserved 4-7 (b, DIV-CAP-04/05); ycbcr_12bit_capture parse (e1); capture_display_fixes switch plumbing (e2). |
| `Lib/blackmagic_decklink/BlackMagicDeckLinkTranslate.h` | +53/-2 |  | b+e2 | declarations; default YCbCr12Capture::V210 changes Bill's default input handling (e2: needs owner ruling or Original-pathway gate). |
| `Lib/magewell/MagewellCaptureDevice.cpp` | +451/-47 |  | b+e2 | capture-thread fault recovery, pool starvation, HDR InfoFrame change-only logging, AVI BT.2020 read, ARGB range, rate policy (b, DIV-CAP-02); magewell_recover_faults restore switch (e2 plumbing). |
| `Lib/magewell/MagewellCaptureDevice.h` | +43/-2 |  | b | members for the above. |
| `Lib/magewell/MagewellBackend.cpp` | +71/-0 |  | c+e2 | VP_NO_MAGEWELL CI stub (build-only, default build unchanged); RecoverFaults switch (e2). |
| `Lib/magewell/MagewellBackend.h` | +5/-0 |  | e2 | SetRecoverFaults/RecoverFaults (restore-switch plumbing). |
| `Lib/magewell/MagewellSdk.props` | +9/-1 |  | c | VpCiNoMagewell CI switch; default build unchanged (build infra, include/define only). |
| `Lib/DisplayMode.cpp` | +41/-2 |  | b | NanosecondsPerFrame defined (had no return: UB); refresh limit made shared policy (DIV-CAP-06). |
| `Lib/DisplayMode.h` | +13/-2 |  | b | declares NanosecondsPerFrame + shared policy accessors (DIV-CAP-06). |
| `Lib/ColorSpace.cpp` | +55/-0 |  | b | Rec.601 on non-SD raster mapped by measured-rate family instead of throwing (DIV-CAP-04). |
| `Lib/ColorSpace.h` | +27/-0 |  | b | declarations (DIV-CAP-04). |
| `Lib/VideoFrameEncoding.cpp` | +28/-0 |  | b+e1 | R10b/R12L handling (b); NV12/P010/P016 names/bpp/FourCC (e1, ABI 37). |
| `Lib/VideoFrameEncoding.h` | +19/-1 |  | e1 | NV12/P010/P016 enum + IsTwoPlane420 (ABI 37, VP Renderer filter file sources). |
| `Lib/video_frame_formatter/CNoopVideoFrameFormatter.cpp` | +7/-0 |  | e1 | NV12/P010/P016 contracts (ABI 37). |
| `Lib/vprenderer/AlphaNativeRgbIngress.h` | +6/-1 |  | b | R10b/R10l excluded from libplacebo native upload: no D3D11 texture format, nothing shown (DIV-CAP-07). |

### Rate-constant unification ("general, not fixed") (Lib)

| File | +/- | Bill also changed | Class | Hunks / note |
|---|---|---|---|---|
| `Lib/DisplayRefreshRatePolicy.h` | +38/-0 | YES | b/e2 | DisplayRefreshBounds/CaptureRateBounds shared constants. Widens Bill's 10-240 Hz window to 10-500 Hz; no bug in Bill's tree (DIV-RATE-01, owner rule only). |
| `Lib/OutputReadinessController.cpp` | +3/-2 |  | b/e2 | 10-240 literal -> shared bound (DIV-RATE-01). |
| `Lib/DisplayRefreshRateEstimator.cpp` | +2/-1 |  | b/e2 | same (DIV-RATE-01). |
| `Lib/RationalLiveOutputSequencer.cpp` | +2/-1 |  | b/e2 | same (DIV-RATE-01). |
| `Lib/DisplayRefreshRatePolicy.cpp` | +2/-2 | YES | b/e2 | same (DIV-RATE-01). |
| `Lib/OsdTimingPolicy.h` | +2/-3 | YES | b/e2 | same (DIV-RATE-01). |
| `Lib/vprenderer/AlphaPresentationTelemetry.cpp` | +5/-4 |  | b/e2 | same (DIV-RATE-01). |
| `Lib/microsoft_directshow/video_renderers/DirectShowVideoRenderer.cpp` | +5/-2 |  | b/e2 | bounds helper; clock log by seconds not 600 frames (DIV-RATE-01/02). |
| `Lib/microsoft_directshow/live_source_filter/CBufferedLiveSourceVideoOutputPin.cpp` | +50/-37 |  | b/e2 | delivery hold = 1.15x measured interval (>=50 ms; identical >=23 fps), bounds helpers, log cadence in seconds, generalRates flag (DIV-RATE-01/02). |
| `Lib/vprenderer/AlphaSourceCropPolicy.cpp` | +7/-2 | YES | e2 | FallbackNotice logging around the fps<=0 fallback (log-only). |
| `Lib/ActivePictureTransitionModel.cpp` | +4/-1 | YES | e2 | FallbackNotice logging (log-only). |
| `Lib/vprenderer/BufferedPictureExpansion.cpp` | +4/-1 | YES | e2 | FallbackNotice logging (log-only). |
| `Lib/LiveEpochConvergenceController.cpp` | +17/-2 |  | b/e2 | paced-plateau counts as time with frame floor; identical at <=60 Hz (DIV-RATE-02). |
| `Lib/LiveEpochConvergenceController.h` | +8/-0 |  | b/e2 | generalRates flag + helpers (DIV-RATE-02). |
| `Lib/vprenderer/AlphaCadenceCorrectionPolicy.cpp` | +155/-10 |  | b+e1 | queue_timing_fixes: arrival-rate estimator and seconds-based warm-ups (hunt 2 13-timing M1/M2/L6); switch default off in Bill-compatible policy struct (DIV-TIM-03). |
| `Lib/vprenderer/AlphaCadenceCorrectionPolicy.h` | +61/-0 |  | b+e1 | declarations (DIV-TIM-03). |
| `Lib/ConfigFile.cpp` | +2/-0 |  | e2 | RefreshKey::Apply(m_sections) on every Load: rewrites refresh-key section names for Bill's reader too (global behaviour change, 1 line). |
| `Lib/PPMCorrectionLoader.cpp` | +25/-48 |  | e2 | ppm refresh keys parsed as doubles through RefreshKey (59.94 distinct from 60); rewrites FindBestMatch (-48). |
| `Lib/PPMCorrectionLoader.h` | +6/-4 |  | e2 | map<int>->map<double>. |

### Signal analysis / evidence (Lib)

| File | +/- | Bill also changed | Class | Hunks / note |
|---|---|---|---|---|
| `Lib/AnalysisLumaSource.cpp` | +365/-15 | YES | b/e2+a | Sample() maps full-range sources to the limited code domain (changes Bill's emitted codes; Bill's parity tests edited: DIV-AN-01); +256 packed-DV sampling and shared-plane read-through (a, WP-23). |
| `Lib/AnalysisLumaSource.h` | +91/-1 | YES | a+e2 | AnalysisLumaPlane/SampleLuma shared per-frame luma (WP-23, offered upstream: a); PackedDolbyVision422 (e1); limited-domain contract constant (DIV-AN-01). |
| `Lib/AnalysisFrame.cpp` | +50/-6 | YES | e1 | evidence contract v2 + DV raw-word route (dv_tunnel_gpu_unpack). |
| `Lib/AnalysisFrame.h` | +6/-2 |  | e1 | dolbyTunnelPackedAlignment. |
| `Lib/SceneDetector.cpp` | +1/-3 |  | a | SampleLuma call (WP-23). |
| `Lib/ActivePictureEvidence.cpp` | +6/-6 | YES | a | SampleLuma call x2 (WP-23). |
| `Lib/SubtitleBoxDetector.cpp` | +21/-20 | YES | a+b | SampleLuma (WP-23); excludes full-range planar from the fast path (follows DIV-AN-01). |
| `Lib/SubtitleCutPasteShader.h` | +50/-12 | YES | e1 | rgba32f composite precision when float32 FBOs are on (audit 2026-10-16). |
| `Lib/vprenderer/DolbyVisionLuminancePolicy.h` | +38/-0 | YES | e1 | L1 scene minimum carried (status only). |
| `Lib/DolbyVisionLuminanceStatus.h` | +4/-0 | YES | e1 | sceneMin status fields. |

### Config, rules, profiles (Lib)

| File | +/- | Bill also changed | Class | Hunks / note |
|---|---|---|---|---|
| `Lib/DisplayRuleExpression.h` | +92/-19 | YES | b+e1 | source_rate rounds to nearest (59.94 -> 60; old floor missed 59.94p), exponent literals, Kleene 3-valued logic (b; one selection change under !: DIV-RULE-01); new variables ambient/content/source/hdr_inferred (e1). |
| `Lib/StateVariables.cpp` | +9/-2 |  | b+e1 | source_rate rounding (b); hdr_inferred variable (e1). |
| `Lib/microsoft_directshow/MadVRShaderLoader.cpp` | +99/-12 |  | b+e1 | target-shader key press: only rules that declare the chord take part; atomic group commit (audit 2026-10-16: DIV-RULE-02); cinemaOnly flag (e1). |
| `Lib/microsoft_directshow/MadVRShaderLoader.h` | +13/-0 |  | b+e1 | test seams (b) + cinemaOnly (e1). |
| `Lib/MotionConfig.h` | +6/-2 |  | e1/d | motion_engine (auto|vp|smooth_motion). Either/or knob but default auto changes Bill's smooth-motion choice: needs Original-pathway gating check (e2). |
| `Lib/QueueConfiguration.h` | +22/-0 |  | b | TryGetHardCapacityString: legacy [command_line]/[general] queue_size (CFG9: [general] was validated but never read; DIV-CFG-01). |
| `Lib/ConfigurationLiveApply.h` | +85/-0 |  | b+e2 | AltGr and observer pass-through (SH6, b: DIV-GUI-01); global_hotkeys default off = Ctrl/Alt/F-keys only (delta 6, changes Bill's default: e2). |
| `Lib/ActiveProfileStatus.h` | +7/-3 | YES | a | status block v11 adds fallbacks[] (VP-V status text). |
| `Lib/CalibrationProfileMigration.h` | +1/-1 |  | e1 | adds "motion" child section. |
| `Lib/ProfileChangeOverlay.h` | +1/-0 |  | e1 | "hdr" group label. |
| `Lib/UnifiedProfileRuntime.h` | +25/-2 | YES | e1 | VP profile-runtime additions. |
| `Lib/UnifiedProfileRuntime.cpp` | +141/-9 | YES | e1 | session-override groups, NVOF look-ahead rule, selection-change detection. |
| `Lib/RendererProfileConfig.h` | +390/-20 | YES | e1+b | VP keys (saturation..), Original-pathway baseline hook, [vpv] section; key-chord grammar SH10 (b, DIV-GUI-01). |
| `Lib/MainConfigSchema.h` | +266/-96 | YES | e1+b | VP keys; global_actions validation (SH6); several rule-list refactors (-96): check each refactor is behaviour-neutral. |
| `Lib/DebugLogRetention.h` | +55/-0 |  | b | in-session 100 MB rollover (.r1-.r3) so one long session cannot grow vp.log without bound (DIV-LOG-01). Bill's tip has DebugLogAsync.h: overlap. |
| `Lib/DebugLog.h` | +129/-1 | YES | b+e1 | rollover, recent-line ring, reopen handling (DIV-LOG-01); recent-line ring serves state reports (e1). |
| `Lib/RendererResetPolicy.h` | +51/-0 |  | b | bounded back-off for automatic renderer retry; count clears only after sustained RENDERING (DIV-GUI-02). |
| `Lib/RendererResetCoordinator.h` | +6/-0 |  | b | NativeThreadHandle() for a bounded shutdown wait (DIV-GUI-02). |
| `Lib/DisplayTopologySession.cpp` | +218/-18 | YES | b+e2 | startup recovery never stops the program; native-size ranking; inactive-path modes (DIV-DISP-01); capture_display_fixes off branch (e2 plumbing). |
| `Lib/DisplayTopologySession.h` | +133/-2 |  | b | startup RestorePending form + helpers (DIV-DISP-01). |

### Renderer interface / plugin ABI (Lib)

| File | +/- | Bill also changed | Class | Hunks / note |
|---|---|---|---|---|
| `Lib/IRenderer.h` | +193/-0 | YES | a | new virtual entries: module status, intent, content status (host call-site surface; +193). |
| `Lib/vprenderer/LibplaceboRendererPluginApi.h` | +59/-1 | YES | a | ABI counter 28->44 plus comment log; entries for the new IRenderer virtuals. |
| `Lib/vprenderer/LibplaceboPluginVideoRenderer.cpp` | +207/-14 | YES | a+b | forwarders (a); refuses a DLL whose API/build ID differs before any other call (hunt 2: b, DIV-ABI-01). |
| `Lib/vprenderer/LibplaceboPluginVideoRenderer.h` | +30/-0 | YES | a | IsRefused + forwarders. |
| `Lib/RendererId.cpp` | +11/-0 | YES | a | IsLibplaceboRefused (refused plugin listed, never loaded). |
| `Lib/RendererId.h` | +3/-0 |  | a | declaration. |
| `VPRenderer/LibplaceboPluginExports.cpp` | +84/-13 | YES | a+b | exports: build-ID, held-LED-drive, ConfigureAnalysisDiagnostics try/catch (no exception across C ABI: b, DIV-ABI-01); logging set-up ordering (b). |
| `VPRenderer/VideoProcessor-VPRenderer.vcxproj` | +38/-3 |  | c | VP-V sources + IntDir include + version header path. |
| `VPRenderer/VideoProcessor-VPRendererBeta.vcxproj` | +38/-3 |  | c | same. |
| `Lib/vprenderer/LibplaceboRenderParameters.cpp` | +89/-3 |  | e1 | SDR ambient lift, deband grain, 16-bit surface dither rule. |
| `Lib/vprenderer/LibplaceboRenderParameters.h` | +28/-1 |  | e1 | new fields + fbo_float32 / deband_grain. |
| `Lib/vprenderer/TemporalRenderer.h` | +19/-4 | YES | b/a | HistoryFrame: history frames relabelled with the prepared colour space (only matters once Extended Range relabels the current frame: default nullptr keeps Bill's behaviour: a). |
| `Lib/vprenderer/LibplaceboVideoRenderer.h` | +220/-1 | YES | a+e1 | VP members/methods for the .cpp clusters (+220/-1). |
| `Lib/vprenderer/NlsHookRecovery.h` | +45/-3 |  | b | WithdrawHook: params point at a caller-owned list (was a local: dangling pointer after return) (DIV-RENDER-01). |
| `Lib/vprenderer/LibplaceboVideoRenderer.cpp` | +11073/-368 | YES | a+b+e1+d | see cluster table below (+11073/-368). |

### GUI / Config / ConfigCore

| File | +/- | Bill also changed | Class | Hunks / note |
|---|---|---|---|---|
| `GUI/VideoProcessorDlg.cpp` | +3001/-476 | YES | a+b+e1+e2 | see cluster table below (+3001/-476). |
| `GUI/VideoProcessorDlg.h` | +287/-7 | YES | a+b+e1 | members for the clusters (+287/-7). |
| `GUI/VideoProcessorApp.cpp` | +247/-50 | YES | b+e1 | bounded dialog destruction, crash-state, command-line options for VP keys (bug hunt 2 M1, SH3: b); ADD of VP command-line keys (e1). |
| `GUI/VideoProcessorApp.h` | +23/-0 |  | b+e1 | decls. |
| `GUI/StatsOverlayWindow.cpp` | +28/-0 | YES | a+e1 | extra rows from module status, queue-target row (a/e1). |
| `GUI/StatsOverlayWindow.h` | +19/-0 | YES | a+e1 | StatsExtraRow + queue-target fields. |
| `GUI/FullscreenVideoWindow.cpp` | +13/-7 | YES | b | log only first presses that matched a shortcut; keyboard auto-repeat (DIV-GUI-01). |
| `GUI/ConfigurationRpcServer.cpp` | +3/-1 | YES | a | accepts ProjectorTest/Calibration RPC ops (e1 feature plumbing). |
| `ConfigCore/ConfigEditorCore.cpp` | +92/-21 |  | b | BOM kept out of key parse, wall-clock backup names, last-error defined only after failure, external-file overwrite guard (DIV-CFG-02). |
| `ConfigCore/ConfigEditorCore.h` | +16/-0 |  | b | StoredSection decl. |
| `ConfigCore/ConfigurationRpcClient.cpp` | +62/-7 | YES | b+e1 | receive timeout = busy VP, not version mismatch (b); Calibration action (e1). |
| `ConfigCore/ConfigurationRpcClient.h` | +19/-0 | YES | e1 | Calibration decl. |
| `ConfigCore/ConfigurationRpcProtocol.h` | +10/-1 | YES | e1 | new operations. |
| `Config/ConfigEditorWindow.cpp` | +7089/-350 | YES | b+e1+e2 | see cluster table below (+7089/-350). |
| `Config/ConfigEditorWindow.h` | +65/-1 | YES | e1 | members. |
| `Config/AppMain.cpp` | +49/-5 | YES | e1+b | --sandbox settings preview (e1); title for remote VP (SH9: b). |
| `Config/ProfileListController.h` | +10/-3 |  | b | takeItem current-row fix (audit 2026-10-16). |
| `Config/UpdatePanel.h` | +7/-3 |  | b | unknown mode/channel from helper kept (audit 2026-10-16). |
| `Config/VpTheme.cpp` | +4/-4 | YES | b | QDoubleSpinBox styled like QSpinBox (cosmetic; only needed for VP's double spin boxes: e1). |
| `Config/README.md` | +3/-0 |  | e1 | --sandbox note. |
| `Config/VideoProcessor-Config.vcxproj` | +15/-2 | YES | c | VP-V config sources, /bigobj. |
| `GUI/VideoProcessor-GUI.vcxproj` | +14/-0 |  | c | build-order ProjectReference to both renderer DLLs (build identity). |
| `Lib/VideoProcessor-Lib.vcxproj` | +212/-4 | YES | c | adds VP files/shaders (+212/-4). |
| `Lib/VideoProcessor-Lib.vcxproj.filters` | +35/-0 | YES | c | same. |
| `VideoProcessor.sln` | +35/-0 |  | c | adds VP projects (DvLoopback, StreamRecorder, VPRendererFilter, FakeSpotread) + Debug/PGO mappings. |
| `Directory.Build.targets` | +27/-1 |  | c+e3 | VP-V include root (c); build-identity generation target (e3 fork infra). |

### DV formatter (see plan-f11-triage-and-small-items.md section A)

| File | +/- | Bill also changed | Class | Hunks / note |
|---|---|---|---|---|
| `Lib/video_frame_formatter/CR210DolbyVisionTunnelFormatter.cpp` | +1085/-55 | YES | b+e1+e2 | +1085/-55: class 1 (b) ~520, class 2 (e1) ~535, class 3 (e2, m_bugHuntFixes restore switch) ~45 (triage A.2). |
| `Lib/video_frame_formatter/CR210DolbyVisionTunnelFormatter.h` | +104/-1 |  | b+e1+e2 | +104/-1: ~40 b, ~55 e1, ~8 e2. |
| `Lib/video_frame_formatter/CR210DolbyVisionTunnelFormatter.AVX2.cpp` | +59/-9 |  | b | R10b/R10l loaders (capability, pending owner Q1). |
| `Lib/video_frame_formatter/IVideoFrameFormatter.h` | +97/-0 | YES | e1 | +97: trims/intent/hold fields read only by VP consumers. |

### shaders / 3rdparty / build artefacts

| File | +/- | Bill also changed | Class | Hunks / note |
|---|---|---|---|---|
| `shaders/NLS.glsl` | +108/-19 |  | e2+d | adds `sampling` param default 1.0 = Catmull-Rom replaces Bill's bilinear read; 0 restores bilinear. Default changes Bill's output (e2). |
| `shaders/NLSPlus.glsl` | +91/-3 |  | e2+d | same. |
| `3rdparty/libplacebo-beta/include/libplacebo/renderer.h` | +15/-0 |  | d | fbo_float32/output_fbo_float32 (default false = upstream; header matches the VP fork DLL). |
| `3rdparty/libplacebo-beta/include/libplacebo/shaders/custom.h` | +11/-0 |  | d | pl_mpv_user_shader_set_texture (fork API, additive). |
| `3rdparty/libplacebo-beta/lib/libplacebo-377.def` | +1/-0 |  | d | export of the above. |
| `3rdparty/libplacebo-beta/bin/libplacebo-377.dll` | +-/-- | YES | e3 | binary is NOT Bill's: same source + FL35 output-clamp patches (README). Needs owner ruling. |
| `3rdparty/libplacebo-beta/lib/libplacebo-377.lib` | +-/-- |  | e3 | import lib of the patched build. |
| `3rdparty/libplacebo-beta/README.txt` | +33/-0 | YES | e3 | documents the patched build. |
| `3rdparty/lavfilters/IMediaSideData.h` | +242/-1 |  | e1 | re-vendored from upstream LAV (DV RPU/HDR10+ side data); old HDR10Plus GUID renamed ...Old (e1/e3: licence note, behaviour change for HDR10+ consumers). |
| `3rdparty/lavfilters/README.txt` | +14/-3 |  | e3 | provenance note. |
| `VideoProcessor.cfg` | +68/-18 | YES | e2 | shipped defaults changed: switch_refresh_rate fullscreen_only (was true), target_frames 5 (was 3), look-ahead and fbo_float32 comment blocks, newlldv veto etc. Bill's shipped defaults differ. |
| `CONFIGURATION.html` | +21/-5 | YES | e1 | docs for VP keys. |
| `docs/configuration-public-fields.tsv` | +30/-0 | YES | e1 | docs. |
| `docs/configuration-public-values.tsv` | +10/-0 | YES | e1 | docs. |
| `docs/VP_DOLBY_VISION_TUNNEL.md` | +103/-0 |  | e1 | docs. |
| `docs/libplacebo-beta.md` | +1/-1 |  | e3 | API 25 -> 26 text. |

### packaging and tooling (fork infra, e3 unless noted)

| File | +/- | Bill also changed | Class | Hunks / note |
|---|---|---|---|---|
| `tools/build_installer.ps1` | +75/-17 |  | e3 | -NoUpdater, evidence folder, SDK root, BOM added. |
| `tools/runtime_packaging.ps1` | +53/-2 |  | e3 | build-ID read/compare helpers. |
| `tools/new_update_release.ps1` | +13/-3 |  | e3 | evidence directory, updater helper required. |
| `tools/package_release.ps1` | +10/-3 |  | e3 | paired build-ID assertion; BOM added. |
| `tools/record_runtime_requirement.ps1` | +7/-1 |  | e3 | schema 2 buildId. |
| `tools/stage_app_local_runtime.ps1` | +7/-3 |  | e3 | vprenderer-beta group. |
| `tools/stage_subtitle_trial.ps1` | +5/-0 |  | e3 | build-ID check. |
| `tools/test_portable_config.ps1` | +11/-2 |  | e3 | forward-slash entry names, nested active cfg rejected (b-like hardening of Bill's test). |
| `tools/test_portable_config_contract.ps1` | +5/-1 |  | e3 | new negative cases. |
| `tools/test_libplacebo_variants.py` | +2/-2 | YES | e3 | API 25->26 asserts (stale: plugin API is 44). |
| `packaging/release-manifest.json` | +18/-0 |  | e3 | buildIdentity + shader records. |
| `.agents/skills/vp-release/scripts/build-vp.ps1` | +9/-3 |  | e3 | VP_TEST_REQUIRE_HARDWARE; evidence sidecar path. |
| `docs/VP-0192_INSTALLER.md` | +35/-5 |  | e3 | build-ID / app-local runtime text. |
| `docs/VP-0107_RELEASE_LAYOUT.md` | +13/-0 |  | e3 | build-ID text. |
| `docs/VP_UPDATES.md` | +6/-3 |  | e3 | publish-only-EXE rule. |
| `AGENTS.md` | +28/-0 |  | e3 | VP's own repository guidance (+28). |
| `.gitattributes` | +2/-0 | YES | e3 | *.patch/*.diff -text. |
| `.gitignore` | +1/-0 |  | e3 | __pycache__. |

### Tests (follow their code; assertion edits to Bill's tests flagged)

| File | +/- | Bill also changed | Class | Hunks / note |
|---|---|---|---|---|
| `Test/LibplaceboLutParserTests.cpp` | +12914/-63 | YES | e1 | +12914/-63: VP GPU/renderer tests in Bill's file (173 TargetLutGpuFixture uses); another agent is moving them out. Check the 63 deleted lines are only API follow-ups. |
| `ConfigTests/ConfigEditorWindowTests.cpp` | +9105/-5577 | YES | e1+e2 | +9105/-5577 (-5519 non-whitespace): large rewrite of Bill's Config test file; the deleted lines are Bill's page tests rewritten for VP's reorganised pages. Move VP tests out or re-base on Bill's file. |
| `Test/ConfigFileTests.cpp` | +2074/-8 | YES | e1 | +2074/-8: VP tests inserted; the 8 deleted lines edit Bill's cases (2.40:1 helper, queue_size 28 assertion). |
| `Test/DolbyVisionTunnelFormatterTests.cpp` | +1356/-206 |  | b+e1 | +1356/-206: follows formatter clusters (triage A.4). |
| `Test/VideoProcessor-Test.vcxproj.vcxproj` | +321/-4 | YES | c | +322 test registrations. |
| `Test/VideoProcessor-Test.vcxproj.vcxproj.filters` | +116/-0 | YES | c | +116. |
| `Test/LibplaceboPluginProxyTests.cpp` | +257/-0 |  | a | +257 proxy tests for new IRenderer forwarders. |
| `BetaTests/BetaParametersTests.cpp` | +153/-0 |  | e1 | +153 parameter tests. |
| `BetaTests/VideoProcessor-BetaTests.vcxproj` | +2/-2 |  | c | 2/2. |
| `Test/MagewellCaptureTests.cpp` | +137/-2 |  | b | +137/-2 tests for the capture fixes. |
| `Test/DolbyVisionLuminancePolicyTests.cpp` | +71/-0 |  | e1 | +71. |
| `Test/MotionRendererTests.cpp` | +69/-1 | YES | e1 | +69/-1: deletes the plane_upscaler==bicubic assertion (default changed): flag as Bill-behaviour change test edit. |
| `Test/ConfigEditorCoreTests.cpp` | +56/-0 | YES | b | +56 tests (DIV-CFG-02). |
| `Test/LibplaceboRenderParametersTests.cpp` | +53/-0 |  | e1 | +53. |
| `Test/AlphaSourceCropPolicyTests.cpp` | +47/-0 | YES | e1 | +47. |
| `Test/AnalysisRecoveryParityTests.cpp` | +28/-10 | YES | e2 | +28/-10: deletes Bill's black.luma<8 / headroom>1000 / native==converted asserts (DIV-AN-01 limited-domain change). |
| `Test/AnalysisRecoveryDvTests.cpp` | +17/-13 |  | e2 | +17/-13: deletes Bill's "no rescale" 12-bit assertions (DIV-AN-01). |
| `Test/NativeRgbAnalysisTests.cpp` | +12/-6 |  | e2 | +12/-6: Bill's 16->63 luma expectation edited (DIV-AN-01). |
| `Test/SubtitleBoxDetectorTests.cpp` | +4/-1 | YES | e2 | +4/-1: pixel generator changed to follow the limited mapping (DIV-AN-01). |
| `Test/SubtitleOpaqueCardRefinementTests.cpp` | +6/-3 |  | e2 | +6/-3: expected numbers edited (limited domain). |
| `Test/VideoFrameFormatterTests.cpp` | +10/-4 | YES | b | +10/-4: R10b/R10l no longer native-uploadable (DIV-CAP-07). |
| `Test/WindowResizeLayoutTests.cpp` | +16/-2 |  | e2 | +16/-2: Bill's 1000x1350 placement assertion edited; check which source change drove it. |
| `Test/AnalysisDiagnosticReplayTests.cpp` | +1/-1 | YES | e2 | +1/-1 (small edit of Bill's replay test). |
| `ConfigTests/VideoProcessor-ConfigTests.vcxproj` | +15/-1 | YES | c | 15/1. |


## 3. Cluster tables for the very large files

### 3.1 `vprenderer/LibplaceboVideoRenderer.cpp` (+11,073 / -368, 368 hunks; hunk starts are new-file line numbers)

| Cluster | Hunks (examples) | ~Lines | Class |
|---|---|---|---|
| Includes of VP modules (about 100 `#include`s: VP-V core/segments/modules and vprenderer/*.h helpers) | 2, 62 | 100 | a (c-like) |
| Calls into VP-V adapters and segments: entrance segment S1, output segments S2/S3 (+merged), repeat reuse WP-24 (12378, 23944, 29888), module status WP-11 (27730), diagnostics WP-17, calibration LUT host WP-12 (11159, 24049, 8512), Dynamic Drive WP-15 (26307), analysis host WP-23, fallbacks status (27721), delay budget | 11159, 12378, 23833, 23944, 26307, 27730 | ~650 | a |
| Original-pathway / slot gating: `ApplyOriginalPathwaySettings` (ADR-0019), `pipelineOriginalPathway`, slot `Runs(...)` and `TagSlotHooks` guards (28 sites) | 1534, 2354, 12597, 23296 | ~150 | d |
| Bug fixes in Bill's behaviour: stale-delivery gap merging across dropped repeats (25379, 25453, 25530; `queue_timing_fixes`), timing L5/H1 (30287, 30480), r210 upload note (17799), duplicate shader request same-selector rule (26161 removes Bill's coalescing, 26177 replaces it: audit 2026-10-16), stream-robustness gap rule (25888) | listed | ~250 | b (some gated by restore switches: e2 plumbing) |
| VP features resident in the host: settings parse/fingerprint/transport (1049 +343, 2661 +587, 3494 +217, 11732, 11854 +129), de-judder and B-frame planes (5509 +1635, 29663, 30116), NVOF depth (22459 +394, 13023 +143), intent interpreter (15395, 15575 +2132), custom GLSL hook chain (7325 +394, 23296 +515), ABI-37 file frames (25570 +301, 29563-29792), render-ahead (4716, 10307, 10420, 24122), subtitles/active-picture/look-ahead walk (27938 +190, 29130 +134), DV GPU unpack/tunnel/repack (18761-19115), chroma siting (19219), report-a-problem rows (24451), tests-only seams (679, 5295, 31225 +167) | many | ~9,200 | e1 |
| Deleted Bill code to verify (all inside the clusters above): 26161 (-15, duplicate-request coalescing), 12840 (-40 NLS centre scales, now `NlsMaskMapping::CenterScales`: pure move?), 15395 (-61 intent rewrite), 11159 (-17 automatic LUT reload now policy-selected: confirm Original pathway = Bill's 1 s probe) | | 133 | e2 until verified |

### 3.2 `GUI/VideoProcessorDlg.cpp` (+3,001 / -476)

| Cluster | Hunks | ~Lines | Class |
|---|---|---|---|
| Shortcut layer: foreground-only / global hotkeys, AltGr, auto-repeat drop, unconfigured-key logging, observer pass-through (229, 665, 690, 694, 1474, 1518, 2407, 4990, 8009, 8097, 16332) | | ~450 | b (SH6/audit fixes) + e2 (global_hotkeys default off changes Bill's default) |
| Renderer failure/retry, capture-state race fixes, exit/teardown (114, 6488, 6515, 9647, 9879 +513) | | ~650 | b |
| Report-a-problem / state dump / history (8808, 9384, 14295) | | ~300 | e1 |
| LLDV / DV tunnel hand-off, HDR inference, hdr_inferred pushes (5678, 5721, 13348-13923) | | ~600 | e1 |
| Config RPC (3244 +168), profile dispatch and queue-profile switching (15141 +210, 14762, 14825), renderer combo rebuild with refused plugins (13053, 13086 +240) | | ~700 | e1 / a |
| Overlay/module status poll (18884), fullscreen/focus helpers (8185) | | ~100 | a |
| Rate-key lookup via RefreshKey (5450) | | ~30 | e2 (follows the ConfigFile change) |

### 3.3 `Config/ConfigEditorWindow.cpp` (+7,089 / -350)

| Cluster | Hunks | ~Lines | Class |
|---|---|---|---|
| Pages and controls for VP settings (profile pages 7667 +2196, Actions/LED page 13493 +2065, Startup page, motion/detail/intent groups, shader settings 12667, DeckLink 12-bit control, grain, fbo float32) | 5710-15929 | ~5,600 | e1 |
| Remote-Apply locking, shortcut validation, UI-thread safety, ProfileList fixes (audit 2026-10-16: 1653, 4398, 4646, 4747, 4889, 4903) | | ~350 | b |
| Load-time migrations of the user's configuration (2342 +98 "removing an empty header is a visible migration", 2482 refresh-key convention, 3010 +130 `migrateLegacyQueueOwnership`/refresh-rate-switch) | | ~270 | e2 (rewrites user data on open; Bill's editor does not) |
| Shortcut canonical spelling, controlName, key defaults table (573, 1118) | | ~300 | e1 |
| Misc (RS-232 test, placement clamp, context menu "Activate in VP") | 135, 1436, 3149, 3198 | ~250 | e1 |

### 3.4 Others (sizes only)

- `Test/LibplaceboLutParserTests.cpp` +12,914/-63: VP renderer tests inside Bill's file; being moved by another agent.
- `ConfigTests/ConfigEditorWindowTests.cpp` +9,105/-5,577: wholesale rewrite of Bill's page tests (not whitespace: 9,047/5,519 with -w). Largest single rebase hazard.
- `Test/ConfigFileTests.cpp` +2,074/-8; `Test/DolbyVisionTunnelFormatterTests.cpp` +1,356/-206 (formatter clusters, see below).
- DV formatter family: `CR210...Formatter.cpp` +1,085/-55, .h +104, AVX2 +59/-9, IVideoFrameFormatter.h +97, Noop +7. Use plan-f11 triage A.2/A.3/A.4:
  class 1 (b) about 520 cpp lines, class 2 (e1) about 535 (hook extraction pending), class 3 (e2) about 45 (the `m_bugHuntFixes` restore switch). Table A.3 there already gives the b rows (DIV-DV-01..).

## 4. (b) candidates as register rows

See `upstream-divergence-register-DRAFT.md` for the full rows (IDs DIV-xxx). Summary: DIV-TIM-01/02/03, DIV-COM-01, DIV-CAP-01/02/04/05/06/07, DIV-RULE-01/02, DIV-CFG-01/02, DIV-GUI-01/02, DIV-DISP-01, DIV-LOG-01, DIV-ABI-01, DIV-RENDER-01, DIV-AN-01, DIV-DV-01..09 (from the F-11 triage, not repeated).
Borderline (fix-like, no bug in Bill's tree, kept as e2 until the owner decides): DIV-RATE-01/02.

## 5. Prioritised (e2) and (e3) list

Priority = risk to Bill's users or rebase, then size.

1. **Shipped defaults changed in `VideoProcessor.cfg`** (+71/-28): `switch_refresh_rate: fullscreen_only` (Bill: true), `target_frames: 5` (Bill: 3), new look-ahead/fbo_float32/newlldv comment blocks and defaults. Anyone taking the file gets VP's behaviour. Decide per key: Original-pathway default, or revert to Bill's value.
2. **`ConfigFile::Load` calls `RefreshKey::Apply(m_sections)`** (ConfigFile.cpp, 1 line plus PPMCorrectionLoader double keys, DirectShow/Dlg lookups): global rewrite of refresh-key section names for Bill's reader. Smallest edit with the widest effect; needs a gate or a PR with a justification.
3. **Analysis sample domain (DIV-AN-01):** `AnalysisLumaSource::Sample` now maps full-range sources into the limited code domain; five of Bill's tests had assertions deleted or numbers edited (AnalysisRecoveryParity, AnalysisRecoveryDv, NativeRgbAnalysis, SubtitleBoxDetector, SubtitleOpaqueCardRefinement). Either a genuine bug (then (b) with a repro, and Bill's tests become the evidence) or revert to Bill's mapping with VP reading through a VP-side wrapper.
4. **`libplacebo-377.dll/.lib` is not Bill's binary** (patched output clamp, README documents it) plus the matching fork API in the headers. Rule has no class: ruling needed (keep as the fork's dependency, or restore Bill's binary and make the patches VP-side).
5. **NLS shaders (`NLS.glsl`, `NLSPlus.glsl`):** default `sampling = 1.0` switches Bill's bilinear read to Catmull-Rom (+108/-19, +91/-3); `0` restores Bill's. Either/or exists, but the default differs: make the Original pathway set 0 (check `OriginalPathway.h` carries `nls_sampling`) or default 0.
6. **`global_hotkeys` default off (delta 6):** shortcuts act globally only for Ctrl/Alt/F-keys; Bill's rule was "every configured key is global". Either/or exists (`global_hotkeys on` = Bill's) but default differs; confirm Original pathway sets it on.
7. **Config editor load-time migrations of user data** (ConfigEditorWindow.cpp 2342, 2482, 3010; ~270 lines): rewrite files when opened. Keep only if the migration is needed by VP keys; otherwise gate.
8. **Restore-switch plumbing** (`dv_tunnel_fixes` m_bugHuntFixes ~45 lines, `magewell_recover_faults`, `capture_display_fixes`, `queue_timing_fixes`): off = Bill, but the switches are VP-only and the formatter triage (A.3 class 3) says remove; decide per switch whether "off = Bill" is wanted as class (d) or the fix becomes unconditional.
9. **Edits to Bill's tests that delete assertions:** MotionRendererTests (bicubic plane_upscaler default), WindowResizeLayoutTests (1000x1350 placement), ConfigFileTests (8 lines), AnalysisDiagnosticReplayTests, ConfigEditorWindowTests (-5.5k). Each needs the behaviour change that forced it named, or it is a silent behaviour change.
10. **DeckLink `ycbcr_12bit_capture` default v210 (Bill refused such a signal)** and Magewell BTB/WTW range choice: capability changes on capture, rule (b) only if the old path was a bug (owner BTB/WTW rule is the justification).
11. **Rate-bound widening 10-240 -> 10-500 Hz and log-cadence-by-seconds (DIV-RATE-01/02)**: ten small files; behaviour identical at Bill's rates; no bug. Either argue as PR "rate-independent" or revert to Bill's literals plus a VP-side wrapper.
12. **MotionConfig `motion_engine` auto|vp|smooth_motion** (Bill-side struct): selectable, but `auto` may change which engine owns cadence on Bill's profiles; verify Original pathway.
13. **IMediaSideData.h re-vendored** (+242/-1), **fork infrastructure e3** (AGENTS.md, packaging manifest, tools/*.ps1, build-identity in Directory.Build.targets, `tools/test_libplacebo_variants.py` asserts API 26 while the plugin API is 44: stale test): needs a rule class (proposal: (f) fork infrastructure, listed but not reverted).
14. **Rebase hazards, not classification:** 74 overlapping files; ConfigEditorWindowTests, LibplaceboVideoRenderer.cpp, VideoProcessorDlg.cpp, DisplayTopologySession.cpp, AnalysisLumaSource.cpp, the DV formatter and the vcxproj/filters/sln are the high-conflict ones. Bill's tip has DebugLogAsync.h and DisplayMonitorInventory.h that duplicate integ work.

Suggested next steps (not done here): merge `origin/v1.4.02-beta` into a scratch branch and re-run this sweep so (b) rows lose anything Bill already fixed; script the per-hunk table for the three biggest source files; one Opus read of items 2-6.
