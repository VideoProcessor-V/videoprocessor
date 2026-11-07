# Source identity (`input.source_analysis (identity/)`)

Unit: **input** | order: 45 | status: **wired behind `source_id_consensus` (default OFF)** | header-only; the monitor worker runs it, nothing else changes while it is off

## Purpose

Today the classical source-resolution monitor (`input.source_analysis`) decides alone what the delivered source is. This module combines several opinions (voters) into ONE hysteretic answer per field: horizontal and vertical delivered ratio, the player's upscale kernel, the codec family and a quality tier. It also finds adaptive-bitrate (ABR) rendition switches and tells the user once per title when the stream quality dropped. Every file is pure standard-library C++14: no clock, no threads, no Windows. Time is always SOURCE time passed in by the caller.

## Files

- `SourceIdVote.h`: vote types, `Hypothesis`, and the voter adapters. They take plain structs the call site fills from the monitor's snapshot (`ClassicalReading`, `NetReading`), so nothing here includes the monitor.
- `SourceIdConsensus.h`: `Consensus`, `Settings`, `Result`, `Stats`, and `IndicatorPort`.
- `AbrStepDetector.h`: ABR boundary timing from per-frame quality samples.
- `AbrNoticePolicy.h`: the one-time notice and its wording.
- `SourceIdProduct.h`, `FrameQuality.h`, `SourceIdPipeline.h`, `SourceQualityView.h`: the P14 record and holder, the frame-level quality signal, the worker pipeline and the consumer read API (sections below).
- `SourceIdReplay.h`: replays a source-resolution history CSV through the pipeline (section "History CSV and replay").

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

A pending candidate is kept while its dissent stays at or below `exitDissent`, a lower bar than entering (hysteresis). A safety veto (detail above the re-based size) sets the voted axes native with no dwell of its own; votes from any other voter cannot veto. SID-B (doc 78 D12): the call site raises the veto only from the classical trackers' SUSTAINED-native state (`Tracker::SustainedNative()`: 3 native readings over 1.5 s), never from one reading, and the veto obeys T_min like every other change.

**T_min.** A field that changed cannot change again within `T_min = max(2 x dwellUs, minHoldAfterChangeUs)` (derived by `Settings::MinTimeBetweenChangesUs()`, no constant of its own; 10 s with the defaults), whatever moved it (a candidate or a veto), except through `Discontinuity()` or a **regime boundary towards native**: the contrary hypothesis (an axis rising, or the veto) has persisted for at least `regimeEvaluations` (3) evaluations after a cut / blank / fade flag (`NoteRegimeFlag`, fed by the pipeline from the QualitySample flags) that came after the field's last change and no more than one dwell before the hypothesis appeared. The boundary bypasses T_min only, never the dwell; towards a LOWER ratio nothing bypasses T_min. A held change reads `held: min hold` (`Stats::tMinHolds`); a bypass is counted in `Stats::regimeBypasses`. The monitor feeds the `cut` and `blank` (no informative tile) flags today; no fade source is fed yet (the `fade` flag stays 0).

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

## P14, the source identity product (doc 61 section 2.1, D-SID1)

`Products.h` declares `kSourceIdentity` (`p14_source_identity`, CPU, T0, 256 B of system RAM, unit version `SourceIdentity::kUnitVersion` = 1, catalog expectation `kExpectedSourceIdentityUnit`). It is appended last, so every earlier id is unchanged. It is a **HELD-STATE** product with **no live fill**: the consensus is hysteretic (3 s dwell), so recomputing it on the render thread would differ from the worker's answer. Its consumers read it at **lag 1** in the catalog (the latest sealed record with seq <= N), so no consumer can ever be its live-fill owner (a catalog test pins this).

- `SourceIdProduct.h`: the `Record` (axes, kernel, codec class, tier, latched quality and the event-anchored level, per-field reason/confidence/latch flags, the newest ABR event, the notice serial, `rebaseGen[2]`, `consensusGen`, `maxAgeUs`), its fixed little-endian byte codec (`Pack`/`Unpack`, 256 B, another unit version is refused), `HeldState` (the publish/read holder: an 8-record ring, `ReadLatest(seq)` returns the record that was current for an earlier frame) and `RebaseApplier` (render side: each rebase generation reaches the Indicator port exactly once; a new title or discontinuity restarts the generations).
- Stale: `maxAgeUs` is derived from the settings (twice the longest classical vote expiry), never fixed. A stale, absent, unlatched or other-version record means every consumer uses the classical value, visibly (below).
- The graph/doc view: `docs/design/product-graph.md` is regenerated by `ProductCatalogTests` (`VP_WRITE_PRODUCT_GRAPH_DOC=1`).

## Frame-level quality (the signal the ABR detector needed)

`FrameQuality.h` (ONE unit, CPU reference; the worker and the offline evaluation call the same code): `quality = min(delivered resolution term, noise term)`.

- Resolution term: the frame's delivered/native ratio. A rung with fewer lines is upscaled by the player, so a resolution rung drop is a step here.
- Noise term: the flat-area residual noise of the informative tiles against the stream's OWN recent upper level (80th percentile of its last ~60 s). Relative, never an absolute threshold (the NR metrics are weak: tools/eval/findings-streaming.md). A lower rate denoises, so the term falls. A picture with no noise to lose (clean animation, a clean master) abstains: this signal cannot see a rate-only switch there and the detector then reports nothing (measured limit, tools/eval/source_id/RESULTS.md).
- Skeleton note: this is a robust residual-noise estimate for source QUALITY, not a grain layer and not a grain level the grain split owns (P10 is a GPU product sealed per frame; the monitor's tile worker has no lag-0 access to it). It keeps no frame history, only a window of scalar noise values. `tools/check_shared_units.py` may flag the words "noise sigma"; this is the written justification.

## The worker pipeline and the consensus quality

`SourceIdPipeline.h` (called by the monitor worker only): quality sample -> `AbrStepDetector` -> classical votes -> `Consensus::Evaluate` -> notice policy -> `HeldState::Publish`. The Indicator port is a `DeferredIndicatorPort`: rebase requests are recorded as per-axis generations in the record and applied by the render thread (`Monitor::ApplySourceIdRebase`), so the worker never writes the Indicator. The Quality field is the classical smoothed bitrate quality combined with an event-anchored stream LEVEL (1 until an ABR event; a confirmed Drop lowers it by the event's magnitude, a Rise raises it; reset at a discontinuity), so a rendition switch reaches the tier within one dwell (0.5 s after a boundary) and does not drift back while the stream stays low.

## The read API and the consumers (`SourceQualityView.h`)

`View(record, classicalSnapshot, consensusOn, nowUs)` is the ONE way a consumer asks for the source quality. Consensus off: exactly `Snapshot.bitrateQuality` and today's tier mapping (not a fallback). On with a fresh latched record: `quality = min(bitrateQuality, record.level)` and the latched tier. Otherwise the classical value with an engaged `FallbackCatalog` decision `product.source_quality` and the reason, recovering by itself on the next fresh record. `Monitor::QualityView()` builds it from the monitor's own snapshot and record; a consumer's change is one expression: `snap.bitrateQuality` -> `monitor.QualityView().quality`.

| Consumer | Catalog row | Reads | Change at its call site | Fallback wording |
|---|---|---|---|---|
| dither need | `dither need (source quality)` | P14@1 | `DitherNeed(view)` | "dither need: classical quality" |
| deband | `debander (with hold)` | P14@1 | `view.quality` replaces `bitrateQuality` | "deband: classical quality" |
| contrast recovery | `contrast recovery need` | P14@1 | `ContrastRecoveryActive(view)`: off on a clean high-bitrate source | "contrast recovery: classical quality" |
| adaptive scaling | `adaptive downscale (grain map)` | P14@1 | the `bitrate` callback returns `view` | "adaptive scaling: classical quality" |
| detail pulse / texture restore | their rows | P14@1 | allowances via the `EntranceSegment` parameter assembly | "detail pulse / texture restore: classical quality" |
| status | - | P14 | `Monitor::SourceIdStatusLine()` | "Source identity: classical only (<reason>)" |
| temporal detail auto | - | - | moot (TD auto is dropped) | "TD auto: source quality unknown" |

The Bill-file call sites (LVR `snap.bitrateQuality` readers) are NOT edited here (VP-V files only); they are listed for P11 in the package report. The VP-V parameter structs they feed (`EntranceSegment`, `AdaptiveDownscaleStage`, `VpDebander::Strength`) already take the value as a plain number, so the swap is the single expression above.

## Auto engage (SourceResolutionMonitor, VP-V file)

With `source_id_consensus` on and a fresh record, `AutoFactor` / `AutoFactors` engage a lower re-base only for a LATCHED axis, at the consensus's ratio; the classical confidence minimum (`source_resolution_auto_min_confidence`) still applies (consensus can delay, veto or refine an engage, never add one), and the latch / crossfade still decide WHEN. A stale or absent record: the classical decision, with `Monitor::SourceIdGateFallback()` engaged. Off: the worker skips the pipeline and the noise measurement; every decision is unchanged (only a cheap reset on a discontinuity and an uncontended gate lock remain). The history CSV is written in every mode, with the replay columns (the `sid_*` columns empty when off); see "History CSV and replay". Consensus moves only TOWARDS native: the target is never deeper than the classical reading. The consensus safety veto is fed by the classical trackers: it fires when every tracker is back to native with nothing gathered below it on an informative frame. The SRM history CSV carries the `sid_*` columns while the pipeline runs (shadow or on) and the replay columns always.

## Config keys (module side)

- `source_id_consensus` = off | shadow | on (default **off**; shadow after phase A, doc 78 D16): `Monitor::SetSourceIdMode` (`SetSourceIdConsensus(bool)` = on / off). **Shadow** runs the pipeline and the consensus, publishes P14 (record flag `kFlagShadow`) and the CSV `sid_*` columns, logs one "source_id would-be: <field> <from> -> <to> (<reason>, support <s>)" line per consensus change (and a CSV `would_be` row), but `AutoFactor(s)`, `QualityView` and the re-base port use the classical decision, byte-identical to off. Status line "Source identity: shadow (classical decides)". The settings key plumbing (P3 settings mixin, validators, master-page row) is the SID-5 step and waits for P3 / the master page; until then the Monitor setters are the only entry.
- `source_id_abr_notice` = on | off: `Monitor::SetSourceIdNotice`.

## History CSV and replay (SID-B)

`SourceHistoryLog.h/.cpp` (in the parent folder; C++14, Win32 file calls; `Append` only queues rows in a bounded in-memory queue and never waits on the disk, the analysis worker flushes; a failed write or dropped rows are reported once and shown on the status line) writes the SRM history CSV: one session-long file family, appended; at 20 MB (`kDefaultRotateBytes`) it continues in `source-resolution <stamp>-002.csv`, `-003`, ... of the same session (each with the header and a "session continued" row; a row never straddles files); the newest 5 sessions are kept with all their files. Free-text notes are CSV-quoted. The columns after `note` are what the consensus needs: `mode, t_src, seq, frame_ratio_v, frame_ratio_h, detail_above, mixed, blank, cut, fade, apl, axis_x, axis_y, axis_conf, kernel_id, kernel, kernel_votes, kernel_agree, frame_quality, frame_noise, sid_tier, sid_level, sid_ratio_h, sid_ratio_v, sid_reason` (with the older `declared`, `confidence`, `bitrate_quality`, `frame_ratio`). `SourceIdReplay.h` (`SrmReplay::Run`) feeds a file (or the concatenated files of a session) through `Pipeline` in source time, using the recorded frame quality (`Pipeline::Push(in, &quality)`); the same CSV always gives the same trace (`SrmReplay::Trace`). `python -I tools/eval/source_id/replay.py --srm <file>` reads the same files (rotated sessions followed) and prints a SUMMARY (the replay itself is `SrmReplay`). Limits of a replay: CSV values are rounded (`%.4f` ratios, `%.6f` times and frame quality), so a replay can differ from the live run at a threshold edge; a file written with the pipeline off has no `frame_quality` (it defaults to 1.0) - replay files written in shadow or on. Tests: `SourceIdReplayTests` (a committed fixture, `tests/fixtures/srm-history-replay.csv`, and the writer).

## Still to do (call sites outside this module)

1. LVR (Bill file, P11): construct/refresh the setters from the settings; call `ApplySourceIdRebase(indicatorPort)` once per frame; swap the `snap.bitrateQuality` readers (LVR 27075 / 27141 / 27880) to `monitor.QualityView().quality`; show a fired notice (`TakeSourceIdNotice`) through the one-time FallbackStatus notice `abr_quality_drop`; print `SourceIdStatusLine()` on the Ctrl+I line and `Stats` in Ctrl+Alt+D.
2. The Indicator adapter for `IndicatorPort` (`Advance(kRebaseH/V, Cause::RebaseDecision, fromSeq)`) when I0 is wired in the renderer.
3. Later: the ID-net runtime calls `IdNetVote` on its events.

Fallback entry when consensus is off or unavailable: "Source identity: classical only (<reason>)". The measurement plan is in `MEASUREMENT-PLAN.md`; the streaming-suite evaluation in `tools/eval/source_id/` (`make_abr_reels.py`, `replay.py`, `RESULTS.md`).

## Settings

| Key | Section | Type | Range / values | Default | Config placement | Original (Bill) | Fallbacks / status | Tiers |
|---|---|---|---|---|---|---|---|---|
| `source_id_consensus` | input group (plumbing pending, SID-5: Monitor setter only) | enum | off, shadow, on | off (shadow after phase A, doc 78 D16) | not in Config yet (SID-5, P3 settings mixin) | forced off | "Source identity: off" / "Source identity: shadow (classical decides)" / "Source identity: classical only (<reason>)" / "Source identity: consensus (tier N, level L)"; FallbackCatalog `product.source_quality` | same on all |
| `source_id_abr_notice` | input group (plumbing pending, SID-5) | bool | on, off | on | not in Config yet (SID-5) | forced off | one-time notice `abr_quality_drop` | same on all |

## Tuning (not in Config)

| Name | Meaning | Default |
|---|---|---|
| `SourceIdentity::Settings::dwellUs` / `minHoldAfterChangeUs` | candidate dwell / hold after a change | 3 s / 10 s |
| `SourceIdentity::Settings::regimeEvaluations` | evaluations a contrary hypothesis must persist after a flagged boundary (it must start at or after the flag, within one dwell) | 3 |
| `SourceIdentity::Settings::MinTimeBetweenChangesUs` (T_min) | derived: max(2 x dwell, minHoldAfterChange) | 10 s |
| `Tracker::Settings::nativeEvidenceReadings` / `nativeEvidenceSeconds` | the veto source: sustained native only (see source_analysis/README.md) | 3 / 1.5 s |

## Environment and diagnostics

| Name | Purpose | Default | Scope (test/debug/runtime) |
|---|---|---|---|
| `VP_SOURCE_ID_EVAL_DIR` | reels folder of SourceIdStreamingEvalTests (then `tools/eval/source_id/replay.py --dir`) | unset (the test skips) | test |
| `VP_SOURCE_ID_EVAL_FFMPEG` / `VP_SOURCE_ID_EVAL_ONLY` | ffmpeg path / reel filter | D:\tools\ffmpeg-full\bin\ffmpeg.exe / all | test |
| `VP_WRITE_PRODUCT_GRAPH_DOC` | regenerates docs/design/product-graph.md in ProductCatalogTests | unset | test |
| history CSV `source-resolution <stamp>[-NNN].csv` | the replay input (`replay.py --srm`, `SrmReplay`) | on when the log folder is set | debug |

## Status and fallbacks

- Ctrl+I / Config line `Source identity: ...` (off, shadow, classical only (<reason>), consensus).
- Shadow: log line "source_id would-be: <field> <from> -> <to> (<reason>, support <s>)" per consensus change.
- Consumers fall back to the classical value, visibly (`product.source_quality`), whenever the record is absent, stale, unlatched or another version; in shadow they always use the classical value.
