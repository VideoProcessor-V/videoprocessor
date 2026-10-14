# De-judder on streamed DeckLink input: findings (2026-10-14)

A read-only trace of the code, prompted by the owner's concern about streamed content through the Blackmagic SDK
(repeated or dropped frames, clock drift, HDMI resyncs, and the motion vectors' consumers). LVR =
src/VideoProcessor-Lib/vprenderer/LibplaceboVideoRenderer.cpp. Line numbers are at fdc1fc68.

## What happens today

- **Source rate**: `sourceHz` is the HDMI signal's nominal rate (`DisplayMode::RefreshRateHz`, LVR:13316/13387), not
  the content rate. The extras planner uses `captureRateHz`, measured from capture timestamps (LVR:21713, 18334-18430).
- **23.976 inside 59.94 (Apple TV with match frame rate off)**: there is no pulldown or inverse-telecine detection.
  The ratio is about 1, so the policy returns `IneligibleCadence` and the de-judder is OFF (DejudderPolicy.h:111;
  LVR:11546). The 3:2 judder passes through. With match frame rate on, the signal is 23.976 and the de-judder works.
- **`cadenceRepeat`**: this is a drift-correction re-present of the same queued frame (LVR:22268-22292), and it only
  happens at 1:1 within 200 ppm. It never occurs at 24p on 60 Hz.
- **Pairing**: B is the next queued frame with the same generation and video state (LVR:21581-21604). There is no
  `next == source + 1` check, and the DeckLink missed-frame counter is not used.
  - Lost frames (DeckLink miss, overflow drop, backlog flush, conversion failure) give an A→B motion spanning 2
    periods, warped as 1.
  - Identical repeated content becomes a zero-motion pair (harmless).
- **One missed capture frame** resets `m_measuredFrameRate` (LVR:18348-18364). It is republished after 5 s, so the
  extras hold "timing not known" for about 5 s. The same happens after every Reset or resync.
- **Phase**: it is timestamp-based (DejudderPolicy.h:213-230) and follows slow drift smoothly. 24p on 60 Hz has no
  drop/repeat drift correction.
  - `callbackA` is a software QPC at renderer entry (LVR:18188), not the DeckLink hardware timestamp. 1 ms of jitter
    moves the phase by about 0.024.
  - The latency floor is a 120-frame minimum, which is biased by early outliers.
- **HDMI resync / Reset** (LVR:19238-19277): it starts a new queue generation and resets the rate and telemetry. It does
  not call `ReleaseDejudder`, reset the matcher history, reset the grain map or reset `dejudderAhead`. Correctness
  relies on the generation and sequence checks.

## Vector consumers

| Consumer | Validity check | Stale path? |
|---|---|---|
| Extras warp (field, trust, pixel mask) | `dejudderEstimatedSequence == sourceSequence` (LVR:11821), not a cadence repeat, Active | No |
| Sigma-delta VPVEC/VPPIX | bound only inside an extra; VectorGuard resets on every exit (LVR:12120-12187) | No |
| Occlusion mask | per-slot sequence tag, cleared after use (LVR:11701-11712, 16969-17032) | No (correct pair, wrong spacing over a gap) |
| Rendered-warp cache | keyed by sequence (LVR:11952) | No |
| Adaptive downscale grain map (feeds S1 dither tolerance, LED grain suppression) | the previous frame's map by design; temporally smoothed (AdaptiveDownscale.cpp:616-659) | **Yes**: not reset on cut, resync, Reset or drop |
| Matcher internal history (previous search, hold hysteresis, trust smoothing) | none from the renderer (DejudderWarp.cpp:1915-1939, 2680-2702) | **Yes**: crosses drops, gaps, resyncs, Reset |

There is no single "vectors valid" state; each consumer checks its own key.

## Planned fixes (owner direction: vectors disabled at discontinuities, consumers fall back)

1. **Keep the last good capture rate through a single missed frame** instead of resetting to 0. This removes the 5 s
   holds.
2. **Gap detection**: use the DeckLink frame counter or hardware timestamps, and hold the pair spanning a gap.
3. **Phase from the hardware capture timestamp**, with a robust latency floor (a low percentile, not the minimum).
4. **One "motion valid" epoch**, invalidated at resync, Reset, gap, backlog flush, overflow drop and scene cut. It
   releases or clears the matcher history and the grain map, and every consumer stays on its no-vector fallback until
   a fresh contiguous pair is estimated.
5. **Tests**: synthetic gap, duplicate, missed frame, jitter, drift and mid-stream resync through the capture harness;
   then a replay of real DeckLink recordings (a recorder for the cinema PC).
6. **Later (post-deploy)**: pulldown detection (a content-rate source for both rates).

## Untested today

The de-judder with dropped, gapped or repeated source frames, pulldown, resync or Reset, jitter or drift in PlanExtra,
and the capture-rate discontinuity reset.
