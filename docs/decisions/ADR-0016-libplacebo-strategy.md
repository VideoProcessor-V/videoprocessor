# ADR-0016: libplacebo stays the colour engine; VP owns more around it

- Status: Accepted
- Date: 2026-10-21 (owner decision; recorded 2026-10-24)
- Slots: colour.tone_gamut, scaling.output

## Context

Replacing libplacebo's tone/gamut mapping, peak detection, CMS, GPU abstraction and libdovi would be very costly and risk fidelity.

## Decision

Not a wholesale replacement. VP owns the front end (capture, re-base, repair, 4K route, nets) and the output end (Stage 13, dynamic drive, black level, calibration LUT, dither) as VP passes/hooks. Fork patches stay minimal; the VP/libplacebo boundary is documented. Reconsider only on triggers (growing fork patches, hook limits, adverse upstream), and then replace one part, contained by a liftable module.
