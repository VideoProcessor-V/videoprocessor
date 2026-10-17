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
