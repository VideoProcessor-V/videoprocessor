# Temporal-detail products (reserved): `repair/temporal_detail/products`

Shared unit **U13** (`TemporalDetailProducts`, `TdProducts::kUnitVersion = 1`) | status: **draft, reserved** (R1-W1-RES, owner 2026-11-06).
The engine itself (doc 67v2) is built in reset 2. Reset 1 reserves its **products** so that the ids, layouts, fill rules and unit
version are fixed before any consumer is wired, and wires the consumers' **read paths** now, so reset 2 only fills producers.

## What exists now

| Piece | Where |
|---|---|
| 15 registry entries `td.*` (ids appended after P14, before `kSkeletonProductEnd`) | `core/Products.h` (`AppendTemporalDetailReservedProducts`) |
| catalog rows (unit path, unit version 1, "reserved" producer) and 7 consumer rows | `core/ProductCatalog.h`, generated `docs/design/product-graph.md` |
| contract table, fill rules, VRAM budget per tier | `TdProductContracts.h` |
| the one read API, the placeholder producer, `kEngineRequestedToday = false` | `TdProductReads.h` |
| fallback ids `product.td_scaler`, `td_dither`, `td_chroma`, `td_engine` | `core/FallbackCatalog.h` |
| read paths wired | adaptive scaling (`scaling/output/AdaptiveDownscaleStage`), dither need (`output/dither/DitherNeed.h`, `FrameArgs::td`, `Decision::td`), chroma (`ChromaCfl.h` `Result::td`, `chroma_upscale/basic/BasicChroma.h`) |

## Products, fill rule, tier, declared size per slot

Sizes are the registry's planning rule (`BytesOf`); block maps at 8 px (shift 3) or 16 px (shift 4), half-resolution maps (shift 1),
the low-res base at 1/4 per axis (shift 2) with Cb/Cr 4:2:0 at 1/8 per axis (shift 3), 2 B per sample (R16).

| id | product | fill rule | tier | layout |
|---|---|---|---|---|
| 0 | `td.quality_pattern` | held record (from the source identity record, 68v2) | T0 | 256 B system RAM |
| 1 | `td.trail_confidence` | engine map | T1 | R16F / 8 px block |
| 2 | `td.residual` | engine map | T1 | R16F half resolution |
| 3 | `td.validity` | engine map | T1 | R8 / 8 px block |
| 4 | `td.real_detail` | engine map | T1 | R8 half resolution |
| 5 | `td.grain_level` | engine map (own grain vs coding noise) | T1 | RG16F / 16 px block |
| 6 | `td.stability` | engine map | T1 | R8 / 8 px block |
| 7 | `td.lighting` | engine map | T1 | RGBA16F / 16 px block |
| 8 | `td.drift` (I-to-I) | engine map | T2 | RG16F / 16 px block |
| 9 | `td.motion_model` | held record (camera/object, near-I calibrated) | T0 | 4 KB system RAM |
| 10 | `td.mirror` (reference-set mirror) | held record | T0 | 2 KB system RAM |
| 11 | `td.base_long` | anchor slot, 2 slots (repaired I, previous I) | T1 | R16 1/4 axis + 2 x R16 chroma 1/8 axis |
| 12 | `td.base_short` | anchor slot, 6 slots (one reference per mini-GOP) | T1 | same as long |
| 13 | `td.refined_vectors` (DMVR-style) | engine map | T1 | 8 B / 8 px block |
| 14 | `td.refine_pixel` (BDOF/PROF-style) | optional map: **store vs recompute TBD by measurement** | T2 | RG16F half resolution (store worst case) |

(ids are offsets from `kTdQualityPattern`.) Fill rules: a **held record** is published by the engine's worker and read at lag >= 1 (no live
fill, the P14 rule); an **engine map** is filled by the engine in the render (live-fill owner: "temporal detail"); an **anchor slot** is
filled by a GPU-side copy when an anchor arrives (slots are the history); the **optional map** is stored or recomputed at the point
of use, decided by measurement (the declared size is the store worst case, 0 for recompute).

## VRAM budget per tier (stored slots counted, one slot of per-frame maps; ring multipliers come from the graph)

| frame | T0 GPU | T1 GPU | T2 GPU | system RAM (records) |
|---|---|---|---|---|
| 1920x1080 | 0 | 5.2 MB | 2.1 MB | 6.4 KB |
| 3840x2160 | 0 | 20.6 MB | 8.4 MB | 6.4 KB |
| 7680x4320 | 0 | 82.4 MB | 33.7 MB | 6.4 KB |

The base buffers dominate T1 (8 pictures: 1.56 MB per slot at 4K). `TdProducts::BudgetAt` computes this table from the registry (a test pins
the hand-summed figures); the full-graph VRAM with every consumer active is in `docs/design/product-graph.md`.

## The read rule (`TdProducts::Read`)

- engine **not requested** (today: always, no configuration key exists): not a fallback, the classical form runs, byte-identical, no status line;
- **requested** and a product the consumer needs is absent / stale / of another unit version, or there is no engine: the classical form runs
  **visibly** (the `product.td_*` Decision is engaged, with the reason; consumers add the status line); it recovers by itself;
- every needed product present and fresh: `usable` (reset 2 uses it).

Consumers' sets (a test compares them with the catalog rows): scaler = trail confidence, real detail, grain level, stability, motion model;
dither need = trail confidence, grain level, stability, motion model; chroma = base long/short (+chroma planes), mirror, refined vectors,
trail confidence, real detail. The engine rows (family "temporal detail", repair 13-15) own the live fill of the engine maps.

## Deferred

- No configuration key now (SCHEMA lock held by W1-P3). Reset 2 adds the key that flips `kEngineRequestedToday` to a setting.
- The sigma-delta amplitude read (`product.td_dither` also names it) is wired through dither need's `Decision::td` only.
- Producers, and the per-region confidence product (doc 67v2 section 13 lists it; it folds into `td.validity` + `td.trail_confidence` until measured).
