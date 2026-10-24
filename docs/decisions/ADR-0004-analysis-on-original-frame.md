# ADR-0004: Source analysis runs on the original delivered frame

- Status: Accepted
- Date: 2026-10-20 (owner decision; recorded 2026-10-24)
- Slots: input.source_analysis, repair.encoding

## Context

Kernel/ratio identification, cut/fade/subtitle detection and A's labels describe the delivered signal; any processing before them would change what they measure.

## Decision

input.source_analysis reads the untouched delivered frame even when re-base/repair are on. Low-confidence labels become 'unknown'. The label set is versioned (label schema v1) and part of net fingerprints.
