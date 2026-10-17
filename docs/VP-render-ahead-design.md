# Render-ahead: de-judder budget per source frame (design, 2026-10-17)

Owner (top priority): processing cost should follow the source frame, as in madVR, not the
monitor's refresh rate. The laptop (RTX 3050 Ti) represents the GPU many users have. Not built yet.
HEAD line numbers are from LibplaceboVideoRenderer.cpp unless stated.

## Summary

De-judder holds on the laptop because of how the code is structured, not because the GPU is too
slow.

- **Today:** each extra is presented one at a time, and each waits for the previous picture to
  reach the screen. The next new frame B starts only after the last extra's vblank, so B's whole
  chain (25-29 ms) must fit in one refresh. At 60 Hz that is 12.7 ms.
- **Fix: present ahead, as madVR does.**
  - Queue A's extras straight after A, at phases planned from the film clock.
  - Start B's render at once, so B renders while A and its extras are on screen.
  - The budget becomes GPU work per source period: one new frame plus about 1.5 extras in 41.7 ms
    at 24p, instead of one new frame in 16.7 ms.
  - B's render path is unchanged.
  - Cost: about one source period of extra display latency. It is reported, and can optionally be
    taken back from the input queue.

**Laptop estimate (24p in 30p, 4K, 60 Hz):**

| Settings | GPU work per 5-refresh cycle (83.3 ms) | Busy | Result |
|---|---|---|---|
| Today's settings | about 2x27 + 3x7 = 75 ms | about 90% | the gate keeps 2 of 3 extras |
| With the laptop budget savings | about 61-65 ms | 73-78% | full de-judder, with margin |

## Why the gate exists

- **The gate:** 13718-13722 holds when `workMs + 4 > display period`. The comment explains that
  B's pipeline starts only at the last extra's vblank.
- **The cause: extras are synchronous and locked to vsync.** The loop at 13865-13883 calls
  WaitForVBlank, then GetFrameStatistics, before it plans and presents the next extra
  (DejudderPolicy::PlanExtra, DejudderPolicy.h:292-310).
  - It stops at NextFrameNear or NextFrameArrived (25894).
  - At most one extra is ever queued.
  - RenderLoop then dequeues B and runs the whole of RenderLocked (25851).
- **B is already available.** The queue holds about 200 ms. B's planes are already uploaded for
  the estimate (25503-25510), and C is there for estimate-ahead.
- **Swapchain:** 3 buffers (8215), maximum frame latency 2 (9057), Present(1,0) FIFO.
- **Precedent:** Stage B interpolation already presents frames back to back without vblank waits
  (25832, 25847).

## Phase 1: queued extras

1. **Pure planner `RenderAhead::PlanFrame`** (new file RenderAheadPolicy.h). It plans all of A's
   refreshes and their phases from:
   - the film clock and P_s;
   - the measured display rate (TimelineDisplayHz);
   - the predicted vsync of the next present (from refresh counts plus queued presents, or
     open-loop without statistics).

   It reuses InterpolationTimeline's smoothing and queue trim.
2. **PresentDejudderExtras with `render_ahead`:**
   - no vblank wait loop;
   - warp, render and Present(1,0) each planned extra back to back (FIFO back-pressure);
   - EstimateAhead after the first extra;
   - return at once;
   - keep the present-id bookkeeping, so drift correction still sees one present per frame.
3. **Depth.**
   - VP-owned swapchain buffer count ceil(max ratio)+2, capped at 6.
   - SetMaximumFrameLatency(3), only in this mode.
   - About +66 MB of video memory at 4K.
   - The libplacebo-owned swapchain path keeps today's behaviour.
4. **Lateness feedback:** a late next frame drops one extra on the following frame. The existing
   queue trim stops drift.
5. **Order-sensitive features are unchanged**, because submit order is display order:
   - sigma-delta per present;
   - subtitles;
   - DV and the extended-range late restore (they stay on B's main path).
6. **LED:** call `BeginFrameAt(sequence, predictedDisplayMs)` instead of the wall clock (19806),
   so the gain and projector drive match B's display time.
7. **Latency and audio:** add the planned lead to the presentation target lead (26405-26420).
   Optional `render_ahead_queue_credit` lowers the queue depth by the lead.

## Phase 2 (only if needed): offscreen render-ahead

- A two-slot cache; render B's chain offscreen early, then present it with the OutputOnly pass.
- Only worth doing if Phase 1 shows backbuffer stalls.

## Gate redesign: `dejudder_gate: period | refresh`

- **Measure:** GPU p95 per slot type over 5 s (new frame N, extra X, estimate-ahead E).
- **Average check:** Σ(N + E + extras×X) + margin ≤ u × cycle, with u = `render_ahead_utilisation`
  0.85.
- **Single-frame check:** N + X×n ≤ (n + Q − 1)×P_d.
- **Degrade ladder, one step at a time:**

  | Level | What runs |
  |---|---|
  | L0 | Full |
  | L1 | Cheaper extras: de-judder vectors for the sigma-delta only |
  | L2 | Drop the extra with the smallest s·δ |
  | L3 and on | Drop the next extra, and so on |
  | Hold | 3:2 |

- **No automatic precision drop (owner 2026-10-17):** the ladder never lowers precision. The
  higher-precision cache is the default; a 16-bit cache is only a profile choice, never a
  ladder step.
- **Hysteresis:**
  - step down after 2 s over budget, or at once on a late frame;
  - step up after 5 s under u − 0.10, and no sooner than 10 s after the last step down;
  - log every change.

## The owner's model check

| Case | Renders per source frame |
|---|---|
| Plain 30p on 60 Hz | 1 (the flip model holds it for the second refresh) |
| 24p (native or pulldown) on 60 Hz | 1, plus about 1.5 extras with de-judder |
| 25p on 50 Hz | 1 (plus 1 extra only with even strength) |
| 50p/60p on 50/60 Hz | 1 per refresh, because that is the source period |

Full-chain cost per refresh exists only for Stage B interpolation, smooth motion, and
drift-correction repeats.

## Switches

| Switch | Default |
|---|---|
| `render_ahead` | off, until proven, then on |
| `dejudder_gate` | period (`refresh` restores the old rule) |
| `render_ahead_utilisation` | 0.85 |
| `render_ahead_queue_credit` | off |

## Tests

- **CPU:**
  - planner refresh counts and phases for 24/25/30/48 on 30/50/60/120 Hz, including 1000/1001
    rates;
  - lateness;
  - open-loop operation;
  - gate budgets, ladder order and hysteresis;
  - a FIFO scheduler simulation with ±30% jitter: no late frames at 85%; `refresh` mode unchanged.
- **On-screen (Claude, on the GPU):**
  - OnScreenLiveExtrasAndEstimateAhead with render_ahead, at 24p in 30p and true 24p on 60 Hz;
  - laptop external port and internal panel;
  - the 3090 as a regression check;
  - video memory usage.

## Risks and effort

- **Risks:**
  - the added latency must be reported correctly for audio;
  - telemetry baselines change;
  - the internal panel has no DXGI statistics (Phase 1 runs open-loop there and may unlock it,
    but late frames cannot be detected);
  - clock swings, which the ladder absorbs.
- **Effort:** Phase 1 3-4 days, gate and ladder 2 days, tests 2 days.

## As built (2026-10-17, feat/render-ahead; not yet judged on screen)

- **Planner:** `vprenderer/RenderAheadPolicy.h` (pure). A owns the refreshes from its predicted
  first vsync while (V - film time - L - lead) / P_s < 1. Phase <= 0 (a restart's lead) holds A.
  A refresh A owns but does not render is a *hold*: the next present (the next extra, or B's
  own present) gets SyncInterval 1 + holds (DXGI limit 4), so holds cost no GPU work. This
  relies on DXGI's rule that a present with interval n is shown no sooner than n vblanks after
  the previous one; the on-screen runs must confirm it.
- **Lead:** `MaxExtras(P_s/P_d) x P_d` (the extras' span). The latency floor L is recorded
  without the lead on chained frames and raw on anchors, so it cannot run away.
- **Schedule:** predicted vsyncs chain from the last plan; closed loop on
  `GetFrameStatistics` (no waits): a present shown > P_d/2 after its plan is late, the chain
  moves and the next frame drops one extra. Without statistics it runs open loop.
- **Gate:** `dejudder_gate auto|period|refresh` (auto = period with render_ahead; period
  without render_ahead acts as refresh). p95 over 5 s of N (frame GPU timer), X and E (two
  extra GPU timers on alternate extras, libplacebo pass timers suspended so disjoint
  intervals never nest; CPU timings when the GPU timer is unsupported). Ladder: L0, L1 (only
  when `dither_vectors own`), drop 1..n (smallest s*delta), hold. No automatic precision drop.
- **Swapchain:** VP-owned chain only: buffers ceil(ratio)+2 (3..6; 6 before the rates are
  measured), SetMaximumFrameLatency(3); otherwise 2 as before. The libplacebo-owned chain keeps
  3 buffers / latency 2 (queued mode still works there, with less depth).
- **Audio:** A's presentation target lead becomes its planned refresh minus Present's return.
  `render_ahead_queue_credit on` lowers the dequeue depth by ceil(lead / P_s) (never below 2).
- **LED:** `LedDriveModule::BeginFrameAhead` uses the chain's predicted refresh; sends still land
  on the wall clock (led_landing_ms).
- **Also in this branch:** hiccup repair without motion smoothing (the timeline presents a gap
  pair alone; the matcher is kept while `interpolation_fill_gaps` is on), and the queued-frame
  walk extended to the LED's `led_lookahead_ms` depth (the extra frames feed the LED only).

## Open loop without statistics (follow-up 2026-10-17)

- **Found on screen:** on the laptop's internal panel (Intel iGPU, no DXGI frame statistics)
  every render-ahead case held with "timing not known: display rate" before the planner ran, and
  the on-screen tests passed vacuously.
- **Rate source** (`vprenderer/OpenLoopDisplayRate.h`, pure): the statistics' measured rate
  always wins (closed loop). Without it, and only with `render_ahead on`, the first available of:
  the window's monitor mode as an exact rational (QueryDisplayConfig target mode), DWM's
  composition period (DwmGetCompositionTimingInfo, skipped when it disagrees with the monitor's
  mode by more than 2%: another monitor's rate), the application's display rate
  (SetRenderLoadDisplayRefreshRate), the whole-hertz mode (EnumDisplaySettings), and last a short
  vblank cadence (median QPC interval of ~9 WaitForVBlank returns, at most once per 10 s). The
  system candidates are re-read every 2 s. One line logs the source and every candidate when it
  changes ("Render-ahead: open loop, display rate ... from ..."); the 5 s "Render-ahead:" line
  ends with the rate, its source, open/closed loop, open-loop frames and presents late by QPC.
  `render_ahead off` keeps the old hold (dejudder_gate refresh / legacy unchanged).
- **Open-loop schedule:** no closed-loop correction. A restart anchors on DWM's last vblank when
  DWM composes at the planned rate and the vblank is recent (otherwise on the present time, as
  before). Lateness is weaker: an extra whose Present returned more than one refresh after its
  planned vsync is counted late (one refresh, not half: the open-loop grid may be a refresh off);
  the chain moves by those refreshes and the next frame drops one extra and feeds the gate.
- **DWM-composed window vs independent flip:** in a composed window DWM shows the latest queued
  flip-model present at each composition, and DXGI still holds a present made with SyncInterval
  n for n vblanks, so the holds and queued extras keep their meaning; composition adds about one
  refresh of latency and the panel's DWM rate is the rate that matters (hence DWM's vblank as the
  grid). Fullscreen independent flip uses the same queue without composition. This is the
  documented DXGI behaviour, not yet confirmed on screen: OnScreenRenderAheadExtras reports
  windowed and fullscreen passes separately (VP_TEST_HARNESS_FULLSCREEN=1|both). Panels with
  dynamic refresh (Windows DRR, Intel DRRS) can change rate: the candidates are re-read every 2 s
  and the DWM rate per frame.
- **Bounds:** the DWM and display-rate estimator rates were accepted only in 20..120 Hz; they now
  use `DisplayRefreshBounds` (DisplayMode's 10 Hz floor to 500 Hz). Still fixed elsewhere (flagged):
  DisplayRefreshRatePolicy's 10..240 Hz, the GUI's startup-rate and nominal-rate 240 Hz caps.
- **Tests:** CPU RenderAheadOpenLoopRateTests. On screen, OnScreenRenderAheadExtras is strict
  (render-ahead lines, gate level, queued extras or a gate reason, no display-rate holds, no
  timing-hold storm); OnScreenLiveExtrasAndEstimateAhead marks a run without statistics
  SKIP-NO-DISPLAY-TIMING (inconclusive), which fails run_chain_gate.ps1 -OnScreen.
