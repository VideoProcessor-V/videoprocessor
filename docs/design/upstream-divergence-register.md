# Upstream divergence register

Every place where VP-V behaves differently from Bill's (upstream) code. Each entry is either a
PR-worthy bug fix or an either/or option whose Original setting is Bill's behaviour.

This is the ONE register. The Bill v1.4.02 merge (2026-10-31) merged its rows here (section
"Bill v1.4.02 merge" below) and carried the 2026-10-25 sweep rows at the end; those still need
re-verification against Bill's tip where marked. PRs to billslack2 need the owner's approval.

| Id | Area | Divergence | Scope | Original / off | Kind |
|---|---|---|---|---|---|
| DR-L1 | libplacebo renderer cache (`LibplaceboVideoRenderer.cpp`, `Impl::ApplyResyncLibplaceboFlush`) | On the frame that carries a FULL resync (source change in place, renderer/queue reset, seek, calibration patterns, file discontinuity, format change, large timestamp break), `pl_renderer_flush_cache` drops libplacebo's frame-mix textures and resets its detected peak, and the temporal mixer's retained frames are cleared. No shader rebuild: the fork's body (`src/renderer.c`; no fl35 patch touches it; unchanged in the 388 + fl35 rebuild) destroys only `rr->frames` textures and calls `pl_reset_detected_peak`. Bill's `Reset()` keeps the cache (its comment concerns compiled programs, which this does not touch). | VP pathway only, `resync_event = on` | Original pathway, `resync_event` off/shadow: Bill's behaviour (no flush) | VP-only option (either/or) |
| DR-R3-scene | Scene detector (Bill's `SceneDetector`) | `resync_event = on`: the detector is also `Reset` (same generation) on the first frame of every FULL resync, including FULL causes Bill's `m_sceneDetectorGeneration` does not see (seek without a reset, calibration patterns, in-place format change, counter backwards). A continuity slip changes nothing (the comparison across the slip already reports a real change as a cut). | `resync_event = on` | off/shadow/Original: Bill's generation triggers only | Either/or option |

## Bill v1.4.02 merge (a591ad3b, 2026-10-31)

Owner decisions are binding: in every either/or the Original pathway (`pipeline_original_pathway on`,
`src/VP-V/core/OriginalPathway.h`, now 66 rows) is Bill's code exactly.

| Id | Area | Divergence | Original / off | Kind |
|---|---|---|---|---|
| DR-M-TEMPORAL | `TemporalRenderer` vs entrance stages (`TemporalHistoryPolicy.h`) | While Bill's temporal renderer (smooth motion mixing / deinterlacing) is active it owns the draw: VP's entrance stages (re-base, VP deband, detail) pause, listed under Fallbacks engaged, and Bill gets the captured picture's crop. Before, a re-based crop reached his raw history. A history of VP-processed pictures is the allowed alternative, not built. | Original: no entrance stages; Bill's renderer untouched | Either/or (owner decision 1) |
| DR-M-PRESENT | Presentation queue vs render-ahead (`PresentationOwner.h`) | `render_ahead on`: VP's chain depth/latency; Bill's `present_every_vsync` withheld (Fallbacks engaged). Off: Bill's `presentation_queue_depth` / `present_every_vsync` / verified `max_frame_latency`; the VP-owned chain now applies Bill's verified depth (it reset the device to 2). The delay budget counts the presenting owner's swapchain frames. | `render_ahead off` (Original) = Bill | Either/or (owner decision 2) |
| DR-M-REPEAT | Bill's display repeat cadence | Paused (Fallbacks engaged) while VP's per-refresh timeline or de-judder extras schedule refreshes. | Original has neither: Bill | Either/or (c-item 11) |
| DR-M-DITHER | Config dithering choices | Original: Bill's 4 (blue noise, ordered fixed, white noise, off). VP-V: all 16 incl. error diffusion and Auto; his retired-value display rule never rewrites a VP-V value. Sigma-delta (temporal_dither) unchanged. | Original = Bill's menu | Either/or (owner decision 3) |
| DR-M-LUT | Calibration LUT reload | VP-V also reloads a changed LUT file by itself (WP-12); the merge had dropped this call site. | Original: Bill's explicit Apply only | Either/or (c-item 3) |
| DR-M-SUBQ | Subtitle composite source processing | Outside Bill's temporal composition VP-V passes the configured chroma upscaler / anti-ringing and float32 (audit 2026-10-16); Bill passes none. | Original: none (Bill) | Either/or (c-item 8) |
| DR-M-DV | DV tunnel gate (`DolbyVisionTunnelGate.h`, `VideoProcessorDlg.cpp`) | VP-V: hand-off probe and YCbCr decline; ARGB/BGRA admitted only while bytes are preserved, ANDed with Bill's predicate. R10b/R10l containers behind `SetAcceptR10Containers` = `dv_tunnel_fixes` (DIV-DV-08). | Original: Bill's `DolbyVisionTunnelCanEnable` exactly, R10 refused | Either/or |
| DIV-AN-01 | Analysis luma domain | Was (b?, e2); now an either/or on `source_coding_fixes`: VP-V maps full-range sources to the limited domain (`CodesRange()` reports Limited so consumers interpret it once); Original: Bill's delivered codes + range tag. Bill's five tests restored unmodified. | `source_coding_fixes off` | Either/or |
| DR-M-DEBANDGRAIN | `deband_grain` | VP-V default off (VP's dither adds grain); Original row `deband_grain on` added so Bill's deband keeps libplacebo's grain. | Original = Bill | Either/or |
| DR-M-QUEUE | Backlog recovery (`VpQueueRules.h`) | `queue_timing_fixes on`: VP's rule; off or Original calls Bill's new depth-scaled `ShouldRecoverBacklog`. | Original = Bill | Either/or |
| DIV-CAP-01 | DeckLink delivery fps | `DeliveryModeFps` (card GetFrameRate first; table fallback returns 0) instead of `FPS()` throwing inside the driver callback. | n/a | Bug fix, PR candidate |
| DIV-CAP-02 (ext.) | Magewell | 8-bit LIMITED RGB captured as RGB10/R10l (BTB/WTW); Bill's quiet `recoverStall` keeps valid-pending until the first complete frame and never republishes an invalid state (it flapped); VP's recoverable-fault path covers copy timeout / restart failure, exclusive with his. | n/a | Bug fix, PR candidate |
| DIV-CAP-08 | `VideoState.cpp` P010 layout | Odd file-source sizes use a rounded two-plane layout where Bill's `GetP010FrameLayout` rejects them. P010 keeps Bill's enum number; NV12/P016 follow. | n/a | Bug fix |
| DIV-LOG-02 | `DebugLog.h` | Bill's `DebugLogAsync` transport kept; the crash-state recent-lines ring fills with file logging off; a gap line after a failed reopen (DIV-LOG-01 rollover ported onto his transport). | n/a | Bug fix |
| DIV-CFG-03 | Config shortcuts | A profile shortcut equal to a fixed application shortcut blocks Apply. | n/a | Bug fix |
| DIV-CFG-04 | Config VP-V choice keys | Keep "Inherited / not set"; a saved `auto` is live (Bill's concrete-only / retired-auto rules apply to his keys only). | Bill's keys: Bill | Either/or by key ownership |
| DIV-CFG-05 | Config starting profiles | Display/Motion starting profiles seeded in memory, not a pending change (Bill wrote defaults into a clean file). | n/a | Bug fix |
| DIV-CFG-06 | Config retired downscaler spellings | Migrated once at load with a backup; displaying a profile never mutates the document. | n/a | Bug fix |
| DR-M-ABI | Plugin API 45, ActiveProfileStatus v14, Config RPC wire 3 | Union of both sides' exports/fields; Bill's RPC numbers kept, ours after. Host, renderer and Config deploy as a set. | n/a | Version (no behaviour) |
| DR-M-LIBPLACEBO | `3rdparty/libplacebo-beta` | Bill's API 388 source + fork patches 0001-0004 (FL35 limited-range clamp keeping BTB/WTW, rgba32f FBOs, `pl_mpv_user_shader_set_texture`). OutputEncodingRepair stays off (388 fixed PRE_OUTPUT). | patch defaults = upstream behaviour; the clamp is a bug fix | Fork (DIV-D-02) |
| DR-M-DROP | Queue overflow drop count | The merge counted an overflow drop twice (Bill's `droppedTotal` plus VP's old trailing add); counted once again. | = Bill | Merge repair |
| DR-M-HOLD | Start-up hold black clear | The merge lost `pl_frame_clear` under `if (rendered && startupHoldBlack)`, leaving the hold guarding Bill's `CacheDisplayFrame`; restored. | = Bill (hold off on Original) | Merge repair |
| DR-M-LOOKAHEAD | Active-picture look-ahead gate | VP's LED/intent observers walk the queue without Bill's active-picture frames when his gate refuses them; his look-ahead statistics count only his attempts. | = Bill when observers are off | Merge repair |

Open owner questions from the merge: edge blanking `blank_*` (VP-V viewport keys, default 0) is not in the Original table (a row, or a screen-installation property?); Bill's AGENTS.md text naming an EDID .bin as a release asset is superseded by the publishing rule (EXE/ZIP/vp-update.json only).

## Carried rows (2026-10-25 sweep draft, against Bill 8c453746; re-verify against a591ad3b)

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
