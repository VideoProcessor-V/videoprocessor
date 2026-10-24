# ADR-0018: General, not fixed: derive rates and sizes from measurement

- Status: Accepted
- Date: 2026-10-16 (owner decision; recorded 2026-10-24)
- Slots: all slots

## Context

The software must survive new formats, displays and capture hardware for 5+ years.

## Decision

Never hard-code refresh/frame rates, cadences or resolutions; derive from measured rates; look-aheads in time, not frames.
