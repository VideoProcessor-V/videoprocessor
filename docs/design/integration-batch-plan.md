# Integration batch 1: master implementation plan

Status: **PLAN**, written 2026-10-24 on `docs/integration-batch-plan` (from `feat/skeleton` 30a42479).
Planning only: nothing here has been built or run. Effort is **agent wall-clock hours** (vp-estimates-agent-time),
not human-developer time.

Audience: any agent, or a fresh session after a usage pause. Read section 0, then the status board
(section 1), then only your work package (section 3) and the brief template (section 7). Every
WP cites its sources (file and section). Read the cited text instead of re-deriving it.

Owner decisions this plan implements (queue memory `vp-post-reset-queue.md` items 92-173, esp. 161,
168-173): before the next large test procedure and before the batched Bill merge, do the following as
ONE batch:

1. finish the helper verifications;
2. merge the helper branches into the integration line;
3. apply the drafted patches: silent fallbacks, byte-identical efficiency fixes, segments S1-S3,
   motion/timing extraction plus the queue/timing policy interface, net runner wiring, B3 re-audit
   fixes; B3 stays default ON;
4. extract ALL remaining VP glue (VP settings readers last);
5. close every B3 fallback natively, delete the pre-B3 code, freeze the r6 reference captures;
6. run independent audits (correctness and efficiency) of every work package;
7. run ONE large test procedure;
8. then the batched Bill merge, then M1 deploy prep.

Owner addition (2026-10-24, later): a VP-owned **frame analysis store** computed once per source
frame and read by all VP stages (WP-23, scheduled early; its consumers migrate inside their WPs).

**Dates (owner note 2026-10-24):** M1 (~6-7 Nov) is an estimate, not a lock. Quality and structure
come first. Do not compress steps, skip audits or skip tests to hit a date. Section 6 gives a
realistic estimate, not a deadline.

---

## 0. Standing rules (every WP, every brief)

Sources: memory files named in brackets. The brief template in section 7 repeats them in short form.

| Rule | What it means here | Source |
|---|---|---|
| Host = call sites only | VP logic lives in `src/VP-V/...` module files. `LibplaceboVideoRenderer.cpp`/`.h`, `VideoProcessorDlg.cpp`, `ConfigEditorWindow.cpp` receive only one call per hook point plus a hook registration. Reviewers reject new blocks of more than ~50 lines in the host | vp-host-call-sites-only; central-migration-needs.md §7.1 |
| Bill's files | Never move, rename or claim ownership of a file that exists in Bill's beta (`git cat-file -e origin/v1.4.02-beta:<path>`; the drift guard `VP_UPSTREAM_BETA` on `chore/early-migrations` fails such a move). VP code may be **extracted out of** his files, leaving one call. Bill-owned **clusters** (central-migration-needs.md §2.1: 1, 3, 9, 12-13, 17-18, 21c, 21g, 22, 24, 25, his parts of 2 and 15) get no new logic and no restructuring | vp-host-call-sites-only; queue 158 |
| Either/or | Where VP overlaps Bill's function, keep both as a setting. The Original pathway selects Bill's. Both are visible in status | vp-either-or-overlaps; queue 150 |
| Boundary-only testing of Bill's code | Do not re-test his internals. Test (a) one Original-pathway regression/byte-identity run and (b) each alternative at its slot contract (formats, range incl. BTB/WTW, timing/delay, visible fallbacks), once per option | queue 151 |
| No silent fallbacks | Every automatic downgrade appears in "Fallbacks engaged: ..." (Ctrl+I, Config status, Ctrl+Alt+D) plus a one-time transient notice. Never log-only | vp-no-silent-fallbacks |
| BTB/WTW | Never clip them. Every output-side change is checked with the BTB (code 4) and WTW (code 1019) probes | vp-btb-wtw-preserve |
| Grain | No stage averages grain away. Grain RMS and temporal independence are measured through each changed stage | vp-grain-faithful |
| General, not fixed | No hard-coded refresh rates, frame rates, cadences or resolutions | vp-general-not-fixed |
| Two-commit rule (segments, extractions) | Commit 1: a behaviour-free move, byte-identical. Commit 2: any merge or change, behind a switch, against a stated tolerance | central-migration-needs.md §6.3 |
| Plugin API numbers | Branches write "API: next". The integrator assigns numbers at merge (section 3, WP-2) | queue 145, 126 |
| Upstreamable track | Each WP states upstreamable vs VP-only. Upstreamable changes are clean, self-contained commits with no `src/VP-V` dependency, rebasable onto Bill's beta. **No PR to billslack2 without the owner's explicit approval for that PR; never against enchywastaken** | vp-upstreamable-track |
| CPU budget | Max **2 concurrent builds**, each through `bash D:/codex/vp-build-slots-run.sh <wt> <log> -- ...` with `VP_SLOTS=2`, `/m:4 /p:CL_MPCount=2`. Build only the projects you changed. Tests single-threaded (vstest one process, `/Logger:trx`). Config tests `-Shards 1` alongside a build. Delete stale `.iobj/.ipdb` before an LTCG link | vp-cpu-thread-budget; vp-ltcg-link-workaround; vp-beta-tests-trx |
| Magewell SDK | `/p:MagewellSdkRoot="C:\Program Files\MWCaptureSDK 3.3.1.1596\SDKv3"` on this laptop (no E: drive; AGENTS.md's E: path is for the other workstation) | vp-magewell-sdk-path |
| GPU | One GPU test at a time, run or released by the coordinator. Never during a timing gate | vp-cpu-thread-budget; queue 122, 134 |
| Timing gates exclusive | During a timing gate: no builds, no GPU, at most single-threaded CPU-only unit tests. A hold means **stop builds immediately** | queue 122, 156 |
| Kill only your own PIDs | Record the PID of every script or process you launch. On a hold, kill those PIDs (including scripts: TaskStop does not stop a script an agent launched) and **verify** they are gone. Never kill by name | vp-never-kill-by-name; queue 165 |
| Test scope | Feature/fix agents run only their area's test classes. Full CPU suite and Config suite only at merge checkpoints and the big test | vp-cpu-thread-budget (2026-10-18) |
| Idle agents | An agent waiting on a hold takes constraint-compatible work: read-only audits, docs, rebases, code-only drafts | feedback-idle-agents |
| Say projected time | Before anything over a minute, state the expected duration | vp-say-projected-time |
| Models | Leads/judgement/integration/audits: **opus**. **sonnet** only for mechanical sub-tasks (test registration, manifest regeneration, path updates, docs tables). Model always explicit in the brief | vp-agent-models |
| Pushes | Batched pushes to the fork remote (`fork`, VideoProcessor-V) only by the coordinator; no CI polling | vp-github-suspended |

---

## 1. Status board (update this table in every WP commit)

Markers: `TODO`, `WIP` (branch + last commit + the resume step), `VERIFY` (code done, tests pending),
`AUDIT` (waiting for or fixing an audit), `DONE` (commit), `BLOCKED` (reason).

| WP | Title | Status | Branch / worktree | Last commit | Resume from |
|---|---|---|---|---|---|
| WP-0 | Batch base: B3 default flip, r6 freeze, integration branch | WIP | integ/batch1 (integ-batch1) from feat/skeleton 0d88d9ac (tag batch1-base; includes B3 default on b76571c1, re-audit fixes e9f27487, and two env-gated WIP debug commits e4387610/0d88d9ac: VP_DEBUG_PHASE_AB, VP_DEBUG_PHASE_NOVECTORS, harmless when unset; remove after the A!=B fix) | 0d88d9ac | OWED: step 2-3 frozen r6 + frozen-b3base hash sets (GPU) |
| WP-1 | B3 re-audit fixes | TODO | feat/skeleton (skeleton) | - | step 1 |
| WP-2a | Verify feat/dv-l8 | TODO | feat/dv-l8 (dv-l8) | 5258c487 | step 1 |
| WP-2b | Verify feat/settings-preview | TODO | feat/settings-preview (settings-preview) | 02e127f0 + uncommitted | step 1 |
| WP-2c | Verify chore/early-migrations | TODO | chore/early-migrations (migrate) | 5d5390bd | step 1 |
| WP-2d | Verify fix/vpscalerender-motion | TODO | fix/vpscalerender-motion (motion-harness) | 2f8fb24c | step 1 |
| WP-2e | Verify feat/net-runner (PlNetRunner, fused) | TODO | feat/net-runner (nets) | 5805e928 | step 1 |
| WP-3 | Merge helpers into `integ/batch1` | WIP | integ/batch1 (integ-batch1) | - | merge 1 (feat/dv-l8 f22d9f19) |
| WP-4 | Silent-fallback call sites | TODO | integ/batch1 | - | step 1 |
| WP-5 | Byte-identical efficiency fixes + timers | TODO | perf/byte-identical (efficiency-fixes) | af7a8985 + uncommitted | step 1 |
| WP-6 | Output segments S2/S3 | TODO | feat/segment-runner (segments) | d0212534 + uncommitted | step 1 |
| WP-7 | Motion/timing extraction + queue policy interface | TODO | feat/motion-timing-extract (motion-timing) | c3e45812 + uncommitted | step 1 |
| WP-23 | VP frame analysis store (FAS): interface, producer, consumer migration | TODO | feat/frame-analysis-store (fas) | - | step 1 |
| WP-8 | S1 entrance segment + small B3 closures | TODO | integ/batch1 | - | step 1 |
| WP-9 | B3 fallback closure (rest), pre-B3 deletion | TODO | integ/batch1 | - | step 1 |
| WP-10 | Net runner wiring (A in S1, D in S4) | TODO | integ/batch1 | - | step 1 |
| WP-11 | Generic plugin status entry, status text, VP Ctrl+I/hotkeys out of the dialog | TODO | feat/vp-status-gui | - | step 1 |
| WP-12 | Calibration LUT glue | TODO | integ/batch1 | - | step 1 |
| WP-13 | Dither/output glue outside S3 | TODO | integ/batch1 | - | step 1 |
| WP-14 | DV glue | TODO | integ/batch1 | - | step 1 |
| WP-15 | LED / Dynamic Drive glue | TODO | integ/batch1 | - | step 1 |
| WP-16 | Intent/analysis glue (VP parts) | TODO | integ/batch1 | - | step 1 |
| WP-17 | VP diagnostics/captures adapter | TODO | integ/batch1 | - | step 1 |
| WP-18 | VP settings readers (last) | TODO | integ/batch1 | - | step 1 |
| WP-19 | Independent audits + fix rounds | TODO | read-only + integ/batch1 | - | step 1 |
| WP-20 | The ONE large test procedure | TODO | integ/batch1 | - | step 1 |
| WP-21 | Batched Bill merge | TODO | integ/bill-merge | - | step 1 |
| WP-22 | M1 deploy prep | TODO | integ/bill-merge | - | step 1 |

Mirror every status change in two places: (1) one line in the queue memory
(`vp-post-reset-queue.md`, a new numbered item), (2) the handover
`D:\codex\vp-handover\06-open-items.md` (commit there).

---

## 2. Branch and worktree layout

- **Integration line: `integ/batch1`**, worktree `D:\codex\videoprocessor\integ-batch1`, created in
  WP-0 from the `feat/skeleton` tip after the B3 default flip and the WP-1 fixes. `feat/skeleton`
  stays the B3 lead's branch until WP-0 ends; afterwards the integration line is the only place
  where host call sites change. **One writer for the host file at a time** (the integrator). Helper
  agents deliver new files plus a **call-site patch** (`docs/design/<topic>-callsites.patch`,
  `git apply --check` clean against the stated base), as the current drafts already do.
- Drafting branches stay in their own worktrees (segments, efficiency-fixes, motion-timing, nets,
  and a new `vp-status-gui` for WP-11). They rebase onto `integ/batch1` before their patch is
  applied.
- Existing worktrees are read-only to everyone except their owning agent. Never reuse another
  agent's worktree.
- Commit messages: imperative, WP prefix (`WP-6: ...`), WIP commits start with `WIP(WP-n):` and
  carry a "State:" block (see section 5). End every message with
  `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.

---

## 3. Work packages

Format per WP: goal; inputs; files/areas (with the Bill-owned check); contracts; steps; acceptance;
audit; upstreamable vs VP-only; hours; model; dependencies. "Big test" = WP-20. "Checkpoint" = build
of the touched projects plus the touched test classes (CPU), no GPU unless stated.

### WP-0 Batch base

- **Goal:** one committed, frozen starting point for the batch.
- **Inputs:** the uncommitted B3 default flip in `D:\codex\videoprocessor\skeleton` (ADR-0024,
  `VP-pipeline-final.md`, `VpvSection.cpp`, `SlotRegistry.h`, `PipelineSkeletonTests.cpp`,
  `ConfigEditorWindowTests.cpp`, the renderer file, `RendererCaptureHarnessTests.cpp`) and the
  untracked `docs/design/b3-fallback-closure.md` (queue 168-169).
- **Files:** skeleton lead's files only. The renderer change is the default flip (call site / default
  value), not new logic.
- **Steps:**
  1. Skeleton lead finishes and verifies the flip: build, CPU (PipelineSkeleton, ConfigEditorWindow
     tests), B3 harness + real-clip (GPU, ~40 min, one at a time). Commit, with
     `b3-fallback-closure.md`.
  2. **Freeze the r6 reference** (b3-fallback-closure.md case 13, done FIRST so it exists before any
     non-byte-identical change): run `ByteIdenticalToTheR6Baseline` once with the r6 DLL, record
     the inputs and the raw-R10 hashes as test data (`src/VideoProcessor-Test/data/r6-frozen/` or
     the harness's data folder; hashes only, **never video pixels**, AGENTS.md), and add a test
     `ByteIdenticalToFrozenR6` that compares the current default with the recorded hashes. Both the
     live and the frozen test must pass on the same commit. B3 on and off cases.
  3. Record the B3-on reference set at the same commit (`frozen-b3base`): the hashes of the
     default (B3 on) path for the same inputs. All later "byte-identical" steps compare against
     this set, because the r6 set covers only B3 off.
  4. Create `integ/batch1` and its worktree from that commit; tag it locally `batch1-base`.
- **Acceptance:** live r6 byte identity passes; frozen r6 and frozen-b3base tests pass on
  `batch1-base`; CPU green for the touched classes.
- **Audit:** none (covered by the B3 audits, queue 128/170).
- **Upstreamable:** no. **Hours:** 2-3 (incl. ~1 h GPU). **Model:** opus (skeleton lead).
- **Depends on:** nothing. **Blocks:** everything that changes pixels.

### WP-1 B3 re-audit fixes

- **Goal:** close the B3 re-audit findings (queue 170) on `feat/skeleton` before integration.
- **Inputs:** queue 170 (re-audit of fd60b650..d0212534); queue 128/140 (earlier fixes);
  `VP-pipeline-final.md` §3.8c.
- **Items:** MED-1 transfer double-counted when the arrival time is not a hardware frame-start
  time: add `arrivalIsFrameStart` per source/card (stale/callback times, cards without hardware age,
  pulldown clock) and count the transfer only when true; MED-2 near-black fix partial (1/16-1/2 px
  grainy dark areas): extend the "drawn as themselves" rule only if grain-safe, and make
  `NearBlackTimelineVsPlainProbe` assert a bound; LOWs: detail stages dropped on one Extra fallback
  path, stale fill notice, lenient test parser, jitter depth-14 coverage, grain stats excluding still
  pixels.
- **Files:** VP: `DelayBudget`/queue-hold code (VP parts), `FrameInterpolator.cpp`, harness tests.
  The arrival flag is a field set by the capture adapters; the capture classes are Bill's
  (`CaptureInput.*`, decklink/magewell): add only the flag assignment (call-site sized).
- **Contracts:** delay total per rate unchanged where the flag is true (Magewell, DeckLink
  normalised); grain RMS unchanged on the real-clip set.
- **Steps:** code during any hold; build after; touched tests; a GPU re-run of the near-black
  probe and the ladder test (~30 min).
- **Acceptance:** ladder totals within the existing bounds (4/3/2 frames: 268/226/184 ms ±25,
  spread ≤ 7.2 ms today); near-black probe asserts ≤ 16 differing pixels at codes 0-3 on the still
  1080p case; frozen-b3base unchanged except the near-black probe pixels (listed).
- **Audit:** WP-19 (delta audit). **Upstreamable:** the arrival flag concept yes (generic), the rest
  VP-only. **Hours:** 3-4. **Model:** opus (skeleton lead).
- **Depends on:** WP-0 step 1. Must land **before** WP-0 step 4 if ready; otherwise apply on
  `integ/batch1` and re-record frozen-b3base for the listed pixels only.

### WP-2 Helper verifications (a-e, run in parallel within slots)

Common acceptance for each: replace every `WIP (unverified)` commit with verified commits (or add a
"verified at <sha>" commit when the content is unchanged); build of touched projects; the branch's
own test classes green from the TRX; any GPU test run once by the coordinator (one at a time);
report in `docs/agent-reports/<branch>.md`. Rebase onto `batch1-base` (git only) before verifying,
so verification is against the batch base.

| WP | Branch @ tip | What is unverified (sources) | GPU needed | Audit state | Hours | Model |
|---|---|---|---|---|---|---|
| 2a | feat/dv-l8 @ 5258c487 | WIP 7251a160/fc4630d2/59d9408b (DV deband skip on tunnel pictures, both debanders, visible); 412f632c/9dc8e709 (compare-capture folder + shortcut); the GPU failure `DolbyVisionTrimShaderMatchesItsReference` (check on the base first, queue 113/120); uncommitted `DvDolbyStyle.h` | DV synthetic suite (~40 min) + the trim shader test + the 3.6-code rerun (queue 132) | audited (queue 120), fixes in c019f4df; needs a delta audit of the WIP parts | 2-3 | opus (DV helper) |
| 2b | feat/settings-preview @ 02e127f0 (+ uncommitted) | audit fixes 01402b51, WIP f72d0e63/02e127f0; the 1 known failure (250 ms legacy filter timing); preview rebuild; screenshot script `--screens` | none (Config tests off screen) | logic audit done (queue 127), fixes coded; **usability audit (queue 95) after the screenshots**, then the owner click-through (owner decision point, not blocking the merge) | 3-4 | opus (settings agent); sonnet may run the screenshot pass |
| 2c | chore/early-migrations @ 5d5390bd | everything since c1832ec8 rebuild (queue 165): per-commit builds (`verify2.sh` in the scratchpad), full CPU suite at the midpoint and tip, bisect on failure; part-3 fixes (LED readback non-blocking 1dfcaf21, peak skip 120fd533); `FallbackCatalog` tests | the part-3 GPU tests (LED readback) ~20 min | module findings are self-audit; **independent read-only audit of the whole branch still owed (queue 115)** | 3-4 (+1 audit) | opus (migration agent) |
| 2d | fix/vpscalerender-motion @ 2f8fb24c | audit fixes 0d2a6de8/2f8fb24c built? (queue 136: unbuilt at the time); the kit add-on's stale `tools/vpmotion` binaries (rebuild and re-stage as a pair at `D:\codex\trainkit-motion`); add-on audit fixes (queue 138) | none (CPU); the add-on GPU crash test runs on the 3090 with round 3, not here | audited (queue 129, 138); fixes need a delta check | 1.5-2 | opus (motion agent) |
| 2e | feat/net-runner @ 5805e928 | WIP 055c31ae (PlNetRunner + fused compute) built/run; Winograd chunk 2 (spilled: 99 ms vs 43 ms direct on the 3050 Ti → decide: keep Winograd off on this backend unless a fix lands); GPU vs CPU reference; fused vs exported timing (pl_timer); fp16 storage only if it saves on top of fusion (queue 112) | yes: runner GPU tests (~30-45 min) | audited (queue 131), fixes 5213f7a3/8bca38e9; **PlNetRunner + fused need an audit after verification (queue 117)** | 4-5 | opus (net-runner agent) |

- **Bill-owned check:** 2c's guard must pass (no Bill file moved; the 3 wrongly moved files stay
  dropped). 2a's renderer hook lines and 2d's metadata-only sidecar fields are call-site sized.
- **Upstreamable:** 2c's Bill-file fixes are "notes for Bill" (subtitle temporal-fill margins, dead
  strict Full-G22 code): record them, do not apply to his files. Everything else VP-only.
- **Depends on:** WP-0 (rebase target). 2a-2e run in parallel, max 2 building at once.

### WP-3 Merge the helper branches into `integ/batch1`

- **Goal:** one integration line containing all five helper branches, with numbers and generated
  files assigned once.
- **Inputs:** queue 126 (DV ↔ settings merge notes), 145 (API numbers at integration), 171 (order),
  forecast §3 (helpers add almost no conflicts with each other); vp-chain-gate (two-stage
  pre-merge interaction analysis).
- **Order (fixed) and hot spots:**

| # | Merge | Expected conflicts / hot spots | Resolution rule |
|---|---|---|---|
| 1 | feat/dv-l8 | renderer hook lines (DV trims, deband skip, metadata hold) near the deband/tone-hook code; `RendererProfileConfig.h` vpvKeys; `slots.json` (dv_tone_mapping, dv_compare_capture_folder); plugin API 43 claim; GUI resource 33209; `LibplaceboRendererPluginApi.h` / proxy / exports | keep both; API numbers per the table below |
| 2 | feat/settings-preview | `ConfigEditorWindow.cpp` (13 hunks vs base were forecast for Bill; vs dv-l8 small), `VpvOrganisation.h`, `VpvSection.cpp` (also changed by the B3 flip), `vpv-tiers.json`, `ConfigEditorWindowTests.cpp` | apply queue 126: `dv_compare_capture_folder` → Input > Advanced > "Readouts (diagnostics)" after `intent_display`; `dv_tone_mapping` → Colour unit, "Dolby Vision" card with `dv_trims`. Original pathway clears the folder. B3 switch hidden (queue 169) must stay hidden |
| 3 | chore/early-migrations (+ fix commits) | forwarding headers (11) vs any include added by 1-2; `components-master.json`; test vcxproj; `FallbackCatalog.h` new | includes added by dv-l8/settings to migrated paths switch to `<VP-V/...>`; manifests regenerated, never hand-merged |
| 4 | fix/vpscalerender-motion | the one renderer edit (metadata-only sidecar fields) near the capture writer; `RealClipLibrary.h` / harness tests if touched | keep both |
| 5 | feat/net-runner | `src/VP-V/nets/**` (new); test vcxproj; tools | keep both; wiring is NOT applied here (WP-10) |

- **Plugin API numbering at integration (assigned here, recorded in the merge commit body):**
  - base `VP_LIBPLACEBO_PLUGIN_API_VERSION = 42` (`LibplaceboRendererPluginApi.h:54`);
  - **43** = `ToggleDvCompareCapture` (dv-l8, merged first; update `a55308e7`'s version test);
  - **44** = reserved for WP-11 `GetModuleStatus` (generic status entry). After 44, new VP status
    needs no ABI bump;
  - Bill's `RequestCalibrationLutReload` (his API 29 line) takes the **next free number at the Bill
    merge** (45 if nothing else intervenes; forecast §2.4);
  - RPC op numbers: check for collisions at each merge (the forecast found op 9 = ProjectorTest vs
    Bill's GetDisplayModes; renumber OURS at the Bill merge, not now).
- **Regenerated files (after each merge, never hand-merged):**
  `python tools/check_module_manifests.py --write` (components-master.json), slots/manifest key
  lists via the manifest check, the settings reference generated from `slots.json` if the settings
  branch provides a generator; then `python tools/check_module_manifests.py` must pass.
- **Steps:**
  1. Pre-merge interaction analysis (read-only, parallel sub-agents per branch pair:
     `git merge-tree` conflicts, semantic interactions, new keys vs Config controls, plugin API),
     then the integrator sweeps the whole result alone (vp-chain-gate two-stage rule).
  2. Merge 1-5 in order with `--no-ff`, one merge commit each; build of touched projects after
     each; regenerate files.
  3. Merge checkpoint: full x64 Release solution build (both renderers), full CPU suite
     single-threaded (TRX), Config suite off screen, manifest check.
  4. Short GPU smoke (≤ 30 min, one run): frozen-b3base and frozen r6 byte-identity subset, DV
     synthetic short set. Purpose: bisectability; it is NOT the big test.
- **Acceptance:** all of step 3 green (known: the 2 calibration cases); smoke byte-identical
  (helpers are zero-cost when off; DV deband skip changes DV tunnel pictures only, listed).
- **Audit:** the pre-merge interaction analysis (step 1) is this WP's audit.
- **Upstreamable:** no. **Hours:** 5-7 (incl. ~2 h builds/suites, ~0.5 h GPU). **Model:** opus
  (integrator); sonnet for manifest regeneration and test-registration conflicts.
- **Depends on:** WP-0, WP-2a-e (each branch merges only when its WP-2 row is DONE).

### WP-4 Silent-fallback call sites

- **Goal:** every log-only downgrade found in the module findings appears under "Fallbacks engaged".
- **Inputs:** `docs/design/silent-fallbacks-callsites.md` + `.patch` (on `chore/early-migrations`,
  merged in WP-3); `module-findings-2026-10-24.md` parts 1-3; queue 137, 149, 154.
- **Files:** host: 24 hunks, each one `SyncFallback(...)` line (`git apply -3`); `CustomHookChain`
  (`QuantizerProblem()`); `ScopedDisplayRefreshRate::Switch` gets a `SetFallbackSink` (Bill's
  refresh code, cluster 3: **sink registration only**, one line per exit; no logic change). VP:
  `src/VP-V/core/FallbackCatalog.h` (already tested).
- **Contracts:** one heading "Fallbacks engaged: (list)" / "none" in Ctrl+I, Config status and
  Ctrl+Alt+D (vp-no-silent-fallbacks format). Deband rule: libplacebo's deband substitutes only when
  VP's debander has **failed**, never when the slot is deliberately off, **never on DV tunnel
  pictures**; pass dv-l8's own tunnel-picture flag (not `repr.sys` alone) so the two rules cannot
  disagree.
- **Steps:** apply; wire the "ready but not wired" builders whose anchor is VP code
  (`OutputScaling`, `GeometrySafeFit` status line, `SubtitleDetection` counters are VP-readable);
  builders whose anchor is inside a Bill cluster (`CalibrationLutDropped` in
  `RejectDisplayLutAfterRenderFailure`, `OutputRangeComposed` in `EnsureAutoLimitedFallback`,
  `OutputRequestRejected`) get a **single call line** only, and are listed as upstreamable status
  improvements for Bill; LED `LED drive`/`projector` entries per findings item 1.
- **Acceptance:** `FallbackCatalogTests` + renderer CPU tests; byte identity with default settings
  (frozen-b3base) — picture changes only while a fallback is in force (re-base factors with slot
  off, failed-debander substitution); big test: each fallback forced once via its test hook shows in
  all three surfaces.
- **Audit:** WP-19 (correctness: every listed site; efficiency: no per-frame string work, engage/
  release once). **Upstreamable:** the refresh-rate fallback sink and the status lines in Bill's
  clusters (generic "clear status instead of silent failure", memory rule) — yes, as separate
  commits; the catalog itself VP-only.
- **Hours:** 2-3. **Model:** opus (integrator) or sonnet for the mechanical `-3` application with an
  opus review. **Depends on:** WP-3.

### WP-5 Byte-identical efficiency fixes + GPU timers

- **Goal:** land the efficiency fixes that do not change a single output code, plus the timer plan.
- **Inputs:** `efficiency-audit-2026-10-24.md` §1 (#3, #7, #8, #9, #10), §4 (timers), §5 (order);
  branch `perf/byte-identical` (4f7820d2 ExtendedRange one pass, af7a8985 srcres fit without the
  read lock; uncommitted: early upload policy + VRAM guard, FeatureTextureIdle, PassTimers,
  PassTimerHook, ChangeKey, sigma-delta LQPREV/MVPREV sizing; patches
  `efficiency-callsites.patch`, `efficiency-upstreamable-churn.patch` and its
  `.bill-v1.4.02.patch` variant); queue 159, 161, 165.
- **Files:** VP: `EarlyUploadPolicy.h`, `FeatureTextureIdle.h`, `PassTimerHook.h`,
  `src/VP-V/core/PassTimers.h`, `ExtendedRange.*`, `SourceResolutionMonitor.cpp`,
  `shaders/Temporal sigma-delta.glsl` (VP shader), `CustomHookChain.*`. Host: call sites from
  `efficiency-callsites.patch`. `ChangeKey.h` (render-thread string churn) touches VP log
  signatures in the host; the churn inside **Bill's** clusters is the separate upstreamable patch
  (offer to Bill, do not apply to his clusters in this batch beyond what the owner approves).
- **Excluded (deferred, owner 161):** captured-repeat render reuse (#4, Bill's render loop), the
  shared luma product (Bill's analysis), fbo_float32 per-tier (picture change, owner A/B), polar vs
  separable AdaptiveDownscale (#6, owner A/B), OutputEncodingRepair fold (#5, goes with WP-6 commit
  C).
- **#9 B3 ring hand-over** moves into **WP-8** (the ring becomes S1's); do not implement it twice.
- **Contracts:** byte-identical output; early upload in all modes keeps a visible fallback when the
  VRAM guard refuses ("Fallbacks engaged: early upload"); timers poll, never drain or wait.
- **Steps:** finish and commit the uncommitted work (one commit per finding); rebase onto
  `integ/batch1`; integrator applies the call-site patch; checkpoint.
- **Acceptance:** CPU (EfficiencyPolicyTests, ChangeKeyTests, PassTimersTests, CustomHookChainTests);
  GPU in the big test: frozen-b3base byte identity (all), BTB/WTW probes unchanged (#7 is on the
  ER path), VRAM before/after on the laptop (record, expect −~125 MB sigma-delta, ER freed when
  off); timers: overhead ≤ 3 % of the measured stage time, 0 GPU waits.
- **Audit:** WP-19 (efficiency audit re-run on the changed sites; correctness: early upload thread
  safety and the guard). **Upstreamable:** ExtendedRange one pass is VP (ER is ours); the churn
  patch in Bill's clusters and a generic per-pass timer accumulator are upstreamable.
- **Hours:** 5-7. **Model:** opus (efficiency agent). **Depends on:** WP-3 (rebase). Parallel with
  WP-6/WP-7.

### WP-6 Output segments S2/S3 (with the calibration and dither glue that lives in them)

- **Goal:** the PRE_OUTPUT colour/drive chain and the OUTPUT finish chain each become one VP
  segment with one host call (commit B, byte-identical), then optionally one libplacebo hook each
  (commit C, behind a switch).
- **Inputs:** `segments-callsites.md` (feat/segment-runner, uncommitted drafts); central-migration-
  needs.md §6.1 (S2/S3), §6.3; efficiency audit #1/#5; queue 155, 163.
- **Files:** upstreamable: `vprenderer/HookSegment.{h,cpp}`, `HookSegmentPool.h`,
  `HookSegmentTests.cpp` (commit A). VP: `src/VP-V/core/SegmentRunner.*`,
  `src/VP-V/modules/output/segments/OutputSegments*`, tests, GPU fixture (commit B),
  `OutputSegmentsMerge` (commit C). Host: `segments-callsites.patch`, then
  `segments-merge-callsites.patch`. Note `NlsHookRecovery::RenderOwned` is Bill's (the merged form's
  `Scope` wraps his draw callback from outside — a call line, no change inside his function).
- **Contracts:** S2 = SDR tone shape → DV trims → [Stage 13 placeholder] → LED measure → LED
  compensation → [black level placeholder] → | user shaders (edge) | → calibration LUT (last of
  S2: acts on target light before the output encoding). S3 = encoding repair → occlusion mask →
  sigma-delta (always last). Range incl. BTB/WTW passthrough through LED compensation.
- **Steps:** (1) the segments agent commits A, B, C as drafted; (2) rebase on `integ/batch1`
  (WP-3 + WP-4 merged: DV trims and FallbackCatalog changed nearby); (3) integrator applies
  `segments-callsites.patch` (commit B), checkpoint, then commit C with `output_segments_merged`
  default **off**; (4) Config control for `output_segments_merged` (settings rule: every setting has
  one; Output > Advanced), settings reference entry; (5) follow-ups: `Scope` for
  `PresentDejudderExtras` (after the main path passes), `Merger` → `SlotTiming`, a disabled stage
  re-enabled on a settings change.
- **Acceptance:** CPU: `SegmentRunnerTests.S2SeparateFormIsTodaysHookListTagsAndParameters` (128
  combinations), `S3SeparateStepsAreTodaysLoops`. Big test: commit B frozen-b3base byte-identical;
  commit C with the switch ON: **≤ 1 code at output bit depth vs the separate form**, BTB code 4 and
  WTW code 1019 probes unchanged, sigma-delta statistics unchanged, whole-chain matrix passes; record
  `SlotTiming` before/after (no assumed saving).
- **Owner decision:** default ON for the merged form only after the tolerance gate passes and he
  accepts (section 6, D1).
- **Audit:** WP-19 correctness (hook order, extras, failure → separate form visibly) + efficiency
  (pass count before/after). **Upstreamable:** commit A only (a generic "fuse a run of hooks"
  runner). **Hours:** 6-8. **Model:** opus (segments agent; integrator applies). **Depends on:**
  WP-3, WP-4.

### WP-7 Motion/timing extraction + queue/timing policy interface

- **Goal:** motion segment S0 (consumers fetch vectors directly; one vectors-valid state), queue
  decisions behind `IQueueTimingPolicy` with the VP fixed-budget policy and Bill's policy as an
  either/or, one shared look-ahead walk; `AlphaQueuePolicy.h` (Bill's) restored to 83b7ffea.
- **Inputs:** `motion-timing-callsites.md` (feat/motion-timing-extract, commits c33aaa41..c3e45812,
  uncommitted host/test edits = the patch being finalised); queue 164; forecast §2.1; vp-either-or-
  overlaps; vp-low-latency-vectors; vp-vector-validity.
- **Files:** upstreamable: `vprenderer/QueueTimingPolicy.h`, `QueueLookaheadWalk.h`. VP:
  `src/VP-V/modules/input/queue_timing/*`, `src/VP-V/modules/motion/vector_delivery/MotionSegment.h`,
  `SampledPassTimers.h`, tests. Host + Bill's `AlphaQueuePolicy.h` via
  `motion-timing-callsites.patch` (P-a), then P-b1/P-b2/P-c as separate commits. Restoring
  `AlphaQueuePolicy.h` to Bill's text is **removing our edits from his file**, which the rules
  favour.
- **Contracts:** `PlanHold`, `QueueTarget`, `TicksUntilDue`, `EvaluateBacklog` (motion-timing-
  callsites.md §2); `queue_policy = vp | upstream`, Original pathway → upstream (P-c); both names in
  status. S0: all 9 discontinuity causes invalidate every consumer; low-latency backward current +
  last field.
- **Steps:** (1) agent finishes the patch, rebases on `integ/batch1`; (2) P-a queue part
  (byte-identical decisions) + commit 6 DejudderWarp `PollPassTimers()`; checkpoint; (3) P-b1 motion
  call sites (byte-identical delivery); (4) P-b2 arm sampled timers (1 in 30, poll only); (5) P-c
  Original → `BillQueuePolicy` + `queue_policy` key with a Config control (Input > Total delay
  card, Advanced), settings reference; (6) a **Bill-merge dry run** (`git merge-tree` of the patched
  tree vs Bill's current beta) to confirm the queue hunks shrink as predicted (central §5).
- **Acceptance:** CPU: `QueueTimingPolicy` 11 tests (frozen-host equivalence 20k/200k cases, ladder,
  cap, shed, age gate, either/or, one walk vs his preparation on 50k queues), `MotionSegmentTests` 9.
  Big test: **strict delay gate 4/3/2 frames × 23.976/24/25/29.97/30/50/59.94/60** unchanged (same
  totals ± the gate tolerance, spread unchanged), render-ahead cases, frozen-b3base byte identity,
  capture-harness extras; GPU G1-G2 (`MotionTimingGpuTests.inc`): timer overhead ≤ 3 % of an
  estimate; P-c: one Original-pathway regression run (boundary-only rule).
- **Audit:** WP-19 correctness (decision equivalence, lock/atomics, the `||` short-circuit) +
  efficiency (one walk instead of three; undrained timers). **Upstreamable:** the interface + walk
  headers + the host hunks that do not mention VP (offer only with owner approval).
- **Hours:** 8-10 (+ timing gate share in WP-20). **Model:** opus (motion-timing agent; integrator
  applies). **Depends on:** WP-3. Parallel with WP-5/WP-6 (different host regions; integrator
  serialises the patch applications).

### WP-23 VP frame analysis store (FAS) — early; other WPs consume it

(Numbered last because it was added last; it is scheduled EARLY, right after WP-3.)

- **Goal (owner addition 2026-10-24):** VP's own per-frame analysis is computed **once per source
  frame inside our umbrella**, on VP-owned data, kept in a ring keyed by source-frame sequence and
  READ by every VP stage, instead of each stage re-decoding the raw frame. The efficiency audit found
  the same raw frame's luma decoded 5-7 times on CPU threads (`efficiency-audit-2026-10-24.md` §3).
- **Bill's analysis stays untouched:** scene detection (`SceneDetector`), active picture/crop
  (`ActivePictureEvidence`, `AnalyzeActivePictureLookahead`), subtitle detection remain his code and
  keep their own decodes. Where both compute the same quantity, **ours serves our stages only**
  (either/or rule; status shows which source a VP consumer uses). A shared product for his analysis
  is a possible later upstream offer (D7), not part of this batch.
- **Inputs:** efficiency audit §3 (duplicated luma), §4 (timers); vp-vector-validity (one validity
  model, invalidated at discontinuities); vp-low-latency-vectors; central-migration-needs.md §6.1
  (S0/S1); WP-7's `MotionSegment.h` (vectors-valid state); module findings part 3 (LED readback made
  non-blocking, 1dfcaf21 — the same pattern for FAS readbacks); vp-led-relay-passthrough (LED
  peaks picture-only, full coverage).
- **Files (all new, VP-only):** `src/VP-V/core/FrameAnalysisStore.h` (record, ring, validity,
  reader API; header-only C++14 like the other core types), `src/VP-V/modules/input/source_analysis/
  VpFrameAnalysis*.{h,cpp}` (producer: GPU reduction shaders + CPU fallback), tests in
  `src/VP-V/tests/`. Host: one producer call where the frame is uploaded/converted, one store
  member in `Impl`; consumers read through the store, not through the host.
- **Record fields (per source frame, `seq`):**

| Group | Fields | Producer | Notes |
|---|---|---|---|
| Identity | `seq`, capture time (frame start), source size/format/range/transfer/primaries, active-picture rect used (copied from Bill's decision, read-only), geometry generation | host facts | the record says which crop it measured |
| Luma statistics | histogram (e.g. 256 or 1024 bins, code domain incl. BTB/WTW), mean/APL, percentiles (min/1/50/99/max), peak and black extents, white extent | GPU reduction | full coverage of the active picture (LED rule: picture-only, full coverage) |
| Range occupancy | BTB share (< black code), WTW share (> white code), near-black share, clipped-highlight share | GPU reduction | feeds BTB/WTW checks and LED/black-level decisions |
| Grain/noise | noise/grain RMS estimate per band (flat-block method), confidence | GPU reduction | deband, texture restore, dither, grain gates; never used to remove grain |
| Chroma summary | mean/peak saturation (optional, cheap) | GPU reduction | DV trims reference, CAM16 |
| Motion summary | vectors-valid (from `MotionSegment::Validity()`), global motion, motion magnitude percentiles, vector source (engine ahead / at render / matcher) | S0 (WP-7) | written when the estimate for (seq-1 → seq) or (seq → seq+1) lands; separate validity bit |
| Cadence/repeat | repeat-of-previous flag, cadence phase, film-clock position | existing pulldown/cadence code (VP) | captured repeats get a record pointing at the original (`repeatOf`) — no re-measure (also fixes the LED re-measure on repeats, efficiency §2) |
| Scene | VP scene-cut / fade flag (VP's own detector where one exists; else copied from Bill's decision, labelled as such) | VP / host fact | consumers that need a cut use one value |
| Labels | encoding/ID-net labels v1 when available (codec class, bpp tier, bit depth, delivered ratio, player kernel) | Stage 12 identification / later ID net | net upstream context (WP-10 `CurrentLabelsV1()` reads here) |
| Validity | per-group valid bits, `epoch` (discontinuity counter), `complete` | store | see rules below |

- **Validity rules:** one discontinuity epoch shared with vector validity: source change, signal
  format/size/range change, crop/geometry generation change, seek/reset, dropped or duplicated frame
  in the sequence, queue flush/backlog shed, DV metadata loss, settings change that alters what is
  measured. A record from an older epoch is never served for temporal use (look-ahead windows,
  temporal stats); it may be served for single-frame use only if its own group bits are valid. A
  missing or late record → the consumer uses its existing fallback **visibly** ("Fallbacks engaged:
  frame analysis (<consumer>: <why>)"), never a silent stale value. Records for frames dropped from
  the queue are released with the ring slot.
- **Producer placement:**
  - Runs **on arrival** (when the frame is uploaded), not at render, so look-ahead consumers (LED,
    intent) see queued future frames. With early upload in all modes (WP-5 #3) the GPU reductions run
    on the early-upload path right after the upload/unpack, as compute passes on the VP-owned
    planes; results are read back **asynchronously** through a small readback ring (polled 1-3
    frames later, never `Map` with wait on the render thread; same pattern as 1dfcaf21).
  - When early upload is refused (VRAM guard) the render thread does the reductions at upload time,
    still with an async readback, and the "early upload" fallback already shows.
  - CPU fallback only where no GPU path exists (no D3D11 device / tests): one decode to a reduced
    16-bit luma plane, shared by all VP readers.
  - Cost target: ≤ 0.2 ms GPU per source frame on the 3090 at 4K source (reductions are bandwidth-
    light: one read of the luma plane), measured with the WP-5 timers; CPU work on the render thread
    ~0 (no per-pixel CPU loops).
  - Ring depth = queue depth + look-ahead depth + 2 (derived from the delay budget, never a fixed
    frame count: vp-general-not-fixed).
- **Consumer migration list (each migration is a commit in the WP that owns the consumer):**

| Consumer | Today | After | Migrates in |
|---|---|---|---|
| LED / Dynamic Drive look-ahead (`LedLookahead`, full-coverage rows) | own CPU decode of queued frames | FAS luma stats/peaks over the look-ahead window (one walk, WP-7) | WP-15 |
| LED measurement on captured repeats | re-measures an identical picture | `repeatOf` → cached result | WP-15 |
| Intent / content intent (`BuildGrid` via per-pixel `Sample` lambdas) | own CPU decode | FAS stats/histogram (grid from the reduced plane if a spatial grid is needed) | WP-16 |
| HDR inference (`HdrInference`, VP side of input.signal) | own decode | FAS percentiles/peaks | WP-14 |
| DV trims reference conditions / CAM16 display adaptation | none or own | FAS APL/peak/chroma summary | WP-14 (CAM16 itself later, Stage 13) |
| Deband / texture restore / detail pulse gates | own statistics inside the stages | FAS grain/noise + near-black occupancy (in addition to their per-pixel work) | WP-8 / WP-9 (S1 reads FAS) |
| Temporal dither (sigma-delta) decisions | own statistics | FAS range occupancy + motion summary | WP-13 |
| Nets (labels, upstream context) | `CurrentLabelsV1()` TODO | FAS labels | WP-10 |
| NVOF producer half-size luma | CPU downsample | unchanged in this batch (separate device/thread, acceptable per the audit); revisit later | — |
| Diagnostics (Ctrl+I, Ctrl+Alt+D, crash state) | scattered getters | one FAS summary section | WP-11 / WP-17 |
| Bill's SceneDetector / ActivePicture / subtitles | own decodes | **unchanged** | — |

- **Steps:** (1) interface commit: `FrameAnalysisStore.h` (record, ring, epoch/validity, reader
  API, "Fallbacks engaged" entry) + CPU tests — lands **early in P2/P3** so the consumer WPs code
  against it; (2) producer: GPU reductions + async readback + CPU fallback, behind
  `frame_analysis_store` (default on once the big test passes; Config control in Input > Advanced
  "Readouts (diagnostics)"; settings reference); (3) host call site (producer on arrival, epoch bumps
  at the discontinuity points shared with `MotionSegment`); (4) consumers migrate in their WPs
  (table), each behind the same switch until WP-20, then the old per-consumer decodes are deleted
  in a follow-up commit (two-commit rule).
- **Contracts:** values equal (within stated tolerance) to each consumer's current computation on the
  same frame — consumers must make **the same decisions**; byte-identical output where a consumer's
  decision is unchanged; never blocks the render thread; never serves a stale-epoch record to a
  temporal consumer.
- **Acceptance:** CPU: ring/epoch/validity tests (all discontinuity causes invalidate; late record →
  visible fallback; repeat records point at the original; ring depth derived from the budget);
  reduction maths vs a CPU reference on synthetic frames incl. BTB/WTW codes (histogram exact; means
  within 1e-6 relative); per-consumer equivalence tests (LED look-ahead decisions identical on the
  LED correlation harness clips; intent decisions identical on the intent test set). GPU (big test):
  reductions == CPU reference on captured frames; render-thread time unchanged or lower; GPU cost per
  source frame recorded (target ≤ 0.2 ms 3090-projected); frozen-b3base byte identity with all
  consumers migrated (or the listed, accepted differences); timing gate (producer on the arrival
  path).
- **Audit:** WP-19 (correctness: epochs, thread safety of the ring between the arrival thread and
  the render thread, look-ahead window alignment; efficiency: decodes removed, readback never
  blocking). **Upstreamable:** the store is VP-only; the "one shared luma product" idea for Bill's
  analysis is a later offer (D7).
- **Out of scope:** persisting per-title analysis to disk (not useful for live capture); revisit
  only if a use appears (e.g. file playback pre-analysis).
- **Hours:** interface 2-3; producer + host call 4-6; consumer migrations ~1 h each inside their WPs
  (~6-8 in total, counted there); tests 2. **WP-23 itself: 8-11.** **Model:** opus (design +
  producer); sonnet may write the CPU reference/equivalence test scaffolding.
- **Depends on:** WP-3 (base), WP-5's early-upload policy (producer placement), WP-7's
  `MotionSegment` (motion summary + shared epoch). Interface commit has no dependency beyond WP-3.
  **Consumed by:** WP-8/9, WP-10, WP-13, WP-14, WP-15, WP-16, WP-17. Not on the critical path if the
  interface commit lands before WP-8 starts.

### WP-8 S1 entrance segment + small B3 closures (3, 5, 6, 7, 8, 13) + ring hand-over

- **Goal:** one call `entrance.Run(sourceFrame, role, factors, field) -> processed picture`; the B3
  ring, cache, stage order and per-path rules inside the segment; the small fallback cases closed.
- **Inputs:** `b3-fallback-closure.md` (cases 3, 5, 6, 7, 8, 13; "Order inside the batch" step 1);
  central-migration-needs.md §6.1 S1, §4 b8, §6.4 s3; efficiency audit #9 (ring hand-over);
  ADR-0024.
- **Files:** VP: new `src/VP-V/modules/repair/entrance/EntranceSegment.{h,cpp}` (or under
  `src/VP-V/core` if the pipeline README prefers), stage adapters for re-base / deband / detail /
  texture restore (VP files `SourceRebase.*`, `VpDebander.*`, `DetailPulse.*`, `TextureRestore.*`
  stay where they are; M2 moves them later). Host: cluster 5 and 21d (V) shrink to one call per path
  (main, extras, crossfade).
- **Contracts:** roles `A` and `BAhead`; ring textures in a fixed renderable format chosen at ring
  creation (case 3) with a compute-copy fallback reported as a real error; identity entries return the
  raw frame (5); no next frame / next planes missing / interpolator failure → processed A held,
  visible (6-8); stateful stages advance once per source frame (A), B's decisions from the engine's
  computed-ahead field until WP-9 item 9 changes it.
- **Steps:** commit 1 behaviour-free extraction (byte-identical to frozen-b3base); commit 2 the
  small closures + ring hand-over (#9: stages render into ring-provided targets); timers per stage
  (efficiency §4.2, pl_dispatch timers polled).
- **Acceptance:** CPU: entrance segment order/contract tests, B3 harness CPU parts; GPU (big test):
  commit 1 byte-identical; commit 2 byte-identical except the closed cases' pictures (listed per
  case); B3 harness + real-clip (0 fallbacks on the default path, history breaks 0, ~2.4x cheaper
  kept); timing gate (render thread change).
- **Audit:** WP-19 (correctness: ring lifetime/ownership, stateful continuity; efficiency: copies
  removed). **Upstreamable:** no. **Hours:** 6-8. **Model:** opus (skeleton lead).
- **Depends on:** WP-3 (and WP-5's rebase to avoid two edits of the ring). **Critical path.**

### WP-9 B3 fallback closure (rest) and pre-B3 deletion

- **Goal:** no B3 case falls back to the pre-B3 path; delete that path and the diagnostic key.
- **Inputs:** `b3-fallback-closure.md` cases 9, 4, 10, 2, 1, 11-13 and the order section; queue
  169, 171; vp-low-latency-vectors (backward vectors without look-ahead).
- **Order and items:**
  1. **9a backward-field rule** (6-10 h, M-H): stateful stages (debander hold, detail pulse /
     texture restore history) take the frame's backward field (S-1 → S) so B can always be processed
     as its own A without C. Real-clip grain (RMS, temporal independence) and flicker checks on the
     GPU. Shared with net A_motion's neighbours (WP-10).
  2. **4 N planes** (4-6 h): `WarpProcessed` and `FrameInterpolator::Run` for 1-, 2-, 3-plane
     frames (RGB capture, native RGB 4:4:4); DV tunnel: B3 runs on the tunnel planes as today (the
     post-decode question is separate: `dv-post-decode-deband.md`, not this batch).
  3. **10 fill from processed B** (2-3 h).
  4. **2 compose after the motion picture** (3-5 h): subtitle move applied to the warped/
     interpolated result. Subtitle composition code is Bill's: VP calls his composition on the VP
     picture; no change inside his functions.
  5. **1 crossfade of two processed paths** (4-6 h): ring keyed by re-base factors; `CrossfadeMix`
     mixes the two finished pictures.
  6. **Delete** the `EntrancePath::Extra`/`CrossfadeFrom` per-picture branches,
     `RunVpPhaseInterpolation` + the Main phase path, `aNeeded`, and `pipeline_entrance_once` (keep
     the Original pathway untouched); remove the live r6 DLL comparison only after the frozen r6 test
     has passed on the same commit (WP-0).
- **Files:** VP + the host clusters 5/7/20 (V). Bill's subtitle composition (21c) and the Original
  pathway: untouched.
- **Acceptance:** CPU: case tests per item; "history breaks" counter 0; GPU (big test): B3 harness
  all cases with **0 "entrance once" fallbacks**, real-clip grain RMS within ±2 % of the pre-change
  value and no added flicker (temporal independence metric unchanged), frozen r6 test for B3-off-
  equivalent inputs where defined, frozen-b3base for unaffected cases, DV suite, low-latency 50/60p
  session cases (C absent) with 0 fallbacks.
- **Audit:** WP-19, **mandatory independent audit of 9a** (vector semantics change) before the big
  test. **Upstreamable:** no. **Hours:** 20-30 (b3-fallback-closure.md total 23-35 incl. WP-8's
  small cases). **Model:** opus (skeleton lead); a second opus agent may take items 4 and 10 in
  parallel in a separate worktree (patch handed to the lead).
- **Depends on:** WP-8. **Critical path** (9a → 2/1 → deletion).

### WP-10 Net runner wiring (A in S1, D in S4)

- **Goal:** nets run in their agreed slots with the fingerprint/upstream rules and visible bypasses;
  zero cost when off.
- **Inputs:** `net-runner-wiring.md` + `.patch` (feat/net-runner 0344295d, `SetOutputScale`
  bff781df), `net-export-fingerprint.md`, queue 102, 107, 131, 139, 152; VP-barriers B1/R3/R8;
  central §6.4 s5 (S4 scaling-refine segment).
- **Files:** VP: `src/VP-V/nets/**` (merged in WP-3), a new `ScalingRefineSegment` (S4) wrapping
  AdaptiveDownscale's POST_KERNEL blend + D. `AdaptiveDownscale.*` is VP. Host: `UpdateNets` call,
  `RunEntrance` case → now **a stage of S1** (`case RepairEncoding`), config keys
  (`nets_enabled`, `nets_export_dir`, `nets_stamp_dir`, `nets_repair`, `nets_d`) in the VP settings
  path and `RendererProfileConfig.h`'s `vpvKeys` (one line each), Ctrl+I "Nets:" line through the
  status builder (WP-11 if landed, else the existing getter).
- **Contracts:** upstream key = actually-running upstream nets (`stem@shader-sha16|bypassed`);
  **D bypassed visibly when A runs, when VP deband sits in front of it, or when the hybrid uses
  vectors** (until a D trained on A's output / D_motion exists); A runs only in the A-first order
  (else visibly bypassed); rgba32f intermediates in each net's fingerprint; RGB capture: A on the
  matcher luma through the range helpers, delta to RGB; D light domain must match its training
  (R8) before any default-on. Shaders/weights never in the repo or a deployment.
- **Steps:** rebase the patch onto S1 (WP-8) — the `RunEntrance` case becomes an S1 stage; fill the
  TODOs (`ScalerBlendJson`, `latchFixes`/grain constants from real fields, stage-contract text,
  `CurrentLabelsV1`, `PlaneWithRepairedCrop`, A_motion neighbours from S1's ring + the backward field
  of WP-9a, D's light domain); S4 commit 1 behaviour-free (D off → byte-identical), commit 2 D in the
  segment; Config controls for the 5 keys (Repair/Scaling units, Advanced), settings reference.
- **Acceptance:** CPU: settings reads + invalid value → "Fallbacks engaged"; `NetSettingsHashesNow`
  reproduces the kit's recorded round-1 hashes; one changed setting changes exactly one hash. GPU
  (big test, `VP_TEST_GPU=hardware`, private export via `VP_NETS_EXPORT_DIR`): nets off →
  frozen-b3base byte-identical (zero cost); A runs (Ctrl+I line, SlotTiming samples) and differs
  only inside open tiles; D via the real AdaptiveDownscale hook == `RunInPostKernelHook` on the same
  textures (bit-exact or ≤ 1e-4 code fp32); A on → D bypassed with the "upstream nets" reason; deband
  on → D bypassed visibly; GPU vs CPU reference ≤ 1e-4 code (fp32).
- **Audit:** WP-19 (correctness: key/bypass logic, crop mapping, luma splice; efficiency: per-pass
  pl_timer costs, pool usage). **Upstreamable:** no (VP feature). The mpv-pass shim could be offered
  later; not in this batch. **Hours:** 8-12. **Model:** opus (net-runner agent drafts against S1;
  integrator applies).
- **Depends on:** WP-8 (S1), WP-9a (A_motion neighbours; until then A_motion degrades visibly to the
  single-frame fallback — acceptable for an intermediate commit), WP-2e.

### WP-11 Generic plugin status entry, status text builders, VP Ctrl+I text and hotkeys out of the dialog

- **Goal:** new VP status needs no ABI bump; the VP sections of Ctrl+I and VP hotkeys leave
  `VideoProcessorDlg.cpp` (Bill's file) as single calls.
- **Inputs:** central-migration-needs.md §4 (d) items 2-3, §7.3 (status text builders), b1; queue
  145, 172.
- **Files:** upstreamable: `GetModuleStatus(slotId, buffer)` in `LibplaceboRendererPluginApi.h`,
  `IRenderer.h`, proxy (`LibplaceboPluginVideoRenderer.cpp`), exports (`LibplaceboPluginExports.cpp`)
  — API **44**; key/value text format documented. VP: `src/VP-V/core/StatusText.*` (de-judder, LED,
  intent, fallbacks, nets, DV), `src/VP-V/gui/` (new: VP Ctrl+I sections, VP hotkey table and
  handlers). `VideoProcessorDlg.cpp`: VP code moved out, one call each. `StatsOverlayWindow.cpp`
  (Bill rewrote it upstream, forecast §1: 803 conflicted lines): **do not restructure it**; VP lines
  enter only via the status text it already draws.
- **Contracts:** Ctrl+I output text identical before/after (snapshot test); hotkey bindings and
  their Config-test documented keys unchanged (e.g. dv compare capture Ctrl+Alt+Shift+W).
- **Steps:** commit 1 generic entry (upstreamable, self-contained); commit 2 StatusText builders +
  CPU snapshot tests (copy of today's strings); commit 3 move the dialog's VP sections/hotkeys;
  status getters added since API 42 switch to `GetModuleStatus` where the GUI is ours.
- **Acceptance:** CPU snapshot tests (Ctrl+I text byte-equal for recorded states), Config suite
  (hotkeys), GUI build; big test: one on-screen Ctrl+I check (coordinator, off-screen preferred
  where possible).
- **Audit:** WP-19. **Upstreamable:** commit 1 (and the idea of commit 3 as a hook point).
  **Hours:** 7-9. **Model:** opus; sonnet for the snapshot-test scaffolding.
- **Depends on:** WP-3 (API 43 assigned). Runs **in parallel** in worktree `vp-status-gui` from
  WP-3's result: it touches the dialog/API files, not the renderer's render path.

### WP-12 Calibration LUT glue

- **Goal:** VP calibration glue out of the host: pattern present/readback (cluster 10, V part),
  calibration status getters (cluster 23), calibration-LUT stage selection (cluster 14 V part) as one
  VP adapter. The LUT application itself is already S2's last stage (WP-6).
- **Inputs:** central §4 b3; module findings (VP LUT status wrong, calibration patch wait 0.2-1.6 s,
  `CalibrationLutDropped`/`CalibrationPatchFullField` builders); M2 plan step 3 (the move itself is
  M2, not here); forecast §2.4 (Bill's explicit-Apply LUT reload, merges later as either/or; owner
  decision 4: our calibration may load its own LUT without Apply, with a notice).
- **Files:** VP: `src/VP-V/modules/output/calibration_lut/` (already migrated module) gains a host
  adapter. Bill's `LibplaceboDisplayLut.*` / display LUT load/attach (cluster 14, B): untouched;
  call lines only.
- **Acceptance:** CPU (calibration tests), byte identity (frozen-b3base), calibration harness case;
  the 2 known calibration failures stay known (no new). **Audit:** WP-19. **Upstreamable:** no.
  **Hours:** 2-3. **Model:** opus (or sonnet with opus review: mostly mechanical). **Depends on:**
  WP-6.

### WP-13 Dither/output glue outside S3

- **Goal:** the remaining VP output glue becomes stages or adapters: sigma-delta domain loop
  leftovers, ExtendedRange late restore/carry registration, AdaptiveDownscale measure/blend
  registration (now S4's entry), the de-judder rendered-picture cache registration.
- **Inputs:** central §4 b7, §6.1 (what is NOT in S2/S3: NLS (Bill's), ER, AD, de-judder cache,
  user shaders); `segments-callsites.md` "Not in a segment".
- **Files:** VP modules (`ExtendedRange.*`, `AdaptiveDownscale.*`, `CustomHookChain.*`,
  `SigmaDeltaDomain.h`). NLS / `NlsHookRecovery` are Bill's: untouched.
- **Acceptance:** byte-identical (frozen-b3base); BTB/WTW probes; whole-chain matrix in the big test.
  **Audit:** WP-19. **Upstreamable:** no. **Hours:** 3-4. **Model:** opus. **Depends on:** WP-6,
  WP-10 (S4).

### WP-14 DV glue

- **Goal:** DV metadata handling, trims/Dolby-style selection, metadata hold/corrupt-packet logic and
  the compare capture leave the host as an `input.signal`/`colour.sdr_tone_shape` adapter.
- **Inputs:** central §4 b6 (clusters 21a DV part, 21e DV); dv-l8 report (`docs/agent-reports/` on
  dv-l8) and queue 113, 120, 126; M2 steps 6 and 14 (moves are M2).
- **Files:** VP: `DvDolbyStyle.h`, `DolbyVisionTrims.h`, `SdrToneShape.h`, DV gate/hand-off headers,
  a new `DvHostStage`. Bill's `HDRData.cpp`, `ColorSpace.cpp`, `EOTF.cpp`, his HDR/tone setup in 21a:
  untouched. Capture formatters (`CR210DolbyVisionTunnelFormatter.cpp` is Bill's): untouched.
- **Acceptance:** DV CPU 110+ tests; DV synthetic suite 16/16 (big test); DV trim shader GPU parity
  (dv audit MED-3); frozen-b3base. **Audit:** WP-19. **Upstreamable:** no. **Hours:** 3-4.
  **Model:** opus (DV helper). **Depends on:** WP-3, WP-6 (DV trims are an S2 stage).

### WP-15 LED / Dynamic Drive glue

- **Goal:** `UpdateLedDrive`, suspend/resume, LED status, measurement scheduling and the
  compensation-unavailable wiring (handed off by the migration, queue 149) behind one Dynamic Drive
  host adapter; the measure/compensation pixel work is already S2 stages.
- **Inputs:** central §4 b5 (21e LED part); module findings item 1 (LED dims with no compensation →
  "held at full", now fixed in the module, wiring pending); M2 plan "LED module boundaries"; efficiency
  §2 (LED re-measures captured repeats → feed the cached result; module owner = migration agent).
- **Files:** VP: `src/VP-V/modules/drive/*` (migrated). Host: call lines.
- **Acceptance:** LED CPU tests (incl. `LedScriptLookaheadTests`), byte identity with LED off,
  "Fallbacks engaged: LED drive" when compensation is unavailable (forced test); no projector
  needed (dry run). **Audit:** WP-19. **Upstreamable:** no. **Hours:** 3-4. **Model:** opus.
  **Depends on:** WP-6, WP-7 (the shared look-ahead walk feeds LED statistics).

### WP-16 Intent/analysis glue (VP parts only)

- **Goal:** intent interpreter, black intent, ambient (cluster 19) and their application (21b) as a
  VP adapter. Analysis/crop/subtitle (clusters 17-18, 24) are Bill's and stay.
- **Inputs:** central §4 b4; efficiency §3 (shared luma product: **deferred**, Bill's analysis);
  vp-led-relay-passthrough (script intent).
- **Files:** VP: `IntentInterpreter*`, content intent module. Host: call lines.
- **Acceptance:** CPU (`IntentInterpreterTests`), byte identity intent on/off (frozen-b3base +
  one intent-on capture recorded at WP-0 if not present). **Audit:** WP-19. **Upstreamable:** no.
  **Hours:** 3-4. **Model:** opus. **Depends on:** WP-3.

### WP-17 VP diagnostics/captures adapter

- **Goal:** VP-only diagnostic captures and trace switches behind one adapter: render-ahead logs,
  de-judder hold diagnostics, entrance counters, DV compare capture trigger, B3 history-break counter
  readout, crash-state contributions of VP modules.
- **Inputs:** central §7.6; queue 90 (DV compare capture); `StateReport` (migrated diagnostics
  module).
- **Files:** VP: `src/VP-V/modules/control/diagnostics/` (+ adapter). **Stays Bill's:** subtitle
  capture (`VP_SUBTITLE_CAPTURE_PATH`), local-boundary diagnostics.
- **Acceptance:** CPU; Ctrl+Alt+D bundle contains the same VP sections (snapshot); captures stay
  local, never committed. **Audit:** WP-19. **Upstreamable:** no. **Hours:** 2-3. **Model:** opus
  or sonnet with review. **Depends on:** WP-11 (status text), WP-14.

### WP-18 VP settings readers (LAST, with control.config)

- **Goal:** VP keys read by per-module key tables (key, default, range, reader) in
  `src/VP-V/config/`, driven by `slots.json`; `ReadRendererSettings` calls them. Fixes key-list drift.
- **Inputs:** central §4 b2, §7.5; module findings (slots.json/manifest key lists wrong, item 4);
  settings-preview's `VpvOrganisation.h`; M2 step 19 (the schema move is M2, NOT here).
- **Files:** VP: `src/VP-V/config/*Keys*`. Host cluster 2 (M): `ReadPipelineSettings`,
  `DejudderMatcherSettings`, `ApplyOriginalPathwaySettings` (V parts) move out; Bill's keys and
  `ApplyDisplayRule` stay. `RendererProfileConfig.h`, `MainConfigSchema.h`: call lines/table hooks
  only.
- **Contracts:** **settings fingerprint identical** (`LibplaceboRendererSettingsFingerprintForTest`
  for the default config, the owner's deployed config copy, every tier preset, Original pathway);
  all keys added in this batch (`output_segments_merged`, `queue_policy`, `nets_*`, dv keys) present
  in the tables with Config controls.
- **Acceptance:** CPU (fingerprint tests over all presets), Config suite off screen (full), manifest
  check. **Audit:** WP-19. **Upstreamable:** no. **Hours:** 5-6. **Model:** opus; sonnet for table
  transcription with an opus diff review.
- **Depends on:** every WP that adds keys (WP-6, 7, 10, 11, 14). **Critical path** (last code WP).

### WP-19 Independent audits and fix rounds

- **Goal:** each WP gets an independent read-only audit (correctness + efficiency) before the big
  test; the integrator does one whole-result sweep (two-stage rule).
- **Inputs:** the code diff of each WP (**give auditors the code, not our conclusions**); the owner
  rules in memory; `docs/audit/README.md` checks 8-9 (chain matrix reporting).
- **Audit set:** WP-1 delta; WP-2c whole branch (owed); WP-2e PlNetRunner/fused; WP-4; WP-5; WP-6;
  WP-7; WP-23 (frame analysis store); WP-8; WP-9 (9a mandatory, separately); WP-10; WP-11; WP-12-17 (one combined glue audit);
  WP-18; then the **whole-batch sweep** by the integrator (cross-area interactions: S1 × nets ×
  B3 closure; S2/S3 × LED × DV × calibration; queue policy × render-ahead × B3 ladder).
- **Each audit reports:** HIGH/MED/LOW with file:line; an efficiency section (cost per frame, passes,
  allocations, VRAM); rule checks (host = call sites, Bill files untouched, either/or, visible
  fallbacks, BTB/WTW, grain, general rates); tests honest (negative controls, assertions).
- **Fix rounds:** HIGH and MED fixed before WP-20; LOWs fixed or listed in §3.8c known gaps.
- **Hours:** ~1-1.5 per audit × ~13 ≈ 15-18 (read-only, can run many in parallel, no machine load
  beyond reading) + fixes ~6-10. **Model:** opus for every auditor. **Depends on:** each WP's code
  DONE; runs continuously as WPs finish.

### WP-20 The ONE large test procedure

- **Goal:** one verified result for the whole batch. Quiet machine: **no builds and no other GPU
  work during the timing parts**; every agent's own build/test PIDs stopped and verified first.
- **Inputs:** vp-chain-gate; `tools/run_chain_gate.ps1`, `tools/run_config_tests.ps1`;
  `VP-pipeline-final.md` §3.8c; WP acceptance lists above.
- **Procedure (serial, coordinator only; times are estimates from today's runs):**

| # | Step | Est. time | Pass criteria |
|---|---|---:|---|
| 1 | Clean x64 Release solution build (standard + Beta renderer), stale `.iobj/.ipdb` removed | 40-60 min | 0 errors; warnings no worse |
| 2 | Full CPU suite single-threaded, TRX | 35-45 min | all pass (known skips listed) |
| 3 | Config suite off screen (4 shards, nothing else running) | 11-15 min | all pass (known 250 ms legacy filter timing item resolved or listed) |
| 4 | Manifest check, settings fingerprint tests, Bill-file guard | 5 min | pass; fingerprints identical |
| 5 | Byte identity: frozen r6 (B3-off-equivalent inputs, Original pathway) and frozen-b3base | 30-45 min | identical except the listed, accepted changes (WP-1 near-black pixels, WP-9 closed cases, WP-4 forced-fallback cases) |
| 6 | Segment tolerance gate (WP-6 commit C on vs off) | 20-30 min | ≤ 1 code; BTB 4 / WTW 1019 probes; sigma-delta stats unchanged |
| 7 | Whole-chain gate, Beta 377 (`VideoProcessorBetaTests.exe`, `VP_TEST_MATRIX=full`) then 7.360 | 60-90 min | matrix passes per phase (2 known calibration) incl. BTB/WTW and nets off/on axes |
| 8 | **Timing gates (B3 is the only path)** incl. strict absolute delay gate 4/3/2 frames × 23.976/24/25/29.97/30/50/59.94/60 + render-ahead, real-clip acceptance, B3 harness (0 entrance-once fallbacks), low-latency 50/60p cases | ~2.5-3 h | totals within budget at every rate (spread ≤ the gate tolerance, today 25 ms), 0 new failures; any failure re-run quietly once: fail in a quiet run = real |
| 9 | DV synthetic suite + DV trim shader parity | 40-50 min | 16/16; parity within the test tolerance |
| 10 | Net runner GPU tests (private export) | 30-45 min | WP-10 acceptance list |
| 11 | Grain/flicker real-clip checks for WP-9a and the entrance stages | 30 min | grain RMS within ±2 %, temporal independence unchanged |
| 11b | Frame analysis store: GPU reductions vs CPU reference on captured frames; per-consumer decision equivalence (LED correlation clips, intent set) | 20-30 min | identical decisions; histogram exact; cost recorded |
| 12 | Timers + VRAM report (efficiency): SlotTiming per stage before/after for S2/S3 merged, early upload, ER one pass; VRAM on the laptop | 20 min | recorded (no pass/fail; owner information); no GPU waits from timers |
| 13 | On-screen checks needing the coordinator: Ctrl+I/Config status "Fallbacks engaged" surfaces (forced cases), Config click-path for new keys | 20 min | text matches snapshots |

- **Total:** ~8-11 h wall clock, serial, one laptop. Split across two sessions if needed: steps
  1-7 (no timing) can run on one day, steps 8-13 on another, provided no code changes between them
  (same commit hash recorded).
- **On failure:** bisect by WP merge commits (each WP lands as its own merge/commit group on
  `integ/batch1`); fix on a branch, rerun only the affected steps plus step 5 and the affected
  timing case; a code change touching the render thread re-runs step 8 in full.
- **Result record:** `docs/design/integration-batch1-results.md` (pass/fail per step, commit hash,
  known gaps updated in `VP-pipeline-final.md` §3.8c).
- **Hours:** 10-14 incl. a rerun round. **Model:** opus (coordinator/integrator).
- **Depends on:** WP-1..WP-19 DONE (HIGH/MED audit findings fixed).

### WP-21 Batched Bill merge

- **Goal:** merge Bill's then-current beta once, with the owner's six decisions and the either/or
  rule; boundary-only testing of his code.
- **Inputs:** `upstream-merge-forecast-v1.4.02.md` (all; **refresh it first** with `git merge-tree`
  against his current beta — queue 162: keep the doc, refresh just before the merge); queue 146, 150,
  151; vp-either-or-overlaps; vp-rc3-merge-no-updater (no auto-updater in FL35 deploys).
- **Owner decisions applied (queue 150):** (1) subtitles Off by default with no profile; (2) adopt
  his smooth-motion source processing, keep ours as the either/or alternative; (3) fixed total
  delay authoritative, his adaptive buffering (VP-0208, when it lands) as `queue_policy`'s
  alternative — the WP-7 interface is where it plugs in; (4) our calibration may load its own LUT
  without Config Apply (on-screen notice); (5) display mode policy auto (FL35 60 Hz), settings on the
  Output tab; (6) timing: after this batch's big test.
- **Steps:** (1) refresh forecast (~1 h); (2) one merge commit of his beta tip in a fresh worktree
  `integ-bill` from the WP-20-passed commit; resolve the renderer hunks (expect far fewer than 26:
  WP-6/7/8 shrank our insertions; the forecast's draw-call hunk and the queue hunks especially);
  (3) follow-up commits: depth-scaled stale threshold kept in `BillQueuePolicy` only, walk union
  (WP-7 `SelectWalk` 2-line resolution), API number for his `RequestCalibrationLutReload` (next free,
  ~45), RPC op renumber (ours moves off 9), schema key order (his position-indexed keys first),
  `AnalysisLumaRange` vs our full→limited mapping (one model), his refuse-raw smooth-motion fallback
  made visible, libplacebo fork + his ROI-copy patch as 0005 (fork rebuild; `output_fbo_float32`
  0003 carried, with its loss test); his subtitle edits ported into forwarding-header targets of
  migrated files (port rule, forecast §3); (4) gates (boundary-only): build, CPU (ours + his native
  regressions), Original-pathway regression/byte-identity vs his behaviour (one run), each either/or
  option once at its slot contract (smooth motion: formats, BTB/WTW, sigma-delta still last;
  queue policy: delay totals), chain gate incl. BTB/WTW with smooth motion, strict delay 4/3/2 ×
  rates, DV suite, B3 harness; re-baseline frozen sets explicitly for smooth-motion/subtitle cases;
  owner: one live subtitle session.
- **Hours:** 14-20 + gates ~6 (forecast §4; likely the lower end after the extraction).
  **Model:** opus (integrator); sonnet for test-registration conflicts.
- **Depends on:** WP-20 passed.

### WP-22 M1 deploy prep

- **Goal:** a deployable x64 Release build of the merged result.
- **Steps:** chain gate on the GeForce (`VP_TEST_GPU=hardware`) after Beta 377; full audit + cinema
  checklist; 3090 real-time check when the 3090 is free (vp-cinema-gpu-3090; the laptop proves
  correctness and relative timing only); deploy only when the owner asks: back up and replace
  `VideoProcessor.exe` + `vprenderer\VideoProcessorVPRenderer.dll` as a pair from one build, verify
  both hashes; timestamped config backups; `[vprenderer.HDR]` deband off in the deployed config; no
  auto-updater (FL35 rule); report backups and exact config edits (AGENTS.md).
- **Hours:** 4-6 + owner time. **Model:** opus. **Depends on:** WP-21.

---

## 4. Dependency graph

```
WP-0 base ──┬─> WP-1 B3 re-audit fixes ─┐
            ├─> WP-2a dv-l8 verify ─────┤
            ├─> WP-2b settings verify ──┤
            ├─> WP-2c migrations verify ┼─> WP-3 merge helpers ─┬─> WP-4 silent fallbacks ─> WP-6 S2/S3 ─┬─> WP-12 calibration glue
            ├─> WP-2d motion-harness ───┤   (integ/batch1)      │                                       ├─> WP-14 DV glue
            └─> WP-2e net-runner verify ┘                       │                                       └─> WP-15 LED glue <─ WP-7
                                                                ├─> WP-5 efficiency (parallel)
                                                                ├─> WP-7 motion/timing (parallel) ──────────> WP-15
                                                                ├─> WP-11 status/GUI (parallel worktree) ─> WP-17 diagnostics
                                                                ├─> WP-16 intent glue (parallel)
                                                                ├─> WP-23 frame analysis store: interface early ─> producer (after WP-5 early upload, WP-7 S0)
                                                                │      └─ consumed by WP-8/9, 10, 13, 14, 15, 16, 17 (migrations inside those WPs)
                                                                └─> WP-8 S1 entrance ─> WP-9a backward field ─┬─> WP-9b N planes / fill
                                                                                                              ├─> WP-10 nets wiring (S1 + S4) ─> WP-13 output glue
                                                                                                              └─> WP-9c compose / crossfade ─> WP-9d delete pre-B3
all code WPs ─> WP-18 settings readers (last) ─> WP-19 final sweep + fixes ─> WP-20 BIG TEST ─> WP-21 Bill merge ─> WP-22 M1 prep
(WP-19 audits run continuously as each WP finishes)
```

**Critical path:** WP-0 → (WP-2e, the longest verification) → WP-3 → WP-8 → WP-9a → WP-9c/9d (with
WP-10 alongside) → WP-18 → WP-19 sweep/fixes → WP-20 → WP-21 → WP-22.

---

## 5. Schedule on one laptop

### 5.1 Resource lanes

| Lane | Capacity | Holder rule |
|---|---|---|
| Build slot 1 | 1 build (`/m:4 CL_MPCount=2`) | integrator / skeleton lead priority |
| Build slot 2 | 1 build | helper agents in priority order: DV > settings > net runner > motion/timing > efficiency > segments > status/GUI > migrations |
| GPU | 1 test at a time | coordinator runs or releases; never during a timing gate |
| Timing gate | exclusive (no builds, no GPU) | announced hold; agents stop and verify their own PIDs; single-threaded CPU-only unit tests allowed |
| Read-only agents | unlimited | audits, drafts, docs at any time, including during timing gates |

### 5.2 Phases (agent wall-clock; sessions of ~8-12 active hours)

| Phase | Work (parallel within a row) | Elapsed | Merge point / gate |
|---|---|---:|---|
| P1 | WP-0 (lead) + WP-1 (lead, after the flip) ∥ WP-2a-e (helpers; 2 builds max; GPU queue: WP-0 B3 harness → 2a DV suite → 2e runner tests → 2c LED readback) ∥ WP-6/7/5 agents finish their drafts code-only ∥ audits owed: 2c whole branch, 2e PlNetRunner | ~6-9 h | **M1-point:** `batch1-base` tagged; each WP-2 row DONE |
| P2 | WP-3 merges (integrator, slot 1) ∥ helpers rebase drafts onto each merge (git only) ∥ WP-11 starts in its own worktree after merge 1 (API 43 fixed) ∥ WP-23 interface commit drafted (new files only, worktree `fas`) | ~5-7 h | **M2-point:** merge checkpoint (full build, CPU, Config, ≤30 min GPU smoke) |
| P3 | Integrator applies in order WP-4 → WP-5 → WP-6 (B) → WP-7 (P-a, then P-b1/b2/c) on `integ/batch1`, one checkpoint per WP, then the WP-23 interface + producer call site ∥ lead does WP-8 (S1 reads FAS) in a separate worktree on `integ/batch1` and hands a branch ∥ WP-11, WP-16 in parallel ∥ audits of each as they land | ~12-16 h | **M3-point:** S1/S2/S3/S0 in; CPU green; short GPU byte-identity smoke |
| P4 | Lead WP-9a → 9b/9c → 9d ∥ net-runner agent WP-10 against S1 (then S4) ∥ WP-12/13/14/15/17 glue (one agent each, or two agents in sequence; integrator applies) ∥ audits | ~20-28 h | **M4-point:** pre-B3 deleted; all glue in; 9a audit done |
| P5 | WP-18 (settings readers) ∥ remaining audits; then the integrator's whole-batch sweep and fix round | ~8-12 h | **M5-point:** code freeze for the big test (commit hash recorded) |
| P6 | **WP-20 big test** (serial; split over 2 sessions allowed with no code change between) + rerun round | ~10-14 h | **batch1 DONE** |
| P7 | WP-21 Bill merge + its gates | ~20-26 h | merged baseline |
| P8 | WP-22 M1 prep (+ 3090 check, owner) | ~4-6 h + owner | M1 |

GPU windows are short and serialised in P1-P5 (smoke runs only); all long GPU and timing work sits
in P6 (and P7's gates). That is the "one large test procedure" the owner asked for; the smoke runs
exist only to keep bisects short.

### 5.3 Holds

- A hold is announced by the coordinator (message + `D:\codex\vp-build-slots\HOLD` file with the
  reason and expected end). Agents check for it before starting any build/GPU step.
- On a hold: stop your own builds/scripts by recorded PID, verify with `tasklist /FI "PID eq <pid>"`,
  release your slot, switch to read-only work. Never kill another agent's process.
- Build-slot runner: `VP_SLOTS=2` exported in every brief; a slot older than 75 min is reclaimed by
  the runner even when live (VP-barriers R7): long LTCG builds should touch `owner.txt` or the
  coordinator raises the reclaim age before the batch (0.5 h fix, do in P1).

---

## 6. Risks, owner decision points, estimate

### 6.1 Risks

| Risk | Where | Mitigation |
|---|---|---|
| WP-9a changes the stateful stages' vector semantics (grain, flicker) | WP-9 | separate audit; real-clip grain/flicker checks; keep the forward-field rule selectable until the big test passes, then remove |
| Render-thread timing regressions from extractions (CPU tests cannot see them) | WP-7, WP-8, WP-9 | byte identity at each commit; timing gates in WP-20; bisect by WP commit groups |
| Merged S2/S3 not byte-identical | WP-6 C | switch default off; tolerance gate; owner acceptance (D1) |
| Gate load failures (gate10 lesson) | WP-20 | exclusive hold, PIDs verified, quiet rerun rule |
| Bill keeps committing; the merge grows | WP-21 | extraction first shrinks our insertions (WP-6/7/8); refresh the forecast just before; one merge commit |
| Laptop VRAM (4 GB) with early upload in all modes + rgba32f nets | WP-5, WP-10 | VRAM guard visible fallback; VRAM report in WP-20; fbo_float32 per tier is the owner's lever (D2) |
| D light domain mismatch (R8) | WP-10 | D stays off by default until confirmed; fingerprint locks the domain |
| One host writer = bottleneck | P3-P4 | helpers deliver patches; integrator applies in a fixed order; read-only work fills waits |
| Usage limits mid-WP | all | section 8 resumability |
| Hidden Bill-file edits | all | Bill-file guard in the manifest check; audit rule check |
| FAS consumers change decisions or read stale records | WP-23 + consumer WPs | per-consumer equivalence tests; shared epoch with vector validity; switch until WP-20; old decodes deleted only after the big test |
| FAS readback stalls the render/arrival thread | WP-23 | async readback ring, never Map-with-wait (1dfcaf21 pattern); timers in WP-20 |

### 6.2 Owner decision points

| # | Decision | When | Claude's recommendation |
|---|---|---|---|
| D1 | Accept the merged S2/S3 hooks (`output_segments_merged` default on) after the tolerance gate (≤ 1 code, probes unchanged) | after WP-20 step 6 | accept if 0-1 code and timers show a saving |
| D2 | `fbo_float32` per tier: default ON everywhere (full precision); laptop tiers may choose off (visible). The audit measured WTW highlights ≤ 0.84 of a 10-bit step when off | before WP-18 (key table) / settings | keep ON by default; offer off on Low/Classic only, shown in status |
| D3 | AdaptiveDownscale polar vs separable: A/B on clips (upscale study prepared on feat/net-runner) | when the laptop/3090 can run the study (not in this batch's critical path) | separable if faithful (owner preference vp-prefer-separable-simple) |
| D4 | D bypassed while VP deband is on (deployed HDR deband is off; SDR deband on?) — accept that D runs only with deband off until a D trained with deband exists (round 3) | WP-10 | accept; record as a known gap |
| D5 | P-c: Original pathway uses `BillQueuePolicy` (behaviour change on Original only, equal under the stated precondition) | WP-7 step 5 | yes (Original = Bill's) |
| D6 | Settings: the owner's click-through of the pseudo-final layout (after the usability audit) and the tier-meaning changes (queue 135) | after WP-2b | not blocking the merge; fixes land before WP-18 |
| D7 | Upstreamable PRs to Bill (HookSegment, queue policy interface + walk, generic status entry, refresh fallback sink, churn patch, "notes for Bill") | any time after WP-20 | owner approves each PR individually; none opened by agents |
| D8 | 3090 window for the real-time check (laptop cannot prove 4K real time) | before WP-22 | after round 2/3 training pauses |

### 6.3 Effort totals (agent wall-clock hours)

| Group | WPs | Hours |
|---|---|---:|
| Base + B3 fixes | WP-0, WP-1 | 5-7 |
| Helper verifications | WP-2a-e | 14-19 (+1 audit) |
| Merges | WP-3 | 5-7 |
| Patches | WP-4, 5, 6, 7 | 21-28 |
| S1 + B3 closure | WP-8, 9 | 26-38 |
| Nets | WP-10 | 8-12 |
| Remaining glue | WP-11-18 | 28-37 |
| Frame analysis store | WP-23 (+ ~6-8 h of consumer migrations counted inside WP-8-17) | 8-11 |
| Audits + fixes | WP-19 | 21-28 |
| Big test | WP-20 | 10-14 |
| **Batch subtotal** | WP-0..20 + WP-23 | **~150-205 h of agent work** (much of it parallel) |
| Bill merge | WP-21 | 20-26 |
| M1 prep | WP-22 | 4-6 + owner |

The agent-work total is larger than the ~80 h option-(d) figure because it also contains the
helper verifications, B3 closure (23-35 h), net wiring, audits and the big test.

### 6.4 Realistic estimate (not a deadline)

- **Critical path:** ~85-115 h of serial agent wall-clock (WP-0 2-3 → WP-2e 4-5 → WP-3 5-7 →
  WP-8 6-8 → WP-9 20-30 → WP-18 5-6 → WP-19 sweep/fixes 6-10 → WP-20 10-14 → WP-21 20-26 →
  WP-22 4-6). Parallel lanes keep everything else off this path if two helper agents and the
  integrator run concurrently.
- At ~10 active hours per day on the laptop (usage windows, owner travel, the laptop being needed
  elsewhere), that is **~9-12 working days** from the next session start.
- Starting ~26 Oct: **batch 1 done (big test passed) around 3-5 Nov; Bill merge done around
  5-8 Nov; M1 deploy-ready around 7-12 Nov**, most likely ~9-10 Nov. The ~6-7 Nov estimate is
  reachable only if every gate passes first time and the laptop is available every day; it should
  not be pursued by skipping steps (owner note).
- Biggest swing factors: WP-9a (backward-field rule), timing-gate failures needing quiet reruns, and
  the size of Bill's upstream growth by merge time.

---

## 7. Agent brief template (every WP brief carries all of this)

```
TASK: WP-<n> <title> — goal in one sentence. Section 3 of docs/design/integration-batch-plan.md
      (branch docs/integration-batch-plan) is the spec; read it and the cited sources.
MODEL: opus (lead/judgement) | sonnet (mechanical sub-task: <what>).
WORKTREE / BRANCH: D:\codex\videoprocessor\<name> on <branch>, from <base sha>. Do not touch
      other worktrees. Integrator-only: host call sites on integ/batch1 (deliver a patch instead).
RULES:
 - Host = call sites only (LibplaceboVideoRenderer.cpp/.h, VideoProcessorDlg.cpp, ConfigEditorWindow.cpp).
 - Never move/rename/own a file in Bill's beta (git cat-file -e origin/v1.4.02-beta:<path>);
   no logic in Bill's clusters (central-migration-needs.md §2.1).
 - Overlap with Bill's function = either/or setting; Original pathway = Bill's.
 - No silent fallbacks ("Fallbacks engaged: ..." in Ctrl+I, Config status, Ctrl+Alt+D + one-time notice).
 - BTB/WTW preserved; grain never averaged; no hard-coded rates/sizes.
 - Two-commit rule: behaviour-free move first (byte-identical), change second behind a switch.
 - Plugin API: write "API: next"; the integrator assigns numbers.
 - Every new setting: Config control + settings reference entry + slots.json/manifest; regenerate
   with `python tools/check_module_manifests.py --write`.
 - State UPSTREAMABLE vs VP-ONLY per commit; upstreamable commits self-contained, no src/VP-V
   dependency. No PR/push to billslack2 or enchywastaken. Pushes only by the coordinator.
LOAD:
 - Builds only via: VP_SLOTS=2 bash D:/codex/vp-build-slots-run.sh <wt> <log> -- MSBuild ... /m:4
   /p:CL_MPCount=2 /p:Configuration=Release /p:Platform=x64
   /p:MagewellSdkRoot="C:\Program Files\MWCaptureSDK 3.3.1.1596\SDKv3". Build only touched projects.
   Delete stale .iobj/.ipdb before LTCG links.
 - Tests: your area's classes only, single-threaded, vstest /Logger:trx (read the TRX).
 - NO GPU tests unless the coordinator releases the GPU to you; never during a timing gate.
 - HOLD (D:\codex\vp-build-slots\HOLD or a message): stop your builds/scripts by recorded PID at once,
   verify they are gone, release the slot, continue read-only. Never kill by name.
 - Say the projected time before anything over a minute.
ACCEPTANCE: <copy the WP's acceptance list, with tolerances>.
AUDIT: the WP gets an independent read-only audit (correctness + efficiency); do not audit yourself.
RESUMABILITY: commit after every step; WIP commits start "WIP(WP-<n>):" with a State: block; update
      the status board row (section 1) and docs/agent-reports/<branch>.md.
REPORT: what changed (commits), tests run with results (TRX paths), what is unverified, the next step.
WRAP-UP: at the wrap-up time stop new work, commit (WIP-marked), report.
Commit messages end with: Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
```

---

## 8. Resumability (usage runs out, session ends, laptop needed)

1. **Commit WIP immediately** in the WP's worktree: `WIP(WP-n): <what>` with a body:
   ```
   State: <step k of the WP's step list>, <done / half-done>
   Built: yes/no (log path)   Tested: <classes, TRX path> / not run
   Next: <the exact next action>
   Open: <questions, known breakage>
   ```
   Never leave uncommitted edits in a worktree overnight; never commit captured video pixels.
2. **Update the status board** (section 1) in the same commit if the WP's branch carries this doc,
   otherwise in the integrator's next commit on `integ/batch1`.
3. **Record the state** as one new numbered item in the queue memory (`vp-post-reset-queue.md`):
   WP, branch, sha, next step.
4. **Handover:** append the same line to `D:\codex\vp-handover\06-open-items.md` and commit in that
   repo (private, local).
5. **Release resources:** stop your own PIDs, release build slots, never leave a stale slot folder.
6. **Resuming:** read section 1, then the WP's last commit body ("Next:"), rebase onto the current
   `integ/batch1` tip (git only), re-run the WP's last checkpoint before continuing. If the WP's
   base moved under a patch, re-check with `git apply --check` (or `-3`) before editing by hand.
7. **Partial batch:** WPs land on `integ/batch1` only when their checkpoint is green; a half-done WP
   stays on its own branch. The big test (WP-20) never runs on a tree with a WIP WP in it.

---

## 9. Sources (read these; this plan only cites them)

- Memory: `vp-post-reset-queue.md` items 91-173; `vp-host-call-sites-only.md`,
  `vp-either-or-overlaps.md`, `vp-upstreamable-track.md`, `vp-pipeline-units.md` (chain-segment
  principle), `vp-no-silent-fallbacks.md`, `vp-btb-wtw-preserve.md`, `vp-grain-faithful.md`,
  `vp-general-not-fixed.md`, `vp-cpu-thread-budget.md`, `vp-chain-gate.md`, `feedback-idle-agents.md`,
  `vp-never-kill-by-name.md`, `vp-barriers-2026-10-24.md`.
- `D:\codex\videoprocessor\central-assess` → `docs/design/central-migration-needs.md` (§2.1
  clusters, §4 (d), §5 sequence, §6 segments, §7 quick wins).
- `efficiency-audit` → `docs/design/efficiency-audit-2026-10-24.md`.
- `upstream-forecast` → `docs/design/upstream-merge-forecast-v1.4.02.md`.
- `migrate` → `docs/design/migration-m2-plan.md`, `migration-early-inventory.md`,
  `migration-path-map.md`, `module-findings-2026-10-24.md` (parts 1-3),
  `silent-fallbacks-callsites.md` / `.patch`.
- `nets` → `docs/design/net-runner-wiring.md` / `.patch`, `net-export-fingerprint.md`,
  `upscale-study.md`.
- `skeleton` → `docs/design/VP-pipeline-final.md` (§3.8c known gaps), `docs/decisions/ADR-0024-*`,
  `docs/design/b3-fallback-closure.md` (written, not yet committed at 30a42479).
- `dv-l8` → `docs/design/dv-post-decode-deband.md` (later item, not this batch), `docs/agent-reports/`.
- `settings-preview` → `docs/settings/organisation.md`, `docs/settings/what-changed.md`.
- `segments` → `docs/design/segments-callsites.md` (draft, uncommitted).
- `efficiency-fixes` → `docs/design/efficiency-callsites.patch`, `efficiency-upstreamable-churn*.patch`
  (drafts, uncommitted).
- `motion-timing` → `docs/design/motion-timing-callsites.md` (draft; commits c33aaa41..c3e45812).

## End state (owner, 2026-10-24)

Bill's code becomes an UNMODIFIED, separately built component (a "vendor drop": take his new release, rebuild, done). Our framework attaches only through hook points and interfaces (segments, queue/timing policy, frame analysis store, net runner, status entry). Not a separate process: sharing GPU pictures across processes adds synchronisation, latency and complexity for no gain.
Path: option (d) extraction (this batch) + offering the hook points to Bill as PRs (each approved by the owner). Until he accepts them, the only changes to his code are a small, documented patch set that adds the hook points. Every WP must leave that patch set smaller or no larger.

D1 decided: the merged S2/S3 hooks become the default once the GPU fixture shows only removed-rounding differences (BTB/WTW unchanged).
D3 decided: no A/B; use the correct downscaler, and the separable 1-D version wherever the polar one can't be used.
D4 decided: D runs only with VP deband off for now; a D for the deband-on case goes on the round-3 list.
D8 decided: the 3090 real-time check runs with the next training run, after the restructure is complete.

## WP-24: captured-repeat render reuse (owner concern: GPU time lost in Bill's host)
The largest GPU waste on Bill's side: captured repeats (24-in-30, 30-in-60) re-run the whole libplacebo render (efficiency audit #4; 20-50% of renders on that content). Recover it inside option (d) with ONE small hook in his render loop: when the source frame is a captured repeat of the last rendered one and nothing upstream changed, reuse the cached rendered picture (the de-judder extras' rendered-picture cache) and run only the OUTPUT segment (S3: dither etc. keep running per refresh). Upstreamable (offer to Bill). Byte-identity: repeats must equal a full re-render except temporal-dither phase (dither keeps advancing). Early in the batch, after WP-6; ~4-6 agent h; timing-gated.

## WP-23 addendum: one luma decode for everyone (owner: "pre-empt and feed his code")
Today the same 4K raw frame is decoded to luma on the CPU 5-7 times: Bill's SceneDetector, ActivePictureEvidence, SubtitleBoxDetector, HdrInference, and our ContentIntent/IntentInterpreter, LedLookahead and the NVOF producer. Bill already has `AnalysisLumaSource` (a shared luma product with a generation counter). Make it the single per-frame decode: filled once (by the frame analysis store's producer), read by all of his analysers and ours. We feed his code its INPUT once; his decisions (crop, subtitles, scene cuts) stay his, and where ours overlap the either/or rule applies. The change to his analysers is small and upstreamable (offer to Bill). Resolve the AnalysisLumaRange vs our full→limited mapping clash here (one range model, chosen explicitly). Saves CPU threads (laptop), not displayed latency.

## Change (owner, 2026-10-24 evening): M2 moves are IN the batch
The remaining M2 module moves (our files only, per docs/design/migration-m2-plan.md, each with its mandatory README/keys/fallback review) are done inside this batch, paired with the glue extraction of the same module (WP-6..18), so each module is touched once: extract its glue into the module, move its files into src/VP-V, review it, test it. Bill's files never move (guard in tools/migration_path_map.py). M2 step 0 (switch includers, delete the forwarding headers) runs at the end, just before WP-20 (the big test round). Adds roughly 30-35 agent hours; dates are estimates.
