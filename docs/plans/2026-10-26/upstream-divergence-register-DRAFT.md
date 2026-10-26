# Upstream divergence register (DRAFT for docs/design/upstream-divergence-register.md)

Status: first-pass draft from the 2026-10-25 sweep of integ/batch1 `0925f594` against `origin/v1.4.02-beta` `8c453746`
(three-dot diff: 181 Bill files modified). Every row needs re-checking against Bill's tip before it is final: integ has not merged Bill's last 153 commits.

## Rule

Every change to Bill's files is one of:

- **(a)** a host call site into VP code;
- **(b)** a behaviour change that is a genuine bug fix in Bill's code, worth a PR to Bill, registered here with its justification;
- **(c)** an include-path or project-registration change for moved VP headers and sources;
- **(d)** an either/or hook point where Bill's behaviour remains selectable (the Original pathway is Bill's behaviour);
- anything else reverts to Bill's code.

A reverted row stays in the register as "closed" with the commit id, so a later rebase against Bill's tip can be audited.
PRs to billslack2 need explicit per-PR owner approval; "PR candidate" below never means a PR was opened.

## Row format

One row per cluster, grouped by Bill file. Columns: ID (DIV-`<area>`-NN), lines (new-file span or approximate size), class (a/b/c/d), what differs,
Bill's behaviour and why it is a bug (or which hook it is), upstream status (PR candidate / already in Bill's tip / not worth a PR / stays as hook /
closed), pinning test, date.

**Justification text is paraphrased from the in-code comments of each change and is not yet reproduced against Bill's build; the pinning-test column is the expected test, not always verified.** Provenance tags in code comments ("audit 2026-10-16", "bug hunt 2/3", "hunt 2") map to the VP audit and bug-hunt reports; the repro lives in those reports, not here.

## Class (b) rows

### Timing

| ID | File(s) | Class | What differs | Bill's behaviour / why a bug | Upstream | Test | Date |
|---|---|---|---|---|---|---|---|
| DIV-TIM-01 | `IntegerMath.h` (new section), `WallClock.cpp`, `VideoTimingController.cpp`, `microsoft_directshow/DirectShowTimingClock.cpp`, `microsoft_directshow/live_source_filter/ALiveSourceVideoOutputPin.h` | b | overflow-safe round(a*b/div) with a 128-bit intermediate for every tick conversion | `ticks * 1e7` overflowed int64 after about 10.7 days at 1 MHz (25.6 h at 10 MHz QPC); the old fallback truncated to whole seconds so 24-60 frames shared one timestamp, and DirectShowTimingClock froze the graph clock | **partly in Bill's tip already** (VideoTimingController.cpp and DirectShowTimingClock.cpp use a 128-bit mul/div there): on rebase take Bill's text for those two; keep ours for WallClock.cpp and the pin | IntegerMath tests, TimingClock tests | 2026-10-25 |
| DIV-TIM-02 | `TimingClock.cpp` | b | `TimingClockDiffMs` keeps the sub-millisecond part | integer division dropped the fraction, biasing short intervals | PR candidate | hunt 2 13-timing L2 test | |
| DIV-TIM-03 | `vprenderer/AlphaCadenceCorrectionPolicy.{h,cpp}` | b (gated by `queue_timing_fixes`) | capture rate measured on QPC from frame arrivals (least-squares line), warm-ups in seconds, bounded phase, disabled policy forgets its phase | capture rate taken from the nominal fallback while display rate is measured: the two clocks disagree and the phase drifts | PR candidate once the switch is removed (restore switch is e2 plumbing) | AlphaCadence tests | |

### Capture backends

| ID | File(s) | Class | What differs | Bill's behaviour / why a bug | Upstream | Test | Date |
|---|---|---|---|---|---|---|---|
| DIV-COM-01 | `ACaptureDevice.h`, `magewell/MagewellCaptureDeviceDiscoverer.h`, `blackmagic_decklink/BlackMagicDeckLinkCaptureDeviceDiscoverer.h`, `VideoState.{h,cpp}` | b | `m_refCount{0}`; VideoState copy constructor starts a new count | refcount uninitialised, and a copied VideoState inherited the source's count: use-after-free or leak depending on construction path | PR candidate (trivial) | MagewellCaptureTests | |
| DIV-CAP-01 | `blackmagic_decklink/BlackMagicDeckLinkCaptureDevice.{h,cpp}`, `...Discoverer.cpp` | b | `CaptureCallbackBoundary` around driver callbacks (no exception crosses COM), idempotent StopCapture, forced-stop state, frame period from the card's own mode, throw -> status for informational state, tolerant duplicate arrival/removal | exceptions thrown on the driver thread, asserts only in Debug, StopCapture not idempotent contrary to ACaptureDevice's contract | PR candidate (split into 2-3 PRs by area) | MagewellCaptureTests analogue; add DeckLink fake | |
| DIV-CAP-02 | `magewell/MagewellCaptureDevice.{h,cpp}` (plus `MagewellBackend`) | b (+ e2 plumbing: `magewell_recover_faults` restore switch) | recoverable capture faults restart the device; thread abort no longer leaves CAPTURING; pool starvation checked on every pass; HDR InfoFrame read/log on change only (was 4x per second); AVI BT.2020 read; 8-bit ARGB range per BTB/WTW rule | aborted capture thread left state CAPTURING; a fault ended capture until a manual restart; log flooded at the signal poll rate | PR candidate, switch removed first | MagewellCaptureTests (+137) | |
| DIV-CAP-04 | `ColorSpace.{h,cpp}`, `BlackMagicDeckLinkTranslate.{h,cpp}` | b | Rec.601 flagged on a non-SD raster maps by measured-rate family (625 vs 525) instead of throwing; `Translate` extra params | `Translate` threw for Rec.601 on HD/UHD rasters: capture refused | PR candidate | translate tests | |
| DIV-CAP-05 | `BlackMagicDeckLinkTranslate.cpp` | b | reserved CTA-861.3 EOTF values 4-7 map to SDR and report | reserved values treated as valid transfer functions | PR candidate | translate tests | |
| DIV-CAP-06 | `DisplayMode.{h,cpp}` | b | `NanosecondsPerFrame()` defined | declared and defined with an empty body (no return statement): undefined behaviour | PR candidate (trivial); the shared refresh-rate policy part is DIV-RATE-01 | DisplayMode tests | |
| DIV-CAP-07 | `vprenderer/AlphaNativeRgbIngress.h`, `VideoFrameEncoding.cpp` | b | R10b/R10l excluded from libplacebo's native upload; helpers cover R10b/R12L | no D3D11 texture format matches their bit order: nothing is shown | PR candidate | VideoFrameFormatterTests | |

### Rules and shaders

| ID | File(s) | Class | What differs | Bill's behaviour / why a bug | Upstream | Test | Date |
|---|---|---|---|---|---|---|---|
| DIV-RULE-01 | `DisplayRuleExpression.h`, `StateVariables.cpp` | b | `${source_rate}` rounds to nearest (59.94 -> 60); exponent literals (`1e-3`) parse; three-valued (Kleene) evaluation; bare Boolean variable means true | floor made `${source_rate} > 59` miss 59.94p and `== 23` match 23.976p; `1e-3` was split as a range; unavailable variables made `!(x == y)` true while `x != y` was false. One selection change: a negated comparison on an unavailable variable no longer matches | PR candidate, flag the `!` semantic change in the PR | rule tests | |
| DIV-RULE-02 | `microsoft_directshow/MadVRShaderLoader.{h,cpp}` | b | on a key press only rules that declare the chord take part; group selections commit atomically; test seams | pressing a key pinned unrelated members/reset unrelated groups whose automatic condition happened to be true; a failure part-way left earlier groups changed | PR candidate | MadVR shader loader tests | |
| DIV-RENDER-01 | `vprenderer/NlsHookRecovery.h` | b | `WithdrawHook` points `pl_render_params` at a caller-owned hook list | params pointed at a local that died before the render finished: dangling pointer | PR candidate | NlsHookRecovery tests | |

### Config, GUI, display, logging, ABI

| ID | File(s) | Class | What differs | Bill's behaviour / why a bug | Upstream | Test | Date |
|---|---|---|---|---|---|---|---|
| DIV-CFG-01 | `QueueConfiguration.h` | b | `TryGetHardCapacityString` reads legacy `[command_line]` / `[general]` queue_size | `[general] queue_size` was validated by MainConfigSchema but never read | PR candidate | QueueConfiguration tests | |
| DIV-CFG-02 | `ConfigCore/ConfigEditorCore.{h,cpp}` | b | UTF-8 BOM kept out of key parse, wall-clock backup names, last-error defined only after failure, overwrite guard for external file | BOM made the first key unreadable by the editor (VP's reader skips it); backup names did not sort | PR candidate | ConfigEditorCoreTests (+56) | |
| DIV-GUI-01 | `ConfigurationLiveApply.h` (AltGr/observer part), `GUI/VideoProcessorDlg.{h,cpp}`, `GUI/FullscreenVideoWindow.cpp`, `RendererProfileConfig.h` (chord grammar) | b | AltGr (Ctrl+Right Alt) never matches a Ctrl+Alt shortcut; keyboard auto-repeat dropped; unconfigured keys logged once; background observer never swallows keys VP does not act on; chord grammar exactly as the player's | AltGr+letter fired shortcuts while typing characters; held keys re-dispatched commands; observer swallowed keys | PR candidate (separate from the `global_hotkeys` default: see sweep item 6) | shortcut tests | |
| DIV-GUI-02 | `RendererResetPolicy.h`, `RendererResetCoordinator.h`, `VideoProcessorDlg.cpp` (retry/teardown), `VideoProcessorApp.cpp` (bounded dialog destruction) | b | bounded back-off for automatic renderer retry (count clears only after sustained rendering), bounded shutdown wait | a failing renderer retried without bound and spun the UI thread; exit could hang | PR candidate | RendererResetPolicyTests | |
| DIV-DISP-01 | `DisplayTopologySession.{h,cpp}` | b (+ `capture_display_fixes` switch: e2 plumbing) | startup recovery never stops the program; target native size ranked first; inactive paths get modes | recovery failure stopped startup; ApplyMaximumMode ignored the native mode | PR candidate; **conflicts** with Bill's tip, which now uses `DisplayMonitorInventory` here | topology tests | |
| DIV-LOG-01 | `DebugLogRetention.h`, `DebugLog.h` | b | in-session 100 MB rollover to `.r1`-`.r3`, reopen handling | startup rotation only: one long session or a log storm grew vp.log unbounded | PR candidate **but overlaps** Bill's new `DebugLogAsync.h`: re-evaluate after merging the tip | DebugLogRetention tests | |
| DIV-ABI-01 | `vprenderer/LibplaceboPluginVideoRenderer.cpp`, `VPRenderer/LibplaceboPluginExports.cpp` | b | API version and build ID checked before any other call; refused DLL listed; no C++ exception crosses the C ABI | an old or foreign DLL was called before the check; exceptions crossed the DLL boundary | PR candidate | LibplaceboPluginProxyTests | |
| DIV-AN-01 | `AnalysisLumaSource.{h,cpp}`, `AnalysisFrame.cpp`, `SubtitleBoxDetector.cpp` | b? (**e2 until justified**) | `Sample()` maps full-range sources (8/12-bit RGB, DV SDR-tunnel RGB, full-range YCbCr) into the limited 10-bit code domain; evidence contract version 2 | Bill emitted raw codes, so detectors saw full-range black at code 0 and white at 1023 while limited sources use 64/940 | needs a repro before it counts as a bug; five of Bill's tests were edited | AnalysisRecoveryParity/Dv, NativeRgbAnalysis, SubtitleBoxDetector, SubtitleOpaqueCardRefinement | |

### DV formatter

Class (b) rows come from `plan-f11-triage-and-small-items.md` table A.3 and keep their IDs there (DIV-DV-01 whole-raster placement vote, DIV-DV-02 flat-header placement proof, DIV-DV-03 `LooksLikeTunnelWith` and SDR exit, DIV-DV-04 truncated-frame rejection, DIV-DV-05 time-based loss limit and ID-wrap guard, DIV-DV-06 loss in source pictures, DIV-DV-07 interlaced never locks, DIV-DV-08 R10b/R10l word layouts (pending owner Q1), DIV-DV-09 2-of-3 packet vote (pending owner Q2)). Class 2 (hook extraction) and class 3 (restore switch, removal) are tracked in that plan.

## Class (a) host call-site files (no per-row justification needed beyond the call-site doc)

Listed with their design note: `vprenderer/LibplaceboVideoRenderer.{h,cpp}` (docs/design/*-callsites.md: entrance, output, motion-timing, repeat-reuse, wp11, wp12, wp15, wp17, wp23, silent-fallbacks, m2-dv, efficiency), `IRenderer.h`, `vprenderer/LibplaceboRendererPluginApi.h`, `LibplaceboPluginVideoRenderer.{h,cpp}`, `RendererId.{h,cpp}`, `ActiveProfileStatus.h`, `AnalysisLumaSource.h` (shared luma plane, WP-23, offered upstream), `SceneDetector.cpp`, `ActivePictureEvidence.cpp`, `SubtitleBoxDetector.cpp`, `GUI/StatsOverlayWindow.{h,cpp}`, `GUI/ConfigurationRpcServer.cpp`, `vprenderer/TemporalRenderer.h` (`HistoryFrame`, default nullptr keeps Bill's behaviour).

## Class (c) include/registration

`VideoProcessor-Lib.vcxproj(.filters)`, `VideoProcessor-VPRenderer(.Beta).vcxproj`, `VideoProcessor-GUI.vcxproj` (renderer ProjectReferences), `VideoProcessor-Config.vcxproj`, `VideoProcessor-Test.vcxproj(.filters)`, `VideoProcessor-ConfigTests.vcxproj`, `VideoProcessor-BetaTests.vcxproj`, `VideoProcessor.sln`, `Directory.Build.targets` (VP-V include root), `magewell/MagewellSdk.props` (`VpCiNoMagewell`, CI only).

## Class (d) either/or hooks

| ID | Where | Hook | Bill's behaviour selected by | Status |
|---|---|---|---|---|
| DIV-D-01 | `vprenderer/LibplaceboVideoRenderer.cpp` `ApplyOriginalPathwaySettings`, slot `Runs()`/`TagSlotHooks` guards | Original pathway (ADR-0019) | `pipeline_original_pathway on` | ~150 lines; modularity-integ-batch1.md says the Original pathway was verified against bill-v1.4.02 (OriginalPathway.h, 56 entries); re-run after rebase |
| DIV-D-02 | `3rdparty/libplacebo-beta/include/libplacebo/renderer.h`, `shaders/custom.h`, `lib/libplacebo-377.def` | fork render-parameter fields and `pl_mpv_user_shader_set_texture` | default false / NULL = upstream behaviour | additive; the DLL itself is a separate ruling (sweep item 4) |
| DIV-D-03 | `shaders/NLS.glsl`, `shaders/NLSPlus.glsl` | `sampling` parameter | `sampling = 0` = bilinear (Bill) | **default is 1 (Catmull-Rom)**: not yet either/or at default; see sweep item 5 |
| DIV-D-04 | `MotionConfig.h` | `motion_engine` auto/vp/smooth_motion | `smooth_motion` | verify `auto` under the Original pathway (sweep item 12) |
| DIV-D-05 | `vprenderer/LibplaceboVideoRenderer.cpp` calibration LUT reload (hunk 11159) | policy `CalibrationLutHostPolicy` | Bill's explicit Apply is the other choice (note says "Bill's explicit Apply") | confirm default equals Bill's 1 s probe under the Original pathway |
| DIV-D-06 | `ConfigurationLiveApply.h` `global_hotkeys` | on = every configured key global (Bill's rule) | `global_hotkeys on` | default is off: sweep item 6 |

## Open (not yet a row)

(e2) and (e3) items are in the sweep note, section 5: `D:\codex\videoprocessor\skeleton-notes\divergence-sweep-firstpass.md`. They become rows only after the owner decides revert, gate, or register.
