<!-- Copyright(C) 2026 VP-V contributors -->
<!-- SPDX-License-Identifier: GPL-3.0-only -->
# Composed vectors: per-pixel chain of the engine's adjacent pairs (ENGINE-COMPOSED-VECTORS)

Unit: motion/estimation (sub-folder `composed/`, submodule of slot `motion.estimation`). Skeleton product `composed_vectors` at the FIXED id 34 (`kComposedVectors`, owner R14-95 fixed id table, `core/Products.h`), made by `VpComposed::Product` (the planes). The id is still in `kReservedProductIds` (an absent registry slot, no catalog row) until its registry decl (lane product, Cpu reference, T1, history 8, input P2 lag 0, a per-frame handle) and its reader row land (lead ruling SHOULD S3). Readers: P10g multi-frame grain level and TemporalDetail's neighbour warp (owner 2026-11-14, option B weak texture; lead12\proof-weaktex-BC.md s11; register row ENGINE-COMPOSED-VECTORS).

## Settings

| Key | Section | Type | Range / values | Default | Config placement | Original (Bill) | Fallbacks / status | Tiers |
|---|---|---|---|---|---|---|---|---|
| (none) | | | | | | | | |

No config key is read. Code defaults (hand-checked): depth 8 back + 4 ahead (TD up to `VpSkeleton::kAheadCeiling` = 6), low latency 10 back + 0 ahead, back ceiling 16, landing tolerance `consistencyPx = 1.0` source px (trust 1 up to it, 0 from twice it), `noReverseTrust = 0.5`, byte budget unbounded.

## Form / step contract (owner decision R14-16)

The product form is a BLOCK field, read through the shared plane sampler. A plane is ALWAYS a raster of `step` source px per cell holding displacement (dx, dy, source px), trust 0..1 and uncertainty (px), per (frame, direction, offset k). The default step is `kBlockStep` = 8 (4K = 480 x 270 cells). Readers (P10g, TemporalDetail) read a plane through `Plane::Warp(x, y)` (the chain samples the adjacent pair fields with `SampleField`, `Product::Value` goes through `Plane::Warp`; `ComposeStep` is the producer: it samples the engine's pair fields, never a plane through a reader path) (landing position, trust, uncertainty, `valid`; bilinear `Plane::At` underneath). A reader may ask for a finer step (`SetReaderStep`; step 1 = per pixel, a CPU reference form); the finest active step is computed (one product, step is a variant axis). A GPU form of the block field must publish the same fields at the same step (102b ECV-GPU-FORM, closed by R14-16).

## Samplers (one each, rule 1)

* Vector-field sampler: `BilinearTap` (`ComposedVectors.h`), the one bilinear tap of the dx / dy / trust / uncertainty rasters, used by `Plane::At` and by the producer's `SampleField`. `Plane::Warp` gives the landing.
* Image warp sampler: `SampleBicubic` (`WarpSampler.h`, Catmull-Rom, 16 taps, false when a tap falls outside the frame; no clamped taps) resamples a neighbour IMAGE plane at a landing; `Plane::SampleLanded(img, x, y, out)` = `Warp` + `SampleBicubic`. Owner 2026-11-14 16:43 item 4: "shared neighbour-warp sampler (ECV) = bicubic" (citing S-T1 Q1, which compared bilinear and bicubic warps of the neighbour image). Every reader (P10g grain level, TemporalDetail) warps neighbour images through it; no private warp or bicubic.
* A landing that is not valid (trust 0, or outside the frame) keeps the dx / dy the chain held: a reader of dx / dy must check trust or `Warped::valid` first. `Product::Value` reports it as trust 0.

## Degradations (never silent, rule 25)

* Pair absent or stale: the direction ends at the last reached offset (`Reached`, `Status`, `FallbackCatalog::ComposedVectors`).
* Reverse pair absent (the newest queued frame): the step runs on the pair trust x `noReverseTrust`, uncertainty +2 x `consistencyPx`, plane flagged `degraded`, served and not cached.
* Beyond the plan (no reader, low latency ahead, queue, ceiling): `Got::OutOfPlan`, counted; the plan's own `note` says what was asked and what was allowed.
* Byte budget (`Config::budgetBytes`): a plane that would exceed it is refused, `Got::Budget`, counted (`Stats.budgetRefused`); the direction ends there (`Reached`, `Status`).
* Epoch cause: `Invalidate()` drops every plane.

## Not built here

The `IPairSource` adapter over the P2 slot / CellTrust memo / MotionValidity (mirror of `repair/temporal_detail/SharedMotionSource.h`) and the `Invalidate()` call at each epoch cause belong to the first reader lane (register row ECV-ADAPTER-WIRING). Provenance of the uncertainty rule: `repair/temporal_detail/TemporalDetailModel.h` (composed uncertainty, per-pixel chain carries it without the resample).
