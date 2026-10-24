# ADR-0020: One user-set total delay, the same at every frame rate; 4 frames recommended

- Status: Accepted (refines ADR-0003)
- Date: 2026-10-24 (owner decision)
- Slots: input.queue_timing, output.presentation, every look-ahead consumer

## Context

The barrier scan (B2) showed that 200 ms is unreachable at 24p with 4 queued frames. The
4 frames take 167 ms, and the swapchain depth, render-ahead and processing come on top. The old
rule also rounded the queue UP (200 ms x 23.976 = 5 frames). The audio delay in the AVR is
fixed, so the video delay must never change between titles.

## Decision

1. **One user-set total delay**, set to match the AVR.
   - The RECOMMENDED setting is the design depth of **4 queued frames** at the slowest source
     rate: about 230-250 ms at 24p. It is stated clearly in Config and the docs.
   - Queue depth = floor((total - fixed costs) / frame period), where fixed costs = swapchain
     depth + render-ahead + processing (+ capture transfer where covered).
   - The depth is capped at 4, with a graceful 4 -> 3 -> 2 fallback ladder (a "Fallbacks
     engaged" entry). Every feature keeps working at the lower margin.
   - Below 2 frames the setting is reported as infeasible, never silently exceeded.
2. **The same total at EVERY frame rate.** Faster rates are padded (`DelayBudget::Plan::paddingMs`),
   so the AVR delay never changes between titles.
3. Config shows the resulting frames, the degraded state and the margin for the chosen total.
   The budget stays configurable.
4. The real-time check on the 3090 comes after the owner returns. Until then, laptop per-stage
   GPU timings are scaled by the measured per-stage ratios (VP-pipeline-final.md 3.9).

## Consequences / open

- `DelayBudget.h` implements the rule (`Compute`, `RecommendedBudgetMs`) with tests. Wiring it
  into `QueueLatency.h` / the queue launch follows the fix/r6-baseline queue changes (no
  concurrent edits).
- The delay gate tests the recommended total and lower totals (4/3/2 frames) at
  23.976/24/25/30/50/60p. The measured total must be identical across rates.
