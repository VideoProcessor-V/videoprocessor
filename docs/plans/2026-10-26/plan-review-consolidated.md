# Plan review (quality gate): WP-16/9/10, M2 rest + WP-18 + Config, F-11 + small items

## 0. STATUS UPDATE 2026-10-26 (after the end-of-week merges; integ/batch1 local d85519e2, fork integ/batch1-wip)

Done since this plan was written (merged on integ, Opus-audited, compile + targeted green):
- Item 1: S1 (feat/m2-entrance) is in group D; B2 (`statefulBreaks` + segment uses `LogEntranceCounters`) merged.
- Item 2: Config usability merged (Dynamic Drive Main, four tiers with Upper "planned", Set-by lines, Approve/Keep record-only).
- Item 11 (part): step 0 commit A (25 of our includes switched, 9 forwarders deleted, 51 kept for Bill includers), step 9 (NlsMaskMapping.h moved; keys listed), step 7 (SdrAmbientLift extracted from Bill's LibplaceboRenderParameters). STILL OPEN in item 11: Ambient*/AmbientConfig into colour.appearance, CTwoPlane420VideoFrameFormatter.* into a module.
- Item 18 (part): our 136 tests moved out of Bill's LibplaceboLutParserTests (his file = his blob again); one category scheme (tools/test-categories.json, checker, filter generator). STILL OPEN: wire the generated filters into run_chain_gate / plan_test_shards / dv scripts / CI; G6.
- F-8 (hdr_inference either/or) merged, matches Bill exactly in original mode.
- Copyright headers: VP-V contributors + GPL-3.0-only on 60 VP files.

Baseline 2026-10-26 on d85519e2: CPU 4283/4284 (1 stale source pin, below), Config gate passed, GPU-hardware 19/19, WARP-capable graphics tests on the RTX 219/219.

New items (add to the list):
- N1 (first, tiny): fix DetailRestoreConfigTests::DetailRestoreSourceResolutionInputOrderInTheRenderer (source pin must read src/VP-V/segments/EntranceSegment.cpp after the S1 move).
- N2: tier sliders get an APPLY button (owner 2026-10-26): moving the slider previews what the tier changes, nothing is written until Apply (also removes "tracking applies crossed tiers").
- N3: divergence sweep first pass (divergence-sweep-firstpass.md): 181 Bill files changed (+57k); ~88% of added lines are VP feature code inside Bill files (e1) -> plan a large extraction programme (bigger than item 3/F-11 alone); e2 behaviour/default changes -> either/or (Original = Bill) or revert; some defaults need an owner yes/no list. Opus review of the first pass + re-sweep after merging Bill's tip into a scratch branch.
- N4: test runs: GpuWarp tests run both on WARP and on the real GPU (VP_TEST_GPU=hardware) in WP-20 and baselines.
- N5: README mentions of LibplaceboLutParserTests for moved tests (7 module READMEs).
- N6 (owner 2026-10-26): INTENT AUDITS per module (all now-separated modules + segments), against the owner's standing directives: no silent fallbacks (every downgrade visible: Ctrl+I, notice, Config status, Ctrl+Alt+D), precision/bit-depth throughput end to end (12-bit only via VPV for DV displays, 10-bit + sigma-delta otherwise; no hidden 8-bit or fp16 truncation; full-precision tiers), grain never averaged/interpolated away, BTB/WTW preserved through every conversion, no hard-coded rates/cadences/resolutions, slot off = byte-identical, Original pathway = Bill's, either/or kept, vector validity/fallback rules, LED rules (CNTR never above need, hold full when measurement off). One checklist doc; opus audits (sonnet may pre-collect evidence). After the code is final (after item 14 / N3 extraction), before WP-20.
- N7 (owner 2026-10-26): EFFICIENCY SWEEPS of every separated module: GPU passes per frame, redundant uploads/readbacks/decodes, per-frame allocations, shader/LUT rebuilds, CPU waits/locks, duplicated work between modules now that boundaries are explicit; findings fixed or listed; costs confirmed on the 3090 in WP-20. Same timing as N6 (can share the per-module walk).
- N8 (owner 2026-10-26): CHROMA PLAN to be written: the held chroma-upscale placeholder slot right after the input stages (pipeline-units; output must stay 2-plane luma + full-res CbCr so libplacebo does not upsample again; runs once per source frame); VP chroma upscaler vs libplacebo's as either/or (Original = Bill's); interplay with native RGB/4:4:4/4:2:2 ingest, DV tunnel unpack and re-base; whether a chroma repair add-on (nets are luma-only today) is wanted; tests + byte-identity when the slot is off.
- N9 (owner 2026-10-26): UPSCALE/DOWNSCALE NET PLAN to be written: the agreed design (memory vp-neural-bounded): always-4K processing, per-axis x2 net restore only on axes the re-base found upscaled, display aspect only in the separable downscale, output scaling always a downscale except 8K outputs; single-axis C/B nets, D refine (D_small likely), A repair; VP two-axis 1-D luma re-base matching the training port before C is enabled (fingerprint-gated); the 8K bypass placeholder (neural stages bypassed, motion at 1/4); processing-resolution policy; how this extends WP-10 (which only wires A and D) and depends on the round-2/round-3 kit results.
- Q5 changed: no deployed config exists (the owner runs madVR); golden fixtures = (1) the owner's madVR-derived settings in Bill's context (Original pathway), (2) VP-V defaults.
- Owner answered all 8 questions in section 5 as recommended.

Remaining list order: N1 -> 0 -> 3 (+N3 planning) -> 4 -> 5 -> 6 -> 7 -> 8 -> 9 -> 10 -> 11 rest -> 12 -> 13 -> 14 (+N2 can go with any Config round) -> 15 -> N6 intent audits + N7 efficiency sweeps -> 16 (incl. final Bill-code gate) -> WP-20 -> 17 -> 18 rest -> 19 -> 20.


Read-only review, 2026-10-25 (opus). Plans reviewed: `plan-wp16-wp9-wp10.md` (P1),
`plan-m2rest-wp18-settings.md` (P2), `plan-f11-triage-and-small-items.md` (P3), against
`batch-plan/docs/design/integration-batch-plan.md`. Spot checks by git/grep on `group-d`
(integ-group-d aa5d1e6b), `integ-batch1` (0349051c), `feat/m2-entrance` (85b8e592), Bill =
`origin/v1.4.02-beta` (tip 8c453746; our merge-base 83b7ffea). No builds, no edits, no git writes.

## 1. Fact checks

| # | Claim (plan) | Verdict | Evidence / correction |
|---|---|---|---|
| 1 | 55 forwarders; 46 have a Bill-owned includer; 16 Bill files (P2) | **Confirmed** | manifests list 55; 46 with Bill includers; same 16 files; 9 free forwarders exactly as listed. No forwarder old path exists on Bill's tip (no Bill file was moved). |
| 2 | ~100 include lines in Bill files; 42 of our includer files / 149 lines (P2) | **Partly wrong (minor)** | Count gives ~84 Bill include lines and ~16 of our includer files (~28 lines, 112 total). Commit A is smaller than stated; sizes unchanged. Bill-file include lines are our additions (e.g. LutParserTests diff adds all `<vprenderer/...>` lines): confirmed. |
| 3 | Step 7 files byte-identical to Bill; RenderParameters +89/-3, +28/-1 (P2) | **Confirmed** | `ToneMappingTuning.h`, `LibplaceboOutputPolicy.*` 0 diff vs tip and vs merge-base. |
| 4 | Step 9: only `NlsMaskMapping.h` ours (P2) | **Confirmed** | NlsGeometryPolicy, NlsHookRecovery (+45/-3 ours), AnamorphicPresentation, AlphaSourceCropPolicy are Bill's. Note: the AlphaSourceCropPolicy "-2054" is **Bill's upstream rewrite** (our diff vs merge-base is +7/-2); P2's wording is right, but quote merge-base diffs, not tip diffs, when sizing *our* divergence (e.g. `RendererProfileConfig.h` is +389/-20 vs merge-base, not +394/-102). |
| 5 | Step 19 / WP-18 files all Bill's (P2) | **Confirmed** | ConfigFile.cpp (+2), ConfigSchema.h (0), MainConfigSchema.h, RendererProfileConfig.h are Bill's; AmbientConfig/AmbientAdaptation ours. Step 19 file moves are forbidden by the rule, not a choice (drop Q18a). |
| 6 | Net wiring patch corrupt (P1) | **Confirmed** | `git apply --check`: "corrupt patch at line 16" on both trees. |
| 7 | D plugs into the **existing** `postBlend` callback of `AdaptiveDownscaleStage` (P1) | **Wrong** | No `postBlend` anywhere in `src`. `AdaptiveDownscaleStage::Host` has fallbackFramePeriod/bitrate/nlsMapping/ssimInChain/motion/publishStatus only. A seat must be **added** in `AdaptiveDownscale::Blending` (VP file, fine) + Host callback. Also check D's behaviour when the blend hook is not registered (no downscale): D must then report "bypassed (no output scaling pass)" visibly. +1 h to WP-10. "No new S4 segment" still holds. |
| 8 | S1 missing from integ/batch1 and integ-group-d (P1) | **Confirmed** | `feat/m2-entrance` is not an ancestor of either (24 commits; merge-base with group D 43ebff50). Being re-added per caller. **Not sized in any plan** (gap). |
| 9 | F-11 .cpp +1094/-55; class split ~520/535/45 (P3) | **Total confirmed, split unverified** | numstat matches all P3 file rows. Split is a hunk-span estimate; P3's own step 1 (per-hunk table) must produce the exact one. P3 **missed** `CTwoPlane420VideoFrameFormatter.{h,cpp}` (+131): it is a new VP file (not Bill's) in Bill's folder, so not divergence but an M2 move candidate. Bill's tip touched `CR210...cpp` and `IVideoFrameFormatter.h` (+1 each, 9a26dc17, `sceneMinPq`): small WP-21 conflict; keep Bill's field in Bill's struct when moving extras. |
| 10 | F-11 Q1 "does Bill's DeckLink path deliver R10b/R10l?" (P3) | **Answerable from code: yes** | Bill's host (tip, LibplaceboVideoRenderer.cpp ~815-820) routes `R10b`/`R10l`/`R210` with `dolbyVisionTunnel` to `CR210DolbyVisionTunnelFormatter`, which only reads r210 words; Bill's config offers `rgb_10bit_packing` AUTO/R210/R10B/R10L. So R10b/R10l = **real bug, class 1**. Drop Q1. |
| 11 | Bill: first 4096 px probe, loss limit 24 (P3) | **Confirmed** | `kPacketLossLimit = 24`, `min(w*h, 4096)` in Bill's .cpp. |
| 12 | LibplaceboLutParserTests: uncategorised GPU methods, 1 category (P3) | **Confirmed, with a bigger finding** | 204 methods, 206 `TargetLutGpuFixture` refs, 1 Category. But Bill's version has 68 methods; **we added ~136 methods / +12,914 lines to Bill's test file**. B1's "do not edit Bill's file" rationale is weak (the lines are ours), and the real issue is a 12.9k-line VP block in a Bill file that Bill also edits (68 -> 75 methods on his tip). See gap G6. |
| 13 | No `L"TestCategory"`; Category counts 55/4/2/1 (P3) | **Confirmed** | exact. |
| 14 | `EntranceCounters` has no `statefulBreaks` (P3 B2) | **Confirmed** | adapter struct (DiagnosticsAdapter.h:427) has 10 fields, no breaks; segment (m2-entrance EntranceSegment.cpp:312) and host (5911) print "history breaks". P3's claim "m2-entrance and wp17 both in integ groups" is **wrong** (S1 not in): B2 belongs inside the S1 re-cut. |
| 15 | Intent keys mapped to `control.config` in OriginalPathway.h 75-79; no intent slot (P1) | **Confirmed** | `PipelineSlots.h` has no intent Id; `control.net_runtime`/`repair.encoding`/`scaling.output` exist (placeholder/partial). |
| 16 | Key-name conflict `encoding_repair`/`hybrid_refine_d` vs `nets_repair`/`nets_d` (P1) | **Confirmed** | slots.json 486/1109/1845. |
| 17 | `perf/byte-identical` "must exist before the byte-identical branch merges" (P3 B4) | **Wrong premise** | `perf/byte-identical` is already an ancestor of integ/batch1. B4 is now evidence for WP-20 (steps 5/12), not a merge gate. |
| 18 | LED repeat refusal `ledMeasurePerRefresh` (P3 B3) | **Confirmed** | RepeatReusePolicy.h:94-98, host 7688. |
| 19 | Frozen r6 / frozen-b3base sets owed (P1) | **Confirmed** | only referenced in docs; no data set in tree. |

## 2. Rule check

- **F-11 class 3 removal violates two rules (main correction).** `OriginalPathway.h:35` sets
  `dv_tunnel_fixes = off` (= Bill's formatter) and the owner's either/or rule says "Original = Bill's,
  keep both selectable". Removing `m_bugHuntFixes` makes the Original pathway diverge from Bill and
  deletes the either/or. **Keep the switch** (register it as "VP restore switch for Original; not
  part of any PR"). Drop P3 step 3 (-0.7 h) and Q3. The PR form to Bill simply omits the switch.
- **F-11 class 1 verdicts:** placement vote, flat-header proof, grey-SDR exit, truncated frame,
  interlaced lock, loss counted in source pictures: genuine bugs (keep). Time-based limits: genuine
  bug at 50/60p, identical at Bill's 23.976 (general-not-fixed rule) - keep. **R10b/R10l: genuine bug**
  (fact 10). **Packet majority vote: an improvement, not a bug** unless shown to recover real packets:
  rule says otherwise match Bill. Decide on evidence in F-11 step 1 (count recoveries on the DV synthetic
  corpus + any recorded real captures with link noise): zero real recoveries -> revert to Bill; else keep
  with the reproduction in the register. Lead decision, not an owner question.
- **Behaviour-neutral refactors in Bill's file** (static helpers ~45, GPU unpack split ~60): not bug
  fixes, so each must be justified as the minimum seat for a kept fix or hook. Register the GPU split as
  "hook point, upstreamable" (D7 list) rather than "stays". The new `DolbyVisionTunnelRaster.h` in Bill's
  folder is a VP file: put it under `src/VP-V/modules/input/signal/` and include it from Bill's .cpp
  (one include line) unless Bill would want it in a PR.
- **WP-16 slot:** required by "every slot switchable off"; not an owner question (decide: add
  `control.intent`). Ensure the new slot's `settings_keys` go into WP-18's tables.
- **WP-16 / step 0:** P1 plans **new forwarders** for the intent `git mv`. Do not: switch includers in the
  same commit (Bill-file include lines go into the step-0 patch). Same for any WP-9.0 moves.
- **WP-9a hidden key `entrance_field`** and B3's replay: both need Original entries (P1 has it; P3 B3 adds
  no key, fine). Temporary key removal after WP-20 is a post-test task.
- **WP-9 case 2** (stages see the clean frame under subtitle move) is a visible VP-path change: fine under
  the rules, but list in WP-20 step 5 accepted changes and ask the owner (Q3 below).
- **No silent fallbacks:** WP-10 D-not-seated case (fact 7), step 7's dead manifest fallback text, step 9's
  NLS safe-fit possibly log-only: all must be visible or removed. P2 flags the last two; add the first.
- **Grain:** WP-9a audit + real-clip RMS gate present. B3 replay variant is exact by construction (good).
- **No hard-coded rates:** F-11 time limits comply; B3 must not assume N repeats.

## 3. Gaps, double counting, conflicts, ordering errors

Gaps
- G1 **S1 re-cut unsized** (prereq of WP-9, WP-10, B2): ~4-5 h opus (host ~-850/+100 over regions moved by
  WP-23/m2-output/m2-motion/WP-14; EntranceSegment re-applied; build + CPU). Fold B2 (`statefulBreaks`
  field + adapter call) into it.
- G2 **Frozen r6 / frozen-b3base recording** must happen **now**, on the current integ tip before 9a/9.0
  change anything (P1 put it "before 9.6"; too late for b3base as a pre-change reference). ~1 h GPU on
  mains, lead only.
- G3 **WP-10 D seat** in `AdaptiveDownscale::Blending` (fact 7): +1 h.
- G4 **Ambient files decided three times**: P1 Q2 (into intent), P2 step 7 (`SdrAmbientLift` ->
  colour.appearance), P2 Q18c (`AmbientConfig/Adaptation` -> appearance). Decide once (lead): all
  `Ambient*` + `SdrAmbientLift` -> `colour.appearance` in the M2-tail round; intent module includes them.
- G5 `CTwoPlane420VideoFrameFormatter.*` (ours, in Bill's folder) not in any M2 list: add to M2 tail.
- G6 **VP test methods inside Bill test files** (LutParserTests +12.9k lines/~136 methods;
  DolbyVisionTunnelFormatterTests +1356; ConfigFileTests, MotionRendererTests): move them into
  `src/VP-V/**/tests`. Largest avoidable WP-21 conflict surface. Merge with B1 (categories land on the moved
  methods; filters regenerated once). After WP-20, before WP-21 (test-only; CPU/WARP rerun + GPU subset).
- G7 WP-19 audit cost for these packages not in any plan: ~1 audit per package, ~0.5 h / 100-150k each.

Double counting
- WP-8 commit 2 folded into WP-9.0 (P1): do not also count master-plan WP-8 hours.
- B2 is inside S1 re-cut; step 9 shares step 0's build; F-11 step 3 removed (rule).
- Ambient move counted in P1 and P2: count once (M2 tail).

Conflicts (serialise these shared files)
- `slots.json` / `PipelineSlots.h` / `OriginalPathway.h` / `RendererProfileConfig.h` validator /
  `VpvSection.cpp`: WP-16 (new slot), WP-10 (5 keys + Config controls), WP-9a (`entrance_field`),
  Config usability (`retirement_A..D`), 9.6 (deletes `pipeline_entrance_once`), WP-18 (tables).
  Order: WP-16 slot id first (pre-assign), WP-10 keys, Config usability, 9.6, then WP-18.
- `EntranceSegment.cpp` + S1 host adapter: WP-9.0 -> WP-10 A stage -> 9a (P1 order is right).
- `VpvSection.cpp`: Config usability vs WP-10 Config controls: same agent or strictly sequential.

Ordering errors
- P2 "step 0 after all moves" omits WP-16's intent move, WP-9.0 consumer moves, Ambient/TwoPlane420 moves:
  step 0 commit B must follow WP-16 and WP-9.0 (and 9.6, which deletes includers).
- WP-18 golden fingerprints must be captured **after 9.6** (key removal changes the fingerprint) and after
  WP-10/16/Config keys, i.e. WP-18 is genuinely last; P1's order omits WP-18 entirely.
- P3 B2 "after m2-entrance and wp17 land": S1 is not landed; move into G1.
- P3 B4 framing (merge gate) obsolete (fact 17); G9 waits for WP-9 ring.

## 4. Consolidated ordered work list

Hours = agent wall-clock; tokens approximate. S = sonnet-draftable per written plan, O = opus needed.
Every package: independent read-only audit before its verification build (rule; ~0.5 h, 100-150k, O).

| # | Package | Deps | Hours | Tokens | Model split |
|---|---|---|---|---|---|
| 0 | **Frozen r6 + frozen-b3base recording** (GPU, mains, lead) | integ tip now | 1 | 50k | O (lead runs) |
| 1 | **S1 re-cut on group D tip** + B2 (`statefulBreaks` in adapter, segment uses `LogEntranceCounters`) | group D landed | 4-5 | 450-550k | O |
| 2 | **Config usability** (Set-by, DD Main, Approve/Keep, four tiers) | none (disjoint) | 4.5-5.5 | 650-800k | S labels/wording/tests; O lock refresh + review |
| 3 | **F-11 DV formatter** steps 1, 2, 4, 5 (no class-3 removal; vote by evidence; R10b/l class 1; extras struct last) | group D | 6-7 | 850-950k | S per-hunk table + parser/log moves; O hooks, extras struct, register |
| 4 | **WP-9.0** consumer adapter + cases 3,5,6,7,8 (+S1 factors) | 1 | 6-8 | 500-600k | O |
| 5 | **WP-16 intent** module + `control.intent` slot (no forwarders) | group D, slot id pre-assigned | 5-7 | 350-500k | S `git mv`+includes; O IntentHost + slot |
| 6 | **WP-10 A stage** (NetHost, keys renamed in slots.json, RepairEncodingStage, Config controls) | 4; after 2 for VpvSection | 6-8 | 400-500k | O; S settings ref/Config tests |
| 7 | **WP-9a** backward field + hidden `entrance_field`, mandatory independent audit | 4 (parallel with 5/6, separate worktree) | 7-10 | ~600k | O only |
| 8 | **WP-9b** cases 4+10 | 4 | 5-8 | 300-400k | O |
| 9 | **WP-9c** cases 2+1 | 8 | 7-10 | 300-400k | O |
| 10 | **WP-10 D** (new seat in AdaptiveDownscale::Blending, bypass reasons) + A_motion after 7 | 6, 7 | 4-6 | 200-300k | O |
| 11 | **M2 tail**: step 0 commit A, step 9 (NlsMaskMapping), step 7 (SdrAmbientLift) + Ambient* + TwoPlane420 -> modules, one build | 5 (ambient decision) | 3.5-4.5 | 400-500k | S script/moves; O README/key review |
| 12 | **WP-9.6** pre-B3 deletion | 0, 7-9 | 3-5 | ~300k | O |
| 13 | **B4 (a)-(c)** byte-identity GPU tests written (G7/GS/G8a/G10, G3, G8b/GT; G9 after 9) | 9 for G9 | 7-8 writing | ~900k | S test scaffolds; O comparator/harness hooks |
| 14 | **WP-18 + control.config** (key tables, golden fingerprints captured first) | 2, 5, 6, 10, 11, 12 (key freeze) | 7-9 | 700-900k | S table transcription; O design, side-effect keys, audit of (b) |
| 15 | **Step 0 commit B** (Bill-file include patch, delete 46 forwarders, regenerable) | 14 | 1.5 | ~120k | S; O diff review of 16 Bill files |
| 16 | **WP-19 sweep + fixes** for the above | all | (master plan 6-10) | | O |
| -- | **WP-20 big test** (runs B4 tests, frozen compare, 9a grain gates, nets GPU, DV suite) | 0-16 | 10-14 | | O |
| 17 | Remove temporary `entrance_field`, FAS old decodes (master plan) | WP-20 pass | 1 | 80k | S + O check |
| 18 | **B1 + G6**: VP tests out of Bill test files, one category scheme, filter generator | WP-20 | 5-6 | 400-500k | S moves/attrs; O filter/trait experiment |
| 19 | **B3** LED once per source frame (replay variant), LED correlation harness | WP-20 | 3 | 450k | O |
| 20 | B4 G9 run, B3/B1 GPU legs in one GPU slot | 18, 19 | 1-2 | 100k | O |

Before WP-20 (items 0-16): about **80-100 agent hours, ~8.0-9.5M tokens** (incl. ~12 audits ~1.5M),
critical path S1 -> WP-9.0 -> 9a -> 9b -> 9c -> 9.6 -> WP-18 -> step 0B -> sweep (~45-55 h).
After WP-20, before WP-21 (items 17-20): **10-12 h, ~1.0-1.1M tokens.** B3 and B1/G6 deliberately follow
the big test: B3 is a default-off cost saving on hardware-visible LED code; G6/B1 are test-only and
re-verifiable without a second big test.
Parallel lanes (two worktrees, builds max two at once): lane A = 1 -> 4 -> 7/8/9 -> 12; lane B = 2 -> 3 ->
5 -> 6 -> 11 -> 13; then 10, 14, 15 serial.

## 5. Owner questions (plain language, with recommendation)

1. **Rare motion combination (WP-9 case 10).** One non-default combination (warping the raw source picture
   with gap filling on) needs extra work to run through the new single path. Build it properly (2-3 h), or
   have VP refuse that combination with an on-screen notice? **Recommend: build it** (fidelity first).
2. **Resolution change crossfade (WP-9 case 1).** When the source resolution changes (a few times a film,
   ~1 s), keeping motion smoothing on during the crossfade means running two processed pictures briefly.
   Do that, or pause smoothing for that second? **Recommend: keep smoothing on** (two pictures).
3. **Subtitle moving.** When VP moves subtitles, the clean-up stages will now work on the picture *before*
   the subtitle is placed (cleaner, and what the nets learn from). Tiny pixel differences near subtitles
   compared with today. Accept? **Recommend: yes.**
4. **Compression-repair net order.** The repair net was trained on the picture before resizing; today VP
   resizes first, so the net would stay idle (with a visible "bypassed" note). Keep today's order for this
   batch and decide with the next training round? **Recommend: yes, keep order; decide with the kit.**
5. **Your settings file as a test reference.** May we copy the cinema PC's VideoProcessor settings file
   (no passwords in it) into the test folder, to prove the settings rework changes nothing for your exact
   setup? **Recommend: yes.**
6. **"Approve" on the retirement page.** Should Approve only record your decision (nothing disappears until
   a later clean-up run), rather than hide the controls immediately? **Recommend: record only.**
7. **Dynamic Drive main page.** Main page shows only: on/off, command source (script/compensate/none),
   LED profile, speed preset, projector link; everything else under Advanced, and every greyed control says
   why. OK? **Recommend: yes.**
8. **Top quality tier not built yet.** Show the "Upper" step on the slider marked "planned - uses Mid for
   now", rather than hiding it? **Recommend: show it** (no hidden substitutions).

Decided by rule/evidence (not for the owner): intent slot (yes), DV restore switch (keep, Original =
Bill's), R10b/R10l (bug fix, keep), packet vote (evidence decides), forwarder deletion (regenerable patch),
step 7/9 shrink, step 19 moves (forbidden), ambient files location (colour.appearance), WARP tests stay in
the CPU run, PRs to Bill (owner approves each later, D7).
