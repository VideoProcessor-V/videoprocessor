# ADR-0005: Entrance order: re-base first (PENDING the round-2 A diagnostic)

- Status: Proposed (pending measurement)
- Date: 2026-10-24 (owner decision; recorded 2026-10-24)
- Slots: scaling.rebase, repair.encoding, repair.deband, repair.texture_restore

## Context

The kernel-matched inverse relies on the delivered frame being an exact kernel upscale. Texture restore, breathing and deband add or move fine-band energy that folds into aliasing on inversion. Codec damage lives on the encoded grid, and repair there is ~3x cheaper (A 1080 1.4 ms vs 4K 4.4 ms).

## Decision

PROPOSED: re-base on the untouched delivered frame, THEN repair at the encoded resolution, then deband, then the texture-restore add-on. Native (no re-base) sources are repaired at native size. Round 2 measures BOTH orders on the existing data; the owner decides from the results. The skeleton keeps the order as data so the swap is a one-line change.

## Consequences / open

Pending. A single-axis re-base still lands on the full encoded grid (the unscaled axis is native), so A's domain stays consistent; the state flag (re-based / native / un-reversed) covers imperfect cases.
