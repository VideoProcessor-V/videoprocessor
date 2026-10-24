# ADR-0006: Motion estimated at source size; vectors delivered per axis; one validity state

- Status: Accepted
- Date: 2026-10-20 (owner decision; recorded 2026-10-24)
- Slots: motion.estimation, motion.vector_delivery, motion.consumers

## Context

Estimating at 4K costs ~4x for no gain; A_motion needs vectors before it runs, so motion cannot use A's output. Single-axis re-base gives different x and y factors.

## Decision

Motion is estimated on the ORIGINAL frame at SOURCE size (NVOF seed + VP matcher; NVOF falls back to the matcher). Vectors are scaled PER AXIS (sx, sy) and mapped through crop/A-lens/NLS geometry; precision consumers get an optional +-1 px local refinement at the processing size. One vectors-valid state, invalidated at discontinuities; every consumer falls back on it.
