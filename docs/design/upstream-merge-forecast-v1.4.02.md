# Upstream merge forecast: Bill's v1.4.02-beta into feat/skeleton

Read-only forecast, 2026-10-24. Nothing was merged, edited or built in any source
worktree. Method: `git merge-tree --write-tree` (no worktree touched), `git log`, `git diff`.

| | |
|---|---|
| Ours | `feat/skeleton` 908bd088 (B3 audit fixes; one commit after fd60b650 - same 37-file conflict set at fd60b650) |
| Upstream | `origin/v1.4.02-beta` b1415da4 (billslack2/videoprocessor, PR #193 merge) |
| Merge base | 83b7ffea |
| Upstream since base | 134 commits (~100 non-merge), 197 files, +46k/-2.5k (most is tests) |
| Ours since base | 739 commits, 979 files, +260k/-7.4k |

Policy reminder: billslack2 is read-only for us. Never push to it, never open PRs against it or
against enchywastaken. The merge lands on our side only.

## 1. Textual conflicts

37 files, about 94 hunks, about 6,200 conflicted lines. Everything else (about 160 of Bill's files,
including IRenderer.h, DisplayRefreshRatePolicy, SubtitleBoxDetector.cpp, ActivePictureEvidence,
all his new headers and tests) merges cleanly at the text level.

| File | Hunks | Lines in markers | Our feature it touches |
|---|---:|---:|---|
| vprenderer/LibplaceboVideoRenderer.cpp | 26 | 3,868 | almost everything; see below |
| GUI/StatsOverlayWindow.cpp | 2 | 803 | Ctrl+I readouts, "Fallbacks engaged" line vs Bill's OSD rewrite (columns, sizing, HDR rows, CIH scale) |
| Config/ConfigEditorWindow.cpp | 12 | 460 | settings reorganisation (feat/settings-preview: 13 hunks), LUT page vs Bill's explicit-Apply LUT reload, display profiles page, OSD size, script hints |
| vprenderer/AlphaQueuePolicy.h | 2 | 212 | delay budget sheds (ShouldShedHeldBacklog/Excess, HeldShedFrames), LED look-ahead walk vs Bill's SelectActivePicturePreparation + depth-scaled stale age |
| SubtitleCutPasteShader.h | 4 | 112 | our rgba32f + configured chroma upscaler (audit 2026-10-16) vs Bill's independentAlpha / deinterlace / source processing params |
| vprenderer/TemporalRenderer.h | 1 | 71 | Extended Range history relabel (HistoryFrame) vs Bill's subtitle CaptureComposition |
| Test vcxproj.filters / ConfigFileTests / ConfigEditorWindowTests | 8 | 151 | test registration only (keep both) |
| AnalysisLumaSource.h | 2 | 58 | our full-range -> limited sample domain (fullRangeYuv) vs Bill's AnalysisLumaRange + row helpers |
| LibplaceboPluginVideoRenderer.cpp, PluginApi.h, PluginExports.cpp | 5 | 121 | API 42 (ours) vs 29 (Bill); export lists |
| MainConfigSchema.h | 1 | 24 | our general keys (positional Validate) vs scale_osd / stats_osd_size_percent |
| ConfigurationRpcProtocol.h, ConfigurationRpcServer.cpp | 3 | 23 | RPC op 9 = ProjectorTest (ours) vs GetDisplayModes (Bill) - wire collision |
| configuration-public-values.tsv | 1 | 17 | capture_path/upload/dejudder keys vs output_display.* (keep both) |
| VideoProcessorDlg.cpp/.h | 4 | 34 | GUI hooks (LUT Apply, display modes) |
| VideoTimingController, DirectShowTimingClock, ALiveSourceVideoOutputPin.h | 3 | 36 | same clock-overflow bug fixed twice (our bug hunt 2 M2 vs Bill 4a1f58f4): take ours |
| CR210DolbyVisionTunnelFormatter.cpp | 1 | 5 | same sceneMin fix twice: take either |
| UnifiedProfileRuntime.cpp/.h, ActiveProfileStatus.h, RendererProfileConfig.h, DisplayTopologySession.cpp, AnalysisFrame.cpp | 6 | 44 | profile runtime (subtitle default Off), includes |
| 3rdparty/libplacebo-beta README + **libplacebo-377.dll (binary)** | 2 | - | our FL35 fork (clamp 0001-0002, rgba32f 0003, set_texture 0004) vs Bill's rebuild with translated-ROI-copy |
| vcxproj (Config, ConfigTests, Test), tools/test_libplacebo_variants.py | 4 | 49 | registration |

LibplaceboVideoRenderer.cpp's large hunks (by side size):

- @21355 (Bill 951 lines): his subtitle composition/backing in the render-load path vs our Stage B
  phase picture / vpTimeline refresh logic.
- @27534 (ours 985) and @28549 (ours 117): our custom-hook submission (NLS + OUTPUT-stage hook owning
  quantisation = sigma-delta, Stage 12 re-base planes, Stage B phase picture) vs Bill's
  NlsHookRecovery + `temporalRenderer.Render(..., pictureProcessingEpoch)`. This is the most
  dangerous hunk: it is the actual draw call.
- @22729 (ours 347 / Bill 66), @26414 (Bill 264), @25121: subtitle crop/placement and the
  temporal presentation keys vs our viewport / configured screen.
- @33498 (Bill 371): his prepared outward lookahead (prime raw measurements, three-source
  certificate) vs our AnalyzeActivePictureLookahead (LED walk + source_resolution look-ahead).
- @35700, @36654, @36808: RenderLoop - his preparation window and subtitle-mode plumbing vs our
  source-resolution look-ahead, motion_engine choice, dejudder extras callbacks.
- @194: anonymous-namespace helpers (both add; keep both).
- @30646: queue-overflow drop (our diagnostics record vs his discard reason; keep both).

## 2. Semantic overlaps (the real work)

### 2.1 Lookahead / presentation timing vs our fixed total delay budget - LOW-MEDIUM risk

Bill's VP-0205 is **analysis** lookahead (crop and caption proof on the first eligible frame), not
presentation buffering. Its final form (acbae401) explicitly *defers* the "adaptive presentation
policy" to VP-0208 and keeps beta's presentation admission unchanged. What it brings:

- `PresentationAnalysisBudget` (new to us; from 12b2dfaf): a per-iteration ticket of at most
  2 ms (1 ms unqualified) for optional synchronous analysis. It never waits or moves PTS.
  Compatible with the fixed delay, but it reads display phase (vsync QPC) and a GPU timer: check it
  against our render-ahead / open-loop windowed mode (no DXGI stats) - it falls back to 1 ms, fine.
- Bounded background workers (raw, subtitle, component, outward) and a prepared-proof cache.
  Extra CPU and memory bandwidth on the render machine; never joined on the render path. Must be
  covered by our timing gates (strict absolute delay gate; render stalls were exactly what /m:4
  builds caused).
- `SelectActivePicturePreparation`: walks up to three distinct future sources *already queued*.
  No extra queue depth or wait. Our LED walk (`SelectLookaheadWalk`) and Bill's preparation union
  must be merged into one walk: indices = union(active picture, LED wanted depth, 3 outward).
- **`StaleBacklogAgeThresholdMs(rate, desiredDepth)`**: Bill raises the stale threshold to
  (depth+1) periods. Our `staleBeyondTarget` path already subtracts the target's own wait, and our
  held-queue shed rules (ShouldShedHeldBacklog / ShouldShedHeldExcess / HeldShedFrames) own
  shedding when the delay budget's time hold is active. Taking Bill's threshold on top would
  double-count the target and leave backlog in place - the total delay would exceed the budget
  (the defect gate 5/7 fixed). Resolution: keep our rule on the VP timeline / held path; Bill's
  only on the legacy frame-count path with staleBeyondTarget off.
- VP-0208 (future, upstream backlog): "deliberate stable buffering ... some steady added latency is
  acceptable". This is where a real conflict with the owner's fixed total delay budget / 4-3-2
  ladder / queue cap could arrive. Not in this merge; watch it.

Our B3 entrance ring, render-ahead, AlphaQueuePolicy caps and motion look-ahead are untouched by
upstream logic; the conflicts there are textual (adjacent code).

### 2.2 Smooth-motion source processing vs our motion engine, sigma-delta, BTB/WTW - HIGH risk

Bill's e5666b7e / 1a4d081a / 480d3af6 (docs/SOURCE_RATE_PROCESSING.md): libplacebo smooth motion now
processes each retained source through scaling, tone/gamut mapping, source hooks, NLS and the
calibration LUT into an output-sized **RGBA16F** cache with a *separate* renderer, then blends in
linear float, and the output pass keeps only overlays, range conversion and output-depth dither.

- This is conceptually our B3 "ring A: process each source once, stateful stages advanced once",
  implemented differently and only for the libplacebo smooth-motion engine. Under our
  motion_engine exclusivity (VP motion OR libplacebo smooth motion) it changes the pixels of every
  profile that selects smooth motion. Byte identity vs r6 will fail for those cases by design.
- Our OUTPUT-stage custom hooks (sigma-delta for the FL35 8-bit link, LED compensation,
  OutputEncodingRepair off on 377) must run once in the output pass, never baked into the cache.
  Our rgba32f FBO option (fork 0003) vs his RGBA16F cache: precision step-down on that path.
- BTB/WTW (owner rule: kept in all cases): float caches can hold out-of-range values, but linear
  blending of PQ/limited content and the "range conversion" in the output pass must be checked;
  the chain gate's BTB/WTW cases must run with smooth motion on.
- Calibration LUT now applied in source preparation on that path; our VP calibration LUT stage
  placement must match.
- "Refuse raw fallback after failed subtitle preparation" is a new automatic behaviour: per our
  no-silent-fallbacks rule it must be visible (Ctrl+I / Fallbacks engaged), not log-only.

### 2.3 Subtitles (VP-0207 backing/overlay, cut-paste, measurement cadence) - MEDIUM risk

About 20 subtitle files change upstream (+~6k incl. tests): backing lip, cue continuation, local
glyph proof, bar-strip clearance, handoff, measurement stride two above 30 fps, Temporal subtitle
fill retired from the picker, and **subtitle processing defaults to Off without a profile**
(b75c2e7b). Overlap with ours:

- Our subtitle detection core moved on chore/early-migrations (see section 3); Bill's edits to
  SubtitleBoxDetector.h, SubtitleBoxLookahead.h (+771) and SubtitleTemporalFill.h land in the
  **forwarding headers**, not the module files. They need a manual port.
- SubtitleCutPasteShader: keep our precision/chroma quality *and* his new parameters.
- Owner rule "legible is enough; keep detection/placement correct": the Off default is an owner
  decision (below).

### 2.4 LUT explicit Apply / API 29 vs our calibration LUT stage and plugin API 42/43 - LOW-MEDIUM

- Bill's VP-0203 adds `IVideoRenderer::RequestCalibrationLutReload()` **in the vtable** (between
  ResetSubtitlePreviewMode and GetDolbyVisionLuminanceStatus) and calibration files reload only on
  an explicit Config Apply. IRenderer.h merges cleanly, which hides that it is a vtable layout
  change: the merged API must be bumped past every claim. Ours 42, feat/dv-l8 claims 43 -> the
  merge takes **44** if dv-l8 lands first (else 43 and dv-l8 renumbers). Build-ID check stays the
  real guard.
- Optional exports outside the version: GetHdrStatus (RendererHdrStatus), StartupPresentation
  readiness V1. Keep both alongside our EndHeldLedDrive etc.
- Our automatic calibration writes LUTs and expects them to take effect; with explicit-Apply only,
  the calibration controller must call the reload itself after a run. Our LUT files sit in
  `src/VP-V/modules/output/calibration_lut/` on the migration branch; Bill's LibplaceboDisplayLut.cpp
  is not moved, so no path issue.

### 2.5 Black reference vs BTB/WTW, black level, CNTR - LOW risk (but one domain clash)

Bill's "black reference" (c48eb134, 13f43bcb) is **crop/matte detection**: qualify a raised
full-range perimeter floor before it gains crop authority. It does not touch rendered black
level, BTB/WTW or the CNTR/software split; DV 1bc052ce only preserves authored black L1 scenes in
the HDR status. The clash is in the analysis domain: our AnalysisLumaSource maps full-range
samples to limited codes inside Sample() (`fullRangeYuv`, audit 2026-10-16; the LED look-ahead
depends on it) while Bill adds `AnalysisLumaRange` and Full-range-aware thresholds. One range
model must win or Full-range sources are converted twice (wrong crop floor, and possibly LED
peaks). Engineering decision, test with full-range RGB and P010 fixtures.

### 2.6 Display modes, timing, DV, capture, config, Config GUI

- Advanced display mode profiles (`output_display.mode_policy auto|advanced`, monitor,
  allowed_modes, restore_previous_mode) and DisplayRefreshRatePolicy changes merged cleanly. Check
  against our "derive from measured rates, never hard-code" rule and the owner's fixed FL35
  2560x1600@60 chain; mode_policy should not switch the FL35 away from 60 Hz unless asked.
- RPC operation 9 collision (ProjectorTest vs GetDisplayModes): renumber Bill's to 11 on our side.
- MainConfigSchema: our comment says Validate() indexes rules by position with the file_* keys
  last - insert scale_osd / stats_osd_size_percent before them (or fix Validate).
- Config GUI: Bill's new controls (display profiles, OSD size, LUT Apply, script hints hidden when
  Off, Temporal fill removed) must be placed in feat/settings-preview's VpvOrganisation.h units.
- Capture: no Magewell/DeckLink capture changes upstream beyond the clock-overflow fix (duplicate
  of ours). DV: only the sceneMin duplicate and the HDR status rows. No conflict with feat/dv-l8
  beyond the API number.
- libplacebo-377.dll: keep our fork binary; rebuild the fork (vmcpherson2012-dev/libplacebo vp-fl35)
  with Bill's translated-roi-copy patch as 0005 (exact-pixel CopySubresourceRegion, no API change),
  update README/manifest/hash. Do not take Bill's DLL: it drops our clamp/rgba32f/set_texture
  patches (sigma-delta would fail to link/run).

## 3. Interaction with chore/early-migrations

Of the 84 moved paths, Bill modifies five, all subtitle detection core:

| Old path | Bill | Carried by rename? |
|---|---|---|
| SubtitleBoxDetector.cpp | +240/-3 | yes (clean into src/VP-V/modules/input/source_analysis/) |
| SubtitleBarEvidence.h | +49/-2 | yes |
| SubtitleBoxDetector.h | +20 | **no - conflicts in the forwarding header** |
| SubtitleBoxLookahead.h | +771/-31 | **no - conflicts in the forwarding header** (module copy stays without his changes) |
| SubtitleTemporalFill.h | +7 | **no - conflicts in the forwarding header** |

Merging upstream into the migration branch gives 44 conflicted files (the 37 + those 3 forwarders,
VideoProcessor-Lib.vcxproj, and three test files). Port rule: keep the forwarder, apply Bill's diff
for that file to the module path, register his new files in the module MANIFEST. Bill's other new
subtitle headers (SubtitleBackingLip.h, SubtitleCueContinuation*.h, SubtitleLocalGlyphProof.h,
SubtitleBarStripClearance.h) arrive at old-style paths and belong to the same module.

Other helper branches add almost nothing: settings-preview 37 files (13 ConfigEditor hunks),
dv-l8 / net-runner / motion harness 37 files each (same set).

## 4. Recommended sequence

1. **Not now.** Finish the current order first: B3 audit fixes -> verified baseline (chain gate,
   strict delay gate, DV suite, byte identity vs r6). Merging upstream before the baseline would
   make every gate failure ambiguous.
2. **Merge upstream right after the verified baseline, before the helper branches.** Reason: the
   helpers barely add conflicts, but upstream changes the libplacebo binary, the draw call, the
   smooth-motion path and the API number; helpers then rebase onto one known baseline. Exception:
   if early-migrations (M1) is merged first by plan ("merge after B3"), upstream must then be merged
   after it using the forwarder port rule - acceptable, adds about 1 h. Preferred order:
   baseline -> upstream -> M1 (early migrations) -> settings / dv-l8 / net runner / motion -> M2
   and the central-code migration. Do it before M2 for sure: M2 moves the renderer and queue code
   that holds 26 of the hunks, and rename detection on a 40k-line file split will not carry them.
3. **One merge commit** of b1415da4 (his PRs are already integrated and reviewed upstream; replaying
   ~20 PRs would repeat the renderer conflict 20 times). Resolve in a fresh worktree from the
   baseline tip, then follow-up commits for the semantic items (stale threshold, walk union, API
   bump, RPC renumber, schema order, range model, visible fallbacks, libplacebo fork 0005).
4. **Gates after the merge:** full x64 Release build (standard + Beta renderer); CPU suites (ours +
   Bill's ~1,800 native regressions); byte identity vs r6 on VP-motion / Original pathways (expected
   identical), with smooth-motion and subtitle-on cases re-baselined explicitly as the new
   reference; chain gate whole-chain matrix incl. BTB/WTW with smooth motion; strict absolute delay
   gate at 4/3/2 frames, 24/50/60p, and render-ahead; DV suite; B3-on harness; one live subtitle
   session (owner) and a long-uptime clock check.

**Effort (agent wall-clock):** merge + textual resolution 4-6 h (renderer 3-4 h alone); semantic
follow-ups 3-5 h; libplacebo fork rebuild with 0005 1-1.5 h; builds and CPU suites 2 h; GPU gates
(chain + delay + byte identity + DV) 4-6 h. **Total about 14-20 h**, plus about 1 h if after M1.

## 5. Owner decisions needed

1. **Subtitle default Off without a profile** (upstream b75c2e7b): adopt, or keep our current
   default for the owner chain?
2. **Smooth-motion source processing** (RGBA16F cache, separate renderer, LUT/NLS in preparation):
   adopt Bill's path for the libplacebo smooth-motion engine (pixels change, re-baseline), or keep
   ours behind a switch? Interaction with our rgba32f option and sigma-delta.
3. **VP-0208 stance**: Bill plans adaptive buffering with "some steady added latency acceptable".
   Confirm the fixed total delay budget stays authoritative on our side; we would take only
   analysis-side parts of VP-0208.
4. **Explicit Apply for calibration LUT reloads**: fine for manual LUT edits; confirm automatic
   calibration may reload its own LUT without a Config Apply.
5. **Advanced display mode profiles**: leave `mode_policy` at auto (no change to the FL35 60 Hz
   chain), and where they sit in the reorganised Config.
6. **Timing**: confirm "after the verified baseline, before helper merges and M2".
