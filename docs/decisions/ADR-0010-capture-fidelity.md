# ADR-0010: Capture keeps precision: GPU unpack and native RGB by default

- Status: Accepted
- Date: 2026-10-20 (owner decision; recorded 2026-10-24)
- Slots: input.capture

## Context

The CPU formatters reduce v210/RGB to P010 4:2:0.

## Decision

Default capture_path gpu (v210/UYVY -> P210 4:2:2) and native RGB ingest (r210/R12B/R12L 4:4:4). The automatic CPU fallback must be visible (ADR-0001).

## Consequences / open

Low priority: the CPU fallback should keep 4:2:2.
