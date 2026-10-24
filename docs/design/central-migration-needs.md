# Central code migration: needs assessment (2026-10-24)

Read-only assessment of the code that the M2 plan
(`docs/design/migration-m2-plan.md` on `chore/early-migrations`) leaves in place "by design": the renderer host
`src/VideoProcessor-Lib/vprenderer/LibplaceboVideoRenderer.cpp`/`.h`, the renderer plugin API, the GUI
host/dialogs, `shaders/`, `DebugLog.h` and the other large shared files.

All line numbers and sizes are taken at `feat/skeleton` **fd60b650** unless stated otherwise. Effort
is **agent wall-clock hours** (not human-developer time). Nothing here was built or run.

Related: ADR-0017 (migrate when touched), ADR-0022 (Bill's renderer host stays and calls into the
modules), `docs/design/VP-pipeline-final.md` §5 (skeleton wiring), `src/VP-V/README.md`,
`docs/design/module-findings-2026-10-24.md` (on `chore/early-migrations`), queue item 144 (merge
Bill's v1.4.02-beta before M2).

## 1. The measurements

### 1.1 File sizes (fd60b650)

| File | Lines | Notes |
|---|---:|---|
| `vprenderer/LibplaceboVideoRenderer.cpp` | **32,711** | 2.0 MB, about 500k tokens: no agent can read it whole. 161 `#include`s, 764 `DebugLog::` calls, 157 lock sites, 361 `activeSettings.` reads |
| `vprenderer/LibplaceboVideoRenderer.h` | 551 | pimpl: almost everything is in `struct Impl` inside the `.cpp` |
| `vprenderer/LibplaceboRendererPluginApi.h` | 95 | ABI version 42, a hand-maintained counter (see 2.1) |
| `vprenderer/LibplaceboPluginVideoRenderer.cpp` | 812 | EXE-side proxy of the DLL |
| `VideoProcessor-VPRenderer/LibplaceboPluginExports.cpp` | 155 | DLL exports |
| `IRenderer.h` | 740 | the renderer interface (every status getter adds a vtable slot) |
| `RendererProfileConfig.h` | 3,364 | profile schema, included by the GUI, Config and renderer |
| `MainConfigSchema.h` | 510 | |
| `DebugLog.h` | 508 | 53 includers; 4 commits since 10-01: stable |
| `VideoProcessor-GUI/VideoProcessorDlg.cpp` / `.h` | 20,463 / 1,470 | 316 `CVideoProcessorDlg::` methods; Ctrl+I text, hotkeys, status polling |
| `VideoProcessor-GUI/VideoProcessorApp.cpp` | 2,455 | |
| `VideoProcessor-GUI/StatsOverlayWindow.cpp` | 1,094 | |
| `VideoProcessor-Config/ConfigEditorWindow.cpp` | 16,329 | being reorganised by `feat/settings-preview` (VP-V section in `src/VP-V/config/`) |
| `shaders/` | 18 files, 588 KB | runtime deploy layout, not C++ |

### 1.2 Churn (fd60b650, commits since 2026-10-01)

| File | Non-merge commits | Our lines since the 10-14 Bill merge base (83b7ffea) | Bill's lines on v1.4.02-beta since the same base |
|---|---:|---:|---:|
| `LibplaceboVideoRenderer.cpp` | **230** (of 709 repo-wide; 1 in 3 commits) | +15,240 / -3,300 | **+5,396 / -915** (54 commits, all 10-14..10-23) |
| `ConfigEditorWindow.cpp` | 191 | +7,010 / -349 | +824 / -47 |
| `RendererProfileConfig.h` | 93 | +382 / -20 | +75 / -5 |
| `VideoProcessorDlg.cpp` | 58 | +3,222 / -474 | +260 / -189 |
| `LibplaceboVideoRenderer.h` | 46 | +228 | +39 |
| `LibplaceboRendererPluginApi.h` | 21 | +54 | +29 |
| `StatsOverlayWindow.cpp` | 7 | +69 | +198 / -560 |
| `DebugLog.h` | 4 | +129 | 0 |

Other evidence:

- **52 merge commits** touched the renderer file since 10-01; 39 merge messages since 10-01 mention
  conflicts (most of the round-4/round-5 integration merges of 10-18).
- The file grew from 20,771 lines (10-14) to 32,711 (10-24): **+57 % in ten days**. Bill's copy grew
  20,771 -> 25,252 over the same days.
- Of the 3 local branches updated since 10-20 that differ from fd60b650 in code, all 3 touch it
  (`feat/dv-l8` +120, `feat/skeleton` +207, `fix/vpscalerender-motion` +17).
- **Bug density proxy:** 97 of the 230 commits on the file since 10-01 are fix/bug/regress/repair/
  audit commits (42 %), against 242 of 709 repo-wide (34 %). Every audit today (B3, module findings
  sections 2 and 6-7) ended in "hand-off: renderer file" because no other agent may edit it.
- **Who writes it:** 302 commits by billslack2, 250 by us (all time). Bill is still very active in it.

### 1.3 What Bill changes vs what we change (added lines, 83b7ffea -> each tip, keyword match)

| Topic | Bill (v1.4.02-beta) | Us (fd60b650) |
|---|---:|---:|
| subtitles / captions / backing | **870** | 333 |
| crop / active picture / look-ahead / proofs | **887** | 523 |
| de-judder / render-ahead / interpolation / motion | 63 | **1,772** |
| entrance / re-base / deband / detail / texture restore | 18 | **702** |
| intent / ambient / black intent | 1 | **340** |
| calibration / display LUT | 21 | **174** |
| LED / Dynamic Drive | 16 | **173** |
| Dolby Vision | 2 | **160** |
| fallbacks | 17 | **178** |

Bill's hunks are in `Impl` (3,641 added lines), the anonymous-namespace settings code (686),
`GetFrameRateAndPPM` (364), `RenderLoop` (310) and `AnalyzeActivePictureLookahead` (215). **The two
sides work on almost disjoint concerns that happen to share one file.** That is the central fact
for the options below.

## 2. Question 1: what lives in the renderer file

The file has three parts: an anonymous namespace of free functions (lines 174-5190), `struct
LibplaceboVideoRenderer::Impl` (5372-25795, about 20,400 lines), and the public
`LibplaceboVideoRenderer::` methods (25827-32711, about 6,900 lines). `RenderLocked` alone is
**7,612 lines** (18145-25757), with 32 parameters.

### 2.1 Responsibility clusters

Owner column: **B** = Bill's original code (and still where he works), **V** = VP-V additions,
**M** = mixed. Sizes are approximate.

| # | Cluster | Where (fd60b650) | ~Lines | Owner |
|---|---|---|---:|---|
| 1 | Utilities: clocks, paths, JSON escape, SHA-256, colour-space translation (`TranslateColorSpace`, `ToDisplayEotf`) | 174-947 | 770 | B |
| 2 | **Settings: `RendererSettings` struct (205 fields), fingerprint, display rules, value parsers, `ApplyDisplayRule` (1,140 lines), VP pipeline/de-judder readers, `ReadRendererSettings`** | 948-4400 | **3,450** | M (Bill's keys plus every VP key; `ReadPipelineSettings`, `DejudderMatcherSettings`, `ApplyOriginalPathwaySettings` are V) |
| 3 | Display refresh-rate switching, DisplayConfig, DWM timing, `ScopedDisplayRefreshRate`, refresh command rules | 4400-5190 | 790 | B |
| 4 | GPU timing scopes, dither pass totals | 5191-5371 | 180 | M |
| 5 | **Entrance / B3 ring**: `RunEntrance`, `RunMainEntrance`, ring, once-snapshot, B-as-A, rebase/deband/detail/crossfade stages, early upload | 5414-6430 | 1,020 | V |
| 6 | Capture unpack glue: RGB, DV-on-CPU, session fallbacks | 6430-6630 | 200 | M |
| 7 | **De-judder / render-ahead / VP phase timeline**: upload layout, render-ahead gate and timers, open-loop rate, `PublishDejudderStatus`, release | 6630-7460 | 830 | V |
| 8 | Subtitle quality, hook loader, LED gamma, **slot-hook tagging/ordering (`TagSlotHooks`, `OrderFrameSlotHooks`, `UpdatePipelineSlots`), `EngageFallback`/`ReleaseFallback`**, render-ahead budget, target primaries | 7460-8030 | 570 | V (skeleton wiring) |
| 9 | Impl state and presentation-evidence bookkeeping, retirement, backlog recovery, `RenderLoadSnapshot` | 8030-8780 | 750 | B |
| 10 | Calibration pattern present/readback, black frame | 8780-9050 | 270 | V (pattern) / B (black frame) |
| 11 | Shader cache load/save, DV/HDR output plan, `LogResolvedRenderOptions`, presentation-evidence scopes | 9050-9950 | 900 | M |
| 12 | Presentation crop/translation (`...retiringTranslationFitInspection`, 603 lines) | 9948-10551 | 600 | B |
| 13 | **Swapchain/output contract**: encodings, VP-owned swapchain, `ConfigureSwapchainOutput` (413), auto-limited fallback, BT.2020 signal | 10553-11490 | 940 | B (+ V fallbacks) |
| 14 | Display LUT load/attach/reject | 11487-11700 | 210 | B (V calibration-LUT stage selects) |
| 15 | **Settings apply**: render-option build (355), viewport apply, profile live-apply, pending settings, settings state publish | 11704-12970 | 1,270 | M |
| 16 | Shader rules, custom shader status, NLS hook mapping | 12967-13420 | 450 | M |
| 17 | Scene verification, motion invalidate, active-picture authority, remembered-edge return, sparse boundary, black-level traces | 13420-14250 | 830 | B (+ V motion invalidate) |
| 18 | **Analysis/crop/subtitle draw path** (one 1,620-line function) | 14259-15879 | 1,620 | B |
| 19 | **Intent interpreter / black intent / ambient** | 15884-16393 | 510 | V |
| 20 | **Motion engine and de-judder estimate, holds, `PresentDejudderExtras` (1,068)** | 16398-18104 | 1,710 | V |
| 21 | **`RenderLocked`** (7,612 lines; sub-areas by keyword density): | 18145-25757 | 7,610 | M |
| 21a | frame setup, DV metadata, HDR/tone | 18145-19745 | ~1,600 | M |
| 21b | intent/ambient application | 19745-20145 | ~400 | V |
| 21c | **subtitle + crop + caption presentation** | 20145-22945 | ~2,800 | B |
| 21d | entrance call + rebase/deband/detail | 22945-23345 | ~400 | V |
| 21e | LED measurement/compensation hooks, DV, tone hooks | 23745-24145 | ~400 | V |
| 21f | output hook chain: user shaders, de-judder cache, dither/sigma-delta, calibration LUT, slot order | 24145-24945 | ~800 | V |
| 21g | present, telemetry, post-stall advisor | 24945-25757 | ~800 | B |
| 22 | Public lifecycle: ctor/dtor, `OnVideoState` (158), `OnVideoFrame` (328), `EnqueueFrame` (178), queue delay hold, frame-rate/PPM, `Build`, shader/display rule selection, `ApplyApplicationState` (201), reset/retire | 25827-28320 | 2,500 | B (+ V queue delay hold, LED) |
| 23 | **Status getters for Ctrl+I / crash state / IRenderer**: output mode, output contract, scene timing, presentation timing, calibration, LED, intent, content intent, DV luminance, de-judder, source resolution, fallbacks | 28320-29190 + scattered | 900 | M (most new getters are V) |
| 24 | `AnalyzeActivePictureLookahead` (1,117) | 29192-30308 | 1,120 | B |
| 25 | Conversion loop (258), `RenderLoop` (1,646), queue generation, `SetState` | 30310-32711 | 2,400 | B (+ V B3 ladder, render-ahead pacing) |

Roughly: **Bill-owned core ~17,500 lines, VP-V glue ~9,500 lines, mixed ~5,500 lines.**

### 2.2 The other central pieces

- **Plugin API** (`LibplaceboRendererPluginApi.h`, `IRenderer.h`, proxy, exports): every new status
  getter is a vtable slot plus an API number. Branches claim numbers independently (dv-l8 claims 43;
  36/37/38/39 were each claimed and renumbered at 10-17 merges). The build-ID check makes the counter
  almost redundant.
- **GUI host** (`VideoProcessorDlg.cpp`, 20k): Ctrl+I text building, hotkeys, status polling,
  config apply. Bill touches it lightly (+260); we add a lot (+3,222).
- **Config editor** (`ConfigEditorWindow.cpp`, 16k): `feat/settings-preview` is already moving the
  VP-V pages into `src/VP-V/config/` (generated from `slots.json`). Treat that as the plan for it.
- **`shaders/`**: runtime layout; the manifests point at it. No migration need.
- **`DebugLog.h`**: stable, used everywhere. No migration need; if a VP-V home is wanted, a
  forwarder-free `src/VP-V/core/` wrapper is enough.

## 3. Question 2: the need

### 3.1 Cost of leaving it as is

1. **Merge conflicts between our own agents.** One in three commits lands in this file; every
   integration round of 10-18 recorded conflicts in it; today 3 of 3 code branches touch it. The M2
   plan itself is gated by it (blockers 2 and 3). Conflicts cost agent time and, worse, are where
   timing regressions slip in unnoticed (a wrongly resolved hunk in `RenderLocked` passes CPU tests).
2. **Audit difficulty and context limits.** At ~500k tokens, no agent or reviewer sees the whole
   file. Audits work from grep and slices, so cross-cluster interactions (for example entrance ring
   x render-ahead x LED measurement in the same 7,612-line function) are the ones they miss. Every
   findings document today has a "hand-off: renderer file" section because only one lead may edit
   it: a single-writer bottleneck.
3. **Bug density.** 42 % fix-type commits on this file vs 34 % repo-wide; the B3 audit's HIGH items
   both sit in renderer glue (stateful re-run after B-as-A; invisible "B per picture" fallback).
4. **Testability.** VP glue inside `Impl` is reachable only through the GPU capture harness
   (`RendererCaptureHarnessTests`, the 2.5 h timing gates) or not at all. That contradicts the
   owner's module principles (liftable, standalone tests) for exactly the features he cares most
   about (motion, entrance, LED, intent).
5. **Bill-upstream merges.** Leaving it as is keeps git's 3-way merge working line by line, which is
   good. But both sides are growing the same file fast (Bill +5.4k, us +15.2k in ten days), and they
   collide where VP hooks are spliced into Bill's functions (`RenderLocked`, `RenderLoop`,
   `ApplyDisplayRule`, `ReadRendererSettings`). The more VP code sits interleaved in his functions,
   the more of his edits conflict.

### 3.2 What splitting gains

- Parallel agents work in different files: the module that owns de-judder, LED or intent changes
  without touching the host.
- Each VP feature gets CPU-testable logic and a README-checked API (the M2 definition of done).
- The host shrinks back toward Bill's shape (~23k), so his future changes merge with fewer
  conflicts: his code is unchanged, and our insertions into his functions become one-line calls.
- Audits can read one cluster whole.

### 3.3 What splitting risks

- **Behaviour change.** Moving code out of `RenderLocked` changes evaluation order, lock scope or
  object lifetime unless done carefully. 157 lock sites and 361 `activeSettings` reads make this
  real.
- **Timing regressions.** Render-ahead, the delay ladder and B3 are sensitive to small changes in
  the render thread; CPU tests cannot see them. The only gates are the 2.5 h skeleton timing runs on
  this one laptop, and the FL35 render-ahead check.
- **Upstream divergence.** Moving or reformatting **Bill's** code (clusters 1-3, 9, 12-13, 18, 21c,
  21g, 22, 24, 25) turns each of his future hunks into a manual port: git does not follow code moved
  between files on merge. That would make "merging Bill's upstream changes must stay feasible"
  false within weeks, given 54 commits from him in ten days.

## 4. Question 3: options

Common test strategy for any change below:

- **CPU:** full suite single-threaded, read from the TRX (`/Logger:trx`).
- **Byte identity vs r6:** the harness byte-identity set (`ByteIdentical` to r6) for every pathway
  the moved code is on, B3 on and off. Mechanical extraction must be byte-identical; any difference
  is a defect in the move.
- **Timing:** skeleton timing gates (~2.5 h per run, GPU, this laptop, no concurrent builds per
  item 122). Group extractions so a gate covers several steps; bisect only on failure.
- **Chain gate** (Beta 377 first, GeForce) before any deploy built from the result.

### (a) Leave it

- Effort: 0 h now. Ongoing: conflict resolution at every integration (several agent hours per
  round, observed), and the single-writer hand-off bottleneck.
- Risk: low immediate, rising: growth of +1k lines/day, more audits ending in hand-offs, VP features
  untestable standalone.
- Bill merges: unchanged (line-by-line, but conflicts grow with interleaving).
- Verdict: not compatible with the owner's years-long, modular goal.

### (b) Extract the VP-V glue per module behind small interfaces

The host keeps Bill's core. Each VP cluster moves into its module as a **host adapter**
(`src/VP-V/modules/<unit>/<slot>/<Name>HostStage.{h,cpp}` or similar) with a narrow interface; the
renderer keeps one call per hook point, registered through `SlotRegistry` / `EntranceStages` /
`OutputHookOrder`. State moves out of `Impl` into the adapter object, owned by `Impl`.

| Step | Cluster(s) | ~Lines out | Est. h | Verify |
|---|---|---:|---:|---|
| b1 | Status getters' text/structs and fallback bookkeeping (23, part of 8) | 700 | 3 | CPU, Ctrl+I snapshot tests |
| b2 | VP settings readers (VP part of 2): `ReadPipelineSettings`, de-judder/LED/intent/DV/entrance keys into per-module readers called from `ReadRendererSettings` | 1,500 | 5 | CPU (settings fingerprint must be identical), Config suite |
| b3 | Calibration pattern/output (10, 23 calibration) | 450 | 2 | CPU, calibration harness |
| b4 | Intent interpreter + application (19, 21b) | 900 | 3 | CPU, byte identity (intent on/off) |
| b5 | LED hooks and status (21e LED part, `UpdateLedDrive`, suspend/resume) | 600 | 3 | CPU, LED tests, byte identity LED off |
| b6 | DV glue (21a DV part, 21e DV) | 500 | 3 | CPU, DV autotest (GPU) |
| b7 | Output hook chain glue (21f) | 800 | 4 | byte identity output side, BTB/WTW probes |
| b8 | Entrance / B3 ring (5, 21d) | 1,400 | 5 | byte identity entrance B3 on/off, **timing gate** |
| b9 | De-judder / render-ahead / motion engine (7, 20, 21 motion parts, `RenderLoop` render-ahead pacing) | 2,800 | 8 | byte identity, **timing gates**, FL35 render-ahead |
| b10 | Queue delay hold / B3 ladder hooks in the public layer (22, 25 V parts) | 400 | 3 | **timing gate**, delay totals per rate |

- **Effort:** ~39 agent hours of coding and CPU checks, plus 1-2 h per module for the M2
  README/key/fallback pass (~10 h), plus GPU checkpoints (Claude's): about 4 timing-gate runs
  (~10 h wall clock, serial) and 3-4 byte-identity/DV runs. **Total ~50 agent hours, ~3 working
  days elapsed** with gates.
- **Result:** host ~23k lines (close to Bill's own 25k shape); ~9.5k lines of VP code in modules
  with CPU tests.
- **Risk:** medium; high only for b8-b10 (render thread, timing). Mitigated by byte identity and by
  doing timing clusters last, one per gate.
- **Bill merges:** better than today. His functions keep their code; our insertions shrink to
  single calls, so his edits around them merge cleanly or with trivial conflicts.

### (c) Full split of the host into components

Split the whole host: queue/timing, presentation/swapchain, settings, analysis/crop/subtitle,
render loop, plus (b).

- **Effort:** ~110-140 agent hours, plus many gates (every component touches timing or the draw).
- **Risk:** high: re-architects Bill's render thread, swapchain and crop/subtitle state machines
  (the code he changes daily).
- **Bill merges:** effectively ends them. His +5.4k lines in ten days would each need manual porting
  into a different structure. Only sensible if the fork stops taking his changes, or if Bill adopts
  the same split upstream.
- Verdict: not now.

### (d) Hybrid (recommended shape)

(b) for every VP-V cluster, plus three low-risk, mechanical pieces of the central code that are VP's
or neutral, and an explicit **do-not-touch list** for Bill's code:

1. (b) steps b1-b10.
2. **Plugin API:** stop growing the vtable per feature. Add one generic status/query entry
   (`GetModuleStatus(slotId, buffer)`, structured text or key/values) so new VP status needs no ABI
   bump; allocate API numbers only at integration (never on a feature branch). ~3 h.
3. **GUI host:** move the VP-V Ctrl+I section building and VP hotkey handlers out of
   `VideoProcessorDlg.cpp` into `src/VP-V/` (GUI side), leaving one call each. Config editor: follow
   `feat/settings-preview` (already doing the same). ~5 h.
4. **Bill's code stays where it is, unmodified in structure** (clusters 1, 3, 9, 12-13, 17-18, 21c,
   21g, 22, 24, 25 and his parts of 2 and 15). Fixes to it stay in place, as small diffs.

- **Effort:** ~58 agent hours total (b ~50 + 8), ~3-4 working days elapsed with gates.
- **Risk:** as (b).
- **Bill merges:** as (b), plus fewer GUI/API conflicts.

## 5. Question 4: recommendation

**Option (d), hybrid**, in this order:

0. **Merge Bill's v1.4.02-beta first** (queue item 144), after today's B3 audit fixes, and
   re-verify the baseline. Any extraction before that makes his 54 renderer commits harder to merge.
1. **M1 deploy (~6-7 Nov)** as planned. No central extraction before M1 except the quick wins in
   section 7 that are rules or new files (no host edits while feature branches are open).
2. **M2 freeze window: step 0** (include sweep, forwarders) and the **quick wins that edit the host**
   (b1 status/fallbacks, plugin API generic status). They are small and fit the freeze.
3. **Pair each M2 module move with its glue extraction**, as a separate commit right after the
   behaviour-free `git mv` commit: b3 with step 3 (calibration), b7 with step 5 (dither) and steps
   4/6/7, b6 with step 14 (signal/DV), b8 with steps 11-13 (entrance), b9 with steps 16-17
   (motion), b10 with step 18 (queue/timing), b2 last with step 19 (control.config). b4/b5 (intent,
   LED) go in right after the LED re-cut. Doing them together avoids touching each module twice and
   reuses each M2 GPU checkpoint.
4. **Timing clusters last** (b8, b9, b10), each with its own timing gate run, never two at once.
5. **As segments (section 6):** the extractions land as stages of contiguous VP segments (S1
   entrance, S2/S3 output, motion). Each is a behaviour-free move first, then merged into one hook per
   segment behind a tolerance gate (s0-s4; s5 with the net runner).

**Gates required:** CPU suite per commit group; byte identity vs r6 (B3 on and off) after every
extraction that touches the draw path; the skeleton timing gates after b8, b9 and b10; the DV
autotest after b6; the chain gate (Beta 377 first) before any deploy from the result; a
Bill-merge dry run (`git merge --no-commit` of his then-current beta into a scratch worktree) after
b9, to confirm his changes still merge.

**Stopping rule (what stays central for good):**

- Everything Bill owns and still edits: frame queue and `RenderLoop`, conversion loop, swapchain and
  output contract, presentation crop and translation, the analysis/crop/subtitle path and
  `AnalyzeActivePictureLookahead`, refresh-rate switching, his settings keys and display rules,
  the lifecycle (`OnVideoState`/`OnVideoFrame`/`EnqueueFrame`/`SetState`).
- The libplacebo call sequence itself (`pl_render_image`, hook registration), because it is the
  boundary the owner's libplacebo strategy keeps.
- `DebugLog.h`, `shaders/` (runtime layout), the plugin ABI shell.
- Stop extracting when the remaining VP code in the host is only one-line calls into modules at
  registered hook points. Revisit Bill's code only if the fork stops merging his upstream or he
  splits it himself.

## 6. Owner principle: contiguous VP segments

> Owner (2026-10-24): "where possible our code hands to the next stage in our umbrella before being
> passed back to the appropriate stage, so any linked input/output code stays contained in our
> tested bed."

Applied to option (d), the unit of extraction is not just the module but the **segment**: a run of
adjacent VP-owned slots. Inside a segment, VP stages pass VP-owned textures directly from one stage
to the next. The host (and libplacebo) sees **one entry and one exit** per segment.

### 6.1 Segments today vs target

Counts are from the fd60b650 code: `RunEntrance` 5667-5685, the stage functions 6170-6430,
`RenderLocked`'s hook assembly 23790-24711 and `PresentDejudderExtras` 17036-18104. A "crossing"
is one place where control and a texture pass between host code and VP code. It is either a
host-written call per stage or a separately registered libplacebo hook.

| Segment | Slots in order (built now; *planned*) | Crossings today | Target |
|---|---|---|---|
| **S0 Analysis + motion estimation** (original frame) | source analysis (crop, scene, subtitle detection: **Bill's**); NVOF/matcher estimate; vector store/validity | Estimation reads separately uploaded native planes (`DejudderPlaneUpload`, 7153) and hands the vectors back to the host. The host then passes them to each consumer (warp, texture restore, temporal dither, LED look-ahead). In total: **1 upload + 1 estimate call + ~4 vector hand-offs** | One motion segment: `motion.Estimate(frame)`. Consumers then fetch vectors directly through `motion.vector_delivery`. Analysis stays in Bill's host code (a hand-back by design) |
| **S1 Entrance** (source size, once per source frame) | re-base -> *repair A* -> deband -> texture restore (+ detail pulse) -> *4K route* -> *refine* -> *chroma* | `RunEntrance` is already one host loop with VP-owned `pl_frame`s. But each stage has host glue (`EntranceRebase`/`Deband`/`Detail`, 50-110 lines each) with per-path branches for main, extras A/B, crossfade and B-as-A. That makes **3 host->VP calls per run, on up to 4 paths**; the ring/cache is held in `Impl` | One call: `entrance.Run(sourceFrame, path, vectors) -> pl_frame`. The ring, B3 cache, stage order and per-path rules live inside the segment |
| **S2 Output colour + drive** (PRE_OUTPUT) | SDR tone shape -> DV trims -> *Stage 13 appearance* -> LED measure -> LED compensation (adaptive gamma) -> *black level* -> calibration LUT (VP) | **5-7 separate libplacebo hooks**: tone shape (1+), DV trims (1), LED measure (1), LED compensation (1-2), calibration LUT (1). Host code registers and tags each one (`TagSlotHooks` x5). Each is a libplacebo pass boundary with libplacebo-owned intermediates. User PRE_OUTPUT shaders follow the LUT | **One** PRE_OUTPUT hook that runs the segment on VP-owned textures via `pl_dispatch` |
| **S3 Output finish** (OUTPUT) | adaptive downscale blend -> de-judder rendered-picture cache -> occlusion mask -> sigma-delta dither | **4-6 more hooks**: downscale blend (1-2), NLS, de-judder cache (1), sigma-delta (1-3), mask. Host logic also reorders them so the dither stays last (24435-24470) | **One** OUTPUT hook, with the dither last inside it. Together with S2: **2 crossings** for the whole output end, against ~10-13 today |
| **S4 Scaling refine** (POST_KERNEL, planned) | libplacebo scaler -> *hybrid scaler blend* -> *D* -> *scaling.refine* | not built (net runner wiring pending) | **One** POST_KERNEL hook that runs the VP part as a segment (NetRunner passes on VP textures) |
| De-judder extras / phase pictures | warp of S1's processed picture -> S2/S3 | `PresentDejudderExtras` re-assembles the hook list for every extra (17413-17467), so each extra repeats the same crossings | Extras call the same S1/S2/S3 segment objects; no second hook list |

### 6.2 Where a hand-back is unavoidable

- **libplacebo's main scaler** (`pl_render_image` kernel/EWA): the owner's libplacebo strategy is
  to own the code around it, not replace it. So S1 ends before the scaler and S4 starts after it.
- **Tone/gamut mapping, CMS, peak detection and the DV decode (libdovi):** the colour engine stays
  in libplacebo, so S1 -> libplacebo -> S2 is a fixed boundary. DV tunnel pictures are decoded
  inside libplacebo, so an S1 stage that needs the decoded DV picture cannot run before it (queue
  items 132-133).
- **libplacebo's output encoding between PRE_OUTPUT and OUTPUT:** this is why S2 and S3 are two
  segments, not one. They can merge into a single OUTPUT segment only if VP owns the encoding step.
  VP already repairs that step (`OutputEncodingRepair`, for the PRE_OUTPUT encoding bug). Decide
  this when S3 is built, not now.
- **User shaders (mpv format) placed between VP stages:** today each one is a libplacebo hook, so
  each one splits a segment. Either restrict them to segment edges, or run them inside the segment
  through the NetRunner's mpv-pass shim (B1/R3), which already executes mpv-format passes on VP
  textures.
- **Presentation, swapchain, subtitle/crop placement and overlays:** these are Bill's host code.
  VP segments hand their result back before them.

### 6.3 The segment runner and how it fits (d)

- **Core type** (`src/VP-V/core/SegmentRunner.h`, header-only, C++14): a segment holds an ordered
  list of stage objects taken from `SlotRegistry` for its zone. Each stage implements:
  - `Contract In()/Out()`: size, format, colour domain, range (including BTB/WTW), field/vector
    validity;
  - `Run(ctx, in, out)` on VP-owned textures.

  The runner checks `OrderValidator` and the contracts when settings change, not on every frame. It
  owns the intermediate texture pool, records `SlotTiming` per stage, and reports downgrades
  through `FallbackStatus`.
- **Host adapter:** one object per segment in the host (`EntranceSegment`, `OutputColourSegment`,
  `OutputFinishSegment`, `ScalingRefineSegment`). The host makes one call, or registers one hook
  whose callback calls the segment. That is exactly the "one-line call at a registered hook point"
  that (d)'s stopping rule asks for. Under (d), the glue extractions b3-b7 land **as stages of
  S2/S3**, not as separate host adapters, and b8 becomes S1.
- **Testing:** each segment is testable on its own:
  - CPU tests cover order, contracts and fallbacks.
  - A small GPU fixture feeds a synthetic or captured texture that matches the entry contract and
    checks the exit (golden image within tolerance, BTB/WTW probes). It needs neither the renderer
    nor the 2.5 h harness.
  - The renderer harness then checks only the wiring: each segment is called once, with the right
    inputs.
- **Timing and cost:** each removed libplacebo hook removes a pass boundary, which costs at least one
  full-frame read and write of an intermediate (a registered hook is never free, R2). Two changes
  should save measurable GPU time per refresh:
  - going from ~10-13 output hooks to 2;
  - extras reusing the segments instead of re-assembling hooks.

  Measure the saving with `SlotTiming` before and after; do not assume a number. Fusing adjacent
  stages into fewer passes becomes possible only once a segment owns its intermediates; it is a
  later step.
- **Byte identity:** merging separate hooks into one segment changes the intermediate formats and
  pass boundaries (libplacebo's FBO format vs the segment's pool). So the merge **cannot be assumed
  byte-identical**. Do it in two commits:
  1. A behaviour-free move: the segment object registers the same hooks, in the same order, from
     one host call. This is byte-identical to r6.
  2. The merge into one hook, checked against a tolerance gate and accepted by the owner. The gate:
     at most 1 code value of difference at the output bit depth, BTB/WTW probes unchanged and
     sigma-delta statistics unchanged.

### 6.4 Effort and place in the sequence

| Step | What | Est. h | When |
|---|---|---:|---|
| s0 | `SegmentRunner` core, contracts, CPU tests, GPU fixture | 5 | M2 freeze, after b1 (new files, no host risk) |
| s1 | S2 + S3 behaviour-free: hooks registered by segment objects (carries b3-b7) | +3 over b3-b7 | with M2 steps 3-7 and 14 |
| s2 | S2/S3 merged into one PRE_OUTPUT + one OUTPUT hook; extras reuse them | 6 | after s1, before the timing clusters; tolerance gate + chain gate + owner review |
| s3 | S1 entrance segment (carries b8) | +3 over b8 | with M2 steps 11-13; timing gate |
| s4 | Motion segment and direct vector delivery (carries part of b9) | +2 over b9 | with M2 steps 16-17; timing gates |
| s5 | S4 scaling-refine segment | 3 | when the net runner / hybrid scaler wiring lands, not in M2 |

Segments add about **22 agent hours** to (d), for **~80 agent hours in total** (about 4-5 working
days elapsed with gates). Without s5, which waits for D, it is ~77 h.

## 7. Question 5: quick wins (low risk, reduce conflicts now)

Ordered by value for risk. The first three need no host edit and can start before M1.

1. **Growth rule (now, 0 h of code):** new VP features land in their module's files; the host gets
   only the call site and a hook registration. Reviewers reject new >50-line blocks in
   `LibplaceboVideoRenderer.cpp`. Stops the +1k lines/day.
2. **Plugin API numbers allocated at integration only (now):** feature branches mark "API: next" and
   the integrator assigns the number at merge (dv-l8's 43 claim is the next collision).
3. **Status text builders as new files (now, ~2 h):** new `src/VP-V/core/StatusText.*` building the
   Ctrl+I/crash-state lines for de-judder, LED, intent, fallbacks from plain structs, with CPU tests.
   Branches start calling them; the host's old builders switch over in the freeze (b1).
4. **Fallback bookkeeping (freeze, ~1 h):** the module-findings hand-off list (log-only downgrades
   that need `EngageFallback`) is done as one batch in the freeze, not spread over feature branches.
5. **VP settings readers as tables (freeze or b2, ~5 h):** per-module key tables (key, default, range,
   reader) in `src/VP-V/config/`, driven by `slots.json`; `ReadRendererSettings` calls them. This also
   fixes the key-list drift found in the module findings. The settings fingerprint must stay
   identical (existing `LibplaceboRendererSettingsFingerprintForTest`).
6. **Diagnostics out (freeze, ~2 h):** VP-only diagnostic captures and trace switches (render-ahead
   logs, de-judder hold diagnostics, entrance counters) behind one diagnostics adapter. Bill's
   subtitle capture and local-boundary diagnostics stay.

## 8. Summary

| | (a) leave | (b) VP glue out | (c) full split | (d) hybrid |
|---|---|---|---|---|
| Agent hours | 0 (ongoing conflict cost) | ~50 | ~110-140 | **~58; ~80 with segments (section 6)** |
| Risk | rising | medium (timing clusters high) | high | medium |
| Host size after | 32.7k and growing | ~23k | ~5k + components | ~23k |
| Bill merges | as today, worsening | better | ends them | better |
| Fits the owner's modular goal | no | mostly | yes | **yes, within the fork constraint** |
