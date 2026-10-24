# ADR-0007: Processing resolution: always 4K (or the output size if larger); 8K placeholder

- Status: Accepted
- Date: 2026-10-19 (owner decision; recorded 2026-10-24)
- Slots: scaling.route_4k, scaling.rebase

## Context

A single processing size keeps nets and stages on one trained domain; the cost is mainly throughput.

## Decision

Processing resolution = min(source, max(4K, effective output active size after display mode + A-lens + NLS)); recomputed on output-profile change (clean reconfigure, vectors invalidated). No passthrough option, including 1080->1080. When the entrance route is OFF, scale straight to output (low-end path). 8K: an 8K source to a <=4K display is downscaled to 4K first; only an 8K display keeps native 8K with all nets bypassed and motion at 1/4 per axis.

## Consequences / open

The downstream engine must work at arbitrary processing sizes when the route is off.
