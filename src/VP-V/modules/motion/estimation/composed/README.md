<!-- Copyright(C) 2026 VP-V contributors -->
<!-- SPDX-License-Identifier: GPL-3.0-only -->
# Composed vectors: per-pixel chain of the engine's adjacent pairs (ENGINE-COMPOSED-VECTORS)

Unit: motion/estimation (sub-folder `composed/`, submodule of slot `motion.estimation`). Skeleton product `composed_vectors` (`kComposedVectors`, lane product, Cpu reference, T1, history 8, input P2 lag 0), made by `VpComposed::Product` (the planes) with a per-frame handle in the registry. Readers: P10g multi-frame grain level and TemporalDetail's neighbour warp (owner 2026-11-14, option B weak texture; lead12\proof-weaktex-BC.md s11; register row ENGINE-COMPOSED-VECTORS).

## Settings

| Key | Section | Type | Range / values | Default | Config placement | Original (Bill) | Fallbacks / status | Tiers |
|---|---|---|---|---|---|---|---|---|
| (none) | | | | | | | | |

No config key is read. Code defaults (hand-checked): depth 8 back + 4 ahead (TD up to `VpSkeleton::kAheadCeiling` = 6), low latency 10 back + 0 ahead, back ceiling 16, landing tolerance `consistencyPx = 1.0` source px (trust 1 up to it, 0 from twice it), `noReverseTrust = 0.5`, byte budget unbounded.

## Form / step contract

A plane is ALWAYS a raster of `step` source px per cell holding displacement (dx, dy, source px), trust 0..1 and uncertainty (px), per (frame, direction, offset k). Step 1 = per pixel; the finest step any active reader declares is computed (one product, step is a variant axis); coarser readers sample it with `Plane::At` (bilinear). A GPU form (register row ECV-GPU-FORM) must publish the same fields at the same step. The CPU form at step 1 is a reference only (4K: 14 planes x 8.3 M px per frame).

## Degradations (never silent, rule 25)

* Pair absent or stale: the direction ends at the last reached offset (`Reached`, `Status`, `FallbackCatalog::ComposedVectors`).
* Reverse pair absent (the newest queued frame): the step runs on the pair trust x `noReverseTrust`, uncertainty +2 x `consistencyPx`, plane flagged `degraded`, served and not cached.
* Beyond the plan (no reader, low latency ahead, queue, ceiling): `Got::OutOfPlan`, counted; the plan's own `note` says what was asked and what was allowed.
* Epoch cause: `Invalidate()` drops every plane.

## Not built here

The `IPairSource` adapter over the P2 slot / CellTrust memo / MotionValidity (mirror of `repair/temporal_detail/SharedMotionSource.h`) and the `Invalidate()` call at each epoch cause belong to the first reader lane (register row ECV-ADAPTER-WIRING). Provenance of the uncertainty rule: `repair/temporal_detail/TemporalDetailModel.h` (composed uncertainty, per-pixel chain carries it without the resample).
