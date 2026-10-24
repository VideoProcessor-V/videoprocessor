# Output segments S2/S3: segment runner, call sites and patches (DRAFT 2026-10-24)

Status: **draft code, not integrated.** Branch `feat/segment-runner` from `feat/skeleton`
d0212534. No GPU test has been run (written only). Read with
`central-migration-needs.md` section 6 and `efficiency-audit-2026-10-24.md` findings #1 and #5.

## Commits: upstreamable vs VP-only

| # | Commit | Kind | Depends on | Output |
|---|---|---|---|---|
| A | `vprenderer/HookSegment*`: run a run of libplacebo hooks as ONE hook with internal hand-offs, its own texture pool, 1-in-N GPU timing; `HookSegmentTests` | **upstreamable** (libplacebo only, no VP-V include, written in the vprenderer style; nothing calls it yet) | - | unchanged |
| B | `VP-V/core/SegmentRunner`, `VP-V/segments/OutputSegments` (S2/S3 separate form), CPU tests, GPU fixture, this note, `segments-callsites.patch` | **VP-only** (pipeline slots, contracts, "Fallbacks engaged", VP's modules) | A (GPU fixture only) | **byte-identical** once the patch is applied |
| C | `OutputSegmentsMerge` (merged form behind `output_segments_merged`, default off), `segments-merge-callsites.patch` | **VP-only** (uses A) | A, B | switch off: unchanged; on: tolerance gate |

Patch files (none applied; each checked with `git apply --check --cached`):

| Patch | Kind | Applies to | Contents |
|---|---|---|---|
| `segments-upstream.patch` | upstreamable | d0212534 | project entries for `HookSegment.cpp` (renderer, renderer Beta, tests) and `HookSegmentTests.cpp` |
| `segments-callsites.patch` | VP-only | d0212534 + the upstream patch | `LibplaceboVideoRenderer.cpp` call sites for S2/S3; project entries |
| `segments-merge-callsites.patch` | VP-only | the two above | the switch, the merger, one `Scope` around the render |

To offer A to Bill: commit A plus `segments-upstream.patch` are self-contained. A generic host
hook point for him would be one `HookSegment::Runner::Fuse` call on the list handed to
`pl_render_image`, with a membership rule of his choice (for example "hooks the renderer owns").
The VP membership rule (`Merger`) is not needed for that.

## What the segments contain (from RenderLocked today, d0212534 ~23950-24800)

- **S2 output colour + drive (PRE_OUTPUT):** SDR tone shape -> DV trims (same slot switch) ->
  [Stage 13: placeholder, no hook] -> LED measurement -> LED compensation (gain, gamma boost,
  shadow lift, veil, BTB/WTW passthrough) -> [black level: placeholder] -> | user shaders (the
  edge: they stay exactly where they are and split the segment) | -> calibration LUT (VP stage).
- **S3 output finish (OUTPUT):** encoding repair (libplacebo 7.360 workaround, first at OUTPUT)
  -> occlusion mask -> sigma-delta (always last).

**Deviation from the brief:** the calibration LUT stays the last stage of S2, not the first of
S3. It must act on target light before libplacebo's output encoding, which Beta 377 applies
between PRE_OUTPUT and OUTPUT, and the de-judder rendered-picture cache (a PRE_OUTPUT hook after
the LUT) keeps the LUT-mapped picture for the extras. The LUT can move into S3 only once VP owns
the encoding step (section 6.2). The repair IS folded into S3 (merged form).

Not in a segment (host / Bill's or other stages): NLS, ExtendedRange restores and carry, adaptive
downscale measure/blend, the de-judder cache, user shaders.

## Commit B: the separate form (byte-identical)

`segments-callsites.patch` replaces, in `RenderLocked`:

| Today | After |
|---|---|
| tone shape: `Collect` + `TagSlotHooks` + 5 parameters | decision fills `colourFrame.tone` |
| DV trims: `Collect` + 10 parameters, state from the hook count | decision fills `colourFrame.dv`; state/log/fallback after the segment (`finishDvTrims`) |
| LED measure push + compensation `Collect` + 11 parameters | `colourFrame.ledMeasure/ledCompensation/compensation` |
| `customHooks.Collect` + LUT push | user hooks collected into `userFrameHooks` (the edge); **one call** `outputColour.Collect(colourFrame, frameHooks, TagSlotHooks)` |
| sigma-delta domain loop, dither-last reorder, mask parameter loops, mask insert, repair insert | `outputFinish.PrepareDither / KeepDitherLast / MarkMask / InsertMask / Repair` at the same four places |

S2 is one call. S3 stays four steps in the separate form, because the host places NLS, the
ExtendedRange restores and the de-judder cache between them; the merged form gives S3 its one exit.

Why byte-identical: the adapters append the same hook pointers in the same order and set the
same parameters with the same values (`SegmentRunnerTests.S2SeparateFormIsTodaysHookListTagsAndParameters`
compares against a copy of today's code for all 128 stage combinations; `S3SeparateStepsAreTodaysLoops`
likewise). Only log lines move (the SDR tone shape and DV trims logs are written after the
segment collected). The GPU fixture (`OutputSegmentsGpuTests`, not run) renders both lists and
expects 0 codes.

## Commit C: the merged form (switch `output_segments_merged`, default off)

- At the render (inside `NlsHookRecovery::RenderOwned`'s draw callback) a `Merger::Scope` swaps
  the hooks handed to libplacebo for a merged list and restores them afterwards. All host code
  that finds hooks by pointer (extras filter, repair re-check, sigma-delta vector guard) keeps the
  unmerged list. Parameters are all set before the merge.
- `HookSegment::Runner::Fuse` replaces each run of two or more adjacent segment hooks at one
  stage by one hook. Its callback calls the original hooks in order; each gets libplacebo's own
  `pl_hook_params` with the previous hook's output as input. Texture outputs come from the
  segment's pool (the same rgba32f/rgba16f format as libplacebo's FBOs). No maths changes.
- With the designed chain: S2 = 1 PRE_OUTPUT hook (5 today), S3 = 1 OUTPUT hook (3 today).
  A user PRE_OUTPUT shader splits S2 into two hooks (tone..compensation, then the LUT).
- A stage that fails inside the merged hook is skipped from then on, the segment drops to the
  separate form until the next settings change, and "Fallbacks engaged: output segment S2/S3
  (separate hooks: ...)" says so. A failed contract or order check does the same.
- Timing: `SetSampleEvery(30)`; per stage, only shader-returning stages are timed by the runner
  (texture-returning hooks dispatch inside themselves). Polled, never waited on.

**Tolerance gate (must pass before the switch may default on):** at most 1 code at the output bit
depth vs the separate form; BTB (code 4) and WTW (code 1019) probes unchanged; sigma-delta
statistics unchanged; whole-chain matrix and owner acceptance. Expected 0 (only pass boundaries
change; intermediates keep libplacebo's format). Then measure the saving with `SlotTiming`
before/after (audit #1 estimates ~1-1.5 ms per refresh on the 3090; do not assume it).

## Known gaps / follow-ups

- De-judder extras (`PresentDejudderExtras`) still use the separate form; add a `Scope` there
  after the main path passes the gate.
- The setting has no Config control yet (owner rule: every setting gets one) and is not in the
  settings reference; add with integration.
- Per-stage timing into `SlotTiming` is wired in the core, not yet from `Merger` (`LastNs` exists).
- A stage disabled inside the merged hook stays disabled after a settings change re-merges.
- Fusing stages into fewer passes (per-pixel stages in one shader) is the next step once the
  segment owns its intermediates; not attempted.

## Verification done

- Build: `VideoProcessor-Test` and `VideoProcessor-VPRenderer` (x64 Release) through
  `vp-build-slots-run.sh` with ALL three patches applied in the work tree (then reverted):
  0 errors, no new warnings (`segments-build3.log`).
- CPU tests, single-threaded (vstest, trx): SegmentRunnerTests (12), HookSegmentTests (4), plus
  PipelineSkeletonTests and CustomHookChainTests as neighbours: 54/54 passed.
- Patches: each `git apply --check --cached` clean against d0212534, in order
  upstream -> callsites -> merge-callsites.
- NOT done: any GPU test (OutputSegmentsGpuTests written only), renderer harness, chain gate,
  timing measurement, Beta (377) project build.
