# ADR-0011: A placeholder slot for a VP-owned chroma upscale

- Status: Accepted
- Date: 2026-10-22 (owner decision; recorded 2026-10-24)
- Slots: scaling.chroma_upscale

## Context

libplacebo's chroma upscaling is a possible picture-compromise point that VP does not control.

## Decision

Reserve scaling.chroma_upscale right after the input stages (after re-base/4K route/refine). When active, libplacebo receives 4:4:4 and must not re-upsample. Separable-over-polar candidates (CfL-style luma-guided, separable Lanczos).

## Consequences / open

Verify libplacebo's 4:2:2/4:2:0 handling in our path before building anything.
