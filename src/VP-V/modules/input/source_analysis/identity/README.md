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
- `SourceGeometryProduct.h`: the P14g `source_geometry` reservation (wave S2, section below): unit version, size bound, byte order, staleness rule, the absent placeholder and the one read API.

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

## P14, the source identity product (doc 61 section 2.1, D-SID1)

`Products.h` declares `kSourceIdentity` (`p14_source_identity`, CPU, T0, 256 B of system RAM, unit version `SourceIdentity::kUnitVersion` = 1, catalog expectation `kExpectedSourceIdentityUnit`). It is appended last, so every earlier id is unchanged. It is a **HELD-STATE** product with **no live fill**: the consensus is hysteretic (3 s dwell), so recomputing it on the render thread would differ from the worker's answer. Its consumers read it at **lag 1** in the catalog (the latest sealed record with seq <= N), so no consumer can ever be its live-fill owner (a catalog test pins this).

- `SourceIdProduct.h`: the `Record` (axes, kernel, codec class, tier, latched quality and the event-anchored level, per-field reason/confidence/latch flags, the newest ABR event, the notice serial, `rebaseGen[2]`, `consensusGen`, `maxAgeUs`), its fixed little-endian byte codec (`Pack`/`Unpack`, 256 B, another unit version is refused), `HeldState` (the publish/read holder: an 8-record ring, `ReadLatest(seq)` returns the record that was current for an earlier frame) and `RebaseApplier` (render side: each rebase generation reaches the Indicator port exactly once; a new title or discontinuity restarts the generations).
- Stale: `maxAgeUs` is derived from the settings (twice the longest classical vote expiry), never fixed. A stale, absent, unlatched or other-version record means every consumer uses the classical value, visibly (below).
- The graph/doc view: `docs/design/product-graph.md` is regenerated by `ProductCatalogTests` (`VP_WRITE_PRODUCT_GRAPH_DOC=1`).

## P14g, the source geometry product (wave S2 reservation, doc 78 section 6)

`Products.h` declares `kSourceGeometry` (`source_geometry`, id 29, appended after the doc 72 list; CPU, T0, history 1, system RAM, declared 512 B,
unit version `SourceGeometry::kUnitVersion` = 1, catalog expectation `kExpectedSourceGeometryUnit`). Like P14 it is **HELD STATE with no live fill**: the
source-resolution side is the single writer and publishes on each evaluation; a consumer reads the latest SEALED record with seq <= N (a lag >= 1 read);
a record older than `maxAgeUs` (`SourceGeometry::MaxAgeUs`: twice the longest MEASURED evaluation interval, never a fixed rate; a record that states no
maximum age is treated as stale) is stale. The layout is fixed little-endian of at most 512 B (`PutU16/32/64`, `GetU16/32/64`); its fields (per axis the
measured and the applied state, n, N, ratio, phase, kernel and parameters, sharpening, double scaling, confidence; the active area; the coded-raster
class; the reserved chroma chain; geometryGen / appliedGen) are laid out by the source-identity build inside that bound, which bumps the unit version and
the catalog expectation with any layout change. This wave holds only the reservation: the id, the catalog row, the catalog readers and an ABSENT
placeholder (`PlaceholderEntry()` publishes nothing).

Provisional readers (catalog rows, all lag-1 held reads): quality_pattern and coding_evidence and temporal detail (one row of the temporal-detail family),
the adaptive scaler, the chroma stage, status. Each has an absent-tested read (`SourceGeometry::Read(consumer, entry, requested, frameSeq, nowUs)`): not
requested (today: no key asks for the estimate, `kRequestedToday` = false) = byte-identical, nothing engaged, no string built; requested with the record
absent / stale / of another unit version / not yet sealed = the delivered raster and the reader's classical form, VISIBLY (`product.source_geometry`, the
reason names the consumer and the cause; `Combine()` is the one decision over every consumer so one reader's recovery cannot release another's entry).

| Reader | Falls back to | Status wording |
|---|---|---|
| quality_pattern, coding_evidence | grid-free evidence | "frame pattern: no source geometry" |
| temporal detail | the delivered raster (levels 3 / 4) | "temporal detail: classical form (source geometry: <cause>)" |
| adaptive scaler, chroma stage | today's behaviour, byte-identical | "<reader>: classical form (source geometry: <cause>)" |
| status | classical only | "Source identity: classical only (<reason>)" |

The shared accessor `identity/SourceGeometryView.h` (type-separated `Latched()` / `Provisional()`) belongs to the source-identity build and reads through `Read()`.

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

With `source_id_consensus` on and a fresh record, `AutoFactor` / `AutoFactors` engage a lower re-base only for a LATCHED axis, at the consensus's ratio; the classical confidence minimum (`source_resolution_auto_min_confidence`) still applies (consensus can delay, veto or refine an engage, never add one), and the latch / crossfade still decide WHEN. A stale or absent record: the classical decision, with `Monitor::SourceIdGateFallback()` engaged. Off: the worker skips the pipeline and the noise measurement; behaviour and the CSV are unchanged (only a cheap reset on a discontinuity and an uncontended gate lock remain). Consensus moves only TOWARDS native: the target is never deeper than the classical reading. The consensus safety veto is fed by the classical trackers: it fires when every tracker is back to native with nothing gathered below it on an informative frame. The SRM history CSV gains `sid` columns only while consensus is on.

## Config keys (module side)

- `source_id_consensus` = off | on (default **off**): `Monitor::SetSourceIdConsensus`. The settings key plumbing (P3 settings mixin, validators, master-page row) is the SID-5 step and waits for P3 / the master page; until then the Monitor setters are the only entry.
- `source_id_abr_notice` = on | off: `Monitor::SetSourceIdNotice`.

## Still to do (call sites outside this module)

1. LVR (Bill file, P11): construct/refresh the setters from the settings; call `ApplySourceIdRebase(indicatorPort)` once per frame; swap the `snap.bitrateQuality` readers (LVR 27075 / 27141 / 27880) to `monitor.QualityView().quality`; show a fired notice (`TakeSourceIdNotice`) through the one-time FallbackStatus notice `abr_quality_drop`; print `SourceIdStatusLine()` on the Ctrl+I line and `Stats` in Ctrl+Alt+D.
2. The Indicator adapter for `IndicatorPort` (`Advance(kRebaseH/V, Cause::RebaseDecision, fromSeq)`) when I0 is wired in the renderer.
3. Later: the ID-net runtime calls `IdNetVote` on its events.

Fallback entry when consensus is off or unavailable: "Source identity: classical only (<reason>)". The measurement plan is in `MEASUREMENT-PLAN.md`; the streaming-suite evaluation in `tools/eval/source_id/` (`make_abr_reels.py`, `replay.py`, `RESULTS.md`).
