# ADR-0003: Fixed known delay: 200 ms target, 250 ms margin, built for 4 queued frames

- Status: Accepted
- Date: 2026-10-20 (owner decision; recorded 2026-10-24)
- Slots: input.queue_timing, output.presentation, motion.estimation

## Context

Audio is delayed separately by a fixed amount in the AVR, so VP's end-to-end video delay must be exactly constant in every mode. 24p is the main use case; a 5-frame queue alone is 208 ms at 24p.

## Decision

200 ms is the design target for every mode/tier/feature; 250 ms is the selectable margin. Build for 4 queued frames with a graceful fallback at 2-3 frames (each look-ahead feature degrades, never waits, never adds delay). Queue depth = the deepest single look-ahead + the hiccup spare; features share the queue. Time-based look-aheads (motion ahead, LED latency, latch) in ms. Render-ahead is counted inside the budget. The forced 5-frame minimum is removed.

## Consequences / open

The delay gate (24p at 200 and 250 ms with 4/3/2 frames; repeated at 25/30/50/60p and with nets/tiers on) is part of done for every feature.
