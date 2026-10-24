# ADR-0014: Stage 13 CAM16 appearance adaptation, off by default

- Status: Accepted
- Date: 2026-10-19 (owner decision; recorded 2026-10-24)
- Slots: colour.appearance

## Context

On a dim screen the Stevens and Hunt effects appear; they are corrected by hand today (gamma boost, saturation +5).

## Decision

CAM16 reference -> screen appearance matching at PRE_OUTPUT after the SDR tone shape and before the LED measurement; static per viewing setup; replaces the manual boost/saturation when on; off by default, on after the i1Pro screen-white measurement.

## Consequences / open

One owner per correction with the ambient light sensor (no double correction).
