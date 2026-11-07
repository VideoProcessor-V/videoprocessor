# Product graph

The product graph of the VP-V skeleton: what is made once, who makes it, who reads it, what each reader does when it is missing, and what it costs in VRAM. The tables between the generated markers are rewritten by a test; everything above and below them is hand-written (SA0, doc 51).

## How to read

The generated region holds three tables:

- **Products.** One row per declared product: id, name, backend, history depth, tier, residency, producing unit and unit version, lane producer, live-fill owner and consumers.
- **Consumers.** One row per consumer, in pathway order (input, motion, repair, scaling, colour, drive, output; inside a unit, render order), with the products it reads and its fallback (kind and FallbackCatalog `product.*` id).
- **VRAM per tier.** The declared bytes of the products for each tier at 1080p, 4K and 8K. Decimal MB. These are declared/estimated, not measured.

The declaration is split in three files, each with one job:

| File | Job |
|---|---|
| `src/VP-V/core/Products.h` | The registry. Declares what a product IS: id, inputs with lag or lead, backend, history, size rule, tier, residency, honoured stamps |
| `src/VP-V/core/ProductGraph.h` | The graph. Builds one node per product that the active consumers need (closure, topological, deterministic), ring and live-pool counts, rebuild triggers, reductions |
| `src/VP-V/core/ProductCatalog.h` | The catalog. Adds per product: producing unit path, unit version, lane producer, live-fill owner. Adds per consumer: module, pathway unit and order, fallback kind and FallbackCatalog id |

To regenerate the region, run `ProductCatalogTests` with the environment variable `VP_WRITE_PRODUCT_GRAPH_DOC=1`. Without the variable the same test compares the region with the catalog and fails on any difference, so the file cannot drift. Never edit the region or the two marker lines by hand.

## Reading the VRAM table on a 4 GB card

The generated VRAM table is the worst case with every consumer active. A 4 GB laptop (RTX 3050 Ti) at 4K does NOT fit it
(3.7 GB of products alone, before the render, the swapchain and libplacebo). The VRAM governor can shed tiers (T2 first,
then T1) but cannot make that set fit: the laptop needs PROFILE-LEVEL consumer disabling (temporal detail and chroma
from luma off, as today) plus the compact grain split (SA2) and the compact reference store (SA6). The governor
makes any remaining cutback visible; it is not a substitute for choosing the consumer set per card.

## Owner rules

1. **Store only shared heavy precursors.** Per-consumer maps (strength maps, evidence gates, clean and G, a sigma that differs from the split sigma, camera accessor vectors, per-pixel light level) are derived on the fly by the reader and never stored.
2. **One Indicator.** Cut, fade, resync and validity are read from the Indicator. No consumer keeps a private reset or its own cut detector.
3. **Ancestry per slot.** Every derived slot records the ancestors (product, sequence, generation) it was made from. A reader refuses a slot whose ancestry does not match, and engages its fallback.
4. **The fill rule.** Never two producers for one slot of one frame. The lane makes the product when it can. When the lane missed the frame, the live-fill owner makes it. The live-fill owner is the first consumer in pathway order that reads the product.
5. **Visible graceful fallbacks.** Every consumer declares a fallback (own reduced form, lower tier or off) with a FallbackCatalog `product.*` id. An automatic downgrade shows in Ctrl+I, in Config status and as a one-time notice; it is never log-only.
6. **Off is byte-identical.** A consumer or product that is off adds no pass, no texture and no change to the picture.
7. **No private full-resolution textures outside the allocator (SA1).** Full-resolution GPU products and history allocate through the product slots and the VRAM governor. An exception needs a README justification.
8. **VRAM stated for every package.** Each package states its 1080p, 4K and 8K VRAM from the declared size rules, and passes it through the governor.

## Notes (hand-written; the generated region below is rewritten by ProductCatalogTests)

- Doc 72 (R1-W1-RES round 2): ids 16-18 are td.blocks / td.memory / td.anchor_pass (TemporalDetail unit v2); p7g_camera_guide is registered as id 20; the engine products coding_evidence, quality_pattern (+ causal), refined_field, td.anchor_refs, td.delta, td.maps (U19 TdMapsView) and td.trail_summary follow in that order. Sizes are the budget-governed FLOOR (stage 0); the larger stages are in `modules/repair/temporal_detail/products/TdProductContracts.h` (no fixed per-GPU tiers; the VRAM / ms governor decides).
- Repair-stack order (WP9): temporal detail -> deband -> contrast recovery. Deband is the fallback layer under temporal detail (it reads td.maps validity / stability and coding_evidence); today's text was deband -> TD -> CR.
- The framework limit is kMaxInputs = 16 / kMaxConsumes = 10, but the registry still declares only the first 4 inputs of each intended list (`kDeclaredInputsToday`); the full lists are in the contract table, nothing is dropped. Reset-2 open item: the declarations catch up (short-queue substitutes for the lead edges).
- The VRAM table below is NOT comparable with doc 72's numbers (floor 54.95 MB GPU, full 241.6 MB at 4K; `TdProducts::BudgetAt`). Doc 72 sums the DECLARED size of each engine product once (one slot, at its governed stage), counts `td.anchor_pass` as Transient (the shared arena's peak, a maximum, not a sum) and adds no ring or spare. This table is the whole graph's worst case: every node's ring (history + 1 + renders in flight + lane lead) plus its live pool, which includes one spare per input reference a lane reader holds (`laneReadRefs`), with every consumer active at `laneLead 3`; a Transient product is not in a tier's ring or live bytes. The two differ by those multipliers and spares and by the other products of the graph, so neither number is the other's total.
- `td.blocks` over-declares its 8 px quadtree pool on purpose: the registry has no "pool fraction" term, so the pool is declared as 6 B at EVERY 8 px cell (100%), while the contract says the pool holds at most 25% of the 8 px cells (about 6 B per 16 px block). The surplus is about 18 B per 16 px block (0.6 MB at 4K) and only makes the declared budget safe (declared >= worst case); the allocator is never short. It is recorded here and in `Products.h` rather than hidden.

<!-- BEGIN GENERATED product-graph -->

Generated by `ProductCatalogTests` from `core/ProductCatalog.h` and `core/Products.h`. Do not edit by hand: run the test with `VP_WRITE_PRODUCT_GRAPH_DOC=1` to rewrite it. Sizes are decimal MB for ONE slot (declared sizes, not measured).

### Products

| id | token | unit (version) | backend | lane producer | live-fill owner | consumers | stamps | tier | residency | 1080p MB | 4K MB | 8K MB |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 0 | p0_rebase_decision | scaling/rebase (v1) | render | none | entrance stages | entrance stages | full,rebaseH,rebaseV,settings.rebase | T0 | vram | 0.0 | 0.0 | 0.0 |
| 1 | p1_half_luma | input/source_analysis/lane (v1) | cpu | HalfLumaProducer | optical-flow producer | optical-flow producer, matcher / motion warp | full | T0 | vram | 1.3 | 5.2 | 20.7 |
| 2 | p2_vector_pair | motion/estimation (v1) | ofa | NVOF producer (MotionAheadProducer) | optical-flow producer | optical-flow producer, entrance stages, matcher / motion warp, source monitor pan, motion intent, interpolator / hiccup repair, de-judder extras, occlusion mask, grain map, temporal detail, detail pulse, texture restore, debander (with hold), residual stabilise, sigma-delta (analysis field mapped) | full,continuity,settings.motion | T0 | vram | 0.3 | 1.3 | 5.2 |
| 3 | p2m_matcher_pair | motion/estimation (v1) | compute | matcher (DejudderWarp estimate) | none | none | full,continuity,settings.motion | T0 | vram | 0.3 | 1.3 | 5.2 |
| 4 | p3_analysis_field | motion/estimation (v1) | compute | analysis field pass | entrance stages | entrance stages, interpolator / hiccup repair, grain map, temporal detail, detail pulse, texture restore, debander (with hold), residual stabilise, sigma-delta (analysis field mapped) | full,continuity,settings.motion,settings.analysis | T0 | vram | 0.5 | 2.1 | 8.3 |
| 5 | p4_source_stats | input/source_analysis/lane (v1) | cpu | SourceStatsProducer | source resolution monitor | source resolution monitor, source identification, HDR inference, matcher / motion warp, detail pulse, texture restore, adaptive downscale (grain map), LED look-ahead | full,continuity,settings.analysis | T0 | system RAM | 0.0 | 0.0 | 0.0 |
| 6 | p5_monitor_tiles | input/source_analysis (v1) | compute | tile pass + async readback | source resolution monitor | source resolution monitor, source identification | full,settings.analysis | T1 | vram | 4.2 | 4.2 | 4.2 |
| 7 | p6_entrance_ahead | scaling/rebase (v1) | compute | entrance B-ahead | entrance stages | entrance stages | full,continuity,rebaseH,rebaseV,settings.rebase,settings.analysis | T1 | vram | 16.6 | 66.4 | 265.4 |
| 8 | p7_camera_fit | motion/estimation/global_model (v1) | cpu | camera fit (CPU worker) | temporal detail | camera measurement, temporal detail, detail pulse, texture restore, camera diagnostics | full,continuity,settings.motion | T0 | system RAM | 0.0 | 0.0 | 0.0 |
| 9 | p8_camera_labels | motion/estimation/global_model (v1) | compute | camera label pass | occlusion mask | occlusion mask, grain map, temporal detail, detail pulse, texture restore, debander (with hold), residual stabilise, sigma-delta (analysis field mapped), camera diagnostics | full,continuity | T0 | vram | 0.1 | 0.5 | 2.1 |
| 10 | p9_camera_smoothed | motion/estimation/global_model (v1) | cpu | camera smoothing (CPU) | camera object split | camera object split, de-judder extras, interpolator / hiccup repair, temporal detail, detail pulse, texture restore, camera roll-off | full,continuity,settings.motion | T1 | system RAM | 0.0 | 0.0 | 0.0 |
| 11 | grain.split | repair/grain_split (v2) | compute | grain split pass | region evidence (derived maps) | region evidence (derived maps), interpolator / hiccup repair, temporal detail, detail pulse, texture restore, debander (with hold), chroma from luma, adaptive downscale (grain map) | full,continuity,settings.analysis | T0 | vram | 33.3 | 133.2 | 532.9 |
| 12 | grain.split.chroma | repair/grain_split (v2) | compute | grain split chroma pass | chroma from luma | chroma from luma | full,continuity,settings.analysis | T1 | vram | 16.7 | 66.6 | 266.5 |
| 13 | region_block_stats | input/source_analysis/regions (v2) | compute | region block statistics pass | region evidence (derived maps) | region evidence (derived maps), temporal detail, detail pulse, texture restore, debander (with hold), light-level users, adaptive downscale (grain map) | full,continuity,settings.motion,settings.analysis,settings.output | T0 | vram | 0.8 | 3.1 | 12.4 |
| 14 | p12_light_level | input/source_analysis/light_level (v3) | compute | light level pass | region evidence (derived maps) | region evidence (derived maps), debander (with hold), light-level users | full,continuity,settings.analysis,settings.output | T1 | vram | 0.1 | 0.5 | 2.1 |
| 15 | p13_source_ring | input/capture (queue + early upload) (v0) | render | none | temporal detail | temporal detail, chroma from luma | full,continuity | T0 | vram | 0.0 | 0.0 | 0.0 |
| 16 | td.blocks | repair/temporal_detail (v2) | compute | reserved (engine: reset 2) | temporal detail | temporal detail | full,continuity,rebaseH,rebaseV,settings.rebase,settings.analysis | T1 | vram | 0.4 | 1.7 | 6.7 |
| 17 | td.memory | repair/temporal_detail (v2) | compute | none | temporal detail | temporal detail, chroma upscale (temporal-detail read path) | full,continuity,rebaseH,rebaseV,settings.rebase,settings.analysis | T1 | vram | 11.8 | 47.2 | 188.7 |
| 18 | td.anchor_pass | repair/temporal_detail (v2) | compute | reserved (engine: reset 2) | temporal detail | temporal detail | full,continuity,rebaseH,rebaseV,settings.rebase,settings.analysis | T2 | transient (arena) | 6.2 | 24.9 | 99.5 |
| 19 | p14_source_identity | input/source_analysis/identity (v1) | cpu | none | none | detail pulse, texture restore, debander (with hold), contrast recovery need, adaptive downscale (grain map), dither need (source quality) | full,continuity,settings.analysis | T0 | system RAM | 0.0 | 0.0 | 0.0 |
| 20 | p7g_camera_guide | motion/estimation/global_model (v1) | cpu | camera guide (camera worker) | camera guidance | camera guidance | full,continuity,settings.motion | T0 | system RAM | 0.2 | 0.6 | 2.6 |
| 21 | coding_evidence | input/source_analysis/regions (v1) | compute | reserved (fused with the region block statistics pass) | temporal detail | temporal detail, debander (temporal-detail read path) | full,continuity,rebaseH,rebaseV,settings.rebase,settings.analysis | T0 | vram | 0.3 | 1.0 | 4.1 |
| 22 | quality_pattern | input/source_analysis/identity/quality_pattern (v1) | cpu | reserved (lane CPU job) | temporal detail | temporal detail, dither need (temporal-detail read path) | full,continuity,settings.analysis | T0 | system RAM | 0.0 | 0.0 | 0.0 |
| 23 | quality_pattern.causal | input/source_analysis/identity/quality_pattern (v1) | cpu | reserved (lane CPU job) | none | none | full,continuity,settings.analysis | T0 | system RAM | 0.0 | 0.0 | 0.0 |
| 24 | refined_field | motion/estimation/refine (v1) | compute | reserved (field refine pass after P3) | temporal detail | temporal detail, chroma upscale (temporal-detail read path), adaptive scaling (temporal-detail read path), sigma-delta (temporal-detail read path) | full,continuity,settings.motion | T0 | vram | 0.3 | 1.0 | 4.1 |
| 25 | td.anchor_refs | repair/temporal_detail (v2) | compute | reserved (engine: reset 2) | temporal detail | temporal detail | full,continuity,rebaseH,rebaseV,settings.rebase,settings.analysis | T1 | vram | 0.0 | 0.0 | 0.0 |
| 26 | td.delta | repair/temporal_detail (v2) | compute | reserved (engine: reset 2) | temporal detail | temporal detail, chroma upscale (temporal-detail read path) | full,continuity,rebaseH,rebaseV,settings.rebase,settings.analysis | T1 | vram | 0.8 | 3.2 | 13.0 |
| 27 | td.maps | repair/temporal_detail (v1) | compute | reserved (engine: reset 2) | temporal detail | temporal detail, debander (temporal-detail read path), chroma upscale (temporal-detail read path), adaptive scaling (temporal-detail read path), sigma-delta (temporal-detail read path), dither need (temporal-detail read path) | full,continuity,rebaseH,rebaseV,settings.rebase,settings.analysis,settings.output | T1 | vram | 0.2 | 0.8 | 3.1 |
| 28 | td.trail_summary | repair/temporal_detail (v2) | cpu | reserved (engine: reset 2) | temporal detail | temporal detail | full,continuity,settings.analysis | T1 | system RAM | 0.0 | 0.0 | 0.0 |

### Consumers

| id | consumer | module | pathway | reads | fallback kind | fallback id |
|---|---|---|---|---|---|---|
| 782 | optical-flow producer | VP-V/modules/motion/estimation/MotionAheadProducer | input 5 | p1_half_luma, p2_vector_pair | own reduced form | product.nvof_producer |
| 775 | source resolution monitor | VP-V/modules/input/source_analysis/SourceResolutionMonitor | input 10 | p5_monitor_tiles, p4_source_stats | own reduced form | product.source_resolution |
| 776 | source identification | VP-V/modules/input/source_analysis/identity | input 20 | p5_monitor_tiles, p4_source_stats | own reduced form | product.source_id |
| 774 | HDR inference | VP-V/modules/input/signal | input 30 | p4_source_stats | own reduced form | product.hdr_inference |
| 780 | entrance stages | VP-V/core/EntranceStages | input 40 | p0_rebase_decision, p6_entrance_ahead | off | product.entrance |
| 264 | entrance segment | VP-V/modules/motion/estimation/VectorDemand | input 45 | p2_vector_pair, p3_analysis_field | own reduced form | product.vector_demand |
| 783 | region evidence (derived maps) | VP-V/modules/input/source_analysis/regions | input 50 | grain.split, region_block_stats, p12_light_level | own reduced form | product.region_evidence |
| 781 | matcher / motion warp | VP-V/modules/motion/consumers/DejudderWarp | motion 20 | p1_half_luma, p2_vector_pair, p4_source_stats | own reduced form | product.motion_warp |
| 576 | camera guidance | VP-V/modules/motion/estimation/global_model/link | motion 24 | p7g_camera_guide | own reduced form | product.camera |
| 521 | camera measurement | VP-V/modules/motion/estimation/global_model | motion 25 | p7_camera_fit@lag1 | own reduced form | product.camera |
| 525 | camera object split | VP-V/modules/motion/estimation/global_model | motion 27 | p9_camera_smoothed | own reduced form | product.camera |
| 265 | source monitor | VP-V/modules/motion/estimation/VectorDemand | motion 30 | p2_vector_pair | own reduced form | product.vector_demand |
| 266 | intent | VP-V/modules/motion/estimation/VectorDemand | motion 35 | p2_vector_pair | own reduced form | product.vector_demand |
| 777 | interpolator / hiccup repair | VP-V/modules/motion/consumers | motion 40 | p3_analysis_field, grain.split | own reduced form | product.interpolator |
| 257 | interpolator | VP-V/modules/motion/estimation/VectorDemand | motion 41 | p2_vector_pair | own reduced form | product.vector_demand |
| 520 | camera occlusion filler | VP-V/modules/motion/estimation/global_model | motion 45 | p8_camera_labels | own reduced form | product.camera |
| 256 | dejudder extras | VP-V/modules/motion/estimation/VectorDemand | motion 50 | p2_vector_pair | own reduced form | product.vector_demand |
| 522 | camera de-judder warp | VP-V/modules/motion/estimation/global_model | motion 55 | p9_camera_smoothed | own reduced form | product.camera |
| 258 | hiccup repair | VP-V/modules/motion/estimation/VectorDemand | motion 66 | p2_vector_pair | own reduced form | product.vector_demand |
| 523 | camera hiccup repair | VP-V/modules/motion/estimation/global_model | motion 67 | p9_camera_smoothed | own reduced form | product.camera |
| 259 | occlusion mask | VP-V/modules/motion/estimation/VectorDemand | motion 81 | p2_vector_pair | own reduced form | product.vector_demand |
| 261 | grain map | VP-V/modules/motion/estimation/VectorDemand | repair 5 | p2_vector_pair, p3_analysis_field | own reduced form | product.vector_demand |
| 513 | camera grain map | VP-V/modules/motion/estimation/global_model | repair 6 | p8_camera_labels, p3_analysis_field | own reduced form | product.camera |
| 778 | temporal detail | VP-V/modules/repair/temporal_detail | repair 10 | grain.split, region_block_stats, p13_source_ring, td.blocks, td.memory, td.anchor_pass | lower tier | product.temporal_detail |
| 269 | temporal detail | VP-V/modules/motion/estimation/VectorDemand | repair 11 | p2_vector_pair, p3_analysis_field | own reduced form | product.vector_demand |
| 515 | camera temporal detail | VP-V/modules/motion/estimation/global_model | repair 12 | p7_camera_fit, p8_camera_labels, p9_camera_smoothed | own reduced form | product.camera |
| 792 | temporal detail engine (reserved: precursors) | VP-V/modules/repair/temporal_detail/products | repair 13 | coding_evidence, quality_pattern, refined_field, td.anchor_refs | own reduced form | product.td_engine |
| 793 | temporal detail engine (reserved: outputs) | VP-V/modules/repair/temporal_detail/products | repair 14 | td.delta, td.maps, td.trail_summary | own reduced form | product.td_engine |
| 768 | detail pulse | VP-V/modules/repair/detail_pulse | repair 20 | grain.split, region_block_stats, p4_source_stats, p14_source_identity@lag1 | own reduced form | product.detail_pulse |
| 263 | detail pulse | VP-V/modules/motion/estimation/VectorDemand | repair 21 | p2_vector_pair, p3_analysis_field | own reduced form | product.vector_demand |
| 516 | camera detail pulse | VP-V/modules/motion/estimation/global_model | repair 22 | p7_camera_fit, p8_camera_labels, p9_camera_smoothed | own reduced form | product.camera |
| 769 | texture restore | VP-V/modules/repair/texture_restore | repair 30 | grain.split, region_block_stats, p4_source_stats, p14_source_identity@lag1 | own reduced form | product.texture_restore |
| 267 | texture restore | VP-V/modules/motion/estimation/VectorDemand | repair 31 | p2_vector_pair, p3_analysis_field | own reduced form | product.vector_demand |
| 517 | camera texture restore | VP-V/modules/motion/estimation/global_model | repair 32 | p7_camera_fit, p8_camera_labels, p9_camera_smoothed | own reduced form | product.camera |
| 770 | debander (with hold) | VP-V/modules/repair/deband | repair 40 | grain.split, region_block_stats, p12_light_level, p3_analysis_field, p14_source_identity@lag1 | own reduced form | product.debander |
| 262 | deband hold | VP-V/modules/motion/estimation/VectorDemand | repair 41 | p2_vector_pair, p3_analysis_field | own reduced form | product.vector_demand |
| 519 | camera debander | VP-V/modules/motion/estimation/global_model | repair 42 | p8_camera_labels | own reduced form | product.camera |
| 791 | debander (temporal-detail read path) | VP-V/modules/repair/deband/VpDebander | repair 43 | td.maps, coding_evidence | own reduced form | product.debander |
| 784 | light-level users | VP-V/modules/repair | repair 50 | p12_light_level, region_block_stats | own reduced form | product.light_level |
| 786 | contrast recovery need | VP-V/modules/repair | repair 55 | p14_source_identity@lag1 | own reduced form | product.contrast_recovery |
| 268 | residual stabilise | VP-V/modules/motion/estimation/VectorDemand | repair 61 | p2_vector_pair, p3_analysis_field | own reduced form | product.vector_demand |
| 514 | camera residual stabilise | VP-V/modules/motion/estimation/global_model | repair 62 | p8_camera_labels | own reduced form | product.camera |
| 524 | camera roll-off | VP-V/modules/motion/estimation/global_model | repair 70 | p9_camera_smoothed | own reduced form | product.camera |
| 779 | chroma from luma | VP-V/modules/scaling/chroma_upscale | scaling 10 | grain.split, grain.split.chroma, p13_source_ring | lower tier | product.chroma_cfl |
| 789 | chroma upscale (temporal-detail read path) | VP-V/modules/scaling/chroma_upscale/basic/BasicChroma | scaling 11 | td.maps, td.delta, td.memory, refined_field | own reduced form | product.chroma_cfl |
| 771 | adaptive downscale (grain map) | VP-V/modules/scaling/adaptive_downscale | scaling 20 | region_block_stats, grain.split, p4_source_stats, p14_source_identity@lag1 | own reduced form | product.adaptive_downscale |
| 787 | adaptive scaling (temporal-detail read path) | VP-V/modules/scaling/output/AdaptiveDownscaleStage | scaling 21 | td.maps, refined_field | own reduced form | product.adaptive_downscale |
| 773 | LED look-ahead | VP-V/modules/drive/led_measure | drive 10 | p4_source_stats | own reduced form | product.led_lookahead |
| 772 | sigma-delta (analysis field mapped) | VP-V/modules/output/dither | output 10 | p3_analysis_field | own reduced form | product.sigma_delta |
| 260 | output dither | VP-V/modules/motion/estimation/VectorDemand | output 11 | p2_vector_pair, p3_analysis_field | own reduced form | product.vector_demand |
| 518 | camera dither vectors | VP-V/modules/motion/estimation/global_model | output 12 | p8_camera_labels | own reduced form | product.camera |
| 790 | sigma-delta (temporal-detail read path) | VP-V/modules/output/dither/SigmaDeltaVectors | output 13 | td.maps, refined_field | own reduced form | product.sigma_delta |
| 785 | dither need (source quality) | VP-V/modules/output/dither | output 15 | p14_source_identity@lag1 | own reduced form | product.dither_need |
| 788 | dither need (temporal-detail read path) | VP-V/modules/output/dither/DitherNeed | output 16 | td.maps, quality_pattern | own reduced form | product.dither_need |
| 512 | camera diagnostics | VP-V/modules/motion/estimation/global_model | output 90 | p7_camera_fit, p8_camera_labels | own reduced form | product.camera |

### VRAM per tier (all consumers active, laneLead 3, render-ahead 0, optical-flow engine present)

Ring buffers + live pools of every node, by tier (decimal MB).

| frame | T0 ring | T0 live | T1 ring | T1 live | T2 ring | T2 live | total |
|---|---|---|---|---|---|---|---|
| 1920x1080 | 257.9 | 326.2 | 304.0 | 130.7 | 31.1 | 6.2 | 1056.2 |
| 3840x2160 | 1031.1 | 1304.3 | 1140.0 | 484.7 | 124.4 | 24.9 | 4109.4 |
| 7680x4320 | 4124.0 | 5216.8 | 4484.1 | 1900.7 | 497.7 | 99.5 | 16322.7 |

<!-- END GENERATED product-graph -->

## Frame histories

Every GPU frame history or reference store, from doc 51 (`D:\codex\vp-handover\51-skeleton-adoption-audit.md`). Decimal MB. All figures are declared/estimated, not measured. The 4K column is from doc 51; 1080p is 4K x 0.25 and 8K is 4K x 4 unless doc 51 gives the number. Output-size buffers are given at output = source size; the owner's FL35 output (2560x1600) is about half of 4K.

| Store | Layout | 1080p | 4K | 8K | Note |
|---|---|---|---|---|---|
| TemporalDetail slot store | 5 slots x 83 MB (10 B/px); whole module about 1 GB (split 265, slots 415, acc 133, delta/prevg 133, crop/out 33, blocks 25) | ~21 per slot, ~250 module | 83 per slot, ~1003 module | ~330 per slot, 4270 module (doc 51) | Doc 20 designed about 16 MB per reference |
| grain_split ring | 4 + 3 live slots at 32 B/px (rgba32f x2) = 265 per slot | ~66 per slot, ~465 ring | 265 per slot, 1860 ring | ~1060 per slot, ~7400 ring | Declared, not allocated in production. Compact target about 37 per slot (~9 / ~148 at 1080p / 8K) |
| Skeleton LaneSourceRing (P13) | keep-alive views of the unpacked planes of queued frames | 0 extra | 0 extra | 0 extra | No copy; owned by queue/upload |
| EarlyUpload ring | 11 slots x 22.1 (v210) | ~61 | 243 | 973 | Raw slot release after unpack to be verified (SA10) |
| NVOF / vector-engine pairs | NVOF producer GPU 1.4; CPU half u8 2.1 (system RAM); store of 16 pairs about 10 | ~0.4 / 0.5 / 2.5 | 1.4 / 2.1 / ~10 | 5.6 / 8.4 / ~40 | The engine size may be fixed by the engine, so the scaled columns are an upper bound |
| Camera model inputs | guide/keep textures about 2.7; CPU blocks 2.6 x 4 | ~0.7 | ~2.7 | ~10.8 | LabelPass 1.6 is test only |
| Render-ahead / RenderedWarp cache | cache + warped + displacement at output | ~83 | 332 | ~1330 | Render-ahead shares this cache; swapchain 100-200 is separate |
| Bill's TemporalRenderer history | `std::deque` of up to 8 stored packets; each holds `prepared[2]` and `processed[2]` textures, rgba16f (float, 4 components, 16 bit) via `pl_find_fmt` | not measured here | not measured here | not measured here | READ ONLY (src/VideoProcessor-Lib/vprenderer/TemporalRenderer.h): depth cap 8 and the format are read from the code; how many are held at run time and the byte total are not measured here. One rgba16f texture is about 66 MB at 4K, declared |
| Entrance B3 ring (re-base) | 3 x 25 MB; with temps about 150 | ~19 ring | 75 ring (~150 with temps) | ~300 ring | Becomes P6 slots |
| AnalysisLumaSource pool | 12 slots, system RAM, not VRAM | ~50 | 199 | 797 | Retire for GPU-resident formats; doc 21 forbids full-resolution per-frame planes in system RAM |

### Duplication

- **TD future slot vs queue/P13.** TemporalDetail copies the future reference into its own slot, but the same unpacked plane is already held by the queue. P13 exposes it as a view with no copy.
- **TD prevg vs P10 history 1.** TD keeps its own previous-frame grain (`prevg`, part of its 133 MB delta/prevg). P10 already holds the previous frame's grain as history 1.
- **TD and CfL own splits vs P10.** TD has its own fallback split (265 MB at 4K) and CfL makes three own splits (about 400 MB at 4K). Both recompute what the grain_split product stores. The band split is implemented seven times in total (doc 51 section 0).
- **Entrance ring vs P6.** The entrance B3 ring (3 x 25 MB) holds the same output P6 (entrance ahead) declares as a product.

### Proposals

a. **TD reads references from P10 clean planes and P13 instead of its own copies.** The slot store keeps only what no shared product holds (cleaned structure plus half-size base, about 4.5 B/px, at most 37 MB per slot at 4K, so at most 187 MB for 5 slots). The future reference is a P13 view; `prevg` is P10 history 1.

b. **TD admission work becomes lane products precomputed in the gaps.** Because the references are known one frame ahead, `td.analysis` (per-block E1/E2, focus, admission decision, under 1 MB) and `td.admission` (the cleaner on admission frames, transient arena) run as lane products in the gaps of the previous frame, tier T1 and T2 respectively. Estimated 1-2 ms on the laptop for td.analysis, a spike of about 3-6 ms for the cleaner (estimates, not measured). On the laptop the gap is only about 4 ms per source frame at today's load, so these moves pay only after the removals in (a) and S1 to S3.

c. **VRAM saved per tier** (doc 51 section 5; 4K is from doc 51, 1080p is x 0.25, 8K is x 4, laptop = 3090; declared/estimated, not measured):

| Change | 1080p | 4K | 8K |
|---|---|---|---|
| S1: one compact grain_split node (P10, about 37 MB per slot x 3) instead of the 32 B/px node, TD's own split and CfL's own splits | ~0.2 GB | ~0.8 GB (from ~0.93-1.86 GB to ~0.13 GB) | ~3.2 GB |
| S2: TD on the store (compact slots, prevg from P10, accumulators in the arena, future reference from P13) | ~0.15 GB | ~0.6 GB (1003 to about 400 MB) | ~2.4 GB |
| S3: CfL temporaries into the arena, splits from P10/P10c | ~0.11 GB | ~0.45 GB (624 to about 150 MB, arena shared) | ~1.8 GB |
| S10: source ring P13, EarlyUpload raw-slot release, AnalysisLumaSource pool retired for GPU formats | ~60 MB VRAM (verify) + ~50 MB system RAM | up to ~240 MB VRAM (verify) + ~200 MB system RAM | ~960 MB VRAM (verify) + ~800 MB system RAM |

### Recommendation

1. Do (a), then (b), after SA2 (compact P10) lands. SA6 carries both.
2. Keep TemporalDetail tier-off on the 4 GB laptop (RTX 3050 Ti) until S1 and S2 are in. VRAM, not only milliseconds, is the binding constraint there.
3. No TD, CfL, TR or deband wiring package merges without its SA package.
4. This document makes declarations only. No code is moved by it.

## Shared units

One dependency unit per shared computation. A unit is a CPU reference (the law, bit-exact test oracle), the GLSL that implements it and a version constant. The layout below is PROPOSED, consistent with the module tree: in the producing submodule `src/VP-V/modules/<unit>/<submodule>/`:

- `<Name>Ref.h`: the CPU reference;
- `<Name>Glsl.h`: the GLSL;
- `constexpr uint32_t k<Name>UnitVersion`: bumped on any change of the law, a pass or a layout, together with the expected number in `ProductCatalog.h`. A reader built against another number engages a visible fallback.

| Unit (proposed name) | Computes | Proposed owning submodule | Readers that should call it (not recompute) |
|---|---|---|---|
| Half luma (`HalfLuma`) | Half and quarter luma, R16 GPU, plus an engine-size u8 view for NVOF | `src/VP-V/modules/input/source_analysis/lane/` (`HalfLumaRef.h`, `HalfLumaGlsl.h`, `kHalfLumaUnitVersion`) | NVOF producer (`HalfLuma8`), matcher downsample and inner pyramids, interpolator `LOW_HALF`, AdaptiveDownscale `lowres`, AnalysisLumaSource decode, TR/DP/debander half passes, P4 |
| Band split (`BandSplit`) | Gaussian bases and the grain-free / grain bands (P10, P10c chroma extension) | `src/VP-V/modules/repair/grain_split/` (`BandSplitRef.h`, `BandSplitGlsl.h`, `kBandSplitUnitVersion`) | TemporalDetail fallback split, CfL x3, DetailPulse, TextureRestore, VpDebander, interpolator grain bands, AdaptiveDownscale |
| Grain estimate (`GrainEstimate`) | Noise level N and Wiener factor f, spatial; GrainError stays in the temporal motion product | `src/VP-V/modules/repair/grain_split/` (`GrainEstimateRef.h`, `GrainEstimateGlsl.h`, `kGrainEstimateUnitVersion`) | VpFrameAnalysis 8x8 RMS, detector flat histogram, TextureRestore N + Wiener, AdaptiveDownscale Laplacian, debander veto share, TD |
| Block stats (`BlockStats`) | Per-block mean, variance, gradient, tensor, Laplacian, band energies E1/E2/E3 (P11) | `src/VP-V/modules/input/source_analysis/regions/` (`BlockStatsRef.h`, `BlockStatsGlsl.h`, `kBlockStatsUnitVersion`) | VpFrameAnalysis 8x8, detector 8x8, DetailPulse 32-px tiles, TextureRestore 16-px tiles, TD MEAS, debander tensor, AdaptiveDownscale Sobel/partials |
| Cut / fade / flash (`SceneEvent`) | One event decision, written to P4 and the Indicator (FULL = cut, MINOR = cut possible) | `src/VP-V/modules/input/source_analysis/lane/` (`SceneEventRef.h`, `SceneEventGlsl.h` where a GPU form is needed, `kSceneEventUnitVersion`) | LED StatsBuilder, HdrInference, matcher M3/M4, SRM FrameCutJudge, DetailPulse, TextureRestore, AdaptiveDownscale, sigma-delta gate. Bill's SceneDetector stays (Original pathway) |
| Histogram / APL (`SourceHistogram`) | 1024-bin histogram, APL, p1/p50/p99, picture-only peak | `src/VP-V/modules/input/source_analysis/lane/` (`SourceHistogramRef.h`, `SourceHistogramGlsl.h`, `kSourceHistogramUnitVersion`) | LED look-ahead, HdrInference grid, matcher M3 16-bin, detector histogram. The libplacebo peak and the LED output-domain hook stay where they are |
| Global motion (`GlobalMotion`) | One global-motion estimate per pair (P2 global; P2m on non-NVOF cards) | `src/VP-V/modules/motion/estimation/` (`GlobalMotionRef.h`, `GlobalMotionGlsl.h`, `kGlobalMotionUnitVersion`) | FlowGlobal (worker), D1 (GPU), engineGlobal, SRM pan, intent |

## Known duplicates

Doc 51 section 2. This is the initial report of `tools/check_shared_units.py` (non-blocking). Counts and the kept authority are declared, not measured.

| Data | Computed by | Count | Keep | Others become |
|---|---|---|---|---|
| Half/quarter luma | Producer `HalfLuma8` (CPU); matcher `Downsample` x2 at 4K and inner pyramids; interpolator `LOW_HALF`; AdaptiveDownscale `lowres`; AnalysisLumaSource decode; TR/DP/debander half passes | 6-8 | P1 (GPU R16 half and quarter, plus an engine-size u8 for NVOF) | Readers |
| Band split / Gaussian bases | grain_split; TD fallback; CfL x3; DP sigma1/sigma2.5; TR sigma1/sigma2.5/sigma6; debander sigma6; interpolator bands | 7 implementations | P10 grain_split (+ P10c chroma extension) | Accessor reads; consumers derive extra sigma on the fly at half resolution |
| Grain / noise level | VpFrameAnalysis 8x8 RMS; detector flatHistogram; grain_split N; TR N + Wiener; matcher GrainError; AdaptiveDownscale Laplacian; debander veto share | 7 | P10 N/f (spatial) + P3 GrainError (temporal, motion compensated) | Readers |
| Block statistics (mean/var/gradient/tensor/energy) | VpFrameAnalysis 8x8; detector 8x8; region_block_stats; DP 32-px tiles; TR 16-px tiles; TD MEAS; debander tensor; AdaptiveDownscale Sobel/partials | 8 | P11 region_block_stats (add E1/E2/E3 per block) | Readers |
| Histogram / APL / peak | VpFrameAnalysis 1024-bin; LED GPU 256-bin + CPU look-ahead; HdrInference grid; matcher M3 16-bin; detector histogram; libplacebo peak | 6 | P4 (source); LED output hook (output domain); libplacebo (fork) | Readers |
| Cut / fade / flash | SceneDetector (Bill); LED StatsBuilder; HdrInference; matcher M3/M4; SRM FrameCutJudge; DP reduce; TR FRAME_BODY; AdaptiveDownscale 20-code; sigma-delta gate; lane P4 | about 10 | P4 event + the Indicator | Readers; Bill's SceneDetector stays (Original) |
| Active-picture rect | Bill evidence (authority); Bill subtitle bars; matcher PictureArea; VpFrameAnalysis (empty Rect); LED picture-only | 5 | Bill's rect, carried in P4 | Readers (TR and debander currently ignore it: a bug) |
| Global motion | FlowGlobal (worker); D1 GPU; engineGlobal | 3 | P2 global (P2m on non-NVOF) | Readers (doc 27 C4) |
| Block vectors | Matcher field; sigma-delta own search | 2 | P3 | Sigma-delta reads P3 mapped |
| Previous-frame grain | TD `prevg`; grain_split ring | 2 | P10 history 1 | TD reads it |
| Future/past source frames | Queue planes; EarlyUpload slots; AnalysisLumaSource pool; TD future slot copy; entrance ring | 3-5 copies | P13 source ring | Views |
| Motion engine instance | Active MotionWarp + idle hiccup MotionWarp + interpolator | 2 | One | Shared |
| Product descriptors | `VpSkeleton::ProductDecl`; `GrainSplit::kProductDecl`; TD `ProductDecl` copy; CfL `ProductInfo` | 4 types | Core `ProductDecl` | Deleted |
