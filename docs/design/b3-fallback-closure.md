# B3 fallback closure: retiring the pre-B3 path

Status: plan, 2026-10-24. Not implemented. It will be implemented in ONE integration batch
together with:

- the net-runner wiring;
- the glue/segment extraction (S1 entrance segment);
- the silent-fallback call sites;
- the efficiency fixes.

One big gate run comes at the end of that batch.

## Goal and context

B3 (`pipeline_entrance_once`, ADR-0024) has been the default since 2026-10-24. The owner has
retired the pre-B3 path. It still exists only as:

- the hidden diagnostic key `pipeline_entrance_once: off`;
- the place where B3's remaining cases fall back to.

After the closures below, the pre-B3 per-picture path can be deleted from
`LibplaceboVideoRenderer.cpp`:

- the `EntrancePath::Extra` per-picture stages on warped/interpolated pictures;
- `RunVpPhaseInterpolation` followed by the Main phase path;
- the A-alongside (`aNeeded`) processing;
- the per-picture crossfade-from stages.

The byte-identity reference moves from live code to a **frozen r6 capture set**: the
`ByteIdenticalToTheR6Baseline` inputs and their recorded raw-R10 hashes, stored as test data.

**Bill's Original pathway is not affected.** It has no VP motion pictures: the de-judder,
interpolation and hiccup repair are VP additions and are off there. It never used B3 or the
pre-B3 motion path.

## How the closures fit S1 and the net runner

**S1 entrance segment** (segments-callsites / central-migration-needs §6). There is one call:
`entrance.Run(sourceFrame, path, vectors) -> processed picture`. The B3 ring, the stage order
and the per-path rules all live inside it.

- After the closures, S1 needs only two roles:
  - `A`: the frame drawn as itself;
  - `BAhead`: the next frame, processed as its own A.
- It also needs one parameter: the re-base factors.
- The motion consumers (warp, interpolator, hiccup pairs, the crossfade mixer) only ASK S1 for
  processed pictures. They never run a stage. That is the B3 design. Each closure below removes
  one way a consumer can still bypass S1.

**Net runner front end** (net-runner-wiring.md §5): net A is `case RepairEncoding` inside S1.

- It therefore runs once per source frame, for A, and for B when B is processed ahead as its
  own A.
- A_motion's neighbours (motion-aligned previous/next luma) come from S1's ring and the vector
  field.
- The C dependency of item 9 is shared with A_motion's "next" neighbour, so solving 9 once
  solves both.

## The cases

Effort is in agent wall-clock hours. Risk: L, M or H.

| # | Fallback case (code) | How often in real use | Native design | S1 / net-runner interaction | Effort | Risk |
|---|---|---|---|---|---|---|
| 1 | **Source-resolution crossfade** (`EntranceOnceCrossfading`: `rebaseFade < 1`, ~1 s per latch) | Each source_resolution auto latch or cut change: a few times per film, ~1 s each | Ring entries keyed by re-base factors: S1 makes A/B for BOTH factor sets during the fade. The consumer warps or interpolates each pair, and `CrossfadeMix` mixes the two finished pictures (output domain, as today). The from-path detail decision uses `ApplyForeign` on the from-path's own ring entry | S1 `Run(frame, A or BAhead, factors)`. The ring key already contains the factors. Net A runs per factor set only in the A-first order (before re-base: one A result shared by both paths) | 4-6 | M |
| 2 | **Composed picture** (subtitle cut/paste moves the subtitle into a composed RGB picture; `image.planes != a`) | Whenever subtitle moving is on and subtitles show | Compose AFTER the motion picture. Warp or interpolate the processed planes, then apply the subtitle move to the result, as the rendered warp already does through the cache. Alternatively, S1 takes the composed picture as its source (one plane, see 4) | The composition becomes a post-S1 stage on the consumer's output. S1 and net A see only the clean frame, which is also what nets were trained on | 3-5 | M |
| 3 | **Ring copy failure** (`CopyEntrance`: format not renderable or sampleable) | Not seen; only an odd stage output format | Allocate ring textures in a fixed renderable format chosen when the ring is created (r16/rg16 or rgba16f). Keep a compute-copy fallback for any format. The failure then becomes a real error ("Fallbacks engaged") instead of the pre-B3 path | Inside S1 (the ring is S1's) | 1 | L |
| 4 | **Picture not in two planes** (RGB capture: one RGB plane; native RGB ingest 4:4:4; the DV tunnel's RGB route; the matcher luma `dejudderRgbLuma`) | Every RGB source. That can be the owner's LLDV-as-RGB chain, so possibly COMMON | Generalise `WarpProcessed` and `FrameInterpolator::Run(estimate size)` to N planes of any component count: per-plane `plane_scale` from the real sizes, and grain-preserve bands per plane. The motion field already comes from the matcher luma. For DV tunnel pictures decoded inside libplacebo, S1 cannot run on the decoded picture. There B3 runs on the tunnel planes as today (still once per frame) or the stages move after libplacebo's decode (a separate decision) | S1 handles 1-, 2- and 3-plane frames. Net A's RGB path (range helpers, then a delta to RGB) is already in the wiring draft | 4-6 | M |
| 5 | **Both entries identity** (no stage changed the picture; `pa->identity && pb->identity`) | Whenever no entrance stage acts: deband idle, detail off, no re-base. Common on clean sources | Not a real fallback; today it runs the identical pre-B3 path. Natively, interpolate or warp the caller's raw planes directly (identity entries already map to them through `EntranceOwnedFrame`), with no stages. One path either way | S1 returns the raw frame for an identity entry, which is its normal result | 0.5 | L |
| 6 | **No next frame known** (`nextSequence == 0`) | Stream start, queue empty, after a reset: seconds per session | The picture is the processed A, held: the interpolator's "no B" rule, made from S1's A. No pre-B3 path | S1 `Run(A)` only | 0.5 | L |
| 7 | **Next frame's planes not uploaded** (extras with Stage A interpolation; `dejudderNextTextures` null) | Rare (an upload failure, low memory) | As in 6: hold the processed A, shown | as in 6 | 0.5 | L |
| 8 | **Interpolator failure on processed planes** (shader rejected, dispatch failed) | Not seen | As in 6: the processed A held, under "Fallbacks engaged" (already visible). Never the pre-B3 path | none | 0.5 | L |
| 9 | **B processed per picture** ("B's vectors not ready ahead"). Causes: (a) `dejudder_estimate_ahead off`; (b) C not queued: start-up, short queues, LOW LATENCY (queue 1-2); (c) the estimate ahead failed after its snapshot | (a) only a deliberate diagnostic setting. (b) every low-latency session and every start-up. (c) rare. **Low latency makes this COMMON for 50/60p game/sports use** | Remove the C dependency. The stateful stages (debander hold, detail pulse / texture restore history) take the frame's BACKWARD field (S-1 -> S) instead of the forward one (S -> S+1). For B that is the A->B pair, which already exists when A's pictures are made. B can then always be processed as its own A, with no estimate ahead. This matches the agreed low-latency vector rule (backward vectors without look-ahead; memory "Low-latency vectors"). It also gives net A_motion's neighbours without C. Retest the debander's grain gates and the detail stages' flicker on real clips | S1 `Run(BAhead)` needs only the A->B field: the consumer passes the pair's field it already holds. Net A_motion: previous = A (processed), next = none, i.e. backward-only A_motion. Its training must match, a decision for the kit | 6-10 | M-H |
| 10 | **Occlusion fill off on processed planes** (source warp; `fill && !processedTarget`; shown as "entrance once fill") | Only `dejudder_warp_stage: source` with the occlusion fill on (the default warp is rendered) | Fill from the processed B. B exists as B's A in the ring, and the fill shader samples it through `luma_scale` like A | S1 `Run(BAhead)` (already made for interpolation; for the warp, when the fill wants it) | 2-3 | L-M |
| 11 | **Late fallback pictures after B-as-A** (the HIGH-1 guard: hold of processed A, or the Extra per-picture path without detail when no held A) | Only as a consequence of 1-10 | Disappears when 1-10 are closed. Until then it never restarts a history (asserted by the "history breaks" counter) | none | 0 | L |
| 12 | **The extras' per-picture path for render-ahead or composed pictures** (the extras loop's `!processedFrame` branch) | Covered by 2 and 4 | Disappears with 2 and 4 | the extras call S1/S2/S3 objects (segments §6) | 0 | L |
| 13 | **The diagnostic key `pipeline_entrance_once: off`** | Tests only | Delete it with the pre-B3 code. Freeze the r6 byte-identity reference as a capture set first: inputs plus raw-R10 hashes in test data. The live `ByteIdenticalToTheR6Baseline...` then compares the default to the frozen hashes | none | 1-2 | L |

**Not B3 fallbacks, listed so they are not confused with them:**

- the rendered warp (the default 24p-in-60 de-judder), which already warps the finished picture;
- the Original pathway (no VP motion);
- the stateless promotion (B's per-picture run IS B's A when no stage keeps state).

## Order inside the integration batch

1. 3, 5, 6, 7, 8 and 13 (small, low risk) as part of the S1 extraction. With them S1 has its
   final interface: `Run(frame, role, factors, field)`.
2. 9, the backward-field rule, together with net A_motion's neighbours, since they share the
   C dependency. Real-clip grain and flicker checks.
3. 4 (N planes) and 10 (fill from processed B).
4. 2 (compose after the motion picture) and 1 (crossfade of two processed paths).
5. Delete the pre-B3 code (the `Extra`/`CrossfadeFrom` per-picture branches,
   `RunVpPhaseInterpolation` with the Main phase path, `aNeeded`) and the diagnostic key.
6. One big gate: the chain gate (B3 only), the delay gate, DV, real-clip acceptance, the frozen
   r6 set, and the nets' zero-cost and bypass tests.

**Total:** about 23-35 agent hours. The riskiest step is item 9, because it changes the
stateful stages' vector semantics.
