# VP HDR optical-flow interpolator + 50/60p - implementation plan (2026-10-16)

Planning only (read-only survey); builds on `docs/VP-nvof-ahead.md` §"smooth motion (optical flow)"
(flow -> forward/backward consistency -> vector infill -> bidirectional warp + blend -> hole fill;
the engine sees an 8-bit luma copy for motion only, the picture is built from the real 10/16-bit
frames, VP's temporal dither on every frame; exclusive with the other engines by profile rules).

## Exists

Engine wrapper (`NvOpticalFlow.*`); producer ahead with forward + backward 4x4 fields, costs and the
global camera (`MotionAheadProducer.*`); `VectorStore` keyed by generation; depth decision
(`MotionAhead.h`); matcher / warp / trust / D5 fill (`DejudderWarp.*`); extras planner
(`DejudderPolicy.h`); per-vsync timeline for smooth motion (`TemporalRenderer.h`); engine
exclusivity (`DejudderPolicy::ResolveEngine`); motion invalidation (`RequestMotionInvalidate`).

## Missing

- The interpolator itself.
- A per-vsync VP synthesis path (the extras model cannot do 60p on 50 Hz; at 50p on 60 Hz it only
  fills the duplicate refresh).
- One vectors-valid state (`VP-dejudder-stream-robustness.md`: each consumer checks its own key).
- Uploading the producer's 4x4 field to the render GPU (today it only seeds the matcher).
- The producer starts only with the de-judder.
- Fixed rates against the owner's rule: `DejudderPolicy::IsFilmLike` (30.5, 47.5-48.5) and the
  10-240 / 10-480 Hz timing bounds - flag, generalise behind a switch.

## Build order

1. **One vectors-valid state** (`MotionValidity.h`, pure): epoch + `Invalidate(cause)` from
   resync / queue reset / gaps / cadence repeats / drift drops / smooth-motion mixing / re-base
   changes; `motionGeneration` folded in; every consumer gates on it. No behaviour change - proven by
   `NativeAndNewSettingsOffArePixelIdentical` and `StreamFaultsKeepTheDejudderSafeAndRecovering`.
2. **Interpolation policy** (`InterpolationPolicy.h`, pure): from measured capture / pulldown film
   rate and measured display rate, ratios and tolerances only. Film class stays with the de-judder;
   video class at a non-integer ratio -> interpolator; integer ratios pass through (50/60 sports: no
   smoothing). `Phase(outputPts, ptsA, period)`.
3. **GPU interpolator** (`FrameInterpolator.*`, MotionWarp's dispatch style): A, B, t, fields
   (matcher's or the engine's uploaded 4x4 flow); reuse the field GLSL, D3 infill, the occlusion
   mask; hole fill hold-nearest (blend optional); output in the input plane formats, so scaling,
   tone mapping and dither run unchanged (HDR-safe).
4. **Per-vsync presentation**: a VP mode in `TemporalRenderer` (B from the queue); each draw with
   t > 0 renders the interpolated planes; no B (zero queue / low latency) = A, today's output.
5. **Vectors**: producer wanted for the interpolator too; `Take(a, b)` when valid, else engine at
   render time, else the matcher. Off in low latency.
6. **Gap / repeat fill** (own switch, default off): a fresh pair across k missing periods.
7. Optional NvFRUC SDR reference (licence unreviewed; about a day).
8. Config, tests, corpus; then the 3090 and the chain gate (new matrix axis).

## Config keys (renderer input group; `RendererProfileConfig.h` validation, editor, defaults)

| Key | Values | Default |
|---|---|---|
| `interpolation` | off / auto (non-integer ratio only) / always (smooth-motion parity) | off |
| `interpolation_fallback` | hold / blend | hold |
| `interpolation_fill_gaps` | off / on | off |
| `source_class` | measured / legacy (the IsFilmLike generalisation) | legacy |

Queue minimum: 1 frame ahead (5 with the producer); low latency exempt.

## Tests

CPU: validity invalidation on every cause; every consumer falls back while invalid; a ratio sweep
(integer ratios pass through, 1.2 / 0.833 / 1.001 by the rules, film to the de-judder); zero queue /
low latency hold; exclusivity with smooth motion; phase timeline at any ratio; gap pairs only with
the switch; Config validation and editor controls. GPU: t=0 is A and t=1 is B exactly; a pan lands at
the true phase; occlusion holds nearest; a cut holds the whole frame; 10-bit / PQ ramps keep
precision; engine field vs matcher field. Harness: runs at a measured rate mismatch; zero-queue low
latency from the first frame; stream faults fall back and recover; exclusive with smooth motion.

## Open questions for the owner

1. Per-vsync synthesis re-renders the chain per output (cost at 4K on the 3050 Ti): ship the
   extras-only model (fill the duplicate refresh) first?
2. Interpolate only at non-integer ratios (50p on 60 Hz), never "always" for video-class sources?
3. Fallback hold (the de-judder's "never a blend") or blend (smooth-motion parity)? Proposed: switch,
   default hold.
4. Gap fill pairs across a gap (against "never pair across a gap") - OK behind a switch?
5. Near-1 ratios (59.94 on 60): leave to drift correction (excluded by the ratio tolerance)?
6. Generalise `IsFilmLike` now (behind `source_class`) or later?
7. NvFRUC SDR reference: include?
8. Subtitle-composed and RGB sources: hold?

## Step 1 wiring

`src/VideoProcessor-Lib/MotionValidity.h` (pure, header-only) and
`src/VideoProcessor-Test/MotionValidityTests.cpp` exist; nothing calls them yet. Line numbers are in
`LibplaceboVideoRenderer.cpp` (LVR) as of 2026-10-16 in the shared tree, and they move as others edit.

API: `MotionValidity::State`: `Invalidate(Cause)` (any thread; moves the epoch), `Epoch()`,
`Generation()` (= epoch), `StreamGeneration()` (moves only on Resync / QueueReset / Seek /
SourceFormatChange, so it equals today's `motionGeneration`), `Current(epoch)`,
`PairValid(from, to, epochAtFrom, epochAtTo[, contiguous])` (same current epoch, `to == from + 1`,
plus the caller's contiguity), `NoteKept(sequence[, holeCause])` -> `Continuity {First, Contiguous,
Repeat, Break}` (render thread; a repeat is counted and excluded, a hole invalidates), `KeptPairValid`,
`LastKept`, `EpochOfLastKept`, `Consume(seenEpoch)` -> `Pending {changed, release, cause,
invalidations}` (the render thread's apply point), `Count(Cause)`, `Total()`, `Repeats()`,
`LastCause()`. Free functions: `Describe(Cause)` / `Name(Cause)`, `ReleasesResources(Cause)` (Resync,
SourceFormatChange), `Adjacent(from, to)`. `MotionValidity::ConsumerGate`: `Bind(from, to, epoch)`,
`Usable / UsableFrom / UsableAt`, counted `Check / CheckAt`, `Uses()` / `Fallbacks()`.

### A. The state and the request/apply split

| Where | Today | Replacement |
|---|---|---|
| LVR:5225 `std::atomic<uint64_t> motionGeneration{ 1 }` | the producer's generation | `MotionValidity::State motionValidity;` plus `uint64_t motionValiditySeen = MotionValidity::State::kInitialEpoch;` (render thread). Delete `motionGeneration`. |
| LVR:10374-10379 `motionInvalidate`, `motionInvalidateCause` | request flag + cause | delete. `motionInvalidations`, `motionInvalidateLogTick`, `motionRecoveryLogPending` and `motionInvalidateLoggedCause` stay for logging (the last one becomes a `MotionValidity::Cause`). |
| LVR:10381 `RequestMotionInvalidate(bool release, const char* cause)` | sets the flag | `void RequestMotionInvalidate(MotionValidity::Cause c) { motionValidity.Invalidate(c); }`, or call `motionValidity.Invalidate` directly at the call sites in B. |
| LVR:10392 `ApplyMotionInvalidate()` | `motionInvalidate.exchange(0)` | `const auto p = motionValidity.Consume(motionValiditySeen); if (!p.changed) return; motionInvalidations += p.invalidations; if (p.release) ReleaseDejudder(); else { ...today's forget body... }`. The log uses `MotionValidity::Describe(p.cause)` and compares `p.cause` instead of the string pointer. Add `debander.ForgetHold()` (E, last row). It is still called from LVR:12669. |

### B. Causes (every `Invalidate` call)

| Where | Today | Replacement |
|---|---|---|
| LVR:21814 `Reset()` | `RequestMotionInvalidate(true)` | `motionValidity.Invalidate(Cause::Resync)` |
| LVR:21817 `Reset()` | `motionGeneration.fetch_add(1)` | delete (the Resync moves both the epoch and `StreamGeneration()`) |
| LVR:21857 `ResetLiveQueue()` | `RequestMotionInvalidate(false, "live queue reset")` | `Invalidate(Cause::QueueReset)` (a caller that knows it is a seek may pass `Cause::Seek`) |
| LVR:21859 `ResetLiveQueue()` | `motionGeneration.fetch_add(1)` | delete |
| LVR:24436-24437 dequeue | `if (dequeued.gapBefore) RequestMotionInvalidate(false, dequeued.gapCause)` | `Invalidate(Cause::Gap)`. Keep `gapCause` for a log line. These all set `gapBefore` and need no change: missed frames (LVR:20635), pulldown lock change / off (LVR:20698-20700, 20746-20748), queue overflow drop (LVR:20797-20802), backlog flush (LVR:24857). |
| LVR:25151 `correctionDecision.action == Drop` (render loop: frame released, then `continue`) | nothing (sequence checks cover it: the next frame has no estimate) | `m_impl->motionValidity.Invalidate(Cause::DriftDrop)` before `continue`. `NoteKept` (C) also catches the hole. |
| LVR:24721 motion engine resolved (`smooth != smoothMotionEngine`) | log only | on a change: `Invalidate(Cause::SmoothMotionMix)` |
| LVR:24734 `temporalRenderer.Mixing() != smoothMotionMixingLogged` | log only | on a change: `Invalidate(Cause::SmoothMotionMix)` |
| LVR:18362-18363 `rebaseFactor / rebaseFactorX =` | assigned | keep the previous values and call `Invalidate(Cause::RebaseChange)` when they change. Today the field survives a factor change (comment at LVR:19292: the vectors belong to the native frame), so this is a small behaviour change: one estimate's hold per change, and changes are rare (smooth motion or deinterlace toggles, a paused re-base after a failure). Optional for step 1 for the field; VP deband's hold weights are plane-sized and do need it. |
| LVR:24444 / 24457 `SameDejudderVideoState(state, next.state)` false | B not offered (no pair) | where a frame's `state` differs from the previously rendered frame's (a format change without a Reset): `Invalidate(Cause::SourceFormatChange)` (releases). A Reset normally comes first, so this is usually a harmless second invalidation. |
| LVR:12990-12998 `motionSourceLogged != source` | log only | when the source switches between the engine and the block searches: `Invalidate(Cause::MotionSourceChange)` (the trust and hold hysteresis belong to the old source). Use the same cause at the producer `Stop()` (LVR:18244) and at producer start. |
| LVR:12942 `dejudder->SetSettings(next)` | applied in place | `Invalidate(Cause::SettingsChange)` when `opticalFlow` or a setting that changes what the field means (match tolerance, est*, lowRes) changes. This changes behaviour, so it is optional for step 1; if the pixel-identity test must hold, count it in the log only. |

### C. Contiguity (`NoteKept`)

In `UpdateDejudderEstimate`, next to the `ApplyMotionInvalidate()` call at LVR:12669 (callers:
LVR:14818 / 14851 / 14926), call `motionValidity.NoteKept(sourceSequence, Cause::DriftDrop)` first,
then `ApplyMotionInvalidate()`. Call it for every presented frame, cadence repeats included:
`cadenceRepeat` frames reuse `sourceSequence` (LVR:25205), so they come back `Repeat` and leave the run
intact. A hole (an overflow, backlog or drift drop that nothing else reported) comes back `Break`,
has already invalidated, and is applied before this frame's estimate. Skip `motionDraw > 0` (smooth
motion's extra draws, LVR:24768): they carry the same sequence, so calling it is harmless but only
adds noise.

### D. The de-judder (pair producer)

| Where | Today | Replacement |
|---|---|---|
| LVR:4766 `dejudderEstimatedSequence` | the estimated A | add `uint64_t dejudderEstimatedEpoch = 0;`, set to `motionValidity.Epoch()` together with the sequence at LVR:12784 and LVR:12808, cleared at LVR:10409 |
| LVR:12760 `if (reason != Active \|\| cadenceRepeat \|\| sourceSequence == dejudderEstimatedSequence) return;` | | add `\|\| !motionValidity.PairValid(sourceSequence, nextSourceSequence, motionValidity.EpochOfLastKept(), motionValidity.Epoch())`. LVR:24444 already drops B for `gapBefore`, repeats, film updates and a different state; this adds adjacency and the epoch. |
| LVR:12780 `dejudderAhead.Consume(sourceSequence, nextSourceSequence)` | the ahead pair | store `motionValidity.Epoch()` when the extras make the ahead estimate and also require `motionValidity.Current(thatEpoch)`. `ApplyMotionInvalidate` already calls `Reset()`, so this is a second safeguard. |
| LVR:12969 `motionProducer->Take(aSequence, bSequence)` | store hit | call it only when `motionValidity.PairValid(aSequence, bSequence, motionValidity.EpochOfLastKept(), motionValidity.Epoch())`; otherwise `SetExternalFlow(nullptr)` (the matcher runs) |
| DejudderWarp.h:445 `ForgetHistory()` | called from `ApplyMotionInvalidate` | unchanged; it stays the only call (A) |

### E. Consumers (each gates on the epoch its vectors were made in)

| Consumer | Where | Replacement |
|---|---|---|
| Extras (warp) | LVR:13128 `dejudderEstimatedSequence != f.sourceSequence` -> hold | add `\|\| !motionValidity.Current(dejudderEstimatedEpoch)` -> `DejudderHold(MotionValidity::Describe(motionValidity.LastCause()))`. The cadence-repeat hold at LVR:13133 is unchanged: the repeated presentation falls back and the pair survives. |
| Sigma-delta in the extras (VPVEC/VPPIX) | LVR:13410 `pixelVectors`, LVR:13479-13510 `SetSigmaDeltaVectors` | no change: already behind the LVR:13128 gate |
| Sigma-delta on B's first present (next-frame VPVEC) | made at LVR:13651-13667 (`dejudderNextVectorsSequence = f.nextSourceSequence`), used at LVR:19503 | add `MotionValidity::ConsumerGate ditherNextGate;`. `Bind(f.sourceSequence, f.nextSourceSequence, dejudderEstimatedEpoch)` at LVR:13667; at LVR:19503 add `&& ditherNextGate.CheckAt(motionValidity, sourceSequence)`; `Clear()` at LVR:13651 and LVR:19527. |
| Occlusion mask | written at LVR:12982 (`dejudderMaskSequence[maskSlot] = bSequence`), read at LVR:19288-19296, cleared at LVR:4961, 10407 and 19354 | add `uint64_t dejudderMaskEpoch[2]`, set to `motionValidity.Epoch()` with the sequence at LVR:12982; at LVR:19290 also require `motionValidity.Current(dejudderMaskEpoch[slot])` (or use two `ConsumerGate`s with `UsableAt(bSequence)`) |
| Grain map (Stage 8) | `SetGrainField` at LVR:19105 and LVR:19169 when `dejudderEstimatedSequence == sourceSequence`; read at LVR:19284 (`LastMapState() == Used`) | add `&& motionValidity.Current(dejudderEstimatedEpoch)` at both sites. `AdaptiveDownscale::ForgetMap()` (AdaptiveDownscale.h:208), called from `ApplyMotionInvalidate`, stays the reset. |
| Residual / CNN-SSIM hold motion | `SetMotion` at LVR:19095 and LVR:19159 | add `&& motionValidity.Current(dejudderEstimatedEpoch)` |
| VP deband field | LVR:18404 `vpDebandParams.field = dejudder->Field()` | add `&& motionValidity.Current(dejudderEstimatedEpoch)` |
| VP deband hold | VpDebander.cpp:163-164 (`consecutive = params.sequence == weightSequence + 1`) | Finding: the hold checks sequence adjacency only, and a missed-capture gap keeps the sequence contiguous, so today the hold carries across that gap. Add `void ForgetHold() { weightSequence = 0; prevValid = false; }` to `VpDebander::Module` (the same two lines as the end of `Release`, VpDebander.cpp:138-139) and call `debander.ForgetHold()` from `ApplyMotionInvalidate`. |

### F. The producer (`motionGeneration` fold-in)

| Where | Today | Replacement |
|---|---|---|
| LVR:24277 `const uint64_t generation = m_impl->motionGeneration.load(...)` | Submit's generation and the engagement key | `m_impl->motionValidity.StreamGeneration()`: the same values as today, so step 1 does not change behaviour |
| LVR:24282-24296 `motionAheadEngagedGeneration` | disengages on a new generation | unchanged, keyed on `StreamGeneration()`. Keying it on the epoch would wait for 4 queued frames after every gap. |
| LVR:24311 `motionProducer->Submit(ahead.sourceSequence, generation, prepared.source)` | no break passed | frames with `ahead.gapBefore` (or `filmUpdate`) must not pair with the previous submission. Add `bool breakBefore` to `Producer::Submit` (MotionAheadProducer.h:168); in `Process`, set `havePrevious = false` when `job.breakBefore` (MotionAheadProducer.cpp:378-386), and replace `previousSequence < job.sequence` at :386 with `previousSequence + 1 == job.sequence` (`MotionValidity::Adjacent`). Today such pairs are computed but never requested (D's `Take` filter); with this the store holds only valid pairs. |
| Full fold (later, step 1b) | | key the store on `Generation()` (the epoch) and have `Take(a, b, epoch)` refuse other epochs. This costs up to a queue depth of matcher pairs after each gap, because ahead pairs computed before the gap frame is dequeued are discarded. If that shows, stamp the epoch at enqueue instead. |

### G. Tests to add once wired (the plan's step-1 proof)

`NativeAndNewSettingsOffArePixelIdentical` and `StreamFaultsKeepTheDejudderSafeAndRecovering` must
still pass unchanged. Add: for each cause, until a fresh contiguous pair arrives, the extras hold, the
mask is not installed, the grain field is not set, the next-frame VPVEC is not bound and `Take` is not
called; a missed-capture gap resets VP deband's hold.
