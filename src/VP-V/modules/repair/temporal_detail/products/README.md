# Temporal-detail products (reserved): `repair/temporal_detail/products`

The skeleton products of the temporal-detail engine (doc 72) and the contract table that fixes their ids, layouts, fill rules, unit
versions and budget-governed stages. The engine itself is built in reset 2; reset 1 declares the products and wires the consumers' read
paths (adaptive scaling, dither need, sigma-delta, chroma, deband), so reset 2 only fills producers.
Unit: repair/temporal_detail (products); shared accessor: `repair/temporal_detail/shared/TdMapsView.h` (U19).

## Settings

| Key | Section | Type | Range / values | Default | Config placement | Original (Bill) | Fallbacks / status | Tiers |
|---|---|---|---|---|---|---|---|---|
| `temporal_detail_engine` | deferred (profile; SCHEMA lock held by W1-P3, reset 2 adds it) | enum | off, auto | off (`TdProducts::kEngineRequestedToday = false`) | deferred: VP-V > Repair > Temporal detail | n/a (VP-only) | `product.adaptive_downscale`, `product.dither_need`, `product.sigma_delta`, `product.chroma_cfl`, `product.debander`, `product.td_engine`; status lines "<consumer>: classical form (temporal-detail products: <reason>)" | budget-governed: the VRAM / ms governor decides which stage of each governed buffer fits (see Products) |

## Environment and diagnostics

| Name | Purpose | Default | Scope |
|---|---|---|---|
| (none) | | | |

## Products

Registry ids (doc 72 section 1.1; ids 16-18 keep their numbers with new tokens and contracts, TemporalDetail unit version 2):

| id | token | what | fill rule (lane / live / reduced) | unit (version) | tier | floor size per slot at 4K |
|---|---|---|---|---|---|---|
| 16 | `td.blocks` (v1: td.analysis) | per-block TD state: motion + lighting layer | lane (sequential) / TD apply / carry-forward | TemporalDetail (2) | T1 | 1.68 MB |
| 17 | `td.memory` (v1: td.refstore) | the whole budget-governed store (slots, bands, chroma) | lane only | TemporalDetail (2) | T1 | 47.2 MB (stage 0) |
| 18 | `td.anchor_pass` (v1: td.admission) | anchor repair, Transient arena | T2 gap job | TemporalDetail (2) | T2 | 24.9 MB arena peak |
| 20 | `p7g_camera_guide` | camera guidance, now REGISTERED (was claimed as `kSkeletonProductEnd`) | lane / camera worker | camera (1) | T0 | 0.6 MB |
| 21 | `coding_evidence` (P16) | per 8x8 block 8 B + 256 B summary | lane (fused with P11) / TD / abstain | U13 CodingEvidence (1) | T0 | 1.04 MB |
| 22 | `quality_pattern` (P15) | per-frame LANE product, role at lead 2 (NOT P14's held state) | lane CPU / TD / P15c | QualityPattern (1) | T0 | 64 B x history (2 s of the measured source rate) |
| 23 | `quality_pattern.causal` (P15c) | the short-queue substitute of P15 | same | QualityPattern (1) | T0 | 64 B x history (2 s of the measured source rate) |
| 24 | `refined_field` (P3r) | DMVR-style per-block vectors + confidence; per-pixel offsets are NOT stored (derived on the fly) | lane after P3 / TD / unrefined P3 | U15 FieldRefine (1) | T0 | 1.04 MB |
| 25 | `td.anchor_refs` | received luma of the last anchors | TD copy | TemporalDetail (2) | T1 | 0 (governed) |
| 26 | `td.delta` | lane -> render hand-off of the repair delta | lane / TD apply / carry-forward | TemporalDetail (2) | T1 | 3.24 MB (stage 0) |
| 27 | `td.maps` | THE consumer-facing per-block map product (12 fields, U19) | lane / TD apply / classical | U19 TdMapsView (1) | T1 | 0.78 MB |
| 28 | `td.trail_summary` | CPU record per frame for source identity / status | TD job | TemporalDetail (2) | T1 | 256 B x history (0.25 s of the measured source rate) |

The seven per-block map reservations of the first list (trail confidence, residual + validity, real detail, grain level, stability,
lighting view, I-to-I drift) are FIELDS of `td.maps` (`TdMapsView::Field`); the per-pixel BDOF/PROF offsets are dropped (derived by U15's accessor).
Consumer fallback when absent: each consumer's classical form, byte-identical today (table in "Status and fallbacks").

**Budget-governed, no fixed tiers.** The registry declares each large buffer at its FLOOR (stage 0: the repair of the still / dark regions
plus the full-resolution mid band at the true source resolution, the least the budget grants). `TdProductContracts.h` lists the larger stages
(`td.memory` full, `td.anchor_refs` 3 anchors, `td.delta` with the full-resolution atlas); the governor adds them while VRAM and ms headroom
last and sheds them in reverse order, each step visible. Declared totals at 4K: floor 54.95 MB GPU (+ 24.9 MB arena peak; the CPU records scale with the measured source rate: 7.7 KB at 24 p, at most 97.9 KB unmeasured), full
241.6 MB (`TdProducts::BudgetAt`, pinned by a test). Ring multipliers come from the graph.

**Inputs beyond the framework's limit.** The framework holds 4 inputs per product (`kMaxInputs`) and 6 consumed products per consumer. The registry
declares the first four of each intended input list (never silently dropping one) and the contract table records the full lists (`td.blocks` 16,
`coding_evidence` 5, `td.anchor_pass` 5); a test compares prefix and order. **Dependency: W1-SKG** (lead edges in SlotTable, kMaxInputs >= 16,
kMaxConsumes >= 10, ordered-chain lane jobs, history declarations): after it lands, the declarations catch up to the table.

**Repair-stack order (WP9) recorded here: TD -> deband -> contrast recovery** (today's text: deband -> TD -> CR). Deband becomes the fallback layer under
temporal detail: it reads `td.maps` validity and stability (and `coding_evidence` for its blocking need) so a TD-covered block is not debanded twice; with the
products absent (reset 1) it debands everywhere exactly as today. The order table itself belongs to WP9 (`repair/stack/RepairStackOrder.h`).

## Status and fallbacks

| Consumer | Reads | Absent / stale / unit mismatch | Base id (catalog row) | Status text (once requested) |
|---|---|---|---|---|
| adaptive scaling | `td.maps` (real detail, stability, coding noise), `refined_field` | today's behaviour | `product.adaptive_downscale` | "adaptive scaling: classical form (temporal-detail products: ...)" in the stage's status lines |
| dither need | `td.maps` (own grain, stability), `quality_pattern` | today's grain path | `product.dither_need` | appended to the dither-need status |
| sigma-delta | `td.maps` (stability), `refined_field` | today's vectors / own search | `product.sigma_delta` | `NextVectorGuard::TemporalDetailRead()` |
| chroma upscale | `td.maps` (validity, trail confidence), `td.delta`, `td.memory`, `refined_field` | the plain kernel | `product.chroma_cfl` | `Result::td` (ChromaCfl, BasicChroma) |
| debander (fallback layer) | `td.maps` (validity, stability), `coding_evidence` | deband everywhere | `product.debander` | `VpDebander::Module::TemporalDetailRead()` |
| the engine | all of the above | classical forms | `product.td_engine` | |

Today `kEngineRequestedToday` is false: nothing is requested, no line appears, every path is byte-identical. Requested with products absent: the
Decision is engaged and the line says which product is absent / stale / of another unit version; it recovers by itself.

**Keys are per id + cause.** The base id above is the consumer's OWN classical-cause entry (for example a missing chroma split) and the id the catalog row
names. The temporal-detail read never engages that bare id: its entry is `TdProducts::FallbackKey(consumer, causeMask)`, for example
`product.chroma_cfl [td: absent]` or `product.chroma_cfl [td: absent+stale]` (causes: absent, unit version, stale, engine not built; a mixed read names
every cause present). The GUI's one-time notice and `Registry.Engage` key on that string, so the consumer's own cause and each engine cause are separate
entries, none overwrites or releases another, and a new cause is announced once. `TdProducts::SyncRead(registry, consumer, read)` engages the key of the
read and releases the consumer's other cause keys (recovery removes the entry); it never touches the base id. A read that is not engaged carries an empty
id on purpose: the per-frame read of a consumer that is not requested builds no string (the keys are built once, `NeededBy` returns a stored vector).

## Tests

`TdProductsTests` in `TdProductsTests.cpp` (CPU only, `VideoProcessor-Test`): ids, registry validity, contracts and intended-input prefixes, governed stages,
the hand-summed budget, `TdMapsView`, the read rule and the five consumer read paths. `ProductCatalogTests` (catalog agreement, live-fill owners, unit
versions). Command: `vstest.console.exe x64\Release\VideoProcessor-Test.dll /TestCaseFilter:"FullyQualifiedName~TdProductsTests|FullyQualifiedName~ProductCatalogTests"`.
No GPU.
