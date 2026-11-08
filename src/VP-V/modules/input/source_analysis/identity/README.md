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
- `chain/`: the scaling-chain estimator (line sampler, polyphase proposer, descale verifier, chain evidence, power probe, the per-axis reading `ChainReading.h`, the worker `ChainEstimator`, governor, status texts and the evidence log); see `chain/README.md`.
- `SourceGeometryProduct.h`: the P14g `source_geometry` product: unit version, size bound, byte order, staleness rule, the record (per axis MEASURED and the CONFIDENT applied reading), its codec, the holder and the absent placeholder. `SourceGeometryView.h`: the one read API of the record's content (`Confident()` / `Provisional()`, shared unit U21).
- `SourceIdReplay.h`: replays a source-resolution history CSV through the pipeline (section "History CSV and replay").

## Voters and trust

| Voter | Fields | Notes |
|---|---|---|
| ClassicalResolution | AxisH, AxisV | Uniform declared ratio when it is not native, else the per-axis readings. Only voter allowed a safety veto. |
| ClassicalKernel | Kernel | Confidence = agreeing fraction, ramped up over the first fitted votes. |
| BitrateQuality | Quality | Tier from the bitrate reading through `qualityTierEdges`; indirect evidence, trust 0.5. |
| IdNet | all | Known trust profile held as settings: axes trusted, kernel only at confidence 0.9 or more, codec capped at 0.25, bitrate-tier head ignored (trust 0) until re-validated. |
| Chain | AxisH, AxisV, Kernel | The chain estimator's CONFIDENT reading, read from the latest sealed P14g record (`ChainReadingOf`, `ChainVote`): only axes it reads confident (above c1) are voted, the kernel only when verified and invertible; confidence = LLR / c1 capped at 1. An ordinary weighted voter, no authority; trust 0.5 is a conservative placeholder (it can only make the consensus abstain more) until the calibration run sets it. |

Each vote carries a confidence per field; weight = confidence x voter trust. The net is event driven, so its maximum evidence age is long (30 s) while the classical voters expire after a few seconds. Stale votes are ignored and reported (`held: stale`), never used.

## Consensus and hysteresis

Votes sit in slots indexed by `VoterId`, so the order of `Submit` calls never matters and every sum runs in a fixed order. Per field, `Evaluate(seq, timeUs)`:

1. Groups fresh votes by equal value (ratios within `ratioTolerance`, others exactly).
2. Takes the heaviest group as the candidate (ties favour the current value, then the lower voter id).
3. If it equals the current value: refresh. Otherwise the change needs support at least `enterSupport` (0.67), weight at least `enterMinWeight`, enough distinct voters (1 when classical votes alone; 2 when two or more voters are fresh), the candidate to persist for `dwellUs` (3 s), and `minHoldAfterChangeUs` (10 s) since the field last changed.
4. Anything else holds the current value with a reason: disagreement, weak evidence, too few voters, dwell, min hold, stale.

A pending candidate is kept while its dissent stays at or below `exitDissent`, a lower bar than entering (hysteresis). A safety veto (detail above the re-based size) sets the voted axes native at once, bypassing dwell and hold; votes from any other voter cannot veto.

A pending candidate's dwell restarts when it is held for disagreement (intentional: only a steady candidate latches).

The Chain voter's dwell counts DISTINCT informative evaluations (N4): the chain's vote joins the groups only after the same value was seen from `chainMinDistinctEvaluations` (2) different evaluations (`Vote::evidenceSeq` = the record's evaluation sequence), so a record that is read again for every analysed frame - an estimator that stopped, a held record - never satisfies the dwell by repetition; the consensus dwell in time then applies as for any voter. The chain's vote is judged fresh by the record's OWN maximum age (the writer's planned interval x 2), not by the consensus evidence age.

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

## P14g, the source geometry product (wave S2 reservation, doc 78 section 6)

`Products.h` declares `kSourceGeometry` (`source_geometry`, id 29, appended after the doc 72 list; CPU, T0, history 1, system RAM, declared 512 B,
unit version `SourceGeometry::kUnitVersion` = 1, catalog expectation `kExpectedSourceGeometryUnit`). Like P14 it is **HELD STATE with no live fill**: the
source-resolution side is the single writer and publishes on each evaluation; a consumer reads the latest SEALED record with seq <= N (a lag >= 1 read);
a record older than `maxAgeUs` (`SourceGeometry::MaxAgeUs`: twice the longest MEASURED evaluation interval, never a fixed rate, saturating; the age is exact
for any clock pair) is stale; a record that states no maximum age yet is "evaluation interval not measured yet" and is never used. Held state is ENFORCED
for P14 and P14g (`ProductDecl::heldState`: lag >= 1 readers only, no live pool). The layout is fixed little-endian of at most 512 B behind a fixed 8 B header
(u16 magic 'G' + unit version, u16 length, u32 reserved; `WriteHeader` / `ReadHeader` check it before any payload byte; `FieldBudgets()` is the doc 78 field
ceiling incl. the effective bandwidth per axis, 311 B + header, pinned by a test; `PutU16/32/64`, `GetU16/32/64`); its fields (per axis the
measured and the applied state, n, N, ratio, phase, kernel and parameters, sharpening, double scaling, confidence; the active area; the coded-raster
class; the reserved chroma chain; geometryGen / appliedGen) are laid out by the source-identity build inside that bound, which bumps the unit version and
the catalog expectation with any layout change. The record (SID train r2, unit v1, 371 B payload) is: per axis a MEASURED geometry (the diagnostic best hypothesis, whatever its strength) and an APPLIED geometry (the CONFIDENT reading, written only while the evidence is above c1 and the evaluation informative; an UNSURE axis applies nothing, state Unknown; not sticky); the states `Candidate` and `Unstable` are reserved numerics of the parked latch design and are never written (no unit bump). The writer is the chain estimator (`chain/ChainEstimator`), published after every evaluation; the ABSENT placeholder stays as the producer of a build without the estimator wired.

Provisional readers (catalog rows, all lag-1 held reads): quality_pattern and coding_evidence and temporal detail (one row of the temporal-detail family),
the adaptive scaler, the chroma stage, status. Each has an absent-tested read (`SourceGeometry::Read(consumer, entry, requested, frameSeq, nowUs)`): not
requested (`Requested(consensusOn)`: only source_id_consensus On asks) = byte-identical, nothing engaged, no string built; requested with the record
absent / stale / of another unit version / not yet sealed = the delivered raster and the reader's classical form, VISIBLY (`product.source_geometry`, the
reason names the consumer and the cause; `Combine()` is the one decision over every consumer so one reader's recovery cannot release another's entry).

| Reader | Falls back to | Status wording |
|---|---|---|
| quality_pattern, coding_evidence | grid-free evidence | "frame pattern: no source geometry" |
| temporal detail | the delivered raster (levels 3 / 4) | "temporal detail: classical form (source geometry: <cause>)" |
| adaptive scaler, chroma stage | today's behaviour, byte-identical | "<reader>: classical form (source geometry: <cause>)" |
| status | classical only | "Source identity: classical only (<reason>)" |

The shared accessor `identity/SourceGeometryView.h` (type-separated `Confident()` / `Provisional()`; shared unit U21) reads through `Read()`: a fresh record whose axis is UNSURE is no fallback, the axis simply makes no claim.

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

## Auto engage (SourceResolutionMonitor, VP-V file; SID train r2, N1)

With `source_id_consensus` On and a fresh record, the CONSENSUS (chain + classical voters) feeds `AutoFactor` / `AutoFactors` IN PLACE OF the tracker's declared value (WHAT): a latched uniform ratio below native is the wish (per axis in `AutoFactors`: the latched ratio of each axis, native for an axis it has not latched), anything else is native. WHEN stays the ORIGINAL latch's: the same engage moments (cut, black, low detail, fast pan, else the crossfade after the max wait), look-ahead, dwell and the same immediate / gentle revert paths. So a chain-only detection (the classical trackers declared nothing) creates a re-base only through the consensus. A stale or absent record: the classical decision, with `Monitor::SourceIdGateFallback()` engaged. Off: the original, byte for byte. Shadow: the original decides, the consensus is computed, published and logged as `would_be` lines.

Native-evidence clear (N2): while the consensus is On (the veto exemption also in Shadow, so the would-be is what On decides) and the chain's latest usable record reads an axis CONFIDENTLY below native, ONE native-looking informative frame neither ends the trackers' window / declaration (`Tracker::SetChainContradictsNative`, the frame is counted in `ChainHeld()` / `Monitor::NativeFramesHeldByChain()`) nor raises the classical safety veto on that axis (otherwise the veto would pin the consensus native on every such frame and the chain could never be adopted). Contract details (train r2 round 1): (S2) the record is the latest SEALED one with `evalSeq <=` the sequence of the frame being analysed (`ReadGeometry(rec, cause, seq)`, as the pipeline's chain vote reads it; the newest of all only when the job has no sequence), so frame-bound and live decide alike and a record sealed after a frame never holds it; (S3) PER AXIS: axis H confident holds `trackerH` and the combined tracker, axis V holds `trackerV` and the combined tracker, and the veto is exempt only on the confident axes (`ClassicalReading::chainHoldMask` -> `Vote::vetoExemptFields`; the other axis keeps the classical veto); (S1) BOUNDED: the hold lasts only while the record is fresh by its own maximum age and confident (stale / UNSURE / native = the original behaviour), and while it holds the chain estimator evaluates at its ACQUIRING cadence (`ChainEstimator::NoteNativeHeld`, see chain/README.md), so its own reading re-tests quickly. Measured end to end by `ChainRevertBoundTests` (real monitor + real estimator on a confident 540 -> 1080 upscale that becomes genuinely native): the consensus reverts within `W + 2 x acquiring interval + one frame` of the switch, W = windowMargin x (c1 / windowYieldNats) x acquiring interval (15.9 s with the shipped margin 1.5 since the revert window; it was 31.9 s with 3.0), and within 2 x acquiring interval + 2 frames after a menu end, the first cut after a menu episode or a format change (the evidence window is cleared at those moments, chain/README.md "Revert window"); the tests print the measured times. A genuine native source (the chain reads native or UNSURE, or no usable record) reverts exactly as before. The incident of the 6.3 h run (one native-looking frame cleared the declaration two seconds after the engage) stays OPEN until a shadow-deploy CSV captures the frame.

Earlier text kept for the unchanged parts: the latch / crossfade still decide WHEN. Off: the worker skips the pipeline and the noise measurement; every decision is unchanged (only a cheap reset on a discontinuity and an uncontended gate lock remain). The consensus safety veto is fed by the classical trackers: it fires when every tracker is back to native with nothing gathered below it on an informative frame (and the chain does not contradict native, N2). The SRM history CSV is written in every mode with the replay columns (the `sid_*` columns are filled while the pipeline runs, shadow or on; see source_analysis/README.md "History CSV"). A shadow `would_be` row is written per consensus change.

## Config keys (module side)

- `source_id_consensus` = off | shadow | on (default **off**; shadow after phase A, doc 78 D16): `Monitor::SetSourceIdMode` (`SetSourceIdConsensus(bool)` = on / off). **Shadow** runs the pipeline and the consensus, publishes P14 (record flag `kFlagShadow`, bit 4; `kFlagConsensusOn` stays clear) and the CSV `sid_*` columns, and logs one "source_id would-be: <field> <from> -> <to> (<reason>, support <s>)" line per consensus change (`WouldBeLines`), but `AutoFactor(s)`, `QualityView`, the re-base port and the ABR notice use the classical decision / stay silent, byte-identical to off. Status line "Source identity: shadow (classical decides)". The settings key plumbing (P3 settings mixin, validators, master-page row) is the SID-5 step and waits for P3 / the master page; until then the Monitor setters are the only entry. Defence in depth: `View()` and the Monitor gate also treat a record flagged `kFlagShadow` as no usable record (reason "shadow record"), so a future call site passing consensus-on cannot act on a shadow record.
- `source_id_abr_notice` = on | off: `Monitor::SetSourceIdNotice`.

## Settings

| Key | Section | Type | Range / values | Default | Config placement | Original (Bill) | Fallbacks / status | Tiers |
|---|---|---|---|---|---|---|---|---|
| `source_id_consensus` | input group (plumbing pending, SID-5: Monitor setter only) | enum | off, shadow, on | off (shadow after phase A, doc 78 D16) | not in Config yet (SID-5, P3 settings mixin) | forced off | "Source identity: off" / "Source identity: shadow (classical decides)" / "Source identity: classical only (<reason>)" / "Source identity: consensus (tier N, level L)"; FallbackCatalog `product.source_quality` | same on all |
| `source_id_abr_notice` | input group (plumbing pending, SID-5) | bool | on, off | on | not in Config yet (SID-5) | forced off | one-time notice `abr_quality_drop` (only while `source_id_consensus` is on; never in shadow) | same on all |

## Status and fallbacks

- Ctrl+I / Config line `Source identity: ...` (off, off (Original pathway), shadow, classical only (<reason>), consensus), followed by ` | chain: H 1920->3840 lanczos3; V ...` or ` | classical (chain: <state>: <reasons>)` while a chain estimator is wired; the estimator's own line (`Snapshot::chainNote`) rides on the existing stats line.
- `product.source_geometry` (engaged while the consensus is On and the geometry record is absent, stale, of another unit version or corrupt): `Monitor::SourceIdGeometryFallback()`.
- Shadow: log line "source_id would-be: <field> <from> -> <to> (<reason>, support <s>)" per consensus change.
- Consumers fall back to the classical value, visibly (`product.source_quality`), whenever the record is absent, stale, unlatched or another version; in shadow they always use the classical value.

## History CSV and replay (SID-B0)

The SRM history CSV (`SourceHistoryLog.h/.cpp` in the parent folder, see source_analysis/README.md "History CSV") is session-long, appended and rotated, and carries what the consensus needs: `mode, t_src, seq, frame_ratio_v, frame_ratio_h, detail_above, mixed, blank, cut, fade, apl, axis_x, axis_y, axis_conf, kernel_id, kernel, kernel_votes, kernel_agree, frame_quality, frame_noise, sid_tier, sid_level, sid_ratio_h, sid_ratio_v, sid_reason` (with the older `declared`, `confidence`, `bitrate_quality`, `frame_ratio`). `SourceIdReplay.h` (`SrmReplay::Run`; columns by NAME, never by position) feeds a file (or the concatenated files of a session; the repeated headers and `session` rows are skipped, a `discontinuity` row resets the pipeline) through `Pipeline` in source time, using the recorded frame quality (`Pipeline::Push(in, &quality)`); the same CSV always gives the same trace (`SrmReplay::Trace`), so a captured session can be re-run against changed thresholds. `python -I tools/eval/source_id/replay.py --srm <file>` reads the same files (a rotated session's continuations are followed) and prints a SUMMARY; the replay itself is `SrmReplay`. Limits: CSV values are rounded (`%.4f` ratios and `declared`, `%.6f` times and frame quality, and only `%.2f` / `%.3f` for the base-row `confidence`, `bitrate_quality` and `coherence_*`; the consensus weight is confidence x trust against `enterMinWeight` 0.25 / support 0.67, so a replay can differ from the live run at a threshold edge by more than the finer roundings suggest); the relative-noise tiles are not stored (the recorded frame quality stands in); the replay has no chain voter (a session captured with the chain enabled replays on the classical voters only, so its consensus trace can differ), and the CSV `detail_above` is the veto as the consensus saw it after the N2 exemption (0 when both axes were exempt; a one-axis exemption leaves it 1) - the raw classical flag is not a column yet (a later CSV version); a file written with the pipeline off has no `frame_quality` (it defaults to 1.0) - replay files written in shadow or on. Tests: `SourceIdReplayTests` (a committed fixture, `tests/fixtures/srm-history-replay.csv`).

## Still to do (call sites outside this module)

1. LVR (Bill file, P11): construct/refresh the setters from the settings (`SetSourceIdMode`, `SetOriginalPathway`, `SetChainEnabled`, `SetChainEvidence`, optionally `SetChainTap(this)`); call `ApplySourceIdRebase(indicatorPort)` once per frame; swap the `snap.bitrateQuality` readers (LVR 27075 / 27141 / 27880) to `monitor.QualityView().quality`; show a fired notice (`TakeSourceIdNotice`) through the one-time FallbackStatus notice `abr_quality_drop`; print `SourceIdStatusLine()` on the Ctrl+I line and `Stats` in Ctrl+Alt+D.
2. The Indicator adapter for `IndicatorPort` (`Advance(kRebaseH/V, Cause::RebaseDecision, fromSeq)`) when I0 is wired in the renderer.
3. Later: the ID-net runtime calls `IdNetVote` on its events.

Fallback entry when consensus is off or unavailable: "Source identity: classical only (<reason>)". The measurement plan is in `MEASUREMENT-PLAN.md`; the streaming-suite evaluation in `tools/eval/source_id/` (`make_abr_reels.py`, `replay.py`, `RESULTS.md`).
