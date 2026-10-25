# Output segments S2/S3: segment runner, call sites and patches (DRAFT 2026-10-24)

Status: **draft code, not integrated.** Branch `feat/segment-runner`, rebased 2026-10-25 onto
`integ/batch1` 38badcb0 (silent-fallbacks + modularity fixes), with the two audits' HIGH/MED findings fixed
(skeleton-notes/audit-feat-segment-runner*.md). No GPU test has been run (written only). The
fixes are not yet build-verified (audit first, then the verification build). Read with
`central-migration-needs.md` section 6 and `efficiency-audit-2026-10-24.md` findings #1 and #5.

## Commits: upstreamable vs VP-only

| # | Commit | Kind | Depends on | Output |
|---|---|---|---|---|
| A (+ A2 timing hand-over, A3 audit fixes) | `vprenderer/HookSegment*`: run a run of libplacebo hooks as ONE hook with internal hand-offs, its own texture pool, 1-in-N GPU timing; `HookSegmentTests` | **upstreamable** (libplacebo only, no VP-V include, written in the vprenderer style; nothing calls it yet) | - | unchanged |
| B | `VP-V/core/SegmentRunner`, `VP-V/segments/OutputSegments` (S2/S3 separate form), CPU tests, GPU fixture, this note, `segments-callsites.patch` | **VP-only** (pipeline slots, contracts, "Fallbacks engaged", VP's modules) | A (GPU fixture only) | **byte-identical** once the patch is applied |
| C | `OutputSegmentsMerge` (merged form behind `output_segments_merged`, default off), `segments-merge-callsites.patch` | **VP-only** (uses A) | A, B | switch off: unchanged; on: tolerance gate |

Patch files (none applied; each checked with `git apply --check --cached`):

| Patch | Kind | Applies to | Contents |
|---|---|---|---|
| `segments-upstream.patch` | upstreamable | 38badcb0 | project entries for `HookSegment.cpp` (renderer, renderer Beta, tests) and `HookSegmentTests.cpp` |
| `segments-callsites.patch` | VP-only | 38badcb0 + the upstream patch | `LibplaceboVideoRenderer.cpp` call sites for S2/S3 (feat/dv-l8's DV trims block included); project and filters entries; `SlotRegistry.h`: `output.dither` switch honoured (true only together with these call sites) |
| `segments-merge-callsites.patch` | VP-only | the two above | the switch, the merger, a `Scope` around the main draw and each extra, `Report` feeding the host's disabled-hook handling |

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

## Owner rules (2026-10-25)

- **Every S2/S3 slot is switchable** (`pipeline_slots_off`, Original pathway; off = no hook,
  byte-identical to the slot not existing): S2's `Collect` checks `registry.Runs()` for
  `colour.sdr_tone_shape` (with the DV trims), `drive.led_measure`, `drive.led_compensation`,
  `output.calibration_lut`; S3's `PrepareDither` drops the sigma-delta when `output.dither` is off
  (libplacebo's own dither then runs). Tests: `EachS2SlotSwitchesOffAndLeavesTheRestUnchanged`,
  `DitherSlotOffRemovesTheSigmaDelta`, `OriginalPathwayAndPlaceholdersAreByteIdentical`.
- **Order comes from the registry** (validated by `OrderValidator` against `SlotContracts`), not
  from code: S2 emits its stages sorted by `registry.Order()`; the user shaders stay just before
  the LUT (R1). Equal to today's `OrderFrameSlotHooks` result for every valid order, except that
  the DV trims stay with their slot (today's untagged trims hook would be stranded between the
  LED measurement and compensation, against rule 3b). Test: `S2FollowsAValidatedCustomOrder`.
- **Placeholders stay placeholders:** Stage 13 and black level have no component, never run,
  add no hook and cost nothing (test above).
- **S4 (POST_KERNEL, hybrid scaler / D) stays reserved:** the merge touches only hooks the S2/S3
  segments emitted; POST_KERNEL hooks are never members.

## Silent-fallbacks integration (38badcb0)

- The SDR tone shape's `SyncFallback` stays in its logging block (now run after S2 collected, so
  "the shader did not load" is known).
- The DV trims are tagged with `colour.sdr_tone_shape` by S2, as the host does since 38badcb0.
- `PrepareDither` returns `DitherPrep {found, domainDropped, slotOff}`: `FallbackCatalog::Dither`
  engages for an unsupported domain or a shader problem, never for `output.dither` switched off
  (`DitherFallbackRequested`; test `DitherFallbackOnlyForDomainOrShaderNotForSlotOff`).
- A placeholder slot (Stage 13, black level) with a registered component but no S2 stage is
  counted (`ColourCounts::unhandled`) and listed as "S2 placeholder stage", never skipped silently.
- The callsites patch flips `SwitchHonoured(output.dither)` together with the call sites and
  records it in PipelineSkeletonTests, slots.json (tests) and the regenerated slot matrix.
- The merge patch adds `output_segments_merged` (validator, Original pathway = off, slots.json,
  manifest); `Merger::Configure` also never merges in the Original pathway.

## Core API changes for S1 (m2-entrance)

- `StageInfo::gives` now defaults to all zero (a zero field keeps the input's value). Set the
  fields a stage really changes.
- `Segment::Configure` no longer engages a fallback when the merged form is not wanted; new
  `Rechecked()` and `FallbackChanged()`. `Downgrade` holds until the next settings change.
- `Segment::Active` orders slot stages by the registry (a validated custom order is accepted).
- Unchanged: `StageContract`, `Flow`, `CheckOrder`, `CheckContracts`, `TimingSampler`. S1 (no
  libplacebo hooks) uses the check functions directly, not `Segment::Configure`.

## Commit B: the separate form (byte-identical)

`segments-callsites.patch` replaces, in `RenderLocked`:

| Today | After |
|---|---|
| tone shape: `Collect` + `TagSlotHooks` + 5 parameters | decision fills `colourFrame.tone` |
| DV trims: `Collect` + 10 parameters, state from the hook count | decision fills `colourFrame.dv`; state/log/fallback after the segment (`finishDvTrims`) |
| LED measure push + compensation `Collect` + 11 parameters | `colourFrame.ledMeasure/ledCompensation/compensation` |
| `customHooks.Collect` + LUT push | user hooks collected into `userFrameHooks` (the edge); **one call** `outputColour.Collect(colourFrame, pipelineSlots, frameHooks, TagSlotHooks)` |
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
  `pl_hook_params` with the previous hook's output as input. Intermediates take the format of the
  texture libplacebo hands in (audit M1); pools are trimmed on size changes (M3). No maths changes.
- **No saving today (audit M2):** every S2/S3 stage takes and returns a texture, which libplacebo
  already chains without extra passes. The merged form removes no pass and costs a second set of
  intermediates. It stays OFF and is not put through the tolerance gate until stages are fused
  into fewer shaders (shader-returning stages chained into one dispatch).
- With the designed chain: S2 = 1 PRE_OUTPUT hook (5 today), S3 = 1 OUTPUT hook (3 today).
  A user PRE_OUTPUT shader splits S2 into two hooks (tone..compensation, then the LUT).
- A stage that fails inside the merged hook leaves the merge at once and runs separately AT ITS
  OWN PLACE (never reordered, audit H1); the segment drops to the separate form until the next
  settings change ("Fallbacks engaged: output segment S2 (separate hooks: drive.led_measure:
  hook failed)"), and `Report` hands its signature to the host's disabled-hook handling (M4).
  A merged hook libplacebo disabled downgrades its segment the same way (H2). A failed contract or
  order check does the same while the merged form is wanted; with it off nothing is engaged (L4).
- Timing: `SetSampleEvery(30)`; per stage, only shader-returning stages are timed by the runner
  (texture-returning hooks dispatch inside themselves). Polled, never waited on.

**Tolerance gate (must pass before the switch may default on):** at most 1 code at the output bit
depth vs the separate form; BTB (code 4) and WTW (code 1019) probes unchanged; sigma-delta
statistics unchanged; whole-chain matrix and owner acceptance. Expected 0 (only pass boundaries
change; intermediates keep libplacebo's format). Then measure the saving with `SlotTiming`
before/after (the earlier ~1-1.5 ms estimate does not apply to texture-in/out hooks, see M2).

## Extras, timing and the setting (added 15:30)

- **De-judder extras** draw through the same segment objects: `PresentDejudderExtras` wraps its
  per-extra parameters (`timedParams`) in a `Merger::Scope`, so with the switch on each extra
  gets the same merged S2/S3 hooks (the cached-picture path gets the merged S3 only, since it
  draws OUTPUT hooks alone). The pointer filters before it (LED measurement, mask, cache,
  repair re-check) still work on the unmerged list. The fallback draw of A keeps the separate list.
- **Timing:** `HookSegment::Runner::TakeTimings` hands over the timer results that arrived;
  `Merger::Report` records them per slot in each segment's `TimingSampler` (`S2Timing()`,
  `S3Timing()`; S3's repair and mask go under `output.dither`). Limitation: only stages that
  return a shader are timed by the runner. VP's S2/S3 hooks all return textures (they dispatch
  inside themselves), so their per-stage time must come from libplacebo's pass information
  (`info_callback`, as `DitherExtraPassCallback` does); feeding that into the same samplers is
  the follow-up. The extras' `info_callback` timing of the sigma-delta is unaffected.
- **Setting key for the settings branch (no Config control added here):** `output_segments_merged`
  = `on` / `off` (profile toggle, default `off`; owner: becomes the default once the GPU fixture
  shows only removed-rounding differences with BTB/WTW unchanged). Unit: output; tier: advanced.

## Known gaps / follow-ups

- Per-stage GPU time for texture-returning stages via `info_callback` (see above).
- A merged S3 GPU case with the real sigma-delta (statistics, dither last) is still to be written.
- Fusing per-pixel stages into fewer passes is the next step; not attempted.

## Verification done

- 2026-10-24 (before the audit fixes): build of the test and renderer projects with all three
  patches applied, 0 errors; CPU tests 54/54.
- 2026-10-25 (audit fixes): NOT build-verified yet (re-audit first). Patches regenerated:
  `git apply --check --cached` clean against 38badcb0 in order upstream -> callsites ->
  merge-callsites; drift check and slot matrix clean with all three applied.
- NOT done: any GPU test (OutputSegmentsGpuTests written only), renderer harness, chain gate,
  timing measurement, Beta (377) project build.
