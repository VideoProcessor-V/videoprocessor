# Decision records

Short ADR-style records of the major VP design decisions, migrated from the planning memory on 2026-10-24. The frozen design is docs/design/VP-pipeline-final.md; slot data is docs/design/slots.json.

| ADR | Decision | Date | Status |
|---|---|---|---|
| [ADR-0001](ADR-0001-no-silent-fallbacks.md) | No silent fallbacks: one 'Fallbacks engaged' heading | 2026-10-20 | Accepted |
| [ADR-0002](ADR-0002-btb-wtw-preserved.md) | Below-black and above-white are preserved in all cases | 2026-10-13 | Accepted |
| [ADR-0003](ADR-0003-fixed-known-delay.md) | Fixed known delay: 200 ms target, 250 ms margin, built for 4 queued frames | 2026-10-20 | Accepted |
| [ADR-0004](ADR-0004-analysis-on-original-frame.md) | Source analysis runs on the original delivered frame | 2026-10-20 | Accepted |
| [ADR-0005](ADR-0005-entrance-order-rebase-first.md) | Entrance order: re-base first (PENDING the round-2 A diagnostic) | 2026-10-24 | Proposed (pending measurement) |
| [ADR-0006](ADR-0006-motion-source-size-per-axis.md) | Motion estimated at source size; vectors delivered per axis; one validity state | 2026-10-20 | Accepted |
| [ADR-0007](ADR-0007-always-4k-processing.md) | Processing resolution: always 4K (or the output size if larger); 8K placeholder | 2026-10-19 | Accepted |
| [ADR-0008](ADR-0008-nets-refine-fingerprints.md) | Nets: one refine net at the end, fingerprints, bypass on mismatch, consolidation | 2026-10-24 | Accepted |
| [ADR-0009](ADR-0009-repair-choices.md) | Repair: net A / VP restore / off; deband after; texture restore a separate add-on | 2026-10-20 | Accepted |
| [ADR-0010](ADR-0010-capture-fidelity.md) | Capture keeps precision: GPU unpack and native RGB by default | 2026-10-20 | Accepted |
| [ADR-0011](ADR-0011-chroma-upscale-slot.md) | A placeholder slot for a VP-owned chroma upscale | 2026-10-22 | Accepted |
| [ADR-0012](ADR-0012-output-scaling-hybrid.md) | Output scaling: hybrid SSIM (w 0.2) + optional D; Lanczos fallback; polar retired | 2026-10-19 | Accepted |
| [ADR-0013](ADR-0013-output-end-order.md) | Output-end order and the two LUTs | 2026-10-20 | Accepted |
| [ADR-0014](ADR-0014-stage13-appearance.md) | Stage 13 CAM16 appearance adaptation, off by default | 2026-10-19 | Accepted |
| [ADR-0015](ADR-0015-skeleton-first.md) | Skeleton first: zero-cost placeholder slots, tiers per unit | 2026-10-20 | Accepted |
| [ADR-0016](ADR-0016-libplacebo-strategy.md) | libplacebo stays the colour engine; VP owns more around it | 2026-10-21 | Accepted |
| [ADR-0017](ADR-0017-modules-and-migration.md) | Module folders, manifests and the migration directive | 2026-10-21 | Accepted |
| [ADR-0018](ADR-0018-general-not-fixed.md) | General, not fixed: derive rates and sizes from measurement | 2026-10-16 | Accepted |
| [ADR-0019](ADR-0019-original-pathway-baseline.md) | An "Original pathway" baseline reproduces Bill's pathway with every VP addition off | 2026-10-24 | Accepted |
| [ADR-0020](ADR-0020-one-total-delay.md) | One user-set total delay, the same at every frame rate; 4 frames recommended | 2026-10-24 | Accepted |
| [ADR-0021](ADR-0021-reorderable-modules.md) | Reorderable modules, with declared contracts and a validator | 2026-10-24 | Accepted |
| [ADR-0022](ADR-0022-vpv-source-folder.md) | All VP-V code under one source folder, src/VP-V | 2026-10-24 | Accepted |
| [ADR-0023](ADR-0023-vpv-config-section.md) | One self-contained VP-V Config section, namespaced keys, managed upstream controls | 2026-10-24 | Accepted |
| [ADR-0024](ADR-0024-b3-once-per-frame-default.md) | The input side runs once per source frame (B3); the default once accepted, a hard gate | 2026-10-24 | Accepted |
