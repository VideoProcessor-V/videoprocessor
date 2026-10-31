# Source identity (`input.source_analysis (identity/)`)

Unit: **input** | order: 45 | status: **draft** | header-only, not wired into the renderer

## Purpose

Today the classical source-resolution monitor (`input.source_analysis`) decides alone what the delivered source is. This module combines several opinions (voters) into ONE hysteretic answer per field: horizontal and vertical delivered ratio, the player's upscale kernel, the codec family and a quality tier. It also finds adaptive-bitrate (ABR) rendition switches and tells the user once per title when the stream quality dropped. Every file is pure standard-library C++14: no clock, no threads, no Windows. Time is always SOURCE time passed in by the caller.

## Files

- `SourceIdVote.h`: vote types, `Hypothesis`, and the voter adapters. They take plain structs the call site fills from the monitor's snapshot (`ClassicalReading`, `NetReading`), so nothing here includes the monitor.
- `SourceIdConsensus.h`: `Consensus`, `Settings`, `Result`, `Stats`, and `IndicatorPort`.
- `AbrStepDetector.h`: ABR boundary timing from per-frame quality samples.
- `AbrNoticePolicy.h`: the one-time notice and its wording.

## Voters and trust

| Voter | Fields | Notes |
|---|---|---|
| ClassicalResolution | AxisH, AxisV | Uniform declared ratio when it is not native, else the per-axis readings. Only voter allowed a safety veto. |
| ClassicalKernel | Kernel | Confidence = agreeing fraction, ramped up over the first fitted votes. |
| BitrateQuality | Quality | Tier from the bitrate reading through `qualityTierEdges`; indirect evidence, trust 0.5. |
| IdNet | all | Known trust profile held as settings: axes trusted, kernel only at confidence 0.9 or more, codec capped at 0.25, bitrate-tier head ignored (trust 0) until re-validated. |

Each vote carries a confidence per field; weight = confidence x voter trust. The net is event driven, so its maximum evidence age is long (30 s) while the classical voters expire after a few seconds. Stale votes are ignored and reported (`held: stale`), never used.

## Consensus and hysteresis

Votes sit in slots indexed by `VoterId`, so the order of `Submit` calls never matters and every sum runs in a fixed order. Per field, `Evaluate(seq, timeUs)`:

1. Groups fresh votes by equal value (ratios within `ratioTolerance`, others exactly).
2. Takes the heaviest group as the candidate (ties favour the current value, then the lower voter id).
3. If it equals the current value: refresh. Otherwise the change needs support at least `enterSupport` (0.67), weight at least `enterMinWeight`, enough distinct voters (1 when classical votes alone; 2 when two or more voters are fresh), the candidate to persist for `dwellUs` (3 s), and `minHoldAfterChangeUs` (10 s) since the field last changed.
4. Anything else holds the current value with a reason: disagreement, weak evidence, too few voters, dwell, min hold, stale.

A pending candidate is kept while its dissent stays at or below `exitDissent`, a lower bar than entering (hysteresis). A safety veto (detail above the re-based size) sets the voted axes native at once, bypassing dwell and hold; votes from any other voter cannot veto.

A pending candidate's dwell restarts when it is held for disagreement (intentional: only a steady candidate latches).

After an ABR boundary (`NoteAbrBoundary`) the dwell is shortened to `abrDwellUs` for `abrWindowUs`, because a rendition switch is sudden.

## ABR step detector

Per analysed frame the call site pushes a `QualitySample` (quality, live delivered ratio, mean luma, blank/cut/fade flags). The detector compares the median quality of a post window with a pre window and needs a step of at least `minStep`, both windows stable (median absolute deviation), enough valid samples, and the new level persisting for `confirmUs`. Blank and fade samples, and samples darker than `darkApl`, are excluded as evidence either way. A step on a cut is a confound: it is emitted only when the delivered ratio stepped with it or it lies on the measured segment grid; otherwise it is counted (`Confounded()`) and dropped. The segment period is MEASURED from two agreeing intervals between accepted steps; `segmentMinUs`/`segmentMaxUs` only judge plausibility. An event carries direction, magnitude, ratio step, confidence, `onGrid` and `cutConfound`.

AV1, VVC and LCEVC switch behaviour (an LCEVC enhancement layer can drop independently of its base, giving a quality step without a resolution step) must be measured on real material; the detector holds no codec-specific constants.

## Notice policy

`NoticePolicy::Consider(event, consensusResult, text)` fires only when notices are enabled, the event is a Drop of enough confidence, the consensus has LATCHED the lower state (quality tier and/or axis ratio went down in that Result, so the drop outlived the dwell: a flicker never latches), and it has not fired for this title key. `NewTitle(key)` re-arms; the call site passes the Indicator's FULL/era counter. The wording never claims an unmeasured cause:

> Stream quality dropped: the player switched to a lower-quality stream. Restarting playback on the player usually brings full quality back.

With measured heights the clause "(about 2160p to 1080p)" is added after "stream". `NoticeDecision` returns a `FallbackCatalog::Decision` with the stable id `abr_quality_drop`.

## Indicator interaction

The central Indicator (resync design 25 A2/A4, package I0) does not exist yet. `IndicatorPort::AdvanceRebase(axis, fromSeq, cause)` is the ONLY way consensus moves a re-base stamp: once per changed axis (0 = H, 1 = V), with cause `ratio`, `kernel` or `veto`. A kernel change re-bases every axis that is currently re-based; a native axis has no kernel to undo. Consensus keeps no private epochs or flags. `Discontinuity(seq)` (FULL: new title or source) returns everything to native/unknown, drops the votes, and emits no stamp, because the Indicator's FULL already bumps both axes.

## Latch and crossfade

Consensus decides WHAT the source is. The source-resolution latch (cut, black, pan, crossfade in source time) still decides WHEN the switch is shown.

## Determinism

Fixed voter slots and group order, no unordered containers, no clock, no random source. Tests compare serialised traces and shuffled submission orders.

## Config keys (draft, NOT wired)

- `source_id_consensus` = off | on. Default off until measured. Off is byte-identical to today because nothing is wired.
- `source_id_abr_notice` = on | off.

## Renderer call sites needed (not wired)

1. SRM worker: push one `QualitySample` per analysed frame into `AbrStepDetector` (the worker owns it; the event reaches the render thread through the snapshot).
2. LPR `AutoFactors` site (about LPR:22598): build a `ClassicalReading` from the snapshot, `Submit` the classical votes, `Evaluate(seq, source time)`, and gate the auto engage target on the result.
3. Indicator I0 implements `IndicatorPort`.
4. FULL/era events call `Consensus::Discontinuity` and `NoticePolicy::NewTitle`.
5. A fired notice goes to the FallbackStatus registry and the GUI's one-time notice.
6. Ctrl+I line and the Ctrl+Alt+D dump print `Stats` and the per-field reasons.
7. SRM history CSV gets the consensus columns.
8. Later: the ID-net runtime calls `IdNetVote` on its events. The net is event driven, so the call site must trigger it on an ABR event or a consensus candidate; otherwise its old, disagreeing vote holds a real switch until its maximum evidence age.
9. `BitrateQualityVote` reads the SMOOTHED `Snapshot.bitrateQuality` and therefore lags. The detector needs FRAME-level quality, which needs a new Snapshot field (the SRM worker must publish it).

Fallback entry when consensus is off or unavailable: "Source identity: classical only (<reason>)". The measurement plan is in `MEASUREMENT-PLAN.md`.
