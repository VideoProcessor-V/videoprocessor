# ADR-0012: Output scaling: hybrid SSIM (w 0.2) + optional D; Lanczos fallback; polar retired

- Status: Accepted
- Date: 2026-10-19 (owner decision; recorded 2026-10-24)
- Slots: scaling.output

## Context

The hybrid scaler is the best measured output downscaler; polar adds cost without a measured gain; the owner prefers separable filters.

## Decision

The output hybrid SSIM downscaler stays hybrid (adaptive_w_base 0.2) inside libplacebo with the optional D refine add-on (own switch). Lanczos replaces hermite as the fallback. Polar retired.

## Consequences / open

D counts only if accuracy gains come without sharpness/detail/grain loss.
